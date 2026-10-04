<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=ko source_sha256=5b5b58f71f1129b96f7164783daefd9467d64ba6bc19df7d96ef3a99bfaf8fe6 -->
# 멀티테넌트 챗봇 서버

## 현재 소스 경계와 과거 측정

mock은 provider-free입니다. cache는 topology, model, instruction, extra configuration, provider name을 바인딩하지만 live cache의 tenant/provider capability key를 쓰지 않습니다. mock 재사용 결과는 production cross-tenant 격리 증명이 아닙니다.

C++ live는 타입 SDK 요청과 전체 불변 Outcome을 사용합니다. engine cache identity는
tenant/topology 및 캡처된 provider/model/host instruction에 연결됩니다. 신뢰 host의
tenant 선택, quota/store 경계, thread 격리는 유지됩니다. portable JSON 요약은 native authority가 아닙니다.
아래 수치/실행 기록은 과거 자료이며 새 전환의 실행 또는 live pass가 아닙니다.
live binary는 **1,000 request / 32 worker**를 고정하며 저비용 small-smoke flag가 없습니다.
단일 호출 검증으로 실행하지 마세요. live 키/네트워크와 별도 승인된 상당한 provider 비용이 필요합니다.
가격이나 zero-error를 보장하지 않습니다. 키/prompt/artifact는 비공개이며 raw native payload를 공개하지 않습니다.
provider retry는 한 명시적 layer, 기본 off이며 현재 public API에 예전 throttle-provider wrapper는 없습니다.

`server_live_llm.cpp`는 타입 provider factory에 경로별 timeout 180초를 전달합니다.
전역 기본값은 그대로이며 timeout이 불확실하거나 관찰된 호출의 재전송 권한은 아닙니다.

보존된 interface-3 model-free E2E에서 전용 mock workload 1,000 graph 요청·오류0·컴파일 topology3개·cache hit997을
관찰했고 Alice topology 교체는 기존 fanout engine을 재사용했습니다. 이는 topology load/cache 증거이며
production 인증·quota·semantic response·memory capacity 보장이 아닙니다. isolated-host CLI는
서로 다른 policy tuple2개를 출력했지만 graph를 실행하지 않았습니다.


**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**하나의 프로세스가 N명의 고객에게 각기 다른 N개의 에이전트 토폴로지로 동시에 서비스를 제공합니다.** 측정값: 1000개의 동시 실제 OpenAI 호출 / 6명의 고객 / 3개의 토폴로지 / **최대 29MB / 오류 0건**.

mock workload는 캡처된 설정이 동등할 때만 compiled engine을 공유합니다. 인증, tenant 선택, store와 quota는 host 책임입니다. 아래 과거 RSS는 그 리소스의 상한이 아닙니다.

이 쿡북은 해당 구조의 동작하는 최소 구현입니다.

## 격리 계약 (production 경계)

위 측정은 topology 공유 측정이며 production SaaS의 보안이나 용량 보장이 아닙니다. 인증된 ingress가 불변 `TenantScope`를 만들고 tenant별 provider/model policy, `GraphRegistry` snapshot, `ToolSet`, `ScopedStore`, `ScopedCheckpointStore`, Harness namespace와 `TenantQuota`를 제공합니다.

```cpp
auto backend_store = std::make_shared<InMemoryStore>(); // or a tenant DB
auto backend_checkpoints = std::make_shared<InMemoryCheckpointStore>();
TenantScope scope("tenant-a", "authz-a"); // trusted ingress only
ScopedStore store(scope, backend_store);
ScopedCheckpointStore checkpoints(scope, backend_checkpoints);
TenantQuota quota({.max_concurrency = 32, .max_queue = 64,
                   .max_model_tokens = 2'000'000, .max_artifacts = 1000});
```

같은 공개 thread/checkpoint/run/artifact ID는 tenant마다 다시 사용할 수 있습니다. scoped adapter는 private backend namespace에 매핑하고 잘못된 tenant 조회는 absence를 반환합니다. resume, replay, fork, cancellation과 dereference에 같은 ingress scope를 사용하세요.

`CatalogConfig::materialization_context_identity`는 provider/model policy, tool catalog, store와 registry snapshot을 식별하는 비밀이 아닌 host identity입니다. generation cache identity에 포함됩니다. credentials, authorization token이나 topology JSON을 identity/cache key/journal/log/diagnostic에 넣지 마세요. exact capability receipt가 의도적으로 동등한 경우에만 생략하세요.

mock는 topology 재사용을 보여주며 tenant/provider 격리, quota 또는 production memory를 증명하지 않습니다. production host는 tenant별 scoped engine/resource binding을 유지하고 직접 측정해야 합니다.

### 오프라인 isolated-host reference

```bash
cmake --build build --target cookbook_multi_tenant_isolated_host
./build/cookbook_multi_tenant_isolated_host
```

이 CLI는 서로 다른 topology, provider/model, tool, store와 quota policy tuple 두 개를 출력합니다. graph를 실행하지 않습니다. synthetic identity를 사용하고 network credential은 필요 없습니다. production ingress/control-plane은 배포 host가 구현해야 합니다.

## 시나리오

6명의 고객이 3개의 서로 다른 토폴로지를 사용합니다:

| 고객 | 토폴로지 | 형태 | 요청당 LLM 호출 |
|---|---|---|---|
| alice, bob | **simple** | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| 이브, 프랭크 | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3 (병렬) |

각 고객의 graph_def는 인라인 JSON으로 정의되지만, 실제 운영 환경에서는 Postgres `customer_graphs.graph_def JSONB` 행으로 직접 저장할 것입니다.

Core 코드 흐름 ([server.cpp](server.cpp)):

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // This provider-free style demo still binds all captured configuration.
        const std::string key = json::array({def, ctx.model, ctx.instructions,
            ctx.extra_config, ctx.provider_name}).dump();
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile (lock 밖에서, 다른 customer 차단 안 함).
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — 다른 thread 가 먼저 넣음
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = {{"messages", json::array({{{"role", "user"}, {"content", user_message}}})}};
auto result   = engine->run(cfg);
```

동일한 토폴로지를 공유하는 고객은 엔진 인스턴스를 공유합니다. 고객 그래프 수정은 해시를 변경하여 새 엔진 컴파일 + 캐시를 트리거합니다.

## 빌드 / 실행

### Mock 버전 (provider 호출 없음)

native 구성/link에는 키 없이도 SchemaProvider가 필요합니다. 먼저 `CMAKE_PREFIX_PATH` 또는 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 지정하세요.

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

OpenAI 키 없이 작동합니다. NG 엔진 용량을 측정합니다 (동시 요청 1000개 / 컴파일 캐시 적중률 / 메모리).

### 라이브 LLM 버전 (OpenRouter DeepSeek)

```bash
# .env must contain OPENROUTER_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**비용 ≈ 공급자에 따라 다름** (고정 DeepSeek 경로를 통한 호출 2330회).

## 측정값

| 측면 | Mock 1000 req | 실시간 100 req | **실시간 1000 req** |
|---|---|---|---|
| OK / 오류 | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
| 벽시계 시간 | 5 ms | 11.5 s | 50.2 s |
| 평균 지연 시간 | 39 µs | 1.58 s | 1.4 s |
| 최대 지연 시간 | 2.99 ms | 9.33 s | 14.4 s |
| 처리량 | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **Peak RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
| 컴파일 캐시 적중률 | 99.7% | 94% | **99.4%** |
| 개별 엔진 수 | 3 | 6 | 6 |

**측정 환경**: WSL2 / 32-스레드 asio 스레드 풀 / 단일 호스트 / 실제 OpenRouter DeepSeek API 호출.

핵심 수치:

- **동시 진행 중인 1000개 LLM 코루틴 + 커넥션 메모리 비용 ≈ 29 MB**. 100 req → 1000 req 증가 +7 MB ⇒ 추가 커넥션당 약 8 KB. asio 코루틴 + httplib SSL 커넥션 풀의 조합.
- 과거 1000 요청 실행의 오류 0건 기록은 현재 reliability 보장이 아닙니다.
- **캐시 적중률 99.4%** — 이 과거 workload의 관찰이며 1,000 production tenant의 메모리 용량을 보장하지 않습니다.

## LangGraph 비교 — 실제 의미

이 실행은 LangGraph를 벤치마크하지 않았습니다. LangGraph가 고객마다 프로세스 하나를 요구하지는 않습니다. 비교에는 같은 graph, store, provider 및 isolation policy가 필요합니다. 이전 process-per-customer 추정은 측정값이 아닙니다.

## 실제 시나리오 — 어디까지 가능한가

6명 고객의 과거 load로 cloud instance 용량이나 10,000/100,000 connection을 예측할 수 없습니다. 실제 인증, tenant별 리소스, store, provider route와 limit으로 production host를 측정하세요. SDK transport의 현재 검증 범위는 Linux/POSIX입니다.

## Hot-swap 데모

`server.cpp` 끝부분은 alice의 토폴로지가 `simple` → `fanout`로 제자리에서 변경되는 것을 보여주며, 즉시 다음 요청을 처리합니다. 배포 주기 0회, 재시작 0회입니다. 실제 운영 환경에서는 고객이 웹 UI에서 그래프 JSON을 편집 → DB 저장 → 다음 요청이 새 토폴로지를 사용하는 흐름이 됩니다.

## 향후 개선 사항

- **CheckpointStore 통합** — 현재는 요청별로 히스토리를 입력으로 전달합니다. Postgres CheckpointStore를 사용하면 thread_id별로 자동 영속화가 가능합니다.
- **Pinned Provider** — 모든 고객이 동일한 OpenRouter DeepSeek 모델을 사용합니다. `NodeContext::provider`는 고객별 컨텍스트를 계속 전달할 수 있습니다.
- **스트리밍 응답** — `run(input)`와 `input.stream_cb` + SSE를 결합한 토큰 수준 스트리밍. NG의 `run(NodeInput)` 경로를 스트림 콜백과 함께 직접 사용합니다.
- **A/B 실험 프레임워크** — graph_def 해시 + customer_id 고정(sticky) 분할을 통한 트래픽 분산. 코드 패턴을 직접 확장합니다.
- **스트리밍 + 취소 통합** — 클라이언트 연결 해제 시 아웃바운드 LLM 소켓을 중단합니다. NG의 `RunConfig::cancel_token`를 직접 연결합니다.

## Core 메시지

보존된 mock 증거는 1,000 request, compiled topology 3개와 cache hit 997입니다. isolated-host CLI는 policy reference이며 serving workload가 아닙니다. 위 live timing은 과거 기록이며 새 전환의 pass가 아닙니다.
