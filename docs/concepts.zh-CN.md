<!-- neograph-i18n: source=docs/concepts.md locale=zh-CN source_sha256=0f718bca31f68497ef00b56cb3dd01cd534f53f3dfd2a42741524fae51a18a36 -->
# NeoGraph 核心概念——叙事指南

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

先阅读本文，再查看示例。各节按构建图的顺序介绍 channel、node、edge、fan-out、路由、检查点和流式传输。

熟悉 LangGraph 的读者会认识带 reducer 的 channel、`Send`、`Command` 和检查点。NeoGraph 的 [Core 与 ProgramRuntime](../README.md#core-and-programruntime) 职责不同；本指南从 Core 图执行开始。Python provider 调用使用 typed [binding 契约](python-binding.md)，而不是已删除的 completion 类。

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
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

普通 ready batch 读取该 batch 更新前的 channel 状态。执行中的兄弟 node 不能读取另一兄弟返回的写入。引擎缓冲这些写入，在 batch 结束后通过 reducer 合并，再以更新后的状态评估路由。这是图调度，不会同步模型内部计算。

例如 `counter` 从 0 开始，两个 ready node 都返回 `counter + 1`，两者读取的都是 0。overwrite reducer 的结果是 1，而不是 2。向自定义 sum reducer 分别写入增量 1，才可合并为 2。多分支 `Send` 使用应用各自 payload 的隔离状态副本；单个 `Send` 路径把 payload 应用到共享状态。Reducer 顺序本身不能保证模型响应或外部效果可复现。

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

### Channel 生命周期与 checkpoint 契约

Reducer 合并写入。数组 retention 是独立策略：`unbounded`（默认）、`latest`，或具有正数 `retention_limit` 的 `bounded`。Retention 在每次写入后裁剪数组，包括 `ChannelWrite.Mode.Overwrite`；`latest` 保留最后一个元素，直到下一次写入。Persistence 独立选择 `checkpoint`（默认，materialized 值与 version）或 `ephemeral`（持久 checkpoint 省略两者）。Bounded retention 改变可见历史，不只是存储大小。

引擎按 node 返回写入的顺序、static batch 的 scheduler-ready 顺序、多 `Send` 的调用顺序合并，不使用完成顺序。Pending write 重放到相同 task slot。Overwrite 是有序 last-writer-wins，append 保留元素顺序。自定义 reducer 在 replay 时应纯粹且稳定。Regrouping 要求结果相同则需结合律；顺序独立才需交换律。显式 overwrite 绕过 reducer 后仍应用 retention。这些规则不保证模型响应或外部效果可复现。

Ephemeral 值跨 superstep 存活，不在每 step reset。每个 checkpoint 记录声明的 ephemeral 名称及是否已写入，但不保存值。Resume、`resume_if_exists`、exact-ID resume、state update 拒绝已写入的 ephemeral 状态、旧 checkpoint 缺失的 guard 或已改变的 ephemeral channel 集合。第一次 ephemeral 写入前的 checkpoint 可按文档顺序 replay pending write 并 resume。`update_state` 拒绝 ephemeral 写入。多 `Send` in-process worker 在隔离副本中继承 live ephemeral 值。正确性所需状态应持久化到 checkpoint，或在新 run 中从持久输入重建。

`GraphState::restore` 拒绝具有 ephemeral channel 的图。使用匹配 guard 的 `restore_checkpoint`，或对含全部 live 值与 version 的同 process snapshot 使用 `restore_runtime`。Guard 使用 checkpoint metadata，不改变 channel blob layout 或 store schema。无 ephemeral channel 的图仍可使用旧 full-value checkpoint。Downgrade 到无 guard binary 前，需 drain ephemeral thread 及 fork，或从持久输入重新开始；旧 reader 不能强制新增 guard。

Checkpoint channel 使用 full materialized snapshot。Memory、SQLite、PostgreSQL 去重未改变的 `(thread, channel, version)` 值，但 append 历史每次写入都改变 version，因此 snapshot 仍增长。Pending write 记录未完成 superstep 的成功 task，不是通用 channel delta。当前不提供 per-step reset 策略；安全设计必须定义写入及路由后 reset、interrupt、replay 和 Send 行为，不能与 ephemeral persistence 混淆。

Delta-backed checkpoint 是设计，不是 channel 设置。该格式会从 full snapshot 重放有序 `{channel, version, write mode, value}` delta，最多 *K* 个（可加 byte 阈值）。它必须保留 overwrite、retention、version、reducer identity，并在清除 pending write 前原子 publish snapshot/delta 与 checkpoint pointer；拒绝缺失 link、version gap、未知 reducer 或失败 replay。采用它需要新 schema version 和实测收益。将现有 snapshot 移为 base 而不伪造历史 delta；可逆 rollout 保留 old-reader full snapshot；若有 delta-only record，除非以原 reducer registry materialize，否则拒绝 downgrade。当前 store 保持 full-snapshot 方式。

以 `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite` 测量 baseline；仅在隔离 local DB 上添加 `postgres` 和 `--pg-url`。各行报告 logical serialized byte、save/load p50/p95、reconstruction depth；legacy 行报告 blob count。Allocation request 使用 `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`。Native JSON/SQL allocator 并非都被 C++ `operator new` 捕获。用相同 payload、history、backend 对比实测值。Logical byte 不等于 durable physical byte。SQLite 使用退出时删除的唯一 temporary DB；`--sqlite-path` 保留新 file 并拒绝已有 path。

已记录的 Linux x86-64 Debug baseline 为一个 thread、256 history step、512-byte message、一次 iteration。它是历史测量，不是性能目标：

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

删除前的另一次 repeat 测得 SQLite DB/WAL 为 14,811,136 / 4,210,672 byte（总计 19,021,808）。计算包括构建及 JSON parse 在内的 process-wide `malloc`、`calloc`、非零 `realloc` request 的 Linux `LD_PRELOAD` shim，与 `--history-steps 0` 相比测得 Memory 额外 88,277 request / 605,289,027 requested byte，SQLite 为 114,295 / 867,319,964。这是累计 request，不是 live memory 或 store-only allocation。Aligned/internal allocation 未被捕获。Shim 不是依赖；形成结论前使用支持的 profiler 及多次 warm run。

### 写入通道

节点返回一个 `ChannelWrite` 列表：

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
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
| `llm_call` | 准备一次拥有所有权的 typed 请求，经 gate/reserve/receipt 后 dispatch 同一 handle，保留全部顺序消息及完整结果。 | 读取`provider`、`model`、`instructions`、`tools`自`NodeContext`。 |
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

Python 在 `input.ctx` 上公开 `cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`trace_id`、`run_id`、`model_token_budget` 和 typed provider 证据。Deadline 通过 `has_deadline`、`deadline_remaining_ms` 查看；原始 C++ steady-clock 值保持不透明。C++ 调用者通过 `RunMetadata` 提供 deadline 和 trace metadata，并传播到嵌套 subgraph。

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
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
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

`Send` 允许 node 在运行时选择 target 调用次数，例如每个 topic 一个 researcher。引擎在普通 ready batch 返回且写入应用后，在同一编号 superstep 内执行发出的 Send。

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

引擎每个 `Send` 调用一次 compiled target，不保证新建 node object。因此 target 必须安全处理并发调用的 member state。Payload 在 target 读取 channel 前应用。单个 Send 使用共享状态；多个 Send 使用 ready batch 后状态的隔离副本，并在全部分支完成后按调用顺序合并返回写入。随后合并普通 node 和 Send target 的信号，规划下一 ready batch 的路由。

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

`build()` 默认 `EngineConfig::worker_count == 1`，不创建 engine-owned thread pool，而在调用者 coroutine executor 上 dispatch 分支。Coroutine I/O 可重叠，单 thread executor 上 CPU-bound 工作可能串行化。Multi-thread caller executor 或并发 run 仍要求安全处理 node member state。

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
                    updates=[ng.ChannelWrite("retries",  (input.state.get("retries") or 0) + 1)],
                ),
            )
```

### Command与条件边的使用场景

- **条件边**：路由依赖于不需要使用节点逻辑的状态谓词。更清晰、声明式。
- **Command**：路由依赖于最自然地在节点内部编写的逻辑 — 多标准评分、内容检查、重试决策。也是原子性更新状态并同时选择下一个节点的唯一方式。

### fan-in 下后写者胜出

若多个兄弟返回非空 `Command.goto_node`，传入路由顺序中的最后一个 command 覆盖普通 edge 和 barrier。Static batch 提供 ready 顺序；多 `Send` 提供调用顺序，而非完成顺序。所有返回的 command update 仍通过写入 pipeline 合并。若冲突 command 会改变 workflow，宜只让一个 node 决定路由。

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

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` 将检查点复制到调用方指定的目标线程，并返回新检查点 ID。省略检查点 ID 时选择源线程的最新检查点。副本保留待执行的 continuation；编辑状态本身不会安排新工作。

Resume 已完成且 `next_nodes == ["__end__"]` 的 continuation 时，只恢复保存的结果，不执行节点。要在编辑后的状态上继续暂停的工作，应从 `get_state_history()` 选择仍有待执行节点的精确早期检查点 ID，fork 该 ID，编辑副本后再 resume。历史上的空 `next_nodes` 向量有所不同：未指定精确 ID 的 latest resume 保留开始新执行的旧行为，而 exact-ID resume 仍固定于指定快照。

[Example 08](../examples/08_state_management.cpp) 保留新 turn 流程：fork 已完成的检查点，编辑用户消息，再以 `resume_if_exists=true` 调用 `run()`；仅当这次新执行中断时才 resume。它不是 resume 暂停 fork 的示例。

`ChatMessage` / `ChatTool` 和 JSON 只是 portable projection，不是 native 权限。Portable 格式仍为 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真实 C++ checkpoint sidecar 保留内存 native seal。持久 native 历史需要 host-owned `sp::NativeArchive`：closed v3 / `spna3` 使用独立密钥提供经认证的 owner-private custody；archive v2 被拒绝，不升级或解释。认证绑定全部 semantic descriptor 选择（origin/path/header、policy、请求 field mapping、usage path、stop mapping）、owner 和精确 custody binding。这不是加密或 vendor-issuer 认证；不得公开 archive 正文、密钥、native blob 或 raw wire 观测。Archive 是证据存储，不是资金 grant 或 spending lease。Program/external bank 仍由独立 journal 拥有，复制 snapshot 不能创建 credit。

Provider 历史有不同模式。同一路径的 native continuation 在原 binding 下保留真实 reasoning、signature 和有序 tool group。Gemini 默认为 `NativeOnly`；显式 `PortableForeign` 接纳调用方创建且不带 native seal、wire output 或 signature 的 assistant text 和 tool call。只有第一个外部 function call 获得 Google 文档规定的 bypass marker；text-only turn 不获得 signature。此 projection 不授予 native 权限，也不会修复失败的 native seal 或将其降级为 portable。它不会让任意跨 vendor 历史都具有 native 可移植性。

Responses `previous_response_id` 选择 provider 保存的会话状态；请求只携带新输入。Client-tool 所有权需要本地证据时，`previous_response_history` 提供真实的先前所有权证据，不作为重复输入发送。Cursor 既不是完整 native replay seal，也不是 archive 权限，仍受 origin、route、model、configuration 和完成状态检查约束。

当前 SDK interface revision 和 shared-library generation 均为 4；使用方必须以匹配的 header 和 library 重新构建。Output generation cap 与 native replay configuration 分开，在每次调用中接受 admission 和 accounting。提高新 semantic call 的 cap 不会续期原 bank、grant 或 deadline。除明确文档化的 per-turn 选择外，content、prefix、origin、route、policy、tools 和 reasoning controls 的 binding 保持不变。Portable JSON v2 与 native archive v3 / `spna3` 不变；下方历史 ABI3 测量不是 interface4 结果。

Python 公开与 C++ 相同的所有权 request/outcome 边界：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。Provider 历史使用带 typed part 的 `ProviderMessage`；`ChatMessage` 仍是图的便利 projection。SDK 失败可通过 `ProviderOutcome.failure` 读取，host observer/settlement 异常保留 `outcome` 和 `cause`。构造器及 GIL/回调行为参见 [Python binding 指南](python-binding.md)。

`input_total`、`output_total` 和 `total` 等用量计数器为 `std::optional<sp::Count>`；存在的 count 有 `uint64_t value` 和 `Evidence`。`Usage` 还记录 stage、quality 和 conflict。缺失表示未知，不应伪造零值。

`UsageAccumulator::snapshot()` 返回累计报告。`total_tokens_wide()` 返回已计费 token 与未解决预留之和，不能把它显示为报告用量。结算要求具有 input/output count 的 final、consistent 报告，并计入有依据的最大 total，不截断超额用量。任何累计报告缺少 counter，汇总该 counter 也为未知。预留、本地计费和 vendor 发票是不同的记录。

**Standalone bank journal 修正——当前契约已修订；实际 runtime 证据如下。** Owner-approved protocol 要求单调 trusted-store namespace obligation，以及真实不可变 original owner/thread/graph scope、ceiling、deadline/clock identity、generation。只有对全部 checkpoint commitment/revision 的精确 durable head CAS 才可发放 host-owned opaque lease。精确 pending effect window 必须在 provider I/O 前持久化；结算必须采用真实 SDK outcome 及实际 charge、nullable report、hold、dedup identity。Checkpoint 与 next head 必须在同一 owned actor/revision 下原子 publish。删除 bank metadata、prune checkpoint、replay old authenticated snapshot、覆盖同一 ID 或失去 actor 都不能授予 credit。已有 65 hold 时将 ceiling 130 降至 129，不能再批准另一个 65；已证明 no-effect 的失败可 release unchanged head，使 authentic 130 恢复仍可进行。Crash/unknown/lost-lease window 保持 hold，不 refund/retry/fallback。Plain/pristine archive 配置不授予 money/native spending lease；当前 `config.usage` 不能替换既有 standalone obligation，Program/external-bank journal 所有权不变。这是要求契约。实际 currency/custody 证据与 instrumentation 限制见下文，不是稳定 released API 保证。

**当前声明；集成 runtime 证据如下:** `<neograph/graph/checkpoint.h>` 声明 `ManagedBudgetLeaseScope`，包含 `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity`。`OwnedManagedBudgetLease` 暴露 read-only `scope()`、`actor_id()`、不可变 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()`，没有公开 authority-import constructor。`ManagedBudgetEffectReceipt` 暴露 `active()`、`effect_id()`、`claim_amount()`、`request_digest()`；default receipt 不授予权限。`CheckpointStore` 声明 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` 及 `_async` counterpart。Sync `CheckpointStoreCore` 与 `AsyncCheckpointStore` 暴露各自 variant。`managed_budget_checkpoint_commitment(checkpoint)` 绑定完整持久 checkpoint，而非仅 bank JSON。这些声明不证明 backend CAS、currency 安全性、installed ABI 兼容性或实际成功的 runtime 路径。

**真实 InMemory shared-bank fork 已保留并实证。** 原始真实 C++ fork 使用 ONE original financial journal 和 trusted current branch head，不复制 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及 `_async` 要求 authentic current source/full commitment 与实际 same-bank native C++ pointer；durable standalone fork 仍明确 unsupported。`OwnedManagedBudgetLease::scope()` 及 original owner/thread/graph、ceiling、deadline/clock、generation 保持不可变。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` 单独选择 execution branch；`GraphState::budget_original_thread_id()` 标识原始 financial bank。精确 selected-branch head CAS 和 global actor/revision 将所有 branch 对 canonical current counter、pending effect、burned identity 串行化。Original/fork branch 保持可用但不补充额度。Stale snapshot、checkpoint copy、imported JSON 不能发放 alias 或回退 head。原始 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 证明在未修改 test_graph_engine.cpp:810–913 中 PASSED；saved original ceiling30 不同于 effective fork ceiling20；widening31 和 JSON-only restore 必须拒绝。Unbounded reported observation 是事实 data，不是 finite grant。只有已证明 zero-effect 的 lease 能 release unchanged head；unknown/pending effect 保留 obligation。

**当前 release-error 契约；实际 suite/probe 如下。** `<neograph/graph/engine.h>` 中 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始 execution exception，`release_error()` 暴露次要 durable lease-disposition 失败。`outcome()` 在存在真实 SDK 证据时保留它，若没有 SDK outcome 则为 null；release 失败不能伪造结果或授权重新 dispatch。Closed `_neograph_managed_budget_scope` metadata 描述原始 logical scope/cap/deadline clock/generation，但只是 data，不是 backend CAS 权限。

**Archive-owner/retention 契约；实际 suite/probe 如下。** 只有 finite standalone root 或 authenticated finite source 才从真实配置的 `sp::NativeArchive::owner_scope()` 继承省略的 original owner；unbounded/plain owner metadata 语义不变。显式冲突的 archive owner 在 lease acquire 前拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及对应 Core/Async storage capability 默认 false；真实 InMemory backend override 为 true，wrapper 必须委托真实 retention。此 read-only 描述允许合法 unleased/plain/unbounded C++ native checkpoint custody，但不授予 spending credit 或 native replay authority。Leased custody 使用真实 store-issued receipt，而非 JSON flag 或猜测的 store type。

**Native-custody pre-I/O gate；实际 suite/probe 如下。** Managed effect begin 在任何 pending-effect/slot/held-window 修改前要求真实绑定的 NativeArchive 或实际 local store-issued private C++ retention capability。Private capability 不从 JSON import，也不经 wire 传输。C++ sidecar 无法跨越边界，因此即使 remote backend 是 InMemory，gRPC 仍要求真实 client/server archive。Archive 未提供 finite source owner 时，原始 anonymous owner scope 保持空值；真实 archive binding 必须匹配 original scope。Financial head/lease 证据本身不证明 native-custody readiness。

`ProgramFailure` 保留 live `provider_outcome`、`provider_cause`。Canonical factual SDK witness 将真实 archive custody 绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复后的失败前立即恢复配置的 custody。公开 data-only `ProgramResult::create()` 不能用预填 witness 绕过；未解析完的 parsed seal 不是可执行结果。进程重启后原始 exception pointer 不可用（`provider_cause == nullptr`），不会从 text 重建。无法持久化的失败不能 serialize/publish/replay。

`RecordedBindingSet` 是 source-bound move-only data，不是调用方提供的 dispatcher。可信 Catalog `recorded_capability_binder` 独立读取真实持久 source event，materialize captured-only capability。`ProgramRuntime::replay_recorded()` 检查原始 selected-source permission，再通过 durable CAS 转移真实剩余 bank；inherited spend 不是新的 model grant。旧 `start_recorded` 续期 API 已删除。InMemory/File/SQLite/PostgreSQL Program store 在整个执行期间保留精确不可变 owned lease，不因 expiry 续期。Controlled JavaScript 仍验证 underlying capability manifest，消费精确 completed command 结果，不重新 dispatch external effect。

**Recorded-control causal fix 已在 full suite 实证。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Reservation 选择认证 settlement transition，而非曾拒绝首个新 Core checkpoint 的普通 Running→Running transition。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化；既有 Recorded CPU/Memory await/handoff scenario 在 full suite pass；remote TSan coverage 限制如下明确保留。

以下观察记录于本次文档整理之前。它们是历史证据，不是新测试运行，也不保证所有 platform、transport 或 security 属性。

**付费观测已完成；不是普遍 qualification。** 原始 `SPQUAL1` base630/1000000 microUSD 不变；同一原始 ledger 中 ONE hash-chained `A` 接纳批准的 extension480/3000000，aggregate1110/4000000。Calls/spent/hold/settlement 累积，不产生新 grant ID/header/reset。精确 declaration byte/file identity 和 original authorization/baseline/catalog/activation/ledger-prefix hash/totals 仍固定；删除、替换、变更均 fail closed。最终 canonical ledger 为 calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 是 LOCAL catalogue meter，不是 invoice。记录的 five-family60-pair baseline 完成600 request：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4次）、Interactions57/60（incorrect-vision buffered1次/SSE2次）；合计293/300 pair，不是300/300。其他 old600 financial record 保留，但不是完整 behavioral proof。此前 M5/media one-shot cohort 不变。此前 Google3-round prerequisite 保留 invalid-tool2次/unreadable-positive1次失败状态。不批准更多付费调用。最终 SDK 证据与 native-axis 限制不同于 baseline 成功。 此前 activation/reopen smoke 保留为两次 reopen 后 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass；这是限定的历史 checkpoint，不是最终 ledger totals。此前验证的 Chat60-pair cohort 保留实际 attempt120、UpperBound charge120、无 UnknownHold。

**Native-axis 观测不是 cryptographic 验证或 native consumption/equivalence。** Generate 接纳 mutation/omission/duplication。Interactions 接纳 isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication。删除全部 thought/signature 返回 generic400；保留 THOUGHT item 而删除全部 signature field 也返回 generic400。最后一次 capture 只有 local encoded-original retention control，没有 same-capture server positive；此前 positive cohort 仍是真实证据。这仅建立 aggregate-carrier-absence boundary，不证明 issuer/signature 验证或 vendor consumption。实际 report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`；prerequisite-failed/not-run/negative-inconclusive 状态保持为事实。 Thought-only/carrier-only omission 在仍有其他 carrier 时被接纳；这不加强 issuer-validation/native-consumption 声明。

**实际集成证明及剩余限制。** 最新 Core full run：2242 test、失败0、skip16（RAM process-loss 不适用14项/live-credential gate2项）、130.17秒。`PgNestedJsonRoundTrips` 精确保留 duplicate key/order/null metadata、blob、residual，0.18秒 pass。未修改的原始 shared-bank fork 和既有 Recorded CPU/Memory await/handoff scenario 均 pass。真实 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe 在 plain 和 ASan+UBSan pass。LOCAL Memory/SQLite/PostgreSQL TSan scope7项 pass、warning0。包含 system Abseil/Protobuf 的 full mixed gRPC TSan 为 exit66，dependency/generated-RPC stack 有 race warning402项。这是 instrumentation/coverage 限制，不是已证明的 false positive；不声称 remote TSan/race-free，不 suppress warning。Installed find_package Program C++/C ABI/dualQuickJS3个 consumer pass。Fresh installed NeoGraph/SchemaProvider typed consumer 实际2个 HTTP request、coroutine 开始前 provider 销毁、native/tool replay、refusal、known-zero/raw 保留、实际 LinkedMismatch 拒绝均 pass。Browser Alice/Bob isolation、generation2 replacement 已实际目视验证；PostgreSQL Program Chat black-box6项18.989秒 pass。最新 SDK26/26、失败0、74.07秒 pass。最终 ReleaseGraph16配置 ×fresh process3次/48记录以38.29秒、失败0、全部 actual protocol/owned-outcome check pass 完成。NeoGraph `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保留独立最终 cohort。测量期间未执行 compiler/付费 model；历史 cohort 不变，不声明 semantic/resource equivalence。Unstable SDK/ABI3 不是稳定 release 或更广 platform qualification。

Host 交付 limit、extent-bounded 诊断/raw 证据、共同 provider error 及最小 media 证据见 [typed provider reference](reference-en.md#owned-outcome)。

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

> 旧 issue #16 曾观察到部分 glibc/OpenSSL 组合在每请求嵌套 `io.run()` 与旧 child-thread provider streaming bridge 下发生 `getaddrinfo` SEGV。该 bridge 已由 typed 切换删除。当时结构测试未完整重现 downstream HTTPS/sanitizer/load 条件，不是当前验证。当前调用方使用显式 `ProviderMode` 与 `invoke_async` / `dispatch_async`，应从既有长寿命 executor 驱动，而非嵌套每请求 loop。不得伪造单个 token 或重发 completion 作为绕过。

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer` 与 `neograph_engine.openinference.openinference_tracer` 把 graph event 转为 run/node span。后者记录 `CHAIN` 标签和 node payload projection。每次 run 选择一个 graph callback。要把模型调用记录为 `LLM` span，应在 graph compile 前用 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` 包装 typed provider。Wrapper 使用 native C++ observer 及继承的 `prepare`/一次性 `dispatch` 或 `invoke`。准备或丢弃 request 不打开 span；已准入 dispatch 打开 span，保留原 owned outcome、取消、deadline 与 typed event。Tracer 失败不替换 provider 结果或异常。

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

Local Phoenix endpoint 使用 `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest`，并安装 `opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`。将 graph specification、现有 provider、明确的 model 与 `RunConfig` 传给 `trace_graph`。`ParentContextTracer` 明确将 run root 传入 worker dispatch，不保证自动 cross-thread 或 node 级 parent 传播。Python 使用 dispatch 时的 active OTel context，prepared operation 在其生命周期内保留 tracer adapter。

LLM span 仅含公开 role/text projection、已声明 scalar 和已知 usage count。已知零值会记录，未知值被省略。Native replay/reasoning、raw wire envelope/event 和 encoded request body 不进入 trace；真实 custody 保留在 request/outcome。包括失败 partial report 的 usage 属性不能证明 vendor charge 或 budget authority；charged/reserved accounting 由 `UsageAccumulator.authority_snapshot()` 与 Program 的 `provider_budget_authority` 负责。

公开 text、异常消息及 graph payload 仍可能含 application secret。应选择或 redact exporter 接收的数据，参见 [OpenTelemetry 敏感数据指引](https://opentelemetry.io/docs/security/handling-sensitive-data/)。[OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) 定义 `CHAIN` 和 `LLM`；[参考](reference-en.md#105-observability--opentelemetry--openinference)列出 NeoGraph 的属性 subset、token event、Python typed 调用示例及 C++ 生命周期要求。

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

### 读取 Python RunResult status 与 state

`result.status` 公开 typed `Completed`、`Interrupted`、`StepLimit`、`SafePoint` 状态。`result.output` 是 portable 最终 state；`result.interrupted`、`result.max_steps_exhausted`、`result.execution_trace` 描述 run。`result.native_messages`、`result.provider_outcomes` 保留完整 typed provider 证据。参见 [Python binding 指南](python-binding.md#hitl-and-state)。

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
