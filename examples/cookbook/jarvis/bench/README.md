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

## 공정성 조건

- 프롬프트(persona.txt 공유)·결정 검증(chat 강등)·메모리 포맷(JsonFileStore)·
  복창 가드·stdout 마커까지 동일. 다른 것은 프레임워크와 언어뿐.
- LangGraph 쪽은 관용적 스택(langgraph + langchain-openai) 사용.
- 측정은 컨테이너 내부 `driver.py` (stdin 주입 → `[jarvis:tts]` 마커 왕복).

## 파일

- `langgraph_twin.py` — LangGraph 쌍둥이 (동일 토폴로지·프로토콜)
- `driver.py` / `analyze.py` — 측정·비교표
- `Dockerfile.neograph` / `Dockerfile.langgraph` — 벤치 이미지
- `turns_mock.txt`(200) / `turns_groq.txt`(20) — 턴 세트
- `../config-bench/` — 빈 카탈로그·레지스트리 (chat 경로 고정용)
