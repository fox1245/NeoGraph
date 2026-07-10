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
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek v4 pro via OpenRouter
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

**What the live runs showed** (DeepSeek v4 pro): it authored *coherent*
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
