#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/context_store.h>
#include <neograph/controlled_provider.h>
#include "fixtures/typed_provider.h"
#include <codecs/messages.h>
#include <core/native.h>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>

using namespace neograph;
using namespace neograph::graph;

// ── Helper: minimal graph JSON ──

static json make_linear_graph(const std::string& node_name = "worker") {
    return {
        {"name", "test_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {node_name, {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}}
        }}
    };
}

static json make_conditional_graph() {
    return {
        {"name", "cond_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"router",  {{"type", "custom"}}},
            {"path_a",  {{"type", "custom"}}},
            {"path_b",  {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "router"}},
            {{"from", "router"}, {"condition", "route_channel"},
             {"routes", {{"a", "path_a"}, {"b", "path_b"}}}},
            {{"from", "path_a"}, {"to", "__end__"}},
            {{"from", "path_b"}, {"to", "__end__"}}
        }}
    };
}

// ── Custom node that writes to result ──

class EchoNode : public GraphNode {
public:
    EchoNode(const std::string& name, const std::string& value)
        : name_(name), value_(value) {}

    asio::awaitable<NodeOutput> run(NodeInput /*in*/) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json(value_)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::string value_;
};

// ── Router node that writes to __route__ ──

class RouterNode : public GraphNode {
public:
    RouterNode(const std::string& name, const std::string& route)
        : name_(name), route_(route) {}

    asio::awaitable<NodeOutput> run(NodeInput /*in*/) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"__route__", json(route_)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::string route_;
};

// ── Test fixture ──

class GraphEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register custom node type for tests
        NodeFactory::instance().register_type("custom",
            [](const std::string& name, const json& /*config*/, const NodeContext& /*ctx*/) {
                return std::make_unique<EchoNode>(name, "done_by_" + name);
            });
    }
};

// ── Linear execution ──

TEST_F(GraphEngineTest, LinearExecution) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 1);
    EXPECT_EQ(result.execution_trace[0], "worker");
}

// ── Result channel written ──

TEST_F(GraphEngineTest, ResultChannelWritten) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_TRUE(result.output.contains("channels"));
    auto channels = result.output["channels"];
    ASSERT_TRUE(channels.contains("result"));
    EXPECT_EQ(channels["result"]["value"], "done_by_worker");
}

// ── Conditional routing ──

TEST_F(GraphEngineTest, ConditionalRoutingA) {
    // Override to route to "a"
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "a");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[0], "router");
    EXPECT_EQ(result.execution_trace[1], "path_a");
}

TEST_F(GraphEngineTest, ConditionalRoutingB) {
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "b");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[1], "path_b");
}

// ── Max steps safety ──

TEST_F(GraphEngineTest, MaxStepsLimit) {
    // Create a cycle: worker -> worker (via condition always routing back)
    json cycle_graph = {
        {"name", "cycle"},
        {"channels", {
            {"result", {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"looper", {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "looper"}},
            {{"from", "looper"}, {"to", "looper"}}
        }}
    };

    auto engine = GraphEngine::compile(cycle_graph, NodeContext{});
    RunConfig config;
    config.max_steps = 5;
    auto result = engine->run(config);

    ASSERT_EQ(result.execution_trace.size(), 5u);
    EXPECT_TRUE(result.max_steps_exhausted());
    ASSERT_TRUE(result.output.contains("_neograph"));
    EXPECT_EQ(result.output["_neograph"]["max_steps_exhausted"], true);
}

TEST_F(GraphEngineTest, ExactBoundaryCompletionIsNotExhausted) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    config.max_steps = 1;
    auto result = engine->run(config);

    ASSERT_EQ(result.execution_trace.size(), 1u);
    EXPECT_FALSE(result.max_steps_exhausted());
    EXPECT_FALSE(result.output.contains("_neograph"));
}

TEST_F(GraphEngineTest, ZeroMaxStepsWithPendingStartWorkIsExhausted) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    config.max_steps = 0;
    auto result = engine->run(config);

    EXPECT_TRUE(result.execution_trace.empty());
    EXPECT_TRUE(result.max_steps_exhausted());
    ASSERT_TRUE(result.output.contains("_neograph"));
    EXPECT_EQ(result.output["_neograph"]["max_steps_exhausted"], true);
}

TEST_F(GraphEngineTest, StartToEndWithZeroMaxStepsIsNotExhausted) {
    json empty_graph = {
        {"name", "empty"},
        {"channels", {{"result", {{"reducer", "overwrite"}}}}},
        {"nodes", json::object()},
        {"edges", {{{"from", "__start__"}, {"to", "__end__"}}}}
    };
    auto engine = GraphEngine::compile(empty_graph, NodeContext{});
    RunConfig config;
    config.max_steps = 0;
    auto result = engine->run(config);

    EXPECT_TRUE(result.execution_trace.empty());
    EXPECT_FALSE(result.max_steps_exhausted());
    EXPECT_FALSE(result.output.contains("_neograph"));
}

// ── Empty input ──

TEST_F(GraphEngineTest, EmptyInput) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    // No input — should still execute
    auto result = engine->run(config);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(result.execution_trace.size(), 1);
}

// ── Concurrency: shared engine, distinct thread_ids ──
//
// Verifies that one GraphEngine instance can serve many concurrent run()
// calls with different thread_ids without cross-contamination, races, or
// checkpoint store corruption. This is the property we want to document
// for users who want to host multi-tenant agent workloads on top of
// NeoGraph without writing a separate async layer.

namespace {

// Stateless node that echoes the "input_val" channel * 2 into "result"
// with a small sleep to widen the race window.
class DoublerNode : public GraphNode {
public:
    DoublerNode(const std::string& name, std::atomic<int>* counter)
        : name_(name), counter_(counter) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json v = in.state.get("input_val");
        int input = v.is_number_integer() ? v.get<int>() : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (counter_) counter_->fetch_add(1, std::memory_order_relaxed);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json(input * 2)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int>* counter_;
};

static json make_doubler_graph() {
    return {
        {"name", "doubler"},
        {"channels", {
            {"input_val", {{"reducer", "overwrite"}}},
            {"result",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"doubler", {{"type", "doubler"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "doubler"}},
            {{"from", "doubler"},   {"to", "__end__"}}
        }}
    };
}

} // namespace

TEST_F(GraphEngineTest, ConcurrentRunDifferentThreadIds) {
    static std::atomic<int> g_counter{0};
    g_counter = 0;

    NodeFactory::instance().register_type("doubler",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<DoublerNode>(name, &g_counter);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_doubler_graph(), NodeContext{}, store);

    constexpr int N_THREADS       = 16;
    constexpr int RUNS_PER_THREAD = 25;

    std::vector<std::future<std::string>> futures;
    futures.reserve(N_THREADS);

    for (int t = 0; t < N_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&engine, t]() -> std::string {
            for (int j = 0; j < RUNS_PER_THREAD; ++j) {
                int input_val = t * 1000 + j;
                RunConfig cfg;
                cfg.thread_id = "tid_" + std::to_string(t) + "_" + std::to_string(j);
                cfg.input = {{"input_val", input_val}};

                RunResult r;
                try {
                    r = engine->run(cfg);
                } catch (const std::exception& e) {
                    return std::string("threw: ") + e.what();
                }
                if (r.interrupted) return "unexpected interrupt";
                if (!r.output.contains("channels")) return "missing channels";
                auto ch = r.output["channels"];
                if (!ch.contains("result")) return "missing result channel";
                int got = ch["result"]["value"].get<int>();
                int want = input_val * 2;
                if (got != want) {
                    return "mismatch: got=" + std::to_string(got) +
                           " want=" + std::to_string(want);
                }
            }
            return "";
        }));
    }

    for (int t = 0; t < N_THREADS; ++t) {
        std::string err = futures[t].get();
        EXPECT_TRUE(err.empty()) << "thread " << t << ": " << err;
    }

    EXPECT_EQ(g_counter.load(), N_THREADS * RUNS_PER_THREAD);

    // Each unique thread_id should have its own checkpoint(s) in the store.
    for (int t = 0; t < N_THREADS; ++t) {
        for (int j = 0; j < RUNS_PER_THREAD; ++j) {
            std::string tid = "tid_" + std::to_string(t) + "_" + std::to_string(j);
            auto cps = store->list(tid, 100);
            EXPECT_FALSE(cps.empty()) << "no checkpoints for " << tid;
        }
    }
}

TEST_F(GraphEngineTest, ConcurrentRunSameThreadIdNoCrash) {
    // Hammer one thread_id from many threads simultaneously. We don't
    // require deterministic semantics here (LangGraph doesn't either),
    // only that the engine + checkpoint store don't corrupt or crash.
    static std::atomic<int> g_counter{0};
    g_counter = 0;

    NodeFactory::instance().register_type("doubler",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<DoublerNode>(name, &g_counter);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_doubler_graph(), NodeContext{}, store);

    constexpr int N_THREADS = 8;
    constexpr int RUNS = 50;

    std::vector<std::future<bool>> futures;
    for (int t = 0; t < N_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&engine, t]() {
            for (int j = 0; j < RUNS; ++j) {
                RunConfig cfg;
                cfg.thread_id = "shared_tid";
                cfg.input = {{"input_val", t * 100 + j}};
                try {
                    auto r = engine->run(cfg);
                    if (r.interrupted) return false;
                } catch (...) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& f : futures) EXPECT_TRUE(f.get());
    EXPECT_EQ(g_counter.load(), N_THREADS * RUNS);
}

namespace {

using neograph::test::LocalProvider;
class BrokerProbeProvider final : public LocalProvider {
    struct State { std::atomic<unsigned> calls{0}; };
    explicit BrokerProbeProvider(std::shared_ptr<State> state)
        : LocalProvider([state](ProviderRequest, const PreparedProviderRequest&,
                               const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            ++state->calls;
            co_return neograph::test::success("provider");
        }, "broker-probe"), state_(std::move(state)), calls(state_->calls) {}
    std::shared_ptr<State> state_;
public:
    BrokerProbeProvider() : BrokerProbeProvider(std::make_shared<State>()) {}
    std::atomic<unsigned>& calls;
};

class RecordingProviderCallBroker final : public ProviderCallBroker {
public:
    bool replay = false;
    bool fail_first = false;

    asio::awaitable<sp::runtime::Result> invoke(ProviderCallIdentity identity,
                                            std::shared_ptr<Provider> provider,
                                            ProviderRequest request) override {
        {
            std::lock_guard lock(mutex_);
            identities_.push_back(std::move(identity));
            if (fail_first && identities_.size() == 1) {
                throw std::runtime_error("pre-dispatch broker failure");
            }
        }
        if (replay) co_return neograph::test::success("replayed");
        co_return co_await provider->invoke_async(std::move(request));
    }

    std::vector<ProviderCallIdentity> identities() const {
        std::lock_guard lock(mutex_);
        return identities_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ProviderCallIdentity> identities_;
};

json broker_probe_graph() {
    return {{"name", "broker-probe"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"reason", {{"type", "llm_call"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "reason"}},
                                     {{"from", "reason"}, {"to", "__end__"}}})}};
}

json broker_classifier_graph() {
    return {{"name", "broker-classifier"},
            {"channels", {{"messages", {{"reducer", "append"}}},
                          {"__route__", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"router", {{"type", "intent_classifier"},
                                    {"prompt", "Choose shopping or support"},
                                    {"routes", json::array({"shopping", "support"})}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "router"}},
                                     {{"from", "router"}, {"to", "__end__"}}})}};
}

RunResult run_broker_probe(GraphEngine& engine, std::string thread_id,
                           std::string run_id,
                           std::shared_ptr<ProviderCallBroker> broker,
                           bool streaming = true) {
    RunConfig config;
    config.thread_id = std::move(thread_id);
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
    RunMetadata metadata;
    metadata.owner_scope = "tenant:probe";
    metadata.run_id = std::move(run_id);
    RunResources resources;
    resources.provider_call_broker = std::move(broker);
    if (!streaming) {
        return neograph::async::run_sync(engine.run_async(
            std::move(config), std::move(metadata), std::move(resources)));
    }
    return neograph::async::run_sync(engine.run_stream_async(
        std::move(config), {}, std::move(metadata), std::move(resources)));
}

}  // namespace

TEST(GraphProviderBrokerTest, ReplaysWithoutTransportAndKeepsConcurrentRunsScoped) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto engine = GraphEngine::compile(broker_probe_graph(), context);
    auto replay_broker = std::make_shared<RecordingProviderCallBroker>();
    replay_broker->replay = true;
    auto dispatch_broker = std::make_shared<RecordingProviderCallBroker>();

    auto first = std::async(std::launch::async, [&] {
        return run_broker_probe(*engine, "thread-one", "run-one", replay_broker);
    });
    auto second = std::async(std::launch::async, [&] {
        return run_broker_probe(*engine, "thread-two", "run-two", dispatch_broker);
    });
    EXPECT_FALSE(first.get().interrupted);
    EXPECT_FALSE(second.get().interrupted);
    EXPECT_EQ(provider->calls.load(), 1U);

    const auto replayed = replay_broker->identities();
    const auto dispatched = dispatch_broker->identities();
    ASSERT_EQ(replayed.size(), 1U);
    ASSERT_EQ(dispatched.size(), 1U);
    EXPECT_EQ(replayed[0].owner_scope, "tenant:probe");
    EXPECT_EQ(replayed[0].run_id, "run-one");
    EXPECT_EQ(replayed[0].thread_id, "thread-one");
    EXPECT_EQ(dispatched[0].run_id, "run-two");
    EXPECT_EQ(dispatched[0].thread_id, "thread-two");
    EXPECT_EQ(replayed[0].task_id, dispatched[0].task_id);
    EXPECT_FALSE(replayed[0].task_id.empty());
    EXPECT_EQ(replayed[0].node_name, "reason");
}

TEST(GraphProviderBrokerTest, NodeRetryKeepsOneLogicalTaskIdentity) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto engine = GraphEngine::compile(broker_probe_graph(), context);
    RetryPolicy retry;
    retry.max_retries = 1;
    retry.initial_delay_ms = 0;
    engine->set_node_retry_policy("reason", retry);
    auto broker = std::make_shared<RecordingProviderCallBroker>();
    broker->fail_first = true;

    EXPECT_FALSE(run_broker_probe(*engine, "retry-thread", "retry-run", broker).interrupted);
    const auto seen = broker->identities();
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0].task_id, seen[1].task_id);
    EXPECT_EQ(seen[0].thread_id, seen[1].thread_id);
    EXPECT_EQ(provider->calls.load(), 1U);
}

TEST(GraphProviderBrokerTest, ExactCheckpointReplayKeepsCallSlotAcrossEngineRebuild) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto interrupted_graph = broker_probe_graph();
    interrupted_graph["interrupt_before"] = json::array({"reason"});
    auto seed = GraphEngine::compile(interrupted_graph, context, checkpoints);
    RunConfig config;
    config.thread_id = "checkpoint-broker-thread";
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
    RunMetadata metadata;
    metadata.owner_scope = "tenant:probe";
    metadata.run_id = "checkpoint-broker-run";
    RunResources seed_resources;
    seed_resources.provider_call_broker = std::make_shared<RecordingProviderCallBroker>();
    const auto interrupted = neograph::async::run_sync(seed->run_async(config, metadata, seed_resources));
    ASSERT_TRUE(interrupted.interrupted);
    ASSERT_FALSE(interrupted.checkpoint_id.empty());

    const auto replay = [&]() {
        auto engine = GraphEngine::compile(interrupted_graph, context, checkpoints);
        auto broker = std::make_shared<RecordingProviderCallBroker>();
        broker->replay = true;
        RunResources resources;
        resources.provider_call_broker = broker;
        const auto result = neograph::async::run_sync(engine->resume_from_async(
            config, interrupted.checkpoint_id, {}, {}, metadata, resources));
        EXPECT_FALSE(result.interrupted);
        const auto identities = broker->identities();
        EXPECT_EQ(identities.size(), 1U);
        return identities.empty() ? ProviderCallIdentity{} : identities.front();
    };
    const auto first = replay();
    const auto rebuilt = replay();
    EXPECT_FALSE(first.task_id.empty());
    EXPECT_EQ(first.task_id, rebuilt.task_id);
    EXPECT_EQ(first.thread_id, rebuilt.thread_id);
    EXPECT_EQ(first.run_id, rebuilt.run_id);
    EXPECT_EQ(provider->calls.load(), 0U);
}

TEST(GraphProviderBrokerTest, NonStreamingRunCanUseInvocationBroker) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto engine = GraphEngine::compile(broker_probe_graph(), context);
    auto broker = std::make_shared<RecordingProviderCallBroker>();
    broker->replay = true;

    EXPECT_FALSE(run_broker_probe(*engine, "nonstream-thread", "nonstream-run",
                                  broker, false).interrupted);
    EXPECT_EQ(provider->calls.load(), 0U);
    ASSERT_EQ(broker->identities().size(), 1U);
}

TEST(GraphProviderBrokerTest, ConflictingStrictInterpositionFailsClosed) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto engine = GraphEngine::compile(broker_probe_graph(), context);
    auto strict = std::make_shared<RuntimeInterpositionController>(
        provider, std::make_shared<InMemoryContextStore>(),
        std::make_shared<InMemoryProviderDispatchReceiptStore>(),
        "sha256:" + std::string(64, 'a'));
    engine->set_runtime_interposition(strict);
    auto broker = std::make_shared<RecordingProviderCallBroker>();

    EXPECT_THROW(run_broker_probe(*engine, "strict-thread", "strict-run", broker),
                 std::exception);
    EXPECT_EQ(provider->calls.load(), 0U);
    EXPECT_TRUE(broker->identities().empty());
}

TEST(GraphProviderBrokerTest, ClassifierUsesTheSameInvocationBoundary) {
    auto provider = std::make_shared<BrokerProbeProvider>();
    NodeContext context;
    context.provider = provider;
    context.model = "probe-model";
    auto engine = GraphEngine::compile(broker_classifier_graph(), context);
    auto broker = std::make_shared<RecordingProviderCallBroker>();
    broker->replay = true;

    EXPECT_FALSE(run_broker_probe(*engine, "classifier-thread", "classifier-run", broker)
                     .interrupted);
    EXPECT_EQ(provider->calls.load(), 0U);
    const auto seen = broker->identities();
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen[0].node_name, "router");
    EXPECT_FALSE(seen[0].task_id.empty());
}

namespace {

struct NativeCheckpointProbe {
    unsigned provider_calls = 0;
    std::atomic<unsigned> tool_calls{0};
    std::vector<sp::messages::Request> requests;
    std::vector<sp::runtime::Result> outcomes;
};

// Capture an authentic native fixture before any graph dispatch authority exists.
// The actual provider operations still perform one admitted runtime preparation.
sp::runtime::Result native_tool_outcome(const sp::descriptor::ValidatedDescriptor& descriptor) {
    sp::messages::Request request;
    request.model = "fixture-model";
    request.account_scope = "fixture-account";
    request.max_tokens = 64;
    request.messages = {test::message("message", sp::Role::User)};
    request.tools = {{"read", "read fixture", test::document(R"({"type":"object"})"), {}, {}}};
    auto encoded = sp::messages::encode(descriptor, request, false);
    if (auto* error = std::get_if<sp::Error>(&encoded))
        throw std::logic_error(error->safe_message);
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator,
        std::get<sp::messages::EncodedRequest>(std::move(encoded)).context);
    codec.buffered(R"({"id":"msg-native","type":"message","role":"assistant","model":"fixture-model",
        "content":[{"type":"thinking","thinking":"inspect","signature":"fixture-signature"},
                   {"type":"tool_use","id":"call-native","name":"read","input":{"path":"a"}},
                   {"type":"text","text":"pending"}],
        "stop_reason":"tool_use","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}})", {});
    codec.finish();
    auto outcome = accumulator.take_outcome();
    if (!outcome || !std::holds_alternative<sp::Completion>(*outcome))
        throw std::logic_error("native fixture response was not admitted");
    return std::make_shared<const sp::Outcome>(std::move(*outcome));
}

class NativeCheckpointProvider final : public test::LocalProvider {
public:
    NativeCheckpointProvider(std::shared_ptr<NativeCheckpointProbe> probe,
                             sp::descriptor::ValidatedDescriptor descriptor)
        : LocalProvider(
              [probe, native = native_tool_outcome(descriptor)](
                  ProviderRequest request, const PreparedProviderRequest&,
                  const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                  if (++probe->provider_calls > 2)
                      throw std::logic_error("checkpoint replay dispatched an extra provider turn");
                  probe->requests.push_back(
                      std::get<sp::messages::Request>(std::move(request.payload)));
                  auto result = probe->provider_calls == 1
                      ? native : test::success("finished", test::usage(2, 3, 5));
                  probe->outcomes.push_back(result);
                  co_return result;
              }, "native-checkpoint",
              std::make_shared<sp::runtime::Client>(descriptor)) {}
    std::string_view family() const noexcept override { return "anthropic.messages"; }
};

class NativeCheckpointTool final : public Tool {
public:
    explicit NativeCheckpointTool(std::shared_ptr<NativeCheckpointProbe> probe)
        : probe_(std::move(probe)) {}
    ChatTool get_definition() const override {
        return {"read", "read fixture", json{{"type", "object"}}};
    }
    std::string execute(const json& args) override {
        if (args.at("path") != "a") throw std::logic_error("unexpected native tool argument");
        ++probe_->tool_calls;
        return "read-result";
    }
    std::string get_name() const override { return "read"; }
private:
    std::shared_ptr<NativeCheckpointProbe> probe_;
};

} // namespace

TEST(GraphProviderBrokerTest, SealedToolHistorySurvivesInMemoryCheckpointWithoutArchive) {
    auto probe = std::make_shared<NativeCheckpointProbe>();
    auto provider = std::make_shared<NativeCheckpointProvider>(
        probe, test::descriptor("anthropic.messages"));
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<NativeCheckpointTool>(probe));
    NodeContext context;
    context.provider = provider;
    context.model = "fixture-model";
    context.tools = ToolSet(std::move(tools));
    context.provider_controls.account_scope = "fixture-account";
    context.provider_controls.max_output_tokens = 64;
    const json definition = {
        {"name", "native-checkpoint"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"llm", {{"type", "llm_call"}}}, {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"type", "conditional"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}})},
        {"interrupt_after", json::array({"tools"})}};
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(definition, context, checkpoints);
    RunConfig config;
    config.thread_id = "native-checkpoint-thread";
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "message"}}})}};
    config.usage = std::make_shared<UsageAccumulator>();
    ASSERT_TRUE(config.usage->try_reserve(17, 100));
    const auto interrupted = engine->run(config);
    ASSERT_TRUE(interrupted.interrupted);
    EXPECT_EQ(probe->provider_calls, 1u);
    EXPECT_EQ(probe->tool_calls.load(), 1u);
    ASSERT_EQ(interrupted.native_messages.size(), 3u);
    const auto native = interrupted.native_messages[1].native;
    ASSERT_NE(native, nullptr);
    EXPECT_TRUE(native->complete());
    EXPECT_EQ(config.usage->total_tokens_wide(), 22u);
    const auto checkpoint = checkpoints->load_by_id(interrupted.checkpoint_id);
    ASSERT_TRUE(checkpoint.has_value());
    ASSERT_NE(checkpoint->native_history, nullptr);
    const auto guard = checkpoint->metadata.value("_neograph_ephemeral_guard", json());

    GraphState json_only;
    json_only.init_channel("messages", ReducerType::APPEND,
                           ReducerRegistry::instance().get("append"), json::array());
    EXPECT_THROW(json_only.restore_checkpoint(checkpoint->channel_values, guard),
                 std::invalid_argument);
    EXPECT_EQ(probe->provider_calls, 1u);
    EXPECT_EQ(probe->tool_calls.load(), 1u);
    auto tampered = checkpoint->channel_values;
    tampered["channels"]["messages"]["value"][0]["content"] = "rewritten-prefix";
    EXPECT_THROW(json_only.restore_checkpoint(tampered, guard, checkpoint->native_history),
                 std::invalid_argument);
    EXPECT_EQ(config.usage->total_tokens_wide(), 22u);
    GraphState trusted;
    trusted.init_channel("messages", ReducerType::APPEND,
                         ReducerRegistry::instance().get("append"), json::array());
    trusted.restore_checkpoint(checkpoint->channel_values, guard, checkpoint->native_history);
    const auto restored = trusted.get_provider_messages();
    ASSERT_EQ(restored.size(), 3u);
    EXPECT_EQ(restored[1].native, native);

    engine.reset();
    auto rebuilt = GraphEngine::compile(definition, context, checkpoints);
    const auto completed = rebuilt->resume_from(config, interrupted.checkpoint_id);
    EXPECT_FALSE(completed.interrupted);
    EXPECT_EQ(probe->provider_calls, 2u);
    EXPECT_EQ(probe->tool_calls.load(), 1u);
    ASSERT_EQ(probe->requests.size(), 2u);
    const auto& replayed = probe->requests[1].messages;
    ASSERT_EQ(replayed.size(), 3u);
    EXPECT_EQ(replayed[1].native, native);
    ASSERT_EQ(replayed[1].parts.size(), 3u);
    EXPECT_EQ(std::get<sp::Thinking>(replayed[1].parts[0]).signature, "fixture-signature");
    EXPECT_EQ(std::get<sp::ToolCall>(replayed[1].parts[1]).id, "call-native");
    EXPECT_EQ(std::get<sp::Text>(replayed[1].parts[2]).value, "pending");
    ASSERT_EQ(replayed[2].parts.size(), 1u);
    EXPECT_EQ(std::get<sp::ToolResult>(replayed[2].parts[0]).tool_use_id, "call-native");
    EXPECT_EQ(std::get<sp::ToolResult>(replayed[2].parts[0]).content, "read-result");
    ASSERT_EQ(completed.native_messages.size(), 4u);
    EXPECT_EQ(std::get<sp::Text>(completed.native_messages.back().parts[0]).value, "finished");
    ASSERT_EQ(completed.provider_outcomes.size(), 2u);
    EXPECT_EQ(completed.provider_outcomes[0], probe->outcomes[0]);
    EXPECT_EQ(completed.provider_outcomes[1], probe->outcomes[1]);
    ASSERT_TRUE(completed.usage.input_total.has_value());
    ASSERT_TRUE(completed.usage.output_total.has_value());
    EXPECT_EQ(completed.usage.input_total->value, 4u);
    EXPECT_EQ(completed.usage.output_total->value, 6u);
    EXPECT_EQ(config.usage->total_tokens_wide(), 27u);
}

TEST(GraphProviderBrokerTest, ManagedInMemoryBudgetSurvivesResumeAndForkWithoutRenewal) {
    auto calls = std::make_shared<unsigned>(0);
    auto provider = std::make_shared<test::LocalProvider>(
        [calls](ProviderRequest, const PreparedProviderRequest&,
                const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            co_return test::success("ok", test::usage(2, 1, 3));
        }, "managed-bank-provider", test::bounded_client("managed-model", 8));
    NodeContext context;
    context.provider = provider;
    context.model = "managed-model";
    context.provider_controls.max_output_tokens = 1;
    const json definition = {
        {"name", "managed-bank-checkpoint"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"first", {{"type", "llm_call"}}}, {"second", {{"type", "llm_call"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "first"}},
            {{"from", "first"}, {"to", "second"}},
            {{"from", "second"}, {"to", "__end__"}}})},
        {"interrupt_after", json::array({"first"})}};
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(definition, context, checkpoints);
    RunConfig config;
    config.thread_id = "managed-bank-thread";
    config.model_token_budget = 30;
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}};
    ASSERT_EQ(config.usage, nullptr);
    const auto interrupted = engine->run(config);
    ASSERT_TRUE(interrupted.interrupted);
    EXPECT_EQ(*calls, 1u);
    for (const auto& message : interrupted.native_messages) EXPECT_EQ(message.native, nullptr);
    const auto checkpoint = checkpoints->load_by_id(interrupted.checkpoint_id);
    ASSERT_TRUE(checkpoint.has_value());
    ASSERT_NE(checkpoint->native_history, nullptr);
    const auto guard = checkpoint->metadata.value("_neograph_ephemeral_guard", json());
    GraphState original;
    original.init_channel("messages", ReducerType::APPEND,
                          ReducerRegistry::instance().get("append"), json::array());
    original.restore_checkpoint(checkpoint->channel_values, guard, checkpoint->native_history);
    const auto bank = original.budget_bank();
    ASSERT_NE(bank, nullptr);
    EXPECT_EQ(original.budget_ceiling(), 30u);
    EXPECT_EQ(bank->total_tokens_wide(), 3u);
    GraphState json_only;
    json_only.init_channel("messages", ReducerType::APPEND,
                           ReducerRegistry::instance().get("append"), json::array());
    EXPECT_THROW(json_only.restore_checkpoint(checkpoint->channel_values, guard),
                 std::invalid_argument);

    const auto fork_id = engine->fork(config.thread_id, "managed-bank-fork", checkpoint->id);
    const auto forked = checkpoints->load_by_id(fork_id);
    ASSERT_TRUE(forked.has_value());
    GraphState branch;
    branch.init_channel("messages", ReducerType::APPEND,
                        ReducerRegistry::instance().get("append"), json::array());
    branch.restore_checkpoint(forked->channel_values,
        forked->metadata.value("_neograph_ephemeral_guard", json()), forked->native_history);
    EXPECT_EQ(branch.budget_bank(), bank);
    EXPECT_EQ(branch.budget_ceiling(), 30u);

    engine.reset();
    auto rebuilt = GraphEngine::compile(definition, context, checkpoints);
    auto widened = config;
    widened.model_token_budget = 31;
    EXPECT_THROW(rebuilt->resume_from(widened, checkpoint->id), std::invalid_argument);
    EXPECT_EQ(*calls, 1u);
    EXPECT_EQ(bank->total_tokens_wide(), 3u);
    auto resumed = config;
    resumed.model_token_budget = 0;
    const auto completed = rebuilt->resume_from(resumed, checkpoint->id);
    EXPECT_FALSE(completed.interrupted);
    EXPECT_EQ(*calls, 2u);
    EXPECT_EQ(bank->total_tokens_wide(), 6u);
    const auto completed_checkpoint = checkpoints->load_by_id(completed.checkpoint_id);
    ASSERT_TRUE(completed_checkpoint.has_value());
    GraphState completed_state;
    completed_state.init_channel("messages", ReducerType::APPEND,
                                ReducerRegistry::instance().get("append"), json::array());
    completed_state.restore_checkpoint(completed_checkpoint->channel_values,
        completed_checkpoint->metadata.value("_neograph_ephemeral_guard", json()),
        completed_checkpoint->native_history);
    EXPECT_EQ(completed_state.budget_bank(), bank);
    EXPECT_EQ(completed_state.budget_ceiling(), 30u);

    auto lowered = config;
    lowered.thread_id = "managed-bank-fork";
    lowered.model_token_budget = 20;
    const auto branch_completed = rebuilt->resume_from(lowered, fork_id);
    EXPECT_FALSE(branch_completed.interrupted);
    EXPECT_EQ(*calls, 3u);
    EXPECT_EQ(bank->total_tokens_wide(), 9u);
    EXPECT_EQ(branch.budget_bank()->total_tokens_wide(), 9u);
    const auto branch_checkpoint = checkpoints->load_by_id(branch_completed.checkpoint_id);
    ASSERT_TRUE(branch_checkpoint.has_value());
    GraphState lowered_state;
    lowered_state.init_channel("messages", ReducerType::APPEND,
                              ReducerRegistry::instance().get("append"), json::array());
    lowered_state.restore_checkpoint(branch_checkpoint->channel_values,
        branch_checkpoint->metadata.value("_neograph_ephemeral_guard", json()),
        branch_checkpoint->native_history);
    EXPECT_EQ(lowered_state.budget_bank(), bank);
    EXPECT_EQ(lowered_state.budget_ceiling(), 20u);
}
