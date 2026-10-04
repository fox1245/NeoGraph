<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=ko source_sha256=4de91aa4e04dc5f5a30c8878f37fa3dbf2c16671b0545387dd6ee20465a280ee -->
# JARVIS — 음성 기반 메타 오케스트레이터

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 타입 제공자 전환 — 범위 한정 실행 상태

C++ 라우터·합성기·전문가 픽스처는 타입 `ProviderRequest`, `sp::Message`, `sp::Event` 및 전체 불변 `sp::Outcome` (`sp::runtime::Result`)을 사용하며 기존 문자열 응답 API가 아니다. `src/provider_support.h`의 Jarvis/coder/researcher mock은 고정 라우터 JSON, 사용자 텍스트 echo, 명시적인 가상 연구 응답을 제공한다. 키나 네트워크 제공자는 필요 없지만 실제 연구·추론은 아니다. 아래 기존 설정/프로필 경로는 이 문서 변경으로 생성하거나 고치지 않는다.

로컬 음성은 선택 사항이며 선택한 whisper/Moonshine 모델, ONNX Runtime/Supertonic 자산, miniaudio 및 사용 가능한 마이크·스피커가 필요하다. 텍스트/mock 실행은 음성 동작의 증거가 아니다. 클라우드 불필요는 로컬/mock에만 해당한다. 라이브 요청에는 승인된 `OPENROUTER_API_KEY`, 네트워크·제공자 용량이 필요하며 프롬프트, 대화 메모리, 첨부 도구/위임 결과를 OpenRouter로 전송한다. 모델은 고정되어 있고 네이티브 요청의 ZDR은 지역 상주 보장이 아니다. 키를 로그·저장소에 넣지 않는다. nullable 토큰 사용량은 청구액이 아니며 비용에는 현재 엔드포인트/모델 가격과 실제 청구 사용량이 필요하다.

`[jarvis:ttft]`는 비어 있지 않은 첫 `sp::PartDelta` 중 `PartKind::Text`와 `DeltaChannel::Content`에만 발생하며 사용량·추론·헤더 이벤트는 제외한다. 첫 합성 텍스트 시점이지 실제 TTS 청취 시작이 아니다. Python REPL driver는 protocol client이며 pybind benchmark는 전환된 타입 binding을 사용하므로 별도 실행 증거가 필요하다. 현재 실행 증거는 실제 CLI 인사, 영속화된 합성 메모리 turn과 정상 EOF 종료에 한정된다. 마이크 입력·ASR·TTS·pybind 벤치나 vendor 추론을 검증하지 않는다. 아래 시간·음성/live 실행 주장은 과거 기록이며 현재 전환 qualification이 아니다.

위 CLI 증거는 interface 3에서 기록했습니다. 보존된 A2A 1.0 wire 변경과 SDK interface-4 제어는
소스 계약이며 새 runtime pass가 아닙니다.

> 로컬/mock은 클라우드 제공자가 필요 없으며 선택적 음성은 로컬 자산·장치가 필요하다.
> 마이크는 Tony, NeoGraph는 JARVIS, 도구/전문가는 JARVIS의 부하.

이 쿡북은 **아닙니다** "음성 TTS 예제". 이는 NeoGraph의 멀티에이전트 프리미티브 — MCP 도구, 양방향 A2A, 비동기 병렬, Store 메모리, ReAct 서브그래프 — **단 한 줄의 음성으로 엮어낸** 데모입니다.

## 이것이 JARVIS인 이유

영화 속 JARVIS는 단순히 음성 TTS를 갖춘 챗봇이 아닙니다. JARVIS는 다섯 가지를 동시에 수행합니다:

1. Tony가 말을 마치기 전에 의도를 포착 — **빠른 의도 분류**
2. 가능하면 직접 답하고, 그렇지 않으면 팀에게 위임 — **4-way 라우팅**
3. 여러 정보를 한 번에 수집 — **병렬 fan-out**
4. 어제의 대화를 기억 — **장기 메모리**
5. 다른 JARVIS/시스템에서 호출 가능 — **양방향 A2A**

따라서 이 쿡북의 핵심은 보이스가 아니라 **그래프 형태**입니다. 보이스는 단지 입출력 셸일 뿐이며, "자비스 스타일"을 만드는 것은 NeoGraph의 오케스트레이션 엔진입니다.

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

## 두 개의 Catalog JSON 파일 — JARVIS의 "내가 할 수 있는 일"

JARVIS가 시작되면 두 개의 파일을 읽고 능력 목록을 작성합니다. **이는 코드를 다시 컴파일하지 않고도 능력을 추가/제거할 수 있음을 의미합니다.**

### `config/mcp_catalog.json` — 도구

JARVIS가 직접 호출할 수 있는 함수형 도구 목록입니다. 각 항목은 하나의 MCP 서버(HTTP 또는 stdio)에 대응합니다.

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

시작 시 각 MCP 서버에서 `get_tools()`를 호출하여 → 도구 정의를 병합하고 라우터의 시스템 프롬프트에 "사용 가능한 도구"로 주입합니다.

### `config/agent_registry.json` — 전문가(A2A)

JARVIS가 전체 작업을 위임할 수 있는 서브에이전트입니다. 각각은 A2A 엔드포인트로서 별도의 프로세스/머신으로 실행됩니다.

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

시작 시 JARVIS는 설정된 AgentCard를 fetch합니다. 호출에는 호환되는 JSON-RPC 0.x/1.0 interface가 필요하며 discovery 응답만으로 호환성을 입증하지 않습니다. client는 설정된 endpoint를 바꾸지 않고 card dialect를 선택합니다. card-selected 호출은 다른 dialect로 fallback하지 않으며 전달한 SSE event는 재전송하지 않습니다. 외부 Python agent와 NeoGraph instance도 card와 wire 동작이 일치하면 이 계약을 사용할 수 있습니다.

## 라우터(의도 분류) — JARVIS의 두뇌

고정된 DeepSeek 모델(`~deepseek/deepseek-v4-flash-latest`)에 대한 단일 호출이 다음을 반환합니다:

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — 도구나 위임이 없습니다. 신시사이저가 자체 지식 + 대화 메모리를 사용하여 직접 응답합니다. 인사, 자기소개, 가벼운 대화, "아까 내가 뭐라고 했지?" 스타일의 대화 회상. 라우터가 카탈로그에 없는 도구/에이전트를 만들어내면 검증 단계에서 이 모드로 강등됩니다.
- `direct` — 단일 도구 호출. 결과가 단순하면(`"3:30 PM"`), `skip_synthesis=true`로 합성(synthesis)을 건너뛰고 바로 TTS로 이동. **빠름.**
- `delegate` — `delegate_to`이 가리키는 A2A 엔드포인트에 전적으로 위임. 결과를 받은 후 음성용 한 줄 요약만 합성.
- `parallel` — 여러 개의 `tool_calls`. NeoGraph의 `make_parallel_group`을 사용하여 동시에 실행하고, 리듀서가 결과를 결합하여 신시사이저에 전달합니다.

### 라우터와 신시사이저

모든 것을 ReAct를 사용하는 하나의 대형 LLM으로 처리하면 턴당 1-3초가 걸려 JARVIS 특유의 느낌이 사라집니다.
- 라우터: 소형 모델, 약 200ms, 단일 JSON
- 신시사이저: 대형 모델, 약 800-1500ms, 단일 자연어 응답
- 도구가 즉각적인 답변을 제공하면 신시사이저를 건너뛰어 → 응답이 약 500ms에 시작됩니다

영화 JARVIS의 빠른 응답 타이밍은 바로 이 분리에서 비롯됩니다.

## 메모리 (`Store`)

각 턴이 시작될 때, `memory_lookup` 노드는 최근 N개 턴 + 사용자 선호도(`tony.prefers.language=ko`, `tony.last_topic=...`)를 NeoGraph에서 가져옵니다. `Store`.

각 턴이 끝나면 JARVIS는 응답 + Tony의 발화 + 사용된 도구를 Store에 푸시합니다. 다음 턴의 라우터는 "앞서 언급한 그 것"과 같은 참조를 해결할 수 있습니다. `JsonFileStore` 파일에 영속화되어 재시작 후에도 기억됩니다. 빈 턴(STT 실패/노이즈)은 메모리 오염 방지를 위해 커밋에서 제외됩니다. `prefs.native_lang` 은 추정된 모국어(language consistency)를 유지합니다.

이 JSON 파일은 speech/conversation projection을 저장하며 인증된 native provider replay custody가 아닙니다.

## 양방향 A2A — JARVIS가 호출하고 호출됨

- **호출(Calling)**: 다음을 통해 전문가에게 위임 `A2AClient` 에서 `agent_registry.json`.
- **호출됨**: JARVIS 자체가 `A2AServer`(포트 8200)을 노출합니다.
  - 외부 시스템은 `POST /`에 JSON-RPC를 전송합니다. 0.x는 `message/send`, 1.0은 `A2A-Version: 1.0`과 `SendMessage`를 쓰며 header가 응답 dialect를 선택합니다.
  - 모바일 앱, 다른 NeoGraph 인스턴스, 심지어 다른 JARVIS도 이를 호출할 수 있습니다.
  - 텍스트 입력은 마이크/STT 단계를 건너뛰고 라우터로 직접 전달됩니다.

**JARVIS 간 통신 데모**: 홈 JARVIS(8200) ↔ 오피스 JARVIS(8201). "오피스 JARVIS에서 오늘 회의록 가져와" → 홈 JARVIS가 A2A를 통해 오피스 JARVIS를 호출 → 응답이 음성으로 Tony에게 전달됩니다.

## 백그라운드 트리거(프로액티브)

background timer/event trigger는 설계이며 구현된 컴포넌트가 아닙니다. host는 `27_async_concurrent_runs.cpp` 패턴으로 calendar/sensor event의 text를 queue에 넣을 수 있습니다. A2A server는 별도로 구현되어 있으며 proactive trigger를 검증하지 않습니다.

## 디렉터리 구조

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

## 빌드 / 실행

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — install SchemaProvider and optional voice dependencies first
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
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

라이브 제공자는 핀된 DeepSeek 모델과 함께 OpenRouter로 고정되며, `OPENROUTER_API_KEY`는 `.env`에 있습니다. 키가 없으면 MockProvider(에코)로 오프라인에서 실행됩니다.

## 음성 스택 세부 사항

### 라이브 마이크(miniaudio + Silero VAD)
`JARVIS_MIC=1` 또는 구성 `use_microphone:true`. 캡처 작업자 스레드는 512-샘플 창에서 Silero VAD를 실행하여 음성 시작/종료를 감지합니다(200ms 사전 롤, 500ms 침묵 종료). **백프레셔**: 추론 중 캡처를 폐기하여 TTS 에코, 오래된 발화, 시작 노이즈를 차단합니다. 장치 오류(WSL2 마이크 연결 해제 등)는 자동으로 stdin으로 대체됩니다. 튜닝: `JARVIS_VAD_THRESHOLD`(기본값 0.5), 관찰: `JARVIS_MIC_DEBUG=1`.

### STT — 두 가지 옵션(구성 `stt.type`를 통해 교체)
- **`whisper_stt`**(기본값): whisper.cpp. `language:"auto"`는 99개 언어를 자동으로 감지합니다 → **화자의 언어로 답변 및 TTS**. **언어 일관성**: store.prefs에서 기본 언어를 유지하므로 외국어로 잘못 식별된 짧은 발화가 갑자기 전환되지 않습니다(전환하려면 일관된 잘못된 식별이 필요함).
- **`moonshine_stt`**: Moonshine-tiny ONNX(27M, supertonic과 ORT 공유). 엣지, 저지연, 한국어 특화. 언어별 모델이므로 언어는 고정됩니다.

### GPU 가속 (whisper.cpp ROCm/HIP)
번들된 whisper.cpp는 CPU 전용입니다 — large 모델은 CPU에서 약 32초가 걸립니다 (11초 클립). AMD GPU (gfx1201=R9700, ROCm≥7.2) 환경에서 `bash scripts/build_whisper_hip.sh`를 실행하여 GGML_HIP 빌드를 사용하면 **약 7초 (4.5배)** 가 소요됩니다. run_jarvis.sh는 자동으로 ROCm 런타임과 WSL dxg를 로드합니다.

## 벤치마크 — NeoGraph vs LangGraph (`bench/`)

LangGraph(Python 트윈 `langgraph_twin.py`)에서 동일한 토폴로지(mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)를 미러링하고, 동일한 제약 조건(`--cpus=2 --memory=2g`) 컨테이너에서 측정합니다.

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 구현 상태

**과거 음성 실행 증거** — 이전 실제 하드웨어 live 단일 turn 실행

알려진 제한 사항 / 다음 버전:
- **Barge-in 미지원** — TTS 재생 중 발화는 backpressure를 통해 폐기됩니다(v2에서 취소 token을 추가할 예정).
- **스트리밍 STT 미적용** — utterance 완료 후 batch transcription을 수행합니다. Moonshinev2를 사용한 chunk-by-chunk streaming의 도입이 다음 후보입니다.
- **다중 화자 · 장기 메모리 압축** — 단일 화자를 가정합니다. lookup 기본은 최근 6턴, commit은 최신 24턴을 보존하며 이전 이력을 요약하지 않습니다.
- **백그라운드 트리거(프로액티브)** — 설계되었으나 구현되지 않음.

## License / 외부 종속성

| Library | 라이선스(License) | 역할 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS (99M, ONNX, 31개 언어) |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT (99개 언어 auto-detect, CPU/ROCm) |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | Edge STT 옵션 (27M ONNX) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / 퍼블릭 도메인 | 마이크 캡처 + 스피커 재생 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 음성 시작/종료 감지 (ONNX) |
| ONNX Runtime | MIT | supertonic·moonshine·VAD 추론 |
