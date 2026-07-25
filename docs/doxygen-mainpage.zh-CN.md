<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=zh-CN source_sha256=11e68f6a872b8ae6ddcc70f8b1cb6a883e34d9e14d1da05358c48d47d56bf2d1 -->
# NeoGraph C++ API 参考 {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)



一个 C++17 图 agent 引擎库，面向 C++ 的 LangGraph，带有可选的 Python 绑定。此站点是公共 C++ 标头的 **生成参考**`include/neograph/`。

## 从哪里开始

如果你刚接触 NeoGraph，**首先阅读叙事文档** - 一旦你知道要查找的内容，此生成参考将用于查找类签名。

|用途|入口|
|---|---|
|NeoGraph 是什么、为什么需要它、基准| [README](https://github.com/fox1245/NeoGraph#readme) |
|心智模型——通道、节点、边、发送、命令|[核心概念](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
|针对常见问题的症状优先修复|[故障排除](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
|39 个可运行的 C++ 程序|[示例/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
|23 个可运行的 Python 程序|[绑定/python/示例/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
|异步/协程内部结构| [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## 顶级标头

便捷头文件引入了完整的核心+图引擎 API：
```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

子命名空间：

- `neograph`— 基础类型（`Provider`, `Tool`, `ChatMessage`）
- `neograph::graph`— 引擎、节点、状态、检查点
- `neograph::llm`— 提供者实现（OpenAI、模式驱动、代理助手）
- `neograph::mcp`— 模型上下文协议客户端
- `neograph::async`— 协程 + io_context 基础设施
- `neograph::util`— 并发原语

## 第一个程序
```cpp
#include <neograph/neograph.h>
#include <neograph/llm/mock_provider.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    json definition = {
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

实际使用 LLM 时，将`MockProvider`替换为 `llm::OpenAIProvider` 或`llm::SchemaProvider`。完整的`Provider`接口位于`neograph::Provider`。

## 参考索引

侧边栏中的类列表、文件列表和命名空间列表是从以下头文件生成：`include/neograph/`。 [类列表](annotated.html)是最有用的切入点。

## 来源

项目主页：<https://github.com/fox1245/NeoGraph>

许可证：MIT。
