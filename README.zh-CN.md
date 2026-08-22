<!-- neograph-i18n: source=README.md locale=zh-CN source_sha256=6ba467cfa403c387e0a433c35a7d0002d1579850b8820d50544b399c8cadb239 -->
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

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --parallel
./build/example_core_quickstart
```

完整源代码位于[examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)。它注册一个 C++ 节点，编译一个严格图，运行该图，并读取一个类型化通道。

在需要时启用可编程控制平面：

```bash
cmake -S . -B build-program \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

参见[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp)和[QuickJS 编写边界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)。

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

## Python

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

仅使用 Core 的用户不需要为 Program 或 QuickJS 付费：

```bash
cmake -S . -B build-core \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

重要选项：

| 选项 | 用途 |
|---|---|
| `NEOGRAPH_BUILD_PROGRAM` | 持久化 Program 值、目录、运行时、血缘及迁移 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program 编写及生成器命令 |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python 扩展 |
| `NEOGRAPH_BUILD_SQLITE` | SQLite 检查点、上下文、Hook 及提供方回执存储 |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL 检查点及 Program 持久化组件 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP 客户端与服务器角色 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | 可选协议集成 |

使用与你的部署匹配的窄 CMake 目标：`neograph::core`、`neograph::llm`、`neograph::program`、`neograph::mcp`、`neograph::a2a`，或其他已启用的组件。

## 验证

该仓库运行确定性的 C++ 和 Python 测试套件、Program 重放/迁移探针、DSL 能力夹具、文档/i18n 检查、消毒器，以及可选的实时模型评估。基准声明应归于 [benchmarks](benchmarks/README.md) 和带日期的 [性能报告](docs/performance-deep-dive.md)，而非作为永恒的 API 保证。

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
