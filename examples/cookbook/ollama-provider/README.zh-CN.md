<!-- neograph-i18n: source=examples/cookbook/ollama-provider/README.md locale=zh-CN source_sha256=e9208ffec69b9c3d4b86998e648ea60f8b2f9c217008241cd11b71af7c346bed -->
# NeoGraph + Ollama（本地 LLM）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

让 NeoGraph graph 使用 [Ollama](https://ollama.com) 上本地服务的模型 — 无 API key、无网络出站、完整隐私。两条路径，都可在 NeoGraph 0.2.3+ 使用：

## 路径 A — 用 `OpenAIProvider` 调 Ollama 的兼容 endpoint *(无需新增代码)*

Ollama 暴露了 `/v1/chat/completions`，其 OpenAI request/response shape 兼容。把 NeoGraph 内置的 `OpenAIProvider` 指向它即可 — 同样的 native asio HTTP path、同样的 connection pool、同样的速度。

```python
from neograph_engine.llm import OpenAIProvider
# OpenAIProvider appends `/v1/chat/completions` itself, so base_url
# is the bare host — NOT host + "/v1". Ollama returns 404 otherwise.
provider = OpenAIProvider(
    api_key="sk-anything",                       # required field, value ignored
    base_url="http://127.0.0.1:11434",            # NOT ".../v1"
    default_model="qwen2.5:0.5b",
)
```

这就是完整集成。参见 [`via_openai_compat.py`](via_openai_compat.py)。

**何时选择路径 A**：你想要速度，且不关心 Ollama 专有功能（model pull/load、带 native fields 的 streaming、`keep_alive` 和 `options` blocks）。适合 95% 的用户。

## 路径 B — 通过自定义 Python `Provider` 调原生 Ollama API

演示 v0.2.3 的 `Provider` trampoline 确实不绑定特定厂商。它包装 Ollama 原生 `/api/chat`（参数更丰富，无兼容层转换），文件为 [`via_native_api.py`](via_native_api.py)。

```python
class OllamaProvider(ng.Provider):
    def __init__(self, model="qwen2.5:0.5b", host="http://127.0.0.1:11434"):
        super().__init__()
        self.model = model
        self.host = host
    def complete(self, params):
        r = httpx.post(f"{self.host}/api/chat", json={
            "model": params.model or self.model,
            "messages": [{"role": m.role, "content": m.content}
                         for m in params.messages],
            "stream": False,
            "options": {"temperature": params.temperature},
        }, timeout=120)
        r.raise_for_status()
        body = r.json()
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = body["message"]["content"]
        return out
    def get_name(self): return "ollama"
```

**何时选择路径 B**：你需要 Ollama 原生字段（`options`、`keep_alive`、`format=json` constraint），或者你的团队已经使用 `ollama` Python SDK，并希望保留它作为客户端接口。

## 运行

```bash
# 1. start Ollama (separate terminal)
ollama serve

# 2. pull a small model so it fits in CI / laptop RAM
ollama pull qwen2.5:0.5b      # ~400 MB

# 3. install NeoGraph + httpx (Path B uses httpx)
pip install neograph-engine>=0.2.3 httpx

# 4. run either path
python via_openai_compat.py
python via_native_api.py
```

两个 demo 都构建严格的 one-node graph，并将其 `llm_call` 路由到本地模型。System prompt 来自共享的 `NodeContext.instructions`。不需要外部 API key。

## 输出

```
[ollama] using qwen2.5:0.5b at http://127.0.0.1:11434 (OpenAI-compat path)
[user] What's the capital of France?
       user: What's the capital of France?
  assistant: Paris is the capital of France.
```

（实际 completions 会随模型变化。原生路径会打印 `(native /api/chat path)`，并使用自己的示例问题。）

## 说明

- **为什么是 `qwen2.5:0.5b`？** 这是能处理英语 + 简单推理 的最小主流模型。冷启动 2-3 s 加载，之后 CPU 上约 ~100 ms / completion。很适合 cookbook demo。确认 loop 可用后，可换成 `llama3.2:3b` / `qwen2.5:7b` / `phi4:14b`。
- **第一次调用很慢** — Ollama 在第一次请求时懒加载模型到内存中（sub-1B 约 ~2-5 s）。后续调用为 warm。`keep_alive` 参数（路径 B）控制模型空闲后保持 loaded 的时长。
- **工具调用**：Ollama 的 tool-calling 支持依赖模型，并通过 `/v1/chat/completions` endpoint 使用 OpenAI 兼容形状。用路径 A + [`../byo-openai/hybrid_with_tools.py`](../byo-openai/hybrid_with_tools.py) 中的 agentic-provider pattern，搭配支持工具的 Ollama 模型（例如 `qwen2.5:7b`）。
- **Streaming**：路径 A 继承 NeoGraph 通过 `OpenAIProvider.complete_stream` 提供的原生 streaming。路径 B 的示例不 streaming；如果需要，添加 `"stream": True` + chunk loop。

## 为什么这很重要

NeoGraph（5 µs engine 开销）结合本地 Ollama 模型，可以提供 **零外部依赖** 的完整 agent stack：

- 13 MB native binary + ~400 MB model = **可装进 Raspberry Pi 5**
- 无 API key、无 rate limit、无网络出站、完整数据隐私
- 图语义与 LangGraph 对齐；LLM 调用使用 Ollama 质量的模型
- engine 层每次迭代开销处在噪声中（500-2000 ms model inference 占主导），但框架占用明显小于 LangGraph + Python runtime

适用于：边缘 AI、设备端 agent、有隐私要求的部署、自托管助手，以及任何厌倦早期原型阶段 OpenAI 账单的人。
