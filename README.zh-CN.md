<!-- neograph-i18n: source=README.md locale=zh-CN source_sha256=73d8153a6b0ecc5957982362ae8429663ac2730be7c65242cb1c4b1031fc170c -->
<p align="center">
<h1 align="center">NeoGraph</h1>
  <p align="center">
<strong>一个快速的C++图运行时，带有持久化的可编程智能体控制平面。</strong><br>
当延迟至关重要时，采用静态 Core 执行。当控制至关重要时，采用 QuickJS Programs、子智能体、Hook、运行时上下文和经过验证的拓扑演化。
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
<a href="#quick-start">快速入门</a> &middot;
<a href="#two-runtime-layers">架构</a> &middot;
<a href="#python">Python</a> &middot;
<a href="examples/README.md">示例</a> &middot;
<a href="docs/reference-en.md">C++ 参考手册</a> &middot;
<a href="docs/python-binding.md">Python 参考手册</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo-v3.mp4">
    <img src="docs/images/neograph-promo-v3.gif" alt="NeoGraph — generated Programs, semantic admission, runtime topology, Hooks, context and Python parity" width="900">
  </a>
</p>

## NeoGraph 如今是什么

NeoGraph 有两个刻意分离的执行层：

| 层 | 用于 | 契约 |
|---|---|---|
| **GraphEngine / Core** | 固定或宿主选择的图，低开销，嵌入式部署 | 不可变的编译拓扑；C++ 节点通过 Pregel 风格的 super-steps 执行 |
| **ProgramRuntime / QuickJS** | 运行时控制、子Program、结构化并发、拓扑替换与迁移 | Program 的不可变代次；持久的类型化命令；日记化状态转变与重放 |

模型永远不会获得编译器、目录、凭据、迁移或授权授予访问权限。生成的源代码遵循：

```text
proposal → reserve → compile → semantic validate → admit → publish → migrate or spawn
```

被拒绝的提案无法发布`ProgramVersion`，且其动态编译预算不会恢复。参见[严格运行时插桩](docs/STRICT_RUNTIME_INTERPOSITION.md)和[DSL 能力评估](docs/DSL_CAPABILITY_EVAL.md)。

<a id="quick-start"></a>
## 快速入门

### C++ Core

即使 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 也已是必需的外部 C++ 依赖，因为 Core 公开拥有所有权的 typed provider 契约。请安装 SDK runtime package，将安装 prefix 设为 `SCHEMAPROVIDER_PREFIX`；下方 configure 使用 `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`。也可用 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` 明确指定 checkout。不会自动选择猜测的 sibling checkout 或旧 bundled interpreter。当前 SDK runtime/archive 支持 Linux/POSIX；不承诺无依赖、无需 OpenSSL、native Windows/macOS 或 WASM runtime。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
cmake --build build --parallel
./build/example_core_quickstart
```

完整源代码位于[examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)。它注册一个 C++ 节点，编译一个严格图，运行该图，并读取一个类型化通道。

在需要时启用可编程控制平面：

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

参见[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp)和[QuickJS 编写边界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)。

### 性能构建

Ninja 和 Unix Makefiles 等单配置生成器在 `CMAKE_BUILD_TYPE` 为空时不会
选择优化级别。NeoGraph 会对此配置发出警告，因为 GCC/Clang 会在没有
Release 的 `-O3 -DNDEBUG` 标志时编译 QuickJS 和 NeoGraph。

在 GCC 或 Clang 上进行本机性能构建：

```bash
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build-performance --parallel
```

`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` 会在优化配置中加入
`-march=native -mtune=native`。它能提升本机吞吐量，但会使产物不可移植；
分发二进制时请关闭它。Release 加固默认保持启用。

在 GCC/Clang 上，最终的 Release 配置对 QuickJS 使用 C11、对 NeoGraph
使用 C++20 和 `-O3 -DNDEBUG`。默认加固包含
`-D_GLIBCXX_ASSERTIONS`、`-fstack-protector-strong`、
`-fcf-protection=full`、Linux 的 `-D_FORTIFY_SOURCE=2` 以及
RELRO/NOW 链接。默认不启用 LTO 或本机特定调优。

<a id="two-runtime-layers"></a>
## 两个运行时层

### GraphEngine / Core

- 静态和条件边、循环、屏障、`Send` fan-out 和 `Command` 路由；
- 检查点/恢复、精确检查点恢复、fork、状态历史、HITL 和 `NodeInterrupt`；
- 同步与协程 API、流式处理、取消与 token 核算；
- 图级与节点级重试策略、jitter与有界可复用节点缓存；
- 自定义注册表、提供者、工具、MCP、A2A 与 ACP 集成；
- 安全点捕获与形状保持的 GraphEngine 生成迁移。

### ProgramRuntime / QuickJS

- 在受限 QuickJS `define()` 和生成器 `main(input)` 中的标准 JavaScript 计算；
- 密封命令：`callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint`、`cancelScope`，以及被准入(admission)的主机能力；
- 不可变 Program 包、版本、目录、准入(admission)配置与策略快照；
- 持久化命令日志、精确重放、子代系谱、不可续期预算与进程恢复；
- 检查点替换与受限的实时 GraphEngine 拓扑迁移；
- 在准入(admission)生成的 Program 之前，进行主机方的语义验证。

已安装的 JavaScript 表面可通过 `javascript_authoring_capability_manifest()` 进行机器读取，并在 CI 中对照实际的 QuickJS 绑定进行检查。

## 运行时安全与上下文

NeoGraph 将重要行为移出模型自由裁量范围：

- 不可变的 RAW 消息历史与 `ContextEpoch` 选择；
- 派生上下文、必需 Skills 与硬约束；
- 保守的转换收据，精确保留必需工件；
- 在原生、stdio 或 HTTP 执行后端上的强制生命周期 Hooks；
- 提供方分发与终端结果收据；
- 持久的运行时开发者指令与已准入(admission)的拓扑转换。

NeoGraph 保证构建、准入(admission)、分发与证据边界。它不声称 LLM 处理了每个 token。
## Typed C++ provider 调用

`SchemaProvider` 接收获准的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 及可选 `SchemaProvider::Defaults`。descriptor 是 closed/versioned 数据 admission，不是请求/响应 interpreter 或任意 primitive registry。credential 应放在 runtime options，而非公开 descriptor。Defaults 仅包含 typed OpenRouter routing 和 Responses 保留 (`responses_store`)，后者仅适用于 Responses。Hosted OpenRouter routing、retention、JSON 格式仍是声明的 typed 控制。Images、Veo、Decisions 使用独立的 NeoGraph typed client 和独立授权，不继承 SDK chat grant。

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

提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、完整 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。使用量是带依据、阶段、质量的 nullable `uint64_t`；缺失表示未知，绝不是零。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

`ChatMessage` / `ChatTool` 和 JSON 只是 portable projection，不是 native 权限。当前格式为 [`provider-message-v2`](schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](schemas/runtime-history-record-v2.schema.json)。真实 C++ 内存 checkpoint sidecar 无需 archive 即可保留 native seal。持久 native 历史和 bank 引用需要真实 `sp::NativeArchive`：closed v2 / `spna2` 是使用独立密钥、经过认证的 owner-private 受保护 custody，不是加密或 vendor-issuer 认证。不得公开 archive 正文、密钥、native blob 或 raw wire 观测。managed 恢复/fork 共享 charged/reserved/report/dedup canonical bank，不更新预算。通用有界持久 fork 必须使用外部 host-shared bank/journal，复制 snapshot 不能授予独立支出权限。


实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为不稳定 `0.0.0`、interface revision 3 / shared ABI 3，使用 out-of-line capability check，不表示稳定发布。当前 runtime/archive 为 Linux/POSIX，不代表 Windows、macOS、WASM runtime 已获验证。Python provider binding/wrapper 已延期，不由本 C++ 变更完成移植。

## Python

> 下方 Python 资料描述既有 binding；provider binding/wrapper 已明确延期，未针对 typed lossless C++ 切换移植或执行。安装历史 wheel 不会提供新的 C++ provider API。
Python 包使用相同的 C++ 引擎，现包含 Program、Hook、strict-context、运行时策略与 SQLite 持久化接口：

```bash
pip install neograph-engine
```

### 五秒演示（无需 API 密钥）

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

Python 额外公开：

- `RetryPolicy`、按节点的运行时覆盖、`RunMetadata`、精确的 `resume_from` 以及可复用的缓存作用域；
- `ProgramSource`、`ProgramRegistryBuilder`、`ProgramCompiler`、`LocalProgramHost`、句柄和结果；
- 强制的 `HookRuntime` 回调以及失败时关闭的生命周期投递；
- `RuntimeContextRequirements`、`ContextTransformReceipt`、SQLite 持久化上下文/分发存储，以及 `StrictRuntimeProfile`。

参见 [Python 绑定指南](docs/python-binding.md) 和 [Python 示例](bindings/python/examples/README.md)。

## 构建配置

Core-only 构建仍可省去 Program/QuickJS，但不能省去 SchemaProvider runtime：

```bash
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

重要选项：

| 选项 | 用途 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 明确指定 SDK source checkout；未指定时必须安装 runtime package。 |
| `NEOGRAPH_BUILD_PROGRAM` | 持久化 Program 值、目录、运行时、血缘及迁移 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program 编写及生成器命令 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 为优化配置选择不可移植的本机指令调优 |
| `NEOGRAPH_WARN_ON_UNOPTIMIZED_SINGLE_CONFIG` | 单配置构建缺少 `CMAKE_BUILD_TYPE`、可能遗漏 Release 优化标志时发出警告 |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python 扩展 |
| `NEOGRAPH_BUILD_SQLITE` | SQLite 检查点、上下文、Hook 及提供方回执存储 |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL 检查点及 Program 持久化组件 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP 客户端与服务器角色 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | 可选协议集成 |

使用与你的部署匹配的窄 CMake 目标：`neograph::core`、`neograph::llm`、`neograph::program`、`neograph::mcp`、`neograph::a2a`，或其他已启用的组件。

SDK imported target 提供 `include/SchemaProvider` include root；公开示例直接使用 `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>`，不依赖 recipe 专用 helper。

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

## 验证

`scripts/test_find_package.sh` 描述 installed-consumer 检查，文件存在不代表当前已通过。当前 SDK ABI3 全量重建/CTest 已通过 26/26；shared 安装 consumer 实际执行 local HTTP 两 turn typed 请求、tool/native/refusal/known-zero 结果及 mismatch 拒绝。这不代表 NeoGraph、Python、Windows、macOS、WASM 或付费 live-provider 兼容已验证。NeoGraph 集成验证另行报告。

## 文档

- [概念](docs/concepts.md)
- [C++ 参考](docs/reference-en.md)
- [Python 绑定](docs/python-binding.md)
- [并发与取消](docs/concurrency.md)
- [异步指南](docs/ASYNC_GUIDE.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [QuickJS 公共创作边界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [严格运行时插桩](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [故障排除](docs/troubleshooting.md)
- [示例](examples/README.md)

## 许可证

MIT — 参见 [LICENSE](LICENSE)。第三方声明：[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
