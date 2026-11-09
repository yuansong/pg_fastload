# pg_fastload 用户手册

> 适用版本：`pg_fastload 1.0.0`（基于 pg_bulkload 3.1.23 二次实现）
> 对应数据库：PostgreSQL 18.x（本版本仅在 18.6 上完整验证）
> 英文版：[USER_MANUAL.en.md](USER_MANUAL.en.md)

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

---

## 1. 概览

`pg_fastload` 是 PostgreSQL 的高速批量加载工具。它把一次加载拆成四个阶段：

| 阶段 | 内容 | 串行路径 | 并行路径（`PARALLEL=ON`） |
|---|---|---|---|
| ① 读输入 | 从文件/STDIN 取字节 | leader 进程 | 1 个 reader bgworker |
| ② 解析记录 | 记录边界 + 字段切分 | leader 进程 | N 个 worker |
| ③ 形成元组 | 输入函数 → `heap_form_tuple()` | leader 进程 | N 个 worker |
| ④ 写入 | `WriterInsert()` | leader 进程 | leader 进程（不变） |

**没有 `PARALLEL` 时，行为与 pg_bulkload 完全一致**；打开 `PARALLEL=ON` 后只有 ①②③ 被并行化，
④ 以及所有事务/约束/错误语义都留在原处。因此 `PARALLEL=ON` 是一个"可以随时打开"的开关：
不满足条件的场景会自动回到串行路径并打印一条 INFO。

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
| `SKIP` / `OFFSET` | 整数 ≥ 0 | `0` | 跳过前 N 条记录（记录单位，非字节） |
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
| `PARSE_ERRORS` / `MAX_ERR_CNT` | 整数，`-1` = 不限 | `0` | 允许的解析错误数；超过则装载失败 |
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
| 字段数不符 / 输入函数报错 / 未闭合引号 | 计入 `parse_errors`；≤ `PARSE_ERRORS` 时写入 `PARSE_BADFILE` 并继续 | **该 worker 退出，整次装载失败** |
| 重复键 | `DUPLICATE_ERRORS` 上限内写入 `DUPLICATE_BADFILE`，按 `ON_DUPLICATE_KEEP` 取舍 | 由 leader 串行写入，行为与串行一致 |
| 约束违反 | 报错回滚 | 报错回滚 |

因此：

- **需要容错装载就用 `PARALLEL=OFF`**；
- 并行路径适合"数据已清洗、要求快"的场景；
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
CPU 1.20s/13.40u sec elapsed 1.76 sec
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

*相关文档：[架构](ARCHITECTURE.md) · [修改点](CHANGES.md) · [性能基准](BENCHMARK.md) · [验证报告](VERIFICATION.md) · [优化记录](OPTIMIZATION.md)*
