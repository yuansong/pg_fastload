/*
 * pg_fastload: include/progress.h
 *
 *   Load progress tracking.
 *
 *   A pg_fastload() call is a single SQL statement that runs for minutes on a
 *   large input, and the session that issued it is busy until it returns.
 *   Progress therefore has to be *published*, not printed: the loading backend
 *   maintains a small status record for the duration of the load and every
 *   other session can read it through the pgfastload.pg_fastload_progress
 *   view.
 *
 *   The record lives in $PGDATA/pg_fastload/progress/<pid>.status and is
 *   replaced atomically (write to .tmp, rename) on every tick, so a reader
 *   never sees a half-written line.  A plain file is used instead of dynamic
 *   shared memory because GetNamedDSMSegment() only exists from PostgreSQL 17,
 *   while this source tree supports 15 and later; the file also survives a
 *   crashed backend, which a DSM registry cannot ({pid} liveness is checked on
 *   read and stale entries are ignored and cleaned up).
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Progress reporting
 *       Copyright (c) 2026, yuanyuansong
 */
#ifndef PROGRESS_H_INCLUDED
#define PROGRESS_H_INCLUDED

#include "postgres.h"

/* Default publish cadence, in milliseconds. */
#define FL_PROGRESS_DEFAULT_INTERVAL_MS		1000

/* Accepted PROGRESS_INTERVAL range, in milliseconds. */
#define FL_PROGRESS_MIN_INTERVAL_MS			100
#define FL_PROGRESS_MAX_INTERVAL_MS			3600000

/*
 * Status directory under $PGDATA -- a subdirectory of the log directory, so
 * that status records never mix with the per-load log files -- and the file
 * naming scheme: <pid>.status, replaced through <pid>.tmp.
 */
#define FL_PROGRESS_DIRNAME					"pg_fastload/progress"
#define FL_PROGRESS_FILE_SUFFIX				".status"
#define FL_PROGRESS_TMP_SUFFIX				".tmp"

/* Fixed layout of one status record; the reader side is lib/progress.c. */
typedef struct ProgressInfo
{
	const char *direction;		/* "load" or "export"; NULL means load */
	const char *infile;			/* INPUT (load) or OUTPUT (export) */
	const char *datname;		/* database running the operation */
	const char *relname;		/* fully qualified table */
	const char *type;			/* resolved TYPE: CSV / BINARY / TUPLE ... */
	const char *mode;			/* "serial" or "parallel" */
	bool		enabled;		/* PROGRESS = ON */
	bool		notice;			/* PROGRESS_NOTICE = ON */
	int			interval_ms;	/* publish cadence, 0 => default */
	int64		limit_rows;		/* LIMIT, INT64_MAX when unlimited */
	int64		total_rows_hint;	/* estimated total rows (pg_class.reltuples
								 * for TABLE= exports), 0 when unknown */
} ProgressInfo;

extern void ProgressInit(const ProgressInfo *info);
extern void ProgressUpdate(int64 rows, int64 bytes);
extern void ProgressFinish(int64 rows, int64 bytes);
extern void ProgressAbort(void);

/* SQL-callable: one row per running load (see the pg_fastload_progress view). */
extern Datum pg_fastload_progress(PG_FUNCTION_ARGS);

#endif   /* PROGRESS_H_INCLUDED */
