# 매턴 하니스를 제안하는 챗봇

**Languages:** [English](README.md) | [한국어](README.ko.md)

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
./build-chat/cookbook_program_chatbot --mock --db evolving-chat.sqlite
```

브라우저에서 `http://127.0.0.1:8768`을 엽니다. 데모 모드에서는 `검토`, `비교`,
`review`를 포함한 요청이 review 하니스를 제안합니다. 승인된 변경은 다음 턴에
사용됩니다. 강제 교체 체크박스는 두 템플릿을 번갈아 시연하며 품질 향상을 뜻하지 않습니다.

OpenRouter 연결은 키와 모델명을 환경변수로 지정합니다. 기존 예제의 ZDR 라우팅을
유지하므로 해당 경로를 지원하는 모델을 사용합니다. `.env` 자동 로드는 하지 않습니다.

```bash
export OPENROUTER_API_KEY='...'
export OPENROUTER_MODEL='provider/model-id'
./build-chat/cookbook_program_chatbot --live --session openrouter-demo
```

PostgreSQL은 WSL native Docker에서 전용 DB를 실행한 뒤 같은 WSL 셸에
`NEOGRAPH_CHAT_POSTGRES_URL`을 설정합니다. 설정하면 대화와 Program 저장소
모두 PostgreSQL을 사용하고, 없으면 `--db`의 SQLite 파일을 사용합니다.

## 구현 범위

모델은 `{plan, reason, confidence}`를 제안합니다. host는 `direct`/`review`,
길이가 제한된 이유, 0~1 범위의 confidence만 허용합니다. 0.7 미만이거나
유효하지 않은 제안은 거절하고, 현재 plan과 같으면 유지합니다.

이번 예제는 **검토된 템플릿의 매개변수 합성**입니다. 모델이 작성한 임의의
JavaScript를 실행하지 않습니다. 제안이 자신의 권한이나 예산을 발급할 수 없으며,
자식 하니스도 독립된 host grant와 컴파일·승인·게시·바인딩을 통과해야 합니다.
템플릿 승인은 답변 품질의 증명이 아닙니다. QuickJS와 모델 노드의 실행 보장은
`Unmanaged`로 표시합니다.

기본 세션 한도는 tenant마다 12턴·모델 호출 100회·20만 토큰입니다. 호출 전에
예약하고 실제 사용량으로 정산하며, 사용량이 없으면 예약량을 유지합니다. 입력 예약은
UTF-8 바이트와 여유분을 사용하며 모델별 토크나이저의 정확한 계산은 아닙니다.
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
