# pg_fastload 文档索引 / Documentation Index

| 中文 | English | 内容 |
|---|---|---|
| [../README.md](../README.md)（英）/ [../README.zh-CN.md](../README.zh-CN.md)（中） | 项目定位、与 pg_bulkload 的差异、架构摘要、快速开始、限制 |
| [USER_MANUAL.md](USER_MANUAL.md)（英）/ [USER_MANUAL.zh-CN.md](USER_MANUAL.zh-CN.md)（中） | 安装、控制文件语法、关键字全表、SQL/CLI 用法、并行调优、错误处理、FAQ、迁移与卸载 |
| [ARCHITECTURE.md](ARCHITECTURE.md)（英）/ [ARCHITECTURE.zh-CN.md](ARCHITECTURE.zh-CN.md)（中） | 四阶段流水线、组件职责、数据流、DSM/IPC 协议、降级设计、内存生命周期 |
| [PORTING.md](PORTING.md)（英）/ [PORTING.zh-CN.md](PORTING.zh-CN.md)（中） | PG 15–19 支持矩阵、内核差异与适配点、验收方法与结果、分支策略 |
| [CHANGES.md](CHANGES.md) | — | 相对 pg_bulkload 3.1.23 的文件级修改点，按已采纳的轮次排列 |
| [BENCHMARK.md](BENCHMARK.md) | — | 测量方法、1 GiB CSV / 定长结果、冷缓存、与上游的历史对比、调参建议 |
| [VERIFICATION.md](VERIFICATION.md) | — | 验证环境、90/90 与 44/44 用例、构建与内存核查、合规检查、残留风险 |
| [OPTIMIZATION.md](OPTIMIZATION.md) | — | 逐轮优化记录、性能演进、未采纳方案汇总 |

## 图

| 文件 | 说明 |
|---|---|
| [img/architecture.zh-CN.svg](img/architecture.zh-CN.svg) | 整体架构图（中文） |
| [img/architecture.svg](img/architecture.svg) | Overall architecture (English) |

## 关于 `pg_timestamp`

`util/pg_timestamp` 是随本项目一起提供的可选工具：执行 `util/pg_timestamp.sql` 会把 `timestamp`
类型的输入函数（`pg_type.typinput`）替换为 `pg_timestamp_in`，从而跳过装载时的文本时间戳解析开销。

注意：这是**实例级全局改动**，会影响该实例上所有 `timestamp` 的文本解析；
`util/uninstall_pg_timestamp.sql` 可还原。它不参与并行路径，本版本未对它做修改。
