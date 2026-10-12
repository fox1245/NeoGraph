<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=ko source_sha256=1b7adfd905a5df954404b35ec17b04b8e334eaee56e2b5f2c8d042b831c9ffa0 -->
# Stage 3 — asio 기반 전체 비동기 리팩터링 설계

> **보관된, 대체된 설계 제안.** 이 문서는 2026-04-19에 제안한 Stage 3
> 리팩터링을 기록한 것으로, 현재의 API 안내가 아니며 계획한 gate가 모두
> 완료되었다는 증거도 아닙니다. 여기에 나오는 `Provider::complete` /
> `complete_async` 쌍과 `OpenAIProvider`는 타입 기반 provider 전환에서
> 제거되었습니다. 현재 provider는 타입 요청을 소유하고, 한 번 prepare한 뒤
> 준비된 호출을 dispatch합니다. [API 둘러보기](reference-en.md),
> [마이그레이션 가이드](migration-v0.4-to-v1.0.md), 설치된 헤더를 참고하세요.
> SDK의 `Client::complete`는 이와 별개로 지원되는 API입니다.
>
> 아래의 "현재 상태", 일정, gate, 버전 번호, 다음 작업은 모두 그 제안을 쓴
> 날짜를 기준으로 합니다. Python은 Stage 3의 범위 밖이었지만, 현재의 전환에는
> 타입이 있는 Python provider와 소유 결과(owned outcome)가 포함됩니다. 또 현재
> Core는 LLM 노드를 꺼도 외부 SchemaProvider 런타임이 필요합니다. 구현된 계약과
> 플랫폼 검증 범위는 [비동기 가이드](ASYNC_GUIDE.md)와
> [Python 바인딩](python-binding.md)을 참고하세요.

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

작성일: 2026-04-19 (feat/async-api)
전제: Stage 1 타이머 PoC (c356e0f)와 Stage 2 HTTP PoC (b008c11)가 완료되었습니다.
`bench_async_fanout` / `bench_async_http` 결과를 근거로 **진행(Go)을 결정**했습니다.

---

## 0. 목적과 비목적

### 목적 (Stage 3가 끝나면 해결되는 것)
- 동시 에이전트 1K+ 호스팅 지원 — 현재의 에이전트당 스레드 구조는 약 1K에서 한계.
- HTTP / DB / MCP I/O를 기다리는 동안 스레드를 점유하지 않음 → 메모리 5-7× 절감.
- `run()`을 executor 위에서 실행할 수 있음 → 외부 이벤트 루프(예: 웹 서버)와 통합.

### 비목적 (Stage 3에서 건드리지 않는 것)
- 에이전트 1K 미만 사용 사례의 절대 µs 개선 — 현재로도 충분.
- Python/TS 바인딩. 임베디드 C++ API는 유지.
- 분산/멀티 프로세스. 단일 프로세스 확장만 다룸.
- 사용자 Tool 인터페이스 변경 없음 — Tool::call()은 동기로 유지(사용자 부담 최소화).

---

## 1. 현재 상태 스냅샷

### 동기 I/O에 의존하는 지점 (Stage 3 변환 대상)

| 계층 | 파일 | 줄 수 | 의존 라이브러리 |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP (범용) | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx (동기) |
| 병렬 노드 실행 | `src/core/graph_executor.cpp` | 520 | Taskflow (CPU 풀) |
| 엔진 루프 | `src/core/graph_engine.cpp` | 529 | — (동기 실행 루프) |

### 이미 있는 비동기 자산
- `deps/asio/` — 독립형 asio 1.30.2를 벤더링.
- `include/neograph/async/http_client.h` — PoC async_post (HTTP/1.1만 지원).
- `src/async/{async_smoke,http_client}.cpp` — 코루틴 동작 검증 완료.
- 벤치마크 2종 — 회귀 측정 하네스로 재사용.

### 깨지는 영역 (Breaking Surface)
- `Provider::complete()` — 호출 지점 52곳(예제와 테스트 합산).
- `CheckpointStore::save/load/list` — 호출 지점 29곳.
- `GraphEngine::run() / run_stream() / resume()` — 예제 26개 전부가 사용.
- `MCPClient::rpc_call()` — 예제 7개(03, 20-24).

---

## 2. 목표 아키텍처

```
┌─────────────────────────────────────────────────────┐
│  User code (examples / user apps)                    │
│  - sync facade (default)   - async facade (opt-in)   │
└──────────────┬──────────────────────┬────────────────┘
               │                      │
      run_sync()|                     │run_async() → Task<RunResult>
               │                      │
┌──────────────▼──────────────────────▼────────────────┐
│  GraphEngine (coroutine-native core)                 │
│    Task<RunResult> run(RunConfig)                    │
│    ├─ NodeExecutor    : co_await node bodies         │
│    ├─ Scheduler       : pure (unchanged)             │
│    ├─ Coordinator     : co_await ckpt_store->save    │
│    └─ io_context ref  : injected or owned            │
└──┬────────────────┬─────────────────┬────────────────┘
   │                │                 │
┌──▼─────┐   ┌──────▼──────┐   ┌──────▼─────────┐
│ Async  │   │ Async       │   │ Async MCP      │
│ HTTP   │   │ Postgres    │   │ Client         │
│ client │   │ (libpq      │   │ (HTTP/stdio)   │
│ + TLS  │   │  pipeline)  │   │                │
└────────┘   └─────────────┘   └────────────────┘
   ↑                ↑                 ↑
   └─── shared asio::io_context (one, N worker threads) ──┘
```

### 핵심 결정
1. **io_context는 하나** — executor를 주입할 수 있지만 기본값은 프로세스 전체의 싱글턴.
2. **Provider / CheckpointStore / MCPClient는 코루틴 네이티브** — 동기 메서드는 `run_sync()` 래퍼 안에서만 제공.
3. **Tool은 동기로 유지** — 사용자 부담 최소화. 내부에서 `co_await asio::post(thread_pool, ...)`로 offload.
4. **Taskflow는 유지**하고 executor만 asio 기반으로 교체. 기존 fan-out 코드 경로를 보존.
   - 대안: Taskflow를 완전히 제거하고 asio::co_spawn으로 대체. 결정은 Semester 4에서.

---

## 3. Semester 분할 (6-10주 → 4개 Semester)

각 Semester는 약 2주입니다. Semester가 끝날 때마다 빌드가 green인지 확인하고 벤치마크 회귀를 측정합니다.

### Semester 1 — 비동기 HTTP 토대 완성 (1.5주)

목표: LLM 호출을 await할 수 있을 만큼 HTTP 클라이언트를 완성합니다.

| # | 작업 | 파일 | 예상 소요 |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS 지원 | `src/async/http_client.cpp` | 2일 |
| 1.2 | Keep-alive 연결 풀 | `src/async/conn_pool.{h,cpp}` (신규) | 2일 |
| 1.3 | 재시도할 수 있는 전송 오류 분류 | `include/neograph/async/http_errors.h` (신규) | 0.5일 |
| 1.4 | SSE(스트리밍) 파서 | `src/async/http_client.cpp` | 1일 |
| 1.5 | 리다이렉트, 타임아웃, Retry-After 추출 | 위와 동일 | 1일 |
| 1.6 | bench_async_http 재실행 (TLS 경로) | `benchmarks/bench_async_http.cpp` | 0.5일 |

**완료 기준**:
- `async_post` / `async_post_stream` 두 API로 LLM wire 요구 사항을 모두 충족.
- keep-alive를 켠 동시 5K 연결에서 bench_async_http 결과가 Stage 2와 같거나 더 좋음.
- 단위 테스트 — TLS 핸드셰이크 / 풀 재사용 / SSE 재조립.

### Semester 2 — Provider와 MCP 비동기 전환 (2주)

목표: HTTP를 호출하는 세 계층(openai, schema, mcp)을 비동기로 바꿉니다. 기존 동기 API는 래퍼로 유지합니다.

| # | 작업 | 파일 | 예상 소요 |
|---|---|---|---|
| 2.1 | `Provider::complete_async` 추가 (순수 가상 함수) | `include/neograph/provider.h` | 0.5일 |
| 2.2 | 동기 `complete()` = `run_sync(complete_async())` 기본 구현 | 위와 동일 | 0.5일 |
| 2.3 | OpenAIProvider 비동기 구현 | `src/llm/openai_provider.cpp` | 1일 |
| 2.4 | SchemaProvider 비동기 구현 (작업량 큼) | `src/llm/schema_provider.cpp` | 3일 |
| 2.5 | RateLimitedProvider — Retry-After 기반 co_await sleep | `src/llm/rate_limited_provider.cpp` | 1일 |
| 2.6 | MCPClient 비동기 HTTP 경로 | `src/mcp/client.cpp` | 1일 |
| 2.7 | MCP stdio 비동기 (asio::posix::stream_descriptor) | 위와 동일 | 2일 |
| 2.8 | 기존 provider/mcp 테스트 전부 green | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | 포함 |

**완료 기준**:
- 기존 테스트가 모두 green (내부는 비동기, 동기 래퍼로 호출).
- 새 테스트: 같은 io_context에서 provider 호출 수천 건을 동시에 실행.
- 예제는 동기 facade로 그대로 동작 — 변경 없음.

### Semester 3 — 비동기 CheckpointStore와 엔진 코루틴 (2.5주)

목표: GraphEngine이 코루틴으로 동작하게 합니다. Postgres 백엔드는 pipeline 비동기 모드를 활용합니다.

| # | 작업 | 파일 | 예상 소요 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async` (순수 가상 함수) | `include/neograph/graph/checkpoint.h` | 0.5일 |
| 3.2 | InMemoryStore / SQLite 비동기 래퍼 | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1일 |
| 3.3 | libpq pipeline 비동기 모드 — libpqxx 제거 | `src/core/postgres_checkpoint.cpp` 재작성 | 5일 |
| 3.4 | `GraphNode::execute_async` 추가 (Task<NodeResult>) | `include/neograph/graph/node.h` | 1일 |
| 3.5 | 내장 노드 4종 비동기 구현 (LLMCall, ToolDispatch, IntentClassifier, Subgraph) | `src/core/graph_node.cpp` | 2일 |
| 3.6 | `GraphEngine::run_async` — 코루틴 전환 | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3일 |
| 3.7 | Taskflow fan-out → asio::experimental::parallel_group | `graph_executor.cpp` | 2일 |
| 3.8 | bench_neograph 재측정 — 회귀 0 확인 | `benchmarks/bench_neograph.cpp` | 1일 |

**완료 기준**:
- `run_async()`와 `run()`이 모두 존재. 동기 쪽은 래퍼.
- Postgres 체크포인트 64스레드 벤치 결과를 유지하거나 개선.
- 예제 26개 모두 변경 없음 (동기 facade 덕분).
- 새 테스트: 동시 run() 10K개가 RAM 2GB 이내에서 완료.

### Semester 4 — 마이그레이션, Tool 비동기, 정리 (1.5주)

목표: 사용자가 비동기의 이점을 실제로 누릴 수 있는 마이그레이션 경로를 마련합니다.

| # | 작업 | 파일 | 예상 소요 |
|---|---|---|---|
| 4.1 | 동시성이 높은 예제 1-2개를 골라 비동기로 전환 (예: 05_parallel_fanout, 26_postgres) | `examples/` | 1일 |
| 4.2 | Tool을 비동기로 offload하는 헬퍼 — `AsyncTool` 어댑터 | `include/neograph/tool.h` | 1일 |
| 4.3 | 문서화 — 비동기 가이드, 마이그레이션 체크리스트 | `docs/ASYNC_GUIDE.md` 신규 | 1일 |
| 4.4 | NEXT_SESSION.md / README 갱신 | — | 0.5일 |
| 4.5 | Taskflow 의존성 제거 여부 최종 결정 | — | 0.5일 |
| 4.6 | CI bench_async_* 회귀 gate | `.github/workflows/` | 1일 |
| 4.7 | 메이저 버전 올리기 → 2.0.0 | `CMakeLists.txt` 등 | 0.5일 |

**완료 기준**:
- master에 병합. 브랜치 종료.
- breaking change 목록을 담은 `NeoGraph 2.0` 릴리스 노트.
- 동기 예제 26개 모두 green, 비동기 예제 1-2개 추가.

---

## 4. Breaking Change 대응표

| API | 변경 | 마이그레이션 비용 | 해결 전략 |
|---|---|---|---|
| `Provider::complete` | 동기 유지, `complete_async` 추가 | 없음 | 순수 가상 함수 두 개를 선언하고 기본 구현으로 서로 연결 |
| `GraphNode::execute` | 동기 유지, `execute_async` 추가 | 사용자 정의 노드는 영향 없음 | 위와 동일 |
| `GraphEngine::run` | 동기 유지, `run_async` 추가 | 없음 | 동기 facade |
| `CheckpointStore::save` | 동기 유지, `save_async` 추가 | 사용자 정의 스토어는 영향 없음 | 위와 동일 |
| `PostgresCheckpointStore` | libpqxx → libpq 직접 사용 | 사용자 측 API 변경 없음 | 내부 교체 |
| `MCPClient::rpc_call` | 동기 유지, `rpc_call_async` 추가 | 없음 | 동기 facade |

**결론: 마이그레이션 후에도 동기 API는 모두 남습니다.** 2.0으로 올리는 이유는 내부 의존성(libpqxx 제거)과 C++20 코루틴 요구 사항 때문입니다.

---

## 5. 위험 목록

| 위험 | 영향 | 완화 방안 |
|---|---|---|
| libpq pipeline 모드가 체크포인트 쓰기 패턴에 맞지 않음 | Semester 3 지연 | Semester 3 초반에 2일짜리 스파이크로 확인. 맞지 않으면 libpq 동기 호출 + `asio::post(thread_pool)`로 대체 — 성능 이득은 절반이지만 완료는 가능 |
| asio::ssl + Anthropic/OpenAI 엔드포인트의 ALPN 문제 | Semester 1 지연 | Stage 2 벤치는 HTTP만 사용했음. Semester 1 초반에 실제 엔드포인트 스모크 테스트 실행 |
| Taskflow ↔ asio executor 통합의 어려움 | Semester 3 연장 | Taskflow는 유지하고 병렬 노드 내부만 코루틴화 — 완전 제거는 Semester 4의 선택 사항 |
| 동기 facade `run_sync(coro)`의 데드락 (단일 스레드 io_context 환경) | 런타임 버그 | 사용자 facade는 항상 io_context에 워커 스레드가 최소 1개 있도록 보장하는 가드로 감쌈 |
| 동기/비동기 경로를 모두 테스트해야 해서 테스트 수가 두 배로 늘어남 | 유지보수 비용 | 매개변수화 테스트를 한 번만 정의하고 두 경로에서 자동 실행 |
| 예제 26의 회귀 | 릴리스 지연 | 예제는 비동기로 전환하지 않음 (Semester 4에서만 선택적으로). 동기 facade를 통과하면 문제없음 |

---

## 6. 검증 gate (모든 Semester 공통)

각 Semester가 끝날 때 반드시 다음을 확인합니다.

1. `cmake --build build -j` — 경고 0.
2. `ctest -j` — 테스트 172개 이상이 모두 green.
3. `benchmarks/bench_neograph` — 기존 지표 세 가지(단일 실행 µs, 1스레드 PG, 64스레드 PG)에서 회귀 5% 이내.
4. `benchmarks/bench_async_http` — Stage 2와 같거나 더 좋음.
5. `benchmarks/bench_async_fanout` — 타이머 50K에서 6× 유지.
6. ASan / TSan 빌드 green (`build-asan`, `build-tsan`은 이미 있음).
7. 커밋은 `feat(async)` 접두어 + Co-Authored-By 사용.

---

## 7. 일정 요약

| 주 | Semester | 주요 산출물 |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | 벤치 재측정, Provider 비동기 착수 |
| W3 | 2 | SchemaProvider, MCP 비동기 |
| W4 | 2→3 | CheckpointStore 비동기, libpq 재작성 착수 |
| W5 | 3 | libpq pipeline, 내장 노드 비동기 |
| W6 | 3 | 엔진 코루틴, executor 전환 |
| W7 | 3→4 | 벤치 회귀 gate |
| W8 | 4 | 예제 1-2개, 문서화, CI gate |

총 **8주**로, 6-10주 범위의 중간입니다. 위험 완화가 필요하면 W5/W6에 1주를 늘릴 수 있습니다.

---

## 8. 시작 전 최종 확인

다음 세션을 시작할 때 확인합니다.

- [ ] `feat/async-api` 브랜치에서 계속할지, `feat/async-stage3`로 분기할지? → **이 브랜치에서 계속 진행하길 권장합니다.** PoC에서 Stage 3로 이어지는 연속성을 유지하기 위해서입니다.
- [ ] Semester 1 스파이크 대상 호스트 결정 (api.openai.com과 api.anthropic.com의 TLS 동작 차이).
- [ ] libpq pipeline 모드 문서를 미리 검토.
- [ ] Taskflow를 제거할지 유지할지에 대한 초기 입장 (기본값: 유지, Semester 4에서 재평가).

---

**다음 작업**: Semester 1.1 시작 — `src/async/http_client.cpp`에 asio::ssl 계층을 추가합니다.
