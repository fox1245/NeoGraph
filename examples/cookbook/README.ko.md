<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=b668003b55bbf84e6463dc6dbc7c708f77d62a9face15528b6fc7e32caac0182 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 disposition: [`spec/neograph-example-disposition-v1.json`](../../spec/neograph-example-disposition-v1.json).

# 네오그래프 요리책


여러 NeoGraph 기능을 하나의 시스템으로 구성하는 엔드 투 엔드 레시피
실제 작업 시나리오. 각각은 독립적입니다. 폴더를 복사하고,
README를 따라 실행하세요.

|자세한 해설서|그것이 보여주는 것|
|---|---|
|[`the-beast/`](the-beast/)|**자체 진화 에이전트: 생성 · 진화 · 롤백.** Beast는 (1) DSL 표면에서 NeoGraph 토폴로지를 작성하고 3개의 게이트(정교 → 엄격한 컴파일 + 번역 검증 → 정적 검증)를 통해 일관성을 입증하고, (2) 컴파일러를 피트니스 게이트로 사용하여 `evolve()`를 통해 실제 돌연변이 연산자로 진화시키고, (3) 체크포인터로 생존자를 생성하고 되감습니다. 이전 상위 단계로 실행(`load_by_id` 시간 여행). NeoGraph만이 안전하게 만드는 범주: 하네스는 데이터이며 DSL 컴파일러는 단일 노드가 실행되기 전에 일관성을 입증합니다. 오프라인 스텁 작성자인 DeepSeek v4 pro(OpenRouter)가 실제로 컴파일러 진단 자체 복구 루프를 사용하여 하네스를 작성하는 **라이브 변형**, 모델이 도구 카탈로그를 삼키고 바인딩된 도구를 자율적으로 호출하는 ReAct 에이전트를 작성하는 **포지(forge)인 **apex 변형**MCP에 대한 도구를 발견하고 *부족한 도구를 작성*하는 변형**, 모델이 NODE LOGIC(자체 `goto` 흐름을 제어하는 모델 작성 코드를 실행하는 `script_node`, 선택적으로 Sandbox2 격리됨)를 작성하는 **스크립트 변형** 및 **evolve 변형** — 실제 출력 점수 피트니스를 갖춘 밈적 루프(목표를 계산할 때까지 산술 파이프라인의 배선을 발전시킵니다):Darwinian mutation/selection(오프라인으로 검증된 등반) 및 획득한 솔루션의 Lamarckian LLM 주입. 실제 실행(CI 게이터블 건전성)에 대해 일관성 유효성 검사기의 판정을 경험적으로 교차 확인하는 **게이트 평가** 하네스, 수천 개의 퍼징된 돌연변이로 확장하고 보증의 경계(정직한 효과 계약과 관련된 사운드, 런타임이 거짓말하는 계약을 지원함)를 정직하게 매핑하는 **게이트 퍼즈**를 제공합니다.500/500) — 작은 단계의 의미론 + 효과 격자에 대한 Progress/non-fault 정리를 입증하는 정식 동반자 **SOUNDNESS.md**, 샌드박스에 대한 **계약 파생 seccomp** 레이어 및 **baldwin** 쌍 — 하네스 토폴로지의 다윈 대 볼드윈 대 라마르키안 밈적 진화: 기본 변형 (그라데이션이 없는 글로벌 고원) 및 **적대적** 변형(`baldwin_adv`, 사기성 그라데이션 풍경 + 실제 언덕 오르기 학습)여기서 블라인드 검색은 적극적으로 *기만*됩니다. 두 가지 모두 학습 유도 진화가 글로벌 블라인드 진화에 도달할 수 없음을 보여주고(CI 제어) Baldwin-vs-Lamarck 비교를 정직하게 보고합니다. 30 구성 스윕은 Whitley Baldwin>Lamarck 반전이 개별 토폴로지(측정됨, 명명된 메커니즘, 가정되지 않음)에서 재현되지 **않습니다**. 그런 다음 **baldwin_llm** 데모는 모델 자체를 학습 연산자로 만듭니다(`?` 단계 채우기).산술 파이프라인)이므로 토글은 문자 그대로입니다. Baldwin은 각 세대마다 모델을 다시 참조하고 Lamarck는 수정 사항을 유전에 적용합니다(호출 횟수 감소). 그리고 **소설가**는 한 줄짜리 전제를 가벼운 소설 길이의 `.txt`로 바꿉니다. "중간에서 길을 잃음"을 치료하는 명시적인 스토리 상태(outline/bible/summary 채널 + 자체 순환 작가 노드)에 대한 그래프입니다. 각 장은 전체 이전 텍스트가 아닌 압축된 외부화된 상태에 대해 새로 생성되었습니다.단어가 작성되기 전에 연결을 증명하는 일관성 게이트를 사용합니다(오프라인 결정적 스텁, 실제 산문의 경우 `OPENROUTER_API_KEY`).|
|[`ai-assembly/`](ai-assembly/)|다중 인물 A2A: 국회의원 4명(각각 고유한 A2A 엔드포인트) + 법안을 동시에 방송하고 투표를 집계하는 의장. 교차 언어: C++ 구성원 서버 + Python 또는 C++ 스피커.|
|[`byo-openai/`](byo-openai/)|자체 `openai.OpenAI()` 클라이언트 가져오기: NeoGraph의 `Provider`를 서브클래스하여 모든 LLM 호출을 SDK에 위임하고 모든 재시도/Azure/관찰 가능성 구성을 유지합니다. 또한: 에이전트 제공자 패턴을 통한 도구 호출.|
|[`jarvis/`](jarvis/)|**음성 기반 메타 오케스트레이터(스켈레톤).** 마이크 → Whisper.cpp(자동 감지 언어) → 라우터(직접/대리자/병렬 3방향) → MCP 도구 또는 A2A 전문가 → 초음속 온디바이스 TTS, 사용자가 감지한 언어. JSON 기반 도구 + 에이전트 카탈로그, A2A 양방향(JARVIS 자체에 도달 가능). 기기 내에서는 클라우드가 필요하지 않습니다.|
|[`minimal-mcp/`](minimal-mcp/)|**LLM 없음, API 키 없음, fastmcp 없음**을 사용한 MCP 클라이언트 왕복: ~60라인 stdlib stdio 서버 + `initialize` → `tools/list` → `tools/call`를 수행하는 C++ 하네스. NeoGraph의 MCP 클라이언트에는 유선 프로토콜을 말하는 프로세스만 필요하다는 것을 보여줍니다. 피어는 무엇이든 될 수 있습니다.|
|[`ollama-provider/`](ollama-provider/)|Ollama를 통한 로컬 LLM. 두 가지 경로: Ollama의 호환 엔드포인트(새 코드 없음)에 대한 내장 `OpenAIProvider` 또는 기본 `/api/chat`에 대한 사용자 정의 `Provider`. 외부 API 키가 없는 전체 에이전트 스택.|

각 요리책에는 표면에 발생한 마찰도 기록되어 있습니다.
공개 API의 거친 가장자리를 찾습니다.
