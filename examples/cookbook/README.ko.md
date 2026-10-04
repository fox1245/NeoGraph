<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=3bd4cbc27205ab7259743f31d806c59f387979fc3e3e4c2412e30f4a43c7eee9 -->
# NeoGraph 쿡북


## 타입 C++ 전환 상태

현재 C++ recipe는 다섯 타입 SDK family의 소유된 `ProviderRequest`, 순서 있는
`sp::Event`, 불변 `std::shared_ptr<const sp::Outcome>` (`Completion`/`Failure`)을
사용합니다. `ChatMessage`/`ChatTool`은 portable 투영이며 native replay 권한이 아닙니다.
prepare는 정확히 한 번 수행합니다. durable 호출자는
`Provider::request_digest(prepared)`에 claim/receipt를 연결하고 같은 handle을 dispatch합니다.
로컬 wait가 끝나도 원격 model이 중지되었거나 청구하지 않는다는 증거는 아닙니다. durable receipt는 자동 redispatch를 막지만 external effect의 exactly-once를 보장하지 않습니다.
출력 JSON으로 history를 재구성하거나 failure를 최종 텍스트로 축소하지 않습니다.

canonical persistence는 `provider-message-v2`/`runtime-history-record-v2`를 사용하며
portable 요약은 native record를 대체하지 않습니다. optional control은 호출자가 선택하고
조용히 clamp하지 않습니다. bounded call에는 진짜 model fact가 필요하며 누락은 `LimitUnknown`입니다.
reservation/charged/held 값은 nullable provider usage와 별개이며 budget 갱신, 가격, forecast, invoice가 아닙니다.

header-only `examples/provider_example_support.h`는 실제 SDK runtime을 사용합니다.
Core-only를 포함한 모든 native 빌드는 `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)`의 `SchemaProvider::runtime` 또는
명시적 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>` 또는 immutable public SDK archive fallback을 사용합니다. offline installed/source SDK 구성에는 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 fetching을 끕니다.
설치 include root는 `include/SchemaProvider`입니다. interface/capability 검사를
수행합니다. SDK `0.1.0` alpha의 interface/shared ABI는 4입니다.

보존된 interface-3 model-free 실행에서는 Assembly 로컬 A2A member 서버 4개와 C++ speaker,
JARVIS CLI synthetic turn·메모리 영속화, Beast strict Core 컴파일·진화·checkpoint 롤백,
전용 mock topology load와 retrieval index 재사용·admission, ProgramChat 브라우저 tenant 격리·
generation 교체 및 SQLite6개·PostgreSQL6개 black-box 시나리오를 확인했습니다.
아래 목록은 이 실행 범위와 미검증 surface를 구분합니다. vendor inference, 음성, 전환된 Python binding,
모든 Beast live 변형이나 전용 live multitenant 1,000/32 load의 통과를 주장하지 않습니다.
아래 과거 측정은 새 전환의 qualification이 아닙니다. live 실행에는 키/네트워크/모델 접근과 비용이 필요합니다.
키, prompt, artifact를 비공개로 유지하세요. envelope/native inspection 출력은 민감하므로
공개 log에 내보내지 마세요. native archive는 owner-private 인증 custody이며 암호화나 vendor issuer 인증이 아닙니다.

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

여러 NeoGraph 기능을 결합하는 레시피입니다. C++ 타깃은 필수 SDK 패키지를 제공한 NeoGraph 트리에서 빌드하세요. 폴더만 복사하면 standalone 빌드가 되지 않습니다.

| 쿡북 | 표시되는 내용 |
|---|---|
| [`the-beast/`](the-beast/) | **자가 진화 에이전트: 생성 · 진화 · 롤백.** 이 Beast는 엄격한 Core JSON을 작성하고, 실행 전에 검증하며, `evolve()`로 경계가 있는 Core 토폴로지를 진화시키고, 체크포인트를 통해 롤백합니다. 라이브, 정점, 위조, 스크립트 및 산술 진화 변형은 동일한 컴파일러/검증 경계를 유지합니다; JavaScript 또는 신뢰할 수 있는 C++가 소스 작성의 소유권을 가지는 반면, 엄격한 Core JSON은 상호 교환 데이터로 남습니다. |
| [`ai-assembly/`](ai-assembly/) | 다중 페르소나 A2A: 국회의원 4명(각각 자체 A2A 엔드포인트) + 법안을 병렬로 방송하고 표를 집계하는 국회의장. 교차 언어: C++ 의원 서버 + Python 또는 C++ 의장. |
| [`byo-openai/`](byo-openai/) | 타입 Python 공급자 요청 및 실제 prepared handle 사용. 호환성과 실행 증거는 해당 recipe를 참조하세요. |
| [`jarvis/`](jarvis/) | 음성 기반 메타 오케스트레이터. 선택적 로컬 ASR/TTS, chat/direct/delegate/parallel 4방향 라우터, MCP 도구, A2A 전문가와 대화 메모리를 결합합니다. 로컬/mock은 클라우드 없이 동작하며 live 추론은 OpenRouter를 사용합니다. |
| [`minimal-mcp/`](minimal-mcp/) | **LLM, API 키, fastmcp 없이** MCP 클라이언트 왕복: ~60줄의 stdlib stdio 서버 + 다음을 수행하는 C++ 하네스 `initialize` → `tools/list` → `tools/call`. NeoGraph의 MCP 클라이언트가 와이어 프로토콜을 말하는 프로세스만 필요하다는 것을 보여줍니다 — 피어는 무엇이든 될 수 있습니다. |
| [`openrouter-provider/`](openrouter-provider/) | 타입 Python 공급자 요청 및 실제 prepared handle 사용. 호환성과 실행 증거는 해당 recipe를 참조하세요. |

각 쿡북은 또한 표면화된 마찰점을 문서화합니다 — 공용 API의 거친 가장자리를 찾는 데 유용합니다.

## 전체 recipe 목록과 증거 범위

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 타입 C++ 전환; 실제 로컬 A2A member 서버 4개와 C++ speaker를 offline 실행 검증; synthetic abstention은 모델 판단이 아님 |
| [`byo-openai/`](byo-openai/) | 타입 Python 소스 전환; 과거 측정은 전환된 구현의 검증이 아님 |
| [`jarvis/`](jarvis/) | 타입 C++ 전환; CLI synthetic turn·메모리 영속화·정상 EOF 검증; 음성/Python surface는 미검증 |
| [`minimal-mcp/`](minimal-mcp/) | 실제 stdio handshake/discovery와 계산·UTC·demo-weather 호출 검증; LLM 없음 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 전용 mock1,000요청·오류0·컴파일 topology3개·cache hit997; isolated host는 reference metadata만 출력; live1,000/32 미검증 |
| [`openrouter-provider/`](openrouter-provider/) | 타입 Python 소스 전환; 과거 측정은 전환된 구현의 검증이 아님 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 실제 브라우저 tenant 격리·generation 교체, SQLite6개·PostgreSQL6개 black-box 및 명시 host model policy 검증; vendor inference 없음 |
| [`the-beast/`](the-beast/) | 타입 C++ 전환; 실제 strict Core 컴파일·진화·checkpoint 롤백 검증; 모든 live 변형의 통과 주장이 아님 |
| [`topology-retrieval/`](topology-retrieval/) | mock Python ranking/index 재사용 및 실제 C++ registry admission/migration·unknown-key 거부 검증; 외부 pointer는 권한이 아님 |

Python MCP server, Jarvis CLI/REPL driver, retrieval HTTP client는 protocol client이며 provider binding 구현이 아닙니다. Assembly Python speaker는 A2A binding을, Jarvis pybind benchmark는 전환된 native binding을 사용합니다. 해당 실행 증거는 위 C++ 실행과 별개입니다. SDK runtime은 현재 Linux/POSIX에서 검증되었으며 macOS/Windows transport 검증을 주장하지 않습니다. live multitenant 1,000/32는 smoke가 아닙니다.

## Interface-4 제어와 보존된 호출 제한

`ProviderControls`는 family별 reasoning, sampling, tool selection을 보존합니다.
Chat은 admitted OpenRouter reasoning object, usage/include 제어, 대체 model을,
Responses는 명시적 서버 보관 continuation, verbosity, truncation, include 선택을 제공합니다.
Messages는 manual/adaptive/disabled thinking, effort, cache 제어, tool choice를,
Gemini는 명시적 portable foreign history, thinking level, safety setting, sampling, tool choice를
제공합니다. 지원하지 않는 family/origin/model 조합은 I/O 전에 거부합니다. native message에는
진짜 custody가 필요하며 출력 JSON, cursor, portable 투영으로 만들 수 없습니다.
정확한 타입은 [provider reference](../../docs/reference-en.md)를 참조하세요.

Forge는 low reasoning effort를 요청하고 ask마다 deadline 300초를 고정합니다. 완료된 빈
`MaxTokens` 응답에 text와 유효/무효 client call이 없을 때만 현재 출력 cap의 두 배로 한 번 더
호출할 수 있습니다. 두 outcome과 usage를 모두 보존하며 failure, observer/settlement 오류는
재시도하지 않습니다. 기존 live chatbot 경로 `multi_tenant_chatbot/server_live_llm.cpp`와
`self_evolving_chatbot/server_multi.cpp`의 provider timeout은 180초입니다.
ProgramChat의 별도 CLI 제한이나 공유 factory 기본값은 바꾸지 않습니다.

[번호 예제 계약](../README.md#retained-example-contracts)은 Deep Research의 제한된 cap ladder와
deadline discovery, 예제 16/28, fork/new-turn, 계산된 replay 절약량, evolution file 모드,
A2A 0.x/1.0 snapshot을 설명합니다. 이 소스 변경은 위 실행 기록을 interface-4 또는
live-provider 통과로 바꾸지 않습니다.
