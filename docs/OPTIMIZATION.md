# pg_fastload optimization log

> Records all **adopted** optimizations of pg_fastload 1.0.0 from the first version to the current one, as well as approaches evaluated and **rejected**.
> Optimization item IDs (P0 / C / S / P-1 / W) correspond to [CHANGES.md](CHANGES.md).

Baseline environment: 1 GiB TPC-H lineitem CSV (7,375,042 rows × 16 columns), VM 16 vCPU / 7.3 GiB, PostgreSQL 18.6 release, WRITER=DIRECT, no index, warm cache 3-round median.

---

## 1. Overview

| Round | Topic | Adopted | Rejected |
|---|---|---|---|
| Round 1 | Rename + first integration | 4 defect fixes | — |
| Round 2 | P0 bottleneck optimization | 3 items (P0-1..3) | — |
| Round 3 | Single-reader, multi-worker parallel architecture | 1 architecture + 3 iterations (C-1..3) | — |
| Round 4 | Unified kernel shm_mq transport + cold-cache large-block I/O | 2 items (S-1, S-2) | 1 item (S-3) |
| Round 5 | Fixed-length parallelization | 1 item (P-1) | — |
| Round 6 | Index-less spool early exit | 1 item (W-3a) | 3 items (W-1, W-2, W-3) |
| Round 7 | Code cleanup | 9 places (no behavioral change) | — |
| Round 8 | Serial CSV bulk field split | 2 items (F-1 split, F-2 encoding-loop hoist) | — |

Cumulative: **15 adopted optimizations + 1 round of code cleanup; 4 rejected** (see Section 8 for details).

---

## 2. Optimization timeline (adopted)

| # | Round | Item | Detail | vs. previous |
|---|---|---|---|---|
| 1 | P0 | w=1 serial fallback | When PARALLEL_WORKERS ≤ 1, skip the full DSM + bgworker + queue machinery and take the original single-process CSV loop directly | fl_w1 **16.215 → 8.070 s (2.01×)** |
| 2 | P0 | run-based worker record copy | Byte-by-byte fl_append_rec changed to memcpy by contiguous runs; non-owned records skipped with zero copy | fl_w2 7.76 → 6.82 s (1.14×); w4 3.27→2.95; w8 2.53→2.32 |
| 3 | P0 | profiling hooks on parallel path | Added GLOBAL.PARALLEL + <PARALLEL> (SETUP/WAIT/COPY/WRITE) + <WORKER> (SCAN/FORM/IPC); eliminated WRITER.TOAST pseudo-timing | Performance-neutral (observability) |
| 4 | Round 3 | Single-reader, multi-worker architecture | Single reader bgworker single-pass scan, dispatched via per-worker queue; file I/O reduced from ×N to ×1 | First version warm-cache w2 10.82 s (gains realized on the cold-cache side) |
| 5 | Round 3 | C-1 byte-chunked round-robin routing | Per-record rec_no % N (head-of-line blocking, reader blocked 9.2 s) → switch owner every 512 KiB of contiguous records | w2 10.82 → 10.59 s |
| 6 | Round 3 | C-2 worker batch ring fetch | 2 spinlocks per record → one lock for batch fetch, lock-free frame switch | worker SCAN **5.2 → 0.32 s** |
| 7 | Round 3 | C-3 queue capacity 1 → 8 MiB | 1 MiB only buffers ≈6 ms of work; under single-queue backpressure the other queues starve; queue 8 MiB + 2 MiB return | **w2 10.59 → 3.46 s (3.06×)**; w4 5.13→1.97; w8 2.34→1.95 |
| 8 | Round 4 | S-1 unified kernel shm_mq transport | Removed the in-house FastloadRing (~200 lines of hand-written synchronization); both directions use kernel shm_mq; worker whole-frame zero-copy in single message | w2 3.46 → 3.33 s; w15 2.01→1.89; auto 1.99→1.88 |
| 9 | Round 4 | S-2 cold-cache large-block I/O | FL_READ_CHUNK 1 MiB, FL_STAGE_CAP 512 KiB; blocking syscalls reduced ~15× (16386→1026 reads + 17193→2148 sends) | Cold-cache good window: w2 33.3→**3.0 s**, w4 31.7→**1.8 s**, auto 30.1→**1.5 s**; warm cache flat |
| 10 | Round 5 | P-1 fixed-length parallelization | BINARY/FIXED plugged into the single-reader, multi-worker pipeline: reader splits by STRIDE, worker rebuilds COL descriptor via DSM blob and mirrors ExtractValuesFromFixed | Fixed-length 1 GiB: serial 1.62 s → w8 **0.89 s (1.82×)** |
| 11 | Round 6 | W-3a index-less spool early exit | SpoolerInsert() returns early when ri_NumIndices <= 0, eliminating per-tuple ExecStoreHeapTuple + econtext busy-wait (0.28 s/1 GiB) | fl_auto 1.87 → **1.75 s (1.07×)**; w15 1.91→1.73; fl_off 8.24→7.91 |
| 12 | Round 7 | Code cleanup | Dead fields, duplicated logic (single definition of FastloadDefaultWorkers()), undocumented aliases, shared pointers inside lock critical sections, MULTI_PROCESS combination guards, artifact cleanup | No behavioral change; 90/90 + 44/44 + 8/8 all green |
| 13 | Round 8 | F-1 bulk CSV field split | Quote-free records no longer walk the byte-by-byte state machine: one `memchr('\n')`, one quote/CR pre-scan, one record copy into the scratch field buffer, then each delimiter is cut in place. Same splitter in the parallel workers (`fl_parse_fields`) | `<CSV> SPLIT` **1.92 → 0.90 s**; fl_off 7.99 → **5.81 s**, auto 1.76 → **1.37 s** |
| 14 | Round 8 | F-2 encoding-loop hoist | `CheckerConversion()` was invoked once per field even with no `ENCODING` option; the 118 M-call loop now sits behind `if (checker->check_encoding)` | Included in the 3.82 s `VALUE` figure |

---

## 3. Performance evolution per configuration (1 GiB warm-cache median, seconds)

| Config | Original baseline (first ver.) | After P0 | After single-reader, multi-worker | After Round 4 | After Round 6 | **Current (Round 8)** | vs. original | vs. serial |
|---|---|---|---|---|---|---|---|---|
| fl_off (serial control) | 8.19 | 8.12 | 8.13 | 8.24 | 7.91 | **5.81** | 1.41× | 1.00× |
| fl_w1 | 16.215 | 8.07 | 8.05 | 8.23 | 7.86 | **5.80** | **2.80×** | ≈serial |
| fl_w2 | 7.76 | 6.82 | 3.46 | 3.32 | 3.28 | **2.76** | **2.81×** | 2.11× |
| fl_w4 | 3.27 | 2.95 | 1.97 | 1.96 | 1.91 | **1.58** | **2.07×** | 3.68× |
| fl_w8 | 2.53 | 2.32 | 1.95 | 1.88 | 1.75 | **1.40** | **1.81×** | 4.15× |
| fl_w15 | 2.30 | 2.32 | 2.01 | 1.91 | 1.73 | **≈1.37** | **1.68×** | 4.24× |
| fl_auto (=15) | 2.32 | 2.34 | 1.99 | 1.87 | 1.75 | **1.37** | **1.69×** | 4.24× |

> "Original baseline" is the median of the first version (each worker scanning the full file, with per-record modulo routing), hence w=1 as high as 16.2 s.
>
> Round 8 removed ~1.0 s of per-byte field splitting from every configuration, which is why the serial column moves too:
> the serial path is now 1.22× upstream `pg_bulkload` and 1.43× `COPY ... FROM`, and with parallelism on the load is
> 6.07× `COPY ... FROM` ([BENCHMARK §3, §3.1](BENCHMARK.md#31-the-serial-path-against-upstream-and-against-copy--from)).
> `fl_w15` was not re-measured separately; at that width it tracks `auto` within noise (they share the same 15-worker default).

---

## 4. Cold-cache evolution (posix_fadvise eviction, 1 GiB)

| Config | First ver. (N× scan) | Single-reader, multi-worker (ring buffer + small block) | Final version (good window) | `COPY ... FROM` (same conditions) |
|---|---|---|---|---|
| orig / fl_off | ≈7.1 s | ≈7.1 s | 5.81~5.94 s (stable) | 8.50~8.57 s |
| fl_w2 | 46.1 s (6.49× slower) | 30~37 s | **≈3.0 s (1.95× vs serial, 2.82× `COPY`)** | 8.53 s |
| fl_w4 | — | ≈31.7 s | **≈1.8 s (3.24× vs serial, 4.69× `COPY`)** | 8.53 s |
| fl_auto | — | ≈30.1 s | **≈1.5 s (3.81× vs serial, 5.50× `COPY`)** | 8.53 s |

**Residual note**: the final version still has random 11~62 s stalling rounds. Diagnostics (vmstat, dd probes, multiple rounds of cold_diag) confirmed that within the stall window the guest has 0 steal / 0 iowait / 100% idle, and total read volume and bandwidth are normal — this belongs to **host/hypervisor amplification of latency for blocking syscalls** (a single blocking call amplified to ~1.85 ms); the serial path is largely immune because stdio's large-block fread has ~500× fewer syscalls. The code-side systematic component (small blocks ×33k syscalls) was eliminated by S-2: before the fix 100% of parallel rounds were affected, after the fix only random windows are affected, and under good windows it surpasses serial by 3.8× and `COPY ... FROM` by 5.5×.

---

## 5. Cumulative gains

1. **w=1**: 16.215 → 5.80 s, **2.80×** (eliminated fixed overhead of the parallel mechanism, back to serial level);
2. **w=2**: 7.76 → 2.76 s, **2.81×** (2.11× vs. serial);
3. **w=4**: 3.27 → 1.58 s, **2.07×** (3.68× vs. serial);
4. **w=8 / w15 / auto**: 2.53 / 2.30 / 2.32 → 1.40 / ≈1.37 / 1.37 s,
   **1.81× / 1.68× / 1.69×** (≈4.2× vs. serial, already saturated at the leader-side serial `WriterInsert`
   structural floor of ~1.14 s/1 GiB);
5. **Fixed-length mode**: 1 GiB serial 1.62 s → fastest parallel 0.89 s (**1.82×**);
6. **Cold cache**: from 6.49× slower than serial in the first version → under good windows **surpasses serial by 3.8×**
   (and `COPY ... FROM` by 5.5×);
7. **Serial path (PARALLEL=OFF)**: 8.19 → **5.81 s (1.41×)**, of which W-3a contributed the spool early exit and
   Round 8 the bulk field split; it now runs **1.22× upstream pg_bulkload** and **1.43× `COPY ... FROM`**
   on the same file.

---

## 6. Maintainability gains

| Item | Change |
|---|---|
| Hand-written sync code | Removed in-house SPSC ring buffer ~200 lines (spinlock / head / tail / 100 µs backoff / count control) |
| Transport implementation | Both directions unified to the kernel shm_mq, with a single set of EOF/failure semantics |
| Default value definition | Worker default count converged from two implementations (inlined sysconf() inside ReaderInit() + orchestration layer) into FastloadDefaultWorkers() |
| Dead code | Removed FastloadShared.workers_ready, FastloadWorkerId.nworkers, undocumented alias PARSE_WORKERS |
| Observability | Profile covers all stages (GLOBAL / READER / WRITER / MERGE / PARALLEL / WORKER), GLOBAL total ≈ elapsed |
| Minimal-change principle | Round 6 achieved 1.33× (auto tier) with only 3 lines of effective code; did not rewrite the transport layer for marginal gains |

---

## 7. Verification status

fastload_verify.py **90/90 PASS**, fastload_verify_fixed.py **44/44 PASS**,
MULTI_PROCESS combination probe **8/8 PASS**; content digests for 7 configurations
`7375042|9258128314029119699254` (CSV) and `10737418|12495206446709334590242` (fixed-length)
all match GOLDEN; release + debug dual build rc=0. See [VERIFICATION.md](VERIFICATION.md) for details.

---

## 8. Summary of rejected approaches

All approaches below were evaluated at the source-code or measured level, and were not implemented because **the premise did not hold, the gain was insufficient, or the semantic cost was too high**.
The figures in this section are all measured or estimated from profile data.

### 8.1 S-3: offloading parallelism to WRITER=PARALLEL

| Item | Content |
|---|---|
| Idea | Hand the leader-serial ④ to an upstream parallel writer, forming "multi-process parsing + independent process writing" |
| Measurement/analysis | pg_bulkload's parallel writer is actually "1 insertion process + independent libpq connection + SysV IPC queue", **does not parallelize insertion itself**; the independent transaction makes rows visible to others before the caller commits, **breaking BEGIN/COMMIT atomicity**; measured negative gain for already-saturated scenarios |
| Conclusion | Rejected. It does nothing to decouple the structural bottleneck "leader-side serial WriterInsert is the ~1.3 s/1 GiB floor" |
| Status | Original semantics retained for compatibility, but **mutually exclusive** with PARALLEL=ON (when both are specified the reader falls back to serial and prints INFO) |

### 8.2 W-1: WAL / data write batching

| Item | Content |
|---|---|
| Idea | The WAL and data writes of the DIRECT writer could be further batched |
| Evidence | It is already a batched implementation: WAL writes only 1 first-page record per load (first-page guard in writer_direct.c + tail fsync); data goes through a self-managed 1024-page (8 MiB) block buffer for a single large-block write(); LSF fsyncs every 8 MiB |
| Conclusion | Rejected (**premise does not hold**, no optimization headroom) |

### 8.3 W-2: parallel page-fill pipeline

| Item | Content |
|---|---|
| Idea | Workers pre-assemble 8 KB page images; leader only does WAL + file write |
| Evidence | The WRITE phase ~1.28 s/1 GiB is pure per-tuple CPU (toast check + infomask/XID stamping + PageAddItem + SpoolerInsert ≈ 174 ns/row); the critical path of the auto-tier pipeline is the reader's READ 1.66 s, and the upper bound of total write-side optimization is 1.87 → ~1.7 s (≈1.10×) |
| Cost | Requires rewriting the worker/leader transport layer (block-number pre-allocation, t_ctid patching, Spooler semantics) |
| Conclusion | Rejected (limited gain vs. large change, violating "keep architecture simple first") |

### 8.4 W-3: parallel B-tree index build

| Item | Content |
|---|---|
| Idea | Build the index in parallel when loading with an index |
| Preliminary measurement | 1 GiB lineitem: no index 1.99 s; single btree index (l_orderkey, 84 MB) 3.87 s. Index cost ≈1.9 s (≈50%), there is indeed headroom |
| Path L1 | Reuse PG's native parallel btree (CREATE INDEX after load): load 2.06 s + CREATE INDEX **32.33 s** = 34.4 s, **8.3× slower**. Root cause: DIRECT bypasses the buffer manager, so index build must re-read the 1 GiB heap; this VM's amplified block-read latency makes the re-read cost far exceed the spool's CPU cost → **rejected by measurement** |
| Path L2 | In-house parallel spool→build (sharded spool + cross-process parallel sort + each building its own leaf-page segment + leader linking): upper-bound gain ≈1.3~1.4× (index-only scenarios), requires ~800~1200 lines of cross-process pipeline rework (sort state, WAL coordination, uniqueness check, crash recovery); tuplestore is a leader-private structure, cross-process sharing needs a separate segment-file protocol → **rejected** (cost/benefit mismatch) |
| Conclusion | The existing batched spool build is already 8.3× faster than "build index after load", the correct choice under the DIRECT architecture; W-3 closed |

### 8.5 Original architecture "N processes each scanning the full file" (superseded by single-reader, multi-worker)

| Item | Content |
|---|---|
| Idea | N workers each open() a full scan of the same CSV, with rec_no % N modulo ownership, doing field splitting only on owned records |
| Pros | Avoided the correctness difficulty of "unknown quote state at byte-segment start points" |
| Cost | I/O ×N + N duplicate boundary scans: 2.82× for 32M rows under warm cache, **reverses to 0.15× under cold cache (6.49× slower)** |
| Conclusion | Rejected, replaced by single-reader, multi-worker. The first version's record-boundary state machine, SKIP/LIMIT window semantics, degradation list, and race-tolerance design are all inherited by the single-reader, multi-worker architecture |

### 8.6 Progress through the loading session's own `NOTICE` stream

| Item | Content |
|---|---|
| Idea | Report progress with `NOTICE` messages (`PROGRESS=ON`) plus a copy in `LOGFILE` |
| Implementation cost | Very low: a formatting helper and a cadence check in the insert loop |
| Why it was insufficient | It reports to the **wrong place**: the loading session is the busy one, and its client is blocked inside the statement, so the person who needs the number cannot see it. Cross-session visibility (another psql window, a monitoring agent, `pg_stat_activity`-style polling) is the actual requirement, and a `NOTICE` also disappears above `client_min_messages = warning` |
| Conclusion | Rejected as the primary mechanism, **kept as an option**: progress is published to `$PGDATA/pg_fastload/progress/<pid>.status` and read through `pgfastload.pg_fastload_progress`; `PROGRESS_NOTICE=ON` still streams the `NOTICE` form for people who want it in the client or in `LOGFILE` |
| Alternatives considered | `dsm_registry` (`GetNamedDSMSegment`) — only exists from PG 17, while this tree supports 15+; SysV shared memory — would force `shared_preload_libraries` on every deployment; a background worker that samples `pg_stat_activity` — cannot see per-load byte counters |

---

*Related documents: [Changes](CHANGES.md) · [Verification report](VERIFICATION.md) · [Benchmarks](BENCHMARK.md) · [Architecture](ARCHITECTURE.md)*
