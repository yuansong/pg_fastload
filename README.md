# pg_fastload

PostgreSQL 高速批量加载扩展，**基于 [pg_bulkload](https://github.com/ossc-db/pg_bulkload) 3.1.23 二次实现**。

作者：**yuanyuansong**  
许可：BSD 3-Clause（与原项目一致，见 [LICENSE](LICENSE) / [COPYRIGHT](COPYRIGHT) / [NOTICE](NOTICE)）

---

## 它做了什么

原版 `pg_bulkload` 把一次加载分成四个阶段：

| 阶段 | 内容 | 原版位置 |
|---|---|---|
| ① 读 CSV | `SourceRead()` 从文件取字节 | `ReaderNext()` |
| ② 解析 CSV | 记录边界 + 字段切分（状态机） | `ReaderNext()` |
| ③ 形成元组 | 输入函数 → Datum → `heap_form_tuple()` | `ReaderNext()` |
| ④ 写入数据库 | `WriterInsert()`（DIRECT / BUFFERED / PARALLEL） | 主循环 |

原版四阶段在**同一个 backend 进程里串行**。`pg_fastload` 在不改变 ④ 的前提下，把 ①②③ 交给多个独立的 PostgreSQL 后台进程并行执行。

**关键约定**：`PARALLEL=ON` 时类型仍然写成 `TYPE = CSV`，**没有 `CSVMT` 这个类型名**。不指定 `PARALLEL` 时行为与原版完全一致。

---

## 快速开始

### 编译安装

```bash
# 需要 PostgreSQL 18 的 pg_config 在 PATH 中（或显式指定）
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
```

```sql
CREATE EXTENSION pg_fastload;
```

### 控制文件

```
OUTPUT = public.order_line
INPUT = /tmp/tpcc/order_line.csv
TYPE = CSV
WRITER = DIRECT
DELIMITER = ','
PARALLEL = ON
PARALLEL_WORKERS = 2
```

```bash
pg_fastload sample.ctl
```

SQL 入口：

```sql
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/tmp/tpcc/order_line.csv',
  'OUTPUT=public.order_line',
  'TYPE=CSV',
  'WRITER=DIRECT',
  'PARALLEL=ON',
  'PARALLEL_WORKERS=2'
]);
```

---

## 并行参数

| 参数 | 说明 | 默认 |
|---|---|---|
| `PARALLEL` | `ON` 时 `TYPE=CSV / BINARY / FIXED` 走多进程并行路径 | `OFF`（原版串行） |
| `PARALLEL_WORKERS` | 并行解析工作进程数，合法范围 1..64 | **在线 CPU 核数 − 1** |

默认值的理由：④ 写入由主 backend 串行执行、单遍扫描由 reader bgworker 承担，各占一个进程，所以解析进程数取 `CPU − 1`。例如 16 核机器默认 15 个解析 worker + 1 个 reader + 1 个写入 leader，2 核机器默认 1 个（并回退串行路径）。实际取值可用 `nproc` 确认，并需要 `max_worker_processes >= PARALLEL_WORKERS + 1`。

并行 worker 只实现「读 + 解析 + 形成元组」三阶段，因此下列语义仍由原版单进程路径承担；命中任一条即自动**降级回原版单进程路径**并打印一条 INFO，保证原有功能不变：

- `FILTER` / `FORCE_NOT_NULL`；
- 目标表含 dropped 列；
- 显式 `ENCODING` 转码、`CHECK_CONSTRAINTS`、表含 NOT NULL 列（需要 `CheckerConstraints()`）；
- `PARSE_ERRORS != 0` / `PARSE_BADFILE` 的逐行容错；
- `INPUT = STDIN` 或缺失（reader 需要可独立 `open()` 的路径）；
- `TYPE` 不是 CSV / BINARY / FIXED（即 `TUPLE / FUNCTION`）。

完整清单见 [docs/PARALLEL_DESIGN.md](docs/PARALLEL_DESIGN.md#9-并行度与串行回退)。

---

## 架构一览（Plan C 单读多解）

```
                ┌──────────────── 主 backend（leader）────────────────┐
 控制文件 ──▶ ParseOptions ──▶ Reader / Writer ──▶ WriterInsert() ④ 串行
                │                                        ▲
                │  dsm + shm_toc + 双向 shm_mq            │ HeapTuple
                ▼                                        │
   ┌──────── reader bgworker ────────┐    ┌─ worker #0 .. #N-1 ─┐
   │ ① 单遍扫描输入文件               │    │ ② 字段切分           │
   │   SKIP/LIMIT 裁剪 → 分块轮转路由  │ ─▶ │ ③ 形成元组           │
   └─────────────────────────────────┘    └──────────┬──────────┘
                        输入 shm_mq（8 MiB/个）        回传 shm_mq（2 MiB/个）
```

- **文件 I/O 只发生 1 次**：单个 reader bgworker 顺序扫描一遍输入文件（CSV 走
  quote-aware 记录边界状态机，BINARY/FIXED 按 STRIDE 定长切分），按 SKIP/LIMIT
  全局窗口裁剪后，以 ~512 KiB 分块轮转把记录路由进各 worker 的输入队列。
- **worker 只做 ②③**：从输入队列取记录，字段切分 + `heap_form_tuple`，经
  回传队列发回 leader。进程间只交换成型元组，全部使用 PostgreSQL 原生
  `dsm` / `shm_toc` / `shm_mq`，没有引入第三方 IPC。
- **④ 串行不变**：主 backend 收到消息后，在 `wt->context` 中 `palloc` 出可写
  副本再交给 `WriterInsert()`（因为 `WriterInsert` 会原地修改 `t_data` 的
  infomask / xmin / t_self / t_ctid）。
- **冷缓存反超**：1× 读使文件超 page cache 时并行不再退化（方案 B 曾比串行
  慢 6.49×；Plan C 好窗口下反超串行最高 4.9×）。

详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 与 [docs/PARALLEL_DESIGN.md](docs/PARALLEL_DESIGN.md)。

---

## 已知限制（本版本）

1. 并行路径支持 `DELIMITER / QUOTE / ESCAPE / NULL / SKIP / LIMIT`（CSV）；BINARY/FIXED 支持 `COL / STRIDE / SKIP / OFFSET / PRESERVE_BLANKS / LIMIT`；`FILTER` 与 `FORCE_NOT_NULL` 会自动降级到单进程。
2. 并行路径假设输入编码与数据库编码一致（不调用 `CheckerConversion()` 做逐字段转码）。非 ASCII 场景请显式设置 `ENCODING` 或降级使用 `PARALLEL=OFF`。
3. 并行路径中坏行（字段数不符、输入函数报错、未闭合引号）会导致该 worker 退出并使本次加载失败，不写 `PARSE_BADFILE`；单进程路径的逐行容错行为不变。
4. 分块轮转路由不保证物理插入顺序（heap 表本身也不保证行序）。正确性以内容集合（multiset）为准。
5. 需要 `max_worker_processes` 足够容纳 N+1 个动态 bgworker（N 个解析 worker + 1 个 reader）。
6. 并行化覆盖 `CSV` 与 `BINARY / FIXED`（定长）；`TUPLE / FUNCTION` 后续按相同方式改造。BINARY/FIXED 并行路径要求记录为完整 STRIDE（EOF 尾残字节忽略，与串行一致），NULLIF 模式长度不超过 64 字节。
7. 有效并行度 ≤ 1 时（含单核机器 `auto` 解析为 1）自动走原版串行路径。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [LICENSE](LICENSE) / [COPYRIGHT](COPYRIGHT) / [NOTICE](NOTICE) | 许可、原项目署名、作者 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 整体架构与数据流 |
| [docs/PARALLEL_DESIGN.md](docs/PARALLEL_DESIGN.md) | 并行化设计、分工协议、边界处理 |
| [docs/CHANGES.md](docs/CHANGES.md) | 相对 pg_bulkload 的文件级修改点 |

---

## 版权与署名

本项目是 pg_bulkload 的二次实现。原项目版权归 **NTT, Inc. (2008-2026)** 所有，采用 BSD 3-Clause 风格许可；`COPYRIGHT` 原文与每个源文件头部的原始版权行均完整保留。并行加载部分的版权归 **yuanyuansong (2026)**，以相同许可发布。本项目未获 NTT, Inc. 背书或赞助。
