<!-- neograph-i18n: source=docs/python-binding.md locale=ko source_sha256=e376356dbf8e9ae47791d3beacd6167ce86569148053f4255acd8ad0f73b17ea -->
# Python 바인딩

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`은 같은 C++ 런타임을 pybind11로 감싼 Python 인터페이스입니다. wheel에는 Core, LLM, Program/QuickJS, MCP, SQLite 런타임 영속성(durability)이 켜져 있습니다. 소스에서 선택적으로 빌드하면 컴파일한 컴포넌트만 노출됩니다.

이 가이드의 내용은 SchemaProvider SDK `0.3.0`(인터페이스 리비전 `6`, 공유 라이브러리 ABI 리비전 `6`)에 맞춰 빌드한 wheel이 있어야 실행됩니다. wheel에 함께 들어 있는 컴포넌트는 같이 설치해야 합니다. `complete`를 노출하는 이전 wheel은 provider 인터페이스가 달라서 아래의 타입 기반 예제를 실행할 수 없습니다.

```bash
pip install neograph-engine
```

## 타입이 있는 provider 요청과 결과

`Provider`는 요청 준비와 디스패치를 분리합니다. `SchemaProvider`는 SchemaProvider SDK로 계열(family)별 요청을 검증하고 인코딩합니다. `PreparedProviderRequest`는 그 네이티브 준비 결과를 보관하다가, 디스패치 한 번에 소비됩니다.

| 작업 | Python 시그니처 | 결과 |
|---|---|---|
| descriptor 승인(admission) | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor` (올바르지 않은 닫힌 JSON이면 `ValueError` 발생) |
| provider 생성 | `SchemaProvider(descriptor, options, defaults)` | 승인된 엔드포인트/계열과 런타임 정책을 가진 provider |
| 요청 생성 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 타입이 있는 `ProviderRequest` |
| 준비 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 한 번 디스패치 | `provider.dispatch(prepared)` | 소유권을 가진 `ProviderOutcome` |
| 준비와 디스패치를 한 번에 | `provider.invoke(request)` | 소유권을 가진 `ProviderOutcome` |

`model`은 반드시 명시해야 합니다. `ProviderControls`는 `max_output_tokens`, `temperature`, `top_p` 같은 타입이 있는 제한과 생성 제어를 제공합니다. 팩토리가 SDK의 계열별 payload를 만들며, 이 payload를 원시 딕셔너리로 대신할 수는 없습니다. 스트리밍을 요청하려면 `request.mode`를 `ProviderMode.Stream`으로 설정합니다. `on_event`만 지정해서는 스트리밍이 선택되지 않습니다. `request`에는 `cancel_token`, `on_event`, `observer_limits`, 그리고 선택 사항인 `timeout_ms`도 들어 있습니다.

`ProviderControls.provider`, `response_format`, `SchemaProviderDefaults.provider`, `OpenRouterRouting.max_price`, `ProviderToolResult.host`는 원본과 분리된(detached) 선택적 레코드를 반환합니다. 값이 있으면 레코드를 읽어 수정한 다음 같은 속성에 다시 대입해야 합니다. `controls.provider.order`만 바꿔서는 controls에 반영되지 않습니다. 레코드를 지우려면 `None`을 대입합니다.

`request.timeout_ms`에 표현할 수 있는 0 이상의 값을 대입하면 그 즉시 절대 데드라인이 시작됩니다. 준비 직전에 설정합니다. 이후에 읽으면 남은 시간이 밀리초로 나오고, 데드라인이 지나면 0으로 고정됩니다. `None`으로 두면 준비할 때 provider의 기본 타임아웃을 씁니다. 준비된 핸들을 들고 기다려도 데드라인이 갱신되지는 않습니다.

### descriptor와 런타임 정책

descriptor는 `load_provider_descriptor`가 받는, 버전이 있는 닫힌 JSON입니다. 여기에서 계열, `base_url`, 경로, 필드 바인딩, 인증 규칙을 승인(admission)합니다. `model`은 요청에서, 자격 증명은 런타임 옵션에서 지정합니다. `ProviderRuntimeOptions`로는 `api_key`, `default_timeout_ms`, `ca_file`, `workers`, SDK의 전송 및 리소스 제한을 지정하고, `SchemaProviderDefaults`로는 타입이 있는 provider 기본값을 지정합니다. 자격 증명은 descriptor 파일이나 로그에 넣지 말고 런타임 옵션에만 두어야 합니다.

`ProviderDescriptorPolicy.identity`는 SDK 정책의 SHA-256 다이제스트 원시 값을 담은 Python `bytes`를 반환합니다. 화면에 표시할 때는 `policy.identity.hex()`를 사용합니다. identity 자체는 UTF-8 텍스트도, 16진수 문자열의 별칭도 아닙니다.

현재 전송 계층은 libcurl을 사용합니다. `prefer_libcurl` 스위치, 런타임에서 엔드포인트를 바꾸는 기능, WebSocket 전송은 없습니다. descriptor 승인(admission)과 지원하는 요청 계열은 [C++ provider 레퍼런스](reference-en.md)에서 설명합니다.

[SDK 사용 가이드](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)를 따라 요청 한 건만 받는 루프백 peer를 띄운 다음, 다른 터미널에서 텍스트만 주고받는 아래 예제를 실행합니다. 예제의 엔드포인트, 모델, 출력 상한은 이 peer에 맞춰져 있고 자격 증명은 보내지 않습니다. HTTPS peer를 쓴다면 신뢰할 CA를 `NG_EXAMPLE_CA_FILE`로 지정합니다. 호스팅된 엔드포인트를 쓰려면 descriptor의 origin과 요청의 모델을 직접 바꾸고, 그 origin의 자격 증명을 런타임 옵션에 넣어야 합니다. 호스팅된 서비스를 호출하면 요금이 발생할 수 있습니다.

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

핸들이 필요 없다면 `provider.invoke(request)`로 준비와 디스패치를 한 번에 할 수 있습니다. `outcome.text`는 사용자에게 보이는 텍스트만 추린 것이고, `outcome.messages`에는 타입이 있는 파트 전체가 남아 있습니다. 사용량 카운터가 없으면 `unknown`을 출력하고, 보고된 값이 0이면 `0`을 출력합니다.

링크한 합성(synthetic) peer를 쓸 때 예상 출력은 다음과 같습니다.

```text
consumed: True
Hello.
output tokens: 0
```

peer의 `usage` 멤버를 모두 지우고 다시 시작하면 카운터가 없는 경로를 확인할 수 있습니다. 마지막 줄이 `output tokens: unknown`으로 바뀝니다. 이 확인은 프로토콜 매핑을 검증할 뿐, 실제 모델의 토큰 계산을 검증하는 것이 아닙니다.

### 계열(family)별 controls

패키지 버전, 네이티브 archive v3, 이식 가능한 JSON v2는 서로 별개입니다. 아래 코드 조각은 디스패치 없이 controls만 만듭니다. 각 조각은 그 계열의 승인(admission)된 provider와 함께 써야 합니다. 첫 번째 루프백 요청은 바꾸지 않습니다. OpenRouter 전용 controls는 승인(admission)되지 않은 origin을 I/O 이전에 거부합니다.

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

레코드는 기본 생성자로 만든 뒤 필드를 대입합니다. 이 레코드들에는 키워드 생성자가 없습니다. 선택적 레코드와 벡터는 분리된 복사본을 반환하므로, 수정한 뒤 다시 대입해야 합니다. `OpenRouterRouting.max_price`가 그런 경우입니다. `routing.max_price.prompt_nano_usd = ...`는 복사본만 수정하므로, `ng.OpenRouterMaxPrice`를 만들어 객체 전체를 대입합니다. 금액은 정수 nanoUSD이며 `prompt_nano_usd`와 `completion_nano_usd`는 100만 토큰당 값이고, 그 밖에 `image_nano_usd`, `audio_nano_usd`, `request_nano_usd`가 있습니다. 전송할 때는 정확한 십진 USD 값으로 변환됩니다(`75_000_000`은 `0.075`). 이 중 하나 이상은 반드시 지정해야 하고 2**53을 넘는 값은 허용되지 않습니다. `SchemaProviderDefaults.provider`는 `max_price`를 다른 라우팅 필드와 같은 방식으로 병합합니다. 요청에 지정한 값이 기본값을 대체합니다. `ChatReasoningOptions`에는 `max_tokens`와 `exclude`도 있습니다. 중첩된 Chat reasoning, `include_reasoning`, `usage_include`, `models`는 선언된 OpenRouter origin이 있어야 쓸 수 있습니다. 스칼라 `reasoning_effort`, `service_tier`, 타입이 있는 라우팅, 응답 형식은 계속 쓸 수 있습니다.

Responses의 verbosity는 `Low/Medium/High`, truncation은 `Disabled/Auto`입니다. include에서는 `WebSearchSources`, `FileSearchResults`, `MessageOutputTextLogprobs`, `ComputerCallOutputImageUrl`, `CodeInterpreterCallOutputs`도 지원합니다. `responses_include=None`이면 암호화된 reasoning을 포함하는 기본 include를 유지합니다. `[]`를 포함해 명시적인 리스트를 지정하면 그 리스트가 그대로 적용되고, 이후 네이티브 재생(replay)을 할 수 없게 될 수 있습니다. 기존의 store, reasoning, 호스팅 도구, 도구 호출 상한은 그대로 타입이 있는 설정으로 남아 있습니다.

Messages의 thinking 모드는 `Manual/Adaptive/Disabled`입니다. Manual이면 `thinking_budget`이 승인(admission)된 최솟값 이상이면서 출력 상한보다 작아야 합니다. 모드를 지정하지 않고 budget만 지정하면 Manual로 선택됩니다. Adaptive와 disabled에서는 budget을 지정할 수 없습니다. thinking을 켜면 벤더의 규칙에 따라 temperature가 전송에서 빠지지만, 모델이 temperature를 금지하는 경우에는 명시적으로 지정한 temperature를 거부합니다. effort는 `Low/Medium/High/Max`, 캐시 TTL은 `FiveMinutes/OneHour`이고, tool choice는 `Auto/Any/None_/Tool`입니다. `Tool`을 쓰려면 선언된 클라이언트 도구의 `name`이 필요합니다. Messages에서 라우팅을 쓰려면 선언된 OpenRouter origin이 있어야 합니다.

Gemini의 thinking level은 `Minimal/Low/Medium/High`이며 `thinking_budget`과 함께 쓸 수 없습니다. tool choice는 `Auto/Any/None_/Validated`이고 `allowed_function_names` 리스트를 받으며 `required_tool`과 함께 쓸 수 없습니다. 안전 카테고리는 `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`, 임계값은 `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`입니다. 계열이 맞지 않거나 범위를 벗어난 값, 서로 호환되지 않는 thinking/상한/도구 controls는 I/O 이전에 거부됩니다. 모델 접두사에 따른 temperature 금지는 마지막 `/` 뒤의 접미사까지 포함해 ASCII 대소문자를 구분하지 않고 비교합니다. Chat/Responses에서는 `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`가, Messages에서는 `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`이 해당합니다. [SDK controls 승인(admission)](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)을 참고하세요.

### 승인(admission) 전의 배포 헤더

일반 `load_provider_descriptor(source, policy=None)`는 `${VAR}`를 문자 그대로 취급합니다. 아래 헬퍼는 실제 descriptor 승인(admission) 전에 호스트의 값을 미리 처리합니다.

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)`는 Messages용으로 선택 사항인 `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA`를 읽습니다. 설정하지 않았거나 비어 있는 값은 생략합니다. descriptor에 문자 그대로 적은 헤더는 환경 변수보다 우선하고, 명시적으로 넘긴 쌍은 대소문자를 구분하지 않고 둘 모두보다 우선합니다. 중복된 override, 유효하지 않거나 예약된 이름, 줄바꿈은 승인(admission)에 실패합니다. 두 헬퍼 모두 선택 인자 `policy`를 받습니다. 두 헬퍼는 이미 승인(admission)된 descriptor를 수정하지 않으며 인코더 템플릿도 평가하지 않습니다.

### Responses 커서와 외부 Gemini 기록

`previous_response_id`는 provider가 보관한 Responses 상태를 선택합니다. 전체 기록을 다시 보내지 말고 새 입력만 보냅니다. 성공한 Responses 호출에서 이어 가려면 그 호출에 바인딩된 controls와 실제 종료 ID를 그대로 유지합니다.

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

`previous_response_history`는 `ProviderMessage`의 벡터이며, 응답 하나나 JSON 객체가 될 수 없습니다. 이 값은 전송되지 않습니다. 클라이언트 도구의 소유권을 실제로 확인하려면 원래 prefix 전체와, ID가 커서와 같은 마지막 assistant 메시지가 필요할 수 있습니다. 서버가 보관하는 텍스트를 그대로 이어 가는 경우에는 비워 둘 수 있습니다. 이후 같은 프로세스에서 나온 커서 출력은 마지막 메시지의 비공개 소유권을 유지하지만, 그렇다고 전체 기록의 네이티브 재생(replay)이나 archive 권한이 되지는 않습니다. 실패했거나 편집되었거나 origin/모델/설정/route 상태가 맞지 않으면 거부합니다. 전체 네이티브 재생(replay)과 archive에는 여전히 원래의 전체 prefix가 필요합니다. [SDK Responses 이어 가기](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)를 참고하세요.

호출자가 만든 외부(foreign) assistant `Text`/`ProviderToolCall` 기록을 네이티브 상태, wire 출력, 서명 없이 쓰려면 `gemini.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign`을 명시적으로 설정합니다. 기본값은 `NativeOnly`입니다. 첫 번째 외부 함수 호출에만 Google의 서명 검증 우회 값이 붙고, 텍스트만 있는 턴에는 서명이 붙지 않습니다. 실제 네이티브 그룹은 여전히 엄격하게 검증합니다. 이 모드는 실패한 봉인을 복구하거나 제거하거나 격하할 수 없고, reasoning 권한을 가져오거나 이식 가능한 데이터에 네이티브 재생(replay) 권한을 부여할 수도 없습니다. [SDK 외부 Gemini 기록](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)을 참고하세요.

PortableForeign은 서명뿐 아니라 가져온 wire 메타데이터도 거부합니다. 커서 출력은 `NativeReplay.complete == False`이며 archive 대상이 될 수 없습니다. 커서 출력의 비공개 소유자가 전체 기록 재생(replay)을 승인하지는 않습니다.

### 준비된 핸들과 Python provider

디스패치는 준비된 핸들을 한 번 소비하며, 디스패치가 실패해도 마찬가지입니다. 디스패치 전에는 `prepared.valid`와 `prepared.error`를, 이후에는 `prepared.consumed`를 확인합니다. Python 객체를 계속 들고 있어도 다시 쓸 수 있게 되지는 않습니다. 다시 시도하려면 새 요청을 만들고 준비해야 합니다.

Python 서브클래스는 `Provider(family)`를 호출하고 `get_name()`과 `prepare(request)`를 구현합니다. 서브클래스는 준비 작업을 `SchemaProvider`에 위임하고 그 provider가 만든 실제 준비 핸들을 반환할 수 있습니다. 성공한 SDK 결과를 직접 만들거나 JSON에서 디스패치 권한을 가져올 수는 없습니다. 공통 `invoke`와 `dispatch`는 네이티브 수명 주기를 사용합니다. 예전의 `complete` 메서드를 오버라이드해도 이 수명 주기를 구현한 것이 아닙니다.

네이티브 실행이 Python의 `prepare` 오버라이드를 호출했을 때 `None`을 반환하면, 핸들이 소비되기 전에 `TypeError`가 발생합니다. 실제 `PreparedProviderRequest`를 반환해야 합니다.

`NodeContext(provider=provider)`로 컨텍스트를 만들거나 `ctx.provider`에 대입하면, 원래의 Python provider가 네이티브 공유 소유자 lease에 보관됩니다. 네이티브 컨텍스트, 컴파일된 노드, 엔진의 복사본은 이 lease를 유지하므로, `ctx.provider`를 다시 대입하거나 Python 쪽 참조가 모두 수거된 뒤에도 같은 객체의 오버라이드를 호출합니다. 다시 대입하면 변경 가능한 컨텍스트가 가진 lease만 해제되며, 앞으로 컴파일할 때만 영향을 줄 뿐 이미 만들어진 엔진에는 영향이 없습니다. 이 방식은 객체의 동일성과 수명을 유지할 뿐, provider의 변경 가능한 상태를 고정하지는 않습니다. 마지막 lease가 해제될 때 GIL을 잡은 채로 Python 소유자를 해제합니다.

제거된 `CompletionParams`, `ChatCompletion`, `OpenAIProvider`, `RateLimitedProvider`에는 호환용 별칭이 없습니다. 검증된 `SchemaProvider`를 만들어 요청 팩토리를 사용해야 합니다. `ChatMessage`와 `ToolCall`은 그래프에서 편하게 쓰는 값으로 남아 있으며, SDK의 전체 `ProviderMessage`, `ProviderToolCall`과는 다릅니다.

### 결과, 실패, 사용량

`ProviderOutcome`은 변경할 수 없는 SDK 결과를 소유합니다. `outcome.completion`이나 `outcome.failure`를 확인합니다. 값이 없는 쪽은 `None`입니다. completion과 failure 뷰에는 순서가 있는 전체 메시지, 사용량, 시도 증거, 종료/오류 정보, 보관된 wire 증거가 그대로 담겨 있습니다. 실패한 경우에도 부분 출력이 남아 있을 수 있습니다. 실패를 보고할 때는 이 증거를 함께 유지해야 하며, 부분 텍스트를 성공 결과로 반환해서는 안 됩니다.

`ProviderCompletion.wire_envelope`와 `ProviderPartialCompletion.wire_envelope`는 계열마다 다른 증거이며 값이 없을 수도 있습니다(nullable). 읽기 전에 `wire_envelope is not None`인지 확인합니다. 현재 버퍼링 방식의 Chat 디코더는 이 값을 `None`으로 두고, 응답 JSON 전체를 `raw_events`에 `ProviderRawWire`(SDK의 `RawWire`)로 보관합니다. 이때 `type == "chat.completion"`이고 문서는 `payload`에 들어 있습니다. envelope가 있다고 가정하지 않고 실제 타입이 있는 raw 이벤트를 확인합니다. raw 이벤트가 있다고 해서 실패가 성공이 되지는 않으며, envelope가 없다고 해서 보관된 응답이 사라진 것도 아닙니다. SDK는 envelope를 대신 만들어 주는 대체 경로를 두지 않습니다.

소유한 결과 또는 그 결과에서 보관한 타입 뷰를 계속 들고 있으면, 실제 메시지, 네이티브 소유자, 사용할 수 있는 wire 문서가 호출이 끝나고 provider가 수거된 뒤에도 유효합니다. raw payload에는 provider 내부 필드가 그대로 남아 있지만, 네이티브 트레이싱은 raw envelope/이벤트와 네이티브 재생(replay)/reasoning을 제외합니다. Python의 JSON 뷰는 데이터 복사본이며 네이티브 재생(replay) 권한도, 금전 권한도 아닙니다.

필요한 의미가 역할과 텍스트라면, 타입이 있는 전체 메시지/파트를 읽고 논리적인 역할과 텍스트를 비교합니다. 체크포인트와 Chat 요청의 텍스트 content는 문자열일 수도, 타입이 있는 텍스트 파트의 배열일 수도 있습니다. 호출자는 우연히 나온 직렬화 형태 하나만 가정해서는 안 됩니다. 이어 가기에는 전체 파트와 실제 네이티브 소유권을 사용해야 하며, 텍스트나 JSON으로 평탄화한 대용품을 써서는 안 됩니다.

`ProviderMessage.parts`, completion/partial의 `messages`와 `raw_events`, 사용량의 `extra`/`conflicts`는 바인딩된 값을 포함해 분리된 리스트나 맵을 반환합니다. 원래의 컬렉션을 교체한 뒤에도 보관해 둔 파트는 안전하게 읽을 수 있고, 복사본을 바꿔도 변경할 수 없는 결과는 달라지지 않습니다. 메시지를 수정하려면 `parts = message.parts`로 받아 `parts`를 고친 다음 `message.parts = parts`로 다시 대입합니다. `message.parts.append(...)`는 임시 Python 리스트만 바꿉니다.

SDK 실패는 예외가 아니라 반환되는 데이터입니다. 호스트 observer나 예산 정산(settlement)에서 실패하면 `ProviderObserverError` 또는 `ProviderBudgetSettlementError`가 발생하며, 둘 다 `ProviderOutcomeError`에서 파생됩니다. 이 예외들은 실제 결과(outcome)와 원인(cause)을 유지합니다. 따라서 콜백이 실패했다고 해서 다시 디스패치해도 된다는 뜻은 아닙니다.

저장해 둔 Python provider 또는 그래프 예외의 원인을 여러 번 조회해도 원래의 예외 객체와 traceback이 그대로 유지됩니다. 네이티브 중첩 예외 변환을 거쳐 도달한 원인도 마찬가지입니다.

사용량 카운터는 `value`와 `evidence`를 가진 `UsageCount` 값이고, 알 수 없으면 `None`입니다. 보고된 0은 알려진 사용량이므로 카운터가 없는 경우와 다릅니다. `count.value`를 읽기 전에 `count is not None`을 확인합니다. 회계에서는 `count or 0`을 쓰거나, 알 수 없는 입력/출력/합계 카운터를 0으로 바꾸어서는 안 됩니다.

`outcome.usage.provider_cost`는 변경할 수 없는 `ProviderReportedCost` 뷰입니다. 선택 필드인 `ProviderUsdAmount`는 `total`, `upstream_total`, `upstream_input`, `upstream_output`이며, 각각 `nano_usd`, `evidence`, `rounding`을 가집니다. `status`는 이 순서를 따르는 4개짜리 배열이고, `byok_status`는 null 가능한 `is_byok`를 설명합니다. `ProviderCostStatus`, `ProviderCostSource`, `ProviderCostRounding`은 값 없음, 사용 가능, 형식 오류, 정밀도 초과, 오버플로, 알 수 없는 통화, 충돌이라는 증거를 그대로 보존합니다. 금액은 원래의 십진 표기가 아니라 파싱한 binary64 값에서 올림한 것입니다. 보고된 0이나 `is_byok=False`는 값이 없다는 뜻이 아닙니다. 이 관찰 값은 청구서도, 토큰 예산 청구도 아닙니다. 호출별 금액 증거는 토큰을 합산한 bank가 아니라 순서가 있는 `provider_outcomes`에서 확인합니다.

`UsageAccumulator`는 토큰만 집계합니다. 보고서가 하나뿐이면 금액 메타데이터가 그대로 유지되지만, 두 번째 보고서가 들어오면 스냅샷의 `provider_cost`는 첫 호출의 오래된 비용도 합계도 아닌 정규 Missing/None으로 명시적으로 초기화됩니다. 그 스냅샷을 복원해도 비용이 되살아나지 않고 토큰 권한이 갱신되지도 않습니다. 정확한 금액 증거는 원래의 각 결과(outcome)나 provider 저널 항목에서 읽어야 합니다.

재개하거나 이어 갈 때 `RunResult.provider_outcomes`에는 원래의 결과가 순서대로 먼저 오고, 그 뒤에 새로 만들어진 결과가 붙습니다. `RunResult.usage`는 현재 사용 중인 회계 bank의 값을 반영하며, 이 bank는 체크포인트에서 이전 보고서를 복원할 수 있습니다. 따라서 새 provider 호출 없이 재개했더라도 사용량이 `None`이나 0이라는 보장은 없습니다. 증거를 복원할 때 원래의 provider 요청을 다시 디스패치해서는 안 되고, 같은 비용을 두 번 청구해서도 안 됩니다. 보관된 보고서는 이전 호출을 설명할 뿐이며 새로운 지출 허용량을 부여하지 않습니다.

### 네이티브 기록과 이식 가능한 내보내기

반환된 `ProviderMessage`에는 타입이 있는 전체 파트와 실제 네이티브 재생(replay) 상태가 들어 있습니다. 네이티브 재생(replay) 상태와 wire 출력은 읽기 전용입니다. provider 호출을 직접 이어 갈 때는 원래 요청의 `request.messages` prefix 전체를 그대로 두고 그 뒤에 `outcome.messages`를 붙입니다. 이 목록에는 입력 기록 전체가 아니라 반환된 출력만 들어 있습니다. 첫 번째 호출 예제의 `request`와 `outcome`을 쓴다면 `history = request.messages + outcome.messages`를 만든 뒤, `ng.make_provider_request(provider, "example-model", history, controls=controls)`로 다음 요청을 만듭니다. 이렇게 만드는 것만으로는 아무것도 전송되지 않습니다. 다음 턴에는 새 사용자 메시지나 필요한 도구 결과도 함께 넣어야 합니다.

실제 `NativeContext` 재생(replay)은 반환된 assistant 메시지뿐 아니라 원래의 prefix도 검사합니다. assistant 출력만 넘기면 wire 디스패치 전에 `ReplayIneligible`로 실패합니다. 그 assistant 메시지를 `NativeArchive`에서 불러와도 보관(custody)과 prefix 바인딩은 유지되지만, 원래의 전체 prefix는 여전히 필요합니다.

그래프에서는 `RunConfig.provider_messages`가 전체 기록을 받고, `RunResult.native_messages`에는 캡처한 전체 기록이 이미 들어 있습니다. 그래프 결과를 기록으로 쓸 때는 원래 입력을 앞에 다시 붙이지 않습니다. `RunResult.provider_outcomes`는 보관된 결과를 노출하고, 노드 안에서는 `RunContext.provider_outcomes`와 `provider_loop_history`에 provider 증거가 남아 있습니다.

`RunConfig.provider_messages`, `RunResult.native_messages`, `ProviderLoopEntry.messages`도 분리된 기록 복사본을 반환합니다. 변경 가능한 입력 기록을 고치려면 반환된 리스트를 수정한 뒤 `config.provider_messages`에 다시 대입합니다. 그 입력을 교체한 뒤에도 보관된 메시지와 파트는 유효하며, 결과와 루프 기록 속성은 계속 읽기 전용입니다.

사용자 정의 노드에서 네이티브 기록을 채널 쓰기에 담으려면 `provider_messages_write(messages_or_outcome)`를 사용합니다. `portable_message(ChatMessage)`는 이식 가능한 내용을 가져오며 가져온 네이티브 reasoning은 거부합니다. `project_message(ProviderMessage)`는 관찰용으로만 쓰는 그래프 투영을 만듭니다.

네이티브 기록을 영구 저장하려면 `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)`을 사용합니다. 이 호출은 archive 또는 `ProviderError`를 반환합니다. `save(messages, binding="")`는 참조 또는 오류를, `load(reference, binding="")`는 타입이 있는 메시지 또는 오류를 반환합니다. SDK가 제공하는 인증된 로컬 보관(custody)과 별도의 호스트 키를 사용하며, 이식 가능한 JSON이 재생(replay) 권한이나 관리되는 예산 권한으로 바뀌지는 않습니다.

이식 가능한 레코드는 지금도 `RuntimeHistoryRecord.serialize_canonical()`로 직렬화할 수 있습니다. 실제 네이티브 메시지가 들어 있는 레코드를 영구 저장하려면 `owner_scope`가 `owner_id`와 일치하는 실제 `NativeArchive`로 `record.serialize_canonical(archive, owner_id)`를 호출합니다. 복원은 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`로 하며, 이식 가능한 레코드라면 기본값으로 충분합니다. 하지만 네이티브 archive 참조에는 같은 소유자 범위의 archive가 필요합니다. 인자 없이 직렬화하거나 JSON을 가져와서는 네이티브 권한을 되살릴 수 없습니다. Python의 parse 호출과 archive를 쓰는 직렬화 호출은 archive I/O를 포함한 네이티브 작업 동안 GIL을 해제합니다.

`RuntimeHistoryRecord.message`는 분리된 타입 복사본을 반환하며, 이 복사본은 실제 공유 네이티브 소유자를 유지합니다. 이 복사본을 바꿔도 변경할 수 없는 RAW 레코드의 identity는 달라지지 않습니다. `ContextStore.hydrate_records(range)`는 타입이 있는 레코드를 반환하고, `history_record_by_message_id(feed, message_id)`는 레코드 또는 `None`을 반환합니다. `InMemoryContextStore`와 `SQLiteContextStore`는 이 메서드를 상속합니다. `SQLiteContextStore(database_path, archive=None)`는 네이티브 보관을 위한 실제 archive를 받습니다. archive 없이 저장하면, 보관이 필요한 경우 네이티브 기록을 거부합니다. 생성과 타입 조회는 GIL을 해제합니다. `LocalProgramHost`는 마지막 선택 키워드 인자로 `native_history_archive=None`을 받아 실제 Program 런타임에 전달합니다. 이 인자는 보관을 제공할 뿐 추가 grant나 영속 Program 저장소 백엔드를 제공하지 않습니다.

이식 가능한 상태 딕셔너리와 JSON 내보내기는 메시지 데이터를 설명할 뿐입니다. 네이티브 재생(replay) 상태, provider origin, 관리되는 예산 권한은 이런 데이터로 되살릴 수 없습니다. 내보낸 JSON의 `native` 플래그는 설명용이며 권한 토큰이 아닙니다. `ChatMessage`, 평평한 텍스트 채널, JSON을 거치면 provider 고유의 파트가 사라질 수 있습니다. 그런 파트가 필요하면 원래의 prefix와 함께 타입이 있는 전체 기록을 보존해야 합니다.

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

### 컴파일 시점의 도구 소유권

Python `Tool` 인스턴스, 네이티브 C++ 도구, `MCPClient.get_tools()` 결과는 `ng.NodeContext(tools=[...])`로 한꺼번에 넘깁니다. 컴파일할 때 그래프 노드를 만들기 **전에** 이 목록을 스냅샷으로 떠서 네이티브 `ToolSet`이 소유하게 합니다. 컴파일된 엔진은 컨텍스트의 `tools` 리스트를 바꾸거나 외부 참조를 놓은 뒤에도 `run()`과 `resume()`에 걸쳐 같은 도구를 그대로 유지합니다. 목록을 나중에 바꾸면 그 이후의 컴파일에만 영향을 줍니다. 네이티브 MCP 도구는 비동기 실행 경로를 그대로 유지하며, 컴파일 이후에 소유권을 넘기거나 디스패치마다 소유권을 새로 할당하는 일은 없습니다.

## Core API 동등성

Python은 별도의 Python 스케줄러를 두지 않고 C++의 실행 기능을 그대로 노출합니다.

- 동기 방식과 asyncio 방식의 run/stream/resume
- 정확한 체크포인트를 지정하는 `resume_from`, fork, 상태 조회, 순서가 있는 상태 쓰기
- 그래프 인터럽트와 `NodeInterrupt`로 하는 정적·동적 HITL
- `RunMetadata`의 데드라인, trace/run 식별자, 모델 토큰 상한
- 그래프 전체 및 노드별 `RetryPolicy`(지터 포함)
- 실행 범위로 한정되거나 명시적으로 재사용하는 `CacheScope`
- 체크포인트 및 장기 Store 백엔드
- 사용자 정의 노드, 리듀서, 조건, provider, 도구
- 도구 게이트, 실행 정책, 필수 수명 주기 Hook, 엄격한 런타임 인터포지션

### 동등성 계약

여기서 말하는 동등성은 Python이 같은 네이티브 실행 경로와 안전 계약에 도달한다는 뜻입니다. C++ 내부의 모든 저장소 타입이나 권한 타입을 Python으로 복제한다는 뜻은 아닙니다.

| 기능 | 네이티브 C++ 경로 | Python 인터페이스 | 상태 |
|---|---|---|---|
| Core 그래프 컴파일과 실행 | `GraphEngine` | `GraphEngine.compile`, run/stream/async 메서드 | 같은 스케줄러, 같은 런타임 |
| 런타임 식별자, 데드라인, 예산 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 실행마다 같은 값 |
| 재시도 및 노드 캐시 정책 | `RetryPolicy`, `CacheScope` | 그래프/노드 setter와 캐시 범위 | 같은 런타임 정책 |
| 체크포인트, HITL, 시간 여행 | 체크포인트 저장소와 resume API | resume, 정확한 `resume_from`, fork, 상태 이력/갱신 | 같은 체크포인트 계약 |
| Program 작성과 로컬 실행 | 컴파일러, Catalog, `ProgramRuntime` | `ProgramCompiler`, `LocalProgramHost`, 핸들/결과 | 네이티브 소유자 범위의 편의용 호스트 |
| 필수 수명 주기 Hook | 레지스트리, 러너, `HookRuntime` | 정의와 `create_hook_runtime` 콜백 | 같은 실패 시 차단 수명 주기 경계 |
| 런타임 컨텍스트와 strict dispatch | 컨텍스트 저장소, receipt, 인터포지션 | 대응하는 불변 값, 저장소, `StrictRuntimeProfile` | 같은 네이티브 컨트롤러 |
| wheel의 영속 기본값 | SQLite Core/컨텍스트/디스패치 저장소 | `_HAVE_SQLITE` export | PyPI wheel에서 활성화 |

원시 `ProgramCatalog`, 전환 저장소, 교체/마이그레이션 컨트롤러, 합성 게이트웨이, Hook 저널, RPC executor는 호스트를 구성할 때 쓰는 API로 남아 있습니다. 권한을 가진 이 경로들의 일부만 노출하면 `proposal -> compile -> admit -> publish -> migrate/spawn`이라는 필수 프로토콜을 우회하게 됩니다. 앞으로 Python 호스트 컨트롤러가 생긴다면 이 프로토콜과 갱신할 수 없는 lineage 예산을 소유자 범위의 단위 하나로 묶어야 합니다. `_HAVE_PROGRAM`은 원시 컨트롤 플레인 관리 기능의 동등성까지 보장하지 않습니다.

### 런타임에서 재시도 정책 덮어쓰기

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

그래프 정의의 `"retry_policy"`는 선언적 기본값으로 그대로 남습니다. 런타임 setter는 그와 별개인 C++/Python 설정 인터페이스입니다.

### 메타데이터와 정확한 재개

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

Python 노드 안에서는 `input.ctx.trace_id`, `run_id`, `has_deadline`, `deadline_remaining_ms`, `model_token_budget`으로 같은 값을 읽을 수 있습니다.

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)`는 기본적으로 데드라인 없이 시작합니다. 생성자에 넘기는 timeout과 `metadata.set_timeout_ms(timeout)`는 steady clock이 표현할 수 있는 남은 범위 안의 0 이상 정수 밀리초만 받습니다. 부호 있는 duration으로 변환하거나 데드라인에 더하기 전에 이 범위를 검증합니다. 음수이거나 범위를 넘는 정수는 `OverflowError` 또는 `ValueError`를 발생시키며, setter가 실패해도 이전 데드라인은 그대로 남습니다. 0은 즉시 만료되는 데드라인을 설정합니다. 데드라인을 없애려면 `metadata.clear_deadline()`을 호출합니다.

### 캐시 범위

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable`은 그 노드가 테넌트, provider, Store, 도구, 자격 증명, 시간, 재개 상태와 무관하다고 명시적으로 선언하는 값입니다.

## Program과 QuickJS

Python wheel에는 `neograph::program`과 제한된 QuickJS 프런트엔드가 포함되어 있습니다. Python으로 정의한 노드는 변경할 수 없는 Program 레지스트리에 참여하고 네이티브 `ProgramRuntime`으로 실행할 수 있습니다.

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

`ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer`, `add_registered_condition`은 이 프로세스에 이미 등록된 실행 대상을 복사해 옵니다. 등록은 `@ng.node`나 `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer`, `ng.ConditionRegistry.register_condition`으로 합니다. 등록되지 않은 이름을 넘기면 세 메서드 모두 `ng.ProgramRegistryError`를 발생시키며, 메시지에 어떤 등록 호출이 필요한지 적혀 있습니다. 이 오류의 `kind`는 `"node"`, `"reducer"`, `"condition"` 중 하나이고 `name`은 빠진 이름입니다. `ValueError`와 `RuntimeError`를 모두 상속하므로, 둘 중 어느 쪽을 잡도록 작성한 핸들러든 그대로 이 오류를 잡습니다. 레지스트리 스냅샷에 없는 노드 타입, 리듀서, 조건을 쓰는 Program은 대신 컴파일 시점에 실패합니다. `ProgramCompiler.compile`과 `LocalProgramHost.compile_admit` 모두 `ng.ProgramCompileError`와 진단 코드 `P_CORE_GC_REGISTRY_MISS`를 냅니다.

진단 dict에는 모두 `help` 항목이 있습니다. 컴파일이 반환하는 진단에서는 `doc`([진단 가이드](diagnostics.md)의 항목), `related`, `suggestions`, `candidates`를 담은 dict이고, 봉인된 `bundle.diagnostics`에서는 `None`입니다. 예외에는 항상 이 항목이 들어 있습니다. `ProgramCompiler(..., echo_routes=True)`를 쓰면 라우팅된 후속 노드 표를 담은 `P_CORE_ROUTES` 메모도 반환합니다. `ProgramCompiler`나 `LocalProgramHost`에 `soundness_check=True`를 주면 [건전성 검사](program-graphs.md#soundness-check)를 실행합니다. 이 검사는 witness 추적이 달린 `P_CORE_S1`부터 `P_CORE_S5`까지의 경고와 `P_CORE_SOUNDNESS` 요약을 냅니다. 이 중 어느 것도 봉인되지 않습니다.

`LocalProgramHost`는 소유자 범위의 인메모리 편의용 호스트입니다. 컴파일러, Catalog, 승인(admission) 정책, 전환 저장소, ProgramRuntime은 여전히 C++ 쪽 구현을 씁니다. 생성된 제안은 승인(admission) 전에 호스트의 의미 검증을 추가로 통과해야 합니다. [DSL 기능 평가](DSL_CAPABILITY_EVAL.md)를 참고하세요.

`LocalProgramHost`를 소멸시키면, 실제 `ProgramRuntime`이 스케줄러 작업을 취소하고 비우고 join하는 동안 호출자의 GIL을 놓습니다. 그래서 실행 중인 Python 노드가 끝날 수 있습니다. 호스트의 나머지 멤버를 소멸시키기 전에는 GIL을 다시 잡으며, 이 멤버들의 Python 콜백/객체 소유자는 GIL에 안전한 소멸을 보장합니다.

네이티브 Program의 기록된 실행 API는 `ProgramRuntime::replay_recorded`이며 `start_recorded`를 대체합니다. `LocalProgramHost`는 `run`, `start`, `resume`, `fork`를 제공하고, 기록된 바인딩의 원시 컨트롤 플레인은 노출하지 않습니다. Program 결과에는 기존의 핸들, 예산, 권한 필드와 함께 네이티브 런타임이 제공한 타입이 있는 provider 결과와 실패 증거가 담깁니다.

`ProgramResult.failure`는 딕셔너리가 아니라 읽기 전용 `ProgramFailure` 값이거나 `None`입니다. `provider_outcome`과 `provider_cause`에는 provider 실패 증거가 남고, `code`, `message`, `operation_id`, `core_node`, `attempts`, `witness`는 실패한 연산을 설명합니다. 결과 필드에는 `bundle_id`, `operation_id`, `attempt`, `checkpoint`, `interrupt`, `provider_budget_authority`도 있습니다.

설치된 JavaScript 어휘 전체를 dict로 얻을 수 있습니다.

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### 모듈 구조와 `callCore`

JavaScript Program은 그래프 빌더 하나를 반환하는 `define()`을 export해야 합니다. 이 그래프가 Program의 유일한 Core입니다. `define()`이 없으면 컴파일이 `ProgramCompileError`와 `P_JS_DEFINE_MISSING`을 발생시키므로, `main()`만 export하는 모듈은 거부됩니다. `main()`은 선택 사항이며, 없으면 실행이 Core를 직접 실행합니다.

`ng.callCore(name, input, sourceSite)`에서 name에는 `ng.graph(...)`에 준 이름을 써야 하며, 위 예제에서는 `"main"`입니다. 다른 이름을 쓰면 실행이 `P_JS_CONTROL_COMMAND`로 실패합니다. 단계를 달리하려면 그 그래프 안에 단계를 두고 입력을 바꿔 다시 호출합니다. 노드 타입은 모두 `@ng.node`나 `ng.NodeFactory.register_type`으로 먼저 등록한 뒤 `add_registered_node`에 넘겨야 합니다. 등록되지 않은 타입을 넘기면 `ng.ProgramRegistryError`가 발생합니다.

`callCore`가 완료되면 Core의 출력 객체가 `main()`에 반환됩니다. 위의 빠른 시작 예제는 이 객체를 그대로 `result.output`으로 반환합니다.

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels`에는 채널마다 `value`와 `version`이 들어 있습니다. 각 채널의 값은 같은 이름의 최상위 키로도 복사됩니다. 그 키가 이미 있으면 복사하지 않습니다. 그래서 `main()`은 `result.value`를 읽을 수 있습니다. `channels`나 `global_version`이라는 이름의 채널은 `channels` 아래에서만 접근할 수 있습니다. barrier 목록은 정렬해서 저장됩니다. `graph.barrier("join", ["zeta", "alpha"])`는 `bundle.sealed_core_definitions`에 `{"wait_for": ["alpha", "zeta"]}`로 나타나며, 순서는 동작에 영향을 주지 않습니다.

### command가 실패하면 실행이 끝납니다

완료된 command만 `main()`을 다시 진행시킵니다. yield한 command가 완료되지 않는 경우가 있습니다. 예를 들어 `callCore`의 노드가 예외를 던지거나, Core 스텝 예산을 소진하거나, 더 이상 발동할 수 없는 barrier에서 멈추거나, wall-time 한도를 넘기는 경우입니다. 이때 `ProgramRuntime`은 그 결과를 기록하고 그에 맞는 `status`와 `failure`가 담긴 실행의 최종 결과로 삼습니다. 제너레이터는 재개되지 않습니다. `yield`를 감싼 `try`/`catch`는 실행되지 않고, 이후의 JavaScript도 실행되지 않습니다. 실패 정책에 따라 실패한 join(`ng.all`, `ng.race`, `ng.quorum`)도 같은 방식으로 실행을 끝냅니다. 실패는 Python에서 `result.status`와 `result.failure`로 처리합니다. 인터럽트된 command는 다릅니다. 실행은 `Interrupted`로 멈추고 `resume()`이 그 command를 이어서 진행합니다. join의 멤버가 인터럽트되면 그 join이 일시 중지됩니다. join은 새 멤버를 더 시작하지 않고, 이미 실행 중인 멤버가 끝나도록 둡니다. 그 멤버들의 결과로 join이 결정되면(fail-fast 실패, race 승자, quorum 충족 또는 충족 불가) 그 결과로 끝납니다. 그렇지 않으면 첫 번째로 멈춘 멤버에서 실행이 `Interrupted`로 멈추고, `resume()`이 그 멤버를 이어서 진행하면서 이미 끝난 멤버는 command 저널에서 재생(replay)합니다. 이 멤버들은 다시 실행되거나 과금되지 않습니다. `ng.await(command, timeout_ms)`의 데드라인은 절대 시각입니다. command 저널에 await가 시작된 시각이 기록되므로, 재개하거나 중단된 프로세스에서 복구할 때는 남은 시간만 쓸 수 있습니다. 데드라인이 지난 뒤에 재개하면 await는 응답받은 노드를 실행하지 않고 즉시 실행을 `TimedOut`과 `P_AWAIT_TIMEOUT`으로 끝냅니다(#391). plan 수준의 `await` 연산도 재개를 거쳐 같은 방식으로 데드라인을 유지합니다. 불확실한 효과로 멈춘 멤버, 즉 payload의 `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`는 Core 스텝에서처럼 멤버의 실패보다 우선합니다(#392). join은 실패하지 않고 그 멤버에서 일시 중지하며, 실패한 멤버는 기록해 두었다가 재개할 때 그 실패로 끝납니다. 성공이 결과를 정하는 join은 그 결과로 끝납니다(#397). 재개할 때는 응답이 이어 가는 멤버가 끝까지 실행된 뒤에 기록된 실패로 join이 끝나므로, 응답이 그 멤버에 도달하고 그 효과가 확정됩니다(#400). join이 일시 중지될 때는 선언 순서와 상관없이 불확실한 효과로 멈춘 멤버에서 먼저 멈춥니다(#401). 결과를 모으는 `ng.all`(`collect: true`)은 이 우선순위의 의도된 예외입니다. 이 노드는 일시 중지된 멤버까지 포함해 모든 멤버를 기다린 뒤에 결과를 보고합니다. 그래서 다른 멤버가 일반 질문으로 멈춰 있는 동안에는 한 멤버의 실패로 끝나지 않고 그 멤버에서 실행이 `Interrupted`로 멈추며, 모든 멤버가 정리된 뒤에 실패를 보고합니다(#401).

### 그래프 검사와 barrier 정체

컴파일은 스케줄러가 실제로 따라가는 경로를 기준으로 Core 그래프를 검사합니다. 조건부 엣지가 있는 노드는 첫 번째 조건부 엣지로만 라우팅됩니다. JavaScript Program은 엄격한 문서이므로 아래 각 경우에 `ng.ProgramCompileError`가 발생합니다.

- `P_JS_GRAPH_ARGUMENT`: `.node()` 호출이 노드 이름을 `__start__`나 `__end__`로 지정한 경우
- `P_JS_GRAPH_ENTRY`: `define()`이 노드는 있지만 `.entry()`가 없는 그래프를 반환한 경우
- `P_CORE_E13`: 조건부 엣지도 있는 노드에서 나가는 정적 엣지. 그 엣지는 발동하지 않기 때문입니다.
- `P_CORE_E14`: 같은 노드에서 나가는 두 번째 조건부 엣지. 이 엣지로는 라우팅되지 않기 때문입니다.

`P_CORE_E15`는 `bundle.compile_diagnostics`에 나오는 경고입니다. barrier 멤버에 join이나 `__end__`가 아닌 곳으로 이어지는 경로가 있고, 그 경로를 타면 join이 정체된다는 뜻입니다. 루프는 유효합니다. 모든 순환을 `P_CORE_E16`으로 거부하려면(예: plan이나 체크리스트) `ProgramCompiler`나 `LocalProgramHost`에 `require_acyclic=True`를 넘깁니다.

barrier는 `wait_for` 목록의 모든 노드가 자신으로 라우팅되었을 때 발동합니다. 다른 경로로 join에 도달한 노드는 그 멤버를 대신하지 못합니다. 예를 들어 멤버의 실패 경로에 둔 복구용 노드가 그렇습니다. 실행할 노드가 더 없는데 join에 신호가 남아 있으면 실행은 완료되지 않습니다. 이때 실행은 `P_CORE_BARRIER_STALLED`와 함께 `Failed`로 끝나고, `failure.core_node`는 그 join을 가리키며, `failure.witness["stalled_barriers"]`에는 정체된 join마다 `wait_for`, `received`, `missing` 노드가 나열됩니다. 이런 실행이 정상적으로 끝나는 경우는 마지막 슈퍼스텝에서 `__end__`로 가는 경로를 명시했을 때뿐입니다. `__end__`로 가는 정적 엣지(`.exit()`가 만드는 엣지), `__end__`로 가는 조건부 라우팅, `Command.goto("__end__")`가 여기에 해당합니다. 후속 노드가 없다고 해서 끝나지 않으며, 더 앞선 슈퍼스텝에서 명시한 종료도 인정되지 않습니다. 같은 상태의 `GraphEngine` 실행은 `ng.RunStatus.BarrierStalled`를 반환하고, 같은 내용이 `result.output["_neograph"]["barrier_stalled"]`에 들어 있습니다.

### 오류와 진단

Python에서 발생하는 Program 오류는 타입이 있고 진단 정보를 유지합니다.

| 예외 | 발생 위치 | 상위 클래스 |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`, `LocalProgramHost.compile`, `compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit`와 `compile_admit` (예: `P_ADMIT_POLICY`, `P_ADMIT_PROVIDER`) | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`, `start`, `start_active`, `resume` (예: `P_START_BUDGET`, `P_RUN_NOT_FOUND`) | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`, `add_registered_reducer`, `add_registered_condition` | `ValueError`, `RuntimeError` |

앞의 세 예외의 메시지는 `Program compilation failed`, `Program admission failed`, `Program operation failed` 중 하나로 시작하고, 이어서 진단마다 한 줄이 붙습니다. 각 줄에는 코드, JSON 포인터, 메시지가 있고, 소스 위치가 있으면 `(source:line:col)`도 붙습니다. `.diagnostics`는 `code`, `severity`, `phase`, `message`, `source_id`, `json_pointer`, `span`, `witness`를 가진 dict의 리스트입니다.

컴파일에 성공한 bundle에는 목록이 두 개 있습니다. `bundle.compile_diagnostics`는 컴파일러가 반환한 목록으로, 현재 규칙에 따른 경고(JavaScript 호출 위치는 `span`에 있음)와 `P_JS_CONTROL_CHARACTER` 같은 작성 경고가 들어 있습니다. 작성자에게는 이 목록을 보여 줍니다. `bundle.diagnostics`는 bundle에 봉인되어 `bundle.id`에 포함되는 사본입니다. bundle이 처음 봉인될 때의 규칙과 문구를 그대로 유지하므로 새 컴파일러를 써도 bundle id가 바뀌지 않으며, 그 때문에 두 목록은 다를 수 있습니다. 봉인된 사본에는 span이 없고, 현재 규칙에서는 사라진 경고가 남아 있을 수 있습니다. 예를 들어 Python 노드만 쓰는 채널에 나오는 `P_CORE_E6`가 그렇습니다. 컴파일러도 `LocalProgramHost`도 lint 결과를 stderr에 출력하지 않습니다.

<a id="model-calling-nodes"></a>
### 모델을 호출하는 노드

`LocalProgramHost`는 자신이 승인(admission)한 Program의 내장 `llm_call` 노드에 provider 하나를 바인딩합니다. 레지스트리는 provider를 이름으로 선언하고 그 provider가 필요한 노드를 지정합니다. 호스트는 provider 객체, 모델, 요청 controls를 비밀 값이 아닌 `binding_label` 아래에 제공합니다.

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

`add_provider(name, semantic_version, implementation_digest)`는 provider의 identity를 등록하고, `add_registered_node("llm_call", ..., provider=name)`은 내장 노드를 그 identity를 정적 요구 사항으로 지닌 채 등록합니다. `provider=`는 `llm_call`에만 바인딩할 수 있으며, 다른 타입이나 알 수 없는 provider를 지정하면 `ValueError`가 발생합니다. 호스트는 descriptor id가 무엇이든 `node_context`의 provider를 그 이름에 바인딩합니다. `NodeContext(provider_controls=...)`에는 노드가 전송할, `max_output_tokens` 같은 타입이 있는 `ProviderControls`를 넘깁니다. `node_context`에는 provider와 `binding_label`이 필요하고 도구는 필요 없습니다.

바인딩에서 어떤 identity에 들어가는 부분은 레이블뿐입니다. 그 provider를 요구하는 Program 버전에는 capability receipt가 붙는데, 이 receipt의 identity는 레이블과 provider의 레지스트리 identity를 합친 다이제스트입니다. provider의 키, 엔드포인트, 모델은 여기에 들어가지 않습니다. 그래서 모델을 바꿔도 버전 id는 그대로이고, 레이블을 바꾸면 버전 id가 바뀝니다. provider를 요구하지 않는 Program에는 receipt가 붙지 않으며 bundle id와 버전 id도 그대로입니다. 레이블은 `a-z`, `0-9`, `.`, `_`, `-`, `:` 중 1~64자여야 하고, 영문자나 숫자로 시작해야 하며, API 키처럼 보이면 안 됩니다. `node_context`가 없으면, 그 provider를 요구하는 Program은 승인(admission) 단계에서 `P_ADMIT_PROVIDER`로 거부됩니다. `provider=` 없이 등록한 `llm_call`이나 `intent_classifier` 노드가 있는 Program도 마찬가지입니다.

호출마다 NeoGraph에 내장된 가격 카탈로그로 비용을 산정하고 그 값을 실행의 `monetary_microunits`와 대조합니다. 요청을 보내기 전에 그 호출의 상한 금액을 예약합니다. OpenRouter에서는 요청에도 이 상한이 `provider.max_price`로 함께 실립니다. 값은 route 상한의 입력/출력 단가이고, controls나 `SchemaProviderDefaults`에 이미 더 낮은 `max_price`가 있으면 그 값입니다. 이렇게 하면 OpenRouter가 더 비싼 엔드포인트로 호출을 보내지 못합니다. 가격 산정 과정에서 요청의 탐색용 사본(대화가 사용자 메시지 하나인 요청)을 준비하되 전송하지는 않으므로, Python provider의 `prepare`가 이 사본을 보게 됩니다. 상한을 정할 수 없는 호출(카탈로그에 없는 모델, route 상한이 없는 router 별칭, 출력 상한이 없는 요청)이나 예약액이 남은 예산에 맞지 않는 호출은 아무것도 전송하기 전에 거부되고, 실행은 `P_MONEY_*` 코드로 실패합니다. 모든 `ProgramRunBudget` 필드가 그렇듯 `monetary_microunits = 0`은 아무것도 허용하지 않으므로, 이 값이면 모든 호출이 거부됩니다. 응답이 온 뒤에는 `result.monetary_records`에 호출마다 `ng.ProgramMonetaryRecord`가 하나씩 들어 있고, 각 레코드의 `evidence`는 다음 중 하나입니다. `reported`는 OpenRouter의 `usage.cost`, `estimated`는 카탈로그 단가(카탈로그는 `catalog_version`과 `catalog_digest`로 식별), `held`는 결과나 사용량을 알 수 없어 예약액을 그대로 청구한 경우입니다. `result.usage.monetary_microunits`는 이들의 합계입니다. 코드, 상한, 정산 규칙은 [가격 증거와 가격 카탈로그](reference-en.md#pricing-evidence-and-the-price-catalog)에 있습니다.

모델을 호출하는 Python 노드는 자기 provider를 직접 소유하며, Program 예산에는 이런 호출이 잡히지 않습니다.

### 자식 Program

`ng.spawn`에는 자식 바인딩 resolver가 필요한데 `LocalProgramHost`는 resolver를 설정하지 않습니다. 따라서 Python에서 `ng.spawn`을 호출하면 항상 실행이 `P_CHILD_BINDING`("Program spawn has no configured child binding resolver")으로 실패합니다. 자식 Program을 쓰려면 `RuntimeConfig::child_binding_resolver`를 설정하는 C++ 호스트가 필요합니다. 이 resolver는 바인딩 이름마다 `link_module_child`가 만든 `ModuleLinkReceipt`와, 승인(admission)된 자식 `ProgramVersion`을 대응시킵니다. 자식의 수명 주기는 [재귀적 에이전트](PROGRAM_RECURSIVE_HARNESSES.md)를 참고하세요.

### 예산

`LocalProgramHost`에 넘기는 `budget_ceiling`은 승인(admission) 정책의 상한이 됩니다. `max_child_depth`와 `max_total_children`을 제외한 모든 필드는 양수여야 합니다. 모델을 전혀 호출하지 않는 Program이라도 `model_tokens`, `monetary_microunits`, `max_dynamic_compiles`는 양수여야 합니다. 그렇지 않으면 생성자가 0인 필드를 이름으로 알려 주는 `ValueError`를 발생시킵니다. 자식 관련 두 필드는 둘 다 0이거나 둘 다 양수여야 합니다.

`compile_admit(source, budget)`와 `compile(source, budget)`는 `budget`을 모든 필드의 최솟값이자 최댓값으로 bundle에 봉인하며, 봉인된 예산이 상한을 넘는 bundle은 승인(admission)에서 거부됩니다. 그 버전을 실행하는 `run`, `start`, `start_active` 호출은 필드 하나하나가 같은 값을 넘겨야 합니다. 예산이 더 크거나 작으면 `ng.ProgramDiagnosticError`와 `P_START_BUDGET`이 발생하고, 메시지에는 처음으로 다른 필드가 적혀 있습니다. 예: `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`. 다른 예산을 쓰려면 다시 컴파일하고 승인(admission)해야 하며, 그 결과는 새 `ProgramVersion`입니다. 실행 예산에서는 `wall_time_ms`, `max_concurrency`, `max_program_operations`, `max_core_steps`가 양수여야 하고, `model_tokens`와 `monetary_microunits`는 0이어도 됩니다. 0은 아무것도 허용하지 않는다는 뜻입니다. `model_tokens = 0`인데 모델 토큰을 쓰는 실행은 `BudgetExhausted`로 끝나고, `monetary_microunits = 0`이면 가격이 매겨지는 provider 호출은 전부 전송 전에 거부됩니다([모델을 호출하는 노드](#model-calling-nodes) 참고).

### 실패 라우팅

`on_failure`가 있는 `retryPolicy(node, {...})`는 노드의 최종 실패가 실행을 실패시키는 대신 핸들러로 라우팅되게 합니다. 규칙은 [Program 그래프 기능](program-graphs.md#failure-routing)에 있습니다. Python 노드는 예외 속성으로 실패를 표현합니다. 문자열 `code`(`app.compile_error` 같은 작성자 코드), 불리언 `retryable`, 문자열 `public_message`가 그 속성입니다. `str(exc)`는 레코드에 들어가지 않습니다. `ng.ProgramDiagnosticError`, `ng.ProgramCompileError`, `ng.ProgramAdmissionError`를 발생시키는 노드는 설정 오류 판정(`NG.ConfigError`)을 보고하며, 이 판정은 재시도하거나 라우팅하지 않습니다. 핸들러에 `interruptBefore`가 있으면 실행은 `Interrupted`로 멈추고 `result.interrupt.pending_input.core_interrupt_value`에 `{"reason": "failure_route", "failure": record}`가 담깁니다. `host.resume(run_id, value)`는 핸들러에서 이어서 진행합니다. 실패 종료 노드(failure exit)는 선언된 코드와 함께 실행을 `Failed`로 끝냅니다. 실패 엣지가 없는 노드 실패도 분류된 코드(`NG.TaskFailed`, 작성자 코드 등)와 함께 실행을 `Failed`로 끝냅니다.

`ProgramRunBudget.max_node_retries`(기본값 `None`, 상한 없음)는 실행 전체의 노드 재시도 횟수를 제한합니다. `result.usage.node_retries`는 사용한 횟수를, `result.remaining_budget.max_node_retries`는 남은 횟수를 나타냅니다. `compile_admit`은 이 값을 봉인하지 않으므로, 다시 컴파일하지 않고도 실행마다 설정할 수 있습니다. `LocalProgramHost(..., failure_text_redactor=fn)`과 `GraphEngine.compile(..., failure_text_redactor=fn)`은 `error_text`를 설정한 노드에 쓸 redactor를 받습니다. `fn`은 `{"node", "code", "kind", "attempts", "retryable", "public_message"}`를 받아 남길 텍스트를 반환하거나 `None`을 반환합니다. redactor가 없으면 텍스트를 기록하지 않습니다. `GraphEngine`에서는 실패 종료 뒤의 `result.status`가 `ng.RunStatus.FailureExit`이고, `engine.resolved_retry_policy(node)`는 노드가 실제로 따르는 재시도 타이밍을 반환합니다.

`retryPolicy(node, {attempt_timeout_ms: ...})`는 노드의 시도마다 시간을 제한합니다(100~3 600 000 ms). 시간이 초과된 시도에서는 `input.ctx.cancel_token.is_cancelled()`가 true가 되고 시도는 `NG.Timeout`으로 실패합니다. 이 실패는 다른 실패와 마찬가지로 재시도하거나 라우팅할 수 있습니다. 그 뒤에 노드가 반환하는 값은 버려집니다. Python 본문은 선점할 수 없으므로, sleep하거나 블로킹하는 노드는 토큰을 폴링해야 합니다. 다음 시도는 시간이 초과된 시도가 반환한 뒤에야 시작됩니다. 중지 데드라인(다시 timeout 값, 최소 1초) 안에 반환하지 않으면 실행이 `NG.EffectUncertain`으로 실패합니다. 이런 본문은 백그라운드에서 계속 실행되며, 인터프리터가 종료될 때 바인딩이 그 본문이 끝나기를 기다립니다. 백오프가 실행의 `wall_time_ms`를 넘기게 되면 `deadline` 종류로 재시도를 멈춥니다. `GraphEngine`에서는 `engine.set_node_attempt_timeout_ms(node, ms)`로 호스트 타임아웃을 설정하고(`None`이면 해제하며, 정의에 있는 타임아웃은 줄일 수만 있습니다), `engine.resolved_attempt_timeout_ms(node)`는 노드가 실제로 따르는 값을 반환합니다. [Program 그래프 기능](program-graphs.md#attempt-timeouts-and-the-deadline)을 참고하세요.

### 스케줄러 스레드, 워커, 타임아웃

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)`에는 스레드 설정이 두 가지 있습니다. `scheduler_threads`는 실행을 구동하는 `ProgramRuntime` 스케줄러 풀의 스레드 수입니다. 이 풀이 JavaScript 제어, `callCore` 디스패치, join, 데드라인을 처리합니다. 값은 1 이상이어야 합니다. `worker_count`는 그래프 하나 안의 fan-out을 위한 Core 엔진별 워커 풀 크기이며, 승인(admission)된 Program의 `max_concurrency`를 넘을 수 없습니다. 기본값 1이면 fan-out 분기가 스케줄러 스레드에서 실행됩니다.

Program을 실행하는 동안 Python 노드 본문은 스케줄러 스레드를 점유하지 않습니다. 바인딩이 소유한 워커 풀에서 실행되며 기본 크기는 32개 스레드입니다. 이 크기를 바꾸려면 첫 Program 실행 전에 `NEOGRAPH_PYTHON_NODE_THREADS`를 1~1024 범위의 값으로 설정합니다. 스케줄러 스레드가 기본값 하나뿐이어도 호스트의 여러 실행, `ng.all`, `ng.race`, `ng.quorum`의 멤버, 하나의 Core 실행 안의 fan-out 분기가 동시에 진행되고, 노드가 실행되는 중에도 데드라인이 발동합니다. `ng.race`는 가장 먼저 끝난 멤버를 반환하고, `ng.quorum`은 필요한 성공 횟수가 채워지면 반환합니다. Python 코드는 여전히 GIL 아래에서 실행되므로 CPU 중심의 노드는 병렬로 실행되지 않습니다. 노드가 `time.sleep`, I/O, 네이티브 호출처럼 GIL을 놓는 동안에는 서로 겹쳐 실행됩니다. 일반 `GraphEngine` 실행은 Python 노드 본문을 계속 호출한 스레드에서 실행합니다.

어느 제한도 실행 중인 Python 노드를 중단시키지 않으며, 취소는 협력적으로 동작합니다. 실행의 `wall_time_ms`가 다 지나면 런타임이 실행을 취소하고, 실행 중이던 노드가 반환한 뒤에 결과가 도착합니다. 이 결과는 `status`가 `TimedOut`, `failure.code`가 `P_RUNTIME_TIMEOUT`이고 메시지는 `Program wall-time budget (N ms) expired`입니다. 타임아웃이 중단시킨 연산의 텍스트는 `failure.witness["cause_message"]`에 있습니다. `ng.await(command, timeoutMs, sourceSite)`의 데드라인이 지나면 실행은 `TimedOut`과 `P_AWAIT_TIMEOUT`으로 끝나며, 데드라인 뒤에 도착한 완료는 받아들이지 않습니다. 두 경우 모두 자식 Core 실행이 취소됩니다. `input.ctx.cancel_token.is_cancelled()`를 확인하는 노드는 일찍 멈추고, 이를 무시하는 노드는 반환될 때까지 결과를 지연시킵니다. `TimedOut`과 `Cancelled` 결과의 `execution_trace`에는 완료된 Core 노드가 나열됩니다.

Program 엔진은 lint 결과나 fan-out 안내를 stderr에 출력하지 않습니다. `EngineConfig::worker_count`를 권장하는 안내는 직접 만든 `GraphEngine`에서만 나옵니다.

### 인메모리 상태와 재개

기본적으로 `LocalProgramHost`는 bundle, 버전, 활성화 정보, 실행 레코드, 체크포인트를 호스트 객체 안의 메모리에 보관하며 디스크에는 아무것도 쓰지 않습니다. 실행을 재개하려면 그 실행을 시작한 호스트 인스턴스가 살아 있는 동안 그 인스턴스로 재개해야 합니다. 프로세스를 다시 시작한 뒤나 다른 `LocalProgramHost`에서는 `resume()`이 `ng.ProgramDiagnosticError`와 `P_RUN_NOT_FOUND`를 발생시킵니다. 메시지는 `run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them`입니다. PostgreSQL Program 저장소는 C++ 호스트 API입니다.

`sqlite_directory=path`를 지정하면 호스트는 bundle, 버전, 실행 레코드, 체크포인트를 그 디렉터리의 SQLite 파일(`program.db`, `checkpoints.db`)에, provider 호출을 provider 호출 저널(`provider_calls.db`)에 보관합니다. 그래서 실행이 프로세스보다 오래 살아남으며, 같은 디렉터리를 같은 레지스트리로 연 호스트가 그 실행을 이어 갑니다. `resume()`은 인터럽트된 실행을 이어 가고, `reconnect(run_id)`는 프로세스가 중단되면서 실행 중인 채로 남은 실행을 이어 갑니다. 복구는 저널에 기록된 요청을 다시 전송하지 않습니다. 저널이 응답을 재생(replay)하므로 호출은 한 번만 과금됩니다. provider를 바인딩하는 호스트에는 `native_history_archive=`도 필요합니다(처음에는 `ng.NativeArchive.provision`, 이후에는 `ng.NativeArchive.open`). 저널이 모든 응답을 archive의 보관(custody) 아래 두기 때문입니다. 자식 예산이 없고 `define()`만 있는 Program의 실행은 wall time 동안 실행 lease를 잡습니다. lease가 만료될 때까지 `reconnect()`는 `P_RUN_LEASE_HELD`를 발생시키고, 만료된 뒤에는 실행을 `TimedOut`으로 끝내면서 저널에 기록된 호출을 과금합니다. 크래시 복구는 실행의 마지막 영속 체크포인트에서 재개합니다. 인터럽트가 남긴 체크포인트가 그 예입니다.

`resume(run_id, value)`는 핸들을 반환하며, 결과는 `.wait()`로 받습니다. `pending_id`를 지정하지 않으면 실행에서 응답을 기다리는 호출이 하나뿐일 때 그 호출을 재개합니다. 기다리는 호출이 없거나 여럿이면 `ValueError`가 발생하므로, `result.interrupt.pending_input.call_id`를 `pending_id`로 넘겨 호출을 정확히 지정합니다. `fork(run_id, value, version, budget)`는 그 실행의 정확한 체크포인트(또는 `checkpoint_id=`)에서 `version`의 새 실행을 시작하고, 대기 중인 호출에 `value`로 응답합니다. `budget`은 원본 실행의 `remaining_budget` 범위 안이어야 하고, `pending_id=`로 호출을 지정하는 방법은 `resume()`과 같습니다. fork는 `resume()`처럼 일시 중지된 실행이 끝낸 작업을 재생(replay)합니다. 일시 중지된 스텝에서 끝난 노드나 Send는 다시 실행되거나 과금되지 않고, 그 provider 호출도 다시 전송되지 않습니다(#390). Core 스텝이 여러 인터럽트로 멈춰 있으면 `result.interrupt.pending_input.core_interrupt_id`(또는 `pending_effect.core_interrupt_id`)가 이 대기 호출이 응답하는 Core 인터럽트를 가리키고, `resume()`은 정확히 그 인터럽트에만 응답합니다. 그 스텝의 나머지 인터럽트는 대기 호출 하나씩 차례로 다시 질문합니다. Core 인터럽트가 하나뿐이면 이 값은 `""`입니다(#388). `fork()`도 정확히 그 인터럽트에 응답하며, fork된 실행은 그 스텝의 나머지 질문을 다시 던집니다(#394). JavaScript `main()`을 fork하면 `resume()`처럼, 원본이 멈춘 command에서 이어 갑니다. `main()`은 원본 실행의 입력으로 다시 실행되고(fork의 `input`은 쓰이지 않음), 이미 끝난 command는 원본의 command 저널에서 재생되며 다시 실행, 전송, 과금되지 않습니다. 멈춰 있던 `callCore`는 자신의 체크포인트에서 `value`로 재개됩니다(#395). fork가 재생하는 기록된 실패는 provider 증거를 유지합니다. fork는 이 증거를 원본 실행의 보관(custody)에서 자신의 보관으로 다시 바인딩하므로, `result.failure.provider_outcome`은 원본과 마찬가지로 fork에서도 복원됩니다(#398).

`resume()`이 반환한 결과의 `execution_trace`에는 재개 이후에 실행된 Core 노드만 나열됩니다. 인터럽트 전에 실행된 노드는 인터럽트된 결과에 나타납니다. `a → b → c` 형태의 Core가 `b` 앞에서 인터럽트되면, 인터럽트된 결과는 `["a"]`를, 재개한 결과는 `["b", "c"]`를 가집니다.

## 필수 수명 주기 Hook

Hook은 모델이 도구를 호출하기로 결정해서 실행되는 것이 아니라 호스트의 수명 주기 이벤트로 실행됩니다.

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

`FailClosed`에서 콜백이 실패하면 보호 대상인 런타임 경계가 차단됩니다. 관측 정보를 잃어도 괜찮은 경우에만 `Continue`를 쓸 수 있습니다.

`HookBoundaryBlocked`에는 차단된 Hook의 `invocation_id`와 `state`(`"failed"`, `"timed_out"` 등)가 들어 있습니다. Python 도구는 [도구 실패 계약](reference-en.md#tool-failure-contract)을 따릅니다. 도구 안에서(예: 중첩된 엔진 실행에서) `HookBoundaryBlocked`가 발생하면 실행이 중단되고, `NodeInterrupt`가 발생하면 실행이 일시 중지됩니다. 그 밖의 예외는 모델이 읽는 도구 결과가 됩니다.

## 런타임 컨텍스트, Skill, strict dispatch

바인딩은 변경할 수 없는 RAW 기록 레코드, 컨텍스트 artifact, epoch, 필수 Skill/제약, 변환 receipt, provider 디스패치 receipt를 노출합니다.

RAW `RuntimeHistoryRecord`를 만들 때 Assistant 메시지에는 `RuntimeHistoryRecordData.trust`를 `RuntimeTrustClass.ModelOutput`으로 설정합니다. 기본값인 `UntrustedInput`은 User 메시지만 받으며, Assistant 출력에 쓰면 거부됩니다. 이 레이블은 메시지의 출처를 나타낼 뿐입니다. 도구 실행, 예산, 코드 실행 권한을 부여하지 않으며, 실제 네이티브 재생(replay) 보관(custody)을 대체하지도 않습니다.

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

`ContextTransformReceipt`는 파생 증거를 자유롭게 허용하지만, 필수 artifact는 모두 바이트 단위로 동일하게 유지되어야 합니다.

전체 strict 경로에는 영속 SQLite 저장소를 사용합니다.

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

## HITL과 상태

정적 `interrupt_before`/`interrupt_after`, 동적 `NodeInterrupt`, 동기 `resume`, asyncio `resume_async`, 정확한 `resume_from`에는 체크포인트 저장소가 필요합니다.

`ng.Send`로 디스패치된 Python 노드가 `ng.NodeInterrupt`를 발생시키면 다른 노드와 마찬가지로 실행이 일시 중지됩니다. `run`, `run_stream`, `run_async`, `run_stream_async`는 체크포인트가 있는 인터럽트된 `RunResult`를 반환합니다. `resume`, `resume_async`, `resume_from`은 응답을 넘겨 그 Send를 실행하고, Send를 내보낸 노드와 이미 끝난 Send는 다시 실행하지 않고 재생(replay)합니다. 한 스텝의 노드나 Send 여럿이 `ng.NodeInterrupt`를 발생시키면 `result.interrupts`가 모두를 `{"id", "node", "value"}` 형태로 나열하고, `result.interrupt_id`는 그중 첫 번째를 가리킵니다. `resume`, `resume_async`, `resume_from`은 `interrupt_id=`를 받아 그 인터럽트에 응답하며, 지정하지 않으면 첫 번째 인터럽트에 응답합니다. 멈춰 있던 다른 노드나 Send는 그 재개에서 각자 같은 질문을 다시 던지고, 다음 재개가 그 질문에 응답합니다(#388). 대기 중이 아닌 `interrupt_id`를 지정하면 `ValueError`가 발생합니다. 같은 id를 `RunMetadata.resume_interrupt_id`로도 전달할 수 있습니다. 응답은 재개가 다시 들어간 스텝에서 응답을 받은 노드나 Send에만 전달됩니다. 재개된 실행의 이후 스텝에서는 `input.ctx.resume_value`와 도구 게이트의 `ToolGateContext.resume_value`가 모두 `None`이므로, 거기서 질문하는 노드나 게이트는 실행을 다시 일시 중지시킵니다(#393). `interrupt_before`로 멈춘 뒤의 재개는 그 게이트가 붙잡고 있던 노드에 값을 전달하고, `interrupt_after`로 멈춘 뒤의 재개는 어떤 노드에도 값을 전달하지 않습니다. 한 스텝에서 게이트가 있는 노드 여럿이 ready 상태이면 게이트마다 따로 응답합니다. 스텝의 게이트가 모두 응답받을 때까지 실행은 다음 게이트 노드 앞에서 다시 멈추고, 그 뒤에 각 게이트 노드가 자기 게이트의 응답을 받습니다(#396). 한 스텝 안에서는 순서와 상관없이 실패한 노드나 Send가 질문하는 쪽보다 우선합니다. 실행은 실패를 발생시키고 인터럽트는 보고하지 않습니다. 불확실한 효과(payload의 `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`)만은 실패와 함께 있어도 일시 중지하여 조정(reconcile)할 수 있게 합니다(#392).

Python `CheckpointStore` 서브클래스는 `requires_managed_budget(thread_id) -> bool`을 구현할 수 있습니다. 이 동기 가상 메서드는 영속화된 managed-bank 거부 의무를 읽습니다. 엔진의 네이티브 비동기 파사드가 이 메서드를 호출하며, 바인딩은 Python 오버라이드를 위해 GIL을 잡습니다. 체크포인트 상태가 제거되거나 삭제된 뒤에도 영속화된 의무를 사실대로 보고해야 합니다. 이 오버라이드를 생략하면 `False`를 반환하는 대신, 지원하지 않는 백엔드라는 네이티브의 명시적 오류가 그대로 유지됩니다.

이 읽기 메서드는 지출, 복원, lease 권한을 부여하지 않습니다. 관리되는 한도 실행에는 지원되는 실제 네이티브 managed-budget lease가 여전히 필요하며, 이 메서드만 구현해서는 그 lease를 얻을 수 없습니다.

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

상태 조회와 시간 여행에는 `get_state_history`, `update_state`, `fork`를 사용합니다. `get_state_view()`는 Pydantic 기반의 평평한 채널 접근을 제공하고, `get_state()`는 표준 중첩 표현을 그대로 유지합니다.

## 비동기와 취소

`run_async`, `run_stream_async`, `resume_async`는 `asyncio.Future` 객체를 반환합니다. 이 그래프 Future를 취소하면 `CancelToken`으로 취소를 요청합니다. 네이티브 I/O가 취소 경계에 도달해야 작업이 끝납니다. 그래프 스트리밍 콜백은 호출자의 asyncio 루프 스레드로 돌아와서 실행됩니다.

Provider의 `invoke`와 `dispatch`는 동기 Python 메서드입니다. 바인딩은 네이티브 호출이 실행되는 동안 GIL을 놓았다가, Python provider 오버라이드와 이벤트 콜백을 실행할 때 GIL을 다시 잡습니다. 이벤트 콜백은 소유권을 가진 `ProviderEvent` 값을 받으며 네이티브 워커 스레드에서 실행될 수 있습니다. 콜백이 asyncio가 소유한 상태를 갱신해야 한다면 `loop.call_soon_threadsafe`를 사용합니다.

observer는 `request.on_event = callback`으로 지정하고 `None`을 대입하면 해제합니다. `request.on_event`를 읽으면 원래 Python 콜러블의 동일성이 유지되며, `RunConfig.on_provider_event`도 같은 규칙을 따릅니다. 각 이벤트는 문자열 `kind`와 타입이 있는 `value`를 노출하고, `ProviderPartDelta` 값은 자신의 `bytes`를 소유합니다. 콜백이 끝난 뒤에도 이벤트를 보관해도 빌려 온 SDK 텍스트 뷰를 붙잡고 있는 것은 아닙니다. 취소를 요청하려면 `request.cancel_token`에 지정한 토큰의 `token.cancel()`을 호출합니다.

동기 provider 호출이 이벤트 루프 스레드를 막지 않게 하려면 `asyncio.to_thread(provider.invoke, request)`를 사용합니다. 이 await를 취소하는 것만으로는 provider 호출이 취소되지 않습니다. `request.cancel_token`에 `CancelToken`을 지정하고 취소를 명시적으로 요청해야 합니다.

유효한 준비 핸들의 토큰이 이미 취소되어 있으면 provider 디스패치의 사전 점검이 소유권을 가진 SDK `Cancelled` 실패를 반환합니다. 동기 디스패치도 같은 네이티브 비동기 provider 경로를 끝까지 실행합니다. 그래프, 호스트, 코루틴 진입 시점의 취소는 별개의 경계이며, `ProviderOutcome`이 반환되기 전에 예외가 발생할 수 있습니다. 취소를 요청한다고 해서 모든 경계에서 타입이 있는 `Cancelled` 데이터가 반환되는 것은 아닙니다. 또한 취소 요청이 있었다고 해서 요청이 전송되지 않았다는 것, 원격 작업이 멈췄다는 것, 요금이 발생하지 않는다는 것이 증명되지도 않습니다.

GIL을 놓는 일과 Python 콜백을 위해 다시 잡는 일이 바인딩에서 왜 별개의 책임인지는 [pybind11 GIL 문서](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil)에 설명되어 있습니다.

## 프로토콜과 관측성

- 빌드에 포함되어 있으면 MCP 클라이언트 도구를 `neograph_engine.mcp`로 쓸 수 있습니다.
- 빌드에 포함되어 있으면 A2A 클라이언트 타입을 `neograph_engine.a2a`로 쓸 수 있습니다.
- `ProtocolHostAdapter`는 공식 Python A2A/ACP 서버 SDK를 NeoGraph의 세션 의미론과 통합합니다.
- `neograph_engine.tracing`과 `neograph_engine.openinference`는 Phoenix, Langfuse, Arize 및 호환 백엔드를 위한 벤더 중립적인 OTel/OpenInference 데이터를 내보냅니다.

### 네이티브 A2A 탐색과 스트리밍

`a2a.WireDialect`는 `V1_0`(기본)과 `V0_3`(호환) 중 하나입니다. `client.wire_dialect()`는 생성 직후나 카드를 강제로 가져온 직후에는 `None`을 반환하고, 카드로 선택한 RPC나 성공한 프로브가 있어야 값이 채워집니다. `AgentCard.supported_interfaces`에는 `url`, `protocol_binding`, `protocol_version`, `tenant`를 가진 분리된 `AgentInterface` 레코드가 들어 있고, `card.raw`는 관찰용 JSON입니다. 네이티브 클라이언트는 첫 호출 전에 카드를 가져와 호환되는 JSONRPC 인터페이스를 고르며, 정규화된 기본 URL을 우선합니다. RPC는 그 인터페이스의 URL이 기본 URL과 같은 origin이면 그 URL로 갑니다. 카드가 다른 origin을 지정하도록 허용하려면 `client.set_allow_cross_origin_interfaces(True)`를 호출하고, 실제로 쓰는 엔드포인트는 `client.endpoint_url()`로 확인합니다. 카드를 가져올 수 없을 때만 클라이언트가 프로브를 하며, 1.0을 먼저 시도하고 `-32601`, `-32009`, HTTP 400이면 0.3으로 되돌아갑니다. 카드로 선택한 호출은 되돌아가지 않습니다. 메시지 id는 임의의 UUID입니다. `a2a.A2ARpcError`는 실제 정수 `.code`를 유지합니다.

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

이 예제를 실행하기 전에 해당 엔드포인트에서 실제 로컬 A2A 서버를 시작해야 합니다. 이 서버는 Chat 루프백 peer와 별개입니다. `send_message(params)`는 스트리밍 없이 여러 파트를 보내는 오버로드이며, 텍스트용 편의 오버로드도 그대로 있습니다. 자격 증명은 `client.set_authorization_header(authorization_header)`로 직접 설정하고 그 값이 로그에 남지 않게 합니다. `Part.media_type`, `file`, `data`, `metadata`, 메시지의 extension/참조 ID, 작업 artifact에는 타입이 있는 관찰 데이터만 남으며, provider 네이티브 권한은 전혀 없습니다.

`params.message`와 `Task.status`는 원본을 직접 가리키는(live) 인라인 레코드입니다. 선택적 configuration, 벡터, 이벤트의 자식 값은 분리된 스냅샷이므로, 변경 가능한 입력을 갱신하려면 다시 대입해야 합니다. `StreamEvent.type`은 `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task`를 사용하며, `status_update`, `artifact_update`, `task`, `is_final()`로 확인합니다. V1의 시작 task는 최종 상태가 아닙니다. 네이티브 SSE가 상태 갱신과 artifact의 추가/교체 갱신을 조립합니다. 블로킹 호출은 GIL을 놓고, 콜백은 GIL을 잡으며 GIL에 안전한 소유자를 유지합니다. 콜백으로 전달되었다고 해서 출력을 본 뒤에 다시 디스패치해도 되는 것은 아닙니다. 콜백 스레드가 asyncio 루프라는 보장은 없으므로 필요하면 `loop.call_soon_threadsafe`를 사용합니다.

`OpenInferenceProvider`는 `neograph_engine.openinference`에서 import합니다. 생성자는 `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")`이며, 이 파사드는 네이티브 C++ 래퍼에 위임합니다. 래퍼는 타입이 있는 `prepare(request)`, `dispatch(prepared)`, `invoke(request)`를 상속합니다. 기본 span 이름은 레이블일 뿐 `complete` 메서드가 되살아난 것이 아닙니다. `opentelemetry-api`와 `opentelemetry-sdk`를 설치해야 합니다. 생성에는 OTel API가, 내보내기에는 설정된 SDK span processor/exporter가 필요합니다.

요청을 준비하거나, 승인(admission)에 실패하거나, 요청을 포기해도 LLM span은 만들어지지 않습니다. 승인(admission)된 디스패치는 span 하나를 시작하고, 완료, SDK 실패, 디스패치 예외 중 하나로 끝날 때 그 span을 닫습니다. 준비된 연산은 래퍼와 외부 트레이서 참조가 수거된 뒤에도 Python 트레이서 어댑터를 유지합니다. 블로킹 `invoke`/`dispatch`는 GIL을 놓고, OTel 호출과 Python 참조 소멸은 GIL을 잡습니다. 트레이서에서 실패해도 네이티브의 최선 노력(best-effort) 정책에 따라 처리되며, 소유 결과, 원래의 이벤트, 제품 예외는 대체되지 않습니다.

어댑터는 디스패치 시점에 Python 트레이서의 `start_span`을 호출합니다. 호출자의 컨텍스트는 네이티브 워커 스레드로 자동 전달되지 않습니다. 아래 예제처럼 디스패치 전에 쓰려는 OTel 부모 컨텍스트를 캡처해 트레이서 어댑터로 명시적으로 넘겨야 합니다. 그래프 노드의 provider가 그래프/노드 부모를 필요로 할 때도 마찬가지입니다. OTel 문서의 [명시적 부모 컨텍스트 선택](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span)을 참고하세요.

예제의 `ParentContextTracer`는 캡처한 부모 컨텍스트를 하나만 유지합니다. 논리적 부모가 다르면 새 컨텍스트를 캡처해서 어댑터와 provider 래퍼를 새로 만들어야 합니다. 이전 어댑터를 재사용하면 이전 부모가 그대로 쓰입니다.

위 provider 예제의 `provider`와 `controls`를 사용합니다. 이 코드 조각을 실행하기 전에 요청 한 건만 받는 peer를 다시 시작합니다.

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

콘솔 exporter에는 `openinference.span.kind="LLM"`인 `llm.request` span이 나타나고, 그 `parent_id`가 `request` span의 `span_id`와 같아야 합니다. 링크한 peer를 쓰면 출력 결과에서 핸들은 소비되었고 실패는 없으며 `Hello.`가 들어 있습니다. 새 요청에는 `observed.invoke(request)`로 같은 준비와 디스패치를 한 번에 할 수 있습니다. 이미 소비된 핸들을 다시 디스패치해서는 안 됩니다.

LLM span 필드에는 승인(admission)된 모델, 선언한 temperature/출력 상한, 공개 메시지의 역할과 보이는 `Text` 내용, 알려진 입력/출력/합계 사용량이 들어갑니다. 카운트가 없으면 그 속성을 생략하고, 보고된 0은 0으로 남습니다. 스트리밍 요청은 보이는 텍스트 content의 델타에 대해서만 `llm.token` 이벤트를 추가하며, 텍스트는 `attributes["chunk"]`에 들어 있습니다. SDK 실패는 안전한 메시지로 ERROR 상태를 설정하고 부분 공개 출력을 유지하며, 성공한 완료는 OK를 설정합니다. `request.on_event`는 여전히 원래의 타입 이벤트를 받습니다.

네이티브 재생(replay), reasoning 파트, raw envelope, raw 이벤트는 이 LLM span 필드에 들어가지 않습니다. 결과(outcome)에는 네이티브 메시지와 증거가 그대로 남아 있으며, 트레이싱으로 내보낸 데이터는 재생(replay) 보관(custody)이나 회계 권한을 부여하지 않습니다. 공개 텍스트에도 민감한 애플리케이션 데이터가 들어 있을 수 있으므로 프롬프트와 exporter를 그에 맞게 선택해야 합니다. `openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)`는 그래프 이벤트용 별도 컨텍스트 매니저로 계속 남아 있으며 provider의 LLM span을 대체하지 않습니다.

## 선택적 컴포넌트

공개 패키지는 선택적 C++ 컴포넌트가 포함되었는지를 사실대로 표시합니다.

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A` 플래그
- 포함되지 않은 컴포넌트는 Python으로 흉내 내지 않고 아예 노출하지 않음
- PyPI wheel은 Program/QuickJS, LLM, MCP, SQLite를 켜 두며, 소스 빌드는 CMake 옵션을 따름

## 테스트와 예제

바인딩 테스트 스위트는 Core 실행, 사용자 정의 콜백, asyncio, 취소, Program 컴파일/런타임, 필수 Hook, strict 컨텍스트, SQLite 영속성, 프로토콜, README 예제를 다룹니다.

- [Python 예제](../bindings/python/examples/README.md)
- [C++ 예제](../examples/README.md)
- [QuickJS 작성 경계](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [엄격한 런타임 인터포지션](STRICT_RUNTIME_INTERPOSITION.md)
