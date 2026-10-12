<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=zh-CN source_sha256=eee79303f2ce57761c1bf62d99755e7589782385c4ffa411c4d5c6ad4ad55207 -->
# 严格的运行时拦截

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph 的严格运行时路径，把强制的上下文、生命周期 Hook 和 provider 分发的证据，
从模型的自由裁量中拿了出来。直接发起带类型的 provider 调用，仍然可用于受信的嵌入场景；
`StrictRuntimeProfile` 把严格路径所需的依赖装配在一起。

## 保证的边界

```text
durable RAW history + admitted artifacts + required Skills/constraints
  -> immutable ContextEpoch
  -> RuntimeTurnAssembler
  -> ContextAssemblyReceipt
  -> mandatory BeforeProviderRequest Hooks
  -> durable ProviderDispatchReceipt
  -> provider
  -> ProviderDispatchOutcomeReceipt
  -> mandatory AfterProviderResponse Hooks
```

这项保证涵盖：上下文的精确构造、强制制品的存在、请求的标识、分发的准入(admission)，
以及已知的和需要对账的 provider 结果。它并不声称 LLM 关注或遵从了每一个 token。

宿主自己编写的自定义原生节点，仍然是受信的代码。给这样的节点一个原始的 `Provider`，
就有意地离开了严格 profile；生成出来的拓扑只能得到已注册的节点，无法凭空造出这种权限。

## 严格 profile

`StrictRuntimeProfileConfig` 要求具备：

- 一个 provider；
- 一个 `DurableContextStore`；
- 一个支持终态 outcome 的 `DurableProviderDispatchReceiptStore`；
- 一个 `HookRuntime`；
- 一个内容寻址的 provider 绑定标识；
- 一个非零的输入 token 上限；以及
- 可选的、确切的必需上下文和 Skill 制品标识。

只有 `RuntimeGuaranteeProfile::Strict` 的 epoch 才能被激活。把这个 profile 挂到
`GraphEngine` 上，会同时在内置的使用方上，装好 provider 拦截和生命周期 Hook。

## provider outcome 的生命周期

provider 边界现在会记录两个相互独立的不可变值：

1. `ProviderDispatchReceipt` 在分发之前写入。
2. `ProviderDispatchOutcomeReceipt` 在尝试之后，记录 `Succeeded`、`Failed` 或
   `ReconciliationRequired`。

成功的结果，会绑定完整的 SDK outcome 观测数据的摘要。
已证明未发送的带类型 Failure，记录为 `Failed`；送达状态不确定的，记录为 `ReconciliationRequired`。
分发之后发生异常，并不能说明远端 provider 是否已经执行，所以控制器不会悄悄重试。
SQLite schema v3 把终态回执单独存储，并在重启后检查它们与已准入分发之间的确切绑定。
回执摘要是证据，不是 native 续接的托管，也不是可以执行的、已存储的 outcome。

控制器接受 `ProviderRequest`，返回不可变的、持有所有权的 `sp::runtime::Result`，
其中保留有序的消息/parts 和部分失败的证据。如果在已有真实结果之后，结算或回执的持久化失败了，
`ProviderDispatchOutcomePersistenceError` 会保留这份结果和原始的 cause；
次要的 observer 失败仍保留在 `delivery_error()` 里。
token 的计费和预留，与可为空的 provider 用量报告是分开的。

## Program Core 的 provider 调用（与独立的严格运行时分开）

Program 使用内置的 Core LLM 节点时，宿主可以设置
`RuntimeConfig::core_provider_call_resolver` 和
`require_core_provider_call_broker = true`。对于每一个确切的
`ProgramCoreProviderCallContext`，从
`<neograph/program/sqlite_provider_call_broker.h>` 返回
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`；需要链接 `neograph::program_sqlite`。
部署标识必须是由宿主持有的、对实际 provider 路由、模型部署和权限（包括凭据版本）求出的 SHA-256 标识；
broker 不会从 `Provider` 对象中自行发现这些信息。重启或重新连接时，要重新绑定同一个持久数据库。

journal 以 owner、不可变的 Program 版本、运行、操作、Core 的线程/任务/节点以及内置调用的序号，
作为每次调用的键，**而不是**请求内容或 Program 的尝试。它在传输之前，用 SQLite FULL 同步提交标记。
这个标记表示传输*可能*已经发生，并不表示 provider 收到了它，也不表示副作用恰好发生了一次。
完整的、不可变的 SDK Completion/Failure outcome，以编码版本 2 存储，用于精确绑定的重放。
已证明未发送的 Failure 记录为 `Failed`；送达状态不确定、发生异常，或在结算之前崩溃，
都需要对账，并且绝不会悄悄重新分发。用
`inspect(owner, logical_call_id(context, core_identity))` 检查状态；
只有在拿到经独立验证的 provider 一侧证据和完整的 Completion outcome 时，
才能使用 `reconcile_success`。重放流式调用时，返回的是已捕获的 outcome，不会合成流事件。

更大的输出上限，是一次新的语义调用，不是传输层的重试，也不是对同一个 journal 槽位的重放。接口 4 只把这个上限从 native replay 的配置中排除；已准备请求的摘要和保守的资源预留额度仍然包含它。给每个已准入的调用一个不同的、确定性的序号，保留每次尝试的 outcome 和核算以及原来的截止时间，并从同一个资源 bank 获得准入。已有槽位里的摘要发生变化，会被拒绝；送达状态不确定、observer 失败或结算失败之后，native 历史和游标都既不能续期额度，也不能授权再次发送。

有序的消息 parts、原始观测、可为空的用量、尝试元数据和 native 续接状态，
仍然是已存储结果的一部分。native outcome 需要把宿主的 `sp::NativeArchive`
传给 `SQLiteProgramProviderCallJournal(database_path, native_archive)`；
可移植的 JSON 投影无法重建这种权限。旧的、有损的回执会被拒绝，而不是被升级，
也不会被悄悄再次分发。journal 把保守的预留额度/已提交的 token 数，与 provider 的报告分开保存。
请使用持久的文件系统数据库路径：空路径、`:memory:` 和 `file:` URI 路径都会被拒绝。
这个 broker 作用于 Core 已有的 ReAct 消息状态，而不是装配好的 `ContextEpoch`，
并且不能与引擎的严格运行时拦截用在同一个内置调用上。宿主自己编写的原生 Provider 调用，
不在它的范围之内。

## 通过 native、stdio 或 HTTP 运行的强制 Hook

`MandatoryHookRunner` 既接受现有的 native 适配器，也接受与传输方式无关的
`HookExecutionBackend`。`RpcHookExecutionAdapter` 把 `HookRpcExecutor` 绑定到该后端。
同一个固定的 `hooks/invoke` JSON-RPC 方法，既可以走 `StdioJsonRpcTransport`，
也可以走 `HttpJsonRpcTransport`。两者都运行在 `MCPClient` 会话之上，
所以 endpoint 是一个同时提供 `hooks/invoke` 的 MCP 服务器。这个方法是 NeoGraph 的扩展，
不属于 MCP；没有实现它的 MCP 服务器会回答 `-32601`。

RPC Hook 的制品是证据，绝不是权限。`ContextStoreHookArtifactPublisher`
只接受满足下列条件的制品：

- 类型是 `HookOutput`；
- `source_digest` 等于确切的 Hook 调用 ID；并且
- 运行时事件与该调用一致。

发布以 owner 为作用域，并且是幂等的。如果外部副作用已经成功，但它的制品无法发布，
该 Hook 就会结算为 `ReconciliationRequired`；它不会被报告为干净的成功。

## 必需的上下文与转换

`RuntimeContextRequirements` 把所有必需的制品 ID，与其中必须是 `RequiredSkill`
制品的那一部分分开。`HardConstraint` 是专门的必需制品类型。每个必需的制品，
都必须被当前生效的 epoch 选中，必须保持 `required=true`，并计入强制 token 的数量。

`ContextTransformReceipt` 在 v1 中有意保持保守。转换器可以替换或压缩可选的证据，
但每个必需的输入制品 ID，都必须以字节完全相同的形式出现在输出集合里。
改写不被接受为约束得以保留的证明。

## 运行时的开发者指令

`RuntimeDeveloperInstruction` 是不可变的开发者输入，不是权限。
`RuntimeInstructionController::submit_and_plan` 按下面的顺序执行：

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

封闭的决策集合是：

- `SatisfiedInPlace`；
- `Rejected`；
- `ReplaceAtHandoff`；以及
- `MigrateGraph`。

应用一次转换时，在委托给现有的 `ProgramRuntime::replace` 或 `migrate_graph`
路径之前，会立即重新检查谱系 head。过期的决策无法变成权限。

## 有界的 Program 合成

`ProgramSynthesisGateway` 提供由宿主持有的、生成后继者的路径：

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

预留必须表现为恰好扣减一个不可续期的 `max_dynamic_compiles` 单位，并且不得增加任何其他预算。
预留发生在编译之前，所以被拒绝的源码拿不回它的编译额度。语义验证是强制的，
在编译之后、准入解析器之前运行。它不可变的回执，绑定了提案、预留、编译好的 bundle、
验证器标识、语义契约标识、结论和证据摘要。被拒绝的结论会给出带类型的证据，
并且无法发布 `ProgramVersion`。网关从不激活、绑定、迁移或 spawn 它的结果。
这些仍然是宿主通过现有 Program API 做出的独立决定。

运行时的指令规划器可以调用这个网关，然后在替换或迁移的决策里，
返回确切的已准入版本。这样就保持了：

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

而无需把编译器、Catalog、凭据或激活权限暴露给生成出来的 JavaScript。
