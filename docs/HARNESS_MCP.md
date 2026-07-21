# NeoGraph Harness MCP

NeoGraph Harness compiles a bounded multi-worker workflow before it runs. The
stable MCP surface stays at six tools:

- `neograph_schema` discovers the installed request contract and presets.
- `neograph_compile` elaborates, compiles, and validates without executing.
- `neograph_start` starts a retained artifact or an inline request.
- `neograph_get` polls compact status or dereferences a result artifact URI.
- `neograph_resume` validates and submits the exact pending host result.
- `neograph_cancel` cooperatively cancels a queued, running, or waiting workflow.

The shipped presets are `fanout_judge`, `pr_review_panel`, `bug_triage`, and
`research_synthesis`. Presets produce ordinary strict-core graph artifacts, so
the same diagnostics and source maps apply to preset and DSL requests.

## Build And Run

Build the local stdio server with the OpenAI-compatible provider adapter:

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON
cmake --build build-harness --target example_harness_mcp_server -j
export OPENAI_API_KEY=your-key
export NEOGRAPH_HARNESS_MODEL=gpt-4o-mini
```

`NEOGRAPH_HARNESS_API_KEY` takes precedence over `OPENAI_API_KEY`.
`NEOGRAPH_HARNESS_BASE_URL` selects any OpenAI-compatible endpoint. The server
writes protocol messages only to stdout and diagnostics only to stderr.

For host interoperability smoke tests only, set `NEOGRAPH_HARNESS_SMOKE=1`.
That explicit mode uses a deterministic in-process provider returning a valid
zero-findings review, requires no API key, and must not be used as an LLM
quality test.

Durable host-brokered calls require both record and checkpoint persistence.
The example enables both with one explicit directory:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

This stores immutable artifacts and mutable run records in `runs.db`, and graph
checkpoints in `checkpoints.db`. Both SQLite stores use WAL mode and a bounded
busy timeout. The directory survives server restarts. A
`host_brokered` catalog entry is rejected at compile time when either store is
missing, so a workflow cannot advertise resumability it does not have.

Custom embeddings can construct the same backend through
`SqliteHarnessRecordStore` from the optional `neograph::mcp_sqlite` target.
`FileHarnessRecordStore` remains available for deployments that prefer atomic
JSON files.

## Streamable HTTP

Remote transport is opt-in so the existing stdio-only target remains small and
does not silently gain an HTTP/OpenSSL dependency:

```bash
cmake -S . -B build-harness-http \
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

For any non-loopback deployment, terminate TLS at a trusted reverse proxy and
use its OAuth/OIDC validation or an equivalent `bearer_authorizer`. Forward the
original `Authorization` and `Origin` headers, do not expose a cleartext public
listener, and deploy one authorization domain per Harness state directory.

## Host Setup

Use an absolute path for `SERVER`:

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

Claude Code, local project scope:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

Codex CLI:

```bash
codex mcp add neograph-harness -- "$SERVER"
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
      "command": ["/absolute/path/to/example_harness_mcp_server"],
      "enabled": true,
      "environment": {
        "OPENAI_API_KEY": "{env:OPENAI_API_KEY}",
        "NEOGRAPH_HARNESS_MODEL": "gpt-4o-mini"
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
    "max_worker_retries": 1
  },
  "policy": {
    "read_only": true,
    "evidence_required": ["file", "line", "evidence"]
  }
}
```

The host should follow this sequence:

1. Call `neograph_compile` and stop if `ok` is false.
2. Call `neograph_start` with the returned `artifact_id`.
3. Poll `neograph_get` with `run_id`; this returns only outcome and counts.
4. If detail is needed, call `neograph_get` with the same `run_id` and a
   returned `neograph://runs/...` URI as `uri`. Do not pull traces into context
   by default.

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
