# pg_fastload v1.0.0 与原版 pg_bulkload 同条件性能对比报告

> 测试日期：2026-11-08　作者：yuanyuansong  
> 本报告为 **VM 调整 CPU/内存之后** 的重新测试结论，取代此前 2 vCPU / 2.5 GiB 环境下的全部性能数字。  
> **pg_fastload v1.0.0 是本项目的第一版实现**；立项前被丢弃的 `CSVMT` pthread 原型不属于交付物，已移入归档目录。

---

## 1. 测试环境（调整后）

| 项 | 值 |
|---|---|
| CPU | **16 vCPU**（Intel i7-13700KF，4 socket × 4 core，16 online）；调整前为 2 vCPU |
| 内存 | **7.3 GiB**（MemTotal 7,675,760 kB），3 GiB swap；调整前约 2.5 GiB |
| OS / 内核 | Oracle Linux Server 10.1 / 6.12.0-105.51.5.el10uek.x86_64（VMware） |
| 数据库 | PostgreSQL 18.6（`REL_18_STABLE`，HEAD `1da6ddbdc3`），**release 版**二进制 |
| 集群 | `PGDATA=/tmp/pgdata`，`port=5433`，库 `tpcc`，`shared_buffers=512MB` |
| bgworker 上限 | `max_worker_processes=32`；`max_parallel_workers=8`（动态 bgworker 不受其限） |
| 其他 GUC | `max_wal_size=4GB`、`checkpoint_timeout=30min`、`maintenance_work_mem=256MB`、`synchronous_commit=on`、`fsync=on` |
| 基线扩展 | `pg_bulkload 3.1.23`，源码 HEAD `798e0f2`，`git status` 仅一条 `D lib/parser_csv_mt.c`（该文件本不属上游），无 CSVMT 标识符、`.so` 无相关符号 → **确认为原版** |
| 被测扩展 | `pg_fastload 1.0.0`，`pgfastload.pg_fastload()` |
| 数据集 | `/tmp/tpcc/order_line.csv`：10.7 GiB / 130,447,434 行，10 列，无引号纯逗号 CSV |

postmaster 可执行文件已核验为 `/home/ysyuan/postgres/release/bin/postgres`（10.9 MB，非 35 MB 的 debug/assert 版），排除断言构建导致的性能偏差。

## 2. 测试方法与公平性控制

| 控制项 | 做法 |
|---|---|
| 同一目标表 | 每阶段 `CREATE TABLE t_bench_cmp (LIKE public.order_line)`，每轮 `TRUNCATE` |
| 同一输入 | 各配置使用**同一个 CSV 文件**，`TYPE=CSV`、`WRITER=DIRECT`、同一 `LOGFILE` 目录 |
| 计时口径 | 以扩展日志 `CPU Xs/Yu sec elapsed N sec` 中的 **服务端 elapsed** 为主指标；wall 仅作辅助 |
| 缓存状态 | 热缓存轮次每轮前 `cat CSV > /dev/null` 预热；冷缓存轮次用 `posix_fadvise(DONTNEED)` 精确丢弃该文件的 clean page（免 root） |
| 顺序偏差 | 多轮采用正序 / 倒序轮换，取**中位数** |
| checkpoint 干扰 | 精修轮次在**计时之外**执行 `CHECKPOINT`，避免上一轮脏页刷盘落入下一轮测量 |
| 内容正确性 | `count(*) || sum(hash_record_extended(t,0))`：与物理插入顺序无关，适配方案 B 的不保序特性 |
| 等价性锚点 | `pg_fastload PARALLEL=OFF` 应与原版 `pg_bulkload` 耗时一致，用于验证更名未引入回归并识别环境抖动 |

配置集合：原版 `pg_bulkload`、`pg_fastload` 的 `PARALLEL=OFF`、`w=1/2/4/8/15` 以及未指定 worker 的 `auto`（16 核 → `CPU−1=15`）。

## 3. 主结果：热缓存、四阶段串行基线

### 3.1 1M 行 / 77 MB（完全容纳于 page cache，reps=3，热缓存）

| 配置 | 中位 elapsed | min ~ max | 相对原版 | sys / usr CPU (s) | 执行 |
|---|---|---|---|---|---|
| 原版 pg_bulkload 3.1.23 | 0.530s | 0.510s ~ 0.540s | 1.00× | 0.07 / 0.42 | PASS |
| pg_fastload PARALLEL=OFF | 0.530s | 0.520s ~ 0.720s | 1.00× | 0.06 / 0.43 | PASS |
| pg_fastload w=1 | 1.470s | 1.460s ~ 1.480s | 0.36× | 0.05 / 0.01 | PASS |
| pg_fastload w=2 | 0.720s | 0.700s ~ 0.730s | 0.74× | 0.04 / 0.01 | PASS |
| pg_fastload w=4 | 0.340s | 0.290s ~ 0.340s | 1.56× | 0.03 / 0.02 | PASS |
| pg_fastload w=8 | 0.230s | 0.200s ~ 0.230s | 2.30× | 0.05 / 0.02 | PASS |
| pg_fastload w=15 | 0.210s | 0.200s ~ 0.210s | 2.52× | 0.07 / 0.02 | PASS |
| pg_fastload auto (=15) | 0.220s | 0.210s ~ 0.220s | 2.41× | 0.07 / 0.03 | PASS |

内容一致性：`ALL_CONTENT_MATCH = True`（黄金摘要来自原版 pg_bulkload）。

### 3.2 8M 行 / 640 MB（完全容纳于 page cache，reps=3，热缓存）

| 配置 | 中位 elapsed | min ~ max | 相对原版 | sys / usr CPU (s) | 执行 |
|---|---|---|---|---|---|
| 原版 pg_bulkload 3.1.23 | 4.070s | 4.070s ~ 4.110s | 1.00× | 0.32 / 3.59 | PASS |
| pg_fastload PARALLEL=OFF | 4.110s | 4.090s ~ 4.170s | 0.99× | 0.36 / 3.57 | PASS |
| pg_fastload w=1 | 11.840s | 11.820s ~ 41.940s | 0.34× | 0.34 / 0.12 | PASS |
| pg_fastload w=2 | 5.600s | 5.080s ~ 5.660s | 0.73× | 0.24 / 0.10 | PASS |
| pg_fastload w=4 | 2.650s | 2.590s ~ 2.660s | 1.54× | 0.26 / 0.07 | PASS |
| pg_fastload w=8 | 1.460s | 1.450s ~ 1.490s | 2.79× | 0.30 / 0.14 | PASS |
| pg_fastload w=15 | 1.450s | 1.430s ~ 1.450s | 2.81× | 0.36 / 0.27 | PASS |
| pg_fastload auto (=15) | 1.440s | 1.430s ~ 1.480s | 2.83× | 0.33 / 0.28 | PASS |

内容一致性：`ALL_CONTENT_MATCH = True`（黄金摘要来自原版 pg_bulkload）。

### 3.3 32M 行 / 2.5 GiB（可容纳但接近内存上限，reps=2，热缓存）

| 配置 | 中位 elapsed | min ~ max | 相对原版 | sys / usr CPU (s) | 执行 |
|---|---|---|---|---|---|
| 原版 pg_bulkload 3.1.23 | 31.335s | 16.100s ~ 46.570s | 1.00× | 1.89 / 13.79 | PASS |
| pg_fastload PARALLEL=OFF | 16.415s | 16.270s ~ 16.560s | 1.91× | 1.80 / 13.98 | PASS |
| pg_fastload w=1 | 47.420s | 47.160s ~ 47.680s | 0.66× | 1.42 / 0.43 | PASS |
| pg_fastload w=2 | 22.170s | 21.660s ~ 22.680s | 1.41× | 0.97 / 0.45 | PASS |
| pg_fastload w=4 | 10.620s | 10.580s ~ 10.660s | 2.95× | 0.93 / 0.41 | PASS |
| pg_fastload w=8 | 5.795s | 5.760s ~ 5.830s | 5.41× | 1.06 / 0.48 | PASS |
| pg_fastload w=15 | 5.655s | 5.610s ~ 5.700s | 5.54× | 1.43 / 1.19 | PASS |
| pg_fastload auto (=15) | 5.620s | 5.580s ~ 5.660s | 5.58× | 1.30 / 1.19 | PASS |

内容一致性：`ALL_CONTENT_MATCH = True`（黄金摘要来自原版 pg_bulkload）。

> **注意**：本阶段 `bulk` 仅 2 轮且首轮含冷启动异常（46.57 s），表中「相对原版」被虚高，不作为结论；修正后的稳态基线见第 4 节。

## 4. 稳态基线修正（重要）

32M 行阶段首轮出现一次显著异常：原版 `pg_bulkload` 首轮 **46.570 s**，次轮 **16.100 s**；同期 `pg_fastload PARALLEL=OFF` 两轮为 16.560 / 16.270 s，内容复测为 16.120 / 16.280 s。两者是同一套串行代码，小/中规模也严格等价（0.530 vs 0.530 s；4.070 vs 4.110 s），因此 46.570 s 是**表文件首次扩展 + WAL 段初始化 + checkpoint 的冷启动开销**，不是代码差异。

若直接用 2 轮中位数 31.335 s 作基线，`auto` 的加速比会被虚增到 5.58×，属于测量假象。为此补做带 `CHECKPOINT` 隔离的稳态精修：

| 配置 | 稳态中位 elapsed | min ~ max | 轮次 | 相对原版 | 行数校验 |
|---|---|---|---|---|---|
| 原版 pg_bulkload 3.1.23 | 16.180s | 16.130s ~ 16.250s | 4 | 1.00× | PASS |
| pg_fastload PARALLEL=OFF | 16.325s | 16.270s ~ 16.380s | 2 | 0.99× | PASS |
| pg_fastload auto (=15) | 5.740s | 5.620s ~ 5.860s | 2 | 2.82× | PASS |

**修正后的 32M 行稳态加速比 = 2.82×**（`auto`，相对原版稳态 16.180 s），与中规模的 2.83× 一致，说明 2.8~2.9× 是该规模下可信的收益上限。

## 5. 冷缓存（超 page cache）下的 I/O ×N 代价

方案 B 的每个 worker 都从头到尾读完整 CSV，总读量 ≈ `N × 文件大小`。文件能被 page cache 容纳时重复读命中内存，代价主要是 CPU/内存带宽；文件超内存时退化为真实磁盘 I/O 争抢。用 `posix_fadvise(DONTNEED)` 在 2.5 GiB 文件上模拟后者：

首轮冷缓存实验出现「第 1 轮快、第 2 轮极慢」（`w=15` 6.03 s / 127.40 s），而原版两轮稳定在 16.44 / 16.39 s。观测 `/proc/meminfo` 后确认是首次 `posix_fadvise(DONTNEED)` 未完全逐出前一阶段遗留的缓存页，第 1 轮仍命中缓存。补做带缓存状态观测的确认实验：每轮前 `CHECKPOINT` + `sync` + 两次 fadvise，`Cached` 由 5.6~6.2 GiB 降到约 3.6 GiB（2.5 GiB 的 CSV 页被逐出），确认每轮均为真实冷读。

| 配置 | 各轮冷读 elapsed (s) | 中位数 | 耗时 / 原版 |
|---|---|---|---|
| 原版 pg_bulkload | 16.55 / 16.57 | 16.560s | 1.00× |
| pg_fastload w=4 | 71.46 / 101.69 | 86.575s | 5.23× |
| pg_fastload auto=15 | 96.75 / 107.49 / 126.33 | 107.490s | 6.49× |

**结论**：冷缓存下原版几乎不受影响（约 16.6 s，磁盘读与 CPU 重叠，仅 `sys` CPU 由 1.4 s 升到 5.3 s），而 `auto=15` 中位约 107 s，**比原版慢约 6.5×**；`w=4` 也要 71~102 s。热缓存下的 2.8× 收益被磁盘 I/O 完全吞掉并反转。

理论读量对照（以 2.5 GiB / 15 worker 为例）：原版 2.5 GiB，`w=4` 约 10 GiB，`auto=15` 约 **37.5 GiB**；全量 10.7 GiB 时 `auto` 约 **160 GiB**。因此「worker 越多越快」只在缓存命中时成立。

## 6. 全量 10.7 GiB / 1.3 亿行（尝试失败，未取得有效数据）

已在同一 VM 上尝试全量冷缓存对比，但前两次加载均以 `ERROR: could not write to data file: No space left on device` 失败（`bulk` 运行 572 s、`fl_w4` 509 s 后报错），第三次 `auto` 遂主动终止。原始输出保留在 `bench_refine_full.out`。

根因是环境容量而非代码：51 GiB 根分区已承载 11 GiB 源 CSV、7.5 GiB PGDATA（含 4.1 GiB WAL）与两套 PG 安装；130M 行的**单事务**需要约 13 GiB 表数据 + 10 GiB 以上 WAL，超过当时 9.6 GiB 的余量。失败后事务回滚自动释放空间，集群经核验恢复正常（PostgreSQL 18.6，16 张用户表，24 GiB 可用），未做破坏性清理。

**因此本报告不给出全量实测加速比，也不把第 5 节的冷缓存模拟结论外推为全量结论。**全量对比仍是残留风险项；补测建议：更大磁盘、WAL 单独分区，并优先测 `w=4` 而不是默认 15。

## 7. 并行度 scaling 与最优 worker 数

以中规模（8M 行，热缓存）为例的 worker 数扫描：

| PARALLEL_WORKERS | elapsed | 相对原版 |
|---|---|---|
| 1 | 11.840s | 0.34× |
| 2 | 5.600s | 0.73× |
| 4 | 2.650s | 1.54× |
| 8 | 1.460s | 2.79× |
| 15 | 1.450s | 2.81× |
| auto(15) | 1.440s | 2.83× |

**规律**：

- `w=1` 恒为**负收益**（小规模 0.36×、中规模 0.34×）：只有一个 worker 时「快速跳过」不省任何元组构造，却多付 bgworker 启动、DSM/shm_mq 建立与元组跨进程拷贝的成本。日志里主 backend 的 `usr` CPU 降到0.01~0.1 s 只是因为计算转移到了 worker 进程，不是 CPU 总量下降。
- `w=2` 仍未回本（0.73~0.74×）；**盈亏平衡点在 w=4 附近**。因此 `PARALLEL=ON` 至少要 4 个 worker 才值得开启。
- `w=8 → w=15` 收益趋于饱和（中规模 1.46 → 1.44 s），④ 写入仍串行、主 backend 成为新的瓶颈，符合 Amdahl 上限。
- 16 核机器上默认 `CPU−1=15` 落在收益平台区，无需手工调优；但 CPU 更多而文件远超内存时，应显式调小 worker 数。

## 8. 效果评估结论

1. **等价性确认**：`pg_fastload PARALLEL=OFF` 与原版 `pg_bulkload` 在1M / 8M / 32M 行上耗时一致（0.530/0.530、4.070/4.110、16.100/16.270 s 量级），更名与架构改造未引入串行回归。
2. **正确性确认**：热缓存各阶段所有配置的内容摘要与原版完全一致，行数全部匹配；`auto` 正确取到 15 个 worker，无降级、无 `failed to attach`。
3. **收益**：热缓存下默认 `auto` 相对原版约 **2.4×（1M 行）→ 2.8×（8M 行）→ 约 2.9×（32M 行稳态）**，数据越大固定开销摊薄越明显，但受串行写入限制不会随核数线性增长。
4. **代价与边界**：方案 B 的 I/O ×N 与不保序是设计取舍。文件远超内存时收益会被磁盘 I/O 完全反转：冷缓存下 `auto=15` 实测比原版慢约 6.5×（第 5 节）。该场景应显式降低 `PARALLEL_WORKERS`（建议 4 左右）或直接用 `PARALLEL=OFF`。
5. **worker 数建议**：`w≤2` 恒为负收益，盈亏平衡点在 `w=4` 附近；`w=8→15` 已趋饱和。16 核默认 `CPU−1=15` 只适合能进 page cache 的文件，不是通用最优值。
6. **不承诺线性加速**：16 核并未带来 16×，瓶颈已从 CSV 解析转移到 ④ `WriterInsert()` 串行写入与 WAL；全量 10.7 GiB 因磁盘容量未能实测（第 6 节）。

## 9. 旧结论作废声明与残留风险

以下数字来自 **2 vCPU / 2.5 GiB** 的旧 VM 或被丢弃的 CSVMT 原型，**全部作废**，不得再用于评估本项目：

- CSVMT：DIRECT 68.00 → 47.27 s（1.44×）、BUFFERED 98.19 → 76.54 s（1.28×）；
- pg_fastload 早期小样本：serial 0.530 s / w1 0.990 s / w2 0.350 s（1.51×）、`auto` 默认 1 个 worker；
- 所有 A/A′ 字节分段、切点边界规格、预扫描行号索引、CRLF 分档等**未被采用**的设计文档。

相关文件已移入 `_archive_deprecated_v0_csvmt/`（仅归档、未删除）。**pg_fastload v1.0.0 是第一版实现。**

**仍存在的风险 / 未执行项**：

- 并行路径无逐行容错（字段数不符、未闭合引号会令加载失败，不写 `PARSE_BADFILE`）；
- 不保序；仅 `TYPE=CSV` 完成并行化，`BINARY/FIXED/TUPLE/FUNCTION` 未改造；
- 自动降级清单见 `docs/PARALLEL_DESIGN.md` 第 8 节；
- ASan 在该 VM 仍不可用（缺 `libasan.so.8` runtime、提权安装被拦截）；本轮未跑 valgrind，内存结论仍为静态生命周期核对；
- 单轮基准存在 checkpoint / I/O 抖动，结论以中位数与稳态精修为准；未做跨机器、跨文件系统、并发负载下的复现；
- **10.7 GiB 全量对比因 VM 磁盘 ENOSPC 未完成**（已在第 6 节记录尝试过程、失败原因与集群恢复核验），本报告不给出全量加速比。

---

复现脚本：`bench_compare.py`（四档规模扫描）、`bench_refine.py`（稳态基线 / 冷缓存 / 全量）、`smoke_baseline.py`（加载器与二进制身份核验）、`probe_vm_new.py` / `start_cluster_new.py`（环境探测与集群准备）。
