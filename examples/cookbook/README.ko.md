<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=b668003b55bbf84e6463dc6dbc7c708f77d62a9face15528b6fc7e32caac0182 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 disposition: [`spec/neograph-example-disposition-v1.json`](../../spec/neograph-example-disposition-v1.json).

# 네오그래프 요리책


여러 NeoGraph 기능을 하나의 시스템으로 구성하는 엔드 투 엔드 레시피
실제 작업 시나리오. 각각은 독립적입니다. 폴더를 복사하고,
README를 따라 실행하세요.

|자세한 해설서|그것이 보여주는 것|
|---|---|
|[`the-beast/`](the-beast/)|**자체 진화 에이전트: 생성 · 진화 · 롤백.** Beast는 strict Core JSON을 작성하고 실행 전에 컴파일·검증하며, `evolve()`로 Core 토폴로지를 진화시키고 체크포인트로 롤백합니다. live, apex, forge, script, evolve 변형도 같은 경계를 사용하며 소스 작성은 JavaScript 또는 신뢰된 C++, strict Core JSON은 교환 데이터로 유지됩니다. |
|[`ai-assembly/`](ai-assembly/)|다중 인물 A2A: 국회의원 4명(각각 고유한 A2A 엔드포인트) + 법안을 동시에 방송하고 투표를 집계하는 의장. 교차 언어: C++ 구성원 서버 + Python 또는 C++ 스피커.|
|[`byo-openai/`](byo-openai/)|자체 `openai.OpenAI()` 클라이언트 가져오기: NeoGraph의 `Provider`를 서브클래스하여 모든 LLM 호출을 SDK에 위임하고 모든 재시도/Azure/관찰 가능성 구성을 유지합니다. 또한: 에이전트 제공자 패턴을 통한 도구 호출.|
|[`jarvis/`](jarvis/)|**음성 기반 메타 오케스트레이터(스켈레톤).** 마이크 → Whisper.cpp(자동 감지 언어) → 라우터(직접/대리자/병렬 3방향) → MCP 도구 또는 A2A 전문가 → 초음속 온디바이스 TTS, 사용자가 감지한 언어. JSON 기반 도구 + 에이전트 카탈로그, A2A 양방향(JARVIS 자체에 도달 가능). 기기 내에서는 클라우드가 필요하지 않습니다.|
|[`minimal-mcp/`](minimal-mcp/)|**LLM 없음, API 키 없음, fastmcp 없음**을 사용한 MCP 클라이언트 왕복: ~60라인 stdlib stdio 서버 + `initialize` → `tools/list` → `tools/call`를 수행하는 C++ 하네스. NeoGraph의 MCP 클라이언트에는 유선 프로토콜을 말하는 프로세스만 필요하다는 것을 보여줍니다. 피어는 무엇이든 될 수 있습니다.|
|[`openrouter-provider/`](openrouter-provider/)|OpenRouter 고정 DeepSeek provider 예제. 호환 endpoint용 내장 `OpenAIProvider`와 직접 HTTP를 사용하는 사용자 정의 Python `Provider`를 비교합니다.|

각 요리책에는 표면에 발생한 마찰도 기록되어 있습니다.
공개 API의 거친 가장자리를 찾습니다.
