<!-- neograph-i18n: source=docs/python-binding.md locale=zh-CN source_sha256=c027e55aafc7a82ed76cae8126196d9edc668bcb52bded563f7e6a4496f744a4 -->
# Python 绑定

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` 是同一套 C++ 运行时的 pybind11 接口。wheel 启用了 Core、LLM、Program/QuickJS、MCP 和 SQLite 运行时持久化；从源码构建时，可选组件只有编译进去的才会暴露。

本指南要求使用针对 SchemaProvider SDK `0.3.5`、接口修订版 `6` 和共享库 ABI 修订版 `6` 构建的 wheel，并且要连同其捆绑的组件一起安装。仍然暴露 `complete` 的旧版 wheel 使用的是另一套 provider 接口，无法运行下面带类型的示例。

```bash
pip install neograph-engine
```

## 带类型的 provider 请求与 outcome

`Provider` 把请求的准备和分发分成两步。`SchemaProvider` 通过 SchemaProvider SDK 校验并编码某个请求族（family）专属的请求。`PreparedProviderRequest` 持有这份原生的准备结果，直到某次分发把它消耗掉。

| 操作 | Python 签名 | 结果 |
|---|---|---|
| 准入(admission)描述符 | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`；无效的封闭 JSON 会抛出 `ValueError` |
| 构造 provider | `SchemaProvider(descriptor, options, defaults)` | 已准入 endpoint 和请求族、带运行时策略的 provider |
| 构造请求 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 带类型的 `ProviderRequest` |
| 准备 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 分发一次 | `provider.dispatch(prepared)` | 持有所有权的 `ProviderOutcome` |
| 准备并分发 | `provider.invoke(request)` | 持有所有权的 `ProviderOutcome` |

`model` 必须显式指定。`ProviderControls` 提供带类型的限制和生成控制项，包括 `max_output_tokens`、`temperature` 和 `top_p`。工厂函数会构造 SDK 中各请求族专属的 payload，不能用原始字典代替。要请求流式输出，把 `request.mode` 设为 `ProviderMode.Stream`；只设置 `on_event` 不会选择流式。`request` 上还有 `cancel_token`、`on_event`、`observer_limits`，以及可选的 `timeout_ms`。

`ProviderControls.provider` 和 `response_format`、`SchemaProviderDefaults.provider`、`OpenRouterRouting.max_price`，以及 `ProviderToolResult.host`，返回的都是与原对象分离的可选记录（副本）。记录存在时，先读出来，改完再赋回属性；只改 `controls.provider.order`，controls 本身不会更新。赋值 `None` 可清除记录。

给 `request.timeout_ms` 赋一个可表示的非负值，会立即启动一个绝对截止时间。请在准备之前才设置它；之后再读取，得到的是剩余的毫秒数，过期后固定为 0。保持为 `None` 时，准备阶段使用 provider 的默认超时。拿着已准备好的句柄等待，不会续期它的截止时间。

### 描述符与运行时策略

描述符是 `load_provider_descriptor` 接受的、封闭且带版本的 JSON。它准入的内容有：请求族、`base_url`、路由、字段绑定和认证规则。`model` 由请求提供；凭据由运行时选项提供。`ProviderRuntimeOptions` 提供 `api_key`、`default_timeout_ms`、`ca_file`、`workers`，以及 SDK 的传输和资源限制。`SchemaProviderDefaults` 提供带类型的 provider 默认值。凭据只放在运行时选项里，不要写进描述符文件和日志。

`ProviderDescriptorPolicy.identity` 返回 Python `bytes`，内容是 SDK policy 的原始 SHA-256 摘要。要显示它，用 `policy.identity.hex()`；identity 本身既不是 UTF-8 文本，也不是十六进制字符串的别名。

当前的传输层使用 libcurl，没有 `prefer_libcurl` 开关，不能在运行时覆盖 endpoint，也没有 WebSocket 传输。描述符的准入规则和支持的请求族，见 [C++ provider 参考](reference-en.md)。

先按 [SDK 使用指南](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)启动只处理一次请求的回环（loopback）对端，再在另一个终端运行下面这个纯文本示例。示例中的 endpoint、模型和输出上限都与该对端一致，也不会发送任何凭据。如果对端是 HTTPS，通过 `NG_EXAMPLE_CA_FILE` 设置它所信任的 CA。要改用托管的 endpoint，需要显式修改描述符的 origin 和请求中的模型，并在运行时选项里提供该 origin 的凭据；调用托管服务可能产生费用。

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

不需要句柄时，`provider.invoke(request)` 把准备和分发合在一起完成。`outcome.text` 是可见文本的投影；`outcome.messages` 保留完整的带类型 parts。缺失的用量计数器打印 `unknown`，上报为零的打印 `0`。

使用上面链接中的模拟对端，预期输出如下：

```text
consumed: True
Hello.
output tokens: 0
```

把对端的整个 `usage` 成员删掉并重启，就能走到计数器缺失的路径；最后一行应变成 `output tokens: unknown`。这检查的是协议映射，而不是真实模型的 token 计数。

### 各请求族的控制项

可移植的控制项（`max_output_tokens`、`temperature`、`reasoning_effort`、`store` 等）是 `ProviderControls` 的成员；各请求族自己的控制项在该族的嵌套结构（`chat`、`responses`、`messages`、`generate`、`interactions`）里。一次调用只应用 provider 所属请求族的嵌套结构，忽略其他的，所以节点调用不同请求族的图也只需一个 `ProviderControls`。请求族无法表达的可移植控制项仍会抛出 `ValueError`。下面的片段只构造控制项，不分发。第一个回环请求保持不变：仅限 OpenRouter 的控制项，会在 I/O 之前拒绝那个未准入的 origin。

```python
controls = ng.ProviderControls()
controls.max_output_tokens = 4096

# Chat Completions (OpenRouter-only members need a declared OpenRouter origin).
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
controls.chat.reasoning = reasoning
controls.chat.usage_include = True
controls.chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]
routing = ng.OpenRouterRouting()
price = ng.OpenRouterMaxPrice()
price.prompt_nano_usd = 75_000_000       # 0.075 USD per million prompt tokens
price.completion_nano_usd = 300_000_000  # 0.30 USD per million completion tokens
routing.max_price = price
controls.provider = routing

# Responses: hosted tools the service runs.
image = ng.ResponsesImageGenerationTool()
image.size = "1024x1024"
controls.responses.hosted_tools = [image, ng.ResponsesWebSearchTool()]
controls.responses.verbosity = ng.ResponsesVerbosity.Low
controls.responses.include = [ng.ResponsesInclude.ReasoningEncryptedContent]

# Anthropic Messages: thinking, effort, caching and server tools.
controls.messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
controls.messages.output_effort = ng.MessagesOutputEffort.High
search = ng.MessagesToolDefinition()
search.type = "web_search_20250305"
search.name = "web_search"
search.max_uses = 3
controls.messages.tools = [search]

# Gemini generateContent: speech output.
controls.generate.response_modalities = [ng.GeminiResponseModality.Audio]
voice = ng.GeminiVoiceConfig()
voice.prebuilt_voice_name = "Kore"
speech = ng.GeminiSpeechConfig()
speech.voice_config = voice
controls.generate.speech_config = speech

# The same controls, written as data. Unknown keys are refused.
controls = ng.ProviderControls.from_dict({
    "max_output_tokens": 4096,
    "responses": {"hosted_tools": [{"type": "image_generation", "size": "1024x1024"},
                                   {"type": "web_search"}]},
    "generate": {"response_modalities": ["Audio"],
                 "speech_config": {"voice_config": {"prebuilt_voice_name": "Kore"}}},
})
```

先用默认构造创建记录再逐个字段赋值，或者用 `from_dict` 构造：所有控制项类及其嵌套结构中的 SDK 类型都接受 dict，拒绝未知的键，枚举按成员名（`"Audio"`）接受，托管工具、内置工具和响应格式按 `"type"`（`"image_generation"`、`"google_search"`、`"audio"`）选择类。SDK 没有对应类的 Responses 托管工具类型会成为 `ResponsesOpenRouterServerTool`（`openrouter:` 类型）或 `ResponsesGenericHostedTool`，需要 SDK 策略准入。嵌套结构本身与原对象相连（`controls.generate.speech_config = ...` 会改变 `controls`），但其中的可选记录和向量返回分离的副本：改完要赋回去。`OpenRouterRouting.max_price` 就是其中之一：`routing.max_price.prompt_nano_usd = ...` 改的是副本，所以应该构造一个 `ng.OpenRouterMaxPrice`，再把整个对象赋回去。它的金额都是整数 nanoUSD：`prompt_nano_usd` 和 `completion_nano_usd` 是每百万 token 的价格，另外还有 `image_nano_usd`、`audio_nano_usd` 和 `request_nano_usd`；发送时转成精确的十进制美元（`75_000_000` 即 `0.075`）。至少要设置一项，且每一项都不能超过 2**53。`SchemaProviderDefaults.provider` 对 `max_price` 的合并方式与其他路由字段相同：请求自己的值会替换默认值。`ChatReasoningOptions` 还有 `max_tokens` 和 `exclude`。`chat.reasoning`、`chat.include_reasoning`、`chat.usage_include`、`chat.models`、`chat.plugins` 和 `chat.image_config` 要求描述符声明了 OpenRouter origin；标量的 `reasoning_effort`、`service_tier`、带类型的路由和响应格式则不受此限。0.15 的扁平成员（`chat_reasoning`、`responses_include`、`messages_tool_choice`、`gemini_thinking_level` 等）在 0.16.0 中仍可用，并会移入对应的嵌套结构；其他 family 与 0.15 一样拒绝它们，同时设置扁平成员和它的嵌套成员会抛出 `ValueError`。它们将在 0.17.0 中移除。

Responses 的 verbosity 取 `Low/Medium/High`，truncation 取 `Disabled/Auto`；include 还支持 `WebSearchSources`、`FileSearchResults`、`MessageOutputTextLogprobs`、`ComputerCallOutputImageUrl`、`CodeInterpreterCallOutputs`。`responses.include = None` 保留默认的加密 reasoning include；显式给出列表（包括 `[]`）则以该列表为准，并且可能导致之后无法使用 native replay。已有的 store、reasoning、托管工具和工具调用上限，仍然是带类型的。

Messages 的 thinking 取 `Manual/Adaptive/Disabled`。Manual 要求 `thinking_budget` 不小于准入的最小值，并且小于输出上限；没有指定模式但给了 budget 时，选用 manual。Adaptive 和 disabled 不允许设置 budget。按 vendor 的语义，启用 thinking 时会省略 temperature；而模型层面的禁止规则会拒绝显式指定的 temperature。effort 取 `Low/Medium/High/Max/XHigh`，缓存 TTL 取 `FiveMinutes/OneHour`；工具选择取 `Auto/Any/None_/Tool`，其中 `Tool` 要求 `name` 是一个已声明的客户端工具。Messages 的路由要求描述符声明了 OpenRouter origin。

Gemini 的 thinking level 取 `Minimal/Low/Medium/High`，与 `thinking_budget` 互斥。工具选择取 `Auto/Any/None_/Validated`，并带有列表 `allowed_function_names`，与 `required_tool` 互斥。安全类别取 `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`，阈值取 `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`。请求族不对、取值越界，以及互相冲突的 thinking、上限、工具控制项，都会在 I/O 之前被拒绝。按模型名前缀禁止 temperature 的规则，按 ASCII 不区分大小写来比较，最后一个 `/` 之后的部分同样参与比较：Chat/Responses 用 `gpt-5`、`gpt-6`、`o1`、`o3`、`o4`；Messages 用 `claude-opus-4-7`、`claude-opus-4-8`、`claude-opus-5`、`claude-sonnet-5`、`claude-fable-`。参见 [SDK 的控制项准入](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)。

### 准入之前的部署 header

直接调用 `load_provider_descriptor(source, policy=None)` 时，`${VAR}` 会按字面处理。下面这些辅助函数会在描述符真正准入之前，先处理来自宿主的值：

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` 会为 Messages 读取可选的 `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA`。未设置或为空的值会被省略。描述符里写死的 header 优先于环境变量；显式传入的键值对又优先于前两者，且名称不区分大小写。重复的覆盖项、无效或保留的名称，以及换行符，都会使准入失败。两个辅助函数都接受可选的 `policy`；它们既不会修改已准入的描述符，也不会求值编码器模板。

### Responses 游标与外来的 Gemini 历史

`previous_response_id` 用来选定由 provider 保存的 Responses 状态。此时只发送新的输入，不要再重复发送完整历史。从一次成功的 Responses 调用开始，保留它绑定的控制项和真实的终止 ID：

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses = ng.ProviderControls()
responses.responses.previous_response_id = first_outcome.messages[-1].id
responses.responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history` 是 `ProviderMessage` 的向量，不是单个 response，也不是 JSON 对象。它不会发送出去。要具备真实的客户端工具所有权，可能需要完整的原始前缀，外加 ID 等于该游标的终止 assistant；纯粹由服务端保存的文本续接，可以让它留空。之后在进程内产生的游标输出，保留私有的终止所有权，但不会因此变成完整历史的 native replay，也不具备 archive 权限。失败、被编辑，或者 origin/模型/配置/路由对不上的状态，一律拒绝。完整的 native replay 和 archive 仍然需要它们原来的完整前缀。参见 [SDK 的 Responses 续接](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)。

如果历史里的 assistant `Text`/`ProviderToolCall` 是调用方自己构造的外来内容，没有 native 状态、wire 输出和签名，请显式设置 `controls.generate.history_mode = ng.GeminiHistoryMode.PortableForeign`；默认值是 `NativeOnly`。只有第一个外来的 function call 能绕过 Google 的签名校验器；纯文本的轮次不带签名。真实的 native 分组仍然要经过严格校验。这个模式无法修复、剥离或降级一个失败的 seal，也不能引入 reasoning 的权限，更不能让可移植数据获得 native replay。参见 [SDK 的外来 Gemini 历史](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)。

PortableForeign 既拒绝签名，也拒绝导入的 wire 元数据。游标输出的 `NativeReplay.complete == False`，不能进入 archive；它的私有所有者也不授权完整历史的重放。


### 已准备的句柄与 Python provider

分发会消耗已准备的句柄，只能用一次，分发失败也算用掉。分发之前检查 `prepared.valid` 和 `prepared.error`，分发之后检查 `prepared.consumed`。继续持有这个 Python 对象并不能让它重新可用。要再试一次，请重新构造并准备一个新请求。

Python 子类调用 `Provider(family)`，并实现 `get_name()` 和 `prepare(request)`。子类可以把准备工作委托给某个 `SchemaProvider`，并返回它产生的真实的已准备句柄。子类无法凭空构造一个成功的 SDK outcome，也不能从 JSON 导入分发权限。通用的 `invoke` 和 `dispatch` 走原生的生命周期；重写旧的 `complete` 方法并不能实现这套生命周期。

原生执行调用 Python 重写的 `prepare` 时，如果返回 `None`，会在句柄被消耗之前抛出 `TypeError`。请返回真实的 `PreparedProviderRequest`。

`NodeContext(provider=provider)` 以及对 `ctx.provider` 的赋值，会把原来的 Python provider 保存在原生的共享所有者租约（shared owner lease）中。原生 context、已编译节点和引擎的各个副本都持有这份租约，即使之后 `ctx.provider` 被重新赋值，或者外部的 Python 引用都被回收了，它们调用的仍然是同一个对象重写的方法。重新赋值只会释放这个可变 context 自己的租约，影响的是之后的编译，已有的引擎不受影响。这保留的是对象的身份和生命周期，并不会冻结 provider 的可变状态。最后一份租约会在持有 GIL 时释放 Python 所有者。

已移除的 `CompletionParams`、`ChatCompletion`、`OpenAIProvider` 和 `RateLimitedProvider` 没有保留兼容别名。请构造经过校验的 `SchemaProvider`，并使用请求工厂。`ChatMessage` 和 `ToolCall` 仍是方便图使用的值，与 SDK 完整的 `ProviderMessage` 和 `ProviderToolCall` 是两回事。

### 结果、失败与用量

`ProviderOutcome` 保存的是一份不可变的、持有所有权的 SDK 结果。检查 `outcome.completion` 或 `outcome.failure` 即可，不存在的那个分支为 `None`。completion 视图和 failure 视图都保留完整的有序消息、用量、尝试（attempt）证据、停止/错误信息，以及保留下来的 wire 证据。失败时可能保留部分输出。报告失败时要保留这些证据，不要把部分文本当成成功结果返回。

`ProviderCompletion.wire_envelope` 和 `ProviderPartialCompletion.wire_envelope` 是可为空的、各请求族专属的证据。读取之前先检查 `wire_envelope is not None`。当前带缓冲的 Chat 解码器会让它保持 `None`，并把完整的响应 JSON 作为 `ProviderRawWire`（SDK 中的 `RawWire`）保存在 `raw_events` 里，其 `type == "chat.completion"`，文档内容在 `payload` 中。请检查实际的带类型 raw event，不要假定一定有 envelope。raw event 不会把失败变成成功；envelope 缺失也不代表保留的响应丢了。SDK 不会兜底去合成 envelope。

请保留持有所有权的 outcome，或它保留下来的带类型视图：其中真实的消息、native 所有者和可用的 wire 文档，在调用结束、provider 被回收之后仍然有效。原始 payload 保留了 provider 私有的字段，而 native tracing 不包含原始 envelope/事件，也不包含 native replay 和 reasoning。Python 的 JSON 视图是数据副本，不具备 native replay 或资金方面的权限。

要使用完整的带类型 messages/parts；需要比较的语义是逻辑上的 role 和文本时，就比较这些。检查点和 Chat 请求中的文本内容，合法的形式既可以是文本字符串，也可以是带类型的文本 part 数组；调用方不应硬性要求其中某一种碰巧出现的序列化形状。续接时，要使用完整的 parts 和真实的 native 所有权，不要用压平的文本或 JSON 去替代。

`ProviderMessage.parts`、completion 与 partial 的 `messages` 和 `raw_events`，以及 usage 的 `extra`/`conflicts`，返回的都是分离的列表或映射，连同其中绑定的值。保留下来的 parts，在原来的集合被替换之后仍可放心检查；修改副本不会改写不可变的 outcome。要编辑一条消息，先 `parts = message.parts`，修改 `parts`，再赋回 `message.parts = parts`。`message.parts.append(...)` 只会改动那个临时的 Python 列表。

SDK 的失败是作为数据返回的。宿主的 observer 或预算结算失败时，会抛出 `ProviderObserverError` 或 `ProviderBudgetSettlementError`（均派生自 `ProviderOutcomeError`），它们保留真实的 outcome 和 cause。因此，回调失败并不意味着可以重新分发。

对已保存的 Python provider 或图异常的 cause 反复检查，得到的始终是原来的异常对象和 traceback，经由原生嵌套异常转换才拿到的 cause 也是如此。

用量计数器是带 `value` 和 `evidence` 的 `UsageCount`，未知时为 `None`。上报为零是已知的用量，与没有上报计数器不同。读取 `count.value` 之前，先检查 `count is not None`。核算时不要写 `count or 0`，也不要把未知的 input/output/total 计数器替换成 0。

`outcome.usage.provider_cost` 是不可变的 `ProviderReportedCost` 视图。它的可选 `ProviderUsdAmount` 字段有 `total`、`upstream_total`、`upstream_input` 和 `upstream_output`，每个字段带 `nano_usd`、`evidence` 和 `rounding`。`status` 是按这个顺序排列的四项数组；`byok_status` 描述可为空的 `is_byok`。`ProviderCostStatus`、`ProviderCostSource` 和 `ProviderCostRounding` 保留了缺失、可用、格式错误、精度超限、溢出、币种未知和冲突这几类证据。金额是对解析后的 binary64 向上取整得到的，而不是依据原始的十进制字面量。上报为零或 `is_byok=False`，都不等于缺失。这些观测既不是发票，也不是 token 预算的计费；要查看每次调用的金额证据，请检查有序的 `provider_outcomes`，而不是聚合的 token bank。

`UsageAccumulator` 是只统计 token 的聚合。只有一份报告时，它保留这份报告的金额元数据；收到第二份报告后，快照的 `provider_cost` 会被明确重置为规范的 Missing/None，既不是过期的首次调用成本，也不是两者之和。恢复这份快照，不会重建成本，也不会续期 token 权限。要读取确切的金额证据，请到每个原始 outcome 或 provider 的 journal 条目里去找。

恢复运行或续接时，`RunResult.provider_outcomes` 先按顺序保留原来的 outcome，后面接着新产生的 outcome。`RunResult.usage` 反映的是当前生效的核算 bank，它可以从检查点恢复之前的报告；所以，没有新的 provider 调用的恢复，也不保证 usage 为 `None` 或零。恢复证据时，既不能重新分发原来的 provider 请求，也不能重复计费。保留下来的报告描述的是之前的调用，不会授予新的消费额度。

### Native 历史与可移植导出

返回的 `ProviderMessage` 带有完整的带类型 parts 和真实的 native replay 状态。它们的 native replay 和 wire 输出是只读的。要直接续接 provider 的对话，需要保留原请求完整的 `request.messages` 前缀，再追加 `outcome.messages`；后者包含的是返回的输出，而不是完整的输入历史。用第一次调用示例中的 `request` 和 `outcome`，构造 `history = request.messages + outcome.messages`，再用 `ng.make_provider_request(provider, "example-model", history, controls=controls)` 构造下一个请求。这一步构造本身不会发送任何东西；任何下一轮请求还必须带上新的用户消息，或者所需的工具结果。

真实的 `NativeContext` replay 除了检查返回的 assistant 消息，还会检查原始前缀。只传 assistant 输出，会在 wire 分发之前以 `ReplayIneligible` 失败。从 `NativeArchive` 加载这条 assistant 消息，会保留它的托管和前缀绑定；但仍然需要原始的完整前缀。

在图里，`RunConfig.provider_messages` 接受完整的历史，`RunResult.native_messages` 已经包含捕获到的完整历史。直接把这份图的结果当作历史使用，不要再在前面拼接原始输入。`RunResult.provider_outcomes` 给出保留下来的 outcome；在节点内部，`RunContext.provider_outcomes` 和 `provider_loop_history` 保留 provider 的证据。

`RunConfig.provider_messages`、`RunResult.native_messages` 和 `ProviderLoopEntry.messages` 返回的同样是分离的历史副本。要更新可变的输入历史，先改返回的列表，再赋回 `config.provider_messages`。保留下来的消息和 parts，在输入被替换之后仍然有效；结果和 loop-history 的属性依旧是只读的。

在自定义节点里，用 `provider_messages_write(messages_or_outcome)`，可以让 native 历史保留在该节点的通道写入中。`portable_message(ChatMessage)` 导入可移植的内容，并拒绝导入 native reasoning；`project_message(ProviderMessage)` 生成仅供观察的图投影。

要持久化 native 历史，用 `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)`，它返回一个 archive 或 `ProviderError`。它的 `save(messages, binding="")` 返回引用或错误；`load(reference, binding="")` 返回带类型的消息或错误。这用的是 SDK 经过认证的本地托管，以及一份独立的宿主密钥。它不会让可移植的 JSON 变成 replay 或受管预算（managed-budget）的权限。

可移植的记录仍然可以用 `RuntimeHistoryRecord.serialize_canonical()`。要持久化含有真实 native 消息的记录，用 `record.serialize_canonical(archive, owner_id)`，并传入一个真实的 `NativeArchive`，它的 `owner_scope` 要与 `owner_id` 一致。用 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")` 恢复；可移植记录用默认参数即可，但 native 的 archive 引用必须配上同一个 owner 作用域下的 archive。无参数的序列化，或者导入的 JSON，都无法重建 native 权限。Python 的 parse 和支持 archive 的序列化调用，在执行原生工作（包括 archive I/O）期间会释放 GIL。

`RuntimeHistoryRecord.message` 返回分离的、带类型的副本，其中保留着真实的共享 native 所有者；修改这个副本，不会改写不可变的 RAW 记录的身份。`ContextStore.hydrate_records(range)` 返回带类型的记录，`history_record_by_message_id(feed, message_id)` 返回一条记录或 `None`。`InMemoryContextStore` 和 `SQLiteContextStore` 继承了这些方法。`SQLiteContextStore(database_path, archive=None)` 接受一个真实的 archive 来做 native 托管；没有 archive 的存储，在需要托管时会拒绝 native 历史。构造和带类型的检索都会释放 GIL。`LocalProgramHost` 接受最后一个可选关键字参数 `native_history_archive=None`，并把它转发给真正的 Program 运行时；它提供的是托管，不是额外的 grant，也不是持久的 Program 存储后端。

可移植的状态字典和 JSON 导出描述的是消息数据。它们无法重建 native replay 状态、provider 的 origin 或受管预算的权限。导出的 JSON 中的 `native` 标志只是描述性的，不是授权令牌。经由 `ChatMessage`、扁平的文本通道或 JSON 转换，可能丢掉 provider 专属的 parts；如果需要这些 parts，就保留带原始前缀的完整带类型历史。

## Core 图快速上手

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

### 编译时的工具所有权

把 Python 的 `Tool` 实例、原生 C++ 工具和 `MCPClient.get_tools()` 的结果，一起作为 `ng.NodeContext(tools=[...])` 传入。编译时会在**构造图节点之前**，把这个列表快照成一个持有所有权的原生 `ToolSet`。编译好的引擎在 `run()` 和 `resume()` 中始终使用这批确切的工具，即使之后替换了 context 的 `tools` 列表，或者放掉了外部引用，也不受影响。之后再改这个列表，只会影响下一次编译。原生 MCP 工具保留异步执行路径；编译之后没有所有权转移，每次分发也不会再分配所有权。

### stderr 上的 lint 提示

用 `GraphEngine.compile` 构建 `schema_version` 为 1 的图时，校验警告会输出到 stderr，例如 `[neograph] lint [E7] $: node(s) unreachable from __start__ ...`。`GraphEngine.compile(..., stderr_notices=False)` 会同时关闭这些警告和 fan-out 串行执行的一次性提示。它是 C++ `EngineConfig::stderr_notices` 在 Python 中的对应开关，默认值为 `True`；校验错误仍然会抛出。

只由 `Send` 或 `Command.goto` 启动的节点会被报告为不可达，因为图定义里没有声明动态目标的地方，此时这条提示是预期内的。不要为了消除它而加边：指向 Send 目标的静态边会让目标带着发送方的状态再多运行一次（见 [Send](concepts.md#5-send--dynamic-fan-out)）。

## Core API 对等性

Python 暴露的是 C++ 的执行能力，而不是另外一套 Python 调度器：

- 同步和 asyncio 的 run/stream/resume；
- 精确检查点的 `resume_from`、fork、状态检查和有序的状态写入；
- 通过图的 interrupt 和 `NodeInterrupt` 实现静态和动态的 HITL；
- `RunMetadata` 的截止时间、trace/run 标识和模型 token 上限；
- 图级别和单个节点的 `RetryPolicy`，包括 jitter；
- 仅在单次执行内有效，或显式声明可复用的 `CacheScope`；
- 检查点和长期 Store 后端；
- 自定义节点、reducer、条件、provider 和工具；
- 工具闸门、执行策略、强制的生命周期 Hook，以及严格的运行时拦截（interposition）。

### 对等性契约

“对等”的意思是：Python 走的是同一条原生执行路径，遵守同样的安全契约；并不是说 C++ 内部的每个存储类型或权限类型都被复制到了 Python。

| 能力 | 原生 C++ 路径 | Python 接口 | 状态 |
|---|---|---|---|
| 编译并执行 Core 图 | `GraphEngine` | `GraphEngine.compile`、run/stream/async 方法 | 调度器和运行时相同 |
| 运行时标识、截止时间和预算 | `RunMetadata`、`RunConfig` | `RunMetadata`、`RunConfig.model_token_budget` | 每次运行的取值相同 |
| 重试与节点缓存策略 | `RetryPolicy`、`CacheScope` | 图/节点的 setter 和缓存作用域 | 运行时策略相同 |
| 检查点、HITL 与时间旅行 | 检查点存储和 resume API | resume、精确的 `resume_from`、fork、状态历史/更新 | 检查点契约相同 |
| Program 的编写与本地执行 | 编译器、Catalog 和 `ProgramRuntime` | `ProgramCompiler`、`LocalProgramHost`、句柄/结果 | 原生的、按 owner 划分作用域的便捷宿主 |
| 强制的生命周期 Hook | 注册表、runner 和 `HookRuntime` | 定义，加上 `create_hook_runtime` 回调 | 生命周期边界相同，均为失败时关闭 |
| 运行时上下文与严格分发 | 上下文存储、回执（receipt）和拦截 | 对应的不可变值、存储和 `StrictRuntimeProfile` | 同一个原生控制器 |
| wheel 默认的持久化 | SQLite 的 Core/上下文/分发存储 | `_HAVE_SQLITE` 导出项 | PyPI wheel 中已启用 |

原始的 `ProgramCatalog`、转换存储（transition store）、替换/迁移控制器、synthesis 网关、Hook journal 和 RPC 执行器，仍然是供宿主自行组合的 API。只暴露这些带权限路径的片段，会绕过必需的 `proposal -> compile -> admit -> publish -> migrate/spawn` 协议。`_HAVE_PROGRAM` 并不声称与原始控制面的管理能力对等。

### 运行时覆盖重试策略

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

图定义中的 `"retry_policy"` 仍是声明式的默认值。运行时的 setter 是另一套 C++/Python 配置接口。

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

在 Python 节点内，通过 `input.ctx.trace_id`、`run_id`、`has_deadline`、`deadline_remaining_ms` 和 `model_token_budget` 可以拿到同样的值。

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)` 默认没有截止时间。传入的超时值和 `metadata.set_timeout_ms(timeout)` 接受非负整数毫秒，且不能超出 steady clock 剩余的范围；校验发生在转换为有符号时长、以及加上截止时间之前。负数或过大的整数会抛出 `OverflowError` 或 `ValueError`；setter 失败时，原来的截止时间保持不变。设为 0 表示截止时间立即到达。要去掉截止时间，用 `metadata.clear_deadline()`。

### 缓存作用域

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` 是一个显式的声明，表明该节点与租户、provider、Store、工具、凭据、时间和恢复状态都无关。

## Program 与 QuickJS

Python wheel 构建了 `neograph::program` 和受限的 QuickJS 前端。用 Python 定义的节点可以加入不可变的 Program 注册表，并通过原生的 `ProgramRuntime` 执行。

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

`ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer` 和 `add_registered_condition`，会复制一个已经在本进程中注册过的可执行对象，注册方式是 `@ng.node` 或 `ng.NodeFactory.register_type`、`ng.ReducerRegistry.register_reducer`、`ng.ConditionRegistry.register_condition`。传入未注册的名称时，它们都会抛出 `ng.ProgramRegistryError`，错误信息里会指明应该调用哪个注册函数。该错误的 `kind` 是 `"node"`、`"reducer"` 或 `"condition"`，`name` 是缺失的名称。它同时继承 `ValueError` 和 `RuntimeError`，所以针对其中任一个编写的处理代码，仍然能捕获它。如果 Program 用到的节点类型、reducer 或条件不在注册表快照里，则改为在编译时失败：`ProgramCompiler.compile` 和 `LocalProgramHost.compile_admit` 都是如此，抛出 `ng.ProgramCompileError`，诊断码为 `P_CORE_GC_REGISTRY_MISS`。

每个诊断字典都有一个 `help` 条目：对于编译返回的诊断，它是一个字典，包含 `doc`（它在[诊断指南](diagnostics.md)中的条目）、`related`、`suggestions` 和 `candidates`；对于已封存的 `bundle.diagnostics`，它是 `None`。异常则始终带有它。`ProgramCompiler(..., echo_routes=True)` 还会返回一条 `P_CORE_ROUTES` 提示，附带路由后继表。在 `ProgramCompiler` 或 `LocalProgramHost` 上设置 `soundness_check=True`，会运行[健全性检查](program-graphs.md#soundness-check)：产生 `P_CORE_S1` 到 `P_CORE_S5` 的警告（带有 witness trace），以及一条 `P_CORE_SOUNDNESS` 汇总。这些内容都不会被封存。

`LocalProgramHost` 是按 owner 划分作用域的内存便捷宿主，用的仍然是 C++ 的编译器、Catalog、准入策略、转换存储和 ProgramRuntime。生成出来的 proposal，在准入之前还应通过宿主的语义验证器；参见 [DSL 能力评测](DSL_CAPABILITY_EVAL.md)。

销毁 `LocalProgramHost` 时，调用方持有的 GIL 会被释放，同时它内部真正的 `ProgramRuntime` 会取消、排空并 join 调度器的工作，这样正在运行的 Python 节点可以执行完。之后它重新获取 GIL，再销毁宿主的其余成员；这些成员中的 Python 回调和对象所有者，仍能保证在持有 GIL 的情况下安全销毁。

原生的 Program 录制执行 API 是 `ProgramRuntime::replay_recorded`。`LocalProgramHost` 提供 `run`、`start`、`resume` 和 `fork`，不暴露原始的 recorded-binding 控制面。Program 的结果除了原有的句柄、预算和权限字段，还保留原生运行时给出的带类型的 provider outcome 和失败证据。

`ProgramResult.failure` 是只读的 `ProgramFailure` 值或 `None`，不是字典。它的 `provider_outcome` 和 `provider_cause` 保留 provider 的失败证据；`code`、`message`、`operation_id`、`core_node`、`attempts` 和 `witness` 描述失败的那个操作。结果字段还包括 `bundle_id`、`operation_id`、`attempt`、`checkpoint`、`interrupt` 和 `provider_budget_authority`。在没有 `native_history_archive` 的宿主（默认的内存宿主就是如此）上，`provider_outcome` 持有去掉 native 状态的消息，`witness["provider_outcome_native_retained"]` 为 `False`，原始 outcome 仍留在 `provider_cause`。

已安装的 JavaScript 词汇表的确切内容，可以作为字典取得：

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### 模块结构与 `callCore`

JavaScript Program 必须导出 `define()`，它返回一个图构建器。这张图就是该 Program 唯一的 Core。没有 `define()` 时，编译会抛出带 `P_JS_DEFINE_MISSING` 的 `ProgramCompileError`，所以只导出 `main()` 的模块会被拒绝。`main()` 是可选的；没有它时，运行会直接执行这个 Core。

`ng.callCore(name, input, sourceSite)` 必须用传给 `ng.graph(...)` 的名称来指定这个 Core，上面的例子里是 `"main"`。用其他任何名称，运行都会以 `P_JS_CONTROL_COMMAND` 失败。要执行不同的步骤，把它们放进这张图里，再用不同的输入调用它。每个节点类型都要先用 `@ng.node` 或 `ng.NodeFactory.register_type` 注册，然后才能传给 `add_registered_node`；对未注册的类型，它会抛出 `ng.ProgramRegistryError`。

`callCore` 完成后，会把 Core 的输出对象返回给 `main()`。上面的快速上手示例原样把它作为 `result.output` 返回：

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels` 保存每个通道的 `value` 和 `version`。`version` 是所有通道共用的写入计数器在该通道最后一次写入时的值，`global_version` 是运行最后一次写入之后的计数器。它们只表示写入的先后顺序，既不是通道收到的写入次数，也不是超步序号；从未写入过的通道，版本为 0（参见[版本](concepts.md#versions)）。每个通道的值还会复制到一个以通道名命名的顶层键下，除非这个键已经存在，所以 `main()` 可以直接读 `result.value`；名为 `channels` 或 `global_version` 的通道，只能通过 `channels` 访问。屏障列表是排好序保存的：`graph.barrier("join", ["zeta", "alpha"])` 在 `bundle.sealed_core_definitions` 中显示为 `{"wait_for": ["alpha", "zeta"]}`，传入的顺序没有影响。

### 命令失败会结束运行

只有已完成的命令才会让 `main()` 继续执行。如果 yield 出去的命令没有完成，运行就会终止，例如：`callCore` 中的节点抛出异常、用完了 Core 的步数预算、停在一个再也无法触发的屏障上，或者超出了挂钟时间限制。此时 `ProgramRuntime` 会记录这一结果，把它作为运行的最终结果，并带上对应的 `status` 和 `failure`。生成器不会被恢复：包在 `yield` 外面的 `try`/`catch` 永远不会执行，之后的 JavaScript 也不会再运行。join（`ng.all`、`ng.race`、`ng.quorum`）如果按其失败策略判定为失败，运行同样以这种方式结束。请在 Python 中通过 `result.status` 和 `result.failure` 处理失败。被中断的命令则不同：运行以 `Interrupted` 状态停止，`resume()` 会继续那条命令。join 的某个成员发生中断，会让整个 join 暂停：join 不再启动新的成员，已经在运行的成员则让它们跑完。如果这些成员的结果已经能决定 join 的结局（fail-fast 的失败、race 的胜出者、已满足或已不可能满足的 quorum），join 就按该结局结束。否则运行以 `Interrupted` 停在第一个暂停的成员上，`resume()` 会继续这个成员，并从命令 journal 重放已经完成的成员，不会再次运行它们，也不会再次计费。`ng.await(command, timeout_ms)` 的截止时间是绝对的：命令 journal 记录了 await 的起始时间，所以 resume 或者进程停止后的恢复，只剩下尚未用完的那部分时间。如果恢复时已经过了截止时间，await 会立刻以 `TimedOut` 和 `P_AWAIT_TIMEOUT` 结束运行，不会运行被回应的节点（#391）。计划级的 `await` 操作，同样在 resume 前后保持它的截止时间。因不确定的副作用而暂停的成员（即 payload 的 `kind` 为 `tool_effect_reconciliation` 的 `NodeInterrupt`），优先级高于成员的失败，这与 Core 的步骤一致（#392）：join 会停在这个成员上，而不是直接失败；失败的成员被记录下来，等到恢复后，join 再以这个失败结束；如果已有成功的结果决定了 join，则按那个结局结束（#397）。恢复时，被回应所继续的那个成员先运行到结束，已记录的失败才结束 join，这样回应能送达该成员，它的副作用也能有定论（#400）。无论 join 因何暂停，它总是先停在因不确定副作用而暂停的成员上，与声明顺序无关（#401）。收集式的 `ng.all`（`collect: true`）是这条优先级规则有意设置的例外：它要等所有成员（包括暂停的成员）结束之后才报告，所以只要另一个成员因普通问题而暂停，某个成员的失败就不会让它结束；运行以 `Interrupted` 停在那个成员上，等所有成员都有了结果，再报告这个失败（#401）。

### 图检查与屏障停滞

编译时会沿着调度器实际会走的路由来检查 Core 图：带条件边的节点，只会走它的第一条条件边。JavaScript Program 是严格的文档，所以下面每一种情况都会抛出 `ng.ProgramCompileError`：

- `P_JS_GRAPH_ARGUMENT`：`.node()` 调用中把节点命名为 `__start__` 或 `__end__`；
- `P_JS_GRAPH_ENTRY`：`define()` 返回的图有节点，却没有 `.entry()`；
- `P_CORE_E13`：某个节点既有条件边，又有静态出边；这条静态边永远不会触发；
- `P_CORE_E14`：同一个节点有第二条条件边；它永远不会参与路由。

`P_CORE_E15` 是 `bundle.compile_diagnostics` 中的一条警告：屏障的某个成员有一条路由，通向的既不是 join 也不是 `__end__`，走这条路由会让 join 停滞。循环是合法的；如果要拒绝所有循环（例如计划和检查清单），给 `ProgramCompiler` 或 `LocalProgramHost` 传 `require_acyclic=True`，循环会以 `P_CORE_E16` 被拒绝。

屏障在它的 `wait_for` 列表中的每个节点都路由到它之后才会触发。通过别的途径到达 join 的节点（例如挂在某个成员失败路由上的修复节点），不能代替那个成员。运行已经无事可做、join 手里却仍有信号时，运行不会算作完成：它以 `Failed` 结束，错误码为 `P_CORE_BARRIER_STALLED`，`failure.core_node` 给出该 join 的名称，`failure.witness["stalled_barriers"]` 列出每个停滞的 join，以及它的 `wait_for`、`received` 和 `missing` 节点。只有在最后一个超步里显式路由到 `__end__`，这样的运行才会正常结束：指向 `__end__` 的静态边（`.exit()` 写出的就是这种）、指向 `__end__` 的条件路由，或者 `Command.goto("__end__")`。没有后继的节点不算，在更早的超步里显式结束也不算。`GraphEngine` 在同样的状态下返回 `ng.RunStatus.BarrierStalled`，`result.output["_neograph"]["barrier_stalled"]` 里有同样的详细信息。

### 错误与诊断

在 Python 中抛出的 Program 错误是带类型的，并保留各自的诊断：

| 异常 | 抛出位置 | 父类 |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`、`LocalProgramHost.compile` 和 `compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit` 和 `compile_admit`，例如 `P_ADMIT_POLICY` 或 `P_ADMIT_PROVIDER` | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`、`start`、`start_active` 和 `resume`，例如 `P_START_BUDGET` 或 `P_RUN_NOT_FOUND` | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer` 和 `add_registered_condition` | `ValueError`、`RuntimeError` |

前三个异常的消息分别以 `Program compilation failed`、`Program admission failed` 或 `Program operation failed` 开头，后面每条诊断占一行：包含它的错误码、JSON pointer 和消息；有源码位置时，还带上 `(source:line:col)`。`.diagnostics` 是字典的列表，每个字典有 `code`、`severity`、`phase`、`message`、`source_id`、`json_pointer`、`span` 和 `witness`。

编译成功的 bundle 带有两个列表。`bundle.compile_diagnostics` 是编译器返回的内容：按当前规则产生的警告（JavaScript 调用位置在 `span` 里），以及 `P_JS_CONTROL_CHARACTER` 之类的编写警告。应该把这个列表展示给作者。`bundle.diagnostics` 是封存进 bundle 的那一份副本，并由 `bundle.id` 覆盖。它保留的是 bundle 最初封存时的规则和措辞，这样更新的编译器就不会改变 bundle id；因此它可能与前者不同：它没有 span，而且可能仍然列出当前规则已经去掉的警告，比如只有 Python 节点写入的通道所对应的 `P_CORE_E6`。编译器和 `LocalProgramHost` 都不会向 stderr 打印 lint 信息。

<a id="model-calling-nodes"></a>
### 调用模型的节点

`LocalProgramHost` 会给它所准入的 Program 中内置的 `llm_call` 节点绑定一个 provider。注册表按名称声明 provider，以及需要它的节点；宿主则提供 provider 对象、模型和请求控制项，并带上一个不含机密的 `binding_label`：

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

`add_provider(name, semantic_version, implementation_digest)` 注册 provider 的身份；`add_registered_node("llm_call", ..., provider=name)` 注册内置节点，并把该身份作为静态要求。`provider=` 只能绑定 `llm_call`，用于其他类型或未知的 provider 时抛出 `ValueError`。无论描述符的 id 是什么，宿主都会把它的 `node_context` provider 绑定到这个名称上。`NodeContext(provider_controls=...)` 接受节点要发送的带类型 `ProviderControls`，例如 `max_output_tokens`。`node_context` 需要一个 provider 和 `binding_label`，不需要工具。

在这个绑定里，只有 label 会进入任何身份标识。需要该 provider 的 Program 版本，带有一份 capability receipt，其身份是 label 与 provider 的注册表身份合起来的摘要；provider 的密钥、endpoint 和模型都不在其中，所以换模型不会改变版本 id，换 label 才会。不需要 provider 的 Program 没有 receipt，bundle id 和版本 id 都不变。label 必须由 1 到 64 个字符组成，字符取自 `a-z`、`0-9`、`.`、`_`、`-` 和 `:`，以字母或数字开头，并且看起来不能像 API key。没有 `node_context` 时，需要该 provider 的 Program 会在准入时以 `P_ADMIT_PROVIDER` 被拒绝；含有未带 `provider=` 注册的 `llm_call` 或 `intent_classifier` 节点的 Program 也一样。

每次调用都按编译进 NeoGraph 的价格目录计价，并计入运行的 `monetary_microunits`。请求发出之前，会先预留这次调用的上限。在 OpenRouter 上，请求还会把这个上限作为 `provider.max_price` 带上：取路由上限（route ceiling）的输入和输出费率，或者控制项或 `SchemaProviderDefaults` 中已设置的、更低的 `max_price`，这样 OpenRouter 就不能把调用路由到更贵的 endpoint。计价时会准备（但绝不发送）一个探测请求，其对话只有一条用户消息，所以 Python provider 的 `prepare` 能看到它。无法确定上限的调用（目录里没有该模型、路由别名没有路由上限、请求没有输出上限），或者预留额度超出剩余预算的调用，会在发出任何东西之前被拒绝，运行以 `P_MONEY_*` 错误码失败。与 `ProgramRunBudget` 的每个字段一样，`monetary_microunits = 0` 表示什么都不授予，所以这样的运行会拒绝每一次调用。收到回复后，`result.monetary_records` 为每次调用保存一条 `ng.ProgramMonetaryRecord`，带有它的 `evidence`：`reported`（OpenRouter 的 `usage.cost`）、`estimated`（按目录费率估算；`catalog_version` 和 `catalog_digest` 指明是哪个目录）或 `held`（outcome 或用量未知，因此按预留额度计费）。provider 以已完成的 HTTP 4xx 响应（408、425、429 除外，例如密钥无效或请求格式错误）拒绝的调用，以及从未发出的调用，会释放预留、不留记录，也不收取金额和模型 token。429、5xx 响应和被切断的调用仍为 `held`。`result.usage.monetary_microunits` 是它们的总和。错误码、上限和结算规则见[定价证据与价格目录](reference-en.md#pricing-evidence-and-the-price-catalog)。

调用模型的 Python 节点自己持有 provider；Program 的预算看不到这样的调用。

### 子 Program

`ng.spawn` 需要子绑定解析器（child-binding resolver），而 `LocalProgramHost` 没有配置。因此从 Python 调用的每个 `ng.spawn` 都会让运行以 `P_CHILD_BINDING`（“Program spawn has no configured child binding resolver”）失败。使用子 Program，需要一个设置了 `RuntimeConfig::child_binding_resolver` 的 C++ 宿主，它把每个绑定名映射到 `link_module_child` 产生的 `ModuleLinkReceipt`，以及已准入的子 `ProgramVersion`。子 Program 的生命周期见[递归智能体](PROGRAM_RECURSIVE_HARNESSES.md)。

### 预算

传给 `LocalProgramHost` 的 `budget_ceiling` 会成为准入策略的上限。除了 `max_child_depth` 和 `max_total_children`，其余每个字段都必须为正数，包括 `model_tokens`、`monetary_microunits` 和 `max_dynamic_compiles`，即使 Program 从不调用模型也是如此。否则构造函数会抛出 `ValueError`，并指出为零的字段。这两个与子 Program 相关的字段必须同为零，或同为正数。

`compile_admit(source, budget)` 和 `compile(source, budget)` 会把 `budget` 封存进 bundle，作为每个字段的最小值，同时也是最大值；封存的预算超过上限的 bundle，准入时会被拒绝。针对该版本的每次 `run`、`start` 和 `start_active` 调用，都必须逐个字段传入相同的值。预算比封存值大或小，都会抛出带 `P_START_BUDGET` 的 `ng.ProgramDiagnosticError`，消息会指出第一个不一致的字段，例如 `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`。想用不同的预算，需要重新编译并准入，得到的是一个新的 `ProgramVersion`。运行预算要求 `wall_time_ms`、`max_concurrency`、`max_program_operations` 和 `max_core_steps` 为正数；`model_tokens` 和 `monetary_microunits` 可以为零。零表示什么都不授予：`model_tokens = 0` 时，用到模型 token 的运行会以 `BudgetExhausted` 结束；`monetary_microunits = 0` 时，每一次需要计价的 provider 调用都会在发出之前被拒绝（参见[调用模型的节点](#model-calling-nodes)）。模型 token 预算使运行停下时，无论是模型的响应超出了预算，还是下一次调用在发送前被拒绝，`status` 都是 `BudgetExhausted`，`failure.code` 都是 `NG.BudgetExhausted`；`failure.witness["model_tokens"]` 含有 `granted`、`used` 和 `over_by`。其他预算也以同样的形式结束运行，运行在超出额度后才结束时也一样：金额以 `P_MONEY_BUDGET` 和 `failure.witness["monetary"]` 结束，Core 步骤以 `P_CORE_STEP_BUDGET` 和 `failure.witness["core_steps"]` 结束，wall time 则以 `TimedOut`、`P_RUNTIME_TIMEOUT` 和 `failure.witness["wall_time_ms"]` 结束。超出多项预算的运行会把每一项都列在 witness 里，status 和错误码取自 wall time、模型 token、金额、Core 步骤中排在最前的那一项；参见 [Program 指南中的预算](program-guide.md#budgets)。

### 失败路由

带有 `on_failure` 的 `retryPolicy(node, {...})`，会把节点最终的失败路由给一个处理器，而不是让运行失败；规则见 [Program 图功能](program-graphs.md#failure-routing)。Python 节点用异常的属性来说明它的失败：字符串 `code`（作者自定义的错误码，例如 `app.compile_error`）、布尔值 `retryable` 和字符串 `public_message`。`str(exc)` 永远不会进入记录。节点抛出 `ng.ProgramDiagnosticError`、`ng.ProgramCompileError` 或 `ng.ProgramAdmissionError` 时，报告的是配置类的结论（`NG.ConfigError`），这类结论既不会重试，也不会被路由。处理器带有 `interruptBefore` 时，运行以 `Interrupted` 停止，`result.interrupt.pending_input.core_interrupt_value` 里是 `{"reason": "failure_route", "failure": record}`；`host.resume(run_id, value)` 会在该处理器处继续。失败出口（failure exit）让运行以其声明的错误码 `Failed` 结束；没有失败边的节点失败时，运行以归类后的错误码（`NG.TaskFailed`、作者自定义的错误码等）`Failed` 结束。

`ProgramRunBudget.max_node_retries`（默认 `None`，即没有上限）限制整个运行中节点重试的总次数；`result.usage.node_retries` 统计已重试的次数，`result.remaining_budget.max_node_retries` 是剩余次数。`compile_admit` 不会封存它，所以运行时可以直接设置，不必重新编译。`LocalProgramHost(..., failure_text_redactor=fn)` 和 `GraphEngine.compile(..., failure_text_redactor=fn)` 接收一个脱敏函数，用于设置了 `error_text` 的节点：`fn` 收到 `{"node", "code", "kind", "attempts", "retryable", "public_message"}`，返回要保留的文本，或 `None`。没有这个函数时，不会写入任何文本。在 `GraphEngine` 上，失败出口之后 `result.status` 是 `ng.RunStatus.FailureExit`，`engine.resolved_retry_policy(node)` 返回节点实际使用的重试时间参数。

`retryPolicy(node, {attempt_timeout_ms: ...})` 限制节点每次尝试的时长（100 到 3 600 000 ms）。尝试超时后，`input.ctx.cancel_token.is_cancelled()` 会变成 true，该次尝试以 `NG.Timeout` 失败，可以和其他失败一样被重试和路由；它之后返回的内容一律丢弃。Python 函数体无法被抢占，所以会 sleep 或阻塞的节点应当轮询这个 token：只有超时的那次尝试返回之后，下一次尝试才会开始；如果某次尝试在它的停止期限（同样是这个超时，但至少 1 秒）内仍未返回，运行就以 `NG.EffectUncertain` 失败。这样的函数体会继续在后台运行，解释器退出时，binding 会等它结束。如果退避等待会越过运行的 `wall_time_ms`，则停止重试，原因类别（kind）为 `deadline`。在 `GraphEngine` 上，`engine.set_node_attempt_timeout_ms(node, ms)` 设置宿主一侧的超时（`None` 表示清除；定义自带的超时只能被缩短），`engine.resolved_attempt_timeout_ms(node)` 返回节点实际使用的超时。参见 [Program 图功能](program-graphs.md#attempt-timeouts-and-the-deadline)。

### 调度线程、worker 与超时

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)` 有两个线程设置。`scheduler_threads` 设定 `ProgramRuntime` 调度线程池中的线程数，这个线程池驱动运行：JavaScript 控制、`callCore` 分发、join 和截止时间；它至少为 1。`worker_count` 设定每个 Core 引擎的 worker 线程池，用于同一张图内部的 fan-out，且不能超过已准入 Program 的 `max_concurrency`；取默认值 1 时，fan-out 的各分支在调度线程上运行。

在 Program 运行期间，Python 节点的函数体不占用调度线程。它运行在 binding 自己持有的 worker 线程池上，默认 32 个线程；要修改，在第一次运行 Program 之前，把 `NEOGRAPH_PYTHON_NODE_THREADS` 设为 1 到 1024 之间的值。即使调度线程保持默认的 1 个，宿主的多个运行、`ng.all`、`ng.race` 和 `ng.quorum` 的各成员，以及同一个 Core 运行的 fan-out 分支，也会同时推进，而且节点运行期间截止时间照样会触发。`ng.race` 返回最先完成的成员，`ng.quorum` 在凑够所需的成功数后返回。Python 代码仍然在 GIL 下运行，所以 CPU 密集的节点不会并行；只有节点释放 GIL 时（例如在 `time.sleep`、I/O 或原生调用中），它们才会重叠。普通的 `GraphEngine` 运行仍然在调用线程上执行 Python 节点函数体。

这两个限制都不会打断正在运行的 Python 节点；取消是协作式的。运行的 `wall_time_ms` 用完时，运行时会取消该运行，等正在运行的节点返回后才给出结果，其中 `status` 为 `TimedOut`，`failure.code` 为 `P_RUNTIME_TIMEOUT`，消息为 `Program wall-time budget (N ms) expired`。`failure.witness["wall_time_ms"]` 含有 `granted`、`used` 和 `over_by`；被超时打断的那个操作的文本，在 `failure.witness["cause_message"]` 里。`ng.await(command, timeoutMs, sourceSite)` 的截止时间一过，运行就以 `TimedOut` 和 `P_AWAIT_TIMEOUT` 结束，截止时间之后才到达的完成结果不会被接受。两种情况下，子 Core 运行都会被取消；检查 `input.ctx.cancel_token.is_cancelled()` 的节点会提前停止，忽略它的节点则会让结果推迟到它返回为止。`TimedOut` 和 `Cancelled` 的结果会在 `execution_trace` 里列出已完成的 Core 节点。

Program 的引擎不会向 stderr 输出 lint 信息，也不会输出 fan-out 提示。推荐设置 `EngineConfig::worker_count` 的那条提示，只会出现在自行构建的 `GraphEngine` 上。

### 内存状态与恢复

默认情况下，`LocalProgramHost` 把 bundle、版本、激活记录、运行记录和检查点都保存在宿主对象的内存里，不向磁盘写任何东西。要恢复一个运行，必须通过启动它的那个宿主实例，且该对象还活着。进程重启之后，或者换另一个 `LocalProgramHost`，`resume()` 会抛出带 `P_RUN_NOT_FOUND` 的 `ng.ProgramDiagnosticError`：`run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them`。PostgreSQL 的 Program 存储是 C++ 宿主的 API。

设置 `sqlite_directory=path` 后，宿主会把 bundle、版本、运行记录和检查点保存在该目录下的 SQLite 文件里（`program.db`、`checkpoints.db`），把 provider 调用保存在 provider 调用 journal 里（`provider_calls.db`），这样运行就能在进程结束后继续存在：用同一个目录和同一个注册表打开的宿主，可以接着运行。`resume()` 继续被中断的运行，`reconnect(run_id)` 继续一个在进程停止时仍处于运行状态的运行。恢复过程从不重新发送 journal 中记录过的请求：journal 会重放当时的回复，这次调用只计一次费。绑定了 provider 的宿主还需要 `native_history_archive=`（第一次用 `ng.NativeArchive.provision`，之后用 `ng.NativeArchive.open`），因为 journal 把每条回复都放在这个 archive 的托管之下。只含 `define()`、且没有子预算的 Program 运行，在其挂钟时间内持有一个执行租约，以免别的运行时接着运行一个所有者可能仍然存活的运行：在租约到期之前，`reconnect()` 会抛出 `P_RUN_LEASE_HELD`，消息里会说明原因；到期后，运行以 `TimedOut` 结束，并为已记入 journal 的调用计费。带有 `main()` 的 Program，或带子预算（`max_child_depth` 和 `max_total_children` 都大于 0）启动的 Program，不持有租约，`reconnect()` 会立即从最近一个检查点继续。崩溃恢复从运行最近一个持久的检查点继续，例如中断时留下的那个。每个 Core 调用都从它停下的地方继续（#402）：保存过 Core 检查点的 `callCore`（typed plan 中是 `call_core`）从它最近的检查点继续，所以已完成的调用重放其结果，已暂停的调用再次提问。在第一个检查点之前停止的调用，可能已经开始却没有留下痕迹。因此 `reconnect()` 会把运行以 `Interrupted` 交回，并带上指明该调用（`root.javascript.<n>`，或 plan 的操作）的 `result.interrupt.pending_effect`，JavaScript 和 typed plan 都一样（#404）。C++ 宿主用 `ProgramRuntime::reconcile()` 回应：`Unknown` 让运行保持 `AmbiguousEffect`；带着调用结果的 `Completed` 让运行继续，该调用预留的挂钟时间和 Core 步数会还给运行（#403）。它预留的 token 和金额仍视为已花费，因为无法知道它用了多少。

`resume(run_id, value)` 返回一个句柄，调用 `.wait()` 取得结果。不带 `pending_id` 时，它恢复该运行中唯一在等待的调用。没有等待中的调用，或者有多个时，它抛出 `ValueError`；这时把 `result.interrupt.pending_input.call_id` 作为 `pending_id` 传入，即可精确指定要恢复的调用。`fork(run_id, value, version, budget)` 从该运行确切的检查点（或 `checkpoint_id=` 指定的检查点）出发，启动 `version` 的一个新运行，并用 `value` 回应它待处理的调用；`budget` 是分叉从源运行的 `remaining_budget` 中分得的一份：`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_program_operations`、`max_core_steps` 和 `max_dynamic_compiles` 是量，由分叉从源运行剩下的量中取走，其余留给源运行；`max_concurrency` 是上限，分叉带着自己的上限（不超过源运行的），源运行保留自己的。比源运行剩余量更大的分叉会抛出 `P_FORK_BUDGET`，会让源运行的 `wall_time_ms` 或 `max_core_steps` 变成 0 的分叉会抛出 `P_FORK_SOURCE_BUDGET`，两种情况都不会改变源运行。`pending_id=` 与 `resume()` 一样用来指定调用。fork 会像 `resume()` 一样重放暂停的运行已经完成的内容：暂停那一步中已完成的节点或 Send，既不会再运行，也不会再计费，它们的 provider 调用也不会再次发送（#390）。在 fork 中再次运行的节点或 Send（例如暂停的那个）会从源运行的 journal 重放它在提问之前发出的 provider 调用，并把这些调用重新绑定到 fork：它们既不会再次发送，也不会再次计费（#405）。这种重放需要持久的 provider 调用 journal，设置了 `sqlite_directory` 的宿主有这样的 journal。内存宿主没有，所以在内存宿主上，fork 中再次运行的节点会把它在提问之前发出的 provider 调用再发送一次、再计一次费，fork 会把这一点告诉你：`fork()` 返回的句柄有 `notices`，是由 `ng.ProgramNotice` 对象（`code`、`message`、`witness`）组成的列表。如果暂停的那次尝试在暂停的操作里记录了 provider 调用，`notices` 中就有一条代码为 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` 的通知。它的 `witness` 包含 `source_run_id`、`source_attempt`、`operation_id`、`calls`（每项有 `call_id`、`node`、`model` 和 `microunits`）以及这些调用的总 `microunits`。通知不是错误，也不改变 fork 发送什么、计费多少。通知无法说明列出的调用中哪些会在 fork 中再次运行，因为在暂停那一步中已完成的节点所发出的调用，会从该步保存的写入重放，不会再次发送（#390）。在有 journal 的宿主上做的 fork，以及暂停的那次尝试没有在暂停的操作里记录调用的 fork，`notices` 都是空列表。这个列表属于 `fork()` 返回的句柄，不随运行一起保存，所以其他所有句柄（包括 `reconnect()` 返回的）的列表都是空的。如果某个 Core 步骤在多个中断上暂停，`result.interrupt.pending_input.core_interrupt_id`（或 `pending_effect.core_interrupt_id`）会指明这个待处理调用所回应的是哪个 Core 中断，`resume()` 就恰好回应那一个；该步的其他中断会再次提问，每次处理一个待处理调用。只有一个 Core 中断时，它是 `""`（#388）。`fork()` 同样恰好回应那个中断，fork 出来的运行会重新提出该步的其他问题（#394）。fork 一个 JavaScript `main()`，会像 `resume()` 一样，从源运行暂停时所在的命令继续：`main()` 用源运行的输入重新运行（fork 自己的 `input` 不使用），已完成的命令从源运行的命令 journal 重放，不再运行、发送或计费，暂停的 `callCore` 则带着 `value` 从它的检查点恢复（#395）。fork 重放的、已记录的失败会保留它的 provider 证据：fork 会把这份证据从源运行的托管重新绑定到自己的托管，因此 `result.failure.provider_outcome` 在 fork 中与在源运行中一样得以恢复（#398）。

`resume()` 返回的结果中，`execution_trace` 只列出恢复之后运行的 Core 节点；中断之前运行的节点出现在被中断的那份结果里。对于在 `b` 之前中断的 Core `a → b → c`，被中断的结果是 `["a"]`，恢复后的结果是 `["b", "c"]`。

## 强制的生命周期 Hook

Hook 由宿主的生命周期事件触发，而不是由模型决定调用某个工具而触发。

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

在 `FailClosed` 下，回调失败会阻塞受保护的运行时边界。只有在可以接受观测数据丢失的情况下，才能使用 `Continue`。

`HookBoundaryBlocked` 带有被阻塞的 Hook 的 `invocation_id` 和它的 `state`（`"failed"`、`"timed_out"` 等）。Python 工具遵循[工具失败契约](reference-en.md#tool-failure-contract)：在工具内部抛出的 `HookBoundaryBlocked`（例如由嵌套的引擎运行抛出）会终止运行，`NodeInterrupt` 会让运行暂停，其他任何异常都会变成模型能读到的工具结果。

## 运行时上下文、Skill 与严格分发

Python 绑定暴露了不可变的 RAW 历史记录、上下文制品（artifact）、纪元（epoch）、必需的 Skill 与约束、转换回执（transformation receipt），以及 provider 分发回执。

创建 RAW `RuntimeHistoryRecord` 时，如果是 Assistant 消息，要把 `RuntimeHistoryRecordData.trust` 设为 `RuntimeTrustClass.ModelOutput`。默认的 `UntrustedInput` 只接受 User 消息，用于 Assistant 输出会被拒绝。这些标签描述的是消息的来源，不授予执行工具、预算或代码方面的任何权限，也不能替代真实的 native replay 托管。

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

`ContextTransformReceipt` 允许任意的派生证据，但要求每个必需的制品保持字节级完全一致。

完整的严格路径需要使用持久的 SQLite 存储：

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

`max_input_tokens` 与对编码后请求的启发式估算值（字节数除以三）比较，不是分词器统计出的数值；参见[上下文预算](STRICT_RUNTIME_INTERPOSITION.md#context-budget)。

与 C++ 一样，这个 profile 由宿主逐步推进：宿主每次 provider 调用前，都要追加 RAW 记录，并激活新的 epoch。如果某次调用提交了 epoch 首次分发之后新增的助手消息或工具消息，就会抛出 `RuntimeError`，错误信息会点明这一契约。`profile.attach(engine)` 之后，在一次 `engine.run()` 内部运行工具循环，就会触发这种情况；参见[宿主逐步推进契约](STRICT_RUNTIME_INTERPOSITION.md#host-stepped-contract)。

## HITL 与状态

静态的 `interrupt_before`/`interrupt_after`、动态的 `NodeInterrupt`、同步的 `resume`、asyncio 的 `resume_async`，以及精确的 `resume_from`，都需要检查点存储。

由 `ng.Send` 分发的 Python 节点如果抛出 `ng.NodeInterrupt`，会像其他节点一样让运行暂停：`run`、`run_stream`、`run_async` 和 `run_stream_async` 返回带检查点的、已中断的 `RunResult`；`resume`、`resume_async` 和 `resume_from` 带着回应运行这个 Send，而发出这些 Send 的节点和已经完成的 Send 则直接重放，不会再运行。同一步里有多个节点或 Send 抛出 `ng.NodeInterrupt` 时，`result.interrupts` 把每一个都列为 `{"id", "node", "value"}`，`result.interrupt_id` 指的是第一个。`resume`、`resume_async` 和 `resume_from` 接受 `interrupt_id=`，回应它指定的那个中断，不带时回应第一个；本次恢复中，其余暂停的节点或 Send 会再次提出各自的问题，由下一次恢复来回应（#388）。`interrupt_id` 如果不是待处理的中断，会抛出 `ValueError`。`RunMetadata.resume_interrupt_id` 携带的是同一个 id。回应只送达它所回应的那个节点或 Send，而且只在恢复重新进入的那一步里有效：恢复后的运行在之后每一步中，`input.ctx.resume_value` 以及工具闸门的 `ToolGateContext.resume_value` 都是 `None`，所以在那里提问的节点或闸门会让运行再次暂停（#393）。`interrupt_before` 暂停之后的恢复，会把这个值交给被该闸门拦住的节点；`interrupt_after` 暂停之后的恢复，则不把它交给任何节点。同一步里有多个带闸门的节点同时就绪时，每个闸门单独回应：在下一个带闸门的节点之前，运行会再次暂停，直到这一步的每个闸门都得到回应，然后每个带闸门的节点收到的是对它自己那个闸门的回应（#396）。在同一步里，失败的节点或 Send 比提问的节点或 Send 优先，与它们的顺序无关：运行会抛出该失败，且不报告任何中断；只有不确定的副作用（payload 的 `kind` 为 `tool_effect_reconciliation` 的 `NodeInterrupt`）在出现失败时仍会暂停，以便对账（#392）。

`input.ctx.resume_interrupt_id` 告诉节点，`input.ctx.resume_value` 回应的是哪个中断（#406）。如果该值回应的是节点自身的 `NodeInterrupt`，它就是这个中断的 id（`result.interrupt_id` 报告的 `"s3:review"` 这样的值），无论恢复是用 `interrupt_id=` 指定的，还是回应了第一个中断。如果该值是拦住此节点的 `interrupt_before` 闸门的批准，它就是 `None`。位于闸门之后、同时也会提出自己问题的节点，在 `resume_interrupt_id` 为 `None` 时抛出 `ng.NodeInterrupt`，这样就不会把放行的批准当作对自己问题的回应。[核心概念指南](concepts.md)给出了这种写法。

Python 的 `CheckpointStore` 子类可以实现 `requires_managed_budget(thread_id) -> bool`。这个同步的虚方法读取已持久化的受管 bank 拒绝义务（denial obligation）；引擎原生的异步门面会调用它，binding 会为这个 Python 重写获取 GIL。即使检查点状态已被剥离或删除，也要如实报告已持久化的义务。不重写这个方法，则保留原生明确的“后端不支持”错误，而不是返回 `False`。

这个读取接口不授予消费、恢复或租约方面的任何权限。受管的有界执行仍然需要实际受支持的原生受管预算租约；只实现这个读取接口，无法提供租约。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

检查状态和时间旅行，用 `get_state_history`、`update_state` 和 `fork`。`get_state_view()` 提供基于 Pydantic 的扁平通道访问，`get_state()` 则保留规范的嵌套表示。

## 异步与取消

`run_async`、`run_stream_async` 和 `resume_async` 返回 `asyncio.Future` 对象。取消这些图的 Future，会通过 `CancelToken` 发出取消请求；原生 I/O 要到达它的取消边界，操作才会结束。图的流式回调会回到调用方的 asyncio 事件循环线程。解释器退出时，模块在导入时注册的 `atexit` 钩子会在终结开始之前，停止支撑这些 Future 的线程，以及运行 Python 节点和工具主体的线程池。它会等正在运行的 Python 代码结束，丢弃仍在等待的运行；之后再调用 `run_async`、`run_stream_async` 或 `resume_async` 会抛出 `RuntimeError`。

Provider 的 `invoke` 和 `dispatch` 是同步的 Python 方法。它们的绑定在原生调用运行期间释放 GIL，随后在执行 Python 的 provider 重写和事件回调时重新获取。事件回调收到的是持有所有权的 `ProviderEvent` 值，可能在原生 worker 线程上运行。回调需要更新属于 asyncio 的状态时，请使用 `loop.call_soon_threadsafe`。

用 `request.on_event = callback` 指定 observer，赋值 `None` 则清除。读取 `request.on_event` 得到的是原来那个 Python 可调用对象本身（对象身份不变）；`RunConfig.on_provider_event` 遵循同样的规则。每个事件提供字符串 `kind` 和带类型的 `value`；`ProviderPartDelta` 的值自己持有其 `bytes`。回调结束后继续保留事件，并不会保留借用的 SDK 文本视图。要请求取消，对赋给 `request.cancel_token` 的 token 调用 `token.cancel()`。

用 `asyncio.to_thread(provider.invoke, request)` 可以让同步的 provider 调用不占用事件循环线程。仅取消这个 await，并不会取消 provider 调用：需要把一个 `CancelToken` 赋给 `request.cancel_token`，并显式请求取消。

对于有效的已准备句柄，如果它的 token 已经被取消，provider 分发的预检会返回一个持有所有权的 SDK `Cancelled` 失败。同步分发走的是同一条原生异步 provider 路径，并等它跑完。在图、宿主或协程入口处的取消是另一个边界，可能在 `ProviderOutcome` 返回之前就抛出异常；请求取消，并不保证在每个边界上都得到带类型的 `Cancelled` 数据。取消请求同样不能证明没有请求发出、远端的工作已经停止，或者不会产生费用。

[pybind11 的 GIL 文档](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil)解释了为什么释放 GIL 和为 Python 回调重新获取 GIL，是绑定层中两项各自独立的职责。

## 协议与可观测性

- 构建时包含 MCP 的话，MCP 客户端工具可通过 `neograph_engine.mcp` 使用。
- 构建时包含 A2A 的话，A2A 客户端类型可通过 `neograph_engine.a2a` 使用。
- `ProtocolHostAdapter` 把官方的 Python A2A/ACP 服务端 SDK 与 NeoGraph 的会话语义对接起来。
- `neograph_engine.tracing` 和 `neograph_engine.openinference` 输出与厂商无关的 OTel/OpenInference 数据，可用于 Phoenix、Langfuse、Arize 及兼容的后端。

### 原生 A2A 的发现与流式调用

`a2a.WireDialect` 取 `V1_0`（主要）或 `V0_3`（兼容）。构造之后，或强制获取 card 之后，`client.wire_dialect()` 返回 `None`，直到发生由 card 选定的 RPC 或探测成功为止。`AgentCard.supported_interfaces` 包含分离的 `AgentInterface` 记录，字段有 `url`、`protocol_binding`、`protocol_version`、`tenant`；`card.raw` 是仅供观察的 JSON。原生客户端在第一次调用之前会获取 card，并选择一个兼容的 JSONRPC 接口，优先选用其规范化后的基础 URL。接口的 URL 与基础 URL 同源时，RPC 发往该 URL；`client.set_allow_cross_origin_interfaces(True)` 允许 card 指定另一个 origin，`client.endpoint_url()` 报告正在使用的 endpoint。只有在无法获取 card 时，客户端才会探测：先试 1.0，遇到 `-32601`、`-32009` 或 HTTP 400 时回退到 0.3；由 card 选定的调用不回退。消息 id 是随机 UUID。`a2a.A2ARpcError` 保留实际的整数 `.code`。

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

运行这个示例之前，先在该 endpoint 上启动一个真正的本地 A2A 服务；它与 Chat 的回环对端是两回事。`send_message(params)` 是非流式的多 part 重载；文本的便捷重载仍然保留。凭据要用 `client.set_authorization_header(authorization_header)` 显式设置，并且不要让该值进入日志。`Part.media_type`、`file`、`data`、`metadata`、消息的扩展/引用 ID 和任务制品，保留的是带类型的/数据层面的观测，绝不具备 provider 的 native 权限。

`params.message` 和 `Task.status` 是活的内联记录；可选的配置、向量和事件的子对象是分离的快照，要更新可变的输入，需要重新赋值。`StreamEvent.type` 取 `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task`；可检查 `status_update`、`artifact_update`、`task` 和 `is_final()`。V1 的起始任务不是终态；原生 SSE 会把状态更新以及追加/替换的制品更新组装起来。阻塞调用会释放 GIL；回调会获取 GIL，并持有保证在 GIL 下安全销毁的所有者。回调交付不意味着可以根据已观察到的输出重新分发。回调线程不保证是 asyncio 事件循环线程；需要时请使用 `loop.call_soon_threadsafe`。


从 `neograph_engine.openinference` 导入 `OpenInferenceProvider`。它的构造函数是 `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")`；这个门面把调用委托给原生的 C++ 包装器，包装器继承了带类型的 `prepare(request)`、`dispatch(prepared)` 和 `invoke(request)`。默认的 span 名只是一个标签，并不是恢复了 `complete` 方法。需要安装 `opentelemetry-api` 和 `opentelemetry-sdk`；构造时需要 OTel API，导出时需要配置好的 SDK span processor/exporter。

准备、准入失败和放弃，都不会创建 LLM span。通过准入的 dispatch 会启动一个 span，并在完成、SDK 失败或 dispatch 抛出异常时结束它。即使包装器和外部对 tracer 的引用都已被回收，已准备的操作仍然保留 Python 的 tracer 适配器。阻塞的 `invoke`/`dispatch` 会释放 GIL；OTel 调用和 Python 引用的销毁会获取 GIL。tracer 的失败遵循原生的尽力而为（best-effort）策略，不会替换持有所有权的 outcome、原始事件或产品本身的异常。

适配器在 dispatch 时调用 Python tracer 的 `start_span`。调用方的 context 不会自动带入原生 worker 线程。请在 dispatch 之前捕获想要的 OTel 父 context，并像下面这样通过 tracer 适配器显式传入；图节点中的 provider 如果需要图或节点作为父 span，做法相同。OTel 文档中介绍了[如何显式选择父 context](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span)。

示例中的 `ParentContextTracer` 只保存一个已捕获的父 context。要换一个逻辑上的父 span，需要重新捕获 context，并构造新的适配器/provider 包装器；沿用旧适配器，父 span 仍是原来的那个。

沿用上面 provider 示例中的 `provider` 和 `controls`，运行这个片段之前，先重启那个只处理一次请求的对端：

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

控制台 exporter 应该显示 `llm.request`，其中 `openinference.span.kind="LLM"`，并且它的 `parent_id` 与 `request` span 的 `span_id` 一致。使用上面链接的对端时，打印出的结果是句柄已被消耗、没有失败，并且包含 `Hello.`。对于新的请求，`observed.invoke(request)` 把同样的准备和分发合在一起；不要再次分发已被消耗的句柄。

LLM span 的字段包括：已准入的模型、声明过的 temperature 和输出上限、公开的消息 role 与可见的 `Text` 内容，以及已知的 input/output/total 用量计数。缺失的计数会省略对应的属性；上报为零的仍然是零。流式请求只会为可见文本内容的增量添加 `llm.token` 事件，文本在 `attributes["chunk"]` 中。SDK 失败时，使用安全的消息把状态设为 ERROR，并保留部分公开输出；成功完成时设为 OK。`request.on_event` 仍然收到原始的带类型事件。

native replay、reasoning parts、原始 envelope 和原始事件都不会进入这些 LLM span 字段。outcome 仍然保留它的 native 消息和证据；tracing 导出既不赋予 replay 托管，也不赋予核算方面的权限。公开文本可能含有敏感的应用数据，请据此选择提示词和 exporter。`openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)` 仍然是独立的图事件上下文管理器，它不能代替 provider 的 LLM span。

## 可选组件

公开的包会如实标明可选的 C++ 组件：

- `_HAVE_PROGRAM`、`_HAVE_SQLITE`、`_HAVE_POSTGRES`、`_HAVE_MCP`、`_HAVE_A2A`；
- 缺失的组件就是不存在，不会在 Python 里模拟；
- PyPI wheel 启用了 Program/QuickJS、LLM、MCP 和 SQLite；从源码构建则取决于 CMake 选项。

## 测试与示例

绑定的测试集覆盖了 Core 执行、自定义回调、asyncio、取消、Program 的编译与运行时、强制 Hook、严格的上下文、SQLite 持久化、协议，以及 README 中的示例。

- [Python 示例](../bindings/python/examples/README.md)
- [C++ 示例](../examples/README.md)
- [Program 指南](program-guide.md)
- [严格的运行时拦截](STRICT_RUNTIME_INTERPOSITION.md)
