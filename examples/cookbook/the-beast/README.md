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
user-driven cookbook, but production wants a sandbox around the
interpreter. That is a **build option**, off by default:

Sandboxed-api embeds poorly via FetchContent, so link a pre-built tree
(build recipe in the CMake comment above the option):

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

With it on, the python runs under Google **Sandbox2** — its own
user/pid/mount/net namespaces, a read-only FS view limited to the
interpreter + the two work files, and CPU/wall/file rlimits. Needs
`libcap-dev`, `libunwind-dev`, a C++20 toolchain; verified on Linux/WSL2.

**Seccomp policy synthesised from the effect contract.** Python's syscall
footprint is too large to allowlist safely, so the default action stays
permissive — but the node's declared *capabilities* subtract syscalls: a
node that declares no `"net"` capability has `socket`/`connect`/`bind`/…
seccomp-blocked (EPERM); no `"exec"` capability blocks `execve`/`execveat`.
The policy is *derived from the declared contract*, not hand-written. This
was verified with a negative test — the **same** python, under the **same**
sandbox, differing only by the declared cap:

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

So it is more than the network namespace: with no `net` cap, the
`socket()` *syscall* fails (defense in depth on top of the netns). Honest
scope: this is **container-grade + a contract-derived seccomp blocklist**,
not a full syscall allowlist — a kernel exploit via an unblocked syscall is
still not contained. A tighter per-node allowlist (and capability-based
secret mediation) is the documented next step.

## Evolve — memetic (Darwinian + Lamarckian)

The offline `the_beast.cpp` runs evolution.h's `evolve()`, but that path's
`evaluate()` is **gate-only**: it compiles/validates a mutant and calls it
"cost 0" — it never *runs* the harness or scores its output. So fitness is
flat and nothing climbs (the best individual stays the seed).

[`the_beast_evolve.cpp`](the_beast_evolve.cpp) supplies the missing layer:
a **real fitness that executes the harness and scores its actual output**,
then drives a memetic loop.

- **The task** (a genuine one, output-scored — not a structural proxy):
  assemble an ARITHMETIC PIPELINE that computes a target number. Five op
  nodes exist — `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` — each reads
  the `acc` channel (init 0), applies its op, writes it back. The harness's
  answer is whatever `acc` holds after execution; **fitness =
  `-(|acc - 20|)`**. The *topology* (which ops run, in what order)
  determines the number, so evolving the wiring evolves the computation.
- **Darwinian**: random rewiring (`all_operators()`) + selection by the
  measured output — stumbles toward 20.
- **Lamarckian**: an LLM does the arithmetic, wires a chain that hits 20
  exactly, and injects that acquired solution as a heritable seed.

```console
$ ./build/cookbook_the_beast_evolve --darwin-only   # offline, deterministic
gen 0  seed acc=5   fitness -15
gen 1  best acc=10  fitness -10  (mut)
gen 2  best acc=24  fitness -4   (mut)   # overshoot
gen 6  best acc=19  fitness -1   (mut)
gen 9  best acc=20  fitness -0   (mut)   → Solved
champion: acc=20, origin 'mut'. Pure Darwinian mutation + selection.

$ ./build/cookbook_the_beast_evolve                 # + Lamarckian (needs OPENROUTER_API_KEY)
gen 2  best acc=24  fitness -4  (mut)
gen 3  [Lamarckian] LLM refinement acc=20  fitness -0  → injected (heritable)
       Solved via Lamarckian injection.
champion: acc=20, origin 'LLM'. The winner is a Lamarckian acquired trait ...
```

The contrast is the whole point: **blind mutation stumbles toward the
target** (5→10→24→19→20 by generation 9, computing the number by trial);
**the LLM does the arithmetic** — `(0+2)*5*2 = 20` — and jumps straight to
the answer when injected. Because that acquired solution becomes the
heritable champion (`origin 'LLM'`), it is Lamarckian; blind variation +
selection is Darwinian; running both is a memetic algorithm.

Honest notes: pure Darwinian is verified offline and deterministic. The
Lamarckian LLM call (deepseek-v4-flash) is **occasionally flaky** — the
streamed reply sometimes comes back unparseable, in which case the run
logs `[Lamarckian] LLM returned no parseable harness` and Darwinian
carries the round; the final line reports the champion's *actual* origin,
never a Lamarckian win that didn't happen.

## Gate-eval — is the coherence gate actually sound?

The Beast's whole safety argument is that the static validator is a *sound*
coherence oracle: an ERROR means the harness would genuinely fault at
runtime; no error means it runs. That was **asserted, not measured** — the
first thing every reviewer asked about.

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp) measures it. It runs a
labeled corpus of topologies through the validator (predicted verdict) AND
through the engine (ground truth) and cross-checks. Offline, deterministic,
no key — `exit 0` iff every verdict matched execution, so **CI can gate on
soundness**.

```console
$ ./build/cookbook_the_beast_gate_eval
case                     | validator      | runtime | sound?
coherent                 | ok             | CLEAN   | yes
E4-undeclared-write      | ERROR:E4       | FAULT   | yes   # reject ⇒ genuine runtime throw
E3-dangling-edge         | ERROR:E3       | FAULT   | yes
E7-unreachable(warn)     | ok             | CLEAN   | yes   # a warning does NOT reject a correct graph
E10-empty-routes         | ERROR:E10      | not run | yes   # dispatch is UB by design — the gate stops it
runtime cross-check: 4/4 cases where the validator's verdict matched execution.
```

The property under test:

> validator reports an ERROR ⟹ the graph faults when executed;
> validator reports no error ⟹ the graph executes cleanly.

The first line is *soundness* (an error-flagged graph that ran clean would be
a soundness hole); a warning-flagged graph that runs clean shows the gate
does not *over*-reject. E10/E8-class errors are verdict-only — running an
empty route map dereferences `rend()` (UB), which is exactly the fault the
gate exists to prevent, so it is checked but not executed. This is a
demonstration corpus, not exhaustive coverage of every diagnostic — but it
turns "the gate is sound" from a slogan into a measured, CI-enforced 4/4.

## Gate-fuzz — the guarantee and its boundary, at scale

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp) pushes gate_eval from 5
hand-labeled cases to thousands of fuzzed ones — but honestly. The naive move
(fuzz N graphs, print precision 1.0) would be theatre: the **engine re-runs the
validator on compile and throws on any error**, so "validator-error ⟹
engine-faults" is true *by construction*. So the program measures the two things
that are actually informative:

```console
$ ./build/cookbook_the_beast_gate_fuzz 2>/dev/null   # lint → stderr
LAYER 1 — static gate vs engine over 2000 honest-contract mutants:
  gate rejected 1586, gate passed 414;  agreements 2000, DISAGREEMENTS 0
  runtime faults AFTER the gate passed (soundness holes): 0
LAYER 2 — a node that LIES about its effect contract (500 mutants):
  static gate PASSED (blind to the lie): 500/500
  runtime GraphState guard FAULTED (backstop caught it): 500/500
CI gate (Layer 1: 0 disagreements over 2000; Layer 2: runtime backstops 100%): PASS
```

- **Layer 1 — consistency at scale.** Fuzz a coherent seed with random
  structural mutators (dangling edge → E3, undeclared write → E4, orphan writer,
  dropped edge → E7 *warning*, extra valid edge). Over 2000 mutants the compiler
  gate and the engine never disagree. This is not a soundness *discovery* (it's
  partly by construction) — it is a **regression guarantee**: if a future change
  makes the static gate and the runtime diverge, this fails.
- **Layer 2 — the boundary.** The gate trusts each node's declared **effect
  contract**. A node that *lies* — declares `writes:["out"]` but actually writes
  the undeclared `phantom` channel at runtime — sails past the static gate
  (500/500), and the **runtime `GraphState` write-guard** catches every one
  (500/500). That is not a gate bug; it is the designed division of labour.

The result is a *precise* statement of the guarantee, which is more honest than a
suspiciously-perfect confusion matrix: **the static gate is sound relative to
honest contracts, with a runtime backstop for dishonest ones** — Layer 1 and
Layer 2 each CI-enforced.

The formal companion, [`SOUNDNESS.md`](SOUNDNESS.md), *proves* this: a small-step
semantics of super-step execution, the effect lattice `(𝒫(Chan), ⊆)`, the gate as
a well-formedness judgment `⊢ G ok`, and a Progress theorem (a gate-passing graph
under honest contracts never faults) with the honesty hypothesis proved necessary
and the runtime write-guard as its fail-stop backstop. Every premise is checked
against the engine source; `gate_eval`/`gate_fuzz` are the model's fidelity
checks. The two harnesses here are Cor 6.4 and Prop 6.5 of that document, run.

## Baldwin — does memetic beat blind, and does inheritance matter?

The `evolve` variant showed Darwinian mutation + a Lamarckian LLM injection.
The sharper research question every reviewer raised: **is there a task where
blind evolution AND a one-shot solver both stall but the memetic combination
wins — and does *how* you inherit the learned trait change the result the way
the literature predicts?** (Whitley 1994; Hinton & Nowlan 1987.)

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp) is that experiment, run over
real NeoGraph harnesses. The genome is the wiring of an affine pipeline; each
stage is committed to an op **or left plastic (`?`)** for lifetime learning to
resolve. Fitness is the signature of the **assembled harness when run** — and
the startup cross-check proves the fast analytic fitness equals the compiled
engine's on 200 topologies (the same discipline as gate-eval). The landscape is
**deceptive**: a broad decoy hill (0.85) visible everywhere, and a narrow,
**gradient-free** global plateau (1.0) that only *learning* — which searches the
neighborhood spanned by the plastic genes — can find.

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

A note on what the fitness *is*: each genome compiles to a real NeoGraph
topology and the cross-check proves the engine runs 200 of them exactly as the
analytic model predicts — the *substrate* is a genuine, faithfully-executed
harness. The *objective* the GA optimizes is a deceptive Hamming landscape over
the wiring (a controlled testbed for the dynamics), not the raw execution
output. Both facts are stated plainly rather than blurred.

Two findings, held to different standards:

1. **Memetic beats blind (robust — CI-gated).** Blind Darwinian evolution
   assimilates the global only ~25% — the chance floor — because the plateau has
   no committed-space gradient, so selection follows the decoy and is trapped.
   Learning exposes the plateau and assimilates it ~75%. The gate asserts the
   *margin* (means over 24 seeds), not a per-run threshold count, because a
   per-run count is nudged by init luck; the 25%-vs-75% margin is the stable
   signal.
2. **The Baldwinian control (measured — never gated).** Baldwin (don't inherit
   the learned trait) vs Lamarck (write it into the genome): here **74% vs 78%**
   global — Lamarckian is marginally ahead, the *expected* outcome on a landscape
   that is deceptive but not adversarial (write-back's speed outweighs its
   diversity cost). Whitley's **reversal** (Baldwin > Lamarck) needs a
   specifically adversarial landscape; this simple two-peak construction does not
   robustly exhibit it, and that is **reported honestly, not tuned into a fluke.**
   (It is genuinely delicate: an early version with an index-based tie-break
   *appeared* to show the reversal — an artifact. Ties at the selection boundary
   are now broken by a per-seed random draw and **averaged over the sweep**, and
   the ~74-vs-78 ordering is stable across seed bases; the apparent reversal did
   not survive that fix.)

This is the honest shape of the result the reviewers asked for: the robust claim
(learning-guided evolution solves what blind evolution cannot) is measured and
CI-enforced; the delicate claim (non-inheritance beats inheritance) is measured
and reported as-is, with the negative outcome named rather than hidden.

## Baldwin-adv — adversarial landscape + real hill-climb learning

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp) sharpens both sides of
the previous experiment. Learning is now **real local search** (multi-restart
hill-climbing over the plastic genes to a local optimum — the discrete analogue
of a refiner, and the slot the LLM plugs into), and the landscape is genuinely
**adversarial**: a broad decoy hill whose gradient points *away* from a small,
steep global ball. Blind committed-space search is not at the chance floor — it
is actively **deceived** down the decoy gradient.

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **Memetic ≫ blind (robust, CI-gated).** Darwin is deceived onto the decoy
  (~5% global / ~92% decoy); learning finds the global ball a single genome
  cannot (76-98%). This is a *stronger* separation than the plateau — the blind
  baseline is misled, not merely blind. Stable across seed bases (Darwin 2-5%,
  learners 76-98%).
- **Baldwin vs Lamarck: the reversal does NOT reproduce.** Lamarckian
  write-back wins by a stable margin (98% vs 76%). A parameter sweep across the
  whole reachable/unreachable boundary (30+ configs, three sweeps) found **no
  regime** where non-inheritance robustly beats write-back: when the global is
  reachable, write-back's speed dominates; when it is not, both fail with only a
  marginal (~3-4 pt) Baldwin diversity edge. This is the honest empirical answer
  to "does Whitley's Baldwin > Lamarck reversal reproduce over harness
  topologies?" — **no**, in this discrete regime, and the program says so with
  the mechanism named. (Whitley's reversal was established on *continuous*
  multimodal functions with real-valued local search; the discrete topology-GA
  here does not exhibit it.)

## Baldwin-llm — the model IS the learning operator

The mechanical learners above (random guess, hill-climb) were always the *slot*
an LLM refiner plugs into. [`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)
plugs it in, on a task the model can actually reason about: fill the `?` stages
of an arithmetic pipeline so `acc` reaches a target. The **learning operator is
the model** (it chooses ops for the `?` stages); fitness is the assembled
harness *run*. The Baldwin/Lamarck toggle becomes literal:

- **Baldwinian** scores the model's fill but keeps the gene `?` — the model must
  be consulted **again** next generation. Learning is not inherited.
- **Lamarckian** writes the fill into the genome — the `?` becomes committed.
  The acquired trait is **heritable**; the model need not be consulted again.

```console
$ ./build/cookbook_the_beast_baldwin_llm       # oracle learner (default, offline)
  Baldwinian (learner = oracle):
    gen 0: … committed genes 16/24 | learner calls 5
    gen 3: … committed genes 10/24 | learner calls 6      # re-learns every gen
  Lamarckian (learner = oracle):
    gen 0: … committed genes 24/24 | learner calls 5
    gen 3: … committed genes 24/24 | learner calls 0      # banked; no re-learning
total learner invocations: Baldwin 23 vs Lamarck 5  (Lamarck banked its way to fewer)
```

The observable difference is not fitness (both reach the target) but a **genome
economy**: Lamarck banks the learner's work into heredity (genes commit, calls
fall to zero); Baldwin re-learns every generation (genes stay plastic, calls
stay high). Run offline with a deterministic **oracle** learner (default), or
`--llm` with `OPENROUTER_API_KEY` to make **the model** the learner — in which
case those invocations are real API calls, and heredity is literally the
difference between paying the model once and paying it every generation. This is
the concrete meaning of "does the model's fix become heritable?" — shown as a
trace, not asserted. (The `--llm` path needs network; it falls back to the
oracle and logs on any call/parse failure, so the demo always completes.)

## Novelist — a premise in, a light-novel-length `.txt` out

The simplest genuinely-useful writing harness, and the honest form of the
"NovelWriter" idea: give it a premise, get back a whole light-novel-sized
manuscript as plain text. [`the_beast_novelist.cpp`](the_beast_novelist.cpp) is
the cure for **lost-in-the-middle** made concrete — a long story is *not* written
in one giant context. It is a small graph over an **explicit story state**:

    channels:  premise · outline · bible · summary · book · idx · total

so each chapter is generated **fresh against the compact externalized state**
(the outline beat, the story bible, a running summary) instead of re-reading 60k
characters. The model never has to *remember* who a character is across a novel —
it reads the `bible` channel.

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

The graph is `__start__ → planner → writer ⟲`: `planner` turns the premise into
an outline + initial bible; `writer` writes chapter `idx` into `book` and
**updates `summary` and `bible`** so the next iteration stays grounded, then
**self-loops with a `Command` goto** until `idx+1 == total`. Effect contracts are
declared, so the **coherence gate proves the wiring before a word is written** —
every story-state channel is actually consumed, no dangling stage.

Offline (no key) a **deterministic stub** planner/writer runs the *exact same
graph*, so the pipeline — state threading, the goto loop, accumulation, the
`.txt` output — is verifiable without a network; `OPENROUTER_API_KEY` swaps in
the model for the real prose. Honest scope: the gate proves the *plumbing*
(the story-state is wired and threaded), not the *prose* — narrative quality is
the model's job, and continuity beyond structure would be a checker node (the
runtime-backstop pattern), left as the obvious next node to add.

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
