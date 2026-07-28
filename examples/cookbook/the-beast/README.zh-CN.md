<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=zh-CN source_sha256=aa9675ba1cbeeb80c64724416d97b82171a94f2261f16551966e181ee742405d -->
# 野兽 — 生成·进化·回滚

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 一个自我进化的代理，编写自己的执行框架，并在
> DSL 编译器，并通过检查点机制倒回其执行。
> **生成。进化了。倒回。野兽依然存在。**

大多数“代理框架”都可以让您“构建”图。野兽做了三件事
静态执行框架无法做到的事情——所有这三个都是**真实的、离线的和
在这个程序中具有确定性**（无 API 密钥）：

1. **在运行时生成**一个新的执行框架，并在执行之前证明它是一致的
   单节点运行。
2. 使用编译器本身，使用真正的变异运算符来**演进**它
   作为适应度门控。
3. **通过以下方式将正在运行的执行框架回滚到任何先前的超级步骤
   检查点机制——真正的时间旅行，而不是重播。

这只是安全的，因为在 NeoGraph 中，执行框架是 **数据** — 一种拓扑
JSON 中描述（问题 #56）——DSL 编译器（问题 #75）可以
*在运行之前证明执行框架是一致的*。把它拿走，然后“一个代理
自己写图的”只是一台生产破图的机器。
编译器将怪物从负担变成了范畴。

## 运行它

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — 3 gates passed. Core lockfile nodes: s1_n s2_n s3_n
  (DSL surface expanded away: vars/templates/use gone.)

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
## 第一幕 — 生成 + 门

The Beast 在 DSL **surface** 中编写了一个执行框架（`vars` / `templates` /
`use`) 并迫使它按顺序通过三个相干门。一个执行框架
任何门失败的都会被**丢弃**。

| 门控 | API | 捕获的问题 |
|---|---|---|
| **1. 展开** | `Elaborator::elaborate`|针对 DSL 坐标的表层错误 - 未知模板、丢失/额外 `use` 参数、变量周期、节点名称冲突。总体和确定性：相同的 DSL 总是产生字节相同的核心，因此门 2-3 推理出固定的产物。 |
| **2. 编译 + TV** | `GraphCompiler::compile`（严格，`schema_version: 1`）+`verify_roundtrip` |拼写错误或不受支持的键是*硬错误*（已消费键核算），而不是无声丢弃。 翻译验证然后断言`canon(source) == canon(compile(source).to_json())`——编译器不能悄悄地重新连接任何东西。 |
| **3. 验证** | `GraphValidator::validate` |该图的**含义**：悬空边（E3）、永远不会发射的障碍（E8）、不完整的路由表（E10）、通道效应违规（E4/E6）。错误仅适用于“永远”不可能正确的构造；其余的都是lint 提示。 |

种子是一个 `stage` 模板，通过 `use` 实例化 3 次；
细化将其扩展为核心链`s1_n → s2_n → s3_n`。

## 第二幕——进化（编译器是适应度函数）

`neograph::graph::evolve()`（问题#80）运行**真正的变异算子**
种子上方 — `swap_template`、`add_use`、`remove_use`、`tune_param`、
`toggle_conditional_edge`、`toggle_barrier`、`add_edge`、`remove_edge`。
每个后代都首先通过**编译门控**：无效的后代死亡
零成本死亡，完全不执行。拒绝率本身就是一种健康
衡量算子的指标。

关键的设计选择：突变空间是**DSL（M4），而不是原始空间
JSON**，因此后代在结构上是有效的*通过构造* - 这是
为什么这里的拒绝计数是0。门是安全网，使
无约束的进化是安全的，并且它始终武装在每个子代上。

每次运行都会通过 `to_json(result)` 发出可区分的谱系：
个体的父代、世代、突变和核心锁定文件。那
谱系**是**进化规模的回滚面——提交
它，差异它，恢复整整一代。

## 第三幕 — 回滚（时间旅行检查点机制）

幸存的执行框架是由 `InMemoryCheckpointStore` 衍生出来的
通过`EngineConfig::checkpoint_store`附上。引擎快照
每个超级步骤结束时的状态。然后：

- `store->list("beast-run")`返回完整的时间线——你可以*看到*
  `trail` 每一步增长一个节点。
- `store->load_by_id(earlier.id)` **恢复**准确的通道状态
  更早的一步。演示从 `["s1_n","s2_n","s3_n"]` 回滚到
  `["s1_n","s2_n"]` — 后面的步骤确实消失了。这是
  `load_by_id` / `load_latest` 时间旅行，同样的机械HITL
  中断/恢复和线程分叉是建立在其基础上的。

## 上线——模型实际上编写了执行框架

`the_beast.cpp` 离线（存根作者）。 [`the_beast_live.cpp`](the_beast_live.cpp)
是真实的：现场LLM已交付`NodeFactory::export_schema()`
（该引擎构建接受的确切调色板 - 它不会漂移，因为它
*是*引擎的架构，请参阅[`../../52_export_schema.cpp`](../../52_export_schema.cpp))
并要求在 DSL 表层编写一个执行框架。无论它返回什么
经历同样的三道门控制；拒绝门的诊断
直接反馈到对话中并且模型重写 -
真正的自我修复循环。

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek v4 flash via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — all three gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```
**实时运行显示了什么**（DeepSeek v4 flash）：作者*coherent*
首次尝试跨线性管道（菱形扇出）时的执行框架 /
屏障扇入和条件路由器 - 自我修复循环已启动
但有能力的模型很少会出错。大门仍然值得保留
lint：他们标记了菱形 (E9) 上缺少障碍且无法到达
路由器（E7）上的处理程序作为警告。重点不在于型号
经常失败；就是当它这样做时，它无法获得损坏的执行框架
经过编译器**——创造力是无限的，连贯性是经过验证的。

这里的节点是确定性的 `beast_node` 工作人员，因此实时运行成本
一次LLM通话（创作）并免费执行；将它们交换为
`llm_call`，每个节点也成为实时呼叫。

## Apex — 执行框架吞噬了工具

Stub-worker 演示证明生成的执行框架是*一致的*，但是
该执行框架自身从不执行实际动作。 [`the_beast_apex.cpp`](the_beast_apex.cpp) 是
真正的杀器：模型收到一个**工具目录**并被要求编写一个
ReAct 代理 — `llm_call` ⇄ `tool_dispatch` 在 `has_tool_calls` 上循环。
它所编写的执行框架经过一致性门控验证，然后**与
工具绑定**（`ctx.tools` + `engine->own_tools`）。生成的代理
自行决定调用哪些工具以及何时调用。

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```
真正的运行——自我修复循环真正启动，然后自主运行
工具调用：

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
这就是整个论文的一次运行：模型幻觉了 `nodes`
模式两次（添加`id`，然后`name`键），以及严格编译器的
**已消费键统计拒绝了两者** — 诊断返回到
谈话在第三次尝试时就自行修复了。然后
由机器编写、经过编译器验证的代理运行实时 ReAct 循环并调用
两个工具自主。创造力是无限的，工具的使用是自主的，
**一致性是不容谈判的。**

## Forge — 当它缺少工具时，它会编写一个

[`the_beast_forge.cpp`](the_beast_forge.cpp)是顶点加工具
供应链。给定一个任务，它：

1. **DISCOVER** — 生成一个现成 MCP stdio 服务器并列出其工具
   真正的 MCP 协议（`MCPClient::get_tools`）。
2. **FORGE**——针对任务需要但目录缺乏的能力，
   架构师 LLM **编写了一个 Python MCP 服务器**来实现它；我们
   将其具体化到磁盘，启动它，然后**重新发现**新工具
   通过MCP。 （如果生成的服务器初始化失败，则进行自我修复。）
3. **作者** — 在 *组合* 目录上编写 ReAct 代理；三
   一如既往的门+自我修复。
4. **SPAWN** — 绑定每个发现的*和*伪造工具并运行
   代理，自主调用它们。

真实的运行 - 模型编写了缺少的工具并且代理使用了它：

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
两个实时 MCP 子流程（一个股票，一个是 Beast 编写的*本次运行*），一个
每个都有真正的`tools/list`，一个真正的ReAct循环。只有创作模型是
远程。

### 它也可以定义自定义*节点*吗？

老实说：NeoGraph 节点 **类型** 是通过注册的 C++ 类
`NodeFactory::register_type` — 你不能 JIT 编译一个全新的原子
运行时的 C++ 节点类型。但其意图通过三种方式得以体现
野兽*可以*从数据中驱动：

- **复合节点** — DSL 的 `templates` / `use` (M4) 让模型
  纯粹在数据中定义可重用的节点/拓扑单元；这正是
  `the_beast.cpp` 的种子所做的事。
- **递归** — `subgraph`节点将整个执行框架嵌入为一个节点，
  因此 Beast 创作的执行框架可以包含 Beast 创作的子执行框架
  （N级自包含扩展）。
- **通过代码自定义行为** — 上面的锻造模式*是*运行时
  模型创作的行为：它编写的工具成为可调度的
  单位。同样的技巧可以推广到通用的 `script_node` 类型（一个
  预注册的 C++ 节点，执行模型编写的代码），这是
  获得此能力的诚实方式：让模型在数据中定义其逻辑。“LLM定义其逻辑的新原子节点”。

真正不可能做到的一件事是发出新的*编译期 C++ 节点类*；模型需要专业化的一切
行为已经存在于编译器的数据/脚本/子图面中
盖茨。

## 脚本 — 通用墨盒（模型编写的节点逻辑 + 流程）

上面的每个变体都允许模型作者*工具*（叶功能）。
[`the_beast_script.cpp`](the_beast_script.cpp)让它创作**节点逻辑
— 包括工具绝对无法实现的控制流（`goto`）
express.** `script_node` 是一个预编译的 C++ 节点，其配置包含
模型编写的Python；在`run()`处，它向节点传递通道状态并且
将代码返回的任何内容 — `{writes, goto, sends}` — 应用于
图。该模型定义了节点的行为*和*图的流程，在
数据，无需重新编译。

一致性是不容谈判的。该脚本在配置中声明其契约
（`reads` / `writes` / `goto_targets`）；执行框架通过三个DSL
门加上野兽层**合约检查**（声明的写入必须是
声明渠道； goto 目标必须是真实节点）加上 **运行时
拒绝声明之外的任何写入/转到的包装器**。那
恢复 Beast 层的效果/路线保证，**零变化
NeoGraph 核心** — 附加且向后兼容。

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```
实时运行——模型编写了一个计数器循环，其控制流是它自己的
`goto`：

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
`tick` 之外没有静态边：循环存在只是因为
模型的 Python 返回 `{"goto": "tick"}` 直到计数器达到 3，然后
`{"goto": "__end__"}`。 `--selftest`从一个运行相同的机制
没有 API 密钥的预置执行框架，因此 CI 可以离线使用它。

**边界（诚实）。**编译器证明图的*形状*；的
合约证明了节点的*表面契约*（它可能的通道/目标）
触摸）；只有脚本的*内部逻辑*未经证实——受
`timeout` 在子流程上，`max_steps` 在运行上。跑步
模型编写的代码是任意代码执行：对于本地来说很好，
用户驱动的cookbook，但生产需要一个沙箱
口译员。这是一个 **构建选项**，默认情况下关闭：

Sandboxed-api 通过 FetchContent 嵌入效果不佳，因此链接预先构建的树
（在选项上方的 CMake 注释中构建配方）：

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```
有了它，Python 就可以在 Google **Sandbox2** 下运行——它自己的
user/pid/mount/net 命名空间，只读 FS 视图，仅限于
解释器 + 两个工作文件，以及 CPU/wall/file rlimits。需求
`libcap-dev`、`libunwind-dev`，C++20 工具链；在 Linux/WSL2 上验证。

**从效果契约合成的 Seccomp 策略。** Python 的系统调用
足迹太大，无法安全列入许可名单，因此保留默认操作
宽松 — 但节点声明的*功能*减去系统调用：a
声明没有 `"net"` 能力的节点具有 `socket`/`connect`/`bind`/…
seccomp 阻塞 (EPERM)；无`"exec"`能力块`execve`/`execveat`。
该保单*源自已声明的合同*，而不是手写的。这个
已通过负面测试进行验证 - **相同** python，在**相同**下
沙箱，仅声明的上限不同：

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```
因此它不仅仅是网络命名空间：没有 `net` 上限，
`socket()` *系统调用*失败（网络顶部的深度防御）。诚实
范围：这是**容器级 + 合约派生的 seccomp 块列表**，
不是完整的系统调用允许列表 - 通过未阻止的系统调用进行内核利用是
仍然没有被收容。更严格的每个节点白名单（以及基于能力的
秘密调解）是记录在案的下一步。

## 进化——模因（达尔文+拉马克）

离线的 `the_beast.cpp` 有意设置 `run_evaluation=false`，因此只按结构有效性
进行选择。通用 evolution API 也可以执行任务，并将输出与预期通道值进行精确比较。

[`the_beast_evolve.cpp`](the_beast_evolve.cpp) 则使用连续距离指标，使接近目标的
结果能够继续改进，而不是都落入通用评分器的同一个输出不匹配类别。

- **任务**（真正的任务，输出评分 - 不是结构代理）：
  组装一个计算目标数字的算术管道。五操作
  节点存在 - `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` - 每个读取
  `acc` 通道（init 0），应用其操作，将其写回。执行框架的
  答案是执行后`acc`所成立的内容； **适应度=
  `-(|acc - 20|)`**。 *拓扑*（哪些操作运行，按什么顺序）
  决定了数量，因此不断发展的接线也会不断发展计算。
- **达尔文**：随机重新布线（`all_operators()`）+选择
  测得的输出 — 跌跌撞撞地接近 20。
- **拉马克**：LLM进行算术，连接一条达到 20 的链
  准确地说，并将获得的溶液作为可遗传的种子注入。

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
对比就是重点：**盲目突变跌跌撞撞地走向了
目标**（第9代5→10→24→19→20，通过尝试计算数量）；
**LLM进行算术** - `(0+2)*5*2 = 20` - 并直接跳至
注射时的答案。因为获得的解决方案成为
可遗传的冠军（`origin 'LLM'`），是拉马克式的；盲目变异+
选择是达尔文式的；运行两者是一种模因算法。

诚实的说明：纯粹的达尔文主义是经过离线验证和确定性的。的
拉马克 LLM 调用 (deepseek-v4-flash) **偶尔不稳定** —
流式回复有时会返回无法解析的结果，在这种情况下，运行
日志`[Lamarckian] LLM returned no parseable harness`和达尔文主义
进行回合；最后一行报告了冠军的*实际*起源，
拉马克式的胜利从来都没有发生过。

## Gate-eval — 一致性门控真的健全吗？

Beast 的整个安全论点是静态验证器是一个*健全的*
coherence oracle：错误意味着执行框架确实会出现错误
运行时；没有错误就代表运行了。这是**断言的，而不是测量的**——
每个审稿人问的第一件事。

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp) 对其进行测量。它运行一个
通过验证器标记拓扑语料库（预测结果）并且
通过引擎（地面实况）和交叉检查。离线、确定性、
无密钥 — `exit 0` 当且仅当每个判决都匹配执行时，所以 **CI 可以启动
健全性**。

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

> 验证器报告错误 ⟹ 执行时图出现故障；
> 验证器报告没有错误 ⟹ 图执行干净。

第一行是*健全性*（运行干净的带有错误标记的图将是
健全性漏洞）；带有警告标志的图运行干净，显示了大门
不*过度*拒绝。 E10/E8 级错误仅可判定 — 运行
空路由映射取消引用`rend()`（UB），这正是错误
门的存在是为了防止，所以它被检查但不被执行。这是一个
演示语料库，并没有详尽地涵盖每一个诊断——但它
将“大门完好无损”从一句口号变成了经过深思熟虑、CI 强制实施的 4/4。

## Gate-fuzz——大规模的保证及其边界

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp)将gate_eval从5推入
手工标记的案例到数千个模糊的案例——但诚实。天真的举动
（模糊 N 图，打印精度 1.0）将是戏剧：**引擎重新运行
编译时的验证器并抛出任何错误**，因此“验证器错误⟹
发动机故障”是真的*通过构造*。所以该程序衡量两件事
实际上信息丰富：

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
- **第 1 层 — 规模一致性。** 使用随机模糊连贯种子
  结构突变（悬空边→E3，未声明的写入→E4，孤儿作家，
  下降沿 → E7 *警告*，额外有效沿）。编译器有超过 2000 个突变体
  门和引擎永远不会不一致。这不是一个健全的*发现*（这是
  部分是通过构造）——这是一个**回归保证**：如果未来发生变化
  使静态门和运行时出现分歧，这会失败。
- **第 2 层 — 边界。** 门信任每个节点声明的 **效果
  合同**。 *谎言*的节点 — 声明 `writes:["out"]` 但实际写入
  运行时未声明的 `phantom` 通道 — 驶过静态门
  (500/500)，并且**运行时`GraphState`写保护**捕获每一个
  (500/500)。这不是门错误；而是错误。这是设计好的分工。

结果是一份“精确”的保证声明，比一份更诚实的保证
可疑的完美混淆矩阵：**静态门相对于
诚实的合约，为不诚实的合约提供运行时保障**——第 1 层和
第 2 层每个 CI 强制执行。

正式同伴，[`SOUNDNESS.md`](SOUNDNESS.md)，*证明*了这一点：一小步
超步执行的语义，效果格`(𝒫(Chan), ⊆)`，门为
良构性判断`⊢ G ok`，以及进步定理（过门图
在诚实合同下永远不会导致故障），并且诚实假设被证明是必要的
运行时写保护作为其故障停止后备。每个场所都经过检查
针对发动机源； `gate_eval`/`gate_fuzz` 是模型的保真度
检查。这里的两个执行框架是该文档的 Cor 6.4 和 Prop 6.5，运行。

## Baldwin — 模因会盲目吗？继承重要吗？

`evolve`变体显示达尔文突变+拉马克LLM注射。
每个审稿人提出的更尖锐的研究问题：**是否有一项任务
盲目进化和一次性求解器都停滞不前，但模因组合
获胜——你继承学到的特质*如何*改变结果
文献预测？**（Whitley 1994；Hinton & Nowlan 1987。）

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp)是那个实验，跑完了
真正的 NeoGraph 执行框架。基因组是仿射管道的布线；每个
舞台致力于终身学习的op **或左塑料（`?`）**
解决。适应度度是运行时**组装的执行框架的签名 - 并且
启动交叉检查证明快速分析适合度等于编译的适合度
引擎有 200 种拓扑（与 Gate-Eval 相同）。风景是
**欺骗性**：宽阔的诱饵山 (0.85) 随处可见，还有一个狭窄的、
**无梯度**全局HIGH原（1.0）仅*学习* - 搜索
塑料基因跨越的邻域——可以找到。

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```
关于适应性*是什么*的注释：每个基因组都会编译成真正的 NeoGraph
拓扑和交叉检查证明引擎运行了其中 200 个
分析模型预测——*基材*是真实的、忠实执行的
执行框架。遗传算法优化的“目标”是一个具有欺骗性的汉明景观
接线（动态的受控测试台），而不是原始执行
输出。这两个事实都被清楚地陈述而不是模糊不清。

遵循不同标准的两个发现：

1. **模因盲目击败（稳健——CI门控）。** 盲目的达尔文进化论
   只同化了全局约 25%——机会下限——因为HIGH原有
   没有已提交空间梯度，因此选择会跟随诱饵并被捕获。
   学习暴露HIGH原并吸收它~75%。门断言
   *边际*（意味着超过 24 个种子），不是每次运行的阈值计数，因为
   每次运行的计数由初始运气推动； 25% 与 75% 的利润率是稳定的
   信号。
2. **鲍德温控制（测量 - 从未门控）。**鲍德温（不继承
   习得性状）与拉马克（将其写入基因组）：此处 **74% vs 78%**
   全局——拉马克稍微领先，这是景观上的“预期”结果
   这是欺骗性的，但不是对抗性的（回写的速度超过了它的速度）
   多样性成本）。惠特利的**逆转**（鲍德温>拉马克）需要一个
   特别是对抗性的景观；这个简单的两峰结构并不
   有力地展示它，这是**诚实地报道的，而不是侥幸。**
   （这确实很微妙：带有基于指数的平局决胜规则的早期版本
   *似乎*显示了逆转——一个神器。选择边界处的联系
   现在被每个种子随机抽签打破并**在扫描过程中取平均值**，并且
   ~74-vs-78 排序在种子库中是稳定的；明显的逆转确实
   无法在该修复中幸存。）

这是审稿人要求的结果的诚实形式：强有力的主张
（学习引导的进化解决了盲目进化无法解决的问题）被测量和
CI 强制执行；微妙的主张（非继承优于继承）是经过衡量的
并按原样报告，并明确指出而不是隐藏负面结果。

## Baldwin-adv — 对抗性景观 + 真正的爬山学习

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp) 锐化两侧
之前的实验。学习现在**真正的本地搜索**（多次重启
越过可塑性基因达到局部最优——离散模拟
炼油厂的，以及LLM插入的插槽），风景真的很美
**对抗性**：一座宽阔的诱饵山，其梯度点*远离*一个小山，
陡峭的全局球。盲目的已提交空间搜索不属于机会层——它
沿着诱饵梯度积极地**欺骗**。

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```
- **模因≫盲目（稳健，CI门控）。**达尔文被诱饵欺骗
  （~5% 全局/~92% 诱饵）；学习发现全局球只有一个基因组
  不能（76-98%）。这是比HIGH原“更强”的分离——盲区
  基线被误导，而不仅仅是盲目的。跨种子基地稳定（达尔文 2-5%，
  学习者 76-98%）。
- **鲍德温 vs 拉马克：逆转不会重现。** 拉马克
  回写以稳定的优势获胜（98% vs 76%）。一个参数扫过
  发现整个可到达/不可到达边界（30 多个配置，三次扫描）**没有
  制度**，其中非继承有力地击败了回写：当全局是
  可达，回写速度占主导地位；如果不是，则两者都会失败，只有一个
  边（~3-4 分）鲍德温多样性边。这是诚实的经验答案
  到“惠特利的鲍德温>拉马克逆转是否在执行框架上重现
  拓扑？” — **不**，在这个离散的政权中，程序是这么说的
  命名的机制。 （惠特利的反转建立在*连续*
  具有实值局部搜索的多模态函数；离散拓扑-遗传算法
  这里就不展示了。）

## Baldwin-llm — 模型是学习算子

上面的机械学习者（随机猜测、爬山）始终是*槽*
一个LLM精炼器插入。 [`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)
将其插入模型实际上可以推理的任务：填充`?`阶段
算术流水线的一部分，因此`acc`达到目标。 **学习运算符是
模型**（它为`?`阶段选择操作）；适应度是集合体
执行框架*运行*。鲍德温/拉马克的切换变得字面意思：

- **Baldwinian** 对模型的填充进行评分，但保留基因 `?` — 模型必须
  下一代**再次**咨询。学习不是遗传的。
- **拉马克**将填充写入基因组 - `?` 被提交。
  获得的性状是**可遗传的**；无需再次查阅该模型。

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
可观察到的差异不是适应性（都达到了目标），而是**基因组
经济**：拉马克将学习者的工作存入遗传（基因被提交，调用
降至零）；鲍德温重新学习每一代（基因保持可塑性，呼吁
保持HIGH位）。使用确定性 **oracle** 学习器（默认）离线运行，或者
`--llm` 与 `OPENROUTER_API_KEY` 使**模型**成为学习者 - 其中
如果这些调用是真正的 API 调用，那么遗传实际上就是
为模型支付一次费用和每一代都支付费用之间的区别。这是
“模型的修正是否可以遗传？”的具体含义— 显示为
跟踪，未断言。 （`--llm`路径需要网络；它回退到
oracle 并记录任何调用/解析失败，因此演示始终完成。）

## 小说家 — 前提输入，轻小说长度`.txt`输出

最简单且真正实用的写作执行框架，以及诚实的形态
“小说家”理念：给它一个前提，拿回一整本轻小说
手稿为纯文本。 [`the_beast_novelist.cpp`](the_beast_novelist.cpp) 是
**迷失在中间**问题的具体呈现——一个长篇故事*不是*在单个大上下文中
写成的。这是一个关于**显式故事状态**的小图：

渠道：前提·大纲·圣经·摘要·书籍·idx·总计

所以每一章都是根据紧凑的外部化状态生成的**新鲜的**
（大纲节拍、故事圣经、连续摘要）而不是重新阅读 60k
字符。该模型永远不必“记住”小说中的角色是谁 -
它读取`bible`通道。

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```
该图是`__start__ → planner → writer ⟲`：`planner`将前提变成
大纲+初始圣经； `writer`将章节`idx`写入`book`并且
**更新`summary`和`bible`**，以便下一次迭代保持基础，然后
**使用 `Command` goto** 进行自循环，直到 `idx+1 == total`。效果契约已
声明，因此**一致性门控在写入一个词之前就证明了接线的正确性** —
每个故事状态通道都被实际消耗，没有悬空的阶段。

**批量生成、批量进化——每章风格各异。**每一章
实际上是一个独立的子代理（每次`writer`调用仅基于
共享的故事状态）。为了防止各章雷同，作者为每章发展了一种
每章 **风格基因组** — 5 维风格空间中的一个点（POV · 紧张 · 情绪 ·
镜头·步调，480种组合）。它运行一个迷你 GA（`baldwin`模因循环，
针对*品种*而不是目标）：一批候选基因组进化为
最大化 **新颖性**（即与已用风格的距离） - 那么获胜者是
提交并推入`styles_used`，因此下一章被迫远离
它。离线样式跟踪是确定性的并且明显变化：

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```
新颖性搜索最大限度地提升独特性（它不能*保证*每个维度
都不同）——诚实的表述，已足以打破单一模型在长篇写作中语调单一的局限。

离线（无密钥）**确定性存根**规划者/编写者运行*完全相同的
图*，因此管道——状态线程编排、goto 循环、累加、
`.txt`输出——无需网络即可验证； `OPENROUTER_API_KEY`切换为
真实模型的散文。诚实的范围：门控证明了*数据通路*
（故事状态通道的接线和线程编排），而不是*散文质量*——叙事质量是
模型的工作；结构之外的连续性可以由一个检查器节点
（运行时后停止模式）来承载，这是最自然的下一步。

## 暴露的边缘问题

- **E6 在 `trail`** 上“已写入但从未读取”被作为 lint 发出 — 并且它
  是*正确*：`trail`是终端输出通道，没有下游
  *节点*消耗；只有主机通过`RunResult::channel`读回它。
  验证器对图的通道表面非常精确，而不是
  错了。故意保持可见以显示效果分析的工作情况。
- **序列化检查点状态是通道包装的**
  (`channel_values["channels"]["trail"]["value"]`)，不平坦——演示的
  `channel_of()` 助手将其打开。相同形状`RunResult::channel`
  读。
- 核心锁文件通过阐述保留了`schema_version: 1`，这
  是选择 Gate 2 进入严格模式的原因 - 在 DSL 表层进行创作
  绝不默默降级一致性保证进化循环
  取决于。
