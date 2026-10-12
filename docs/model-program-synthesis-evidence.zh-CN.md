<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=zh-CN source_sha256=c34041ccc8bf5143dd152f292a26c02eb4a487757b4657e0ecfe75b4740a3373 -->
# 模型生成的 QuickJS Program 合成证据

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- 状态：历史上的有界 PoC 证据；不是对当前绑定的新验证
- 观测日期：2026-08-21
- 模型：通过 OpenRouter 使用的 `deepseek/deepseek-v4-flash-0731`
- Provider 响应：`gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 已验证的链路

一次模型响应产生了下面这份 QuickJS Program 源码：

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const pairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of pairs) {
        graph.node(name, {type});
    }
    graph.entry("seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    return yield ng.callCore("model-synthesized", input, "model-generated:1");
}
```

宿主没有直接执行这份提案。探针强制执行的是：

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

已提交到仓库的夹具，以确定性的方式验证同一条宿主流水线。它的持久标识如下：

| 证据 | 标识 |
|---|---|
| Program 源码 | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| 提案 | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 预留 | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| Bundle | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 合成回执 | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

Catalog 查找到了确切的已准入版本。Program 执行完成，trace 为 `seed -> double -> finish`；
每个节点都恰好运行了一次。最终的通道值是 `value = 12` 和 `path = "model-generated"`。

## 运行时的拓扑替换

合成网关现在在宿主持有的 `ProgramBudgetBounds` 之下，编译后继者。每种资源的最大值，
是预留的 `remaining_after_reservation`；因此它是一个权限上限，来自已经扣减过的谱系，
而不是来自模型的输出。下界只是 JavaScript 运行时的结构性下限：一个挂钟时间单位、
一个 worker、一个 Program 操作和一个 Core 步骤。可消耗的授权和子授权，下限保持为零。
宿主给出的上下界颠倒了，会在源码求值之前被拒绝。

这样，一次精确的替换，就可以带着它更小的、已扣减挂钟时间的谱系剩余额度，
而不会放宽任何不可续期的预算。普通的精确预算编译器重载，仍然可用于固定的独立调用。

另一个来源 Program，走到了一个持久的顶层 `ng.checkpoint`。宿主消耗了那次交接，
并以模型生成的拓扑作为后继者，调用了 `ProgramRuntime::replace`。目标就是上面合成网关所产生的
那个确切的 ProgramVersion；没有编译或准入第二个 bundle。夹具的这次转换产生了：

| 证据 | 值 |
|---|---|
| 来源 ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| 来源运行 | `run-e153d50d90cc8d222c5f363c99399569` |
| 目标运行 | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| 目标 ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 替换回执 | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| 当前生效的 generation | `2` |
| 目标状态 | `completed` |

过期的来源节点运行了零次。后继者的 trace 是 `seed -> double -> finish`，后继者的每个节点都运行了一次，
最终输出再次包含 `value = 12` 和 `path = "model-generated"`。

最近一次真实调用 DeepSeek 的运行，报告了 `replacement_uses_synthesis_version =
true`、目标状态 `completed`、当前生效的 generation 为 `2`，以及同样的“过期节点运行零次”的结果。

## 负面证据与提示词约定

更早的模型输出，在执行之前就被拒绝了：

- `P_JS_DEFINE_MISSING`：没有同步导出的 `define()`；
- `P_JS_DEFINE_VALUE`：`define()` 返回的是形似图的普通数据，而不是不透明的 `ng.graph()` 构建器；
- `P_JS_GRAPH_ARGUMENT`：通道/节点构建器的参数，与经过审阅的 DSL schema 不符；以及
- `P_JS_EVALUATION`：一个没有加引号的 reducer 标识符，引用了在有界 QuickJS 上下文中并不存在的环境状态。

因此，成功的提示词，明确规定了确切的受信编写接口：`ng.graph`、带 `initial` 的 `graph.channel`、
`graph.node(name, {type})`、entry、edge、exit，以及封存的 `ng.callCore` 命令。
无效的提案，不会产生任何 ProgramVersion，也不会运行任何节点。

## 范围边界

这证明了，一个外部模型可以合成 QuickJS 的拓扑源码，这份源码随后经过预留、编译、准入、发布、执行，
并在持久的运行时检查点处，被选作另一个 Program generation。它还进一步证明，
合成网关自己准入的那个版本，可以在扣减动态编译额度之后成为这个后继者；
运行时既不会把提案当作权限重新使用，也不会在替换路径上重新编译源码。

这种 Program 层面的替换，不能与任意的 GraphEngine 状态/前沿迁移混为一谈。
从来源 Core 拓扑迁移到模型拓扑的迁移计划，被正确地归类为 `blocked`，因为两者的物化方式和运行时契约不同。

NeoGraph 现在还有一条有意收得很窄的 P1 GraphEngine 路径：
`GraphSemanticMigrationAdapter`。宿主必须根据确切的、已准入的来源和目标制品，准备这个不可变的适配器。
它只准入这样的 Program：只有声明（没有运行时的 JavaScript 控制）、单根的 `call_core`，
并且检查点中的通道/reducer、节点名、边、路由、屏障、重试/中断的形态、能力绑定、权限，
以及输入/输出契约，全都相同。因此，它可以把一个按标识映射的前沿和通道快照，
带入一个封存 Core 定义和编译计划标识都不同的后继者。适配器存放在迁移回执里，并在恢复时重新校验。

QuickJS 控制、节点/前沿的改名、通道或 reducer 的转换、屏障成员的变化、待处理的副作用、
子 Program，以及任意的拓扑编辑，仍然是失败时关闭的。这些情形仍然需要显式的交接/重启，
直到之后某类映射证明了每一个受影响的状态维度。自动的子绑定/spawn、覆盖每个合成边界的崩溃恢复，
以及 Program 内部的 `ng.proposeProgram` 命令接口，同样仍是各自独立的资格认证闸门。

## 模型生成的 P1 GraphEngine 迁移

P1 适配器还用真实的 OpenRouter 响应 `gen-1787291529-fCOHp8pry7EwHHHu1MUH`
（`deepseek/deepseek-v4-flash-0731`），做了端到端的演练。模型生成了一份只有声明的
QuickJS `define()` 源码（SHA-256
`346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`），
它的拓扑保持了来源图 `work -> followup` 的前沿形态，同时通过 `migration_epoch: 2`
引入了一个不同的目标 Core 定义。

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

对于 bundle/物化发生了变化的情形，普通的迁移计划仍然是 `blocked`，这正是应有的结果。
随后，由宿主创建的适配器，准入了这条窄的恒等投影。目标在 generation `2` 完成；
`work` 在来源 generation 上运行了一次，`followup` 在后继者上运行了一次。
确切的适配器标识，持久化在迁移回执里。

要复现，使用 `program_model_synthesis_probe` 目标，并执行：

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

对于 GraphEngine 的 P1 路径，使用 `program_model_semantic_migration_probe`，并执行：

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
