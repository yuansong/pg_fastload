/*
 * pg_fastload: lib/progress.c
 *
 *   Load progress tracking and the pgfastload.pg_fastload_progress view.
 *
 *   A pg_fastload() call is a single SQL statement that runs for minutes on a
 *   large input; until it returns, the statement prints nothing and the
 *   session that issued it is unusable.  Progress therefore cannot be
 *   "printed to the loader's client" -- it has to be readable by any other
 *   session.  The loading backend maintains one small status record per load:
 *
 *       $PGDATA/pg_fastload/progress/<pid>.status
 *
 *   rewritten atomically (write .tmp, rename) on every tick, and
 *   pgfastload.pg_fastload_progress turns those records into rows:
 *
 *       SELECT * FROM pgfastload.pg_fastload_progress;
 *
 *   Why a file and not shared memory: PostgreSQL only grew a named-DSM
 *   registry (GetNamedDSMSegment) in version 17, while this source tree still
 *   supports 15 and 16; requesting plain SysV shared memory would in turn
 *   force shared_preload_libraries on every deployment.  A status file needs
 *   neither, is cluster-wide (so a load started in one database is visible
 *   from another), and degrades safely -- a crashed backend leaves a readable
 *   record whose pid is simply no longer alive.
 *
 *   Reported values:
 *     - percent: rows / LIMIT when LIMIT is finite, else bytes read / file
 *       size when the input is a regular file, else omitted (STDIN, pipe).
 *     - rows   : rows inserted by the leader, i.e. the number the statement
 *                will eventually report.
 *     - read   : bytes read by the reader (serial: the parser's counter;
 *                parallel: published by the single reader process).
 *     - rate, ETA: computed by the reader of the record, never stored.
 *
 *   Cost: the hot loop pays one integer compare per row; the clock is sampled
 *   once every FL_PROGRESS_ROW_GATE rows, and the record is rewritten at most
 *   once per PROGRESS_INTERVAL (1 KiB file, no fsync -- it is a status file,
 *   not data).
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Progress reporting
 *       Copyright (c) 2026, yuanyuansong
 */

#include "pg_fastload.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "funcapi.h"
#include "miscadmin.h"

#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "logger.h"
#include "progress.h"

PG_FUNCTION_INFO_V1(pg_fastload_progress);

/*
 * Rows between two clock samples.  Large enough that reading the clock is
 * lost in the noise, small enough that a slow load (1k rows/s) still keeps the
 * cadence within half a second.
 */
#define FL_PROGRESS_ROW_GATE	512

/* A status record is a handful of short lines; anything bigger is garbage. */
#define FL_STATUS_BUFSIZE		4096

typedef struct ProgressState
{
	bool		enabled;		/* PROGRESS = ON for this call */
	bool		notice;			/* PROGRESS_NOTICE = ON */
	bool		write_failed;	/* publishing broken; stop trying */
	int			interval_ms;	/* minimum gap between two records */
	int64		total_bytes;	/* input size, 0 when unknown */
	int64		total_rows;		/* LIMIT, 0 when infinite/unknown */
	int64		next_gate;		/* next row count worth a clock check */
	TimestampTz	started;
	TimestampTz	last_publish;
	char		dir[MAXPGPATH];	/* $PGDATA/pg_fastload */
	char		path[MAXPGPATH];	/* .../progress-<pid>.status */
	char		tmp[MAXPGPATH];	/* .../progress-<pid>.tmp */
	char	   *infile;
	char	   *datname;
	char	   *relname;
	char	   *type;
	char	   *mode;
	char	   *direction;
} ProgressState;

static ProgressState progress;

/* One parsed status record, as read back by pg_fastload_progress(). */
typedef struct ProgressStatus
{
	int			pid;
	double		backend_start;
	double		started;
	double		interval_ms;
	int64		rows;
	int64		bytes_read;
	int64		total_bytes;
	int64		total_rows;
	char	   *datname;
	char	   *relname;
	char	   *infile;
	char	   *type;
	char	   *mode;
	char	   *direction;
	char	   *state;
} ProgressStatus;

/* ------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

/* TimestampTz -> Unix epoch seconds (as a double, so fractions survive). */
static double
fl_to_epoch(TimestampTz ts)
{
	return (double) ts / (double) USECS_PER_SEC +
		(double) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
}

/* Unix epoch seconds -> TimestampTz. */
static TimestampTz
fl_from_epoch(double epoch)
{
	double		usecs;

	usecs = (epoch -
			 (double) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)
		* (double) USECS_PER_SEC;

	if (usecs > (double) PG_INT64_MAX)
		return DT_NOEND;
	if (usecs < (double) PG_INT64_MIN)
		return DT_NOBEGIN;
	return (TimestampTz) usecs;
}

/*
 * A status value occupies one line, and paths really can contain a newline,
 * so flatten control characters before writing.
 */
static char *
fl_safe_string(const char *src)
{
	char	   *dst;
	char	   *p;

	if (src == NULL)
		return pstrdup("");

	dst = pstrdup(src);
	for (p = dst; *p != '\0'; p++)
	{
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	}
	return dst;
}

static bool
fl_pid_alive(int pid)
{
#ifdef WIN32
	/* No portable probe here; a stale record is then simply reported. */
	return true;
#else
	if (pid <= 0)
		return false;
	if (kill(pid, 0) == 0)
		return true;
	return errno == EPERM;
#endif
}

static char *
fl_status_dir(void)
{
	return psprintf("%s/%s", DataDir, FL_PROGRESS_DIRNAME);
}

/* <pid>.status -- a live record. */
static bool
fl_is_status_file(const char *name)
{
	size_t		suffixlen = strlen(FL_PROGRESS_FILE_SUFFIX);
	size_t		len = strlen(name);

	if (len <= suffixlen)
		return false;
	if (*name < '0' || *name > '9')
		return false;
	return strcmp(name + len - suffixlen, FL_PROGRESS_FILE_SUFFIX) == 0;
}

/* <pid>.tmp -- the half-written record of a crashed publisher. */
static bool
fl_is_temp_file(const char *name)
{
	size_t		suffixlen = strlen(FL_PROGRESS_TMP_SUFFIX);
	size_t		len = strlen(name);

	if (len <= suffixlen)
		return false;
	if (*name < '0' || *name > '9')
		return false;
	return strcmp(name + len - suffixlen, FL_PROGRESS_TMP_SUFFIX) == 0;
}

/* The <pid> part of a record file name, or 0. */
static int
fl_name_pid(const char *name)
{
	return atoi(name);
}

/* ------------------------------------------------------------------------
 * Formatting helpers (human output, used by PROGRESS_NOTICE)
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
	char		tmp[32];
	int			len;
	int			i;
	size_t		out = 0;

	if (value < 0)				/* never happens; keep it safe anyway */
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
	int64		total;

	if (seconds < 0 || seconds != seconds)	/* negative or NaN */
		seconds = 0;
	total = (int64) seconds;

	snprintf(buf, buflen, "%02d:%02d:%02d",
			 (int) (total / 3600),
			 (int) ((total / 60) % 60),
			 (int) (total % 60));
}

/* ------------------------------------------------------------------------
 * Publishing: write the status record
 * ------------------------------------------------------------------------ */

/*
 * Replace the record atomically.  Publishing must never fail a load, so a
 * write error only disables further publishing and leaves a line in LOGFILE.
 */
static void
fl_publish(const char *state, int64 rows, int64 bytes)
{
	StringInfoData buf;
	int			fd;
	int			written;

	if (!progress.enabled || progress.write_failed || progress.path[0] == '\0')
		return;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "version=1\n");
	appendStringInfo(&buf, "pid=%d\n", MyProcPid);
	appendStringInfo(&buf, "backend_start=%.6f\n", fl_to_epoch(MyStartTimestamp));
	appendStringInfo(&buf, "started=%.6f\n", fl_to_epoch(progress.started));
	appendStringInfo(&buf, "datname=%s\n", progress.datname);
	appendStringInfo(&buf, "relname=%s\n", progress.relname);
	appendStringInfo(&buf, "infile=%s\n", progress.infile);
	appendStringInfo(&buf, "type=%s\n", progress.type);
	appendStringInfo(&buf, "mode=%s\n", progress.mode);
	appendStringInfo(&buf, "direction=%s\n", progress.direction);
	appendStringInfo(&buf, "state=%s\n", state);
	appendStringInfo(&buf, "interval_ms=%d\n", progress.interval_ms);
	appendStringInfo(&buf, "rows=" INT64_FORMAT "\n", rows);
	appendStringInfo(&buf, "bytes_read=" INT64_FORMAT "\n", bytes);
	appendStringInfo(&buf, "total_bytes=" INT64_FORMAT "\n",
					 progress.total_bytes);
	appendStringInfo(&buf, "total_rows=" INT64_FORMAT "\n",
					 progress.total_rows);

	fd = open(progress.tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		goto failed;

	written = (int) write(fd, buf.data, (size_t) buf.len);
	close(fd);

	if (written != buf.len)
	{
		unlink(progress.tmp);
		goto failed;
	}

	if (rename(progress.tmp, progress.path) != 0)
	{
		unlink(progress.tmp);
		goto failed;
	}

	pfree(buf.data);
	return;

failed:
	pfree(buf.data);
	progress.write_failed = true;
	LoggerLog(INFO,
			  "pg_fastload: could not update the progress record (%s): %s; "
			  "progress publishing disabled for this load\n",
			  progress.path, strerror(errno));
}

/* Remove this load's record (and any leftover temporary file). */
static void
fl_unpublish(void)
{
	if (progress.path[0] != '\0')
		unlink(progress.path);
	if (progress.tmp[0] != '\0')
		unlink(progress.tmp);
}

/*
 * Drop records left behind by backends that no longer exist.  Runs once per
 * load, so the directory cannot accumulate entries across crashes.
 */
static void
fl_purge_stale(void)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(progress.dir);
	if (dir == NULL)
		return;

	while ((de = ReadDir(dir, progress.dir)) != NULL)
	{
		char	   *path;
		int			pid;

		if (!fl_is_status_file(de->d_name) && !fl_is_temp_file(de->d_name))
			continue;

		pid = fl_name_pid(de->d_name);
		if (pid <= 0 || pid == MyProcPid || fl_pid_alive(pid))
			continue;

		path = psprintf("%s/%s", progress.dir, de->d_name);
		unlink(path);
		pfree(path);
	}
	FreeDir(dir);
}

/* ------------------------------------------------------------------------
 * Human-readable output (PROGRESS_NOTICE)
 * ------------------------------------------------------------------------ */

static void
fl_notice(const char *msg)
{
	ereport(NOTICE, (errmsg("%s", msg)));
	LoggerLog(INFO, "%s\n", msg);
}

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
		pct = 100.0 * (double) bytes / (double) progress.total_bytes;

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
	else if (strcmp(progress.direction, "export") == 0)
		appendStringInfo(&msg, "%s written", readbuf);
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

	fl_notice(msg.data);
	pfree(msg.data);
}

/* ------------------------------------------------------------------------
 * Public interface: called by the loader
 * ------------------------------------------------------------------------ */

void
ProgressInit(const ProgressInfo *info)
{
	struct stat st;

	memset(&progress, 0, sizeof(progress));

	if (!info->enabled)
		return;

	progress.enabled = true;
	progress.notice = info->notice;
	progress.interval_ms = (info->interval_ms > 0) ? info->interval_ms
		: FL_PROGRESS_DEFAULT_INTERVAL_MS;
	progress.total_rows = (info->limit_rows != INT64_MAX) ? info->limit_rows : 0;

	progress.infile = fl_safe_string(info->infile);
	progress.datname = fl_safe_string(info->datname);
	progress.relname = fl_safe_string(info->relname);
	progress.type = fl_safe_string(info->type);
	progress.mode = fl_safe_string(info->mode);
	progress.direction = fl_safe_string(info->direction ? info->direction
									   : "load");

	/*
	 * Only a load knows the size of its input up front.  An export grows its
	 * output as it goes, so it reports rows and a rate, never a percentage.
	 */
	if (strcmp(progress.direction, "load") == 0 &&
		info->infile != NULL && stat(info->infile, &st) == 0 &&
		S_ISREG(st.st_mode))
		progress.total_bytes = (int64) st.st_size;

	snprintf(progress.dir, sizeof(progress.dir), "%s", fl_status_dir());
	snprintf(progress.path, sizeof(progress.path),
			 "%s/%d" FL_PROGRESS_FILE_SUFFIX, progress.dir, MyProcPid);
	snprintf(progress.tmp, sizeof(progress.tmp),
			 "%s/%d" FL_PROGRESS_TMP_SUFFIX, progress.dir, MyProcPid);

	if (pg_mkdir_p(progress.dir, 0700) != 0 && errno != EEXIST)
	{
		progress.write_failed = true;
		LoggerLog(INFO,
				  "pg_fastload: could not create %s: %s; "
				  "load progress will not be published\n",
				  progress.dir, strerror(errno));
	}
	else
		fl_purge_stale();

	progress.started = GetCurrentTimestamp();
	progress.last_publish = progress.started;

	fl_publish("running", 0, 0);

	if (progress.notice)
	{
		char		totalbuf[48];
		char		mesg[320];
		bool		exporting = (strcmp(progress.direction, "export") == 0);

		fl_human_bytes(progress.total_bytes, totalbuf, sizeof(totalbuf));
		if (progress.total_bytes > 0)
			snprintf(mesg, sizeof(mesg),
					 "pg_fastload progress: start, %s %s, interval %d ms",
					 exporting ? "output" : "input",
					 totalbuf, progress.interval_ms);
		else
			snprintf(mesg, sizeof(mesg),
					 "pg_fastload progress: start, %s size unknown, "
					 "interval %d ms",
					 exporting ? "output" : "input", progress.interval_ms);

		fl_notice(mesg);
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
	TimestampTz now;

	if (!progress.enabled || progress.write_failed)
		return;

	if (rows < progress.next_gate)
		return;
	progress.next_gate = rows + FL_PROGRESS_ROW_GATE;

	now = GetCurrentTimestamp();
	if (TimestampDifferenceMilliseconds(progress.last_publish, now)
		< progress.interval_ms)
		return;

	progress.last_publish = now;

	fl_publish("running", rows, bytes);

	if (progress.notice)
		fl_progress_emit(rows, bytes, false);
}

/*
 * Normal end of a load: emit the closing line (if requested) and remove the
 * record, because "no record" is what the view reports as "not running".
 */
void
ProgressFinish(int64 rows, int64 bytes)
{
	if (!progress.enabled)
		return;

	if (bytes <= 0 && progress.total_bytes > 0)
		bytes = progress.total_bytes;

	if (progress.notice)
		fl_progress_emit(rows, bytes, true);

	fl_unpublish();

	progress.enabled = false;
}

/*
 * Aborted load (parse-error contract, cancel, error): drop the record without
 * any closing line -- the load did not finish, and the statement is failing.
 */
void
ProgressAbort(void)
{
	if (!progress.enabled)
		return;

	fl_unpublish();

	progress.enabled = false;
}

/* ------------------------------------------------------------------------
 * Reading the records back: pgfastload.pg_fastload_progress
 * ------------------------------------------------------------------------ */

/*
 * Parse one status file.  Missing keys keep their zero/NULL value; the caller
 * only needs pid to be present.
 */
static bool
fl_read_status(const char *path, ProgressStatus *st)
{
	int			fd;
	char		buf[FL_STATUS_BUFSIZE];
	int			len;
	char	   *line;
	char	   *next;

	memset(st, 0, sizeof(ProgressStatus));

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	len = (int) read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return false;
	buf[len] = '\0';

	for (line = buf; line != NULL && *line != '\0'; line = next)
	{
		char	   *eq;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';

		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';

		if (strcmp(line, "pid") == 0)
			st->pid = atoi(eq + 1);
		else if (strcmp(line, "backend_start") == 0)
			st->backend_start = strtod(eq + 1, NULL);
		else if (strcmp(line, "started") == 0)
			st->started = strtod(eq + 1, NULL);
		else if (strcmp(line, "interval_ms") == 0)
			st->interval_ms = strtod(eq + 1, NULL);
		else if (strcmp(line, "rows") == 0)
			st->rows = strtoll(eq + 1, NULL, 10);
		else if (strcmp(line, "bytes_read") == 0)
			st->bytes_read = strtoll(eq + 1, NULL, 10);
		else if (strcmp(line, "total_bytes") == 0)
			st->total_bytes = strtoll(eq + 1, NULL, 10);
		else if (strcmp(line, "total_rows") == 0)
			st->total_rows = strtoll(eq + 1, NULL, 10);
		else if (strcmp(line, "datname") == 0)
			st->datname = pstrdup(eq + 1);
		else if (strcmp(line, "relname") == 0)
			st->relname = pstrdup(eq + 1);
		else if (strcmp(line, "infile") == 0)
			st->infile = pstrdup(eq + 1);
		else if (strcmp(line, "type") == 0)
			st->type = pstrdup(eq + 1);
		else if (strcmp(line, "mode") == 0)
			st->mode = pstrdup(eq + 1);
		else if (strcmp(line, "direction") == 0)
			st->direction = pstrdup(eq + 1);
		else if (strcmp(line, "state") == 0)
			st->state = pstrdup(eq + 1);
	}

	return st->pid > 0;
}

Datum
pg_fastload_progress(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Datum		values[20];
	bool		nulls[20];
	char	   *dirname;
	DIR		   *dir;
	struct dirent *de;
	double		now;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot "
						"accept a set")));

	InitMaterializedSRF(fcinfo, 0);
	tupdesc = rsinfo->setDesc;

	now = fl_to_epoch(GetCurrentTimestamp());
	dirname = fl_status_dir();

	dir = AllocateDir(dirname);
	if (dir != NULL)
	{
		while ((de = ReadDir(dir, dirname)) != NULL)
		{
			ProgressStatus st;
			char	   *path;
			bool		alive;
			double		elapsed;
			double		pct = -1.0;

			if (!fl_is_status_file(de->d_name))
				continue;

			path = psprintf("%s/%s", dirname, de->d_name);
			if (!fl_read_status(path, &st))
			{
				pfree(path);
				continue;
			}

			alive = fl_pid_alive(st.pid);

			/* A load whose backend is gone is removed after being reported. */
			if (!alive)
				unlink(path);

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			values[0] = Int32GetDatum(st.pid);
			values[1] = TimestampTzGetDatum(fl_from_epoch(st.backend_start));
			values[2] = TimestampTzGetDatum(fl_from_epoch(st.started));

			if (st.direction != NULL)
				values[3] = PointerGetDatum(cstring_to_text(st.direction));
			else
				values[3] = PointerGetDatum(cstring_to_text("load"));

			if (st.datname != NULL)
				values[4] = DirectFunctionCall1(namein,
												CStringGetDatum(st.datname));
			else
				nulls[4] = true;

			if (st.relname != NULL)
				values[5] = PointerGetDatum(cstring_to_text(st.relname));
			else
				nulls[5] = true;

			if (st.infile != NULL)
				values[6] = PointerGetDatum(cstring_to_text(st.infile));
			else
				nulls[6] = true;

			if (st.type != NULL)
				values[7] = PointerGetDatum(cstring_to_text(st.type));
			else
				nulls[7] = true;

			if (st.mode != NULL)
				values[8] = PointerGetDatum(cstring_to_text(st.mode));
			else
				nulls[8] = true;

			values[9] = PointerGetDatum(cstring_to_text(alive ? "running" : "stale"));

			elapsed = now - st.started;
			if (elapsed < 0)
				elapsed = 0;
			values[10] = Float8GetDatum(elapsed);

			values[11] = Int64GetDatum(st.rows);
			values[12] = Int64GetDatum(st.bytes_read);
			values[13] = Int64GetDatum(st.total_bytes);
			values[14] = Int64GetDatum(st.total_rows);

			if (st.total_rows > 0)
				pct = 100.0 * (double) st.rows / (double) st.total_rows;
			else if (st.total_bytes > 0)
				pct = 100.0 * (double) st.bytes_read / (double) st.total_bytes;

			if (pct >= 0)
			{
				if (pct > 100.0)
					pct = 100.0;
				values[15] = Float8GetDatum(pct);
			}
			else
				nulls[15] = true;

			if (elapsed > 0)
			{
				values[16] = Float8GetDatum((double) st.bytes_read / elapsed);
				values[17] = Float8GetDatum((double) st.rows / elapsed);
			}
			else
			{
				nulls[16] = true;
				nulls[17] = true;
			}

			if (pct > 0 && pct < 100 && elapsed > 0)
				values[18] = Float8GetDatum(elapsed * (100.0 - pct) / pct);
			else
				nulls[18] = true;

			values[19] = Int32GetDatum((int) st.interval_ms);

			tuplestore_putvalues(rsinfo->setResult, tupdesc, values, nulls);
			pfree(path);
		}
		FreeDir(dir);
	}

	pfree(dirname);

	return (Datum) 0;
}
