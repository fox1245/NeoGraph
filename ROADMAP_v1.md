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

---

## Pattern retrospective — 9 downstream findings (issue #36)

ProjectDatePop's `cpp_backend` stress-test over the v0.5 → v0.8
window landed 9 NeoGraph findings. **At least 7 of those 9 trace
to the same structural pattern** that Candidates 1 + 6 close — not
incrementally, but by *eliminating the surface where the pattern
can recur*.

### The unifying pattern

> **"X is safe only when Y" — but the Y precondition is not stated
> in the docstring, not enforced at compile-time, and not even
> surfaced at runtime when violated. The default path silently does
> the wrong thing, often only on a specific corner of an inputs
> cross-product.**

| # | The hidden conditional invariant |
|---|---|
| #4 | `Provider::complete_stream_async` default bridge is safe **only when** the native sync `complete_stream` doesn't itself use `run_sync` — silently violated by `SchemaProvider` WS path |
| #5 | `Provider`'s 4-virtual cross-product is safe **only when** the override surface picked happens to avoid the bridge nesting — invariant invisible from `provider.h` |
| #6 | `schema_mutex_` × on_chunk locking is safe **only when** the user's callback doesn't re-enter SchemaProvider — undocumented pre-fix |
| #9 | C++ openinference parity required because the Python wrapper had a hidden assumption about callback-thread identity that didn't translate |
| #16 | NeoGraph's bundled cpp-httplib is correct **only when** every consumer TU defines `CPPHTTPLIB_OPENSSL_SUPPORT` — silent ODR violation otherwise |
| #34 | `extra_fields` apply **only when** `params.tools` is non-empty — silently dropped reasoning fields for tool-less calls |
| #35 | `temperature` is sent **only when** `params.temperature ≥ 0` — but the schema has no way to declare "this provider doesn't accept temperature at all", forcing every call site to negate the default |

Two further findings (#17 docs gaps, #33 per-call binding gap) are
gap reports rather than hidden-invariant traps; the same root
diagnosis (the abstraction declared a static surface but didn't
expose the dynamic equivalent) applies.

### Why Candidates 1 + 6 close the *class*, not just the instances

Each finding above closed via a **targeted patch** to the specific
override site that misbehaved (PR #10, PR #11, PR #12, PR #19,
PR #20, PR #37, PR #37). Each patch left the *surface that allowed
the pattern* unchanged: 8 GraphNode virtuals, 4 Provider virtuals,
schema build_body branch tree. The next downstream — or the next
vendor schema, or the next refactor that adjusts one default — will
discover a new corner of the same cross-product where some other
"X is safe only when Y" lurks.

Candidates 1 and 6 collapse those cross-products to **one virtual
each**. After they land:

- **Candidate 1** (GraphNode 8 → 1): there is no longer a "which of
  the 8 virtuals you override determines whether the bridge is
  safe" decision. The user overrides `run(NodeInput)`. Sync vs
  async, stream vs non-stream, writes vs full-result are all body
  shape choices — no hidden invariants tied to virtual identity.
- **Candidate 6** (Provider 4 → 1): new implementations use
  `CompletionProvider::do_invoke()` as their one override point. The
  existing `Provider` surface remains stable for compatibility, while
  the new path has one explicit request mode and one drain pattern.

The remaining 2 findings (#9 thread identity, #16 ODR macro) are
*not* fixed by Candidates 1 + 6 — they're separate issue classes
(observability layer parity, build-system convention). #9 is
already resolved (PR #12 + parity tests). #16 is now compile-time
guarded (v0.8.0 `api.h`).

### What stays load-bearing about this retrospective

The 9 findings would have surfaced **regardless of project age**.
None of them required a long-running production deployment or an
exotic vendor — they came from a single downstream consumer
(ProjectDatePop) writing realistic agent flows over ~3 weeks.
Without Candidates 1 + 6, the next downstream of comparable depth
will land another 5-10 findings of the same shape. With them, the
class is closed.

This is the structural argument for **prioritising Candidates 1 +
6 in the v1.0 cycle** over more cosmetic v0.x cleanups. Each new
"X is safe only when Y" finding paid for itself in patch effort,
but the cumulative effort across 7 findings already exceeds what
Candidates 1 + 6 are estimated to cost.

### Mitigation in the v0.x deprecation window

Until Candidates 1 + 6 land, pin the invariants where they exist
today:

- `[[deprecated]]` on the legacy 8 virtuals + `docs/migration-v0.4-to-v1.0.md`
  — landed v0.4 / v0.8.
- `@warning` blocks on every override point that has a "safe only
  when Y" precondition (e.g. `Tracer::start_span`,
  `OpenInferenceTracerSession::close`).
- compile-time `#error` guards on cross-TU invariants that the
  language can express (e.g. `CPPHTTPLIB_OPENSSL_SUPPORT` macro
  consistency — landed v0.8).
- friendly runtime errors that name the invariant when violated
  (e.g. `Unknown reducer: 'foo'. Available: ...` — landed v0.8).

These narrow the window where the pattern bites, but don't close
the class. Candidate 1 + 6 do.

---

## Status

| # | Candidate | Status | Triggering rounds / issues |
|---|---|---|---|
| 1 | Single `run()` dispatch + tags | **Landed in v0.9.0.** `run(NodeInput)` is pure virtual; the legacy 8 virtuals and fallback chain are gone. | v0.3.1 #2, v0.3.2 #10 (×2 langs); pattern reinforced by #36 (9 downstream findings) |
| 2 | Explicit `RunContext` arg | **Landed in v0.4–v0.8** (`RunContext::store` field added v0.8 #27) | v0.3.1 multi-Send, v0.3.2 C++ scope |
| 3 | Hierarchical CancelToken | **Landed in v0.4** (`CancelToken::fork()` + cascade) | v0.3.2 hooks, v0.3.2 emit-vs-bind |
| 4 | Self-evolving graph runtime hooks | Research | TODO_v0.3.md #8 |
| 5 | pgvector RAG example | Cookbook | TODO_v0.3.md #9 |
| 6 | Provider single dispatch | **Landed without removal.** `CompletionProvider::do_invoke()` is the recommended one-override path. Existing `Provider::complete*` methods remain supported; deprecation warnings were withdrawn and no removal is planned. | #4 (closed v0.7), #5 (compatibility policy), pattern reinforced by #36 |

---

# Execution plan

> **Status:** Candidate 1 is complete. The plan below records how the
> migration was staged from v0.4.0 through the destructive v0.9.0
> v1-preparation release; it is historical context, not remaining work.

## The user-facing motivation

Forget the bug-class framing for a moment. From a **new user opening
the README** today, the surface looks fragmented:

  - "How do I write a node?" — 8 virtuals (`execute` / `execute_async`
    / `execute_full` / `execute_full_async` / `execute_stream` /
    `execute_stream_async` / `execute_full_stream` /
    `execute_full_stream_async`). Which one to pick? Answer is "it
    depends on Send/Command, sync/async, streaming/non-streaming"
    — three orthogonal axes the user has to reason about up front.
  - "How do I cancel?" — `RunConfig::cancel_token` exists, but for
    cancel to reach the LLM you also need: (a) the engine to install
    a thread_local scope, (b) Provider::complete to read it,
    (c) run_sync to register a hook, (d) the worker not to retry.
    None of that is in one place to read.
  - "How do I update state?" — at v0.3.2 it's `dict | list[ChannelWrite]`.
    Before that the README documented one shape and the binding
    silently no-op'd on the other. New users hit "why didn't my
    write apply?" and have to debug.
  - "How do I read state?" — nested `state["channels"][name]["value"]`
    OR flat `engine.get_state_view(thread_id).<channel>` OR a typed
    Pydantic subclass. Three valid answers; no single canonical one.
  - "How do I run a graph?" — `run` (sync) vs `run_async` vs
    `run_stream` vs `run_stream_async` vs `resume` vs `resume_async`.
    Six entry points, multi-axis matrix again.

**Each individual addition was justified** (resume_if_exists is a
real chat semantics, StateView is a real ergonomics win, etc.). But
**the cumulative effect is a surface where doing one thing has 2-4
ways scattered across docs, examples, and binding code.** v0.3.x
patches kept piling on; the v0.3.x cancel rounds (5 of them) made
visible that this fragmentation is also where bugs hide — when the
"right way" is in M places of N, the omission in place N+1 is the
silent-no-op / forgotten-pattern bug.

The architectural sharpenings (Candidates 1-3) collapse this to:

  - **One way to write a node** (`run(NodeInput) -> NodeOutput` + tags).
  - **One way to thread per-run metadata** (`RunContext` arg).
  - **One way to cancel** (`token->fork()` for nested ops, parent
    cancels all).
  - **One way to read state** (StateView is canonical; raw dict
    is the escape hatch).
  - **One way to run** (collapse run / run_async etc. into one
    method that takes a stream callback or returns an iterator).

This is the v1.0 contract — the docs page reads short again.

## Versioning strategy

| Version | Scope | Public API |
|---|---|---|
| **v0.4.x** | RunContext lands as a *new* parameter, old methods deprecated but still work. CancelToken gains `fork()` additive. New `run(NodeInput)` lands additive. | Both APIs callable. Deprecation warnings. |
| **v0.5.x** | Examples and pybind binding migrate to new API. Old API stays deprecated. | Both APIs callable. Heavier deprecation warnings + docs steer to new. |
| **v1.0.0** | Remove old API (8 virtuals, thread_local scope, single-handler CancelToken signal-on-self). | Single canonical API. |

Rationale: **no v0.4 → v1.0 leap.** A two-release deprecation
window lets downstream consumers (neoclaw, NeoProtocol Executor,
the WASM spike, anything outside this repo) migrate one component
at a time. cibuildwheel matrix stays intact across the window —
20 wheels per release path unchanged, just the dependency-on-old-
methods slowly reduces.

If the migration takes longer than expected (e.g. third-party C++
GraphNode subclasses are common), v0.5 becomes v0.5.x with
extended deprecation, v0.6 stretches the window. Drop the old API
only when the deprecation warnings have been quiet for a release.

## PR sequencing

Each row is one mergeable PR. They land in order, all on master
(no long-lived feature branch — the project's commit history is
straight-line and the deprecation strategy means each PR is
independently shippable to PyPI as v0.4.0+i, v0.4.0+(i+1), etc.).

| # | PR | Scope | Lands in |
|---|---|---|---|
| 1 ✓ | **`RunContext` plumbing (internal)** — landed `a473f0e` | Add `struct RunContext` to `engine.h`. Engine's `execute_graph_async` constructs and threads it through. NodeExecutor passes it to `execute_full_async`. Pybind wraps it. **No public-facing change** — old methods still take only `state`; the new `ctx` lives alongside in the dispatch path. ctest 442/442 + pytest 96/96 green. Bench median 5.365 µs (BASE 5.285 µs, +1.5%) — within WSL2 ~3% noise floor, inside the ±5% band off the 5.185 µs baseline. Pybind wrap deferred to PR 7 (binding migration) since PR 1 has zero pybind diff. | v0.4.0 |
| 2 ✓ | **`GraphNode::run(NodeInput) -> NodeOutput`** — landed `607ce66` | New virtual on GraphNode. Default implementation delegates to the old 8 virtuals (priority order preserved). Registers as the engine's preferred dispatch entry. Existing C++ subclasses still compile + work via the default fallback. ctest 442 → 445 (3 new NodeRunDispatch tests) + pytest 96/96 + 5 live LLM/WS green. Bench median 6.122 µs vs PR1 BASE 6.160 µs (Δ -0.6%) on A/B 10 rounds (host noisy today, PR1 BASE drifted from yesterday's 5.285 → 6.160 — same code, WSL2 jitter; A/B comparison cancels host drift). **Trap caught**: ``run(const NodeInput&)`` SEGV'd inside asio's executor under the pybind async path (coroutine-reference-parameter UAF, the v0.2.0 RunConfig crash shape). Fix: take ``NodeInput`` by value. Documented in node.h. | v0.4.0 |
| 3 ✓ | **CancelToken `fork()` additive** — landed `897645c` | Add `std::shared_ptr<CancelToken> CancelToken::fork()`. Parent `cancel()` cascades to children. `add_cancel_hook` keeps working (deprecated; `[[deprecated]]` annotation lands in PR 4). `run_sync(aw, cancel)` switches to `cancel->fork()`. The single-signal `slot()` API stays for the engine's outer co_spawn. ctest 445 → 452 (7 new CancelTokenFork tests) + pytest 96/96 + 5 live LLM/WS green. Bench A/B 20 rounds (interleaved both directions): Δ min +1.0%, Δ median +1.5% — within ±5% band; bench path has no `cancel_token` so doesn't hit `fork()`, the small delta is binary layout noise (PR3 bench binary is 3.7KB smaller than PR2, layout differs). | v0.4.0 |
| 4 ✓ | **Deprecation annotations** — landed `35a4517` | Add `[[deprecated]]` on the 8 old virtuals + `add_cancel_hook` (Hook returned by it deprecates indirectly). Trampoline scopes (`CurrentCancelTokenScope` / `current_cancel_token()`) deferred — that's the smuggling channel that PR 7 (binding migration) replaces with `ctx.cancel_token` reads, so deprecating it now would force suppress at every smuggling site without a clear migration path. Internal call sites (graph_node.cpp default chain, default `run()` forwarder) bracketed by new `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` macros (api.h — GCC/clang/MSVC portable). User code overriding deprecated virtuals or calling `add_cancel_hook` sees migration warnings; engine internals stay clean. ctest 452/452 + pytest 96/96 + 5 live LLM/WS green. Bench A/B 10 rounds: Δ median +0.3%, min +0.8% — pure attribute change, layout noise. `-Werror=deprecated-declarations` not enabled (CI never had `-Werror` to begin with; warnings stay informational through deprecation window). | v0.4.0 |
| 5 ✓ | **StateView canonical, raw dict deprecated** — landed `f31aa53` | Mark `engine.get_state(thread_id) -> dict` as soft-deprecated in the pybind docstring. New canonical = `get_state_view(thread_id) -> StateView` (already in v0.3.2). No `DeprecationWarning` emit, no `[[deprecated]]` annotation — raw dict has legitimate uses (per-channel `version` access, snapshot serialization). v1.0 keeps it as escape hatch unless the soft-deprecation generates loud feedback. Zero behavioural change. ctest 452/452 + pytest 96/96 green. | v0.4.0 |
| 6 ✓ | **Examples migrate** — landed `a2a24ef` (PR 6a, C++) + `0a76e3a` (PR 6b, Python) | 7 C++ + 19 Python examples (44 GraphNode subclasses total) switched to the unified ``run(NodeInput)`` API. PR 6a hand-migrated; PR 6b used an AST-scoped helper to safely batch-rewrite. Smoke runs match v0.3.2 outputs bit-for-bit. ctest 452/452 + pytest 96/96 green. | v0.4.x (split into 6a + 6b) |
| 7 ✓ | **Pybind binding migrates** — landed `4e186a5` | ``PyGraphNodeOwner`` now overrides ``GraphNode::run(NodeInput)`` and dispatches to Python user's ``run`` method via ``has_user_method`` MRO walk; falls through to the legacy chain when not present. Bound ``RunContext`` / ``NodeInput`` / ``CancelToken`` to Python (re-exported from the package). Smuggling ``CurrentCancelTokenScope`` STAYS — the legacy chain still installs it for un-migrated user code. PR 9 deletes it along with the legacy 8 virtuals. ctest 452/452 + pytest 96/96 + 5 live LLM/WS green; new ``run(input)`` API exercised end-to-end. | v0.4.x |
| 8 ✓ | **Docs rewrite** — landed `519a00b` | `docs/reference-en.md` §6 GraphNode collapsed to a single `run()` virtual; new RunContext + CancelToken (with `fork()` example) subsections under §7. README "Differences from LangGraph" picked up a "One node method" entry pointing at `run(input)`. The `@ng.node` decorator's internal `_DecoratorNode` now uses `run()` so the Five-second demo runs through the new path. concepts.md / troubleshooting.md sweeps deferred to PR 9 (where they become obviously stale once the legacy chain is deleted). | v0.5.0 |
| 9 ✓ | **Old API removal** — built-in migration `d1070dc`; legacy GraphNode chain `19819d8`; cancel hook `1d786a5`; thread-local/Python legacy bridge `9e8e956`; obsolete Python tests `4392fbb`. | v0.9.0 |

## Completed post-v0.4.0 plan (historical)

v0.4.0 shipped 2026-05-05 (`4cae42c`, tag `v0.4.0`). The observation
window and destructive removal described below have both completed;
v0.9.0 shipped the removal on 2026-05-14.

### Phase A — Deprecation window (completed)

Duration: weeks ~ one minor cycle. No engine code changes; this phase
exists so deprecation warnings have time to surface real downstream
breakage before v1.0 deletes the underlying code.

Watch for:

  1. **Deprecation visibility** — are users actually seeing the
     `[[deprecated]]` warnings on the 8 legacy virtuals + `add_cancel_hook`?
     PR 4 (`35a4517`) put internal call sites under
     `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` so warnings should ONLY come
     from user override sites. Issue tracker / discussion / direct
     feedback channels for "what's this warning?" mentions.
  2. **Legacy chain regressions** — any newly discovered case where the
     legacy 8-virtual default chain breaks (silent no-op, forgotten
     scope, etc.). v0.3.x had 5 rounds of these; one more is plausible.
  3. **Downstream consumer breakage** — third-party C++ subclasses of
     `GraphNode`. Known consumers in this repo's orbit:
     - `neoclaw` — `src/neoclaw_nodes.cpp:94` still has
       `std::vector<ChannelWrite> execute(const GraphState&) override`.
       Must self-migrate to `run(NodeInput)` before v1.0 ships or
       neoclaw breaks on the v1.0 wheel.
     - `NeoProtocol` Executor runtime — uses NeoGraph WASM build;
       v0.4.0 binding test recommended.
     - WASM spike — engine-zero-diff path was the v0.3.x baseline; v0.4
       run() addition is additive so likely fine, but verify.
  4. **Newcomer-mode trap surface** — the `ee11ed6` newcomer sweep
     closed 5 traps from the chatbot demo session. Streaming / MCP /
     async fan-out / HITL resume are paths that demo didn't touch;
     similar trap density possible. Surface via fresh `cibuildwheel`
     + first-time-user simulation, or via a separate session priming.
  5. **Optional patch releases** — if Phase A surfaces a real bug,
     ship v0.4.x patch. If a new feature is genuinely needed before
     v1.0, ship v0.5.0 minor (still in deprecation window).

Exit criterion: Phase A ends when deprecation warnings have been
"quiet for a release" — concretely, one full minor cycle (e.g. v0.5.0
shipped) with zero user-visible breakage tied to legacy paths.

### Phase B — destructive removal (completed in v0.9.0)

The sub-PRs landed independently in the order below so each step could
be reviewed and reverted on its own.

| Sub-PR | Scope | Risk | Files touched |
|---|---|---|---|
| **9b** | Delete `graph_node.cpp` legacy default chain (the 8-virtual cross-routing logic with `ExecuteDefaultGuard` recursion-detection); delete the 8 virtual declarations from `node.h`; migrate internal nodes in `src/core/deep_research_graph.cpp` (5+ subclasses) and `src/core/plan_execute_graph.cpp` (3+ subclasses) from `execute()` / `execute_full()` overrides to `run(NodeInput)`. | **High** — every internal GraphNode subclass must migrate in one PR. Built-in nodes already migrated in PR 9a (`d1070dc`); these two graph factories were the holdouts because their nodes are file-local, not in `nodes/`. | `node.h`, `graph_node.cpp`, `deep_research_graph.cpp`, `plan_execute_graph.cpp` |
| **9c** | Delete `add_cancel_hook` + `Hook` RAII class + `hooks_` member + `hooks_mu_` + `cancel()`'s hook iteration loop. `cancel.h` shrinks to just `fork()` + `cancel()` + `is_cancelled()` + `slot()` + `bind_executor()`. | **Medium** — `fork()` is the canonical replacement, exercised by 7 CancelTokenFork ctest. Failure mode is link-error in any user code still calling `add_cancel_hook` (caught at compile, not silent). | `cancel.h` only (impl is header-only) |
| **9d** | Delete `CurrentCancelTokenScope` (header + impl) + `current_cancel_token()` thread_local accessor + the `state.run_cancel_token_` member + `set_run_cancel_token` / `run_cancel_token` / `run_cancel_token_shared` accessors. `cancel.cpp` becomes empty (file removable). `RunContext::cancel_token` is now the only path. | **Medium** — every internal smuggling site must already read `ctx.cancel_token` (PR 7 binding done; provider-side reads need audit). Failure mode: provider that still reads `current_cancel_token()` returns null → cancel doesn't propagate to LLM HTTP. | `cancel.h`, `cancel.cpp` (delete), `state.h`, `graph_state.cpp`, plus audit sweep over `provider/*` |
| **9e** | Delete `PyGraphNodeOwner`'s 6 legacy GraphNode overrides (`execute(GraphState&)`, `execute_full`, `execute_full_async`, `execute_stream`, `execute_full_stream`, `execute_full_stream_async`) — keep only `run(NodeInput)` + `get_name()` + dtor. Delete `tests/test_node_default_dispatch.cpp` + `tests/test_node_async_default.cpp` + their CMakeLists entries. | **Low** — pure subtraction, no logic to break. Failure mode: any user Python class still defining only `execute()` (no `run()`) hits NotImplementedError on dispatch. Phase A should have surfaced these. | `bindings/python/src/bind_node.cpp`, `tests/CMakeLists.txt`, two test files |

After 9b–e land:

  - **SOVERSION introduce** (not "bump" — currently no
    `set_target_properties(... VERSION ... SOVERSION ...)` exists
    on any neograph_* lib). v1.0.0 is the natural moment to introduce
    SOVERSION 1 across `libneograph_core` / `_llm` / `_postgres` /
    `_sqlite` / `_mcp` / `_a2a` / `_acp`. Verify cibuildwheel matrix
    (manylinux soname suffix, macOS install_name, Windows DLL — each
    handles SOVERSION differently; treat as a bench-style verify
    rather than assuming "CMake property = it works").
  - **Docs sweep** — `docs/concepts.md` "8 dispatch entry points"
    paragraph collapses to one; `docs/troubleshooting.md` deletes
    legacy-chain entries; README "Differences from LangGraph"
    becomes "How NeoGraph thinks" (most LG-delta entries no longer
    apply because the gap closed).
  - **v1.0.0 tag → PyPI publish** — last step. Rollback cost is high
    (yanking PyPI release + reverting tag), so verify the full ctest
    + pytest + 5 live LLM + cibuildwheel 20-wheel matrix green
    BEFORE pushing the tag.

### Post-v0.4.0 minor corrections to this roadmap

Two small inaccuracies in earlier drafts that the audit caught:

  - **PyGraphNodeOwner legacy override count is 6, not 7.** Earlier
    notes said "7 overrides remove, run() only remains." Actual
    GraphNode-derived overrides in `bind_node.cpp:183`'s
    `PyGraphNodeOwner` are 6 (the 8 GraphNode virtuals minus
    `execute_async` and `execute_stream_async` which were never
    overridden — default chain handles them). After 9e: `run()` +
    `get_name()` + dtor remain, not just `run()`.
  - **SOVERSION is "introduce" not "bump."** The `CMakeLists.txt:5`
    comment mentions SOVERSION but no actual
    `set_target_properties(... SOVERSION ...)` call exists. v1.0
    is the first version to set it. Implication: cibuildwheel
    matrix runs need to verify wheel layout doesn't regress when
    SOVERSION suffix appears on Linux .so / macOS dylib install_name.

### Historical counterfactual: "What if we never remove?"

If Phase B never lands (legacy stays in v1.0+), the system **does
not break** — every current scenario keeps working, all 452 ctest
pass, deprecation warnings fire on user override sites only. The
cost is structural rather than acute:

  - **Bug-class breeding ground stays open.** v0.3.x's 5-round cancel
    propagation patch series happened because the same pattern had
    to be threaded through 8 dispatch entry points × 2 languages.
    Leaving the legacy chain keeps M-of-N omission bugs available
    for the next cross-cutting concern (deadline / trace_id / metric
    handle / observability tracing).
  - **`state.run_cancel_token_` non-channel-set member** drops on
    every multi-Send fan-out unless explicitly forwarded. Any new
    per-run field added here repeats the v0.3.1 pointer-drop bug.
  - **Two API surfaces in docs** — newcomer can't tell `execute` vs
    `run` apart without reading source; `ee11ed6` newcomer sweep's 5
    traps were exactly this docs-gap shape.
  - **SOVERSION never introduced cleanly.** Distro packagers
    (Debian, Homebrew, conda-forge) treat libraries without
    SOVERSION as upstream-mismanaged.
  - **Warning fatigue.** Permanent deprecation warnings train users
    to ignore them, so the next real deprecation gets buried.

None of these break v0.4.0 today. They make every future evolution
slower and bug-prone. The v1.0 promise of "single canonical way" is
the answer to all five at once.

## Per-PR contract

Each PR must:

  - **Not break ctest 442/442 + pytest 96/96** at the time it merges
    (deprecation warnings allowed in the build, errors not).
  - **Not regress the bench** (median µs/iter on `bench_neograph` seq
    path, measured per `feedback_wsl2_bench_isolation.md` — fresh
    worktree, taskset+chrt).
  - **Touch at most one of**: header surface OR engine internals OR
    binding OR examples. Mixed PRs make review hard and revert
    expensive.
  - **Add a row to this table when it merges** — strike-through the
    proposed line, link the merge commit, note any scope drift.

## Perf retrospective — `b59444f` 18-day latent par regression

Near the end of the v1.0 cycle, the README's "engine overhead" boast
(par 11.8 µs) was revealed to be broken. Measurement + parallel-bisect
result: a single commit `b59444f` was the regression that had been
latent for 18 days (2026-04-26 → 2026-05-13).

### What happened

- `b59444f` changed `GraphEngine::compile()`'s default worker count
  from `1` to `std::thread::hardware_concurrency()`. Intent: fan-out
  nodes receive real parallel execution without explicit configuration.
- Side effect: the 1-node sequential + 5-node fan-out micro-bench
  picked up an additional **per-iter cross-thread submit cost of
  ~75 µs/iter**. 11.8 µs → 283 µs (24×).
- The April 27 perf audit (`project_perf_audit_2026-04-27.md`) records
  `fd60aab` as the "fix", but that was a separate regression (timing-
  measurement pattern) and left the worker-count default unchanged.
  The par micro-bench itself was being measured in the
  "default=hardware_concurrency" mode, so it looked numerically
  normal, but the README's actual 11.8 µs claim was a pre-`b59444f`
  value.
- Although the v1.0 cycle's Per-PR contract requires "Not regress the
  bench", the bench at the time was being measured against the same
  (regressed) baseline, so it fell inside the ±5% band and passed
  silently. Latent for 18 days.
- On 2026-05-13, a per-commit parallel bisect (`git worktree add` for
  11 worktrees in parallel, taskset+chrt measurement) confirmed
  `b59444f` as the single commit responsible for the par 11.8 µs →
  283 µs jump. Revert (`e5ecb08`) restored 11.8 → 12.2 µs.

### Trade-off — why default=1 is correct

A `asio::thread_pool` cross-thread submit costs roughly 75 µs per
task. If a single node's execution time is in ms (LLM call, HTTP,
etc.), that cost disappears into the noise — but on NeoGraph's
celebrated "engine overhead serial/parallel µs-scale" path, it is
the same order of magnitude and shows up directly.

- **CPU-tiny / sequential nodes (micro-bench, validator chain, etc.)** —
  default=1 is overwhelmingly better. Sequential on a single
  io_context thread with no worker pool.
- **Genuine fan-out intent (sleep-bound sims, separate-process calls,
  sync HTTP)** — the user must explicitly call
  `engine->set_worker_count_auto()` or `set_worker_count(N)`. One line.

To make this trade-off explicit, `e5ecb08`'s commit message + the
following fan-out example 5 sites (10/14/21/36 + the
`deep_research_graph` builder) added explicit
`set_worker_count_auto()` calls, and the migration doc's Migration 3
section was beefed up.

### Per-PR contract reinforcement (preventing the next regression)

Checking only that `bench_neograph par` micro-bench is within ±5% of
baseline was insufficient — when the baseline itself has regressed,
it slides down together. In a follow-up patch:

  - The bench-regression CI uses a **README-stated absolute value**
    (`seq ≈ 5.0 µs`, `par ≈ 12 µs`, etc.) as a second wall-time-anchor
    gate. Catches baseline-itself regressions.
  - Or add a GitHub Actions cron for master → master 7-day regression
    measurement (a nightly-soak-style pattern).
  - If a per-PR diff touches `GraphEngine::compile()` or
    `set_worker_count`, the PR body must include "separate micro-bench
    measurement results" (automated via a CODEOWNERS hook).

All three are follow-up work. At least one must land before the v1.0
release.

### What we learned

1. **"Default-value change" can be a perf-critical contract even when
   it has no functional meaning.** If the README's boast numbers come
   from the "default" path, then default change = README change.
2. **The baseline of regression measurement can itself regress.**
   Don't only do ±band comparison; also set absolute-value anchors.
3. **Parallel bisect (11 parallel worktrees + result aggregation)
   pinpointed an 18-day-latent regression in 30 minutes.** Much
   faster than linear bisect — the default tool when master has
   grown long.

## Memory of v0.3.x traps to avoid during refactor

The build/release pipeline has accumulated landmines from v0.1.x →
v0.3.x. Each of these has a memory entry — this table is the
checklist when you touch the relevant area:

| Trap | Where it bites | Memory entry |
|---|---|---|
| `NEOGRAPH_API` macro on every public class + free function | New engine sub-libraries (postgres / sqlite / mcp / a2a / acp). Windows DLL boundary. | `feedback_neograph_api_discipline.md` |
| Cross-branch stale .so contamination | `BUILD_SHARED_LIBS=ON` build/ used across branches → ABI mismatch SEGV in compile() | `feedback_cross_branch_stale_so_trap.md` |
| Build dir contamination on bench measurements | Long-lived build/ dirs produce slower binaries than fresh worktree builds (+0.4 µs/iter false signal) | `feedback_bench_build_dir_contamination.md` |
| WSL2 measurement jitter | Plain "many reps + median" doesn't converge — needs taskset + chrt FIFO 99 | `feedback_wsl2_bench_isolation.md` |
| Doxygen `/*` wildcard in comments | `fs/*` / `terminal/*` inside `/**` opens nested comment, suppresses subsequent diagnostics. Use `&#42;` HTML entity. | `feedback_doxygen_slash_star_trap.md` |
| ASan `__cxa_throw` interceptor CHECK | C++ exceptions crossing pybind boundary trip the interceptor under `LD_PRELOAD libasan.so`. Deselect by keyword in CI; cancel/throw correctness is exercised by TSan + live LLM tests. | (this session — add note in feedback) |
| TSan eptr lifetime race | NodeInterrupt's exception_ptr crossing co_await boundary trips libstdc++ `__exception_ptr::_M_release`. Fix: extract reason as `std::string`, throw fresh on main thread. | `feedback_parallel_group_eptr_race.md` |
| MSVC needs explicit `<array>` / `<algorithm>` | libstdc++ pulls them transitively; MSVC v143 doesn't. Test files using `std::array` etc. break Windows CI silently. | (this session — add note in feedback) |
| scikit-build-core 0.12.2 Windows single_config | `-G` flag detected, env-var ignored — Windows wheel loses SQLite=OFF override. Use `[[tool.scikit-build.overrides]]` + `cmake.define`. | `feedback_libcurl_unconditional_dep.md` |
| Wheel OpenSSL CA path | manylinux libssl uses AlmaLinux paths absent on Ubuntu. `__init__.py` auto-set `SSL_CERT_FILE` from certifi. | `feedback_wheel_openssl_ca.md` |
| pyproject.toml runtime deps not auto-installed in CI's PYTHONPATH flow | `pip install --quiet pytest` line must mirror pyproject.toml's `dependencies = [...]`. v0.3.2 lost this for pydantic. | (this session — add note in feedback) |
| `compile()` default worker count regression | `b59444f` changed the default from `1 → hardware_concurrency`, latent par micro-bench 11.8 → 283 µs (24×). The baseline-itself-regresses pattern. Fix in `e5ecb08`. | "Perf retrospective" section (above) |

If a refactor PR adds a new sub-library, new public class, new
runtime dep, new test pattern that throws across pybind, new wheel
platform — open this table first. Half of the v0.3.x patch series
was rediscovering items already on this list.

## Documentation impact map

When the refactor lands, these pages need edits:

  - **`README.md`** — "Python Binding" section's RunConfig table,
    "Differences from LangGraph" deltas (most entries become
    obsolete and should be deleted, not edited), "What's covered
    by the binding" surface list.
  - **`docs/reference-en.md`** (1622 lines) — GraphNode / Node /
    Provider / CancelToken / RunConfig sections. Roughly 30-40%
    rewrite. The narrative-tour structure stays; the API surface
    shrinks.
  - **`docs/concepts.md`** — 531-line conceptual narrative. The
    "8 dispatch entry points" paragraph collapses to one. Cancel
    propagation paragraph cleans up.
  - **`docs/troubleshooting.md`** — most v0.3.x entries become
    obsolete. The "silent no-op" / "forgot to override" /
    "thread_local missing" entries can be deleted.
  - **`bindings/python/examples/`** — every example (22 Python +
    30 C++) updated.
  - **`Doxyfile`** — no changes; PROJECT_NUMBER reads from
    pyproject.toml so v1.0.0 propagates automatically.
  - **`ROADMAP_v1.md`** (this file) — cross out landed candidates,
    add post-mortem on what was harder/easier than expected.

## Definition of done for v1.0

  1. Single canonical way to do each of: write a node, cancel a run,
     read state, update state, run a graph.
  2. README's "Python Binding" section reads in under 5 minutes for
     a new user.
  3. `docs/reference-en.md` GraphNode section is one method, not
     eight.
  4. v0.3.x deprecation warnings have been silent for at least one
     release before final removal.
  5. ctest / pytest / live LLM / Valgrind / Doxygen all green at
     the v1.0 tag.

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

## Candidate 5 — Cookbook track: pgvector RAG example

### Context

`bindings/python/examples/` (23 examples) covers ReAct, HITL,
intent routing, multi-agent debate, deep-research (web crawl /
web search), self-evolving graph, etc. — but **no vector
retrieval / RAG** example. Confirmed not folded into 16/17:
those are web research, not embedding-based retrieval.

RAG is one of the most common LLM patterns; absence is a real
discoverability gap for users evaluating NeoGraph.

### Why it's a cookbook entry, not an engine concern

The engine surface is sufficient as-is — `PostgresCheckpointStore`
already brings a connection pool / config story that an embedding
+ pgvector node can re-use. No engine API additions needed; the
work is purely a worked example (~150-200 lines):

  - `EmbeddingNode` — calls OpenAI embeddings or local model.
  - `RetrieveNode` — pgvector similarity query against a
    pre-populated table.
  - `RAGCallNode` — LLM call with retrieved context concatenated.
  - One-time index setup script (separate from the example body
    so the example doesn't reindex on every run).

### Why deferred from v0.3.x

The v0.3.x series was scoped around engine bugs / ergonomics
exposed by the FastAPI SSE chat-demo feedback. RAG isn't an
engine bug; it's "common pattern needs a worked example."
Belongs to a separate cookbook drumbeat where each entry is a
real-world recipe rather than a v-bump.

### Triggering round

TODO_v0.3.md item #9 — confirmed cookbook material (no engine
gap), deferred to a future "examples track" sweep.

---

## Candidate 6 — Provider single dispatch

### Symptom

`Provider` exposes four virtual methods (one dimension smaller than
GraphNode's eight):

```
complete           complete_async
complete_stream    complete_stream_async
```

`(sync/async) × (stream/non-stream)` — same shape as Candidate 1,
same "override at least one of N" contract, same trap. The
non-streaming pair is safe (one bridge step deep). The streaming
pair was unsafe pre-#10: `complete_stream` is sync httplib, the
default `complete_stream_async` bridge wrapped it inline (and the
WebSocket Responses path nested `run_sync` on top of the engine's
io_context worker), producing an intermittent segfault when called
from inside `GraphEngine::run_stream_async` (issue #4, fixed by
PR #10's worker-thread bridge + `SchemaProvider` native override).

### Why this is a cleanup, not a blocker

The concrete crash (#4) is closed — PR #10 spawns a dedicated
worker thread for the sync `complete_stream` and dispatches tokens
back onto the awaiter's executor; `SchemaProvider` overrides the
WS path to skip even the worker thread. Both `OpenAIProvider` and
`SchemaProvider` HTTP/SSE paths inherit the safe default, so the
4-virtual cross-product is no longer crash-prone. What remains is
the architectural wart: the override surface is wider than
necessary, and the safety of the bridge depends on which corner of
the cross-product you override (an invariant nothing pins at
compile time).

### Landed direction

The one-override path is additive rather than a replacement:

```cpp
class CompletionProvider : public Provider {
public:
    asio::awaitable<ChatCompletion>
    invoke_request(CompletionRequest request);

protected:
    virtual asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) = 0;
};
```

`CompletionRequest` explicitly selects collect or stream mode without
inferring transport from callback presence. Final adapters preserve all
existing `Provider` entry points. This gives new implementations one
override while leaving existing source and binary contracts intact.

### Adjacent — `schema_provider.cpp` split

`schema_provider.cpp` is ~1,800 LoC concentrating multi-vendor
schema mapping + HTTP/SSE wire + body-build + response-parse. The
single-dispatch rewrite is a natural moment to split into
`SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`.
Mentioning here so it doesn't need a separate ROADMAP entry; can
split if the work happens in different PRs.

### Triggering round

Issue #5 — surfaced while debugging #4. The concrete crash closed in
PR #10; the architectural cleanup landed through the additive
`CompletionProvider` path and a permanent compatibility policy.

### Landing log (v0.9.0 candidate cycle)

5 PR landed sequentially on master, mid-2026-05-13:

| PR | Scope | Lands in |
|---|---|---|
| **#40 (PR1)** | Add new virtual `Provider::invoke(params, on_chunk = nullptr)`. Default impl forwards to the 4 legacy virtual chain (all existing Provider subclasses behave unchanged). 6 new ctest. | v0.9.0 |
| **#41+#42 (PR2)** | Engine built-in LLM nodes (`LLMCallNode`, `IntentClassifierNode`) dispatch via `provider->invoke(params, on_chunk)`. PR #41 was merged only on the stacked base, then reapplied to master via PR #42. | v0.9.0 |
| **#43 (PR3)** | Migrate all engine-internal sync LLM call sites — `agent.cpp` (5 sites), `deep_research_graph.cpp` (6 sites). Add thread-local cancel propagation parity to `Provider::invoke()` default (reproduces legacy `complete()`'s `current_cancel_token()` behavior). 3 new ctest. | v0.9.0 |
| **#44 (PR4)** | Mark all 4 legacy virtuals with `[[deprecated]]`. Migrate 3 sites in `plan_execute_graph.cpp` to `invoke()`. Wrap the 4 virtual override blocks of `OpenInferenceProvider` and `RateLimitedProvider` (the decorators) in `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` — blocks the internal-forwarder warning; only user-facing override / call sites warn. | v0.9.0 |
| **#45 (PR5)** | C++ examples migration (`31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`). | v0.9.0 |

### Additive compatibility path (revised 2026-07)

Keep the existing `Provider` vtable and add a separate
`CompletionProvider`. New implementations take an explicit
`CompletionRequest` and override `do_invoke()` only. The existing
four virtuals and the callback-based `invoke()` are wired into the
new implementation through a final adapter; existing Provider
subclasses and the Python trampoline are preserved as-is.

The existing virtuals continue to be supported as a stable API with
no removal plan. Compatibility and security fixes continue to apply,
but there is no obligation to back-port all new features to the
existing four virtuals. New implementations and new direct calls
should use `do_invoke()` and `invoke_request()` respectively. This
policy does not change existing public signatures, virtual order,
object size, or vtable. #127's native async transport and operation
ownership are separate from this API policy.

Follow-up:

  - **6b**: New Providers are written against `CompletionProvider`. Do
    not change the immediate inheritance of existing built-ins because
    of ABI impact.
  - **6c**: Complete native async transport and request-owned
    cancellation / lifetime.
  - **adjacent**: Split `schema_provider.cpp` (1800 LoC) into
    `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl` (same
    PR as 6b above or separate — decide at implementation time).

---

## Candidate 7 — gRPC transport (opt-in component)

### Context

The HasMCP cold-email (2026-05-15) was not the trigger, but that
email gave a free industry signal that "gRPC is the next transport
direction." The MCP community is discussing [adding gRPC as a
standard transport](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966),
and Google is working on gRPC-as-native-MCP-transport. gRPC aligns
with almost every axis of NeoGraph's 4-axis narrative — protobuf
binary serialization (performance / lightweight), HTTP/2
multiplexing (multi-tenant connection cost), native bidi streaming
(token / event), and schema enforcement + small wire (embedded).

### Decision (2026-05-15)

- **No in-house implementation.** Communication protocols carry a
  significant reinvent-the-wheel risk — use the standard `grpc++` +
  `protoc`.
- **opt-in only.** `NEOGRAPH_BUILD_GRPC` option, **default OFF**.
  grpc++ pulls in protobuf + abseil + c-ares + re2 + zlib (tens of
  MB transitive), breaking the "2 deps / libc.so.6 only / 1.2 MB
  binary" lightweight axis. Default OFF is the only way to stop it,
  and applies the `cmake-option-default-flip-trap` (EDDSkills, newly
  added this session) discipline: `find_package(Protobuf/gRPC)` only
  inside the option gate; no default flips.
- **NeoGraph-native API, independent of the MCP standard.**
  `proto/neograph.proto` = `GraphService { RunGraph(unary) /
  RunGraphStream(server-stream) / Health }`. payload is a JSON
  string (preserves the graph-as-data property — if we modeled it
  with strongly-typed proto messages, every user graph change would
  regenerate the .proto). Once the MCP-over-gRPC standard is fixed,
  add an MCP-shaped service next to this one (this service
  unchanged).

### Landed (v0.9.x cycle, scaffold)

- `NEOGRAPH_BUILD_GRPC=OFF` option + conditional `find_package` +
  fatal guard.
- `proto/neograph.proto`, `src/grpc/graph_service.cpp` (hash-keyed
  compile cache — reuses the multi_tenant_chatbot cookbook pattern),
  `include/neograph/grpc/graph_service.h` (`NEOGRAPH_HAVE_GRPC`
  guard), `examples/52_grpc_server.cpp`.

### Verified — first grpc++-equipped build (2026-05-16)

After installing apt `libgrpc++-dev protobuf-compiler-grpc` (1.51.1)
+ protoc 3.21.12, build with `-DNEOGRAPH_BUILD_GRPC=ON` and
end-to-end passes:
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client`
    all compile and link OK.
  - C++ client → server: **Health** (ok/version/default_graph),
    **RunGraph** unary (`{"text":"hello from grpc"}` →
    `"HELLO FROM GRPC"`, trace=[upper]), **RunGraphStream**
    (5 events, FINAL payload, status OK). `RESULT: PASS (failures=0)`.
  - protoc codegen path (raw `add_custom_command`) works. **One bug
    fixed**: in VERBATIM mode the quotes in
    `ARGS --proto_path="${dir}"` are passed literally, so protoc
    sees `"…/proto"` (including the quotes) as the directory →
    "directory does not exist". Removing the quotes
    (`--proto_path=${dir}`) closed it.

### WSL Windows-PATH leak trap (reproducible — build environment warning)

Two contaminations were caught when building with grpc++ ON in this
environment (WSL2, massive Windows PATH leak). They do not appear on
a clean Linux host / CI, but WSL developers hit them:

  1. **anaconda re2** — When `gRPCConfig.cmake` does
     `find_package(re2)`, if no system re2 cmake config exists
     (apt `libre2-dev` is not installed), it picks up
     `/mnt/c/ProgramData/anaconda3/Library/lib/cmake/re2/
     re2Targets.cmake` (Windows) from PATH and errors in
     `set_target_properties`. Fix: `-DCMAKE_IGNORE_PREFIX_PATH=/mnt/c;…`
     + `-DCMAKE_IGNORE_PATH=…/anaconda3/Library/lib/cmake;…` → grpc
     falls back to system pkg-config re2 ("Found RE2 via
     pkg-config").
  2. **ZLIB include** — `FindZLIB` picks up the library from the
     system (`/usr/lib/.../libz.so`) but `ZLIB_INCLUDE_DIR` from
     `/mnt/c/gtk/include` (Windows zlib.h) in PATH → `-isystem
     /mnt/c/gtk/include` leaks into every grpc-linked target →
     `/mnt/c/gtk/include/libintl.h` rewrites `printf` as the
     `libintl_printf` macro → `std::printf` compile error. Fix:
     explicitly set `-DZLIB_INCLUDE_DIR=/usr/include
     -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so`.

  → Both are cousins of `cmake-option-default-flip-trap` (an
  environment leak drags `find_package` to the wrong prefix). The
  EDDSkills SKILL `wsl-windows-path-cmake-find-leak` was added
  (2026-05-16).

### NexaGraph predecessor analysis — gRPC-MCP's real ROI is the checkpoint

NeoGraph's predecessor NexaGraph (`/root/Coding/NexaGraph`) had
already implemented and operated gRPC-MCP early on. Investigation
findings (Explore, 2026-05-16):

- **Implementation substance**: `proto/rag_service.proto`
  (RAGService, 11 unary RPCs — vector_search / graph_search / ingest
  / chat history / image task / **graph checkpoint** 5 RPCs),
  `src/nexagraph/grpc_client.cpp` fully implemented, integrated in
  production from api_server.cpp via the `GRPC_TARGET` env. Server is
  dual-transport (HTTP JSON-RPC + gRPC 50051). No streaming (all
  unary).
- **Overhead-reduction claims** (`DOCS/grpc-client-plan.md`):
  serialization 1ms→0.01ms, embedding 1536d 15KB→6KB, new
  connection per request → HTTP/2 multiplexing. **No measurements —
  design rationale only.**
- **Honest evaluation**: For a typical MCP tool call, LLM inference
  (hundreds of ms) is dominant, so the 1ms serialization saving is
  noise. The area where gRPC's gain *actually exists* is
  **large structured payloads** — embedding vectors, RAG ingest, and
  especially **graph checkpoints** (`channel_values_json` +
  `channel_versions_json` grow large every step). Small tool metadata
  / string queries are <1% (cognitive complexity is not worth it). In
  other words, not "MCP in general faster", but limited to
  "large-payload MCP".

**Key finding — reordering priorities when introducing in NeoGraph:**

1. **gRPC CheckpointStore = real ROI (top-priority candidate)**.
   NexaGraph's `grpc_checkpoint.cpp` already **inherits from
   `neograph::graph::CheckpointStore`** — it used NeoGraph's
   checkpoint abstraction from that point on. In other words, almost
   drop-in porting in the form of adding `GrpcCheckpointStore` next
   to NeoGraph's `Postgres/Sqlite CheckpointStore` (~150 LoC). Big
   payload + independent of the (MCP #966) standard + fits naturally
   inside the just-built `neograph::grpc` component. Checkpoints are
   large JSON blobs every step, the only hot path where the gRPC
   binary gain is actually measurable.
2. **MCP-over-gRPC transport (general tool call) = lower priority**.
   LLM dominant, so the gain is small + MCP-over-gRPC standard not
   yet fixed (#966). After the standard is fixed, even then only for
   large-payload tools like RAG / embedding.

### GrpcCheckpointStore — landed + measured (2026-05-16)

Added to `neograph::grpc`: `GrpcCheckpointStore` (client, inherits
from `CheckpointStore` — same as NexaGraph) +
`CheckpointServiceImpl`+`run_checkpoint_server` (server, wraps an
arbitrary `CheckpointStore` backend) + `checkpoint_to/from_json`
helpers. 5 RPCs in `CheckpointService` proto. Round-trip preservation
of NeoGraph's rich fields (next_nodes vector / `CheckpointPhase` enum
/ `barrier_state` nested map / `schema_version`) that NexaGraph's
flat-mapping could not handle — example 54 correctness PASS.

**Measurement result (example_grpc_checkpoint, 1536-d embedding +
12-turn, 200 iters, localhost loopback) — closes "PLAUSIBLE BUT
UNPROVEN". Honestly though, half is rejected:**

| Metric | Value |
|---|---|
| JSON (checkpoint_json) | 29 080 B |
| Protobuf wire (CheckpointBlob) | 29 131 B |
| Notional JSON-RPC envelope | 29 155 B |
| protobuf / JSON-RPC payload | **99.9%** |
| InMemory in-process | save 27 µs / load 36 µs |
| gRPC round-trip | save 720 µs / load 755 µs |
| gRPC network overhead | **+693 µs save / +719 µs load** |

**Honest conclusion — NexaGraph's "serialization 15KB→6KB binary
compression" claim is unmet in NeoGraph's JSON-in-proto design
(payload 99.9% identical).** Reason: for graph-as-data robustness,
the entire checkpoint is packed as a single proto string field →
protobuf field-level compression does not apply. NexaGraph had
field-per-member proto so it compressed, but every checkpoint-format
drift requires a proto regen (every time `next_nodes` /
`barrier_state` / `schema_version` is added). In other words,
**we deliberately chose schema-stability over compression in this
trade-off, so the payload gain really is 0 (PROVEN: not beneficial
by design).**

gRPC's *actual* gain is only transport — HTTP/2 connection reuse
(eliminates the per-call connect of JSON-RPC / HTTP1.1). A single
loopback round-trip of +700 µs does not show this (the delta only
appears under load / remote RTT). In other words, **the transport
gain is still load-test-dependent — cannot be PROVEN with a single
measurement.**

→ Priority re-confirmed:
  - **GrpcCheckpointStore's real value = "remote checkpoint via typed
    RPC + HTTP/2 connection-reuse" + "polyglot: any language server
    can implement `CheckpointService`"**. Not the payload compression
    NexaGraph advertised. Ship it as a cookbook, but the honest
    selling point is not "compression" but "typed remote checkpoint,
    zero DB drivers in the agent process".
  - **MCP-over-gRPC transport (general tool call) = hold**. Payload
    compression does not apply with JSON-in-proto as confirmed by
    checkpoint measurement, so if tool calls follow the same design
    the compression gain is 0 + LLM dominant. Only reconsider for
    large binary tools (raw embeddings, etc.) where the standard
    (#966) is fixed and field-per-member is justified.
  - Remaining verification: under load (N concurrent checkpoint
    saves), does HTTP/2 multiplexing actually delta against
    per-call-connect — bench job candidate (sustained, not
    single-shot).

### ToolCalling: JSON-RPC vs gRPC head-to-head (2026-05-16)

User request — not checkpoint, but *tool call* itself, head-to-head
against both transports on real servers. `proto` gets
`ToolService.CallTool`, example 55 launches **the same compute fn on
both (a) httplib JSON-RPC 2.0 `tools/call` (MCP shape, HTTP/1.1
keep-alive) (b) gRPC ToolService (HTTP/2)** and measures the same.

**Honesty incident — "gRPC 70x faster" was a measurement artifact.**
First run: JSON-RPC p50 43 ms (constant regardless of payload).
43 ms = textbook signature of the TCP delayed-ACK timer. Cause:
`CPPHTTPLIB_TCP_NODELAY` default **false** → Nagle on, gRPC has
TCP_NODELAY default on → unfair. Committing "gRPC 70x" as-is would
have been a lie. Applied `Server/Client::set_tcp_nodelay(true)` on
both sides and re-measured.

**Fair-condition result (loopback, both sides keep-alive + NODELAY,
N=300 p50, 2 reproductions):**

| payload | gRPC p50 | JSON-RPC p50 | ratio |
|---|---|---|---|
| tiny args (~30 B) | 433 / 448 µs | 436 / 410 µs | **0.99–1.09× (tie)** |
| 1536-float (~12 KB) | 655 / 680 µs | 1079 / 1016 µs | **0.61–0.67× (gRPC ~1.5×)** |
| args wire (tiny) | 42 B | 118 B | envelope overhead |
| args wire (12 KB) | 12025 B | 12100 B | **99% (compression 0)** |

**Truth:**
- **Small tool call (the majority of real tool calls): JSON-RPC ≈
  gRPC tie.** Transport-switch ROI ≈ 0.
- **Large-payload tool call (~12 KB+, embedding / RAG chunk return):
  gRPC ~1.5×.** The area NexaGraph mentioned, but 1.5× not 70×.
- Payload compression is still 0 (JSON-in-proto, consistent with the
  checkpoint measurement).
- Loopback ceiling — on a real network, RTT adds equally to both
  sides and the ratio converges further toward 1. The 1.5× is the
  best case.

**Candidate 7 final verdict:**
- gRPC's ROI is (1) **polyglot sidecar / remote typed RPC** (language
  boundary), (2) **~1.5× on large-payload tool / checkpoint**. Mass
  migration of general tool calls is worthless (tie + standard #966
  not yet fixed).
- MCP-over-gRPC transport: **hold, confirmed**. "General MCP tool
  calls get faster" is disproven by measurement (tie). Only after the
  standard is fixed, and only for embedding-heavy tools.
- Nagle incident → EDDSkills SKILL candidate
  `bench-shock-number-nagle-first` (shocking transport-bench number
  = suspect TCP_NODELAY / Nagle / delayed-ACK first; a cousin of
  `perf-regression-bench-bisect`). Add after user approval.

### Why NeoGraph JSON-RPC ties gRPC — yyjson (PROVEN)

User insight: "JSON-RPC parses with yyjson so it is fast; structurally
gRPC has to win." Added a transport-stripped pure-codec microbench
to example 55 to verify:

| 12 KB payload, codec only, 5000 iters | µs |
|---|---|
| yyjson parse+dump | **38.9** |
| protobuf ser+parse | **1.75** |
| → yyjson / protobuf | **22.3× slower** |

**User is exactly right.** protobuf is a structurally 22× faster
codec. But in the round-trip the difference dilutes to 1.5× at
12 KB — the serialization gap ~37 µs is a small slice of the full
round-trip 692–1096 µs (the rest is socket I/O / syscall / HTTP
framing). **Quantitative evidence that the tool-call hot path is
dominated by socket I/O, not the codec.**

Key implication — **NeoGraph's JSON-RPC ties gRPC thanks to yyjson,
not because the JSON-RPC protocol is fast.** With a typical stack
(Python's `json` is ~50× slower than yyjson, ~2 ms on 12 KB), the
codec dominates the round-trip → there gRPC structurally dominates.
Only NeoGraph uses yyjson and thus avoids that trap.

→ This is a hidden selling point and the *final* justification for
holding Candidate 7: "Other frameworks have JSON-parsing as a
bottleneck, so gRPC transport is critical for them, but NeoGraph's
MCP / JSON-RPC is not because of yyjson." For NeoGraph specifically,
MCP-over-gRPC has even less ROI (the codec advantage is already
canceled by yyjson). gRPC is only for polyglot / remote boundary +
~1.5× on large-payload purposes — confirmed.

### NexaGraph second harvest — history compression + GrpcRemoteTool (2026-05-16)

After a full NexaGraph survey, in addition to the already-ported
`GrpcCheckpointStore`, three additional *general-purpose,
non-duplicate, not-yet-in-NeoGraph* items were additionally ported.
(The RAG-app-specific stdio / HTTP MCP in
`proto/rag_mcp_server/backend` was excluded because NeoGraph already
has it or it is app-specific. `DOCS/graph-engine-design.md` is in
fact NeoGraph's design ancestor, so it is not a "porting" target.)

1. **`neograph::history` (new core utility, additive)** — ported the
   core only from NexaGraph's CAF `compress_history` actor with the
   actor shell stripped off:
   - `compact_history(messages, Provider&, model, max_tokens=12000,
     recent_keep=6) -> awaitable<CompactedHistory>` — when the token
     estimate exceeds budget, summarize the section between (system
     1 + last N) with a single LLM call, replacing it with a
     system-summary message. `co_await provider.invoke()` (does not
     use deprecated `complete()`, zero async-lib dependency — core
     internals already use coroutines).
   - `sanitize_tool_calls(messages&)` — a defense that NeoGraph
     **completely lacked**: 2-pass removal of OpenAI tool-pairs broken
     by truncation (assistant `tool_call` with no response / tool
     message with no call), idempotent. `compact_history` applies it
     internally to its output → the compression result never
     produces a 400.
   - `estimate_tokens` — conservative ~3 chars/tok estimate (mixed
     KO / EN).
   - example 56 `history_compaction` (offline MockProvider, no key
     required) — sanitize 3→1, compact 29 msgs/975 tok →
     6 msgs/208 tok, original-unchanged verification PASS.
     `src/core/history.cpp` builds into `neograph_core` for all
     configs — 496/497 ctest PASS (1 failure = pre-existing
     `pybind_smoke` openinference module missing, unrelated).

2. **`neograph::grpc::GrpcRemoteTool`** — example 55 is the side that
   *exports* tools via gRPC (`run_tool_server`), this is its mirror —
   the side that *imports* a remote `ToolService.CallTool` as an
   ordinary `neograph::Tool`. Ported NexaGraph's `GrpcTool` adapter.
   pimpl (public header is grpc++-free, same posture as
   `GrpcCheckpointStore`). Since the simple proto has no list-tools
   RPC, the definition is injected via the ctor. Server error →
   rethrow as `runtime_error` (tool error, not transport error —
   same contract as a local Tool). example 57 `grpc_remote_tool` —
   server thread + `Tool&` polymorphic call + error path PASS.
   **Consumer-side materialization of gRPC's ROI #1 (polyglot remote
   typed RPC)** — from the agent's viewpoint, the process-boundary
   tool is indistinguishable from a local tool at the call site.

### Remaining (still open)

  - Add a `grpc-build` job to CI (apt deps + ON build +
    `example_grpc_client` / `server` smoke — on a clean ubuntu runner
    the WSL traps above do not apply).
  - `RunGraphStream`'s `ServerWriter::Write` is called inside a
    streaming-node callback — currently assumes a single super-step
    loop thread. In multi-worker fan-out graphs, if the callback is
    invoked on a worker thread, `ServerWriter` synchronization is
    required (gRPC `ServerWriter` is not thread-safe). Current
    examples are single-node, so this is not exposed.
  - TLS / auth: document user wiring instead of `run_server`'s
    insecure default.
