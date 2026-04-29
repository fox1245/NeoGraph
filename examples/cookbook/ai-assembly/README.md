# AI National Assembly (AI 국회)

A toy demo built **as a fresh NeoGraph user** — every API choice was
made by reading the public docs (README, examples on github, Doxygen)
without ever opening NeoGraph's source. The point is two-fold:
prove A2A works for a real multi-persona scenario, and surface the
friction a brand-new C++ developer hits along the way.

## What it does

Four 국회의원 (members of the National Assembly) sit on different ports,
each one an A2A endpoint backed by a distinct persona prompt and the
same OpenAI model (`gpt-5.4-mini`). The Speaker (의장) is a separate
program that broadcasts a bill to every member in parallel via
NeoGraph's `A2AClient`, parses each member's vote out of the reply, and
declares the outcome.

```
                          ┌──────────────────┐
                          │  의장 (speaker)  │
                          │   A2AClient ×4   │
                          └─────────┬────────┘
                fetch_agent_card +    send_message_sync
            ┌──────────┬───────────┴───────────┬──────────┐
            ▼          ▼                       ▼          ▼
       :8101 진보당  :8102 보수당          :8103 중도당  :8104 녹색당
       김진보        박보수                정중도        나녹색
       (PersonaNode → OpenAI gpt-5.4-mini, persona-specific system prompt)
```

Each member is a one-node NeoGraph (`__start__ → persona → __end__`)
served behind `a2a::A2AServer`. The graph reads a `prompt` channel and
writes a `response` channel; the A2A server's default
`GraphAgentAdapter` surfaces those over JSON-RPC.

## Live transcript (gpt-5.4-mini, 2026-04-29)

Bill: [`bills/basic_income.txt`](bills/basic_income.txt) — universal
basic income, 50만원/month, funded by land + carbon + progressive tax.

```
[국회의장] 의안 상정: [국민기본소득법]

[진보당 김진보]   사회적 약자 보호 + 자산·탄소 과세 = 부합        → 찬성
[보수당 박보수]   200조 의무지출 + 시장 왜곡 + 부동산 충격         → 반대
[중도당 정중도]   취지 인정하나 금액 과다, 단계 축소 수정안 제안   → 반대
[녹색당 나녹색]   탄소세 + 불로소득 과세 + 분배 정의               → 찬성

[국회의장] 표결 결과:  찬성 2  /  반대 2  /  기권 0
[국회의장] 찬반 동수입니다 — 본 법안은 부결됩니다 (관례).
```

Each persona's reasoning genuinely tracks their party's stated values.
That's not the framework's doing — it's just OpenAI honoring distinct
system prompts — but the assembly mechanics (parallel A2A, vote tally,
discovery) are pure NeoGraph.

## Build + run (in NeoGraph tree)

```bash
# from NeoGraph repo root
cmake --build build-pybind --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENAI_API_KEY=sk-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

## Python speaker variant (v0.2.1+, cross-language A2A)

The same speaker logic, in ~100 lines of Python, against the same
C++ member servers — proves the A2A protocol bridges languages
cleanly:

```bash
pip install neograph-engine          # >= 0.2.1
# (start the C++ members in another terminal as above)
PYTHONPATH=build-pybind python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

The Python A2A binding (`neograph_engine.a2a`) ships in v0.2.1.
Server side (graph-as-A2A-endpoint) stays C++-only for now.

## Friction journal — what a fresh NeoGraph user tripped over

These are the rough edges discovered while building this. **All four
were fixed in v0.2.1** — left here as a record.

### 1. A2A was C++-only — Python binding didn't expose it (FIXED in v0.2.1)

`pip install neograph-engine` works, but pre-v0.2.1's `neograph_engine`
didn't export `A2AClient` / `AgentCard`. v0.2.1 adds the
`neograph_engine.a2a` submodule (client + AgentCard + Task/Message/
Part/TaskState/Role) — see the Python speaker variant above.

**Server-side binding is still C++-only**; A2AServer needs a
GIL-aware lifecycle contract that's a follow-up for v0.3.

### 2. No system install / no headers in the wheel (FIXED in README v0.2.1)

The README now has a "Using NeoGraph from your CMake project" section
showing the `FetchContent_Declare` pattern. This cookbook also lives
inside the NeoGraph tree so it can `add_executable` directly without
any external dependency — the standalone variant uses FetchContent.

### 3. `OpenAIProvider::create()` `unique_ptr` vs `shared_ptr` (FIXED in v0.2.1)

`OpenAIProvider::create_shared(cfg)` was added — returns
`shared_ptr<Provider>` directly so it captures cleanly into
`NodeFactory` closures. The cookbook uses it on line ~133 of
`member_server.cpp`.

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
  reliably honors `투표: 찬성/반대/기권` when asked. Persona output
  staying inside the format made this a 5-line tally function.
- Build was clean — FetchContent pulled v0.2.0, no manual dep
  installation. OpenSSL/CURL on a stock Ubuntu was enough.

## Files

```
ai-national-assembly/
├── CMakeLists.txt              # FetchContent NeoGraph v0.2.0
├── src/
│   ├── member_server.cpp       # one binary, configurable persona
│   └── speaker.cpp             # orchestrator, broadcasts bill, tallies
├── prompts/
│   ├── jinbo.txt               # 진보당 김진보
│   ├── bosu.txt                # 보수당 박보수
│   ├── jungdo.txt              # 중도당 정중도
│   └── nokdang.txt             # 녹색당 나녹색
├── bills/
│   └── basic_income.txt        # sample bill: 국민기본소득법
└── scripts/
    └── run_session.sh          # spin up 4 members + run speaker
```

## License

MIT, same as NeoGraph.
