# NeoGraph core concepts — a narrative guide

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

Read this once before diving into the examples. It builds up the
mental model in the order you'd construct one yourself: graph →
channels → nodes → edges → fan-out → routing override →
checkpoints → streaming.

The Python material below describes existing bindings; provider bindings/wrappers are explicitly deferred and have not been ported or exercised for the typed lossless C++ cutover. Installing a historical wheel does not expose the new C++ provider API.

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

> **If you've used LangGraph before:** the primitives are intentionally
> the same — channels with reducers, nodes that emit writes, conditional

If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
> edges, `Send`, `Command`, checkpoints. The README summarizes NeoGraph's
> [two runtime layers](../README.md#two-runtime-layers). The narrative below
> assumes nothing.

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
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

A super-step is the unit of parallelism, of checkpointing, and of
streaming events. Two nodes that can both run "now" are the same
super-step; they observe the same input state and their writes
combine via reducers when the step ends.

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

`reducer` (`overwrite`, `append`, or a registered custom reducer) combines
updates. `retention` (`unbounded` by default, `latest`, or `bounded` with a
positive `retention_limit`) trims **arrays after each write**, including
explicit `ChannelWrite.Mode.Overwrite` writes. It does not expire channels by
super-step; `latest` still keeps its last element until overwritten. The
independent `persistence` choice is `checkpoint` (default, full materialized
value and version) or `ephemeral` (omit value and version from durable
checkpoints). Bounded retention changes observable state, not just storage:
it is not a substitute for preserving a full conversation history.

The policy dimensions are distinct: **combination** is the reducer;
**runtime lifetime** is currently across super-steps (array retention limits
the retained elements, not the lifetime); **checkpoint representation** is
currently full materialized state or omission. A proposed per-step lifetime
would reset to the declared initial value *after* a step's writes have been
applied and its routing decision completed, before the next step reads state.
That option is not currently available: safe rollout needs a precise contract
for interrupts in the middle of a step, pending-write replay, and `Send`
workers. It must never be conflated with today's `persistence: "ephemeral"`,
which does **not** reset values at step boundaries.

The engine merges the writes of each node in their returned order. Parallel
static nodes merge in the scheduler's ready order, multi-`Send` results in
`Send` invocation order, regardless of finish order; pending writes are
replayed into those same slots. Overwrite is therefore **ordered
last-writer-wins**, not a commutative merge. Append preserves element order.
Custom reducers on concurrent branches must be deterministic, pure under
replay, and associative if regrouping writes must leave the result unchanged;
commutativity is additionally required only if callers want order
independence. Side effects belong in nodes, not reducers. A write's explicit
overwrite mode bypasses the reducer, then still applies retention.

An ephemeral channel remains live in memory across super-steps, but its
contents cannot be reconstructed from a checkpoint. Every engine checkpoint made
with ephemeral channels records their names and whether they have been
written, without recording their values. Resume (including
`resume_if_exists`, exact-ID resume, and state updates) rejects a checkpoint
when an ephemeral value was written, a guard is absent (older checkpoint),
or the declared ephemeral channel set changed. A checkpoint captured before
the first ephemeral write may resume safely; pending writes replay from that
checkpoint in deterministic order. `update_state` refuses to write an
ephemeral value into a checkpoint. Isolated in-process `Send` workers inherit
the live ephemeral state; this runtime snapshot is not persisted. Keep
correctness-critical state in `checkpoint` channels, or reconstruct it
explicitly from durable inputs in a fresh run; ephemeral is suitable only
for disposable scratch data.
Direct `GraphState::restore` also rejects an ephemeral channel: callers must
use `restore_checkpoint` with its matching guard or `restore_runtime` for a
same-process, non-durable snapshot that includes every ephemeral value and version.

This guard uses the existing checkpoint metadata field, not a new channel
blob layout or a bumped store schema: legacy full-value checkpoints keep
working for graphs without ephemeral channels. On upgrade, a historical
checkpoint for a graph declaring ephemeral channels has no guard and must
fail closed; restart from durable inputs rather than guessing whether
scratch data was needed. Before rolling back to a runtime that does not
enforce guards, stop resuming threads with ephemeral channels (including
forks), drain them or restart those threads from known durable inputs, and
only then downgrade. Older binaries cannot recognize this additive
metadata field and are not safe readers for such threads.

Checkpoint storage currently uses **full materialized values** for all
checkpointed channels. Memory, SQLite, and PostgreSQL stores deduplicate
unchanged `(thread, channel, version)` values across checkpoints, but a
growing append history changes version on every write and still incurs a
growing full snapshot. Pending writes log successful tasks in an incomplete
super-step, not a general append-only channel-delta format.

**Delta-backed policy (design, not an available channel setting):** a future
store may record ordered `{channel, version, write mode, value}` deltas
between full snapshots, with a configurable maximum of *K* deltas between
snapshots (and optionally a byte threshold). Load from the newest complete
snapshot and replay at most *K* subsequent writes in the scheduler's fold
order; preserve overwrite resets, retention, version counters, and custom
reducer identity. Atomic publication must commit snapshot/delta and
checkpoint pointer together before clearing pending writes; missing links,
unknown reducer identities, version gaps, or failed replay must error rather
than return partial state. Custom reducers must be stable and replay-pure.
This format is **not enabled** until its schema migration and measured cost
justify it: assign a new checkpoint schema version, migrate old full
snapshots into a base snapshot without synthesizing historical deltas, retain
old-reader-readable full snapshots during a reversible rollout, and refuse
downgrade if a delta-only record exists (or materialize it with the original
reducer registry before rolling back). Existing overwrite/append/custom
graphs and all stores continue using the current format by default.

To measure the current full-snapshot baseline, build and run
`bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload
512 --backends memory,sqlite` (add `postgres` and `--pg-url` for a local
isolated test database). The history rows report logical serialized checkpoint
bytes, p50/p95 save and load latency, and reconstruction depth; the legacy
rows report blob count. Repeat under an allocation profiler (for example
`heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps
256 --payload 512 --backends memory`) to collect allocation count and bytes;
the native JSON and SQL allocators are not all intercepted by C++ `operator
new`. Use identical payloads, history lengths, and backend setup for any
future delta-format comparison; report measured values, not estimated
savings. SQLite and PostgreSQL durable stores may have different physical
bytes from the logical serialized checkpoint total.
The SQLite benchmark defaults to a unique temporary database removed on exit;
`--sqlite-path` retains its new output file and refuses an existing path.

One measured baseline (Linux x86-64, Debug build, one thread, 256 history
steps, 512-byte messages, one iteration; not a performance target):

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

In a separate repeat before history-thread deletion, SQLite reported a
14,811,136-byte database file plus a 4,210,672-byte WAL
(19,021,808 physical file bytes at that sampling point).

On the same workload, a Linux `LD_PRELOAD` shim counting process-wide
`malloc`, `calloc`, and nonzero `realloc` requests (including benchmark
construction and JSON parsing) observed **88,277** additional allocation
requests / **605,289,027** requested bytes for memory and **114,295** /
**867,319,964** for SQLite versus otherwise identical runs with
`--history-steps 0`. These are cumulative requests, **not** live memory,
physical checkpoint bytes, or allocations attributable only to the store.
Aligned allocations and internal allocator activity are not intercepted.
The shim is a measurement aid, not a library dependency; repeat with a
supported allocation profiler and multiple warm runs before drawing
performance conclusions.

### Writing to channels

A node returns a list of `ChannelWrite`s:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
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

Python exposes `cancel_token`, `thread_id`, `step`, `stream_mode`, `store`,
and `resume_value` on `input.ctx`. C++ callers may set `deadline` and
`trace_id` on `RunMetadata`; the engine propagates them through nested subgraphs.
Those two fields are not exposed by the Python binding yet.

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
    def get_definition(self): return ng.ChatTool(name="calc", ...)
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

`Send` is for cases where the number of next-step nodes depends on
state. Classic use: split a list of search topics into N parallel
researcher invocations.

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

The engine's `run_sends_async` instantiates `researcher` once per
`Send`, each with its own `state.get("topic")`, and runs them in
parallel via `asio::experimental::make_parallel_group`.

### Mental model

A `Send(target, payload)` is "instantiate `target` with this state
patch and add it to the ready set". The payload is applied as a
state write before the target sees `state`.

After the parallel group finishes, the next super-step's routing comes
from each Send-spawned task's outgoing edges (or its `Command.goto`,
if it emitted one).

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

`build()` defaults to `EngineConfig::worker_count == 1` — no engine-owned thread
pool, fan-out branches dispatch inline on the coroutine's own
executor. That's a no-allocate fast path that's cheap for sequential
graphs and safe for nodes that hold non-thread-safe state.

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
                    updates=[ng.ChannelWrite("retries",  input.state.get("retries", 0) + 1)],
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

If multiple Commands fire in the same super-step (rare — only
possible when multiple parallel-group siblings emit them), the last
one wins. The order is determined by parallel-group completion, which
is non-deterministic — design around this by ensuring at most one
sibling emits a `Command`.

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

`engine.fork(thread_id, from_checkpoint_id)` returns a new thread that
starts from a past checkpoint. Useful for "what if I had answered
differently" branching.

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Portable formats remain [`provider-message-v2`](../schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json). Genuine C++ checkpoint sidecars retain native seals in memory. Durable native history requires host-owned `sp::NativeArchive`: closed v3 / `spna3`, with authenticated owner-private custody and an independent key. Archive v2 is rejected, not upgraded or interpreted. Authentication binds every semantic descriptor choice (origin/paths/headers, policy, request field mappings, usage path and stop mappings), owner and exact custody binding. It is neither encryption nor vendor-issuer authentication; never publish archive bodies, keys, native blobs or raw wire observations. An archive is evidence storage, not a money grant or a spending lease. Program/external banks remain independently journal-owned; snapshot copies cannot create credit.

**Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The owner-approved protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.

**Current declarations; integrated runtime evidence below:** `<neograph/graph/checkpoint.h>` declares `ManagedBudgetLeaseScope` with `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks` and `deadline_clock_identity`. `OwnedManagedBudgetLease` exposes read-only `scope()`, `actor_id()`, immutable `bank_generation()`, `revision()`, `head_checkpoint_id()` and `head_commitment()`; it has no public authority-import constructor. `ManagedBudgetEffectReceipt` exposes `active()`, `effect_id()`, `claim_amount()` and `request_digest()`; a default receipt grants nothing. `CheckpointStore` declares `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)` and `release_managed_budget_lease(lease)`, with `_async` counterparts. Sync `CheckpointStoreCore` and `AsyncCheckpointStore` expose their respective variants. `managed_budget_checkpoint_commitment(checkpoint)` covers the full durable checkpoint, not just bank JSON. These declarations do not establish backend CAS, currency safety, installed ABI compatibility or a successfully exercised runtime path.

**Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Current release-error contract; exercised suite/probes below.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

**Archive-owner/retention contract; exercised suite/probes below.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

**Native-custody pre-I/O gate; exercised suite/probes below.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.

`ProgramFailure` retains live `provider_outcome` and `provider_cause`. Its canonical factual SDK witness binds genuine archive custody to owner/run/version/bundle/operation/attempt; Runtime eagerly restores configured custody before exposing a recovered failure. Public data-only `ProgramResult::create()` cannot bypass this with a prefilled witness, and an unresolved parsed seal is not an executable result. After process restart the original exception pointer is unavailable (`provider_cause == nullptr`), not recreated from text. A failure that cannot be persisted cannot be serialized, published or replayed.

`RecordedBindingSet` is source-bound, move-only data, never a caller-supplied dispatcher. The trusted Catalog `recorded_capability_binder` independently materializes captured-only capabilities from real persisted source events. `ProgramRuntime::replay_recorded()` checks original selected-source permissions, then transfers the actual remaining bank through durable CAS; inherited spend is not a new model grant. The old `start_recorded` renewal API is removed. InMemory, File, SQLite and PostgreSQL Program stores preserve the exact immutable owned lease throughout execution; expiry does not renew it. Controlled JavaScript still validates the underlying capability manifest and consumes exact completed command outcomes without redispatching external effects.

**Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

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

> Historical Python provider/wrapper examples below are not ported to the typed C++ contract and are not current provider guidance. The C++ change does not implement or qualify Python bindings. C++ observers export only established public text/scalars and nullable counts, never raw native state.
Same callback shape as streaming, different consumer. Pass an OTel
tracer-emitting callback into `engine.run_stream(cfg, cb)` and every
`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` event becomes a
span.

Two layers ship in-tree:

  - `neograph_engine.tracing.otel_tracer` — vendor-neutral OTel
    spans. Spans flow to any OTel backend (Jaeger, Tempo, Honeycomb,
    Datadog).
  - `neograph_engine.openinference` — LLM-shape attribute layer
    that turns the same spans into a *LangSmith-style chat-bubble
    trace* in Phoenix / Arize / Langfuse:

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

Spin up Phoenix once: `docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`. Open http://localhost:6006 — the trace
renders as a chain (`graph.run` → `node.X` → `llm.complete`) with
prompt / response / token counts visible in the LLM detail pane.
Same code, swap the OTLP endpoint URL for Langfuse self-host and
the trace shows up there with the same shape.

This is the answer to *"NeoGraph doesn't have LangSmith"* — you
get the LangSmith UX (chat bubbles, DAG hierarchy, token cost) by
running Phoenix or Langfuse locally with one Docker command. No
SaaS contract, no per-trace pricing.

See `docs/reference-en.md` §10.5 for the attribute-key schema and
the trade-off note between `otel_tracer` and `openinference_tracer`.

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

### "Python RunResult has no .status / .final_state attribute"

The Python binding doesn't expose those attributes. Use `result.output`,
`result.interrupted`, `result.max_steps_exhausted`, and
`result.execution_trace`. C++ callers can use `RunResult::status()` for the
typed `Completed` / `Interrupted` / `StepLimit` view. See the
[Python binding guide](python-binding.md#hitl-and-state).

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
