<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=zh-CN source_sha256=837a72c1600b8f89e2a5600d0ea31ad8fb44f043e2e9c11d49ff18551164f306 -->
# 使用你自己的 OpenAI 客户端

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

多数生产环境 Python 用户已经有自己的 `openai.OpenAI()` 客户端实例，
并带有自己的重试、自定义传输层、可观测性钩子，
或 OpenRouter 路由。本 cookbook 展示如何把那个已有客户端
作为自定义 `Provider` 插入 NeoGraph — 而不是使用 NeoGraph 内置的 `OpenAIProvider`。

诀窍是：NeoGraph 的 `Provider` 从 v0.2.3+ 开始可以在 Python 中继承。
子类的 `complete(params)` 会在图 node（LLMCallNode、ReAct 循环等）中运行，
就像内置 provider 一样运行。

## 何时使用哪一种

| 你想要 | 使用 |
|---|---|
| “只想通过 OpenRouter 使用固定 DeepSeek 路由” | 使用官方 `openai` SDK 的 `OpenAISdkProvider` |
| “我已经配置好带重试 / Azure / 代理 / 钩子的 `openai.OpenAI()`” | 本 cookbook（继承 `Provider`，委托给你的客户端） |
| “我通过官方 `openai` SDK 使用 OpenRouter API” | 本 cookbook，使用固定 DeepSeek 模型 |
| “我想在测试中模拟 LLM” | 本 cookbook，配合确定性的 stub |

重点是：**NeoGraph 的图引擎不关心 LLM 调用是怎么发生的** —
它只需要 `params -> ChatCompletion`。

## 全部内容只有 60 行

见 [`hybrid.py`](hybrid.py)。关键形状如下：

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "deepseek/deepseek-v4-flash-0731"):
        super().__init__()
        self.client = client
        self.model  = model

    def complete(self, params: ng.CompletionParams) -> ng.ChatCompletion:
        # Translate NeoGraph params into the SDK's chat-completions shape.
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        resp = self.client.chat.completions.create(
            model=params.model or self.model,
            messages=messages,
            temperature=params.temperature,
        )
        # Translate back into NeoGraph's response shape.
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = resp.choices[0].message.content or ""
        return out

    def get_name(self) -> str:
        return "openai-sdk"
```

就是这样。把 `OpenAISdkProvider(OpenAI(api_key=...))` 传入一个
`NodeContext`，任何使用 `llm_call` node 的 NeoGraph 图都会通过 SDK 路由 —
同时保留所有已经挂在 SDK 客户端上的重试 / Azure / 可观测性 / 代理配置。

## 运行

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
python hybrid.py
```

输出：
```
[hybrid] using openai SDK inside NeoGraph 0.2.3 graph
[hybrid] running one llm_call through the OpenAI SDK provider
[provider] complete() call #1 (2 msgs) — model=deepseek/deepseek-v4-flash-0731
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

内置 `llm_call` 使用共享的 `NodeContext.instructions` 作为 system prompt。
当不同图阶段需要不同 prompt 时，请使用自定义 node type。

## 你保留的内容

- 你的 `openai.OpenAI()` 客户端的 `default_headers`、重试策略、
  自定义 `http_client=httpx.Client(...)`、Azure / 代理配置。
- 挂在 SDK 层的 `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` /
  `weights & biases` 集成 — 它们会拦截每次调用。
- 你对 `usage`（token 数）、错误、重试的现有跟踪。

## 相比 `neograph_engine.llm.OpenAIProvider` 放弃的内容

- 原生 HTTP 路径（asio + 连接池）— 约比 SDK 快 1.5×，且没有 GIL 争用。
  如果瓶颈是 OpenAI 调用，SDK 没问题；如果瓶颈是框架开销，原生版本会赢。

## 工具调用 — 三种可用模式

Provider trampoline 允许 `complete()` 干净地返回 `tool_calls`。
目前**不能工作**的是 C++ `tool_dispatch` 图 node 回调到 Python `Tool` 子类 —
这条路径会段错误（已有 issue；v0.3 跟踪）。今天有三种模式可用：

### A. Agentic Provider（`byo-openai` 推荐）

把工具循环放在 `complete()` **内部**。用户的 `openai.OpenAI` 客户端已经支持 tool-calling；
让它完成 agentic loop（调用 → 在 Python 中分发 → 结果 → 调用 → 文本），
并只把最终 assistant message 返回给 NeoGraph。图的每个“turn”只看到一次 `complete()`，
不需要 `tool_dispatch` node。

```python
class AgenticOpenAIProvider(ng.Provider):
    def __init__(self, client, tools_by_name):
        super().__init__()
        self.client = client
        self.tools  = tools_by_name      # {"calc": calc_fn, ...}
    def complete(self, params):
        messages = [{"role": m.role, "content": m.content} for m in params.messages]
        sdk_tools = [{"type":"function",
                      "function":{"name":n,"description":fn.__doc__ or "",
                                  "parameters":fn.schema}}
                     for n, fn in self.tools.items()]
        for _ in range(10):  # cap loops
            r = self.client.chat.completions.create(
                model=params.model or "deepseek/deepseek-v4-flash-0731",
                messages=messages, tools=sdk_tools)
            choice = r.choices[0]
            if not choice.message.tool_calls:
                out = ng.ChatCompletion()
                out.message.role    = "assistant"
                out.message.content = choice.message.content or ""
                return out
            messages.append(choice.message.model_dump())
            for tc in choice.message.tool_calls:
                fn = self.tools[tc.function.name]
                result = fn(**stdjson.loads(tc.function.arguments))
                messages.append({"role":"tool","tool_call_id":tc.id,
                                 "content":str(result)})
```

取舍：NeoGraph 看不到中间步骤（没有每次 tool call 的检查点），
但你保留了所有 SDK 行为，并且没有分发边界摩擦。

### B. C++ 工具 + Python Provider

使用内置 C++ 工具（来自 `neograph_engine.mcp` 的 `MCPTool`，
或任何其他 C++ 侧 `Tool`）作为分发路径，并用你的 Python Provider 处理 LLM 调用。
图中的 `tool_dispatch` node 可以正常调用 C++ 工具；只有回调到 Python `Tool` 子类
的那条路径会崩溃。

### C. Provider 返回 tool_calls；自定义 Python node 负责分发

跳过内置 `tool_dispatch` node。自己写一个 `@ng.node("dispatch")`，
读取 `messages[-1].tool_calls`，直接调用你的 Python 工具，
并把工具结果消息写回。全程留在 Python 中。

## A2A + 自定义 Provider

本 cookbook 可以自然地与 [ai-assembly cookbook](../ai-assembly/) 组合 —
把每个成员的 provider 替换为 `OpenAISdkProvider(...)`，
即可在每个 persona 上获得所有 SDK 层行为，同时仍然使用 NeoGraph 的 A2A 桥接。
