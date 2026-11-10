# pg_fastload 1.0.0

**PostgreSQL 15 – 19 的多进程并行批量加载与导出扩展。**

发布日期：2026-11-09 · 首个公开版本 · BSD 3-Clause

`pg_fastload` 保留 pg_bulkload 的单进程加载管道，并在其之上用 PostgreSQL 原生共享内存实现了
一条并行通路：单个 reader 只扫一遍输入，N 个后台进程解析记录、成型元组，而所有插入仍由发起
加载的 backend 完成 —— 并且发生在调用方的事务里。

English version: [RELEASE_NOTES_v1.0.0.md](https://github.com/yuansong/pg_fastload/blob/master/RELEASE_NOTES_v1.0.0.md)

---

## 核心优势

| # | 能力 | 实测 |
|---|---|---|
| 1 | 并行装载仍然只有一个事务：①②③ 交给 N 个 bgworker，④ 留在调用方串行 —— 不额外开连接、未提交的行不会被看到、原子性与串行一致 | 5.81 s → **1.37 s（4.24×）**，1 GiB / 737 万行，16 vCPU |
| 2 | 输入文件只读一遍，因此数据超出 page cache 后加速比也不会倒挂 | 任意 worker 数下均为 1× 读取 |
| 3 | 串行路径本身就是最快的：无引号记录用 `memchr` 批量切分，不再走逐字节状态机 | **上游 pg_bulkload 的 1.20×**、**`COPY ... FROM` 的 1.46×** |
| 4 | 不会悄悄改变语义：13 种情况自动降级到串行并打一行 `INFO`，坏行让整次装载失败，而不是留下取决于时序的子集 | 13 类降级、全有或全无 |
| 5 | 从 pg_bulkload 平滑迁移：控制文件关键字、`TYPE` 取值、日志格式、`WRITER` 模式全部一致 | 只需改两个标识符 |
| 6 | 双向一个引擎：`pgfastload.pg_fastload_export()` 用同一套 Writer 与 `COL` / `OUT_COL` 布局写出 CSV / BINARY / FIXED | 并行导出 **9.60×**；CSV 与 `COPY ... TO` 逐字节相同 |

## 功能清单

**装载** —— CSV 与定长（`BINARY` / `FIXED`）均可并行；`TUPLE` / `FUNCTION` 走串行路径；
`PARALLEL=ON` 配 `PARALLEL_WORKERS=1..64`（默认 = 在线 CPU 核数 − 1）；`WRITER=DIRECT /
BUFFERED / PARALLEL`；`SKIP` / `LIMIT`、`DUPLICATE_*`、`FILTER`、`FORCE_NOT_NULL`、`NULLIF`、
`ENCODING`、`TRUNCATE=YES`、`MULTI_PROCESS`；串行路径支持 `PARSE_ERRORS` + `PARSE_BADFILE`
逐行容错。

**导出** —— `pgfastload.pg_fastload_export()` 支持 `TABLE=` 或 `QUERY=`、`TYPE=CSV / BINARY /
FIXED`、`OUT_COL`、`HEADER`、`LIMIT`、`DELIMITER` / `QUOTE` / `ESCAPE` / `NULL`；`TABLE=` 默认
并行，`QUERY=` 始终串行。

**可观测性** —— 每次装载都会发布进度，任何会话都能通过 `pgfastload.pg_fastload_progress` 读取
（`percent`、字节/行速率、`eta_seconds`），也可用 `PROGRESS_NOTICE=ON` 直接收 `NOTICE`；
`USE_PROFILE=1` 构建输出 `GLOBAL / READER / CSV / WRITER / MERGE / PARALLEL / WORKER` 分段；
日志包含每个 worker 的产出行数与 `CPU x/y sec elapsed z sec`。

**运维** —— `pg_fastload x.ctl` 命令行（`-o KEY=VALUE` 覆盖、`-r` 恢复）与
`pgfastload.pg_fastload(ARRAY[...])` 两个等价入口；单条 `master` 分支服务 PostgreSQL 15 / 16 /
17 / 18 / 19，版本差异全部在编译期用 `PG_VERSION_NUM` 隔离。

## 性能（1 GiB / 737 万行，16 vCPU，热缓存，`DIRECT`，无索引）

| 配置 | 耗时 | 相对串行 |
|---|---|---|
| `PARALLEL=OFF` | 5.81 s | 1.00× |
| `PARALLEL=ON, PARALLEL_WORKERS=2` | 2.76 s | 2.11× |
| `PARALLEL=ON, PARALLEL_WORKERS=4` | 1.58 s | 3.68× |
| `PARALLEL=ON`（15 worker） | **1.37 s** | **4.24×** |

同一文件的参照点：上游 pg_bulkload 3.1.23 7.00 s、`COPY ... FROM` 8.48 s、`COPY` 进 UNLOGGED 表
7.67 s。定长 1 GiB：串行 1.62 s → 8 worker 0.89 s。导出 740 万行 / 1.13 GiB：串行 4.08 s →
15 worker 0.42 s。

方法与全部前提说明：[docs/BENCHMARK.zh-CN.md](https://github.com/yuansong/pg_fastload/blob/master/docs/BENCHMARK.zh-CN.md)。

## 验证

- `util/fastload_verify.py` **90/90**，定长套件 **44/44**，覆盖带引号、孤立 CR、跨块记录在串行与
  并行两条路径上的行为。
- 以上游 pg_bulkload 3.1.23 为 oracle 的差分审计：45 个用例 × 4 种配置，PG 15 / 16 / 17 / 18
  上 **0 处不一致**。
- 导出套件：串行 39 个断言、并行每个主版本 33 个断言（五个主版本合计 **165/165**）。
- 进度套件（`fl_progress_test.py`）在 15.19 / 16.15 / 17.11 / 18.6 / 19rc1 上全部通过。
- 详见 [docs/VERIFICATION.zh-CN.md](https://github.com/yuansong/pg_fastload/blob/master/docs/VERIFICATION.zh-CN.md)、
  [docs/PORTING.zh-CN.md](https://github.com/yuansong/pg_fastload/blob/master/docs/PORTING.zh-CN.md)。

## 安装

```bash
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config
sudo make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
```

```sql
CREATE EXTENSION pg_fastload;
```

不需要 `shared_preload_libraries`；请保证 `max_worker_processes ≥ PARALLEL_WORKERS + 1`。

## 从 pg_bulkload 升级

把 `pg_bulkload` 换成 `pg_fastload`、`pgbulkload` 换成 `pgfastload` 即可，控制文件原样复用。
表与堆格式没有变化，不涉及数据迁移。

## 交付物

- 源码：本标签（`git archive --format=zip v1.0.0`）
- 二进制包：`pg_fastload_v1.0.0_deliverables.zip` —— 源码树、预编译 release 与 debug `.so`、
  验收工具与其证据。摘要记录在同级 `fastload_deliverables_sha256.txt` 中，由打包脚本
  （`make_fastload_deliverables.py`）在每次构建时重新校验。

## 许可与来源

BSD 3-Clause。本项目是基于 [pg_bulkload](https://github.com/ossc-db/pg_bulkload) 3.1.23
（`798e0f2`）的二次实现：上游 `COPYRIGHT` 逐字节保留，每个继承文件都保留 NTT, Inc. 的版权头。
NTT, Inc. 与 pg_bulkload 贡献者均不为本项目背书。
