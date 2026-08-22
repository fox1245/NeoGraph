<!-- neograph-i18n: source=docs/concepts.md locale=zh-CN source_sha256=3d95cddd2a9d9ff0c7b8028968a5bfab4c44b404af3eff0115f8edfb25a7f1cc -->
# NeoGraph 核心概念——叙事指南

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

进入示例之前请先阅读本文。它按照您自己构建心智模型的顺序来建立该模型：图 → 通道 → 节点 → 边 → fan-out → 路由覆盖 → 检查点 → 流式处理。

代码示例采用 Python 侧编写，因为其更简洁；所有内容均与 C++ API 一一对应（有关类签名，请参阅 [`reference-en.md`](reference-en.md) 以及 `include/neograph/` 下的公共头文件）。

> **如果你之前用过 LangGraph：** 这些原语有意保持一致——带 reducer 的通道、发出写入的节点、条件边、`Send`、`Command`、检查点。README 总结了 NeoGraph 的[两个运行时层](../README.md#two-runtime-layers)。下面的叙述不假设任何前提。

---

## 目录

（第 8.5 节在 v0.6.0 中添加——`Tracing — OpenTelemetry + Phoenix / Langfuse`。编号标题保持 1-9 以保持外部文档链接稳定；8.5 位于 Streaming 和 Common pitfalls 之间。）


1. [整体概览](#1-the-big-picture)
2. [通道与 reducer](#2-channels--reducers)
3. [节点](#3-nodes)
4. [边与条件路由](#4-edges--conditional-routing)
5. [Send — 动态 fan-out](#5-send--dynamic-fan-out)
6. [Command — 路由覆盖 + 状态补丁](#6-command--routing-override--state-patch)
7. [检查点、中断、HITL](#7-checkpoints-interrupts-hitl)
8. [流式事件](#8-streaming-events)
9. [常见陷阱](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 总体概览

一个 NeoGraph **图**由四部分组成：

| 部分 | 它是什么 | 由……定义 |
|---|---|---|
| **通道** | 共享状态中的命名槽位。每个都有一个归约器，定义新写入如何与现有值组合。 | `definition["channels"]` |
| **节点** | 读取状态、发出写入（并可选择`Send` / `Command`）的函数。 | `definition["nodes"]` |
| **边** | 静态下一节点指针。 | `definition["edges"]` |
| **条件边** | 谓词驱动的路由——基于状态从多个下一节点中选择一个。 | `definition["conditional_edges"]` |

执行是一个**超步循环**：

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

超步是并行、检查点和流式事件的单位。两个可以在“现在”同时运行的节点属于同一个超步；它们观察到相同的输入状态，其写入在步骤结束时通过归约器组合。

---

<a id="2-channels--reducers"></a>
## 2. 通道与归约器

每一份状态都存在于一个命名通道中。通道跨节点和超步持久存在；节点通过写入通道进行通信。

### 定义通道

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 内置归约器

| 归约器 | 新写入语义 | 典型用途 |
|---|---|---|
| `"overwrite"` | 新值替换旧值。并行写入时后写者胜。 | 单值暂存（当前节点、当前问题、路由提示）。 |
| `"append"` | 新列表（必须是列表！）被级联到现有列表。顺序：先前步骤的值在前，本步骤的写入按节点执行顺序追加。 | 对话消息、搜索结果、fan-out 收集。 |

> 两个 reducer 都在引擎启动时于 `ReducerRegistry::ReducerRegistry()` 中注册（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。自定义 reducer 通过 C++ 的 `ReducerRegistry::register_reducer(name, fn)` 或 Python（自 v0.1.9 起）注册：
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Python 可调用对象在 GIL 下运行；并发 Send fan-out 会像 Python 自定义节点一样在其上串行化。重新注册名称会替换之前的 reducer。

### 写入通道

节点返回一个 `ChannelWrite` 列表：

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

值的形状必须与 reducer 匹配：
- `"append"` → 必须是列表（将被拼接）。
- `"overwrite"` → 任何可 JSON 序列化的值。

### 从节点读取状态

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)` 返回通道的当前值，如果通道存在但尚未被写入，则返回 `None`。对于聊天消息的类型化访问，`state.get_messages()` 返回 `list[ChatMessage]`（从 `messages` 通道解析）——由 `llm_call` 内部使用。

### 版本

每个通道携带一个单调递增的 `version` 编号。引擎将其用于检查点差异比较和 `state.channel_version(name)` 检查 API。你通常不会直接读取它。

---

<a id="3-nodes"></a>
## 3. 节点

注册节点类型的三种方式，按控制程度递增排列：

### 3.1 内置节点

| `type`（JSON 格式） | 功能 | 配置 |
|---|---|---|
| `llm_call` | 调用 `provider->complete_async(messages, tools)` 并将助手消息追加到 `messages`。 | 读取`provider`、`model`、`instructions`、`tools`自`NodeContext`。 |
| `tool_dispatch` | 查看最新助手消息的 `tool_calls`，通过 `Tool::execute` 执行每一项，追加 `{role: "tool", tool_call_id, content}` 结果。 | 读取 `tools` 从 `NodeContext`. |
| `intent_classifier` | LLM 将用户意图分类为 N 个标签之一，并将所选标签写入 `__route__`。与 `route_channel` 条件配对使用。 | `extra_config: {labels, prompt_template}` |
| `subgraph` | 将另一个图嵌入为单个节点。内部状态通过配置的键映射进行映射。 | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` 装饰器（仅限 Python）

定义只写节点的最短方式：

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

被装饰的函数必须返回一个 `list[ChannelWrite]`（或 `None`，视为 `[]`）。它不能发出 `Send` 或 `Command`——对于这些情况，请继承 `GraphNode`。

### 3.3 完整的 `GraphNode` 子类

重写 `run(input)` 以获得完全控制。它于 v0.4.0 中引入，并且从 v0.9.0 起是唯一的自定义节点入口点——一个方法，一个签名：

```python
class Researcher(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # input.state    — read channels via input.state.get(...)
        # input.ctx      — RunContext (cancel_token, thread_id, step, ...)
        # input.stream_cb — non-None when running in streaming mode
        topic = input.state.get("topic")
        result = await_llm(topic, cancel_token=input.ctx.cancel_token)
        return ng.NodeResult(
            writes=[ng.ChannelWrite("findings", [result])],
            command=ng.Command(goto_node="evaluator"),  # optional
            sends=[],                                    # optional
        )
```

Python 暴露了 `cancel_token`, `thread_id`, `step`, `stream_mode`, `store`，以及 `resume_value` 在 `input.ctx`上。C++ 调用者可以在 `deadline` 和 `trace_id` 上设置 `RunMetadata`；引擎会将它们传播到嵌套子图中。这两个字段尚未通过 Python 绑定暴露。

您也可以返回一个裸的 `list[ChannelWrite]` 当您不需要 `Send` 或 `Command` — 绑定时会自动将其提升为一个 `NodeResult` 。

> **从 v0.3.x 迁移：** 已移除的 pre-v0.4 多入口节点 API 有一个替代方案：重写 `run(input)`。从 `input.state` 读取状态，当非 None 时通过 `input.stream_cb` 发出令牌，并从 `input.ctx.cancel_token` 读取取消令牌。

注册该类型，以便JSON加载器可以实例化它：

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

工厂会看到 `(name, per-node config, NodeContext)`，因此同一个类可以在多个名称下以不同配置实例化。

### 3.4 工具（独立概念，由 `tool_dispatch` 使用）

`Tool` 不是节点——它是 `tool_dispatch` 调用的东西。继承 `ng.Tool`，重写三个方法，将实例传入 `NodeContext(tools=[…])`：

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

引擎在编译时获得工具列表的所有权——你的本地引用之后可以释放。

---

<a id="4-edges--conditional-routing"></a>
## 4. 边与条件路由

### 静态边

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

来自同一源节点的多条边fan-out（每个后继者进入下一个超级步骤的ready集合）。从一个超级步骤到同一目标的两条边会去重为对目标的一次执行。

### 条件边

条件边运行一个**命名条件**，并从 `routes` 映射中选择下一个节点：

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名称解析为引擎中注册的`ConditionFn`。两个作为内置项提供：

| 条件 | 返回值 | 何时使用 |
|---|---|---|
| `has_tool_calls` | `"true"` 如果最新的助手消息具有非空的 `tool_calls`; `"false"` 否则。 | ReAct 循环 — 持续调度工具，直到LLM停止请求。 |
| `route_channel` | `__route__`通道中的任何字符串；回退到`"default"`。 | 与`intent_classifier`配对使用，以实现显式意图路由。 |

自定义条件通过C++的`ConditionRegistry::register_condition(name, fn)`或Python（自v0.1.9起）注册：

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

可调用对象接收实时的`GraphState`（因此`state.get(channel)`和`state.get_messages()`可以工作），并且必须返回一个与条件边的`routes`键之一匹配的字符串。

### 两种等效形式——自 v0.1.8 起均可用

条件边可以位于`edges`数组内部（带有`condition`字段）**或**在单独的`conditional_edges`块中。两种形式都被接受；选择更清晰的一种：

```python
# Form A — top-level (LangGraph parity, recommended for Python)
"edges":             [{"from": "__start__", "to": "llm"}, ...],
"conditional_edges": [{"from": "llm", "condition": "...", "routes": {...}}]

# Form B — inline (used by every C++ example)
"edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "llm", "condition": "...", "routes": {...}},
]
```

> **历史：** 在v0.1.8之前，形式A被图编译器静默丢弃——README和每个Python示例都使用它，因此ReAct循环退化为单次LLM调用。在提交`e23a523`中修复。如果你在≤0.1.7的wheel上看到此问题，请升级。

---

<a id="5-send--dynamic-fan-out"></a>
## 5. 发送 — 动态fan-out

`Send`适用于下一步节点数量取决于状态的情况。经典用法：将搜索主题列表拆分为N个并行的研究者调用。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

引擎的 `run_sends_async` 实例化 `researcher` 每个 `Send`一次，每个都有其自己的 `state.get("topic")`，并通过 `asio::experimental::make_parallel_group` 并行运行它们。

### 心智模型

`Send(target, payload)`是“用此状态补丁实例化`target`并将其添加到就绪集合”。在目标看到`state`之前，负载作为状态写入被应用。

并行组完成后，下一个超步的路由来自每个 Send 派生的任务的外出边（或如果它发出了一个 `Command.goto`，则来自该边）。

### 常见形态：fan-out 5，fan-in至汇总器

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher` 的出边就是 `{"from": "researcher", "to": "summarizer"}` —— 与静态边相同的去重规则，因此 summarizer 只运行一次。

### 工作线程数调优

`build()` 默认为 `EngineConfig::worker_count == 1` —— 无引擎拥有的线程池，fan-out 分支在协程自身的执行器上内联分发。这是一条零分配快速路径，对顺序图成本低廉，且对持有非线程安全状态的节点是安全的。

要实现真正的并行，请显式选择加入一个池。精确选择 N 以匹配您的 fan-out 宽度，或使用 `set_worker_count_auto()` 来获取 `hardware_concurrency()`（回退值为 4）：

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

当多 Send（或多出边）fan-out 在未选择线程池的情况下运行时，NeoGraph 会发出一次性 stderr 警告，使静默串行的情况不会在雷达下溜走。若你有意驱动串行 fan-out（例如对 worker=1 快速路径进行基准测试），可用 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` 抑制该警告。

---

<a id="6-command--routing-override--state-patch"></a>
## 6. Command — 路由覆盖 + 状态补丁

`Command` 允许节点在同一个返回值中决定下一步去向并变更状态。它绕过常规出边。

```python
class Evaluator(ng.GraphNode):
    def run(self, input):
        if score(input.state) >= 0.8:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="summarizer",
                    updates=[ng.ChannelWrite("verdict", "accepted")],
                ),
            )
        else:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="planner",                  # loop back
                    updates=[ng.ChannelWrite("retries",  input.state.get("retries", 0) + 1)],
                ),
            )
```

### Command与条件边的使用场景

- **条件边**：路由依赖于不需要使用节点逻辑的状态谓词。更清晰、声明式。
- **Command**：路由依赖于最自然地在节点内部编写的逻辑 — 多标准评分、内容检查、重试决策。也是原子性更新状态并同时选择下一个节点的唯一方式。

### fan-in 下后写者胜出

若多个 Command 在同一超级步骤中触发（罕见 —— 仅当多个并行组兄弟节点发出时可能发生），则最后一个生效。顺序由并行组完成情况决定，这是非确定性的 —— 设计时应确保最多一个兄弟节点发出 `Command`。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. 检查点、中断、HITL

### 设置检查点存储库

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

附加存储后，每个超级步骤都会以 `(thread_id, checkpoint_id)` 为键向存储写入一个检查点。`RunResult.checkpoint_id` 字段是最新的一个。

### 静态中断点

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

引擎返回一个 `RunResult`，其中 `interrupted=True` 和 `interrupt_node` 已设置。要恢复：

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### 通过 `NodeInterrupt` 进行动态中断

从节点主体内部抛出（Python：`raise ng.NodeInterrupt(reason)`，C++：`throw NodeInterrupt(...)`）。引擎捕获、持久化状态、返回一个在抛出节点处中断的 `RunResult` —— 使用相同的恢复 API。

当暂停决策取决于中间节点输出时非常有用（例如“LLM 是否产生了值得展示给人类的东西？”）。

### 时间旅行

`engine.fork(thread_id, from_checkpoint_id)` 返回一个从过去检查点开始的新线程。适用于“如果我当时回答不同会怎样”的分支。

---

<a id="8-streaming-events"></a>
## 8. 流式事件

`run_stream` / `run_stream_async` 在事件触发时调用回调。模式是可按位 OR 的位掩码：

| 模式 | 触发 |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN` 针对来自 `Provider` 的每个流式令牌 |
| `DEBUG` | `__routing__` 事件，显示下一就绪集合 |
| `VALUES` | `__state__` 事件，包含每个超级步骤后的完整状态 |
| `UPDATES` | `CHANNEL_WRITE` 每个 `ChannelWrite` 的事件 |
| `ALL` | 以上所有 |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **注意：** `event.node_name`（而非 `event.node`）。C++ 结构体字段为 `node_name`；pybind 保留原始名称。

对于聊天形式的流式传输（兼容 LangChain 的消息字典，带有增量 `content_so_far`），请使用辅助函数：

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 放置（C++）

当从 C++ 驱动 `engine.run_stream_async()` 时，外部 `asio::io_context.run()` 应从应用程序的主线程（或任何已通过正常进程启动路径初始化的长生命周期线程）调用。已验证良好的形态：

```cpp
// Main-thread driver — what examples/40 and the SchemaProvider tests use.
asio::io_context io;
asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await engine->run_stream_async(cfg, cb);
}, asio::detached);
io.run();
```

```cpp
// Dedicated worker thread driver — also fine.
std::thread t([&]() {
    asio::io_context io;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(cfg, cb);
    }, asio::detached);
    io.run();
});
t.join();
```

> **已知限制——在HTTP服务器工作线程回调中嵌套`io.run()`**（问题#16）：如果嵌套执行`asio::io_context.run()`，并将其置于`httplib::Server::set_chunked_content_provider`内部，或者置于通过`Provider::complete_stream_async`默认桥接创建子线程的等效逐请求工作线程回调中，某些glibc/OpenSSL组合会在`getaddrinfo`中出现SEGV。仓库内测试（[`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp)）覆盖了这一结构并能稳定通过，但测试套件无法完整复现下游环境中的真实HTTPS `api.openai.com`、TSan/ASan下的glibc解析器以及并发请求负载。**解决方法：**
>
> 1. **在 HTTP 服务器回调内部使用 `co_await provider->complete_async(...)` 而非 `complete_stream_async`**，并从辅助函数中将组装好的回复作为单个 `LLM_TOKEN` 事件发出。令牌类型 UX 会丢失；引擎 + 节点 + 工具循环可端到端工作。这是 ProjectDatePop 的下游 `cpp_backend` 目前使用的方案。
> 2. **将 `io.run()` 移出每请求回调**：在专用工作线程上为引擎运行一个长生命周期 `asio::io_context`，将每请求工作排队到其上，并将结果发布回 HTTP 服务器的响应接收器。避免与 SEGV 相关的每请求嵌套 `std::thread` 生成。

---

## 8.5. 追踪——OpenTelemetry + Phoenix / Langfuse

与流式传输相同的回调形态，不同的消费者。将 OTel 追踪器发射回调传入 `engine.run_stream(cfg, cb)`，每个 `NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` 事件都会成为跨度。

两个层级随货内置：

  - `neograph_engine.tracing.otel_tracer` — 供应商中立的 OTel 跨度。跨度流向任何 OTel 后端（Jaeger、Tempo、Honeycomb、Datadog）。
  - `neograph_engine.openinference` — LLM 形态属性层，可将相同的跨度在 Phoenix / Arize / Langfuse 中转换为 *LangSmith 风格的聊天气泡追踪*：

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer

trace.set_tracer_provider(TracerProvider())
trace.get_tracer_provider().add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
tracer = trace.get_tracer("my-app")

# Wrap the provider — every Provider.complete() now emits an LLM-kind span.
wrapped = OpenInferenceProvider(real_provider, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)
```

启动一次 Phoenix：`docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`。打开 http://localhost:6006 — 追踪呈现为链（`graph.run` → `node.X` → `llm.complete`），提示词 / 响应 / token 计数在 LLM 详情面板中可见。相同代码，将 OTLP 端点 URL 替换为 Langfuse 自托管，追踪即以相同形状显示在那里。

这就是对*“NeoGraph没有LangSmith”*的回应——你可以通过一条Docker命令本地运行Phoenix或Langfuse来获得LangSmith的UX（聊天气泡、DAG层级、token成本）。无需SaaS合约，无单次追踪计费。

参见 `docs/reference-en.md` §10.5 了解属性键模式以及 `otel_tracer` 与 `openinference_tracer` 之间的权衡说明。

---

<a id="9-common-pitfalls"></a>
## 9. 常见陷阱

这些均已被真实用户遇到；从 [`docs/troubleshooting.md`](troubleshooting.md) 交叉引用。

### “我的ReAct循环只运行一次”

你使用的是 wheel ≤ 0.1.7。图编译器静默丢弃了 `conditional_edges` 块。升级到 ≥ 0.1.8。使用 `result.execution_trace == ['llm', 'dispatch', 'llm']` 验证（不仅仅是 `['llm']`）。

### “Provider调用挂起60秒然后报错”

你使用的是 wheel ≤ 0.1.6。捆绑的 OpenSSL 硬编码了 RHEL CA 路径，这些路径在 Ubuntu / Debian / macOS 上不存在。升级到 ≥ 0.1.7（导入时自动将 `SSL_CERT_FILE` 设置为 certifi 的捆绑包）或手动设置 `SSL_CERT_FILE`。

### “我的fan-out比我预期的要慢”

`compile()` 默认为 `set_worker_count(1)` （无引擎拥有的线程池——fan-out 分支在调用方的执行器上串行运行）。如需真正的并行，请调用 `engine.set_worker_count(N)` ，其中 N 与您的 Send fan-out 宽度匹配，或 `engine.set_worker_count_auto()` 用于 `hardware_concurrency()`。NeoGraph 还会在首次多 Send fan-out 在未选择加入池的情况下运行时，向 stderr 打印一次性警告——这是提示，而非错误。Python 自定义节点在小型 fan-out 上会遇到 GIL 争用，因此请同时使用 1 和 N 进行基准测试。

### “Python RunResult 没有 .status / .final_state 属性”

Python绑定不暴露这些属性。请使用`result.output`、`result.interrupted`、`result.max_steps_exhausted`和`result.execution_trace`。C++调用方可使用`RunResult::status()`获得类型化的`Completed` / `Interrupted` / `StepLimit`视图。参见[Python绑定指南](python-binding.md#hitl-and-state)。

### “Unknown reducer: <name>”

内置两个reducer：`overwrite`和`append`。请在编译前通过C++的`ReducerRegistry::register_reducer`或Python的`ng.ReducerRegistry.register_reducer`注册自定义reducer。

### “条件已注册，但我的条件边没有触发”

验证表单是加载器接受的表单（[§4](#4-edges--conditional-routing) 中的表单 A 或表单 B）— 自 v0.1.8 起两者均可用。在较旧的 wheel 上，仅表单 B 可用。

### execution_trace 只显示起始节点

路由回退至`__end__`。最可能的原因是起始节点缺少边，或者您的条件返回了不在`routes`映射中的值，且显式`"default"`路由指向`__end__`。严格图不再按映射顺序选择路由：开放或未指定的条件在声明时使用`"default"`，否则引擎会抛出包含源节点、条件和返回标签的错误。封闭条件若返回其声明标签之外的值，则始终抛出错误。

---

## 下一步去哪里

- [Python 示例](../bindings/python/examples/) — 21 个自包含脚本，涵盖上述所有概念。
- [C++ 示例](../examples/) — 36 个结构相同的程序。
- [`reference-en.md`](reference-en.md) — 逐类详尽的 API。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 深入探讨异步/协程层。
