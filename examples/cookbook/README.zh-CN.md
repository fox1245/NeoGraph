<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=2b960566263f063bf11a97a63b315005e7ab13700b5839294441f20eb52f6256 -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

端到端配方，将多个 NeoGraph 功能组合成实际可运行的场景。每个配方都是自包含的：复制文件夹，按其README操作，即可运行。

| Cookbook | 它所展示的内容 |
|---|---|
| [`the-beast/`](the-beast/) | **一个基于自演化的智能体：生成 · 演化 · 回滚。** The Beast 编写严格的 Core JSON，在执行前进行语义验证，使用`evolve()`演化其有界 Core 拓扑，并通过检查点回滚。live（活体）、apex、forge、script 和 arithmetic-evolution 变体保留了相同的 compiler/validation 边界； JavaScript 或受信任的 C++ 负责源码编写，而严格的 Core JSON 仍作为数据交换格式。 |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A：4名国民议会议员（每个代表都拥有自己的 A2A 端点）+ 一名议长，并行广播一项法案并集中计票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | 自带自己的`openai.OpenAI()`客户端：继承NeoGraph的`Provider`，将每次LLM调用委托给SDK，保留你所有的重试/Azure/可观测性配置。同时包括：通过agentic-provider模式的工具调用。 |
| [`jarvis/`](jarvis/) | **语音驱动的元编排器（骨架）。** 麦克风 → whisper.cpp（自动检测语言）→ 路由器（直接 / 委托 / 并行 3-way）→ MCP 工具或 A2A 专家 → 超音设备端 TTS，使用用户检测到的语言。 JSON 驱动的工具 + agent 目录，A2A 双向（JARVIS 本身也可达 可达）。设备端，无需云端。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP客户端往返通信，**无需LLM、无需API密钥、无需fastmcp**：一个约60行的stdlib stdio服务器 + 一个C++测试框架，它执行`initialize` → `tools/list` → `tools/call`。这表明NeoGraph的MCP客户端只需要一个说线路协议的进程——对等体可以是任何东西。 |
| [`openrouter-provider/`](openrouter-provider/) | OpenRouter固定的DeepSeek提供方接口：内置的`OpenAIProvider`针对兼容性端点和直接HTTP的自定义Python`Provider`。 |

每份cookbook还记录了它暴露的摩擦点——对于寻找公共API的粗糙边缘很有用。
