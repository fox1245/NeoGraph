<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=ko source_sha256=8baffd5ea72da3575627014a32aaaf9257b389214daebaf2c5336633f74ff996 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# 멀티 테넌트 챗봇 서버


**하나의 프로세스는 N개의 서로 다른 에이전트 토폴로지를 통해 N명의 고객에게 동시에 서비스를 제공합니다.**
측정: 동시 실제 OpenAI 호출 1000개 / 고객 6명 / 토폴로지 3개 /
**최대 29MB/오류 0개**.

> "100명의 고객이 각각 다른 서비스를 사용하는 챗봇 SaaS를 어떻게 운영합니까?
> 에이전트 하네스 — ReAct, Plan&Execute, 팬아웃, 반사…?"
>
> LangGraph 답변: 고객당 하나의 프로세스를 시작하십시오. 100명의 고객 = 100개의 프로세스 =
> ~8GB + supervisord/k8s.
>
> NeoGraph 답변: **DB에 고객당 하나의 graph_def JSON 행을 넣고,
> 하나의 컴파일 캐시 항목만 있으면 완료됩니다.** 프로세스당 <30MB에 맞습니다.

이 요리책은 해당 구조를 최소한으로 구현한 것입니다.

## 대본

6명의 고객이 3가지 다른 토폴로지를 사용합니다.

|고객|토폴로지|모양|LLM call/request|
|---|---|---|---|
|앨리스, 밥|**단순한**|`start → respond → end`| 1 |
|찰리, 데이비드|**반사적**|`start → draft → critique → final → end`| 3 |
|이브, 프랭크|**팬아웃**|`start → [perspective_a, _b, _c] → merge → end`|3(병렬)|

각 고객의 graph_def는 인라인 JSON로 정의되지만 실제 생산에서는
Postgres `customer_graphs.graph_def JSONB` 행으로 직접 저장하십시오.

핵심 코드 흐름([server.cpp](server.cpp:140-176)):

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

동일한 토폴로지를 공유하는 고객은 엔진 인스턴스를 공유합니다. 고객 그래프
수정하면 해시가 변경되어 새 엔진 컴파일 + 캐시가 트리거됩니다.

## 빌드/실행

### 모의 공급자 버전(외부 종속성 없음)

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

OpenAI 키 없이 작동합니다. NG 엔진 용량 측정(동시 요청 1000개/
컴파일 캐시 적중률/메모리).

### 라이브 LLM 버전(OpenRouter DeepSeek)

```bash
# 저장소 루트의 .env에 OPENROUTER_API_KEY가 있어야 합니다.
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**비용은 provider와 사용량에 따라 다릅니다**(고정 DeepSeek 경로를 통한 LLM 호출 2330회).

## 측정

|측면|모의 1000 요청|라이브 100 요청|**라이브 1000 요청**|
|---|---|---|---|
|확인/오류| 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
|벽 시간|5ms|11.5초|50.2초|
|평균 대기 시간|39μs|1.58초|1.4초|
|최대 대기 시간|2.99ms|9.33초|14.4초|
|처리량|200K RPS|8.67 RPS|**19.9 RPS**|
|**피크 RSS**|**5.25MB**|**21.9MB**|**29.25MB**|
|컴파일 캐시 적중률| 99.7% | 94% | **99.4%** |
|독특한 엔진| 3 | 6 | 6 |
**측정 환경**: WSL2 / 32-thread asio thread pool / 단일 호스트 /
실제 OpenRouter DeepSeek API 호출.

주요 번호:

- **1000개의 동시 진행 중인 LLM 코루틴 + 연결 메모리 비용 ≒
29MB**. 요청 100개 → 요청 1000개 증가 +7MB ⇒ 추가 연결당 ~8KB.
asio 코루틴 + httplib SSL 연결 풀의 조합입니다.
- **동시 1000개에서 오류 0** — NG는 속도 제한/네트워크 지터/TLS를 정상적으로 흡수합니다.
재시도 없는 핸드셰이크 지터. 공급자 측 스로틀은 다음을 통해 강화될 수 있습니다.
`RateLimitedProvider` 래퍼.
- **캐시 적중률 99.4%** — 다음과 같은 경우 더 많은 고객이 있어도 적중률이 유지됩니다.
토폴로지 수는 동일하게 유지됩니다. **1000 고객 시나리오 메모리도 최대 30MB로 유지됩니다**.

## LangGraph 비교 - 실제 의미

LangGraph를 사용하여 동일한 다중 테넌트 시나리오를 시도하면 다음과 같은 병목 현상이 발생합니다.

|측면|네오그래프|랭그래프 추정|
|---|---|---|
|하나의 프로세스에서 N 고객 × N 토폴로지|**예**(29MB / 1000요청)|아니요 — StateGraph는 Python 객체, serialization/storage입니다(피클 번들 가져오기 경로).|
|고객별 토폴로지 변경|DB 행 1개 UPDATE|코드홍보 → CI → 배포주기|
|버전 격리(고객 A의 v1/v2 그래프 공존)|`graph_versions` 행 추가|Python 네임스페이스 충돌, 해킹 필요|
|다중 프로세스 적용|불필요한|고객 = 프로세스 공통 패턴|
|메모리(6고객)|29MB|6 × ~80MB = 480MB(LG 유휴 기준)|
|메모리 (1000 고객)|~30MB(캐시는 변경되지 않음)|**~80GB**(고객당 프로세스)|
|운영 인프라|하나의 바이너리|gunicorn / Supervisord / k8s + 프로세스 오케스트레이션|

**프로세스당 30MB 대 80GB.** 2700× 차이가 실제 멀티 테넌트 챗봇의 본질입니다.
SaaS 운영.

## 실제 시나리오 - 어디까지 갈 수 있나요?

`t2.micro`(1 vCPU / 1GB RAM, ~$0.01/hour)에서 가능한 시나리오:

|대본|NG 메모리 추정|t2.micro에서 가능합니까?|
|---|---|---|
|동시 활성 기내 LLM 100개 + 고객 100명 × 토폴로지 3개|~10MB|✅ 넉넉함 ~990MB 남음|
|동시 기내 1000명 + 고객 1000명 × 토폴로지 10개|~30MB|✅ 넉넉함 ~970MB 남음|
|동시 기내 10,000명 + 고객 10,000명 × 토폴로지 100개|~85MB|✅ 넉넉함 ~915MB 남음|
|동시 기내 100,000명 + …|~800MB|⚠️ RAM가 거의 다 사용되었습니다|

물론 OpenRouter rate limit이 처리량의 상한입니다.

**핵심은 한계 고객 비용이 ~0**이라는 것입니다.

> t2.micro의 LangGraph 100명의 고객을 위한 1GB = 100개의 프로세스 =
> 8GB 필요 → 인스턴스 자체를 시작할 수 없습니다. **m5.2xlarge(32GB, ~$0.38/hour)가 필요합니다.**
>
> NG와 동일한 작업 = **단일 t2.micro($0.01/hour). 38배 인프라
> 비용 차이.**

## 핫스왑 데모

`server.cpp` 끝은 `simple` → `fanout`에서 앨리스 토폴로지의 내부 변경을 보여줍니다.
다음 요청을 즉시 처리합니다. 0 배포 주기, 0 다시 시작. 실제 생산
고객은 웹 UI에서 그래프 JSON를 편집 → DB 저장 → 다음 요청에서 새 토폴로지를 사용하게 됩니다.

## 향후 개선 사항

- **CheckpointStore 통합** — 현재 요청별로 기록을 입력으로 전달합니다.
Postgres CheckpointStore를 사용하면 thread_id당 자동 지속성이 유지됩니다.
- **고정 공급자** — 모든 고객이 동일한 OpenRouter DeepSeek 모델을 사용하며,
  `NodeContext::provider`에는 고객별 컨텍스트를 담을 수 있습니다.
- **스트리밍 응답** — 토큰 수준의 경우 `input.stream_cb` + SSE가 포함된 `run(input)`
스트리밍. 스트림 콜백과 함께 NG의 `run(NodeInput)` 경로를 직접 사용하세요.
- **A/B 실험 프레임워크** — graph_def 해시 + customer_id 고정 분할을 통한 트래픽 분할.
코드 패턴을 직접 확장합니다.
- **스트리밍 + 통합 취소** — 클라이언트 연결 해제 시 아웃바운드 LLM 소켓을 중단합니다.
NG의 `RunConfig::cancel_token`를 직접 연결하세요.

## 핵심 메시지

> *"6000명의 고객 × 3개의 토폴로지 = 29MB. JSON 행 편집 1개 = 배포 없이 핫스왑.
> 1000개의 동시 실제 OpenAI 호출에서 오류가 0개입니다. 단일 t2.micro에서 작동 가능."*

이 한 줄은 성능 수치보다 NeoGraph의 더 영향력 있는 판매 포인트일 수 있습니다.
(`5.5 MB L3 fit / 1024 worker idle 31 MB`).
