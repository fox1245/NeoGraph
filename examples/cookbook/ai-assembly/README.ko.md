<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=ko source_sha256=4fc02b6c921618283b005ec1a6e8819e815c28840f34ad72b347d1ad86ff4e4b -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# AI 국회


**새로운 NeoGraph 사용자**로 구축된 장난감 데모 — 모든 API 선택은
공개 문서(README, github의 예제, Doxygen)를 읽고 작성했습니다.
NeoGraph의 소스를 열지 않고도 말이죠. 요점은 두 가지입니다.
A2A가 실제 다중 인물 시나리오에서 작동함을 증명하고
새로운 C++ 개발자가 그 과정에서 부딪히는 마찰.

## 기능

네 명의 국회의원이 서로 다른 항구에 앉아 있는데,
각각은 고유한 페르소나 프롬프트로 지원되는 A2A 엔드포인트이며
동일한 OpenAI 모델(`gpt-5.4-mini`). 국회의장(국회의장)은 별도이다.
모든 회원에게 동시에 법안을 방송하는 프로그램
NeoGraph의 `A2AClient`는 응답에서 각 회원의 투표를 분석하고,
결과를 선언합니다.

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
       (PersonaNode → OpenAI gpt-5.4-mini, persona-specific system prompt)
```

각 구성원은 1노드 NeoGraph(`__start__ → persona → __end__`)입니다.
`a2a::A2AServer` 뒤에서 제공됩니다. 그래프는 `prompt` 채널을 읽고
`response` 채널을 작성합니다. A2A 서버의 기본값
`GraphAgentAdapter`는 JSON-RPC 이상의 항목을 표면화합니다.

## 실시간 대본(gpt-5.4-mini, 2026-04-29)

청구서: [`bills/basic_income.txt`](bills/basic_income.txt) — 범용
기본소득, 500,000 won/month, 토지 + 탄소 + 누진세로 자금 지원.

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

각 페르소나의 추론은 해당 당사자가 명시한 가치를 진정으로 추적합니다.
그것은 프레임워크가 하는 일이 아닙니다. 단지 OpenAI가 고유한 특성을 존중하는 것뿐입니다.
시스템 프롬프트 — 그러나 조립 메커니즘(병렬 A2A, 투표 집계,
발견)은 순수한 NeoGraph입니다.

## 빌드 + 실행(NeoGraph 트리에서)

```bash
# from NeoGraph repo root; A2A and LLM are optional build components
cmake -S . -B build-cookbook \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENAI_API_KEY=sk-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

멤버 서버는 실제 OpenAI 호출을 하므로 `OPENAI_API_KEY`와 네트워크가
필요합니다. 컴파일 자체는 오프라인으로 검증할 수 있습니다.

## Python 스피커 변형(v0.2.1+, 교차 언어 A2A)

동일한 C++ 멤버 서버에 연결하는 약 100줄의 Python 스피커입니다.

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

Python A2A 바인딩(`neograph_engine.a2a`)은 v0.2.1에 제공됩니다.
서버 측(graph-as-A2A-endpoint)은 현재 C++ 전용으로 유지됩니다.

## 마찰 저널 — 새로운 NeoGraph 사용자가 넘어진 것

### 1. A2A는 C++ 전용이었습니다. Python 바인딩에서는 이를 노출하지 않았습니다(v0.2.1의 FIXED).

`pip install neograph-engine`는 작동하지만 v0.2.1 이전의 `neograph_engine`
`A2AClient` / `AgentCard`를 내보내지 않았습니다. v0.2.1은
`neograph_engine.a2a` 하위 모듈(클라이언트 + AgentCard + Task/Message/
Part/TaskState/Role) — 위의 Python 스피커 변형을 참조하세요.

**서버측 바인딩은 여전히 ​​C++ 전용입니다**; A2A서버에는
v0.3의 후속 조치인 GIL 인식 수명 주기 계약입니다.

### 2. 시스템 설치 없음/휠 헤더 없음(README v0.2.1의 FIXED)

이제 README에는 "CMake 프로젝트에서 NeoGraph 사용" 섹션이 있습니다.
`FetchContent_Declare` 패턴을 보여줍니다. 이 요리책도 살아있어요
NeoGraph 트리 내부에서 `add_executable`를 직접 사용하지 않고
모든 외부 종속성 - 독립형 변형은 FetchContent를 사용합니다.

### 3. `OpenAIProvider::create()` `unique_ptr` 대 `shared_ptr`(v0.2.1의 FIXED)

`OpenAIProvider::create_shared(cfg)`가 추가되었습니다 — 반환
`shared_ptr<Provider>`를 직접 사용하여 깔끔하게 캡처합니다.
`NodeFactory` 폐쇄. 요리책은 ~133행에서 그것을 사용합니다.
`member_server.cpp`.

### 4. `.env` 자동 로드는 A2A 하위 프로세스(v0.2.1의 DOCUMENTED)에 전파되지 않습니다.

`cppdotenv::auto_load_dotenv()`는 호출하는 바이너리 내부에서 작동합니다.
하지만 하위 서버를 분기하는 실행 프로그램 스크립트는 `source .env`를 사용해야 합니다.
먼저 상위 쉘에서. 이제 문서화되었습니다.
아래의 [`docs/troubleshooting.md`](../../../docs/troubleshooting.md)
"소스에서 빌드".

### 5. 순조롭게 진행된 점(긍정적인 메모)

- `A2AServer::start_async` + 자동 포트(`port=0`)는 고통스럽지 않았습니다.
- AgentCard 검색(`fetch_agent_card`)이 방금 작동했습니다. 수동 없음
HTTP가 필요합니다.
- `std::async` 선물의 동시 `send_message_sync` - 아니요
클라이언트 측 잠금, 공유 세션 상태 없음. A2A 사양 /
NeoGraph는 둘 다 병렬 클라이언트 요청을 깔끔하게 처리합니다.
상자.
- 인트리 CMake 빌드는 자체 포함되어 있습니다. 위와 같이
  `NEOGRAPH_BUILD_A2A=ON`, `NEOGRAPH_BUILD_LLM=ON`으로 구성하세요.
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

## 특허

MIT, NeoGraph와 동일합니다.
