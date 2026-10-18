<!-- neograph-i18n: source=CHANGELOG.md locale=zh-CN source_sha256=25f95f9371f16a3b6f4c3652ac7590a9e6a4d409eb7241e5c1045ebf5f400ba2 -->
<!-- neograph-i18n: source=CHANGELOG.md locale=zh-CN source_sha256=eccdfd77485685d085575f9bf352ac89afc251515fbfcdb1677a340561fbaa73 -->
# 变更日志

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

以下带日期的条目保留历史 API 与测量。旧 Provider 兼容保证已被 [0.14.0] 的 breaking C++ typed 契约取代，不是当前 API 指南。

NeoGraph 的所有显著变更均记录在本文件中。

格式遵循 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)。版本控制遵循 [语义化版本](https://semver.org/spec/v2.0.0.html)。

---

## [未发布]

### 变更
- **采用 SchemaProvider `2ffecf8202dfe5c1c2184cf5d720b76354209f83`（`maint/interface6` 上的 SDK 0.3.4）（#511）。**
  - **兼容性。** 接口修订号和共享库代数保持为 6，公共 struct 或 enum 的布局不变，内置描述符策略及其 identity 也不变。NeoGraph 现在在构建以及已安装 `NeoGraphConfig` 的 `find_dependency` 中要求 `SchemaProvider 0.3.4 EXACT`。
  - **行为变更：Chat Completions 保留生成的图像。** 图像模型（如 OpenRouter 的 `google/gemini-3.1-flash-lite-image`）在 `message.images` 中返回图像。SDK 过去忽略该成员，因此 token 已计费，结果中却没有图像。现在每张图像成为 wire 类型为 `images` 的 `Opaque` part，其 `wire_metadata` 保存 wire 条目（数据 URL 位于 `image.image_url.url`）。该 part 可以保留在历史的 assistant 消息上，但不会再发回提供方。
  - **行为变更：带有 code-execution `container` 或缓存 `diagnostics` 对象的 Messages 响应可以正常完成。** 过去会以 `Unsupported` 失败，已计费的回答随之丢失。使用 `web_fetch_20260318` 或 `web_search_20260318` 的 Claude 5.5 会同时返回这两个字段。它们保留在 `raw_events` 中。
  - **Python。** 现在可以读取 `Opaque.wire_metadata`，因此能在 Python 中取出保留的图像。
  - **本版本线不修复的问题。** 在 `claude-haiku-4-5` 等不支持 programmatic tool calling 的模型上，`_20260318` 网页工具需要 `allowed_callers`，而接口 6 无法发送该字段；此类模型请改用 `web_search_20250305` 或 `web_fetch_20250910`。Lyria 等不支持 thinking 的 Gemini 模型仍会因 family 默认的 `includeThoughts` 而返回 400。两者都将在 NeoGraph 0.16.0 所采用的 SchemaProvider 0.4.0 中修复（#512）。
  - 请使用 0.3.4 的头文件和库一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。

### 已弃用
- **类型化客户端 `VeoClient`、`ImagesClient` 和 `DecisionsClient` 只接受即将停止服务的模型。它们不再扩展，并将在 0.16.0 中移除（#510）。**
  - **`VeoClient`。** 只接受 `veo-3.1-generate-preview`、`veo-3.1-fast-generate-preview` 和 `veo-3.1-lite-generate-preview`。Google 于 2026-10-22 关闭这三个模型，从这一天起它没有可调用的模型。Gemini API 没有其他 Veo 模型，Google 指定 Gemini Omni（`gemini-omni-1.1-flash`，经 Interactions API）作为替代。
  - **`ImagesClient`。** OpenAI 已于 2026-05-12 移除 `dall-e-2` 和 `dall-e-3`。`gpt-image-1` 于 2026-10-23 关闭，`gpt-image-1-mini` 和 `gpt-image-1.5` 于 2026-12-01 关闭。Gemini 配置（`gemini-3.1-flash-lite-image`）不受影响。
  - **`DecisionsClient`。** 只接受 `typesafe/jev-1.13`。
  - **替代方案。** 图像、视频、音频、嵌入、rerank、decisions 以及实时 API，所有提供方端点都改由 SchemaProvider 负责。它按能力配置而不是固定模型列表接受模型。NeoGraph 0.16.0 将采用它，删除这三个客户端，并通过它提供 Omni 视频。

## [0.15.0] - 2026-10-18

### 变更

- **回环 MCP HTTP 服务器只应答回环名称和列出的 `Host` 名称（#499）。** 服务器检查 `Origin`，但不检查 `Host`，因此不带 `Origin` 的请求无论发往哪个名称都会被处理。DNS 重绑定页面会在 `Host` 中发送它自己的名称；它的 `initialize` 被拒绝，只是因为它同时发送了 `Origin`。现在由 `MCPHttpServerConfig::allowed_hosts` 列出服务器应答的名称：主机名、IPv4 地址和带方括号的 IPv6 地址，比较时不含端口、不区分大小写；`*` 表示任意名称。绑定到回环地址（`127.0.0.1`、`::1`、`localhost`）时，服务器总是应答 `localhost`、`127.0.0.1` 和 `[::1]`，其他 `Host` 无论是否带有 `Origin`，都会被以 `403`（`-32003`，"Host is not allowed"）拒绝。没有 `Host` 的请求，或 `Host` 不是 `host[:port]` 形式的请求，得到 `400`（`-32600`）。与官方 TypeScript SDK 的 `createMcpExpressApp()` 一样，不比较端口。绑定到非回环地址时，列表为空则和以前一样不做任何检查，非空则只接受列出的名称。检查覆盖端点上的 `POST`、`GET` 和 `DELETE`，不覆盖公开的 RFC 9728 元数据。带端口的条目、不带方括号的 IPv6 地址和空条目，会在构造服务器时以 `std::invalid_argument` 拒绝。`neograph-harness-mcp` 从 `NEOGRAPH_HARNESS_ALLOWED_HOSTS` 读取以逗号分隔的列表。**行为变更：** 位于原样转发公网 `Host` 的本地反向代理或隧道之后的回环服务器，在把该名称或 `*` 列入列表之前，会拒绝这些请求。`MCPHttpServerConfig` 增加了成员，因此请一起重新构建 NeoGraph 和 C++ 使用方代码（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **用完尝试次数的失败不再报告为可重试，ACP 客户端对请求的拒绝也不再重试（#500）。** 每个协议主机发送的 `FailureVerdict::retryable` 是节点的重试判定，与是否还有剩余尝试无关。因此，每次尝试都失败的节点，或没有重试策略、失败一次的节点，都会告诉对方 `retryable: true`；把它理解为“再试一次可能成功”的编辑器，可能会重试引擎已经重试过的操作。现在，一旦节点重试循环用完了节点的尝试次数（`kind` 为 `exhausted`），`retryable` 即为 `false`。因运行的截止时间或重试上限而停止重试的失败以及 `NG.ProviderFailed` 保持 `true`。新的 `FailureVerdict::retryable_by_class` 为在引擎之外重试的调用方保留原来的含义，ACP（`data`）、A2A（`metadata["neograph/failure"]`）、gRPC（ErrorInfo 的 metadata）和 MCP 服务器（`_meta["io.neograph/failure"]`）会把它与 `retryable` 一起发送。ACP 客户端对 agent 请求应答的 JSON-RPC 错误现在是 `acp::ACPClientError`，携带方法、代码、客户端的消息和 `data`，其 `what()` 不变。如果代码表明请求不可能成功（`-32700`、`-32600`、`-32601`、`-32602`、`-32002` resource not found、`-32000` authentication required），它就是 `acp::ACPClientRefusal`，即不可重试的 `ToolRequestRefusal`：`fs/read_text_file` 收到 `-32002` 的节点只运行一次，其轮次报告 `retryable: false`、`retryable_by_class: false`。`-32603`、其他代码和超时仍和以前一样重试。`NodeFailure::retryable`、路由的失败记录和 Program 任务重试保留按类别的含义。**行为变更：** 对用完尝试次数的失败读到 `retryable: true` 的对方，现在读到 `false`；ACP 客户端拒绝其请求的节点不再重试。`FailureVerdict` 增加了成员，因此请一起重新构建 NeoGraph 和 C++ 使用方代码（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。

### 修复

- **向已停止读取的子进程写入时，宿主不再因 SIGPIPE 而终止（#495）。** NeoGraph 会写入 stdio MCP 服务器和进程工具（`ProcessTool`）的 stdin。子进程关闭其 stdin 或退出之后，这样的写入会引发 SIGPIPE，而它的默认动作会终止整个进程。stdio MCP 客户端只在自己的 I/O 线程上屏蔽 SIGPIPE。在信号发给写入线程的 Linux 上这已足够，但在为整个进程引发该信号的 macOS 上并不够：正如一次 macOS CI 运行所示，在读取 `initialize` 之前就退出的 MCP 服务器可能终止 C++ 宿主。进程工具桥则完全没有屏蔽它。现在在 macOS 上，这类管道的写端会标记 `F_SETNOSIGPIPE`；在 Linux 上，进程工具桥在每次写入时于自己的线程上屏蔽 SIGPIPE，并收回该写入引发的信号。写入随之以 `EPIPE` 失败，调用会像其他情况一样失败或结束。此外，对于读端已不存在的已满 stdin 管道（只报告 `POLLERR`），进程工具桥会立即再次 poll，直到子进程退出，一直占用一个 CPU 核心；现在它会在此结束输入。忽略 SIGPIPE 的 Python 宿主不受影响；运行 NeoGraph 的 A2A 或 MCP HTTP 服务器的进程也不受影响，因为 cpp-httplib 的服务器会把进程的 SIGPIPE 设为 `SIG_IGN`。
- **停止 A2A 服务器时，不再切断尚未开始发送的流（#493）。** cpp-httplib（0.41 和 0.61 都是如此）在监听器停止后不会再调用 chunked content provider。`A2AServer::stop()` 先停止监听器，因此处理函数已接受请求、但流尚未发送开头 Task 的 `SendStreamingMessage` 或 `SubscribeToTask`，会在一个字节都没发送的情况下被关闭：运行仍在继续，客户端读到的却是 `End of file`，而不是运行的事件。`GraphHostA2AContract.SharedBoundary` 偶尔会因此失败。现在 `stop()` 会等到它正在处理的每个请求都已应答，或对流而言已开始发送，然后才停止监听器；该流会像已打开的流一样以运行的最终状态结束。在等待期间到达的请求不会开始工作，而是得到 JSON-RPC 错误 `-32603`（reason `UNAVAILABLE`，"The agent is stopping; retry later"）。
- **节点通过抛出异常的门面调用工具时，工具服务拒绝的请求不再重试（#502）。** `ToolExecutionController::execute_async` 会把工具的 `ToolRequestRefusal`（MCP `-32602`、gRPC `INVALID_ARGUMENT`、`acp::ACPClientRefusal`）转为失败结果，并以 `ToolExecutionError` 抛出，而该异常并不说明调用已被拒绝。节点失败分类器把它读作普通失败，因此节点的重试策略会再次发送被拒绝的请求，每个协议主机都报告 `retryable_by_class: true`。现在 `ToolExecutionResult` 增加了在这类调用中设置的 `refused`，分类器对不可重试的被拒绝调用，按处理拒绝本身的方式处理：节点只运行一次。`ToolExecutionResult` 增加了成员，因此请一起重新构建 NeoGraph 和 C++ 使用方代码（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。

## [0.14.3] - 2026-10-18

### 变更

- **采用 SchemaProvider `0437853529b5932e7097a38629a95779d08f03aa`，即 `maint/interface6` 上的 SDK 0.3.3（#496）。** 接口修订版本和共享库代数仍为 6，公开的 struct 和 enum 布局不变。SDK 的两项修复会改变 NeoGraph 用户看到的行为。**行为变更：** 流式文本部分不再传递文本为空的 delta（SchemaProvider #37）。OpenAI Chat、Anthropic Messages、Gemini 和 Responses 的流各自会发送一个这样的 delta，NeoGraph 把它作为文本为空的 `LLM_TOKEN` 事件和 `llm.token` span 事件传了出去，因此统计 token 数或把第一个事件当作第一个 token 的使用方会差一个。文本、各部分和最终结果都不变。**行为变更：** buffered 路径和 streaming 路径没有指向同一个字面模型（`/v1beta/models/<model>:generateContent` 与 `/v1beta/models/<model>:streamGenerateContent?alt=sse`）的 `google.generate` 描述符，在加载时就会以指明路径的 `ConfigError` 被拒绝；以前它能加载，之后每个请求都以 `InvalidConfig` 失败（SchemaProvider #40）。SDK 文档还说明了位于以其他模型名应答的代理或网关之后的部署，如何把它声明为路由网关（SchemaProvider #38）。NeoGraph 现在在构建和安装的 `NeoGraphConfig` 的 `find_dependency` 中要求 `SchemaProvider 0.3.3 EXACT`。请用 0.3.3 的头文件和库一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。

### 修复

- **MCP HTTP 服务器处理写了另一个受支持 `MCP-Protocol-Version` 的请求（#490）。** `initialize` 之后，只要请求的 `MCP-Protocol-Version` 与会话协商的修订版本不同，服务器就一律以 `400` 应答，服务器自己支持的修订版本（`2025-11-25`、`2025-06-18`、`2025-03-26`、`2024-11-05`）也不例外。MCP 2025-11-25 只要求对无效或不受支持的修订版本返回 `400`，官方 TypeScript 服务器接受任一受支持的修订版本；conformance 套件的 `server-sse-multiple-streams` 场景在 `2025-11-25` 会话上发送 `2025-03-26`，收到了三次 `400`。现在这样的请求按会话协商的修订版本处理：批处理、工具字段等都跟随会话，而不是该头。一个会话中第一个这样的请求会向 stderr 写一行警告。不受支持或格式不正确的值仍是 `400`（`-32600`，消息现在是 "MCP-Protocol-Version is not a supported revision"），没有该头的请求仍使用协商的修订版本。**行为变更：** 发送另一个受支持修订版本的客户端会得到处理，而不是被拒绝。
- **绑定到回环地址时，MCP HTTP 服务器默认接受它自己的回环来源（#491）。** 带有 `Origin` 的请求只要不在 `allowed_origins` 中就以 `403` 拒绝，而该列表默认为空，所以回环服务器连自己的来源也拒绝。conformance 套件的 `dns-rebinding-protection` 场景用服务器自己的 `Host` 和 `Origin` 发送 `initialize` 并期望成功，在默认设置下会失败。现在绑定到回环地址（`127.0.0.1`、`::1`、`localhost`）时，服务器还接受所绑定端口上的 `http://localhost:<port>`、`http://127.0.0.1:<port>` 和 `http://[::1]:<port>`，按浏览器发送的形式精确比较，并在第一次接受每个来源时向 stderr 写一行。DNS 重绑定页面发送的是它自己的来源，因此仍被拒绝；其他端口上的回环来源、`https` 来源和 `null` 也被拒绝。其他 localhost 端口上的页面（例如浏览器检查器）仍须列入该列表。非回环绑定与以前一样只接受列出的来源。**行为变更：** 绑定到回环地址时，`Origin` 为服务器自己回环来源的请求会得到处理，而不是以 `403` 拒绝。

## [0.14.2] - 2026-10-18

### 变更

- **需要重新构建 C++ 使用方：`WsClientOptions` 新增了成员，`A2AServer` 和 `WsClient` 新增了函数（#456, #458）。** `neograph::async::WsClientOptions` 新增 `handshake_timeout` 和 `read_timeout`，因此其 layout 改变（它按值传给 `ws_connect`）；`A2AServer` 新增 `set_trust_forwarded_headers`，`WsClient` 新增 private 成员函数。请一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **采用 SchemaProvider `91fb993e50e49e3b03d290b170c83cf57afbc518`（`maint/interface6` 上的 SDK 0.3.2），它修复了 SchemaProvider #33：从 Google 的 OpenAI 兼容端点流式传输重新可用（#459）。** Google 的 `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` 在除最后一个之外的每个流式 chunk 中都省略 `finish_reason`，而 SDK 的 Chat 编解码器拒绝这样的 chunk，因此自 NeoGraph 在 0.13.0 将 provider 层迁移到 SchemaProvider 以来，来自该端点、超过一个 chunk 的 `openai.chat` 流都以 `ProtocolCorrupt`（"invalid response protocol"）结束。现在 SDK 在流仍打开时把缺失的成员读作 `null`；流在 `[DONE]` 之前仍须带有字符串 `finish_reason`，缺少该成员的 buffered choice 仍被拒绝。OpenRouter、原生 `google.generate` 系列和 buffered 调用不受影响。interface revision 和 shared-library generation 仍为 6，public struct 或 enum 的 layout 不变。NeoGraph 现在在构建和已安装 `NeoGraphConfig` 的 `find_dependency` 中要求 `SchemaProvider 0.3.2 EXACT`，因此已安装的 0.3.1 包不再匹配。请针对 0.3.2 的头文件和库一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **面向其他语言实现方的 gRPC 契约说明（#453）。** proto 和 reference 现在写明了 0.14 早已要求的内容：`CheckpointService` 必须提供所有 RPC，包括 `RequiresManagedBudget`。引擎在每次新运行和恢复时都会调用它，遇到 `UNIMPLEMENTED` 会让运行失败（从不假定不存在受管 budget），因此按 0.13 RPC 集合编写的服务器需要一个 `RequiresManagedBudget`，在不管理 budget 时返回 `required = false`。`RunGraphRequest.thread_id` 已注明为必填（自 0.14 起空值以 `INVALID_ARGUMENT` 拒绝）。自 0.14 起 gRPC 头文件会包含 gRPC 自身的头文件，所以带有 `using namespace neograph;` 的代码必须把 NeoGraph 的类型写成 `neograph::grpc::...`：在那里单独的 `grpc::` 有歧义。仅文档变更，没有行为变更。
- **随附的 cpp-httplib 从 0.41.0 更新到 0.61.0（#460）。** 只有 A2A 服务器、MCP Streamable HTTP 服务器和测试对端使用 cpp-httplib；NeoGraph 的 HTTP 客户端使用 libcurl 和 Asio。`deps/httplib.h` 重新成为未经修改的上游发行版：旧头文件中针对 MSVC 的本地修复（`X509_get_subject_name` 返回 `const X509_NAME *`）自 0.44.0 起已进入上游，#450 的两个 backport 也包含在 0.61.0 中。服务器获得了上游自 0.41.0 以来对请求解析的加固：`Content-Length` 无效、request-target 或 chunk-size 行含有控制字节，或带有多个 `Host` 字段的请求会得到 400 响应并关闭连接；流水线请求无需等待 keep-alive 超时即可处理；回调抛出的异常只断开该连接，不再终止进程；listen backlog 由 5 改为 128。**行为变更：** 自 cpp-httplib 0.44.0 起不再对请求头的值做百分号解码，因此头的值按发送时的原样到达服务器：服务器检查的 `Authorization`、`Origin`、`Mcp-Session-Id` 等头按字节比较，对这些值做了百分号编码的客户端将不再匹配。
- **QuickJS 性能门禁允许运行器波动超出上限最多 10%（#463）。** `javascript_wrapped_core_ratio` 在托管运行器上介于其上限的 0.79 倍到 1.05 倍之间，因此 CI extended 在没有回归的情况下失败：逐字节相同的基准测试二进制文件在一次运行中测得中位数 547，在另一次运行中测得 617。预注册文件现在声明 `threshold_tolerance` 为 0.1，`scripts/run_quickjs_performance.py` 也固定同一个值。超出上限但不高于上限 1.1 倍的统计值视为通过，并在 JSON 报告和 job 日志中记为警告；更高的值与以前一样失败。上限本身不变。超出上限的统计值现在会在 job 日志中列出，而不仅仅记录在上传的报告里。
- **QuickJS 性能门禁改为判定 JavaScript 包装的 Core 运行本身的耗时；它与直接 Core 运行的比值仍写入报告，但不再阻断门禁（#465）。** `javascript_wrapped_core_ratio` 以直接 Core 运行的耗时为分母，因此仅仅 Core 变快就会使它上升：#442 之后，在较慢的运行器类别上，直接运行从 8.94 µs 降到 7.97 µs，JavaScript 包装的运行变化不到 2%（4,311 µs 和 4,366 µs），比值却从 489 升到 549。反过来，Core 变慢可能掩盖 JavaScript 路径的变慢。新的阻断用例 `javascript_wrapped_core` 以每次调用的 µs 计量同一运行，上限为中位数 5,500 µs、p95 6,000 µs。在 2026-10-09 至 2026-10-16 的 39 次 CI extended 测量中，各次运行的中位数在 2,436 到 4,515 µs 之间，各次运行的 p95 最高为 4,720 µs；预注册文件记录了这一依据。比值连同其上限保留在报告中，超出上限时在 job 日志中记为警告。上限以 CI extended 的 GitHub 托管 x86-64 运行器为基准，预注册文件和 README 现已写明这一点：在更慢的硬件上本地运行时，即使没有回归，该用例也可能失败（一台安静的 Arm Neoverse-N1 机器测得中位数 6,234 µs）。报告现在还会记录 CPU 型号和运行器环境。
- **Program 调用快了约 30%：canonical JSON 直接从底层 yyjson 值写出（#469）。** 每条 Program 记录（run record、日志条目、事件、lineage）都通过 canonical JSON 获得身份，这项序列化占了一次 Program 调用三分之一的时间。`canonical_json_bytes`、其大小和深度检查以及严格的重复成员检查都遍历 `neograph::json` 句柄：每次访问子节点都复制一个句柄，带来一次原子引用计数开销；每次遍历都构建键和值列表；每个键或字符串都被复制。现在这三种遍历都直接读取句柄下的 yyjson 值。字节、成员顺序、限制以及先报告哪个错误都不变：一个随机化测试把新的写出器与旧的进行比较，所有固定身份都不变。在一台安静的 Arm 机器上，`quickjs-performance` 门禁中 JavaScript 包装的 Core 运行从 6,126 µs 降到 4,350 µs，`structured_join_sequence` 从 10,060 µs 降到 6,967 µs，`structured_join_await` 从 7,505 µs 降到 5,143 µs；canonical JSON 在包装运行中的占比从 34% 降到 12%。
- **Program 调用快了约 23%：json 文档从每线程的块缓存获取内存（#471）。** 每个 `neograph::json` 值都拥有自己的 yyjson 文档，每个文档最多分别分配四次：`shared_ptr` 控制块、文档本身、一个值池块和一个字符串池块。一个 Program 记录的主体在每次 transition 中创建并释放数百个这样的文档。同一尺寸的块多于 malloc 每线程缓存所能容纳的数量，因此大多走了较慢的路径。现在这些块来自按尺寸分类的每线程缓存：32 字节到 2 KiB，每类最多 64 个，每线程少于 260 KiB。更大的块仍像以前一样交给 malloc。在其他线程释放的块进入那个线程的缓存，线程结束时释放其缓存。使用 AddressSanitizer、ThreadSanitizer 或 MemorySanitizer 的构建直接分配每个块。在一台安静的 Arm 机器上，`quickjs-performance` 门禁中 JavaScript 包装的 Core 运行从 6,196 µs 降到 4,745 µs，`structured_join_sequence` 和 `structured_join_await` 快了 24%。与 #469 一起，这三个用例快了 44% 到 47%。
- **QuickJS 性能门禁还会在同一运行器上把每个用例与 base 构建进行比较（#476）。** 不同类型的托管运行器之间数值相差可达 1.7 倍，因此绝对上限无法发现 #467 中 9% 到 23% 的变慢。现在 `scripts/verify_ci.py quickjs-performance --base <ref>` 用 `git archive` 导出该提交并构建其 QuickJS 基准测试。随后 `scripts/run_quickjs_performance.py` 在两个构建上运行除 `replay_growth` 以外的所有 primitive 和 control 用例，各取 20 个样本并交替进行。head 中位数超过 base 中位数 1.1 倍时记为警告，阻断用例超过 1.25 倍时会用新样本再比较一次，只有这次仍超过 1.25 倍才失败。base 构建中没有的用例报告为跳过。比值记录在预注册文件（`ab_comparison`）中，报告会记录比较结果。CI extended 把分支与其在 `master` 上的 merge base 比较，把 `master` 与最新发布版本比较。绝对上限仍然适用。
- **Program 调用又快了约 20%：记录主体在一个 JSON 文档中构建（#478）。** Program 调用此前用 `json` 初始化列表构建记录的 canonical 字节。初始化列表为每个键、每个值和每个键值对各创建一个 yyjson 文档，再把每个值复制到结果中。因此在 `quickjs-performance` 门禁的 JavaScript 包装 Core 运行中，每次调用会产生约 5,400 个文档。现在由内部构建器（`neograph::detail::JsonBuilder`）在一个文档中构建事件、run record、lineage 和 generation 记录、日志记录、invocation 以及命令日志条目的主体。构建器的值重载与 `json` 的转换构造函数一一对应，因此每个成员保持相同的 yyjson 类型，字节、身份和限制检查都不变。一个新测试固定了变更前生成的代表性记录的 31 个身份。现在每次调用约产生 1,000 个文档。在一台 x86 桌面机上，JavaScript 包装的 Core 运行、`structured_join_sequence` 和 `structured_join_await` 分别快了 21%、18% 和 19%。

### 已添加

- **内置 gRPC 服务器响应标准 health 服务，并且可以停止（#454）。** `run_server`、`run_tool_server` 和 `run_checkpoint_server` 现在提供 `grpc.health.v1.Health`：在接收调用期间为服务器及其服务（`neograph.v1.GraphService`、`neograph.v1.ToolService`、`neograph.v1.CheckpointService`）返回 `SERVING`，停止后返回 `NOT_SERVING`，因此 Kubernetes gRPC 探针和 `grpc-health-probe` 可以使用。gRPC 关于 health 服务的进程级默认值只在内置服务器启动期间打开，随后恢复，因此宿主自己的服务器不受影响。新的重载接受 `ServeOptions`（`<neograph/grpc/serve_options.h>`）：一个 `std::stop_token`，请求停止后为进行中的调用等待 `shutdown_grace`（默认 5 秒）再停止服务器；以及一个接收所绑定端口的 `on_serving` 回调，因此地址可以使用端口 0。服务器反射由宿主选择：链接 `gRPC::grpc++_reflection` 并在服务器启动前调用 `grpc::reflection::InitProtoReflectionServerBuilderPlugin()`，gRPC 会把反射加到所有服务器上，包括内置服务器。现有签名不变。
- **`json::parse` 接受调用方指定的嵌套上限（#481）。** 新的重载 `json::parse(std::string_view, std::size_t max_depth)` 和 `json::parse(std::istream&, std::size_t max_depth)` 拒绝数组和对象嵌套超过 `max_depth` 层的文本；上限为 0 时只接受标量。不带上限的重载仍使用 `json::max_parse_depth`（自 #480 起为 256），NeoGraph 自身的调用（包括其服务器和客户端）也继续使用这些重载。复制和比较值时每一层递归一次，因此大于 256 的上限用于调用方信任的文本，并且要在栈足以复制和比较这么深的值的线程上读取。现有签名不变：只增加符号，不改变布局。

### 修复

- **即使服务器保持 GET 事件流打开，`MCPClient::shutdown()` 也会返回（#448）。** 在 Streamable HTTP 上，客户端在 `initialize` 之后打开可选的 GET 事件流。当服务器在会话的 `DELETE` 之后仍保持该流打开时，`shutdown()` 和析构函数会一直等到服务器关闭它，可能永远等待。官方 Python 和 TypeScript 服务器在收到 `DELETE` 时关闭该流，掩盖了这个问题；conformance suite 的 `sse-retry` 场景不关闭，因而在退出时超时。当流刚结束时，shutdown 还会等待重连前的停顿，也就是服务器指定的 `retry`。listener 的停止信号是一次被取消的定时器等待，而 `awaitable_operators::operator||` 把它算作失败，于是继续等待流读取或停顿。现在停止等待在定时器被取消或到期时都会正常完成，并且 shutdown 让定时器到期，因此尚未开始等待的 listener 也会停止。没有 API 变更。
- **MCP HTTP 客户端可以读取以单个 HTTP chunk 发送的超过 1 MiB 的 SSE 事件，并跟随端点同源的 307 和 308 重定向（#449）。** 对于像 Node 和 uvicorn 服务器那样每个事件写成一个 chunk 的服务器，若响应流中大于 1 MiB 的事件位于一个 chunk 中，即使客户端接受最大 16 MiB 的事件，也会以 `async_post_stream: response chunk limit` 失败；现在 chunk 可以和事件一样大。`307` 或 `308` 响应（例如基于 `mcp` 1.x 的 FastMCP 服务器把 `POST /mcp` 移到 `/mcp/` 时发送的响应）以前会报错；现在仅当 `Location` 与端点 URL 同源（相同的 scheme、host 和 port；相对 `Location` 也算）时，客户端才会带着原方法、正文和头部在 `Location` 重发请求，最多五次。到其他源的重定向（包括从 https 到 http）不会被跟随，也不会向那里发送任何内容：调用像以前一样以 `307` 或 `308` 失败，错误消息会说明未跟随重定向。`301`、`302` 和 `303` 不会被跟随。**行为变更：** 在同一源内重定向的服务器现在可以访问，而不再以 HTTP 307 或 308 失败。
- **MCP HTTP 服务器在响应没有正文的 `DELETE` 后关闭连接（#450）。** 官方客户端发送会话 `DELETE` 时不带 `Content-Length`。随附的 cpp-httplib 在 handler 运行后认为该请求有正文，于是保持连接打开等待读取正文，直到客户端关闭或读取超时，并且可能把该连接上的下一个请求当作正文读取；它在声明 `Connection: close` 的响应之后也保持连接打开。随附的 cpp-httplib 已更新到 0.61.0（#460），两处都已修复：没有 `Content-Length` 或 `Transfer-Encoding` 的请求没有需要排空的正文（RFC 9112 §6.3），带有 `Connection: close` 的响应会关闭连接（RFC 9112 §9.6）。**行为变更：** 仍基于随附 cpp-httplib 构建的两个 NeoGraph 服务器，即 A2A 服务器和 MCP Streamable HTTP 服务器（NeoGraph 的 HTTP 客户端使用 libcurl 和 Asio，不受影响），现在会在带有 `Connection: close` 的响应之后关闭连接，而 cpp-httplib 会给每个 `4xx` 和 `5xx` 响应加上这个头；遵守该头的客户端（包括 NeoGraph 自身）本来就会打开新连接。
- **gRPC：`RunGraphStream` 不再发送抛出的异常文本（#451）。** 节点抛出异常时，流会把引擎的 `ERROR` 事件原样作为 `DEBUG` 事件转发，因此异常的 `what()` 会先于经过净化的失败到达客户端，而 unary `RunGraph` 只发送公开消息。现在节点错误（包括其重试）是一个 `error` 为固定文本 `node failed` 的 `DEBUG` 事件（保留 `attempts`、`handled`、`routed_to`、`retry`、`max_retries` 和 `delay_ms`）；运行的失败事件（带公开消息的 `{error, code}`）和最终 status 不变。**行为变更：** 从流事件中读取异常文本的客户端在那里会得到 `node failed`。
- **gRPC：`RunGraphStream` 遵循 `stream_mode`，状态快照为 `VALUES`（#452）。** `RunGraphRequest.stream_mode` 被忽略，因此每个流都携带全部事件；完整状态快照（节点 `__state__`）以 `UPDATES` 发送，因此从未发送过 `VALUES`。现在按 `StreamMode` 的位应用模式（`EVENTS=1`、`TOKENS=2`、`VALUES=4`、`UPDATES=8`、`DEBUG=16`；0 与以前一样流式发送所有事件，未知位被忽略），`TOKEN` 事件只在带有 `TOKENS` 时发送，状态快照是 `VALUES` 事件；如 proto 现在所述，中断和节点错误仍是 `DEBUG` 事件。无论模式如何都会发送 `FINAL`。**行为变更：** 设置了 `stream_mode` 的请求只会收到所请求的事件，从 `UPDATES` 事件读取状态快照的客户端要改为在 `VALUES` 事件中查找。
- **gRPC `CheckpointService`：服务器无法完整存储的记录会被拒绝，而不是被截断（#453）。** 服务器在保存前把 `checkpoint_json` 解析为 `Checkpoint`，因此它不认识的顶层成员（由更新的引擎或其他实现写入）会被悄悄丢弃。现在 `SaveCheckpoint` 和 `PublishManagedBudgetCheckpoint` 会以 `INVALID_ARGUMENT`（`NG.InvalidRequest`；managed-budget 发布与其其他拒绝一样为 `FAILED_PRECONDITION`）拒绝这样的记录以及 `schema_version` 比服务器更新的记录，并且不存储任何内容。`channel_values` 和 `metadata` 中的自由 JSON 仍像以前一样保留。**行为变更：** 发送了额外顶层成员（以前会被丢弃）的客户端现在会收到错误。
- **A2A：以 JSON `null` 发送的可选成员视为不存在（#455）。** 消息解析把存在但为 null 的 `taskId` 或 `contextId` 读成空字符串，因此在 0.3 和 1.0 两种 dialect 中，`"taskId": null` 都返回 `-32001 Task not found`，`"contextId": null` 会以空的 context id 存储任务。配置成员也有同样的缺陷：`"blocking": null` 被读作 `false`，导致 send 立即返回 SUBMITTED 任务，`"historyLength": null` 被读作 0。现在对服务器和客户端而言，值为 null 的 `taskId`、`contextId`、`name`、`description`、`blocking`、`returnImmediately`、`historyLength` 或 `configuration` 都等同于不存在。这是 0.3 conformance kit 中 NeoGraph 失败而参考服务器通过的唯一一个 quality 测试（`test_null_optional_fields`）。**行为变更：** 把这些成员设为 `null` 的请求现在会像它们不存在一样成功。
- **A2A：TLS 终止代理之后的卡片 URL，以及不再有空的 `url`（#456）。** 当 `AgentCard::url` 为空时，`A2AServer` 会通告客户端所使用的地址，且总是使用 `http` scheme，因此在终止 TLS 的代理之后，卡片（及其 `supportedInterfaces`）会把 SDK 客户端引向明文 HTTP。现在 `A2AServer::set_trust_forwarded_headers(true)` 会从 RFC 7239 `Forwarded`（其第一个元素）获取 scheme 和 host，若没有则从 `X-Forwarded-Proto` / `X-Forwarded-Host` 获取；只接受 `http` / `https` 和合理的 host。由于客户端可以自行发送这些头，它默认关闭；已配置的 `AgentCard::url` 仍然优先。没有 `url` 的 `AgentCard` 的 0.3 JSON（例如在 `A2AServer` 之外为注册表写出的）不再包含 `"url": ""`：它使用首选 transport 的第一个 interface，若没有则省略该成员。**行为变更：** 这样的卡片不再带有空的 `url`，而是没有 `url` 成员。
- **ACP：`$/cancel_request` 会取消进行中的 `session/prompt`（#457）。** 服务器会为自己待处理的客户端请求发送 `$/cancel_request`，却忽略来自客户端的该通知，因此它所指向的 prompt 会一直运行到完成。现在 `requestId` 属于正在运行的 `session/prompt` 的 `$/cancel_request` 会像 `session/cancel` 那样取消该 turn，prompt 仍会收到一个 `stopReason: "cancelled"` 的响应；未知或已完成请求、或其他方法请求的 `requestId` 会被忽略。**行为变更：** 为 prompt 发送 `$/cancel_request` 的客户端现在得到 `cancelled`，而不是完成的 turn。
- **`WsClient` 有了握手期限和可选的读取期限（#458）。** 对于接受 TCP 连接却从不响应 Upgrade 请求的服务器，`ws_connect` 会永远等待。`WsClientOptions::handshake_timeout`（默认 30 秒）限制名称解析、连接、TLS 握手和 Upgrade 交换；`WsClientOptions::read_timeout`（默认 0，即无期限，因为等待服务器推送的空闲连接是正常的）限制一次 `recv()`。两者到期时都抛出 `asio::system_error(asio::error::timed_out)`；由于帧可能只读了一半，读取超时会关闭连接。负值会抛出 `std::invalid_argument`。**行为变更：** 使用默认选项的 `ws_connect` 会在 30 秒后失败，而不是永远等待。
- **Program 中没有超时的 Core 节点 attempt 等待其主体的开销减少了（#467）。** 自 #367 和 #368 起，Program 中的 Core 运行把每个节点主体放在节点 attempt 工作线程池上执行，而不是共享的 Program 调度器上。编排器为每个这样的 attempt 各自创建 strand、协程和 24 小时定时器来等待。没有超时的 attempt 既不会超时也不会被分离（detach），因此现在主体结束时只需一次 post 即可恢复编排器。每个节点少一次线程唤醒，在一台安静的 Arm 机器上每个节点约少 8 µs。#367 和 #368 的每节点开销大部分仍然存在：主体在另一个线程上运行，常常在另一个核心上，这使 `quickjs-performance` 门禁中 JavaScript 包装的 Core 运行慢了 9%，`structured_join_sequence` 慢了 12%。带超时的 `ng.await` 在 dispatch 之前把开始时间记录到命令日志（#391），因此还要再慢 11%，而崩溃恢复需要这条记录。
- **`json::parse` 拒绝数组和对象嵌套超过 256 层的文本（#480、#483）。** 复制 `neograph::json` 值以及比较两个值时，每一层嵌套都会递归一次，NeoGraph 内部遍历值的部分函数也是如此，而 `json::parse` 不限制深度，因此嵌套足够深的文本可能耗尽线程栈。现在，对于嵌套深度超过新常量 `json::max_parse_depth`（256，即 Program JSON 已有的上限）的文本，`json::parse` 会在复制值之前抛出 `json::parse_error`。在这个深度下，帧最大的函数也只需约 384 KB 栈，小于 macOS 辅助线程的 512 KiB。**行为变更：** 来自对端、文件和调用方、嵌套超过 256 层的文本不再能解析；信任此类文本的调用方可以传入更高的上限（#481）。`SqliteCheckpointStore`、`PostgresCheckpointStore`、`SqliteHarnessRecordStore`、`FileHarnessRecordStore`、`SqliteEvidenceLedger` 以及 NeoGraph 内部的复制会不限深度地读回 NeoGraph 写入的文本，因此包含更深值的检查点仍能加载。值不超过 256 个的文档跳过该检查，更大的文档会多遍历一次其值。
- **Python：转换包含自身的值或嵌套超过 256 层的值时会抛出异常，而不是让进程崩溃（#484）。** 绑定以递归方式把 Python 值转换为 `neograph::json`，既没有深度上限，也没有循环检查，因此包含自身的 dict 或 list 会一直递归直到栈耗尽。现在，嵌套超过 `json::max_parse_depth`（256）层的 dict、list 和 tuple 会抛出 `ValueError`，包含自身的值也会达到这一深度并抛出同样的异常。每个容器还会经过 Python 的递归检查，因此从 Python 调用栈深处开始的转换会抛出 `RecursionError`，而不是耗尽栈。
- **NeoGraph 内部遍历 `json` 值的递归函数在 `json::max_parse_depth` 处停止（#485）。** 以下函数每一层递归一次，且没有自己的上限：图往返检查，Program 编译器的条件、操作和配置字符串检查，Program 元数据差异，harness 日志脱敏（redaction），harness 主机配置过滤，MCP adoption 扫描，JSON Schema 的 `const`、`enum` 和 `uniqueItems` 相等比较，Decisions 客户端的值检查，以及 harness Program 转换器的操作检查。因此，未经过解析上限的值可能耗尽线程栈，例如在代码中构建的值、以更高上限解析的值，或从存储中读回的值。现在，当数组和对象嵌套超过 256 层时，每个函数都会抛出 `std::invalid_argument`，而不再继续递归。它会失败而不是提前返回，因此不会跳过脱敏或检查。Program 的 JSON Pointer 赋值在超过 256 个 token 时会像无效指针一样失败。上限以内的值行为不变。

## [0.14.1] - 2026-10-14

### 变更

- **Linux wheel 改用固定的镜像构建（#333）。** `manylinux_2_34` 镜像固定为构建 0.14.0 时的 digest，CI runner 也不再使用 `-latest` 标签。Linux x86_64 wheel 捆绑的库（包括 LGPL 的 `keyutils-libs` 和 `libxcrypt`）与 0.14.0 逐字节相同，因此已发布的库源码仍然适用。
- **需要重新构建 C++ 使用方：`RuntimeConfig` 新增了成员，`ProgramHandle` 新增了函数（#431）。** `RuntimeConfig` 新增 `core_provider_calls_journaled`，`ProgramHandle` 新增 `notices()`，`ProgramNotice` 为新增。请一起重新构建 NeoGraph、C++ 使用方与 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **在没有持久 provider 调用 journal 的宿主上，fork 会告知这一点（#431）。** fork 只从持久的 provider 调用 journal（`SQLiteProgramProviderCallJournal`，`LocalProgramHost` 在设置了 `sqlite_directory` 时有）重放暂停的调用所发出的 provider 调用。进程内的 broker 没有 journal，所以在 fork 中再次运行的节点会把先前的调用再发送一次、再计一次费，这一点没有变。**行为变化：** 当 `RuntimeConfig::core_provider_calls_journaled` 为 `false`（默认值），且暂停的那次尝试在暂停的操作里记录了已计价的 provider 调用时，`ProgramRuntime::fork` 返回的句柄现在带有一条代码为 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` 的 `ProgramNotice`（`ProgramHandle::notices()`，Python 中是 `handle.notices`）。它的 `witness` 列出这些调用（`call_id`、`node`、`model`、`microunits`）及其合计。通知不是错误，也不改变 fork 发送什么、计费多少。在暂停那一步中已完成的节点所发出的调用，会从该步保存的写入重放，所以通知无法说明列出的调用中哪些会在 fork 中再次运行。通知不随运行一起保存，因此 `reconnect()` 和 `resume()` 返回的句柄上没有。resolver 返回 `SQLiteProgramProviderCallJournal` 的 broker 的宿主应设置 `core_provider_calls_journaled = true`（`LocalProgramHost` 在有 `sqlite_directory` 时会设置）。已写入 Program 指南、Python 绑定指南和参考文档。

### 修复

- **Core 运行路径找回了 0.14.0 损失的大部分速度（#442）。** 0.14.0 加入 barrier 停滞判定、失败路由、重试尝试和 `any_of` 槽位后，普通图的每次运行比 #343 刚合并时慢了 14–19%，因此 0.14.0 中“Core 引擎速度已回到 v0.11.1 水平”这句话对发布的标签并不成立。现在每次运行只分配一次簿记数据，没有失败路由的图不再读取路由表，fan-out 把分支状态放在一个 vector 里，节点调用在一个可被 Asio 复用的协程帧中运行。在空闲机器上用 `bench_neograph 10000 5000 1` 交替测量 24 轮，中位数如下：在我们自己的 ARM 服务器上，顺序运行从每次 11.56 µs 降到 10.34 µs，并行运行从每次 24.90 µs 降到 23.04 µs（#343 刚合并时：10.16 µs 和 21.91 µs）；在一台 x86_64 机器（Core i7-8750H）上，从 7.68 µs 降到 6.78 µs，从 18.64 µs 降到 16.87 µs（#343 刚合并时：6.45 µs 和 15.91 µs）。与 #343 刚合并时相比仍有 2–7% 的差距。API 和行为不变。（发布时引用的数值在空闲机器上无法复现，因此在发布后更正。）
- **Program：超出金额、Core 步骤或 wall time 额度才结束的运行，会带着该项预算的记录结束 (#443)。** 用量在 plan 完成之后才结算，而被发现超出 Core 步骤、金额或 wall time 额度的运行，曾以 `BudgetExhausted` 结束且没有 `failure`，调用方无从知道是哪项预算、超出多少。超出 wall time 的运行也是如此：被截止时间停下的运行以 `TimedOut` 结束，它却以 `BudgetExhausted` 结束。现在，每一种超出预算的结束都有 failure 记录，witness 在该预算的键下含有 `granted`、`used` 和 `over_by`，与模型 token 的记录 (#420) 相同：金额是 `P_MONEY_BUDGET` 和 `failure.witness["monetary"]`，Core 步骤是 `P_CORE_STEP_BUDGET` 和 `failure.witness["core_steps"]`，wall time 是 `P_RUNTIME_TIMEOUT` 和 `failure.witness["wall_time_ms"]`。wall time 在两条路径上都以 `TimedOut` 结束。超出多项预算的运行会把每一项都列在 witness 里，status 和错误码取自 wall time、模型 token、金额、Core 步骤中排在最前的那一项。Core 引擎在 Core 步骤额度处停下的运行，以及被截止时间停下的运行，其记录也带上了 witness（停在额度处时 `over_by` 为 0）。**行为变更：** 超出金额或 Core 步骤额度才结束的运行现在有 `failure`；超出 wall time 额度才结束的运行以 `TimedOut` 而不是 `BudgetExhausted` 结束；超时的 failure 带有 `witness["wall_time_ms"]`。
- **挂在普通 `GraphEngine` 上的严格 profile，会拒绝工具循环的第二次 LLM 调用，而不是重发第一轮的上下文（#444）。** 挂上 `StrictRuntimeProfile` 后，一次 `engine.run()` 内部的工具循环（LLM 调用、工具分发、LLM 调用，`create_react_graph` 构建的就是这种结构）每次都用 epoch 固定的 RAW 范围来构建提示词，因为运行过程中没有任何环节把助手记录和工具记录追加到上下文 feed，也不会激活新的 epoch。工具结果只留在图状态里，到不了 provider，模型可能反复请求同一个工具，直到步数上限或预算让这次运行停下。设置了控制器的 `llm::Agent` 有同样的缺陷。严格路径由宿主逐步推进：宿主每次 provider 调用前，都要追加 RAW 记录，并激活新的 epoch。当 provider 调用到达的 epoch，其所属的一轮已经被图推进过，也就是所提交的消息里含有 epoch 首次分发之后新增的助手消息或工具消息，而这些消息又没有作为宿主指令或受信补充输入一并带上时，现在会抛出 `std::logic_error`，错误信息点明这一契约（节点不会重试；Python 中为 `RuntimeError`）。**行为变更：** 在严格 profile 下于一次运行内部循环、过去会发送过期上下文的图或智能体，现在会在第二次 provider 调用时失败。同一轮的重试、在 epoch 首次分发时就已含有这些消息的对话，以及把对话作为受信补充输入传入的内置节点（例如 plan executor），都不受影响。激活 epoch 即开始下一轮。同一次运行中切换到后续 epoch 时因回执冲突而失败的问题也一并修复：控制器每次激活都会把分发计数器清零，导致序号大于 1 的 epoch 下的第一次分发，沿用了该运行第一次分发的 id。现在这样的分发 id 为 `<run id>-epoch-<序号>-dispatch-<n>`，运行第一个 epoch 之下的 id 不变。严格路径指南记录了这一契约；NeoGraph 目前还没有在一次运行内部推进 epoch 的辅助函数。
- **文档：上下文管理、预算、放置位置与并发（#328、#326、#329、#330）。** 只改了文档和注释，行为没有变化。严格路径指南对比了 `messages` 通道、`history::compact_history`、记录 profile 和严格 profile（#328）。它还说明，上下文估算值（编码后请求体的字节数除以三）是针对整个编码后请求的启发式，不是被证明的上界（#326），并给出放置位置的指引：哪些 `ContextPlacement` 值会在各轮之间移动，稳定的制品应放在 `BeforeHistory`，以及在 `anthropic.messages`、`google.generate` 和 `google.interactions` 上，System 角色的制品（`RequiredSkill`、`HardConstraint`）不论放置位置如何，都会进入顶层的 system 字段（#329）。并发指南说明：上下文 feed 在追加冲突时失败即关闭，检查点存储按 `thread_id` 采用最后写入者获胜，对同一个 `thread_id` 的运行做串行化是宿主的职责，并且计划提供进程内的守卫（#330）。
- **由 Send 启动的执行返回 Send 时，运行会失败，而不是丢掉它们（#432）。** 由 Send 启动的节点返回的 Send 过去会被丢弃，其目标从未运行，也没有任何提示。现在运行会在应用该超步的任何结果之前失败，错误信息会指出节点和目标。**行为变更：** Send 目标返回 Send 的图过去会在不运行这些目标的情况下结束，现在会失败；请把条目写入通道，由普通节点发出 Send。
- **失败的 Program 尝试用与引擎追踪相同的名称记录 Send 执行（#433）。** 失败的 Core 会在没有引擎追踪的情况下重新抛出异常，因此失败的尝试根据 `NODE_END` 事件构建追踪，过去只用节点名记录 Send 执行。现在 Send 执行的 `NODE_END` 事件带有 `"send": true`，尝试的追踪按完成顺序把它记为 `<目标>[send]`。**行为变更：** 失败的 Program 尝试的追踪把 Send 执行记为 `<目标>[send]`，Send 执行的 `NODE_END` 事件多了 `send` 字段。
- **Python 也能关闭构建时的 lint（#415）。** `GraphEngine.compile` 新增关键字 `stderr_notices`，默认值为 `True`，与 C++ 的 `EngineConfig::stderr_notices` 相同。设为 `stderr_notices=False` 后，不再输出 strict 图的 lint（例如针对只由 `Send` 或 `Command.goto` 启动的节点的 E7），也不再输出 fan-out 串行执行的一次性提示；校验错误仍然会抛出。E7 本身没有变化：图定义里没有声明动态目标的地方，校验器无法区分由 Send 启动的节点和没有任何入口的节点，文档现已写明这一点。文档还说明，指向 Send 目标的静态边不能用来消除 lint：目标会在所有 Send 结束后，在共享状态上再多运行一次。不传这个关键字的代码行为不变，图的运行方式也不变。
- **明确了 `RunResult::execution_trace` 的契约（#424）。** 只改了文档和测试，行为没有变化。关于由 `Send` 启动的节点缺失于轨迹的报告，未能复现：`run`、`run_stream`、`run_async`、`run_stream_async` 和 `resume`，无论有没有 worker 线程池，都会把每次 Send 执行记为 `<目标>[send]`。`types.h`、`engine.h` 和文档现在写明了顺序：超步按先后排列，一个超步内先列就绪节点，再按发出顺序列 Send，与各次执行结束的先后无关。暂停的超步不记录任何内容，恢复后的运行会记录重新进入的整个超步，子图只算一项。
- **文档：通道的 `version` 是所有通道共用的同一个写入计数器的值（#416）。** `RunResult` 的文档、头文件和指南曾把 `output["channels"][name]["version"]` 说成该通道被写入的次数，把 `output["global_version"]` 说成超步计数器，Python 的 `global_version()` docstring 则说它是各通道版本之和。实际上它们是同一个计数器的值：向任何通道写入（包括运行输入的写入）都会让它加 1。通道的版本是该通道最后一次写入时的计数器，`global_version` 是运行最后一次写入之后的计数器。版本随每次写入递增，任意两个通道不会有相同的版本，但两者都既不是写入次数，也不是超步序号。从未写入过的通道，版本为 0，即使它有 `initial` 值也一样。检查点保存这两个数（引擎不设置 `Checkpoint::channel_versions`），恢复或 fork 出来的运行接着这些值继续计数。数值没有变化，测试把它们固定了下来（`ChannelVersions.*`、`test_channel_versions.py`）。

## [0.14.0] - 2026-10-13

### 变更

- **需要重新构建 C++ 使用方：task graph 的类型新增了成员（#412）。** `TaskGraphProposalLimits` 新增 `min_schema_version`、`max_attempts`、`max_literal_bytes` 和 `max_total_literal_bytes`，嵌入它的 `TaskGraphProposalOptions`、`TaskGraphFragmentCompileOptions` 和 `TaskGraphExpansionPolicy` 的布局随之改变；`TaskGraphTask` 新增 `literals` 与 `max_attempts`，`CompiledTaskGraphTask` 新增 `input_literals` 与 `max_attempts`，`TaskGraphInputLiteral` 为新增类型。`TaskGraphProposal::SCHEMA_VERSION_2` 与 `CompiledTaskGraphFragment::SCHEMA_VERSION_2` 为新增，`TaskGraphProposal::schema_version()` 返回文档自身的版本。请一起重新构建 NeoGraph、C++ 使用方与 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **需要重新构建 C++ 使用方：工具、Hook、MCP、gRPC 与图类型新增了成员。** `RunResources` 新增 `hook_runtime`，`InvocationResult` 新增 `failure`，`MCPTool` 新增基类 `ContextualAsyncTool` 与 `execute_result_async(arguments, context)`，`MCPClient` 新增受约束的 `call_tool_result_async`，`MandatoryHookRunner` 新增 `blocks` 与一个 private 成员函数，`GrpcRemoteTool` 新增基类 `ContextualAsyncTool` 与 `set_call_timeout`，`GrpcCheckpointStore` 新增 `set_rpc_timeout`；`graph::FailureVerdict`、`graph::classify_failure`、`ToolExecutionError`、`grpc::make_tool_service` 与 `grpc::make_checkpoint_service` 为新增。`proto/neograph.proto` 以新的字段编号新增了 `ToolCallResponse.error_code`，因此传输保持兼容。请一起重新构建 NeoGraph、C++ 使用方与 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **工具 Hook 在每条工具路径上运行。** 引擎的 `HookRuntime`（`EngineConfig::hook_runtime`、`GraphEngine::set_hook_runtime`）过去只发布检查点 Hook；现在它也像 `llm::Agent::set_hook_runtime` 对 Agent 所做的那样，运行其节点发起的每个工具调用（`tool_dispatch`、plan-and-execute 的 executor、deep-research 的 researcher、使用 `make_tool_execution_context` 的主机节点）的 `BeforeToolExecution` 与 `AfterToolExecution` Hook。`RunResources::hook_runtime` 会为一次运行替换它，子图的工具调用使用父运行的，Program Core 的工具调用使用 Program 主机的 `RuntimeConfig::hook_runtime`。**行为变化：** Hook registry 接纳了工具阶段 Hook 的主机现在也会看到图与 Program 的工具调用，强制 Hook 可以阻断它们；无法运行的 Hook 边界（没有 owner scope 的运行）在检查点、Provider 与 Program 事件上也会以 state 为 `failed` 的 `HookBoundaryBlocked` 关闭式失败，而不是抛出 `std::invalid_argument`。
- **Hook 阻断以 `HookBoundaryBlocked` 离开图运行。** 图运行过去抛出原因为 `HookBoundaryBlocked` 的 `NodeExecutionError`，而 `llm::Agent` 直接抛出它；现在二者都会抛出 `HookBoundaryBlocked` 本身及其 invocation id 与 state，无论它位于原因链何处，节点也不会为此发出 `ERROR` 事件。在失败原因链中发现的取消同样以 `CancelledException` 离开。Python 早已看到 `ng.HookBoundaryBlocked`；该异常不再带有 `node_name` 与 `attempts` 属性。
- **Hook 只在超时时报告 `timed_out`。** 最后一次尝试失败的强制 Hook 过去被记录并报告为 `timed_out`；现在它在 `InMemoryHookJournal`、`SQLiteHookJournal` 与 `HookBoundaryBlocked::state()` 中都是 `failed`，耗尽的失败也不再列入 `pending()`。截止时间已过、或其原生目标超时的 Hook（RPC Hook 早已如此）报告 `timed_out`，包括其租约越过截止时间的情况。
- **运行截止时间之后不再启动任何工具调用。** 在每条分发路径上，将在 `RunContext::deadline`（`ToolExecutionContext::deadline`）当时或之后开始的调用都是状态为 `expired`、错误为 `run deadline elapsed before the tool started` 的工具结果，且不运行其 Hook。过去在未配置 Hook 时它会照常运行，配置了 Hook 时则以 `timed_out` 的 `HookBoundaryBlocked` 停止运行。截止时间也约束准入队列中的等待。
- **MCP 与 gRPC 服务器区分保留失败类别，且不发送异常文本。** `MCPServer` 对 Hook 阻断以 JSON-RPC 错误 `-32010`、对中断以 `-32011` 应答，并把其他所有失败的稳定代码放进 `isError` 结果的 `_meta["io.neograph/failure"]`。客户端取消的调用按 MCP 的要求在任何传输上都不应答（应答 sink 的 `NoResponseSink` 收到其 id）；被 `stop()` 取消的调用返回 `-32000`。gRPC 使用标准状态码：失败的 `RunGraph` 或 `RunGraphStream` 以该类别的状态（`UNKNOWN`、`DEADLINE_EXCEEDED`、`UNAVAILABLE`、`RESOURCE_EXHAUSTED`、`FAILED_PRECONDITION`、`INTERNAL`、`ABORTED`、`CANCELLED`）以及 `ErrorInfo.reason` 为稳定代码的 `google.rpc.Status` 结束；无效请求在任何东西运行之前即为 `INVALID_ARGUMENT`（没有图时为 `FAILED_PRECONDITION`）；`ToolService` 把工具自身的失败连同 `error_code` 放在 payload 中，对运行控制信号以状态应答，对未知工具以 `NOT_FOUND`、对格式错误的参数以 `INVALID_ARGUMENT` 应答。所有消息都是公开消息（`NodeFailureError::public_message` 或类别描述），绝不是 `what()`。**行为变化：** 通过 gRPC 失败的图运行过去是设置了 `error` 的状态 `OK`，失败的流以 `OK` 结束；`RunGraphResponse.error` 已弃用并保留一个版本。`MCPServer` 的工具错误过去带有异常文本，被取消或停止的调用过去是 `isError` 结果。参见[协议失败映射](docs/reference-en.md#protocol-failure-mapping)。
- **`ToolExecutionController::execute_async` 抛出带类型的结果。** 这个抛出异常的门面过去对 `timed_out`、`expired` 与 `rejected` 抛出 `asio::system_error`，对其他所有失败抛出 `std::runtime_error`；现在它抛出携带完整 `ToolExecutionResult` 的 `ToolExecutionError`，对取消状态仍抛出 `CancelledException`。节点失败分类器把其中不确定的效果视为 `NG.EffectUncertain`，因此节点重试策略不再重新运行这类调用，并把超时或过期的调用视为 `NG.Timeout`。`timed_out` 的 asio 或 `std::system_error` 现在也是 `NG.Timeout` 而不是 `NG.TaskFailed`；二者仍会被重试且可路由。
- **需要重新构建 C++ 使用方代码：A2A 和 ACP 的布局新增了成员。** `a2a::AgentCard` 新增 `skills`、`security_schemes`、`security_requirements` 和 `extensions`，`a2a::GraphAgentAdapter` 新增虚函数 `build_resume_value`，`a2a::A2AClient` 新增 endpoint 策略状态。`acp::ClientCapabilities` 新增 `elicitation`，`acp::ACPClient` 新增成员，`acp::ACPGraphAdapter` 新增虚函数。请一起重新构建 NeoGraph、C++ 使用方代码和 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **A2A：1.0（v1.0.1）成为主版本，native server 遵循其 task 规则。** 0.3 作为兼容 dialect 保留。行为变更：task id 由 server 生成，因此指定未知 `taskId` 的 message 不再创建该 task，而是以 TaskNotFoundError（`-32001`）失败（NeoGraph collaboration envelope 保留发送方指定的 id）；发给 terminal task 的 message 以 `-32004` 失败，发给运行中 task 的以 `-32603`（ErrorInfo `TASK_BUSY`）失败，发给等待 input 的 Program task 的以 `-32004` 失败，因为尚不支持通过 A2A 回答 Program 的 input 请求；对 terminal task 的 `CancelTask` 以 TaskNotCancelableError（`-32002`）失败且 task 不变；in-flight 上限以 `-32603`（ErrorInfo `RESOURCE_EXHAUSTED`）应答，而不是 `REJECTED` task；`A2A-Version` 只接受 1.0 和 0.3；`AgentCard::streaming` 默认值改为 `true`，`streaming = false` 的 card 拒绝 streaming method。card 按版本提供：`A2A-Version: 1.0` 得到纯 1.0 card，不带版本的请求得到 0.3 card（仍首先列出 1.0 interface）。task 的 history 以用户 message 开头。在 Program 路径上，缺少 credential 时得到带 `WWW-Authenticate` 的 HTTP 401，无权的 collaboration peer 得到 HTTP 403，而不是 `AUTH_REQUIRED` task；card 声明 Bearer security scheme。
- **A2A client：第一次调用获取 AgentCard 并遵循所选 interface。** 行为变更：未获取 card 的 client 过去先探测 0.3。现在它在第一次调用前获取 card，只有无法获取时才从 1.0 开始探测，遇到 `-32601`、`-32009` 或 HTTP 400 时改用 0.3。所选 interface URL 与 `base_url` 同源时，RPC 发往该 URL；`set_allow_cross_origin_interfaces(true)` 允许 card 指定其他源（未允许时 client 保持 `base_url` 并向 stderr 写警告），`endpoint_url()` 返回正在使用的 endpoint。message id 为随机 UUID。`A2ACallerNode` 只在 task 等待输入时保留 `<output_key>_task_id`，并始终发送 context id，因此以 terminal 结束的轮次之后会在同一对话中开始新的 task。
- **ACP：失败的 prompt turn 是 JSON-RPC error，异常文本不会到达 client。** 失败的 turn 是携带 public message 和 `data: {code, retryable}` 的 `-32603`（过去是一个包含异常文本的 chunk，随后是 `stopReason: end_turn`）；provider 拒绝以 `refusal` 结束，超出模型 token 预算以 `max_tokens` 结束，step 上限以 `max_turn_requests` 结束（没有空 chunk），client 未请求的取消为 `-32800`。
- **ACP：NodeInterrupt 在同一 turn 中通过 `session/request_permission` 询问**，并以所选 option 从 checkpoint 恢复（过去是带 `_meta` 的 chunk 和 `end_turn`，由下一个 prompt 作答）；可通过 `ACPGraphAdapter::interrupt_permission` 和 `interrupt_resume_value` 定制。
- **ACP error code 遵循 v1 schema：** busy 和 overload 的 turn 为带 `data.reason` 的 `-32603`（不再使用 ACP 保留给 "authentication required" 的 `-32000`），`initialize` 之前的调用为 `-32600`，未知的 `session/resume` 为 `-32002`。
- **ACP 取消：** 没有进行中 turn 时的 cancel 被忽略（`set_pre_armed_cancel` 可恢复旧的预先取消）；挂起的 agent→client 请求以 `$/cancel_request` 放弃；permission 请求没有硬性 timeout（`set_permission_timeout`）；stdin EOF 会取消进行中的 turn（`set_eof_grace`）。
- **ACP prompt content 与 session context：** `resource_link` 和已声明的 embedded resource 会到达 graph；格式错误或未声明的 content block 为 `-32602`；`_acp_cwd` 和 `_acp_additional_directories` 来自 `build_session_state`；`terminal/*` 和 `elicitation/*` 需要 client capability；`agentInfo` 始终是有效的 Implementation。
- **需要重新构建 C++ 使用方：`HookRuntime` 新增了成员。** `HookRuntime` 新增 `compose`、`emit_once_async` 与两个私有成员，`MandatoryHookRunner` 新增一个私有成员函数；`graph::RemoteControlFailure` 与 `tool_call_not_started` 为新增。请一起重新构建 NeoGraph、C++ 使用方与 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **子图的工具调用会经过两个 Hook 运行时。** 当子图的引擎与其父运行都有 `HookRuntime` 时，其工具调用会经过两者，先是父运行的 Hook，再是子图的（`HookRuntime::compose`），任一方阻断都会阻断该调用。**行为变更：**过去父运行的运行时会替换子图引擎的运行时，子图的工具 Hook 因此从不运行。
- **工具 Hook 事件具有持久身份，并且只交付一次。** 在图运行中，以及调用具有任务槽位（`ToolExecutionContext::effect_task_id`）的任何地方，`BeforeToolExecution` 与 `AfterToolExecution` 事件都携带 `task_id` 与 `call_ordinal`，即调用的效果身份（运行、线程、包含节点尝试的任务与调用序号），`request_id` 也由它派生，而不再来自进程内计数器。再次运行某个 batch 的恢复或重放不会再次交付已为该身份交付过的事件；被 Hook 阻断的那次发出不算交付，会再次运行。`llm::Agent` 为每一轮的 batch 分配各自的任务槽位。**行为变更：**过去恢复后的 batch 会再次运行其调用的所有工具 Hook。
- **gRPC `ErrorInfo.reason` 遵循 AIP-193。** `reason` 现在是由稳定代码派生的 UPPER_SNAKE 常量（`HOOK_BLOCKED`、`TIMEOUT`、`TOOL_NOT_FOUND`、`INVALID_REQUEST`、`NO_GRAPH`、`CAPACITY`、`BACKEND_UNAVAILABLE`、`BARRIER_STALLED` 等；作者代码取其类别的 reason），`NG.X` 代码本身移到了 `ErrorInfo.metadata["code"]`；domain 仍为 `neograph`。**行为变更：**过去 `reason` 是 `NG.X` 代码；读取它的客户端应改为读取 `metadata["code"]`。NeoGraph 自己的客户端两者都接受。
- **远程 Hook 阻断或中断是带类型的工具失败。** 当 `MCPTool` 收到 JSON-RPC 错误 `-32010`（服务器的 Hook 阻断了调用）或 `-32011`（服务器的运行需要输入），或 `GrpcRemoteTool` 收到 `ErrorInfo` 代码为 `NG.HookBlocked` 或 `NG.Interrupted` 的状态时，会抛出 `graph::RemoteControlFailure`：这是一个携带代码 `NG.HookBlocked` 或 `NG.Interrupted` 与远程公开消息、不会重试的普通失败。它既不停止也不暂停本地运行，本地运行会带着失败的工具结果继续；远程中断的原生传递属于后续阶段。**行为变更：**过去这些是无类型的工具失败（`NG.TaskFailed`），超时的远程 Hook 阻断（`DEADLINE_EXCEEDED`）看起来像调用自身的截止时间。
- **需要重新构建 C++ 使用方：MCP 与 async 类型新增了成员。** `MCPError` 新增 `http_status()`，`StdioClientConfig` 新增 `cancel_grace`，`MCPHttpServerConfig` 新增 `max_http_threads`、`session_idle_timeout` 与 `protected_resource_metadata`，`MCPServer` 新增 `protocol_version()`、`NoResponseSink` 类型以及接受它的 `set_response_sink` 重载，`MCPClient` 新增接受截止时间与取消令牌的 `initialize` 和 `initialize_async` 重载以及 `set_tool_list_changed_handler`，`MCPTool` 新增一个私有成员函数，`SseEventParser` 新增 `last_event_id()`、`retry_ms()` 与私有成员；`async::async_request_stream`、`async::HttpResponseHead`、`mcp::MCP_SUPPORTED_PROTOCOL_VERSIONS`、`mcp::is_supported_protocol_version`、`MCPServer::Reply` 以及接受它的 `MCPServer::handle_message` 重载为新增。`ToolExecutionController` 新增私有成员函数。请一起重新构建 NeoGraph、C++ 使用方与 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **MCP 客户端按原样使用其 URL。** `MCPClient(url)`、`MCPTool(url, ...)` 和 Python 的 `mcp.MCPClient(url)` 按 MCP Streamable HTTP 传输的要求，把每个请求发送到配置的 URL，包括路径和查询。**行为变化：** 过去除非路径已以 `/mcp` 结尾，否则会附加 `/mcp`（`http://host:8000` 变成 `http://host:8000/mcp`，`/v1/rpc` 变成 `/v1/rpc/mcp`），并拒绝查询字符串；请传入服务器完整的 MCP 端点 URL，例如 `http://host:8000/mcp`。示例的默认值现在也采用这种形式。
- **MCP 服务器也应答 2025-06-18、2025-03-26 和 2024-11-05；客户端只接受已知的修订版。** `MCPServer` 像官方 SDK 服务器一样，在客户端请求的修订版为 `2025-11-25`、`2025-06-18`、`2025-03-26` 或 `2024-11-05` 时以该修订版协商，否则以 `2025-11-25` 协商，并只向旧修订版的客户端发送该修订版定义的字段（`2025-06-18` 没有 `icons` 和 `execution`；`2025-03-26` 还没有 `title`、`outputSchema`、`_meta`、`structuredContent` 和 `resource_link` 内容，只有结构化数据的结果以文本携带这些数据；`2024-11-05` 还没有 `annotations` 和音频内容；该修订版没有的内容块以携带其 JSON 的文本块发送）。HTTP 传输将 `MCP-Protocol-Version` 与会话的修订版对照，缺少该头时使用会话的修订版。NeoGraph 的 MCP 客户端通过 stdio 和 Streamable HTTP 接受同样的四种答复（`MCP_SUPPORTED_PROTOCOL_VERSIONS`），不支持更早的 HTTP+SSE 传输。**行为变化：** 过去服务器对所有客户端都答复 `2025-11-25`，会话中缺少该头时返回 400；客户端过去接受任何 `protocolVersion`，例如 `1999-01-01`，现在对不在列表中的版本会以协议错误使 `initialize` 失败并断开连接。尚不提供或使用 MCP `2026-07-28`，计划将其作为第二种模式。
- **MCP 工具注册拒绝官方客户端会拒绝的内容。** `MCPServer::register_tool` 和 `register_raw_tool` 要求 `inputSchema`（如有，`outputSchema` 也一样）声明 `"type": "object"`，并且是本验证器支持的 JSON Schema 2020-12；名称须为 1 到 128 个 `A-Za-z0-9_.-` 字符（SEP-986）。`ToolDefinition::input_schema` 的默认值现在是 `{"type": "object"}`，空对象按该默认值读取。**行为变化：** 过去这类注册会成功；一个未设置模式就注册的工具会让官方客户端拒绝整个 `tools/list`。
- **未知的 MCP 工具是协议错误。** 对服务器上不存在的工具调用 `tools/call`，按 MCP 工具的“Error Handling”的要求，返回 `data.code` 为 `NG.ToolNotFound` 的 JSON-RPC 错误 `-32602` `Unknown tool: <name>`。**行为变化：** 过去返回 `isError` 结果。
- **MCP HTTP 会话会过期并腾出空位，被取消的调用不再得到应答。** `MCPHttpServer` 关闭 `session_idle_timeout`（30 分钟）内没有请求的会话；新客户端在 `max_sessions` 时 initialize 时，关闭没有进行中请求、最久未使用的会话。已关闭会话的 id 返回 404，告诉客户端重新 initialize。只有在所有会话都忙时，initialize 才会以 503 和 `Retry-After` 被拒绝。**行为变化：** 过去会话一直存活到 `DELETE`，因此在 64 个不在 `close()` 时发送 `DELETE` 的官方 TypeScript 客户端之后，每个新客户端都会永久得到 503。客户端已取消的调用现在以空的 `text/event-stream` 结束其 POST，而不是 JSON-RPC 错误 `-32800`；所有官方 TypeScript 客户端都把 `-32800` 报告为对未知请求的应答。`MCPServer` 的应答 sink 也不再收到 `-32800`，与 `NoResponseSink` 一起设置的 sink 改为收到该调用的 id。
- **加固的 MCP stdio 会话在有 stderr 输出时继续运行、有序停止，并在终止前通知服务器。** 加固会话（`StdioClientConfig`）持续读取服务器的 stderr 并保留最后 `max_stderr_bytes`；被取消或超时的请求会发送 `notifications/cancelled`，等待 `cancel_grace`（500 ms），然后终止服务器；关闭任何 stdio 服务器时都先关闭其 stdin，在 SIGTERM 前最多等待一秒，在 SIGKILL 前再最多等待一秒。**行为变化：** 过去加固会话在 stderr 写满 64 KiB 后就结束，被取消或超时时会立即杀死服务器而不通知它；每次 stdio 关闭都以 20 ms 间隔发送 SIGTERM 和 SIGKILL。
- **MCP 客户端在任何请求之前先握手，并监听其 HTTP 会话。** 尚未初始化的客户端上的 `MCPClient::rpc_call_async` 会先执行握手，因此基于 MCP 的 hook RPC 传输不再在 `initialize` 之前发送请求。`initialize` 之后，HTTP 客户端像官方客户端一样打开会话的可选 `GET` 事件流，接收不属于任何调用的服务器请求与通知（FastMCP v1 把 ping 发到那里）。**行为变化：** 每个 HTTP 会话会多保持一个打开的连接；没有该流的服务器返回 405，客户端随后不再请求。
- **需要重新构建 C++ 使用方：Program provider 调用结构体新增了成员，被计价的调用会执行 `monetary_microunits` (#372)。** `ProgramResultData` 新增 `monetary_records`，`ProgramCoreProviderCallContext` 新增 `monetary_account`，`SQLiteProgramProviderCallJournal::Record` 新增 `monetary_record`，`Provider` 新增静态函数 `invocation_upper_bound`。采用 SchemaProvider `f7ed816` 后，`sp::OpenRouterRouting` 新增 `max_price`，`ProviderControls`、`SchemaProvider::Defaults` 以及 SDK 的 Chat、Messages、Responses 请求也随之变大，接口修订号仍为 6。请用匹配的 SDK 一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。provider journal 会给已有数据库添加 `monetary_record` 列。存储的 `ProgramResult` 只在有调用被计价时才带 `monetary_records`，因此其他结果的字节与 id 不变。**行为变更：** 与所有 `RunBudget` 数量一样，`monetary_microunits = 0` 不授予任何额度，因此金额预算为零的运行会以 `P_MONEY_BUDGET` 拒绝计价 broker 计价的每个调用；此前没有任何东西收取金额。
- **需要重新构建 C++ 使用方：中断标识增加了成员 (#388)。** `RunResult` 新增 `interrupt_id` 和 `interrupts`，`RunMetadata` 新增 `resume_interrupt_id`，`ProgramPendingInputData` 和 `ProgramPendingEffectData` 新增 `core_interrupt_id`，`graph/types.h` 中新增 `PendingInterrupt`。请一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **Core：单个 `Send` 也像多个时一样只把 payload 留给自己 (#389)。** 行为变更：某一步中唯一的 `Send` 的 payload 不再进入共享 run 状态、下一步的节点或 run 的最终状态；它只对该 Send 自身的执行可见，就像多个 Send 的步骤中每个 payload 早已如此。在 Send 步骤之后读取单个 Send 的 payload 的图，必须改为把该值作为 write 返回。本仓库中没有示例或测试依赖旧行为。
- **Core：恢复值不再到达后续步骤 (#393)。** 行为变更：在恢复后的 run 的后续超步中读取 `ctx.resume_value`（Python 的 `input.ctx.resume_value`，以及 tool gate 中的 `ToolGateContext.resume_value`）的节点现在看不到该值；`interrupt_after` 暂停之后的恢复、从步骤检查点或 safe point 的恢复中的所有节点也是如此。该值只交给恢复所回答的调用：恢复所回答的 `NodeInterrupt` 的节点或 `Send`，或 `interrupt_before` 闸门拦下的节点。带有 `messages` 通道的图仍会在其中以用户消息的形式收到该值。本仓库中没有示例或测试依赖旧行为。
- **Core：无论提问兄弟节点的索引如何，失败都会结束其所在步骤 (#392)。** 行为变更：一个兄弟节点抛出 `NodeInterrupt`、另一个失败的 fan-out 步骤或 Send 步骤，现在无论顺序如何都会使 run 失败；此前，索引较小的中断会使 run 暂停并丢弃该失败。取消现在无论顺序如何都会停止这样的步骤。对账（reconciliation）中断在失败旁仍会暂停，此时检查点即使只有它一个也会在 `_neograph.interrupts` 中列出；其他检查点保持字节不变。
- **Core：步骤中的每个 `interrupt_before` 闸门单独提问 (#396)。** 行为变更：当一个超步中有多个带闸门节点就绪时，恢复回答 run 所停的闸门，在所有闸门都被回答之前，run 会在该步每个其他带闸门节点之前再次暂停；此前一次恢复会运行所有节点。该步后续闸门的 Before 检查点会在 `_neograph.answered_gates` 中列出已回答的闸门；只有一个闸门的步骤保持检查点字节不变。
- **Program：join 在失败成员旁为对账而暂停 (#397)。** 行为变更：在 JavaScript `ng.all`（fail-fast）、`ng.race`、`ng.quorum` 以及计划级 `parallel` 和 `quorum` 中，因不确定效果（Core 的 `tool_effect_reconciliation` 中断）而暂停的成员，现在会在成员的失败本将结束 join 时使 join 暂停，恢复后由该失败结束 join；此前失败会立即结束 join。由成功决定的 join 不变。
- **Program：JavaScript `main()` 的 fork 继续其暂停的命令 (#395)。** 行为变更：fork 以源运行的输入运行 `main()`，其运行记录保存该输入而不是 fork 的输入；源运行未在 `callCore` 中暂停的 fork 会以 `P_FORK_STATE` 被拒绝，而不是从头重新开始 `main()`。
- **Program：fork 或图迁移会重新绑定其携带的 provider 证据 (#398)。** 行为变更：fork 或图迁移带入新运行的、带有 provider 证据的已记录失败，会在源运行的保管（custody）下恢复，并在新运行的保管下重新存储；与源运行保管不匹配的记录会在任何内容发布之前以 `P_FORK_CUSTODY` 拒绝该 fork。
- **Program：计划 quorum 会计入 Core 节点失败的分支 (#399)。** 行为变更：这样的分支是 quorum 的失败分支，quorum 继续进行并按自身规则结束（达成、`P_QUORUM_UNREACHABLE`，或被后续分支暂停）；此前节点失败会立即使运行失败。
- **Program：恢复的 join 会等待它所答复的成员 (#400)。** 行为变更：在恢复所答复的成员运行期间，被重放成员的完成本应作出的决定（fail-fast 失败、race 的第一个完成者、已达成或无法达成的 quorum）会被暂缓，且不会启动其他成员；该决定在被答复的成员结束时作出。
- **Program：暂停的 join 先询问对账 (#401)。** 行为变更：无失败而暂停的 JavaScript join 或计划级 `parallel`，现在无论声明顺序如何，都会先在第一个因不确定效果而暂停的成员上暂停，再轮到因普通问题而暂停的成员。收集式 `ng.all` 在失败旁仍会在暂停的成员上暂停，这现在被记录为 #392/#397 优先级的有意例外。
- **C++ 使用方须重新构建：图和 Program 结构体增加了 member (#357, #358, #361, #365, #367)。** `EngineConfig` 增加了 `validation` 和 `stderr_notices`，`RunMetadata` 和 `RunContext` 增加了 `shared_scheduler`，`ProgramCompilerConfig` 增加了 `require_acyclic`，`ProgramCompilerConfig` 和 `ValidationOptions` 增加了 `soundness_check` 和 `soundness_state_cap`，`GraphEngine` 增加了一个 private member，`RunStatus` 增加了 `BarrierStalled`。#364 使 `EngineConfig` 增加了 `failure_text_redactor`，`RunMetadata` 增加了 `node_retry_ceiling`，`RunBudget` 增加了 `max_node_retries`，`ProgramUsage` 增加了 `node_retries`，`CatalogConfig` 增加了 `failure_text_redactor`，`Scheduler`、`NodeExecutor` 和 `GraphEngine` 增加了成员，`RunStatus` 增加了 `FailureExit`，`ProgramDiagnosticError`、`ProgramCompileError` 和 `ProgramAdmissionError` 增加了基类 `graph::ConfigurationFailure`。这些改变了 layout：NeoGraph、C++ 使用方和 Python 扩展须一起重新构建（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。列出所有值的 `RunStatus` `switch` 需要 `BarrierStalled` 和 `FailureExit` 分支。
- **C++ 使用方须重新构建：尝试超时改变了图的布局，节点主体获得尝试令牌 (#364, #367, #368)。** `NodeRetryPolicySpec` 增加了 `attempt_timeout_ms`，`NodeExecutor` 增加了成员，`GraphEngine` 增加了成员和一个声明的析构函数；请一起重新构建 NeoGraph、C++ 使用方和 Python 扩展（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。**行为变更：**在 Program 运行中，以及对每个带超时的尝试，节点主体在工作线程池上运行，而不是在驱动运行的协程上运行，其 `ctx.cancel_token` 是属于该尝试的、运行令牌的子令牌：取消运行仍会取消它，但取消它只会取消该尝试。因此在 Program 运行中，并行步骤的节点主体会重叠执行，其 `NODE_END` 事件按完成顺序到达。不使用 `attempt_timeout_ms` 的定义保持其字节和 id。
- **Program：失败的节点报告分类后的代码 (#364)。** 没有 failure edge 的节点失败会使 Program 运行以节点失败分类器的代码（`NG.TaskFailed`、`app.compile_error` 这样的作者代码、`NG.ProviderFailed` 等）而不是 `P_RUNTIME_CORE_FAILURE` 结束为 `Failed`，`failure.witness.node_failure` 中保存 `{node, code, kind, attempts, retryable, routable}`。`P_RUNTIME_CORE_FAILURE` 只保留给没有节点失败记录的 Core 失败。编排计划的 `retry` 操作也询问同一个分类器：在逻辑、配置、预算或 Provider 失败、已提交的效果或已达到的重试上限之后，不再重新运行其主体。
- **节点重试与工具失败遵循同一个失败分类器 (#364)。** 节点重试循环和工具失败契约都询问同一个分类器。分类器会遍历整条原因链（`NodeExecutionError` 的原因、`std::nested_exception`、子图失败），并采用找到的最严重分类。仍会重试的：`std::runtime_error` 及其子类、取消之外的 asio 错误，以及根因为上述之一的子图失败。不再重试的：节点内部抛出的 Program 诊断（C++ 或 Python 的 `ProgramDiagnosticError`、`ProgramCompileError`、`ProgramAdmissionError`），它们现在通过 `graph::ConfigurationFailure` 标记成为 `NG.ConfigError`；再次运行该节点会再次得到同一诊断。不再重试的：根因为 `std::logic_error` 或未知异常的子图失败、`std::runtime_error` 与 `std::logic_error` 体系之外的任何异常（包括 `throw 42`），以及位于 `std::nested_exception` 之后的配额失败。工具原因链中任意位置的运行控制信号（例如工具所运行的图的失败中包含的 Hook 阻断）现在会停止运行，而不再变成工具消息。参见[节点失败契约](docs/reference-en.md#node-failure-contract)。
- **节点重试的每次尝试都有自己的效果命名空间 (#364)。** 尝试 0 沿用任务 id（`s3:act`），因此现有日志的槽位保持不变；尝试 k 使用 `s3:act#a<k>`。因此重试会向 `ProviderCallBroker` 请求新的调用槽位而不是第一次尝试的槽位，被重试节点所运行的子图也会在新的子线程中开始。尝试 k 开始之前，一条以其命名空间为键的空待写记录会记下这次开始，因此恢复会继续该尝试并保留剩余的重试次数，而不是从尝试 0 重新开始。在 `ToolEffectBroker` 下，提交了非幂等效果的尝试绝不会被重试，并以 `NG.EffectCommitted` 失败；结果未知的效果会暂停运行以便核对，即使节点把该中断变成了错误也是如此。
- **示例 11（Clay 聊天机器人）已重新设计。** 文本使用系统 TrueType/OpenType 字体并按 UTF-8 测量，因此韩文等文字都能显示（可用 `NEOGRAPH_CHAT_FONT`、`NEOGRAPH_CHAT_BOLD_FONT`、`NEOGRAPH_CHAT_MONO_FONT` 覆盖选择）。窗口包含显示模型与状态的标题栏、头像、带代码块的轻量 Markdown、带建议的欢迎卡片、跟随新输出的滚动条，以及在回复流式输出期间 Send 按钮变为 Stop 的输入框；Stop 和退出都会取消回复。
- **采用 SchemaProvider `maint/interface6` 上的 SDK 0.3.1，提交 `69f8aba5c75decd21d786853de134ac713634966`，纳入 SchemaProvider 的 #26 至 #29。** interface revision 与 shared-library generation 仍为 6，公开的结构体和 enum 布局不变。NeoGraph 在构建以及安装后的 `NeoGraphConfig` 的 `find_dependency` 中都要求 `SchemaProvider 0.3.1 EXACT`，已安装的 0.3.0 包因此不再匹配。请用 0.3.1 的头文件和库，一起重新构建 NeoGraph、C++ 使用方和 Python 扩展。变化如下：直连 OpenAI 的 Chat（`https://api.openai.com`）把输出上限写成 `max_completion_tokens`，因为当前的 OpenAI 模型会以 HTTP 400 拒绝 `max_tokens`；OpenRouter 和兼容 endpoint 仍用 `max_tokens`，显式设置的 `bindings.max_output_tokens` 在任何 origin 上都优先。厂商的 400 响应点名了被拒绝的参数时，`Error::safe_message` 里也会给出该名称（`invalid request: unsupported parameter 'max_tokens'`）。含多个结果的 Chat `Tool` 消息，每个结果写成一条 wire `tool` 消息。从 4.7 代起的所有 Claude 模型都在本地拒绝 `temperature`，因此 `claude-haiku-5-5` 不会再发到 API 才收到 HTTP 400。中途被切断的 Anthropic 流不再报告 `message_start` 里的占位输出 token 数，Gemini 在省略 `candidatesTokenCount` 时会推导出 `output_total` 和 `total`。已完成的 HTTP 4xx 响应现在是 `RejectedBeforeOutput`（见下一条）。**行为变更：** 输出上限成员属于 native history 的 origin identity，因此在这次采用之前针对直连 OpenAI Chat 封存的 history，继续对话时会变为 `ReplayIneligible`；对该对话把 `bindings.max_output_tokens` 显式设为 `max_tokens` 即可继续。其他 origin 和 family 的对话以及新对话不受影响。
- **provider 返回的已完成 HTTP 4xx 响应会释放 hold，且不收取任何费用 (SchemaProvider #29)。** `provider_failure_proves_not_sent` 只在 `NotSent` 时释放 token 预留、Program 的金额 hold 和 dispatch receipt，所以 401、400、404 响应也让整笔预留一直被 hold。在 SDK 0.3.1 中，这类响应是 `RejectedBeforeOutput`：服务器拒绝了请求，什么都没有运行，也不会计费。新增的 `provider_failure_proves_not_accepted` 是所有结算路径（`ProviderBudgetClaim`、`ControlledProvider`、managed budget journal、Program 的进程内 broker 和 SQLite provider broker、MCP harness provider）共用的唯一规则。有 `NotSent` 依据时，或者是 408、425、429 之外的已完成 HTTP 4xx 响应、且该失败没有输出、stop reason 和用量、也没有可能已被计费的先前尝试时，它成立。**行为变更：** 这样的调用过去按整笔预留收费，在 `ProgramResult::monetary_records()` 中是 `held`，dispatch receipt 也是 `ReconciliationRequired`；现在不留金额记录，不收取模型 token，并记录为 `Failed`。HTTP 408、425、429、5xx 响应、被切断的调用以及请求发出后的 transport 失败仍保留 hold，早期版本已保存的结果也不变。用量 stage 为 `Missing` 表示没有证据，`Partial` 表示目前所见，两者都不能结算调用；推导出的数值与报告的数值一样用于结算，所以输出预算在思考中耗尽的 Gemini 调用，按实际用量收费，而不是按整笔预留。
- **采用 SchemaProvider `maint/interface6` 当时的最新提交 `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`：OpenRouter `provider.max_price` (#372)。** `sp::OpenRouterRouting` 新增 `max_price`（`sp::OpenRouterMaxPrice`），即 OpenRouter 可路由到的 endpoint 价格上限：可选的整数 nanoUSD 金额，`prompt` 和 `completion` 按每百万 token 计，另有 `image`、`audio` 和 `request`，以精确的十进制 USD 发送（`75'000'000` 即 `0.075`）。OpenRouter 只会路由到标价不高于上限的 endpoint，没有这样的 endpoint 时请求失败。没有任何金额或含有超过 2^53 金额的上限会在 I/O 前被拒绝。`ProviderControls::provider` 在 Chat Completions、Responses 和 Messages 请求上携带它，`SchemaProvider::Defaults::provider` 像其他 routing 字段一样合并它：请求自身的 `max_price` 取代默认值。Python 提供 `ng.OpenRouterMaxPrice`（`prompt_nano_usd`、`completion_nano_usd`、`image_nano_usd`、`audio_nano_usd`、`request_nano_usd`）和 `OpenRouterRouting.max_price`，后者返回分离的副本：修改后要把整个对象重新赋值回去。上限属于 native configuration binding，因此在轮次之间改变它会使已捕获的 native history 变为 `ReplayIneligible`；没有上限的 routing 保持原有的字节和 digest。库通过 `RequiredProvider` 中包含的 capability bit `sp::capability::OpenRouterRoutingMaxPrice` 声明该字段，因此 NeoGraph 的 handshake 会拒绝会丢弃上限的早期 interface-6 库。SDK 0.3.0 与 interface/shared ABI 6 不变，但请求布局变大了：见 #372 的重新构建条目。 同一 SDK 修订还使 HTTP transport 在所有平台上拒绝早于 TLS 1.2 的版本，不论 libcurl 构建或 TLS 后端的默认设置如何。
- **根据 code scanning 的安全加固。** CI 工作流仅以仓库读取权限运行。NeoGraph 自身的 HTTP/2 池只协商 TLS 1.2 及以上并且只使用 HTTP 和 HTTPS，测试中的回环 TLS 服务器也拒绝更旧的协议。vendored QuickJS 的 `js_realloc_array` 以 64 位计算增长量和字节数，不再让 `int` 乘法溢出，而是以 OutOfMemory 失败（记录在 `deps/quickjs/NEOGRAPH_PROVENANCE`）。vendored Clay 示例头文件以 `size_t` 计算 arena 大小。`docs/promo` 下的宣传视频工具使用修补后的 `source-map-js`、`postcss-selector-parser` 和 `fast-uri`。
- **采用 SchemaProvider `maint/interface6` 当时的最新提交 `0d536be66017dfb8c4c91d21f4f62bd8ce698d55`。** 使用 OpenAI Responses 的图和 agent 工具循环，在工具的 parameter schema 不兼容 strict 时（例如未关闭 `additionalProperties` 的 MCP 工具 schema）不再以 HTTP 400（`InvalidRequest`）失败：policy 的 strict 默认值只应用于每个 object 都设置 `"additionalProperties": false` 并在 `required` 中列出每个 property 的 schema，其他工具以 `"strict": false` 发送。对不兼容的 schema 显式设置 `strict = true` 会在 dispatch 前以 `InvalidRequest` 失败，并指出工具名和第一个失败的 JSON pointer。`created` 时间戳在 chunk 之间递增的 OpenAI 兼容本地服务器的 Chat 流不再以 `ProtocolCorrupt` 失败；该值减小，或 `id`、`model` 改变时仍会失败。codec 已接受的 in-band 供应商失败（例如 Responses 的 `response.failed` 事件或缓冲的失败响应体）现在无论大小都按其自身错误码分类。此前，超过 runtime `max_error_bytes`（默认 64 KiB）的失败，例如重复 hosted `image_generation` 结果的 3 MB `response.failed`，会被报告为 `RetryClass::Never` 的 `ResourceLimit`，而不是供应商的失败。runtime 自行缓冲并解析的错误仍受 `max_error_bytes` 限制。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **采用 SchemaProvider `maint/interface6` 上已合并的 PR #25 提交 `f7cb27e68ee02f3a27d01c31af85bcac96f00963`。** `Usage::input_uncached` 在所有 family 中遵循同一规则（输入 − 缓存读取 − 缓存写入；仅在从不发送写入计数的 wire 上将缺失值视为 0）：直连 OpenAI Chat 现在也会报告该值，Responses 会减去上报的缓存写入。Gemini 的 stop 类型已由测试固定。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **采用 SchemaProvider `maint/interface6` 上已合并的 PR #23 提交 `45dd2ad36e18717adae3e2d10b2b9c8bb0e85471`：用量与供应商数值完全一致。** 已通过真实 API 调用验证：OpenRouter Messages 流式响应不再因末尾的 `[DONE]` 失败，并带有上报的费用；Chat 保留 `video_tokens` 等供应商明细计数；上报的 USD 金额与供应商十进制值一致（不再多 1 nanoUSD），输入 + 输出 = 合计；Gemini 在合计可证明时报告推理为 0；Interactions 按名称记录 modality；`claude-haiku-4-5` 等 Anthropic 别名在以带日期模型应答时也被接受。保存的结果可能带有 `CostRounding::CeilingShortestDecimal`，旧记录仍可读取，Python 暴露 `ProviderCostRounding.CeilingShortestDecimal`。SDK 0.3.0 与 interface/shared ABI 6 不变。
- **`llm::Agent` 以及 deep-research、plan-execute 工具循环会继续被暂停的轮次 (#311)。** 收到 `sp::StopKind::PauseTurn`（Anthropic/Chat 的 `pause_turn`）时，它们会在现有迭代预算内重新发送包含该暂停 assistant 轮次的历史，而不再把暂停的轮次当作最终结果返回。同样的判断可通过 `paused_turn()` 使用。参考文档现已说明 Responses 的 `response.done` 别名规则以及已声明网关的服务模型规则。
- **采用 SchemaProvider `maint/interface6` 分支上已合并的 PR #21 提交 `83112573ba59e3b561fc33c22394638be7aa5294`：SDK 0.3.0、interface/shared ABI 6 不变，并加入路由网关修复。** Responses 与 Messages 现在接受已声明的 OpenRouter origin 针对 `~deepseek/deepseek-v4-flash-latest` 等路由别名返回的实际服务模型名；此前此类 Responses 调用都会以 `invalid response protocol (HTTP 200)` 失败。服务模型在同一响应内不得变化，replay 仍绑定到请求的模型。NeoGraph 的 API 与 ABI 均无变化。
- **采用合并的 SchemaProvider PR #16 提交 `3b88e4ba020c3a4d39ff0660014e7292b516b7cb`，须以 SDK 0.3.0 EXACT、interface/shared ABI 6 重建。** 这取代下方记录的先前 interface-5 SDK 步骤的当前要求。blocking 调用使用单一 async SDK pipeline，继续拒绝 event-loop/callback worker 重入。outcome、usage 观测、journal、native archive 重载与 Python 类型化值保留服务商报告金额、status、source、evidence、rounding 和冲突。canonical v2 JSON 仅在金额元数据完全缺失时省略 `provider_cost`，保持旧缺失记录 bytes 不变。零和 BYOK false 等金额证据阻止将未知 token hold 当作 proven-not-sent 退款；金额不转换为 token 或支出权限。Native archive v3 和 portable JSON v2 不变。
- **Core 引擎速度恢复到 v0.11.1 的水平。** 在历史测量中,3 节点顺序基准从每次运行 10.19 µs 降到 6.29 µs(v0.11.1:6.10),parallel-3 基准从 30.03 µs 降到 15.66 µs(15.80);每个线程各用一个引擎时,2、4、8 线程下的单线程耗时与 v0.11.1 相同。损失来自每个 fan-out 分支一个 strand、每个节点一次 `RunContext` 拷贝、包住上下文绑定注册表的进程级 mutex(现为 64 个分片)、由多个 JSON 文档构建的状态快照、什么都不返回的协程调用(没有 Send 时的 `run_sends_async`、没有检查点时的 `record_pending_write_async`)以及调度器中的有序集合。各提交的测量数据见 #343。
- **只链接 `neograph::core` 的程序不再加载 libcurl。** `neograph::core` 链接了 SchemaProvider 的运行时,而该运行时链接了 libcurl 传输层,因此仅运行图的进程也会加载 libcurl 及其所需的约 25 个库(基准测试启动时常驻内存 12.7 MB,v0.11.1 为 5.0 MB)。现在只有 `neograph::llm` 链接传输层;同一基准测试以 7.7 MB 启动,共享库从 36 个减少到 7 个。不使用 `neograph::llm` 而自行构造 `sp::runtime::Client` 的代码必须链接 `SchemaProvider::transport`。SchemaProvider 已固定到包含此拆分的修订版本。参见 #347。
- **须使用 SchemaProvider 0.2.0 alpha、interface/shared-library 代际 5 重新构建。** SDK 新增可选的连接、首个响应和空闲 timeout：默认关闭，不延长总 deadline，可通过 `ProviderRequest::options`（`sp::runtime::RunOptions`）按调用设置；图的 LLM 节点暂未暴露它们。五个 API family 的真实 transport lifecycle 检查与独立 private test fixture 解决 SDK #2、#3、#4。C++ 使用方与 Python wheel 必须使用匹配的 SDK header/library 一起重新构建；native archive 仍为 v3，portable JSON 仍为 v2。
- **只链接 `neograph::core` 的程序不再加载 libcrypto。** SDK 自行实现了 SHA-256、HMAC-SHA256 和 OS 随机源的使用，其运行时不再链接 OpenSSL，archive 与 policy 字节不变。仅运行图的基准测试（`bench_neograph 1 1 1`）以 6.3 MB 常驻内存启动，而不是 7.7 MB（Linux x86_64，31 次中位数），共享库从 7 个减少到 6 个，与启动内存为 5.0 MB 的 v0.11.1 相同。直接调用 OpenSSL 的代码必须自行链接 `OpenSSL::Crypto`。

### 新增

- **Program：检查扁平的 step plan（`step-plan/v1`），并把它转换成 task graph proposal。** `intake_step_plan`（`neograph/program/step_plan.h`）接收 planner 回答的文本或已解析的文档，要么把所有问题以带稳定 `STEP_*` 错误码的 `{path, code, message, hint, detail}` violation 的有序列表返回，要么把计划按字节确定性地转换成 schema 2 的 `TaskGraphProposal`，并附带指回各 step 的 source map。计划是一个有序的 step 列表：每个 step 指名宿主 allow-list 中的一个模板，为每个输入给出字面量或对前面某个 step 的引用，还可以加上顺序边和重试提示。格式是封闭的，不含 provider、凭据、预算或可执行文本，也无法包含环。转换后的 proposal 走不变的可信路径（`TaskGraphProposal::parse`、`TaskGraphFragmentCompiler::compile`、`expand_task_graph`）；通过 intake 只说明计划格式正确、符合策略，不说明它解决了请求。反馈中取自计划的字符串会截到 64 个可打印字节，字面量的值从不回显。`format_violations_for_model` 把 violation 渲染成长度有界的提示词文本，`step_plan_json_schema` 为某个目录生成 JSON Schema，`schemas/step-plan-v1.schema.json` 是与目录无关的形状。参见 [`docs/plan-intake.md`](docs/plan-intake.md)。
- **Program：次数有界的 plan repair 循环。** `add_step_plan_repair` 注册 `neograph.step_plan_intake/v1`、`neograph.step_plan_route/v1` 和 `neograph.step_plan_report/v1`，让 `plan -> intake -> (plan again)` 成为一张普通的 Core 图。intake 节点检查 planner 的最后一次回答，把转换后的 proposal 和报告（`status`、`attempts`、`last_violations`、`violation_code_histogram`、`plan_digests`、`envelope`）写入通道，并带上 violation 重建 `messages`：`feedback` 为 `replace`（默认）时上下文不会增长，为 `append` 时保留整段对话。尝试次数、run 的预算、provider 调用日志和 Core 检查点为这个循环设界并留下记录，所以恢复后的 run 不会再去问模型。放弃的循环以回退分支或 failure exit（`plan.repair_exhausted`）结束。宿主的 allow-list 和限额是所注册可执行体的选项，封印后的图无法更改它们。`repair_step_plan` 把同样的循环包在回调外面，供在启动 run 之前先做规划的宿主使用。
- **Program：task graph 的 task 可以带字面量输入和尝试次数，调度器会重试（proposal 与 fragment 的 schema 2）。** schema 2 的 task 可以带 `literals`（在 binding 之后赋给已声明输入字段的 JSON 值）和 `max_attempts`。新增错误码 `P_PROPOSAL_LITERAL_TARGET`、`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`、`P_PROPOSAL_LITERAL_SIZE` 和 `P_PROPOSAL_ATTEMPTS`，schema 1 的文档只要用了其中任一成员就是 `P_PROPOSAL_SCHEMA_VERSION`。宿主通过 `TaskGraphProposalLimits::max_literal_bytes`、`max_total_literal_bytes` 和 `max_attempts` 启用；默认值（0、0、1）拒绝字面量和重试。汇总按尝试次数预留 task 的预算，每次尝试的子运行都带着完整的预算启动。task 的子运行以可重试的 Core 节点失败（与 plan 级 `retry` 操作的规则相同）结束且还有剩余尝试时，调度器用下一个尝试编号、新的尝试 identity 和新的子运行再次派发该 task。对将要重试的尝试不会写入 `Failed`，依赖它的 task 会等待，也不会触发 `fail_fast`。两个成员都没用到的文档和 fragment 保持原有的字节与 identity。
- **Core：节点可以知道一次恢复回答的是哪个中断（#406）。** 当 `resume_value` 回答的是节点自身的 `NodeInterrupt` 时，`RunContext::resume_interrupt_id`（Python 中为 `input.ctx.resume_interrupt_id`）就是该中断的 id。无论恢复是指名回答，还是回答了第一个中断，它都与 `RunResult::interrupt_id` 和 `RunResult::interrupts` 报告的 id 相同。当该值是拦住此节点的 `interrupt_before` 闸门的批准时，它为空（Python 中为 `None`）。位于闸门之后、同时也会提出自己问题的节点，不必再把放行的批准当作自己问题的回答：id 为空时，它就提问。哪个调用收到恢复值保持不变（#393）。`RunContext` 新增了成员，原生使用方需用匹配的头文件重新构建。
- **A2A：native server 支持 `ListTasks`、`SubscribeToTask` 和非阻塞发送。** `ListTasks` 按 `contextId`、`status` 和 `statusTimestampAfter` 过滤，并按最新优先分页；`SubscribeToTask`（0.3 为 `tasks/resubscribe`）流式发送当前 task 及其实时 event；`returnImmediately`（0.3 为 `blocking: false`）立即返回已提交的 task。每个 run 都在自己的 worker 上执行，因此 task 的生命周期长于断开的连接，每个 status 都带有 timestamp。不声明 push notification、extended card 以及 HTTP+JSON/gRPC binding，其 method 以 `-32003` / `-32004` 应答。
- **ACP：client 提供的 stdio MCP server** 通过 NeoGraph 的 MCP client 启动，node 通过 `ACPClient::session_tools()` 访问其 tool。server 只得到最小的环境，即 agent 的 `PATH`（Windows 上还有 `SystemRoot`）加上 client 的 `env`，不会得到可能含有 provider API 密钥的其余 agent 环境。
- **A2A：card 将 collaboration envelope 声明为 extension。** 带有 `CollaborationMailbox` 的 Program agent 在 `capabilities.extensions`（`AgentCard::extensions`）中列出 `a2a::COLLABORATION_EXTENSION_URI`，因此 collaboration message 中由发送方指定的幂等 task id 成为已声明的 extension（A2A spec 4.6）。
- **ACP：增量 update。** 模型 token 以 `agent_message_chunk` 流式发送，tool 调用以 `tool_call`/`tool_call_update` 报告。
- **官方 SDK interop suite 和 protocol-interop CI job。** `bindings/python/tests/interop/a2a_suite` 用 a2a-sdk 1.2.2 以及 @a2a-js/sdk 1.3.0 和 0.3.14 驱动 native A2A server 和 client，并用 A2A v1.0.1 proto 与 JSON Schema 以及 v0.3.0 JSON Schema 校验每条 message；`acp_suite` 用 agent-client-protocol 0.12.1、1.0.0rc2 和 @agentclientprotocol/sdk 1.8.0 驱动 native ACP agent（`neograph_acp_probe_agent`），并用 ACP schema v1.25.0 校验每条 message。suite 固定 SDK 版本，未安装固定版本时干净地跳过，并在安装这些固定版本和 Node.js 22 的 `.github/workflows/protocol-interop.yml` 中运行。
- **MCP：2025-03-26 和 2024-11-05 对端的 JSON-RPC 批处理。** MCP 2025-03-26 要求接收方接受 JSON-RPC 批处理。`MCPServer` 和 `MCPHttpServer` 现在对协商了 `2025-03-26` 的客户端，以及未定义批处理但同样对待的 `2024-11-05` 客户端发来的批处理，在 stdio 和 HTTP 上以一个数组应答（只含通知的批处理为 HTTP 202）；被取消的调用被略去，没有剩余内容可应答的批处理不发送任何内容。之后的修订版去掉了批处理，initialize 也不能放进批处理，因此这样的批处理与以前一样是一个 `-32600` 错误（HTTP 400）。`MCPServer::handle_message(message, reply)` 把每条消息的结果（应答、批处理的数组或无）交给调用方，`MCPHttpServer` 和 stdio 循环使用它。NeoGraph 的 MCP 客户端逐条处理服务器发来的批处理，并以一个数组应答其中的请求。MCP 互操作测试套件按每个会话协商的修订版的 schema 校验其消息。
- **MCP：protected resource metadata、有界的 initialize、工具列表变化、继承的环境。** `MCPHttpServerConfig::protected_resource_metadata` 为 OAuth 部署发布 RFC 9728 元数据，并在每个 401 质询中连同 `scope` 指明其位置。`MCPClient::initialize(name, deadline, cancel_token)` 约束握手。`MCPClient::set_tool_list_changed_handler` 报告 `notifications/tools/list_changed`。不带 `replace_environment` 的 `StdioClientConfig` 现在接受添加到继承环境或覆盖其中同名条目的 `environment` 条目；过去会被拒绝。
- **Python MCP 客户端：传输错误、加固 stdio、shutdown 和有界调用。** `mcp.MCPTransportError` 携带 `failure` 与 `http_status`，`mcp.MCPError` 也携带 `http_status`，`mcp.StdioClientConfig` 与 `MCPClient(config)` 启动加固的 stdio 服务器，`MCPClient.shutdown()` 永久结束客户端，`initialize()` 与 `call_tool_result()` 接受 `timeout` 和 `cancel_token`，`set_tool_list_changed_handler()` 接受可调用对象。
- **官方 MCP SDK 互操作测试及其 CI 作业。** `bindings/python/tests/interop/mcp` 针对 NeoGraph 的 MCP 服务器与客户端运行官方 Python SDK（`mcp` 2.3.0 与 1.30.0）和 TypeScript SDK（`@modelcontextprotocol/sdk` 1.32.1、`@modelcontextprotocol/client` 与 `@modelcontextprotocol/server` 2.3.1），并用 MCP 2025-11-25 `schema.json` 校验捕获的每条消息；新的 `MCP interop` workflow 安装固定版本的 SDK 和 Node 并运行它们。没有这些 SDK 时测试会跳过。
- **Program：可选启用的健全性检查 (#363, #364, #365)。** `ProgramCompilerConfig::soundness_check`（Python：`ProgramCompiler` 和 `LocalProgramHost` 上的 `soundness_check=True`；`GraphValidator`：`ValidationOptions::soundness_check`）会探索根图的路由所允许的每一个运行。状态由就绪集合、每个 join 持有的信号（含 `any_of` 槽位）以及带 `max_visits` 的节点的访问次数组成；conditional 路由和失败 edge 是不确定的选择，failure exit 会结束运行。检查返回以展示问题的最短路由序列为 witness 的警告：`P_CORE_S1`（从某一时刻起没有运行能够完成）、`P_CORE_S2`（运行结束时 join 仍持有信号，以 barrier-stalled 结束或经由显式结束）、`P_CORE_S3`（永不触发的 join）、`P_CORE_S4`（没有出口的循环）和 `P_CORE_S5`（写同一个 overwrite 通道的两个节点在同一个 super-step 中就绪，`P_CORE_E5` 的 fan-out 兄弟之外）。它们的 help 指向 witness 中的路由选择；对于可由一个槽位之外的节点填补的停滞，会建议 `any_of` 槽位。`P_CORE_SOUNDNESS` 摘要在探索了每个运行时为注记，否则为警告：类型声明了 `Command.goto` 或 `Send`（effect 契约 `"control": ["goto"]` 或 `["send"]`）的节点之后的运行无法分析，达到 `soundness_state_cap`（默认 100 000 个状态）时检查以 not checked 停止。两种情况都不声称未探索的内容。没有任何内容被封存，因此包 id 不变。在有界穷举 join 测试框架枚举的每个图上，判定都与其参考解释器一致，每个 witness 都能逐步重放。capability manifest 增加了标志 `diagnostics.soundness_check`。
- **Program：可以编译失败路由键 (#364)。** `graph.retryPolicy(node, {max_retries, non_retryable, max_visits, error_text, on_failure, error_channel, ...})` 降级为严格 Core 键 `node_retry_policies`（只保存写出的字段，带有 `max_retries` 0 到 10、`max_visits` 1 到 100 等范围）和 `failure_edges`，`graph.failureExit(node, {code})` 写入 `failure_exits`。编译器会检查它们（哨兵、自身、join 或隐式结束目标、同一节点的第二条 edge、非 append 的 error channel 报告 `P_CORE_GC_FAILURE_EDGE`；范围和代码报告 `P_CORE_GC_FIELD_VALUE`；未知名称报告 `P_CORE_E3`），把 failure edge 加入路由后继映射使 E7、E11 和 E15 能看到它们，把 `error_channel` 计为已写入，并对没有任何东西读取的节点配置 `retry_policy` 键发出 `P_CORE_E18` 警告。图级策略中的 `on_failure` 是带提示的未知字段。这些键会被封存，除 `node_retry_policies` 外都属于迁移形状；不使用它们的定义保持原有 id。执行它们的运行时见下一条。`TopologySpec` 和 `CompiledGraph` 新增了成员。参见 [Program 图功能](docs/program-graphs.md)。
- **失败路由运行时 (#364)。** 最终失败可路由且有 failure edge 的节点不再使运行失败：失败的尝试不写入任何内容，一条结构化记录（`node`、`kind`、`code`、`attempts`、`retryable`、`step`、`routed_to`、`effects`）被追加到该边的 `error_channel`，节点只路由到该边的目标。静态节点、扇出分支、一个或多个 `Send` 目标以及子图节点走同一条路径；崩溃后的恢复会从 pending write 重放路由而不会再次运行该节点；有 `interruptBefore` 的目标带着 `{"reason": "failure_route", "failure": record}` 暂停；`ERROR` 事件增加了 `handled` 和 `routed_to`。自由文本按节点选择启用（`error_text`），只来自带类型的 `public_message`，需要宿主的 `EngineConfig::failure_text_redactor`（`CatalogConfig::failure_text_redactor`；Python 为 `GraphEngine.compile` 和 `LocalProgramHost` 的 `failure_text_redactor=`），并会去除不可见字符、截断到 512 字节。重试时间按字段确定，对于定义中声明了节点级策略的节点，宿主的节点级策略只能收紧（`GraphEngine::resolved_retry_policy`）；`non_retryable` 中的代码不会重试。`max_visits` 把超出上限的激活作为 `NG.VisitLimit` 路由，计数器保存在检查点元数据中。failure exit 节点以 `RunStatus::FailureExit` 和 `output["_neograph"]["failure_exit"]` 结束运行，并使 Program 运行以声明的代码结束为 `Failed`。`RunBudget::max_node_retries`（Python 为 `ProgramRunBudget.max_node_retries`）限制整个运行的节点重试，并以 `ProgramUsage::node_retries` 报告；二者只在设置时序列化，因此现有标识不变。清单标志 `retry_policy.per_node`、`failure_edges` 和 `failure_exits` 已开启。参见 [Program 图功能](docs/program-graphs.md#failure-routing)。
- **尝试超时与感知截止时间的退避 (#364)。** `graph.retryPolicy(node, {attempt_timeout_ms})`（Core `node_retry_policies[node].attempt_timeout_ms`，100 到 3 600 000）限制节点的每次尝试。超时的尝试通过它自己的子 `CancelToken` 被取消，并以 `NG.Timeout` 失败，该失败会被重试且可被路由；第一个终止事件获胜，因此在超时时刻或之后的完成就是超时。该尝试的迟到完成会被丢弃：它的写入、结果和事件不会到达运行，它发起的经中介的 Tool 调用会被拒绝，下一次尝试只在它停止之后才开始。在停止截止时间（再一个超时时长，至少一秒）前未停止的尝试会以 `NG.EffectUncertain` 使运行失败；它在后台继续运行并保持其节点存活，`wait_for_node_attempts()` 会等待它（Python 绑定在解释器退出时等待）。当下一次尝试之前的退避会越过运行的截止时间（Program 的 `wall_time_ms`、`RunMetadata::deadline`）时，节点会立即以 kind `deadline` 停止重试，而不是一直睡到预算结束运行；编排计划的 `retry` 操作在这种停止之后也不会再次运行其主体。静态节点、fan-out 分支、`Send` 目标、子图节点以及同步和异步入口的行为都相同，崩溃后的恢复会继续正在运行的那次尝试。`GraphEngine::set_node_attempt_timeout_ms` 和 `resolved_attempt_timeout_ms`（Python 中也有）让宿主可以设置超时，并且只能缩短定义中的超时。`NodeAttemptTimeout` 是超时尝试失败的原因。清单标志 `retry.attempt_timeout` 已开启。参见 [Program 图功能](docs/program-graphs.md#attempt-timeouts-and-the-deadline)。
- **Program：在金额预算下绑定的 `llm_call` provider (#372)。** Python 宿主为 Program 的 `llm_call` 节点绑定 provider：`ProgramRegistryBuilder.add_provider(name, version, digest)` 声明它，`add_registered_node("llm_call", ..., provider=name)` 要求它，`LocalProgramHost(..., node_context=ng.NodeContext(provider=..., model=..., provider_controls=...), binding_label=...)` 通过与 MCP harness 相同模式的 `CatalogCapabilityBinder` 绑定它。只有经过校验的非机密标签进入 identity，位于 provider 的 capability receipt 中；密钥和模型从不进入，不需要 provider 的 Program 保持其 bundle id 与版本 id。每个绑定的调用都依据现在编译进 `neograph::program` 的价格目录，针对运行的 `monetary_microunits` 计价。分发之前，broker 为调用的输入上限与输出上限预留目录上限（OpenRouter 上为 route ceiling），对无法确定上限或预留放不下的调用，在发送任何请求之前以 `P_MONEY_*` 失败拒绝。得到结果后，按报告费用（OpenRouter `usage.cost`，证据 `reported`）或目录估算（`estimated`，写明目录 version 与 digest）结算；结果或 usage 未知时保留预留额（`held`）。`ProgramResult::monetary_records()`（Python 为 `result.monetary_records`、`ng.ProgramMonetaryRecord`）列出各次调用，`ProgramUsage::monetary_microunits` 是其总和。C++ 宿主通过 `bind_priced_provider_call(context)` 或 `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)` 启用；后者把每次调用的记录写入其 journal 行：崩溃前记录的调用在恢复后只收费一次（execution lease 过期结束运行时也一样），较早 attempt 记录的调用不会再次收费。`LocalProgramHost(sqlite_directory=..., native_history_archive=...)` 把运行、检查点和该 journal 保存在 SQLite 文件中，`reconnect(run_id)` 继续停止的进程留下的仍在运行中的运行。在 OpenRouter 上，broker 会把 route ceiling 作为 `provider.max_price` 固定到 Chat Completions、Responses 和 Messages 请求上，并逐字段保留宿主已设置的更低上限，因此 OpenRouter 只会路由到标价不高于该调用预留价格的 endpoint。见[计价证据](docs/reference-en.md#pricing-evidence-and-the-price-catalog)与 [Python 指南](docs/python-binding.md#model-calling-nodes)。
- **带类型的节点失败 (#364)。** `NodeFailureError(code, public_message, retryable)` 以 `app.compile_error` 这样的作者代码使节点失败，`retryable = false` 会停止其重试循环。`node_failure_of()` 返回执行器所抛出的 `NodeExecutionError` 上附带的 `NodeFailure` 记录：代码、分类、停止重试的原因、尝试次数、可重试与可路由标志、`public_message` 以及最后一次尝试的 Tool 效果。Python 节点的异常也通过其 `code`、`retryable` 与 `public_message` 属性以相同方式读取，调用方收到的仍是原始异常。
- **Program：witness 路径、`any_of` 建议和路由表 (#363, #365)。** `P_CORE_E15` 的 witness 现在带有 `paths`，即从 barrier 成员经过每条绕过路由的路由序列；当该序列回到 join 时还带有 `substitute` 和 `suggested_wait_for`：返回的诊断会建议一个 `any_of` 槽位，让 join 之前的节点替代该成员（对 #357 为 `[{"any_of": ["B", "FIX"]}, "C"]`）。停滞运行的 witness 也会增加由执行轨迹和路由后继映射得出的同样的 `paths` 和 `suggested_wait_for`。`ProgramCompilerConfig::echo_routes`（Python：`ProgramCompiler(..., echo_routes=True)`）以 `P_CORE_ROUTES` 注记返回路由后继表，`GraphValidator::routed_successors()` 公开该表。这些都不会被封存。
- **Program：barrier join 中的 `any_of` 槽位 (#363)。** `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` 为每个槽位等待一个备选项：任一备选项向 join 发出信号即填满槽位，所有槽位填满后 join 运行，随后清空全部信号。#357 的结构现在在两条路由上都能完成。在 join 运行之后才到达的备选项会开启一个永远不会完成的轮次，因此运行以 `P_CORE_BARRIER_STALLED` 结束（失败即关闭）；新警告 `P_CORE_E17` 会报告可能同时运行的备选项。只有一个名称的槽位等同于该名称，因此与旧形式等价的定义保持其字节和 id；其他定义按 UTF-8 字节排序输出到新的 `TopologySpec::join_specs` 中，`CompiledGraph` 和 `Scheduler` 也携带它。校验器逐槽位检查 E3、E8 和 E15，停滞 witness 输出槽位，迁移适配器把槽位 join 列为 barrier。any、quorum 等 join 模式仍然推迟。`Scheduler` 新增了成员和构造函数，布局随之改变。参见 [Program 图功能](docs/program-graphs.md)。
- **Program：图元数据 (#362)。** `ng.graph("plan").metadata({...})` 将宿主可读的作者声明存为 Core 定义的顶层 `"metadata"` 对象，可从 `bundle.sealed_core_definitions()` 读取（Python：`bundle.sealed_core_definitions[0]["definition"]["metadata"]`）。它随定义一起参与哈希，从不传给节点工厂、状态、检查点、输出或事件。限制为规范 JSON 8 KiB、嵌套深度 4、成员 64 个、键长 1 到 128 字节；不可见字符会报错（`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT`）。`GraphSemanticMigrationAdapter::metadata_changes()` 列出元数据差异，这些差异不改变 `topology_shape_id()`。使用 Program API v2 键（包括元数据）的 Core 定义不封存校验器警告；不使用的程序保持原有包 id。`TopologySpec` 和 `CompiledGraph` 新增了成员，布局随之改变。参见 [Program 图功能](docs/program-graphs.md)。
- **Program：JavaScript 能力清单公开功能标志、签名和参数 schema (#365)。** `javascript_authoring_capability_manifest()` 新增 `features`（每个编写功能一个布尔值，例如 `graph.barrier`、`graph.metadata` 或 `failure_edges`，宿主或模型可据此了解本构建接受哪些功能）、每个方法的 TypeScript `dts` 签名及共享声明 `dts_prelude`，以及每个图构建器参数的 JSON Schema。新增 schema 文件：`schemas/javascript-capability-manifest-v1.schema.json` 描述清单，`schemas/core-definition-v1.schema.json` 描述用于约束解码的封存 Core 定义。
- **Program：诊断附带自我修复用的 help (#365)。** 编译返回的每个诊断（`ProgramCompileError::diagnostics()`、`ProgramBundle::compile_diagnostics()`、Python 异常的 `.diagnostics`）都带有 `help`：指向新[诊断指南](docs/diagnostics.md)中对应条目的链接、带标签的相关位置、以 RFC 6902 JSON Patch 编辑表示并标明适用性（`machine`、`maybe` 或 `placeholder`）的修复建议，以及 `candidates`，即与未知名称相差不超过两次编辑的已声明名称。关于同一未知名称的诊断会合并为一个，其余位置成为相关位置；最多返回 20 个诊断，错误在前，随后附一个 `P_DIAGNOSTICS_TRUNCATED` 注记。JavaScript 消息使用构建器调用的名称，例如 `graph.retryPolicy({...}) has an unknown field 'on_exhausted'`。封存的诊断从不携带 `help`，因此包 id 不变。`program::Diagnostic` 新增了成员，布局随之改变：请重新构建 C++ 使用方（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **Program：`P_JS_CONTROL_CHARACTER` 警告 (#361)。** JavaScript 配置字符串中含有制表符、换行和回车以外的 C0 控制字符时（例如本想写正则单词边界的 `"\b"`），会在该字符串的调用位置发出警告。该警告只出现在编译返回的诊断中。
- **拓扑验证遵循 scheduler 实际采用的路由，并新增报告四类问题 (#357, #358)。** 带有 conditional edge 的节点只通过其第一条 conditional edge 路由，因此 E7（不可达节点）、E8（barrier liveness）和 E11（没有通往 `__end__` 的路径）不再计入该节点的 static edge 或之后的 conditional edge。E13 报告从同时带有 conditional edge 的节点发出的 static edge，这种 edge 永远不会触发。E14 报告同一节点的第二条 conditional edge，这种 edge 永远不会路由。E15（警告）报告其 route 通向 join 或 `__end__` 以外位置的 barrier 成员；走该 route 时，运行以 barrier-stalled 结束。E16（错误，需显式启用）为需要 DAG 的宿主报告任何环；可通过 `ValidationOptions::require_acyclic`、`ProgramCompilerConfig::require_acyclic`，或 Python `ProgramCompiler` 与 `LocalProgramHost` 上的 `require_acyclic=True` 启用。既不列出 reads 也不列出 writes 的 effects 契约现在视为未声明，因此 E6 不再把 Python 节点写入的通道报告为 dead(#361)。**行为变更：** E13 和 E14 在 strict 文档（`schema_version` 1 及以上，包括所有 JavaScript Program）中是错误，因此此类图不再能编译；legacy 文档会收到警告。bundle id 不变：Program 编译按当前规则失败并通过 `compile_diagnostics` 返回其警告，但封存的是先前规则的警告（`ValidationOptions::original_rules`），准入(admission)也按这些规则 link，因此先前封存的 bundle 仍可继续 materialize。`EngineConfig::validation` 选择 `GraphEngine` build 和 link 时使用的规则。
- **`EngineConfig::stderr_notices` (#361)。** 默认值为 `true`。为 `false` 时，`build()` 和 `link()` 不会把验证 lint 打印到 stderr（错误仍会抛出），也不会打印 fan-out 串行运行的一次性提示。Program Catalog 在关闭该选项的情况下 link 其引擎，因此 `compile_admit` 不再打印 `[neograph] lint [E6] ...`，Program 运行也不再打印提及 `EngineConfig::worker_count`（`LocalProgramHost` 用户无法设置的选项）的提示。Program lint 只通过诊断到达调用方。
- **Python：`LocalProgramHost(..., worker_count=1)` (#370)。** 它通过 `CatalogConfig::worker_count` 为图内的并行 fan-out 设置每个 Core 引擎的工作线程池，且不得超过已准入 Program 的 `max_concurrency`。类的 docstring 说明了 `scheduler_threads` 和 `worker_count`。
- **`ChatTool::strict` 可按工具设置 OpenAI Responses strict 模式；C++ 使用方须重新构建。** 该标志是默认未设置的 `std::optional<bool>`，由 `make_provider_request` 传入 Responses function 工具。未设置时沿用上文所述的 SDK 默认值，`false` 按原样发送，对不兼容 strict 的 schema 设置 `true` 会在 dispatch 前以 `InvalidRequest` 失败。其他 family 无法发送该标志，因此以 `std::invalid_argument`（Python 中为 `ValueError`）拒绝 `true`，接受 `false`。Python：`ChatTool(..., strict=None)` 及 `strict` 属性。`ChatTool` 增加了 member，layout 随之改变：NeoGraph、C++ 使用方和自定义 `Tool` 实现以及 Python 扩展须一起重新构建（见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- `CheckpointCoordinator::records_pending_writes(parent_cp_id)`:当 `record_pending_write[_async]` 确实会存储内容时返回 true,调用方可据此跳过调用。

### 修复

- **Program：持有执行租约的运行在 `reconnect()` 时会说明原因（#421）。** 没有子预算的、只有 `define()` 的 Program 运行，会在其挂钟时间内持有执行租约。因此在租约到期前，`reconnect()` 会抛出 `P_RUN_LEASE_HELD`，文字是“owned by another live runtime”，看起来像是需要子预算。现在错误会点明租约，说明租约到期后 `reconnect()` 会让运行以 `TimedOut` 结束，并指出 `main()` 和子预算（`max_child_depth`、`max_total_children`）不持有租约，会从最近一个检查点继续；witness 里带有 `expires_at_ms` 和 `retry_after_ms`。哪些运行持有租约没有变化。Program 指南在“重启”一节列出了这条规则。
- **Program：分叉只从源运行的“量”中取自己的份额，不占用它的并发，也不会让源运行无法恢复（#414）。** 分叉的预算整个从已暂停运行的剩余预算中扣除，`max_concurrency` 也不例外。因此，把运行的全部 `remaining_budget` 给了分叉，源运行就没有并发可用（`P_CONCURRENCY_BUDGET`、`P_TRANSITION_CONFLICT`），或者没有挂钟时间可用（`P_RUN_LEASE`；有 `main()` 时在 0 ms 处 `TimedOut`）。挂钟时间、模型 token、金额、Program 操作数、Core 步数、动态编译数和子预算都是“量”：分叉取走自己的份额，其余留给源运行。`max_concurrency` 是“上限”：分叉带着自己的上限（不超过源运行的），源运行保留自己的；转换存储会检查分叉没有改动它。**行为变更：** 分叉要拿走源运行剩下的全部挂钟时间或全部 Core 步数时，`fork()` 会抛出 `P_FORK_SOURCE_BUDGET`，并且不改变任何东西。把已暂停运行的全部 `remaining_budget` 给分叉，以前是被接受的，现在请只给一份。规则写在 Program 指南和 Python 绑定指南里。
- **Program：模型 token 预算耗尽时，运行以同一种形态结束（#420）。** 模型的一次响应使运行超出 `model_tokens` 时，运行以没有 `failure` 的 `BudgetExhausted` 结束；而预留放不下、在发送前被拒绝的调用，则以带有 `NG.BudgetExhausted` 的 `Failed` 结束，调用方只能一种情况看 `status`，另一种情况看 `failure.code`。现在两者都以 `BudgetExhausted` 结束，`failure.code` 为 `NG.BudgetExhausted`。这个代码是 `graph::classify_failure` 给预算耗尽的代码，MCP、gRPC、A2A 和 ACP 主机也以它报告。`failure.witness["model_tokens"]` 含有 `granted`、`used` 和 `over_by`，`over_by` 是最后一次响应超出的量（被拒绝的调用为 0，其 failure 仍保留 provider 证据）。没有获得 `model_tokens` 却调用了模型的运行也会得到这条记录。**行为变更：** 因模型响应而超出预算的运行现在有 `failure`；下一次调用在发送前被拒绝的运行以 `BudgetExhausted` 结束，而不再是 `Failed`。
- **Program：有 `main()` 时，`remaining_budget.max_node_retries` 也是上限减去已用的重试次数（#419）。** 带有 JavaScript `main()` 的 Program 报告的仍是开始时的上限，就像没有发生过重试（`usage.node_retries` 为 1 时报告 5）。原因是结算命令的剩余预算时没有扣除该次尝试的重试；只有 `define()` 的 Program 本来就报告 4。现在两者都报告上限减去该次尝试的重试次数，恢复后的尝试从这个剩余值开始。
- **Program：重试用尽的失败在有 `main()` 和重试上限时，也会在 `interruptBefore` 处理器处暂停（#418）。** 带有 JavaScript `main()` 和 `max_node_retries` 的 run 中，节点用尽重试并被路由到 `interruptBefore` 处理器时，run 不会以 `Interrupted` 暂停，而是以 `Failed` 结束，错误码为 `P_TRANSITION_CONFLICT`。`callCore` 在暂停之前一直持有预留，而释放该预留的转换在检查时要求剩余的重试上限等于调用开始时的值，可调用所用的重试早已把上限减小。现在检查接受按该 attempt 所用重试减小后的上限，更高或更低的值都不接受，没有上限的 run 仍然没有上限。**行为变更：** 这样的 run 现在以 `Interrupted` 结束，剩余上限按所用重试减少，并且可以恢复。没有 `main()`、没有上限或没有重试的 run 不受影响。内存、SQLite 和 PostgreSQL 存储共用这项检查，已存储的数据不变。
- **Program：带有 native 状态的 provider 失败，在没有 native archive 的宿主上也会发布（#426）。** 流在中途被切断，或 provider 只返回了部分回复就失败时，没有 `native_history_archive` 的宿主不会发布 run 的 terminal 状态，内存中的 `LocalProgramHost` 也属于此类：stderr 会输出 `terminal publication exception ... Program provider evidence lacks durable native custody`，`run()` 返回失败的结果，而 `reconnect()` 报告 `P_RUN_LEASE_HELD`。provider 的 native replay 状态只能存放在 archive 的已认证 custody 之下，所以整个失败被拒绝。现在这样的宿主会发布失败，并只保留证据中可移植的部分：消息、usage、stop reason、error 和 raw event 保持不变，每条消息去掉 native seal，witness 中的 `provider_outcome_native_retained: false` 说明了这一点。启动时就拒绝该配置，会拒掉所有绑定了 provider 的内存宿主；在所有宿主上丢弃证据，又会丢掉有 archive 的宿主本可保留的内容，所以这个回退只用于没有 archive 的情况。**行为变更：** 在没有 archive 的宿主上，结果的 `failure.provider_outcome` 持有去掉 native 状态的消息（原始 outcome 仍留在 `provider_cause`），run 可以重新连接并读取。有 archive 的宿主照旧保存 native 证据，archive 写入失败时失败仍然不会发布。
- **A2A：对重启后恢复的 Program task 调用 `SubscribeToTask`，会跟随其 run 直到最终 status（#413）。** 从邮箱恢复 Program task 的服务器，没有这个进程未启动的 run 的 channel，所以流发送 Task 后立即关闭，没有最终 status。现在流会跟随持久 run，直到 run 产生结果或 cancel 生效，然后像服务器自己启动的 run 一样发送结果 artifact 和最终 status（暂停的 run 以 `TASK_STATE_INPUT_REQUIRED` 结束）。调用之前已经结束的恢复 run 会先被读出，并作为 terminal 以 `-32004` 回答。无法跟随 run 时，流不会空着关闭，而是以 `-32603` 错误事件（ErrorInfo reason `RUN_UNAVAILABLE`）结束；task 在本服务器上没有 run 时，调用以 `-32603` 回答。**行为变更：** 已恢复且仍在运行的 task 的订阅者，现在会保持连接直到 run 结束，`A2AServer::stop()` 会等待这个连接，就像等待服务器自己启动的 run 的流一样。
- **带有 Python 节点的引擎仍然存活时，Python 进程能够正常退出（#429）。** 使用 `run_async` 并在脚本结束时仍引用着引擎（例如模块全局变量）的脚本，可能在解释器退出时崩溃；负载下大约每 10 次出现 1 次，表现为 SIGSEGV 或 SIGABRT。支撑 `run_async` 的线程在释放最后一次运行的 Python 对象时，趁主线程终结解释器的时候去获取 GIL，`PyThread_exit_thread` 使该线程被强制退出；强制展开栈的过程要么以 `std::terminate` 结束，要么执行了触及已释放线程状态的析构函数。脚本结束时仍在执行 Python 代码的运行，每次都会崩溃。现在模块在导入时注册一个 `atexit` 钩子。钩子在终结开始之前，先等待比所属运行存活更久的节点尝试，不再接受 `run_async` 调用，（释放 GIL 后）等正在运行的回调结束，然后 join `run_async` 线程以及运行 Python 节点和工具主体的线程池。此后，没有 GIL 的线程所释放的内容，例如进程退出时销毁的、被放弃的运行的处理程序，会被直接丢弃，不再调用解释器。**行为变化：** 钩子运行之后（在之后的 `atexit` 处理函数或终结器中），`run_async`、`run_stream_async` 和 `resume_async` 会抛出 `RuntimeError`；退出时仍在等待的运行会被放弃，而不是留着使进程崩溃。测试一次启动 8 个子进程运行复现脚本，要求每个退出状态都是 0。
- **服务器在运行前拒绝的 MCP 或 gRPC 请求，不再以 `reconciliation_required` 的形式到达模型（#423）。** 失败的 MCP 调用，无论是 `isError` 结果还是 JSON-RPC 错误，一律以 `{"status":"reconciliation_required","retryable":false,"effect_uncertain":true}` 到达模型，除非工具有能排除效果的执行策略。即使服务器在运行任何东西之前就拒绝了请求、模型只需修正参数，也是如此。现在，被拒绝的请求无论策略如何，都是带有服务器消息、`retryable` 与 `effect_uncertain` 均为 false 的失败工具结果。属于这一类的有：JSON-RPC 错误 `-32700`、`-32600`、`-32601`、`-32602`；`_meta["io.neograph/failure"].code` 为 `NG.InvalidRequest` 的 `isError` 结果；以及 `GrpcRemoteTool` 的状态 `NOT_FOUND` 与 `INVALID_ARGUMENT`。对于不符合工具输入 schema 的参数，`MCPServer` 现在会在结果中带上这个标记（此前是不带标记的 `isError` 结果）。可能发生在工具开始之后的失败（其他 `isError` 结果、`-32603`、服务器错误），在策略不能排除效果时仍与以前一样为 `reconciliation_required`。**行为变化：** 被拒绝请求的工具消息，以及 `MCPServer` schema 校验结果的 `_meta`。新增：`ToolRequestRejected` 与 `ToolRequestRefusal` 接口（`<neograph/tool_execution.h>`）；工具确知自己的调用没有运行时抛出前者，有自己基类的异常类型则实现后者。节点失败分类器把拒绝视为普通失败，仅当 `retryable()` 为 true 时才重试。对于被拒绝的 JSON-RPC 错误，`MCPTool` 仍然抛出 `MCPError`，捕获它的调用方不受影响。参见[工具失败契约](docs/reference-en.md#tool-failure-contract)。
- **`make_tool_service()` 和 `make_checkpoint_service()` 只包含各自的头文件就能使用（#425）。** 两者都返回 `std::unique_ptr<::grpc::Service>`，而 `<neograph/grpc/tool_service.h>` 和 `<neograph/grpc/grpc_checkpoint.h>` 只前向声明了 `grpc::Service`，因此调用方如果不另外包含生成的 `neograph.grpc.pb.h`，销毁返回值的代码就无法编译。现在两个头文件都包含 gRPC 服务基类头文件（`<grpcpp/impl/service_type.h>`）。测试在不包含任何其他 gRPC 头文件的编译单元中，分别构建这两个头文件。
- **串行 fan-out 提示只在各分支一个接一个运行时输出（#417）。** 在没有工作线程池的引擎上，只要 fan-out 的宽度不小于 2，就会输出 `[neograph] warning: fan-out ... executing serially` 提示，等待 I/O 的协程节点也不例外；这类节点在同一个线程上交错运行，`worker_count` 为 1 时三个分支也是同时在进行的。现在引擎会记录每个分支的节点主体运行的时间段，对静态 fan-out 和 `Send` 都是如此，只有任意两个分支都没有重叠时，才在 fan-out 结束后输出提示。**行为变化：** 提示出现在 fan-out 之后而不是之前，分支有重叠时则不再出现。同步分支（`SyncGraphNode`、Python 节点）仍然每个引擎输出一次，`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` 也仍然可以关闭它。
- **Program：fork 会重放它所继续的调用在提问前发出的 provider 调用（#405）。** 已暂停 run 的 fork 会在 fork 的 run 下再次运行被暂停的调用。按 run 记录调用的 SQLite provider journal 会把该调用在提问前发出的 provider 调用再次发送并再次计费，而恢复源 run 时这些调用是重放的。现在 fork 的每次尝试都会把源 run（`ProgramCoreProviderCallContext::replay_source`）告知 broker，journal 则接管源 run 对同一调用的记录，即 fork 之前记录在源 Core 线程同一槽位上的同一请求。结果会以 fork 的 dispatch id 重新编码，该行保留源的尝试编号，因此该调用既不会再次发送，也不会再次计费；结果未知的源调用在 fork 中仍是不确定的。没有 journal 的进程内 broker 仍会再次发送，与节点重试相同。
- **Program：在发布检查点之前停止的 typed plan 会被恢复，不再被阻止（#404）。** 对于不是 replay-safe 且尚未发布检查点的 plan，`reconnect()` 会抛出 `P_RUN_RECOVERY_BLOCKED`。现在它遵循 JavaScript `callCore` 的规则（#402）：plan 的每个调用按 plan 顺序，从自己启动的 Core 运行的最新检查点继续；第一个没有保存任何检查点的调用，会连同指明该操作的 `pending_effect`（`core_node` 为 `"plan"`）把 run 以 `Interrupted` 交回。以 `Completed` 调用 `reconcile()` 会把结果记录为该操作的结果，plan 从那里继续；`Unknown` 则让 run 保持 `AmbiguousEffect`。
- **Program：停止的 JavaScript `callCore` 从自己的 Core 检查点继续（#402）。** 进程在 `callCore` 中停止的 run，即使调用的 Core 已保存检查点，`reconnect()` 也会把它交回协调。因此已经完成的调用，或在 run 尚未发布的问题上暂停的调用，都必须手动协调。现在恢复会让待处理命令的每个调用，从该调用自己 Core 线程的最新检查点继续，并且不提供任何回答：已完成的调用重放其结果，已暂停的调用再次提问，join 的成员也以同样方式继续。未保存检查点的调用可能已经开始却没有留下痕迹，因此仍会交回协调，除非可以确定它从未开始（命令日志没有记录其开始的、带超时的 `ng.await`）。最新检查点是 `interrupt_before` 或 `interrupt_after` 暂停的调用同样交回：恢复绝不会越过未回答的闸门。重新启动的尝试现在在绑定操作时，会像计金额一样，对重启前记入日志的调用计 token，因此从检查点重放的调用只计费一次。
- **Program：以 Completed 协调 JavaScript 命令后，`main()` 会继续（#403）。** 命令会预留 run 剩余的全部 wall time 和 Core 步数，而被协调的命令一直占着这些预留，因此 `main()` 在协调后立即以 `TimedOut`（"Program wall-time budget (0 ms) expired"）结束。当 run 带有前一个命令的检查点时，用于协调的暂停还会释放协调本应结算的预留。现在协调一个命令会把它的 wall time 和 Core 步数还给 run，暂停也会保留预留，直到协调完成。命令预留的 token 和金额仍视为已花费，因为无法知道它用了多少。
- **ACP 接受 Windows 工作区路径。** `session/new` 和 `session/resume` 过去会拒绝所有不以 `/` 开头的 `cwd` 与 `additionalDirectories` 条目，因此 Windows 上的 ACP 客户端无法打开会话。现在与 POSIX 路径一样，带盘符的路径（`C:\work`、`C:/work`）和 UNC 共享（`\\host\share`）也算绝对路径；相对路径和盘符相对路径（`C:work`）仍返回 `-32602`。
- **Program：预算维度留空的 task graph task 也按它实际获得的量计入（#412）。** `TaskGraphProposal` 的 task 把 `budget` 的某个维度留成 0 时，proposal 与 fragment 的上限把该维度算作 0，但子运行启动时却拿到该维度 link 的全部上限；留空的 `output_bytes` 则允许任意大小的输出。于是 planner 能让 expansion 花掉比所检查的汇总上限更多的资源。proposal 的 schema 2 堵上了这个缺口：task 必须为模板上限非 0 的每个维度写明非 0 的值（`P_PROPOSAL_BUDGET_ZERO`），schema 2 的 fragment 按预留的值原样启动每个 task。与 `RunBudget` 一样，0 的维度不授予任何额度，输出被限制在写明的字节数内。schema 1 的 proposal 和 fragment 保持原有的 canonical 字节、identity 与启动行为，已存数据和现有宿主不受影响；接收 planner 提案的宿主把 `TaskGraphProposalLimits::min_schema_version` 设为 2 即可拒绝 schema 1。被拒绝的 expansion proposal 现在会在 `failure.witness.diagnostics` 中报告前几条 diagnostic 的代码和指针。
- **Program：fork 会重放其继续的调用所发出的 provider 调用（#405）。** 已暂停 run 的 fork 会在 fork 的 run 下再次运行被暂停的调用，而按 run 记录调用的 SQLite provider 日志会再次发送并再次计费该调用在提问前发出的 provider 调用；源 run 的恢复则会重放它。现在 fork 的每次尝试都会把源 run 告知代理（`ProgramCoreProviderCallContext::replay_source`），日志会接管源 run 对同一调用的记录，即 fork 之前在源 Core 线程的同一槽位上记录的同一请求。结果会以 fork 的 dispatch id 重新编码，该行保留源的尝试编号，因此该调用既不会再次发送也不会再次计费；结果未知的源调用在 fork 中仍是不确定的。没有日志的进程内代理仍会再次发送，与节点重试相同。
- **Program：在发布检查点之前停止的 typed plan 会被恢复，而不再被阻止（#404）。** 对于不是 replay-safe 且尚未发布检查点的 plan，`reconnect()` 会抛出 `P_RUN_RECOVERY_BLOCKED`。现在它遵循 JavaScript `callCore` 的规则（#402）：每个 plan 调用按 plan 顺序从其启动的 Core 运行的最新检查点继续，第一个未保存任何内容的调用会以 `Interrupted` 交回 run，并带有指明该操作的 `pending_effect`（`core_node` 为 `"plan"`）。以 `Completed` 调用 `reconcile()` 会把结果记录为该操作的结果，plan 从那里继续；`Unknown` 让 run 保持 `AmbiguousEffect`。
- **Program：停止的 JavaScript `callCore` 从其自身的 Core 检查点继续（#402）。** 进程在 `callCore` 中停止的 run，即使调用的 Core 已保存检查点，`reconnect()` 也会把它交回协调，因此已完成的调用、或在 run 尚未发布的问题上暂停的调用都必须手动协调。现在恢复会让待处理命令的每个调用从该调用自身 Core 线程的最新检查点继续，且不提供任何回答：已完成的调用重放其结果，已暂停的调用再次提问，join 的成员也以同样方式继续。未保存检查点的调用可能已经开始却未留下痕迹，因此仍会交回协调，除非可以确定它从未开始（命令日志未记录其开始的带超时的 `ng.await`）。最新检查点是 `interrupt_before` 或 `interrupt_after` 暂停的调用也同样交回：恢复绝不会越过未回答的门。重新启动的尝试现在在绑定操作时，会像对金额一样对重启前记入日志的调用的 token 计费，因此从检查点重放的调用只计费一次。
- **Program：以 Completed 协调 JavaScript 命令后 `main()` 会继续（#403）。** 命令会预留 run 剩余的全部 wall time 和 Core 步数，而被协调的命令一直保留着它们，因此 `main()` 在协调后立即以 `TimedOut`（"Program wall-time budget (0 ms) expired"）结束；当 run 带有前一个命令的检查点时，用于协调的暂停还会释放协调本应结算的预留。现在协调一个命令会把其 wall time 和 Core 步数还给 run，暂停也会一直保留预留直到协调。命令预留的 token 和金额仍视为已花费，因为无法知道它用了多少。
- **A2A 任务流在运行安静时发送 SSE 注释。** `SendStreamingMessage`、`message/stream`、`SubscribeToTask` 和 `tasks/resubscribe` 的流，在 Task 与下一个状态之间什么也不发送。因此在单个节点中停留时间超过客户端读取超时（httplib 和 httpx 默认 5 秒）的运行，会在仍在工作时被断开，订阅者看到的是“无响应”，而不是最终状态。现在静默 5 秒的流会写出 SSE 解析器会忽略的注释 `: keep-alive`。**行为变化：** 流的 `data:` 事件之间可能出现注释行，不使用 SSE 解析器、直接读取原始行的客户端必须跳过它们。订阅者加入运行中任务的路径没有缺陷：Task、游标和通道在同一把锁内取得，运行的最终状态、通道关闭和运行的移除也在同一把锁内完成。
- **gRPC 检查点失败不再表现为“没有检查点”。** `CheckpointService` 过去在加载时对后端故障回答“未找到”、对列表回答空列表，因此恢复的运行会悄悄从头开始，`GrpcCheckpointStore::delete_thread` 也会把失败的删除报告为成功。现在后端失败为 `UNAVAILABLE`，客户端会抛出异常；失败的删除会抛出异常；格式错误的 `SaveCheckpoint` 为 `INVALID_ARGUMENT`。`GrpcCheckpointStore` 为每个 RPC（`set_rpc_timeout`，默认 30 秒）、`GrpcRemoteTool` 为每次调用（`set_call_timeout`，默认 60 秒）设置截止时间。
- **NeoGraph 的 MCP 客户端在停止等待时会告知服务器。** 客户端放弃的请求，无论是取消还是超时，随后都会发送 `notifications/cancelled`（`initialize` 除外），使服务器可以停止工作；过去服务器会把被放弃的工具运行到底。
- **运行被取消时，进行中的 MCP 调用会停止。** `MCPTool` 过去忽略分发器的 cancel token 与截止时间，因此取消运行或发生尝试超时时仍会等待服务器的应答（于是尝试会越过停止截止时间并以 `NG.EffectUncertain` 失败）。现在它在 stdio 与 HTTP 上都遵守二者：取消为 `CancelledException`（`cancellation_requested`），截止时间或请求超时为 `timed_out` 调用，迟到的响应会被丢弃。`grpc::GrpcRemoteTool` 遵循同一规则：运行的 cancel token 会取消其 RPC，运行截止时间即为 RPC 截止时间。
- **格式错误的 UTF-8 不再清空文本。** `json::dump` 过去对含有格式错误字符串的文档返回空字符串，因此失败的工具消息、MCP 响应行或 gRPC payload 会以空内容到达，Python 也会引发 `UnicodeDecodeError`。现在在每个边界上，每个格式错误的最大子序列都变为 U+FFFD，格式正确的文档字节保持不变。
- **工具代码抛出的系统错误是工具失败。** 运行中的工具抛出的 `std::system_error`（文件不存在、连接被拒）过去被报告为准入状态 `rejected`；现在为 `failed` 或 `reconciliation_required`。主机准入的拒绝现在与资源仲裁器一样为 `rejected` 或 `expired`，而不是 `failed`。
- **抛出 `HookBoundaryBlocked` 的 Python 节点不会被重试。** C++ 节点与 Python 工具的 Hook 阻断会停止运行，而 Python 节点的 Hook 阻断过去是会被重试和路由的普通失败；现在它会在不重试的情况下停止运行。
- **过长或非 UTF-8 的 Hook 失败也会被记录。** 超过 4096 字节、或在 UTF-8 序列中间被截断的 Hook 目标失败消息过去会使回执无效，并以不相关的错误使 Hook 边界失败；现在回执文本会在码点边界处截断并修复。
- **Python `mcp.MCPError` 保留 `code` 与 `data`。** 过去只有消息到达 Python；现在 JSON-RPC 错误的 `code` 与 `data` 作为属性提供。
- **A2A：异常文本不再到达 peer。** 失败 run 的 status message 曾包含 `what()`（例如异常消息中的路径或 credential）。现在 peer 只收到失败的 public message 和 `metadata["neograph/failure"] = {code, retryable}`；失败的 Program run 同样如此。在任何 node 完成之前的 hook 阻断为 `REJECTED`，之后为 `FAILED`。`retryable` 与 MCP、gRPC 和 ACP 一样来自 `graph::classify_failure`，现在 provider 失败（`NG.ProviderFailed`）也为 `true`：node retry loop 把它交给 provider runtime，但新的请求可能会成功。
- **A2A：interrupt 说明所需内容，后续 message 恢复它。** `INPUT_REQUIRED` 以 text 携带 interrupt 的 reason，以 data part 携带其 payload，发给该 task 的 message 从 checkpoint 恢复 run，而不是重新开始运行 graph。
- **A2A：请求和 card 遵循规范。** 无效 params 得到带 field violation 的 `-32602`，格式错误的 envelope 和 batch 得到 `-32600`，每个 error 都带有 `google.rpc.ErrorInfo` detail。skill 总有 tag，1.0 card 不含 0.3 字段，card 支持 `ETag`、`Cache-Control` 和 `If-None-Match`。0.3 stream 以 task 开始，以 `final: true` 结束。
- **Python example 28 和 `test_protocol_sdk_e2e.py` 可在 agent-client-protocol 0.12 和 1.0 上运行。** 1.0 移除了它们导入的 `text_block`/`image_block`/`update_*` helper。
- **从未开始的中介工具调用会释放其槽位。** `SQLiteToolEffectBroker` 过去把未开始就结束的调用（`expired`、`rejected`、`cancelled_before_start`）记录为 `reconciliation_required` 并让其槽位保持待定，因此运行会为对账而暂停，该槽位之后的每次调用也是如此。现在它按原状态将调用作为终态且效果确定的结果返回，并释放槽位；节点尝试的内存记录本来就是这样处理的。
- **超过 `output_limit_bytes` 的工具输出，在调用可能已产生效果时为效果不确定。** 过去它总是 `effect_uncertain` 为 false 的 `failed`；现在它遵循调用开始之后所有失败的规则：除非策略为 `ReadOnly`、`ExternalRead` 或 `Idempotent`，否则为 `reconciliation_required` 且效果不确定。
- **工具执行控制器不再重新启动超时的 Hook 目标。** Hook 运行器已将超时的原生目标作为最终状态处理，但运行该目标的控制器在目标策略允许重试时（幂等或只读目标的 `retry_max_attempts` 大于 1），会在这一次尝试内重试目标，因此可能仍在运行的目标会在其旁边再次启动。现在超时的 Hook 目标尝试在控制器中同样是最终状态；工具调用保留其策略的重试。
- **MCP 工具参数和结果按 JSON Schema 2020-12 校验。** 过去服务器和客户端各自只检查一部分关键字：服务器会运行参数违反 `minimum`、`anyOf` 或 `$ref` 的工具，两者都不把 `1.0` 当作整数。现在一个验证器为服务器、客户端和 Harness 处理 core、applicator、unevaluated 与 validation 词汇以及本地引用；客户端遇到无法检查的方言的模式时跳过检查，而不是让调用失败。
- **MCP 错误应答不再携带 `"id": null`。** 过去对能读取 id 的无效信封、解析错误、批处理以及每个 HTTP 错误正文都以 null id 应答，而 2025-11-25 模式不允许这样做；现在回传可读取的 id，无法读取 id 的错误不带 id。
- **NeoGraph 的 MCP 客户端能读取官方服务器的事件流。** 过去只在 `"\n\n"` 处拆分 Server-Sent Events，因此在官方 Python 服务器的 CRLF 流中会丢失通知之后的每个应答（会记录日志或报告进度的工具）；现在遵循 WHATWG 规则，并在应答到达后停止读取。服务器在应答之前关闭的流会在其 `retry` 间隔后用 `GET` 和 `Last-Event-ID` 续接。
- **NeoGraph 的 MCP 客户端应答服务器请求。** 在 stdio 和 HTTP 调用的事件流上，服务器的 `ping` 得到空结果，其他请求得到 `-32601`；过去这些请求得不到应答，因此会 ping 客户端的服务器会失败。复用待处理调用 id 的服务器请求不再完成该调用。
- **NeoGraph 的 MCP 客户端恢复过期会话并结束自己的会话。** 对携带 `Mcp-Session-Id` 的请求返回的 HTTP 404 会开始一个新会话并重新发送该请求一次；过去客户端会一直发送失效的会话 id。`shutdown()` 和最后一个引用的释放会发送 `DELETE`，URL 构造函数 `MCPTool(url, ...)` 为所有调用保持一个会话，而不是每次调用都打开一个从不结束的会话而耗尽服务器的会话上限。
- **MCP HTTP 错误正文中的 JSON-RPC 错误保留其代码。** 以 HTTP 错误状态和 JSON-RPC 错误正文拒绝请求的服务器（例如只使用 MCP `2026-07-28` 的服务器返回的 `-32022`）过去会引发以正文为文本的传输错误；现在是带有代码、data 与 `http_status()` 的 `MCPError`。
- **被取消的图运行会通知 MCP 服务器。** 当运行的取消令牌取消了等待 MCP 调用的协程时，传输过去把中断的等待报告为关闭或连接失败，且不发送 `notifications/cancelled`；现在这算作取消，并通知服务器。
- **MCP HTTP：取消不会被饿死，应答超时会取消工具，`DELETE` 会关闭其连接。** cpp-httplib 的线程池可能让 `notifications/cancelled` 排在它本应停止的调用之后；现在 HTTP 线程按需启动，最多到 `max_http_threads`。超过 `response_timeout` 的请求除了返回 504，还会取消其工具调用。像官方 Python 客户端那样带 `Content-Type` 而无正文的 `DELETE`，过去会使同一连接上的下一个请求丢失。
- **结构化的 MCP 工具结果也以文本携带其 JSON。** Harness 工具和示例 59 过去返回 `structuredContent`，但没有 MCP 为无法读取结构化内容的客户端所要求的、位于文本块中的序列化 JSON。
- **Program：恢复的尝试在崩溃恢复后以其恢复值继续 (#377)。** 崩溃恢复会以运行的输入重新执行由 `resume()`、`reconcile()` 或 `fork()` 启动的尝试，而不是该尝试恢复其检查点时使用的值。`define()` Program 的节点把运行的输入当作恢复值，`main()` Program 中被中断的 `callCore` 以命令的输入恢复，绑定了提供者时 SQLite 日志会以 "logical call binding conflict" 失败（fail closed）。运行记录现在在尝试开始前持久地保存该值 `ProgramRunRecord::attempt_resume()`，恢复时以它继续。带有该值的记录使用运行记录存储模式 5；其他记录保持各自的模式、字节和 id，已存储的记录照常加载。`ProgramRunRecordData` 新增了成员：请重新构建 C++ 使用方（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。派生（fork）的 `main()` Program 的第一次尝试尚未覆盖。
- **Program：重放的提供者调用的令牌每次运行只计费一次 (#378)。** 在使用 SQLite 提供者日志的中断与恢复中，恢复的尝试会再次计入先前尝试已记录并结算的调用的模型令牌：没有新请求，剩余的 `model_tokens` 却从 999,976 降到 999,952。日志仍然识别这些调用，因此既不重发也不重复计数，但只为尝试计入它自己记录的调用，#372 的金额计费已经这样做。崩溃前记录的调用在恢复后仍计费一次，节点重试的新调用照常计费。
- **Program：join 报告使其停止的失败 (#379)。** fail-fast 的 `ng.all` 按声明顺序报告第一个失败的成员，因此一个先声明、因另一成员失败而被取消的兄弟会取代那个失败：以 `P_MONEY_BUDGET` 被拒的成员被报告为操作已取消。join 现在报告使其停止的那个成员的失败；使用 `collect` 时仍按声明顺序报告第一个失败。计划级的 `parallel` 操作也有同样的缺陷，被取消的分支会取代使其取消的失败。`ng.race` 与 `ng.quorum` 原本就是正确的，现在由测试固定。
- **Program：join 成员的中断会暂停 join，恢复时重放已结束的成员 (#380)。** 成员的 Core 发生中断的 `ng.all` 无法正确恢复。运行保留的是最后写入检查点的那个成员的检查点，因此 SQLite 宿主恢复了一个已经完成的成员，join 再次变为 `Interrupted`；内存宿主重新运行已完成的成员，把它的模型调用发送并计费了两次；兄弟成员中断时仍在运行的成员被取消，被放弃调用的预留仍然保留。现在成员的中断会暂停其 join：join 不再启动新成员，让已在运行的成员运行结束，并保留它们得到的全部结果。如果这些结果按 join 自身的规则结束了 join（fail-fast 失败、race 的胜者、已满足或已无法达到的 quorum），join 就以此结束，因此 `ng.race` 与 `ng.quorum` 在可能时无需被暂停的成员即可结束。否则 join 以声明顺序中第一个被暂停的成员变为 `Interrupted`，其输出列出目前为止的结果，命令日志记录已结束的成员。`resume()` 以恢复值继续被暂停的成员，并重放已记录的成员，不会再次运行或再次计费；其余成员会重新运行。内存宿主与 SQLite 宿主行为相同，嵌套的 join 也一样。记录已结束成员的命令日志条目使用条目存储模式 2；其他条目保持各自的模式、字节和 id。`ProgramJavaScriptCommandJournalEntryData` 新增了成员：请重新构建 C++ 使用方（参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)）。
- **Program：因过期的 execution lease 而结束的运行会计入其日志中调用的令牌 (#381)。** 当停止进程的根 execution lease 过期、`reconnect()` 以 `TimedOut` 结束运行时，该尝试记录在日志中的提供者调用只计入了金额（#372），没有计入模型令牌：`usage.model_tokens` 为 0，`remaining_budget.model_tokens` 仍保留着这些令牌。现在两者都计入一次。`ProgramCoreProviderCallContext` 新增了 `journaled_usage`，即日志型 broker 为这类运行绑定时计费的令牌账户：请重新构建 C++ 使用方。
- **Core：恢复只会跳过它所回答的那个中断的闸门 (#382)。** 任何恢复之后的第一个 super-step 都会跳过其 `interrupt_before` 检查。进程在某一步的提交与下一步的中断保存之间停止后，恢复会从该步骤的检查点继续，被闸门拦下的节点未经批准就运行了。`interrupt_after` 之后的恢复，或 `update_state(..., as_node)` 之后的恢复，同样会不经询问就运行下一个带闸门的节点；`interruptAfter` 后接 `interruptBefore` 的 Program 会走到这条路径。现在只有恢复 `interrupt_before` 暂停才会运行被拦下的节点，恢复 `NodeInterrupt` 会继续其所在的步骤；其他任何恢复仍会在即将运行的 `interrupt_before` 节点之前暂停。`ResumeContext` 新增了 `current_node`：请重新构建 C++ 使用方。
- **Core：每条检查点保存路径都写入相同的运行元数据 (#383)。** 节点执行器为 `NodeInterrupt` 保存的检查点在构建元数据时，不包含引擎自身保存路径所添加的 `barrier_stalled` 与 `failure_exit`。现在每条路径都用同一个辅助函数构建元数据，该函数根据检查点是在步骤进行中保存还是在步骤之间保存来处理 `max_visits` 计数器。在当前调度器下，运行停滞或到达 failure exit 之后不会再发生 `NodeInterrupt`，因此此前并没有检查点出现差异；测试会针对相同的运行状态比较每条路径保存的检查点。
- **Program：被中断的分支会暂停 plan 级 `parallel` (#384)。** 两个分支都被中断的 plan 级 `parallel`（带类型的 operation plan，而非 `ng.all`）只恢复了第一个分支：第二个分支要么以恢复值作为输入重新运行，要么在每次恢复时再次询问，而已经完成的分支会再次运行并再次计费。现在 `parallel` 遵循 #380 的 join/interrupt 约定：失败的分支仍会结束它；否则它会在声明顺序中第一个暂停的分支上进入 `Interrupted`，运行从该分支的检查点恢复。恢复会继续待处理问题所指的 Core 调用，重放已完成的分支而不再运行或计费，并以暂停状态重放其他暂停的分支，使其再次提出自己的问题。这些记录作为运行记录的 `ProgramRunRecord::settled_branches()`，保存在 `Interrupted` 记录以及恢复它的尝试的 `Running` 记录上；携带它们的记录使用运行记录存储 schema 6，其他记录保持原有 schema 与 id。`ProgramRunRecordData` 新增了 `settled_branches`：请重新构建 C++ 使用方。
- **soundness 检查：访问次数只在 failure 目标完成时重置 (#364)。** 可选的 soundness 检查在节点的 failure edge 目标仅仅运行时就会重置该节点的 `max_visits` 计数，即使目标自身失败并走了它自己的 failure edge；运行时只在目标完成时重置。现在检查也采用同样的方式，并会探索节点停留在上限的状态。
- **Core：中断的 `Send` target 会暂停 run (#385)。** `Send` 的 target 抛出的 `NodeInterrupt` 会以异常形式逃出 run 且不留下检查点，因此无从恢复；map 步骤把 Send 分派给提问节点的 Program 会以 `P_RUNTIME_CORE_FAILURE` 失败。现在 run 会像 ready batch 中的节点一样暂停，在所有同步、异步和流式入口以及 Python 中都是如此。`NodeInterrupt` 检查点从该 super-step 开始时的状态重新进入整个 super-step，暂停的结果也报告该状态。恢复会重放发出 Send 的节点以及每个已完成的 Send，因此它们只运行一次、只计费一次；提问的 Send 以相同的 task id 带着答案运行，因此它记入日志的 provider 调用会被重放。在前面没有检查点的步骤中（例如位于入口的 map 节点），已完成的工作会记录在中断检查点之下，并且在这类步骤中已完成的 ready batch 节点现在也会被重放，而不是再次运行。
- **Program：plan 恢复从 plan 停止的位置继续 (#386)。** 恢复暂停的 plan 级 Program（带类型的 operation plan，而非 `main()`）时，会以恢复值作为状态从根重新运行 plan：暂停调用之前的 operation 会再次运行并再次计费（token、金额、Core 步骤和 Program operation），`branch` 也会用恢复值求值条件。现在暂停的 run 记录除了已完成的 `parallel` 分支结果和其他暂停调用的问题之外，还按 operation id 保存 run 正在执行的每个 operation 的位置（`sequence` 的下一个子节点、`loop` 的迭代、`retry` 的尝试、`map` 的项或 `race` 的胜者，以及该位置的状态；根上还保存 plan 的输入）。resume、reconcile、fork、graph migration 及其各自的恢复都会携带这些记录：已完成的部分既不会再次运行也不会再次计费，`branch` 看到的是它第一次看到的状态，只有被回答的调用会收到恢复值。fork 会为每个记录中的暂停调用单独提供其检查点的副本，`reconcile` 留为模糊状态的 effect 也会保留这些记录。`parallel` 和 `race` 的分支 0 现在在该 operation 自己的 Core 线程上继续，因此嵌套 `parallel` 的第一个分支不再与根的第一个分支并行地运行在根线程上；没有嵌套 parallel 的程序保持其线程 id，而被恢复回答的调用会在其检查点所在的线程上继续，因此在此变更之前暂停的 run 仍可恢复。这些记录扩展了 `ProgramRunRecord::settled_branches()`（#384），暂停调用的记录现在以调用自身的 operation id 为键；在 `sequence`、`loop`、`retry`、`map`、`race` 或 `branch` 内暂停的 plan 现在存储 run 记录模式 6，`P_PLAN_PARALLEL_RECORD` 现在是 `P_PLAN_REPLAY_RECORD`。
- **Core：因 `NodeInterrupt` 暂停的 run 会报告它保存的检查点 (#386)。** 之前它报告的是存储中该线程的最新检查点。SQLite 和 PostgreSQL 按毫秒时间戳、再按 step 对线程的检查点排序，而同一线程上的每个 run 都从 step 0 重新开始；因此，当同一线程上较早的 run 在同一毫秒内保存了最后一个检查点时，该检查点就是最新的。恢复所报告的检查点会返回较早 run 的最终状态，而不会运行暂停的节点：在 SQLite 写入立即返回的 macOS 上，恢复的 Program plan 调用没有运行任何 Core 步骤，并返回了前一个调用的输出。现在，节点、并行步骤或 `Send` 因 `NodeInterrupt` 而暂停的 run，其 `RunResult::checkpoint_id` 就是 run 为该暂停保存的检查点。
- **Core：在 SQLite 和 PostgreSQL 上，线程的最新检查点是最后保存的那个 (#386, #396)。** `SqliteCheckpointStore` 和 `PostgresCheckpointStore` 按毫秒时间戳、再按 step 选出线程的最新检查点，并以此为 `list()` 和 `get_state_history()` 排序。两者都相同或 step 回退的保存会以错误的顺序出现：同一线程上较晚的 run 从 step 0 重新开始（Program plan 的下一个 Core 调用），同一 step 的两个 `interrupt_before` 门各自保存该 step 的检查点 (#396)，而在 SQLite 写入立即返回的 macOS 上，多次保存落在同一毫秒内。于是 `resume()` 从比最后保存的检查点更早的检查点继续，再次回答了已经回答过的门或中断，导致 run 再次暂停，或某个节点收到了另一个问题的答案。现在两个存储都像 `InMemoryCheckpointStore` 一样按保存顺序为线程的检查点排序，`load_latest()` 就是 `list()` 的第一项。保存线程已持有的检查点 ID 时，三个存储都会替换该检查点并使其成为最新；内存存储不再把这样的 ID 列出两次。每个存储在打开数据库时为 `neograph_checkpoints` 添加可为 NULL 的 `save_seq` 列。PostgreSQL 用序列 `neograph_checkpoints_save_seq` 为保存编号，该序列也是列的默认值，因此不设置该列的写入方所写的行同样会被编号；SQLite 取该线程最大编号加一，写入方未编号的行由触发器编号。迁移不重写任何行，重复执行也没有影响：迁移前保存的行保持 `save_seq` 为 NULL 及其 (timestamp, step) 顺序，排在之后保存的所有行之后。索引 `neograph_checkpoints_save_order` 取代 `neograph_checkpoints_recent`。上一个版本仍可打开已迁移的数据库，它保存的行也会被编号。
- **SQLite：同时打开同一个新数据库的存储都能打开。** 当另一个存储或进程在同一时刻打开同一个新数据库文件时，存储可能无论 busy timeout 多长都会立即以 `database is locked` 失败：两个连接同时把同一文件切换到 WAL 时，二者都持有读锁，由于等待可能死锁，SQLite 对第二个请求写锁的连接不调用 busy handler，直接返回 `SQLITE_BUSY`。macOS 上的两个测试进程遇到了这种情况。现在 `SqliteCheckpointStore`、`SQLiteContextStore`、`SQLiteHookJournal`、`SQLiteProviderDispatchReceiptStore`、`SQLiteToolEffectBroker`、`SQLiteProgramProviderCallJournal` 和 `SqliteHarnessRecordStore` 会在 busy timeout 用尽之前重试该切换，之前在切换之后才设置 timeout 的存储现在先设置它。`SqliteCheckpointStore` 在一个 `BEGIN IMMEDIATE` 事务中执行其模式和迁移，若该事务在 timeout 之前以 `SQLITE_BUSY` 结束则重新执行。`GateIdentity` 和 `ResumeValueScope` 测试为每个用例独占创建一个随机命名的目录，因此同时开始的两个用例不再共享或删除同一个数据库。
- **构建：MSVC 以 `/bigobj` 编译所有目标。** 在 Windows AddressSanitizer 构建中，`src/program/runtime.cpp` 超出了 COFF 目标文件的节数上限（`C1128`）。现在顶层 `CMakeLists.txt` 将 `/bigobj` 与 `/utf-8` 一起传给所有 MSVC 目标，因此库、测试、Python wheel 和已安装包的构建都会使用它；仅 `neograph_program_tests` 拥有的该标志已移除。
- **Core 和 Program：fork 已暂停的 run 时会重放已完成的工作 (#390)。** 对 `NodeInterrupt` 检查点执行的 `GraphEngine::fork`、Program 的 fork，以及已暂停调用的检查点副本都会丢弃检查点的 pending write，因此在被中断的 super-step 中已完成的节点和 `Send` 目标会在 fork 出的 run 中连同其效果和 provider 调用一起再次运行。现在 fork 会复制 resume 源 run 时要重放的 write，fork 出的 run 与 resume 一样进行重放：已完成的工作不会再次运行、发送或计费。Python 主机可以用 `LocalProgramHost.fork(run_id, value, version, budget, ...)` fork 一个 Program run。
- **Program：`ng.await` 在 resume 和恢复之后仍遵守截止时间 (#391)。** `main()` 中的 `ng.await(..., timeout_ms)` 每次 resume 后都会从零重新计时，因此每经历一次 interrupt 和 resume，作者写下的截止时间就被延长一次。现在 await 开始时会把开始时间记录到 command journal（带 `await_started_at` 的 pending 条目，command journal 条目 schema 3），resume 或恢复后的 attempt 只等待剩余时间；计划级的 `await` 把开始时间记录在其重放记录 (#386) 中。没有该字段的 journal 条目保持原有的 schema 和字节。
- **Program：被中断的分支会暂停计划级 `quorum` (#387)。** 计划级 `quorum`（类型化操作计划，而非 `ng.quorum`）把 Core 调用被中断的分支计为未成功，并继续执行下一个分支：问题被丢弃，quorum 依靠后面的分支完成或失败。现在它遵循 #380 和 #384 的 join/中断契约：暂停的分支计为仍可能达到的成功，因此除非已完成的分支已经决定了 quorum，否则它会暂停 quorum，并且不再启动后续分支。run 记录的 `settled_branches` 保存每个自行结束的分支的结果；恢复会重放它们而不再次运行或计费，用恢复值继续暂停的分支，然后从那里继续；后面的分支提问时 quorum 会再次暂停。剩余分支已无法达到 `min_success` 的 quorum 现在以 `P_QUORUM_UNREACHABLE` 失败，而不是以较少的成功完成，并且 quorum 会报告其分支的执行轨迹。
- **Core：超步中的每个中断单独回答 (#388)。** 当一个超步中的多个 ready 节点或 `Send` target 抛出 `NodeInterrupt` 时，一次 `resume(value)` 会把同一个值交给所有中断，因此第二个问题被第一个问题的答案回答，且从未被提出。现在暂停的 `RunResult` 在 `interrupts`（`{id, node, value}`，id 是暂停调用的 task id）中列出该步的所有中断，并通过 `interrupt_node`、`interrupt_value` 和 `interrupt_id` 报告第一个。恢复回答 `RunMetadata::resume_interrupt_id`（Python 中为 `resume`、`resume_async` 和 `resume_from` 的 `interrupt_id=`）指定的中断，或第一个中断；该步的其他调用在没有该值的情况下运行并再次提出自己的问题。不在等待中的 id 会在任何内容运行之前以 `std::invalid_argument`（`ValueError`）被拒绝。在多个中断上暂停的检查点在 `_neograph.interrupts` 中列出它们；在一个中断上暂停的检查点保持其字节不变。此类步骤的 Program pending input 或 effect 在 `core_interrupt_id` 中指明其 Core 中断（pending 值存储模式 2；其他值保持模式 1、字节和 id），恢复 run 时正好回答该中断；JavaScript join 以及计划级 `parallel` 和 `quorum` 已经每次恢复回答一个暂停的成员。
- **Core：一个 `Send` 和多个 `Send` 遵循同一契约 (#389)。** 只有一个 `Send` 的步骤把 payload 应用到共享 run 状态并在其上运行 target，而有多个 Send 的步骤在隔离副本上运行每个 target；被重放的单个 Send 则完全不应用 payload。现在每个 Send 都在应用了自身 payload 的状态副本上运行，带重试，无论是实时运行还是重放，只有它返回的 write 会进入共享状态。已更正 `executor.h` 中称多 Send 路径不重试的注释：每次 Send 尝试都在其自身的 effect namespace 中重试（#364）。
- **测试：计划暂停测试在没有 QuickJS 时也能构建。** `JoinTestStores` 位于 `tests/test_program_runtime.cpp` 的 QuickJS 专用块内，而块外的计划级 `parallel` 和恢复测试（#384、#386）使用了它，因此在 `NEOGRAPH_BUILD_QUICKJS_CONTROL=OFF` 时该文件无法编译。共享存储现已移到该块之外，并用守卫包住了三个 admit JavaScript 源的测试。
- **Core：恢复值只送达它所回答的调用 (#393)。** 恢复之后，`ctx.resume_value` 会留在恢复后 run 的所有后续超步的所有节点上，因此之后请求批准的节点，或在工具运行前提问的 `ToolGate`，会把先前的答案当作自己的答案而不提问：一次批准回答了另一个问题。现在该值只送达恢复重新进入的那一步中的唯一一个调用，包括它的每一次尝试：恢复所回答的 `NodeInterrupt` 的节点或 `Send`（#388），或 `interrupt_before` 闸门拦下的节点。其他所有调用以及后续步骤的所有节点都看不到它，因此提问的节点或闸门会再次暂停 run。通过父图恢复的子图经由父图中被回答的节点收到自己的答案，Program 的 `call_core` 和 JavaScript 的 `callCore` 恢复也遵循同一规则。
- **Core：fan-out 步骤和同一步的 Send 按同一条规则结束 (#392)。** 兄弟节点既有失败又有中断的步骤是暂停还是失败，取决于哪一个的索引更小。现在，fan-out 分支和 Send，无论一个 worker 还是多个，无论索引或完成顺序如何，都遵循同一条规则：取消会停止 run；否则，不确定的效果（`tool_effect_reconciliation` 中断）即使在失败旁也会让该步为对账而暂停，只报告这些中断，失败的兄弟节点会在恢复时再次运行；否则，派发顺序中的第一个失败使 run 失败，该步的普通中断不会被报告；否则，该步在所有中断上暂停（#388）。沿 failure edge 路由的失败（#364）是节点的结果，不会结束该步。JavaScript join 以及计划 `parallel` 和 `quorum` 早已在成员失败时结束（#380、#384、#387）。
- **测试：fork 回答其等待调用所指的 Core 中断 (#394)。** 有报告称，在带有多个中断的 Core 步骤上暂停的 Program 运行的 fork 不会把 `core_interrupt_id` 传给其第一次尝试。实际上会传：fork 把保留该 id 的源等待调用发布到其第一条运行记录上，尝试像恢复或对账一样从那里读取它。现在测试在内存和 SQLite 上固定了该记录和答案，并在 Python 中与一个不会被再次发送或计费的绑定 `llm_call` 一起固定。
- **Core：同一步中的两个 `interrupt_before` 闸门不再被一次恢复同时通过 (#396)。** 当同一超步中有两个带闸门节点就绪时，一次恢复会运行两者，第二个人工批准从未被询问。现在恢复回答 run 所停的闸门（以带闸门节点的 task id 标识，#388），该步其他每个带闸门节点都会在任何内容运行之前以自己的 Before 中断再次暂停 run。所有闸门都被回答后该步才运行，每个带闸门节点收到对其自身闸门的回答（#393）。
- **Program：join 与 Core 一样把对账暂停排在成员失败之前 (#397)。** JavaScript `ng.all`、`ng.race`、`ng.quorum` 以及计划级 `parallel` 和 `quorum`，即使另一个成员已为对账一个结果未知的效果而暂停，也会因成员失败而结束，从而丢弃该效果。现在它们遵循 #392 的优先级：取消、对账暂停、失败、普通中断。失败的成员会被记录，恢复时重放而不再次运行；计划级 `parallel` 会记录 Core 节点失败的分支，并再次运行被该失败取消的分支。
- **Program：JavaScript `main()` 的 fork 会恢复其暂停的 `callCore` (#395)。** fork 的第一次尝试既没有命令日志也没有 Core 恢复值，因此 `main()` 从头开始：已完成的命令再次运行，其 provider 调用和效果被再次发送和计费，所有提问再次提出，答案丢失。现在 fork 携带重新绑定到目标 bundle 的源命令日志，把暂停调用的检查点副本保存在该调用的 Core 线程上，以源输入运行 `main()`，并像恢复一样以 fork 的值恢复暂停的调用；第一次尝试会预留被恢复命令的资源，恢复（recovery）也以同样方式继续。
- **Program：fork 在正确的保管下重放已记录的 provider 失败 (#398)。** 一个在带有 provider 证据的失败旁暂停的运行，无论该失败记录在命令日志中的收集式 `ng.all`（#395）里，还是在计划的 `settled_branches`（#386）里，其 fork 都会在与记录不匹配的 fork 保管下恢复该证据，因而以保管不匹配失败。现在 fork 和图迁移在携带此类记录时，都会把它从源运行的保管重新绑定到新运行的保管，因此重放会恢复证据（`result.failure.provider_outcome`），且不再发送任何内容。计划 `parallel` 会像 quorum 一样存储已结束分支的 provider 证据。
- **Program：分支的 Core 节点失败时，计划 quorum 不再使运行失败 (#399)。** 节点抛出异常的分支曾被抛出 quorum，成为运行的失败。现在它是一个失败分支，带有运行本应报告的代码、节点、尝试次数和 provider 证据；当后续分支使 quorum 暂停时它会被记录，并在恢复时重放。取消和丢失的检查点仍会结束运行，#392/#397 的优先级保持不变。
- **Program：恢复的 join 不再取消它所答复的成员 (#400)。** 在失败成员旁为对账而暂停的（#397）fail-fast `ng.all`、`ng.race` 或 `ng.quorum`，恢复时会立即重放该失败成员；其完成会结束 join，并在对账成员的节点仍在消费答复时取消它，导致效果从未落定。现在被答复的成员先运行到结束，然后由重放的失败结束 join。
- **Program：恢复的 join 先确定它重放的成员 (#400)。** 恢复时 JavaScript join 按声明顺序启动成员，因此重放暂停前所记录结果的成员，可能在恢复所答复的成员已经结束之后才被登记。结果因此取决于时序：在失败成员旁为对账而暂停的 `ng.race`，可能以被答复成员的值完成，而不是以重放的失败结束。现在恢复的 join 先启动它重放的成员，然后是恢复所答复的成员，然后是其余成员。重放的成员总在任何运行的成员之前确定，因此 race 的胜者和 fail-fast join 的失败在每次运行中都相同。
- **Program：暂停的 join 中普通问题不再优先于不确定效果 (#401)。** 无失败的 join（无论是否为收集式）或计划级 `parallel` 会在声明顺序中第一个暂停的成员上暂停，因此普通问题先于因不确定效果而暂停的成员被询问。现在它与 Core 步骤（#392）一样先询问对账。收集式 `ng.all` 的例外（它等待所有成员，因此在失败旁也会在暂停的成员上暂停）被保留、记录在文档中并由测试固定。
- **Program：分支立即结束的 plan `parallel` 或 `race` 不再挂起。** plan 级 `parallel` 或 `race` 在运行它的调度线程上等待分支，而分支通过一个非线程安全的 asio channel 在 strand 上发出完成信号。当等待正在另一个调度线程上开始时，若最后一个分支恰好发出信号，该信号可能丢失，run 永远不会结束；在 Windows 上，立即返回的分支就这样挂起了。现在等待与其他 join 一样在分支发出信号的 strand 上运行，而在登记成员期间就已决定的 JavaScript join 也从该 strand 发出信号。
- **Core：进程中途停止的恢复可以重做。** 进行恢复的宿主会用相同的值和中断 id 重做它正在执行的恢复。当 step 只因一个中断而暂停、且被回答的节点或 `Send` 在进程停止前已完成时，重做会失败：指定该中断会抛出 `std::invalid_argument`（"is not pending"），按默认回答则会把值交给该 step 中第一个尚未运行的 `Send`，而它从未提问。现在这种检查点的中断就是它暂停的那个节点，无论是否已完成；指定已完成 `Send` 的重做也会被接受，已完成的工作会被重放。由恢复模型测试框架发现。
- **Program：被回答的成员已完成时，恢复已继续的 join。** 在逐个运行成员的 JavaScript join 中，进程可能在被回答的成员完成、join 已记录它之后，但在下一个成员的暂停发布之前停止。随后 `reconnect()` 会以 `P_TRANSITION_CONFLICT`（"Recorded join members do not match the paused join"）使 run 失败。现在恢复会重放已记录的成员并继续 join，下一个成员会提出自己的问题。由恢复模型测试框架发现。
- **Program：恢复在后一个 Core 调用中停止的 JavaScript `main()`。** run 保存它最后发布的 Core 调用的检查点。当后一个 `callCore` 被记录为 pending 之后进程在其中停止时，`reconnect()` 会把前一个调用的检查点当作 pending 调用的检查点，并以 `CheckpointIncompatible`（"Exact Core resume checkpoint does not match a pending JavaScript callCore coordinate"）结束 run。现在它像进程在第一个调用中停止时一样，把 run 交回以协调 pending 调用，并且不会再次运行该调用。由恢复模型测试框架发现。
- **节点重试不再与先前尝试的效果冲突 (#364)。** 过去一个调用作用域覆盖了整个重试循环，所以每次尝试都复用第一次尝试的效果任务 id 与调用序号：在 `ToolEffectBroker` 下，重试会重放第一次尝试的回执，或因参数改变而被拒绝；重试期间发生崩溃后，恢复也会落在第一次尝试的槽位上。
- **未确认的 Program Provider 分发是结果不确定的效果 (#364)。** 当 SQLite Provider 日志提交分发标记之后传输层抛出异常时，原始异常被直接重新抛出，节点重试可能再次发送请求。现在日志在这种情况下，以及稍后再次调用尚未解决的槽位时，抛出 `ProgramProviderDispatchUncertainError`（一个不带结果的 `ProviderOutcomeError`）；它既不会被重试也不会被路由。
- **Program：`LocalProgramHost.resume(run_id, value)` 无需 pending id 即可使用 (#359)。** 只有一个等待中的调用时恢复该调用；没有或有多个时抛出指向 `result.interrupt.pending_input.call_id` 的 `ValueError`。显式 id 仍保持精确匹配的约定。
- **Program：失败的运行会保留已完成节点的 trace (#359)。** `execution_trace` 列出失败前已完成的节点，`failure.core_node` 指出抛出异常的节点。失败结果的 identity 随之变化。
- **Program：编译期 JavaScript 遵守运行预算的 wall time (#360)。** 不终止的 `define()` 会在编译器上限与预算 `wall_time_ms` 中较小者处以 `P_JS_TIMEOUT` 停止，而不是 60 秒之后。Python 通过 `ProgramCompiler` 和 `LocalProgramHost` 的 `javascript_limits=` 传入 `ng.ProgramJavaScriptCompileLimits` 来设置编译上限。
- **Program：编写期诊断指向源码位置 (#361)。** JavaScript 构建器诊断在编译返回的诊断中（`ProgramBundle::compile_diagnostics()`，Python `bundle.compile_diagnostics`）携带调用位置（pointer、行、列）。封存进 bundle 的诊断保持不变，因此 bundle id 保持稳定。编译失败会抛出 `ng.ProgramCompileError`（`RuntimeError` 子类），其消息以 `source:line:col` 结尾，`.diagnostics` 携带详细信息。注册表缺失提示会给出 Python 的注册调用名。
- **Python：`ProgramRegistryBuilder` 以同一种错误类型拒绝未注册的可执行单元 (#370)。** `add_registered_node` 现在会像 `add_registered_reducer` 和 `add_registered_condition` 一样检查节点类型是否已注册，而不是把缺失留到准入(admission)阶段。三个方法都会抛出 `ng.ProgramRegistryError`：其 `kind` 为 `"node"`、`"reducer"` 或 `"condition"`，`name` 为缺失的名称，消息会指出 Python 注册调用。它同时继承 `ValueError` 和 `RuntimeError`；此前 reducer 和条件方法抛出的是 `RuntimeError`。使用注册表快照中不存在的名称的 Program 仍会以带有 `P_CORE_GC_REGISTRY_MISS` 的 `ng.ProgramCompileError` 编译失败。
- **停在再也无法触发的 barrier 上的运行不再是 `Completed` (#357)。** 当没有就绪节点、最后一个超步没有显式到达 `__end__`，且某个 join 仍持有 signal 时（例如 barrier 成员走了绕过 join 的 conditional route），`RunResult::status()` 返回新的 `RunStatus::BarrierStalled`（Python 为 `ng.RunStatus.BarrierStalled` 和 `RunResult.barrier_stalled`），`output["_neograph"]["barrier_stalled"]` 以 `{node, wait_for, received, missing}` 列出每个停滞的 join。Program 运行以 `P_CORE_BARRIER_STALLED` 结束为 `Failed`，`failure.core_node` 设为该 join，witness 列出停滞的 join。只有最后一个超步中通往 `__end__` 的显式 route 才会抑制停滞：通往 `__end__` 的 static edge（由 `.exit()` 写入）、通往 `__end__` 的 conditional route 或 `Command.goto("__end__")`。隐式结束（例如没有后继的节点或 Send worker）永远不会抑制，更早超步中的显式结束也不会。运行保存的每个检查点都会记录停滞，因此从中恢复会再次报告。`interruptAfter` 检查点会保存受 barrier 约束的下一批节点，因此恢复时不再在最后一个成员到达前运行 join。Core 耗尽 step 预算的 JavaScript `callCore` 现在以 `P_CORE_STEP_BUDGET` 将运行结束为 `BudgetExhausted`；此前它会作为已完成返回 `main()`。**行为变更：** 在这些情况下原本以 `Completed` 结束的运行，现在以 `BarrierStalled` 结束，在 Program 中则以 `Failed` 结束。
- **保留节点名和没有入口的图不再能编译 (#358)。** 名为 `__start__` 或 `__end__` 的节点会被同名的路由 sentinel 遮蔽：指向 `__end__` 的 edge 会结束运行，JavaScript 构建器会封存一个 `__end__` → `__end__` 自环。有节点但没有从 `__start__` 出发的 edge 的图什么都不运行，却报告 `Completed`。图解析器现在在所有 schema 版本中报告 `GC_RESERVED_NODE` 和 `GC_NO_ENTRY`（在 Program 中为 `P_CORE_GC_RESERVED_NODE` 和 `P_CORE_GC_NO_ENTRY`）；没有节点的图仍被接受。JavaScript 构建器在 `.node()` 调用处以 `P_JS_GRAPH_ARGUMENT` 拒绝保留名称，`define()` 以 `P_JS_GRAPH_ENTRY` 拒绝没有 `.entry()` 的图。evolution 的 `remove_edge` mutation 会保留图唯一的入口 edge。**行为变更：** 这些图此前可以编译；现在 `GraphEngine::compile` 和 Program 编译会拒绝它们。
- **Program：C++ 节点主体和 Core 子图不再占用 Program scheduler (#367, #368)。** 针对 Python 节点函数体的修复让其他所有节点主体仍留在 Program scheduler 线程上：在默认的 `scheduler_threads=1` 下，一个阻塞其线程 800 ms 的 C++ 节点使两个这样的成员的 `ng.all` 耗时 1.6 秒，使 `ng.race` 返回它而不是更快的成员，节点阻塞的 Core 子图也在下一层造成同样的问题。现在 Program 运行的每个节点主体都在进程范围的工作线程池上运行（`NEOGRAPH_NODE_ATTEMPT_THREADS` 可设为 1 到 1024，默认 32），每次尝试都是一个带有自己取消令牌的分派单元，因此 scheduler 只负责编排。取消仍是协作式的：被取消的运行仍会等待主体停止。普通的 `GraphEngine` 运行仍在调用线程上运行不带超时的主体。
- **Program：Python 节点函数体不再占用 Program scheduler (#367, #368)。** Python 节点函数体在整个执行期间都运行在 Program scheduler 线程上，因此 join 成员只能依次运行：两个 800 ms 成员的 `ng.all` 耗时 1.6 秒，`ng.race` 返回先声明的慢成员，`ng.quorum` 会等待慢成员并将其计入，而在默认 `scheduler_threads=1` 下 `ng.await` 的 deadline 无法触发。在 Program 运行中，Python 节点函数体现在运行在 binding 拥有的工作线程池上（32 个线程；`NEOGRAPH_PYTHON_NODE_THREADS` 可设为 1 到 1024），因此成员可以并发推进，计时器也能触发；Python 代码仍由 GIL 串行化。普通 `GraphEngine` 运行仍在调用线程上执行 Python 函数体。`ng.race` 和 fail-fast join 以最先完成的成员而非最先声明的成员定局，`ng.quorum` 恰好保留达成 quorum 的那些成功结果。`ng.await` 会记录其 deadline：在其之后到达的完成即使计时器尚未触发也算超时，且超时优先于被取消子任务抛出的错误。取消仍是协作式的，因此 await 会在子任务停止后返回。**行为变更：** 迟到的完成现在会以 `P_AWAIT_TIMEOUT` 将运行结束为 `TimedOut` 而不是 `Completed`，且 `ng.race` 可能返回不同的成员。
- **Program：没有 provider 的模型调用节点不再中止进程 (#366)。** 未配置 provider 而构建的 `llm_call` 或 `intent_classifier` 节点会在首次运行时解引用它并中止进程（SIGABRT），无论是在普通 `GraphEngine` 中，还是在通过 `ProgramRegistryBuilder.add_registered_node` 注册该节点的 Program 中都是如此。现在这两种节点都会抛出指明节点的 `std::invalid_argument`，executor 不会重试该异常，因此运行以 `NodeExecutionError` 失败；`GraphEngine` 仍会为只关心结构的工具编译此类图。Program 准入(admission)以 `P_ADMIT_PROVIDER` 拒绝它们，Python 将其作为 `ng.ProgramAdmissionError` 抛出。自 #372 起，Python 通过 `LocalProgramHost(node_context=...)` 绑定 provider。
- **Python：Program 准入(admission)错误与运行时诊断错误现已类型化 (#369)。** 准入失败此前只抛出一个裸的 `RuntimeError("Program admission failed with 1 diagnostic(s)")`，`P_START_BUDGET` 等运行时诊断错误会丢失其代码和 witness。现在所有 binding 入口都会抛出 `ng.ProgramAdmissionError` 和 `ng.ProgramDiagnosticError`；两者都与 `ng.ProgramCompileError` 一样是 `RuntimeError` 的子类。消息中每条诊断占一行，包含其代码、JSON pointer 和消息，有源码位置时还附带 `source:line:col`；`.diagnostics` 保留代码、severity、phase、消息、源码位置和 witness。预算错误会指出资源及其限制，例如 `Program invocation budget wall_time_ms=9000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`；每条结构性 start-budget 规则、准入的 policy-ceiling 与 structural-floor 检查以及 Catalog 的 `worker_count` 检查也是如此。
- **Program：面向 Python 宿主的消息与 trace (#370)。** 恢复 `LocalProgramHost` 未持有的运行会抛出带有 `P_RUN_NOT_FOUND` 的 `ng.ProgramDiagnosticError`，消息会说明运行保存在启动它的宿主的内存中。wall-time 超时以 `Program wall-time budget (N ms) expired` 失败；被其中断的操作的文本移到 `failure.witness["cause_message"]`。`TimedOut`、`Cancelled` 和诊断失败会与节点失败已有的行为一样，保留已完成节点的 `execution_trace`；这些结果的 identity 随之变化。对于 JavaScript 源码，编译返回的 E9 fan-in 提示会给出 `graph.barrier("<node>", [...])`；封存的副本保留 Core 的措辞，因此 bundle id 不变。**行为变更：** 超时的 `failure.message` 是新文本；请检查 `status` 或 `failure.code`，而不是消息。
- **Program：第二次被 interrupt 的 `callCore` 不再终止进程 (#371)。** 在恢复之后或首次 `run()` 期间，第二个 `callCore` 被 interrupt 的 JavaScript `main()` 会以 `std::terminate` 结束。被 interrupt 的预留的释放是对照 attempt 的累计用量检查的，而累计用量还包含先前 command 已结算的工作，因此 store 拒绝了 publication，fallback 构造了无效结果。in-memory、SQLite 和 PostgreSQL transition store 现在用 `is_valid_program_journal_reservation_release` 检查该释放：每个带用量的资源退回的量可以是从预留减去 attempt 用量到整个预留之间的任意值。现在每个被 interrupt 的 `callCore` 都返回一个可由 `resume()` 继续的 `Interrupted` 结果。无法恢复的结果不再携带 interrupt，逃出 terminalization 的异常会以 `P_RUNTIME_INTERNAL` 将运行结束为 `Failed`，而不是终止宿主。
- **严格上下文组装接受带多个 required artifact 的短 history (#373)。** mandatory token 数累加每个 required artifact 的消息 projection JSON，而输入 token 数测量的是更小的已准备 wire body。history 较短且有两个或更多 required artifact 时，mandatory 数超过输入数，`StrictRuntimeProfile.invoke`（以及 `RuntimeTurnAssembler::assemble`）以 `Mandatory token count exceeds estimated input tokens` 失败。现在两个数都按每 3 字节 1 个 token 计算已准备请求所携带的字节：required artifact 计其文本，role 和 JSON 框架只计入输入数。超过 `max_input_tokens` 的 required 内容仍会抛出 `ContextBudgetBlocked`，mandatory 数超过输入数的 receipt 会被拒绝并给出两个数，例如 `Mandatory input tokens (41) exceed the estimated input tokens (40) they are part of`。新的 assembly receipt 记录更小的 `mandatory_input_tokens`，因此其 identity 与之前构建的 receipt 不同；已存储的 receipt 仍可 parse。
- **Program：`main()` 的每个结果列出它运行的 Core 节点 (#374)。** 对会 interrupt 的 Core 调用两次 `callCore` 时，每个结果的 `execution_trace` 都晚了一个 attempt：`["draft"]`、`["draft"]`、`["deploy", "deploy"]`。被 interrupt 的 command 只带着自己的 trace 结束 attempt，因此丢掉了同一 attempt 中先完成的 command；而 resume 又把先前 attempt 已完成的每个 command 的已记录 trace 再加一遍。现在每个 `Interrupted` 或 `Completed` 结果按顺序准确列出自上一个结果以来运行的 Core 节点：`["draft"]`、`["deploy", "draft"]`、`["deploy"]`。由后续 command 以其他 status 结束的结果，也会保留该 attempt 中先运行的 command 的节点。失败的结果也像只用 `define()` 的 run 一样，列出 Core 失败前已完成的节点；此前 Core 失败的 `callCore` 的 `execution_trace` 为空。受影响结果的 identity 随之变化。
- **适用于所有 schema 版本的拓扑错误不再建议删除 `schema_version` (#375)。** `GraphEngine::compile` 会在每个错误列表末尾附上 `Remove 'schema_version' to fall back to lenient parsing`，没有 `schema_version` 的文档也是如此。对于 lenient 解析不会放宽的 `GC_NO_ENTRY` 和 `GC_RESERVED_NODE`，摘要现在改为说明要修改什么：添加一条从 `"__start__"` 出发的 edge，或重命名该节点。只有列表中有未知键（`GC_UNKNOWN_FIELD`）或节点 config schema 错误（`GC_NODE_CONFIG`）时，才会出现 lenient 提示和 `strict` 前缀。诊断代码、pointer 和消息保持不变。
- **按节点的引擎设置拒绝未知节点名 (#376)。** `set_node_retry_policy`、两个 `set_node_cache_enabled` 重载，以及 `EngineConfig` 的 `node_retry_policies`、`cached_nodes` 和 `node_cache_policies` 映射，会接受图中不存在的节点名，而该设置从不生效。现在它们抛出 `std::invalid_argument`（Python 中为 `ValueError`），并指明设置、图、未知节点以及图中的节点，例如 `set_node_retry_policy: graph 'demo' has no node 'fecth' (nodes: 'fetch', 'parse')`；`build()` 和 `link()` 会在构造引擎之前检查 `EngineConfig` 映射。**行为变化：** 为不存在或拼错的节点设置策略的代码，过去会被静默忽略，现在会失败。
- **重试 backoff 不再溢出 (#364)。** 下一次重试延迟是 `delay_ms * backoff_multiplier` 先转换为 `int`，再限制到 `max_delay_ms`。超出 `int` 范围的乘积属于未定义行为，在 x86 上会变为 `INT_MIN`，导致之后的每次重试都不等待就执行；加入 jitter 的延迟也有同样的转换。现在两者都以 `double` 计算：NaN 和大于 `max_delay_ms` 的值限制为最大值，负值限制为 0，转换放在最后。
- **Program：为零的预算上限会指出相应字段 (#370)。** `PolicySnapshot` 和 Python `LocalProgramHost` 会以 `Every policy budget ceiling except child budgets must be positive; zero: model_tokens, max_dynamic_compiles` 的形式拒绝含零字段的上限，并列出每个为零的字段。`max_child_depth` 和 `max_total_children` 仍可同时为零。
- **Program：Python 与 QuickJS 文档与运行时一致 (#370)。** Python Program 快速入门重新可以运行：旧示例用绑定不接受的默认值参数调用了 `GraphState.get`。QuickJS 的 `main()` 示例导出 `define()` 并且只调用该 Core，并说明失败的 `callCore` 会结束运行，而不会进入 `try`/`catch`。Python 绑定指南现在涵盖 `callCore` 结果的形状、按排序存储的 barrier `wait_for`、在 `LocalProgramHost` 下以 `P_CHILD_BINDING` 失败的 `ng.spawn`、`compile_admit` 固定的预算、所有字段都必须为正的上限、图检查与 barrier 停滞、类型化的 Program 错误、与封存的 `diagnostics` 并列的 `compile_diagnostics`、以 `P_ADMIT_PROVIDER` 准入(admission)失败的 `llm_call` 节点、`scheduler_threads`、`worker_count` 与 `NEOGRAPH_PYTHON_NODE_THREADS`、超时、只存在于内存中的运行、`resume()` 的默认行为，以及恢复后的 `execution_trace`；故障排除指南列出了对应的错误，参考与概念指南列出了 `RunStatus::BarrierStalled`。README 指向该 Program 示例，而不是不使用 Program API 的 Python 示例。
- **示例可端到端运行。** 所有示例都已用真实 provider 或离线模式运行，并修复了失败：多智能体辩论和自我进化聊天机器人以可移植文本传递轮次，而不是把原生状态重放到配置不同的调用中；the-beast 各变体为模型编写的 `llm_call` 节点指定模型，使用共享的 zero-data-retention provider 工厂并报告运行失败；A2A 客户端默认使用示例 38 的端口；Harness MCP 服务器可在无 token 的 loopback HTTP 上工作；示例 34 为生成图片提高流限制，并在该部分使用支持图片输入的模型；program chatbot 演示策略与 descriptor 策略 revision 4 一致；多租户 live 服务器接受 `NG_MULTI_TENANT_REQUESTS` 和 `NG_MULTI_TENANT_WORKERS`；Crawl4AI 客户端接受带路径的 base URL 并发送 bearer token；RE 智能体在需要密钥之前先检查 GhidraMCP 后端。
- **并行工具 dispatch 的 fallback 捕获的工具失败现在是合法 JSON。** 在多调用 batch 中，若某个调用在逐调用错误处理之外失败，其工具消息以字符串拼接构造为 `{"error": "..."}`，异常文本中的引号、反斜杠或换行会产生非法 JSON。现在改用 JSON 库序列化。
- **所有工具路径上的工具失败遵循同一契约。** 在多调用 batch 中，`HookBoundaryBlocked` 会变成模型可以读取并绕过的 `{"error": ...}` 工具消息，而单个调用则会停止运行；现在两者都会停止运行。工具代码在 C++ 或 Python 中抛出的 `HookBoundaryBlocked` 或 `NodeInterrupt`，此前在所有路径上都被 `ToolExecutionController` 转换为失败的工具结果；现在它会停止或暂停运行，因此工具可以请求人工确认。无法运行的强制工具 Hook（例如 `llm::Agent` 运行没有 owner scope 时）现在会阻断运行，而不是让每个调用都以发给模型的消息失败。抛出异常的 `get_name()` 现在无论在单个调用还是 batch 中都成为该调用的 `failed` 结果，此前单个调用会使节点失败；只携带 `error`、并以非错误结果到达模型的 batch fallback 消息，现在与任何失败调用一样具有 `tool_status` 和 `{"error", "status", "retryable", "effect_uncertain"}` 结构。节点重试策略不再重试 `HookBoundaryBlocked`；当 batch 中多个调用都使运行停止时，按取消、Hook 阻断、中断的顺序优先。Python 的 `HookBoundaryBlocked` 新增 `invocation_id` 与 `state`。参见[工具失败契约](docs/reference-en.md#tool-failure-contract)。
- **声明了 `outputSchema` 的 MCP 工具的错误消息得以保留。** 客户端在检查 `isError` 之前就验证 output schema，因此不带 `structuredContent` 的合法错误结果会以 "MCP tool advertised outputSchema but returned no structuredContent" 失败，`[FUNCTION_NOT_FOUND] No function found named 'x'` 之类的工具消息既到不了调用方，也到不了模型。现在按照 MCP 规范的允许和官方 Python SDK 的做法，错误结果跳过 output schema 验证；成功结果仍会验证。
- **Program publication无需node handle移动赋值即可使用libc++构建。** 在所有index publication之后直接初始化提取的handle，不依赖库特定的`noexcept` trait，同时保留commit原子性。
- **Windows interface-6 archive fixture原子地创建私有custody目录。** 禁止继承、仅允许当前用户的DACL满足native archive admission，保留所有outcome、金额证据、replay和错误binding检查。
- **Anthropic Messages 和 Gemini 的并行 client-tool 结果保持在同一 turn。** family 编码前合并相邻 portable 纯结果消息，保留 mixed、native 和 turn 边界。其他 API family 与已保存 history 不变(#311)。
- **History compaction 保留权限和未完成证据。** 原样传递指定 typed controls，仅接受正常结束且非空的摘要；拒绝的 outcome 仍被记录，但不截断 history。摘要是 user 来源内容而非 system 指令，真实 native replay prefix 保持受保护(#314, #325)。
- **Program fan-out join 不再丢失等待前到达的完成通知。** 持久完成状态处理 signal-before-wait；scoped 取消停止新 dispatch，并取消当前或迟接入的子任务。部分 launch 失败会先 drain 所有借用状态的 producer 再传播。Await 在可能同步进入的子 publication 前注册 deadline(#341)。
- **Child generation 和暂停 migration 使用一致的 durable head。** 依据 immutable admitted lineage 校验 generation，record/journal snapshot 不再混合不同 publication。释放暂停 source 不会更新已消耗 wall-time budget(#324)。 Native PostgreSQL head通过一个MVCC statement读取，admission和recovery移动已拥有的head成员而非复制。
- **子任务预约恢复保留已获准的wall-time grant。** 替换后剩余时间减少时，cold reconnect将保留的预约绑定到原始不可变generation。当前可支出预算、token和金额上限以及子任务的原始deadline均不更新。
- **Synthesis persistence fixture使用父run budget，而非与验证目标无关的本地await deadline。** 专门的timeout和取消测试继续验证operation deadline。
- **Blocking Tool 异常 handoff 只有一个 terminal 所有者。** async-to-blocking bridge 保留 typed 异常和 system error code，publication 后 worker 不再访问异常对象(#339)。
- **示例和 benchmark driver 遇到不可用输出时失败。** partial/refusal 输出不能成为成功的 routing fallback，UTF-8 preview 保留完整 code point；失败尝试不计入成功 timing sample(#317)。
- **Durable Program 集成验证检查隔离和恢复。** cold rematerialization 后保持各 node 最小权限；provider journal 和 Tool effect 区分 run、attempt、slot，拒绝改变的权限，并在 reconnect 后保留未决 dispatch 证据(#293, #295, #297)。
- **使用引擎工作线程池的 multi-Send fan-out 中,某个分支的取消处理函数可能在该分支自身的操作完成期间运行。** multi-Send 步骤的分支运行在裸线程池执行器上,而静态 fan-out 的分支早已各有一个 strand,因此来自另一个线程池线程的取消会与分支的协程竞争(`cancellation_signal::emit` 中的数据竞争,已在 ThreadSanitizer 下复现)。现在每个使用线程池的 Send 分支都在自己的 strand 上运行;没有线程池的运行保持不变。参见 #344 和 #345。
- **启用 HTTP 的 `neograph-harness-mcp` 和 `example_harness_mcp_server` 现在会链接 `OpenSSL::Crypto`。** 这两个可执行文件直接调用 OpenSSL（`SHA256`、`CRYPTO_memcmp`），却从未自行链接它；SchemaProvider 0.2.0 不再链接 libcrypto 后，`NEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` 的共享库构建会因 `SHA256` 的未定义引用而链接失败。
- **同步图执行现在等待 scheduler 的 post 调用完全结束后才销毁 private `io_context`。** 完成处理函数即使已运行，线程池中的线程仍可能处于唤醒调用中，使 `pthread_cond_signal` 与 `pthread_cond_destroy` 发生竞态(#346)。同步 bridge 会等待外部线程的 post 与 tracked-work 释放，关闭取消通知的接收，再处理迟到的排队通知，最后销毁 context。调用线程执行、嵌套调用和父级取消权限均保留；post 处理函数抛出异常也不会丢弃仍由 bridge 所有的 coroutine frame。
- **SDK 固定提交前移到 SchemaProvider `8f32af7`，SDK 仍为 0.2.0，interface/shared-library 代际仍为 5。** SDK 现在会在 provisioning 失败的所有路径上清零 archive 激活密钥（SDK #10）；不再把 libcurl 8.21 合成的完成计时当作“请求已发送”的证据（连接被拒绝和不受信任的 TLS 保持 `NotSent`，因此其安全重试可用）；把字面地址写入 libcurl 的 DNS 缓存，使未启用 c-ares 的 libcurl（macOS 上的 Homebrew）不再创建超出配置工作线程数的解析线程；并把 qualification meter 的固定表移出线程栈。SDK 自身的测试现在可在 native Windows 和真实的 macOS 运行器上执行。这是源码固定，不是新的 SDK 发布或版本。
- **引擎在自己的 fan-out 工作线程上释放时，进程不再崩溃；持有 Python GIL 释放引擎时也不再死锁。** 失败的 fan-out 分支会保留它的异常，直到运行返回之后其协程帧在工作线程上展开。如果该异常拥有那个拥有引擎的宿主对象（例如 traceback 持有 `GraphEngine` 的 Python 异常），工作线程就会放下最后一个引用，`~GraphEngine` 便在该线程池自己的线程上销毁引擎的线程池。从线程池自己的线程 join 会跳过该线程，于是线程池在工作线程脚下被释放，而该线程离开处理函数时还会继续使用它：这就是负载下的 Python 测试套件中偶发的 segmentation fault。现在这样的线程池会立即停止，并在工作线程离开后由池外的线程销毁。在 Python 绑定中，放下 `GraphEngine` 最后一个引用的线程，会在析构函数 join 工作线程期间释放 GIL；释放失败分支的 Python 异常的工作线程需要 GIL，因此此前 join 可能永远等下去。

## [0.13.1] - 2026-10-07
NeoGraph `0.13.1` 面向 alpha SDK `0.1.1`。interface revision 与 shared-library generation 仍为 4，因此使用者只需选择 SDK `0.1.1`（已安装的 package config 要求精确版本）并重新构建。Native archive 仍为 v3 / `spna3`，portable JSON 仍为 v2。功能性变更是下面的 Windows transport 修复：作为 0.13.0 发布的 Windows wheel 存在该缺陷，应替换为 0.13.1。修复提交的 hosted CI 与 Windows wheel gate 已通过（GitHub Actions run 37576205702、37576205395，仅 CPython 3.12）；完整的 CPython 3.9–3.13 矩阵在发布 tag 上运行。
### 修复
- **Windows：较大的请求体可能一直卡到操作截止时间。** SDK transport 等待 `FD_WRITE`，而 Winsock 只在 `send()` 以 `WSAEWOULDBLOCK` 失败之后才记录该事件。一次成功但把 socket 填满的发送不会产生事件，因此约 4%～6% 的 4 MB 上传（在 loopback 上测得）停在约 131 KB 或 197 KB，并以重试安全性为 `PossiblyAccepted` 的 `DeadlineExceeded` 结束。固定的 SDK 现在在等待写入期间还会用短定时器探测 `select()`。已发布的 0.13.0 Windows wheel 存在该缺陷。回归测试：`test_large_post_does_not_stall_when_the_send_buffer_fills`（Windows 上重复 150 轮新连接；在 0.13.0 wheel 上 8 次运行全部失败）。Issue #340。

### 变更
- **CI 已分层。** 现在每次 push 和 pull request 只运行 `ci.yml`：Linux、macOS、Windows 的完整 native suite 加 2 个 installed-consumer 行，仅改文档时跳过。sanitizer、fuzz canary、性能 gate、真实 ARM64、gRPC、Visual Studio generator 以及其余 8 个 installed-consumer 行原样移到 `ci-extended.yml`，每晚及按需运行。Wheel 在 packaging 变更时为每个平台构建 CPython 3.12，在发布 tag、每周 canary 和手动运行时构建 CPython 3.9–3.13，每个解释器一个 job。Windows 依赖复用 vcpkg binary archive。`verify_ci.py` 支持 `--jobs auto`、`wheel --python` 和 `native-windows --generator`，`native-windows` 现默认用 Ninja 构建（请在 x64 MSVC 环境运行，或指定 `--generator "Visual Studio 17 2022"`）。所有 runner 仍为标准 GitHub-hosted runner。
- **ctest 以按测试的超时运行。** `verify_ci.py` 传入 `--timeout 600`（ASan 与 MSVC ASan 为 1200），因此卡住的测试会按名称失败，而不会耗尽 job 上限（Issue #341）。

## [0.13.0] - 2026-10-07

NeoGraph `0.13.0` 使用 alpha SDK `0.1.0`，loaded interface revision/shared generation 4。所有 native consumer 和 Python wheel 必须以匹配 header/library 重建。Native archive 保持 v3 / `spna3`，portable JSON 保持 v2。Retained-feature 集成已通过本地 Linux x86_64 native 与 installed wheel 验证。提交 `79c21e1` 的 hosted Wheels 与 CI(GitHub Actions run 37459723655、37459723545)构建了 Linux x86_64、Linux aarch64、macOS arm64 和 Windows AMD64 wheel,并通过了除 hosted 原生 Windows job(`build-windows`,在 ctest 之前超过 30 分钟限制)之外的所有 CI job。发布需经受保护的 release 批准。此前付费观测和 benchmark cohort 保留 SDK3 范围。

SDK4 Linux x86_64 验证运行了全部 27 个注册 case：初次 full run 有 25 个通过，随后两个修正 case 在限定重跑中通过。这不是第二次 full-suite 27/27 运行。未改动 Stop CLI 的 buffered/SSE valid/invalid client-call 四个 case，以及准确 installed README consumer 的三个 variant（reported zero、unknown count、HTTP400 failure）通过。这些本地证明不新增 vendor 或 Windows/macOS/ARM64 验证。

NeoGraph SDK4 验证范围为初次与修正后限定运行合计的 Core 1261 case、Program 691 pass 与设计上的 Memory process-loss skip 14 case，以及 installed Python 初次 499 pass 加修正后限定 5 pass，共 504 case。不声称第二次 full-suite 运行。实际运行了本地 A2A 0.x/1.0 peer、evolution file mode、plan replay、修改的 C++ helper 和 11 个 Python application。

### 变更
- **Windows provider 构造与 byte-budget Bridge。** SDK 门控 `NativeSocketRuntime`(原名 `PosixRuntime`)现在也授予 Windows,其 transport 已有 Winsock 事件路径。已发布的 `max_host_connections` 准入上限为 `INT64_MAX`,大于 Windows 的 32 位 `long`,Windows 运行中被拒绝;现为 2147483647,并由回归测试固定。NeoGraph 的 SDK Bridge 现在在构造 pmr 容器之前接收 byte budget:MSVC 的 `unordered_map` 在构造函数中分配内存,之前的 0 budget 因此在此处抛出 `bad_alloc`。原生 MSVC Release 中 1915 个已注册用例全部通过(2 个设计内 skip),变更前 1916 个中有 209 个失败。仅测试的改动:3 处 `exception_ptr` 同一性断言使用平台感知的辅助函数,因为 MSVC 的 `current_exception()` 返回副本;1 个 trusted-archive fixture 测试只设置 POSIX 权限位,因此与同类测试一样在 Windows 上跳过;Images/Veo 的 HTTPS peer 同时监听 `::1`,因为 `localhost` 先解析到它,而 Windows 拒绝 loopback 连接需要约 2 秒。1915 个用例的执行在开发机上完成,因为 hosted 原生 Windows CI job 在 ctest 之前就超时;hosted Windows wheel job 为 CPython 3.9-3.13 完成了构建、repair 和 installed-wheel 测试。SDK 自身仅限 POSIX 的测试不会在 Windows 上运行。
- **Windows wheel 在 hosted CI 上构建并测试。** 首次 hosted Windows wheel 运行需要四处修复:wheel job 现在自行准备与清单 `builtin-baseline` 对应的 vcpkg 检出(runner 镜像的检出比它旧);CMake define 通过 `SKBUILD_CMAKE_DEFINE` 传递,因为带引号的 `CMAKE_ARGS` 值在 cibuildwheel 下被拆成无用变量,CMake 只在 `<vcpkg root>/installed` 中查找(工具链仅在 manifest 模式下遵循 `VCPKG_INSTALLED_DIR`);`verify_ci.py` 为 TLS 测试 fixture 指定 vcpkg 的 `openssl.cnf`;一个归档保管的 Python 测试在 Windows 上跳过(`mkdir(mode=0o700)` 不会创建仅限所有者的 DACL)。wheel 包含 libcurl 8.22.0、OpenSSL 3.6.5、libpq 18.4、nghttp2 1.70.0、SQLite 3.53.4、zlib 1.3.2 以及 Microsoft Visual C++ 运行时 DLL `msvcp140`、`msvcp140_1`、`msvcp140_atomic_wait`;这三个 DLL 不参与 delvewheel 的名称改写,以按 Microsoft 的 Distributable Code 条款保持未修改并保留有效的 Microsoft 签名。`THIRD_PARTY_LICENSES.md` 记录了从产物观测到的 macOS、Windows 和 Linux 构成以及仍缺少的内容(新增组件的逐版本声明全文)。hosted run 37459723655 为四个平台生成了 CPython 3.9-3.13 wheel,已发布 Windows 产物的签名检查属于发布检查。
- **Local/hosted 共用验证入口。** `python scripts/verify_ci.py <profile> --work-dir <fresh-output> --jobs N` 集中十四个既有 CI gate，不移除 native platform、sanitizer、installed consumer、wheel、archive 或 release gate。Installed consumer 不再依赖 Git/Bash；独立的 immutable QuickJS performance gate 仍需 Linux/Git provenance。Native Linux 在 native PostgreSQL test 后串行运行完整 Python/protocol suite。Benchmark checker 保留原四个 workload 和数值边界，拒绝歧义或无效 metric。Source review 与 CLI help 不构成 runtime/platform/performance qualification，仍需集成执行。
- **单一 QuickJS matrix 命令。** 将重复 shell build driver 替换为 `python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4`，保留 `<fresh-build-root>/quickjs-performance.json`。自有新输出已存在时会被拒绝。不修改 immutable performance runner、preregistration 和源码根目录 Git provenance 要求。Native Windows toolchain 转发与 work-dir 专用 wheel vcpkg root 保留声明的 bootstrap/repair gate；PostgreSQL 前提检查向 `psql` 传递显式 DB URL。这些是源码变更，不宣称新 profile 已通过。
- **已完成取消 context 的寿命。** Detach 在 context 销毁前释放已完成的 Asio slot handler；generation 检查丢弃旧的排队 emit，callback 可在不重入 token lock 的情况下 rebind。取消状态不重置。必须一起重建 NeoGraph 与所有 native consumer。本地取消/fork 23 个 case 和实际 plain/ASan retained-token consumer 已通过。[Commit `24d3166` CI](https://github.com/fox1245/NeoGraph/actions/runs/37275542831/job/111651716661) 的 1,333-case TSan 运行也通过，包含 retained-parent-token teardown 回归及三个 skip。证明限于既有 dependency suppression 和禁用 ASLR 的环境，不宣称普遍 race-freedom。Detach 前必须完成 operation，不能在正在执行的 signal callback 内 detach。
- **Platform wheel 依赖与 cold loading。** 将 pybind11 固定为 `2.13.6`，解决 SDK 直接 OpenSSL Crypto 依赖，并为 macOS 14 使用匹配的 LLVM 20 header/runtime。保留 CPython 3.9–3.13、所有 platform 行和 installed wheel full suite；随后运行 Linux replacement 与拒绝 Homebrew 访问的 macOS cold-loader 检查。准确的最终 artifact 仍是 release gate，不以 source-only 检查宣称 portability。
- **匹配的 Linux LGPL library source。** SDist 的 `deps/redistribution/` 包含 library-only keyutils/libxcrypt source、downstream patch 和 build/replacement material。保留 file-scoped grant 与接收者的修改权；grant 未确认的 upstream test 不属于 library input，因此排除。仅提供 source/notice 不能证明最终 wheel 的 replacement arrangement 或已完成法律审查。
- **修复 Python cookbook import。** Native OpenRouter 示例从 installed package 的 `neograph_engine.llm` 导入 `SchemaProvider`，不再使用已删除的顶层 export。
- **显式 SDist 允许列表。** 使用 scikit-build-core 1.0 以上、`explicit` inclusion mode 和 root 锚定文件 pattern。实际 1131-file SDist 已通过允许列表、private/cache 排除、SDK4 pin 与必需内容检查；`manual` mode 曾包含无关文件。Wheel command 使用解包后的 package directory，并将包含空格或 Windows backslash 的 test path 作为独立参数传入。
- **保留的 typed controls 和模型规则 (#309)。** 将 newer-master `c708e51a`、`165dbbbd`、`5553d9ea` 集成到各 family controls：Chat reasoning/include/usage/替代模型，Responses provider-held cursor/parallel tools/verbosity/truncation/include，Messages manual/adaptive/disabled thinking/effort/cache/tool choice/routing，以及 Gemini thinking level/safety/tool choice/temperature。错误 family/origin/value 在 I/O 前拒绝。准入 model prefix 规则不区分大小写，包含 gateway suffix，并拒绝禁止的显式 temperature；Messages thinking 省略 temperature。不恢复 JSON knob interpreter。
- **显式 history 和 deployment headers (#305/#306, #310)。** 集成 `9e161fc8`、`c5035e00`、`02b4c275`。真实 same-route native reasoning 保持所有权。Gemini `PortableForeign` 接受 unsigned assistant text/client-tool history，仅首个 foreign function call 使用文档化 bypass，失败 native seal 从不降级。Responses `previous_response_id` 使用新 input，真实 tool ownership 需要时接收非 wire 的 `previous_response_history`；provider-held state 不是 full native/archive replay 权限。Deployment environment header 在 admission 前显式 host preprocessing，支持 optional 省略和不区分大小写的 override 优先级。
- **保留的 outcome、usage 和 retry 语义 (#307/#308/#312)。** 新 failure/terminal 修复（`782e23ea`、`0eb271de`、`f89afe08`、`99fa81a2`）保留为 typed 所有权 failure/partial evidence，包括 HTTP-200 error。`ec69b845` 的 cache/reasoning 意图使用 nullable 完整报告，不把缺失计数变为零。`054a2c1c`/`b67ccf4c` 通过 SDK no-output-replay safety 和 bounded exponential full-jitter policy 保留，不恢复旧 retry wrapper。`51374ce8` 的 client-tool intent 在 valid/invalid call 的 Stop event 与最终 outcome 中一致；具体 stop reason 与 server 执行 tool 保持区分。
- **保留的 A2A wire 和 HTTP streaming 修复。** 集成 `08c4df70`、`cd21e3e3`。Card 选择的 0.x/1.x dialect 同时改变 method/body/version header；SSE task/status/artifact 组装保留 output，已观察 event 后不 dialect retry。Server encoding 由 version header 决定。答案优先级为 terminal/interrupted agent status、artifact、agent history。Fixed-length stream reply 保留 bounded body/status，拒绝 early EOF/buffered surplus。保留 bound discovery/restart 行为。本地 native/Python peer 验证通过，macOS 验证尚未完成。
- **保留的 research、fork 和 example 修复。** 将 `861619a4`、`03e78f6d`、`5553d9ea`、`9b4b2776` 集成到 typed 路径。Research 仅对已完成且无 tool call 的 empty `MaxTokens` 额外调用最多两次，在原 bank/deadline 下将 cap 倍增至 16384，保留独立 call identity 与每次 outcome/usage report。可见 partial report 为 `Incomplete` 而不重试；empty final report 失败。Exception/已交付 stream part 不授权 redispatch。Fork 保留 pending continuation；example 08 保持 new-turn 流程。Evolution 在两种 mode 注册 `pnoop`，plan replay 节省四次调用。Research、file mode 与修改 helper 的限定检查已在本地通过。
- **Breaking 类型化 Python provider API。** Python 使用 `make_provider_request`、`Provider.prepare`、单次使用的 `Provider.dispatch` 和拥有所有权的 `ProviderOutcome` completion/failure view。`SchemaProvider` 接受 `ValidatedDescriptor`、`ProviderRuntimeOptions` 与 `SchemaProviderDefaults`；类型化 `ProviderMessage` 独立于 graph 便捷类型 `ChatMessage`。缺失 usage 保持 `None`。旧 completion API 与 `OpenAIProvider`/`RateLimitedProvider` 已删除，无 shim。Installed Linux wheel 与实际示例执行在上述限定范围通过，不是所有 platform/hosted vendor 的新验证。
- **事件驱动 provider handoff；C++ 必须重建。** 将SDK Bridge的1ms完成/取消polling替换为capacity-one coalesced Asio通知。Active drain无需额外wake提交便重新观察publication hint，context shutdown在channel teardown前分离non-owning publisher endpoint。`CancelToken::stop_token()`与`is_cancelled()`共用`std::stop_source`状态，保留fork/Asio-slot取消，unbound/shared token也不占用调用者slot。所有C++ consumer必须以匹配header/library重建，不能仅替换pre-v1 shared library。Local TLS GraphEngine A/B：48 process record、4800 measured graph run、失败0；256B padding p50为1.302474→0.952992ms，吞吐738.90→1022.96 graph run/s。Tool/SSE吞吐下降2.54%/6.69%，并非普遍speedup。43项targeted regression、200轮ASan/UBSan publisher-teardown、300次installed-SDK consumer通过；TSan无法在此host执行。未改变Parser/ISA flag及paid/Python qualification。参见[实测结果](benchmarks/provider-notification-summary.json)。
- **E2E发现的 protocol/demo 修正。** `neograph_get` 不再忽略 URI/cursor page，而是选择 retained 结果；stdio/HTTP lifecycle、URI及SQLite重启验证通过。空 A2A discovery URL 跟随真实 bind endpoint 和重启，保留显式 public URL；uppercase示例广告已验证 SSE capability。MCP时间demo使用所请求的 IANA timezone。ProgramChat mock显式选择提供的 model-fact policy，不绕过 bounded admission。新验证覆盖39个编号target、9个offline Beast模式、Assembly A2A、Jarvis持久化、专用mock topology load、retrieval admission和ProgramChat SQLite/PostgreSQL/browser；measurement-only/live/voice/GUI/延期Python边界单独保留。
- **文档/注释一致性。** 已归档 Stage 3 提案明确标记已删除的 Provider completion API 已被取代，并链接当前类型化 prepare/dispatch 指南。Cookbook 状态区分已实际运行的离线 Assembly/Jarvis/Beast 及 ProgramChat PostgreSQL/browser 范围与历史耗时、未验证 live/voice/Python 变体和独立 multitenant load recipe。当前 cancellation/HTTP/isolation 注释不再描述已删除的 provider API。Runtime 行为和 qualification 边界均未改变。
- **Breaking C++ typed provider/custody 契约。** 所有 consumer/custom provider 必须以匹配的新 header/library 重建。即使 `NEOGRAPH_BUILD_LLM=OFF`，Core 也要求外部 `SchemaProvider::runtime`。Provider 使用拥有所有权的 `ProviderRequest` → move-only `PreparedProviderRequest` → 不可变 `sp::runtime::Result`、显式 Collect/Stream 和声明的 family control。Completion 兼容 type、JSON descriptor interpreter、primitive registry、Responses WebSocket 已删除，无 alias/shim。SDK `0.1.0` 是 alpha（interface revision/shared generation 4），不是稳定 interface。下方此前 C++ runtime/archive 验证是 SDK3 Linux/POSIX 记录，没有执行 Python wrapper 或 retained SDK4 变更。

  Fresh installed find_package Program C++/C ABI/dualQuickJS consumer 与 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer pass。Interface/ABI 声明本身不同于实际 package 结果；不声称更广 platform 或稳定 release。
  Native custody 为 closed v3 / `spna3`（拒绝 v2），绑定全部 semantic descriptor field 和精确 owner/custody。诊断 duplicate key 保留原始 raw evidence，但不能成为可执行 config；non-2xx JSON/named SSE error 保留为事实 SDK 证据。Host-positive `observer_limits` 只能降低上限，digest v3 绑定实际上限。PMR ownership 包括 queued/draining container，cancellation 在 queue mutex 外进行。Generic messages 保持 Generic，mapped native history 保留 C++ sidecar。
  共同 `ProviderOutcomeError`、`ProviderFailure`、`ProgramFailure` 保留全部 SDK 结果及原始/次要 live cause，不发生 outer Node/Program 重新 dispatch。Native failure witness 需要真实 archive custody/Runtime hydration；预填 data-only witness 不能绕过 admission，nonpersistable 失败不能 publish/replay，重启后的原始 exception pointer 为 null。
  `RecordedBindingSet` 是 trusted Catalog 从 source event capture 并认证的 source-bound data。`replay_recorded` 检查原始 selected-source permission 后以 durable CAS 转移实际 remaining credit；inherited spend 不是新 grant。续期型 `start_recorded` API 已删除；精确 owned Program lease 不因 expiry 续期，captured JavaScript command 不能重复 external effect。持有 Archive 不授予 spending 权限。分别批准的最小 Images/Veo/Decisions 三次调用成功不是广泛 qualification 或 invoice 证据.实际集成验证与剩余限制见下文，此前 M5 测量为历史。

  **Recorded-control causal fix 已在 full suite 实证。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Reservation 选择认证 settlement transition，而非曾拒绝首个新 Core checkpoint 的普通 Running→Running transition。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化；既有 Recorded CPU/Memory await/handoff scenario 在 full suite pass；remote TSan coverage 限制如下明确保留。

  **付费观测已完成；不是普遍 qualification。** 原始 `SPQUAL1` base630/1000000 microUSD 不变；同一原始 ledger 中 ONE hash-chained `A` 接纳批准的 extension480/3000000，aggregate1110/4000000。Calls/spent/hold/settlement 累积，不产生新 grant ID/header/reset。精确 declaration byte/file identity 和 original authorization/baseline/catalog/activation/ledger-prefix hash/totals 仍固定；删除、替换、变更均 fail closed。最终 canonical ledger 为 calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 是 LOCAL catalogue meter，不是 invoice。记录的 five-family60-pair baseline 完成600 request：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4次）、Interactions57/60（incorrect-vision buffered1次/SSE2次）；合计293/300 pair，不是300/300。其他 old600 financial record 保留，但不是完整 behavioral proof。此前 M5/media one-shot cohort 不变。此前 Google3-round prerequisite 保留 invalid-tool2次/unreadable-positive1次失败状态。最终 SDK 证据与 native-axis 限制不同于 baseline 成功。 此前 activation/reopen smoke 保留为两次 reopen 后 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass；这是限定的历史 checkpoint，不是最终 ledger totals。此前验证的 Chat60-pair cohort 保留实际 attempt120、UpperBound charge120、无 UnknownHold。

  **Native-axis 观测不是 cryptographic 验证或 native consumption/equivalence。** Generate 接纳 mutation/omission/duplication。Interactions 接纳 isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication。删除全部 thought/signature 返回 generic400；保留 THOUGHT item 而删除全部 signature field 也返回 generic400。最后一次 capture 只有 local encoded-original retention control，没有 same-capture server positive；此前 positive cohort 仍是真实证据。这仅建立 aggregate-carrier-absence boundary，不证明 issuer/signature 验证或 vendor consumption。实际 report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`；prerequisite-failed/not-run/negative-inconclusive 状态保持为事实。 Thought-only/carrier-only omission 在仍有其他 carrier 时被接纳；这不加强 issuer-validation/native-consumption 声明。

  **实际集成证明及剩余限制。** 最新 Core full run：2242 test、失败0、skip16（RAM process-loss 不适用14项/live-credential gate2项）、130.17秒。`PgNestedJsonRoundTrips` 精确保留 duplicate key/order/null metadata、blob、residual，0.18秒 pass。未修改的原始 shared-bank fork 和既有 Recorded CPU/Memory await/handoff scenario 均 pass。真实 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe 在 plain 和 ASan+UBSan pass。LOCAL Memory/SQLite/PostgreSQL TSan scope7项 pass、warning0。包含 system Abseil/Protobuf 的 full mixed gRPC TSan 为 exit66，dependency/generated-RPC stack 有 race warning402项。这是 instrumentation/coverage 限制，不是已证明的 false positive；不声称 remote TSan/race-free，不 suppress warning。Installed find_package Program C++/C ABI/dualQuickJS3个 consumer pass。Fresh installed NeoGraph/SchemaProvider typed consumer 实际2个 HTTP request、coroutine 开始前 provider 销毁、native/tool replay、refusal、known-zero/raw 保留、实际 LinkedMismatch 拒绝均 pass。Browser Alice/Bob isolation、generation2 replacement 已实际目视验证；PostgreSQL Program Chat black-box6项18.989秒 pass。最新 SDK26/26、失败0、74.07秒 pass。最终 ReleaseGraph16配置 ×fresh process3次/48记录以38.29秒、失败0、全部 actual protocol/owned-outcome check pass 完成。NeoGraph `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保留独立最终 cohort。测量期间未执行 compiler/付费 model；历史 cohort 不变，不声明 semantic/resource equivalence。Unstable SDK/ABI3 不是稳定 release 或更广 platform qualification。

  已完成的 chat pair 不证明 downstream vendor 对 native-continuation 的消费。
- **Standalone bank journal 修正——当前契约已修订；实际 runtime 证据如下。** Protocol 要求单调 trusted-store namespace obligation，以及真实不可变 original owner/thread/graph scope、ceiling、deadline/clock identity、generation。只有对全部 checkpoint commitment/revision 的精确 durable head CAS 才可发放 host-owned opaque lease。精确 pending effect window 必须在 provider I/O 前持久化；结算必须采用真实 SDK outcome 及实际 charge、nullable report、hold、dedup identity。Checkpoint 与 next head 必须在同一 owned actor/revision 下原子 publish。删除 bank metadata、prune checkpoint、replay old authenticated snapshot、覆盖同一 ID 或失去 actor 都不能授予 credit。已有 65 hold 时将 ceiling 130 降至 129，不能再批准另一个 65；已证明 no-effect 的失败可 release unchanged head，使 authentic 130 恢复仍可进行。Crash/unknown/lost-lease window 保持 hold，不 refund/retry/fallback。Plain/pristine archive 配置不授予 money/native spending lease；当前 `config.usage` 不能替换既有 standalone obligation，Program/external-bank journal 所有权不变。这是要求契约。实际 currency/custody 证据与 instrumentation 限制见下文，不是稳定 released API 保证。
  实际声明包括 `ManagedBudgetLeaseScope`、`OwnedManagedBudgetLease`、`ManagedBudgetEffectReceipt` 及 checkpoint-store acquire/begin/settle/publish/release 操作。`begin_managed_budget_effect` 绑定精确 claim/prepared request digest。实际 backend/runtime currency probe 与 installed-consumer 结果已记录；声明本身不是 runtime 证据。

  **真实 InMemory shared-bank fork 已保留并实证。** 原始真实 C++ fork 使用 ONE original financial journal 和 trusted current branch head，不复制 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及 `_async` 要求 authentic current source/full commitment 与实际 same-bank native C++ pointer；durable standalone fork 仍明确 unsupported。`OwnedManagedBudgetLease::scope()` 及 original owner/thread/graph、ceiling、deadline/clock、generation 保持不可变。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` 单独选择 execution branch；`GraphState::budget_original_thread_id()` 标识原始 financial bank。精确 selected-branch head CAS 和 global actor/revision 将所有 branch 对 canonical current counter、pending effect、burned identity 串行化。Original/fork branch 保持可用但不补充额度。Stale snapshot、checkpoint copy、imported JSON 不能发放 alias 或回退 head。原始 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 证明在未修改 test_graph_engine.cpp:810–913 中 PASSED；saved original ceiling30 不同于 effective fork ceiling20；widening31 和 JSON-only restore 必须拒绝。Unbounded reported observation 是事实 data，不是 finite grant。只有已证明 zero-effect 的 lease 能 release unchanged head；unknown/pending effect 保留 obligation。

  **当前 release-error 契约；实际 suite/probe 如下。** `<neograph/graph/engine.h>` 中 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始 execution exception，`release_error()` 暴露次要 durable lease-disposition 失败。`outcome()` 在存在真实 SDK 证据时保留它，若没有 SDK outcome 则为 null；release 失败不能伪造结果或授权重新 dispatch。Closed `_neograph_managed_budget_scope` metadata 描述原始 logical scope/cap/deadline clock/generation，但只是 data，不是 backend CAS 权限。

  **Archive-owner/retention 契约；实际 suite/probe 如下。** 只有 finite standalone root 或 authenticated finite source 才从真实配置的 `sp::NativeArchive::owner_scope()` 继承省略的 original owner；unbounded/plain owner metadata 语义不变。显式冲突的 archive owner 在 lease acquire 前拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及对应 Core/Async storage capability 默认 false；真实 InMemory backend override 为 true，wrapper 必须委托真实 retention。此 read-only 描述允许合法 unleased/plain/unbounded C++ native checkpoint custody，但不授予 spending credit 或 native replay authority。Leased custody 使用真实 store-issued receipt，而非 JSON flag 或猜测的 store type。

  **Native-custody pre-I/O gate；实际 suite/probe 如下。** Managed effect begin 在任何 pending-effect/slot/held-window 修改前要求真实绑定的 NativeArchive 或实际 local store-issued private C++ retention capability。Private capability 不从 JSON import，也不经 wire 传输。C++ sidecar 无法跨越边界，因此即使 remote backend 是 InMemory，gRPC 仍要求真实 client/server archive。Archive 未提供 finite source owner 时，原始 anonymous owner scope 保持空值；真实 archive binding 必须匹配 original scope。Financial head/lease 证据本身不证明 native-custody readiness。

### 新增
- **按所有者作用域划分的实验性活动 Program 启动。** C++ `ProgramRuntime`
  和 Python `LocalProgramHost` 在新运行时只选择一次已批准的不可变激活记录，
  同时返回所选激活记录和固定于对应版本的句柄。回滚仅影响后续启动。
  图迁移不再假定未知的节点局部状态可迁移，而是拒绝不透明的检查点元数据。
  跨存储持久 GC 和生产性能验证仍是独立的准入条件。
- **Program Core Tool grant 的持久身份。** 宿主可在 SQLite 中批准受所有者
  作用域约束的精确 Tool grant，并在每次 Program 操作及重连时重新绑定
  gate/controller。运行时还校验已批准的可执行绑定 fingerprint；撤销在重启后
  仍然有效，并阻止该 grant ID 的所有尝试。效果结果仍需单独的逐调用持久
  Tool broker。
- **性能构建配置。** 新增面向本机 GCC/Clang 构建的可选
  `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` 开关；当单配置生成器未设置
  `CMAKE_BUILD_TYPE`、因而遗漏 Release 优化标志时发出警告。Release 加固
  默认保持启用。
- **Program 驱动 A2A 服务的严格认证。** Program A2A 构造函数新增可选的
  `require_authenticated_requests` 标志。启用后，除协作 envelope 外，普通
  message、stream、task 查询和取消 RPC 也必须通过已配置的 authenticator。
  为保持兼容，默认值仍为 false。
- **宿主拥有的 A2A 控制消息拦截。** `ProgramAgentAdapter` 现在可在启动
  `ProgramRuntime` 前，把已认证的 typed message 交给宿主 callback。callback
  必须保持 task/context identity，返回的 Task 会保留用于常规 task 观测。
- **不受信任的补充运行时上下文。** 新增
  `ContextArtifactKind::UntrustedSupplemental` 和
  `ContextPlacement::AfterHistory`。现有 artifact kind 的 system message
  行为保持不变，同时可以将显式不受信任的 RAW 或 derived context 绑定到
  receipt，并在完整的 Human/AI/Tool 时间顺序历史之后作为 user data 传递。

### 修复
- **子图恢复边界。** 管理状态更新保留中断执行的续行身份；保留型子图区分新的
  父调用与同一次调用的恢复。Stateless 静态中断在产生效果前拒绝，中断恢复使用
  异步检查点读取。
- **MCP 与宿主进程隔离。** 采用的工具需要独立固定审批和精确参数值；撤销或
  schema 变化会终止仍被引用的 client。加固启动与 stderr 限额、handle/FD
  隔离、后代清理及输出读取上限。
  macOS 启动支持 Darwin 信号宏，并在 fork 前通过平台访问器读取继承环境。
  POSIX close 回退使用 descriptor 硬限制而非 65536 上限，也会关闭降低软限制前
  已打开的高编号 FD。Windows 继承测试检查父进程原始 event 的状态，
  不再把可复用的 handle 数值当作对象身份。
- **干净 runner 上的契约检查。** CI 显式安装 Protobuf 开发头文件与库，
  而不只是编译器和运行时包。
  隔离 stdio fixture 显式批准用于发现 Python 的搜索路径。
- **租户安全的持久 Harness 记录。** File/SQLite 中的作用域 ID 和引用可往返，
  长 File 键使用固定长度 hash 文件名。SQLite 保留策略不会删除其他 namespace
  的记录或 journal。
- **可运行的 Harness CLI。** 启动策略使用 Program 支持的子深度上限，真实 stdio
  compile/start/get 回归验证覆盖交付的 binary。
- **新目录中的活动 Program 启动。** 在固定运行版本之前恢复已选定的持久版本，
  不再重新读取激活指针。
- **生成 artifact 的持久重放。** Provider receipt 在重新打开和结果核对后仍
  保留 artifact 顺序、payload 和嵌套 metadata。易失性 SQLite 路径以及缺少
  artifact 证据的旧 receipt 会被明确拒绝，不会伪造空结果或重新发送调用。
- **宣传视频依赖安全。** 将 `docs/promo/package-lock.json` 中的间接依赖
  `js-yaml` 从 4.3.1 更新至 4.3.2，修复通过合并空映射耗尽 CPU 的
  拒绝服务漏洞（CVE-2026-84375 / GHSA-2883-xcg3-v3hh）。
- **TSan CI RSS 检查。** 由于 shadow memory 和运行时管理开销可能超过
  普通构建的阈值，在 GCC/Clang ThreadSanitizer 下跳过仅适用于普通构建的
  RSS 上限检查。保留 sanitizer 下的 200 路并发 stress test，以及非
  sanitizer 构建中原有的 10 MiB RSS 上限。

## [0.12.1] - 2026-08-23

### 修复
- **Python 示例运行时契约。** 路由通道示例改用保留的 `__route__`，声明动态
  `Send` 载荷通道，并将纯缓存示例设为 `CacheScope::Reusable`。同时保留
  OpenRouter 基础 URL，使 Windows 控制台输出具备 UTF-8 安全性，并拒绝或重试
  状态成功但内容为空的 reasoning 模型响应。
- **Responses 传输演示。** 官方 OpenAI 使用真实的 WebSocket 流式入口，兼容
  网关则选择构建中可用的 HTTP/2 或 HTTP/1.1。深度研究演示采用有界的令牌和
  时间预算以及三路、两 worker 的 fan-out，并已通过 OpenRouter 实际验证。
- **可操作的 Windows 网络错误。** Python 现公开 `_HAVE_LIBCURL`，并把采用
  本地代码页编码的 `std::system_error` 消息转换为含 category 与 code 的安全
  ASCII 诊断。因此 Winsock 10060 等根因不会再被 `UnicodeDecodeError` 覆盖。

## [0.12.0] - 2026-08-23

### 已添加
- **严格的运行时插桩完成。** 新增了 `StrictRuntimeProfile`、持久的提供方终端结果回执和 SQLite schema v3 迁移、带有 JSON-RPC stdio/HTTP 工件发布的传输无关强制 Hook 后端、通用必需上下文和 `HardConstraint` 工件、精确保留的 `ContextTransformReceipt`、持久的运行时开发者指令决策，以及编译前预留的 `ProgramSynthesisGateway`。生成的源码仍无法自行激活、绑定、迁移或生成；这些仍是由主机拥有的独立 Program 转换。`DurableProviderDispatchReceiptStore` 的自定义子类现在必须实现终端 `settle`/`outcome`；C++ 使用者必须针对此 ABI 变更重新构建。

- **隔离的 PostgreSQL Program 存储集成装置。** 新增摘要固定的、仅回环的 `tests/fixtures/q7-postgres/compose.yaml`，带有 tmpfs 存储和健康门控。当 `NEOGRAPH_TEST_POSTGRES_URL` 指定一个可抛弃的测试数据库时，`ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility` 覆盖发布、激活、重新打开和所有者隔离。这是测试基础设施。

- **失败时关闭的 QuickJS 遗留数据排空审计。** 新增了 `scripts/audit_legacy_drain.py` 及其 CTest 契约。该工具从显式枚举的冻结 Program/测试装备存储快照生成规范化的、内容寻址的证明；它拒绝未知或可变记录、未分类的遗留来源、仅排空记录，以及活动或可恢复的遗留运行。带有活动 `-wal`、`-shm` 或 `-journal` sidecar 的 SQLite 输入在打开前即被拒绝，因此活动 WAL 数据库的原始副本不能作为最终证明。 这确立了证据机制，但并不声称部署特定的最终排空或遗留解析器删除已完成。

- **PostgreSQL 终排空归档扫描。** 遗留排空审计器现在接受冻结的 `program_postgres_dump` 自定义归档，并且仅以仅数据、严格表、脚本输出模式调用 `pg_restore`；它从不恢复到位止。它根据持久化的身份验证 Program 包、版本和激活表，拒绝缺失或更改任何必需表的归档，并将旧版 Program 版本的激活视为最终移除的阻塞项。

- **无部署的最终证明模式。** 遗留排空审计器仅在从未存在预发布或生产 NeoGraph 部署时才接受命名的操作员认证。该模式既不接受存储目标也不接受历史遗留工件，将生成的证明标记为 `evidence_mode: "no_deployment_attestation"`，并对任何混合或未认证的空库存采取失败时关闭。它无法覆盖已排空、删除、丢失或无法访问的历史状态。

- **OpenRouter 提供程序路由。** `OpenAIProvider` 现在将对象在 `CompletionParams::extra_fields.provider` 传递到 Chat Completions 请求体中作为 `provider`；非对象值在发出 HTTP 请求前失败。这公开了 OpenRouter 文档化的按调用路由首选项，同时保持其他原生 `extra_fields` 键被忽略。live Beast cookbook 固定其提供程序，并使用显式的 180 秒超时来实现其 4,000 token 的生成预算。

- **Copy Ninja 本地图节点桥接。** 新增了无传输的 `a2a::CopyNinjaNode`，它包装了一个单独物化的 Copy Ninja 工装，读取 `prompt`，并覆盖 `response`。 新增了 `cookbook_the_beast_copy_ninja` 实时食谱：其 LLM 只能编写此固定本地节点，必须通过正常 Core 门控之后的第四道本地绑定门控，并且如果合成源智能体观察到 RPC 则失败。 卡片文本、端点、凭据以及源仍然排除在未获准入(admission)的候选之外，并且调用方提示永远不会进入编写 LLM 请求。


- **可选的 Program 组件边界。** 新增了选择加入的 `NEOGRAPH_BUILD_PROGRAM` 开关、导出的 `neograph::program` 目标以及 `<neograph/program/program.h>` 入口点。 安装的软件包组件发现现在仅在构建时报告 Program；仅安装 Core 时保留现有的 `neograph::core` 链接接口。

- **不可变的Program值模型。** 新增了稳定的类型化诊断、深度拥有的规范JSON和C++构建器 `ProgramSource` 输入，以及不可变的内容寻址 `ProgramBundle`/`ProgramVersion` 值、规范序列化、带 SHA-256 算法标签的身份、source maps、导入，以及严格版本化的存储值模式。 `neograph::program` 现在是一个已编译的导出库，同时仅依赖 Core。Bundle/版本 v1 投影现在要求 sealed Core 定义和计划身份、语义化版本的可执行摘要、契约、闭包、边界以及类型化的准入(admission)/物化收据。它们的身份绑定格式和存储版本，语义集使用稳定顺序，diagnostics 拒绝无效指针、逆转的 spans 和未知枚举，而当没有可用的精确偏移时，解析器 spans 保持缺失。

- **密封 Program 准入(admission)闭包。** 添加了不可变的 `RegistrySnapshot`、`AdmissionProfile` 和 `PolicySnapshot` 值，具有构建时可调用捕获、严格规范清单、域分离指纹，以及在 `ProgramVersion` 中的失败时关闭跨指纹验证。Core 现在为 Program 物化公开了显式命名的仅本地解析/链接/验证入口点；现有的本地优先/全局回退重载保持不变。注册表现在记录规范精确的可执行依赖边，用于传递准入(admission)闭包，并且仅本地条件检查覆盖遗留键边文档，无需查询进程全局注册表。

- **单根 `call_core` Program 编译器。** 新增了`ProgramCompiler`，它仅接受封闭的 Program-v1 信封，在执行密封前执行纯局部 Core 解析/往返/语义验证，并发出带有 RFC 6901 指针和 source-map 归属的聚合类型化诊断。编译过程推导出规范 Program、注册表、传递可执行闭包、能力/效应、导入 Merkle、密封定义以及 Core plan 身份，而无需调用工厂或可调用对象。所编写的文档 schema、完整有限预算合同、零分派拒绝测试以及静态和共享已安装消费者覆盖范围随编译器一起提供。Core 获得了新增的全量解析/往返及局部验证报告，而遗留的 throwing API 保留其现有行为。

- **固定 Program 运行时垂直切片。** 添加了 `ProgramCatalog`、`EngineGenerationCache`、`ProgramRuntime`、共享的 `ProgramHandle`、不可变的 `ProgramResult`、类型化 Program 事件信封、内存中的 `ProgramStore`，以及仅追加的 CAS `ProgramJournal`。准入(admission)在物化之前重新计算不受信任的包语义；每次尝试固定一个不可变的 Core 代并调用现有的 `GraphEngine` 异步路径。运行时执行现在将完成、中断、精确检查点恢复、取消、超时、Core 步骤耗尽、检查点不兼容和失败映射为类型化终止状态，同时保留不可续用预算和检查点谱系。日志提交先于检查点/终止事件投递，并发恢复只有一个 CAS 胜出者，并且 PR6 切片在 Core 代理存在之前拒绝有副作用或非空模式的 Program。

- **QuickJS 控制语言前端。** 添加了可选加入的 `NEOGRAPH_BUILD_QUICKJS_CONTROL`、密封的 `ProgramSource::from_javascript(...)`，以及一个私有的仅编译 QuickJS 上下文。源信封固定引擎/语言/宿主 API 版本；其唯一的 `ng` 宿主表面是一个版本化图构建器，内存、栈和中断轮询限制失败时关闭。JavaScript 产生一个不可变的 `call_core` Program 计划，并且永远不会成为运行时 VM、字节码工件或 Core 依赖。

- **A2A Agent Card 兼容性候选。** 添加了一个单请求、未认证、无重定向的 well-known 卡片收集器，以及一个仅工厂的不可变候选编译器。候选仅保留摘要固定的来源、有界协议事实和安全技能 ID；自由格式卡片文本、广告的 RPC 端点、提供商/安全配置和凭据被排除。Copy Ninja PoC 额外要求独立观察到的行为固定到该摘要，并且绝不派发源智能体。

- **SQLite Harness 记录存储（issue #147 后续）。** 为 WAL 支持、模式版本化的工件/运行持久化添加了可选的 `neograph::mcp_sqlite` 目标和 `SqliteHarnessRecordStore`，具有不可变工件和运行到工件绑定。Harness MCP 二进制 现在将记录存储在 `runs.db` 中，而检查点仍保留在 `checkpoints.db` 中。
- **AMD OpenMP 目标卸载概念验证。** 添加了可选加入的 `bench_openmp_offload` 基准测试，它在相同的数值 fan-out 工作负载上比较串行 CPU 执行、OpenMP 自动线程化、每迭代 GPU 映射和持久 GPU 数据。它报告真实设备与主机回退执行、正确性、包含传输的延迟、仅内核延迟和加速比。`NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201` 为 Radeon AI PRO R9700 启用 ROCm/Clang 设备镜像。


### Revised translation: 变更

- **C++ ABI 和 SOVERSION 策略（issue #194）。** 每个编译的公共 `neograph_*` 库现在携带项目 `VERSION` 和主 `SOVERSION`；安装的共享库从其自身目录解析兄弟依赖。Pre-v1 版本使用 ABI 代 0，但可能声明强制重建边界。所有针对 `0.11.1` 或更早版本构建的 C++ 消费者必须为下一个版本重建，因为 `NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser` 和提供商配置公共布局已更改。包含有界 `UsageAccumulator` 预留的版本是另一个强制重建边界：其公共对象布局现在携带预留记账状态。版本 1.0 将 ABI 代更改为 1 并冻结受支持的 v1 布局。CI 现在构建并运行隔离的静态和共享已安装消费者，并检查 ELF/Mach-O 加载器元数据。参见 [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)。
- **`GraphNode::run(input)` 迁移指南完成。** Python `GraphNode` 基类不再引用已删除的 `execute*` 方法；当 `run(input)` 缺失时，它引发包含迁移文档路径的 `NotImplementedError`。C++/Python 参考、异步/流式指南和示例 README 已与实际 v0.9.0 单一入口点对齐。迁移过程在 [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) 中记录了 C++ 和 Python 示例。
- **Provider API 永久兼容性策略（issue #5）。** 计划移除的 `Provider::complete()`、`complete_async()`、`complete_stream()`、`complete_stream_async()` 和基于回调的 `invoke()` 已被撤销，`[[deprecated]]` 警告已被移除。现有 API 继续接收兼容性和安全修复。新的 Provider 实现和直接调用者分别建议使用 `CompletionProvider::do_invoke()` 和 `invoke_request(CompletionRequest)`；将所有新功能回移植到现有 API 不保证。公共签名、虚拟顺序、对象大小和 vtable 保持不变。

### 移除

- **弃用的 TransformerCPP 集成示例。** 移除了 `example_inproc_gemma`、`NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE` 和 `TRANSFORMERCPP_DIR`，它们依赖于一个不再可用的外部托管仓库。使用标准 OpenAI 兼容本地服务器的 `example_local_transformer` 被保留。

### 已修复

- **补全 Python Program wheel 依赖。** PyPI 构建现会启用 QuickJS 控制运行时，在扩展模块旁捆绑 `neograph_program` 加载器，并导出 Windows 上跨 Program/Harness DLL 边界使用的私有求值器。这样可防止 `P_JS_UNAVAILABLE`、因缺少加载器导致的 import 失败，以及共享构建中的 `LNK2019` 失败。命令 identity 的构建也会避免跨组件 dylib 边界的嵌套 initializer-list JSON 复制，从而防止在 Program 执行期间可能发生的 macOS arm64 崩溃。Windows wheel repair 会对外部 DLL 名称进行哈希改名，防止预先加载的系统 `sqlite3.dll` 覆盖捆绑版本并在构建 `SQLiteContextStore` 时导致崩溃。
- **有界远程传输和凭据来源。** HTTP/1.1、HTTP/2、SSE 和 WebSocket 接收路径现在在分配不受信任大小之前强制执行保守的响应、头、块、行、帧、握手和消息限制。重定向的 POST 请求仅在规范化同源内被跟随，提供商凭据需要 TLS，除非启用了显式数字回环开发异常，并且 WebSocket 调试输出不再包含请求头或负载。
- **QuickJS `all` 加入启动竞争。** 完成处理器现在在关闭 JavaScript 加入之前等待初始成员启动注册。立即完成的子项不能再在其兄弟初始或替换命令派发之前恢复生成器；重复的运行时回归覆盖两条路径。
- **Harness 聚合发现的溯源（issue #174）。** 详情现在包含一个 `finding_sources` 数组，与现有的扁平 `findings` 数组对齐。每个条目记录其聚合索引、源 worker ID 和 worker 本地索引，同时不改变经 schema 验证的 worker 输出或既有的 `findings` 形状。
- **Harness 导出结果 lint（issue #173）。** 节点效果契约现在可以在可选的 `exports` 数组中声明写入通道，当调用者在图执行后消费它们时。因此，Harness 编译和 `GraphEngine` 运行时验证都为真正仅写入的通道保留 E6，而不会在 `final_result` 上产生误报。
- **MCP 2025-11-25 工具客户端契约现代化(issue #147 M0)。** 初始化现为幂等操作并保留协商后的服务器元数据;HTTP 工具复用发现会话;`/mcp` 端点构造由请求和通知共享;工具发现遵循不透明游标;JSON-RPC 代码/数据、完整工具元数据、非文本内容、`structuredContent`、`isError` 和 `_meta` 在 C++ 和 Python 路径中均得以保留。新增可配置的 HTTP 超时/静态/动态标头、输出模式验证、严格响应 ID 检查,以及类型化的 `InitializeResult`、`ToolDefinition`、`ListToolsPage` 和 `CallToolResult` API。SSE 检测现使用 `Content-Type`,而非将包含 `data:` URL 的 JSON 错误分类。
- **每任务取消状态和已发布发射生命周期安全。** `GraphEngine::run`、`run_async`、`run_stream`、`run_stream_async` 每次运行从调用方提供的父级创建一个执行子级，仅将该子级绑定到内部 `co_spawn`/同步桥，并将同一子级作为 `RunContext` 传递。因此，在单个父级下取消所有并发运行不会覆盖彼此的取消槽。分叉的执行子级通过已发布的发射保留现有的 `shared_ptr` 所有权，防止引擎工作完成与发射执行之间的释放后使用。由取消引起的 asio `operation_aborted` 被传播为 `CancelledException`，而不是可重试的节点错误。`CancelToken` 0.11.x 对象布局和内联/仅头文件行为不变。已编译的 C++ 使用者需要重新编译才能获取更新的 `fork()` 生命周期行为。仅替换共享库可保持对象布局兼容性，但嵌入在消费者二进制文件中的现有内联函数体不会改变。然而，当外部代码在它直接创建的令牌上调用 `bind_executor()` 时，调用方仍负责保持令牌存活，直到执行器的已发布工作完成。
- **PostgreSQL 异步连接全局超时策略已记录。** 异步初始连接和替换在所有主机/IP 地址上使用单一超时。直接写入正向连接字符串中的显式 `connect_timeout` 强制执行,最小值为 2 秒;未指定、为零、为负或仅由环境变量/服务文件提供的值使用操作安全默认值 30 秒。这与 libpq 的按主机同步超时有意不同;同步创建/替换行为保持不变。
- **JARVIS 模拟构建修复（问题 #130）。** 修复了 `cookbook_jarvis` 编译失败，原因是当音频依赖缺失时 `MicCapture` 仍为不完整类型。添加了 `NEOGRAPH_JARVIS_FORCE_MOCK`，使 ASan CI 始终构建模拟配置，无论运行器安装的包如何。会话运行器现在使用实际的 CMake 输出路径和专家目标名称，并正确启动现有的 `demo_mcp_server.py`。
- **节点失败上下文保留（问题 #123）。** C++ 执行错误作为`NodeExecutionError`传播，包含原始`exception_ptr`、失败节点名称和尝试次数；终端`ERROR`事件也记录相同上下文。在 Python 中，原始异常对象、类型、参数、用户属性和回溯按原样保留，仅添加`.node_name`和`.attempts`属性。`NodeInterrupt`、取消和内存不足异常遵循现有控制流，不被包装。

### 修复（文档）

- **从 Provider 手册中移除了被忽略的每节点提示(issue #116)。** 修复了三个 Python 示例,这些示例描述使用 `config.system` 的多角色行为,而内置 `llm_call` 不会读取该行为。每个示例已重写为使用 `NodeContext.instructions` 的严格单调用图,相关 README 已与实际行为对齐。
- **保留字`RunContext::deadline` 文档更正（问题 #115）。** 修复了文档和 Doxygen 注释，这些内容将`deadline` 和 `trace_id` 呈现为可使用的每次运行元数据，而它们无法通过`RunConfig` 设置，也未在 Python 中暴露。
- **`GraphNode::run` 示例签名修复（问题 #129）。**修复了公共头文件示例接受`const NodeInput&`（按引用传递）而未能覆盖实际按值虚拟函数的问题，并通过编译时测试锁定了按值约定，以满足协程参数生命周期要求。

### 已添加

- **向后兼容的 Provider 迁移路径。** 新的`CompletionRequest` 将流式模式与回调存在分离，`CompletionProvider` 要求新实现仅写入`do_invoke()`。现有的`Provider` vtable、四个旧虚拟函数、基于回调的`invoke()` 和 Python `complete()` 子类契约均保留。

- **Python 持久化后端** (#117) — `Store` 和 `CheckpointStore` 现在可作为可构造的子类基类，并具有 C++ 到 Python 的虚拟分发。`StoreItem`、`CheckpointPhase`、`Checkpoint` 和 `PendingWrite` 以 JSON 形状的字段暴露；检查点待写入方法保持可选。
- **Python 同步取消** (#119) — Python 调用方可以构造一个 `CancelToken`，将其赋值给 `RunConfig.cancel_token`，并从另一个线程协作地停止 `engine.run()`。

- **Python 检查点历史** (#118) — `GraphEngine.get_state_history()` 以最新优先的顺序暴露检查点记录，使调用者能够在从历史状态分叉之前检查父链接、元数据、步骤和 ID。

- **DSL 表面（细化层）+ 模式演化门** (#75 M4)。
  - **Elaborator**：`vars`（`{"$var":...}` / `${...}` 插值，无环强制）/ `templates`+`use`（精确参数匹配强制，节点前缀重命名——包括局部引用、屏障和路由；通道是共享状态，因此它们全局合并）/ `when` 条件包含。**非图灵完备且完全**：每个DSL文档在有限时间内规范化到唯一核心，并且相对于该核心是幂等的。所有错误均报告DSL源代码坐标（`use[2].args`，`vars.model`），并包含源映射（输出位置 → 生成语法）。锁文件工作流：`./example_elaborate harness.dsl.json > harness.json`（示例53）。
  - **`GraphCompiler::upgrade_to_latest()`**：无损 v0→v1 机械转换——严格拒绝的键被隔离到 `x-upgraded-<key>` 注释命名空间（零数据删除），空屏障被显式移除。整个语料库经过测试，保证“旧版宽松编译 IR == 升级后严格编译 IR”（规范等价，版本戳除外）。
  - **Schema 演进门**：针对 `tests/fixtures/schema_snapshot.json` 基线的仅添加子集判定（JSON Subschema 族的可判定子集）——节点类型/属性/reducers/条件的移除、必需集增加、封闭条件标签更改以及效果契约更改均导致测试失败 = CI 合并阻断。不兼容更改强制在同一审查提交中进行版本升级 + 升级器 + 快照重新生成。

- **PBT / 增量验证框架**（#75 M3）。300 种子确定性拓扑生成器（来自 schema 包络的有效严格文档，自检测功能覆盖率——当 conditional_edges/barrier/interrupt 出现率低于 30% 时测试失败：未测试的功能成为失败，而非静默空洞）。
  - **突变检测**：在 300 种子语料库上确认，翻译验证捕获了所有 5 种丢弃类型（conditional_edges/edge/barrier/interrupt/channel）的所有应用 + 3 种错误接线类型（路由折叠 / 边重定向 / 节点重命名 = 丢弃+伪造平衡）。应用率下限（10% 的种子）也已断言。
  - **参考解释器增量**：一个独立模型，从代码不相交的实现重新实现文档化的超步语义（goto 抢占、屏障累积、字典序回退、隐式 __end__），与 Scheduler 在 12 步 × 300 图上进行比较（DESIL 教训：仅靠验证器无法捕获错误执行）。
  - **Engine ↔ Studio 共享语料库**：`tests/fixtures/topology_corpus/` 15 个变体（3 个有效 + 12 个违反 E3–E11）与 NeoGraph-Studio `tests/corpus/` 字节相同，两者断言相同的判定（代码:严重性多重集）——两个实现不能静默分歧。

- **GraphValidator — 拓扑静态语义检查（E3–E11 + 效果）**（#75 M2）。解析（M1）与执行之间的传递层。在严格文档（schema_version>=1）中，错误是编译失败，警告是 stderr lint；在宽松文档中，仅错误级诊断作为 stderr 警告浮现（对现有图零噪音）。判定哲学 = 检查器健全性优先：只有那些在引擎语义下永远不可能正确的事物才是错误（悬空引用 E3、无信号路径的屏障 E8——goto 绕过屏障记账因此不可恢复、空路由 E10——分发将解引用 rend() UB、未声明通道写入 E4——运行时确认抛出）；Command.goto/Send 可以证明合理的事物是警告（可达性 E7、无逃逸循环 E11、无屏障的普通 fan-in E9、覆盖竞争 E5、死通道 E6）。每个诊断都附带机器可读的见证（反例）JSON——用于 Studio 画布高亮（M3）。
  - **路由完备性（E10）**：`ConditionSpec` 标签契约引入。通过 `register_condition` 3 参数重载声明条件的输出标签集要求封闭条件路由与标签精确匹配——未覆盖的标签落入调度器的“字典序最后路由”回退（顺序相关的任意目标），这是错误。内置 `has_tool_calls` = 封闭 {false,true}，`route_channel` = 开放 + 已知 {default}。
  - **通道效果契约**：`register_type` 4 参数重载声明每节点类型的读/写通道。E4/E5/E6 分析仅在图中**每个**节点类型都被声明时激活（单个未知类型跳过整个分析——健全性优先于覆盖）。内置 3 种类型（llm_call/tool_dispatch/intent_classifier）完全声明。
  - `node_effects` · `condition_specs` 添加到 `export_schema()`（现有 `conditions` 数组保留用于向后兼容）。22 个新测试。

- **拓扑编译时一致性门 — 消费键记账 + 翻译验证**（#75 M1）。双重机制结构性阻断“静默语义丢失”类别（与 v0.1.0–v0.1.7 `conditional_edges` 静默丢弃同种）：
  - **消费键记账**：声明 `"schema_version": 1` 的文档切换到严格编译——未消费的键（拼写错误 `conditionnal_edges`、不支持的字段、被空 `wait_for` 静默丢弃的屏障、内联条件上被忽略的 `to`）全部被收集并报告为编译错误。标记发生在**解析块内部**，因此擦除解析阶段也会擦除标记，导致使用这些功能的严格文档立即失败——一种丢弃回归不可能静默的结构。`_`/`x-` 前缀键（`_comment`、`x-studio-*`）始终允许作为注释命名空间。没有 `schema_version` 的现有文档保留宽松行为（字节保留）。
  - **翻译验证**：每次编译时执行 `CompiledGraph::to_json()` 重新发射 + `GraphCompiler::canon()` 规范形式检查 `canon(input) ==
    canon(re-emit)`。不匹配（= 编译器丢弃了某些内容或接线错误）在严格文档上抛出异常，在宽松文档上输出 stderr 警告。等价性是结构比较——像交换路由键这样的接线错误也会被捕获（存在性比较会遗漏的类别）。
  - `NodeFactory::config_schema(type)` 查询添加，`schema_version` 字段在 `export_schema()` 中记录。27 个新测试（`tests/test_compiler_strict.cpp`）——v0.1.x 丢弃突变模拟（conditional_edges/barrier/interrupt 丢弃
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### Revised translation: 变更

- **stdio MCP 并发调用——用于 I/O 重叠的 correlation-ID 解复用器。** `0.11.0` 并发工具调度实际上仅对 HTTP MCP 实现了重叠。stdio MCP 在 `StdioSession::rpc_call_async` 中为**整个请求→响应往返**持有一个容量为 1 的通道锁，通过单个会话管道将一轮中的多个调用串行化（墙钟时间 ≈ 延迟之和）。单个管道并非根本原因——JSON-RPC `id` 的存在正是为了在单条连接上进行流水线处理。将锁替换为 correlation-ID 解复用器：
  - 将容量为 1 的通道重新用作**仅写锁** — 仅在帧写入的瞬间持有，因此两个调用的字节不会交错，而读取不再被串行化。
  - 单个读取协程（`run_reader`）独占读取侧，并通过 JSON-RPC `id` 将每个响应行投递到正确调用方的接收端。N 个并发调用重叠读取，因此墙钟时间 ≈ max(延迟)——**但仅当对端 MCP 服务器并发处理时**（单线程顺序服务器会触及 Amdahl 下限）。
  - 读取器仅在存在进行中的调用时惰性运行，并在等待者为空时退出，因此私有 `run_sync` io_context 正常返回。等待者仅在其调用方处于等待状态时存在，并通过 `MCPTool` 的 `shared_ptr` 保持会话存活，因此读取器永远不会触及已销毁的会话（无需析构函数 join）。在管道 EOF/错误时，读取器关闭所有接收器，因此等待中的调用方会收到异常而不是无限挂起。
  - **无 API/语法变更**——公共头文件未改动，现有代码无需重新编译。引擎开销回归为 0（`bench_neograph` 交错 A/B，seq/par Δ 0%）。
  - 测试：基于线程的延迟夹具 `tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO`（5×100 ms 调用在约 130 ms 内完成，而串行下限为 500 ms；通过 `id` 验证每个响应路由到其调用方）。ASan+UBSan ×3 通过。

## [0.11.0] - 2026-06-25

### 已添加

- **并发工具调度 — `Tool::execute_async` 官方异步路径。** `ToolDispatchNode` 从单个助手轮次中**并发**执行多个 `tool_call`，使用引擎的 `make_parallel_group`。此前每次调用通过同步方式顺序执行 `execute()`，且MCP工具尤其会阻塞于每次调用通过 `io_context` 的过程，导致并行的MCP调用无法重叠（在外部C++分支中通过并行MCP调用发现）。修复： `run_sync` 生成自身
  - Virtual `execute_async()` 已添加到 `Tool` — 默认实现桥接到同步 `execute()`，因此现有工具无需更改即可正常工作。
  - `MCPTool` 转换为 `AsyncTool`，使用原生 `execute_async`（stdio 使用 `rpc_call_async`，HTTP 使用新的 `MCPClient::initialize_async`/`call_tool_async` 进行异步握手——`run_sync` 已移除）。
  - `ToolDispatchNode::run` 通过与 node fan-out 相同的 `make_parallel_group` 惯用法并发调度调用（单次调用内联），结果按调用顺序应用。通过同步 `execute()` 门面保持向后兼容。
  - 验证：478/478 ctest，Valgrind 0 泄漏，TSAN 0 竞争。

### 已修复

- **Python 异步执行异常保留（issue #122）。** 修复了 `run_async`、`run_stream_async` 和 `resume_async` 用新的 `RuntimeError` 将原始 Python 节点异常包装为字符串从而覆盖它的问题。现在原始 Python 异常对象、类型、用户属性和回溯通过 pybind11 的标准异常转换路径保留，C++ `py::type_error` 作为 Python `TypeError` 交付，与同步执行一致。`resume_async` 中的空回调现在保留到协程完成，同时修复了 pybind11 3.x 中暴露的悬空引用冲突。

### 修复（文档）

- **README 摘要徽章针对缺失条件和沙箱测量揭示的内部矛盾进行了修正。** “四个轴”摘要表中的徽章从正文/深入分析中剥离了测量条件，读起来像夸大其词。已修正为与正文测量数值和条件一致（测量数据表本身未变）：
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`**——徽章上的 17 µs 与正文（`At N=10,000 concurrent ... 7 µs p99`）相矛盾，且 `flat` 描述的是 GPU 受限负载测试的运行延迟（648 ms），而非 µs 级测量。徽章已与正文的测量数据和条件对齐。
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`**——`libc.so.6` 专用和 1.2 MB 仅适用于 MinSizeRel + 静态 libstdc++ 构建（默认 Release 动态链接 libstdc++/libgcc_s/libm/libc）。该条件已在深度剖析 §size 中记录，已恢复至徽章。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 直接依赖确实是 `certifi` + `pydantic`（两个），但实际安装树包含 7 个包，包括 pydantic 的传递依赖（pydantic-core、typing-extensions、annotated-types、typing-inspection）。
- **已将 `-DNEOGRAPH_BUILD_POSTGRES=OFF` 添加到深入 MinSizeRel 复现命令中。** PostgreSQL 默认为 ON，因此在没有 libpq 的主机上按原样运行配置会失败。已修复。

## [0.10.0] — 2026-05-20

### 已添加

- **串行 fan-out 一次性 stderr 警告（issue #62，PR #63）。** `compile()` 的默认值为 `set_worker_count(1)` — fan-out 分支在调用方的执行器上串行执行，没有引擎拥有的线程池。这种预期行为对于仅根据文档构建多 Send 图的用户来说，看起来像是静默串行执行。首次 `NodeExecutor` 在没有池的情况下调度多 Send（或多出边）fan-out 时，向 stderr 添加了一条一次性指导消息。`std::atomic` + 比较交换保证即使在并发 fan-out 下也恰好发出一次。调用 `set_worker_count(N>=2)` 会重建 `NodeExecutor`，自然重置该标志。可通过环境变量 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`（或 `true` / `yes`）抑制 — 用于有意的 worker=1 串行执行、基准测试和 CI stderr 断言场景。由 5 个 Linux + macOS 单元测试覆盖（`test_fanout_worker_warning.py`）：触发 / 一次性 / 池选择加入静默 / 环境变量静默 / 单 Send 无警告。Windows：pytest capfd 与 wheel 二进制文件中的 MSVC CRT fd 缓存不兼容，因此模块级跳过 — wheel 二进制 stderr 输出本身是正常的。

- **拓扑 JSON Schema 导出 — `NodeFactory::export_schema()`**（issue #56，无代码可视化块编辑器的先决条件）。将引擎消费的拓扑 JSON 格式作为机器可读的 schema（JSON Schema Draft 2020-12）一次性导出：`{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`。另一个仓库的块编辑器从该 schema 自动生成其调色板 → 编辑器和引擎不会跨版本漂移。完全增量式：
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### 已修复

- **拓扑顶层容器格式验证（#126）。** `channels`/`nodes` 必须是对象；如果不是，则在所有模式下拒绝。`edges`/`conditional_edges` 数组验证在严格模式下强制执行，保留旧版键控边映射兼容性。错误记录路径和 JSON 类型，而非完整输入。
- **`max_steps` 终止状态已暴露（#114）。** 添加了 `RunResult::max_steps_exhausted()` 和只读 Python 属性 `RunResult.max_steps_exhausted`。仅当在仍有节点待执行时达到 `max_steps` 才为 True；在 gRPC 单响应和流式最终 JSON 中提供相同状态。C++ 结构体大小不变。

- **`set_worker_count` / `set_worker_count_auto` 文档字符串更正（issue #62，PR #63）。** v1.0 准备周期有意将 `compile()` worker 池默认值从 `set_worker_count(hardware_concurrency())` 恢复为 `set_worker_count(1)`（原因见 `src/core/graph_engine.cpp:69-93` 注释），但四个面向用户的文档字符串保留了旧声明 → 信任文档构建多 Send fan-out 图的用户得到了单线程上的静默串行执行。单元测试不可见（假 spawn、即时主体）；仅在真实墙钟 e2e 中暴露。
  - 重写了 `set_worker_count` / `set_worker_count_auto` 在 `bindings/python/src/bind_graph.cpp` 中的 Python 文档字符串以匹配实际行为：`compile()` 默认值为 1，`set_worker_count_auto()` / `set_worker_count(N>=2)` 是显式选择加入。
  - 相应更正了 `include/neograph/graph/engine.h` 中的两个 Doxygen 注释。Doxygen Pages 在 master 推送时自动重建。
  - 更正了 `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md` 中相同的过时声明（默认 = hardware_concurrency）。

- **补充了 v0.9.0 中缺失的三个 API 迁移。** PR `9b`（`19819d8`）在 v1.0 准备周期中破坏性地移除了 `GraphNode` 旧版 8 虚拟链，但 PR `#48`（`6e654ad`，“C++ 示例迁移到 `GraphNode::run()`”）仅迁移了 `examples/` — 以下 3 个文件被遗漏，导致 v0.9.0 以构建损坏状态发布：
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

CI 未将这些目标作为 add_executables 处理，或（Docker 构建依赖）将其隔离在单独环境中，因此合并至 master 和标签操作均通过。

**修复**：所有三个均从 `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` 模式迁移。节点逻辑不变。

**v1.0 关键卖点原生重新验证** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

在矩阵重跑期间，除了缺失的 API 迁移之外，还发现了一个独立的回归问题——`benchmarks/concurrent/Dockerfile.neograph` 根本无法构建，因为它未能跟踪 master 上 CMake 选项默认值的变化（与 v0.9.0 发布时的情况相同）。随着时间的推移，以下选项默认值从 OFF 翻转为 ON：
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      public build guidance — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

**Dockerfile 修复**：`libssl-dev` apt 添加 + 所有非核心选项均使用显式 `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` 固定。注释注明“因两次漂移事件而显式冻结”。`find_package(OpenSSL REQUIRED)` 在 CMakeLists.txt 中的条件化留作单独任务——需要验证对其他构建路径（PyPI wheel、ARM64 等）的影响。

**6 路矩阵关键结果**（并发=10000，2 个 CPU / 1 GiB）：

    | 引擎          | 模式          | wall_ms | p99_us      | 峰值_MB | 成功/错误 |
  |---|---|---|---|---|---|
    | **neograph**    | 线程池    | **16**  | **18**      | **5.1** | 10000/0 |
    | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
    | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
    | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
    | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
    | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

NG 与 LangGraph（营销对比轴）：wall **快 237 倍**，p99 **快 4134 倍**，峰值 RSS **仅为 1/12**。

**严苛场景**（并发数=10000，1 核 CPU / 512 MiB）：
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

相同的 v0.3.0 / v0.5.0 测量结果——**在破坏性 9b 之后，NeoGraph 的“10K 并发工作器、峰值 RSS 5 MB、无 OOM”卖点没有出现回归。**

## [0.9.0] — 2026-05-14 — v1.0 准备（候选 1 阶段 B + 候选 6）

两个 v1.0 单分派统一在一个周期内汇合：

  - **候选 1 阶段 B (`9b`–`9f`)** — `GraphNode` 的全部 8 个旧虚拟方法（`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`）+ `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + 全部 6 个 `PyGraphNodeOwner` 旧覆盖已移除。**破坏性变更**——弃用窗口已关闭。用户 GraphNode 子类 / 用户 Python 节点必须迁移到单一方法 `run(NodeInput)` / `def run(self, input)`。
  - **候选 6** — `Provider` 4 虚拟交叉积 → 1 虚拟 `invoke()`。仍处于添加 + 弃用阶段——旧 4 个虚拟方法保持不变且功能正常，仅可见弃用警告。该侧的阶段 B（`Provider` 旧方法移除）也在 v1.0.0 发布前关闭。

同一周期还包括 b59444f 的潜在并行回归回退（`e5ecb08`）+ 显式 fan-out 示例调用 + 3 个 CI 环境修复（httplib 宏保护 / Windows MSVC unistd.h / pybind pytest 迁移），全部属于此 [Unreleased]。

### 已添加

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 标准单分派入口点。在一个方法中同时处理非流式（`on_chunk == nullptr`）和流式（`on_chunk` 提供）场景。将之前的 4 虚拟交叉积（`complete` / `complete_async` / `complete_stream` / `complete_stream_async`）整合为一个异步流式超集。默认实现转发到 4 个旧虚拟方法，因此现有 Provider 子类无需更改即可工作。新增 6 个 ctest（`ProviderInvokeDefault`）。（PR #40）
- **`invoke()` 取消传播对等性** — 当未设置 `params.cancel_token` 且引擎线程本地作用域处于活动状态时，`current_cancel_token()` 会自动标记。等同于旧同步 `complete()` 行为（引擎内调用 `provider->invoke(params, ...)` 的节点主体自动接收运行中图的取消信号）。新增 3 个 ctest（`InvokeCancelPropagation`）。（PR #43）
### Revised translation: 变更

- **引擎中所有内部 LLM 调用均通过`invoke()`路由**——`LLMCallNode`、`IntentClassifierNode`（PR #41/#42）、`Agent::complete` / `Agent::run_stream`（PR #43）、`SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode`（PR #43）、`PlannerNode` / `ExecutorNode`（PR #44）。NeoGraph 内的 LLM 分发统一到单一接口。
- **C++ 示例迁移（2 个文件）**——`31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp` 现在使用新的`invoke()`。用户构建中无弃用警告。（PR #45）
- **`GraphEngine::compile()` 默认工作线程数恢复为 1**（`e5ecb08`）。`b59444f` 是潜在 18 天（2026-04-26 → 2026-05-13）并行微基准回归 11.8 → 283 µs（24×）的根因——通过二分法（并行 11 个工作树）精确定位提交。从 v1.0 默认=1（对 CPU 密集型顺序/并行分发最优）；对于有意的 fan-out，添加一行`engine->set_worker_count_auto()`以打开 hardware_concurrency。向 5 个受影响的 fan-out 示例（10/14/21/36 + deep_research_graph 构建器）添加了显式调用。

### 已弃用

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 全部 4 个旧版虚拟方法均带有 `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` 标记。旧版方法在弃用窗口期内按原样运行。在 v1.0.0 中移除。内部转发器以 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 包裹，因此警告仅在面向用户的覆写/调用点出现。(PR #44)

### 已删除(候选 1 阶段 B — 破坏性)

- **`GraphNode` 旧版 8 个虚拟方法** — `execute(GraphState&)` / `execute_full(...)` / 6 个变体 + `ExecuteDefaultGuard` 递归保护
  + 300 多行默认链。全部移除。`run(NodeInput)` 是唯一的纯虚拟方法。(提交 `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` 成员 + `cancel()` 钩子迭代** — `cancel.h` 仅保留 `fork()` + `cancel()` + `is_cancelled()` + `slot()`。(提交 `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 3 个访问器** — `RunContext::cancel_token` 是唯一的取消通道。`src/core/cancel.cpp` 被清空为存根（该文件本身是未来删除的候选对象）。(提交 `9e8e956`)
- **6 个 `PyGraphNodeOwner` 旧版覆写** — pybind trampoline 仅调用 `run(self, input)`。Python 用户代码自 v0.9.0 起也要求单一方法。(提交 `9e8e956`)
- **2 个过时的 pytest 文件** — `test_execute_stream_dispatch.py`（v0.3.2 仅流式回退分发验证）+ `test_streaming_only_error_
  hint.py`（execute_full_stream 优先 — 在 v1.0 中无意义）。(提交 `4392fbb`)

### 已修复

- **向 5 个 fan-out 示例添加了显式调用** — 恢复了被 `e5ecb08` 的默认工作线程数回退所掩盖的真实并行意图：`examples/10_send_command.cpp`、`examples/14_plan_executor.cpp`、`examples/21_mcp_fanout.cpp`、`examples/36_classifier_fanout.cpp`、`src/core/deep_research_graph.cpp` 的 `create_deep_research_graph()` 构建器现在调用 `set_worker_count_auto()`。验证：`classifier_fanout` 4.22 倍加速（25.2 毫秒串行 → 6.0 毫秒并行）。(提交 `99c470b`)
- **`bench_async_http` httplib 宏保护** — `bench_async_http.cpp` 包含 `<httplib.h>` 通过 `<neograph/async/conn_pool.h>` 但 `CPPHTTPLIB_OPENSSL_SUPPORT` 未定义，导致 ODR 保护拒绝。已添加 `target_compile_definitions(... PRIVATE ...)` 到 CMake 目标。(提交 `d4be42a`)
- **Windows MSVC `unistd.h` 缺失** — `test_schema_provider_extra_
  fields_temperature.cpp` 使用了仅 POSIX 的 `mkstemps` + `close`，导致 Windows 构建完全失败。将整个文件包裹在 `#ifndef _WIN32` 保护中（覆盖率由 Linux/macOS 保证）。(提交 `3c49f12`)
- **16 个 Python 测试已迁移** — wheel CI pytest 在 28 个节点类上命中了 `AttributeError`，并带有遗留的 `def execute(self, state)` 模式。已批量迁移至 `def run(self, input)`；流式节点获得了 `input.stream_cb` None 守卫。（提交 `4392fbb`）

### 迁移(用户代码)

**Provider 调用(Candidate 6 — 弃用阶段)**

新增代码:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4 个旧版虚拟覆写在废弃窗口期内继续工作，但 `-Wdeprecated-declarations` 警告在用户覆写点可见。移除发生在 v1.0.0 之前；建议在废弃窗口期内进行迁移。

**`GraphNode` 子类（候选 1 阶段 B — 破坏性）**

C++ code:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python 代码：
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**fan-out 意图（worker 数量默认值变更）**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md` 中的迁移 1/2/3 章节（run() / ctx.cancel_token / worker 数量默认值）+ Provider 章节（将在下一次文档扫描中添加）提供了逐案例的前后对比指南。

## [0.8.0] — 2026年5月13日 — DX 策略 + 下游驱动的 API 缺口已解决

将真实下游（ProjectDatePop）反馈和内部覆盖率差异所暴露的 8 个问题（#22、#25、#26、#27、#28、#34、#35 + #16 后续）打包为一次次要版本升级。新增两个公共辅助函数（`RunResult::channel<T>`、`RunContext::store`）、11 个新的离线示例、`docs/migration-v0.4-to-v1.0.md` 迁移指南，以及一个包含 5 项的 DX 包，以减少新用户在最初 30 分钟内遇到的摩擦。

### 已添加

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 用于从结果中提取通道值的一行辅助函数。两种输出形状（嵌套的 `output["channels"][name]["value"]` 标准格式 + 由 `react_graph` 等构建器添加的扁平键）均自动处理。新增 9 个 ctest。（问题 #25）
- **`RunContext::store`** — 节点主体通过一行代码到达 Store `in.ctx.store->get(ns, key)`。旧模式（捕获 `shared_ptr<Store>` 于 `NodeFactory` lambda 中）仍然有效 — 新代码只需要新的形态。3 个新的 ctest。（Issue #27）
- **`Provider::complete_stream` 非纯默认主体** — 最小 mock / 测试夹具只需覆盖 `complete()`。现有的流式原生覆盖保持不变。新增 2 个 ctest。（问题 #22）
- **`neograph::json` 数组 `.front()` / `.back()`** — nlohmann 肌肉记忆模式（`msgs.back()["content"]`）现在可以编译。新增 4 个 ctest。（问题 #26）
- **11 个新的离线示例（41-51）** — `resume_if_exists_chat`、`custom_reducer_condition`、`store_personalization`、`request_queue_backpressure`、`cancel_token`、`node_cache`、`sqlite_checkpoint`、`openinference`、`async_tool`、`minimal`。全部 rc=0，无 API 密钥 / 外部服务依赖。填补了 27/53 个 `NEOGRAPH_API` 类中先前零引用的空白。
- **`examples/51_minimal.cpp`** — 30 行入门示例，包含一个节点，无 LLM、无工具、无 mock provider。在 5 分钟内理解 NeoGraph 的运行方式。
- **`docs/migration-v0.4-to-v1.0.md`** — 逐案例的前后对比 4 个示例 + 从 `[[deprecated]]` 旧 8 虚拟链（`execute` / `execute_async` / 等）迁移到新 `run(NodeInput) ->
  awaitable<NodeOutput>` 的常见错误。同时从 `NEOGRAPH_DEPRECATED_VIRTUAL` 宏消息中链接。
- **README “常见陷阱 5” 章节** — 新用户在最初 30 分钟内遇到的五件事（`channel<T>` 用法、`in.ctx.store`、`neograph::graph::` 子命名空间、`<httplib.h>` 宏、GCC 13 协程 ICE）集中在一处。每项都有修复方法 + 相关示例/问题链接。
- **编译时 `#error` 保护（`include/neograph/api.h`）** — 当用户 TU 在 NeoGraph 头文件之前包含 `<httplib.h>` 而未定义 `CPPHTTPLIB_OPENSSL_SUPPORT` 时，编译失败并显示清晰消息 + 退出宏（`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`）。将旧的 #16 运行时 SEGV 提升为编译时失败。
- **`example_minimal` 5 个新的友好错误消息 ctest** — 对 `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel` 消息的契约锁定，在消息正文中嵌入可用名称 + 注册方法 + 故障排查链接。
- **`docs/troubleshooting.md` 4 个新条目** — Tracer 适配器 `close()` 挂起/崩溃 (#24)、GCC 13 协程 ICE (#23)、友好错误消息指南 (#22)、`RunResult::output` 形状 (#25)。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` 块** — 明确记录了适配器作者的裸指针陷阱。指出 `RecordedSpan` + 包装器分离模式是正确的方法。引用现有的 `tests/test_openinference_cpp.cpp::InMemoryTracer` + 新的 `examples/49_openinference.cpp::PrintTracer`。(Issue #24)

### 已修复

- **`SchemaProvider::build_body` 静默丢弃 `extra_fields` 当 `params.tools` 为空时。**旧代码将 `extra_fields` 的应用限制在 `if (!params.tools.empty())`内部，导致核心模式字段如 `reasoning` 和 `response_format` 在无工具调用中完全消失。修复：将其移至工具分支之外，以便始终应用。新增3个ctest。(Issue #34)
- **`temperature_path` schema 端选择退出。** 推理模型（gpt-5.x、o 系列）具有互斥的 `temperature` 和 `reasoning.effort`，但 schema 无法声明“此提供者不接受 temperature”，迫使每次调用都使用 `params.temperature = -1.0f` 哨兵值变通方案。修复：在 schema 中指定 `"temperature_path": null` 会导致 build_body 完全跳过它。4 个新的 ctest。(Issue #35)
- **友好的 RuntimeError 消息** — `ReducerRegistry::get` / `ConditionRegistry::get` / `NodeFactory::create` “未知 <thing>: foo” 和 `GraphState::write` / `apply_writes` `Write to unknown channel` 现在在消息正文中嵌入可用名称 + 注册方法 + 故障排查链接。新手仅凭消息即可确定后续步骤。
- **`SchemaProvider::complete_stream_async` HTTP/SSE 分支** 现在在长期存在的专用 `bridge_thread_` 上分发（旧：`Provider` 基础默认值每次调用都会生成一个新的 `std::thread`）。旧行为在 glibc `internal_strlen` 中触发了 SEGV，并带有冷线程本地解析器 / NSS 状态。WS 分支已经是原生 co_await，因此不受影响。在 awaiter 的执行器上进行令牌分发得以保留（PR #10 不变量）。(Issue #16)
- **`example/09_all_features.cpp`** Store 演示 — 添加了 docstring 指针，指向 `examples/43_store_personalization.cpp` 以了解节点正文读取模式。选项 2 — 选项 3（内联实时节点）将在 #27 的 `RunContext::store` 落地后一起清理。(Issue #28)

### 文档

- `RunResult::output` 的规范形状（channels 包装）及其与构建器（如 `react_graph`）添加的扁平键投影的关系已在头文件 docstring 中记录。建议使用新的辅助函数（`channel<T>` / `channel_raw` / `has_channel`）。(Issue #25)
- `RunContext::store` 字段 `@brief` 块 — 两种管道模式（推荐 `in.ctx.store` / 兼容旧工厂闭包捕获）并排提供代码示例。(Issue #27)
- 两种路径都记录在 `examples/43_store_personalization.cpp` 文件头注释中。

## [0.7.0] — 2026-05-11 — C++ OpenInference + 异步流式桥接

在一个次要版本中关闭了针对 v0.6.0 提交的四个问题。头条：`Provider::complete_stream_async` 默认值在从外部引擎协程内部等待时不再段错误（issue #4）——这是位于 NeoGraph 前面的 SSE / 流式 HTTP 后端最常见的形式。配套：v0.6.0 Python OpenInference 层的 C++ 对等实现，以便 Phoenix / Arize / Langfuse 以与渲染 Python 跟踪相同的方式渲染 C++ 驱动的跟踪（issue #9）。另外：静音了装饰性的 Python OTel 分离噪声（issue #2），并且相同的 `thread_id` 并发运行 + `schema_mutex_` × on_chunk 锁定不变量现在固定在 docstring 中（issue #6）。

### 已添加

- C++ 对等实现 `neograph_engine.openinference`（问题 #9）。新的 `neograph::observability` 模块涵盖两个部分：
  - `Tracer` / `Span` — 小型无依赖抽象接口，使 NeoGraph 本身不引入 opentelemetry-cpp。下游提供适配器包装自己的后端（OTel SDK、内存测试假件、日志记录器等）。4 个属性设置器（string、int64、double、bool — bool 特意重命名为 `set_attribute_bool`，使 `const char*` 字面量不会意外解析到它），外加用于流式 token 诊断的 `add_event`、状态和 `end()`。
  - `openinference_tracer(tracer)` — 打开一个 CHAIN 类型的根 span，返回一个 `OpenInferenceTracerSession`，其 `cb` 字段接入 `engine.run_stream()`，并为每个节点打开一个 CHAIN 类型的子 span，将 `NODE_START`/`END` 负载塞入 `input.value` / `output.value` JSON 块中，并将 `LLM_TOKEN` 事件记录为离散的 span 事件。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`，在每次`llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`）。流式重载还会追加 `complete*` 调用时附加 OpenInference LLM 类属性集（ `llm.token` 事件以及最终组装好的 `output.value`.
  - `tests/test_openinference_cpp.cpp` 中的 7 个对等测试，驱动 `InMemoryTracer` 参考适配器 — 断言根 + 每节点 CHAIN span 层级、ERROR / INTERRUPT 状态呈现、LLM_TOKEN span 事件记录、会话关闭时的掉队 span 清理、LLM 提供程序属性集、流式 token 事件以及异常状态传播。

### 已修复

- `Provider::complete_stream_async` 默认桥接不再在流的持续时间内阻塞等待协程的执行器。修复前，默认是 `co_return complete_stream(...)` 内联的，这（a）在完整的 HTTP/SSE 接收循环期间挂起了引擎的 `io_context` 工作线程——因此同一执行器上的其他节点协程停滞——并且（b）对于 `SchemaProvider`的 WebSocket Responses 分支，还通过 `run_sync` io_context，在共享提供者状态上竞争，并在从外部 `run_sync(complete_stream_ws_responses(...))`在引擎工作线程之上额外嵌套了一个新的 `GraphEngine::run_stream_async`内部调用时产生间歇性段错误。新的默认值为同步 `complete_stream`生成一个专用工作线程，将每个 token 分派回等待者的执行器（因此用户的 `on_chunk` 与等待协程单线程运行——无重入），并通过一次性 `steady_timer.cancel()`恢复协程。工作线程异常在等待者处重新抛出。 `SchemaProvider` 添加了一个原生 `complete_stream_async` 覆盖，通过直接 `co_await`处理 `complete_stream_ws_responses`. `OpenAIProvider` 来跳过 WebSocket 路径的工作线程，从而透明地受益于新的基础默认值（无 WS 路径，无特殊用例）。在 `tests/test_provider_async_default.cpp`: `StreamAsyncBridgeDoesNotBlockExecutor` 中有两个新测试： `StreamAsyncBridgeRethrowsWorkerException`（一个并发 ticker 协程在流期间推进 + 块在等待者线程上传递，而非工作线程）和

- `openinference_tracer`: 静默 `Failed to detach context` stderr 回溯，该回溯是 OTel 的 SDK 在每次关闭时发出的，当 tracer 与 `engine.run_stream_async` + `StreamMode.ALL`一起使用时。在 NODE_START 时创建的 OTel contextvars token 是从一个不同的 `asyncio.Task` 中分离的（NODE_END 回调从引擎的 continuation 触发，而非调用者的任务），因此 `Context.reset(token)` 引发了 `ValueError`；SDK 吞掉了该异常，但仍将完整的回溯信息路由到 `logger.exception`，污染了生产日志而不影响语义。修复方案在 attach 时记录（线程，任务），并在不匹配时跳过 detach，同时在 `logging.Filter` ，仅在我们的 `opentelemetry.context` 上安装一个窄范围的 `_safe_detach` 位于栈上时丢弃该消息。同步调用者和同任务异步调用者仍能在节点 span 下获得正确的 LLM-span 嵌套。（Issue #2）

---

## [0.6.0] — 2026-05-07 — OpenInference可观测性层

关闭LangSmith UX差距。NeoGraph已经发出OTel形状的跨度（因此跟踪流向任何OTel后端）；此版本添加了LLM特定的属性层，Phoenix / Arize / Langfuse使用该层将跟踪渲染为聊天气泡+令牌计数UI，而不是扁平的通用应用程序跨度列表。已针对本地Phoenix容器进行端到端验证——writer→critic图产生6跨度层次结构（CHAIN根→节点跨度→LLM跨度），模型名称、提示/响应和令牌计数在Phoenix UI中可见。

### 已添加

- `neograph_engine.openinference` 模块：
  - `openinference_tracer(tracer)` — 上下文管理器，镜像 `otel_tracer`，但用 `openinference.span.kind = "CHAIN"` 标记根和节点 span，并将节点负载放入 `input.value` / `output.value` JSON 块中。
  - `OpenInferenceProvider(inner, tracer)` — 包装任何 `Provider`。在每次 `complete()` 打开一个 `llm.complete` 子跨度，标记为 `span.kind = "LLM"`，捕获 `llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`，以及兼容 Langfuse 的 `input.value` / `output.value` 数据块。
- `bindings/python/tests/test_openinference.py` 中的 4 个测试 — InMemorySpanExporter 对属性存在性、span 层级、异常路径和节点输入/输出 JSON 块的断言。

### 已修复

- `openinference_tracer` 现在将每个节点 span 附加为 OTel *当前* 上下文（通过 `otel_context.attach`），以便节点主体内打开的子 LLM span 嵌套在其节点 span 下。没有这个，C++→Python pybind 回调边界上的 contextvar 传播会产生每次运行 3+ 个不相关的 trace_id，而不是预期的单一 trace 树。token 在 NODE_END / ERROR / INTERRUPT 时被分离，以恢复之前的当前 span。现有 `otel_tracer` 文档记录了相同的模式 — 显式附加/分离，而非 `trace.use_span(...).__enter__()`，后者在没有匹配的 `__exit__` 时不安全。

### 备注

- OpenTelemetry 仍然是一个可选依赖。导入 `neograph_engine.openinference` 仅在首次使用时，如果 `opentelemetry-api` 未安装，才会引发明确的 ImportError；不会在导入时引发。
- 对于Phoenix端到端运行：：

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

将 OTLP gRPC 导出器配置为 `http://localhost:4317` 并打开 `http://localhost:6006` 以查看追踪。模块文档字符串中包含完整代码片段。

---

## [0.5.0] — 2026-05-07 — 绑定人体工学改进：实时变更列表属性

修复了通过绑定暴露的 message / writes / sends 列表在 Python 最自然惯用变更方式下的静默无效陷阱。此前 `params.messages.append(msg)` 修改的是副本，底层 C++ 向量从未看到新条目——优雅失败（无崩溃、无警告）导致 LLM 回复质量下降。现在 `.append()` 会直接推送到实时 std::vector。

### 已添加

- `bindings/python/src/opaque_types.h` — 五种向量类型的 `PYBIND11_MAKE_OPAQUE`：`std::vector<ChatMessage>`、`<ChatTool>`、`<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector` 将每种类型注册为 Python 类（`ChatMessageList`、`ChatToolList`、`ToolCallList`、`ChannelWriteList`、`SendList`），支持针对实时 C++ 向量的完整可变序列协议。
- 每种类型的 `py::implicitly_convertible<py::list, …>` — 传统的构建后赋值模式（`params.messages = [m1, m2]`）保持原样工作；赋值会自动将 Python 列表转换为绑定类。
- `bindings/python/examples/23_evolving_chat_agent.py` — 每线程演化的聊天智能体（实时 LLM）：智能体的 JSON 定义在轮次之间根据累积的对话历史进行重写。演示了跨演化的检查点恢复（先前消息保留）、`__graph_meta__` 审计通道模式，以及验证器边界（白名单节点类型、必需通道）。

### Revised translation: 变更

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends` 现在返回其绑定类，而非普通 `list`。`len()`、迭代、`__getitem__`、`__setitem__`、`.append()`、`.extend()`、切片——所有行为都像 Python 列表。只有 `isinstance(x, list)` 返回 False。仓库及下游 grep 确认零个此类 isinstance 调用点。
- `.github/workflows/nightly.yml` — 移除 `ops/s ≥ 600K` 门控。在连续 4 次失败且使用 `err=0` 和 `leak=false` 后，阈值（根据本地硬件校准为 969K ops/s）在共享 GitHub 托管运行器上无法达到（测得 233~273K ops/s，比本地低 3-4 倍）。吞吐量回归检测位于 PR 时的 `bench-regression` 任务中（硬件稳定，单次调度在微秒级）。夜间浸泡测试的实际价值是 5 分钟内的 `err==0` + `leak_suspect==false` — 两者均保留为硬性门禁。

### 备注

- `ChatMessage.image_urls`（`std::vector<std::string>`）有意未迁移 — `vector<string>` 在绑定中使用过于广泛，无法在不扫描每个调用点的情况下进行全局 OPAQUE。已记录为剩余限制；v0.6+ 候选。

---

## [0.4.0] — 2026-05-05 — v1.0 准备：统一 `run(NodeInput)` 调度

v1.0 打磨轨道的开篇版本。8 个虚拟 `GraphNode` 交叉乘积（`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`）折叠为单一规范方法：`run(NodeInput) -> awaitable<NodeOutput>`。每次运行的取消元数据从非通道集 `GraphState` 成员 + 线程本地走私通道移入显式 `RunContext` 参数。`deadline` 和 `trace_id` 仅作为保留扩展槽添加，不由 `RunConfig` 填充。`CancelToken` 获得分层 `fork()`，因此多 Send fan-out 工作线程各自拥有私有信号，父级的 `cancel()` 会级联到该信号。

### 已添加

- `RunContext` (`include/neograph/graph/engine.h`) — 显式的每次运行元数据：可用的 `cancel_token`、`thread_id`、`step`、`stream_mode`，以及保留的 `deadline` 和 `trace_id` 槽位。引擎将其贯穿于每次 `NodeExecutor::run` 调用。**PR 1，提交 `a473f0e`。**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 单一规范分发入口点。`NodeInput { state, ctx,
  stream_cb }`；`NodeOutput { writes, command, sends }`。默认主体转发到旧版 8 个虚函数，以便现有子类保持可编译。**PR 2，提交 `607ce66`。**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 具有自身 `cancellation_signal` 的子令牌。父级 `cancel()` 级联到所有活动子级（并递归到孙级）。`run_sync(aw, parent_token)` 切换到 `parent_token->fork()`，使每个嵌套操作绑定自己的槽位 — 关闭了 v0.3.x 的 emit-vs-bind 竞争以及多 Send 单处理程序覆盖。v0.3.x 的 `add_cancel_hook` 列表通过弃用继续工作。**PR 3，提交 `897645c`。**
- `[[deprecated]]` 作用于 8 个旧版 `GraphNode` 虚拟方法 + `add_cancel_hook`。内部调用点（graph_node.cpp 默认链、默认 `run()` 转发器）由新的 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 宏（`api.h` — GCC / clang / MSVC 可移植）括起。覆盖已弃用虚拟方法的用户代码会看到迁移警告；引擎内部保持干净。**PR 4，提交 `35a4517`。**
- `engine.get_state_view(thread_id) -> StateView` 现在是规范状态读取；原始字典 `engine.get_state(...)` 在文档字符串中软弃用（不发出警告 — 原始字典仍然是有效的逃生舱）。**PR 5，提交 `f31aa53`。**
- 7 个 C++ + 19 个 Python 示例迁移到 `run(NodeInput)`。冒烟运行与 v0.3.2 输出逐位匹配。**PR 6a/6b，提交 `a2a24ef` / `0a76e3a`。**
- Pybind `PyGraphNodeOwner` 覆盖 `run(NodeInput)` 并分派到 Python 用户的 `run` 方法（如果定义），否则回退到旧版链。`RunContext` / `NodeInput` / `CancelToken` 暴露给 Python；`cancel_token` 可通过 `input.ctx.cancel_token` 访问，无需线程本地走私。**PR 7，提交 `4e186a5`。**
- `docs/reference-en.md` §6 GraphNode 折叠为单个 `run()`。在 §7 下添加了 RunContext + `fork()` 示例小节。README 的“与 LangGraph 的差异”新增了“一个节点方法”条目。**PR 8，提交 `519a00b`。**
- 内置 C++ 节点（`LLMCallNode`、`ToolDispatchNode`、`RouteToNode`）迁移到 `run(NodeInput)` 覆盖。**PR 9a，提交 `d1070dc`。**
- 新手模式陷阱修复：README CMake 片段记录了 `graph::` 子命名空间、cppdotenv 路径、`OpenAIProvider::create()` 与 `create_shared()`、`neograph::json` 作为 nlohmann 子集、3 参数与 2 参数 `compile()`。Python `compile(def, ctx, store=None)` 关键字参数已添加（增量式，非破坏性）。**提交 `ee11ed6`。**

### Revised translation: 变更

- README：“10K 工作进程实测压力测试”部分 — RTX 4070 Ti + Gemma 4 E2B Q4 在 neoclaw 上，N=10000 完成 @ 0 错误 / 424 秒 / 2572 MB 峰值 / 约 1 KB 边际工作进程成本 / p99 648 毫秒（`7840b81`）。
- README：“生产经济学”部分 — 舰队安全 + RAM 增量框架（`b82b15a`）。
- README：“无需 Docker”+“依赖漂移免疫”要点，位于 LangGraph 增量列表中（`333b482`、`a6061d7`）。

### 已弃用

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — 通过 v0.5.x 保持与 `[[deprecated]]` 注解兼容，在 v1.0 中移除。
- `CancelToken::add_cancel_hook` — 由 `fork()` 替代。相同的弃用窗口。

### 备注

- 验证：442 → 452 ctest（新增 3 个 NodeRunDispatch + 7 个 CancelTokenFork）+ 96 个 pytest + 5 个实时 LLM/WS 在 v0.4.0 标签下全部通过。
- 一个子 PR（`run(const NodeInput&)` 引用参数）在 pybind 异步路径下触发了 v0.2.0 RunConfig 协程引用 UAF 崩溃形态。修复在合并前落地：`NodeInput in` 按值传递。记录于 `node.h`。

---

## [0.3.2] — 2026-05-05 — 取消传播加固（5 轮）

五轮补丁系列，填补了 v0.3.0 单次取消所暴露的缺口：Send fan-out 传播、进程内轮询、Python 钩子、C++ 作用域、异常类型。同时落地了来自 FastAPI SSE 聊天演示评估的 TODO_v0.3.md 反馈批次 — `resume_if_exists`、字典或列表 `update_state`、用于类型化状态读取的 StateView。

### 已添加

- `RunConfig::resume_if_exists` — 选择性地恢复先前线程的检查点，无需显式调用 `resume()` 。标准多轮聊天语义： `engine.run(cfg)` 在存在 `thread_id` 时继续对话。
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 接受两种形态。修复前仅 `dict` 有效；传递列表会静默无操作。列表形式与每个节点主体的 emit 形态对称。
- `StateView`（`bindings/python/neograph_engine/state_view.py`）— Pydantic 类型化状态读取。`engine.get_state_view(thread_id) ->
  StateView` 返回扁平点访问（`view.messages` / `view.foo`）以及用于字典逃生舱口的 `view.raw`。子类化以定义类型化通道：`class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 断言飞行中取消确实在套接字层中止了每个 Send 派生的兄弟节点（这是 v0.3.1 根因补丁）。
- `examples/22_self_evolving_graph.py` — 移至 v0.3.2，并包含 TODO_v0.3.md #9 食谱折叠。
- Doxygen `/* */` 通配符修复 — `acp/types.h` 包含带有路径通配符的 `/**` 块（`fs/*`、`terminal/*`），这些通配符打开了嵌套注释并抑制了所有后续诊断。已替换为 `&#42;` HTML 实体。

### 已修复

- 取消传播，5 个累计轮次：
  1. v0.3.0 单节点 — `cancel_token` 到达 `Provider::complete`。
  2. v0.3.1 多发送指针丢弃 — fan-out 工作线程现在共享 `run_cancel_token_shared()`（此前在 `init_state +
     restore` 于通道集之外重建每工作线程状态时丢失）。
  3. v0.3.1+ 进程内轮询 — 引擎超步循环在步骤之间轮询，而不只是在 LLM I/O 时。
  4. v0.3.2 Python 钩子 — `add_cancel_hook` 在每次运行的令牌上注册回调，在 `cancel()` 时触发。允许同步 Python `execute()` 安装临时取消处理器，而无需线程局部作用域。
  5. v0.3.2 C++ 作用域 + 重试 + 异常类型化 — 在主线程上重新抛出 `NodeInterrupt`（避免 libstdc++ `__exception_ptr::_M_release` 竞态），重试预算尊重取消，运行时与逻辑异常分离。
- 仅 `execute_stream` 的 Python 节点静默落入默认 `execute` 路径（NotImplementedError）。现在当用户仅覆盖流式变体时，`run_stream` 直接连接 `execute_stream`。
- `update_state` 接受 list[ChannelWrite] — 关闭静默无操作（TODO_v0.3.md #5）。

### 备注

- 442 个 ctest + 96 个 pytest + 2 个实时 LLM（单个 + fanout 取消）在 v0.3.2 标签处通过（`915e90e`）。
- 27/30 个 C++ 示例 + 20/22 个 Python 示例在 `examples/run_all.py` 下通过。跳过的测试需要外部服务（Postgres / Crawl4AI / 实时 OpenAI）。
- Valgrind 6 个示例 0 错误，815 次分配 / 815 次释放，干净。
- 基准中位数 5.185 µs/迭代（seq 路径，v0.3.0 基线）— 整轮中零性能回归。

---

## [0.3.0] — 2026-05-04 — 协作式取消传播

关闭 FastAPI SSE 聊天演示评估期间报告的生产成本泄漏缺口：前端 `AbortController` 取消 asyncio 任务不再让上游 OpenAI 请求运行至完成。取消传播贯穿运行的每一层。

### 已添加

- `neograph::graph::CancelToken`（原子标志 + asio `cancellation_signal`）和 `CancelledException` — `include/neograph/graph/cancel.h`。协作式取消原语。通过 `RunConfig::cancel_token` 传递（可选 `shared_ptr`）；引擎超级步骤循环在步骤之间轮询 `is_cancelled()`，并以 `CancelledException` 退出。令牌的 `cancellation_slot()` 绑定到运行的 `co_spawn`，因此进行中的 LLM HTTP 套接字操作在线上被中止（asio `operation_aborted`）。
- `CompletionParams::cancel_token` — 供用户在多个 `provider.complete()` 调用之间传递中止的显式固定项。 `Provider::complete` 读取它（或回退到由 `current_cancel_token()` ），并将该槽位绑定到其内部 `PyGraphNode::execute_full_async`设置的线程本地 `run_sync` io_context，因此即使被取消命中的同步 Python 节点也会停止计费。
- `GraphState::run_cancel_token()` — 每次运行时的非序列化句柄，由 pybind `PyGraphNode` 用于安装一个 `CurrentCancelTokenScope` 包裹同步 Python `execute()` 调用。这正是让同步 Python 用户无需修改节点代码即可获得透明取消传播的机制。
- pybind `engine.run_async` / `run_stream_async`：asyncio `Future.cancel()` 现在通过 `add_done_callback` 连接到 `CancelToken::cancel()`，并且 `co_spawn` 绑定令牌的取消槽。
- pybind 安全解析辅助函数 `_safe_set_future_result` / `_safe_set_future_exception` — 防护 `future.set_result` / `set_exception` 通过 `call_soon_threadsafe` 发布的调用，防止取消的未来 `InvalidStateError` 风暴。
- `bindings/python/tests/test_async_cancel_live_llm.py` — 实时 OpenAI E2E 测试，断言 OpenAI HTTP 在 `Future.cancel()` 后 3 秒内完成（实际为即时完成；修复前约为 7–8 秒的未取消流式传输）。除非设置 `NEOGRAPH_LIVE_LLM=1`，否则跳过。
- `examples/22_self_evolving_graph.py` — 自演化图 PoC：`prompted_llm` 节点从 JSON 配置读取自身提示词，使 LLM 重写器可在运行之间修改图定义并重新编译。演示 `0.0 → 0.4` 分数提升；记录了重写器中的通道流推理缺口。

### Revised translation: 变更

- `Provider::complete(params)` 现在将其内部取消槽绑定到其 `run_sync` 当 `params.cancel_token` 被设置，或当线程本地 `current_cancel_token()` 处于活动状态时。对于未选择加入的调用者，保留之前的默认行为（无取消）。
- `neograph::async::run_sync` 增加了可选的 `graph::CancelToken*` 参数；当非空时，绑定的 spawn 会绑定该令牌的槽。
- pybind `resolve_future_async` 通过安全解析辅助函数路由，而不是调用 `future.set_result` 直接通过 `call_soon_threadsafe`.

### 路线图（推迟至 v0.3.x — 参见 `TODO_v0.3.md`）

- 在同一 `thread_id` 上实现 LangGraph 风格的自动检查点恢复。
- `run_async` 错误消息中的仅流式节点提示。
- `cb.emit_token(node, data)` 易用辅助函数。
- README“与 LangGraph 的差异”部分。
- `update_state` 签名与文档对齐。
- `get_state` 扁平辅助函数 / Pydantic 访问器。
- 在`run_parallel_async`和`run_sends_async`分支fan-out中对取消传播进行实时验证。
- pgvector RAG 示例。

---

## [未发布] — 第 4 阶段

阶段4关闭了异步路径上的最后一个`run_sync`跳点。`run_async`现在端到端地停留在调用方的执行器上：在`io_context`线程上的三个50毫秒智能体在`examples/27_async_concurrent_runs`中从约150毫秒（串行）降至约50毫秒（重叠）。

### 破坏性

- **`GraphNode::execute_full_async` 默认已切换为异步优先。** 它现在将 `co_await execute_async(state)` 包装到 `NodeResult` 中，而不是调用同步的 `execute_full(state)`。任何仅从同步的 `Command`/`Send` 覆盖中发出 `execute_full` 的子类，都必须添加一行 `execute_full_async` 桥接：
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
没有桥接，`Command`/`Send`会在异步路径上被静默丢弃——这是2.0的潜在调度缺陷，3.0通过路由到同步路径来修复，代价是每个超级步骤产生一次`io_context`生成。所有树内子类（`deep_research_graph`、示例10/14/21、测试5处）现在都带有桥接。

### 性能

- 示例27的墙钟时间：**152毫秒 → 53毫秒**（在`io_context`线程上的3个智能体×50毫秒定时器步骤，完全重叠）。
- 在单次运行基准测试中没有可测量的性能回退； `run()` 仍然通过一个新的单线程 `io_context` 经由 `run_sync` 驱动同一个协程。

### 测试

- 115/115 tests pass
- 295/295 ASan+UBSan 通过
- Valgrind 在协程重度子集上干净（20 个测试，2.4 s）

### 发布后验证（当日）

- **所有30个示例已重新运行：** 26/29通过，0失败，3个受环境限制（clay_chatbot → raylib，postgres_react_hitl → docker compose，deep_research完整循环 → crawl4ai服务）。`21_mcp_fanout`测量为3次MCP调用/8毫秒墙钟——阶段4的重叠在真实网络I/O下保持。

- **ARM64兼容性（docker buildx --platform linux/arm64）：** 仓库根目录下的`Dockerfile.arm64-smoke`。ubuntu:24.04-arm64 + core+llm+async+sqlite+tests在QEMU仿真下构建完成约需15分钟；**306/306 ctest在ARM64上通过**。剥离后的二进制大小为0.81-0.88 MB（与x86_64几乎相同）。示例27在仿真下运行时间为65毫秒（原生x86_64：53毫秒）。确认Linux/ARM64与macOS beta（Apple Silicon）一起作为受支持的平台。

- **缓存局部性（Ryzen 5800X / Zen 3，Valgrind cachegrind，32 KB L1i/d 8路，32 MB L3 16路）：** `bench_concurrent_neograph`扫描N=1 → 10,000。

    | N | I refs | LLi 未命中 | LLi 未命中率 | 原生 p50 |
  |---:|---:|---:|---:|---:|
    | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
    | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
    | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

末级指令未命中在N的4个数量级范围内保持平稳，约4,320次。唯一热代码工作集≈277 KB（L3的0.85%）。N=10,000时648 M条指令仅产生4,329次LL未命中——大约每150,000条指令1次未命中。本地p50纯粹因I-cache预热从17 µs降至5 µs。"突发并发鲁棒性"定位的首个实测证据。

---

## [3.0.0] — 2026-04-22

3.0 移除了 Taskflow 依赖，并在单一的 asio 协程路径上统一了同步和异步超步执行。图定义 JSON、节点 ABI、检查点模式以及公共入口点（`run`, `run_async`, `run_stream`, `resume`）与 2.0 保持源代码兼容；破坏性变更仅限于 `GraphNode` 子类，这些子类仅从 **sync** `Command`/`Send` 。 `execute_full` 覆盖中发出

### 破坏性

- **`deps/taskflow/`和Taskflow INTERFACE目标已移除。** 同步超级步循环、`run_one`、`run_parallel`、`run_sends`以及进程范围的`tf::Executor`静态变量已被删除。通过NeoGraph的include路径`#include <taskflow/...>`的下游消费者必须单独引入Taskflow。
- **`GraphNode::execute_full_async`默认现在通过直接调用桥接到同步`execute_full`（无`co_await execute_async`）。** 这保留了从仅同步覆盖中发出的`Command`/`Send`——这是2.0的常见模式——通过所有入口点现在共享的异步路径。需要非阻塞I/O和`Command`/`Send`的异步原生节点必须直接覆盖`execute_full_async`；文档字符串自2.0以来一直如此说明，但2.0从未实际执行过，因为同步`run()`完全绕过了协程路径。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 同步方法已移除。** 请使用 `_async` 对等节点。
- **CPU 并行 fan-out 为可选启用。** 此前 Taskflow 默认提供进程级线程池。在 3.0 中，`run_parallel_async` 和 `run_sends_async` 的多 Send 分支会根据驱动协程的执行器进行分支调度——由同步 `run()` 启动的单线程 io_context，或调用方自己的执行器（用于 `run_async()`）。I/O 密集型 fan-out 仍可重叠（单线程上的 co_await 挂起）；CPU 密集型 fan-out 会串行化，除非调用方为 `run_async()` 使用多线程执行器，或通过 `engine->set_worker_count(N)` 选择启用引擎拥有的线程池。

### 已添加

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 与现有单线程 `run_sync` 并行的 N 工作线程同步↔异步桥接。为调用启动一个新的 `asio::thread_pool`，以便内部 `make_parallel_group` 分支在独立工作线程上执行。
- `GraphEngine::set_worker_count(n)` — 可选启用的引擎拥有线程池，由 `NodeExecutor` 用于并行 fan-out 调度。重建执行器；必须在任何并发运行之前调用。

### Revised translation: 变更

- `GraphEngine::execute_graph` (sync) 已移除。所有入口点（`run`, `run_stream`, `resume`）均通过 `execute_graph_async` 经由 `neograph::async::run_sync` 路由，因此超级步循环、重试退避、检查点 I/O 和并行 fan-out 现在端到端地存在于同一条协程路径上。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` 从 `tf::Executor` / `tf::Taskflow` 切换为 `asio::thread_pool` + `asio::post` 用于调用方侧驱动。

### 性能（Release 模式基准测试 -O3 -DNDEBUG，Linux 参考环境，10 次运行中位数）

- `seq` 引擎开销（3 节点链，计数器）：**每次调用约 5.0 µs**。
- `par` 引擎开销（5 工作线程 fan-out + 汇总器）：**每次调用约 11.8 µs**。
- 整个基准测试过程（预热 + 顺序 + 并行迭代）的峰值 RSS：**4.8 MB**。
- 与 LangGraph 1.1.9 在相同工作负载下的对比：**顺序执行快 131 倍，并行执行快 199 倍**（每次迭代）；RSS 约轻 12 倍。

本 CHANGELOG 的先前草稿将“约 46 µs 顺序 / 约 114 µs 并行”列为 3.0 回归。这些数字来自 `CMAKE_BUILD_TYPE` 未设置的构建树，因此基准二进制文件在未启用 `-O3 -DNDEBUG` 的情况下编译。在正确的 Release 构建中，异步对等节点折叠相比 2.0 的 Taskflow 同步路径是**改进**（2.0 README 在同一主机上宣称 20.65 µs 顺序 / 150.7 µs 并行）。修正后的图表见 [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png)。

### 迁移

- 如果您的节点覆盖 `execute()` / `execute_async()` 且不发出 `Command` / `Send`，则无需操作。
- 如果您覆盖同步 `execute_full` 以发出 `Command` / `Send`：无需更改——3.0 异步路径默认现在直接调用您的同步覆盖。`Command.goto_node` 路由通过同步和异步入口点均可工作。
- 如果您覆盖 `execute_async`（异步原生 I/O）且需要 `Command` / `Send`：请直接覆盖 `execute_full_async` 并在其中组装 `NodeResult`。仅覆盖 `execute_async` 会静默丢弃 `Command` / `Send`，因为默认的 `execute_full_async` 现在通过同步 `execute_full` 路由，而非异步 `execute_async`。
- 如果您依赖 Taskflow 的进程级池通过 `engine->run()`进行 CPU 并行 fan-out：请在 compile() 之后调用 `engine->set_worker_count(N)` 一次，或通过 `run_async()` 在您自己的多线程 `asio::thread_pool` / io_context 上驱动引擎。

---

## [2.0.0] — 2026-04-22

首个包含 Stage 3 异步 API 的公开版本。这是一个破坏性版本；以下变更影响编译（C++ 标准）和 ABI（抽象基类新增了异步对等接口）。同步调用点逐位保留，因此**未覆盖 `Provider` / `CheckpointStore` / `GraphNode` / `Tool` 的应用程序代码无需修改即可继续工作**。

### 破坏性

- **需要 C++20。** 公共 API 暴露了 `asio::awaitable<T>` 返回类型，这些类型需要 `std::coroutine` 支持。使用者必须使用 `-std=c++20` （或更高版本）进行编译。已测试 GCC 13+、Clang 15+；参见 `docs/ASYNC_GUIDE.md` §4.1 了解 GCC 13 协程的解决方法。
- **已移除 libpqxx 依赖。** `neograph::postgres` 现在直接链接 libpq。Ubuntu 24.04 用户不再遇到 libpqxx-7.8t64 的 C++17/C++20 ABI 分裂所引入的 `pqxx::argument_error::argument_error(..., std::source_location)` 链接错误。CMake 查找现在针对 `PostgreSQL::PostgreSQL`（CMake 自带的 FindPostgreSQL）。仅安装了 `libpqxx-dev` 的使用者现在还必须安装/保留 `libpq-dev`。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` 的 ABI 已扩展。** 每个都增加了异步对等虚函数（`complete_async`、`save_async`、`execute_async`、`rpc_call_async` 及其变体）。下游子类需针对 2.0 头文件重新编译；除非子类希望提供原生异步覆盖（任何执行真实 I/O 的实现者都建议这样做），否则源码无需更改。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread` 不再是纯虚函数。** 它们现在有默认实现，通过 `_async` 桥接到匹配的 `neograph::async::run_sync`对端。覆盖同步侧的子类继续正常工作；未提供任何覆盖的子类（以前会导致编译错误）现在会无限递归——约定：每个同步/异步对至少覆盖一个。

### 已添加

- **异步 API** 覆盖所有 I/O 层（完整参考见 `docs/ASYNC_GUIDE.md`）：
  - 基类和所有内置提供者（OpenAI、Schema、RateLimited）上的 `Provider::complete_async`。
  - HTTP 和 stdio 传输的 `MCPClient::rpc_call_async`。stdio 使用 `asio::posix::stream_descriptor`。
  - 所有八个同步方法的 `CheckpointStore::*_async`。
  - `GraphNode::execute_async` + stream / full / full_stream 变体，带有异步原生交叉默认值。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async` 驱动 `execute_graph_async`——一个端到端的协程超级步骤循环，包括通过 `asio::experimental::make_parallel_group` 的并行 fan-out。
  - `neograph::AsyncTool` 适配器，用于希望使用协程体同时保留同步 `Tool` 接口的用户工具。
- **`neograph::async` 命名空间** — HTTP 客户端、连接池、SSE 解析器、run_sync 桥、URL 端点拆分器。参见 `include/neograph/async/*.h`。
- **新示例**:
  - `examples/27_async_concurrent_runs.cpp` — 单个 `io_context` 上的多个智能体。
  - `examples/05_parallel_fanout.cpp`（重写）— 使用 `run_parallel_async` 在单次图运行内进行异步 fan-out。
- **CI 基准回归门禁**（`.github/workflows/ci.yml`）— PR 检查对 `bench_async_http` / `bench_async_fanout` / `bench_neograph` 强制设置下限。

### 性能

在 feat/async-api 分支上对照 Stage 2 同步基线测得:

- `bench_async_http --mode async_pool --concur 1000`：6064 ops/s → **17834 ops/s**（2.9×）。
- `bench_async_fanout --concur 50000`：线程每智能体不可实现 → **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs`（3 × 50ms 异步工作）：150ms（同步）→ **50ms**（1 个 io_context 线程）。
- `examples/05_parallel_fanout`（3 × 100-150ms 异步工作）：370ms（顺序）→ **150ms**（1 个 io_context 线程）。
- `bench_neograph` 引擎开销：不变（约 30 µs 顺序 / 约 205 µs 并行）。协程机制不会使热路径退化。

### 尚未包含在 2.0.0 中

- **Taskflow 依赖**保留。同步 `engine.run()` 路径仍使用它进行 fan-out；Sem 4.5 重新审视同步路径是否可以被 `run_sync(*_async)` 替换，以便完全移除该依赖。

### 跨平台

2.0.0 中支持三个平台，处于不同的稳定性层级。该层级反映的是平台在发布前经历的真实世界验证程度 — 而非功能覆盖范围（代码库通过 `#ifdef _WIN32` 拆分实现单一来源；一旦测试通过，各平台功能等同）。

#### Linux — **GA**（生产就绪）

* Ubuntu 24.04，GCC 13。
* 本地全部 332/332 ctest 通过（Postgres 通过 docker `postgres:16-alpine`），且所有基准测试均处于已提交的 CI 下限之内。
* MCP stdio 基于 fork/pipe/execvp + `asio::posix::stream_descriptor`。
* Postgres 异步对端基于 libpq 非阻塞模式 + `asio::posix::stream_
  descriptor` 包装 `PQsocket`。
* 上文引用的每个性能数字的参考平台。

#### macOS — **测试版**

* macos-latest（Apple Silicon），通过 Xcode 使用 Clang。
* CI 构建并运行非 Postgres 测试；Postgres 集成用例在无服务容器时自行跳过。POSIX 路径（同一 fork/ pipe + asio::posix 代码）会被执行。
* `CoreFoundation` + `Security` 框架通过 httplib 链接，用于 TLS 上的系统证书加载。
* 建议视为测试版，直至2-4周的持续集成运行及用户报告确认无运行时行为差异（协程调度、SIGPIPE/EPIPE 形态、管道缓冲区大小）。若这些运行无异常，则定向推广至正式版(GA)。

#### Windows — **内部版**

* windows-latest, MSVC 19.44 (VS 2022), x64。
* CI 范围：**仅 core + async + MCP + LLM**。Windows CI 任务中禁用 Postgres 和 SQLite 后端，因为 vcpkg 每次运行都会从源码编译 OpenSSL / libpq / zlib / lz4（约 20 分钟，自 `x-gha` 被移除后上游没有可用的二进制缓存后端）。Windows 用户通过自己的 vcpkg / choco 配置在本地编译这些组件。
* OpenSSL 通过运行器预装的 choco 包（`C:/Program Files/OpenSSL-Win64/`）。httplib + asio::ssl 中的 TLS 路径可编译并链接。
* MCP stdio：`CreateProcess` + 命名管道（FILE_FLAG_OVERLAPPED）+ `asio::windows::stream_handle`。重叠管道路径依据 MSDN 规范编写，未经本地 Windows 验证；预计首批用户会暴露边界情况（ERROR_IO_PENDING 处理、大型 JSON 响应上的管道缓冲区边界）。
* Postgres 异步对端（本地启用时）：`asio::ip::tcp::
  socket::assign` 包装 `PQsocket` 返回的 SOCKET（通过 `native_handle_type` 转换以保留 64 位 SOCKET 值）。Windows CI 不执行此路径——仅限本地。
* 协程机制位于 MSVC 的 `<coroutine>` 中；按规范行为应与 GCC/Clang 一致，但 `examples/27` 跨运行重叠测量尚未在 Windows 上确认。
* 在 2.0.0 之前视为 **alpha**。一旦一个生产用户运行多智能体工作负载一周而未遇到 stdio/管道或协程调度器问题，并且 Postgres 异步对等端由愿意运行 vcpkg 完整 libpq 构建的用户本地验证，则提升为 beta。

> **模式**：CI 绿色是底线，而非上限。第 3 层运行时
> 行为差异（协程调度时序、管道缓冲区
> 边界、套接字接管语义）仅在真实
> 工作负载下才会显现。上述层级语言让用户对每个平台
> 有正确的预期，而不是假装三者
> 在第一天就可互换。

### 提升后修复

- **`async::HttpResponse` 头映射** — 响应面现在暴露一个 `headers` 的 `(name, value)` 对向量，保留线上顺序和原始大小写，外加 `get_header(name)` 作为不区分大小写的访问器。Retry-After 和 Location 仍作为专用字段保留以保持向后兼容。解除了下面 MCP 会话跟踪修复的阻塞。
- **MCP `Mcp-Session-Id` 头跟踪** — Sem 2.6 httplib→async_post 迁移静默丢弃了此功能。现在每个 post-initialize RPC 都通过新的头访问器回显服务器分配的会话 ID，因此服务器的会话状态保持可路由。
- **MCP stdio 可等待互斥锁** — `StdioSession::rpc_call_async` 使用 `std::mutex`，当同一单线程 io_context 上的两个协程调用同一会话时会发生死锁（第二个的 `lock_guard` 阻塞了第一个所需的 worker）。已替换为 `asio::experimental::channel<void(error_code)>` 容量为 1 的信号量，使第二个获取者协作式挂起。
- **`PostgresCheckpointStore` 异步对等** — 全部八个 CheckpointStore 异步方法（`save_async`、`load_latest_async`、`load_by_id_async`、`list_async`、`delete_thread_async`、`put_writes_async`、`get_writes_async`、`clear_writes_async`）现在均为真正的异步。内部实现：`PQsetnonblocking(1)` + `PQsendQueryParams` + `asio::posix::stream_descriptor` 基于 `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`。在 4 个槽位的池上并发调用四次 `save_async`，现在会在线缆层面并行执行提交 fsync，而不是通过 `run_sync` 串行化。

---

## [0.1.0] — 2026-04 之前

预发布开发版本。不提供公共 API 稳定性保证。
