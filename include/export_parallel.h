/*
 * pg_fastload: include/export_parallel.h
 *
 *   Shared-memory protocol for the parallel export path.
 *
 *   The load direction is a single reader fanning records out to N parser
 *   workers, with the leader doing the (serial) heap insert.  The export
 *   direction is the mirror image: N query workers format rows and a single
 *   dedicated writer process does nothing but write(2).  The writer is the
 *   serial stage, so every cost that can be parallelised is pushed into the
 *   workers -- including the text formatting, which is the dominant term.
 *
 *       leader (orchestration, progress, sample .ctl)
 *          |                                    ^
 *          | launch / poll                      | rows_written, bytes_written
 *          v                                    |
 *      query worker 0 --shm_mq 0-->  writer process --> OUTPUT file
 *      query worker 1 --shm_mq 1-->       |
 *      ...                               |
 *      query worker N --shm_mq N-->  ------+
 *
 *   Row order is deliberately NOT preserved: a query worker emits whatever its
 *   block range yields, in whatever order the OS gives it, and the writer
 *   appends in arrival order.  See USER_MANUAL 18.6.
 *
 *   Copyright (c) 2026, yuanyuansong
 */
#ifndef EXPORT_PARALLEL_H_INCLUDED
#define EXPORT_PARALLEL_H_INCLUDED

#include "postgres.h"

#include "access/tupdesc.h"
#include "storage/block.h"
#include "storage/spin.h"
#include "storage/shm_mq.h"

#include "fastload_parallel.h"
#include "writer.h"

/* Must not collide with FASTLOAD_SHM_MAGIC or other DSM users. */
#define FL_EXPORT_SHM_MAGIC			UINT64CONST(0xF17A510AD0000002)

/* shm_toc keys */
#define FL_EXPORT_KEY_HEADER		0
#define FL_EXPORT_KEY_PARAMS		1
#define FL_EXPORT_KEY_MQ_BASE		2	/* 2 .. 2 + nworkers - 1 */

/* Per-queue depth.  The writer only copies bytes to a file, so it never
 * falls behind by much; this just absorbs scheduling noise. */
#define FL_EXPORT_MQ_SIZE			(2 * 1024 * 1024)

/* Hard cap on query workers, also bounds the arrays in the control block. */
#define FL_EXPORT_MAX_WORKERS		FASTLOAD_MAX_WORKERS

/* A worker stops filling a chunk at whichever of these comes first. */
#define FL_EXPORT_CHUNK_ROWS		4096
#define FL_EXPORT_CHUNK_BYTES		(256 * 1024)

/* Request types (FlExportParams.type). */
#define FL_EXPORT_TYPE_CSV			0
#define FL_EXPORT_TYPE_BINARY		1

/*
 * Header of one shm_mq message: a run of already-formatted rows.  The payload
 * follows immediately.  nbytes is redundant with the message length but makes
 * the writer's bounds check explicit.
 */
typedef struct FlExportChunk
{
	int32		nrows;
	int32		nbytes;
} FlExportChunk;

/*
 * Everything the workers and the writer need, copied into DSM by the leader.
 * The three variable-length blobs follow the fixed part, in this order:
 * header text (header_len), serialized snapshot (snap_len), and the writer
 * option list (opts_len), each NUL-terminated where it makes sense.
 */
typedef struct FlExportParams
{
	int			nworkers;
	int			type;			/**< FL_EXPORT_TYPE_CSV / _BINARY */
	int			natts;
	int64		limit;			/**< INT64_MAX when unlimited */
	bool		header;			/**< CSV: the writer emits a header line */
	Oid			relid;
	BlockNumber nblocks;
	int			header_len;
	int			snap_len;
	int			opts_len;
	char		dbname[NAMEDATALEN];
	char		username[NAMEDATALEN];
	char		output[MAXPGPATH];
	char		relname[MAXPGPATH];
	char		data[FLEXIBLE_ARRAY_MEMBER];
} FlExportParams;

/*
 * Control block (shm_toc key 0).  The leader polls the counters for progress;
 * the state fields let it tell "finished" from "died".
 */
typedef struct FlExportShared
{
	slock_t		mutex;
	int			nworkers;
	int			writer_state;		/**< FL_WORKER_* */
	char		writer_error[256];
	int			worker_state[FL_EXPORT_MAX_WORKERS];
	char		worker_error[FL_EXPORT_MAX_WORKERS][256];
	int64		worker_rows[FL_EXPORT_MAX_WORKERS];
	int64		rows_written;
	int64		bytes_written;
	bool		abort_requested;
	pg_atomic_uint64 rows_reserved; /* LIMIT budget, reserved per chunk */
} FlExportShared;

/*
 * Passed in BackgroundWorker.bgw_extra (128 bytes).  Each worker owns a
 * disjoint block range, so no runtime coordination is needed beyond this.
 */
typedef struct FlExportWorkerId
{
	int			worker_id;
	BlockNumber start;			/**< first block of the range */
	BlockNumber nblocks;		/**< number of blocks in the range */
} FlExportWorkerId;

/* What the leader hands to FlExportParallelTry(). */
typedef struct FlExportRequest
{
	const char *relname;		/**< TABLE=, qualified as given */
	const char *output;			/**< OUTPUT=, absolute path */
	int			type;			/**< FL_EXPORT_TYPE_* */
	int			nworkers;		/**< resolved worker count */
	int64		limit;
	bool		header;
	Oid			relid;
	BlockNumber nblocks;
	TupleDesc	desc;			/**< row type of the scan */
	Writer	   *wt;				/**< leader-side writer, already parameterised */
	StringInfo	writer_opts;	/**< "KEY=value\0" list, replayed in workers */
} FlExportRequest;

/*
 * Decide whether this request can be spread over workers; resolves
 * req->nworkers to the effective count when it can.  Pure: no DSM, no
 * progress record, no file -- so the caller can still fall back to serial.
 */
extern bool FlExportParallelSupported(FlExportRequest *req);

/*
 * Attempt the parallel export (the request must have passed the check above).
 * On success *rows / *bytes receive what the writer process reported.
 */
extern bool FlExportParallelTry(FlExportRequest *req, int64 *rows,
								int64 *bytes);

/* Default worker count: online CPUs minus one, clamped to [1, MAX]. */
extern int	FlExportDefaultWorkers(void);

#endif   /* EXPORT_PARALLEL_H_INCLUDED */
