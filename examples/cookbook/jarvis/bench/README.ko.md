<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=ko source_sha256=28ea3137d4e9a60940cf0193feba3c0bb112948387f6e46dcc95360ef28ce6e3 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# JARVIS 오케스트레이션 벤치마크 — NeoGraph 대 LangGraph


동일한 토폴로지 미러링(마이크→stt→병합→메모리→라우터→4방향→synth/skip→커밋→tts)
NeoGraph(C++ 모의 빌드) 및 LangGraph(Python 트윈 `langgraph_twin.py`)에서,
동일한 제약 조건(`--cpus=2 --memory=2g`) 컨테이너에서 측정합니다.

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 과거 측정 결과 (2026-07-05, OpenRouter 전환 전; Groq)

|미터법|네오그래프|랭그래프|델타|
|---|---|---|---|
|순수 그래프 overhead/turn (모의 0ms LLM, 200 회전)|**0.38ms**|3.07ms|+2.7ms(8.1×)|
|Groq real inference/turn(8b 라우터+70b 신디사이저, 20턴)|684ms|706ms|+22ms(~3%)|
|그로크 p99|775ms|870ms|+95ms(n=20, 노이즈 마진)|
|콜드 스타트|7.9ms|716ms| ~90× |
|RSS (모의)|7.5MB|68MB| ~9× |

해석:
- 그래프 엔진 자체는 양쪽에서 LLM에 비해 저렴합니다(0.4ms 대 3ms). Groq 델타 +22ms
~19ms는 HTTP 클라이언트 스택 차이입니다(langchain-openai httpx+pydantic vs asio).
- 회전 간 격차는 **성장형**입니다. 추론이 빨라질수록 커집니다. — 10% 이상
200ms 회전(Cerebras 수준/단일 호출 경로), 로컬 소형 모델(~50ms/call)의 경우 20-30%.
- 90× 시작 · 9× RSS는 추론 속도와 관련 없는 **고정 간격**입니다 — 즉시
엣지 Always-On, 콜드 스타트, 멀티 테넌트(100 JARVIS = <1GB)와 관련됩니다.

## E2E 라운드 — 실제 MCP 도구 왕복 포함(2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_e2e.sh
```

공유 데모 MCP 서버 컨테이너(time/calc/weather) + 24회전 혼합 세트(직접 공구 호출 ·
병렬 팬아웃 · 채팅 · 메모리 리콜), 각각 2라운드에 대한 ABBA 순서 인터리빙:

|라운드(실행 순서)|평균|p50|최대|메모|
|---|---|---|---|---|
|네오그래프 r1|810ms| 791 | 1052 | |
|언어 그래프 r1|673ms| 667 | 934 | |
|언어 그래프 r2|1442ms| 1025 | 3830 |마지막 7턴 2.4~3.8초 - Groq 스로틀 창|
|네오그래프 r2|689ms| 665 | 983 |LG r2 직후 실행에도 불구하고 안정적|

**결론: 이러한 조건(한국→Groq WAN, ~700ms/turn)에서 공급자 측
분산(라운드 간 ±130~770ms)이 프레임워크 델타를 완전히 삼킵니다.
(모의 측정 ~3ms + HTTP 스택 ~19ms).** 전환 순서가 승자를 뒤집었습니다 —
e2e 턴 대기 시간은 프레임워크의 우월성을 결정할 수 없으며 제어된 모의만 확인할 수 있습니다.
라운드에서는 고정 오버헤드와 startup/memory를 측정합니다. E2E 검증됨: 두 하네스 모두
실제 도구로 올바르게 작동합니다(라우팅 모드는 21/24, direct/parallel 실제와 일치함).
왕복), 시작 74ms 대 1944~2483ms, RSS 14MB 대 122MB가 확인되었습니다.

의미: 프레임워크 차이는 **낮은 분산 +
낮은 절대 대기 시간**(로컬 추론, 동일 데이터 센터 추론)
"빠른 추론". WAN 전반의 클라우드 추론은 관계없이 네트워크를 지배하게 만듭니다.
프레임워크의.

## 경계 측정 라운드 — 공급자 분산 제거(2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
```

프록시 경계 측정을 통해 E2E의 "분산이 델타를 삼키는" 문제를 해결합니다.
**호출별 업스트림(WAN+Groq) 시간**을 기록하려면 Groq 앞에 nginx를 배치하고
잔차만 비교합니다(그래프 + HTTP 클라이언트 직렬화 + 로컬 MCP + 파이프)
왕복 왕복에서 뺀 후. 통계적 해결 방법이 아님(증가
ABBA/재시도 횟수) 그러나 노이즈 소스 자체를 측정하고 빼는 결과
라운드가 다른 Groq 창에 부딪히더라도 흔들리지 마십시오.

| |Avg/turn 업스트림|**잔여 p50**|잔여 p90|잔여 최소~최대|
|---|---|---|---|---|
|네오그래프|1613ms|**3.5ms**|19.1ms| 1.9~80.5 |
|랭그래프|1417ms|**14.7ms**|25.1ms| 10.8~33.3 |

- 원시 벽시계에는 이번에 "LG가 189ms 더 빠릅니다"가 표시됩니다(Groq는 NG 라운드를 더 나쁘게 했습니다)
창 — 업스트림 평균 +196ms). 잔여 쇼 **NG는 p50 -11.1ms** — 명확함
잡음 방향에 관계없이 신호를 복원하는 방법을 시연합니다.
- 잔여 p50은 모의 라운드 예측과 일치합니다(그래프 0.4 대 3.1ms + HTTP 스택 차이) —
페이로드 교차 검증에 성공했습니다.
- 콜‐턴 매핑은 **순서 기반**입니다(콜 횟수 확인 = 2×턴 횟수, 로그 순서 = 턴 순서).
시간 창 매핑에는 다음과 같은 WSL2 벽시계 단계(실행 중 -0.8초 반전으로 측정됨)가 있습니다.
대체 전용. 드라이버 타임스탬프도 단조로운 앵커에서 파생됩니다.
- 트랩 노트: Groq(Cloudflare)는 403으로 `Python-urllib` UA를 차단합니다 — 실수하기 쉽습니다
프록시 문제의 경우. 실제 연기 테스트는 curl/httpx-family UA를 사용합니다.

## TTFT 라운드 스트리밍 (2026-07-05)

최신 LLM는 모든 스트림을 서비스하므로 벤치마크가 일치합니다. 두 신디 호출 모두 스트리밍으로 변경되었습니다.
(C++ `invoke(p, on_chunk)`, 랭그래프 `SYNTH_LLM.stream()`),
드라이버는 `[jarvis:ttft]` 마커를 사용하여 **턴-센드 → 첫 번째 신디사이저 토큰** 시간을 측정합니다.
nginx는 `proxy_buffering off`를 통해 SSE를 전달하므로 `$upstream_header_time`는
실제 첫 번째 바이트. 제거하기 위해 라운드당 별도의 로그(mv + `nginx -s reopen`)
라운드 분할 추측.

| |인지된 TTFT p50|완료 시간 p50|Avg/turn 업스트림|
|---|---|---|---|
|네오그래프|**631ms**|744ms|726ms|
|랭그래프|**629ms**|723ms|753ms|

- **TTFT는 효과적으로 묶인 것으로 인식됩니다(델타 -2ms).** NeoGraph의 분명히 느린 TTFT
이전(800 대 603)은 순수한 공급자 분산이었습니다. 이번에는 Groq가 두 가지 모두를 제공했습니다.
공정한 창(업스트림 726 대 753)으로 격차를 제거합니다. "NG 라운드만 불운" 확인
번식에 대한 의심.
- **완료 시간 잔여(순수 프레임워크) 재현**: NeoGraph 4.1ms 대 LangGraph
14.6ms(이전 프록시 라운드 3.5 대 14.7과 일치) 프레임워크 오버헤드
결론은 확실하다.
- **TTFT-residual은 ±tens ms 노이즈 내에서 0입니다**(음수도 나타납니다). 에 비해
인지된 TTFT 625ms vs 업스트림 합 673ms, 2를 빼는 분해능(±50ms)
독립 시계(클라이언트 단조 vs nginx 벽시계)가 프레임워크보다 큽니다.
기여도(밀리초). 즉, **프레임워크 차이는 TTFT 경로의 관찰 한계보다 낮습니다**
— 총 residual/mock에서만 신호가 노이즈 이상으로 나타납니다.
- **Streaming benefit**: Perceived TTFT (631) ≪ completion time (744) — user starts hearing
0.6초 안에 대답하세요. 대기하는 비스트리밍에 비해 체감 속도 향상 확인
완료를 위해.

요약: 프레임워크 순수 성능은 NeoGraph(총 잔여·모의, 재현 가능)를 선호합니다.
그러나 **TTFT는 스트리밍에 묶여 있고 공급자 분산이 지배적인 것으로 인식됩니다**. Edge/multi-tenant
(90× 시작 · 9× RSS)은 NeoGraph의 실제 전장으로 남아 있습니다.

## 공정성 조건

- 프롬프트(persona.txt 공유) · 결정 검증(채팅 다운그레이드) · 메모리 형식(JsonFileStore) ·
축어적 가드 · stdout 마커가 동일합니다. 프레임워크와 언어만 다릅니다.
- LangGraph 측은 관용적 스택(langgraph + langchain-openai)을 사용합니다.
- 측정은 컨테이너 내부 `driver.py`(stdin 주입 → `[jarvis:tts]` 마커 왕복)입니다.

## 파일

- `langgraph_twin.py` — LangGraph 쌍(동일한 토폴로지·프로토콜, 실제 도구 호출을 통해)
MCP_URL가 설정된 경우 공식 mcp SDK 영구 세션)
- `driver.py` / `analyze.py` — 측정 · 비교표
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — 벤치마크 이미지
- `run_bench.sh`(코어) / `run_bench_e2e.sh`(실제 도구 E2E) — 러너
- `turns_mock.txt`(200) / `turns_openrouter.txt`(20) / `turns_e2e.txt`(24) — 턴 세트
- `../config-bench/` — 빈 카탈로그(채팅 경로 고정) /
`../config-bench-e2e/` — 공유 MCP 서버 카탈로그
