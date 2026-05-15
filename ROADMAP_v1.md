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
- **Candidate 6** (Provider 4 → 1): same collapse for `Provider`.
  The "X is safe only when Y" trap of #4/#5 cannot recur because
  there is no Y to violate — there's only one entry point and
  one canonical drain pattern.

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
| 1 | Single `run()` dispatch + tags | Proposed (v0.4 PR 2 plumbed `run(NodeInput)` additively; legacy 8 still live) | v0.3.1 #2, v0.3.2 #10 (×2 langs); pattern reinforced by #36 (9 downstream findings) |
| 2 | Explicit `RunContext` arg | **Landed in v0.4–v0.8** (`RunContext::store` field added v0.8 #27) | v0.3.1 multi-Send, v0.3.2 C++ scope |
| 3 | Hierarchical CancelToken | **Landed in v0.4** (`CancelToken::fork()` + cascade) | v0.3.2 hooks, v0.3.2 emit-vs-bind |
| 4 | Self-evolving graph runtime hooks | Research | TODO_v0.3.md #8 |
| 5 | pgvector RAG example | Cookbook | TODO_v0.3.md #9 |
| 6 | Provider single dispatch | **Landed (additive + deprecation)** v0.9.0 candidate — PR #40 / #41+#42 / #43 / #44 / #45. Phase B (legacy 4 virtual 제거) v1.0.0 대기. | #4 (closed v0.7), #5 (open as v1.0 tracking item), pattern reinforced by #36 |

---

# Execution plan

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
| 9 partial | **Old API removal** — PR 9a `d1070dc` (built-in nodes migrated to run()); 9b–e (legacy 8 virtuals + add_cancel_hook + smuggling thread_local + PyGraphNodeOwner legacy overrides) **deferred to v1.0.0 release moment** — ROADMAP itself says "Drop the old API only when the deprecation warnings have been quiet for a release", so removal waits for v0.4.0 ship + deprecation feedback period. See "Post-v0.4.0 plan" section below for the 4-way sub-PR split. | v1.0.0 |

## Post-v0.4.0 plan (deprecation window → v1.0)

v0.4.0 shipped 2026-05-05 (`4cae42c`, tag `v0.4.0`). PR 1~9a + newcomer
sweep (`ee11ed6`) all landed. The remaining work is split into two
phases that run sequentially: a passive observation window, then the
destructive removal release.

### Phase A — Deprecation window (observe, don't code)

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

### Phase B — v1.0 destructive removal (4 sub-PRs, sequential)

Each sub-PR is independently mergeable on master. Land in this order;
each must keep ctest 452/452 + pytest 96/96 green at every step.
**Do NOT bundle into one commit** — review and revert cost compounds.

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

### "What if we never remove?" cost analysis

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

## Perf retrospective — `b59444f` 18일 잠재 par 회귀

v1.0 cycle 막바지에 README 의 "engine overhead" 자랑 (par 11.8 µs)
이 깨졌다는 게 드러났다. 측정 + 분신술 bisect 결과 단일 commit
`b59444f` 가 18일 (2026-04-26 → 2026-05-13) 잠재해 있던 회귀였다.

### 무슨 일이 있었나

- `b59444f` 가 `GraphEngine::compile()` 의 기본 워커 수를
  `1` 에서 `std::thread::hardware_concurrency()` 로 바꿈. 의도:
  fan-out 노드가 명시 설정 없이도 진짜 병렬 실행 받게.
- 부작용: 1-node 시퀀셜 + 5-노드 fan-out micro-bench 가 **per-iter
  cross-thread submit 비용 ~75 µs/iter** 추가로 부담. 11.8 µs →
  283 µs (24×).
- 4월 27일 perf audit (`project_perf_audit_2026-04-27.md`) 에서
  `fd60aab` 가 "fix" 했다고 기록돼있는데, 그건 별개 회귀 (시간 측정
  pattern) 였고 워커 수 기본값은 그대로 두었다. par micro-bench
  자체는 "기본=hardware_concurrency" 모드에서 측정되고 있어서
  numerically 정상으로 보였지만, 실제 README 의 11.8 µs 클레임은
  pre-b59444f 시점의 값.
- v1.0 cycle 의 Per-PR contract 가 "Not regress the bench" 요구함에도
  당시 bench 가 같은 (회귀된) baseline 에서 측정되니 ±5% band 안에
  들어와 무사통과. 18일 동안 잠재.
- 2026-05-13 매 commit 별로 분신술 bisect (`git worktree add` 11개
  병렬, taskset+chrt 측정) — `b59444f` 가 par 11.8 µs → 283 µs 점프
  단일 commit 으로 확인. revert (`e5ecb08`) 로 11.8 → 12.2 µs 복귀.

### Trade-off — 왜 기본=1 이 옳은가

`asio::thread_pool` cross-thread submit 은 task 당 약 75 µs. 노드
하나의 실행 시간이 ms 단위 (LLM 호출, HTTP 등) 면 그 비용은 묻히지만
NeoGraph 가 자랑하는 "engine overhead 시리얼/병렬 µs 단위" 패스에서는
같은 차원 비용이라 직접 보임.

- **CPU-tiny / 시퀀셜 노드 (micro-bench, validator chain 등)** — 기본=1
  이 압도적. 워커 풀 없이 io_context 한 스레드 위에서 sequential.
- **진짜 fan-out 의도 (sleep-바운드 시뮬, 별도 process 호출, sync
  HTTP)** — 사용자가 `engine->set_worker_count_auto()` 또는
  `set_worker_count(N)` 명시 호출해야 함. 한 줄.

이 트레이드오프를 명문화하려고 `e5ecb08` 의 commit message + 직후
fan-out 예제 5곳 (10/14/21/36 + deep_research_graph builder) 에
`set_worker_count_auto()` 명시 호출 추가 + migration doc 의 Migration
3 섹션 보강.

### Per-PR contract 보강 (다음 회귀 방지)

`bench_neograph par` micro-bench 가 baseline 대비 ±5% 안에 있을지만
검사하는 게 부족했음 — baseline 자체가 회귀된 상태에서 함께 미끄러질
수 있음. 다음 패치에서:

  - bench-regression CI 가 **README 에 명시된 절대 값** (`seq ≈ 5.0
    µs`, `par ≈ 12 µs` 같은 wall-time anchor) 을 두 번째 게이트로
    사용. baseline 자체 회귀 캐치.
  - 또는 GitHub Actions 측 cron 으로 master → master 7일 회귀 측정
    cron 추가 (nightly-soak 같은 패턴).
  - per-PR diff 가 `GraphEngine::compile()` 또는 `set_worker_count` 를
    건드리면 PR 본문에 "별도 micro-bench 측정 결과" 필수 (CODEOWNERS
    훅으로 자동화).

세 가지 다 후속 작업. v1.0 release 전 한 가지는 들어가야 함.

### 무엇을 배웠나

1. **"기본값 변경"은 functional 한 의미가 없어 보여도 perf-critical 한
   contract 일 수 있다.** README 가 자랑하는 숫자가 "기본값" 패스에서
   나온 값이라면, 기본값 변경 = README 변경.
2. **회귀 측정의 baseline 도 회귀할 수 있다.** ±band 비교만 하지 말고
   절대값 anchor 도 두자.
3. **분신술 bisect (병렬 worktree 11개 + 결과 취합) 가 18일 잠재 회귀
   pinpoint 하는데 30분이면 충분.** Linear bisect 보다 훨씬 빠름 —
   master 가 길어졌을 때 default 도구.

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
| `compile()` default worker count regression | `b59444f` 가 기본을 `1 → hardware_concurrency` 로 바꿔 par micro-bench 11.8 → 283 µs (24×) 잠재. baseline 자체 회귀 패턴. fix `e5ecb08` 로 복귀. | "Perf retrospective" 섹션 (위) |

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

### Suggested direction (v1.0)

Async-first single dispatch:

```cpp
class Provider {
public:
    // Only thing native subclasses override.
    virtual asio::awaitable<CompletionStream>
    invoke(CompletionRequest req) = 0;
};
```

`CompletionStream` is a sender / awaitable / channel — the caller
picks how to drain it (collect-to-end for sync, await-final for
non-streaming async, iterate for streaming). Sync + non-streaming
variants become thin base-class adapters that apply `run_sync`
exactly once at the outermost boundary, never composed with each
other. Native subclasses only ship the async-streaming path
because it's a strict superset of the rest.

Pair with Candidate 1 (GraphNode 8-virtual flattening) — same
shape, same fix, should land in the same v1.0 cycle so the
"override one virtual" contract is consistent across the public
surface.

### Adjacent — `schema_provider.cpp` split

`schema_provider.cpp` is ~1,800 LoC concentrating multi-vendor
schema mapping + HTTP/SSE wire + body-build + response-parse. The
single-dispatch rewrite is a natural moment to split into
`SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`.
Mentioning here so it doesn't need a separate ROADMAP entry; can
split if the work happens in different PRs.

### Triggering round

Issue #5 — surfaced while debugging #4. Concrete crash closed by
PR #10; architectural cleanup deferred to v1.0 alongside
Candidate 1.

### Landing log (v0.9.0 candidate cycle)

5 PR landed sequentially on master, mid-2026-05-13:

| PR | Scope | Lands in |
|---|---|---|
| **#40 (PR1)** | `Provider::invoke(params, on_chunk = nullptr)` 새 virtual 추가. default impl 이 4 legacy virtual chain 으로 forward (모든 기존 Provider subclass 무변경 동작). 6 신규 ctest. | v0.9.0 |
| **#41+#42 (PR2)** | engine 의 built-in LLM 노드 (`LLMCallNode`, `IntentClassifierNode`) 가 `provider->invoke(params, on_chunk)` 통해 dispatch. PR #41 가 stacked base 에만 머지되어 PR #42 로 master 재적용. | v0.9.0 |
| **#43 (PR3)** | engine-내부 sync LLM 호출 사이트 모두 마이그레이션 — `agent.cpp` (5 사이트), `deep_research_graph.cpp` (6 사이트). `Provider::invoke()` default 에 thread-local cancel propagation parity 추가 (legacy `complete()` 의 `current_cancel_token()` 동작 재현). 3 신규 ctest. | v0.9.0 |
| **#44 (PR4)** | 4 legacy virtual 모두 `[[deprecated]]` 마커. `plan_execute_graph.cpp` 3 사이트 invoke() 마이그레이션. `OpenInferenceProvider` 와 `RateLimitedProvider` (decorator 들) 의 4 virtual override 블록을 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 로 감쌈 — internal forwarder warning 차단, user-facing override / 호출 사이트만 warning. | v0.9.0 |
| **#45 (PR5)** | C++ examples 마이그레이션 (`31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`). | v0.9.0 |

### v1.0 destructive removal (deferred)

`SchemaProvider` / `OpenAIProvider` / `RateLimitedProvider` /
`OpenInferenceProvider` 는 4 legacy virtual override 와 새 invoke()
의 default forward 를 함께 갖고 있는 상태. v1.0.0 sub-PR 시퀀스 (Candidate 1
의 9b–9e mirror):

  - **6b**: native subclass 가 invoke() native override (4 virtual
    override 의 코드를 invoke 안으로 통합). 기존 4 override 는 thin
    adapter 또는 제거.
  - **6c**: 4 legacy virtual 자체 삭제 from `Provider`. internal
    forwarder PUSH_IGNORE 가드 제거. v1.0.0 에서 `Provider` 가 단일
    pure-virtual `invoke()` + `get_name()` 만.
  - **adjacent**: `schema_provider.cpp` (1800 LoC) 의 `SchemaParser` /
    `SchemaWireBuilder` / `SchemaProviderImpl` 분할 (위 6b 와 같은
    PR 또는 별도 — 구현 시 결정).

---

## Candidate 7 — gRPC transport (opt-in component)

### Context

HasMCP cold-email (2026-05-15) 이 trigger 한 게 아니라, 그 메일이
"gRPC 가 다음 transport 방향" 이라는 업계 신호를 공짜로 줬다. MCP
커뮤니티가 [gRPC를 표준 transport로 추가](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966)
논의 중이고 Google 이 gRPC-as-native-MCP-transport 작업 중. gRPC 는
NeoGraph 4축 narrative 와 거의 모든 축에서 정합 — protobuf 바이너리
직렬화(성능/경량), HTTP/2 multiplexing(multi-tenant connection 비용),
네이티브 bidi streaming(token/event), 스키마 강제+작은 wire(임베디드).

### 결정 (2026-05-15)

- **자체 구현 X.** 통신 프로토콜은 reinvent-the-wheel 리스크가 큼 —
  표준 `grpc++` + `protoc` 사용.
- **opt-in only.** `NEOGRAPH_BUILD_GRPC` 옵션, **default OFF**.
  grpc++ 가 protobuf + abseil + c-ares + re2 + zlib (수십 MB
  transitive) 를 끌어와 "2 deps / libc.so.6 only / 1.2 MB binary"
  경량 축을 깬다. default OFF 가 그걸 막는 유일한 방법 +
  `cmake-option-default-flip-trap` (EDDSkills, 같은 세션 신설) 규율
  적용: `find_package(Protobuf/gRPC)` 를 옵션 gate 안에만, default
  flip 금지.
- **NeoGraph-native API, MCP 표준과 독립.** `proto/neograph.proto` =
  `GraphService { RunGraph(unary) / RunGraphStream(server-stream) /
  Health }`. payload 는 JSON string (graph-as-data 속성 보존 — proto
  강타입 메시지로 모델링하면 user graph 변경마다 .proto regen).
  MCP-over-gRPC 표준이 확정되면 그때 MCP-shaped service 를 이것 옆에
  추가 (이 service 무변경).

### Landed (v0.9.x cycle, scaffold)

- `NEOGRAPH_BUILD_GRPC=OFF` 옵션 + conditional `find_package` + fatal
  guard.
- `proto/neograph.proto`, `src/grpc/graph_service.cpp` (hash-keyed
  compile cache — multi_tenant_chatbot cookbook 패턴 재사용),
  `include/neograph/grpc/graph_service.h` (`NEOGRAPH_HAVE_GRPC` 가드),
  `examples/52_grpc_server.cpp`.

### Verified — first grpc++-equipped build (2026-05-16)

apt `libgrpc++-dev protobuf-compiler-grpc` (1.51.1) + protoc 3.21.12
설치 후 `-DNEOGRAPH_BUILD_GRPC=ON` 빌드 + end-to-end 통과:
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client`
    전부 컴파일·링크 OK.
  - C++ client → server: **Health** (ok/version/default_graph),
    **RunGraph** unary (`{"text":"hello from grpc"}` →
    `"HELLO FROM GRPC"`, trace=[upper]), **RunGraphStream**
    (5 events, FINAL payload, status OK). `RESULT: PASS (failures=0)`.
  - protoc codegen 경로 (raw `add_custom_command`) 동작. 단 **버그
    1개 fix**: VERBATIM 모드에서 `ARGS --proto_path="${dir}"` 의
    따옴표가 literal 로 전달돼 protoc 가 `"…/proto"` (따옴표 포함)
    를 디렉토리로 인식 → "directory does not exist". 따옴표 제거
    (`--proto_path=${dir}`) 로 닫음.

### WSL Windows-PATH 누출 함정 (재현 가능 — 빌드 환경 주의)

이 환경(WSL2, Windows PATH 거대 누출)에서 grpc++ ON 빌드 시 2개
오염이 잡혔다. 깨끗한 Linux 호스트/CI 면 안 나지만 WSL 개발자는 부딪침:

  1. **anaconda re2** — `gRPCConfig.cmake` 가 `find_package(re2)` 할
     때 시스템에 re2 cmake config 가 없으면(apt `libre2-dev` 는 안 깜)
     PATH 의 `/mnt/c/ProgramData/anaconda3/Library/lib/cmake/re2/
     re2Targets.cmake` (Windows) 를 잡고 `set_target_properties`
     에러. fix: `-DCMAKE_IGNORE_PREFIX_PATH=/mnt/c;…` +
     `-DCMAKE_IGNORE_PATH=…/anaconda3/Library/lib/cmake;…` → grpc 가
     시스템 pkg-config re2 로 fallback ("Found RE2 via pkg-config").
  2. **ZLIB include** — `FindZLIB` 가 lib 는 시스템
     (`/usr/lib/.../libz.so`) 잡지만 `ZLIB_INCLUDE_DIR` 은 PATH 의
     `/mnt/c/gtk/include` (Windows zlib.h) 를 잡음 → `-isystem
     /mnt/c/gtk/include` 가 모든 grpc-링크 타깃에 누출 →
     `/mnt/c/gtk/include/libintl.h` 가 `printf` 를 `libintl_printf`
     매크로로 치환 → `std::printf` 컴파일 에러. fix:
     `-DZLIB_INCLUDE_DIR=/usr/include -DZLIB_LIBRARY=/usr/lib/
     x86_64-linux-gnu/libz.so` 명시.

  → 둘 다 `cmake-option-default-flip-trap` 의 사촌 (환경 누출이
  find_package 를 엉뚱한 prefix 로 끌고 감). EDDSkills SKILL
  `wsl-windows-path-cmake-find-leak` 추가 완료 (2026-05-16).

### NexaGraph 전신 분석 — gRPC-MCP 의 진짜 ROI 는 checkpoint

NeoGraph 의 전신 NexaGraph (`/root/Coding/NexaGraph`) 가 초기에 이미
gRPC-MCP 를 구현·동작시켰음. 조사 결과 (Explore, 2026-05-16):

- **구현 실체**: `proto/rag_service.proto` (RAGService, 11 unary RPC —
  vector_search / graph_search / ingest / chat history / image task /
  **graph checkpoint** 5 RPC), `src/nexagraph/grpc_client.cpp` 완전
  구현, api_server.cpp 에서 `GRPC_TARGET` env 로 production 통합.
  서버는 dual-transport (HTTP JSON-RPC + gRPC 50051). streaming 없음
  (전부 unary).
- **오버헤드 감소 주장** (`DOCS/grpc-client-plan.md`): 직렬화
  1ms→0.01ms, 임베딩 1536d 15KB→6KB, 요청마다 새 연결→HTTP/2
  multiplexing. **측정치 없음 — 설계 rationale 만.**
- **정직한 평가**: 일반 MCP tool call 은 LLM inference (수백 ms) 가
  dominant 라 직렬화 1ms 절감은 noise. gRPC 이득이 *실재하는* 영역은
  **대용량 구조화 payload** — embedding 벡터, RAG ingest, 그리고
  특히 **graph checkpoint** (channel_values_json + channel_versions_
  json 이 step 마다 큼). 작은 tool metadata/string query 는 <1%
  (인지 복잡도 안 맞음). 즉 "MCP 전반 빠르게" 가 아니라 "대용량
  payload MCP" 한정.

**핵심 발견 — NeoGraph 도입 시 우선순위 재정렬:**

1. **gRPC CheckpointStore = 진짜 ROI (1순위 후보)**. NexaGraph 의
   `grpc_checkpoint.cpp` 가 이미 **`neograph::graph::CheckpointStore`
   를 상속** — NeoGraph 의 checkpoint 추상을 그때부터 썼다. 즉
   NeoGraph 의 `Postgres/Sqlite CheckpointStore` 옆에 `GrpcCheckpoint
   Store` 를 추가하는 형태로 거의 그대로 포팅 가능 (~150 LoC). 큰
   payload + 표준(MCP #966) 무관 + 방금 만든 `neograph::grpc`
   컴포넌트 안에 자연스럽게 들어감. checkpoint 는 step 마다 큰 JSON
   blob 이라 gRPC binary 이득이 측정 가능한 유일한 hot path.
2. **MCP-over-gRPC transport (일반 tool call) = 후순위**. LLM
   dominant 라 이득 작음 + MCP-over-gRPC 표준 미확정(#966). 표준
   확정 후, 그때도 RAG/embedding 같은 대용량 tool 에 한해.

### GrpcCheckpointStore — landed + measured (2026-05-16)

`neograph::grpc` 에 `GrpcCheckpointStore`(client, `CheckpointStore`
상속 — NexaGraph 와 동일) + `CheckpointServiceImpl`+`run_checkpoint_
server`(server, 임의 `CheckpointStore` backend wrap) + `checkpoint_to
/from_json` 헬퍼 추가. proto 에 `CheckpointService` 5 RPC. NexaGraph
flat-mapping 이 못 한 NeoGraph rich fields (next_nodes vector /
CheckpointPhase enum / barrier_state nested map / schema_version) 까지
round-trip 보존 — example 54 correctness PASS.

**측정 결과 (example_grpc_checkpoint, 1536-d embedding + 12-turn,
200 iters, localhost loopback) — "PLAUSIBLE BUT UNPROVEN" 닫음. 단
정직하게, 절반은 기각:**

| 항목 | 값 |
|---|---|
| JSON (checkpoint_json) | 29 080 B |
| Protobuf wire (CheckpointBlob) | 29 131 B |
| Notional JSON-RPC envelope | 29 155 B |
| protobuf / JSON-RPC payload | **99.9%** |
| InMemory in-process | save 27 µs / load 36 µs |
| gRPC round-trip | save 720 µs / load 755 µs |
| gRPC 네트워크 오버헤드 | **+693 µs save / +719 µs load** |

**정직한 결론 — NexaGraph 의 "직렬화 15KB→6KB binary 압축" 주장은
NeoGraph 의 JSON-in-proto 설계에서 미달성 (payload 99.9% 동일).**
이유: graph-as-data robustness 위해 checkpoint 전체를 proto string
한 필드로 담음 → protobuf field-level 압축 안 걸림. NexaGraph 는
field-per-member proto 라 압축됐지만 checkpoint format drift 마다
proto regen (next_nodes/barrier_state/schema_version 추가 시마다). 즉
**압축 vs schema-안정성 trade-off 에서 후자를 의도 선택했고, 그래서
payload 이득은 0 이 맞다 (PROVEN: not beneficial by design).**

gRPC 의 *실재* 이득은 transport 뿐 — HTTP/2 connection reuse (JSON-
RPC/HTTP1.1 의 per-call connect 제거). 단일 loopback round-trip
+700 µs 측정으론 안 드러남 (부하·원격 RTT 에서만 delta). 즉
**transport 이득은 still load-test-dependent — 단발 측정으로 PROVEN
못 함.**

→ 우선순위 재확정:
  - **GrpcCheckpointStore 의 진짜 가치 = "원격 checkpoint 를 typed
    RPC + HTTP/2 connection-reuse 로" + "polyglot: 어느 언어 서버든
    CheckpointService 구현 가능"**. NexaGraph 가 광고한 payload
    압축이 아님. cookbook 으로 ship 하되 셀링 포인트를
    "압축" 아니라 "typed remote checkpoint, DB driver 0 in agent
    process" 로 정직하게.
  - **MCP-over-gRPC transport (일반 tool call) = 보류 유지**. payload
    압축이 JSON-in-proto 에서 안 걸리는 게 checkpoint 에서 측정으로
    확인됐으니, tool call 도 같은 설계면 압축 이득 0 + LLM dominant.
    표준 #966 확정 + field-per-member 가 정당화되는 대용량 binary
    tool (raw embedding 등) 한정으로만 재검토.
  - 남은 검증: 부하 상황 (N 동시 checkpoint save) 에서 HTTP/2
    multiplexing 이 per-call-connect 대비 실제 delta 내는지 —
    bench job 후보 (단발 아닌 sustained).

### Remaining (still open)

  - CI 에 `grpc-build` job 추가 (apt deps + ON 빌드 +
    `example_grpc_client`/`server` 스모크 — 깨끗한 ubuntu runner 라
    위 WSL 함정은 없음).
  - `RunGraphStream` 의 `ServerWriter::Write` 가 streaming node
    callback 안에서 호출됨 — 현재 단일 super-step loop thread 가정.
    multi-worker fan-out 그래프에서 callback 이 worker thread 에서
    불리면 `ServerWriter` 동기화 필요 (gRPC `ServerWriter` 는
    not thread-safe). 현 example 은 단일-노드라 미노출.
  - TLS / auth 는 `run_server` 의 insecure 기본 대신 사용자 wiring
    문서화.
