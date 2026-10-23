<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=zh-CN source_sha256=8629673a4ffa2bad19d7820ef967e71430cbd2f853599858f6a6d053e70af8af -->
# QuickJS DSL 的能力与模型合成评测

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

## 问题

NeoGraph 必须区分两个论断：

1. 已准入的 QuickJS DSL 能够表达某种能力；以及
2. LLM 能够合成出真正正确地用到该能力的源码。

第二个论断并不能由第一个推出。只有真正的 `ProgramCompiler` 接受了某份源码，
并且针对该用例的语义验证器确认了降级后的 Core IR，或封存的 JavaScript 命令树，
这份源码才算通过评测。仅靠对源码文本做关键字匹配是不够的。

## 能力 manifest

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json)
是机器可读的评测清单。目前它覆盖：

- graph/channel/node/entry/edge/exit，以及普通的 JavaScript 构造；
- 已注册的条件路由；
- 静态的 fan-out、fan-in 和屏障；
- 静态的 HITL 中断和图的重试策略；
- 由注册表介导的动态 `Send` 和 `NodeInterrupt` 行为；
- 带有普通 JavaScript 分支/循环控制的 `callCore`；
- 降级为有界 `ng.all` 的 JavaScript map；
- `all`、`parallel`、通用的 `join`、`race` 和 `quorum`；
- 嵌套在 `await` 里的子 `spawn`；
- `emit`、`checkpoint` 和 `cancelScope`；以及
- 一个已准入的原生 `hostCapability` import 槽位。

每个已提交到仓库的 JavaScript 夹具，都由 `program_dsl_capability_probe`
编译并做语义验证。这些是确定性的 CTest 测试，不需要模型，也不需要网络。

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

针对图构建器的专项回归测试，还证明了重复调用修改方法能够正确累积，
以及屏障、中断和重试的声明在降级之后依然保留：

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## 实机模型评测

这个需要主动开启的 runner，用自然语言描述语义，再加上公开的 API 签名，来要求模型给出源码。
它不会把仓库里已提交的答案给模型。每个响应都会送到确定性 CTest 所用的同一个原生探针。
runner 会把 `skills/neograph-harness-authoring/SKILL.md` 及其
`references/quickjs-authoring.md` 加载到模型的上下文中，并在报告里记录这份指导的摘要。
`--skill` 可以选择另一个带有这份配套参考的入口。宿主调用真正的编译器桥；
模型返回源码，并收到诊断信息，用于有限次的修复。这个 skill 不授予任何运行时权限。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model z-ai/glm-5.3-flash `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` 接受以逗号分隔的子集，`--attempts` 重复进行互相独立的一次性试验，
`--repair-attempts` 把权威的探针诊断，连同被拒绝的完整源码，一起返回给模型。
provider 或响应层面的失败，与编译或语义层面的拒绝是分开记录的。
默认的模型是 `z-ai/glm-5.3-flash`；每次生成最多 4,096 个 completion token，
并使用 cookbook 里的 ZDR provider 路由设置。下面的 DeepSeek 结果，
是在运行器加载 skill 之前记录的。

`--provider anthropic` 会把请求发往 Anthropic Messages API，并使用 `ANTHROPIC_API_KEY`；默认仍然是
`openrouter`。部分当前模型会拒绝 `temperature` 参数，所以除非指定了 `--temperature`，否则不会发送它；
它也不接受 `--reasoning-effort`，那是 OpenRouter 的设置。
`--dry-run` 在没有密钥、也不发起网络调用的情况下，原样打印首轮和修复轮的请求，其中的机密请求头的值
会被隐去（它仍会运行 `--probe` 来读取原生清单）。没能拿到可用回复的失败（网络、限流、过载、请求被拒）
会记为 `failureKind: "provider"`，可以用 `--provider-retries` 重试；回复不是可用的源码封装，则记为
`model_output`。报告中的 `summary.usage` 汇总了包括修复轮在内的每一次回复的输入和输出 token。

## 单独评测这些指令

一个源码用例通过，衡量的是 skill、提供的原生 API 参考、用例契约和模型配置合在一起的效果。
它并不能证明 skill 本身就足够了。runner 会在它的证据里，保留系统提示词、API 参考、
模型的原始内容、用量、停止原因和严格信封的结果。
旧的源码提取器可能会从带围栏的 JSON 里把内容恢复出来；strictEnvelope 把这种情况，
与符合所要求的纯 JSON 信封区分开。

只有在明确选用单独的对比 profile 时，才使用 --reasoning-effort。省略它，则沿用 provider 的默认值。
在每一组前后对比之内，要保持该值、模型、采样、输出上限、任务和编译器都不变。

对于聊天机器人内部的 Harness selector，成对的评测器是：

~~~powershell
bun run scripts/run_harness_skill_ab.ts --before saved-skill/SKILL.md `
  --after skills/neograph-harness-authoring/SKILL.md `
  --provider relace --repeats 2 --output skill-comparison.json
~~~

两个目录都必须包含 references/chat-template-proposals.md。默认的夹具包括：一句问候、
一个持续进行独立审阅的请求，以及一个已记录的审阅场景。期望的计划不会出现在模型的输入里。
评测器固定实机聊天的 profile，交替使用 AB/BA 顺序，保留原始响应，
并把 JSON 的形态与期望的计划/置信度分开评分。
--provider 会固定一个后端并关闭回退；不指定时，路由策略是共享的，但实际的 provider 可能不同。
返回的 provider 和模型名都会被记录。用例和重复次数都有上限；不做任何修复，也不做悄悄的重试。

这是提示词层面的评测，不是运行时的准入，也不是关于回答质量的统计结论。
针对某个用例做的少量重复采样，是回归证据，而不是对模型总体可靠性的估计。

## 当前结果（2026-10-23）

通过率无法区分各组。在有运行器的 API 参考和用例文字的情况下，`claude-haiku-5-5` 无论使用以前的 skill、
重写后的 skill，还是完全没有 skill 文本，都在 110 次试验中的 110 次于首次尝试写出了被编译器接受、
并经用例验证器确认的源码。这项测量已经触顶（ceiling），所以既无法证明 skill 有帮助，也无法证明重写有帮助。

设置：通过 Anthropic Messages API 使用 `claude-haiku-5-5`，`cases.json` 中的 11 个用例，每个对照组（arm）
运行 2 次，每次对每个用例做 5 次互相独立的一次性试验（每组 110 次），`--repair-attempts 2`，每次生成的
`max_tokens` 为 4,096（含思考）。该模型的 API 会拒绝 `temperature`，所以没有发送任何采样参数：模型按
provider 默认值采样，并使用自适应思考，因此各次试验是真正的样本。方案、留出（held-out）用例（按用例 id
排序的第一个、中间一个和最后一个：`graph_basics`、`program_durability`、`registry_mediated`）和分析方法，
在第一次试验之前就写进了一个不入库的本地文件。不带 skill 文本的对照组和每组的第二次运行，是在首次运行
显示已触顶之后，以补充说明的形式加入的。在修订最终确定之前，没有读过留出用例的源码和诊断。没有请求需要
重试，也没有试验以 provider 失败告终。

要重复一次运行：

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --provider anthropic --model claude-haiku-5-5 `
  --attempts 5 --repair-attempts 2 --provider-retries 5 `
  --skill path\to\SKILL.md `
  --output evidence.json
```

密钥从环境中的 `ANTHROPIC_API_KEY` 读取（这里由 env 文件设置），不会出现在任何输出中。“本次修改之前的
skill”是本次修改的基准提交上的 skill；不带 skill 文本的对照组是一个空的入口文件，旁边放一个空的
`references/quickjs-authoring.md`。一次 55 次试验的运行约需一分钟，有 skill 文本时用了 23 万到 27 万输入
token，没有时用了 6 万。八次运行加一次冒烟试验共用了 160 万输入 token 和 31 万输出 token。各次运行的
报告没有入库。

| 对照组 | Skill 摘要 | 一次通过 | 修复后 | 平均修复次数 | 带围栏的回复 | 首轮输入 token（平均） |
|---|---|---|---|---|---|---|
| 本次修改之前的 skill | `b59a0a379690` | 110/110 | 110/110 | 0.00 | 7/110 | 4890 |
| 重写后的 skill（所测文本） | `1ec37bcd85e1` | 110/110 | 110/110 | 0.00 | 3/110 | 4306 |
| 中间修订（未采用） | `34509f71ec3c` | 107/110 | 110/110 | 0.03 | 3/110 | 4154 |
| 不带 skill 文本（对照） | `75a11da44c80` | 110/110 | 110/110 | 0.00 | 0/110 | 1087 |

在“之前”、重写后和对照三组中，每个用例都是 10/10 一次通过。中间修订在 `program_control_flow`、
`program_durability` 和 `program_host_capability` 中各漏了一次，都在第一次修复后解决。在三个留出用例上
（每组 30 次试验），“之前”、重写后和对照三组都是 30/30 一次通过，中间修订为 29/30。

失败类别按每一个被拒绝或不合规的回复统计：

- 被拒绝的源码：“之前”和重写后各 110 次试验中都没有。
- 带围栏的回复：尽管提示词禁止，JSON 封装仍被包在 Markdown 围栏里。提取器能把它还原，所以这些试验
  算通过，但它们违反了要求的原始封装。之前 7/110（两次运行分别为 3/55 和 4/55，其中 4 次在
  `program_structured_concurrency`），重写后 3/110，没有 skill 文本时为 0/110。
- 中间修订，3 次首轮被拒绝：图名称取自用例而不是所提供的名称（2 次，`graph name must be capability`），
  以及对一个本是普通对象的响应读取了 `channels` 包装（1 次，`TypeError: cannot read property 'accepted'
  of undefined`）。该修订删掉了防止这两种错误的两条规则，重写后的文本把它们恢复了。

数据支持的结论：在有运行器的 API 参考和用例文字的情况下，不论有没有 skill，模型都能在首次尝试中写出
全部 11 个能力组（110/110），所以这项测量无法证明 skill 能提高通过率，也没能区分以前的 skill、重写后的
skill 和没有 skill。不同的是成本和回复格式。以前的 skill 文本让每次调用多出约 3,800 个输入 token（是对照组
输入的 4.5 倍），重写后多出约 3,200 个，首轮输入因此降低了 12%。带围栏的回复在以前的文本下比没有文本时
更常见（7/110 对 0/110，Fisher p = 0.014），但单看每一次运行都不显著（3/55 和 4/55 对 0/55，p = 0.24 和
0.12），p 值没有针对所做的多次比较做校正，7 次中有 4 次出在同一个用例里，而以前的文本中究竟是什么造成的，
没有检验过。重写后的文本与两者都无法区分（3/110，对“之前”p = 0.33，对对照 0.25）。中间修订的 3/110
漏判不显著（p = 0.25），但每一次在文本里都看得到原因。重写后的文本是在读过中间修订在八个非留出用例上的
失败之后写的，所以它在那里的结果不是独立检验；只有留出用例是未经触碰的比较，而 30/30 对 30/30 区分不了
各组。

测量之后的修正。作为“重写后的 skill”测量的文本，其摘要为 `1ec37bcd85e1`；本次修改实际发布的文本，摘要为
`753b82da7b2d`。运行之后，把参考文档与编译器逐条对照，改正了六处表述：“没有 `import`”的说法、“未知的键会被
拒绝”的适用范围、`exit` 必需且 `__end__` 不能作为边的目标的说法、`yield*` 会被拒绝的说法（只有用在命令上才会
被拒绝）、`cancelScope` 接受的作用域，以及拒绝信息的形态。发布的文本长了 688 字节（约 230 个 token），因此
首轮输入约为 4,540 个 token，比以前的 skill 少 7%。它没有用 110 次试验来测量。对三个用例
（`program_durability`、`program_map`、`graph_routing`）各做 3 次尝试的冒烟运行，一次通过 9/9，原始封装
9/9，没有 provider 失败（40,761 个输入 token，5,212 个输出 token）。这次运行是回归检查，不是测量。

这是针对一个模型、一个 provider、一种采样配置、在提示词明确给出契约的用例上做的小样本能力测量，
不是可靠性结论。要显示 skill 的作用，需要未经辅助的模型有时会失败的任务。skill 的 chat 模板和
Harness MCP 这两个使用面没有被测量。

## 较早的结果（DeepSeek）

这次较早的运行使用了不同的模型、较早的 skill 和较早的 Program 实现，记录于运行器加载 skill 之前。
在实机运行中，模型对全部 11 个能力组，都给出了通过探针验证的源码。
静态的 HITL/重试，需要一次由诊断引导的修复。结构化并发需要两次修复：
先是恢复 ES 模块的导出，再把错误的 Core 绑定替换成确切的已准入名称。
控制流在一次确切绑定的修复之后通过。在宿主提供了原生 API manifest、
并且毫不含糊地说明了已准入的 Core 标识符之后，map 一次性通过。

| 能力用例 | 模型方面的证据 | 重要的观察 |
|---|---|---|
| `graph_basics` | 通过 | 用循环构建的节点降级正确 |
| `graph_routing` | 在重复试验中通过 | 更早的输出用了被禁止的 CommonJS/`require` |
| `graph_fanout_barrier` | 通过 | fan-out 的边和屏障成员都是准确的 |
| `graph_hitl_retry` | 一次修复后通过 | 最初的输出用错了节点名 |
| `registry_mediated` | 通过 | 模型正确引用了宿主准入的动态节点 |
| `program_control_flow` | 一次修复后通过 | 更早的试验反复调用 `core`/`Core`，而不是已准入的 Core `capability` |
| `program_map` | 注入确切标识符后通过 | 更早的试验用了 CommonJS、`yield*` 和错误的 Core 名称 |
| `program_structured_concurrency` | 两次修复后通过 | 验证了确切的嵌套命令和 Core 绑定 |
| `program_spawn_await` | 通过 | `Await(Spawn(...))` 和超时在结构上得到验证 |
| `program_durability` | 通过 | emit、checkpoint 和取消命令都是准确的 |
| `program_host_capability` | 通过 | import 槽位和规范的输入相匹配 |

验证器除了检查命令的种类和输入，还检查确切的 Core 名称，要求每个嵌套的 `callCore` 都使用 `capability`。
因此，使用 `core`、`Core` 或节点名 `work` 的模型源码会被拒绝。

这证明的是能力，不是统计意义上的可靠性。用当前模型做的重复试验，以及单独报告的 provider 失败，
见上面的“当前结果”。

## 对生成的 Program 的影响

原始的一次性源码生成，不足以作为产品层面的保证。最低限度的安全合成路径是：

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

在较早的那次运行中，模型反复把 ES 模块与 CommonJS 混淆，把图名称与节点名称混淆，
把所要求的 Core 标识与 `core` 这样的泛称混淆。因此，宿主应当注入确切的签名和已准入的标识符，
尽可能保留固定的模块脚手架，并且绝不能把看起来像样的源码，
当成所要求的拓扑已经构建出来的证据。

NeoGraph 在 `ProgramSynthesisGateway` 中强制执行这条边界：每份网关配置都必须提供一个由宿主持有的
语义验证器。验证成功，会在 Catalog 准入之前产生一份内容寻址的回执；验证被拒绝，
则抛出 `ProgramSynthesisValidationError`，保留确切的证据，并且绝不会调用准入解析器。
已经消耗的动态编译预留，仍然保持已消耗的状态。

`javascript_authoring_capability_manifest()` 以机器可读的数据，给出已安装的图构建器和命令词汇表、
确切的签名、分类、限制和 profile 约束。符合性测试会把这份 manifest，
与两个 QuickJS 上下文中实际安装的属性做比较，这样 API 一旦漂移，测试集就会失败。

manifest 还发布了：`features`，即每个编写功能一个布尔标志（例如 `graph.metadata`、
`barrier.any_of` 或 `failure_edges`）；每个方法的 TypeScript `dts` 签名，其中共用的声明放在
`dts_prelude` 里；以及每个图构建器参数的 JSON Schema。有两个 schema 文件作为支撑：
[`schemas/javascript-capability-manifest-v1.schema.json`](../schemas/javascript-capability-manifest-v1.schema.json)
描述 manifest 本身，
[`schemas/core-definition-v1.schema.json`](../schemas/core-definition-v1.schema.json)
描述封存的 Core 定义，供对生成的定义做约束解码使用。
测试会对照它的 schema 校验 manifest，也会对照 Core schema 校验黄金身份语料库中的定义。
