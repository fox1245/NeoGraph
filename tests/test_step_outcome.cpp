// #392: one rule decides how a super-step ends when its siblings end
// differently, whatever their index or completion order.
//
// In a fan-out step, or among the Sends of a step, whether the run paused or
// failed used to depend on which exception had the lower index: an interrupt
// before a failure dropped the failure and paused the run (the failing sibling
// simply ran again on resume), and a failure before an interrupt discarded the
// interrupt and failed the run.
//
// The rule, for fan-out branches and for Sends, one worker or many, in this
// order of precedence:
//
//  1. Run control (cancellation, out of memory) stops the run.
//  2. An uncertain effect (a reconciliation interrupt) pauses the step for
//     reconciliation, even when a sibling failed: ending the run would abandon
//     an effect whose outcome nobody knows. With a failure in the step, the
//     pause reports only the reconciliation interrupts; the failed sibling
//     recorded no result, so it runs again on resume.
//  3. A failure fails the run, with the first failure in dispatch order. The
//     ordinary interrupts of the step are not reported and no NodeInterrupt
//     checkpoint is saved.
//  4. Otherwise the step pauses on its interrupts (#388).
//
// A failure routed along a failure edge (#364) is the node's result, like a
// completion: it does not fail the step, and a sibling's interrupt still
// pauses it; the route is taken when the step completes.

#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_effect_broker.h>

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

void count_run(const std::string& key) {
    std::lock_guard lock(runs_mutex());
    ++runs_map()[key];
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

std::string text_of(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

// What one invocation does: "ask" raises an ordinary NodeInterrupt until it
// is answered, "recon" the reconciliation interrupt the executor raises for an
// uncertain effect, "fail" and "handled" throw (a "handled" node has a failure
// edge), "cancel" throws CancelledException and "plain" finishes.
NodeOutput behave(const std::string& does, const std::string& who, const NodeInput& in) {
    const auto& answer = in.ctx.resume_value;
    if (does == "ask" && !answer) throw NodeInterrupt("approve " + who, json{{"asks", who}});
    if (does == "recon" && !answer)
        throw NodeInterrupt("Mediated Tool effect requires reconciliation",
                            json{{"kind", "tool_effect_reconciliation"},
                                 {"thread_id", in.ctx.thread_id},
                                 {"task_id", who}});
    if (does == "fail" || does == "handled") throw std::runtime_error("boom " + who);
    if (does == "cancel") throw CancelledException("cancelled " + who);
    NodeOutput out;
    out.writes.push_back(ChannelWrite{
        "results", json::array({answer ? who + "=" + text_of(*answer) : who})});
    return out;
}

class ActNode final : public GraphNode {
public:
    ActNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(config_.value("probe", std::string()) + "/" + name_);
        co_return behave(config_.value("does", std::string("plain")), name_, in);
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

// A Send target that does what its item says ("1:ask" asks, and so on).
class WorkerNode final : public GraphNode {
public:
    WorkerNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto item = in.state.get("item").get<std::string>();
        count_run(config_.value("probe", std::string()) + "/" + item);
        co_return behave(item.substr(item.find(':') + 1), item, in);
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

// A real uncertain effect: the node dispatches "ledger" through the run's
// ToolEffectBroker, which cannot tell whether the write happened until the
// test marks it reconciled.
class UncertainBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(ToolEffectIdentity, Tool&, json,
                                                 ToolExecutionContext) override {
        ToolExecutionResult result;
        if (reconciled.load()) {
            result.status = ToolTerminalStatus::Succeeded;
            result.output = "{}";
            co_return result;
        }
        result.status           = ToolTerminalStatus::ReconciliationRequired;
        result.effect_uncertain = true;
        result.error            = "the write may have happened";
        co_return result;
    }
    std::atomic<bool> reconciled{false};
};

class LedgerTool final : public Tool {
public:
    ChatTool get_definition() const override {
        ChatTool tool;
        tool.name        = "ledger";
        tool.description = "writes a ledger entry";
        return tool;
    }
    std::string execute(const json&) override { return "{}"; }
    std::string get_name() const override { return "ledger"; }
};

class UncertainNode final : public GraphNode {
public:
    UncertainNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        count_run(config_.value("probe", std::string()) + "/" + name_);
        LedgerTool tool;
        ToolCall   call;
        call.id        = "call-1";
        call.name      = "ledger";
        call.arguments = "{}";
        std::vector<ToolCall> calls{call};
        std::vector<Tool*>    tools{&tool};
        (void)co_await dispatch_tool_calls(std::move(calls), std::move(tools), in.ctx.tool_gate,
                                           ToolGateContext{}, make_tool_execution_context(in.ctx));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
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
        register_type<ActNode>("step_outcome.act");
        register_type<UncertainNode>("step_outcome.uncertain");
        register_type<MapNode>("step_outcome.map");
        register_type<WorkerNode>("step_outcome.worker");
        return true;
    }();
    (void)registered;
}

json edge(const std::string& from, const std::string& to) { return json{{"from", from}, {"to", to}}; }

json base(const char* name) {
    return json{{"schema_version", 1},
                {"name", name},
                {"channels",
                 json{{"items", json{{"reducer", "overwrite"}, {"initial", json::array()}}},
                      {"item", json{{"reducer", "overwrite"}, {"initial", ""}}},
                      {"results", json{{"reducer", "append"}, {"initial", json::array()}}},
                      {"failures", json{{"reducer", "append"}, {"initial", json::array()}}}}},
                {"nodes", json::object()},
                {"edges", json::array()}};
}

bool any_handled(const std::vector<std::string>& sequence) {
    for (const auto& does : sequence)
        if (does == "handled") return true;
    return false;
}

// anchor, then one branch per entry of `sequence` ("n1", "n2", ...), in
// dispatch order. A "handled" branch routes its failure to "handler".
json fan_out(const std::string& probe, const std::vector<std::string>& sequence) {
    auto definition = base("step_outcome_fan_out");
    auto nodes      = json::object();
    auto edges      = json::array();
    auto failures   = json::array();
    nodes["anchor"] = json{{"type", "step_outcome.act"}, {"probe", probe}};
    edges.push_back(edge("__start__", "anchor"));
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        const auto name = "n" + std::to_string(index + 1);
        const char* type = sequence[index] == "uncertain" ? "step_outcome.uncertain" : "step_outcome.act";
        nodes[name] = json{{"type", type}, {"probe", probe}, {"does", sequence[index]}};
        edges.push_back(edge("anchor", name));
        edges.push_back(edge(name, "__end__"));
        if (sequence[index] == "handled")
            failures.push_back(json{{"from", name}, {"to", "handler"}, {"error_channel", "failures"}});
    }
    if (any_handled(sequence)) {
        nodes["handler"] = json{{"type", "step_outcome.act"}, {"probe", probe}};
        edges.push_back(edge("handler", "__end__"));
        definition["failure_edges"] = failures;
    }
    definition["nodes"] = nodes;
    definition["edges"] = edges;
    return definition;
}

// anchor, then a map node whose Sends do what `sequence` says, in Send order.
json sends(const std::string& probe, const std::vector<std::string>& sequence) {
    auto definition = base("step_outcome_sends");
    auto nodes      = json::object();
    nodes["anchor"] = json{{"type", "step_outcome.act"}, {"probe", probe}};
    nodes["map"]    = json{{"type", "step_outcome.map"}, {"probe", probe}};
    nodes["worker"] = json{{"type", "step_outcome.worker"}, {"probe", probe}};
    auto edges      = json::array({edge("__start__", "anchor"), edge("anchor", "map"),
                                   edge("map", "__end__"), edge("worker", "__end__")});
    if (any_handled(sequence)) {
        nodes["handler"] = json{{"type", "step_outcome.act"}, {"probe", probe}};
        edges.push_back(edge("handler", "__end__"));
        definition["failure_edges"] = json::array(
            {json{{"from", "worker"}, {"to", "handler"}, {"error_channel", "failures"}}});
    }
    definition["nodes"] = nodes;
    definition["edges"] = edges;
    return definition;
}

json items_of(const std::vector<std::string>& sequence) {
    auto items = json::array();
    for (std::size_t index = 0; index < sequence.size(); ++index)
        items.push_back(std::to_string(index + 1) + ":" + sequence[index]);
    return items;
}

// The name a sibling runs and reports under: "n2" for a branch, "2:fail" for
// a Send.
struct Path {
    const char* label;
    bool        sends;
    int         workers;
};

std::string who(const Path& path, const std::vector<std::string>& sequence, std::size_t index) {
    return path.sends ? std::to_string(index + 1) + ":" + sequence[index]
                      : "n" + std::to_string(index + 1);
}

std::string reported_node(const Path& path, const std::vector<std::string>& sequence,
                          std::size_t index) {
    return path.sends ? std::string("worker") : who(path, sequence, index);
}

std::size_t index_of(const std::vector<std::string>& sequence, const std::string& does) {
    for (std::size_t index = 0; index < sequence.size(); ++index)
        if (sequence[index] == does) return index;
    return sequence.size();
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> next{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ng-step-outcome-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++next));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

const Path kPaths[] = {{"fan-out, one worker", false, 1},
                       {"fan-out, four workers", false, 4},
                       {"sequential Sends", true, 1},
                       {"parallel Sends", true, 4}};

// One case of the matrix: a fresh engine and thread for `sequence` on `path`.
class Case {
public:
    Case(const Path& path, std::vector<std::string> sequence, bool sqlite, const TempDir& dir)
        : path_(path), sequence_(std::move(sequence)), probe_(unique_probe("outcome")) {
        register_types();
        store_ = sqlite ? std::shared_ptr<CheckpointStore>(
                              std::make_shared<SqliteCheckpointStore>(dir.file(probe_ + ".sqlite")))
                        : std::make_shared<InMemoryCheckpointStore>();
        EngineConfig config;
        config.checkpoint_store = store_;
        config.stderr_notices   = false;
        config.worker_count     = path.workers;
        engine_ = GraphEngine::build(path.sends ? sends(probe_, sequence_) : fan_out(probe_, sequence_),
                                     std::move(config));
        config_.thread_id = probe_;
        if (path.sends) config_.input = json{{"items", items_of(sequence_)}};
    }

    // The run's result, or the exception it ended with.
    RunResult       result;
    std::exception_ptr error;

    void run() { capture([this] { return engine_->run(config_); }); }
    void resume(const json& value) {
        capture([this, &value] { return engine_->resume(config_.thread_id, value); });
    }

    std::string who(std::size_t index) const { return ::who(path_, sequence_, index); }
    std::string reported(std::size_t index) const { return reported_node(path_, sequence_, index); }
    int         runs_of(std::size_t index) const { return ::runs(probe_, who(index)); }
    int         handler_runs() const { return ::runs(probe_, "handler"); }
    std::optional<Checkpoint> latest() const { return store_->load_latest(probe_); }
    CheckpointStore&          store() const { return *store_; }

private:
    template <class F>
    void capture(F operation) {
        result = RunResult{};
        error  = nullptr;
        try {
            result = operation();
        } catch (...) {
            error = std::current_exception();
        }
    }

    Path                             path_;
    std::vector<std::string>         sequence_;
    std::string                      probe_;
    std::shared_ptr<CheckpointStore> store_;
    std::unique_ptr<GraphEngine>     engine_;
    RunConfig                        config_;
};

std::string failure_message(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        return failure.what();
    }
}

bool is_cancellation(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const CancelledException&) {
        return true;
    } catch (...) {
        return false;
    }
}

std::string label(const Path& path, const std::vector<std::string>& sequence) {
    std::string text = path.label;
    text += ": [";
    for (std::size_t index = 0; index < sequence.size(); ++index)
        text += (index ? ", " : "") + sequence[index];
    return text + "]";
}

json recorded_interrupts(CheckpointStore& store, const std::string& checkpoint_id) {
    const auto saved = store.load_by_id(checkpoint_id);
    if (!saved || !saved->metadata.is_object() || !saved->metadata.contains("_neograph"))
        return json();
    const auto reserved = saved->metadata["_neograph"];
    return reserved.is_object() && reserved.contains("interrupts") ? reserved["interrupts"] : json();
}

class StepOutcome : public ::testing::TestWithParam<bool> {
protected:
    TempDir dir_;
};

}  // namespace

TEST_P(StepOutcome, AFailureFailsTheStepWhateverTheInterruptsIndex) {
    for (const auto& path : kPaths) {
        for (const auto& sequence : std::vector<std::vector<std::string>>{{"ask", "fail"},
                                                                          {"fail", "ask"}}) {
            SCOPED_TRACE(label(path, sequence));
            Case run(path, sequence, GetParam(), dir_);
            run.run();
            ASSERT_TRUE(run.error) << "the run paused on the interrupt and dropped the failure";
            const auto failed = index_of(sequence, "fail");
            const auto record = node_failure_of(run.error);
            ASSERT_TRUE(record);
            EXPECT_EQ(record->node, run.reported(failed));
            EXPECT_NE(failure_message(run.error).find("boom " + run.who(failed)), std::string::npos)
                << failure_message(run.error);
            const auto latest = run.latest();
            ASSERT_TRUE(latest.has_value());
            EXPECT_NE(latest->interrupt_phase, CheckpointPhase::NodeInterrupt)
                << "the interrupt of a failed step saved a checkpoint to resume";
            EXPECT_EQ(run.runs_of(index_of(sequence, "ask")), 1);
            EXPECT_EQ(run.runs_of(failed), 1);
        }
    }
}

TEST_P(StepOutcome, TheFirstFailureInDispatchOrderIsReported) {
    for (const auto& path : kPaths) {
        const std::vector<std::string> sequence{"ask", "fail", "fail"};
        SCOPED_TRACE(label(path, sequence));
        Case run(path, sequence, GetParam(), dir_);
        run.run();
        ASSERT_TRUE(run.error);
        EXPECT_NE(failure_message(run.error).find("boom " + run.who(1)), std::string::npos)
            << failure_message(run.error);
    }
}

TEST_P(StepOutcome, AnUncertainEffectPausesTheStepEvenWhenASiblingFailed) {
    for (const auto& path : kPaths) {
        for (const auto& sequence : std::vector<std::vector<std::string>>{
                 {"recon", "fail"}, {"fail", "recon"}, {"recon", "ask", "fail"},
                 {"fail", "ask", "recon"}}) {
            SCOPED_TRACE(label(path, sequence));
            Case run(path, sequence, GetParam(), dir_);
            run.run();
            ASSERT_FALSE(run.error) << "the failure abandoned the uncertain effect: "
                                    << failure_message(run.error);
            ASSERT_EQ(run.result.status(), RunStatus::Interrupted);
            const auto recon = index_of(sequence, "recon");
            ASSERT_EQ(run.result.interrupts.size(), 1U)
                << "only the reconciliation is reported; an ordinary interrupt is not";
            EXPECT_EQ(run.result.interrupt_node, run.reported(recon));
            EXPECT_EQ(run.result.interrupt_value["value"]["kind"], json("tool_effect_reconciliation"));
            EXPECT_EQ(run.result.interrupt_id, run.result.interrupts.front().id);
            const auto listed = recorded_interrupts(run.store(), run.result.checkpoint_id);
            ASSERT_TRUE(listed.is_array()) << "a pause beside a failure lists what it answers";
            ASSERT_EQ(listed.size(), 1U);
            EXPECT_EQ(listed[0]["id"], json(run.result.interrupt_id));

            // Reconciled, the step runs again: the failed sibling recorded no
            // result, runs again and fails the run.
            run.resume(json{{"reconciled", true}});
            ASSERT_TRUE(run.error);
            const auto record = node_failure_of(run.error);
            ASSERT_TRUE(record);
            EXPECT_EQ(record->node, run.reported(index_of(sequence, "fail")));
            EXPECT_EQ(run.runs_of(recon), 2);
            EXPECT_EQ(run.runs_of(index_of(sequence, "fail")), 2);
        }
    }
}

TEST_P(StepOutcome, WithoutAFailureEveryInterruptIsReported) {
    for (const auto& path : kPaths) {
        for (const auto& sequence :
             std::vector<std::vector<std::string>>{{"recon", "ask"}, {"ask", "recon"}}) {
            SCOPED_TRACE(label(path, sequence));
            Case run(path, sequence, GetParam(), dir_);
            run.run();
            ASSERT_FALSE(run.error) << failure_message(run.error);
            ASSERT_EQ(run.result.status(), RunStatus::Interrupted);
            ASSERT_EQ(run.result.interrupts.size(), 2U);
            EXPECT_EQ(run.result.interrupt_node, run.reported(0)) << "the first in dispatch order";
        }
    }
}

TEST_P(StepOutcome, ARoutedFailureIsTheNodesResultAndTheInterruptPauses) {
    for (const auto& path : kPaths) {
        for (const auto& sequence :
             std::vector<std::vector<std::string>>{{"ask", "handled"}, {"handled", "ask"}}) {
            SCOPED_TRACE(label(path, sequence));
            Case run(path, sequence, GetParam(), dir_);
            run.run();
            ASSERT_FALSE(run.error) << failure_message(run.error);
            ASSERT_EQ(run.result.status(), RunStatus::Interrupted);
            ASSERT_EQ(run.result.interrupts.size(), 1U);
            EXPECT_EQ(run.result.interrupt_node, run.reported(index_of(sequence, "ask")));
            EXPECT_EQ(run.handler_runs(), 0);

            run.resume(json("ok"));
            ASSERT_FALSE(run.error) << failure_message(run.error);
            ASSERT_EQ(run.result.status(), RunStatus::Completed);
            EXPECT_EQ(run.handler_runs(), 1) << "the routed failure takes its edge";
            EXPECT_EQ(run.runs_of(index_of(sequence, "handled")), 1)
                << "the routed failure replays its record";
        }
    }
}

TEST_P(StepOutcome, CancellationStopsTheRunWhateverElseTheStepDid) {
    for (const auto& path : kPaths) {
        for (const auto& sequence : std::vector<std::vector<std::string>>{
                 {"cancel", "recon"}, {"recon", "cancel"}, {"fail", "cancel"},
                 {"ask", "cancel"}}) {
            SCOPED_TRACE(label(path, sequence));
            Case run(path, sequence, GetParam(), dir_);
            run.run();
            ASSERT_TRUE(run.error);
            EXPECT_TRUE(is_cancellation(run.error)) << failure_message(run.error);
        }
    }
}

TEST_P(StepOutcome, AMediatedEffectLeftUncertainPausesBesideAFailure) {
    // The reconciliation interrupt the executor raises for an effect a
    // ToolEffectBroker could not settle, rather than one a node raises.
    for (const int workers : {1, 4}) {
        for (const auto& sequence : std::vector<std::vector<std::string>>{{"uncertain", "fail"},
                                                                          {"fail", "uncertain"}}) {
            SCOPED_TRACE(label(Path{"fan-out", false, workers}, sequence));
            const auto probe = unique_probe("uncertain");
            register_types();
            EngineConfig config;
            config.checkpoint_store =
                GetParam() ? std::shared_ptr<CheckpointStore>(std::make_shared<SqliteCheckpointStore>(
                                 dir_.file(probe + ".sqlite")))
                           : std::make_shared<InMemoryCheckpointStore>();
            config.stderr_notices = false;
            config.worker_count   = workers;
            auto engine           = GraphEngine::build(fan_out(probe, sequence), std::move(config));
            const auto broker     = std::make_shared<UncertainBroker>();
            RunResources resources;
            resources.tool_effect_broker = broker;
            RunConfig run_config;
            run_config.thread_id = probe;

            const auto paused = neograph::async::run_sync(
                engine->run_async(run_config, RunMetadata{}, resources));
            ASSERT_EQ(paused.status(), RunStatus::Interrupted);
            const auto uncertain = "n" + std::to_string(index_of(sequence, "uncertain") + 1);
            EXPECT_EQ(paused.interrupt_node, uncertain);
            EXPECT_EQ(paused.interrupt_value["value"]["kind"], json("tool_effect_reconciliation"));
            ASSERT_EQ(paused.interrupts.size(), 1U);

            broker->reconciled = true;
            std::exception_ptr error;
            try {
                (void)neograph::async::run_sync(engine->resume_async(
                    run_config, json{{"reconciled", true}}, nullptr, RunMetadata{}, resources));
            } catch (...) {
                error = std::current_exception();
            }
            ASSERT_TRUE(error) << "the failed sibling runs again and fails the run";
            const auto record = node_failure_of(error);
            ASSERT_TRUE(record);
            EXPECT_EQ(record->node, "n" + std::to_string(index_of(sequence, "fail") + 1));
            EXPECT_EQ(runs(probe, uncertain), 2);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Stores, StepOutcome, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Sqlite" : "InMemory"; });
