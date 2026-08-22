<!-- neograph-i18n: source=examples/cookbook/self_evolving_chatbot/README.md locale=zh-CN source_sha256=14c932ce835be59435fe30b831344894d899490a1478a3bd34f442e9113414da -->
# 自进化聊天机器人

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**该聊天机器人框架会在运行时根据用户行为重塑*其自身*的拓扑。此能力为 NeoGraph 独有；LangGraph 无法在运行时重塑图结构。**

[multi_tenant_chatbot](../multi_tenant_chatbot/) cookbook 的自然扩展——前者客户框架*固定*，后者*进化*。使用相同的编译缓存 + thread_id 隔离，并增加一步 LLM 评判。

## 两个演示

| 文件 | 场景 | 成本 | 实际耗时 |
|---|---|---|---|
| [server.cpp](server.cpp) | Alice 1 人 × 5 轮——最小进化机制演示 | ~$0.003 | 16 秒 |
| [server_multi.cpp](server_multi.cpp) | **5 位客户 × 5 轮——各自独立的进化时间线 + 涌现式聚类** | ~$0.02 | 7 分钟 |

构建 / 运行（两者均可）：

```bash
cmake --build build --target cookbook_self_evolving_chatbot cookbook_self_evolving_chatbot_multi
./build/cookbook_self_evolving_chatbot         # single (alice)
./build/cookbook_self_evolving_chatbot_multi   # multi (5 customers)
```

## 为何只有 NG 能做到

| 尝试方案 | LangGraph | NeoGraph |
|---|---|---|
| 每位客户使用不同的框架 | ❌ StateGraph = Python 对象 | ✅ graph_def JSON 行 |
| Harness 在运行时重塑自身 | ❌ 模块重载 + 运行中状态丢失 | **✅ 一次数据库 UPDATE + 下次请求时重新编译引擎** |
| 1000 个客户的 1000 个不同图 | ❌ 每个客户一个进程 = 80 GB | ✅ 单进程 / 独立形状缓存 |
| 涌现式集群发现 | N/A | ✅ graph_def 哈希分布 = 客户行为集群 |

LangChain/LangGraph 的 StateGraph 是 Python 类实例 — pickle 同时捆绑导入路径，运行时节点/边重塑需要 Python 模块重载，运行中对话状态丢失。**NG 的图即 JSON 意味着演进 = 一次 JSON 修改。**

## Core 机制

在每个回合结束时，固定的DeepSeek模型通过OpenRouter充当LLM评判者：它查看对话历史+当前拓扑，并用一个词回应最合适的方案：

- `simple` — 1 次 LLM 调用，简短直接回答（适用于事实性提问）
- `reflexive` — 3 次 LLM 调用（初稿 → 批评 → 终稿）（适用于追求准确性的场景）
- `fanout` — 3 个并行 LLM 视角 → 合并（适用于多视角需求）

若判定不一致，则就地更新客户数据库的 graph_def。下一轮使用新的拓扑——**0 部署、0 重启，运行中状态得以保留**。

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

## 演示 1——Alice 1 人（server.cpp）

历经 5 轮逐步演化。用户自然地从事实性问题→多视角问题过渡，harness 遵循 简单→fan-out 的演化。

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

## 演示 2——多客户（server_multi.cpp）⭐

**真正的影响在这里。** 5 个客户展现出不同的行为模式，各自拥有独立的演化时间线。演示涌现式集群发现。

每个客户的行为模式假设与实际结果：

| 客户 | 行为模式 | 假设 | 实际演化 | 验证 |
|---|---|---|---|---|
| **alice** | 渐进式（事实性→多视角） | 中途 fan-out | `simple → fanout(t3)` | ✅ |
| **bob** | 仅事实性问题（"X 是什么？" × 5） | 简单的维护 | `simple` 全部 5 轮 | ✅ |
| **charlie** | 追求准确性（“验证你的答案”） | 反射式 | `simple → reflexive(t1)` 立即 | ✅ |
| **david** | 一开始就“从多角度对比X与Y” | 快速fan-out | `simple → fanout(t1)` 立即 | ✅ |
| **eve** | 混合（事实型 ↔ 多视角 ↔ 谨慎振荡） | 振荡风险 | `simple → fanout(t2) → reflexive(t4) → fanout(t5)` **振荡** | ✅ |

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

1. **行为模式假设4/5准确验证** — 人类预测的演化路径与LLM 审判者的实际演化决策完全契合。即 **LLM 审判者能可靠且无偏见地检测用户的意图转换。**

2. **实际观测到Eve的振荡 ⚠️** — 话语序列 [事实 → 多角度 → 事实 → 谨慎 → 事实] 导致拓扑振荡 [简单 → fan-out → fan-out(保持) → 反身 → fan-out]。**需要防抖动保护**，须由数据验证（冷却或滞后增强）。

3. **涌现式集群发现** — 5位客户的多样话语模式自然归类为 **3个拓扑集群**。编译缓存大小 = 3 = 不同集群数量。

**这是真正有趣的涌现属性** — NG的图即数据天然成为客户行为集群发现机制。graph_def分布 = 客户行为的本质集群形状。

4. **内存效率** — 5 客户 → 3 引擎。通过缓存共享节省 2 个客户的(引擎)内存。**扩展至 1000 客户，如果不同形状收敛至约 10 个，引擎内存保持近常量 → 真实 1000+ 客户多租户可容纳于单进程。**

5. **顺序模拟仅需 7 分钟墙钟时间** — 生产环境中，每个客户独立，因此可并行。5 个客户并行 ≈ 1.5 分钟 + 编译缓存并发访问安全(`std::shared_mutex`)，因此无竞态。

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

每个请求处理流程：

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

- **防抖动保护(anti-oscillation guard)** — 处理边缘情况。若在最近 N 轮中已演进，则锁定；或采用滞回（若当前拓扑不比下一个候选者低 N%，则不更改）。
- **LLM生成的graph_def** — 当前从 3 个预定义拓扑中选择。更雄心勃勃的方案是，LLM 可以完全生成 graph_def JSON。[`the-beast/`](../the-beast/) 菜谱演示了相同的模型作者拓扑及编译/验证门控。
- **并行客户处理** — 顺序演示 7 分钟，按客户并行 = 约 1.5 分钟。直接使用 `asio::thread_pool` + 编译缓存。
- **A/B框架** — 同时对同一客户运作 2 个拓扑，按响应满意度决定胜者。以 graph_id 粘性分配。
- **CheckpointStore 集成** — Postgres + 上述 SQL schema，用于真实生产就绪。
- **自适应演进速率** — 根据客户历史稳定性调整评估间隔（稳定 = 每 10 轮，不稳定 = 每轮）。

## Core

> **自演进 + 多租户组合是 NG (NeoGraph) 的真正本质。**“AI 代理
> “自我构建”的系统借助NG的图即数据范式**切实可行**。
> LLM 输出其自身的 harness → 数据库更新 → 立即应用 — 形成闭环路径，用于
> 相比 LangGraph 的 StateGraph-as-Python 模型，NG 是**该市场中唯一的玩家**。
>
> *“5 个客户 × 5 轮 = 19 MB / 3 个互引擎 / 涌现簇
> /发现 / 振荡诊断。这是真实自我改进多租户智能体架构的起点
> *基石。”*
