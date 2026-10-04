<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=zh-CN source_sha256=acf353dedaa0142260f857bf89a32b1c5cbee7fb7c2b31df5fb67fa7ef131dd3 -->
# 使用你自己的 OpenAI 客户端

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

在 NeoGraph 自定义 `GraphNode` 中使用已有的 `openai.OpenAI()` 客户端。
官方 SDK 保留自己的 HTTP 客户端、重试策略、请求头及 SDK 层监测配置。
NeoGraph 调度节点，并将其 `ChannelWrite` 结果应用到图状态。

[`hybrid.py`](hybrid.py) 调用 SDK 一次并追加助手回复。
[`hybrid_with_tools.py`](hybrid_with_tools.py) 在单个节点中运行 SDK 工具调用循环，
执行三个 Python 函数，再追加最终回复。两者使用 OpenRouter，默认模型为
`~deepseek/deepseek-v4-flash-latest`。请求中的 `provider={"zdr": true}`
要求 OpenRouter 使用零数据保留路由策略；它不构成地理数据驻留策略。

## 前置条件与本地运行

安装从当前类型化提供商迁移代码构建的 wheel，以及 `openai` 包。
仍提供已删除完成 API 的旧版本无法运行这些示例。在此目录执行以下命令前，
先启动本地 Chat Completions 协议服务。以下是验证步骤，不是已成功运行的记录。

```bash
python -m pip install openai
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid_with_tools.py
```

`OPENROUTER_BASE_URL` 包含 API 前缀：本地服务使用 `/v1`，OpenRouter 使用
`/api/v1`。SDK 追加 `/chat/completions`。规范回环主机 `127.0.0.1` 和 `::1`
使用固定虚拟凭据 `local-smoke`，即使环境中存在托管服务密钥也不会使用它。
其他主机要求 HTTPS、`NG_ALLOW_HOSTED_CALLS=1` 和 `OPENROUTER_API_KEY`；
未明确允许时，程序在发出请求前以状态码 2 退出。托管调用可能产生费用。
示例可以读取已有 `.env`，但不覆盖已导出的变量。不要提交密钥或记录请求授权头。

## 图状态与 SDK 请求

每个图在 `START_NODE` 与 `END_NODE` 之间有一个自定义节点。
有作用域的 `GraphRegistry` 注册节点类型，不使用全局提供商子类或完成跳板。
节点读取 `messages` 通道，将系统指令放在 SDK 请求前面，再返回通道写入。
append 归约器保留输入用户消息及随后的助手消息。系统消息仅存在于请求中。

对于 `hybrid.py`，本地服务接收一次缓冲模式的 `POST /v1/chat/completions`，
包含 `model="fixture-model"`、系统消息、用户消息、`temperature=0.7` 和
`provider={"zdr": true}`。返回标准 Chat Completion JSON 对象，含 `id`、
`object="chat.completion"`、`created`、`model`，以及一个 `choices` 条目。
该条目包含 `index=0`、助手消息及 `finish_reason="stop"`。`usage` 可省略。
预期状态有两条消息；`sdk_usage` 是 SDK 用量字典或 `None`，不会虚构零计数。
收到工具调用时，纯文本节点会失败，不会丢弃工具调用后继续成功。

## 工具循环

`hybrid_with_tools.py` 的第一个请求还声明函数工具 `reverse_string`、
`word_count` 和 `calc`。确定性的本地服务可以返回三个助手工具调用，各自使用
不同 id，并携带 JSON 参数字符串：`{"s":"NeoGraph"}`、
`{"text":"the quick brown fox"}`、`{"expr":"17*23+5"}`。
设置 `finish_reason="tool_calls"`。

节点将助手工具调用消息追加到 SDK 内部历史，执行每个函数，再追加带匹配
`tool_call_id` 的工具结果。第二个请求必须包含 `hparGoeN`、`4`、`396`
这些结果。返回不含工具调用且 `finish_reason="stop"` 的助手文本。
预期图状态仅包含原始用户与最终助手消息、`tool_calls=3`，以及两个条目的
`sdk_usage` 列表。每项是对应响应的用量字典或 `None`；最后一次调用的用量
不能代表整个循环。此交换中程序打印三次工具执行及两次 SDK 调用。

工具异常会变成工具结果中的错误文本，以便模型作出回应。连续八次工具调用响应
耗尽上限后会抛出错误，不写入虚构的最终答案。`calc` 在此算术演示中使用
Python 表达式求值；它不是用于执行不可信表达式的沙箱。

## 提供商证据边界

这些节点写入应用拥有的 JSON 状态，不生成 `ProviderOutcome`、原生重放权限、
提供商收据或每次工具调用的检查点。SDK 重试与中间工具调用留在节点内部；
图检查点不是这些请求的持久收据。运行结束后关闭客户端。

需要 NeoGraph 的类型化提供商结果与原生 SDK 传输时，请使用
[OpenRouter SchemaProvider 示例](../openrouter-provider/README.md)。
将已有 SDK 客户端传给这些自定义节点会保留客户端配置，但不会使其成为
`SchemaProvider`。
