<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=ko source_sha256=654b29f033cd6ac7d9dc23dfc92c5a26cf75810462da7c3da4f3df39e9c192dc -->
# JARVIS 오케스트레이션 벤치마크 — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 타입 제공자 전환 — 소스 상태

C++ 라우터·합성기·전문가 픽스처는 타입 `ProviderRequest`, `sp::Message`, `sp::Event` 및 전체 불변 `sp::Outcome` (`sp::runtime::Result`)을 사용하며 기존 문자열 응답 API가 아니다. `src/provider_support.h`의 Jarvis/coder/researcher mock은 고정 라우터 JSON, 사용자 텍스트 echo, 명시적인 가상 연구 응답을 제공한다. 키나 네트워크 제공자는 필요 없지만 실제 연구·추론은 아니다. 아래 기존 설정/프로필 경로는 이 문서 변경으로 생성하거나 고치지 않는다.

로컬 음성은 선택 사항이며 선택한 whisper/Moonshine 모델, ONNX Runtime/Supertonic 자산, miniaudio 및 사용 가능한 마이크·스피커가 필요하다. 텍스트/mock 실행은 음성 동작의 증거가 아니다. 클라우드 불필요는 로컬/mock에만 해당한다. 라이브 요청에는 승인된 `OPENROUTER_API_KEY`, 네트워크·제공자 용량이 필요하며 프롬프트, 대화 메모리, 첨부 도구/위임 결과를 OpenRouter로 전송한다. 모델은 고정되어 있고 네이티브 요청의 ZDR은 지역 상주 보장이 아니다. 키를 로그·저장소에 넣지 않는다. nullable 토큰 사용량은 청구액이 아니며 비용에는 현재 엔드포인트/모델 가격과 실제 청구 사용량이 필요하다.

`[jarvis:ttft]`는 비어 있지 않은 첫 `sp::PartDelta` 중 `PartKind::Text`와 `DeltaChannel::Content`에만 발생하며 사용량·추론·헤더 이벤트는 제외한다. 첫 합성 텍스트 시점이지 실제 TTS 청취 시작이 아니다. Python REPL driver는 protocol client이며 pybind benchmark는 전환된 타입 binding을 사용하므로 별도 실행 증거가 필요하다. 현재 [Jarvis CLI 실행 증거](../README.md)는 인사, 영속화된 합성 메모리 turn과 정상 EOF 종료에 한정되며 이 벤치 round의 증거가 아니다. 아래 벤치 시간·실행 주장은 과거 기록이다. CLI 실행은 마이크·ASR·TTS·pybind 벤치나 vendor 추론을 검증하지 않는다.

NeoGraph(C++ 목업 빌드)와 LangGraph(파이썬 트윈 `langgraph_twin.py`)에서 동일한 토폴로지(mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)를 미러링하고, 동일한 제약 조건(`--cpus=2 --memory=2g`) 컨테이너에서 측정합니다.

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 과거 결과(2026-07-05, OpenRouter 마이그레이션 이전; Groq)

| 메트릭 | NeoGraph | LangGraph | 델타 |
|---|---|---|---|
| 그래프 순수 오버헤드/턴(목업 0ms LLM, 200턴) | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq 실시간 추론/턴(8b 라우터+70b 합성, 20턴) | 684ms | 706ms | +22ms (~3%) |
| Groq p99 | 775ms | 870ms | +95ms (n=20, 노이즈 마진) |
| 콜드 스타트 | 7.9ms | 716ms | ~90× |
| RSS (mock) | 7.5 MB | 68MB | ~9× |

해석:
- 그래프 엔진 자체는 양측 모두 LLM에 비해 저렴하다(0.4ms 대비 3ms). Groq 델타는 약 19ms 중 +22ms로, HTTP 클라이언트 스택 차이(langchain-openai httpx+pydantic 대비 asio)에서 발생한다.
- 턴 간 간격은 **성장형(growth-type)** — 추론 속도가 빨라지면 커진다 — 200ms 턴(Cerebras급 / 단일 호출 경로)에서 10%+, 소규모 로컬 모델(~50ms/호출)의 경우 20-30%.
- 이 과거 container 구성의 startup/RSS 비율은 약90×/9×였습니다. production JARVIS100개 메모리 용량을 보장하지 않습니다.

## E2E Round — 실제 MCP 도구 왕복(2026-07-05) 포함

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_e2e.sh
```

공유 데모 MCP 서버 컨테이너(시간/계산/날씨) + 24턴 혼합 세트(직접 도구 호출 · 병렬 fan-out · 채팅 · 메모리 재생(replay)), 각각 2 라운드 동안이고 ABBA 순서 인터리빙:

| 라운드(실행 순서) | 평균 | p50 | 최대 | 메모 |
|---|---|---|---|---|
| NeoGraph | 810ms | 791 | 1052 |  |
| langgraph 라운드 1 | 673ms | 667 | 934 |  |
| GraphEngine | 1442ms | 1025 | 3830 | 지난 7턴 2.4~3.8초 — Groq 제한(스로틀) 구간 |
| neograph 라운드 2 | 689ms | 665 | 983 | LG R2 직후 실행했음에도 안정적 |

**결론: 이러한 조건(한국→Groq WAN, 턴당 약 700ms)에서는 제공자 측 분산(라운드 간 ±130~770ms)이 프레임워크 차이(목 측정 약 3ms + HTTP 스택 약 19ms)를 완전히 삼켜버린다.** 실행 순서를 바꾸면 승자도 바뀜 — 종단 간 턴 지연시간으로는 프레임워크 우위를 판별할 수 없으며, 통제된 mock 라운드만이 고정 오버헤드와 시작/메모리를 측정합니다. 종단 간 검증 완료: 두 하네스 모두 실제 도구와 정확히 작동(라우팅 모드 일치 21/24, 직접/병렬 실제 왕복), 시작 시간 74ms vs 1944~2483ms, RSS 14MB vs 122MB 확인됨.

시사점: 프레임워크 차이는 **낮은 분산으로 느 런과 낮은 절대 지연시간**(로컬 추론, 동일 데이터센터 추론)에서만 의미를 갖습니다 — 단순히 “빠른 추론”이 아니라. 클라우드 추론을 WAN으로 거치면 프레임워크와 무관하게 네트워크가 지배적이 됩니다.

## 경계 측정 라운드 — 제공자 분산 제거 (2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
```

Jarvis는 API 키를 TLS로만 보내므로, 러너는 nginx 프록시(`nginx-openrouter.conf`, 포트 8443)에서 TLS를 종단하고 실행마다 일회용 CA와 서버 인증서를 생성합니다(호스트에 `openssl` 필요, CA와 키는 임시 디렉터리에 두었다가 종료 시 삭제). 두 클라이언트는 `https://jarvis-openrouter-proxy:8443/openrouter/v1`을 호출하며 해당 CA 번들을 사용합니다: Jarvis는 `OPENROUTER_CA_FILE`, LangGraph 트윈은 `SSL_CERT_FILE`을 사용합니다. 프록시는 분석기가 위치로 파싱하는 `$msec $request_time $upstream_connect_time $upstream_header_time $upstream_response_time $status`를 기록합니다. 턴이 미완료이거나 자식 프로세스가 실패하면 드라이버는 0이 아닌 코드로 종료하며, 프록시 로그 회전 실패도 러너를 중단합니다.

제공자 분산이 “측정값을 삼키는” E2E 문제를 프록시 경계 측정으로 해결: Groq 앞에 nginx를 두어 **호출별 업스트림(WAN+Groq) 시간을 기록**하고, 턴 왕복에서 이를 뺀 나머지(그래프 + HTTP 클라이언트 직렬화 + 로컬 MCP + 파이프)만 비교한다. 통계적 우회(ABBA/재시도 횟수 증가)가 아니라 노이즈 원천 자체를 측정하고 빼는 방식이므로, 라운드가 서로 다른 Groq 구간에 걸쳐도 결과가 흔들리지 않는다.

|  | 턴당 평균 업스트림 | **잔차 p50** | 잔차 p90 | 잔차 min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 원시 벽시계 시간은 이번에 "LG가 189ms 더 빠름"을 보여줍니다 (Groq이 NG에 더 나쁜 라운드 시간 창을 제공 — 업스트림 평균 +196ms). 잔차는 **NG p50 −11.1ms** — 측정 노이즈 방향과 무관하게 방법이 신호를 복원함을 보여주는 명확한 연관입니다. (correction: "신호을" → "신호를", "증시" → "증거", and "업스트림 젖요" (garbled) corrected to "업스트림 평균")
- 잔차 p50은 mock 라운드 예측과 일치함 (그래프 0.4 대 3.1ms + HTTP 스택 차이) — 페이로드 교차 검증 성공.
- Call↔turn 매핑은 **순서 기반**(호출 수 = 2×turn 수, 로그 순서 = turn 순서)이며, 시간 블록 매핑은 historical wall-clock step(실행 중 −0.8초 역전 측정)을 fallback 전용으로 사용. Driver 타임스탬프도 monotonic anchor에서 파생.
- 트랩 주의: Groq(Cloudflare)가 `Python-urllib` UA를 403으로 차단합니다 — 프록시 문제로 오인하기 쉽습니다. 실제 스모크 테스트는 curl/httpx 계열 UA를 사용합니다.

## 스트리밍 TTFT 라운드 (2026-07-05)

이 과거 round는 두 synthesis call을 streaming으로 변경했습니다. 당시 C++ streaming provider와 LangGraph `SYNTH_LLM.stream()`를 사용했으며 현재 C++ 경로는 `ProviderMode::Stream`/`sp::Event`입니다. driver는 `[jarvis:ttft]`로 turn-send → first synthesis text를 측정합니다. nginx의 `proxy_buffering off`는 SSE를 통과시키며 `$upstream_header_time`은 첫 byte입니다. round별 로그(mv + `nginx -s reopen`)로 round 경계를 분리합니다.

|  | 인지된 TTFT p50 | 완료 시간 p50 | 턴당 평균 업스트림 |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **인지된 TTFT가 사실상 동일(차이 −2ms).** 이전에 NeoGraph의 TTFT가 더 느렸던 것(800 vs 603)은 순수한 제공자 분산이었음 — 이번에는 Groq가 양쪽에 공정한 윈도우를 제공해(업스트림 726 vs 753) 간격이 사라졌음. "NG 라운드는 단지 불운"이라는 의심이 재현을 통해 확인됨.
- **과거 completion residual** — NeoGraph4.1ms/LangGraph14.6ms(이전 proxy3.5/14.7)였습니다. residual에는 client serialization, local MCP와 pipe overhead가 포함되므로 graph computation만의 직접 측정은 아닙니다.
- **TTFT-잔차는 ±수십 ms 노이즈 내에서 0입니다** (음수도 나타남). 인지된 TTFT 625ms 대 업스트림 합계 673ms와 비교 시, 두 독립적 클록(클라이언트 모노토닉 vs nginx 벽시계)을 빼는 해상도(±50ms)가 프레임워크 기여도(ms)보다 큽니다. 즉, **프레임워크 차이는 TTFT 경로의 관측 한계 미만입니다** — 신호는 총 잔여/모의에서만 노이즈 위에 나타납니다.
- **스트리밍 관찰** — 과거 first-synthesis-text는631ms, completion은744ms였습니다. `[jarvis:ttft]`는 텍스트 marker이며 최초 가청 TTS 재생이나0.6초 청취 시작을 측정하지 않습니다.

과거 streaming text-marker TTFT는 동률이었습니다. 이 측정은 현재 SDK transport, audio latency 또는 production tenant capacity를 검증하지 않습니다.

## 공정성 조건

- 프롬프트(persona.txt 공유) · 결정 검증(채팅 다운그레이드) · 메모리 형식(JsonFileStore) · 원문 보호 · stdout 마커 동일. 프레임워크와 언어만 다릅니다.
- LangGraph 쪽은 관용적 스택(langgraph + langchain-openai)을 사용합니다.
- 측정은 컨테이너 내부 측정 `driver.py` (stdin 주입 → `[jarvis:tts]` 마커 왕복).
- 초기 출력 cap: 양쪽 모두 명목 응답 router 300 / synthesis 220 토큰에 reasoning 여유분 1024토큰을 더하고 낮은 reasoning effort를 요청합니다. 완료된 빈 `MaxTokens` 응답을 cap 두 배로 한 번 재요청하는 것은 C++ 쪽뿐입니다. 추가 provider 호출이 있으면 분석기의 턴당 두 호출 잔차 가정이 무효이므로 불일치 경고를 비교 가능한 측정값으로 취급하면 안 됩니다.

## 파일

- `langgraph_twin.py` — LangGraph 트윈(동일 토폴로지·프로토콜, MCP_URL이 설정되면 공식 mcp SDK 영구 세션을 통한 실제 도구 호출)
- `driver.py` / `analyze.py` — 측정 · 비교 표
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` / `Dockerfile.proxy` — 벤치마크 이미지
- `run_bench.sh`(Core) / `run_bench_e2e.sh`(실제 도구 E2E) / `run_bench_proxy.sh`(TLS 프록시 경계 측정) — 실행기
- `nginx-openrouter.conf` — 프록시 설정; `analyze_proxy.py` / `analyze_ttft.py` — 그 로그 분석기
- `turns_mock.txt`(200) / `turns_openrouter.txt`(20) / `turns_e2e.txt`(24) — 턴 세트; `turns_openrouter.txt`는 나머지 두 파일에서 채팅 턴만 그대로 골라 낸 것
- `../config-bench/` — 빈 카탈로그(채팅 경로 고정됨) / `../config-bench-e2e/` — 공유 MCP 서버 카탈로그
