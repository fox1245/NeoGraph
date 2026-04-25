# Pybind11 binding — next-session handoff

Living plan for the next NeoGraph session. Sister docs / context the
next session should also read:

- `README.md` — architecture, build options, BUILD_SHARED_LIBS section
  (added 2026-04-25)
- Memory: `~/.claude/projects/-root-Coding-NeoGraph/memory/project_neograph.md`
  (project overview), `plan_pybind11_binding.md` (design notes), and
  the wider MEMORY.md index.

## TL;DR — pick this up

1. **Design + 1st commit (~2-4 h)**: minimal pybind11 module exposing
   `Provider`, `Tool`, `GraphEngine::compile`, `RunConfig`, `RunResult`.
   Goal: a Python script can call `compile(json_def, ctx).run(config)`
   and round-trip a JSON state.
2. **Custom Python nodes (~2-4 h)**: pybind11 trampoline so a Python
   subclass of `neograph.GraphNode` can override `execute(state)` and
   the engine dispatches into Python under proper GIL handling.
3. **Wheel packaging (~1-2 d)**: manylinux + macOS arm64 + Win wheels
   carrying `libneograph_*.so` plus the binding. Synergizes with the
   shared-libs work just landed (commit 85619e6) — wheel installs are
   inherently dynamic-linking, so SHARED is the right mode.

## Why this matters

The pitch is *"LangGraph-grade Python ergonomics + NeoGraph µs latency
in one import"*. Today a NeoGraph user has two choices: write C++ end
to end (a real barrier for the Python-fluent majority of agent
developers) or ignore NeoGraph entirely. Pybind11 closes that gap
without forking the engine.

Concrete wins versus pure LangGraph:
- **Engine overhead drops 130×–640×** — see README.md "Engine overhead
  vs Python graph/pipeline frameworks" benchmark. NeoGraph's 5 µs
  per super-step vs LangGraph's 656 µs survives the binding layer
  intact (pybind11 dispatch is sub-µs).
- **`pip install neograph`** — no Docker, no venv hell. Self-contained
  wheel with bundled .so.
- **Same Python primitives that LangChain users already know** —
  `state.get("messages")`, `ChannelWrite("findings", ...)`, etc.
- **Migration path** — a LangGraph user can swap their engine without
  rewriting their tool integrations or LLM client code.

## Today's context (2026-04-25)

What landed in this session that the binding builds on:

- **BUILD_SHARED_LIBS support** (commit `85619e6`, follow-up `110a5fb`):
  NeoGraph now builds as `.so`/`.dylib` on Linux/macOS. RPATH
  (`$ORIGIN`/`@loader_path`) wires up cleanly. 330/330 ctest pass.
  Downstream re-agent verified.
- **`re-agent` repo** (`fox1245/re-agent`, private, Phase 3 closed):
  parallel fan-out + checkpoint + resume + dual backend, ~750 LOC
  one-file C++. Pin bumped to NeoGraph `110a5fb` in commit `2d57787`.
- **`re-agent-reimpl` repo** (`fox1245/re-agent-reimpl`, private,
  Phase 4 closed): clean-room reimpl agent. Test-driven loop converged
  10/10 on crackme01 in 4 iters / $0.025.

Both downstream repos are private — they're personal-use RE tooling.
The Pybind11 work belongs strictly to NeoGraph (public).

## Pybind11 design

### Surface to expose (commit 1)

Read-only mapping with thin marshalling. JSON → `py::dict` happens at
the boundary, NOT inside the engine.

```python
import neograph
from neograph.llm import OpenAIProvider

provider = OpenAIProvider(api_key="sk-...", default_model="gpt-4o-mini")

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": "__start__", "to": "llm"},
        {"from": "llm", "to": "__end__"},
    ],
}

ctx = neograph.NodeContext(provider=provider)
engine = neograph.GraphEngine.compile(definition, ctx)

result = engine.run({"messages": [{"role": "user", "content": "Hi"}]})
print(result.output["channels"]["messages"]["value"])
```

Symbols to wrap:

| C++ type / fn | Python | Notes |
|---|---|---|
| `neograph::Provider` | `neograph.Provider` (abstract) | Base class. Subclasses `OpenAIProvider`, `SchemaProvider` exposed in `neograph.llm`. |
| `neograph::CompletionParams` | `neograph.CompletionParams` | Plain data class. |
| `neograph::ChatCompletion` | `neograph.ChatCompletion` | Plain data class. |
| `neograph::Tool` | `neograph.Tool` (abstract) | For pure-Python tool subclasses use the trampoline (commit 2). |
| `neograph::graph::NodeContext` | `neograph.NodeContext` | Constructable from kwargs (`provider=`, `tools=`, `model=`, `instructions=`). |
| `neograph::graph::GraphEngine` | `neograph.GraphEngine` | Static `compile(definition, ctx, store=None)` + instance `run(input)` / `run_async(input)`. |
| `neograph::graph::RunConfig` | `neograph.RunConfig` | Constructable, `input` accepts a dict. |
| `neograph::graph::RunResult` | `neograph.RunResult` | `.output` exposes nested dict. |
| `neograph::graph::ChannelWrite` | `neograph.ChannelWrite` | Plain data class. |
| `neograph::graph::Send` | `neograph.Send` | Plain data class. |
| `neograph::graph::Command` | `neograph.Command` | Plain data class. |
| `neograph::graph::GraphState` | `neograph.GraphState` | Read-only view from inside a custom node. `.get(channel)` returns `dict`/`list`/`str`/`int` per JSON type. |

### Custom Python nodes (commit 2)

Trampoline that lets a Python subclass of `neograph.GraphNode` plug
into the C++ scheduler:

```python
class MyAnalyzeNode(neograph.GraphNode):
    def __init__(self, provider):
        super().__init__()
        self.provider = provider

    def execute(self, state):
        target = state.get("target_function")
        # ...do work in Python, possibly calling self.provider.complete(...)
        return [neograph.ChannelWrite("findings", [proposal])]

# Register so the JSON definition can reference it by type name
neograph.NodeFactory.instance().register_type(
    "analyze",
    lambda name, json, ctx: MyAnalyzeNode(ctx.provider),
)
```

GIL handling — *the* hairy bit. Two rules:

1. **C++ that doesn't call Python releases the GIL**:
   `engine->run(...)` and `engine->run_async(...)` wrap with
   `py::gil_scoped_release` so other Python threads keep running while
   the engine churns.
2. **C++ that calls into a Python node acquires the GIL**:
   the node trampoline wraps every dispatch with
   `py::gil_scoped_acquire` before invoking the Python `execute` method,
   then releases when returning.

The engine already runs Send branches on `fan_out_pool_` worker
threads (added in re-agent's `set_worker_count` work — see
`graph_engine.cpp:60-62` for the contract). Each worker thread will
call into Python and need its own GIL acquire. Pybind11's
`gil_scoped_acquire` handles that re-entrantly.

### Wheel packaging (commit 3+)

- `pyproject.toml` with `cmake-build-extension` or `scikit-build-core`
  driving the CMake invocation. `scikit-build-core` is the modern
  default, ~30-line config.
- Force `BUILD_SHARED_LIBS=ON` in the wheel build so the bundled
  `libneograph_*.so` siblings work alongside the binding `.so`. Set
  RPATH `$ORIGIN` (already configured in NeoGraph CMakeLists).
- `cibuildwheel` matrix in CI for manylinux2014 (x86_64, aarch64),
  macOS (universal2), and Windows. Note Windows DLL exports aren't
  wired (warned at configure) — the Win row is **deferred** until the
  `NEOGRAPH_API` macro pass is done.

## First commit shape — minimal pybind11 module

Targets to land in commit 1:

```
bindings/
  python/
    CMakeLists.txt         ← pybind11_add_module(...)
    src/
      module.cpp           ← root PYBIND11_MODULE
      bind_provider.cpp
      bind_graph.cpp
      bind_state.cpp
    pyneograph/
      __init__.py          ← re-exports + version
    tests/
      test_smoke.py        ← compile + run a 2-node graph end-to-end
```

CMake integration in root `CMakeLists.txt`:

```cmake
option(NEOGRAPH_BUILD_PYBIND "Build Python bindings (pybind11)" OFF)
if(NEOGRAPH_BUILD_PYBIND)
    add_subdirectory(bindings/python)
endif()
```

Default OFF so existing C++ builds aren't affected. Wheel build flips
it ON via the scikit-build config.

## Open questions for the user

These are decisions to confirm before commit 1 lands:

1. **Repo location** — bindings live IN NeoGraph (`bindings/python/`)
   or as a separate sister repo (`pyneograph`)?
   - In-repo: tighter feedback loop, single source of truth, version
     always matches engine.
   - Separate repo: PyPI release independent of NeoGraph tag cuts.
   - Recommendation: **in-repo**. The release surface stays manageable.

2. **Package name on PyPI** — `neograph` (clean, but namespace squat
   risk if someone else takes it) or `neograph-engine` /
   `pyneograph` (safer)?
   - Recommendation: check PyPI first, take `neograph` if free.

3. **Async API exposure** — expose `engine.run_async()` as `async def`
   in Python (returns awaitable) or only sync `run()`?
   - Async needs asio↔asyncio bridge → trickier. Sync-first is OK
     for commit 1.

4. **Custom-node API ergonomics** — go with `class MyNode(GraphNode)`
   trampoline pattern, OR a decorator (`@neograph.node`) wrapping a
   plain function? Decorator is more Pythonic but loses access to
   `Command` / `Send` emission.
   - Recommendation: **both**. Trampoline as primary, decorator as
     sugar for the common write-only case.

## What not to scope into pybind11 commits

- **Anthropic / Gemini providers** — those are NeoGraph-side work, not
  binding work. If they don't exist yet in C++ when the binding lands,
  they just don't show up in `neograph.llm` until they do.
- **MCP client binding** — `neograph::mcp::MCPClient` is useful from
  Python but has subprocess-spawning quirks that need careful GIL
  handling. Defer to commit 4+.
- **Postgres checkpoint binding** — there's already `psycopg2` /
  `asyncpg` for Python users. Wrapping `PostgresCheckpointStore`
  duplicates that. SQLite checkpoint binding IS worth doing because
  the wire format dedup matters and SQLite has no Python equivalent
  with the same schema.

## Estimated total scope

- Commit 1 (basic surface): 2–4 hours
- Commit 2 (Python custom nodes + GIL): 2–4 hours
- Commit 3 (wheel packaging, Linux only): 4–8 hours
- Commit 4+ (cibuildwheel matrix, MCP binding, async): 1–2 days
- **Total to "pip install neograph" working on Linux**: 1–2 days
- **Total to multi-platform wheels**: 3–5 days

## Verifying the handoff

When picking this up next session, sanity-check that the upstream
state hasn't drifted:

```bash
cd /root/Coding/NeoGraph
git log --oneline -5
# expect: 110a5fb cleanup, 85619e6 BUILD_SHARED_LIBS, c7ee23e split, ...
git status                  # should be clean
ls build-shared-test/lib*.so 2>/dev/null && echo "shared build cached" \
                              || echo "rebuild needed: cmake -S . -B build-shared-test -DBUILD_SHARED_LIBS=ON -DNEOGRAPH_BUILD_TESTS=ON && cmake --build build-shared-test -j"
```

Then start with the design questions above and land commit 1.
