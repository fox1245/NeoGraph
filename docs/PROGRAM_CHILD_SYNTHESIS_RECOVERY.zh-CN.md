<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_RECOVERY.md locale=zh-CN source_sha256=6ed07e73d44459a4ff1885cb7f07064be6994987113c36f0c7bae8c9c79d76ed -->
# 子合成恢复验证

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_RECOVERY.zh-CN.md)

N3 在 SQLite 和 PostgreSQL 上验证经过审查的源、单子 checkpoint 场景。恢复现在区分已记录的 child 操作和未分类外部效果；当 Core 派发没有持久 checkpoint 或结果时，仍明确要求对账。

## 恢复规则

恢复考虑派发前，父命令 journal 必须匹配获准 Program、command ordinal、payload 和效果身份。只有 `spawn`，或以合成 `spawn` 结尾的 `await` 链可以进入此恢复路径。宿主重新验证 grant、原始 generation、实际 compile 扣减和生成 binding。先前记录的派发还必须匹配精确 child run ID 和规范输入。

恢复复用命令已有资源预留和 operation 扣减，不重复计费，也不授予另一次动态编译。子预留重建包括待处理命令持有的资源；重建只改变进程本地记账视图，不改变持久预算。现有子记录及终止结果仍具有最终约束力。

相等且已存在的 `Compiling` 或 `Validating` 发布，不证明新调用方拥有该工作。只有收到 `Published` 的写入方可启动认领阶段。并发冷恢复测试在此发布前同步两个独立进程，要求一个成功 worker、一个冲突以及一次语义验证。

## 进程退出矩阵

每个案例在选定发布后不运行析构函数就退出进程，再打开全新 Catalog、transition 和 checkpoint stores。两个数据库运行相同案例。

| 中断点 | 必需恢复 |
|---|---|
| `Reserved` | 从已有预留编译一次 |
| `Compiling` | 保留扣减；要求对账 |
| `Compiled` | 复用 bundle |
| `Validating` | 保留扣减；要求对账 |
| `Validated` | 复用语义证据 |
| `Admitting` | 复用冻结准入 |
| `Admitted` | 绑定获准版本 |
| `Bound` | 执行记录的父命令 |
| `Dispatching` | 复用其 child 身份和输入 |
| `Spawned` | 恢复已有 child，或显式对账不确定 Core 派发 |
| Child relation `Publishing` | 完成已有 child 的初始发布 |
| Child relation `Dispatched` | 非重放安全 Core 工作缺少 checkpoint/result 时对账 |
| Child terminal result | 复用结果；不再执行已完成 child |
| Parent child-result attachment | 复用已有 join 结果 |
| Parent command result | 重放记录的命令结果 |
| Parent terminal result | 返回同一终止结果身份 |

每个数据库有 16 个进程退出边界，另加一场双进程恢复竞争。测试还验证原始 child 身份、一条持久 child relation、单次 compile 扣减以及保留的 command-operation 计数。执行标记验证已完成 child 工作不会跨进程退出重复。

## 不确定的子执行

仅有 `Dispatched` relation 不能证明 Core 工作是否在进程消失前执行。若 child 仍记录为运行中，没有精确 checkpoint，且其 plan 在缺少 checkpoint 时不能安全重放，父级重连采取失败关闭策略。`recover_child_synthesis` 记录 `ReconciliationRequired` 与 `P_CHILD_SYNTHESIS_CHILD_UNCERTAIN`，保留 child ID 和输入。

此非活动父级恢复检查不会把活动父级重连误判为执行丢失。活动重连返回现有 attempt。

即使 synthesis record 已是 `Spawned`，不确定性处置也适用。记录可追加该对账处置而不更改早期产物。它不创建替代 child，也不伪造成功结果。此变更不提供自动对账批准 API。

## 附加检查

- 预留确认丢失后合成历史无法读取，不能退还 compile 单位。读取恢复后复用原请求。
- PostgreSQL 测试从预留事务内部终止数据库连接。新连接看到原父级且无部分预留；后续 attempt 只扣减一次。
- 语义验证期间取消，不能进入准入或 child 派发。
- Catalog 激活不能将生成 binding 重定向到其他版本。
- Retention pins 保留必需版本。宿主若移除 child 版本，缓存合成证据不能执行它。自动从所有合成历史收集 retention roots 仍由宿主负责。
- Program 替换保留 compile 扣减，不将旧 run 未附接的生成 binding 转移到 successor generation。已附接 descendants 现在可通过 [递归 Harness 契约](PROGRAM_RECURSIVE_HARNESSES.md) 保留。
- PostgreSQL 测试共用现有 CTest 数据库资源锁，包括进程退出矩阵及连接终止案例。

## 此验证的限制

这是针对所述纵向场景的进程和连接故障验证，不声称普遍外部效果恰好一次、断电验证、跨宿主数据库故障切换，或自动解决不确定 Core/provider 结果。包含混合宿主效果的一般结构化 join 保留既有对账行为。多个生成 child 树、任意外部验证器期间取消、完整图迁移组合、自动 retention-root 发现，以及合成性能或存储增长验证，需要单独覆盖。

宿主集成见 [持久化契约](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)；未改变的经过审查的源及权限边界见 [授权契约](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)。
