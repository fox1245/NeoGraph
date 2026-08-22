<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=2b960566263f063bf11a97a63b315005e7ab13700b5839294441f20eb52f6256 -->
# NeoGraph 쿡북

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

여러 NeoGraph 기능을 실제 동작 시나리오로 결합하는 종단 간 레시피입니다. 각각은 독립적입니다: 폴더를 복사하고, README를 따르고, 실행하세요.

| 쿡북 | 표시되는 내용 |
|---|---|
| [`the-beast/`](the-beast/) | **자가 진화 에이전트: 생성 · 진화 · 롤백.** 이 Beast는 엄격한 Core JSON을 작성하고, 실행 전에 검증하며, `evolve()`로 경계가 있는 Core 토폴로지를 진화시키고, 체크포인트를 통해 롤백합니다. 라이브, 정점, 위조, 스크립트 및 산술 진화 변형은 동일한 컴파일러/검증 경계를 유지합니다; JavaScript 또는 신뢰할 수 있는 C++가 소스 작성의 소유권을 가지는 반면, 엄격한 Core JSON은 상호 교환 데이터로 남습니다. |
| [`ai-assembly/`](ai-assembly/) | 다중 페르소나 A2A: 국회의원 4명(각각 자체 A2A 엔드포인트) + 법안을 병렬로 방송하고 표를 집계하는 국회의장. 교차 언어: C++ 의원 서버 + Python 또는 C++ 의장. |
| [`byo-openai/`](byo-openai/) | 자체 클라이언트 가져오기 `openai.OpenAI()`: NeoGraph의 `Provider`를 하위 클래스로 만들어 모든 LLM 호출을 SDK에 위임하고, 재시도 / Azure / 관찰 가능성 구성을 모두 유지하세요. 또한 agentic-provider 패턴을 통한 도구 호출도 가능합니다. |
| [`jarvis/`](jarvis/) | **음성 기반 메타 오케스트레이터(스켈레톤).** 마이크 → whisper.cpp(언어 자동 감지) → 라우터(직접/위임/병렬 3방향) → MCP 도구 또는 A2A 전문가 → 사용자의 감지된 언어로 된 슈퍼톤 온디바이스 TTS. JSON 기반 도구 + 에이전트 카탈로그, A2A 양방향(JARVIS 자체도 도달 가능). 온디바이스, 클라우드 필요 없음. |
| [`minimal-mcp/`](minimal-mcp/) | **LLM, API 키, fastmcp 없이** MCP 클라이언트 왕복: ~60줄의 stdlib stdio 서버 + 다음을 수행하는 C++ 하네스 `initialize` → `tools/list` → `tools/call`. NeoGraph의 MCP 클라이언트가 와이어 프로토콜을 말하는 프로세스만 필요하다는 것을 보여줍니다 — 피어는 무엇이든 될 수 있습니다. |
| [`openrouter-provider/`](openrouter-provider/) | OpenRouter 고정 DeepSeek 공급자 서피스: 호환성 엔드포인트에 대한 기본 제공 `OpenAIProvider` 및 직접 HTTP를 사용하는 사용자 지정 Python `Provider`. |

각 쿡북은 또한 표면화된 마찰점을 문서화합니다 — 공용 API의 거친 가장자리를 찾는 데 유용합니다.
