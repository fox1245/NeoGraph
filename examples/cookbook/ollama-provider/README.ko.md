<!-- neograph-i18n: source=examples/cookbook/ollama-provider/README.md locale=ko source_sha256=e9208ffec69b9c3d4b86998e648ea60f8b2f9c217008241cd11b71af7c346bed -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph + Ollama(로컬 LLM)


로컬에서 제공되는 모델에 대해 NeoGraph 그래프를 실행하세요.
[Ollama](https://ollama.com) — API 키 없음, 네트워크 송신 없음, 전체
은둔. 두 가지 경로, 둘 다 NeoGraph 0.2.3+에서 작동:

## 경로 A — Ollama의 호환 엔드포인트에 대한 `OpenAIProvider` *(새 코드 없음)*

Ollama는 OpenAI request/response를 사용하여 `/v1/chat/completions`를 노출합니다.
모양. NeoGraph에 내장된 `OpenAIProvider`를 가리키면
done — 동일한 기본 asio HTTP 경로, 동일한 연결 풀, 동일한 속도.

```python
from neograph_engine.llm import OpenAIProvider
# OpenAIProvider appends `/v1/chat/completions` itself, so base_url
# is the bare host — NOT host + "/v1". Ollama returns 404 otherwise.
provider = OpenAIProvider(
    api_key="sk-anything",                       # required field, value ignored
    base_url="http://127.0.0.1:11434",            # NOT ".../v1"
    default_model="qwen2.5:0.5b",
)
```

이것이 전체 통합입니다. [`via_openai_compat.py`](via_openai_compat.py)를 참조하세요.

**경로 A를 선택하는 경우**: 속도를 원하고 Ollama는 신경 쓰지 않습니다.
특정 기능(모델 pull/load, 기본 필드로 스트리밍,
`keep_alive` 및 `options` 블록). 사용자의 95%.

## 경로 B — 사용자 정의 Python `Provider`를 통한 네이티브 Ollama API

v0.2.3 `Provider` 트램폴린이 정품임을 입증합니다.
공급업체에 구애받지 않습니다. Ollama의 기본 `/api/chat`를 래핑합니다(더 풍부한 매개변수,
호환 계층 번역 없음), 다음으로 배송
[`via_native_api.py`](via_native_api.py).

```python
class OllamaProvider(ng.Provider):
    def __init__(self, model="qwen2.5:0.5b", host="http://127.0.0.1:11434"):
        super().__init__()
        self.model = model
        self.host = host
    def complete(self, params):
        r = httpx.post(f"{self.host}/api/chat", json={
            "model": params.model or self.model,
            "messages": [{"role": m.role, "content": m.content}
                         for m in params.messages],
            "stream": False,
            "options": {"temperature": params.temperature},
        }, timeout=120)
        r.raise_for_status()
        body = r.json()
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = body["message"]["content"]
        return out
    def get_name(self): return "ollama"
```

**경로 B를 선택하는 경우**: Ollama 기본 필드(`options`,
`keep_alive`, `format=json` 제약 조건) 또는 팀에서 이미
`ollama` Python SDK를 클라이언트 표면으로 유지하려고 합니다.

## 달리다

```bash
# 1. start Ollama (separate terminal)
ollama serve

# 2. pull a small model so it fits in CI / laptop RAM
ollama pull qwen2.5:0.5b      # ~400 MB

# 3. install NeoGraph + httpx (Path B uses httpx)
pip install neograph-engine>=0.2.3 httpx

# 4. run either path
python via_openai_compat.py
python via_native_api.py
```

두 데모 모두 엄격한 단일 노드 그래프를 구축하고 `llm_call`를 통해 라우팅합니다.
현지 모델. 시스템 프롬프트는 공유에서 나옵니다.
`NodeContext.instructions`. 외부 API 키가 필요하지 않습니다.

## 산출

```
[ollama] using qwen2.5:0.5b at http://127.0.0.1:11434 (OpenAI-compat path)
[user] What's the capital of France?
       user: What's the capital of France?
  assistant: Paris is the capital of France.
```

(실제 완료는 모델에 따라 다릅니다. 기본 경로가 인쇄됩니다.
`(native /api/chat path)` 및 자체 샘플 질문을 사용합니다.)

## 메모

- **왜 `qwen2.5:0.5b`인가?** 처리할 수 있는 가장 작은 주류 모델
영어 + 간단한 추론. 콜드 스타트 ​​시 2~3초 안에 로드된 후
CPU에서 ~100ms/완료. 요리책 데모에 적합합니다. 다음으로 교환
`llama3.2:3b` / `qwen2.5:7b` / `phi4:14b` 일단 확인하면
루프가 작동합니다.
- **첫 번째 호출이 느립니다** — Ollama가 모델을 메모리에 느리게 로드합니다.
첫 번째 요청 시(하위 1B의 경우 ~2~5초) 후속 통화는 따뜻합니다.
`keep_alive` 매개변수(경로 B)는 모델이 유지되는 기간을 제어합니다.
유휴 후에도 로드된 상태를 유지합니다.
- **도구 호출**: Ollama의 도구 호출 지원은 모델에 따라 다릅니다.
`/v1/chat/completions`를 통해 OpenAI 호환 형태를 사용합니다.
끝점. 경로 A + 에이전트 제공자 패턴을 사용하십시오.
[`../byo-openai/hybrid_with_tools.py`](../byo-openai/hybrid_with_tools.py)
도구를 사용할 수 있는 Ollama 모델(예: `qwen2.5:7b`)을 사용합니다.
- **스트리밍**: 경로 A는 다음을 통해 NeoGraph의 기본 스트리밍을 상속합니다.
`OpenAIProvider.complete_stream`. 경로 B의 예는 스트리밍되지 않습니다.
필요한 경우 `"stream": True` + 청크 루프를 추가하세요.

## 이것이 중요한 이유

NeoGraph(5 µs 엔진 오버헤드)와 로컬 Ollama 모델 결합
**외부 종속성이 전혀 없는** 전체 에이전트 스택을 제공합니다.

- 13MB 기본 바이너리 + ~400MB 모델 = **Raspberry Pi 5에 적합**
- API 키 없음, 속도 제한 없음, 네트워크 송신 없음, 전체 데이터 개인 정보 보호
- 그래프 의미론의 LangGraph 패리티; Ollama 품질의 모델을 위한
LLM 통화
- 엔진 레이어의 반복당 오버헤드는 노이즈에 있습니다(
500~2000ms 모델 추론이 지배적이지만 프레임워크 공간은
LangGraph + Python 런타임보다 의미 있게 작음

적합 대상: 엣지 AI, 온디바이스 에이전트, 개인 정보 보호가 필요한 배포,
자체 호스팅 어시스턴트, 초기 OpenAI 청구서에 지친 사람
프로토타이핑.
