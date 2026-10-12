<!-- neograph-i18n: source=docs/concepts.md locale=zh-CN source_sha256=3ddbf9b3903df0b61f2694ecfbb12af0682b96f25f81d50b4efc4865c4c576a1 -->
# NeoGraph 核心概念导读

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

建议先读本文，再看示例。各节的顺序就是搭建一张图的顺序：通道、节点、边、fan-out、路由、检查点和流式输出。

用过 LangGraph 的读者，对带 reducer 的通道、`Send`、`Command` 和检查点不会陌生。NeoGraph 的 [Core 与 ProgramRuntime](../README.md#core-and-programruntime) 各有职责，本文从 Core 的图执行讲起。Python 里调用 provider 走带类型的 [绑定契约](python-binding.md)，不再使用已移除的 completion 类。

---

## 目录

（第 8.5 节是 v0.6.0 新增的，标题为 `Tracing — OpenTelemetry + Phoenix / Langfuse`。
编号标题仍然是 1-9，这样外部文档里的链接不会失效；
8.5 位于“流式事件”和“常见陷阱”之间。）


1. [总体概览](#1-the-big-picture)
2. [通道与 reducer](#2-channels--reducers)
3. [节点](#3-nodes)
4. [边与条件路由](#4-edges--conditional-routing)
5. [Send：动态 fan-out](#5-send--dynamic-fan-out)
6. [Command：覆盖路由并修补状态](#6-command--routing-override--state-patch)
7. [检查点、中断与 HITL](#7-checkpoints-interrupts-hitl)
8. [流式事件](#8-streaming-events)
9. [常见陷阱](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 总体概览

一张 NeoGraph **图**由四部分组成：

| 部分 | 含义 | 定义位置 |
|---|---|---|
| **通道** | 共享状态中有名字的槽位。每个通道都有一个 reducer，规定新写入如何与已有的值合并。 | `definition["channels"]` |
| **节点** | 读取状态并发出写入的函数，也可以发出 `Send` 或 `Command`。 | `definition["nodes"]` |
| **边** | 固定的“下一个节点”指向。 | `definition["edges"]` |
| **条件边** | 由谓词决定的路由，根据状态从多个后继节点中选一个。 | `definition["conditional_edges"]` |

执行过程是一个**超步循环**：

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

普通的就绪批次读到的，是这一批更新之前的通道状态。同一批里的兄弟节点，在执行期间看不到彼此返回的写入：引擎先缓存这些写入，等整批跑完，再交给 reducer 合并，然后基于更新后的状态计算路由。这是图的调度机制，引擎不负责同步模型内部的计算。

例如 `counter` 初始为 0，两个就绪节点都返回 `counter + 1`，两者读到的都是 0。overwrite reducer 的结果是 1，而不是 2；换成自定义的求和 reducer，两个值为 1 的增量写入才会合并成 2。每个 `Send`（不论一个还是多个）都在一份隔离的状态副本上运行，副本里已应用该 `Send` 自己的 payload，所以 payload 不会进入共享状态（#389）。reducer 的合并顺序并不能让模型的响应或外部副作用变得可复现。

---

<a id="2-channels--reducers"></a>
## 2. 通道与 reducer

所有状态都放在有名字的通道里。默认情况下，通道的物化值会跨超步、跨检查点保留；节点之间通过写通道来通信。写入如何合并、数组保留多少、是否进入检查点，这三件事相互独立。

### 定义通道

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 内置 reducer

| Reducer | 新写入的语义 | 典型用途 |
|---|---|---|
| `"overwrite"` | 新值替换旧值。并行写入时，最后写入者胜出。 | 存放单个值的暂存区（当前节点、当前问题、路由提示）。 |
| `"append"` | 新写入必须是列表，会拼接到现有列表之后。此前各步的值在前；并行节点按调度器的就绪顺序（不是完成顺序）依次合并，每个节点内部再按其返回的顺序。 | 对话消息、搜索结果、fan-out 的收集结果。 |

> 两个 reducer 都在引擎启动时由 `ReducerRegistry::ReducerRegistry()` 注册（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。自定义 reducer 可以在 C++ 中用 `ReducerRegistry::register_reducer(name, fn)` 注册，也可以在 Python 中注册（自 v0.1.9 起）：
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Python 可调用对象在 GIL 下运行；并发的 Send fan-out 会像 Python 自定义节点一样，在 GIL 上串行执行。对同一个名称重复注册，会替换之前的 reducer。

### 通道的生命周期与检查点契约

reducer 负责合并写入。数组保留是另一项独立的策略，有三种取值：`unbounded`（默认）、`latest`，以及带正整数 `retention_limit` 的 `bounded`。每次写入之后都会按该策略裁剪数组，`ChannelWrite.Mode.Overwrite` 也不例外；`latest` 只保留最后一个元素，直到下一次写入把它替换掉。持久化方式同样独立选择：`checkpoint`（默认，保存物化值和版本）或 `ephemeral`（持久检查点里两者都不保存）。bounded 改变的是能看到的历史，而不只是占用多少存储。

引擎合并写入时，同一节点的多条写入按返回顺序，静态批次按调度器的就绪顺序，多个 `Send` 的结果按调用顺序，与谁先完成无关。待定写入（pending write）重放时，也回到相同的任务槽位。overwrite 是有序的“最后写入者胜出”，append 保持元素顺序。自定义 reducer 应当是纯函数，并且重放时结果稳定。如果要求重新分组后结果不变，reducer 需要满足结合律；只有要求结果与顺序无关，才需要满足交换律。显式 overwrite 会绕过 reducer，之后仍会应用保留策略。以上规则都不保证模型输出或外部副作用可复现。

ephemeral 值在各个超步之间一直有效，不会每步重置。每个检查点会记录声明过的 ephemeral 通道名，以及这些通道是否写入过，但不保存它们的值。恢复运行、`resume_if_exists`、按精确 ID 恢复和状态更新，遇到下列情况都会拒绝：ephemeral 状态已有写入；旧检查点里缺少这个 guard；ephemeral 通道的集合发生了变化。第一次写入 ephemeral 通道之前的检查点仍然可以恢复，其待定写入按文档规定的顺序重放。`update_state` 拒绝写入 ephemeral 通道。多 `Send` 的进程内 worker 会在各自的隔离副本中继承当前的 ephemeral 值。对正确性至关重要的状态，要么放进检查点，要么在新的运行中从持久化的输入重建。

`GraphState::restore` 会拒绝带 ephemeral 通道的图。请改用带匹配 guard 的 `restore_checkpoint`；如果是同一进程内、包含全部当前值和版本的快照，则用 `restore_runtime`。guard 存放在检查点的元数据里，不改变通道 blob 的布局，也不改变存储的 schema。没有 ephemeral 通道的图，仍然可以使用旧的完整值检查点。要降级到不带这些 guard 的二进制之前，应先排空使用 ephemeral 通道的线程和分叉，或者从持久化的输入重新启动它们；旧版本的读取方无法强制执行这个新增的 guard。

进入检查点的通道保存的是完整的物化快照。Memory、SQLite 和 PostgreSQL 都会对没有变化的 `(thread, channel, version)` 值去重，但 append 的历史每次写入都会改变版本，快照仍然会增大。待定写入记录的是未完成超步中已成功的任务，不是通用的通道增量。目前没有“每步重置”的策略：要安全地设计它，必须规定写入和路由之后的重置时机，以及中断、重放和 Send 下的行为。它也不能与 ephemeral 持久化混为一谈。

基于增量（delta）的检查点目前只是设计，不是通道设置。这种格式会在完整快照的基础上，按顺序重放最多 *K* 个 `{channel, version, write mode, value}` 增量（也可以再加字节数阈值）。它必须保留 overwrite、保留策略、版本和 reducer 的标识；在清除待定写入之前，要以原子方式发布快照、增量和检查点指针；遇到缺失的链接、版本跳号、未知的 reducer 或重放失败时，必须拒绝。要采用它，需要新的 schema 版本，并且有实测的收益。迁移时，把现有快照作为基线，不凭空编造增量；在可回退的上线期间，要为旧版本的读取方保留完整快照；如果记录里只有增量，除非原来的 reducer 注册表能把它们物化，否则拒绝降级。当前的存储仍然是完整快照方式。

基线用 `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite` 测量；只有在隔离的本地数据库上，才加上 `postgres` 和 `--pg-url`。每一行报告逻辑序列化字节数、保存和加载的 p50/p95，以及重建深度；legacy 行报告 blob 数量。要统计内存分配请求，运行 `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`。原生的 JSON 和 SQL 分配器，并不都在 C++ `operator new` 的截获范围内。对比时要使用相同的 payload、历史长度和后端配置，依据实测而不是估算的收益。逻辑字节数不等于持久化后实际占用的物理字节数。SQLite 使用退出时删除的唯一临时数据库；`--sqlite-path` 会保留新建的文件，并拒绝已存在的路径。

下面记录的一次 Linux x86-64 Debug 基线，使用 1 个线程、256 个历史步、512 字节的消息，只迭代 1 次。这些是历史测量值，不是性能目标：

| Backend | 逻辑检查点字节数 | 保存 p50/p95 (µs) | 加载 p50/p95 (µs) | 重放深度 |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

在数据库删除之前另测了一次，SQLite 数据库和 WAL 的大小分别为 14,811,136 / 4,210,672 字节（共 19,021,808 字节）。另外用 Linux 的 `LD_PRELOAD` shim 统计了整个进程的 `malloc`、`calloc` 和非零 `realloc` 请求，包含对象构造和 JSON 解析。与 `--history-steps 0` 相比，Memory 多出 88,277 次请求、共 605,289,027 请求字节，SQLite 多出 114,295 次、867,319,964 字节。这些是累计的请求量，不是存活内存，也不只是存储层的分配。对齐分配和内部分配没有截获。这个 shim 不是项目依赖；下结论之前，请使用受支持的 profiler，并在预热后多跑几次。

### 写入通道

节点返回一个 `ChannelWrite` 列表：

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
]
```

值的形状必须与 reducer 匹配：
- `"append"` → 必须是列表（会被拼接）。
- `"overwrite"` → 任何可 JSON 序列化的值。

### 在节点中读取状态

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)` 返回通道的当前值；如果通道存在但还没写入过，返回 `None`。要以带类型的方式访问聊天消息，用 `state.get_messages()`，它返回 `list[ChatMessage]`（从 `messages` 通道解析而来），`llm_call` 内部就是这样用的。

### 版本

每个通道都带一个单调递增的 `version` 编号。引擎用它做检查点的差异比较，也用它提供 `state.channel_version(name)` 这个检查接口。一般不需要直接读它。

---

<a id="3-nodes"></a>
## 3. 节点

注册节点类型有三种方式，控制程度依次提高：

### 3.1 内置节点

| `type`（JSON 中） | 作用 | 配置 |
|---|---|---|
| `llm_call` | 准备一个归自己所有的带类型请求，依次经过 gate、reserve、receipt 处理后，分发同一个句柄，并保留所有按序的消息和完整结果。 | 从 `NodeContext` 读取 `provider`、`model`、`instructions`、`tools`。 |
| `tool_dispatch` | 查看最近一条 assistant 消息的 `tool_calls`，通过 `Tool::execute` 逐个执行，并追加 `{role: "tool", tool_call_id, content}` 形式的结果。 | 从 `NodeContext` 读取 `tools`。 |
| `intent_classifier` | 由 LLM 把用户意图归入 N 个标签之一，并把选中的标签写入 `__route__`。与 `route_channel` 条件配合使用。 | `extra_config: {labels, prompt_template}` |
| `subgraph` | 把另一张图嵌入为一个节点。内部状态按配置的键映射对应起来。 | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` 装饰器（仅限 Python）

这是定义只写入节点的最短写法：

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

被装饰的函数必须返回 `list[ChannelWrite]`（返回 `None` 时按 `[]` 处理）。它不能发出 `Send` 或 `Command`；需要的话，请继承 `GraphNode`。

### 3.3 完整的 `GraphNode` 子类

覆盖 `run(input)` 即可获得完全的控制。它在 v0.4.0 引入，从 v0.9.0 起成为自定义节点唯一的入口：一个方法，一种签名。

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

Python 在 `input.ctx` 上提供 `cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`trace_id`、`run_id`、`model_token_budget`，以及带类型的 provider 证据。检查截止时间用 `has_deadline` 和 `deadline_remaining_ms`；C++ 的 steady-clock 原始值是不透明的。C++ 调用方通过 `RunMetadata` 传入截止时间和 trace 元数据，这些信息会传播到嵌套的子图。

如果不需要 `Send` 或 `Command`，也可以直接返回一个裸的 `list[ChannelWrite]`，绑定层会自动把它提升为 `NodeResult`。

> **从 v0.3.x 迁移：** v0.4 之前的多入口节点 API 已移除，只有一个替代方案：覆盖 `run(input)`。从 `input.state` 读取状态；`input.stream_cb` 不为 None 时，通过它发出 token；取消令牌从 `input.ctx.cancel_token` 读取。

注册这个类型，JSON 加载器才能实例化它：

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

工厂函数拿到的参数是 `(name, per-node config, NodeContext)`，因此同一个类可以用不同的名称和配置实例化多次。

### 3.4 工具（独立的概念，由 `tool_dispatch` 调用）

`Tool` 不是节点，而是 `tool_dispatch` 去调用的对象。继承 `ng.Tool`，覆盖三个方法，再把实例传给 `NodeContext(tools=[…])`：

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
    def execute(self, args):  return str(args["x"] * 2)
```

编译时引擎会接管工具列表的所有权，之后本地持有的引用可以释放。

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

同一个源节点引出多条边时会 fan-out（每个后继节点都进入下一个超步的就绪集合）。同一个超步里有两条边指向同一个目标时，会去重，目标只执行一次。

### 条件边

条件边会运行一个**具名条件**，并从 `routes` 映射里选出下一个节点：

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名会解析为引擎中注册的某个 `ConditionFn`。内置了两个：

| 条件 | 返回值 | 适用场景 |
|---|---|---|
| `has_tool_calls` | 最近一条 assistant 消息的 `tool_calls` 非空时返回 `"true"`，否则返回 `"false"`。 | ReAct 循环：一直分发工具，直到 LLM 不再请求。 |
| `route_channel` | 返回 `__route__` 通道里的字符串；没有时回退为 `"default"`。 | 与 `intent_classifier` 配合，做显式的意图路由。 |

自定义条件可以在 C++ 中用 `ConditionRegistry::register_condition(name, fn)` 注册，也可以在 Python 中注册（自 v0.1.9 起）：

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

可调用对象收到的是当前的 `GraphState`（所以 `state.get(channel)` 和 `state.get_messages()` 都能用），必须返回一个字符串，且与该条件边 `routes` 中的某个键相同。

### 两种等价写法，自 v0.1.8 起都可用

条件边可以写在 `edges` 数组里（带 `condition` 字段），**也可以**写在单独的 `conditional_edges` 块里。两种写法都接受，选更清楚的一种即可：

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

> **历史：** v0.1.8 之前，图编译器会悄悄丢掉写法 A。README 和所有 Python 示例用的恰好都是它，结果 ReAct 循环退化成只调用一次 LLM。该问题已在提交 `e23a523` 中修复。如果在 ≤ 0.1.7 的 wheel 上遇到这种现象，请升级。

---

<a id="5-send--dynamic-fan-out"></a>
## 5. Send：动态 fan-out

`Send` 让节点在运行时决定要调用目标节点多少次，比如每个主题派一个 researcher。引擎先等普通的就绪批次返回、并应用完它们的写入，再执行发出的 Send，这些 Send 仍属于同一个编号的超步。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### 心智模型

每个 `Send` 会让引擎调用一次编译好的目标节点，但不保证每次都是新的节点对象。因此 Send 的目标节点必须能安全处理并发调用下的成员状态。payload 会在目标节点读取通道之前应用。每个 Send（这一步派发的是一个还是多个都一样）都在就绪批次之后的状态副本上运行，这份副本是隔离的，已应用该 Send 自己的 payload。所以 payload 只对该 Send 自己的执行可见（包括它的每一次重试），不会泄漏到下一步，也不会出现在最终状态里（#389）。所有 Send 都结束后，它们返回的写入按调用顺序合并。随后，路由把普通节点和 Send 目标节点发出的信号合在一起，得出下一个就绪批次。

Send 的目标节点抛出 `NodeInterrupt` 时，会像就绪批次中的节点一样暂停运行。它的 `NodeInterrupt` 检查点会重新进入整个超步：检查点里保存的是这一步开始时的状态，并把这一步的就绪节点列为下一批节点，暂停结果报告的也是这份状态。恢复时会把这一步重新跑一遍。已完成的每个就绪节点和每个 Send 直接重放已记录的结果，不会再次运行，也不会再次计费；暂停的那个 Send 以及其他没有完成的 Send，则在相同的 task id 下带着恢复值运行，所以它们之前的尝试已写入日志的 provider 调用会被重放。如果这一步之前没有检查点（例如入口处的 map 节点），已完成的结果就记录在中断检查点本身之下。

### 常见形态：fan-out 出 5 路，再 fan-in 到 summarizer

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher` 的出边就是 `{"from": "researcher", "to": "summarizer"}`，与静态边适用同样的去重规则，所以 summarizer 只运行一次。

### 调整 worker 数量

`build()` 默认 `EngineConfig::worker_count == 1`，因此不会创建引擎自己的线程池，而是在调用方的协程执行器上分发各分支。协程中的 I/O 可以重叠；CPU 密集的工作在单线程执行器上可能变成串行。调用方的执行器是多线程，或者有多个运行同时进行时，节点的成员状态仍然必须是线程安全的。

要真正并行，需要显式选用线程池。N 取与 fan-out 宽度一致的值，或者用 `set_worker_count_auto()`，取 `hardware_concurrency()`（取不到时回退为 4）：

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

多 Send（或多条出边）的 fan-out 在没有选用线程池的情况下运行时，NeoGraph 会向 stderr 输出一次警告，免得默默串行执行的情况没人注意。如果确实要串行 fan-out（例如测试 worker=1 快速路径的基准），可以设置 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` 关闭警告。

---

<a id="6-command--routing-override--state-patch"></a>
## 6. Command：覆盖路由并修补状态

`Command` 让节点在同一个返回值里，既决定下一步去哪里，又修改状态。它会绕过常规的出边。

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
                    updates=[ng.ChannelWrite("retries",  (input.state.get("retries") or 0) + 1)],
                ),
            )
```

### Command 与条件边，何时用哪个

- **条件边**：路由取决于状态上的某个谓词，不需要节点逻辑。更简洁，也是声明式的。
- **Command**：路由取决于最适合写在节点里的逻辑，比如多条件打分、检查内容、决定是否重试。它也是唯一能原子地同时更新状态并选择下一个节点的方式。

### fan-in 时最后写入者胜出

如果多个兄弟节点都返回了非空的 `Command.goto_node`，则取路由顺序中最后一个 command，它会覆盖普通的边和屏障（barrier）。静态批次提供的是就绪顺序；多 `Send` 提供的是调用顺序，而不是完成顺序。所有返回的 command 更新仍然会经过写入管线合并。如果相互冲突的 command 会改变工作流的走向，最好只设一个负责路由决策的节点。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. 检查点、中断与 HITL

### 配置检查点存储

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

挂上存储后，每个超步都会向存储写入一个检查点，键为 `(thread_id, checkpoint_id)`。`RunResult.checkpoint_id` 字段是最新的那一个。

### 静态中断点

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

引擎会返回 `interrupted=True` 且设置了 `interrupt_node` 的 `RunResult`。恢复运行的方法如下：

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

恢复只会跳过它所回应的那个中断的闸门。恢复 `interrupt_before` 暂停，会运行被它拦下的节点；恢复 `NodeInterrupt`，则继续该中断所在的那一步。其他恢复方式，比如 `interrupt_after` 暂停之后的恢复，或进程停止后从某一步的检查点恢复，在即将运行 `interrupt_before` 节点时仍会先暂停。恢复值只交给 `interrupt_before` 暂停所拦下的那个节点，不交给其他节点；`interrupt_after` 暂停之后的恢复，或从其他任何检查点恢复，都不会把恢复值交给任何节点（#393）。同一步里有多个带闸门的节点同时就绪时，要逐个回应（#396）：一次恢复只回应运行所停在的那个闸门；该步运行之前，其余尚未回应闸门的带闸门节点，会各自以自己的 `interrupt_before` 暂停再次让运行停下。该步的所有闸门都回应后，这一步才会运行，每个带闸门的节点收到的是对它自己那个闸门的回应。

### 通过 `NodeInterrupt` 动态中断

在节点函数体内抛出即可（Python：`raise ng.NodeInterrupt(reason)`；C++：`throw NodeInterrupt(...)`）。引擎会捕获它、持久化状态，并返回一个在抛出处的节点中断的 `RunResult`，恢复用的是同一套 API。不论是就绪批次里的节点还是 `Send` 的目标节点，也不论走同步、异步还是流式入口，都是如此。暂停的那一步中已经完成的兄弟节点，恢复时是重放，不会再运行一次；即使这一步之前没有检查点，也一样。

同一步里的多个中断要逐个回应（#388）。一个超步中有多个节点或 Send 抛出 `NodeInterrupt` 时，运行会在这些中断上全部暂停：`RunResult.interrupts` 以 `{id, node, value}` 的形式列出每一个，其中 id 是被暂停的那次调用的 task id（节点形如 `s3:review`，Send 形如 `s3:send[1]:worker:<hash>`），`interrupt_node`、`interrupt_value` 和 `interrupt_id` 报告的是第一个。一次恢复回应的是 `interrupt_id=` 指定的那个中断（C++ 中是 `RunMetadata::resume_interrupt_id`），未指定则回应第一个；这一步里其余被暂停的节点或 Send 会在没有该值的情况下重新运行，再次提出各自的问题，由下一次恢复来回应。指定一个并不处于等待状态的中断，会在任何节点运行之前抛出 `ValueError`（C++ 中是 `std::invalid_argument`）。在多个中断上暂停的检查点，会在元数据（`_neograph.interrupts`）中列出它们；只在一个中断上暂停的检查点，格式不变。恢复值只送达这个暂停步骤中被回应的那次调用（#393）：恢复后的运行中，后续各步的节点都看不到 `ctx.resume_value`，所以在那里提问的节点或工具闸门会再次暂停运行，而不会把别的问题的答案拿去用。通过父图恢复的子图，经由父图中被回应的节点拿到它的答案。带有 `messages` 通道的图，仍会在该通道里以用户消息的形式收到这个值。JavaScript 的 join 以及计划级的 `parallel` 和 `quorum` 遵循同样的规则：每次恢复回应一个暂停的成员，其余成员重新提问。

兄弟节点以不同方式结束的一步，不论它们的序号或完成顺序如何，都按同一条规则收尾（#392）。这条规则对 fan-out 的各分支和同一步里的各个 Send 都成立，不论只有一个 worker 还是有多个。运行被取消时，该步停止。否则，如果存在不确定的副作用，即 payload 的 `kind` 为 `tool_effect_reconciliation` 的 `NodeInterrupt`，那么即使有兄弟节点失败，这一步也会为对账而暂停，因为此时结束运行会丢下一个谁也不知道结果的副作用；这次暂停只报告这类中断，而失败的兄弟节点没有记录结果，会在该步恢复时重新运行。否则，只要有失败，运行就以派发顺序（先按就绪顺序，再按 Send 顺序）中的第一个失败而失败：这一步里普通的中断不会被报告，也不会为它们保存检查点。再否则，这一步在所有中断上暂停，如上所述。沿失败边（failure edge）路由的失败属于节点的结果，与正常完成一样：兄弟节点的中断仍会让这一步暂停，等这一步完成后才走这条路由。JavaScript 的 join 以及计划级的 `parallel` 和 `quorum`，对成员按同样的方式排序（#397）：有成员因不确定的副作用而暂停时，即使旁边有成员失败，join 也会停在第一个这样的成员上，除非已有成功的结果决定了 join 的结果（race 的胜出者、已满足的 quorum）；失败的成员会被记录下来，待恢复所回应的那个成员运行结束、并且它的副作用有了定论之后，失败成员再让 join 结束（#400）。无论 join 因何暂停，因不确定副作用而暂停的成员，都比因普通问题而暂停的成员先被询问，与声明顺序无关（#401）。收集式的 `ng.all`（`collect: true`）是上述规则有意设置的例外：它要等所有成员（包括暂停的成员）都结束后才报告，所以只要还有成员因普通问题而暂停，某个成员的失败就不会让它结束；它会停在那里，等所有成员都有了结果，再报告这个失败（#401）。

当是否暂停取决于节点的中间输出时，这种方式很有用（例如：“LLM 的产出是否值得展示给人看？”）。

### 时间旅行

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` 把一个检查点复制到调用方指定的目标线程，并返回新检查点的 ID。省略检查点 ID 时，取源线程最新的检查点。副本保留待继续的 continuation；编辑状态不会安排新的工作。`NodeInterrupt` 检查点的 fork 还会带上暂停超步的待定写入，所以恢复这个 fork 时，会像恢复源检查点一样，重放该步中已完成的节点和 Send，而不会把它们连同 provider 调用再跑一遍（#390）。源检查点保留它自己的写入。

恢复一个 `next_nodes == ["__end__"]` 的已完成 continuation，只会还原已保存的结果，不执行任何节点。想在编辑后的状态上继续已暂停的工作，请从 `get_state_history()` 中选一个仍有待执行节点的、更早的确切检查点，fork 这个 ID，编辑 fork 出来的副本，再恢复它。历史快照中 `next_nodes` 为空向量的情况有所不同：不指定确切 ID、只恢复最新状态时，保持新开一次运行的原有行为；按确切 ID 恢复则固定在那份快照上。

[示例 08](../examples/08_state_management.cpp)沿用新一轮对话（new-turn）的流程：fork 一个已完成的检查点，修改用户消息，再用 `resume_if_exists=true` 调用 `run()`；只有这次新运行发生中断时才恢复。它并没有演示如何恢复一个已暂停的 fork。

`ChatMessage` / `ChatTool` 和 JSON 只是可移植的投影（portable projection），并不具备 native 权限。可移植格式仍然是 [`provider-message-v2`](../schemas/provider-message-v2.schema.json) 和 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真正的 C++ 检查点 sidecar 把 native seal 保存在内存里。要持久保存 native 历史，需要由宿主持有的 `sp::NativeArchive`：封闭的 v3 / `spna3` 格式，带经过认证的、归 owner 私有的托管（custody），并使用独立的密钥。archive v2 会被拒绝，既不会升级，也不会被解释。认证绑定的内容包括：描述符里每一项语义选择（origin、路径、header、policy、请求字段映射、usage 路径和 stop 映射）、owner，以及确切的托管绑定。它既不是加密，也不是对 vendor 签发方的认证；不要公开 archive 的正文、密钥、native blob 或原始的 wire 观测数据。archive 只是证据存储，不是资金 grant，也不是消费租约（spending lease）。Program 和外部的 bank 仍由各自独立的 journal 持有；复制快照不能凭空产生额度（credit）。

provider 的历史有几种不同的模式。走同一条路由时，native continuation 会在原来的绑定下，保留真实的 reasoning、签名和有序的工具调用组。Gemini 默认使用 `NativeOnly`；显式指定 `PortableForeign` 时，接受调用方自己构造的 assistant 文本和工具调用，这些内容没有 native seal、wire 输出和签名。只有第一个外来的 function call 会带上 Google 文档规定的绕过标记；纯文本的轮次不带签名。这种投影不授予 native 权限，也不会修复或降级一个校验失败的 native seal。它也不意味着任意多 vendor 混合的历史都能原样在 native 层面移植。

Responses 的 `previous_response_id` 用来选定由 provider 保存的会话状态，此时请求只携带新的输入。当客户端工具的归属需要本地证据时，由 `previous_response_history` 提供真实的先前归属证据，它不会作为重复输入发出去。游标（cursor）既不是完整的 native replay seal，也不具备 archive 权限；它仍然要通过 origin、路由、模型、配置和完成状态的检查。

当前的 SDK 接口修订版和共享库代数（generation）都是 4；使用方必须针对匹配的头文件和库重新构建。输出生成上限在每次调用时都要经过准入(admission)并计入用量，与 native replay 的配置分开。为一次新的语义调用调高它，不会续期原来的 bank、grant 或截止时间。除了文档里明确列出的逐轮可选项，content、prefix、origin、路由、policy、工具和 reasoning 控制都保持绑定。可移植 JSON v2 和 native archive v3 / `spna3` 保持不变；下文的历史 ABI3 测量值不是 interface4 的结果。

Python 暴露的所有权式 request/outcome 边界与 C++ 相同：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。provider 的历史请使用带类型化部分（typed parts）的 `ProviderMessage`；`ChatMessage` 仍只是方便图使用的投影。SDK 返回的失败可以通过 `ProviderOutcome.failure` 读取，而宿主的 observer 或结算（settlement）抛出的异常会保留 `outcome` 和 `cause`。构造函数以及 GIL 和回调的行为，请看 [Python 绑定指南](python-binding.md)。

`input_total`、`output_total`、`total` 等用量计数器的类型是 `std::optional<sp::Count>`；有值的计数带有 `uint64_t value` 和 `Evidence`。`Usage` 还会记录 stage、quality 和 conflicts。缺失表示未知，绝不会被当成 0。

`UsageAccumulator::snapshot()` 返回累计的报告。`total_tokens_wide()` 返回已计费的 token 数加上尚未结清的预留量，不能把它当作上报的用量显示出来。结算要求报告是最终的、前后一致的，且带有 input 和 output 计数；结算按有依据的最大总量计费，超出的用量也不做截断。只要累计的某份报告里缺少某个计数器，汇总后该计数器仍然是未知。预留、本地计费和 vendor 的发票是不同的记录。

**独立 bank journal 的修正：当前契约已修订，实际运行的证据见下文。** 经 owner 批准的协议要求：有一个单调的、受信存储命名空间义务（obligation），以及真实且不可变的原始 owner/thread/graph 作用域、上限（ceiling）、截止时间/时钟标识和 generation。只有对完整检查点承诺（commitment）和 revision 做精确的持久 head CAS，才能签发由宿主持有的不透明租约（lease）。精确的 pending effect 窗口必须在 provider I/O 之前持久化；结算必须使用真实的 SDK outcome 和实际计费，以及可空的 report、hold 和去重标识。检查点和下一个 head 必须在同一个 owned actor/revision 下原子发布。删除 bank 元数据、裁剪（prune）检查点、重放旧的已认证快照、覆盖同一个 ID 或丢失 actor，都不能产生额度（credit）。已有 65 的 hold 时，把上限 130 收紧到 129，不能再准入另一个 65；已证明没有副作用的失败，可以释放保持不变的 head，使真实的 130 恢复仍能进行。崩溃、未知、租约丢失的窗口一直处于 hold 状态，不退款、不重试、不降级（fallback）。普通/原始（plain/pristine）的 archive 配置不授予资金或 native 消费租约，当前的 `config.usage` 也不能替换已有的独立义务；Program 和外部 bank 的 journal 所有权不变。这是必须满足的契约；实际的币种/托管证据和测量手段的局限见下文，它不是稳定发布的 API 保证。

**当前声明；集成运行的证据见下文：** `<neograph/graph/checkpoint.h>` 声明了 `ManagedBudgetLeaseScope`，包含 `owner_scope`、逻辑 `thread_id`、私有的后端 `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks` 和 `deadline_clock_identity`。`OwnedManagedBudgetLease` 提供只读的 `scope()`、`actor_id()`、不可变的 `bank_generation()`、`revision()`、`head_checkpoint_id()` 和 `head_commitment()`；它没有可导入权限的公开构造函数。`ManagedBudgetEffectReceipt` 提供 `active()`、`effect_id()`、`claim_amount()` 和 `request_digest()`；默认构造的 receipt 不授予任何东西。`CheckpointStore` 声明了 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)` 和 `release_managed_budget_lease(lease)`，以及对应的 `_async` 版本。同步的 `CheckpointStoreCore` 和 `AsyncCheckpointStore` 各自提供相应的变体。`managed_budget_checkpoint_commitment(checkpoint)` 覆盖完整的持久检查点，而不只是 bank 的 JSON。这些声明本身并不能证明后端的 CAS、币种安全、已安装 ABI 的兼容性，或某条运行路径已被成功演练。

**真实的 InMemory shared-bank fork 已保留并经过验证。** 最初那个真实的 C++ fork 使用唯一一份原始的 financial journal 和受信的当前分支 head，而不是复制出来的 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（及 `_async`）要求提供真实的当前源及其完整 commitment，以及指向同一个 bank 的实际 native C++ 指针；持久化的独立 fork 仍明确不支持。`OwnedManagedBudgetLease::scope()` 以及原始 owner/thread/graph、上限、截止时间/时钟和 generation 保持不可变。只读的、由存储签发的 `execution_thread_id()` / `execution_storage_thread_id()` 单独指定执行分支；`GraphState::budget_original_thread_id()` 标识原始的 financial bank。对所选分支 head 做精确 CAS，再加全局的 actor/revision，使所有分支针对规范的当前计数器、pending effect 和已作废（burned）的标识串行化。原分支和 fork 分支都保持可用，无需补充额度；过期的快照、复制的检查点和导入的 JSON 都不能产生别名（alias）或回退 head。原有的 root30 → charge3 → original continuation6 → fork lower20 → continuation9 同一 bank 的验证，在未改动的 test_graph_engine.cpp:810–913 中通过（PASSED）；保存的原始上限 30 与 fork 的有效上限 20 是两回事；放宽到 31 以及仅用 JSON 恢复，都必须被拒绝。上报的无上限观测值是事实数据，不是有限的 grant。只有已证明零副作用的租约才能释放保持不变的 head；未知或 pending 的副作用会保留其义务。

**当前的释放错误（release-error）契约；实际演练的测试集/探针见下文。** `<neograph/graph/engine.h>` 中的 `graph::ManagedBudgetLeaseReleaseError` 派生自 `ProviderOutcomeError`。`cause()` 保留原始的执行异常，`release_error()` 暴露次要的、持久化租约处置阶段的失败。`outcome()` 在有真实 SDK 证据时保留该证据，没有 SDK outcome 时为 null；释放失败既不能凭空构造 outcome，也不允许重新分发。封闭的 `_neograph_managed_budget_scope` 元数据描述原始的逻辑作用域、上限、截止时间时钟和 generation，但它只是数据，不具备后端 CAS 的权威。

**Archive owner 与留存（retention）契约；实际演练的测试集/探针见下文。** 只有有限的独立根（finite standalone root）或经认证的有限来源，才会从真实配置的 `sp::NativeArchive::owner_scope()` 继承被省略的原始 owner；无限（unbounded）或普通（plain）的 owner 元数据语义不变。显式冲突的 archive owner 会在获取租约之前被拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 以及 Core/Async 对应的存储能力，默认返回 false；真实的 InMemory 后端重写为 true，包装器必须把真实的留存能力转发下去。这个只读的描述，允许合法的、未加租约/普通/无限的 C++ native 检查点托管，但它既不授予消费额度（credit），也不授予 native replay 权限。加了租约的托管使用存储实际签发的 receipt，而不是 JSON 标志或猜测出来的存储类型。

**native 托管的 I/O 前闸门（pre-I/O gate）；实际演练的测试集/探针见下文。** 开始一个受管的副作用（managed effect）之前，在修改任何 pending effect、slot 或 held 窗口之前，必须有真正绑定的 NativeArchive，或者存储实际签发的本地私有 C++ 留存能力。这个私有能力不会从 JSON 导入，也不会通过 wire 传输。即使远端后端是 InMemory，gRPC 也要求客户端和服务端各有真实的 archive，因为 C++ sidecar 无法跨越这条边界。没有 archive 提供有限的来源 owner 时，原始的匿名 owner 作用域保持为空；真实的 archive 绑定必须与原始作用域一致。只有 financial head/租约的证据，并不能证明 native 托管已经就绪。

`ProgramFailure` 保留存活的 `provider_outcome` 和 `provider_cause`。它规范的、事实性的 SDK witness，把真实的 archive 托管绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复出来的失败之前，会先主动恢复配置好的托管。公开的、纯数据的 `ProgramResult::create()` 不能用预先填好的 witness 绕过这一步，未解析完成的 parsed seal 也不是可执行的结果。进程重启之后，原来的异常指针不再可用（`provider_cause == nullptr`），也不会根据文本重建。无法持久化的失败，也就无法序列化、发布或重放。

`RecordedBindingSet` 是绑定到来源（source-bound）、只能移动的数据，不是调用方提供的 dispatcher。受信的 Catalog `recorded_capability_binder` 根据真实持久化的来源事件，独立地物化出只含已捕获内容的能力（captured-only capabilities）。`ProgramRuntime::replay_recorded()` 先检查原来所选来源的权限，再通过持久的 CAS 转移实际剩余的 bank；继承下来的花费不是新的模型 grant。旧的 `start_recorded` 续期 API 已经移除。InMemory、File、SQLite 和 PostgreSQL 的 Program 存储，在整个执行期间保留那份确切的、不可变的、归自己所有的租约；过期不会使它续期。受控的 JavaScript 仍会校验底层的 capability manifest，并消费确切的已完成命令结果，不会重新分发外部副作用。

**Recorded-control 因果修复已在完整测试集中验证。** 重放已捕获的命令时，执行之前只为新增的 CPU 挂钟时间和 Core 工作做持久的预留，然后通过结果 CAS 发布测得的工作量，以及新产生的 Core 检查点。它不消耗新的模型、资金或 Program 操作额度，也不会重新分发已捕获的外部副作用。未对账的预留仍然记为已扣减。这笔预留会选择经认证的结算转换，而不是曾经拒绝首个新 Core 检查点的普通 Running→Running 转换。await 通道的接收、定时器的等待/取消、以及 handoff 等待的发起/释放，都在各自所属的 executor/strand 上串行化；现有的 Recorded CPU/Memory await/handoff 场景在完整测试集中通过；远端 TSan 覆盖的局限仍在下文明确列出。

下面这些观测记录于本次文档校对之前。它们是历史证据，不是新的测试运行结果，也不保证适用于所有平台、传输方式或安全属性。

**已完成的付费观测；并非通用的资格认证（qualification）。** 最初的 `SPQUAL1` 基线为 630/1000000 microUSD，保持不变；同一份原始账本（ledger）里，由一个哈希链接的 `A` 接纳了获批的扩展额度 480/3000000，合计 1110/4000000，调用数、已花费额度、hold 和结算都是累计的，没有新的 grant ID、header，也没有重置。确切的声明字节/文件标识，以及原始的授权/基线/catalog/激活/账本前缀的哈希和总额都保持固定；删除、替换或改动都按“失败时关闭”处理。最终的规范账本为：调用 1110 次、已花费 437958、hold 1287828 microUSD，eventA1，上限 1110/4000000；已花费加 hold 共计 1.725786 美元，这是本地 catalogue 的计量，不是发票。文档记录的五个 family、每个 60 对的基线共完成 600 次请求：Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（4 次 vision 判断错误，均为 SSE）、Interactions 57/60（1 次 buffered、2 次 SSE 的 vision 判断错误）；合计 293/300 对，不是 300/300。其他旧的 600 次相关财务记录被保留，但并不构成完整的行为证明。更早的 M5/media 一次性 cohort 保持不变。更早的三轮 Google 前置条件检查，仍保留 2 次 invalid-tool 和 1 次 unreadable-positive 失败。不再授权任何付费调用。最终的 SDK 证据和 native-axis 的局限，与基线的成功是两回事。更早的激活/重新打开（reopen）冒烟测试，仍记录为两次 reopen 后调用 610、已花费 219159、hold 751233，SDK 的 meter/canary/vision 4 个测试在 19.38 秒内通过；这些都是特定范围内的历史检查点，不是最终的账本总数。更早验证过的 Chat 60 对 cohort 仍保留 120 次实际尝试、120 次 UpperBound 计费，没有 UnknownHold。

**native-axis 观测：不是密码学验证，也不代表 native 消费或等价。** Generate 接受了篡改（mutation）、省略（omission）和重复（duplication）。Interactions 接受了孤立的真实来源/正向对照、单个 owner 的签名篡改、省略 thought-carrier、省略 call-carrier，以及重复。删除全部 thought 和签名，返回 generic 400；删除全部签名字段、但保留 THOUGHT 项，同样返回 generic 400。最后一次采集（capture）只有本地的、编码后原件的留存对照，没有同一次采集中的服务端正向结果；更早的正向 cohort 仍然是真实的。这些观测只确立了“整体 carrier 缺失”的边界，不能证明 issuer/签名校验或 vendor 的消费。实际报告：SDK 的 `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json` 和 `qualification-signature-presence-results.json`；前置条件失败、未运行、负向结果不确定这几种状态，如实保留。只要还剩另一个 carrier，仅省略 thought 或仅省略 carrier 的请求就被接受；这并不能加强 issuer 校验或 native 消费方面的任何说法。

**实际的集成验证与遗留局限。** 最近一次 Core 完整运行：2242 个测试，0 失败，16 个跳过（14 个 RAM 进程丢失用例不适用，2 个需要真实凭据的闸门），耗时 130.17 秒。`PgNestedJsonRoundTrips` 精确保留了重复的键、顺序、null 元数据、blob 和 residual，用时 0.18 秒。未改动的原始 shared-bank fork 和现有的 Recorded CPU/Memory await/handoff 场景均通过。针对真实包装的 Memory/SQLite/PostgreSQL/gRPC，执行了 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 这组探针，普通构建和 ASan+UBSan 构建下均通过。本地 Memory/SQLite/PostgreSQL 的 TSan 范围：7 项通过，0 个警告。含系统 Abseil/Protobuf 的完整混合 gRPC TSan 以退出码 66 结束，在依赖库和生成的 RPC 栈里有 402 条 race 警告：这是插桩/覆盖范围的局限，不是已证实的误报；我们不声称远端 TSan 无 race，也没有屏蔽任何警告。已安装的 find_package Program C++/C ABI/双 QuickJS 这 3 个使用方均通过。全新安装的 NeoGraph/SchemaProvider 类型化使用方通过了：2 次真实的 HTTP 请求、协程启动前销毁 provider、native/工具重放、refusal、已知为零/原始值的保留，以及真实的 LinkedMismatch 拒绝。浏览器中 Alice/Bob 的隔离和 generation 2 的替换，已经目视验证；PostgreSQL Program Chat 的 6 个黑盒测试在 18.989 秒内通过。最近一次 SDK：26/26 通过，0 失败，74.07 秒。最终的 ReleaseGraph：16 种配置 ×3 次全新进程重复，共 48 条记录，用时 38.29 秒完成，0 失败，所有实际的协议/owned-outcome 检查均通过。NeoGraph 的 `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保存了这个独立的最终 cohort。测量期间没有运行编译器，也没有调用付费模型；历史 cohort 保持不变，也不声称语义或资源上等价。不稳定的 SDK/ABI3 既不是稳定发布，也不是对更多平台的资格认证。

宿主的交付限制、有范围上限的诊断/原始证据、常见的 provider 错误，以及最小化的媒体证据，见 [typed provider reference](reference-en.md#owned-outcome)。

---

<a id="8-streaming-events"></a>
## 8. 流式事件

`run_stream` / `run_stream_async` 会在事件触发时调用回调。模式是可以按位或组合的位掩码：

| 模式 | 发出的事件 |
|---|---|
| `EVENTS` | `NODE_START`、`NODE_END`、`INTERRUPT` |
| `TOKENS` | `Provider` 流式返回的每个 token 对应一个 `LLM_TOKEN` |
| `DEBUG` | `__routing__` 事件，显示下一批就绪节点 |
| `VALUES` | `__state__` 事件，每个超步结束后给出完整状态 |
| `UPDATES` | 每个 `ChannelWrite` 对应一个 `CHANNEL_WRITE` 事件 |
| `ALL` | 以上全部 |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **注意：** 用 `event.node_name`，不是 `event.node`。C++ 结构体的字段是 `node_name`，pybind 保留了原名。

要做聊天形态的流式输出（与 LangChain 兼容的消息字典，带增量的 `content_so_far`），请使用辅助函数：

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 的放置位置（C++）

在 C++ 中驱动 `engine.run_stream_async()` 时，最外层的 `asio::io_context.run()` 应当在应用的主线程上调用（或者在任何经由正常进程启动路径初始化过的长生命周期线程上调用）。经过测试、确认可用的写法：

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

> 历史问题 #16 曾在某些 glibc/OpenSSL 组合上观察到：在每个请求里嵌套 `io.run()`，并配合旧的子线程 provider 流式桥接，会在 `getaddrinfo` 中出现 SEGV。类型化切换（typed cutover）已移除这个桥接；当时那次结构性测试的结果，并未穷尽复现下游的 HTTPS/sanitizer/负载条件，也不是当前的资格认证。当前的调用方应通过 `invoke_async` / `dispatch_async` 显式指定 `ProviderMode`，并在已有的长生命周期 executor 上驱动工作，而不要嵌套每个请求各自的事件循环。不要把合成单个假 token，或重新发送一次 completion 当作变通办法。

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer` 和 `neograph_engine.openinference.openinference_tracer` 会把图事件转成 run 和 node 的 span。后者把 span 标记为 `CHAIN`，并记录节点 payload 的投影。每次运行只选一个图回调。要把模型调用记录为 `LLM` span，需要在编译图之前，用 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` 包装带类型的 provider。这个包装器使用 C++ 原生的 observer，沿用继承来的 `prepare` 和一次性的 `dispatch` 或 `invoke`。准备请求或放弃请求都不会开启 span；通过准入(admission)的 dispatch 才会开启 span，并且不会改变它所拥有的 outcome、取消、截止时间或类型化事件。tracer 的失败不会替换 provider 的结果或异常。

```python
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine import GraphEngine, NodeContext
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer, self.parent_context = tracer, parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)


def trace_graph(graph_spec, inner_provider, model, cfg):
    provider = TracerProvider()
    provider.add_span_processor(BatchSpanProcessor(
        OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
    tracer = provider.get_tracer("my-app")
    try:
        with openinference_tracer(tracer) as cb:
            parent = ParentContextTracer(tracer, otel_context.get_current())
            observed = OpenInferenceProvider(inner_provider, parent)
            engine = GraphEngine.compile(
                graph_spec, NodeContext(provider=observed, model=model))
            return engine.run_stream(cfg, cb)
    finally:
        provider.shutdown()
```

要在本地启动 Phoenix 端点，运行 `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest`，并安装 `opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`。调用 `trace_graph` 时，传入图规格、一个已有的 provider、明确指定的模型和一个 `RunConfig`。`ParentContextTracer` 会把运行的根 span 显式带入 worker 的分发中；不承诺自动跨线程传播，也不承诺按节点自动传播父 span。Python 在 dispatch 时使用当前活动的 OTel 上下文，已准备好的操作在其生命周期内一直持有 tracer 适配器。

LLM span 包含公开的 role/文本投影、声明过的标量，以及已知的用量计数。已知为零的值会记录，未知的则省略。native replay/reasoning、原始的 wire 信封/事件和编码后的请求体都不进入 trace，它们仍由 request/outcome 真实保管。用量属性（包括失败时上报的部分报告）既不能证明 vendor 实际收费，也不构成预算权限；已计费和已预留的核算，归 `UsageAccumulator.authority_snapshot()` 和 Program 的 `provider_budget_authority` 负责。

公开文本、异常信息和图的 payload 仍可能含有应用的机密。请自行选择，或脱敏后再交给 exporter；参见 [OpenTelemetry 关于敏感数据的指引](https://opentelemetry.io/docs/security/handling-sensitive-data/)。`CHAIN` 和 `LLM` 由 [OpenInference 规范](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md)定义；[参考文档](reference-en.md#105-observability--opentelemetry--openinference)列出了 NeoGraph 支持的属性子集、token 事件、Python 带类型调用的示例，以及 C++ 的生命周期要求。

---

<a id="9-common-pitfalls"></a>
## 9. 常见陷阱

下面这些问题都有真实用户遇到过，[`docs/troubleshooting.md`](troubleshooting.md) 中也有对应的交叉引用。

### “我的 ReAct 循环只运行一次”

这是 wheel ≤ 0.1.7 的问题：图编译器悄悄丢掉了 `conditional_edges` 块。升级到 ≥ 0.1.8，并用 `result.execution_trace == ['llm', 'dispatch', 'llm']` 验证（而不只是 `['llm']`）。

### “provider 调用挂起 60 秒后报错”

这是 wheel ≤ 0.1.6 的问题：内置的 OpenSSL 把 RHEL 的 CA 路径写死了，而这些路径在 Ubuntu / Debian / macOS 上并不存在。升级到 ≥ 0.1.7（导入时会自动把 `SSL_CERT_FILE` 设为 certifi 的证书包），或者手动设置 `SSL_CERT_FILE`。

### “我的 fan-out 比预期慢”

`compile()` 默认调用 `set_worker_count(1)`（引擎不创建自己的线程池，fan-out 的各分支在调用方的 executor 上串行运行）。要真正并行，请调用 `engine.set_worker_count(N)`，N 取与 Send 的 fan-out 宽度一致的值；或者调用 `engine.set_worker_count_auto()`，取 `hardware_concurrency()`。多 Send 的 fan-out 在没有选用线程池的情况下首次运行时，NeoGraph 还会向 stderr 输出一次警告，这只是提示，不是错误。fan-out 规模较小时，Python 自定义节点会遇到 GIL 争用，所以 N 和 1 两种配置都要测一下。

### 读取 Python RunResult 的 status 和状态

`result.status` 给出带类型的状态：`Completed`、`Interrupted`、`StepLimit`、`SafePoint`、`BarrierStalled` 或 `FailureExit`。其中 `BarrierStalled` 表示某个屏障再也无法触发，运行在没有到达 `__end__` 的情况下停止；`FailureExit` 表示某个失败退出节点已执行完毕，运行以失败告终。`result.output` 仍然是可移植的最终状态；`result.interrupted`、`result.max_steps_exhausted` 和 `result.execution_trace` 描述这次运行。`result.native_messages` 和 `result.provider_outcomes` 保留完整的带类型 provider 证据。参见 [Python 绑定指南](python-binding.md#hitl-and-state)。

### “Unknown reducer: <name>”

内置的 reducer 只有两个：`overwrite` 和 `append`。自定义 reducer 要在编译之前注册，C++ 用 `ReducerRegistry::register_reducer`，Python 用 `ng.ReducerRegistry.register_reducer`。

### “条件已注册，但条件边没有触发”

先确认写法是加载器接受的那种（[§4](#4-edges--conditional-routing) 中的写法 A 或写法 B），两种从 v0.1.8 起都可用。更老的 wheel 只支持写法 B。

### “execution_trace 只显示起始节点”

路由落到了 `__end__`。最可能的原因是起始节点缺少出边，或者条件返回的值不在 `routes` 映射里，而显式声明的 `"default"` 路由又指向 `__end__`。严格的图不再按映射的排列顺序选择路由：开放的（open）或未指定的条件，在声明了 `"default"` 时使用它，否则引擎抛出异常，信息中包含源节点、条件和返回的标签。封闭的（closed）条件一旦返回了它声明的标签之外的值，一定会抛出异常。

---

## 接下来读什么

- [Python 示例](../bindings/python/examples/)：21 个可独立运行的脚本，覆盖以上每个概念。
- [C++ 示例](../examples/)：36 个程序，结构相同。
- [`reference-en.md`](reference-en.md)：逐个类列出的完整 API。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md)：深入讲解异步/协程层。
