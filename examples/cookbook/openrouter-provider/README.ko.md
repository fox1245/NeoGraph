<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=ko source_sha256=4d18b0fee54089948ca59063eb6177567e1b5db09a87123125e808bee6889add -->
# NeoGraph + OpenRouter(고정 DeepSeek)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

OpenRouter의 고정 `~deepseek/deepseek-v4-flash-latest` 모델에 대해 NeoGraph 그래프를 실행합니다. 이 쿡북은 동일한 API 키, 모델 및 명시적 zero-data-retention(ZDR) 제공업체 선호를 사용하는 동등한 두 개의 공급자 표면을 보여줍니다:

1. 내장된 `OpenAIProvider`를 OpenRouter의 OpenAI 호환 채팅 엔드포인트(`via_openai_compat.py`)에 대해 사용합니다.
2. 정규화된 채팅 요청을 직접 게시하는 사용자 지정 Python `Provider`(`via_http.py`).


두 번째 경로는 NeoGraph의 `Provider` 트램펄린이 전송(transport)에 무관(agnostic)함을 보여주면서도 wire contract를 명시적으로 유지함을 나타냅니다.

## 경로 A — 내장 공급자

`OpenAIProvider` 추가( `/v1/chat/completions` 자체가 추가되므로, 순수 OpenRouter API 기본 URL(`https://openrouter.ai/api`)을 전달하고 `/v1` 접미사는 전달하지 마십시오:

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="~deepseek/deepseek-v4-flash-latest",
    provider_routing={"zdr": True},
```

[`via_openai_compat.py`](via_openai_compat.py) 참조.

## 경로 B — 사용자 지정 직접 HTTP 공급자

[`via_http.py`](via_http.py)는 `Provider`를 서브클래싱하고, `https://openrouter.ai/api/v1/chat/completions`에 게시하며, `choices[0].message`를 `ChatCompletion`로 매핑하고, 토큰 사용량을 ⟪NeoGraph⟫의 응답에 복사합니다. 또한 요청은 OpenRouter에 ZDR 공급자 기본 설정을 요청합니다.

## 실행 ⟦3b22460e100a⟧ 출력:

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

두 데모 모두 엄격한 단일 노드 그래프를 구축하고 해당 `llm_call`를 동일한 고정 DeepSeek 모델을 통해 라우팅합니다. 공유 `NodeContext.instructions`는 시스템 프롬프트를 제공합니다. 키가 없으면 명확한 메시지와 함께 종료됩니다. 목 공급자는 라이브 공급자 경로를 조용히 변경하지 않습니다.

## 출력 형태

```text
[openrouter] using ~deepseek/deepseek-v4-flash-latest via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

실제 완료 결과는 다릅니다. 직접 HTTP 경로는 `via direct HTTP`를 출력하며 자체 산술 질문을 사용합니다.

## 왜 두 경로를 모두 유지하나요?

- 경로 A는 NeoGraph의 네이티브 HTTP 구현과 연결 풀링을 사용합니다.
- 경로 B는 애플리케이션 코드가 소유한 사용자 지정 헤더, 전송 정책 또는 응답 변환을 위한 가장 작은 예시입니다.
- 두 경로 모두 정확히 동일한 OpenRouter 엔드포인트 계열, API 키, 그리고 고정된 DeepSeek 모델을 사용하므로 제공업체 표면 비교가 의미 있습니다.

OpenRouter API 요청 및 응답 형태:
<https://openrouter.ai/docs/api-reference/overview>
