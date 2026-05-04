# v0.3 follow-ups

Originally from the FastAPI SSE chat-demo feedback (2026-05-04).
v0.3.0 ships cancel propagation; this file tracks the remaining
mental-model and ergonomics gaps.

## ✅ Closed in v0.3.1 (2026-05-04, session 2)

1. **Auto checkpoint resume on same `thread_id`** — opt-in
   `RunConfig.resume_if_exists` lands. When True and a checkpoint
   store is configured, `engine.run/run_async/run_stream` loads the
   latest checkpoint for `thread_id`, then applies `input` on top via
   the channel reducers (so APPEND-reduced `messages` grows with the
   new turn). Default `False` keeps existing fresh-start behaviour.
   Tests: `tests/test_resume_if_exists.cpp` (6) +
   `bindings/python/tests/test_resume_if_exists.py` (6).
2. **Better error message for streaming-only nodes** —
   `GraphNode.execute()` (Python base) now walks the subclass MRO for
   `execute_stream` / `execute_full_stream`, and if either is defined
   the `NotImplementedError` includes a hint pointing at
   `engine.run_stream() / run_stream_async()`. Tests:
   `bindings/python/tests/test_streaming_only_error_hint.py` (4).
3. **Token-emit helper** —
   `from neograph_engine.streaming import emit_token` collapses the
   4-line `GraphEvent` construction ritual to
   `emit_token(cb, self._name, token)`. Tests:
   `bindings/python/tests/test_emit_token_helper.py` (5).
4. **README "Differences from LangGraph" section** — added under
   the Python Binding section. Calls out: opt-in multi-turn resume,
   `update_state(channel_writes)` shape, `get_state` nested dict,
   Python `Provider.complete` only, streaming-only nodes need
   `run_stream*`, and the new `emit_token` helper. Plus added
   `resume_if_exists` to the `RunConfig` table.
7. **Cancel propagation for parallel / Send branches** — verified
   static parallel via shared parent state (already correct in
   v0.3.0). Found and fixed the multi-Send gap: dynamic fan-out
   workers built isolated `GraphState` from `serialize/restore`, but
   `run_cancel_token_` lives outside the channel set and was being
   dropped — so a cancelled run still leaked cost on Send-spawned
   branches. Added `GraphState::run_cancel_token_shared()` and
   `NodeExecutor::run_sends_async` now forwards it onto each
   isolated `send_state`. Tests:
   `tests/test_cancel_token_propagation.cpp` (3 — static parallel,
   multi-Send, mid-fan-out abort).

## Status: v0.3.x feedback closed

All engine-affecting items from the v0.3.x feedback batch (FastAPI
SSE chat-demo + ergonomics rounds) are landed. Remaining item #9
(pgvector RAG example) is purely a worked example — no engine
gap — and lives on a future cookbook track recorded as Candidate
5 in `ROADMAP_v1.md`. v0.3.x as a series is closed; further engine
work targets v0.4 / v1.0.

## ✅ Closed in v0.3.2

9. **pgvector RAG example → ROADMAP cookbook track** — confirmed
   no engine gap (existing `PostgresCheckpointStore` infrastructure
   is sufficient; RAG nodes are pure user code). Recorded as
   Candidate 5 in `ROADMAP_v1.md` under the same Research/Cookbook
   section as #8. Belongs to a future cookbook drumbeat rather
   than the engine v-bump series.

6. **Flat `StateView` helper for `get_state` dict shape** —
   `engine.get_state_view(thread_id)` returns a Pydantic v2
   ``StateView`` with channels as top-level attributes
   (``view.messages`` instead of
   ``state["channels"]["messages"]["value"]``). The base class
   allows arbitrary channel names via ``extra="allow"`` — works on
   any graph without a user-declared model. Subclass ``StateView``
   with declared fields for typed access; mismatches raise pydantic
   ``ValidationError`` instead of silent type coercion.
   ``view.raw`` preserves the unflattened dict for callers needing
   version / metadata.

   Pydantic v2 added as a hard dep (it's table-stakes in modern
   LLM Python — FastAPI, LangChain, datamodel libs all use it).

   Tests: ``bindings/python/tests/test_state_view.py`` (12).

8. **Self-evolving graph v2 → research track in `ROADMAP_v1.md`** —
   the topology-aware modifier prompt direction is recorded as
   research candidate #4 there. Engine-side changes are likely
   small once an LLM eval shows what introspection actually moves
   the needle; deferred from v0.3.x because it's not a user
   blocker for the shipped engine.

5. **`update_state` accepts both dict and `list[ChannelWrite]`** —
   the v0.3.1 README description ("channel_writes is a list of
   ChannelWrite") was actually wrong: the engine took a JSON
   object only, so passing a list **silently no-op'd** (the C++
   `is_object()` check rejected it). Pybind binding now dispatches
   on input shape:
   - `dict` `{channel: value}` → existing path (LangGraph's
     `values={...}` shape, kwarg name differs).
   - `list[ChannelWrite]` → reduce to dict (last-write-wins per
     channel); duck-typed `.channel`/`.value` objects also accepted.
   - Other types raise `TypeError` so the silent-no-op trap can't
     come back.

   README `Differences from LangGraph` section corrected.
   Tests: `bindings/python/tests/test_update_state_shapes.py` (11).

10. **`execute_stream`-only nodes dispatch through `run_stream`** —
    fixed both at the Python binding AND the C++ engine level.

    **Python**: `PyGraphNode::execute_full_stream` now consults
    `execute_stream` before falling back to `execute_full`, so a
    Python node that only overrides `execute_stream(state, cb)`
    works correctly under `engine.run_stream()` /
    `run_stream_async()`. The v0.3.1 hint message in
    `GraphNode.execute()` no longer misdirects.

    **C++** (sister fix): a C++ subclass with only
    `execute_stream` override hit the same problem — the default
    `GraphNode::execute_full_stream` called `execute_full` first,
    which chained through `execute` / `execute_async` defaults
    and tripped `ExecuteDefaultGuard`'s recursion check. The
    `runtime_error` it threw escaped before
    `result.writes = execute_stream(state, cb)` could run. Fixed
    by introducing `GraphNodeMissingOverride` (subclass of
    `runtime_error` for back-compat) — the default-recursion
    guard throws this dedicated type, and both
    `execute_full_stream{,_async}` defaults catch *only* this
    type and fall through to `execute_stream{,_async}`. Real
    user-thrown errors propagate untouched.

    Priority order (preserved, both languages): execute_full_stream
    → execute_stream → execute_full → execute.

    Tests:
    `bindings/python/tests/test_execute_stream_dispatch.py` (5),
    `tests/test_execute_stream_only_dispatch.cpp` (2).
