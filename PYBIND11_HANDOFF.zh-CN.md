<!-- neograph-i18n: source=PYBIND11_HANDOFF.md locale=zh-CN source_sha256=ef50f7757126e94faa6a671e4cc22a02c5c49481f94e6d0652d812d1c4734b07 -->
# Pybind11 绑定——下一会话交接

**Languages:** [English](PYBIND11_HANDOFF.md) | [한국어](PYBIND11_HANDOFF.ko.md) | [日本語](PYBIND11_HANDOFF.ja.md) | [简体中文](PYBIND11_HANDOFF.zh-CN.md)

> **历史交接：** 本文档描述的绑定已发布。当前自定义 Python 节点实现
> `run(input)`；旧的 `execute*` 节点接口已在 v0.9.0 中移除。请参阅
> `docs/python-binding.md` 和 `docs/migration-v0.4-to-v1.0.md` 获取
> 受支持的 API。

下一 NeoGraph 会话的实时计划。下次会话还应阅读的姊妹文档/上下文：

- `README.md` — 架构、构建选项、BUILD_SHARED_LIBS 部分（新增于 2026-04-25）
- 记忆：`~/.claude/projects/-root-Coding-NeoGraph/memory/project_neograph.md`
  （项目概述）、`plan_pybind11_binding.md`（设计说明）以及更广泛的
  MEMORY.md 索引。

## TL;DR — 从这里接手

1. **设计 + 第一次提交（~2–4 小时）**：最小 pybind11 模块，暴露
   `Provider`、`Tool`、`GraphEngine::compile`、`RunConfig`、`RunResult`。
   目标：Python 脚本可以调用 `compile(json_def, ctx).run(config)` 并对
   JSON 状态进行往返。
2. **自定义 Python 节点（~2–4 小时）**：pybind11 跳板，使
   `neograph.GraphNode` 的 Python 子类能够实现 `run(input)`，并且引擎
   在适当的 GIL 处理下分发到 Python 中。
3. **Wheel 打包（~1–2 天）**：manylinux + macOS arm64 + Win wheels，
   携带 `libneograph_*.so` 加上绑定。与刚刚落地的共享库工作（提交
   85619e6）协同——wheel 安装本质上是动态链接的，因此 SHARED 是正确模式。

## 为什么这很重要

卖点是 *"在一个 import 中获得 LangGraph 级的 Python 易用性 + NeoGraph 微秒延迟"*。
今天 NeoGraph 用户有两个选择：全程编写 C++（对大多数以 Python 为主流的
agent 开发者是一个真正的障碍）或完全忽略 NeoGraph。Pybind11 无需分叉引擎
即可弥合这一差距。

与纯 LangGraph 相比的具体优势：
- **引擎开销降低 130×–640×** — 见 README.md "引擎开销 vs Python 图/管道
  框架" 基准测试。NeoGraph 每次超级步骤 5 µs 对 LangGraph 的 656 µs 在
  绑定层完整保留（pybind11 分发在亚微秒级）。
- **`pip install neograph`** — 无需 Docker、无 venv 地狱。自包含 wheel
  附带捆绑的 .so。
- **LangChain 用户已熟悉的 Python 原语** — `state.get("messages")`、
  `ChannelWrite("findings", ...)` 等。
- **迁移路径** — LangGraph 用户可以在不重写其工具集成或 LLM 客户端代码
  的情况下更换其引擎。

## 今天的上下文（2026-04-25）

本次会话落地、绑定将建立在其之上的内容：

- **BUILD_SHARED_LIBS 支持**（提交 `85619e6`，后续 `110a5fb`）：
  NeoGraph 现在在 Linux/macOS 上构建为 `.so`/`.dylib`。RPATH
  （`$ORIGIN`/`@loader_path`）干净地接线。330/330 ctest 通过。
  下游 re-agent 已验证。
- **`re-agent` 仓库**（`fox1245/re-agent`，私有，第三阶段已关闭）：
  并行扇出 + 检查点 + 恢复 + 双后端，约 750 行 C++ 单文件。Pin 升级到
  NeoGraph `110a5fb`，提交 `2d57787`。
- **`re-agent-reimpl` 仓库**（`fox1245/re-agent-reimpl`，私有，
  第四阶段已关闭）：全新重实现的 agent。测试驱动循环在 4 轮迭代 /
  $0.025 中收敛到 crackme01 的 10/10。

两个下游仓库都是私有的——它们是个人用途的逆向工程工具。
Pybind11 工作严格属于 NeoGraph（公开）。

## Pybind11 设计

### 待暴露的接口（提交 1）

通过薄编组的只读映射。JSON → `py::dict` 在边界发生，而非在引擎内部。

```python
import neograph
from neograph.llm import OpenAIProvider

provider = OpenAIProvider(api_key="sk-...", default_model="gpt-4o-mini")

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": "__start__", "to": "llm"},
        {"from": "llm", "to": "__end__"},
    ],
}

ctx = neograph.NodeContext(provider=provider)
engine = neograph.GraphEngine.compile(definition, ctx)

result = engine.run({"messages": [{"role": "user", "content": "Hi"}]})
print(result.output["channels"]["messages"]["value"])
```

待包装的符号：

| C++ 类型 / 函数 | Python | 说明 |
|---|---|---|
| `neograph::Provider` | `neograph.Provider`（抽象） | 基类。子类 `OpenAIProvider`、`SchemaProvider` 暴露在 `neograph.llm` 中。 |
| `neograph::CompletionParams` | `neograph.CompletionParams` | 纯数据类。 |
| `neograph::ChatCompletion` | `neograph.ChatCompletion` | 纯数据类。 |
| `neograph::Tool` | `neograph.Tool`（抽象） | 对于纯 Python Tool 子类使用跳板（提交 2）。 |
| `neograph::graph::NodeContext` | `neograph.NodeContext` | 可通过 kwargs 构造（`provider=`、`tools=`、`model=`、`instructions=`）。 |
| `neograph::graph::GraphEngine` | `neograph.GraphEngine` | 静态 `compile(definition, ctx, store=None)` + 实例 `run(input)` / `run_async(input)`。 |
| `neograph::graph::RunConfig` | `neograph.RunConfig` | 可构造，`input` 接受 dict。 |
| `neograph::graph::RunResult` | `neograph.RunResult` | `.output` 暴露嵌套字典。 |
| `neograph::graph::ChannelWrite` | `neograph.ChannelWrite` | 纯数据类。 |
| `neograph::graph::Send` | `neograph.Send` | 纯数据类。 |
| `neograph::graph::Command` | `neograph.Command` | 纯数据类。 |
| `neograph::graph::GraphState` | `neograph.GraphState` | 从自定义节点内部访问的只读视图。`.get(channel)` 根据 JSON 类型返回 `dict`/`list`/`str`/`int`。 |

### 自定义 Python 节点（提交 2）

跳板模式，使 `neograph.GraphNode` 的 Python 子类能够插入 C++ 调度器：

```python
class MyAnalyzeNode(neograph.GraphNode):
    def __init__(self, provider):
        super().__init__()
        self.provider = provider

    def run(self, input):
        target = input.state.get("target_function")
        # ...do work in Python, possibly calling self.provider.complete(...)
        return [neograph.ChannelWrite("findings", [proposal])]

# Register so the JSON definition can reference it by type name
neograph.NodeFactory.instance().register_type(
    "analyze",
    lambda name, json, ctx: MyAnalyzeNode(ctx.provider),
)
```

GIL 处理——*最棘手的部分*。两条规则：

1. **不调用 Python 的 C++ 代码释放 GIL**：
   `engine->run(...)` 和 `engine->run_async(...)` 用
   `py::gil_scoped_release` 包裹，因此其他 Python 线程在引擎运行时保持运行。
2. **调用 Python 节点的 C++ 代码获取 GIL**：
   节点跳板用 `py::gil_scoped_acquire` 包裹每次分发，在调用 Python
   `run` 方法前获取，返回时释放。

引擎已经在 `fan_out_pool_` 工作线程上运行 Send 分支（在 re-agent 的
`set_worker_count` 工作中添加——见 `graph_engine.cpp:60-62` 的约定）。
每个工作线程将调用 Python 并需要自己的 GIL 获取。Pybind11 的
`gil_scoped_acquire` 可重入地处理这一点。

### Wheel 打包（提交 3+）

- 使用驱动 CMake 调用的 `cmake-build-extension` 或 `scikit-build-core`
  的 `pyproject.toml`。`scikit-build-core` 是现代默认，约 30 行配置。
- 在 wheel 构建中强制 `BUILD_SHARED_LIBS=ON`，使捆绑的
  `libneograph_*.so` 兄弟文件与绑定 `.so` 协同工作。设置
  RPATH `$ORIGIN`（已在 NeoGraph CMakeLists 中配置）。
- CI 中的 `cibuildwheel` 矩阵，用于 manylinux2014（x86_64、aarch64）、
  macOS（universal2）和 Windows。注意 Windows DLL 导出尚未接线
  （在配置时警告）——Windows 行**推迟**到 `NEOGRAPH_API` 宏通过完成后。

## 首次提交形态——最小 pybind11 模块

在提交 1 中落地的目标：

```
bindings/
  python/
    CMakeLists.txt         ← pybind11_add_module(...)
    src/
      module.cpp           ← root PYBIND11_MODULE
      bind_provider.cpp
      bind_graph.cpp
      bind_state.cpp
    pyneograph/
      __init__.py          ← re-exports + version
    tests/
      test_smoke.py        ← compile + run a 2-node graph end-to-end
```

根 `CMakeLists.txt` 中的 CMake 集成：

```cmake
option(NEOGRAPH_BUILD_PYBIND "Build Python bindings (pybind11)" OFF)
if(NEOGRAPH_BUILD_PYBIND)
    add_subdirectory(bindings/python)
endif()
```

默认 OFF，不影响现有 C++ 构建。Wheel 构建通过 scikit-build 配置将其
切换为 ON。

## 待与用户确认的开放性问题

这些是在提交 1 落地前需确认的决策：

1. **仓库位置** — 绑定放在 NeoGraph 内（`bindings/python/`）还是
   作为单独的姊妹仓库（`pyneograph`）？
   - 在仓库内：更紧密的反馈循环、单一真源、版本始终与引擎匹配。
   - 独立仓库：PyPI 发布独立于 NeoGraph 标签切割。
   - 推荐：**在仓库内**。发布面保持可管理。

2. **PyPI 上的包名** — `neograph`（简洁，但如他人占用则有命名空间抢占
   风险）还是 `neograph-engine` / `pyneograph`（更安全）？
   - 推荐：先检查 PyPI，若 `neograph` 空闲则使用。

3. **异步 API 暴露** — 将 `engine.run_async()` 暴露为 Python 中的
   `async def`（返回 awaitable）还是仅暴露同步 `run()`？
   - 异步需要 asio↔asyncio 桥接 → 更复杂。提交 1 可以仅同步。

4. **自定义节点 API 易用性** — 使用 `class MyNode(GraphNode)` 跳板模式，
   还是使用 `@neograph.node` 装饰器包装纯函数？装饰器更 Pythonic 但丢失
   对 `Command` / `Send` 发送的访问。
   - 推荐：**两者兼备**。跳板作为主要方式，装饰器作为常见仅写入情况的
     语法糖。

## 不在 pybind11 提交范围内的事项

- **Anthropic / Gemini Provider** — 这些是 NeoGraph 侧的工作，不是绑定
  工作。如果它们绑定落地时在 C++ 中尚不存在，则它们不显示在
  `neograph.llm` 中，直到存在为止。
- **MCP 客户端绑定** — `neograph::mcp::MCPClient` 从 Python 中很有用，
  但有子进程生成怪癖，需要仔细的 GIL 处理。推迟到提交 4+。
- **Postgres 检查点绑定** — Python 用户已经有 `psycopg2` /
  `asyncpg`。包装 `PostgresCheckpointStore` 重复了这一点。SQLite
  检查点绑定值得做，因为线路格式去重很重要，且 SQLite 没有相同模式的
  Python 等价物。

## 预估总工作量

- 提交 1（基本接口）：2–4 小时
- 提交 2（Python 自定义节点 + GIL）：2–4 小时
- 提交 3（Wheel 打包，仅 Linux）：4–8 小时
- 提交 4+（cibuildwheel 矩阵、MCP 绑定、异步）：1–2 天
- **到 "pip install neograph" 在 Linux 上可用的总计**：1–2 天
- **到多平台 wheels 的总计**：3–5 天

## 验证交接

在下一会话接手时，进行健全性检查以确认上游状态未漂移：

```bash
cd /root/Coding/NeoGraph
git log --oneline -5
# expect: 110a5fb cleanup, 85619e6 BUILD_SHARED_LIBS, c7ee23e split, ...
git status                  # should be clean
ls build-shared-test/lib*.so 2>/dev/null && echo "shared build cached" \
                              || echo "rebuild needed: cmake -S . -B build-shared-test -DBUILD_SHARED_LIBS=ON -DNEOGRAPH_BUILD_TESTS=ON && cmake --build build-shared-test -j"
```

然后从上述设计问题开始，落地提交 1。
