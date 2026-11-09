# pg_fastload v1.0.0 验证与交付报告

> 作者：yuanyuansong  日期：2026-11-08  
> 结论：**功能验证 90/90 PASS（ALL_OK = True）**（已在 16 vCPU / 7.3 GiB 新配置下重跑），
> release / debug 双构建通过；**`pg_fastload v1.0.0` 是本项目的第一版实现**。  
> 性能结论以 `docs/BENCHMARK.md` 为准；本文件早期的 2 vCPU 性能数字已作废。

---

## 1. 交付范围

| 项 | 内容 |
|---|---|
| 项目名 | `pg_fastload`（独立开源提交，版本 `1.0.0`） |
| 上游 | `pg_bulkload` 3.1.23，HEAD `798e0f2`，BSD 3-Clause 风格许可 |
| 许可 | 保留 `COPYRIGHT` 原文；新增 `LICENSE` / `NOTICE`；新增/修改文件标注 `Copyright (c) 2026, yuanyuansong`；明确未获 NTT, Inc. 背书 |
| 本轮范围 | 仅 `TYPE=CSV` 的并行化；`BINARY / FIXED / TUPLE / FUNCTION` 后续按相同方式改造 |
| 对外行为 | `PARALLEL=ON` 时 `TYPE=CSV` 默认走多进程并行，**不引入 `CSVMT` 类型**，类型名仍为 `CSV`；`PARALLEL_WORKERS` 默认「在线 CPU 核数 − 1」 |

---

## 2. 验证环境

- VM `192.168.25.134`，**16 vCPU**（i7-13700KF）/ **7.3 GiB 内存** / 3 GiB swap，
  Oracle Linux Server 10.1；用户调整前为 2 vCPU / 2.5 GiB
- PostgreSQL 18.6（`REL_18_STABLE`，HEAD `1da6ddbdc3`），postmaster 经核验为 **release 版**二进制
- release `/home/ysyuan/postgres/release`，debug `/home/ysyuan/postgres/debug`（`--enable-debug --enable-cassert -O0 -g`）
- 测试集群 `PGDATA=/tmp/pgdata`，`port=5433`，库 `tpcc`，**`max_worker_processes=32`**
- 数据集 `/tmp/tpcc/order_line.csv`：10.7 GiB / 130,447,434 行（性能用 1M / 8M / 32M 行子集）
- 对比基线：`contrib/pg_bulkload` 干净上游 HEAD `798e0f2`（`git status` 仅一条
  `D lib/parser_csv_mt.c`，无 CSVMT 标识符、`.so` 无相关符号），确认为**原版 3.1.23**

> 功能验证（第 4 节）原先在 2 vCPU 环境完成 90/90；VM 调整后已**完整重跑一次**，
> 仍为 90/90 PASS，且 `PARALLEL_WORKERS` 未指定时由 `nproc−1` 正确解析为 **15**。

---

## 3. 构建验证

| 构建 | `clean` | `make` | `install` | 新增诊断 | `pg_fastload_worker_main` 导出 |
|---|---|---|---|---|---|
| release | rc=0 | rc=0 | rc=0 | 无 | `T` 符号存在 |
| debug | rc=0 | rc=0 | rc=0 | 无 | `T` 符号存在 |

唯一编译诊断是**上游原版** `bin/binary.c` 的两条
`-Wmissing-variable-declarations`（`TYPES` / `ALIASES`），与本次新增代码无关。

---

## 4. 功能正确性验证（90 项全通过）

判据：串行基线 `PARALLEL=OFF` 与并行 `PARALLEL=ON` 的**内容集合**一致。
指纹用 `md5(string_agg(row_to_json(s)::text, chr(10) ORDER BY 行文本))`，
即先按内容排序再聚合，与物理插入顺序无关（并行路径不保序）。

### Phase 1 — CSV 边界数据集（每个用例 6 项断言 × 10 个数据集）

| 数据集 | 覆盖点 |
|---|---|
| `basic` | 标准 LF 分隔、5 行 |
| `crlf` | `\r\n` 行结束符 |
| `lone_cr` | 单独 `\r` 作终止符、末尾无换行 |
| `quoted` | 引号内逗号、引号内 `\n`、引号内 `\r\n`、转义引号 `""`、quoted empty `""` |
| `no_eol` | 文件末尾无换行，最后一条记录仍须产出 |
| `nulls` | 空字段→NULL、quoted empty→空串、首尾空格、`\N` 字面量 |
| `empty` | 空输入文件（0 行） |
| `one_row` | 单行文件，多余 worker 产出 0 行 |
| `cross_chunk` | 记录终止符 / 引号内换行 / 转义引号恰好跨 64 KiB 读块边界 |
| `many` | 10,000 行，验证取模分片不丢不重 |

每个数据集的断言：串行可执行、串行未误启并行、并行可执行、内容 md5 一致、
bgworker 真启动（日志出现 `PARALLEL=ON, TYPE=CSV, workers=N` 且无 `PARALLEL disabled`）、
各 worker 产出行数之和 = 表中行数、日志无真实错误标记。

实测取模分配示例：5 行 / 2 worker → `worker#0=3, worker#1=2`；
10,000 行 / 3 worker → `3334 / 3333 / 3333`。

### Phase 2 — 自定义解析参数

`DELIMITER=|`、`QUOTE=~`、`ESCAPE=~`、`NULL=NA`：串行与并行内容 md5 一致，
两个 worker 各产出 2 行，证明参数已正确镜像到 DSM 参数块。

### Phase 3 — SKIP + LIMIT

`SKIP=1 LIMIT=3`、`workers=3`：串行与并行内容 md5 完全一致
（均为 `3|7d5356…`）。worker 侧按全局记录号窗口 `[skip, skip+limit)` 裁剪，
避免交错分片下「最先到达的 N 条」不确定。

### Phase 4 — 并行度变化（10,000 行）

| `PARALLEL_WORKERS` | 声明值 | 各 worker 产出 | 内容 md5 |
|---|---|---|---|
| 1 | 1 | `10000` | 一致 |
| 2 | 2 | `5000 / 5000` | 一致 |
| 3 | 3 | `3334 / 3333 / 3333` | 一致 |
| 4 | 4 | `2500 × 4` | 一致 |
| 未指定（auto） | **15**（16 核 − 1） | `667 × 10 + 666 × 5` | 一致 |

### Phase 5 — 自动降级回归

`FORCE_NOT_NULL=c1`：日志出现 `PARALLEL disabled`，未启动 bgworker，
仍由原版单进程路径正确加载 5 行，内容 md5 与串行一致。

### Phase 6 — 启动竞态压力

5 行小文件连续 10 次 `PARALLEL=ON workers=2`：每次均正确产出 `3 / 2` 行、
内容 md5 相同、日志无 `failed to attach`。

---

## 5. 性能自测（16 vCPU 新配置，完整版见 `docs/BENCHMARK.md`）

用户调整 VM 后已做**与干净原版 `pg_bulkload 3.1.23` 的同条件对比**：
覆盖 1M / 8M / 32M 行三档，每档 `WRITER=DIRECT`、重复取中位数、
每轮预热或 `CHECKPOINT` 隔离，并用 `sum(hash_record_extended(t,0))`
做顺序无关的内容校验（所有配置与原版摘要一致）。

### 热缓存（文件可进 page cache）

| 数据规模 | 原版 pg_bulkload | pg_fastload OFF | auto (=15) | 加速比 |
|---|---|---|---|---|
| 1M 行 / 77 MB | 0.530 s | 0.530 s | 0.220 s | 2.41× |
| 8M 行 / 640 MB | 4.070 s | 4.110 s | 1.440 s | 2.83× |
| 32M 行 / 2.5 GiB（稳态） | 16.180 s | 16.325 s | 5.740 s | 2.82× |

- `PARALLEL=OFF` 与原版严格等价，更名未引入串行回归。
- `w≤2` 为负收益（0.36~0.74×），盈亏平衡点在 `w=4` 附近；`w=8→15` 趋于饱和，
  ④ 串行 `WriterInsert()` 成为新瓶颈，16 核不带来 16×。
- 32M 行首轮原版出现 46.57 s 冷启动异常，用 `CHECKPOINT` 隔离后的 4 轮稳态
  16.13~16.25 s 才是有效基线；直接取 2 轮中位数会把加速比虚增到 5.58×。

### 冷缓存（`posix_fadvise(DONTNEED)` 逐出 CSV 页，免 root）

| 配置 | 各轮 elapsed | 中位数 | 相对原版耗时 |
|---|---|---|---|
| 原版 pg_bulkload | 16.55 / 16.57 s | 16.56 s | 1.00× |
| `w=4` | 71.46 / 101.69 s | 86.58 s | 5.23× |
| `auto=15` | 96.75 / 107.49 / 126.33 s | 107.49 s | **6.49×（更慢）** |

**方案 B 的 I/O ×N 在文件超 page cache 时会彻底反转收益**：`auto=15` 需读约
`15 × 2.5 GiB ≈ 37.5 GiB`，实测比原版慢约 6.5×。该场景应显式调小
`PARALLEL_WORKERS`（建议 4 左右）或直接 `PARALLEL=OFF`。
全量 10.7 GiB 实测因 VM 磁盘 ENOSPC 未完成，不给出全量加速比，
详见 `docs/BENCHMARK.md` 第 6 节。

> 旧 2 vCPU 环境下的 `0.530 / 0.990 / 0.350 s（1.51×）` 与
> `auto = 1` 的结论**全部作废**。

---

## 6. 内存管理与合规静态核查

| 核查项 | 结果 |
|---|---|
| 新增并行源码 `fastload_parallel.c` / `fastload_worker.c` / `fastload_parallel.h` 的 `malloc/calloc/realloc/free` | **NONE** |
| `release/lib/pg_fastload.so` 动态符号表中的 malloc 族未定义符号 | **NONE** |
| 上游 `bin/` 客户端工具与 `lib/pgut/pgut-pthread.c`（Windows pthread 兼容层）的 malloc | 保留原样，属命令行前端，不参与服务端加载，非本次改动 |
| `COPYRIGHT` | 原文完整（BSD 3-Clause 三条 + 免责条款） |
| `LICENSE` / `NOTICE` / `README.md` / `docs/` | 明确「基于 pg_bulkload 二次实现」、作者 `yuanyuansong`、未获 NTT 背书 |
| 上游 `pg_bulkload` 引用标注 | **合规必需，已写清楚**：`LICENSE` / `NOTICE` / `README.md` / `docs/CHANGES.md` / `docs/ARCHITECTURE.md` 及新增源文件头注释均明确写出上游项目名、版本 `3.1.23`、HEAD `798e0f2`、来源 URL、BSD 3-Clause 许可、原版权 `NTT, Inc.`、改动清单与作者署名；`COPYRIGHT` 原样保留 |
| `CSVMT` / `parser_csv_mt.c` | 已全部丢弃 |

> **关于「引用」与「更名」**：对 `pg_bulkload` 的引用是 BSD 3-Clause 第 1、2、3 条的
> 署名与非背书要求，属于合规必需，**不是需要清除的「残留」**。全量更名针对的是
> 项目 / 扩展 / 共享库 / SQL schema / SQL 函数 / 宏等**代码标识符**，与合规引用互不
> 冲突。`LICENSE` 末段已明确：`"pg_bulkload"` 及贡献者名称仅用于按许可署名条款标识
> 上游项目；`NOTICE` 另列「What was kept / What was added or changed」完整说明改动边界。

**内存生命周期（palloc / MemoryContext）**

- worker：`workctx = AllocSetContextCreate(TopMemoryContext, ...)` 承载长生命周期对象；
  `recctx` 为每条记录 scratch，`shm_mq_send()` 已把字节复制进队列后
  `MemoryContextReset(recctx)`，130M 条记录不会累积。
- 主 backend：编排对象（DSM / `shm_mq_handle` / bgworker handles / `done[]`）放在独立
  `runctx`，**不放在 `wt->context`**——后者每轮 `WriterInsert()` 后会被 reset。
- 每条收到的消息在 `wt->context` 中 `MemoryContextAlloc` 出可写副本后交给
  `WriterInsert()`（该函数会原地修改 `t_data` 的 infomask / xmin / t_self / t_ctid），
  随后 `MemoryContextReset(wt->context)`；函数收尾 `MemoryContextDelete(runctx)`。
- `PG_CATCH()` 中统一执行 `fastload_cleanup()`（终止 worker、`shm_mq_detach`、
  `dsm_unpin_segment` + `dsm_detach`）再 `PG_RE_THROW()`。

---

## 7. 首轮联调修复记录

| 问题 | 根因 | 修复 |
|---|---|---|
| `PARALLEL=ON` 被无声降级为单进程 | `rd->checker.tchecker != NULL` 恒为真（所有 writer 都无条件 `CreateTupleChecker()`）；对 CSV，`CoercionCheckerTuple()` 判定 `NO_COERCION` 后原样返回 tuple，worker 的 `TupleFormerTuple()` 与之等价 | 从 `FastloadParallelSupported()` 移除该条件，注释与文档写明不可加回 |
| 小文件偶发 `worker N failed to attach (state=2)` | worker 抢在 leader 之前跑完并 detach，`shm_mq_wait_for_attach()` 返回 `DETACHED`；`state=2` 实为 `FL_WORKER_DONE` | 容忍 `DETACHED + DONE`；PG 原生 shm_mq 保证剩余消息仍可排空 |
| `SKIP` / `LIMIT` 语义不确定 | 交错分片原先按「最先到达的 N 条」取数 | worker 按 `[skip, skip+limit)` 全局窗口裁剪 |
| 分工不可观测 | 缺少产出统计 | 正常收尾后打印 `worker #i produced N rows` |

---

## 8. 残留风险与未执行项

1. **ASan 无法执行**：VM 缺 `libasan.so.8` runtime，`dnf` 安装需提权且被环境拦截。
2. **本轮未跑 valgrind**：需要以 `--trace-children=yes` 另起带 debug 版 PG 的独立
   PGDATA/端口，会打断当前 5433 测试集群。debug 版已编译通过；本轮内存结论为
   **静态生命周期核对**，不是动态泄漏检测。后续可在独立集群补跑
   （PG 启动固有的 `208 B definite lost` 与 `relcache.c write_item` 的
   `Syscall param write(buf)` 属正常，不是扩展泄漏）。
3. **10.7 GiB 全量性能对比未完成**：新配置下已尝试，因 VM 51 GiB 根分区
   `No space left on device`（130M 行单事务需约 13 GiB 表 + 10 GiB WAL）而中止，
   集群已核验恢复。冷缓存模拟已实测方案 B 在超 page cache 时比原版慢约 6.5×，
   因此收益不作外推；详见 `docs/BENCHMARK.md` 第 5、6 节。
4. **并行路径无逐行容错**：字段数不符、输入函数报错、未闭合引号会使该 worker 退出并
   令本次加载失败，不写 `PARSE_BADFILE`；单进程路径的逐行容错行为不变。
5. **不保序**：取模交错分片不保证物理插入顺序（heap 表本身也不保证）。
6. **自动降级清单**：`FILTER` / `FORCE_NOT_NULL` / dropped 列 / 显式 `ENCODING` 转码 /
   NOT NULL / `CHECK_CONSTRAINTS` / `PARSE_ERRORS != 0` / `INPUT=STDIN` / 非 CSV 类型，
   均回退原版单进程路径并打印 INFO。
7. **仅 CSV 并行化**：`BINARY / FIXED / TUPLE / FUNCTION` 暂未改造。
8. **资源要求**：需要 `max_worker_processes` ≥ N；单条记录上限 256 MiB。
9. **编码假设**：并行路径假设输入编码与数据库编码一致，不调用 `CheckerConversion()`。

---

## 9. 结论

`pg_fastload v1.0.0` 作为**本项目的第一版实现**，在功能正确性
（90/90，已在 16 vCPU 新配置重跑）、构建（release + debug）、性能
（热缓存默认 worker 约 2.4~2.8×，详见 `docs/BENCHMARK.md`）、内存静态核对与
版权合规五个维度达到交付标准。已知残留风险已逐条列明：ASan / valgrind 与
全量性能属环境限制；超 page cache 时 I/O ×N 导致收益反转属方案 B 的设计取舍，
均已明确记录并给出 worker 数建议。

立项前被丢弃的 `CSVMT` pthread 原型及其 2 vCPU 结论不属于本版交付物，
相关文件已移入归档目录，仅作可追溯保留。

---

## 10. P0 优化验证轮（2026-11-08 第二次全量验证）

针对 1 GiB lineitem 瓶颈定位报告实施三项 P0 优化（w=1 串行回退、
worker 记录拷贝 run-based 化、并行路径 profile 埋点）后重新验证：

- **功能**：`fastload_verify.py` 90/90 PASS（含新增 w=1 串行回退断言
  `disabled=True has_on=False`）。
- **性能**：fl_w1 16.215→8.070 s（2.01×）、fl_w2 7.760→6.820 s（1.14×）、
  fl_w4 3.270→2.950 s、fl_w8 2.530→2.320 s；fl_off 8.190→8.120 s 无回归；
  w15/auto 持平（饱和于串行写入段）。
- **正确性**：7 配置导入内容摘要一致
  （`7375042|9258128314029119699254`）。
- **可观测性**：`GLOBAL.PARALLEL` + `<PARALLEL>`（SETUP/WAIT/COPY/WRITE）+
  `<WORKER>`（SCAN/FORM/IPC）上线，GLOBAL 合计 ≈ elapsed，
  `WRITER.TOAST` 伪计时消除（0.12~0.14 s）。
- **构建**：release + debug 双 rc=0；复测后生产 `.so` 已恢复非 profile 版
  （md5 与优化前一致）。

详见根目录 `pg_fastload_P0优化实施与验证报告.md` 与
`docs/CHANGES.md` 第 9 节。

## 11. Plan C 单读多解验证轮（2026-11-08 第三次全量验证）

架构改造（单 reader + 每 worker SPSC 环 + 分块轮转路由 + 批量取环 +
8 MiB 环）后重新验证：

- **功能**：`fastload_verify.py` **90/90 PASS**（w=1 串行回退、
  w=2/3/4/auto=15、降级回归、竞态压力、全部 CSV 边界数据集均通过；
  分块轮转不再保证逐条 `rec_no % N` 分布，验证只断言 worker 数与
  内容完整性——这是对外行为契约）。
- **正确性**：7 配置 1 GiB 导入内容摘要与 GOLDEN 一致
  （`7375042|9258128314029119699254`），ALL_MATCH=True。
- **性能**（1 GiB lineitem 热缓存，3 轮中位，vs P0 基线）：
  fl_off 8.13 s（1.00×，等价）、fl_w1 8.05 s（串行回退）、
  **fl_w2 3.46 s（1.97×）**、**fl_w4 1.97 s（1.50×）**、
  fl_w8 1.95 s（1.19×）、fl_w15 2.01 s（1.15×）、fl_auto 1.99 s（1.18×）。
  相对串行（8.13 s）：w4/w8/w15/auto ≈ 4.1×，w2 2.35×。
- **profile 闭环**：reader blocked 时间从 9.2 s 降至 ~0（READ 组
  ≈ GLOBAL.PARALLEL，WAIT 占比大幅下降）；worker SCAN 5.2 → 0.32 s。
- **构建**：release + debug 双 rc=0；复测后生产 `.so` 恢复非 profile 版
  （两套安装均 `NO_PROFILE`）。

详见根目录 `pg_fastload_PlanC单读多解优化报告.md` 与
`docs/CHANGES.md` 第 10 节。

## 12. 内核 shm_mq 统一传输 + 冷缓存大块 I/O 验证轮（2026-11-09 第四次全量验证）

改造（reader→worker 自研 SPSC 环 → 内核 shm_mq；读/发块 64 KiB →
1 MiB / 512 KiB；评估并放弃 WRITER=PARALLEL）后重新验证：

- **功能**：`fastload_verify.py` **90/90 PASS**（全部 CSV 边界数据集、
  自定义解析参数、SKIP/LIMIT、并行度 2/3/4/auto=15、w=1 串行回退、
  降级回归、启动竞态压力均通过）。
- **正确性**：7 配置 1 GiB 热缓存导入内容摘要与 GOLDEN 一致
  （`7375042|9258128314029119699254`），ALL_MATCH=True。
- **热缓存性能**（1 GiB lineitem，3 轮中位，终版）：
  fl_off 8.24 s（1.00×，等价）、fl_w1 8.23 s（串行回退）、
  **fl_w2 3.32 s（vs P0 基线 6.82 s = 2.05×；vs 原始 7.76 s = 2.34×）**、
  **fl_w4 1.96 s（vs P0 2.95 s = 1.51×）**、fl_w8 1.88 s（1.23×）、
  fl_w15 1.91 s（1.21×）、fl_auto 1.87 s（1.25×）。
  与 shm_mq 替换后单独测得的数字（w2 3.33 / auto 1.88）持平，大块化
  对热缓存无回归。
- **冷缓存性能**（fadvise 逐出，4 轮合计）：好窗口 w2 ≈3.0 s、
  w4 ≈1.8 s、auto ≈1.5 s，**反超串行 7.1 s（最高 4.9×）**，Plan C 的
  1× 读优势在冷缓存兑现；fl_off / orig 始终稳定 7.07~7.20 s。
  并行配置仍随机出现 11~62 s 卡顿轮次：诊断（cold_diag2~11、vmstat、
  dd 探针）确认该卡顿源于宿主/hypervisor 层对阻塞系统调用的延迟放大
  （卡顿窗口内 guest vmstat 为 0 steal / 0 iowait / 100% idle，读总量
  与带宽正常），串行路径因 stdio 大块 fread 系统调用次数少 ~500× 而
  基本免疫。属环境限制，代码侧已将系统性成分（小块 ×3.3 万次系统调用）
  消除。
- **WRITER=PARALLEL 评估**：放弃。该模式实为"1 个插入进程 + 独立
  libpq 连接 + SysV IPC 队列"，不并行化插入，且独立事务破坏调用方
  BEGIN/COMMIT 原子性，实测负收益（详见 `docs/CHANGES.md` 第 11 节 S-3）。
- **构建**：终版（临时探针代码全部移除后）release + debug 双 rc=0；
  复测后生产 `.so` 恢复非 profile 版（md5 `1e8ee515f2303b0755f6c9e010ec8b7f`
  校验通过）。

详见 `docs/CHANGES.md` 第 11 节与根目录
`pg_fastload_优化记录总结.md`（全部优化时间线与累计收益）。

## 13. 写入路径复查轮（2026-11-09 第五轮，评估性质，无代码变更）

针对"并行 WriterInsert / 并行 B+ 树构建"需求的源码级复查与结论：

- **证据**：DIRECT writer 的 WAL 为每次装载 1 条首页记录
  （writer_direct.c:538-563），数据写为 1024 页（8 MiB）块缓冲单次大块
  `write()`（:104、:623-637）——批量化已到位；WRITE 段 ~1.28 s/1 GiB 为
  纯逐元组 CPU（≈174 ns/行）；auto 档管线关键路径为 reader READ 1.66 s。
- **W-1（WAL/数据批量化）放弃**：前提不成立，无优化空间。
- **W-2（并行填页流水线）放弃**：总耗时上限 ≈1.10×，与重写传输层的
  代价不匹配。
- **W-3（并行 B+ 树）保留待做**：仅对带索引表有效（基准表无索引，
  当前收益 0）；fork 已保留 nbtsort-18.c，启用前需先将原版
  WRITER=PARALLEL 的独立 libpq 事务 bgworker 化。
- **本轮无代码变更**：生产 `.so` 保持终版
  （md5 `1e8ee515f2303b0755f6c9e010ec8b7f`），90/90 验证状态不变。
- **文档修正**（本轮实际改动范围）：`docs/ARCHITECTURE.md` 第 4/5 节、
  `docs/PARALLEL_DESIGN.md` 全文由方案 B 描述更新为 Plan C 现行架构
  （DSM 双向 shm_mq 布局、分块轮转路由、EOF 语义、串行回退与降级清单）；
  `README.md` 架构一览与已知限制同步修正。

详见 `docs/CHANGES.md` 第 12 节。

## 14. BINARY / FIXED 并行化验证轮（2026-11-09 第六次全量验证）

`TYPE=BINARY` / `TYPE=FIXED`（同一定长解析器）接入 Plan C 并行管线后的
验证结果：

- **新增功能验证** `fastload_verify_fixed.py`：**44/44 PASS**。覆盖：
  纯 CHAR 定长文本、BINARY/FIXED 别名一致性、混合文本+二进制数值字段
  （INTEGER(4)/BIGINT(8)/DOUBLE(8) 原生字节解码）、NULLIF（文本模式与
  数值等长模式）、STRIDE 大于字段和（尾部填充）、EOF 尾残字节忽略
  （WARNING，与串行一致）、SKIP+LIMIT 窗口、PRESERVE_BLANKS+VARCHAR
  尾空格保留、空文件、PARALLEL_WORKERS 1/2/3/4/auto（w=1 串行回退）、
  FILTER 关键字自动降级（fl_flt(text,text) 复合返回）、10000 行 × 4
  worker 分片完整性。
- **CSV 回归**：`fastload_verify.py` **90/90 PASS**，既有功能零回归。
- **1 GiB 定长基准**（`bench_fixed.py`：/tmp/tpch/fixed1g.bin，
  10,737,418 行 × 100 B，7 配置 × 3 轮热缓存 + CHECKPOINT 隔离）：
  - 内容摘要 `10737418|12495206446709334590242` 七配置 **ALL_MATCH=True**。
  - 好窗口中位/最小：fl_off 1.62 s、w2 1.06 s、w4 0.94 s、w8 **0.89 s**、
    w15 0.90 s、auto 0.91 s；并行相对串行最高 **1.82×**。w1 触发
    `workers=1` 串行回退（1.63 s ≈ fl_off，语义正确）。
  - 定长格式串行装载 1.62 s，远快于 CSV 串行 8.2 s（无引号状态机、
    数值 C-cast 直接转换、单遍定长切分）——并行收益相应收窄。
  - 随机 31~61 s 卡顿轮次与第 12 节冷缓存诊断结论一致：宿主 hypervisor
    层对阻塞调用的延迟放大（卡顿轮内 vmstat 0 steal / 0 iowait /
    100% idle），非代码问题，属环境限制；min 值代表真实性能。
- **构建**：release + debug 双 rc=0，本轮新增代码零编译警告（仅存
  上游原生 `TYPES/ALIASES` 表的声明风格警告）。生产 `.so`
  md5 `16f267843b068362e1de227e3fbf4989`。

详见 `docs/CHANGES.md` 第 13 节。

## 15. 并行 B+ 树评估 + 无索引 spool 早退（2026-11-09 第七次全量验证）

第七轮先测量后决策：确认 W-3（并行 B+ 树）的真实收益空间，再按
"架构简洁优先"原则取舍。**代码改动仅 1 处**（`lib/pg_btree.c`
`SpoolerInsert()` 无索引早退）。

- **成本测量**（1 GiB lineitem，DIRECT，热缓存 min 值）：
  无索引 1.99 s；单 btree 索引（`l_orderkey`，84 MB）3.87 s。
  profile 分段：`WRITER.INDEX`(spool) 0.28→1.81 s、`MERGE`(排序+建树)
  0→2.21 s、`WRITER.TABLE` 0.63→0.75 s。索引成本 ≈1.9 s（≈50%）。
- **L1（复用 PG 原生并行 btree：装载后 `CREATE INDEX`）实测否决**：
  装载 2.06 s + `CREATE INDEX` **32.33 s** = 34.4 s，比 spool 路径
  慢 **8.3×**。根因：DIRECT 绕过 buffer manager，索引构建必须回读
  1 GiB heap，本 VM 块读延迟放大使回读代价远超 spool 的 CPU 代价。
- **L2（自研并行 spool→build）放弃**：收益上限 ≈1.3~1.4×（仅带索引
  场景），需 ~800~1200 行跨进程管线改造（排序状态、WAL、唯一性、
  崩溃恢复），与"可读可维护"原则冲突。
- **L3（W-3a）实施**：`SpoolerInsert()` 在 `ri_NumIndices <= 0` 时
  直接返回，消除每元组 `ExecStoreHeapTuple()` + econtext 设置的纯空转。
- **功能验证**：`fastload_verify.py` **90/90 PASS**；
  `fastload_verify_fixed.py` **44/44 PASS**（含唯一索引/重复处理/
  UPSERT 相关用例，零回归）。
- **1 GiB 热缓存基准**（7 配置 × 3 轮，CHECKPOINT 隔离，中位）：
  内容摘要 `7375042|9258128314029119699254` **ALL_MATCH=True**；
  fl_off 7.91 s、fl_w1 7.86 s、fl_w2 3.28 s、fl_w4 1.91 s、
  fl_w8 **1.75 s**、fl_w15 **1.73 s**、fl_auto **1.75 s**。
  相对上一版：auto 1.87→1.75（1.07×）、w15 1.91→1.73（1.10×）、
  fl_off 8.24→7.91（1.04×）。
- **构建**：release + debug 双 rc=0，无新增警告；生产 `.so`
  md5 `c43d49a6bc1321a87b8fb326c01a53e3`（基准脚本已校验恢复）。
- **遗留缺陷修复**：测量脚本首次跑基准时遗留了测试索引，导致首轮
  数字全面偏慢（fl_off 10.20 s）；已 DROP 索引后重测，上表为无索引
  干净环境的权威数字。

详见 `docs/CHANGES.md` 第 14 节。
