<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=ko source_sha256=9a8d8defc5b23d66cb6abe96f28e5a3dd82b273a8833a472cf355d9f5b836b35 -->
# JARVIS 오케스트레이션 벤치마크 — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

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
- 90× startup · 9× RSS는 **고정 격차**로 추론 속도와 무관하다 — 엣지 상시 가동 · 콜드 스타트 · 다중 테넌트(100 JARVIS = <1GB) 환경에서 즉시 관련성이 있다.

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

제공자 분산이 “측정값을 삼키는” E2E 문제를 프록시 경계 측정으로 해결: Groq 앞에 nginx를 두어 **호출별 업스트림(WAN+Groq) 시간을 기록**하고, 턴 왕복에서 이를 뺀 나머지(그래프 + HTTP 클라이언트 직렬화 + 로컬 MCP + 파이프)만 비교한다. 통계적 우회(ABBA/재시도 횟수 증가)가 아니라 노이즈 원천 자체를 측정하고 빼는 방식이므로, 라운드가 서로 다른 Groq 구간에 걸쳐도 결과가 흔들리지 않는다.

|  | 턴당 평균 업스트림 | **잔차 p50** | 잔차 p90 | 잔차 min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 원시 벽시계 시간은 이번에 "LG가 189ms 더 빠름"을 보여줍니다 (Groq이 NG에 더 나쁜 라운드 시간 창을 제공 — 업스트림 평균 +196ms). 잔차는 **NG p50 −11.1ms** — 측정 노이즈 방향과 무관하게 방법이 신호를 복원함을 보여주는 명확한 연관입니다. (correction: "신호을" → "신호를", "증시" → "증거", and "업스트림 젖요" (garbled) corrected to "업스트림 평균")
- 잔차 p50은 mock 라운드 예측과 일치함 (그래프 0.4 대 3.1ms + HTTP 스택 차이) — 페이로드 교차 검증 성공.
- Call↔turn 매핑은 **순서 기반**(호출 수 = 2×turn 수, 로그 순서 = turn 순서)이며, 시간 블록 매핑은 WSL2 wall-clock step(실행 중 −0.8초 역전 측정)을 fallback 전용으로 사용. Driver 타임스탬프도 monotonic anchor에서 파생.
- 트랩 주의: Groq(Cloudflare)가 `Python-urllib` UA를 403으로 차단합니다 — 프록시 문제로 오인하기 쉽습니다. 실제 스모크 테스트는 curl/httpx 계열 UA를 사용합니다.

## 스트리밍 TTFT 라운드 (2026-07-05)

현대 LLM 서비스는 모두 스트리밍하므로 벤치마크도 그에 맞춘다: 두 synth 호출 모두 스트리밍으로 변경됨 (C++ `invoke(p, on_chunk)`, LangGraph `SYNTH_LLM.stream()`), 드라이버는 `[jarvis:ttft]` 마커로 **턴 전송 → 첫 synth 토큰** 시간을 측정. nginx는 `proxy_buffering off`로 SSE를 통과시키므로 `$upstream_header_time`가 실제 첫 바이트입니다. 라운드별 분리 로그 (mv + `nginx -s reopen`) 를 사용하여 라운드 분할 추측을 제거.

|  | 인지된 TTFT p50 | 완료 시간 p50 | 턴당 평균 업스트림 |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **인지된 TTFT가 사실상 동일(차이 −2ms).** 이전에 NeoGraph의 TTFT가 더 느렸던 것(800 vs 603)은 순수한 제공자 분산이었음 — 이번에는 Groq가 양쪽에 공정한 윈도우를 제공해(업스트림 726 vs 753) 간격이 사라졌음. "NG 라운드는 단지 불운"이라는 의심이 재현을 통해 확인됨.
- **완료 시간 잔차(순수 프레임워크) 재현**: NeoGraph 4.1ms vs LangGraph 14.6ms (이전 프록시 라운드 3.5 vs 14.7과 일치). 프레임워크 오버헤드 결론은 확고함.
- **TTFT-잔차는 ±수십 ms 노이즈 내에서 0입니다** (음수도 나타남). 인지된 TTFT 625ms 대 업스트림 합계 673ms와 비교 시, 두 독립적 클록(클라이언트 모노토닉 vs nginx 벽시계)을 빼는 해상도(±50ms)가 프레임워크 기여도(ms)보다 큽니다. 즉, **프레임워크 차이는 TTFT 경로의 관측 한계 미만입니다** — 신호는 총 잔여/모의에서만 노이즈 위에 나타납니다.
- **스트리밍 이점**: 인지된 TTFT(631) ≪ 완료 시간(744) — 사용자는 0.6초 만에 답변 듣기를 시작합니다. 완료를 기다리는 비스트리밍 방식보다 인지된 속도 개선이 확인되었습니다.

요약: 프레임워크 순수 성능은 NeoGraph가 유리하며(총 잔차·모의(mock), 재현 가능), **TTFT는 스트리밍에서 동률이며 공급자 분산이 지배적입니다**. 엣지/멀티테넌트(시작 90배·RSS 9배)는 여전히 NeoGraph의 실제 전장입니다.

## 공정성 조건

- 프롬프트(persona.txt 공유) · 결정 검증(채팅 다운그레이드) · 메모리 형식(JsonFileStore) · 원문 보호 · stdout 마커 동일. 프레임워크와 언어만 다릅니다.
- LangGraph 쪽은 관용적 스택(langgraph + langchain-openai)을 사용합니다.
- 측정은 컨테이너 내부 측정 `driver.py` (stdin 주입 → `[jarvis:tts]` 마커 왕복).

## 파일

- `langgraph_twin.py` — LangGraph 트윈(동일 토폴로지·프로토콜, MCP_URL이 설정되면 공식 mcp SDK 영구 세션을 통한 실제 도구 호출)
- `driver.py` / `analyze.py` — 측정 · 비교 표
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — 벤치마크 이미지
- `run_bench.sh`(Core) / `run_bench_e2e.sh`(실제 도구 E2E) — 실행기
- `turns_mock.txt`(200) / `turns_openrouter.txt`(20) / `turns_e2e.txt`(24) — 턴 세트
- `../config-bench/` — 빈 카탈로그(채팅 경로 고정됨) / `../config-bench-e2e/` — 공유 MCP 서버 카탈로그
