<!-- neograph-i18n: source=docs/ASYNC_STAGE3_DESIGN.md locale=ko source_sha256=504932a9848ba52c794d9ac51f4b7b599605bab11e2e679c4a2d11fd7f9d43e2 -->
# Stage 3 — asio 기반 전체 비동기 재설계

**Languages:** [English](ASYNC_STAGE3_DESIGN.md) | [한국어](ASYNC_STAGE3_DESIGN.ko.md) | [日本語](ASYNC_STAGE3_DESIGN.ja.md) | [简体中文](ASYNC_STAGE3_DESIGN.zh-CN.md)

작성일: 2026-04-19 (feat/async-api)
전제 조건: Stage 1 타이머 PoC (c356e0f) + Stage 2 HTTP PoC (b008c11) 완료.
`bench_async_fanout` / `bench_async_http` 결과에 기반한 **진행 결정**.

---

## 0. 목적과 비목적

### 목적 (Stage 3 종료 시 해결될 사항)
- 1K+ 동시 에이전트 호스팅 지원 — 현재 스레드-당-에이전트는 ~1K에서 한계.
- HTTP / DB / MCP I/O 대기 중 스레드 점유 없음 → 5–7× 메모리 절약.
- `run()`이 실행자(executor) 위에서 실행 가능 → 외부 이벤트 루프 통합(예: 웹 서버).

### 비목적 (Stage 3에서 다루지 않음)
- 1K 미만의 에이전트 사용 사례에 대한 절대 µs 개선 — 현재 충분.
- Python/TS 바인딩. 내장 C++ API 유지.
- 분산/다중 프로세스. 단일 프로세스 확장만.
- 사용자 Tool 인터페이스 변경 — Tool::call() 동기 유지(사용자 부담 최소화).

---

## 1. 현재 상태 스냅샷

### 동기 I/O 의존 지점 (Stage 3 변환 대상)

| 계층 | 파일 | 줄 수 | 의존 라이브러리 |
|---|---|---|---|
| LLM HTTP | `src/llm/openai_provider.cpp` | 228 | httplib |
| LLM HTTP (일반) | `src/llm/schema_provider.cpp` | 1400+ | httplib |
| MCP HTTP | `src/mcp/client.cpp` | 471 | httplib |
| DB | `src/core/postgres_checkpoint.cpp` | 692 | libpqxx (동기) |
| 병렬 노드 실행 | `src/core/graph_executor.cpp` | 520 | Taskflow (CPU 풀) |
| 엔진 루프 | `src/core/graph_engine.cpp` | 529 | — (동기 실행 루프) |

### 기존 비동기 자산
- `deps/asio/` — 독립형 asio 1.30.2 벤더링됨(vendored).
- `include/neograph/async/http_client.h` — PoC async_post (HTTP/1.1 전용).
- `src/async/{async_smoke,http_client}.cpp` — 코루틴 동작 검증 완료.
- 2개 벤치마크 유형 — 회귀 측정 도구로 재사용.

### 깨지는 표면(Breaking Surface)
- `Provider::complete()` — 52개 호출 지점 (예제 + 테스트 합계).
- `CheckpointStore::save/load/list` — 29개 호출 지점.
- `GraphEngine::run() / run_stream() / resume()` — 26개 예제 모두 사용.
- `MCPClient::rpc_call()` — 7개 예제 (03, 20–24).

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
1. **하나의 io_context** — 실행자 주입 가능, 기본값은 프로세스 전역 싱글톤.
2. **Provider / CheckpointStore / MCPClient는 코루틴-네이티브** — 동기 메서드는 `run_sync()` 래퍼 안에서만 제공.
3. **Tool은 동기 유지** — 사용자 부담 최소화. 내부적으로 `co_await asio::post(thread_pool, ...)`로 오프로드.
4. **Taskflow 유지**, 실행자를 asio 기반으로 교체. 기존 팬아웃 코드 경로 보존.
   - 대안: Taskflow 완전 제거 + asio::co_spawn 교체. Semester 4에서 결정.

---

## 3. 학기 분할 (6–10주 → 4학기)

각 학기 ≈ 2주. 학기 종료 시 빌드 통과 + 벤치 회귀 측정.

### Semester 1 — 비동기 HTTP 기초 완성 (1.5주)

목표: LLM 호출을 기다릴 수 있게(as awaitable) HTTP 클라이언트 완성.

| # | 과제 | 파일 | 예상 |
|---|---|---|---|
| 1.1 | asio::ssl HTTPS 지원 | `src/async/http_client.cpp` | 2일 |
| 1.2 | Keep-alive 연결 풀 | `src/async/conn_pool.{h,cpp}` (신규) | 2일 |
| 1.3 | 재시도 가능 전송 오류 분류 | `include/neograph/async/http_errors.h` (신규) | 0.5일 |
| 1.4 | SSE (스트리밍) 파서 | `src/async/http_client.cpp` | 1일 |
| 1.5 | 리다이렉트, 타임아웃, Retry-After 추출 | 위와 동일 | 1일 |
| 1.6 | bench_async_http 재실행 (TLS 경로) | `benchmarks/bench_async_http.cpp` | 0.5일 |

**완료 기준**:
- `async_post` / `async_post_stream` 두 API가 모든 LLM 전선 요구사항 커버.
- bench_async_http 결과가 keep-alive 활성화 시 5K 동시에서 Stage 2와 같거나 더 나음.
- 단위 테스트 — TLS 핸드셰이크 / 풀 재사용 / SSE 재조립.

### Semester 2 — Provider & MCP 비동기 변환 (2주)

목표: HTTP 호출을 하는 세 계층(openai, schema, mcp)을 비동기화. 기존 동기 API는 래퍼로 보존.

| # | 과제 | 파일 | 예상 |
|---|---|---|---|
| 2.1 | `Provider::complete_async` 추가 (순수 가상) | `include/neograph/provider.h` | 0.5일 |
| 2.2 | 동기 `complete()` = `run_sync(complete_async())` 기본 구현 | 위와 동일 | 0.5일 |
| 2.3 | OpenAIProvider 비동기 구현 | `src/llm/openai_provider.cpp` | 1일 |
| 2.4 | SchemaProvider 비동기 구현 (대규모 작업) | `src/llm/schema_provider.cpp` | 3일 |
| 2.5 | RateLimitedProvider — Retry-After 기반 co_await sleep | `src/llm/rate_limited_provider.cpp` | 1일 |
| 2.6 | MCPClient 비동기 HTTP 경로 | `src/mcp/client.cpp` | 1일 |
| 2.7 | MCP stdio 비동기 (asio::posix::stream_descriptor) | 위와 동일 | 2일 |
| 2.8 | 모든 기존 provider/mcp 테스트 통과 | `tests/test_schema_provider_*`, `test_rate_limited_provider.cpp` | 포함 |

**완료 기준**:
- 모든 기존 테스트 통과 (비동기 내부, 동기 래퍼 호출).
- 새 테스트: 같은 io_context에서 수천 개의 provider 호출이 동시에 실행.
- 예제는 여전히 동기 파사드로 동작 — 변경 없음.

### Semester 3 — 비동기 CheckpointStore + 엔진 코루틴 (2.5주)

목표: GraphEngine이 코루틴으로 동작. Postgres 백엔드가 파이프라인 비동기 모드 활용.

| # | 과제 | 파일 | 예상 |
|---|---|---|---|
| 3.1 | `CheckpointStore::save_async / load_async` (순수 가상) | `include/neograph/graph/checkpoint.h` | 0.5일 |
| 3.2 | InMemoryStore / SQLite 비동기 래퍼 | `src/core/graph_checkpoint.cpp`, `src/core/sqlite_checkpoint.cpp` | 1일 |
| 3.3 | libpq 파이프라인 비동기 모드 — libpqxx 제거 | `src/core/postgres_checkpoint.cpp` 재작성 | 5일 |
| 3.4 | `GraphNode::execute_async` 추가 (Task<NodeResult>) | `include/neograph/graph/node.h` | 1일 |
| 3.5 | 내장 노드 4개 타입 비동기 구현 (LLMCall, ToolDispatch, IntentClassifier, Subgraph) | `src/core/graph_node.cpp` | 2일 |
| 3.6 | `GraphEngine::run_async` — 코루틴 변환 | `src/core/graph_engine.cpp`, `graph_executor.cpp` | 3일 |
| 3.7 | Taskflow 팬아웃 → asio::experimental::parallel_group | `graph_executor.cpp` | 2일 |
| 3.8 | bench_neograph 재측정 — 회귀 0 확인 | `benchmarks/bench_neograph.cpp` | 1일 |

**완료 기준**:
- `run_async()`와 `run()` 모두 존재. 동기는 래퍼.
- Postgres 체크포인트 64스레드 벤치 결과 유지 또는 개선.
- 26개 예제 모두 — 변경 없음 (동기 파사드 덕분).
- 새 테스트: 10K 동시 run()이 2GB RAM 내에서 완료.

### Semester 4 — 이전, Tool 비동기, 정리 (1.5주)

목표: 사용자가 실제로 비동기 이점을 누릴 수 있는 이전 경로 확립.

| # | 과제 | 파일 | 예상 |
|---|---|---|---|
| 4.1 | 높은 동시성 예제 후보 1-2개를 비동기 변환으로 선정 (예: 05_parallel_fanout, 26_postgres) | `examples/` | 1일 |
| 4.2 | Tool의 비동기 오프로드 도우미 — `AsyncTool` 어댑터 | `include/neograph/tool.h` | 1일 |
| 4.3 | 문서화 — 비동기 안내, 이전 체크리스트 | `docs/ASYNC_GUIDE.md` 신규 | 1일 |
| 4.4 | NEXT_SESSION.md / README 갱신 | — | 0.5일 |
| 4.5 | Taskflow 의존성 제거 최종 결정 | — | 0.5일 |
| 4.6 | CI bench_async_* 회귀 게이트 | `.github/workflows/` | 1일 |
| 4.7 | 주 버전 증가 → 2.0.0 | `CMakeLists.txt` 등 | 0.5일 |

**완료 기준**:
- master에 병합. 브랜치 종료.
- `NeoGraph 2.0` 릴리스 노트에 변경 사항 목록 포함.
- 26개 동기 예제 모두 통과, 1-2개 새 비동기 예제 추가.

---

## 4. 변경 사항 행렬

| API | 변경 | 이전 비용 | 해결 전략 |
|---|---|---|---|
| `Provider::complete` | 동기 유지, `complete_async` 추가 | 없음 | 두 순수 가상 선언, 기본 구현으로 상호 연결 |
| `GraphNode::execute` | 동기 유지, `execute_async` 추가 | 사용자 정의 노드 영향 없음 | 동일 |
| `GraphEngine::run` | 동기 유지, `run_async` 추가 | 없음 | 동기 파사드 |
| `CheckpointStore::save` | 동기 유지, `save_async` 추가 | 사용자 정의 저장소 영향 없음 | 동일 |
| `PostgresCheckpointStore` | libpqxx → 직접 libpq | 사용자 API 변경 없음 | 내부 교체 |
| `MCPClient::rpc_call` | 동기 유지, `rpc_call_async` 추가 | 없음 | 동기 파사드 |

**결론: 모든 동기 API는 이전 후에도 유지**. 2.0 범프는 내부 의존성(libpqxx 제거) + C++20 코루틴 요구사항 때문.

---

## 5. 위험 등록부

| 위험 | 영향 | 완화 |
|---|---|---|
| libpq 파이프라인 모드가 체크포인트 쓰기 패턴에 부적합 | Semester 3 지연 | Semester 3 초반 2일 스파이크로 검증. 부적합 시 libpq 동기 호출 + `asio::post(thread_pool)`로 후퇴 — 절반 성능 향상 달성하지만 완료 가능 |
| asio::ssl + Anthropic/OpenAI 엔드포인트 ALPN 문제 | Semester 1 지연 | Stage 2 벤치는 HTTP만 사용. Semester 1 초반에 실제 엔드포인트 스모크 테스트 실행 |
| Taskflow ↔ asio 실행자 통합 난이도 | Semester 3 연장 | Taskflow 유지, 병렬 노드 내부만 코루틴으로 — 완전 제거는 Semester 4 선택 사항 |
| 동기 파사드 `run_sync(coro)` 교착 (단일 스레드 io_context 환경) | 런타임 버그 | 사용자 파사드는 항상 io_context에 최소 1 작업자 스레드를 보장하는 보호 장치로 감쌈 |
| 동기/비동기 경로 모두 테스트 → 테스트 수 두 배 | 유지보수 비용 | 매개변수화된 테스트를 한 번 정의하고 두 경로 자동 실행 |
| 예제 26개 회귀 | 릴리스 지연 | 예제는 비동기 변환하지 않음 (Semester 4만 선택 사항). 동기 파사드 통과 시 OK |

---

## 6. 검증 게이트 (모든 학기 공통)

각 학기 완료 시 반드시:

1. `cmake --build build -j` — 경고 0.
2. `ctest -j` — 172+ 테스트 모두 통과.
3. `benchmarks/bench_neograph` — 기존 세 지표(단일 실행 µs, 1스레드 PG, 64스레드 PG)가 5% 회귀 이내.
4. `benchmarks/bench_async_http` — Stage 2와 같거나 더 나음.
5. `benchmarks/bench_async_fanout` — 50K 타이머 6× 유지.
6. ASan / TSan 빌드 통과 (`build-asan`, `build-tsan` 이미 존재).
7. 커밋은 `feat(async)` 접두사 + Co-Authored-By 사용.

---

## 7. 일정 요약

| 주 | 학기 | 주요 결과물 |
|---|---|---|
| W1 | 1 | TLS + keep-alive + SSE |
| W2 | 1→2 | 벤치 재측정, Provider 비동기 시작 |
| W3 | 2 | SchemaProvider, MCP 비동기 |
| W4 | 2→3 | CheckpointStore 비동기, libpq 재작성 시작 |
| W5 | 3 | libpq 파이프라인, 내장 노드 비동기 |
| W6 | 3 | 엔진 코루틴, 실행자 변환 |
| W7 | 3→4 | 벤치 회귀 게이트 |
| W8 | 4 | 예제 1-2, 문서화, CI 게이트 |

총 **8주** — 6-10주 범위의 중간점. 위험 완화 필요 시 W5/W6에서 1주 확장 가능.

---

## 8. 시작 전 최종 확인

다음 세션 진입 시 확인:

- [ ] `feat/async-api` 브랜치에서 계속? 아니면 `feat/async-stage3`로 분기? → **이 브랜치 계속 권장**. PoC → Stage 3 연속성 보존.
- [ ] Semester 1 스파이크 대상 호스트 결정 (api.openai.com vs. api.anthropic.com TLS 동작 차이).
- [ ] libpq 파이프라인 모드 문서 사전 검토.
- [ ] Taskflow 제거 vs 유지 초기 입장 (기본값: 유지, Semester 4에서 재평가).

---

**다음 작업**: Semester 1.1 시작 — `src/async/http_client.cpp`에 asio::ssl 계층 추가.