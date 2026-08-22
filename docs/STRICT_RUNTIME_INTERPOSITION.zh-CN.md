<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=zh-CN source_sha256=59193d3d0f34fd9e49284edb43ce62f5ec0352c27fdcfce47bf1dceec7f9454a -->
# 严格运行时介入

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph 的严格运行时路径将强制性上下文、生命周期 Hook 和提供方分派证据移出模型自由裁量范围。它是增量式的：为受信任的嵌入场景，遗留直接 provider 调用仍然存在，而 `StrictRuntimeProfile` 则负责组装严格路径所需的依赖。

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

成功的结果绑定规范化补全的摘要。分发之后的异常无法证明远程提供者是否已执行操作，因此控制器记录`ReconciliationRequired`而不是重试。SQLite schema v3单独存储结果，并在重启后验证每个结果仍然绑定精确的已准入分发回执。

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
