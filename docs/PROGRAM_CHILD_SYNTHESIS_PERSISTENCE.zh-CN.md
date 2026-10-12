<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md locale=zh-CN source_sha256=14283f61af8f5c3ec52011a184bc863369346836057023301f372ff29340a1ab -->
# 持久的子 Program 合成

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.zh-CN.md)

子 Program 合成的持久化可用于内存参考存储、SQLite 和 PostgreSQL。
[恢复矩阵](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)列出了针对单个子 Program 场景所测试的恢复行为。
宿主入口使用现有的顶层检查点，以及现有的 `ng.spawn` / `ng.await` 子 Program 生命周期。
这是一条有界的、针对经过审阅的源码的路径；它不实现模型生成器、模板渲染器，也不新增 DSL 命令。

## 宿主集成

配置 `RuntimeConfig::child_synthesis_gateway` 和 `child_synthesis_grant_resolver`。
解析器必须使用 `(owner_scope, parent_run_id, grant_id)`，从受信的宿主策略中选出确切的授权。
从合成记录里加载出来的授权，并不能证明它有权限。

1. 在父 Program 的下一个顶层检查点处，安排一个 `ProgramHandoff`。
2. 加载父运行、当前生效的谱系和 generation，并按
   [授权契约](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)所述，选定一份经过审阅的源码实例和授权。
3. 在持有 handoff 的同时，调用 `ProgramRuntime::prepare_child_synthesis(owner, parent, handoff,
   proposal, grant, binding_name)`。
4. 返回的记录到达 `Bound` 之后，释放 handoff。
5. 父 Program 在它正常的 `ng.spawn` / `ng.await` 路径里，使用这个绑定名。

例如，已准入的父 Program 可以这样 yield：

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

检查点请求如何对应到经过审阅的源码和策略，由宿主决定。
检查点的 payload 不能给自己授予编译或执行的权限。
持久路径通过运行时事务来负责预留；它不会调用网关独立的 `reserve` 或 `reserve_child` 回调。

## 原子发布

`ProgramTransitionPublication::child_synthesis_records` 在与父 Program 的运行快照、
journal head 和谱系相同的事务里，追加一条不可变的记录。第一条 `Reserved` 记录，
必须绑定确切的先前父快照、来源谱系、产生的谱系，以及一个单位的动态编译扣减。
相互冲突的请求，不能复用一模一样的预算转换，来为另一个不同的绑定出资。
完全相同的发布重试，返回 `AlreadyPresent`。

SQLite 使用它现有的 `BEGIN IMMEDIATE` 事务。PostgreSQL 使用它现有的事务和 owner 的 advisory lock。
两者都追加到合成日志，并按追加顺序重建经过校验的请求 head。写入失败时，
父 Program、谱系和合成记录一起回滚。内存参考实现保留它的强异常保证。

带有合成记录的发布，使用存储 schema 6。普通的发布仍然序列化为 schema 5；
读取方仍支持 schema 1–5。`load_child_syntheses(owner, parent_run)` 返回当前的请求 head。
对于不支持这种历史的自定义存储，基类实现会失败时关闭。
带动态预算的运行要恢复，需要这个读取能力。

记录最大为 8 MiB，最多 16 个修订。它们保留提案、原始的宿主授权和父 Program 的上下文、预留，
以及累积的各阶段输出。规范标识和阶段校验，会拒绝被改动的先前制品、不同的 generation、
改了名的绑定和被修改的证据。绑定名在同一个父运行内是唯一的。SQL 日志只追加，
没有对大型历史的保留和压缩。

## 阶段与恢复

| 持久状态 | 恢复之后的下一步 |
|---|---|
| `Reserved` | 认领并编译一次 |
| `Compiling` | 标记为 `ReconciliationRequired`；不要重放状态不确定的编译 |
| `Compiled` | 复用已存储的 bundle，并认领语义验证 |
| `Validating` | 标记为 `ReconciliationRequired`；不要重新运行状态不确定的验证器 |
| `Validated` | 复用已接受的回执和证据；选定准入策略 |
| `Admitting` | 复用冻结的准入，以及幂等的 Catalog 准入 |
| `Admitted` | 使用带作用域的模块回执，链接确切的已准入版本 |
| `Bound` | 继续父 Program 正常的子 Program 命令 |
| `Dispatching` | 通过 `start_child` 复用已记录的子 ID 和输入 |
| `Spawned` | 复用子 Program 及其结果，或者在没有持久检查点时，对账 Core 的分发 |
| `Failed` / `ReconciliationRequired` | 保留结果，并阻止父 Program 的自动重放 |

要重新连接一个合成尚未完成的、不活跃的父 Program，先调用
`recover_child_synthesis(owner, parent_run, proposal_id)`。运行中的父 Program 则需要使用被持有检查点的 API。
每次恢复都会重新选定宿主权限，并检查原来的 generation。已完成的语义验证和冻结的准入会被复用。
语义验证被拒绝时，保留它的回执和证据，并且永远不会走到准入。

生成出来的绑定，是根据实际的父运行和 generation 来解析的，而不是静态解析器那个以父 Program 版本为范围的作用域。
分发在 `start_child` 之前，记录稳定的子 ID 和输入；一份授权不能创建第二次调用。
正常的子 Program 预算、发布、执行和 join，仍然具有权威性。

完成、重试和恢复时（包括提交确认丢失的情形），父 Program 都保持已扣减的编译额度。
合成会扣减已过去的挂钟时间；恢复出来的尝试，同样保留原来的合成截止时间。
spawn 状态的发布，会保持现有命令那笔正在进行中的预留。

## 测试覆盖与局限

后端符合性测试覆盖：成功的 spawn/join、规范的往返、owner/运行的隔离、重复和并发的请求、
后端写入失败时的回滚、已提交阶段的复用、语义验证被拒绝、被撤销的授权，
以及状态含糊的编译/验证认领。Linux 的进程退出测试，在冻结的准入之后不经析构函数就终止进程，
然后在 SQLite 和 PostgreSQL 上重新打开 Program 存储、转换存储和检查点存储，再成功 join 子 Program。

[恢复矩阵](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)把覆盖范围扩大到：每个数据库 16 个进程退出的边界、
并发的进程恢复、丢失的确认、真实的 PostgreSQL 连接终止、取消、版本留存、激活，以及 Program 替换。
它的局限同样是明确的：没有自动对账的授权器，并且混合了多种副作用的一般性 join、断电、
跨主机的故障切换，以及图迁移与留存的所有组合，都不在这个单个子 Program 场景的覆盖范围之内。
