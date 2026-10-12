<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ko source_sha256=ed7f628ae6ae172e9e2cbd3d681d4880b503f251e10b745e7be089fbcdc42200 -->
# JARVIS 그래프 — 노드별 상세 설명

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

## 타입 기반 provider 전환 — 소스 상태

C++ router, synthesizer, specialist 픽스처는 타입이 있는 `ProviderRequest`, `sp::Message`, `sp::Event`를 사용합니다. provider 결과는 예전의 응답 문자열이 아니라 불변 `sp::Outcome` 값(`sp::runtime::Result`) 전체입니다. `src/provider_support.h`는 로컬 Jarvis/coder/researcher 목(mock) 프로파일을 구현하며, 이 프로파일은 결정적인 router JSON, 사용자 텍스트 에코, 명시적으로 합성한 researcher 응답을 제공합니다. 이 픽스처는 자격 증명도 네트워크 provider도 필요하지 않지만, 실제 조사나 전문가 수준의 추론은 제공하지 않습니다. 아래에 나오는 기존 설정/프로파일 경로는 이번 문서 수정으로 고치거나 새로 만들지 않았습니다.

로컬 음성은 선택 사항이며, 선택한 whisper/Moonshine 모델, ONNX Runtime/Supertonic 자산, miniaudio, 사용할 수 있는 마이크/스피커 장치가 필요합니다. 텍스트/목 모드로 동작한다고 해서 음성이 동작한다는 증거가 되지는 않습니다. "클라우드 없이 동작"한다는 말은 로컬/목 모드에만 적용됩니다. 실제(live) 요청에는 승인된 `OPENROUTER_API_KEY`, 네트워크 접근, provider 용량이 필요하며, 프롬프트, 대화 메모리, 첨부된 도구/위임 결과를 OpenRouter로 보냅니다. 실제 모델은 고정되어 있고, 네이티브 요청에는 ZDR을 설정하지만 이는 데이터 거주지(residency) 보장이 아닙니다. 키는 로그와 버전 관리에 넣어서는 안 됩니다. provider의 토큰 사용량은 null일 수 있는 회계 정보이며 청구 금액이 아닙니다. 비용을 알려면 현재의 엔드포인트/모델 가격과 청구 대상 사용량이 필요합니다.

`[jarvis:ttft]`는 `PartKind::Text`와 `DeltaChannel::Content`를 가진, 비어 있지 않은 첫 `sp::PartDelta`에서 출력되며, usage, reasoning, 헤더, 다른 이벤트에서는 출력되지 않습니다. 이 값이 재는 것은 합성된 첫 텍스트이지 TTS가 처음 소리를 내는 시점이 아닙니다. Python REPL 드라이버는 계속 프로토콜 클라이언트이고, pybind 벤치마크는 타입 기반으로 옮긴 바인딩을 사용하므로 실행 증거를 따로 확보해야 합니다. 보존된 C++ 증거가 다루는 것은 CLI 인사, 합성 메모리 영속화, 정상적인 EOF 종료입니다. 아래의 음성/실제 요청 시간은 과거 기록이며 옮긴 경로를 검증한 것이 아닙니다.

`config/jarvis_graph.json`의 각 노드가 무엇을 하는지, 왜 그 위치에 있는지를 설명합니다.
README.md의 다이어그램과 함께 읽는 것이 가장 좋습니다.

## 한 턴의 흐름

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 500ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8이 JARVIS의 체감 응답 시간입니다. 목표 분포는 다음과 같습니다.
- 짧은 답변 (direct + skip_synthesis): T0→T8 ≈ 1.0-1.5s
- 일반 (direct + synth): T0→T8 ≈ 2.0-3.0s
- 위임 (delegate): T0→T8 ≈ 3.0-8.0s (전문가의 작업 시간에 따라 달라짐)
- 종합 (parallel + synth): T0→T8 ≈ 2.5-4.0s

## 노드별 상세

### mic_capture (`voice_in`) — 실시간 마이크 구현됨
- 기본값은 stdin 모드(텍스트 / `wav:/path`)입니다. **`use_microphone:true` 또는 환경 변수
  `JARVIS_MIC=1`**을 지정하면 실시간 마이크 캡처가 켜집니다.
- `miniaudio` 캡처 장치가 16kHz 모노 f32 오디오를 콜백으로 전달하고, 콜백은 이를 mutex 버퍼에 넣습니다.
- VAD 워커 스레드가 512샘플(32ms) 창마다 `Silero VAD`(ONNX) 추론을 실행해 음성 확률을 구합니다.
- `vad_threshold`(0.5)를 넘으면 녹음이 시작되고(앞부분이 잘리지 않도록 200ms를 미리 확보),
  무음이 500ms 이어지면 발화가 끝납니다. PCM은 발화 큐에 들어가고 run()이 이를 voice_in으로 받습니다.
- 250ms보다 짧은 잡음은 무시하고, `max_utterance_seconds`를 넘으면 강제로 끝냅니다.
- **장치 초기화에 실패하면(WSL2 오디오 브리지 없음 등) 자동으로 stdin으로 대체합니다.**
  크래시는 나지 않습니다. WSLg/PulseAudio 소스가 있으면 WSL2에서도 실시간으로 동작합니다.

### stt (`whisper_stt` 또는 `moonshine_stt`)
- 노드가 살아 있는 동안 whisper.cpp 모델 하나를 재사용합니다(재로딩 비용 ×).
- `language="auto"`는 처음 30초(또는 발화 전체)에서 언어를 자동으로 감지합니다.
- 출력은 `user_text`(문자열 하나)와 `user_lang`(ISO 코드)입니다.
- 인식 신뢰도가 너무 낮으면 빈 문자열이 되고, 그 턴은 router 단계에서 건너뜁니다.

**GPU 가속(whisper.cpp ROCm/HIP)**: 번들된 whisper.cpp는 CPU 전용이라
whisper-large-v3-turbo가 CPU에서 약 32초(jfk는 11초) 걸리므로 실시간 사용에는 맞지 않습니다. AMD GPU
(gfx1201=R9700, ROCm 7.2 이상)가 있으면 `bash scripts/build_whisper_hip.sh`를 실행해 GGML_HIP
빌드를 만들고 whisper_install을 교체합니다. 그러면 **약 7초(4.5배)**로 줄어듭니다. run_jarvis.sh는 ROCm
런타임과 WSL dxg 브리지(HSA_ENABLE_DXG_DETECTION)를 자동으로 불러옵니다. GPU가 있으면 설정을 large 모델로 그대로 두어도 실시간으로 쓸 수 있고,
없으면 whisper-small(CPU에서 약 8초)로 바꿉니다.

**대안 `moonshine_stt`**(Moonshine-tiny ONNX): 27M 초경량 모델이며, 입력은 mel이 아니라 16kHz 원시
파형입니다. 구조는 seq2seq(인코더 + 디코더 2개 모델로 분리 + KV 캐시)입니다. supertonic TTS와
ONNX Runtime을 공유합니다. 언어별로 변형 모델이 따로 있어서 `user_lang`은 설정으로 고정됩니다
(tiny-ko = "ko"). 토크나이저는 tokenizer.json의 SentencePiece BPE에서 바로 디코딩합니다
(▁→공백 + ByteFallback + Fuse). 설정의 stt.type으로 교체할 수 있고,
Python optimum 참조 구현과 55토큰 문자 단위 일치를 검증했습니다. int8(약 28MB)은 전체 ORT
빌드가 필요해서(번들된 축소 빌드에는 ConvInteger가 빠져 있음) 기본값은 fp32(약 183MB)입니다.

### text_or_voice (`channel_merge`)
- voice_in(STT를 거친 입력)과 text_in(외부 A2A) 중 활성 경로를 고릅니다.
- 둘 다 비어 있으면 빈 턴이 되고, 그래프는 한 사이클을 그냥 통과합니다.
- 외부 A2A 호출에도 `user_lang`을 포함해야 합니다(없으면 "en"으로 간주).

### memory_lookup (`memory_lookup`)
- NeoGraph `Store`의 `jarvis.tony` 네임스페이스에서 읽습니다.
- 최근 N개 턴 + prefs + last_topic을 하나로 묶어 `memory_context`로 보냅니다.
- 비용 ×, 항상 그래프 시작 시 실행됩니다.

### router (`intent_classifier`)
- OpenRouter를 거치는, 고정된 DeepSeek 모델(약 200-400ms)을 사용합니다.
- 시스템 프롬프트는 persona.txt의 [router], MCP 카탈로그 텍스트, 에이전트 레지스트리 텍스트를 합친 것입니다.
- 출력 JSON을 검증합니다. 파싱에 실패하면 mode=chat으로 대체합니다. 도구/에이전트 이름은
  카탈로그·레지스트리와 대조하고, 실제로 존재하지 않으면 chat으로 낮춥니다.
  LLM이 지어낸 `delegate_to:"null"`이 다운스트림으로 흘러가는 것을 막기 위해서입니다.
- chat 모드는 도구나 위임 없이 곧바로 response_synth로 갑니다. 인사, 자기소개, 대화 회상에 쓰입니다.

### direct_branch (`tool_dispatch`)
- `route_decision.tool_calls[0]`을 한 번 디스패치합니다.
- 결과를 `tool_results` 채널에 추가합니다.
- skip_synthesis=true이면 다음 노드를 건너뛰고 TTS로 바로 갑니다.

### parallel_branch (`parallel_tool_fanout`)
- `route_decision.tool_calls`를 모두 동시에 실행합니다(`make_parallel_group`).
- 동시 실행 상한은 `max_concurrent`(기본값 4)로 정합니다.
- 모든 결과를 순서대로 `tool_results`에 추가합니다. 이 목록은 리듀서를 거쳐 synthesizer가 사용합니다.

### delegate_branch (`a2a_delegate`)
- `route_decision.delegate_to`가 가리키는 A2A 엔드포인트로 user_text를 보냅니다.
- `timeout_seconds`를 넘으면 오류 응답을 만듭니다(JARVIS가 음성으로 "Expert not responding"이라고 말함).
- 응답에서 `[SUMMARY]` 줄을 먼저 뽑아 `delegated_reply`에 저장합니다.

### response_synth (`llm_call`)
- 고정된 DeepSeek 모델을 OpenRouter로 호출합니다(약 800-1500ms).
- 시스템 프롬프트는 persona.txt의 [synth]에 언어 지시와 세션 경계 주석을 더한 것입니다.
- 대화 기록(memory_context.recent_turns)은 **user/assistant 역할 턴의 messages 배열로 전달**합니다.
  예전에는 user 메시지 안에 JSON으로 넣었더니 모델이 지난 답변을
  그대로 내용으로 취급했습니다(memory parrot).
- 이번 턴의 user 메시지는 user_text에 tool_results / delegated_reply를 붙인 것입니다.
- 그대로 따라 하는 것을 막는 가드(verbatim guard)가 있습니다. 출력이 trim한 뒤 과거 답변과 정확히 같으면 한 번 다시 생성합니다.
- 출력은 `final_text`(TTS가 읽을 문자열)입니다.
- skip_synthesis=true 경로에서는 건너뜁니다(이 자리를 synth_skip이 차지합니다).

### synth_skip (`passthrough`)
- tool_results의 마지막 항목(보통 도구의 원본 응답)을 그대로 final_text로 복사합니다.
- 예: 시간 도구가 "3:30 PM"을 반환하면 그대로 음성으로 출력합니다.
- JARVIS의 응답 속도를 높이는 핵심 수단입니다. 큰 LLM 호출 한 번(약 1초)을 통째로 아낍니다.

### memory_commit (`memory_commit`)
- 이번 턴의 user_text, final_text, 사용한 도구 이름을 Store의 turns에 추가합니다.
- 다음 턴의 memory_lookup이 router의 컨텍스트로 쓰려고 이 값을 가져옵니다.
- 비동기로 처리할 수 있지만(TTS와 병렬) 현재는 직렬로 처리합니다.

### tts (`supertonic_tts`)
- final_text와 user_lang으로 supertonic 추론을 실행해 44.1kHz PCM을 만듭니다.
- miniaudio 스피커 재생을 시작하며, 첫 청크는 약 100-300ms 뒤에 나옵니다.
- barge-in(재생 중 끼어들기)은 구현되어 있지 않으며, 재생하는 동안의 마이크 입력은 버립니다.

## 그래프 바깥 — 백그라운드 트리거 / A2A 서버

메인 그래프는 사이클마다 발화 하나와 그에 대한 응답 하나를 처리합니다. A2A 서버는
구현되어 있고, 아래의 백그라운드 트리거는 아직 구현되지 않은 설계를 설명합니다.

### 백그라운드 트리거 그래프
- 별도의 `GraphEngine`(또는 그냥 std::thread)을 둡니다.
- 타이머와 외부 이벤트를 감시합니다.
- 이벤트가 발생하면 JARVIS 메인의 `text_in` 채널에 메시지를 주입합니다.
- Tony가 묻기 전에 JARVIS가 먼저 말합니다("Sir, meeting in 10 minutes.").

### A2A 서버 (JARVIS를 외부에 노출)
- `agent_registry.json`의 `self` 섹션을 기준으로 합니다.
- 같은 엔진을 `GraphAgentAdapter`로 감싸 노출합니다(예제 38의 패턴).
- 외부 텍스트 입력은 STT 단계를 건너뛰고 `text_in` → router로 바로 들어갑니다.
- 응답은 텍스트로 돌려보낼 수도 있고, 로컬 TTS로 동시에 재생할 수도 있습니다.

## 알려진 제약 / 다음 버전

- **Barge-in 미지원** — TTS 재생 중의 마이크 입력은 무시합니다. v2에서 취소 토큰을 추가합니다.
- **다중 화자 미지원** — 한 사람만 있다고 가정합니다. 화자 분리에는 별도의 노드가 필요합니다(예: pyannote).
- **장기 기억 압축** — 조회는 기본적으로 최근 6개 턴을 읽고, 저장은 최근 24개만 유지합니다. 오래된 턴은 요약하지 않고 버립니다.
- **카탈로그 핫 리로드** — 카탈로그는 시작할 때 읽으므로, 수정한 뒤에는 다시 시작해야 합니다. 자동 리로드는 구현되어 있지 않습니다.
