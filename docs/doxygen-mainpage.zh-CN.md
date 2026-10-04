<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=zh-CN source_sha256=2bf76338f86fbdedaf1a0d47cb15d8901c26412e1d6988676ceec5bb62ce2f92 -->
# NeoGraph C++ API 参考 {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

一个 C++20 图智能体引擎库——面向 C++ 的 LangGraph，附带可选的 Python 绑定。本网站是 `include/neograph/` 中公共 C++ 头文件的**生成参考**。

## 从哪里开始

如果你是 NeoGraph 的新手，**请先阅读叙述性文档** —— 本参考文档仅用于在您了解所需内容后查找类签名。

| 关于 | 请前往 |
|---|---|
| 了解 NeoGraph 是什么、为何选择它以及基准测试 | [README](https://github.com/fox1245/NeoGraph#readme) |
| 心智模型 —— 频道、节点、边、Send、Command | [Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| 症状优先的常见问题修复 | [故障排查](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| C++ 示例（验证另行报告） | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python typed provider 与图示例 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| 异步 / 协程内部机制 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## 顶层头文件

便捷头文件引入了完整的 Core + GraphEngine API：

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

子命名空间：

- `neograph`           — 基础类型（`Provider`、`Tool`、`ChatMessage`）
- `neograph::graph`    — 引擎、节点、状态、检查点
- `neograph::llm` — `SchemaProvider`, `Agent`; typed SDK runtime
- `neograph::mcp`      — Model Context Protocol 客户端
- `neograph::async`    — 协程 + io_context 基础设施
- `neograph::util`     — 并发原语

## 一个入门程序

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model) {
    neograph::llm::SchemaProvider provider(
        std::move(descriptor), std::move(options), {});
    std::vector<sp::Message> history{
        {.role = sp::Role::User, .parts = {sp::Text{"hi"}}}};
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history));
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));
}
```

`SchemaProvider` 接收获准的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 及可选 `SchemaProvider::Defaults`。descriptor 是 closed/versioned 数据 admission，不是请求/响应 interpreter 或任意 primitive registry。credential 应放在 runtime options，而非公开 descriptor。Defaults 仅包含 typed OpenRouter routing 和 Responses 保留 (`responses_store`)，后者仅适用于 Responses。Hosted OpenRouter routing、retention、JSON 格式仍是声明的 typed 控制。Images、Veo、Decisions 使用独立的 NeoGraph typed client 和独立授权，不继承 SDK chat grant。

提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、保留的 native continuation 与 family 提供的 wire 证据、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。使用量是带依据、阶段、质量的 nullable `uint64_t`；缺失表示未知，绝不是零。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

Wire 证据由 family 提供，且是可选的：`sp::Completion::wire_envelope` 可以为 null（Python 的 `ProviderCompletion.wire_envelope` 为 `None`）。当前 buffered Chat 将完整响应 JSON 保留在 `raw_events` 的 `RawWire` 中，`type == "chat.completion"`，文档位于 `payload`，而 `wire_envelope` 保持 null。请从 family 实际保留的位置读取证据，不会伪造 fallback envelope。Native continuation 与 raw buffer 仍为受保护的证据，不进入 trace payload。

这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。发布目标为 SDK `0.1.0` alpha、interface revision 4 / shared ABI 4，使用 out-of-line capability check；alpha 不承诺稳定 API。已记录的 interface-3 runtime/archive 验证仅覆盖 Linux/POSIX，不验证 interface 4 或 Windows、macOS、WASM runtime。

Python 使用 `SchemaProvider(ValidatedDescriptor, ProviderRuntimeOptions,
SchemaProviderDefaults)`，以及接收 typed `ProviderMessage` part 的
`make_provider_request`。`prepare` 返回 `PreparedProviderRequest`；
`dispatch` 只消费一次该对象，而 `invoke` 先准备再分派请求。
`ProviderOutcome` 保留不可变 completion/failure/partial view，未知使用量为
`None`。图的 `ChatMessage` 是独立的便捷类型，不是 provider message alias。
阻塞式 provider 调用释放 GIL；Python async 调用方可使用 `asyncio.to_thread`。
构造函数、错误和限制见 [`Python binding guide`](python-binding.md)。


实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
## 参考索引

侧边栏中的类列表、文件列表和命名空间列表是根据 `include/neograph/` 下的头文件生成的。[类列表](annotated.html) 是最常用的入口点。

## 源代码

项目主页：<https://github.com/fox1245/NeoGraph>

许可证：MIT。
