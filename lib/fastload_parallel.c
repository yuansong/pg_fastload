/*
 * pg_fastload: lib/fastload_parallel.c
 *
 *   Leader-side orchestration of the parallel CSV loader.
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Parallel CSV loader extensions
 *       Copyright (c) 2026, yuanyuansong
 *
 *   The leader backend keeps stage (4) WriterInsert() strictly serial, and
 *   delegates stages (1)(2)(3) to 1 reader + N parser backend processes
 *   registered with RegisterDynamicBackgroundWorker(): the reader
 *   walks the input once and routes records into per-worker reader->worker
 *   shm_mq input queues, each worker parses its queue and streams tuples
 *   back over a second PostgreSQL native shm_mq, all inside one DSM
 *   segment located via shm_toc.  Both transports are kernel shm_mq.
 */
#include "pg_fastload.h"

#include <unistd.h>

#include "access/htup_details.h"
#if PG_VERSION_NUM >= 190000
#include "utils/lsyscache.h"	/* get_database_name() */
#else
#include "commands/dbcommands.h"	/* get_database_name() */
#endif
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/proc.h"		/* MyProc */
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "fastload_parallel.h"
#include "binary.h"
#include "logger.h"
#include "pg_profile.h"
#include "progress.h"
#include "reader.h"
#include "writer.h"

/* Declared here so that pg_fastload.c can call it without extra headers. */
extern int64 FastloadParallelRun(Reader *rd, Writer *wt);
extern bool FastloadParallelSupported(Reader *rd, Writer *wt);

#ifdef ENABLE_FASTLOAD_PROFILE
/*
 * Leader-side stage accumulators (seconds).  FastloadParallelRun() owns
 * them; pg_fastload.c prints them in the <PARALLEL> / <WORKER> groups.
 */
double prof_par_setup = 0.0;
double prof_par_wait = 0.0;
double prof_par_copy = 0.0;
double prof_par_write = 0.0;
double prof_par_read = 0.0;
double prof_par_scan = 0.0;
double prof_par_form = 0.0;
double prof_par_ipc = 0.0;

/*
 * Charge the wall time elapsed since *prev to *acc, then rebase *prev.
 * INSTR_TIME_* are monotonic-clock primitives; this deliberately does not
 * touch the global prof_top so the nesting used by pg_fastload.c and by
 * the serial writer's own FASTLOAD_PROFILE() calls stays intact.
 */
static void
fl_tick(double *acc, instr_time *prev)
{
	instr_time	now;
	instr_time	delta;

	INSTR_TIME_SET_CURRENT(now);
	delta = now;
	INSTR_TIME_SUBTRACT(delta, *prev);
	*acc += INSTR_TIME_GET_DOUBLE(delta);
	*prev = now;
}
#define FL_TICK(acc, prev)	fl_tick(&(acc), &(prev))
#else
#define FL_TICK(acc, prev)	((void) 0)
#endif

static void fastload_fill_params(FastloadParams *p, Reader *rd, Writer *wt,
								 int nworkers);
static void fastload_terminate_workers(BackgroundWorkerHandle **handles,
									   int ntot);
static void fastload_cleanup(BackgroundWorkerHandle **handles,
							 shm_mq_handle **mqh,
							 dsm_segment *seg, int nworkers);
static void fastload_wait_reader_ready(FastloadShared *hdr,
									   BackgroundWorkerHandle *rhandle);

/*
 * Decide whether the parallel path is usable.  Anything that needs the
 * original single-process parser transparently falls back to the original
 * loop, so existing behavior is preserved bit-for-bit.
 *
 * Deliberately conservative: the parallel path does not implement the
 * per-row error absorption (PARSE_BADFILE / PARSE_ERRORS), encoding
 * conversion, CHECK_CONSTRAINTS / NOT NULL checking, FILTER,
 * FORCE_NOT_NULL, dropped columns, STDIN input, or MULTI_PROCESS
 * (WRITER=PARALLEL, i.e. the upstream parallel writer).  Those all take the
 * serial path.
 *
 * NOTE on tchecker: CreateTupleChecker() is called unconditionally by every
 * writer (writer_direct.c / writer_buffered.c / writer_parallel.c), hence
 * rd->checker.tchecker is ALWAYS non-NULL.  It is a type-coercion helper,
 * NOT a constraint flag.  For TYPE=CSV the tuple is formed directly with the
 * target TupleDesc by heap_form_tuple(), so CoercionCheckerTuple() resolves
 * to NO_COERCION and returns the very same tuple.  The worker's
 * TupleFormerTuple() output is therefore bit-for-bit identical to the
 * serial path.  Using "tchecker != NULL" as a fallback condition would
 * silently disable parallel loading for every table -- do not add it back.
 */
bool
FastloadParallelSupported(Reader *rd, Writer *wt)
{
	int			i;
	int			nworkers;
	int			nbfields;
	size_t		rec_len;
	Field	   *bfields;

	if (!rd->parallel || (!rd->is_csv && !rd->is_fixed))
		return false;

	/*
	 * PARALLEL_WORKERS=1 (including "auto" resolving to 1 on a 1~2 core
	 * host) must behave exactly like the original pg_bulkload loader: no
	 * DSM segment, no background worker and no shm_mq hop.  ReaderInit()
	 * has already resolved the default, so parallel_workers is the
	 * effective count here; the extra guard only protects callers that
	 * bypass ReaderInit().
	 *
	 * Measured on the 1 GiB TPC-H lineitem baseline: the former behaviour
	 * (one worker doing the full scan twice and streaming every tuple
	 * across a queue) took 16.380 s vs 8.260 s for the serial loader,
	 * i.e. 0.50x.  All of that loss is pure overhead.
	 */
	nworkers = rd->parallel_workers;
	if (nworkers <= 0)
		nworkers = FastloadDefaultWorkers();
	if (nworkers <= 1)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (workers=1, "
				  "using the original single-process parser)\n");
		return false;
	}

	if (rd->parallel_has_filter || rd->parallel_has_fnn)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (FILTER / "
				  "FORCE_NOT_NULL requires the single-process parser)\n");
		return false;
	}

	/*
	 * MULTI_PROCESS=YES (WRITER=PARALLEL) wraps the writer in the upstream
	 * parallel writer: a second backend that inserts over the pgut IPC queue
	 * in its own transaction.  That mechanism already moves stage (4) out of
	 * this backend, so stacking the parallel reader on top buys no extra
	 * throughput and only adds a second process fan-out plus a second
	 * transaction boundary.  Keep that combination on the single-process
	 * reader, which is exactly the upstream behaviour.
	 */
	if (wt->multi_process)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (MULTI_PROCESS / "
				  "WRITER=PARALLEL is not combined with the parallel reader)\n");
		return false;
	}

	/*
	 * BINARY / FIXED: the worker rebuilds the COL field descriptors from a
	 * serialized copy.  NULLIF patterns longer than the DSM slot are the
	 * only descriptor feature the wire format cannot carry (the pattern is
	 * stored inline, fixed size); fall back to the single-process parser.
	 */
	if (rd->is_fixed)
	{
		BinaryParserGetMeta(rd->parser, &nbfields, &rec_len, &bfields);

		if (nbfields <= 0 || rec_len <= 0)
		{
			LoggerLog(INFO, "pg_fastload: PARALLEL disabled (BINARY/FIXED "
					  "requires COL descriptors and a record length)\n");
			return false;
		}
		for (i = 0; i < nbfields; i++)
		{
			if (bfields[i].nulllen > FASTLOAD_NULLSTR_MAX)
			{
				LoggerLog(INFO, "pg_fastload: PARALLEL disabled (NULLIF "
						  "pattern longer than %d bytes requires the "
						  "single-process parser)\n", FASTLOAD_NULLSTR_MAX);
				return false;
			}
		}
	}

	if (rd->checker.check_encoding ||
		rd->checker.has_constraints ||
		rd->checker.has_not_null)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (encoding conversion "
				  "or constraint checking requires the single-process path)\n");
		return false;
	}

	if (rd->max_parse_errors != 0)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (PARSE_ERRORS / "
				  "PARSE_BADFILE requires the single-process path)\n");
		return false;
	}

	if (rd->infile == NULL || rd->infile[0] == '\0' ||
		pg_strcasecmp(rd->infile, "stdin") == 0)
	{
		LoggerLog(INFO, "pg_fastload: PARALLEL disabled (STDIN / missing "
				  "INPUT is not supported by the worker processes)\n");
		return false;
	}

	if (wt->desc == NULL)
		return false;

	for (i = 0; i < wt->desc->natts; i++)
	{
		if (TupleDescAttr(wt->desc, i)->attisdropped)
		{
			LoggerLog(INFO, "pg_fastload: PARALLEL disabled (dropped column)\n");
			return false;
		}
	}

	return true;
}

/*
 * Default worker count = online CPUs - 1, because the writer still occupies
 * one process.  Clamped to [1, FASTLOAD_MAX_WORKERS].
 */
int
FastloadDefaultWorkers(void)
{
	long	n = sysconf(_SC_NPROCESSORS_ONLN);
	int		w;

	if (n < 1)
		n = 1;
	w = (int) n - 1;
	if (w < 1)
		w = 1;
	if (w > FASTLOAD_MAX_WORKERS)
		w = FASTLOAD_MAX_WORKERS;
	return w;
}

static void
fastload_fill_params(FastloadParams *p, Reader *rd, Writer *wt, int nworkers)
{
	TupleDesc	desc = wt->desc;
	int			i;
	char	   *db;
	char	   *usr;
	Size		base_size = offsetof(FastloadParams, cols) +
		sizeof(FastloadColumn) * desc->natts;

	MemSet(p, 0, base_size);

	p->nworkers = nworkers;
	p->natts = desc->natts;
	p->pmode = rd->is_fixed ? FL_PMODE_FIXED : FL_PMODE_CSV;
	p->skip = rd->parallel_skip;
	p->limit = rd->limit;
	p->delim = rd->parallel_delim ? rd->parallel_delim : ',';
	p->quote = rd->parallel_quote ? rd->parallel_quote : '"';
	p->escape = rd->parallel_escape ? rd->parallel_escape : '"';
	p->null_len = rd->parallel_null ? (int) strlen(rd->parallel_null) : 0;
	if (p->null_len >= FASTLOAD_NULLSTR_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("NULL string is too long for parallel mode")));
	if (p->null_len > 0)
		memcpy(p->nullstr, rd->parallel_null, p->null_len);
	p->encoding = rd->checker.encoding;
	/* TupleFormerInit() does not need a collation; FilterInit() is not used
	 * on the parallel path.  PG_GET_COLLATION() needs fcinfo, which is not
	 * available in this translation unit. */
	p->collation = InvalidOid;

	strlcpy(p->infile, rd->infile, MAXPGPATH);
	if (rd->parse_badfile != NULL)
		strlcpy(p->parse_badfile, rd->parse_badfile, MAXPGPATH);

	db = get_database_name(MyDatabaseId);
	strlcpy(p->dbname, db ? db : "", NAMEDATALEN);
	if (db)
		pfree(db);

	usr = GetUserNameFromId(GetUserId(), false);
	strlcpy(p->username, usr ? usr : "", NAMEDATALEN);

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		p->cols[i].atttypid = att->atttypid;
		p->cols[i].atttypmod = att->atttypmod;
		p->cols[i].attcollation = att->attcollation;
		p->cols[i].attnotnull = att->attnotnull;
		p->cols[i].attisdropped = att->attisdropped;
		strlcpy(p->cols[i].attname, NameStr(att->attname), NAMEDATALEN);
	}

	/*
	 * BINARY / FIXED: append the serialized COL field descriptors after
	 * cols[].  FastloadParallelRun() already reserved exactly
	 * BinarySerializedSize() bytes for them.
	 */
	if (rd->is_fixed)
	{
		int			nb;
		size_t		rec_len;
		Field	   *bf;

		BinaryParserGetMeta(rd->parser, &nb, &rec_len, &bf);
		p->nbinfields = nb;
		p->rec_stride = (int64) rec_len;
		BinarySerializeFields(bf, nb, (char *) p + base_size);
	}
}

static void
fastload_terminate_workers(BackgroundWorkerHandle **handles, int ntot)
{
	int			i;

	if (handles == NULL)
		return;
	for (i = 0; i < ntot; i++)
	{
		BgwHandleStatus	st;
		pid_t			pid;

		if (handles[i] == NULL)
			continue;
		st = GetBackgroundWorkerPid(handles[i], &pid);
		if (st == BGWH_STARTED || st == BGWH_NOT_YET_STARTED)
			TerminateBackgroundWorker(handles[i]);
	}
}

static void
fastload_cleanup(BackgroundWorkerHandle **handles, shm_mq_handle **mqh,
				 dsm_segment *seg, int nworkers)
{
	int		i;

	/* handles[0] is the reader, 1..nworkers are the parse workers */
	fastload_terminate_workers(handles, nworkers + 1);
	if (mqh != NULL)
	{
		for (i = 0; i < nworkers; i++)
			if (mqh[i])
				shm_mq_detach(mqh[i]);
	}
	if (seg != NULL)
	{
		dsm_unpin_segment(dsm_segment_handle(seg));
		dsm_detach(seg);
	}
}

/*
 * Wait until the reader announces FL_WORKER_RUNNING.  The process is
 * guaranteed started (WaitForBackgroundWorkerStartup), but it may still
 * fail inside its own main (e.g. the input file cannot be opened); it
 * then marks reader_state=FAILED before exiting.  Bounded to ~2 minutes
 * so a pathological postmaster cannot wedge the load forever.
 */
static void
fastload_wait_reader_ready(FastloadShared *hdr, BackgroundWorkerHandle *rhandle)
{
	BgwHandleStatus	st;
	pid_t			pid;
	int				i;

	st = WaitForBackgroundWorkerStartup(rhandle, &pid);
	if (st == BGWH_STOPPED)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_fastload reader worker exited during startup")));

	for (i = 0; i < 120000; i++)	/* 120000 x 1ms = 2 min */
	{
		int		state;
		char	errbuf[256];

		CHECK_FOR_INTERRUPTS();

		SpinLockAcquire(&hdr->mutex);
		state = hdr->reader_state;
		memcpy(errbuf, hdr->reader_error, sizeof(errbuf));
		SpinLockRelease(&hdr->mutex);

		if (state == FL_WORKER_RUNNING || state == FL_WORKER_DONE)
			return;
		if (state == FL_WORKER_FAILED)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_fastload reader failed during startup: %s",
							errbuf)));

		st = GetBackgroundWorkerPid(rhandle, &pid);
		if (st == BGWH_STOPPED)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_fastload reader worker exited before "
							"announcing readiness")));

		pg_usleep(1000L);
	}

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("pg_fastload reader did not become ready in time")));
}

/*
 * Run stages (1)(2)(3) in one reader + N parse workers and stage (4) here.
 * The reader walks the input file once and routes records into
 * per-worker reader->worker shm_mq input queues; workers parse their queue
 * and stream tuples back over per-worker return shm_mq handles.  Requires
 * max_worker_processes to cover nworkers + 1 background workers.
 * Returns the number of rows inserted (already accumulated in wt->count).
 *
 * IMPORTANT: all orchestration state lives in a dedicated MemoryContext
 * ("pg_fastload parallel"), NOT in wt->context.  wt->context is reset after
 * every WriterInsert() call by the original main loop contract; putting the
 * DSM / queue / handle objects there would leave dangling pointers after the
 * very first row.  WriterInsert() itself is still called with
 * CurrentMemoryContext = wt->context, exactly as in the serial path.
 */
int64
FastloadParallelRun(Reader *rd, Writer *wt)
{
	int					nworkers = rd->parallel_workers;
	MemoryContext		runctx;
	MemoryContext		outerctx;
	dsm_segment		   *seg = NULL;
	shm_toc			   *toc = NULL;
	FastloadShared	   *hdr = NULL;
	FastloadParams	   *params;
	Size				params_size;
	Size				segsize;
	shm_mq			   *mq;
	shm_mq_handle	  **mqh = NULL;
	BackgroundWorkerHandle **handles = NULL;	/* [0]=reader, 1..N=workers */
	BackgroundWorker	worker;
	shm_toc_estimator	e;
	bool			   *done = NULL;
	int					ndone = 0;
	int					i;
#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time			tick;
#endif

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(tick);
	prof_par_setup = prof_par_wait = prof_par_copy = prof_par_write = 0.0;
	prof_par_read = prof_par_scan = prof_par_form = prof_par_ipc = 0.0;
#endif

	if (nworkers <= 0)
		nworkers = FastloadDefaultWorkers();
	if (nworkers > FASTLOAD_MAX_WORKERS)
		nworkers = FASTLOAD_MAX_WORKERS;

	runctx = AllocSetContextCreate(TopMemoryContext,
								   "pg_fastload parallel",
								   ALLOCSET_DEFAULT_SIZES);
	outerctx = MemoryContextSwitchTo(runctx);

	PG_TRY();
	{
		params_size = offsetof(FastloadParams, cols) +
			sizeof(FastloadColumn) * wt->desc->natts;
		if (rd->is_fixed)
		{
			int			nb;
			size_t		rec_len;
			Field	   *bf;

			BinaryParserGetMeta(rd->parser, &nb, &rec_len, &bf);
			params_size += BinarySerializedSize(bf, nb);
		}

		shm_toc_initialize_estimator(&e);
		shm_toc_estimate_chunk(&e, sizeof(FastloadShared));
		shm_toc_estimate_chunk(&e, params_size);
		for (i = 0; i < nworkers; i++)
		{
			shm_toc_estimate_chunk(&e, (Size) FASTLOAD_QUEUE_SIZE);
			shm_toc_estimate_chunk(&e, (Size) FASTLOAD_INPUT_MQ_SIZE);
		}
		shm_toc_estimate_keys(&e, 2 + 2 * nworkers);
		segsize = shm_toc_estimate(&e);

		seg = dsm_create(segsize, 0);
		dsm_pin_segment(seg);
		toc = shm_toc_create(FASTLOAD_SHM_MAGIC,
							 dsm_segment_address(seg), segsize);

		/* key 0: control block */
		hdr = shm_toc_allocate(toc, sizeof(FastloadShared));
		MemSet(hdr, 0, sizeof(FastloadShared));
		SpinLockInit(&hdr->mutex);
		hdr->nworkers = nworkers;
		hdr->reader_state = FL_WORKER_INIT;
		for (i = 0; i < nworkers; i++)
			hdr->worker_state[i] = FL_WORKER_INIT;
		shm_toc_insert(toc, FASTLOAD_KEY_HEADER, hdr);

		/* key 1: serialized parameters + column definitions */
		params = shm_toc_allocate(toc, params_size);
		fastload_fill_params(params, rd, wt, nworkers);
		shm_toc_insert(toc, FASTLOAD_KEY_PARAMS, params);

		/*
		 * keys 2..: one return shm_mq per worker (leader = receiver), then
		 * one reader->worker input shm_mq per worker (reader = sender,
		 * worker = receiver).  Both are created here; each end attaches
		 * its own sender/receiver PGPROC when the process starts.
		 */
		mqh = palloc0(sizeof(shm_mq_handle *) * nworkers);
		handles = palloc0(sizeof(BackgroundWorkerHandle *) * (nworkers + 1));
		done = palloc0(sizeof(bool) * nworkers);

		for (i = 0; i < nworkers; i++)
		{
			mq = shm_mq_create(shm_toc_allocate(toc,
												(Size) FASTLOAD_QUEUE_SIZE),
							   (Size) FASTLOAD_QUEUE_SIZE);
			shm_mq_set_receiver(mq, MyProc);
			shm_toc_insert(toc, FASTLOAD_KEY_MQ_BASE + i, mq);

			mq = shm_mq_create(shm_toc_allocate(toc,
												(Size) FASTLOAD_INPUT_MQ_SIZE),
							   (Size) FASTLOAD_INPUT_MQ_SIZE);
			shm_toc_insert(toc, FASTLOAD_KEY_INPUT_BASE + i, mq);
		}

		/*
		 * Register the reader first and wait for it to announce RUNNING:
		 * the parse workers block on their rings, and a reader that dies
		 * during startup must be detectable (its PG_CATCH marks
		 * reader_state=FAILED only after the process got started).
		 */
		MemSet(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_ConsistentState;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		strlcpy(worker.bgw_library_name, "pg_fastload", MAXPGPATH);
		strlcpy(worker.bgw_function_name, "pg_fastload_reader_main",
				BGW_MAXLEN);
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_fastload CSV reader");
		worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
		worker.bgw_notify_pid = MyProcPid;

		if (!RegisterDynamicBackgroundWorker(&worker, &handles[0]))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("could not register pg_fastload reader worker"),
					 errhint("PARALLEL mode needs max_worker_processes to "
							 "cover workers (%d) + 1 reader.", nworkers)));

		fastload_wait_reader_ready(hdr, handles[0]);

		/* register the parse workers */
		MemSet(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_ConsistentState;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		strlcpy(worker.bgw_library_name, "pg_fastload", MAXPGPATH);
		strlcpy(worker.bgw_function_name, "pg_fastload_worker_main", BGW_MAXLEN);
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_fastload worker");
		worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
		worker.bgw_notify_pid = MyProcPid;

		for (i = 0; i < nworkers; i++)
		{
			FastloadWorkerId	wid;
			shm_mq_result		r;

			wid.worker_id = i;
			memcpy(worker.bgw_extra, &wid, sizeof(wid));

			if (!RegisterDynamicBackgroundWorker(&worker, &handles[i + 1]))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
						 errmsg("could not register pg_fastload background worker"),
						 errhint("PARALLEL mode needs max_worker_processes to "
								 "cover workers (%d) + 1 reader.", nworkers)));

			mq = shm_toc_lookup(toc, FASTLOAD_KEY_MQ_BASE + i, false);
			mqh[i] = shm_mq_attach(mq, seg, handles[i + 1]);

			r = shm_mq_wait_for_attach(mqh[i]);
			if (r != SHM_MQ_SUCCESS)
			{
				int		st;
				char	errbuf[256];

				SpinLockAcquire(&hdr->mutex);
				st = hdr->worker_state[i];
				memcpy(errbuf, hdr->worker_error[i], sizeof(errbuf));
				SpinLockRelease(&hdr->mutex);

				/*
				 * A worker that finishes very quickly (empty or tiny input)
				 * can attach, send every tuple and detach BEFORE the leader
				 * reaches wait_for_attach(); it then reports SHM_MQ_DETACHED.
				 * That is only a timing race, not a failure.  PostgreSQL's
				 * shm_mq keeps already-queued messages readable after the
				 * sender detaches (see shm_mq_detach_internal(): "When the
				 * sender detaches, the receiver can read any messages
				 * remaining in the queue"), so the consume loop will drain
				 * this queue normally.  Only a worker that never reached
				 * FL_WORKER_DONE is a genuine startup failure.
				 */
				if (r == SHM_MQ_DETACHED && st == FL_WORKER_DONE)
				{
					/* fall through; queued tuples are still readable */
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pg_fastload worker %d failed to attach "
									"(state=%d): %s",
									i, st, errbuf)));
			}
		}

		FL_TICK(prof_par_setup, tick);

		LoggerLog(INFO, "pg_fastload: PARALLEL=ON, TYPE=%s, workers=%d "
				  "(default: online CPUs - 1)\n",
				  rd->is_fixed ? "BINARY/FIXED" : "CSV", nworkers);

		while (ndone < nworkers && wt->count < rd->limit)
		{
			bool	any = false;

			CHECK_FOR_INTERRUPTS();

			for (i = 0; i < nworkers && wt->count < rd->limit; i++)
			{
				Size			len;
				void		   *data;
				shm_mq_result	r;

				if (done[i])
					continue;

				r = shm_mq_receive(mqh[i], &len, &data, true);
				FL_TICK(prof_par_wait, tick);
				if (r == SHM_MQ_SUCCESS)
				{
					HeapTupleData	tup;
					char		   *buf;

					any = true;

					/*
					 * WriterInsert() mutates tuple->t_data in place
					 * (infomask, xmin/cmin/xmax, t_self, t_ctid).  shm_mq
					 * buffers are shared and must never be written to, so
					 * make a private copy in wt->context first.
					 */
					buf = MemoryContextAlloc(wt->context, len);
					memcpy(buf, data, len);
					FL_TICK(prof_par_copy, tick);

					/* MemSet also clears t_self / t_tableOid. */
					MemSet(&tup, 0, sizeof(tup));
					tup.t_len = (uint32) len;
					tup.t_data = (HeapTupleHeader) buf;

					/*
					 * Inner PUSH gives the writer's own profile macros a
					 * fresh time base.  Without it the first
					 * FASTLOAD_PROFILE(&prof_writer_toast) in every run
					 * charges the entire worker wait to "TOAST", which is
					 * the pseudo-timer documented in the 1 GiB report.
					 */
					FASTLOAD_PROFILE_PUSH();
					/* WriterInsert() must run in wt->context, as before. */
					MemoryContextSwitchTo(wt->context);
					WriterInsert(wt, &tup);
					MemoryContextSwitchTo(runctx);
					FASTLOAD_PROFILE_POP();

					wt->count += 1;
					MemoryContextReset(wt->context);
					FL_TICK(prof_par_write, tick);

					/*
					 * PROGRESS=ON.  The reader publishes its byte count in the
					 * control block; sampling it every 1024 rows keeps the
					 * spinlock out of the per-row path.
					 */
					if ((wt->count & 1023) == 0)
					{
						int64	read_bytes;

						SpinLockAcquire(&hdr->mutex);
						read_bytes = hdr->bytes_read;
						SpinLockRelease(&hdr->mutex);
						ProgressUpdate(wt->count, read_bytes);
					}
				}
				else if (r == SHM_MQ_DETACHED)
				{
					int		st;
					char	errbuf[256];

					SpinLockAcquire(&hdr->mutex);
					st = hdr->worker_state[i];
					memcpy(errbuf, hdr->worker_error[i], sizeof(errbuf));
					SpinLockRelease(&hdr->mutex);

					done[i] = true;
					ndone++;
					any = true;

					if (st != FL_WORKER_DONE)
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("pg_fastload worker %d terminated "
										"unexpectedly (state=%d): %s",
										i, st, errbuf)));
				}
				/* SHM_MQ_WOULD_BLOCK: keep polling this worker later. */
			}

			if (!any)
			{
				pg_usleep(1000L);	/* 1ms; workers are producing */
				FL_TICK(prof_par_wait, tick);
			}
		}

		/*
		 * Close the progress stream with the exact final numbers.  A load
		 * that is about to be aborted by the parse-error contract below is
		 * not "done", so it gets no closing line.
		 */
		{
			int64	read_bytes;
			int		nerr;

			SpinLockAcquire(&hdr->mutex);
			read_bytes = hdr->bytes_read;
			nerr = hdr->parse_errors;
			SpinLockRelease(&hdr->mutex);

			if (nerr == 0)
				ProgressFinish(wt->count, read_bytes);
		}

		/*
		 * Parse-error contract.
		 *
		 * The serial path stops reading at the first rejected record and
		 * returns normally, keeping the rows that were already written.  A
		 * parallel load cannot reproduce that set: records for the rejected
		 * one are already in flight in other workers' queues, and whether
		 * they land before the abort is observed depends on worker timing.
		 * Measured on a 200k-row file with one bad record at row 100k, the
		 * parallel path committed 156k / 175k / 194k rows for w=2/4/8 while
		 * pg_bulkload committed exactly 100k -- a nondeterministic superset.
		 *
		 * We therefore make the parallel contract strictly atomic: report
		 * the same WARNING lines as the serial path (for log parity), then
		 * fail the statement so the transaction rolls back.  No partial
		 * load, no timing dependence.  PARALLEL=OFF keeps the serial
		 * semantics for callers that want them.
		 */
		{
			int		nerr;
			int64	recno;
			int		field;
			char	errbuf[512];

			SpinLockAcquire(&hdr->mutex);
			nerr = hdr->parse_errors;
			recno = hdr->error_recno;
			field = hdr->error_field;
			memcpy(errbuf, hdr->error_message, sizeof(errbuf));
			SpinLockRelease(&hdr->mutex);

			if (nerr > 0)
			{
				rd->parse_errors += nerr;
				if (field > 0)
					LoggerLog(WARNING, "Parse error Record " INT64_FORMAT
							  ": Input Record " INT64_FORMAT
							  ": Rejected - column %d. %s\n",
							  rd->parse_errors, recno, field, errbuf);
				else
					LoggerLog(WARNING, "Parse error Record " INT64_FORMAT
							  ": Input Record " INT64_FORMAT
							  ": Rejected. %s\n",
							  rd->parse_errors, recno, errbuf);
				LoggerLog(WARNING, "Maximum parse error count exceeded - "
						  INT64_FORMAT " error(s) found in input file\n",
						  rd->parse_errors);

				ereport(ERROR,
						(errcode(ERRCODE_DATA_EXCEPTION),
						 errmsg("parallel load aborted: input record "
								INT64_FORMAT " was rejected", recno),
						 errdetail("%s", errbuf),
						 errhint("PARALLEL requires PARSE_ERRORS = 0 and rolls "
								 "back the whole load on the first rejected "
								 "record.  Use PARALLEL = OFF to keep the rows "
								 "read before the error, like the serial path.")));
			}
		}

		/*
		 * Belt-and-braces: a reader error normally surfaces earlier as a
		 * worker hitting a failed ring mid-record, but check the reader's
		 * own state too so no failure path can pass silently.
		 */
		{
			bool	rfailed;
			char	errbuf[256];

			SpinLockAcquire(&hdr->mutex);
			rfailed = hdr->reader_failed;
			memcpy(errbuf, hdr->reader_error, sizeof(errbuf));
			SpinLockRelease(&hdr->mutex);

			if (rfailed)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("pg_fastload reader failed: %s", errbuf)));
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(outerctx);
		fastload_cleanup(handles, mqh, seg, nworkers);
		MemoryContextDelete(runctx);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * Report how many rows each worker actually produced.  This makes the
	 * chunk-based round-robin sharding directly observable in the log and
	 * lets a functional test assert that no record was lost or duplicated.
	 * Read under the same spinlock the worker used when publishing it.
	 */
	if (ndone == nworkers && hdr != NULL)
	{
		for (i = 0; i < nworkers; i++)
		{
			int64	rows;

			SpinLockAcquire(&hdr->mutex);
			rows = hdr->worker_rows[i];
			SpinLockRelease(&hdr->mutex);
			LoggerLog(INFO, "pg_fastload: worker #%d produced %ld rows\n",
					  i, (long) rows);
		}
	}

#ifdef ENABLE_FASTLOAD_PROFILE
	/* Pull reader/worker-side stage times out of DSM before detaching it. */
	if (hdr != NULL)
	{
		SpinLockAcquire(&hdr->mutex);
		prof_par_read = hdr->reader_scan;
		SpinLockRelease(&hdr->mutex);
		for (i = 0; i < nworkers; i++)
		{
			SpinLockAcquire(&hdr->mutex);
			prof_par_scan += hdr->worker_scan[i];
			prof_par_form += hdr->worker_form[i];
			prof_par_ipc += hdr->worker_ipc[i];
			SpinLockRelease(&hdr->mutex);
		}
	}
#endif

	/* workers still alive (e.g. LIMIT reached) must be stopped */
	fastload_cleanup(handles, mqh, seg, nworkers);
	MemoryContextSwitchTo(outerctx);
	MemoryContextDelete(runctx);

	return wt->count;
}
