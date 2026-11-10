/*
 * pg_fastload: lib/export_parallel.c
 *
 *   Parallel export: N query workers + 1 writer process.
 *
 *   Mirrors lib/fastload_parallel.c in the other direction.  Loading reads one
 *   file with one reader and fans records out to parser workers while the
 *   leader does the serial heap insert; exporting scans a table in N workers
 *   that format rows into bytes, and a single dedicated process appends those
 *   bytes to the output file.  Everything that costs CPU -- scanning,
 *   deforming, calling output functions, quoting -- happens where there are
 *   cores; the one unavoidable serial stage (writing the file) is left with
 *   nothing but write(2).
 *
 *   Row order is not preserved.  The workers own disjoint ctid block ranges
 *   and the writer appends in arrival order, which is what the caller was
 *   promised: a complete dump, not a sorted one.
 *
 *   Copyright (c) 2026, yuanyuansong
 */
#include "pg_fastload.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "commands/dbcommands.h"
#include "catalog/pg_class.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "export.h"
#include "export_parallel.h"
#include "fastload_parallel.h"
#include "logger.h"
#include "pg_profile.h"
#include "progress.h"
#include "writer.h"

PGDLLEXPORT void pg_fastload_export_writer_main(Datum main_arg);
PGDLLEXPORT void pg_fastload_export_worker_main(Datum main_arg);

/* How often the leader republishes progress while waiting. */
#define FL_EXPORT_PROGRESS_MS			50

/* Below this many blocks per worker, splitting costs more than it saves. */
#define FL_EXPORT_MIN_BLOCKS_PER_WORKER	16

/*
 * PG19 grew a ScanOptions argument on table_beginscan().  The export scan is a
 * plain sequential heap scan with no scan keys, so 0 is the right value there
 * and the older signature is kept intact for PG15..PG18.
 */
#if PG_VERSION_NUM >= 190000
#define FL_TABLE_BEGINSCAN(rel, snap) \
	table_beginscan((rel), (snap), 0, NULL, 0)
#else
#define FL_TABLE_BEGINSCAN(rel, snap) \
	table_beginscan((rel), (snap), 0, NULL)
#endif

static void fl_export_set_state(FlExportShared *hdr, int type, int id,
								int state, const char *msg);
static void fl_write_all(int fd, const char *buf, size_t len, const char *path);
static bool fl_export_send_chunk(shm_mq_handle *mqh, StringInfo chunk,
								 int nrows);

/* ========================================================================
 * Helpers
 * ========================================================================*/

int
FlExportDefaultWorkers(void)
{
	return FastloadDefaultWorkers();
}

/*
 * Record a state transition and wake the leader, so that "the writer failed"
 * is noticed immediately instead of on the next poll tick.
 */
static void
fl_export_set_state(FlExportShared *hdr, int type, int id, int state,
					const char *msg)
{
	SpinLockAcquire(&hdr->mutex);
	if (type == 0)
		hdr->writer_state = state;
	else
	{
		hdr->worker_state[id] = state;
		if (msg != NULL)
			snprintf(hdr->worker_error[id],
					 sizeof(hdr->worker_error[id]), "%s", msg);
	}
	SpinLockRelease(&hdr->mutex);

	if (MyBgworkerEntry != NULL && MyBgworkerEntry->bgw_notify_pid != 0)
	{
		PGPROC	   *registrant =
			BackendPidGetProc(MyBgworkerEntry->bgw_notify_pid);

		if (registrant != NULL)
			SetLatch(&registrant->procLatch);
	}
}

/*
 * write(2) may return a short count; the data file must not have holes.
 */
static void
fl_write_all(int fd, const char *buf, size_t len, const char *path)
{
	size_t		done = 0;

	while (done < len)
	{
		ssize_t		n = write(fd, buf + done, len - done);

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to \"%s\": %m", path)));
		}
		if (n == 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to \"%s\": wrote 0 bytes", path)));
		done += (size_t) n;
	}
}

/*
 * Send one chunk of already-formatted bytes.  Blocks while the queue is full:
 * that is the backpressure tying worker throughput to the writer's.  Returns
 * false when the writer went away, i.e. nothing more can be written.
 */
static bool
fl_export_send_chunk(shm_mq_handle *mqh, StringInfo chunk, int nrows)
{
	FlExportChunk	h;
	Size			total;
	char		   *buf;
	shm_mq_result	r;

	if (chunk->len == 0 || nrows == 0)
	{
		resetStringInfo(chunk);
		return true;
	}

	h.nrows = nrows;
	h.nbytes = chunk->len;
	total = sizeof(FlExportChunk) + chunk->len;

	/* shm_mq copies the message, so one contiguous palloc is enough. */
	buf = palloc(total);
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), chunk->data, chunk->len);

	r = shm_mq_send(mqh, total, buf, false, false);
	pfree(buf);
	resetStringInfo(chunk);

	return (r == SHM_MQ_SUCCESS);
}

/* ========================================================================
 * Writer process: the only stage that touches the output file
 * ========================================================================*/

PGDLLEXPORT void
pg_fastload_export_writer_main(Datum main_arg)
{
	dsm_segment	   *seg;
	shm_toc		   *toc;
	FlExportShared *hdr;
	FlExportParams *p;
	shm_mq_handle **mqh;
	bool		   *done;
	int				ndone = 0;
	int				fd = -1;
	int				i;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload export writer: unable to map DSM segment")));
	toc = shm_toc_attach(FL_EXPORT_SHM_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload export writer: bad DSM magic")));

	hdr = shm_toc_lookup(toc, FL_EXPORT_KEY_HEADER, false);
	p = shm_toc_lookup(toc, FL_EXPORT_KEY_PARAMS, false);

	mqh = palloc0(sizeof(shm_mq_handle *) * p->nworkers);
	done = palloc0(sizeof(bool) * p->nworkers);

	PG_TRY();
	{
		FlExportChunk  *chunk;
		Size			len;
		void		   *data;
		int64			rows = 0;
		int64			bytes = 0;

		/*
		 * O_EXCL: an export never overwrites.  The file is created here and
		 * nowhere else, so this is the authoritative check even though the
		 * leader tested the path first to fail fast.
		 */
		fd = BasicOpenFilePerm(p->output,
							   O_WRONLY | O_CREAT | O_EXCL | PG_BINARY,
							   S_IRUSR | S_IWUSR);
		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create output file \"%s\": %m",
							p->output)));

		/* The header is written here, so it is always the first line. */
		if (p->header && p->header_len > 0)
		{
			fl_write_all(fd, p->data, p->header_len, p->output);
			bytes += p->header_len;
		}

		for (i = 0; i < p->nworkers; i++)
		{
			shm_mq	   *mq = shm_toc_lookup(toc, FL_EXPORT_KEY_MQ_BASE + i,
										   false);

			shm_mq_set_receiver(mq, MyProc);
			mqh[i] = shm_mq_attach(mq, seg, NULL);
		}

		fl_export_set_state(hdr, 0, 0, FL_WORKER_RUNNING, NULL);

		/*
		 * Drain in arrival order.  Round-robin with a latch wait keeps the
		 * writer from pinning itself to one queue while the others fill up.
		 */
		while (ndone < p->nworkers)
		{
			bool		progress = false;

			CHECK_FOR_INTERRUPTS();

			for (i = 0; i < p->nworkers; i++)
			{
				shm_mq_result r;

				if (done[i])
					continue;

				r = shm_mq_receive(mqh[i], &len, &data, true);
				if (r == SHM_MQ_SUCCESS)
				{
					progress = true;
					if (len < sizeof(FlExportChunk))
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("pg_fastload export writer: short "
										"message (%zu bytes)", len)));

					chunk = (FlExportChunk *) data;
					if (chunk->nbytes > 0)
						fl_write_all(fd,
									 (char *) data + sizeof(FlExportChunk),
									 chunk->nbytes, p->output);

					rows += chunk->nrows;
					bytes += chunk->nbytes;

					SpinLockAcquire(&hdr->mutex);
					hdr->rows_written = rows;
					hdr->bytes_written = bytes;
					SpinLockRelease(&hdr->mutex);
				}
				else if (r == SHM_MQ_DETACHED)
				{
					done[i] = true;
					ndone++;
					progress = true;
				}
				/* SHM_MQ_WOULD_BLOCK: come back to this queue later. */
			}

			if (!progress)
			{
				(void) WaitLatch(MyLatch,
								 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 10L, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);
			}
		}

		if (pg_fsync(fd) != 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not fsync output file \"%s\": %m",
							p->output)));

		fl_export_set_state(hdr, 0, 0, FL_WORKER_DONE, NULL);
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		FlushErrorState();
		SpinLockAcquire(&hdr->mutex);
		hdr->writer_state = FL_WORKER_FAILED;
		snprintf(hdr->writer_error, sizeof(hdr->writer_error), "%s",
				 edata->message ? edata->message : "unknown error");
		SpinLockRelease(&hdr->mutex);
		FreeErrorData(edata);

		if (fd >= 0)
			close(fd);
		dsm_detach(seg);
		proc_exit(1);
	}
	PG_END_TRY();

	if (fd >= 0 && close(fd) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close output file \"%s\": %m", p->output)));

	for (i = 0; i < p->nworkers; i++)
		shm_mq_detach(mqh[i]);
	dsm_detach(seg);
	proc_exit(0);
}

/* ========================================================================
 * Query worker: scan a block range, format rows, ship bytes
 * ========================================================================*/

PGDLLEXPORT void
pg_fastload_export_worker_main(Datum main_arg)
{
	dsm_segment	   *seg;
	shm_toc		   *toc;
	FlExportShared *hdr;
	FlExportParams *p;
	FlExportWorkerId wid;
	MemoryContext	workctx;
	MemoryContext	rowctx;
	StringInfoData	chunk;
	Relation		rel = NULL;
	TableScanDesc	scan = NULL;
	Writer		   *wt = NULL;
	Snapshot		snap = NULL;
	shm_mq_handle  *mqh = NULL;
	int64			sent = 0;
	bool			use_checker = false;
	int				fieldno = -1;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload export worker: unable to map DSM segment")));
	toc = shm_toc_attach(FL_EXPORT_SHM_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload export worker: bad DSM magic")));

	hdr = shm_toc_lookup(toc, FL_EXPORT_KEY_HEADER, false);
	p = shm_toc_lookup(toc, FL_EXPORT_KEY_PARAMS, false);
	memcpy(&wid, MyBgworkerEntry->bgw_extra, sizeof(wid));

	workctx = AllocSetContextCreate(TopMemoryContext,
									"pg_fastload export worker",
									ALLOCSET_DEFAULT_SIZES);

	PG_TRY();
	{
		char	   *opts;
		char	   *end;

		BackgroundWorkerInitializeConnection(p->dbname, p->username, 0);
		StartTransactionCommand();

		/*
		 * Use the caller's snapshot, not a fresh one: every worker must see
		 * the same rows, i.e. the same set one sequential scan would have
		 * returned when the statement started.
		 */
		snap = RestoreSnapshot(p->data + p->header_len);
		RegisterSnapshot(snap);
		PushActiveSnapshot(snap);

		rel = table_open(p->relid, AccessShareLock);

		/* Rebuild the same writer the leader parameterised. */
		opts = p->data + p->header_len + p->snap_len;
		end = opts + p->opts_len;

		if (p->type == FL_EXPORT_TYPE_CSV)
		{
			wt = CreateCsvWriter(NULL);
			wt->desc = RelationGetDescr(rel);
		}
		else
			wt = CreateBinaryWriter(NULL);

		while (opts < end)
		{
			char	   *key = opts;
			char	   *value = opts + strlen(key) + 1;

			if (key[0] == '\0')
				break;
			if (value >= end)
				break;
			if (!WriterParam(wt, key, value))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid keyword \"%s\" for the export worker",
								key)));
			opts = value + strlen(value) + 1;
		}

		MemoryContextSwitchTo(workctx);
		initStringInfo(&chunk);
		enlargeStringInfo(&chunk, FL_EXPORT_CHUNK_BYTES + 64 * 1024);

		if (p->type == FL_EXPORT_TYPE_CSV)
		{
			CsvWriterPrepare(wt);
			CsvWriterSetSink(wt, &chunk);
		}
		else
		{
			BinaryWriterPrepare(wt);
			BinaryWriterSetSourceDesc(wt, RelationGetDescr(rel));
			BinaryWriterSetSink(wt, &chunk);
			use_checker = (wt->tchecker != NULL);
		}

		/* Transient output-function results: one reset per chunk. */
		rowctx = AllocSetContextCreate(workctx, "pg_fastload export row",
									   ALLOCSET_DEFAULT_SIZES);

		scan = FL_TABLE_BEGINSCAN(rel, snap);
		heap_setscanlimits(scan, wid.start, wid.nblocks);

		mqh = NULL;
		{
			shm_mq	   *mq = shm_toc_lookup(toc,
											FL_EXPORT_KEY_MQ_BASE + wid.worker_id,
											false);

			shm_mq_set_sender(mq, MyProc);
			mqh = shm_mq_attach(mq, seg, NULL);
		}
		fl_export_set_state(hdr, 1, wid.worker_id, FL_WORKER_RUNNING, NULL);

		for (;;)
		{
			int			rows_in_chunk = 0;
			int64		allowed = FL_EXPORT_CHUNK_ROWS;

			CHECK_FOR_INTERRUPTS();

			SpinLockAcquire(&hdr->mutex);
			if (hdr->abort_requested || hdr->writer_state == FL_WORKER_FAILED)
			{
				SpinLockRelease(&hdr->mutex);
				break;
			}
			SpinLockRelease(&hdr->mutex);

			/*
			 * Reserve the next slice of the LIMIT budget.  Over-reserving is
			 * harmless -- the counter is only compared against the limit --
			 * and it keeps the cap exact without a lock per row.
			 */
			if (p->limit > 0 && p->limit != INT64_MAX)
			{
				uint64		old = pg_atomic_fetch_add_u64(&hdr->rows_reserved,
														  (uint64) allowed);

				if (old >= (uint64) p->limit)
					break;
				if (old + (uint64) allowed > (uint64) p->limit)
					allowed = p->limit - (int64) old;
			}

			while (rows_in_chunk < allowed)
			{
				HeapTuple	tuple = heap_getnext(scan, ForwardScanDirection);

				if (tuple == NULL)
					break;

				MemoryContextSwitchTo(rowctx);

				/*
				 * BINARY/FIXED: convert to the OUT_COL layout first (width
				 * and range checks live in the checker), then pack.
				 */
				if (use_checker)
					tuple = wt->tchecker->checker(wt->tchecker, tuple,
												  &fieldno);

				WriterInsert(wt, tuple);
				rows_in_chunk++;

				MemoryContextSwitchTo(workctx);

				if (chunk.len >= FL_EXPORT_CHUNK_BYTES)
					break;
			}

			MemoryContextReset(rowctx);

			if (rows_in_chunk == 0)
				break;

			if (!fl_export_send_chunk(mqh, &chunk, rows_in_chunk))
				break;			/* writer is gone; the leader reports it */

			sent += rows_in_chunk;
		}

		fl_export_set_state(hdr, 1, wid.worker_id, FL_WORKER_DONE, NULL);
		SpinLockAcquire(&hdr->mutex);
		hdr->worker_rows[wid.worker_id] = sent;
		SpinLockRelease(&hdr->mutex);
	}
	PG_CATCH();
	{
		ErrorData  *edata = CopyErrorData();

		FlushErrorState();
		SpinLockAcquire(&hdr->mutex);
		hdr->worker_state[wid.worker_id] = FL_WORKER_FAILED;
		snprintf(hdr->worker_error[wid.worker_id],
				 sizeof(hdr->worker_error[wid.worker_id]), "%s",
				 edata->message ? edata->message : "unknown error");
		SpinLockRelease(&hdr->mutex);
		FreeErrorData(edata);

		if (mqh != NULL)
			shm_mq_detach(mqh);
		if (scan != NULL)
			table_endscan(scan);
		if (rel != NULL)
			table_close(rel, AccessShareLock);
		if (IsTransactionState())
			AbortCurrentTransaction();
		dsm_detach(seg);
		proc_exit(1);
	}
	PG_END_TRY();

	shm_mq_detach(mqh);
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Nothing was written, so there is nothing to commit: ending the
	 * transaction releases the snapshot and the relation lock.
	 */
	PopActiveSnapshot();
	if (IsTransactionState())
		AbortCurrentTransaction();

	dsm_detach(seg);
	proc_exit(0);
}

/* ========================================================================
 * Leader: decide, set up DSM, launch, wait, clean up
 * ========================================================================*/

bool
FlExportParallelSupported(FlExportRequest *req)
{
	int			nworkers = req->nworkers;

	if (req->relname == NULL || req->relid == InvalidOid)
		return false;

	/*
	 * A query worker scans the heap directly with heap_setscanlimits(), so
	 * only plain tables can be split.  Partitioned parents, foreign tables,
	 * views, or a quoted name this code would have to re-parse stay serial.
	 */
	if (strchr(req->relname, '"') != NULL)
		return false;
	if (req->nblocks < (BlockNumber) (FL_EXPORT_MIN_BLOCKS_PER_WORKER * 2))
		return false;

	/* 1 is an explicit request for the serial path, not "use the default". */
	if (nworkers == 1)
		return false;
	if (nworkers <= 0)
		nworkers = FlExportDefaultWorkers();
	if (nworkers < 2)
		return false;
	if (nworkers > FL_EXPORT_MAX_WORKERS)
		nworkers = FL_EXPORT_MAX_WORKERS;

	/* No point handing a worker fewer blocks than it costs to start one. */
	if ((uint64) nworkers > (uint64) req->nblocks / FL_EXPORT_MIN_BLOCKS_PER_WORKER)
		nworkers = Max(2, (int) (req->nblocks / FL_EXPORT_MIN_BLOCKS_PER_WORKER));

	req->nworkers = nworkers;
	return true;
}

bool
FlExportParallelTry(FlExportRequest *req, int64 *rows, int64 *bytes)
{
	int					nworkers = req->nworkers;
	MemoryContext		runctx;
	MemoryContext		outerctx;
	dsm_segment		   *seg = NULL;
	shm_toc			   *toc = NULL;
	FlExportShared	   *hdr = NULL;
	FlExportParams	   *p;
	Size				params_size;
	Size				segsize;
	BackgroundWorkerHandle **handles = NULL;
	BackgroundWorker	worker;
	shm_toc_estimator	e;
	StringInfoData		header;
	int					i;
	bool				ready = false;

	if (req->relname == NULL || req->relid == InvalidOid)
		return false;

	if (nworkers == 1)
		return false;
	if (nworkers <= 0)
		nworkers = FlExportDefaultWorkers();
	if (nworkers < 2)
		return false;
	if (nworkers > FL_EXPORT_MAX_WORKERS)
		nworkers = FL_EXPORT_MAX_WORKERS;

	runctx = AllocSetContextCreate(TopMemoryContext,
								   "pg_fastload export parallel",
								   ALLOCSET_DEFAULT_SIZES);
	outerctx = MemoryContextSwitchTo(runctx);

	/* The header line is built by the same code the serial path uses. */
	initStringInfo(&header);

	PG_TRY();
	{
		Size		header_len = 0;
		Size		snap_len;
		Size		opts_len = 0;
		Snapshot	snap;

		if (req->header && req->type == FL_EXPORT_TYPE_CSV)
		{
			CsvWriterPrepare(req->wt);
			CsvWriterAppendHeader(req->wt, &header);
			header_len = header.len;
		}
		else if (req->type == FL_EXPORT_TYPE_BINARY)
		{
			BinaryWriterPrepare(req->wt);
			BinaryWriterSetSourceDesc(req->wt, req->desc);
		}

		snap = GetActiveSnapshot();
		if (snap == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_fastload export: no active snapshot")));
		snap_len = EstimateSnapshotSpace(snap);

		if (req->writer_opts != NULL)
			opts_len = req->writer_opts->len;

		params_size = offsetof(FlExportParams, data) + header_len +
			snap_len + opts_len;

		shm_toc_initialize_estimator(&e);
		shm_toc_estimate_chunk(&e, sizeof(FlExportShared));
		shm_toc_estimate_chunk(&e, params_size);
		for (i = 0; i < nworkers; i++)
			shm_toc_estimate_chunk(&e, (Size) FL_EXPORT_MQ_SIZE);
		shm_toc_estimate_keys(&e, 2 + nworkers);
		segsize = shm_toc_estimate(&e);

		seg = dsm_create(segsize, 0);
		dsm_pin_segment(seg);
		toc = shm_toc_create(FL_EXPORT_SHM_MAGIC, dsm_segment_address(seg),
							 segsize);

		hdr = shm_toc_allocate(toc, sizeof(FlExportShared));
		MemSet(hdr, 0, sizeof(FlExportShared));
		SpinLockInit(&hdr->mutex);
		pg_atomic_init_u64(&hdr->rows_reserved, 0);
		hdr->nworkers = nworkers;
		hdr->writer_state = FL_WORKER_INIT;
		for (i = 0; i < nworkers; i++)
			hdr->worker_state[i] = FL_WORKER_INIT;
		shm_toc_insert(toc, FL_EXPORT_KEY_HEADER, hdr);

		p = shm_toc_allocate(toc, params_size);
		MemSet(p, 0, params_size);
		p->nworkers = nworkers;
		p->type = req->type;
		p->natts = req->desc ? req->desc->natts : 0;
		p->limit = req->limit;
		p->header = req->header;
		p->relid = req->relid;
		p->nblocks = req->nblocks;
		p->header_len = (int) header_len;
		p->snap_len = (int) snap_len;
		p->opts_len = (int) opts_len;
		strlcpy(p->output, req->output, MAXPGPATH);
		strlcpy(p->relname, req->relname, MAXPGPATH);

		{
			char	   *db = get_database_name(MyDatabaseId);
			char	   *usr = GetUserNameFromId(GetUserId(), false);

			strlcpy(p->dbname, db ? db : "", NAMEDATALEN);
			strlcpy(p->username, usr ? usr : "", NAMEDATALEN);
			if (db)
				pfree(db);
			if (usr)
				pfree(usr);
		}

		if (header_len > 0)
			memcpy(p->data, header.data, header_len);
		SerializeSnapshot(snap, p->data + header_len);
		if (opts_len > 0)
			memcpy(p->data + header_len + snap_len,
				   req->writer_opts->data, opts_len);

		shm_toc_insert(toc, FL_EXPORT_KEY_PARAMS, p);

		for (i = 0; i < nworkers; i++)
		{
			shm_mq	   *mq;

			mq = shm_mq_create(shm_toc_allocate(toc, (Size) FL_EXPORT_MQ_SIZE),
							   (Size) FL_EXPORT_MQ_SIZE);
			shm_toc_insert(toc, FL_EXPORT_KEY_MQ_BASE + i, mq);
		}

		handles = palloc0(sizeof(BackgroundWorkerHandle *) * (nworkers + 1));

		/* Writer first: it owns the file and must be ready before rows flow. */
		MemSet(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_ConsistentState;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		strlcpy(worker.bgw_library_name, "pg_fastload", MAXPGPATH);
		strlcpy(worker.bgw_function_name, "pg_fastload_export_writer_main",
				BGW_MAXLEN);
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_fastload export writer");
		worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
		worker.bgw_notify_pid = MyProcPid;

		if (!RegisterDynamicBackgroundWorker(&worker, &handles[0]))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("could not register pg_fastload export writer"),
					 errhint("PARALLEL export needs max_worker_processes to "
							 "cover workers (%d) + 1 writer.", nworkers)));

		for (i = 0; i < 120000; i++)
		{
			int			st;
			char		errbuf[256];

			CHECK_FOR_INTERRUPTS();
			SpinLockAcquire(&hdr->mutex);
			st = hdr->writer_state;
			memcpy(errbuf, hdr->writer_error, sizeof(errbuf));
			SpinLockRelease(&hdr->mutex);

			if (st == FL_WORKER_RUNNING || st == FL_WORKER_DONE)
			{
				ready = true;
				break;
			}
			if (st == FL_WORKER_FAILED)
				ereport(ERROR,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("pg_fastload export writer failed: %s",
								errbuf)));
			pg_usleep(1000L);
		}
		if (!ready)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_fastload export writer did not become ready")));

		MemSet(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_ConsistentState;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		strlcpy(worker.bgw_library_name, "pg_fastload", MAXPGPATH);
		strlcpy(worker.bgw_function_name, "pg_fastload_export_worker_main",
				BGW_MAXLEN);
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_fastload export worker");
		worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
		worker.bgw_notify_pid = MyProcPid;

		for (i = 0; i < nworkers; i++)
		{
			FlExportWorkerId wid;
			BlockNumber	start;

			start = (BlockNumber) ((uint64) req->nblocks * i / nworkers);
			wid.worker_id = i;
			wid.start = start;
			wid.nblocks = (BlockNumber)
				((uint64) req->nblocks * (i + 1) / nworkers) - start;
			MemSet(worker.bgw_extra, 0, BGW_EXTRALEN);
			memcpy(worker.bgw_extra, &wid, sizeof(wid));

			if (!RegisterDynamicBackgroundWorker(&worker, &handles[i + 1]))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
						 errmsg("could not register pg_fastload export worker"),
						 errhint("PARALLEL export needs max_worker_processes "
								 "to cover workers (%d) + 1 writer.", nworkers)));
		}

		LoggerLog(INFO, "pg_fastload: export PARALLEL=ON, TYPE=%s, workers=%d, "
				  "blocks=%u\n",
				  req->type == FL_EXPORT_TYPE_CSV ? "CSV" : "BINARY/FIXED",
				  nworkers, req->nblocks);

		/* Wait for the writer, republishing progress on a fixed cadence. */
		for (;;)
		{
			int			st;
			char		errbuf[256];
			int64		rows_done;
			int64		bytes_done;

			CHECK_FOR_INTERRUPTS();

			SpinLockAcquire(&hdr->mutex);
			st = hdr->writer_state;
			rows_done = hdr->rows_written;
			bytes_done = hdr->bytes_written;
			memcpy(errbuf, hdr->writer_error, sizeof(errbuf));
			SpinLockRelease(&hdr->mutex);

			ProgressUpdate(rows_done, bytes_done);

			if (st == FL_WORKER_DONE)
				break;
			if (st == FL_WORKER_FAILED)
			{
				/* Stop the workers before unwinding. */
				SpinLockAcquire(&hdr->mutex);
				hdr->abort_requested = true;
				SpinLockRelease(&hdr->mutex);
				ereport(ERROR,
						(errcode(ERRCODE_IO_ERROR),
						 errmsg("pg_fastload export writer failed: %s",
								errbuf)));
			}

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 FL_EXPORT_PROGRESS_MS, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}

		/* A worker that died turns the whole export into a failure. */
		for (i = 0; i < nworkers; i++)
		{
			int			st;
			char		errbuf[256];

			SpinLockAcquire(&hdr->mutex);
			st = hdr->worker_state[i];
			memcpy(errbuf, hdr->worker_error[i], sizeof(errbuf));
			SpinLockRelease(&hdr->mutex);

			if (st != FL_WORKER_DONE)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("pg_fastload export worker %d failed "
								"(state=%d): %s", i, st, errbuf)));
		}

		SpinLockAcquire(&hdr->mutex);
		*rows = hdr->rows_written;
		SpinLockRelease(&hdr->mutex);
	}
	PG_CATCH();
	{
		int			j;

		if (handles != NULL)
		{
			for (j = 0; j < nworkers + 1; j++)
			{
				BgwHandleStatus st;
				pid_t		pid;

				if (handles[j] == NULL)
					continue;
				st = GetBackgroundWorkerPid(handles[j], &pid);
				if (st == BGWH_STARTED || st == BGWH_NOT_YET_STARTED)
					TerminateBackgroundWorker(handles[j]);
			}
		}
		MemoryContextSwitchTo(outerctx);
		if (seg != NULL)
			dsm_detach(seg);
		MemoryContextDelete(runctx);
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(outerctx);
	if (seg != NULL)
		dsm_detach(seg);
	MemoryContextDelete(runctx);

	return true;
}
