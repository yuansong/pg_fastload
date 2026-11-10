# pg_fastload 验证报告

> 版本 `1.0.0`　作者：yuanyuansong　最后更新：2026-11-10
>
> 本文记录**当前版本**的验证状态，按「功能 → 构建 → 内存 → 合规」四个维度给出可复现的结论。
> 性能数字见 [BENCHMARK.md](BENCHMARK.md)；逐轮优化过程见 [OPTIMIZATION.md](OPTIMIZATION.md)。

---

## 1. 交付范围

| 项 | 内容 |
|---|---|
| 项目 | `pg_fastload`（独立开源提交，版本 `1.0.0`） |
| 上游 | `pg_bulkload` 3.1.23，HEAD `798e0f2`，BSD 3-Clause 风格许可 |
| 并行覆盖 | `TYPE=CSV` 与 `TYPE=BINARY / FIXED`（定长）；`TUPLE / FUNCTION` 走串行 |
| 导出覆盖 | `pgfastload.pg_fastload_export()`：`TYPE=CSV / BINARY / FIXED`，源为 `TABLE=` 或 `QUERY=`（单进程，见 §12） |
| 对外行为 | `PARALLEL=ON` 时上述类型默认走多进程并行；**不引入任何额外类型名**，`TYPE` 仍是上游五种；`PARALLEL_WORKERS` 默认「在线 CPU 核数 − 1」 |
| 许可 | 保留 `COPYRIGHT` 原文；新增 `LICENSE` / `NOTICE`；新增/修改文件标注 `Copyright (c) 2026, yuanyuansong`；明确未获 NTT, Inc. 背书 |

---

## 2. 验证环境

| 项 | 值 |
|---|---|
| 主机 | VM `192.168.25.134`，**16 vCPU**（i7-13700KF）/ **7.3 GiB** 内存 / 3 GiB swap |
| OS | Oracle Linux Server 10.1，内核 6.12（VMware） |
| PostgreSQL | 18.6（`REL_18_STABLE`，HEAD `1da6ddbdc3`）；postmaster 经核验为 **release 版**二进制 |
| 安装 | release `/home/ysyuan/postgres/release`、debug `/home/ysyuan/postgres/debug`（`--enable-debug --enable-cassert -O0 -g`） |
| 测试集群 | `PGDATA=/tmp/pgdata`，`port=5433`，库 `tpcc`，`max_worker_processes=32` |
| 数据 | `/tmp/tpch/lineitem.csv`：1 GiB / 7,375,042 行 / 16 列；`/tmp/tpch/fixed1g.bin`：1 GiB / 10,737,418 行 × 100 B |
| 对比基线 | 干净上游 `pg_bulkload 3.1.23`（源码 HEAD `798e0f2`，构建产物无任何并行相关符号），确认为原版 |

---

## 3. 构建验证

| 构建 | `clean` | `make` | `install` | 新增诊断 | `pg_fastload_worker_main` 导出 |
|---|---|---|---|---|---|
| release | rc=0 | rc=0 | rc=0 | 无 | `T` 符号存在 |
| debug（`--enable-cassert`） | rc=0 | rc=0 | rc=0 | 无 | `T` 符号存在 |

唯一编译诊断是**上游原版** `bin/binary.c` 的两条 `-Wmissing-variable-declarations`
（`TYPES` / `ALIASES`），与新增代码无关。

---

## 4. 功能正确性

判据：串行基线（`PARALLEL=OFF`）与并行（`PARALLEL=ON`）的**内容集合**一致。
指纹为 `count(*) || sum(hash_record_extended(t, 0))`——顺序无关，适配并行路径不保序的特性。

### 4.1 CSV：90/90 PASS

| 阶段 | 内容 | 用例形态 |
|---|---|---|
| Phase 1 | 10 个边界数据集 × 6 项断言 | `basic`（LF）、`crlf`、`lone_cr`、`quoted`（引号内逗号/换行/转义 `""`/quoted empty）、`no_eol`（末行无换行）、`nulls`（空字段→NULL、`\N` 字面量、首尾空格）、`empty`（0 行）、`one_row`、`cross_chunk`（终止符/引号内换行/转义恰好跨 1 MiB 读块边界）、`many`（10,000 行） |
| Phase 2 | 自定义解析参数 | `DELIMITER=\|`、`QUOTE=~`、`ESCAPE=~`、`NULL=NA`，串并行摘要一致 |
| Phase 3 | `SKIP` + `LIMIT` | `SKIP=1 LIMIT=3`、workers=3，摘要逐位一致 |
| Phase 4 | 并行度变化 | workers = 1 / 2 / 3 / 4 / 未指定（auto=15） |
| Phase 5 | 自动降级回归 | `FORCE_NOT_NULL=c1` → 日志出现 `PARALLEL disabled`、未启 bgworker、内容正确 |
| Phase 6 | 启动竞态压力 | 5 行小文件连续 10 次 `PARALLEL=ON workers=2` |

Phase 1 的 6 项断言（每个数据集）：串行可执行；串行未误启并行；并行可执行；内容摘要一致；
bgworker 真启动（日志有 `PARALLEL=ON, TYPE=CSV, workers=N` 且无 `PARALLEL disabled`）；
各 worker 产出之和 = 表行数，且日志无真实错误标记（按 `ERROR:` / `FATAL:` 前缀判定）。

### 4.2 定长：44/44 PASS

覆盖：纯 `CHAR` 定长文本、`BINARY`/`FIXED` 别名一致性、混合文本 + 二进制数值
（INTEGER(4) / BIGINT(8) / DOUBLE(8) 原生字节解码）、`NULLIF`（文本模式与数值等长模式）、
`STRIDE` 大于字段和（尾部填充）、EOF 尾残字节忽略（WARNING，与串行一致）、`SKIP`+`LIMIT` 窗口、
`PRESERVE_BLANKS`+`VARCHAR` 尾空格保留、空文件、workers 1/2/3/4/auto（w=1 串行回退）、
`FILTER` 自动降级、10,000 行 × 4 worker 分片完整性。

### 4.3 `MULTI_PROCESS` 组合：8/8 PASS

`MULTI_PROCESS=YES`（即 `WRITER=PARALLEL`）与 `PARALLEL=ON` 同时指定时必须「不叠加」：
串行装载正确、组合装载正确、日志出现 `PARALLEL disabled (MULTI_PROCESS ...)`、
无 bgworker 启动、内容摘要一致、`MULTI_PROCESS` 单独使用仍正常。

### 4.4 1 GiB 内容摘要

| 套件 | 摘要 | 配置数 | 结果 |
|---|---|---|---|
| CSV 1 GiB | `7375042\|9258128314029119699254` | 7（off / w1 / w2 / w4 / w8 / w15 / auto） | **ALL_MATCH = True** |
| 定长 1 GiB | `10737418\|12495206446709334590242` | 7 | **ALL_MATCH = True** |

---

## 5. 性能验证

结论摘要（1 GiB CSV，热缓存，3 轮中位，`DIRECT`，无索引）：

| 配置 | 耗时 | 相对串行 |
|---|---|---|
| `PARALLEL=OFF` | 5.81 s | 1.00× |
| `w=1`（串行回退） | 5.80 s | 1.00× |
| `w=2` | 2.76 s | 2.11× |
| `w=4` | 1.58 s | 3.68× |
| `w=8` | 1.40 s | 4.15× |
| `auto`（默认 15） | 1.37 s | 4.24× |

对两个参照实现（同一文件、同一空表）：`PARALLEL=OFF` 5.81 s 对上游 `pg_bulkload 3.1.23` 7.00 s
（**1.20×**）、对 `COPY ... FROM` 8.48 s（**1.46×**；对 UNLOGGED 表 `COPY` 7.67 s，1.32×）。
第八轮改动前先确认了串行等价性（6.95 s 对 7.09 s，差 2%），所以这个差距来自优化，不是行为差异。

定长 1 GiB：串行 1.62 s → 并行最快 0.89 s（w=8，**1.82×**）。
冷缓存（`posix_fadvise` 逐出）好窗口下反超串行最高 4.9×。
完整方法、公平性控制与分档结果见 [BENCHMARK.md](BENCHMARK.md)。

**测量注意事项**（历史教训）：计时之外必须 `CHECKPOINT` 隔离，否则上一轮脏页刷盘会造出
5.58× 的假加速比；`TRUNCATE` 与 `pg_fastload()` 不能写在同一条 `psql -c` 里（`MULTI_PROCESS=YES`
时会自锁到超时）。

---

## 6. 内存管理与合规静态核查

| 核查项 | 结果 |
|---|---|
| 新增并行源码（`fastload_parallel.c` / `fastload_worker.c` / `fastload_parallel.h`）的 `malloc/calloc/realloc/free` | **NONE** |
| `release/lib/pg_fastload.so` 动态符号表中的 malloc 族未定义符号 | **NONE** |
| 上游 `bin/` 客户端与 `lib/pgut/pgut-pthread.c`（Windows pthread 兼容层）的 malloc | 保留原样：属命令行前端，不参与服务端加载 |
| `COPYRIGHT` | 原文完整（BSD 3-Clause 三条 + 免责条款） |
| `LICENSE` / `NOTICE` / `README*` / `docs/` | 明确「基于 pg_bulkload 二次实现」、作者 `yuanyuansong`、未获 NTT 背书 |
| 上游引用标注 | **合规必需，已写清楚**：`LICENSE` / `NOTICE` / `README.md` / `docs/CHANGES.md` / `docs/ARCHITECTURE.md` 及新增源文件头注释均写明上游项目名、版本 `3.1.23`、HEAD `798e0f2`、来源 URL、BSD 3-Clause、原版权 `NTT, Inc.`、改动清单与作者署名 |
| 类型列表 | 与上游一致：`BINARY / FIXED / CSV / TUPLE / FUNCTION`，无任何额外类型名 |

> **「引用」与「更名」的区别**：对 `pg_bulkload` 的引用是 BSD 3-Clause 第 1、2、3 条的署名与非
> 背书要求，属合规必需，不是要清除的「残留」。全量更名针对的是项目 / 扩展 / 共享库 / SQL schema /
> SQL 函数 / 宏等**代码标识符**，与合规引用互不冲突。

**内存生命周期（palloc / MemoryContext）**

- worker：`workctx = AllocSetContextCreate(TopMemoryContext, ...)` 承载长生命周期对象；
  `recctx` 为每条记录 scratch，`shm_mq_send()` 已把字节复制进队列后 `MemoryContextReset(recctx)`，
  1.3 亿条记录不会累积。
- 主 backend：编排对象（DSM / `shm_mq_handle` / bgworker handles / `done[]`）放在独立 `runctx`，
  **不放在 `wt->context`**——后者每轮 `WriterInsert()` 后会被 reset。
- 每条收到的消息在 `wt->context` 中 `MemoryContextAlloc` 出可写副本后交给 `WriterInsert()`
  （该函数会原地修改 `t_data`），随后 `MemoryContextReset`；函数收尾 `MemoryContextDelete(runctx)`。
- `PG_CATCH()` 中统一执行 `fastload_cleanup()`（终止 worker、`shm_mq_detach`、
  `dsm_unpin_segment` + `dsm_detach`）再 `PG_RE_THROW()`。

---

## 7. 各轮验证结论（已采纳的优化）

| 轮次 | 改动 | 验证结论 |
|---|---|---|
| P0（第二轮） | w=1 串行回退、记录拷贝 run-based 化、并行 profile 埋点 | CSV 90/90；w=1 16.215→8.070 s；内容摘要一致；profile `GLOBAL` 合计 ≈ elapsed，`WRITER.TOAST` 伪计时消除 |
| 单读多解（第三轮） | 单 reader 单遍扫描 + 分块轮转 + 批量取环 + 8 MiB 队列 | CSV 90/90；7 配置摘要一致；w2 6.82→3.46 s；worker SCAN 5.2→0.32 s；reader 阻塞时间→~0 |
| shm_mq + 大块 I/O（第四轮） | reader→worker 改用内核 shm_mq；读 1 MiB / 发 512 KiB | CSV 90/90；热缓存持平略优（w2 3.46→3.32 s）；冷缓存好窗口反超串行最高 4.9×；净删约 200 行手写同步代码 |
| 定长并行（第五轮） | BINARY/FIXED 接入单读多解管线 | 定长 44/44 + CSV 90/90；定长 1 GiB 1.62→0.89 s（1.82×）；7 配置摘要一致 |
| W-3a（第六轮） | `SpoolerInsert()` 无索引早退 | CSV 90/90 + 定长 44/44；fl_off 8.24→7.91 s、auto 1.87→1.75 s；无索引路径零回归 |
| 代码整理（第七轮） | 死字段/重复逻辑/别名/锁临界区/组合防护 | CSV 90/90 + 定长 44/44 + MULTI_PROCESS 8/8；7 配置摘要一致；性能在 ±5% 波动内 |
| 串行 CSV 批量切分（第八轮） | 无引号记录在 `CSVParserRead()` 与 worker 的 `fl_parse_fields()` 走 memchr 切分；编码循环外提到 `check_encoding` 之后 | CSV 90/90（含 quoted / lone-CR / cross-chunk / 空行 / 多余列等回退用例）+ 定长 44/44；15.19/16.15/17.11/18.6/19rc1 编译干净；fl_off 7.99→5.81 s、auto 1.76→1.37 s；两条路径摘要均为 `7375042\|9258128314029119699254` |

评估后未采纳的方案（`WRITER=PARALLEL` 作为并行方案、WAL/数据写批量化、并行填页流水线、
并行 B+ 树构建）及其证据见 [OPTIMIZATION.zh-CN.md](OPTIMIZATION.zh-CN.md#八未采纳方案汇总)。

---

## 8. 残留风险与环境限制

1. **ASan 无法执行**：VM 缺 `libasan.so.8` runtime，`dnf` 安装需提权且被环境拦截。
2. **未跑动态泄漏检测**：valgrind 需以 `--trace-children=yes` 另起带 debug 版 PG 的独立
   PGDATA/端口，会打断当前 5433 测试集群；本轮内存结论为**静态生命周期核对**（§6）。
   PG 启动固有的 `208 B definite lost` 与 `relcache.c` 的 `Syscall param write(buf)` 属正常，
   不是扩展泄漏。
3. **全量 10.7 GiB 性能对比未完成**：130M 行单事务需约 13 GiB 表 + 10 GiB WAL，超出 VM 51 GiB
   根分区（`No space left on device`）。因此不给出全量加速比，收益不做外推。
4. **冷缓存仍有随机卡顿轮次**：`vmstat` 在卡顿窗口内显示 0 steal / 0 iowait / 100% idle，
   读总量与带宽正常，属宿主/hypervisor 层对阻塞调用的延迟放大；串行路径因 stdio 大块 `fread`
   的系统调用次数少约 500× 而基本免疫。代码侧的系统性成分已消除（见 §7 第四轮）。
5. **并行路径无逐行容错**：坏行会使该 worker 退出并令整次加载失败，不写 `PARSE_BADFILE`；
   单进程路径行为不变。
6. **不保序**：分块轮转路由不保证物理插入顺序（heap 表本身也不保证）。
7. **编码假设**：并行路径假设输入编码 = 数据库编码，不调用 `CheckerConversion()`。

---

## 9. 多版本适配验证（2026-11-10）

对 PostgreSQL **15.19 / 16.15 / 17.11 / 19rc1** 分别做"独立集群 + 独立构建"的验收，
脚本为 [`util/fl_ver_test.sh`](../util/fl_ver_test.sh)（18.6 的验证见 §3–§5）。

五个主版本共用**同一份代码、同一个分支 `master`**：所有差异都是 `PG_VERSION_NUM` 分支，15 以下
在编译期即被拒绝。2026-11-10 用 `git archive` 导出 `master` 后逐版本重新构建确认（五个版本
`INSTALL_OK`，各自 `CREATE EXTENSION` 并做串行/并行摘要比对）；证据 `compat_master_pg1x.out`。

| 环境 | 值 |
|---|---|
| 构建 | 源码树内 `make clean && make && make install`，`PG_CONFIG` 指向目标版本前缀 |
| 集群 | 每版本临时 `initdb`，端口 `54<major>`，`max_worker_processes=32`，`fsync=off` |
| 数据 | 200,000 行 CSV（含 `"q,%d"` 引号字段）+ 等量 100 字节定长记录 |
| 计时 | 不做性能比较，只看**内容与行为**是否正确 |

| 用例 | 断言 | 15.19 | 16.15 | 17.11 | 19rc1 |
|---|---|---|---|---|---|
| CSV `PARALLEL=OFF` vs `w2/w4/w8/auto` | 行数 200,000 且摘要一致 | PASS | PASS | PASS | PASS |
| 定长 `TYPE=BINARY` `OFF` vs `w4/w8` | 行数 10.7M 级一致、摘要一致 | PASS | PASS | PASS | PASS |
| `FILTER` 降级 | 串行与 `PARALLEL=ON` 摘要一致 + `LOGFILE` 出现 `PARALLEL disabled (FILTER` | PASS | PASS | PASS | PASS |
| 解析容错 | 默认停在第 1 条坏记录（保留已装载行）；`PARSE_ERRORS=2` 保留 2 条好记录 | PASS | PASS | PASS | PASS |
| CLI 客户端 | `pg_fastload -h -p -U -d x.ctl` 退出码 0、装载 200,000 行 | PASS | PASS | PASS | PASS |
| 服务端日志 | 无 `PANIC` / 段错误 | PASS | PASS | PASS | PASS |

过程中发现并修复的两个真实缺陷：

1. **PG19 `FuncnameGetCandidates()` 出参**：19 新增 `int *fgc_flags` 且**无条件解引用**，
   原实现传 `NULL`，导致任何 `FILTER` / `TYPE=FUNCTION` 装载直接段错误（后端进程被信号 11 终止，
   集群进入恢复）。改为传一次性 `int[1]` 复合字面量后通过。
2. **`MyBackendId` 移除**：PG19 删除了整个 `BackendId` API，`pg_fastload.so` 装载时报
   `undefined symbol: MyBackendId`。改为 `FastloadBackendNumber` 宏（≥17 用 `MyProcNumber`）。

另外核实并修正了一处文档错误：`PARSE_ERRORS` 超限的实际行为是**停止读取并正常返回**
（保留已装载行），不是手册原来写的"装载失败"。

逐条内核差异与处理方式见 [PORTING.md](PORTING.md)。

---

## 10. 与 pg_bulkload 的差分验证（2026-11-10）

目标：证明 `pg_fastload` 与它 fork 的上游**行为等价**，而不只是自洽（串行 vs 并行）。

| 项 | 值 |
|---|---|
| oracle | 上游 `pg_bulkload` 3.1.23（HEAD `798e0f2`），编译到**同一台服务器、同一集群、同一张表** |
| 比对内容 | SRF 结果行（`skip`/`count`/`parse_errors`/`duplicate_new`/`duplicate_old`）+ 内容摘要 `count(*)\|\|coalesce(sum(hash_record_extended(t,0)),0)` + 消息严重级 |
| 配置 | `pgbulkload.pg_bulkload`、我们 `PARALLEL=OFF`、`PARALLEL=ON PARALLEL_WORKERS=2`、`PARALLEL=ON PARALLEL_WORKERS=4` |
| 规模 | 3 套实验脚本共 75 例 → 225 次配置比对；另有可移植脚本 [`util/fl_diff_test.py`](../util/fl_diff_test.py)（44 例 × 4 配置）可在任何装有两个扩展的主机上复跑 |

用例覆盖：引号/转义/自定义 `DELIMITER`+`QUOTE`+`ESCAPE`/CRLF/裸 CR/空行与纯空白行/缺列与多列/
未闭合引号/`NULL` 串/`FORCE_NOT_NULL`/`ENCODING`/类型转换失败（int、numeric、date）/`SKIP`+`LIMIT`
（含窗口跨引号记录）/`LIMIT=0`/`SKIP` 超出 EOF/唯一索引与主键冲突（`ON_DUPLICATE_KEEP=NEW|OLD`）/
`WRITER=BUFFERED`/`WRITER=PARALLEL`/`MULTI_PROCESS`/`PARSE_ERRORS=0` 与 `=2`/3 万~20 万行输入/
超过 1 MiB 读块的记录/引号与 3 字节 UTF-8 字符跨读块边界/定长 `SKIP`、`LIMIT`、`SKIP`+`LIMIT`
与尾部残记录。

| 配置 | 结果（四个主版本一致） |
|---|---|
| `PARALLEL=OFF`（75 例） | **75/75 与 oracle 完全一致**（结果行、摘要、消息严重级） |
| `PARALLEL=ON` w=2 / w=4（150 例） | **120/150 一致**，30 例为已文档化的并行原子中止（被拒记录：WARNING 相同，表为空 vs oracle 的"错误前的行"） |
| 合计 mismatch | **0**（每版本 225 次比对，总计 900 次） |

该审计查出并修掉 4 个真缺陷（并行错误吸收、`SKIP` 行/记录语义、首个被拒记录后的不确定超集、
被跳过超长行的缓冲增长），详见
[CHANGES.zh-CN.md](CHANGES.zh-CN.md#17-与上游-pg_bulkload-的差分审计2026-11-10)。

**15.19 / 16.15 / 17.11 / 18.6** 上重跑结果相同（合计 900 次比对）。PG **19rc1** 无 oracle：上游
3.1.23 无法在其上编译（`nodes.h: unknown type name 'Datum'`，正是我们 `bin/pgut/pgut-list.h`
修掉的问题），故 19rc1 由单主版本验收（§9）与 90/90、44/44 套件覆盖。

修后回归：`fastload_verify.py` **90/90 PASS**、`fastload_verify_fixed.py` **44/44 PASS**、
`fl_ver_test.sh` 在 15.19 / 16.15 / 17.11 / 19rc1 全部 PASS。

---

## 11. 进度视图验证（2026-11-10）

加载进度必须满足一个 `NOTICE` 无法满足的性质：**装载会话正忙时也能读到**。所以下列检查全部是
一个连接发起装载、另一个连接读 `pgfastload.pg_fastload_progress`。

随扩展交付的脚本：`util/fl_progress_test.py`（每个版本 8 项检查，3M 行生成的 CSV），在
**15.19 / 16.15 / 17.11 / 18.6 / 19rc1** 上各跑一遍。原始跨会话证据：PG 18.6 上的 `_t_view.py`，
输入为 1 GiB TPC-H `lineitem.csv`（7,375,042 行）；另有 `_t_stale.py`，对运行中的装载执行 `SIGKILL`。

| 检查项 | 结果 |
|---|---|
| 默认（不写关键字） | 第二个会话看得到运行中的装载：`datname`、`relname = public.flprog`、`input_type = CSV`、`mode = serial`、`state = running`、`interval_ms = 1000`、`total_bytes` = 文件大小；结束后 0 行 |
| `PROGRESS=OFF` | 全程视图为空，`$PGDATA/pg_fastload/progress/` 下不写任何文件 |
| 只写 `PROGRESS_INTERVAL=100` | 发布开启；行数在攀升（3M 行文件上观测到 2.3M~2.8M），百分比与字节比一致（误差 <1 个百分点） |
| `PARALLEL=ON` w=2 | `mode = parallel`；行数与串行一致 |
| `LIMIT=1000000` | `total_rows = 1000000`，`percent = rows / total_rows`，`rows` 不超过 `total_rows`；装载正好停在 100 万行 |
| `PROGRESS_NOTICE=ON` | 在视图之外恢复 NOTICE 流（100 ms 间隔 4 行）；收尾行 `100.0% | done` 带精确行数，等于函数返回值；**默认关闭**（其余用例 0 条 NOTICE） |
| `PROGRESS_INTERVAL=50` | 报错 `PROGRESS_INTERVAL must be between 100 and 3600000 milliseconds` |
| 单调性与上界 | `rows` 与 `percent` 单调不减，`percent` 落在 `0..100` |
| 跨会话，1 GiB 串行 | 第 0 次轮询 1,117,697 行（15.2%），第 5 次 6.5M；全程 `state = running`；结束后 `rows in view = 0` |
| 清理 | 成功后记录被删除；被 kill 的后端记录以 `state = 'stale'` 报告一次后删除 |
| 开销（串行 3M 行，三次交错运行中位数） | 不开 0.434~0.442 s vs 开 0.434~0.442 s（**−0.2%~+1.9%**，噪声范围），15/16/17/19 同样在噪声内 |

没有任何用例出现行数不符、残留陈旧记录，或 `PROGRESS=OFF` 时视图出现行。CSV 90/90、定长 44/44
不受影响（记录在行路径之外发布，且发布失败不会让装载失败）。

---

## 12. 导出功能验证（2026-11-10）

随扩展交付的脚本：`util/fl_export_test.sh`（PG 18.6，`PGPORT/PGUSER/PGDATABASE` 从环境读），
**39 项断言全部 PASS，退出码 0**。原始输出：`export_pg18.out`。

| 分组 | 检查项 | 结果 |
|---|---|---|
| CSV 等价性 | 与 `COPY (SELECT * FROM exp_t) TO ... (FORMAT csv, HEADER)` **`cmp` 逐字节相同**；返回行数 = 5 | PASS |
| CSV 往返 | `SKIP=1` 跳过表头回灌，内容摘要 `5\|-1683897567441736485` 与源表一致 | PASS |
| CSV 方言 | `DELIMITER=\| QUOTE=' NULL=\N`：内嵌换行、字段内含分隔符/引号、字面量 `\N` 与 NULL 的区分都保留，回灌摘要相同 | PASS |
| `QUERY=` | `WHERE id >= 3 ORDER BY id DESC` 生效（首条数据行 id=5，3 行）；`LIMIT=2` 返回 2 行、文件 2 行 | PASS |
| 空表 | 返回 0，文件 0 字节 | PASS |
| BINARY 布局 | 7 列 44 B/行 × 4 行 = **176 B**；样例 `.ctl` 的 `COL` 与 `OUT_COL` 逐项一致 | PASS |
| BINARY 往返 | `COL=...CHAR(8)` 回灌摘要 `4\|-5144772349234490677` 与源表一致 | PASS |
| 样例 `.ctl` | 含 `# exported from public.exp_bin`；`OUTPUT` 为文件名（不是源表名，避免手滑自追加） | PASS |
| 补位语义 | 同一份 `.bin` 用 `COL=VARCHAR(8)` 回灌，`'xy'` 变成 `'xy      '`（长度 8）—— 文档化行为，与 `Read_varchar` 一致 | PASS |
| FIXED 往返 | 纯文本布局（数值列用 `CHAR(n)` 承载）回灌摘要与源表一致 | PASS |
| NULLIF | 含 NULL 的 3 行表，`OUT_COL=INTEGER NULLIF 00000000` / `CHAR(4) NULLIF '    '` 往返摘要 `3\|3014503705147286095` 一致 | PASS |
| 错误路径 | 缺 `OUTPUT`／相对路径／`BINARY` 缺 `OUT_COL`／`FIXED`+`INTEGER`／`OUT_COL` 数量不符／目标已存在／NULL 无 `NULLIF`／未知关键字／缺 `TABLE`+`QUERY`／非 `SELECT` 的 `QUERY`／`DELIMITER=QUOTE`，11 条全部按预期报错 | PASS |
| 进度 | 800 万行导出期间另一会话读到 `export\|running\|CSV\|serial\|true\|-\|true\|/tmp/.../big.csv`；结束后视图 0 行；产物 > 100 MB、8,000,000 行 | PASS |

吞吐（`public.exp_big`，800 万行 / 521 MB 表 / 312 MiB CSV，热缓存，5 轮取中位）：

| 操作 | 耗时 |
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

输出 `cmp` 相同（带表头 326,888,903 B）。串行路径的分解见上表；自 2026-11-10 起 `PARALLEL=ON` 已是
`TABLE=` 导出的默认行为，串行只用于 `QUERY=`／显式关闭／小表（见 §12.1）。证据：`export_perf.out`、
`export_perf2.out`、`export_perf_bin.out`；数字与解读见
[BENCHMARK §8](BENCHMARK.zh-CN.md#8-导出吞吐pg_fastload_export-与-copy-to-对比)。

回归：导出只新增 writer 与字节计数，装载路径未改；验收后重跑 `fastload_verify.py` 与
`fastload_verify_fixed.py` 仍为 90/90 与 44/44。

多版本：导出代码在 **15.19 / 16.15 / 17.11 / 18.6 / 19rc1** 上均 `INSTALL_OK`。其中 19 上暴露
过一个真实差异：`TimestampTzGetDatum()` 声明在 `utils/timestamp.h`，18 及以前由 `utils/builtins.h`
间接带入，19 不再带入，导致 `export.c` 报 `implicit declaration`；补上显式 include 后通过。
功能验收只在 18.6 上执行（与整个项目的验证范围一致）。

### 12.1 并行导出（2026-11-10）

随扩展交付的脚本：`util/fl_export_parallel_test.sh`。**每个大版本 33 项断言**，五个大版本全跑：
**15.19 / 16.15 / 17.11 / 18.6 / 19rc1 → 165/165 PASS**，各自退出码 0。原始输出：
`export_par_15.out` … `export_par_19.out`，18.6 另见 `_run_harness.out`。

| 分组 | 检查项 | 结果 |
|---|---|---|
| 串行基线 | `PARALLEL=OFF`：200,000 行、200,001 行文本、回灌摘要与源表一致 | PASS |
| 并行 CSV（4 worker） | 200,000 行、200,001 行、表头只出现一次（`id,pad`）、回灌摘要与源表一致、**行集**与串行输出一致（`sort` + `cmp`）、日志 `workers=4` | PASS |
| `LIMIT` 跨 worker | `LIMIT=1000` → 恰好 1000 行 / 1001 行文本 / 1000 个不同 id（块区间之间无重复） | PASS |
| 默认 worker 数 | 不给 `PARALLEL_WORKERS` → 200,000 行、摘要一致、日志 `workers=15`（在线 CPU − 1） | PASS |
| 并行 BINARY | 200,000 行、文件 7,200,000 B（200,000 × 36）、样例 `.ctl` 存在且含 `# exported from public.pe_src`、回灌摘要与源表一致 | PASS |
| 回退 | `QUERY=` → 串行且日志有 `PARALLEL disabled`；10 行小表 → 串行且日志有；`PARALLEL_WORKERS=1` → 串行且日志 `workers=1` | PASS |
| 被拒输入 | `OUTPUT` 已存在（写进程报 `could not create output file`）、`.ctl` 已存在、`PARALLEL_WORKERS=0` | PASS |
| 运行中进度 | 100 万行并行导出期间另一会话读到 `parallel\|export\|running`；结束后视图 0 行；产物 1,000,001 行 | PASS |

这套脚本抓到一个真实缺陷并已修复：`PARALLEL_WORKERS=1` 会走进「默认值」分支（哨兵是 0），
于是静默按 `CPU−1` 并行，而不是走串行路径。现在由 `workers=1 falls back to the serial path`
这条断言钉住。

扩展性（`public.lineitem`，7,375,042 行、1.13 GiB CSV，输出在 tmpfs，5 轮取中位）：

| 配置 | 耗时 | 相对串行 |
|---|---|---|
| `COPY ... TO`（服务端） | 3.956 s | 1.03× |
| `PARALLEL=OFF` | 4.077 s | 1.00× |
| 2 / 4 / 8 个 worker | 1.950 / 0.999 / 0.611 s | 2.09× / 4.08× / 6.68× |
| auto（15 个 worker） | **0.424 s** | **9.60×** |

全量正确性：4 worker 的输出重新装载进 `LIKE` 副本，与行序无关的摘要一致
（`7375042|9258128314029119699254`）。

为什么用 tmpfs：本机磁盘上实测串行 4.06 s、并行 31.0 s；而裸 `write()`+`fsync()` 微基准（不经过
PostgreSQL）复现出「256 KiB 块大小不变，`fsync()` 在约 0.1 s 与恰好 30 s / 60 s 之间跳变」。
也就是说这台机器上任何含 `fsync()` 的计时都在度量 hypervisor 的存储抖动 —— 与装载路径记录的是同一个
现象。证据：`export_par_bench_shm.out`、`export_par_disk_ab.out`、`_wtest.txt`。

装载侧完全不受影响：CSV 90/90、定长 44/44 重跑均通过。

---

## 13. 结论

当前版本在功能正确性（CSV 90/90、定长 44/44、`MULTI_PROCESS` 组合 8/8、导出 39/39、并行导出五个
大版本各 33/33）、上游等价性（与 pg_bulkload 3.1.23 的 225 次差分比对 0 mismatch）、加载进度
（五个大版本各 8/8 视图检查、收尾数字精确、开销不可测）、导出（CSV/BINARY/FIXED，串行路径与 `COPY`
逐字节相同，并行 9.60× 扩展）、构建（release + debug 双 rc=0 且新增代码零警告）、性能（CSV 相对
串行 4.5×、定长 1.82×）、内存生命周期（全 palloc、无 malloc 族符号）、多版本适配（PG 15/16/17/19
全部 PASS）与版权合规（保留原版权 + 明确署名与非背书）九个维度达到交付标准。

残留风险均可归因于环境限制或明确的设计取舍，已在 §8 与 [OPTIMIZATION.md](OPTIMIZATION.md)
中写明，并给出相应的使用建议（例如冷缓存场景的 worker 数选择、需要容错时使用 `PARALLEL=OFF`）。
