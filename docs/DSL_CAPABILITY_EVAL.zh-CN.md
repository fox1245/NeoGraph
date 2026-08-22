<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=zh-CN source_sha256=a2b954c6e311d3acabd6fcc547511af245a49b2303e4897e12628ae7187191c4 -->
# QuickJS DSL能力与模型综合评估

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

状态：已实现，确定性一致性门控；实时模型评估为选择性加入 Observed: 2026-08-22

## 问题

NeoGraph必须区分两项主张：

1. 准入(admission)的QuickJS DSL可以表示一种能力；以及
2. LLM能够生成实际正确使用该能力的源代码。

第二项声明并非由第一项所蕴含。只有当真实的 `ProgramCompiler` 接受该source，且特定于用例的语义验证器确认了lowered的Core IR或密封的JavaScript command tree时，该source才通过此项评估。基于源码文本关键字匹配并不足以满足要求。

## Capability能力清单

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json) 是机器可读的评估清单。它目前覆盖：

- graph/channel/node/entry/edge/exit以及普通 JavaScript 构造；
- 已注册的条件路由；
- 静态 fan-out、fan-in 以及障碍；
- 静态 HITL 中断以及图重试策略；
- 注册表中介的动态`Send`与`NodeInterrupt`行为；
- `callCore` 使用普通 JavaScript 分支/循环控制；
- JavaScript 映射被降级为有界的 `ng.all`；
- `all`、`parallel`、通用`join`、`race`和`quorum`；
- 子 `spawn` 嵌套于 `await` 内；
- `emit`, `checkpoint`, and `cancelScope`; and
- 一个已准入(admission)的原生`hostCapability`导入槽位。

每个签入的JavaScript fixture均由`program_dsl_capability_probe`编译并执行语义验证。这些是确定性的CTest测试，无需模型或网络。

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

聚焦的图构建器回归测试同样证明重复的可变调用能正确累积，且barrier、interrupt和retry声明在降级后仍然存活：

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## 实时模型评估

可选runner（opt-in）使用自然语言语义和公共API签名请求源。它不会将已检入的答案提供给模型。每个响应都会发送到与确定性CTest相同的原生探针。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model deepseek/deepseek-v4-flash-0731 `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` 接受逗号分隔的子集，`--attempts` 重复独立的单次试验，`--repair-attempts` 将权威探针诊断连同被拒绝的完整来源一起返回给模型。Provider/response 失败与编译或语义拒绝保持分离。

## 观察到DeepSeek结果

在初次运行和精确标识符重新评估之间，模型为所有11个能力组产生了探针验证的源。静态HITL/retry需要一次诊断引导的修复。结构化并发需要两次修复：第一次恢复ES模块导出，第二次将错误的Core绑定替换为精确的准入(admission)名称。控制流在一次精确绑定修复后通过。Map在一次通过后通过，此前宿主提供了原生API清单并明确陈述了准入(admission)的Core标识符。

| 能力用例 | 模型证据 | 重要观察 |
|---|---|---|
| `graph_basics` | 已通过 | 循环构建的节点正确降级 |
| `graph_routing` | 在重试尝试中通过 | 早前输出使用了被禁止的CommonJS/`require` |
| `graph_fanout_barrier` | 已通过 | fan-out边和屏障成员资格完全匹配 |
| `graph_hitl_retry` | 经过一次修复后通过 | 初始输出使用了错误的节点名称 |
| `registry_mediated` | 已通过 | 模型正确引用了主机准入(admission)的动态节点 |
| `program_control_flow` | 经过一次修复后通过 | 早前的试验反复调用`core`/`Core`，而非已准入(admission)的Core`capability` |
| `program_map` | 在精确标识符注入后通过 | 早前的试验使用了CommonJS、`yield*`和错误的Core名称 |
| `program_structured_concurrency` | 经过两次修复后通过 | 精确的嵌套命令和Core绑定已得到验证 |
| `program_spawn_await` | 已通过 | `Await(Spawn(...))`和超时在结构上得到验证 |
| `program_durability` | 已通过 | Emit、检查点和取消命令完全匹配 |
| `program_host_capability` | 已通过 | 导入槽位和规范输入匹配 |

初始验证器对 `callCore` 案例产生了误报，因为它检查了命令种类和输入，但未检查确切的 Core 名称。验证器已加强，要求每个嵌套的 `capability` 都包含 `callCore`。因此，之前使用 `core`, `Core`或节点名称 `work` 的已接受模型源现在会被正确拒绝。

这是能力证明，而非统计可靠性声明。逐案例的一次性成功率和修复成功率仍需通过重复试验来评估，且提供商失败须单独报告。

## 对生成的Program的后果

原始一次性源生成不是足够的产品保证。最低安全合成路径是：

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

模型反复混淆ES模块与CommonJS、图名称与节点名称，以及请求的Core身份与诸如`core`之类的通用词。因此，NeoCode应注入精确签名和准入(admission)的标识符，在可能的情况下保留固定的模块脚手架，并且绝不应将看似合理的源视为已构建请求拓扑的证据。

NeoGraph 现在在 `ProgramSynthesisGateway`中强制执行此边界：每个网关配置必须提供宿主拥有的语义验证器。成功的验证会在 Catalog 准入(admission)之前生成内容寻址的收据；被拒绝的决策会抛出 `ProgramSynthesisValidationError`，保留确切的证据，并且绝不调用准入(admission)解析器。已消耗的动态编译预留仍然保持已消耗状态。

`javascript_authoring_capability_manifest()` 以机器可读数据的形式暴露已安装的graph-builder和命令词汇表、精确签名、分类、限制及profile约束。一致性测试将该清单与QuickJS两个上下文中实际安装的属性进行比较，从而API漂移会使测试套件失败。
