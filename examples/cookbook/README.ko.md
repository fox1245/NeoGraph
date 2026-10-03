<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=01f0466eb43755a45f571b1f6bcdd22e270983bc426fb68d843fdc881e63aa3e -->
# NeoGraph 쿡북


## 타입 C++ 전환 상태

현재 C++ recipe는 다섯 타입 SDK family의 소유된 `ProviderRequest`, 순서 있는
`sp::Event`, 불변 `std::shared_ptr<const sp::Outcome>` (`Completion`/`Failure`)을
사용합니다. `ChatMessage`/`ChatTool`은 portable 투영이며 native replay 권한이 아닙니다.
prepare는 정확히 한 번 수행합니다. durable 호출자는
`Provider::request_digest(prepared)`에 claim/receipt를 연결하고 같은 handle을 dispatch합니다.
출력 JSON으로 history를 재구성하거나 failure를 최종 텍스트로 축소하지 않습니다.

canonical persistence는 `provider-message-v2`/`runtime-history-record-v2`를 사용하며
portable 요약은 native record를 대체하지 않습니다. optional control은 호출자가 선택하고
조용히 clamp하지 않습니다. bounded call에는 진짜 model fact가 필요하며 누락은 `LimitUnknown`입니다.
reservation/charged/held 값은 nullable provider usage와 별개이며 budget 갱신, 가격, forecast, invoice가 아닙니다.

header-only `examples/provider_example_support.h`는 실제 SDK runtime을 사용합니다.
LLM 빌드는 `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)`의 `SchemaProvider::runtime` 또는
명시적 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`가 필요합니다.
설치 include root는 `include/SchemaProvider`입니다. interface/capability 검사를
수행하며 SDK package는 unstable `0.0.0` (interface 3)입니다.

현재 model-free 실행에서는 Assembly 로컬 A2A member 서버 4개와 C++ speaker,
JARVIS CLI synthetic turn·메모리 영속화, Beast strict Core 컴파일·진화·checkpoint 롤백,
ProgramChat 브라우저 tenant 격리·generation 교체 및 PostgreSQL black-box 6개 시나리오를 확인했습니다.
아래 목록은 이 실행 범위와 미검증 surface를 구분합니다. vendor inference, 음성, 보류된 Python binding,
모든 Beast live 변형이나 전용 multitenant server/load의 통과를 주장하지 않습니다.
아래 과거 측정은 새 전환의 qualification이 아닙니다. live 실행에는 키/네트워크/모델 접근과 비용이 필요합니다.
키, prompt, artifact를 비공개로 유지하세요. envelope/native inspection 출력은 민감하므로
공개 log에 내보내지 마세요. native archive는 owner-private 인증 custody이며 암호화나 vendor issuer 인증이 아닙니다.

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

여러 NeoGraph 기능을 실제 동작 시나리오로 결합하는 종단 간 레시피입니다. 각각은 독립적입니다: 폴더를 복사하고, README를 따르고, 실행하세요.

| 쿡북 | 표시되는 내용 |
|---|---|
| [`the-beast/`](the-beast/) | **자가 진화 에이전트: 생성 · 진화 · 롤백.** 이 Beast는 엄격한 Core JSON을 작성하고, 실행 전에 검증하며, `evolve()`로 경계가 있는 Core 토폴로지를 진화시키고, 체크포인트를 통해 롤백합니다. 라이브, 정점, 위조, 스크립트 및 산술 진화 변형은 동일한 컴파일러/검증 경계를 유지합니다; JavaScript 또는 신뢰할 수 있는 C++가 소스 작성의 소유권을 가지는 반면, 엄격한 Core JSON은 상호 교환 데이터로 남습니다. |
| [`ai-assembly/`](ai-assembly/) | 다중 페르소나 A2A: 국회의원 4명(각각 자체 A2A 엔드포인트) + 법안을 병렬로 방송하고 표를 집계하는 국회의장. 교차 언어: C++ 의원 서버 + Python 또는 C++ 의장. |
| [`byo-openai/`](byo-openai/) | 과거 공급자 recipe; Python binding 전환 보류. |
| [`jarvis/`](jarvis/) | **음성 기반 메타 오케스트레이터(스켈레톤).** 마이크 → whisper.cpp(언어 자동 감지) → 라우터(직접/위임/병렬 3방향) → MCP 도구 또는 A2A 전문가 → 사용자의 감지된 언어로 된 슈퍼톤 온디바이스 TTS. JSON 기반 도구 + 에이전트 카탈로그, A2A 양방향(JARVIS 자체도 도달 가능). 온디바이스, 클라우드 필요 없음. |
| [`minimal-mcp/`](minimal-mcp/) | **LLM, API 키, fastmcp 없이** MCP 클라이언트 왕복: ~60줄의 stdlib stdio 서버 + 다음을 수행하는 C++ 하네스 `initialize` → `tools/list` → `tools/call`. NeoGraph의 MCP 클라이언트가 와이어 프로토콜을 말하는 프로세스만 필요하다는 것을 보여줍니다 — 피어는 무엇이든 될 수 있습니다. |
| [`openrouter-provider/`](openrouter-provider/) | 과거 공급자 recipe; Python binding 전환 보류. |

각 쿡북은 또한 표면화된 마찰점을 문서화합니다 — 공용 API의 거친 가장자리를 찾는 데 유용합니다.

## 전체 recipe 및 보류 surface 목록

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 타입 C++ 전환; 실제 로컬 A2A member 서버 4개와 C++ speaker를 offline 실행 검증; synthetic abstention은 모델 판단이 아님 |
| [`byo-openai/`](byo-openai/) | 과거 공급자 recipe; Python binding 전환 보류 |
| [`jarvis/`](jarvis/) | 타입 C++ 전환; CLI synthetic turn·메모리 영속화·정상 EOF 검증; 음성/Python surface는 미검증 |
| [`minimal-mcp/`](minimal-mcp/) | protocol-only client/server; 의도적으로 변경 없음 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 타입 C++ 전환; 전용 server 실행 및 live 1,000/32 load는 미검증 |
| [`openrouter-provider/`](openrouter-provider/) | 과거 공급자 recipe; Python binding 전환 보류 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 타입 C++ 전환; vendor inference 없이 ProgramChat 브라우저 tenant 격리·generation 교체 및 PostgreSQL black-box 6개 시나리오 검증 |
| [`the-beast/`](the-beast/) | 타입 C++ 전환; 실제 strict Core 컴파일·진화·checkpoint 롤백 검증; 모든 live 변형의 통과 주장이 아님 |
| [`topology-retrieval/`](topology-retrieval/) | protocol-only client/server; 의도적으로 변경 없음 |

Python MCP server, Jarvis CLI/REPL driver, retrieval HTTP client는 protocol client이며 provider binding 구현이 아닙니다. Assembly Python speaker와 Jarvis pybind benchmark는 보류된 binding에 의존합니다. live multitenant 1,000/32는 smoke가 아닙니다.
