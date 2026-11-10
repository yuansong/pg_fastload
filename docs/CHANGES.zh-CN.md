# 相对 pg_bulkload 3.1.23 的修改点

> 上游：`https://github.com/ossc-db/pg_bulkload`，HEAD `798e0f2`，版本 3.1.23
> 本项目作者：yuanyuansong，2026
>
> 本文只记录**已采纳**的改动，按轮次排列；评估后未采纳的方案集中记录在
> [OPTIMIZATION.md](OPTIMIZATION.md) 的「未采纳方案」一节，不在本文逐轮重复。

---

## 1. 全局更名（108 个文件 + 18 个文件/目录名）

| 原名 | 新名 |
|---|---|
| 项目 / 扩展 / 共享库 `pg_bulkload` | `pg_fastload` |
| SQL schema `pgbulkload` | `pgfastload` |
| SQL 函数 `pgbulkload.pg_bulkload()` | `pgfastload.pg_fastload()` |
| `lib/pg_bulkload.c` / `.sql.in` / `.control` / `--3.1.23.sql` | `pg_fastload.c` / `.sql.in` / `.control` / `--1.0.0.sql` |
| `include/pg_bulkload.h` | `include/pg_fastload.h` |
| 宏 `PG_BULKLOAD_*` / `BULKLOAD_*` / `FASTLOAD_PROFILE*` | `PG_FASTLOAD_*` / `FASTLOAD_*` |
| `bin/pg_bulkload.c` / 客户端可执行名 | `bin/pg_fastload.c` / `pg_fastload` |
| `SPECS/pg_bulkload-pg*.spec` / `msvc/*` / `docs/pg_bulkload*.html` | 对应 `pg_fastload*` |

版本号重置为 `1.0.0`（独立项目起点）。

**未改动**：`COPYRIGHT` 原文件（BSD-3-Clause，NTT, Inc. 2008-2026）逐字节保留；所有继承源文件
头部的 `Copyright (c) ... NTT, Inc.` 行保留。

---

## 2. 新增文件

| 文件 | 作用 |
|---|---|
| `LICENSE` | BSD-3-Clause 许可 + 原版权声明 + 并行化贡献版权 |
| `NOTICE` | 来源、版本、许可、署名、保留项与修改项说明 |
| `README.md`（英）/ `README.zh-CN.md`（中） | 项目说明：定位、与 pg_bulkload 的差异、架构、快速开始、限制 |
| `docs/USER_MANUAL.md`（英）/ `docs/USER_MANUAL.zh-CN.md`（中） | 用户手册：安装、关键字全表、用法、并行调优、FAQ |
| `docs/ARCHITECTURE.md`（英）/ `docs/ARCHITECTURE.zh-CN.md`（中） | 整体架构、组件职责、数据流、IPC 协议、降级设计 |
| `docs/CHANGES.md` | 本文件 |
| `docs/BENCHMARK.md` | 性能基准 |
| `docs/VERIFICATION.md` | 验证报告 |
| `docs/OPTIMIZATION.md` | 优化记录与未采纳方案 |
| `docs/img/architecture.svg`（英）/ `docs/img/architecture.zh-CN.svg`（中） | 架构图（英 / 中） |
| `include/fastload_parallel.h` | DSM 共享协议：`FastloadParams` / `FastloadColumn` / `FastloadShared` / `FastloadWorkerId`、shm_toc key、队列容量、magic |
| `lib/fastload_parallel.c` | **leader 侧**：`FastloadDefaultWorkers()`、`FastloadParallelSupported()`、`FastloadParallelRun()`；DSM/shm_toc/shm_mq 创建、bgworker 注册、round-robin 消费、`WriterInsert()` 前可写副本 |
| `lib/fastload_worker.c` | **reader / worker 侧**：`pg_fastload_reader_main()`、`pg_fastload_worker_main()`、CSV quote-aware 边界状态机、定长 STRIDE 切分、分块轮转路由、字段切分、复用 `TupleFormer` 形成元组、`shm_mq_send` |

---

## 3. 修改文件

| 文件 | 修改内容 |
|---|---|
| `include/reader.h` | `struct Reader` 增加 `parallel`、`parallel_workers`、`is_csv`、`is_fixed`、`parallel_skip`、`parallel_delim/quote/escape`、`parallel_null`、`parallel_has_filter`、`parallel_has_fnn` |
| `lib/reader.c` | ① `ReaderCreate()` 记录 `is_csv`（`TYPE=CSV`）与 `is_fixed`（`TYPE=BINARY/FIXED`）；② `ReaderInit()` 用 `FastloadDefaultWorkers()` 计算默认 worker 数；③ `ReaderParam()` 新增 `PARALLEL` / `PARALLEL_WORKERS`；④ 镜像 `SKIP/OFFSET`、`DELIMITER`、`QUOTE`、`ESCAPE`、`NULL`、`FILTER`、`FORCE_NOT_NULL` 到 Reader，同时**仍转发给 `ParserParam()`**，单进程语义不变 |
| `lib/pg_fastload.c` | 主循环改为 `if (FastloadParallelSupported(rd, wt)) FastloadParallelRun(...); else 原循环`；`<PARALLEL>` profile 组新增 `READ` 段；其他统计/日志/收尾逻辑不动 |
| `lib/binary.c` + `include/binary.h` | 新增 `COL` 描述符序列化：`BinaryParserGetMeta`、`BinarySerializedSize`、`BinarySerializeFields`、`BinaryDeserializeFields`（定长并行用，函数指针不跨进程） |
| `lib/parser_binary.c` | 导出 `BinaryParserGetMeta`（nfield / rec_len / fields） |
| `lib/pg_btree.c` | `SpoolerInsert()` 在 `ri_NumIndices <= 0` 时提前返回，消除无索引表的每元组空转 |
| `include/pg_profile.h` | 新增并行阶段埋点：`prof_par_setup/wait/copy/write/read`、`prof_par_scan/form/ipc` |
| `lib/Makefile` | `SRCS` 增加 `fastload_parallel.c`、`fastload_worker.c`；其余（含 `-lpthread`、`pgut/pgut-ipc.c`）保留 |
| `lib/exports.txt` | 增加导出符号 `pg_fastload_worker_main`（bgworker 通过 `load_external_function` 按符号名加载） |

---

## 4. 删除 / 丢弃

- **实验性的多线程 CSV 原型已全部丢弃**（含其解析器源文件与构建项）：并行能力不以新的 `TYPE`
  暴露，而是 `PARALLEL=ON` 时由 `TYPE=CSV / BINARY / FIXED` 自动启用。`ReaderCreate()` 的类型列表
  与上游完全一致：`BINARY / FIXED / CSV / TUPLE / FUNCTION`。
- **上游 HTML 手册**（`docs/index.html`、`pg_fastload.html`、`pg_timestamp.html`、`style.css`
  及日文版 `*_ja.html`）删除：其性能数据为 PostgreSQL 9.3 时代、内容已被
  `docs/USER_MANUAL*.md` 完整覆盖且与实际实现不一致，保留会造成误导。

---

## 5. 完全未改动的模块

`lib/parser_csv.c`、`lib/parser_tuple.c`、`lib/parser_function.c`、`lib/source.c`、
`lib/writer*.c`（含 `writer_parallel.c`）、`lib/logger.c`、`lib/pg_strutil.c`、`bin/*`（除更名）、
`util/*`、`SPECS/*`、`msvc/*`、`lib/pgut/*`。

这保证了：

- 所有原有控制文件关键字与语义不变；
- `WRITER = DIRECT / BUFFERED / PARALLEL` 三种写入方式不变；
- `badfile` / 逐行容错 / `SKIP` / `LIMIT` / `ENCODING` / `FILTER` / `FORCE_NOT_NULL` 在单进程路径下不变；
- 上游 PARALLEL writer 依赖的 `pgut-ipc.c` 仍在构建中。

---

## 6. 新增行为摘要

| 控制文件 | 行为 |
|---|---|
| `PARALLEL = OFF`（默认） | 与 pg_bulkload 完全一致 |
| `PARALLEL = ON`, `TYPE = CSV / BINARY / FIXED` | 多进程并行 ①②③，④ 主 backend 串行 |
| `PARALLEL = ON`, `TUPLE / FUNCTION` | 自动降级为单进程（INFO 提示） |
| `PARALLEL = ON` + `FILTER` / `FORCE_NOT_NULL` / dropped 列 / 转码 / NOT NULL / `CHECK_CONSTRAINTS` / `PARSE_ERRORS` / `STDIN` / `MULTI_PROCESS` | 自动降级为单进程（INFO 提示，完整清单见 USER_MANUAL §10.4） |
| `PARALLEL_WORKERS = n` | 显式指定 1..64 |
| 未指定 `PARALLEL_WORKERS` | 在线 CPU 核数 − 1，clamp 到 1..64 |

---

## 7. 合规说明

- 保留 `COPYRIGHT` 原文（BSD-3-Clause 第 1、2 条要求）。
- 每个继承源文件保留 NTT 版权行；新增/修改文件额外标注 `Copyright (c) 2026, yuanyuansong`。
- 明确声明基于 pg_bulkload 3.1.23 二次实现、遵循原许可、未使用 NTT 名义背书（第 3 条要求）。
- 二进制分发时需在随附文档中复现版权声明与免责条款；`LICENSE` / `NOTICE` / `COPYRIGHT` 随源码提供。

---

## 8. 首轮联调修复记录

| 问题 | 根因 | 修复 |
|---|---|---|
| `PARALLEL=ON` 被无声降级为单进程 | `rd->checker.tchecker != NULL` 是恒真条件：所有 writer 都无条件 `CreateTupleChecker()`；对 CSV 而言 `CoercionCheckerTuple()` 判定 `NO_COERCION` 后原样返回 tuple，worker 的 `TupleFormerTuple()` 输出与之等价 | 从 `FastloadParallelSupported()` 移除该条件，并在注释与文档中写明不可加回 |
| 小文件偶发 `worker N failed to attach (state=2)` | worker 抢在主 backend 之前跑完并 detach，`shm_mq_wait_for_attach()` 返回 `SHM_MQ_DETACHED`；`state=2` 实为 `FL_WORKER_DONE` | 编排层容忍 `DETACHED + FL_WORKER_DONE`，仅对未到达 DONE 的 worker 报错；PG 原生 shm_mq 保证剩余消息仍可排空 |
| `SKIP` / `LIMIT` 语义与上游不一致（交错分片下「最先到达的 N 条」不确定） | worker 原先只按记录号取模，未限制全局记录窗口 | worker 侧按 `[skip, skip+limit)` 窗口裁剪，先减后比避免溢出 |
| 并行分工不可观测 | 缺少每 worker 产出统计 | `FastloadParallelRun()` 正常收尾后打印 `worker #i produced N rows` |

---

## 9. P0 性能优化（2026-11-08 第二轮）

依据：1 GiB TPC-H lineitem 基准 + `USE_PROFILE=1` 阶段计时
（`READER.PARSER` 占原版耗时 78.6%、并行路径 N× 全量扫描占 auto 耗时 65%、
`WRITER.TOAST` 被误累积为 15.39 s 等结论）。

| # | 瓶颈 | 修改 | 效果（1 GiB，16 vCPU） |
|---|---|---|---|
| P0-1 | `PARALLEL_WORKERS=1` 仍创建 DSM + bgworker + shm_mq，耗时 16.2 s（串行 8.2 s 的 2 倍） | `FastloadParallelSupported()` 对有效 worker 数 ≤ 1 返回 false 并输出 INFO，走原版单进程 CSV 循环 | w=1 **16.215 → 8.070 s（2.01×）**，与 `PARALLEL=OFF` 等价 |
| P0-2 | worker 对每条记录逐字节调用 `fl_append_rec()` 拷贝（含大量非归属记录），整条记录再二遍切分 | 记录边界状态机改为 run-based 连续区间 `memcpy`；非归属记录零拷贝直接跳过；归属记录按连续段一次拷入。引号/转义/CRLF/跨块/末行无换行语义不变 | w=2 **7.760 → 6.820 s（1.14×）**，w=4 **3.270 → 2.950 s（1.11×）**，w=8 **2.530 → 2.320 s（1.09×）** |
| P0-3 | 并行路径无任何 profile 埋点：GLOBAL 各阶段读 0，leader 等待时间被误累积进 `WRITER.TOAST` | 新增 `GLOBAL.PARALLEL` 总段 + `<PARALLEL>`（SETUP/WAIT/COPY/WRITE）+ `<WORKER>`（SCAN/FORM/IPC，worker 经 DSM 发布本地计时）；`WriterInsert` 内层 PUSH/POP | GLOBAL 合计 ≈ elapsed；`WRITER.TOAST` 回归 0.12~0.14 s 正常量级 |

验证：release + debug 双构建 rc=0；`fastload_verify.py` **90/90 PASS**；
7 配置 1 GiB 内容摘要（`count||sum(hash_record_extended)`）完全一致。

---

## 10. 单读多解并行架构（2026-11-08 第三轮）

依据：首版「每个 worker 各自 `open()` 全量 CSV 扫描」的结构性瓶颈——I/O ×N、N 份重复边界扫描，
文件超出 page cache 时总读量 ≈ N × 文件大小（冷缓存实测比串行慢 6.49×）。

**架构**：单个 reader bgworker（无 DB 连接）单遍扫描 CSV，把记录按分块轮转分发给 N 个解析
worker；worker 只解析归属记录并经 shm_mq 回传 leader。文件 I/O 从 ×N 降为 ×1。
reader 为 handle[0]，worker 为 handle[1..N]；`max_worker_processes ≥ N+1`。

三个实测瓶颈与修复（每次修复均经 profile 数据确认）：

| # | 瓶颈（profile 证据） | 修改 | 效果（1 GiB，w=2） |
|---|---|---|---|
| C-1 | reader 逐条 `rec_no % N` 交错路由造成队头阻塞：一条环满即卡死全部环（reader blocked 9.205 s / 8.49M waits） | 改为**字节量分块轮转**：每 ~512 KiB 连续记录切换 owner（`FL_ROUTE_BYTES`），路由字节数只统计窗口内记录 | 与 C-2/C-3 叠加后生效 |
| C-2 | worker 逐条取环：每记录 2 次 spinlock + 2 次小 memcpy（1500 万次锁操作，与 reader 争用），worker SCAN 5.2 s | worker 侧**批量取环**：一次锁内整块拉取，无锁切帧；超大记录回退流式读取 | SCAN 5.2 → 0.32 s |
| C-3 | 1 MiB 环只缓冲 2 个 512 KiB 路由块（≈6 ms worker 工作量）：reader 被一个环阻塞的整个窗口内其余环全部挨饿（worker idle ~45%） | 队列容量 **1 MiB → 8 MiB**，回传队列 1 → 2 MiB；DSM 随实际 worker 数伸缩（15 worker ≈ 144 MiB） | **w2 10.59 → 3.46 s（3.06×）**、w4 5.13 → 1.97 s、w8 2.34 → 1.95 s |

最终 1 GiB 热缓存对比（P0 基线 → 单读多解）：w2 **6.82 → 3.46 s（1.97×）**、
w4 **2.95 → 1.97 s（1.50×）**、w8 2.32 → 1.95 s（1.19×）、w15 2.32 → 2.01 s、
auto 2.34 → 1.99 s（1.18×）；fl_off / w1 与 P0 等价（串行路径零改动）。
高并发已饱和于 leader 串行 `WriterInsert`（WRITE ≈ 1.25~1.36 s 下限）。

验证：release + debug 双构建 rc=0；`fastload_verify.py` **90/90 PASS**；
7 配置内容摘要一致；复测后生产 `.so` 已恢复非 profile 版。

---

## 11. 内核 shm_mq 统一传输 + 冷缓存大块 I/O（2026-11-09 第四轮）

依据：单读多解架构定稿后的阶段耗时分析——reader→worker 仍走自研 SPSC 字节环
（spinlock + head/tail + 100 µs 退避，约 200 行手写同步代码），与 worker→leader 方向的内核
shm_mq 形成两套传输实现；冷缓存场景 reader 64 KiB 小块读 + 64 KiB 小块发产生约 3.3 万次阻塞
系统调用，在本虚拟化宿主上被放大为分钟级回归。

| # | 项 | 修改 | 效果 |
|---|---|---|---|
| S-1 | reader→worker 自研 SPSC 环替换为内核 shm_mq | 删除 `FastloadRing`（spinlock/head/tail/退避/计数控制），DSM 改为每 worker 一个 8 MiB shm_mq；reader 以 `shm_mq_send` 分帧发送、worker 以 `shm_mq_receive` 接收（整帧同消息零拷贝快路径，跨消息重组）；EOF 语义 = reader 先置 `reader_state=DONE` 再 `shm_mq_detach`，worker 见 `SHM_MQ_DETACHED` 后校验状态区分正常收尾/崩溃 | 净删约 200 行手写同步代码，传输实现与回传方向统一为内核原语；热缓存 w2 3.46→3.33 s、w15 2.01→1.89 s、auto 1.99→1.88 s |
| S-2 | 冷缓存小块系统调用延迟放大 | `FL_READ_CHUNK` 64 KiB→**1 MiB**（每次载入 16386→1026 次 read）、`FL_STAGE_CAP` 64 KiB→**512 KiB**（17193→2148 次 send）；删除 `FL_DRAIN_CAP`（worker 侧改由 shm_mq 帧边界自然分批） | 阻塞系统调用次数降约 15×；消除冷缓存**系统性**回归（好窗口下 w2 3.0 / w4 1.8 / auto 1.5 s，反超串行 7.1 s，最高 4.9×）；宿主偶发卡顿窗口（vmstat 0 steal / 0 iowait / 100% idle）属 hypervisor 层现象 |

涉及文件：`lib/fastload_worker.c`（reader/worker 传输层重写、FastloadRouter）、
`lib/fastload_parallel.c`（DSM 布局、profile 摘要去 ring 字段）、
`include/fastload_parallel.h`（删 `FastloadRing`，`FASTLOAD_KEY_RING_BASE`→`FASTLOAD_KEY_INPUT_BASE`，
`FASTLOAD_RING_SIZE`→`FASTLOAD_INPUT_MQ_SIZE`）。

验证：release + debug 双构建 rc=0；`fastload_verify.py` **90/90 PASS**；7 配置热缓存内容摘要
`7375042|9258128314029119699254` 与 GOLDEN 一致；复测后生产 `.so` 恢复非 profile 版。

---

## 12. BINARY / FIXED 并行化（2026-11-09 第五轮，P-1）

背景：并行路径原仅支持 `TYPE=CSV`（`FastloadParallelSupported` 以 `rd->is_csv` 守卫）。
本轮将 `TYPE=BINARY` 与 `TYPE=FIXED`（两者在 `reader.c` 中映射到同一个定长解析器
`CreateBinaryParser`，COL 描述 + STRIDE）接入单读多解并行管线。

| 组件 | 修改 |
|---|---|
| `include/reader.h` / `lib/reader.c` | `Reader` 增加 `is_fixed`（idx 0/1 → BINARY/FIXED 均为定长） |
| `lib/binary.c` + `include/binary.h` | 新增 COL 字段描述符序列化：`BinaryParserGetMeta`（导出 nfield/rec_len/fields）、`BinarySerializedSize`/`BinarySerializeFields`（leader 把 kind/offset/len/nullif 写入 DSM blob）、`BinaryDeserializeFields`（worker 按 kind 从静态 `TYPES[]` 表重建 read 函数指针并分配 work buffer；**函数指针不跨进程传递**） |
| `include/fastload_parallel.h` | `FastloadParams` 增加 `pmode`（FL_PMODE_CSV/FL_PMODE_FIXED）、`nbinfields`、`rec_stride`；DSM 参数块在 `cols[]` 之后追加定长 blob |
| `lib/fastload_parallel.c` | 守卫放行 `is_fixed`；fixed 专属回退：无 COL、rec_len≤0、NULLIF 模式长于 64 字节 → 串行；`fastload_fill_params` 序列化 blob |
| `lib/fastload_worker.c` | ① reader 侧 `fl_reader_scan_fixed`：按 STRIDE 直接切记录（无引号状态机），帧协议/SKIP/LIMIT/分块轮转与 CSV 完全一致；尾残字节 WARNING 忽略（对齐串行）。② worker 侧 `fl_emit_record_fixed`：镜像 `ExtractValuesFromFixed`——scratch 拷贝（队列内存只读）+ 字段尾临时 NUL 终止 + 按序调用 `field->read()`；STRIDE 校验防错 |

验证：

- 新增 `fastload_verify_fixed.py` **44/44 PASS**：纯 CHAR 文本、BINARY/FIXED 别名、混合文本+二进制
  数值（INTEGER/BIGINT/DOUBLE 原生字节）、NULLIF（文本+数值）、STRIDE>字段和（尾部填充）、
  尾残字节、SKIP+LIMIT、PRESERVE_BLANKS+VARCHAR、空文件、w=1/2/3/4/auto、FILTER 降级、
  10000 行 × 4 worker 分片。
- CSV 回归 `fastload_verify.py` **90/90 PASS**（无回归）。
- 1 GiB 定长基准（10,737,418 行 × 100 B，7 配置 × 3 轮热缓存）：内容摘要
  `10737418|12495206446709334590242` 七配置 ALL_MATCH=True；好窗口 fl_off **1.62 s**、
  w2 1.06 s、w4 0.94 s、w8 **0.89 s**、w15 0.90 s、auto 0.91 s（相对串行最高 **1.82×**）。

已知限制（定长并行）：输入编码须与数据库编码一致、FILTER/约束检查/PARSE_BADFILE 降级串行；
NULLIF 模式长度 ≤ 64 字节；记录必须为完整 STRIDE（尾残忽略）。

---

## 13. 无索引 spool 早退（2026-11-09 第六轮，W-3a）

先测量后决策：1 GiB lineitem 在**无索引**下 `WRITER.INDEX` 段仍耗 0.28 s，profile 显示这是一段
纯空转——`SpoolerInsert()` 无论目标表有无索引，每元组都执行 `ExecStoreHeapTuple()` +
`GetPerTupleExprContext()` + `IndexSpoolInsert()` 循环（`ri_NumIndices == 0` 时循环体不执行）。

修改（`lib/pg_btree.c`）：`SpoolerInsert()` 开头提前解析 `relinfo`，`ri_NumIndices <= 0` 直接 return。
3 行有效代码，无语义变化（有索引路径逻辑完全不变）。

| 配置 | 上一版 | 本轮 | 提升 |
|---|---|---|---|
| fl_off | 8.24 s | **7.91 s** | 1.04× |
| fl_w1 | 8.23 s | **7.86 s** | 1.05× |
| fl_w2 | 3.32 s | 3.28 s | 1.01× |
| fl_w4 | 1.96 s | 1.91 s | 1.03× |
| fl_w8 | 1.88 s | **1.75 s** | 1.07× |
| fl_w15 | 1.91 s | **1.73 s** | 1.10× |
| fl_auto | 1.87 s | **1.75 s** | 1.07× |

验证：`fastload_verify.py` **90/90 PASS**、`fastload_verify_fixed.py` **44/44 PASS**
（含唯一索引/UPSERT/重复处理用例，零回归）；release + debug 双构建 rc=0。

---

## 14. 代码整理（2026-11-09，code review）

对并行子系统做了一次以可维护性为目标的复查，**无行为变化**：

| 项 | 改动 |
|---|---|
| 死字段 | 删除 `FastloadShared.workers_ready`（从未被读取）；删除 `FastloadWorkerId.nworkers`（worker 数一律从 DSM 参数块读取） |
| 重复逻辑 | worker 数默认值原有两处实现（`ReaderInit()` 内联 `sysconf()` + 编排层），统一为 `FastloadDefaultWorkers()` 单一定义并在 `include/fastload_parallel.h` 导出 |
| 无用依赖 | 移除 `lib/reader.c` 的 `#include <unistd.h>`（`sysconf()` 移除后不再需要） |
| 未文档化别名 | 删除 `PARSE_WORKERS` 别名，只保留 `PARALLEL_WORKERS` |
| 内存 | `reader.c` 重新赋值 `parallel_null` 前先 `pfree` 旧值 |
| 并发正确性 | `SHM_MQ_DETACHED` 分支先把 `worker_error[i]` 复制到本地缓冲再释放 spinlock，避免把共享内存指针带出临界区 |
| 组合防护 | `FastloadParallelSupported()` 增加 `MULTI_PROCESS` / `WRITER=PARALLEL` 判定：与 `PARALLEL=ON` 同时指定时不叠加（回退串行 reader 并打印 INFO），避免两个进程扇出套叠 |
| 注释 | 修正 `fastload_worker.c` 中 SKIP/LIMIT 路由注释的错位；`t_start/t_end` 收进 `#ifdef ENABLE_FASTLOAD_PROFILE` |
| 生成物 | 删除仓库内遗留的 `lib/exports.list`（由 Makefile 从 `exports.txt` 生成，已在 `.gitignore`） |

回归：release + debug 双构建 rc=0（仅上游 `TYPES`/`ALIASES` 两条声明风格警告）；
CSV **90/90 PASS**、定长 **44/44 PASS**、`MULTI_PROCESS` 专项探针 **8/8 PASS**；
1 GiB 7 配置内容摘要 `7375042|9258128314029119699254` ALL_MATCH=True。
复测（3 轮中位）：fl_off 7.99 s、fl_w1 7.97 s、fl_w2 3.52 s、fl_w4 1.92 s、
fl_w8 1.76 s、fl_w15 1.74 s、fl_auto 1.76 s，与上一轮差异在 ±5% 运行间波动内。

---

## 15. 文档体系重构（2026-11-09）

| 改动 | 说明 |
|---|---|
| 新增 README（现为英/中双份） | `README.md` / `README.zh-CN.md`：定位、与 pg_bulkload 三张差异表（设计思路 / 功能特性 / 使用方式）、架构图与组件职责、快速开始、降级清单、限制 |
| 新增用户手册（现为英/中双份） | `docs/USER_MANUAL.md` / `docs/USER_MANUAL.zh-CN.md`：安装、控制文件语法、关键字全表、SQL/CLI、输入格式、并行调优、错误处理、诊断、FAQ、迁移与卸载 |
| 架构文档中英双版 | `docs/ARCHITECTURE.md` / `docs/ARCHITECTURE.zh-CN.md` 合并原 `PARALLEL_DESIGN.md` 内容并重写，追加架构图与组件职责表；删除 `PARALLEL_DESIGN.md` |
| 架构图 | 新增 `docs/img/architecture.svg`（英）与 `architecture.zh-CN.svg`（中），与现行单读多解实现一致 |
| 删除日文与遗留 HTML | 删除 `index_ja.html`、`pg_fastload-ja.html`、`pg_timestamp-ja.html` 及上游英文 HTML 手册（见 §4） |
| 文档归并 | 优化记录集中到 `docs/OPTIMIZATION.md`：正文按「已采纳」逐轮记录，评估后未采纳的方案集中在末章，不在本文逐轮出现 |

---

## 16. 多版本适配 PG 15 / 16 / 17 / 19（2026-11-10）

目标：同一份源码在 15.19 / 16.15 / 17.11 / 18.6 / 19rc1 上编译并通过同一套功能验收。
版本差异全部用 `#if PG_VERSION_NUM` 收敛，不做业务代码分叉。

| 文件 | 改动 |
|---|---|
| `lib/fastload_worker.c` | `populate_compact_attribute()` 仅 ≥18 调用（18 新增，早期版本按需计算） |
| `lib/fastload_parallel.c` | `get_database_name()` 头文件按版本选择（19 移到 `utils/lsyscache.h`）；补 `storage/proc.h`（`MyProc`） |
| `lib/writer_parallel.c` | 同上，`get_database_name()` 头文件按版本选择 |
| `lib/pgut/pgut-be.h` | 新增 `FastloadBackendNumber` / `FastloadInvalidBackend`：≥17 用 `MyProcNumber` / `INVALID_PROC_NUMBER`（19 移除了 `MyBackendId`），更早用 `MyBackendId` / `InvalidBackendId` |
| `lib/pg_btree.c` | 临时关系定位改用上述宏（同时修正 17 分支误用 `MyBackendType` / `InvalidCommandId` 的问题）；索引构建分支接入 `nbtsort-19.c` |
| `lib/writer_direct.c` | 同上宏；≥19 `PageAddItem()` 直接传 `const void *`（`Item` typedef 已删除）、`PageSetChecksumInplace()` → `PageSetChecksum()` |
| `lib/pg_strutil.c` | ≥19 `FuncnameGetCandidates()` 新增 `int *fgc_flags` 出参，且**无条件解引用**，改传一次性 `int[1]` 复合字面量（传 `NULL` 实测段错误） |
| `lib/nbtree/nbtsort-19.c` | 新增：PG19 的 `nbtsort.c`（`BTSpool` 已私有化、`palloc*_object` 等），与 18 版差 59 行 |
| `bin/pgut/pgut-list.h` | ≥19 补 `typedef uint64_t Datum;`（`Datum` 从 `c.h` 移入 `postgres.h`，前端不可用） |
| `bin/recovery.c` | ≥19 `pd_lsn` 改用 `PageXLogRecPtrGet()`（`PageXLogRecPtr` 变单 64 位值） |
| `util/fl_ver_test.sh` | 新增：单主版本功能验收脚本（9 类断言），`PG_PREFIX` / `SRC` / `ROWS` 可覆盖 |
| `docs/PORTING.md` / `PORTING.zh-CN.md` | 新增：支持矩阵、10 条内核差异与处理、验收方法与结果 |

验收（四个版本各跑一遍，全部 PASS）：CSV 串行/并行 5 配置摘要一致、定长 3 配置摘要一致、
`FILTER` 降级内容一致且 `LOGFILE` 有 `PARALLEL disabled (FILTER` 提示、解析错误默认停在第 1 条
坏记录而 `PARSE_ERRORS=2` 保留 2 条好记录、CLI 退出码 0 且装载 200,000 行、服务端日志无 PANIC。

顺带修正：用户手册 `PARSE_ERRORS` 的描述原为"超过则装载失败"，实际是**超过即停止读取并正常返回**
（已装载行保留，日志给 `Maximum parse error count exceeded`），中英文两版已改。

---

## 17. 与上游 pg_bulkload 的差分审计（2026-11-10）

方法：把上游 `pg_bulkload` 3.1.23 编译到**同一台服务器、同一个集群**当 oracle，用三套差分脚本把
同一份输入分别喂给 `pgbulkload.pg_bulkload()` 与 `pgfastload.pg_fastload()` 的三种配置
（`PARALLEL=OFF` / `PARALLEL=ON` w=2 / w=4），逐例比对 SRF 结果行
（`skip`/`count`/`parse_errors`/重复计数）、内容摘要
`count(*) || coalesce(sum(hash_record_extended(t,0)),0)` 与消息严重级。共 75 例：引号、转义、
CRLF、裸 CR、空行、缺列/多列、未闭合引号、类型转换失败、`SKIP`+`LIMIT` 窗口、唯一索引与主键冲突
（含 `ON_DUPLICATE_KEEP`）、`WRITER=BUFFERED/PARALLEL`、`MULTI_PROCESS`、`FORCE_NOT_NULL`、
`NULL`/`ENCODING`/自定义 `DELIMITER`+`QUOTE`+`ESCAPE`、`PARSE_ERRORS` 0/2、20 万行与 1 MiB
读块边界输入、定长（含 `SKIP`/`LIMIT`/尾部残记录）。

审计前串行路径 75 例全部正确；并行路径查出并修掉 4 个真缺陷。

| # | 缺陷 | 根因 | 修法 |
|---|---|---|---|
| 1 | 被拒记录直接打死 worker（连带整条语句），`count`/`parse_errors` 从不返回 | 并行 reader 自己抛 `unterminated CSV quoted field`，worker 的解析/转换错误也无人吸收（串行路径由 `ReaderNext()` 吸收） | worker 内 `PG_TRY`/`PG_CATCH`（`fl_emit_record_safe`），首个错误连同记录号、列号发布到控制块（`fl_record_parse_error`），记录追加到 `PARSE_BADFILE`，由 leader 上报；截断的末条记录改投给 worker 而不在 reader 报错 |
| 2 | `SKIP` 语义不一致：`SKIP=1` 并行按 1 条**记录**，上游按 1 **行** | `SKIP` 被 `fl_route_record()` 当成记录偏移；上游前导循环数物理行且不识别引号 | 新增 `fl_skip_lines()` 在记录扫描前走原始字节（`LF`/`CRLF`/裸 `CR`），`fl_route_record()` 只保留 `LIMIT` |
| 3 | 首个坏记录之后仍继续装载：oracle 10 万行，w=2/4/8 分别装成 **15.6/17.6/19.4 万**行且每次不同 | 坏记录之后的行已在其它 worker 队列里，"停止读取、保留已读"在并行下无法复现 | 并行契约改为**原子**：先打同样的 WARNING，再 `ERROR` 整体回滚；永不多装、与 worker 时序无关（见 USER_MANUAL §11） |
| 4 | 被跳过的超长行（> 1 MiB）导致堆溢出 | `fl_skip_lines()` 往固定 `FL_READ_CHUNK + 2` 缓冲累加（记录扫描每轮会把余量消费掉，被跳过的行不会）；SKIP 后第一轮扫描也有同样假设 | 两处改为 `repalloc` 按需增长 |

附带：8 字节帧头第二字改携带记录号（`hdr8[1]`），leader 侧消息能报出输入记录号，与串行一致。

审计可复现：`util/fl_diff_test.py` 是自包含版本（44 例 × 4 配置，不依赖 SSH/VM），只需一个装了
两个扩展的集群。

修后结果：**0 mismatch** —— 75 例 × 3 配置 = 225 次比对，其中 30 次是已文档化的并行原子中止
（`PARALLEL=OFF` 与 oracle 每一例都完全一致）。全套在 **15.19 / 16.15 / 17.11 / 18.6** 上重跑结果
相同（合计 900 次比对）；四个版本的单主版本验收（`util/fl_ver_test.sh`）与
`fastload_verify.py` 90/90、`fastload_verify_fixed.py` 44/44 同时通过。PG19 无 oracle：上游
3.1.23 在 19 上编译失败（正是我们 `bin/pgut/pgut-list.h` 修掉的 `Datum` 位置问题），改由功能验收
与串并行自洽覆盖。

---

## 18. 装载进度：`pg_fastload_progress` 视图（2026-11-10）

`pg_fastload()` 是一次 SQL 调用，大文件跑几分钟也看不到任何输出。而让装载会话自己"打印"进度也
解决不了问题：忙的正是那个会话，它的客户端被一条尚未结束的语句阻塞着。因此进度改为**发布**出来，
从别处读取：

```sql
-- 会话 1：接下来半小时都很忙
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/lineitem.csv', 'OUTPUT=public.lineitem', 'PARALLEL=ON']);

-- 会话 2、监控采集程序，或同实例的另一个数据库
SELECT * FROM pgfastload.pg_fastload_progress;
```

```
 pid  | datname |    relname      | input_type |  mode    | state   | elapsed_seconds |  rows   | bytes_read | total_bytes
------+---------+-----------------+------------+----------+---------+-----------------+---------+------------+-------------
 4121 | tpcc    | public.lineitem | CSV        | parallel | running |            3.42 | 3687521 |  536870912 |  1073741824
```

视图共 20 列：`pid`、`backend_start`、`started`、`direction`（第 20 节加入，`load`/`export`）、
`datname`、`relname`、`infile`、`input_type`、`mode`、`state`、`elapsed_seconds`、`rows`、
`bytes_read`、`total_bytes`、`total_rows`、`percent`、`bytes_per_second`、`rows_per_second`、
`eta_seconds`、`interval_ms`。

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `PROGRESS` | `ON` / `OFF` | `ON` | 为本次装载发布状态记录 |
| `PROGRESS_INTERVAL` | `100`..`3600000` 毫秒 | `1000` | 两条记录之间的最小间隔；**指定即等价于 `PROGRESS=ON`** |
| `PROGRESS_NOTICE` | `ON` / `OFF` | `OFF` | 额外向装载客户端与 `LOGFILE` 输出 `NOTICE` 进度行 |

```
NOTICE:  pg_fastload progress: start, input 1.00 GiB, interval 1000 ms
NOTICE:  pg_fastload progress: 12.6% | rows 927,744 | 129.0 MiB / 1.00 GiB | 122.8 MiB/s | 883.1k rows/s | elapsed 00:00:01 | ETA 00:00:07
NOTICE:  pg_fastload progress: 100.0% | rows 7,375,042 | 1.00 GiB / 1.00 GiB | 149.0 MiB/s | 1.07M rows/s | elapsed 00:00:06 | done
```

| 文件 | 改动 |
|---|---|
| `include/progress.h`、`lib/progress.c` | 重写：原子替换的状态记录（`$PGDATA/pg_fastload/progress/<pid>.status`）、按 pid 存活状态清理陈旧记录、把记录变成行的 `pg_fastload_progress()` SRF |
| `lib/pg_fastload.sql.in` | `pgfastload.fl_progress_rows()`（20 个 OUT 列，`RETURNS SETOF record`，`EXECUTE` 从 `PUBLIC` 收回）与 `pgfastload.pg_fastload_progress` 视图 |
| `lib/exports.txt` | `pg_fastload_progress`、`pg_finfo_pg_fastload_progress` |
| `include/reader.h`、`lib/reader.c` | `PROGRESS` 默认改为 `ON`；新增 `PROGRESS_NOTICE`；保留 `Reader.type` 供记录使用；三者都写入装载日志 |
| `lib/pg_fastload.c` | 组装一份 `ProgressInfo`（库、表、`TYPE`、`serial`/`parallel`）；错误路径上 `ProgressAbort()` |
| `lib/fastload_parallel.c` | 除注释外无改动：仍由 leader 发布，每 1024 行采样一次 reader 的字节计数 |
| `util/fl_progress_test.py` | 验收脚本重写：一个连接发起装载，另一个连接轮询视图 |

设计要点：

- **用状态文件而不是共享内存**：PG 直到 17 才有命名 DSM 注册表（`GetNamedDSMSegment`），而本代码树
  支持 15 及以上；改用普通 SysV 共享内存又会让每个部署都被迫打开 `shared_preload_libraries`。文件
  两样都不需要，且是集群级的（在 `tpcc` 里发起的装载，从 `postgres` 也看得到），还能安全降级：
  后端被 kill 后留下的记录仍可读，只是 pid 不再存活。
- **原子替换**：每次刷新先写 `<pid>.tmp` 再 `rename()` 覆盖 `<pid>.status`，读取方永远看不到写了一半
  的行；发布失败只关闭本次装载的发布并记一行日志，绝不会让装载失败。
- **百分比来源**：给了 `LIMIT` 就按 `rows / LIMIT`（请求的行到齐即算完成，即使 reader 故意提前停）；
  否则按 `bytes_read / 文件大小`；两者都没有（STDIN、管道）则 `percent` 为 `NULL`，字节／行速率照常。
- **单调不减、不说谎**：记录的百分比由单调计数器算出；`NOTICE` 流额外把周期行封顶 `99.9%`，因为
  reader 可能已到 EOF 而 leader 还在插最后几行。
- **结束与失败**：`ProgressFinish()` 删除记录（视图回到 `0 行`）；错误路径上的 `ProgressAbort()`
  同样删除但不打收尾行——因解析错误契约（第 17 节）停下的装载不算"完成"。被 `SIGKILL` 的后端
  记录会以 `state = 'stale'` 被报告一次，随后由下一个读取者删除。
- **开销**：每插入一行一次整数比较；每 512 行取一次时钟；约 1 KiB 的记录、无 `fsync`，最多每个间隔
  重写一次。3M 行串行装载实测在跑动噪声内（±2%）。

验证（`util/fl_progress_test.py`，随扩展交付；PG 15.19 / 16.15 / 17.11 / 18.6 / 19rc1 各 8 项）：
视图能看到运行中的装载，数据库、表、`TYPE`、模式、间隔均正确，结束后回到空；`PARALLEL=ON` 报告
`mode = parallel`；`LIMIT` 把 `percent` 切成 `rows / LIMIT` 且 `total_rows` 有值；`PROGRESS=OFF`
全程不发布；只给 `PROGRESS_INTERVAL` 也能开启；`PROGRESS_NOTICE=ON` 追加 NOTICE 流，收尾行带
`100.0%` 与精确行数；`PROGRESS_INTERVAL=50` 报错
`PROGRESS_INTERVAL must be between 100 and 3600000 milliseconds`；行数与百分比单调且落在 `0..100`。
1 GiB 装载的跨会话轮询（`_t_view.py`）显示：不在装载的那个会话里，行数从 1.1M 一路涨到 6.5M。
CSV 90/90、定长 44/44 与多版本验收均不受影响。

## 19. 单分支 master（2026-11-10）

早期版本发布过五个分版本分支 —— `REL_15_STABLE` … `REL_19_STABLE`。它们其实是**同一个提交**的
指针：`git diff REL_15_STABLE REL_18_STABLE` 为空，任意两支之间都为空。保留它们会让人以为有五个
开发线，实际上只有一条；一个修复还得推五次才能保持一致。

仓库现在只有 `master` 一个分支，支持范围写在源码里而不是分支名里：

| 文件 | 改动 |
|---|---|
| `include/pg_fastload.h` | 原 `PG_VERSION_NUM < 80300` 守卫改为 `< 150000`：15 以下直接 `#error pg_fastload requires PostgreSQL 15 or later`，未经测试的组合不可能静默产出二进制 |
| `docs/PORTING.md`、`PORTING.zh-CN.md` | 支持矩阵去掉「分支」列；分支约定改写为「只有一个分支 `master`」 |
| `README.md`、`README.zh-CN.md` | 「每一代主版本有对应的 `REL_1x_STABLE` 分支」改为单分支、差异由 `PG_VERSION_NUM` 收敛 |
| 交付说明（中英） | 构建章节同步该表述 |
| `make_fastload_deliverables.py` | 多版本检查改为断言单分支策略，不再断言五个分支名 |

为什么单树够用：15 与 19 之间实际遇到的每处差异本来就是 `#if PG_VERSION_NUM` 分支
（[PORTING.md](PORTING.md) §3 列了约 30 处），加上由同一个宏选中的按版本内置 nbtree 构建器。
没有任何运行期选择，除 `PG_CONFIG` 外不需要记住额外编译开关。

验证方式：用 `git archive` 导出 `master`（不借助工作区），再从该导出目录逐版本编译 ——
15.19 / 16.15 / 17.11 / 18.6 / 19rc1 全部 `INSTALL_OK`，`CREATE EXTENSION` 成功，同一文件串行与
`PARALLEL=ON` 装载的内容摘要一致，进度视图存在。证据：`compat_master_pg1x.out`。

五个分版本分支已从本地与远端删除。日常开发使用 `master`；远端残留的 `REL_18_STABLE` 是原先的
默认分支指针，在 Settings → Branches 里把默认分支切到 `master` 后即可删除。

---

## 20. 导出：`pg_fastload_export()`（2026-11-10）

装载一直是单向的：文件 → 表。需要把表写成文件时，用户只能退回 `COPY`；上游虽然有一个
`WRITER=BINARY`，但它只能把**本次装载的输入流**序列化成定长文件，起点仍然是 `INPUT=`，
没有任何办法从一张表出发，CSV 更是连写者都没有。这一节补上反方向：

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV']);
-- 7375042
```

### 20.1 为什么是「同一套 Writer + SPI 游标」

| 决策 | 理由 |
|---|---|
| 复用 `Writer` 抽象而不是新写一条输出管线 | 装载的 `COL` 描述符已经定义了定长文件的每一个字节；导出用 `OUT_COL` 描述同一件事，往返能力就是免费的 |
| 扫描用 `SPI_cursor_open_with_args(NULL, q, ..., read_only=true)` | `TABLE=` 与 `QUERY=` 才能共用一条路径；游标自带调用方快照、权限检查与行可见性，和普通 `SELECT` 完全一致；非 `SELECT` 由游标本身拒绝 |
| CSV 新写一个 `CsvWriter` | 装载侧只有 `parser_csv.c`（读），没有任何写 CSV 的代码；`COPY` 的引号规则必须逐条复刻才能保证字节相同 |
| BINARY/FIXED 复用 `BinaryWriter` | 它本来就是「把元组按 `COL` 布局写文件」的那一半，装载时用于 spool；导出正是它的第二个用户 |
| 每批 1000 行 `SPI_cursor_fetch` | 摊薄 SPI 往返，又不让宽行在两次取数之间把 SPI 上下文钉住几百 MB |

新增/修改文件：

| 文件 | 内容 |
|---|---|
| `lib/export.c`（新） | `pg_fastload_export()`：两遍解析选项（先定 `TYPE` 决定 writer，再把其余关键字交给该 writer）、SPI 游标扫描、逐行 `WriterInsert`、进度发布、错误路径 `ProgressAbort` + `WriterClose(onError)` |
| `lib/writer_csv.c`（新） | `CreateCsvWriter()`：`CsvWriterInsert` 逐列 `getTypeOutputInfo` + `OutputFunctionCall`，`csv_put_field()` 复刻 `CopyAttributeOutCSV()` 的五条引号规则；4 MiB 缓冲；`DELIMITER`/`QUOTE`/`ESCAPE`/`NULL`/`HEADER`/`OUTPUT`；方言在打开文件前校验（分隔符与引号不能相同、都不能是换行、`NULL` 串不能含分隔符或引号） |
| `include/export.h`（新） | 函数声明 |
| `include/writer.h` | `CreateCsvWriter()`、`BinaryWriterSetSourceDesc()`；`struct Writer` 增加 `bytes`（已产出字节，进度用）与 `sample_output`（样例控制文件里的来源注释） |
| `lib/writer_binary.c` | 新增 `BinaryWriterSetSourceDesc()`；`bytes` 累计；NULL 无 `NULLIF` 时的报错信息改为可操作的 `column N is NULL and OUT_COL gives it no representation` + hint；样例控制文件加一行 `# exported from <table>` |
| `lib/progress.c`、`include/progress.h` | 状态记录增加 `direction`（`load`/`export`）；只有装载才计算 `total_bytes`；视图第 4 列 `direction`，共 20 列 |
| `lib/pg_fastload.sql.in` | `pgfastload.pg_fastload_export(text[]) RETURNS bigint`；`fl_progress_rows()` 增加 `OUT direction text` |
| `lib/exports.txt` | `pg_fastload_export`、`pg_finfo_pg_fastload_export` |
| `lib/Makefile` | `SRCS` 增加 `export.c`、`writer_csv.c` |

### 20.2 三个踩过的坑

1. **游标元组没有类型 OID。** `BinaryWriter` 的 checker 原本从元组头部的 `HeapTupleHeaderGetTypeId`
   拿源类型（装载时元组由目标表的 `TupleDesc` 现造，一定有）。游标交出来的元组没有，于是报
   `type with OID 0 does not exist`。修法：导出显式调用 `BinaryWriterSetSourceDesc(wt, desc)`，
   把结果行的 `TupleDesc` 交下去，并把 checker 切成 `BinaryWriterCheckerTuple`（唯一会做宽度
   与取值范围校验的变体）。
2. **样例控制文件的 `OUTPUT` 不能指向源表。** 上游的默认值是文件名去掉扩展名；一度改成源表名
   看似更贴心，实测 `pg_fastload b.bin.ctl` 直接把 4 行追加回源表（`TRUNCATE = NO`），把它变成
   8 行。现在的做法：`OUTPUT` 保持文件名，另加一行 `# exported from public.t` 说明来源 —— 手滑
   只会得到 `relation "b" does not exist`，不会改数据。
3. **定长格式的文本列没有「精确往返」。** 字段一律补空格到声明宽度，而装载端 `Read_char`
   （`COL=CHAR(n)`）裁尾随空格、`Read_varchar`（`COL=VARCHAR(n)`）原样保留。所以 `'xy'` 写进
   8 字节字段后，用 `VARCHAR(8)` 回灌得到 `'xy      '`，用 `CHAR(8)` 才还原成 `'xy'`（代价是
   原值自带的尾随空格会丢）。这是格式的性质而非缺陷，写进手册 §18.5 并用两条断言钉住：
   同一份 `.bin`，`CHAR(8)` 回灌摘要等于源表、`VARCHAR(8)` 回灌摘要等于补位后的内容。

### 20.3 性能

导出是单进程串行的（源是游标、目标是单文件，没有读放大可摊），因此不接 `PARALLEL`。
与 `COPY` 和"把同样的字节装回去"分别对照后：

| 操作 | 800 万行 / 521 MB 表 / 312 MiB CSV（热缓存，5 次中位） |
|---|---|
| `SELECT count(*)`（16 核并行顺序扫描） | 0.05 s |
| `COPY ... TO '/dev/null' (FORMAT csv)` | 0.70 s |
| `COPY ... TO '<文件>' (FORMAT csv)` | 0.80 s |
| `pg_fastload_export(TYPE=CSV)` → `/dev/shm` | 0.86 s |
| `pg_fastload_export(TYPE=CSV)` → 磁盘 | **0.90 s** |
| `pg_fastload_export(TYPE=BINARY)` → 磁盘 | 0.94 s |
| `pg_fastload_export(QUERY=SELECT id ...)` | 0.49 s |
| 同一份 CSV 装回来，`PARALLEL=OFF` | 1.06 s |
| 同一份 CSV 装回来，`PARALLEL_WORKERS=4` | 0.48 s |

第一轮测量拿 `psql` 的 `\copy`（0.95 s）当基准，得出"导出略快"的结论，这是错的：`\copy` 要为
800 万行走一趟客户端往返。服务端 `COPY` 是 0.80 s，导出实际慢约 12% —— 一个共享 writer 同时
服务 CSV/BINARY/FIXED，输出逐字节相同，整个 0.90 s 里只有 0.04 s 花在文件上。

定长也不比 CSV 便宜（0.94 s 对 0.90 s）：它的布局 pass 要先跑 tuple checker，建强制转换后的
元组、把每个字段拷进声明宽度。

### 20.4 验证

`util/fl_export_test.sh`（随扩展交付，PG 18.6）共 39 项断言，全部通过：

- **CSV 逐字节等于 `COPY`**，且 CSV 往返（`SKIP=1` 跳过表头）内容摘要相同；
- **方言**：`DELIMITER=|` + `QUOTE='` + `NULL=\N` 导出后原样回灌，摘要不变（含内嵌换行、
  字段内含分隔符/引号、值为字面量 `\N` 的区分）；
- **`QUERY=` + `LIMIT=`**：`WHERE`/`ORDER BY` 生效（首行 id=5），`LIMIT=2` 只写 2 行；
- **空表** → 0 行、0 字节文件；
- **BINARY**：7 列定长 44 B/行 × 4 行 = 176 B；`CHAR(8)` 回灌摘要等于源表；样例控制文件
  记录来源且 `OUTPUT` 为文件名；`VARCHAR(8)` 回灌保留补位空格（文档化行为）；
- **FIXED**：纯文本布局（数值列用 `CHAR(n)` 承载）回灌摘要等于源表；
- **`NULLIF`** 让定长格式保住 NULL：3 行含 NULL 的表往返摘要相同；
- **11 条错误路径**：缺 `OUTPUT`、相对路径、`BINARY` 缺 `OUT_COL`、`FIXED` + `INTEGER`、
  `OUT_COL` 数量不符、目标文件已存在、NULL 未声明 `NULLIF`、未知关键字、缺 `TABLE`/`QUERY`、
  非 `SELECT` 的 `QUERY`、`DELIMITER` 等于 `QUOTE`；
- **进度**：800 万行导出期间另一会话读到 `export|running|CSV|serial|...`，结束后视图回到 0 行。
  （此时只有串行一种模式；自 §20.5 起同一张表报的是 `mode=parallel`。）

装载侧的回归：CSV 90/90、定长 44/44 不受影响（导出只新增了 writer 与 `bytes` 计数，
`WriterInsert`/`ReaderNext` 路径未改）。

### 20.5 并行导出：N 个查询进程 + 1 个写进程（2026-11-10）

§20 当时说导出单进程、要并行就得「各 worker 写临时文件 + 有序拼接，等于给整个输出补一次完整读+写」。
那个估算的前提是**必须保住行序**。其实不必：导出是转储，契约只是「每行恰好一次」。一旦放开行序，
拼接就完全消失，装载的进程模型可以直接反向复用：

| 装载（`PARALLEL=ON`） | 导出（`PARALLEL=ON`） |
|---|---|
| 1 个 reader bgworker 单遍扫文件 | N 个查询 bgworker 各扫一段互不相交的 `ctid` 块区间 |
| N 个 parser bgworker 造元组 | N 个查询 bgworker 把行格式化成字节 |
| leader（后端）做串行 `WriterInsert` | 1 个 writer bgworker 做串行 `write(2)` + `fsync` |
| reader → worker 走 `shm_mq` | worker → writer 走 `shm_mq`，每个 worker 一条队列 |

新增文件：

| 文件 | 内容 |
|---|---|
| `include/export_parallel.h` | DSM 布局（`FlExportShared` / `FlExportParams` / `FlExportChunk`）、key、队列与分块大小、请求结构 |
| `lib/export_parallel.c` | 写进程、查询进程、`FlExportParallelSupported()` 降级判定、`FlExportParallelTry()` 编排 |

修改文件：

| 文件 | 改动 |
|---|---|
| `lib/export.c` | 解析 `PARALLEL=` / `PARALLEL_WORKERS=`；把 writer 选项序列化成 `KEY\0value\0` blob；`ResolveExportRelid()`（RangeVar → Oid，不走 SPI）；`ExportParallelAttempt()` 分发与回退；原串行 SPI 游标块变成 `else` 分支 |
| `lib/writer.h`、`lib/writer_csv.c`、`lib/writer_binary.c` | 初始化拆成 `Prepare`（方言/outfunc/OUT_COL 布局，不涉及文件）+ `Init`（再加文件），并加 `StringInfo` **sink**：有 sink 时 writer 只往调用方内存追加，完全不碰文件 |
| `lib/Makefile`、`lib/exports.txt` | 编译 `export_parallel.c`；导出两个 bgworker 入口（10、11） |
| `util/fl_export_parallel_test.sh` | 新增验收脚本（33 项断言） |

三个值得记录的设计点：

1. **用 `heap_setscanlimits()` 而不是 `ctid` 谓词。** `WHERE ctid BETWEEN ...` 只是把过滤下推，
   每个 worker 仍要扫完整张表（N 倍读放大）；把区间塞进扫描描述符后，每个 worker 从自己的块开始、
   到自己的块结束。分片公式 `nblocks * i / nworkers` 在启动前算好 —— 没有共享队列、没有任务窃取、
   数据通路上没有锁。
2. **快照与 writer 选项以字节形式跨进程。** 每个 worker 用 `RestoreSnapshot()` 恢复 leader
   `SerializeSnapshot()` 的结果，这样所有 worker 看到同一个快照（不会扫到之后插入的行）。writer
   选项走 `KEY\0value\0` blob，因为 `Writer` 里存的是函数指针，它活不过 bgworker 的 `fork()`/`exec`。
3. **写进程不认格式。** 一条消息就是 `{nrows, nbytes}` 加一段字节，因此 CSV 与 BINARY 共用一个写进程；
   表头由写进程在第一块之前写一次，这才让 `HEADER=ON` 在任何调度下都正确。

`LIMIT=` 依然精确：`pg_atomic_fetch_add_u64(&rows_reserved, CHUNK_ROWS)` 让各 worker 随意超订，
最后一个把本块裁到剩余额度，于是行数精确，而**是哪几行**不确定。

写验收脚本时抓到一个语义 bug：`PARALLEL_WORKERS=1` 被当成「没指定」（0 才是哨兵）而被默认值
`CPU−1` 顶掉。修法是在套默认值之前直接拒绝 `nworkers == 1`，并用一条断言钉住日志里出现
`PARALLEL disabled (workers=1)`。

### 20.6 并行导出的性能

`public.lineitem`（7,375,042 行、1.13 GiB CSV），输出在 tmpfs，5 轮取中位：

| 配置 | 耗时 | 相对串行 |
|---|---|---|
| `COPY ... TO` | 3.956 s | 1.03× |
| `PARALLEL=OFF` | 4.077 s | 1.00× |
| 2 个 worker | 1.950 s | 2.09× |
| 4 个 worker | 0.999 s | 4.08× |
| 8 个 worker | 0.611 s | 6.68× |
| auto（15 个 worker） | **0.424 s** | **9.60×** |

15 个 worker 时管线以 2.85 GB/s 推过**一个** `write(2)` 循环：瓶颈从 worker 变成写进程 —— 这正是装载侧
「串行插入下限」在导出方向的镜像。完整表格、以及为什么这台机器的磁盘数字不能用（裸 `write`+`fsync`
微基准显示：块大小不变时 `fsync()` 会在 0.1 s 与恰好 30 s/60 s 之间跳变）见
[BENCHMARK §8](BENCHMARK.zh-CN.md#8-导出吞吐pg_fastload_export-与-copy-to-对比)。

### 20.7 验证

`util/fl_export_parallel_test.sh`（随扩展交付）在 PG 18.6 上跑 33 项断言：串行基线；4 worker 的 CSV
导出与串行**行集**相同、表头只出现一次、摘要往返、日志里 `workers=4`；`LIMIT` 跨 worker 精确且不重复；
默认 worker 数（`CPU−1`）；并行 BINARY（文件大小、样例 `.ctl` 来源、回灌摘要）；三条串行回退
（`QUERY=`、小表、`PARALLEL_WORKERS=1`）各自断言日志行；三条被拒输入（输出已存在、`.ctl` 已存在、
`PARALLEL_WORKERS=0`）；运行中的 `mode='parallel'` 进度记录。

33 项在 **15.19 / 16.15 / 17.11 / 18.6 / 19rc1** 上全部通过（合计 165 项）。PG 19 需要一处编译修正：
`table_beginscan()` 多了一个 `ScanOptions` 参数，调用点现在走 `FL_TABLE_BEGINSCAN()`。装载侧套件不受影响
（CSV 90/90、定长 44/44）。

---

## 21. 串行 CSV 路径：批量切分（第八轮，2026-11-10）

### 21.1 先把两个说法变成数字

动代码之前有两个断言需要先证实：「`PARALLEL=OFF` 就是 pg_bulkload 的循环，耗时应与它一致」，以及
「串行比 `COPY` 慢」。两者此前都没有在完全同条件下量过，所以第一步是四路对照（同一文件、同一空表、
`CHECKPOINT` 在计时之外、各 3 轮）：

| 配置 | 中位 | 相对上游 |
|---|---|---|
| 上游 `pg_bulkload 3.1.23` | 7.00 s | 1.00× |
| `pg_fastload PARALLEL=OFF` | 6.95 s | 1.02× |
| `COPY ... FROM`（普通表，走 WAL） | 8.48 s | 0.83× |
| `COPY ... FROM`（UNLOGGED，数据不写 WAL） | 7.67 s | 0.91× |

结论：第一个说法成立（差 2%，同一条路径）；第二个说法不成立 —— 串行装载本来就比 `COPY ... FROM`
快 1.22×，甚至比往 UNLOGGED 表 `COPY` 还快。「COPY 更快」的印象来自 `COPY ... TO`，那是**导出**
方向（同表 3.96 s），做的是另一件事，见 §20。

于是问题变成：这 6.95 s 到底花在哪。原来的 profile 只能给到 `READER 6.69 / WRITER 1.25`，
所以在 `CSVParserRead()` 内部补了两个计数器 —— `<CSV> SPLIT`（字节 → 字段指针）与
`<CSV> VALUE`（字段指针 → Datum + `heap_form_tuple`）：

| 分段 | 耗时 | 占比 |
|---|---|---|
| `<CSV> SPLIT` | 1.92 s | 23% |
| `<CSV> VALUE` | 4.04 s | 47% |
| `WRITER`（填页 + 块落盘） | 1.16 s | 14% |

串行路径有四分之一是逐字符状态机，而这一段可以整块去掉且不改任何语义。

### 21.2 改动

`CSVParserRead()` 的参考实现原样保留；一条记录只有在**既无引号、也无回车**时才走新的快路径：

1. 一次 `memchr('\n')` 在已缓冲窗口内找到记录末尾；
2. 一次预扫描拒绝引号与 `\r`，因此带引号的多行字段、转义引号、lone-CR 文件仍走状态机；
3. 记录**整条**复制进字段缓冲一次，随后把每个分隔符字节就地改成上一字段的 NUL 结尾 ——
   每行 16 次 `memchr` + 16 次写入，取代约 150 次分支判断。

快路径从不改动记录缓冲，因此错误落盘（`ParserDumpRecord`）、`strlen(self->cur)` 与缓冲补数据
看到的仍是原始字节。`field_num`、`parsing_field`、`base.count` 与 `checkFieldIsNull()` 完全复刻
慢循环的记账规则，包括「多余列覆盖最后一个目标列」与「空行视为一个 NULL 字段」。

同一份 profile 还顺带暴露两件小事：

- 没给 `ENCODING` 时 `CheckerConversion()` 仍被逐字段调用；现在整段 1.18 亿次调用被
  `if (checker->check_encoding)` 挡住；
- 同一个批量切分器也加到了 `fastload_worker.c` 的 `fl_parse_fields()`，并行 worker 一并受益
  （worker 手里的记录不含行结束符，只需判引号）。

套件当场抓到一个回归并修掉：第一版在**判定是否可走快路径之前**就把行尾 `\r` 写成 `\0`，
导致随后回退到慢循环时缓冲已被破坏（`quoted`/`cross_chunk` 两个用例：串行只剩 2 行、并行 5 行）。
现在先判定、成功后才改字节；`fastload_verify.py` 的两个用例正好钉住这条路径。

### 21.3 性能

1 GiB CSV / 7,375,042 行，热缓存，3 轮中位：

| 配置 | 改动前 | 改动后 | 变化 |
|---|---|---|---|
| `PARALLEL=OFF` | 7.99 s | **5.81 s** | −27% |
| `PARALLEL_WORKERS=2` | 3.52 s | **2.76 s** | −22% |
| `PARALLEL_WORKERS=4` | 1.92 s | **1.58 s** | −18% |
| `PARALLEL_WORKERS=8` | 1.76 s | **1.40 s** | −20% |
| `auto`（15） | 1.76 s | **1.37 s** | −22% |

串行路径现在是**上游的 1.22×**、**`COPY ... FROM` 的 1.43×**；并行是串行的 4.24×、
是 `COPY ... FROM` 的 **6.07×**，下限仍是 leader 串行 `WriterInsert`（≈1.14 s）。
（§21.1 的四路表是改动前的测量，方法相同；此处的比值取自改动后的批次，其 `COPY` 数值为 8.32 s。）
`<CSV> SPLIT` 1.92 → 0.90 s；`<CSV> VALUE`
（3.82 s）已占串行路径 68%，它就是 `COPY` 同样要做的逐字段工作，这块没有批量技巧可挖。

### 21.4 验证

PG 18.6 上 `fastload_verify.py` **90/90**、`fastload_verify_fixed.py` **44/44**，其中
quoted / lone-CR / cross-chunk / 空行 / 多余列这些用例正好钉住慢路径的回退；**15.19 / 16.15 /
17.11 / 18.6 / 19rc1** 全量编译通过；1 GiB 摘要在 `PARALLEL=OFF` 与 `auto` 上均为
`7375042|9258128314029119699254`。

---

*未采纳方案汇总见 [OPTIMIZATION.zh-CN.md](OPTIMIZATION.zh-CN.md#八未采纳方案汇总)；性能数据见
[BENCHMARK.md](BENCHMARK.md)；验证结论见 [VERIFICATION.md](VERIFICATION.md)。*
