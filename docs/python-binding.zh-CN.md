<!-- neograph-i18n: source=docs/python-binding.md locale=zh-CN source_sha256=73069d840a47c09f7c2b2b6f74da22c57bf16c51e1837e07042d293c6692dd5d -->
# Python 绑定

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` 是同一 C++ 运行时的 pybind11 接口。该 wheel 支持 Core、LLM、Program/QuickJS、MCP 和 SQLite 运行时持久化；可选源码构建仅暴露其编译的组件。

本指南描述当前类型化绑定 API。暴露 `complete` 的旧 wheel 使用不同的提供商接口，无法运行以下类型化示例。

```bash
pip install neograph-engine
```

## 类型化提供商请求与结果

`Provider` 将请求准备与分发分开。`SchemaProvider` 通过 SchemaProvider SDK 验证并编码特定请求族的请求。`PreparedProviderRequest` 持有该原生准备结果，直到一次分发将其消耗。

| 操作 | Python 签名 | 结果 |
|---|---|---|
| 准入描述符 | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`；无效的封闭 JSON 引发 `ValueError` |
| 构造提供商 | `SchemaProvider(descriptor, options, defaults)` | 具有已准入端点/请求族和运行时策略的提供商 |
| 构造请求 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 类型化 `ProviderRequest` |
| 准备 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 分发一次 | `provider.dispatch(prepared)` | 拥有所有权的 `ProviderOutcome` |
| 准备并分发 | `provider.invoke(request)` | 拥有所有权的 `ProviderOutcome` |

`model` 需要显式指定。`ProviderControls` 提供类型化限制和生成控制，包括 `max_output_tokens`、`temperature` 和 `top_p`。工厂构造 SDK 特定请求族的载荷，不能用原始字典替换。将 `request.mode` 设为 `ProviderMode.Stream` 以请求流式输出；仅设置 `on_event` 不会选择该模式。`request` 还包含 `cancel_token`、`on_event`、`observer_limits` 和可选的 `timeout_ms`。

`ProviderControls.provider` 和 `response_format`、`SchemaProviderDefaults.provider`、`ProviderToolResult.host` 返回独立的可选记录。有值时，读取记录，修改后再赋回属性；只修改 `controls.provider.order` 不会更新 controls。赋值 `None` 可清除记录。

将可表示的非负值赋给 `request.timeout_ms` 会立即建立绝对截止时间。请在准备之前设置；之后读取返回剩余毫秒数，过期后限制为零。保持 `None` 时，会在准备阶段使用提供商默认超时。持有准备句柄并等待不会续延其截止时间。

### 描述符与运行时策略

描述符是 `load_provider_descriptor` 接受的带版本的封闭 JSON，准入请求族、`base_url`、路由、字段绑定及认证规则。请求指定 `model`，运行时选项提供凭据。`ProviderRuntimeOptions` 提供 `api_key`、`default_timeout_ms`、`ca_file`、`workers` 和 SDK 的传输/资源限制。`SchemaProviderDefaults` 提供类型化的提供商默认值。请将凭据保存在运行时选项中，不写入描述符文件或日志。

`ProviderDescriptorPolicy.identity` 返回 Python `bytes`，包含 SDK 策略的原始 SHA-256 摘要。显示时使用 `policy.identity.hex()`；标识本身既不是 UTF-8 文本，也不是十六进制字符串别名。

当前传输使用 libcurl。没有 `prefer_libcurl` 开关、运行时端点覆盖或 WebSocket 传输。[C++ 提供商参考](reference-en.md)说明描述符准入及支持的请求族。

启动 [SDK 使用指南](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)中的单请求回环协议端，然后在另一个终端运行这个纯文本示例。端点、模型和输出上限与该协议端匹配，不发送凭据。HTTPS 协议端可通过 `NG_EXAMPLE_CA_FILE` 指定受信任的 CA。使用托管端点时，显式更改描述符 origin 和请求模型，并在运行时选项中提供该 origin 的凭据；托管调用可能产生费用。

```python
import json
import os
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

descriptor = ng.load_provider_descriptor(json.dumps({
    "descriptor_version": 1,
    "revision": 1,
    "id": "python-guide-chat",
    "family": "openai.chat",
    "connection": {
        "base_url": "http://127.0.0.1:8765",
        "paths": {
            "buffered": "/v1/chat/completions",
            "streaming": "/v1/chat/completions",
        },
    },
    "bindings": {
        "model": "model", "messages": "messages", "stream": "stream",
        "max_output_tokens": "max_tokens", "usage": ["usage"],
    },
    "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                     "tool_calls": "ToolUse", "content_filter": "ContentFilter"},
}))
options = ng.ProviderRuntimeOptions(
    api_key="",
    ca_file=os.getenv("NG_EXAMPLE_CA_FILE", ""),
    default_timeout_ms=30_000,
)
provider = SchemaProvider(descriptor, options=options)
controls = ng.ProviderControls()
controls.max_output_tokens = 64
request = ng.make_provider_request(
    provider, "example-model",
    [ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text("Say hello.")])],
    controls=controls,
)
prepared = provider.prepare(request)
outcome = provider.dispatch(prepared)
print("consumed:", prepared.consumed)
if outcome.failure is not None:
    print("failed:", outcome.failure.error.kind,
          outcome.failure.error.safe_message)
else:
    print(outcome.text)
count = outcome.usage.output_total
print("output tokens:", count.value if count is not None else "unknown")
```

不需要句柄时，`provider.invoke(request)` 会合并准备和分发。`outcome.text` 是可见文本的投影，`outcome.messages` 保留完整的类型化部分。省略的使用量计数器显示为 `unknown`；报告的零显示为 `0`。

使用链接中的合成响应协议端时，预期输出为:

```text
consumed: True
Hello.
output tokens: 0
```

删除协议端整个 `usage` 成员并重新启动，以验证缺失计数器路径；最后一行应变为 `output tokens: unknown`。这验证协议映射，不验证真实模型的 token 计数。

### 接口 4 的请求族控制

需要针对 SDK 接口/共享库代次 4 构建的 wheel。包版本、native archive v3 和 portable JSON v2 独立。示例只构造控制，需要对应请求族的已准入 provider。保持首次回环不变；OpenRouter 专用控制在未准入本地 origin 上于 I/O 前拒绝。

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]

responses = ng.ProviderControls()
responses.parallel_tool_calls = False
responses.verbosity = ng.ResponsesVerbosity.Low
responses.truncation = ng.ResponsesTruncation.Disabled
responses.responses_include = [ng.ResponsesInclude.ReasoningEncryptedContent]

messages = ng.ProviderControls()
messages.max_output_tokens = 4096
messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
messages.output_effort = ng.MessagesOutputEffort.High
cache = ng.MessagesCacheControl()
cache.ttl = ng.MessagesCacheTtl.FiveMinutes
messages.cache_control = cache
choice = ng.MessagesToolChoice()
choice.mode = ng.MessagesToolChoiceMode.Auto
choice.disable_parallel_tool_use = True
messages.messages_tool_choice = choice

gemini = ng.ProviderControls()
gemini.temperature = 0.7
gemini.gemini_thinking_level = ng.GeminiThinkingLevel.Low
safety = ng.GeminiSafetySetting()
safety.category = ng.GeminiSafetyCategory.Harassment
safety.threshold = ng.GeminiSafetyThreshold.BlockMediumAndAbove
gemini.safety_settings = [safety]
choice = ng.GeminiToolChoice()
choice.mode = ng.GeminiToolChoiceMode.Auto
gemini.gemini_tool_choice = choice
```

先默认构造再赋字段，没有关键字构造函数。optional 记录/向量返回独立副本，修改后须重新赋值。Chat reasoning 的 `effort/max_tokens/exclude/enabled`、`include_reasoning`、`usage_include`、候选 `models` 需要声明的 OpenRouter origin。现有 `reasoning_effort/service_tier/provider/response_format` 仍保留。

Responses verbosity 为 `Low/Medium/High`，truncation 为 `Disabled/Auto`，include 为 `ReasoningEncryptedContent/WebSearchSources/FileSearchResults/MessageOutputTextLogprobs/ComputerCallOutputImageUrl/CodeInterpreterCallOutputs`。`responses_include=None` 保留默认 encrypted reasoning；包括 `[]` 的显式列表只选择该列表，可能使后续 native replay 不合格。store/reasoning/hosted tools/tool-call 上限仍保留。

Messages thinking 为 `Manual/Adaptive/Disabled`。Manual budget 至少为准入最小值且小于 output cap；未指定 mode 的 budget 选择 manual。Adaptive/disabled 禁止 budget。启用 thinking 省略 temperature，但模型禁令仍拒绝显式 temperature。effort 为 `Low/Medium/High/Max`，TTL 为 `FiveMinutes/OneHour`，tool choice 为 `Auto/Any/None_/Tool`，其中 `Tool` 需要声明的 client tool `name`。routing 需要声明的 OpenRouter origin。

Gemini level 为 `Minimal/Low/Medium/High`，与 `thinking_budget` 互斥。tool choice 为 `Auto/Any/None_/Validated`，`allowed_function_names` 为列表，与 `required_tool` 互斥。safety category 为 `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`，threshold 为 `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`。错误请求族/范围/thinking·cap·tool 组合于 I/O 前拒绝。temperature 禁止前缀按 ASCII 忽略大小写，也匹配最后 `/` 后的模型。Chat/Responses: `gpt-5/gpt-6/o1/o3/o4`；Messages: `claude-opus-4-7/claude-opus-4-8/claude-opus-5/claude-sonnet-5/claude-fable-`。参阅 [SDK 控制准入](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)。

### 准入前部署标头

`load_provider_descriptor(source, policy=None)` 按字面处理 `${VAR}`。以下 helper 在实际准入前处理宿主值；`source` 为 Messages 描述符 JSON 文本。

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` 读取 Messages 的可选 `ANTHROPIC_WORKSPACE_ID/ANTHROPIC_BETA`，省略未设置/空值。字面标头优先于环境，显式 pair 忽略大小写覆盖两者。重复 override/无效或保留名称/换行导致准入失败。两个 helper 接受可选 `policy`，不修改已准入描述符或求值 encoder template。

### Responses 游标与外来 Gemini 历史

`previous_response_id` 选择 provider 保存的状态。只发送新输入，不重发完整历史，并保留首次成功 Responses 调用的绑定控制和真实 terminal ID。

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses.previous_response_id = first_outcome.messages[-1].id
responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history` 是 `ProviderMessage` 向量，不是单个响应/JSON 对象，不会发送。真实 client-tool 所有权可能需要原始完整前缀和 ID 等于 cursor 的 terminal assistant。服务器保存文本可以使用空向量。后续进程内 cursor 的 private terminal 所有权不会成为完整 replay/archive 权限。失败/修改/origin·model·config·route 不匹配均拒绝。完整 native replay/archive 仍需要原始完整前缀。参阅 [SDK Responses 续接](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)。

调用者创建的外来 assistant `Text/ProviderToolCall` 没有 native state/wire output/signature 时，显式设置 `gemini.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign`；默认 `NativeOnly`。只有首个外来 function call 获得 Google validator bypass，纯文本 turn 不生成 signature。真实 native group 仍严格验证，不修复/剥离/降级失败 seal，不导入 reasoning 权限，不向 portable data 授予 replay。参阅 [SDK 外来 Gemini 历史](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)。

PortableForeign 也拒绝 wire metadata。cursor 输出的 `NativeReplay.complete == False`，不具备 archive 资格；private owner 不授权完整历史 replay。


### 准备句柄与 Python 提供商

分发会消耗准备句柄一次，包括分发失败的情况。分发前检查 `prepared.valid` 和 `prepared.error`，之后检查 `prepared.consumed`。保留 Python 对象不会使其可重用。再次尝试时应构造并准备新的请求。

Python 子类调用 `Provider(family)`，实现 `get_name()` 和 `prepare(request)`。子类可以委托 `SchemaProvider` 准备请求，并返回其真实的准备句柄。不能任意创建成功的 SDK 结果，也不能从 JSON 导入分发权限。公共 `invoke` 与 `dispatch` 使用原生生命周期，重写旧 `complete` 方法不能实现该契约。

原生执行调用 Python 重写的 `prepare` 时，返回 `None` 会在消耗句柄之前引发 `TypeError`。请返回真实的 `PreparedProviderRequest`。

`NodeContext(provider=provider)` 及对 `ctx.provider` 的赋值通过原生共享所有权租约保留原始 Python 提供商。原生上下文、已编译节点及引擎的副本保留此租约，因此即使重新赋值 `ctx.provider` 或回收外部 Python 引用，仍会调用同一对象的重写方法。重新赋值只释放可修改上下文的租约，影响后续编译，不会改变现有引擎。这保留对象身份及生命周期，不会冻结提供商的可变状态。最后一个租约在持有 GIL 时释放 Python 所有者。

已删除的 `CompletionParams`、`ChatCompletion`、`OpenAIProvider` 和 `RateLimitedProvider` 没有兼容别名。请构造经过验证的 `SchemaProvider`，使用请求工厂。`ChatMessage` 与 `ToolCall` 仍是图的便捷值，与完整 SDK 类型 `ProviderMessage` 和 `ProviderToolCall` 不同。

### 结果、失败与使用量

`ProviderOutcome` 保留拥有所有权的不可变 SDK 结果。检查 `outcome.completion` 或 `outcome.failure`；不存在的分支为 `None`。完成与失败视图保留完整的有序消息、使用量、尝试证据、停止/错误信息和留存的线协议证据。失败也可能保留部分输出。报告失败时应保留这些证据，不应把部分文本作为成功结果返回。

`ProviderCompletion.wire_envelope` 和 `ProviderPartialCompletion.wire_envelope` 是随提供商家族而异的证据，可以为 `None`。读取前请检查 `wire_envelope is not None`。当前缓冲式 Chat 解码器将其保持为 `None`，并将完整响应 JSON 保留在 `raw_events` 中。该项是 `type == "chat.completion"` 的 `ProviderRawWire`（SDK 的 `RawWire`），文档位于 `payload`。应检查实际的类型化 raw 事件，不应假定一定存在封装。raw 事件不会把失败变为成功；没有封装也不表示没有保留响应。SDK 不会通过回退合成封装。

请保留拥有所有权的结果或其留存的类型化视图：真实消息、原生所有权及存在的线协议文档在调用结束和提供商对象被垃圾回收后仍然有效。raw payload 保留提供商的私有字段，但原生追踪排除 raw 封装/事件及原生重放/推理。Python JSON 视图是数据副本，不是原生重放权限或财务权限。

请使用完整的类型化消息/部分；需要逻辑角色和文本语义时，应比较这些值。检查点与 Chat 请求的文本 content 可以合法地表示为文本字符串或类型化文本部分数组；调用方不应要求某一种偶然采用的序列化形状。继续执行时，请使用完整部分和真实原生所有权，不要以扁平文本或 JSON 代替。

`ProviderMessage.parts`、完成/部分结果的 `messages` 和 `raw_events`、使用量的 `extra`/`conflicts` 返回独立的列表或映射，包括其中的绑定值。替换原集合后，保留的部分仍可安全读取；修改副本不会改写不可变结果。编辑消息时，用 `parts = message.parts` 读取，修改 `parts`，再以 `message.parts = parts` 赋回。`message.parts.append(...)` 仅改变临时 Python 列表。

SDK 失败以数据返回。宿主观察器或预算结算失败会引发派生自 `ProviderOutcomeError` 的 `ProviderObserverError` 或 `ProviderBudgetSettlementError`，保留真实结果和原因。因此，回调失败并不授予再次分发的权限。

反复读取已保存的 Python 提供商或图异常原因，会保留原始异常对象和 traceback；通过原生嵌套异常转换访问的原因也遵循同一规则。

使用量计数器是带有 `value` 与 `evidence` 的 `UsageCount`，未知时为 `None`。报告的零是已知使用量，与省略的计数器不同。读取 `count.value` 前请检查 `count is not None`。计费时不要用 `count or 0`，也不要将未知的输入/输出/总计数器替换为零。

恢复或继续运行时，`RunResult.provider_outcomes` 按顺序保留原有结果，再追加新产生的结果。`RunResult.usage` 反映当前计费账库，其中可能包含从检查点恢复的先前报告；恢复后没有新提供商调用，也不保证使用量为 `None` 或零。恢复证据不得重新分发原始提供商请求，也不得重复计费。保留的报告描述先前调用，不授予新的支出额度。

### 原生历史与可移植导出

返回的 `ProviderMessage` 包含完整的类型化部分及真实的原生重放状态。原生重放及线协议输出为只读。直接继续调用提供商时，须保留原请求的完整 `request.messages` 前缀，再追加 `outcome.messages`；后者是返回的输出，不是完整输入历史。使用首次调用示例中的 `request` 和 `outcome`，构造 `history = request.messages + outcome.messages`，再用 `ng.make_provider_request(provider, "example-model", history, controls=controls)` 创建下一请求。此构造本身不发送任何内容；下一轮还须包含新的用户消息或所需的工具结果。

真实的 `NativeContext` 重放会检查原始前缀及返回的 Assistant 消息。只传入 Assistant 输出会在线协议分发前以 `ReplayIneligible` 失败。通过 `NativeArchive` 加载该 Assistant 消息会保留其保管权限和前缀绑定，仍须提供原始完整前缀。

对于图，`RunConfig.provider_messages` 接受完整历史，`RunResult.native_messages` 已包含捕获的完整历史。直接将该图结果作为历史使用，不要再次在前面添加原始输入。`RunResult.provider_outcomes` 暴露保留的结果；在节点内，`RunContext.provider_outcomes` 和 `provider_loop_history` 保留提供商证据。

`RunConfig.provider_messages`、`RunResult.native_messages` 和 `ProviderLoopEntry.messages` 也返回独立的历史副本。要更新可修改的输入历史，编辑返回的列表后再赋给 `config.provider_messages`。替换输入后，保留的消息和部分仍然有效；结果及循环历史属性保持只读。

在自定义节点中，用 `provider_messages_write(messages_or_outcome)` 为通道写入保留原生历史。`portable_message(ChatMessage)` 导入可移植内容，拒绝导入原生推理权限；`project_message(ProviderMessage)` 生成仅供观察的图投影。

原生持久化使用 `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)`，返回存档或 `ProviderError`。`save(messages, binding="")` 返回引用或错误；`load(reference, binding="")` 返回类型化消息或错误。它使用独立的宿主密钥执行 SDK 已认证的本地保管，不会将可移植 JSON 转换为重放权限或受管理预算权限。

`RuntimeHistoryRecord.serialize_canonical()` 仍可用于可移植记录。持久化含有真实原生消息的记录时，须使用 `owner_scope` 与 `owner_id` 一致的真实 `NativeArchive`，调用 `record.serialize_canonical(archive, owner_id)`。恢复时使用 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`。默认参数足以处理可移植记录，但原生存档引用必须使用同一所有者范围的存档。无参数序列化或导入 JSON 不能重建原生权限。Python 的 parse 和存档感知序列化调用会在原生工作期间释放 GIL，包括存档 I/O。

`RuntimeHistoryRecord.message` 返回独立的类型化副本，保留真实的共享原生所有权；修改副本不会改写不可变 RAW 记录的标识。`ContextStore.hydrate_records(range)` 返回类型化记录列表，`history_record_by_message_id(feed, message_id)` 返回记录或 `None`。`InMemoryContextStore` 与 `SQLiteContextStore` 继承这些方法。`SQLiteContextStore(database_path, archive=None)` 接受真实存档以提供原生保管权限；在必须提供此权限时，无存档存储会拒绝原生历史。构造与类型化检索会释放 GIL。`LocalProgramHost` 接受最后一个可选关键字 `native_history_archive=None`，并将其传给真实 Program 运行时；它提供保管权限，不提供额外授权或持久 Program 存储后端。

可移植状态字典和 JSON 导出描述消息数据，不能重建原生重放状态、提供商来源或受管理预算权限。导出 JSON 中的 `native` 标志用于描述，不是授权令牌。转换为 `ChatMessage`、扁平文本通道或 JSON 可能丢弃提供商特有的部分；需要这些部分时，请保留包含原始前缀的完整类型化历史。

## Core 图快速入门

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

### 编译期工具所有权

将 Python `Tool`、原生 C++ 工具以及 `MCPClient.get_tools()` 的结果统一传入
`ng.NodeContext(tools=[...])`。编译前会快照至持有工具的 `ToolSet`；
引擎在 `run()` 和 `resume()` 期间持续持有。之后重新赋值上下文的 `tools`
只影响下一次编译，不会改变已有引擎；MCP 工具保留原生异步执行路径。

## Core API 对等性

Python 暴露的是 C++ 执行能力，而非独立的 Python 调度器：

- 同步与 asyncio 的 run/stream/resume；
- 精确检查点 `resume_from`、fork、状态检查与有序状态写入；
- 通过图中断和 `NodeInterrupt` 实现静态与动态 HITL；
- `RunMetadata` 截止时间、trace/run 标识与模型 token 上限；
- 图级与节点级 `RetryPolicy`，包括抖动；
- 执行局部或可显式复用的 `CacheScope`；
- 检查点与长期 Store 后端；
- 自定义节点、reducer、条件、provider 与工具；
- 工具门控、执行策略、强制生命周期 Hook 与严格运行时介入。

### 对等性契约

这里的“对等性”是指Python使用相同的原生执行路径和安全契约，而不是把每个内部C++存储类型或权限类型原样复制到Python中。

| 功能 | 原生C++路径 | Python接口 | 状态 |
|---|---|---|---|
| 编译并执行Core图 | `GraphEngine` | `GraphEngine.compile`、run/stream/async方法 | 相同的调度器和运行时 |
| 运行时标识、截止时间与预算 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 每次运行使用相同的值 |
| 重试与节点缓存策略 | `RetryPolicy`, `CacheScope` | 图/节点setter与缓存作用域 | 相同的运行时策略 |
| 检查点、HITL与时间旅行 | 检查点Store与恢复API | 恢复、精确`resume_from`、分叉、状态历史/更新 | 相同的检查点契约 |
| Program编写与本地执行 | 编译器、Catalog与`ProgramRuntime` | `ProgramCompiler`、`LocalProgramHost`、句柄/结果 | 原生、限定所有者作用域的便捷主机 |
| 强制生命周期Hook | 注册表、运行器与`HookRuntime` | 定义及`create_hook_runtime`回调 | 相同的失败关闭生命周期边界 |
| 运行时上下文与严格分发 | 上下文Store、收据与介入 | 对应的不可变值、Store与`StrictRuntimeProfile` | 相同的原生控制器 |
| 带持久化能力的wheel默认配置 | SQLite Core/上下文/分发Store | `_HAVE_SQLITE`导出 | 在PyPI wheel中启用 |

原始`ProgramCatalog`、转换Store、替换/迁移控制器、合成网关、Hook日志和RPC执行器仍属于主机组合API。若只暴露这些权限路径的一部分，就可能绕过必需的`proposal -> compile -> admit -> publish -> migrate/spawn`协议。未来的Python主机控制器必须把该协议及其不可续增的谱系预算绑定为一个限定所有者作用域的整体。`_HAVE_PROGRAM`并不声称原始控制平面管理也已实现对等。

### 运行时重试覆盖

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

图定义的 `"retry_policy"` 仍是声明式默认。运行时 setter 是独立的 C++/Python 配置接口。

### 元数据与精确恢复

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

在 Python 节点内部，相同的值可通过 `input.ctx.trace_id`、`run_id`、`has_deadline`、`deadline_remaining_ms` 和 `model_token_budget` 获取。

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)` 默认不设截止时间。构造时指定的超时及 `metadata.set_timeout_ms(timeout)` 接受处于 steady clock 剩余范围内的非负整数毫秒，在转换为有符号 duration 或相加生成截止时间前验证范围。负数或过大的整数会引发 `OverflowError` 或 `ValueError`；setter 失败会保留先前的截止时间。零会设置立即到期的截止时间。调用 `metadata.clear_deadline()` 可将其清除。

### 缓存作用域

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` 是一个显式断言，表明该节点独立于租户、提供商、Store、工具、凭据、时间和恢复状态。

## Program 与 QuickJS

Python wheels 构建 `neograph::program` 以及受限的 QuickJS 前端。Python 定义的节点可以参与不可变的 Program 注册表，并通过原生 `ProgramRuntime` 执行。

```python
import neograph_engine as ng

@ng.node("my_node")
def my_node(state):
    return [ng.ChannelWrite("value", state.get("value", 0) + 1)]

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
print(result.status, result.output)
```

`LocalProgramHost` 是一个所有者作用域的内存便捷宿主。它仍然使用 C++ 编译器、Catalog、准入(admission)策略、转换存储和 ProgramRuntime。生成的提案在准入(admission)前还应通过宿主语义验证器；参见 [DSL 能力评估](DSL_CAPABILITY_EVAL.md)。

销毁 `LocalProgramHost` 时，会在真实 `ProgramRuntime` 取消、排空并 join 调度器工作期间释放调用方的 GIL，使运行中的 Python 节点可以结束。销毁剩余宿主成员前会重新获取 GIL；Python 回调/对象所有者仍按 GIL 安全方式销毁。

原生 Program 的记录执行 API 是取代 `start_recorded` 的 `ProgramRuntime::replay_recorded`。`LocalProgramHost` 暴露 `run`、`start` 和 `resume`，不暴露原始记录绑定的控制平面。Program 结果在现有句柄、预算和权限字段之外，还保留原生运行时提供的类型化提供商结果及失败证据。

`ProgramResult.failure` 是只读的 `ProgramFailure` 值或 `None`，不是字典。其 `provider_outcome` 与 `provider_cause` 保留提供商失败证据；`code`、`message`、`operation_id`、`core_node`、`attempts` 和 `witness` 描述失败的操作。结果字段还包括 `bundle_id`、`operation_id`、`attempt`、`checkpoint`、`interrupt` 和 `provider_budget_authority`。

精确安装的 JavaScript 词汇表以字典形式提供：

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## 强制生命周期 Hooks

Hooks 由宿主生命周期事件触发，而非由模型决定调用工具。

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

在 `FailClosed` 下的回调失败会阻塞受保护的运行时边界。仅当可接受观测性损失时，`Continue` 才可用。

## 运行时上下文、Skills 与严格分发

该绑定暴露不可变的 RAW 历史记录、上下文工件、epochs、必需的 Skills/约束、转换收据和提供商分发收据。

创建 RAW `RuntimeHistoryRecord` 时，Assistant 消息须将 `RuntimeHistoryRecordData.trust` 设为 `RuntimeTrustClass.ModelOutput`。默认值 `UntrustedInput` 仅接受 User 消息，用于 Assistant 输出会被拒绝。这些标签描述消息来源，不授予工具执行、预算或代码权限，也不能替代真实的原生重放保管权限。

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt` 允许任意派生的证据，但要求每个必需工件保持字节一致。

对于完整的严格路径，请使用持久化 SQLite 存储：

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
outcome = profile.invoke(request)
profile.attach(engine)
```

## HITL 与状态

静态 `interrupt_before`/`interrupt_after`、动态 `NodeInterrupt`、同步 `resume`、asyncio `resume_async` 以及精确 `resume_from` 都需要检查点存储。

Python `CheckpointStore` 子类可以实现 `requires_managed_budget(thread_id) -> bool`。此同步虚方法读取已持久化的受管理预算银行拒绝义务；引擎的原生异步门面调用该方法，绑定在执行 Python 重写方法时获取 GIL。即使检查点状态中的银行信息被剥离，或检查点被删除，也须报告真实的持久义务。省略重写方法会保留原生明确的后端不支持错误，而不是返回 `False`。

此读取方法不授予支出、恢复或租约权限。有界的受管理执行仍需受支持的真实原生受管理预算租约；仅实现此读取方法不能提供这些租约。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

使用 `get_state_history`、`update_state` 和 `fork` 进行检视和时间旅行。`get_state_view()` 提供基于 Pydantic 的扁平通道访问，而 `get_state()` 保留规范的嵌套表示。

## 异步与取消

`run_async`、`run_stream_async` 和 `resume_async` 返回 `asyncio.Future`。取消这些图 Future 会通过 `CancelToken` 请求取消；原生 I/O 必须到达取消边界，操作才能结束。图的流式回调会返回调用方的 asyncio 事件循环线程。

提供商的 `invoke` 和 `dispatch` 是同步 Python 方法。绑定在原生调用运行时释放 GIL，在执行 Python 提供商重写方法及事件回调时重新获取 GIL。事件回调接收拥有所有权的 `ProviderEvent` 值，可能在原生工作线程上执行。回调需要更新 asyncio 管理的状态时，请使用 `loop.call_soon_threadsafe`。

通过 `request.on_event = callback` 设置观察器，或用 `None` 清除。读取 `request.on_event` 会保留原始 Python 回调对象的身份；`RunConfig.on_provider_event` 也遵循此规则。每个事件暴露字符串 `kind` 和类型化 `value`；`ProviderPartDelta` 值拥有自己的 `bytes`。回调后保留事件不会保留借用的 SDK 文本视图。要请求取消，请对赋给 `request.cancel_token` 的令牌调用 `token.cancel()`。

用 `asyncio.to_thread(provider.invoke, request)` 在事件循环线程之外执行同步提供商调用。仅取消该 await 不会取消提供商调用；需要将 `CancelToken` 赋给 `request.cancel_token`，并显式请求取消。

对于令牌已取消的有效准备句柄，提供商分发的预检会返回拥有所有权的 SDK `Cancelled` 失败。同步分发也会运行同一原生异步提供商路径直至结束。图、宿主或协程入口处的取消属于另一边界，可能在返回 `ProviderOutcome` 之前引发异常；请求取消不保证在每个边界都返回类型化的 `Cancelled` 数据。取消请求也不能证明请求没有发出、远程工作已经停止或不会产生费用。

[pybind11 GIL 文档](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil) 解释了为何释放 GIL 与为 Python 回调重新获取 GIL 是绑定的两项独立职责。

## 协议与可观测性

- MCP 客户端工具在构建后可通过 `neograph_engine.mcp` 使用。
- A2A 客户端类型在构建后可通过 `neograph_engine.a2a` 使用。
- `ProtocolHostAdapter` 将官方 Python A2A/ACP 服务器 SDK 与 NeoGraph 会话语义集成。
- `neograph_engine.tracing` 和 `neograph_engine.openinference` 为 Phoenix、Langfuse、Arize 及兼容后端发出供应商中立的 OTel/OpenInference 数据。

### 原生 A2A 发现与流式调用

`a2a.WireDialect` 为 `V0_3/V1_0`。`client.wire_dialect()` 在构造/强制 card fetch 后到首次 card-selected RPC 或成功 probe 前为 `None`。`AgentCard.supported_interfaces` 为含 `url/protocol_binding/protocol_version/tenant` 的独立 `AgentInterface` 列表；`card.raw` 为观察 JSON。客户端在兼容 JSONRPC 中优先匹配规范化 base URL；card URL 不改变 RPC endpoint。无 card 时仅数字 RPC error `-32601` 允许 probe，card-selected 调用不 fallback。`a2a.A2ARpcError.code` 是实际整数代码。

```python
from neograph_engine import a2a
from uuid import uuid4

client = a2a.A2AClient("http://127.0.0.1:8080")
card = client.fetch_agent_card()
for interface in card.supported_interfaces:
    print(interface.protocol_version, interface.tenant)

params = a2a.MessageSendParams()
params.message.message_id = str(uuid4())
params.message.role = "user"
params.message.parts = [
    a2a.Part.text_part("Explain this item."),
    a2a.Part.text_part("Keep the answer short."),
]
configuration = a2a.MessageSendConfiguration()
configuration.blocking = True
configuration.accepted_output_modes = ["text/plain"]
params.configuration = configuration
events = []

def on_event(event):
    events.append(event)  # Owned snapshot remains valid after the callback.
    return True

task = client.send_message_stream(params, on_event)
print(client.wire_dialect(), task.id, task.state)
```

先在该 endpoint 启动真实本地 A2A server，与 Chat 回环独立。`send_message(params)` 是非流式 multipart overload，仍保留 text overload。显式用 `client.set_authorization_header(authorization_header)` 设置认证，不记录该值。`Part.media_type/file/data/metadata`、message extensions/reference IDs、task artifacts 保留类型/数据观察，不是 provider native 权限。

`params.message` 和 `Task.status` 是 live inline 记录；optional configuration/向量/event 子记录为独立 snapshot，修改输入须重新赋值。检查 `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task` 和 `status_update/artifact_update/task/is_final()`。V1 opening task 不代表 final；原生 SSE 组装 status 和 append/replace artifact。blocking 调用释放 GIL，callback/所有者销毁获取 GIL。观察输出后不重新 dispatch。callback thread 不保证是 asyncio loop，需要时使用 `loop.call_soon_threadsafe`。


从 `neograph_engine.openinference` 导入 `OpenInferenceProvider`。构造签名为 `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")`；此 Python 类委托给原生 C++ 包装器，继承类型化的 `prepare(request)`、`dispatch(prepared)` 和 `invoke(request)`。默认 span 名只是标签，不会恢复 `complete` 方法。安装 `opentelemetry-api` 与 `opentelemetry-sdk`；构造时需要 OTel API，导出时需要配置 SDK span processor/exporter。

准备、准入失败和放弃准备请求都不会创建 LLM span。已准入的分发会启动一个 span，并在完成、SDK 失败或分发异常时结束。即使包装器及外部 tracer 引用已被回收，准备操作仍保留 Python tracer 适配器。阻塞的 `invoke`/`dispatch` 释放 GIL；OTel 调用和 Python 引用销毁时获取 GIL。Tracer 失败遵循原生尽力而为策略，不替换拥有所有权的结果、原始事件或产品异常。

适配器在分发时调用 Python tracer 的 `start_span`。调用方的上下文不会自动传入原生工作线程。分发前捕获所需的 OTel 父上下文，并按下例通过 tracer 适配器显式传入；图节点提供商需要图/节点父 span 时也应如此。参见 OTel 的[显式父上下文选择文档](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span)。

示例中的 `ParentContextTracer` 保留一个已捕获的父上下文。使用不同的逻辑父级时，请重新捕获上下文，并构造新的适配器/提供商包装器；重用旧适配器会继续使用先前的父级。

使用上方提供商示例的 `provider` 和 `controls`。运行以下片段前，请重新启动单请求协议端:

```python
import neograph_engine as ng
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import ConsoleSpanExporter, SimpleSpanProcessor
from neograph_engine.openinference import OpenInferenceProvider

class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer = tracer
        self.parent_context = parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)

traces = TracerProvider()
traces.add_span_processor(SimpleSpanProcessor(ConsoleSpanExporter()))
tracer = traces.get_tracer("python-guide")
with tracer.start_as_current_span("request"):
    observed = OpenInferenceProvider(
        provider, ParentContextTracer(tracer, otel_context.get_current()),
        span_name="llm.request",
    )
    request = ng.make_provider_request(
        observed, "example-model",
        [ng.ProviderMessage(role=ng.ProviderRole.User,
                            parts=[ng.Text("Say hello.")])],
        controls=controls,
    )
    prepared = observed.prepare(request)
    outcome = observed.dispatch(prepared)
    print("consumed:", prepared.consumed)
    print("failure:", outcome.failure is not None)
    print(outcome.text)
traces.shutdown()
```

控制台 exporter 应显示 `openinference.span.kind="LLM"` 的 `llm.request`，其 `parent_id` 应与 `request` span 的 `span_id` 一致。使用链接中的协议端时，打印结果应为已消费、无失败，并含 `Hello.`。对于新请求，`observed.invoke(request)` 合并相同的准备与分发流程；不要再次分发已消费的句柄。

LLM span 字段包含已准入的模型、声明的 temperature/输出上限、公开消息角色与可见 `Text` 内容，以及已知的输入/输出/总使用量计数器。缺失的计数器不会产生对应属性；报告的零仍为零。流式请求仅为可见文本内容增量添加 `llm.token` 事件，文本位于 `attributes["chunk"]`。SDK 失败以安全消息设置 ERROR 状态，并保留部分公开输出；成功完成设置 OK。`request.on_event` 仍接收原始类型化事件。

原生重放、推理 part、原始信封和原始事件不会进入这些 LLM span 字段。结果仍保留其原生消息和证据；追踪导出不会赋予重放保管或记账权限。公开文本可能包含敏感应用数据，请据此选择提示词和 exporter。`openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)` 仍是独立的图事件上下文管理器，不替代提供商 LLM span。

## 可选组件

公共包如实标记可选的 C++ 组件：

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- 缺失的组件是缺失的，而不是在 Python 中模拟的；
- PyPI wheel 启用 Program/QuickJS、LLM、MCP 和 SQLite；源码构建遵循其 CMake 选项。

## 测试与示例

绑定测试套件涵盖 Core 执行、自定义回调、asyncio、取消、Program 编译/运行时、强制 Hook、严格上下文、SQLite 持久化、协议以及 README 示例。

- [Python 示例](../bindings/python/examples/README.md)
- [C++ 示例](../examples/README.md)
- [QuickJS 创作边界](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [严格运行时插桩](STRICT_RUNTIME_INTERPOSITION.md)
