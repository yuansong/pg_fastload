# pg_fastload Verification Report

> Version `1.0.0`  Author: yuanyuansong  Last updated: 2026-11-10
>
> This document records the verification status of the **current version**, with reproducible conclusions along four dimensions: functionality → build → memory → compliance.
> Performance numbers: see [BENCHMARK.md](BENCHMARK.md); round-by-round optimization process: see [OPTIMIZATION.md](OPTIMIZATION.md).

---

## 1. Delivery Scope

| Item | Content |
|---|---|
| Project | `pg_fastload` (standalone open-source release, version `1.0.0`) |
| Upstream | `pg_bulkload` 3.1.23, HEAD `798e0f2`, BSD 3-Clause-style license |
| Parallel coverage | `TYPE=CSV` and `TYPE=BINARY / FIXED` (fixed-width); `TUPLE / FUNCTION` go through the serial path |
| Export coverage | `pgfastload.pg_fastload_export()`: `TYPE=CSV / BINARY / FIXED` from `TABLE=` or `QUERY=` (single-process, see §12) |
| External behavior | When `PARALLEL=ON`, the above types default to multi-process parallelism; **no additional type names are introduced** — `TYPE` remains the upstream's five types; `PARALLEL_WORKERS` defaults to "online CPU cores − 1" |
| License | The `COPYRIGHT` original text is retained; `LICENSE` / `NOTICE` added; added/modified files marked `Copyright (c) 2026, yuanyuansong`; explicitly states it is not endorsed by NTT, Inc. |

---

## 2. Verification Environment

| Item | Value |
|---|---|
| Host | VM `192.168.25.134`, **16 vCPU** (i7-13700KF) / **7.3 GiB** RAM / 3 GiB swap |
| OS | Oracle Linux Server 10.1, kernel 6.12 (VMware) |
| PostgreSQL | 18.6 (`REL_18_STABLE`, HEAD `1da6ddbdc3`); postmaster verified to be a release-build binary |
| Install | release `/home/ysyuan/postgres/release`, debug `/home/ysyuan/postgres/debug` (`--enable-debug --enable-cassert -O0 -g`) |
| Test cluster | `PGDATA=/tmp/pgdata`, `port=5433`, database `tpcc`, `max_worker_processes=32` |
| Data | `/tmp/tpch/lineitem.csv`: 1 GiB / 7,375,042 rows / 16 columns; `/tmp/tpch/fixed1g.bin`: 1 GiB / 10,737,418 rows × 100 B |
| Comparison baseline | Clean upstream `pg_bulkload 3.1.23` (source HEAD `798e0f2`, build artifacts contain no parallel-related symbols), confirmed to be the original |

---

## 3. Build Verification

| Build | `clean` | `make` | `install` | New diagnostics | `pg_fastload_worker_main` export |
|---|---|---|---|---|---|
| release | rc=0 | rc=0 | rc=0 | none | `T` symbol present |
| debug (`--enable-cassert`) | rc=0 | rc=0 | rc=0 | none | `T` symbol present |

The only compilation diagnostics are two upstream-original `-Wmissing-variable-declarations` warnings in `bin/binary.c` (`TYPES` / `ALIASES`), unrelated to the added code.

---

## 4. Functional Correctness

Criterion: the serial baseline (`PARALLEL=OFF`) and the parallel (`PARALLEL=ON`) runs produce an identical **content set**.
The fingerprint is `count(*) || sum(hash_record_extended(t, 0))` — order-independent, suited to the parallel path's non-order-preserving nature.

### 4.1 CSV: 90/90 PASS

| Phase | Content | Test-case shape |
|---|---|---|
| Phase 1 | 10 boundary datasets × 6 assertions | `basic` (LF), `crlf`, `lone_cr`, `quoted` (comma/newline inside quotes / escaped `""` / quoted empty), `no_eol` (last line without trailing newline), `nulls` (empty field→NULL, `\N` literal, leading/trailing spaces), `empty` (0 rows), `one_row`, `cross_chunk` (terminator / newline inside quotes / escape landing exactly on a 1 MiB read-block boundary), `many` (10,000 rows) |
| Phase 2 | Custom parser parameters | `DELIMITER=\|`, `QUOTE=~`, `ESCAPE=~`, `NULL=NA`, serial/parallel digests identical |
| Phase 3 | `SKIP` + `LIMIT` | `SKIP=1 LIMIT=3`, workers=3, digests bit-for-bit identical |
| Phase 4 | Parallelism degree variation | workers = 1 / 2 / 3 / 4 / unspecified (auto=15) |
| Phase 5 | Automatic downgrade regression | `FORCE_NOT_NULL=c1` → log shows `PARALLEL disabled`, no bgworker started, content correct |
| Phase 6 | Startup race stress | 5-line small file, 10 consecutive runs of `PARALLEL=ON workers=2` |

The 6 assertions of Phase 1 (per dataset): serial is executable; serial does not mistakenly enable parallelism; parallel is executable; content digests match;
a bgworker really started (log shows `PARALLEL=ON, TYPE=CSV, workers=N` with no `PARALLEL disabled`);
the sum of each worker's output equals the table row count, and the log has no real error markers (judged by `ERROR:` / `FATAL:` prefixes).

### 4.2 Fixed-width: 44/44 PASS

Coverage: pure `CHAR` fixed-width text, `BINARY`/`FIXED` alias consistency, mixed text + binary numerics
(INTEGER(4) / BIGINT(8) / DOUBLE(8) native byte decoding), `NULLIF` (text mode and equal-length numeric mode),
`STRIDE` larger than the sum of fields (trailing padding), trailing residual bytes at EOF ignored (WARNING, consistent with serial), `SKIP`+`LIMIT` window,
`PRESERVE_BLANKS`+`VARCHAR` trailing-space preservation, empty file, workers 1/2/3/4/auto (w=1 serial fallback),
`FILTER` automatic downgrade, 10,000 rows × 4 workers shard integrity.

### 4.3 `MULTI_PROCESS` combination: 8/8 PASS

When `MULTI_PROCESS=YES` (i.e. `WRITER=PARALLEL`) and `PARALLEL=ON` are both specified, they must "not stack":
serial load correct, combined load correct, log shows `PARALLEL disabled (MULTI_PROCESS ...)`,
no bgworker started, content digests match, `MULTI_PROCESS` alone still works normally.

### 4.4 1 GiB content digests

| Suite | Digest | Config count | Result |
|---|---|---|---|
| CSV 1 GiB | `7375042\|9258128314029119699254` | 7 (off / w1 / w2 / w4 / w8 / w15 / auto) | **ALL_MATCH = True** |
| Fixed-width 1 GiB | `10737418\|12495206446709334590242` | 7 | **ALL_MATCH = True** |

---

## 5. Performance Verification

Conclusion summary (1 GiB CSV, warm cache, median of 3 rounds, DIRECT, no index):

| Config | Time | vs serial | vs `COPY ... FROM` |
|---|---|---|---|
| `PARALLEL=OFF` | 5.81 s | 1.00× | 1.43× |
| `w=1` (serial fallback) | 5.80 s | 1.00× | 1.43× |
| `w=2` | 2.76 s | 2.11× | 3.01× |
| `w=4` | 1.58 s | 3.68× | 5.27× |
| `w=8` | 1.40 s | 4.15× | 5.94× |
| `auto` (default 15) | 1.37 s | 4.24× | 6.07× |
| `COPY ... FROM` (reference) | 8.32 s | 0.70× | 1.00× |

Against the two reference implementations, same file and same empty table: `PARALLEL=OFF` 5.81 s vs
upstream `pg_bulkload 3.1.23` 6.98 s (**1.22×**) and vs `COPY ... FROM` 8.32 s (**1.43×**, and 1.29×
against `COPY` into an UNLOGGED table). Serial equivalence was re-established before the Round 8 change
(6.95 s vs 7.09 s, 2%), so the gap is the optimisation, not a behavioural difference.

Fixed-width 1 GiB: serial 1.62 s → fastest parallel 0.89 s (w=8, **1.82×**); this mode has no `COPY`
counterpart, because `COPY` cannot read an unframed fixed-length record.
Under a good window with cold cache (evicted via `posix_fadvise`), it beats serial by 3.8× and
`COPY ... FROM` by 5.5×.
Full methodology, fairness controls, and tiered results: see [BENCHMARK.md](BENCHMARK.md).

**Measurement caveats** (lessons from experience): besides timing, `CHECKPOINT` isolation is mandatory, otherwise the previous round's dirty-page flush creates a
false 5.58× speedup; `TRUNCATE` and `pg_fastload()` cannot be in the same `psql -c` (it self-deadlocks to timeout when `MULTI_PROCESS=YES`).

---

## 6. Memory Management and Compliance Static Review

| Check item | Result |
|---|---|
| `malloc/calloc/realloc/free` in the added parallel sources (`fastload_parallel.c` / `fastload_worker.c` / `fastload_parallel.h`) | **NONE** |
| Undefined malloc-family symbols in the dynamic symbol table of `release/lib/pg_fastload.so` | **NONE** |
| `malloc` in the upstream `bin/` client and `lib/pgut/pgut-pthread.c` (Windows pthread compatibility layer) | Kept as-is: belongs to the command-line frontend and is not involved in server-side loading |
| `COPYRIGHT` | Original text intact (BSD 3-Clause three clauses + disclaimer) |
| `LICENSE` / `NOTICE` / `README*` / `docs/` | Explicitly states "secondary implementation based on pg_bulkload", author `yuanyuansong`, not endorsed by NTT |
| Upstream attribution | **Compliance-required, clearly written**: `LICENSE` / `NOTICE` / `README.md` / `docs/CHANGES.md` / `docs/ARCHITECTURE.md` and the header comments of added source files all state the upstream project name, version `3.1.23`, HEAD `798e0f2`, source URL, BSD 3-Clause, original copyright `NTT, Inc.`, the change list, and the author attribution |
| Type list | Consistent with upstream: `BINARY / FIXED / CSV / TUPLE / FUNCTION`, no additional type names |

> **The difference between "attribution" and "renaming"**: references to `pg_bulkload` are the attribution and non-endorsement requirements of BSD 3-Clause clauses 1, 2, and 3; they are compliance-required, not "leftovers" to be cleaned up. Full renaming targets code identifiers such as the project / extension / shared library / SQL schema / SQL functions / macros, and does not conflict with compliance attributions.

**Memory lifecycle (palloc / MemoryContext)**

- worker: `workctx = AllocSetContextCreate(TopMemoryContext, ...)` holds long-lived objects;
  `recctx` is a per-record scratch; after `shm_mq_send()` has copied the bytes into the queue, `MemoryContextReset(recctx)` is called,
  so 130 million records do not accumulate.
- main backend: orchestration objects (DSM / `shm_mq_handle` / bgworker handles / `done[]`) live in a separate `runctx`,
  **not in `wt->context`** — the latter is reset after each `WriterInsert()` round.
- Each received message is `MemoryContextAlloc`'d into a writable copy in `wt->context` and passed to `WriterInsert()`
  (which modifies `t_data` in place), then `MemoryContextReset`; the function finalizes with `MemoryContextDelete(runctx)`.
- In `PG_CATCH()`, `fastload_cleanup()` is uniformly executed (terminate workers, `shm_mq_detach`,
  `dsm_unpin_segment` + `dsm_detach`) before `PG_RE_THROW()`.

---

## 7. Verification Conclusions per Round (adopted optimizations)

| Round | Change | Verification conclusion |
|---|---|---|
| P0 (Round 2) | w=1 serial fallback, run-based record copy, parallel profiling hooks | CSV 90/90; w=1 16.215→8.070 s; content digests match; profile `GLOBAL` total ≈ elapsed, `WRITER.TOAST` pseudo-timing eliminated |
| Single-read multi-decode (Round 3) | Single reader single-pass scan + chunked round-robin + batched slot acquisition + 8 MiB queue | CSV 90/90; 7-config digests match; w2 6.82→3.46 s; worker SCAN 5.2→0.32 s; reader blocking time → ~0 |
| shm_mq + large-block I/O (Round 4) | reader→worker switched to kernel shm_mq; read 1 MiB / send 512 KiB | CSV 90/90; warm cache on par, slightly better (w2 3.46→3.32 s); cold cache good window beats serial by up to 4.9×; net removal of ~200 lines of hand-written synchronization code |
| Fixed-width parallelism (Round 5) | BINARY/FIXED connected to the single-read multi-decode pipeline | Fixed-width 44/44 + CSV 90/90; fixed-width 1 GiB 1.62→0.89 s (1.82×); 7-config digests match |
| W-3a (Round 6) | `SpoolerInsert()` early-exit on no index | CSV 90/90 + fixed-width 44/44; fl_off 8.24→7.91 s, auto 1.87→1.75 s; zero regression on the no-index path |
| Code cleanup (Round 7) | Dead fields / duplicated logic / aliases / lock critical sections / combination guards | CSV 90/90 + fixed-width 44/44 + MULTI_PROCESS 8/8; 7-config digests match; performance within ±5% fluctuation |
| Serial CSV bulk split (Round 8) | Quote-free records take a memchr-based splitter in `CSVParserRead()` and in the workers' `fl_parse_fields()`; encoding loop hoisted behind `check_encoding` | CSV 90/90 (incl. quoted / lone-CR / cross-chunk / blank-line / extra-column fallbacks) + fixed-width 44/44; clean build on 15.19/16.15/17.11/18.6/19rc1; fl_off 7.99→5.81 s, auto 1.76→1.37 s; digest `7375042\|9258128314029119699254` on both paths |

The evaluated-but-rejected approaches (using `WRITER=PARALLEL` as the parallelism scheme, WAL/data write batching, parallel page-fill pipeline, parallel B+ tree build) and their evidence are in [OPTIMIZATION.md](OPTIMIZATION.md#8-summary-of-rejected-approaches).

---

## 8. Residual Risks and Environmental Limitations

1. **ASan cannot be run**: the VM lacks the `libasan.so.8` runtime, and installing via `dnf` requires privileges and is blocked by the environment.
2. **Dynamic leak detection not run**: valgrind requires launching a separate PGDATA/port with a debug-build PG using `--trace-children=yes`, which would disrupt the current 5433 test cluster; this round's memory conclusion is a **static lifecycle review** (§6).
   The 208 B definite lost inherent to PG startup and the `Syscall param write(buf)` in `relcache.c` are normal and not extension leaks.
3. **Full 10.7 GiB performance comparison not completed**: a 130M-row single transaction needs ~13 GiB table + 10 GiB WAL, exceeding the VM's 51 GiB root partition (`No space left on device`). Therefore no full-scale speedup is given, and benefits are not extrapolated.
4. **Cold cache still has random stalling rounds**: `vmstat` shows 0 steal / 0 iowait / 100% idle in the stall window, with normal total read volume and bandwidth — it is a latency amplification of blocking calls at the host/hypervisor layer; the serial path is largely immune because the stdio large-block `fread` makes ~500× fewer syscalls. The systematic component on the code side has been eliminated (see §7 Round 4).
5. **Parallel path has no per-row fault tolerance**: a rejected record stops the pipeline; the leader logs
   the same `Parse error Record N ...` WARNING lines as the serial path (the record is also appended to
   `PARSE_BADFILE`) and then fails the statement, rolling the whole load back. This is a deliberate
   all-or-nothing contract — see §10 and USER_MANUAL §11; the single-process path keeps
   `PARSE_ERRORS` semantics unchanged.
6. **No ordering guarantee**: chunked round-robin routing does not guarantee physical insertion order (the heap table itself does not either).
7. **Encoding assumption**: the parallel path assumes the input encoding = database encoding and does not call `CheckerConversion()`.

---

## 9. Multi-Version Compatibility Verification (2026-11-10)

For PostgreSQL **15.19 / 16.15 / 17.11 / 19rc1**, the **acceptance** testing was performed with "separate cluster + separate build" for each,
the script being [`util/fl_ver_test.sh`](../util/fl_ver_test.sh) (for 18.6, see §3–§5).

All five majors are built from **one tree, branch `master`**: every difference is a `PG_VERSION_NUM`
branch, and majors below 15 are rejected at compile time. Re-confirmed on 2026-11-10 by exporting
`master` with `git archive` and building each major from that export (`INSTALL_OK` on all five,
`CREATE EXTENSION` and a serial-vs-parallel digest comparison on each); evidence
`compat_master_pg1x.out`.

| Environment | Value |
|---|---|
| Build | In-tree `make clean && make && make install`, with `PG_CONFIG` pointing to the target version prefix |
| Cluster | A temporary `initdb` per version, port `54<major>`, `max_worker_processes=32`, `fsync=off` |
| Data | 200,000-row CSV (including `"q,%d"` quoted field) + the same number of 100-byte fixed-width records |
| Timing | No performance comparison, only verifying that content and behavior are correct |

| Case | Assertion | 15.19 | 16.15 | 17.11 | 19rc1 |
|---|---|---|---|---|---|
| CSV `PARALLEL=OFF` vs `w2/w4/w8/auto` | row count 200,000 and digests match | PASS | PASS | PASS | PASS |
| Fixed-width `TYPE=BINARY` `OFF` vs `w4/w8` | row count at the 10.7M level matches, digests match | PASS | PASS | PASS | PASS |
| `FILTER` downgrade | serial and `PARALLEL=ON` digests match + `LOGFILE` shows `PARALLEL disabled (FILTER` | PASS | PASS | PASS | PASS |
| Parse fault tolerance | By default stops at the 1st bad record (keeping already-loaded rows); `PARSE_ERRORS=2` keeps 2 good records | PASS | PASS | PASS | PASS |
| CLI client | `pg_fastload -h -p -U -d x.ctl` exit code 0, loads 200,000 rows | PASS | PASS | PASS | PASS |
| Server log | No `PANIC` / segfault | PASS | PASS | PASS | PASS |

Two real defects found and fixed during the process:

1. **PG19 `FuncnameGetCandidates()` output parameter**: 19 added an `int *fgc_flags` that is dereferenced unconditionally;
   the original implementation passed `NULL`, causing any `FILTER` / `TYPE=FUNCTION` load to segfault directly (backend process terminated by signal 11,
   cluster enters recovery). Fixed by passing a one-off `int[1]` compound literal.
2. **`MyBackendId` removed**: PG19 removed the entire `BackendId` API; loading `pg_fastload.so` reported
   `undefined symbol: MyBackendId`. Replaced with the `FastloadBackendNumber` macro (uses `MyProcNumber` for ≥17).

Additionally, one documentation error was verified and corrected: when `PARSE_ERRORS` is exceeded, the actual behavior is to **stop reading and return normally**
(keeping already-loaded rows), not the "load fails" stated in the original manual.

Per-kernel differences and handling: see [PORTING.md](PORTING.md).

---

## 10. Differential Verification against pg_bulkload (2026-11-10)

Goal: prove that `pg_fastload` is behaviourally identical to the upstream it was forked from, instead
of only being self-consistent (serial vs parallel).

| Item | Value |
|---|---|
| Oracle | upstream `pg_bulkload` 3.1.23 (HEAD `798e0f2`) built against the **same** server, same cluster, same table |
| Comparison | SRF result row (`skip`/`count`/`parse_errors`/`duplicate_new`/`duplicate_old`) + content digest `count(*)\|\|coalesce(sum(hash_record_extended(t,0)),0)` + severity of emitted messages |
| Configurations | `pgbulkload.pg_bulkload`, ours `PARALLEL=OFF`, `PARALLEL=ON PARALLEL_WORKERS=2`, `PARALLEL=ON PARALLEL_WORKERS=4` |
| Suites | 3 lab suites, 75 cases → 225 configuration comparisons, plus the portable [`util/fl_diff_test.py`](../util/fl_diff_test.py) (44 cases × 4 configurations, 132 comparisons) that reproduces the core of the audit on any host with both extensions installed |

Case coverage: quoting / escaping / custom `DELIMITER`+`QUOTE`+`ESCAPE` / CRLF / lone CR / blank and
whitespace-only lines / missing and extra fields / unterminated quotes / `NULL` strings /
`FORCE_NOT_NULL` / `ENCODING` / type-conversion failures (int, numeric, date) / `SKIP`+`LIMIT`
windows including windows crossing a quoted record / `LIMIT=0` / `SKIP` beyond EOF / duplicates on a
unique index and on a primary key with `ON_DUPLICATE_KEEP=NEW|OLD` / `WRITER=BUFFERED` /
`WRITER=PARALLEL` / `MULTI_PROCESS` / `PARSE_ERRORS=0` and `=2` / 30k-200k-row inputs / records larger
than the 1 MiB read chunk / a closing quote, and a 3-byte UTF-8 character, straddling the chunk
boundary / fixed-length loads with `SKIP`, `LIMIT`, `SKIP`+`LIMIT` and a trailing partial record.

| Configuration | Result (identical on each of the four majors) |
|---|---|
| `PARALLEL=OFF` (75 cases) | **75/75 identical** to the oracle (result row, digest and message severity) |
| `PARALLEL=ON` w=2 / w=4 (150 cases) | **120/150 identical**, 30 documented atomic aborts (rejected-record cases: same WARNING, empty table instead of the oracle's "rows read before the error") |
| Total mismatches | **0** (225 comparisons per major, 900 in total) |

Four defects were found and fixed by this audit (parallel error absorption, `SKIP` line-vs-record
semantics, nondeterministic superset after the first rejected record, reader buffer growth on a long
skipped line); details in [CHANGES.md](CHANGES.md#17-upstream-parity-audit-and-fixes-2026-11-10).

Runs were repeated on PG **15.19 / 16.15 / 17.11 / 18.6** with identical results (900 comparisons in
total). PG **19rc1** has no oracle: upstream 3.1.23 does not compile against it
(`nodes.h: unknown type name 'Datum'` — the relocation our `bin/pgut/pgut-list.h` fix handles), so
19rc1 is covered by the per-major acceptance script (§9) plus the 90/90 and 44/44 suites.

Regression after the fixes: `fastload_verify.py` **90/90 PASS**, `fastload_verify_fixed.py`
**44/44 PASS**, `fl_ver_test.sh` PASS on 15.19 / 16.15 / 17.11 / 19rc1.

---

## 11. Progress View Verification (2026-11-10)

The load progress has to satisfy one property that no `NOTICE` can: it must be readable **while the
loading session is busy**. So the checks below all start a load in one connection and read
`pgfastload.pg_fastload_progress` from another one.

Shipped harness: `util/fl_progress_test.py` (8 checks per major, 3 M-row generated CSV), run on
**15.19 / 16.15 / 17.11 / 18.6 / 19rc1**. Raw cross-session evidence: `_t_view.py` on PG 18.6 over the
1 GiB TPC-H `lineitem.csv` (7,375,042 rows), plus `_t_stale.py`, which `SIGKILL`s a running loader.

| Check | Result |
|---|---|
| Default (no keyword) | running load visible from a second session: `datname`, `relname = public.flprog`, `input_type = CSV`, `mode = serial`, `state = running`, `interval_ms = 1000`, `total_bytes` = file size; 0 rows after the load |
| `PROGRESS=OFF` | view empty for the whole load, nothing written under `$PGDATA/pg_fastload/progress/` |
| `PROGRESS_INTERVAL=100` alone | publishing enabled; rows climb (2.3 M..2.8 M observed mid-load on the 3 M-row file), percent agrees with the byte ratio within 1 pp |
| `PARALLEL=ON` w=2 | `mode = parallel`; same row count as serial |
| `LIMIT=1000000` | `total_rows = 1000000`, `percent = rows / total_rows`, `rows` never exceeds `total_rows`; the load stops after exactly 1 M rows |
| `PROGRESS_NOTICE=ON` | NOTICE stream restored on top of the view (4 lines at 100 ms); closing line `100.0% | done` carries the exact row count returned by the function; **off by default** (0 NOTICE lines in every other case) |
| `PROGRESS_INTERVAL=50` | rejected: `PROGRESS_INTERVAL must be between 100 and 3600000 milliseconds` |
| Monotonicity / bounds | `rows` and `percent` never decrease, `percent` inside `0..100` |
| Cross-session, 1 GiB serial | poll 0 → 1,117,697 rows (15.2%), poll 5 → 6.5 M; `state = running` throughout; `-- after: rows in view = 0` |
| Cleanup | record deleted on success; `state = 'stale'` reported once and then deleted for a killed backend |
| Cost (serial 3 M rows, median of 3 interleaved runs) | 0.434-0.442 s off vs 0.434-0.442 s on (**−0.2%..+1.9%**, i.e. noise), same on 15/16/17/19 |

No case produced a mismatched row count, a stale record left behind, or a row in the view while
`PROGRESS=OFF`. The CSV 90/90 and fixed-length 44/44 suites are unaffected (the record is published
outside the row path and cannot fail a load).

---

## 12. Export Verification (2026-11-10)

Shipped script: `util/fl_export_test.sh` (PG 18.6, `PGPORT`/`PGUSER`/`PGDATABASE` from the
environment). **39 assertions, all PASS, exit code 0.** Raw output: `export_pg18.out`.

| Group | Checks | Result |
|---|---|---|
| CSV equality | **`cmp`-identical** to `COPY (SELECT * FROM exp_t) TO ... (FORMAT csv, HEADER)`; returns 5 | PASS |
| CSV round trip | reload with `SKIP=1`, digest `5\|-1683897567441736485` equals the source | PASS |
| CSV dialect | `DELIMITER=\| QUOTE=' NULL=\N`: embedded newline, embedded delimiter/quote, and a literal `\N` kept distinct from NULL; digest preserved | PASS |
| `QUERY=` | `WHERE id >= 3 ORDER BY id DESC` honoured (first data row id=5, 3 rows); `LIMIT=2` returns 2 rows and writes 2 lines | PASS |
| Empty table | returns 0, 0-byte file | PASS |
| BINARY layout | 7 columns, 44 B/row × 4 rows = **176 B**; the sample `.ctl` `COL` lines match the `OUT_COL` declarations one for one | PASS |
| BINARY round trip | reload with `COL=...CHAR(8)`, digest `4\|-5144772349234490677` equals the source | PASS |
| Sample `.ctl` | carries `# exported from public.exp_bin` and keeps `OUTPUT` = file name (not the source table, so a careless run cannot append back into it) | PASS |
| Padding semantics | the same `.bin` reloaded with `COL=VARCHAR(8)` turns `'xy'` into `'xy      '` (length 8) — documented behaviour, matching `Read_varchar` | PASS |
| FIXED round trip | text-only layout (numbers carried in `CHAR(n)`) reloads to the source digest | PASS |
| NULLIF | 3-row table with NULLs, `OUT_COL=INTEGER NULLIF 00000000` / `CHAR(4) NULLIF '    '`, round-trip digest `3\|3014503705147286095` | PASS |
| Error paths | missing `OUTPUT`, relative path, `BINARY` without `OUT_COL`, `FIXED`+`INTEGER`, `OUT_COL` count mismatch, existing target, NULL without `NULLIF`, unknown keyword, no `TABLE`/`QUERY`, non-`SELECT` `QUERY`, `DELIMITER=QUOTE` — 11 rejections, all as expected | PASS |
| Progress | during an 8M-row export another session reads `export\|running\|CSV\|serial\|true\|-\|true\|/tmp/.../big.csv`; the view is empty afterwards; output > 100 MB with 8,000,000 lines | PASS |

Throughput (`public.exp_big`, 8M rows / 521 MB table / 312 MiB CSV, page cache, median of 5):

| Operation | Time |
|---|---|
| `SELECT count(*)` (16-core parallel seq scan) | 0.05 s |
| `COPY ... TO '/dev/null' (FORMAT csv)` | 0.70 s |
| `COPY ... TO '<file>' (FORMAT csv)` | 0.80 s |
| `pg_fastload_export(TYPE=CSV)` → `/dev/shm` | 0.86 s |
| `pg_fastload_export(TYPE=CSV)` → disk | **0.90 s** |
| `pg_fastload_export(TYPE=BINARY)` → disk | 0.94 s |
| `pg_fastload_export(QUERY=SELECT id ...)` | 0.49 s |
| load the same CSV back, `PARALLEL=OFF` | 1.06 s |
| load the same CSV back, `PARALLEL_WORKERS=4` | 0.48 s |

The outputs are `cmp`-identical (326,888,903 B with header). The export does not participate in
parallelism; that is a design choice, not an unfinished item (see
[ARCHITECTURE §4.1](ARCHITECTURE.md#41-the-other-direction-export-data-flow)). The decomposition shows
why: at 0.90 s the export is CPU-bound inside one backend, so 15 of the 16 cores stay idle, and the
file write itself costs 0.04 s. Evidence: `export_perf.out`, `export_perf2.out`, `export_perf_bin.out`;
numbers and reading in [BENCHMARK §8](BENCHMARK.md#8-export-throughput-pg_fastload_export-vs-copy-to).

Regression: the export only adds a writer and a byte counter and leaves the load path untouched;
`fastload_verify.py` and `fastload_verify_fixed.py` were re-run afterwards and still report 90/90 and
44/44.

Multi-version: the export code reaches `INSTALL_OK` on **15.19 / 16.15 / 17.11 / 18.6 / 19rc1**. PG 19
did expose one real difference: `TimestampTzGetDatum()` is declared in `utils/timestamp.h`, which
`utils/builtins.h` pulled in indirectly up to 18 but no longer does in 19, so `export.c` failed with
an `implicit declaration`; an explicit include fixes it. Functional acceptance was run on 18.6 only,
matching the verification scope of the rest of the project.

### 12.1 Parallel export (2026-11-10)

Shipped script: `util/fl_export_parallel_test.sh`. **33 assertions per major**, run on all five:
**15.19 / 16.15 / 17.11 / 18.6 / 19rc1 → 165/165 PASS**, exit code 0 on each. Raw output:
`export_par_15.out` … `export_par_19.out`, plus `_run_harness.out` for 18.6.

| Group | Checks | Result |
|---|---|---|
| Serial baseline | `PARALLEL=OFF`: 200,000 rows reported, 200,001 lines, reload digest equals the source | PASS |
| Parallel CSV (4 workers) | 200,000 rows, 200,001 lines, header exactly once (`id,pad`), reload digest equals the source, **row set** equals the serial output (`sort` + `cmp`), log says `workers=4` | PASS |
| `LIMIT` across workers | `LIMIT=1000` → exactly 1000 rows / 1001 lines / 1000 distinct ids (no duplication across block ranges) | PASS |
| Default worker count | no `PARALLEL_WORKERS` → 200,000 rows, digest matches, log says `workers=15` (online CPUs − 1) | PASS |
| Parallel BINARY | 200,000 rows, file 7,200,000 B (200,000 × 36), sample `.ctl` present and carrying `# exported from public.pe_src`, reload digest equals the source | PASS |
| Fallbacks | `QUERY=` → serial and logs `PARALLEL disabled`; 10-row table → serial and logs it; `PARALLEL_WORKERS=1` → serial and logs `workers=1` | PASS |
| Rejected inputs | existing `OUTPUT` (writer reports `could not create output file`), existing `.ctl`, `PARALLEL_WORKERS=0` | PASS |
| Live progress | while a 1M-row parallel export runs, another session reads `parallel\|export\|running`; the view is empty afterwards; 1,000,001 lines | PASS |

A real defect was found by this harness and fixed: `PARALLEL_WORKERS=1` was reaching the "default" branch
(0 is the sentinel) and silently running `CPUs − 1` workers instead of taking the serial path. The
`workers=1 falls back to the serial path` assertion now pins it.

Scaling (`public.lineitem`, 7,375,042 rows, 1.13 GiB CSV, output on tmpfs, median of 5):

| Config | Time | vs serial |
|---|---|---|
| `COPY ... TO` (server side) | 3.956 s | 1.03× |
| `PARALLEL=OFF` | 4.077 s | 1.00× |
| 2 / 4 / 8 workers | 1.950 / 0.999 / 0.611 s | 2.09× / 4.08× / 6.68× |
| auto (15 workers) | **0.424 s** | **9.60×** |

Correctness at full size: the 4-worker output reloaded into a `LIKE` copy of the source gives the same
order-independent digest (`7375042|9258128314029119699254`).

Why tmpfs: on-disk runs on this host measured serial 4.06 s vs 31.0 s parallel, and a bare
`write()`+`fsync()` microbenchmark (no PostgreSQL) reproduced `fsync()` alternating between ~0.1 s and
exactly 30 s / 60 s at an unchanged 256 KiB block size. The hypervisor's storage stalls dominate any
`fsync()`-inclusive timing here — the same artefact documented for the load path. Evidence:
`export_par_bench_shm.out`, `export_par_disk_ab.out`, `_wtest.txt`.

Load-side suites are unaffected by all of this: CSV 90/90 and fixed-length 44/44 re-run clean.

---

## 13. `SKIP_INDEX` and Export Progress Verification (2026-11-10)

`verify_skipindex_export.py` on PG 18.6, **20/20 PASS**:

- **S1 (drop + rebuild round trip)**: a table with two plain btree indexes loads with
  `SKIP_INDEX=ON` — the NOTICE reports 2 dropped indexes, the two verbatim `CREATE INDEX`
  statements are printed, `pg_index` confirms 0 indexes remain during load, row count 500,000
  and the order-independent digest match the source; re-running the printed DDLs restores
  exactly 2 indexes.
- **S2 (guard)**: `SKIP_INDEX=ON` + primary-key table + `DUP_ERRORS` → `ERROR ... cannot be
  combined with DUP_ERRORS`, load refused.
- **S3 (no-index no-op)**: `SKIP_INDEX=ON` on an index-less table loads normally with no
  drop NOTICE; digest matches.
- **P1 (export percent/ETA)**: a `TABLE=` CSV export with `PROGRESS_NOTICE=ON` streams
  `progress: NN.N% of N rows` lines with ETA, closing `100.0% | done`, output byte-size
  consistent with the source.
- **P2 (QUERY= export)**: percent stays absent (rows/rate lines only), LIMIT-derived
  percentages still work.

Regressions after the change: `fastload_verify.py` **90/90**, `fastload_verify_fixed.py`
**44/44**; build rc=0, zero warnings in added code.

## 14. Conclusion

The current version meets the delivery criteria across nine dimensions: functional correctness (CSV 90/90, fixed-width 44/44, `MULTI_PROCESS` combination 8/8, export 39/39, parallel export 33/33 on each of five majors), upstream parity (225 differential comparisons against pg_bulkload 3.1.23 with 0 mismatches), load progress (8/8 view checks on five majors, exact final numbers, no measurable cost), export (CSV/BINARY/FIXED with CSV byte-identical to `COPY` on the serial path; 9.60× parallel scaling), build (release + debug both rc=0 with zero warnings in added code), performance (CSV ~4.5× over serial, fixed-width 1.82×), memory lifecycle (all palloc, no malloc-family symbols), multi-version compatibility (PG 15/16/17/19 all PASS), and copyright compliance (retained original copyright + explicit attribution and non-endorsement).

The residual risks can all be attributed to environmental limitations or explicit design trade-offs, as documented in §8 and [OPTIMIZATION.md](OPTIMIZATION.md), along with corresponding usage recommendations (e.g., worker-count selection for cold-cache scenarios, using `PARALLEL=OFF` when fault tolerance is needed).
