<!-- neograph-i18n: source=examples/cookbook/multi_tenant_chatbot/README.md locale=zh-CN source_sha256=5b5b58f71f1129b96f7164783daefd9467d64ba6bc19df7d96ef3a99bfaf8fe6 -->
# 多租户聊天机器人服务器

## 当前源代码边界与历史测量

mock 为 provider-free；cache 绑定 topology、model、instruction、extra configuration、provider name，不使用 live cache 的 tenant/provider capability key。mock 复用结果不是 production cross-tenant 隔离证明。

C++ live 使用类型化 SDK request 与完整不可变 Outcome。engine cache identity 绑定
 tenant/topology 与捕获的 provider/model/host instruction。可信 host 的 tenant 选择、quota/store 边界、
thread 隔离保持不变。portable JSON 摘要不是 native authority。
以下数值/运行记录是历史资料，不是新迁移的执行或 live pass。
live binary 固定 **1,000 request / 32 worker**，没有低成本 small-smoke flag。
不要作为单次调用验证运行；它需要 live 密钥/network 和单独批准的大量 provider 费用。
不保证价格或 zero-error。密钥/prompt/artifact 保持私密，不公开 raw native payload。
provider retry 是一个显式 layer，默认 off；当前 public API 没有旧 throttle-provider wrapper。

`server_live_llm.cpp` 向类型化 provider factory 传递路径专用180秒 timeout。
全局默认值不变；timeout 不授予重发不确定或已观测调用的权限。

保留的 interface-3 model-free E2E 运行了专用mock workload：1,000 graph请求、error0、compiled topology3、cache hit997，
Alice topology替换复用了既有fanout engine。这是topology load/cache证据，不保证production认证、
quota、semantic response或memory capacity。isolated-host CLI输出了两个不同policy tuple，但未执行graph。


**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**一个进程同时为N个客户提供N种不同的智能体拓扑。** 测量结果：1000个并发真实OpenAI调用 / 6个客户 / 3种拓扑 / **峰值29 MB / 0错误**。

mock workload 仅在捕获配置等价时共享 compiled engine。认证、tenant 选择、store 和 quota 仍由 host 负责。下面历史 RSS 不是这些资源的上限。

本手册是该结构的一个可运行的最小实现。

## 隔离契约 (production 边界)

上面是 topology 共享测量，不是 production SaaS security/capacity 保证。已认证 ingress 创建不可变 `TenantScope`，并给每个 tenant 独立 provider/model policy、`GraphRegistry` snapshot、`ToolSet`、`ScopedStore`、`ScopedCheckpointStore`、Harness namespace 和 `TenantQuota`。

```cpp
auto backend_store = std::make_shared<InMemoryStore>(); // or a tenant DB
auto backend_checkpoints = std::make_shared<InMemoryCheckpointStore>();
TenantScope scope("tenant-a", "authz-a"); // trusted ingress only
ScopedStore store(scope, backend_store);
ScopedCheckpointStore checkpoints(scope, backend_checkpoints);
TenantQuota quota({.max_concurrency = 32, .max_queue = 64,
                   .max_model_tokens = 2'000'000, .max_artifacts = 1000});
```

公开 thread/checkpoint/run/artifact ID 可在不同 tenant 再用。scoped adapter 映射 private backend namespace，错误 tenant lookup 返回 absence。resume、replay、fork、cancellation 和 dereference 必须使用同一 ingress scope。

`CatalogConfig::materialization_context_identity` 是标识 provider/model policy、tool catalog、store 和 registry snapshot 的 non-secret host identity，属于 generation cache identity。不要把 credential、authorization token 或 topology JSON 放入 identity/cache key/journal/log/diagnostic。仅在 exact capability receipt 有意等价时省略。

mock 展示 topology 复用，不证明 tenant/provider 隔离、quota 或 production memory。production host 应为每个 tenant 保留 scoped engine/resource binding 并自行测量。

### Offline isolated-host reference

```bash
cmake --build build --target cookbook_multi_tenant_isolated_host
./build/cookbook_multi_tenant_isolated_host
```

此 CLI 输出两套不同 topology、provider/model、tool、store、quota policy tuple，不执行 graph。它用 synthetic identity，无需 network credential；production ingress/control-plane 由 deployment host 实现。

## 场景

6个客户使用3种不同的拓扑：

| 客户 | 拓扑 | 形状 | LLM调用/请求 |
|---|---|---|---|
| alice, bob | simple | `start → respond → end` | 1 |
| charlie, david | **reflexive** | `start → draft → critique → final → end` | 3 |
| eve, frank | **fanout** | `start → [perspective_a, _b, _c] → merge → end` | 3（并行） |

每个客户的 graph_def 都以内联 JSON 定义，但真实生产环境会直接将其存储为 Postgres `customer_graphs.graph_def JSONB` 行。

Core 代码流程（[server.cpp](server.cpp)）：

```cpp
class CompileCache {
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<GraphEngine>> cache_;
    std::atomic<std::size_t> hits_{0}, misses_{0};
public:
    std::shared_ptr<GraphEngine> get_or_compile(const json& def, const NodeContext& ctx) {
        // This provider-free style demo still binds all captured configuration.
        const std::string key = json::array({def, ctx.model, ctx.instructions,
            ctx.extra_config, ctx.provider_name}).dump();
        {
            std::shared_lock lk(mu_);
            if (auto it = cache_.find(key); it != cache_.end()) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Miss — compile (lock 밖에서, 다른 customer 차단 안 함).
        auto raw = GraphEngine::build(def, EngineConfig{.node_context = ctx});
        std::shared_ptr<GraphEngine> engine(raw.release());
        {
            std::unique_lock lk(mu_);
            auto [it, inserted] = cache_.emplace(key, engine);
            if (!inserted) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return it->second;  // race — 다른 thread 가 먼저 넣음
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return engine;
    }
    std::size_t hits()   const { return hits_.load(); }
    std::size_t misses() const { return misses_.load(); }
    std::size_t size()   { std::shared_lock lk(mu_); return cache_.size(); }
};

// On request arrival
auto def    = db.fetch_graph(customer_id);   // One JSONB row
auto engine = cache.get_or_compile(def, ctx);
RunConfig cfg;
cfg.thread_id = customer_id + "__" + session_id;   // Session isolation key
cfg.input     = {{"messages", json::array({{{"role", "user"}, {"content", user_message}}})}};
auto result   = engine->run(cfg);
```

共享相同拓扑的客户共享引擎实例。客户图修改会改变哈希值，触发新的引擎编译和缓存。

## 构建/运行

### Mock 版本 (无 provider 调用)

即使没有 key，native 配置/link 也需要 SchemaProvider。请先设置 `CMAKE_PREFIX_PATH` 或 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`。

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
- 历史1000 request 运行的零错误不是当前 reliability 保证。
- **Cache hit rate 99.4%** — 这是历史 workload 观察，不保证 1,000 production tenant 的 memory capacity。

## LangGraph 对比 — 真实意义

这些运行未 benchmark LangGraph。LangGraph 不要求每个客户一个 process。比较需要相同 graph、store、provider 和 isolation policy；以前的 process-per-customer 估算不是测量结果。

## 实际场景——它能走多远

六客户历史 load 不能确定 cloud instance 容量，也不能预测 10,000/100,000 connection。请用真实认证、tenant 独立资源、store、provider route 和 limit 测量 production host。SDK transport 当前验证范围是 Linux/POSIX。

## 热切换演示

`server.cpp` 结束处展示了 alice 的拓扑从 `simple` → `fanout` 的就地变更，并立即处理下一个请求。0 个部署周期，0 次重启。真实生产环境会是客户在 Web UI 中编辑图 JSON → 数据库保存 → 下一个请求使用新拓扑。

## 未来增强功能

- **CheckpointStore 集成** — 当前每次请求将历史记录作为输入传入。借助 Postgres CheckpointStore，可按 thread_id 自动持久化。
- **固定 Provider** — 每个客户 PR都使用相同的 OpenRouter DeepSeek 模型；`NodeContext::provider` 仍可携带客户特定上下文。
- **流式响应** — `run(input)` 与 `input.stream_cb` 结合SSE实现token级流式传输。直接使用NG的`run(NodeInput)`路径及流回调。
- **A/B 实验框架** — 按 graph_def hash 和 customer_id 固定分流；扩展现有代码模式。
- **流式处理+取消集成** — 连接客户端断开与 `RunConfig::cancel_token`；这不是远端模型已停止工作的保证。

## Core

保留的 mock 证据是 1,000 request、三 compiled topology 和 997 cache hit。isolated-host CLI 是 policy reference，不是 serving workload。上方 live timing 是历史记录，不是新迁移的 pass。
