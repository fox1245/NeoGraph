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
#include <neograph/tool_effect_broker.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_set>
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

class RecordingToolEffectBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(
        ToolEffectIdentity identity, Tool& tool, json arguments,
        ToolExecutionContext context) override {
        const auto key = std::make_tuple(identity.run_id, identity.thread_id,
                                         identity.task_id, identity.call_ordinal);
        {
            std::lock_guard lock(mutex_);
            if (const auto found = receipts_.find(key); found != receipts_.end()) {
                if (found->second.tool_name != tool.get_name() ||
                    found->second.arguments != arguments ||
                    found->second.tool_call_id != identity.tool_call_id) {
                    ToolExecutionResult conflict;
                    conflict.status = ToolTerminalStatus::Rejected;
                    conflict.error = "Tool effect replay binding changed";
                    co_return conflict;
                }
                co_return found->second.result;
            }
        }
        auto controller = context.controller ? context.controller
                                             : default_tool_execution_controller();
        auto result = co_await controller->execute_result_async(
            tool, arguments, context);
        {
            std::lock_guard lock(mutex_);
            identities_.push_back(identity);
            receipts_.emplace(key, Receipt{tool.get_name(), std::move(arguments),
                                           identity.tool_call_id, result});
        }
        co_return result;
    }

    std::vector<ToolEffectIdentity> identities() const {
        std::lock_guard lock(mutex_);
        return identities_;
    }

private:
    struct Receipt {
        std::string tool_name;
        json arguments;
        std::string tool_call_id;
        ToolExecutionResult result;
    };
    mutable std::mutex mutex_;
    std::map<std::tuple<std::string, std::string, std::string, std::uint64_t>,
             Receipt> receipts_;
    std::vector<ToolEffectIdentity> identities_;
};

class RefusingToolEffectBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(
        ToolEffectIdentity, Tool&, json, ToolExecutionContext) override {
        ToolExecutionResult refused;
        refused.status = ToolTerminalStatus::Failed;
        refused.error = "dispatch marker was not persisted";
        co_return refused;
    }
};

class UncertainToolEffectBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(
        ToolEffectIdentity, Tool&, json, ToolExecutionContext) override {
        ToolExecutionResult unknown;
        unknown.status = ToolTerminalStatus::ReconciliationRequired;
        unknown.error = "effect outcome is unknown";
        unknown.effect_uncertain = true;
        co_return unknown;
    }
};

class ThrowingToolEffectBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(
        ToolEffectIdentity, Tool&, json, ToolExecutionContext) override {
        throw std::runtime_error("receipt persistence failed");
        co_return ToolExecutionResult{};
    }
};

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

TEST(ToolGate, EffectBrokerUsesBatchOrdinalAndReplaysWithoutToolExecution) {
    std::atomic<int> first_runs{0}, second_runs{0};
    SpyTool first("first", &first_runs);
    SpyTool second("second", &second_runs);
    auto broker = std::make_shared<RecordingToolEffectBroker>();
    ToolExecutionContext execution;
    execution.identity.owner_scope = "tenant-a";
    execution.identity.root_run_id = "run-1";
    execution.identity.thread_id = "thread-1";
    execution.effect_task_id = "s2:tool_dispatch";
    execution.effect_broker = broker;
    const auto calls = std::vector<ToolCall>{
        make_call("same-model-id", "first", R"({"segment":1})"),
        make_call("same-model-id", "second", R"({"segment":2})")};
    auto invoke = [&](std::vector<ToolCall> requested) {
        return neograph::async::run_sync(dispatch_tool_calls(
            std::move(requested), {&first, &second}, {}, {}, execution));
    };

    const auto initial = invoke(calls);
    ASSERT_EQ(initial.size(), 2u);
    EXPECT_EQ(first_runs.load(), 1);
    EXPECT_EQ(second_runs.load(), 1);
    const auto identities = broker->identities();
    ASSERT_EQ(identities.size(), 2u);
    EXPECT_EQ(identities[0].task_id, "s2:tool_dispatch");
    EXPECT_EQ(identities[0].call_ordinal, 0u);
    EXPECT_EQ(identities[1].call_ordinal, 1u);

    const auto replay = invoke(calls);
    ASSERT_EQ(replay.size(), 2u);
    EXPECT_EQ(first_runs.load(), 1);
    EXPECT_EQ(second_runs.load(), 1);
    EXPECT_EQ(replay[0].content, initial[0].content);
    EXPECT_EQ(replay[1].content, initial[1].content);

    auto changed = calls;
    changed[0].arguments = R"({"segment":99})";
    const auto conflict = invoke(std::move(changed));
    ASSERT_EQ(conflict.size(), 2u);
    EXPECT_EQ(conflict[0].tool_status, "rejected");
    EXPECT_EQ(first_runs.load(), 1);
}

TEST(ToolGate, EffectBrokerRefusalAndMissingIdentityExecuteNoTool) {
    std::atomic<int> runs{0};
    SpyTool tool("effect", &runs);
    ToolExecutionContext execution;
    execution.identity.thread_id = "thread-1";
    execution.effect_task_id = "s1:tool_dispatch";
    execution.effect_broker = std::make_shared<RefusingToolEffectBroker>();
    const auto refused = neograph::async::run_sync(dispatch_tool_calls(
        {make_call("call-1", "effect")}, {&tool}, {}, {}, execution));
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0].tool_status, "failed");
    EXPECT_EQ(runs.load(), 0);

    execution.effect_task_id.clear();
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(
                     {make_call("call-1", "effect")}, {&tool}, {}, {}, execution)),
                 std::invalid_argument);
    EXPECT_EQ(runs.load(), 0);
}

TEST(ToolGate, UncertainBrokerOutcomeInterruptsBeforeAnotherModelTurn) {
    std::atomic<int> runs{0};
    SpyTool tool("effect", &runs);
    ToolExecutionContext execution;
    execution.identity.thread_id = "thread-1";
    execution.effect_task_id = "s1:tool_dispatch";
    execution.effect_broker = std::make_shared<UncertainToolEffectBroker>();
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(
                     {make_call("call-1", "effect")}, {&tool}, {}, {}, execution)),
                 NodeInterrupt);
    EXPECT_EQ(runs.load(), 0);
}

TEST(ToolGate, BrokerExceptionAlsoInterruptsBeforeAnotherModelTurn) {
    std::atomic<int> runs{0};
    SpyTool tool("effect", &runs);
    ToolExecutionContext execution;
    execution.identity.thread_id = "thread-1";
    execution.effect_task_id = "s1:tool_dispatch";
    execution.effect_broker = std::make_shared<ThrowingToolEffectBroker>();
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(
                     {make_call("call-1", "effect")}, {&tool}, {}, {}, execution)),
                 NodeInterrupt);
    EXPECT_EQ(runs.load(), 0);
}

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

TEST(ToolGate, BatchSnapshotRejectsDuplicateSelectorsBeforeAnyToolRuns) {
    std::atomic<int> runs{0};
    SpyTool tool("write", &runs);
    std::size_t gate_visits = 0;
    ToolGate gate = [&gate_visits](ToolCall, ToolGateContext gctx)
            -> asio::awaitable<ToolDecision> {
        ++gate_visits;
        if (!gctx.batch_calls || gctx.batch_calls->size() != 2) {
            co_return ToolDecision::interrupt("missing batch snapshot");
        }
        std::unordered_set<std::string> selected;
        for (const auto& sibling : *gctx.batch_calls) {
            const auto segment = json::parse(sibling.arguments).at("segment_id").get<std::string>();
            if (!selected.insert(segment).second) {
                co_return ToolDecision::interrupt("duplicate segment selector");
            }
        }
        co_return ToolDecision::allow();
    };

    EXPECT_THROW(dispatch({make_call("1", "write", R"({"segment_id":"s1"})"),
                           make_call("2", "write", R"({"segment_id":"s1"})")},
                          {&tool}, gate), NodeInterrupt);
    EXPECT_EQ(runs.load(), 0);
    EXPECT_EQ(gate_visits, 2u);

    const auto distinct = dispatch({make_call("3", "write", R"({"segment_id":"s1"})"),
                                    make_call("4", "write", R"({"segment_id":"s2"})")},
                                   {&tool}, gate);
    EXPECT_EQ(distinct.size(), 2u);
    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(gate_visits, 4u);
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

    engine->set_tool_gate(gate);

    RunConfig cfg;
    cfg.thread_id = "tg-e2e";

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

// ── 7. And a refusal actually refuses, across the resume ──────────────────
//
// This is the case that caught a defect the C++ tests above had missed. The
// gate used to hang off RunConfig — and `resume()` builds its own RunConfig
// internally, so the gate silently vanished the moment a human answered the
// prompt it had raised. Every tool then ran unchecked.
//
// ResumeAfterApprovalRunsEachToolExactlyOnce did not notice: with no gate, both
// tools run once, which is exactly what "exactly once" asserts. It was green for
// the wrong reason. Only asking the human to say NO exposes it — an approval and
// a vanished gate look identical; a refusal and a vanished gate do not.
TEST(ToolGate, ARefusalStillRefusesAfterTheResume) {
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

    engine->set_tool_gate([](ToolCall call, ToolGateContext gctx)
            -> asio::awaitable<ToolDecision> {
        if (call.name != "shell") co_return ToolDecision::allow();
        if (!gctx.resume_value) co_return ToolDecision::interrupt("needs approval");
        if (gctx.resume_value->value("approved", false))
            co_return ToolDecision::allow();
        co_return ToolDecision::deny("the human said no");
    });

    RunConfig cfg;
    cfg.thread_id = "tg-refuse";
    ASSERT_TRUE(engine->run(cfg).interrupted);

    auto done = engine->resume("tg-refuse", json{{"approved", false}});

    EXPECT_FALSE(done.interrupted);
    EXPECT_EQ(g_runs_b.load(), 0)
        << "the human refused and the tool ran anyway — the gate did not survive resume()";
    EXPECT_EQ(g_runs_a.load(), 1)
        << "refusing one call must not block the others";
}
