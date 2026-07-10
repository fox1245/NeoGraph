# The Beast — generate · evolve · roll back

> A self-evolving agent that writes its own harness, evolves it under the
> DSL compiler, and rewinds its execution through the checkpointer.
> **Generated. Evolved. Rewound. The Beast remains.**

Most "agent frameworks" let you *build* a graph. The Beast does three
things no static harness can — and all three are **real, offline, and
deterministic** in this one program (no API key):

1. **Generates** a new harness at runtime and proves it coherent before a
   single node runs.
2. **Evolves** it with real mutation operators, using the compiler itself
   as the fitness gate.
3. **Rolls back** a running harness to any prior super-step via the
   checkpointer — genuine time-travel, not a replay.

That is only safe because in NeoGraph a harness is **data** — a topology
described in JSON (issue #56) — and the DSL compiler (issue #75) can
*prove a harness coherent before it runs*. Take that away and "an agent
that writes its own graph" is just a machine for producing broken graphs.
The compiler is what turns the monster from a liability into a category.

## Run it

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — 3 gates passed. Core lockfile nodes: s1_n s2_n s3_n
  (DSL surface expanded away: vars/templates/use gone.)

── ACT II · evolve the harness (compiler = fitness) ──
  generations: 4 · offspring: 36 · survived compile gate: 36 · rejected (invalid, never run): 0
  sample mutations that produced offspring:
    gen 1: remove_edge: removed edges[0]        →  3 nodes
    gen 1: toggle_ce: added conditional edge from s2_n  →  3 nodes
    gen 1: toggle_barrier: added barrier on s3_n →  3 nodes
  (full diffable lineage via to_json(result) — the evolutionary rollback surface.)

── ACT III · spawn + roll back through the checkpointer ──
  ran to completion, trail = ["s1_n","s2_n","s3_n"]
  checkpoint timeline (3 snapshots):
    step 0  id=aac922ed  trail=["s1_n"]
    step 1  id=4b74daa9  trail=["s1_n","s2_n"]
    step 2  id=a528eb9d  trail=["s1_n","s2_n","s3_n"]
  >> ROLLBACK to step 1 (id=4b74daa9)
     final trail was ["s1_n","s2_n","s3_n"]; restored trail = ["s1_n","s2_n"]  (later steps gone)

Generated. Evolved. Rewound. The Beast remains.
```

## Act I — generate + gate

The Beast authors a harness in the DSL **surface** (`vars` / `templates` /
`use`) and forces it through three coherence gates, in order. A harness
that fails any gate is **discarded**.

| Gate | API | Catches |
|---|---|---|
| **1. Elaborate** | `Elaborator::elaborate` | Surface errors against DSL coordinates — unknown template, missing/extra `use` args, variable cycle, node-name collision. Total & deterministic: the same DSL always yields byte-identical core, so gates 2–3 reason about a fixed artifact. |
| **2. Compile + TV** | `GraphCompiler::compile` (strict, `schema_version: 1`) + `verify_roundtrip` | A typo'd or unsupported key is a *hard error* (consumed-key accounting), not a silent drop. Translation validation then asserts `canon(source) == canon(compile(source).to_json())` — the compiler cannot have quietly rewired anything. |
| **3. Validate** | `GraphValidator::validate` | What the graph **means**: dangling edges (E3), barriers that can never fire (E8), incomplete route maps (E10), channel-effect violations (E4/E6). Errors only for constructs that can *never* be right; the rest are lint. |

The seed is one `stage` template instantiated three times via `use`;
elaboration expands it into a core chain `s1_n → s2_n → s3_n`.

## Act II — evolve (the compiler is the fitness function)

`neograph::graph::evolve()` (issue #80) runs **real mutation operators**
over the seed — `swap_template`, `add_use`, `remove_use`, `tune_param`,
`toggle_conditional_edge`, `toggle_barrier`, `add_edge`, `remove_edge`.
Every offspring passes the **compile gate first**: invalid offspring die
for free, without ever executing. The rejection rate is itself a health
metric on the operators.

The key design choice: the mutation space is the **DSL (M4), not raw
JSON**, so offspring are structurally valid *by construction* — which is
why the reject count here is 0. The gate is the safety net that makes
unconstrained evolution safe, and it stays armed on every child.

Each run emits a diffable genealogy via `to_json(result)`: every
individual's parent, generation, mutation, and core lockfile. That
lineage **is** the rollback surface at the evolutionary scale — commit
it, diff it, revert a whole generation.

## Act III — roll back (checkpointer time-travel)

The surviving harness is spawned with an `InMemoryCheckpointStore`
attached (`engine->set_checkpoint_store(store)`). The engine snapshots
state at the end of every super-step. Afterwards:

- `store->list("beast-run")` returns the full timeline — you can *see*
  `trail` grow one node per step.
- `store->load_by_id(earlier.id)` **restores** the exact channel state at
  an earlier step. The demo rolls back from `["s1_n","s2_n","s3_n"]` to
  `["s1_n","s2_n"]` — the later steps are genuinely gone. This is
  `load_by_id` / `load_latest` time-travel, the same machinery HITL
  interrupt/resume and thread-forking are built on.

## Going live — the model actually writes the harness

`the_beast.cpp` is offline (stub authors). [`the_beast_live.cpp`](the_beast_live.cpp)
is the real thing: a live LLM is handed `NodeFactory::export_schema()`
(the exact palette this engine build accepts — it cannot drift because it
*is* the engine's schema, see [`../../52_export_schema.cpp`](../../52_export_schema.cpp))
and asked to author a harness in the DSL surface. Whatever it returns
goes through the same three gates; on rejection the gate's diagnostics
are fed straight back into the conversation and the model rewrites — a
genuine self-repair loop.

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek v4 flash via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — all three gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**What the live runs showed** (DeepSeek v4 flash): it authored *coherent*
harnesses on the first try across a linear pipeline, a diamond fan-out /
barrier fan-in, and a conditional router — the self-repair loop is armed
but a capable model rarely trips it. The gates still earned their keep as
lint: they flagged a missing barrier on the diamond (E9) and unreachable
handlers on the router (E7) as warnings. The point is not that the model
fails often; it is that **when it does, it cannot get the broken harness
past the compiler** — creativity is unbounded, coherence is proven.

The nodes here are deterministic `beast_node` workers so a live run costs
one LLM call (the authoring) and executes for free; swap them for
`llm_call` and each node becomes a live call too.

## Apex — the harness devours the tools

The stub-worker demos prove the generated harness is *coherent*, but the
harness never acts. [`the_beast_apex.cpp`](the_beast_apex.cpp) is the
monster: the model is handed a **tool catalog** and asked to author a
ReAct agent — `llm_call` ⇄ `tool_dispatch` looping on `has_tool_calls`.
The harness it writes is gated for coherence, then **spawned with the
tools bound** (`ctx.tools` + `engine->own_tools`). The spawned agent then
decides, on its own, which tools to call and when.

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

A real run — the self-repair loop firing for real, then autonomous
tool-calling:

```
Tool catalog offered: calculator get_weather

── Attempt #1: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'id'    (strict, schema_version 1)
  → feeding diagnostics back for self-repair.
── Attempt #2: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'name'
  → feeding diagnostics back for self-repair.
── Attempt #3: model authors a tool-calling agent ──
  ACCEPTED — coherent tool-calling agent. Nodes: agent(llm_call) tools(tool_dispatch)

── Spawning the agent it wrote — live, tools bound ──
  user task: What is 23 multiplied by 19, and what's the weather in Seoul?
  [the harness is calling tools autonomously]
    tool → {"result":437.0}
    tool → {"weather":"19C, clear"}
  tool calls executed by the harness: 2
  final answer: 23 × 19 = 437; Weather in Seoul: 19°C, clear.

The model wrote the agent. The compiler proved it. The agent ate the tools.
```

This is the whole thesis in one run: the model hallucinated a `nodes`
schema twice (adding `id`, then `name` keys), and the strict compiler's
**consumed-key accounting rejected both** — the diagnostics went back into
the conversation and it repaired itself on the third try. Then the
machine-authored, compiler-proven agent ran a live ReAct loop and called
two tools autonomously. Creativity is unbounded, tool-use is autonomous,
**coherence is non-negotiable.**

## Forge — when it lacks a tool, it writes one

[`the_beast_forge.cpp`](the_beast_forge.cpp) is the apex plus a tool
supply chain. Given a task, it:

1. **DISCOVER** — spawns a stock MCP stdio server and lists its tools over
   the real MCP protocol (`MCPClient::get_tools`).
2. **FORGE** — for the capability the task needs but the catalog lacks,
   the architect LLM **writes a Python MCP server** implementing it; we
   materialize it to disk, launch it, and **re-discover** the new tool
   over MCP. (Self-repairs if the generated server fails to initialize.)
3. **AUTHOR** — writes a ReAct agent over the *combined* catalog; three
   gates + self-repair as always.
4. **SPAWN** — binds every discovered *and* forged tool and runs the
   agent, which calls them autonomously.

A real run — the model wrote the missing tool and the agent used it:

```
── DISCOVER · stock MCP server ──
  tools: get_current_time calculate get_weather

── FORGE · the model writes a Python MCP server for what's missing ──
  attempt #1: wrote 5225 bytes → /tmp/beast_forged_server.py
  FORGED + re-discovered over MCP: reverse_string

── AUTHOR · the model writes a ReAct agent over the full catalog ──
  #1 REJECTED at 'compile': ... unknown or unconsumed key 'id' → self-repair.
  ACCEPTED — coherent agent: agent(llm_call) tools(tool_dispatch)

── SPAWN · run the agent it wrote, tools bound ──
  [harness dispatching tools autonomously]
    tool → retsnom                         # the forged reverse_string
    tool → 2026-07-10 06:13:21 (UTC)       # the discovered get_current_time
  final answer: Reversed 'monster' → retsnom; current UTC time is 2026-07-10 06:13:21.

It discovered tools, forged the missing one, and used them all.
```

Two live MCP subprocesses (one stock, one the Beast wrote *this run*), a
real `tools/list` on each, a real ReAct loop. Only the authoring model is
remote.

### Can it define custom *nodes* too?

Honestly: NeoGraph node **types** are C++ classes registered through
`NodeFactory::register_type` — you cannot JIT-compile a brand-new atomic
C++ node type at runtime. But the intent is covered three ways that the
Beast *can* drive from data:

- **Composite nodes** — the DSL's `templates` / `use` (M4) let the model
  define reusable node/topology units purely in data; that is exactly
  what `the_beast.cpp`'s seed does.
- **Recursion** — a `subgraph` node embeds a whole harness as one node,
  so a Beast-authored harness can contain Beast-authored sub-harnesses
  (N-level self-proliferation).
- **Custom behavior via code** — the forge pattern above *is* runtime
  behavior authored by the model: a tool it wrote becomes a dispatchable
  unit. The same trick generalizes to a generic `script_node` type (a
  pre-registered C++ node that executes model-written code), which is the
  honest way to get a "new atomic node whose logic the LLM defined."

The one thing that is genuinely off the table is emitting new *compiled
C++ node classes* at runtime; everything the model needs to specialize
behavior lives in the data/script/subgraph surface the compiler already
gates.

## Script — the universal cartridge (model-authored node logic + flow)

Every variant above lets the model author *tools* (leaf capabilities).
[`the_beast_script.cpp`](the_beast_script.cpp) lets it author **node logic
— including control flow (`goto`) that tools categorically cannot
express.** `script_node` is one pre-compiled C++ node whose config carries
model-written Python; at `run()` it hands the node the channel state and
applies whatever the code returns — `{writes, goto, sends}` — to the
graph. The model defines a node's behavior *and* the graph's flow, in
data, with no recompile.

Coherence stays non-negotiable. The script declares its contract in config
(`reads` / `writes` / `goto_targets`); the harness passes the three DSL
gates PLUS a Beast-layer **contract check** (declared writes must be
declared channels; goto targets must be real nodes) PLUS a **runtime
wrapper** that rejects any write/goto outside the declaration. That
restores the effect/route guarantees at the Beast layer with **zero change
to NeoGraph core** — additive and backward compatible.

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

Live run — the model wrote a counter loop whose control flow is its own
`goto`:

```
── Attempt #1: model writes node logic ──
  ACCEPTED — coherent, and the script's write/goto surface is contract-checked.

── Spawning — the node's own code drives the loop via goto ──
  [tick #1 — script decides: continue or exit]
  [tick #2 — script decides: continue or exit]
  [tick #3 — script decides: continue or exit]
  trace: tick -> tick -> tick -> END
  final counter = 3  (the model's goto logic ran the loop, contract-enforced)
```

There are no static edges out of `tick`: the loop exists only because the
model's Python returns `{"goto": "tick"}` until the counter hits 3, then
`{"goto": "__end__"}`. `--selftest` runs the identical mechanism from a
canned harness with no API key, so CI can exercise it offline.

**Boundaries (honest).** The compiler proves the graph's *shape*; the
contract proves the node's *surface* (which channels/targets it may
touch); only the script's *inner logic* is unproven — bounded by a
`timeout` on the subprocess and `max_steps` on the run. Running
model-written code is arbitrary code execution: fine for a local,
user-driven cookbook, but production needs a sandbox around the
interpreter (Sandbox2 / nsjail) — a selectable build option, not on by
default.

## Friction surfaced

- **E6 "written but never read" on `trail`** is emitted as lint — and it
  is *correct*: `trail` is a terminal output channel that no downstream
  *node* consumes; only the host reads it back via `RunResult::channel`.
  The validator is being precise about the graph's channel surface, not
  wrong. Left visible on purpose to show the effect analysis working.
- **Serialized checkpoint state is channel-wrapped**
  (`channel_values["channels"]["trail"]["value"]`), not flat — the demo's
  `channel_of()` helper unwraps it. Same shape `RunResult::channel`
  reads.
- The core lockfile keeps `schema_version: 1` through elaboration, which
  is what opts gate 2 into strict mode — authoring in the DSL surface
  never silently downgrades the coherence guarantees the evolution loop
  depends on.
