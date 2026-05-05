// PR 2 (v0.4.0) — GraphNode::run(NodeInput) -> awaitable<NodeOutput>
// regression. Three concerns:
//
//   1. A node that overrides ``run()`` directly fires through
//      NodeExecutor's dispatch path and observes the ``ctx``
//      threaded by the engine (thread_id, stream_mode, step).
//   2. A legacy node that overrides only ``execute()`` (no ``run``)
//      keeps working — default ``run()`` forwards to
//      ``execute_full_async`` → ``execute_full`` → ``execute``.
//   3. On the streaming path, ``in.stream_cb`` is non-null and the
//      override can emit ``LLM_TOKEN`` events through it.

#include <gtest/gtest.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <atomic>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Shared observation slot — the factory lambda captures a
// shared_ptr<RunObs> by value, every node it produces records into the
// same slot, the test asserts on it after run().
struct RunObs {
    std::atomic<int> invocations{0};
    std::string      thread_id;
    int              step        = -1;
    int              stream_mode = -1;
    bool             had_cb      = false;
};

class RunOverrideNode : public GraphNode {
public:
    RunOverrideNode(std::string name, std::shared_ptr<RunObs> obs)
        : name_(std::move(name)), obs_(std::move(obs)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        obs_->invocations.fetch_add(1, std::memory_order_acq_rel);
        obs_->thread_id   = in.ctx.thread_id;
        obs_->step        = in.ctx.step;
        obs_->stream_mode = static_cast<int>(in.ctx.stream_mode);
        obs_->had_cb      = (in.stream_cb != nullptr);

        if (in.stream_cb) {
            (*in.stream_cb)(GraphEvent{
                GraphEvent::Type::LLM_TOKEN, name_, json("from-run-override")});
        }

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages",
            json::array({json{{"role", "assistant"},
                              {"content", "ran via override"}}})});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string             name_;
    std::shared_ptr<RunObs> obs_;
};

// Legacy node: only overrides execute(). Default run() must forward
// down through execute_full_async → execute_full → execute.
class LegacyExecuteOnlyNode : public GraphNode {
public:
    LegacyExecuteOnlyNode(std::string name, std::shared_ptr<RunObs> obs)
        : name_(std::move(name)), obs_(std::move(obs)) {}

    std::vector<ChannelWrite> execute(const GraphState&) override {
        obs_->invocations.fetch_add(1, std::memory_order_acq_rel);
        return {ChannelWrite{"messages",
            json::array({json{{"role", "assistant"},
                              {"content", "ran via legacy execute()"}}})}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string             name_;
    std::shared_ptr<RunObs> obs_;
};

// Compile a one-node graph driven by ``factory_type``. The caller
// pre-registers the factory; this helper just builds the JSON and
// compiles it.
std::unique_ptr<GraphEngine> make_one_node_engine(
    const std::string& node_name, const std::string& factory_type) {
    json def;
    def["name"]     = "test_run_dispatch_graph";
    def["channels"] = {{"messages", {{"reducer", "append"}}}};
    def["nodes"]    = {{node_name, {{"type", factory_type}}}};
    def["edges"]    = json::array({
        json{{"from", "__start__"}, {"to", node_name}},
        json{{"from", node_name},   {"to", "__end__"}}});
    return GraphEngine::compile(def, NodeContext{});
}

} // namespace

TEST(NodeRunDispatch, OverrideRunReceivesEngineThreadedContext) {
    auto obs = std::make_shared<RunObs>();
    NodeFactory::instance().register_type(
        "test_run_dispatch:override_basic",
        [obs](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<RunOverrideNode>(name, obs);
        });

    auto engine = make_one_node_engine(
        "worker", "test_run_dispatch:override_basic");

    RunConfig cfg;
    cfg.thread_id   = "tid-run-override";
    cfg.stream_mode = StreamMode::EVENTS;
    auto result = engine->run(cfg);

    EXPECT_EQ(obs->invocations.load(), 1);
    EXPECT_EQ(obs->thread_id, "tid-run-override");
    EXPECT_EQ(obs->stream_mode, static_cast<int>(StreamMode::EVENTS));
    EXPECT_GE(obs->step, 0);
    // Non-streaming run — the engine must hand the override a null cb.
    EXPECT_FALSE(obs->had_cb);

    // The override's writes landed.
    ASSERT_TRUE(result.output.contains("channels"));
    const auto& msgs = result.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0]["content"], "ran via override");
}

TEST(NodeRunDispatch, OverrideRunSeesStreamCbOnStreamingPath) {
    auto obs = std::make_shared<RunObs>();
    NodeFactory::instance().register_type(
        "test_run_dispatch:override_stream",
        [obs](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<RunOverrideNode>(name, obs);
        });

    auto engine = make_one_node_engine(
        "streamy", "test_run_dispatch:override_stream");

    std::vector<std::string> tokens;
    auto cb = [&tokens](const GraphEvent& ev) {
        if (ev.type == GraphEvent::Type::LLM_TOKEN) {
            tokens.push_back(ev.data.get<std::string>());
        }
    };

    RunConfig cfg;
    cfg.thread_id   = "tid-streamy";
    cfg.stream_mode = StreamMode::TOKENS;
    auto result = engine->run_stream(cfg, cb);

    EXPECT_EQ(obs->invocations.load(), 1);
    EXPECT_TRUE(obs->had_cb)
        << "streaming run must hand the override a non-null stream_cb";
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "from-run-override");
}

TEST(NodeRunDispatch, LegacyExecuteOnlyNodeStillWorksViaDefaultRun) {
    // A node that does NOT override run() — default run() must
    // forward through execute_full_async → execute_full → execute.
    auto obs = std::make_shared<RunObs>();
    NodeFactory::instance().register_type(
        "test_run_dispatch:legacy_execute_only",
        [obs](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<LegacyExecuteOnlyNode>(name, obs);
        });

    auto engine = make_one_node_engine(
        "legacy", "test_run_dispatch:legacy_execute_only");

    RunConfig cfg;
    cfg.thread_id = "tid-legacy";
    auto result = engine->run(cfg);

    EXPECT_EQ(obs->invocations.load(), 1)
        << "default GraphNode::run() must forward to legacy 8-virtual chain";

    ASSERT_TRUE(result.output.contains("channels"));
    const auto& msgs = result.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0]["content"], "ran via legacy execute()");
}
