# AI National Assembly

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**Current scoped runtime evidence.** Four actual local A2A member servers and
the C++ Speaker completed an offline session after the typed-provider cutover.
Synthetic abstentions are fixture output, not model judgment or vendor inference.
This does not qualify live provider calls or the deferred Python Speaker binding.

A toy demo built **as a fresh NeoGraph user** — every API choice was
made by reading the public docs (README, examples on github, Doxygen)
without ever opening NeoGraph's source. The point is two-fold:
prove A2A works for a real multi-persona scenario, and surface the
friction a brand-new C++ developer hits along the way.

## What it does

Four members of the National Assembly sit on different ports,
each one an A2A endpoint backed by a distinct persona prompt and the
same OpenRouter route for the pinned DeepSeek model. The Speaker (Speaker of
the National Assembly) is a separate program that broadcasts a bill to every
member in parallel via NeoGraph's `A2AClient`, parses each member's vote out
of the reply, and declares the outcome.

```
                          ┌──────────────────┐
                          │  Speaker         │
                          │   A2AClient ×4   │
                          └─────────┬────────┘
                fetch_agent_card +    send_message_sync
            ┌──────────┬───────────┴───────────┬──────────┐
            ▼          ▼                       ▼          ▼
       :8101 Progress    :8102 Conservative  :8103 Center  :8104 Green
       Kim Jinbo         Park Bosu           Jung Jungdo   Na Noksaek
       (PersonaNode → OpenRouter DeepSeek, persona-specific system prompt)
```

Each member is a one-node NeoGraph (`__start__ → persona → __end__`)
served behind `a2a::A2AServer`. The graph reads a `prompt` channel and
writes a `response` channel; the A2A server's default
`GraphAgentAdapter` surfaces those over JSON-RPC.

## Live transcript (DeepSeek via OpenRouter, 2026-04-29)

Historical pre-cutover transcript; not current typed-provider execution evidence.

Bill: [`bills/basic_income.txt`](bills/basic_income.txt) — universal
basic income, 500,000 won/month, funded by land + carbon + progressive tax.

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

Each persona's reasoning genuinely tracks their party's stated values.
That's not the framework's doing — it is the pinned model following distinct
system prompts — but the assembly mechanics (parallel A2A, vote tally,
discovery) are pure NeoGraph.

## Build + run (in the NeoGraph tree)

```bash
# from NeoGraph repo root; A2A and LLM are optional build components
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-cookbook \
    -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

# Offline fixture: no .env loading, credentials or provider transport.
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh --mock
# Live: privately configure OPENROUTER_API_KEY in the environment or .env.
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

Install the typed SchemaProvider CMake package and build dependencies first.
This is an integrated build, not a standalone cookbook project.
`NEOGRAPH_BUILD_DIR` selects binaries; otherwise the launcher checks
`build-pybind`, `build`, then recipe-local `build`. `--mock` returns synthetic
abstentions, not model judgment or usage. Live calls send bill/persona prompts
to OpenRouter and require a valid key, network and provider credit; four members
incur model charges with no fixed cost guarantee. Keep keys, `.env`, prompts and
printed transcripts private; never publish raw native records. A2A publishes
portable response text only. C++ uses typed `ProviderRequest`/SDK events and full
immutable `sp::Outcome`; projection does not grant native replay authority.
The persona node participates in the existing runtime-interposition/provider-broker boundary and records the actual owned outcome once in the shared graph sink, including drained observer-error outcomes. Cancellation/deadlines are propagated; bounded calls require admitted model-limit facts rather than an input-byte estimate.
This documents source migration, not a new build/test/live qualification.

## Python speaker variant (v0.2.1+, cross-language A2A)

Python bindings are **deferred** for this cutover. `speaker.py` requires a
separately available compatible `neograph_engine.a2a`; the C++ migration does
not establish that prerequisite. The following is historical usage only:

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

The v0.2.1 binding was a historical release result, not current qualification.
A2A wire clients/protocol are unchanged.

## Friction journal — what a fresh NeoGraph user tripped over


Historical pre-cutover friction follows, not current legacy API support claims.

### 1. A2A was C++-only — Python binding didn't expose it (FIXED in v0.2.1)

Historically v0.2.1 added the Python A2A client. Current bindings are deferred;
no future release delivery is promised.

### 2. No system install / no headers in the wheel (FIXED in README v0.2.1)

Historically README described FetchContent. This recipe supplies integrated
NeoGraph targets only, no standalone CMake project. SchemaProvider is required.

### 3. `OpenAIProvider::create()` `unique_ptr` vs `shared_ptr` (FIXED in v0.2.1)

Historically `create_shared` addressed the old ownership friction. The current
recipe instead uses `examples::make_openrouter_provider` and typed outcomes.

### 4. `.env` autoload doesn't propagate to A2A child processes (DOCUMENTED in v0.2.1)

`cppdotenv::auto_load_dotenv()` works inside the binary that calls
it, but a launcher script forking child servers must `source .env`
in the parent shell first. Now documented in
[`docs/troubleshooting.md`](../../../docs/troubleshooting.md) under
"Build from source".

### 5. What worked smoothly (positive notes)

- `A2AServer::start_async` + auto-port (`port=0`) was painless.
- AgentCard discovery (`fetch_agent_card`) just worked — no manual
  HTTP needed.
- Concurrent `send_message_sync` from `std::async` futures — no
  client-side locking, no shared session state. The A2A spec /
  NeoGraph both handle parallel client requests cleanly out of the
  box.
- `parse_vote` regex on free-form Korean text works because the model
  reliably honors `vote: support/oppose/abstain` when asked. Persona output
  staying inside the format made this a 5-line tally function.
- Historical in-tree build experience; current prerequisites are listed above.

## Files

```
ai-assembly/
├── member_server.cpp           # one configurable persona server
├── speaker.cpp                 # orchestrator, broadcasts bill, tallies
├── speaker.py                  # Python A2A client variant
├── prompts/
│   ├── jinbo.txt               # Kim Jinbo (Progress)
│   ├── bosu.txt                # Park Bosu (Conservative)
│   ├── jungdo.txt              # Jung Jungdo (Center)
│   └── nokdang.txt             # Na Noksaek (Green)
├── bills/
│   └── basic_income.txt        # sample bill: National Basic Income Law
└── scripts/
    └── run_session.sh          # spin up 4 members + run speaker
```

## License

MIT, same as NeoGraph.
