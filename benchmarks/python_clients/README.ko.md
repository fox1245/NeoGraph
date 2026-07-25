<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=ko source_sha256=6d8012ba0393efa7a76c1905fcab2a826c43a0cd2d8287940fcb28ec62dc25b4 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# Python 클라이언트 오버헤드 — NeoGraph 바인딩과 표준 SDK


`neograph_engine`(pybind11을 통한 C++ 엔진)를 통한 동일한 워크로드
공식 Python SDK를 통해. C++ 엔진 벤치는 다음과 같습니다.
엔진 오버헤드에서 100×-600× 승리([`benchmarks/`](../README.md) 참조)
이 폴더는 더 좁은 질문에 답합니다: 사용자가 앉아 있을 때
Python, 그 승리 중 어떤 것이 구속력 있는 경계에서 살아남나요?

방법론: 처리 중인 Python 모의 서버가 미리 준비된 결과를 반환합니다.
1ms 미만의 응답이므로 서버 측 시간은 균일한 상수입니다. 그만큼
delta는 순전히 클라이언트 측입니다. — JSON 빌드, HTTP, 구문 분석입니다.

## 순차적 오버헤드(K=1)

`bench_a2a_clients.py`:

|고객|중앙값|p95|처리량|
|---|---:|---:|---:|
|`neograph_engine.a2a.A2AClient`|**1,137μs**|1,381μs|**860 req/s**|
|`a2a-sdk` 1.0.2|2,196μs|2,746μs|444 req/s|

→ NeoGraph **1.93×** 더 빨라졌습니다.

`bench_openai_clients.py`:

|고객|중앙값|p95|처리량|
|---|---:|---:|---:|
|`neograph_engine.llm.OpenAIProvider`|**1,252μs**|1,423μs|**789 req/s**|
|`openai` 2.33|1,927μs|2,393μs|509 req/s|

→ NeoGraph **1.54×** 더 빨라졌습니다.

따라서 결합된 OpenAI-call-inside-A2A 왕복은 다음과 같습니다.
**~3배 더 빠릅니다** NeoGraph 바인딩을 통해 바인딩할 때보다
SDK 스택 — 각 레이어가 독립적이기 때문에 승리는 복합적입니다.

## 동시 처리량

`bench_concurrent.py`, K = 1/4/16/64 진행 중 요청, 총 500개:

|케이|네오그래프 req/s|a2a-sdk req/s|속도 향상|
|----:|---------------:|--------------:|--------:|
|   1 |            881 |           448 |  1.97×  |
|   4 |        **1,461** |           446 |  **3.28×**  |
|  16 |            403 |           390 |  1.03×  |
|  64 |            343 |           275 |  1.25×  |

K=4 행이 가장 깔끔하게 읽혀졌습니다. NeoGraph의 pybind11 래퍼 릴리스
C++ HTTP 교환 중에 GIL가 발생하므로 `ThreadPoolExecutor`가 확장됩니다.
거의 선형적으로요. `a2a-sdk`는 비동기식이며 하나의 이벤트 루프를 사용합니다.
진행 중인 요청을 더 추가해도 요청당 도움이 되지 않습니다.
직렬화 비용은 Python 내에서 지불됩니다.

(K=16+ 모의 서버의 `http.server.ThreadingHTTPServer` 때문에 둘 다 삭제됩니다.
~400 r/s 동시 스레드로 포화됩니다. 이는 Python stdlib입니다.
제한 사항은 클라이언트 속성이 아닙니다. NeoGraph A2A C++ 서버
엔진당 땀 한 방울 흘리지 않고 200개의 동시 실행을 처리합니다.
벤치이지만 위의 클라이언트 측 숫자는 그 자체로 나타납니다.)

## 이것이 의미하는 바

- **Sub-μs 엔진 승리는 바인딩 경계에서 유지되지 않습니다**.
**2-3× 클라이언트 승리가 발생합니다.** 요청당 오버헤드는 GIL 릴리스입니다.
HTTP 배관 및 JSON 구문 분석 — 모두 기본 클라이언트
httpx + pydantic보다 빠릅니다.
- 실제 LLM 워크로드(호출당 300ms 이상)의 경우 클라이언트 오버헤드는 다음과 같습니다.
소음 속에서 — 그러나 높은 RPS에서 빠른 끝점까지(모의 테스트,
내부 서비스, 에이전트 팬아웃, 멀티샷 라우팅)
벽 시간을 지배합니다.
- **동시 스레드 규모.** `send_message`의 GIL 릴리스 /
`complete()`를 사용하면 실제 ThreadPoolExecutor를 실행하지 않고도 실행할 수 있습니다.
Python의 병렬성 벽에 부딪혔습니다. 클라이언트가 한 명일 때 유용합니다.
N개의 에이전트로 팬아웃됩니다.

## 낳다

```bash
pip install neograph-engine==0.2.2 a2a-sdk openai httpx
python bench_a2a_clients.py 500       # sequential A2A
python bench_openai_clients.py 500    # sequential OpenAI
python bench_concurrent.py 500        # concurrent A2A
```

위 수치는 x86_64 Ubuntu 24.04에서 2026년 4월 29일에 측정되었습니다.
(WSL2), Python 3.12.3, 이 프로세스 내 모의 서버에 대해
접는 사람. 결과는 ±5% 이내에서 재현 가능합니다.
