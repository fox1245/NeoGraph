<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=zh-CN source_sha256=c70c7b805d11a43a76fb1402e0b7ab7160eea9d0b9137fc779776b717d66c453 -->
# The Beast — 生成 · 演化 · 回滚

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 一个自我进化的智能体，它将自己的harness编写为严格的Core JSON，在Core编译器下进化它，并通过检查点器回退其执行。**生成。进化。回退。野兽仍在。**

大多数“智能体框架”让你*构建*一个图。The Beast 能做到任何静态 harness 无法做到的三件事——而且这三件事都是**真实的、离线的、确定性的**，全部包含在这一程序中（无需 API 密钥）：

1. **在运行时生成**一个新的 harness，并在任何节点运行之前证明其一致性。
2. 使用真实的变异算子**演化**它，以编译器本身作为适应度门槛。
3. 通过检查点机制将正在运行的 harness **回滚**到任意先前超级步骤——这是真正的时间旅行，而非重放。

这之所以安全，仅仅是因为在 NeoGraph 中，一个 harness 就是**数据**——用严格 Core JSON 描述的拓扑（issue #56）——并且 Core 编译器可以在 harness 运行*之前*证明其一致性。严格 Core JSON 是一种互换工件，而非第二源语言。编译器才是将这份“怪兽”从负债转变为类别定义的力量。

## 运行它

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — strict compile and validation gates passed. Core nodes: s1_n s2_n s3_n
  (strict Core JSON is already the canonical interchange representation.)

── ACT II · evolve the harness (compiler = fitness) ──
  generations: 4 · offspring: 36 · survived compile gate: 36 · rejected (invalid, never run): 0
  sample mutations that produced offspring:
    gen 1: remove_edge: removed edges[0]        →  3 nodes
    gen 1: toggle_ce: added conditional edge from s2_n  →  3 nodes
    gen 1: toggle_barrier: added barrier on s3_n →  3 nodes
  (full diffable lineage via to_json(result) — the evolutionary rollback surface.)

── ACT III · spawn + roll back through the checkpointer ──
  ran to completion, trail = ["s1_n","s2_n","s3_n"]
  checkpoint timeline (3 snapshots):
    step 0  id=aac922ed  trail=["s1_n"]
    step 1  id=4b74daa9  trail=["s1_n","s2_n"]
    step 2  id=a528eb9d  trail=["s1_n","s2_n","s3_n"]
  >> ROLLBACK to step 1 (id=4b74daa9)
     final trail was ["s1_n","s2_n","s3_n"]; restored trail = ["s1_n","s2_n"]  (later steps gone)

Generated. Evolved. Rewound. The Beast remains.
```

## 第一幕 —— 生成 + 门控

The Beast 直接以严格的 Core JSON 形式编写一个 harness，并按顺序强制其通过编译器与验证关卡。未能通过任何关卡的 harness 将被**丢弃**。

| 关卡 | API | 捕获 |
|---|---|---|
| **1. 编译 + TV** | `GraphCompiler::compile` (严格, `schema_version: 1`) + `verify_roundtrip` | 拼写错误或不支持的键是*硬错误*（已消费键记账），而不是静默丢弃。翻译验证断言 `canon(source) == canon(compile(source).to_json())`。 |
| **2. 验证** | `GraphValidator::validate` | 图**意味着**什么：悬空边（E3）、永远无法触发的屏障（E8）、不完整路由映射（E10）、通道效应违例（channel-effect violations）（E4/E6）。 |

种子包含三个显式节点，作为核心链 `s1_n → s2_n → s3_n` 连接。

## 第二幕 — 进化（编译器即适应度函数）

`neograph::graph::evolve()` (issue #80) 在种子上运行**真实的变异算子** — `toggle_conditional_edge`、`toggle_barrier`、`add_edge` 和 `remove_edge`。每个后代首先通过**编译门**：无效后代免费消亡，从不执行。拒绝率本身是算子健康度的指标。

变异空间是有界的严格 Core 拓扑，而非源语言。后代保持规范互换表示形式，而编译门槛对每个子代始终保持启用。

每次运行通过 `to_json(result)` 发出可差异化的谱系：每个个体的父代、世代、变异和核心锁文件。该谱系**就是**进化尺度上的回退表面 — 提交它、差异比较它、回退整个世代。

## 第三幕 — 回滚（检查点时间漫游）

存活的测试框架在启动时附带一个 `InMemoryCheckpointStore` 通过 `EngineConfig::checkpoint_store`进行连接。引擎在每个超级步骤结束时对状态进行快照。之后：

- `store->list("beast-run")` 返回完整时间线 — 你可以*看到* `trail` 每一步增长一个节点。
- `store->load_by_id(earlier.id)` **恢复**较早步骤的精确通道状态。演示从 `["s1_n","s2_n","s3_n"]` 回退到 `["s1_n","s2_n"]` — 后续步骤真正消失。这是 `load_by_id` / `load_latest` 时间旅行，与 HITL 中断/恢复和线程分叉所基于的机制相同。

## 进入现场演示——模型实际编写 harness

`the_beast.cpp` 是离线的（存根作者）。[`the_beast_live.cpp`](the_beast_live.cpp) 是真实的东西：一个实时 LLM 被交给 `NodeFactory::export_schema()`（此引擎构建接受的精确调色板 — 它不能漂移，因为它*就是*引擎的模式，参见 [`../../52_export_schema.cpp`](../../52_export_schema.cpp)）并被要求以严格的 Core JSON 编写测试框架。无论它返回什么，都经过相同的编译器和验证门；在拒绝时，诊断信息直接反馈到对话中，模型重写 — 一个真正的自我修复循环。

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek V4 Flash 0731 via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

`the_beast_live.cpp` 将 `~deepseek/deepseek-v4-flash-latest` 固定到 `provider: {"zdr": true, "only": ["morph"], "allow_fallbacks": false}`。在验证时，OpenRouter 将 Morph 的数据中心列为美国，并将该模型/提供商端点列为支持 ZDR。这是严格的提供商选择，而非 OpenRouter 的区域驻留保证：其文档化的区域驻留保证目前是企业 EU 路由。如果 Morph 的合格端点不可用，请求将失败，而不是将提示发送到不同的提供商。

实时 cookbook 将其提供方超时设置为 180 秒：此推理模型 4,000 个令牌的生成预算合理地超过了通用的 60 秒默认设置。



```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — strict compile and validation gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**实时运行所展示的情况**（DeepSeek v4 flash）：它在一次尝试中就为线性流水线、菱形 fan-out/屏障 fan-in 以及条件路由器生成了*连贯的*测试支架——自我修复循环已启用，但能力强的模型很少触发它。门控仍然发挥了 lint 的作用：它们以警告的形式标记了菱形结构中缺少屏障（E9）和路由器上不可达的处理程序（E7）。重点不在于模型经常失败；而在于**一旦失败，它就无法让损坏的测试支架通过编译器**——创造力无边界，连贯性可验证。

这里的节点是确定性的 `beast_node` 工作者，因此实时运行花费一次 LLM 调用（编写）并免费执行；将它们替换为 `llm_call`，每个节点也变成实时调用。

## 复制 Ninja——验证过的本地能力成为图节点

[`the_beast_copy_ninja.cpp`](the_beast_copy_ninja.cpp) 使一条狭窄的能力到工具链的路径可执行。它**不会**将 A2A 卡片变成代码：

1. 一个合成的回环服务器暴露一个众所周知的 Agent Card；采集过程精确执行该 GET 请求，并且从不跟随卡片通告的 RPC URL；
2. `AgentCardCandidateCompiler` 产生一个不可变的、**未准入(admission)**的描述符，排除自由形式的卡片文本、端点、凭据和可执行源代码；
3. 一个独立提供的、摘要固定的行为配置文件验证唯一的 `copy-ninja.hello-world-echo.v1` 模板，然后将其物化为本地 `CopyNinjaNode`；并且
4. 实时Beast仅编写一个双通道、单节点拓扑。正常的严格编译/往返 → 验证门先运行；随后第四个本地绑定门要求恰好 `copy_ninja_local` 在 `__start__` 与 `__end__` 之间。

调用者的提示词被刻意排除在 LLM 消息之外：由模型生成拓扑，而图的本地部分单独消费提示词。如果合成的源服务器观察到任何 RPC，运行也会失败。这是对单一固定本地行为[的证明]，而**不是**源代码传输、委托、准入(admission)或通用行为等效性。

```console
$ cmake -S . -B build -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_A2A=ON
$ cmake --build build --target cookbook_the_beast_copy_ninja
$ ./build/cookbook_the_beast_copy_ninja "Grace"
```

2026-08-08 观察到的实时结果：作者模型首次尝试即通过全部四个门控；图返回 `Hello, World! I have received your request (Grace)`，包含一次发现 GET 和零次源智能体 RPC。

## Apex — 装备吞噬工具

存根工作线程演示证明生成的工具链是*连贯的*，但工具链从不行动。[`the_beast_apex.cpp`](the_beast_apex.cpp) 才是怪物：模型被交给一个**工具目录**并要求编写一个 ReAct 智能体 — `llm_call` ⇄ `tool_dispatch` 在 `has_tool_calls` 上循环。它编写的工具链经过连贯性门控，然后**以绑定工具的方式生成**（`ctx.tools` + `engine->own_tools`）。生成的智能体随后自行决定调用哪些工具以及何时调用。

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

一次真实运行 — 自修复循环真实触发，随后自主调用工具：

```
Tool catalog offered: calculator get_weather

── Attempt #1: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'id'    (strict, schema_version 1)
  → feeding diagnostics back for self-repair.
── Attempt #2: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'name'
  → feeding diagnostics back for self-repair.
── Attempt #3: model authors a tool-calling agent ──
  ACCEPTED — coherent tool-calling agent. Nodes: agent(llm_call) tools(tool_dispatch)

── Spawning the agent it wrote — live, tools bound ──
  user task: What is 23 multiplied by 19, and what's the weather in Seoul?
  [the harness is calling tools autonomously]
    tool → {"result":437.0}
    tool → {"weather":"19C, clear"}
  tool calls executed by the harness: 2
  final answer: 23 × 19 = 437; Weather in Seoul: 19°C, clear.

The model wrote the agent. The compiler proved it. The agent ate the tools.
```

这就是整个论点的一次运行：模型两次幻觉出一个 `nodes` 模式（先添加 `id`，再添加 `name` 键），而严格编译器的**已消耗键核算拒绝了这两次** — 诊断信息回到对话中，它在第三次尝试时自我修复。然后机器编写、编译器证明的智能体运行了一个实时 ReAct 循环并自主调用了两个工具。创造力无界，工具使用自主，**连贯性不可妥协。**

## 熔炉——“当缺少工具时，它编写一个”

[`the_beast_forge.cpp`](the_beast_forge.cpp) 是顶点加上工具供应链。给定一个任务，它：

1. **发现** — 生成一个标准 MCP stdio 服务器并通过真实 MCP 协议列出其工具（`MCPClient::get_tools`）。
2. **锻造（FORGE）** —— 对于任务需要但目录中缺失的能力，架构师 LLM **编写一个实现该能力的 Python MCP 服务**；我们将其物化到磁盘、启动它，并通过 MCP **重新发现**新工具。（如果生成的服务初始化失败，会自我修复。）
3. **创作（AUTHOR）** —— 基于*合并后的*目录编写一个 ReAct 智能体；一如既往地设置三道关卡 + 自我修复。
4. **生成（SPAWN）** —— 绑定所有已发现*和*锻造的工具并运行该智能体，由后者自主调用这些工具。

一次真实运行 —— 模型编写了缺失的工具，智能体也确实使用了它：

```
── DISCOVER · stock MCP server ──
  tools: get_current_time calculate get_weather

── FORGE · the model writes a Python MCP server for what's missing ──
  attempt #1: wrote 5225 bytes → /tmp/beast_forged_server.py
  FORGED + re-discovered over MCP: reverse_string

── AUTHOR · the model writes a ReAct agent over the full catalog ──
  #1 REJECTED at 'compile': ... unknown or unconsumed key 'id' → self-repair.
  ACCEPTED — coherent agent: agent(llm_call) tools(tool_dispatch)

── SPAWN · run the agent it wrote, tools bound ──
  [harness dispatching tools autonomously]
    tool → retsnom                         # the forged reverse_string
    tool → 2026-07-10 06:13:21 (UTC)       # the discovered get_current_time
  final answer: Reversed 'monster' → retsnom; current UTC time is 2026-07-10 06:13:21.

It discovered tools, forged the missing one, and used them all.
```

两个实时 MCP 子进程（一个是标准的，一个是 Beast 在*本次运行*中编写的），每个上有一个真实的 `tools/list`，一个真实的 ReAct 循环。只有作者模型是远程的。

### 它还能定义自定义*节点*吗？

老实说：NeoGraph 节点**类型**是通过 `NodeFactory::register_type` 注册的 C++ 类 — 你无法在运行时 JIT 编译一个全新的原子 C++ 节点类型。但意图通过 Beast *可以*从数据驱动的三种方式得到覆盖：

- **复合节点** — 显式 Core 节点和边让模型纯粹以数据定义可复用的拓扑单元；这正是 `the_beast.cpp` 的种子所做的。
- **递归** — 一个`subgraph`节点将整个harness嵌入为一个节点，因此由Beast编写的harness可以包含由Beast编写的子harness（N级自我增殖）。
- **通过代码实现自定义行为** — 上述forge模式*就是*由模型编写的运行时行为：它编写的工具成为一个可调度的单元。同样的技巧可以推广到通用的`script_node`类型（一个预注册的C++节点，执行模型编写的代码），这是获得“由LLM定义逻辑的新原子节点”的正道。

真正被排除在外的就是运行时生成新的*编译C++节点类*；模型因需定制行为所依赖的一切都位于编译器已代管的数据/脚本/子图界面上。

## 脚本 —— 通用弹药筒（模型编写的节点逻辑 + 流程）

上述每个变体都让模型编写*工具*（叶子能力）。[`the_beast_script.cpp`](the_beast_script.cpp)让它编写**节点逻辑——包括工具根本无法表达的控制流（`goto`）。** `script_node`是一个预编译的C++节点，其配置携带模型编写的Python；在`run()`时，它将通道状态交给节点，并将代码返回的任何内容——`{writes, goto, sends}`——应用到图中。模型在数据中定义节点的行为*和*图的流程，无需重新编译。

一致性仍然不可妥协。脚本在配置中声明其契约（`reads` / `writes` / `goto_targets`）；harness通过严格的Core编译器/验证门 PLUS 一个Beast层**契约检查**（声明的写入必须是已声明的通道；goto目标必须是真实节点）PLUS 一个**运行时包装器**，拒绝任何超出声明的写入/goto。这在不改变NeoGraph核心的情况下恢复了Beast层的效果/路由保证——是增量的且向后兼容的。

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

实况运行——模型编写了一个计数器循环，其控制流是它自己的`goto`：

```
── Attempt #1: model writes node logic ──
  ACCEPTED — coherent, and the script's write/goto surface is contract-checked.

── Spawning — the node's own code drives the loop via goto ──
  [tick #1 — script decides: continue or exit]
  [tick #2 — script decides: continue or exit]
  [tick #3 — script decides: continue or exit]
  trace: tick -> tick -> tick -> END
  final counter = 3  (the model's goto logic ran the loop, contract-enforced)
```

从`tick`没有静态边：循环之所以存在，仅仅是因为模型的Python在计数器达到3之前返回`{"goto": "tick"}`，然后返回`{"goto": "__end__"}`。`--selftest`从无API密钥的预置harness运行相同的机制，因此CI可以离线执行它。

**边界（诚实的）。** 编译器证明图的*形状*；契约证明节点的*表面*（它可以接触哪些通道/目标）；只有脚本的*内部逻辑*未被证明——受子进程上的`timeout`和运行上的`max_steps`约束。运行模型编写的代码是任意代码执行：对于本地、用户驱动的cookbook来说没问题，但生产环境希望在解释器周围加沙箱。这是一个**构建选项**，默认关闭：

Sandboxed-api 通过 FetchContent 嵌入效果不佳，因此请链接预构建的树（构建配方在选项上方的 CMake 注释中给出）：

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

开启后，Python在Google **Sandbox2**下运行——它自己的用户/pid/挂载/网络命名空间，一个只读的文件系统视图，仅限于解释器加两个工作文件，以及CPU/墙钟/文件rlimit。需要`libcap-dev`、`libunwind-dev`、C++20工具链；已在Linux/WSL2上验证。

**从效果契约合成的Seccomp策略。** Python的系统调用足迹太大，无法安全地白名单化，因此默认操作保持许可——但节点声明的*能力*减去系统调用：一个声明无`"net"`能力的节点具有`socket`/`connect`/`bind`/…被seccomp阻止（EPERM）；无`"exec"`能力则阻止`execve`/`execveat`。该策略*从声明的契约推导*，而非手写。这已通过一个负向测试验证——**相同的**python，在**相同的**沙箱下，仅因声明的cap不同而不同：

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

所以这不仅仅是网络命名空间：没有`net`cap时，`socket()`*系统调用*失败（在netns之上进行纵深防御）。诚实的范围：这是**容器级 + 契约派生的seccomp黑名单**，不是完整的系统调用白名单——通过未阻止的系统调用进行的内核利用仍然无法被遏制。更严格的每节点白名单（以及基于能力的秘密中介）是文档化的下一步。

## 演化——模因（达尔文式 + 拉马克式）

离线的`the_beast.cpp`故意设置`run_evaluation=false`，因此其选择仅是结构性的。通用演化API也可以执行任务并评分精确的期望通道值。

[`the_beast_evolve.cpp`](the_beast_evolve.cpp)使用自定义的连续距离度量：接近命中可以朝着数值目标改进，而不是接收通用评分器的单一输出不匹配类别。

- **任务**（一个真实的任务，按输出评分——而非结构性代理）：组装一个计算目标数字的算术流水线。存在五个操作节点——`add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)`——每个节点读取`acc`通道（初始值为0），应用其操作，然后写回。测试框架的答案就是`acc`在执行后所持有的值；**适应度 = `-(|acc - 20|)`**。*拓扑*（哪些操作运行，以什么顺序）决定了数字，因此演化连线即演化计算。
- **达尔文式**：随机重新连线（`all_operators()`）+ 按测量输出进行选择——跌跌撞撞地逼近20。
- **拉马克式：** LLM 进行运算逻辑，接入一条精确命中 20 的链路，并将那个获取的方案作为可遗传的种子注入。

```console
$ ./build/cookbook_the_beast_evolve --darwin-only   # offline, deterministic
gen 0  seed acc=5   fitness -15
gen 1  best acc=10  fitness -10  (mut)
gen 2  best acc=24  fitness -4   (mut)   # overshoot
gen 6  best acc=19  fitness -1   (mut)
gen 9  best acc=20  fitness -0   (mut)   → Solved
champion: acc=20, origin 'mut'. Pure Darwinian mutation + selection.

$ ./build/cookbook_the_beast_evolve                 # + Lamarckian (needs OPENROUTER_API_KEY)
gen 2  best acc=24  fitness -4  (mut)
gen 3  [Lamarckian] LLM refinement acc=20  fitness -0  → injected (heritable)
       Solved via Lamarckian injection.
champion: acc=20, origin 'LLM'. The winner is a Lamarckian acquired trait ...
```

对比正是关键所在：**盲目变异跌跌撞撞地逼近目标**（第9代时5→10→24→19→20，通过试错计算出数字）；**LLM则做算术**——`(0+2)*5*2 = 20`——并在被注入时直接跳到答案。因为该习得解成为可遗传的冠军（`origin 'LLM'`），这是拉马克式的；盲目变异+选择是达尔文式的；同时运行两者则是模因算法。

诚实的说明：纯达尔文式已离线验证且是确定性的。拉马克式LLM调用（deepseek-v4-flash）**偶尔不稳定**——流式回复有时返回不可解析的内容，此时运行记录`[Lamarckian] LLM returned no parseable harness`并由达尔文式承担该轮；最终行报告冠军的*实际*来源，绝不报告未发生的拉马克式胜利。

## 门评估——coherence gate 真的健壮吗？

Beast 的整个安全论证在于，静态验证器是一个*健全的*一致性预言机：ERROR 表示测试装置在运行时确实会错误；无错误则表示它可以执行。这是**断言而不是测量**——即每位评审者在追问的第一件事。

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp)对其进行度量。它将一个带标签的拓扑语料库通过验证器（预测判定）和引擎（真实结果）运行，并进行交叉核对。离线、确定性、无密钥——`exit 0`当且仅当每个判定都与执行匹配，因此**CI可以基于健全性进行门控**。

```console
$ ./build/cookbook_the_beast_gate_eval
case                     | validator      | runtime | sound?
coherent                 | ok             | CLEAN   | yes
E4-undeclared-write      | ERROR:E4       | FAULT   | yes   # reject ⇒ genuine runtime throw
E3-dangling-edge         | ERROR:E3       | FAULT   | yes
E7-unreachable(warn)     | ok             | CLEAN   | yes   # a warning does NOT reject a correct graph
E10-empty-routes         | ERROR:E10      | not run | yes   # dispatch is UB by design — the gate stops it
runtime cross-check: 4/4 cases where the validator's verdict matched execution.
```

被测属性：

> 验证器报告ERROR⟹图在执行时出错；验证器报告无错误⟹图干净执行。

第一行是*健全性*（一个被标记错误的图却干净运行将是健全性漏洞）；一个被标记警告的图干净运行则表明门控不会*过度*拒绝。E10/E8类错误仅作判定——运行空路由映射会解引用`rend()`（UB），这正是门控存在所要防止的故障，因此它被检查但不执行。这是一个演示语料库，并非每个诊断的穷尽覆盖——但它将“门控是健全的”从口号变成了可测量的、CI强制的4/4。

## Gate-fuzz — 该保证及其边界，规模化实现

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp)将gate_eval从5个手工标记的案例扩展到数千个模糊测试的案例——但诚实地做。天真的做法（模糊测试N个图，打印精度1.0）将是作秀：**引擎在编译时重新运行验证器并在任何错误时抛出异常**，因此“验证器错误⟹引擎故障”*按构造*为真。因此该程序度量两件真正有信息量的事情：

```console
$ ./build/cookbook_the_beast_gate_fuzz 2>/dev/null   # lint → stderr
LAYER 1 — static gate vs engine over 2000 honest-contract mutants:
  gate rejected 1586, gate passed 414;  agreements 2000, DISAGREEMENTS 0
  runtime faults AFTER the gate passed (soundness holes): 0
LAYER 2 — a node that LIES about its effect contract (500 mutants):
  static gate PASSED (blind to the lie): 500/500
  runtime GraphState guard FAULTED (backstop caught it): 500/500
CI gate (Layer 1: 0 disagreements over 2000; Layer 2: runtime backstops 100%): PASS
```

- **Layer 1 — 大规模一致性。** 使用随机结构变异器对 coherent seed 进行模糊测试（悬空边 → E3，未声明写入 → E4，悬空写入器，丢弃边 → E7 *警告*，额外有效边）。在超过 2000 个变异体上，编译器门控与引擎从未产生分歧。这并非健全性 *发现*（部分上是由构造保证的）——而是一个**回归保证**：如果未来的变更使静态门控与运行时产生分歧，此测试将失败。
- **第2层——边界。** 门控信任每个节点声明的**效果契约**。一个*撒谎*的节点——声明`writes:["out"]`但实际上在运行时写入未声明的`phantom`通道——能通过静态门控（500/500），而**运行时`GraphState`写保护**捕获每一个（500/500）。这不是门控的缺陷；这是设计好的分工。

结果是：对保证条款的*精确*陈述，比起一份完美得可疑的混淆矩阵更为诚实——**静态门控相对于诚实的契约而言是可靠的，对于不诚实的契约则有运行时兜底** — 第1层和第2层各自由 CI 强制执行。

形式化配套文档[`SOUNDNESS.md`](SOUNDNESS.md)*证明*了这一点：超步执行的小步语义、效果格`(𝒫(Chan), ⊆)`、作为良构性判定的门控`⊢ G ok`，以及一个Progress定理（在诚实契约下通过门控的图永不出错），其中诚实性假设被证明是必要的，运行时写保护是其失败即停止的后备。每个前提都对照引擎源码检查；`gate_eval`/`gate_fuzz`是模型的保真度检查。这里的两个测试框架是该文档推论6.4和命题6.5的运行实例。

## Baldwin — 模因是否胜过盲选，继承是否重要？

`evolve`变体展示了达尔文式变异+拉马克式LLM注入。每个评审者提出的更尖锐的研究问题是：**是否存在一个任务，盲目进化和一次性求解器都停滞，但模因组合获胜——并且*如何*继承习得特征是否以文献预测的方式改变结果？**（Whitley 1994；Hinton & Nowlan 1987。）

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp)就是那个实验，在真实的NeoGraph测试框架上运行。基因组是仿射流水线的连线；每个阶段被提交给一个操作**或保持可塑（`?`）**以供终身学习来解决。适应度是**运行时的组装测试框架**的签名——而启动交叉检查证明快速解析适应度在200个拓扑上与编译引擎相等（与gate-eval相同的纪律）。该景观是**欺骗性的**：一个广泛可见的诱饵山丘（0.85），以及一个狭窄的、**无梯度**的全局平台（1.0），只有*学习*——它搜索由可塑基因张成的邻域——才能找到。

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

关于适应度*是什么*的说明：每个基因组都编译为真实的 NeoGraph 拓扑，交叉验证证明引擎运行了 200 个这样的拓扑，且结果与分析模型预测完全一致——*底层*是一个真实、忠实执行的测试平台。遗传算法优化的*目标*是布线上的一个欺骗性 Hamming 景观（用于研究动态的可控测试台），而非原始执行输出。这两个事实都被明确陈述，而非模糊处理。

两项发现，按不同标准衡量：

1. **模因式胜过盲目式（稳健——CI 门控）。** 盲目的达尔文式进化对全局的把握仅约 25%——即随机基线——因为平台区没有已提交空间的梯度，因此选择会跟随诱饵并被困住。学习暴露了平台区并将其把握约 75%。门控断言*差值*（24 个种子的均值），而非每次运行的阈值计数，因为每次运行的计数受初始化运气影响；25% 对 75% 的差值才是稳定信号。
2. **Baldwin式对照组（已测量——从不门控）**。Baldwin（不继承学习到的trait）vs Lamarck（将学习到的trait写入基因组）：这里全局 **74% vs 78%** — Lamarckian 略微领先，这是*预期*的结果，因为该拓扑具有欺骗性但并非对抗性（写回的速度超过其多样性代价）。Whitley的**逆转**（Baldwin > Lamarck）需要特定的对抗性拓扑；这个简单的双峰构造无法鲁棒地展示这一点，并且这是**如实报告，而非调优出的偶然结果。**（这确实很微妙：早期版本使用基于索引的平局决胜*似乎*显示出逆转——一个伪象。选择边界处的平局现在通过每个种子的随机抽取来打破，并在整个扫描中取均值**，且 ~74-vs-78 的顺序在各种种子基底上稳定；表观逆转在该修复后未能存续。）

This is the honest shape of the result the reviewers asked for: the robust claim (learning-guided evolution solves what blind evolution cannot) 已经 measurement 并CI外强制 enforced; the delicate claim (non-inheritance)优于 inheritance) 已测量并 reported as-is, 其中负结果被命名，而非隐藏。

## Baldwin-adv —— adversarial environment + real hill-climbing learner

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp) 使先前实验的两端都更加锐利。学习现在成为**真正的局部搜索**（在可塑性基因上进行多次重启爬山直至局部最优——这是精炼器的离散类比，也是LLM插入的槽位），而景观是真正**对抗性**的：一个宽阔的诱饵山丘，其梯度指向*远离*一个狭小陡峭的全局球体。盲目的固定空间搜索并非处于机会下限——它正沿着诱饵梯度被主动**欺骗**。

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **Memetic ≫ blind（稳健，CI 门控）。** Darwin 被诱骗到诱饵目标（全局约5% / 诱饵约92%）；学习能找到单个基因组无法找到的全局球（76-98%）。这是一个*更强*的区分，超越了平台期——盲基线是被误导，而不仅仅是盲目。在种子基座上稳定（Darwin 2-5%，学习者76-98%）。
- **Baldwin 对 Lamarck：反转无法复现。** Lamarck 写回以稳定优势获胜（98% 对 76%）。对全部可达/不可达边界的参数扫描（30+ 配置，三次扫描）发现 **没有任何机制区间** 中非继承性能稳健地击败写回：当全局可达时，写回的速度占优；当不可达时，两者都失败，仅存在 Baldwin 多样性约 3-4 个百分点的边际优势。这是对“Whitley 的 Baldwin > Lamarck 反转能否在 harness 拓扑上复现”这一问题诚实的经验性回答——**不能**，在这个离散机制中，程序以点名机制的方式给出了这一结论。（Whitley 的反转是在 *连续* 多模态函数上以实值局部搜索确立的；此处的离散拓扑-GA 并未呈现该现象。）

## Baldwin-llm — 模型本身即学习算子

上述机械学习器（随机猜测、爬山）始终是LLM精炼器插入的*槽位*。[`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp) 将其插入，在一个模型实际能够推理的任务上：填充算术流水线的`?`阶段，使`acc`达到目标。**学习算子就是模型**（它为`?`阶段选择操作）；适应度是组装好的测试框架*运行*的结果。Baldwin/Lamarck切换变得字面化：

- **Baldwin式**对模型的填充进行评分，但保持基因`?`——模型必须在下一代**再次**被咨询。学习不被继承。
- **Lamarck式**将填充写入基因组——`?`变得承诺化。获得的性状是**可遗传的**；模型无需再次被咨询。

```console
$ ./build/cookbook_the_beast_baldwin_llm       # oracle learner (default, offline)
  Baldwinian (learner = oracle):
    gen 0: … committed genes 16/24 | learner calls 5
    gen 3: … committed genes 10/24 | learner calls 6      # re-learns every gen
  Lamarckian (learner = oracle):
    gen 0: … committed genes 24/24 | learner calls 5
    gen 3: … committed genes 24/24 | learner calls 0      # banked; no re-learning
total learner invocations: Baldwin 23 vs Lamarck 5  (Lamarck banked its way to fewer)
```

可观察的差异不是适应度（两者都达到目标），而是**基因组经济性**：Lamarck将学习者的工作存入遗传（基因承诺化，调用降至零）；Baldwin每代重新学习（基因保持可塑性，调用保持高位）。离线运行使用确定性**预言机**学习者（默认），或使用`--llm`配合`OPENROUTER_API_KEY`使**模型**成为学习者——在这种情况下，这些调用是真实的API调用，而遗传性字面上就是支付模型一次与每代支付模型之间的区别。这就是"模型的修复是否变得可遗传？"的具体含义——以轨迹展示，而非断言。（`--llm`路径需要网络；它回退到预言机并在任何调用/解析失败时记录日志，因此演示始终完成。）

## Novelist——输入一个前提，输出一篇轻小说长度的`.txt`

最简单的真正有用的写作框架，也是"NovelWriter"理念的诚实形式：给它一个前提，得到整篇轻小说大小的手稿作为纯文本。[`the_beast_novelist.cpp`](the_beast_novelist.cpp) 是**中间丢失**问题的具体解药——一个长故事*不是*在单个巨大上下文中写成的。它是一个基于**显式故事状态**的小图：

    channels:  premise · outline · bible · summary · book · idx · total

因此每章都是**针对紧凑的外部化状态全新生成**（大纲节拍、故事圣经、运行摘要），而不是重新阅读6万字符。模型永远不必*记住*小说中某个角色是谁——它读取`bible`通道。

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

该图是`__start__ → planner → writer ⟲`：`planner`将前提转化为大纲+初始圣经；`writer`将章节`idx`写入`book`并**更新`summary`和`bible`**，使下一次迭代保持有据可依，然后**使用`Command` goto自循环**直至`idx+1 == total`。效果契约被声明，因此**一致性门在写出一个字之前证明布线**——每个故事状态通道都被实际消费，没有悬空阶段。

**批量生成，批量进化——每章有独特感觉。** 每章实际上是一个隔离的子智能体（一次全新的`writer`调用，仅由共享故事状态作为基础）。为防止它们读起来相同，写作者为每章进化一个**风格基因组**——5维风格空间中的一个点（视角·时态·情绪·镜头·节奏，480种组合）。它运行一个迷你GA（`baldwin`模因循环，目标是*多样性*而非目标值）：一批候选基因组进化以最大化**新颖性**——与已用风格的距离——然后胜者被承诺并推入`styles_used`，使下一章被压力推离它。离线时风格轨迹是确定性的且明显多样：

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```

新颖性搜索最大化差异性（它并不*保证*每个维度都不同）——诚实，且足以打破单模型长文本的单调声音失败。

离线（无密钥）时，**确定性桩**规划器/写作者运行*完全相同*的图，因此流水线——状态线程化、goto循环、累积、`.txt`输出——无需网络即可验证；`OPENROUTER_API_KEY`替换为模型以生成真实散文。诚实的范围：门证明*管道*（故事状态已布线和线程化），而非*散文*——叙事质量是模型的工作，超出结构的连续性将是一个检查器节点（运行时后盾模式），留作明显的下一个待添加节点。

## 暴露的摩擦

- **E6 "已写但从未被读" 在`trail`上**作为lint发出——而且它是*正确的*：`trail`是一个终端输出通道，没有下游*节点*消费它；只有宿主通过`RunResult::channel`读回它。验证器在精确处理图的通道表面，而非出错。故意保持可见以展示效果分析在工作。
- **序列化检查点是通道包装的**（`channel_values["channels"]["trail"]["value"]`），而非扁平结构——演示中的`channel_of()`辅助函数会将其解包。`RunResult::channel`读取时采用相同结构。
- 核心锁文件在整个验证过程中保持`schema_version: 1`，这使规范交换表示保持严格，并防止演化循环静默降低其一致性保证。
