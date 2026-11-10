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

## 18. 装载进度显示（2026-11-10）

`pg_fastload()` 是一次 SQL 调用，大文件跑几分钟也看不到任何输出。新增两个关键字解决这个问题：

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `PROGRESS` | `ON` / `OFF` | `OFF` | 按固定间隔输出一行进度 |
| `PROGRESS_INTERVAL` | `100`..`3600000` 毫秒 | `1000` | 两行之间的最小间隔；**指定即等价于 `PROGRESS=ON`** |

```
NOTICE:  pg_fastload progress: start, input 1.00 GiB, interval 200 ms
NOTICE:  pg_fastload progress: 12.6% | rows 927,744 | 129.0 MiB / 1.00 GiB | 122.8 MiB/s | 883.1k rows/s | elapsed 00:00:01 | ETA 00:00:07
NOTICE:  pg_fastload progress: 100.0% | rows 7,375,042 | 1.00 GiB / 1.00 GiB | 149.0 MiB/s | 1.07M rows/s | elapsed 00:00:06 | done
```

| 文件 | 改动 |
|---|---|
| `include/progress.h`、`lib/progress.c` | 新模块：单位格式化（字节/速率/`hh:mm:ss`/千分位）、间隔节流、`NOTICE` + `LOGFILE` 双写 |
| `include/reader.h` | `Parser.bytes_read`（百分比来源）；`Reader.progress` / `progress_interval` |
| `lib/reader.c` | 新增 `PROGRESS` / `PROGRESS_INTERVAL` 参数解析与范围校验，并写入装载日志 |
| `lib/parser_csv.c`、`lib/parser_binary.c` | 每次 `SourceRead()` 累加 `bytes_read`（含被 SKIP 掉的记录，百分比因此覆盖 SKIP 段） |
| `lib/pg_fastload.c` | 日志就绪后 `ProgressInit()`；串行循环内 `ProgressUpdate()` / `ProgressFinish()` |
| `include/fastload_parallel.h` | `FastloadShared.bytes_read`，由 reader 进程发布 |
| `lib/fastload_worker.c` | CSV 与定长两条 reader 路径每次 `read()` 后 `fl_publish_bytes()`（每 1 MiB 一次 spinlock） |
| `lib/fastload_parallel.c` | leader 每插入 1024 行采样一次 `bytes_read` 并 `ProgressUpdate()`；即将中止的装载不打印收尾行 |

设计要点：

- **百分比来源**：给了 `LIMIT` 就按 `行数 / LIMIT`（请求的行到齐即算完成，即使 reader 故意提前停）；
  否则按 `已读字节 / 文件大小`；两者都没有（STDIN、管道）则不显示百分比。
- **单调不减、不说谎**：周期行最高 `99.9%`，`100.0%` 只属于收尾行——reader 可能已到 EOF 而 leader
  还在插最后几行。
- **开销**：每插入一行一次整数比较，每 512 行才取一次时钟。1 GiB 串行基线实测：不开 6 922 ms、
  开 6 917 ms（三次交错运行中位数，−0.07%，即噪声范围内）。
- **中止不打收尾行**：因解析错误契约（第 17 节）停下的装载不算"完成"。

验证（`_t_progress.py`，PG 18.6，15 个用例）：默认与 `PROGRESS=OFF` 零输出；只给
`PROGRESS_INTERVAL` 也能开启；串行 1 GiB 在 200 ms 间隔下输出 36 行；`PARALLEL=ON` w=4 输出 10 行、
auto 输出 3 行；`LIMIT=1000000` 百分比切换为 `of 1,000,000 rows`；定长 100 MiB 输出 3 行；
`PROGRESS_INTERVAL=50` 报错 `PROGRESS_INTERVAL must be between 100 and 3600000 milliseconds`；
每例收尾行的行数与函数返回值完全一致；百分比单调且不超过 100。CSV 90/90、定长 44/44 与多版本
验收均不受影响。

---

*未采纳方案汇总见 [OPTIMIZATION.zh-CN.md](OPTIMIZATION.zh-CN.md#八未采纳方案汇总)；性能数据见
[BENCHMARK.md](BENCHMARK.md)；验证结论见 [VERIFICATION.md](VERIFICATION.md)。*
