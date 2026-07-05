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
