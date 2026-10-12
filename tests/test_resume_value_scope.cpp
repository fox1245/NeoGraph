// #393: a resume value reaches only the invocation the resume answers.
//
// After a resume, ctx.resume_value used to stay visible to every node of every
// later super-step of the resumed run. A later node that asks for approval, or
// a ToolGate that asks before a tool runs, saw the earlier answer and did not
// ask: one approval silently answered a different question.
//
// The contract: a resume value is delivered to exactly one invocation, in the
// step the resume re-enters. Resuming a NodeInterrupt checkpoint delivers it
// to the interrupt the resume answers (#388); resuming the checkpoint an
// interrupt_before gate saved delivers it to the gated node. Every other
// invocation of that step, every invocation of a later step, and every
// invocation of a resume from any other checkpoint (interrupt_after, a step's
// checkpoint after a crash, a safe point) runs with ctx.resume_value empty, so
// a node that asks pauses the run again. A subgraph resumed through its parent
// receives its answer through the parent's answered invocation.

#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/tool_dispatch.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::mutex&                 runs_mutex() { static std::mutex m; return m; }
std::map<std::string, int>& runs_map() { static std::map<std::string, int> m; return m; }

int count_run(const std::string& key) {
    std::lock_guard lock(runs_mutex());
    return ++runs_map()[key];
}

int runs(const std::string& probe, const std::string& name) {
    std::lock_guard lock(runs_mutex());
    const auto      found = runs_map().find(probe + "/" + name);
    return found == runs_map().end() ? 0 : found->second;
}

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

std::string probe_of(const json& config) { return config.value("probe", std::string()); }

std::string text_of(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// Asks its own question until it is answered, and records the answer it got.
class AskNode final : public GraphNode {
public:
    AskNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        const auto& answer = in.ctx.resume_value;
        if (!answer) throw NodeInterrupt("approve " + name_, json{{"asks", name_}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_ + "=" + text_of(*answer)})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

class PlainNode final : public GraphNode {
public:
    PlainNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// One Send to "worker" per entry of "items".
class MapNode final : public GraphNode {
public:
    MapNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(probe_of(config_) + "/" + name_);
        NodeOutput out;
        const auto items = in.state.get("items");
        for (std::size_t index = 0; index < items.size(); ++index)
            out.sends.push_back(Send{"worker", json{{"item", items[index]}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Send target that asks about its item until answered.
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item = in.state.get("item").get<std::string>();
        count_run(probe_of(config_) + "/" + name_ + ":" + item);
        const auto& answer = in.ctx.resume_value;
        if (!answer) throw NodeInterrupt("approve " + item, json{{"asks", item}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({item + "=" + text_of(*answer)})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Asks; once answered, its first attempt fails with a retryable error, so the
// answer must reach the retry too.
class FlakyAskNode final : public GraphNode {
public:
    FlakyAskNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto  probe  = probe_of(config_);
        const auto& answer = in.ctx.resume_value;
        if (!answer) {
            count_run(probe + "/" + name_);
            throw NodeInterrupt("approve " + name_, json{{"asks", name_}});
        }
        if (count_run(probe + "/" + name_ + ":answered") == 1)
            throw std::runtime_error("transient failure after the answer");
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_ + "=" + text_of(*answer)})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// Stands in for a model that asked for the "shell" tool.
class ToolCallingNode final : public GraphNode {
public:
    ToolCallingNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        count_run(probe_of(config_) + "/" + name_);
        json assistant;
        assistant["role"]       = "assistant";
        assistant["content"]    = "";
        assistant["tool_calls"] = json::array(
            {json{{"id", "call-1"}, {"name", "shell"}, {"arguments", "{}"}}});
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", json::array({assistant})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

class CountingTool final : public Tool {
public:
    explicit CountingTool(std::atomic<int>* runs) : runs_(runs) {}
    ChatTool get_definition() const override {
        ChatTool tool;
        tool.name        = "shell";
        tool.description = "counts its runs";
        return tool;
    }
    std::string execute(const json&) override {
        runs_->fetch_add(1);
        return R"({"ok": true})";
    }
    std::string get_name() const override { return "shell"; }

private:
    std::atomic<int>* runs_;
};

template <class NodeType>
void register_type(const char* type) {
    NodeFactory::instance().register_type(
        type,
        [](const std::string& name, const json& config, const NodeContext&) {
            return std::make_unique<NodeType>(name, config);
        },
        json{{"type", "object"}});
}

void register_types() {
    static const bool registered = [] {
        register_type<AskNode>("resume_scope.ask");
        register_type<PlainNode>("resume_scope.plain");
        register_type<MapNode>("resume_scope.map");
        register_type<WorkerNode>("resume_scope.worker");
        register_type<FlakyAskNode>("resume_scope.flaky_ask");
        register_type<ToolCallingNode>("resume_scope.tool_llm");
        return true;
    }();
    (void)registered;
}

json channels() {
    return json{{"items", json{{"reducer", "overwrite"}, {"initial", json::array()}}},
                {"item", json{{"reducer", "overwrite"}, {"initial", ""}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}}};
}

json typed(const std::string& probe, const char* type) {
    return json{{"type", type}, {"probe", probe}};
}

json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json define(const char* name, json nodes, json edges) {
    return json{{"name", name}, {"channels", channels()}, {"nodes", std::move(nodes)},
                {"edges", std::move(edges)}};
}

// first asks, then second asks in the next step.
json two_steps(const std::string& probe) {
    return define("resume_scope_two_steps",
                 json{{"first", typed(probe, "resume_scope.ask")},
                      {"second", typed(probe, "resume_scope.ask")}},
                 json::array({edge("__start__", "first"), edge("first", "second"),
                              edge("second", "__end__")}));
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store,
                                   int workers = 2, NodeContext context = {}) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    config.worker_count     = workers;
    config.node_context     = std::move(context);
    return GraphEngine::build(definition, std::move(config));
}

std::multiset<std::string> results_of(const RunResult& result) {
    std::multiset<std::string> out;
    const auto                 values = result.output["channels"]["results"]["value"];
    for (std::size_t index = 0; index < values.size(); ++index)
        out.insert(values[index].get<std::string>());
    return out;
}

std::string asks(const RunResult& result) {
    const auto value = result.interrupt_value;
    if (!value.is_object() || !value.contains("value")) return {};
    const auto payload = value["value"];
    return payload.is_object() && payload.contains("asks") ? payload["asks"].get<std::string>()
                                                           : std::string{};
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> next{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ng-resume-scope-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++next));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string file(const char* name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

class ResumeValueScope : public ::testing::TestWithParam<bool> {
protected:
    std::shared_ptr<CheckpointStore> store() {
        if (!store_)
            store_ = GetParam()
                         ? std::shared_ptr<CheckpointStore>(std::make_shared<SqliteCheckpointStore>(
                               dir_.file("checkpoints.sqlite")))
                         : std::make_shared<InMemoryCheckpointStore>();
        return store_;
    }

private:
    TempDir                          dir_;
    std::shared_ptr<CheckpointStore> store_;
};

RunConfig thread(const std::string& id) {
    RunConfig config;
    config.thread_id = id;
    return config;
}

}  // namespace

TEST_P(ResumeValueScope, AnAnswerDoesNotReachANodeOfALaterStep) {
    const auto probe  = unique_probe("two-steps");
    auto       engine = build(two_steps(probe), store());
    const auto paused = engine->run(thread("two-steps"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_id, "s0:first");

    // The answer is first's. second asks its own question in the next step.
    const auto again = engine->resume("two-steps", json("yes"));
    ASSERT_EQ(again.status(), RunStatus::Interrupted)
        << "second must ask; it took first's answer instead";
    EXPECT_EQ(again.interrupt_node, "second");
    EXPECT_EQ(again.interrupt_id, "s1:second");
    EXPECT_EQ(asks(again), "second");

    const auto done = engine->resume("two-steps", json("ok"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"first=yes", "second=ok"}));
    EXPECT_EQ(runs(probe, "first"), 2);
    EXPECT_EQ(runs(probe, "second"), 2);
}

TEST_P(ResumeValueScope, ASendAnswerDoesNotReachTheNextStep) {
    for (const int workers : {1, 4}) {
        SCOPED_TRACE("workers=" + std::to_string(workers));
        const auto probe  = unique_probe("send");
        auto       engine = build(define("resume_scope_send",
                                        json{{"map", typed(probe, "resume_scope.map")},
                                             {"worker", typed(probe, "resume_scope.worker")},
                                             {"after", typed(probe, "resume_scope.ask")}},
                                        json::array({edge("__start__", "map"), edge("map", "after"),
                                                     edge("worker", "__end__"),
                                                     edge("after", "__end__")})),
                                  store(), workers);
        auto config  = thread("send-" + std::to_string(workers));
        config.input = json{{"items", json::array({"x"})}};
        const auto paused = engine->run(config);
        ASSERT_EQ(paused.status(), RunStatus::Interrupted);
        EXPECT_EQ(asks(paused), "x");

        const auto again = engine->resume(config.thread_id, json("w"));
        ASSERT_EQ(again.status(), RunStatus::Interrupted) << "after must ask; it took x's answer";
        EXPECT_EQ(again.interrupt_node, "after");
        EXPECT_EQ(asks(again), "after");

        const auto done = engine->resume(config.thread_id, json("z"));
        ASSERT_EQ(done.status(), RunStatus::Completed);
        EXPECT_EQ(results_of(done), (std::multiset<std::string>{"x=w", "after=z"}));
        EXPECT_EQ(runs(probe, "map"), 1);
    }
}

TEST_P(ResumeValueScope, FanOutAnswersDoNotReachTheJoinStep) {
    const auto probe  = unique_probe("fan-out");
    auto       engine = build(define("resume_scope_fan_out",
                                    json{{"anchor", typed(probe, "resume_scope.plain")},
                                         {"left", typed(probe, "resume_scope.ask")},
                                         {"right", typed(probe, "resume_scope.ask")},
                                         {"final", typed(probe, "resume_scope.ask")}},
                                    json::array({edge("__start__", "anchor"), edge("anchor", "left"),
                                                 edge("anchor", "right"), edge("left", "final"),
                                                 edge("right", "final"),
                                                 edge("final", "__end__")})),
                              store());
    const auto paused = engine->run(thread("fan-out"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    ASSERT_EQ(paused.interrupts.size(), 2U);

    const auto right = engine->resume("fan-out", json("L"));
    ASSERT_EQ(right.status(), RunStatus::Interrupted);
    EXPECT_EQ(right.interrupt_node, "right");

    const auto final_step = engine->resume("fan-out", json("R"));
    ASSERT_EQ(final_step.status(), RunStatus::Interrupted) << "final must ask; it took right's answer";
    EXPECT_EQ(final_step.interrupt_node, "final");
    EXPECT_EQ(final_step.interrupt_id, "s2:final");

    const auto done = engine->resume("fan-out", json("F"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done),
              (std::multiset<std::string>{"anchor", "left=L", "right=R", "final=F"}));
}

TEST_P(ResumeValueScope, AGateAnswerReachesOnlyTheGatedNode) {
    const auto probe = unique_probe("gate");
    auto       definition =
        define("resume_scope_gate",
              json{{"gated", typed(probe, "resume_scope.ask")},
                   {"sibling", typed(probe, "resume_scope.ask")},
                   {"later", typed(probe, "resume_scope.ask")}},
              json::array({edge("__start__", "gated"), edge("__start__", "sibling"),
                           edge("gated", "later"), edge("sibling", "__end__"),
                           edge("later", "__end__")}));
    definition["interrupt_before"] = json::array({"gated"});
    auto       engine              = build(definition, store());
    const auto paused              = engine->run(thread("gate"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "gated");
    EXPECT_EQ(runs(probe, "gated"), 0);

    // The gate's answer goes to the node it held, not to its sibling.
    const auto sibling = engine->resume("gate", json("G"));
    ASSERT_EQ(sibling.status(), RunStatus::Interrupted) << "sibling must ask; it took the gate's answer";
    EXPECT_EQ(sibling.interrupt_node, "sibling");
    EXPECT_EQ(sibling.interrupt_id, "s0:sibling");

    const auto later = engine->resume("gate", json("S"));
    ASSERT_EQ(later.status(), RunStatus::Interrupted) << "later must ask; it took sibling's answer";
    EXPECT_EQ(later.interrupt_node, "later");

    const auto done = engine->resume("gate", json("T"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"gated=G", "sibling=S", "later=T"}));
    EXPECT_EQ(runs(probe, "gated"), 1) << "the gated node replays after it ran with its answer";
}

TEST_P(ResumeValueScope, AnInterruptAfterAnswerReachesNoNode) {
    const auto probe      = unique_probe("after");
    auto       definition = define("resume_scope_after",
                                  json{{"draft", typed(probe, "resume_scope.plain")},
                                       {"approve", typed(probe, "resume_scope.ask")}},
                                  json::array({edge("__start__", "draft"), edge("draft", "approve"),
                                               edge("approve", "__end__")}));
    definition["interrupt_after"] = json::array({"draft"});
    auto       engine             = build(definition, store());
    const auto paused             = engine->run(thread("after"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "draft");

    // No node asked the interrupt_after question: approve asks its own.
    const auto approve = engine->resume("after", json("reviewed"));
    ASSERT_EQ(approve.status(), RunStatus::Interrupted) << "approve took the review as its approval";
    EXPECT_EQ(approve.interrupt_node, "approve");

    const auto done = engine->resume("after", json("A"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"draft", "approve=A"}));
}

TEST_P(ResumeValueScope, ARecoveryFromAStepCheckpointDeliversTheValueToNoNode) {
    // The shape of crash recovery: a resume from the checkpoint a step
    // committed, carrying the value of an earlier answer.
    const auto probe  = unique_probe("recovery");
    auto       engine = build(define("resume_scope_recovery",
                                    json{{"draft", typed(probe, "resume_scope.plain")},
                                         {"approve", typed(probe, "resume_scope.ask")}},
                                    json::array({edge("__start__", "draft"), edge("draft", "approve"),
                                                 edge("approve", "__end__")})),
                              store());
    const auto paused = engine->run(thread("recovery"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    std::string committed;
    for (const auto& checkpoint : engine->get_state_history("recovery"))
        if (checkpoint.interrupt_phase == CheckpointPhase::Completed && checkpoint.step == 0)
            committed = checkpoint.id;
    ASSERT_FALSE(committed.empty());

    const auto again = engine->resume_from(thread("recovery"), committed, json("stale"));
    ASSERT_EQ(again.status(), RunStatus::Interrupted) << "approve took a value it never asked for";
    EXPECT_EQ(again.interrupt_node, "approve");
}

TEST_P(ResumeValueScope, ASubgraphGetsItsAnswerThroughItsParentAndItsLaterStepsAsk) {
    const auto probe = unique_probe("subgraph");
    const auto inner = define("resume_scope_inner",
                             json{{"inner_first", typed(probe, "resume_scope.ask")},
                                  {"inner_second", typed(probe, "resume_scope.ask")}},
                             json::array({edge("__start__", "inner_first"),
                                          edge("inner_first", "inner_second"),
                                          edge("inner_second", "__end__")}));
    auto       engine =
        build(define("resume_scope_outer",
                    json{{"child", json{{"type", "subgraph"}, {"definition", inner}}},
                         {"outer", typed(probe, "resume_scope.ask")}},
                    json::array({edge("__start__", "child"), edge("child", "outer"),
                                 edge("outer", "__end__")})),
              store());
    const auto paused = engine->run(thread("subgraph"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "child");
    EXPECT_EQ(asks(paused), "inner_first");

    // The parent's answered invocation hands the answer to the subgraph,
    // whose next step asks its own question.
    const auto inner_second = engine->resume("subgraph", json("a"));
    ASSERT_EQ(inner_second.status(), RunStatus::Interrupted);
    EXPECT_EQ(inner_second.interrupt_node, "child");
    EXPECT_EQ(asks(inner_second), "inner_second") << "inner_second took inner_first's answer";

    const auto outer = engine->resume("subgraph", json("b"));
    ASSERT_EQ(outer.status(), RunStatus::Interrupted) << "outer took the subgraph's answer";
    EXPECT_EQ(outer.interrupt_node, "outer");

    const auto done = engine->resume("subgraph", json("c"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    const auto results = results_of(done);
    EXPECT_EQ(results.count("inner_first=a"), 1U);
    EXPECT_EQ(results.count("inner_second=b"), 1U);
    EXPECT_EQ(results.count("outer=c"), 1U);
    EXPECT_EQ(runs(probe, "inner_first"), 2);
    EXPECT_EQ(runs(probe, "inner_second"), 2);
}

TEST_P(ResumeValueScope, AToolGateInALaterStepAsksAgain) {
    const auto       probe = unique_probe("tool-gate");
    std::atomic<int> shell_runs{0};
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<CountingTool>(&shell_runs));
    NodeContext context;
    context.tools   = ToolSet(std::move(tools));
    auto definition = define("resume_scope_tool_gate",
                            json{{"ask", typed(probe, "resume_scope.ask")},
                                 {"llm", typed(probe, "resume_scope.tool_llm")},
                                 {"tools", json{{"type", "tool_dispatch"}}}},
                            json::array({edge("__start__", "ask"), edge("ask", "llm"),
                                         edge("llm", "tools"), edge("tools", "__end__")}));
    definition["channels"]["messages"] = json{{"reducer", "append"}};
    auto engine                        = build(definition, store(), 2, std::move(context));
    std::atomic<int> gate_asked{0};
    engine->set_tool_gate([&gate_asked](ToolCall, ToolGateContext gctx)
                              -> asio::awaitable<ToolDecision> {
        if (!gctx.resume_value) {
            gate_asked.fetch_add(1);
            co_return ToolDecision::interrupt("shell needs approval", json{{"asks", "shell"}});
        }
        if (gctx.resume_value->is_object() && gctx.resume_value->value("approved", false))
            co_return ToolDecision::allow();
        co_return ToolDecision::deny("not approved");
    });

    const auto paused = engine->run(thread("tool-gate"));
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "ask");

    // The approval answers ask's question, not the gate's.
    const auto gate = engine->resume("tool-gate", json{{"approved", true}});
    ASSERT_EQ(gate.status(), RunStatus::Interrupted) << "the gate took ask's answer as approval";
    EXPECT_EQ(gate.interrupt_node, "tools");
    EXPECT_EQ(asks(gate), "shell");
    EXPECT_EQ(gate_asked.load(), 1);
    EXPECT_EQ(shell_runs.load(), 0) << "the tool ran without its own approval";

    const auto done = engine->resume("tool-gate", json{{"approved", true}});
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(shell_runs.load(), 1);
}

TEST_P(ResumeValueScope, EveryAttemptOfTheAnsweredInvocationSeesTheAnswer) {
    const auto probe      = unique_probe("retry");
    auto       definition = define("resume_scope_retry",
                                  json{{"flaky", typed(probe, "resume_scope.flaky_ask")},
                                       {"next", typed(probe, "resume_scope.ask")}},
                                  json::array({edge("__start__", "flaky"), edge("flaky", "next"),
                                               edge("next", "__end__")}));
    register_types();
    EngineConfig config;
    config.checkpoint_store = store();
    config.stderr_notices   = false;
    RetryPolicy retry;
    retry.max_retries                  = 1;
    retry.initial_delay_ms             = 1;
    config.node_retry_policies["flaky"] = retry;
    auto engine                        = GraphEngine::build(definition, std::move(config));

    ASSERT_EQ(engine->run(thread("retry")).status(), RunStatus::Interrupted);
    const auto next = engine->resume("retry", json("r"));
    ASSERT_EQ(next.status(), RunStatus::Interrupted);
    EXPECT_EQ(next.interrupt_node, "next") << "the retry of flaky ran with its answer";
    EXPECT_EQ(runs(probe, "flaky:answered"), 2);

    const auto done = engine->resume("retry", json("n"));
    ASSERT_EQ(done.status(), RunStatus::Completed);
    EXPECT_EQ(results_of(done), (std::multiset<std::string>{"flaky=r", "next=n"}));
}

INSTANTIATE_TEST_SUITE_P(Stores, ResumeValueScope, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Sqlite" : "InMemory"; });
