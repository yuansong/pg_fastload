# pg_fastload Performance Benchmark

> Environment: VM with 16 vCPUs (Intel i7-13700KF) / 7.3 GiB RAM, Oracle Linux 10.1,
> PostgreSQL 18.6 release (`REL_18_STABLE`, HEAD `1da6ddbdc3`), `PGDATA=/tmp/pgdata`, `port=5433`, database `tpcc`.
> Measurement dates: 2026-11-08 ~ 2026-11-10.
>
> This document is in two parts: **Sections 3–6 present results for the current version (single-read, multi-decode + all optimizations)**, where Section 4 separately provides
> the `TYPE=BINARY` vs `TYPE=FIXED` comparison (re-tested on 2026-11-10); Section 7 retains the same-condition comparison with upstream
> `pg_bulkload 3.1.23`, whose data was measured in the first version (N processes each doing a full scan), used only to illustrate serial-path equivalence and
> the historical baseline for parallel scalability.

---

## 1. Dataset

| Name | File | Size | Structure |
|---|---|---|---|
| CSV 1 GiB | `/tmp/tpch/lineitem.csv` | 1 GiB / 7,375,042 rows / 16 columns | TPC-H lineitem, plain comma-separated, no quoting |
| Fixed-length 1 GiB | `/tmp/tpch/fixed1g.bin` | 1 GiB / 10,737,418 rows | 100 B per row: `CHAR(16)` + `INTEGER(4)` + `BIGINT(8)` + `DOUBLE(8)` + `CHAR(64)`, native binary field mirror |
| Small / medium / large tiers (historical comparison) | `/tmp/tpcc/order_line.csv` subset | 1M rows / 77 MB, 8M rows / 640 MB, 32M rows / 2.5 GiB | TPC-C order_line, 10 columns |

## 2. Measurement Method and Fairness Controls

| Control | Approach |
|---|---|
| Target table | Each phase `CREATE TABLE t_bench_cmp (LIKE public.order_line)`, each round `TRUNCATE` (**separate session**, not the same session as the load) |
| Input | All configurations use the **same file**; `TYPE`, `WRITER=DIRECT`, and `LOGFILE` directories are identical |
| Timing metric | Server-side **elapsed** from the extension log as the primary metric, wall-clock as secondary |
| Cache state | Warm cache: `cat CSV > /dev/null` warm-up before each round; cold cache: `posix_fadvise(DONTNEED)` to precisely drop the file's clean pages (no root needed), and read `Cached` from `/proc/meminfo` to confirm |
| Ordering bias | Multiple rounds alternate forward / reverse order, take the **median** |
| CHECKPOINT interference | `CHECKPOINT` is run **out-of-timing** — otherwise the previous round's dirty-page flush would fall into the next round, which once produced a false speedup of 5.58× |
| Content correctness | `count(*) \|\| sum(hash_record_extended(t,0))`, independent of physical insertion order |
| Equivalence anchor | `PARALLEL=OFF` should match upstream timing, used to identify environment jitter |
| Index | Unless otherwise specified, the benchmark table has **no index** (the gain from W-3a comes precisely from this scenario) |

Configuration set: `fl_off` (`PARALLEL=OFF`, serial control), `fl_w1`, `fl_w2`, `fl_w4`, `fl_w8`, `fl_w15`,
`fl_auto` (no `PARALLEL_WORKERS` specified, 16 cores → 15).

---

## 3. Current Version: 1 GiB CSV (Warm Cache, Median of 3 Rounds)

| Config | r1 | r2 | r3 | **Median** | vs Serial | vs First Version |
|---|---|---|---|---|---|---|
| `fl_off` | 8.02 | 7.99 | 7.98 | **7.99 s** | 1.00× | 1.03× |
| `fl_w1` | 8.00 | 7.97 | 7.97 | **7.97 s** | 1.00× | 2.03× |
| `fl_w2` | 3.52 | 3.68 | 3.31 | **3.52 s** | 2.27× | 2.20× |
| `fl_w4` | 2.11 | 1.92 | 1.90 | **1.92 s** | 4.16× | 1.70× |
| `fl_w8` | 1.76 | 1.77 | 1.74 | **1.76 s** | 4.54× | 1.44× |
| `fl_w15` | 1.74 | 1.74 | 31.83\* | **1.74 s** | 4.59× | 1.32× |
| `fl_auto` | 1.76 | 19.19\* | 1.71 | **1.76 s** | 4.54× | 1.32× |

\* Occasional host hypervisor jitter round (see §5 for details), automatically excluded when taking the median.

**Content verification**: The digests for all 7 configurations are `7375042|9258128314029119699254` (`ALL_MATCH = True`).

**Segmented profile (`USE_PROFILE=1`)**:

| Config | GLOBAL | PARALLEL{COPY, READ, SETUP, WAIT, WRITE} | WORKER{FORM, IPC, SCAN} |
|---|---|---|---|
| `fl_w1` | 7.985 s | all 0 (serial path) | all 0 |
| `fl_w2` | 3.519 s | COPY 0.178 / READ 3.435 / WAIT 2.169 / WRITE 1.125 | FORM 5.654 / IPC 0.728 / SCAN 0.530 |
| `fl_w4` | 1.915 s | COPY 0.204 / READ 1.827 / WAIT 0.491 / WRITE 1.176 | FORM 5.796 / IPC 1.206 / SCAN 0.378 |
| `fl_auto` | 1.761 s | COPY 0.183 / READ 1.554 / WAIT 0.350 / WRITE 1.152 | FORM 6.452 / IPC 17.206 / SCAN 1.358 |

Interpretation:

- `WRITE ≈ 1.15 s` barely changes with the number of workers — this is precisely the floor of the leader's serial `WriterInsert`;
- The `WORKER` group is the **sum of each worker's time** (accumulated across processes), so `FORM 6.45 s` with 15 workers corresponds to ≈0.43 s per worker and is not on the critical path;
- `WAIT` decreases as the worker count grows (w2 2.17 s → auto 0.35 s), indicating that at w2 the leader is waiting for data, while at auto the leader is waiting on its own writes.

---

## 4. Fixed-Length Input: `TYPE=BINARY` vs `TYPE=FIXED` (1 GiB, Warm Cache)

### 4.1 Why the Two Should Run at the Same Speed

`TYPE=BINARY` and `TYPE=FIXED` map to the **same parser** in `ReaderCreate()` (`CreateBinaryParser()`, indices 0 and 1); `FIXED` is the historical alias carried over from pg_bulkload.
Both are "fixed-length records, native binary mirrors with fields declared by `COL=type(length)`",
**not** the binary stream format of PostgreSQL `COPY ... WITH (FORMAT binary)`.
Since the parser, splitting logic, and parallel routing are completely identical, the measured timings of the two should fall within run-to-run noise — which is exactly what this section verifies.

### 4.2 Dataset and Field Mapping

| Item | Value |
|---|---|
| File | `/tmp/tpch/fixed1g.bin`, 1,073,741,800 bytes |
| Size | 10,737,418 records × 100 bytes |
| Record structure | `CHAR(16)` + `INTEGER(4)` + `BIGINT(8)` + `DOUBLE(8)` + `CHAR(64)` |
| Target table | `public.t_bench_bf (c1 text, c2 integer, c3 bigint, c4 double precision, c5 text)`, **no index** |
| Content distribution | First field increments as `row%013d`; integer/float generated by `i%1000000`, `i*7919`, `i/7.0`; last field cycles through `a`–`z` |

### 4.3 Measurement Method

Identical to §2, to allow horizontal comparison with CSV results: `WRITER=DIRECT`; out-of-timing `CHECKPOINT` each round; input file warmed up with `cat > /dev/null` first; `TRUNCATE` in a separate session; forward/reverse alternation to cancel drift; take the **median**; after each configuration's load, compute `count(*) || sum(hash_record_extended(t,0))` to verify content.

Driver script: `bench_binary_fixed.py` (3 rounds of `BINARY`, 3 rounds of `FIXED`).

### 4.4 Results (2026-11-10, PostgreSQL 18.6, 16 vCPU)

| Config | `TYPE=BINARY` | `TYPE=FIXED` | Difference | vs Serial (BINARY) |
|---|---|---|---|---|
| `fl_off` | 1.59 s | 1.60 s | +0.6% | 1.00× |
| `fl_w1` | 1.61 s | 1.61 s | 0% | 0.99× (serial fallback, semantically correct) |
| `fl_w2` | 1.07 s | 1.07 s | 0% | 1.49× |
| `fl_w4` | 0.91 s\* | 0.91 s | 0% | 1.75× |
| `fl_w8` | **0.87 s** | **0.86 s** | −1.1% | **1.83×** |
| `fl_w15` | 0.89 s | 0.88 s | −1.1% | 1.79× |
| `fl_auto` | 0.89 s | 0.88 s | −1.1% | 1.79× |

\* The first round of `BINARY`'s `fl_w4` hit host jitter twice (31 s); that cell was re-tested separately for 6 rounds: 5 rounds landed in 0.89–0.91 s, the other 1 round at 31 s, median taken as 0.91 s (raw values in `bench_bf_w4.json`); the remaining cells are medians of 3 rounds.

**Conclusion 1: The two are equivalent.** The maximum difference across the 7 configurations is 1.1%, within run-to-run noise; content digests 14/14 are all
`10737418|12495206446709334590242` (`ALL_MATCH = True`).
`TYPE=FIXED` can be used as an alias for `TYPE=BINARY`, and vice versa.

**Conclusion 2: The parallel speedup narrows according to the "parallelizable CPU share."** Fixed-length serial needs only 1.59 s, while CSV serial needs 7.99 s
(§3) — fixed-length has no quoting state machine, values are directly C-cast, and splitting is by fixed offset. The fixed overhead of loading 1 GiB (reading the file, writing heap pages, leader serial `WriterInsert`) therefore accounts for a larger share, so the speedup drops from 4.5× for CSV to 1.83×,
but the **absolute time is still 1/2 of CSV's** (w8: 0.87 s vs 1.76 s).

**Round noise**: Of the 42 timing units this run, 5 were polluted by 19–31 s host hypervisor jitter (both serial and
parallel, unrelated to the configuration); all were removed by the median; the rounds judged as polluted retain their raw values in `bench_binary_fixed.json`
for review.

---

## 5. Cold Cache (`posix_fadvise` Eviction of 1 GiB CSV)

| Config | Elapsed per Round | Clean Run Window Median | vs Serial |
|---|---|---|---|
| `fl_off` (serial) | 7.07 / 7.14 / 7.20 | 7.14 s | 1.00× |
| `fl_w2` | 3.02 / 3.31 / 30.6\* | **3.02 s** | 2.36× |
| `fl_w4` | 1.78 / 1.86 / 41.2\* | **1.82 s** | 3.92× |
| `fl_auto` | 1.51 / 1.58 / 62.4\* | **1.55 s** | 4.61× |

\* Host-layer jitter round.

**On residual jitter**: Parallel configurations still randomly show rounds of 11~62 s. The diagnosis (per-second vmstat sampling, `dd` probes,
multiple cold_diag runs) is — within the jitter window the guest's `steal` is 0, `iowait` is 0, CPU 100% idle,
total read volume and bandwidth are normal, i.e. the host/hypervisor amplifies the latency of **blocking syscalls** (a single call amplified to ~1.85 ms).
The serial path uses stdio bulk `fread`, with about 500× fewer syscalls, and is therefore largely immune.

The **systematic** component that can be eliminated on the code side has been eliminated: before S-2, each load did 16386 reads + 17193 sends; after,
1026 + 2148 (~15× reduction). Before the fix, 100% of cold-cache parallel rounds suffered; after the fix, only random windows suffer,
and within the clean run window parallel **overtakes serial by up to 4.9×**. This is the core benefit of the "1× read" design: the first version's N× scan was 6.49× slower than serial in this scenario.

---

## 6. Same-Condition Comparison with Upstream pg_bulkload 3.1.23 (First-Version Measurement, Historical Baseline)

> This data was measured in the v1.0.0 first version (N workers each doing a full scan). It is retained to ① prove that `PARALLEL=OFF` is strictly equivalent to upstream; ② illustrate the historical baseline for parallel scalability.
> The current version's numbers at these sizes should beat the table below (see Section 3 for the same-file 1 GiB comparison).

### 6.1 Warm Cache

| Data Size | Upstream pg_bulkload | pg_fastload OFF | pg_fastload auto(=15) | Speedup (vs Upstream) |
|---|---|---|---|---|
| 1M rows / 77 MB | 0.530 s | 0.530 s | 0.220 s | 2.41× |
| 8M rows / 640 MB | 4.070 s | 4.110 s | 1.440 s | 2.83× |
| 32M rows / 2.5 GiB (steady state) | 16.180 s | 16.325 s | 5.740 s | 2.82× |

- `PARALLEL=OFF` is strictly equivalent to upstream (0.530 vs 0.530, 4.110 vs 4.070, 16.325 vs 16.180),
  the rename and code cleanup introduced no serial regression; differences are within ±1.5%.
- At the time, `w≤2` gave negative returns (0.36~0.74×), with the break-even point near `w=4`; `w=8→15` tends to saturate —
  ④ the serial `WriterInsert` becomes the bottleneck, and 16 cores will not deliver 16×.
- On the first round of 32M rows, upstream showed a 46.57 s cold-start anomaly; the steady-state 16.13~16.25 s across the 4 rounds isolated with `CHECKPOINT` is the valid baseline. Taking the median of two rounds directly would inflate the speedup to a false 5.58×.

### 6.2 32M-Row Scalability (First Version, Steady State)

32M rows / 2.5 GiB is close to the memory ceiling (7.3 GiB), so the steady state is taken over 2 rounds + `CHECKPOINT` isolation:

| Config | Steady Median elapsed | min ~ max | Rounds | vs Upstream |
|---|---|---|---|---|
| Upstream pg_bulkload 3.1.23 | 16.180 s | 16.130 ~ 16.250 | 4 | 1.00× |
| `PARALLEL=OFF` | 16.325 s | 16.270 ~ 16.380 | 2 | 0.99× |
| `auto` (=15) | 5.740 s | 5.620 ~ 5.860 | 2 | **2.82×** |

The tiered numbers measured in the same stage without `CHECKPOINT` isolation were w1 47.42 / w2 22.17 / w4 10.62 /
w8 5.80 / w15 5.66 s, where the first round includes upstream's 46.57 s cold-start anomaly, inflating the whole thing, and is **not used as a conclusion** —
which is also why this report insists on "`CHECKPOINT` run out-of-timing".

At large scale the parallel benefit is more pronounced than at 1 GiB (first version's 1 GiB was only 1.33×, 8M rows 2.83×, 32M rows 2.82×),
because the fixed overhead (DSM creation, bgworker startup, on the order of tens of milliseconds) is amortized, while the total parallelizable parsing CPU grows linearly with row count.

---

## 7. Conclusions and Tuning Recommendations

1. **Zero serial-path regression**: `PARALLEL=OFF` matches upstream (±1.5%), so `PARALLEL` can safely be used as a pure-gain switch.
2. **Parallel benefit depends on "parse share"**: CSV (byte-by-byte state machine) 4.5×; fixed-length (C-cast) 1.8× —
   the "cheaper" the format, the sooner it saturates at the leader's serial writes. `TYPE=BINARY` and `TYPE=FIXED` use the same parser,
   with a measured difference ≤1.1% (§4); either choice has no performance impact, so pick by semantics.
3. **Worker count**: 8~15 is already near saturation (1 GiB CSV: w8 4.54×, w15 4.59×); more workers only increase
   memory usage (8 MiB input queue per worker). It is recommended to start from the default value (CPU−1).
4. **The larger the scale, the more parallelism pays off**: first version 32M rows 2.82× vs 1 GiB 1.33×, showing that the fixed overhead (DSM, bgworker
   startup, tens of milliseconds) is non-negligible for small files.
5. **Don't panic-downgrade workers on cold cache**: the 1× read design lets parallel still overtake serial under cold cache (clean run window 4.6×);
   true minute-level jitter is a host-layer problem, not a configuration problem.
6. **Indexed loads**: index cost is about half of the total indexed-load time (1 GiB measured ≈1.9 s), and that phase runs serially in
   the leader; for bulk imports it is recommended to first drop non-essential indexes (spool batch build is already 8.3× faster than "post-load `CREATE INDEX`",
   see [OPTIMIZATION.md](OPTIMIZATION.md#84-w-3-parallel-b-tree-index-build)).

---

## 8. Export Throughput: `pg_fastload_export` vs `COPY TO`

Export is single-process by construction: one backend fetches rows from a cursor and writes one file.
There is no `PARALLEL` keyword on this path, so the comparison is against `COPY` on one side and against
the loader fed the same bytes on the other.

| Item | Value |
|---|---|
| Table | `public.exp_big`: 8,000,000 rows / 521 MB heap (`id int4` + `pad text` (md5)) |
| Output | CSV, 326,888,903 B with header (312 MiB) |
| Environment | as in §2: 16 vCPU / 7.3 GiB, page cache, median of 5 |

| Step | Time | What it isolates |
|---|---|---|
| `SELECT count(*)` | 0.05 s | reading 521 MB is free: 16-core parallel seq scan |
| `SELECT count(*)`, parallel scan disabled | 0.14 s | the serial heap scan on its own |
| `COPY ... TO '/dev/null' (FORMAT csv)` | 0.70 s | scan + CSV formatting, no file |
| `COPY (SELECT ...) TO '/dev/null'` | 0.74 s | same with or without a parallel plan |
| `COPY ... TO '<file>' (FORMAT csv)` | 0.80 s | + 0.10 s to write 327 MB |
| `pg_fastload_export(TYPE=CSV)` → `/dev/shm` | 0.86 s | formatting only, no disk |
| `pg_fastload_export(TYPE=CSV)` → disk | **0.90 s** | ≈363 MB/s, 8.9M rows/s |
| `pg_fastload_export(TYPE=BINARY)` → disk | 0.94 s | 288,000,000 B fixed layout |
| `pg_fastload_export(QUERY=SELECT id ...)` | 0.49 s | one column, 62.9 MB |
| load the same CSV back, `PARALLEL=OFF` | 1.06 s | UNLOGGED target, `WRITER=DIRECT` |
| load the same CSV back, `PARALLEL_WORKERS=4` | 0.48 s | 2.2× the serial load |

Reading the table: the scan is not where the time goes. A serial heap scan of the 521 MB table costs
0.14 s, and `COPY (SELECT ...) TO /dev/null` costs 0.74 s whether or not the planner is allowed a
parallel scan, so ~0.70 s of either export is per-row work in the writer. Of our 0.90 s, about 0.48 s
is per-row overhead that does not depend on what is written — cursor round trip,
`heap_deform_tuple`, one `MemoryContextSwitchTo`/`Reset` pair per row — 0.40 s is text output plus
quoting of the 264 MB `pad` column, and 0.04 s is the file write itself. The export is therefore
CPU-bound inside a single backend, with 15 of the 16 cores idle, and a cursor cannot use a parallel
plan at all. `PROGRESS=ON` is free (0.90 s either way); the clock is sampled every 512 rows.

Two consequences worth stating plainly:

* **Export is slower than `COPY TO`, by roughly 12%** (0.90 s vs 0.80 s server-side). The earlier
  figure of 0.95 s in this section was `psql`'s `\copy`, which pays a client round trip for 8M rows;
  server-side `COPY` does not. The output is still `cmp`-identical, and the export path is the one that
  also drives BINARY/FIXED, so paying 0.10 s for one shared writer is a fair trade.
* **BINARY is not cheaper than CSV** (0.94 s vs 0.90 s), unlike what one might expect from a
  `memcpy`-style writer. The layout pass runs the tuple checker first — it builds a coerced tuple and
  copies every field into its declared width — which offsets the text conversion it avoids.

Parallelising this path would have to preserve row order to stay comparable (`cmp`) and to round-trip,
so the only workable design is ctid-range partitioning into per-worker temporary files plus an ordered
stitch, which adds a full read and write of the output. The parallel loader's 0.48 s is an upper bound
that ignores that stitch; the realistic ceiling on this host is about 2×. It is left as a documented
trade-off (USER_MANUAL §15, item 11) rather than shipped.

---
