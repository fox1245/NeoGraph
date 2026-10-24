# Program Coding Swarm (`program-coding-swarm`)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

An experimental proof of concept showing how the NeoGraph Program API runs a multi-agent coding workflow as an explicit, budget-bounded hierarchy of Programs rather than a single linear write-test-review loop. Self-synthesized or replaced topologies are not claimed to be safe or reliable on their own; safety in this example comes from host-owned compilation, admission, budget ceilings, and a deterministic verification gate.

The coding targets in this cookbook (`slugify`, `escape_html`, and `ini_kv`) are a toy target: small deterministic text-transform pipelines checked against test vectors by a host-owned verification node.

## What the example demonstrates

1. **Parallel work (`root -> children`)**: A root Program runs an `orchestrator` node (backed by a model) to plan the module split, spawns one child Harness per module in parallel (`ng.spawn` / `ng.await`), joins their outputs, and runs a final integration pass. Pass `--concurrency <n>` or `--sequential` (`--concurrency 1`) to run the same topology and inputs with bounded or sequential fan-out and compare wall-clock overlap in the report.
2. **Resource allocation per module**: Each module carries a criticality (`standard` or `critical`). The critical module (`escape_html`) receives a larger non-renewable token and monetary budget carved from the root budget, a higher attempt limit, a stronger implementer role (`implementer-strong`), and a grandchild second-opinion Harness. Remaining budgets per node are printed at the end of the run and recorded in the report.
3. **Role-based model mapping**: Nodes reference model roles (`orchestrator`, `implementer-cheap`, `implementer-strong`, `reviewer`, `second-opinion`) rather than hard-coded model identifiers or prices. Role bindings, provider endpoints, and `SchemaProvider` model catalogue entries live in [`demo-models.json`](demo-models.json) (offline default) and [`live-models.json`](live-models.json) (live measurement).
4. **Host-owned precondition gate**: A module child may publish its result only when the host-owned `verify` node passes all deterministic test cases **and** the reviewer's verdict is read and equals `"accept"` (plus the grandchild second-opinion verdict for critical modules). In the offline default run, `slugify` attempt 1 fails `verify` and `escape_html` attempt 1 is rejected by `review`; the gate blocks both attempts from publishing and triggers bounded retries.
5. **Supervision in depth (`root -> child -> grandchild`)**: For the critical module (`escape_html`), the child Harness spawns a grandchild Harness (`second-opinion:escape_html`) after primary review passes. The program prints the three-level hierarchy with each level's role, model role, granted and remaining budgets, and outcome.
6. **Topology as data on SQLite (mid-run replacement and rollback)**: Every compiled `ProgramBundle`, admitted `ProgramVersion`, run lineage, and generation is stored in SQLite (`SQLiteProgramStore`, `SQLiteProgramTransitionStore`, `SqliteCheckpointStore`). Mid-run, after `slugify` attempt 1 is blocked, the host replaces `slugify`'s child topology at a held `ng.checkpoint` with a stricter admitted variant (generation 2), carrying over the remaining non-renewable budget and keeping children intact. After attempt 2 completes, the host rolls back to the earlier admitted version (generation 3) and prints the 3-generation history read back from SQLite.
7. **Central vs. delegated synthesis (`--synthesis=central|delegated`, default `central`)**:
   - `central`: The host compiles, links (`link_module_child`), and admits the complete tree (`root`, all `child` modules, and the critical module's `grandchild`) before execution starts. Children may only pass parameters (`focus`, candidate data) to the pre-admitted grandchild template.
   - `delegated`: The host pre-admits only the root and child modules. At run time, the critical child (`escape_html`) pauses at a checkpoint and proposes its grandchild JavaScript topology from the reviewed template library; the host validates that an over-budget proposal is refused by `authorize_program_child_synthesis` (`admissions_refused`) and then synthesizes and admits the grandchild within the child's remaining authority and budget via `ProgramSynthesisGateway` and `ProgramRuntime::prepare_child_synthesis`.
   - Each run executes both modes on the same task, models, and budgets and records a side-by-side comparison in the output and JSON report.

## Offline mode (default, run by CTest)

Build the target and run it without arguments (no API keys and no network calls):

```bash
cmake --build build --target cookbook_program_coding_swarm
./build/cookbook_program_coding_swarm
ctest --test-dir build -R cookbook.program_coding_swarm --output-on-failure
```

## Live mode and measurement report

Live mode uses real providers configured in [`live-models.json`](live-models.json):

- `orchestrator` (root): Anthropic Messages API (`claude-sonnet-5-5`), reading `ANTHROPIC_API_KEY`.
- `implementer`, `implementer-cheap`, `implementer-strong`, `reviewer` (children): OpenRouter Chat Completions (`deepseek/deepseek-v4.1-flash-1017`), reading `OPENROUTER_API_KEY`.
- `second-opinion` (grandchild): OpenRouter Chat Completions (`inception/mercury-3`), reading `OPENROUTER_API_KEY`.

Copy [`.env.example`](.env.example) to `.env` (or export the variables in your shell) and run:

```bash
export ANTHROPIC_API_KEY="..."
export OPENROUTER_API_KEY="..."

./build/cookbook_program_coding_swarm \
  --live \
  --models examples/cookbook/program-coding-swarm/live-models.json \
  --synthesis=central \
  --runs 3 \
  --max-cost-usd 0.50 \
  --max-cost-usd-other 0.25 \
  --report swarm-report.json
```

### CLI flags

- `--mock` (default) / `--live`: Select deterministic offline scripted providers or live providers.
- `--models <file.json>`: Path to the role and model catalogue configuration (`demo-models.json` or `live-models.json`).
- `--synthesis=central|delegated`: Select primary synthesis mode (`central` by default); the peer mode is also executed and compared under `synthesis_comparison`.
- `--concurrency <n>` / `--sequential`: Cap how many module children run in parallel (`--sequential` sets concurrency to `1`).
- `--runs <k>`: Repeat the task `k` times and aggregate median parallel/sequential wall times, median run cost, and total gate blocks.
- `--max-cost-usd <usd>`: Cap the total run monetary budget enforced through `RunBudget::monetary_microunits` and `bind_priced_provider_call`.
- `--max-cost-usd-other <usd>`: Separate monetary cap for non-Anthropic providers (OpenRouter child and grandchild calls).
- `--report <file.json>`: Write the machine-readable measurement report as JSON.

### OpenRouter routing and Zero Data Retention (`zdr`)

In [`live-models.json`](live-models.json), OpenRouter roles set:

```json
"provider": {
  "sort": "price",
  "allow_fallbacks": true,
  "data_collection": "allow"
}
```

This allows OpenRouter to pick low-cost endpoints with fallback enabled and without a `zdr` filter (which drops many low-cost endpoints). Each call is bounded before dispatch by `bind_priced_provider_call` using the model's `ceiling_input` and `ceiling_output` rates in `live-models.json`, which pins `provider.max_price` on the outbound OpenRouter request. Note that `sp::OpenRouterRouting` in the current C++ SDK forwards `zdr`, `allow_fallbacks`, `require_parameters`, `data_collection`, `only`, `order`, `ignore`, and `max_price`; `"sort"` in `live-models.json` is recorded in the report's `openrouter_routing` metadata.

To require Zero Data Retention or pin specific upstream providers, edit the role's `"provider"` block in `live-models.json`:

```json
"provider": {
  "zdr": true,
  "allow_fallbacks": false,
  "data_collection": "deny",
  "only": ["deepinfra", "together"]
}
```

### Reading `--report <file.json>`

- `primary_run.total_wall_time_ms` and `primary_run.sum_children_wall_time_ms`: Compare total wall time against the sum of child durations; `children_intervals_overlapped` confirms concurrent child execution from overlapping `[wall_clock_start_ms, wall_clock_end_ms]` intervals.
- `primary_run.cost_by_module` and `primary_run.cost_by_model_role`: Show cost distribution across modules and model roles (`implementer-cheap` vs. `implementer-strong`, `reviewer`, `second-opinion`, `orchestrator`).
- `primary_run.nodes`: Per-node hierarchy (`root`, `child`, `grandchild`), resolved provider/model, attempts, tokens in/out, cost (`cost_source`: `"provider_reported"` when OpenRouter returns usage cost, or `"catalogue"`), wall-clock start/end, outcome, and `budget_granted` vs. `budget_remaining`.
- `primary_run.calls`: Per-call telemetry including `provider_routed_to` (which upstream provider OpenRouter routed the call to).
- `primary_run.gate_decisions`: Every attempt evaluated by the host gate, showing `verify_passed`, `reviewer_verdict_read`, `reviewer_verdict`, `second_opinion_verdict`, `published`, and `block_reason`.
- `primary_run.generation_transitions`: The SQLite generation history (`initial` -> `replace_stricter` -> `rollback_initial`) with token and monetary budgets before and after each transition.
- `synthesis_comparison.central` and `synthesis_comparison.delegated`: Side-by-side comparison of central vs. delegated synthesis on the same task, models, and budgets.
- `aggregates`: Multi-run medians (`median_wall_time_ms_parallel`, `median_wall_time_ms_sequential`, `median_cost_usd_per_run`, `total_gate_blocks_across_runs`).

## Measured live run (2026-10-24)

- **Setup**: `--live --synthesis=central --runs 3`, caps `--max-cost-usd 2.00 --max-cost-usd-other 0.50`; models from [`live-models.json`](live-models.json) (root orchestrator on the Anthropic Messages API, children and the grandchild through OpenRouter with `{"sort":"price","allow_fallbacks":true,"data_collection":"allow"}`). Linux x86-64.
- **All 3 runs**: every module published on its first attempt (3/3 modules per run, 0 gate blocks, 0 refused admissions in central mode); the run verification printed `PASS`.
- **Wall time (median of 3)**: 23.6 s with the module children in parallel, 32.6 s for the same work run sequentially (about 1.38x). The per-run parallel wall times were 23.6 s, 21.6 s and 28.8 s. Child intervals overlapped in every run.
- **Cost per run (from the run's monetary records)**: 0.0291 to 0.0299 USD, median 0.0298 USD; of that 0.0014 to 0.0020 USD for the OpenRouter children and grandchild, and about 0.028 USD for the root orchestrator. Tokens per run: about 2.7k in and 3.9k to 4.7k out.
- **Mid-run topology replacement and rollback**: ran as in the offline run: 3 generations read back from SQLite, non-renewable budgets carried over (tokens 20000 -> 20000), children preserved.
- **Delegated mode (executed in the same run for comparison)**: 3 modules published, 1 gate block and 1 refused admission (the over-budget grandchild proposal, refused by design), 32.6 s wall.
- **What this does not show**: with these live models the gate never had to block in central mode, so the gate-block-and-retry path is shown by the offline run, not by this live run; 3 runs are not a reliability measurement.
