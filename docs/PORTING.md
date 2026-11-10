# pg_fastload portability (PostgreSQL 15 – 19)

> **Bottom line**: the same source tree builds, runs `CREATE EXTENSION` and passes the same
> functional acceptance suite on PostgreSQL **15.19 / 16.15 / 17.11 / 18.6 / 19rc1**. Every
> version difference is confined to `#if PG_VERSION_NUM` branches and to the version-pinned
> nbtree builder — there is **no forked business logic**.
>
> Verified on 2026-11-10 with [`../util/fl_ver_test.sh`](../util/fl_ver_test.sh).
>
> 中文版：[PORTING.zh-CN.md](PORTING.zh-CN.md)

---

## 1. Support matrix

| Major | Verified minor | Status |
|---|---|---|
| 15 | 15.19 | build + functional suite pass |
| 16 | 16.15 | build + functional suite pass |
| 17 | 17.11 | build + functional suite pass |
| 18 | 18.6 (HEAD `1da6ddbdc3`) | build + functional suite pass; benchmark numbers come from this major |
| 19 | 19rc1 | build + functional suite pass |

Branch policy: **one branch, `master`.**

- Every supported major is built from the same tree. There is no `REL_1x_STABLE` per-major branch and
  no cherry-picking between majors: a fix is written once and reaches all five.
- All version differences are resolved at compile time — `#if PG_VERSION_NUM` branches plus the
  version-pinned nbtree builder listed in §3.1. Nothing is selected at run time or by build flags
  you have to remember.
- `include/pg_fastload.h` refuses anything below 15 with `#error`. An unsupported major therefore
  fails at compile time, instead of producing a binary that breaks later.
- The five per-major branches that earlier releases published were pointers to one and the same
  commit; they carried no code difference. They were removed in favour of `master` (see
  [CHANGES.md](CHANGES.md) §19).

## 2. Building

```bash
# PostgreSQL 15 shown; <prefix> is the installation prefix of that major
make USE_PGXS=1 PG_CONFIG=<prefix>/bin/pg_config
make USE_PGXS=1 PG_CONFIG=<prefix>/bin/pg_config install
```

**Note**: `make clean` needs the same `USE_PGXS=1 PG_CONFIG=...`. If no `pg_config` is on `PATH`,
a bare `make clean` never loads PGXS, so **object files are not deleted** and switching majors
silently reuses `.o` files built for another server. The symptom is `undefined symbol` when
running `CREATE EXTENSION`; the acceptance script had exactly this bug and now passes `PG_CONFIG`
to `clean` as well.

One install produces:

| Artifact | Location |
|---|---|
| `pg_fastload.so` | `<prefix>/lib/` |
| `pg_fastload.control`, `pg_fastload--1.0.0.sql` | `<prefix>/share/extension/` |
| `pg_fastload` (CLI client) | `<prefix>/bin/` |
| `pg_timestamp.so` (optional helper) | `<prefix>/lib/` |

## 3. Version differences and how they are handled

Every core difference actually met between 15 and 19 is listed below; none is left open.

| # | Core change | Introduced | Affected code | Handling |
|---|---|---|---|---|
| 1 | `populate_compact_attribute()` added; TupleDesc now precomputes attribute metadata | 18 | `fl_build_tupdesc()` (worker-side TupleDesc) | call only on ≥18; earlier majors compute it on demand |
| 2 | `RelationSetNewRelfilenode()` → `RelationSetNewRelfilenumber()` | 16 | index rebuild path | existing `pgut-be.h` macro renames per major |
| 3 | `BackendId` → `ProcNumber` (`rd_backend`, `RelFileLocatorBackend.backend`) | 17 | temp relation / `smgr` locator | `FastloadBackendNumber` / `FastloadInvalidBackend` macros |
| 4 | `MyBackendId` and the whole `BackendId` API removed | 19 | same as #3 | same macros: ≥17 uses `MyProcNumber` / `INVALID_PROC_NUMBER` |
| 5 | `Datum` moved from `c.h` into `postgres.h` (unavailable to frontend code) | 19 | CLI client (`bin/pgut/pgut-list.h` pulls in `nodes/pg_list.h`) | frontend header supplies `typedef uint64_t Datum;` |
| 6 | `PageXLogRecPtr` is a single 64-bit value in CPU byte order instead of `{xlogid, xrecoff}` | 19 | `IsPageCreatedByLoader()` in the recovery tool | use `PageXLogRecPtrGet()` |
| 7 | `get_database_name()` moved from `commands/dbcommands.h` to `utils/lsyscache.h` | 19 | `fastload_parallel.c`, `writer_parallel.c` | pick the header per major |
| 8 | `FuncnameGetCandidates()` gained a `int *fgc_flags` out-parameter that is **dereferenced unconditionally** | 19 | `ParseFunction()` (`FILTER` / `TYPE=FUNCTION`) | pass a one-element compound literal; passing `NULL` segfaults |
| 9 | `Item` typedef removed; `PageAddItemExtended()` takes `const void *`; `PageSetChecksumInplace()` → `PageSetChecksum()` | 19 | `DirectWriterInsert()` and page checksum fill-in | version branch; ≥19 passes the pointer directly |
| 10 | nbtree internals refactored: `BTSpool`/`BTWriteState` no longer exported, new `palloc0_object()` macros, `table_beginscan_parallel()` gained `SO_NONE` | 19 | index spool builder | new `lib/nbtree/nbtsort-19.c` (see §3.1) |
| 11 | `table_beginscan()` gained a trailing `uint32 flags` (`ScanOptions`) argument | 19 | `FlExportQueryWorker()` in `export_parallel.c` | `FL_TABLE_BEGINSCAN()` macro passes `0` on ≥19 and keeps the 4-argument form below |
| 12 | `TimestampTzGetDatum()` no longer arrives via `utils/builtins.h` | 19 | `export.c` | explicit `#include "utils/timestamp.h"` |

### 3.1 Version-pinned nbtree builders

`WRITER=DIRECT` bypasses the buffer manager, so the loader has to build indexes itself in bulk.
The project therefore vendors one copy of PostgreSQL's `nbtsort.c` per major (inherited from
pg_bulkload, not a new decision):

| File | Covers |
|---|---|
| `lib/nbtree/nbtsort-9.5.c` … `nbtsort-9.6.c` | 9.5 – 9.6 |
| `lib/nbtree/nbtsort-10.c` … `nbtsort-17.c` | 10 – 17 |
| `lib/nbtree/nbtsort-18.c` | 18 |
| `lib/nbtree/nbtsort-19.c` | 19 |
| `lib/nbtree/nbtsort-common.c` | shared `_bt_spoolinit()` for 14 and later |

`lib/pg_btree.c` `#include`s exactly one of them behind `#if PG_VERSION_NUM`, so it all compiles
into a single translation unit. The 18 → 19 delta measured only 59 lines (`const` qualifiers,
`palloc*_object` macros, the `PageAddItem` parameter type), so adding 19 is mostly review work
rather than a rewrite.

## 4. How to verify

The script works per major with a throw-away cluster that is destroyed afterwards:

| Step | Detail |
|---|---|
| Build | `make clean` (with `PG_CONFIG`) → `make` → `make install` |
| Cluster | `initdb` + `port=54<major>`, `max_worker_processes=32`, `fsync=off` (test only) |
| Data | 200,000 CSV rows (with quoted fields) and the same number of 100-byte binary records |
| Case A | CSV: `PARALLEL=OFF` / `w2` / `w4` / `w8` / `auto`; row count and content digest compared per config |
| Case B | Fixed length: `PARALLEL=OFF` / `w4` / `w8`, same comparison |
| Case C | `FILTER` downgrade: serial and `PARALLEL=ON` agree, and the `LOGFILE` contains `PARALLEL disabled (FILTER` |
| Case D | Parse errors: default `PARSE_ERRORS=0` stops at the first bad record (rows already loaded are kept); `PARSE_ERRORS=2` loads both good records |
| Case E | CLI: `pg_fastload -h -p -U -d xxx.ctl` exits 0 and loads the right number of rows |
| Case F | Server log without `PANIC` / segfault |

The content digest is `count(*) || sum(hash_record_extended(t,0))`, which is independent of
physical insert order, so serial and parallel output can be compared directly.

## 5. Results (2026-11-10)

| Major | A: CSV serial/parallel | B: fixed serial/parallel | C: FILTER downgrade | D: parse errors | E: CLI | F: log | Verdict |
|---|---|---|---|---|---|---|---|
| 15.19 | 5/5 digests match | 3/3 match | match + notice present | 1 / 2 rows | rc=0, 200,000 rows | no PANIC | **PASS** |
| 16.15 | 5/5 digests match | 3/3 match | match + notice present | 1 / 2 rows | rc=0, 200,000 rows | no PANIC | **PASS** |
| 17.11 | 5/5 digests match | 3/3 match | match + notice present | 1 / 2 rows | rc=0, 200,000 rows | no PANIC | **PASS** |
| 18.6 | see [VERIFICATION.md](VERIFICATION.md) (90/90 + 44/44) | same | same | same | same | no PANIC | **PASS** |
| 19rc1 | 5/5 digests match | 3/3 match | match + notice present | 1 / 2 rows | rc=0, 200,000 rows | no PANIC | **PASS** |

The digest values (CSV and fixed length) are identical on all four majors, which shows the parallel
path produces byte-for-byte the same content as the serial path.

The export suites run on all five as well: `util/fl_export_test.sh` 39/39 and
`util/fl_export_parallel_test.sh` 33/33 per major (**165/165** for the parallel one), so the parallel
export — background workers, DSM, `heap_setscanlimits()` — is exercised on every major, not only 18.6.

## 6. Known limitations

- **Parallel index build (W-3) is not implemented**, independent of the server version: with
  indexes present the index build still runs serially in the leader. See the rejected-ideas
  section of [OPTIMIZATION.md](OPTIMIZATION.md).
- **PostgreSQL 19 is a pre-release** (`19rc1`). The port targets PostgreSQL 19 (`REL_19_STABLE`); if nbtree
  internals change again before GA, `lib/nbtree/nbtsort-19.c` must be re-reviewed.
- Majors below 15 still have `#if` branches in the source but are **not tested** by this project
  and are not claimed as supported.
