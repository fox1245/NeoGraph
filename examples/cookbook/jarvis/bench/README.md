# jarvis 오케스트레이션 벤치 — NeoGraph vs LangGraph

동일 토폴로지(mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)를
NeoGraph(C++ mock 빌드)와 LangGraph(python 쌍둥이 `langgraph_twin.py`)로 미러링,
동일 제약(`--cpus=2 --memory=2g`) 컨테이너에서 측정.

```bash
GROQ_API_KEY=... bash bench/run_bench.sh     # mock 200턴 + groq 20턴 × 양쪽
```

## 실측 결과 (2026-07-05, WSL2, --cpus=2 --memory=2g)

| 지표 | NeoGraph | LangGraph | 델타 |
|---|---|---|---|
| 순수 그래프 오버헤드/턴 (mock 0ms LLM, 200턴) | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq 실추론/턴 (8b 라우터+70b 합성, 20턴) | 684ms | 706ms | +22ms (~3%) |
| Groq p99 | 775ms | 870ms | +95ms (n=20, 노이즈 여지) |
| 콜드 스타트 | 7.9ms | 716ms | ~90× |
| RSS (mock) | 7.5MB | 68MB | ~9× |

해석:
- 그래프 기계 자체는 양쪽 다 LLM 대비 저렴 (0.4ms vs 3ms). Groq 델타 +22ms 중
  ~19ms 는 HTTP 클라이언트 스택 차이(langchain-openai httpx+pydantic vs asio).
- 턴당 격차는 추론이 빨라질수록 커지는 **성장형** — 턴 200ms 대(Cerebras급/
  단일콜 경로)면 10%+, 로컬 소형모델(~50ms/콜)이면 20~30%.
- 기동 90×·RSS 9× 는 추론 속도와 무관한 **고정 격차** — 엣지 상시 구동·
  콜드스타트·멀티테넌트(자비스 100개 = 1GB 미만)에서 즉시 유효.

## E2E 라운드 — 실제 MCP 도구 왕복 포함 (2026-07-05)

```bash
GROQ_API_KEY=... bash bench/run_bench_e2e.sh
```

공유 데모 MCP 서버 컨테이너(시간/계산/날씨) + 24턴 혼합 세트(direct 도구호출·
parallel 팬아웃·chat·기억회상), ABBA 순서 교차로 2회씩:

| 라운드(실행 순) | mean | p50 | max | 비고 |
|---|---|---|---|---|
| neograph r1 | 810ms | 791 | 1052 | |
| langgraph r1 | 673ms | 667 | 934 | |
| langgraph r2 | 1442ms | 1025 | 3830 | 후반 7턴 2.4~3.8s — Groq 스로틀 창 |
| neograph r2 | 689ms | 665 | 983 | LG r2 직후 실행인데 안정 |

**평결: 이 조건(한국→Groq WAN, 턴 ~700ms)에서는 공급자측 분산(라운드 간
±130~770ms)이 프레임워크 델타(mock 실측 ~3ms + HTTP 스택 ~19ms)를 완전히
삼킨다.** 순서를 바꾸자 승자가 뒤집혔다 — e2e 턴 레이턴시로는 프레임워크
우열을 판별할 수 없고, 판별되는 것은 통제된 mock 라운드의 고정 세금과
기동/메모리뿐. E2E 가 검증한 것: 양쪽 하네스 모두 실도구 경로가 정상 동작
(라우팅 모드 일치 21/24, direct/parallel 실왕복), 기동 74ms vs 1944~2483ms,
RSS 14MB vs 122MB 재확인.

시사점: 프레임워크 차이가 유의미해지는 조건은 "빠른 추론"만으로는 부족하고
**낮은 분산 + 낮은 절대 레이턴시**(로컬 추론, 동일 데이터센터 내 추론)가
필요하다. WAN 건너 클라우드 추론에서는 어떤 프레임워크든 네트워크가 지배.

## 경계 계측 라운드 — 공급자 분산 소거 (2026-07-05)

```bash
GROQ_API_KEY=... bash bench/run_bench_proxy.sh
```

E2E 의 "분산이 델타를 삼킨다" 문제를 프록시 경계 계측으로 해결: nginx 를
Groq 앞에 세워 **콜별 상류(WAN+Groq) 시간을 로깅**하고, 턴 왕복에서 차감한
잔차(= 그래프 + HTTP 클라이언트 직렬화 + 로컬 MCP + 파이프)만 비교한다.
ABBA 나 재시도 횟수를 늘리는 통계적 우회가 아니라 잡음원 자체를 계측해서
빼는 방식 — 라운드가 서로 다른 Groq 창을 만나도 결과가 흔들리지 않는다.

| | 상류 평균/턴 | **잔차 p50** | 잔차 p90 | 잔차 min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 원시 벽시계로는 이번에도 "LG 가 189ms 빠름"(Groq 이 NG 라운드에 더 나쁜
  창을 줌 — 상류 평균 +196ms). 잔차로는 **NG 가 p50 −11.1ms** — 방법이
  잡음 방향과 무관하게 신호를 복원함을 보여주는 대조.
- 잔차 p50 이 mock 라운드 예측(그래프 0.4 vs 3.1ms + HTTP 스택 차)과
  정합 — 실페이로드 교차검증 성공.
- 콜↔턴 매핑은 **순서 기반**(콜 수 = 2×턴수 검증 후 로그 순서 = 턴 순서).
  시간창 매핑은 WSL2 벽시계 스텝(런 중 ±0.8s 역행 실측)으로 오귀속 발생 —
  폴백으로만 사용. 드라이버 타임스탬프도 모노토닉 앵커로 파생.
- 함정 메모: Groq(Cloudflare)는 `Python-urllib` UA 를 403 으로 차단 —
  프록시 문제로 오인하기 쉽다. 실측 스모크는 curl/httpx 계열 UA 로.

## 스트리밍 TTFT 라운드 (2026-07-05)

요즘 LLM 서비스는 전부 스트리밍이라 벤치도 거기 맞췄다: 양쪽 합성 콜을
스트리밍으로 바꾸고(C++ `invoke(p, on_chunk)`, LangGraph `SYNTH_LLM.stream()`),
드라이버가 **turn-send → 첫 합성 토큰** 시각을 `[jarvis:ttft]` 마커로 잰다.
nginx 는 `proxy_buffering off` 로 SSE 를 통과시켜 `$upstream_header_time` 이
진짜 첫 바이트가 된다. 라운드마다 로그를 분리(mv + `nginx -s reopen`)해
라운드 분할 추측을 제거.

| | 체감 TTFT p50 | 완료시간 p50 | 상류 평균/턴 |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **체감 TTFT 사실상 동률(delta −2ms).** 앞선 라운드에서 NeoGraph TTFT 가
  느려 보였던 건(800 vs 603) 순수 공급자 분산 — 이번엔 Groq 가 양쪽에 공평한
  창을 줘서(상류 726 vs 753) 격차가 사라졌다. "NeoGraph 라운드만 운이
  나쁘다"는 의심이 옳았음을 재현으로 확인.
- **완료시간 잔차(프레임워크 순수)는 재현**: NeoGraph 4.1ms vs LangGraph
  14.6ms (직전 프록시 라운드 3.5 vs 14.7 과 정합). 프레임워크 자체
  오버헤드는 NeoGraph 가 낮다는 결론은 견고.
- **TTFT-잔차는 ±수십 ms 노이즈 안에서 0** (음수까지 나옴). 체감 TTFT
  625ms 대비 상류합 673ms 처럼, 두 독립 시계(클라 monotonic vs nginx
  벽시계)를 빼는 해상도(±50ms)가 프레임워크 기여(수 ms)보다 크다. 즉
  **TTFT 경로에선 프레임워크 차이가 관측 한계 아래** — total 잔차/mock
  에서만 신호가 노이즈 위로 드러난다. 이 음수는 버그가 아니라 "무시할
  수준"의 정직한 표현.
- **스트리밍 실익**: 체감 TTFT(631) ≪ 완료시간(744) — 사용자가 답을
  0.6초에 듣기 시작. 완료까지 기다리던 비스트리밍 대비 체감 속도 개선이
  수치로 확인.

정리: 프레임워크 순수 성능은 NeoGraph 우위(total 잔차·mock, 재현),
그러나 **실사용자 체감 TTFT 는 스트리밍에서 둘이 동률이고 공급자 분산이
지배**. 엣지/멀티테넌트(기동 90×·RSS 9×)가 여전히 NeoGraph 의 실질 무대.

## 공정성 조건

- 프롬프트(persona.txt 공유)·결정 검증(chat 강등)·메모리 포맷(JsonFileStore)·
  복창 가드·stdout 마커까지 동일. 다른 것은 프레임워크와 언어뿐.
- LangGraph 쪽은 관용적 스택(langgraph + langchain-openai) 사용.
- 측정은 컨테이너 내부 `driver.py` (stdin 주입 → `[jarvis:tts]` 마커 왕복).

## 파일

- `langgraph_twin.py` — LangGraph 쌍둥이 (동일 토폴로지·프로토콜, MCP_URL
  설정 시 공식 mcp SDK 영구 세션으로 실도구 호출)
- `driver.py` / `analyze.py` — 측정·비교표
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — 벤치 이미지
- `run_bench.sh`(코어) / `run_bench_e2e.sh`(실도구 E2E) — 러너
- `turns_mock.txt`(200) / `turns_groq.txt`(20) / `turns_e2e.txt`(24) — 턴 세트
- `../config-bench/` — 빈 카탈로그 (chat 경로 고정) /
  `../config-bench-e2e/` — 공유 MCP 서버 카탈로그
