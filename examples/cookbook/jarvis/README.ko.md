<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=ko source_sha256=19709bb07c36265ac28bd757009fd525505ef7bcd930e8d1b5ee6b763c4ff454 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# JARVIS — 음성 기반 메타 오케스트레이터


> 클라우드 제로 종속성은 단일 Raspberry Pi에서 실행됩니다.
> 마이크는 Tony, NeoGraph는 JARVIS, tools/experts는 JARVIS의 부하입니다.

이 요리책은 "음성 TTS 예시"가 **아닙니다**. NeoGraph의 시연입니다.
다중 에이전트 기본 요소 — MCP 도구, 양방향 A2A, 비동기 병렬, 메모리 저장,
ReAct 하위 그래프 — **한 줄의 음성으로 엮여 있습니다**.

## 이것이 JARVIS인 이유

영화 속 JARVIS는 단순한 음성 챗봇인 TTS가 아닙니다. JARVIS는 다섯 가지 작업을 동시에 수행합니다.

1. Tony가 말하기 전에 의도 파악 — **빠른 의도 분류**
2. 가능하면 직접 응답하고 그렇지 않으면 부하 직원에게 위임 — **4방향 라우팅**
3. 한 번에 여러 정보 수집 — **병렬 팬아웃**
4. 어제의 대화를 기억합니다 — **장기 기억**
5. 다른 JARVIS/시스템에서 호출 가능 — **양방향 A2A**

그래서 이 요리책의 핵심은 음성이 아닌 **그래프 모양**입니다. 목소리는 바로
input/output 쉘; "JARVIS 느낌"을 만들어내는 것은 NeoGraph의 오케스트레이션 엔진입니다.

## 전체 그래프

```
                          ┌────────────────────────┐
                          │ Background triggers    │
                          │ (timer / external events)│ ── A2A server for
                          └───────────┬────────────┘     JARVIS calls go here
                                      │
 [Microphone]──[VAD]──[whisper.cpp STT]──[memory_lookup]──[intent_router]
    miniaudio                          ▲                   │
                                       │ Store             │
                                       │ (conversation accumulation)│
                                       │                   │ Router makes 4-way decision
                                       │                   │ (chat goes directly to synthesizer)
                                       │                   │
                           ┌───────────┴───────────────────┴───────────────┐
                           │                                                │
                   [direct_branch]        [delegate_branch]        [parallel_branch]
                        │                       │                       │
               MCP tool single call        Delegate to expert entirely       Send / fan-out
               (time, weather, memo, etc.)    (coder, researcher, ...)     to multiple tools simultaneously
                        │                       │                       │
                        └───────────────────────┼───────────────────────┘
                                                │
                                        [response_synth]
                                        (synthesize natural response with large LLM)
                                                │
                                                ↓
                                   [supertonic TTS] ──→ [Speaker]
                                   (in detected language)     miniaudio
```

## 두 개의 카탈로그 JSON 파일 — JARVIS의 "내가 할 수 있는 것"

JARVIS가 시작되면 두 개의 파일을 읽고 해당 기능 목록을 작성합니다.
**즉, 코드를 다시 컴파일하지 않고도 add/remove 기능을 사용할 수 있습니다.**

### `config/mcp_catalog.json` — 도구

JARVIS 함수 유형 도구 목록을 직접 호출할 수 있습니다.
각 항목은 하나의 MCP 서버(HTTP 또는 stdio)에 해당합니다.

```json
{
  "tools": [
    {
      "name": "time_weather",
      "transport": "http",
      "url": "http://127.0.0.1:8000",
      "description": "Short, immediate-answer information like current time, weather, exchange rates",
      "enabled": true
    },
    {
      "name": "personal_memo",
      "transport": "stdio",
      "command": ["python3", "examples/demo_mcp_stdio_server.py"],
      "description": "Tony's personal memo storage/retrieval",
      "enabled": true
    }
  ]
}
```

시작 시 각 MCP 서버에서 `get_tools()`를 호출 → 도구 정의 병합
그리고 이를 "사용 가능한 도구"로 라우터의 시스템 프롬프트에 삽입합니다.

### `config/agent_registry.json` — 전문가 (A2A)

하위 에이전트 JARVIS는 전체 작업을 위임할 수 있습니다. 각각은 별도의 process/machine로 실행됩니다.
A2A 엔드포인트로.

```json
{
  "agents": [
    {
      "name": "coder",
      "url": "http://127.0.0.1:8210",
      "expertise": "Code writing, review, debugging",
      "fetch_card_on_start": true
    },
    {
      "name": "researcher",
      "url": "http://127.0.0.1:8211",
      "expertise": "Web search + summarization, academic paper organization",
      "fetch_card_on_start": true
    }
  ]
}
```

시작 시 각 URL에서 `AgentCard`를 요청하고 → 응답하는 것만 활성화합니다.
**핵심 트릭**: Python이든 상관없이 A2A 표준을 따르는 모든 외부 에이전트
다른 사람이 만든 A2A 봇, 다른 NeoGraph 인스턴스 등 — JARVIS의 봇이 될 수 있습니다.
이 JSON에 URL를 추가하면 됩니다.

## 라우터(의도 분류) — JARVIS의 두뇌

고정된 DeepSeek 모델(`deepseek/deepseek-v4-flash-0731`)에 대한 단일 호출은 다음을 반환합니다.

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — 도구나 위임이 없습니다. Synthesizer는 자체 지식+대화 메모리를 활용하여 직접 답변합니다.
인사, 자기소개, 잡담, "내가 아까 뭐라고 했지?" 스타일 대화 회상. 라우터의 경우
카탈로그에 없는 tools/agents를 발명하면 검증 단계에서 이 모드로 강등됩니다.
- `direct` — 단일 도구 호출. 결과가 단순하면(`"3:30 PM"`), `skip_synthesis=true`로 합성을 건너뜁니다.
TTS로 바로 이동하세요. **빠른.**
- `delegate` — `delegate_to`가 가리키는 A2A 엔드포인트에 완전히 위임합니다.
결과를 얻은 후 음성에 대한 한 줄 요약만 합성합니다.
- `parallel` — 다중 `tool_calls`. NeoGraph의 `make_parallel_group`를 사용하여 동시에 실행하고,
감속기는 합성기의 결과를 결합합니다.

### 라우터와 신디사이저를 분리해야 하는 이유

ReAct를 사용하여 하나의 대형 LLM를 통해 모든 것을 실행하면 턴당 1~3초가 걸리므로 JARVIS 느낌이 사라집니다.
- 라우터: 소형 모델, ~200ms, 단일 JSON
- 합성기: 대형 모델, ~800-1500ms, 단일 자연어 응답
- 도구가 즉각적인 응답을 제공하는 경우 합성기를 건너뛰고 → 응답은 ~500ms 후에 시작됩니다.

영화 JARVIS의 빠른 응답 타이밍은 이러한 분리에서 비롯됩니다.

## 메모리(`Store`)

각 턴이 시작될 때 `memory_lookup` 노드는 마지막 N 턴 + 사용자 기본 설정을 가져옵니다.
NeoGraph `Store`의 (`tony.prefers.language=ko`, `tony.last_topic=...`).

각 턴이 끝나면 JARVIS는 응답 + Tony의 발화 + 사용된 도구를 저장에 푸시합니다.
다음 턴의 라우터는 "앞서 언급한 것"과 같은 참조를 해결할 수 있습니다. `JsonFileStore`
파일에 유지 — 다시 시작해도 기억합니다. 빈 회전(STT 고장/소음)은 제외됩니다.
메모리 오염을 방지하기 위해 커밋을 수행합니다. `prefs.native_lang`는 예상 모국어를 유지합니다.
(언어 일관성).

## 양방향 A2A — JARVIS 호출 및 호출됨

- **전화**: `agent_registry.json`에서 `A2AClient`를 통해 전문가에게 위임합니다.
- **호출 중**: JARVIS 자체는 `A2AServer`(포트 8200)를 노출합니다.
  - 외부 시스템은 `POST /v1/messages`를 통해 JARVIS에 문자 메시지를 보낼 수 있습니다.
  - 모바일 앱, 다른 NeoGraph 인스턴스, 심지어 다른 JARVIS도 호출할 수 있습니다.
  - 텍스트 입력은 microphone/STT 단계를 건너뛰고 라우터로 직접 이동합니다.

**JARVIS-to-JARVIS 통신 데모**: 홈 JARVIS(8200) ⇔ 사무실 JARVIS(8201).
"JARVIS 사무실에서 오늘 회의록 가져오기" → 집 JARVIS가 A2A를 통해 JARVIS 사무실에 전화합니다.
→ 음성을 통해 Tony에게 응답이 전달됩니다.

## 백그라운드 트리거(사전 대응)

별도의 비동기 그래프가 백그라운드에서 실행됩니다.
- 타이머(5분마다 달력 확인)
- 외부 이벤트(홈 센서, 이메일 수신)
- 외부 A2A 호출

이벤트가 발생하면 JARVIS의 메인 그래프에 메시지를 주입합니다. → JARVIS가 말합니다.
토니가 묻기 전에. ("선생님, 10분 후에 회의가 있습니다.")

NeoGraph의 `27_async_concurrent_runs.cpp` 패턴을 정확하게 사용합니다.

## 디렉토리 구조

```
jarvis/
├── README.md                      ← This document
├── CMakeLists.txt                 External dependencies (whisper/onnxruntime/miniaudio) gated
├── config/                        Default config (graph · catalog · registry · persona)
├── config-demo/                   Execution preset (real-tools / mock)
├── config-bench*/                 Benchmark config
├── src/
│   ├── main.cpp                   Entry point (node registration · graph compilation · main loop)
│   ├── audio/                     miniaudio capture (+Silero VAD) · playback, supertonic TTS
│   ├── stt/                       whisper_node (multi-language · language consistency) + moonshine_node (edge)
│   ├── orchestrator/              Router, MCP catalog loader, A2A dispatcher
│   └── memory/                    Store-based conversation memory (JsonFileStore persistence)
├── specialists/                   coder / researcher (separate A2A servers)
├── bench/                         NeoGraph vs LangGraph benchmark (twin · driver · Docker)
├── assets/download.sh             Download whisper/supertonic/moonshine/silero models
├── scripts/
│   ├── run_jarvis.sh              Execution wrapper (LD_LIBRARY_PATH · ROCm · dxg auto)
│   ├── jarvis_repl.py             Korean readline REPL (text/wav input)
│   ├── build_whisper_hip.sh       Build whisper.cpp ROCm/HIP GPU
│   └── demo_mcp_server.py         Demo MCP server (time/weather/calc)
└── docs/architecture.md          Detailed node-by-node graph explanation
```

## 빌드/실행

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — onnxruntime, whisper.cpp, miniaudio found on system (or mock if missing)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENROUTER_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

라이브 공급자는 고정된 DeepSeek 모델을 사용하는 OpenRouter이며 `.env`의
`OPENROUTER_API_KEY`로 활성화됩니다. 키가 없으면 MockProvider(echo)를
사용하여 오프라인으로 실행됩니다.

## 보이스 스택 세부정보

### 라이브 마이크(미니 오디오 + Silero VAD)
`JARVIS_MIC=1` 또는 구성 `use_microphone:true`. 캡처 작업자 스레드가 Silero VAD를 실행합니다.
512 샘플 창에서 음성 start/end를 감지합니다(200ms 프리롤, 500ms 무음 종료).
**백프레셔**: TTS 에코, 오래된 발화,
그리고 소음이 시작됩니다. 장치 오류(WSL2 마이크 연결 끊김 등)는 자동으로 stdin으로 돌아갑니다.
조정: `JARVIS_VAD_THRESHOLD`(기본값 0.5), 관찰: `JARVIS_MIC_DEBUG=1`.

### STT — 두 가지 옵션(`stt.type` 구성을 통해 교체)
- **`whisper_stt`** (기본값): 속삭임.cpp. `language:"auto"`는 99개 언어를 자동으로 감지합니다.
→ **화자 언어로 답변 및 TTS**. **언어 일관성**: 모국어 유지
store.prefs에서는 외국어로 잘못 식별된 짧은 발화가 갑자기 전환되지 않도록 합니다(필요
전환에 대한 일관된 오인).
- **`moonshine_stt`**: 달빛처럼 작은 ONNX(27M, ORT를 초음속과 공유).
엣지, 저지연, 한국형. 언어별 모델이므로 lang이 수정되었습니다.

### GPU 가속(whisper.cpp ROCm/HIP)
번들 속삭임.cpp는 CPU 전용입니다. CPU(11초 클립)에서는 최대 32초가 소요됩니다. AMD GPU
(gfx1201=R9700, ROCm≥7.2) GGML_HIP에 대해 `bash scripts/build_whisper_hip.sh`를 실행합니다.
빌드 → **~7초(4.5×)**. run_jarvis.sh는 ROCm 런타임과 WSL dxg를 자동으로 로드합니다.

## 벤치마크 — NeoGraph 대 LangGraph(`bench/`)

동일한 토폴로지 미러링(마이크→stt→병합→메모리→라우터→4방향→synth/skip→커밋→tts)
LangGraph(Python Twin `langgraph_twin.py`)에서 동일한 제약 조건을 측정합니다.
(`--cpus=2 --memory=2g`) 컨테이너.

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 이행현황

**완벽한 기능** — 실제 하드웨어(OpenRouter DeepSeek)에서 실시간 음성 단일 턴을
검증했습니다. 마이크→VAD→STT→라우터→4방향→신디사이저→TTS 풀 체인 +

알려진 제한 사항/다음 버전:
- **참여 지원되지 않음** — TTS 재생 중 발언은 백프레셔를 통해 삭제됩니다.
(v2에는 취소 토큰이 추가됩니다).
- **STT 스트리밍이 적용되지 않음** — 발화 완료 후 일괄 전사입니다. 문샤인 v2
에르고딕 인코더 청크별 스트리밍이 다음 후보입니다.
- **다중 스피커 · 장기 메모리 압축** — 단일 스피커 가정, 24회전 제한.
- **백그라운드 트리거(사전 대응)** — 설계되었지만 구현되지 않았습니다.

## 라이센스/외부 종속성

|도서관|특허|역할|
|---|---|---|
|[supertonic](https://github.com/supertone-inc/supertonic)|MIT|TTS(99M, ONNX, 31개 언어)|
|[whisper.cpp](https://github.com/ggerganov/whisper.cpp)|MIT|STT(99개 언어 자동 감지, CPU/ROCm)|
|[Moonshine](https://github.com/moonshine-ai/moonshine)|MIT|엣지 STT 옵션(27M ONNX)|
|[miniaudio](https://github.com/mackron/miniaudio)|MIT-0 / 공개 도메인|마이크 캡처 + 스피커 재생|
|[Silero VAD](https://github.com/snakers4/silero-vad)|MIT|음성 start/end 감지(ONNX)|
|ONNX 런타임|MIT|초음속·월광·VAD 추론|
