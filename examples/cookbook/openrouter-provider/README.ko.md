# NeoGraph + OpenRouter (고정 DeepSeek)

**언어:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph 그래프를 OpenRouter의 고정 모델
`deepseek/deepseek-v4-flash-0731`에 연결합니다. 동일한 API 키, 모델,
명시적인 zero-data-retention(ZDR) provider 선호도를 사용하는 두 가지
Provider 경로를 비교합니다.

1. 내장 `OpenAIProvider`와 OpenRouter의 OpenAI-compatible chat endpoint
   (`via_openai_compat.py`)
2. 정규화된 chat 요청을 직접 전송하는 사용자 정의 Python `Provider`
   (`via_http.py`)

두 번째 경로는 NeoGraph `Provider` trampoline이 transport와 무관하다는
점을 wire contract를 명시한 채 보여줍니다.

## 경로 A — 내장 Provider

`OpenAIProvider`가 `/v1/chat/completions`를 자동으로 붙이므로 `/v1`을
포함하지 않은 OpenRouter 기본 URL을 전달합니다.

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="deepseek/deepseek-v4-flash-0731",
    provider_routing={"zdr": True},
```

자세한 코드는 [`via_openai_compat.py`](via_openai_compat.py)를 보세요.

## 경로 B — 사용자 정의 직접 HTTP Provider

[`via_http.py`](via_http.py)는 `Provider`를 상속해
`https://openrouter.ai/api/v1/chat/completions`로 요청하고,
`choices[0].message`와 token usage를 NeoGraph 응답으로 변환합니다.
요청에는 OpenRouter ZDR provider 선호도도 포함됩니다.

## 실행

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

두 데모는 엄격한 단일 노드 그래프를 만들고 같은 고정 DeepSeek 모델로
`llm_call`을 실행합니다. system prompt는 공유
`NodeContext.instructions`에서 옵니다. 키가 없으면 명확한 오류로
종료하며 live 경로가 조용히 mock으로 바뀌지 않습니다.

## 출력 형태

```text
[openrouter] using deepseek/deepseek-v4-flash-0731 via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

실제 응답은 달라질 수 있습니다. 직접 HTTP 경로는 `via direct HTTP`를
출력하고 별도의 산술 질문을 사용합니다.

## 두 경로를 유지하는 이유

- 경로 A는 NeoGraph native HTTP 구현과 connection pool을 사용합니다.
- 경로 B는 애플리케이션이 custom header, transport 정책, 응답 변환을
  소유해야 할 때 가장 작은 예제입니다.
- 두 경로가 동일한 OpenRouter endpoint 계열, API 키, 고정 DeepSeek
  모델을 사용하므로 Provider 표면 비교가 공정합니다.

OpenRouter API 요청/응답 형식:
<https://openrouter.ai/docs/api-reference/overview>
