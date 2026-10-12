<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=zh-CN source_sha256=d3f40032eb64bd20115a5e0189753523ae701025f3b2a0139baca7973c0aba1b -->
# NeoGraph C++ API 参考 {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

一个 C++20 的图智能体引擎库，可以看作 C++ 版的 LangGraph，另带可选的 Python 绑定。本站是 `include/neograph/` 中公共 C++ 头文件的**自动生成参考**。

## 从哪里开始

刚接触 NeoGraph 的话，**请先读叙述式的文档**；这份自动生成的参考，是在已经知道要找什么之后，用来查类签名的。

| 想了解 | 前往 |
|---|---|
| NeoGraph 是什么、为什么用它、基准测试 | [README](https://github.com/fox1245/NeoGraph#readme) |
| 心智模型：通道、节点、边、Send、Command | [核心概念](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| 按症状查找常见问题的解决办法 | [故障排查](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| C++ 示例（验证结果另行报告） | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python 带类型的 provider 和图的示例 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| 异步/协程的内部机制 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## 顶层头文件

这个便捷头文件会引入完整的 core 和图引擎 API：

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

子命名空间：

- `neograph`           — 基础类型（`Provider`、`Tool`、`ChatMessage`）
- `neograph::graph`    — 引擎、节点、状态、检查点
- `neograph::llm` — `SchemaProvider`、`Agent`；带类型的 SDK 运行时
- `neograph::mcp`      — Model Context Protocol 客户端
- `neograph::async`    — 协程与 io_context 基础设施
- `neograph::util`     — 并发原语

## 第一个程序

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

`SchemaProvider` 接受已准入的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 和可选的 `SchemaProvider::Defaults`。加载描述符是对封闭且带版本的数据做准入，不是请求/响应解释器，也不是任意原语的注册表。凭据应放在运行时选项里，而不是公开的描述符文件里。Defaults 只包含带类型的 OpenRouter 路由和 Responses 的留存（`responses_store`）；后者只对 Responses 有效。托管的 OpenRouter 路由、留存和 JSON 格式，仍然是需要声明的、带类型的控制项。Images、Veo 和 Decisions 使用各自独立的 NeoGraph 带类型客户端，并有各自独立的授权；它们不继承 SDK 的 chat 授权。

provider 调用返回 `sp::runtime::Result`：它是不可变的、持有所有权的 `std::shared_ptr<const sp::Outcome>`，其中包含 `sp::Completion` 或 `sp::Failure`。请保留完整的 outcome，而不只是用于显示的文本。有序的消息/parts、保留下来的 native 续接状态和请求族提供的 wire 证据、有序的原始观测、停止证据和真实的尝试元数据，在调用结束和客户端销毁之后依然存在。用量计数器是可为空的 `uint64_t` 值，附带证据、stage 和 quality：缺失表示未知，绝不会是零。失败会保留它原来的部分 outcome。`ProviderFailure::outcome()` 和 `ProviderObserverError::outcome()` 保留这份结果；后者还会在 `cause()` 中保留 observer 的异常。

wire 证据由请求族提供，而且是可选的：`sp::Completion::wire_envelope` 可能为空（在 Python 中，`ProviderCompletion.wire_envelope` 为 `None`）。带缓冲的 Chat 目前把完整的响应 JSON 作为 `RawWire` 保存在 `raw_events` 里，其 `type == "chat.completion"`，文档内容在 `payload` 中；`wire_envelope` 则保持为空。请到请求族保留证据的地方去读取；不会伪造一个兜底的 envelope。native 续接状态和原始缓冲区仍然是受保护的证据，不会出现在 trace 的 payload 里。

这是一次源码和二进制层面的不兼容变更：每个 C++ 使用方和自定义 provider，都要用匹配的新头文件和库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器和 Responses WebSocket 路径都已移除，没有别名，也没有兼容桥。发布所针对的是 SDK `0.1.1` alpha，接口修订版 4 / 共享库 ABI 4，并带有非内联的 capability 检查；alpha 不是对稳定 API 的承诺。

Python 使用 `SchemaProvider(ValidatedDescriptor, ProviderRuntimeOptions,
SchemaProviderDefaults)` 和 `make_provider_request`，消息是带类型的
`ProviderMessage` parts。`prepare` 返回 `PreparedProviderRequest`；
`dispatch` 把它消耗掉（只能用一次），`invoke` 则一次完成准备和分发。
`ProviderOutcome` 保留不可变的 completion/failure/partial 视图，未知的用量
为 `None`。图使用的 `ChatMessage` 是另一个便捷类型，不是 provider 消息的别名。
阻塞的 provider 调用会释放 GIL；Python 的异步调用方可以使用
`asyncio.to_thread`。构造函数、错误和限制，参见
[Python 绑定指南](python-binding.md)。


如果在已有真实结果之后，副作用后的核算或终态回执的持久化失败了，`ProviderDispatchOutcomePersistenceError` 会在 `outcome()` 中保留原来的不可变结果，在 `cause()` 中保留原来的持久化异常。如果交付也失败了，`delivery_error()` 会保留原来的 observer 异常。持久化成功、随后 observer 失败时，会原样重新抛出那个原始的 observer 异常；状态未知、没有结果的传输失败，不会伪造 outcome。

## 参考索引

侧边栏中的类列表、文件列表和命名空间列表，都是根据 `include/neograph/`
下的头文件生成的。最好用的入口是[类列表](annotated.html)。

## 源码

项目主页：<https://github.com/fox1245/NeoGraph>

许可证：MIT。
