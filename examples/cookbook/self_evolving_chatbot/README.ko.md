<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=ko source_sha256=0c6d156e7378f114498c63578e7981c6401394ada5684fc924fb72ec7c867849 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# 스스로 진화하는 챗봇


**챗봇 하네스는 사용자 행동에 따라 런타임에 *자체* 토폴로지를 재구성합니다.
이 기능은 NeoGraph의 고유한 기능입니다. LangGraph는 런타임에 그래프의 모양을 바꿀 수 없습니다.**

[multi_tenant_chatbot](../multi_tenant_chatbot/) 요리책의 자연스러운 확장 — 그 중 하나는
고객 하네스는 *고정*되어 있지만 *진화*됩니다. 동일한 컴파일 캐시 + thread_id 격리
LLM 심사 단계를 하나 더 추가합니다.

## 두 개의 데모

|파일|대본|비용|벽 시간|
|---|---|---|---|
|[server.cpp](server.cpp)|Alice 1명 × 5턴 — 최소 진화 메커니즘 데모| ~$0.003 |16초|
|[server_multi.cpp](server_multi.cpp)|**5명의 고객 × 5턴 — 각각 별도의 진화 타임라인 + 자연 발생 군집**| ~$0.02 |7분|

빌드/실행(둘 다):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## 왜 NG만이 이를 할 수 있는가?

|시도|랭그래프|네오그래프|
|---|---|---|
|고객마다 다른 하네스|❌ StateGraph = Python 객체|✅ graph_def JSON 행|
|하네스는 런타임에 자체적으로 모양이 변경됩니다.|❌ 모듈 재로드 + 비행 중 상태 손실|**✅ 하나의 DB UPDATE + 다음 요청 시 새 엔진 컴파일**|
|1000명의 고객의 1000가지 다양한 그래프|❌ 고객당 프로세스 = 80GB|✅ 단일 프로세스 / 고유한 모양 캐시|
|자연 발생 군집 발견|N/A|**✅ graph_def 해시 분포 = 고객 행동 군집**|

LangChain/LangGraph의 StateGraph는 Python 클래스 인스턴스입니다. 피클은 가져오기 경로도 번들로 제공합니다.
런타임 node/edge 재구성에는 Python 모듈 다시 로드가 필요하며, 진행 중인 대화 상태가 손실됩니다.
**NG의 JSON 그래프는 진화 = 하나의 JSON 수정을 의미합니다.**

## 핵심 메커니즘

각 턴이 끝나면 LLM 심사위원(gpt-4o-mini)이 대화 내역 + 현재를 살펴봅니다.
토폴로지와 한 단어로 가장 잘 어울리는 응답:

- `simple` — 1개의 LLM 통화, 짧은 직접 응답(사실적 Q에 적합)
- `reflexive` — 3개의 LLM 호출(초안 → 비평 → 최종)(정확성 추구에 적합)
- `fanout` — 3개의 병렬 LLM 관점 → 병합(다중 뷰 요구 사항에 적합)

판단이 다를 경우 고객 DB의 graph_def를 in-place 업데이트합니다. 다음 차례에서는 새로운 토폴로지를 사용합니다.
**배포 0회, 재시작 0회, 진행 중 상태 보존**.

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

## 데모 1 - Alice 1인(server.cpp)

5턴에 걸쳐 점진적으로 진화합니다. 사용자는 자연스럽게 사실적 질문 → 다관점으로 이동합니다.
질문, 하네스는 단순 → 팬아웃 진화를 따릅니다.

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

## 데모 2 — 다중 고객(server_multi.cpp) ⭐

**실질적인 영향이 여기에 있습니다.** 5명의 고객이 서로 다른 행동 패턴을 보여줍니다.
별도의 진화 타임라인. 자연 발생 군집 발견 데모.

각 고객의 행동 패턴 가설 + 실제 결과:

|고객|행동 패턴|가설|실제 진화|확인|
|---|---|---|---|---|
|**alice**|점진적(사실적 → 다중뷰)|중간에 팬아웃|`simple → fanout(t3)`| ✅ |
|**bob**|사실만("X란 무엇입니까?" × 5)|간단한 유지|`simple` 5턴 모두| ✅ |
|**charlie**|정확성 추구("답변 확인")|반사적|즉시 `simple → reflexive(t1)`| ✅ |
|**david**|시작부터 "X 대 Y 다중 각도 비교"|빠른 팬아웃|즉시 `simple → fanout(t1)`| ✅ |
|**eve**|혼합(사실 ⇔ 멀티뷰 ⇔ 주의깊은 진동)|진동 위험|`simple → fanout(t2) → reflexive(t4) → fanout(t5)` **진동**| ✅ |

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

### 주요 관찰

1. **행동 패턴 가설 4/5가 정확하게 검증됨** — 인간의 예측 진화 경로
LLM 심사위원의 실제 진화 결정이 정확히 일치합니다. 즉, **LLM 판사는 안정적으로 감지합니다.
사용자 의도 변화**.

2. **Eve의 진동은 실제로 관찰되었습니다 ⚠️** — 발화 [사실 → 다중 → 사실
→ 주의 → 사실] 토폴로지를 진동하게 함 [단순 → 팬아웃 →
fanout(maintain) → 반사 → 팬아웃]. **플래핑 방지 가드가 필요합니다**
데이터로 검증됨(쿨다운 또는 히스테리시스 개선 필요)

3. **자연 발생 군집 발견** — 5명의 고객의 다양한 발화 패턴을 자연스럽게 파악
**3개의 토폴로지 군집**로 분류합니다. 컴파일 캐시 크기 =
3 = 서로 다른 군집 수.

**이것은 정말 흥미로운 자연 발생 속성입니다** — NG의 그래프는 자연스럽게 데이터로 표시됩니다.
고객 행동 군집 발견 메커니즘이 됩니다. graph_def
분포 = 고객 행동의 본질적인 군집 형태.

4. **메모리 효율성** — 고객 5명 → 엔진 3개. 2 고객의 엔진
캐시 공유를 통해 메모리가 절약되었습니다. **형태가 다른 경우 최대 1000명의 고객으로 확장
~10으로 수렴, 엔진 메모리는 거의 일정하게 유지됨 → 실제
1000명 이상의 고객 다중 테넌트가 하나의 프로세스에 적합합니다.**

5. **순차 시뮬레이션에는 단 7분의 실제 시간이 소요됩니다** — 생산 과정에서 각 고객은
독립적이므로 병렬이 가능합니다. 5명의 고객 병렬 = ~1.5분 +
컴파일 캐시는 동시 액세스가 안전하므로(`std::shared_mutex`) 경합이 없습니다.

## 생산 시나리오 - 실제 구현

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

- **진동 방지 가드** — 모든 케이스를 처리합니다. 마지막 N 턴 동안 진화한 경우 잠금,
또는 히스테리시스(현재 토폴로지가 다음 후보보다 N% 낮지 않은 경우 변경하지 마십시오).
- **LLM 생성 graph_def** — 현재 사전 정의된 토폴로지 3개 중에서 선택합니다.
처음부터 graph_def JSON을 생성하는 방법은
[`the-beast/`](../the-beast/) cookbook의 모델 작성 토폴로지와
컴파일/검증 게이트를 참고하세요.
- **병렬 고객 처리** — 순차 데모 7분, 고객당 병렬 = ~1.5분.
`asio::thread_pool` + 컴파일 캐시를 직접 사용하세요.
- **A/B 프레임워크** — 동일한 고객에 대해 2개의 토폴로지를 동시에 운영하고 승자를 결정합니다.
반응 만족. graph_id로 고정 분할됩니다.
- **CheckpointStore 통합** — Postgres + 실제 SQL 스키마 위
생산 준비.
- **적응형 진화율** — 고객 내역 안정성을 기반으로 평가 간격을 조정합니다.
(안정 = 10회전마다, 불안정 = 매 회전).

## 핵심 메시지

> **자체 진화 + 멀티 테넌트 조합은 NG의 진정한 본질입니다. ** "AI 에이전트"의 비전
> 스스로 구축하는 것"은 NG의 데이터로서의 그래프 패러다임을 사용하여 **실질적으로 구현 가능**합니다.
> LLM는 자체 하네스 출력 → DB UPDATE → 즉시 적용 — 폐쇄 경로
> LangGraph의 StateGraph-as-Python 모델인 NG는 이 시장의 **유일한 플레이어**입니다.
>
> *"5명의 고객 × 5턴 = 19MB / 3개의 개별 엔진 / 자연 발생 군집
> 발견/진동 진단. 진정한 자기 개선형 다중 테넌트 에이전트의 출발점
> 하부 구조."*
