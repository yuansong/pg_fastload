/*
 * pg_fastload: lib/fastload_worker.c
 *
 *   Parallel CSV processes (independent PostgreSQL background workers).
 *
 *   Based on pg_bulkload 3.1.23
 *       Copyright (c) 2008-2026, NTT, Inc.
 *   Parallel CSV loader extensions
 *       Copyright (c) 2026, yuanyuansong
 *
 *   Single-reader design: ONE reader background worker opens the input
 *   file and walks it from byte 0 exactly once.  For TYPE=CSV it uses the
 *   quote-aware record-boundary state machine (CRLF, lone CR, lone LF,
 *   doubled quotes and newlines inside quoted fields are all handled).
 *   For TYPE=BINARY / TYPE=FIXED (same fixed-length parser) it simply
 *   splits the stream by the STRIDE record length -- see
 *   fl_reader_scan_fixed().  The reader applies the SKIP/LIMIT window
 *   and routes every in-window record verbatim into the per-worker input
 *   queue of its owner -- a PostgreSQL NATIVE shm_mq, the same kernel
 *   transport the workers already use to stream tuples back to the
 *   leader -- where the owning worker picks it up, splits fields,
 *   forms a HeapTuple by reusing TupleFormer and streams it back over a
 *   second native shm_mq.  Both directions of the pipeline are therefore
 *   kernel shm_mq; no hand-rolled shared-memory protocol remains.
 *
 *   Routing is CHUNK-BASED round-robin: the reader hands each worker a
 *   contiguous ~FL_ROUTE_BYTES block of records before moving to the next
 *   worker, instead of interleaving record by record.  Strict per-record
 *   modulo routing was measured to cause head-of-line blocking at low
 *   worker counts (w=2: the reader blocked 9.2 s of 10.9 s wall -- one
 *   full queue stalls the single reader, which in turn starves every
 *   other queue of its interleaved records).  Chunking turns the pipeline
 *   into "reader fills worker i's queue while worker i-1 drains", which
 *   removes the lock-step coupling.
 *
 *   Compared with plan B (every worker scanning the whole file) this makes
 *   the file I/O 1x instead of Nx and removes the per-worker boundary
 *   scanning entirely; the per-record parse cost is unchanged and still
 *   spread across the workers.
 */
#include "pg_fastload.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/tupdesc.h"
#include "access/tupdesc_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "fastload_parallel.h"
#include "binary.h"
#include "reader.h"

#include "portability/instr_time.h"

/*
 * reader: input read chunk.  Each read() is a blocking syscall, so the
 * chunk must be large: measured on the 1 GiB baseline, 64 KiB reads made
 * the reader issue 16k syscalls per load, and on a noisy host each one of
 * those can be inflated to ~2 ms (vmstat shows 0 steal / 0 iowait -- it is
 * wake-up latency, not bandwidth), turning a 3 s cold load into 33 s.
 * 1 MiB chunks cut the blocking-syscall count (and therefore the exposure)
 * 16x while keeping the parse loop's memory footprint small.
 */
#define FL_READ_CHUNK		(1024 * 1024)
#define FL_REC_INIT			(64 * 1024)
#define FL_REC_MAX			(256 * 1024 * 1024)
/*
 * reader: bytes routed to one owner before switching to the next one
 * (chunk-based round-robin; see the file header).  Half the input queue
 * size, so the reader can stay at most ~2 chunks ahead of any single
 * worker.
 */
#define FL_ROUTE_BYTES		(512 * 1024)
/*
 * reader: staging batch flushed to an owner's input queue as ONE message;
 * the worker then splits the record frames straight out of the message
 * (zero-copy fast path) without extra synchronization.  One flush per
 * FL_ROUTE_BYTES routing window keeps the blocking send count low (same
 * wake-up-latency argument as FL_READ_CHUNK above) without exceeding the
 * per-window route quota.
 */
#define FL_STAGE_CAP		(512 * 1024)
/*
 * worker: frame reassembly buffer for records that straddle two reader
 * messages.  Whole frames inside one message never touch this buffer --
 * they are emitted straight from the queue memory.
 */
#define FL_PEND_INIT		(64 * 1024)
#define FL_HDR_LEN			8	/* {uint32 len; uint32 reserved} */

/*
 * Reader-side routing state: one staging buffer per owner, the chunked
 * round-robin cursor, and every worker's input queue handle.
 */
typedef struct FastloadRouter
{
	shm_mq_handle  **out;		/* worker i's input queue (reader = sender) */
	char		   **stage;		/* per-owner staging buffer */
	int			   *stage_len;
	int				nworkers;
	int				owner_cur;	/* current chunk owner */
	int64			route_left; /* bytes still routed to owner_cur */
	bool			peer_gone;	/* a worker detached: stop routing */
} FastloadRouter;

typedef struct FastloadField
{
	char   *str;		/* NUL-terminated, dequoted value inside field_buf */
	int		rawlen;		/* raw byte length before dequoting (for NULL test) */
	bool	is_null;
} FastloadField;

typedef struct FastloadWorkerCtx
{
	FastloadParams	   *p;
	FastloadShared	   *hdr;
	shm_mq_handle	   *in_mqh;	/* reader->worker input queue */
	int					worker_id;
	shm_mq_handle	   *mqh;

	TupleDesc			desc;
	TupleFormer			former;
	Filter				filter;

	/* Long-lived allocations, outside any transaction memory context. */
	MemoryContext		workctx;
	/* Per-record scratch: reset after every emitted tuple. */
	MemoryContext		recctx;

	char			   *field_buf;
	int					field_cap;
	FastloadField	   *fields;
	int					fields_cap;
	int					nfields;
	int					maxfields;
	int					minfields;

	/*
	 * FL_PMODE_FIXED (TYPE = BINARY / FIXED): COL field descriptors rebuilt
	 * from the DSM parameter blob (BinaryDeserializeFields), plus a scratch
	 * copy of one record -- queue memory is read-only for us, but the fixed
	 * read functions temporarily NUL-terminate fields in place.
	 */
	Field			   *bfields;
	int					nbfields;
	char			   *fix_scratch;

	int64				sent;
	bool				leader_gone;

#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time			prof_scan;	/* ring wait time (input acquisition) */
	instr_time			prof_form;
	instr_time			prof_ipc;
#endif
} FastloadWorkerCtx;

PGDLLEXPORT void pg_fastload_worker_main(Datum main_arg);
PGDLLEXPORT void pg_fastload_reader_main(Datum main_arg);

static TupleDesc fl_build_tupdesc(FastloadParams *p);
static void fl_worker_init(FastloadWorkerCtx *ctx, dsm_segment *seg,
						   shm_toc *toc);
static void fl_consume_input(FastloadWorkerCtx *ctx);
static void fl_emit_record(FastloadWorkerCtx *ctx, char *rec, int len);
static void fl_emit_record_fixed(FastloadWorkerCtx *ctx, char *rec, int len);
static void fl_parse_fields(FastloadWorkerCtx *ctx, char *rec, int len);
static void fl_append_run(char **rec, int *used, int *cap,
						  const char *src, int len);
static void fl_reader_scan(FastloadParams *p, FastloadRouter *router);
static void fl_reader_scan_fixed(FastloadParams *p, FastloadRouter *router);

/* ------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

void
pg_fastload_worker_main(Datum main_arg)
{
	dsm_segment		   *seg;
	shm_toc			   *toc;
	FastloadWorkerCtx	ctx;
	FastloadWorkerId	wid;
	FastloadShared	   *hdr;
	PGPROC			   *registrant;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	MemSet(&ctx, 0, sizeof(ctx));

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload worker: unable to map DSM segment")));
	toc = shm_toc_attach(FASTLOAD_SHM_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload worker: bad DSM magic")));

	/* worker_id / nworkers live in bgw_extra (no shared counter to race on) */
	memcpy(&wid, MyBgworkerEntry->bgw_extra, sizeof(wid));
	ctx.worker_id = wid.worker_id;

	hdr = shm_toc_lookup(toc, FASTLOAD_KEY_HEADER, false);
	ctx.hdr = hdr;

	fl_worker_init(&ctx, seg, toc);

	/*
	 * Announce readiness and wake the leader's shm_mq_wait_for_attach().
	 * All DSM structures have been attached at this point.
	 */
	SpinLockAcquire(&hdr->mutex);
	hdr->worker_state[ctx.worker_id] = FL_WORKER_RUNNING;
	SpinLockRelease(&hdr->mutex);
	registrant = BackendPidGetProc(MyBgworkerEntry->bgw_notify_pid);
	if (registrant != NULL)
		SetLatch(&registrant->procLatch);

	PG_TRY();
	{
		fl_consume_input(&ctx);

		SpinLockAcquire(&hdr->mutex);
		hdr->worker_state[ctx.worker_id] = FL_WORKER_DONE;
		hdr->worker_rows[ctx.worker_id] = ctx.sent;
#ifdef ENABLE_FASTLOAD_PROFILE
		/*
		 * Publish this process's stage times while the DSM segment is
		 * still mapped.  The leader sums them into the <WORKER> group.
		 */
		hdr->worker_form[ctx.worker_id] =
			INSTR_TIME_GET_DOUBLE(ctx.prof_form);
		hdr->worker_ipc[ctx.worker_id] =
			INSTR_TIME_GET_DOUBLE(ctx.prof_ipc);
		hdr->worker_scan[ctx.worker_id] =
			INSTR_TIME_GET_DOUBLE(ctx.prof_scan);
#endif
		SpinLockRelease(&hdr->mutex);
	}
	PG_CATCH();
	{
		SpinLockAcquire(&hdr->mutex);
		hdr->worker_state[ctx.worker_id] = FL_WORKER_FAILED;
		snprintf(hdr->worker_error[ctx.worker_id],
				 sizeof(hdr->worker_error[ctx.worker_id]),
				 "worker %d aborted (see server log)", ctx.worker_id);
		SpinLockRelease(&hdr->mutex);
		PG_RE_THROW();
	}
	PG_END_TRY();

	shm_mq_detach(ctx.mqh);
	shm_mq_detach(ctx.in_mqh);
	dsm_detach(seg);

	/*
	 * There is deliberately no CommitTransactionCommand() here.  The
	 * transaction started in fl_worker_init() only exists so that
	 * getTypeInputInfo()/fmgr_info() can read the catalogs.  All of our own
	 * allocations live in workctx (a child of TopMemoryContext), so letting
	 * proc_exit() abort the transaction is both safe and simpler.
	 */
	proc_exit(0);
}

/* ------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------ */

static TupleDesc
fl_build_tupdesc(FastloadParams *p)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(p->natts);
	for (i = 0; i < p->natts; i++)
	{
		TupleDescInitEntry(desc, (AttrNumber) (i + 1),
						   p->cols[i].attname,
						   p->cols[i].atttypid,
						   p->cols[i].atttypmod, 0);
		TupleDescInitEntryCollation(desc, (AttrNumber) (i + 1),
									p->cols[i].attcollation);
		TupleDescAttr(desc, i)->attnotnull = p->cols[i].attnotnull;
		TupleDescAttr(desc, i)->attisdropped = p->cols[i].attisdropped;
		populate_compact_attribute(desc, i);
	}
	return desc;
}

static void
fl_worker_init(FastloadWorkerCtx *ctx, dsm_segment *seg, shm_toc *toc)
{
	FastloadParams *p;
	shm_mq		   *mq;

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_ZERO(ctx->prof_scan);
	INSTR_TIME_SET_ZERO(ctx->prof_form);
	INSTR_TIME_SET_ZERO(ctx->prof_ipc);
#endif

	/*
	 * Keep every allocation for the whole process outside the transaction
	 * memory context.  A per-record child context is reset after each tuple,
	 * otherwise 130M heap_form_tuple() results would never be freed.
	 */
	ctx->workctx = AllocSetContextCreate(TopMemoryContext,
										 "pg_fastload worker",
										 ALLOCSET_DEFAULT_SIZES);
	ctx->recctx = AllocSetContextCreate(ctx->workctx,
										 "pg_fastload worker record",
										 ALLOCSET_SMALL_SIZES);
	(void) MemoryContextSwitchTo(ctx->workctx);

	p = shm_toc_lookup(toc, FASTLOAD_KEY_PARAMS, false);
	ctx->p = p;

	/* worker needs catalog access for getTypeInputInfo() in TupleFormerInit */
	BackgroundWorkerInitializeConnection(p->dbname, p->username, 0);
	StartTransactionCommand();
	(void) MemoryContextSwitchTo(ctx->workctx);

	ctx->desc = fl_build_tupdesc(p);

	MemSet(&ctx->filter, 0, sizeof(ctx->filter));
	ctx->filter.funcstr = NULL;
	TupleFormerInit(&ctx->former, &ctx->filter, ctx->desc);
	ctx->maxfields = ctx->former.maxfields;
	ctx->minfields = ctx->former.minfields;

	/*
	 * One extra slot beyond maxfields lets us detect "extra data" without
	 * ever writing past the end of the array.
	 */
	ctx->fields_cap = Max(ctx->maxfields + 2, 2);
	ctx->field_cap = FL_REC_INIT;
	ctx->field_buf = palloc(ctx->field_cap);
	ctx->fields = palloc(sizeof(FastloadField) * ctx->fields_cap);

	/* FL_PMODE_FIXED: rebuild the COL field descriptors from the blob. */
	if (p->pmode == FL_PMODE_FIXED)
	{
		const char *blob = (const char *) p + offsetof(FastloadParams, cols) +
			sizeof(FastloadColumn) * p->natts;

		ctx->nbfields = p->nbinfields;
		BinaryDeserializeFields(blob, p->nbinfields, &ctx->bfields);
		ctx->fix_scratch = palloc(p->rec_stride + 1);
	}

	mq = shm_toc_lookup(toc, FASTLOAD_KEY_MQ_BASE + ctx->worker_id, false);
	shm_mq_set_sender(mq, MyProc);
	ctx->mqh = shm_mq_attach(mq, seg, NULL);

	/* reader->worker input queue: this worker is the receiver */
	mq = shm_toc_lookup(toc, FASTLOAD_KEY_INPUT_BASE + ctx->worker_id, false);
	shm_mq_set_receiver(mq, MyProc);
	ctx->in_mqh = shm_mq_attach(mq, seg, NULL);
}

static void
fl_append_run(char **rec, int *used, int *cap, const char *src, int len)
{
	int		newcap;

	if (len <= 0)
		return;
	if (*used + len + 1 > *cap)
	{
		newcap = *cap;
		while (*used + len + 1 > newcap)
		{
			if (newcap >= FL_REC_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("pg_fastload: single CSV record exceeds %d bytes",
								FL_REC_MAX)));
			newcap = Min(newcap * 2, FL_REC_MAX);
		}
		*rec = repalloc(*rec, newcap);
		*cap = newcap;
	}
	memcpy(*rec + *used, src, len);
	*used += len;
}

/* ------------------------------------------------------------------------
 * Worker: drain this worker's input queue
 * ------------------------------------------------------------------------ */

static void
fl_consume_input(FastloadWorkerCtx *ctx)
{
	char	   *pend;
	Size		pend_cap = FL_PEND_INIT;
	Size		pend_len = 0;
	uint32		pend_total = 0;	/* pending frame length, 0 until the
								 * 8-byte header is reassembled */
#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time	t_start;
	instr_time	t_end;
	INSTR_TIME_SET_CURRENT(t_start);
#endif

	pend = palloc(pend_cap);

	for (;;)
	{
		Size			mlen;
		void		   *mdata;
		shm_mq_result	r;
		const char	   *p;
		Size			n;

		r = shm_mq_receive(ctx->in_mqh, &mlen, &mdata, false);
		if (r == SHM_MQ_DETACHED)
		{
			/*
			 * The reader detached its end.  A clean EOF has
			 * reader_state=DONE (set before the detach); anything else
			 * means the reader died in the middle of the record stream.
			 */
			int		st;
			char	errbuf[256];

			SpinLockAcquire(&ctx->hdr->mutex);
			st = ctx->hdr->reader_state;
			memcpy(errbuf, ctx->hdr->reader_error, sizeof(errbuf));
			SpinLockRelease(&ctx->hdr->mutex);

			if (st != FL_WORKER_DONE)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("pg_fastload reader terminated in the "
								"middle of the record stream: %s", errbuf)));
			break;
		}

		/*
		 * Split the record frames out of the message.  The reader has
		 * already applied SKIP / LIMIT and chunk-based round-robin
		 * ownership, so every frame here is one to parse and emit.
		 */
		p = (const char *) mdata;
		n = mlen;
		while (n > 0)
		{
			Size	take;

			if (pend_total == 0 && pend_len < FL_HDR_LEN)
			{
				uint32		len;

				/*
				 * Fast path: when the whole next frame is inside this
				 * message, emit it straight from the queue memory --
				 * no copy at all.  fl_emit_record() only reads the
				 * record bytes (fields are dequoted into field_buf).
				 */
				if (pend_len == 0 && n >= FL_HDR_LEN)
				{
					memcpy(&len, p, sizeof(uint32));
					if (FL_HDR_LEN + (Size) len <= n)
					{
						fl_emit_record(ctx, (char *) p + FL_HDR_LEN,
									   (int) len);
						p += FL_HDR_LEN + (Size) len;
						n -= FL_HDR_LEN + (Size) len;
						continue;
					}
				}

				/* Reassemble the 8-byte header into pend. */
				take = Min(n, FL_HDR_LEN - pend_len);
				memcpy(pend + pend_len, p, take);
				pend_len += take;
				p += take;
				n -= take;
				if (pend_len < FL_HDR_LEN)
					break;		/* message exhausted, header incomplete */

				memcpy(&pend_total, pend, sizeof(uint32));
				if (FL_HDR_LEN + (Size) pend_total > pend_cap)
				{
					pend_cap = FL_HDR_LEN + (Size) pend_total;
					pend = repalloc(pend, pend_cap);
				}
				if (pend_len == FL_HDR_LEN + (Size) pend_total)
				{
					/* zero-length record (empty CSV line) */
					fl_emit_record(ctx, pend + FL_HDR_LEN, (int) pend_total);
					pend_len = 0;
					pend_total = 0;
				}
			}
			else
			{
				/* body of the pending frame */
				take = Min(n, FL_HDR_LEN + (Size) pend_total - pend_len);
				memcpy(pend + pend_len, p, take);
				pend_len += take;
				p += take;
				n -= take;
				if (pend_len == FL_HDR_LEN + (Size) pend_total)
				{
					fl_emit_record(ctx, pend + FL_HDR_LEN, (int) pend_total);
					pend_len = 0;
					pend_total = 0;
				}
			}
		}
	}

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t_end);
	ctx->prof_scan = t_end;
	INSTR_TIME_SUBTRACT(ctx->prof_scan, t_start);
	INSTR_TIME_SUBTRACT(ctx->prof_scan, ctx->prof_form);
	INSTR_TIME_SUBTRACT(ctx->prof_scan, ctx->prof_ipc);
#endif
}

/* ------------------------------------------------------------------------
 * Reader process: single-pass record splitter and router
 *
 * Walks the whole input file ONCE with the same quote-aware boundary state
 * machine the plan-B workers used, applies the SKIP/LIMIT window (global
 * record-number window [skip, skip + limit), subtract first to avoid
 * overflow), and routes every in-window record verbatim into the input
 * shm_mq of its chunk owner (~FL_ROUTE_BYTES of contiguous records per
 * worker in round-robin).  Record content is byte-for-byte what the serial
 * path materializes; dequoting and field splitting remain in the workers.
 * ------------------------------------------------------------------------ */

/*
 * Send one byte fragment into a worker's input queue.  Returns false when
 * the worker is gone (queue detached -- e.g. the load was aborted or the
 * leader hit LIMIT and stopped receiving); peer_gone then makes the whole
 * scan stop gracefully.
 */
static bool
fl_router_send(FastloadRouter *router, int owner, const void *data, Size len)
{
	shm_mq_result	r;

	if (router->peer_gone)
		return false;

	r = shm_mq_send(router->out[owner], len, data, false, false);
	if (r == SHM_MQ_DETACHED)
	{
		router->peer_gone = true;
		return false;
	}
	return true;
}

/* Flush one owner's staging buffer into its input queue (one message). */
static void
fl_stage_flush(FastloadRouter *router, int owner)
{
	if (router->stage_len[owner] > 0)
	{
		fl_router_send(router, owner, router->stage[owner],
					   (Size) router->stage_len[owner]);
		router->stage_len[owner] = 0;
	}
}

/*
 * Apply the SKIP/LIMIT window and route one record into the current
 * chunk owner's staging buffer (advancing to the next owner whenever
 * FL_ROUTE_BYTES have been routed to the current one; flush the stage
 * first when it cannot fit).  Returns true when the scan must stop
 * (window fully delivered, or a worker is gone), false when the record
 * was routed (or dropped as skipped).
 *
 * Chunk accounting is over ROUTED bytes only (post-SKIP/LIMIT), so the
 * per-worker share stays balanced for the delivered window regardless
 * of SKIP; exact per-record modulo distribution is NOT preserved (and
 * was never an output contract -- content completeness is).
 */
static bool
fl_route_record(FastloadParams *p, FastloadRouter *router,
				int64 rec_no, const char *rec, int len)
{
	uint32		hdr8[2];
	int			owner;
	Size		need;

	if (router->peer_gone)
		return true;			/* a worker is gone: stop the scan */

	if (rec_no < p->skip)
		return false;
	if (p->limit != INT64_MAX && (rec_no - p->skip) >= p->limit)
		return true;			/* whole window delivered: stop early */

	if (router->route_left <= 0)
	{
		router->owner_cur = (router->owner_cur + 1) % p->nworkers;
		router->route_left = FL_ROUTE_BYTES;
	}
	owner = router->owner_cur;
	router->route_left -= (int64) sizeof(hdr8) + len;

	need = sizeof(hdr8) + (Size) len;

	if (router->stage_len[owner] > 0 &&
		(Size) router->stage_len[owner] + need > (Size) FL_STAGE_CAP)
	{
		fl_stage_flush(router, owner);
		if (router->peer_gone)
			return true;
	}

	if (need > (Size) FL_STAGE_CAP)
	{
		/* jumbo record: stream directly, one message per piece */
		int		done = 0;

		hdr8[0] = (uint32) len;
		hdr8[1] = 0;
		if (!fl_router_send(router, owner, hdr8, sizeof(hdr8)))
			return true;
		while (done < len)
		{
			int		piece = Min((int) FL_ROUTE_BYTES, len - done);

			if (!fl_router_send(router, owner, rec + done, piece))
				return true;
			done += piece;
		}
		return false;
	}

	hdr8[0] = (uint32) len;
	hdr8[1] = 0;
	memcpy(router->stage[owner] + router->stage_len[owner], hdr8,
		   sizeof(hdr8));
	router->stage_len[owner] += sizeof(hdr8);
	memcpy(router->stage[owner] + router->stage_len[owner], rec, len);
	router->stage_len[owner] += len;
	return false;
}

static void
fl_reader_scan(FastloadParams *p, FastloadRouter *router)
{
	int			fd;
	char	   *chunk;
	char	   *pend;
	int			pend_cap = FL_READ_CHUNK + 2;
	int			pend_len = 0;
	char	   *rec;
	int			rec_cap = FL_REC_INIT;
	int			rec_used = 0;
	bool		in_quote = false;
	bool		eof = false;
	bool		stopped = false;
	int64		rec_no = 0;
	int			rounds = 0;

	/*
	 * Per-owner staging buffers: consecutive records for the same owner are
	 * coalesced here and flushed to the queue in ~64 KiB messages.  Routing
	 * switches owner every ~FL_ROUTE_BYTES, so within a chunk the reader
	 * would otherwise take the queue lock twice per record and the workers
	 * would wake up to one record at a time; measured, that capped the
	 * pipeline at ~100 MB/s for w=2.  Per-queue FIFO order is preserved
	 * (each stage is flushed in order), so the byte stream each worker sees
	 * is identical to the unbatched writer.
	 */
	int			w;

	for (w = 0; w < p->nworkers; w++)
	{
		router->stage[w] = palloc(FL_STAGE_CAP);
		router->stage_len[w] = 0;
	}

	fd = open(p->infile, O_RDONLY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_fastload reader: could not open input file \"%s\": %m",
						p->infile)));

	chunk = palloc(FL_READ_CHUNK);
	pend = palloc(pend_cap);
	rec = palloc(rec_cap);

	for (;;)
	{
		int		n = 0;
		int		total;
		int		limit;
		int		i = 0;
		int		run = 0;		/* first byte not yet appended to rec */

		if (!eof)
		{
			n = (int) read(fd, chunk, FL_READ_CHUNK);
			if (n < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("pg_fastload reader: could not read input file: %m")));
			if (n == 0)
				eof = true;
		}

		if (n > 0)
		{
			memcpy(pend + pend_len, chunk, n);
			pend_len += n;
		}
		total = pend_len;

		if (total == 0)
			break;			/* empty file, or nothing left */

		/*
		 * Every byte in [0, limit) has a real lookahead byte in pend; at
		 * EOF every byte is decided with '\0' as the lookahead.
		 */
		limit = eof ? total : total - 1;

		while (i < limit)
		{
			char	c = pend[i];
			char	nx = (i + 1 < total) ? pend[i + 1] : '\0';
			int		step = 1;

			if (in_quote)
			{
				if (c == p->escape &&
					(nx == p->quote || nx == p->escape))
				{
					/* both bytes are verbatim content: fold into the run */
					fl_append_run(&rec, &rec_used, &rec_cap,
								  pend + run, i + 2 - run);
					run = i + 2;
					step = 2;
				}
				else
				{
					/* CRLF / LF / CR inside quotes stays verbatim */
					if (c == p->quote)
						in_quote = false;
					/* c stays in the pending run */
				}
			}
			else if (c == p->quote)
			{
				/* c stays in the pending run; dequoting happens later */
				in_quote = true;
			}
			else if (c == '\r' || c == '\n')
			{
				/* terminator bytes are never part of the record */
				fl_append_run(&rec, &rec_used, &rec_cap,
							  pend + run, i - run);

				/* SKIP / LIMIT window, then route to the chunk owner */
				if (fl_route_record(p, router, rec_no, rec, rec_used))
				{
					/* whole window delivered: stop the scan early */
					stopped = true;
					break;
				}

				rec_used = 0;
				rec_no++;
				step = (c == '\r' && nx == '\n') ? 2 : 1;
				run = i + step;	/* skip the whole CRLF pair */
			}
			/* else: ordinary byte, stays in the pending run */

			i += step;
		}

		if (stopped)
			break;

		/* Flush this round's tail into the record still being built. */
		if (run < limit)
			fl_append_run(&rec, &rec_used, &rec_cap,
						  pend + run, limit - run);

		/* Keep the unconsumed tail for the next round. */
		if (i < total)
		{
			memmove(pend, pend + i, total - i);
			pend_len = total - i;
		}
		else
			pend_len = 0;

		if (eof && pend_len == 0)
			break;

		if ((++rounds & 0x3F) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	if (!stopped && in_quote)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("unterminated CSV quoted field")));

	/* Final record without a trailing newline / CR. */
	if (!stopped && rec_used > 0)
	{
		(void) fl_route_record(p, router, rec_no, rec, rec_used);
		rec_no++;
	}

	/* Flush whatever is left in the staging buffers, then signal EOF. */
	for (w = 0; w < p->nworkers; w++)
		fl_stage_flush(router, w);

	close(fd);
}

/*
 * FL_PMODE_FIXED (TYPE = BINARY / FIXED): the input is a stream of
 * fixed-length records (STRIDE bytes each, no delimiters, no quoting).
 * Records are routed verbatim with the same 8-byte frame protocol the CSV
 * scan uses; the worker-side stride check in fl_emit_record_fixed() makes
 * any reader bug fail loudly instead of corrupting data.
 *
 * SKIP / LIMIT are applied by fl_route_record() exactly as in the CSV
 * scan; SKIP counts records, matching the serial parser's OFFSET unit.
 * A trailing partial record at EOF is ignored with a WARNING, matching
 * the serial parser's behavior.
 */
static void
fl_reader_scan_fixed(FastloadParams *p, FastloadRouter *router)
{
	size_t		stride = (size_t) p->rec_stride;
	int			fd;
	char	   *pend;
	Size		pend_cap;
	Size		pend_len = 0;
	int64		rec_no = 0;
	bool		eof = false;
	bool		stopped = false;
	int			rounds = 0;
	int			w;

	for (w = 0; w < p->nworkers; w++)
	{
		router->stage[w] = palloc(FL_STAGE_CAP);
		router->stage_len[w] = 0;
	}

	fd = open(p->infile, O_RDONLY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("pg_fastload reader: could not open input file \"%s\": %m",
						p->infile)));

	/* One read chunk plus one carried partial record always fits. */
	pend_cap = FL_READ_CHUNK + stride;
	pend = palloc(pend_cap);

	for (;;)
	{
		int		n = 0;
		Size	i = 0;

		if (!eof)
		{
			n = (int) read(fd, pend + pend_len, FL_READ_CHUNK);
			if (n < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("pg_fastload reader: could not read input file: %m")));
			if (n == 0)
				eof = true;
			else
				pend_len += (Size) n;
		}

		while (pend_len - i >= stride)
		{
			if (fl_route_record(p, router, rec_no, pend + i, (int) stride))
			{
				stopped = true;
				break;
			}
			rec_no++;
			i += stride;
		}

		if (stopped)
			break;

		/* Keep the unconsumed tail for the next round. */
		if (i > 0)
		{
			memmove(pend, pend + i, pend_len - i);
			pend_len -= i;
		}

		if (eof)
			break;

		if ((++rounds & 0x3F) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	if (!stopped && pend_len > 0)
		elog(WARNING, "Ignore %d bytes at the end of file", (int) pend_len);

	/* Flush whatever is left in the staging buffers, then signal EOF. */
	for (w = 0; w < p->nworkers; w++)
		fl_stage_flush(router, w);

	close(fd);
}

/*
 * Reader background worker entry point.  Unlike the parse workers this
 * process needs NO database connection (it only reads the input file and
 * sends into the workers' input queues), so it is registered with
 * BGWORKER_SHMEM_ACCESS only.
 */
void
pg_fastload_reader_main(Datum main_arg)
{
	dsm_segment		   *seg;
	shm_toc			   *toc;
	FastloadShared	   *hdr;
	FastloadParams	   *p;
	FastloadRouter		router;
	PGPROC			   *registrant;
#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time			t_start;
	instr_time			t_end;
#endif
	int					i;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload reader: unable to map DSM segment")));
	toc = shm_toc_attach(FASTLOAD_SHM_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_fastload reader: bad DSM magic")));

	hdr = shm_toc_lookup(toc, FASTLOAD_KEY_HEADER, false);
	p = shm_toc_lookup(toc, FASTLOAD_KEY_PARAMS, false);

	/* Attach every worker's input queue; the reader is the sender. */
	router.out = palloc(sizeof(shm_mq_handle *) * p->nworkers);
	router.stage = palloc(sizeof(char *) * p->nworkers);
	router.stage_len = palloc(sizeof(int) * p->nworkers);
	router.nworkers = p->nworkers;
	router.owner_cur = -1;		/* first advance lands on owner 0 */
	router.route_left = 0;		/* forces the advance on the first record */
	router.peer_gone = false;
	for (i = 0; i < p->nworkers; i++)
	{
		shm_mq	   *mq;

		mq = shm_toc_lookup(toc, FASTLOAD_KEY_INPUT_BASE + i, false);
		shm_mq_set_sender(mq, MyProc);
		router.out[i] = shm_mq_attach(mq, seg, NULL);
	}

	/* Announce readiness so the leader can start the parse workers. */
	SpinLockAcquire(&hdr->mutex);
	hdr->reader_state = FL_WORKER_RUNNING;
	SpinLockRelease(&hdr->mutex);
	registrant = BackendPidGetProc(MyBgworkerEntry->bgw_notify_pid);
	if (registrant != NULL)
		SetLatch(&registrant->procLatch);

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t_start);
#endif

	PG_TRY();
	{
		if (p->pmode == FL_PMODE_FIXED)
			fl_reader_scan_fixed(p, &router);
		else
			fl_reader_scan(p, &router);

#ifdef ENABLE_FASTLOAD_PROFILE
		INSTR_TIME_SET_CURRENT(t_end);
		INSTR_TIME_SUBTRACT(t_end, t_start);
#endif

		/*
		 * Mark DONE before detaching: the workers read reader_state when
		 * their shm_mq_receive reports SHM_MQ_DETACHED, and DONE is what
		 * tells a clean EOF apart from a reader crash.
		 */
		SpinLockAcquire(&hdr->mutex);
		hdr->reader_state = FL_WORKER_DONE;
#ifdef ENABLE_FASTLOAD_PROFILE
		hdr->reader_scan = INSTR_TIME_GET_DOUBLE(t_end);
#endif
		SpinLockRelease(&hdr->mutex);

		/* Release the workers: their receive now reports clean EOF. */
		for (i = 0; i < p->nworkers; i++)
			shm_mq_detach(router.out[i]);
	}
	PG_CATCH();
	{
		/*
		 * Release the workers first (they would otherwise wait on a queue
		 * that will never receive more data), then record the failure and
		 * rethrow so the process exits non-zero.
		 */
		for (i = 0; i < p->nworkers; i++)
			shm_mq_detach(router.out[i]);

		SpinLockAcquire(&hdr->mutex);
		hdr->reader_state = FL_WORKER_FAILED;
		hdr->reader_failed = true;
		snprintf(hdr->reader_error, sizeof(hdr->reader_error),
				 "reader aborted (see server log)");
		SpinLockRelease(&hdr->mutex);

		PG_RE_THROW();
	}
	PG_END_TRY();

	dsm_detach(seg);
	proc_exit(0);
}

/* ------------------------------------------------------------------------
 * Field splitting + tuple formation for one owned record
 * ------------------------------------------------------------------------ */

static void
fl_parse_fields(FastloadWorkerCtx *ctx, char *rec, int len)
{
	FastloadParams *p = ctx->p;
	int			dst = 0;
	int			raw_start = 0;
	int			str_start = 0;
	int			i;
	bool		in_quote = false;

	ctx->nfields = 0;
	if (len + 1 > ctx->field_cap)
	{
		ctx->field_cap = len + 1;
		ctx->field_buf = repalloc(ctx->field_buf, ctx->field_cap);
	}
	ctx->field_buf[0] = '\0';
	ctx->fields[0].str = ctx->field_buf;
	ctx->fields[0].rawlen = 0;
	ctx->fields[0].is_null = false;

	for (i = 0; i < len; i++)
	{
		char	c = rec[i];
		char	nx = (i + 1 < len) ? rec[i + 1] : '\0';

		if (in_quote)
		{
			if (c == p->escape && i + 1 < len &&
				(nx == p->quote || nx == p->escape))
			{
				ctx->field_buf[dst++] = nx;
				i++;
			}
			else if (c == p->quote)
				in_quote = false;
			else
				ctx->field_buf[dst++] = c;	/* CRLF inside quotes preserved */
		}
		else if (c == p->quote)
		{
			in_quote = true;
		}
		else if (c == p->delim)
		{
			int		rawlen = i - raw_start;
			int		fn = ctx->nfields;

			/* Bounds check BEFORE any write into ctx->fields[]. */
			if (fn >= ctx->fields_cap)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("extra data after last expected column")));

			ctx->field_buf[dst] = '\0';
			ctx->fields[fn].str = ctx->field_buf + str_start;
			ctx->fields[fn].rawlen = rawlen;
			/*
			 * Match the original checkFieldIsNull(): the LENGTH is the raw
			 * (pre-dequote) field length, while the CONTENT compared is the
			 * dequoted string.  So a quoted empty field "" (rawlen 2) is an
			 * empty string, while an unquoted empty field (rawlen 0) is NULL.
			 */
			ctx->fields[fn].is_null =
				(rawlen == p->null_len &&
				 memcmp(p->nullstr, ctx->field_buf + str_start,
						p->null_len) == 0);

			ctx->nfields++;
			raw_start = i + 1;
			str_start = ++dst;
			ctx->field_buf[dst] = '\0';
			if (ctx->nfields < ctx->fields_cap)
			{
				ctx->fields[ctx->nfields].str = ctx->field_buf + dst;
				ctx->fields[ctx->nfields].rawlen = 0;
				ctx->fields[ctx->nfields].is_null = false;
			}
		}
		else
			ctx->field_buf[dst++] = c;
	}

	/* closing field */
	{
		int		rawlen = len - raw_start;
		int		fn = ctx->nfields;

		if (fn >= ctx->fields_cap)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("extra data after last expected column")));

		ctx->field_buf[dst] = '\0';
		ctx->fields[fn].str = ctx->field_buf + str_start;
		ctx->fields[fn].rawlen = rawlen;
		ctx->fields[fn].is_null =
			(rawlen == p->null_len &&
			 memcmp(p->nullstr, ctx->field_buf + str_start,
					p->null_len) == 0);
		ctx->nfields++;
	}
}

static void
fl_emit_record(FastloadWorkerCtx *ctx, char *rec, int len)
{
	MemoryContext	oldctx;
	int				i;
	HeapTuple		tuple;
	shm_mq_result	r;
#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time		t0;
	instr_time		t1;
#endif

	/* Fixed-length input: separate parse path (no delimiters/quotes). */
	if (ctx->p->pmode == FL_PMODE_FIXED)
	{
		fl_emit_record_fixed(ctx, rec, len);
		return;
	}

	/*
	 * The reader process has already applied SKIP, LIMIT and chunk-based
	 * ownership before routing this record into our input queue, so
	 * everything that reaches fl_emit_record() must be parsed and emitted.
	 */
	if (ctx->leader_gone)
		return;

	/*
	 * Per-record scratch.  TupleFormerValue() and heap_form_tuple() palloc
	 * here; shm_mq_send() copies the bytes into the queue, so resetting the
	 * context right afterwards is safe.
	 */
	oldctx = MemoryContextSwitchTo(ctx->recctx);

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t0);
#endif

	fl_parse_fields(ctx, rec, len);

	if (ctx->nfields > ctx->maxfields)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("extra data after last expected column")));
	if (ctx->nfields < ctx->minfields)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("missing data for column %d", ctx->nfields + 1),
				 errdetail("only %d columns, required %d",
						   ctx->nfields, ctx->maxfields)));

	for (i = 0; i < ctx->nfields; i++)
	{
		int		index = ctx->former.attnum[i];

		if (!ctx->fields[i].is_null)
		{
			ctx->former.values[index] =
				TupleFormerValue(&ctx->former, ctx->fields[i].str, index);
			ctx->former.isnull[index] = false;
		}
		else
		{
			ctx->former.values[index] = (Datum) 0;
			ctx->former.isnull[index] = true;
		}
	}

	tuple = TupleFormerTuple(&ctx->former);

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_ACCUM_DIFF(ctx->prof_form, t1, t0);
	INSTR_TIME_SET_CURRENT(t0);
#endif

	for (;;)
	{
		r = shm_mq_send(ctx->mqh, tuple->t_len, tuple->t_data, false, true);
		if (r == SHM_MQ_SUCCESS)
		{
			ctx->sent++;
			break;
		}
		if (r == SHM_MQ_DETACHED)
		{
			/* leader stopped early (e.g. LIMIT reached) */
			ctx->leader_gone = true;
			break;
		}
		/* Defensive: nowait=false should never yield WOULD_BLOCK. */
		CHECK_FOR_INTERRUPTS();
	}

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_ACCUM_DIFF(ctx->prof_ipc, t1, t0);
#endif

	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(ctx->recctx);
}

/*
 * FL_PMODE_FIXED (TYPE = BINARY / FIXED): parse one STRIDE-byte record
 * with the serialized COL field descriptors and emit the tuple.
 *
 * This mirrors the serial path (parser_binary.c: ExtractValuesFromFixed):
 * each field is temporarily NUL-terminated at its end so that Read_char()
 * can strlen()/trim it and the NULLIF checks see a bounded string; the
 * byte is restored afterwards.  The record frame may point straight into
 * the input queue's memory (zero-copy fast path), which is read-only for
 * us, so all of this happens on a private scratch copy.
 */
static void
fl_emit_record_fixed(FastloadWorkerCtx *ctx, char *rec, int len)
{
	FastloadParams *p = ctx->p;
	MemoryContext	oldctx;
	char		   *scr;
	int				i;
	HeapTuple		tuple;
	shm_mq_result	r;
#ifdef ENABLE_FASTLOAD_PROFILE
	instr_time		t0;
	instr_time		t1;
#endif

	if (ctx->leader_gone)
		return;

	/* The reader only ever routes whole records; keep the check cheap. */
	if (len != (int) p->rec_stride)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_fastload worker: fixed record length mismatch "
						"(%d != %d bytes)", len, (int) p->rec_stride)));

	scr = ctx->fix_scratch;
	memcpy(scr, rec, (Size) len);
	scr[len] = '\0';

	oldctx = MemoryContextSwitchTo(ctx->recctx);

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t0);
#endif

	for (i = 0; i < ctx->nbfields; i++)
	{
		Field		   *f = &ctx->bfields[i];
		int				j = ctx->former.attnum[i];
		int				nh = f->offset + f->len;
		char			saved;
		Datum			value;
		bool			isnull;

		saved = scr[nh];
		scr[nh] = '\0';
		f->in = scr + f->offset;

		value = f->read(&ctx->former, f->in, f, j, &isnull);

		scr[nh] = saved;

		ctx->former.values[j] = value;
		ctx->former.isnull[j] = isnull;
	}

	tuple = TupleFormerTuple(&ctx->former);

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_ACCUM_DIFF(ctx->prof_form, t1, t0);
	INSTR_TIME_SET_CURRENT(t0);
#endif

	for (;;)
	{
		r = shm_mq_send(ctx->mqh, tuple->t_len, tuple->t_data, false, true);
		if (r == SHM_MQ_SUCCESS)
		{
			ctx->sent++;
			break;
		}
		if (r == SHM_MQ_DETACHED)
		{
			/* leader stopped early (e.g. LIMIT reached) */
			ctx->leader_gone = true;
			break;
		}
		/* Defensive: nowait=false should never yield WOULD_BLOCK. */
		CHECK_FOR_INTERRUPTS();
	}

#ifdef ENABLE_FASTLOAD_PROFILE
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_ACCUM_DIFF(ctx->prof_ipc, t1, t0);
#endif

	MemoryContextSwitchTo(oldctx);
	MemoryContextReset(ctx->recctx);
}
