# pg_fastload 并行化设计（Plan C 单读多解）

> 本文件描述 `PARALLEL=ON` 且 `TYPE=CSV` 时的现行多进程实现。
> v1.0.0 首版采用"方案 B"（N 个 worker 各自全量扫描 + `rec_no % N` 取模路由），
> 因冷缓存 I/O ×N 反噬（实测比串行慢 6.49×）被 Plan C 取代；方案 B 的设计
> 动机以历史注记形式保留在文末附录。

## 1. 架构总览

```
主 backend（leader）
 ├─ 照常完成 ParseOptions / ReaderInit / WriterInit / CheckerInit / ParserInit
 ├─ 创建 DSM 段（dsm_create + dsm_pin_segment，按实际 worker 数伸缩）
 ├─ 注册 N+1 个动态 bgworker：handle[0] = reader，handle[1..N] = 解析 worker
 └─ 串行执行 ④ WriterInsert()（消费 worker 回传的成型元组）

reader bgworker（handle[0]，无数据库连接，仅 BGWORKER_SHMEM_ACCESS）
 └─ 单遍扫描 CSV（quote-aware 记录边界状态机）
     → 按 SKIP/LIMIT 全局记录窗口裁剪
     → 分块轮转路由（每 ~512 KiB 连续记录切换 owner）
     → 写入 owner worker 的输入 shm_mq

worker #i（handle[1..N]，完整 backend 进程）
 ├─ 从输入 shm_mq 批量拉取字节块，切出完整记录
 ├─ 字段切分 → TupleFormerValue/TupleFormerTuple（heap_form_tuple）
 └─ 经回传 shm_mq 把成型元组发回 leader
```

要点：**文件 I/O 只发生 1 次**（reader），解析 CPU 分布到 N 个 worker，
④ 写入保持单进程事务语义。

## 2. 为什么是单 reader（Plan C 的动机）

方案 B 中每个 worker 各自 `open()` 全量扫描，总读量 ≈ `N × 文件大小`：
热缓存（文件在 page cache 内）只是浪费内存带宽；**冷缓存（文件超 page cache）
则变成真实磁盘 I/O 争抢**——2.5 GiB 实测 auto=15 比串行慢 6.49×。

Plan C 把"扫文件"收敛为单点后：

- 冷缓存总读量恒为 1× 文件大小，好窗口下并行**反超串行**
  （1 GiB 实测 w2 ≈3.0 s / w4 ≈1.8 s / auto ≈1.5 s vs 串行 7.1 s）；
- 记录边界状态机只跑一遍，不再 N 份重复。

reader 必须解决"任意字节偏移处引号状态未知"的问题，因此采用**整文件顺序
单遍扫描**而不是按字节区间分段起跑（起跑点落在引号字段内部的 `in_quote`
初值无法确定，CSV 不是可随机访问格式）。

## 3. 记录边界状态机（reader 内执行）

reader 只判断"一条记录到哪结束"，不切字段。状态机带 `in_quote` 上下文：

| 当前状态 | 字节 | 处理 | 是否记录结束 |
|---|---|---|---|
| 引号内 | `escape` + (`quote` 或 `escape`) | 两字节原样保留 | 否 |
| 引号内 | `quote` | 原样保留，退出引号态 | 否 |
| 引号内 | `\r` / `\n` / 其他 | **原样保留**（字段内容） | 否 |
| 引号外 | `quote` | 原样保留，进入引号态 | 否 |
| 引号外 | `\r` | 结束记录；若下一字节是 `\n` 一并吞掉 | **是** |
| 引号外 | `\n` | 结束记录 | **是** |
| 引号外 | 其他 | 原样保留 | 否 |

要点：

- **CRLF 双重语义**：引号内的 `\r\n` 是字段值，必须逐字节保留；只有引号外的
  `\r` / `\n` 才是终止符。
- **单字节 lookahead**：CRLF 与 `""` 转义都需要看下一字节；读块（1 MiB）末字节
  carry 到下一块，EOF 单独处理末字节。
- **最后一条记录无终止符**：`rec_used > 0` 时补发一条；`in_quote` 仍为真则报
  `unterminated CSV quoted field`。
- **跨块长记录**：记录缓冲从 64 KiB 起按需翻倍，上限 256 MiB。

## 4. 路由：分块轮转（替代逐条取模）

reader 按**字节量分块轮转**选择归属 worker：每累计约 `FL_ROUTE_BYTES`
（512 KiB）连续记录切换一次 owner。窗口内所有记录整块写入该 worker 的
输入队列。

- 逐条 `rec_no % N` 交错路由曾在方案 B/早期 Plan C 中造成队头阻塞
  （一条队列满即卡住全部路由，reader blocked 9.2 s），分块轮转消除该问题；
- 轮转**不保证**记录按原始顺序到达各 worker，也不保证各 worker 行数均分
  ——正确性契约是**内容集合一致**（见 §8），行数分布不是契约；
- SKIP/LIMIT 窗口在路由前按全局记录号裁剪，窗口外记录直接跳过。

## 5. 进程间通信：PostgreSQL 内核 shm_mq（双向）

不使用 pg_bulkload 的 `pgut/pgut-ipc.c` `Queue`（原版 PARALLEL writer 仍依赖它，
保留在构建中），pg_fastload 并行路径全部使用 PG 原生原语：

| 组件 | 用途 |
|---|---|
| `dsm_create` / `dsm_attach` / `shm_toc_*` | 共享段创建与对象定位 |
| `shm_mq_create/set_sender/set_receiver/attach/send/receive/detach` | 双向消息队列 |

**DSM 布局**（key 见 `include/fastload_parallel.h`）：

| key | 对象 | 说明 |
|---|---|---|
| 0 | `FastloadShared` | spinlock 保护的 reader/worker 状态、错误文本、行数 |
| 1 | `FastloadParams` | CSV 参数、列定义数组、SKIP/LIMIT、worker 数等 |
| `2 .. 2+N-1` | 回传 shm_mq × N | `FASTLOAD_KEY_MQ_BASE+i`，worker→leader，2 MiB/个 |
| `2+N .. 2+2N-1` | 输入 shm_mq × N | `FASTLOAD_KEY_INPUT_BASE+i`，reader→worker，8 MiB/个 |

- **reader→worker**：reader 是 sender，以整条记录为消息分帧发送；
  reader 侧 512 KiB stage 缓冲攒批发送（减少系统调用）。
- **worker→leader**：worker 是 sender，leader 是 receiver，成型元组逐条发送。
- **EOF 语义**：reader 扫描完成后先置 `reader_state = FL_WORKER_DONE`，
  再对全部输入队列 `shm_mq_detach`；worker 读到 `SHM_MQ_DETACHED` 后
  校验 `reader_state` 区分**正常收尾**与 **reader 崩溃**（崩溃则报错退出）。
- **容量依据**：8 MiB 输入队列是实测决定性参数——1 MiB 只能缓冲约 6 ms 的
  worker 工作量，reader 被单个队列背压阻塞时其余队列全部挨饿（见
  `docs/CHANGES.md` 第 10 节 C-3）。
- `max_worker_processes ≥ N + 1`（N 个解析 worker + 1 个 reader）。

## 6. worker 消费与解析

- **批量拉取**：一次 `shm_mq_receive` 通常返回一个整帧（记录），零拷贝直接
  解析；跨消息的长记录重组进 `pend` 缓冲。
- **SKIP/LIMIT 语义**：全局记录号窗口 `[skip, skip+limit)`，先减法再比较防
  int64 溢出；窗口由 reader 裁剪，worker 收到的即窗口内记录。
- **字段语义**：NULL 判定沿用原版——用去引号前的 raw 长度与 NULL 标记比对
  （`""` 的 raw 长度为 2，不等于空 NULL 标记，加载为空串而非 SQL NULL）；
  字段数校验沿用原版报错文本。
- **元组成形**：主 backend 把列定义序列化进 DSM，worker 重建 TupleDesc
  （**不打开目标表**，避免与 leader 锁冲突）；`TupleFormerInit` 需要
  syscache，由 `BackgroundWorkerInitializeConnection` +
  `StartTransactionCommand()` 提供。

## 7. leader 消费（④ 串行）

```c
/* 回传消息缓冲位于共享内存（只读），必须先复制 */
buf = MemoryContextAlloc(wt->context, len);
memcpy(buf, data, len);
tup.t_len = len;
tup.t_data = (HeapTupleHeader) buf;
WriterInsert(wt, &tup);          /* 原地修改 t_data，故必须可写副本 */
wt->count += 1;
MemoryContextReset(wt->context);
```

leader 按 round-robin 轮询 N 条回传队列，全部 worker DONE 或达到 LIMIT 即
结束。编排对象（DSM/队列/句柄）放在独立 MemoryContext（`runctx`），
**不放** `wt->context`（每行被 reset）。

## 8. 正确性契约

heap 表不承诺物理行序，`WriterInsert` 不依赖输入顺序。并行加载的正确性
判据是**内容集合一致**：

```
count(*) 相同 且 sum(hash_record_extended(t, 0)) 相同
```

（顺序无关摘要；不使用 `md5(string_agg)` 之类依赖行序的校验。）
验证套件：`tools/fastload_verify.py`（CSV，90/90 用例）与
`tools/fastload_verify_fixed.py`（BINARY/FIXED，44/44 用例）以此为断言。

## 9. 并行度与串行回退

```
PARALLEL_WORKERS 显式指定 → 取该值（1..64）
未指定                   → 在线 CPU 核数 − 1（下限 1）
有效 worker 数 ≤ 1        → FastloadParallelSupported() 返回 false，串行回退
```

④ 写入由 leader 独占一个进程/核，解析进程数取 `CPU − 1`
（16 核 → 15 解析 + 1 reader + 1 写入 leader；注意 reader 也占一个
bgworker 槽位）。

**自动降级清单**（命中任一条走原版单进程路径并打印 INFO）：

- `PARALLEL` 未开启，或有效 worker 数 ≤ 1；
- `TYPE` 不是 CSV / BINARY / FIXED（即 `TUPLE / FUNCTION`）；
- 指定 `FILTER` 或 `FORCE_NOT_NULL`；
- 目标表含 dropped 列；
- 显式 `ENCODING` 需逐字段转码（`checker.check_encoding`）；
- 表含 NOT NULL 列或指定 `CHECK_CONSTRAINTS`（并行 worker 不执行
  `CheckerConstraints()`）；
- `PARSE_ERRORS != 0`（含 `PARSE_BADFILE` 错误吸收语义）；
- `INPUT` 为 STDIN 或缺失（reader 需要可独立 `open()` 的文件路径）；
- BINARY/FIXED 专属：无 COL 描述、STRIDE ≤ 0（解析器初始化异常）、
  任一 NULLIF 模式长于 64 字节（DSM 定长槽位装不下）；
- `wt->desc` 不可用。

**注意**：`rd->checker.tchecker` **不是**降级条件——所有 writer 都无条件调用
`CreateTupleChecker()`，该指针恒非 NULL；对 TYPE=CSV 其判定为 NO_COERCION
并原样返回 tuple，worker 侧 `TupleFormerTuple()` 输出与之逐位等价。

## 9a. BINARY / FIXED（定长）并行模式

`TYPE=BINARY` 与 `TYPE=FIXED` 在 `reader.c` 中映射到同一个定长解析器
（COL 字段描述 + STRIDE 记录长度），并行化共用 CSV 的全部管线基础设施，
只替换两处：

**① reader 侧记录边界（`fl_reader_scan_fixed`）**：无引号、无分隔符，
按 STRIDE 定长切分。读入 1 MiB chunk + 结转不足一条记录的尾部字节，
每凑满一条 STRIDE 即按既有分块轮转路由（同 8 字节帧协议、同
SKIP/LIMIT 窗口语义——SKIP 单位=记录，与串行解析器的 OFFSET 一致）。
EOF 尾残字节 WARNING 忽略，对齐串行行为。

**② worker 侧字段解析（`fl_emit_record_fixed`）**：leader 把解析好的
COL 描述符经 `BinarySerializeFields()` 序列化进 DSM 参数块
（kind 索引 + offset + len + nullif），worker 用
`BinaryDeserializeFields()` 重建 `Field[]`——read/write 函数指针**不跨
进程传递**，由 kind 从静态 `TYPES[]` 表重新解析，CHAR 类字段的 work
buffer 由 worker 重新分配。解析流程镜像串行 `ExtractValuesFromFixed()`：
队列内存对 worker 只读，先拷贝到 scratch；每字段尾临时写 NUL（供
`Read_char` 的 strlen/截尾与 NULLIF 定界）后调用 `field->read()`，随即
还原。STRIDE 校验（len != rec_stride 报错）保证 reader 侧任何切分缺陷
都会显式失败而非静默错位。

数值字段（INTEGER/BIGINT/DOUBLE）保持串行语义：按目标列 typId 直接
C-cast（`Read_int32` 等宏族），数值转换零字符串解析开销——这是定长
串行装载（1 GiB 1.62 s）远快于 CSV 串行（8.2 s）的主因。

实测（1 GiB / 10,737,418 行 × 100 B，热缓存好窗口）：
fl_off 1.62 s、w2 1.06 s、w4 0.94 s、w8 0.89 s、auto 0.91 s
（并行最高 **1.82×**；内容摘要 7 配置一致）。

## 10. worker 启动竞态

小文件/空文件下，worker 可能在 leader attach 前就跑完并 detach，此时
`shm_mq_receive` 返回 `SHM_MQ_DETACHED`（`state=2` 即 `FL_WORKER_DONE`）。
这不是失败：PG 官方语义保证 sender detach 后 receiver 仍可读取队列剩余消息。
编排层只在 `DETACHED` 且状态**不是** `FL_WORKER_DONE` 时报错。已用 5 行
小文件连续 10 次并行加载做竞态压力验证（90 项用例含此场景）。

## 11. 实测结论（16 vCPU / 7.3 GiB，1 GiB lineitem，热缓存 3 轮中位）

| 配置 | 原始基线（方案 B v1.0.0） | 终版（Plan C + shm_mq + 大块 I/O） |
|---|---|---|
| PARALLEL=OFF | 8.19 s | 8.24 s（≈持平，串行路径零改动） |
| w=1 | 16.215 s | 8.23 s（串行回退） |
| w=2 | 7.76 s | **3.32 s（2.34×）** |
| w=4 | 3.27 s | **1.96 s（1.67×）** |
| w=8 | 2.53 s | **1.88 s（1.35×）** |
| auto(=15) | 2.32 s | **1.87 s（1.24×）** |

- 高并发饱和于 leader 串行 `WriterInsert`（~1.3 s/1 GiB 的逐元组 CPU 下限）。
- 冷缓存（fadvise 逐出）：好窗口 w2 ≈3.0 / w4 ≈1.8 / auto ≈1.5 s，
  **反超串行 7.1 s（最高 4.9×）**；残留随机卡顿轮次为宿主 hypervisor 层
  延迟放大（详见 `docs/VERIFICATION.md` 第 12 节）。
- 写入路径复查结论（2026-11-09）：DIRECT writer 的 WAL 本就是每次装载一条
  首页记录、数据写已按 8 MiB 批量进行，④ 无批量化空间；并行填页上限仅
  ~1.10×（READ 1.66 s 为管线关键路径），故维持 ④ 串行。

## 附录：方案 B 历史注记（已废弃）

v1.0.0 首版方案：N 个 worker 各自全量扫描同一 CSV，`rec_no % N` 取模归属，
仅对归属记录做字段切分与元组成形。该设计解决了"字节分段起跑点引号状态
未知"的正确性难题，但付出 N× I/O 与 N× 边界扫描的代价；热缓存下
32M 行实测 2.82×，冷缓存反转至 0.15×（慢 6.49×）。方案 B 的记录边界
状态机、SKIP/LIMIT 窗口语义、降级清单与竞态容忍设计被 Plan C 全部继承。
完整历史见 `docs/CHANGES.md` 第 10 节与 `pg_fastload_PlanC单读多解优化报告.md`。
