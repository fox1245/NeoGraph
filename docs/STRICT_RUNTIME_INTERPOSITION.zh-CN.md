<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=zh-CN source_sha256=9b872c2d049d049aa2c5e7393b485bc79c3b89a5268878b1fff5a263c347208f -->
# 严格运行时介入

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph 的严格运行时路径将必需上下文、生命周期 Hook 和 provider dispatch 证据从模型自由裁量中分离。受信任的嵌入场景仍可直接调用 typed provider；`StrictRuntimeProfile` 组装严格路径所需的依赖。

## 保证边界

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

该保证涵盖精确上下文构建、强制性工件存在性、请求身份、准入(admission)分派，以及已知/需要对账的provider提供方结果。它并不声称LLM关注到或遵守了每个令牌。

宿主编写的自定义原生节点仍是受信任代码。将原始 `Provider` 交给此类节点，使 node 有意离开严格配置文件；由生成的 Topology 拓扑仅能获得已注册节点，并且无法伪造这种该 authority 权限。

## 严格配置文件

`StrictRuntimeProfileConfig` 需要具备：

- 一个 provider；
- a `DurableContextStore`;
- 一个支持终止结果的`DurableProviderDispatchReceiptStore`；
- a `HookRuntime`;
- 一个内容寻址的提供者绑定身份；
- 一个非零最大 input token；及
- 可选的精确必需上下文和Skill工件标识。

只有 `RuntimeGuaranteeProfile::Strict` epoch 才能被激活。将配置文件附加到 `GraphEngine` 会在内置消费者上安装提供者介入（provider interposition）和生命周期 Hook。

## provider 结果生命周期

提供者边界现在记录两个独立的不可变值：

1. `ProviderDispatchReceipt` 在分派之前写入。
2. `ProviderDispatchOutcomeReceipt` 在尝试之后记录`Succeeded`、`Failed`或`ReconciliationRequired`。

成功结果绑定完整 SDK outcome 观测的 digest。已证明未发送的 typed Failure 记录为 `Failed`；不确定的交付记录为 `ReconciliationRequired`。dispatch 后的异常不能确定远程 provider 是否执行，因此控制器不会暗中重试。SQLite schema v3 单独存储 terminal receipt，重启后检查其精确 admitted dispatch binding。Receipt digest 是证据，不是 native continuation custody 或可执行的已存 outcome。

控制器接收 `ProviderRequest`，返回不可变、拥有所有权的 `sp::runtime::Result`，保留顺序 message/part 和部分失败证据。实际结果之后若结算或 receipt 持久化失败，`ProviderDispatchOutcomePersistenceError` 保留结果与原始 cause；次要 observer 失败保留在 `delivery_error()` 中。Token charge/reservation 与 nullable provider 用量 report 分开。

## Program Core provider 调用（独立于 standalone Strict Runtime）

Program 使用内置 Core LLM node 时，宿主可设置
`RuntimeConfig::core_provider_call_resolver` 和
`require_core_provider_call_broker = true`。对每个精确的
`ProgramCoreProviderCallContext`，返回
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`。
头文件为 `<neograph/program/sqlite_provider_call_broker.h>`，链接目标为
`neograph::program_sqlite`。Deployment identity 是宿主拥有的 SHA-256 identity，
绑定真实 provider route、model deployment 和权限（包括 credential version）。
Broker 不会从 `Provider` 猜测这些值。重启/reconnect 时重新绑定同一 durable database。

Journal 以 owner、不可变 Program version、run、operation、Core
thread/task/node 和内置 call ordinal 为键，不以 request 内容或 Program
attempt 为键。传输前以 SQLite FULL 同步 commit marker。Marker 表示可能发生了
传输，不证明 provider 收到请求或效果恰好执行一次。完整不可变 SDK
Completion/Failure outcome 以 encoding version 2 存储，用于精确绑定的 replay。
已证明未发送的 Failure 记录为 `Failed`；不确定交付、异常或结算前 crash
需要 reconciliation，绝不暗中重新 dispatch。用
`inspect(owner, logical_call_id(context, core_identity))` 检查状态。
只有独立确认的 provider-side 证据和完整 Completion outcome 才可用于
`reconcile_success`。Streaming replay 返回 captured outcome，不合成 stream event。

更大的 output cap 是新的 semantic call，不是同一 journal slot 的 transport retry 或 replay。Interface 4 仅从 native replay configuration 中移除 cap；prepared-request digest 和保守 resource claim 仍包含它。每个准入 call 必须有独立确定性 ordinal，保留每次 attempt 的 outcome/accounting 和原始 deadline，并从同一 resource bank 获得 admission。既有 slot 的 digest 变化会被拒绝；native history 或 cursor 不续期 credit，也不授权在 uncertain delivery、observer 或 settlement 失败后重新发送。

顺序 message part、raw 观测、nullable 用量、attempt metadata 和 native
continuation 均保留在存储结果中。Native outcome 需要宿主向
`SQLiteProgramProviderCallJournal(database_path, native_archive)` 提供
`sp::NativeArchive`。Portable JSON projection 不能重建该权限。旧的 lossy
receipt 会被拒绝，不会升级或暗中重新 dispatch。Journal 将保守 claim/committed
token 数量与 provider report 分开保存。使用 durable filesystem database path；
空路径、`:memory:` 和 `file:` URI 均被拒绝。

该 broker 使用 Core 现有的 ReAct message state，而非组装的 `ContextEpoch`。
同一内置调用不能同时使用 engine Strict Runtime interposition。
宿主编写的 native Provider 调用不在其范围内。


## 对native、stdio或HTTP的强制Hook

`MandatoryHookRunner`接受现有的原生适配器或传输中立的`HookExecutionBackend`。`RpcHookExecutionAdapter`将`HookRpcExecutor`绑定到该后端。同一个固定的`hooks/invoke` JSON-RPC方法可以使用`StdioJsonRpcTransport`或`HttpJsonRpcTransport`。

RPC Hook工件是证据，而非权威。`ContextStoreHookArtifactPublisher`仅接受满足以下条件的工件：

- 类型为`HookOutput`；
- `source_digest`等于精确的Hook调用ID；且
- 运行时事件与调用匹配。

发布是所有者作用域内的且幂等的。如果外部效果已成功但其工件无法发布，则Hook结算为`ReconciliationRequired`；它不会被报告为完全成功。

## 必需的上下文与变换

`RuntimeContextRequirements`将所有必需的工件ID与必须为`RequiredSkill`工件的子集分开。`HardConstraint`是一种专用的必需工件类型。每个必需工件都必须在活动纪元中被选中，必须保留`required=true`，并贡献给强制令牌计数。

`ContextTransformReceipt`在v1中刻意采取保守策略。变换器可以替换或压缩可选证据，但每个必需的输入工件ID必须按字节完全一致地出现在输出集中。改写不视为约束保持的证明。

## 运行时开发者指令

`RuntimeDeveloperInstruction` 是不可变的开发者输入，而非权威。`RuntimeInstructionController::submit_and_plan` 执行此顺序：

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

已关闭的决策为：

- `SatisfiedInPlace`;
- `Rejected`;
- `ReplaceAtHandoff`；以及
- `MigrateGraph`.

应用转换时，会在委托给现有`ProgramRuntime::replace`或`migrate_graph`路径之前立即重新检查谱系头部。过期的决策不能成为权威。

## 有界Program合成

`ProgramSynthesisGateway`提供宿主拥有的生成后继路径：

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

预留必须显示一个不可再生`max_dynamic_compiles`单元的精确递减，且不得增加任何其他预算。预留发生在编译之前，因此被拒绝的源代码不会收回其编译单元。语义验证是强制性的，在编译之后但在准入(admission)解析器之前运行。其不可变收据绑定提案、预留、编译后的捆绑包、验证器身份、语义合约身份、裁决和证据摘要。被拒绝的裁决暴露类型化证据，且不能发布`ProgramVersion`。网关永远不会激活、绑定、迁移或生成其结果。这些仍是通过现有Program API进行的独立宿主决策。

运行时指令规划器可以调用网关，然后在替换或迁移决策中返回确切的已准入版本。这保留了：

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

而不向生成的JavaScript暴露编译器、Catalog、凭据或激活权限。
