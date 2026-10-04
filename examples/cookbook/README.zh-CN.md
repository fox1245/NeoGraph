<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=8261e54a3cabee1f0a4290735688fedd7911b77b3720ca60c5096cbf35a23437 -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 类型化 C++ 迁移状态

当前 C++ recipe 使用五个类型化 SDK family 的拥有所有权的 `ProviderRequest`、
有序 `sp::Event` 和不可变 `std::shared_ptr<const sp::Outcome>`（`Completion`/`Failure`）。
`ChatMessage`/`ChatTool` 是 portable 投影，不是 native replay 权限。
只 prepare 一次；durable 调用者将 claim/receipt 绑定到
`Provider::request_digest(prepared)`，并 dispatch 同一个 handle。
local wait 结束不证明 remote model 已停止或不会计费。durable receipt 阻止自动 redispatch，但不保证 external effect 的 exactly-once。
不要从打印的 JSON 重建 history，也不要把 failure 简化为最终文本。

canonical persistence 使用 `provider-message-v2`/`runtime-history-record-v2`；
portable 摘要不能替代 native record。optional control 由调用者选择，不暗中 clamp。
bounded call 需要真实 model fact；缺失为 `LimitUnknown`。
reservation/charged/held 与 nullable provider usage 独立，不更新 budget，也不是价格、forecast 或 invoice。

header-only `examples/provider_example_support.h` 使用真实 SDK runtime。
包括 Core-only 在内的所有 native 构建都需要 `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)` 的 `SchemaProvider::runtime`，
或显式 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`，或 immutable public SDK archive fallback。offline installed/source SDK 可用 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` 禁止 fetching。
安装 include root 是 `include/SchemaProvider`；执行 interface/capability 检查，
SDK `0.1.0` alpha 的 interface/shared ABI 为4。

保留的 interface-3 model-free 执行覆盖 Assembly 四个真实本地 A2A member 服务器与 C++ speaker、
JARVIS CLI synthetic turn 和记忆持久化、Beast strict Core 编译·演化·checkpoint 回滚、
专用mock topology load与retrieval index复用·admission、ProgramChat浏览器tenant隔离·
generation替换及SQLite六个、PostgreSQL六个black-box场景。
下表将这些历史运行与包括已安装 Python application、A2A peer 和 Forge helper 的
[当前 interface-4 local 证据](../README.md#typed-c-cutover-status)分开。
vendor inference、语音、全部 Beast live 变体或专用 live multitenant1,000/32 load 仍未验证。
历史测量不是新迁移的 qualification。live 运行需要密钥、网络和模型访问并产生费用。
密钥、prompt、artifact 必须保持私密；envelope/native inspection 输出含敏感内容，不得进入公开 log。
native archive 是经过认证的 owner-private custody，不是加密或 vendor issuer 认证。


这些 recipe 组合多个 NeoGraph 功能。C++ target 需从提供必需 SDK 包的 NeoGraph 树构建。仅复制文件夹不能提供 standalone 构建。

| Cookbook | 它所展示的内容 |
|---|---|
| [`the-beast/`](the-beast/) | **一个基于自演化的智能体：生成 · 演化 · 回滚。** The Beast 编写严格的 Core JSON，在执行前进行语义验证，使用`evolve()`演化其有界 Core 拓扑，并通过检查点回滚。live（活体）、apex、forge、script 和 arithmetic-evolution 变体保留了相同的 compiler/validation 边界； JavaScript 或受信任的 C++ 负责源码编写，而严格的 Core JSON 仍作为数据交换格式。 |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A：4名国民议会议员（每个代表都拥有自己的 A2A 端点）+ 一名议长，并行广播一项法案并集中计票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | 类型化 Python provider request 与真实 prepared handle；兼容性和运行证据见对应 recipe。 |
| [`jarvis/`](jarvis/) | 语音驱动元编排器。可选本地 ASR/TTS 结合 chat/direct/delegate/parallel 四路 router、MCP tools、A2A specialists 和会话 memory。本地/mock 无需云端；live 推理使用 OpenRouter。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP客户端往返通信，**无需LLM、无需API密钥、无需fastmcp**：一个约60行的stdlib stdio服务器 + 一个C++测试框架，它执行`initialize` → `tools/list` → `tools/call`。这表明NeoGraph的MCP客户端只需要一个说线路协议的进程——对等体可以是任何东西。 |
| [`openrouter-provider/`](openrouter-provider/) | 类型化 Python provider request 与真实 prepared handle；兼容性和运行证据见对应 recipe。 |

每份cookbook还记录了它暴露的摩擦点——对于寻找公共API的粗糙边缘很有用。

## 完整 recipe 清单与证据范围

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 保留的 interface-3 四服务器/C++ session；当前已安装 Python Speaker 已通过 local A2A 0.3/1.0 peer 执行；synthetic abstention 不是模型判断 |
| [`byo-openai/`](byo-openai/) | 类型化 Python 源码迁移；历史测量不验证迁移后的实现 |
| [`jarvis/`](jarvis/) | 保留的 interface-3 CLI synthetic turn、记忆持久化与正常 EOF；语音与 pybind benchmark 未重新验证 |
| [`minimal-mcp/`](minimal-mcp/) | 已验证真实stdio handshake/discovery及计算·UTC·demo-weather调用；没有LLM |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 专用mock1,000请求、error0、compiled topology3、cache hit997；isolated host只输出reference metadata；live1,000/32未验证 |
| [`openrouter-provider/`](openrouter-provider/) | 类型化 Python 源码迁移；历史测量不验证迁移后的实现 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 已验证真实浏览器tenant隔离·generation替换、SQLite六个与PostgreSQL六个black-box及显式host model policy；无vendor inference |
| [`the-beast/`](the-beast/) | 类型化 C++ 迁移；已执行真实 strict Core 编译·演化·checkpoint 回滚；不声称所有 live 变体通过 |
| [`topology-retrieval/`](topology-retrieval/) | 已验证mock Python ranking/index复用及真实C++ registry admission/migration·unknown-key拒绝；外部pointer不是权限 |

Python MCP server、Jarvis CLI/REPL driver 和 retrieval HTTP client 是 protocol client，不是 provider binding 实现。Assembly Python speaker 使用 A2A binding，Jarvis pybind benchmark 使用已迁移 native binding。其运行证据与上方 C++ 运行分开。SDK runtime 当前仅在 Linux/POSIX 上获得验证，不声称已验证 macOS/Windows transport。live multitenant 1,000/32 不是 smoke。

## Interface-4 控制与保留的调用限制

`ProviderControls` 保留各 family 的 reasoning、sampling 和 tool selection。Chat 提供 admitted
OpenRouter reasoning object、usage/include 控制及备用 model；Responses 提供显式服务端保存
continuation、verbosity、truncation 和 include 选择；Messages 提供 manual/adaptive/disabled
thinking、effort、cache 控制及 tool choice；Gemini 提供显式 portable foreign history、
thinking level、safety setting、sampling 及 tool choice。不支持的 family/origin/model 在 I/O 前拒绝。
native message 仍需真实 custody；输出 JSON、cursor、portable 投影不能创建它。
准确类型见 [provider reference](../../docs/reference-en.md)。

Forge 请求 low reasoning effort，每次 ask 固定300秒 deadline。仅已完成的空 `MaxTokens` 响应
没有 text 和有效/无效 client call 时，才以当前输出 cap 的两倍额外调用一次。保留两个 outcome
和 usage；failure、observer/settlement 错误不重试。旧 live chatbot 路径
`multi_tenant_chatbot/server_live_llm.cpp` 和 `self_evolving_chatbot/server_multi.cpp` 使用180秒
provider timeout，不改变 ProgramChat 的独立 CLI 限制或共享 factory 默认值。

[编号示例契约](../README.md#retained-example-contracts) 说明 Deep Research 的有限 cap ladder
与 deadline discovery、示例16/28、fork/new-turn、计算的 replay 节省数、evolution file 模式及
A2A 0.x/1.0 snapshot。这些源码更新不会把上面的历史运行改称 interface-4 或 live-provider 通过。
