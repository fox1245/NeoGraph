# 자비스 그래프 — 노드별 상세

`config/jarvis_graph.json` 의 각 노드가 무엇을 하고 왜 그 자리에 있는지.
README.md 의 그림과 같이 읽으면 좋음.

## 한 턴의 수명

```
T0  마이크 활성, 토니 발화 시작 감지
T1  발화 끝 (VAD 가 200ms 무음 감지)
T2  STT 완료 — 텍스트 + 감지된 언어 코드
T3  메모리 조회 완료 — 최근 6턴 + 선호 + 마지막 주제
T4  라우터 결정 완료 — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way 분기 — 자가응답(chat) / 도구 직접 / 위임 / 병렬 중 하나 완료
T6  응답 합성 완료 (또는 skip 분기로 우회)
T7  메모리 커밋 완료
T8  TTS 첫 청크 재생 시작 ← 토니가 답을 "듣기 시작" 하는 지점
T9  TTS 마지막 청크 재생 완료, 마이크 다시 활성 대기
```

T0→T8 가 자비스의 체감 응답 속도. 목표 분포:
- 단답 (direct + skip_synthesis): T0→T8 ≈ 1.0-1.5초
- 일반 (direct + synth): T0→T8 ≈ 2.0-3.0초
- 위임 (delegate): T0→T8 ≈ 3.0-8.0초 (전문가 작업 시간에 좌우)
- 종합 (parallel + synth): T0→T8 ≈ 2.5-4.0초

## 각 노드 상세

### mic_capture (`voice_in`) — 라이브 마이크 구현됨
- 기본은 stdin 모드(텍스트 / `wav:/경로`). **`use_microphone:true` 또는 env
  `JARVIS_MIC=1`** 이면 라이브 마이크 캡처 활성화.
- `miniaudio` 캡처 디바이스가 16kHz mono f32 를 콜백으로 흘림 → mutex 버퍼
- VAD 워커 스레드가 512샘플(32ms) 윈도우로 `Silero VAD`(ONNX) 추론 → speech prob
- `vad_threshold`(0.5) 넘으면 녹음 시작(200ms 프리롤로 어두 안 잘림),
  500ms 연속 무음이면 발화 종료 → PCM 을 발화 큐에 push → run() 이 voice_in 으로
- 250ms 미만 잡음은 무시, `max_utterance_seconds` 초 넘으면 강제 종료
- **디바이스 초기화 실패(WSL2 오디오 브리지 없음 등)면 자동 stdin 폴백** —
  크래시 없음. WSLg/PulseAudio 소스가 있으면 WSL2 에서도 실동작.

### stt (`whisper_stt` 또는 `moonshine_stt`)
- whisper.cpp 모델 1개를 노드 수명 동안 재사용 (재로딩 비용 ×)
- `language="auto"` 면 첫 30초 (또는 발화 전체) 로 자동 감지
- 결과: `user_text` (한 문자열), `user_lang` (ISO 코드)
- 인식 신뢰도가 너무 낮으면 빈 문자열 — 라우터 단계에서 턴 스킵

**GPU 가속 (whisper.cpp ROCm/HIP)**: 번들 whisper.cpp 는 CPU 전용이라
whisper-large-v3-turbo 가 CPU 에서 ~32초(jfk 11초)로 라이브에 부적합. AMD GPU
(gfx1201=R9700 등, ROCm≥7.2)면 `bash scripts/build_whisper_hip.sh` 로
GGML_HIP 빌드 → whisper_install 교체 → **~7초(4.5×)**. run_jarvis.sh 가 ROCm
런타임·WSL dxg 브리지(HSA_ENABLE_DXG_DETECTION)를 자동 로드. GPU 있으면
config 를 large 로 둬도 실시간, 없으면 whisper-small(CPU ~8초)로 교체.

**대체 옵션 `moonshine_stt`** (Moonshine-tiny ONNX): 27M 초경량, raw 16kHz
파형 입력(mel 아님), seq2seq(encoder + 2-모델 분리 decoder + KV캐시). supertonic
TTS 와 같은 ONNX Runtime 재사용. 언어별 flavor 모델이라 `user_lang` 은 config
고정(tiny-ko = "ko"). 토크나이저는 SentencePiece BPE 를 tokenizer.json 에서 직접
디코드(▁→space + ByteFallback + Fuse). config 의 stt.type 을 바꾸면 스왑되고,
파이썬 optimum 레퍼런스와 55토큰 글자단위 패리티 검증됨. int8(~28MB)은 풀 ORT
빌드 필요(번들 축소빌드는 ConvInteger 미포함) → 기본 fp32(~183MB).

### text_or_voice (`channel_merge`)
- voice_in 경로(STT 통과)와 text_in 경로(외부 A2A) 중 살아있는 쪽 선택
- 둘 다 비어있으면 빈 turn — 그래프 그냥 한 사이클 통과
- 외부 A2A 호출일 때는 `user_lang` 도 같이 들어와야 함 (없으면 "en" 가정)

### memory_lookup (`memory_lookup`)
- NeoGraph `Store` 에서 `jarvis.tony` 네임스페이스 읽음
- 최근 N턴 + prefs + last_topic 합쳐서 `memory_context` 에 한 덩어리로 push
- 비용 ×, 그래프 시작마다 무조건 실행

### router (`intent_classifier`)
- 작은 LLM (gpt-4o-mini, ~200-400ms)
- 시스템 프롬프트 = persona.txt [router] + MCP 카탈로그 텍스트 + 에이전트 레지스트리 텍스트
- 출력 JSON 검증: 파싱 실패 → fallback (mode=chat). 도구명/에이전트명은 카탈로그·
  레지스트리와 대조해 실재하지 않으면 chat 으로 강등 (LLM 이 발명한
  `delegate_to:"null"` 같은 결정이 하류로 흐르지 않게).
- chat 모드는 도구/위임 없이 response_synth 로 직행 — 인사·자기소개·대화 회상용

### direct_branch (`tool_dispatch`)
- `route_decision.tool_calls[0]` 한 번 dispatch
- 결과를 `tool_results` 채널에 append
- skip_synthesis=true 면 다음 노드로 합성 우회하고 바로 TTS

### parallel_branch (`parallel_tool_fanout`)
- `route_decision.tool_calls` 전부 동시 실행 (`make_parallel_group`)
- `max_concurrent` 로 상한 (기본 4)
- 모든 결과를 `tool_results` 에 순서대로 append → reducer 가 합성기에서 활용

### delegate_branch (`a2a_delegate`)
- `route_decision.delegate_to` 가 가리키는 A2A 엔드포인트에 user_text 던짐
- `timeout_seconds` 초과 시 오류 응답 (자비스가 음성으로 "전문가가 응답하지 않습니다")
- 응답에서 `[SUMMARY]` 줄 우선 추출 → `delegated_reply` 에 저장

### response_synth (`llm_call`)
- 큰 LLM (gpt-4o, ~800-1500ms)
- 시스템 프롬프트 = persona.txt [synth] (+ 언어 지시 + 구세션 경계 주석)
- 대화 이력(memory_context.recent_turns)은 **messages 배열의 user/assistant
  역할 턴으로 전달** — user 메시지에 JSON 덤프로 인라인하면 모델이 과거 답변을
  본문으로 취급해 verbatim 복창하는 사고가 났었음 (기억 앵무새)
- 현재 턴 user 메시지 = user_text + tool_results / delegated_reply 첨부
- 복창 가드: 출력이 과거 답변과 trim 후 완전 일치하면 1회 재생성
- 출력 = `final_text` (TTS 가 읽을 문자열)
- skip_synthesis=true 경로에서는 우회됨 (synth_skip 이 그 자리)

### synth_skip (`passthrough`)
- tool_results 마지막 항목 (보통 도구의 raw 응답) 을 그대로 final_text 로 복사
- 예: 시간 도구가 "15시 30분" 반환 → 그대로 음성으로
- 자비스 응답 속도의 비밀 무기 — 합성 LLM 한 번 (~1초) 통째로 절약

### memory_commit (`memory_commit`)
- 이번 턴의 user_text + final_text + 사용된 도구 이름들을 Store turns 에 append
- 다음 턴 memory_lookup 이 이걸 끌어다 라우터에 컨텍스트 제공
- 비동기로 처리 가능 (TTS 와 병렬) — 현재는 직렬

### tts (`supertonic_tts`)
- final_text + user_lang 으로 supertonic 추론 → 44.1kHz PCM
- miniaudio 로 스피커 재생 시작 → 첫 청크 ~100-300ms 후
- 재생 중 voice_in 활성화 감지 시 (barge-in) cancel token 으로 중단
  - 초기 골격은 barge-in 미지원, v2 에서 추가 예정

## 그래프 바깥 — 백그라운드 트리거 / A2A 서버

자비스 본체 그래프는 한 발화 한 응답의 단순 사이클이지만, main.cpp 가
같이 띄우는 두 가지가 자비스 느낌을 완성함:

### 백그라운드 트리거 그래프
- 별도 `GraphEngine` (또는 그냥 std::thread)
- 타이머 / 외부 이벤트 감시
- 이벤트 발생 시 자비스 본체의 `text_in` 채널에 메시지 주입
- 토니가 묻기 전에 자비스가 먼저 말함 ("Sir, 회의 10분 전입니다.")

### A2A 서버 (자비스를 외부에 노출)
- `agent_registry.json` 의 `self` 섹션 기반
- 같은 engine 을 `GraphAgentAdapter` 로 감싸서 노출 (example 38 패턴)
- 외부 텍스트 입력은 STT 단계 건너뛰고 `text_in` → router 직행
- 응답은 텍스트로도 돌려보내고, 동시에 로컬 TTS 로도 재생 가능

## 알려진 한계 / 다음 버전

- **barge-in 미지원** — TTS 재생 중 마이크 입력 무시. v2 에서 cancel token 도입.
- **다중 화자 미지원** — 한 사람 가정. 화자 분리는 별도 노드 (pyannote 같은 거) 필요.
- **장기 메모리 압축** — 대화가 길어지면 turns 가 무한 증가. #56 history_compaction 패턴 도입 필요.
- **카탈로그 핫리로드** — JSON 변경 감지는 수동 (SIGHUP 등). inotify 기반 자동 리로드는 v2.
