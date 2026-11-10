# pg_fastload User Manual

> Version: `pg_fastload 1.0.0` (a derivative implementation built on pg_bulkload 3.1.23)
> Target database: PostgreSQL 18.x (this release is fully verified on 18.6 only)
> Chinese version: [USER_MANUAL.zh-CN.md](USER_MANUAL.zh-CN.md)

---

## Contents

1. [Overview](#1-overview)
2. [Installation and upgrade](#2-installation-and-upgrade)
3. [Quick start](#3-quick-start)
4. [Conceptual model](#4-conceptual-model)
5. [Control file syntax](#5-control-file-syntax)
6. [Keyword reference](#6-keyword-reference)
7. [SQL interface](#7-sql-interface)
8. [Command line](#8-command-line)
9. [Input formats](#9-input-formats)
10. [Parallel mode](#10-parallel-mode)
11. [Error handling and badfiles](#11-error-handling-and-badfiles)
12. [Logging and diagnostics](#12-logging-and-diagnostics)
13. [Performance tuning](#13-performance-tuning)
14. [Troubleshooting](#14-troubleshooting)
15. [Known limitations](#15-known-limitations)
16. [Migrating from pg_bulkload](#16-migrating-from-pg_bulkload)
17. [Upgrade and uninstall](#17-upgrade-and-uninstall)
18. [Exporting table data (`pg_fastload_export`)](#18-exporting-table-data-pg_fastload_export)

---

## 1. Overview

`pg_fastload` is a high-speed bulk loader for PostgreSQL. A load consists of four stages:

| Stage | Work | Serial path | Parallel path (`PARALLEL=ON`) |
|---|---|---|---|
| ① Read input | pull bytes from file/STDIN | leader backend | one reader bgworker |
| ② Parse records | record boundaries + field splitting | leader backend | N workers |
| ③ Form tuples | input functions → `heap_form_tuple()` | leader backend | N workers |
| ④ Insert | `WriterInsert()` | leader backend | leader backend (unchanged) |

**Without `PARALLEL`, behaviour is identical to pg_bulkload.** With `PARALLEL=ON`, only ①②③ are
parallelised; ④ plus all transaction, constraint and error semantics stay where they were.
`PARALLEL=ON` is therefore a switch you can leave on: whenever the parallel path cannot guarantee
identical semantics, the load silently falls back to the serial path and logs one INFO line.

The opposite direction has an entry point too: `pgfastload.pg_fastload_export()` writes a table or
the result of any `SELECT` to a CSV / BINARY / FIXED file, using the same Writer and the same
`COL`/`OUT_COL` layout description, so an exported fixed-width file loads back unchanged
(see [§18](#18-exporting-table-data-pg_fastload_export)).

### When to use it

- Large initial loads (hundreds of thousands of rows or more) where ①②③ dominate and the host has
  several cores;
- The input file is on local storage or in page cache, and the format is CSV or fixed-length;
- The target table can tolerate an `AccessExclusiveLock` for the duration of the load.

### When not to use it

- You need per-row tolerance (bad rows to a badfile, load continues): use `PARALLEL=OFF`;
- Input comes from `STDIN` or a pipe: `PARALLEL=ON` downgrades automatically;
- `TYPE=TUPLE` / `FUNCTION`: not parallelised;
- Single-core or dual-core hosts: gains are near zero, prefer `PARALLEL=OFF`.

---

## 2. Installation and upgrade

### 2.1 Requirements

| Item | Requirement |
|---|---|
| PostgreSQL | server development package (`postgresql-server-dev-18` or a source tree) with a working `pg_config` |
| Compiler | GCC / Clang, C99 |
| Other | `libpq` (build time, used by the `WRITER=PARALLEL` second connection), `pthread` |
| GUC | `max_worker_processes ≥ PARALLEL_WORKERS + 1` (≥ 32 recommended with the default 15 workers) |

`shared_preload_libraries` is **not** required.

### 2.2 Build and install (PGXS)

```bash
tar xzf pg_fastload-1.0.0.tar.gz
cd pg_fastload
make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config
sudo make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config install
```

| Artifact | Location |
|---|---|
| `pg_fastload.so` | `$(pg_config --pkglibdir)/` |
| `pg_fastload.control`, `pg_fastload--1.0.0.sql` | `$(pg_config --sharedir)/extension/` |
| `pg_fastload` (CLI) | `$(pg_config --bindir)/` |
| `pg_timestamp` (optional tool) | `$(pg_config --bindir)/` |

> `lib/pg_fastload.control` and `lib/pg_fastload--*.sql` are build products generated from the
> `.in` templates by the Makefile; their absence from the repository is expected.

### 2.3 Create the extension

```sql
CREATE EXTENSION pg_fastload;
```

This creates schema `pgfastload` and function `pgfastload.pg_fastload(text[])`.
The loader runs as a superuser context; grant usage on the schema to other roles explicitly.

### 2.4 Directories and permissions

- The load runs **as the database server**, so the `INPUT` file must be readable by the `postgres`
  OS user (the client's own permissions are irrelevant).
- Log and status files default to `$PGDATA/pg_fastload/` (derived from the server's working
  directory).
- `DIRECT` mode writes data files directly and maintains
  `$PGDATA/pg_fastload/{dboid}.{relid}.loadstatus` for crash recovery.

---

## 3. Quick start

### 3.1 Which machine holds the control file

This is the most common trap when migrating from pg_bulkload, so the answer comes first:

| Usage | Control file | Data file (`INPUT`) | Why |
|---|---|---|---|
| **SQL interface**<br>`SELECT pgfastload.pg_fastload(ARRAY[...])` | **none needed** | **the database server** | keywords are inline in the array; the file is opened by the server backend / reader process |
| **Command line**<br>`pg_fastload xxx.ctl` | **the client machine**<br>(where you run it) | **the database server** | the CLI only reads the control file; the load itself runs on the server |

Three consequences:

1. **The SQL interface has no control file.** Putting a `.ctl` file on the client does nothing —
   the server never sees it and never reads it.
2. **`INPUT` is always a server-side path** and must be readable by the database OS user
   (usually `postgres`). A client-local path (`C:\data\a.csv`, `/home/me/a.csv`) fails with
   `could not open file`.
3. **Relative paths in a control file are expanded on the client before being sent.** So when the
   control file is on the client and the data file is on the server, write the server's absolute
   path rather than a relative one.

> Rule of thumb: **the control file follows the client, the data file follows the server, and the
> SQL interface has no control file at all.**


### 3.2 Control file

`order_line.ctl`:

```
OUTPUT            = public.order_line
INPUT             = /tmp/tpcc/order_line.csv
TYPE              = CSV
WRITER            = DIRECT
DELIMITER         = ','
TRUNCATE          = YES
PARALLEL          = ON
```

```bash
pg_fastload -d tpcc order_line.ctl
```

The summary printed to the terminal is also written to the log file.

### 3.3 SQL

```sql
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/tmp/tpcc/order_line.csv',
  'OUTPUT=public.order_line',
  'TYPE=CSV',
  'WRITER=DIRECT',
  'TRUNCATE=YES',
  'PARALLEL=ON'
]);
```

Returns one row:

| Column | Meaning |
|---|---|
| `skip` | records skipped by `SKIP` |
| `count` | records loaded |
| `parse_errors` | parse errors (only the serial path can report > 0) |
| `duplicate_new` | duplicate keys where the new row was kept |
| `duplicate_old` | duplicate keys where the old row was kept |
| `system_time` / `user_time` | server CPU time (seconds) |
| `duration` | server elapsed time (seconds) |

### 3.4 Minimal parallel example

```
OUTPUT   = public.t
INPUT    = /data/t.csv
PARALLEL = ON
```

When `PARALLEL_WORKERS` is omitted the loader uses *online CPUs − 1*. To see the effective value,
run with `VERBOSE=YES` and look for `PARALLEL=ON, TYPE=CSV, workers=N` in the log, or
`PARALLEL disabled (...)` if it downgraded.

---

## 4. Conceptual model

### 4.1 Three independent dimensions

| Dimension | Keyword | Values | Notes |
|---|---|---|---|
| Input format | `TYPE` | `CSV` / `BINARY` / `FIXED` / `TUPLE` / `FUNCTION` | `FIXED` is an alias of `BINARY`; both use the fixed-length parser |
| Write path | `WRITER` | `DIRECT` / `BUFFERED` / `PARALLEL` | `PARALLEL` means `DIRECT` + `MULTI_PROCESS=YES` |
| Execution mode | `PARALLEL` | `ON` / `OFF` | only affects whether ①②③ run in multiple processes |

They compose: `TYPE=CSV` + `WRITER=DIRECT` + `PARALLEL=ON` means "parallel parsing, direct heap write".

### 4.2 The three `WRITER` modes

| Mode | Write path | Transaction | Use when |
|---|---|---|---|
| `DIRECT` (default) | bypasses shared buffers, writes data files + LSF, builds indexes in one batch afterwards | caller's transaction | large initial loads; fastest |
| `BUFFERED` | regular buffer manager + WAL | caller's transaction | conservative choice needing normal crash safety; slower than `DIRECT` |
| `PARALLEL` | `DIRECT` plus a second backend (own libpq connection + SysV IPC queue) doing the inserts | **separate transaction** | the upstream parallel-write scheme; it does not parallelise the insert itself and breaks the caller's `BEGIN/COMMIT` atomicity, so it is rarely advisable |

`WRITER=PARALLEL` and `PARALLEL=ON` are **never stacked**: when both are given, the reader stays
serial and logs an INFO line (avoiding two competing process fan-outs).

### 4.3 Parallel execution model

```
leader (the backend that issued the load)
  ├─ parse options / create DSM / register bgworkers
  ├─ ④ WriterInsert() (serial, one tuple at a time)
  └─ aggregate statistics, clean up
reader bgworker (one, no database connection)
  └─ single pass over the input → SKIP/LIMIT trimming → chunk round-robin dispatch
worker bgworkers × N
  └─ split fields → input functions → heap_form_tuple → send back
```

Protocol details and design trade-offs: [ARCHITECTURE.md](ARCHITECTURE.md).

---

## 5. Control file syntax

```
# comment: whole line starting with #
KEY = VALUE
KEY = 'VALUE WITH SPACES'
```

| Item | Rule |
|---|---|
| Separator | `=`, any surrounding whitespace |
| Case | keywords are case-insensitive; values usually too (paths excepted) |
| Comments | `#` to end of line (outside quotes) |
| Quoting | single quotes when the value contains spaces or `#`; no escapes inside |
| Unknown keyword | error `invalid keyword "XXX"` (never silently ignored) |
| Duplicate keyword | error (`ASSERT_ONCE`) |
| Empty value | error `option "XXX" has no value` |

Every keyword can also be supplied on the command line with `-o KEY=VALUE`
(see [§8](#8-command-line)) or as an element of the SQL array.

---

## 6. Keyword reference

### 6.1 General

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `TYPE` | `CSV` / `BINARY` / `FIXED` / `TUPLE` / `FUNCTION` | `CSV` | input format |
| `WRITER` / `LOADER` | `DIRECT` / `BUFFERED` / `PARALLEL` | `DIRECT` | write path |
| `MULTI_PROCESS` | `YES` / `NO` | `NO` | boolean form of `WRITER=PARALLEL` |

### 6.2 Input

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `INPUT` / `INFILE` | path or `STDIN` | required | input file; `STDIN` uses the client encoding |
| `SKIP` / `OFFSET` | integer ≥ 0 | `0` | skip the first N **input lines** (`\n`, `\r\n` or a lone `\r` ends a line; quote marks are not honoured, so a multi-line quoted record counts as several lines — same rule as pg_bulkload) |
| `LOAD` / `LIMIT` | integer | unlimited | load at most N records (counted after `SKIP`) |
| `ENCODING` | encoding name | database encoding | input encoding; requires per-field conversion |

### 6.3 Output and transaction

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `OUTPUT` / `TABLE` | `schema.table` | required | target table; table name only, **no database name** (the connection decides) |
| `TRUNCATE` | `YES` / `NO` | `NO` | truncate the target before loading |
| `VERBOSE` | `YES` / `NO` | `NO` | more detailed progress logging |

### 6.4 CSV parsing (`TYPE=CSV`)

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `DELIMITER` | single character | `,` | field separator |
| `QUOTE` | single character | `"` | quote character |
| `ESCAPE` | single character | same as `QUOTE` | escape character; equal to `QUOTE` means the RFC4180 `""` form |
| `NULL` | string | empty string | field text meaning SQL NULL (compared **before** unquoting) |
| `FORCE_NOT_NULL` | comma-separated column names | none | treat the `NULL` string as a literal value for these columns |
| `FILTER` | function name | none | per-field rewrite (`func(text,text)`); significantly slower |
| `SKIP` / `OFFSET` | integer | `0` | as above (CSV parser level) |

CSV semantics: [§9.1](#91-csv).

### 6.5 Fixed-length parsing (`TYPE=BINARY` / `FIXED`)

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `COL` | `name type [NULLIF 'pattern']` | required | one `COL` per column; types below |
| `STRIDE` | integer > 0 | sum of field lengths | bytes per record (including padding) |
| `PRESERVE_BLANKS` | `YES` / `NO` | `NO` | keep trailing blanks in `CHAR`/`VARCHAR` |
| `FILTER` | function name | none | as CSV |

`COL` types:

| Class | Names | Length |
|---|---|---|
| Text | `CHAR` / `CHARACTER`, `VARCHAR` / `CHARACTER VARYING` | delimited by `NULLIF` or to the end of the field |
| Signed integers | `SMALLINT`(`SHORT`), `INTEGER`(`INT`), `BIGINT`(`LONG`) | 2 / 4 / 8 native bytes |
| Unsigned integers | `UNSIGNED SMALLINT`(`UNSIGNED SHORT`), `UNSIGNED INTEGER`(`UNSIGNED INT`) | 2 / 4 bytes |
| Floating point | `FLOAT`(`REAL`), `DOUBLE` | 4 / 8 bytes |

Numeric fields are decoded from **native binary bytes** (C cast), with no string parsing — the main
reason fixed-length loading is several times faster than CSV.

### 6.6 Error handling

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `PARSE_ERRORS` / `MAX_ERR_CNT` | integer, `-1` = unlimited | `0` | tolerated parse errors; **exceeding it stops reading** (rows already loaded are kept and the call returns normally, without an error) |
| `PARSE_BADFILE` | path | `{log-basename}.prs.{ext}` | where failed records are written |
| `DUPLICATE_ERRORS` | integer, `-1` = unlimited | `0` | tolerated duplicate-key errors |
| `DUPLICATE_BADFILE` | path | `{log-basename}.dup.csv` | where duplicate rows are written |
| `ON_DUPLICATE_KEEP` | `NEW` / `OLD` | `NEW` | keep the new or the old row on conflict |
| `CHECK_CONSTRAINTS` | `YES` / `NO` | `NO` | validate target CHECK constraints |

### 6.7 Logging

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `LOGFILE` | path | `$PGDATA/pg_fastload/YYYYMMDDHHMMSS_{db}_{nsp}_{tbl}.log` | load log |

### 6.8 Parallel (new in pg_fastload)

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `PARALLEL` | `ON` / `OFF` | `OFF` | enable the multi-process parallel load |
| `PARALLEL_WORKERS` | `1`..`64` | online CPUs − 1 (≥ 1) | number of parser workers; `≤ 1` falls back to serial |

### 6.9 Progress (new in pg_fastload)

| Keyword | Values | Default | Notes |
|---|---|---|---|
| `PROGRESS` | `ON` / `OFF` | `ON` | publish a status record for this load, readable by every session through `pgfastload.pg_fastload_progress` |
| `PROGRESS_INTERVAL` | `100`..`3600000` (ms) | `1000` | minimum gap between two published records; **specifying it implies `PROGRESS=ON`** |
| `PROGRESS_NOTICE` | `ON` / `OFF` | `OFF` | additionally stream a `NOTICE` line to the loading client (and `LOGFILE`) |

See [§12.4](#124-load-progress) for the view, its columns and the cost.

---

## 7. SQL interface

```sql
CREATE EXTENSION pg_fastload;

SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/t.csv',
  'OUTPUT=public.t',
  'TYPE=CSV',
  'PARALLEL=ON'
]);
```

- The argument is a `text[]` of `KEY=VALUE` items, identical to control-file keywords.
- The function is `VOLATILE STRICT` and runs **inside the caller's transaction**; rows are not
  visible to others until you `COMMIT`.
- On failure it raises `ERROR` and the transaction rolls back (the `DIRECT` path may leave an LSF
  record behind — see [§14](#14-troubleshooting)).
- The server reads the file as the function owner (usually a superuser), so the file only needs to
  be readable by `postgres`.

Combining several loads in one transaction:

```sql
BEGIN;
SELECT pgfastload.pg_fastload(ARRAY['INPUT=/data/a.csv','OUTPUT=public.t','PARALLEL=ON']);
SELECT pgfastload.pg_fastload(ARRAY['INPUT=/data/b.csv','OUTPUT=public.t','PARALLEL=ON']);
COMMIT;
```

Exporting is a second function with the same signature, except that it writes a file rather than a
table:

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.t',
  'OUTPUT=/data/t.csv',
  'TYPE=CSV'
]);
-- returns bigint: rows written
```

Note that `TABLE` is the **source** here (it is the target of a load) and `OUTPUT` is a **file**
(it is a table name for a load). Full keywords, format details and round-trip semantics are in
[§18](#18-exporting-table-data-pg_fastload_export).

---

## 8. Command line

```
pg_fastload [options] [control-file]
```

Without a control file, every parameter must come from `-o`.

| Option | Long form | Meaning |
|---|---|---|
| `-i` | `--infile` / `--input` | same as `INPUT=`, overrides the control file |
| `-O` | `--output` | same as `OUTPUT=` |
| `-l` | `--logfile` | same as `LOGFILE=` |
| `-P` | `--parse-badfile` | same as `PARSE_BADFILE=` |
| `-u` | `--duplicate-badfile` | same as `DUPLICATE_BADFILE=` |
| `-o` | `--option` | append any `KEY=VALUE`; repeatable, highest priority |
| `-D` | `--pgdata` | `$PGDATA` for recovery mode |
| `-r` | `--recovery` | recovery mode: handle interrupted `DIRECT` loads via their LSF files |
| `-d` / `-U` / `-h` / `-p` | | connection parameters; fall back to `PGDATABASE` / `PGUSER` / `PGHOST` / `PGPORT` |
| `-e` / `-E` | | echo executed SQL / set error level (shared pgut options) |
| `--help` | | usage |

Examples:

```bash
# everything on the command line
pg_fastload -d tpcc \
  -o INPUT=/data/t.csv \
  -o OUTPUT=public.t \
  -o PARALLEL=ON -o PARALLEL_WORKERS=8

# control file plus overrides
pg_fastload -d tpcc base.ctl -o SKIP=1000000 -o LIMIT=100000
```

**Exit status**: `0` on success, non-zero on failure (check `$?`).

---

## 9. Input formats

### 9.1 CSV

| Feature | Behaviour |
|---|---|
| Record terminator | `\n`, `\r\n`, lone `\r` (same as serial) |
| Newline inside quotes | part of the field, does not end the record |
| Escaping | `ESCAPE` defaults to `QUOTE`, so `""` is one literal quote; when `ESCAPE ≠ QUOTE`, `ESCAPE` + char is used |
| Empty field | empty text `''` (two adjacent delimiters) → `NULL` (with the default empty `NULL` string) |
| Quoted empty | `""` has raw length 2 and is **not** the empty NULL string → loaded as an empty string |
| No trailing newline | the last record is still produced |
| Wrong field count | error (serial: counted as a parse error, may go to the badfile; parallel: the load fails) |
| Record size cap | 256 MiB |

### 9.2 Fixed-length (`BINARY` / `FIXED`)

```
TYPE   = FIXED
STRIDE = 100
COL    = id      INTEGER
COL    = name    VARCHAR NULLIF ','
COL    = amount  DOUBLE
```

- Records are exactly `STRIDE` bytes; fields follow the `COL` declaration order;
- `CHAR`/`VARCHAR` need a `NULLIF` terminator (or run to the end of the field/record);
- Trailing bytes shorter than one record at EOF are ignored with a WARNING (same as serial);
- The parallel path requires whole records (no continuation of a partial record).

### 9.3 `TUPLE`

`TYPE=TUPLE` reads a binary tuple stream produced by pg_fastload's own `WRITER=BINARY` export. It is
meant for internal fast movement/recovery, not as a general import format.

### 9.4 `FUNCTION`

`TYPE=FUNCTION` produces tuples row by row from a SQL function; useful to load query results, and it
does not participate in parallelism.

---

## 10. Parallel mode

### 10.1 Enabling

```
PARALLEL         = ON
PARALLEL_WORKERS = 8      # optional
```

### 10.2 Choosing the worker count

| Situation | Suggestion |
|---|---|
| Default | omit `PARALLEL_WORKERS` (CPUs − 1) |
| File much larger than RAM (cold cache) | start with the default; if you see stalls, try 4–8 |
| Target table has several indexes | more workers still helps (index build stays serial in the leader) |
| Single/dual core | `PARALLEL=OFF` |
| Shared host | set it explicitly and leave headroom |

### 10.3 Resource usage

| Resource | With the default 15 workers |
|---|---|
| bgworker slots | 16 (15 parsers + 1 reader) → `max_worker_processes ≥ 16`, 32 recommended |
| DSM shared memory | ≈ 144 MiB (input queues 8 MiB × 15 + return queues 2 MiB × 15 + control blocks) |
| Per-process memory | one `AllocSet` per worker, proportional to record size, 256 MiB per record cap |

### 10.4 Automatic downgrade (slower, never broken)

`PARALLEL=ON` falls back to the serial path and logs one INFO line in these cases:

| # | Condition | INFO line shape |
|---|---|---|
| 1 | effective workers ≤ 1 | `PARALLEL disabled (workers=1)` |
| 2 | `TYPE` is `TUPLE` / `FUNCTION` | `PARALLEL disabled (TYPE=...)` |
| 3 | `FILTER` / `FORCE_NOT_NULL` given | `PARALLEL disabled (FILTER)` etc. |
| 4 | target table has dropped columns | `PARALLEL disabled (dropped columns)` |
| 5 | explicit `ENCODING` needing conversion | `PARALLEL disabled (encoding conversion)` |
| 6 | `NOT NULL` columns or `CHECK_CONSTRAINTS` | `PARALLEL disabled (constraints)` |
| 7 | `PARSE_ERRORS != 0` | `PARALLEL disabled (PARSE_ERRORS)` |
| 8 | `INPUT=STDIN` / missing | `PARALLEL disabled (INPUT is not a file)` |
| 9 | `WRITER=PARALLEL` / `MULTI_PROCESS=YES` | `PARALLEL disabled (MULTI_PROCESS / WRITER=PARALLEL ...)` |
| 10 | fixed-length without `COL` / with `STRIDE ≤ 0` | `PARALLEL disabled (fixed-length needs COL/STRIDE)` |
| 11 | fixed-length `NULLIF` pattern > 64 bytes | `PARALLEL disabled (NULLIF longer than 64 bytes)` |
| 12 | fixed-length trailing partial record | load continues, tail ignored (WARNING) |
| 13 | target descriptor unavailable | `PARALLEL disabled (no TupleDesc)` |

> To find out why a load did not go parallel: set `VERBOSE=YES` and grep the log for `PARALLEL`.

### 10.5 Correctness contract

The parallel path **does not guarantee physical row order** (neither does a heap table). Correctness
is defined on the row multiset:

```sql
SELECT count(*) || '|' || sum(hash_record_extended(t, 0)) FROM public.t AS t;
```

Serial and parallel runs must produce the same digest; the verification suite asserts exactly that
(see [VERIFICATION.md](VERIFICATION.md)).

---

## 11. Error handling and badfiles

| Case | Serial path (`PARALLEL=OFF`) | Parallel path (`PARALLEL=ON`) |
|---|---|---|
| Wrong field count / input-function error / unterminated quote | counted in `parse_errors`, written to `PARSE_BADFILE`, load continues; once `PARSE_ERRORS` is exceeded reading **stops and the call returns normally** (log: `Maximum parse error count exceeded`), rows already loaded are kept | the same `Parse error Record N: Input Record M ...` WARNING lines are emitted, then the statement **fails with `ERROR` and the whole load is rolled back** (no partial rows) |
| Duplicate key | written to `DUPLICATE_BADFILE` while ≤ `DUPLICATE_ERRORS`; `ON_DUPLICATE_KEEP` decides the survivor | handled serially by the leader, same behaviour as serial |
| Constraint violation | error, rollback | error, rollback |

Therefore:

- **Use `PARALLEL=OFF` whenever you need tolerant loading**;
- The parallel path targets clean data where speed matters. It is deliberately all-or-nothing on a
  rejected record: rows for a rejected line are already in flight in other workers, so "keep the rows
  read before the error" would depend on worker timing and could load a nondeterministic superset of
  what pg_bulkload loads;
- When omitted, `PARSE_BADFILE` / `DUPLICATE_BADFILE` are derived from the `LOGFILE` basename and
  live in the same directory.

---

## 12. Logging and diagnostics

### 12.1 Load log

```
pg_fastload 1.0.0
...
PARALLEL = ON
PARALLEL_WORKERS = 15
TYPE = CSV
...
worker #0 produced 492013 rows
...
CPU 1.20s/13.40u sec elapsed 1.76 sec
```

| Look for | Meaning |
|---|---|
| `PARALLEL=ON, TYPE=CSV, workers=N` | parallelism really started |
| `PARALLEL disabled (...)` | downgraded; the parentheses give the reason |
| `worker #i produced N rows` | per-worker output (chunk round-robin, not necessarily balanced) |
| `CPU x/y sec elapsed z sec` | server CPU / wall clock |

### 12.2 Stage profiling (a `USE_PROFILE=1` build)

```
make USE_PGXS=1 USE_PROFILE=1 PG_CONFIG=/path/to/pg_config
```

| Group | Sections | Meaning |
|---|---|---|
| `GLOBAL` | `INIT` / `READER` / `WRITER` / `FLUSH` / `MERGE` / `INDEX` / `FINISH` / `PARALLEL` | whole run; `PARALLEL` is the parallel subsystem total |
| `READER` | `SOURCE` / `PARSER` | serial path read/parse |
| `WRITER` | `TOAST` / `TABLE` / `INDEX` | insert breakdown |
| `PARALLEL` | `SETUP` / `WAIT` / `COPY` / `WRITE` / `READ` | orchestration, waiting for workers, copying returned tuples, heap write, reader scan |
| `WORKER` | `SCAN` / `FORM` / `IPC` | worker-side dequeue / tuple forming / messaging (summed across processes) |

The `GLOBAL` sections should add up to roughly `elapsed`; a large gap means an un-instrumented path
(please report it).

> Profiling builds add overhead — **never** use them in production. Production builds contain no
> instrumentation at all.

### 12.3 Process visibility

```sql
SELECT pid, backend_type, state FROM pg_stat_activity WHERE backend_type LIKE '%fastload%';
```

During a parallel load you should see one reader plus N workers.

### 12.4 Load progress

A `pg_fastload()` call is one SQL statement, so without help it stays silent until it returns —
uncomfortable when a 100 GB load runs for half an hour. Asking the loading session to *print*
progress does not help either: that session is the busy one, and its client is blocked on a
statement that has not finished. Progress therefore has to be **published**, and read from
somewhere else:

```sql
-- session 1, busy for the next 30 minutes
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/lineitem.csv',
  'OUTPUT=public.lineitem',
  'PARALLEL=ON'
]);

-- session 2, or a monitoring agent, or another database of the same cluster
SELECT * FROM pgfastload.pg_fastload_progress;
```

```
 pid  | datname |    relname      | input_type |  mode    | state   | elapsed_seconds |  rows   | bytes_read | total_bytes
------+---------+-----------------+------------+----------+---------+-----------------+---------+------------+-------------
 4121 | tpcc    | public.lineitem | CSV        | parallel | running |            3.42 | 3687521 |  536870912 | 1073741824
```

| Column | Meaning |
|---|---|
| `pid` | backend running the load |
| `backend_start`, `started` | session start / load start |
| `datname`, `relname`, `infile` | database, target table, input given to `INPUT` |
| `direction` | `load` or `export`: the same record also covers `pg_fastload_export()` (see [§18](#18-exporting-table-data-pg_fastload_export)) |
| `input_type`, `mode` | resolved `TYPE` (`CSV`, `BINARY`, `FIXED`, …) and `serial` / `parallel` |
| `state` | `running` while the backend is alive, `stale` for a record left by a killed backend |
| `elapsed_seconds` | since `started` |
| `rows` | rows the leader has inserted — the number the statement will report |
| `bytes_read`, `total_bytes` | bytes consumed by the reader / size of the input file (`0` when unknown: STDIN, pipe). For an export `bytes_read` counts bytes **written** and `total_bytes` is always `0` |
| `total_rows` | `LIMIT`, `0` when unlimited |
| `percent` | `rows ÷ total_rows` when `LIMIT` is finite, else `bytes_read ÷ total_bytes`, else `NULL` |
| `bytes_per_second`, `rows_per_second` | averages since the load started |
| `eta_seconds` | remaining work ÷ rate, `NULL` when the percentage is unknown |
| `interval_ms` | publishing cadence |

Notes:

- **Every load publishes by default.** The record is one small file per running load,
  `$PGDATA/pg_fastload/progress/<pid>.status`, rewritten atomically (`.tmp` + rename) at most once
  per `PROGRESS_INTERVAL`. The view simply lists that directory and deletes records whose backend is
  gone, so it is cluster-wide: a load started in `tpcc` is visible from `postgres`, and from any
  other database of the same instance.
- **Nothing is printed** to the loading session, which keeps its output clean. If you also want the
  old client-side stream (or a copy in `LOGFILE` for a load you started hours ago), add
  `PROGRESS_NOTICE=ON`:

  ```
  NOTICE:  pg_fastload progress: start, input 1024.0 MiB, interval 1000 ms
  NOTICE:  pg_fastload progress: 50.0% | rows 3,687,521 | 512.0 MiB / 1024.0 MiB | 136.4 MiB/s | 982.0k rows/s | elapsed 00:00:03 | ETA 00:00:03
  NOTICE:  pg_fastload progress: 100.0% | rows 7,375,042 | 1024.0 MiB / 1024.0 MiB | 137.1 MiB/s | 987.4k rows/s | elapsed 00:00:07 | done
  ```

  `NOTICE` obeys `client_min_messages`, so it disappears at `warning` and above; the view and
  `LOGFILE` do not.
- **`PROGRESS=OFF` publishes nothing** for that load: the view stays empty, and nothing is written
  under `$PGDATA/pg_fastload/progress/`. `PROGRESS_INTERVAL` only tunes the cadence of an already
  published record; specifying it on its own is still a request for progress and turns publishing
  on.
- **The percentage never regresses.** It stops at `99.9%` until the load is done: the reader can be
  at EOF while the leader still has rows to insert, and a mid-load `100.0%` only reads as a stall.
  Where the row count matters more than the byte count, use `LIMIT`, which switches the percentage
  to `rows ÷ LIMIT` — that is what the caller actually asked for.
- **Ends and failures are visible.** A finished load removes its record, so the view goes back to
  `0 rows`; so does a load that aborts on a rejected record or is cancelled. A backend killed with
  `SIGKILL` cannot clean up, and its record is reported once as `state = 'stale'` and then deleted
  by the next reader (or by the next load's startup purge).
- **Cost**: one integer comparison per inserted row in the hot loop; the clock is sampled once every
  512 rows and the file is written at most once per interval. Measured overhead on a 3 M-row serial
  load is below the run-to-run noise (±2%); a 1 KiB status file with no `fsync` is not data.
- **Permissions**: the rows expose database, table and input file names, so `EXECUTE` on
  `pgfastload.fl_progress_rows()` is revoked from `PUBLIC` (the view is the owner's). Grant the view
  to a monitoring role instead of widening the function:

  ```sql
  GRANT SELECT ON pgfastload.pg_fastload_progress TO monitor;
  ```
- A load that aborts on a rejected record (see [§11](#11-error-handling-and-badfiles)) never reports
  a closing line, because it is not done.


---

## 13. Performance tuning

1. **Confirm that ①②③ is the bottleneck.** Use a `USE_PROFILE=1` build and look at `READER.PARSER`
   (serial) or `PARALLEL{READ,WAIT}`. If `WRITE` already dominates, extra workers buy little (the
   serial ④ floor is ~1.3 s per GiB).
2. **Load without indexes.** `DIRECT` builds indexes in one batch after the load (about 8× faster
   than "load then `CREATE INDEX`"). Drop non-essential indexes for bulk imports.
3. **Raise `maintenance_work_mem`** (e.g. 1 GB): it drives spool sorting and index build.
4. **Worker count**: start from the default; with a warm cache 8–15 is already near saturation.
5. **Truncate from a different session** (especially with `MULTI_PROCESS=YES`) to avoid self-locking.
6. **Batch, don't trickle**: every load pays a fixed cost (connection, DSM, worker start-up, tens of
   milliseconds).
7. **Cold cache**: pg_fastload reads the file exactly once, but hypervisor/storage jitter can amplify
   blocking-call latency. Minute-long stalls with idle CPUs are environmental, not an extension defect.

---

## 14. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `PARALLEL mode needs max_worker_processes to cover workers (N) + 1 reader` | not enough worker slots | raise `max_worker_processes` (≥ N+1) and restart |
| No speedup, log shows `PARALLEL disabled` | a downgrade condition | follow the reason in parentheses ([§10.4](#104-automatic-downgrade-slower-never-broken)) |
| Load hangs, `pg_stat_activity` shows a lock wait | `MULTI_PROCESS=YES` self-deadlocking with a `TRUNCATE` in the same session | truncate from another session, or use `TRUNCATE=YES` |
| `cannot load to ...` | target is not a plain table (view / foreign table / partitioned parent) | use a plain table or a concrete partition |
| `pg_fastload only supports tables with "heap" access method` | non-heap AM | change the table type |
| `permission denied for table ...` | missing `INSERT` (or `DELETE` when `DUPLICATE_ERRORS` is used) | grant it |
| `invalid keyword "XXX"` | typo | see [§6](#6-keyword-reference) |
| `INPUT option required` / `OUTPUT option required` | missing mandatory keyword | add it |
| `unterminated CSV quoted field` | unbalanced quote | fix the data, or inspect with `PARALLEL=OFF` |
| `invalid TYPE "XXX"` | unsupported type | use `CSV`/`BINARY`/`FIXED`/`TUPLE`/`FUNCTION` |
| Table is empty after a "successful" load | `LIMIT=0`, `SKIP` beyond the file, or a concurrent truncate | check parameters and concurrency |
| Table unusable after a crashed `DIRECT` load | data files written but never committed; LSF left behind | `pg_fastload -r -D $PGDATA` (cleans up per the LSF record and removes the status file) |
| Load reported success but the application sees no rows | `WRITER=PARALLEL`'s separate transaction, or the caller did not `COMMIT` | check the transaction boundaries |

---

## 15. Known limitations

1. No per-row tolerance on the parallel path ([§11](#11-error-handling-and-badfiles)).
2. Row order is not preserved on the parallel path.
3. The parallel path assumes input encoding = database encoding.
4. `TUPLE` / `FUNCTION` are not parallelised.
5. Fixed-length parallel: `NULLIF` patterns ≤ 64 bytes; records must be whole `STRIDE`.
6. Record size cap: 256 MiB.
7. Requires `max_worker_processes ≥ PARALLEL_WORKERS + 1`.
8. `WRITER=PARALLEL` uses a separate transaction (upstream behaviour) and is not atomic with the caller.
9. Fully verified only on PostgreSQL 18.6 / x86_64 Linux.
10. During a parallel load the target table holds an `AccessExclusiveLock`, excluding any concurrent access.
11. Export ([§18](#18-exporting-table-data-pg_fastload_export)) runs single-process serially; `PARALLEL` has no effect on it.
12. A fixed-width export pads every field: reloading a text column as `CHAR(n)` loses trailing blanks, as
    `VARCHAR(n)` keeps the padding — neither is an exact round trip (see [§18.5](#185-round-trip-semantics-read-this-section)).

---

## 16. Migrating from pg_bulkload

| Item | pg_bulkload | pg_fastload |
|---|---|---|
| Extension | `CREATE EXTENSION pg_bulkload` | `CREATE EXTENSION pg_fastload` |
| SQL function | `pgbulkload.pg_bulkload(...)` | `pgfastload.pg_fastload(...)` |
| CLI | `pg_bulkload` | `pg_fastload` |
| Control file | `KEY = VALUE` | **identical**, reusable as-is |
| Parallelism | `WRITER=PARALLEL` | new `PARALLEL=ON`; `WRITER=PARALLEL` still works |

```sql
-- 1) keep the old extension around for a while (rollback safety)
CREATE EXTENSION pg_fastload;
-- 2) verify one load
SELECT count, duration FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/t.csv','OUTPUT=public.t','PARALLEL=ON']);
-- 3) drop the old extension once satisfied
DROP EXTENSION pg_bulkload;
```

Both can coexist (different schema, extension and shared library names), but never load into the same
table concurrently from both.

---

## 17. Upgrade and uninstall

```sql
-- upgrade within the same major
ALTER EXTENSION pg_fastload UPDATE;

-- remove
DROP EXTENSION pg_fastload;
```

```bash
# remove binaries and extension files
sudo make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config uninstall
```

Uninstalling leaves historical logs and LSF files under `$PGDATA/pg_fastload/`. If an interrupted
`DIRECT` load left an LSF file, run `pg_fastload -r` before deleting anything.

---

## 18. Exporting table data (`pg_fastload_export`)

A load is "file → table"; an export is "table → file". Both share the same `Writer` abstraction, so
the same `OUT_COL` layout describes both the bytes an export produces and the bytes a load expects
to read — that is where every round-trip claim in this manual comes from.

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV'
]);
--  pg_fastload_export
-- --------------------
--              7375042
```

Returns `bigint`: the number of rows written.

### 18.1 Execution model

| Step | Implementation | Note |
|---|---|---|
| Fetch rows | `SPI_cursor_open_with_args(..., read_only)` + `SPI_cursor_fetch()`, 1000 rows per batch | goes through the planner; privileges, visibility and snapshot are those of a plain `SELECT` |
| Format | `CsvWriter` (`lib/writer_csv.c`) or `BinaryWriter` (`lib/writer_binary.c`) | 4 MiB buffer, `fsync` on `close()` |
| Write | `open()` as the **server**, absolute path required | an existing `OUTPUT` is an error, never overwritten |
| Concurrency | single process, serial | see 18.7: the source is a cursor and the target is one file, so there is no read amplification to parallelise |

`TABLE=` expands to `SELECT * FROM <table>` internally; use `QUERY=` to filter, sort or project.
They are mutually exclusive and one is required.

### 18.2 Keywords

| Keyword | Values | Default | Meaning |
|---|---|---|---|
| `TABLE=` | relation name | — | source table, alternative to `QUERY=` |
| `QUERY=` | any `SELECT` | — | read-only cursor; `UPDATE`/`DELETE` are rejected (`cannot open UPDATE query as cursor`) |
| `OUTPUT=` | absolute path | **required** | output file |
| `TYPE=` | `CSV` / `BINARY` / `FIXED` | `CSV` | `BINARY`/`FIXED` are fixed-width layouts and need `OUT_COL` |
| `OUT_COL=` | type declaration, repeatable | — | one per output column, in query order; same syntax as the loader's `COL` |
| `DELIMITER=` `QUOTE=` `ESCAPE=` `NULL=` `HEADER=` | char / boolean | `,` `"` `"` empty `OFF` | `TYPE=CSV` only, same meaning as `COPY ... (FORMAT csv)` |
| `LIMIT=` | ≥ 1 | unlimited | stop after this many rows |
| `LOGFILE=` | absolute path | `<OUTPUT>.log` | export log |
| `PROGRESS=` | `ON` / `OFF` | `ON` | publish a `direction=export` progress record |
| `PROGRESS_NOTICE=` | `ON` / `OFF` | `OFF` | additionally emit `NOTICE` progress lines |
| `PROGRESS_INTERVAL=` | 100..3600000 ms | `1000` | implies `PROGRESS=ON` |

`TYPE` accepts only these three values; `TYPE=TUPLE` / `FUNCTION` are load-side input formats and
have no meaning for an export.

### 18.3 CSV: byte-identical to `COPY`

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV',
  'HEADER=ON']);
```

```
$ psql -c "\copy public.lineitem TO '/tmp/copy.csv' WITH (FORMAT csv, HEADER)"
$ cmp /tmp/copy.csv /data/lineitem.csv && echo identical
identical
```

The quoting rules replicate `CopyAttributeOutCSV()` one by one: a field is quoted when it contains
`DELIMITER`, `QUOTE`, `ESCAPE`, `\n` or `\r`, and the quote character is doubled inside; a field
equal to the `NULL` string is quoted too (otherwise it is indistinguishable from NULL); when the
`NULL` string is empty, an empty field is quoted (`""` and an empty string are two different
values). By default NULL is written as an empty field; `NULL=\N` separates the two, exactly like
`COPY`.

- `HEADER=ON` writes the column names from the result `TupleDesc` (expression columns in `QUERY=`
  may be named `?column?`).
- Empty table without `HEADER` → 0-byte file; empty table with `HEADER=ON` → just the header line.
- Throughput: an 8M-row / 312 MiB table exports in 0.89 s from page cache (`COPY TO` takes 0.95 s,
  and the outputs are `cmp`-identical). Both are single-process sequential writers; the export is
  slightly faster because of its 4 MiB buffer and the absence of a per-row `DestReceiver`.

### 18.4 BINARY / FIXED: fixed-width layout and a sample control file

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.bin',
  'TYPE=BINARY',
  'OUT_COL=INTEGER',      -- l_orderkey
  'OUT_COL=INTEGER',      -- l_partkey
  'OUT_COL=INTEGER',      -- l_suppkey
  'OUT_COL=INTEGER',      -- l_linenumber
  'OUT_COL=DOUBLE',       -- l_quantity
  'OUT_COL=DOUBLE',       -- l_extendedprice
  ...]);
```

| Declaration | Width | Encoding |
|---|---|---|
| `SMALLINT` / `INTEGER` / `BIGINT` | 2 / 4 / 8 | native little-endian binary, same as the loader's `COL` |
| `FLOAT` / `DOUBLE` | 4 / 8 | IEEE 754 |
| `CHAR(n)` / `VARCHAR(n)` / bare `n` | n | text, right-padded with spaces to n bytes |

`TYPE=FIXED` produces exactly the same bytes as `BINARY`; the difference is in what may be
**declared**: `FIXED` is the fixed-width text format and accepts only `CHAR(n)` / `VARCHAR(n)`, so
`OUT_COL=INTEGER` is rejected (`TYPE=FIXED writes text only`). Use `BINARY` for native numeric
fields.

The export also writes a sample control file at `<OUTPUT>.ctl`, ready to be edited:

```ini
INPUT = /data/lineitem.bin
# exported from public.lineitem
OUTPUT = lineitem
...
TYPE = BINARY
COL = INTEGER (4)
...
```

`OUTPUT` is the file name (`lineitem.bin` → `lineitem`), **not** the source table: a sample describes
a file, and the target table is your decision. Defaulting to the source table would make one careless
`pg_fastload lineitem.bin.ctl` append the rows back into it. Export and load share the same layout
code, so a `TYPE=BINARY` sample also loads a text file produced by `TYPE=FIXED` (both use the same
fixed-length reader).

### 18.5 Round-trip semantics (read this section)

| Case | Result |
|---|---|
| numeric / boolean / time columns | `BINARY` round-trips exactly (with matching `OUT_COL` and `COL`) |
| text column reloaded as `COL=CHAR(n)` | exact, but **trailing blanks of the value are lost** (`CHAR` semantics, `Read_char` trims) |
| text column reloaded as `COL=VARCHAR(n)` | the padding is **kept**: `'xy'` in an 8-byte field comes back as `'xy      '` (8 chars) |
| `NULL` | fixed-width formats have no NULL bit; declare `OUT_COL=TYPE NULLIF <pattern>`; a real NULL without `NULLIF` is an error |

The third row is a property of the format, not a defect: a fixed-width field is always full, and the
`VARCHAR` reader keeps the declared width while the `CHAR` reader trims it. So

- to get "reload exactly what you exported", declare text columns `OUT_COL=CHAR(n)` and reload with
  `COL=CHAR(n)`;
- to keep trailing blanks, you need `VARCHAR` plus values that fill the width (`length(v) = n`), or
  `TYPE=CSV`.

`NULL` handling is explicit: without `NULLIF`, a NULL in the data raises
`column N is NULL and OUT_COL gives it no representation` instead of silently writing 0 or spaces.

Widths and ranges are checked before writing: `value too long for type character(n)` and unsigned
overflow raise on the offending row, the statement rolls back, and the partially written file stays
(the `LOGFILE` names the failing column).

### 18.6 Progress

An export publishes into the same view ([§12.4](#124-load-progress)) with `direction = export`,
`infile` set to the output file and `bytes_read` counting written bytes. The total is unknown, so
`percent` and `eta_seconds` are `NULL` while `rows_per_second` and `bytes_per_second` work:

```
 pid  | datname |    relname     | direction | input_type |  mode  |  state  |  rows   | bytes_read
------+---------+----------------+-----------+------------+--------+---------+---------+------------
 4312 | tpcc    | public.lineitem| export    | CSV        | serial | running | 3687521 |  536870912
```

### 18.7 Limitations

1. **Single process, serial.** `PARALLEL` has no effect on an export (no error, no parallelism). For a
   parallel export, split by partition or key range and run several `COPY` statements concurrently.
2. **Read-only, but holds a snapshot**: the scan runs under the caller's snapshot, so a long export
   delays `VACUUM` through `oldest xmin`.
3. **The output must be a server-side, absolute path that does not exist yet.**
4. **Superuser required** (same as a load: writing a file is a server-side action).
5. `TYPE=FIXED` accepts character types only; the number of `OUT_COL` declarations must equal the
   number of output columns (CSV needs no `OUT_COL`).
6. `QUERY=` takes a single `SELECT` (no trailing `;`, no `WITH ... INSERT`).
7. The sample control file's `OUTPUT` is the file name — change it to your target table before
   reloading. The sample always says `TYPE = BINARY`, which also reads `FIXED` text layouts.

---

*Related: [Architecture](ARCHITECTURE.md) · [Changes](CHANGES.md) · [Benchmarks](BENCHMARK.md) · [Verification](VERIFICATION.md) · [Optimization log](OPTIMIZATION.md)*
