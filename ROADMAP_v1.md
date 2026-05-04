# NeoGraph v1.0 — design sharpening roadmap

This file tracks **architectural** changes targeted at a future v1.0
major bump. These are NOT incremental patches; each is a public-API
break candidate that needs a deprecation window. Maintained as a
living document — add candidates here when a v0.3.x patch series
exposes a structural pain point, prune when one lands.

## Why this file exists

The v0.3.x cancel-propagation series (5 rounds: v0.3.0 single-node,
v0.3.1 multi-Send pointer, v0.3.1+ in-process polling, v0.3.2 hooks
for Python, v0.3.2 C++ scope+retry+exception-typing) was a single
logical fix that needed 5 patches because the **same cross-cutting
concern (cancel) had to be threaded through ~8 dispatch entry points
plus 2 entry languages (C++/Python)**. Each patch closed one entry
path while leaving others open.

The bug pattern was almost never "architecture is wrong" — it was
"the right pattern was applied in M of N places." The v0.3.x series
verified the *core* design (Pregel BSP super-step, channel reducers,
Send/Command dynamic dispatch, asio coroutine throughout) by
exception: bugs were caught without ever questioning the model.

What the series DID expose are three high-cognitive-load seams in
the current design that make N-place implementation distribution
error-prone. Each candidate below addresses one seam.

---

## Candidate 1 — Single dispatch entry with tag-based routing

### Symptom

`GraphNode` exposes 8 virtual methods:

```
execute            execute_async
execute_full       execute_full_async
execute_stream     execute_stream_async
execute_full_stream execute_full_stream_async
```

These form a `(sync/async) × (writes/full) × (stream/non-stream)`
cross-product. Defaults chain through each other; priority order
must stay consistent. Every default chain hop is a place a bug can
hide:

- v0.3.1 #2: hint message didn't mention streaming variants.
- v0.3.2 #10 (Python): `PyGraphNode::execute_full_stream` skipped
  the `execute_stream` branch — `run_stream` useless for
  streaming-only nodes.
- v0.3.2 #10 (C++): `GraphNode::execute_full_stream` default
  called `execute_full` first → `ExecuteDefaultGuard` recursion
  threw → `execute_stream` never reached.
- GCC-13 codegen workaround needed in `execute_full_stream_async`
  because `catch(T&)` around `co_await` silently misses.

### Sharpening

Single virtual dispatch:

```cpp
class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
};

struct NodeInput {
    const GraphState&    state;
    const RunContext&    ctx;          // see Candidate 2
    GraphStreamCallback  stream_cb;    // null if non-stream
    bool                 is_async;     // hint, not a hard contract
};

struct NodeOutput {
    std::vector<ChannelWrite> writes;
    std::optional<Command>    command;
    std::vector<Send>         sends;
};
```

User overrides ONE method. Sync/async distinction handled by the
engine (the engine wraps sync overrides in run_sync, async overrides
get awaited directly — but it's an engine concern, not a user
concern). Streaming distinction: `stream_cb` non-null = streaming
expected; user uses or ignores. Command/Send: just populate fields.

Migration: keep the 8 virtuals as deprecated thin shims for one
release. New code overrides `run()`. Trampoline (`PyGraphNode`)
becomes a one-liner.

### Cost

- Public API break — every existing GraphNode subclass needs a
  `run()` rewrite.
- `RunContext` (Candidate 2) is a hard prerequisite or `run()`
  can't carry per-run metadata.
- Engine internal dispatch logic gets simpler but the engine must
  pick sync-vs-async based on a runtime hint or convention.

---

## Candidate 2 — Explicit `RunContext` for per-run metadata

### Symptom

Today `RunConfig::cancel_token` is the only per-run "metadata" the
caller can set. The engine smuggles it in via two mechanisms:

1. `GraphState::run_cancel_token_` — a member that lives in
   GraphState but is **not in the channel set**, so `serialize()`
   loses it.

   - v0.3.1 multi-Send fix: `init_state(send_state) +
     send_state.restore(snapshot)` rebuilt the per-worker state
     but dropped `run_cancel_token_` because it's outside the
     channel set. Required an explicit
     `send_state.set_run_cancel_token(parent.run_cancel_token_shared())`
     on every Send fan-out worker.
   - Whoever adds the next per-run field (deadline? trace_id?
     metric handle?) will hit this exact bug again.

2. `current_cancel_token()` thread_local — set by
   `CurrentCancelTokenScope` at execute_full_async entry.

   - v0.3.2 C++ fix: PyGraphNode installed the scope; native C++
     `GraphNode::execute_full_async` default did NOT, so multi-Send
     C++ workers' `Provider::complete` saw a null thread-local and
     run_sync ran without cancel binding. 7s cost-leak.
   - Every new dispatch entry point needs to remember to install
     the scope. Forget = silent feature breakage.

Both mechanisms exist because there's no first-class place for
per-run metadata. They're workarounds.

### Sharpening

Explicit `RunContext` carried alongside `GraphState` through every
dispatch:

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::optional<Deadline>       deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    // ... extension point for future cross-cutting concerns
};

class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
    // in.ctx is the RunContext — no thread_local, no
    // serialize-loses-it. Every dispatch path threads it explicitly.
};
```

`Provider::complete(params, ctx)` takes the context too. No
thread_local. No `current_cancel_token()`. Send fan-out workers
copy `ctx` by value (cheap — shared_ptr + a few strings).

### Cost

- Public API break — every Provider, every GraphNode, every Tool.
- Wider signature throughout — `state, ctx` everywhere.
- BUT: closes the entire class of "I forgot to thread the
  cancel/trace/deadline" bugs. One signature, one place to add
  new fields, no workaround.

### What v0.3.x bugs this would have prevented

- v0.3.1 multi-Send pointer drop: ctx is just an explicit field,
  not buried in a non-serialized member.
- v0.3.2 C++ thread_local missing: no thread_local at all.
- Future deadline / trace_id / metric leaks: same shape, same
  preventive coverage.

---

## Candidate 3 — Hierarchical / per-consumer CancelToken

### Symptom

`CancelToken` was designed around one `cancellation_signal sig_` +
one `bind_executor` slot. asio's `cancellation_slot` is single-
handler — last `bind_cancellation_slot` wins. Concurrent
consumers (multi-Send fan-out workers each calling Provider::complete
→ inner run_sync → bind_cancellation_slot) silently overwrote each
other's binding; only the last bound HTTP got cancelled.

v0.3.2 grafted a `add_cancel_hook` list on top of this single-signal
design so each nested run_sync owns its own private signal that the
parent's `cancel()` fires by iterating hooks. Works, but reads as
"compensating for a single-consumer primitive used in N-consumer
contexts." Plus an emit-vs-bind race: if cancel was already set when
add_cancel_hook is called, the synchronous fire posts emit BEFORE
co_spawn binds the slot, and the emit is lost. v0.3.2 added an
eager `is_cancelled()` short-circuit at run_sync entry to dodge
this — another patch on a patch.

### Sharpening

Hierarchical cancellation:

```cpp
class CancelToken {
public:
    /// Create a child token. Parent.cancel() cascades to child.
    /// Each child has its OWN cancellation_signal — no
    /// single-consumer assumption.
    std::shared_ptr<CancelToken> fork();

    /// Cancel this token (and recursively all children).
    void cancel();

    bool is_cancelled() const noexcept;
    asio::cancellation_slot slot();  // each token has its own
    void bind_executor(asio::any_io_executor ex);
};
```

Each `run_sync(aw, parent_token)` does:
```cpp
auto child = parent_token->fork();
child->bind_executor(io.get_executor());
asio::co_spawn(io, body(),
    asio::bind_cancellation_slot(child->slot(), asio::detached));
```

No add_cancel_hook list to graft on. No emit-vs-bind race
(child created fresh, signal bound first, fork() snapshot of
parent state). Multi-Send fan-out: 3 sibling tokens, parent
cancels all three.

Borrow from: Go's `context.Context` cancellation, asio's
`asio::cancellation_state` / `make_cancellation_filter` (if
asio gains the right API). The pattern is well-known.

### Cost

- Public API change to CancelToken (additive — `fork()` is
  new). The old `add_cancel_hook` would deprecate.
- Internal: every `run_sync(aw, cancel)` becomes
  `run_sync(aw, cancel->fork())`.
- Net: one primitive replaces "single signal + hook list +
  eager-cancel short-circuit + per-consumer race notes."

---

## Cross-cutting observations

The three candidates compose: Candidate 2 carries Candidate 3's
token through the dispatch path; Candidate 1's single `run()`
naturally takes a `RunContext` containing the cancellation child.

If only ONE lands, prefer Candidate 2 — it kills the largest
class of recurring bugs (anything that needs to be threaded
through every dispatch).

Tracking: this file is updated when a v0.3.x patch round
exposes a new architectural seam, or when a candidate lands
(strike-through and link to the merge commit).

## Status

| # | Candidate | Status | Triggering rounds |
|---|---|---|---|
| 1 | Single `run()` dispatch + tags | Proposed | v0.3.1 #2, v0.3.2 #10 (×2 langs) |
| 2 | Explicit `RunContext` arg | Proposed | v0.3.1 multi-Send, v0.3.2 C++ scope |
| 3 | Hierarchical CancelToken | Proposed | v0.3.2 hooks, v0.3.2 emit-vs-bind |
| 4 | Self-evolving graph runtime hooks | Research | TODO_v0.3.md #8 |

---

# Research track (less load-bearing than the v1.0 sharpenings above)

## Candidate 4 — Self-evolving JSON agent v2 (research)

### Context

`bindings/python/examples/22_self_evolving_graph.py` proves the loop
closes: an LLM modifier proposes a JSON edit to the running graph,
the engine recompiles, the new graph runs. PoC works but the LLM
struggles to reason about channel data flow when proposing edits —
it doesn't "see" which nodes read/write which channels, so its
proposals frequently route data through the wrong wires.

### Research direction

Expose channel topology to the modifier prompt explicitly. Two
forms to investigate:

1. **Topology summary in prompt** — engine emits a per-node
   spec like ``"node X reads {a,b}, writes {c}"`` derived from
   compiled channel access patterns. Modifier prompt receives
   this alongside the JSON definition.

2. **Per-stage channel proposals** — modifier proposes channels
   *per stage* (split / synthesize / etc.) rather than as a flat
   set. Engine compose-checks that each proposed stage's channel
   set is consistent with the upstream/downstream stages.

### Why it's not a v1.0 must-have

- Not a user blocker for the shipped engine — the PoC's gap is
  in the *prompt engineering*, not the engine.
- Requires LLM eval harness (correctness rate per topology
  variant, cost, edit-cycles-to-converge) before any engine-
  side surface change is justified.
- May fold into a broader "graph introspection API" v1.x
  feature once the eval shows what introspection LLMs actually
  use.

### Cost

- Engine surface addition (topology accessor) is small if
  research validates it.
- Most of the work is LLM-side experimentation outside this
  repo's hot path.

### Triggering round

TODO_v0.3.md item #8 — deferred from v0.3.x as research, not a
user blocker.
