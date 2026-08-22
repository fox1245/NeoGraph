<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=ko source_sha256=4922ec93b98cf57b8a7fc967e471974122e6b7608a53fc5f1b826cb01f3fd9b8 -->
# AI 국회

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

신규 NeoGraph 사용자로서 만든 장난감 데모입니다. 모든 API 선택은 NeoGraph의 소스를 열어보지 않고 공개 문서(README, GitHub의 예제, Doxygen)를 읽는 방식으로 이루어졌습니다. 그 목적은 두 가지입니다. A2A가 실제 다중 페르소나 시나리오에서 작동함을 입증하고, 새로운 C++ 개발자가 그 과정에서 겪는 마찰을 드러내는 것입니다.

## 기능

국회의원 네 명이 서로 다른 포트에 배치되어 있으며, 각 포트는 고유한 페르소나 프롬프트와 고정된 DeepSeek 모델을 위한 동일한 OpenRouter 경로로 지원되는 A2A 엔드포인트입니다. 국회의장은 NeoGraph의 `A2AClient`를 통해 모든 의원에게 법안을 병렬로 브로드캐스트하고, 각 의원의 투표를 답변에서 파싱하여 결과를 선언하는 별도의 프로그램입니다.

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

각 멤버는 `__start__ → persona → __end__` 뒤에서 제공되는 단일 노드 NeoGraph(`a2a::A2AServer`)입니다. 그래프는 `prompt` 채널을 읽고 `response` 채널에 씁니다. A2A 서버의 기본 `GraphAgentAdapter`는 이를 JSON-RPC로 표면화합니다.

## ⟦14b8b266c2a6⟧

Bill: [`bills/basic_income.txt`](bills/basic_income.txt) — 기본소득, 월 50만 원, 토지세 + 탄소세 + 누진세로 재원을 마련합니다.

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

각 페르소나의 추론은 실제로 해당 정당의 명시된 가치를 추적합니다. 이는 프레임워크의 소행이 아니라 고정된 모델이 서로 다른 시스템 프롬프트를 따르는 것입니다. 그러나 의회 메커니즘(병렬 A2A, 투표 집계, 발견)은 순수한 NeoGraph입니다.

## 빌드 + 실행 (NeoGraph 트리 내에서)

```bash
# from NeoGraph repo root; A2A and LLM are optional build components
cmake -S . -B build-cookbook \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENROUTER_API_KEY=sk-or-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

회원 서버는 실시간 OpenRouter 호출을 수행합니다; `OPENROUTER_API_KEY` 및 네트워크 액세스가 필요합니다. 컴파일 자체는 오프라인입니다.

## Python 스피커 변형(v0.2.1+, 크로스 언어 A2A)

동일한 스피커 로직을 Python으로 작성, 동일한 C++ 멤버 서버에 대하여 ~100줄의 Python로 구성되어 A2A Protocol이 언어 간 깔끔하게 연결함을 입증합니다:

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

파이썬 A2A 바인딩(`neograph_engine.a2a`)은 v0.2.1에 포함됩니다. 서버 측(graph-as-A2A-endpoint)은 현재 C++ 전용으로 유지됩니다.

## 마찰 일지 — 새로운 NeoGraph 사용자가 걸려 넘어진 것


이것은 이를 구축하면서 발견된 거친 부분입니다. **네 가지 모두 v0.2.1에서 수정되었습니다** — 기록으로 여기에 남겨두었습니다.

### 1. A2A는 C++ 전용이었습니다 — Python 바인딩이 이를 노출하지 않았습니다 (v0.2.1에서 수정됨)

`pip install neograph-engine` 작동하지만, v0.2.1 이전 버전의 `neograph_engine` 는 내보내지 않았습니다 `A2AClient` / `AgentCard`. v0.2.1은 `neograph_engine.a2a` 서브모듈(client + AgentCard + Task/Message/ Part/TaskState/Role)을 추가합니다. — 위의 Python 스피커 변형을 참조하세요.

**서버 측 바인딩은 여전히 C++ 전용입니다**; A2AServer에는 v0.3에서 후속 작업으로 진행될 GIL 인식 수명 주기 계약이 필요합니다.

### 2. 시스템 설치 없음 / 휠에 헤더 없음 (README v0.2.1에서 수정됨)

README에 이제 "CMake 프로젝트에서 NeoGraph 사용하기" 섹션이 있으며 `FetchContent_Declare` 패턴을 보여줍니다. 이 쿡북은 NeoGraph 트리 내부에도 있으므로 외부 종속성 없이 `add_executable` 직접 사용할 수 있습니다. 스탠드얼론 변형은 FetchContent를 사용합니다.

### 3. `OpenAIProvider::create()` `unique_ptr` 대 `shared_ptr` (v0.2.1에서 수정됨)

`OpenAIProvider::create_shared(cfg)` 추가됨 — `shared_ptr<Provider>`를 직접 반환하여 `NodeFactory` 클로저로 깔끔하게 캡처됩니다. 쿡북은 `member_server.cpp`의 ~133행에서 사용합니다.

### 4. `.env` 자동 로드가 A2A 자식 프로세스로 전파되지 않음 (v0.2.1에서 문서화됨)

`cppdotenv::auto_load_dotenv()`는 이를 호출하는 바이너리 내부에서 작동하지만, 자식 서버를 포크하는 실행 스크립트는 먼저 부모 셸에서 `source .env`를 수행해야 합니다. 이제 [`docs/troubleshooting.md`](../../../docs/troubleshooting.md)의 "Build from source(소스에서 빌드)" 항목에 문서화되었습니다.

### 5. 원활하게 작동한 부분 (긍정적 메모)

- `A2AServer::start_async` + 자동 포트 (`port=0`)는 문제없었습니다.
- 에이전트카드 발견(`fetch_agent_card`)이 방금 작동했습니다 — 수동 HTTP가 필요 없었습니다.
- 동시에 `send_message_sync`에서 `std::async` 퓨처를 처리합니다 — 클라이언트 측 잠금 없음, 공유 세션 상태 없음. A2A 사양 및 NeoGraph 모두 병렬 클라이언트 요청을 기본적으로 깔끔하게 처리합니다.
- `parse_vote` 자유 형식 한국어 텍스트의 정규 표현식은 모델이 요청 시 `vote: support/oppose/abstain`를 안정적으로 존중하기 때문에 작동합니다. 페르소나 출력이 형식 안에 유지되어 이는 5-라인 집계(해당) 함수가 되었습니다.
- 인트리 CMake 빌드는 자체적으로 완결됩니다. 다음을 사용하여 구성하십시오: `NEOGRAPH_BUILD_A2A=ON` 및 `NEOGRAPH_BUILD_LLM=ON` 위에 표시된 대로.

## 파일

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

## 라이선스(License)

MIT, NeoGraph와 동일합니다.
