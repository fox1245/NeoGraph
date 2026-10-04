<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ko source_sha256=233aaa7dda31f7a4a50265468f10eced76421314e47324e2b9dea8bf425f179b -->
# 매턴 하니스를 제안하는 챗봇

## 현재 타입 Program chat 계약

`program_chat.cpp`는 `evolving-chat/v6`, `chat.step` version `1.5.0`
(manifest digest `chat.step/v6`), ledger schema `neograph.program-chat-call/v6`를 사용합니다.
기존 browser/HTTP protocol은 변경하지 않았습니다. Alice/Bob owner scope,
reviewed-template generation, 역할별 prompt, effect/capability grant,
정확한 checkpoint lineage와 nonrenewable budget은 host가 소유합니다.

```bash
# configure 전에 SCHEMAPROVIDER_SOURCE를 제공된 SDK checkout 경로로 설정하세요.
cmake -S . -B build-chat -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON -DNEOGRAPH_BUILD_SQLITE=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE"
cmake --build build-chat --target cookbook_program_chatbot
./build-chat/cookbook_program_chatbot --mock --no-env --model program-chat-mock \
  --descriptor-policy examples/cookbook/self_evolving_chatbot/demo-policy.json --db evolving-chat.sqlite \
  --native-archive-dir .chat-native --session demo
```

loopback browser는 `http://127.0.0.1:8768`이며 `alice-demo`/`bob-demo`는 production 인증이
아닌 demo bearer token입니다. live에는 OpenRouter eligible route, 키, 네트워크가 필요하며
CLI default는 GLM-5.3-Flash입니다. archive는 host-only이며 기본 경로는 `DB_PATH.native`입니다.
독립 키와 owner-private custody를 DB와 함께 보존하세요. 이는 인증 custody이며 암호화나
vendor issuer proof가 아닙니다. raw native payload, 키, prompt, ledger artifact를 공개 log에
내보내지 마세요. 이 durable recipe에는 archive가 필요하지만 진짜 C++ in-memory checkpoint sidecar에는 필요 없습니다.

`turn:role`마다 한 번 prepare하고 정확한 `Provider::request_digest(prepared)`를 계산하여
`Provider::conservative_token_upper_bound(prepared)`를 예약하고 pending claim을
durable 저장한 뒤 같은 handle을 dispatch합니다. 전체 호출 범위 상한은 승인된 모델의
input/output 한도, 실제 output cap, hosted invocation 한도와 retry 정책으로 계산합니다.
모델 사실이 없으면 예약과 dispatch 전에 거절합니다. private loopback fixture에도
명시적인 지원 모델 정책 사실이 필요하며 mock은 한도나 provider usage를 만들어내지 않습니다.
예약은 provider usage, forecast, invoice가 아닙니다. nullable wide provider count와
`charged_tokens`는 별개이며 known zero는 누락이 아닙니다. consistent final input/output
evidence가 있고 이전 usage가 unknown이 아니며 transport 내부 재전송이 없을 때만 정산합니다.
그 외에는 원래 예약을 `UnknownHold`로 유지합니다. 예약 초과 report도 charge에 포함하며
`cost`는 unknown입니다.
`--descriptor-policy PATH`(C++ `Options::descriptor_policy_file`)로 호스트 소유 SDK
descriptor-policy JSON을 지정합니다. 내장 codec resource snapshot으로 한 번 승인한 뒤
두 tenant가 공유하며 기본값은 내장 정책입니다. 명시적인 fixture 정책은 정확한 loopback
`openrouter_origins`와 `program-chat-mock`(또는 지정한 fixture 모델)의 한도를 선언해야 합니다.
승인된 정책 identity를 세션 settings에 바인딩하므로 같은 파일 경로라도 내용이 바뀌면 새 세션이
필요합니다. 모델 제안이나 checkpoint는 정책을 선택할 수 없습니다. `--mock`에도 해당 사실이
필요하며 예산 증가는 명시적으로만 지정합니다.
제공된 [demo-policy.json](demo-policy.json)은 `program-chat-mock`의 input 4,096·output 65,536
토큰 사실만 선언합니다. offline recipe에서 명시적으로 선택하세요. live 모델 catalog,
spending grant, usage report나 budget reset이 아니며, 명시적 `--model`은 승인 검사를
약화하지 않고 상속된 모델 환경 설정보다 우선합니다.

restart에서 pending은 `UnknownHold`가 되고 자동 재전송하지 않습니다. completed replay는 같은
prepared digest/reservation을 요구하며 archive에서 원본 불변 Outcome/native role history를
복원하고 settlement/output을 검증합니다. DB, archive/key, session, provider/model/settings,
build identity를 유지하세요. 설정 변경은 새 session이 필요합니다. SDK retry는 off (`max_attempts=1`)입니다.
restart/교체는 budget을 갱신하지 않습니다. model JSON은 제안이지 native authority가 아닙니다.
현재 범위 한정 실행 증거: 실제 ProgramChat PostgreSQL blackbox에서 6개 scenario가
18.989초에 pass했으며 실제 browser에서 Alice/Bob 격리와 generation-2 교체를 관찰했습니다.
이 model-free 증거는 vendor 추론 qualification이나 별도 `multi_tenant_chatbot` server/load
recipe, 모든 storage 변형 또는 유예된 Python 제공자 binding의 실행 증거가 아닙니다.


**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`cookbook_program_chatbot`은 Alice와 Bob의 대화·예산·실행 계보를 분리하고,
매턴 하니스 제안을 평가하는 Program 예제입니다. 승인된 후보는 새로운
immutable ProgramVersion으로 컴파일·등록하고, 대화 중인 assistant의
체크포인트에서 교체합니다. 메인 orchestrator는 같은 논리적 assistant를 기다립니다.

- `direct`: 입력 → 답변 → 다음 하니스 제안
- `review`: 입력 → 초안 → 별도 reviewer 하니스 합성·스폰·대기 → 수정 → 제안
- 화면: 대화, 실제 컴파일된 Core JSON, DSL, 에이전트 트리, 세대, 변경 이유,
  노드 차이, 승인/유지/거절 결과, 누적 모델 사용량과 남은 Program 예산
- 영속화: SQLite와 PostgreSQL 모두 대화·호출 장부·Program 아티팩트·전환을 저장

## 실행

```bash
cmake -S . -B build-chat -G Ninja \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-chat --target cookbook_program_chatbot -j 4
./build-chat/cookbook_program_chatbot --mock --no-env --model program-chat-mock \
  --descriptor-policy examples/cookbook/self_evolving_chatbot/demo-policy.json --db evolving-chat.sqlite
```

브라우저에서 `http://127.0.0.1:8768`을 엽니다. 데모 모드에서는 `검토`, `비교`,
`review`를 포함한 요청이 review 하니스를 제안합니다. 승인된 변경은 다음 턴에
사용됩니다. 강제 교체 체크박스는 두 템플릿을 번갈아 시연하며 품질 향상을 뜻하지 않습니다.

OpenRouter 연결은 환경변수 또는 `.env`에서 읽습니다. 기존 예제의 ZDR 라우팅을
유지하며 기본 모델은 `z-ai/glm-5.3-flash`입니다.

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='z-ai/glm-5.3-flash'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
# 기존 파일을 사용할 때:
./build-chat/cookbook_program_chatbot --live --env-file /path/to/.env \
  --model z-ai/glm-5.3-flash --session glm-demo
```

`--model`은 모델 환경변수보다 우선하고, 프로세스 환경변수는 `.env`보다 우선합니다.
파일을 지정하지 않으면 현재 폴더에서 가장 가까운 `.env`를 찾습니다. `--no-env`로
탐색을 끌 수 있습니다. 키는 출력·저장하지 않습니다. 출력 한도는 호출당 기본
4,096토큰(제공자의 추론 토큰 포함)이며 `--max-output-tokens`로 양의 64-bit 값을 지정합니다.
선택한 host policy의 model limit과 기존 session budget은 계속 적용되며 CLI의 고정 8,192 상한은 없습니다.
`--provider-timeout-seconds`는 호출당 제한을 1~120초로 지정하며 기본값은 120초입니다.
체크포인트 대기는 그 사이의 순차 호출을 고려하고 reviewer의 자식 예산은 180초입니다.
타임아웃된 호출은 예약 예산을 유지하며 자동 재호출하지 않습니다.
GLM 5.3 Flash 채팅은 출력 한도 안에 실제 답변을 남기도록 추론 강도 `low`를 기본으로
사용합니다. `--reasoning-effort default`로 제공자 기본 설정을 사용할 수 있으며,
직접 지정한 값은 해당 모델이 지원해야 합니다. DSL 생성 평가기는 별도 설정을 유지합니다.

PostgreSQL은 WSL native Docker에서 전용 DB를 실행한 뒤 같은 WSL 셸에
`NEOGRAPH_CHAT_POSTGRES_URL`을 설정합니다. 설정하면 대화와 Program 저장소
모두 PostgreSQL을 사용하고, 없으면 `--db`의 SQLite 파일을 사용합니다.

## 구현 범위

모델은 `{plan, reason, confidence}`를 제안합니다. host는 `direct`/`review`,
길이가 제한된 이유, 0~1 범위의 confidence만 허용합니다. 0.7 미만이거나
유효하지 않은 제안은 거절하고, 현재 plan과 같으면 유지합니다.
제안 호출은 네이티브 provider의 JSON 응답 모드를 사용하며, 필드와 허용값은 host가
별도로 검증합니다.
빈 제안이나 출력이 잘린 제안은 거절하되 이미 생성된 답변은 유지합니다. 확인된 사용량은
정산하고 종료 이유를 제안 진단에 남기며, 전송 결과가 불확실한 호출은 자동 재전송하지 않습니다.

이번 예제는 **검토된 템플릿의 매개변수 합성**입니다. 모델이 작성한 임의의
JavaScript를 실행하지 않습니다. 제안이 자신의 권한이나 예산을 발급할 수 없으며,
자식 하니스도 독립된 host grant와 컴파일·승인·게시·바인딩을 통과해야 합니다.
템플릿 승인은 답변 품질의 증명이 아닙니다. QuickJS와 모델 노드의 실행 보장은
`Unmanaged`로 표시합니다.

host는 [SKILL.md](../../../skills/neograph-harness-authoring/SKILL.md)와
`references/chat-template-proposals.md`를 실제 하니스 제안 모델의 문맥에 넣습니다.
답변·reviewer 호출에는 각 역할의 지침을 사용합니다. SKILL 해시와 출력 한도를 세션에
기록하고 실제 프롬프트도 호출 식별자에 포함합니다. 지침이나 모델, 빌드가 바뀌면
새 `--session`을 사용해야 하며 기존 예산을 조용히 초기화하지 않습니다.

같은 스킬의 별도 참고 문서는 QuickJS 소스 작성과 컴파일 진단 수정, 재귀 자식 생성,
체크포인트 교체를 안내합니다. DSL 생성 평가기는 이 작성 지침과 네이티브 API 명세를
모델에 제공하고 반환된 소스를 실제 컴파일러로 검증합니다. 챗봇의 템플릿 제안 모드는
모델에게 컴파일러 도구를 직접 노출하지 않습니다. 이전 빌드의 예제 DB에는 새 세션을 만드세요.

기본 세션 한도는 tenant마다 12턴·모델 호출 100회·20만 토큰입니다. 호출 전에 승인된
전체 호출 범위 토큰 상한을 예약합니다. 이전 usage가 unknown이 아니고 transport 내부
재전송이 없을 때만 일관된 최종 사용량으로 정산하며, 누락된 사용량은 예약을 유지합니다.
모델 한도가 없으면 dispatch 전에 거절하며 요청 바이트로 토큰 상한을 추정하지 않습니다.
금액은 가격을 가정하지 않고 미상으로 표시하며 금액 한도를 제공하지 않습니다.
Program의 컴파일·연산·Core step·자식 수/깊이 예산은 교체나 재시작으로 초기화되지 않습니다.
대기 시간도 세션의 wall-time 한도에 포함됩니다.

프로세스 종료 후 같은 DB·session·모델로 실행하면 다음 요청에서 기존 계보를
복구합니다. 결과가 불확실한 모델 호출은 자동 재전송하지 않습니다. 중단된 컴파일도
무료 재시도하지 않고 조정이 필요한 상태로 남깁니다. 명시적 세션 취소는 복구용
프로세스 종료와 달리 실행 가족을 종료합니다.

단일 서버 프로세스용 예제입니다. loopback에 바인딩하며 `alice-demo`/`bob-demo`
토큰은 owner 선택을 보여 주는 로컬 데모 인증입니다. 운영 인증 시스템은 아닙니다.

## 검증

```bash
python3 examples/cookbook/self_evolving_chatbot/test_program_chat.py \
  ./build-chat/cookbook_program_chatbot
```

SQLite 실행 후 `NEOGRAPH_CHAT_POSTGRES_URL`을 설정해 같은 검증을 PostgreSQL에서
반복할 수 있습니다. 두 tenant 동시 실행, 유지/교체, reviewer 재귀 스폰, 중복 요청,
프로세스 복구, 예산 소진, 인증, 실제 provider 어댑터의 로컬 HTTP 응답 처리를 검증합니다.
외부 OpenRouter 호출 성공 여부는 별도 확인해야 합니다.

기존 `server.cpp`, `server_multi.cpp`는 다음 요청의 Core 그래프를 선택하는 이전
예제로 유지합니다. 새 예제의 Program 세대 교체와는 구현 범위가 다릅니다.
