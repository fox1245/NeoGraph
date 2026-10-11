<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ko source_sha256=72d9de3ebec44aa6eb7f92a3cabc4a08d177461e8869eaff2b0bd003b353c8d3 -->
# 매턴 하니스를 제안하는 챗봇

## 현재 타입 Program chat 계약

`program_chat.cpp`는 `evolving-chat/v6`, `chat.step` version `1.5.0`
(manifest digest `chat.step/v6`), ledger schema `neograph.program-chat-call/v6`를 사용합니다.
기존 browser/HTTP protocol은 변경하지 않았습니다. Alice/Bob owner scope,
reviewed-template generation, 역할별 prompt, effect/capability grant,
정확한 checkpoint lineage와 nonrenewable budget은 host가 소유합니다.

기존 `server_multi.cpp` live-provider 경로는 호출당 600초를 허용해, 관측된 가장 느린 속도로도
8,192토큰 답변을 마칠 수 있게 합니다. 30초 동안 아무 바이트도 오지 않는 연결은 실패로 처리합니다
(OpenRouter는 헤더를 즉시 보내고 생성 중에는 몇 초마다 keepalive 공백을 보냅니다).
ProgramChat의 `--provider-timeout-seconds` 범위/기본값이나 공유 provider factory의 기본값은
바꾸지 않으며 failure 재전송도 활성화하지 않습니다.

```bash
# Set SCHEMAPROVIDER_SOURCE to the supplied SDK checkout before configuring.
# From the repository root; configure Program, QuickJS control and SQLite (or PostgreSQL).
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
recipe, 모든 storage 변형 또는 전환된 Python 제공자 binding의 실행 증거가 아닙니다.

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`cookbook_program_chatbot`은 Alice와 Bob의 메시지, owner scope, catalog, engine cache, Program family, 호출 ledger와 nonrenewable budget을 분리합니다. 턴마다 bounded Harness 제안을 평가하고 승인된 immutable successor를 assistant의 durable checkpoint에서 교체합니다. orchestrator는 같은 논리적 assistant를 계속 기다립니다. review 경로는 초안 → 독립 Program identity의 reviewer child 합성/대기 → 답변 수정이며 inspector는 Core JSON, DSL, tree, generation, 남은 예산, 이유, topology diff와 admission 결과를 표시합니다. SQLite/PostgreSQL은 artifact, transition, chat, reservation/result와 evolution decision을 저장합니다. OpenRouter는 typed SDK Chat provider를, offline demo는 같은 compiler/admission/runtime API를 사용합니다.

## 빌드와 실행

```bash
cmake -S . -B build-chat -G Ninja \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE"
cmake --build build-chat --target cookbook_program_chatbot -j 4
./build-chat/cookbook_program_chatbot --mock --no-env --model program-chat-mock \
  --descriptor-policy examples/cookbook/self_evolving_chatbot/demo-policy.json --db evolving-chat.sqlite
```

`http://127.0.0.1:8768`에서 Alice/Bob을 선택합니다. demo에서 `review`, `검토`, `비교`는 review Harness를 제안합니다. 첫 답변은 현재 Harness를 사용하고 승인된 변경은 다음 turn의 resume에 적용됩니다. force checkbox는 검토된 두 plan을 교대할 뿐 품질 향상을 주장하지 않습니다.

live OpenRouter의 키와 사용 가능한 모델을 환경에 설정합니다:

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='z-ai/glm-5.3-flash'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
# Or read an existing dotenv file without printing its values:
./build-chat/cookbook_program_chatbot --live --env-file /path/to/.env \
  --model z-ai/glm-5.3-flash --session glm-demo
```

기본 모델은 `z-ai/glm-5.3-flash`입니다. `--model`은 `OPENROUTER_MODEL`보다, process 환경은 dotenv보다 우선합니다. `--env-file` 미지정 시 작업 폴더에서 가장 가까운 `.env`를 찾고 `--no-env`는 이를 끕니다. named chatbot 설정만 읽으며 파일은 데이터로 파싱하고 shell로 source하지 않습니다. GLM 5.3 Flash chat은 output cap 안에 visible reply를 남기도록 `reasoning_effort=low`를 기본 지정합니다. `--reasoning-effort default`는 override를 생략하며 explicit 값은 model/provider가 지원해야 합니다. DSL capability evaluator의 generation 설정은 별개입니다.

ZDR eligible route가 필요합니다. token 가격을 가정하지 않으며 DSL/DB/HTTP body/source에 credentials를 넣지 마세요. provider, model, skill, output cap, build 변경에는 새 `--session`이 필요하고 기존 session은 budget limit을 유지합니다. `NEOGRAPH_CHAT_BASE_URL`은 호환 endpoint를 선택하며 plain HTTP는 literal loopback과 `--allow-loopback-provider`가 필요한 protocol test용입니다.

output limit은 [OpenRouter chat API](https://openrouter.ai/docs/api/api-reference/chat/create-a-chat-completion)의 `max_completion_tokens`로 보냅니다. 기본 4,096은 provider reasoning token을 포함하며 `--max-output-tokens`는 양의 64-bit 값입니다. host model limit/session budget은 유지되고 고정 8,192 CLI cap은 없습니다. `--provider-timeout-seconds`는 1..120초(기본120)입니다. checkpoint wait는 사이의 순차 호출을 포함하며 reviewer child는 별도180초 예산입니다. timeout은 예약을 `UnknownHold`로 유지하고 자동 retry하지 않습니다.

WSL native Docker에서 전용 PostgreSQL DB를 만들고 같은 shell에서 URL을 설정합니다. 예제는 테이블을 만들지만 production deployment/migration manager는 아닙니다.

```bash
export NEOGRAPH_CHAT_POSTGRES_URL='postgresql://USER:PASSWORD@127.0.0.1:PORT/DATABASE'
./build-chat/cookbook_program_chatbot --mock --session postgres-demo
```

이 변수가 있으면 chat와 Program persistence가 PostgreSQL을, 없으면 `--db`가 SQLite를 선택합니다. 각 backend는 빌드에서 끌 수 있습니다.

## 합성되는 것

model은 `{plan, reason, confidence}`를 반환합니다. host는 `direct`/`review`, bounded reason과 [0,1] confidence만 허용합니다. 0.7 미만, invalid JSON, unknown plan은 거절하고 unchanged plan은 `kept`입니다. evolution은 JSON-object response mode를 요청해도 application이 정확한 field/value를 검증합니다. 빈/잘린 proposal은 완성된 답변을 잃지 않고 거절합니다. known usage는 정산하고 stop reason은 diagnostic에 보존하며 transport uncertainty는 자동 redispatch를 막습니다.

검토된 JavaScript template을 매개변수화합니다. source identity, bounded compilation, host semantic/template validation, Catalog admission, generation CAS를 통과해야 합니다. child는 durable synthesis gateway와 독립적으로 저장된 host grant를 통과합니다. model은 grant/credential/arbitrary import/native code/추가 budget을 발급하지 못합니다. reviewed-template synthesis의 검증이지 임의 model-written JavaScript나 답변 품질 증명이 아닙니다. QuickJS generator control과 model node는 `Unmanaged`이며 inspector는 unknown external model effect의 strict replay를 주장하지 않습니다.

## 에이전트 작성 스킬

host는 [SKILL.md](../../../skills/neograph-harness-authoring/SKILL.md)와 `references/chat-template-proposals.md`를 evolution model의 system context에 넣습니다. 답변/reviewer는 역할별 prompt를 사용합니다. skill digest는 session에, 실제 prompt/output cap은 call identity에 바인딩합니다. 이전 build DB는 새 session을 사용하세요.

스킬의 별도 QuickJS authoring/native runtime-handoff guide를 source-generation evaluator가 사용합니다. native compiler manifest를 model에 제공하고 반환 source를 compile한 뒤 rejected diagnostic을 bounded repair에 돌려줍니다. [DSL capability evaluation](../../../docs/DSL_CAPABILITY_EVAL.md)을 보세요. chatbot template 경로는 model-callable compiler tool을 노출하지 않습니다.

## 회계와 복구

tenant session 기본은 12turn/100model call/200,000model token입니다. dispatch 전 admitted whole-window bound를 예약합니다. 이전 usage unknown이나 transport 내부 resend가 없어야 consistent final usage로 정산하고 missing usage는 예약을 유지합니다. model limit 없이는 request byte로 token을 추정하지 않고 dispatch 전에 거절합니다. reservation 초과 report도 charge이며 monetary cost는 unknown입니다. 가격/금액 상한을 제공하지 않습니다.

Program compile/operation/Core step/child-depth/wall-time budget은 restart/replacement로 갱신하지 않으며 idle도 wall-time을 소모합니다. dynamic successor는 held checkpoint에서 host-only `ProgramRuntime::reserve_synthesis`로 먼저 예약합니다.

같은 DB/session/provider/model로 재개하면 첫 turn이 family를 reconnect하고 마지막 checkpoint를 reconcile합니다. stored result는 재사용하며 pending/uncertain call은 자동 재전송하지 않습니다. interrupted compilation intent는 무료 재컴파일 없이 남습니다. replacement 전 저장된 admitted successor는 정확한 held checkpoint에서 publish하고 이미 committed replacement는 lineage로 해결합니다. DB/session당 server process 하나만 지원합니다. loopback listener와 `alice-demo`/`bob-demo`는 production 인증이 아니며 explicit cancellation은 process-loss restart와 달리 family를 닫습니다.

## 검증

```bash
python3 examples/cookbook/self_evolving_chatbot/test_program_chat.py \
  ./build-chat/cookbook_program_chatbot
# Run the same command with NEOGRAPH_CHAT_POSTGRES_URL for PostgreSQL.
```

black-box suite는 concurrent tenant 두 명, keep/swap, recursive reviewer, idempotent request, restart, budget exhaustion, authorization과 실제 provider adapter의 local HTTP fixture를 다룹니다. external OpenRouter 성공은 주장하지 않습니다. `--script scenario.json`은 `{tenant, request_id, message, force_swap?}` 배열이고 `--crash-after-script`는 family를 cancel하지 않고 committed snapshot 후 종료하여 restart를 시연합니다.

## 이전 Core 예제

`server.cpp`/`server_multi.cpp`와 기존 타깃은 다음 request의 Core graph를 선택하며 새 `cookbook_program_chatbot`의 live Program replacement를 보여주지 않습니다. 보존된 전환 전 기록은 단일 Alice 16초(당시 약 $0.003), 고객5명/25turn/76model call의424초·18.99MB·compiled engine3개(당시 약 $0.02)입니다. 최종 topology는 fanout3/reflexive1/simple1이었습니다. 이 기록은 현재 route 가격이나 전환된 source 검증이 아닙니다.
