<!-- neograph-i18n: source=docs/troubleshooting.md locale=ko source_sha256=f2f219b4b5c913d0969a37ca02b44d437dde4dcc74c6475cc9f9c52944c99597 -->
**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

# 문제 해결


먼저 증상을 해결하고 근본 원인을 해결한 후 해결하세요. 뭔가에 부딪히면
여기에는 없습니다. 증상이 있는 문제를 열어주세요.
나중에 이 목록에 들어가세요.

> **5초 동안의 상태 점검.** 무엇보다 먼저 확인하세요.
> 당신은 최신 패치를 사용하고 있습니다:
> ```bash
> pip install --neograph-엔진 업그레이드
> python -c "neograph_engine 가져오기; print(neograph_engine.__version__)"
> ```
> 아래의 대부분의 문제는 특정 릴리스에서 해결되었습니다. 먼저 업그레이드하고,
> 두 번째 디버그.

---

## 설치/가져오기

### `pip install neograph-engine`는 성공하지만 `import`는 실패합니다.

Python 버전/플랫폼이 일치하지 않을 가능성이 있습니다. 우리는 다음을 위한 바퀴를 배송합니다:

|플랫폼|버전|
|---|---|
|리눅스 x86_64 (manylinux_2_34)|파이썬 3.9 – 3.13|
|리눅스 aarch64 (manylinux_2_34)|파이썬 3.9 – 3.13|
|macOS arm64(14+)|파이썬 3.9 – 3.13|
|윈도우 x64 (MSVC)|파이썬 3.9 – 3.13|

이 매트릭스 외부의 모든 항목은 sdist로 전달됩니다(출처
빌드)에는 CMake 3.16+, OpenSSL 및 C++20 툴체인이 필요합니다. 만약에
귀하의 플랫폼이 목록에 없고 소스 빌드가 실패했습니다.
문제.

### Linux의 `ImportError: ... GLIBC_2.32 not found`

Linux 휠은 `manylinux_2_34`입니다. glibc ≥ 2.34가 필요합니다(Ubuntu 22.04+,
데비안 12+, RHEL 9+). 이전 배포판에서는 소스에서 빌드하세요.

### Windows의 `ImportError: DLL load failed`

Windows 휠은 자체 종속성을 제공하지만 Python 설치는
휠 아키텍처(x64)와 일치해야 합니다. 다음으로 확인하세요.

```powershell
python -c "import platform; print(platform.architecture())"
```

`('32bit', ...)`가 인쇄되면 32비트 Python을 사용하는 것입니다.
64비트 하나.

---

## TLS / 네트워크

### 공급자 호출이 60초 동안 중단된 후 `ConnPool::async_post: timeout` 오류가 발생함

**영향을 받음:** `neograph-engine` 휠 v0.1.0 – v0.1.6.

**근본 원인:** 번들 OpenSSL에는 CA 저장소 경로가 컴파일되어 있습니다.
`/etc/pki/tls/...`(RHEL 규칙)을 가리키고 있습니다. 우분투, 데비안에서는
macOS, CA 스토어는 다른 곳에 있으므로(`/etc/ssl/certs/...`)
휠의 libssl은 피어 인증서와 TLS 핸드셰이크를 확인할 수 없습니다.
오류가 발생하기 전에 전체 요청 시간 초과를 자동으로 기다립니다.

**수정(≥ v0.1.7):** 휠의 `__init__.py`가 이제 자동 포인트됩니다.
가져오기 시 `certifi.where()`의 `SSL_CERT_FILE`. 치받이:

```bash
pip install --upgrade neograph-engine
```

**오래된 휠에 대한 해결 방법:**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**v0.1.7+에서 자동 수정을 선택 해제하려면**(예: 사용자 지정 CA가 있는 경우)
번들): 가져오기 전에 `NEOGRAPH_SKIP_CERT_AUTOFIX=1`를 설정하세요.

### `urllib`는 작동하지만 NeoGraph는 작동하지 않습니다.

위와 동일한 근본 원인 — `urllib`는 OpenSSL 시스템을 사용하지만
휠은 잘못된 CA 경로와 함께 번들로 제공되는 OpenSSL을 사용합니다. 같은 수정:
v0.1.7 이상으로 업그레이드하거나 `SSL_CERT_FILE`를 설정하세요.

### WebSocket 응답(`use_websocket=True`)은 `close=1000`를 사용하여 즉시 닫힙니다.

세 가지 일반적인 원인(빈도순):

1. **API 키/조직에서는 WebSocket 액세스가 활성화되지 않았습니다.** 일부 OpenAI
계층 1 계정에는 아직 WebSocket 모드 액세스 권한이 없습니다. 다음으로 돌아가세요.
`use_websocket=False`를 설정하여 HTTP/SSE.
2. **특정 프록시 경로에서 `User-Agent` 헤더가 누락되었습니다.** 수정됨
`d7c61d0`를 커밋합니다. v0.1.4 이상으로 업그레이드하세요.
3. **`temperature` 필드는 일부 Responses-API 모델에서 거부되었습니다.** 동일
commit은 지원되는 모델의 WS 핸드셰이크에서 이를 제거합니다.

### WASM를 통해 브라우저에서 실행할 때 CORS 오류

WASM 빌드는 아직 browser-CORS에 대한 바이패스 헤더를 구현하지 않습니다.
상태를 확인하려면 [Issue #wasm-cors](../../issues)를 추적하세요.

---

## 그래프 컴파일/실행

### `RuntimeError: Unknown reducer: <name>`

두 개의 리듀서(`"overwrite"` 및 `"append"`)가 바인딩과 함께 제공됩니다.
등록하지 않으면 다른 것은 컴파일되지 않습니다.

**사용자 정의 감속기 등록(Python, v0.1.9 이후):**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

기존 이름을 다시 등록하면 이전 감속기가 대체됩니다. 그만큼
호출 가능은 GIL에서 실행됩니다. 동시 팬아웃 직렬화 보내기
Python 사용자 정의 노드와 동일한 방식입니다.

`"last_value"`(일반적인 LangGraph 별칭)를 입력한 경우 — 이는
여기 `"overwrite"`가 있습니다. 동일한 의미, 다른 이름.

### `RuntimeError: Unknown condition: <name>`

내장 조건: `has_tool_calls`, `route_channel`. 다른 이름
등록되어야 합니다.

**사용자 정의 조건 등록(Python, v0.1.9부터):**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

콜러블은 라이브 `GraphState`를 수신합니다(`state.get(channel)` /
`state.get_messages()` 사용 가능) 일치하는 문자열을 반환해야 합니다.
조건부 에지의 `routes` 키 중 하나입니다.

### `RuntimeError: Write to unknown channel: <name>`

`ChannelWrite`의 채널 이름이 다음과 일치하지 않습니다.
`definition["channels"]`. 채널 이름은 정확합니다. `messages` 및
`Messages`는 다릅니다.

### `RuntimeError: Unknown node type: <name>`

노드 중 하나의 `type` 필드가 해당 노드에 없는 항목을 참조합니다.
공장 등록. 내장형(`llm_call`, `tool_dispatch`,
`intent_classifier`, `subgraph`) 유형 이름은 위에 명시되어 있습니다.
자신의 유형에 따라 전화해야 합니다.
`ng.NodeFactory.register_type(type_name, factory)` BEFORE 컴파일.

### 내 ReAct 루프는 한 번만 실행됩니다 — `execution_trace == ['llm']`

**영향을 받음:** `neograph-engine` 휠 v0.1.0 – v0.1.7.

**근본 원인:** 그래프 컴파일러가 최상위 수준을 삭제했습니다.
`conditional_edges`는 자동으로 차단됩니다. README 빠른 시작과
모든 Python 예제는 이 형식을 사용하므로 ReAct 루프는
단일 LLM 호출(도구 디스패치 없음)

**수정(≥ v0.1.8):** 이제 컴파일러는 두 가지 형식을 모두 허용합니다 — 최상위 수준
`conditional_edges` 어레이 또는 `condition`가 있는 인라인 인 `edges`
필드. 다음을 사용하여 업그레이드하고 확인하세요.

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**오래된 휠에 대한 해결 방법:** 조건부 인라인을 삽입합니다.

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

### `result.execution_trace`가 비어 있음/시작 노드만 표시

그래프는 즉시 `__end__`로 라우팅됩니다. 가장 일반적인 원인:

1. **`__start__`에서 누락된 가장자리.** 모든 그래프에는 최소한 하나가 필요합니다.
`{"from": ng.START_NODE, "to": "..."}` 가장자리.
2. **조건부가 `routes` 맵에 없는 값을 반환했습니다.**
조건의 반환 값이 어떤 키와도 일치하지 않으면 엔진이 다음을 수행합니다.
사전순으로 마지막 항목을 대체용으로 사용합니다. 그것이 다음에 매핑된다면
`__end__`, 자동으로 종료됩니다. 항상 기본 분기를 포함합니다.
3. **`max_steps=0` 또는 `max_steps=1`** — 실행이 천장에 도달했습니다.
즉시. 기본값은 25입니다. ReAct 루프에는 일반적으로 10개 이상이 필요합니다.

### 컴파일 오류: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraph는 사이클(ReAct 루프는 사이클임)을 허용하지만 컴파일러는
*무조건* 주기를 포착합니다 — 조건 없이 `a → b → a`
탈출하다. `__end__`로 라우팅할 수 있는 조건부 에지를 추가합니다.

---

## 성능

### 팬아웃이 예상보다 느립니다.

두 가지 일반적인 원인:

1. **엔진 소유 작업자 풀이 없습니다.** `compile()`의 기본값은 다음과 같습니다.
`set_worker_count(1)` — 풀 없음, 팬아웃 분기 디스패치 인라인
호출자의 실행 프로그램에서 순차적으로 실행됩니다. 풀을 한 번 선택하세요.
`compile()` 이후(및 `run()` 이전):

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

NeoGraph는 또한 처음으로 일회성 stderr 경고를 인쇄합니다.
다중 전송(또는 다중 송신 에지) 팬아웃은 풀 없이 실행됩니다.
따라서 자동 직렬 케이스가 표시됩니다. 다음으로 억제
작업자=1 빠른 경로인 경우 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`
의도적인.
2. **Python 사용자 정의 노드는 본문 중에 GIL**를 보유합니다. 만약 당신의
`@ng.node` 함수는 CPU 바인딩된 Python 작업을 수행하지만 팬아웃 속도가 빨라지지 않습니다.
위로. ONNX / PyTorch / numpy / `requests.get`는 동안 GIL를 릴리스합니다.
기본 호출이므로 병렬화됩니다. 순수한 Python 채점 루프의 경우,
얼마나 많은 작업자를 설정하는지는 중요하지 않습니다.

### `bench_neograph par`는 200μs 이상을 보고합니다.

**v1.0 이전 휠.** v0.1.4–v0.x는 작업자 풀 기본값을 다음과 같이 유지했습니다.
크로스 스레드 제출 비용을 지불한 `hardware_concurrency()`
모든 팬아웃 틱. v1.0에서는 기본값을 `set_worker_count(1)`로 되돌렸습니다.
(풀 없음, 제출 비용 없음) — `par`가 의 프리플립 야구장으로 돌아왔습니다.
신선한 `compile()`. 다음과 같은 풀을 선택하세요.
`engine.set_worker_count(N)` / `engine.set_worker_count_auto()` 일 때
워크로드의 팬아웃 분기는 실제로 실제 스레드의 이점을 얻습니다.
풀(CPU 바인딩 바디, 넓은 팬아웃 너비)

### 스트리밍 콜백이 노드당 두 번 실행됩니다.

**영향을 받음:** Python `@ng.node` 쓰기 전용 노드. 고정됨
`re-agent`는 `2a5c5dc` / `5993935`를 커밋하고 NeoGraph에 복제됩니다.
주인.

**v1 이전 릴리스의 근본 원인:** 순수 쓰기 `GraphNode` 하위 클래스(없음
`Command`, `Send` 없음)은 결과에 대해 한 번, 스트림에 대해 한 번 실행될 수 있습니다.
훅. 단일 `run(NodeInput)` 재정의를 업그레이드하고 구현합니다. v1 호출
해당 메서드를 한 번 사용하고 선택적 스트림 싱크를 `in.stream_cb`로 노출합니다.

`@ng.node` 데코레이터(서브클래싱 아님)를 사용하는 경우 이는 다음과 같습니다.
이미 처리되었습니다.

---

## 체크포인트/포스트그레스

### `PostgresCheckpointStore`를 찾을 수 없음/가져오기 오류

PyPI 휠은 `PostgresCheckpointStore`가 활성화된 상태로 제공됩니다(libpq는
v0.1.3부터 ​​번들로 제공됨). `import neograph_engine; neograph_engine.PostgresCheckpointStore`
직접 작업해야 합니다.

`-DNEOGRAPH_BUILD_POSTGRES=ON` 없이 소스에서 빌드한 경우
클래스는 바인딩에 존재하지 않습니다. 플래그를 사용하여 CMake 구성을 다시 실행하세요.
설정한 다음 다시 빌드하세요.

### 포스트그레스 연결: `FATAL: password authentication failed`

`PostgresCheckpointStore` 연결 문자열은 libpq를 따릅니다.

```
postgresql://user:password@host:port/dbname
```

비밀번호에 URL 특수 문자(`@`, `:`, `/`, `%`)가 포함된 경우 URL-encode
또는 `key=value` 형식을 사용하세요.

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 30초 후 비동기 Postgres 재연결 시간 초과

비동기 initial/replacement 연결은 하나의 프로덕션 안전 기한을 사용합니다.
전체 시도. 직접 작성된 긍정적인 `connect_timeout=N`
연결 문자열은 `connect_timeout=1`를 사용하여 전역 예산을 초 단위로 설정합니다.
PostgreSQL의 최소값인 2초로 반올림됩니다. 명시적인 값이 다음과 같은 경우
없음, 0 또는 음수인 경우 NeoGraph는 30초를 사용합니다. `PGCONNECT_TIMEOUT` 및
서비스 파일 시간 초과 값이 너무 늦게 해결되어 초기 비동기를 바인딩할 수 없습니다.
연결 단계이므로 기본값인 30초도 사용합니다. 값을 직접 입력
비동기 최종 기한이 달라야 하는 경우 연결 문자열에 있습니다.

예산은 다중 호스트 연결 문자열의 모든 호스트와 확인된 IP에 걸쳐 있습니다.
호스트별로 곱해지지 않습니다. 이는 의도적으로 동기식과 다릅니다.
libpq, 여기서 `connect_timeout`는 각 호스트에 별도로 적용됩니다. 동기식
`PostgresCheckpointStore` 구성 및 교체는 변경되지 않습니다.

예를 들어 이는 완전한 비동기 교체 시도에 60초를 제공합니다.

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### 포스트그레스 `relation "neograph_checkpoints" does not exist`

상점은 처음 사용할 때 테이블을 작성합니다(`CREATE TABLE IF NOT EXISTS`).
DB 사용자에게 CREATE 권한이 없는 경우 스키마를 직접 실행하세요.
SQL는 [`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)에 있습니다.
`kSchema` 아래에 있습니다.

---

## 예제/도커

### 예 26의 `docker compose run agent`가 PG를 찾지 못했습니다.

작성 파일은 `db` 서비스가 다음과 같이 연결될 것으로 예상합니다.
`postgres://neograph:neograph@db:5432/neograph`. 밖에 있으면
docker-compose, 대신 `PG_URL`를 연결 가능한 호스트로 설정하세요. 보다
[`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)
전체 환경 테이블의 경우.

### Crawl4AI 예제가 시작을 거부합니다.

Crawl4AI는 선택적 Docker 컨테이너입니다.

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

예제 17, 25, 26은 `CRAWL4AI_URL`(기본값)일 때 정상적으로 폴백됩니다.
`http://localhost:11235`)에 연결할 수 없습니다.

### `example_clay_chatbot` 빌드 대상을 찾을 수 없습니다.

예제 11에는 CMake에서 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`가 필요합니다.
시간을 구성하십시오:

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

Clay(UI 레이아웃) + Raylib(렌더러)를 가져옵니다. 그래서 뒤에 있습니다.
깃발.

---

## 스트리밍 이벤트

### `event.node`는 `AttributeError`를 발생시킵니다.

속성은 `event.node_name`입니다(C++ 필드 이름과 일치). 같은
`event.type`(열거형) 및 `event.data`(JSON 사전)의 경우.

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### 내 `StreamMode.TOKENS` 콜백이 실행되지 않습니다.

공급자는 스트리밍을 지원해야 합니다. 현재:

|공급자|스트리밍?|
|---|---|
|`OpenAIProvider`|✓ HTTP/SSE|
|`SchemaProvider("openai_responses")`|✓ SSE|
|`SchemaProvider("openai_responses", use_websocket=True)`|✓ WS|
|`SchemaProvider("claude")`|✓ SSE|
|사용자 정의 Python `Provider` 하위 클래스|`complete_stream` impl에 따라 다릅니다.|

사용자 정의 Python `Provider`의 경우 `complete_stream`를 재정의합니다. Python 서브클래스
비동기 가상 재정의를 노출하지 마세요. 새로운 C++ 백엔드의 경우 다음에서 파생됩니다.
`CompletionProvider` 및 `do_invoke()`에서 `request.streaming()`를 처리합니다. 기존의
C++ `Provider` 서브클래스는 계속해서 `complete_stream()`를 재정의할 수 있습니다.
`complete_stream_async()`. 스트리밍 구현이 없으면 기본값은
수집된 응답은 증분 토큰이 아닌 하나의 청크로 표시됩니다.

---

## 오픈텔레메트리

### 내 OTel 범위는 `parent_id=None`로 나타납니다(1개가 아닌 4개의 별도 추적).

**영향을 받음:** `9073671` 커밋 전 `neograph_engine.tracing`.

**근본 원인:** `tracer.start_span` + `use_span(...).__enter__()`
전역으로 전파되지 않는 contextvars에 의존합니다.
C++ → Python pybind 콜백 경계.

**수정:** 이제 `otel_tracer` 도우미가 다음을 통해 상위 컨텍스트의 스냅샷을 찍습니다.
`set_span_in_context(root_span)`를 작성하고 이를 명시적으로 각 항목에 전달합니다.
하위 노드의 `start_span`. `9073671`를 넘어 업그레이드하세요.

자신만의 Otel 통합을 진행하는 경우에도 똑같이 하세요. 의존하지 마세요.
바인딩 경계를 넘어 contextvars에 있습니다.

### 내 LLM 스팬은 내 노드 스팬과 다른 추적 ID로 표시됩니다.

**영향을 받음:** v0.6.0 최종 버전 이전의 `neograph_engine.openinference`
(`fa8ed50` 커밋).

**근본 원인:** `openinference_tracer` 세트 `parent_ctx`(a
스냅샷) 그러나 노드 범위를 OTel 현재로 *연결*한 적은 없습니다.
문맥. 따라서 노드 본문이 `provider.complete()`라고 하면
`OpenInferenceProvider`는 다음을 통해 `llm.complete` 스팬을 열었습니다.
`tracer.start_as_current_span(...)`, 새로운 범위가
전역 루트 및 추적은 각 개별 추적 ID로 조각화됩니다.
LLM 호출.

**수정:** 이제 `openinference_tracer`가 작동합니다.
`NODE_START`의 `otel_context.attach(set_span_in_context(span))`
결과 토큰을 범위와 함께 보관합니다. `NODE_END` /
`ERROR` / `INTERRUPT` 범위를 종료하기 전에 토큰을 분리하고,
이전 전류 범위를 복원합니다. v0.6.0에서 확인됨
Phoenix — `graph.run > node.X > llm.complete`를 사용한 단일 추적 트리
계층.

v0.6.0 이상을 사용 중이고 *여전히* 분할 추적이 표시되는 경우 공급자
포장되지 않았습니다. `ctx.provider = OpenInferenceProvider(inner, tracer)`를 확인하세요.
`engine.compile(...)` **전에** 실행됩니다. 그렇지 않으면 엔진이 바인딩됩니다.
포장되지 않은 공급자에게.

### `openinference`를 가져올 때 `pip install opentelemetry-api`에서 ImportError가 발생합니다.

`neograph_engine.openinference` `opentelemetry`를 지연 가져옵니다. 그만큼
ImportError는 한 줄의 설치 힌트와 함께 처음 사용할 때만 발생합니다.

    pip install opentelemetry-api opentelemetry-sdk

범위를 푸시하려면 `opentelemetry-exporter-otlp`를 추가하세요.
OTLP를 통한 Phoenix/Langfuse/Tempo.

### 내 사용자 정의 `Tracer` 어댑터가 `session.close()` 이후 정지/충돌/가비지 인쇄(#24 문제)

`neograph::observability::Tracer` 어댑터(C++)를 작성했습니다.
레코드는 메모리 내 목록에 걸쳐 있으며 **다음** 이후에 목록을 살펴봅니다.
`OpenInferenceTracerSession::close()`를 호출합니다. 산책은 읽는다
해제된 메모리.

`close()`는 루트를 통해 내부 `unique_ptr<Span>`를 재설정합니다.
범위(및 노드별 범위 스택). 어댑터를 나눠준 경우
반환된 래퍼 개체에 대한 **원시 포인터**
`start_span`, 그 포인터는 `close()` 순간에 매달려 있습니다.
반환 — 래퍼는 호출자가 소유했으며 호출자는
방금 석방했습니다.

**수정:** 어댑터는 기록된 범위 데이터 자체를 소유해야 합니다.
호출자가 소유한 래퍼에 대한 원시 포인터를 추적하면 됩니다. 모양:

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

참고 자료: `tests/test_openinference_cpp.cpp::InMemoryTracer`
(표준 테스트 픽스처) 및 `examples/49_openinference.cpp::PrintTracer`
(stderr-printing 데모) 둘 다 이 정확한 패턴을 사용합니다. 같은
경고는 `Tracer`의 `@warning` 블록에 있으며
헤더에 `OpenInferenceTracerSession::close()`가 있습니다.

**버그가 나타나는 방식:** 관찰 가능한 실패 모드에는 깨끗한 오류가 포함됩니다.
검사 루프 내부 충돌(최상의 경우), 중간 중단
스팬 이름 인쇄(해제된 버퍼에 우연히
문자열 포맷터를 반복하는 것) 또는 단지 잘못된 것
속성 값. 세 가지 모두 근본 원인은 동일합니다.

---

## 빌드 오류

### GCC 13 내부 컴파일러 오류: `build_special_member_call`, `cp/call.cc:11096`(#23 문제)

Ubuntu 24.04의 재고 GCC 13(또는 GCC 13.x)을 사용하고 있습니다. 빌드
사망 원인:

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

...코루틴 내부에서 `co_await x.foo_async(...)`를 수행하는 라인에서
(일반적으로 람다 본문은 `main()`에서 `asio::co_spawn`로 전달됩니다.)
이것은 코드가 아닌 GCC 13 프런트 엔드 버그입니다. GCC 14+, 클랭 18+,
및 MSVC 19.40+는 모두 동일한 소스를 변경하지 않고 컴파일합니다.

**세 가지 이스케이프**(선호도 순):

1. **컴파일러 업그레이드** — `sudo apt install gcc-14 g++-14` on
Ubuntu 24.04(24.10은 기본적으로 GCC 14 제공)
`cmake -DCMAKE_CXX_COMPILER=g++-14 ...`. 가장 깨끗한 수정; 하자
자연스러운 방식으로 코드를 작성합니다.

2. **대신 `neograph::async::run_sync`**를 통해 코루틴을 구동합니다.
`main()`의 `asio::co_spawn`. 동일한 관찰 가능한 행동, 아니요
프런트엔드 ICE:

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

`run_sync`는 자체 프라이빗 `io_context`를 구축하고
완료될 때까지 대기 가능 - 내부적으로는 무엇과 동일
`co_spawn + io.run()`는 그렇게 하지만 통화 사이트는
컴파일러의 관점이므로 ICE는 절대 실행되지 않습니다.

3. **코루틴을 재구성**하여 `co_await`가 내부에서 발생하도록 합니다.
자유 함수나 람다가 아닌 일반 클래스의 멤버 함수
몸. 이는 어떤 경우에는 작동하지만 진단이 항상 그런 것은 아닙니다.
올바른 모양 변경을 가리킵니다. 옵션 1이나 2가 더 안정적입니다.

**이 저장소의 내용은 다음과 같습니다.** CMakeLists에는 예제별
`example_03`(원래 ICE 사이트) 주변의 툴체인 게이트
`examples/50_async_tool.cpp`는 다음을 사용하여 문제를 해결합니다.
`main()`의 `co_spawn` 대신 `run_sync`입니다. 새로운 코루틴
자연스러운 `co_spawn`-from-main 모양을 따르는 예제/테스트
동일한 툴체인에서 동일한 ICE가 발생합니다. 옵션 1을 적용하면 됩니다.
또는 2.

## Python 유형 ID(v0.5.0+)

### `isinstance(params.messages, list)`는 False를 반환합니다.

**영향을 받음:** v0.5.0 이상, 5개 벡터 속성 표면:
`CompletionParams.messages`, `.tools`, `ChatMessage.tool_calls`,
`NodeResult.writes`, `.sends`.

**이유:** v0.5.0에서는 `params.messages.append(...)`의 자동 무작동 문제를 수정했습니다.
이러한 벡터를 불투명 유형으로 바인딩하여
(`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`) 따라서 `.append`가 변경됩니다.
라이브 C++ 벡터. 절충안: 이제 속성 유형은 다음과 같습니다.
예를 들어 일반 Python `list`가 아닌 `ChatMessageList`(pybind 클래스).

**아직 작동하는 기능:**
- `params.messages = [m1, m2]` - `py::implicitly_convertible<py::list, …>`
할당 시 Python 목록을 자동 변환합니다.
- `for m in params.messages` — 반복 프로토콜.
- `len(params.messages)`, `params.messages[i]`, `params.messages[i] = m`,
슬라이싱.
- `params.messages.append(...)`, `.extend(...)`, `.insert(...)`,
`.pop(...)`, `.clear()` — 모두 C++ 벡터 라이브로 푸시됩니다.

**파손된 사항(드물게):**
- `isinstance(x, list) → False`. 정말 일반 Python이 필요한 경우
목록, 구체화: `list(params.messages)`.
- `json.dumps(params.messages)` — 바인딩된 클래스는 직접적으로
JSON-직렬화 가능. 변환: `json.dumps([{"role": m.role,
"content": m.content} for m in params.messages])`.

`ChatMessage.image_urls`(`std::vector<std::string>`)는 *아닙니다*
마이그레이션 — `vector<string>`는 바인딩에서 너무 광범위하게 사용됩니다.
호출 사이트 청소 없이 전역 OPAQUE. `.append()` 무작동
문서화된 제한 사항으로 남아 있습니다. v0.6+ 후보를 통해
`add_image_url()` 편의 방법.

---

## 소스에서 빌드

### CMake 구성: Windows의 `Could NOT find SQLite3`

Windows 휠 빌드는 `-DNEOGRAPH_BUILD_SQLITE=OFF`를 설정합니다.
SQLite는 MSVC 런타임 전체에서 ABI와 호환되지 않습니다. 건물을 짓고 있는 경우
자신이 사용하기 위해 Windows의 소스에서 다음을 통해 SQLite를 설치하거나
vcpkg를 사용하거나 `-DNEOGRAPH_BUILD_SQLITE=OFF`를 명시적으로 전달합니다.

### CMake 구성: Linux의 `Could NOT find CURL`

선택적 종속성. 패키지 관리자를 통해 설치하십시오.

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

또는 비활성화: `-DNEOGRAPH_USE_LIBCURL=OFF`. libcurl이 없으면,
`SchemaProvider`의 `prefer_libcurl=True` 모드(HTTP/2)를 사용할 수 없습니다.
— 기본 ConnPool(HTTP/1.1)이 여전히 작동합니다.

### Pybind 바인딩이 정의되지 않은 참조와 연결되지 않습니다.

재실행하지 않고 새 코드를 가져온 후 `make`를 재실행할 가능성이 높습니다.
CMake. 빌드 디렉토리의 컴파일된 객체 파일은 다음의 기호를 참조합니다.
오래된 헤더. `make clean && make` 또는 삭제 및 재구성
빌드 디렉토리.

### 스크립트에서 A2A 서버를 시작할 때 `OPENAI_API_KEY not set`

`cppdotenv::auto_load_dotenv()`는 바이너리 내부에서 `.env`를 읽습니다.
호출하지만 런처 스크립트에서 분기된 하위 프로세스가 호출합니다.
**아님** 런처가 아직 내보내지 않은 어떤 것도 상속받지 않습니다. 만약에
귀하의 스크립트는 다음을 수행합니다.

```bash
./member_server 8101 ...   # forks before any env is set up
```

…각 어린이는 텅 빈 환경을 보고 시작하기를 거부합니다. 원천
먼저 런처에서 `.env`를 실행하므로 변수가 다음으로 내보내집니다.
포크를 수행하는 쉘:

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

요리책의 `scripts/run_session.sh`는 다음과 같은 전체 패턴을 보여줍니다.
형제 `.env`로 대체됩니다.

### 다중 인물/다중 프로세스 A2A: OpenAI 공급자를 공유할 위치

`OpenAIProvider::create_shared(cfg)` 사용(`shared_ptr<Provider>` 반환)
`create(cfg)` 대신(`unique_ptr` 반환) 공유양식은
`NodeFactory` 람다로 캡처 가능하고 모든 환경에서 재사용 가능
그래프 노드 및 A2A 요청 — `create()`의 `unique_ptr`는 강제로
수동으로 `release()`를 수행하고 다시 래핑해야 합니다.

---

## C++ 소비자 — `httplib.h` 매크로 일관성(로드 베어링, #16 발행)

**NeoGraph에 연결**되는 C++ 애플리케이션을 빌드하는 경우 AND
또한 자체 번역 단위로 `#include <httplib.h>`를 사용합니다(예:
자체 `httplib::Server` SSE 엔드포인트 실행), 다음을 포함하는 모든 TU
`<httplib.h>` MUST `#define CPPHTTPLIB_OPENSSL_SUPPORT` **이전**
포함합니다. 하나의 TU에서도 매크로가 누락되면 자동으로
처음으로 `getaddrinfo` 내부의 SEGV
`SchemaProvider::complete_stream`가 LLM 엔드포인트에 도달합니다.

### 왜 이런 일이 발생합니까?

`cpp-httplib`는 헤더 전용입니다. `httplib::ClientImpl` 클래스는 다음과 같습니다.
**조건부로 더 커짐** `CPPHTTPLIB_OPENSSL_SUPPORT`가 정의된 경우
(SSL 관련 멤버를 얻습니다. 레이아웃은 ~8바이트만큼 이동합니다.) 왜냐하면
라이브러리의 함수는 모두 `inline`이며 링커는 하나를 유지합니다.
인라인 함수별로 인스턴스화하고 중복 항목을 삭제합니다. 두 개라면
바이너리의 TU는 다양한 `ClientImpl` 레이아웃에 대해 컴파일됩니다.
(하나는 매크로를 정의하고 다른 하나는 정의하지 않았기 때문에) 링커는
하나의 정의; *다른* TU의 컴파일 측에서는 다음 멤버에 액세스합니다.
잘못된 오프셋 — 전형적인 ODR 위반입니다. 부패가 시작됩니다
인접 필드(예: `proxy_host_`는 결국 오프셋에서 읽음)
실제로는 `path_`의 꼬리입니다.) 그리고 `httplib::ClientImpl::create_client_socket`
와일드 `proxy_host_.c_str()`를 사용하여 "프록시 사용" 경로로 분기됩니다.
→ `getaddrinfo` → `internal_strlen` → SEGV.

### 징후

ASan에서:

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

ASan 없음: gdb를 통한 동일한 스택, 와일드 포인터 값 포함
*텍스트처럼 보일 수 있습니다*(잘못된 오프셋에 있는 바이트 모두 가능)
슬롯 — ASan에서는 일반적으로 `0xBE` 격리 포이즌입니다. 없이
ASan은 다음과 같이 디코딩되는 초기화되지 않은 스택 콘텐츠일 수 있습니다.
JSON / UTF-8 조각이 있고 메모리 손상처럼 *보입니다*
실제 범인 - 이것이 오해의 소지가 있는 증상입니다.)

### 고치다

`<httplib.h>`를 포함하는 모든 TU에서:

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

또는 CMake에서 전역적으로(선호 — 전체에 걸쳐 일관성을 보장합니다.
전체 목표):

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

자신의 httplib 사용이 필요한 경우에만 매크로가 무해합니다.
`Server` (`SSLClient` 아님) — 회원 **추가**만 합니다. 아무것도 아님
실제로 SSL를 수행해야 합니다.

### ASan 없이 감사하는 방법

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

`<httplib.h>`의 포함 사이트 앞에 다음이 **있지** 않은 경우
`#define CPPHTTPLIB_OPENSSL_SUPPORT`(동일한 TU에서 또는
컴파일 플래그 정의), 그것은 거의 확실히 버그입니다.

### NeoGraph가 이 문제를 해결할 수 없는 이유

NeoGraph의 자체 .cpp 파일은 모두 매크로를 일관되게 정의합니다. 그만큼
위반은 다운스트림 TU도 httplib.h를 가져올 때만 발생합니다.
*매크로 없이*. 컴파일 타임에 이를 감지하려면 다음이 필요합니다.
(a) 공개 헤더에 `httplib::ClientImpl`를 노출하는 NeoGraph
(우리는 의도적으로 그렇게 하지 않습니다. httplib는 `SchemaProvider.cpp` 내부에 있습니다.)
또는 (b) 번역 전반에 걸친 구조체 크기의 링크 시간 `static_assert`
C++에서는 지원하지 않는 단위입니다. 함정을 문서화하는 것이 최선이다
우리는 할 수 있다; 이 섹션은 문서입니다. #16 문제가 종료되었습니다.

---

## 토폴로지 JSON를 구축하는 tool/editor가 엔진에서 표류합니다.

### 징후

생성기 GUI 또는 시각적 블록 편집기를 작성(또는 사용)했습니다.
NeoGraph 토폴로지 JSON. 노드 유형, 감속기 또는 조건을 제공했습니다.
그런 다음 엔진은 `Unknown node type:`를 사용하여 `compile()`에서 거부합니다.
/ `Unknown reducer:` / `Unknown condition:` — 또는 사용자가 그린 가지
조용히 절대 발사되지 않습니다.

### 왜 이런 일이 발생합니까?

도구의 팔레트는 손으로 관리되었으며 NeoGraph보다 뒤쳐졌습니다.
실제로 링크된 버전입니다. 분기 케이스는 고전적인 최상위 수준입니다.
`conditional_edges` 회귀(v0.1.0–v0.1.7에서는 자동으로 삭제됨,
v0.1.8 수정) — 해당 블록을 내보내는 도구는 해당 블록이
로더→컴파일 왕복.

### 고치다

팔레트를 손으로 관리하지 마십시오. 엔진은 기계가 읽을 수 있는
정확히 허용되는 스키마 — 도구를 여기에 고정합니다.

- C++: `neograph::graph::NodeFactory::instance().export_schema()`.
- CLI: `./example_export_schema > schema.json`
(`examples/52_export_schema.cpp`).
- 파이썬: `neograph_engine.export_schema()` → dict.

문서에는 `neograph_version`가 포함되어 있습니다. 도구를 사용하여 다음과 비교하게 하세요.
캐시된 스키마를 확인하고 불일치에 대해 경고합니다. `node_types`는 무엇이든 반영합니다.
호출시 `NodeFactory`에 등록되어 있으므로 사용자 정의를 등록하십시오.
노드 types/reducers/conditions *내보내기 전*, 정확히 당신이 하는 대로
`compile()` 이전. (배경: #56를 발행하세요.)

---

## 엄격한 토폴로지 검증

### 징후

`compile()`는 `strict topology validation failed (schema_version 1)`를 던졌습니다.
`$: unknown or unconsumed key 'conditionnal_edges'`와 같은 키 나열,
`nodes.X.barrier: 'wait_for' is missing or empty` 또는
`translation validation failed: compiled graph does not round-trip`.

### 왜 이런 일이 발생합니까?

토폴로지는 이를 엄격하게 선택하는 `"schema_version": 1`를 선언합니다.
컴파일: 컴파일러가 소유한 모든 객체의 모든 키는 다음과 같아야 합니다.
파서가 *소비*합니다. 아무도 사용하지 않은 키는 거의 항상 오타입니다
(`conditionnal_edges`, `max_retry`, `promt`) 또는 엔진 구성
그렇지 않으면 **자동으로 중단**됩니다. — 뒤에 있는 실패 모드
v0.1.0–v0.1.7 `conditional_edges` 회귀. 왕복
(translation-validation) 오류는 컴파일된 그래프가 다음과 같이 다시 방출되었음을 의미합니다.
JSON는 더 이상 입력과 일치하지 않습니다. 컴파일러가 손실되거나 다시 연결되었습니다.
메시지에는 정확히 무엇인지 나열되어 있습니다.

### 고치다

- 나열된 키를 수정합니다. 각 오류에는 JSON 경로가 포함됩니다.
- 주석과 편집자 메타데이터는 주석 네임스페이스에 속합니다.
`_` 또는 `x-`(예: `_comment`, `x-studio-pos`)로 시작하는 키는 다음과 같습니다.
항상 허용되며 검증되지 않습니다.
- 장벽에는 비어 있지 않은 `wait_for` 배열이 필요합니다. 인라인 조건부
에지는 `routes`를 통해 라우팅하므로 해당 `to`는 작동하지 않습니다.
`routes`에 대상을 지정하거나 삭제하세요.
- 선언된 구성 스키마 *와 함께* 등록된 사용자 정의 노드 유형
(3-인수 `register_type`)는 닫힌 세계에서 확인됩니다. 추가하다
유형을 선택 해제하려면 스키마에 `"additionalProperties": true`를 추가하세요.
- 과거의 관대한 구문 분석으로 돌아가려면 다음을 제거하세요.
`schema_version` — 알 수 없는 키는 다시 무시되고 왕복됩니다.
불일치는 stderr에서만 경고합니다. 새로운 문서는 엄격하게 유지되어야 합니다.

---

## 버그 신고

증상이 위와 같지 않은 경우:

1. `pip install --upgrade neograph-engine`를 먼저 실행하세요. 많은 문제가 발생합니다.
패치 수준 수정.
2. 최소 재생산 캡처:
   - 그래프 정의
   - 사용 중인 노드 유형
   - 정확한 `engine.run(...)` 호출
   - `result.execution_trace` 및 (스트리밍하는 경우) 본 이벤트
3. 플랫폼, Python 버전, `neograph_engine.__version__`를 기록해 두세요.
4. Open an issue at <https://github.com/fox1245/NeoGraph/issues>.

버그가 특정 LLM 엔드포인트에 대해서만 나타나는 경우에도 문의해 주세요.
와이어 레벨 모양(OpenAI의 경우 `example_responses_envelope`)을 포함합니다.
응답; 해당되는 경우 원시 HTTP 추적의 경우 `tcpdump`/`wireshark`).
