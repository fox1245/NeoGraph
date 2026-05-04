# v0.3 follow-ups

Remaining items from the FastAPI SSE chat-demo feedback (2026-05-04).
v0.3.0 ships the cancel propagation work; everything below is pending.

## 1. Auto checkpoint resume on same `thread_id` (LangGraph parity)

Currently `engine.run_async()` with the same `thread_id` does NOT
auto-resume from the last checkpoint — every run starts fresh from
`config.input` and the registered reducers. Multi-turn callers thread
prior state through the input dict themselves (see
`examples/16_deep_research_chat.py`). This is the biggest mental-model
mismatch for users coming from LangGraph.

Options:
- **Opt-in** — `RunConfig.resume_if_exists = True` flag. Loads latest
  checkpoint for `thread_id` if present, applies `input` on top.
  Backwards-compatible default.
- **Opt-out** — flip the default and add `RunConfig.fresh_start = True`
  for callers that want today's behaviour. More LangGraph-like but a
  silent semantics change.

Lean towards opt-in for v0.3.x; opt-out behind a major bump (v0.4).

## 2. Better error message for streaming-only nodes hitting `run_async`

When a node defines only `execute_full_stream` and is run via
`run_async` (non-stream), the engine raises:

```
NotImplementedError: GraphNode subclasses must override execute() ... OR execute_full() ...
```

without mentioning the streaming variants. Add hint:
`"(this node defines execute_full_stream — call run_stream_async() or run_stream() instead)"`.

Catch in `GraphNode::execute_full` (the default-throw path) by
checking `has_user_method("execute_full_stream")`.

## 3. Token-emit helper

Replace this 4-line ritual:

```python
ev = ng.GraphEvent()
ev.type = ng.GraphEvent.Type.LLM_TOKEN
ev.node_name = self._name
ev.data = token
cb(ev)
```

with:

```python
cb.emit_token(self._name, token)
```

Either bind a wrapper in pybind that decorates the user's `cb`, or
add `GraphStreamCallback.emit_token(node_name, data)` C++-side and
expose it as a method on the cb param. Cleaner path: a Python-side
helper module `neograph_engine.streaming` (already exists) gains
`emit_token(cb, node, data)` free function.

## 4. README "Differences from LangGraph" section

The "LangGraph for C++" pitch primes users to expect identical
semantics. Document the deltas up front:

- Same `thread_id` re-run does NOT auto-resume (until item 1 lands).
- `update_state(thread_id, channel_writes, as_node='')` — channel_writes
  is a list of `ChannelWrite`, not a `values=` dict.
- `Provider.complete_async` is not bound on Python user-defined
  Provider subclasses — only `complete` (sync). For async-native
  provider integrations stay in C++.
- `get_state(thread_id)` returns nested dict
  (`state["channels"]["messages"]["value"]`); a flat helper would
  improve ergonomics.

## 5. `update_state` signature drift

The docstring/example forms suggest `values={...}` keyword usage but
the actual signature is `(thread_id, channel_writes, as_node='')`
where `channel_writes` is a list of `ChannelWrite`. Either accept a
dict overload or rewrite the docs.

## 6. `get_state` dict shape — Pydantic / accessor helper

`state["channels"]["messages"]["value"]` is too deep for the common
read. Add either:
- Pydantic model — `StateView` with `.messages`, `.channels.foo`
- Plain helper — `state.channel("messages")` returning the value

## 7. Cancel propagation for parallel branches (v0.3.0 gap)

v0.3.0 wires cancel_token through `GraphState` → `PyGraphNode::execute_full_async`.
For `run_one_async` (single-node path) this works end-to-end.

For `run_parallel_async` fan-out branches that dispatch to
`fan_out_pool_` worker threads: each branch's `execute_full_async`
DOES still install the `CurrentCancelTokenScope` (because we read
from GraphState, which is shared across branches), so this should
work — but it hasn't been verified with a parallel-branch live LLM
test. Add one before declaring full coverage.

For `run_sends_async` (Send-driven dynamic fan-out): same — the
multi-send path uses isolated state copies; verify they carry
`run_cancel_token_` from the parent state.

## 8. Self-evolving JSON agent v2 (PoC improvements)

`examples/22_self_evolving_graph.py` proves the loop closes (graph
JSON self-edit + recompile works) but the LLM is bad at reasoning
about channel data flow. v2 should expose channel topology
explicitly to the modifier prompt (`"node X reads {a,b}, writes
{c}"`) and let the modifier propose per-stage channels. Optional —
research direction, not a user blocker.

## 9. pgvector RAG example

`bindings/python/examples/` has no RAG node — common use case.
Add an example using pgvector via the existing Postgres connection
infrastructure.
