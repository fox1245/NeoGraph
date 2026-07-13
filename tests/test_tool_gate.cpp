// The tool gate: observe and intervene between "the model asked for tool X" and
// "tool X runs" (issue #89).
//
// Before this there was no hook there at all, which made four things
// unimplementable without forking the engine: permission prompts ("the agent
// wants to run `rm -rf build/` — allow?"), audit, argument rewriting, and the
// per-call interrupt (#90). They are not four features. They are one primitive
// wearing four hats — observe and intervene at the tool-call boundary — so
// there is one gate, returning Allow / Deny / Interrupt.
//
// THE ORDERING IS THE WHOLE DESIGN. The gate runs over every call BEFORE any
// tool executes. Not because it is tidier: because a permission gate that runs
// the command and then asks is not a permission gate.
//
// Consider three tools dispatched together, and the second one needs approval.
// If the gate were consulted after execution, tools one and three would already
// have had their effects. The run then pauses, the human approves, and the
// engine resumes — which re-enters the node from the top, because a node that
// interrupted did not record its writes. Tools one and three run AGAIN. The
// approval prompt for `rm -rf` would have double-committed the `git commit`
// sitting next to it.
//
// So: decide everything first (no side effects yet), then act. That is what
// InterruptPausesBeforeAnySiblingRuns and ResumeAfterApprovalRunsEachToolOnce
// exist to hold in place.

#include <gtest/gtest.h>

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/tool_dispatch.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Records that it ran, and with which arguments — the two things every case
// below needs to assert on.
class SpyTool : public Tool {
public:
    SpyTool(std::string name, std::atomic<int>* runs, json* last_args = nullptr)
        : name_(std::move(name)), runs_(runs), last_args_(last_args) {}

    ChatTool get_definition() const override {
        ChatTool t;
        t.name        = name_;
        t.description = "spy";
        return t;
    }

    std::string execute(const json& arguments) override {
        if (runs_) runs_->fetch_add(1);
        if (last_args_) *last_args_ = arguments;
        return R"({"ok": true})";
    }

    std::string get_name() const override { return name_; }

private:
    std::string       name_;
    std::atomic<int>* runs_;
    json*             last_args_;
};

ToolCall make_call(std::string id, std::string name, std::string args = "{}") {
    ToolCall tc;
    tc.id        = std::move(id);
    tc.name      = std::move(name);
    tc.arguments = std::move(args);
    return tc;
}

// Drives dispatch_tool_calls directly — the single place both the graph node and
// the Agent route through, so a gate proven here is proven for both. (That the
// two paths really do share it is asserted separately, further down.)
std::vector<ChatMessage> dispatch(std::vector<ToolCall> calls,
                                  std::vector<Tool*> tools,
                                  ToolGate gate = {},
                                  ToolGateContext gctx = {}) {
    return neograph::async::run_sync(
        dispatch_tool_calls(std::move(calls), std::move(tools),
                            std::move(gate), std::move(gctx)));
}

} // namespace

// ── 1. No gate: nothing changes ───────────────────────────────────────────
//
// The anchor. Every existing example, cookbook and user graph sets no gate, and
// must behave exactly as it did.
TEST(ToolGate, NoGateMeansEveryToolRuns) {
    std::atomic<int> runs{0};
    SpyTool tool("shell", &runs);

    auto msgs = dispatch({make_call("1", "shell")}, {&tool});

    EXPECT_EQ(runs.load(), 1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].role, "tool");
    EXPECT_EQ(msgs[0].content, R"({"ok": true})");
}

// ── 2. Deny: the tool does not run, and the model is told why ─────────────
//
// A denial is not an error and not silence. The model has to see it, or it will
// simply ask for the same tool again on the next turn.
TEST(ToolGate, DenyStopsTheToolAndTellsTheModel) {
    std::atomic<int> runs{0};
    SpyTool tool("shell", &runs);

    ToolGate gate = [](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        co_return ToolDecision::deny("shell access is disabled in this workspace");
    };

    auto msgs = dispatch({make_call("1", "shell")}, {&tool}, gate);

    EXPECT_EQ(runs.load(), 0) << "a denied tool must not execute";
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].role, "tool");
    EXPECT_NE(msgs[0].content.find("shell access is disabled"), std::string::npos)
        << "the model cannot react to a denial it never sees";
}

// ── 3. Allow can rewrite the arguments ────────────────────────────────────
//
// Ambient values (tenant, thread, credentials) get injected here instead of
// every tool having to know about them.
TEST(ToolGate, AllowCanRewriteTheArguments) {
    std::atomic<int> runs{0};
    json seen;
    SpyTool tool("query", &runs, &seen);

    ToolGate gate = [](ToolCall call, ToolGateContext) -> asio::awaitable<ToolDecision> {
        json args = json::parse(call.arguments);
        args["tenant_id"] = "acme";
        co_return ToolDecision::allow(std::move(args));
    };

    auto msgs = dispatch({make_call("1", "query", R"({"sql": "select 1"})")},
                         {&tool}, gate);

    EXPECT_EQ(runs.load(), 1);
    EXPECT_EQ(seen.value("tenant_id", ""), "acme")
        << "the tool received the arguments the model sent, not the rewritten ones";
    EXPECT_EQ(seen.value("sql", ""), "select 1")
        << "rewriting must not drop what the model asked for";
    EXPECT_EQ(msgs.size(), 1u);
}

// ── 4. Interrupt pauses BEFORE any sibling has run ────────────────────────
//
// The load-bearing test of this whole recipe. Three calls; the middle one needs
// a human. Neither sibling may have executed when we stop — otherwise approving
// the dangerous one silently re-runs the harmless ones.
TEST(ToolGate, InterruptPausesBeforeAnySiblingRuns) {
    std::atomic<int> runs_a{0}, runs_b{0}, runs_c{0};
    SpyTool a("read",  &runs_a);
    SpyTool b("shell", &runs_b);
    SpyTool c("write", &runs_c);

    ToolGate gate = [](ToolCall call, ToolGateContext) -> asio::awaitable<ToolDecision> {
        if (call.name == "shell") {
            json payload;
            payload["tool"] = "shell";
            co_return ToolDecision::interrupt("shell needs approval", payload);
        }
        co_return ToolDecision::allow();
    };

    std::vector<ToolCall> calls{
        make_call("1", "read"), make_call("2", "shell"), make_call("3", "write")};

    EXPECT_THROW(dispatch(calls, {&a, &b, &c}, gate), NodeInterrupt);

    EXPECT_EQ(runs_a.load(), 0) << "a sibling ran before the gate stopped the batch";
    EXPECT_EQ(runs_b.load(), 0) << "the tool that needed approval ran anyway";
    EXPECT_EQ(runs_c.load(), 0) << "a sibling ran before the gate stopped the batch";
}

// The interrupt carries what the caller needs to render the prompt — riding the
// machinery #94 built, not a second interrupt mechanism.
TEST(ToolGate, TheInterruptCarriesItsReasonAndPayload) {
    std::atomic<int> runs{0};
    SpyTool tool("shell", &runs);

    ToolGate gate = [](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        json payload;
        payload["cmd"] = "rm -rf build/";
        co_return ToolDecision::interrupt("shell needs approval", payload);
    };

    try {
        dispatch({make_call("1", "shell")}, {&tool}, gate);
        FAIL() << "expected a NodeInterrupt";
    } catch (const NodeInterrupt& ni) {
        EXPECT_EQ(ni.reason(), "shell needs approval");
        EXPECT_EQ(ni.value()["cmd"], "rm -rf build/");
    }
}

// ── 5. The gate sees the human's answer on the resumed run ────────────────
//
// Without this the gate would interrupt again on resume, forever: it has no way
// to know the question has already been answered.
TEST(ToolGate, TheGateSeesTheResumeValue) {
    std::atomic<int> runs{0};
    SpyTool tool("shell", &runs);

    ToolGate gate = [](ToolCall, ToolGateContext gctx) -> asio::awaitable<ToolDecision> {
        if (!gctx.resume_value) {
            co_return ToolDecision::interrupt("needs approval");
        }
        if (gctx.resume_value->value("approved", false)) {
            co_return ToolDecision::allow();
        }
        co_return ToolDecision::deny("the human said no");
    };

    // Fresh: pauses.
    EXPECT_THROW(dispatch({make_call("1", "shell")}, {&tool}, gate), NodeInterrupt);
    EXPECT_EQ(runs.load(), 0);

    // Approved: runs.
    ToolGateContext approved;
    approved.resume_value = json{{"approved", true}};
    dispatch({make_call("1", "shell")}, {&tool}, gate, approved);
    EXPECT_EQ(runs.load(), 1);

    // Refused: does not run, and the model is told.
    ToolGateContext refused;
    refused.resume_value = json{{"approved", false}};
    auto msgs = dispatch({make_call("1", "shell")}, {&tool}, gate, refused);
    EXPECT_EQ(runs.load(), 1) << "a refused tool must not execute";
    EXPECT_NE(msgs[0].content.find("the human said no"), std::string::npos);
}

// ── 6. End to end through the graph: pause, approve, resume ───────────────
//
// And the count that matters: after approval, each tool ran exactly once. Twice
// would mean the interrupt let siblings through and the resume re-ran them.
namespace {

std::atomic<int> g_runs_a{0}, g_runs_b{0};

class ToolCallingNode : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput in) override {
        // Stand in for an LLM that asked for two tools.
        json msgs = json::array();
        json assistant;
        assistant["role"]    = "assistant";
        assistant["content"] = "";
        assistant["tool_calls"] = json::array({
            json{{"id", "1"}, {"name", "read"},  {"arguments", "{}"}},
            json{{"id", "2"}, {"name", "shell"}, {"arguments", "{}"}},
        });
        msgs.push_back(assistant);
        NodeResult out;
        out.writes.push_back(ChannelWrite{"messages", msgs});
        co_return out;
    }
    std::string get_name() const override { return "llm"; }
};

json gate_graph() {
    return json{
        {"name", "tool_gate_graph"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"llm",   {{"type", "tg_llm"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "llm"}},
            json{{"from", "llm"},       {"to", "tools"}},
            json{{"from", "tools"},     {"to", "__end__"}}
        })}
    };
}

} // namespace

TEST(ToolGate, ResumeAfterApprovalRunsEachToolExactlyOnce) {
    g_runs_a = 0;
    g_runs_b = 0;

    NodeFactory::instance().register_type("tg_llm",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ToolCallingNode>();
        });

    auto read  = std::make_unique<SpyTool>("read",  &g_runs_a);
    auto shell = std::make_unique<SpyTool>("shell", &g_runs_b);

    NodeContext ctx;
    ctx.tools = {read.get(), shell.get()};

    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(gate_graph(), ctx, store);

    ToolGate gate = [](ToolCall call, ToolGateContext gctx)
            -> asio::awaitable<ToolDecision> {
        if (call.name != "shell") co_return ToolDecision::allow();
        if (!gctx.resume_value) {
            json payload;
            payload["cmd"] = "rm -rf build/";
            co_return ToolDecision::interrupt("shell needs approval", payload);
        }
        co_return ToolDecision::allow();
    };

    RunConfig cfg;
    cfg.thread_id = "tg-e2e";
    cfg.tool_gate = gate;

    auto paused = engine->run(cfg);

    ASSERT_TRUE(paused.interrupted) << "the gate's Interrupt must pause the graph";
    EXPECT_EQ(paused.interrupt_node, "tools");
    EXPECT_EQ(paused.interrupt_value.value("reason", ""), "shell needs approval");
    EXPECT_EQ(paused.interrupt_value["value"]["cmd"], "rm -rf build/");
    EXPECT_EQ(g_runs_a.load(), 0) << "the harmless tool ran while we were asking";
    EXPECT_EQ(g_runs_b.load(), 0);

    auto done = engine->resume("tg-e2e", json{{"approved", true}});

    EXPECT_FALSE(done.interrupted);
    EXPECT_EQ(g_runs_a.load(), 1)
        << "the harmless tool ran twice — the approval double-applied its effects";
    EXPECT_EQ(g_runs_b.load(), 1);
}
