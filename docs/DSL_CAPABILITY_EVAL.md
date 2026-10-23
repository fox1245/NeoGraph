# QuickJS DSL capability and model-synthesis evaluation

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

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
The runner loads `skills/neograph-harness-authoring/SKILL.md` and its
`references/quickjs-authoring.md` into the model's context and records the guidance
digest in the report. `--skill` selects another entrypoint with that companion
reference. The host invokes the actual compiler bridge; the model returns source
and receives diagnostics for bounded repair. The skill grants no runtime authority.

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model z-ai/glm-5.3-flash `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` accepts a comma-separated subset, `--attempts` repeats independent
one-shot trials, and `--repair-attempts` returns the authoritative probe
diagnostic to the model with the rejected full source. Provider/response
failure is kept separate from compile or semantic rejection.
The default model is `z-ai/glm-5.3-flash`; each generation is capped at 4,096
completion tokens and uses the cookbook's ZDR provider-routing setting. The
DeepSeek result below was recorded before the runner loaded the skill.

`--provider anthropic` posts to the Anthropic Messages API with
`ANTHROPIC_API_KEY` instead; the default stays `openrouter`. It sends no
`temperature` unless `--temperature` is given, because some current models reject
the parameter, and it refuses `--reasoning-effort`, an OpenRouter setting. `--dry-run`
prints the exact first-turn and repair requests, with secret header values redacted,
without a key and without a network call (it still runs `--probe` for the native
manifest). A failure to get a usable reply (network, rate limit, overload, rejected
request) is recorded as `failureKind: "provider"` and can be retried with
`--provider-retries`; a reply that is not a usable source envelope is `model_output`.
`summary.usage` in the report sums the input and output tokens of every response,
repairs included.

## Evaluate the instructions separately

A passed source case measures the combined skill, supplied native API reference,
case contract and model configuration. It does not establish that the skill by
itself is sufficient. The runner retains the system prompt, API reference, raw
model content, usage, stop reason and strict-envelope result in its evidence.
The legacy source extractor may recover fenced JSON; strictEnvelope distinguishes
that from compliance with the requested raw JSON envelope.

Use --reasoning-effort only when explicitly selecting a separate comparison
profile. Omission retains the provider default. Hold that value, model, sampling,
output cap, task and compiler fixed within each before/after comparison.

For the chatbot's internal Harness selector, the paired evaluator is:

~~~powershell
bun run scripts/run_harness_skill_ab.ts --before saved-skill/SKILL.md `
  --after skills/neograph-harness-authoring/SKILL.md `
  --provider relace --repeats 2 --output skill-comparison.json
~~~

Both directories must contain references/chat-template-proposals.md. The default
fixtures include a greeting, an ongoing request for independent review and a
recorded review scenario. The expected plan stays outside the model's input.
The evaluator fixes the live chat profile, alternates AB/BA order, retains raw
responses and grades JSON shape separately from the expected plan/confidence.
--provider pins a backend with fallback disabled; without it, routing policy is
shared but the actual provider can differ. Returned provider/model names are
recorded. Cases and repeats are bounded; no repair or silent retry is performed.

This is a prompt-level evaluation, not runtime admission or a statistical claim
about answer quality. Small repeated samples from a targeted case are regression
evidence, not an estimate of general model reliability.

## Current result (2026-10-23)

The pass rate could not tell the arms apart. With the runner's API reference and the
case text, `claude-haiku-5-5` wrote a source that the compiler accepted and the case
validator confirmed on the first attempt in 110 of 110 trials with the previous skill,
with the rewritten skill and with no skill text at all. The measure is at its ceiling,
so it can show neither that a skill helps nor that the rewrite did.

Setup: `claude-haiku-5-5` through the Anthropic Messages API, the 11 cases of
`cases.json`, two runs per arm of 5 independent one-shot trials per case (110 trials
per arm), `--repair-attempts 2`, 4,096 `max_tokens` per generation including
thinking. The API rejects `temperature` for this model, so no sampling parameter was
sent: the model sampled at the provider default with adaptive thinking, and the
attempts are real samples. The protocol, the held-out cases (the first, middle and
last case id: `graph_basics`, `program_durability`, `registry_mediated`) and the
analysis were written to a local file, which is not checked in, before the first
trial. The no-skill control and the second run of each arm were added, as addenda
written after the first run showed the ceiling. The held-out sources and diagnostics
were not read until the revision was final. No request needed a retry and no trial
ended in a provider failure.

To repeat one run:

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --provider anthropic --model claude-haiku-5-5 `
  --attempts 5 --repair-attempts 2 --provider-retries 5 `
  --skill path\to\SKILL.md `
  --output evidence.json
```

The key is read from `ANTHROPIC_API_KEY` in the environment (here the env file sets
it) and appears in no output. "Skill before this change" is the skill as it stood on
the base commit of this change; the no-skill control is an empty entry file beside an
empty `references/quickjs-authoring.md`. A run of 55 trials takes about a minute and
used 0.23 to 0.27 million input tokens with a skill text and 0.06 million without. The
eight runs and one smoke trial together used 1.6 million input and 0.31 million output
tokens. The per-run reports are not checked in.

| Arm | Skill digest | One-shot | After repair | Mean repairs | Fenced reply | First-turn input tokens (mean) |
|---|---|---|---|---|---|---|
| Skill before this change | `b59a0a379690` | 110/110 | 110/110 | 0.00 | 7/110 | 4890 |
| Rewritten skill, as measured | `1ec37bcd85e1` | 110/110 | 110/110 | 0.00 | 3/110 | 4306 |
| Intermediate revision (not kept) | `34509f71ec3c` | 107/110 | 110/110 | 0.03 | 3/110 | 4154 |
| No skill text (control) | `75a11da44c80` | 110/110 | 110/110 | 0.00 | 0/110 | 1087 |

Every case passed 10/10 one-shot in the "before", rewritten and control arms. The
intermediate revision missed one trial each in `program_control_flow`,
`program_durability` and `program_host_capability`, each fixed by the first repair. On
the three held-out cases (30 trials per arm) the "before", rewritten and control arms
scored 30/30 one-shot, the intermediate revision 29/30.

Failure classes, counted over every rejected or irregular reply:

- Rejected source, "before" and rewritten: none in 110 trials each.
- Fenced reply: the JSON envelope came wrapped in a Markdown fence although the
  prompt forbids it. The extractor recovers it, so these trials pass, but they break
  the requested raw envelope. 7/110 before (3/55 and 4/55 in the two runs, 4 of the 7
  in `program_structured_concurrency`), 3/110 with the rewrite, 0/110 without skill
  text.
- Intermediate revision, 3 rejected first turns: a graph named after the case instead
  of the supplied name (2, `graph name must be capability`) and the `channels`
  wrapper read from a response that was a plain object (1, `TypeError: cannot read
  property 'accepted' of undefined`). That revision had dropped the two rules that
  prevent them; the rewrite restores both.

What the data supports: with the runner's API reference and the case text, the model
authors all 11 capability groups in 110/110 first attempts, with or without the skill,
so this measure cannot show that a skill raises the pass rate, and it did not
distinguish the previous skill, the rewrite and no skill. What differed is cost and
reply format. The previous skill text added about 3,800 input tokens per call (4.5
times the control's input); the rewrite adds about 3,200, which lowers the first-turn
input by 12%. Fenced replies were more frequent with the previous text than with none
(7/110 against 0/110, Fisher p = 0.014), but each run alone is not significant (3/55
and 4/55 against 0/55, p = 0.24 and 0.12), the p values are not adjusted for the several
comparisons made, 4 of the 7 are in one case, and what in the previous text caused
them was not tested. The rewrite is not distinguishable from either arm (3/110, p =
0.33 against before and 0.25 against the control). The intermediate revision's 3/110
misses are not significant (p = 0.25) but each has a visible cause in the text. The
rewrite was written after reading the intermediate revision's failures on the eight
non-held-out cases, so its result there is not an independent test; the held-out cases
carry the only untouched comparison, and at 30/30 against 30/30 they cannot separate
the arms.

Corrections after the measurement. The text measured as "Rewritten skill" has digest
`1ec37bcd85e1`; the text this change ships has digest `753b82da7b2d`. Checking the
reference against the compiler after the runs corrected six statements: the claim that
there is no `import`, the scope of "an unknown key is rejected", that `exit` is
required and `__end__` is no edge target, that `yield*` is rejected (only on a
command), the scopes `cancelScope` takes, and the shape of a rejection. The shipped
text is 688 bytes (about 230 tokens) longer, so its first-turn input is about 4,540
tokens, 7% below the previous skill. It was not measured on 110 trials. A smoke run of
it on three cases (`program_durability`, `program_map`, `graph_routing`) with 3
attempts each passed 9/9 one-shot with 9/9 raw envelopes and no provider failure
(40,761 input and 5,212 output tokens). That run is a regression check, not a
measurement.

This is a small-sample capability measure for one model, one provider and one sampling
configuration, on cases whose contract the prompt states explicitly, not a reliability
claim. Showing an effect of a skill needs tasks the unaided model sometimes fails.
The chat-template and Harness MCP surfaces of the skill were not measured.

## Earlier result (DeepSeek)

This earlier run used a different model, an earlier skill and an earlier Program
implementation, and was recorded before the runner loaded the skill. In live runs,
the model produced probe-verified source for all 11 capability groups. Static
HITL/retry required one diagnostic-guided repair. Structured concurrency required
two repairs:
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

The validator checks the exact Core name as well as the command kind and inputs,
and requires `capability` for every nested `callCore`. Model sources that use
`core`, `Core`, or the node name `work` are rejected.

This is proof of capability, not a statistical reliability claim. Repeated trials
with a current model, and with provider failures reported separately, are under
Current result above.

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

In the earlier run the model repeatedly confused ES modules with CommonJS, graph
names with node names, and requested Core identity with generic words such as
`core`. A host should therefore inject exact signatures and admitted identifiers,
retain a fixed module scaffold where possible, and never treat plausible-looking
source as evidence that the requested topology was built.

NeoGraph enforces this boundary in `ProgramSynthesisGateway`: every gateway
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

The manifest also publishes `features`, one boolean flag per authoring feature
(for example `graph.metadata`, `barrier.any_of` or `failure_edges`), a TypeScript
`dts` signature for every method with shared declarations in `dts_prelude`, and a
JSON Schema for every graph-builder argument. Two schema files back it:
[`schemas/javascript-capability-manifest-v1.schema.json`](../schemas/javascript-capability-manifest-v1.schema.json)
describes the manifest itself, and
[`schemas/core-definition-v1.schema.json`](../schemas/core-definition-v1.schema.json)
describes a sealed Core definition, for constrained decoding of generated
definitions. Tests validate the manifest against its schema and the definitions
of the golden identity corpus against the Core schema.
