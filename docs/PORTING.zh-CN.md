# pg_fastload 多版本适配（PostgreSQL 15 – 19）

> **结论**：同一份源码在 PostgreSQL **15.19 / 16.15 / 17.11 / 18.6 / 19rc1** 上均可编译、
> `CREATE EXTENSION` 并通过同一套功能验收；版本差异全部收敛在 `#if PG_VERSION_NUM` 分支与
> 按版本内置的 nbtree 构建器里，**不维护分叉的业务代码**。
>
> 验证日期：2026-11-10　测试脚本：[`../util/fl_ver_test.sh`](../util/fl_ver_test.sh)（远端执行）

---

## 1. 支持矩阵

| 主版本 | 已验证小版本 | 状态 |
|---|---|---|
| 15 | 15.19 | 编译 + 功能验收通过 |
| 16 | 16.15 | 编译 + 功能验收通过 |
| 17 | 17.11 | 编译 + 功能验收通过 |
| 18 | 18.6（HEAD `1da6ddbdc3`） | 编译 + 功能验收通过；基准数据来自该版本 |
| 19 | 19rc1 | 编译 + 功能验收通过 |

分支约定：**只有一个分支 `master`。**

- 五个主版本共用同一份代码，没有 `REL_1x_STABLE` 分版本分支，也不需要跨分支 cherry-pick：
  改一处，五个版本同时受益。
- 版本差异全部在编译期解决 —— `#if PG_VERSION_NUM` 分支加 §3.1 列出的按版本内置 nbtree 构建器；
  没有运行期分派，也没有需要额外记住的编译开关。
- `include/pg_fastload.h` 用 `#error` 拒绝 15 以下的主版本：不受支持的组合在编译期就失败，
  不会产出一个运行期才出问题的二进制。
- 早期版本发布的五个分版本分支其实指向同一个提交，代码没有任何差异；现已合并为 `master`
  （见 [CHANGES.zh-CN.md](CHANGES.zh-CN.md) §19）。

## 2. 构建

```bash
# 以 PostgreSQL 15 为例，<prefix> 是该版本的安装前缀
make USE_PGXS=1 PG_CONFIG=<prefix>/bin/pg_config
make USE_PGXS=1 PG_CONFIG=<prefix>/bin/pg_config install
```

**注意**：`make clean` 也必须带上同样的 `USE_PGXS=1 PG_CONFIG=...`。若主机 PATH 中没有 `pg_config`，
裸 `make clean` 不会加载 PGXS，**目标文件不会被删除**，于是切换主版本时会静默复用上一版本编译的 `.o`，
表现为 `CREATE EXTENSION` 时报 `undefined symbol`（本项目的验收脚本曾踩到，已在脚本内修正）。

一次安装同时产出：

| 产物 | 位置 |
|---|---|
| `pg_fastload.so` | `<prefix>/lib/` |
| `pg_fastload.control`、`pg_fastload--1.0.0.sql` | `<prefix>/share/extension/` |
| `pg_fastload`（命令行客户端） | `<prefix>/bin/` |
| `pg_timestamp.so`（可选工具） | `<prefix>/lib/` |

## 3. 版本差异与适配点

下表是 15 → 19 实际遇到的全部内核差异，均已处理，无遗留。

| # | 内核变化 | 引入版本 | 影响本项目的位置 | 处理 |
|---|---|---|---|---|
| 1 | `populate_compact_attribute()` 新增；TupleDesc 改为预计算属性元数据 | 18 | `fl_build_tupdesc()`（worker 里构造 TupleDesc） | 只在 ≥18 调用；更早版本由访问器按需计算 |
| 2 | `RelationSetNewRelfilenode()` → `RelationSetNewRelfilenumber()` | 16 | 索引重建路径 | `pgut-be.h` 既有宏按版本改名 |
| 3 | `BackendId` → `ProcNumber`（`rd_backend`、`RelFileLocatorBackend.backend`） | 17 | 临时表/`smgr` 定位 | `FastloadBackendNumber` / `FastloadInvalidBackend` 宏 |
| 4 | `MyBackendId` 连同 `BackendId` API 一并移除 | 19 | 同上 | 同上，≥17 走 `MyProcNumber` / `INVALID_PROC_NUMBER` |
| 5 | `Datum` 从 `c.h` 移入 `postgres.h`（前端不可用） | 19 | 命令行客户端（`bin/pgut/pgut-list.h` 引入 `nodes/pg_list.h`） | 前端头里补 `typedef uint64_t Datum;` |
| 6 | `PageXLogRecPtr` 由 `{xlogid, xrecoff}` 变为单个 64 位值，且按 CPU 字节序存放 | 19 | `IsPageCreatedByLoader()`（恢复工具判断空页） | 改用 `PageXLogRecPtrGet()` |
| 7 | `get_database_name()` 从 `commands/dbcommands.h` 移到 `utils/lsyscache.h` | 19 | `fastload_parallel.c`、`writer_parallel.c` | 版本分支选择头文件 |
| 8 | `FuncnameGetCandidates()` 增加 `int *fgc_flags` 出参，且**无条件解引用** | 19 | `ParseFunction()`（`FILTER` / `TYPE=FUNCTION`） | 传入一次性 `int[1]` 复合字面量；传 `NULL` 会段错误 |
| 9 | `Item` typedef 移除；`PageAddItemExtended()` 参数改 `const void *`；`PageSetChecksumInplace()` → `PageSetChecksum()` | 19 | `DirectWriterInsert()` 与页校验和回填 | 版本分支，≥19 直接传指针 |
| 10 | nbtree 内部重构：`BTSpool`/`BTWriteState` 不再导出，`palloc0_object()` 等新分配宏，`table_beginscan_parallel()` 增加 `SO_NONE` | 19 | 索引 spool 构建器 | 新增 `lib/nbtree/nbtsort-19.c`（见 §3.1） |

### 3.1 nbtree 构建器的按版本内置

`WRITER=DIRECT` 绕过 buffer manager，索引必须由装载器自己批量构建，因此本项目**按主版本各内置一份**
PostgreSQL 的 `nbtsort.c`（这是 pg_bulkload 的既有做法，不是本次新增的取舍）：

| 文件 | 覆盖版本 |
|---|---|
| `lib/nbtree/nbtsort-9.5.c` … `nbtsort-9.6.c` | 9.5 – 9.6 |
| `lib/nbtree/nbtsort-10.c` … `nbtsort-17.c` | 10 – 17 |
| `lib/nbtree/nbtsort-18.c` | 18 |
| `lib/nbtree/nbtsort-19.c` | 19 |
| `lib/nbtree/nbtsort-common.c` | 14 以后各版本共用的 `_bt_spoolinit()` |

`lib/pg_btree.c` 用 `#if PG_VERSION_NUM` 把其中一份 `#include` 进来，编译成同一个翻译单元。
18 → 19 的差异实测只有 59 行（`const` 限定、`palloc*_object` 宏、`PageAddItem` 参数类型），
所以新增 19 版本的成本主要在校对而不是重写。

## 4. 验收方法

脚本以"目标版本 + 一次性集群"为单位执行，每个版本跑一遍后销毁集群：

| 步骤 | 内容 |
|---|---|
| 构建 | `make clean`（带 `PG_CONFIG`）→ `make` → `make install` |
| 集群 | `initdb` + `port=54<major>`、`max_worker_processes=32`、`fsync=off`（仅测试环境） |
| 数据 | 200,000 行 CSV（含引号字段）与等量 100 字节定长二进制 |
| 用例 A | CSV：`PARALLEL=OFF` / `w2` / `w4` / `w8` / `auto`，逐配置比对行数与内容摘要 |
| 用例 B | 定长：`PARALLEL=OFF` / `w4` / `w8`，同上 |
| 用例 C | `FILTER` 降级：带 `FILTER` 时串行与 `PARALLEL=ON` 内容一致，且 `LOGFILE` 出现 `PARALLEL disabled (FILTER` |
| 用例 D | 解析容错：默认 `PARSE_ERRORS=0` 停在第 1 条坏记录（保留已装载行）；`PARSE_ERRORS=2` 时两条好记录都装载 |
| 用例 E | 命令行客户端：`pg_fastload -h -p -U -d xxx.ctl` 退出码 0、行数正确 |
| 用例 F | 服务端日志无 `PANIC` / 段错误 |

内容摘要用 `count(*) || sum(hash_record_extended(t,0))`，与插入顺序无关，可直接比对串行与并行的等价性。

## 5. 验收结果（2026-11-10）

| 版本 | A: CSV 串行/并行 | B: 定长串行/并行 | C: FILTER 降级 | D: 解析容错 | E: CLI | F: 日志 | 结论 |
|---|---|---|---|---|---|---|---|
| 15.19 | 5/5 摘要一致 | 3/3 一致 | 一致 + 提示存在 | 1 / 2 行 | rc=0，200,000 行 | 无 PANIC | **PASS** |
| 16.15 | 5/5 摘要一致 | 3/3 一致 | 一致 + 提示存在 | 1 / 2 行 | rc=0，200,000 行 | 无 PANIC | **PASS** |
| 17.11 | 5/5 摘要一致 | 3/3 一致 | 一致 + 提示存在 | 1 / 2 行 | rc=0，200,000 行 | 无 PANIC | **PASS** |
| 18.6 | 见 [VERIFICATION.md](VERIFICATION.md)（90/90 + 44/44） | 同左 | 同左 | 同左 | 同左 | 无 PANIC | **PASS** |
| 19rc1 | 5/5 摘要一致 | 3/3 一致 | 一致 + 提示存在 | 1 / 2 行 | rc=0，200,000 行 | 无 PANIC | **PASS** |

摘要值（CSV / 定长）在四个版本上完全一致，说明并行路径产出的内容与串行逐字节等价。

## 6. 已知限制

- **索引并行构建（W-3）未实现**，与版本无关：带索引时索引构建仍在 leader 内串行完成，详见
  [OPTIMIZATION.md](OPTIMIZATION.md) 的未采纳方案。
- **PG 19 为预发布版本**（`19rc1`）。适配基于 PostgreSQL 19（`REL_19_STABLE`），正式发布前若 nbtree 内部再次调整，
  `lib/nbtree/nbtsort-19.c` 需要重新校对。
- 未验证的主版本（< 15）源码里保留了 `#if` 分支，但**未经本项目测试**，不作为支持承诺。
