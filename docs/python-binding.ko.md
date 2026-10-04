<!-- neograph-i18n: source=docs/python-binding.md locale=ko source_sha256=73069d840a47c09f7c2b2b6f74da22c57bf16c51e1837e07042d293c6692dd5d -->
# Python 바인딩

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`는 동일한 C++ 런타임의 pybind11 표면입니다. 휠은 Core, LLM, Program/QuickJS, MCP 및 SQLite 런타임 지속성을 활성화합니다. 선택적 소스 빌드는 컴파일하는 구성 요소만 노출합니다.

이 가이드는 현재 타입 기반 바인딩 API를 설명합니다. `complete`를 노출하는 이전 휠은 다른 제공자 인터페이스를 사용하므로 아래 타입 기반 예제를 실행할 수 없습니다.

```bash
pip install neograph-engine
```

## 타입 기반 제공자 요청과 결과

`Provider`는 요청 준비와 디스패치를 분리합니다. `SchemaProvider`는 SchemaProvider SDK를 통해 계열별 요청을 검증하고 인코딩합니다. `PreparedProviderRequest`는 한 번의 디스패치가 소비할 때까지 그 네이티브 준비 결과를 보유합니다.

| 작업 | Python 시그니처 | 결과 |
|---|---|---|
| 서술자 승인 | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`; 잘못된 폐쇄형 JSON은 `ValueError` 발생 |
| 제공자 생성 | `SchemaProvider(descriptor, options, defaults)` | 승인된 엔드포인트/계열 및 런타임 정책을 가진 제공자 |
| 요청 생성 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 타입 기반 `ProviderRequest` |
| 준비 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 한 번 디스패치 | `provider.dispatch(prepared)` | 소유된 `ProviderOutcome` |
| 준비 후 디스패치 | `provider.invoke(request)` | 소유된 `ProviderOutcome` |

`model`은 명시적으로 지정합니다. `ProviderControls`는 `max_output_tokens`, `temperature`, `top_p` 등의 타입 기반 제한과 생성 제어를 제공합니다. 팩터리는 SDK의 계열별 페이로드를 생성하며 원시 딕셔너리로 대체할 수 없습니다. 스트리밍을 요청하려면 `request.mode`를 `ProviderMode.Stream`으로 설정합니다. `on_event` 지정만으로는 스트리밍 모드를 선택하지 않습니다. `request`는 `cancel_token`, `on_event`, `observer_limits`, 선택적 `timeout_ms`도 보유합니다.

`ProviderControls.provider`와 `response_format`, `SchemaProviderDefaults.provider`, `ProviderToolResult.host`는 분리된 선택적 레코드를 반환합니다. 값이 있으면 레코드를 읽고 수정한 뒤 속성에 다시 대입합니다. `controls.provider.order`만 바꾸면 controls에는 반영되지 않습니다. 레코드를 비우려면 `None`을 대입합니다.

`request.timeout_ms`에 표현 가능한 음이 아닌 값을 지정하면 즉시 절대 마감 시간이 시작됩니다. 준비 직전에 설정합니다. 이후 읽은 값은 남은 밀리초이며 만료되면 0으로 제한됩니다. `None`으로 두면 준비 시 제공자의 기본 제한 시간을 사용합니다. 준비된 핸들을 보관하며 기다려도 마감 시간은 갱신되지 않습니다.

### 서술자와 런타임 정책

서술자는 `load_provider_descriptor`가 수용하는 버전이 명시된 폐쇄형 JSON입니다. 계열, `base_url`, 경로, 필드 바인딩, 인증 규칙을 승인합니다. `model`은 요청에 지정하고 자격 증명은 런타임 옵션에 지정합니다. `ProviderRuntimeOptions`는 `api_key`, `default_timeout_ms`, `ca_file`, `workers` 및 SDK의 전송/자원 제한을 제공합니다. `SchemaProviderDefaults`는 타입 기반 제공자 기본값을 제공합니다. 자격 증명은 런타임 옵션에 보관하고 서술자 파일이나 로그에 넣지 않습니다.

`ProviderDescriptorPolicy.identity`는 SDK 정책의 원시 SHA-256 다이제스트를 담은 Python `bytes`를 반환합니다. 표시할 때는 `policy.identity.hex()`를 사용합니다. 식별자 자체는 UTF-8 텍스트나 16진수 문자열 별칭이 아닙니다.

현재 전송은 libcurl을 사용합니다. `prefer_libcurl` 스위치, 런타임 엔드포인트 재정의, WebSocket 전송은 없습니다. [C++ 제공자 참조](reference-en.md)에서 서술자 승인과 지원 요청 계열을 설명합니다.

[SDK 사용 가이드](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)의 단일 요청 루프백 피어를 시작한 뒤 다른 터미널에서 이 텍스트 전용 예제를 실행합니다. 엔드포인트, 모델, 출력 상한이 그 피어와 일치하며 자격 증명을 보내지 않습니다. HTTPS 피어에는 `NG_EXAMPLE_CA_FILE`로 신뢰할 CA를 지정합니다. 호스팅 엔드포인트에는 서술자 origin과 요청 모델을 명시적으로 바꾸고 그 origin의 자격 증명을 런타임 옵션에 지정합니다. 호스팅 호출에는 비용이 발생할 수 있습니다.

```python
import json
import os
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

descriptor = ng.load_provider_descriptor(json.dumps({
    "descriptor_version": 1,
    "revision": 1,
    "id": "python-guide-chat",
    "family": "openai.chat",
    "connection": {
        "base_url": "http://127.0.0.1:8765",
        "paths": {
            "buffered": "/v1/chat/completions",
            "streaming": "/v1/chat/completions",
        },
    },
    "bindings": {
        "model": "model", "messages": "messages", "stream": "stream",
        "max_output_tokens": "max_tokens", "usage": ["usage"],
    },
    "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                     "tool_calls": "ToolUse", "content_filter": "ContentFilter"},
}))
options = ng.ProviderRuntimeOptions(
    api_key="",
    ca_file=os.getenv("NG_EXAMPLE_CA_FILE", ""),
    default_timeout_ms=30_000,
)
provider = SchemaProvider(descriptor, options=options)
controls = ng.ProviderControls()
controls.max_output_tokens = 64
request = ng.make_provider_request(
    provider, "example-model",
    [ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text("Say hello.")])],
    controls=controls,
)
prepared = provider.prepare(request)
outcome = provider.dispatch(prepared)
print("consumed:", prepared.consumed)
if outcome.failure is not None:
    print("failed:", outcome.failure.error.kind,
          outcome.failure.error.safe_message)
else:
    print(outcome.text)
count = outcome.usage.output_total
print("output tokens:", count.value if count is not None else "unknown")
```

핸들이 필요 없다면 `provider.invoke(request)`가 준비와 디스패치를 함께 수행합니다. `outcome.text`는 표시 가능한 텍스트 투영이며, `outcome.messages`는 전체 타입 기반 파트를 보존합니다. 누락된 사용량 카운터는 `unknown`, 보고된 0은 `0`을 출력합니다.

링크된 합성 응답 피어의 예상 출력:

```text
consumed: True
Hello.
output tokens: 0
```

누락된 카운터 경로를 확인하려면 피어의 `usage` 멤버 전체를 제거하고 다시 시작합니다. 마지막 줄은 `output tokens: unknown`으로 바뀌어야 합니다. 실제 모델의 토큰 계수가 아닌 프로토콜 매핑을 확인하는 예제입니다.

### 인터페이스 4 계열별 제어

SDK 인터페이스/공유 라이브러리 세대 4 wheel이 필요합니다. 패키지 버전, native archive v3, portable JSON v2는 별개입니다. 아래 예제는 제어 구성만 하며 해당 계열의 승인된 제공자가 필요합니다. 첫 루프백은 그대로 둡니다. OpenRouter 전용 제어는 미승인 로컬 origin에서 I/O 전에 거부됩니다.

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]

responses = ng.ProviderControls()
responses.parallel_tool_calls = False
responses.verbosity = ng.ResponsesVerbosity.Low
responses.truncation = ng.ResponsesTruncation.Disabled
responses.responses_include = [ng.ResponsesInclude.ReasoningEncryptedContent]

messages = ng.ProviderControls()
messages.max_output_tokens = 4096
messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
messages.output_effort = ng.MessagesOutputEffort.High
cache = ng.MessagesCacheControl()
cache.ttl = ng.MessagesCacheTtl.FiveMinutes
messages.cache_control = cache
choice = ng.MessagesToolChoice()
choice.mode = ng.MessagesToolChoiceMode.Auto
choice.disable_parallel_tool_use = True
messages.messages_tool_choice = choice

gemini = ng.ProviderControls()
gemini.temperature = 0.7
gemini.gemini_thinking_level = ng.GeminiThinkingLevel.Low
safety = ng.GeminiSafetySetting()
safety.category = ng.GeminiSafetyCategory.Harassment
safety.threshold = ng.GeminiSafetyThreshold.BlockMediumAndAbove
gemini.safety_settings = [safety]
choice = ng.GeminiToolChoice()
choice.mode = ng.GeminiToolChoiceMode.Auto
gemini.gemini_tool_choice = choice
```

기본 생성 후 필드를 대입합니다. 키워드 생성자는 없습니다. 선택적 레코드/벡터는 분리된 복사본이므로 수정 후 다시 대입합니다. Chat reasoning의 `effort/max_tokens/exclude/enabled`, `include_reasoning`, `usage_include`, 대체 `models`는 선언된 OpenRouter origin이 필요합니다. 기존 `reasoning_effort/service_tier/provider/response_format`도 유지됩니다.

Responses verbosity는 `Low/Medium/High`, truncation은 `Disabled/Auto`, include는 `ReasoningEncryptedContent/WebSearchSources/FileSearchResults/MessageOutputTextLogprobs/ComputerCallOutputImageUrl/CodeInterpreterCallOutputs`입니다. `responses_include=None`은 기본 encrypted reasoning을 유지하고 `[]`를 포함한 명시적 목록은 그 목록만 선택하여 이후 native replay가 불가능할 수 있습니다. store/reasoning/hosted tools/tool-call 상한도 유지됩니다.

Messages thinking은 `Manual/Adaptive/Disabled`; Manual budget은 승인된 최소 이상, output cap 미만이며 mode 없이 budget만 있으면 manual입니다. Adaptive/disabled는 budget을 금지합니다. 활성 thinking은 temperature를 생략하지만 모델 금지는 명시적 temperature를 거부합니다. effort는 `Low/Medium/High/Max`, TTL은 `FiveMinutes/OneHour`, tool choice는 `Auto/Any/None_/Tool`; `Tool`은 선언된 client tool `name`이 필요합니다. routing은 선언된 OpenRouter origin이 필요합니다.

Gemini level은 `Minimal/Low/Medium/High`로 `thinking_budget`와 배타적입니다. tool choice는 `Auto/Any/None_/Validated`, `allowed_function_names`는 목록이며 `required_tool`과 배타적입니다. safety category는 `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`, threshold는 `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`입니다. 잘못된 계열/범위/thinking·cap·tool 조합은 I/O 전에 거부합니다. temperature 금지 접두사는 ASCII 대소문자 무시 및 마지막 `/` 뒤 모델로 비교합니다. Chat/Responses: `gpt-5/gpt-6/o1/o3/o4`; Messages: `claude-opus-4-7/claude-opus-4-8/claude-opus-5/claude-sonnet-5/claude-fable-`. [SDK 제어 승인](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)을 참조합니다.

### 승인 전 배포 헤더

`load_provider_descriptor(source, policy=None)`는 `${VAR}`를 리터럴로 처리합니다. 아래 helper는 실제 승인 전에 호스트 값을 처리하며 `source`는 Messages 서술자 JSON 문자열입니다.

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)`는 Messages의 선택적 `ANTHROPIC_WORKSPACE_ID/ANTHROPIC_BETA`를 읽고 미설정/빈 값을 생략합니다. 리터럴 헤더가 환경보다 우선하고 명시적 pair가 둘 다 대소문자 무시로 재정의합니다. 중복 override/잘못된·예약 이름/줄바꿈은 승인 실패입니다. 두 helper는 선택적 `policy`를 받으며 승인된 서술자 변경이나 encoder template 평가를 하지 않습니다.

### Responses 커서와 외부 Gemini 이력

`previous_response_id`는 제공자가 보관한 상태를 선택합니다. 전체 이력 대신 새 입력만 보내고 성공한 첫 Responses 호출의 바인딩된 제어와 실제 terminal ID를 유지합니다.

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses.previous_response_id = first_outcome.messages[-1].id
responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history`는 `ProviderMessage` 벡터이며 단일 응답/JSON 객체가 아닙니다. 와이어로 보내지 않습니다. 실제 client-tool 소유권에는 전체 원래 접두 이력과 cursor ID의 terminal assistant가 필요할 수 있습니다. 서버 보관 텍스트는 빈 벡터도 가능합니다. 후속 프로세스 내 커서의 private terminal 소유권은 전체 replay/archive 권한이 아닙니다. 실패/수정/origin·model·config·route 불일치는 거부합니다. 전체 native replay/archive에는 원래 전체 접두 이력이 필요합니다. [SDK Responses 연속 호출](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)을 참조합니다.

호출자 생성 외부 assistant `Text/ProviderToolCall`에 native state/wire output/signature가 없으면 `gemini.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign`를 명시합니다. 기본값은 `NativeOnly`입니다. 첫 외부 function call만 Google validator 우회를 받으며 텍스트 전용 turn에는 signature가 없습니다. 실제 native group은 엄격히 검증하며 실패 seal 복구/제거/강등, reasoning 권한 수입, portable data에 replay 부여를 하지 않습니다. [SDK 외부 Gemini 이력](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)을 참조합니다.

PortableForeign는 wire metadata도 거부합니다. 커서 출력은 `NativeReplay.complete == False`이며 archive 부적격입니다. private owner는 전체 이력 replay를 승인하지 않습니다.


### 준비된 핸들과 Python 제공자

디스패치는 실패한 경우에도 준비된 핸들을 한 번 소비합니다. 디스패치 전에 `prepared.valid`와 `prepared.error`를 확인하고 이후에는 `prepared.consumed`를 확인합니다. Python 객체를 보관한다고 재사용할 수 있는 것은 아닙니다. 다시 시도하려면 새 요청을 만들고 준비합니다.

Python 하위 클래스는 `Provider(family)`를 호출하고 `get_name()` 및 `prepare(request)`를 구현합니다. 준비를 `SchemaProvider`에 위임하고 그 진짜 준비 핸들을 반환할 수 있습니다. 성공한 SDK 결과를 임의로 만들거나 JSON에서 디스패치 권한을 가져올 수는 없습니다. 공통 `invoke`와 `dispatch`는 네이티브 수명주기를 사용하며 과거 `complete` 메서드를 재정의해도 이를 구현하지 못합니다.

네이티브 실행이 Python `prepare` 재정의 메서드를 호출할 때 `None`을 반환하면 핸들을 소비하기 전에 `TypeError`가 발생합니다. 실제 `PreparedProviderRequest`를 반환해야 합니다.

`NodeContext(provider=provider)`와 `ctx.provider` 대입은 네이티브 공유 소유권 리스로 원래 Python 제공자를 보유합니다. 네이티브 컨텍스트, 컴파일된 노드, 엔진의 복사본은 이 리스를 유지하므로 `ctx.provider`를 재할당하거나 외부 Python 참조가 수거된 뒤에도 같은 객체의 재정의 메서드를 호출합니다. 재할당은 수정 가능한 컨텍스트의 리스만 해제하며 기존 엔진이 아닌 이후 컴파일에만 영향을 줍니다. 객체의 동일성과 수명을 보존할 뿐, 제공자의 변경 가능한 상태를 동결하지는 않습니다. 마지막 리스는 GIL을 획득한 상태에서 Python 소유자를 해제합니다.

제거된 `CompletionParams`, `ChatCompletion`, `OpenAIProvider`, `RateLimitedProvider`에는 호환 별칭이 없습니다. 검증된 `SchemaProvider`를 생성하고 요청 팩터리를 사용합니다. `ChatMessage`와 `ToolCall`은 그래프 편의 값으로 유지되며 SDK 전체 값을 담는 `ProviderMessage`, `ProviderToolCall`과 구별됩니다.

### 결과, 실패, 사용량

`ProviderOutcome`은 소유된 불변 SDK 결과를 보존합니다. `outcome.completion` 또는 `outcome.failure`를 확인합니다. 해당하지 않는 분기는 `None`입니다. 완료와 실패 뷰는 순서가 보존된 전체 메시지, 사용량, 시도 증거, 종료/오류 정보, 보존된 와이어 증거를 유지합니다. 실패에도 부분 출력이 남을 수 있습니다. 부분 텍스트를 성공으로 반환하지 말고 실패 보고에 이 증거를 보존합니다.

`ProviderCompletion.wire_envelope`과 `ProviderPartialCompletion.wire_envelope`은 제공자 계열별 증거이며 `None`일 수 있습니다. 읽기 전에 `wire_envelope is not None`을 확인합니다. 현재 버퍼링 Chat 디코더는 이 값을 `None`으로 두고 전체 응답 JSON을 `raw_events`에 보존합니다. 이 항목은 `type == "chat.completion"`인 `ProviderRawWire`(SDK의 `RawWire`)이며 문서는 `payload`에 있습니다. 봉투가 있다고 가정하지 말고 실제 타입 기반 raw 이벤트를 확인합니다. raw 이벤트가 실패를 성공으로 바꾸지는 않으며, 봉투가 없다고 보존된 응답도 없는 것은 아닙니다. SDK 폴백으로 봉투를 합성하지 않습니다.

소유된 결과나 보존된 타입 기반 뷰를 유지합니다. 실제 메시지, 네이티브 소유권, 존재하는 와이어 문서는 호출 종료 및 제공자 객체의 가비지 수집 후에도 유효합니다. raw payload는 제공자의 비공개 필드를 보존하지만 네이티브 추적에서는 raw 봉투/이벤트와 네이티브 재생/추론을 제외합니다. Python JSON 뷰는 데이터 복사본이며 네이티브 재생 권한이나 재정 권한이 아닙니다.

전체 타입 기반 메시지/파트를 사용하고, 필요한 의미가 논리적 역할과 텍스트라면 그 값을 비교합니다. 체크포인트와 Chat 요청의 텍스트 content는 텍스트 문자열 또는 타입 기반 텍스트 파트 배열로 표현할 수 있습니다. 호출자는 우연히 선택된 직렬화 형태 하나만 요구하면 안 됩니다. 이어서 실행할 때는 평면 텍스트나 JSON 대체물이 아니라 전체 파트와 실제 네이티브 소유권을 사용합니다.

`ProviderMessage.parts`, 완료/부분 결과의 `messages`와 `raw_events`, 사용량의 `extra`/`conflicts`는 내부의 바인딩된 값까지 분리된 리스트나 맵으로 반환합니다. 원래 컬렉션을 교체한 뒤에도 보관한 파트를 안전하게 읽을 수 있으며, 복사본을 수정해도 불변 결과는 바뀌지 않습니다. 메시지를 수정하려면 `parts = message.parts`로 읽고 `parts`를 수정한 뒤 `message.parts = parts`로 대입합니다. `message.parts.append(...)`는 임시 Python 리스트만 변경합니다.

SDK 실패는 반환 데이터입니다. 호스트 관찰자나 예산 정산 실패는 `ProviderOutcomeError`에서 파생된 `ProviderObserverError` 또는 `ProviderBudgetSettlementError`를 발생시키며 실제 결과와 원인을 보존합니다. 따라서 콜백 실패는 재디스패치 권한을 주지 않습니다.

저장된 Python 제공자 또는 그래프 예외의 원인을 반복해서 읽어도 원래 예외 객체와 traceback을 보존합니다. 네이티브 중첩 예외 변환으로 접근한 원인도 같은 규칙을 따릅니다.

사용량 카운터는 `value`와 `evidence`를 가진 `UsageCount`이거나, 알 수 없는 경우 `None`입니다. 보고된 0은 알려진 사용량이며 누락된 카운터와 다릅니다. `count.value`를 읽기 전에 `count is not None`을 확인합니다. 회계 처리에서 `count or 0`을 쓰거나 알 수 없는 입력/출력/전체 카운터를 0으로 대체하지 않습니다.

재개나 이어서 실행할 때 `RunResult.provider_outcomes`는 원래 결과를 순서대로 보존하고 그 뒤에 새 결과를 추가합니다. `RunResult.usage`는 현재 회계 은행을 반영하며, 체크포인트에서 이전 보고를 복원할 수 있습니다. 재개 후 새 제공자 호출이 없어도 사용량이 `None`이나 0이라고 보장할 수 없습니다. 증거 복원은 원래 제공자 요청을 다시 디스패치하거나 두 번 청구해서는 안 됩니다. 보존된 보고는 이전 호출을 설명할 뿐, 새 지출 한도를 부여하지 않습니다.

### 네이티브 이력과 이식 가능한 내보내기

반환된 `ProviderMessage`는 전체 타입 기반 파트와 실제 네이티브 재생 상태를 보유합니다. 네이티브 재생과 와이어 출력은 읽기 전용입니다. 제공자를 직접 이어서 호출할 때는 원래 요청의 전체 `request.messages` 접두 이력을 보존하고 `outcome.messages`를 덧붙입니다. 이 결과 메시지는 반환된 출력이며 전체 입력 이력이 아닙니다. 첫 호출 예제의 `request`와 `outcome`으로 `history = request.messages + outcome.messages`를 구성한 다음 `ng.make_provider_request(provider, "example-model", history, controls=controls)`로 다음 요청을 만듭니다. 이 구성만으로는 아무것도 전송하지 않습니다. 다음 턴에는 새 사용자 메시지나 필요한 도구 결과도 포함해야 합니다.

실제 `NativeContext` 재생은 반환된 Assistant 메시지뿐 아니라 원래 접두 이력도 검사합니다. Assistant 출력만 전달하면 와이어 디스패치 전에 `ReplayIneligible`로 실패합니다. `NativeArchive`에서 그 Assistant 메시지를 로드해도 보관 권한과 접두 이력 바인딩은 유지되므로 원래 전체 접두 이력이 여전히 필요합니다.

그래프에서 `RunConfig.provider_messages`는 전체 이력을 받으며 `RunResult.native_messages`에는 이미 캡처된 전체 이력이 들어 있습니다. 원래 입력을 다시 앞에 붙이지 말고 이 그래프 결과를 이력으로 사용합니다. `RunResult.provider_outcomes`는 보존된 결과를 노출합니다. 노드 안에서는 `RunContext.provider_outcomes`와 `provider_loop_history`가 제공자 증거를 보존합니다.

`RunConfig.provider_messages`, `RunResult.native_messages`, `ProviderLoopEntry.messages`도 분리된 이력 복사본을 반환합니다. 수정 가능한 입력 이력을 갱신하려면 반환된 리스트를 수정하고 `config.provider_messages`에 다시 대입합니다. 입력을 교체한 뒤에도 보관한 메시지와 파트는 유효하며, 결과와 루프 이력 속성은 읽기 전용입니다.

사용자 정의 노드에서는 `provider_messages_write(messages_or_outcome)`으로 채널 쓰기에 네이티브 이력을 보존합니다. `portable_message(ChatMessage)`는 이식 가능한 내용을 가져오며 네이티브 추론 권한의 가져오기를 거부합니다. `project_message(ProviderMessage)`는 관찰 전용 그래프 투영을 만듭니다.

네이티브 영속성을 위해 `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)`는 아카이브 또는 `ProviderError`를 반환합니다. `save(messages, binding="")`는 참조 또는 오류, `load(reference, binding="")`는 타입 기반 메시지 또는 오류를 반환합니다. 별도의 호스트 키로 SDK의 인증된 로컬 보관을 사용합니다. 이식 가능한 JSON에 재생 권한이나 관리 예산 권한을 부여하지는 않습니다.

`RuntimeHistoryRecord.serialize_canonical()`은 이식 가능한 기록에 계속 사용할 수 있습니다. 실제 네이티브 메시지를 포함한 기록을 영속화할 때는 `owner_scope`가 `owner_id`와 일치하는 실제 `NativeArchive`로 `record.serialize_canonical(archive, owner_id)`를 호출합니다. 복원에는 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`를 사용합니다. 이식 가능한 기록에는 기본 인자로 충분하지만 네이티브 아카이브 참조에는 동일한 소유자 범위의 아카이브가 필요합니다. 인자 없는 직렬화나 가져온 JSON은 네이티브 권한을 재생성하지 못합니다. Python의 parse와 아카이브를 사용하는 직렬화 호출은 아카이브 I/O를 포함한 네이티브 작업 중 GIL을 해제합니다.

`RuntimeHistoryRecord.message`는 실제 공유 네이티브 소유권을 유지하는 분리된 타입 기반 복사본을 반환합니다. 이 복사본을 바꿔도 불변 RAW 기록의 식별자는 바뀌지 않습니다. `ContextStore.hydrate_records(range)`는 타입 기반 기록 목록을 반환하고, `history_record_by_message_id(feed, message_id)`는 기록 또는 `None`을 반환합니다. `InMemoryContextStore`와 `SQLiteContextStore`는 이 메서드를 상속합니다. `SQLiteContextStore(database_path, archive=None)`에는 네이티브 보관용 실제 아카이브를 전달할 수 있습니다. 보관 권한이 필요한 네이티브 이력은 아카이브 없는 저장소에서 거부됩니다. 생성과 타입 기반 조회는 GIL을 해제합니다. `LocalProgramHost`는 마지막 선택적 키워드 `native_history_archive=None`을 받아 실제 Program 런타임으로 전달합니다. 이는 보관 권한을 제공할 뿐, 추가 권한이나 영속 Program 저장소 백엔드를 제공하지 않습니다.

이식 가능한 상태 딕셔너리와 JSON 내보내기는 메시지 데이터를 기술합니다. 네이티브 재생 상태, 제공자 출처, 관리 예산 권한을 재생성할 수 없습니다. 내보낸 JSON의 `native` 플래그는 설명 값이며 권한 토큰이 아닙니다. `ChatMessage`, 평면 텍스트 채널, JSON으로 변환하면 제공자별 파트가 누락될 수 있습니다. 그 파트가 필요하면 원래 접두 이력을 포함한 전체 타입 기반 이력을 보존합니다.

## Core 그래프 빠른 시작

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

### 컴파일 시 도구 소유권

Python `Tool`, 네이티브 C++ 도구, `MCPClient.get_tools()` 결과를
`ng.NodeContext(tools=[...])`에 함께 전달합니다. 컴파일 전에 소유된
`ToolSet`으로 스냅샷되어 엔진이 `run()` 및 `resume()` 동안 보유합니다.
나중에 컨텍스트 `tools`를 재할당해도 기존 엔진은 바뀌지 않으며 다음 컴파일에만
적용됩니다. MCP 도구의 네이티브 비동기 경로도 유지됩니다.

## Core API 패리티

Python은 별도의 Python 스케줄러가 아닌 C++ 실행 기능을 노출합니다:

- 동기 및 asyncio run/stream/resume;
- 정확한 체크포인트 `resume_from`, 포크, 상태 검사 및 순서화된 상태 쓰기;
- 그래프 인터럽트 및 `NodeInterrupt`를 통한 정적 및 동적 HITL;
- `RunMetadata` 데드라인, 추적/실행 ID 및 모델 토큰 상한;
- 그래프 전체 및 노드별 `RetryPolicy`, 지터 포함;
- 실행 로컬 또는 명시적으로 재사용 가능한 `CacheScope`;
- 체크포인트 및 장기 Store 백엔드;
- 사용자 정의 노드, 리듀서, 조건, 공급자 및 도구;
- 도구 게이트, 실행 정책, 필수 수명주기 Hook 및 엄격한 런타임 개입.

### 패리티 계약

여기서 “패리티”는 Python이 동일한 네이티브 실행 경로와 안전 계약을 사용한다는 뜻입니다. 모든 내부 C++ 저장소나 권한 타입을 Python에 그대로 복제한다는 뜻은 아닙니다.

| 기능 | 네이티브 C++ 경로 | Python 표면 | 상태 |
|---|---|---|---|
| Core 그래프 컴파일 및 실행 | `GraphEngine` | `GraphEngine.compile`, run/stream/async 메서드 | 동일한 스케줄러와 런타임 |
| 런타임 ID, 데드라인 및 예산 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 실행별 동일한 값 |
| 재시도 및 노드 캐시 정책 | `RetryPolicy`, `CacheScope` | 그래프/노드 setter 및 캐시 범위 | 동일한 런타임 정책 |
| 체크포인트, HITL 및 시간 이동 | 체크포인트 Store 및 재개 API | 재개, 정확한 `resume_from`, 포크, 상태 이력/갱신 | 동일한 체크포인트 계약 |
| Program 작성 및 로컬 실행 | 컴파일러, Catalog 및 `ProgramRuntime` | `ProgramCompiler`, `LocalProgramHost`, 핸들/결과 | 네이티브 소유자 범위 편의 호스트 |
| 필수 수명주기 Hook | 레지스트리, 실행기 및 `HookRuntime` | 정의와 `create_hook_runtime` 콜백 | 동일한 실패 시 차단 수명주기 경계 |
| 런타임 컨텍스트 및 엄격한 디스패치 | 컨텍스트 Store, 영수증 및 개입 | 대응하는 불변 값, Store 및 `StrictRuntimeProfile` | 동일한 네이티브 컨트롤러 |
| 영속성 휠 기본값 | SQLite Core/컨텍스트/디스패치 Store | `_HAVE_SQLITE` 내보내기 | PyPI 휠에서 활성화 |

원시 `ProgramCatalog`, 전환 Store, 교체/마이그레이션 컨트롤러, 합성 게이트웨이, Hook 저널 및 RPC 실행기는 호스트 조합 API로 남습니다. 권한을 가진 이 경로들의 일부만 노출하면 필수 `proposal -> compile -> admit -> publish -> migrate/spawn` 프로토콜을 우회하게 됩니다. 향후 Python 호스트 컨트롤러는 이 프로토콜과 비갱신 계보 예산을 하나의 소유자 범위 단위로 묶어야 합니다. `_HAVE_PROGRAM`은 원시 제어 평면 관리 패리티를 주장하지 않습니다.

### 런타임 재시도 재정의

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

그래프 정의의 `"retry_policy"`는 선언적 기본값으로 유지됩니다. 런타임 세터는 별도의 C++/Python 구성 표면입니다.

### 메타데이터 및 정확한 재개

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Python 노드 내부에서는 동일한 값들이 `input.ctx.trace_id`, `run_id`, `has_deadline`, `deadline_remaining_ms`, `model_token_budget`를 통해 사용할 수 있습니다.

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)`는 기본적으로 마감 시간 없이 시작합니다. 생성자의 제한 시간과 `metadata.set_timeout_ms(timeout)`은 steady clock의 남은 범위 안에서 음이 아닌 정수 밀리초를 받으며, 부호 있는 duration 변환이나 마감 시간 덧셈 전에 범위를 검증합니다. 음수 또는 너무 큰 정수는 `OverflowError`나 `ValueError`를 발생시키고, 실패한 setter는 이전 마감 시간을 유지합니다. 0은 즉시 만료되는 마감 시간입니다. 제거하려면 `metadata.clear_deadline()`을 호출합니다.

### 캐시 범위

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable`는 노드가 테넌트, 제공자, Store, 도구, 자격 증명, 시간 및 재개 상태와 무관하다는 명시적 주장입니다.

## Program 및 QuickJS

Python 휠은 `neograph::program`과 제한된 QuickJS 프론트엔드를 빌드합니다. Python으로 정의된 노드는 변경 불가능한 Program 레지스트리에 참여하고 네이티브 `ProgramRuntime`을 통해 실행할 수 있습니다.

```python
import neograph_engine as ng

@ng.node("my_node")
def my_node(state):
    return [ng.ChannelWrite("value", state.get("value", 0) + 1)]

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
print(result.status, result.output)
```

`LocalProgramHost`는 소유자 범위의 인메모리 편의 호스트입니다. 여전히 C++ 컴파일러, Catalog, 승인(admission) 정책, 전환 저장소 및 ProgramRuntime을 사용합니다. 생성된 제안은 승인(admission) 전에 호스트 의미 검증을 추가로 통과해야 합니다. [DSL 기능 평가](DSL_CAPABILITY_EVAL.md)를 참조하십시오.

`LocalProgramHost`를 소멸할 때 실제 `ProgramRuntime`이 스케줄러 작업을 취소하고 마무리하며 join하는 동안 호출자의 GIL을 해제하므로 실행 중인 Python 노드가 끝날 수 있습니다. 나머지 호스트 멤버를 소멸하기 전에 GIL을 다시 획득하며, Python 콜백/객체 소유자의 GIL 안전 소멸도 유지합니다.

네이티브 Program의 기록 실행 API는 `start_recorded`를 대체한 `ProgramRuntime::replay_recorded`입니다. `LocalProgramHost`는 `run`, `start`, `resume`을 노출하며 원시 기록 바인딩 제어 평면은 노출하지 않습니다. Program 결과는 기존 핸들, 예산, 권한 필드와 함께 네이티브 런타임이 제공한 타입 기반 제공자 결과 및 실패 증거를 보존합니다.

`ProgramResult.failure`는 딕셔너리가 아닌 읽기 전용 `ProgramFailure` 값 또는 `None`입니다. `provider_outcome`과 `provider_cause`는 제공자 실패 증거를 보존하며, `code`, `message`, `operation_id`, `core_node`, `attempts`, `witness`는 실패한 작업을 설명합니다. 결과 필드에는 `bundle_id`, `operation_id`, `attempt`, `checkpoint`, `interrupt`, `provider_budget_authority`도 포함됩니다.

정확히 설치된 JavaScript 어휘는 dict로 사용할 수 있습니다:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## 필수 수명 주기 Hooks

Hook은 모델이 도구를 호출하기로 결정하는 것이 아니라 호스트 수명 주기 이벤트에 의해 트리거됩니다.

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

`FailClosed` 하의 콜백 실패는 보호된 런타임 경계를 차단합니다. `Continue`는 관찰 손실이 허용 가능한 경우에만 사용할 수 있습니다.

## 런타임 컨텍스트, Skill 및 엄격한 디스패치

바인딩은 변경 불가능한 RAW 기록, 컨텍스트 아티팩트, 에포크, 필수 Skill/제약 조건, 변환 영수증 및 제공자 디스패치 영수증을 노출합니다.

RAW `RuntimeHistoryRecord`를 만들 때 Assistant 메시지에는 `RuntimeHistoryRecordData.trust`를 `RuntimeTrustClass.ModelOutput`으로 설정합니다. 기본값 `UntrustedInput`은 User 메시지만 허용하므로 Assistant 출력에 사용하면 거부됩니다. 이 레이블은 메시지 출처를 나타냅니다. 도구 실행, 예산, 코드 권한을 부여하지 않으며 실제 네이티브 재생 보관 권한을 대신하지 않습니다.

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt`는 임의의 파생 증거를 허용하지만 모든 필수 아티팩트가 바이트 단위로 동일하게 유지되도록 요구합니다.

전체 엄격 경로에는 지속형 SQLite 저장소를 사용하십시오:

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
outcome = profile.invoke(request)
profile.attach(engine)
```

## HITL 및 상태

정적 `interrupt_before`/`interrupt_after`, 동적 `NodeInterrupt`, 동기식 `resume`, asyncio `resume_async`, 그리고 정확한 `resume_from`은 체크포인트 저장소를 필요로 합니다.

Python `CheckpointStore` 하위 클래스는 `requires_managed_budget(thread_id) -> bool`을 구현할 수 있습니다. 이 동기 가상 메서드는 영속화된 관리 예산 은행의 거부 의무를 읽습니다. 엔진의 네이티브 비동기 파사드가 이 메서드를 호출하며, 바인딩은 Python 재정의 메서드를 실행할 때 GIL을 획득합니다. 체크포인트 상태에서 은행 정보를 제거하거나 체크포인트를 삭제해도 실제 영속 의무를 보고해야 합니다. 재정의 메서드를 생략하면 `False`를 반환하는 대신 지원되지 않는 백엔드라는 명시적인 네이티브 오류가 유지됩니다.

이 조회는 지출, 복원, 리스 권한을 부여하지 않습니다. 한도가 적용되는 관리 실행에는 지원되는 실제 네이티브 관리 예산 리스가 여전히 필요하며, 이 조회 메서드만 구현해서는 리스를 제공할 수 없습니다.

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

검사 및 시간 이동(time-travel)에는 `get_state_history`, `update_state`, `fork`를 사용하십시오. `get_state_view()`는 평면적인 Pydantic 기반 채널 접근을 제공하는 반면, `get_state()`는 표준 중첩 표현을 유지합니다.

## 비동기 및 취소

`run_async`, `run_stream_async`, `resume_async`는 `asyncio.Future` 객체를 반환합니다. 이 그래프 Future를 취소하면 `CancelToken`을 통해 취소를 요청합니다. 작업이 끝나려면 네이티브 I/O가 취소 경계에 도달해야 합니다. 그래프 스트리밍 콜백은 호출자의 asyncio 루프 스레드로 돌아옵니다.

제공자의 `invoke`와 `dispatch`는 동기 Python 메서드입니다. 바인딩은 네이티브 호출 동안 GIL을 해제하고 Python 제공자 재정의 메서드나 이벤트 콜백을 실행할 때 다시 획득합니다. 이벤트 콜백은 소유된 `ProviderEvent` 값을 받으며 네이티브 작업자 스레드에서 실행될 수 있습니다. asyncio가 소유한 상태를 갱신해야 한다면 `loop.call_soon_threadsafe`를 사용합니다.

관찰자는 `request.on_event = callback`으로 지정하거나 `None`으로 해제합니다. `request.on_event`를 읽으면 원래 Python 콜백 객체의 동일성이 유지되며 `RunConfig.on_provider_event`도 같은 규칙을 따릅니다. 각 이벤트는 문자열 `kind`와 타입 기반 `value`를 노출하며 `ProviderPartDelta` 값은 자체 `bytes`를 소유합니다. 콜백 이후 이벤트를 보관해도 빌린 SDK 텍스트 뷰를 보관하는 것이 아닙니다. 취소를 요청하려면 `request.cancel_token`에 지정한 토큰의 `token.cancel()`을 호출합니다.

동기 제공자 호출을 이벤트 루프 스레드 밖에서 실행하려면 `asyncio.to_thread(provider.invoke, request)`를 사용합니다. 이 await를 취소하는 것만으로는 제공자 호출이 취소되지 않습니다. `request.cancel_token`에 `CancelToken`을 지정하고 명시적으로 취소를 요청해야 합니다.

유효한 준비 핸들의 토큰이 이미 취소된 경우 제공자 디스패치의 사전 검사는 소유된 SDK `Cancelled` 실패를 반환합니다. 동기 디스패치도 동일한 네이티브 비동기 제공자 경로가 끝날 때까지 실행합니다. 그래프, 호스트, 코루틴 진입 시점의 취소는 별개의 경계이며, `ProviderOutcome`이 반환되기 전에 예외를 발생시킬 수 있습니다. 취소를 요청해도 모든 경계에서 타입 기반 `Cancelled` 데이터가 보장되지는 않습니다. 취소 요청은 요청이 전송되지 않았거나 원격 작업이 중단되었거나 요금이 발생하지 않을 것이라는 증거도 아닙니다.

[pybind11 GIL 문서](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil)는 GIL 해제와 Python 콜백을 위한 재획득이 별개의 바인딩 책임인 이유를 설명합니다.

## 프로토콜 및 관찰 가능성

- MCP 클라이언트 도구는 빌드 시 `neograph_engine.mcp`를 통해 사용할 수 있습니다.
- A2A 클라이언트 유형은 빌드 시 `neograph_engine.a2a`를 통해 사용할 수 있습니다.
- `ProtocolHostAdapter`는 공식 Python A2A/ACP 서버 SDK를 NeoGraph 세션 의미론과 통합합니다.
- `neograph_engine.tracing` 및 `neograph_engine.openinference`는 Phoenix, Langfuse, Arize 및 호환 백엔드를 위한 공급업체 중립적 OTel/OpenInference 데이터를 내보냅니다.

### 네이티브 A2A 탐색과 스트리밍

`a2a.WireDialect`는 `V0_3/V1_0`입니다. `client.wire_dialect()`는 생성/강제 card fetch 후 첫 card-selected RPC 또는 성공 probe까지 `None`입니다. `AgentCard.supported_interfaces`는 `url/protocol_binding/protocol_version/tenant`를 가진 분리된 `AgentInterface` 목록이며 `card.raw`는 관찰 JSON입니다. 호환 JSONRPC 중 정규화 base URL을 우선하며 card URL은 RPC endpoint를 바꾸지 않습니다. card가 없을 때 숫자 RPC 오류 `-32601`만 probe를 허용하고 card-selected 호출은 fallback하지 않습니다. `a2a.A2ARpcError.code`는 실제 정수 코드입니다.

```python
from neograph_engine import a2a
from uuid import uuid4

client = a2a.A2AClient("http://127.0.0.1:8080")
card = client.fetch_agent_card()
for interface in card.supported_interfaces:
    print(interface.protocol_version, interface.tenant)

params = a2a.MessageSendParams()
params.message.message_id = str(uuid4())
params.message.role = "user"
params.message.parts = [
    a2a.Part.text_part("Explain this item."),
    a2a.Part.text_part("Keep the answer short."),
]
configuration = a2a.MessageSendConfiguration()
configuration.blocking = True
configuration.accepted_output_modes = ["text/plain"]
params.configuration = configuration
events = []

def on_event(event):
    events.append(event)  # Owned snapshot remains valid after the callback.
    return True

task = client.send_message_stream(params, on_event)
print(client.wire_dialect(), task.id, task.state)
```

이 endpoint에 실제 로컬 A2A server를 먼저 시작합니다. Chat 루프백과 별개입니다. `send_message(params)`는 비스트리밍 multipart overload이고 text overload도 유지됩니다. `client.set_authorization_header(authorization_header)`로 인증을 명시하고 로그에 남기지 않습니다. `Part.media_type/file/data/metadata`, message extensions/reference IDs, task artifacts는 타입/데이터 관찰이며 provider native 권한이 아닙니다.

`params.message`, `Task.status`는 live inline 레코드이고 선택적 configuration/벡터/event 자식은 분리된 snapshot입니다. 입력 변경은 다시 대입합니다. `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task`와 `status_update/artifact_update/task/is_final()`을 확인합니다. V1 opening task는 final이 아니며 native SSE가 status와 append/replace artifact를 조립합니다. blocking 호출은 GIL을 해제하고 callback/소유자 해제는 GIL을 획득합니다. 출력 관찰 후 재디스패치하지 않습니다. callback thread는 asyncio loop가 보장되지 않으므로 필요하면 `loop.call_soon_threadsafe`를 사용합니다.


`neograph_engine.openinference`에서 `OpenInferenceProvider`를 가져옵니다. 생성자는 `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")`입니다. 이 Python 클래스는 타입 기반 `prepare(request)`, `dispatch(prepared)`, `invoke(request)`를 상속하는 네이티브 C++ 래퍼에 위임합니다. 기본 span 이름은 라벨이며 `complete` 메서드를 복원하지 않습니다. `opentelemetry-api`와 `opentelemetry-sdk`를 설치합니다. 생성에는 OTel API가 필요하며, 내보내려면 SDK span processor/exporter를 설정해야 합니다.

준비, 승인 실패, 준비된 요청의 폐기는 LLM span을 만들지 않습니다. 승인된 디스패치는 하나의 span을 시작하고 완료, SDK 실패 또는 디스패치 예외 시 끝냅니다. 준비된 작업은 래퍼와 외부 tracer 참조가 수거된 뒤에도 Python tracer 어댑터를 유지합니다. 블로킹 `invoke`/`dispatch`는 GIL을 해제하며 OTel 호출과 Python 참조 소멸 시 GIL을 획득합니다. Tracer 실패에는 네이티브 최선 노력 정책을 적용하며 소유된 결과, 원래 이벤트나 제품 예외를 바꾸지 않습니다.

어댑터는 디스패치 시 Python tracer의 `start_span`을 호출합니다. 호출자의 컨텍스트가 네이티브 작업자 스레드로 자동 전달되지는 않습니다. 디스패치 전에 의도한 OTel 부모 컨텍스트를 캡처하고 아래처럼 tracer 어댑터를 통해 명시적으로 전달합니다. 그래프/노드 부모가 필요한 그래프 노드 제공자에도 같은 방법을 적용합니다. OTel의 [명시적 부모 컨텍스트 선택 문서](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span)를 참고하세요.

예제의 `ParentContextTracer`는 캡처한 부모 컨텍스트 하나를 보관합니다. 다른 논리적 부모를 사용할 때는 컨텍스트를 새로 캡처하고 어댑터/제공자 래퍼를 새로 생성합니다. 기존 어댑터를 재사용하면 이전 부모가 유지됩니다.

위 제공자 예제의 `provider`와 `controls`를 사용합니다. 다음 조각을 실행하기 전에 단일 요청 피어를 다시 시작합니다:

```python
import neograph_engine as ng
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import ConsoleSpanExporter, SimpleSpanProcessor
from neograph_engine.openinference import OpenInferenceProvider

class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer = tracer
        self.parent_context = parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)

traces = TracerProvider()
traces.add_span_processor(SimpleSpanProcessor(ConsoleSpanExporter()))
tracer = traces.get_tracer("python-guide")
with tracer.start_as_current_span("request"):
    observed = OpenInferenceProvider(
        provider, ParentContextTracer(tracer, otel_context.get_current()),
        span_name="llm.request",
    )
    request = ng.make_provider_request(
        observed, "example-model",
        [ng.ProviderMessage(role=ng.ProviderRole.User,
                            parts=[ng.Text("Say hello.")])],
        controls=controls,
    )
    prepared = observed.prepare(request)
    outcome = observed.dispatch(prepared)
    print("consumed:", prepared.consumed)
    print("failure:", outcome.failure is not None)
    print(outcome.text)
traces.shutdown()
```

콘솔 exporter에는 `openinference.span.kind="LLM"`인 `llm.request`가 표시되어야 하며, 그 `parent_id`는 `request` span의 `span_id`와 같아야 합니다. 연결된 피어를 사용하면 출력 결과는 소비되었고 실패가 없으며 `Hello.`를 포함합니다. 새 요청에서는 `observed.invoke(request)`가 같은 준비와 디스패치를 결합합니다. 소비된 핸들은 다시 디스패치하지 않습니다.

LLM span 필드에는 승인된 모델, 선언된 temperature/출력 상한, 공개 메시지 역할과 표시 가능한 `Text` 내용, 알려진 입력/출력/전체 사용량 카운터가 포함됩니다. 누락된 카운터는 해당 속성을 생략하며 보고된 0은 0으로 유지됩니다. 스트리밍 요청은 표시 가능한 텍스트 내용 델타에만 `llm.token` 이벤트를 추가하며 텍스트는 `attributes["chunk"]`에 넣습니다. SDK 실패는 안전한 메시지로 ERROR 상태를 설정하고 부분 공개 출력을 유지하며, 성공한 완료는 OK를 설정합니다. `request.on_event`는 계속 원래 타입 기반 이벤트를 받습니다.

네이티브 재생, 추론 part, 원시 엔벌로프와 원시 이벤트는 이 LLM span 필드에 들어가지 않습니다. 결과는 네이티브 메시지와 증거를 계속 유지합니다. 추적 내보내기는 재생 custody나 회계 권한을 부여하지 않습니다. 공개 텍스트에도 민감한 애플리케이션 데이터가 포함될 수 있으므로 프롬프트와 exporter를 그에 맞게 선택합니다. `openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)`는 별도의 그래프 이벤트 컨텍스트 관리자로 유지되며 제공자 LLM span을 대신하지 않습니다.

## 선택적 구성 요소

공개 패키지는 선택적 C++ 구성 요소를 정직하게 표시합니다:

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- 누락된 구성 요소는 Python에서 에뮬레이션되지 않고 존재하지 않습니다;
- PyPI 휠은 Program/QuickJS, LLM, MCP 및 SQLite를 활성화합니다; 소스 빌드는 CMake 옵션을 따릅니다.

## 테스트 및 예제

바인딩 스위트는 Core 실행, 사용자 정의 콜백, asyncio, 취소, Program 컴파일/런타임, 필수 Hook, 엄격한 컨텍스트, SQLite 지속성, 프로토콜 및 README 예제를 다룹니다.

- [Python 예제](../bindings/python/examples/README.md)
- [C++ 예제](../examples/README.md)
- [QuickJS 작성 경계](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [엄격한 런타임 인터포지션](STRICT_RUNTIME_INTERPOSITION.md)
