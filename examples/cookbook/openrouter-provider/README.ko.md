<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=ko source_sha256=2ae7c3fb71401e8c8db0b7c5a6116f56d8076f1c963b768aa55f98931801bc16 -->
# NeoGraph + OpenRouter

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 쿡북은 OpenRouter Chat Completions API에 대한 두 요청 경로를 유지합니다.
둘 다 기본 모델이 `~deepseek/deepseek-v4-flash-latest`이며
`provider={"zdr": true}`로 데이터 무보존 라우팅을 요청합니다.
이 설정은 지리적 데이터 상주를 보장하지 않습니다.

[`via_openai_compat.py`](via_openai_compat.py)는 네이티브 `SchemaProvider`와
내장 `llm_call` 노드를 사용합니다. [`via_http.py`](via_http.py)는 `httpx`로
HTTP 요청과 응답 매핑을 소유하는 사용자 정의 Python `GraphNode`를 사용합니다.
엔드포인트/자격 증명 헬퍼는 공유하지만 전송이나 결과 표현은 공유하지 않습니다.
호환성 예제에는 `httpx`가 필요하지 않습니다.

## 경로 A: 타입 SchemaProvider

호환성 경로는 폐쇄형 버전 지정 `openai.chat` 디스크립터로
`load_provider_descriptor`를 호출합니다. `connection.base_url`에는 오리진을,
`connection.paths`에는 전체 API 경로를 넣습니다. OpenRouter에서는 각각
`https://openrouter.ai`와 `/api/v1/chat/completions`입니다. 알 수 없는 디스크립터
필드는 거부되며 임의 JSON으로 코덱 동작을 주입할 수 없습니다.

타입 `OpenRouterRouting` 값이 `SchemaProviderDefaults`에 `zdr=True`를 설정합니다.
`ProviderRuntimeOptions`는 API 키, 120초 타임아웃, 선택 사항인
`OPENROUTER_CA_FILE`을 제공합니다. 런타임은 libcurl을 사용하며 HTTP2나 WebSocket
전송 선택기는 없습니다. `NodeContext`는 명시적 모델과 시스템 지시를 제공합니다.
`RunConfig.provider_messages`는 `Text` 부분을 가진 타입 `ProviderMessage`를
제공합니다. 내장 노드는 타입 요청을 생성하고 준비한 뒤 공급자를 통해 디스패치합니다.

루프백 검증에서는 `provider_policy_json()`을 읽고 `openai.chat` 계열의
`openrouter_origins`에 정확한 루프백 오리진을 추가합니다. 전체 계열 및 코덱
리소스 데이터를 `load_provider_policy`로 승인한 뒤 디스크립터를 로드합니다.
검증 서버 오리진은 선언된 정책 데이터이며 런타임 엔드포인트 덮어쓰기나 ZDR 검증
우회가 아닙니다.

성공한 결과는 불변 공급자 결과와 타입 메시지 이력을 유지합니다.
`outcome.completion`에는 완료가, 실패 시에는 `outcome.failure`에 실패가 있습니다.
누락된 사용량 카운터는 `None`이고 관측된 0은 `value=0`인 `UsageCount`입니다.
예제는 원시 페이로드나 키를 덤프하지 않고 어시스턴트 텍스트와 알려진 입출력 카운터를
출력합니다. 공급자 실패를 성공한 답변으로 바꾸지 않습니다.

## 경로 B: 사용자 정의 HTTP 노드

`OpenRouterHttpNode`는 그래프 메시지를 읽고 시스템 지시를 앞에 넣어 선택한
엔드포인트로 JSON을 보냅니다. `choices[0].message`를 일반 `ChannelWrite`로
매핑합니다. 사용자 정의 HTTP 헤더, 타임아웃 정책, 응답 변환은 애플리케이션 코드가
소유합니다. append 리듀서는 사용자와 그 다음 어시스턴트 메시지를 유지합니다.
`http_usage`는 응답의 사용량 딕셔너리를 저장하며 없으면 `None`입니다.
실행 후 `httpx` 클라이언트를 닫습니다.

이 경로는 `ProviderOutcome`, 준비된 요청 권한, 네이티브 재생 이력 또는 타입 공급자
사용량을 만들지 않습니다. HTTP 오류가 나면 노드가 실패하며 이 텍스트 전용 예제는
도구 호출을 버리지 않고 실패합니다. 기존 공식 SDK 클라이언트가 호출과 도구 루프를
담당해야 한다면 [BYO OpenAI SDK 쿡북](../byo-openai/README.md)을 사용하세요.

## 준비 사항과 로컬 실행

현재 타입 공급자 전환 체크아웃에서 빌드한 wheel을 설치하세요. 이전 완료 API
릴리스에서는 이 예제를 실행할 수 없습니다. 경로 B에는 `httpx`를 설치하고,
이 디렉터리에서 아래 명령을 실행하기 전에 로컬 Chat Completions 서버를 시작하세요.
아래는 검증 절차이며 성공한 실행 기록이 아닙니다.

```bash
python -m pip install httpx
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_openai_compat.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_http.py
```

`OPENROUTER_BASE_URL`에는 API 접두사가 포함됩니다. 두 경로 모두
`/chat/completions`를 추가합니다. 정규 루프백 호스트 `127.0.0.1`과 `::1`은
환경에 호스팅 키가 있어도 고정 더미 자격 증명 `local-smoke`를 사용합니다.
다른 호스트에는 HTTPS, `NG_ALLOW_HOSTED_CALLS=1`, `OPENROUTER_API_KEY`가
필요합니다. 허용하지 않으면 두 프로그램 모두 요청 전에 상태 코드 2로 종료합니다.
호스팅 호출은 비용이 발생할 수 있습니다. 기존 `.env`는 내보낸 변수를 덮어쓰지
않습니다. 키를 커밋하거나 Authorization 헤더를 기록하지 마세요. 경로 A에서
OpenRouter가 아닌 호스트는 라우팅 정책에 명시적으로 승인되어야 합니다.
호출 허용만으로 OpenRouter 의미 체계를 부여하지 않습니다.

## 로컬 프로토콜과 예상 상태

서버는 시스템 메시지, 사용자 메시지, `model="fixture-model"`,
`provider={"zdr": true}`를 담은 버퍼링 방식 `POST /v1/chat/completions`를 받습니다.
경로 B는 `temperature=0.7`도 보냅니다. 경로 A는 프랑스 수도를, 경로 B는
`17 * 23`을 묻습니다. 경로 A용 서버 응답 예시:

```json
{
  "id": "fixture-chat-1",
  "object": "chat.completion",
  "created": 0,
  "model": "fixture-model",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Paris."},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 8, "completion_tokens": 2, "total_tokens": 10}
}
```

경로 A에서는 실제 성공한 공급자 결과 하나, `result.provider_messages`의 사용자와
어시스턴트, 그래프 상태의 어시스턴트 답변을 예상합니다. 이 응답의 사용량은 입력 8,
출력 2입니다. 알 수 없는 카운터를 검증하려면 `usage`를 생략하세요. 가짜 0을
기대하지 마세요. 경로 B에는 어시스턴트 내용으로 `"391"`을 반환하고 그래프 메시지
두 개와 응답의 `http_usage` 딕셔너리를 예상합니다. 어떤 예제도 모의 공급자로
조용히 전환하지 않습니다.

OpenRouter 요청/응답 레퍼런스:
<https://openrouter.ai/docs/api-reference/overview>
