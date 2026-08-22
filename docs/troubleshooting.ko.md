<!-- neograph-i18n: source=docs/troubleshooting.md locale=ko source_sha256=ac341ae5a04c54a36f6e1d4e165be17f5cf789b924e015626c3a01c9c3a447b9 -->
# 문제 해결

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

증상이 먼저, 원인과 수정 방법은 그 다음입니다. 여기에 없는 문제를 만나면 증상을 포함한 이슈를 열어 주시기 바랍니다. 이후 이 목록에 추가될 가능성이 높습니다.

> **5초 점검.** 다른 무엇보다 먼저, 다음을 확인하세요.
> 최신 패치 버전을 사용 중인지:
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
> 아래 대부분의 문제는 특정 릴리스에서 수정되었습니다. 먼저 업그레이드하고,
> 그다음에 디버깅하세요.

---

## 설치 / 가져오기

### `pip install neograph-engine`는 성공하지만 `import`는 실패합니다

Python 버전 / 플랫폼 불일치일 가능성이 높습니다. 다음을 위한 휠을 제공합니다:

| 플랫폼 | 버전 |
|---|---|
| Linux x86_64 (manylinux_2_34) | Python 3.9 – 3.13 |
| Linux aarch64 (manylinux_2_34) | Python 3.9 – 3.13 |
| macOS arm64 (14 이상) | Python 3.9 – 3.13 |
| Windows x64 (MSVC) | Python 3.9 – 3.13 |

이 매트릭스에 속하지 않는 모든 것은 sdist(소스 빌드)로 처리되며, 여기에는 CMake 3.16+, OpenSSL, C++20 툴체인이 필요합니다. 플랫폼이 목록에 없고 소스 빌드가 실패하면 이슈를 열어 주세요.

### Linux에서의 `ImportError: ... GLIBC_2.32 not found`

Linux 휠은 `manylinux_2_34`입니다 — glibc ≥ 2.34 (Ubuntu 22.04+, Debian 12+, RHEL 9+)가 필요합니다. 이전 배포판에서는 소스에서 빌드하세요.

### Windows에서의 `ImportError: DLL load failed`

Windows 휠은 자체 종속성을 포함하지만 Python 설치본은 휠 아키텍처(x64)와 일치해야 합니다. 다음으로 확인하세요:

```powershell
python -c "import platform; print(platform.architecture())"
```

`('32bit', ...)`가 출력되면 32비트 Python을 사용 중인 것입니다 — 64비트 버전을 설치하세요.

---

## TLS / 네트워크

### 공급자 호출이 60초 동안 멈춘 후 `ConnPool::async_post: timeout` 오류가 발생합니다

**영향:** `neograph-engine` 휠 v0.1.0 – v0.1.6.

**근본 원인:** 번들된 OpenSSL에 `/etc/pki/tls/...` (RHEL 규칙)을 가리키는 컴파일된 CA 저장소 경로가 있습니다. Ubuntu, Debian, macOS에서는 CA 저장소가 다른 곳(`/etc/ssl/certs/...`)에 있으므로, 휠의 libssl이 어떤 피어 인증서도 검증할 수 없고 TLS 핸드셰이크가 오류가 발생하기 전에 전체 요청 시간 초과를 조용히 대기합니다.

**수정 (≥ v0.1.7):** 휠의 `__init__.py`가 이제 가져오기 시 `SSL_CERT_FILE`를 `certifi.where()`에 자동으로 지정합니다. 업그레이드:

```bash
pip install --upgrade neograph-engine
```

**이전 휠에 대한 해결 방법:**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**v0.1.7+에서 자동 수정을 거부하려면** (예: 사용자 지정 CA 번들이 있는 경우): 가져오기 전에 `NEOGRAPH_SKIP_CERT_AUTOFIX=1`를 설정하세요.

### `urllib`는 작동하지만 NeoGraph는 작동하지 않습니다

위와 동일한 근본 원인 — `urllib`는 시스템 OpenSSL을 사용하는 반면, 휠은 잘못된 CA 경로가 있는 번들 OpenSSL을 사용합니다. 동일한 수정: ≥ v0.1.7로 업그레이드하거나 `SSL_CERT_FILE`를 설정하세요.

### WebSocket 응답(`use_websocket=True`)이 `close=1000`와 함께 즉시 종료됨

빈도순으로 세 가지 일반적인 원인:

1. **API 키/조직에서 WebSocket 액세스가 활성화되지 않음.** 일부 OpenAI 티어 1 계정은 아직 WebSocket 모드 액세스 권한이 없습니다. `use_websocket=False`를 설정하여 HTTP/SSE로 대체하십시오.
2. **특정 프록시 경로에 `User-Agent` 헤더 누락.** 커밋 `d7c61d0`에서 수정됨. ≥ v0.1.4로 업그레이드하십시오.
3. **일부 Responses-API 모델에서 `temperature` 필드가 거부됨.** 동일한 커밋에서 지원되는 모델의 WS 핸드셰이크에서 해당 필드를 제거합니다.

### 브라우저에서 WASM을 통해 실행할 때 발생하는 CORS 오류

WASM 빌드는 아직 브라우저-CORS용 우회 헤더를 구현하지 않습니다. 상태는 [WASM/CORS 이슈](https://github.com/fox1245/NeoGraph/issues)를 추적하십시오.

---

## 그래프 컴파일 / 실행

### `RuntimeError: Unknown reducer: <name>`

바인딩과 함께 두 개의 리듀서가 제공됩니다: `"overwrite"` 및 `"append"`. 다른 것은 등록하지 않는 한 컴파일되지 않습니다.

**사용자 정의 리듀서 등록(v0.1.9부터 Python에서):**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

기존 이름을 다시 등록하면 이전 리듀서가 대체됩니다. 호출 가능 객체는 GIL 하에서 실행되며, 동시 Send fan-out은 Python 사용자 정의 노드와 동일한 방식으로 이에 대해 직렬화됩니다.

`"last_value"`(일반적인 LangGraph 별칭)을 입력한 경우 — 여기서는 `"overwrite"`입니다. 동일한 의미, 다른 이름입니다.

### `RuntimeError: Unknown condition: <name>`

내장 조건: `has_tool_calls`, `route_channel`. 다른 이름은 등록해야 합니다.

**사용자 지정 조건 등록(Python에서, v0.1.9부터):**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

호출 가능 객체는 라이브 `GraphState`(`state.get(channel)` / `state.get_messages()` 사용 가능)를 수신하며 조건부 엣지의 `routes` 키 중 하나와 일치하는 문자열을 반환해야 합니다.

### `RuntimeError: Write to unknown channel: <name>`

`ChannelWrite`의 채널 이름이 `definition["channels"]`의 어떤 것과도 일치하지 않습니다. 채널 이름은 정확히 일치해야 합니다; `messages`와 `Messages`는 서로 다릅니다.

### `RuntimeError: Unknown node type: <name>`

노드 중 하나의 `type` 필드가 팩토리 레지스트리에 없는 것을 참조합니다. 내장 유형(`llm_call`, `tool_dispatch`, `intent_classifier`, `subgraph`)의 경우 유형 이름은 위에 명시되어 있습니다. 자체 유형의 경우 컴파일 전에 `ng.NodeFactory.register_type(type_name, factory)`를 호출해야 합니다.

### 내 ReAct 루프가 한 번만 실행됩니다 — `execution_trace == ['llm']`

**영향 범위:** `neograph-engine` 휠 v0.1.0 – v0.1.7.

**근본 원인:** 그래프 컴파일러가 최상위 `conditional_edges` 블록을 조용히 삭제했습니다. README 퀵스타트와 모든 Python 예제가 이 형식을 사용하므로 ReAct 루프가 단일 LLM 호출(도구 디스패치 없음)로 축소되었습니다.

**수정(≥ v0.1.8):** 컴파일러는 이제 두 형식을 모두 허용합니다 — 최상위 `conditional_edges` 배열 또는 `edges` 내 인라인에 `condition` 필드 포함. 업그레이드 후 다음으로 확인하십시오:

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**이전 wheel에 대한 우회책:** 조건부를 인라인으로 배치하세요:

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "llm",
     "condition": "has_tool_calls",
     "routes": {"true": "dispatch", "false": ng.END_NODE}},
]
# (no separate conditional_edges block)
```

### `result.execution_trace`가 비어 있거나 시작 노드만 표시됩니다

그래프가 즉시 `__end__`로 라우팅되었습니다. 가장 흔한 원인:

1. **`__start__`에서 나가는 엣지 누락.** 모든 그래프에는 최소한 하나의 `{"from": ng.START_NODE, "to": "..."}` 엣지가 필요합니다.
2. **조건식이 `routes` 맵에 없는 값을 반환함.** 조건식의 반환 값이 어떤 키와도 일치하지 않을 때, 열린 또는 불특정 조건은 명시적 `"default"` 경로를 사용합니다. 그것이 `__end__`에 매핑되면 정상적으로 종료됩니다. `"default"`가 없으면 라우팅은 소스 노드, 조건, 반환된 라벨을 포함하는 오류를 발생시킵니다. 닫힌 조건은 항상 선언된 집합 밖의 라벨을 거부합니다.
3. **`max_steps=0` 또는 `max_steps=1`** — 실행이 즉시 상한에 도달했습니다. 기본값은 25입니다. ReAct 루프는 일반적으로 10개 이상이 필요합니다.

### 컴파일 오류: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraph는 사이클을 허용하지만(ReAct 루프는 사이클입니다), 컴파일러는 *무조건적* 사이클 — 조건부 탈출이 없는 `a → b → a` — 을 잡아냅니다. `__end__`로 라우팅할 수 있는 조건부 엣지를 추가하세요.

---

## 성능

### Fan-out이 예상보다 느립니다.

일반적인 두 가지 원인:

1. **엔진 소유 워커 풀 없음.** `compile()`는 기본적으로 `set_worker_count(1)`입니다 — 풀이 없으면 fan-out 분기는 호출자의 실행기에서 인라인으로 디스패치되어 직렬로 실행됩니다. `compile()` 이후(그리고 `run()` 이전)에 한 번 풀을 선택하세요:

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

NeoGraph는 또한 풀 없이 다중-Send(또는 다중-나가는-엣지) fan-out이 처음 실행될 때 일회성 stderr 경고를 출력하므로, 조용한 직렬 사례가 보입니다. worker=1 빠른 경로가 의도적이라면 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`로 억제하세요.
2. **Python 사용자 정의 노드는 본문 동안 GIL을 보유합니다.** `@ng.node` 함수가 CPU 바운드 Python 작업을 수행하면 fan-out은 속도를 높이지 않습니다. ONNX / PyTorch / numpy / `requests.get`는 네이티브 호출 중 GIL을 해제하므로 병렬화됩니다. 순수 Python 점수 루프의 경우 워커 수를 얼마로 설정하든 상관없습니다.

### `bench_neograph par`가 200+ µs를 보고합니다

**v1.0 이전 휠.** v0.1.4–v0.x는 워커 풀 기본값을 `hardware_concurrency()`로 유지했으며, 이는 모든 fan-out 틱에서 크로스 스레드 제출 비용을 지불했습니다. v1.0은 기본값을 `set_worker_count(1)`(풀 없음, 제출 비용 없음)으로 되돌렸습니다 — `par`는 새 `compile()`에서 플립 이전의 대략적인 수준으로 돌아왔습니다. 워크로드의 fan-out 분기가 실제 스레드 풀(CPU 바운드 본문, 큰 fan-out 폭)의 이점을 얻을 때 `engine.set_worker_count(N)` / `engine.set_worker_count_auto()`로 풀을 선택하세요.

### My streaming callback_graph fires twice per node

**영향:** Python `@ng.node` 쓰기 전용 노드. `re-agent` 커밋 `2a5c5dc` / `5993935`에서 수정되었으며 NeoGraph master에 복제되었습니다.

**v1 이전 릴리스의 근본 원인:** 순수 쓰기 `GraphNode` 하위 클래스(`Command` 없음, `Send` 없음)는 결과에 대해 한 번, 스트림 Hook에 대해 한 번 실행될 수 있었습니다. 업그레이드하고 단일 `run(NodeInput)` 오버라이드를 구현하세요. v1은 해당 메서드를 한 번 호출하고 선택적 스트림 싱크를 `in.stream_cb`로 노출합니다.

`@ng.node` 데코레이터를 사용하는 경우(하위 클래스화가 아닌), 이미 처리됩니다.

---

## 체크포인트 / Postgres

### `PostgresCheckpointStore`를 찾을 수 없음 / 가져오기 오류

PyPI 휠은 `PostgresCheckpointStore`가 활성화된 상태로 제공됩니다(libpq는 v0.1.3부터 번들됨). `import neograph_engine; neograph_engine.PostgresCheckpointStore`는 직접 작동할 것입니다.

소스에서 `-DNEOGRAPH_BUILD_POSTGRES=ON` 없이 빌드한 경우, 해당 클래스는 바인딩에 존재하지 않습니다. 플래그를 설정한 상태로 CMake 구성을 다시 실행한 후 다시 빌드하세요.

### Postgres 연결: `FATAL: password authentication failed`

`PostgresCheckpointStore` 연결 문자열은 libpq를 따릅니다:

```
postgresql://user:password@host:port/dbname
```

비밀번호에 URL 특수 문자(`@`, `:`, `/`, `%`)가 포함된 경우 URL 인코딩하세요 — 또는 `key=value` 형식을 사용하세요:

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 비동기 Postgres 재연결은 30초 후에 시간 초과됩니다.

비동기 초기/대체 연결은 전체 시도에 대해 하나의 프로덕션 안전 데드라인을 사용합니다. 연결 문자열에 직접 작성된 양수 `connect_timeout=N`는 해당 전역 예산을 초 단위로 설정하며, `connect_timeout=1`는 PostgreSQL의 최소 2초로 올림 처리됩니다. 명시적 값이 없거나 0 또는 음수인 경우 NeoGraph는 30초를 사용합니다. `PGCONNECT_TIMEOUT` 및 서비스 파일 타임아웃 값은 초기 비동기 연결 단계를 제한하기에는 너무 늦게 해석되므로 30초 기본값을 사용합니다. 비동기 데드라인이 달라야 하는 경우 연결 문자열에 값을 직접 넣으세요.

예산은 다중 호스트 연결 문자열의 모든 호스트와 해석된 IP를 포괄하며, 호스트별로 곱해지지 않습니다. 이는 `connect_timeout`가 각 호스트에 개별적으로 적용되는 동기 libpq와 의도적으로 다릅니다. 동기 `PostgresCheckpointStore` 구성 및 교체는 변경되지 않습니다.

예를 들어, 이는 완전한 비동기 교체 시도에 60초를 부여합니다:

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### Postgres `relation "neograph_checkpoints" does not exist`

저장소는 첫 사용 시 테이블을 생성합니다(`CREATE TABLE IF NOT EXISTS`). DB 사용자에게 CREATE 권한이 없는 경우 스키마를 수동으로 실행하세요. SQL은 [`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)의 `kSchema` 아래에 있습니다.

---

## 예시 / docker

### `docker compose run agent` 예제 26이 PG를 찾지 못함

compose 파일은 `db` 서비스가 `postgres://neograph:neograph@db:5432/neograph`로 접근 가능할 것으로 예상합니다. docker-compose 외부에 있는 경우 `PG_URL`를 접근 가능한 호스트로 설정하세요. 전체 환경 변수 표는 [`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)를 참조하세요.

### Crawl4AI 예제가 시작되지 않음

Crawl4AI는 선택적 Docker 컨테이너입니다:

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

예제 17, 25, 26은 `CRAWL4AI_URL`(기본값 `http://localhost:11235`)에 접근할 수 없을 때 정상적으로 대체됩니다.

### `example_clay_chatbot` 빌드 대상이 없음

예제 11은 CMake 구성 시 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`가 필요합니다:

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

Clay(UI 레이아웃) + Raylib(렌더러)를 가져옵니다 — 그래서 플래그 뒤에 있는 것입니다.

---

## 스트리밍 이벤트

### `event.node`는 `AttributeError`를 발생시킵니다

속성은 `event.node_name`입니다(C++ 필드 이름과 일치). `event.type`(열거형) 및 `event.data`(JSON 사전)도 동일합니다.

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### 내 `StreamMode.TOKENS` 콜백이 절대 실행되지 않습니다

공급자는 스트리밍을 지원해야 합니다. 현재:

| 공급자 | 스트리밍? |
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` | ✓ WS |
| `SchemaProvider("claude")` | ✓ SSE |
| 사용자 정의 Python `Provider` 하위 클래스 | 사용자의 `complete_stream` 구현에 따라 달라집니다 |

사용자 정의 Python `Provider`의 경우, `complete_stream`를 재정의하세요. Python 하위 클래스는 async 가상 재정의를 노출하지 않습니다. 새 C++ 백엔드의 경우, `CompletionProvider`에서 파생하고 `request.streaming()`을(를) `do_invoke()`에서 처리하세요. 기존 C++ `Provider` 하위 클래스는 계속해서 `complete_stream()` 또는 `complete_stream_async()`를 재정의하실 수 있습니다. 스트리밍 구현이 없는 경우, 기본값은 수집된 응답을 토큰 단위가 아닌 하나의 덩어리로 출력합니다.

---

## OpenTelemetry

### 내 OTel 스팬이 `parent_id=None`로 나타납니다(1개가 아닌 4개의 개별 트레이스)

**영향:** `neograph_engine.tracing` 커밋 `9073671` 이전.

**근본 원인:** `tracer.start_span` + `use_span(...).__enter__()`는 contextvars에 의존하는데, 이는 C++ → Python pybind 콜백 경계를 넘어 전파되지 않습니다.

**수정:** `otel_tracer` 헬퍼는 이제 `set_span_in_context(root_span)`를 통해 부모 컨텍스트를 스냅샷하고 각 자식 노드의 `start_span`에 명시적으로 전달합니다. `9073671` 이후 버전으로 업그레이드하십시오.

직접 OTel 통합을 구축하는 경우에도 동일하게 수행하십시오. 바인딩 경계를 넘어 contextvars에 의존하지 마십시오.

### 내 LLM 스팬이 내 노드 스팬과 다른 trace ID로 표시됩니다.

**영향:** `neograph_engine.openinference` v0.6.0 최종 버전 이전(커밋 `fa8ed50`).

**근본 원인:** `openinference_tracer` 세트가 `parent_ctx`(스냅샷)을 설정했지만 노드 스팬을 OTel 현재 컨텍스트로 *연결*하지 않았습니다. 그래서 노드 본문이 `provider.complete()`와 `OpenInferenceProvider`를 호출하고 `llm.complete` 스팬을 `tracer.start_as_current_span(...)`를 통해 열었을 때, 새 스팬은 전역 루트로 폴백되어 추적이 LLM 호출별로 별도의 추적 ID로 분할되었습니다.

**수정:** `openinference_tracer` 이제 `otel_context.attach(set_span_in_context(span))` 에 대해 `NODE_START` 수행하고 결과 토큰을 스팬과 함께 저장합니다. `NODE_END` / `ERROR` / `INTERRUPT` 스팬을 종료하기 전에 토큰을 분리하여 이전 현재 스팬을 복원합니다. v0.6.0에서 Phoenix에 대해 검증됨 — 단일 트레이스 트리와 `graph.run > node.X > llm.complete` 계층 구조.

v0.6.0+를 사용 중인데도 여전히 분할 트레이스가 보인다면, 공급자가 래핑되지 않은 것입니다 — `ctx.provider = OpenInferenceProvider(inner, tracer)`가 `engine.compile(...)` **이전에** 실행되는지 확인하십시오. 그렇지 않으면 엔진이 래핑되지 않은 공급자에 바인딩됩니다.

### `pip install opentelemetry-api`가 `openinference`를 가져올 때 ImportError를 발생시킵니다

`neograph_engine.openinference`는 `opentelemetry`를 지연 가져옵니다. ImportError는 첫 사용 시에만 발생하며 한 줄의 설치 힌트가 표시됩니다::

    pip install opentelemetry-api opentelemetry-sdk

스팬을 OTLP를 통해 Phoenix / Langfuse / Tempo로 푸시하려면 `opentelemetry-exporter-otlp`를 추가하세요.

### 내 사용자 정의 `Tracer` 어댑터가 `session.close()` 이후에 멈추거나 / 충돌하거나 / 쓰레기를 출력합니다 (이슈 #24)

`neograph::observability::Tracer` 어댑터(C++)를 작성하여 스팬을 메모리 내 목록에 기록한 다음, `OpenInferenceTracerSession::close()` 호출 **후에** 목록을 순회했습니다. 순회는 해제된 메모리를 읽습니다.

`close()`는 루트 스팬(및 노드별 스팬 스택)에 대한 내부 `unique_ptr<Span>`를 재설정합니다. 어댑터가 `start_span`에서 반환한 래퍼 객체에 **원시 포인터**를 제공했다면, `close()`가 반환되는 순간 해당 포인터는 댕글링 상태가 됩니다. 래퍼는 호출자가 소유했으며, 호출자가 방금 해제했기 때문입니다.

**수정 방법:** 어댑터는 기록된 스팬 데이터 자체를 소유해야 하며, 호출자 소유 래퍼에 대한 원시 포인터만 추적해서는 안 됩니다. 형태는 다음과 같습니다.

```cpp
// Owned by the tracer (lives until tracer drops):
struct RecordedSpan {
    std::string name;
    RecordedSpan* parent = nullptr;
    std::map<std::string, std::string> attrs;
    // ...status, events, ended flag...
};

// Owned by the OpenInference layer (may be reset on close):
class WrapperSpan : public obs::Span {
    RecordedSpan* rec_;        // pointer into the tracer-owned data
public:
    void set_attribute(...) override { rec_->attrs[...] = ...; }
    // ...
};

class MyTracer : public obs::Tracer {
    std::vector<std::unique_ptr<RecordedSpan>> records_;  // ← owns data
    // start_span builds a fresh RecordedSpan, returns a Wrapper
    // pointing at it. Walk records_ for inspection — never the
    // wrappers.
};
```

참조: `tests/test_openinference_cpp.cpp::InMemoryTracer` (표준 테스트 픽스처) 및 `examples/49_openinference.cpp::PrintTracer` (stderr 출력 데모)는 둘 다 이 정확한 패턴을 사용합니다. 동일한 경고가 `@warning` 블록의 `Tracer` 및 `OpenInferenceTracerSession::close()` 헤더에 있습니다.

**버그가 나타나는 방식:** 관찰 가능한 실패 모드에는 검사 루프 내부에서의 깨끗한 크래시(최상의 경우), 스팬 이름을 인쇄하는 도중의 행(hang)(해제된 버퍼에 문자열 포맷터를 반복하게 만드는 어떤 것이 들어 있었던 경우), 또는 잘못된 속성 값 등이 있습니다. 이 세 가지 모두 동일한 근본 원인입니다.

---

## 빌드 오류

### GCC 13 내부 컴파일러 오류: `build_special_member_call`, `cp/call.cc:11096` (이슈 #23)

Ubuntu 24.04의 기본 GCC 13(또는 모든 GCC 13.x)을 사용 중입니다. 빌드가 다음 오류로 실패합니다.

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

…코루틴 내부에서 `co_await x.foo_async(...)` 를 수행하는 줄(일반적으로 `asio::co_spawn` 에 전달된 람다 본문)에서 `main()`에서 발생합니다. 이는 GCC 13 프론트엔드 버그이며, 여러분의 코드 문제가 아닙니다. GCC 14+, Clang 18+, MSVC 19.40+는 모두 동일한 소스를 변경 없이 컴파일합니다.

**세 가지 해결 방법**, 선호 순서대로:

1. **컴파일러를 업그레이드하세요** — Ubuntu 24.04에서 `sudo apt install gcc-14 g++-14` (24.10은 기본적으로 GCC 14를 제공), 그런 다음 `cmake -DCMAKE_CXX_COMPILER=g++-14 ...`. 가장 깔끔한 해결책이며, 자연스러운 방식으로 코드를 작성할 수 있습니다.

2. **`neograph::async::run_sync`로 코루틴을 구동합니다.** `asio::co_spawn`을 `main()`에서 사용하는 대신 이 방법을 쓰면 관찰 가능한 동작은 같지만 프론트엔드 ICE가 발생하지 않습니다:

   ```cpp
   // Instead of:
   asio::co_spawn(io,
       [&]() -> asio::awaitable<void> {
           result = co_await tool.execute_async(args);   // ← GCC 13 ICEs here
       },
       asio::detached);
   io.run();

   // Do:
   #include <neograph/async/run_sync.h>
   result = neograph::async::run_sync(tool.execute_async(args));
   ```

`run_sync`는 자체 비공개 `io_context`를 구축하고 awaitable을 완료까지 구동합니다. 내부적으로는 `co_spawn + io.run()`가 수행하는 것과 동일하지만, 호출 지점은 컴파일러 관점에서 동기식이므로 ICE가 발생하지 않습니다.

3. **코루틴을 재구성**하여 `co_await`가 자유 함수나 람다 본문이 아닌 일반 클래스의 멤버 함수 내부에서 발생하도록 합니다. 이는 일부 경우에 작동하지만 진단이 항상 올바른 형태 변경을 가리키는 것은 아닙니다. 옵션 1 또는 2가 더 안정적입니다.

**이 저장소에서 문제가 발생하는 위치:** CMakeLists에는 원래 ICE가 발생한 `example_03` 주위에 예제별 툴체인 게이트가 있으며, `examples/50_async_tool.cpp`는 `run_sync`를 사용하고 `co_spawn`을 `main()`에서 사용하지 않는 방식으로 문제를 우회합니다. 자연스러운 `co_spawn`-from-main 형태를 따르는 새 코루틴 예제나 테스트는 같은 툴체인에서 동일한 ICE를 만납니다. 이때는 위의 1번 또는 2번 방법을 적용하십시오.

## Python 타입 동일성(v0.5.0+)

### `isinstance(params.messages, list)`는 False를 반환합니다.

**영향 범위:** v0.5.0 이상, 다섯 개의 벡터 속성 표면: `CompletionParams.messages`, `.tools`, `ChatMessage.tool_calls`, `NodeResult.writes`, `.sends`.

**이유:** v0.5.0은 `params.messages.append(...)`의 무음 no-op을 수정하면서 이 벡터들을 불투명 타입(`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`)으로 바인딩하여 `.append`가 라이브 C++ 벡터를 변경하도록 했습니다. 그 대가로 속성의 타입은 이제 예를 들어 `ChatMessageList`(pybind 클래스)이며, 일반 Python `list`가 아닙니다.

**계속 작동하는 것:**
- `params.messages = [m1, m2]` — `py::implicitly_convertible<py::list, …>`는 할당 시 Python 리스트를 자동으로 변환합니다.
- `for m in params.messages` — 반복 프로토콜.
- `len(params.messages)`, `params.messages[i]`, `params.messages[i] = m`, 슬라이싱.
- `params.messages.append(...)`, `.extend(...)`, `.insert(...)`, `.pop(...)`, `.clear()` — 모두 실시간으로 C++ 벡터에 전달됩니다.

**무엇이 깨졌나 (드문 경우):**
- `isinstance(x, list) → False`. 일반 Python 리스트가 정말로 필요하다면, 구체화하세요: `list(params.messages)`.
- `json.dumps(params.messages)` — 바인딩된 클래스는 직접 JSON 직렬화할 수 없습니다. 변환하세요: `json.dumps([{"role": m.role,
  "content": m.content} for m in params.messages])`.

`ChatMessage.image_urls` (`std::vector<std::string>`)는 마이그레이션되지 *않았습니다* — `vector<string>`는 호출 사이트 스위핑(callsite sweeping) 없이 전역 OPAQUE에 대한 바인딩에서 너무 광범위하게 사용됩니다. `.append()` no-op은 문서화된 제한 사항으로 그 자리에 남아 있습니다. v0.6+ 후보는 `add_image_url()` 편의 메서드를 통한 것입니다.

---

## 소스에서 빌드

### CMake 구성: Windows의 `Could NOT find SQLite3`

현재 Windows 휠은 SQLite를 활성화하고 이에 맞는 런타임 DLL을 함께 제공합니다. 사용자 정의 소스 빌드에서는 동일한 vcpkg/MSVC 툴체인을 통해 SQLite를 설치하십시오. SQLite가 의도적으로 필요 없다면 `-DNEOGRAPH_BUILD_SQLITE=OFF`를 전달할 수 있지만, 호환되지 않는 MSVC 런타임용 DLL을 섞어 사용해서는 안 됩니다.

### CMake 구성: Linux의 `Could NOT find CURL`

선택적 의존성. 패키지 관리자를 통해 설치하세요:

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

또는 비활성화: `-DNEOGRAPH_USE_LIBCURL=OFF`. libcurl이 없으면 `SchemaProvider`의 `prefer_libcurl=True` 모드(HTTP/2)를 사용할 수 없습니다 — 기본 ConnPool(HTTP/1.1)은 여전히 작동합니다.

### Pybind 바인딩이 정의되지 않은 참조로 링크에 실패합니다

새 코드를 가져온 후 CMake를 다시 실행하지 않고 `make`를 다시 실행하고 있을 가능성이 높습니다. 빌드 디렉터리의 컴파일된 개체 파일이 이전 헤더의 기호를 참조합니다. `make clean && make`를 실행하거나 빌드 디렉터리를 삭제하고 다시 구성하십시오.

### 스크립트에서 A2A 서버를 시작할 때의 `OPENAI_API_KEY not set`

`cppdotenv::auto_load_dotenv()`는 이를 호출하는 바이너리 내부의 `.env`를 읽지만, 런처 스크립트에서 포크된 자식 프로세스는 런처가 이미 내보내지 않은 것은 **어떤 것도** 상속하지 않습니다. 스크립트가 다음을 수행하는 경우:

```bash
./member_server 8101 ...   # forks before any env is set up
```

…각 자식은 빈 환경을 보고 시작을 거부합니다. 포크를 수행하는 셸에 변수가 내보내지도록 런처에서 먼저 `.env`를 소싱하십시오:

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

쿡북의 `scripts/run_session.sh`는 형제 `.env`로의 폴백이 있는 전체 패턴을 보여줍니다.

### 멀티 페르소나 / 멀티 프로세스 A2A: OpenAI 제공자를 공유할 위치

`OpenAIProvider::create_shared(cfg)`(`shared_ptr<Provider>` 반환)를 사용하고, `create(cfg)`(`unique_ptr` 반환)는 사용하지 마십시오. 공유 형태는 `NodeFactory` 람다에 캡처할 수 있고 모든 그래프 노드와 A2A 요청에서 재사용할 수 있습니다. 반면 `create()`의 `unique_ptr`를 사용하면 직접 `release()`한 뒤 다시 래핑해야 합니다.

---

## C++ 소비자 — `httplib.h` 매크로 일관성(필수적인, 이슈 #16)

C++ 애플리케이션을 빌드할 때 **NeoGraph에 링크**하고 동시에 자체 번역 단위에서 `#include <httplib.h>`도 사용하는 경우(예: 자체 `httplib::Server` SSE 엔드포인트를 실행하기 위해), `<httplib.h>`를 포함하는 모든 TU는 include **이전에** `#define CPPHTTPLIB_OPENSSL_SUPPORT`를 반드시 수행해야 합니다. 단 하나의 TU에서 매크로가 누락되면 `getaddrinfo` 내부에서 `SchemaProvider::complete_stream`가 LLM 엔드포인트에 처음 도달할 때 자동으로 SEGV가 발생합니다.

### 발생 원인

`cpp-httplib`는 헤더 전용입니다. 클래스 `httplib::ClientImpl`는 `CPPHTTPLIB_OPENSSL_SUPPORT`가 정의될 때 **조건부로 더 커집니다**(SSL 관련 멤버가 추가되고 레이아웃이 약 8바이트 이동). 라이브러리의 모든 함수가 `inline`이므로 링커는 인라인 함수마다 인스턴스화를 하나씩 유지하고 중복을 폐기합니다. 바이너리의 두 TU가 서로 다른 `ClientImpl` 레이아웃에 대해 컴파일되면(하나는 매크로를 정의하고 다른 하나는 정의하지 않았기 때문에), 링커는 하나의 정의를 선택합니다. *다른* TU의 컴파일 측은 잘못된 오프셋에서 멤버에 접근합니다 — 전형적인 ODR 위반입니다. 손상은 인접 필드에 발생합니다(예: `proxy_host_`는 실제로 `path_`의 꼬리인 오프셋에서 읽게 됩니다), 그리고 `httplib::ClientImpl::create_client_socket`는 와일드한 `proxy_host_.c_str()` → `getaddrinfo` → `internal_strlen` → SEGV 경로로 "프록시 사용" 분기에 진입합니다.

### 증상

ASan 하에서:

```
==NNNN==ERROR: AddressSanitizer: SEGV on unknown address
    #0 internal_strlen (...)
    #1 getaddrinfo
    #2 httplib::detail::create_socket
    #3 httplib::detail::create_client_socket
    #4 httplib::ClientImpl::create_client_socket
    #5 httplib::SSLClient::create_and_connect_socket
    ...
    #N neograph::llm::SchemaProvider::complete_stream
```

ASan 없이: gdb를 통한 동일한 스택, 와일드 포인터 값은 텍스트처럼 보일 수 있습니다(잘못된 오프셋 슬롯에 있던 바이트가 무엇이든 — ASan에서는 일반적으로 `0xBE` 격리 독(quarantine poison)이고, ASan 없이는 JSON/UTF-8 조각으로 디코딩되어 실제 원인으로 인한 메모리 손상처럼 *보이는* 초기화되지 않은 스택 콘텐츠일 수 있습니다 — 이것이 오해를 불러일으키는 증상입니다).

### 수정

`<httplib.h>`를 포함하는 모든 번역 단위에서:

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

또는 CMake에서 전역적으로 (권장 — 전체 타깃에서 일관성을 보장함):

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

매크로는 자체 httplib 사용이 `Server`만 필요하고 `SSLClient`가 필요하지 않더라도 무해합니다 — 멤버를 **추가**할 뿐이며, 자체 측에서 실제로 SSL을 수행할 필요는 없습니다.

### ASan 없이 감사하는 방법

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

`<httplib.h>`의 어떤 포함 지점이라도 `#define CPPHTTPLIB_OPENSSL_SUPPORT`(같은 TU에서 또는 컴파일 플래그 정의를 통해)가 **앞에** 오지 않는다면, 그것이 거의 확실히 버그입니다.

### NeoGraph가 이 문제를 해결해 줄 수 없는 이유

NeoGraph의 자체 .cpp 파일은 모두 매크로를 일관되게 정의합니다. 위반은 다운스트림 TU가 매크로 *없이* httplib.h도 가져올 때만 발생합니다. 컴파일 시간에 이를 감지하려면 (a) NeoGraph가 공개 헤더에서 `httplib::ClientImpl`를 노출하거나(의도적으로 하지 않음 — httplib는 `SchemaProvider.cpp` 내부에 유지됨), 또는 (b) C++가 지원하지 않는 TU 간 구조체 크기의 링크 시간 `static_assert`가 필요합니다. 함정을 문서화하는 것이 우리가 할 수 있는 최선이며, 이 섹션이 그 문서입니다. 이슈 #16 종료.

---

## 토폴로지 JSON을 만드는 도구/에디터가 엔진과 달라짐

### 증상

NeoGraph 토폴로지 JSON을 생성하는 생성기, GUI 또는 시각적 블록 편집기를 작성했거나 사용하고 있습니다. 이 도구는 엔진이 `compile()`에서 `Unknown node type:` / `Unknown reducer:` / `Unknown condition:`로 거부하는 노드 유형, 리듀서 또는 조건을 제공했거나, 그린 분기가 조용히 절대 실행되지 않습니다.

### 발생 원인

도구의 팔레트는 수동으로 유지 관리되었으며 실제로 링크된 NeoGraph 버전보다 뒤처졌습니다. 분기 사례는 전형적인 최상위 `conditional_edges` 회귀입니다(v0.1.0–v0.1.7에서 조용히 제거되었고, v0.1.8에서 수정됨) — 해당 블록을 생성하는 도구는 로더→컴파일 왕복을 통과하는지 확인해야 합니다.

### 수정

팔레트를 수동으로 유지 관리하지 마십시오. 엔진은 정확히 허용하는 항목의 기계 판독 가능 스키마를 생성합니다. 도구를 여기에 고정하십시오:

- C++: `neograph::graph::NodeFactory::instance().export_schema()`.
- CLI: `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`).
- Python: `neograph_engine.export_schema()` → dict입니다.

문서에는 `neograph_version`가 포함되어 있습니다. 도구가 이를 캐시된 스키마와 비교하고 불일치 시 경고하도록 하십시오. `node_types`는 호출 시점에 `NodeFactory`에 등록된 내용을 반영하므로, `compile()` 전에 하는 것과 정확히 동일하게 사용자 정의 노드 유형/리듀서/조건을 *내보내기 전에* 등록하십시오. (배경: 이슈 #56.)

---

## 엄격한 토폴로지 검증

### 증상

`compile()`는 `strict topology validation failed (schema_version 1)`를 던지며, `$: unknown or unconsumed key 'conditionnal_edges'`, `nodes.X.barrier: 'wait_for' is missing or empty` 또는 `translation validation failed: compiled graph does not round-trip`와 같은 키를 나열합니다.

### 발생 원인

토폴로지가 `"schema_version": 1`를 선언하며, 이는 엄격한 컴파일을 선택하게 합니다: 컴파일러가 소유한 모든 객체의 모든 키는 파서가 *소비*해야 합니다. 아무도 소비하지 않은 키는 거의 항상 오타(`conditionnal_edges`, `max_retry`, `promt`)이거나 엔진이 그렇지 않으면 **조용히 삭제**할 구조입니다 — v0.1.0–v0.1.7 `conditional_edges` 회귀의 근본 원인입니다. 라운드트립(재출력-검증) 오류는 컴파일된 그래프가 JSON으로 다시 출력될 때 더 이상 입력과 일치하지 않음을 의미합니다: 컴파일러가 무언가를 잃어버렸거나 재배선했으며, 메시지가 정확히 무엇인지 나열합니다.

### 수정

- 나열된 키를 수정하세요 — 각 오류에는 JSON 경로가 포함되어 있습니다.
- 주석과 편집기 메타데이터는 주석 네임스페이스에 속합니다: `_` 또는 `x-`로 시작하는 키(예: `_comment`, `x-studio-pos`)는 항상 허용되며 검증되지 않습니다.
- 장벽은 비어 있지 않은 `wait_for` 배열이 필요합니다; 인라인 조건부 엣지는 `routes`를 통해 라우팅되므로, 그 위의 `to`는 죽은 것입니다 — 대상을 `routes`로 이동하거나 삭제하십시오.
- 선언된 구성 스키마(3-인자 `register_type`)로 등록된 사용자 정의 노드 유형은 폐쇄 세계로 검사됩니다; 유형을 옵트아웃하려면 스키마에 `"additionalProperties": true`를 추가하십시오.
- 역사적 관대한 파싱으로 대체하려면 `schema_version`를 제거하십시오 — 그러면 알 수 없는 키가 다시 무시되고 라운드트립 불일치는 stderr에만 경고합니다. 새 문서는 엄격하게 유지해야 합니다.

### 호환성 타임라인

- 모든 `0.x` 릴리스는 관대한 호환성 경로에서 누락되거나 0인 `schema_version` 문서를 유지합니다. `0.x` 업데이트는 이를 조용히 엄격한 문서로 재해석하지 않습니다.
- 새 정의, 내장 그래프 팩토리, 유지 관리되는 예제는 현재 버전(`TOPOLOGY_SCHEMA_VERSION`, 현재 `1`)을 선언합니다.
- 계획된 `1.0.0` 경계는 누락 또는 0 버전을 조용히 라우팅 또는 파싱 의미를 변경하는 대신 마이그레이션 진단으로 거부합니다.
- C++ 입력은 `GraphCompiler::upgrade_to_latest()`로, Python 입력은 `ng.upgrade_topology()`로 업그레이드하십시오. 무시된 레거시 데이터는 충돌 방지 `x-upgraded-*` 주석 아래에 유지됩니다. 엄격한 Core JSON은 유지되는 교환 아티팩트입니다; JavaScript 소스는 QuickJS `define()`를 통해 재컴파일해야 합니다.

---

## 버그 신고

위 증상이 해당되지 않는 경우:

1. 먼저 `pip install --upgrade neograph-engine`를 실행하십시오 — 많은 문제가 패치 수준 수정입니다.
2. 최소 재현기를 캡처하세요:
   - 그래프 정의
   - 사용 중인 노드 유형
   - 정확한 `engine.run(...)` 호출
   - `result.execution_trace` 및 (스트리밍 중인 경우) 본인이 확인한 이벤트
3. 플랫폼, Python 버전, `neograph_engine.__version__`을 명시하세요.
4. <https://github.com/fox1245/NeoGraph/issues>에서 이슈를 여세요.

버그가 특정 LLM 엔드포인트에서만 발생하는 경우, 와이어 수준 형태(OpenAI Responses의 경우 `example_responses_envelope`, 관련 시 원시 HTTP 추적의 경우 `tcpdump`/`wireshark`)도 포함해 주세요.
