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
export OPENROUTER_MODEL='your-provider/model-id'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
```

OpenRouter uses the existing cookbook's ZDR routing option. Choose a model with
an eligible route. No model or price is hardcoded. The application does not read `.env` files. Do not
put credentials in the DSL, database, HTTP body or source. Changing the provider,
model or build requires a new explicit `--session`; reopening a session retains
its stored budget limits. `NEOGRAPH_CHAT_BASE_URL` can select another compatible
endpoint; plain HTTP requires `--allow-loopback-provider` and a literal loopback
address, intended for protocol tests.

The native adapter sends the output limit as `max_completion_tokens`, supported
by the [OpenRouter chat API](https://openrouter.ai/docs/api/api-reference/chat/send-chat-completion-request).

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
