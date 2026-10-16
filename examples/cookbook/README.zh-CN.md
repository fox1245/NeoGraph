<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=b202d3a6beaacd5b85d1b7f6b98d90923a2029fee8aae4c6c83a52aa9bd8ca65 -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 类型化 C++ 迁移状态

当前 C++ recipe 使用五个类型化 SDK family 的自有 `ProviderRequest`、
有序 `sp::Event` 和不可变 `std::shared_ptr<const sp::Outcome>`（`Completion`/`Failure`）。
`ChatMessage`/`ChatTool` 是 portable 投影，不是 native 重放权限。
只 prepare 一次；durable 调用者将 claim/receipt 绑定到
`Provider::request_digest(prepared)`，并 dispatch 同一个 handle。
local wait 结束不证明 remote model 已停止或不会计费。durable receipt 阻止自动 redispatch，但不保证 external effect 的 exactly-once。
不要从打印的 JSON 重建 history，也不要把 failure 简化为最终文本。

canonical persistence 使用 `provider-message-v2`/`runtime-history-record-v2`；
portable 摘要不能替代 native record。optional control 由调用者选择，不暗中 clamp。
bounded call 需要真实 model fact；缺失为 `LimitUnknown`。
reservation/charged/held 与 nullable provider usage 独立，不更新 budget，也不是价格、forecast 或 invoice。

header-only `examples/provider_example_support.h` 使用真实 SDK runtime。
包括仅 Core 构建在内的所有 native 构建都需要 `SchemaProvider::runtime`。CMake 使用已安装的 package、
显式的 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`，或不可变的公开 SDK archive fallback。
使用离线的已安装/源码 SDK 时，可用 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` 禁止 fetching。
安装 include root 是 `include/SchemaProvider`；执行 interface/capability 检查，
SDK `0.3.2` alpha 的 interface revision/shared ABI 为 6。

保留的 interface-3 model-free 执行覆盖 Assembly 四个真实本地 A2A member 服务器与 C++ speaker、
JARVIS CLI synthetic turn 和记忆持久化、Beast strict Core 编译·演化·检查点回滚、
专用 mock 拓扑加载与 retrieval index 复用·准入(admission)、ProgramChat 浏览器 tenant 隔离·
generation 替换及 SQLite 六个、PostgreSQL 六个 black-box 场景。
下表将这些历史运行与包括已安装 Python application、A2A peer 和 Forge helper 的
[较早的 interface-4 local 证据](../README.md#typed-c-cutover-status)分开。
vendor inference、语音、全部 Beast live 变体和专用 live multitenant 1,000/32 load 仍未验证。
历史测量不是新迁移的 qualification。live 运行需要密钥、网络和模型访问并产生费用。
密钥、prompt、artifact 必须保持私密；envelope/native inspection 输出含敏感内容，不得进入公开 log。
native archive 是经过认证的 owner-private custody，不是加密或 vendor issuer 认证。


这些 recipe 组合多个 NeoGraph 功能。C++ target 需从提供必需 SDK 包的 NeoGraph 树构建。只复制 recipe 文件夹，得不到独立构建。

| Cookbook | 展示内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自演化智能体：生成 · 演化 · 回滚。** The Beast 编写严格的 Core JSON，在执行前先验证，用 `evolve()` 演化其有界 Core 拓扑，并通过检查点回滚。live、apex、forge、script 和 arithmetic-evolution 变体保留了相同的 compiler/validation 边界；源码由 JavaScript 或受信任的 C++ 编写，严格的 Core JSON 仍是交换数据。 |
| [`ai-assembly/`](ai-assembly/) | 多角色 A2A：4 名国民议会议员（各有自己的 A2A 端点）+ 一名议长（Speaker），并行广播一项议案并统计投票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | Python provider 的 prepare，走真实的类型化 SDK 路径；兼容性和运行证据见对应 recipe。 |
| [`jarvis/`](jarvis/) | 语音驱动元编排器。可选本地 ASR/TTS 结合 chat/direct/delegate/parallel 四路 router、MCP tools、A2A specialists 和会话 memory。本地/mock 无需云端；live 推理使用 OpenRouter。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP 客户端往返通信，**无需模型、无需 API 密钥**：一个约 60 行的标准库 stdio 服务器 + 一个 C++ 测试框架，它执行 `initialize` → `tools/list` → `tools/call`。这表明 NeoGraph 的 MCP 客户端只需要一个会说 wire 协议的进程，对端可以是任何东西。 |
| [`openrouter-provider/`](openrouter-provider/) | 类型化的 Python OpenRouter provider 请求与不可变 outcome；兼容性和运行证据见对应 recipe。 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 一个进程通过编译缓存为许多客户的拓扑提供服务。模拟二进制什么都不需要，实时版本需要 key。 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 按客户选择并演化拓扑的聊天服务器。ProgramChat 有 `--mock` 模式，Core 服务器需要 key。 |
| [`topology-retrieval/`](topology-retrieval/) | 为示例 64 提供输入的候选选择（稠密、BM25、排名融合、重排）。`--mock` 需要 `pip install faiss-cpu numpy`。 |

每份 cookbook 还记录了它暴露的摩擦点，有助于找出公共 API 的粗糙之处。

## 完整 recipe 清单与证据范围

| Recipe | 状态 |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 保留的 interface-3 四服务器/C++ session；当前已安装 Python Speaker 已通过 local A2A 0.3/1.0 peer 执行；synthetic abstention 不是模型判断 |
| [`byo-openai/`](byo-openai/) | Installed SDK4 wheel 与官方 OpenAI client 已通过 local Chat peer 执行：验证 known/unknown usage、真实 tool 函数、八次调用上限及 hosted-call guard；无 vendor inference |
| [`jarvis/`](jarvis/) | 保留 interface-3 CLI 持久化/EOF 证据；验证当前 Python MCP tool、LangGraph twin/driver、pybind per-turn/startup 正确性；不是新的性能或语音 qualification |
| [`minimal-mcp/`](minimal-mcp/) | 真实 Python stdlib server 已由官方 MCP client 执行：验证 handshake/discovery 及计算、时间、demo-weather 调用；未使用 LLM 或真实天气服务 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 专用 mock 1,000 个请求：0 个错误、3 个编译后的拓扑、997 次缓存命中；isolated host 只输出 reference metadata；live 1,000/32 未验证 |
| [`openrouter-provider/`](openrouter-provider/) | Installed SDK4 native provider 与 custom HTTP node 已通过 local Chat peer 执行：验证 known/unknown usage、保留 history 及 hosted-call guard；无 vendor inference |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 已执行真实浏览器 tenant 隔离/generation 替换，以及 SQLite 六个、PostgreSQL 六个 black-box 场景；显式 host model policy；无 vendor inference |
| [`the-beast/`](the-beast/) | 类型化 C++ 迁移；已执行真实 strict Core 编译·演化·检查点回滚；不声称所有 live 变体通过 |
| [`topology-retrieval/`](topology-retrieval/) | 已执行 mock Python ranking/index 复用，以及真实 C++ registry 准入(admission)/迁移和 unknown-key 拒绝；外部 pointer 不是权限 |

Python MCP server、Jarvis CLI/REPL driver 和 retrieval HTTP client 是 protocol client，不是 provider binding 实现。Assembly Python speaker 使用 A2A binding，Jarvis pybind benchmark 使用已迁移 native binding。其运行证据与上方 C++ 运行分开。SDK runtime 当前仅在 Linux/POSIX 上获得验证，不声称已验证 macOS/Windows transport。live multitenant 1,000/32 不是 smoke。

## Provider 控制与保留的调用限制

`ProviderControls` 保留各 family 的 reasoning、sampling 和 tool selection。Chat 提供已准入的
OpenRouter reasoning object、usage/include 控制及备用 model；Responses 提供显式服务端保存
continuation、verbosity、truncation 和 include 选择；Messages 提供 manual/adaptive/disabled
thinking、effort、cache 控制及 tool choice；Gemini 提供显式 portable foreign history、
thinking level、safety setting、sampling 及 tool choice。准入(admission)会在 I/O 前拒绝不支持的 family/origin/model 组合。
native message 仍需真实 custody；输出 JSON、cursor、portable 投影不能创建它。
准确类型见 [provider 参考](../../docs/reference-en.md)。

Forge 请求 low reasoning effort，每次 ask 固定 300 秒 deadline。仅已完成的空 `MaxTokens` 响应
没有 text 和有效/无效 client call 时，才以当前输出 cap 的两倍额外调用一次。保留两个 outcome
和 usage；failure、observer/settlement 错误不重试。旧 live chatbot 路径
`multi_tenant_chatbot/server_live_llm.cpp` 和 `self_evolving_chatbot/server_multi.cpp` 使用 180 秒
provider timeout，不改变 ProgramChat 的独立 CLI 限制或共享 factory 默认值。

[编号示例契约](../README.md#retained-example-contracts)说明 Deep Research 的有限 cap ladder
与 deadline discovery、示例 16/28、fork/new-turn、计算的重放节省数、evolution file 模式及
A2A 0.x/1.0 snapshot。这些源码更新不会把上面的历史运行改称为对当前 SDK 的验证，也不会改称为 live-provider 通过。
