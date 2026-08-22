<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ko source_sha256=812a1f340ed6b8f92ddd742cc1c8f239265b501fa010c81929825f0973738e38 -->
# 나만의 OpenAI 클라이언트 가져오기

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

대부분의 프로덕션 Python 사용자는 이미 자체 재시도, 사용자 지정 전송, 관찰성 Hook 또는 OpenRouter 라우팅을 갖춘 `openai.OpenAI()` 클라이언트 인스턴스를 보유하고 있습니다. 이 cookbook은 기존 클라이언트를 사용자 지정 `Provider` 으로 NeoGraph에 연결하는 방법을 보여줍니다 — NeoGraph의 기본 제공 `OpenAIProvider`.

핵심: NeoGraph의 `Provider`은 v0.2.3 이상에서 Python 하위 클래스화가 가능합니다. 하위 클래스의 `complete(params)`는 내장 공급자와 마찬가지로 그래프 노드(LLMCallNode, ReAct 루프 등) 내부에서 실행됩니다.

## 어떤 것을 언제 사용할지

| 원하는 것 | 사용하세요 |
|---|---|
| "OpenRouter를 통해 고정된 DeepSeek 라우트만 사용" | `OpenAISdkProvider` 공식 `openai` SDK와 함께 |
| "재시도 / Azure / 프록시 / 훅이 설정된 `openai.OpenAI()`를 이미 보유함" | 이 쿡북(`Provider` 하위클래스화, 사용자 클라이언트에 위임) |
| "공식 `openai` SDK를 통해 OpenRouter API를 사용 중" | 고정된 DeepSeek 모델이 포함된 이 쿡북 |
| "테스트에서 LLM을 목킹하고 싶음" | 결정적 스텁이 포함된 이 쿡북 |

핵심: **NeoGraph의 그래프 엔진은 LLM 호출이 어떻게 이루어지는지 신경 쓰지 않습니다** — `params -> ChatCompletion`만 있으면 됩니다.

## 60줄로 된 전체 코드

[`hybrid.py`](hybrid.py)를 참조하세요. 핵심 형태:

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "~deepseek/deepseek-v4-flash-latest"):
        super().__init__()
        self.client = client
        self.model  = model

    def complete(self, params: ng.CompletionParams) -> ng.ChatCompletion:
        # Translate NeoGraph params into the SDK's chat-completions shape.
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        resp = self.client.chat.completions.create(
            model=params.model or self.model,
            messages=messages,
            temperature=params.temperature,
        )
        # Translate back into NeoGraph's response shape.
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = resp.choices[0].message.content or ""
        return out

    def get_name(self) -> str:
        return "openai-sdk"
```

이것이 전부입니다. `OpenAISdkProvider(OpenAI(api_key=...))`를 `NodeContext`에 전달하면 `llm_call` 노드를 사용하는 모든 NeoGraph 그래프가 SDK를 통해 라우팅됩니다 — SDK 클라이언트에 연결된 모든 재시도 / Azure / 관측성 / 프록시 구성이 유지됩니다.

## 실행 ⟦3b22460e100a⟧ 출력:

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
python hybrid.py
```

출력:
```
[hybrid] using openai SDK inside NeoGraph 0.2.3 graph
[hybrid] running one llm_call through the OpenAI SDK provider
[provider] complete() call #1 (2 msgs) — model=~deepseek/deepseek-v4-flash-latest
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

기본 `llm_call`는 공유 `NodeContext.instructions`를 시스템 프롬프트로 사용합니다. 그래프 단계마다 다른 프롬프트가 필요한 경우 사용자 지정 노드 유형을 사용하십시오.

## 유지하는 것

- `openai.OpenAI()` 클라이언트의 `default_headers`, 재시도 정책, 커스텀 `http_client=httpx.Client(...)`, Azure / proxy 구성.
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` / `weights & biases` 통합은 SDK 수준에서 연결됩니다 — 모든 호출을 가로챕니다.
- 기존의 `usage`(토큰 수) 추적, 오류, 재시도.

## `neograph_engine.llm.OpenAIProvider` 대비 포기하는 것

- 네이티브 HTTP 경로(asio + 연결 풀) — SDK보다 약 1.5배 빠르며 GIL 경합이 전혀 없음. 병목이 OpenAI 호출이라면 SDK로 충분하고, 프레임워크 오버헤드가 문제라면 네이티브 경로가 우세합니다.

## 도구 호출 — 세 가지 작동 방식

Provider 트램펄린을 통해 `complete()`가 `tool_calls`를 깨끗하게 반환할 수 있습니다. 현재 **작동하지 않는** 부분은 C++ `tool_dispatch` 그래프 노드가 Python `Tool` 하위 클래스를 다시 호출하는 경로입니다. 해당 경로는 세그폴트가 발생합니다(기존 문제, v0.3에서 추적 중). 오늘 작동하는 세 가지 패턴은 다음과 같습니다.

### A. 에이전트형 Provider(`byo-openai`에 권장)

툴 루프를 **내부에서** 수행하세요 `complete()`. 사용자의 `openai.OpenAI` 클라이언트는 이미 tool-calling을 지원합니다; 에이전틱 루프(call → dispatch in Python → result → call → text)를 완료하고 NeoGraph에 최종 어시스턴트 메시지만 반환하게 하세요. 그래프는 "turn"당 정확히 하나의 `complete()`를 보며, `tool_dispatch` 노드가 필요하지 않습니다.

```python
class AgenticOpenAIProvider(ng.Provider):
    def __init__(self, client, tools_by_name):
        super().__init__()
        self.client = client
        self.tools  = tools_by_name      # {"calc": calc_fn, ...}
    def complete(self, params):
        messages = [{"role": m.role, "content": m.content} for m in params.messages]
        sdk_tools = [{"type":"function",
                      "function":{"name":n,"description":fn.__doc__ or "",
                                  "parameters":fn.schema}}
                     for n, fn in self.tools.items()]
        for _ in range(10):  # cap loops
            r = self.client.chat.completions.create(
                model=params.model or "~deepseek/deepseek-v4-flash-latest",
                messages=messages, tools=sdk_tools)
            choice = r.choices[0]
            if not choice.message.tool_calls:
                out = ng.ChatCompletion()
                out.message.role    = "assistant"
                out.message.content = choice.message.content or ""
                return out
            messages.append(choice.message.model_dump())
            for tc in choice.message.tool_calls:
                fn = self.tools[tc.function.name]
                result = fn(**stdjson.loads(tc.function.arguments))
                messages.append({"role":"tool","tool_call_id":tc.id,
                                 "content":str(result)})
```

절충: NeoGraph는 중간 단계를 보지 못하지만(도구 호출별 체크포인트가 없음), 모든 SDK 동작을 유지하고 디스패치 경계 마찰이 없습니다.

### B. C++ 도구 + Python Provider

디스패치 경로에는 내장 C++ 도구(`MCPTool`에서 가져온 `neograph_engine.mcp`, 또는 다른 C++ 측 `Tool`)를 사용하고, LLM 호출에는 Python Provider를 사용한다. 그래프의 `tool_dispatch` 노드는 C++ 도구를 정상적으로 호출할 수 있지만, 이후 Python `Tool` 하위 클래스로의 콜백만 충돌이 난다.

### C. Provider가 tool_calls를 반환합니다. 커스텀 Python 노드가 디스패치합니다.

내장된 `tool_dispatch` 노드를 건너뜁니다. 직접 작성한 `@ng.node("dispatch")` 를 사용하여 `messages[-1].tool_calls`를 읽고, Python 도구를 직접 호출하며, 도구 결과 메시지를 다시 작성합니다. 전적으로 Python으로 유지됩니다.

## A2A + 커스텀 Provider

이 쿡북은 [ai-assembly cookbook](../ai-assembly/)과 자연스럽게 구성됩니다. 각 구성원의 provider를 `OpenAISdkProvider(...)`로 교체하면 NeoGraph의 A2A 브리지를 사용하면서 모든 페르소나에 대해 SDK 수준의 동작을 모두 얻을 수 있습니다.
