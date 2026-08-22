<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ko source_sha256=14c932ce835be59435fe30b831344894d899490a1478a3bd34f442e9113414da -->
# 자가 진화 챗봇

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**챗봇 하니스가 런타임에 사용자 행동을 기반으로 *자체* 토폴로지를 재구성합니다. 이 기능은 NeoGraph에만 있는 기능입니다. LangGraph는 런타임에 그래프를 재구성할 수 없습니다.**

[multi_tenant_chatbot](../multi_tenant_chatbot/) 요리책의 자연스러운 확장입니다. 해당 요리책은 고객 하니스가 *고정*되어 있고, 이 요리책은 *진화*합니다. 동일한 컴파일 캐시 + thread_id 격리를 사용하며, LLM 심사 단계가 하나 더 추가됩니다.

## 두 가지 데모

| 파일 | 시나리오 | 비용 | 벽시계 시간 |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1명 × 5턴 — 최소 진화 메커니즘 데모 | ~$0.003 | 16초 |
| [server_multi.cpp](server_multi.cpp) | **고객 5명 × 5턴 — 각각의 개별 진화 타임라인 + 창발적 클러스터** | ~$0.02 | 7분 |

빌드 / 실행 (모두):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## 왜 오직 NG만이 이것을 할 수 있는가

| 시도 | LangGraph | NeoGraph |
|---|---|---|
| 고객별로 다른 하네스 | ❌ StateGraph = Python 객체 | ✅ graph_def JSON 행 |
| Harness는 런타임에 스스로 형태를 재구성함 | ❌ 모듈 리로드 + 진행 중 상태 손실 | **✅ DB UPDATE 1회 + 다음 요청 시 새 엔진 컴파일** |
| 고객 1,000명의 서로 다른 그래프 1,000개 | ❌ 고객별 프로세스 = 80 GB | ✅ 단일 프로세스 / 고유 형태 캐시 |
| 창발적 클러스터 발견 | N/A | **✅ graph_def 해시 분포 = 고객 행동 클러스터** |

LangChain/LangGraph의 StateGraph는 Python 클래스 인스턴스 — pickle은 import 경로도 함께 번들링하므로, 런타임 노드/엣지 재구성에는 Python 모듈 리로드가 필요하고 진행 중 대화 상태가 손실됨. **NG의 graph-as-JSON은 진화 = JSON 수정 한 번을 의미.**

## Core 메커니즘

각 턴이 끝날 때, OpenRouter를 통해 고정된 DeepSeek 모델이 LLM 심판 역할을 수행: 대화 기록 + 현재 토폴로지를 확인하고 가장 적합한 답을 한 단어로 응답:

- `simple` — LLM 호출 1회, 짧고 직접적인 답변 (사실적 Q에 적합)
- `reflexive` — LLM 호출 3회 (초안 → 비평 → 최종) (정확성 추구에 적합)
- `fanout` — 병렬 LLM 관점 3개 → 병합 (다중 관점 요구사항에 적합)

판단이 다를 경우, 고객 DB의 graph_def를 제자리에서 업데이트합니다. 다음 턴은 새 토폴로지를 사용합니다 — **배포 0회, 재시작 0회, 진행 중 상태 보존**.

```cpp
std::string suggested = llm_judge_topology(
    provider, customer.history, customer.topology_name);

if (suggested != customer.topology_name) {
    customer.topology_def  = topo_registry[suggested]();   // New graph_def
    customer.topology_name = suggested;
    // Cache sees new hash next turn and automatically compiles new engine.
    // Real production: DB UPDATE customer_graphs SET graph_def = ...
}
```

## 데모 1 — Alice 1 Person (server.cpp)

5회에 걸친 점진적 진화. 사용자는 사실 질문 → 다중 관점 질문으로 자연스럽게 이동하며, 하네스는 단순 → fan-out 진화를 따릅니다.

```
── Turn 1 [topology=simple] ──
User: What is a cloud?
Bot:  A cloud is a visible mass of condensed water vapor...
[Evaluating harness fit...] judge → simple

── Turn 3 [topology=simple] ──
User: Now explain blockchain to me — I want both the
      technical view and the economic view.
Bot:  **Technical View:** Blockchain is a decentralized digital ledger...
[Evaluating harness fit...] judge → fanout
  ⟹ EVOLVE: simple → fanout (in-place, deploy 0)

── Turn 4-5 [topology=fanout] ──
... multi-perspective response after ...

Evolution timeline:
  Turn 0:  simple   (initial)
  Turn 3:  fanout   (evolved)
```

## 데모 2 — Multi-Customer (server_multi.cpp) ⭐

**실제 영향력이 여기에 있습니다.** 5명의 고객이 서로 다른 행동 패턴을 보이며, 각각 별도의 진화 타임라인을 가집니다. 창발적 클러스터 발견 데모.

각 고객의 행동 패턴 가설 + 실제 결과:

| 고객 | 행동 패턴 | 가설 | 실제 진화 | 검증 |
|---|---|---|---|---|
| **alice** | 점진적(사실적 → 다중 시각) | 중간에 fan-out | `simple → fanout(t3)` | ✅ |
| **bob** | 사실만 ("X는 무엇인가?" × 5) | simple 유지됨 | `simple` 전체 5턴 | ✅ |
| **charlie** | 정확성 추구("답변 검증") | 반사적 | `simple → reflexive(t1)` 즉시 | ✅ |
| **david** | 처음부터 "X vs Y 다각도 비교" | 빠른 fan-out | `simple → fanout(t1)` 즉시 | ✅ |
| **eve** | 혼합(사실적 ↔ 다각도 ↔ 신중한 진동) | 진동 위험 | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **진동(oscillation)** | ✅ |

### 요약 결과

```
=== Aggregate stats ===
Customers:           5
Total turns:         25
Total main LLM:      51
Total judge LLM:     25
Total LLM calls:     76
Wall time:           424 s
Peak RSS:            18.99 MB
Compile cache size:  3   ← 5 customers → 3 distinct engine

=== Final topology distribution ===
  fanout:    3 customers  (alice, david, eve)
  reflexive: 1 customer   (charlie)
  simple:    1 customer   (bob)
```

### 핵심 관찰

1. **행동 패턴 가설 4/5 정확히 검증됨** — 인간의 예측된 진화 경로와 LLM 심판의 실제 진화 결정이 정확히 일치함. 즉, **LLM 심판이 사용자 의도 전환을 확실히 감지함**.

2. **Eve의 진동이 실제로 관찰됨 ⚠️** — 발화 [사실적 → 다중 → 사실적 → 신중 → 사실적]이 토폴로지 진동 [단순 → fan-out → fan-out(유지) → 반사적 → fan-out]을 유발합니다. **데이터로 검증된 안티-플래핑 가드가 필요합니다**(쿨다운 또는 히스테리시스 강화 필요).

3. **창발적 클러스터 발견** — 5명의 고객의 다양한 발화 패턴이 자연스럽게 **3개의 토폴로지 클러스터**로 분류됩니다. 컴파일 캐시 크기 = 3 = 고유 클러스터 개수.

**이것이 진정으로 흥미로운 창발 속성입니다** — NG의 그래프-로서-데이터는 자연스럽게 고객 행동 클러스터 발견 메커니즘이 됩니다. graph_def 분포 = 고객 행동의 본질적인 클러스터 형태.

4. **메모리 효율성** — 5명의 고객 → 3개의 엔진. 2명의 고객의 엔진 메모리는 캐시 공유로 절약됩니다. **1000명의 고객으로 확장할 때, 고유 형태가 약 10개로 수렴하면 엔진 메모리는 거의 일정하게 유지됩니다 → 실제 1000명 이상의 다중 테넌트가 단일 프로세스에 맞습니다.**

5. **순차 시뮬레이션은 벽 시계 기준 7분만 걸립니다** — 운영 환경에서 각 고객은 독립적이므로 병렬화가 가능합니다. 5명의 고객 병렬 처리 = 약 1.5분 + 컴파일 캐시는 동시 접근에 안전하므로(`std::shared_mutex`) 레이스가 없습니다.

## 운영 시나리오 — 실제 구현

```sql
CREATE TABLE customer_graphs (
    customer_id   TEXT PRIMARY KEY,
    graph_def     JSONB NOT NULL,
    topology_name TEXT,
    updated_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE customer_evolution_log (
    id            SERIAL PRIMARY KEY,
    customer_id   TEXT REFERENCES customer_graphs(customer_id),
    turn          INT,
    from_topology TEXT,
    to_topology   TEXT,
    judge_reason  TEXT,
    evolved_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE customer_sessions (
    thread_id     TEXT PRIMARY KEY,
    customer_id   TEXT,
    history       JSONB,
    updated_at    TIMESTAMPTZ DEFAULT NOW()
);
```

각 요청 처리 흐름:

```cpp
auto& cust    = db.fetch_customer(customer_id);  // graph_def + topology_name
auto  engine  = cache.get_or_compile(cust.graph_def, ctx);  // Hash-based cache
auto  history = db.fetch_history(thread_id);     // Session isolation key
RunConfig rcfg;
rcfg.thread_id = thread_id;
rcfg.input = {{"messages", history + user_msg}};
auto result = engine->run(rcfg);

db.append_history(thread_id, user_msg, result);

if (turn % EVAL_INTERVAL == 0) {
    auto suggested = llm_judge_topology(provider, history, cust.topology_name);
    if (suggested != cust.topology_name && !in_cooldown(cust)) {
        db.update_customer_graph(customer_id, topo_registry[suggested](),
                                  suggested);
        db.log_evolution(customer_id, turn, cust.topology_name, suggested);
    }
}
```

## 향후 확장

- **안티-진동 가드** — Eve 사례를 처리합니다. 지난 N턴 내에 진화했으면 잠금, 또는 히스테리시스(현재 토폴로지가 다음 후보보다 N% 낮지 않으면 변경하지 않음).
- **LLM 생성 graph_def** — 현재는 3개의 사전 정의된 토폴로지에서 선택합니다. 더 야심차게는, LLM이 graph_def JSON을 처음부터 생성할 수 있습니다. [`the-beast/`](../the-beast/)cookbook은 동일한 모델 작성 토폴로지와 컴파일/검증 게이트를 보여줍니다.
- **병렬 고객 처리** — 순차 데모는 7분이고, 고객당 병렬 처리 = 약 1.5분입니다. `asio::thread_pool` + 컴파일 캐시를 직접 사용합니다.
- **A/B 프레임워크** — 동일 고객에 대해 2개의 토폴로지를 동시 운영하고, 응답 만족도로 승자를 결정합니다. graph_id로 고정 분할을 수행합니다.
- **CheckpointStore 통합** — Postgres + 위 SQL 스키마로 실제 운영 준비를 합니다.
- **적응형 진화 속도** — 고객 기록 안정성에 따라 평가-간격을 조정합니다(안정적 = 10턴마다, 불안정 = 매턴).

## Core 메시지

> **자기 진화 + 다중 테넌트 조합이 NG의 진정한 본질입니다.** 5명의 고객이 3개의 엔진에 저장되고, 2개의 체크포인트가 유지됩니다. 고유한 토폴로지 수가 안정화되면서 서브에이전트 파생을 통해 메모리 오버헤드가 감소합니다. A/B 실험, 히스테리시스, 병렬 처리, LLM 생성 graph_def 파생을 통해 확장성, 메커니즘 지속성, 안정성, 공정성을 보장합니다.
> 스스로를 구축하는 것"은 NG의 그래프-데이터 패러다임으로 **실질적으로 구현 가능**합니다.
> LLM이 자체 하네스를 출력 → DB UPDATE → 즉시 적용 — 폐쇄된 경로를 위한
> NG는 이 시장에서 **유일한 플레이어**입니다.
>
> *"고객 5명 × 턴 5회 = 19MB / 3 distinct engine / emergent cluster
> 발견 / 진동 진단. 실제 자기 개선형 멀티 테넌트 에이전트 인프라를 위한 출발점입니다.
> 인프라."*
