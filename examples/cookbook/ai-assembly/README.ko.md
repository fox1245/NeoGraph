<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=ko source_sha256=e1c5c9d708af4408691ca586bfbccaab38566f6f8c1f22894dc3d61bff8b68c2 -->
# AI 국회

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**현재 범위 한정 실행 증거.** 타입 제공자 전환 후 실제 로컬 A2A 멤버 서버
네 개와 C++ 의장이 오프라인 세션을 완료했습니다. 합성 기권은 fixture 출력이며
모델 판단이나 vendor 추론이 아닙니다. live 제공자 호출이나 유예된 Python
의장 binding을 검증하지 않습니다.

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
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-cookbook \
    -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

# 오프라인 fixture: .env 로드/provider 통신 없음
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh --mock
# live: 환경 또는 .env에서 키를 비공개 설정
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

typed SchemaProvider CMake 패키지와 빌드 의존성을 먼저 설치하세요. 통합 NeoGraph 타깃이며 standalone 프로젝트는 없습니다. `NEOGRAPH_BUILD_DIR`로 바이너리를 선택하며 미설정 시 `build-pybind`, `build`, recipe의 `build` 순으로 찾습니다. `--mock`는 모델 판단/사용량이 아닌 합성 기권입니다. live는 법안/페르소나 프롬프트를 OpenRouter로 보내며 유효한 키, 네트워크, 크레딧이 필요합니다. 네 멤버 호출은 비용을 발생시키며 고정 비용을 보장하지 않습니다. 키, `.env`, 프롬프트와 출력 기록을 비공개로 보관하고 raw native 기록을 공개하지 마세요. A2A에는 portable 응답만 전달합니다. C++은 typed `ProviderRequest`/SDK 이벤트와 완전한 불변 `sp::Outcome`을 사용하며 projection은 native replay 권한이 아닙니다. 소스 마이그레이션 기록이지 새 실행 검증이 아닙니다.

## Python 스피커 변형(v0.2.1+, 크로스 언어 A2A)

Python 바인딩은 이 전환에서 **deferred(연기)**입니다. `speaker.py`는 별도의 호환 `neograph_engine.a2a`가 필요하며 C++ 전환 완료가 이를 보장하지 않습니다. 아래는 역사적 사용 예입니다.

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

v0.2.1 바인딩은 역사적 릴리스 결과이지 현재 검증이 아닙니다. A2A wire client/protocol은 변경되지 않았습니다.

## 마찰 일지 — 새로운 NeoGraph 사용자가 걸려 넘어진 것


전환 이전 역사적 마찰 기록이며 현재 legacy API 지원 주장이 아닙니다. 위 live transcript도 역사적 기록입니다.

### 1. A2A는 C++ 전용이었습니다 — Python 바인딩이 이를 노출하지 않았습니다 (v0.2.1에서 수정됨)

과거 v0.2.1에서 Python A2A client가 추가되었습니다. 현재 바인딩은 연기되어 있으며 향후 릴리스 제공을 약속하지 않습니다.

### 2. 시스템 설치 없음 / 휠에 헤더 없음 (README v0.2.1에서 수정됨)

과거 README는 FetchContent를 설명했습니다. 이 recipe에는 통합 타깃만 있고 standalone CMake 프로젝트는 없습니다. SchemaProvider가 필요합니다.

### 3. `OpenAIProvider::create()` `unique_ptr` 대 `shared_ptr` (v0.2.1에서 수정됨)

과거 `create_shared`는 이전 소유권 문제를 해결했습니다. 현재는 `examples::make_openrouter_provider`와 typed outcome을 사용합니다.

### 4. `.env` 자동 로드가 A2A 자식 프로세스로 전파되지 않음 (v0.2.1에서 문서화됨)

`cppdotenv::auto_load_dotenv()`는 이를 호출하는 바이너리 내부에서 작동하지만, 자식 서버를 포크하는 실행 스크립트는 먼저 부모 셸에서 `source .env`를 수행해야 합니다. 이제 [`docs/troubleshooting.md`](../../../docs/troubleshooting.md)의 "Build from source(소스에서 빌드)" 항목에 문서화되었습니다.

### 5. 원활하게 작동한 부분 (긍정적 메모)

- `A2AServer::start_async` + 자동 포트 (`port=0`)는 문제없었습니다.
- 에이전트카드 발견(`fetch_agent_card`)이 방금 작동했습니다 — 수동 HTTP가 필요 없었습니다.
- 동시에 `send_message_sync`에서 `std::async` 퓨처를 처리합니다 — 클라이언트 측 잠금 없음, 공유 세션 상태 없음. A2A 사양 및 NeoGraph 모두 병렬 클라이언트 요청을 기본적으로 깔끔하게 처리합니다.
- `parse_vote` 자유 형식 한국어 텍스트의 정규 표현식은 모델이 요청 시 `vote: support/oppose/abstain`를 안정적으로 존중하기 때문에 작동합니다. 페르소나 출력이 형식 안에 유지되어 이는 5-라인 집계(해당) 함수가 되었습니다.
- 역사적 in-tree 빌드 경험입니다. 현재 조건은 위를 참조하세요.

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
