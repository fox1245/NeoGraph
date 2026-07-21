# NeoGraph Harness MCP

NeoGraph Harness compiles a bounded multi-worker workflow before it runs. The
MCP surface stays at five tools:

- `neograph_schema` discovers the installed request contract and presets.
- `neograph_compile` elaborates, compiles, and validates without executing.
- `neograph_start` starts a retained artifact or an inline request.
- `neograph_get` polls compact status or dereferences a result artifact URI.
- `neograph_cancel` cooperatively cancels a queued or running workflow.

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
