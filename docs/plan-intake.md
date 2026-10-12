# Plan intake and plan repair

**Languages:** [English](plan-intake.md) | [한국어](plan-intake.ko.md) | [日本語](plan-intake.ja.md) | [简体中文](plan-intake.zh-CN.md)

A planner model that has to produce a task graph writes a closed, typed document. A step plan is a smaller document for the same job: an ordered, flat list of steps. A pure function checks it against the host's template allow-list, reports every problem in a form the model can act on, and lowers a valid plan into a `TaskGraphProposal`. The proposal then takes the existing trusted path unchanged: `TaskGraphProposal::parse`, `TaskGraphFragmentCompiler::compile`, publication and `expand_task_graph`.

A step plan is data and carries no authority. Passing intake means the plan is well formed and inside policy. It does not mean the plan solves the request; a plan can pass every check and still pick the wrong tool or the wrong order.

```
planner text --intake_step_plan--> violations | TaskGraphProposal JSON
TaskGraphProposal JSON --parse--> --compile--> --publish--> expand_task_graph
```

This page covers the step plan, its violations, the schema-2 proposal it lowers to (explicit budgets, literal inputs, attempts), the retry of a task, and the plan repair loop. Changing a graph that is already running is not part of this release.

## The step plan

```json
{
  "schema": "step-plan/v1",
  "steps": [
    {"id": "s1", "use": "web.search/v2",
     "params": {"query": "weather in Seoul tomorrow", "limit": 3},
     "retry": {"max_attempts": 2}},
    {"id": "s2", "use": "text.summarize/v1",
     "params": {"text": {"from_step": "s1", "field": "items/0/snippet"}, "style": "one sentence"}},
    {"id": "s3", "use": "notes.save/v1",
     "params": {"title": "Seoul weather", "body": {"from_step": "s2", "output": "summary", "field": "text"}},
     "after": ["s1"]}
  ]
}
```

The format is closed: every key not listed below is a violation.

| Key | Type | Rules |
|---|---|---|
| `schema` | string | Must equal `step-plan/v1`. |
| `steps` | array | 1 to `StepPlanLimits::max_steps` (default 32) steps. |
| `steps[].id` | string | `[a-z][a-z0-9_]{0,31}`, unique. |
| `steps[].use` | string | A template id from the host allow-list. Nothing else selects behavior. |
| `steps[].params` | object | Optional. Each key is an input name of the template: its `input_fields` entry without the leading `/` (a nested input such as `/filter/key` is written `filter/key`). Each value is a literal or a reference. |
| reference | object | Has `from_step` (the id of an earlier step). `output` is an artifact name; the default is the producer's `out`, else its only artifact. `field` is an output field without the leading `/`; the default is the artifact's only field. |
| `steps[].out` | string | Optional. Names the artifact the step is expected to produce; later references default `output` to it. Must be a declared artifact of the template. |
| `steps[].after` | array of ids | Optional extra ordering edges, to earlier steps only. |
| `steps[].retry` | object | Optional `{"max_attempts": n}`, from 1 to the lower of `StepPlanLimits::max_retry_attempts` (3) and `TaskGraphProposalLimits::max_attempts`. |

A literal is any JSON value: a string, a finite number, a boolean, null, an array or an object. A literal is at most `max_literal_bytes` (4096) of canonical JSON, a plan's literals together at most `max_total_literal_bytes` (32768), nesting at most `max_literal_depth` (6). The host's `TaskGraphProposalLimits::max_literal_bytes` and `max_total_literal_bytes` cannot be exceeded either, and no object inside a literal may have the key `from_step`.

A reference or `after` may only name a step listed earlier. That makes the dependency graph acyclic by construction: the format has no cycle code, and the model has one rule to follow. Independent steps stay parallel, because an edge exists only where a reference or `after` creates one.

### What a step plan cannot say

| Not in the format | Why |
|---|---|
| a provider, model, endpoint, URL, credential or header | Trust and authority are host policy. The template id is the only selector. |
| code, a script, a command, an expression or a template language | A plan is data. A reference selects a field; it never evaluates anything. |
| budgets, timeouts, capability or effect lists, a child binding | The host derives them from the template contract and its policy. |
| a join policy | A task graph joins all-of. |
| conditionals, loops, nested plans | The format stays small. Branching belongs to the Program that runs the plan. |

A key that names an authority (`provider`, `endpoint`, `url`, `api_key`, `token`, `secret`, `credential`, `command`, `code`, `script`, `model`, `budget`, `timeout`) gets the code `STEP_FORBIDDEN_FIELD` and a hint that the host decides it. That list only improves the hint. The closed key set is what keeps authority out of the plan.

## Checking a plan

```cpp
StepPlanIntakeOptions options;
options.source_id          = "planner:turn-1";
options.proposal_limits    = policy.limits;              // the limits the host gives TaskGraphProposal::parse
options.template_allowlist = policy.template_allowlist;  // the same trusted list

const auto result = intake_step_plan(model_text, options);
if (result.ok()) {
    const json& proposal = *result.proposal;             // schema 2, canonical; result.proposal_hash is its identity
} else {
    const auto feedback = format_violations_for_model(result.violations, 2048);
}
```

`intake_step_plan` takes the planner's text or an already parsed `json`. From text it uses the first top-level JSON object, in a fenced block or after prose. `result.envelope` says how it was found: `Strict` when the whole text is the object, `Recovered` otherwise. A key that appears twice in an object is `STEP_DUPLICATE_KEY`; the parsed overload cannot see duplicates.

The result holds the sorted violations, `plan_digest` (the sha256 identity of the canonical plan), the lowered `proposal` and `proposal_hash`, a `source_map` from the lowered proposal's pointers to the plan's steps, and a `report` with the counts `steps`, `edges`, `depth`, `width` and `literal_bytes`. Intake is deterministic: the same document value (key order and whitespace do not matter) and the same options give the same violations and the same proposal bytes. It never throws for the content of a plan; it throws `std::invalid_argument` only for options that are themselves invalid.

`StepPlanIntakeOptions::template_info` is advisory and can only add rejections: `required_params` makes a missing input a violation, and `summary` is text for the planner's prompt. `step_plan_json_schema(templates, limits)` generates a JSON Schema specialised to a catalog (`use` is an enum of the allowed ids, `params` lists each template's input names) for providers that enforce structured output. It is an aid; the validator stays the authority. [`schemas/step-plan-v1.schema.json`](../schemas/step-plan-v1.schema.json) is the catalog-independent shape.

## Violations

Each violation is `{path, code, message, hint, detail}`. `path` is a JSON Pointer into the submitted document (`""` is the document). A message names ids, template names and codes only; a string taken from the plan is cut to 64 bytes and made printable before it is echoed, and a literal value is never echoed, so feedback cannot carry a model-written instruction back into the next prompt. A hint is one imperative sentence that lists at most five valid names.

The list is sorted by path, segment by segment, with two numeric segments compared as integers (`/steps/2` before `/steps/10`), then by code, then by detail. It is cut at `max_violations` (32) and ends with `STEP_VIOLATIONS_TRUNCATED` when anything was left out. When any violation of the structure is found, the plan-level limits (`STEP_LIMIT_EDGES`, `STEP_LIMIT_DEPTH`, `STEP_LIMIT_BUDGET`) are not computed, since they need a sound graph.

| Code | Path | Meaning |
|---|---|---|
| `STEP_NOT_JSON` | `""` | No complete, valid JSON object in the text. |
| `STEP_DUPLICATE_KEY` | the second key | A key repeats in an object. |
| `STEP_TYPE` | the value | Wrong JSON type; `detail.expected` names the right one. |
| `STEP_SCHEMA` | `/schema` | Missing, or not `step-plan/v1`. |
| `STEP_UNKNOWN_FIELD` | the key | A key outside the closed set. |
| `STEP_FORBIDDEN_FIELD` | the key | An authority word; the host decides this. |
| `STEP_EMPTY`, `STEP_LIMIT_STEPS` | `/steps` | No steps, or too many. |
| `STEP_LIMIT_PARAMS`, `STEP_LIMIT_AFTER`, `STEP_LIMIT_DOCUMENT` | the list or `""` | A step has more inputs or `after` entries than allowed, or the document is larger than `max_document_bytes` or nested too deeply. |
| `STEP_ID_REQUIRED`, `STEP_ID_FORMAT`, `STEP_ID_DUPLICATE` | `/steps/i/id` | Id problems. |
| `STEP_USE_REQUIRED`, `STEP_USE_UNKNOWN` | `/steps/i/use` | Missing, or not in the allow-list; the hint names the closest ids. |
| `STEP_PARAM_UNKNOWN`, `STEP_PARAM_MISSING` | `/steps/i/params/key` | The template has no such input, or a required input is absent. |
| `STEP_LITERAL_TOO_LARGE`, `STEP_LITERAL_DEPTH`, `STEP_LITERAL_RESERVED_KEY`, `STEP_LITERAL_NUMBER` | the param | A literal limit, including a host that accepts no literals; the key `from_step` inside a literal; a number that is not finite. |
| `STEP_REF_SYNTAX` | the reference | A malformed reference object. |
| `STEP_REF_UNKNOWN_STEP`, `STEP_REF_FORWARD` | `.../from_step` | Not a step, or not an earlier step (a step cannot name itself). |
| `STEP_REF_OUTPUT_UNKNOWN`, `STEP_REF_FIELD_UNKNOWN`, `STEP_REF_FIELD_AMBIGUOUS`, `STEP_REF_OUT_MISMATCH` | `.../output`, `.../field` or the param | The output or field is not in the producer's contract; several exist and none was named (the hint lists them); `output` differs from the producer's `out`. |
| `STEP_OUT_UNKNOWN` | `/steps/i/out` | Not a declared artifact of the template. |
| `STEP_AFTER_UNKNOWN`, `STEP_AFTER_FORWARD`, `STEP_AFTER_DUPLICATE` | `/steps/i/after/j` | Ordering edge problems. |
| `STEP_RETRY_FIELD`, `STEP_RETRY_RANGE` | `/steps/i/retry/...` | A key other than `max_attempts`, or a value outside the allowed range. |
| `STEP_LIMIT_EDGES`, `STEP_LIMIT_DEPTH`, `STEP_LIMIT_BUDGET` | `/steps` | A plan-level ceiling: dependencies, the longest chain, or the reserved budget summed over the steps (a retry counts once per attempt). |
| `STEP_LOWERING_REJECTED` | the mapped step | `TaskGraphProposal::parse` rejected the lowered form; `detail.proposal_code` has its `P_PROPOSAL_*` code. This is a host policy that differs from the limits intake checks, or a gap in intake; parse stays the authority. |
| `STEP_VIOLATIONS_TRUNCATED` | `""` | The list was cut. |

An example of what the model receives:

```json
[
  {"path": "/steps/2/use", "code": "STEP_USE_UNKNOWN",
   "message": "Template 'web.serach' is not available.",
   "hint": "Use one of: text.summarize/v1, web.search/v2. Closest: web.search/v2.",
   "detail": {"given": "web.serach", "choices": ["text.summarize/v1", "web.search/v2"]}},
  {"path": "/steps/3/params/text/from_step", "code": "STEP_REF_FORWARD",
   "message": "Step 's4' does not come before this step.",
   "hint": "Reference only steps listed earlier in 'steps'; reorder the steps or drop the reference."}
]
```

## Lowering

Lowering is a pure function of the document and the options.

- Tasks follow the order of the steps. A reference becomes an input binding, a literal becomes a schema-2 literal, and `after` and reference producers become `depends_on` (sorted, unique).
- The planner chooses no budget. Each step reserves, in every dimension, the lower of its template's ceiling and the host's per-task ceiling, and all four dimensions are explicit. A retry of `n` attempts reserves it `n` times.
- The proposal is always schema 2, so the host's launch budgets are exact (see below), and is parsed by `TaskGraphProposal::parse` before intake returns it. The returned `proposal` is already in canonical form.

## Proposal schema 2

`TaskGraphProposal` schema 2 is what a step plan lowers to, and what a planner may write directly (see [`schemas/task-graph-proposal-v2.schema.json`](../schemas/task-graph-proposal-v2.schema.json)). A schema-1 document keeps its canonical bytes and identity; a document that uses a schema-2 member while declaring schema 1 is `P_PROPOSAL_SCHEMA_VERSION`.

### Explicit budgets (#412)

A schema-1 task that left a budget dimension at 0 was counted as 0 by the proposal and fragment ceilings, but its child was launched with the link's whole ceiling for that dimension, and an unset `output_bytes` allowed any output. Schema 2 closes the gap:

- A task must state a non-zero amount for every dimension in which its template has a non-zero ceiling (`P_PROPOSAL_BUDGET_ZERO`). A dimension may stay 0 only where the template's ceiling is 0.
- A schema-2 fragment launches each task with exactly its reservation. A dimension of 0 grants nothing, as in `RunBudget`, and the output is held to the stated `output_bytes`.

| | Schema 1 | Schema 2 |
|---|---|---|
| unset budget dimension | accepted; counted as 0; the child gets the link ceiling | `P_PROPOSAL_BUDGET_ZERO` where the template can spend it |
| `output_bytes` of 0 | any output | no output fits |
| canonical bytes and identity of existing documents | unchanged | not applicable |

A host that takes proposals from a planner sets `TaskGraphProposalLimits::min_schema_version` to 2 to refuse schema 1.

### Literal inputs

A task may carry `literals`: `[{"to": {"field": "/question"}, "value": ...}]`. A literal assigns a JSON value to one declared input field of the template, after the bindings, in the child's input. Its target must be an input of the template (`P_PROPOSAL_LITERAL_TARGET`) and receive no other binding or literal (`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`). A literal is data, never evaluated. Literals are sorted by target in the canonical form.

### Attempts

A task may declare `max_attempts` (1 to the host's `TaskGraphProposalLimits::max_attempts`; `P_PROPOSAL_ATTEMPTS`). Every attempt starts a new child with the task's whole budget, so the aggregate reserves the budget once per attempt: `reserved_budget` of the task is its `budget` times `max_attempts`, and both the proposal and the fragment check the sum against the ceilings.

### Host limits

| `TaskGraphProposalLimits` | Default | Meaning |
|---|---|---|
| `min_schema_version` | 1 | Lowest schema accepted. |
| `max_attempts` | 1 | Most attempts a task may declare. 1 means no retries. |
| `max_literal_bytes` | 0 | Bytes of canonical JSON per literal. 0 refuses literals. |
| `max_total_literal_bytes` | 0 | Bytes of canonical JSON of all literals of a proposal. |

An expansion that is rejected reports its first diagnostics in `failure.witness.diagnostics` as `{code, pointer}`.

## Retry of a task

After a chunk of tasks settles, a task whose child failed with a failure the node failure classifier calls retryable (`failure.witness.node_failure.retryable` is true, and its `kind` is not `effect_committed`, `ceiling` or `deadline`; the rule of the plan-level `retry` operation) and that has attempts left is dispatched again with the next attempt number. Cancelled and timed-out tasks, and failures that are not node failures (a missing input, an output outside its contract), are never retried.

- Terminal states stay absorbing, so nothing is written as `Failed` for an attempt that will be retried. The next dispatch writes the task `Active` with the next attempt number and a new attempt identity, and keeps the failure of the previous attempt in the record until the next outcome replaces it. Attempts never decrease.
- Each attempt is a new child with its own run id, derived from the attempt identity, and the whole task budget.
- The retry happens at the next scan of the graph, like any task that becomes ready: a task that depends on the retried one waits, and under the `fail_fast` policy a retryable failure does not cancel the other tasks.
- A crash before the next attempt starts relaunches the same attempt; its child already holds the failure, so the decision is made again the same way.

## Plan repair

A model's first plan is often invalid. Plan repair asks again, with the violations in the prompt, a bounded number of times. It is a pattern over existing mechanisms, not a new plan operation: a Core graph `plan -> intake -> (plan again)` where `intake` is a registered pure node. The run's budgets, the provider call journal and the Core checkpoints give the loop what it needs: bounded attempts, a ceiling on tokens and money, a deadline, and a recorded state, so a resumed run does not ask the model again for an answer it already has.

```json
{
  "schema_version": 1,
  "name": "main",
  "channels": {
    "messages": {"reducer": "overwrite", "initial": [{"role": "user", "content": "..."}]},
    "proposal": {"reducer": "overwrite", "initial": null},
    "repair": {"reducer": "overwrite", "initial": {}}
  },
  "nodes": {
    "plan": {"type": "llm_call"},
    "intake": {"type": "neograph.step_plan_intake/v1", "max_attempts": 3, "feedback": "replace"}
  },
  "edges": [
    {"from": "__start__", "to": "plan"},
    {"from": "plan", "to": "intake"}
  ],
  "conditional_edges": [
    {"from": "intake", "condition": "neograph.step_plan_route/v1",
     "routes": {"ok": "__end__", "retry": "plan", "give_up": "__end__"}}
  ],
  "node_retry_policies": {"plan": {"max_visits": 3}}
}
```

In a Program, the Core definition's `name` must equal the name of the root operation that embeds it (`main` here). `plan` is any node that reads `messages` and appends the model's answer as an assistant message. The Program then expands the accepted proposal from the planner's output with `"proposal_source": {"artifact": "input", "field": "/channels/proposal/value"}`. [`examples/65_program_plan_intake.cpp`](../examples/65_program_plan_intake.cpp) runs this flow offline with a scripted planner: a first answer with three mistakes, a repair turn, and the accepted plan expanded into child Programs.

### The registered executables

`add_step_plan_repair(builder, options)` registers three executables with a `RegistrySnapshotBuilder`. `StepPlanRepairNodeOptions::intake` holds the trusted allow-list and limits; the sealed graph cannot change them, and the executables' identities cover them, so a Program admitted with one allow-list does not run with another.

| Executable | Role |
|---|---|
| `neograph.step_plan_intake/v1` (node) | Reads the last assistant message of `messages`, runs `intake_step_plan`, and writes `proposal` (the lowered proposal, or null), `repair` and, when another attempt follows, `messages`. Configuration: `max_attempts` (1 to `max_attempts_ceiling`, default 3) and `feedback`. |
| `neograph.step_plan_route/v1` (condition) | Returns `ok`, `retry` or `give_up` from `repair.status`. Anything else is `give_up`. |
| `neograph.step_plan_report/v1` (node) | Rewrites `repair.status` as `exhausted` and nothing else, for a graph that ends the loop in a failure exit. |

`feedback: "replace"` (the default) rebuilds `messages` for each attempt as the system messages, the first user message, the rejected plan and the violations, so the context does not grow and an earlier mistake is not re-read. `"append"` keeps the whole conversation and adds the violations. The node writes `messages` whole, so the channel's reducer does not matter. The violations are rendered by `format_violations_for_model`, capped at `max_feedback_bytes` (2048).

### The report

The `repair` channel holds the report, which is the same value `repair_step_plan` returns:

```json
{"status": "exhausted", "attempts": 3, "max_attempts": 3,
 "last_violations": [{"path": "...", "code": "STEP_USE_UNKNOWN", "message": "...", "hint": "...", "detail": {}}],
 "violation_code_histogram": {"STEP_USE_UNKNOWN": 3, "STEP_REF_FORWARD": 1},
 "plan_digests": ["sha256:...", "sha256:...", "sha256:..."],
 "envelope": ["strict", "recovered", "strict"]}
```

`status` is `ok`, `retry` or `exhausted`. `last_violations` is cut at 8 KiB.

### Bounds and the final failure

| Limit | Mechanism |
|---|---|
| attempts | `max_attempts` of the intake node, within the host ceiling; `max_visits` on `plan` is the backstop that fails the run if that logic were wrong |
| tokens and money | the run's budgets; run the planner graph as a child Program (`spawn` joined by `await`) to give planning its own ceiling |
| wall time | the run's `wall_time_ms`, and `attempt_timeout_ms` for each model call |
| model-call retries | `max_retries` of the `plan` node, which is separate from the attempts above |

There are two ways to end after the last attempt:

- Fall back (recommended for user-facing flows). The `give_up` route goes to `__end__`; the report says `exhausted` and the parent Program branches on `/channels/repair/value/status` to answer directly or ask the user. No fragment is published.
- Fail. The `give_up` route goes to a `neograph.step_plan_report/v1` node declared as a failure exit with the code `plan.repair_exhausted`. The run ends `Failed` with that code and its output still carries the report.

A stop by budget is the existing `BudgetExhausted` status and code, so a host can tell "the model could not fix it" from "the money ran out".

### Planning before the run

A host that plans before it starts a run uses `repair_step_plan`:

```cpp
PlanRepairOptions options;
options.intake       = intake_options;
options.max_attempts = 3;
const auto result = repair_step_plan(options, request_text,
    [&](const std::vector<PlannerMessage>& messages) {
        return PlannerReply{call_model(messages), prompt_tokens, completion_tokens};
    });
if (result.ok) start_run(result.proposal);   // the proposal becomes the run input
```

The accepted proposal becomes the run's input, which the run record persists, so a resume never plans again. Accounting is the host's own: the run's budgets do not include planning. Prefer the Core graph when planning must count against the run.

## Compatibility and rollout

The tree is pre-1.0. These changes add members to public structs, so rebuild NeoGraph, every C++ consumer and Python extensions together (see [`docs/ABI_POLICY.md`](ABI_POLICY.md) and the changelog entry).

| Record | Rule |
|---|---|
| schema-1 proposal | Canonical bytes and identity unchanged. |
| schema-2 proposal | New. An older binary rejects it with `P_PROPOSAL_SCHEMA_VERSION`. |
| schema-1 fragment, and stored records of it | Unchanged, and launched as before (an unset dimension gets the link ceiling). |
| schema-2 fragment | New. An older binary refuses to parse it. |

No stored data needs a migration. Upgrade every process that can recover a run before any writer starts producing schema-2 proposals, literals or retries; rolling back after a schema-2 fragment exists means draining those expansions first.

What is not here: changing a graph while it runs, a durable fragment store for SQLite or PostgreSQL, and a catalog view of template costs for the planner.
