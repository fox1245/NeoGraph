<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=zh-CN source_sha256=0c6d156e7378f114498c63578e7981c6401394ada5684fc924fb72ec7c867849 -->
# 自演化聊天机器人

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**chatbot harness 会在运行时根据用户行为重塑 *自己的* 拓扑。这个能力是 NeoGraph 独有的；LangGraph 无法在运行时重塑 graph。**

这是 [multi_tenant_chatbot](../multi_tenant_chatbot/) cookbook 的自然扩展 — 那个示例的 customer harness 是 *固定的*，这个示例会 *演化*。同样的 compile cache + thread_id isolation，再加一个 LLM judge step。

## 两个 Demo

| 文件 | 场景 | 成本 | 总耗时 |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1 人 × 5 turns — 最小演化机制 demo | ~$0.003 | 16 s |
| [server_multi.cpp](server_multi.cpp) | **5 个客户 × 5 turns — 每个都有独立演化时间线 + 涌现聚类** | ~$0.02 | 7 min |

构建 / 运行（两者）：

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## 为什么只有 NG 能做到

| 尝试 | LangGraph | NeoGraph |
|---|---|---|
| 每个客户不同 harness | ❌ StateGraph = Python object | ✅ graph_def JSON 行 |
| harness 在运行时重塑自身 | ❌ 模块 reload + in-flight state 丢失 | **✅ 一次 DB UPDATE + 下一请求编译新 engine** |
| 1000 个客户的 1000 个不同 graph | ❌ 每客户一个进程 = 80 GB | ✅ 一个进程 / 不同形状 cache |
| 涌现聚类发现 | N/A | **✅ graph_def hash 分布 = 客户行为聚类** |

LangChain/LangGraph 的 StateGraph 是 Python class instance — pickle 也会捆绑 import path；运行时重塑 node/edge 需要 Python module reload，in-flight conversation state 会丢失。**NG 的 graph-as-JSON 意味着演化 = 修改一次 JSON。**

## 核心机制

每个 turn 结束时，LLM judge（gpt-4o-mini）查看 conversation history + current topology，并用一个词回答最适合的形态：

- `simple` — 1 次 LLM 调用，简短直接回答（适合 factual Q）
- `reflexive` — 3 次 LLM 调用（draft → critique → final）（适合追求准确性）
- `fanout` — 3 个并行 LLM perspectives → merge（适合多视角需求）

如果判断不同，就原地更新 customer DB 的 graph_def。下一 turn 使用新拓扑 — **0 deploy，0 restart，in-flight state preserved**。

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

## Demo 1 — Alice 1 人（server.cpp）

5 个 turns 中逐渐演化。用户自然地从事实型问题转向多视角问题，harness 跟随 simple → fanout 演化。

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

## Demo 2 — Multi-Customer（server_multi.cpp）⭐

**真正的影响在这里。** 5 个客户展现不同的行为模式，每个都有独立演化时间线。这是涌现聚类发现 demo。

每个客户的行为模式假设 + 实际结果：

| 客户 | 行为模式 | 假设 | 实际演化 | 验证 |
|---|---|---|---|---|
| **alice** | 渐进（事实型 → 多视角） | 中途 fanout | `simple → fanout(t3)` | ✅ |
| **bob** | 只问事实型问题（“What is X?” × 5） | 保持 simple | `simple` 全部 5 个 turns | ✅ |
| **charlie** | 追求准确性（“verify your answer”） | reflexive | `simple → reflexive(t1)` 立即发生 | ✅ |
| **david** | 从一开始就“compare X vs Y multi-angle” | 快速 fanout | `simple → fanout(t1)` 立即发生 | ✅ |
| **eve** | 混合（事实型 ↔ 多视角 ↔ 谨慎摇摆） | 摇摆风险 | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **摇摆** | ✅ |

### 汇总结果

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

### 关键观察

1. **行为模式假设 4/5 被准确验证** — 人类预测的演化路径与 LLM judge 的实际演化决定完全匹配。也就是说，**LLM judge 能可靠检测用户意图转移**。

2. **实际观察到 Eve 的摇摆 ⚠️** — 话语序列 [事实型 → 多视角 → 事实型 → 谨慎 → 事实型] 会导致拓扑在 [simple → fanout → fanout(maintain) → reflexive → fanout] 之间摆动。**需要防抖保护**，这点已由数据验证（需要 cooldown 或 hysteresis 增强）。

3. **涌现聚类发现** — 5 个客户的多样 utterance pattern 自然分类为 **3 个拓扑 cluster**。Compile cache size = 3 = distinct cluster count。

   **这是真正有趣的涌现性质** — NG 的 graph-as-data 自然成为客户行为聚类发现机制。graph_def distribution = customer behavior 的 essential cluster shape。

4. **内存效率** — 5 个客户 → 3 个 engines。2 个客户的 engine memory 通过 cache sharing 节省。**放大到 1000 个客户时，如果 distinct shapes 收敛到 ~10，engine memory 几乎保持常数 → 真实 1000+ customer multi-tenant 可装进一个进程。**

5. **顺序模拟只需 7 分钟总耗时** — 生产中每个客户独立，因此可以并行。5 个客户并行 = ~1.5 分钟 + compile cache 可安全并发访问（`std::shared_mutex`），所以无 race。

## 生产场景 — 实际实现

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

每个请求的处理流程：

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

## 未来扩展

- **防摇摆保护** — 处理 eve case。如果最近 N turns 内已演化，则 lockout；或使用 hysteresis（如果当前 topology 不比下一候选低 N%，就不切换）。
- **LLM 生成 graph_def** — 当前从 3 个预定义拓扑中选择。更大胆地说，LLM 可以从零生成 graph_def JSON。请参考 [`the-beast/`](../the-beast/) cookbook 中的模型编写拓扑及编译/验证门控。
- **并行客户处理** — 顺序 demo 7 分钟，按客户并行 = ~1.5 分钟。直接使用 `asio::thread_pool` + compile cache。
- **A/B 框架** — 同时为同一客户运行 2 个拓扑，根据响应满意度决定赢家。按 graph_id sticky split。
- **CheckpointStore 集成** — Postgres + 上述 SQL schema，面向真实生产可用。
- **自适应演化频率** — 根据 customer history stability 调整 eval-interval（stable = every 10 turns，unstable = every turn）。

## 核心信息

> **自演化 + 多租户组合才是 NG 的真正本质。** “AI agent
> that builds itself” 这个愿景可以通过 NG 的 graph-as-data paradigm **实际落地**。
> LLM 输出自己的 harness → DB UPDATE → 立即应用 — 对 LangGraph 的 StateGraph-as-Python model 来说这条路径关闭，NG 是这个市场中 **唯一玩家**。
>
> *“5 个客户 × 5 个 turns = 19 MB / 3 个不同 engine / 涌现聚类发现 / 摇摆诊断。
> 这是走向真实自改进多租户 agent 基础设施的起点。”*
