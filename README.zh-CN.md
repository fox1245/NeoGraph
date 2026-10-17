<!-- neograph-i18n: source=README.md locale=zh-CN source_sha256=13e082778effbbcb79c2d71addf21b2e5b8ac340acd2dbeb45454f64c6141400 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/neograph-engine)](https://pypi.org/project/neograph-engine/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

NeoGraph 是一个 C++20 库，用来运行以图描述的工作流。节点负责干活，具名的状态通道保存数据，边决定接下来运行什么。运行时负责调度节点，把节点的写入合并进状态；配置了检查点存储时，每一步之后都会保存检查点；也可以暂停运行去等待人工输入，之后再恢复。

同一个引擎既可以在 C++ 中使用，也可以在 Python 中使用（`pip install neograph-engine`）。可选组件包括：通过 SchemaProvider SDK 调用模型，MCP、A2A、ACP 和 gRPC 的服务器与客户端，SQLite 和 PostgreSQL 检查点存储，以及 Program 层——受限的 JavaScript 程序在预算之内协调多张图。

## 适合谁

- 构建状态明确的智能体或流水线工作流的 Python 和 C++ 开发者：循环、分支、并行 fan-out、重试、检查点和人工审批步骤。
- 要在 C++ 服务中运行这类工作流，或通过 MCP、A2A、ACP 把它们开放给其他智能体和工具的团队。
- 熟悉 LangGraph 的读者：带归约器的通道、`Send`、`Command` 和检查点是相同的概念，所以[核心概念指南](docs/concepts.md)读起来会很快。

NeoGraph 是 alpha 阶段的软件（版本 0.14.2）。C++ 二进制接口可能在 0.x 版本之间变化，见 [ABI 政策](docs/ABI_POLICY.md)。它不训练模型，也不能替代数值计算库；模型调用只是节点能做的事情之一。

## 快速开始

下面两个示例都运行只有一个节点的图。它们不需要 API 密钥，运行时也不会发起网络调用。

### Python

需要 Python 3.9 或更高版本：

```bash
pip install neograph-engine
```

把下面的内容保存为 `hello.py`：

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite(
        "messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
    )]

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

```bash
python hello.py
```

```text
[{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

PyPI 上的 wheel 是由 0.14.2 版本构建的。[变更日志](CHANGELOG.md)中 *Unreleased* 下列出的变更不在其中；要使用它们，请从源码构建（见[构建配置](#build-configuration-and-further-reading)）。

### C++

需要 C++20 编译器、CMake 3.20 或更高版本、git，以及 libcurl 开发文件（libcurl 7.88 或更高；Debian 和 Ubuntu 上是 `libcurl4-openssl-dev`）。第一次 configure 时会下载固定版本的 SchemaProvider SDK。即使不做模型调用，Core 也需要这个 SDK。离线机器请参阅下面的[构建与运行](#build-and-run)。

目前还没有与本文档对应的带标签发布版本，所以下面的项目会拉取 `master` 分支。要让构建可复现，请把 `master` 换成某个提交哈希。

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_neograph CXX)

include(FetchContent)
# Build only Core: no model calls, protocols, databases, examples or tests.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_LLM OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ASYNC OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_MCP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_A2A OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ACP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_POSTGRES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_SQLITE OFF CACHE BOOL "")
FetchContent_Declare(NeoGraph
  GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
  GIT_TAG master)
FetchContent_MakeAvailable(NeoGraph)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_20)
target_link_libraries(hello PRIVATE neograph::core)
```

`main.cpp`：

```cpp
#include <neograph/neograph.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

const ChannelKey<std::string> text_channel{"text"};

class UpperNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get(text_channel);
        for (auto& character : text)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        co_return NodeOutput{{ChannelWrite{"text", json(std::move(text))}}};
    }

    std::string get_name() const override { return "upper"; }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "quickstart.upper", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"text"})},
             {"exports", json::array({"text"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "hello"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "quickstart.upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);
    std::cout << result.channel(text_channel) << '\n';
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hello
```

```text
HELLO
```

同样的图见 [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)。要改用通过 `cmake --install` 安装的 NeoGraph，请调用 `find_package(NeoGraph CONFIG REQUIRED)` 并链接 `neograph::core`，详见[构建配置](#build-configuration-and-further-reading)。

### 下一步

- [文档索引](docs/README.md)：所有指南，以及各自适合的读者。
- [核心概念](docs/concepts.md)：通道、节点、边、`Send`、`Command`、检查点和流式输出。
- [Python 绑定指南](docs/python-binding.md)和 [C++ 示例](examples/README.md)。
- 服务器与客户端：[MCP](docs/mcp.md)、[A2A](docs/a2a.md)、[ACP](docs/acp.md)。
- [Program 指南](docs/program-guide.md)：带 fan-out、失败路由、审批和预算的受限 JavaScript 程序。

## 图如何改变状态

假设通道 `count` 的值是 `2`。一个递增节点读到 `2`，返回一个提议把值设为 `3` 的写入。待调度的这批节点运行结束后，运行时通过该通道的归约器应用这次写入。下一批中的后续节点读到的是 `3`。

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

这段过程中出现的术语，描述了执行模型：

| 术语 | 在 NeoGraph 中的含义 |
|---|---|
| 节点（Node） | 在宿主中注册的可执行单元。它读取输入状态，返回对通道的写入以及可选的路由命令。 |
| 状态（State） | 当前执行步骤可见的通道值；按配置还包括由运行时管理的历史和用量记账。 |
| 通道（Channel） | 带归约器的具名值，可以选择配置保留策略和检查点持久化策略。 |
| 归约器（Reducer） | 把通道当前值与新写入合并的函数。`overwrite` 替换值，`append` 累积数组元素，自定义归约器定义其他合并方式。 |
| 边（Edge） | 节点之间的调度规则。边可以是无条件的、有条件的，也可以是等待多个前驱的屏障。成环可以让某些阶段重复执行。 |
| 超步（Superstep） | 把一批就绪的节点作为一组来运行，然后应用它们的写入并推进调度的单位。 |

在普通的一批中，就绪的节点读取的是这一批开始之前的通道状态。节点返回 `ChannelWrite` 并不会立刻改变同批其他节点读到的值。这一批成功完成后，执行器应用结果，之后的步骤看到的是更新后的状态。在有多个分支的 `Send` 批次中，每个分支在隔离的状态副本中接收输入，结束后再合并它的输出。这是 Pregel 式的工作组织方式；NeoGraph 的通道与归约器规则是它自己的契约，并不表示实现了 Pregel 的全部特性。

并发执行并不会让每个归约器都与顺序无关。如果两个节点追加文本，或覆盖同一个通道，写入顺序就会影响结果。如果工作流需要与顺序无关，请使用互相独立的通道，或与顺序无关的归约器。通道的保留策略也与合并是两回事：append 通道可以只保留有界的末尾部分。调度、归约器、屏障和取消，见[核心概念](docs/concepts.md)和[并发](docs/concurrency.md)。

## 逐步看 Core 示例

上面的 [C++ 快速开始](examples/62_core_quickstart.cpp)注册了一个把文本转成大写的节点，并编译出这样的拓扑：

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

源码中声明了节点读写哪些通道，所以 `GraphEngine::build_strict` 可以在运行前检查拓扑。不需要模型调用，也不需要 API 密钥。预期输出是 `HELLO`。

<a id="build-and-run"></a>
### 构建与运行

即使 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 也是必需的外部 SDK，因为 Core 会导出它的带类型 provider 契约。下面的命令使用已安装的 [SchemaProvider 运行时包](https://github.com/fox1245/SchemaProvider)：把 `SCHEMAPROVIDER_PREFIX` 设为它的安装前缀。用 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` 指定的检出优先级最高；否则 CMake 优先使用已安装的包，找不到时才下载固定版本的公开 SDK 压缩包。要用已安装的包或指定的检出做离线构建，请设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不会去猜测相邻目录里的检出，也不会使用已移除的内置解释器。

前提条件包括 C++20 编译器、CMake 3.20 或更高版本，以及 SDK 的运行时依赖 libcurl 7.88 或更高版本。SDK 的 TLS 来自 libcurl 的后端，所以 SDK 本身不需要 OpenSSL。包含 NeoGraph 各 HTTPS 组件的完整构建需要 OpenSSL 3。默认构建还会启用 SQLite 和 PostgreSQL 集成，所以下面的命令会关闭不需要的 NeoGraph 组件，SDK 的依赖仍然保留。平台和构建方面的限制见[故障排除](docs/troubleshooting.md)。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build build-core --parallel --target example_core_quickstart
./build-core/example_core_quickstart
```

## Core 与 ProgramRuntime

`GraphEngine` 运行编译好的图。它负责节点调度、状态更新、路由、重试、流式输出、取消，以及图的检查点和恢复。编译好的拓扑是不可变的；受支持的代际迁移发生在受控的安全点，而不是在节点运行期间任意修改。

`ProgramRuntime` 协调已准入（admission）的 Program。Program 可以调用 Core 图并管理子 Program。它加入了不可变的 Program 版本、目录与策略快照、命令日志、子程序谱系、预算、重放，以及已准入的替换和迁移。宿主注册可执行的能力，编译并准入 Program，然后启动一次调用。运行图节点的仍然是 Core。

对前面的研究工作流来说，一张 Core 图可以完成检索与评审。Program 可以调用这张图，为不同的任务启动子 Program，等待它们的结果，并记录生命周期的转换。持久化恢复需要配置好的存储以及相应的保管（custody）契约；内存存储在进程退出后就没有了，而只有日志也不能让外部工具产生的副作用恰好发生一次。

QuickJS 是可选的 Program 编写接口。Program 使用有界的 JavaScript 计算，以及 `callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint` 和 `cancelScope` 这样的生成器命令。宿主批准各项能力，并在发布之前校验生成的源码。模型生成的提案无法访问编译器、凭据、目录或授予权限的机制。

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel --target example_program_quickstart
./build-program/example_program_quickstart
```

[Program 快速开始](examples/63_program_quickstart.cpp)会编译并准入一个调用递增图的 Program，预期输出是 `1`。它使用内存存储和 C++ 的 Program 构建器。JavaScript 编写和持久化执行，请先读[Program 指南](docs/program-guide.md)、[递归 Program](docs/PROGRAM_RECURSIVE_HARNESSES.md)和[严格运行时契约](docs/STRICT_RUNTIME_INTERPOSITION.md)。

<a id="typed-provider-calls"></a>
## 带类型的 provider 调用

模型调用使用经过校验的描述符、运行时选项和带类型的请求。描述符准入只接受封闭的、带版本的数据，不会执行请求/响应解释器。凭据放在运行时选项里，不放在公开的描述符文件中。`SchemaProvider::Defaults` 包含带类型的 OpenRouter 路由和 Responses 保留设置。图像、Veo 和 Decisions 使用各自带类型的客户端和授权。

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor,
    sp::runtime::Options options, std::string model) {
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

准备好的请求只能消费一次。`sp::runtime::Result` 拥有一个不可变的 `sp::Outcome`，其中是 `Completion` 或 `Failure`。需要有序的消息与片段、原生延续信息、原始观测值、停止依据、尝试元数据或失败时的部分结果时，请保留这个结果。用量计数器可以为空：缺失表示未知，观测到的 0 就是 0。用量报告与预算扣减是两种不同的记录；可移植的报告不能授予支出权限。

`usage.provider_cost` 保留 provider 报告的 nanoUSD 金额、各字段的状态、来源、质量、BYOK 依据，以及对解析出的 binary64 值向上取整的结果。它不是发票、令牌数、估算值，也不是支出权限。缺失的金额元数据在规范化的 v2 JSON 中仍然缺失；报告的 0 或 `is_byok=false` 依然是依据。未知的币种以及互相冲突或格式错误的报告会原样保留，不会换算成 USD 或令牌。阻塞式 provider 调用同样驱动唯一的异步 SDK 管线，并拒绝从 SDK 事件循环或回调工作线程重入。

`first_call` 返回之后，用 `std::get_if<sp::Completion>(result.get())` 查看完成结果的 `messages`、`stop` 和 `usage`。否则，`std::get<sp::Failure>(*result)` 会给出 `error.kind`、`error.safe_message`、重试依据，以及 `partial` 中的部分消息和用量。例如，没有 `completion.usage.output_total` 表示输出令牌数未知；有这个计数器且 `value` 为 `0`，则表示报告的是 0。展示用的文本只是保留结果的一种视图。

`ChatMessage`、`ChatTool` 和 JSON 是可移植的投影。真正的原生历史可以连同原生检查点旁路数据留在内存中；持久化的原生历史则需要真正的 `sp::NativeArchive` 和受保护的、仅限所有者的保管。可移植的 JSON 无法重建这种权限。归档用独立的密钥认证保管状态，它既不是加密，也不是对厂商签发者的认证。不要公开归档正文、密钥、原生 blob 或原始的线路观测值。关于持久化失败、观察者、受管预算库和重放边界，见 [provider 参考](docs/reference-en.md)和[迁移指南](docs/migration-v0.4-to-v1.0.md)。

这次带类型的切换移除了 `CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器以及 Responses 的 WebSocket 路径。请重新编译 C++ 使用方并迁移自定义 provider；没有兼容别名。SDK 包是 `0.3.3` alpha，固定在公开提交 `0437853529b5932e7097a38629a95779d08f03aa`（`maint/interface6` 分支），接口修订版为 6，共享 ABI 为 6。金额用量元数据改变了公开布局：请把使用 SDK 的代码、NeoGraph 和 Python 扩展一起重新构建。原生归档 v3 和可移植 JSON v2 保持不变；这一代不声明稳定的 SDK 接口。

## Python

```bash
pip install neograph-engine
```

这里介绍的带类型 provider API 针对 NeoGraph `0.14.0`；更早的 wheel 暴露的是旧接口。源码 API 和构建前提见 [Python 绑定指南](docs/python-binding.md)。[快速开始](#python)里的图不需要 API 密钥。

Python 还提供 Program 的编译与运行、Hook、运行时上下文要求、严格配置、SQLite 持久化，以及精确的检查点恢复。带类型的 provider 使用 `ProviderMessage` 和 `make_provider_request`，然后调用 `prepare`/`dispatch` 或 `invoke`；结果保留由原生持有的完成或失败依据。这些消息与图的便捷类型 `ChatMessage` 不同。阻塞式 provider 调用会释放 GIL，可以用 `asyncio.to_thread` 把它们移出事件循环线程。完整的 provider 输入见 [Python 示例](bindings/python/examples/README.md)，可运行的 Program 示例及其规则见 [Python 绑定指南](docs/python-binding.md#program-and-quickjs)。

## 适用场景与限制

NeoGraph 适合状态转换明确、含分支或循环、并行任务、检查点和人工评审的工作流。也可以把小型固定的图嵌入 C++ 应用。节点内的计算仍由应用负责：图运行时不训练模型，也不能替代数值计算库，模型调用依然带有 provider 的延迟、可用性和成本。

运行时支持图级别和节点级别的重试策略、有界的可复用节点缓存、`Send` fan-out、`Command` 路由、子图、状态历史、分叉、HITL 和 `NodeInterrupt`。MCP、A2A、ACP、gRPC 和可观测性集成都是可选组件。运行时上下文和 Hook 可以要求特定的派发输入并记录送达依据；这些检查并不能证明模型关注了每一个令牌。持久化的原生恢复和有界分叉，需要原来的共享记账权限；复制快照并不能让预算重新生效。

请用实际的节点、provider、存储、并发度和构建配置来测量工作负载。[基准测试](benchmarks/README.md)和[性能指南](docs/performance-deep-dive.md)描述的是测得的配置及其限制，不是适用于所有场景的速度承诺。要测量优化后的执行，单配置构建应指定 `CMAKE_BUILD_TYPE=Release`。`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` 会在受支持的编译器上加入针对宿主的调优；要分发的二进制请关闭它。

<a id="build-configuration-and-further-reading"></a>
## 构建配置与延伸阅读

| 选项 | 用途 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 指定的 SDK 检出。优先于已安装包的查找和下载。 |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | 没有已安装的包时，下载固定版本的公开 SDK 压缩包。默认开启；离线构建时请关闭。 |
| `NEOGRAPH_BUILD_PROGRAM` | Program 运行时、目录、谱系和迁移。默认关闭。 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 内嵌的 QuickJS Program 编写。默认关闭。 |
| `NEOGRAPH_BUILD_PYBIND` | Python 扩展。默认关闭。 |
| `NEOGRAPH_BUILD_LLM` | NeoGraph 的模型调用适配器。关闭它并不会去掉对 SDK 的依赖。 |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | 可选的持久化存储。两者默认都开启。 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP 客户端和 stdio 服务器组件。两者默认都开启（默认值由 `NEOGRAPH_BUILD_MCP` 决定）。 |
| `NEOGRAPH_BUILD_MCP_HTTP_SERVER` | MCP Streamable HTTP 服务器传输。默认关闭。 |
| `NEOGRAPH_BUILD_HARNESS_MCP_BINARY` | 构建并安装 `neograph-harness-mcp` 可执行文件。默认关闭。 |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | 协议集成。A2A 和 ACP 默认开启，gRPC 默认关闭。 |
| `NEOGRAPH_BUILD_ASYNC` | HTTP、SSE 和 WebSocket 辅助库。默认开启。 |
| `NEOGRAPH_BUILD_EXAMPLES` / `NEOGRAPH_BUILD_TESTS` | 示例程序（默认开启）和单元测试（默认关闭）。 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 面向优化配置的、针对宿主的调优，不可移植。默认关闭。 |

如果想把 Core 安装下来供其他项目使用，而不是每次拉取，请构建并安装一次。如果 CMake 下载了 SDK，`cmake --install` 会把 SDK 装进同一个前缀，所以 `CMAKE_PREFIX_PATH` 里只需要一个条目：

```bash
git clone https://github.com/fox1245/NeoGraph.git
cmake -S NeoGraph -B NeoGraph/build-install -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build NeoGraph/build-install --parallel
cmake --install NeoGraph/build-install --prefix "$HOME/neograph"
```

在另一个项目里，把快速开始中的 `FetchContent` 几行换成 `find_package(NeoGraph CONFIG REQUIRED)`，并用 `-DCMAKE_PREFIX_PATH="$HOME/neograph"` 配置。使用已安装版本的项目只链接它需要的已启用组件：

```cmake
find_package(SchemaProvider 0.3.3 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

### 本地 CI 验证（Windows 和 WSL）

在源码根目录运行。需要 Python 3.10 或更高版本、CMake/CTest，并且已经准备好该配置所需的原生工具链和依赖。在 Windows 上使用 VS2022 x64 开发者 PowerShell，通过 `CMAKE_TOOLCHAIN_FILE` 提供 vcpkg 传输工具链，通过专用的 `VCPKG_INSTALLED_DIR` 指定安装目录，并让 `PATH` 中有 OpenSSL/curl 运行时工具。在 WSL 中，请准备 C++20 编译器、pkg-config、libpq、SQLite、OpenSSL 以及支持 HTTP/2 的 curl 开发包。Linux 上的 `native-linux` 和 `asan` 还需要 `psql`，以及指向一个可连接的、**仅供破坏性测试使用**的数据库的 `NEOGRAPH_TEST_POSTGRES_URL`。请准备 Python 的 pytest/pydantic/certifi；`native-linux` 另外需要在所选解释器中装有 `a2a-sdk[http-server]>=1.1,<2`、`agent-client-protocol==0.12.1`、uvicorn 和 httpx。

```powershell
# Windows: choose a new output path for each invocation.
python scripts/verify_ci.py native-windows --work-dir build/local-windows-01 --jobs 4
python scripts/verify_ci.py install --work-dir build/local-install-01 --jobs 4 --shared --program
```

```sh
# WSL/Linux: provision the test database and dependencies before running.
python scripts/verify_ci.py native-linux --work-dir build/local-linux-01 --jobs 4
python scripts/verify_ci.py quickjs-performance --work-dir build/local-quickjs-01 --jobs 4
```

`--work-dir` 必须是你自己拥有的、全新的、不存在的输出路径，不能是源码根目录或它的上级目录；`--jobs` 必须是正数或 `auto`（CPU 数）。遇到已有的输出时会拒绝使用并原样保留，不会自动清理。运行器不会安装依赖，也不会更改宿主策略；固定依赖的下载，以及 cibuildwheel 声明的引导和修复，仍然照常进行。

| 配置 | 保留的目的 / 前提条件 |
|---|---|
| `native-linux` | 包含 PostgreSQL 的完整原生关卡，随后串行运行完整的 Python/协议测试，并重新运行不依赖数据库的 ACP 持久化测试。 |
| `native-posix` | Linux ARM/macOS 原生测试；覆盖 PostgreSQL 的构建与链接，但没有测试服务。 |
| `native-windows` | 基于 MSVC、不依赖数据库的原生/Program/QuickJS 测试，以及单独的 C 嵌入 ABI 冒烟测试；在 x64 MSVC 环境中用 Ninja 构建，也可以传入 `--generator "Visual Studio 17 2022"`。 |
| `asan` | Linux 的 ASan/UBSan/LSan、11 个示例和完整的 Python 测试。需要 GCC 的 libasan/libstdc++。 |
| `tsan` | 单独的 Linux TSan 测试和 5 个示例；允许进程内的 `setarch -R`，抑制列表保持不变。 |
| `msvc-asan` | Windows 上串行运行的 Program/QuickJS 金丝雀测试；需要已激活的 `cl >=19.50`（VS2026），MSVC 19.44 不行。 |
| `grpc` | gRPC 图契约。需要准备 gRPC/Protobuf 编译器和库。 |
| `benchmark` | Linux Release 下检查 4 种工作负载的回归关卡；沿用原有的吞吐量、延迟和目标 RSS 上限。 |
| `quickjs-performance` | Linux 上对比开启与关闭的两个匹配构建；为得到不可变的来源信息，需要真实的源码根目录 Git 检出。上限以 CI 的托管 x86-64 运行器为基准，在更慢的硬件上即使没有回归，`javascript_wrapped_core` 也可能失败。指定 `--base <ref>` 时，还会构建该提交并在同一台机器上逐个用例进行比较。 |
| `fuzz` | Linux Clang/libFuzzer 的 60 秒金丝雀测试；语料会复制到你拥有的输出目录。 |
| `install` | 在隔离的导出前缀中检查 ABI、符号、C++、C11、冲突和重定位的使用方；不需要 Git/Bash。可选 `--shared`、`--core-only` 或 `--program`；`--quickjs` 需要 `--program` 以及 ELF/Mach-O 检查，Windows 行没有它。 |
| `sdist` | 源码压缩包和 Twine。需要 build/twine/scikit-build-core>=1.0/pybind11==2.13.6/ninja>=1.10；可选 `--release-tag v0.14.2`。 |
| `wheel` | 修复后的已安装 wheel。需要 cibuildwheel==2.23.0 和原生/容器 provider；`--arch x86_64\|aarch64\|arm64\|AMD64` 为必填；可选 `--python cp312`，只构建一个 CPython 而不是全部。 |
| `runtime-archive` | 共享 SDK 原生归档的可移植性目标/CTest。需要 Ninja 和平台运行时依赖。 |

本地 Windows/WSL 的结果不能替代原生 ARM/macOS 或 VS2026 sanitizer 行。每次 push 和 pull request 都会运行 `ci.yml`：在 Linux、macOS 和 Windows 上运行完整的原生测试，再加两行已安装使用方的检查；仅修改文档时会跳过。`ci-extended.yml` 每晚运行，也可以按需运行：sanitizer、模糊测试金丝雀、性能关卡、原生 ARM64、gRPC、Visual Studio 生成器，以及其余八行已安装使用方的检查。发布候选版本在打标签之前请运行它。`wheels.yml` 在打包相关变更时为每个平台构建一个 CPython 3.12 的 wheel。在发布标签上，它为 Linux（x86_64 和 aarch64）、macOS（arm64）和 Windows（AMD64）构建 CPython 3.9–3.13 的 wheel，最低平台版本为 glibc 2.34 和 macOS 14。

- [文档索引](docs/README.md)
- [核心概念与图语义](docs/concepts.md)
- [C++ 参考](docs/reference-en.md)和 [Python 绑定指南](docs/python-binding.md)
- [MCP](docs/mcp.md)、[A2A](docs/a2a.md)、[ACP](docs/acp.md) 指南，以及 [Program 指南](docs/program-guide.md)
- [异步指南](docs/ASYNC_GUIDE.md)和[并发与取消](docs/concurrency.md)
- [运行时上下文与严格介入](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [迁移指南](docs/migration-v0.4-to-v1.0.md)和[故障排除](docs/troubleshooting.md)
- [C++ 示例](examples/README.md)、[Python 示例](bindings/python/examples/README.md)和[基准测试方法](benchmarks/README.md)

## 许可证

MIT，见 [LICENSE](LICENSE)。第三方声明在 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) 中。
