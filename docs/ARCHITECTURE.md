# pg_fastload 整体架构

> 基于 pg_bulkload 3.1.23 二次实现；原版权 NTT, Inc.，并行化部分 (c) 2026 yuanyuansong。

## 1. 四阶段流水线

一次加载被划分为四个阶段，这是理解并行化改造的主线：

| # | 阶段 | 主要工作 | 原版代码 |
|---|---|---|---|
| ① | 读 CSV | `SourceRead()` 从 INPUT 取字节流 | `lib/source.c` → `CSVParserRead()` |
| ② | 解析 CSV | 记录边界状态机 + 字段切分、去引号/转义 | `lib/parser_csv.c` |
| ③ | 形成元组 | `CheckerConversion()` → `TupleFormerValue()`（输入函数）→ `TupleFormerTuple()`（`heap_form_tuple`） | `lib/reader.c`、`lib/parser_csv.c` |
| ④ | 写入数据库 | `WriterInsert()`，DIRECT / BUFFERED / PARALLEL 三种 writer | `lib/writer_*.c` |

原版主循环（`lib/pg_fastload.c`）逐条驱动：

```c
while (wt->count < rd->limit)
{
    tuple = ReaderNext(rd);        /* ① + ② + ③ */
    if (tuple == NULL) break;
    WriterInsert(wt, tuple);       /* ④ */
    wt->count += 1;
    MemoryContextReset(wt->context);
}
```

`ReaderNext()` 内含 `PG_TRY/PG_CATCH`，负责逐行吸收解析错误并写 `PARSE_BADFILE`。

## 2. 模块划分

```
bin/pg_fastload.c        客户端：读控制文件 → 拼 options text[] → 执行 SQL
lib/pg_fastload.c        服务端入口 pgfastload.pg_fastload()
                          ParseOptions() / 主循环 / 结果统计
lib/reader.c             Reader、Checker、TupleFormer、Filter
lib/parser_csv.c         单进程 CSV 解析器（保持原样，作为降级路径与语义基准）
lib/parser_binary.c      BINARY / FIXED
lib/parser_tuple.c       TUPLE
lib/parser_function.c    FUNCTION
lib/source.c             文件 / STDIN / 异步读
lib/writer.c              Writer 抽象
lib/writer_direct.c       DIRECT（绕过 shared buffers）
lib/writer_buffered.c     BUFFERED
lib/writer_parallel.c     PARALLEL（原版「主读子写」多进程）
lib/pgut/*               原版工具库（保留，PARALLEL writer 依赖 pgut-ipc.c）

并行化新增（Plan C，(c) 2026 yuanyuansong）：
lib/fastload_parallel.c   编排：DSM/队列创建、reader/worker 注册、
                          FastloadParallelSupported 降级判定、
                          FastloadParallelRun（leader 消费循环）
lib/fastload_worker.c     reader 单遍扫描/路由 + worker 消费/解析/回传
                          （bgworker 入口 pg_fastload_reader_main /
                          pg_fastload_worker_main）
include/fastload_parallel.h  DSM 布局、参数/头结构、key 常量
include/pg_profile.h      USE_PROFILE=1 阶段计时埋点
```

## 3. 单进程数据流（PARALLEL=OFF，默认）

```
控制文件 → ParseOptions → ReaderCreate(TYPE) + WriterCreate(WRITER)
        → ReaderInit / WriterInit / CheckerInit / ParserInit
        → [ ReaderNext ①②③ → WriterInsert ④ ] × N 行
        → WriterClose / ReaderClose → 统计结果
```

与原版 `pg_bulkload` 完全一致。

## 4. 并行数据流（PARALLEL=ON 且 TYPE=CSV / BINARY / FIXED，Plan C 单读多解）

```
主 backend（leader，④ 串行）
 ├─ ParseOptions / ReaderInit / WriterInit / CheckerInit / ParserInit（照常）
 ├─ 创建 DSM 段（dsm_create + dsm_pin_segment，按实际 worker 数伸缩）
 │    ├─ key 0            : FastloadShared（spinlock、reader/worker 状态、错误、行数）
 │    ├─ key 1            : FastloadParams（CSV 参数、列定义、SKIP/LIMIT、worker 数）
 │    ├─ key 2+N ..       : 回传 shm_mq × N（worker→leader，2 MiB/个）
 │    └─ key 2..2+N-1     : 输入 shm_mq × N（reader→worker，8 MiB/个）
 ├─ 注册 N+1 个动态 bgworker：handle[0]=reader，handle[1..N]=解析 worker
 └─ 消费循环（④ 串行）
      for each 回传队列 round-robin:
          shm_mq_receive
          → MemoryContextAlloc(wt->context, len) 可写副本
          → 组装 HeapTupleData 外壳
          → WriterInsert(wt, &tup)
          → wt->count++ / MemoryContextReset

reader bgworker（handle[0]，无数据库连接）
 └─ 单遍扫描输入文件（CSV=quote-aware 状态机；BINARY/FIXED=STRIDE 定长切分）
      ├─ SKIP/LIMIT 全局记录窗口裁剪
      ├─ 分块轮转路由（每 ~512 KiB 连续记录切换 owner）
      └─ 512 KiB stage 缓冲攒批发送 → owner worker 的输入 shm_mq
      结束：置 reader_state=DONE → shm_mq_detach（worker 据此判定正常 EOF）

worker #i（独立 backend 进程）
 ├─ dsm_attach → shm_toc_attach → 取 header / params
 ├─ BackgroundWorkerInitializeConnection + StartTransactionCommand
 ├─ 由列定义重建 TupleDesc（不开目标表，避免与主 backend 锁冲突）
 ├─ TupleFormerInit（复用原版输入函数解析）
 └─ 从输入 shm_mq 批量拉取记录（整帧同消息零拷贝，跨消息重组）
      → 字段切分 + 形成元组（②③）
      → 回传 shm_mq_send 成型元组
      → EOF：校验 reader_state=DONE → shm_mq_detach → proc_exit(0)
```

## 5. 为什么 ④ 保持串行

- `WriterInsert()` 会**原地修改** `tuple->t_data`：infomask、xmin/cmin/xmax、OID、`t_self`、`t_ctid`。多进程同时写同一张堆表会引入锁与 WAL 竞争，收益不确定。
- 主 backend 收到 `shm_mq` 消息缓冲（共享内存，只读）后，必须在 `wt->context` 中 `palloc` 出可写副本再交给 `WriterInsert()`，这是必需步骤，不是优化。
- 原版三种 WRITER（DIRECT / BUFFERED / PARALLEL）因此全部无需改动。
- **2026-11-09 写入路径复查**：DIRECT writer 的 WAL 本就是每次装载一条首页
  记录（`writer_direct.c` 首页守卫），数据写已按 1024 页（8 MiB）块缓冲批量
  `write()`，无批量化空间；WRITE 段 ~1.3 s/1 GiB 是纯逐元组 CPU
  （PageAddItem + 元组头戳记 + SpoolerInsert），为 leader 串行下限。
  并行填页的理论上限仅 ~1.10×（auto 档管线关键路径是 reader READ 1.66 s），
  依据"收益有限即放弃"原则维持 ④ 串行（详见 `docs/CHANGES.md` 第 12 节）。

## 6. 为什么 worker 用独立 backend 进程而不是线程

`CurrentMemoryContext` 是进程全局变量，`PG_exception_stack` 走 `siglongjmp`，`ereport(ERROR)` 依赖 backend 的错误栈。`TupleFormerValue()` 会调用 PG 输入函数（`int4in` / `numeric_in` / `bpcharin`），这些只能在完整的 backend 进程里安全执行。`RegisterDynamicBackgroundWorker` 提供的正是这种进程：有自己的 MemoryContext、syscache、事务，可以安全复用原版的 ③ 逻辑。

## 7. 与原版 PARALLEL writer 的区别

原版 `WRITER = PARALLEL`（`lib/writer_parallel.c`）是「主进程读、另起一个 libpq 连接的 backend 写」，用 `pgut/pgut-ipc.c` 的 `Queue` 传元组，目的是把 ④ 拆出去。`pg_fastload` 的 `PARALLEL` 是「多个 worker 做 ①②③、主 backend 做 ④」，方向相反，且改用 PostgreSQL 原生 `dsm/shm_toc/shm_mq`。两者互不冲突：`pgut-ipc.c` 仍保留在 Makefile 中，因为原版 PARALLEL writer 还依赖它。
