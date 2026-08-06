# Current DSL and Program composition limits

_Status: source-level audit of the current working tree — 2026-08-06. This is an implementation inventory, not a product commitment or a replacement for [the v1 architecture](V1_ARCHITECTURE.md)._

---

## Scope and reading order

This record separates four often-conflated surfaces:

1. the bounded `graph::Elaborator` convenience language;
2. strict Core topology JSON and registered Core node behavior;
3. the typed Program operation tree; and
4. Harness `mode: "dsl"`, which currently translates only the first two.

Code is the source of truth for this snapshot. The superseded [Programmable
Harness DSL study](PROGRAMMABLE_HARNESS_DSL_DESIGN.md) is useful history but
must not be used to infer current runtime behavior.

## Boundary at a glance

```mermaid
flowchart LR
    accTitle: DSL and Program boundary
    accDescr: The bounded Core DSL elaborates into strict topology JSON and Harness wraps that topology in one call_core Program root, while Program JSON is compiled through a separate path.

    core_dsl[Core topology DSL] --> elaborator[Elaborator]
    elaborator --> strict_core[Strict Core JSON]
    strict_core --> harness_translate[Harness dsl translation]
    harness_translate --> call_core_root[Program call_core root]
    program_source[Program JSON] --> program_compiler[ProgramCompiler]
    program_compiler --> program_plan[Typed Program plan]

    classDef core_surface fill:#dbeafe,stroke:#2563eb,stroke-width:2px,color:#1e3a5f
    classDef program_surface fill:#ede9fe,stroke:#7c3aed,stroke-width:2px,color:#3b0764
    classDef bridge_surface fill:#fef9c3,stroke:#ca8a04,stroke-width:2px,color:#713f12

    class core_dsl,elaborator,strict_core core_surface
    class program_source,program_compiler,program_plan program_surface
    class harness_translate,call_core_root bridge_surface
```

The important boundary is directional: Core DSL can produce strict Core JSON,
but it does not produce a Program operation tree. Harness currently preserves
that boundary by wrapping the elaborated graph in one `call_core` root.

## What each surface can express today

| Surface | Accepted constructs | Explicit boundary |
| --- | --- | --- |
| Core topology DSL | `vars`, whole-value and scalar interpolation, non-recursive `templates` + `use`, local node-prefix renaming, global channel merge, and boolean `when` | Elaboration removes all DSL keys and returns strict Core topology JSON; it never emits Program operations. [Elaborator contract](../include/neograph/graph/elaborator.h#L5-L87) |
| Strict Core JSON | Static channels, nodes, edges, conditional edges, barriers, static interrupts, retry policy, and registered node configuration | Topology is data. Runtime-determined routing and pauses come from registered node code, not a JSON expression evaluator. [Topology model](../include/neograph/graph/compiler.h#L73-L93) |
| Program JSON (compiler) | `call_core`, `sequence`, `branch`, bounded `loop`/`retry`, `parallel`, `race`, `quorum`, `map`, `spawn`, `await`, `emit`, `checkpoint`, `cancel`, and `return` | This is a separate typed operation grammar with its own compiler and admission path. The published Program JSON Schema still permits only `call_core`; see P-SCHEMA-003. [Operation list](../src/program/compiler.cpp#L360-L452) |
| Harness `mode: "dsl"` | Bounded Core DSL plus sealed Harness worker enrichment | It elaborates to Core and constructs a Program document with one `call_core` root; it does not accept a Program tree from the request. [Translation path](../src/mcp/harness_program_translator.cpp#L743-L820) |

## Verified practical limits

### H-DSL-001: Harness DSL cannot author Program composition

`HarnessRequestTranslator::translate()` sends `harness.definition` through
`Elaborator` only for `mode == "dsl"`, then emits a Program document whose root
is exactly `{"op":"call_core", ...}`. The request schema admits only
`preset`, `dsl`, and `core` modes.[^harness-modes]

**Effect:** A Harness caller cannot write a top-level `sequence`, `branch`,
`parallel`, `loop`, `spawn`, or `await` plan in the DSL request. Template use
can duplicate static Core topology, but it cannot express Program control
composition.

**Safe extension direction:** add an explicit, separately admitted Program
source mode. Do not reinterpret existing `dsl` as Program JSON; callers rely on
its total elaboration-to-Core contract.

### P-CORE-002: One Program document seals one directly callable Core graph

The Program root must contain one embedded strict Core `definition` whose name
matches the root name. Every nested `call_core` is lowered to that same root
name; a different `core` reference is rejected.[^program-single-core]

**Effect:** A Program can repeat or place control around one pinned Core graph,
but cannot directly compose Core graph `A` followed by Core graph `B` in one
source document. Separately admitted child Programs are the available boundary
for a distinct graph today.

### P-SCHEMA-003: Published Program schema lags the compiler

`ProgramCompiler` accepts the operation tree above, but
`program-document-v1.schema.json` describes one `call_core` root and fixes
`max_program_operations` to exactly one.[^program-schema]

**Effect:** Schema-driven callers cannot author the compiler/runtime control
vocabulary. Those operations are implementation-supported through
`ProgramSource`, but are not yet a schema-supported external JSON contract.

**Safe extension direction:** evolve the schema and its conformance fixtures as
one recursive operation grammar before exposing Program composition through an
adapter. Do not claim that a schema validates a control operation it rejects.

### P-DATA-003: Program dataflow is intentionally small

Program conditions support JSON-pointer equality, inequality, existence, and
`all`/`any`/`not` composition. There are no numeric comparisons, regexes,
user-defined predicates, Program-level variables, interpolation, or expression
evaluation.[^program-condition]

`emit` and `return` carry authored JSON values; they do not evaluate a template
against the current state.[^program-literal-values] `map` passes each literal
item as the body state, but does not introduce a named binding or result
projection language.[^program-map]

**Effect:** Any nontrivial routing predicate or output construction remains a
registered Core node responsibility. That keeps Program admission finite and
inspectable, but makes some ordinary orchestration shapes verbose or
unexpressible without a purpose-built node.

### P-RUNTIME-004: Operation names carry constrained semantics

| Operation | Observed behavior | Consequence |
| --- | --- | --- |
| `race` | The compiler rejects every arity other than two with `P_PLAN_RACE_ARITY`; the runtime implements a deterministic binary race | Three-way races receive a stable grammar diagnostic during compilation. N-ary race remains unsupported. [Compiler](../src/program/compiler.cpp#L680-L702), [runtime](../src/program/run_attempt.cpp#L931-L936) |
| `quorum` | Branches execute in declaration order until enough succeed or success becomes impossible | It is not concurrent fan-out and cannot cancel already-running siblings because it launches none concurrently. [Runtime](../src/program/run_attempt.cpp#L1055-L1080) |
| `map` | Items execute in declaration order | It is ordered serial mapping, not bounded parallel mapping. [Runtime](../src/program/run_attempt.cpp#L1083-L1103) |
| `parallel` | All branches launch concurrently, subject to run-wide `max_concurrency` checks | Parallelism is real but is capped by the admitted budget and nested Core dispatches share that cap. [Runtime](../src/program/run_attempt.cpp#L833-L928) |

### P-CHILD-005: Child control is durable but deliberately narrow

`spawn` accepts a separately admitted `child_binding` and explicitly rejects an
inline body. The compiler also requires it to be the direct body of `await`, so
a Program cannot retain or pass a named child handle.[^program-spawn] The runtime
then calls `launch_child()` and returns the child run/version identifiers to that
`await`. `await` accepts an inline `body` plus an optional timeout; it recognizes
a spawned child body, but has no public handle ID or event-selector form.[^program-await]
`cancel` currently admits only the whole-run scope.[^program-cancel]

**Effect:** This is not a general remote-procedure or dynamic-agent language.
Child identity, authority, and budget remain explicit, but callers cannot
selectively cancel a branch/child or await an arbitrary named event through the
current Program JSON grammar.

### C-CORE-006: Dynamic topology behavior is node implementation behavior

A Core node may return `Send` with a runtime-selected target and input, return
`Command` to override the next node and update channels, or throw
`NodeInterrupt` to request a checkpointed pause.[^core-send-command][^core-interrupt]

**Effect:** The static Core DSL can declare all static graph shapes, but cannot
encode arbitrary data-dependent `Send`, `Command`, or dynamic interrupt logic.
Those behaviors require a registered node type whose implementation is reviewed
and admitted with the appropriate capability/effect closure. This is a
security-preserving boundary, not a missing interpolation feature.

### S-SUBGRAPH-007: File-backed Core subgraphs require profile-specific review

The default Core `subgraph` node supports either an inline inner topology or a
string file path, which it opens with `std::ifstream` before compiling the inner
graph.[^subgraph-file] Harness's generic transport-field rejection blocks keys
such as `url`, `command`, and credentials, but it does not reject a `definition`
string or a file path.[^harness-transport]

**Effect:** This is conditional on the host exposing a `subgraph` node manifest:
current Harness snapshots only expose configured node manifests. If an untrusted
Harness profile exposes the Core `subgraph` node, it needs an explicit path
policy or an inline-only wrapper. Do not treat Core subgraph file loading as a
Program child-program feature; it does not carry Program child identity,
admission, or lifecycle semantics.

## Classification and next decisions

| ID | Classification | Why it matters | Minimum safe next action |
| H-DSL-001 | Product gap | Harness users cannot reach the Program composition layer | Specify a new Program request mode and its source-map, policy, and backward-compatibility contract |
| P-CORE-002 | Product gap | One Program cannot directly compose independently pinned Core graphs | Decide whether multi-Core modules are needed before extending `call_core` references |
| P-SCHEMA-003 | Public-contract gap | Published JSON Schema rejects the compiler's operation vocabulary | Evolve recursive schema, schema tests, and adapter admission together before claiming external Program JSON support |
| P-DATA-003 | Deliberate restriction with usability cost | Prevents ambient computation in Program JSON | Add only typed, bounded data transforms justified by a concrete workload; keep arbitrary predicates in registered nodes |
| P-RUNTIME-004 | Deliberate binary race plus semantic limits | `map`/`quorum` are more serial than their names suggest | Keep binary race validation; decide whether the other two names or their runtime semantics should change |
| P-CHILD-005 | Deliberate authority boundary with missing controls | Child admission is safe, but control operations are narrow | Add scoped cancellation and explicit handle/event await only with durable lineage and recovery semantics |
| C-CORE-006 | Deliberate Core boundary | Keeps dynamic control in reviewed executable identities | Preserve this boundary; do not add raw code or arbitrary callbacks to DSL input |
| S-SUBGRAPH-007 | Conditional security risk | A host that exposes the default file-backed node may admit caller-selected local paths | Add an untrusted-profile rejection test before exposing that manifest |

## Required regression cases before expanding the surface

| Scenario | Observable contract |
| Harness `dsl` containing a Program operation | Fails with a source-level diagnostic; a future explicit Program mode succeeds without changing `dsl` meaning |
| Published Program JSON Schema for `sequence` | Rejects it until the schema is upgraded alongside the compiler/admission contract |
| Three-branch `race` | Fails during compilation with `P_PLAN_RACE_ARITY` until n-ary behavior is implemented |
| `map` and `quorum` | Tests state whether execution is serial or concurrent and enforce that documented choice |
| Multiple Core graph references | A Program either rejects a second Core identity during compilation or resolves a sealed, admitted module reference with an exact source map |
| Child cancellation and await | A child/branch scope cannot be silently accepted as run scope; future support must persist and recover the exact target handle |
| Untrusted subgraph definition string | The relevant admission profile rejects file-backed definitions or resolves only an approved virtual source |

## Evidence

[^harness-modes]: [`src/mcp/harness_program_translator.cpp`](../src/mcp/harness_program_translator.cpp#L687-L690), [`src/mcp/harness_program_translator.cpp`](../src/mcp/harness_program_translator.cpp#L743-L820)
[^program-schema]: [`schemas/program-document-v1.schema.json`](../schemas/program-document-v1.schema.json#L1-L66), [`schemas/program-document-v1.schema.json`](../schemas/program-document-v1.schema.json#L272-L280)
[^program-single-core]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L360-L452), [`src/program/compiler.cpp`](../src/program/compiler.cpp#L587-L600)
[^program-condition]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L455-L512)
[^program-literal-values]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L798-L806), [`src/program/run_attempt.cpp`](../src/program/run_attempt.cpp#L1249-L1261)
[^program-map]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L727-L743), [`src/program/run_attempt.cpp`](../src/program/run_attempt.cpp#L1083-L1103)
[^program-spawn]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L744-L761), [`src/program/compiler.cpp`](../src/program/compiler.cpp#L817-L857), [`src/program/run_attempt.cpp`](../src/program/run_attempt.cpp#L1106-L1125)
[^program-await]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L762-L769), [`src/program/run_attempt.cpp`](../src/program/run_attempt.cpp#L1142-L1221)
[^program-cancel]: [`src/program/compiler.cpp`](../src/program/compiler.cpp#L779-L797)
[^core-send-command]: [`include/neograph/graph/types.h`](../include/neograph/graph/types.h#L247-L292)
[^core-interrupt]: [`include/neograph/graph/types.h`](../include/neograph/graph/types.h#L138-L200)
[^subgraph-file]: [`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp#L245-L290)
[^harness-transport]: [`src/mcp/harness_program_translator.cpp`](../src/mcp/harness_program_translator.cpp#L485-L502), [`src/mcp/harness_program_translator.cpp`](../src/mcp/harness_program_translator.cpp#L937-L972)
