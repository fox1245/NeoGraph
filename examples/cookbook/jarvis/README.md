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
다음 턴 라우터가 "아까 그거" 같은 지칭 해소 가능. `JsonFileStore` 로
파일 영속 — 재시작해도 기억 유지. 빈 턴(STT 실패·노이즈)은 커밋에서
제외해 기억 오염 방지. `prefs.native_lang` 에 추정 네이티브 언어를 유지
(언어 관성).

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
├── CMakeLists.txt                 외부 의존(whisper/onnxruntime/miniaudio) gated
├── config/                        기본 config (그래프·카탈로그·레지스트리·persona)
├── config-demo/                   실행용 프리셋 (real-tools / mock)
├── config-bench*/                 벤치용 config
├── src/
│   ├── main.cpp                   진입점 (노드 등록·그래프 컴파일·메인 루프)
│   ├── audio/                     miniaudio 캡처(+Silero VAD)·재생, supertonic TTS
│   ├── stt/                       whisper_node(다국어·언어관성) + moonshine_node(엣지)
│   ├── orchestrator/              라우터, MCP 카탈로그 로더, A2A 디스패처
│   └── memory/                    Store 기반 대화 메모리(JsonFileStore 영속)
├── specialists/                   coder / researcher (별도 A2A 서버)
├── bench/                         NeoGraph vs LangGraph 벤치 (쌍둥이·드라이버·Docker)
├── assets/download.sh             whisper/supertonic/moonshine/silero 모델 다운로드
├── scripts/
│   ├── run_jarvis.sh              실행 wrapper (LD_LIBRARY_PATH·ROCm·dxg 자동)
│   ├── jarvis_repl.py             한글 readline REPL (텍스트/wav 입력)
│   ├── build_whisper_hip.sh       whisper.cpp ROCm/HIP GPU 빌드
│   └── demo_mcp_server.py         데모 MCP 서버 (시간/날씨/계산)
└── docs/architecture.md          그래프 노드별 상세 설명
```

## 빌드 / 실행

```bash
# 1. 모델 다운로드 (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    경량: JARVIS_WHISPER=small bash assets/download.sh  (라즈베리파이/CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. 빌드 — onnxruntime, whisper.cpp, miniaudio 시스템에서 찾아짐(없으면 mock)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. 실행 — 텍스트/wav 입력 (한글 라인편집 REPL 권장)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # .env 의 OPENAI_API_KEY 자동 로드
#   토니 ▸ 안녕?                                # 텍스트
#   토니 ▸ wav:/경로/음성.wav                    # 오디오 파일 → STT

# 3b. 실행 — 라이브 마이크 (miniaudio 캡처 + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "온라인" 뜬 뒤 말하면 → 발화 끝 감지 → STT → 응답 → TTS

# (도구 데모용 MCP 서버 — 별도 터미널)
python3 scripts/demo_mcp_server.py 8888        # 시간/날씨/계산
```

LLM 프로바이더는 `.env` 의 `OPENAI_API_KEY`(OpenAI 직결) 또는
`OPENAI_BASE_URL`+`JARVIS_ROUTER_MODEL`/`JARVIS_SYNTH_MODEL`(Groq/Cerebras 등
OpenAI 호환) 으로 선택. 없으면 MockProvider 로 오프라인 동작(에코).

## 음성 스택 세부

### 라이브 마이크 (miniaudio + Silero VAD)
`JARVIS_MIC=1` 또는 config `use_microphone:true`. 캡처 워커 스레드가 512샘플
윈도우로 Silero VAD 를 돌려 발화 시작/끝을 감지(200ms 프리롤, 500ms 무음 종료).
**백프레셔**: 추론 중 캡처를 폐기해 TTS 에코·스테일 발화·시작 노이즈를 차단.
디바이스 실패(WSL2 마이크 미연결 등) 시 stdin 자동 폴백. 튜닝:
`JARVIS_VAD_THRESHOLD`(기본 0.5), 관측: `JARVIS_MIC_DEBUG=1`.

### STT — 두 가지 옵션 (config 의 `stt.type` 으로 스왑)
- **`whisper_stt`** (기본): whisper.cpp. `language:"auto"` 로 99개 언어 자동
  감지 → **화자 언어 그대로 응답·TTS**. **언어 관성**: store.prefs 에
  네이티브 언어를 유지해, 짧은 발화가 외국어로 오인식돼도 홱 바뀌지 않고
  고수(연속 오인식이어야 전환).
- **`moonshine_stt`**: Moonshine-tiny ONNX(27M, supertonic 과 ORT 공유).
  엣지·저지연·한국어 flavor. 언어별 모델이라 lang 고정.

### GPU 가속 (whisper.cpp ROCm/HIP)
번들 whisper.cpp 는 CPU 전용 — large 가 CPU 로 ~32초(11초 클립). AMD GPU
(gfx1201=R9700, ROCm≥7.2)면 `bash scripts/build_whisper_hip.sh` 로 GGML_HIP
빌드 → **~7초(4.5×)**. run_jarvis.sh 가 ROCm 런타임·WSL dxg 를 자동 로드.

## 벤치 — NeoGraph vs LangGraph (`bench/`)

동일 토폴로지를 LangGraph 로 미러링해 프레임워크 오버헤드를 실측
(`bench/README.md`). 동일 제약 컨테이너, 4층위(mock/E2E/nginx 경계계측/
스트리밍 TTFT/파이썬-pybind). 요지: 그래프 오버헤드 0.38 vs 3.07ms/턴,
기동 40ms vs ~3s, RSS 36 vs 561MB — 단 클라우드 LLM 턴 레이턴시는 공급자
분산이 지배(경계계측으로 분리). `GROQ_API_KEY=... bash bench/run_bench.sh`.

## 구현 상태

**실기동 완료** — 실기기에서 라이브 음성 한 턴이 도는 것을 검증(실LLM Groq).
mic→VAD→STT→라우터→4-way→합성→TTS 전 구간 + 메모리 영속 + A2A self-server.

알려진 한계 / 다음 버전:
- **barge-in 미지원** — TTS 재생 중 발화는 백프레셔로 폐기(v2 에서 cancel
  token 도입).
- **스트리밍 STT 미적용** — 발화 완성 후 배치 전사. Moonshine v2 ergodic
  encoder 로 청크 단위 스트리밍이 다음 후보.
- **다중 화자·장기 메모리 압축** — 단일 화자 가정, turns 24개 상한.
- **백그라운드 트리거(proactive)** — 설계만 있고 미구현.

## 라이선스 / 외부 의존성

| 라이브러리 | 라이선스 | 역할 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS (99M, ONNX, 31개 언어) |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT (99개 언어 자동감지, CPU/ROCm) |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | 엣지 STT 옵션 (27M ONNX) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / public domain | 마이크 캡처 + 스피커 재생 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 발화 시작/끝 감지 (ONNX) |
| ONNX Runtime | MIT | supertonic·moonshine·VAD 추론 |

전부 MIT 계열 — NeoGraph 의 `THIRD_PARTY_LICENSES.md` 에 추가 예정.
