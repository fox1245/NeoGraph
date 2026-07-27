<!-- neograph-i18n: source=docs/concepts.md locale=zh-CN source_sha256=0972b9d4c384152233869c6375839d3a0469e17124fb375a7352ed453ce486ae -->
# NeoGraph 核心概念——叙事指南

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)



在深入示例之前先阅读一遍本文。它按照你自己构建思维模型的顺序构建思维模型：图→通道→节点→边→扇出→路由覆盖→检查点→流。

代码示例是 Python 端的，因为它们更简洁；一切都 1:1 映射到 C++ API（类签名见 [`reference-en.md`](reference-en.md)，生成参考见 [Doxygen](https://fox1245.github.io/NeoGraph/)）。

> **如果你用过 LangGraph：** 这里的基本原语有意保持相同：
> 带归约器的通道、发出写入的节点、条件边、`Send`、`Command`、
> 检查点。差异见 README 的
> [与 LangGraph 对比](../README.md#vs-langgraph)。
> 下面的叙述不假设你已有背景。

---

## 目录

（第 8.5 节在 v0.6.0 中添加 —`Tracing — OpenTelemetry + Phoenix / Langfuse`。编号标题保持 1-9，以保持外部文档链接稳定； 8.5 位于流式传输和常见陷阱之间。）


1. [大局观](#1-the-big-picture)
2. [通道和归约器](#2-channels--reducers)
3. [节点](#3-nodes)
4. [边和条件路由](#4-edges--conditional-routing)
5. [Send — 动态扇出](#5-send--dynamic-fan-out)
6. [命令-路由覆盖+状态补丁](#6-command--routing-override--state-patch)
7. [检查点、中断、HITL](#7-checkpoints-interrupts-hitl)
8. [流式事件](#8-streaming-events)
9. [常见陷阱](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 大局观

一个 NeoGraph **图**有四件事：

|事物|它是什么|定义为|
|---|---|---|
|**通道**|处于共享状态的命名槽。每个都有一个归约器，定义新写入如何与现有值结合。| `definition["channels"]` |
|**节点**|读取状态、发出写入的函数（以及可选的`Send` / `Command`）。| `definition["nodes"]` |
|**边**|静态下一个节点指针。| `definition["edges"]` |
|**条件边**|谓词驱动的路由——根据状态选择几个下一个节点之一。| `definition["conditional_edges"]` |

执行是一个**超级步循环**：

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

超级步是并行性、检查点和流事件的单位。可以“立即”运行的两个节点是相同的超级步；当步骤结束时，他们观察到相同的输入状态，并且他们的写入通过归约器组合。

---

<a id="2-channels--reducers"></a>
## 2. 通道和归约器

每个状态都存在于一个命名通道中。通道跨节点和跨超级步持续存在；节点通过写入来进行通信。

### 定义通道

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 内置归约器

|归约器|新的写入语义|典型用途|
|---|---|---|
| `"overwrite"` |新值替换旧值。最后写入者在并行写入中获胜。|单值暂存（当前节点、当前问题、路由提示）。|
| `"append"` |新列表（必须是列表！）连接到现有列表。顺序：先上一步的值，此步骤按节点执行顺序写入附加内容。|对话消息、搜索结果、扇出集合。|

>两个归约器都注册在`ReducerRegistry::ReducerRegistry()`
>引擎启动时（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。
>自定义归约器通过 C++ 注册`ReducerRegistry::register_reducer(name, fn)`
>或来自 Python（自 v0.1.9 起）：
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
>Python 可调用运行在GIL;并发发送扇出
>以与 Python 自定义节点相同的方式对其进行序列化。重新注册
>名称取代了以前的归约器。

### 写入通道

一个节点返回一个列表`ChannelWrite`s:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

值的形态必须与归约器匹配：
- `"append"`→ 必须是一个列表（将被连接）。
- `"overwrite"`→ 任意JSON- 可序列化的值。

### 从节点读取状态

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)`返回通道的当前值，或者`None`如果通道存在但尚未写入。对于聊天消息的键入访问，`state.get_messages()`返回`list[ChatMessage]`（解析自`messages`通道）——内部使用`llm_call`。

### 版本

每个通道都承载一个单调的`version`数字。引擎使用它来进行检查点比较和`state.channel_version(name)`检查API。你通常不会直接阅读它。

---

<a id="3-nodes"></a>
## 3. 节点

注册节点类型的三种方法，按控制顺序递增：

### 3.1 内置节点

| `type`（在JSON）|它的作用|配置|
|---|---|---|
| `llm_call` |调用 `provider->complete_async(messages, tools)`并将助理消息附加到`messages`。|读`provider`, `model`, `instructions`, `tools`从`NodeContext`。|
| `tool_dispatch` |查看最新的助理消息`tool_calls`，通过 `Tool::execute` 执行每个调用, 附加`{role: "tool", tool_call_id, content}`结果。|读`tools`从`NodeContext`。|
| `intent_classifier` | LLM将用户意图分类为 N 个标签之一，并将所选标签写入`__route__`。与 `route_channel` 条件配合使用。| `extra_config: {labels, prompt_template}` |
| `subgraph` |将另一个图嵌入为单个节点。内部状态通过配置的键重新映射进行映射。| `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 的`@ng.node`装饰器（仅限 Python）

定义只写节点的最短方法：

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

修饰函数必须返回`list[ChannelWrite]`（或者`None`，视为`[]`）。它不能发射`Send`或者`Command`- 对于那些，子类`GraphNode`。

### 3.3 完整`GraphNode`子类

覆盖`run(input)`以实现完全控制。它在 v0.4.0 中引入，是 v0.9.0 以后唯一的自定义节点入口点——一个方法，一个签名：

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

Python 在 `input.ctx` 上暴露 `cancel_token`、`thread_id`、`step`、`stream_mode`、
`store` 和 `resume_value`。C++ 调用方可以在 `RunMetadata` 中设置 `deadline` 和
`trace_id`，引擎会将它们传播到嵌套 subgraph。这两个字段目前仍未暴露给 Python 绑定。

你也可以返回裸 `list[ChannelWrite]`当你不需要的时候`Send`或者`Command`— 绑定将其提升到`NodeResult`自动地。

>**从 v0.3.x 迁移：** 已移除的 v0.4 之前多入口节点 API 只有一种替代方式：
>覆盖 `run(input)`。从 `input.state` 读取状态，非 None 时通过
>`input.stream_cb` 发出令牌，并从 `input.ctx.cancel_token` 读取取消令牌。

注册类型，以便JSON loader 可以实例化它：

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

工厂看到`(name, per-node config, NodeContext)`因此同一个类可以用不同的配置以多个名称实例化。

### 3.4 工具（单独的概念，由 `tool_dispatch` 使用）

`Tool`不是一个节点——它是供 `tool_dispatch` 调用的对象。子类`ng.Tool`，重写三个方法，将实例传入`NodeContext(tools=[…])`：

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

引擎在编译时获得工具列表的所有权——之后你的本地引用可能会被删除。

---

<a id="4-edges--conditional-routing"></a>
## 4. 边和条件路由

### 静态边

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

来自同一源节点的多个边呈扇形散开（每个后继者都进入下一个超级步的就绪集）。从一个超级步到同一目标的两条边将重复数据删除到目标的一次执行。

### 条件边

条件边运行**命名条件**，并从 `routes` 映射中选择下一个节点：

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名称解析为`ConditionFn`已在引擎中注册。内置两个条件：

|条件|返回|何时使用|
|---|---|---|
| `has_tool_calls` | `"true"`如果最新的助手消息非空`tool_calls`; `"false"`否则。| ReAct循环 — 继续调度工具，直到LLM 停止请求。|
| `route_channel` |无论字符串中是什么`__route__`通道;回落至`"default"`。|配对`intent_classifier`用于显式意图路由。|

自定义条件通过 C++ 注册`ConditionRegistry::register_condition(name, fn)`或来自 Python（自 v0.1.9 起）：

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

可调用对象接收实时 `GraphState`（所以`state.get(channel)`和`state.get_messages()`工作）并且必须返回与条件边之一匹配的字符串`routes`键。

### 两种等效形式 — 均自 v0.1.8 起生效

条件边可能位于`edges`数组（带有`condition`字段）**或**在单独的`conditional_edges`块。两种形式均被接受；选择更清晰的一个：

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

>**历史：** 之前，形式 A 已被图形编译器默默删除
>v0.1.8——README每个 Python 示例都使用了它，所以ReAct循环
>退化为单次 LLM 调用。已在提交中修复`e23a523`。如果你
>在 ≤ 0.1.7 的轮子上看到这个，升级。

---

<a id="5-send--dynamic-fan-out"></a>
## 5.Send — 动态扇出

`Send`适用于下一步节点数量取决于状态的情况。经典用法：将搜索主题列表拆分为 N 个并行的研究人员调用。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

引擎的`run_sends_async`实例化`researcher`每个 `Send`，每个都有自己的`state.get("topic")`，并通过 `asio::experimental::make_parallel_group` 并行运行它们。

### 心智模型

`Send(target, payload)`是“实例化`target`使用此状态补丁并将其添加到就绪集中”。在目标看到之前，有效负载将作为状态写入应用`state`。

并行组完成后，下一个超级步的路由来自每个Send 生成的任务的传出边（或其`Command.goto`，如果它发出了一个）。

### 常见形态：扇出 5、扇入到摘要器

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`的出边就是`{"from": "researcher", "to": "summarizer"}`— 与静态边相同的重复数据删除规则，因此摘要器运行一次。

### 工作器数量调优

`build()`默认为`EngineConfig::worker_count == 1`— 没有引擎拥有的线程池，扇出分支在协程自己的执行器上内联调度。这是一条无分配的快速路径，对于顺序图来说成本低廉，对于保持非线程安全状态的节点来说是安全的。

对于真正的并行性，请明确选择加入池。准确选择 N 来匹配你的扇出宽度，或使用`set_worker_count_auto()`使用 `hardware_concurrency()`（回退为 4）：

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

当多发送（或多输出边）扇出在没有选择加入池的情况下运行时，NeoGraph发出一次性 stderr 警告，因此静默串行案例不会悄悄溜过。使用`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`如果你有意驱动串行扇出（例如，worker=1 快速路径的基准）。

---

<a id="6-command--routing-override--state-patch"></a>
## 6.Command — 路由覆盖 + 状态补丁

`Command`让节点决定下一步去哪里，并且在相同的返回值中改变状态。它绕过常规的传出边。

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

### 何时使用命令与条件边

- **条件边**：路由取决于状态谓词
不需要节点逻辑。更干净、声明式。
- **命令**：路由取决于最自然编写的逻辑
节点内部——多标准评分、内容检查、重试决策。也是原子地更新状态并选择下一个节点的唯一方法。

### 扇入下的最后写入者获胜

如果多个命令在同一个超级步中触发（很少见 - 仅当多个并行组兄弟发出它们时才可能），则最后一个获胜。该顺序由并行组完成确定，这是不确定的——围绕这一点进行设计，确保最多有一个兄弟发出`Command`。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7.检查点、中断、HITL

### 设置检查点存储

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

附加存储后，每个超级步都会向关键的存储写入一个检查点`(thread_id, checkpoint_id)`。`RunResult.checkpoint_id`字段是最新的。

### 静态中断点

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

引擎返回 `RunResult`，其中 `interrupted=True` 且设置了 `interrupt_node`。恢复：

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### 动态中断通过`NodeInterrupt`

从节点体内抛出（Python：`raise ng.NodeInterrupt(reason)`，C++：`throw NodeInterrupt(...)`）。引擎捕获、保留状态、返回`RunResult`在抛出节点处中断 - 相同的恢复API。

当暂停的决定取决于中间节点输出时很有用（例如“LLM是否产生了值得展示给人类的内容？”）。

### 时间旅行

`engine.fork(thread_id, from_checkpoint_id)`返回一个从过去的检查点开始的新线程。对于“如果我的回答不同会怎样”分支很有用。

---

<a id="8-streaming-events"></a>
## 8. 流式事件

`run_stream` / `run_stream_async`当事件触发时调用回调。模式是可进行“或”运算的位掩码：

|模式|发出|
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN`对于来自 a 的每个流式令牌`Provider` |
| `DEBUG` | `__routing__`显示下一个准备就绪的事件|
| `VALUES` | `__state__`每个超级步后具有完整状态的事件|
| `UPDATES` | `CHANNEL_WRITE`每个 `ChannelWrite` 事件 |
| `ALL` |上述全部|

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

>**注意：**`event.node_name`（不是`event.node`）。 C++ 结构体字段
>是`node_name`; pybind 保留原始名称。

对于聊天型流式传输（LangChain- 与增量兼容的消息字典`content_so_far`），使用助手：

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()`放置位置 (C++)

驱动`engine.run_stream_async()`从 C++ 开始，外部`asio::io_context.run()`应该从应用程序的主线程（或任何已通过正常进程启动路径初始化的长寿命线程）调用。经测试良好的形态：

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

>**已知限制——嵌套`io.run()`里面一个HTTP服务器工作器
>回调**（问题#16): 嵌套`asio::io_context.run()`里面一个
> `httplib::Server::set_chunked_content_provider`（或同等
>每个请求的工作器回调本身通过以下方式生成子线程
> `Provider::complete_stream_async`的默认网桥）已被观察到
>到SEGV在`getaddrinfo`在一些 glibc / OpenSSL 组合上。这
>仓库内测试
>（[`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp)）
>覆盖结构形态并正常通过，但下游
>环境（真实的`api.openai.com`超过HTTPS, glibc 解析器
>在 TSan / ASan 下，并发请求负载）并不详尽
>能由测试套件完全复现。 **解决方法：**
>
>1. **使用`co_await provider->complete_async(...)`而不是
>    `complete_stream_async`从里面HTTP服务器回调**，以及
>将组装好的回复作为一个发出`LLM_TOKEN`事件来自
>辅助函数。令牌输入用户体验丢失；引擎+节点+工具循环工作
>端到端。这就是ProjectDatePop的下游`cpp_backend`
>今天使用。
>2. **移动`io.run()`在每个请求回调之外**：运行一个
>长期存在的`asio::io_context`在专用工作线程上
>引擎，将每个请求的工作排队，将结果发回
>进入HTTP服务器的响应接收器。避免了每个请求
>嵌套的`std::thread`产生SEGV相关联。

---

## 8.5。追踪 — OpenTelemetry + Phoenix / Langfuse

与流式传输相同的回调形态，不同的消费者。将 OTel 跟踪器发射回调传递到`engine.run_stream(cfg, cb)`和每`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT`事件成为一个span。

仓库内提供两层：

  - `neograph_engine.tracing.otel_tracer`— 供应商中立的 OTel
span。 Span 流向任何 OTel 后端（Jaeger、Tempo、Honeycomb、Datadog）。
  - `neograph_engine.openinference` — LLM-shape属性层
将相同的span变成 Phoenix / Arize / Langfuse 中的 *LangSmith 风格聊天气泡 trace*：

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

启动一次 Phoenix：`docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix`。打开http://localhost:6006— trace呈现为一条链（`graph.run` → `node.X` → `llm.complete`），提示/响应/令牌计数可见LLM详细信息窗格。相同的代码，交换一下OTLP端点 URL对于 Langfuse 自托管，trace以相同的形态显示在那里。

这就是*的答案”NeoGraph没有LangSmith“* — 你得到LangSmith用户体验（聊天气泡，DAG通过使用一个 Docker 命令在本地运行 Phoenix 或 Langfuse 来实现层次结构、令牌成本）。没有 SaaS 合同，没有按跟踪定价。

看`docs/reference-en.md`§10.5 属性键模式和之间的权衡说明`otel_tracer`和`openinference_tracer`。

---

<a id="9-common-pitfalls"></a>
## 9. 常见陷阱

这些都是真实用户所击中的；交叉引用自 [`docs/troubleshooting.md`](troubleshooting.md)。

### “我的ReAct循环只运行一次”

你的wheel 包 ≤ 0.1.7。图形编译器删除了`conditional_edges`默默地阻止。升级至 ≥ 0.1.8。验证与`result.execution_trace == ['llm', 'dispatch', 'llm']`（不仅`['llm']`）。

### “提供者调用挂起 60 秒，然后出现错误”

你的wheel 包 ≤ 0.1.6。捆绑的 OpenSSL 硬编码RHELUbuntu / Debian / macOS 上不存在的 CA 路径。升级到 ≥ 0.1.7（自动设置`SSL_CERT_FILE`到导入时的 certifi 捆绑包）或设置`SSL_CERT_FILE`手动。

### “我的扇出比我预期的要慢”

`compile()`默认为`set_worker_count(1)`（没有引擎拥有的线程池 - 扇出分支在调用者的执行器上串行运行）。对于真正的并行调用`engine.set_worker_count(N)`其中 N 与你的发送扇出宽度匹配，或者`engine.set_worker_count_auto()`使用 `hardware_concurrency()`。NeoGraph第一次在没有选择加入池的情况下运行多发送扇出时，还会打印一次性 stderr 警告 - 这是一个提示，而不是错误。 Python自定义节点参见GIL小扇出的争用，因此 1 和 N 都进行基准测试。

### “PythonRunResult没有 .status / .final_state 属性”

Python 绑定不会公开这些属性。使用`result.output`, `result.interrupted`, `result.max_steps_exhausted`， 和`result.execution_trace`。 C++ 调用者可以使用`RunResult::status()`对于打字的`Completed` / `Interrupted` / `StepLimit`视图。请参阅表中的README的“读取输出”部分。

### “未知归约器：<name>”

内置两个归约器：`overwrite`和`append`。自定义归约器需要`ReducerRegistry::register_reducer`来自 C++（还没有 Python 钩子）。

### “条件已注册，但我的条件边未触发”

验证该形式是 loader 接受的形式（来自 [§4 的形式 A 或形式 B](#4-edges--conditional-routing)) — 两者都从 v0.1.8 开始工作。对于较旧的wheel 包，只有形式 B 适用。

### “execution_trace 仅显示起始节点”

路由结果变成了`__end__`。请检查起始节点是否缺少边，或条件是否返回了`routes`中不存在的值，而显式的`"default"`路由又指向`__end__`。严格图不再按 map 顺序选择路由：开放条件或未声明输出契约的条件会使用显式`"default"`；若未声明该路由，则错误信息会包含 source node、条件名和返回的 label。封闭条件返回声明范围外的 label 时也一定报错。

---

## 下一步去哪里

- [Python 示例](../bindings/python/examples/)— 21 个独立的
涵盖上述每个概念的脚本。
- [C++ 示例](../examples/)— 36 个具有相同结构的程序。
- [`reference-en.md`](reference-en.md)— 逐类详尽 API。
- [Doxygen](https://fox1245.github.io/NeoGraph/)— 生成参考
用于 C++ 头文件。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md)— 深入探讨异步/协程
层。
