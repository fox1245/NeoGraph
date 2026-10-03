<!-- neograph-i18n: source=examples/cookbook/README.md locale=zh-CN source_sha256=01f0466eb43755a45f571b1f6bcdd22e270983bc426fb68d843fdc881e63aa3e -->
# NeoGraph Cookbooks


## 类型化 C++ 迁移状态

当前 C++ recipe 使用五个类型化 SDK family 的拥有所有权的 `ProviderRequest`、
有序 `sp::Event` 和不可变 `std::shared_ptr<const sp::Outcome>`（`Completion`/`Failure`）。
`ChatMessage`/`ChatTool` 是 portable 投影，不是 native replay 权限。
只 prepare 一次；durable 调用者将 claim/receipt 绑定到
`Provider::request_digest(prepared)`，并 dispatch 同一个 handle。
不要从打印的 JSON 重建 history，也不要把 failure 简化为最终文本。

canonical persistence 使用 `provider-message-v2`/`runtime-history-record-v2`；
portable 摘要不能替代 native record。optional control 由调用者选择，不暗中 clamp。
bounded call 需要真实 model fact；缺失为 `LimitUnknown`。
reservation/charged/held 与 nullable provider usage 独立，不更新 budget，也不是价格、forecast 或 invoice。

header-only `examples/provider_example_support.h` 使用真实 SDK runtime。
LLM 构建需要 `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)` 的 `SchemaProvider::runtime`，
或显式 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`。
安装 include root 是 `include/SchemaProvider`；执行 interface/capability 检查，
SDK package 仍为 unstable `0.0.0`（interface 3）。

当前 model-free 执行已覆盖 Assembly 四个真实本地 A2A member 服务器与 C++ speaker、
JARVIS CLI synthetic turn 和记忆持久化、Beast strict Core 编译·演化·checkpoint 回滚、
ProgramChat 浏览器 tenant 隔离·generation 替换及 PostgreSQL black-box 六个场景。
下表区分这些实际执行范围与未验证 surface。不声称 vendor inference、语音、延期 Python binding、
全部 Beast live 变体或专用 multitenant server/load 已通过。
历史测量不是新迁移的 qualification。live 运行需要密钥、网络和模型访问并产生费用。
密钥、prompt、artifact 必须保持私密；envelope/native inspection 输出含敏感内容，不得进入公开 log。
native archive 是经过认证的 owner-private custody，不是加密或 vendor issuer 认证。

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

端到端配方，将多个 NeoGraph 功能组合成实际可运行的场景。每个配方都是自包含的：复制文件夹，按其README操作，即可运行。

| Cookbook | 它所展示的内容 |
|---|---|
| [`the-beast/`](the-beast/) | **一个基于自演化的智能体：生成 · 演化 · 回滚。** The Beast 编写严格的 Core JSON，在执行前进行语义验证，使用`evolve()`演化其有界 Core 拓扑，并通过检查点回滚。live（活体）、apex、forge、script 和 arithmetic-evolution 变体保留了相同的 compiler/validation 边界； JavaScript 或受信任的 C++ 负责源码编写，而严格的 Core JSON 仍作为数据交换格式。 |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A：4名国民议会议员（每个代表都拥有自己的 A2A 端点）+ 一名议长，并行广播一项法案并集中计票。跨语言：C++ 成员服务器 + Python 或 C++ Speaker。 |
| [`byo-openai/`](byo-openai/) | 历史 provider recipe；Python binding 迁移延期. |
| [`jarvis/`](jarvis/) | **语音驱动的元编排器（骨架）。** 麦克风 → whisper.cpp（自动检测语言）→ 路由器（直接 / 委托 / 并行 3-way）→ MCP 工具或 A2A 专家 → 超音设备端 TTS，使用用户检测到的语言。 JSON 驱动的工具 + agent 目录，A2A 双向（JARVIS 本身也可达 可达）。设备端，无需云端。 |
| [`minimal-mcp/`](minimal-mcp/) | MCP客户端往返通信，**无需LLM、无需API密钥、无需fastmcp**：一个约60行的stdlib stdio服务器 + 一个C++测试框架，它执行`initialize` → `tools/list` → `tools/call`。这表明NeoGraph的MCP客户端只需要一个说线路协议的进程——对等体可以是任何东西。 |
| [`openrouter-provider/`](openrouter-provider/) | 历史 provider recipe；Python binding 迁移延期. |

每份cookbook还记录了它暴露的摩擦点——对于寻找公共API的粗糙边缘很有用。

## 完整 recipe 与延期 surface 清单

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 类型化 C++ 迁移；已 offline 执行四个真实本地 A2A member 服务器与 C++ speaker；synthetic abstention 不是模型判断 |
| [`byo-openai/`](byo-openai/) | 历史 provider recipe；Python binding 迁移延期 |
| [`jarvis/`](jarvis/) | 类型化 C++ 迁移；已执行 CLI synthetic turn、记忆持久化与正常 EOF；语音/Python surface 未验证 |
| [`minimal-mcp/`](minimal-mcp/) | protocol-only client/server；有意保持不变 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 类型化 C++ 迁移；专用 server 执行与 live 1,000/32 load 未验证 |
| [`openrouter-provider/`](openrouter-provider/) | 历史 provider recipe；Python binding 迁移延期 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 类型化 C++ 迁移；不调用 vendor inference，已执行 ProgramChat 浏览器 tenant 隔离·generation 替换及 PostgreSQL black-box 六个场景 |
| [`the-beast/`](the-beast/) | 类型化 C++ 迁移；已执行真实 strict Core 编译·演化·checkpoint 回滚；不声称所有 live 变体通过 |
| [`topology-retrieval/`](topology-retrieval/) | protocol-only client/server；有意保持不变 |

Python MCP server、Jarvis CLI/REPL driver 和 retrieval HTTP client 是 protocol client，不是 provider binding 实现。Assembly Python speaker 与 Jarvis pybind benchmark 依赖延期 binding。live multitenant 1,000/32 不是 smoke。
