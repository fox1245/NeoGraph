<!-- neograph-i18n: source=README.md locale=zh-CN source_sha256=c581af0e3041a7d6e48040bfd17085c2297a0c796d34547f6528392ec2b7ce27 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph 是一个 C++20 运行时，用于执行以图描述的有状态工作流。图定义可执行节点、具名状态通道、合并写入的规则，以及决定下一步执行内容的边。节点可以执行普通计算、调用工具或请求模型输出。运行时负责调度这些节点、应用它们的写入；配置检查点存储后，还会保存执行进度，以便中断和恢复。Python 绑定使用同一个 C++ 引擎。

以研究工作流为例：检索文档，从多个文档中提取发现，汇总这些发现，再请审阅者判断是否需要新一轮检索。文档和发现保存在状态通道中；检索、提取和审阅是节点；边选择下一阶段，或返回检索阶段。图将这些状态转换明确表示出来，而不是把它们隐藏在一连串模型提示中。[示例](examples/README.md)涵盖研究、工具使用、人工审阅和多智能体工作流。

## 图如何改变状态

假设名为 `count` 的通道保存着 `2`。一个递增节点读取 `2`，返回一个提议将值改为 `3` 的写入。当前调度的节点批次结束后，运行时通过该通道的归约器应用这个写入。下一批次中的下游节点读取到 `3`。

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

这段执行轨迹中的术语描述了执行模型：

| 术语 | 在 NeoGraph 中的含义 |
|---|---|
| 节点（Node） | 在宿主中注册的可执行单元。它读取输入状态，返回通道写入以及可选的路由命令。 |
| 状态（State） | 当前执行步骤可见的通道值，以及按配置由运行时管理的历史记录和记账信息。 |
| 通道（Channel） | 一个具名值，带有归约器，以及可选的保留策略和检查点持久化策略。 |
| 归约器（Reducer） | 将当前通道值与传入写入合并的函数。`overwrite` 替换值；`append` 累积数组元素；自定义归约器定义其他合并方式。 |
| 边（Edge） | 节点之间的调度规则。边可以是无条件边、条件边，也可以是等待多个前驱的屏障。环允许重复执行阶段。 |
| 超步（Superstep） | 执行一批已就绪节点，随后应用它们的写入并推进调度。 |

在正常批次中，已就绪节点读取批次开始前的通道状态。返回 `ChannelWrite` 不会立即改变同批其他节点读取到的内容。成功完成后，执行器应用结果，后续步骤才能看到更新后的状态。在多分支 `Send` 批次中，每个分支在隔离的状态副本中接收输入，随后合并其输出。这种工作组织方式借鉴了 Pregel；NeoGraph 的通道和归约器规则有自己的契约，并不意味着它实现了 Pregel 的全部功能。

并发执行并不使所有归约器都与顺序无关。如果两个节点追加文本或覆写同一个通道，写入顺序会影响结果。工作流需要顺序无关性时，应使用独立通道或与顺序无关的归约器。通道保留策略也不同于合并规则：追加通道可以只保留长度受限的末尾部分。[概念](docs/concepts.md)和[并发](docs/concurrency.md)文档介绍了调度、归约器、屏障和取消。

## Core 示例详解

[完整 C++ 快速入门](examples/62_core_quickstart.cpp)注册了一个转大写节点，并编译以下拓扑：

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

节点中的计算是普通 C++ 代码：

```cpp
class UpperNode final : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        auto text = input.state.get(neograph::graph::ChannelKey<std::string>{"text"});
        for (auto& character : text)
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        co_return neograph::graph::NodeOutput{{
            neograph::graph::ChannelWrite{"text", neograph::json(std::move(text))}}};
    }
    std::string get_name() const override { return "upper"; }
};
```

完整源代码包含头文件、声明读写范围的节点注册、拓扑、`GraphEngine::build_strict`、运行输入，以及类型化的输出访问。它不需要模型调用或 API 密钥。预期输出为 `HELLO`。

### 构建与运行

即使设置 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 也仍是必需的外部 SDK，因为 Core 导出了其类型化提供方契约。下面的命令使用已安装的 [SchemaProvider 运行时包](https://github.com/fox1245/SchemaProvider)：将 `SCHEMAPROVIDER_PREFIX` 设为其安装前缀。通过 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` 显式指定的源码检出目录具有最高优先级；否则，CMake 优先使用已安装的软件包，未找到时则获取锁定版本的公共 SDK 源码归档。使用已安装的软件包或显式检出目录进行离线构建时，设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不会猜测同级目录中的源码，也不会使用已移除的内置解释器。

前置依赖包括 C++20 编译器、CMake 3.20 或更新版本，以及 SDK 的运行时依赖，其中包括 OpenSSL 和 libcurl 7.88 或更新版本。包含 NeoGraph HTTPS 组件的完整构建需要 OpenSSL 3。默认构建还启用了 SQLite 和 PostgreSQL 集成；下面的命令关闭了不需要的 NeoGraph 组件，但不会移除 SDK 依赖。记录中的 SDK 接口修订 4 验证覆盖 Linux x86_64 及本地协议、状态对端，准确范围见 [SDK 验证记录](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)。它们不构成新的 Windows、macOS、ARM64、HTTP/3、托管服务商或 WASM 验证；平台和构建限制见[故障排查](docs/troubleshooting.md)。

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

`GraphEngine` 执行编译后的图，负责节点调度、状态更新、路由、重试、流式输出、取消，以及图检查点和恢复。编译后的拓扑不可变；受支持的代际迁移在受控安全点发生，而不是在节点运行期间任意修改拓扑。

`ProgramRuntime` 协调已获准执行的 Program；这些 Program 可以调用 Core 图并管理子 Program。它增加了不可变的 Program 版本、目录和策略快照、命令日志、子 Program 谱系、预算、重放，以及经过准入的替换或迁移。宿主注册可执行能力，编译并准入 Program，然后启动一次调用。Core 仍然负责执行图节点。

在研究工作流中，一个 Core 图可以执行检索和审阅。Program 可以调用该图，为独立任务启动子 Program，等待它们的结果，并记录生命周期转换。持久化恢复需要已配置的存储和相应的保管契约；内存存储无法在进程退出后保留数据，日志本身也不能保证外部工具的副作用恰好发生一次。

QuickJS 是可选的 Program 编写接口。Program 使用有界 JavaScript 计算和生成器命令，例如 `callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint` 和 `cancelScope`。宿主负责能力准入，并在发布前验证生成的源代码。模型生成的提案不会获得编译器、凭据、目录或授予权限的访问能力。

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

[Program 快速入门](examples/63_program_quickstart.cpp)编译并准入一个调用递增图的 Program；预期输出为 `1`。它使用内存存储和 C++ Program 构建器。若要使用 JavaScript 编写 Program 或实现持久化执行，请先阅读[编写边界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)、[递归 Program](docs/PROGRAM_RECURSIVE_HARNESSES.md)和[严格运行时契约](docs/STRICT_RUNTIME_INTERPOSITION.md)。

## 类型化提供方调用

模型调用使用经过验证的描述符、运行时选项和类型化请求。描述符准入接受封闭的、带版本的数据，不执行请求/响应解释器。凭据应放在运行时选项中，而不是公共描述符文件中。`SchemaProvider::Defaults` 包含类型化的 OpenRouter 路由和 Responses 保留控制。Images、Veo 和 Decisions 使用独立的类型化客户端和授权。

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

已准备的请求只能消费一次。`sp::runtime::Result` 拥有不可变的 `sp::Outcome`，其中包含 `Completion` 或 `Failure`。需要有序消息和消息部分、原生续接、原始观测、停止证据、尝试元数据或失败时的部分结果时，应保留该 outcome。用量计数可以为空：缺失表示未知，观测到的零值仍是零。用量报告和预算扣账是不同的记录；可移植报告不能授予支出权限。

`first_call` 返回后，可使用 `std::get_if<sp::Completion>(result.get())` 检查完成结果的 `messages`、`stop` 和 `usage`。否则，`std::get<sp::Failure>(*result)` 提供 `error.kind`、`error.safe_message`、重试证据，以及 `partial` 中的部分消息和用量。例如，`completion.usage.output_total` 缺失表示输出 token 数未知；计数存在且其 `value` 为 `0` 则表示用量为零。显示文本只是所保留 outcome 的一种视图。

`ChatMessage`、`ChatTool` 和 JSON 是可移植投影。真实的原生历史可以与原生检查点伴随数据一起保留在内存中；持久化原生历史需要真正的 `sp::NativeArchive`，以及受保护的、所有者私有的保管机制。可移植 JSON 无法重建这类权限。归档通过独立密钥认证保管关系；它既不是加密，也不是服务商签发者身份认证。不要公开归档内容、密钥、原生二进制数据或原始线路观测。[提供方参考](docs/reference-en.md)和[迁移指南](docs/migration-v0.4-to-v1.0.md)介绍了持久化失败、观察器、托管预算银行和重放边界。

这次类型化接口切换移除了 `CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器和 Responses WebSocket 路径。C++ 使用方需要重新编译，并迁移自定义提供方；没有兼容别名。SDK 包版本为 `0.1.1`，接口处于 alpha 阶段，接口修订号为 4，共享 ABI 为 4；修订号必须匹配，而这些数字并不表示 SDK 接口已经稳定。

## Python

```bash
pip install neograph-engine
```

本文描述的类型化提供方 API 面向 NeoGraph `0.13.1`；历史 wheel 包提供的是旧接口。[Python 绑定指南](docs/python-binding.md)介绍了源码 API 和构建前置条件。

下面的图不需要 API 密钥：

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

预期消息内容为 `Hello, NeoGraph!`。Python 还提供 Program 编译和执行、Hooks、运行时上下文要求、严格配置、SQLite 持久化，以及从确切检查点恢复。类型化提供方使用 `ProviderMessage` 和 `make_provider_request`，随后调用 `prepare`/`dispatch` 或 `invoke`；outcome 保留由原生对象持有的完成或失败证据。这些消息与图的便利类型 `ChatMessage` 值不同。阻塞式提供方调用会释放 GIL；`asyncio.to_thread` 可以将它们移出事件循环线程。完整的提供方和 Program 输入见 [Python 示例](bindings/python/examples/README.md)。

## 适用工作负载与限制

NeoGraph 适用于具有明确状态转换、分支或循环、并行任务、检查点和人工审阅的工作流，也可以在 C++ 应用中嵌入小型固定图。节点计算仍由应用负责：图运行时不训练模型，也不替代数值计算库；模型调用仍受提供方的延迟、可用性和费用约束。

运行时支持图级和节点级重试策略、容量受限的可复用节点缓存、`Send` 扇出、`Command` 路由、子图、状态历史、分叉、HITL 和 `NodeInterrupt`。MCP、A2A、ACP、gRPC 和可观测性集成是可选组件。运行时上下文和 Hooks 可以要求特定的分派输入，并记录交付证据；这些检查不能证明模型关注了每一个 token。持久化原生恢复和受限分叉需要原有的共享记账权限；复制快照不能重新赋予预算。

衡量工作负载时，应使用其实际节点、提供方、存储、并发度和构建配置。[基准测试](benchmarks/README.md)和[性能指南](docs/performance-deep-dive.md)描述的是已测量的配置及其限制，而不是普遍适用的速度保证。使用单配置构建测量优化执行性能时，应指定 `CMAKE_BUILD_TYPE=Release`。`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` 在受支持的编译器上加入针对宿主机器的调优；构建需要分发的二进制文件时，应保持关闭。

## 构建配置与延伸阅读

| 选项 | 用途 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 显式指定 SDK 源码检出目录；优先于已安装软件包的查找和源码获取。 |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | 未安装软件包时，获取锁定版本的公共 SDK 源码归档；默认开启。离线构建时关闭。 |
| `NEOGRAPH_BUILD_PROGRAM` | Program 运行时、目录、谱系和迁移；默认关闭。 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 嵌入式 QuickJS Program 编写接口；默认关闭。 |
| `NEOGRAPH_BUILD_PYBIND` | Python 扩展；默认关闭。 |
| `NEOGRAPH_BUILD_LLM` | NeoGraph 模型调用适配器；关闭它们不会移除 SDK 依赖。 |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | 可选的持久化存储；两者默认开启。 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP 客户端和服务端组件。 |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | 协议集成；gRPC 默认关闭。 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 在优化配置中启用不可移植的宿主专用调优；默认关闭。 |

使用已安装软件包的项目只需链接已启用且实际需要的组件：

```cmake
find_package(SchemaProvider 0.1.1 CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

### 本地 CI 验证（Windows 与 WSL）

从源码根目录使用 Python 3.10+、CMake/CTest 和事先配置好的对应工具链、依赖运行。Windows 使用 VS2022 x64 developer PowerShell，设置 vcpkg transport toolchain 的 `CMAKE_TOOLCHAIN_FILE`、专用 `VCPKG_INSTALLED_DIR`，并将 OpenSSL/curl runtime 工具加入 `PATH`。WSL 需要 C++20 compiler、pkg-config、libpq、SQLite、OpenSSL 和 HTTP/2 curl 开发包。Linux 的 `native-linux` 与 `asan` 还需 `psql` 和指向可连接的 **破坏性测试专用** 数据库的 `NEOGRAPH_TEST_POSTGRES_URL`。在选定 Python 中准备 pytest/pydantic/certifi；`native-linux` 还需 `a2a-sdk[http-server]>=1.1,<2`、`agent-client-protocol==0.12.1`、uvicorn 和 httpx。

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

`--work-dir` 必须是归你所有且尚不存在的新输出路径，不能是源码根目录或其祖先；`--jobs` 必须为正数或 `auto`（CPU 数）。既有输出会被拒绝并保留，不会自动清理。Runner 不安装依赖或改变宿主策略；既有固定依赖获取和 cibuildwheel 声明的 bootstrap/repair 仍然适用。

| Profile | 保留的目的 / 前提 |
|---|---|
| `native-linux` | 完整 native PostgreSQL gate 后串行执行全 Python/protocol suite 和无 DB 的 ACP durable 重跑。 |
| `native-posix` | 实际 Linux ARM/macOS suite；无需测试服务的 PostgreSQL build/link 验证。 |
| `native-windows` | MSVC 无 DB 的 native/Program/QuickJS suite 和独立 C embedding ABI smoke；在 x64 MSVC 环境中默认用 Ninja 构建，也可用 `--generator "Visual Studio 17 2022"` 切换。 |
| `asan` | Linux ASan/UBSan/LSan、11 个示例、全 Python suite；需 GCC libasan/libstdc++。 |
| `tsan` | 独立 Linux TSan suite 和 5 个示例；获准的进程级 `setarch -R`，保留既有 suppression。 |
| `msvc-asan` | 串行 Windows Program/QuickJS canary；需激活的 `cl >=19.50`（VS2026），不可使用 MSVC 19.44。 |
| `grpc` | gRPC graph contract；配置 gRPC/Protobuf compiler 和 library。 |
| `benchmark` | Linux Release 四 workload 回归 gate；保留既有吞吐、延迟、目标 RSS 边界。 |
| `quickjs-performance` | Linux 匹配 enabled/disabled build；不可变 provenance 要求实际源码根目录 Git checkout。 |
| `fuzz` | Linux Clang/libFuzzer 60 秒 canary；将 corpus 复制到自有输出。 |
| `install` | 隔离 exported-prefix ABI/symbol/C++/C11/collision/relocation consumer；无需 Git/Bash。可选 `--shared`、`--core-only` 或 `--program`；`--quickjs` 需 `--program` 和 ELF/Mach-O 检查（Windows 行省略）。 |
| `sdist` | 源码 archive 与 Twine；配置 build/twine/scikit-build-core>=1.0/pybind11==2.13.6/ninja>=1.10；可选 `--release-tag v0.13.1`。 |
| `wheel` | repair 后的 installed wheel；配置 cibuildwheel==2.23.0 与 native/container provider；必须指定 `--arch x86_64\|aarch64\|arm64\|AMD64`；指定 `--python cp312` 则只构建一个 CPython 而非全部。 |
| `runtime-archive` | Shared SDK native-archive portability target/CTest；需 Ninja 和平台 runtime 依赖。 |

本地 Windows/WSL 结果不能替代实际 ARM/macOS 或 VS2026 sanitizer 行。每次 push 和 pull request 运行 `ci.yml`：Linux、macOS、Windows 的完整 native suite 加 2 个 installed-consumer 行，仅改文档时跳过。`ci-extended.yml` 每晚及按需运行：sanitizer、fuzz canary、性能 gate、真实 ARM64、gRPC、Visual Studio generator 和其余 8 个 installed-consumer 行。发布候选请在打 tag 前运行一次。`wheels.yml` 在 packaging 变更时为每个平台构建 CPython 3.12，在发布 tag、每周 canary 和手动运行时构建 CPython 3.9–3.13，并保留 glibc 2.34/macOS 14 下限、完整 installed-wheel test、cold-loader/LGPL replacement gate、4 个 native wheel/archive 平台，以及受保护的 tag/OIDC publication 依赖。源码审查或 CLI help 并非执行或发布证据。

- [概念与图语义](docs/concepts.md)
- [C++ 参考](docs/reference-en.md)和 [Python 绑定指南](docs/python-binding.md)
- [异步指南](docs/ASYNC_GUIDE.md)和[并发与取消](docs/concurrency.md)
- [运行时上下文与严格介入](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md) 和 [QuickJS 编写接口](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [迁移指南](docs/migration-v0.4-to-v1.0.md)和[故障排查](docs/troubleshooting.md)
- [C++ 示例](examples/README.md)、[Python 示例](bindings/python/examples/README.md)和[基准测试方法](benchmarks/README.md)

## 许可证

MIT；见 [LICENSE](LICENSE)。第三方声明见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
