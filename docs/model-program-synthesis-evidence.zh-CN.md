<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=zh-CN source_sha256=b0a57aa0f6c4a726e417d88daf8d0e3f90178de20d610884fcba052b0f622105 -->
# 模型生成的QuickJS Program合成证据

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- 状态：有界PoC已验证
- 观察时间：2026-08-21
- 模型：`deepseek/deepseek-v4-flash-0731` 通过OpenRouter
- 提供商响应：`gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 已验证链

一个模型响应生成了此QuickJS Program源码：

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

主机未直接执行此提案。探针强制执行：

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

检查入库的测试夹具以确定性方式验证相同主机管线。其持久化的身份标识为：

| 证据 | 身份 |
|---|---|
| Program 源码 | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| 提案 | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 预留 | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| 包 | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 合成凭证 | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

目录查找找到了已被准确准入的版本。Program 执行完成并附带轨迹 `seed -> double -> finish`；每个节点正好运行一次。最终通道为 `value = 12` 和 `path = "model-generated"`。

## 运行时拓扑替换

合成网关现在在宿主拥有的 `ProgramBudgetBounds` 下编译后继程序。每个资源的最大值即预留的 `remaining_after_reservation`；因此，它是源自已扣减谱系的权威上限，而非模型输出。下限仅为 JavaScript 运行时结构下限：一个墙钟时间单位、一个 worker、一次 Program 操作以及一个 Core 步骤。可消耗和子级授权保留零下限。倒置的主机边界在源码求值前即被拒绝。

这允许精确替换携带其更小的、墙钟时间已扣减的谱系余量，而无需扩大任何不可再生预算。常规的精确预算编译器重载仍可用于固定的独立调用。

一个不同的源 Program 到达了持久化顶层状态 `ng.checkpoint`。宿主消耗了该交接，并以模型拓扑作为后继调用了 `ProgramRuntime::replace`。目标是上述合成网关的精确 ProgramVersion；未编译或准入任何第二束。夹具转换产生了：

| 证据 | 值 |
|---|---|
| 源 ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| 源运行 | `run-e153d50d90cc8d222c5f363c99399569` |
| 目标运行 | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| 目标 ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 替换收据 | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| 当前代次 | `2` |
| 目标状态 | `completed` |

过时的源节点运行了零次。后继轨迹为 `seed -> double -> finish`，每个后继节点运行一次，最终输出再次包含 `value = 12` 和 `path = "model-generated"`。

最新的实时DeepSeek运行报告了`replacement_uses_synthesis_version =
true`，目标状态为`completed`，活动生成为`2`，并且结果与零陈旧节点一致。

## 负面证据与提示契约

早期模型输出在执行前被驳回：

- `P_JS_DEFINE_MISSING`：没有同步导出的`define()`；
- `P_JS_DEFINE_VALUE`：`define()` 返回了普通的图形状数据，而非不透明的 `ng.graph()` 构建器；以及
- `P_JS_GRAPH_ARGUMENT`：通道/节点构建器参数与审查过的 DSL 架构不匹配；以及
- `P_JS_EVALUATION`：一个未加引号的 reducer 标识符引用了在受限 QuickJS 上下文中不存在的环境状态。

因此，成功的提示指定了精确的受信任创作表面：`ng.graph`、`graph.channel` with `initial`、`graph.node(name, {type})`、entry、edge、exit，以及一个密封的 `ng.callCore` 命令。无效提案不会产生 ProgramVersion，也不会运行任何节点。

## 作用域边界

这证明了外部模型能够合成 QuickJS 拓扑源代码，该源代码随后在持久化运行时检查点被保留、编译、准入、发布、执行，并被选择为不同的 Program 代次。这进一步证明了合成网关自身的准入版本能够在该动态编译扣费后成为该后继版本；运行时既不会将提案重新用作权威，也不会在替换路径上重新编译源代码。

这个 Program 级替换不得与任意 GraphEngine 状态/前置边界迁移相混淆。从源 Core 拓扑到模型拓扑的迁移计划被正确分类为 `blocked`，因为其物化方式和运行时契约不同。

NeoGraph 现在也拥有一条刻意收窄的 P1 GraphEngine 路径：`GraphSemanticMigrationAdapter`。宿主必须从准确的已准入源工件和目标工件准备这个不可变适配器。它仅准入随后列的 Program：仅声明式（无运行时 JavaScript 控制）、单根 `call_core`、具有相同的已检查点通道/reducer、节点名称、边、路由、barrier、重试/中断形状、能力绑定、权限以及输入/输出契约。因此，它可以携带一个身份映射的前沿和通道快照进入具有不同密封 Core 定义和编译后计划身份的后续版本中。该适配器存储在迁移收据中，并在恢复期间重新验证。

QuickJS控制、节点/边界重命名、通道或reducer转换、更改的屏障成员资格、待处理效果、子项以及任意拓扑编辑仍然失败时关闭。这些情况继续需要显式交接/重启，直到后续映射类证明每个受影响的状态维度。自动子绑定/生成、跨每个合成边界的崩溃恢复，以及Program内`ng.proposeProgram`命令面仍然是单独的资格门槛。

## 模型生成的 P1 GraphEngine 迁移

P1适配器也通过一个实时的`deepseek/deepseek-v4-flash-0731` OpenRouter响应`gen-1787291529-fCOHp8pry7EwHHHu1MUH`进行了端到端测试。模型生成了一份仅声明的QuickJS `define()`源码（SHA-256`346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`），其拓扑结构保留了源图的`work -> followup`边界形状，同时通过`migration_epoch: 2`引入了一个独特的目标Core定义。

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

普通迁移计划保持`blocked`，这对于更改的包/实体化是必需的。宿主创建的适配器随后准入(admission)了窄恒等投影。目标在生成`2`处完成；`work`在源生成中运行了一次，`followup`在后续生成中运行了一次。确切的适配器身份被持久化在迁移收据中。

使用 `program_model_synthesis_probe` 目标及以下内容进行复现：

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

对于 GraphEngine P1 路径，请使用 `program_model_semantic_migration_probe`，如下：

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
