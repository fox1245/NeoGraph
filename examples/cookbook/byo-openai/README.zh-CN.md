<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=zh-CN source_sha256=812a1f340ed6b8f92ddd742cc1c8f239265b501fa010c81929825f0973738e38 -->
# 自带自有 OpenAI 客户端

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

大多数生产环境中的 Python 用户已经拥有一个带有自己的重试机制、自定义传输、可观测性钩子或 OpenRouter 路由的 `openai.OpenAI()`∈客户端实例。本 Cookbook 展示了如何将该现有客户端插入 NeoGraph 作为自定义的 `Provider`，而非使用 NeoGraph 的内置 `OpenAIProvider`∈。

诀窍在于：NeoGraph 的 `Provider` 在 v0.2.3+ 中可进行 Python 子类化。子类的 `complete(params)` 会在图节点（LLMCallNode、ReAct 循环等）内部像内置 provider 一样运行。

## 何时使用哪一种

| 你需要 | 使用 |
|---|---|
| “仅通过 OpenRouter 使用固定的 DeepSeek 路由” | `OpenAISdkProvider` 使用官方 `openai` SDK |
| 我已经配置好了一个包含重试 / Azure / 代理 / Hook的`openai.OpenAI()`。 | 本手册（子类`Provider`，委托给您的客户端） |
| "我通过官方 `openai` SDK 使用 OpenRouter API" | 本指南固定使用DeepSeek模型 |
| "我想在测试中 mock LLM" | 本指南使用确定性存根 |

要点：**NeoGraph的图引擎不关心 LLM 调用如何发生** ——它只需要 `params -> ChatCompletion`。

## 整个事情在60行内

参见 [`hybrid.py`](hybrid.py)。关键形状：。

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "~deepseek/deepseek-v4-flash-latest"):
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

就是这样。将 `OpenAISdkProvider(OpenAI(api_key=...))` 传入 `NodeContext`，任何使用 `llm_call` 节点的 NeoGraph 图都将通过 SDK 路由——保留你附加到 SDK 客户端的所有重试/Azure/可观测性/代理配置。

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
[provider] complete() call #1 (2 msgs) — model=~deepseek/deepseek-v4-flash-latest
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

内置的 `llm_call` 使用共享的 `NodeContext.instructions` 作为其系统提示。当不同图阶段需要不同提示时，使用自定义节点类型。

## 您保留的

- 您的 `openai.OpenAI()` 客户端的 `default_headers`、重试策略、自定义 `http_client=httpx.Client(...)`、Azure / 代理配置。
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` / `weights & biases` 集成附加在 SDK 层——它们拦截每一次调用。
- 您现有的对 `usage`（token 计数）、错误、重试的跟踪。

## 与 `neograph_engine.llm.OpenAIProvider` 相比，您所放弃的内容

- 原生HTTP路径（asio + 连接池）——比SDK快约1.5倍，且零GIL争用。如果瓶颈在OpenAI调用，SDK即可；如果瓶颈在框架开销，则原生路径更优。

## 工具调用——三种可行的模式

Provider 跳板让 `complete()` 能干净地返回 `tool_calls`。目前**不可用**的是 C++ `tool_dispatch` 图节点回调 Python `Tool` 子类——该路径存在段错误（这是之前的问题，已跟踪至 v0.3）。三种模式目前可行：

### A. Agentic Provider（推荐用于 `byo-openai`）

在**内部**执行工具循环 `complete()`。用户的 `openai.OpenAI` 客户端已支持工具调用；让其完成智能体循环（调用 → 在 Python 中分发 → 结果 → 调用 → 文本），并仅将最终助手消息返回给 NeoGraph。该图谱在每次“轮”中恰好看到一个 `complete()` ，无需 `tool_dispatch` 节点。

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
                model=params.model or "~deepseek/deepseek-v4-flash-latest",
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

权衡：NeoGraph 看不到中间步骤（没有针对每次工具调用的检查点），但您保留了所有 SDK 行为，且没有分派边界摩擦。

### B. C++工具 + Python Provider

使用内置的C++工具（`MCPTool` 来自 `neograph_engine.mcp`，或任何其他C++侧 `Tool`）用于调度路径，并将您的Python Provider用于LLM调用。图的 `tool_dispatch` 节点调用C++工具正常；只有回调到Python `Tool` 子类的调用会崩溃。

### C. Provider返回tool_calls；自定义Python节点进行分发

跳过内置的`tool_dispatch`节点。编写你自己的`@ng.node("dispatch")`，读取`messages[-1].tool_calls`，直接调用你的Python工具，并将工具结果消息写回。完全保持在Python中。

## A2A + 自定义Provider

本菜谱可以自然地和 [ai-assembly 菜谱](../ai-assembly/) 组合 — 将每个成员的provider替换为`OpenAISdkProvider(...)`，以便在每个人格上获得所有SDK级行为,同时仍然使用NeoGraph的A2A桥接。
