<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ko source_sha256=acf353dedaa0142260f857bf89a32b1c5cbee7fb7c2b31df5fb67fa7ef131dd3 -->
# 나만의 OpenAI 클라이언트 사용하기

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

기존 `openai.OpenAI()` 클라이언트를 NeoGraph 사용자 정의 `GraphNode` 안에서 사용합니다.
공식 SDK가 HTTP 클라이언트, 재시도 정책, 헤더, SDK 수준 계측을 유지합니다.
NeoGraph는 노드를 스케줄링하고 `ChannelWrite` 결과를 그래프 상태에 적용합니다.

[`hybrid.py`](hybrid.py)는 SDK를 한 번 호출하고 어시스턴트 답변을 추가합니다.
[`hybrid_with_tools.py`](hybrid_with_tools.py)는 한 노드 안에서 SDK 도구 호출 루프를
실행하고 세 Python 함수를 호출한 뒤 최종 답변을 추가합니다. 두 예제는 OpenRouter를
사용하며 기본 모델은 `~deepseek/deepseek-v4-flash-latest`입니다.
`provider={"zdr": true}`를 보내 OpenRouter의 데이터 무보존 라우팅 정책을 요청합니다.
이 설정은 지리적 데이터 상주 정책을 보장하지 않습니다.

## 준비 사항과 로컬 실행

현재 타입 공급자 전환 체크아웃에서 빌드한 wheel과 `openai` 패키지를 설치하세요.
제거된 완료 API를 제공하는 이전 릴리스에서는 이 예제를 실행할 수 없습니다.
이 디렉터리에서 아래 명령을 실행하기 전에 로컬 Chat Completions 프로토콜 서버를
시작하세요. 아래 내용은 검증 절차이며 성공한 실행 기록이 아닙니다.

```bash
python -m pip install openai
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid_with_tools.py
```

`OPENROUTER_BASE_URL`에는 API 접두사가 포함됩니다. 이 로컬 서버는 `/v1`,
OpenRouter는 `/api/v1`을 사용합니다. SDK가 `/chat/completions`를 추가합니다.
정규 루프백 호스트 `127.0.0.1`과 `::1`에서는 환경에 호스팅 키가 있어도 고정된
더미 자격 증명 `local-smoke`를 사용합니다. 다른 호스트는 HTTPS,
`NG_ALLOW_HOSTED_CALLS=1`, `OPENROUTER_API_KEY`가 필요합니다. 명시적으로 허용하지
않으면 요청 전에 상태 코드 2로 종료합니다. 호스팅 호출은 비용이 발생할 수 있습니다.
예제는 기존 `.env`를 읽을 수 있지만 이미 내보낸 환경 변수는 덮어쓰지 않습니다.
키를 커밋하거나 요청의 Authorization 헤더를 기록하지 마세요.

## 그래프 상태와 SDK 요청

각 그래프에는 `START_NODE`와 `END_NODE` 사이에 사용자 정의 노드 하나가 있습니다.
범위가 한정된 `GraphRegistry`에 노드 타입을 등록합니다. 전역 공급자 서브클래스나
완료 트램펄린은 사용하지 않습니다. 노드는 `messages` 채널을 읽고 SDK 요청 앞에
시스템 지시를 넣은 뒤 채널 쓰기를 반환합니다. append 리듀서는 입력 사용자 메시지
다음에 어시스턴트 메시지를 유지합니다. 시스템 메시지는 요청 안에만 남습니다.

`hybrid.py`의 서버는 `model="fixture-model"`, 시스템 메시지, 사용자 메시지,
`temperature=0.7`, `provider={"zdr": true}`를 담은 버퍼링 방식
`POST /v1/chat/completions` 하나를 받습니다. `id`, `object="chat.completion"`,
`created`, `model`, 그리고 `index=0`, 어시스턴트 메시지,
`finish_reason="stop"`을 담은 `choices` 항목 하나가 있는 표준 Chat Completion
JSON 객체를 반환하세요. `usage`는 선택 사항입니다. 예상 상태에는 메시지 두 개가
있고 `sdk_usage`는 SDK 사용량 딕셔너리 또는 `None`입니다. 없는 카운터를 0으로
만들지 않습니다. 도구 호출이 오면 이 텍스트 전용 노드는 이를 버리지 않고 실패합니다.

## 도구 루프

`hybrid_with_tools.py`의 첫 요청에는 함수 도구 `reverse_string`, `word_count`,
`calc`도 선언됩니다. 결정적인 로컬 서버는 서로 다른 id와 JSON 인수 문자열
`{"s":"NeoGraph"}`, `{"text":"the quick brown fox"}`, `{"expr":"17*23+5"}`를
가진 어시스턴트 도구 호출 세 개를 반환할 수 있습니다.
`finish_reason="tool_calls"`로 설정하세요.

노드는 SDK 내부 이력에 어시스턴트 도구 호출 메시지를 추가하고 각 함수를 실행한 뒤
일치하는 `tool_call_id`로 결과를 추가합니다. 두 번째 요청에는 `hparGoeN`, `4`,
`396` 결과가 있어야 합니다. 도구 호출 없이 `finish_reason="stop"`인 어시스턴트
텍스트 응답을 반환하세요. 예상 그래프 상태에는 원래 사용자와 최종 어시스턴트
메시지만 있고, `tool_calls=3`이며, `sdk_usage` 목록에는 항목 두 개가 있습니다.
각 항목은 해당 응답의 사용량 딕셔너리 또는 `None`입니다. 마지막 호출의 사용량을
전체 루프 사용량으로 취급하지 않습니다. 이 교환에서 프로그램은 도구 실행 세 번과
SDK 호출 두 번을 출력합니다.

도구 예외는 모델이 대응할 수 있도록 도구 결과 오류 텍스트가 됩니다. 도구 호출
응답이 여덟 번 연속 오면 제한에 도달하여 오류를 발생시키며 가짜 최종 답변을
쓰지 않습니다. `calc`는 산술 데모를 위해 Python 표현식을 평가합니다.
신뢰할 수 없는 표현식을 위한 샌드박스가 아닙니다.

## 공급자 증거의 경계

이 노드는 애플리케이션이 소유한 JSON 상태를 씁니다. `ProviderOutcome`, 네이티브
재생 권한, 공급자 영수증 또는 도구별 체크포인트를 만들지 않습니다. SDK 재시도와
중간 도구 호출은 노드 안에서 수행되므로 그래프 체크포인트는 해당 요청의 영속
영수증이 아닙니다. 실행 후 클라이언트를 닫습니다.

NeoGraph의 타입 공급자 결과와 네이티브 SDK 전송이 필요하면
[OpenRouter SchemaProvider 예제](../openrouter-provider/README.md)를 사용하세요.
기존 SDK 클라이언트를 이 사용자 정의 노드에 전달하면 클라이언트 설정은 유지되지만
그 클라이언트가 `SchemaProvider`가 되지는 않습니다.
