// v0.3.2: TODO_v0.3.md item #10 (C++ side) — execute_stream-only
// GraphNode subclasses dispatch correctly under run_stream.
//
// The default GraphNode::execute_full_stream used to call
// execute_full(state) first to capture Command/Send, then replace the
// writes with execute_stream(state, cb). For a subclass that only
// overrode execute_stream (no execute / execute_full / execute_async /
// execute_full_async), the execute_full call hit the
// ExecuteDefaultGuard recursion check and threw — execute_stream
// never got a chance to run, even via run_stream(). This test pins
// the fix in.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <atomic>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Subclass that ONLY overrides execute_stream. No execute, no
// execute_full, no async peers. This is the natural shape for a
// streaming-aware C++ node that doesn't need Command/Send.
class StreamOnlyCppNode : public GraphNode {
public:
    explicit StreamOnlyCppNode(std::string name,
                               std::atomic<int>* counter)
        : name_(std::move(name)), counter_(counter) {}

    std::vector<ChannelWrite> execute_stream(
        const GraphState&,
        const GraphStreamCallback& cb) override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        if (cb) {
            GraphEvent ev;
            ev.type      = GraphEvent::Type::LLM_TOKEN;
            ev.node_name = name_;
            ev.data      = json("token-from-cpp");
            cb(ev);
        }
        return {ChannelWrite{"messages",
            json::array({json{{"role", "assistant"}, {"content", "ok"}}})}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* counter_;
};

json make_graph(const std::string& type_name) {
    return json{
        {"name", "stream_only_cpp"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"n", {{"type", type_name}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "n"}},
            {{"from", "n"},         {"to", "__end__"}}
        })}
    };
}

} // namespace

// =========================================================================
// run_stream() must dispatch through the user's execute_stream override
// even though the user did NOT override execute / execute_full.
// =========================================================================
TEST(ExecuteStreamOnlyDispatch, RunStreamDispatchesToOverride) {
    std::atomic<int> calls{0};
    NodeFactory::instance().register_type("stream_only_cpp",
        [&calls](const std::string& name, const json&,
                 const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<StreamOnlyCppNode>(name, &calls);
        });

    auto engine = GraphEngine::compile(
        make_graph("stream_only_cpp"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "stream-only-cpp-001";

    std::vector<json> tokens;
    auto cb = [&tokens](const GraphEvent& ev) {
        if (ev.type == GraphEvent::Type::LLM_TOKEN) {
            tokens.push_back(ev.data);
        }
    };

    auto result = engine->run_stream(cfg, cb);

    EXPECT_EQ(calls.load(), 1)
        << "execute_stream override never dispatched";
    ASSERT_EQ(tokens.size(), 1u)
        << "stream callback never received the token";
    EXPECT_EQ(tokens[0].get<std::string>(), "token-from-cpp");

    auto msgs = result.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0]["content"], "ok");
}

// =========================================================================
// Same node must STILL throw when run via the non-streaming run() —
// silently dropping tokens by routing through the wrong entry point
// would be a worse failure mode. The thrown error tells the caller
// they should pick run_stream(). (C++ doesn't have the Python-side
// hint-string, but the throw keeps the bug from being silent.)
// =========================================================================
TEST(ExecuteStreamOnlyDispatch, RunStillRaisesForNonStreaming) {
    std::atomic<int> calls{0};
    // Re-register under a fresh type name so this test doesn't pick
    // up the previous test's factory closure (which writes into
    // a now-destroyed counter).
    NodeFactory::instance().register_type("stream_only_cpp_run",
        [&calls](const std::string& name, const json&,
                 const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<StreamOnlyCppNode>(name, &calls);
        });

    auto engine = GraphEngine::compile(
        make_graph("stream_only_cpp_run"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "stream-only-cpp-run-001";

    EXPECT_THROW({ engine->run(cfg); }, std::runtime_error);
    EXPECT_EQ(calls.load(), 0)
        << "execute_stream must NOT fire for non-streaming run()";
}
