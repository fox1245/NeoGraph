<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md locale=zh-CN source_sha256=7a6203d69da6c363bf3c4c5b6aa5b1b1fcd2f36c656c6f22bffc36cddeeb1e72 -->
# 持久子合成

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.zh-CN.md)

状态：N2 已在内存参考存储、SQLite 和 PostgreSQL 中实现。单子场景的 N3 恢复验证见 [恢复矩阵](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)。宿主入口使用现有顶层 checkpoint 和 `ng.spawn` / `ng.await` 子生命周期。这是一条有界、经过审查的源路径，不实现模型 generator、模板 renderer 或新的 DSL 命令。

## 宿主集成

配置 `RuntimeConfig::child_synthesis_gateway` 和 `child_synthesis_grant_resolver`。resolver 必须用 `(owner_scope, parent_run_id, grant_id)` 从可信宿主策略选择精确 grant。从合成记录加载 grant 不会确立其权限。

1. 在父级下一个顶层 checkpoint 安排 `ProgramHandoff`。
2. 加载父 run、活动 lineage 和 generation，并按 [授权契约](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) 选择经过审查的源实例与 grant。
3. 保持 handoff，调用 `ProgramRuntime::prepare_child_synthesis(owner, parent, handoff, proposal, grant, binding_name)`。
4. 返回记录达到 `Bound` 后释放 handoff。
5. 父级在普通 `ng.spawn` / `ng.await` 路径使用该 binding name。

例如，已获准父级可以让出：

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

宿主决定 checkpoint 请求如何映射到经过审查的源和策略。checkpoint payload 不能给自己授予编译或执行权限。持久路径通过运行时事务管理预留，不调用 gateway 独立的 `reserve` 或 `reserve_child` 回调。

## 原子发布

`ProgramTransitionPublication::child_synthesis_records` 在与父 run 快照、journal head 和 lineage 同一事务中追加一条不可变记录。首条 `Reserved` 记录必须绑定精确的先前父快照、源 lineage、结果 lineage 和一单位动态编译扣减。冲突请求不能复用相同预算转换来为不同 binding 提供预算。精确发布重试返回 `AlreadyPresent`。

SQLite 使用现有 `BEGIN IMMEDIATE` 事务；PostgreSQL 使用现有事务和 owner advisory lock。两者都向 synthesis log 追加，并按追加顺序重建经过验证的 request heads。写入失败时父级、lineage 和合成记录一起回滚。内存参考实现保留强异常保证。

携带 synthesis records 的发布使用存储 schema 6。普通发布继续序列化为 schema 5；reader 保留 schema 1–5。`load_child_syntheses(owner, parent_run)` 返回当前 request heads。对于不支持该历史的自定义 store，基类实现采取失败关闭策略。动态预算运行的恢复要求此读取能力。

记录限制为 8 MiB 和 16 次修订。它们保留 proposal、原始宿主 grant 和父 context、reservation，以及累计阶段输出。规范身份和阶段验证拒绝更改的先前产物、不同 generation、重命名 binding 和修改的证据。binding name 在一个父 run 内唯一。SQL log 仅追加；大型历史的保留与压缩仍是后续工作。

## 阶段与恢复

| 持久状态 | 恢复后的下一步 |
|---|---|
| `Reserved` | 认领并编译一次 |
| `Compiling` | 标记 `ReconciliationRequired`；不重放不确定编译 |
| `Compiled` | 复用存储 bundle 并认领语义验证 |
| `Validating` | 标记 `ReconciliationRequired`；不重新运行不确定验证器 |
| `Validated` | 复用接受的回执与证据；选择准入策略 |
| `Admitting` | 复用冻结的准入及幂等 Catalog 准入 |
| `Admitted` | 使用范围化模块回执链接精确获准版本 |
| `Bound` | 恢复父级普通 child 命令 |
| `Dispatching` | 通过 `start_child` 复用记录的 child ID 与输入 |
| `Spawned` | 复用 child/result，或对缺少持久 checkpoint 的 Core 派发进行对账 |
| `Failed` / `ReconciliationRequired` | 保留结果并阻止自动父级重放 |

重连具有未完成合成的非活动父级前，调用 `recover_child_synthesis(owner, parent_run, proposal_id)`。活动父级要求使用持有 checkpoint 的 API。每次恢复都重新选择宿主权限并检查原始 generation。已完成语义验证及冻结准入会复用。语义拒绝保留回执和证据，绝不进入准入。

生成的 binding 按实际父 run 与 generation 解析，而非静态 resolver 的父版本范围。派发在 `start_child` 前记录稳定 child ID 和输入；一个 grant 不能创建第二次调用。普通子预算、发布、执行与 join 仍具有最终约束力。

父级在完成、重试和恢复时都保留 compile 扣减，包括提交确认丢失的情况。合成扣减已消耗墙钟时间；恢复 attempt 也保留原始合成 deadline。spawn 状态发布保留现有命令的在途预留。

## 验证与剩余工作

后端一致性测试覆盖成功 spawn/join、规范往返、owner/run 隔离、重复及并发请求、后端写失败回滚、已提交阶段复用、语义拒绝、撤销 grant 和不确定 compile/validation 认领。Linux 进程退出测试在冻结准入后不执行析构函数就终止，随后在 SQLite 与 PostgreSQL 上重新打开 Program store、transition store 和 checkpoint store，成功 join 子级。

[N3 恢复矩阵](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) 将覆盖扩展到每个数据库 16 个进程退出边界、并发进程恢复、确认丢失、真实 PostgreSQL 连接终止、取消、版本保留、激活和 Program 替换。限制仍明确：没有自动对账授权器；单子场景不覆盖一般混合效果 join、断电、跨宿主故障切换，以及所有图迁移/保留组合。

此变更增加公开 C++ 类型、虚方法和配置字段。重新构建全部 Program 使用方；不要将旧对象与新库混用。它不增加 Core-only 依赖，也不改变原生控制 C ABI。
