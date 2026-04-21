# Stage 3 — asio 기반 Full Async Refactor 설계

작성: 2026-04-19 (feat/async-api)
전제: Stage 1 timer PoC (c356e0f) + Stage 2 HTTP PoC (b008c11) 완료.
`bench_async_fanout` / `bench_async_http` 결과로 **Go 결정**.

---

## 0. 목적과 비목적

### 목적 (Stage 3이 끝나면 해결되는 것)
- 1K 이상 동시 agent 호스팅 가능 — 현재 thread-per-agent는 ~1K에서 깨짐.
- HTTP / DB / MCP I/O wait 동안 스레드 점유 없음 → 메모리 5-7× 절감.
- `run()` 이 executor 위에서 실행 가능 → 외부 event loop 통합 (e.g. 웹서버).

### 비목적 (Stage 3 에서 건드리지 않음)
- 1K 미만 agent 유즈케이스의 절대 µs 개선 — 현재도 충분함.
- Python/TS 바인딩. 임베디드 C++ API 유지.
- 분산/멀티프로세스. 단일 프로세스 내 스케일링만.
- 사용자 Tool 인터페이스 변경 — Tool::call() 은 동기 유지 (사용자 부담 최소화).

---

## 1. 현재 상태 스냅샷

### 동기 I/O 의존 지점 (Stage 3 전환 대상)

| 레이어 | 파일 | 라인 수 | 의존 라이브러리 |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP (generic) | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx (동기) |
| 병렬 노드 실행 | `src/core/graph_executor.cpp` | 520 | Taskflow (CPU pool) |
| Engine loop | `src/core/graph_engine.cpp` | 529 | — (동기 run loop) |

### 이미 존재하는 async 자산
- `deps/asio/` — standalone asio 1.30.2 vendored.
- `include/neograph/async/http_client.h` — PoC async_post (HTTP/1.1 only).
- `src/async/{async_smoke,http_client}.cpp` — coroutine 동작 확인.
- benchmark 2종 — regression 측정 하네스로 재사용.

### Breaking surface
- `Provider::complete()` — 52개 호출처 (예제 + 테스트 합산).
- `CheckpointStore::save/load/list` — 29개 호출처.
- `GraphEngine::run() / run_stream() / resume()` — 예제 26개 전부 사용.
- `MCPClient::rpc_call()` — 7개 예제 (03, 20–24).

---

## 2. 타겟 아키텍처

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
   └─── 공유 asio::io_context (하나, N worker threads) ──┘
```

### 핵심 결정
1. **io_context 는 하나** — executor 주입 가능하지만 기본은 프로세스 전역 싱글톤.
2. **Provider / CheckpointStore / MCPClient 는 coroutine native** — 동기 메서드는 `run_sync()` wrapper 안에서만 제공.
3. **Tool 은 동기 유지** — 사용자 부담 최소. 내부에서 `co_await asio::post(thread_pool, ...)` 로 offload.
4. **Taskflow 는 유지**, executor 는 asio 기반으로 교체. 기존 fan-out 코드 경로 보존.
   - 대안: Taskflow 완전 제거 + asio::co_spawn 으로 교체. Semester 4 에서 결정.

---

## 3. Semester 분할 (6-10주 → 4 semester)

각 semester = 2주 내외. Semester 끝마다 build green + bench regression 측정.

### Semester 1 — Async HTTP 기반 완성 (1.5주)

목표: LLM call 을 co_await 로 바꿀 수 있을 만큼 HTTP 클라이언트 완성.

| # | 작업 | 파일 | 추정 |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS 지원 | `src/async/http_client.cpp` | 2일 |
| 1.2 | Keep-alive connection pool | `src/async/conn_pool.{h,cpp}` (신규) | 2일 |
| 1.3 | 재시도 가능한 transport 에러 분류 | `include/neograph/async/http_errors.h` (신규) | 0.5일 |
| 1.4 | SSE (streaming) 파서 | `src/async/http_client.cpp` | 1일 |
| 1.5 | Redirect, timeout, Retry-After 추출 | 위와 동일 | 1일 |
| 1.6 | bench_async_http 재실행 (TLS 경로) | `benchmarks/bench_async_http.cpp` | 0.5일 |

**완료 기준**:
- `async_post` / `async_post_stream` 두 API 로 LLM wire 요구사항 전부 커버.
- Keep-alive on 상태 5K concurrent 에서 bench_async_http 결과가 Stage 2 대비 동등 이상.
- 단위 테스트 — TLS handshake / pool reuse / SSE 재조립.

### Semester 2 — Provider & MCP async 전환 (2주)

목표: HTTP 호출하는 세 레이어 (openai, schema, mcp) 를 async 화. 기존 동기 API 는 wrapper 로 유지.

| # | 작업 | 파일 | 추정 |
|---|---|---|---|
| 2.1 | `Provider::complete_async` 추가 (pure virtual) | `include/neograph/provider.h` | 0.5일 |
| 2.2 | 동기 `complete()` = `run_sync(complete_async())` 기본 구현 | 위 | 0.5일 |
| 2.3 | OpenAIProvider async 구현 | `src/llm/openai_provider.cpp` | 1일 |
| 2.4 | SchemaProvider async 구현 (분량 큰 작업) | `src/llm/schema_provider.cpp` | 3일 |
| 2.5 | RateLimitedProvider — Retry-After 기반 co_await sleep | `src/llm/rate_limited_provider.cpp` | 1일 |
| 2.6 | MCPClient async HTTP path | `src/mcp/client.cpp` | 1일 |
| 2.7 | MCP stdio async (asio::posix::stream_descriptor) | 위 | 2일 |
| 2.8 | 기존 provider/mcp 테스트 전부 green | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | 포함 |

**완료 기준**:
- 모든 기존 테스트 green (async 내부, sync wrapper 호출).
- 신규 테스트: 동일 io_context 위에 수천 provider call 동시 실행.
- 예제는 아직 동기 facade 로 동작 — 변경 없음.

### Semester 3 — Async CheckpointStore + Engine coroutine (2.5주)

목표: GraphEngine 이 coroutine 으로 동작. Postgres 백엔드가 pipeline async mode 활용.

| # | 작업 | 파일 | 추정 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async` (pure virtual) | `include/neograph/graph/checkpoint.h` | 0.5일 |
| 3.2 | InMemoryStore / SQLite async wrapper | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1일 |
| 3.3 | libpq pipeline async 모드 — libpqxx 탈피 | `src/core/postgres_checkpoint.cpp` 재작성 | 5일 |
| 3.4 | `GraphNode::execute_async` 추가 (Task<NodeResult>) | `include/neograph/graph/node.h` | 1일 |
| 3.5 | 내장 노드 4종 async 구현 (LLMCall, ToolDispatch, IntentClassifier, Subgraph) | `src/core/graph_node.cpp` | 2일 |
| 3.6 | `GraphEngine::run_async` — 코루틴화 | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3일 |
| 3.7 | Taskflow fan-out → asio::experimental::parallel_group | `graph_executor.cpp` | 2일 |
| 3.8 | bench_neograph 재측정 — regression 제로 확인 | `benchmarks/bench_neograph.cpp` | 1일 |

**완료 기준**:
- `run_async()` 와 `run()` 둘 다 존재. 동기는 wrapper.
- Postgres checkpoint 64-thread 벤치 결과 유지 또는 개선.
- 예제 26개 — 아무것도 바뀌지 않음 (동기 facade 덕에).
- 신규 테스트: 10K concurrent run() 이 2GB RAM 이하에서 완주.

### Semester 4 — Migration, Tool async, 정리 (1.5주)

목표: 사용자가 async 이득을 실제로 얻을 수 있는 마이그레이션 경로 확립.

| # | 작업 | 파일 | 추정 |
|---|---|---|---|
| 4.1 | 예제 중 고동시성 후보 1-2 개 async 화 (e.g. 05_parallel_fanout, 26_postgres) | `examples/` | 1일 |
| 4.2 | Tool 을 async offload 하는 helper — `AsyncTool` adapter | `include/neograph/tool.h` | 1일 |
| 4.3 | 문서 — async guide, migration checklist | `docs/ASYNC_GUIDE.md` 신규 | 1일 |
| 4.4 | NEXT_SESSION.md / README 업데이트 | — | 0.5일 |
| 4.5 | Taskflow 의존 제거 여부 최종 판단 | — | 0.5일 |
| 4.6 | CI 에 bench_async_* regression gate | `.github/workflows/` | 1일 |
| 4.7 | Major version bump → 2.0.0 | `CMakeLists.txt` 등 | 0.5일 |

**완료 기준**:
- master 머지. 브랜치 종료.
- `NeoGraph 2.0` 릴리즈 노트에 breaking change 목록.
- 동기 예제 26개 전부 green, 새 async 예제 1-2개 추가.

---

## 4. Breaking change 매트릭스

| API | 변경 | 마이그레이션 비용 | 해결 전략 |
|---|---|---|---|
| `Provider::complete` | 동기 유지, `complete_async` 신설 | 없음 | pure virtual 로 두 개 선언, 기본 구현으로 상호 연결 |
| `GraphNode::execute` | 동기 유지, `execute_async` 신설 | 커스텀 노드는 영향 없음 | 동일 |
| `GraphEngine::run` | 동기 유지, `run_async` 신설 | 없음 | sync facade |
| `CheckpointStore::save` | 동기 유지, `save_async` 신설 | 커스텀 저장소 영향 없음 | 동일 |
| `PostgresCheckpointStore` | libpqxx → libpq 직접 | 사용자에 API 변경 없음 | 내부 교체 |
| `MCPClient::rpc_call` | 동기 유지, `rpc_call_async` 신설 | 없음 | sync facade |

**결론: 사용자가 넘어가도 동기 API 전부 남음**. 2.0 bump 는 내부 의존성 (libpqxx drop) + coroutine 요구로 인한 C++20 강제 때문.

---

## 5. 리스크 레지스터

| 리스크 | 영향 | 완화 |
|---|---|---|
| libpq pipeline mode 가 checkpoint 쓰기 패턴에 부적합 | Semester 3 차질 | Semester 3 초기 2일 spike 로 검증. 부적합 시 libpq 동기 호출 + `asio::post(thread_pool)` 로 fallback — 성능 이득 절반 수준이지만 완료 가능 |
| asio::ssl + Anthropic/OpenAI endpoint ALPN 이슈 | Semester 1 지연 | Stage 2 bench 때 HTTP 만 썼음. Semester 1 초기에 실 endpoint smoke test 먼저 |
| Taskflow ↔ asio executor 통합 난이도 | Semester 3 연장 | Taskflow 유지하고 병렬 노드 내부만 coroutine 으로 — 완전 제거는 semester 4 선택 사항 |
| Sync facade 의 `run_sync(coro)` 가 deadlock (io_context 단일 스레드 환경) | 런타임 버그 | 사용자 facade 는 항상 io_context 에 워커 스레드 1개 이상 보장하는 guard 로 감쌈 |
| 동기/비동기 경로 양쪽 테스트 → 테스트 수 2배 | 유지보수 비용 | 파라미터라이즈드 테스트 1회 정의, 두 경로 자동 실행 |
| 예제 26개 리그레션 | 릴리즈 지연 | 예제는 async 전환 안 함 (Semester 4 에서 선택적으로만). 동기 facade 통과면 OK |

---

## 6. 검증 게이트 (semester 공통)

각 semester 완료 시 반드시:

1. `cmake --build build -j` — warning 0.
2. `ctest -j` — 172+ 테스트 전원 green.
3. `benchmarks/bench_neograph` — 기존 세 지표 (single run µs, 1-thread PG, 64-thread PG) regression 5% 이내.
4. `benchmarks/bench_async_http` — Stage 2 대비 동등 이상.
5. `benchmarks/bench_async_fanout` — 50K 타이머 6× 유지.
6. ASan / TSan 빌드 green (`build-asan`, `build-tsan` 이미 존재).
7. Commit 은 `feat(async)` 접두어 + Co-Authored-By.

---

## 7. 스케줄 요약

| 주차 | Semester | 주요 deliverable |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | bench 재측정, Provider async 시작 |
| W3 | 2 | SchemaProvider, MCP async |
| W4 | 2→3 | CheckpointStore async, libpq 재작성 시작 |
| W5 | 3 | libpq pipeline, 내장 노드 async |
| W6 | 3 | Engine coroutine, executor 전환 |
| W7 | 3→4 | bench regression 게이트 |
| W8 | 4 | 예제 1-2개, 문서, CI gate |

총 **8주** — 6-10주 범위 중앙값. 리스크 완화 필요 시 W5 / W6 에서 1주씩 확장 여유.

---

## 8. 시작 전 마지막 체크

다음 세션 진입 시 확인:

- [ ] `feat/async-api` 브랜치에 계속? 또는 `feat/async-stage3` 분기? → **이 브랜치 연속 추천**. PoC → Stage 3 연속성 보존.
- [ ] Semester 1 spike 대상 host 결정 (api.openai.com vs. api.anthropic.com TLS 동작 차이).
- [ ] libpq pipeline mode 문서 사전 검토.
- [ ] Taskflow 제거 vs 유지 초기 입장 (기본: 유지, Semester 4 재검토).

---

**다음 액션**: Semester 1.1 착수 — `src/async/http_client.cpp` 에 asio::ssl 계층 추가.
