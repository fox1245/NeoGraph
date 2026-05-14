# Self-evolving chatbot

**Chatbot harness 가 사용자 행동을 보고 *자기 자신의* topology 를
runtime 에 reshape 합니다. LangGraph 가 할 수 없는, NeoGraph 만 가능한
카테고리.**

[multi_tenant_chatbot](../multi_tenant_chatbot/) cookbook 의 자연스러운
확장 — 그쪽은 customer 별 harness 가 *고정*, 이쪽은 *진화*. 같은
compile cache + thread_id 격리 위에 LLM judge 한 단계 더 얹은 구조.

## 두 데모

| 파일 | 시나리오 | 비용 | Wall time |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1명 × 5 turn — 진화 메커니즘 minimal 시연 | ~$0.003 | 16 s |
| [server_multi.cpp](server_multi.cpp) | **5 customer × 5 turn — 각자 별도 evolution timeline + emergent cluster** | ~$0.02 | 7 min |

빌드 / 실행 (둘 다):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customer)
```

## 왜 NG 만 할 수 있나

| 시도 | LangGraph | NeoGraph |
|---|---|---|
| Customer 별 다른 harness | ❌ StateGraph = Python 객체 | ✅ graph_def JSON row |
| Harness 가 runtime 에 자기 reshape | ❌ module reload + in-flight state loss | **✅ DB UPDATE 한 줄 + 다음 요청에 새 engine compile** |
| 1000 customer 의 1000 개 다른 graph | ❌ process per customer = 80 GB | ✅ 한 process / distinct shape 별 cache |
| Emergent cluster discovery | N/A | **✅ graph_def hash distribution 이 customer behavior cluster** |

LangChain/LangGraph 의 StateGraph 가 Python class instance — pickle 도
import path 묶고, runtime 에 노드/엣지 reshape 하려면 Python module
reload 필요, in-flight conversation state 잃음. **NG 는 graph-as-JSON
이라 evolution = JSON 변환 한 줄.**

## 핵심 메커니즘

매 turn 끝에 LLM judge (gpt-4o-mini) 가 conversation history + 현재
topology 를 보고 best fit 을 한 단어로 응답:

- `simple` — 1 LLM call, short direct answer (factual Q 에 적합)
- `reflexive` — 3 LLM calls (draft → critique → final) (accuracy-seeking 에 적합)
- `fanout` — 3 parallel LLM perspectives → merge (multi-view 요구에 적합)

판단이 다르면 in-place 로 customer DB 의 graph_def 업데이트. 다음 turn
부터 새 topology — **deploy 0, restart 0, in-flight state 보존**.

```cpp
std::string suggested = llm_judge_topology(
    provider, customer.history, customer.topology_name);

if (suggested != customer.topology_name) {
    customer.topology_def  = topo_registry[suggested]();   // 새 graph_def
    customer.topology_name = suggested;
    // 다음 turn 에서 cache 가 새 hash 보고 자동으로 새 engine compile.
    // 진짜 production 이면 DB UPDATE customer_graphs SET graph_def = ...
}
```

## 데모 1 — Alice 1명 (server.cpp)

5-turn 의 점진적 진화. 사용자가 factual question → multi-perspective
question 으로 자연스럽게 이동하면 harness 가 따라서 simple → fanout 으로
진화.

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
... 이후 multi-perspective 응답 ...

Evolution timeline:
  Turn 0:  simple   (initial)
  Turn 3:  fanout   (evolved)
```

## 데모 2 — Multi-customer (server_multi.cpp) ⭐

**진짜 임팩트는 여기.** 5 customer 가 각자 다른 행동 패턴 보이고, 각자
별도 evolution timeline 으로 진화. emergent cluster 발견 시연.

각 customer 의 행동 패턴 가설 + 실제 결과:

| Customer | 행동 패턴 | 가설 | 실제 evolution | 검증 |
|---|---|---|---|---|
| **alice** | 점진 (factual → multi-view) | 중반에 fanout | `simple → fanout(t3)` | ✅ |
| **bob** | factual only ("What is X?" × 5) | simple 유지 | `simple` 5턴 내내 | ✅ |
| **charlie** | accuracy-seeking ("verify your answer") | reflexive | `simple → reflexive(t1)` 즉시 | ✅ |
| **david** | 처음부터 "compare X vs Y multi-angle" | fanout 빠르게 | `simple → fanout(t1)` 즉시 | ✅ |
| **eve** | 혼합 (factual ↔ multi-view ↔ careful 진동) | oscillation 위험 | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **진동** | ✅ |

### 종합 결과

```
=== Aggregate stats ===
Customers:           5
Total turns:         25
Total main LLM:      51
Total judge LLM:     25
Total LLM calls:     76
Wall time:           424 s
Peak RSS:            18.99 MB
Compile cache size:  3   ← 5 customer → 3 distinct engine

=== Final topology distribution ===
  fanout:    3 customers  (alice, david, eve)
  reflexive: 1 customer   (charlie)
  simple:    1 customer   (bob)
```

### 핵심 관찰

1. **행동 패턴 가설 4/5 정확 검증** — 사람이 미리 예상한 evolution path 와
   LLM judge 의 실제 진화 결정이 정확히 일치. 즉 **LLM judge 가 사용자
   intent shift 를 신뢰성 있게 감지**.

2. **Eve 의 oscillation 실제 관찰 ⚠️** — 발화가 [factual → multi → factual
   → careful → factual] 로 진동하니 topology 도 [simple → fanout →
   fanout(유지) → reflexive → fanout] 으로 진동. **anti-flapping guard
   필요성** 이 데이터로 검증됨 (cooldown 또는 hysteresis 같은 보강 필요).

3. **Emergent cluster discovery** — 5 customer 의 다양한 발화 패턴이
   자연스럽게 **3 개 topology cluster** 로 분류됨. compile cache size =
   3 = distinct cluster 수.

   **이게 진짜 흥미로운 emergent property** — NG 의 graph-as-data 가
   자연스럽게 customer behavior cluster 발견 메커니즘이 됨. graph_def
   distribution = customer behavior 의 본질적 cluster shape.

4. **메모리 효율** — 5 customer → 3 engine. customer 2 명 분량의 engine
   메모리가 cache 공유로 절감. **scale-up 시 1000 customer 로 가도
   distinct shape 이 ~10 개로 수렴하면 engine 메모리는 거의 일정 → 진짜
   1000+ customer multi-tenant 가 한 process 에 들어감.**

5. **단순 시퀀셜 시뮬레이션이라 wall time 7분** — 진짜 production 에선
   각 customer 가 독립적이라 병렬 가능. 5 customer 병렬이면 ~1.5분 +
   compile cache 가 동시 access safe (`std::shared_mutex`) 라 race 없음.

## Production 시나리오 — 실제로 구현하면

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
auto  engine  = cache.get_or_compile(cust.graph_def, ctx);  // hash 기반 cache
auto  history = db.fetch_history(thread_id);     // session 격리 키로
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

## 향후 확장 후보

- **Anti-oscillation guard** — eve 케이스 처리. 최근 N turn 안에 evolve
  했으면 lockout, 또는 hysteresis (현재 topology 가 next-candidate
  보다 N% 안 낮으면 안 바꿈).
- **LLM-generated graph_def** — 지금은 3 pre-defined topology 중 선택.
  더 야심차게는 LLM 이 graph_def JSON 자체를 처음부터 생성. NG 의
  [v0.5.0 example 23 evolving chat agent](../../23_*.cpp) 의 fork +
  meta 조립 패턴이 그쪽 방향.
- **Parallel customer 처리** — 시퀀셜 데모는 7분, customer 별 병렬이면
  ~1.5분. `asio::thread_pool` + compile cache 그대로 사용.
- **A/B framework** — 같은 customer 에게 2 topology 동시 운영, 응답
  만족도로 winner 결정. graph_id 별 sticky split.
- **CheckpointStore 통합** — Postgres + 위 SQL schema 로 진짜
  production-ready.
- **Evolution rate adaptive** — eval-interval 을 customer 의 history
  안정성에 따라 조절 (안정되면 매 10 turn, 불안정하면 매 turn).

## 핵심 메시지

> **Self-evolving + multi-tenant 결합이 NG 의 진짜 본질.** "AI agent
> that builds itself" 라는 vision 이 NG 의 graph-as-data 패러다임으로
> **실용적으로 구현 가능한 카테고리**. LLM 이 자기 자신의 harness 를
> 출력 → DB UPDATE → 즉시 적용 — LangGraph 의 StateGraph-as-Python
> 모델로는 닫힌 길이고, NG 가 **유일한 player** 인 시장.
>
> *"5 customer × 5 turn = 19 MB / 3 distinct engine / emergent cluster
> 발견 / oscillation 진단까지. 진짜 self-improving multi-tenant agent
> infrastructure 의 시작점."*
