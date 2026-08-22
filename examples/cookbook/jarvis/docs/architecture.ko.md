<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ko source_sha256=27b6441685a1293819d0813b44b53142b1fa05c36ec9ba960702d5243eb9bf92 -->
# JARVIS Graph — 노드별 상세 설명

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

각 노드가 `config/jarvis_graph.json`에서 수행하는 역할과 해당 위치에 있는 이유. README.md의 다이어그램과 함께 읽는 것이 가장 좋습니다.

## 한 턴의 수명 주기

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

T0→T8은 JARVIS의 인지 응답 시간입니다. 목표 분포:
- 짧은 응답(직접 + skip_synthesis): T0→T8 ≈ 1.0-1.5초
- 일반 응답(직접 + 합성): T0→T8 ≈ 2.0-3.0초
- 위임(위임): T0→T8 ≈ 3.0-8.0초(전문가 작업 시간에 따라 다름)
- 종합 (병렬 + 합성): T0→T8 ≈ 2.5-4.0초

## 노드별 세부 사항

### mic_capture (`voice_in`) — 라이브 마이크 구현됨
- 기본값은 stdin 모드(텍스트 / `wav:/path`)입니다. **`use_microphone:true` 또는 환경 변수 `JARVIS_MIC=1`**가 라이브 마이크 캡처를 활성화합니다.
- `miniaudio` 캡쳐 장치가 16kHz 모노 f32를 콜백으로 스트리밍하여 mutex Buffer로 보냅니다.
- VAD worker thread가 `Silero VAD`(ONNX) 추론을 512-샘플(32ms) 창에서 실행 → 음성 확률
- `vad_threshold`(0.5)를 초과하면 녹음이 시작되고(컷오프 방지를 위한 200ms 프리롤), 500ms 연속 무음이 지속되면 발화가 종료되어 → PCM이 발화 큐로 푸시되고 → run()이 voice_in을 받습니다.
- 소음 <250ms 무시됨, `max_utterance_seconds` 초과 시 강제 종료
- 장치 초기화 실패 시 자동 stdin 폴백(WSL2 오디오 브리지 누락 등) — 크래시 없음. WSLg/PulseAudio 소스를 사용할 수 있는 경우 WSL2에서 라이브로 작동합니다.

### stt (`whisper_stt` 또는 `moonshine_stt`)
- 노드 수명 동안 단일 whisper.cpp 모델을 재사용 (재생(replay) 비용 없음 ×)
- `language="auto"` 처음 30초(또는 전체 발화)에서 자동 감지합니다
- 출력: `user_text` (단일 문자열), `user_lang` (ISO 코드)
- 인식 신뢰도가 너무 낮으면 빈 문자열 — 라우터 단계에서 턴 건너뜀

**GPU 가속(whisper.cpp ROCm/HIP)**: 번들된 whisper.cpp는 CPU 전용이므로 whisper-large-v3-turbo는 CPU에서 약 32초가 걸립니다(jfk 11초) — 실시간에는 부적합합니다. AMD GPU(gfx1201=R9700, ROCm≥7.2)에서 `bash scripts/build_whisper_hip.sh`를 실행하여 GGML_HIP 빌드를 만들고 whisper_install을 교체하면 **약 7초(4.5배)**가 됩니다. run_jarvis.sh는 ROCm 런타임과 WSL dxg 브리지(HSA_ENABLE_DXG_DETECTION)를 자동으로 로드합니다. GPU가 있으면 실시간 처리를 위해 config를 크게 유지할 수 있고, 없으면 whisper-small로 전환합니다(CPU 약 8초).

**대체 옵션 `moonshine_stt`** (Moonshine-tiny ONNX): 27M 초경량, 원시 16kHz 파형 입력(멜이 아님), seq2seq(인코더 + 2-모델 분리 디코더 + KV 캐시). supertonic TTS와 ONNX Runtime 공유. 언어별 맞춤 모델이라 `user_lang`는 설정 고정 (tiny-ko = "ko"). 토크나이저는 tokenizer.json의 SentencePiece BPE에서 직접 디코딩 (▁→공백 + ByteFallback + Fuse). config stt.type으로 교체 가능하며 압축 optimum 참조 구현이 55-토큰 문자 수준 일치로 검증됨. int8(약 28MB)는 전체 ORT 빌드 필요(번들된 축소 빌드는 ConvInteger 제외) → 기본 fp32(약 183MB).

### text_or_voice (`channel_merge`)
- voice_in(STT 전달)과 text_in(외부 A2A) 사이의 활성 경로 선택
- 둘 다 비어 있으면 빈 턴 — 그래프가 한 사이클 통과
- 외부 A2A 호출에도 `user_lang` 필요 (누락 시 "en" 가정)

### memory_lookup (`memory_lookup`)
- NeoGraph `Store` `jarvis.tony` 네임스페이스에서 읽음
- 마지막 N턴과 선호도, last_topic을 단일 `memory_context` 푸시로 결합
- 비용 ×, 항상 그래프 시작 시 실행

### router (`intent_classifier`)
- OpenRouter를 통해 DeepSeek 모델 고정 사용(약 200-400ms)
- 시스템 프롬프트 = persona.txt [라우터] + MCP 카탈로그 텍스트 + 에이전트 레지스트리 텍스트
- 출력 JSON 검증: 구문 분석 실패 시 → 폴백(mode=chat). 도구/에이전트 이름은 카탈로그·레지스트리와 대조 검증됨; 실제 항목이 아니면 chat으로 강등(LLM이 만들어낸 `delegate_to:"null"`가 다운스트림으로 흘러가는 것 방지).
- 채팅 모드는 도구/위임 없이 response_synth로 직접 이동 — 인사말, 자기소개, 대화 회상용

### direct_branch (`tool_dispatch`)
- `route_decision.tool_calls[0]`를 한 번 디스패치
- 결과를 `tool_results` 채널에 추가
- skip_synthesis=true면 다음 노드를 우회하여 TTS로 직접 이동

### parallel_branch (`parallel_tool_fanout`)
- 모든 `route_decision.tool_calls`를 동시에 실행 (`make_parallel_group`)
- 상한은 `max_concurrent` 적용(기본 4)
- 모든 결과를 순서대로 `tool_results`에 추가 → 리듀서가 합성에 사용

### delegate_branch (`a2a_delegate`)
- user_text를 `route_decision.delegate_to`가 가리키는 A2A 엔드포인트로 전송
- `timeout_seconds` 초과 시 오류 응답 (JARVIS라는echo voice로 "Expert not responding"이라고 음성으로 말함)
- 먼저 응답에서 `[SUMMARY]` 줄을 추출하여`delegated_reply`에 저장

### response_synth (`llm_call`)
- OpenRouter를 통한 Pinned DeepSeek 모델 (~800-1500ms)
- 시스템 프롬프트 = persona.txt [synth] (+ 언어 지시 + 세션 경계 주석)
- 대화 기록(memory_context.recent_turns)은 **user/assistant 역할 턴으로 구성된 messages 배열로 전달됨** — 이전에는 user 메시지에 포함된 인라인 JSON으로 인해 모델이 과거 답변을 내용 그대로 취급했음(memory parrot).
- 현재 턴 user 메시지 = user_text + tool_results / delegated_reply가 첨부됨
- 축어 일치 방지 장치: 출력이 trim 후 과거 답과 정확히 일치하면 한 번 재생성
- 출력 = `final_text` (TTS가 읽을 문자열)
- skip_synthesis=true 경로에서 우회됨 (synth_skip이 이 위치를 차지)

### synth_skip (`passthrough`)
- tool_results의 마지막 항목(보통 tool의 원시 응답)을 final_text로 직접 복사
- 예: Time tool이 "3:30 PM" 반환 → 음성으로 직접 전송
- JARVIS 응답 속도의 비밀 무기 — 약 1초 규모의 대규모 LLM 호출 전체를 절약

### memory_commit (`memory_commit`)
- 이번 턴의 user_text, final_text에 사용된 tool 이름을 Store turns에 추가
- 다음 턴 memory_lookup이 라우터 컨텍스트에 이를 가져옴
- 비동기적으로(TTS와 병렬) 처리 가능 — 현재 직렬 처리

### tts (`supertonic_tts`)
- final_text + user_lang로 supertonic inference 수행 → 44.1kHz PCM
- miniaudio 스피커 재생 시작 → 첫 청크 약 100~300ms 후
- 재생 중 voice_in 활성화가 감지되면 취소 토큰으로 취소(끼어들기)
  - 초기 스켈레톤은 끼어들기를 지원하지 않음. v2에 추가 예정

## 그래프 외부 — 백그라운드 트리거 / A2A 서버

JARVIS 메인 그래프는 단순한 단일 발언, 단일 응답 주기이지만, main.cpp는 JARVIS 환경을 완성하는 두 개의 추가 구성 요소를 시작합니다:

### 백그라운드 트리거 그래프
- 별도의 `GraphEngine`(또는 그냥 std::thread)
- 타이머 / 외부 이벤트 모니터링
- 이벤트 발생 시 JARVIS 메인의 `text_in` 채널에 메시지 주입
- JARVIS가 Tony가 묻기 전에 말함 ("각하, 10분 후 회의입니다.")

### A2A 서버 (JARVIS 외부 노출)
- `self` 섹션의 `agent_registry.json` 기반
- 동일한 엔진에 `GraphAgentAdapter`(예시 38 패턴)로 래핑하여 노출
- 외부 텍스트 입력은 STT 단계를 건너뛰고 `text_in` → 라우터로 직접 이동합니다.
- 응답은 텍스트로 다시 보내거나, 로컬 TTS를 통해 동시에 재생할 수 있습니다

## 알려진 제한 사항 / 다음 버전

- **Barge-in 미지원** — TTS 재생 중 마이크 입력이 무시됨. v2에서 취소 토큰을 추가합니다.
- **다중 화자 미지원** — 한 명의 화자(speaker)를 가정. 화자 분리(speaker separation)는 별도의 노드(예: pyannote)가 필요합니다.
- **장기 메모리 압축** — 대화가 길어지면 턴(turn)이 무한히 증가함. #56의 history_compaction 패턴이 필요함.
- **카탈로그 핫리로드** — JSON 변경 감지는 수동(SIGHUP 등)입니다. v2에서 inotify를 통한 자동 리로드를 추가합니다.
