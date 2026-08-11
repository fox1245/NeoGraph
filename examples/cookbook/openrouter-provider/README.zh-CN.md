# NeoGraph + OpenRouter（固定 DeepSeek）

**语言:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

让 NeoGraph graph 使用 OpenRouter 的固定模型
`deepseek/deepseek-v4-flash-0731`。本 cookbook 展示两个使用同一 API key、
模型和显式 zero-data-retention (ZDR) provider 偏好的 Provider surface：

1. 内置 `OpenAIProvider` 调用 OpenRouter 的 OpenAI-compatible chat endpoint
   (`via_openai_compat.py`)；
2. 自定义 Python `Provider` 直接发送 normalized chat request
   (`via_http.py`)。

第二条路径说明 NeoGraph 的 `Provider` trampoline 与 transport 无关，同时
把 wire contract 保持为显式代码。

## 路径 A — 内置 Provider

`OpenAIProvider` 会自动追加 `/v1/chat/completions`，因此传入不带 `/v1`
后缀的 OpenRouter API base URL：

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="deepseek/deepseek-v4-flash-0731",
    provider_routing={"zdr": True},
```

完整代码见 [`via_openai_compat.py`](via_openai_compat.py)。

## 路径 B — 自定义直接 HTTP Provider

[`via_http.py`](via_http.py) 继承 `Provider`，向
`https://openrouter.ai/api/v1/chat/completions` POST 请求，并把
`choices[0].message` 与 token usage 映射回 NeoGraph 响应。请求也设置了
OpenRouter 的 ZDR provider 偏好。

## 运行

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

两个 demo 都构建严格的 one-node graph，并通过同一个固定 DeepSeek 模型
执行 `llm_call`。system prompt 来自共享的 `NodeContext.instructions`。
缺少 key 时会明确报错退出；live 路径不会静默切换到 mock provider。

## 输出形状

```text
[openrouter] using deepseek/deepseek-v4-flash-0731 via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

实际 completion 会变化。直接 HTTP 路径会打印 `via direct HTTP`，并使用
自己的算术问题。

## 为什么保留两条路径？

- 路径 A 使用 NeoGraph native HTTP 实现和 connection pool。
- 路径 B 是应用代码自己拥有 custom headers、transport policy 或响应转换
  时的最小示例。
- 两条路径使用完全相同的 OpenRouter endpoint 系列、API key 和固定
  DeepSeek 模型，因此 Provider surface 的比较有意义。

OpenRouter API 请求/响应格式：
<https://openrouter.ai/docs/api-reference/overview>
