# pg_fastload Documentation Index

| Document | English | Chinese |
|---|---|---|
| Project overview | [../README.md](../README.md) | [../README.zh-CN.md](../README.zh-CN.md) |
| User manual | [USER_MANUAL.md](USER_MANUAL.md) | [USER_MANUAL.zh-CN.md](USER_MANUAL.zh-CN.md) |
| Architecture | [ARCHITECTURE.md](ARCHITECTURE.md) | [ARCHITECTURE.zh-CN.md](ARCHITECTURE.zh-CN.md) |
| Portability (PG 15–19) | [PORTING.md](PORTING.md) | [PORTING.zh-CN.md](PORTING.zh-CN.md) |
| Changes vs pg_bulkload | [CHANGES.md](CHANGES.md) | [CHANGES.zh-CN.md](CHANGES.zh-CN.md) |
| Benchmarks | [BENCHMARK.md](BENCHMARK.md) | [BENCHMARK.zh-CN.md](BENCHMARK.zh-CN.md) |
| Verification report | [VERIFICATION.md](VERIFICATION.md) | [VERIFICATION.zh-CN.md](VERIFICATION.zh-CN.md) |
| Optimization ledger | [OPTIMIZATION.md](OPTIMIZATION.md) | [OPTIMIZATION.zh-CN.md](OPTIMIZATION.zh-CN.md) |

English files are the primary (canonical) versions; the `.zh-CN.md` files are their Chinese translations.

## Diagram

| File | Description |
|---|---|
| [img/architecture.svg](img/architecture.svg) | Overall architecture (English) |
| [img/architecture.zh-CN.svg](img/architecture.zh-CN.svg) | Overall architecture (Chinese) |

## About `pg_timestamp`

`util/pg_timestamp` is an optional utility shipped with this project: running `util/pg_timestamp.sql` replaces the input function (`pg_type.typinput`) for `timestamp` with `pg_timestamp_in`, thereby skipping the text timestamp parsing overhead during loading.

Note: this is an **instance-wide global change** that affects text parsing of all `timestamp` values on that instance; `util/uninstall_pg_timestamp.sql` can revert it. It is not part of the parallel path and was not modified in this version.
