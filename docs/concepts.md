# NeoGraph core concepts — a narrative guide

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

Read this before the examples. The sections follow the order used to build a graph: channels, nodes, edges, fan-out, routing, checkpoints and streaming.

If you know LangGraph, you will recognize channels with reducers, `Send`, `Command` and checkpoints. NeoGraph's [Core and ProgramRuntime](../README.md#core-and-programruntime) have separate responsibilities; this guide starts with Core graph execution. Python provider calls use the typed [binding contract](python-binding.md), rather than the removed completion classes.

---

## Table of contents

(Section 8.5 added in v0.6.0 — `Tracing — OpenTelemetry + Phoenix / Langfuse`.
The numbered headings stay 1-9 to keep external docs links stable;
8.5 sits between Streaming and Common pitfalls.)


1. [The big picture](#1-the-big-picture)
2. [Channels & reducers](#2-channels--reducers)
3. [Nodes](#3-nodes)
4. [Edges & conditional routing](#4-edges--conditional-routing)
5. [Send — dynamic fan-out](#5-send--dynamic-fan-out)
6. [Command — routing override + state patch](#6-command--routing-override--state-patch)
7. [Checkpoints, interrupts, HITL](#7-checkpoints-interrupts-hitl)
8. [Streaming events](#8-streaming-events)
9. [Common pitfalls](#9-common-pitfalls)

---

## 1. The big picture

A NeoGraph **graph** is four things:

| Thing | What it is | Defined by |
|---|---|---|
| **Channels** | Named slots in the shared state. Each has a reducer that defines how new writes combine with existing values. | `definition["channels"]` |
| **Nodes** | Functions that read state, emit writes (and optionally `Send` / `Command`). | `definition["nodes"]` |
| **Edges** | Static next-node pointers. | `definition["edges"]` |
| **Conditional edges** | Predicate-driven routing — picks one of several next nodes based on state. | `definition["conditional_edges"]` |

Execution is a **super-step loop**:

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

A normal ready batch reads the channel state from before that batch's updates. A sibling cannot read another sibling's returned writes during execution. The engine buffers those writes, folds them through reducers after the batch, then evaluates routing on the updated state. This is graph scheduling; the engine does not synchronize a model's internal computation.

For example, if `counter` starts at 0 and two ready nodes both return `counter + 1`, both read 0. An overwrite reducer produces 1, not 2. A custom sum reducer can instead combine two increment writes of 1 into 2. Every `Send`, one or many, runs on an isolated state copy with its own payload applied, so a payload never reaches the shared state (#389). Reducer order does not make model responses or external effects reproducible.

---

## 2. Channels & reducers

Every piece of state lives in a named channel. By default channels keep their
materialized value across super-steps and checkpoints; nodes communicate by
writing to them. Combination, array retention, and checkpoint inclusion are
independent decisions.

### Defining channels

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### Built-in reducers

| Reducer | New write semantics | Typical use |
|---|---|---|
| `"overwrite"` | New value replaces old. Last-writer-wins on parallel writes. | Single-value scratch (current node, current question, route hint). |
| `"append"` | New list (must be a list!) is concatenated to the existing list. Previous-step values come first; parallel nodes fold in scheduler-ready order (not completion order), then writes in each node's returned order. | Conversation messages, search results, fan-out collection. |

> Both reducers are registered in `ReducerRegistry::ReducerRegistry()`
> at engine startup ([`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)).
> Custom reducers register from C++ via `ReducerRegistry::register_reducer(name, fn)`
> or from Python (since v0.1.9):
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> The Python callable runs under the GIL; concurrent Send fan-outs
> serialise on it the same way Python custom nodes do. Re-registering
> a name replaces the previous reducer.

### Channel lifecycle and checkpoint contract

The reducer combines writes. Array retention is a separate policy: `unbounded` (default), `latest`, or `bounded` with a positive `retention_limit`. Retention trims arrays after every write, including `ChannelWrite.Mode.Overwrite`; `latest` keeps its last element until another write replaces it. Persistence independently selects `checkpoint` (default, materialized value and version) or `ephemeral` (omit both from durable checkpoints). Bounded retention changes the visible history, not merely storage size.

The engine folds each node's writes in returned order, static batches in scheduler-ready order and multi-`Send` results in invocation order, regardless of finish order. Pending writes replay into those same task slots. Overwrite is ordered last-writer-wins; append preserves element order. Custom reducers should be pure and stable under replay. Associativity is needed if regrouping must preserve the result; commutativity is needed only for order independence. Explicit overwrite bypasses the reducer, then applies retention. None of these rules guarantees reproducible model output or external effects.

Ephemeral values remain live across supersteps; they do not reset per step. Every checkpoint records the declared ephemeral names and whether they were written, but not their values. Resume, `resume_if_exists`, exact-ID resume and state updates reject written ephemeral state, a missing guard in an old checkpoint, or a changed ephemeral channel set. A checkpoint before the first ephemeral write may resume with pending writes replayed in the documented order. `update_state` refuses ephemeral writes. Multi-`Send` in-process workers inherit live ephemeral values in their isolated copies. Keep correctness-critical state checkpointed, or reconstruct it from durable inputs in a fresh run.

`GraphState::restore` rejects graphs with ephemeral channels. Use `restore_checkpoint` with the matching guard, or `restore_runtime` for a same-process snapshot containing every live value and version. The guard uses checkpoint metadata; it does not change the channel blob layout or store schema. Old full-value checkpoints still work for graphs without ephemeral channels. Before downgrading to a binary without these guards, drain or restart ephemeral threads and forks from durable inputs; older readers cannot enforce the additive guard.

Checkpointed channels use full materialized snapshots. Memory, SQLite and PostgreSQL deduplicate unchanged `(thread, channel, version)` values, but append history changes version on every write and still grows the snapshot. Pending writes record successful tasks of an incomplete superstep; they are not general channel deltas. A per-step reset policy is unavailable: its safe design would need to define the reset after writes and routing, plus interrupts, replay and Send behavior. It must not be confused with ephemeral persistence.

Measure the baseline with `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite`; add `postgres` and `--pg-url` only for an isolated local database. Rows report logical serialized bytes, save/load p50/p95 and reconstruction depth; legacy rows report blob count. For allocation requests, run `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`. Native JSON/SQL allocators are not all intercepted by C++ `operator new`. Compare identical payloads, histories and backend setups, using measured rather than estimated savings. Logical bytes differ from durable physical bytes. SQLite uses a unique temporary database removed on exit; `--sqlite-path` retains its new file and refuses an existing path.

One recorded Linux x86-64 Debug baseline used one thread, 256 history steps, 512-byte messages and one iteration. These are historical measurements, not performance targets:

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

A separate repeat measured SQLite database/WAL sizes of 14,811,136 / 4,210,672 bytes (19,021,808 total). A Linux `LD_PRELOAD` shim counting process-wide `malloc`, `calloc` and nonzero `realloc` requests, including construction and JSON parsing, measured 88,277 extra requests / 605,289,027 requested bytes for memory and 114,295 / 867,319,964 for SQLite against `--history-steps 0`. These are cumulative requests, not live memory or store-only allocations. Aligned/internal allocations were not intercepted. Use supported profilers and multiple warm runs before drawing conclusions.

### Writing to channels

A node returns a list of `ChannelWrite`s:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
]
```

The shape of the value must match the reducer:
- `"append"` → must be a list (will be concatenated).
- `"overwrite"` → any JSON-serializable value.

### Reading state from a node

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)` returns the channel's current value, or `None` if
the channel exists but hasn't been written to yet. For typed access to
chat messages, `state.get_messages()` returns `list[ChatMessage]`
(parsed from the `messages` channel) — used internally by `llm_call`.

### Versions

A run keeps one write counter, and all channels share it. Every write
to a channel adds 1 to the counter and stamps that channel's `version`
with the new value, so a channel's `version` is the counter's value at
its last write. The writes of the run input count as well as those of
the nodes, and a write that leaves the value unchanged counts too.
Versions therefore rise with every write and no two channels share one,
but a `version` is not the number of writes the channel received. Three
nodes that each write `last` (overwrite) and `all` (append) leave `last`
at version 5 and `all` at version 6, and `output["global_version"]`, the
counter after the last write, at 6. Neither number is a super-step
count (`Checkpoint.step` holds the super-step). A channel that was never
written has version 0, also when it has an `initial` value.

Checkpoints store each channel's version and the counter. A resumed or
forked run restores them and goes on counting, so the numbers continue
across the pause instead of starting over. Checkpoint stores key a saved
channel value by `(thread, channel, version)`, and
`state.channel_version(name)` and `state.global_version()` read the
numbers. You usually don't read them directly.

---

## 3. Nodes

Three ways to register a node type, in increasing order of control:

### 3.1 Built-in nodes

| `type` (in JSON) | What it does | Configuration |
|---|---|---|
| `llm_call` | Prepares one owned typed request, gates/reserves/receipts it, dispatches the same handle and retains every ordered message plus the full result. | Reads `provider`, `model`, `instructions`, `tools` from `NodeContext`. |
| `tool_dispatch` | Looks at the latest assistant message's `tool_calls`, executes each via `Tool::execute`, appends `{role: "tool", tool_call_id, content}` results. | Reads `tools` from `NodeContext`. |
| `intent_classifier` | LLM classifies user intent into one of N labels and writes the chosen label to `__route__`. Pair with `route_channel` conditional. | `extra_config: {labels, prompt_template}` |
| `subgraph` | Embeds another graph as a single node. Inner state is mapped through configured key remappings. | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 The `@ng.node` decorator (Python only)

The shortest way to define a write-only node:

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

The decorated function must return a `list[ChannelWrite]` (or `None`,
treated as `[]`). It cannot emit `Send` or `Command` — for those,
subclass `GraphNode`.

### 3.3 The full `GraphNode` subclass

Override `run(input)` for full control. It was introduced in v0.4.0 and is
the only custom-node entry point from v0.9.0 onward — one method, one
signature:

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

Python exposes `cancel_token`, `usage`, `thread_id`, `step`, `stream_mode`, `store`, `resume_value`, `trace_id`, `run_id`, `model_token_budget` and typed provider evidence on `input.ctx`. Deadline inspection uses `has_deadline` and `deadline_remaining_ms`; the raw C++ steady-clock value is opaque. C++ callers supply deadline and trace metadata through `RunMetadata`, which propagates into nested subgraphs.

You can also return a bare `list[ChannelWrite]` when you don't need
`Send` or `Command` — the binding lifts it into a `NodeResult`
automatically.

> **Migrating from v0.3.x:** the removed pre-v0.4 multi-entry node API has one
> replacement: override `run(input)`. Read state from `input.state`, emit tokens
> through `input.stream_cb` when non-None, and read the cancel token from
> `input.ctx.cancel_token`.

Register the type so the JSON loader can instantiate it:

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

The factory sees `(name, per-node config, NodeContext)` so the same
class can be instantiated under multiple names with different configs.

### 3.4 Tools (separate concept, used by `tool_dispatch`)

`Tool` is not a node — it's something `tool_dispatch` invokes. Subclass
`ng.Tool`, override three methods, pass instances into
`NodeContext(tools=[…])`:

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
    def execute(self, args):  return str(args["x"] * 2)
```

The engine takes ownership of the tool list at compile time — your
local references can drop afterwards.

---

## 4. Edges & conditional routing

### Static edges

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

Multiple edges from the same source node fan out (every successor goes
into the next super-step's ready set). Two edges to the same target
from one super-step deduplicate to one execution of the target.

### Conditional edges

A conditional edge runs a **named condition** and picks the next node
from the `routes` map:

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

The condition name resolves to a `ConditionFn` registered in the
engine. Two ship as built-ins:

| Condition | Returns | When to use |
|---|---|---|
| `has_tool_calls` | `"true"` if the latest assistant message has non-empty `tool_calls`; `"false"` otherwise. | ReAct loops — keep dispatching tools until the LLM stops asking. |
| `route_channel` | Whatever string is in the `__route__` channel; falls back to `"default"`. | Pair with `intent_classifier` for explicit intent routing. |

Custom conditions register from C++ via `ConditionRegistry::register_condition(name, fn)`
or from Python (since v0.1.9):

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

The callable receives the live `GraphState` (so `state.get(channel)` and
`state.get_messages()` work) and must return a string matching one of
the conditional edge's `routes` keys.

### Two equivalent forms — both work since v0.1.8

Conditional edges may live either inside the `edges` array (with a
`condition` field) **or** in a separate `conditional_edges` block.
Both forms are accepted; pick whichever is clearer:

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

> **History:** form A was silently dropped by the graph compiler before
> v0.1.8 — the README and every Python example used it, so ReAct loops
> degenerated to a single LLM call. Fixed in commit `e23a523`. If you
> see this on a wheel ≤ 0.1.7, upgrade.

---

## 5. Send — dynamic fan-out

`Send` lets a node choose a runtime-dependent number of target invocations, such as one researcher per topic. The engine executes emitted Sends after the ordinary ready batch has returned and its writes have been applied, within the same numbered superstep.

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### Mental model

The engine invokes the compiled target once per `Send`; it does not promise a fresh node object. Send targets therefore need safe member-state handling under concurrent invocations. Each payload is applied before its target reads channels. Every Send, whether the step dispatched one or several, runs on an isolated copy of the post-ready-batch state with its own payload applied, so a payload is visible only to its own Send execution, each retry attempt included, and never to the next step or the final state (#389). The returned writes are folded in invocation order after all Sends finish. Routing then combines ordinary-node and Send-target signals for the next ready batch.

A Send execution cannot emit Send: when one returns Sends, the run fails with an error that names the node and the targets, before any result of the step is applied. To fan out again, write the items to a channel and let an ordinary node that the step routes to send them.

A Send target that raises `NodeInterrupt` pauses the run like a node of the ready batch. Its `NodeInterrupt` checkpoint re-enters the whole superstep: it holds the state the step started from and lists the step's ready nodes as next nodes, and the paused result reports that state. Resume runs the step again. Each ready node and each Send that finished replays its recorded result and is not run, or charged, again; the Send that paused, and any other Send that did not finish, runs with the resume value under the same task id, so a provider call its earlier attempt journaled replays. When no checkpoint precedes the step, for example for a map node at the entry, the finished results are recorded under the interrupt checkpoint itself.

### Common shape: fan-out 5, fan-in to summarizer

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`'s outgoing edge is just `{"from": "researcher", "to": "summarizer"}`
— same dedup rule as static edges, so summarizer runs once.

### The execution trace

`RunResult.execution_trace` has one entry per node execution that completed. A Send execution is listed as `<target>[send]`, so a node that runs by Send and by an edge shows two kinds of entry. The entries follow the order in which the engine applied the results: super-steps in order, and within a step the ready nodes in ready order, then the Sends in the order they were emitted (the ready nodes' order, then each node's own), whatever order the executions finished in. The shape above gives `["planner", "researcher[send]", "researcher[send]", "researcher[send]", "researcher[send]", "researcher[send]", "summarizer"]`.

A step that paused on a `NodeInterrupt` lists nothing, and the run that resumes it lists the whole step, replayed executions included. A subgraph is one entry; its inner nodes are not listed. A Program attempt that fails reports the executions that completed before the failure, in the order they completed, with the same names: a Send execution is `<target>[send]`, from the `"send": true` that its `NODE_END` event carries.

### A Send target needs no edge into it

A node that only `Send` starts has no incoming edge, and that is a supported shape. The validator cannot tell: a graph definition does not declare which nodes a `Send` or `Command.goto` will target, so a graph with `schema_version` 1 prints lint E7 (the node is unreachable from `__start__`) to stderr when it is built. For such a node the notice is expected. Turn it off with `EngineConfig::stderr_notices = false` in C++ or `GraphEngine.compile(..., stderr_notices=False)` in Python. The switch also covers the other build-time lint and the one-time notice that a fan-out ran serially; validation errors still throw.

Do not add an edge to quiet the lint. A static edge from the sender to a Send target makes the target run once more after the Sends, on the shared state, where no Send payload ever landed. A `fan` node that sends `worker` three times and also has an edge to `worker` gives the trace `["fan", "worker[send]", "worker[send]", "worker[send]", "worker"]`, and the last `worker` reads `item` as `None`.

### Worker-count tuning

`build()` defaults to `EngineConfig::worker_count == 1`, so it creates no engine-owned thread pool and dispatches branches on the caller's coroutine executor. Coroutine I/O can overlap; CPU-bound work may serialize on a single-thread executor. A multi-thread caller executor or concurrent runs still require safe node member state.

For real parallelism, opt into a pool explicitly. Pick exactly N to
match your fan-out width, or use `set_worker_count_auto()` for
`hardware_concurrency()` (with a fallback of 4):

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

When the branches of a multi-Send (or multi-outgoing-edge) fan-out have
run one after another because no pool is opted in, NeoGraph emits a
one-shot stderr warning once the fan-out has finished, so the
silent-serial case doesn't fly under the radar. Coroutine branches that
wait on I/O overlap on the one thread and are not reported. Suppress with
`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` if you intentionally drive
serial fan-out (e.g. a benchmark of the worker=1 fast path).

---

## 6. Command — routing override + state patch

`Command` lets a node decide where to go next AND mutate state in the
same return value. It bypasses the regular outgoing edges.

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

### When to use Command vs conditional edge

- **Conditional edge**: routing depends on a state predicate that
  doesn't need node logic. Cleaner, declarative.
- **Command**: routing depends on logic that's most natural to write
  inside a node — multi-criteria scoring, content inspection, retry
  decisions. Also the only way to atomically update state AND choose
  the next node.

### Last-writer-wins under fan-in

If several siblings return a nonempty `Command.goto_node`, the last command in the supplied routing order wins and overrides ordinary edges and barriers. Static batches supply ready order; multi-`Send` supplies invocation order, not completion order. All returned command updates still merge through the write pipeline. Prefer one routing decision-maker when conflicting commands would change the workflow.

---

## 7. Checkpoints, interrupts, HITL

### Setting up a checkpoint store

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

With a store attached, every super-step writes a checkpoint to the
store keyed on `(thread_id, checkpoint_id)`. The `RunResult.checkpoint_id`
field is the latest one.

### Static interrupt points

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

The engine returns a `RunResult` with `interrupted=True` and
`interrupt_node` set. To resume:

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

A resume skips only the gate of the interrupt it answers. Resuming an `interrupt_before` pause runs the node it held, and resuming a `NodeInterrupt` continues its step. Any other resume, such as one after an `interrupt_after` pause or recovery from a step's checkpoint after the process stopped, still pauses before an `interrupt_before` node it is about to run. The resume value goes to the node an `interrupt_before` pause held and to no other node; a resume after an `interrupt_after` pause, or from any other checkpoint, gives it to no node (#393). Several gated nodes that are ready in one step are answered one at a time (#396): a resume answers the gate the run paused at, and before the step runs, every other gated node of the step whose gate is not answered pauses the run again with its own `interrupt_before` pause. Once every gate of the step is answered, the step runs, and each gated node receives the answer to its own gate.

### Dynamic interrupts via `NodeInterrupt`

Throw from inside a node body (Python: `raise ng.NodeInterrupt(reason)`,
C++: `throw NodeInterrupt(...)`). The engine catches, persists state,
returns a `RunResult` interrupted at the throwing node — same
resume API. This holds for a node of the ready batch and for a `Send` target
alike, on every sync, async and streaming entry point. The siblings that
finished in the paused step replay on resume instead of running again, also
in a step that no checkpoint precedes.

Several interrupts of one step are answered one at a time (#388). When several nodes or Sends of one super-step raise `NodeInterrupt`, the run pauses on all of them: `RunResult.interrupts` lists each as `{id, node, value}`, where the id is the paused invocation's task id (`s3:review`, or `s3:send[1]:worker:<hash>` for a Send), and `interrupt_node`, `interrupt_value` and `interrupt_id` report the first. A resume answers the interrupt that `interrupt_id=` names (C++ `RunMetadata::resume_interrupt_id`), or the first one; every other paused node or Send of that step runs again without the value and asks its own question again, and the next resume answers it. Naming an interrupt that is not pending raises `ValueError` (C++ `std::invalid_argument`) before anything runs. A checkpoint that paused on several interrupts lists them in its metadata (`_neograph.interrupts`); one that paused on one is unchanged. The value reaches only the answered invocation of the paused step (#393): every node of a later step of the resumed run sees no `ctx.resume_value`, so a node or a tool gate that asks there pauses the run again instead of taking an answer to another question. A subgraph resumed through its parent receives its answer through the parent's answered node. A graph with a `messages` channel still receives the value there as a user message. JavaScript joins and plan-level `parallel` and `quorum` follow the same rule: one paused member is answered per resume, and the others ask again.

A node can tell which interrupt a resume answers (#406). `ctx.resume_interrupt_id` (Python `input.ctx.resume_interrupt_id`) is the id of the interrupt that `ctx.resume_value` answers when it answers the node's own `NodeInterrupt`: the id that `RunResult::interrupt_id` and `RunResult::interrupts` report, whether the resume named it or answered the first one. It is empty (Python `None`) when the value is the approval of the `interrupt_before` gate that held the node, and whenever there is no value. The delivery rule is unchanged: the gate's approval still reaches the gated node (#393). A node behind a gate that also asks its own question checks the id, so that the approval to run is not taken as the answer to its question:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    const auto& answer = in.ctx.resume_value;
    // Nothing yet, or the approval of the gate that held this node.
    if (!answer || in.ctx.resume_interrupt_id.empty())
        throw NodeInterrupt("deploy needs a target", json{{"ask", "target"}});
    NodeOutput out;
    out.writes.push_back(ChannelWrite{"target", *answer});
    co_return out;
}
```

```python
def run(self, input):
    answer = input.ctx.resume_value
    if answer is None or input.ctx.resume_interrupt_id is None:
        raise ng.NodeInterrupt({"ask": "target"}, reason="deploy needs a target")
    return [ng.ChannelWrite("target", answer)]
```

A step whose siblings end differently ends by one rule, whatever their index or completion order (#392). It holds for the branches of a fan-out and for the Sends of a step, with one worker or many. Cancellation of the run stops it. Otherwise an uncertain effect, a `NodeInterrupt` whose payload `kind` is `tool_effect_reconciliation`, pauses the step for reconciliation even when a sibling failed, because ending the run would abandon an effect whose outcome nobody knows; the pause reports only those interrupts, and the failed sibling, which recorded no result, runs again when the step is resumed. Otherwise a failure fails the run with the first failure in dispatch order (ready order, then Send order): the step's ordinary interrupts are not reported, and no checkpoint is saved for them. Otherwise the step pauses on every interrupt, as above. A failure routed along a failure edge is the node's result, like a completion: a sibling's interrupt still pauses the step, and the route is taken when the step completes. JavaScript joins and plan-level `parallel` and `quorum` rank their members the same way (#397): a member paused on an uncertain effect pauses the join beside a member that failed, on the first such member, unless a success already decides it (a race winner, a quorum met); the failed member is recorded and ends the join when it is resumed, once the member the resume answers has run to its end and settled its effect (#400). Whenever a join pauses, a member paused on an uncertain effect is asked before one paused on an ordinary question, whatever the declaration order (#401). A collecting `ng.all` (`collect: true`) is the deliberate exception to the rule above: it waits for every member, paused ones included, before it reports, so a member's failure does not end it while another member is paused on an ordinary question. It pauses there instead, and reports the failure once every member has settled (#401).

Useful when the decision to pause depends on intermediate node output
(e.g. "did the LLM produce something worth showing the human?").

### Time travel

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` copies a checkpoint into the caller-named destination thread and returns the new checkpoint ID. An omitted checkpoint ID selects the source's latest checkpoint. The copy retains its pending continuation; editing state does not schedule new work. A fork of a `NodeInterrupt` checkpoint also carries the pending writes of the paused super-step, so resuming the fork replays the nodes and Sends that finished in that step, as resuming the source does, instead of running them and their provider calls again (#390). The source keeps its own writes.

Resuming a completed continuation with `next_nodes == ["__end__"]` restores the stored result without executing nodes. To continue paused work on edited state, select an exact earlier checkpoint from `get_state_history()` that still has pending nodes, fork that ID, edit the fork and resume it. A historical empty `next_nodes` vector is different: latest resume without an exact ID retains the fresh-run behavior, while exact-ID resume stays pinned to that snapshot.

[Example 08](../examples/08_state_management.cpp) keeps its new-turn flow: fork a completed checkpoint, edit the user message, then call `run()` with `resume_if_exists=true`; resume only if that new run interrupts. It does not demonstrate resuming a paused fork.

Checkpoints keep a provider's native history (reasoning, signatures, ordered tool calls) only as C++ sidecars in the process that produced it. `ChatMessage`, `ChatTool` and JSON are portable copies and carry no native authority. A host that must keep native history across restarts configures an `sp::NativeArchive`. The [typed provider reference](reference-en.md#portable-projections) describes these contracts.

---

## 8. Streaming events

`run_stream` / `run_stream_async` invoke a callback as events fire.
Modes are an OR-able bitmask:

| Mode | Emits |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN` for every streamed token from a `Provider` |
| `DEBUG` | `__routing__` events showing the next-ready set |
| `VALUES` | `__state__` events with full state after every super-step |
| `UPDATES` | `CHANNEL_WRITE` events per `ChannelWrite` |
| `ALL` | All of the above |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **Note:** `event.node_name` (not `event.node`). The C++ struct field
> is `node_name`; pybind preserves the original name.

For chat-shaped streaming (LangChain-compatible message dicts with
incremental `content_so_far`), use the helper:

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` placement (C++)

When driving `engine.run_stream_async()` from C++, the outer
`asio::io_context.run()` should be called from your application's main
thread (or any long-lived thread that has been initialized through
the normal process startup path). Tested-good shapes:

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

> Call `invoke_async` / `dispatch_async` with an explicit `ProviderMode`, and drive the work from an existing long-lived executor rather than nesting a per-request `io.run()`. Do not synthesize a single fake token or resend a completion as a workaround.

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer` and `neograph_engine.openinference.openinference_tracer` turn graph events into run/node spans. The latter tags spans as `CHAIN` and records node payload projections. Choose one graph callback per run. To record model calls as `LLM` spans, wrap the typed provider with `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` before compiling the graph. The wrapper uses the native C++ observer with inherited `prepare`/one-shot `dispatch` or `invoke`. Preparing or abandoning a request opens no span; admitted dispatch opens a span without changing its owned outcome, cancellation, deadline or typed events. Tracer failures do not replace provider results or exceptions.

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

For a local Phoenix endpoint, run `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest` and install `opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`. Pass a graph specification, an existing provider, an explicit model and a `RunConfig` to `trace_graph`. `ParentContextTracer` explicitly carries the run root into worker dispatch; automatic cross-thread or per-node parent propagation is not promised. Python uses the active OTel context at dispatch, and the prepared operation retains the tracer adapter through its lifetime.

LLM spans contain public role/text projections, declared scalars and known usage counts. Known zero is recorded; unknown is omitted. Native replay/reasoning, raw wire envelopes/events and encoded request bodies stay outside traces and retain their authentic custody in the request/outcome. Usage attributes, including partial reports on failure, do not establish vendor charges or budget authority; charged/reserved accounting belongs to `UsageAccumulator.authority_snapshot()` and Program's `provider_budget_authority`.

Public text, exception messages and graph payloads can still contain application secrets. Choose or redact what your exporter receives; see [OpenTelemetry's sensitive-data guidance](https://opentelemetry.io/docs/security/handling-sensitive-data/). The [OpenInference conventions](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) define `CHAIN` and `LLM`; the [reference](reference-en.md#105-observability--opentelemetry--openinference) lists NeoGraph's attribute subset, token events, Python typed call examples and C++ lifetime requirements.

---

## 9. Common pitfalls

These have all been hit by real users; cross-referenced from
[`docs/troubleshooting.md`](troubleshooting.md).

### "My ReAct loop only runs once"

You're on wheel ≤ 0.1.7. The graph compiler dropped the
`conditional_edges` block silently. Upgrade to ≥ 0.1.8. Verify with
`result.execution_trace == ['llm', 'dispatch', 'llm']` (not just
`['llm']`).

### "Provider call hangs for 60 seconds and then errors"

You're on wheel ≤ 0.1.6. The bundled OpenSSL hardcodes RHEL CA paths
that don't exist on Ubuntu / Debian / macOS. Upgrade to ≥ 0.1.7
(auto-sets `SSL_CERT_FILE` to certifi's bundle on import) or set
`SSL_CERT_FILE` manually.

### "My fan-out is slower than I expected"

`compile()` defaults to `set_worker_count(1)` (no engine-owned thread
pool — fan-out branches run serially on the caller's executor). For
real parallelism call `engine.set_worker_count(N)` where N matches
your Send fan-out width, or `engine.set_worker_count_auto()` for
`hardware_concurrency()`. NeoGraph also prints a one-shot stderr
warning the first time a multi-Send fan-out runs without an opted-in
pool — that's a hint, not an error. Python custom nodes see GIL
contention on small fan-outs, so bench with both 1 and N.

### Reading Python RunResult status and state

`result.status` exposes the typed `Completed`, `Interrupted`, `StepLimit`, `SafePoint`, `BarrierStalled` or `FailureExit` status; `BarrierStalled` means that a barrier can no longer fire and the run stopped without reaching `__end__`, and `FailureExit` that a failure-exit node completed and the run ended as failed. `result.output` remains the portable final state; `result.interrupted`, `result.max_steps_exhausted` and `result.execution_trace` describe the run. `result.native_messages` and `result.provider_outcomes` retain full typed provider evidence. See the [Python binding guide](python-binding.md#hitl-and-state).

### "Unknown reducer: <name>"

Two reducers ship: `overwrite` and `append`. Register custom reducers before
compilation through `ReducerRegistry::register_reducer` in C++ or
`ng.ReducerRegistry.register_reducer` in Python.

### "The condition is registered but my conditional edge doesn't fire"

Verify the form is one the loader accepts (form A or form B from
[§4](#4-edges--conditional-routing)) — both work since v0.1.8. On
older wheels, only form B works.

### "execution_trace shows only the start node"

Routing fell through to `__end__`. Most likely a missing edge from
your start node, or your conditional returned a value not in the
`routes` map and an explicit `"default"` route points to `__end__`.
Strict graphs no longer choose a route by map ordering: an open or
unspecified condition uses `"default"` when declared, otherwise the
engine throws with the source node, condition, and returned label. A
closed condition always throws if it returns outside its declared labels.

---

## Where to next

- [Python examples](../bindings/python/examples/) — self-contained scripts that
  show the concepts above one at a time; most run offline.
- [C++ examples](../examples/) — programs with the same structure.
- [`reference-en.md`](reference-en.md) — exhaustive class-by-class API.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — deep dive on the async / coroutine
  layer.
