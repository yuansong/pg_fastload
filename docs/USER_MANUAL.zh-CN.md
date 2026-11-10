# pg_fastload 用户手册

> 适用版本：`pg_fastload 1.0.0`（基于 pg_bulkload 3.1.23 二次实现）
> 对应数据库：PostgreSQL 18.x（本版本仅在 18.6 上完整验证）
> English version: [USER_MANUAL.md](USER_MANUAL.md) · 中文版：本文

---

## 目录

1. [概览](#1-概览)
2. [安装与升级](#2-安装与升级)
3. [快速上手](#3-快速上手)
4. [概念模型](#4-概念模型)
5. [控制文件语法](#5-控制文件语法)
6. [关键字参考](#6-关键字参考)
7. [SQL 接口](#7-sql-接口)
8. [命令行](#8-命令行)
9. [输入格式](#9-输入格式)
10. [并行模式](#10-并行模式)
11. [错误处理与 badfile](#11-错误处理与-badfile)
12. [日志与诊断](#12-日志与诊断)
13. [性能调优](#13-性能调优)
14. [故障排查](#14-故障排查)
15. [已知限制](#15-已知限制)
16. [从 pg_bulkload 迁移](#16-从-pg_bulkload-迁移)
17. [升级与卸载](#17-升级与卸载)
18. [导出表数据（`pg_fastload_export`）](#18-导出表数据pg_fastload_export)

---

## 1. 概览

`pg_fastload` 是 PostgreSQL 的高速批量加载工具。它把一次加载拆成四个阶段：

| 阶段 | 内容 | 串行路径 | 并行路径（`PARALLEL=ON`） |
|---|---|---|---|
| ① 读输入 | 从文件/STDIN 取字节 | leader 进程 | 1 个 reader bgworker |
| ② 解析记录 | 记录边界 + 字段切分 | leader 进程 | N 个 worker |
| ③ 形成元组 | 输入函数 → `heap_form_tuple()` | leader 进程 | N 个 worker |
| ④ 写入 | `WriterInsert()` | leader 进程 | leader 进程（不变） |

**没有 `PARALLEL` 时，行为与 pg_bulkload 完全一致** —— 同一个记录循环、同一套选项、同一套错误处理。
但**速度不再相同**：无引号的记录不再逐字节走状态机（见
[ARCHITECTURE §6.1a](ARCHITECTURE.zh-CN.md#61a-字段切分两档实现同一语义)），因此在 1 GiB 纯 CSV 上
串行路径约为**上游的 1.2×**、**`COPY ... FROM` 的 1.4×**
（[BENCHMARK §3.1](BENCHMARK.zh-CN.md#31-串行路径对上游对-copy--from)），输出仍是逐位一致。
打开 `PARALLEL=ON` 后只有 ①②③ 被并行化，④ 以及所有事务/约束/错误语义都留在原处。
因此 `PARALLEL=ON` 是一个"可以随时打开"的开关：不满足条件的场景会自动回到串行路径并打印一条 INFO。

反方向也有一个入口：`pgfastload.pg_fastload_export()` 把表或任意 `SELECT` 的结果写成
CSV / BINARY / FIXED 文件，用的是同一套 Writer 与同一套 `COL`/`OUT_COL` 布局描述，
所以导出的定长文件能被 `pg_fastload()` 原样装回去（见 [§18](#18-导出表数据pg_fastload_export)）。

### 适用场景

- 大批量初始化装载（数十万行以上），CPU 是多核且 ①②③ 是瓶颈；
- 输入文件在本地磁盘或 page cache 中，且格式为 CSV 或定长；
- 目标表可以接受 `AccessExclusiveLock`（装载期间独占）。

### 不适用场景

- 需要逐行容错（坏行写 badfile 继续装）：请用 `PARALLEL=OFF`；
- 输入来自 `STDIN` 或管道：`PARALLEL=ON` 会自动降级；
- `TYPE=TUPLE / FUNCTION`：尚未并行化；
- 单核或 2 核机器：并行收益接近 0，建议 `PARALLEL=OFF`。

---

## 2. 安装与升级

### 2.1 依赖

| 项 | 要求 |
|---|---|
| PostgreSQL | 服务端开发包（`postgresql-server-dev-18` 或源码树），`pg_config` 可执行 |
| 编译器 | GCC / Clang，C99 |
| 其他 | `libpq`（构建期用于 `WRITER=PARALLEL` 的第二个连接）、`pthread` |
| GUC | `max_worker_processes ≥ PARALLEL_WORKERS + 1`（默认 15 worker 时建议 ≥ 32） |

`shared_preload_libraries` **不需要**配置。

### 2.2 构建与安装（PGXS）

```bash
tar xzf pg_fastload-1.0.0.tar.gz
cd pg_fastload
make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config
sudo make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config install
```

安装结果：

| 文件 | 位置 |
|---|---|
| `pg_fastload.so` | `$(pg_config --pkglibdir)/` |
| `pg_fastload.control`、`pg_fastload--1.0.0.sql` | `$(pg_config --sharedir)/extension/` |
| `pg_fastload`（命令行） | `$(pg_config --bindir)/` |
| `pg_timestamp`（可选工具） | `$(pg_config --bindir)/` |

> `lib/pg_fastload.control` 与 `lib/pg_fastload--*.sql` 是构建产物，由 `.in` 模板经 Makefile
> 生成，仓库里不存在这两个文件是正常的。

### 2.3 创建扩展

```sql
CREATE EXTENSION pg_fastload;
```

扩展会创建 schema `pgfastload` 与函数 `pgfastload.pg_fastload(text[])`。
`pg_fastload` 以超级用户身份执行装载，普通用户需要被显式授权使用该 schema。

### 2.4 目录与权限

- 装载进程以**数据库服务端身份**读取 `INPUT` 文件，客户端用户对文件路径的权限无关；
  文件必须对 `postgres` 系统用户可读。
- 默认日志与状态文件写在 `$PGDATA/pg_fastload/`（由服务端当前工作目录决定）。
- `DIRECT` 模式会直接写数据文件并维护
  `$PGDATA/pg_fastload/{dboid}.{relid}.loadstatus`，用于崩溃后的恢复。

---

## 3. 快速上手

### 3.1 控制文件与数据文件放在哪台机器

这是从 pg_bulkload 迁移过来最容易踩的坑，先给结论：

| 用法 | 控制文件 | 数据文件（`INPUT`） | 原因 |
|---|---|---|---|
| **SQL 接口**<br>`SELECT pgfastload.pg_fastload(ARRAY[...])` | **不需要控制文件** | **数据库服务器** | 关键字内联在数组里；文件由服务端 backend / reader 进程打开 |
| **命令行**<br>`pg_fastload xxx.ctl` | **客户端机器**<br>（敲命令的那台） | **数据库服务器** | CLI 只负责读控制文件，真正的装载在服务端执行 |

三条推论：

1. **用 SQL 接口时不存在控制文件。** 把 `.ctl` 文件放在客户端不会有任何效果，
   服务端既看不到它也不会去读它。
2. **`INPUT` 一定是服务器侧路径**，并且要能被数据库的 OS 用户（通常是 `postgres`）读取。
   客户端本地路径（`C:\data\a.csv`、`/home/me/a.csv`）会报 `could not open file`。
3. **控制文件里的相对路径由客户端展开成绝对路径后再发给服务端。** 所以当控制文件在客户端、
   数据文件在服务器时，请直接写服务器上的绝对路径，不要写相对路径。

> 记法：**控制文件跟着客户端走，数据文件跟着数据库服务器走，SQL 接口没有控制文件。**

### 3.2 控制文件方式

`order_line.ctl`：

```
OUTPUT            = public.order_line
INPUT             = /tmp/tpcc/order_line.csv
TYPE              = CSV
WRITER            = DIRECT
DELIMITER         = ','
TRUNCATE          = YES
PARALLEL          = ON
```

```bash
pg_fastload -d tpcc order_line.ctl
```

输出到终端的是本次装载的统计摘要；同样的内容也会写入日志文件。

### 3.3 SQL 方式

```sql
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/tmp/tpcc/order_line.csv',
  'OUTPUT=public.order_line',
  'TYPE=CSV',
  'WRITER=DIRECT',
  'TRUNCATE=YES',
  'PARALLEL=ON'
]);
```

返回一行：

| 列 | 含义 |
|---|---|
| `skip` | 被 `SKIP` 跳过的记录数 |
| `count` | 成功装载的记录数 |
| `parse_errors` | 解析错误数（仅串行路径可能 > 0） |
| `duplicate_new` | 重复键中新值被保留的行数 |
| `duplicate_old` | 重复键中旧值被保留的行数 |
| `system_time` / `user_time` | 服务端 CPU 时间（秒） |
| `duration` | 服务端耗时（秒） |

### 3.4 最简并行示例

```
OUTPUT   = public.t
INPUT    = /data/t.csv
PARALLEL = ON
```

`PARALLEL_WORKERS` 省略时使用「在线 CPU 核数 − 1」。查看实际生效值：

```bash
psql -c "SELECT * FROM pgfastload.pg_fastload(ARRAY['INPUT=/data/t.csv',
         'OUTPUT=public.t','PARALLEL=ON','VERBOSE=YES'])" 2>&1 | grep PARALLEL
```

日志里会出现 `PARALLEL=ON, TYPE=CSV, workers=N` 或 `PARALLEL disabled (...)`。

---

## 4. 概念模型

### 4.1 三个可组合的维度

| 维度 | 关键字 | 取值 | 说明 |
|---|---|---|---|
| 输入格式 | `TYPE` | `CSV` / `BINARY` / `FIXED` / `TUPLE` / `FUNCTION` | `FIXED` 是 `BINARY` 的别名，走同一个定长解析器 |
| 写入方式 | `WRITER` | `DIRECT` / `BUFFERED` / `PARALLEL` | `PARALLEL` 等价于 `DIRECT` + `MULTI_PROCESS=YES` |
| 执行模式 | `PARALLEL` | `ON` / `OFF` | 仅影响 ①②③ 是否多进程 |

三者互不干扰：例如 `TYPE=CSV` + `WRITER=DIRECT` + `PARALLEL=ON` 是「并行解析 + 直接写堆表」。

### 4.2 `WRITER` 三种模式

| 模式 | 写入路径 | 事务 | 适用 |
|---|---|---|---|
| `DIRECT`（默认） | 绕过 shared buffers，直接写数据文件 + 维护 LSF，装载后统一建索引 | 调用方事务 | 大批量初始化装载，最快 |
| `BUFFERED` | 走正常 buffer manager + WAL | 调用方事务 | 需要事务可见性/崩溃安全时的保守选择，比 `DIRECT` 慢 |
| `PARALLEL` | `DIRECT` + 第二个 backend（独立 libpq 连接 + SysV IPC 队列）执行插入 | **独立事务** | 上游的并行写入方案；不并行化插入本身，且破坏调用方 `BEGIN/COMMIT` 原子性，一般不建议使用 |

`WRITER=PARALLEL` 与 `PARALLEL=ON` **不会叠加**：同时指定时 reader 回退串行并打印 INFO
（避免两个进程扇出叠在一起）。

### 4.3 并行执行模型

```
leader（发起加载的 backend）
  ├─ 解析参数 / 创建 DSM / 注册 bgworker
  ├─ ④ WriterInsert()（串行，逐条）
  └─ 汇总统计、清理
reader bgworker（1 个，无数据库连接）
  └─ 单遍扫描输入 → SKIP/LIMIT 裁剪 → 分块轮转分发
worker bgworker × N
  └─ 切字段 → 输入函数 → heap_form_tuple → 回传
```

详细协议与设计取舍见 [ARCHITECTURE.md](ARCHITECTURE.md)。

---

## 5. 控制文件语法

```
# 注释：以 # 开头，整行忽略
KEY = VALUE
KEY = 'VALUE WITH SPACES'
```

规则：

| 项 | 说明 |
|---|---|
| 分隔符 | `=`，两侧空格任意 |
| 大小写 | 关键字不区分大小写；值一般也不区分（路径除外） |
| 注释 | `#` 起至行尾（值的引号内部除外） |
| 引号 | 值含空格或 `#` 时用单引号包裹；引号内不转义 |
| 未知关键字 | 报错 `invalid keyword "XXX"`（不做静默忽略） |
| 重复关键字 | 报错（`ASSERT_ONCE`） |
| 空值 | 报错 `option "XXX" has no value` |

等价写法：所有关键字都能用 `-o KEY=VALUE` 在命令行追加（见 [§8](#8-命令行)），
也可以直接作为 SQL 函数的数组元素传入。

**路径解析**：控制文件由**客户端**读取。其中的相对路径（`INPUT`、`OUTPUT`、`LOGFILE`、
`PARSE_BADFILE`、`DUPLICATE_BADFILE`）会以控制文件所在目录为基准展开成绝对路径，然后随
`pg_fastload()` 调用发到**服务端**执行。因此相对路径只有在客户端与服务器目录结构一致时才安全：
跨机器使用时请一律写服务器上的绝对路径（见 [§3.1](#31-控制文件与数据文件放在哪台机器)）。

---

## 6. 关键字参考

### 6.1 通用

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `TYPE` | `CSV` / `BINARY` / `FIXED` / `TUPLE` / `FUNCTION` | `CSV` | 输入格式 |
| `WRITER` / `LOADER` | `DIRECT` / `BUFFERED` / `PARALLEL` | `DIRECT` | 写入方式 |
| `MULTI_PROCESS` | `YES` / `NO` | `NO` | 等价于 `WRITER=PARALLEL` 的开关式写法 |

### 6.2 输入

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `INPUT` / `INFILE` | 路径或 `STDIN` | 无（必填） | 输入文件。仅 `STDIN` 时按客户端编码处理 |
| `SKIP` / `OFFSET` | 整数 ≥ 0 | `0` | 跳过前 N 个**输入行**（`\n`、`\r\n` 或单个 `\r` 都算行尾；不识别引号，因此跨行的带引号记录会算作多行 —— 与 pg_bulkload 规则一致） |
| `LOAD` / `LIMIT` | 整数 | 不限 | 最多装载 N 条记录（`SKIP` 之后计数） |
| `ENCODING` | 编码名 | 数据库编码 | 输入文件编码；需要逐字段转码 |

### 6.3 输出与事务

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `OUTPUT` / `TABLE` | `schema.table` | 无（必填） | 目标表；只写表名，**不写库名**（库由连接决定） |
| `TRUNCATE` | `YES` / `NO` | `NO` | 装载前清空目标表 |
| `VERBOSE` | `YES` / `NO` | `NO` | 输出更详细的进度日志 |

### 6.4 CSV 解析（`TYPE=CSV`）

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `DELIMITER` | 单字符 | `,` | 字段分隔符 |
| `QUOTE` | 单字符 | `"` | 引号字符 |
| `ESCAPE` | 单字符 | 同 `QUOTE` | 转义字符。与 `QUOTE` 相同即 RFC4180 的 `""` 写法 |
| `NULL` | 字符串 | 空串 | 表示 SQL NULL 的字段文本（比较的是**去引号前**的原文） |
| `FORCE_NOT_NULL` | 列名列表（逗号分隔） | 无 | 指定列即使文本等于 `NULL` 串也按普通值处理 |
| `FILTER` | 函数名 | 无 | 逐字段改写函数（`func(text,text)` 形式），会显著降低吞吐 |
| `SKIP` / `OFFSET` | 整数 | `0` | 同上（CSV 解析器级别） |

CSV 语义细节见 [§9.1](#91-csv)。

### 6.5 定长解析（`TYPE=BINARY` / `FIXED`）

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `COL` | `name type [NULLIF 'pattern']` | 无（必填） | 一列一个 `COL`；`type` 见下表 |
| `STRIDE` | 整数 > 0 | 字段长度之和 | 每条记录的字节长度（含填充） |
| `PRESERVE_BLANKS` | `YES` / `NO` | `NO` | `CHAR`/`VARCHAR` 是否保留尾部空格 |
| `FILTER` | 函数名 | 无 | 同 CSV |

`COL` 支持的 `type`：

| 类别 | 名称 | 长度 |
|---|---|---|
| 文本 | `CHAR` / `CHARACTER`、`VARCHAR` / `CHARACTER VARYING` | 由 `NULLIF` 定界或到字段尾 |
| 有符号整数 | `SMALLINT`(`SHORT`)、`INTEGER`(`INT`)、`BIGINT`(`LONG`) | 2 / 4 / 8 字节原生字节序 |
| 无符号整数 | `UNSIGNED SMALLINT`(`UNSIGNED SHORT`)、`UNSIGNED INTEGER`(`UNSIGNED INT`) | 2 / 4 字节 |
| 浮点 | `FLOAT`(`REAL`)、`DOUBLE` | 4 / 8 字节 |

数值字段按**原生二进制字节**解码（C cast），不做字符串解析——这是定长模式比 CSV 快数倍的原因。

### 6.6 错误处理

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `PARSE_ERRORS` / `MAX_ERR_CNT` | 整数，`-1` = 不限 | `0` | 允许的解析错误数；**超过即停止读取**（已装载的行保留，本次调用正常返回，不报错） |
| `PARSE_BADFILE` | 路径 | `{log-basename}.prs.{ext}` | 解析失败记录的落盘文件 |
| `DUPLICATE_ERRORS` | 整数，`-1` = 不限 | `0` | 允许的重复键错误数 |
| `DUPLICATE_BADFILE` | 路径 | `{log-basename}.dup.csv` | 重复键冲突记录的落盘文件 |
| `ON_DUPLICATE_KEEP` | `NEW` / `OLD` | `NEW` | 重复时保留新值还是旧值 |
| `CHECK_CONSTRAINTS` | `YES` / `NO` | `NO` | 是否校验目标表的 CHECK 约束 |

### 6.7 日志

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `LOGFILE` | 路径 | `$PGDATA/pg_fastload/YYYYMMDDHHMMSS_{db}_{nsp}_{tbl}.log` | 装载日志 |

### 6.8 并行（pg_fastload 新增）

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `PARALLEL` | `ON` / `OFF` | `OFF` | 打开多进程并行加载 |
| `PARALLEL_WORKERS` | `1`..`64` | 在线 CPU 核数 − 1（≥1） | 解析 worker 数；`≤1` 直接串行落回 |

### 6.9 加载进度（pg_fastload 新增）

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `PROGRESS` | `ON` / `OFF` | `ON` | 为本次装载发布一条状态记录，任何会话都可通过 `pgfastload.pg_fastload_progress` 读取 |
| `PROGRESS_INTERVAL` | `100`..`3600000`（毫秒） | `1000` | 两条状态记录之间的最小间隔；**指定该参数等价于 `PROGRESS=ON`** |
| `PROGRESS_NOTICE` | `ON` / `OFF` | `OFF` | 额外向装载客户端（及 `LOGFILE`）输出 `NOTICE` 进度行 |

视图、列含义与开销见 [§12.4](#124-加载进度)。

---

## 7. SQL 接口

```sql
CREATE EXTENSION pg_fastload;

SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/t.csv',
  'OUTPUT=public.t',
  'TYPE=CSV',
  'PARALLEL=ON'
]);
```

要点：

- 参数是 `text[]`，每项一个 `KEY=VALUE`，与控制文件关键字完全一致；
- 函数为 `VOLATILE STRICT`，在**调用方事务内**执行；`COMMIT` 之前数据对外不可见；
- 装载失败抛 `ERROR`，整个事务回滚（`DIRECT` 路径会留下 LSF 记录，见 [§14](#14-故障排查)）；
- 服务端以函数所有者（通常是超级用户）身份读文件，因此文件只需对 `postgres` 可读。

在事务中组合多个装载：

```sql
BEGIN;
SELECT pgfastload.pg_fastload(ARRAY['INPUT=/data/a.csv','OUTPUT=public.t','PARALLEL=ON']);
SELECT pgfastload.pg_fastload(ARRAY['INPUT=/data/b.csv','OUTPUT=public.t','PARALLEL=ON']);
COMMIT;
```

反向的导出是另一个函数，签名与调用方式相同，但它读写文件而不是表：

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.t',
  'OUTPUT=/data/t.csv',
  'TYPE=CSV'
]);
-- 返回 bigint：写出的行数
```

注意 `TABLE` 在这里是**源**（装载里它是目标），`OUTPUT` 在这里是**文件**（装载里它是表名）。
完整关键字、格式细节与往返语义见 [§18](#18-导出表数据pg_fastload_export)。

---

## 8. 命令行

```
pg_fastload [选项] [控制文件]
```

不指定控制文件时，所有参数必须由 `-o` 提供。

| 选项 | 长选项 | 说明 |
|---|---|---|
| `-i` | `--infile` / `--input` | 等价于 `INPUT=`，覆盖控制文件中的值 |
| `-O` | `--output` | 等价于 `OUTPUT=` |
| `-l` | `--logfile` | 等价于 `LOGFILE=` |
| `-P` | `--parse-badfile` | 等价于 `PARSE_BADFILE=` |
| `-u` | `--duplicate-badfile` | 等价于 `DUPLICATE_BADFILE=` |
| `-o` | `--option` | 追加任意 `KEY=VALUE`，可重复，优先级最高 |
| `-D` | `--pgdata` | 指定 `$PGDATA`（仅恢复模式用） |
| `-r` | `--recovery` | 恢复模式：依据 LSF 处理中断的 `DIRECT` 装载 |
| `-d` / `-U` / `-h` / `-p` | | 数据库连接参数，缺省走 `PGDATABASE` / `PGUSER` / `PGHOST` / `PGPORT` |
| `-e` / `-E` | | 回显执行的 SQL / 设置错误级别（继承自 pgut 公共选项） |
| `--help` | | 显示帮助 |

示例：

```bash
# 全部参数写在命令行
pg_fastload -d tpcc \
  -o INPUT=/data/t.csv \
  -o OUTPUT=public.t \
  -o PARALLEL=ON -o PARALLEL_WORKERS=8

# 控制文件 + 临时覆盖
pg_fastload -d tpcc base.ctl -o SKIP=1000000 -o LIMIT=100000
```

**退出码**：`0` 成功；非 `0` 表示失败（可用 `$?` 判断）。

---

## 9. 输入格式

### 9.1 CSV

| 特性 | 行为 |
|---|---|
| 记录终止 | `\n`、`\r\n`、单独 `\r`（与串行一致） |
| 引号内换行 | 属于字段内容，不结束记录 |
| 转义 | `ESCAPE` 默认等于 `QUOTE`，即 `""` 表示一个字面引号；`ESCAPE ≠ QUOTE` 时按 `ESCAPE` + 字符处理 |
| 空字段 | 文本 `''`（两个分隔符相邻）→ `NULL`（若 `NULL` 为默认空串） |
| 引号空串 | `""` 原文长度为 2，**不等于**空 NULL 串 → 装载为空字符串 |
| 末行无换行 | 最后一条记录照常产出 |
| 字段数不符 | 报错（串行：计数为解析错误，可写 badfile；并行：装载失败） |
| 单条记录上限 | 256 MiB |

### 9.2 定长（`BINARY` / `FIXED`）

```
TYPE   = FIXED
STRIDE = 100
COL    = id      INTEGER
COL    = name    VARCHAR NULLIF ','
COL    = amount  DOUBLE
```

- 记录长度固定为 `STRIDE`；字段按 `COL` 声明顺序依次排布；
- `CHAR`/`VARCHAR` 需要 `NULLIF` 定界（或延伸到字段尾/记录尾）；
- 文件末尾不足一条记录的残字节：忽略并 WARNING（与串行一致）；
- 并行路径要求记录完整（不支持跨记录的残字节续接）。

### 9.3 `TUPLE`

`TYPE=TUPLE` 读取 **pg_fastload 自己产生的二进制元组流**（由 `WRITER=BINARY` 导出），
用于 `pg_fastload` 内部的快速搬运/恢复，不适合作为通用导入格式。

### 9.4 `FUNCTION`

`TYPE=FUNCTION` 通过 SQL 函数逐行产生元组，适合从任意查询结果装载；不参与并行。

---

## 10. 并行模式

### 10.1 开启

```
PARALLEL         = ON
PARALLEL_WORKERS = 8      # 可省略
```

### 10.2 worker 数怎么选

| 场景 | 建议 |
|---|---|
| 默认 | 省略 `PARALLEL_WORKERS`（= CPU − 1） |
| 文件远大于内存（冷缓存） | 先用默认值；若发现抖动，降到 4~8 |
| 目标表带多个索引 | 提高 worker 仍有收益（索引构建在 leader 串行做） |
| 单核/2 核 | `PARALLEL=OFF` |
| 与其他负载共机 | 显式指定，留出余量 |

### 10.3 资源占用

| 资源 | 默认 15 worker 时 |
|---|---|
| bgworker 槽位 | 16（15 解析 + 1 reader）→ `max_worker_processes ≥ 16`，建议 32 |
| DSM 共享内存 | ≈ 144 MiB（输入队列 8 MiB × 15 + 回传队列 2 MiB × 15 + 控制块） |
| 进程私有内存 | 每 worker 一个 `AllocSet`，随记录大小波动，单条上限 256 MiB |

### 10.4 自动降级（不会失败，只会变慢）

`PARALLEL=ON` 遇到以下情形会回退串行并打印一条 INFO：

| # | 条件 | INFO 内容形态 |
|---|---|---|
| 1 | 有效 worker ≤ 1 | `PARALLEL disabled (workers=1)` |
| 2 | `TYPE` 为 `TUPLE` / `FUNCTION` | `PARALLEL disabled (TYPE=...)` |
| 3 | 指定 `FILTER` / `FORCE_NOT_NULL` | `PARALLEL disabled (FILTER)` 等 |
| 4 | 目标表有 dropped 列 | `PARALLEL disabled (dropped columns)` |
| 5 | 显式 `ENCODING` 需转码 | `PARALLEL disabled (encoding conversion)` |
| 6 | 表有 `NOT NULL` 列或 `CHECK_CONSTRAINTS` | `PARALLEL disabled (constraints)` |
| 7 | `PARSE_ERRORS != 0` | `PARALLEL disabled (PARSE_ERRORS)` |
| 8 | `INPUT=STDIN` / 缺失 | `PARALLEL disabled (INPUT is not a file)` |
| 9 | `WRITER=PARALLEL` / `MULTI_PROCESS=YES` | `PARALLEL disabled (MULTI_PROCESS / WRITER=PARALLEL ...)` |
| 10 | 定长：无 `COL` / `STRIDE ≤ 0` | `PARALLEL disabled (fixed-length needs COL/STRIDE)` |
| 11 | 定长：`NULLIF` 模式 > 64 字节 | `PARALLEL disabled (NULLIF longer than 64 bytes)` |
| 12 | 定长：EOF 尾残字节 | 装载继续，尾残忽略（WARNING） |
| 13 | 无法获取目标表描述符 | `PARALLEL disabled (no TupleDesc)` |

> 排查"为什么没并行"：打开 `VERBOSE=YES`，在日志里搜索 `PARALLEL`。

### 10.5 正确性契约

并行路径**不保证物理行序**（heap 表本身也不保证）。判定正确性的方式是内容集合：

```sql
SELECT count(*) || '|' || sum(hash_record_extended(t, 0)) FROM public.t AS t;
```

串行与并行的该摘要必须一致。验证套件以此为断言（见 [VERIFICATION.md](VERIFICATION.md)）。

---

## 11. 错误处理与 badfile

| 场景 | 串行路径（`PARALLEL=OFF`） | 并行路径（`PARALLEL=ON`） |
|---|---|---|
| 字段数不符 / 输入函数报错 / 未闭合引号 | 计入 `parse_errors`，写 `PARSE_BADFILE` 后继续；超过 `PARSE_ERRORS` 时**停止读取并正常返回**（日志 `Maximum parse error count exceeded`），已装载行保留 | 先输出与串行相同的 `Parse error Record N: Input Record M ...` WARNING，随后**语句以 `ERROR` 失败并整体回滚**（不保留部分行） |
| 重复键 | `DUPLICATE_ERRORS` 上限内写入 `DUPLICATE_BADFILE`，按 `ON_DUPLICATE_KEEP` 取舍 | 由 leader 串行写入，行为与串行一致 |
| 约束违反 | 报错回滚 | 报错回滚 |

因此：

- **需要容错装载就用 `PARALLEL=OFF`**；
- 并行路径适合"数据已清洗、要求快"的场景。它对被拒绝的记录采取**全有或全无**：出错记录之后的那些行此时已在其他 worker 的队列里，若沿用"保留错误前已读行"的语义，结果会依赖 worker 时序，可能装载出 pg_bulkload 永远不会装载的不确定超集；
- `PARSE_BADFILE` / `DUPLICATE_BADFILE` 未指定时按 `LOGFILE` 基名派生，位于同一目录。

---

## 12. 日志与诊断

### 12.1 装载日志

```
pg_fastload 1.0.0
...
PARALLEL = ON
PARALLEL_WORKERS = 15
TYPE = CSV
...
worker #0 produced 492013 rows
...
CPU 1.07s/11.60u sec elapsed 1.37 sec
```

| 关注点 | 含义 |
|---|---|
| `PARALLEL=ON, TYPE=CSV, workers=N` | 并行确实启动 |
| `PARALLEL disabled (...)` | 已降级，括号内是原因 |
| `worker #i produced N rows` | 各 worker 产出（分块轮转，不保证均分） |
| `CPU x/y sec elapsed z sec` | 服务端 CPU / 墙钟 |

### 12.2 阶段剖析（`USE_PROFILE=1` 构建）

以 `make USE_PGXS=1 USE_PROFILE=1` 构建后，每次装载会额外输出：

| 组 | 段 | 含义 |
|---|---|---|
| `GLOBAL` | `INIT` / `READER` / `WRITER` / `FLUSH` / `MERGE` / `INDEX` / `FINISH` / `PARALLEL` | 全流程分段；`PARALLEL` 是并行子系统总耗时 |
| `READER` | `SOURCE` / `PARSER` | 串行路径的读/解析 |
| `CSV` | `SPLIT` / `VALUE` | 仅串行 CSV：字节 → 字段指针（找记录边界、判引号/回车、就地切分隔符），再 字段指针 → `Datum` + `heap_form_tuple()` |
| `WRITER` | `TOAST` / `TABLE` / `INDEX` | 写入细分 |
| `PARALLEL` | `SETUP` / `WAIT` / `COPY` / `WRITE` / `READ` | 编排、等 worker、回传拷贝、写表、reader 扫描 |
| `WORKER` | `SCAN` / `FORM` / `IPC` | worker 侧取记录 / 成型 / 通信（跨进程汇总） |

`GLOBAL` 各段之和应约等于 `elapsed`；若明显小于，说明有未埋点路径（提 issue）。

> 剖析构建有额外开销，**不要**用于生产；生产构建默认不带任何埋点。

### 12.3 进程检查

```sql
SELECT pid, backend_type, state FROM pg_stat_activity WHERE backend_type LIKE '%fastload%';
```

并行装载期间可见 1 个 reader + N 个 worker（pg 18 中动态 bgworker 会出现在 `pg_stat_activity`）。

### 12.4 加载进度

`pg_fastload()` 是一次 SQL 调用，不加处理的话直到返回都不打印任何东西 —— 100 GB 数据跑半小时
会让人心里没底。让装载会话自己"打印"进度也没用：忙的正是那个会话，它的客户端被一条还没结束的
语句阻塞着。所以进度必须**发布**出来，从别的地方读取：

```sql
-- 会话 1：接下来半小时都很忙
SELECT * FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/lineitem.csv',
  'OUTPUT=public.lineitem',
  'PARALLEL=ON'
]);

-- 会话 2，或监控采集程序，或同实例的另一个数据库
SELECT * FROM pgfastload.pg_fastload_progress;
```

```
 pid  | datname |    relname      | input_type |  mode    | state   | elapsed_seconds |  rows   | bytes_read | total_bytes
------+---------+-----------------+------------+----------+---------+-----------------+---------+------------+-------------
 4121 | tpcc    | public.lineitem | CSV        | parallel | running |            3.42 | 3687521 |  536870912 | 1073741824
```

| 列 | 含义 |
|---|---|
| `pid` | 正在装载的后端进程号 |
| `backend_start`、`started` | 会话启动时间 / 装载开始时间 |
| `datname`、`relname`、`infile` | 数据库、目标表、`INPUT` 指定的输入文件 |
| `direction` | `load` 或 `export`：同一条记录也可能来自 `pg_fastload_export()`（见 [§18](#18-导出表数据pg_fastload_export)） |
| `input_type`、`mode` | 解析出的 `TYPE`（`CSV`、`BINARY`、`FIXED` 等）与 `serial`／`parallel` |
| `state` | 后端存活时为 `running`；被 kill 的后端留下的记录为 `stale` |
| `elapsed_seconds` | 自 `started` 起经过的秒数 |
| `rows` | leader 已插入的行数，即语句最终会报告的数字 |
| `bytes_read`、`total_bytes` | reader 已消费字节数 / 输入文件大小（未知时为 `0`：STDIN、管道）。导出时 `bytes_read` 计**已写出**字节，`total_bytes` 恒为 `0` |
| `total_rows` | `LIMIT`，未限行时为 `0` |
| `percent` | 有 `LIMIT` 时 `rows ÷ total_rows`，否则 `bytes_read ÷ total_bytes`，都无则 `NULL` |
| `bytes_per_second`、`rows_per_second` | 自装载开始至今的平均速率 |
| `eta_seconds` | 剩余量 ÷ 速率，百分比未知时为 `NULL` |
| `interval_ms` | 发布间隔 |

注意事项：

- **每个装载默认都发布进度。** 每个运行中的装载对应一个小文件
  `$PGDATA/pg_fastload/progress/<pid>.status`，最多每 `PROGRESS_INTERVAL` 毫秒原子替换一次
  （`.tmp` + rename）。视图只是枚举这个目录、并删掉后端已消失的记录，因此它是集群级的：
  在 `tpcc` 里发起的装载，从 `postgres` 或同实例任何其它数据库都看得到。
- **装载会话不打印任何东西**，输出保持干净。若还想要原来的客户端进度流（或想在 `LOGFILE`
  里留一份，便于事后查看），加上 `PROGRESS_NOTICE=ON`：

  ```
  NOTICE:  pg_fastload progress: start, input 1024.0 MiB, interval 1000 ms
  NOTICE:  pg_fastload progress: 50.0% | rows 3,687,521 | 512.0 MiB / 1024.0 MiB | 136.4 MiB/s | 982.0k rows/s | elapsed 00:00:03 | ETA 00:00:03
  NOTICE:  pg_fastload progress: 100.0% | rows 7,375,042 | 1024.0 MiB / 1024.0 MiB | 137.1 MiB/s | 987.4k rows/s | elapsed 00:00:07 | done
  ```

  `NOTICE` 受 `client_min_messages` 控制，设成 `warning` 及以上就看不到；视图与 `LOGFILE` 不受影响。
- **`PROGRESS=OFF` 完全不发布**：视图对该装载始终为空，`$PGDATA/pg_fastload/progress/` 下也不写
  任何文件。`PROGRESS_INTERVAL` 只调整已发布记录的节奏；单独指定它本身就表示要看进度，因此会
  顺带打开发布。
- **百分比单调不减。** 装载结束前最多停在 `99.9%`：reader 可能已读到文件末尾而 leader 还在插行，
  中途出现 `100.0%` 只会让人误以为卡住。若更关心行数而不是字节数，用 `LIMIT`，它会把百分比
  切成 `rows ÷ LIMIT` —— 这正是调用方真正想要的进度。
- **结束与失败都可见。** 装载完成会删掉自己的记录，视图回到 `0 行`；因被拒记录而中止、或被
  cancel 的装载同样如此。被 `SIGKILL` 杀死的后端无法清理，它的记录会以 `state = 'stale'` 被
  报告一次，随即由下一个读取者（或下一次装载启动时的清理）删除。
- **开销**：热路径每插入一行一次整数比较；每 512 行取一次时钟，文件最多每个间隔写一次。
  3M 行串行装载上的实测开销低于跑动噪声（±2%）；1 KiB 状态文件、无 `fsync`，它不是数据。
- **权限**：这些行会暴露数据库名、表名与输入文件路径，因此
  `pgfastload.fl_progress_rows()` 的 `EXECUTE` 已从 `PUBLIC` 收回（视图以属主身份执行）。需要
  给监控角色授权时，授权视图而不是放开函数：

  ```sql
  GRANT SELECT ON pgfastload.pg_fastload_progress TO monitor;
  ```
- 因被拒记录而中止的装载（见 [§11](#11-错误处理与-badfile)）不会报告收尾行，因为它并没有"完成"。

---

## 13. 性能调优

1. **先确认瓶颈在 ①②③**。用 `USE_PROFILE=1` 看 `READER.PARSER`（串行路径）或
   `PARALLEL{READ,WAIT}`。若 `WRITE` 已占大头，加 worker 收益有限（④ 串行下限 ~1.3 s/1 GiB）。
2. **无索引装载更快**：`DIRECT` 会把索引构建放到装载后的批量 spool（比"装载后 `CREATE INDEX`"快约 8×）。
   大量导入建议先删除非必需索引。
3. **`maintenance_work_mem` 调大**（如 1 GB）：影响 spool 排序与索引构建。
4. **worker 数**：从默认值开始；热缓存场景 8~15 已接近饱和，继续加收益递减。
5. **`TRUNCATE` 与装载分两个会话**（尤其配合 `MULTI_PROCESS=YES` 时），避免自锁。
6. **批量而非频繁小装载**：每次装载都有固定开销（连接、DSM、worker 启动 ≈ 数十毫秒）。
7. **冷缓存**：`pg_fastload` 的文件读取量恒为 1×，但宿主 I/O 抖动会放大阻塞调用延迟；
   若观察到分钟级卡顿而 CPU 空闲，属环境问题（VM/存储），不是扩展缺陷。

---

## 14. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `PARALLEL mode needs max_worker_processes to cover workers (N) + 1 reader` | worker 槽位不足 | 提高 `max_worker_processes`（≥ N+1）并重启 |
| 并行没生效，日志有 `PARALLEL disabled` | 命中降级清单 | 按括号内原因处理（见 [§10.4](#104-自动降级不会失败只会变慢)） |
| 装载卡住、`pg_stat_activity` 显示 `wait_event = relation` 或锁等待 | `MULTI_PROCESS=YES` 与同会话 `TRUNCATE` 自锁 | `TRUNCATE` 放到另一个会话；或改用 `TRUNCATE=YES` 让扩展内部处理 |
| `cannot load to ...` | 目标对象不是普通表（视图/外部表/分区父表） | 改为普通表或具体分区 |
| `pg_fastload only supports tables with "heap" access method` | 目标表用了非 heap AM | 更换表类型 |
| `permission denied for table ...` | 缺少 `INSERT`（或 `DUPLICATE_ERRORS` 时的 `DELETE`）权限 | 授权 |
| `invalid keyword "XXX"` | 关键字拼写错误 | 对照 [§6](#6-关键字参考) |
| `INPUT option required` / `OUTPUT option required` | 缺必填项 | 补全 |
| `unterminated CSV quoted field` | 引号未闭合 | 修数据；或 `PARALLEL=OFF` 观察逐行错误 |
| `invalid TYPE "XXX"` | 类型名不支持 | 用 `CSV`/`BINARY`/`FIXED`/`TUPLE`/`FUNCTION` |
| 装载后表为空但无报错 | `LIMIT=0` / `SKIP` 超过行数 / 目标表被并发清空 | 检查参数与并发 |
| `DIRECT` 装载异常中断后表不可用 | 数据文件已写但未提交，LSF 残留 | `pg_fastload -r -D $PGDATA` 恢复（按 LSF 记录清理并删除状态文件） |
| 装载成功但应用看不到数据 | `WRITER=PARALLEL` 的独立事务已提交，或调用方未 `COMMIT` | 检查事务边界 |

---

## 15. 已知限制

1. 并行路径无逐行容错（见 [§11](#11-错误处理与-badfile)）。
2. 并行路径不保证行序。
3. 并行路径假设输入编码 = 数据库编码。
4. `TUPLE` / `FUNCTION` 未并行化。
5. 定长并行：`NULLIF` 模式 ≤ 64 字节；记录必须完整 `STRIDE`。
6. 单条记录上限 256 MiB。
7. 需要 `max_worker_processes ≥ PARALLEL_WORKERS + 1`。
8. `WRITER=PARALLEL` 使用独立事务（上游行为），与调用方事务不原子。
9. 仅在 PostgreSQL 18.6 / x86_64 Linux 上完整验证。
10. 并行装载期间目标表持有 `AccessExclusiveLock`，与任何并发访问互斥。
11. 导出（[§18](#18-导出表数据pg_fastload_export)）是单进程串行的，`PARALLEL` 对它无效；
    瓶颈是单个后端的 CPU（16 核里只用到 1 核），落盘只占约 4%，见
    [BENCHMARK §8](BENCHMARK.zh-CN.md#8-导出吞吐pg_fastload_export-与-copy-to-对比)。
12. 定长导出用定宽字段，文本列回灌时 `CHAR(n)` 会丢尾随空格、`VARCHAR(n)` 会保留补位空格，
    二者都不等于"精确往返"（见 [§18.5](#185-往返语义务必读这一节)）。

---

## 16. 从 pg_bulkload 迁移

| 项 | pg_bulkload | pg_fastload |
|---|---|---|
| 扩展 | `CREATE EXTENSION pg_bulkload` | `CREATE EXTENSION pg_fastload` |
| SQL 函数 | `pgbulkload.pg_bulkload(...)` | `pgfastload.pg_fastload(...)` |
| 命令行 | `pg_bulkload` | `pg_fastload` |
| 控制文件 | `KEY = VALUE` | **完全相同**，可原样复用 |
| 并行 | `WRITER=PARALLEL` | 新增 `PARALLEL=ON`；`WRITER=PARALLEL` 仍可用 |

迁移步骤：

```sql
-- 1) 保留旧扩展一段时间以便回滚
CREATE EXTENSION pg_fastload;
-- 2) 验证一次装载
SELECT count, duration FROM pgfastload.pg_fastload(ARRAY[
  'INPUT=/data/t.csv','OUTPUT=public.t','PARALLEL=ON']);
-- 3) 确认无误后移除旧扩展
DROP EXTENSION pg_bulkload;
```

两者可以共存（schema、扩展名、共享库名均不同），但不要对同一张表并发装载。

---

## 17. 升级与卸载

```sql
-- 升级（同一 major 内的版本切换）
ALTER EXTENSION pg_fastload UPDATE;

-- 卸载
DROP EXTENSION pg_fastload;
```

```bash
# 移除二进制与扩展文件
sudo make USE_PGXS=1 PG_CONFIG=/usr/pgsql-18/bin/pg_config uninstall
```

卸载不会删除 `$PGDATA/pg_fastload/` 下的历史日志与 LSF 文件，可手动清理
（若存在未完成的 `DIRECT` 装载，应先 `pg_fastload -r` 恢复再删除 LSF）。

---

## 18. 导出表数据（`pg_fastload_export`）

装载是「文件 → 表」，导出是「表 → 文件」。两者共用同一套 `Writer` 抽象，所以同一个
`OUT_COL` 布局既能描述导出的字节，也能描述装载期待读到的字节 —— 这是本文档里所有"往返"
说法的来源。

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV'
]);
--  pg_fastload_export
-- --------------------
--              7375042
```

返回 `bigint`：写出的行数。

### 18.1 执行模型

| 环节 | 实现 | 说明 |
|---|---|---|
| 取行 | `TABLE=` + `PARALLEL=ON`：N 个后台工作进程，各自用 `heap_setscanlimits()` 认领一段 `ctid` 块区间 | 块区间是物理的，所以分片先于启动就确定，运行期无需任何协调 |
| 取行 | `QUERY=`、`PARALLEL=OFF`，或表太小不值得拆分：`SPI_cursor_open_with_args(..., read_only)` + `SPI_cursor_fetch()`，每批 1000 行 | 走查询规划器，权限、行可见性、快照与普通 `SELECT` 完全一致 |
| 格式化 | `CsvWriter`（`lib/writer_csv.c`）或 `BinaryWriter`（`lib/writer_binary.c`） | 工作进程里写进 `StringInfo` 缓冲，每 256 KiB / 4096 行发一条消息 |
| 写文件 | 一个专职写文件进程：以**数据库服务端**身份 `open()`，路径必须是绝对路径 | `OUTPUT` 已存在则直接报错，绝不覆盖；4 MiB 缓冲，关闭时 `fsync` |
| 并发 | `TABLE=` 默认并行；`QUERY=` 始终串行 | 见 [§18.6](#186-并行导出table-的默认行为) |

`TABLE=` 在内部展开为 `SELECT * FROM <table>`；要过滤、排序或只导部分列，用 `QUERY=`。
两者互斥，必须给一个。

并行导出**不保证行序**。输出是这张表的完整合法转储，但行的先后取决于各工作进程谁先完成——
这是「没有共享队列、没有逐行协调」的代价。需要顺序时用 `QUERY=`（始终串行），或在重新装载后自行排序。

### 18.2 关键字

| 关键字 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `TABLE=` | 关系名 | — | 源表，与 `QUERY=` 二选一 |
| `QUERY=` | 任意 `SELECT` | — | 只读游标，`UPDATE`/`DELETE` 会被拒绝（`cannot open UPDATE query as cursor`）；始终串行 |
| `OUTPUT=` | 绝对路径 | **必填** | 输出文件 |
| `TYPE=` | `CSV` / `BINARY` / `FIXED` | `CSV` | `BINARY`/`FIXED` 是定长布局，必须给 `OUT_COL` |
| `OUT_COL=` | 类型声明，可重复 | — | 一个输出列一个，顺序与查询输出列一致；语法与装载的 `COL` 完全相同 |
| `DELIMITER=` `QUOTE=` `ESCAPE=` `NULL=` `HEADER=` | 单字符 / 布尔 | `,` `"` `"` 空串 `OFF` | 仅 `TYPE=CSV`，语义与 `COPY ... (FORMAT csv)` 一致 |
| `LIMIT=` | ≥ 1 | 不限 | 最多写多少行（行数精确，但并行时是哪些行不确定） |
| `PARALLEL=` | `ON` / `OFF` | `ON` | 把 `TABLE=` 导出分摊到多个查询进程 |
| `PARALLEL_WORKERS=` | 1..64 | 在线 CPU 数 − 1 | 工作进程数；给 `1` 即走串行路径 |
| `LOGFILE=` | 绝对路径 | `<OUTPUT>.log` | 导出日志 |
| `PROGRESS=` | `ON` / `OFF` | `ON` | 发布 `direction=export` 的进度记录 |
| `PROGRESS_NOTICE=` | `ON` / `OFF` | `OFF` | 额外输出 `NOTICE` 进度行 |
| `PROGRESS_INTERVAL=` | 100..3600000 ms | `1000` | 指定即等价于 `PROGRESS=ON` |

`TYPE` 只接受这三种值，`TYPE=TUPLE` / `FUNCTION` 是装载专用的输入格式，导出无意义。

### 18.3 CSV：与 `COPY` 逐字节相同

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV',
  'HEADER=ON',
  'PARALLEL=OFF']);      -- 要逐字节比对，必须走串行路径
```

```
$ psql -c "COPY public.lineitem TO '/tmp/copy.csv' WITH (FORMAT csv, HEADER)"
$ cmp /tmp/copy.csv /data/lineitem.csv && echo identical
identical
```

逐字节相同只对**串行**路径成立（`PARALLEL=OFF` 或 `QUERY=`）。并行导出写出的行集完全相同但顺序不同，
只能按多重集比较，见 [§18.6](#186-并行导出table-的默认行为)。

引号规则逐条复刻 `CopyAttributeOutCSV()`：字段含 `DELIMITER`／`QUOTE`／`ESCAPE`／`\n`／`\r`
时整体加引号并把 `QUOTE` 双写；字段内容等于 `NULL` 串时也加引号（否则无法与 NULL 区分）；
`NULL` 串为空时空字段加引号（`""` 与空串是两个值）。默认 `NULL` 写成空字段，`NULL=\N`
可让空字段与 NULL 分道扬镳 —— 与 `COPY` 的选择一致。

- `HEADER=ON` 写一行列名（来自结果行的 `TupleDesc`，`QUERY=` 里的表达式列可能叫 `?column?`）。
- 空表 + 无 `HEADER` → 0 字节文件；空表 + `HEADER=ON` → 只有表头一行。
- 吞吐：800 万行 / 312 MiB 的表，热缓存下串行导出 0.90 s（≈363 MB/s、8.9M 行/s）；同一张表的服务端
  `COPY TO` 是 0.80 s，输出 `cmp` 相同。串行导出 CPU 受限在**一个后端**里 —— 约 0.48 s 花在每行的
  游标/deform/内存上下文开销、0.40 s 文本格式化、0.04 s 落盘 —— 这正是 `PARALLEL=ON`（默认）分摊到
  多个进程的部分。分解与并行扩展曲线见
  [BENCHMARK §8](BENCHMARK.zh-CN.md#8-导出吞吐pg_fastload_export-与-copy-to-对比)。

### 18.4 BINARY / FIXED：定长布局与样例控制文件

```sql
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.bin',
  'TYPE=BINARY',
  'OUT_COL=INTEGER',      -- l_orderkey
  'OUT_COL=INTEGER',      -- l_partkey
  'OUT_COL=INTEGER',      -- l_suppkey
  'OUT_COL=INTEGER',      -- l_linenumber
  'OUT_COL=DOUBLE',       -- l_quantity
  'OUT_COL=DOUBLE',       -- l_extendedprice
  ...]);
```

| 声明 | 宽度 | 编码 |
|---|---|---|
| `SMALLINT` / `INTEGER` / `BIGINT` | 2 / 4 / 8 | 小端原生二进制，与装载端 `COL` 同义 |
| `FLOAT` / `DOUBLE` | 4 / 8 | IEEE 754 |
| `CHAR(n)` / `VARCHAR(n)` / 裸数字 `n` | n | 文本，右侧补空格到 n 字节 |

`TYPE=FIXED` 与 `BINARY` 的字节布局完全相同，区别只在**允许的声明**：`FIXED` 是定长文本
格式，只接受 `CHAR(n)` / `VARCHAR(n)`，写 `OUT_COL=INTEGER` 会被拒绝
（`TYPE=FIXED writes text only`）。数值列想用原生字节序就用 `BINARY`。

导出还会在 `<OUTPUT>.ctl` 写一份样例控制文件，可以直接对着它改：

```ini
INPUT = /data/lineitem.bin
# exported from public.lineitem
OUTPUT = lineitem
...
TYPE = BINARY
COL = INTEGER (4)
...
```

`OUTPUT` 取文件名（`lineitem.bin` → `lineitem`），**不是**源表名：样例描述的是一个文件，
装进哪张表由你决定；默认成源表会让一次手滑的 `pg_fastload lineitem.bin.ctl` 把数据追回源表。
导出与装载走的是同一份布局代码，所以 `TYPE=BINARY` 的样例同样可以装 `TYPE=FIXED` 导出的
纯文本文件（`BINARY` 与 `FIXED` 的读取端是同一套定长解析）。

### 18.5 往返语义（务必读这一节）

| 情形 | 结果 |
|---|---|
| 数值 / 布尔 / 时间等定长列 | `BINARY` 精确往返（`OUT_COL` 与 `COL` 声明一致即可） |
| 文本列，回灌用 `COL=CHAR(n)` | 精确，但**原值的尾随空格会丢**（`CHAR` 语义，`Read_char` 裁空格） |
| 文本列，回灌用 `COL=VARCHAR(n)` | 补位空格**保留**：`'xy'` 写进 8 字节字段，回灌得到 `'xy      '`（8 字符） |
| `NULL` | 定长格式没有独立 NULL 位，必须 `OUT_COL=TYPE NULLIF <模式>`；列里真有 NULL 而没声明 `NULLIF` 会报错 |

第三条是格式本身的限制，不是缺陷：定长字段一定被补满，`VARCHAR` 的读取端按声明长度原样保留，
`CHAR` 的读取端按 `CHAR` 语义裁掉尾随空格。因此

- 想要「导回去和原来一模一样」→ 文本列用 `OUT_COL=CHAR(n)`，回灌 `COL=CHAR(n)`；
- 想保留尾随空格 → 只能 `VARCHAR` + 定宽填满（`length(v) = n`），或用 `TYPE=CSV`。

`NULL` 的处理是显式的：如果不给 `NULLIF` 而数据里出现了 NULL，导出会报
`column N is NULL and OUT_COL gives it no representation`，而不是静默写 0 或空格。

宽度与取值范围在写之前校验：`value too long for type character(n)`、无符号类型的上界都会
在对应的行上抛错，整条语句回滚，文件留下已写入的部分（`LOGFILE` 里有失败的列号）。

### 18.6 并行导出（`TABLE=` 的默认行为）

导出在一个后端里是 CPU 受限的：800 万行的参照表上，约 0.48 s 花在逐行游标/解构、0.40 s 花在文本
格式化、只有 0.04 s 是真正写文件。后一项不可压缩，所以导出把装载的进程模型反向搬了过来：
**N 个查询进程负责格式化，1 个专职写文件进程负责落盘。**

```
SELECT pgfastload.pg_fastload_export(ARRAY[
  'TABLE=public.lineitem',
  'OUTPUT=/data/lineitem.csv',
  'TYPE=CSV',
  'PARALLEL=ON',              -- 默认值；PARALLEL=OFF 走串行
  'PARALLEL_WORKERS=8']);     -- 默认：在线 CPU 数 - 1
```

| 角色 | 做什么 | 不做什么 |
|---|---|---|
| 主进程（你的后端） | 建 DSM、启动写进程与查询进程、发布进度、写样例 `.ctl` | 不碰任何一行数据 |
| 查询进程 × N | 打开表、恢复你的快照、扫自己那段块区间、把行格式化成 256 KiB / 4096 行一块、经 `shm_mq` 发出 | 不打开输出文件 |
| 写进程 × 1 | `O_CREAT\|O_EXCL` 打开 `OUTPUT`，按到达顺序追加每一块，`fsync`，回报行数与字节数 | 不解析、不格式化、不解压 |

每个查询进程拥有**互不相交的一段 `ctid` 块区间**（`heap_setscanlimits()`），分片在启动前就算好，
运行期不需要共享队列、不需要任务窃取、数据通路上没有锁。写进程除了一个接收缓冲不分配任何东西：
一块数据就是「字节流 + `{nrows, nbytes}`」，所以 `TYPE=CSV` 和 `TYPE=BINARY` 共用同一个写进程。

**不保证行序。** 各进程把自己那段的产出直接追加，写进程按到达顺序落盘，因此同一张表导两次可能得到
内容相同、顺序不同的两个文件。这是契约而不是缺陷：它正是数据通路无锁的原因。需要顺序时用 `QUERY=`
（始终串行），或回灌后再排序。串行与并行的输出不能 `cmp`，要按行集或内容摘要比较。

`LIMIT=` 仍然精确。各进程用 `pg_atomic_fetch_add_u64` 抢占共享预算，最后一个进程把本块裁到剩余额度，
所以 `LIMIT=1000` 一定只写 1000 行 —— 但并行时**是哪 1000 行**不确定。

自动回退串行路径（导出日志里记 `PARALLEL disabled (<原因>)`，不报错）：

| 条件 | 日志原因 |
|---|---|
| `QUERY=` | `QUERY= cannot be split by block range` |
| `PARALLEL=OFF` 或 `PARALLEL_WORKERS=1` | `workers=1` |
| 表不足 32 个块 | `the table is too small` |
| 每个进程不足 16 个块 | 不报错，改为调小进程数 |
| 源是视图 / 分区父表 / 外部表，或带引号的名字 | `"..." is not a plain table` |
| 关系名解析不到 | `relation "..." not found` |

`max_worker_processes` 必须 ≥ `N + 1`（N 个查询进程加 1 个写进程）；启动失败降级为串行，不会让导出失败。

前提：库里必须已 `CREATE EXTENSION pg_fastload`（工作进程由该扩展注册），调用者必须是超级用户
（与任何服务端写文件操作一样）。

吞吐与扩展曲线见 [BENCHMARK §8](BENCHMARK.zh-CN.md#8-导出吞吐pg_fastload_export-与-copy-to-对比)。

### 18.7 进度

导出与装载用同一张视图（[§12.4](#124-加载进度)），记录里 `direction = export`、
`infile` 是输出文件、`bytes_read` 是已写出的字节数。因为不知道总量，`percent` 与 `eta_seconds`
为 `NULL`，`rows_per_second`、`bytes_per_second` 照常有：

```
 pid  | datname |    relname     | direction | input_type |  mode  |  state  |  rows   | bytes_read
------+---------+----------------+-----------+------------+--------+---------+---------+------------
 4312 | tpcc    | public.lineitem| export    | CSV        | serial | running | 3687521 |  536870912
```

### 18.8 限制

1. **行序不保证**：并行导出（`TABLE=` 的默认行为）按到达顺序落盘，见 [§18.6](#186-并行导出table-的默认行为)；
   需要顺序用 `PARALLEL=OFF`／`QUERY=`，或回灌后排序。
2. **只读但持快照**：导出在调用方快照下扫描，长导出会拖住 `VACUUM` 的 `oldest xmin`。
3. **目标文件必须是服务端本地路径、绝对路径、且不存在**。
4. **需要超级用户**（与装载一致：写文件是服务端行为）。
5. **并行导出要求库里装了扩展**（工作进程由 `pg_fastload` 注册），且 `max_worker_processes ≥ N + 1`。
6. `TYPE=FIXED` 只支持字符类型；`OUT_COL` 数量必须等于输出列数（CSV 不需要 `OUT_COL`）。
7. `QUERY=` 只接受单条 `SELECT`（不能带 `;`，不能是 `WITH ... INSERT`），且始终串行。
8. 生成样例控制文件的 `OUTPUT` 是文件名，回灌前要改成目标表；样例永远写 `TYPE = BINARY`
   （`FIXED` 的纯文本布局同样适用）。

---

*相关文档：[架构](ARCHITECTURE.md) · [修改点](CHANGES.md) · [性能基准](BENCHMARK.md) · [验证报告](VERIFICATION.md) · [优化记录](OPTIMIZATION.md)*
