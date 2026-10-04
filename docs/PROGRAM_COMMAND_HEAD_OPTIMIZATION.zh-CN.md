<!-- neograph-i18n: source=docs/PROGRAM_COMMAND_HEAD_OPTIMIZATION.md locale=zh-CN source_sha256=52b5dae9fe5c514792e6ec85508f8e548faef3d737711c484cd04294fa3542ef -->
# 命令发布 head 优化 — 2026-09-07

**Languages:** [English](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) | [한국어](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ko.md) | [日本語](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_COMMAND_HEAD_OPTIMIZATION.zh-CN.md)

此变更减少发布 JavaScript 命令前的读取。它一起读取 run、journal 和最新 command，而非分别加载 run/journal 并物化完整命令历史。它不改变持久发布、CAS、预留、checkpoint 或恢复语义。

## 实现

- `ProgramTransitionStore::load_command_publication_head` 返回一致的 run/journal 配对及最新 command append。内存使用一个不可变快照；SQLite 和 PostgreSQL 使用一条语句及现有 `(owner_scope, run_id, sequence)` 主键。
- 数据库投影省略未使用的 migration 和 last-publication 字节。最新 command sequence 和 coordinate 对照其规范值检查；run/owner/bundle/journal 绑定会验证。
- 普通 append 和 settlement 使用最新 command。较旧 coordinate 的重试仍用完整历史。replay 和 store 侧 append/reservation 验证仍照常检查历史；这没有彻底移除历史扫描。
- 现有 C++ store wrappers 获得使用已有虚读取的 fallback，以第二次 run 读取围住读取范围。并发变化采取失败关闭策略。wrappers 可实现新读取，同时保留自己的过滤和权限语义。
- 不改变 schema、不添加 index、不改变 cache lifetime、durability setting、graph generation、compiler identity 或 model configuration。
- 公开 C++ 虚接口增加一个方法：Program 库和 C++ 使用方需一起重新构建。已覆盖现有 wrappers 的源兼容性；不能假设预构建 C++ 使用方 ABI 兼容。

## 正确性

WSL GCC Debug Program suite 当时通过 **680 项测试**，跳过 **2 个不适用的内存存储进程重启案例**。七项新增测试及扩充的后端历史测试覆盖不可变快照、独立 reader/writer 连接、fallback wrappers、撕裂 fallback 读取、所选 tail 损坏和有界 tail 读取。完整历史读取仍拒绝历史损坏。

完整 suite 还覆盖命令恢复、lineage CAS、递归 child 替换、不可续增预算，以及 SQLite/PostgreSQL 真实进程丢失边界。此变更不声称重新运行 Windows 或 sanitizer 验证。

## 配对 Release 比较

使用与 baseline 相同的 Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release 配置。比较前冻结两个二进制。每案例五对独立进程；每进程 3 次 warmup 和 12 次测量调用。案例及 AB/BA 顺序交替。PostgreSQL 使用原生 WSL Docker，每进程新建数据库；SQLite 使用新的 WSL-ext4 文件。配对矩阵期间没有构建或其他测试任务。

before 二进制 hash 匹配原 baseline。下列同轮配对测量取代与旧墙钟计时的比较；这些数据是在当时同一轮中共同测得。不涉及 JIT 或 LLM。

| 案例 | 优化前中位数 (ms) | 优化后中位数 (ms) | 配对降幅中位数 | 配对 after/before 范围 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0076 | 0.0077 | -2.71% | 1.0007–1.0418 |
| memory-cpp-0 | 2.0373 | 2.0332 | -0.58% | 0.9857–1.0336 |
| memory-javascript-0-commands-1 | 3.3096 | 3.3046 | +0.59% | 0.9780–1.0432 |
| memory-javascript-65536-commands-1 | 31.0390 | 30.7436 | +0.89% | 0.9843–1.0272 |
| memory-javascript-0-commands-16 | 32.0270 | 32.2163 | -0.13% | 0.9808–1.0261 |
| sqlite-javascript-0-commands-1 | 19.5180 | 18.9032 | +3.20% | 0.9645–1.0497 |
| sqlite-javascript-65536-commands-1 | 130.1083 | 122.0217 | +6.02% | 0.9213–0.9490 |
| sqlite-javascript-0-commands-16 | 326.5041 | 256.5506 | +20.94% | 0.7812–0.8014 |
| postgres-javascript-0-commands-1 | 81.8919 | 78.3417 | +4.34% | 0.9037–1.0263 |
| postgres-javascript-65536-commands-1 | 257.4166 | 240.3124 | +6.76% | 0.9198–0.9608 |
| postgres-javascript-0-commands-16 | 1035.1816 | 922.3063 | +10.90% | 0.8689–0.9304 |
| postgres-cpp-0 | 49.0719 | 48.2794 | +1.48% | 0.9555–1.0062 |

降幅从配对进程中位数计算，未必等于两个独立汇总中位数之比。配对离散范围内的小差异没有定论。直接 Core 和 C++ Program 行是负对照；它们不使用优化后的命令发布读取。小 payload 内存案例没有确立明确加速。

微小直接 Core 信号促成一次更广的对照检查：七对进程，每个进程 100 次 warmup 和 10,000 次直接测量调用。after/before 中位比为 1.0098，配对范围 0.9825–1.0123。原始结果保留在 `direct-control/`。未更改 Core 源或库代码；该检查没有确立有意义的 Core 性能变化。

## 数据库 API 计数

单独插桩比较使用一次与四次调用，并重复计算差值两次。每个逐 run 计数差在两次重复中均完全相同。插桩计时不用于主要比较。SQLite step calls 包括行迭代；libpq 仅覆盖同步调用。

| 后端 / 每个 Program run 的 Core 调用数 | 优化前 SQL API 调用数 | 优化后 SQL API 调用数 | 优化前提交数 | 优化后提交数 |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

PostgreSQL commit 列只是同步 libpq 子集，不是 async checkpoint 路径的总事务数。两个二进制运行相同持久接口，在测量范围内保持相同 commit 数。

## 复现与证据

以相同 Release 选项，从 before revision (`8fce5e14`) 和变更 revision 构建 `bench_program_cost`。保留两个二进制。一次性 Postgres container 运行且已设置 `NEOGRAPH_COST_POSTGRES_URL` 时：

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

详细方法见 [Program 成本测量](PROGRAM_COST_MEASUREMENT.md)。原始配对样本、计数、源/二进制 hashes 和验证 logs 本地保留在 `artifacts/program-command-head-20260907/`；大型 SQLite 文件留在 WSL。runner 不覆盖输出目录，也不复用现有应用数据库。

下一个剩余目标是发布和 replay 期间的完整历史验证/物化，以及规范记录构造。此变更没有证明这些成本已消除，也没有证明 JIT 会有帮助。
