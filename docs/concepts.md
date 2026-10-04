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

For example, if `counter` starts at 0 and two ready nodes both return `counter + 1`, both read 0. An overwrite reducer produces 1, not 2. A custom sum reducer can instead combine two increment writes of 1 into 2. Multi-branch `Send` uses isolated state copies with each payload applied; the single-`Send` path applies its payload to shared state. Reducer order does not make model responses or external effects reproducible.

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

Delta-backed checkpoints are a design, not a channel setting. Such a format would replay ordered `{channel, version, write mode, value}` deltas from a full snapshot, with at most *K* deltas (optionally a byte threshold). It must preserve overwrite, retention, versions and reducer identity; publish snapshot/deltas and checkpoint pointer atomically before clearing pending writes; and reject missing links, version gaps, unknown reducers or failed replay. Adoption needs a new schema version and measured benefit. Migrate existing snapshots into a base without invented deltas, keep old-reader full snapshots during reversible rollout, and reject downgrade with delta-only records unless the original reducer registry materializes them. Current stores remain full-snapshot stores.

Measure the baseline with `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite`; add `postgres` and `--pg-url` only for an isolated local database. Rows report logical serialized bytes, save/load p50/p95 and reconstruction depth; legacy rows report blob count. For allocation requests, run `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`. Native JSON/SQL allocators are not all intercepted by C++ `operator new`. Compare identical payloads, histories and backend setups, using measured rather than estimated savings. Logical bytes differ from durable physical bytes. SQLite uses a unique temporary database removed on exit; `--sqlite-path` retains its new file and refuses an existing path.

One recorded Linux x86-64 Debug baseline used one thread, 256 history steps, 512-byte messages and one iteration. These are historical measurements, not performance targets:

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

A separate repeat before deletion measured SQLite database/WAL sizes of 14,811,136 / 4,210,672 bytes (19,021,808 total). A Linux `LD_PRELOAD` shim counting process-wide `malloc`, `calloc` and nonzero `realloc` requests, including construction and JSON parsing, measured 88,277 extra requests / 605,289,027 requested bytes for memory and 114,295 / 867,319,964 for SQLite against `--history-steps 0`. These are cumulative requests, not live memory or store-only allocations. Aligned/internal allocations were not intercepted. The shim is not a dependency; use supported profilers and multiple warm runs before drawing conclusions.

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

Each channel carries a monotonic `version` number. The engine uses
this for checkpoint diffing and for the `state.channel_version(name)`
inspection API. You usually don't read it directly.

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

The engine invokes the compiled target once per `Send`; it does not promise a fresh node object. Send targets therefore need safe member-state handling under concurrent invocations. Each payload is applied before its target reads channels. A single Send uses shared state; multiple Sends use isolated copies of the post-ready-batch state and fold returned writes in invocation order after all branches finish. Routing then combines ordinary-node and Send-target signals for the next ready batch.

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

When a multi-Send (or multi-outgoing-edge) fan-out runs without an
opted-in pool, NeoGraph emits a one-shot stderr warning so the
silent-serial case doesn't fly under the radar. Suppress with
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

### Dynamic interrupts via `NodeInterrupt`

Throw from inside a node body (Python: `raise ng.NodeInterrupt(reason)`,
C++: `throw NodeInterrupt(...)`). The engine catches, persists state,
returns a `RunResult` interrupted at the throwing node — same
resume API.

Useful when the decision to pause depends on intermediate node output
(e.g. "did the LLM produce something worth showing the human?").

### Time travel

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` copies a checkpoint into the caller-named destination thread and returns the new checkpoint ID. An omitted checkpoint ID selects the source's latest checkpoint. The copy retains its pending continuation; editing state does not schedule new work.

Resuming a completed continuation with `next_nodes == ["__end__"]` restores the stored result without executing nodes. To continue paused work on edited state, select an exact earlier checkpoint from `get_state_history()` that still has pending nodes, fork that ID, edit the fork and resume it. A historical empty `next_nodes` vector is different: latest resume without an exact ID retains the fresh-run behavior, while exact-ID resume stays pinned to that snapshot.

[Example 08](../examples/08_state_management.cpp) keeps its new-turn flow: fork a completed checkpoint, edit the user message, then call `run()` with `resume_if_exists=true`; resume only if that new run interrupts. It does not demonstrate resuming a paused fork.

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Portable formats remain [`provider-message-v2`](../schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json). Genuine C++ checkpoint sidecars retain native seals in memory. Durable native history requires host-owned `sp::NativeArchive`: closed v3 / `spna3`, with authenticated owner-private custody and an independent key. Archive v2 is rejected, not upgraded or interpreted. Authentication binds every semantic descriptor choice (origin/paths/headers, policy, request field mappings, usage path and stop mappings), owner and exact custody binding. It is neither encryption nor vendor-issuer authentication; never publish archive bodies, keys, native blobs or raw wire observations. An archive is evidence storage, not a money grant or a spending lease. Program/external banks remain independently journal-owned; snapshot copies cannot create credit.

Provider history has separate modes. Same-route native continuation retains authentic reasoning, signatures and ordered tool groups under their original binding. Gemini defaults to `NativeOnly`; explicit `PortableForeign` accepts caller-created assistant text and tool calls without native seals, wire output or signatures. Only the first foreign function call receives Google's documented bypass marker; text-only turns receive no signature. This projection grants no native authority and never repairs or demotes a failed native seal. It does not make arbitrary multi-vendor history natively portable.

Responses `previous_response_id` selects provider-held conversation state; the request carries new input only. When client-tool ownership needs local evidence, `previous_response_history` supplies authentic prior ownership evidence and is not sent as repeated input. A cursor is neither a full native replay seal nor archive authority; it remains subject to origin, route, model, configuration and completed-state checks.

The active SDK interface revision and shared-library generation are 4; consumers must rebuild against matching headers and libraries. The output generation cap is admitted and accounted for on each call, separately from native replay configuration. Raising it for a new semantic call does not renew the original bank, grant or deadline. Content, prefix, origin, route, policy, tools and reasoning controls remain bound, apart from explicitly documented per-turn choices. Portable JSON v2 and native archive v3 / `spna3` remain unchanged; the historical ABI3 measurements below are not interface4 results.

Python exposes the same owned request/outcome boundary as C++: `make_provider_request`, `Provider.prepare`, `dispatch` and `invoke`. Use `ProviderMessage` with typed parts for provider history; `ChatMessage` remains a graph convenience projection. A returned SDK failure is available through `ProviderOutcome.failure`, while host observer/settlement exceptions retain `outcome` and `cause`. See the [Python binding guide](python-binding.md) for constructors and GIL/callback behavior.

Usage counters such as `input_total`, `output_total` and `total` are `std::optional<sp::Count>`; each present count has a `uint64_t value` and `Evidence`. `Usage` also records stage, quality and conflicts. Missing is unknown, never an invented zero.

`UsageAccumulator::snapshot()` returns accumulated reports. `total_tokens_wide()` returns charged tokens plus unresolved reservations; it must not be displayed as reported usage. Settlement requires a final, consistent report with input and output counts and charges the largest supported total, without clamping oversized usage. A missing counter in any accumulated report remains unknown in the aggregate. A reservation, a local charge and a vendor invoice are different records.

**Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The owner-approved protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.

**Current declarations; integrated runtime evidence below:** `<neograph/graph/checkpoint.h>` declares `ManagedBudgetLeaseScope` with `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks` and `deadline_clock_identity`. `OwnedManagedBudgetLease` exposes read-only `scope()`, `actor_id()`, immutable `bank_generation()`, `revision()`, `head_checkpoint_id()` and `head_commitment()`; it has no public authority-import constructor. `ManagedBudgetEffectReceipt` exposes `active()`, `effect_id()`, `claim_amount()` and `request_digest()`; a default receipt grants nothing. `CheckpointStore` declares `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)` and `release_managed_budget_lease(lease)`, with `_async` counterparts. Sync `CheckpointStoreCore` and `AsyncCheckpointStore` expose their respective variants. `managed_budget_checkpoint_commitment(checkpoint)` covers the full durable checkpoint, not just bank JSON. These declarations do not establish backend CAS, currency safety, installed ABI compatibility or a successfully exercised runtime path.

**Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Current release-error contract; exercised suite/probes below.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

**Archive-owner/retention contract; exercised suite/probes below.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

**Native-custody pre-I/O gate; exercised suite/probes below.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.

`ProgramFailure` retains live `provider_outcome` and `provider_cause`. Its canonical factual SDK witness binds genuine archive custody to owner/run/version/bundle/operation/attempt; Runtime eagerly restores configured custody before exposing a recovered failure. Public data-only `ProgramResult::create()` cannot bypass this with a prefilled witness, and an unresolved parsed seal is not an executable result. After process restart the original exception pointer is unavailable (`provider_cause == nullptr`), not recreated from text. A failure that cannot be persisted cannot be serialized, published or replayed.

`RecordedBindingSet` is source-bound, move-only data, never a caller-supplied dispatcher. The trusted Catalog `recorded_capability_binder` independently materializes captured-only capabilities from real persisted source events. `ProgramRuntime::replay_recorded()` checks original selected-source permissions, then transfers the actual remaining bank through durable CAS; inherited spend is not a new model grant. The old `start_recorded` renewal API is removed. InMemory, File, SQLite and PostgreSQL Program stores preserve the exact immutable owned lease throughout execution; expiry does not renew it. Controlled JavaScript still validates the underlying capability manifest and consumes exact completed command outcomes without redispatching external effects.

**Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

The observations below were recorded before this documentation reconciliation. They are historical evidence, not new test runs or guarantees for every platform, transport or security property.

**Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. No further paid calls are authorized. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

**Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

**Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.

Host delivery limits, extent-bounded diagnostic/raw evidence, common provider errors and the minimal media evidence are documented in the [typed provider reference](reference-en.md#owned-outcome).

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

> Historical issue #16 observed `getaddrinfo` SEGV with nested per-request `io.run()` and the old child-thread provider streaming bridge on some glibc/OpenSSL combinations. That bridge is removed by the typed cutover; the historical structural test result did not exhaustively reproduce downstream HTTPS/sanitizer/load conditions and is not a current qualification. Current callers use explicit `ProviderMode` with `invoke_async` / `dispatch_async`, and should drive work from an existing long-lived executor rather than nesting per-request event loops. Do not synthesize a single fake token or resend a completion as a workaround.

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

`result.status` exposes the typed `Completed`, `Interrupted`, `StepLimit` or `SafePoint` status. `result.output` remains the portable final state; `result.interrupted`, `result.max_steps_exhausted` and `result.execution_trace` describe the run. `result.native_messages` and `result.provider_outcomes` retain full typed provider evidence. See the [Python binding guide](python-binding.md#hitl-and-state).

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

- [Python examples](../bindings/python/examples/) — 21 self-contained
  scripts covering every concept above.
- [C++ examples](../examples/) — 36 programs with the same structure.
- [`reference-en.md`](reference-en.md) — exhaustive class-by-class API.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — deep dive on the async / coroutine
  layer.
