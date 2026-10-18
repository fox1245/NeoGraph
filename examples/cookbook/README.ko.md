<!-- neograph-i18n: source=examples/cookbook/README.md locale=ko source_sha256=1ba2435bb0bde68ce969a20064ff5577a3e61d0fc1705b904627a8ec4c2c649e -->
# NeoGraph 쿡북

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 타입 C++ 전환 상태

현재 C++ 레시피는 다섯 타입 SDK family의 소유된 `ProviderRequest`, 순서 있는
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
Core-only를 포함한 모든 native 빌드에는 `SchemaProvider::runtime`이 필요합니다.
CMake는 설치된 패키지, 명시적 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`,
또는 immutable public SDK archive fallback 중 하나를 사용합니다. offline installed/source SDK를 쓸 때는
`-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 fetching을 끕니다.
설치 include root는 `include/SchemaProvider`입니다. interface/capability 검사를
수행합니다. SDK `0.3.5` alpha는 interface revision/shared ABI 6을 씁니다.

보존된 interface-3 model-free 실행에서는 Assembly 로컬 A2A member 서버 4개와 C++ speaker,
JARVIS CLI synthetic turn·메모리 영속화, Beast strict Core 컴파일·진화·checkpoint 롤백,
전용 mock topology load와 retrieval index 재사용·admission, ProgramChat 브라우저 tenant 격리·
generation 교체 및 SQLite 6개·PostgreSQL 6개 black-box 시나리오를 확인했습니다.
아래 목록은 이 과거 실행을 설치된 Python application, A2A peer와 Forge helper를 포함한
[이전 interface-4 local 증거](../README.md#typed-c-cutover-status)와 구분합니다.
vendor inference, 음성, 모든 Beast live 변형이나 전용 live multitenant 1,000/32 load는 미검증입니다.
아래 과거 측정은 새 전환의 qualification이 아닙니다. live 실행에는 키/네트워크/모델 접근과 비용이 필요합니다.
키, prompt, artifact를 비공개로 유지하세요. envelope/native inspection 출력은 민감하므로
공개 log에 내보내지 마세요. native archive는 owner-private 인증 custody이며 암호화나 vendor issuer 인증이 아닙니다.


이 레시피는 NeoGraph의 여러 기능을 조합합니다. C++ 타깃은 필요한 SDK 패키지를 갖춘 NeoGraph 트리에서 빌드하세요. 레시피 폴더만 복사해서는 단독 빌드가 되지 않습니다.

| 쿡북 | 보여 주는 것 |
|---|---|
| [`the-beast/`](the-beast/) | **자가 진화 에이전트: 생성 · 진화 · 롤백.** The Beast는 엄격한 Core JSON을 작성하고, 실행 전에 검증하며, `evolve()`로 경계가 있는 Core 토폴로지를 진화시키고, 체크포인트로 롤백한다. live, apex, forge, script, arithmetic-evolution 변형도 같은 컴파일러/검증 경계를 유지한다. 소스 작성은 JavaScript 또는 신뢰할 수 있는 C++가 맡고, 엄격한 Core JSON은 교환용 데이터로만 남는다. |
| [`ai-assembly/`](ai-assembly/) | 다중 페르소나 A2A: 국회의원 4명(각자 자체 A2A 엔드포인트)과, 법안을 모두에게 병렬로 보내고 표를 집계하는 의장(Speaker). 교차 언어: C++ 의원 서버 + Python 또는 C++ 의장. |
| [`byo-openai/`](byo-openai/) | 실제 타입 SDK 경로를 거치는 Python provider 준비(prepare). 호환성과 실행 증거는 이 레시피에 있다. |
| [`jarvis/`](jarvis/) | 음성 기반 메타 오케스트레이터. 선택 사항인 로컬 ASR/TTS가 4방향 라우터(chat/direct/delegate/parallel), MCP 도구, A2A 전문가, 저장되는 대화 메모리를 감싼다. 로컬/mock 동작에는 클라우드가 필요 없고 live 추론은 OpenRouter를 쓴다. |
| [`minimal-mcp/`](minimal-mcp/) | **모델도 API 키도 없는** MCP 클라이언트 왕복. 표준 라이브러리만 쓰는 약 60줄짜리 stdio 서버와, `initialize` → `tools/list` → `tools/call`을 수행하는 C++ 하네스로 이루어진다. NeoGraph의 MCP 클라이언트에는 와이어 프로토콜을 말하는 프로세스만 있으면 되고, 상대(peer)는 무엇이든 될 수 있음을 보여 준다. |
| [`openrouter-provider/`](openrouter-provider/) | 타입 Python OpenRouter provider 요청과 불변 outcome. 호환성과 실행 증거는 이 레시피에 있다. |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 컴파일 캐시를 거쳐 한 프로세스가 여러 고객의 토폴로지를 서비스한다. mock 바이너리는 아무것도 필요 없고, live 바이너리는 키가 필요하다. |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 고객마다 토폴로지를 고르고 진화시키는 채팅 서버. ProgramChat에는 `--mock` 모드가 있고, Core 서버는 키가 필요하다. |
| [`topology-retrieval/`](topology-retrieval/) | 예제 64로 이어지는 후보 선택(dense, BM25, 순위 결합, 재순위). `--mock`은 `pip install faiss-cpu numpy`가 필요하다. |

각 쿡북은 작업 중 부딪힌 마찰점도 문서로 남기며, 공개 API의 거친 부분을 찾는 데 도움이 됩니다.

## 전체 레시피 목록과 증거 범위

| 레시피 | 상태 |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 보존된 interface-3 네 서버/C++ 세션; 현재 설치된 Python Speaker를 local A2A 0.3/1.0 peer로 실행; 합성 기권은 모델 판단이 아님 |
| [`byo-openai/`](byo-openai/) | 설치된 SDK4 wheel과 공식 OpenAI client를 local Chat peer로 실행: known/unknown usage, 실제 tool 함수, 8회 호출 상한, hosted-call guard 검증; vendor inference 없음 |
| [`jarvis/`](jarvis/) | 보존된 interface-3 CLI 영속성/EOF 증거; 현재 Python MCP tool, LangGraph twin/driver, pybind per-turn/startup 정확성 검증; 새 성능·음성 검증은 아님 |
| [`minimal-mcp/`](minimal-mcp/) | 실제 Python stdlib server를 공식 MCP client로 실행해 handshake/discovery와 계산·시간·demo-weather 호출 검증; LLM이나 실제 기상 서비스는 사용하지 않음 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 전용 mock 1,000요청·오류 0·컴파일 topology 3개·cache hit 997; isolated host는 reference metadata만 출력; live 1,000/32 미검증 |
| [`openrouter-provider/`](openrouter-provider/) | 설치된 SDK4 native provider와 custom HTTP node를 local Chat peer로 실행: known/unknown usage, 유지된 history, hosted-call guard 검증; vendor inference 없음 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 실제 브라우저 tenant 격리·generation 교체, SQLite 6개·PostgreSQL 6개 black-box 및 명시 host model policy 검증; vendor inference 없음 |
| [`the-beast/`](the-beast/) | 타입 C++ 전환; 실제 strict Core 컴파일·진화·checkpoint 롤백 검증; 모든 live 변형의 통과 주장이 아님 |
| [`topology-retrieval/`](topology-retrieval/) | mock Python ranking/index 재사용 및 실제 C++ registry admission/migration·unknown-key 거부 검증; 외부 pointer는 권한이 아님 |

Python MCP server, Jarvis CLI/REPL driver, retrieval HTTP client는 protocol client이며 provider binding 구현이 아닙니다. Assembly Python speaker는 A2A binding을, Jarvis pybind benchmark는 전환된 native binding을 사용합니다. 이들의 실행 증거는 위 C++ 실행과 별개입니다. SDK runtime은 현재 Linux/POSIX에서 검증되었으며 macOS/Windows transport 검증을 주장하지 않습니다. live multitenant 1,000/32는 smoke가 아닙니다.

## Provider 제어와 보존된 호출 제한

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
A2A 0.x/1.0 snapshot을 설명합니다. 이 소스 변경은 위 실행 기록을 현재 SDK의 검증이나
live-provider 통과로 바꾸지 않습니다.
