<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ko source_sha256=837a72c1600b8f89e2a5600d0ea31ad8fb44f043e2e9c11d49ff18551164f306 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# 나만의 OpenAI 클라이언트 가져오기


대부분의 프로덕션 Python 사용자는 이미 `openai.OpenAI()` 클라이언트를 보유하고 있습니다.
자체 재시도, 사용자 지정 전송, 관찰 가능성 후크가 있는 인스턴스
OpenRouter 라우팅. 이 요리책은 연결 방법을 보여줍니다.
기존 클라이언트를 사용자 정의 `Provider`로 NeoGraph에 넣습니다.
NeoGraph의 내장 `OpenAIProvider`.

비결: NeoGraph의 `Provider`는 v0.2.3+에서 Python 하위 클래스화 가능합니다.
서브클래스의 `complete(params)`는 그래프 노드(LLMCallNode,
ReAct 루프 등)은 내장 공급자와 같습니다.

## 언제 어느 것을 사용할지

|당신이 원하는|사용|
|---|---|
|"고정 DeepSeek 경로를 OpenRouter로 사용하고 싶습니다"|공식 `openai` SDK를 사용하는 `OpenAISdkProvider`|
|"재시도/Azure/프록시/후크가 포함된 `openai.OpenAI()`가 이미 설정되어 있습니다."|이 요리책(하위 클래스 `Provider`, 클라이언트에 위임)|
|"공식 `openai` SDK를 통해 OpenRouter API를 사용하고 있습니다"|고정 DeepSeek 모델과 함께 이 요리책|
|"테스트에서 LLM를 조롱하고 싶습니다"|결정론적 스텁이 포함된 이 요리책|

요점: **NeoGraph의 그래프 엔진은 LLM 호출 방식에 신경 쓰지 않습니다.
발생** — `params -> ChatCompletion`만 필요합니다.

## 60줄 안에 모든 내용이 담겨있습니다.

[`hybrid.py`](hybrid.py)를 참조하세요. 주요 모양:

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "deepseek/deepseek-v4-flash-0731"):
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

그게 다야. `OpenAISdkProvider(OpenAI(api_key=...))`를
`NodeContext` 및 `llm_call` 노드를 사용하는 모든 NeoGraph 그래프는 라우팅됩니다.
SDK를 통해 — 모든 재시도/Azure/관찰 가능성/프록시 유지
SDK 클라이언트에 연결된 구성입니다.

## 달리다

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
python hybrid.py
```

산출:
```
[hybrid] using openai SDK inside NeoGraph 0.2.3 graph
[hybrid] running one llm_call through the OpenAI SDK provider
[provider] complete() call #1 (2 msgs) — model=deepseek/deepseek-v4-flash-0731
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

내장된 `llm_call`는 공유 `NodeContext.instructions`를
시스템 프롬프트. 다양한 그래프 단계가 필요한 경우 사용자 정의 노드 유형을 사용하십시오.
다른 프롬프트.

## 당신이 지키는 것

- `openai.OpenAI()` 클라이언트의 `default_headers`, 재시도 정책,
사용자 정의 `http_client=httpx.Client(...)`, Azure/프록시 구성.
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` /
SDK 수준에 연결된 `weights & biases` 통합 —
모든 통화를 차단합니다.
- `usage`(토큰 수), 오류, 재시도에 대한 기존 추적입니다.

## 포기하는 것 vs `neograph_engine.llm.OpenAIProvider`

- 기본 HTTP 경로(asio + 연결 풀) — 보다 ~1.5배 빠릅니다.
SDK 및 GIL 경합이 없습니다. 병목 현상이 OpenAI 호출인 경우
SDK는 괜찮습니다. 프레임워크 오버헤드인 경우 기본 프레임워크가 승리합니다.

## 도구 호출 — 세 가지 작업 패턴

공급자 트램펄린을 사용하면 `complete()`가 `tool_calls`를 깔끔하게 반환할 수 있습니다.
현재 **작동하지 않는** 것은 C++ `tool_dispatch` 그래프 노드입니다.
Python `Tool` 하위 클래스로 다시 호출 — 해당 경로 segfault
(기존 문제, v0.3에서 추적됨) 오늘날에는 세 가지 패턴이 작동합니다.

### A. 에이전트 공급자(`byo-openai`에 권장)

`complete()` **내부**에서 도구 루프를 수행합니다. 사용자의 `openai.OpenAI`
클라이언트는 이미 도구 호출을 지원합니다. 에이전트 루프를 끝내도록 하세요.
(Python에서는 호출 → 디스패치 → 결과 → 호출 → 텍스트) 및 반환만
NeoGraph에 보내는 마지막 보조 메시지입니다. 그래프에는 정확히 하나가 표시됩니다.
"턴"당 `complete()`, `tool_dispatch` 노드가 필요하지 않습니다.

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
                model=params.model or "deepseek/deepseek-v4-flash-0731",
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

트레이드오프: NeoGraph는 중간 단계를 볼 수 없습니다.
도구 호출), 모든 SDK 동작을 유지하고 디스패치가 없습니다.
경계 마찰.

### B. C++ 도구 + Python 공급자

내장된 C++ 도구(`neograph_engine.mcp`의 `MCPTool`,
또는 디스패치 경로에 대한 다른 C++ 측 `Tool`) 및 Python
LLM 호출에 대한 공급자입니다. 그래프의 `tool_dispatch` 노드 호출
C++ 도구는 괜찮습니다. Python `Tool` 하위 클래스로 다시 호출하는 경우에만
충돌.

### C. 공급자는 tool_calls를 반환합니다. 사용자 정의 Python 노드 디스패치

내장된 `tool_dispatch` 노드를 건너뜁니다. 나만의 글쓰기
`messages[-1].tool_calls`를 읽고 호출하는 `@ng.node("dispatch")`
Python 도구를 직접 사용하고 도구 결과 메시지를 작성합니다.
뒤쪽에. 완전히 Python으로 유지됩니다.

## A2A + 맞춤 공급자

이 요리책은
[ai-assembly cookbook](../ai-assembly/) — 각 멤버의 교체
모든 SDK 수준을 얻으려면 `OpenAISdkProvider(...)`를 사용하는 공급자
NeoGraph의 A2A 브리지를 사용하는 동안 모든 페르소나의 동작.
