<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ko source_sha256=0682c23a88e18a9bc1094429a0ccfbc9d49d84c279355eed29ce28f068f1da42 -->
**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

# JARVIS 그래프 — 노드별 세부정보


`config/jarvis_graph.json`의 각 노드가 수행하는 작업과 해당 위치에 있는 이유.
README.md의 다이어그램과 함께 읽는 것이 가장 좋습니다.

## 한 턴의 수명

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 200ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8은 JARVIS의 인지된 응답 시간입니다. 대상 분포:
- 단답형(직접 + skip_synthesis): T0→T8 ≒ 1.0-1.5s
- 일반(직접 + 신디사이저): T0→T8 ≒ 2.0-3.0s
- 위임(delegate) : T0→T8 ≒ 3.0-8.0s (전문가 작업시간에 따라 다름)
- 포괄적(병렬 + 신디사이저): T0→T8 ≒ 2.5-4.0s

## 노드별 세부정보

### mic_capture (`voice_in`) — 라이브 마이크 구현
- 기본값은 stdin 모드(text / `wav:/path`)입니다. **`use_microphone:true` 또는 환경
`JARVIS_MIC=1`**는 라이브 마이크 캡처를 활성화합니다.
- `miniaudio` 캡처 장치는 16kHz 모노 f32를 콜백 → 뮤텍스 버퍼로 스트리밍합니다.
- VAD 작업자 스레드는 512 샘플(32ms) 창에서 `Silero VAD`(ONNX) 추론을 실행합니다 → 음성 문제
- `vad_threshold`(0.5)를 초과하면 녹화가 시작됩니다(컷오프를 피하기 위해 200ms 프리롤).
500ms 연속 무음으로 발화 종료 → PCM가 발화 대기열로 푸시됨 → run()가 voice_in을 얻음
- 250ms 미만의 소음은 무시되고 `max_utterance_seconds`를 초과하면 강제 종료됩니다.
- **장치 초기화 시 자동 stdin 폴백 failure(WSL2 audio bridge missing, etc.)** —
충돌 없음. WSLg/PulseAudio 소스를 사용할 수 있는 경우 WSL2에서 라이브로 작동합니다.

### stt (`whisper_stt` 또는 `moonshine_stt`)
- 노드 수명 동안 단일 Whisper.cpp 모델을 재사용합니다(다시 로드 비용 없음 ×)
- `language="auto"`는 처음 30초(또는 전체 발화)부터 자동 감지합니다.
- 출력: `user_text`(단일 문자열), `user_lang`(ISO 코드)
- 인식 신뢰도가 너무 낮으면 빈 문자열 - 라우터 단계에서 차례를 건너뜁니다.

**GPU 가속(whisper.cpp ROCm/HIP)**: 번들로 포함된 Whisper.cpp는 CPU 전용이므로
Whisper-large-v3-turbo는 CPU(jfk 11s)에서 ~32초가 소요됩니다. — 라이브에는 적합하지 않습니다. AMD GPU
(gfx1201=R9700, ROCm≥7.2) GGML_HIP에 대해 `bash scripts/build_whisper_hip.sh`를 실행합니다.
빌드 → whisper_install 교체 → **~7s (4.5×)**. run_jarvis.sh는 자동으로 ROCm을 로드합니다.
런타임 및 WSL dxg 브리지(HSA_ENABLE_DXG_DETECTION). GPU를 사용하면 구성을 실시간으로 크게 유지할 수 있습니다.
없으면 속삭이는 소형(CPU ~8s)으로 전환합니다.

**대체 옵션 `moonshine_stt`**(Moonshine-tiny ONNX): 27M 초경량, 원시 16kHz
파형 입력(mel 아님), seq2seq(encoder + 2-model separated decoder + KV cache). 주식
초음속 TTS를 사용한 ONNX 런타임. 언어별 특징 모델이므로 `user_lang`는 구성이 수정되었습니다.
(tiny-ko = "ko"). Tokenizer는 tokenizer.json의 SentencePiece BPE에서 직접 디코딩합니다.
( →space + ByteFallback + 퓨즈). 구성 stt.type을 통한 교환이 작동하고
Python 최적 참조는 55개 토큰 문자 수준 패리티를 확인했습니다. int8(~28MB)에는 전체 ORT가 필요합니다.
빌드(번들 축소 빌드에서는 ConvInteger 제외) → 기본값 fp32(~183MB).

### text_or_voice (`channel_merge`)
- voice_in(STT 통과)과 text_in(외부 A2A) 사이의 활성 경로를 선택합니다.
- 둘 다 비어 있으면 빈 회전 — 그래프는 한 사이클을 통과합니다.
- 외부 A2A 호출에는 `user_lang`도 포함되어야 합니다(누락된 경우 "en"으로 가정).

### memory_lookup (`memory_lookup`)
- NeoGraph `Store` `jarvis.tony` 네임스페이스에서 읽습니다.
- 마지막 N 턴 + prefs + last_topic을 단일 `memory_context` 푸시로 결합합니다.
- 비용 ×, 항상 그래프 시작 시 실행

### router (`intent_classifier`)
- 고정 DeepSeek 모델(OpenRouter, ~200-400ms)
- 시스템 프롬프트 = persona.txt [라우터] + MCP 카탈로그 텍스트 + 에이전트 레지스트리 텍스트
- 출력 JSON 유효성 검사: 구문 분석 실패 → 대체(모드=채팅). Tool/agent 이름
카탈로그·등록부에 대해 검증됨; 실제가 아닌 경우 채팅으로 강등됩니다(LLM가 발명한 것을 방지함)
하류로 흐르는 `delegate_to:"null"`).
- 채팅 모드는 tools/delegation 없이 response_synth로 직접 이동합니다 — 인사말의 경우,
자기소개, 대화 회상

### direct_branch (`tool_dispatch`)
- `route_decision.tool_calls[0]`를 한 번 디스패치합니다.
- `tool_results` 채널에 결과를 추가합니다.
- skip_synthesis=true인 경우 다음 노드를 TTS로 직접 우회합니다.

### parallel_branch (`parallel_tool_fanout`)
- 모든 `route_decision.tool_calls`를 동시에 실행합니다(`make_parallel_group`)
- `max_concurrent`를 통한 상한(기본값 4)
- 모든 결과를 `tool_results`에 순서대로 추가 → Reducer는 Synthesizer로 사용

### delegate_branch (`a2a_delegate`)
- user_text를 `route_decision.delegate_to`가 가리키는 A2A 엔드포인트로 보냅니다.
- `timeout_seconds`가 초과된 경우 오류 응답(JARVIS가 음성을 통해 "전문가가 응답하지 않음"이라고 말함)
- 응답에서 먼저 `[SUMMARY]` 라인을 추출 → `delegated_reply`에 저장

### response_synth (`llm_call`)
- 고정 DeepSeek 모델(OpenRouter, ~800-1500ms)
- 시스템 프롬프트 = persona.txt [synth] (+ 언어 지침 + 세션 경계 설명)
- 대화 기록(memory_context.recent_turns)은 **user/assistant의 메시지 배열로 전달됩니다.
역할 전환** — 이전에는 사용자 메시지의 인라인 JSON로 인해 모델이 과거 답변을 처리하게 되었습니다.
내용 그대로(기억 앵무새)로.
- 현재 차례의 사용자 메시지 = user_text + tool_results / delegated_reply 첨부
- 축어적 보호: 트림 후 출력이 과거 답변과 정확히 일치하는 경우 한 번 재생성합니다.
- 출력 = `final_text`(읽을 TTS 문자열)
- skip_synthesis=true 경로에서 우회됨(synth_skip이 이 자리를 차지함)

### synth_skip (`passthrough`)
- tool_results(일반적으로 도구의 원시 응답)의 마지막 항목을 final_text로 직접 복사합니다.
- 예: 시간 도구는 "오후 3시 30분"을 반환 → 음성으로 직접 반환
- JARVIS 응답 속도 비밀 무기 — 최대 1초의 대규모 LLM 호출 전체를 저장합니다.

### memory_commit(`memory_commit`)
- 이번 턴의 user_text + final_text + 사용된 도구 이름을 Store 턴에 추가합니다.
- 다음 차례에 memory_lookup은 라우터 컨텍스트를 위해 이를 가져옵니다.
- 비동기식으로 처리 가능(TTS와 병렬) — 현재 직렬

### tts (`supertonic_tts`)
- final_text + user_lang → 44.1kHz PCM를 사용한 초음속 추론
- 미니오디오 스피커 재생 시작 → 첫 번째 청크 ~100-300ms 이후
- 재생 중에 voice_in 활성화가 감지되면 취소 토큰으로 취소(참여)
  - 초기 스켈레톤은 참여를 지원하지 않습니다. v2에 추가될 예정

## 외부 그래프 — 백그라운드 트리거 / A2A 서버

JARVIS 메인 그래프는 단순한 단일 발화, 단일 응답 주기이지만 main.cpp
JARVIS 느낌을 완성하는 두 가지 추가 구성 요소를 시작합니다.

### 배경 트리거 그래프
- 별도의 `GraphEngine`(또는 그냥 std::thread)
- 타이머/외부 이벤트 모니터링
- 이벤트 발생 시 JARVIS 메인의 `text_in` 채널에 메시지를 주입합니다.
- JARVIS는 Tony가 요청하기 전에 말합니다("선생님, 10분 후에 회의합니다.").

### A2A 서버(JARVIS를 외부에 노출)
- `agent_registry.json`의 `self` 섹션을 기반으로 합니다.
- `GraphAgentAdapter`로 래핑된 동일한 엔진을 노출합니다(예제 38 패턴)
- 외부 텍스트 입력은 STT 단계를 건너뛰고 `text_in` → 라우터로 직접 이동합니다.
- 응답을 텍스트로 다시 보내거나 로컬 TTS를 통해 동시에 재생할 수 있습니다.

## 알려진 제한 사항/다음 버전

- **바지인 지원 안 됨** — TTS 재생 중에 마이크 입력이 무시됩니다. v2에 취소 토큰을 추가합니다.
- **다중 스피커는 지원되지 않습니다** — 한 사람이라고 가정합니다. 스피커 분리에는 별도의 노드(예: pyannote)가 필요합니다.
- **장기 메모리 압축** — 대화가 길어질수록 회전이 무한히 늘어납니다. #56 기록_압축 패턴이 필요합니다.
- **카탈로그 핫 리로드** — JSON 변경 감지는 수동입니다(SIGHUP 등). v2의 inotify를 통해 자동으로 다시 로드됩니다.
