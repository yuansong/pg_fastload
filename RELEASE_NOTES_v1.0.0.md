# pg_fastload 1.0.0

**Multi-process parallel bulk loader and exporter for PostgreSQL 15 – 19.**

Released: 2026-11-09 · First public release · BSD 3-Clause

`pg_fastload` keeps pg_bulkload's single-process loading pipeline and adds a parallel path on
top of native PostgreSQL shared memory: one reader scans the input once, N background workers
parse records and form tuples, and the backend that issued the load still does every insert —
inside the caller's transaction.

简体中文版本：[RELEASE_NOTES_v1.0.0.zh-CN.md](https://github.com/yuansong/pg_fastload/blob/master/RELEASE_NOTES_v1.0.0.zh-CN.md)

---

## Highlights

| # | What | Measured |
|---|---|---|
| 1 | Parallel load that stays one transaction: stages ①②③ in N bgworkers, ④ serial in the caller — no second connection, no early visibility, same atomicity as serial | 5.81 s → **1.37 s (4.24×)**, 1 GiB / 7.37 M rows, 16 vCPU — **6.07× `COPY ... FROM`** |
| 2 | The input file is read exactly once, so the speedup does not invert when the data outgrows the page cache | 1× read at every worker count |
| 3 | The serial path is the fastest available: quote-free records take a bulk `memchr` split instead of the byte-at-a-time state machine | **1.22× upstream pg_bulkload**, **1.43× `COPY ... FROM`** |
| 4 | Never a silent semantic change: 13 conditions fall back to serial with one `INFO` line, and a bad row fails the load instead of keeping a timing-dependent subset | 13 downgrade cases, all-or-nothing |
| 5 | Drop-in from pg_bulkload: same control keywords, `TYPE` values, log format and `WRITER` modes | rename two identifiers |
| 6 | Both directions in one engine: `pgfastload.pg_fastload_export()` writes CSV / BINARY / FIXED with the same Writer and `COL` / `OUT_COL` layout | **9.60×** parallel export vs serial, **9.33× vs `COPY ... TO`**; CSV byte-identical to `COPY ... TO` |
| 7 | `SKIP_INDEX=ON` drops the target's secondary indexes inside the load transaction and prints the verbatim `CREATE INDEX` rebuild DDLs — the manual drop-and-recreate workflow as one keyword, rolled back on load failure | ≈1.9 s/GiB back on an indexed table (~half of a DIRECT load) |
| 8 | Every load *and* export is trackable: one cluster-wide progress view with percent/ETA (exports estimate the total from `reltuples`), and export logs record the MVCC snapshot fingerprint | `pgfastload.pg_fastload_progress`, `PROGRESS_NOTICE=ON` |
| 9 | Benchmarked against *every* upstream writer mode, not just `DIRECT` — upstream's fastest mode is `WRITER=PARALLEL` | **1.58×** the best upstream mode, **4.4–4.6×** its slowest |

## Feature summary

**Loading** — CSV and fixed-length (`BINARY` / `FIXED`) in parallel; `TUPLE` / `FUNCTION` on the
serial path; `PARALLEL=ON` with `PARALLEL_WORKERS=1..64` (default: online CPUs − 1);
`WRITER=DIRECT / BUFFERED / PARALLEL`; `SKIP` / `LIMIT`, `DUPLICATE_*`, `FILTER`,
`FORCE_NOT_NULL`, `NULLIF`, `ENCODING`, `TRUNCATE=YES`, `MULTI_PROCESS`; per-row tolerance with
`PARSE_ERRORS` + `PARSE_BADFILE` on the serial path; `SKIP_INDEX=ON` for
drop-and-rebuild index handling.

**Export** — `pgfastload.pg_fastload_export()` with `TABLE=` or `QUERY=`, `TYPE=CSV / BINARY /
FIXED`, `OUT_COL`, `HEADER`, `LIMIT`, `DELIMITER` / `QUOTE` / `ESCAPE` / `NULL`; parallel by
default for `TABLE=`, always serial for `QUERY=`.

**Observability** — every load publishes progress, readable from any session through
`pgfastload.pg_fastload_progress` (`percent`, byte/row rates, `eta_seconds`), or streamed with
`PROGRESS_NOTICE=ON`; exports use the same view — a `TABLE=` export shows `percent`/`eta_seconds`
from the `reltuples` estimate, and the export log records the snapshot fingerprint
(`xmin/xmax/cid`) for auditability; `USE_PROFILE=1` builds print the stage groups
`GLOBAL / READER / CSV / WRITER / MERGE / PARALLEL / WORKER`; every log carries per-worker row
counts and `CPU x/y sec elapsed z sec`.

**Operations** — `pg_fastload x.ctl` CLI (`-o KEY=VALUE` overrides, `-r` recovery) and
`pgfastload.pg_fastload(ARRAY[...])`; one `master` branch for PostgreSQL 15 / 16 / 17 / 18 / 19,
with the majors isolated behind `PG_VERSION_NUM` at compile time.

## Performance (1 GiB / 7.37 M rows, 16 vCPU, warm cache, `DIRECT`, no indexes)

| Configuration | Time | vs serial | vs `COPY ... FROM` |
|---|---|---|---|
| `PARALLEL=OFF` | 5.81 s | 1.00× | 1.43× |
| `PARALLEL=ON, PARALLEL_WORKERS=2` | 2.76 s | 2.11× | 3.01× |
| `PARALLEL=ON, PARALLEL_WORKERS=4` | 1.58 s | 3.68× | 5.27× |
| `PARALLEL=ON` (15 workers) | **1.37 s** | **4.24×** | **6.07×** |

Reference points on the same file: upstream pg_bulkload 3.1.23 6.98 s, `COPY ... FROM` 8.32 s,
`COPY` into an UNLOGGED table 7.51 s. Fixed-length 1 GiB: 1.62 s serial → 0.89 s with 8 workers.
Export 7.4 M rows / 1.13 GiB: 4.08 s serial → 0.42 s with 15 workers (9.33× `COPY ... TO`).

Method and all caveats: [docs/BENCHMARK.md](https://github.com/yuansong/pg_fastload/blob/master/docs/BENCHMARK.md).

## Verification

- `util/fastload_verify.py` **90/90** and the fixed-length suite **44/44**, including quoted,
  lone-CR and cross-chunk records on both the serial and the parallel path.
- Differential audit against pg_bulkload 3.1.23 as the oracle: 45 cases × 4 configurations,
  **0 mismatches** on PG 15 / 16 / 17 / 18.
- Export suites: 39 assertions serial and 33 per major parallel (**165/165** across five majors).
- Progress suite (`fl_progress_test.py`) passes on 15.19 / 16.15 / 17.11 / 18.6 / 19rc1.
- Details: [docs/VERIFICATION.md](https://github.com/yuansong/pg_fastload/blob/master/docs/VERIFICATION.md),
  [docs/PORTING.md](https://github.com/yuansong/pg_fastload/blob/master/docs/PORTING.md).

## Install

```bash
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config
sudo make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
```

```sql
CREATE EXTENSION pg_fastload;
```

`shared_preload_libraries` is not required. Keep `max_worker_processes ≥ PARALLEL_WORKERS + 1`.

## Upgrade from pg_bulkload

Rename `pg_bulkload` → `pg_fastload` and `pgbulkload` → `pgfastload`; control files are reusable
as-is. The table and heap format are unchanged, so no data migration is involved.

## Deliverables

- Source: this tag (`git archive --format=zip v1.0.0`)
- Binary bundle: `pg_fastload_v1.0.0_deliverables.zip` — the source tree, a prebuilt release and
  debug `.so`, the acceptance tools and their evidence. Its digest is recorded in
  `fastload_deliverables_sha256.txt`, shipped alongside the archive and re-verified by the packer
  (`make_fastload_deliverables.py`) on every build.

## License and provenance

BSD 3-Clause. A derivative implementation built on
[pg_bulkload](https://github.com/ossc-db/pg_bulkload) 3.1.23 (`798e0f2`): the upstream
`COPYRIGHT` is preserved byte for byte and every inherited file keeps its NTT, Inc. header.
Neither NTT, Inc. nor the pg_bulkload contributors endorse or sponsor this work.
