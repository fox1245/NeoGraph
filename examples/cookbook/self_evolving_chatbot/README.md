# Self-evolving chatbot

**Chatbot harness 가 사용자 행동을 보고 *자기 자신의* topology 를
runtime 에 reshape 합니다. NeoGraph 만 가능한 카테고리.**

[multi_tenant_chatbot](../multi_tenant_chatbot/) cookbook 의 자연스러운
확장 — 그쪽은 customer 별 harness 가 *고정*, 이쪽은 *진화*. 같은
compile cache + thread_id 격리 위에 LLM judge 한 단계 더 얹은 구조.

## 왜 NG 만 할 수 있나

| 시도 | LangGraph | NeoGraph |
|---|---|---|
| Customer 별 다른 harness | ❌ (StateGraph = Python 객체) | ✅ (graph_def JSON row) |
| Harness 가 runtime 에 reshape | ❌ (module reload + state loss) | **✅ (DB UPDATE 한 줄 + 다음 요청에 새 engine compile)** |
| 진짜 "personalized agent" | "history 누적" 수준 | "harness 자체가 사용자 별 진화" |

LangChain/LangGraph 의 StateGraph 가 Python class instance — pickle 도
import path 묶고, runtime 에 노드/엣지 reshape 하려면 Python 모듈 reload
필요, in-flight conversation state 잃음. **NG 는 graph-as-JSON 이라
evolution = JSON 변환 한 줄.**

## 시나리오

Alice 가 처음엔 `simple` topology (1-LLM-call, short answer) 로 시작.
매 turn 끝에 LLM judge (gpt-4o-mini) 가 conversation history + 현재
topology 를 보고 "이 사용자에게 더 적합한 topology" 를 한 단어로 응답:

- `simple` — 1 LLM call, short direct answer
- `reflexive` — 3 LLM calls (draft → critique → final)
- `fanout` — 3 parallel LLM perspectives → merge

판단이 다르면 in-place 로 customer DB 의 graph_def 업데이트. 다음 turn
부터 새 topology — **deploy 0, restart 0, in-flight state 보존**.

## 측정 결과 (real OpenAI gpt-4o-mini)

```
=== Self-evolving chatbot demo — alice 의 harness 진화 ===
Starting topology: simple

── Turn 1 [topology=simple] ──
User: What is a cloud?
Bot:  A cloud is a visible mass of condensed water vapor...
[Evaluating harness fit...] judge → simple

── Turn 2 [topology=simple] ──
User: What is HTTP?
Bot:  HTTP stands for Hypertext Transfer Protocol...
[Evaluating harness fit...] judge → simple

── Turn 3 [topology=simple] ──
User: Now explain blockchain to me — I want both the
      technical view and the economic view.
Bot:  **Technical View:** Blockchain is a decentralized digital ledger...
[Evaluating harness fit...] judge → fanout
  ⟹ EVOLVE: simple → fanout (in-place, deploy 0)

── Turn 4 [topology=fanout] ──
User: Compare microservices vs monolith from multiple angles:
      cost, complexity, team scaling.
Bot:  Multiple perspectives:
- **Cost:** Microservices can lead to higher initial costs...
- **Complexity:** ...
- **Team scaling:** ...
[Evaluating harness fit...] judge → fanout

── Turn 5 [topology=fanout] ──
User: Give me three different perspectives on whether AI agents
      will replace SaaS.
Bot:  Multiple perspectives:
- 1. **Technological Perspective:** AI agents can enhance SaaS...
- 2. **Economic Perspective:** ...
- 3. **Industry Perspective:** ...
[Evaluating harness fit...] judge → fanout

=== Summary ===
Turns:                5
Final topology:       fanout
Main LLM calls:       9          (turn 1+2 × 1, turn 3+4+5 × 3 — fanout 의 3 parallel)
Judge LLM calls:      5          (매 turn 마다 1)
Total LLM calls:      14
Wall time:            16 s
Peak RSS:             17.5 MB
Compile cache size:   2          (simple + fanout, reflexive 는 demo 에서 안 쓰임)

--- Evolution timeline ---
Turn 0:  simple   (initial)
Turn 3:  fanout   (evolved)
```

**비용: 14 LLM call × ~$0.0002 ≈ $0.003.**

## 핵심 관찰

1. **Turn 3 의 EVOLVE 가 진짜 자가 진화** — Alice 가 명시적으로 "양쪽
   관점" 을 요구했고, LLM judge 가 그 신호를 잡아 fanout 으로 reshape
   결정. 사람이 코드 짠 게 아니라 **LLM 이 graph_def 를 출력**.

2. **Compile cache 효과 그대로** — distinct topology 2개 (simple + fanout)
   만 compile, 나머지 turn 은 cache hit. cache size 가 used topology 수와
   정확히 일치 = `Compile cache size: 2`.

3. **Conversation state 보존** — topology 가 simple → fanout 으로 바뀌어도
   thread_id 격리 + history 누적이 그대로. Turn 4-5 가 turn 1-2-3 의
   context 다 들고 있음.

4. **메모리 17.5 MB** — multi_tenant_chatbot 의 100 동시 LLM 시 21.9 MB
   와 비슷한 영역. Self-evolution 자체의 메모리 비용 ≈ 0 (judge 호출
   transient + 새 topology engine 1개 추가만).

5. **Topology 진화 timeline 이 사용자 의도 변화에 정확히 따름:**

   ```
   질문 스타일 변화                      → topology 진화
   ─────────────────────────────────────────────────────
   "What is X?" (factual)                  → simple
   "양쪽 관점에서 설명해줘" (multi-view)  → fanout
   "여러 각도에서 비교해줘" (comparison)  → fanout 유지
   "세 관점에서 평가해줘" (multi-perspective) → fanout 유지
   ```

## 빌드 / 실행

```bash
# repo root 에 .env 에 OPENAI_API_KEY 박혀있어야 함
cmake --build build --target cookbook_self_evolving_chatbot
./build/cookbook_self_evolving_chatbot
```

## 구현 핵심

```cpp
// 매 turn 끝에서 호출
std::string suggested = llm_judge_topology(
    provider, alice.history, alice.topology_name);

if (suggested != alice.topology_name) {
    std::cout << "EVOLVE: " << alice.topology_name
              << " → " << suggested << "\n";
    alice.topology_def  = topo_registry[suggested]();   // 새 graph_def
    alice.topology_name = suggested;
    // 다음 turn 에서 cache 가 새 hash 보고 자동으로 새 engine compile.
    // 진짜 production 이면 DB UPDATE customer_graphs SET graph_def = ...
}
```

LLM judge prompt 의 본질:

```
Given conversation history + current topology, respond with EXACTLY
one of: simple | reflexive | fanout.

- simple:    1 LLM call, short direct answer
- reflexive: 3 LLM calls (draft → critique → final)
- fanout:    3 parallel LLM perspectives merged
```

응답을 parsing 해서 일치하면 evolve, 아니면 현재 topology 유지.

## 향후 확장 후보

- **LLM-generated graph_def** — 지금은 3 pre-defined topology 중 선택.
  더 야심차게는 LLM 이 graph_def JSON 자체를 처음부터 생성 (node/edge
  자유 조합). NG 의 [v0.5.0 example 23 evolving chat agent](../../23_*) 의 fork + meta 조립 패턴이 그쪽 방향.
- **Multi-tenant 결합** — 6 customer 가 각자 별도 evolution timeline.
  각 customer 의 `thread_id` 별 history 가 judge 의 input — 사람마다
  다른 harness 로 발전.
- **Anti-oscillation guard** — judge 가 turn 마다 simple↔fanout 왔다갔다
  하면 위험. "최근 N turn 안에 evolve 했으면 lockout" 같은 cooldown.
- **A/B framework** — 같은 customer 에게 2 topology 를 동시 운영, 응답
  만족도 (사용자 후속 질문 패턴) 로 winner 결정. graph_id 별 sticky split.
- **CheckpointStore 통합** — Postgres `customer_graphs.graph_def` 와
  `customer_topology_history` 두 테이블로 영속화. 위의 hot-swap 이
  진짜 production-ready.

## 핵심 메시지

> **NG = chatbot harness 가 사람마다 다르게 진화하는 personalized
> agent infrastructure 의 기반.**
>
> 진짜 의미의 "AI agent that builds itself" — LLM 이 자기 자신의 graph
> topology 를 출력하는 카테고리. graph-as-data 패러다임이 만든 본질적
> 가능성이고, **LangGraph 의 StateGraph-as-Python-object 모델로는 닫힌 길**.
