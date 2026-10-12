<!-- neograph-i18n: source=docs/python-binding.md locale=ko source_sha256=269e9e433933e198c9e5317ed92c0eefcfd2c7f8da1bf1ace61f774d09e63d55 -->
# Python 바인딩

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`는 동일한 C++ 런타임의 pybind11 표면입니다. 휠은 Core, LLM, Program/QuickJS, MCP 및 SQLite 런타임 지속성을 활성화합니다. 선택적 소스 빌드는 컴파일하는 구성 요소만 노출합니다.

이 가이드는 SchemaProvider SDK `0.3.0`, 인터페이스 리비전 `6`, 공유 라이브러리 ABI 리비전 `6`으로 빌드한 휠이 필요합니다. 번들 구성 요소를 함께 설치합니다. `complete`를 노출하는 이전 휠은 다른 제공자 인터페이스를 사용하므로 아래 타입 기반 예제를 실행할 수 없습니다.

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

`ProviderControls.provider`와 `response_format`, `SchemaProviderDefaults.provider`, `OpenRouterRouting.max_price`, `ProviderToolResult.host`는 분리된 선택적 레코드를 반환합니다. 값이 있으면 레코드를 읽고 수정한 뒤 속성에 다시 대입합니다. `controls.provider.order`만 바꾸면 controls에는 반영되지 않습니다. 레코드를 비우려면 `None`을 대입합니다.

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

### 계열별 제어

패키지 버전, native archive v3, portable JSON v2는 별개입니다. 아래 예제는 제어 구성만 하며 해당 계열의 승인된 제공자가 필요합니다. 첫 루프백은 그대로 둡니다. OpenRouter 전용 제어는 미승인 로컬 origin에서 I/O 전에 거부됩니다.

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]
routing = ng.OpenRouterRouting()
price = ng.OpenRouterMaxPrice()
price.prompt_nano_usd = 75_000_000       # 0.075 USD per million prompt tokens
price.completion_nano_usd = 300_000_000  # 0.30 USD per million completion tokens
routing.max_price = price
chat.provider = routing

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

기본 생성 후 필드를 대입합니다. 키워드 생성자는 없습니다. 선택적 레코드/벡터는 분리된 복사본이므로 수정 후 다시 대입합니다. `OpenRouterRouting.max_price`도 그렇습니다. `routing.max_price.prompt_nano_usd = ...`는 복사본을 고치므로 `ng.OpenRouterMaxPrice`를 만들어 객체 전체를 대입합니다. 금액은 정수 nanoUSD로, 백만 토큰당 `prompt_nano_usd`와 `completion_nano_usd`, 그리고 `image_nano_usd`, `audio_nano_usd`, `request_nano_usd`가 있으며 정확한 십진 USD로 전송됩니다(`75_000_000`은 `0.075`). 하나 이상 필요하며 2**53을 넘을 수 없습니다. `SchemaProviderDefaults.provider`는 `max_price`를 다른 routing 필드와 같은 방식으로 병합합니다. 요청 자체의 값이 기본값을 대체합니다. Chat reasoning의 `effort/max_tokens/exclude/enabled`, `include_reasoning`, `usage_include`, 대체 `models`는 선언된 OpenRouter origin이 필요합니다. 기존 `reasoning_effort/service_tier/provider/response_format`도 유지됩니다.

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

`outcome.usage.provider_cost`는 불변 `ProviderReportedCost` 뷰입니다. 선택적 `ProviderUsdAmount` 필드 `total/upstream_total/upstream_input/upstream_output`에는 `nano_usd/evidence/rounding`이 있고 `status`는 이 순서의 4개 요소이며 `byok_status`는 nullable `is_byok`의 상태입니다. `ProviderCostStatus/ProviderCostSource/ProviderCostRounding`은 부재, 가용, 형식 오류, 정밀도 초과, overflow, 알 수 없는 통화, 충돌 근거를 보존합니다. 금액은 원래 decimal 문자열이 아닌 파싱된 binary64에서 올림됩니다. 보고된 0과 `is_byok=False`는 부재가 아닙니다. 이는 청구서나 token 예산 차감이 아니므로 호출별 금액 근거는 집계 token bank가 아니라 순서가 보존된 `provider_outcomes`에서 확인합니다.

`UsageAccumulator`는 token만 집계합니다. 단일 보고에서는 금액 메타데이터를 보존하지만 두 번째 보고부터 snapshot의 `provider_cost`는 canonical Missing/None으로 명시적으로 초기화됩니다. 오래된 첫 호출 비용이나 금액 합계를 표시하지 않습니다. 이 snapshot을 복원해도 비용을 다시 만들거나 token 권한을 갱신하지 않습니다. 정확한 금액 근거는 원래 각 outcome 또는 provider journal 항목에서 읽습니다.

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
checkpoint_id = result.checkpoint_id

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
    # GraphState.get takes only the channel name; a missing channel is None.
    return [ng.ChannelWrite("value", (state.get("value") or 0) + 1)]

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
print(result.status, result.output["value"])  # ProgramTerminalStatus.Completed 1
```

`ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer`, `add_registered_condition`은 이 프로세스에 이미 등록된 실행 단위를 복사합니다. 등록은 `@ng.node` 또는 `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer`, `ng.ConditionRegistry.register_condition`으로 합니다. 등록되지 않은 이름을 넘기면 세 메서드 모두 `ng.ProgramRegistryError`를 발생시키고, 메시지는 필요한 등록 호출을 알려 줍니다. 이 오류의 `kind`는 `"node"`, `"reducer"`, `"condition"` 중 하나이고 `name`은 누락된 이름입니다. `ValueError`와 `RuntimeError`를 모두 상속하므로 둘 중 어느 쪽을 잡던 기존 처리기도 그대로 이 오류를 잡습니다. 레지스트리 스냅샷에 없는 노드 타입, 리듀서 또는 조건을 쓰는 Program은 대신 컴파일 시점에 실패합니다. `ProgramCompiler.compile`과 `LocalProgramHost.compile_admit` 모두 진단 `P_CORE_GC_REGISTRY_MISS`가 담긴 `ng.ProgramCompileError`를 발생시킵니다.

각 진단 dict에는 `help` 항목이 있습니다. 컴파일이 반환한 진단에서는 `doc`([진단 가이드](diagnostics.md)의 해당 항목), `related`, `suggestions`, `candidates`가 담긴 dict이고, 봉인된 `bundle.diagnostics`에서는 `None`입니다. 예외에는 항상 들어 있습니다. `ProgramCompiler(..., echo_routes=True)`는 라우팅된 후속 노드 표를 담은 `P_CORE_ROUTES` 노트도 반환합니다. `ProgramCompiler`나 `LocalProgramHost`에 `soundness_check=True`를 주면 [건전성 검사](program-graphs.md#soundness-check)가 실행되어 witness trace가 있는 `P_CORE_S1`~`P_CORE_S5` 경고와 `P_CORE_SOUNDNESS` 요약이 반환됩니다. 이들 중 어느 것도 봉인되지 않습니다.

`LocalProgramHost`는 소유자 범위의 인메모리 편의 호스트입니다. 여전히 C++ 컴파일러, Catalog, 승인(admission) 정책, 전환 저장소 및 ProgramRuntime을 사용합니다. 생성된 제안은 승인(admission) 전에 호스트 의미 검증을 추가로 통과해야 합니다. [DSL 기능 평가](DSL_CAPABILITY_EVAL.md)를 참조하십시오.

`LocalProgramHost`를 소멸할 때 실제 `ProgramRuntime`이 스케줄러 작업을 취소하고 마무리하며 join하는 동안 호출자의 GIL을 해제하므로 실행 중인 Python 노드가 끝날 수 있습니다. 나머지 호스트 멤버를 소멸하기 전에 GIL을 다시 획득하며, Python 콜백/객체 소유자의 GIL 안전 소멸도 유지합니다.

네이티브 Program의 기록 실행 API는 `start_recorded`를 대체한 `ProgramRuntime::replay_recorded`입니다. `LocalProgramHost`는 `run`, `start`, `resume`, `fork`를 노출하며 원시 기록 바인딩 제어 평면은 노출하지 않습니다. Program 결과는 기존 핸들, 예산, 권한 필드와 함께 네이티브 런타임이 제공한 타입 기반 제공자 결과 및 실패 증거를 보존합니다.

`ProgramResult.failure`는 딕셔너리가 아닌 읽기 전용 `ProgramFailure` 값 또는 `None`입니다. `provider_outcome`과 `provider_cause`는 제공자 실패 증거를 보존하며, `code`, `message`, `operation_id`, `core_node`, `attempts`, `witness`는 실패한 작업을 설명합니다. 결과 필드에는 `bundle_id`, `operation_id`, `attempt`, `checkpoint`, `interrupt`, `provider_budget_authority`도 포함됩니다.

정확히 설치된 JavaScript 어휘는 dict로 사용할 수 있습니다:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### 모듈 구조와 `callCore`

JavaScript Program은 그래프 빌더 하나를 반환하는 `define()`을 반드시 export해야 합니다. 그 그래프가 Program의 유일한 Core입니다. `define()`이 없으면 컴파일이 `P_JS_DEFINE_MISSING`과 함께 `ProgramCompileError`를 발생시키므로, `main()`만 export하는 모듈은 거부됩니다. `main()`은 선택 사항이며, 없으면 실행 시 Core를 직접 실행합니다.

`ng.callCore(name, input, sourceSite)`는 `ng.graph(...)`에 전달한 이름(위 예제에서는 `"main"`)으로 그 Core를 지정해야 합니다. 다른 이름을 쓰면 실행이 `P_JS_CONTROL_COMMAND`로 실패합니다. 서로 다른 단계를 실행하려면 그 단계들을 해당 그래프에 넣고 다른 입력으로 다시 호출하십시오. 모든 노드 타입은 `add_registered_node`에 전달하기 전에 `@ng.node` 또는 `ng.NodeFactory.register_type`으로 등록하십시오. 이 메서드는 등록되지 않은 타입에 대해 `ng.ProgramRegistryError`를 발생시킵니다.

완료된 `callCore`는 Core의 출력 객체를 `main()`에 돌려줍니다. 위 빠른 시작 예제는 이를 그대로 `result.output`으로 반환합니다:

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels`에는 각 채널의 `value`와 `version`이 들어 있습니다. 각 채널의 값은 같은 이름의 최상위 키가 이미 있지 않은 한 채널 이름의 최상위 키로도 복사되므로, `main()`에서 `result.value`를 읽을 수 있습니다. 이름이 `channels` 또는 `global_version`인 채널은 `channels` 아래에서만 접근할 수 있습니다. barrier 목록은 정렬되어 저장됩니다. `graph.barrier("join", ["zeta", "alpha"])`는 `bundle.sealed_core_definitions`에 `{"wait_for": ["alpha", "zeta"]}`로 나타나며, 순서는 아무 영향이 없습니다.

### 실패한 명령은 실행을 끝냅니다

완료된 명령만 `main()`을 재개합니다. yield된 명령이 완료되지 않으면, 예를 들어 노드가 예외를 던졌거나 Core 단계 예산을 소진했거나 더 이상 발화할 수 없는 barrier에서 멈췄거나 wall-time 제한을 넘긴 `callCore`라면, `ProgramRuntime`은 그 결과를 기록하고 해당 `status`와 `failure`를 가진 실행의 최종 결과로 만듭니다. 제너레이터는 재개되지 않습니다. `yield`를 감싼 `try`/`catch`는 실행되지 않으며, 이후의 JavaScript도 실행되지 않습니다. 실패 정책에 따라 실패한 join(`ng.all`, `ng.race`, `ng.quorum`)도 같은 방식으로 실행을 끝냅니다. Python에서는 `result.status`와 `result.failure`로 실패를 처리하십시오. 중단(interrupt)된 명령은 다릅니다. 실행은 `Interrupted`로 멈추고, `resume()`이 그 명령을 이어서 실행합니다. 중단된 join 멤버는 그 join을 일시 정지합니다. join은 더 이상 멤버를 시작하지 않고, 이미 실행 중인 멤버는 끝나게 둡니다. 그 결과가 join을 결정하면(fail-fast 실패, race 승자, 충족되었거나 도달할 수 없게 된 quorum) join은 그렇게 끝납니다. 그렇지 않으면 실행은 첫 번째로 일시 정지된 멤버에서 `Interrupted`로 멈추고, `resume()`은 그 멤버를 이어서 실행하며 끝난 멤버는 명령 저널에서 재생하므로 다시 실행하거나 다시 청구하지 않습니다. `ng.await(command, timeout_ms)`의 마감 시각은 절대 시각입니다. 명령 저널이 await가 시작된 시각을 기록하며, 재개나 멈춘 프로세스의 복구는 남은 시간만 갖습니다. 마감 뒤에 재개되면 await는 응답받은 노드를 실행하지 않고 즉시 `P_AWAIT_TIMEOUT`과 함께 실행을 `TimedOut`으로 끝냅니다(#391). 플랜 수준의 `await` 연산도 재개를 거쳐 같은 방식으로 마감 시각을 유지합니다. 불확실한 효과, 즉 payload `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`에서 일시 정지한 멤버는 Core 스텝에서처럼(#392) 멤버의 실패보다 우선합니다. join은 실패하는 대신 그 멤버에서 일시 정지하고 실패한 멤버를 기록하며, 재개되면 그 실패로 끝납니다. 성공이 결과를 정한 join은 그렇게 끝납니다(#397).

### 그래프 검사와 barrier 정체

컴파일은 스케줄러가 실제로 택하는 경로를 따라 Core 그래프를 검사합니다. 조건부 엣지가 있는 노드는 첫 번째 조건부 엣지로만 라우팅합니다. JavaScript Program은 엄격한 문서이므로 다음은 각각 `ng.ProgramCompileError`를 발생시킵니다:

- `P_JS_GRAPH_ARGUMENT`: 노드 이름을 `__start__` 또는 `__end__`로 지정하는 `.node()` 호출에서 발생;
- `P_JS_GRAPH_ENTRY`: `define()`이 노드는 있지만 `.entry()`가 없는 그래프를 반환할 때 발생;
- `P_CORE_E13`: 조건부 엣지도 가진 노드에서 나가는 정적 엣지에 대해 발생(그 엣지는 결코 발화하지 않음);
- `P_CORE_E14`: 같은 노드에서 나가는 두 번째 조건부 엣지에 대해 발생(그 엣지로는 결코 라우팅되지 않음).

`P_CORE_E15`는 `bundle.compile_diagnostics`에 담기는 경고입니다. barrier 멤버에 join이나 `__end__`가 아닌 다른 곳으로 이어지는 경로가 있고, 그 경로를 타면 join이 정체됩니다. 루프는 유효합니다. 모든 순환을 `P_CORE_E16`으로 거부하려면(예: 계획과 체크리스트) `ProgramCompiler` 또는 `LocalProgramHost`에 `require_acyclic=True`를 전달하십시오.

barrier는 `wait_for` 목록의 모든 노드가 그 barrier로 라우팅한 뒤에 발화합니다. 다른 방식으로 join에 도달하는 노드, 예를 들어 멤버의 실패 경로에 있는 보정(fix-up) 노드는 그 멤버를 대신하지 않습니다. 실행할 것이 더 남지 않았는데 join이 아직 신호를 쥐고 있으면 실행은 완료되지 않습니다. `P_CORE_BARRIER_STALLED`와 함께 `Failed`로 끝나고, `failure.core_node`는 그 join을 가리키며, `failure.witness["stalled_barriers"]`는 정체된 join마다 그 `wait_for`, `received`, `missing` 노드를 나열합니다. 이런 실행을 정상적으로 끝내는 것은 마지막 super-step에서 `__end__`로 향하는 명시적 경로뿐입니다. 즉 `__end__`로 가는 정적 엣지(`.exit()`가 만드는 엣지), `__end__`로 가는 조건부 경로, 또는 `Command.goto("__end__")`입니다. 후속 노드가 없는 노드로는 정상적으로 끝나지 않으며, 더 이른 super-step의 명시적 종료로도 마찬가지입니다. 같은 상태의 `GraphEngine` 실행은 `ng.RunStatus.BarrierStalled`를 반환하고, `result.output["_neograph"]["barrier_stalled"]`에 같은 세부 정보가 담깁니다.

### 오류와 진단

Python에서 발생하는 Program 오류는 타입이 지정되어 있고 진단을 보존합니다:

| 예외 | 발생 위치 | 상위 클래스 |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`, `LocalProgramHost.compile`, `compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit`, `compile_admit` (예: `P_ADMIT_POLICY` 또는 `P_ADMIT_PROVIDER`) | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`, `start`, `start_active`, `resume` (예: `P_START_BUDGET` 또는 `P_RUN_NOT_FOUND`) | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer`, `add_registered_condition` | `ValueError`, `RuntimeError` |

앞의 세 예외의 메시지는 `Program compilation failed`, `Program admission failed` 또는 `Program operation failed`로 시작하고, 그 뒤에 진단마다 한 줄이 이어집니다. 각 줄에는 진단의 코드, JSON 포인터, 메시지가 있고, 소스 위치가 있는 진단이면 `(source:line:col)`도 붙습니다. `.diagnostics`는 `code`, `severity`, `phase`, `message`, `source_id`, `json_pointer`, `span`, `witness`를 가진 dict의 리스트입니다.

컴파일에 성공한 번들은 두 목록을 가집니다. `bundle.compile_diagnostics`는 컴파일러가 반환한 그대로의 목록으로, JavaScript 호출 위치를 `span`에 담은 현재 규칙의 경고와 `P_JS_CONTROL_CHARACTER` 같은 작성 경고가 들어 있습니다. 작성자에게는 이 목록을 보여 주십시오. `bundle.diagnostics`는 번들에 봉인되어 `bundle.id`가 포괄하는 사본입니다. 이 사본은 번들이 처음 봉인될 때의 규칙과 문구를 유지하므로 새 컴파일러가 번들 id를 바꾸지 않으며, 그 때문에 내용이 다를 수 있습니다. span이 없고, Python 노드만 쓰는 채널에 대한 `P_CORE_E6`처럼 현재 규칙에서는 빠진 경고가 여전히 들어 있을 수 있습니다. 컴파일러도 `LocalProgramHost`도 stderr에 lint를 출력하지 않습니다.

<a id="model-calling-nodes"></a>
### 모델을 호출하는 노드

`LocalProgramHost`는 자신이 승인(admission)하는 Program의 내장 `llm_call` 노드에 제공자 하나를 바인딩합니다. 레지스트리는 제공자의 이름과 그것을 요구하는 노드를 선언하고, 호스트는 비밀이 아닌 `binding_label` 아래에서 제공자 객체, 모델, 요청 제어를 제공합니다.

```python
from neograph_engine.llm import SchemaProvider

registry = (
    ng.ProgramRegistryBuilder()
    .add_provider("openrouter", "1.0.0", "sha256:" + "3" * 64)
    .add_registered_node("llm_call", "1.0.0", "sha256:" + "4" * 64, provider="openrouter")
    .add_registered_reducer("append", "1.0.0", "sha256:" + "5" * 64)
    .build()
)
controls = ng.ProviderControls()
controls.max_output_tokens = 256
ceiling.model_tokens = 100_000
ceiling.monetary_microunits = 500_000
# descriptor and options as in "Descriptor and runtime policy" above
host = ng.LocalProgramHost(
    registry, "tenant-a", ceiling,
    node_context=ng.NodeContext(provider=SchemaProvider(descriptor, options),
                                model="deepseek/deepseek-v4-flash-0731",
                                provider_controls=controls),
    binding_label="openrouter-prod")

source = ng.ProgramSource.from_javascript("chat.js", r'''
export function define() {
  const graph = ng.graph("chat");
  graph.channel("messages", {reducer: "append", initial: []});
  graph.node("llm", {type: "llm_call"});
  graph.entry("llm");
  graph.exit("llm");
  return graph;
}
''')
run_budget.model_tokens = 100_000
run_budget.monetary_microunits = 500_000  # 0.50 USD
version = host.compile_admit(source, run_budget)
result = host.run(version, {"messages": [{"role": "user", "content": "Hi"}]}, run_budget)
print(result.usage.monetary_microunits, [r.evidence for r in result.monetary_records])
```

`add_provider(name, semantic_version, implementation_digest)`는 제공자의 identity를 등록하고, `add_registered_node("llm_call", ..., provider=name)`은 그 identity를 정적 요구 사항으로 가진 내장 노드를 등록합니다. `provider=`는 `llm_call`만 바인딩하며 다른 타입이나 알 수 없는 제공자에는 `ValueError`를 발생시킵니다. 호스트는 descriptor id와 상관없이 `node_context`의 제공자를 그 이름에 바인딩합니다. `NodeContext(provider_controls=...)`는 노드가 보내는 typed `ProviderControls`(예: `max_output_tokens`)를 받습니다. `node_context`에는 제공자와 `binding_label`이 필요하며 도구는 없어야 합니다.

바인딩 가운데 identity에 들어가는 것은 레이블뿐입니다. 제공자를 요구하는 Program 버전은 레이블과 제공자의 레지스트리 identity를 다이제스트한 capability receipt를 가집니다. 제공자의 키, endpoint, 모델은 여기에 들어가지 않으므로 모델을 바꿔도 버전 id는 그대로이고 레이블을 바꾸면 달라집니다. 제공자를 요구하지 않는 Program은 receipt가 없으며 번들 id와 버전 id를 그대로 유지합니다. 레이블은 `a-z`, `0-9`, `.`, `_`, `-`, `:`로 된 1~64자이고 영문자나 숫자로 시작해야 하며 API 키처럼 보이면 안 됩니다. `node_context` 없이 제공자를 요구하는 Program은 승인(admission)에서 `P_ADMIT_PROVIDER`로 거부되며, `provider=` 없이 등록한 `llm_call` 또는 `intent_classifier` 노드를 가진 Program도 마찬가지입니다.

각 호출은 NeoGraph에 컴파일된 가격 카탈로그로 실행의 `monetary_microunits`에 대해 가격이 매겨집니다. 요청을 보내기 전에 호출의 상한을 예약합니다. OpenRouter에서는 요청이 그 상한을 `provider.max_price`로도 싣습니다. route ceiling의 입력·출력 요율이거나, controls나 `SchemaProviderDefaults`에 이미 지정한 더 낮은 `max_price`이므로 OpenRouter는 호출을 더 비싼 endpoint로 라우팅할 수 없습니다. 가격을 매길 때 대화를 사용자 메시지 하나로 바꾼 요청의 probe를 준비하되 보내지는 않으므로, Python provider의 `prepare`에는 이 probe도 전달됩니다. 상한을 정할 수 없는 호출(카탈로그 항목이 없는 모델, route ceiling이 없는 router alias, 출력 cap이 없는 요청)이나 예약이 남은 예산에 들어가지 않는 호출은 아무것도 보내기 전에 거부되며, 실행은 `P_MONEY_*` 코드로 실패합니다. 모든 `ProgramRunBudget` 필드와 마찬가지로 `monetary_microunits = 0`은 아무것도 부여하지 않으므로 그런 실행은 모든 호출을 거부합니다. 응답 뒤에는 `result.monetary_records`에 호출마다 `ng.ProgramMonetaryRecord` 하나가 `evidence`와 함께 들어갑니다. `reported`(OpenRouter `usage.cost`), `estimated`(카탈로그 요율이며 `catalog_version`과 `catalog_digest`가 카탈로그를 가리킴), `held`(결과나 usage를 알 수 없어 예약액을 청구) 중 하나입니다. `result.usage.monetary_microunits`는 그 합입니다. 코드, 상한, 정산 규칙은 [가격 증거와 가격 카탈로그](reference-en.md#pricing-evidence-and-the-price-catalog)에 있습니다.

모델을 호출하는 Python 노드는 제공자를 직접 소유하며, Program 예산은 그런 호출을 보지 못합니다.

### 자식 Program

`ng.spawn`에는 자식 바인딩 리졸버가 필요한데, `LocalProgramHost`는 이를 구성하지 않습니다. 따라서 Python에서 호출한 모든 `ng.spawn`은 `P_CHILD_BINDING`("Program spawn has no configured child binding resolver")으로 실행을 실패시킵니다. 자식 Program에는 `RuntimeConfig::child_binding_resolver`를 설정하는 C++ 호스트가 필요합니다. 이 리졸버는 각 바인딩 이름을 `link_module_child`가 반환한 `ModuleLinkReceipt`와 승인(admission)된 자식 `ProgramVersion`에 대응시킵니다. 자식 수명 주기는 [재귀 에이전트](PROGRAM_RECURSIVE_HARNESSES.md)를 참조하십시오.

### 예산

`LocalProgramHost`에 전달한 `budget_ceiling`은 승인(admission) 정책의 상한이 됩니다. `max_child_depth`와 `max_total_children`을 제외한 모든 필드는 양수여야 하며, 모델을 전혀 호출하지 않는 Program이라도 `model_tokens`, `monetary_microunits`, `max_dynamic_compiles` 역시 양수여야 합니다. 그렇지 않으면 생성자가 0인 필드 이름을 담은 `ValueError`를 발생시킵니다. 두 자식 필드는 둘 다 0이거나 둘 다 양수여야 합니다.

`compile_admit(source, budget)`와 `compile(source, budget)`는 `budget`을 모든 필드의 최솟값이자 최댓값으로 번들에 봉인하며, 봉인된 예산이 상한을 넘는 번들은 승인(admission)에서 거부됩니다. 그 버전에 대한 각 `run`, `start`, `start_active` 호출은 필드마다 같은 값을 전달해야 합니다. 더 크거나 작은 예산은 `P_START_BUDGET`이 담긴 `ng.ProgramDiagnosticError`를 발생시키며, 그 메시지에는 값이 다른 첫 번째 필드가 나옵니다. 예를 들면 `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`입니다. 다른 예산을 쓰려면 다시 컴파일하고 승인(admission)하십시오. 그 결과는 새 `ProgramVersion`입니다. 실행 예산의 `wall_time_ms`, `max_concurrency`, `max_program_operations`, `max_core_steps`는 양수여야 하며, `model_tokens`와 `monetary_microunits`는 0이어도 됩니다. 0은 아무것도 부여하지 않습니다. `model_tokens = 0`으로 모델 토큰을 쓴 실행은 `BudgetExhausted`로 끝나고, `monetary_microunits = 0`이면 가격이 매겨지는 모든 제공자 호출이 보내기 전에 거부됩니다([모델을 호출하는 노드](#model-calling-nodes) 참조).

### 실패 라우팅

`on_failure`가 있는 `retryPolicy(node, {...})`는 노드의 최종 실패를 실행을 실패시키는 대신 핸들러로 라우팅합니다. 규칙은 [Program 그래프 기능](program-graphs.md#failure-routing)에 있습니다. Python 노드는 예외 속성으로 실패를 표시합니다. 문자열 `code`(`app.compile_error` 같은 작성자 코드), 불리언 `retryable`, 문자열 `public_message`입니다. `str(exc)`는 레코드에 절대 들어가지 않습니다. 노드가 `ng.ProgramDiagnosticError`, `ng.ProgramCompileError`, `ng.ProgramAdmissionError`를 발생시키면 구성 판정(`NG.ConfigError`)이 되며, 재시도하지도 라우팅하지도 않습니다. 핸들러에 `interruptBefore`가 있으면 실행은 `Interrupted`로 멈추고 `result.interrupt.pending_input.core_interrupt_value`에 `{"reason": "failure_route", "failure": record}`가 들어 있습니다. `host.resume(run_id, value)`는 핸들러에서 이어 갑니다. failure exit는 선언된 코드로 실행을 `Failed`로 끝내고, failure edge가 없는 노드 실패는 분류된 코드(`NG.TaskFailed`, 작성자 코드 등)로 `Failed`가 됩니다.

`ProgramRunBudget.max_node_retries`(기본값 `None`, 상한 없음)는 실행 전체의 노드 재시도를 제한합니다. `result.usage.node_retries`가 그 횟수이고 `result.remaining_budget.max_node_retries`가 남은 양입니다. `compile_admit`은 이를 봉인하지 않으므로 다시 컴파일하지 않고도 실행마다 설정할 수 있습니다. `LocalProgramHost(..., failure_text_redactor=fn)`와 `GraphEngine.compile(..., failure_text_redactor=fn)`은 `error_text`를 설정한 노드를 위한 redactor를 받습니다. `fn`은 `{"node", "code", "kind", "attempts", "retryable", "public_message"}`를 받아 남길 텍스트나 `None`을 돌려줍니다. 없으면 텍스트를 쓰지 않습니다. `GraphEngine`에서는 failure exit 뒤 `result.status`가 `ng.RunStatus.FailureExit`이고, `engine.resolved_retry_policy(node)`는 노드가 실제로 쓰는 재시도 타이밍을 돌려줍니다.

`retryPolicy(node, {attempt_timeout_ms: ...})`는 노드의 각 시도를 제한합니다(100~3 600 000 ms). 타임아웃된 시도에서는 `input.ctx.cancel_token.is_cancelled()`가 참이 되고, 시도는 다른 실패처럼 재시도되고 라우팅될 수 있는 `NG.Timeout`으로 실패합니다. 그 뒤에 시도가 돌려주는 것은 모두 버려집니다. Python 본문은 선점할 수 없으므로, 잠들거나 블로킹하는 노드는 토큰을 확인해야 합니다. 다음 시도는 타임아웃된 시도가 반환한 뒤에야 시작하며, 정지 마감 시각(다시 타임아웃만큼, 최소 1초) 안에 반환하지 않은 시도는 `NG.EffectUncertain`으로 실행을 실패시킵니다. 그런 본문은 백그라운드에서 계속 실행되고, 바인딩은 인터프리터가 종료될 때 이를 기다립니다. 실행의 `wall_time_ms`를 넘게 될 백오프는 kind `deadline`으로 재시도를 멈춥니다. `GraphEngine`에서는 `engine.set_node_attempt_timeout_ms(node, ms)`가 호스트 타임아웃을 설정하고(`None`이면 지우며, 정의 자체의 타임아웃은 줄이기만 할 수 있습니다), `engine.resolved_attempt_timeout_ms(node)`가 노드가 실제로 쓰는 타임아웃을 돌려줍니다. [Program 그래프 기능](program-graphs.md#attempt-timeouts-and-the-deadline)을 참고하세요.

### 스케줄러 스레드, 워커, 타임아웃

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)`는 두 가지 스레드 설정을 받습니다. `scheduler_threads`는 실행을 구동하는(JavaScript 제어, `callCore` 디스패치, join, 데드라인) `ProgramRuntime` 스케줄러 풀의 스레드 수를 정합니다. 1 이상이어야 합니다. `worker_count`는 하나의 그래프 안의 fan-out에 쓰이는 각 Core 엔진의 워커 풀을 정하며, 승인(admission)된 Program의 `max_concurrency`를 넘으면 안 됩니다. 기본값 1에서는 fan-out 분기가 스케줄러 스레드에서 실행됩니다.

Python 노드 본문은 Program 실행 중에 스케줄러 스레드를 점유하지 않습니다. 본문은 바인딩이 소유한 워커 풀에서 실행되며, 이 풀은 기본적으로 스레드 32개입니다. 바꾸려면 첫 Program 실행 전에 `NEOGRAPH_PYTHON_NODE_THREADS`를 1부터 1024까지의 값으로 설정하십시오. 기본값인 스케줄러 스레드 하나로도 호스트의 여러 실행, `ng.all`, `ng.race`, `ng.quorum`의 멤버, 하나의 Core 실행의 fan-out 분기가 동시에 진행되며, 노드가 실행되는 동안에도 데드라인이 발동합니다. `ng.race`는 가장 먼저 끝난 멤버를 반환하고, `ng.quorum`은 필요한 성공이 모이면 반환합니다. 그래도 Python 코드는 GIL 아래에서 실행되므로 CPU 위주의 노드는 병렬로 실행되지 않습니다. 노드는 `time.sleep`, I/O, 네이티브 호출 등에서 GIL을 해제하는 동안 겹쳐 실행됩니다. 일반 `GraphEngine` 실행은 여전히 호출 스레드에서 Python 노드 본문을 실행합니다.

두 제한 모두 실행 중인 Python 노드를 중단시키지 않습니다. 취소는 협력적으로 이루어집니다. 실행의 `wall_time_ms`가 다 되면 런타임은 실행을 취소하고, 결과는 실행 중인 노드가 반환된 뒤에 `status` `TimedOut`, `failure.code` `P_RUNTIME_TIMEOUT`, 메시지 `Program wall-time budget (N ms) expired`로 도착합니다. 타임아웃이 중단시킨 작업의 텍스트는 `failure.witness["cause_message"]`에 있습니다. `ng.await(command, timeoutMs, sourceSite)`의 데드라인이 지나면 실행은 `P_AWAIT_TIMEOUT`과 함께 `TimedOut`으로 끝나며, 데드라인 이후에 도착한 완료는 받아들여지지 않습니다. 두 경우 모두 자식 Core 실행이 취소됩니다. `input.ctx.cancel_token.is_cancelled()`를 확인하는 노드는 일찍 멈추고, 이를 무시하는 노드는 반환될 때까지 결과를 늦춥니다. `TimedOut`과 `Cancelled` 결과는 완료된 Core 노드를 `execution_trace`에 나열합니다.

Program 엔진은 stderr에 lint도 fan-out 안내도 출력하지 않습니다. `EngineConfig::worker_count`를 권하는 안내는 직접 만든 `GraphEngine`에서만 나옵니다.

### 인메모리 상태와 재개

기본적으로 `LocalProgramHost`는 번들, 버전, 활성화, 실행 기록, 체크포인트를 호스트 객체 내부의 메모리에 보관하며 디스크에는 아무것도 쓰지 않습니다. 실행은 그 실행을 시작한 호스트 인스턴스가 살아 있는 동안 그 인스턴스를 통해 재개하십시오. 프로세스를 재시작한 뒤나 다른 `LocalProgramHost`에서는 `resume()`이 `P_RUN_NOT_FOUND`가 담긴 `ng.ProgramDiagnosticError`를 발생시킵니다. 메시지는 `run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them`입니다. PostgreSQL Program 저장소는 C++ 호스트 API입니다.

`sqlite_directory=path`를 주면 호스트는 번들, 버전, 실행 기록, 체크포인트를 그 디렉터리의 SQLite 파일(`program.db`, `checkpoints.db`)에, 제공자 호출을 제공자 호출 journal(`provider_calls.db`)에 보관하므로 실행이 프로세스보다 오래 남습니다. 같은 디렉터리와 같은 레지스트리로 연 호스트가 실행을 이어 갑니다. `resume()`은 중단된 실행을 이어 가고, `reconnect(run_id)`는 멈춘 프로세스가 실행 중인 채로 남긴 실행을 이어 갑니다. 복구는 journal에 기록된 요청을 다시 보내지 않습니다. journal이 응답을 재생하고 호출은 한 번만 청구됩니다. 제공자를 바인딩하는 호스트에는 `native_history_archive=`도 필요합니다(처음에는 `ng.NativeArchive.provision`, 이후에는 `ng.NativeArchive.open`). journal이 모든 응답을 archive의 custody 아래에 보관하기 때문입니다. 자식 예산이 없는 `define()` 전용 Program 실행은 wall time 동안 execution lease를 가집니다. `reconnect()`는 lease가 만료될 때까지 `P_RUN_LEASE_HELD`를 발생시키고, 그 뒤에는 실행을 `TimedOut`으로 끝내며 journal에 기록된 호출을 청구합니다. 크래시 복구는 실행의 마지막 내구 체크포인트(예: 중단이 남긴 체크포인트)에서 재개합니다.

`resume(run_id, value)`는 핸들을 반환합니다. 결과를 받으려면 `.wait()`를 호출하십시오. `pending_id`가 없으면 실행에서 대기 중인 유일한 호출을 재개합니다. 대기 중인 호출이 없거나 여러 개이면 `ValueError`를 발생시킵니다. 호출을 정확히 지정하려면 `result.interrupt.pending_input.call_id`를 `pending_id`로 전달하십시오. `fork(run_id, value, version, budget)`은 실행의 정확한 체크포인트(또는 `checkpoint_id=`)에서 `version`의 새 실행을 시작하고 그 대기 중인 호출에 `value`로 응답합니다. `budget`은 원본 실행의 `remaining_budget` 안에 있어야 하며, `pending_id=`는 `resume()`과 같이 호출을 지정합니다. 포크는 `resume()`과 같이 일시 정지된 실행이 끝낸 작업을 재생합니다. 일시 정지된 스텝에서 끝난 노드나 Send는 다시 실행되거나 과금되지 않으며 그 provider 호출도 다시 전송되지 않습니다(#390). Core 스텝이 여러 인터럽트에서 일시 정지하면 `result.interrupt.pending_input.core_interrupt_id`(또는 `pending_effect.core_interrupt_id`)가 이 대기 호출이 답하는 Core 인터럽트를 지정하며, `resume()`은 정확히 그것에 답합니다. 그 스텝의 다른 인터럽트는 대기 호출 하나씩 다시 묻습니다. Core 인터럽트가 하나이면 `""`입니다(#388). `fork()`도 정확히 그 인터럽트에 답하며, 포크된 실행은 그 스텝의 다른 질문을 다시 묻습니다(#394).

`resume()`이 반환한 결과의 `execution_trace`에는 재개 이후에 실행된 Core 노드만 나열됩니다. 중단 전에 실행된 노드는 중단된 결과에 나타납니다. `b` 앞에서 중단되는 Core `a → b → c`라면 중단된 결과는 `["a"]`, 재개된 결과는 `["b", "c"]`를 가집니다.

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

`HookBoundaryBlocked`는 차단한 Hook의 `invocation_id`와 `state`(`"failed"`, `"timed_out"` 등)를 담습니다. Python 도구는 [도구 실패 계약](reference-en.md#tool-failure-contract)을 따릅니다. 도구 안에서 발생한 `HookBoundaryBlocked`(예: 중첩된 엔진 실행에서 발생한 것)는 실행을 중단하고 `NodeInterrupt`는 실행을 일시 정지하며, 그 밖의 예외는 모델이 읽는 도구 결과가 됩니다.

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

`ng.Send`로 디스패치된 Python 노드가 `ng.NodeInterrupt`를 던지면 다른 노드와 마찬가지로 run이 일시 정지합니다. `run`, `run_stream`, `run_async`, `run_stream_async`는 체크포인트가 있는 중단된 `RunResult`를 반환하고, `resume`, `resume_async`, `resume_from`은 그 Send를 답과 함께 실행하며 Send를 내보낸 노드와 끝난 Send는 다시 실행되지 않고 재생됩니다. 한 스텝의 노드나 Send 여럿이 `ng.NodeInterrupt`를 일으키면 `result.interrupts`는 각각을 `{"id", "node", "value"}`로 나열하고 `result.interrupt_id`는 첫 번째를 지정합니다. `resume`, `resume_async`, `resume_from`은 `interrupt_id=`를 받아 그 인터럽트에 답하며, 없으면 첫 번째에 답합니다. 일시 정지한 다른 노드나 Send는 그 재개에서 자신의 질문을 다시 하고 다음 재개가 그것에 답합니다(#388). 대기 중이 아닌 `interrupt_id`는 `ValueError`를 일으킵니다. `RunMetadata.resume_interrupt_id`도 같은 id를 전달합니다. 답은 재개가 다시 들어가는 스텝에서 그 답이 향하는 노드나 Send에만 전달됩니다. 재개된 run의 이후 스텝에서는 `input.ctx.resume_value`와 tool gate의 `ToolGateContext.resume_value`가 `None`이므로, 거기서 묻는 노드나 gate는 run을 다시 일시 정지합니다(#393). `interrupt_before` 일시 정지 뒤의 재개는 gate가 붙잡아 둔 노드에 값을 주고, `interrupt_after` 일시 정지 뒤의 재개는 어떤 노드에도 주지 않습니다. 한 스텝에 게이트 노드 여럿이 준비되면 각 게이트에 따로 답합니다. 스텝의 모든 게이트가 답을 받을 때까지 run은 다음 게이트 노드 앞에서 다시 멈추고, 그다음 각 게이트 노드는 자신의 게이트에 대한 답을 받습니다(#396). 한 스텝에서 실패한 노드나 Send는 순서와 관계없이 묻는 노드보다 우선합니다. run은 실패를 일으키고 인터럽트를 보고하지 않습니다. 불확실한 효과(payload `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`)만은 조정될 수 있도록 실패 옆에서도 일시 정지합니다(#392).

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
params.message.role = a2a.Role.User
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
