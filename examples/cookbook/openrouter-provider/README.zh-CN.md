<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=zh-CN source_sha256=4d18b0fee54089948ca59063eb6177567e1b5db09a87123125e808bee6889add -->
# NeoGraph + OpenRouter（固定 DeepSeek）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

在OpenRouter固定的`~deepseek/deepseek-v4-flash-latest`模型上运行NeoGraph图。本手册展示两个等效的提供商表面，两者使用相同的API密钥、模型和显式的零数据保留（ZDR）提供商偏好：

1. 内建的 `OpenAIProvider` 针对 OpenRouter 的 OpenAI 兼容聊天端点（`via_openai_compat.py`）；
2. 一个自定义Python `Provider`，直接发布规范化聊天请求（`via_http.py`）。


第二条路径演示NeoGraph的`Provider`跳板与传输无关，同时保持线上协议显式。

## 路径A — 内置提供商

`OpenAIProvider` 会附加 `/v1/chat/completions` 本身，因此请传入不含路径的 OpenRouter API 基础 URL（`https://openrouter.ai/api`），而不是 `/v1` 后缀：

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="~deepseek/deepseek-v4-flash-latest",
    provider_routing={"zdr": True},
```

参见[`via_openai_compat.py`](via_openai_compat.py)。

## 路径B — 自定义直接HTTP提供商

[`via_http.py`](via_http.py)继承`Provider`，发布至`https://openrouter.ai/api/v1/chat/completions`，将`choices[0].message`映射回`ChatCompletion`，并将令牌使用情况复制到NeoGraph的响应中。请求还要求OpenRouter提供其ZDR提供商偏好。

## 运行

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

两个演示都构建了一个严格单节点图，并将其`llm_call`路由到同一个固定的DeepSeek模型。共享的`NodeContext.instructions`提供系统提示。缺少密钥时会以清晰的消息退出；没有模拟提供程序会静默地改变实时提供程序路径。

## 输出形态

```text
[openrouter] using ~deepseek/deepseek-v4-flash-latest via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

实际补全结果各不相同。直接HTTP路径打印`via direct HTTP`并使用其自身的算术问题。

## 为什么保留两条路径？

- 路径A使用NeoGraph的原生HTTP实现和连接池。
- 路径B是自定义请求头、传输策略或由应用程序代码拥有的响应转换的最小示例。
- 两条路径使用完全相同的 OpenRouter 端点族、API 密钥和固定的 DeepSeek 模型，因此供应商层面的比较是有意义的。

OpenRouter API 请求和响应格式：
<https://openrouter.ai/docs/api-reference/overview>
