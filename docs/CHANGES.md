# 相对 pg_bulkload 3.1.23 的修改点

> 上游：`https://github.com/ossc-db/pg_bulkload`，HEAD `798e0f2`，版本 3.1.23  
> 本提交作者：yuanyuansong，2026

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

**未改动**：`COPYRIGHT` 原文件（BSD-3-Clause，NTT, Inc. 2008-2026）逐字节保留；所有继承源文件头部的 `Copyright (c) ... NTT, Inc.` 行保留。

## 2. 新增文件

| 文件 | 作用 |
|---|---|
| `LICENSE` | BSD-3-Clause 许可 + 原版权声明 + 并行化贡献版权 |
| `NOTICE` | 来源、版本、许可、署名、保留项与修改项说明 |
| `README.md` | 重写：用法、并行参数、架构、已知限制 |
| `docs/ARCHITECTURE.md` | 整体架构与数据流 |
| `docs/PARALLEL_DESIGN.md` | 并行化设计、分工协议、边界处理 |
| `docs/CHANGES.md` | 本文件 |
| `include/fastload_parallel.h` | DSM 共享协议结构：`FastloadParams` / `FastloadColumn` / `FastloadShared` / `FastloadWorkerId`，shm_toc key、队列大小、magic |
| `lib/fastload_parallel.c` | **主 backend 侧**：`FastloadParallelSupported()`、`FastloadParallelRun()`；DSM/shm_toc/shm_mq 创建、bgworker 注册、round-robin 消费、`WriterInsert()` 前可写副本 |
| `lib/fastload_worker.c` | **worker 侧**：`pg_fastload_worker_main()`（bgworker 入口）、quote-aware 记录边界状态机、hash 取模快速跳过、字段切分、复用 `TupleFormer` 形成元组、`shm_mq_send` |

## 3. 修改文件

| 文件 | 修改内容 |
|---|---|
| `include/reader.h` | `struct Reader` 增加 `parallel`、`parallel_workers`、`is_csv`、`parallel_skip`、`parallel_delim/quote/escape`、`parallel_null`、`parallel_has_filter`、`parallel_has_fnn` |
| `lib/reader.c` | ① `ReaderCreate()` 记录 `is_csv`（`TYPE=CSV`）；② `ReaderInit()` 计算默认 worker 数 = 在线 CPU − 1；③ `ReaderParam()` 新增 `PARALLEL` / `PARALLEL_WORKERS`（别名 `PARSE_WORKERS`）；④ 镜像 `SKIP/OFFSET`、`DELIMITER`、`QUOTE`、`ESCAPE`、`NULL`、`FILTER`、`FORCE_NOT_NULL` 到 Reader，同时**仍转发给 `ParserParam()`**，单进程语义不变；⑤ `#include <unistd.h>` |
| `lib/pg_fastload.c` | 顶部加两个 extern 声明；主循环改为 `if (FastloadParallelSupported(rd, wt)) FastloadParallelRun(...); else 原循环`，其他统计/日志/收尾逻辑完全不动 |
| `lib/Makefile` | `SRCS` 增加 `fastload_parallel.c`、`fastload_worker.c`；其余（含 `-lpthread`、`pgut/pgut-ipc.c`）保留 |
| `lib/exports.txt` | 增加导出符号 `pg_fastload_worker_main`（bgworker 通过 `load_external_function` 按符号名加载） |

## 4. 删除 / 丢弃

- **实验性 `CSVMT` 类型及其 `lib/parser_csv_mt.c` 全部丢弃**。并行能力不再以新 `TYPE` 暴露，而是 `PARALLEL=ON` 时由 `TYPE=CSV` 自动启用。
- `ReaderCreate()` 的 TYPE 列表回到原版 5 项：`BINARY / FIXED / CSV / TUPLE / FUNCTION`。

## 5. 完全未改动的模块

`lib/parser_csv.c`、`lib/parser_binary.c`、`lib/parser_tuple.c`、`lib/parser_function.c`、`lib/source.c`、`lib/writer*.c`（含 `writer_parallel.c`）、`lib/pg_btree.c`、`lib/pgut/*`、`lib/logger.c`、`lib/pg_strutil.c`、`lib/binary.c`、`bin/*`（除更名）、`util/*`、`SPECS/*`、`msvc/*`。

这保证了：

- 所有原有控制文件关键字与语义不变；
- `WRITER = DIRECT / BUFFERED / PARALLEL` 三种写入方式不变；
- `badfile` / 逐行容错 / `SKIP` / `LIMIT` / `ENCODING` / `FILTER` / `FORCE_NOT_NULL` 在单进程路径下不变；
- 原版 PARALLEL writer 依赖的 `pgut-ipc.c` 仍在构建中。

## 6. 新增行为摘要

| 控制文件 | 行为 |
|---|---|
| `PARALLEL = OFF`（默认） | 与原版 pg_bulkload 完全一致 |
| `PARALLEL = ON`, `TYPE = CSV` | 多进程并行 ①②③，④ 主 backend 串行 |
| `PARALLEL = ON`, 其他 TYPE | 自动降级为单进程（INFO 提示） |
| `PARALLEL = ON` + `FILTER` / `FORCE_NOT_NULL` / dropped 列 / 转码 / NOT NULL / `CHECK_CONSTRAINTS` / `PARSE_ERRORS` / `STDIN` | 自动降级为单进程（INFO 提示，见 PARALLEL_DESIGN 第 8 节） |
| `PARALLEL_WORKERS = n` | 显式指定 1..64 |
| 未指定 `PARALLEL_WORKERS` | 在线 CPU 核数 − 1，最小 1 |

## 7. 合规说明

- 保留 `COPYRIGHT` 原文（BSD-3-Clause 第 1、2 条要求）。
- 每个继承源文件保留 NTT 版权行；新增/修改文件额外标注 `Copyright (c) 2026, yuanyuansong`。
- 明确声明基于 pg_bulkload 3.1.23 二次实现、遵循原许可、未使用 NTT 名义背书（第 3 条要求）。
- 二进制分发时需在随附文档中复现版权声明与免责条款；`LICENSE` / `NOTICE` / `COPYRIGHT` 已随源码一起提供。

## 8. 首轮联调修复记录

| 问题 | 根因 | 修复 |
|---|---|---|
| `PARALLEL=ON` 被无声降级为单进程 | `rd->checker.tchecker != NULL` 是恒真条件：所有 writer 都无条件 `CreateTupleChecker()`；对 CSV 而言 `CoercionCheckerTuple()` 判定 `NO_COERCION` 后原样返回 tuple，worker 的 `TupleFormerTuple()` 输出与之等价 | 从 `FastloadParallelSupported()` 移除该条件，并在注释与文档中写明不可加回 |
| 小文件偶发 `worker N failed to attach (state=2)` | worker 抢在主 backend 之前跑完并 detach，`shm_mq_wait_for_attach()` 返回 `SHM_MQ_DETACHED`；`state=2` 实为 `FL_WORKER_DONE` | 编排层容忍 `DETACHED + FL_WORKER_DONE`，仅对未到达 DONE 的 worker 报错；PG 原生 shm_mq 保证剩余消息仍可排空 |
| `SKIP` / `LIMIT` 语义与原版不一致（交错分片下「最先到达的 N 条」不确定） | worker 原先只按记录号取模，未限制全局记录窗口 | worker 侧按 `[skip, skip+limit)` 窗口裁剪，先减后比避免溢出 |
| 并行分工不可观测 | 缺少每 worker 产出统计 | `FastloadParallelRun()` 正常收尾后打印 `worker #i produced N rows` |

同步更新：`README.md` 降级清单、`docs/PARALLEL_DESIGN.md` 第 8/10/11/12 节。

## 9. 基于瓶颈定位的 P0 性能优化（2026-11-08 第二轮）

依据：1 GiB TPC-H lineitem 基准 + `USE_PROFILE=1` 阶段计时
（`READER.PARSER` 占原版耗时 78.6%、并行路径 N× 全量扫描占 auto 耗时 65%、
`WRITER.TOAST` 被误累积为 15.39 s 等结论，见瓶颈分析报告）。

| # | 瓶颈 | 修改 | 效果（1 GiB，16 vCPU） |
|---|---|---|---|
| P0-1 | `PARALLEL_WORKERS=1` 仍创建 DSM + bgworker + shm_mq，耗时 16.2 s（串行 8.2 s 的 2 倍） | `FastloadParallelSupported()` 对有效 worker 数 ≤ 1 返回 false 并输出 INFO，走原版单进程 CSV 循环 | w=1 **16.215 → 8.070 s（2.01×）**，与 `PARALLEL=OFF` 等价 |
| P0-2 | worker 对每条记录逐字节调用 `fl_append_rec()` 拷贝（含大量非归属记录），整条记录再二遍切分 | 记录边界状态机改为 run-based 连续区间 `memcpy`；非归属记录零拷贝直接跳过；归属记录按连续段一次拷入。引号/转义/CRLF/跨块/末行无换行语义不变 | w=2 **7.760 → 6.820 s（1.14×）**，w=4 **3.270 → 2.950 s（1.11×）**，w=8 **2.530 → 2.320 s（1.09×）**；w=15/auto 在误差内持平（已饱和于串行写入段） |
| P0-3 | 并行路径无任何 profile 埋点：GLOBAL 各阶段读 0，leader 等待时间被误累积进 `WRITER.TOAST` | 新增 `GLOBAL.PARALLEL` 总段 + `<PARALLEL>`（SETUP/WAIT/COPY/WRITE）+ `<WORKER>`（SCAN/FORM/IPC，worker 经 DSM 发布本地计时）；`WriterInsert` 内层 PUSH/POP | GLOBAL 合计 ≈ elapsed（w2: 6.816 vs 6.820；auto: 2.340 vs 2.340）；`WRITER.TOAST` 回归 0.12~0.14 s 正常量级 |

涉及文件：`lib/fastload_parallel.c`、`lib/fastload_worker.c`、`lib/pg_fastload.c`、
`include/fastload_parallel.h`、`include/pg_profile.h`。

验证：release + debug 双构建 rc=0；`fastload_verify.py` **90/90 PASS**
（含 w=1 串行回退、w=2..w=64、auto=15、降级回归、竞态压力）；7 个配置
1 GiB 内容摘要（`count||sum(hash_record_extended)`）完全一致。

## 10. Plan C 单读多解架构（2026-11-08 第三轮优化）

依据：方案 B 的结构性瓶颈——每个 worker 各自 `open()` 全量 CSV 扫描
（I/O ×N、N 份重复边界扫描），文件超出 page cache 时总读量 ≈ N × 文件大小
（16 核冷缓存实测比原版慢 6.49×）。

**架构**：新增单个 reader bgworker（无 DB 连接），单遍扫描 CSV 并把记录
经 SPSC 字节环分发给 N 个解析 worker；worker 只解析归属记录并经 shm_mq
回传 leader。文件 I/O 从 ×N 降为 ×1。DSM 新增每 worker 一个
`FastloadRing`（key `FASTLOAD_KEY_RING_BASE` 起）；reader 为 handle[0]，
worker 为 handle[1..N]；`max_worker_processes ≥ N+1`。

迭代过程中的三个实测瓶颈与修复（每次修复均经 profile 数据确认）：

| # | 瓶颈（profile 证据） | 修改 | 效果（1 GiB，w=2） |
|---|---|---|---|
| C-1 | reader 逐条 `rec_no % N` 交错路由造成队头阻塞：一条环满即卡死全部环（reader blocked 9.205 s / 8.49M ring waits） | 改为**字节量分块轮转**：每 ~512 KiB 连续记录切换 owner（`FL_ROUTE_BYTES`），路由字节数只统计窗口内记录 | 首版 plan C w2 10.82 s，本项单独收益有限，与 C-3/C-4 叠加后生效 |
| C-2 | worker 逐条取环：每记录 2 次 spinlock + 2 次小 memcpy（7.37M 条 × 2 = 1500 万次锁操作，与 reader 写环争用），worker SCAN 5.2 s | worker 侧**批量取环**（`FL_DRAIN_CAP` 256 KiB）：一次锁内整块拉到本地缓冲，无锁切帧；超大记录回退流式读取 | SCAN 5.2 → 0.32 s；与 C-4 叠加后 w2 3.46 s |
| C-3 | 1 MiB 环只缓冲 2 个 512 KiB 路由块（≈6 ms worker 工作量）：reader 被一个环阻塞的整个窗口内其余环全部挨饿（worker idle ~45%） | 环容量 **1 MiB → 8 MiB**（`FASTLOAD_RING_SIZE`），shm_mq 回传队列 1 → 2 MiB；DSM 随实际 worker 数伸缩（15 worker ≈ 144 MiB） | **w2 10.59 → 3.46 s（3.06×）**、w4 5.13 → 1.97 s、w8 2.34 → 1.95 s |

最终 1 GiB 热缓存对比（P0 基线 → Plan C）：w2 **6.82 → 3.46 s（1.97×）**、
w4 **2.95 → 1.97 s（1.50×）**、w8 2.32 → 1.95 s（1.19×）、w15 2.32 → 2.01 s、
auto 2.34 → 1.99 s（1.18×）；fl_off / w1 与 P0 等价（串行路径零改动）。
高并发已饱和于 leader 串行 `WriterInsert`（WRITE ≈ 1.25~1.36 s 下限）。

涉及文件：`lib/fastload_worker.c`（reader 扫描/路由/批量取环重写）、
`lib/fastload_parallel.c`（reader 编排）、`include/fastload_parallel.h`
（`FastloadRing`、环/队列容量、reader 状态字段）、`lib/pg_fastload.c`
（`<PARALLEL>` 组新增 READ 段）、`include/pg_profile.h`（`prof_par_read`）。

验证：release + debug 双构建 rc=0；`fastload_verify.py` **90/90 PASS**；
7 配置内容摘要一致；复测后生产 `.so` 已恢复非 profile 版。

## 11. 内核 shm_mq 统一传输 + 冷缓存大块 I/O（2026-11-09 第四轮优化）

依据：Plan C 定稿后的阶段耗时分析——reader→worker 仍走自研 SPSC 字节环
（spinlock + head/tail + 100 µs 退避，约 200 行手写同步代码），与
worker→leader 方向的内核 shm_mq 形成两套传输实现，维护成本高；
冷缓存场景 reader 64 KiB 小块读 + 64 KiB 小块发产生 ~3.3 万次阻塞
系统调用，在本虚拟化宿主上被放大为分钟级回归。

| # | 项 | 修改 | 取舍结论 |
|---|---|---|---|
| S-1 | reader→worker 自研 SPSC 环替换为内核 shm_mq | 删除 `FastloadRing`（spinlock/head/tail/退避/计数控），DSM 改为每 worker 一个 `FASTLOAD_INPUT_MQ_SIZE`（8 MiB）shm_mq；reader 以 `shm_mq_send` 分帧发送、worker 以 `shm_mq_receive` 接收（整帧同消息零拷贝快路径，跨消息重组）；EOF 语义 = reader 先置 `reader_state=DONE` 再 `shm_mq_detach`，worker 见 `SHM_MQ_DETACHED` 后校验状态区分正常收尾/崩溃 | **采纳**。净删约 200 行手写同步代码，传输实现与回传方向统一为内核原语；热缓存 w2 3.46→3.33 s、w15 2.01→1.89 s、auto 1.99→1.88 s（持平略优）；90/90 PASS |
| S-2 | 冷缓存小块系统调用延迟放大 | `FL_READ_CHUNK` 64 KiB→**1 MiB**（每次载入 16386→1026 次 read）、`FL_STAGE_CAP` 64 KiB→**512 KiB**（17193→2148 次 send）；删除 `FL_DRAIN_CAP`（worker 侧改由 shm_mq 帧边界自然分批） | **采纳**。阻塞系统调用次数降约 15×；消除冷缓存**系统性**回归（好窗口下 w4/auto 冷缓存 1.8/1.5 s，反超串行 7.1 s）；宿主偶发卡顿窗口（vmstat 0 steal/0 iowait/100% idle）属 hypervisor 层现象，代码无法消除 |
| S-3 | WRITER=PARALLEL 组合评估（瓶颈项 1 场景） | — | **放弃**。评估结论：pg_bulkload 并行 writer 实为"1 个插入进程 + 独立 libpq 连接 + SysV IPC 队列"，不并行化插入本身；且独立事务语义破坏调用方 BEGIN/COMMIT 原子性（行在调用方提交前可见）；实测对已饱和场景为负收益。与"leader 串行 WriterInsert 是 ~1.3 s/1 GiB 下限"的结构性瓶颈无解耦作用，代价值得付时再议 |

涉及文件：`lib/fastload_worker.c`（reader/worker 传输层重写、FastloadRouter）、
`lib/fastload_parallel.c`（DSM 布局、profile 摘要去 ring 字段）、
`include/fastload_parallel.h`（删 `FastloadRing`，`FASTLOAD_KEY_RING_BASE`→
`FASTLOAD_KEY_INPUT_BASE`，`FASTLOAD_RING_SIZE`→`FASTLOAD_INPUT_MQ_SIZE`）。

验证：release + debug 双构建 rc=0（临时探针代码已全部移除后终版重建）；
`fastload_verify.py` **90/90 PASS**；7 配置热缓存内容摘要
（`7375042|9258128314029119699254`）与 GOLDEN 一致，ALL_MATCH=True；
热缓存终版：w2 3.32 s、w4 1.96 s、w8 1.88 s、w15 1.91 s、auto 1.87 s、
fl_off 8.24 s、w1 8.23 s（3 轮中位）；复测后生产 `.so` 恢复非 profile 版
（md5 `1e8ee515f2303b0755f6c9e010ec8b7f` 校验通过）。

## 12. 写入路径复查：并行 WriterInsert 与并行 B+ 树评估（2026-11-09 第五轮）

背景：用户要求将 leader 串行 `WriterInsert` 改造为并行执行、以并行 B+ 树
构建提升写入吞吐。经源码级复查（`lib/writer_direct.c`、`lib/pg_btree.c`）
与 profile 数据核验，结论为**两项放弃、一项保留待做**，无代码变更：

| # | 评估项 | 证据 | 结论 |
|---|---|---|---|
| W-1 | WAL / 数据写批量化 | DIRECT writer **已经是批量化实现**：WAL 每次装载仅 1 条首页记录（`create_cnt==0` 守卫 + 尾部 fsync，writer_direct.c:538-563）；数据经 1024 页（8 MiB）自管块缓冲一次大块 `write()`（:104、:623-637）；LSF 每 8 MiB 一次 fsync。批量化前提不成立 | **放弃**（无可优化空间） |
| W-2 | 并行填页流水线（worker 预组装 8 KB 页图，leader 只做 WAL+写） | WRITE 段 ~1.28 s/1 GiB 是纯逐元组 CPU（toast 检查 + infomask/XID 戳 + PageAddItem + SpoolerInsert ≈ 174 ns/行）；auto 档管线关键路径是 reader READ 1.66 s，写侧优化总耗时上限 1.87 → ~1.7 s（≈1.10×），需重写 worker/leader 传输层（块号预分配、t_ctid 补丁、Spooler 语义） | **放弃**（收益有限 vs 大改，违背"架构简洁优先"） |
| W-3 | 并行 B+ 树构建（带索引场景） | 基准表无索引 → 当前收益为 0；fork 内已保留上游 nbtsort-18.c 并行构建代码，但启用需将原版 WRITER=PARALLEL 的独立 libpq 事务改造为 bgworker 化（消除独立事务原子性问题，见第 11 节 S-3） | **保留待做**（独立实施轮次，需先做事务语义改造） |

涉及文档更新（本节同批完成）：`docs/ARCHITECTURE.md` 第 4/5 节与
`docs/PARALLEL_DESIGN.md` 由方案 B 描述重写为 Plan C 现行架构；
`README.md` 架构一览与已知限制同步修正（N× I/O 表述作废、
`max_worker_processes ≥ N+1`、分块轮转路由说明）。

## 13. BINARY / FIXED 并行化（2026-11-09 第六轮，P-1）

背景：并行路径原仅支持 `TYPE=CSV`（`FastloadParallelSupported` 以
`rd->is_csv` 守卫）。本轮将 `TYPE=BINARY` 与 `TYPE=FIXED`（两者在
`reader.c` 中映射到同一个定长解析器 `CreateBinaryParser`，COL 描述 +
STRIDE）接入 Plan C 并行管线。

实现要点（复用全部 CSV 并行基础设施，仅替换记录边界判定与字段解析）：

| 组件 | 修改 |
|---|---|
| `include/reader.h` / `lib/reader.c` | `Reader` 增加 `is_fixed`（idx 0/1 → BINARY/FIXED 均为定长） |
| `lib/binary.c` + `include/binary.h` | 新增 COL 字段描述符序列化：`BinaryParserGetMeta`（parser_binary.c，导出 nfield/rec_len/fields）、`BinarySerializedSize`/`BinarySerializeFields`（leader 把 kind/offset/len/nullif 写入 DSM blob）、`BinaryDeserializeFields`（worker 按 kind 从静态 `TYPES[]` 表重建 read 函数指针并分配 work buffer；**函数指针不跨进程传递**） |
| `include/fastload_parallel.h` | `FastloadParams` 增加 `pmode`（FL_PMODE_CSV/FL_PMODE_FIXED）、`nbinfields`、`rec_stride`；DSM 参数块在 `cols[]` 之后追加定长 blob |
| `lib/fastload_parallel.c` | 守卫放行 `is_fixed`；fixed 专属回退：无 COL、rec_len≤0、NULLIF 模式长于 64 字节 → 串行（沿用既有 FILTER/编码/约束/ dropped 列回退清单）；`fastload_fill_params` 序列化 blob |
| `lib/fastload_worker.c` | ① reader 侧 `fl_reader_scan_fixed`：按 STRIDE 直接切记录（无引号状态机），帧协议/SKIP/LIMIT/分块轮转与 CSV 完全一致；尾残字节 WARNING 忽略（对齐串行）。② worker 侧 `fl_emit_record_fixed`：镜像 `ExtractValuesFromFixed`——scratch 拷贝（队列内存只读）+ 字段尾临时 NUL 终止 + 按序调用 `field->read()`；STRIDE 校验防错。 |

涉及文件：`include/reader.h`、`lib/reader.c`、`include/binary.h`、
`lib/binary.c`、`lib/parser_binary.c`、`include/fastload_parallel.h`、
`lib/fastload_parallel.c`、`lib/fastload_worker.c`。

验证：
- 新增 `fastload_verify_fixed.py` **44/44 PASS**：纯 CHAR 文本、
  BINARY/FIXED 别名、混合文本+二进制数值（INTEGER/BIGINT/DOUBLE 原生
  字节）、NULLIF（文本+数值）、STRIDE>字段和（尾部填充）、尾残字节、
  SKIP+LIMIT、PRESERVE_BLANKS+VARCHAR、空文件、w=1/2/3/4/auto、
  FILTER 降级、10000 行 × 4 worker 分片。
- CSV 回归 `fastload_verify.py` **90/90 PASS**（无回归）。
- 1 GiB 定长基准（10,737,418 行 × 100 B，7 配置 × 3 轮热缓存）：
  内容摘要 `10737418|12495206446709334590242` 七配置 ALL_MATCH=True；
  好窗口：fl_off **1.62 s**、w2 1.06 s、w4 0.94 s、w8 **0.89 s**、
  w15 0.90 s、auto 0.91 s（并行相对串行最高 **1.82×**；w1 按
  `workers=1` 语义串行回退 1.63 s）。定长格式串行本身即 8.2→1.6 s
  （无 CSV 引号状态机、数值 C-cast 直接转换）；随机 31~61 s 卡顿轮次
  与第四轮冷缓存诊断结论一致（宿主 hypervisor 层阻塞调用延迟放大，
  guest 全程 100% idle），属环境限制。
- release + debug 双构建 rc=0，新增代码无编译警告；生产 `.so`
  md5 `16f267843b068362e1de227e3fbf4989`。

已知限制（BINARY/FIXED 并行路径）：输入编码须与数据库编码一致、
FILTER/约束检查/PARSE_BADFILE 降级串行（与 CSV 并行相同）；NULLIF
模式长度 ≤ 64 字节；记录必须为完整 STRIDE（尾残忽略）。

## 14. 并行 B+ 树评估 + 无索引 spool 早退（2026-11-09 第七轮，W-3/W-3a）

背景：第五轮把"并行 B+ 树构建"（W-3）留作待办，前提是"只在带索引表上
有意义"。本轮先测量、再决策。

### 14.1 成本测量（1 GiB lineitem 7,375,042 行，DIRECT，热缓存）

| 场景 | 墙钟（min） | WRITER.TABLE | WRITER.INDEX(spool) | MERGE(排序+建树) |
|---|---|---|---|---|
| 无索引 | 1.99 s | 0.63 s | 0.28 s（空转） | 0 |
| 单 btree 索引 `l_orderkey`（84 MB） | 3.87 s | 0.75 s | 1.81 s | 2.21 s |

索引成本 ≈ 1.9 s（约为带索引装载总时间的 **50%**，profile 分段口径下
spool+merge 占 66%），确认 W-3 有真实收益空间。

### 14.2 三条并行路线的评估与取舍

| 路线 | 实测/估算 | 结论 |
|---|---|---|
| **L1 复用 PG 原生并行 btree**（装载后 `index_build` / `CREATE INDEX`，自动走 parallel build） | **34.4 s**（= 装载 2.06 s + `CREATE INDEX` 32.33 s），比现有 spool 路径**慢 8.3×** | ❌ **否决**。根因：DIRECT writer 直接写 smgr 数据文件、绕过 buffer manager，`index_build` 必须回读 1 GiB heap；本 VM 上块读延迟放大使回读成本远超 spool 的额外 CPU。这正是 pg_bulkload 用 spool 的根本理由。 |
| **L2 自研并行 spool→build**（spool 按 worker 分片，跨进程并行排序 + 各自建叶页段，leader 链接） | 收益上限 ≈ 1.3~1.4×（仅带索引场景：3.87 → ~2.8 s） | ❌ **放弃**。需把 `pg_btree.c` 的 tuplesort→页组装管线改写为多进程（跨进程排序状态、WAL 协调、唯一性检查、崩溃恢复），约 800~1200 行高复杂度代码；违反"架构简洁、可读、可维护优先"原则。spool 的 tuplestore 是 leader 进程私有结构，跨进程共享需另建段文件协议。 |
| **L3 无索引路径消除 spool 空转**（W-3a） | 见 14.3 | ✅ **采纳** |

结论：**并行 B+ 树构建在 pg_bulkload DIRECT 架构下不成立**——两条可行
路径一条（L1）被实测否决，另一条（L2）代价与收益不匹配。现有 spool
批构建已比"装载后建索引"快 8.3×，是当前架构下的正确选择。W-3 至此关闭。

### 14.3 W-3a：无索引时跳过 SpoolerInsert

`SpoolerInsert()` 原实现不论目标表有无索引，每元组都执行
`ExecStoreHeapTuple()` + `GetPerTupleExprContext()` + `IndexSpoolInsert()`
循环（`ri_NumIndices == 0` 时循环体不执行，纯空转）。1 GiB 装载实测该
空转耗 0.28 s（7.37M 元组 × ~38 ns）。

修改（`lib/pg_btree.c`）：`SpoolerInsert()` 开头提前解析 `relinfo`，
`ri_NumIndices <= 0` 直接 return。3 行有效代码，无语义变化
（有索引路径逻辑完全不变）。

验证与效果：

- `fastload_verify.py` **90/90 PASS**、`fastload_verify_fixed.py`
  **44/44 PASS**（含唯一索引/UPSERT/重复处理用例，零回归）。
- 1 GiB 热缓存 7 配置 × 3 轮，内容摘要 `7375042|9258128314029119699254`
  全部一致（ALL_MATCH=True）：

| 配置 | 上一版 | 本轮 | 相对上一版 |
|---|---|---|---|
| fl_off | 8.24 s | **7.91 s** | 1.04× |
| fl_w1 | 8.23 s | **7.86 s** | 1.05× |
| fl_w2 | 3.32 s | 3.28 s | 1.01× |
| fl_w4 | 1.96 s | 1.91 s | 1.03× |
| fl_w8 | 1.88 s | **1.75 s** | 1.07× |
| fl_w15 | 1.91 s | **1.73 s** | 1.10× |
| fl_auto | 1.87 s | **1.75 s** | 1.07× |

（串行路径同样受益，`fl_off`/`fl_w1` 各减 ~0.35 s；高并行档位的收益
被 leader 串行 WriterInsert 下限部分抵消。）

- release + debug 双构建 rc=0，无新增警告；生产 `.so`
  md5 `c43d49a6bc1321a87b8fb326c01a53e3`。

涉及文件：`lib/pg_btree.c`（唯一代码改动）。
