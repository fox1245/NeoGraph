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
| 5 | pgvector RAG example | Cookbook | TODO_v0.3.md #9 |

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
| 1 | **`RunContext` plumbing (internal)** | Add `struct RunContext` to `engine.h`. Engine's `execute_graph_async` constructs and threads it through. NodeExecutor passes it to `execute_full_async`. Pybind wraps it. **No public-facing change** — old methods still take only `state`; the new `ctx` lives alongside in the dispatch path. | v0.4.0 |
| 2 | **`GraphNode::run(NodeInput) -> NodeOutput`** | New virtual on GraphNode. Default implementation delegates to the old 8 virtuals (priority order preserved). Registers as the engine's preferred dispatch entry. Existing C++ subclasses still compile + work via the default fallback. | v0.4.0 |
| 3 | **CancelToken `fork()` additive** | Add `std::shared_ptr<CancelToken> CancelToken::fork()`. Parent `cancel()` cascades to children. `add_cancel_hook` keeps working (deprecated). `run_sync(aw, cancel)` switches to `cancel->fork()`. The single-signal `slot()` API stays for the engine's outer co_spawn. | v0.4.0 |
| 4 | **Deprecation annotations** | Add `[[deprecated]]` on the 8 old virtuals + `add_cancel_hook` + the trampoline scopes. Build with `-Werror=deprecated-declarations` only on internal code; user code gets warnings. Doxygen surfaces deprecation in the rendered docs. | v0.4.0 |
| 5 | **StateView canonical, raw dict deprecated** | Mark `engine.get_state(thread_id) -> dict` as soft-deprecated in the docstring. New canonical = `get_state_view(thread_id) -> StateView`. Raw dict still returns the same shape — no behavioural break. | v0.4.0 |
| 6 | **Examples migrate** | All 30 C++ + 22 Python examples switch to the new API. Each example is a self-contained PR section, easy to review. Removes the "8 virtuals visible across examples" surface load. | v0.4.x (multiple PRs) |
| 7 | **Pybind binding migrates** | `PyGraphNode` overrides `run()` instead of the 8. Drop `CurrentCancelTokenScope` thread_local — RunContext carries the token now. | v0.4.x |
| 8 | **Docs rewrite** | `docs/reference-en.md` (1622 lines) gets a major edit: GraphNode section collapses, RunConfig grows a RunContext column, CancelToken gains a fork() example. README "Differences from LangGraph" updates the entries. | v0.5.0 |
| 9 | **Old API removal** | Delete the 8 virtuals from GraphNode. Delete `add_cancel_hook` + thread_local scope. Delete the deprecated soft-aliases. SOVERSION bumps. | v1.0.0 |

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
