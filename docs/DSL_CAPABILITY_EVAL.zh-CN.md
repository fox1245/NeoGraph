<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=zh-CN source_sha256=7ac0e8bb68d88e27682b67247d72e6d408b05e9f23c6ff59967e81cf9f6b3b30 -->
# QuickJS DSL能力与模型综合评估

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

状态：已实现，受确定性一致性检查约束；实时模型评估为 opt-in。
历史实时观测：2026-08-22；下列记录不是新的运行结果。

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
Runner 将 `skills/neograph-harness-authoring/SKILL.md` 及其
`references/quickjs-authoring.md` 放入模型 context，并在 report 中记录
guidance digest。`--skill` 选择带同一 companion reference 的其他 entrypoint。
宿主调用实际 compiler bridge；模型返回 source，再接收有限 repair 所用的
diagnostic。Skill 不授予 runtime 权限。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model z-ai/glm-5.3-flash `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` 接受逗号分隔的子集，`--attempts` 重复独立的单次试验，`--repair-attempts` 将权威探针诊断连同被拒绝的完整来源一起返回给模型。Provider/response 失败与编译或语义拒绝保持分离。
默认模型为 `z-ai/glm-5.3-flash`。每次 generation 限制为 4,096 个
completion token，采用 cookbook 的 ZDR provider-routing 设置。
下面的 DeepSeek 证据是该 skill-loading 配置之前的历史记录。

## 单独评估指令

Source case 通过衡量的是 skill、提供的 native API reference、case contract
和 model 配置的组合，并不证明 skill 单独就足够。Runner 保留 system prompt、
API reference、raw model content、usage、stop reason 及 strict-envelope
结果。Legacy source extractor 可以恢复 fenced JSON；`strictEnvelope` 将其
与遵守所请求的 raw JSON envelope 区分。

仅在明确选择独立比较 profile 时使用 `--reasoning-effort`。省略时保留
provider default。Before/after 比较固定该值、model、sampling、output cap、
task 和 compiler。

Chatbot 内部 Harness selector 的 paired evaluator 为：

~~~powershell
bun run scripts/run_harness_skill_ab.ts --before saved-skill/SKILL.md `
  --after skills/neograph-harness-authoring/SKILL.md `
  --provider relace --repeats 2 --output skill-comparison.json
~~~

两个 directory 均须有 `references/chat-template-proposals.md`。默认 fixture
包含 greeting、正在进行的独立 review 请求和 recorded review scenario。
Expected plan 不进入模型输入。Evaluator 固定 live chat profile，交替 AB/BA
顺序，保留 raw response，并将 JSON shape 与 expected plan/confidence 分开评分。
`--provider` 固定 backend 并关闭 fallback；省略时 routing policy 相同，但实际
provider 可能不同，返回的 provider/model 名称会被记录。Case 与重复次数有限，
不做 repair 或暗中 retry。

这是 prompt-level 评估，不是 runtime admission，也不是关于答复品质的统计
主张。少量 targeted 重复样本是 regression 证据，不是一般 model 可靠性的估计。

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

初始 validator 只检查 command kind 和 input，没有检查精确 Core 名称，因此在 `callCore` case 中产生误报。后来要求每个嵌套 `callCore` 的名称为 `capability`。之前通过的、使用 `core`、`Core` 或 node 名称 `work` 的 model source 现在被拒绝。

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
