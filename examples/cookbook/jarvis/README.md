# JARVIS — 음성으로 말 거는 메타 오케스트레이터

> 클라우드 0개 의존, 라즈베리파이 한 대에서 돌아가는 자비스.
> 마이크가 토니, NeoGraph 가 자비스, 도구·전문가는 자비스의 부하들.

이 cookbook 은 "음성 TTS 예제" 가 아닙니다. NeoGraph 의 멀티에이전트
프리미티브 — MCP 도구, A2A 양방향, 비동기 병렬, Store 메모리, ReAct
서브그래프 — 를 **음성 한 줄로 묶어내는** 한 개의 시연입니다.

## 이게 왜 자비스인가

영화 속 자비스가 "음성 TTS 가 있는 챗봇" 이어서 자비스가 아닙니다.
자비스는 이 다섯 가지를 동시에 합니다:

1. 토니가 말 끝나기 전에 의도를 잡는다 — **빠른 의도 분류**
2. 자기가 직접 답할 수 있으면 답하고, 아니면 부하한테 시킨다 — **4-way 라우팅**
3. 동시에 여러 정보를 모은다 — **병렬 fan-out**
4. 어제 한 얘기를 기억한다 — **장기 메모리**
5. 다른 자비스/시스템에서 자비스를 부를 수 있다 — **A2A 양방향**

그래서 이 cookbook 의 핵심은 **그래프 모양** 이지 음성이 아닙니다.
음성은 입출력 껍데기일 뿐이고, 안에 들어가는 NeoGraph 의 오케스트레이션이
"자비스 느낌" 을 만듭니다.

## 전체 그래프

```
                          ┌────────────────────────┐
                          │ 백그라운드 트리거       │
                          │ (타이머 / 외부 이벤트)  │  ── A2A 서버로
                          └───────────┬────────────┘     자비스 호출도 여기로
                                      │
[마이크]──[VAD]──[whisper.cpp STT]──[memory_lookup]──[intent_router]
   miniaudio                          ▲                   │
                                      │ Store             │
                                      │ (대화 누적)        │
                                      │                   │ 라우터가 4-way 결정
                                      │                   │ (chat 은 합성기 직행)
                                      │                   │
                          ┌───────────┴───────────────────┴───────────────┐
                          │                                                │
                  [direct_branch]        [delegate_branch]        [parallel_branch]
                       │                       │                       │
              MCP 도구 1회 호출        A2A 로 전문가에게 통째       Send / fan-out
              (시간, 날씨, 메모 등)    위임 (코더, 연구자, ...)     으로 여러 도구 동시
                       │                       │                       │
                       └───────────────────────┼───────────────────────┘
                                               │
                                       [response_synth]
                                       (큰 LLM 으로 자연 응답 합성)
                                               │
                                               ↓
                                  [supertonic TTS] ──→ [스피커]
                                  (감지된 언어 그대로)     miniaudio
```

## 두 개의 카탈로그 JSON — 자비스의 "내가 뭘 할 수 있는지"

자비스가 시작될 때 두 파일을 읽고 능력 목록을 구성합니다.
**코드 재컴파일 없이 능력을 추가/제거할 수 있다는 뜻** 입니다.

### `config/mcp_catalog.json` — 도구

자비스가 직접 호출할 수 있는 함수형 도구 목록.
각 항목은 MCP 서버 한 개에 대응 (HTTP 또는 stdio).

```json
{
  "tools": [
    {
      "name": "time_weather",
      "transport": "http",
      "url": "http://127.0.0.1:8000",
      "description": "현재 시간, 날씨, 환율 같은 짧은 즉답성 정보",
      "enabled": true
    },
    {
      "name": "personal_memo",
      "transport": "stdio",
      "command": ["python3", "examples/demo_mcp_stdio_server.py"],
      "description": "토니의 개인 메모 저장/검색",
      "enabled": true
    }
  ]
}
```

시작 시 각 MCP 서버에서 `get_tools()` 호출 → 도구 정의 합쳐서
라우터의 시스템 프롬프트에 "사용 가능한 도구" 로 주입.

### `config/agent_registry.json` — 전문가 (A2A)

자비스가 통째로 위임할 수 있는 서브에이전트들. 각각 별도 프로세스/머신
에서 도는 A2A 엔드포인트.

```json
{
  "agents": [
    {
      "name": "coder",
      "url": "http://127.0.0.1:8210",
      "expertise": "코드 작성, 리뷰, 디버깅",
      "fetch_card_on_start": true
    },
    {
      "name": "researcher",
      "url": "http://127.0.0.1:8211",
      "expertise": "웹 검색 + 요약, 학술 자료 정리",
      "fetch_card_on_start": true
    }
  ]
}
```

시작 시 각 URL 에 `AgentCard` 요청 → 응답하는 놈만 활성화.
**중요한 트릭**: A2A 표준 따르는 어떤 외부 에이전트라도 — 다른 사람이
만든 Python A2A 봇이든, 또 다른 NeoGraph 인스턴스든 — 이 JSON 에
URL 만 추가하면 자비스의 부하가 됩니다.

## 라우터 (의도 분류) — 자비스의 두뇌

작은/빠른 LLM (예: `gpt-4o-mini` 또는 로컬 `llama-3.2-1b`) 한 번 호출로
다음 JSON 을 받습니다.

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — 도구도 위임도 없이 합성기가 자기 지식 + 대화 기억으로 직접 답변.
  인사, 자기소개, 잡담, "아까 뭐라고 했지" 류 대화 회상. 라우터가 카탈로그에
  없는 도구/에이전트를 발명하면 검증 단계에서 이 모드로 강등된다.
- `direct` — 도구 한 번. 결과가 단순하면 (`"15시 30분"`) `skip_synthesis=true`
  로 응답 합성도 생략하고 TTS 직행. **빠르다.**
- `delegate` — `delegate_to` 가 가리키는 A2A 엔드포인트로 통째로 던짐.
  결과 받아서 음성용 한 줄 요약만 합성.
- `parallel` — `tool_calls` 가 여러 개. NeoGraph 의 `make_parallel_group`
  으로 동시 실행, reducer 가 결과 합쳐서 응답 합성.

### 왜 라우터 / 합성기를 나누는가

전부 한 큰 LLM 으로 ReAct 돌리면 매 턴 1-3초씩 걸려서 자비스 느낌이 안 납니다.
- 라우터: 작은 모델, ~200ms, JSON 한 번
- 합성기: 큰 모델, ~800-1500ms, 자연어 한 번
- 도구가 즉답성이면 합성기 생략 → ~500ms 안에 응답 시작

영화 자비스가 토니 말 끝나자마자 답하는 그 텀포는 이 분리에서 나옵니다.

## 메모리 (`Store`)

매 턴 시작 시 `memory_lookup` 노드가 NeoGraph `Store` 에서 최근 N턴 +
사용자 선호 (`tony.prefers.language=ko`, `tony.last_topic=...`) 끌어옴.

매 턴 종료 시 자비스 응답 + 토니 발화 + 사용된 도구를 Store 에 push.
다음 턴 라우터가 "아까 그거" 같은 지칭 해소 가능.

## A2A 양방향 — 자비스가 부르고 자비스가 불린다

- **부르기**: `agent_registry.json` 의 전문가들에게 `A2AClient` 로 위임.
- **불리기**: 자비스 자체도 `A2AServer` (포트 8200) 띄움.
  - 다른 시스템에서 `POST /v1/messages` 로 자비스에 텍스트 메시지 보낼 수 있음.
  - 휴대폰 앱, 다른 NeoGraph 인스턴스, 심지어 또 다른 자비스가 호출 가능.
  - 텍스트 입력은 마이크/STT 단계를 건너뛰고 라우터로 직행.

**자비스끼리 통신 시연**: 토니 집의 자비스(8200) ↔ 회사 자비스(8201).
"회사 자비스한테 오늘 회의록 받아와" → 집 자비스가 회사 자비스에 A2A
호출 → 응답을 토니에게 음성으로.

## 백그라운드 트리거 (proactive)

별도 비동기 그래프가 백그라운드에서 돌면서:
- 타이머 (5분마다 캘린더 체크)
- 외부 이벤트 (집안 센서, 메일 수신)
- 외부 A2A 호출

이벤트 발생 시 자비스 메인 그래프에 메시지 주입 → 토니가 묻기 전에
자비스가 먼저 말함. ("Sir, 회의 10분 전입니다.")

NeoGraph 의 `27_async_concurrent_runs.cpp` 패턴 그대로 사용.

## 디렉토리 구성

```
jarvis/
├── README.md                      ← 지금 이 문서
├── CMakeLists.txt                 모든 외부 의존(whisper/onnxruntime/miniaudio) gated
├── config/
│   ├── mcp_catalog.json           도구 카탈로그
│   ├── agent_registry.json        A2A 서브에이전트 레지스트리
│   ├── jarvis_graph.json          자비스 메인 그래프 정의 (NeoGraph JSON)
│   └── persona.txt                시스템 프롬프트 (라우터 + 합성기 공유)
├── src/
│   ├── main.cpp                   진입점
│   ├── audio/                     miniaudio 마이크 입력 + 스피커 출력
│   ├── stt/                       whisper.cpp 래퍼 (auto 언어 감지)
│   ├── orchestrator/              라우터, MCP 카탈로그 로더, A2A 디스패처
│   └── memory/                    Store 기반 대화 메모리
├── specialists/
│   ├── coder/                     별도 A2A 서버 (NeoGraph 서브그래프)
│   └── researcher/                별도 A2A 서버
├── assets/
│   ├── download.sh                whisper + supertonic 모델 다운로드
│   └── voices/                    supertonic voice style JSON
├── scripts/
│   └── run_session.sh             전체 시연 실행 (specialists 다 띄우고 자비스 띄움)
└── docs/
    └── architecture.md            그래프 노드별 상세 설명
```

## 빌드 / 실행 (예정)

```bash
# 1. 모델 다운로드 (~250MB: whisper-small + supertonic)
bash examples/cookbook/jarvis/assets/download.sh

# 2. 빌드 — onnxruntime, whisper.cpp, miniaudio 시스템에서 찾아짐
cmake -B build -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build --target cookbook_jarvis -j

# 3. 시연 — 백그라운드로 전문가들 띄우고, 자비스 띄움
bash examples/cookbook/jarvis/scripts/run_session.sh
```

## 현재 상태

이 디렉토리는 **골격(skeleton)** 입니다. 컴파일 안 됩니다.
다음 단계에서 채워질 것:

- [ ] supertonic 통합 (`src/audio/tts_node.{h,cpp}`)
- [ ] whisper.cpp + VAD 통합 (`src/audio/mic_input.cpp`, `src/stt/whisper_node.cpp`)
- [ ] MCP 카탈로그 로더 (`src/orchestrator/mcp_catalog.cpp`)
- [ ] A2A 디스패처 (`src/orchestrator/agent_dispatcher.cpp`)
- [ ] intent_router 노드 (`src/orchestrator/intent_router.cpp`)
- [ ] response_synth 노드
- [ ] specialists 두 개 (coder, researcher) — `examples/cookbook/ai-assembly/member_server.cpp` 패턴 재사용
- [ ] CMake 게이팅 + 모델 다운로드 스크립트
- [ ] 시연 영상

설계 리뷰가 끝나면 `/team` 또는 `/autopilot` 으로 위 체크리스트를 한 번에 채울 예정.

## 라이선스 / 외부 의존성

| 라이브러리 | 라이선스 | 역할 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS (99M params, ONNX, CPU) |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT (자동 언어 감지) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / public domain | 마이크 입력 + 스피커 출력 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 발화 끝 감지 (ONNX) |
| ONNX Runtime | MIT | supertonic + VAD 추론 |

전부 MIT 계열 — NeoGraph 의 `THIRD_PARTY_LICENSES.md` 에 추가 예정.
