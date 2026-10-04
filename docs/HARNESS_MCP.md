# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness compiles a bounded multi-worker workflow before it runs. The
stable MCP surface stays at six tools:

- `neograph_schema` discovers the installed request contract and presets.
- `neograph_compile` compiles and validates without executing.
- `neograph_start` starts a retained artifact or an inline request.
- `neograph_get` polls compact status or dereferences a result artifact URI.
- `neograph_resume` validates and submits the exact pending host result.
- `neograph_cancel` cooperatively cancels a queued, running, or waiting workflow.

The shipped presets are `fanout_judge`, `pr_review_panel`, `bug_triage`, and
`research_synthesis`. Presets produce ordinary strict-Core graph artifacts;
JavaScript requests preserve their own `ProgramSource` envelope and source map.

### JavaScript authoring boundary

`harness.mode` accepts `preset` or `javascript` for new publication. JavaScript
requests carry source text in `harness.source` and may pin `harness.source_id`:

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

The translator wraps that text in the canonical `ProgramSource` JavaScript
envelope (language `javascript`, QuickJS engine, frozen host API, imports, and
source map) and sends it through `ProgramCompiler`, `ProgramCatalog`, and
`ProgramRuntime`. `define()` constructs one graph through the sealed `ng`
binding; an optional generator `main()` owns ordinary control flow and yields
the existing typed Program commands. JavaScript does not dispatch Core nodes,
select providers/tools, or bypass admission, budgets, journals, or replay.

The evaluated module also selects the result contract. A source that exports
only `define()` retains the Core root contract, including the
`channels.final_result.value` wrapper. A source that exports runtime
`main(input)` instead advertises and validates its terminal return directly
against the Harness result schema.

#### Control-flow migration example

Keep `define()` compile-time and every runtime effect behind a yielded typed
command. This complete request gives the generator three operations—the
`ng.all` join plus its two Core calls—and two-way parallelism. Its worker node
exactly repeats the configuration sealed from the request and its terminal
return has the Harness result shape:

```javascript
const source = String.raw`
function workerConfig() {
  return {
    type: "neograph_harness_worker",
    worker_id: "reviewer",
    instructions: "Return structured findings",
    tool_ids: [],
    tool_descriptions: {},
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_ms: 30000,
    max_output_tokens: 512,
    input_token_ceiling: 16384,
    max_retries: 1,
    max_provider_tool_rounds: 8,
    evidence_required: [],
    read_only: true
  };
}

export function define() {
  const graph = ng.graph("review");
  graph.channel("task", {reducer: "overwrite", initial: {}});
  graph.channel("worker_results", {reducer: "append", initial: []});
  graph.channel("final_result", {reducer: "overwrite", initial: null});
  graph.node("reviewer", workerConfig());
  graph.node("judge", {
    type: "neograph_harness_judge",
    barrier: {wait_for: ["reviewer"]}
  });
  graph.edge("__start__", "reviewer");
  graph.edge("reviewer", "judge");
  graph.edge("judge", "__end__");
  return graph;
}

export function* main(input) {
  const results = yield ng.all([
    ng.callCore("review", {task: input.task}, "review:first"),
    ng.callCore("review", {task: input.task}, "review:second")
  ], {max_in_flight: 2}, "review:all");
  return results[0].channels.final_result.value;
}
`;

const request = {
  task: {
    objective: "Review the change",
    acceptance: ["Return structured, evidence-backed findings"]
  },
  harness: {mode: "javascript", source_id: "review:main.js", source},
  workers: [{
    id: "reviewer",
    instructions: "Return structured findings",
    tools: [],
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  }],
  tool_catalog: [],
  budgets: {
    max_steps: 40,
    timeout_seconds: 60,
    max_parallel_workers: 2,
    max_program_operations: 3,
    max_worker_retries: 1,
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  },
  policy: {read_only: true, evidence_required: []}
};
```

Stable source-site strings are part of the durable command coordinate. Keep
them deterministic across retries and restarts. Use `ng.any(...)` when the
first required successes should win and `ng.race(...)` when the first terminal
member should win; both cancel outstanding siblings through structured
concurrency. Ambient I/O, timers, dynamic loading, `eval`, and native handles
remain unavailable.

`harness.mode` must be explicit. `dsl` returns `H_MIGRATION_CORE_DSL`, `core`
returns `H_MIGRATION_CORE_JSON`, and `program`/`program_json` return
`H_MIGRATION_PROGRAM_JSON`; all point at `/harness/mode` and are never selected
from a request's JSON shape or missing fields. Strict Core JSON remains an
internal/interchange representation for validated Core and Program artifacts,
and trusted C++ in-process construction remains supported; neither is a public
Harness authoring language.

Schema export, compile, and start now consume the same immutable
`HarnessAdmissionProfile`. Its scoped `GraphRegistry` and manifest list every
available node, reducer, and condition together with implementation, lowering,
and compatibility metadata. Process-global registry entries are not part of
this palette and cannot be resolved by Harness admission. Compile stops at
validated declarative `TopologySpec`, so rejected input constructs no
`GraphNode` and dispatches no worker or effect. Retained artifacts bind the
profile ID and fingerprint; a different or pre-profile artifact fails closed at
start/resume instead of being reinterpreted.

C++ embedders pass a non-default profile through `HarnessServiceResources` at
construction. This additive resource boundary preserves the existing
`HarnessServiceConfig` layout. The profile fingerprint covers the manifest and
the scoped registry's exported semantic projection. Each
`implementation_identity` is a trusted declaration and must change whenever
the corresponding callable behavior changes.

This is the current Program-backed Harness compatibility adapter.
Accepted Harness requests still translate to the legacy `ProgramSource`,
compile through `ProgramCompiler`, admit through `ProgramCatalog`, and execute
through `ProgramRuntime`; `GraphEngine` remains the only node executor.

The accepted replacement for general authoring is standard JavaScript on
embedded QuickJS. The former `dsl`, standalone `core`, and `program` modes are
rejected for new publication with explicit migration diagnostics; strict Core
JSON remains internal/interchange data. See
[`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) and
[`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md). This document
describes the retained compatibility behavior and migration diagnostics; it
does not authorize new legacy source semantics.

## Local authenticated host workers

`neograph-harness-mcp` can delegate worker inference to one explicitly selected,
already authenticated local CLI. This is **model delegation**, not credential
inheritance: NeoGraph never opens the host's auth files, accepts OAuth tokens,
or translates a subscription into a provider API key. The official host
process uses its own saved login. Requires Linux for process-group isolation;
the host backend fails closed on other platforms. No `auto` selection: multiple
authenticated hosts must not trigger a silent billing/policy decision.

```bash
opencode auth login                 # or claude auth login / codex login, once
neograph-harness-mcp --executor opencode --host-status
neograph-harness-mcp --executor opencode --host-model openai/YOUR_MODEL
```

Choose `--executor claude` or `--executor codex` for the other CLIs; omit
`--host-model` to request that adapter's host default. Environment alternatives
are `NEOGRAPH_HARNESS_EXECUTOR` and `NEOGRAPH_HARNESS_HOST_MODEL`. Status and
normal startup perform a bounded, non-secret version/login preflight first,
and OpenCode verifies explicit model names through `opencode models`.
Claude and Codex do not expose the same portable model-list API; their
model-selection errors are reported from the attempted request. Missing CLI,
logout, unsupported output, quota, policy, and model failures are not silently
converted into provider calls. Exact model or `host default`, CLI version,
executor identity, and mode appear in status/host metadata, without credentials.

Embedders and future shared-process MCP configuration can construct the same
boundary from `<neograph/mcp/harness_host_agent.h>`:
`preflight_host_agent(config)` first, then
`HarnessProgramHostConfig::worker_executor = make_host_agent_executor(config)`.
The `HostAgentExecutorConfig` workspace is an explicit canonical root; bind
its non-secret executor/model identity in `provider_host_configuration` so
retained artifacts cannot be resumed against another route. This boundary
does not modify the direct `Provider` executor or require outbound MCP
Sampling.

The subprocess profile is intentionally read-only and local stdio only.
OpenCode runs `--pure` from a fresh, temporary config directory with only
read/glob/grep allowed; `.env` and known host credential paths remain denied
to the model's read tool, and the workspace root is granted read-only
external-path access. Claude runs `-p --safe-mode --permission-mode plan` with only
Read/Glob/Grep, not `--bare` (which skips subscription login). Codex runs
`exec --ignore-user-config --ignore-rules --ephemeral --sandbox read-only`. Every subprocess
gets a targeted environment allowlist with direct-provider and unrelated
repository/cloud credentials excluded, a depth marker forbidding nested
NeoGraph host delegation, bounded prompt/event/stdout/stderr capture, and a
deadline; cancellation terminates the process group. No shell evaluates
prompts or model IDs. Program-level schema verification and bounded retries
still apply; CLI-generated JSON is not trusted until accepted by the worker
schema gate. Usage is accounted from the host's machine-readable completion
events. These CLI transports do not offer a universal hard generation-time
token cap: an over-budget response is rejected and never sent to the judge,
but its upstream usage may already have been billed. Workspace read-only
permissions are a host policy boundary, not an OS mount namespace; use an
isolated OS account/container where untrusted repository or host policy needs
strong filesystem confinement. The local CLI adapter does not accept Harness
capability tools; use the direct Provider executor for capability-rich workers.

Live tests are opt-in on a trusted, authenticated private runner:
`NEOGRAPH_HARNESS_LIVE_HOST=claude|codex|opencode` selects the one installed
CLI, and `NEOGRAPH_HARNESS_LIVE_MODEL` optionally fixes its model.
The OpenCode OAuth job additionally sets
`NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH=1` and an available `openai/...` model.
The test confirms `opencode auth list` reports OpenAI OAuth (non-secret)
before executing. The marker is a test gate, never a credential. Missing
login, model, or gate produces an explicit skip, not fabricated inference.

The MCP server can be registered with each host using its standard local
stdio configuration, substituting the installed `neograph-harness-mcp` binary
found on your PATH (do not paste credentials into the MCP entry):

```bash
claude mcp add neograph-harness -- neograph-harness-mcp --executor claude
codex mcp add neograph-harness -- neograph-harness-mcp --executor codex
```

For OpenCode, run `opencode mcp add` and choose a local server with command
`neograph-harness-mcp --executor opencode` in its guided prompts. The
equivalent supported `opencode.json` entry is:

```json
{"mcp":{"neograph-harness":{"type":"local","command":["neograph-harness-mcp","--executor","opencode"],"enabled":true}}}
```

See [OpenCode CLI](https://opencode.ai/docs/cli/),
[Claude CLI](https://code.claude.com/docs/en/cli-reference),
[Claude authentication](https://code.claude.com/docs/en/authentication),
[Codex non-interactive mode](https://learn.chatgpt.com/docs/non-interactive-mode),
and [Codex authentication](https://learn.chatgpt.com/docs/auth).
Host subscription quotas, rate limits, model eligibility, retention,
organization policy and data handling still govern delegated calls. Claude.ai
subscription use by third-party products may require Anthropic approval;
this opt-in installed-CLI path is **not** approval to redistribute a hosted
Claude subscription backend. Do not infer that login to one host grants
access to a different vendor or model. This local backend does not enable
remote HTTP delegation or MCP Sampling (unsupported on the portable host
baseline); remote/server deployment should use direct Provider credentials.

### Direct API provider (standalone/server)


Build and install the opt-in local server (the host CLI mode itself needs
no NeoGraph-specific model key):

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness --target neograph_harness_mcp -j
cmake --install build-harness --prefix "$HOME/.local"
export NEOGRAPH_HARNESS_API_KEY=your-key
neograph-harness-mcp --executor provider
```

`NEOGRAPH_HARNESS_API_KEY` takes precedence over `OPENROUTER_API_KEY`
for the included OpenRouter example. Unlike local host execution, direct
provider mode requires its own API key. The server writes protocol messages
only to stdout and diagnostics only to stderr.

For host interoperability smoke tests only, set `NEOGRAPH_HARNESS_SMOKE=1`
with `--executor provider`. It uses a deterministic in-process provider
returning a valid zero-findings review, requires no API key, and is not an
LLM quality test.
### Credentialless OpenCode global MCP adoption

Single-user local installations may explicitly adopt selected credentialless
stdio servers from the OpenCode user-global `opencode.json` source through
`<neograph/mcp/adoption.h>`. Discovery is inspection-only: it reads only that
regular, user-owned global file, never project/workspace configuration or OAuth
stores, and never starts a server. HTTP entries, disabled entries, imported
environment or file references, unsupported fields, shell executors, and
recursive NeoGraph entries are rejected.

Adoption requires explicit no-credential `argv` attestation, an independently
approved launch record, and a selected-tool/schema capability manifest. `pinned`
records bind source content, canonical cwd, executable/interpreter identity,
argv, and any identifiable script/package. Unverifiable launch forms require an
explicit `trusted_mutable` approval; they are never silently downgraded.
Discovery does not approve the bytes it has just inspected.

Each tool manifest must use `argument_policy: "exact-arguments-v1"` with
`argument_predicate: {"allowed_arguments": [<complete approved argument objects>]}`.
Comparison covers argument values, including resources, not just schema shape.
Generic `read-only` or SQL/path labels are rejected: they do not prove that
arbitrary input is safe. Harness authority remains the intersection of the worker
declaration, adopted manifest, static policy, and process boundary. MCP annotations
cannot expand it.

Before constructing a host or provider worker, call
`HardenedMcpClientRegistry::configure_harness(host_config, provider_config)`.
This installs immutable namespaced tool metadata and approved executors together;
`tool_catalog()` supplies the matching request metadata. Revocation and schema
drift fence retained executors and explicitly shut down the shared client, even
when another owner still holds a reference. Schema refresh and tool dispatch honor
the caller's deadline and cancellation token.

The adopted client uses an absolute executable, canonical cwd, replacement
allowlist environment, bounded protocol frames/stderr, and process-tree shutdown.
Windows launches use an inherited-handle allowlist and a kill-on-close Job Object.
Status records contain hashes and names, not argv, raw configuration, stderr, or
credentials. Credential-bearing modes (`secret_injected`, `host_brokered`),
remote HTTP MCP, and arbitrary shell/CLI execution remain unsupported.

The installable example supports adoption with the provider worker executor only.
It consumes a separately reviewed JSON approval, never auto-consent environment
flags:

```bash
export NEOGRAPH_HARNESS_MCP_SOURCE="$HOME/.config/opencode/opencode.json"
export NEOGRAPH_HARNESS_MCP_APPROVAL="$(cat approved-mcp.json)"
neograph-harness-mcp --executor provider
```

The approval object has `launch` and `tools` members matching `McpLaunchApproval`
and `McpToolApproval`. `launch` contains `trust_mode`, the explicit
`argv_no_credentials_attested` boolean, `source_path`, `source_content_hash`,
`cwd`, `executable`, `executable_identity`, `argv_hash`, `interpreter_identity`,
and `package_identity`. The host-side `make_mcp_launch_approval()` helper computes
these identities for review. `tools` contains `server_name`, `launch_identity`
(the approved launch's `executable_identity`), `selected_tools`, `schema_hashes`,
`manifest`, and `policy_version`. Hash normalized
`ToolDefinition::from_json(definition).to_json()` values with
`mcp_tool_schema_hash()` when recording schema approval.

An explicit source override must still name the documented user-global file;
project paths cannot masquerade as global configuration. `--host-status` with
the source configured prints discovery-only redacted records before credential
checks and never starts a downstream server.

Durable host-brokered calls require both record and checkpoint persistence.
The example enables both with one explicit directory:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

This stores immutable artifacts, mutable run records, and an append-only causal
journal in `runs.db`, and graph checkpoints in `checkpoints.db`. Journal rows
and every Harness-created checkpoint bind the run to its immutable artifact,
compiled revision digest, MCP protocol version, and Harness profile. Worker
attempts include duration, validation/retry outcome, and correlation IDs that
join provider, capability, and host-brokered calls to their issuing attempt.
Both SQLite stores use WAL mode and a bounded busy
timeout. Existing version-1 record databases migrate transactionally to version
3 when opened. The directory survives server restarts. A
`host_brokered` catalog entry is rejected at compile time when either store is
missing, so a workflow cannot advertise resumability it does not have.

Custom embeddings can construct the same backend through
`SqliteHarnessRecordStore` from the optional `neograph::mcp_sqlite` target.
The default journal mode recursively replaces common secret and content fields
with `[REDACTED]` before SQLite sees them. `METADATA_ONLY` discards every event
payload; `FULL` preserves provider content, tool arguments, and results exactly
and should only be enabled for data approved for storage. Events can be read in
run order through `HarnessJournal::list_events(run_id, after_sequence, limit)`.
`FileHarnessRecordStore` remains available for deployments that prefer atomic
JSON files; it does not implement the journal boundary.

### Retention

The SQLite store implements the optional `HarnessRetentionStore` sibling
interface; the stable `HarnessRecordStore` vtable is unchanged. Before retaining
an artifact or starting a run, `HarnessService` applies `max_artifacts` and
`max_runs` from `HarnessServiceConfig`. Defaults are 128 each.

Cleanup removes only terminal leaf runs. Queued, running, and input-waiting runs
are protected, as are in-process executions that have not finished journal
finalization. A replay or fork row records `source_run_id`, so its source cannot
be removed while that dependent remains retained. If space is needed, the
dependent leaf is removed first; the source becomes eligible only in a later
step after no retained row references it. Limits are therefore soft when every
candidate is active, explicitly protected, or still referenced.

Within `runs.db`, one transaction deletes a run's journal rows before its run
row and deletes an artifact only after no run references it. After that commit,
the Harness removes the deleted run's checkpoint thread from the separately
configured checkpoint store. A crash or checkpoint-backend failure during that
second phase can leave unreachable checkpoint storage, but cannot leave a
retained replay/fork pointing at a deleted source record. A later administrative
or backend-specific orphan sweep may reclaim such checkpoint-only residue.

`FileHarnessRecordStore` does not implement durable cleanup; its historical
in-memory artifact-cache eviction and hard run-capacity behavior remain.

## Debugger Views

`neograph_get` keeps `status` as its compact default and adds four debugger
views without adding another MCP tool:

| View | Result |
|---|---|
| `attempts` | Journaled worker attempt start/completion/interruption events |
| `trace` | Existing ordered GraphEngine node trace plus the causal journal timeline |
| `checkpoints` | Payload-free checkpoint metadata: ID, parent, node, phase, step, and channel names |
| `diff` | Channel values and versions changed between each checkpoint and its parent |

`attempts` and `trace` accept `after_sequence` as an opaque forward cursor. All
four views accept `limit` from 1 through 1000. Returned artifact URIs can carry
the same pagination as a query, for example:

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

Only `after_sequence` and `limit` are accepted in these URIs. Unknown or
malformed query fields fail instead of being ignored. Journal-backed views
return the payload exactly as persisted, so the configured redaction mode is
preserved. The `diff` view is computed from the checkpoint store rather than
the journal and can contain full channel values; treat access to it like access
to the existing detailed run result.

## Replay Modes

`neograph_start` can replay a completed run without adding another MCP tool:

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` re-executes the compiler-locked graph with the source journal's completed worker-attempt results. It never calls the configured worker, provider, MCP, A2A or capability executor. Source artifact revision, protocol and profile must still match, and the journal must use `FULL` payload mode. `REDACTED` and `METADATA_ONLY` journals cannot replay because they lack exact worker output. Exact captured operation coordinates, original owner/version/input permissions and durable provider-bank custody are required; incompatible older custody is rejected, never reconstructed from token totals or attempt numbers.

Use `mode: "live"` to execute the same retained artifact with live providers and
tools. Snapshots and journal lifecycle events label runs as `recorded_replay` or
`live_replay` and include `source_run_id`; ordinary starts remain `live`.

Recorded replay authenticates the source's original immutable invocation
permissions separately from its remaining spending authority. One source-lineage
CAS transfers the retained remainder to the replay run; a second replay cannot
spend that same source remainder again. Captured calls reuse their historical
operation coordinates without replenishing operation slots. Actual new replay
Core and wall-time work consumes the transferred remainder.

Provider reports, known charges, uncertain reservations, and effect identities
are hydrated exactly once into a separate observation-only bank. Playback cannot
reserve, refund, settle, add reports, or dispatch a fresh effect. A lost replay
execution is fenced by its durable lease and retains its transferred/uncertain
credits; it is not repaired by live dispatch. Run snapshots expose the current
lineage remainder, including a source whose credit moved to a replay.

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Portable formats remain [`provider-message-v2`](../schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json). Genuine C++ checkpoint sidecars retain native seals in memory. Durable native history requires host-owned `sp::NativeArchive`: closed v3 / `spna3`, with authenticated owner-private custody and an independent key. Archive v2 is rejected, not upgraded or interpreted. Authentication binds every semantic descriptor choice (origin/paths/headers, policy, request field mappings, usage path and stop mappings), owner and exact custody binding. It is neither encryption nor vendor-issuer authentication; never publish archive bodies, keys, native blobs or raw wire observations. An archive is evidence storage, not a money grant or a spending lease. Program/external banks remain independently journal-owned; snapshot copies cannot create credit.

**Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The owner-approved protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.

**Current declarations; integrated runtime evidence below:** `<neograph/graph/checkpoint.h>` declares `ManagedBudgetLeaseScope` with `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks` and `deadline_clock_identity`. `OwnedManagedBudgetLease` exposes read-only `scope()`, `actor_id()`, immutable `bank_generation()`, `revision()`, `head_checkpoint_id()` and `head_commitment()`; it has no public authority-import constructor. `ManagedBudgetEffectReceipt` exposes `active()`, `effect_id()`, `claim_amount()` and `request_digest()`; a default receipt grants nothing. `CheckpointStore` declares `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)` and `release_managed_budget_lease(lease)`, with `_async` counterparts. Sync `CheckpointStoreCore` and `AsyncCheckpointStore` expose their respective variants. `managed_budget_checkpoint_commitment(checkpoint)` covers the full durable checkpoint, not just bank JSON. These declarations do not establish backend CAS, currency safety, installed ABI compatibility or a successfully exercised runtime path.

**Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Current release-error contract; exercised suite/probes below.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

**Archive-owner/retention contract; exercised suite/probes below.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

**Native-custody pre-I/O gate; exercised suite/probes below.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.

`ProviderOutcomeError` is the common outcome-preserving host-error base; `ProviderObserverError` and `ProviderDispatchOutcomePersistenceError` retain the complete drained SDK result and original `cause()`. The persistence error also retains secondary observer failure in `delivery_error()`. `ProviderFailure::outcome()` retains the SDK failure itself. These are evidence, not permission for Node/Program to redispatch: the SDK is the sole owner of transport retries, and a caller-selected `max_output_tokens` is never silently clamped. A larger-cap semantic call needs a new prepared digest, distinct deterministic call ordinal, admission from the original resource bank and the original deadline; native replay eligibility grants no renewed credit.

`ProgramFailure` retains live `provider_outcome` and `provider_cause`. Its canonical factual SDK witness binds genuine archive custody to owner/run/version/bundle/operation/attempt; Runtime eagerly restores configured custody before exposing a recovered failure. Public data-only `ProgramResult::create()` cannot bypass this with a prefilled witness, and an unresolved parsed seal is not an executable result. After process restart the original exception pointer is unavailable (`provider_cause == nullptr`), not recreated from text. A failure that cannot be persisted cannot be serialized, published or replayed.

`RecordedBindingSet` is source-bound, move-only data, never a caller-supplied dispatcher. The trusted Catalog `recorded_capability_binder` independently materializes captured-only capabilities from real persisted source events. `ProgramRuntime::replay_recorded()` checks original selected-source permissions, then transfers the actual remaining bank through durable CAS; inherited spend is not a new model grant. The old `start_recorded` renewal API is removed. InMemory, File, SQLite and PostgreSQL Program stores preserve the exact immutable owned lease throughout execution; expiry does not renew it. Controlled JavaScript still validates the underlying capability manifest and consumes exact completed command outcomes without redispatching external effects.

**Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

The following paid, native-axis, and integrated-runtime observations are
historical provider-cutover cohorts. Their counts, failures, skips, and limits
remain unchanged. “Latest” in that retained record means the last run in its
cohort, not verification of the current Python bindings or this documentation
change.

**Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. No further paid calls are authorized. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

**Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

**Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.


## Compatible Fork

Compile a repaired Harness first, then branch an exact preceding checkpoint into
that target artifact through the existing `neograph_start` tool:

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

The source checkpoint must belong to `source_run_id`. Before allocating a run,
the Harness verifies the checkpoint schema, source revision, MCP protocol,
Harness profile, every restored channel and reducer, every continuation node,
and any active barrier interface against the target artifact. An incompatible
branch returns `started: false`, `status: "incompatible_fork"`, and
machine-readable `H_FORK_*` diagnostics with `path` and `witness`; it creates no
run or fork checkpoint.

A checkpoint store is required. Without a record store, a fork may reference
only source runs and artifacts still resident in the current service process;
configure both stores for fork lineage that must survive a restart.

A compatible branch is labeled `compatible_fork` and carries both
`source_run_id` and `source_checkpoint_id` in start responses, snapshots, and
lifecycle journal events. Execution resumes at the selected checkpoint's
`next_nodes`; already committed predecessors do not run again. The target
artifact supplies the repaired topology, worker contracts, and tool catalogue,
while restored channel values, including the original task channel, come from
the source checkpoint. Use a fresh start rather than a fork when the task input
itself must change.

The source run, artifact, and selected checkpoint are references of the fork and
must remain retained while compatibility checking or fork execution can use
them. Retention cleanup must remove dependents first or preserve referenced
sources; it must never delete a source checkpoint between preflight and branch
creation.

## Streamable HTTP

Remote transport is opt-in so the existing stdio-only target remains small and
does not silently gain an HTTP/OpenSSL dependency:

```bash
cmake -S . -B build-harness-http \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness-http --target neograph_harness_mcp -j
cmake --install build-harness-http --prefix "$HOME/.local"

export NEOGRAPH_HARNESS_TRANSPORT=http
export NEOGRAPH_HARNESS_HTTP_HOST=127.0.0.1
export NEOGRAPH_HARNESS_HTTP_PORT=8080
"$HOME/.local/bin/neograph-harness-mcp"
```

The endpoint is `http://127.0.0.1:8080/mcp`. It implements the published MCP
2025-11-25 Streamable HTTP POST contract with per-session MCP lifecycles and
JSON responses. Notifications return HTTP 202. DELETE terminates a session.
The optional standalone GET/SSE channel is deliberately not implemented and
returns HTTP 405, which the transport specification explicitly permits.

Security defaults are transport-level and do not couple authentication to
`GraphEngine` or `HarnessService`:

- The default bind is `127.0.0.1`; a non-loopback bind is rejected unless a
  bearer authorizer is configured.
- Every supplied `Origin` is rejected unless it exactly matches an entry in
  `NEOGRAPH_HARNESS_ALLOWED_ORIGINS` (comma-separated in the executable).
- `NEOGRAPH_HARNESS_BEARER_TOKEN` enables the executable's single-principal
  bearer boundary. Library embeddings can use
  `MCPHttpServerConfig::bearer_authorizer` for OAuth/JWT validation and return a
  stable principal/scope.
- Sessions are bound to the returned authorization scope. A different valid
  principal cannot reuse a leaked `Mcp-Session-Id`.
- The `MCPHttpServer` factory receives that validated scope and returns an
  `MCPHttpServerSession` owner. Multi-tenant embeddings must use the scope to
  select isolated Harness record/checkpoint stores; no auth state enters the
  graph runtime itself.
- Request payload, HTTP worker, queue, session, and response-wait limits are
  bounded by `MCPHttpServerConfig`.

`neograph::mcp::ScopedHarnessStore` maps public IDs and schema-owned record/journal
references into a reversible tenant namespace; opaque request, result, and event
payloads are not rewritten. File and SQLite stores accept these private IDs.
Overlong File keys use fixed-length hash filenames without changing short-key
paths. SQLite retention limits both counts and deletion candidates to the selected
namespace within one transaction, preserving other tenants and protected source
references. This storage boundary does not replace the authenticated scope or
the application's provider/tool/quota policy.

For any non-loopback deployment, terminate TLS at a trusted reverse proxy and
use its OAuth/OIDC validation or an equivalent `bearer_authorizer`. Forward the
original `Authorization` and `Origin` headers, do not expose a cleartext public
listener, and deploy one authorization domain per Harness state directory.

## Host Setup

Use the absolute path to the installed `neograph-harness-mcp` binary for `SERVER`.
These entries select the direct Provider executor; pass the corresponding
`--executor claude|codex|opencode` instead for the local host mode above.

```bash
SERVER=/absolute/path/to/neograph-harness-mcp
```

Claude Code, local project scope:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER" --executor provider
claude mcp get neograph-harness
```

Codex CLI:

```bash
codex mcp add neograph-harness -- "$SERVER" --executor provider
codex mcp list
```

For non-interactive `codex exec` against this trusted local server, set
`mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` in
Codex `config.toml`. Without it, Codex correctly cancels `neograph_compile`
because retaining an artifact is not annotated read-only. Interactive sessions
may keep the default prompt instead.

OpenCode, in project `opencode.json` or user configuration:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "neograph-harness": {
      "type": "local",
      "command": ["/absolute/path/to/neograph-harness-mcp", "--executor", "provider"],
      "enabled": true,
      "environment": {
        "NEOGRAPH_HARNESS_API_KEY": "{env:NEOGRAPH_HARNESS_API_KEY}"
      }
    }
  }
}
```

Verify with `opencode mcp list`. These forms follow each host's official MCP
configuration contract as reviewed on 2026-07-21.

## PR Review Workflow

Ask the host to collect the PR diff with its normal repository tools, then use
the Harness tools. A suitable request is:

```json
{
  "task": {
    "objective": "Review this PR diff. Report only actionable correctness, security, or regression findings. Include the diff after this sentence.",
    "acceptance": [
      "Every finding identifies a file and line",
      "Every finding quotes concrete evidence",
      "Return an empty findings array when no issue is proven"
    ]
  },
  "harness": {"mode": "preset", "preset": "pr_review_panel"},
  "workers": [
    {
      "id": "correctness",
      "instructions": "Review behavior, edge cases, and regressions.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    },
    {
      "id": "security",
      "instructions": "Review trust boundaries, validation, and unsafe side effects.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    }
  ],
  "tool_catalog": [],
  "budgets": {
    "max_steps": 10,
    "timeout_seconds": 600,
    "max_parallel_workers": 2,
    "max_worker_retries": 1,
    "provider_timeout_seconds": 60,
    "max_output_tokens": 4096
  },
  "policy": {
    "read_only": true,
    "evidence_required": ["file", "line", "evidence"]
  }
}
```

### Provider Budgets

`budgets.provider_timeout_seconds` sets one prepared provider operation's
absolute deadline to 1--600 seconds. `budgets.max_output_tokens` sets its output
limit to 1--128000 tokens. Both are optional: omission leaves that control unset
and preserves the admitted SDK/default policy, not unlimited time or output.
The SDK owns any provider retry within the prepared operation's deadline.

A worker may set either field to a smaller value. A worker value above the
Harness-wide value is rejected at compile time. On a deadline, Harness cancels
only the child cancellation token supplied to that provider call; it does not
cancel sibling workers or the enclosing run. The provider must honor the token,
so a provider that cannot be interrupted may return after its deadline.

The host should follow this sequence:

1. Call `neograph_compile` and stop if `ok` is false.
2. Call `neograph_start` with the returned `artifact_id`.
3. Poll `neograph_get` with `run_id`; this returns only outcome and counts.
4. If detail is needed, call `neograph_get` with the same `run_id` and a
   returned `neograph://runs/...` URI as `uri`. Do not pull traces into context
   by default.

### Finding Provenance

The details artifact preserves each schema-validated worker response in
`workers` and keeps the established flat `findings` array for existing clients.
`finding_sources` is a same-length parallel array: each entry contains the
aggregate `finding_index`, source `worker_id`, and that worker's `local_index`.
Use it to identify the source of duplicate local IDs such as `F1`; do not add
provenance fields to the worker's declared finding object.

## Host-Brokered Resume

Use `executor.kind: "host_brokered"` when the MCP host, rather than the worker
process, owns a capability. Set `executor.interaction` to `"tool_result"`
(default) or `"input"`. The provider executor validates the requested arguments
and returns one of two non-terminal run states:

- `awaiting_tool_results`: the host must execute the named capability.
- `input_required`: the host must collect an input value.

`neograph_get` includes a `pending` object with a unique `call_id`, `tool_id`,
validated `arguments`, and `result_schema`. Submit exactly that call through:

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` rejects a mismatched call ID, a result that violates the
declared schema, an expired call, and a late result for a non-waiting run. An
identical duplicate is acknowledged without re-executing the graph; a
conflicting duplicate is rejected. The accepted resume intent is persisted
before execution is scheduled, so polling after a process crash restarts the
resume from the `NodeInterrupt` checkpoint without repeating successful sibling
workers.

### External Effects And Reconciliation

The ordinary host-brokered contract is backward compatible: a catalog entry
without `executor.effect` remains `awaiting_tool_results` after a process
restart and accepts the same `{run_id, call_id, result}` resume request.

For a host capability that can make an externally visible, non-idempotent
change, declare that risk explicitly. Effect metadata is valid only with the
default `host_brokered` `tool_result` interaction; it is not input collection
metadata.

```json
{
  "executor": {
    "kind": "host_brokered",
    "effect": {
      "idempotency": "unsupported",
      "status_query": true,
      "fencing": true
    }
  }
}
```

The pending call then includes a durable `effect` object. Its `effect_id` and
`idempotency_key` are scoped to the Harness run and differ from the provider's
tool-call ID. `status_query` and `fencing` describe host capabilities; Harness
records them but does not invent a provider-specific query or retry protocol.

If the service reconnects while an `idempotency: "unsupported"` call is still
waiting, it changes only that run to `ambiguous_effect`. This means the host
may have performed the effect before the process stopped, but Harness cannot
prove either outcome. The compact status includes `pending` and `ambiguity`,
the journal records `host_brokered.effect.ambiguous`, and Harness neither
replays the tool nor reports the effect as failed or completed.

Resolve the ambiguity through `neograph_resume` after the host checks its own
authoritative system:

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` validates and consumes `result`, then resumes from the checkpoint.
`failed` records a terminal Harness failure without executing the worker again.
`unknown` leaves the run in `ambiguous_effect` for later reconciliation. Exact
duplicate completed, failed, or unknown submissions are idempotent; a conflicting
completed or failed submission is rejected. Every non-duplicate reconciliation
is journaled as `host_brokered.effect.reconciled`.

An ambiguous effect is deliberately not cancellable and does not expire. A
cancel or timeout cannot establish whether the external effect occurred; submit
`unknown` if the authoritative system cannot resolve it yet.

This protocol does not claim exactly-once delivery across a host crash. Hosts
that support idempotency keys or a status query should use those systems to
determine a real outcome before submitting a reconciliation.

Run snapshots include `created_at`, `updated_at`, `expires_at`, and
`poll_after_ms`. The default TTL is 24 hours and the default polling interval is
one second; embeddings can override both through `HarnessServiceConfig`.

## Experimental Tasks Profile

MCP Tasks is not part of core MCP 2025-11-25 and the upstream extension still
labels itself experimental. NeoGraph therefore keeps it disabled by default and
separate from the stable `run_id` plus `neograph_get` polling contract.

To opt in on the example server, durable state must also be enabled:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

The server then advertises `io.modelcontextprotocol/tasks`, marks
`neograph_start` with optional task support, and serves `tasks/get`,
`tasks/update`, and `tasks/cancel`. It returns a `CreateTaskResult` only when the
individual `tools/call` request includes:

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

Clients without that request opt-in receive the ordinary `CallToolResult` and
continue polling `neograph_get`; enabling the profile does not alter the stable
fallback. Task statuses are `working`, `input_required`, `completed`, `failed`,
and `cancelled`. `tasks/update.inputResponses` is keyed by the pending
`call_id`, and polling clients should honor `pollIntervalMs` and `ttlMs`.

## Capability Backends

`make_provider_harness_executor` drives workers through any NeoGraph
`Provider`. If a model requests a declared tool, the executor validates its
arguments and output against the catalog before and after dispatch.

The provider executor builds a typed `ProviderRequest`; `prepare` validates and
encodes without I/O, and `dispatch` consumes the prepared request. `invoke`
combines those steps. The executor retains the owned SDK Completion/Failure
outcome, including ordered messages/parts and partial failures, rather than
turning a failure into text-only success. Provider reports retain unknown usage
as nullable fields; conservative token charges/reservations are separate
authority. Recorded provider playback does not charge or settle those outcomes
again; any new replay CPU/Core work still consumes its transferred remainder.

Use `make_mcp_harness_capability_executor` for initialized downstream
`MCPClient` instances, or `a2a::make_harness_capability_executor` for A2A
agents. The request remains the authority: a worker sees only tool IDs listed
in its `tools` array.

For filesystem tools, declare every path-bearing input in `path_arguments` and
set `policy.workspace_roots`. Relative paths resolve under the first root;
canonical paths outside every configured root are rejected before dispatch,
including escapes through existing symlinks. The canonical path is passed to
the capability backend rather than the model-supplied spelling. Downstream MCP
and A2A services remain separate trust boundaries and should enforce the same
root policy to close filesystem time-of-check/time-of-use races.
With `policy.read_only: true`, compilation rejects every catalog entry not
marked `read_only: true`.

## Distribution And Protocol Profiles

The supported local distribution path is the installable
`neograph-harness-mcp` binary above. Source builds can continue using the
example target, and Python wheels remain library/runtime packages rather than
implicitly installing a remote daemon. MCPB and official registry publication
remain release/discovery packaging options; they are not required for the wire
protocol and should be added only with signed release artifacts and an explicit
remote-auth deployment manifest.

NeoGraph currently publishes only the dated MCP `2025-11-25` profile. Final
SEPs describing a future stateless protocol do not create a new wire version;
no successor profile will be advertised until the MCP project publishes a new
dated specification.
