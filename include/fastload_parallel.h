/*
 * pg_fastload: include/fastload_parallel.h
 *
 *   Shared-memory protocol between the leader backend and the parallel CSV
 *   worker processes.  Built on PostgreSQL native DSM / shm_toc / shm_mq.
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Parallel CSV loader extensions
 *       Copyright (c) 2026, yuanyuansong
 */
#ifndef FASTLOAD_PARALLEL_H_INCLUDED
#define FASTLOAD_PARALLEL_H_INCLUDED

#include "postgres.h"

#include "access/tupdesc.h"
#include "storage/spin.h"

/*
 * Magic number for the shm_toc.  Must not collide with other DSM users.
 */
#define FASTLOAD_SHM_MAGIC			UINT64CONST(0xF17A510AD0000001)

/* shm_toc keys */
#define FASTLOAD_KEY_HEADER			0
#define FASTLOAD_KEY_PARAMS			1
#define FASTLOAD_KEY_MQ_BASE		2	/* 2 .. 2 + nworkers - 1 */
/* per-worker reader->worker input queues, placed at a fixed high base so
 * the MQ keys 2 .. 2 + FASTLOAD_MAX_WORKERS - 1 can never collide */
#define FASTLOAD_KEY_INPUT_BASE		(2 + FASTLOAD_MAX_WORKERS)

/* Per-worker queue size.  Streaming, so this only needs to absorb bursts. */
#define FASTLOAD_QUEUE_SIZE			(2 * 1024 * 1024)

/*
 * Per-worker reader->worker input queue size.  The single reader
 * interleaves chunks across the queues, so while it is blocked on one full
 * queue every OTHER queue is temporarily starved; each queue's buffer must
 * be deep enough to bridge those windows.  Measured at w=2, 1 MiB queues
 * (2 x 512 KiB route chunks ~= 6 ms of worker work) left workers idle ~45%
 * of the wall time; 8 MiB (~16 chunks) keeps them fed.  Bounds the DSM at
 * ~640 MiB even at FASTLOAD_MAX_WORKERS (actual allocation scales with
 * the real worker count: ~144 MiB at the 15-worker default).
 */
#define FASTLOAD_INPUT_MQ_SIZE		(8 * 1024 * 1024)

/* Hard cap, also bounds FastloadShared arrays. */
#define FASTLOAD_MAX_WORKERS		64

/* Fixed-size strings stored in the DSM parameter block. */
#define FASTLOAD_NULLSTR_MAX		64

/* Parallel input mode (FastloadParams.pmode). */
#define FL_PMODE_CSV	0		/* CSV: quote-aware boundary machine */
#define FL_PMODE_FIXED	1		/* BINARY / FIXED: STRIDE-split records */

/*
 * One serialized COL field descriptor for FL_PMODE_FIXED, stored in the
 * blob after cols[].  Wire format produced by BinarySerializeFields()
 * (lib/binary.c): a BinaryFieldWire header followed by nulllen payload
 * bytes; workers rebuild the Field array via BinaryDeserializeFields().
 */

/*
 * One column of the target relation, serialized into DSM so that a worker
 * can rebuild a TupleDesc without opening (and locking) the target table.
 */
typedef struct FastloadColumn
{
	Oid			atttypid;
	int32		atttypmod;
	Oid			attcollation;
	bool		attnotnull;
	bool		attisdropped;
	char		attname[NAMEDATALEN];
} FastloadColumn;

/*
 * Everything a worker needs, copied into DSM by the leader before starting
 * workers.  cols[] is a flexible array of natts entries; for FL_PMODE_FIXED
 * a serialized COL field blob follows it (see BinarySerializeFields()).
 */
typedef struct FastloadParams
{
	int			nworkers;
	int			natts;
	int			pmode;		/**< FL_PMODE_CSV or FL_PMODE_FIXED */
	int			nbinfields;	/**< FL_PMODE_FIXED: number of COL fields */
	int64		rec_stride;	/**< FL_PMODE_FIXED: bytes per record */
	int64		skip;
	int64		limit;
	char		delim;
	char		quote;
	char		escape;
	char		nullstr[FASTLOAD_NULLSTR_MAX];
	int			null_len;
	int			encoding;
	Oid			collation;
	char		infile[MAXPGPATH];
	char		parse_badfile[MAXPGPATH];	/**< PARSE_BADFILE, written by the
											 * first worker to reject a record */
	char		dbname[NAMEDATALEN];
	char		username[NAMEDATALEN];
	FastloadColumn cols[FLEXIBLE_ARRAY_MEMBER];
} FastloadParams;

typedef enum FastloadWorkerState
{
	FL_WORKER_INIT = 0,
	FL_WORKER_RUNNING,
	FL_WORKER_DONE,
	FL_WORKER_FAILED
} FastloadWorkerState;

/*
 * Control block at shm_toc key 0.  worker_state[] / worker_error[] let the
 * leader distinguish a clean EOF from a crashed worker.  reader_state /
 * reader_failed / reader_error do the same for the single reader process;
 * reader_scan is the reader's own published total runtime for
 * the <PARALLEL> profile group.
 */
typedef struct FastloadShared
{
	slock_t		mutex;
	int			nworkers;
	int			reader_state;
	bool		reader_failed;
	char		reader_error[256];
	double		reader_scan;
	int64		bytes_read;			/* input bytes the reader has consumed;
								 * polled by the leader for PROGRESS */
	int			worker_state[FASTLOAD_MAX_WORKERS];
	int64		worker_rows[FASTLOAD_MAX_WORKERS];
	char		worker_error[FASTLOAD_MAX_WORKERS][256];

	/*
	 * Parse-error accounting, mirroring ReaderNext() in lib/reader.c.
	 *
	 * PARALLEL mode is only entered with PARSE_ERRORS=0, so the upstream
	 * contract is: the FIRST rejected record stops the load, the record is
	 * appended to PARSE_BADFILE, a WARNING is logged, and the statement
	 * returns normally with the rows accepted so far.  A worker that
	 * catches such an error publishes it here (first writer wins, guarded
	 * by mutex), sets abort_requested and exits; the reader stops routing
	 * and the leader reports the outcome instead of failing the statement.
	 */
	int			parse_errors;		/* number of rejected records (0 or 1) */
	bool		abort_requested;	/* stop the reader / other consumers */
	int64		error_recno;		/* global record number (after SKIP) */
	int			error_field;		/* column being parsed, 0 if unknown */
	char		error_message[512];	/* message of the first error */

	/*
	 * USE_PROFILE=1 only: each worker publishes its own stage times in
	 * seconds here before switching to FL_WORKER_DONE.  Workers are
	 * independent backend processes, so process-local prof_* globals
	 * cannot be read by the leader.  In production builds these stay zero.
	 */
	double		worker_scan[FASTLOAD_MAX_WORKERS];
	double		worker_form[FASTLOAD_MAX_WORKERS];
	double		worker_ipc[FASTLOAD_MAX_WORKERS];
} FastloadShared;

/*
 * Passed in BackgroundWorker.bgw_extra (128 bytes available).  Only the
 * worker index is needed worker-side; the worker count comes from the DSM
 * parameter block.
 */
typedef struct FastloadWorkerId
{
	int			worker_id;
} FastloadWorkerId;

/*
 * Effective worker count when PARALLEL_WORKERS is not given: online CPUs
 * minus one (the leader keeps inserting serially), clamped to
 * [1, FASTLOAD_MAX_WORKERS].  Single implementation for ReaderInit() and
 * the leader orchestration.
 */
extern int	FastloadDefaultWorkers(void);

#endif   /* FASTLOAD_PARALLEL_H_INCLUDED */
