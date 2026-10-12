<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_RECOVERY.md locale=zh-CN source_sha256=36fc7174e47297f0a9b32ef41b1d1fa5a5db5224b4e9512465bfee950d51c568 -->
# 子 Program 合成的恢复

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_RECOVERY.zh-CN.md)

本页列出在 SQLite 和 PostgreSQL 上，针对“经过审阅的源码、单个子 Program、检查点”这一场景所测试的恢复行为。
恢复会把已记录的子 Program 操作，与未分类的外部副作用区分开；
而当 Core 的分发既没有持久的检查点、也没有结果时，要求显式的对账。

## 恢复规则

在恢复考虑分发之前，父 Program 的命令 journal 必须与已准入的 Program、命令序号、
payload 和副作用标识一致。只有 `spawn`，或者以合成出来的 `spawn` 结尾的 `await` 链，
才能走这条恢复路径。宿主会重新校验授权、原来的 generation、实际扣减的编译额度和生成出来的绑定。
先前已记录的分发，也必须与确切的子运行 ID 和规范输入一致。

恢复会复用该命令已有的资源预留和操作扣减。它不会对同一条命令再次计费，
也不会再授予一次动态编译。重建子 Program 的预留时，会把待处理命令所占用的资源算进去；
这种重建改变的是进程本地的核算视图，而不是持久的预算。已有的子 Program 记录和终态结果仍具有权威性。

一条相同的、已经存在的 `Compiling` 或 `Validating` 发布，并不能证明新的调用方拥有这项工作。
只有收到 `Published` 的写入方，才可以启动已认领的阶段。并发冷恢复的测试，
在这次发布之前同步两个独立的进程，并要求有一个成功的 worker、一次冲突和一次语义验证。

## 进程退出矩阵

每个用例，都在选定的发布之后不经析构函数就退出进程，然后打开全新的 Catalog、
转换存储和检查点存储。两种数据库运行同样的用例。

| 中断点 | 要求的恢复方式 |
|---|---|
| `Reserved` | 根据已有的预留编译一次 |
| `Compiling` | 保持已扣减的额度；要求对账 |
| `Compiled` | 复用 bundle |
| `Validating` | 保持已扣减的额度；要求对账 |
| `Validated` | 复用语义证据 |
| `Admitting` | 复用冻结的准入 |
| `Admitted` | 绑定已准入的版本 |
| `Bound` | 执行已记录的父 Program 命令 |
| `Dispatching` | 复用它的子标识和输入 |
| `Spawned` | 恢复已有的子 Program，或者显式对账状态不确定的 Core 分发 |
| 子关系 `Publishing` | 完成已有子 Program 的初始发布 |
| 子关系 `Dispatched` | 如果不可安全重放的 Core 工作既没有检查点也没有结果，则对账 |
| 子 Program 的终态结果 | 复用该结果；不再次执行已完成的子 Program |
| 父 Program 挂接子 Program 的结果 | 复用已有的 join 结果 |
| 父 Program 的命令结果 | 重放已记录的命令结果 |
| 父 Program 的终态结果 | 返回同一个终态结果标识 |

这是每个数据库 16 个进程退出的边界，外加每个数据库一次两个进程的恢复竞争。
测试还会验证：原来的子 Program 标识、唯一一条持久的子关系、仅有的一次编译扣减，
以及保持不变的命令操作计数。执行标记用来验证，已完成的子 Program 工作不会因进程退出而被重复执行。

## 状态不确定的子 Program 执行

仅凭 `Dispatched` 这一关系，无法确定进程消失之前，Core 的工作是否已经运行过。
如果子 Program 仍记录为运行中、没有确切的检查点，而且它的计划没有检查点就无法安全重放，
那么父 Program 的重新连接就会失败时关闭。`recover_child_synthesis` 会记录
`ReconciliationRequired`，错误码为 `P_CHILD_SYNTHESIS_CHILD_UNCERTAIN`，并保留子 ID 和输入。

这项针对不活跃父 Program 的恢复检查，不会把对运行中父 Program 的重新连接，误判为执行丢失。
对运行中父 Program 的重新连接，返回的是已有的尝试。

即使合成记录已经显示 `Spawned`，这种“不确定”的处置同样适用。该记录可以追加这一对账处置，
而不改动更早的制品。它不会凭空造出替代的子 Program，也不会伪造成功的结果。
没有提供自动批准对账的 API。

## 其他检查

- 预留的确认丢失、随后合成历史又无法读取时，不能退还那一次编译额度。
  读取恢复之后，会复用原来的请求。
- PostgreSQL 测试从预留事务内部终止数据库连接。新的连接看到的是原来的父 Program，
  没有任何部分预留；随后的那次尝试只扣减一次。
- 语义验证期间发生取消，不能走到准入或子 Program 的分发。
- Catalog 的激活不能把生成出来的绑定重定向到另一个版本。
- 留存固定（pin）会保留所需的版本。如果宿主删除了子 Program 的版本，缓存的合成证据也无法执行它。
  从所有合成历史中自动收集留存根，仍是宿主的责任。
- Program 替换会保持已扣减的编译额度，并且不会把旧运行中尚未挂接的生成绑定，转移到后继 generation。
  已挂接的后代，可以通过[递归 Harness 契约](PROGRAM_RECURSIVE_HARNESSES.md)保留下来。
- PostgreSQL 测试共用现有的 CTest 数据库资源锁，包括进程退出矩阵和连接终止的用例。

## 局限

测试针对所述纵向场景中的进程故障和连接故障。它们没有证明外部副作用普遍恰好执行一次，
也没有证明能经受断电、能做跨主机的数据库故障切换，或能自动解决状态不确定的 Core/provider 结果。
包含多种宿主副作用的一般结构化 join，保持它现有的对账行为。多棵生成出来的子 Program 树、
任意外部验证器期间发生的取消、完整的图迁移组合、自动发现留存根，
以及合成的性能和存储增长，都需要另行覆盖。

宿主集成见[持久化契约](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)；
经过审阅的源码和权限边界见[授权契约](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)。
