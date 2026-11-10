/*
 * pg_fastload: include/progress.h
 *
 *   Load progress reporting (PROGRESS = ON).
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Progress reporting
 *       Copyright (c) 2026, yuanyuansong
 */
#ifndef PROGRESS_H_INCLUDED
#define PROGRESS_H_INCLUDED

#include "postgres.h"

/*
 * Default report cadence.  A load reports at most once per interval, so a
 * 1 GiB load produces one line per second instead of one per row.
 */
#define FL_PROGRESS_DEFAULT_INTERVAL_MS		1000

/* Accepted PROGRESS_INTERVAL range, in milliseconds. */
#define FL_PROGRESS_MIN_INTERVAL_MS			100
#define FL_PROGRESS_MAX_INTERVAL_MS			3600000

extern void ProgressInit(const char *infile, bool enabled, int interval_ms,
						 int64 limit_rows);
extern void ProgressUpdate(int64 rows, int64 bytes);
extern void ProgressFinish(int64 rows, int64 bytes);

#endif   /* PROGRESS_H_INCLUDED */
