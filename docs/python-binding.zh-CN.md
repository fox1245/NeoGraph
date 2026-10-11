<!-- neograph-i18n: source=docs/python-binding.md locale=zh-CN source_sha256=a85f65d34d9f9d6112729356028aa150f05ef89a1f2381407ee5cbf54b130054 -->
# Python 绑定

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` 是同一 C++ 运行时的 pybind11 接口。该 wheel 支持 Core、LLM、Program/QuickJS、MCP 和 SQLite 运行时持久化；可选源码构建仅暴露其编译的组件。

本指南要求针对 SchemaProvider SDK `0.3.0`、接口修订版 `6` 和共享库 ABI 修订版 `6` 构建的 wheel；一起安装其捆绑组件。暴露 `complete` 的旧 wheel 使用不同的提供商接口，无法运行以下类型化示例。

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

### 请求族控制

包版本、native archive v3 和 portable JSON v2 独立。示例只构造控制，需要对应请求族的已准入 provider。保持首次回环不变；OpenRouter 专用控制在未准入本地 origin 上于 I/O 前拒绝。

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

`outcome.usage.provider_cost` 是不可变的 `ProviderReportedCost` 视图。可选 `ProviderUsdAmount` 字段 `total/upstream_total/upstream_input/upstream_output` 包含 `nano_usd/evidence/rounding`，`status` 是依此顺序的四元素数组，`byok_status` 描述可空 `is_byok`。`ProviderCostStatus/ProviderCostSource/ProviderCostRounding` 保留缺失、可用、格式错误、精度超限、overflow、未知货币与冲突证据。金额从解析后的 binary64 向上舍入，不是从原始 decimal 字面量舍入。报告零或 `is_byok=False` 不等于缺失。它们不是发票或 token 预算扣账；逐次调用金额证据应查看有序 `provider_outcomes`，而非聚合 token bank。

`UsageAccumulator` 仅聚合 token。单次报告保留金额元数据；第二次报告后 snapshot 的 `provider_cost` 明确重置为 canonical Missing/None，既不保留过时的首次费用，也不进行金额求和。恢复该 snapshot 不重建费用或更新 token 权限。精确金额证据须从每个原始 outcome 或 provider journal 项目读取。

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
checkpoint_id = result.checkpoint_id

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
    # GraphState.get takes only the channel name; a missing channel is None.
    return [ng.ChannelWrite("value", (state.get("value") or 0) + 1)]

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
print(result.status, result.output["value"])  # ProgramTerminalStatus.Completed 1
```

`ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer` 和 `add_registered_condition` 复制本进程中已经注册的可执行单元。注册方式为 `@ng.node` 或 `ng.NodeFactory.register_type`、`ng.ReducerRegistry.register_reducer`、`ng.ConditionRegistry.register_condition`。传入未注册的名称时，这三个方法都会抛出 `ng.ProgramRegistryError`，消息会指出所需的注册调用。该错误的 `kind` 为 `"node"`、`"reducer"` 或 `"condition"`，`name` 为缺失的名称。它同时继承 `ValueError` 和 `RuntimeError`，因此捕获其中任一类型的现有处理代码仍能捕获它。使用注册表快照中不存在的节点类型、reducer 或条件的 Program 会改为在编译时失败：`ProgramCompiler.compile` 和 `LocalProgramHost.compile_admit` 都会抛出带有诊断 `P_CORE_GC_REGISTRY_MISS` 的 `ng.ProgramCompileError`。

每个诊断 dict 都有 `help` 项：对编译返回的诊断，它是包含 `doc`（[诊断指南](diagnostics.md)中的对应条目）、`related`、`suggestions` 和 `candidates` 的 dict；对封存的 `bundle.diagnostics`，它为 `None`。异常中总是带有它。`ProgramCompiler(..., echo_routes=True)` 还会返回一个包含路由后继表的 `P_CORE_ROUTES` 注记。在 `ProgramCompiler` 或 `LocalProgramHost` 上传入 `soundness_check=True` 会运行[健全性检查](program-graphs.md#soundness-check)，返回带 witness trace 的 `P_CORE_S1` 至 `P_CORE_S5` 警告以及 `P_CORE_SOUNDNESS` 摘要。它们都不会被封存。

`LocalProgramHost` 是一个所有者作用域的内存便捷宿主。它仍然使用 C++ 编译器、Catalog、准入(admission)策略、转换存储和 ProgramRuntime。生成的提案在准入(admission)前还应通过宿主语义验证器；参见 [DSL 能力评估](DSL_CAPABILITY_EVAL.md)。

销毁 `LocalProgramHost` 时，会在真实 `ProgramRuntime` 取消、排空并 join 调度器工作期间释放调用方的 GIL，使运行中的 Python 节点可以结束。销毁剩余宿主成员前会重新获取 GIL；Python 回调/对象所有者仍按 GIL 安全方式销毁。

原生 Program 的记录执行 API 是取代 `start_recorded` 的 `ProgramRuntime::replay_recorded`。`LocalProgramHost` 暴露 `run`、`start` 和 `resume`，不暴露原始记录绑定的控制平面。Program 结果在现有句柄、预算和权限字段之外，还保留原生运行时提供的类型化提供商结果及失败证据。

`ProgramResult.failure` 是只读的 `ProgramFailure` 值或 `None`，不是字典。其 `provider_outcome` 与 `provider_cause` 保留提供商失败证据；`code`、`message`、`operation_id`、`core_node`、`attempts` 和 `witness` 描述失败的操作。结果字段还包括 `bundle_id`、`operation_id`、`attempt`、`checkpoint`、`interrupt` 和 `provider_budget_authority`。

精确安装的 JavaScript 词汇表以字典形式提供：

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### 模块结构与 `callCore`

JavaScript Program 必须导出 `define()`，它返回一个图构建器。该图是 Program 唯一的 Core。缺少 `define()` 时，编译会抛出带 `P_JS_DEFINE_MISSING` 的 `ProgramCompileError`，因此只导出 `main()` 的模块会被拒绝。`main()` 是可选的；没有它时，运行会直接执行 Core。

`ng.callCore(name, input, sourceSite)` 必须用传给 `ng.graph(...)` 的名称（上例中为 `"main"`）指定该 Core。使用其他名称会使运行以 `P_JS_CONTROL_COMMAND` 失败。要运行不同的步骤，请把它们放进该图，并用不同的输入再次调用。每种节点类型在传给 `add_registered_node` 之前，都要先用 `@ng.node` 或 `ng.NodeFactory.register_type` 注册；对于未注册的类型，该方法会抛出 `ng.ProgramRegistryError`。

完成的 `callCore` 会把 Core 的输出对象返回给 `main()`。上面的快速入门把它原样作为 `result.output` 返回：

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels` 保存每个通道的 `value` 和 `version`。除非同名的顶层键已存在，每个通道的值还会复制到以通道名命名的顶层键，因此 `main()` 可以读取 `result.value`；名为 `channels` 或 `global_version` 的通道只能在 `channels` 下访问。barrier 列表按排序后存储：`graph.barrier("join", ["zeta", "alpha"])` 在 `bundle.sealed_core_definitions` 中显示为 `{"wait_for": ["alpha", "zeta"]}`，顺序没有任何影响。

### 失败的命令会结束运行

只有完成的命令才会恢复 `main()`。当 yield 出的命令未能完成时，例如 `callCore` 的节点抛出异常、耗尽 Core 步数预算、停在再也无法触发的 barrier 上或超过 wall-time 限制，`ProgramRuntime` 会记录该结果，并把它作为运行的最终结果，带有相应的 `status` 和 `failure`。生成器不会被恢复：包住 `yield` 的 `try`/`catch` 不会执行，之后的 JavaScript 也不会执行。按其失败策略失败的 join（`ng.all`、`ng.race`、`ng.quorum`）也会以同样方式结束运行。在 Python 中通过 `result.status` 和 `result.failure` 处理失败。被中断的命令则不同：运行以 `Interrupted` 停止，`resume()` 会继续该命令。

### 图检查与 barrier 停滞

编译会沿调度器实际采用的路由检查 Core 图：带条件边的节点只通过其第一条条件边路由。JavaScript Program 是严格文档，因此以下每一项都会抛出 `ng.ProgramCompileError`：

- `P_JS_GRAPH_ARGUMENT`：出现在把节点命名为 `__start__` 或 `__end__` 的 `.node()` 调用处；
- `P_JS_GRAPH_ENTRY`：`define()` 返回的图有节点但没有 `.entry()` 时；
- `P_CORE_E13`：针对同时带有条件边的节点发出的静态边，因为该边永远不会触发；
- `P_CORE_E14`：针对同一节点的第二条条件边，因为它永远不会路由。

`P_CORE_E15` 是 `bundle.compile_diagnostics` 中的警告：某个 barrier 成员有一条通往 join 或 `__end__` 以外位置的路由，走这条路由会使 join 停滞。循环是有效的；要以 `P_CORE_E16` 拒绝所有循环（例如用于计划和检查清单），请向 `ProgramCompiler` 或 `LocalProgramHost` 传入 `require_acyclic=True`。

barrier 在其 `wait_for` 列表中的每个节点都路由到它之后才会触发。以其他方式到达 join 的节点（例如成员失败路由上的修复(fix-up)节点）不能代替该成员。当运行已没有可运行的内容而 join 仍持有信号时，运行不会完成：它以 `P_CORE_BARRIER_STALLED` 结束为 `Failed`，`failure.core_node` 指明该 join，`failure.witness["stalled_barriers"]` 列出每个停滞的 join 及其 `wait_for`、`received` 和 `missing` 节点。只有最后一个 super-step 中通往 `__end__` 的显式路由才能让这样的运行正常结束：通往 `__end__` 的静态边（由 `.exit()` 写入）、通往 `__end__` 的条件路由，或 `Command.goto("__end__")`。没有后继的节点不行，更早 super-step 中的显式结束也不行。处于相同状态的 `GraphEngine` 运行返回 `ng.RunStatus.BarrierStalled`，`result.output["_neograph"]["barrier_stalled"]` 包含相同的详细信息。

### 错误与诊断

在 Python 中抛出的 Program 错误是类型化的，并保留其诊断：

| 异常 | 抛出方 | 基类 |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`、`LocalProgramHost.compile` 和 `compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit` 和 `compile_admit`，例如 `P_ADMIT_POLICY` 或 `P_ADMIT_PROVIDER` | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`、`start`、`start_active` 和 `resume`，例如 `P_START_BUDGET` 或 `P_RUN_NOT_FOUND` | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer` 和 `add_registered_condition` | `ValueError`、`RuntimeError` |

前三种错误的消息以 `Program compilation failed`、`Program admission failed` 或 `Program operation failed` 开头，后面每个诊断占一行：其代码、JSON 指针和消息，有源位置时还有 `(source:line:col)`。`.diagnostics` 是字典列表，每个字典包含 `code`、`severity`、`phase`、`message`、`source_id`、`json_pointer`、`span` 和 `witness`。

编译成功的 bundle 带有两个列表。`bundle.compile_diagnostics` 是编译器返回的内容：当前规则下的警告（JavaScript 调用位置在 `span` 中），以及 `P_JS_CONTROL_CHARACTER` 等编写警告。请向作者展示此列表。`bundle.diagnostics` 是封存进 bundle 并由 `bundle.id` 覆盖的副本。它保留 bundle 最初封存时的规则和措辞，因此更新的编译器不会改变 bundle id，也正因如此它可能与前者不同：它没有 span，并且可能仍列出当前规则已删除的警告，例如针对只有 Python 节点写入的通道的 `P_CORE_E6`。编译器和 `LocalProgramHost` 都不会向 stderr 打印 lint。

<a id="model-calling-nodes"></a>
### 调用模型的节点

`LocalProgramHost` 为它准入(admission)的 Program 中的内置 `llm_call` 节点绑定一个提供商。注册表按名称声明提供商以及需要它的节点；宿主在一个非机密的 `binding_label` 下提供提供商对象、模型和请求控制：

```python
from neograph_engine.llm import SchemaProvider

registry = (
    ng.ProgramRegistryBuilder()
    .add_provider("openrouter", "1.0.0", "sha256:" + "3" * 64)
    .add_registered_node("llm_call", "1.0.0", "sha256:" + "4" * 64, provider="openrouter")
    .add_registered_reducer("append", "1.0.0", "sha256:" + "5" * 64)
    .build()
)
controls = ng.ProviderControls()
controls.max_output_tokens = 256
ceiling.model_tokens = 100_000
ceiling.monetary_microunits = 500_000
# descriptor and options as in "Descriptor and runtime policy" above
host = ng.LocalProgramHost(
    registry, "tenant-a", ceiling,
    node_context=ng.NodeContext(provider=SchemaProvider(descriptor, options),
                                model="deepseek/deepseek-v4-flash-0731",
                                provider_controls=controls),
    binding_label="openrouter-prod")

source = ng.ProgramSource.from_javascript("chat.js", r'''
export function define() {
  const graph = ng.graph("chat");
  graph.channel("messages", {reducer: "append", initial: []});
  graph.node("llm", {type: "llm_call"});
  graph.entry("llm");
  graph.exit("llm");
  return graph;
}
''')
run_budget.model_tokens = 100_000
run_budget.monetary_microunits = 500_000  # 0.50 USD
version = host.compile_admit(source, run_budget)
result = host.run(version, {"messages": [{"role": "user", "content": "Hi"}]}, run_budget)
print(result.usage.monetary_microunits, [r.evidence for r in result.monetary_records])
```

`add_provider(name, semantic_version, implementation_digest)` 注册提供商的 identity，`add_registered_node("llm_call", ..., provider=name)` 把以该 identity 为静态需求的内置节点注册进去。`provider=` 只绑定 `llm_call`，对其他类型或未知提供商抛出 `ValueError`。无论 descriptor id 是什么，宿主都把 `node_context` 的提供商绑定到该名称。`NodeContext(provider_controls=...)` 接受节点发送的 typed `ProviderControls`，例如 `max_output_tokens`。`node_context` 需要提供商和 `binding_label`，且不能带工具。

绑定中只有标签进入 identity。需要该提供商的 Program 版本带有一个 capability receipt，其 identity 是标签与提供商注册表 identity 的摘要；提供商的密钥、endpoint 和模型不在其中，因此更换模型不改变版本 id，更换标签会改变它。不需要提供商的 Program 没有 receipt，bundle id 与版本 id 保持不变。标签必须是由 `a-z`、`0-9`、`.`、`_`、`-` 和 `:` 组成的 1 到 64 个字符，以字母或数字开头，且不能看起来像 API 密钥。没有 `node_context` 时，需要该提供商的 Program 在准入时以 `P_ADMIT_PROVIDER` 被拒绝；含有未带 `provider=` 注册的 `llm_call` 或 `intent_classifier` 节点的 Program 也一样。

每次调用都依据编译进 NeoGraph 的价格目录，针对运行的 `monetary_microunits` 计价。发送请求之前会预留调用的上限。无法确定上限的调用（没有目录条目的模型、没有 route ceiling 的 router alias、没有输出上限的请求）或预留超出剩余预算的调用，会在发送任何内容之前被拒绝，运行以 `P_MONEY_*` 代码失败。与所有 `ProgramRunBudget` 字段一样，`monetary_microunits = 0` 不授予任何额度，因此这样的运行会拒绝所有调用。收到回复后，`result.monetary_records` 为每次调用保存一条 `ng.ProgramMonetaryRecord` 及其 `evidence`：`reported`（OpenRouter 的 `usage.cost`）、`estimated`（目录费率；`catalog_version` 与 `catalog_digest` 指明目录）或 `held`（结果或 usage 未知，因此收取预留额）。`result.usage.monetary_microunits` 是它们的总和。代码、上限与结算规则见[计价证据与价格目录](reference-en.md#pricing-evidence-and-the-price-catalog)。

调用模型的 Python 节点自己持有提供商，Program 预算看不到这类调用。

### 子 Program

`ng.spawn` 需要子绑定解析器，而 `LocalProgramHost` 不配置它。因此，从 Python 发起的每个 `ng.spawn` 都会使运行以 `P_CHILD_BINDING`（"Program spawn has no configured child binding resolver"）失败。子 Program 需要一个设置了 `RuntimeConfig::child_binding_resolver` 的 C++ 宿主；该解析器把每个绑定名映射到 `link_module_child` 返回的 `ModuleLinkReceipt` 以及已准入(admission)的子 `ProgramVersion`。子 Program 的生命周期参见[递归智能体](PROGRAM_RECURSIVE_HARNESSES.md)。

### 预算

传给 `LocalProgramHost` 的 `budget_ceiling` 会成为准入(admission)策略的上限。除 `max_child_depth` 和 `max_total_children` 外，每个字段都必须为正数；即使 Program 从不调用模型，`model_tokens`、`monetary_microunits` 和 `max_dynamic_compiles` 也必须为正数。否则构造函数会抛出列出零值字段的 `ValueError`。两个子字段必须同时为零或同时为正数。

`compile_admit(source, budget)` 和 `compile(source, budget)` 会把 `budget` 作为每个字段的最小值和最大值封存进 bundle；封存预算超过上限的 bundle 会在准入(admission)时被拒绝。针对该版本的每次 `run`、`start` 和 `start_active` 调用都必须逐字段传入相同的值。更大或更小的预算会抛出带 `P_START_BUDGET` 的 `ng.ProgramDiagnosticError`，其消息会指出第一个不同的字段，例如 `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`。要使用不同的预算，请重新编译并准入(admission)；结果是一个新的 `ProgramVersion`。运行预算的 `wall_time_ms`、`max_concurrency`、`max_program_operations` 和 `max_core_steps` 必须为正数；`model_tokens` 和 `monetary_microunits` 可以为零。零表示不授予任何额度：以 `model_tokens = 0` 使用了模型 token 的运行以 `BudgetExhausted` 结束；`monetary_microunits = 0` 时，每个被计价的提供商调用都会在发送前被拒绝（见[调用模型的节点](#model-calling-nodes)）。

### 调度线程、worker 与超时

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)` 接受两个线程设置。`scheduler_threads` 设置驱动运行（JavaScript 控制、`callCore` 分派、join 和截止时间）的 `ProgramRuntime` 调度池的线程数。它必须至少为 1。`worker_count` 设置每个 Core 引擎用于单个图内 fan-out 的 worker 池，且不得超过已准入(admission) Program 的 `max_concurrency`；默认值为 1 时，fan-out 分支在调度线程上运行。

在 Program 运行期间，Python 节点主体不会占用调度线程。它在由绑定持有的 worker 池上运行，默认 32 个线程；要更改，请在首次 Program 运行之前把 `NEOGRAPH_PYTHON_NODE_THREADS` 设为 1 到 1024 之间的值。即使使用默认的单个调度线程，宿主的多个运行、`ng.all`、`ng.race` 和 `ng.quorum` 的成员，以及一次 Core 运行的 fan-out 分支也会同时推进，并且截止时间会在节点运行期间触发。`ng.race` 返回第一个完成的成员，`ng.quorum` 在所需的成功数到齐后返回。Python 代码仍在 GIL 下运行，因此 CPU 密集型节点不会并行；节点在释放 GIL 时（例如在 `time.sleep`、I/O 或原生调用中）才会重叠执行。普通的 `GraphEngine` 运行仍在调用线程上运行 Python 节点主体。

两种限制都不会中断正在运行的 Python 节点；取消是协作式的。运行的 `wall_time_ms` 用尽时，运行时会取消该运行，结果在正在运行的节点返回后才到达，`status` 为 `TimedOut`，`failure.code` 为 `P_RUNTIME_TIMEOUT`，消息为 `Program wall-time budget (N ms) expired`。被超时中断的操作的文本位于 `failure.witness["cause_message"]`。`ng.await(command, timeoutMs, sourceSite)` 的截止时间过后，运行以 `P_AWAIT_TIMEOUT` 结束为 `TimedOut`，截止时间之后到达的完成不会被接受。在这两种情况下，子 Core 运行都会被取消；检查 `input.ctx.cancel_token.is_cancelled()` 的节点会提前停止，忽略它的节点会把结果推迟到它返回为止。`TimedOut` 和 `Cancelled` 结果会在 `execution_trace` 中列出已完成的 Core 节点。

Program 引擎不会向 stderr 打印 lint，也不会打印 fan-out 提示。建议设置 `EngineConfig::worker_count` 的提示只来自你自己构建的 `GraphEngine`。

### 内存状态与恢复

默认情况下，`LocalProgramHost` 把 bundle、版本、激活、运行记录和检查点保存在宿主对象内部的内存中，不向磁盘写入任何内容。请在启动该运行的宿主实例仍然存在时，通过该实例恢复运行。进程重启后，或从另一个 `LocalProgramHost` 调用时，`resume()` 会抛出带 `P_RUN_NOT_FOUND` 的 `ng.ProgramDiagnosticError`：`run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them`。PostgreSQL Program 存储是 C++ 宿主 API。

传入 `sqlite_directory=path` 时，宿主把 bundle、版本、运行记录和检查点保存在该目录的 SQLite 文件（`program.db`、`checkpoints.db`）中，把提供商调用保存在提供商调用 journal（`provider_calls.db`）中，因此运行能在进程之后留存：用同一目录和同一注册表打开的宿主会继续它。`resume()` 继续被中断的运行，`reconnect(run_id)` 继续停止的进程留下的仍在运行中的运行。恢复从不重新发送 journal 记录过的请求：journal 重放回复，调用只收费一次。绑定提供商的宿主还需要 `native_history_archive=`（第一次用 `ng.NativeArchive.provision`，之后用 `ng.NativeArchive.open`），因为 journal 把每个回复保存在该 archive 的 custody 之下。没有子预算的仅 `define()` Program 运行在其 wall time 内持有 execution lease：在 lease 过期前 `reconnect()` 抛出 `P_RUN_LEASE_HELD`，之后以 `TimedOut` 结束运行，并对 journal 记录的调用收费。崩溃恢复从运行最后一个持久检查点（例如中断留下的检查点）继续。

`resume(run_id, value)` 返回一个句柄；调用 `.wait()` 获取结果。不传 `pending_id` 时，它会恢复该运行中唯一正在等待的调用。没有调用或有多个调用在等待时，它会抛出 `ValueError`；请把 `result.interrupt.pending_input.call_id` 作为 `pending_id` 传入，以精确指定该调用。

`resume()` 返回的结果中，`execution_trace` 只列出恢复后运行的 Core 节点；中断前运行的节点出现在被中断的结果中。对于在 `b` 之前中断的 Core `a → b → c`，被中断的结果为 `["a"]`，恢复后的结果为 `["b", "c"]`。

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

`HookBoundaryBlocked` 携带发生阻断的 Hook 的 `invocation_id` 及其 `state`（`"failed"`、`"timed_out"` 等）。Python 工具遵循[工具失败契约](reference-en.md#tool-failure-contract)：在工具内抛出的 `HookBoundaryBlocked`（例如来自嵌套的引擎运行）会停止运行，`NodeInterrupt` 会暂停运行，其他任何异常都会成为模型读取的工具结果。

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
params.message.role = a2a.Role.User
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
