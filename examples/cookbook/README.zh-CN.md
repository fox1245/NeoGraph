<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=b668003b55bbf84e6463dc6dbc7c708f77d62a9face15528b6fc7e32caac0182 -->
# NeoGraph Cookbook

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 切换处置：[`spec/neograph-example-disposition-v1.json`](../../spec/neograph-example-disposition-v1.json)。

这些端到端示例会把多个 NeoGraph 功能组合成一个真实可运行的场景。
每一个都是自包含的：复制该文件夹，按它的 README 操作，然后运行。

| 示例 | 展示内容 |
|---|---|
| [`the-beast/`](the-beast/) | **会自我演化的智能体：生成、演化、回滚。** The Beast 会 (1) 在 DSL 层编写一个 NeoGraph 拓扑，并通过三道门控（elaborate → strict compile + translation validation → static validate）证明它前后一致，(2) 通过 `evolve()` 使用真实的变异算子演化它，并把编译器当作适应度门控，(3) 用检查点器生成幸存者，并把它的运行回滚到任意先前的 super-step（`load_by_id` 时间旅行）。这个类别只有 NeoGraph 能安全做到：执行框架本身就是数据，而且 DSL 编译器会在任何节点运行之前证明它前后一致。它包含离线 stub 生成器；一个 **实时变体**，其中 DeepSeek v4 pro (OpenRouter) 会真正编写执行框架，并通过编译器诊断自修复循环来修正；一个 **顶点变体**，其中模型吞下工具目录，并编写一个能自主调用已绑定工具的 ReAct 智能体；一个 **锻造变体**，通过 MCP 发现工具，并把缺少的工具*写成* Python MCP 服务器；一个 **脚本变体**，其中模型会编写节点逻辑（运行模型所写代码的 `script_node`，控制自己的 `goto` 流，可选 Sandbox2 隔离）；以及一个 **演化变体**，也就是带有真实、按输出打分适应度的记忆演化循环（演化算术流水线的接线，直到它能计算目标）：达尔文式变异/选择（已验证能在离线环境中爬升）加上 Lamarckian LLM 对已获得解法的注入。它还附带一个 **gate-eval** 执行框架，用真实执行来经验性地交叉检查一致性验证器的判定（可纳入 CI 的可靠性）；一个 **gate-fuzz**，把它扩展到数千个 fuzz 生成的变异体，并诚实描绘保证边界（相对于诚实的效果契约是可靠的；运行时会兜住说谎契约 500/500）；一份形式化配套文档 **SOUNDNESS.md**，在小步语义 + 效果格上证明 Progress/non-fault theorem；一个面向沙箱的 **contract-derived seccomp** 层；以及一对 **baldwin**，对执行框架拓扑进行 Darwinian vs Baldwinian vs Lamarckian 记忆演化：基础变体（无梯度全局平台期）和一个 **对抗变体**（`baldwin_adv`，欺骗性梯度地形 + 真实爬山学习，其中盲目搜索会被主动*误导*）。二者都通过 CI gate 表明，由学习引导的演化能到达盲目演化无法到达的全局解，并诚实报告 Baldwin-vs-Lamarck 比较：一次 30 配置扫描发现 Whitley Baldwin>Lamarck 反转在离散拓扑上**没有**复现（已测量、已命名机制，而非假设）。随后 **baldwin_llm** 演示让模型本身成为学习算子（填充算术流水线里的 `?` stage），因此开关是字面意义上的：Baldwin 每代重新咨询模型，Lamarck 把修复写入遗传（调用更少）。而 **novelist** 会把一行前提句转成轻小说长度的 `.txt`：一个运行在显式故事状态上的图（outline/bible/summary 通道 + 自循环写作节点），解决“lost in the middle”问题：每一章都针对紧凑的外部化状态新鲜生成，而不是针对全部先前文本；在写出一个字之前，一致性门控会先证明接线正确（离线确定性 stub；真实正文使用 `OPENROUTER_API_KEY`）。 |
| [`ai-assembly/`](ai-assembly/) | 多 persona A2A：4 名国会议员（每个都有自己的 A2A 端点）+ 一个 Speaker，并行广播法案并统计投票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | 带上你自己的 `openai.OpenAI()` 客户端：继承 NeoGraph 的 `Provider`，把每一次 LLM 调用委托给 SDK，同时保留你自己的重试 / Azure / 可观测性配置。另含：通过 agentic-provider 模式实现工具调用。 |
| [`jarvis/`](jarvis/) | **语音驱动的元编排器（骨架）。** 麦克风 → whisper.cpp（自动检测语言）→ router（direct / delegate / parallel 三路）→ MCP 工具或 A2A 专家 → supertonic 设备端 TTS，使用检测到的用户语言。JSON 驱动的工具 + 代理目录，A2A 双向（JARVIS 自身也可被访问）。设备端运行，零云依赖。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP 客户端往返，且**没有 LLM、没有 API 密钥、没有 fastmcp**：约 60 行 stdlib stdio 服务器 + 一个执行 `initialize` → `tools/list` → `tools/call` 的 C++ 执行框架。展示 NeoGraph 的 MCP 客户端只需要一个会说线协议的进程 — 对端可以是任何东西。 |
| [`ollama-provider/`](ollama-provider/) | 通过 Ollama 使用本地 LLM。两条路径：内置 `OpenAIProvider` 对接 Ollama 的兼容端点（零新代码），或者自定义 `Provider` 对接原生 `/api/chat`。完整代理栈，不需要外部 API 密钥。 |

每个示例也会记录它暴露出的摩擦点 — 这有助于找到公开 API 的粗糙边缘。
