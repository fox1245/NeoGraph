# Evolving Harness Chat

**Languages:** [English](README.md) | [한국어](README.ko.md)

A runnable, two-tenant chatbot built on NeoGraph Program. Each turn produces a
bounded Harness proposal. The host keeps the current topology or compiles and
admits an immutable successor, then replaces the assistant at its durable
checkpoint. The orchestrator keeps waiting on the same logical assistant.

- **Alice and Bob:** separate messages, owner scopes, catalogs, engine caches,
  Program families, model-call ledgers and nonrenewable budgets.
- **Recursive agents:** orchestrator → assistant → reviewer. A review Harness
  drafts an answer, synthesizes an admitted reviewer child, waits for its critique,
  then refines the answer. Every reviewer has its own Program identity.
- **Inspector:** current compiled Core JSON, DSL, agent tree, generation, remaining
  Program budgets, proposal reason, topology differences and admission outcome.
- **Persistence:** SQLite or PostgreSQL stores Program artifacts, transitions,
  chat messages, model reservations/results and evolution decisions.
- **OpenRouter:** uses the existing native OpenAI-compatible provider. An explicit
  offline demo mode exercises the same compiler, admission and runtime APIs.

## Build and run

```bash
cmake -S . -B build-chat -G Ninja \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-chat --target cookbook_program_chatbot -j 4
./build-chat/cookbook_program_chatbot --mock --db evolving-chat.sqlite
```

Open http://127.0.0.1:8768. Switch between Alice and Bob. In demo mode, a message
containing `review`, `검토`, or `비교` proposes the review Harness. The first answer
uses the current Harness; an approved change applies when the next turn resumes
that assistant. The force checkbox alternates the two reviewed plans for a demo;
it does not assert that the new plan is better.

For OpenRouter, set credentials and an available model in the environment:

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='z-ai/glm-5.3-flash'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
# Or read an existing dotenv file without printing its values:
./build-chat/cookbook_program_chatbot --live --env-file /path/to/.env \
  --model z-ai/glm-5.3-flash --session glm-demo
```

The live CLI defaults to `z-ai/glm-5.3-flash`. `--model` overrides
`OPENROUTER_MODEL`; process environment values override dotenv values. Without
`--env-file`, the CLI discovers the nearest `.env` from the working directory.
`--no-env` disables this discovery. Only named chatbot settings are consumed;
the file is parsed as data, not sourced as a shell script.
For GLM 5.3 Flash, chat calls default to `reasoning_effort=low` to leave room for
visible replies within the output cap. `--reasoning-effort default` omits this
override; other explicit values must be supported by the chosen model/provider.
The DSL capability evaluator keeps its separate generation configuration.

OpenRouter uses the existing cookbook's ZDR routing option. Choose a model with
an eligible route. No token price is assumed. Do not
put credentials in the DSL, database, HTTP body or source. Changing the provider,
model, skill content, output cap or build requires a new explicit `--session`; reopening a session retains
its stored budget limits. `NEOGRAPH_CHAT_BASE_URL` can select another compatible
endpoint; plain HTTP requires `--allow-loopback-provider` and a literal loopback
address, intended for protocol tests.

The native adapter sends the output limit as `max_completion_tokens`, supported
by the [OpenRouter chat API](https://openrouter.ai/docs/api/api-reference/chat/send-chat-completion-request).
The default is 4,096 per call (including provider reasoning tokens); use
`--max-output-tokens` to set 1..8,192.
`--provider-timeout-seconds` sets a 1..120 second per-call timeout (default 120).
The host checkpoint wait covers the sequential calls between checkpoints, and
the reviewer has a separate 180-second child budget. Timed-out calls remain
uncertain and charged; they are not automatically retried.

For PostgreSQL, run native Docker inside WSL and set the connection string in that
same shell. Use a dedicated database; the example creates its own tables but is
not a production deployment/migration manager.

```bash
export NEOGRAPH_CHAT_POSTGRES_URL='postgresql://USER:PASSWORD@127.0.0.1:PORT/DATABASE'
./build-chat/cookbook_program_chatbot --mock --session postgres-demo
```

When the variable is present, both chat and Program persistence use PostgreSQL.
Without it, `--db` selects SQLite. Either backend can be disabled at build time.

## What is synthesized

The model returns `{plan, reason, confidence}`. The host accepts only `direct` or
`review`, a bounded reason and confidence in [0,1]. Below 0.7, invalid JSON or an
unknown plan is rejected. An unchanged plan is recorded as `kept`.
Evolution calls request JSON-object response mode through the native provider;
the application still validates the exact fields and allowed values.
An empty or truncated proposal is rejected without losing the completed answer.
Known provider usage is settled and its stop reason is retained in the proposal
diagnostic; transport uncertainty still prevents automatic redispatch.

These parameters instantiate reviewed JavaScript templates. A candidate must pass
source identity checks, bounded compilation, host semantic/template validation,
Catalog admission and generation CAS. Child proposals pass the durable child
synthesis gateway and an independently persisted host grant. The model cannot
supply grants, credentials, arbitrary imports, native code or a larger budget.

The example deliberately qualifies **reviewed-template synthesis**, not arbitrary
model-written JavaScript. Template validation establishes an allowed behavior
shape; it is not proof of answer quality. QuickJS generator control and the model
node report **Unmanaged** execution guarantee. Do not interpret the inspector as
claiming strict replay of an unknown external model effect.

## Agent authoring skill

The host loads [SKILL.md](../../../skills/neograph-harness-authoring/SKILL.md) and
its `references/chat-template-proposals.md` into the evolution model's system
context. Ordinary answer/reviewer calls keep their role-specific prompts. The
loaded skill digest is persisted with the session, and model-call identities bind
the actual prompt and output cap, so reopening cannot silently change guidance.
This version changes the example build/registry identity; use a new session for
demo databases created by earlier builds.

The skill also has separate QuickJS authoring and native runtime-handoff guides.
The source-generation evaluator loads the QuickJS route, gives the model the
native compiler manifest, compiles returned source and feeds rejected diagnostics
back for bounded repair. See [DSL capability evaluation](../../../docs/DSL_CAPABILITY_EVAL.md).
The chatbot template route itself does not expose model-callable compiler tools.

## Accounting and recovery

Each tenant has a finite session (default 12 turns / 100 model calls / 200,000
model tokens). Provider calls reserve tokens before dispatch. Known usage settles
the reservation; missing usage retains it. Prompt reservation uses UTF-8 bytes
plus framing/output allowances, not a model-specific tokenizer. Reported usage
above the reservation remains charged. Monetary cost is shown as unknown: this
example does not provide a monetary ceiling or assume token prices.

Program compile, operation, Core-step, child/depth and wall-time budgets remain
nonrenewable through replacement and restart. Idle time consumes the session's
wall-time allowance. Dynamic successor compilation calls the host-only
`ProgramRuntime::reserve_synthesis` at the held checkpoint before compilation.

Reopen the same database, session, provider and model after process loss. The first
new turn reconnects the existing family and reconciles the last checkpoint. Stored
call results are reused; a pending/uncertain provider call is never automatically
sent again. An interrupted compilation intent is retained for reconciliation,
without a free recompile. An admitted successor saved before replacement can be
published from its exact held checkpoint; an already committed replacement is
resolved through the lineage.

This example supports one server process per database/session. The fixed local
`alice-demo` / `bob-demo` bearer tokens illustrate server-side owner selection;
they are not production authentication. The HTTP listener binds loopback. Explicit
cancellation closes the family; it is different from a process-loss restart.

## Verify

```bash
python3 examples/cookbook/self_evolving_chatbot/test_program_chat.py \
  ./build-chat/cookbook_program_chatbot
# Run the same command with NEOGRAPH_CHAT_POSTGRES_URL for PostgreSQL.
```

The black-box suite covers two tenants concurrently, keep/swap, recursive reviewer
creation, idempotent requests, process restart, budget exhaustion, authorization
and a local HTTP fixture using the real provider adapter. The fixture does not
claim a successful external OpenRouter request.

For a reproducible terminal scenario, `--script scenario.json` accepts an array of
`{tenant, request_id, message, force_swap?}`. `--crash-after-script` intentionally
exits after committed snapshots without cancelling the family, for restart tests.

## Earlier Core examples

`server.cpp` / `server_multi.cpp` and their original CMake targets remain available.
They select a graph configuration for the next request. The new
`cookbook_program_chatbot` demonstrates Program generations and recursive child
synthesis; the older examples do not demonstrate live Program replacement.
