<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=zh-CN source_sha256=8baffd5ea72da3575627014a32aaaf9257b389214daebaf2c5336633f74ff996 -->
# 多租户聊天机器人服务器

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**一个进程同时为 N 个客户提供 N 种不同 agent 拓扑。** 测量：1000 个并发真实 OpenAI 调用 / 6 个客户 / 3 种拓扑 / **峰值 29 MB / 0 个错误**。

> “怎样运行一个 chatbot SaaS，其中 100 个客户各自使用不同的 agent harness — ReAct、Plan&Execute、fanout、reflexive……？”
>
> LangGraph 答案：每个客户启动一个进程。100 个客户 = 100 个进程 = ~8 GB + supervisord/k8s。
>
> NeoGraph 答案：**把每个客户的一行 graph_def JSON 放进 DB，每个编译 cache entry 一份，然后就结束。** 每个进程 <30 MB。

这个 cookbook 是该结构的可运行最小实现。

## 场景

6 个客户使用 3 种不同拓扑：

| 客户 | 拓扑 | 形状 | 每请求 LLM 调用 |
|---|---|---|---|
| alice, bob | **simple** | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| eve, frank | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3（并行） |

每个客户的 graph_def 都以内联 JSON 定义，但真实生产环境会直接把它作为 Postgres `customer_graphs.graph_def JSONB` 行存储。

核心代码流程（[server.cpp](server.cpp:140-176)）：

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

共享同一 topology 的客户会共享 engine instances。客户 graph 修改会改变 hash，从而触发新的 engine compile + cache。

## 构建 / 运行

### Mock provider 版本（零外部依赖）

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

无需 OpenAI key 即可运行。测量 NG engine 容量（1000 个并发请求 / compile cache 命中率 / 内存）。

### Live LLM 版本（真实 OpenAI gpt-4o-mini）

```bash
# .env must contain OPENAI_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**成本 ≈ $0.06 / 1000 个请求**（2330 次 LLM 调用 × gpt-4o-mini 费率）。

## 测量

| 方面 | Mock 1000 请求 | Live 100 请求 | **Live 1000 请求** |
|---|---|---|---|
| 成功 / 错误 | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
| 总耗时 | 5 ms | 11.5 s | 50.2 s |
| 平均延迟 | 39 µs | 1.58 s | 1.4 s |
| 最大延迟 | 2.99 ms | 9.33 s | 14.4 s |
| 吞吐量 | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **Peak RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
| Compile cache 命中率 | 99.7% | 94% | **99.4%** |
| 不同 engine 数 | 3 | 6 | 6 |

**测量环境**：WSL2 / 32-thread asio thread pool / single host / real OpenAI API call。

关键数字：

- **1000 个并发 in-flight LLM coroutine + connection memory cost ≈ 29 MB**。100 req → 1000 req 增加 +7 MB ⇒ 每个额外 connection 约 ~8 KB。来自 asio coroutine + httplib SSL connection pool 的组合。
- **1000 并发下 0 errors** — NG 无需 retry 就能平稳吸收 rate-limit / network jitter / TLS handshake jitter。Provider-side throttle 可以用 `RateLimitedProvider` wrapper 加强。
- **Cache hit rate 99.4%** — 只要 topology count 不变，即使客户更多也保持 hit rate。**1000 customer scenario memory 也保持 ~30 MB**。

## LangGraph 对比 — 真实含义

尝试用 LangGraph 实现同一多租户场景会碰到这些瓶颈：

| 方面 | NeoGraph | LangGraph 估算 |
|---|---|---|
| 一个进程中 N 个客户 × N 种拓扑 | **可以**（29 MB / 1000 请求） | 不可以 — StateGraph 是 Python object，序列化/存储很别扭（pickle 会捆绑 import path） |
| 客户特定拓扑变更 | 一行 DB UPDATE | 代码 PR → CI → 部署周期 |
| 版本隔离（customer A 的 v1/v2 graph 共存） | 添加 `graph_versions` 行 | Python namespace 冲突，需要 hack |
| 多进程强制要求 | 不需要 | Customer = process 是常见模式 |
| 内存（6 个客户） | 29 MB | 6 × ~80 MB = 480 MB（LG idle baseline） |
| 内存（1000 个客户） | ~30 MB（cache unchanged） | **~80 GB**（每客户一个进程） |
| 运维基础设施 | 一个 binary | gunicorn / supervisord / k8s + 进程编排 |

**每进程 30 MB vs 80 GB。** 2700× 差异是真实 multi-tenant chatbot SaaS 运维的核心。

## 实际场景 — 能扩到多远

`t2.micro`（1 vCPU / 1 GB RAM，~$0.01/hour）上可行的场景：

| 场景 | NG 内存估算 | t2.micro 上可行吗？ |
|---|---|---|
| 100 concurrent active in-flight LLM + 100 customers × 3 topologies | ~10 MB | ✅ 充足，剩余 ~990 MB |
| 1000 concurrent in-flight + 1000 customers × 10 topologies | ~30 MB | ✅ 充足，剩余 ~970 MB |
| 10,000 concurrent in-flight + 10,000 customers × 100 topologies | ~85 MB | ✅ 充足，剩余 ~915 MB |
| 100,000 concurrent in-flight + … | ~800 MB | ⚠️ RAM 几乎用完 |

* 假设：每个 connection ~8 KB + 每个 compile-cache entry ~10 KB + 5 MB base。

当然，t2.micro 的 1 vCPU 和 OpenAI tier RPM limits 是 *throughput* 上限；**关键点是边际客户成本约为 0**。

> LangGraph 在 1 GB 的 t2.micro 上服务 100 个客户 = 100 个进程 =
> 需要 8 GB → 实例本身无法启动。**需要 m5.2xlarge（32 GB，~$0.38/hour）。**
>
> 同一任务用 NG = **单个 t2.micro（$0.01/hour）。38× 基础设施成本差异。**

## 热切换演示

`server.cpp` 末尾展示了将 alice 的 topology 从 `simple` → `fanout` 原地更改，并立即处理下一次请求。0 deploy cycle，0 restart。真实生产中会是客户在 web UI 中编辑 graph JSON → DB save → 下一次请求使用新 topology。

## 未来增强

- **CheckpointStore 集成** — 当前每个请求都把 history 作为输入传入。有了 Postgres CheckpointStore 后，可按 thread_id 自动持久化。
- **每客户 Provider** — alice=gpt-4o-mini、bob=claude-haiku 这种每客户不同 model/provider。NodeContext::provider 按客户改变。
- **流式响应** — `run(input)` 搭配 `input.stream_cb` + SSE 进行 token-level streaming。直接使用 NG 的 `run(NodeInput)` 路径和 stream callback。
- **A/B 实验框架** — 通过 graph_def hash + customer_id sticky split 分流。可直接扩展该代码模式。
- **Streaming + cancel integration** — 客户端断开时中止 outbound LLM socket。直接接入 NG 的 `RunConfig::cancel_token`。

## 核心信息

> *“6000 个客户 × 3 种拓扑 = 29 MB。编辑一行 JSON = 不部署即可热切换。
> 1000 个并发真实 OpenAI 调用下 0 个错误。单台 t2.micro 即可运行。”*

这一行可能比性能数字（`5.5 MB L3 fit / 1024 worker idle 31 MB`）更能体现 NeoGraph 的影响力卖点。
