<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=zh-CN source_sha256=2ae7c3fb71401e8c8db0b7c5a6116f56d8076f1c963b768aa55f98931801bc16 -->
# NeoGraph + OpenRouter

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本手册保留通往 OpenRouter Chat Completions API 的两条请求路径。
两者的默认模型都是 `~deepseek/deepseek-v4-flash-latest`，并发送
`provider={"zdr": true}` 请求零数据保留路由。这项设置不是地理数据驻留保证。

[`via_openai_compat.py`](via_openai_compat.py) 使用原生 `SchemaProvider` 和
内置 `llm_call` 节点。[`via_http.py`](via_http.py) 使用自定义 Python `GraphNode`，
通过 `httpx` 自己管理 HTTP 请求和响应映射。它们共享端点与凭据辅助函数，
但不共享传输实现或结果表示。兼容路径示例不需要 `httpx`。

## 路径 A：类型化 SchemaProvider

兼容路径使用封闭、带版本的 `openai.chat` 描述符调用
`load_provider_descriptor`。`connection.base_url` 只包含源地址，
`connection.paths` 包含完整 API 路径。对 OpenRouter，它们分别是
`https://openrouter.ai` 和 `/api/v1/chat/completions`。
未知描述符字段会被拒绝，任意 JSON 无法注入编解码器行为。

类型化 `OpenRouterRouting` 值在 `SchemaProviderDefaults` 中设置 `zdr=True`。
`ProviderRuntimeOptions` 提供 API 密钥、120 秒超时及可选
`OPENROUTER_CA_FILE`。运行时使用 libcurl，不提供 HTTP2 或 WebSocket 传输选择器。
`NodeContext` 提供明确的模型及系统指令。`RunConfig.provider_messages` 提供
包含 `Text` 部分的类型化 `ProviderMessage`。内置节点创建类型化请求，准备请求，
然后通过提供商分发。

回环验证时，示例读取 `provider_policy_json()`，将准确的回环源地址加入
`openai.chat` 家族的 `openrouter_origins`，再通过 `load_provider_policy`
重新准入完整的家族和编解码资源数据，最后加载描述符。测试源地址是声明的策略数据，
不是运行时端点覆盖，也没有跳过 ZDR 验证。

成功结果保留不可变提供商结果和类型化消息历史。`outcome.completion` 保存完成结果，
失败时则由 `outcome.failure` 保存失败。缺失的用量计数是 `None`；
已观测的零值是 `value=0` 的 `UsageCount`。示例打印助手文本及可用的输入、输出
计数，不转储原始负载或密钥，也不把提供商失败转换成成功答案。

## 路径 B：自定义 HTTP 节点

`OpenRouterHttpNode` 读取图消息，在前面加入系统指令，将 JSON 发到选定端点，
并把 `choices[0].message` 映射为普通 `ChannelWrite`。自定义 HTTP 请求头、
超时策略、响应转换由应用代码管理。append 归约器保留用户及随后的助手消息。
`http_usage` 保存响应的用量字典，省略时为 `None`。运行后关闭 `httpx` 客户端。

这条路径不生成 `ProviderOutcome`、已准备请求的权限、原生重放历史或类型化
提供商用量。HTTP 错误使节点失败；收到工具调用时，此纯文本示例也会失败，
不会丢弃调用。需要已有官方 SDK 客户端管理调用及工具循环时，请使用
[BYO OpenAI SDK 手册](../byo-openai/README.md)。

## 前置条件与本地运行

安装从当前类型化提供商迁移代码构建的 wheel。旧完成 API 版本无法运行这些示例。
为路径 B 安装 `httpx`，然后在此目录执行下列命令前启动本地 Chat Completions
服务。以下是验证步骤，不是已成功运行的记录。

```bash
python -m pip install httpx
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_openai_compat.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_http.py
```

`OPENROUTER_BASE_URL` 包含 API 前缀；两条路径都会追加 `/chat/completions`。
规范回环主机 `127.0.0.1` 和 `::1` 使用固定虚拟凭据 `local-smoke`，即使环境中
存在托管服务密钥也不使用它。其他主机要求 HTTPS、`NG_ALLOW_HOSTED_CALLS=1`
及 `OPENROUTER_API_KEY`。未明确允许时，每个程序都在发出请求前以状态码 2 退出。
托管调用可能收费。可选的已有 `.env` 不覆盖已导出的变量。不要提交密钥或记录
Authorization 请求头。对于路径 A，非 OpenRouter 主机还需明确的路由策略准入；
仅允许调用并不会赋予该主机 OpenRouter 语义。

## 本地协议与预期状态

本地服务接收缓冲模式的 `POST /v1/chat/completions`，其中包含系统消息、
用户消息、`model="fixture-model"` 及 `provider={"zdr": true}`。
路径 B 还发送 `temperature=0.7`。路径 A 询问法国首都，路径 B 询问 `17 * 23`。
路径 A 的服务响应示例：

```json
{
  "id": "fixture-chat-1",
  "object": "chat.completion",
  "created": 0,
  "model": "fixture-model",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Paris."},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 8, "completion_tokens": 2, "total_tokens": 10}
}
```

路径 A 的预期结果是一个真实的成功提供商结果、`result.provider_messages` 中的
用户与助手，以及图状态中的助手回复。该响应的用量为输入 8、输出 2。
省略 `usage` 可验证未知计数，不应期待虚构的零值。路径 B 的助手内容应返回
`"391"`，预期有两条图消息及返回的 `http_usage` 字典。两个示例都不会悄悄切换到
模拟提供商。

OpenRouter 请求与响应参考：
<https://openrouter.ai/docs/api-reference/overview>
