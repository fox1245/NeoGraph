# QuickJS DSL capability and model-synthesis evaluation

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

Status: implemented, deterministic conformance gated; live model evaluation opt-in
Observed: 2026-08-22

## Question

NeoGraph must distinguish two claims:

1. the admitted QuickJS DSL can represent a capability; and
2. an LLM can synthesize source that actually uses that capability correctly.

The second claim is not implied by the first. A source only passes this
evaluation when the real `ProgramCompiler` accepts it and a case-specific
semantic validator confirms the lowered Core IR or sealed JavaScript command
tree. Source-text keyword matching is not sufficient.

## Capability manifest

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json)
is the machine-readable evaluation inventory. It currently covers:

- graph/channel/node/entry/edge/exit and ordinary JavaScript construction;
- registered conditional routing;
- static fan-out, fan-in, and barriers;
- static HITL interrupts and graph retry policy;
- registry-mediated dynamic `Send` and `NodeInterrupt` behavior;
- `callCore` with ordinary JavaScript branch/loop control;
- JavaScript map lowered to bounded `ng.all`;
- `all`, `parallel`, generic `join`, `race`, and `quorum`;
- child `spawn` nested inside `await`;
- `emit`, `checkpoint`, and `cancelScope`; and
- an admitted native `hostCapability` import slot.

Each checked-in JavaScript fixture is compiled and semantically validated by
`program_dsl_capability_probe`. These are deterministic CTest tests and require
no model or network.

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

The focused graph-builder regression test also proves repeated mutator calls
accumulate correctly and that barrier, interrupt, and retry declarations
survive lowering:

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## Live model evaluation

The opt-in runner asks for source using natural-language semantics plus the
public API signatures. It does not give the model the checked-in answer. Every
response is sent to the same native probe used by deterministic CTest.

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model deepseek/deepseek-v4-flash-0731 `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` accepts a comma-separated subset, `--attempts` repeats independent
one-shot trials, and `--repair-attempts` returns the authoritative probe
diagnostic to the model with the rejected full source. Provider/response
failure is kept separate from compile or semantic rejection.

## Observed DeepSeek result

Across the initial run and exact-identifier re-evaluations, the model produced
probe-verified source for all 11 capability groups. Static HITL/retry required
one diagnostic-guided repair. Structured concurrency required two repairs:
first to restore ES-module exports, then to replace the wrong Core binding with
the exact admitted name. Control flow passed after one exact-binding repair.
Map passed one-shot after the host supplied the native API manifest and stated
the admitted Core identifier unambiguously.

| Capability case | Model evidence | Important observation |
|---|---|---|
| `graph_basics` | Passed | Loop-built nodes lowered correctly |
| `graph_routing` | Passed on a repeated trial | Earlier outputs used forbidden CommonJS/`require` |
| `graph_fanout_barrier` | Passed | Fan-out edges and barrier membership were exact |
| `graph_hitl_retry` | Passed after one repair | Initial output used the wrong node name |
| `registry_mediated` | Passed | Model correctly referenced host-admitted dynamic nodes |
| `program_control_flow` | Passed after one repair | Earlier trials repeatedly called `core`/`Core`, not admitted Core `capability` |
| `program_map` | Passed after exact identifier injection | Earlier trials used CommonJS, `yield*`, and the wrong Core name |
| `program_structured_concurrency` | Passed after two repairs | Exact nested commands and Core bindings were verified |
| `program_spawn_await` | Passed | `Await(Spawn(...))` and timeout were structurally verified |
| `program_durability` | Passed | Emit, checkpoint, and cancellation commands were exact |
| `program_host_capability` | Passed | Import slot and canonical input matched |

The initial validator produced false positives for `callCore` cases because it
checked command kind and inputs but not the exact Core name. The validator was
strengthened to require `capability` for every nested `callCore`. Previously
accepted model sources using `core`, `Core`, or node name `work` are therefore
correctly rejected now.

This is proof of capability, not a statistical reliability claim. Per-case
one-shot and repair success rates still require repeated trials with provider
failures reported separately.

## Consequence for generated Programs

Raw one-shot source generation is not a sufficient product guarantee. The
minimum safe synthesis path is:

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

The model repeatedly confused ES modules with CommonJS, graph names with node
names, and requested Core identity with generic words such as `core`. NeoCode
should therefore inject exact signatures and admitted identifiers, retain a
fixed module scaffold where possible, and never treat plausible-looking source
as evidence that the requested topology was built.

NeoGraph now enforces this boundary in `ProgramSynthesisGateway`: every gateway
configuration must provide a host-owned semantic validator. A successful
validation produces a content-addressed receipt before Catalog admission; a
rejected decision throws `ProgramSynthesisValidationError`, retains the exact
evidence, and never calls the admission resolver. The already consumed dynamic
compile reservation remains consumed.

`javascript_authoring_capability_manifest()` exposes the installed graph-builder
and command vocabulary, exact signatures, classifications, limits, and profile
constraints as machine-readable data. Conformance tests compare that manifest
with the actual properties installed in both QuickJS contexts so API drift fails
the test suite.
