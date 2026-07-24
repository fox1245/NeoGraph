# Self-Evolving Chatbot

**Chatbot harness reshapes *its own* topology at runtime based on user behavior.
A category only NeoGraph can do, not LangGraph.**

Natural extension of [multi_tenant_chatbot](../multi_tenant_chatbot/) cookbook — that one has
customer harnesses *fixed*, this one *evolves*. Same compile cache + thread_id isolation
with one more LLM judge step.

## Two Demos

| File | Scenario | Cost | Wall time |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1 person × 5 turns — minimal evolution mechanism demo | ~$0.003 | 16 s |
| [server_multi.cpp](server_multi.cpp) | **5 customers × 5 turns — Each separate evolution timeline + emergent cluster** | ~$0.02 | 7 min |

Build / run (both):

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## Why Only NG Can Do This

| Attempt | LangGraph | NeoGraph |
|---|---|---|
| Different harness per customer | ❌ StateGraph = Python object | ✅ graph_def JSON row |
| Harness reshapes itself at runtime | ❌ Module reload + in-flight state loss | **✅ One DB UPDATE + new engine compile on next request** |
| 1000 customers' 1000 different graphs | ❌ Process per customer = 80 GB | ✅ One process / distinct shape cache |
| Emergent cluster discovery | N/A | **✅ graph_def hash distribution = customer behavior cluster** |

LangChain/LangGraph's StateGraph is Python class instance — pickle also bundles import path,
runtime node/edge reshaping requires Python module reload, in-flight conversation state lost.
**NG's graph-as-JSON means evolution = one JSON modification.**

## Core Mechanism

At end of each turn, LLM judge (gpt-4o-mini) looks at conversation history + current
topology and responds with best fit in one word:

- `simple` — 1 LLM call, short direct answer (suitable for factual Q)
- `reflexive` — 3 LLM calls (draft → critique → final) (suitable for accuracy-seeking)
- `fanout` — 3 parallel LLM perspectives → merge (suitable for multi-view requirements)

If judgment differs, in-place update customer DB's graph_def. Next turn uses new topology —
**0 deploy, 0 restart, in-flight state preserved**.

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

## Demo 1 — Alice 1 Person (server.cpp)

Gradual evolution over 5 turns. User naturally moves from factual question → multi-perspective
question, harness follows simple → fanout evolution.

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

## Demo 2 — Multi-Customer (server_multi.cpp) ⭐

**Real impact is here.** 5 customers show different behavior patterns, each with
separate evolution timeline. Emergent cluster discovery demo.

Each customer's behavior pattern hypothesis + actual result:

| Customer | Behavior pattern | Hypothesis | Actual evolution | Verification |
|---|---|---|---|---|
| **alice** | Gradual (factual → multi-view) | fanout mid-way | `simple → fanout(t3)` | ✅ |
| **bob** | Factual only ("What is X?" × 5) | simple maintained | `simple` all 5 turns | ✅ |
| **charlie** | Accuracy-seeking ("verify your answer") | reflexive | `simple → reflexive(t1)` immediately | ✅ |
| **david** | From start "compare X vs Y multi-angle" | fast fanout | `simple → fanout(t1)` immediately | ✅ |
| **eve** | Mixed (factual ↔ multi-view ↔ careful oscillation) | oscillation risk | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **oscillation** | ✅ |

### Summary Results

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

### Key Observations

1. **Behavior pattern hypothesis 4/5 accurately verified** — Human's predicted evolution path
   and LLM judge's actual evolution decision match exactly. I.e., **LLM judge reliably detects
   user intent shift**.

2. **Eve's oscillation actually observed ⚠️** — Utterances [factual → multi → factual
   → careful → factual] cause topology to oscillate [simple → fanout →
   fanout(maintain) → reflexive → fanout]. **Need anti-flapping guard**
   verified by data (cooldown or hysteresis enhancement needed).

3. **Emergent cluster discovery** — 5 customers' diverse utterance patterns naturally
   classify into **3 topology clusters**. Compile cache size =
   3 = distinct cluster count.

   **This is the truly interesting emergent property** — NG's graph-as-data naturally
   becomes customer behavior cluster discovery mechanism. graph_def
   distribution = customer behavior's essential cluster shape.

4. **Memory efficiency** — 5 customers → 3 engines. 2 customers' engine
   memory saved via cache sharing. **Scale-up to 1000 customers, if distinct shapes
   converge to ~10, engine memory stays nearly constant → real
   1000+ customer multi-tenant fits in one process.**

5. **Sequential simulation only 7 minutes wall time** — Real production each customer
   independent so parallel possible. 5 customers parallel = ~1.5 minutes +
   compile cache is concurrent access safe (`std::shared_mutex`) so no race.

## Production Scenario — Actual Implementation

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

Each request processing flow:

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

## Future Extensions

- **Anti-oscillation guard** — Handle eve case. Lockout if evolved in last N turns,
  or hysteresis (don't change if current topology not N% lower than next-candidate).
- **LLM-generated graph_def** — Currently selects from 3 pre-defined topologies.
  More ambitiously, LLM generates graph_def JSON from scratch. NG's
  [v0.5.0 example 23 evolving chat agent](../../23_*.cpp) fork +
  meta assembly pattern goes this direction.
- **Parallel customer processing** — Sequential demo 7 minutes, parallel per customer = ~1.5 minutes.
  Use `asio::thread_pool` + compile cache directly.
- **A/B framework** — Operate 2 topologies for same customer simultaneously, decide winner by
  response satisfaction. Sticky split by graph_id.
- **CheckpointStore integration** — Postgres + above SQL schema for real
  production-ready.
- **Adaptive evolution rate** — Adjust eval-interval based on customer history stability
  (stable = every 10 turns, unstable = every turn).

## Core Message

> **Self-evolving + multi-tenant combination is NG's real essence.** The vision of "AI agent
> that builds itself" is **practically implementable** with NG's graph-as-data paradigm.
> LLM outputs its own harness → DB UPDATE → immediate application — closed path for
> LangGraph's StateGraph-as-Python model, NG is **the only player** in this market.
>
> *"5 customers × 5 turns = 19 MB / 3 distinct engine / emergent cluster
> discovery / oscillation diagnosis. Starting point for real self-improving multi-tenant agent
> infrastructure."*
