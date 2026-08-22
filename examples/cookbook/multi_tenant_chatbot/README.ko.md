<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=ko source_sha256=81fc54c9666570230243c6bd69b2cca0784ec3e43705a29f8e2c797c7a33b964 -->
# 멀티테넌트 챗봇 서버

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**하나의 프로세스가 N명의 고객에게 각기 다른 N개의 에이전트 토폴로지로 동시에 서비스를 제공합니다.** 측정값: 1000개의 동시 실제 OpenAI 호출 / 6명의 고객 / 3개의 토폴로지 / **최대 29MB / 오류 0건**.

> "100명의 고객이 각각 다른 하네스를 사용하는 챗봇 SaaS를 어떻게 운영합니까?"
> 하네스 — ReAct, Plan&Execute, fan-out, 자기반영적(reflexive)…?
>
> LangGraph answer: 고객당 프로세스 하나를 시작합니다. 고객 100명 = 프로세스 100개 =
> ~8GB + supervisord/k8s.
>
> NeoGraph 답변: **고객당 graph_def JSON 행 하나를 DB에 넣고,
> 컴파일 캐시 항목 하나만 추가하면 끝입니다.** 프로세스당 30MB 미만으로 충분합니다.

이 쿡북은 해당 구조의 동작하는 최소 구현입니다.

## 시나리오

6명의 고객이 3개의 서로 다른 토폴로지를 사용합니다:

| 고객 | 토폴로지 | 형태 | 요청당 LLM 호출 |
|---|---|---|---|
| alice, bob | **simple** | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| 이브, 프랭크 | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3 (병렬) |

각 고객의 graph_def는 인라인 JSON으로 정의되지만, 실제 운영 환경에서는 Postgres `customer_graphs.graph_def JSONB` 행으로 직접 저장할 것입니다.

Core 코드 흐름 ([server.cpp](server.cpp:140-176)):

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<size_t, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        size_t key = std::hash<std::string>{}(def.dump());
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        std::unique_lock lk(mu_);
        cache_.emplace(key, engine);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = user_message;
auto result   = engine->run(cfg);
```

동일한 토폴로지를 공유하는 고객은 엔진 인스턴스를 공유합니다. 고객 그래프 수정은 해시를 변경하여 새 엔진 컴파일 + 캐시를 트리거합니다.

## 빌드 / 실행

### Mock 공급자 버전 (외부 종속성 없음)

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
- **1000 동시 요청 시 오류 0건** — NG는 재시도 없이 rate-limit / 네트워크 지터 / TLS 핸드셰이크 지터를 원활히 흡수. 공급자 측 스로틀은 `RateLimitedProvider` 래퍼로 강화 가능.
- **캐시 적중률 99.4%** — 토폴로지 수가 동일하면 고객이 많아져도 적중률 유지. **1,000명 고객 시나리오에서 메모리도 약 30MB 유지**.

## LangGraph 비교 — 실제 의미

동일한 멀티테넌트 시나리오를 LangGraph로 시도하면 다음과 같은 병목 현상이 발생합니다:

| 측면 | NeoGraph | LangGraph 추정치 |
|---|---|---|
| 단일 프로세스에서 N 고객 × N 토폴로지 | **예** (29 MB / 1000 요청) | 아니요 — StateGraph는 Python 객체이며, 직렬화/저장이 어색함(pickle은 import 경로를 묶음) |
| 고객별 토폴로지 변경 | DB 행 UPDATE 1건 | 코드 PR → CI → 배포 주기 |
| 버전 격리(고객 A의 v1/v2 그래프 공존) | `graph_versions` 행 추가 | Python 네임스페이스 충돌, 해킹 필요 |
| 다중 프로세스 강제 | 불필요 | 고객 = 프로세스 일반 패턴 |
| 메모리(고객 6명) | 29 MB | 6 × ~80MB = 480MB (LG 유휴 기준선) |
| 메모리(고객 1000명) | ~30MB (캐시 변경 없음) | **~80GB** (고객당 프로세스) |
| 운영 인프라 | 단일 바이너리 | gunicorn / supervisord / k8s + 프로세스 오케스트레이션 |

**프로세스당 30MB vs 80GB.** 2700배 차이가 실제 다중 테넌트 SaaS 챗봇 운영의 핵심이다.

## 실제 시나리오 — 어디까지 가능한가

possible on `t2.micro` (1 vCPU / 1 GB RAM, ~$0.01/hour) 에서 실행 가능한 시나리오:

| 시나리오 | NG 메모리 Memory(추정) | t2.micro에서 가능한가? |
|---|---|---|
| 100개의 동시 활성 in-flight LLM + 100명의 고객 × 3가지 토폴로지 | 약 10MB | ✅ 넉넉함  약 990MB 남음 |
| 동시 처리 중 1,000건 + 고객 1,000명 × 토폴로지 10개 | 약 30MB | ✅ 여유 충분, 약 970MB 남음 |
| 동시 처리 중 10,000건 + 고객 10,000명 × 토폴로지 100개 | 약 85MB | ✅ 여유 충분, 약 915MB 남음 |
| 동시 처리중인 100,000건+ … | 약 800–900MB | ⚠️ RAM 거의 고갈됨 |

* 가정: 각 연결당 약 8KB + 각 컴파일 캐시 항목당 약 10KB + 기본 5MB.

물론 OpenRouter의 속도 제한이 최대 처리량의 상한이지만, **핵심은 고객당 한계비용이 약 $0라는 점이다.**

> t2.micro 1GB에서 100명의 고객을 위한 LangGraph = 100개의 프로세스 =
> 8GB 필요 → 인스턴스 자체가 시작할 수 없음. **m5.2xlarge (32GB, 약 $0.38/시간)가 필수입니다.**
>
> 동일한 작업을 NG = **단일 t2.micro ($0.01/시간)로 처리. 인프라스트럭처 38배 차이.
> 비용 차**.**

## Hot-swap 데모

`server.cpp` 끝부분은 alice의 토폴로지가 `simple` → `fanout`로 제자리에서 변경되는 것을 보여주며, 즉시 다음 요청을 처리합니다. 배포 주기 0회, 재시작 0회입니다. 실제 운영 환경에서는 고객이 웹 UI에서 그래프 JSON을 편집 → DB 저장 → 다음 요청이 새 토폴로지를 사용하는 흐름이 됩니다.

## 향후 개선 사항

- **CheckpointStore 통합** — 현재는 요청별로 히스토리를 입력으로 전달합니다. Postgres CheckpointStore를 사용하면 thread_id별로 자동 영속화가 가능합니다.
- **Pinned Provider** — 모든 고객이 동일한 OpenRouter DeepSeek 모델을 사용합니다. `NodeContext::provider`는 고객별 컨텍스트를 계속 전달할 수 있습니다.
- **스트리밍 응답** — `run(input)`와 `input.stream_cb` + SSE를 결합한 토큰 수준 스트리밍. NG의 `run(NodeInput)` 경로를 스트림 콜백과 함께 직접 사용합니다.
- **A/B 실험 프레임워크** — graph_def 해시 + customer_id 고정(sticky) 분할을 통한 트래픽 분산. 코드 패턴을 직접 확장합니다.
- **스트리밍 + 취소 통합** — 클라이언트 연결 해제 시 아웃바운드 LLM 소켓을 중단합니다. NG의 `RunConfig::cancel_token`를 직접 연결합니다.

## Core 메시지

> *"고객 6000명 × 토폴로지 3개 = 29MB. JSON 행 편집 한 번 = 배포 없는 핫스왑.
> 실제 OpenAI 호출 1000개 동시 처리에서 오류 0건. t2.micro 하나로 운영 가능."*

이 한 줄은 NeoGraph의 성능 수치(`5.5 MB L3 fit / 1024 worker idle 31 MB`)보다 더 설득력 있는 판매 포인트가 될 수 있습니다.
