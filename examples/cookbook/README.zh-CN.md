<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=b668003b55bbf84e6463dc6dbc7c708f77d62a9face15528b6fc7e32caac0182 -->
# NeoGraph Cookbook

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 切换处置：[`spec/neograph-example-disposition-v1.json`](../../spec/neograph-example-disposition-v1.json)。

这些端到端示例会把多个 NeoGraph 功能组合成一个真实可运行的场景。
每一个都是自包含的：复制该文件夹，按它的 README 操作，然后运行。

| 示例 | 展示内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自我演化代理：生成、演化、回滚。** Beast 直接编写 strict Core JSON，在执行前编译和验证，通过 `evolve()` 演化 Core 拓扑，并使用检查点回滚。live、apex、forge、script 和 evolve 变体共享同一边界；JavaScript 或受信任的 C++ 负责源创作，strict Core JSON 仅作为互操作数据保留。 |
| [`ai-assembly/`](ai-assembly/) | 多 persona A2A：4 名国会议员（每个都有自己的 A2A 端点）+ 一个 Speaker，并行广播法案并统计投票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | 带上你自己的 `openai.OpenAI()` 客户端：继承 NeoGraph 的 `Provider`，把每一次 LLM 调用委托给 SDK，同时保留你自己的重试 / Azure / 可观测性配置。另含：通过 agentic-provider 模式实现工具调用。 |
| [`jarvis/`](jarvis/) | **语音驱动的元编排器（骨架）。** 麦克风 → whisper.cpp（自动检测语言）→ router（direct / delegate / parallel 三路）→ MCP 工具或 A2A 专家 → supertonic 设备端 TTS，使用检测到的用户语言。JSON 驱动的工具 + 代理目录，A2A 双向（JARVIS 自身也可被访问）。设备端运行，零云依赖。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP 客户端往返，且**没有 LLM、没有 API 密钥、没有 fastmcp**：约 60 行 stdlib stdio 服务器 + 一个执行 `initialize` → `tools/list` → `tools/call` 的 C++ 执行框架。展示 NeoGraph 的 MCP 客户端只需要一个会说线协议的进程 — 对端可以是任何东西。 |
| [`openrouter-provider/`](openrouter-provider/) | OpenRouter 固定 DeepSeek provider 示例：比较兼容 endpoint 的内置 `OpenAIProvider` 与直接 HTTP 的自定义 Python `Provider`。 |

每个示例也会记录它暴露出的摩擦点 — 这有助于找到公开 API 的粗糙边缘。
