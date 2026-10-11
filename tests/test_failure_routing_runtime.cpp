// #364 failure routing runtime (Program API v2, WP11).
//
// A node whose final failure is routable and that has a failure edge does not
// fail the run: its failed attempts write nothing, one structured record goes
// to the edge's error_channel, and the node routes to exactly the edge's
// target. Every dispatch path (a single node, a fan-out branch, a sequential
// and a parallel Send, a subgraph node, the sync and async entry points) does
// this the same way, through the one retry loop, and a crash-resume replays
// the route from the pending write without running the node again.
//
// Also here: per-field retry timing (the host may only tighten a sealed
// per-node policy), non_retryable codes, the run-level retry ceiling,
// max_visits, failure exits (RunStatus::FailureExit), the failure_route
// interrupt payload, the ERROR event's handled/routed_to, and the opt-in,
// redacted free text.

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/invocation.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// ---------------------------------------------------------------------------
// One configurable node type, counted per probe and node.
// ---------------------------------------------------------------------------

std::mutex&                 calls_mutex() { static std::mutex m; return m; }
std::map<std::string, int>& calls_map() { static std::map<std::string, int> m; return m; }

int bump(const std::string& key) {
    std::lock_guard lock(calls_mutex());
    return calls_map()[key]++;
}
int calls(const std::string& probe, const std::string& node) {
    std::lock_guard lock(calls_mutex());
    const auto found = calls_map().find(probe + "/" + node);
    return found == calls_map().end() ? 0 : found->second;
}
std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

// Config keys (all optional):
//   fail: N           the first N calls fail (-1: every call)
//   fail_on_item: v   fail when the "item" channel holds v
//   throw: "runtime" (default), "logic", or "code" (NodeFailureError with
//          "code", "message" and an optional "retryable")
//   set: {ch: v}      extra writes on success
//   goto: "X"         return Command.goto(X)
//   sends: [items]    one Send to "send_target" per item
// Every successful run appends its name to "trace".
class StepNode final : public GraphNode {
public:
    StepNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto call  = bump(config_.value("probe", std::string()) + "/" + name_);
        const auto fail  = config_.value("fail", 0);
        bool       fails = fail < 0 || call < fail;
        if (config_.contains("fail_on_item") && in.state.get("item") == config_["fail_on_item"])
            fails = true;
        if (fails) {
            const auto kind = config_.value("throw", std::string("runtime"));
            if (kind == "logic") throw std::logic_error("logic failure in " + name_);
            if (kind == "code") {
                std::optional<bool> retryable;
                if (config_.contains("retryable")) retryable = config_["retryable"].get<bool>();
                throw NodeFailureError(config_["code"].get<std::string>(),
                                       config_.value("message", std::string()), retryable);
            }
            throw std::runtime_error("boom from " + name_);
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_})});
        if (config_.contains("item_result"))
            out.writes.push_back(ChannelWrite{"results", json::array({in.state.get("item")})});
        if (config_.contains("set"))
            for (const auto& [channel, value] : config_["set"].items())
                out.writes.push_back(ChannelWrite{channel, value});
        if (config_.contains("goto")) out.command = Command{config_["goto"].get<std::string>(), {}};
        if (config_.contains("sends"))
            for (const auto& item : config_["sends"])
                out.sends.push_back(Send{config_["send_target"].get<std::string>(), json{{"item", item}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

void register_types() {
    static const bool registered = [] {
        NodeFactory::instance().register_type(
            "fr.step",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<StepNode>(name, config);
            },
            json{{"type", "object"}});
        ConditionRegistry::instance().register_condition(
            "fr.decision", [](const GraphState& state) { return state.get("decision").get<std::string>(); });
        ConditionRegistry::instance().register_condition("fr.again", [](const GraphState& state) {
            return state.get("flag").get<bool>() ? std::string("done") : std::string("again");
        });
        return true;
    }();
    (void)registered;
}

json step(const std::string& probe, json extra = json::object()) {
    json config{{"type", "fr.step"}, {"probe", probe}};
    for (const auto& [key, value] : extra.items()) config[key] = value;
    return config;
}
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json channels() {
    return json{{"trace", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"failures", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"results", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"item", json{{"reducer", "overwrite"}, {"initial", nullptr}}},
                {"decision", json{{"reducer", "overwrite"}, {"initial", "done"}}},
                {"flag", json{{"reducer", "overwrite"}, {"initial", false}}},
                {"note", json{{"reducer", "overwrite"}, {"initial", ""}}}};
}

json definition_of(json nodes, json edges, json extra = json::object()) {
    json definition{{"schema_version", 1}, {"name", "fr"}, {"channels", channels()},
                    {"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
    for (const auto& [key, value] : extra.items()) definition[key] = value;
    return definition;
}

json failure_edge(const char* from, const char* to, bool with_channel = true) {
    json value{{"from", from}, {"to", to}};
    if (with_channel) value["error_channel"] = "failures";
    return value;
}

std::unique_ptr<GraphEngine> build(const json& definition, EngineConfig config = {}) {
    register_types();
    config.stderr_notices = false;
    return GraphEngine::build(definition, std::move(config));
}

json channel(const RunResult& result, const std::string& name) {
    return result.output["channels"][name]["value"];
}

// IMPLEMENT -> TEST (exit); TEST fails along its failure edge to ASK_USER (exit).
json fix_loop(const std::string& probe, json test_config, json policy) {
    json nodes{{"IMPLEMENT", step(probe)},
               {"TEST", step(probe, std::move(test_config))},
               {"ASK_USER", step(probe)}};
    json edges = json::array({edge("__start__", "IMPLEMENT"), edge("IMPLEMENT", "TEST"),
                              edge("TEST", "__end__"), edge("ASK_USER", "__end__")});
    json extra{{"failure_edges", json::array({failure_edge("TEST", "ASK_USER")})}};
    if (!policy.is_null()) extra["node_retry_policies"] = json{{"TEST", std::move(policy)}};
    return definition_of(std::move(nodes), std::move(edges), std::move(extra));
}

json fast_retries(int max_retries) {
    return json{{"max_retries", max_retries}, {"initial_delay_ms", 10}, {"backoff_multiplier", 1.0},
                {"max_delay_ms", 10}};
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> next{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ng-failure-routing-" +
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

}  // namespace

// ---------------------------------------------------------------------------
// Handled failures on every dispatch path
// ---------------------------------------------------------------------------

TEST(FailureRoutingRuntime, AHandledFailureRoutesToItsTargetAndWritesOneRecord) {
    const auto probe  = unique_probe("single");
    auto       engine = build(fix_loop(probe, json{{"fail", -1}, {"set", json{{"note", "tested"}}}},
                                       fast_retries(2)));
    const auto result = engine->run(RunConfig{});

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(calls(probe, "TEST"), 3);
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"IMPLEMENT", "TEST", "ASK_USER"}));
    // The failed attempts wrote nothing; only the handler and IMPLEMENT did.
    EXPECT_EQ(channel(result, "trace"), json::array({"IMPLEMENT", "ASK_USER"}));
    EXPECT_EQ(channel(result, "note"), json(""));
    const json expected{{"node", "TEST"},   {"kind", "exhausted"}, {"code", "NG.TaskFailed"},
                        {"attempts", 3},    {"retryable", true},   {"step", 1},
                        {"routed_to", "ASK_USER"}, {"effects", json::array()}};
    EXPECT_EQ(channel(result, "failures"), json::array({expected}));
    // The marker channel never reaches state or output.
    EXPECT_FALSE(result.output["channels"].contains(""));
}

TEST(FailureRoutingRuntime, WithoutAnErrorChannelTheRouteStillHappens) {
    const auto probe = unique_probe("no-channel");
    auto       definition = fix_loop(probe, json{{"fail", -1}}, json());
    definition["failure_edges"] = json::array({failure_edge("TEST", "ASK_USER", false)});
    const auto result = build(definition)->run(RunConfig{});
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(result.execution_trace.back(), "ASK_USER");
    EXPECT_EQ(channel(result, "failures"), json::array());
}

TEST(FailureRoutingRuntime, AHandledFailureInAParallelStepLetsSiblingsApply) {
    const auto probe = unique_probe("parallel");
    json       nodes{{"A", step(probe, json{{"fail", -1}})},
                     {"B", step(probe, json{{"set", json{{"note", "b ran"}}}})},
                     {"C", step(probe)},
                     {"H", step(probe)}};
    json edges = json::array({edge("__start__", "A"), edge("__start__", "B"), edge("A", "__end__"),
                              edge("B", "C"), edge("C", "__end__"), edge("H", "__end__")});
    const auto definition =
        definition_of(nodes, edges, json{{"failure_edges", json::array({failure_edge("A", "H")})}});
    for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
        SCOPED_TRACE(workers);
        EngineConfig config;
        config.worker_count = workers;
        const auto result   = build(definition, std::move(config))->run(RunConfig{});
        EXPECT_EQ(result.status(), RunStatus::Completed);
        EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"A", "B", "C", "H"}));
        EXPECT_EQ(channel(result, "note"), json("b ran"));
        ASSERT_EQ(channel(result, "failures").size(), 1U);
        EXPECT_EQ(channel(result, "failures")[0]["node"], json("A"));
        EXPECT_EQ(channel(result, "failures")[0]["step"], json(0));
    }
}

TEST(FailureRoutingRuntime, AHandledFailureInASendRoutesTheSendTask) {
    // FAN sends one item (sequential Send path) or three (parallel Send path);
    // W fails on item 2 and routes to H; the other Sends apply their results.
    for (const auto& items : {json::array({2}), json::array({1, 2, 3})}) {
        SCOPED_TRACE(items.dump());
        for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
            SCOPED_TRACE(workers);
            const auto probe = unique_probe("send");
            json       nodes{{"FAN", step(probe, json{{"sends", items}, {"send_target", "W"}})},
                             {"W", step(probe, json{{"fail_on_item", 2}, {"item_result", true}})},
                             {"H", step(probe)}};
            json edges = json::array({edge("__start__", "FAN"), edge("FAN", "__end__"),
                                      edge("W", "__end__"), edge("H", "__end__")});
            const auto definition =
                definition_of(nodes, edges, json{{"failure_edges", json::array({failure_edge("W", "H")})}});
            EngineConfig config;
            config.worker_count = workers;
            const auto result   = build(definition, std::move(config))->run(RunConfig{});
            EXPECT_EQ(result.status(), RunStatus::Completed);
            EXPECT_EQ(result.execution_trace.back(), "H");
            json expected_results = json::array();
            for (const auto& item : items)
                if (item != json(2)) expected_results.push_back(item);
            EXPECT_EQ(channel(result, "results"), expected_results);
            ASSERT_EQ(channel(result, "failures").size(), 1U);
            EXPECT_EQ(channel(result, "failures")[0]["node"], json("W"));
            EXPECT_EQ(channel(result, "failures")[0]["routed_to"], json("H"));
        }
    }
}

TEST(FailureRoutingRuntime, ASubgraphFailureRoutesInTheParent) {
    register_types();
    const auto probe = unique_probe("subgraph");
    const json child_definition{
        {"name", "fr_child"},
        {"channels", channels()},
        {"nodes", json{{"INNER", step(probe, json{{"fail", -1}})}}},
        {"edges", json::array({edge("__start__", "INNER"), edge("INNER", "__end__")})}};
    std::shared_ptr<GraphEngine> child(GraphEngine::compile(child_definition, NodeContext{}).release());
    static std::atomic<int> next_type{0};
    const auto type = "fr.subgraph." + std::to_string(++next_type);
    NodeFactory::instance().register_type(
        type,
        [child](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, child);
        },
        json{{"type", "object"}});
    json nodes{{"SUB", json{{"type", type}}}, {"H", step(probe)}};
    json edges = json::array({edge("__start__", "SUB"), edge("SUB", "__end__"), edge("H", "__end__")});
    const auto result =
        build(definition_of(nodes, edges, json{{"failure_edges", json::array({failure_edge("SUB", "H")})}}))
            ->run(RunConfig{});
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"SUB", "H"}));
    ASSERT_EQ(channel(result, "failures").size(), 1U);
    EXPECT_EQ(channel(result, "failures")[0]["node"], json("SUB"));
    EXPECT_EQ(channel(result, "failures")[0]["code"], json("NG.SubgraphFailed"));
}

TEST(FailureRoutingRuntime, SyncAsyncStreamAndInvocationEntryPointsAgree) {
    const auto probe      = unique_probe("entry-points");
    const auto definition = fix_loop(probe, json{{"fail", -1}}, fast_retries(1));
    std::shared_ptr<GraphEngine> engine(build(definition).release());
    RunConfig                    config;
    config.thread_id = "entry";
    const auto sync_result   = engine->run(config);
    const auto async_result  = async::run_sync(engine->run_async(config));
    const auto stream_result = engine->run_stream(config, [](const GraphEvent&) {});
    RunInvocationRequest request;
    request.config = config;
    RunInvocation invocation(engine, std::move(request));
    const auto invoked = invocation.run();
    ASSERT_TRUE(invoked.run_result);
    for (const auto* result : {&async_result, &stream_result, &*invoked.run_result}) {
        EXPECT_EQ(result->status(), sync_result.status());
        EXPECT_EQ(result->execution_trace, sync_result.execution_trace);
        EXPECT_EQ(channel(*result, "failures"), channel(sync_result, "failures"));
    }
    EXPECT_EQ(invoked.status, InvocationStatus::Completed);
    EXPECT_EQ(channel(sync_result, "failures")[0]["attempts"], json(2));
}

TEST(FailureRoutingRuntime, AnUnroutableFailureStillFailsTheRun) {
    const auto probe = unique_probe("unroutable");
    auto       engine = build(fix_loop(probe, json{{"fail", -1}, {"throw", "logic"}}, fast_retries(2)));
    try {
        (void)engine->run(RunConfig{});
        FAIL() << "a logic error is never routed";
    } catch (const NodeExecutionError& error) {
        const auto* record = node_failure_of(error);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->code, "NG.LogicError");
        EXPECT_FALSE(record->routable);
        EXPECT_EQ(record->attempts, 1);
    }
    EXPECT_EQ(calls(probe, "ASK_USER"), 0);
}

TEST(FailureRoutingRuntime, WithoutAFailureEdgeTheRunFailsAsBefore) {
    const auto probe = unique_probe("no-edge");
    json       nodes{{"TEST", step(probe, json{{"fail", -1}, {"throw", "code"},
                                               {"code", "app.flaky"}})}};
    json edges = json::array({edge("__start__", "TEST"), edge("TEST", "__end__")});
    auto engine = build(definition_of(nodes, edges, json{{"node_retry_policies", json{{"TEST", fast_retries(1)}}}}));
    try {
        (void)engine->run(RunConfig{});
        FAIL() << "the node has no failure edge";
    } catch (const NodeExecutionError& error) {
        const auto* record = node_failure_of(error);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->code, "app.flaky");
        EXPECT_EQ(record->kind, NodeFailureKind::Exhausted);
        EXPECT_EQ(record->attempts, 2);
    }
}

TEST(FailureRoutingRuntime, ACommandGotoInTheSameStepPreemptsTheFailureRoute) {
    // Command.goto preempts every route of its super-step (the join model's
    // rule 3); a failure route is a route. The record is still written.
    const auto probe = unique_probe("goto");
    json nodes{{"A", step(probe, json{{"fail", -1}})},
               {"B", step(probe, json{{"goto", "X"}})},
               {"X", step(probe)},
               {"H", step(probe)}};
    json edges = json::array({edge("__start__", "A"), edge("__start__", "B"), edge("A", "__end__"),
                              edge("B", "__end__"), edge("X", "__end__"), edge("H", "__end__")});
    const auto result =
        build(definition_of(nodes, edges, json{{"failure_edges", json::array({failure_edge("A", "H")})}}))
            ->run(RunConfig{});
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"A", "B", "X"}));
    EXPECT_EQ(channel(result, "failures").size(), 1U);
}

// ---------------------------------------------------------------------------
// Retry decisions: non_retryable codes and the run-level ceiling
// ---------------------------------------------------------------------------

TEST(FailureRoutingRuntime, NonRetryableCodesRunOnce) {
    const auto probe  = unique_probe("non-retryable");
    auto       policy = fast_retries(3);
    policy["non_retryable"] = json::array({"app.compile_error"});
    const auto result =
        build(fix_loop(probe, json{{"fail", -1}, {"throw", "code"}, {"code", "app.compile_error"}},
                       policy))
            ->run(RunConfig{});
    EXPECT_EQ(calls(probe, "TEST"), 1);
    const auto record = channel(result, "failures")[0];
    EXPECT_EQ(record["kind"], json("non_retryable"));
    EXPECT_EQ(record["code"], json("app.compile_error"));
    EXPECT_EQ(record["retryable"], json(false));
    EXPECT_EQ(record["attempts"], json(1));

    // A code that is not listed is retried.
    const auto other = unique_probe("non-retryable-other");
    (void)build(fix_loop(other, json{{"fail", -1}, {"throw", "code"}, {"code", "app.flaky"}}, policy))
        ->run(RunConfig{});
    EXPECT_EQ(calls(other, "TEST"), 4);

    // NG codes can be listed too.
    auto generic = policy;
    generic["non_retryable"] = json::array({"NG.TaskFailed"});
    const auto plain = unique_probe("non-retryable-generic");
    (void)build(fix_loop(plain, json{{"fail", -1}}, generic))->run(RunConfig{});
    EXPECT_EQ(calls(plain, "TEST"), 1);
}

TEST(FailureRoutingRuntime, TheRunLevelCeilingStopsRetrying) {
    const auto probe   = unique_probe("ceiling");
    auto       engine  = build(fix_loop(probe, json{{"fail", -1}}, fast_retries(5)));
    RunMetadata metadata;
    metadata.node_retry_ceiling = std::make_shared<NodeRetryCeiling>(2);
    const auto result = engine->run(RunConfig{}, metadata);
    EXPECT_EQ(calls(probe, "TEST"), 3);
    EXPECT_EQ(metadata.node_retry_ceiling->used(), 2U);
    const auto record = channel(result, "failures")[0];
    EXPECT_EQ(record["kind"], json("ceiling"));
    EXPECT_EQ(record["attempts"], json(3));
    EXPECT_EQ(record["retryable"], json(true));

    // A zero ceiling allows no retry at all.
    const auto none = unique_probe("ceiling-zero");
    RunMetadata zero;
    zero.node_retry_ceiling = std::make_shared<NodeRetryCeiling>(0);
    const auto stopped = build(fix_loop(none, json{{"fail", -1}}, fast_retries(5)))->run(RunConfig{}, zero);
    EXPECT_EQ(calls(none, "TEST"), 1);
    EXPECT_EQ(channel(stopped, "failures")[0]["kind"], json("ceiling"));
}

TEST(FailureRoutingRuntime, TheCeilingIsSharedByBranchesAndSubgraphs) {
    // Two failing branches with 3 retries each share a ceiling of 4.
    const auto probe = unique_probe("ceiling-shared");
    json nodes{{"A", step(probe, json{{"fail", -1}})}, {"B", step(probe, json{{"fail", -1}})},
               {"H", step(probe)}};
    json edges = json::array({edge("__start__", "A"), edge("__start__", "B"), edge("A", "__end__"),
                              edge("B", "__end__"), edge("H", "__end__")});
    const auto definition = definition_of(
        nodes, edges,
        json{{"node_retry_policies", json{{"A", fast_retries(3)}, {"B", fast_retries(3)}}},
             {"failure_edges", json::array({failure_edge("A", "H"), failure_edge("B", "H")})}});
    RunMetadata metadata;
    metadata.node_retry_ceiling = std::make_shared<NodeRetryCeiling>(4);
    const auto result = build(definition)->run(RunConfig{}, metadata);
    EXPECT_EQ(metadata.node_retry_ceiling->used(), 4U);
    EXPECT_EQ(calls(probe, "A") + calls(probe, "B"), 6);
    EXPECT_EQ(channel(result, "failures").size(), 2U);

    // A subgraph's retries count toward its parent run's ceiling.
    const auto child_probe = unique_probe("ceiling-subgraph");
    json child_definition{
        {"schema_version", 1},
        {"name", "fr_ceiling_child"},
        {"channels", channels()},
        {"nodes", json{{"INNER", step(child_probe, json{{"fail", -1}})}}},
        {"edges", json::array({edge("__start__", "INNER"), edge("INNER", "__end__")})},
        {"node_retry_policies", json{{"INNER", fast_retries(5)}}}};
    std::shared_ptr<GraphEngine> child(build(child_definition).release());
    static std::atomic<int> next_type{0};
    const auto type = "fr.ceiling_subgraph." + std::to_string(++next_type);
    NodeFactory::instance().register_type(
        type,
        [child](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, child);
        },
        json{{"type", "object"}});
    json parent_nodes{{"SUB", json{{"type", type}}}, {"H", step(child_probe)}};
    json parent_edges =
        json::array({edge("__start__", "SUB"), edge("SUB", "__end__"), edge("H", "__end__")});
    RunMetadata shared;
    shared.node_retry_ceiling = std::make_shared<NodeRetryCeiling>(2);
    const auto routed =
        build(definition_of(parent_nodes, parent_edges,
                            json{{"failure_edges", json::array({failure_edge("SUB", "H")})}}))
            ->run(RunConfig{}, shared);
    EXPECT_EQ(calls(child_probe, "INNER"), 3);
    EXPECT_EQ(shared.node_retry_ceiling->used(), 2U);
    EXPECT_EQ(routed.execution_trace, (std::vector<std::string>{"SUB", "H"}));
}

// ---------------------------------------------------------------------------
// max_visits
// ---------------------------------------------------------------------------

namespace {

// IMPLEMENT -> TEST; TEST routes "again" back to IMPLEMENT until "flag" is set.
// TEST declares max_visits; its failure edge goes to ASK_USER, which sets the
// flag (once "set_flag" is true) and routes back to IMPLEMENT or ends.
json visit_loop(const std::string& probe, int max_visits, bool with_edge, bool handler_loops) {
    json nodes{{"IMPLEMENT", step(probe)},
               {"TEST", step(probe)},
               {"ASK_USER", step(probe, json{{"set", json{{"decision", handler_loops ? "retry" : "stop"}}}})}};
    json edges = json::array({edge("__start__", "IMPLEMENT"), edge("IMPLEMENT", "TEST")});
    json conditional = json::array(
        {json{{"from", "TEST"}, {"condition", "fr.again"},
              {"routes", json{{"again", "IMPLEMENT"}, {"done", "__end__"}}}},
         json{{"from", "ASK_USER"}, {"condition", "fr.decision"},
              {"routes", json{{"retry", "IMPLEMENT"}, {"stop", "__end__"}}}}});
    json extra{{"conditional_edges", std::move(conditional)},
               {"node_retry_policies", json{{"TEST", json{{"max_visits", max_visits}}}}}};
    if (with_edge) extra["failure_edges"] = json::array({failure_edge("TEST", "ASK_USER")});
    return definition_of(std::move(nodes), std::move(edges), std::move(extra));
}

}  // namespace

TEST(FailureRoutingRuntime, MaxVisitsEscalatesAlongTheFailureEdge) {
    const auto probe = unique_probe("visits");
    RunConfig  config;
    config.max_steps = 40;
    const auto result = build(visit_loop(probe, 2, true, false))->run(config);
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(calls(probe, "TEST"), 2);
    const json expected{{"node", "TEST"},   {"kind", "visit_limit"}, {"code", "NG.VisitLimit"},
                        {"attempts", 0},    {"retryable", false},    {"step", 5},
                        {"routed_to", "ASK_USER"}, {"effects", json::array()}};
    EXPECT_EQ(channel(result, "failures"), json::array({expected}));
    EXPECT_EQ(result.execution_trace.back(), "ASK_USER");
}

TEST(FailureRoutingRuntime, TheVisitCounterResetsWhenTheTargetCompletes) {
    // The handler loops back once; TEST then gets its two visits again before
    // the next escalation. The run ends at the step limit, as the loop never
    // stops: a run budget stays the global backstop.
    const auto probe = unique_probe("visits-reset");
    RunConfig  config;
    config.max_steps = 13;
    const auto result = build(visit_loop(probe, 2, true, true))->run(config);
    EXPECT_EQ(result.status(), RunStatus::StepLimit);
    EXPECT_EQ(calls(probe, "TEST"), 4);
    EXPECT_EQ(channel(result, "failures").size(), 2U);
}

TEST(FailureRoutingRuntime, WithoutAFailureEdgeTheVisitLimitFailsTheRun) {
    const auto probe = unique_probe("visits-no-edge");
    RunConfig  config;
    config.max_steps = 40;
    auto engine = build(visit_loop(probe, 1, false, false));
    try {
        (void)engine->run(config);
        FAIL() << "the visit limit fails a node without a failure edge";
    } catch (const NodeExecutionError& error) {
        const auto* record = node_failure_of(error);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->code, "NG.VisitLimit");
        EXPECT_EQ(record->kind, NodeFailureKind::VisitLimit);
        EXPECT_EQ(record->attempts, 0);
    }
    EXPECT_EQ(calls(probe, "TEST"), 1);
}

TEST(FailureRoutingRuntime, VisitCountersLiveInCheckpointMetadataOfDeclaringNodesOnly) {
    const auto probe = unique_probe("visits-metadata");
    auto       store = std::make_shared<InMemoryCheckpointStore>();
    EngineConfig config;
    config.checkpoint_store = store;
    RunConfig run;
    run.thread_id = "visits";
    run.max_steps = 40;
    (void)build(visit_loop(probe, 2, true, false), std::move(config))->run(run);
    std::vector<json> seen;
    for (const auto& checkpoint : store->list("visits")) {
        const auto& metadata = checkpoint.metadata;
        if (metadata.is_object() && metadata.contains("_neograph") &&
            metadata["_neograph"].contains("node_visits"))
            seen.push_back(metadata["_neograph"]["node_visits"]);
    }
    ASSERT_FALSE(seen.empty());
    for (const auto& visits : seen) {
        EXPECT_EQ(visits.size(), 1U) << visits.dump();
        EXPECT_TRUE(visits.contains("TEST")) << visits.dump();
    }

    // A graph without max_visits writes no counters at all.
    const auto plain       = unique_probe("visits-none");
    auto       plain_store = std::make_shared<InMemoryCheckpointStore>();
    EngineConfig plain_config;
    plain_config.checkpoint_store = plain_store;
    RunConfig plain_run;
    plain_run.thread_id = "plain";
    (void)build(fix_loop(plain, json{{"fail", -1}}, json()), std::move(plain_config))->run(plain_run);
    for (const auto& checkpoint : plain_store->list("plain")) {
        const auto& metadata = checkpoint.metadata;
        EXPECT_FALSE(metadata.is_object() && metadata.contains("_neograph") &&
                     metadata["_neograph"].contains("node_visits"))
            << metadata.dump();
    }
}

// ---------------------------------------------------------------------------
// Failure exits, interrupt payload, events and free text
// ---------------------------------------------------------------------------

TEST(FailureRoutingRuntime, AFailureExitEndsTheRunAsFailureExit) {
    const auto probe = unique_probe("exit");
    json nodes{{"A", step(probe)}, {"ABORT", step(probe)}, {"B", step(probe)}, {"C", step(probe)},
               {"D", step(probe)}};
    json edges = json::array({edge("__start__", "A"), edge("__start__", "B"), edge("A", "ABORT"),
                              edge("B", "C"), edge("C", "D"), edge("D", "__end__")});
    auto store = std::make_shared<InMemoryCheckpointStore>();
    for (const auto& [declared, expected_code] :
         std::vector<std::pair<json, std::string>>{{json{{"code", "plan.user_abort"}}, "plan.user_abort"},
                                                   {json::object(), "NG.Aborted"}}) {
        SCOPED_TRACE(expected_code);
        EngineConfig config;
        config.checkpoint_store = store;
        auto engine = build(definition_of(nodes, edges, json{{"failure_exits", json{{"ABORT", declared}}}}),
                            std::move(config));
        RunConfig run;
        run.thread_id = "exit-" + expected_code;
        const auto result = engine->run(run);
        EXPECT_EQ(result.status(), RunStatus::FailureExit);
        EXPECT_TRUE(result.failure_exited());
        EXPECT_EQ(result.output["_neograph"]["failure_exit"],
                  (json{{"node", "ABORT"}, {"code", expected_code}}));
        // The abort ends the run after its super-step: its sibling C ran in
        // that step, but D, after it, never runs.
        EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"A", "B", "ABORT", "C"}));
        // Resuming the final checkpoint still reports the failure exit.
        const auto resumed = engine->resume(run.thread_id);
        EXPECT_EQ(resumed.status(), RunStatus::FailureExit);
    }
}

TEST(FailureRoutingRuntime, AFailureExitIsAnInvocationError) {
    const auto probe = unique_probe("exit-invocation");
    json nodes{{"A", step(probe)}, {"ABORT", step(probe)}};
    json edges = json::array({edge("__start__", "A"), edge("A", "ABORT")});
    std::shared_ptr<GraphEngine> engine(
        build(definition_of(nodes, edges, json{{"failure_exits", json{{"ABORT", json{{"code", "plan.stop"}}}}}}))
            .release());
    RunInvocationRequest request;
    request.config.thread_id = "exit-invocation";
    RunInvocation invocation(engine, std::move(request));
    const auto result = invocation.run();
    EXPECT_EQ(result.status, InvocationStatus::Error);
    EXPECT_NE(result.error.find("plan.stop"), std::string::npos) << result.error;
    EXPECT_FALSE(result.succeeded());
}

TEST(FailureRoutingRuntime, AnInterruptBeforeTheTargetCarriesTheFailureRecord) {
    const auto probe = unique_probe("interrupt");
    auto definition  = fix_loop(probe, json{{"fail", -1}}, fast_retries(1));
    definition["interrupt_before"] = json::array({"ASK_USER"});
    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    auto      engine = build(definition, std::move(config));
    RunConfig run;
    run.thread_id     = "interrupt";
    const auto paused = engine->run(run);
    ASSERT_EQ(paused.status(), RunStatus::Interrupted);
    EXPECT_EQ(paused.interrupt_node, "ASK_USER");
    EXPECT_EQ(paused.interrupt_value["reason"], json("failure_route"));
    EXPECT_EQ(paused.interrupt_value["failure"], channel(paused, "failures")[0]);
    EXPECT_EQ(paused.interrupt_value["failure"]["attempts"], json(2));

    const auto resumed = engine->resume("interrupt");
    EXPECT_EQ(resumed.status(), RunStatus::Completed);
    EXPECT_EQ(resumed.execution_trace, (std::vector<std::string>{"ASK_USER"}));
    EXPECT_EQ(calls(probe, "TEST"), 2);
}

TEST(FailureRoutingRuntime, ErrorEventsSayWhetherTheFailureWasHandled) {
    const auto probe = unique_probe("events");
    std::vector<json> errors;
    const auto result = build(fix_loop(probe, json{{"fail", -1}}, fast_retries(0)))
                            ->run_stream(RunConfig{}, [&](const GraphEvent& event) {
                                if (event.type == GraphEvent::Type::ERROR) errors.push_back(event.data);
                            });
    EXPECT_EQ(result.status(), RunStatus::Completed);
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0]["handled"], json(true));
    EXPECT_EQ(errors[0]["routed_to"], json("ASK_USER"));
    EXPECT_EQ(errors[0]["attempts"], json(1));

    const auto unhandled = unique_probe("events-unhandled");
    std::vector<json> unhandled_errors;
    EXPECT_THROW((void)build(fix_loop(unhandled, json{{"fail", -1}, {"throw", "logic"}}, json()))
                     ->run_stream(RunConfig{},
                                  [&](const GraphEvent& event) {
                                      if (event.type == GraphEvent::Type::ERROR)
                                          unhandled_errors.push_back(event.data);
                                  }),
                 NodeExecutionError);
    ASSERT_EQ(unhandled_errors.size(), 1U);
    EXPECT_EQ(unhandled_errors[0]["handled"], json(false));
    EXPECT_FALSE(unhandled_errors[0].contains("routed_to"));
}

TEST(FailureRoutingRuntime, FreeTextIsOptInRedactedAndSanitized) {
    auto policy           = fast_retries(0);
    policy["error_text"]  = true;
    const json throws_code{{"fail", -1}, {"throw", "code"}, {"code", "app.check"},
                           {"message", "secret=hunter2 \xE2\x80\xAE" + std::string(600, 'x')}};

    // Opt-in with a redactor: the redactor decides, the engine sanitizes and truncates.
    {
        const auto probe = unique_probe("text");
        EngineConfig config;
        std::vector<std::string> seen;
        config.failure_text_redactor = [&seen](const NodeFailure& failure) {
            seen.push_back(failure.public_message);
            auto text = failure.public_message;
            text.replace(text.find("hunter2"), 7, "[redacted]");
            return text;
        };
        const auto result = build(fix_loop(probe, throws_code, policy), std::move(config))->run(RunConfig{});
        const auto detail = channel(result, "failures")[0]["detail"];
        ASSERT_TRUE(detail.contains("untrusted_text")) << channel(result, "failures").dump();
        const auto text = detail["untrusted_text"].get<std::string>();
        EXPECT_EQ(text.rfind("secret=[redacted] ", 0), 0U) << text;
        EXPECT_EQ(text.find("\xE2\x80\xAE"), std::string::npos);  // the bidi override is stripped
        EXPECT_LE(text.size(), 512U);
        ASSERT_EQ(seen.size(), 1U);
    }
    // Opt-in without a redactor: no text.
    {
        const auto probe  = unique_probe("text-no-redactor");
        const auto result = build(fix_loop(probe, throws_code, policy))->run(RunConfig{});
        EXPECT_FALSE(channel(result, "failures")[0].contains("detail"));
    }
    // A redactor without the opt-in: no text.
    {
        const auto   probe = unique_probe("text-no-opt-in");
        EngineConfig config;
        config.failure_text_redactor = [](const NodeFailure& failure) { return failure.public_message; };
        const auto result =
            build(fix_loop(probe, throws_code, fast_retries(0)), std::move(config))->run(RunConfig{});
        EXPECT_FALSE(channel(result, "failures")[0].contains("detail"));
    }
    // what() is never a source: an ordinary exception has no public message.
    {
        const auto   probe = unique_probe("text-what");
        EngineConfig config;
        config.failure_text_redactor = [](const NodeFailure& failure) {
            return failure.public_message.empty() ? std::string("from what") : failure.public_message;
        };
        const auto result =
            build(fix_loop(probe, json{{"fail", -1}}, policy), std::move(config))->run(RunConfig{});
        EXPECT_FALSE(channel(result, "failures")[0].contains("detail"));
    }
}

// ---------------------------------------------------------------------------
// Crash and resume
// ---------------------------------------------------------------------------

namespace {

// Delegates to a SQLite store and "crashes" (throws) on the first save of a
// committed super-step at `crash_step`: the handled pending write is durable,
// the step commit is not.
class CrashingStore final : public CheckpointStore {
public:
    CrashingStore(std::shared_ptr<CheckpointStore> inner, int crash_step)
        : inner_(std::move(inner)), crash_step_(crash_step) {}

    void save(const Checkpoint& checkpoint) override {
        if (!crashed_ && checkpoint.step == crash_step_ &&
            checkpoint.interrupt_phase == CheckpointPhase::Completed) {
            crashed_ = true;
            throw std::runtime_error("process lost before the step commit");
        }
        inner_->save(checkpoint);
    }
    std::optional<Checkpoint> load_latest(const std::string& thread) override { return inner_->load_latest(thread); }
    std::optional<Checkpoint> load_by_id(const std::string& id) override { return inner_->load_by_id(id); }
    std::vector<Checkpoint> list(const std::string& thread, int limit) override { return inner_->list(thread, limit); }
    void delete_thread(const std::string& thread) override { inner_->delete_thread(thread); }
    void put_writes(const std::string& thread, const std::string& parent, const PendingWrite& write) override {
        inner_->put_writes(thread, parent, write);
    }
    std::vector<PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override {
        inner_->clear_writes(thread, parent);
    }
    bool requires_managed_budget(const std::string& thread) override {
        return inner_->requires_managed_budget(thread);
    }

private:
    std::shared_ptr<CheckpointStore> inner_;
    int                              crash_step_;
    bool                             crashed_ = false;
};

}  // namespace

TEST(FailureRoutingRuntime, CrashResumeReplaysAHandledRouteWithoutRerunningTheNode) {
    const TempDir dir;
    const auto    probe      = unique_probe("crash");
    const auto    definition = fix_loop(probe, json{{"fail", -1}}, fast_retries(2));
    RunConfig     run;
    run.thread_id = "crash";

    {  // Process 1: TEST fails three times and is handled; the commit is lost.
        EngineConfig config;
        config.checkpoint_store = std::make_shared<CrashingStore>(
            std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite")), 1);
        auto engine = build(definition, std::move(config));
        EXPECT_THROW((void)engine->run(run), std::runtime_error);
        EXPECT_EQ(calls(probe, "TEST"), 3);
        EXPECT_EQ(calls(probe, "ASK_USER"), 0);
    }
    {  // Process 2: a new engine on the same file resumes and replays the route.
        EngineConfig config;
        config.checkpoint_store = std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite"));
        auto       engine  = build(definition, std::move(config));
        const auto resumed = engine->resume("crash");
        EXPECT_EQ(resumed.status(), RunStatus::Completed);
        EXPECT_EQ(calls(probe, "TEST"), 3) << "resume must not run the handled node again";
        EXPECT_EQ(calls(probe, "ASK_USER"), 1);
        EXPECT_EQ(resumed.execution_trace, (std::vector<std::string>{"TEST", "ASK_USER"}));
        ASSERT_EQ(channel(resumed, "failures").size(), 1U);
        EXPECT_EQ(channel(resumed, "failures")[0]["attempts"], json(3));
    }
}

// ---------------------------------------------------------------------------
// Per-field retry timing: the host may only tighten a sealed per-node policy
// ---------------------------------------------------------------------------

namespace {

json timing_graph() {
    json nodes{{"SEALED", step("timing")}, {"PARTIAL", step("timing")}, {"PLAIN", step("timing")}};
    json edges = json::array({edge("__start__", "SEALED"), edge("SEALED", "PARTIAL"),
                              edge("PARTIAL", "PLAIN"), edge("PLAIN", "__end__")});
    return definition_of(nodes, edges,
                 json{{"retry_policy", json{{"max_retries", 1}, {"initial_delay_ms", 100},
                                            {"backoff_multiplier", 2.0}, {"max_delay_ms", 5000}}},
                      {"node_retry_policies",
                       json{{"SEALED", json{{"max_retries", 4}, {"initial_delay_ms", 50},
                                            {"backoff_multiplier", 1.5}, {"max_delay_ms", 400}}},
                            {"PARTIAL", json{{"max_visits", 3}}}}}});
}

RetryPolicy policy(int retries, int initial, float multiplier, int max_delay, float jitter = 0.0f) {
    RetryPolicy value;
    value.max_retries        = retries;
    value.initial_delay_ms   = initial;
    value.backoff_multiplier = multiplier;
    value.max_delay_ms       = max_delay;
    value.jitter_pct         = jitter;
    return value;
}

void expect_policy(const RetryPolicy& actual, const RetryPolicy& expected) {
    EXPECT_EQ(actual.max_retries, expected.max_retries);
    EXPECT_EQ(actual.initial_delay_ms, expected.initial_delay_ms);
    EXPECT_FLOAT_EQ(actual.backoff_multiplier, expected.backoff_multiplier);
    EXPECT_EQ(actual.max_delay_ms, expected.max_delay_ms);
    EXPECT_FLOAT_EQ(actual.jitter_pct, expected.jitter_pct);
}

}  // namespace

TEST(FailureRoutingRuntime, RetryTimingPrecedenceMatrix) {
    {
        SCOPED_TRACE("definition only");
        auto engine = build(timing_graph());
        expect_policy(engine->resolved_retry_policy("PLAIN"), policy(1, 100, 2.0f, 5000));
        expect_policy(engine->resolved_retry_policy("SEALED"), policy(4, 50, 1.5f, 400));
        expect_policy(engine->resolved_retry_policy("PARTIAL"), policy(1, 100, 2.0f, 5000));
    }
    {
        SCOPED_TRACE("host default replaces the definition's default, not per-node fields");
        EngineConfig config;
        config.retry_policy = policy(2, 20, 3.0f, 900);
        auto engine         = build(timing_graph(), std::move(config));
        expect_policy(engine->resolved_retry_policy("PLAIN"), policy(2, 20, 3.0f, 900));
        expect_policy(engine->resolved_retry_policy("SEALED"), policy(4, 50, 1.5f, 400));
        expect_policy(engine->resolved_retry_policy("PARTIAL"), policy(2, 20, 3.0f, 900));
    }
    {
        SCOPED_TRACE("host per-node: replaces an unsealed node, only tightens a sealed one");
        EngineConfig config;
        config.node_retry_policies["PLAIN"]   = policy(7, 1, 1.0f, 1);
        config.node_retry_policies["SEALED"]  = policy(9, 10, 1.2f, 100, 0.25f);
        config.node_retry_policies["PARTIAL"] = policy(6, 500, 4.0f, 6000);
        auto engine = build(timing_graph(), std::move(config));
        expect_policy(engine->resolved_retry_policy("PLAIN"), policy(7, 1, 1.0f, 1));
        expect_policy(engine->resolved_retry_policy("SEALED"), policy(4, 50, 1.5f, 400, 0.25f));
        expect_policy(engine->resolved_retry_policy("PARTIAL"), policy(1, 500, 4.0f, 6000));
    }
    {
        SCOPED_TRACE("tightening: fewer retries, longer delays, a larger multiplier");
        auto engine = build(timing_graph());
        engine->set_node_retry_policy("SEALED", policy(1, 500, 3.0f, 9000));
        expect_policy(engine->resolved_retry_policy("SEALED"), policy(1, 500, 3.0f, 9000));
        engine->set_retry_policy(policy(30, 1, 1.0f, 1));
        // A sealed node never exceeds 10 retries, even from a host default.
        expect_policy(engine->resolved_retry_policy("PARTIAL"), policy(10, 1, 1.0f, 1));
        expect_policy(engine->resolved_retry_policy("PLAIN"), policy(30, 1, 1.0f, 1));
    }
    {
        SCOPED_TRACE("unknown node names are still rejected (#376)");
        EngineConfig config;
        config.node_retry_policies["MISSING"] = policy(1, 1, 1.0f, 1);
        EXPECT_THROW((void)build(timing_graph(), std::move(config)), std::invalid_argument);
        auto engine = build(timing_graph());
        EXPECT_THROW(engine->set_node_retry_policy("MISSING", policy(1, 1, 1.0f, 1)),
                     std::invalid_argument);
    }
}

TEST(FailureRoutingRuntime, TheTightenedTimingIsWhatTheRetryLoopRuns) {
    const auto probe = unique_probe("timing-run");
    EngineConfig config;
    config.node_retry_policies["TEST"] = policy(1, 10, 1.0f, 10);
    const auto result =
        build(fix_loop(probe, json{{"fail", -1}}, fast_retries(4)), std::move(config))->run(RunConfig{});
    EXPECT_EQ(calls(probe, "TEST"), 2);
    EXPECT_EQ(channel(result, "failures")[0]["attempts"], json(2));
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

TEST(FailureRoutingScheduler, AFailedRoutingFollowsOnlyItsFailureEdge) {
    std::vector<Edge>            edges{{"A", "B"}, {"C", "D"}, {"F", "__end__"}};
    std::vector<ConditionalEdge> conditional;
    Scheduler scheduler(edges, conditional, {}, {}, {{"A", "F"}}, nullptr);
    GraphState   state;
    BarrierState barrier;
    std::vector<StepRouting> routings{{"A", std::nullopt}, {"C", std::nullopt}};
    auto plan = scheduler.plan_next_step(routings, state, barrier, std::vector<bool>{true, false});
    EXPECT_EQ(plan.ready, (std::vector<std::string>{"D", "F"}));
    EXPECT_FALSE(plan.hit_end);
    plan = scheduler.plan_next_step(routings, state, barrier, std::vector<bool>{});
    EXPECT_EQ(plan.ready, (std::vector<std::string>{"B", "D"}));
    EXPECT_THROW((void)scheduler.plan_next_step(routings, state, barrier, std::vector<bool>{false, true}),
                 std::logic_error);
    EXPECT_THROW((void)scheduler.plan_next_step(routings, state, barrier, std::vector<bool>{true}),
                 std::invalid_argument);
    ASSERT_NE(scheduler.failure_target("A"), nullptr);
    EXPECT_EQ(*scheduler.failure_target("A"), "F");
    EXPECT_EQ(scheduler.failure_target("C"), nullptr);
}
