<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=zh-CN source_sha256=a2fa88f0d8ef08b3821d8ffe9810bc53b0ed7cd56251bd719b552628036a5b0d -->
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
| 39 个可运行的C++程序 | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| 23 个可运行的 Python 程序 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
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
- `neograph::llm`      — 提供方实现（OpenAI、模式驱动、子智能体辅助）
- `neograph::mcp`      — Model Context Protocol 客户端
- `neograph::async`    — 协程 + io_context 基础设施
- `neograph::util`     — 并发原语

## 一个入门程序

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/mock_provider.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"echo",     {{"type", "llm_call"}}}}},
        {"edges",    json::array({
            {{"from", "__start__"}, {"to", "echo"}},
            {{"from", "echo"},      {"to", "__end__"}}})}
    };

    NodeContext ctx;
    ctx.provider = std::make_shared<llm::MockProvider>();
    auto engine = GraphEngine::build_strict(
        definition, EngineConfig{.node_context = std::move(ctx)});

    RunConfig cfg;
    cfg.thread_id = "demo";
    cfg.input["messages"] = json::array({{{"role","user"},{"content","hi"}}});
    auto result = engine->run(cfg);
    return 0;
}
```

对于真实的 LLM 使用场景，将 `MockProvider` 替换为 `llm::OpenAIProvider` 或 `llm::SchemaProvider`。完整的 `Provider` 接口位于 `neograph::Provider`。

## 参考索引

侧边栏中的类列表、文件列表和命名空间列表是根据 `include/neograph/` 下的头文件生成的。[类列表](annotated.html) 是最常用的入口点。

## 源代码

项目主页：<https://github.com/fox1245/NeoGraph>

许可证：MIT。
