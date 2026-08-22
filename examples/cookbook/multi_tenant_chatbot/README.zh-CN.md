<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=zh-CN source_sha256=81fc54c9666570230243c6bd69b2cca0784ec3e43705a29f8e2c797c7a33b964 -->
# 多租户聊天机器人服务器

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**一个进程同时为N个客户提供N种不同的智能体拓扑。** 测量结果：1000个并发真实OpenAI调用 / 6个客户 / 3种拓扑 / **峰值29 MB / 0错误**。

> “如何运行一个聊天机器人SaaS，让100个客户各自使用不同的
> 智能体框架——ReAct、Plan&Execute、fan-out、reflexive……？”
>
> LangGraph答案：每个客户启动一个进程。100个客户 = 100个进程 =
> 约8 GB + supervisord/k8s。
>
> NeoGraph答案：**将每个客户的graph_def JSON行放入数据库，
> 一个编译缓存条目，就完成了。** 每个进程占用不到30 MB。

本手册是该结构的一个可运行的最小实现。

## 场景

6个客户使用3种不同的拓扑：

| 客户 | 拓扑 | 形状 | LLM调用/请求 |
|---|---|---|---|
| alice, bob | simple | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| eve, frank | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3（并行） |

每个客户的 graph_def 都以内联 JSON 定义，但真实生产环境会直接将其存储为 Postgres `customer_graphs.graph_def JSONB` 行。

Core 代码流程（[server.cpp](server.cpp:140-176)）：

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

共享相同拓扑的客户共享引擎实例。客户图修改会改变哈希值，触发新的引擎编译和缓存。

## 构建/运行

### 模拟提供程序版本（无外部依赖）

```bash
cmake --build build --target cookbook_multi_tenant_mock
./build/cookbook_multi_tenant_mock
```

无需 OpenAI 键即可工作。衡量 NG engine capacity（1000 并发请求 / 编译缓存命中率 / 内存）。

### 实时 LLM 版本（OpenRouter DeepSeek）

```bash
# .env must contain OPENROUTER_API_KEY at repo root
cmake --build build --target cookbook_multi_tenant_live
./build/cookbook_multi_tenant_live
```

**成本 ≈ 提供商相关**（通过固定的 DeepSeek 路由进行 2330 次调用）。

## 测量值

| 方面 | 模拟 1000 个请求 | 实时 100 个请求 | **实时 1000 个请求** |
|---|---|---|---|
| 正常 / 错误 | 1000 / 0 | 100 / 0 | **1000 / 0** ⭐ |
| 实际耗时 | 5 毫秒 | 11.5 秒 | 50.2 秒 |
| 平均延迟 | 39 微秒 | 1.58 秒 | 1.4 秒 |
| 最大延迟 | 2.99 毫秒 | 9.33 秒 | 14.4 秒 |
| 吞吐量 | 200K RPS | 8.67 RPS | **19.9 RPS** |
| **峰值RSS** | **5.25 MB** | **21.9 MB** | **29.25 MB** |
| 编译缓存命中率 | 99.7% | 94% | **99.4%** |
| 不同的引擎 | 3 | 6 | 6 |

**测量环境**：WSL2 / 32线程 asio 线程池 / 单主机 / 真实 OpenRouter DeepSeek API 调用。

关键数字：

- **1000个并发在途 LLM 协程 + 连接内存成本约 29 MB**。100 请求 → 1000 请求增加 +7 MB ⇒ 每个额外连接约 8 KB。asio 协程 + httplib SSL 连接池的组合。
- **1000 并发时零错误** — NG 优雅地吸收速率限制 / 网络抖动 / TLS 握手抖动而无需重试。可以用 `RateLimitedProvider` 包装器增强提供商侧限流。
- **缓存命中率 99.4%** — 即使客户更多，只要拓扑数量保持不变，命中率也能维持。**1000 客户场景内存也保持在约 30 MB**。

## LangGraph 对比 — 真实意义

使用 LangGraph 尝试相同的多租户场景会遇到这些瓶颈：

| 方面 | NeoGraph | LangGraph 估算 |
|---|---|---|
| N个客户 × N个拓扑在一个进程中 | **是**（29 MB / 1000 次请求） | 否 — StateGraph 是 Python 对象，序列化/存储不便（pickle 捆绑导入路径） |
| 客户特定的拓扑变更 | 一次数据库行更新 | 代码 PR → CI → 部署周期 |
| 版本隔离（客户 A 的 v1/v2 图共存） | 添加`graph_versions`行 | Python 命名空间冲突，需要变通方案 |
| 多进程强制 | 不必要 | 客户 = 进程常见模式 |
| 内存（6 个客户） | 29 MB | 6 × ~80 MB = 480 MB（LG 空闲基线） |
| 内存（1000 位客户） | ~30 MB（缓存不变） | **~80 GB**（每位客户一个进程） |
| 运营基础设施 | 单一二进制文件 | gunicorn / supervisord / k8s + 进程编排 |

**每个进程 30 MB vs 80 GB。** 2700× 的差异是真正多租户聊天机器人 SaaS 运营的核心。

## 实际场景——它能走多远

在 `t2.micro`（1 vCPU / 1 GB RAM，约 $0.01/小时）上可行的场景：

| 场景 | NG 内存估算 | 在 t2.micro 上可行？ |
|---|---|---|
| 100 个并发在途 LLM 请求 + 100 位客户 × 3 种拓扑 | ~10 MB | ✅ 充足，剩余约 990 MB |
| 1000 个并发在途请求 + 1000 个客户 × 10 个拓扑 | 约 30 MB | ✅ 充足，剩余约 970 MB |
| 10,000 个并发在途请求 + 10,000 个客户 × 100 个拓扑 | 约 85 MB | ✅ 充足，剩余约 915 MB |
| 100,000 个并发在途请求 + ... | 约 800 MB | ⚠️ 内存几乎用尽 |

* 假设：每个连接约 8 KB + 每个编译缓存条目约 10 KB + 5 MB 基础

当然，OpenRouter 速率限制是吞吐量的上限；**要点是边际客户成本约为 0**。

> LangGraph 在 t2.micro 1 GB 上，100 个客户 = 100 个进程 =
> 需 8 GB → 实例本身无法启动。**需 m5.2xlarge (32 GB, 约 $0.38/小时) 。**
>
> 使用 NG 完成相同任务 = **单个 t2.micro（$0.01/小时）。38× 基础设施
> 成本差异。**

## 热切换演示

`server.cpp` 结束处展示了 alice 的拓扑从 `simple` → `fanout` 的就地变更，并立即处理下一个请求。0 个部署周期，0 次重启。真实生产环境会是客户在 Web UI 中编辑图 JSON → 数据库保存 → 下一个请求使用新拓扑。

## 未来增强功能

- **CheckpointStore 集成** — 当前每次请求将历史记录作为输入传入。借助 Postgres CheckpointStore，可按 thread_id 自动持久化。
- **固定 Provider** — 每个客户 PR都使用相同的 OpenRouter DeepSeek 模型；`NodeContext::provider` 仍可携带客户特定上下文。
- **流式响应** — `run(input)` 与 `input.stream_cb` 结合SSE实现token级流式传输。直接使用NG的`run(NodeInput)`路径及流回调。
- **A/B 实验框架** — 通过_graph_def 哈希值 + customer_id 固定分拆实现流量拆分。直接扩展代码模式。Code pattern ⟪P4804b8b29a4- 直接复用已有模式。
- **360 页** **流式处理+取消集成** — 客户端断开时中止出站 LLM socket。直接接线 NG 的 `RunConfig::cancel_token`。

## Core

> *6000 个客户 × 3 种拓扑 = 29 MB。编辑一行 JSON = 热替换，无需部署。
> 0 个错误在 1000 并发 real OpenAI 调用下。可在单 t2.micro 上运行。

这一行可能是NeoGraph比性能数据更有影响力的卖点(`5.5 MB L3 fit / 1024 worker idle 31 MB`)。
