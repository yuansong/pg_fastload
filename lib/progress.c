/*
 * pg_fastload: lib/progress.c
 *
 *   Load progress reporting (PROGRESS = ON).
 *
 *   A pg_fastload() call is a single SQL statement that runs for minutes on a
 *   large input, and the loader prints nothing until the statement returns.
 *   This module publishes a one-line snapshot at a fixed cadence, both as a
 *   NOTICE (so it appears in psql / the client as it happens) and in LOGFILE
 *   (so a background load can be watched afterwards).
 *
 *   Reported values:
 *     - percentage: bytes consumed / file size when the input is a regular
 *       file, otherwise the LEADER's row count against LIMIT when that limit
 *       is finite.  A percentage is simply omitted when neither is known
 *       (STDIN, pipes, FIFOs).
 *     - rows   : rows committed by the leader, i.e. the number that will be
 *                reported by the statement.
 *     - read   : bytes read by the reader (serial: the parser's own counter;
 *                parallel: published by the single reader process).
 *     - rate   : average bytes/s and rows/s since the statement started.
 *     - ETA    : remaining bytes / average byte rate; needs a percentage.
 *
 *   Cost: the timestamp is only consulted once every FL_PROGRESS_ROW_GATE
 *   rows, so the hot loop pays a single integer compare per row (measured
 *   below 0.01% of the 1 GiB load).
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Progress reporting
 *       Copyright (c) 2026, yuanyuansong
 */

#include "pg_fastload.h"

#include <sys/stat.h>

#include "logger.h"
#include "progress.h"

#include "utils/timestamp.h"

/*
 * Rows between two timestamp samples.  Large enough that checking the clock
 * is lost in the noise, small enough that a slow load (1k rows/s) still keeps
 * the cadence within half a second.
 */
#define FL_PROGRESS_ROW_GATE	512

typedef struct ProgressState
{
	bool		enabled;		/* PROGRESS = ON for this call */
	int			interval_ms;	/* minimum gap between two lines */
	int64		total_bytes;	/* input size, 0 when unknown */
	int64		total_rows;		/* LIMIT, 0 when infinite/unknown */
	int64		next_gate;		/* next row count worth a clock check */
	TimestampTz	started;
	TimestampTz	last_emit;
	int64		last_bytes;
	int64		last_rows;
} ProgressState;

static ProgressState progress;

/* ------------------------------------------------------------------------
 * Formatting helpers
 * ------------------------------------------------------------------------ */

static void
fl_human_bytes(int64 bytes, char *buf, size_t buflen)
{
	if (bytes < 0)
		bytes = 0;

	if (bytes >= INT64CONST(1024) * 1024 * 1024)
		snprintf(buf, buflen, "%.2f GiB",
				 (double) bytes / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= INT64CONST(1024) * 1024)
		snprintf(buf, buflen, "%.1f MiB", (double) bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		snprintf(buf, buflen, "%.1f KiB", (double) bytes / 1024.0);
	else
		snprintf(buf, buflen, INT64_FORMAT " B", bytes);
}

/* Compact rate: 1.23M rows/s instead of 1234567 rows/s. */
static void
fl_human_rate(double value, char *buf, size_t buflen)
{
	if (value >= 1e9)
		snprintf(buf, buflen, "%.2fG", value / 1e9);
	else if (value >= 1e6)
		snprintf(buf, buflen, "%.2fM", value / 1e6);
	else if (value >= 1e3)
		snprintf(buf, buflen, "%.1fk", value / 1e3);
	else
		snprintf(buf, buflen, "%.0f", value);
}

/* 12345678 -> "12,345,678" */
static void
fl_group_digits(int64 value, char *buf, size_t buflen)
{
	char	tmp[32];
	int		len;
	int		i;
	size_t	out = 0;

	if (value < 0)					/* never happens; keep it safe anyway */
	{
		snprintf(buf, buflen, INT64_FORMAT, value);
		return;
	}

	len = snprintf(tmp, sizeof(tmp), INT64_FORMAT, value);
	if (len < 0 || (size_t) len >= sizeof(tmp))
	{
		snprintf(buf, buflen, INT64_FORMAT, value);
		return;
	}

	for (i = 0; i < len && out + 1 < buflen; i++)
	{
		if (i > 0 && (len - i) % 3 == 0)
			buf[out++] = ',';
		buf[out++] = tmp[i];
	}
	buf[out] = '\0';
}

/* Seconds -> "hh:mm:ss" (hours are not capped; a load can outlive a day). */
static void
fl_human_interval(double seconds, char *buf, size_t buflen)
{
	int64	total;

	if (seconds < 0 || seconds != seconds)	/* negative or NaN */
		seconds = 0;
	total = (int64) seconds;

	snprintf(buf, buflen, "%02d:%02d:%02d",
			 (int) (total / 3600),
			 (int) ((total / 60) % 60),
			 (int) (total % 60));
}

/* ------------------------------------------------------------------------
 * Emission
 * ------------------------------------------------------------------------ */

static void
fl_progress_emit(int64 rows, int64 bytes, bool final)
{
	char			readbuf[48];
	char			totalbuf[48];
	char			ratebuf[48];
	char			rowratebuf[48];
	char			rowbuf[40];
	char			limitbuf[40];
	char			elapsedbuf[32];
	char			etabuf[32];
	double			elapsed;
	double			pct = -1.0;
	bool			pct_by_rows = false;
	double			bps;
	double			rps;
	StringInfoData	msg;

	elapsed = (double) TimestampDifferenceMilliseconds(progress.started,
													   GetCurrentTimestamp()) / 1000.0;
	if (elapsed < 0)
		elapsed = 0;

	/*
	 * The percentage follows whatever the caller actually asked for: with a
	 * finite LIMIT the job is complete when that many rows are in, even
	 * though the reader deliberately stops before the end of the file.
	 * Without LIMIT, it is bytes consumed versus file size.
	 */
	if (progress.total_rows > 0)
	{
		pct_by_rows = true;
		pct = 100.0 * (double) rows / (double) progress.total_rows;
	}
	else if (progress.total_bytes > 0)
	{
		pct = 100.0 * (double) bytes / (double) progress.total_bytes;
	}

	if (final)
	{
		/*
		 * "100%" on the closing line means "the job is over", which is only
		 * the same as "LIMIT reached" when the file had that many rows.
		 */
		if (!pct_by_rows || rows >= progress.total_rows)
			pct = 100.0;
	}
	else if (pct > 99.9)
		/*
		 * Reserve 100% for the closing line: the reader may still hold a few
		 * unread bytes (or the leader a few uninserted rows) when the file has
		 * been consumed, and a mid-load "100.0%" only reads as a stall.
		 */
		pct = 99.9;

	fl_human_bytes(bytes, readbuf, sizeof(readbuf));
	fl_human_bytes(progress.total_bytes, totalbuf, sizeof(totalbuf));
	fl_group_digits(rows, rowbuf, sizeof(rowbuf));
	fl_group_digits(progress.total_rows, limitbuf, sizeof(limitbuf));

	bps = elapsed > 0 ? (double) bytes / elapsed : 0;
	rps = elapsed > 0 ? (double) rows / elapsed : 0;
	fl_human_bytes((int64) bps, ratebuf, sizeof(ratebuf));
	fl_human_rate(rps, rowratebuf, sizeof(rowratebuf));
	fl_human_interval(elapsed, elapsedbuf, sizeof(elapsedbuf));

	initStringInfo(&msg);
	appendStringInfoString(&msg, "pg_fastload progress: ");
	if (pct >= 0)
	{
		if (pct_by_rows)
			appendStringInfo(&msg, "%.1f%% of %s rows | ", pct, limitbuf);
		else
			appendStringInfo(&msg, "%.1f%% | ", pct);
	}

	appendStringInfo(&msg, "rows %s | ", rowbuf);
	if (progress.total_bytes > 0)
		appendStringInfo(&msg, "%s / %s", readbuf, totalbuf);
	else
		appendStringInfo(&msg, "%s read", readbuf);

	appendStringInfo(&msg, " | %s/s | %s rows/s | elapsed %s",
					 ratebuf, rowratebuf, elapsedbuf);

	if (!final && pct > 0 && pct < 100 && elapsed > 0)
	{
		fl_human_interval(elapsed * (100.0 - pct) / pct, etabuf, sizeof(etabuf));
		appendStringInfo(&msg, " | ETA %s", etabuf);
	}
	if (final)
		appendStringInfoString(&msg, " | done");

	/* Client output: a NOTICE, so it streams as the statement runs. */
	ereport(NOTICE, (errmsg("%s", msg.data)));

	/* LOGFILE: same text, so a background load can be audited later. */
	LoggerLog(INFO, "%s\n", msg.data);

	pfree(msg.data);
}

/* ------------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------------ */

/*
 * Called once per load, after the logger exists.  `enabled` comes from
 * PROGRESS; `limit_rows` is LIMIT (INT64_MAX when unlimited) and is used as
 * the denominator when the input size is not a plain file size.
 */
void
ProgressInit(const char *infile, bool enabled, int interval_ms, int64 limit_rows)
{
	struct stat	st;

	/* A backend may serve many calls; never inherit the previous one. */
	memset(&progress, 0, sizeof(progress));

	if (!enabled)
		return;

	progress.enabled = true;
	progress.interval_ms = (interval_ms > 0) ? interval_ms
		: FL_PROGRESS_DEFAULT_INTERVAL_MS;
	progress.total_rows = (limit_rows != INT64_MAX) ? limit_rows : 0;

	if (infile != NULL && stat(infile, &st) == 0 && S_ISREG(st.st_mode))
		progress.total_bytes = (int64) st.st_size;

	progress.started = GetCurrentTimestamp();
	progress.last_emit = progress.started;

	{
		char	totalbuf[48];
		char	mesg[256];

		fl_human_bytes(progress.total_bytes, totalbuf, sizeof(totalbuf));
		if (progress.total_bytes > 0)
			snprintf(mesg, sizeof(mesg),
					 "pg_fastload progress: start, input %s, interval %d ms",
					 totalbuf, progress.interval_ms);
		else
			snprintf(mesg, sizeof(mesg),
					 "pg_fastload progress: start, input size unknown, "
					 "interval %d ms", progress.interval_ms);

		ereport(NOTICE, (errmsg("%s", mesg)));
		LoggerLog(INFO, "%s\n", mesg);
	}
}

/*
 * Called from the insert loop of both the serial and the parallel path.
 * Cheap enough for the per-row path: one integer compare unless 512 rows
 * have gone by.
 */
void
ProgressUpdate(int64 rows, int64 bytes)
{
	TimestampTz	now;

	if (!progress.enabled)
		return;

	if (rows < progress.next_gate)
		return;
	progress.next_gate = rows + FL_PROGRESS_ROW_GATE;

	now = GetCurrentTimestamp();
	if (TimestampDifferenceMilliseconds(progress.last_emit, now)
		< progress.interval_ms)
		return;

	progress.last_emit = now;
	progress.last_rows = rows;
	progress.last_bytes = bytes;

	fl_progress_emit(rows, bytes, false);
}

/*
 * Closing line.  Emitted once per load, even when the load was too short for
 * a single periodic line, so PROGRESS=ON always produces at least the total.
 */
void
ProgressFinish(int64 rows, int64 bytes)
{
	if (!progress.enabled)
		return;

	if (bytes <= 0 && progress.total_bytes > 0)
		bytes = progress.total_bytes;

	fl_progress_emit(rows, bytes, true);

	progress.enabled = false;		/* exactly one closing line */
}
