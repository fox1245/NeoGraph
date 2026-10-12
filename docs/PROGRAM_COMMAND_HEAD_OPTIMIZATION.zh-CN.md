<!-- neograph-i18n: source=docs/PROGRAM_COMMAND_HEAD_OPTIMIZATION.md locale=zh-CN source_sha256=52b5dae9fe5c514792e6ec85508f8e548faef3d737711c484cd04294fa3542ef -->
# 命令发布 head 的优化：2026-09-07

**Languages:** [English](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) | [한국어](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ko.md) | [日本語](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_COMMAND_HEAD_OPTIMIZATION.zh-CN.md)

这项变更减少了发布 JavaScript 命令之前的读取。它把运行、journal 和最新的命令放在一起读取，而不是先分别加载运行和 journal，再把完整的命令历史物化出来。它不改变持久发布、CAS、预留、检查点和恢复的语义。

## 实现

- `ProgramTransitionStore::load_command_publication_head` 返回一致的运行/journal 配对，以及最新的一条命令追加记录。内存实现使用一份不可变的快照；SQLite 和 PostgreSQL 使用一条语句和已有的 `(owner_scope, run_id, sequence)` 主键。
- 数据库的投影，省略了用不到的迁移字节和上一次发布的字节。会对照规范值，检查最新命令的序号和坐标；并校验运行/owner/bundle/journal 的绑定。
- 正常的追加和结算使用最新的命令。针对更早坐标的重试，仍然使用完整的历史。重放，以及存储一侧的追加/预留校验，仍然像以前一样检查历史；这并没有彻底消除历史扫描。
- 现有的 C++ 存储包装器，得到的是一个回退实现：使用它们现有的虚函数读取，并用第二次运行读取来加以围栏。并发的变化会失败时关闭。包装器可以实现新的读取，同时保留它们自己的过滤和权限语义。
- 没有 schema 迁移、新索引、缓存生命周期、持久性设置、图 generation、编译器标识或模型配置方面的变化。
- 公开的 C++ 虚接口新增了一个方法：要把 Program 库和 C++ 使用方一起重新构建。已有包装器的源码兼容性有覆盖；预先构建好的 C++ 使用方，不假定二进制兼容。

## 正确性

WSL 上 GCC Debug 构建的 Program 测试集通过了 **680 个测试**，**跳过了 2 个不适用的内存存储进程重启用例**。新增的七个测试，以及增强后的后端历史测试，覆盖了不可变快照、独立的读取方/写入方连接、回退包装器、回退读取被撕裂（torn）的情形、所选尾部的损坏，以及有界的尾部读取。历史数据的损坏，仍然会被完整历史的读取所拒绝。

完整的测试集还在 SQLite/PostgreSQL 上，演练了命令恢复、谱系 CAS、递归的子 Program 替换、不可续期的预算，以及真实的进程丢失边界。这次变更没有声称重新做过 Windows 或 sanitizer 的运行。

## 成对的 Release 对比

与基线相同的配置：Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release。两个二进制文件在对比之前都已冻结。每个用例 5 对独立的进程；每个进程预热 3 次、正式测量 12 次调用。用例和 AB/BA 顺序交替。PostgreSQL 使用原生的 WSL Docker，每个进程使用一个新的数据库；SQLite 使用 WSL ext4 上的一个新文件。在成对矩阵运行期间，没有进行任何构建，也没有运行其他测试任务。

“之前”的二进制文件哈希，与最初的基线一致。下面当前的前后测量值，取代了与旧挂钟时间的对比；它们是在这一轮里一起测得的。不涉及 JIT，也不涉及 LLM。

| 用例 | 之前的中位数 (ms) | 之后的中位数 (ms) | 成对的中位数降幅 | 成对的之后/之前范围 |
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

降幅是根据成对进程的中位数计算的，所以不一定等于两个独立汇总中位数之比。落在成对离散范围之内的小幅差异，没有结论意义。直接 Core 和 C++ Program 的各行是阴性对照；优化过的命令发布读取在那里并不会用到。小载荷的内存用例，没有确立明显的加速。

直接 Core 上那一点小小的信号，引出了一次范围更大的对照检查：七对进程，每个进程预热 100 次、正式测量 10,000 次直接调用。它的之后/之前比值中位数是 1.0098，各对的范围是 0.9825 到 1.0123。原始结果保留在 `direct-control/` 下。没有改动 Core 的源码或库代码；这项检查并不能说明 Core 的性能有什么实质性的变化。

## 数据库 API 调用次数

另一次带插桩的对比，使用 1 次与 4 次调用，并把这个差值重复测了两遍。每次运行的计数差值，在两次重复中完全一致。这套插桩测得的耗时，不用于上面的主要对比。SQLite 的 step 调用包含逐行迭代；libpq 只涵盖同步调用。

| 后端 / 每次 Program 运行的 Core 调用数 | 之前的 SQL API 调用数 | 之后的 SQL API 调用数 | 之前的提交数 | 之后的提交数 |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

PostgreSQL 的提交数一列，只是 libpq 同步调用的那一部分；它不是异步检查点路径的事务总数。两个二进制文件运行的是同样的持久化接口，在被测量的范围内，提交次数保持不变。

## 复现与证据

用相同的 Release 选项，从“之前”的修订版（`8fce5e14`）和变更后的修订版，分别构建 `bench_program_cost`。保留这两个二进制文件。在一次性的 Postgres 容器运行、并设置了 `NEOGRAPH_COST_POSTGRES_URL` 的情况下：

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

详细的方法：见[测量 Program 的成本](PROGRAM_COST_MEASUREMENT.md)。原始的成对样本、计数、源码/二进制哈希和验证日志，保留在本地的 `artifacts/program-command-head-20260907/` 中；较大的 SQLite 文件留在 WSL 里。这个 runner 不会覆盖输出目录，也不会复用已有的应用数据库。

接下来剩下的优化目标，是发布和重放过程中对完整历史的校验/物化，以及规范记录的构造。这次变更并没有证明这些开销已被消除，也没有证明 JIT 会有帮助。
