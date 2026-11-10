# Changes Relative to pg_bulkload 3.1.23

> Upstream: `https://github.com/ossc-db/pg_bulkload`, HEAD `798e0f2`, version 3.1.23
> Author: yuanyuansong, 2026
>
> This document records only **adopted** changes, arranged by round; approaches that were
> evaluated and not adopted are collected in the "Rejected Approaches" section of
> [OPTIMIZATION.md](OPTIMIZATION.md), and are not repeated round-by-round here.

---

## 1. Global Renaming (108 files + 18 file/directory names)

| Original name | New name |
|---|---|
| Project / extension / shared library `pg_bulkload` | `pg_fastload` |
| SQL schema `pgbulkload` | `pgfastload` |
| SQL function `pgbulkload.pg_bulkload()` | `pgfastload.pg_fastload()` |
| `lib/pg_bulkload.c` / `.sql.in` / `.control` / `--3.1.23.sql` | `pg_fastload.c` / `.sql.in` / `.control` / `--1.0.0.sql` |
| `include/pg_bulkload.h` | `include/pg_fastload.h` |
| Macros `PG_BULKLOAD_*` / `BULKLOAD_*` / `FASTLOAD_PROFILE*` | `PG_FASTLOAD_*` / `FASTLOAD_*` |
| `bin/pg_bulkload.c` / client executable name | `bin/pg_fastload.c` / `pg_fastload` |
| `SPECS/pg_bulkload-pg*.spec` / `msvc/*` / `docs/pg_bulkload*.html` | corresponding `pg_fastload*` |

Version number reset to `1.0.0` (starting point of the standalone project).

**Unchanged**: The original `COPYRIGHT` file (BSD-3-Clause, NTT, Inc. 2008-2026) is preserved
byte-for-byte; the `Copyright (c) ... NTT, Inc.` line at the top of every inherited source file
is preserved.

---

## 2. New Files

| File | Purpose |
|---|---|
| `LICENSE` | BSD-3-Clause license + original copyright notice + parallelization contribution copyright |
| `NOTICE` | Source, version, license, attribution, retained and modified items |
| `README.md` (English) / `README.zh-CN.md` (Chinese) | Project overview: positioning, differences vs. pg_bulkload, architecture, quick start, limitations |
| `docs/USER_MANUAL.md` (English) / `docs/USER_MANUAL.zh-CN.md` (Chinese) | User manual: installation, full keyword table, usage, parallel tuning, FAQ |
| `docs/ARCHITECTURE.md` (English) / `docs/ARCHITECTURE.zh-CN.md` (Chinese) | Overall architecture, component responsibilities, data flow, IPC protocol, downgrade design |
| `docs/CHANGES.md` | This file |
| `docs/BENCHMARK.md` | Performance benchmarks |
| `docs/VERIFICATION.md` | Verification report |
| `docs/OPTIMIZATION.md` | Optimization log and rejected approaches |
| `docs/img/architecture.svg` (English) / `docs/img/architecture.zh-CN.svg` (Chinese) | Architecture diagram |
| `include/fastload_parallel.h` | DSM shared protocol: `FastloadParams` / `FastloadColumn` / `FastloadShared` / `FastloadWorkerId`, shm_toc key, queue capacity, magic |
| `lib/fastload_parallel.c` | **leader side**: `FastloadDefaultWorkers()`, `FastloadParallelSupported()`, `FastloadParallelRun()`; DSM/shm_toc/shm_mq creation, background worker registration, round-robin consumption, writable copy before `WriterInsert()` |
| `lib/fastload_worker.c` | **reader / worker side**: `pg_fastload_reader_main()`, `pg_fastload_worker_main()`, CSV quote-aware boundary state machine, fixed-length STRIDE slicing, chunked round-robin routing, field splitting, reusing `TupleFormer` to form tuples, `shm_mq_send` |

---

## 3. Modified Files

| File | Changes |
|---|---|
| `include/reader.h` | `struct Reader` gains `parallel`, `parallel_workers`, `is_csv`, `is_fixed`, `parallel_skip`, `parallel_delim/quote/escape`, `parallel_null`, `parallel_has_filter`, `parallel_has_fnn` |
| `lib/reader.c` | ① `ReaderCreate()` records `is_csv` (`TYPE=CSV`) and `is_fixed` (`TYPE=BINARY/FIXED`); ② `ReaderInit()` uses `FastloadDefaultWorkers()` to compute the default worker count; ③ `ReaderParam()` adds `PARALLEL` / `PARALLEL_WORKERS`; ④ mirrors `SKIP/OFFSET`, `DELIMITER`, `QUOTE`, `ESCAPE`, `NULL`, `FILTER`, `FORCE_NOT_NULL` onto Reader while **still forwarding them to `ParserParam()`**, so single-process semantics are unchanged |
| `lib/pg_fastload.c` | The main loop is changed to `if (FastloadParallelSupported(rd, wt)) FastloadParallelRun(...); else original loop`; the `<PARALLEL>` profile group gains a new `READ` segment; other statistics/logging/cleanup logic is untouched |
| `lib/binary.c` + `include/binary.h` | New `COL` descriptor serialization: `BinaryParserGetMeta`, `BinarySerializedSize`, `BinarySerializeFields`, `BinaryDeserializeFields` (for fixed-length parallelism; function pointers do not cross processes) |
| `lib/parser_binary.c` | Exports `BinaryParserGetMeta` (nfield / rec_len / fields) |
| `lib/pg_btree.c` | `SpoolerInsert()` returns early when `ri_NumIndices <= 0`, eliminating per-tuple no-op overhead for index-less tables |
| `include/pg_profile.h` | New parallel-stage probes: `prof_par_setup/wait/copy/write/read`, `prof_par_scan/form/ipc` |
| `lib/Makefile` | `SRCS` adds `fastload_parallel.c`, `fastload_worker.c`; the rest (including `-lpthread`, `pgut/pgut-ipc.c`) is retained |
| `lib/exports.txt` | Adds exported symbol `pg_fastload_worker_main` (the background worker loads it by symbol name via `load_external_function`) |

---

## 4. Removed / Discarded

- **The experimental multithreaded CSV prototype has been fully discarded** (including its parser
  source files and build entries): parallel capability is not exposed via a new `TYPE`; instead,
  when `PARALLEL=ON` it is auto-enabled by `TYPE=CSV / BINARY / FIXED`. `ReaderCreate()`'s type list
  is identical to upstream: `BINARY / FIXED / CSV / TUPLE / FUNCTION`.
- **Upstream HTML manual** (`docs/index.html`, `pg_fastload.html`, `pg_timestamp.html`, `style.css`,
  and the Japanese `*_ja.html` variants) removed: its performance data dates from the
  PostgreSQL 9.3 era, its content is fully covered by `docs/USER_MANUAL*.md`, and it is
  inconsistent with the actual implementation; retaining it would be misleading.

---

## 5. Completely Unchanged Modules

`lib/parser_csv.c`, `lib/parser_tuple.c`, `lib/parser_function.c`, `lib/source.c`,
`lib/writer*.c` (including `writer_parallel.c`), `lib/logger.c`, `lib/pg_strutil.c`, `bin/*` (renamed only),
`util/*`, `SPECS/*`, `msvc/*`, `lib/pgut/*`.

This guarantees:

- All original control-file keywords and semantics are unchanged;
- The three write modes `WRITER = DIRECT / BUFFERED / PARALLEL` are unchanged;
- `badfile` / per-row error tolerance / `SKIP` / `LIMIT` / `ENCODING` / `FILTER` / `FORCE_NOT_NULL` are unchanged on the single-process path;
- Upstream's `pgut-ipc.c`, required by the PARALLEL writer, remains in the build.

---

## 6. Summary of New Behavior

| Control file | Behavior |
|---|---|
| `PARALLEL = OFF` (default) | Identical to pg_bulkload |
| `PARALLEL = ON`, `TYPE = CSV / BINARY / FIXED` | Multi-process parallelism ①②③; ④ the main backend runs serially |
| `PARALLEL = ON`, `TUPLE / FUNCTION` | Auto-downgraded to single process (INFO message) |
| `PARALLEL = ON` + `FILTER` / `FORCE_NOT_NULL` / dropped columns / transcoding / NOT NULL / `CHECK_CONSTRAINTS` / `PARSE_ERRORS` / `STDIN` / `MULTI_PROCESS` | Auto-downgraded to single process (INFO message; full list in USER_MANUAL §10.4) |
| `PARALLEL_WORKERS = n` | Explicitly specifies 1..64 |
| When `PARALLEL_WORKERS` is not specified | Online CPU core count − 1, clamped to 1..64 |

---

## 7. Compliance Notes

- The original `COPYRIGHT` text is retained (required by BSD-3-Clause clauses 1 and 2).
- Every inherited source file retains its NTT copyright line; newly added/modified files
  additionally carry `Copyright (c) 2026, yuanyuansong`.
- It is explicitly stated that this is a derivative implementation based on pg_bulkload 3.1.23,
  follows the original license, and does not use the NTT name for endorsement (clause 3 requirement).
- Binary distributions must reproduce the copyright notice and disclaimer in accompanying
  documentation; `LICENSE` / `NOTICE` / `COPYRIGHT` are provided with the source.

---

## 8. First-Round Integration Fixes

| Problem | Root cause | Fix |
|---|---|---|
| `PARALLEL=ON` silently downgraded to single process | `rd->checker.tchecker != NULL` was an always-true condition: every writer unconditionally calls `CreateTupleChecker()`; for CSV, `CoercionCheckerTuple()` returns the tuple as-is after deciding `NO_COERCION`, which is equivalent to the worker's `TupleFormerTuple()` output | Removed that condition from `FastloadParallelSupported()` and documented in comments and docs that it must not be re-added |
| Small files occasionally hit `worker N failed to attach (state=2)` | A worker finished and detached before the main backend, so `shm_mq_wait_for_attach()` returned `SHM_MQ_DETACHED`; `state=2` is actually `FL_WORKER_DONE` | The orchestration layer tolerates `DETACHED + FL_WORKER_DONE`, reporting an error only for workers that never reached DONE; native PG shm_mq guarantees the remaining messages can still be drained |
| `SKIP` / `LIMIT` semantics inconsistent with upstream (under interleaved sharding, "the first N rows to arrive" is non-deterministic) | Workers previously only took modulo of record number, without bounding the global record window | Workers clip to the `[skip, skip+limit)` window on their side, subtracting before comparing to avoid overflow |
| Parallel division of labor was not observable | Missing per-worker output statistics | `FastloadParallelRun()` prints `worker #i produced N rows` after normal completion |

---

## 9. P0 Performance Optimizations (Round 2, 2026-11-08)

Basis: 1 GiB TPC-H lineitem benchmark + `USE_PROFILE=1` stage timing (conclusions such as
`READER.PARSER` accounting for 78.6% of the original runtime, the N× full scan of the parallel
path accounting for 65% of the auto runtime, and `WRITER.TOAST` being mis-accumulated to 15.39 s).

| # | Bottleneck | Change | Effect (1 GiB, 16 vCPU) |
|---|---|---|---|
| P0-1 | `PARALLEL_WORKERS=1` still created DSM + background worker + shm_mq, taking 16.2 s (2× the 8.2 s serial time) | `FastloadParallelSupported()` returns false and emits INFO when the effective worker count ≤ 1, falling back to the original single-process CSV loop | w=1 **16.215 → 8.070 s (2.01×)**, equivalent to `PARALLEL=OFF` |
| P0-2 | The worker called `fl_append_rec()` per byte to copy each record (including many non-owned records), then re-split the whole record a second time | The record-boundary state machine was changed to run-based contiguous-range `memcpy`; non-owned records are skipped with zero copy; owned records are copied in one pass per contiguous segment. Quote/escape/CRLF/cross-block/no-trailing-newline semantics are unchanged | w=2 **7.760 → 6.820 s (1.14×)**, w=4 **3.270 → 2.950 s (1.11×)**, w=8 **2.530 → 2.320 s (1.09×)** |
| P0-3 | The parallel path had no profile probes: every GLOBAL stage read 0, and the leader's wait time was mis-accumulated into `WRITER.TOAST` | Added a `GLOBAL.PARALLEL` total segment + `<PARALLEL>` (SETUP/WAIT/COPY/WRITE) + `<WORKER>` (SCAN/FORM/IPC, workers publish local timings via DSM); inner PUSH/POP around `WriterInsert` | GLOBAL total ≈ elapsed; `WRITER.TOAST` returns to the normal 0.12~0.14 s range |

Verification: release + debug dual build rc=0; `fastload_verify.py` **90/90 PASS**; the 1 GiB
content digests (`count||sum(hash_record_extended)`) for all 7 configurations are identical.

---

## 10. Single-Reader, Multi-Worker Parallel Architecture (Round 3, 2026-11-08)

Basis: the structural bottleneck of the first version—"each worker independently `open()`s and
fully scans the CSV"—where I/O ×N and N duplicate boundary scans meant total reads ≈ N × file
size when the file exceeds the page cache (cold cache measured 6.49× slower than serial).

**Architecture**: a single reader background worker (no DB connection) scans the CSV in a single
pass and distributes records to N parsing workers via chunked round-robin; workers only parse
their owned records and send them back to the leader via shm_mq. File I/O drops from ×N to ×1.
The reader is handle[0] and workers are handle[1..N]; `max_worker_processes ≥ N+1`.

Three empirically observed bottlenecks and their fixes (each fix confirmed by profile data):

| # | Bottleneck (profile evidence) | Change | Effect (1 GiB, w=2) |
|---|---|---|---|
| C-1 | The reader's per-record `rec_no % N` interleaved routing caused head-of-line blocking: a single full ring stalled all rings (reader blocked 9.205 s / 8.49M waits) | Changed to **byte-based chunked round-robin**: owner switches every ~512 KiB of contiguous records (`FL_ROUTE_BYTES`); routed byte count only counts records within the window | Takes effect combined with C-2/C-3 |
| C-2 | The worker fetched the ring per record: 2 spinlocks + 2 small memcpys per record (15 million lock operations, contending with the reader), worker SCAN 5.2 s | **Bulk ring fetch** on the worker side: pull the whole block within a single lock, switch frames without locking; oversized records fall back to streaming reads | SCAN 5.2 → 0.32 s |
| C-3 | A 1 MiB ring buffered only 2 routing blocks of 512 KiB (≈6 ms of worker work): while the reader is blocked by one ring, all other rings starve for the whole window (worker idle ~45%) | Queue capacity **1 MiB → 8 MiB**, return queue 1 → 2 MiB; DSM scales with the actual worker count (15 workers ≈ 144 MiB) | **w2 10.59 → 3.46 s (3.06×)**, w4 5.13 → 1.97 s, w8 2.34 → 1.95 s |

Final 1 GiB warm-cache comparison (P0 baseline → single-reader, multi-worker): w2 **6.82 → 3.46 s
(1.97×)**, w4 **2.95 → 1.97 s (1.50×)**, w8 2.32 → 1.95 s (1.19×), w15 2.32 → 2.01 s,
auto 2.34 → 1.99 s (1.18×); fl_off / w1 are equivalent to P0 (zero changes to the serial path).
At high concurrency the leader's serial `WriterInsert` is already saturated (WRITE floor ≈ 1.25~1.36 s).

Verification: release + debug dual build rc=0; `fastload_verify.py` **90/90 PASS**;
content digests for all 7 configurations match; after re-testing the production `.so` was
restored to the non-profile build.

---

## 11. Unified Kernel shm_mq Transport + Large-Block Cold-Cache I/O (Round 4, 2026-11-09)

Basis: stage-timing analysis after the single-reader, multi-worker architecture was finalized—
reader→worker still went through a hand-rolled SPSC byte ring (spinlock + head/tail + 100 µs
backoff, ~200 lines of hand-written synchronization code), forming two transport implementations
alongside the kernel shm_mq in the worker→leader direction; in cold-cache scenarios the reader's
64 KiB small-block reads + 64 KiB small-block sends produced ~33,000 blocking syscalls, amplified
into minute-scale regressions on this virtualized host.

| # | Item | Change | Effect |
|---|---|---|---|
| S-1 | Replaced the hand-rolled SPSC ring for reader→worker with the kernel shm_mq | Removed `FastloadRing` (spinlock/head/tail/backoff/count control); DSM now allocates one 8 MiB shm_mq per worker; the reader sends framed messages via `shm_mq_send` and the worker receives via `shm_mq_receive` (zero-copy fast path for whole-frame same-message, reassembly across messages); EOF semantics = the reader sets `reader_state=DONE` before `shm_mq_detach`, and the worker checks the state upon seeing `SHM_MQ_DETACHED` to distinguish clean completion from a crash | Net removal of ~200 lines of hand-written synchronization code; transport implementation unified with the return direction on kernel primitives; warm cache w2 3.46→3.33 s, w15 2.01→1.89 s, auto 1.99→1.88 s |
| S-2 | Magnified latency of cold-cache small-block syscalls | `FL_READ_CHUNK` 64 KiB → **1 MiB** (16386 → 1026 reads per load), `FL_STAGE_CAP` 64 KiB → **512 KiB** (17193 → 2148 sends); removed `FL_DRAIN_CAP` (worker side now batches naturally at shm_mq frame boundaries) | Blocking syscalls reduced ~15×; eliminated the cold-cache **systematic** regression (in good windows w2 3.0 / w4 1.8 / auto 1.5 s, beating the 7.1 s serial time, up to 4.9×); the host's occasional stall windows (vmstat 0 steal / 0 iowait / 100% idle) are a hypervisor-layer phenomenon |

Files involved: `lib/fastload_worker.c` (reader/worker transport layer rewrite, FastloadRouter),
`lib/fastload_parallel.c` (DSM layout, profile summary drops ring fields),
`include/fastload_parallel.h` (removed `FastloadRing`, `FASTLOAD_KEY_RING_BASE`→`FASTLOAD_KEY_INPUT_BASE`,
`FASTLOAD_RING_SIZE`→`FASTLOAD_INPUT_MQ_SIZE`).

Verification: release + debug dual build rc=0; `fastload_verify.py` **90/90 PASS**; the warm-cache
content digests `7375042|9258128314029119699254` for 7 configurations match GOLDEN; after re-testing
the production `.so` was restored to the non-profile build.

---

## 12. BINARY / FIXED Parallelization (Round 5, P-1, 2026-11-09)

Background: the parallel path originally supported only `TYPE=CSV` (guarded by `rd->is_csv` in
`FastloadParallelSupported`). This round wires `TYPE=BINARY` and `TYPE=FIXED` (both mapped to the
same fixed-length parser `CreateBinaryParser`, COL descriptor + STRIDE, in `reader.c`) into the
single-reader, multi-worker parallel pipeline.

| Component | Change |
|---|---|
| `include/reader.h` / `lib/reader.c` | `Reader` gains `is_fixed` (idx 0/1 → both BINARY/FIXED are fixed-length) |
| `lib/binary.c` + `include/binary.h` | New COL field-descriptor serialization: `BinaryParserGetMeta` (exports nfield/rec_len/fields), `BinarySerializedSize`/`BinarySerializeFields` (the leader writes kind/offset/len/nullif into the DSM blob), `BinaryDeserializeFields` (the worker rebuilds read function pointers from the static `TYPES[]` table by kind and allocates a work buffer; **function pointers are not passed across processes**) |
| `include/fastload_parallel.h` | `FastloadParams` gains `pmode` (FL_PMODE_CSV/FL_PMODE_FIXED), `nbinfields`, `rec_stride`; the DSM parameter block appends a fixed-length blob after `cols[]` |
| `lib/fastload_parallel.c` | The guard now permits `is_fixed`; fixed-length-only fallbacks: no COL, rec_len≤0, or NULLIF pattern longer than 64 bytes → serial; `fastload_fill_params` serializes the blob |
| `lib/fastload_worker.c` | ① reader side `fl_reader_scan_fixed`: slices records directly by STRIDE (no quote state machine); frame protocol/SKIP/LIMIT/chunked round-robin identical to CSV; trailing residual bytes WARNING-ignored (aligned with serial). ② worker side `fl_emit_record_fixed`: mirrors `ExtractValuesFromFixed`—scratch copy (queue memory is read-only) + temporary NUL termination at field end + sequential `field->read()` calls; STRIDE validation to prevent errors |

Verification:

- New `fastload_verify_fixed.py` **44/44 PASS**: plain CHAR text, BINARY/FIXED aliases, mixed
  text + binary numerics (INTEGER/BIGINT/DOUBLE native bytes), NULLIF (text + numeric),
  STRIDE greater than the sum of fields (tail padding), trailing residual bytes, SKIP+LIMIT,
  PRESERVE_BLANKS+VARCHAR, empty file, w=1/2/3/4/auto, FILTER downgrade, 10000 rows × 4 worker sharding.
- CSV regression `fastload_verify.py` **90/90 PASS** (no regressions).
- 1 GiB fixed-length benchmark (10,737,418 rows × 100 B, 7 configurations × 3 warm-cache rounds):
  content digest `10737418|12495206446709334590242`, ALL_MATCH=True across the seven configurations;
  in good windows fl_off **1.62 s**, w2 1.06 s, w4 0.94 s, w8 **0.89 s**, w15 0.90 s, auto 0.91 s
  (up to **1.82×** vs serial).

Known limitations (fixed-length parallel): input encoding must match the database encoding;
FILTER/constraint checks/PARSE_BADFILE downgrade to serial; NULLIF pattern length ≤ 64 bytes;
records must be complete STRIDEs (trailing residuals ignored).

---

## 13. Early Return for Index-Less Spool (Round 6, W-3a, 2026-11-09)

Measure first, then decide: even with **no index**, 1 GiB lineitem still spent 0.28 s in
`WRITER.INDEX`, and profiling showed this was pure no-op overhead—`SpoolerInsert()` executes
`ExecStoreHeapTuple()` + `GetPerTupleExprContext()` + `IndexSpoolInsert()` per tuple regardless of
whether the target table has an index (when `ri_NumIndices == 0` the loop body does nothing).

Change (`lib/pg_btree.c`): at the top of `SpoolerInsert()`, resolve `relinfo` early and `return`
directly when `ri_NumIndices <= 0`. Three effective lines of code, no semantic change (the
indexed-path logic is completely unchanged).

| Config | Previous | This round | Improvement |
|---|---|---|---|
| fl_off | 8.24 s | **7.91 s** | 1.04× |
| fl_w1 | 8.23 s | **7.86 s** | 1.05× |
| fl_w2 | 3.32 s | 3.28 s | 1.01× |
| fl_w4 | 1.96 s | 1.91 s | 1.03× |
| fl_w8 | 1.88 s | **1.75 s** | 1.07× |
| fl_w15 | 1.91 s | **1.73 s** | 1.10× |
| fl_auto | 1.87 s | **1.75 s** | 1.07× |

Verification: `fastload_verify.py` **90/90 PASS**, `fastload_verify_fixed.py` **44/44 PASS**
(including unique-index/UPSERT/duplicate-handling cases, zero regressions); release + debug dual
build rc=0.

---

## 14. Code Cleanup (2026-11-09, code review)

Performed a maintainability-focused review of the parallel subsystem, **no behavioral changes**:

| Item | Change |
|---|---|
| Dead fields | Removed `FastloadShared.workers_ready` (never read); removed `FastloadWorkerId.nworkers` (worker count is always read from the DSM parameter block) |
| Duplicated logic | The default worker count had two implementations (`sysconf()` inlined in `ReaderInit()` + in the orchestration layer); unified into a single `FastloadDefaultWorkers()` definition, exported in `include/fastload_parallel.h` |
| Unused dependency | Removed `#include <unistd.h>` from `lib/reader.c` (no longer needed after `sysconf()` was removed) |
| Undocumented alias | Removed the `PARSE_WORKERS` alias, keeping only `PARALLEL_WORKERS` |
| Memory | `reader.c` now `pfree`s the old value before reassigning `parallel_null` |
| Concurrency correctness | In the `SHM_MQ_DETACHED` branch, `worker_error[i]` is copied to a local buffer before releasing the spinlock, avoiding passing a shared-memory pointer out of the critical section |
| Combination guard | `FastloadParallelSupported()` adds `MULTI_PROCESS` / `WRITER=PARALLEL` checks: when specified together with `PARALLEL=ON` they do not stack (falls back to a serial reader and prints INFO), avoiding nested fan-out of two process models |
| Comments | Fixed the misplaced SKIP/LIMIT routing comments in `fastload_worker.c`; moved `t_start/t_end` under `#ifdef ENABLE_FASTLOAD_PROFILE` |
| Build artifact | Removed the legacy `lib/exports.list` in the repo (generated by Makefile from `exports.txt`, already in `.gitignore`) |

Regression: release + debug dual build rc=0 (only two upstream declaration-style warnings for
`TYPES`/`ALIASES`); CSV **90/90 PASS**, fixed-length **44/44 PASS**, `MULTI_PROCESS` dedicated
probe **8/8 PASS**; 1 GiB 7-configuration content digest
`7375042|9258128314029119699254` ALL_MATCH=True. Re-test (median of 3 rounds): fl_off 7.99 s,
fl_w1 7.97 s, fl_w2 3.52 s, fl_w4 1.92 s, fl_w8 1.76 s, fl_w15 1.74 s, fl_auto 1.76 s, within the
±5% run-to-run fluctuation vs the previous round.

---

## 15. Documentation System Restructuring (2026-11-09)

| Change | Description |
|---|---|
| New README (now in English and Chinese) | `README.md` / `README.zh-CN.md`: positioning, three difference tables vs. pg_bulkload (design approach / features / usage), architecture diagram and component responsibilities, quick start, downgrade list, limitations |
| New user manual (now in English and Chinese) | `docs/USER_MANUAL.md` / `docs/USER_MANUAL.zh-CN.md`: installation, control-file syntax, full keyword table, SQL/CLI, input formats, parallel tuning, error handling, diagnostics, FAQ, migration and uninstallation |
| Architecture docs (English and Chinese) | `docs/ARCHITECTURE.md` / `docs/ARCHITECTURE.zh-CN.md` merge and rewrite the original `PARALLEL_DESIGN.md` content, adding an architecture diagram and component-responsibility table; `PARALLEL_DESIGN.md` removed |
| Architecture diagram | New `docs/img/architecture.svg` (English) and `docs/img/architecture.zh-CN.svg` (Chinese), consistent with the current single-reader, multi-worker implementation |
| Removed Japanese and legacy HTML | Removed `index_ja.html`, `pg_fastload-ja.html`, `pg_timestamp-ja.html`, and the upstream English HTML manual (see §4) |
| Documentation consolidation | Optimization records consolidated into `docs/OPTIMIZATION.md`: the body records adopted changes round-by-round, and evaluated-but-rejected approaches are collected in the final chapter, not appearing round-by-round in this document |

---

## 16. Multi-Version Adaptation for PG 15 / 16 / 17 / 19 (2026-11-10)

Goal: a single source tree compiles and passes the same functional acceptance on 15.19 / 16.15 /
17.11 / 18.6 / 19rc1. All version differences are folded into `#if PG_VERSION_NUM`; no
business-logic forks.

| File | Change |
|---|---|
| `lib/fastload_worker.c` | `populate_compact_attribute()` is called only for ≥18 (new in 18; earlier versions compute on demand) |
| `lib/fastload_parallel.c` | `get_database_name()` header selected by version (moved to `utils/lsyscache.h` in 19); added `storage/proc.h` (`MyProc`) |
| `lib/writer_parallel.c` | Same as above, `get_database_name()` header selected by version |
| `lib/pgut/pgut-be.h` | New `FastloadBackendNumber` / `FastloadInvalidBackend`: for ≥17 use `MyProcNumber` / `INVALID_PROC_NUMBER` (19 removed `MyBackendId`), earlier versions use `MyBackendId` / `InvalidBackendId` |
| `lib/pg_btree.c` | Temp-relation lookup switched to the above macros (also fixing the 17 branch's misuse of `MyBackendType` / `InvalidCommandId`); the index-build branch hooks into `nbtsort-19.c` |
| `lib/writer_direct.c` | Same macros; for ≥19 `PageAddItem()` passes `const void *` directly (`Item` typedef removed), `PageSetChecksumInplace()` → `PageSetChecksum()` |
| `lib/pg_strutil.c` | For ≥19 `FuncnameGetCandidates()` gained an `int *fgc_flags` out-parameter and **unconditionally dereferences** it, so we pass a one-shot `int[1]` compound literal (passing `NULL` caused a segfault in testing) |
| `lib/nbtree/nbtsort-19.c` | New: PG19's `nbtsort.c` (`BTSpool` was privatized, `palloc*_object`, etc.), differing from the 18 version by 59 lines |
| `bin/pgut/pgut-list.h` | For ≥19 added `typedef uint64_t Datum;` (`Datum` moved from `c.h` into `postgres.h`, unavailable to the frontend) |
| `bin/recovery.c` | For ≥19 `pd_lsn` uses `PageXLogRecPtrGet()` (`PageXLogRecPtr` became a single 64-bit value) |
| `util/fl_ver_test.sh` | New: single-major-version functional acceptance script (9 assertion classes), with overridable `PG_PREFIX` / `SRC` / `ROWS` |
| `docs/PORTING.md` (English) / `docs/PORTING.zh-CN.md` (Chinese) | New: support matrix, 10 kernel differences and their handling, acceptance method and results |

Acceptance (run once per version, all PASS): CSV serial/parallel 5-configuration digests match,
fixed-length 3-configuration digests match, `FILTER` downgrade content matches and `LOGFILE`
carries the `PARALLEL disabled (FILTER` message, parse errors stop at the 1st bad record by
default while `PARSE_ERRORS=2` keeps 2 good records, CLI exit code 0 with 200,000 rows loaded, no
PANIC in the server log.

Incidental fix: the user manual's `PARSE_ERRORS` description originally read "loading fails if
exceeded", but in fact it **stops reading and returns normally once exceeded** (loaded rows are
kept, log shows `Maximum parse error count exceeded`); both the original Chinese and English
versions were corrected.

---

## 17. Upstream Parity Audit and Fixes (2026-11-10)

Method: upstream `pg_bulkload` 3.1.23 was built against the **same** server and used as an oracle.
Three differential suites feed identical input to `pgbulkload.pg_bulkload()` and to
`pgfastload.pg_fastload()` in three configurations (`PARALLEL=OFF`, `PARALLEL=ON` w=2, w=4) and
compare, per case: the SRF result row (`skip`/`count`/`parse_errors`/duplicate counters), the
content digest `count(*) || coalesce(sum(hash_record_extended(t,0)),0)`, and the severity of the
emitted messages. 75 cases were covered: quoting/escaping/CRLF/CR/blank lines/missing and extra
fields/unterminated quotes, type-conversion failures, `SKIP`+`LIMIT` windows, duplicates on unique
indexes and primary keys (including `ON_DUPLICATE_KEEP`), `WRITER=BUFFERED`/`PARALLEL`,
`MULTI_PROCESS`, `FORCE_NOT_NULL`, `NULL`/`ENCODING`/custom `DELIMITER`+`QUOTE`+`ESCAPE`,
`PARSE_ERRORS` 0/2, 200k-row and 1 MiB-chunk-boundary inputs, and fixed-length loads with
`SKIP`/`LIMIT`/a trailing partial record.

Four real defects were found and fixed; the serial path was correct in all 75 cases before the audit.

| # | Defect | Root cause | Fix |
|---|---|---|---|
| 1 | A rejected record killed the worker (and with it the statement) instead of being counted; `count`/`parse_errors` were never reported | the parallel reader raised `unterminated CSV quoted field` itself, and worker parse/convert errors were never absorbed (the serial path absorbs them in `ReaderNext()`) | worker-side `PG_TRY`/`PG_CATCH` (`fl_emit_record_safe`), error published to the control block (`fl_record_parse_error`) together with record number and column, record appended to `PARSE_BADFILE`, leader reports it; the truncated final record is now routed to the worker instead of failing the reader |
| 2 | `SKIP` behaved differently: `SKIP=1` skipped one *record* on the parallel path but one *line* upstream | `SKIP` was applied as a record offset in `fl_route_record()`; upstream's preamble counts physical lines and does not honour quotes | new `fl_skip_lines()` walks raw bytes before the record scan (`LF`, `CRLF`, lone `CR`), `fl_route_record()` keeps only `LIMIT` |
| 3 | After the first rejected record the parallel path kept loading later records: 100k-row oracle vs **156k / 175k / 194k** rows for w=2/4/8, varying run to run | records for a rejected line are already queued in other workers; "stop reading, keep what was loaded" cannot be reproduced in parallel without buffering everything | the parallel contract is now **atomic**: same WARNING lines, then `ERROR` and full rollback. Never loads a record the oracle would not load, and no timing dependence (documented in USER_MANUAL §11) |
| 4 | Heap overflow when a skipped line exceeded the 1 MiB read chunk | `fl_skip_lines()` kept appending into a fixed `FL_READ_CHUNK + 2` buffer (the record scan drains its carry every round, a skipped line does not); the first scan round after a skip had the same assumption | buffer grown with `repalloc` in both places |

Also: the 8-byte record frame now carries the record number (`hdr8[1]`), so leader-side messages can
name the failing input record, as the serial path does.

The audit itself is reproducible: `util/fl_diff_test.py` is a self-contained version of the harness
(44 cases × 4 configurations, no VM or SSH dependency) that needs nothing but both extensions
installed in one cluster.

Result after the fixes: **0 mismatches** — 75 cases × 3 configurations = 225 comparisons, of which 30
are the documented atomic aborts (parallel only; the `PARALLEL=OFF` configuration matches the oracle
exactly in every case). The whole suite was repeated on **15.19 / 16.15 / 17.11 / 18.6** with
identical results (900 comparisons in total), and the per-major functional acceptance
(`util/fl_ver_test.sh`) passes on 15.19 / 16.15 / 17.11 / 19rc1, as do `fastload_verify.py`
**90/90** and `fastload_verify_fixed.py` **44/44**. PG19 has no oracle: upstream 3.1.23 does not
compile there (it hits the `Datum` relocation that our `bin/pgut/pgut-list.h` handles), so 19rc1 is
covered by functional acceptance and serial-vs-parallel self-consistency only.

---

## 18. Load progress: the `pg_fastload_progress` view (2026-11-10)

`pg_fastload()` is a single SQL statement, so a multi-minute load over a large file produced no output
at all until it returned — and asking that same session to print progress does not help, because it is
the busy one: its client is blocked on a statement that has not finished yet. Progress is therefore
**published**, and read from elsewhere:

```sql
-- session 1: busy for the next half hour
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/lineitem.csv', 'OUTPUT=public.lineitem', 'PARALLEL=ON']);

-- session 2, a monitoring agent, or any other database of the same instance
SELECT * FROM pgfastload.pg_fastload_progress;
```

```
 pid  | datname |    relname      | input_type |  mode    | state   | elapsed_seconds |  rows   | bytes_read | total_bytes
------+---------+-----------------+------------+----------+---------+-----------------+---------+------------+-------------
 4121 | tpcc    | public.lineitem | CSV        | parallel | running |            3.42 | 3687521 |  536870912 |  1073741824
```

The view has 19 columns: `pid`, `backend_start`, `started`, `datname`, `relname`, `infile`,
`input_type`, `mode`, `state`, `elapsed_seconds`, `rows`, `bytes_read`, `total_bytes`, `total_rows`,
`percent`, `bytes_per_second`, `rows_per_second`, `eta_seconds`, `interval_ms`.

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `PROGRESS` | `ON` / `OFF` | `ON` | publish a status record for this load |
| `PROGRESS_INTERVAL` | `100`..`3600000` ms | `1000` | minimum gap between two records; **implies `PROGRESS=ON`** |
| `PROGRESS_NOTICE` | `ON` / `OFF` | `OFF` | also stream a `NOTICE` line to the loading client and `LOGFILE` |

```
NOTICE:  pg_fastload progress: start, input 1.00 GiB, interval 1000 ms
NOTICE:  pg_fastload progress: 12.6% | rows 927,744 | 129.0 MiB / 1.00 GiB | 122.8 MiB/s | 883.1k rows/s | elapsed 00:00:01 | ETA 00:00:07
NOTICE:  pg_fastload progress: 100.0% | rows 7,375,042 | 1.00 GiB / 1.00 GiB | 149.0 MiB/s | 1.07M rows/s | elapsed 00:00:06 | done
```

| File | Change |
|---|---|
| `include/progress.h`, `lib/progress.c` | rewritten: atomically replaced status records (`$PGDATA/pg_fastload/progress/<pid>.status`), stale-record purge by pid liveness, and the `pg_fastload_progress()` SRF that turns records into rows |
| `lib/pg_fastload.sql.in` | `pgfastload.fl_progress_rows()` (19 OUT columns, `RETURNS SETOF record`, `EXECUTE` revoked from `PUBLIC`) and the `pgfastload.pg_fastload_progress` view |
| `lib/exports.txt` | `pg_fastload_progress`, `pg_finfo_pg_fastload_progress` |
| `include/reader.h`, `lib/reader.c` | `PROGRESS` now defaults to `ON`; new `PROGRESS_NOTICE`; `Reader.type` kept for the record; all three dumped to the log |
| `lib/pg_fastload.c` | builds one `ProgressInfo` (database, table, `TYPE`, `serial`/`parallel`); `ProgressInit()` / `ProgressAbort()` on the error path |
| `lib/fastload_parallel.c` | unchanged apart from comments: the leader publishes, sampling the reader's byte counter every 1024 rows |
| `util/fl_progress_test.py` | rewritten acceptance harness: starts a load in one connection and polls the view from another |

Design notes:

- **A status file, not shared memory.** PostgreSQL only grew a named-DSM registry
  (`GetNamedDSMSegment`) in 17, while this tree supports 15 and later; plain SysV shared memory would
  force `shared_preload_libraries` on every deployment. A file needs neither, is cluster-wide (a load
  started in `tpcc` is visible from `postgres`), and degrades safely: a killed backend leaves a
  readable record whose pid is simply no longer alive.
- **Atomic replacement.** Each tick writes `<pid>.tmp` and `rename()`s it over `<pid>.status`, so a
  reader never sees a half-written line; a failed publish only disables publishing for that load and
  logs one line — it can never fail the load.
- **Percentage source.** `LIMIT` given → `rows / LIMIT` (the job is complete when the requested rows
  are in, even though the reader deliberately stops early). Otherwise `bytes_read / file size`.
  Neither known (STDIN, pipe) → `percent` is `NULL`, and the byte/row rates still work.
- **Never regresses, never lies.** A record's percentage is computed from monotonic counters; the
  `NOTICE` stream additionally caps the periodic lines at `99.9%`, because the reader can be at EOF
  while the leader still has rows to insert.
- **Ends and failures.** `ProgressFinish()` deletes the record (the view returns to `0 rows`);
  `ProgressAbort()` does the same on the error path without a closing line, because a load stopped by
  the parse-error contract (section 17) is not "done". A `SIGKILL`ed backend is reported once as
  `state = 'stale'` and deleted by the next reader.
- **Cost.** One integer comparison per inserted row — the clock is sampled once every 512 rows, and
  the record (about 1 KiB, no `fsync`) is rewritten at most once per interval. Measured on a 3 M-row
  serial load: inside the run-to-run noise (±2%).

Verification (`util/fl_progress_test.py`, shipped with the extension; PG 15.19 / 16.15 / 17.11 /
18.6 / 19rc1, 8 checks each): the view shows the running load with the right database, table, `TYPE`,
mode and interval and is empty again afterwards; `PARALLEL=ON` reports `mode = parallel`; `LIMIT`
switches `percent` to `rows / LIMIT` and sets `total_rows`; `PROGRESS=OFF` leaves the view empty for
the whole load; `PROGRESS_INTERVAL` alone turns publishing on; `PROGRESS_NOTICE=ON` adds the NOTICE
stream whose closing line carries `100.0%` and the exact row count; `PROGRESS_INTERVAL=50` is
rejected with `PROGRESS_INTERVAL must be between 100 and 3600000 milliseconds`; rows and percent are
monotonic and stay inside `0..100`. Cross-session polling of a 1 GiB load (`_t_view.py`) shows the
row counter climbing from 1.1 M to 6.5 M in a session that is not the loader's. The CSV 90/90 and
fixed-length 44/44 suites and the multi-version acceptance are unaffected.

## 19. One Branch: `master` (2026-11-10)

Earlier releases published five per-major branches — `REL_15_STABLE` … `REL_19_STABLE`. They were
snapshots of the *same* commit: `git diff REL_15_STABLE REL_18_STABLE` is empty, and so is every
other pair. Keeping them suggested five lines of development where there was one, and forced a fix
to be pushed five times to stay consistent.

The repository now has a single branch, `master`. The supported range lives in the source instead of
in the branch namespace:

| File | Change |
|---|---|
| `include/pg_fastload.h` | the old `PG_VERSION_NUM < 80300` guard becomes `< 150000`: majors below 15 fail with `#error pg_fastload requires PostgreSQL 15 or later`, so an untested combination cannot silently produce a binary |
| `docs/PORTING.md`, `PORTING.zh-CN.md` | support matrix loses the "Branch" column; branch policy rewritten as "one branch, `master`" |
| `README.md`, `README.zh-CN.md` | "each major has a matching `REL_1x_STABLE` branch" becomes one branch, differences resolved by `PG_VERSION_NUM` |
| delivery notes (both languages) | the same statement in the build section |
| `make_fastload_deliverables.py` | the portability check asserts the single-branch policy instead of the five branch names |

Why one tree is enough: every difference actually met between 15 and 19 is already a
`#if PG_VERSION_NUM` branch (the ~30 sites listed in [PORTING.md](PORTING.md) §3) plus the
version-pinned nbtree builder chosen by the same macro. Nothing is selected at run time, and no build
flag beyond `PG_CONFIG` has to be remembered.

Verification: `master` was exported with `git archive` (no working-tree help), then each major was
built from that export — 15.19 / 16.15 / 17.11 / 18.6 / 19rc1 all `INSTALL_OK`, `CREATE EXTENSION`
succeeds, a serial and a `PARALLEL=ON` load of the same file produce the same content digest, and the
progress view exists. Evidence: `compat_master_pg1x.out`.

The per-major branches were deleted locally and on the remote; the GitHub default branch is `master`.

---

*A summary of rejected approaches is in [OPTIMIZATION.md](OPTIMIZATION.md#8-summary-of-rejected-approaches); performance data is in [BENCHMARK.md](BENCHMARK.md); verification conclusions are in [VERIFICATION.md](VERIFICATION.md).*
