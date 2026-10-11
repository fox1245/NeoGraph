// Attempt timeouts and deadline-aware backoff (#364, Program API v2 WP12), on
// the dispatched-attempt execution model (#367, #368).
//
// A node attempt with an attempt timeout is one dispatched unit: its body runs
// off the coroutine that drives the super-step, with a CancelToken of its own.
// The first terminal event wins: the body completes, or the timeout passes.
// A timed-out attempt is cancelled through its token and fails with
// NG.Timeout (retried, routable); whatever it returns, writes, emits or calls
// afterwards (its late completion) is discarded. The next attempt starts only
// after the cancelled one has stopped; one that does not stop by its stop
// deadline (the timeout again, at least a second) fails the run with
// NG.EffectUncertain. A backoff that would cross the run's deadline stops
// retrying with kind "deadline".
//
// Every dispatch path (a single node, fan-out branches, sequential and
// parallel Sends, subgraph nodes, the sync and async entry points) takes the
// same route through the one retry loop.

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/invocation.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/tool_dispatch.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

// ---------------------------------------------------------------------------
// What each probe saw, call by call
// ---------------------------------------------------------------------------

struct Call {
    std::string        node;
    std::string        effect_namespace;
    Clock::time_point  started;
    Clock::time_point  stopped;
    bool               saw_cancellation = false;
};

struct Probe {
    std::mutex        mutex;
    std::vector<Call> calls;
    int               tool_executions = 0;
    int               tool_refusals   = 0;
};

std::shared_ptr<Probe> probe(const std::string& name) {
    static std::mutex                                      mutex;
    static std::map<std::string, std::shared_ptr<Probe>> probes;
    std::lock_guard                                        lock(mutex);
    auto&                                                  slot = probes[name];
    if (!slot) slot = std::make_shared<Probe>();
    return slot;
}

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

int calls(const std::string& name) {
    auto            seen = probe(name);
    std::lock_guard lock(seen->mutex);
    return static_cast<int>(seen->calls.size());
}

// Counts its executions in the probe named by the "probe" argument.
class CountingTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return ChatTool{"count", "counts executions", json{{"type", "object"}}};
    }
    std::string execute(const json& arguments) override {
        auto            seen = probe(arguments.at("probe").get<std::string>());
        std::lock_guard lock(seen->mutex);
        ++seen->tool_executions;
        return "counted";
    }
    std::string get_name() const override { return "count"; }
};

CountingTool& counting_tool() {
    static CountingTool tool;
    return tool;
}

// One configurable node. "plan" lists what each call does (the last entry
// repeats); every entry may set:
//   block: ms     sleep the thread, ignoring cancellation
//   wait: ms      wait on a timer bound to ctx.cancel_token (cancellable)
//   emit: true    emit an LLM_TOKEN event {"call": k} through stream_cb
//   tool: true    call the counting tool once through mediated dispatch
//   fail: true    throw std::runtime_error
// A call that does not fail writes {"node:k"} to the "trace" channel and
// {"call": k} to "last".
class AttemptNode final : public GraphNode {
public:
    AttemptNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto probe_name = config_.at("probe").get<std::string>();
        auto       seen       = probe(probe_name);
        std::size_t index      = 0;
        {
            std::lock_guard lock(seen->mutex);
            index = seen->calls.size();
            Call call;
            call.node             = name_;
            call.effect_namespace = make_tool_execution_context(in.ctx).effect_task_id;
            call.started          = Clock::now();
            seen->calls.push_back(std::move(call));
        }
        const auto& plan  = config_.at("plan");
        const auto  entry = plan.at(std::min<std::size_t>(index, plan.size() - 1));
        const auto  call  = static_cast<int>(index);

        std::exception_ptr failure;
        try {
            if (entry.contains("block"))
                std::this_thread::sleep_for(milliseconds(entry["block"].get<int>()));
            if (entry.contains("wait")) {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(milliseconds(entry["wait"].get<int>()));
                co_await timer.async_wait(
                    asio::bind_cancellation_slot(in.ctx.cancel_token->slot(), asio::use_awaitable));
            }
            if (entry.value("emit", false) && in.stream_cb)
                (*in.stream_cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_,
                                           json{{"call", call}, {"probe", probe_name}}});
            if (entry.value("tool", false)) {
                std::vector<ToolCall> tool_calls{
                    ToolCall{"call-" + std::to_string(call), "count", json{{"probe", probe_name}}.dump()}};
                std::vector<Tool*> tools{&counting_tool()};
                (void)co_await dispatch_tool_calls(std::move(tool_calls), std::move(tools), {}, {},
                                                    make_tool_execution_context(in.ctx));
            }
        } catch (const CancelledException&) {
            std::lock_guard lock(seen->mutex);
            ++seen->tool_refusals;
            failure = std::current_exception();
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(seen->mutex);
            seen->calls[index].stopped          = Clock::now();
            seen->calls[index].saw_cancellation = in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled();
        }
        if (failure) std::rethrow_exception(failure);
        if (entry.value("fail", false)) throw std::runtime_error("boom from " + name_);

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_ + ":" + std::to_string(call)})});
        out.writes.push_back(ChannelWrite{"last", json{{"call", call}}});
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
            "at.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<AttemptNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

json node(const std::string& probe_name, json plan, json extra = json::object()) {
    json config{{"type", "at.node"}, {"probe", probe_name}, {"plan", std::move(plan)}};
    for (const auto& [key, value] : extra.items()) config[key] = value;
    return config;
}
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json channels() {
    return json{{"trace", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"failures", json{{"reducer", "append"}, {"initial", json::array()}}},
                {"last", json{{"reducer", "overwrite"}, {"initial", nullptr}}},
                {"item", json{{"reducer", "overwrite"}, {"initial", nullptr}}}};
}

json definition_of(json nodes, json edges, json extra = json::object()) {
    json definition{{"schema_version", 1}, {"name", "at"}, {"channels", channels()},
                    {"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
    for (const auto& [key, value] : extra.items()) definition[key] = value;
    return definition;
}

json timed(int timeout_ms, int max_retries) {
    return json{{"attempt_timeout_ms", timeout_ms}, {"max_retries", max_retries},
                {"initial_delay_ms", 10}, {"backoff_multiplier", 1.0}, {"max_delay_ms", 10}};
}

std::unique_ptr<GraphEngine> build(const json& definition, EngineConfig config = {}) {
    register_types();
    config.stderr_notices = false;
    return GraphEngine::build(definition, std::move(config));
}

json channel(const RunResult& result, const std::string& name) {
    return result.output["channels"][name]["value"];
}

// START -> N -> END, N with `policy`; with `handler`, N fails along its failure
// edge to H.
json single(const std::string& probe_name, json plan, json policy, bool handler = false) {
    json nodes{{"N", node(probe_name, std::move(plan))}};
    json edges = json::array({edge("__start__", "N"), edge("N", "__end__")});
    json extra{{"node_retry_policies", json{{"N", std::move(policy)}}}};
    if (handler) {
        nodes["H"] = node(probe_name, json::array({json::object()}));
        edges.push_back(edge("H", "__end__"));
        extra["failure_edges"] =
            json::array({json{{"from", "N"}, {"to", "H"}, {"error_channel", "failures"}}});
    }
    return definition_of(std::move(nodes), std::move(edges), std::move(extra));
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> next{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ng-attempt-timeout-" +
                 std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
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

const NodeFailure& failure_of(const std::exception_ptr& error) {
    static thread_local std::optional<NodeFailure> record;
    record = node_failure_of(error);
    if (!record) throw std::runtime_error("no node failure record");
    return *record;
}

std::exception_ptr run_capturing(GraphEngine& engine, const RunConfig& config = {},
                                  const RunMetadata& metadata = {}) {
    try {
        (void)engine.run(config, metadata);
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Timeout, retry and the late completion
// ---------------------------------------------------------------------------

TEST(AttemptTimeout, ASlowAttemptIsCancelledAndRetriedThenSucceeds) {
    const auto name = unique_probe("cooperative");
    // The first call waits 5 s on its token; the timeout cancels it at 150 ms.
    auto       engine = build(single(name, json::array({json{{"wait", 5000}}, json::object()}),
                                     timed(150, 1)));
    const auto started = Clock::now();
    const auto result  = engine->run(RunConfig{});
    const auto elapsed = Clock::now() - started;

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_LT(elapsed, milliseconds(1500));
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"N"}));
    // Only the retry wrote anything.
    EXPECT_EQ(channel(result, "trace"), json::array({"N:1"}));
    EXPECT_EQ(channel(result, "last"), (json{{"call", 1}}));
    auto            seen = probe(name);
    std::lock_guard lock(seen->mutex);
    ASSERT_EQ(seen->calls.size(), 2U);
    EXPECT_TRUE(seen->calls[0].saw_cancellation) << "the attempt is cancelled through its own token";
    EXPECT_FALSE(seen->calls[1].saw_cancellation);
    // Each attempt has its own effect namespace.
    EXPECT_EQ(seen->calls[0].effect_namespace, "s0:N");
    EXPECT_EQ(seen->calls[1].effect_namespace, "s0:N#a1");
}

TEST(AttemptTimeout, TheRetryStartsOnlyAfterTheCancelledAttemptStopped) {
    const auto name = unique_probe("blocking");
    // The first call ignores cancellation for 600 ms; the timeout is 150 ms.
    auto       engine = build(single(name, json::array({json{{"block", 600}}, json::object()}),
                                     timed(150, 1)));
    const auto result = engine->run(RunConfig{});

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(channel(result, "trace"), json::array({"N:1"}));
    auto            seen = probe(name);
    std::lock_guard lock(seen->mutex);
    ASSERT_EQ(seen->calls.size(), 2U);
    EXPECT_GE(seen->calls[1].started, seen->calls[0].stopped) << "attempts never overlap";
}

TEST(AttemptTimeout, ALateCompletionIsDiscarded) {
    // The first call blocks past its timeout, then emits an event, calls a
    // tool through mediated dispatch and returns writes. None of that counts:
    // the tool call is refused (its token is cancelled), the event never
    // reaches the run, and nothing it returned is written.
    const auto name   = unique_probe("late");
    auto       engine = build(single(name,
                                     json::array({json{{"block", 500}, {"emit", true}, {"tool", true}},
                                                  json{{"emit", true}, {"tool", true}}}),
                                     timed(150, 1)));
    std::mutex              events_mutex;
    std::vector<GraphEvent> events;
    RunConfig               config;
    config.stream_mode = StreamMode::ALL;
    const auto result  = engine->run_stream(config, [&](const GraphEvent& event) {
        std::lock_guard lock(events_mutex);
        events.push_back(event);
    });

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(channel(result, "trace"), json::array({"N:1"}));
    auto seen = probe(name);
    {
        std::lock_guard lock(seen->mutex);
        EXPECT_EQ(seen->tool_executions, 1) << "no double effect";
        EXPECT_EQ(seen->tool_refusals, 1) << "the late attempt's call is refused";
    }
    std::lock_guard lock(events_mutex);
    int tokens = 0;
    for (const auto& event : events) {
        if (event.type != GraphEvent::Type::LLM_TOKEN) continue;
        ++tokens;
        EXPECT_EQ(event.data.value("call", -1), 1) << "an event of the timed-out attempt";
    }
    EXPECT_EQ(tokens, 1);
    // The retry's NODE_START is the next thing after the timeout; nothing of
    // the first attempt follows it, and N ends once.
    int starts = 0;
    int ends   = 0;
    for (const auto& event : events) {
        if (event.node_name != "N") continue;
        if (event.type == GraphEvent::Type::NODE_START) ++starts;
        if (event.type == GraphEvent::Type::NODE_END) ++ends;
    }
    EXPECT_EQ(starts, 2);
    EXPECT_EQ(ends, 1);
}

TEST(AttemptTimeout, TimeoutExhaustionRoutesAlongTheFailureEdge) {
    const auto name   = unique_probe("route");
    auto       engine = build(single(name, json::array({json{{"wait", 5000}}}), timed(100, 2), true));
    const auto result = engine->run(RunConfig{});

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"N", "H"}));
    const json expected{{"node", "N"},        {"kind", "exhausted"}, {"code", "NG.Timeout"},
                        {"attempts", 3},      {"retryable", true},   {"step", 0},
                        {"routed_to", "H"},   {"effects", json::array()}};
    EXPECT_EQ(channel(result, "failures"), json::array({expected}));
    // N's timed-out attempts wrote nothing; only H did (its first call is the
    // probe's fourth).
    EXPECT_EQ(channel(result, "trace"), json::array({"H:3"}));
}

TEST(AttemptTimeout, TimeoutExhaustionWithoutAFailureEdgeFailsTheRun) {
    const auto name   = unique_probe("unrouted");
    auto       engine = build(single(name, json::array({json{{"wait", 5000}}}), timed(100, 1)));
    const auto error  = run_capturing(*engine);
    ASSERT_TRUE(error);
    const auto& record = failure_of(error);
    EXPECT_EQ(record.code, "NG.Timeout");
    EXPECT_EQ(record.failure_class, NodeFailureClass::Timeout);
    EXPECT_EQ(record.kind, NodeFailureKind::Exhausted);
    EXPECT_EQ(record.attempts, 2);
    EXPECT_TRUE(record.retryable);
    EXPECT_TRUE(record.routable);
    try {
        std::rethrow_exception(error);
    } catch (const NodeExecutionError& failure) {
        try {
            std::rethrow_exception(failure.cause());
        } catch (const NodeAttemptTimeout& timeout) {
            EXPECT_EQ(timeout.node(), "N");
            EXPECT_EQ(timeout.attempt(), 1);
            EXPECT_EQ(timeout.timeout(), milliseconds(100));
            EXPECT_TRUE(timeout.stopped());
        }
    }
    EXPECT_EQ(calls(name), 2);
}

TEST(AttemptTimeout, NonRetryableTimeoutsRunOnce) {
    const auto name   = unique_probe("non-retryable");
    auto       policy = timed(100, 3);
    policy["non_retryable"] = json::array({"NG.Timeout"});
    auto       engine = build(single(name, json::array({json{{"wait", 5000}}}), policy, true));
    const auto result = engine->run(RunConfig{});
    ASSERT_EQ(channel(result, "failures").size(), 1U);
    EXPECT_EQ(channel(result, "failures")[0]["kind"], json("non_retryable"));
    EXPECT_EQ(channel(result, "failures")[0]["code"], json("NG.Timeout"));
    EXPECT_EQ(channel(result, "failures")[0]["attempts"], json(1));
    EXPECT_EQ(calls(name), 2);  // N once, then H
}

// ---------------------------------------------------------------------------
// The stop deadline
// ---------------------------------------------------------------------------

TEST(AttemptTimeout, AnAttemptThatIgnoresCancellationPastItsStopDeadlineFailsTheRun) {
    // Timeout 100 ms, so the stop deadline is one second later. The body
    // blocks for 1.8 s: its effects are uncertain, it is neither retried nor
    // routed (even with a failure edge), and the run fails at the deadline.
    const auto name    = unique_probe("stuck");
    auto       engine  = build(single(name, json::array({json{{"block", 1800}}, json::object()}),
                                      timed(100, 3), true));
    const auto started = Clock::now();
    const auto error   = run_capturing(*engine);
    const auto elapsed = Clock::now() - started;
    ASSERT_TRUE(error);
    EXPECT_GE(elapsed, milliseconds(1000));
    EXPECT_LT(elapsed, milliseconds(1700)) << "the run does not wait for the stuck attempt";
    const auto& record = failure_of(error);
    EXPECT_EQ(record.code, "NG.EffectUncertain");
    EXPECT_EQ(record.failure_class, NodeFailureClass::EffectUncertain);
    EXPECT_EQ(record.kind, NodeFailureKind::NonRetryable);
    EXPECT_FALSE(record.retryable);
    EXPECT_FALSE(record.routable);
    EXPECT_EQ(record.attempts, 1);
    try {
        std::rethrow_exception(error);
    } catch (const NodeExecutionError& failure) {
        try {
            std::rethrow_exception(failure.cause());
        } catch (const NodeAttemptTimeout& timeout) {
            EXPECT_FALSE(timeout.stopped());
        }
    }
    EXPECT_EQ(calls(name), 1) << "no retry, no handler";
    // Destroying the engine does not wait for the detached attempt; the node
    // it runs stays alive until it finishes.
    const auto destroy_started = Clock::now();
    engine.reset();
    EXPECT_LT(Clock::now() - destroy_started, milliseconds(300));
    auto seen = probe(name);
    for (int i = 0; i < 400; ++i) {
        {
            std::lock_guard lock(seen->mutex);
            if (seen->calls.at(0).stopped != Clock::time_point{}) break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    std::lock_guard lock(seen->mutex);
    ASSERT_EQ(seen->calls.size(), 1U);
    EXPECT_NE(seen->calls[0].stopped, Clock::time_point{}) << "the detached body ran to its end";
    EXPECT_GE(seen->calls[0].stopped - seen->calls[0].started, milliseconds(1800));
}

// ---------------------------------------------------------------------------
// Deadline-aware backoff
// ---------------------------------------------------------------------------

TEST(AttemptTimeout, ABackoffThatWouldCrossTheDeadlineStopsWithKindDeadline) {
    const auto name   = unique_probe("deadline");
    auto       policy = json{{"max_retries", 5}, {"initial_delay_ms", 2000}, {"max_delay_ms", 2000}};
    auto       engine = build(single(name, json::array({json{{"fail", true}}}), policy, true));
    RunMetadata metadata;
    metadata.deadline  = Clock::now() + milliseconds(600);
    const auto started = Clock::now();
    const auto result  = engine->run(RunConfig{}, metadata);

    EXPECT_LT(Clock::now() - started, milliseconds(500)) << "no sleep until the deadline";
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"N", "H"}));
    ASSERT_EQ(channel(result, "failures").size(), 1U);
    const auto record = channel(result, "failures")[0];
    EXPECT_EQ(record["kind"], json("deadline"));
    EXPECT_EQ(record["code"], json("NG.TaskFailed"));
    EXPECT_EQ(record["attempts"], json(1));
    EXPECT_EQ(record["retryable"], json(true));
}

TEST(AttemptTimeout, ADeadlineStopWithoutAFailureEdgeFailsTheRun) {
    const auto name   = unique_probe("deadline-unrouted");
    auto       policy = timed(100, 5);
    policy["initial_delay_ms"] = 1000;
    policy["max_delay_ms"]     = 1000;
    auto        engine = build(single(name, json::array({json{{"wait", 5000}}}), policy));
    RunMetadata metadata;
    metadata.deadline = Clock::now() + milliseconds(800);
    const auto error  = run_capturing(*engine, RunConfig{}, metadata);
    ASSERT_TRUE(error);
    const auto& record = failure_of(error);
    // The attempt timed out at 100 ms; a one-second backoff would cross the
    // deadline at 800 ms.
    EXPECT_EQ(record.kind, NodeFailureKind::Deadline);
    EXPECT_EQ(record.code, "NG.Timeout");
    EXPECT_EQ(record.attempts, 1);
    EXPECT_EQ(calls(name), 1);
}

TEST(AttemptTimeout, ABackoffWithinTheDeadlineStillRetries) {
    const auto name   = unique_probe("deadline-room");
    auto       engine = build(single(name, json::array({json{{"fail", true}}, json::object()}),
                                     json{{"max_retries", 1}, {"initial_delay_ms", 50}}));
    RunMetadata metadata;
    metadata.deadline = Clock::now() + milliseconds(5000);
    const auto result = engine->run(RunConfig{}, metadata);
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(channel(result, "trace"), json::array({"N:1"}));
}

// ---------------------------------------------------------------------------
// Resolution: the host may only tighten a definition's timeout
// ---------------------------------------------------------------------------

TEST(AttemptTimeout, TheHostMayOnlyTightenTheAttemptTimeout) {
    const auto name = unique_probe("precedence");
    json       nodes{{"SEALED", node(name, json::array({json::object()}))},
                     {"PARTIAL", node(name, json::array({json::object()}))},
                     {"PLAIN", node(name, json::array({json::object()}))}};
    json       edges = json::array({edge("__start__", "SEALED"), edge("SEALED", "PARTIAL"),
                                    edge("PARTIAL", "PLAIN"), edge("PLAIN", "__end__")});
    const auto definition = definition_of(
        nodes, edges,
        json{{"node_retry_policies", json{{"SEALED", json{{"attempt_timeout_ms", 1000}}},
                                          {"PARTIAL", json{{"max_retries", 1}}}}}});
    auto engine = build(definition);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("SEALED"), 1000);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("PARTIAL"), std::nullopt);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("PLAIN"), std::nullopt);

    // Tighter takes effect; looser does not; clearing the host value restores
    // the definition's.
    engine->set_node_attempt_timeout_ms("SEALED", 500);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("SEALED"), 500);
    engine->set_node_attempt_timeout_ms("SEALED", 5000);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("SEALED"), 1000);
    engine->set_node_attempt_timeout_ms("SEALED", std::nullopt);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("SEALED"), 1000);

    // Without a definition timeout the host value applies, sealed policy or not.
    engine->set_node_attempt_timeout_ms("PARTIAL", 300);
    engine->set_node_attempt_timeout_ms("PLAIN", 700);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("PARTIAL"), 300);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("PLAIN"), 700);

    // Bounds and node names are checked.
    EXPECT_THROW(engine->set_node_attempt_timeout_ms("PLAIN", 99), std::invalid_argument);
    EXPECT_THROW(engine->set_node_attempt_timeout_ms("PLAIN", 3600001), std::invalid_argument);
    EXPECT_THROW(engine->set_node_attempt_timeout_ms("MISSING", 500), std::invalid_argument);
    EXPECT_EQ(engine->resolved_attempt_timeout_ms("PLAIN"), 700);
}

TEST(AttemptTimeout, TheTightenedTimeoutIsWhatTheAttemptRunsWith) {
    // The definition allows 3 s; the host tightens it to 150 ms, so the
    // one-second wait times out and the handler takes over.
    const auto name   = unique_probe("tightened");
    auto       engine = build(single(name, json::array({json{{"wait", 1000}}}), timed(3000, 0), true));
    engine->set_node_attempt_timeout_ms("N", 150);
    const auto started = Clock::now();
    const auto result  = engine->run(RunConfig{});
    EXPECT_LT(Clock::now() - started, milliseconds(800));
    ASSERT_EQ(channel(result, "failures").size(), 1U);
    EXPECT_EQ(channel(result, "failures")[0]["code"], json("NG.Timeout"));
    // A looser host value leaves the definition's 3 s: the wait completes.
    const auto relaxed_name = unique_probe("relaxed");
    auto       relaxed      = build(single(relaxed_name, json::array({json{{"wait", 300}}}), timed(3000, 0)));
    relaxed->set_node_attempt_timeout_ms("N", 3600000);
    EXPECT_EQ(relaxed->run(RunConfig{}).status(), RunStatus::Completed);
}

TEST(AttemptTimeout, AHostTimeoutAppliesToANodeWithoutAPolicy) {
    const auto name = unique_probe("host-only");
    json       nodes{{"N", node(name, json::array({json{{"wait", 5000}}, json::object()}))}};
    json       edges = json::array({edge("__start__", "N"), edge("N", "__end__")});
    EngineConfig config;
    RetryPolicy  retry;
    retry.max_retries      = 1;
    retry.initial_delay_ms = 10;
    config.retry_policy    = retry;
    auto engine = build(definition_of(nodes, edges), std::move(config));
    engine->set_node_attempt_timeout_ms("N", 150);
    const auto result = engine->run(RunConfig{});
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(channel(result, "trace"), json::array({"N:1"}));
}

// ---------------------------------------------------------------------------
// Parity across dispatch paths and entry points
// ---------------------------------------------------------------------------

TEST(AttemptTimeout, AParallelBranchTimesOutWhileItsSiblingApplies) {
    for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
        SCOPED_TRACE(workers);
        const auto name = unique_probe("parallel");
        json       nodes{{"A", node(name, json::array({json{{"wait", 5000}}}))},
                         {"B", node(name, json::array({json::object()}))},
                         {"H", node(name, json::array({json::object()}))}};
        json       edges = json::array({edge("__start__", "A"), edge("__start__", "B"),
                                        edge("A", "__end__"), edge("B", "__end__"), edge("H", "__end__")});
        const auto definition = definition_of(
            nodes, edges,
            json{{"node_retry_policies", json{{"A", timed(100, 1)}}},
                 {"failure_edges", json::array({json{{"from", "A"}, {"to", "H"}, {"error_channel", "failures"}}})}});
        EngineConfig config;
        config.worker_count = workers;
        const auto result   = build(definition, std::move(config))->run(RunConfig{});
        EXPECT_EQ(result.status(), RunStatus::Completed);
        EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"A", "B", "H"}));
        ASSERT_EQ(channel(result, "failures").size(), 1U);
        EXPECT_EQ(channel(result, "failures")[0]["code"], json("NG.Timeout"));
        EXPECT_EQ(channel(result, "failures")[0]["attempts"], json(2));
        // A wrote nothing; B and H did.
        const auto trace = channel(result, "trace");
        ASSERT_EQ(trace.size(), 2U);
        for (const auto& entry : trace) EXPECT_NE(entry.get<std::string>().rfind("A:", 0), 0U);
    }
}

TEST(AttemptTimeout, ASendTargetTimesOutOnBothSendPaths) {
    // FAN sends one item (sequential Send) or three (parallel Sends); W's
    // first call times out, its retry completes.
    for (const auto& items : {json::array({1}), json::array({1, 2, 3})}) {
        for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
            SCOPED_TRACE(items.dump() + " workers " + std::to_string(workers));
            const auto name = unique_probe("send");
            json       nodes{{"FAN", node(name + "-fan", json::array({json::object()}),
                                          json{{"sends", items}, {"send_target", "W"}})},
                             {"W", node(name, json::array({json{{"wait", 5000}}, json::object()}))}};
            json       edges = json::array({edge("__start__", "FAN"), edge("FAN", "__end__"),
                                            edge("W", "__end__")});
            const auto definition =
                definition_of(nodes, edges, json{{"node_retry_policies", json{{"W", timed(150, 1)}}}});
            EngineConfig config;
            config.worker_count = workers;
            const auto result   = build(definition, std::move(config))->run(RunConfig{});
            EXPECT_EQ(result.status(), RunStatus::Completed);
            // Only the first W call timed out; the others completed.
            EXPECT_EQ(calls(name), static_cast<int>(items.size()) + 1);
            int w_writes = 0;
            for (const auto& entry : channel(result, "trace"))
                if (entry.get<std::string>().rfind("W:", 0) == 0) ++w_writes;
            EXPECT_EQ(w_writes, static_cast<int>(items.size()));
            for (const auto& entry : channel(result, "trace")) EXPECT_NE(entry, json("W:0"));
        }
    }
}

TEST(AttemptTimeout, ASubgraphAttemptTimesOutAndItsChildRunIsCancelled) {
    register_types();
    const auto name = unique_probe("subgraph");
    // INNER waits 5 s on its first call; SUB (the parent's node) times out
    // at 150 ms, which cancels the child run through SUB's token.
    const json child_definition{
        {"name", "at_child"},
        {"channels", channels()},
        {"nodes", json{{"INNER", node(name, json::array({json{{"wait", 5000}}, json::object()}))}}},
        {"edges", json::array({edge("__start__", "INNER"), edge("INNER", "__end__")})}};
    std::shared_ptr<GraphEngine> child(GraphEngine::compile(child_definition, NodeContext{}).release());
    static std::atomic<int>      next_type{0};
    const auto                   type = "at.subgraph." + std::to_string(++next_type);
    NodeFactory::instance().register_type(
        type,
        [child](const std::string& node_name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                node_name, child, std::map<std::string, std::string>{},
                std::map<std::string, std::string>{{"trace", "trace"}});
        },
        json{{"type", "object"}});
    json nodes{{"SUB", json{{"type", type}}}};
    json edges = json::array({edge("__start__", "SUB"), edge("SUB", "__end__")});
    const auto started = Clock::now();
    const auto result =
        build(definition_of(nodes, edges, json{{"node_retry_policies", json{{"SUB", timed(150, 1)}}}}))
            ->run(RunConfig{});
    EXPECT_LT(Clock::now() - started, milliseconds(1500));
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(calls(name), 2);
    EXPECT_EQ(channel(result, "trace"), json::array({"INNER:1"}));
    auto            seen = probe(name);
    std::lock_guard lock(seen->mutex);
    EXPECT_TRUE(seen->calls[0].saw_cancellation);
}

TEST(AttemptTimeout, AnInnerNodeTimesOutInsideItsSubgraph) {
    register_types();
    const auto name = unique_probe("inner");
    const json child_definition{
        {"schema_version", 1},
        {"name", "at_inner_child"},
        {"channels", channels()},
        {"nodes", json{{"INNER", node(name, json::array({json{{"wait", 5000}}}))}}},
        {"edges", json::array({edge("__start__", "INNER"), edge("INNER", "__end__")})},
        {"node_retry_policies", json{{"INNER", timed(100, 1)}}}};
    std::shared_ptr<GraphEngine> child(GraphEngine::compile(child_definition, NodeContext{}).release());
    static std::atomic<int>      next_type{0};
    const auto                   type = "at.inner_subgraph." + std::to_string(++next_type);
    NodeFactory::instance().register_type(
        type,
        [child](const std::string& node_name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(node_name, child);
        },
        json{{"type", "object"}});
    json nodes{{"SUB", json{{"type", type}}}, {"H", node(name, json::array({json::object()}))}};
    json edges = json::array({edge("__start__", "SUB"), edge("SUB", "__end__"), edge("H", "__end__")});
    const auto result = build(definition_of(
        nodes, edges,
        json{{"failure_edges", json::array({json{{"from", "SUB"}, {"to", "H"}, {"error_channel", "failures"}}})}}))
                            ->run(RunConfig{});
    EXPECT_EQ(result.status(), RunStatus::Completed);
    ASSERT_EQ(channel(result, "failures").size(), 1U);
    // The child's timeout is the most severe class in SUB's cause chain.
    EXPECT_EQ(channel(result, "failures")[0]["code"], json("NG.Timeout"));
    EXPECT_EQ(calls(name), 3);  // INNER twice, then H
}

TEST(AttemptTimeout, SyncAsyncStreamAndInvocationEntryPointsAgree) {
    const auto name       = unique_probe("entry-points");
    const auto definition = single(name, json::array({json{{"wait", 5000}}}), timed(100, 1), true);
    std::shared_ptr<GraphEngine> engine(build(definition).release());
    RunConfig                    config;
    config.thread_id         = "entry";
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
    EXPECT_EQ(channel(sync_result, "failures")[0]["code"], json("NG.Timeout"));
    EXPECT_EQ(channel(sync_result, "failures")[0]["attempts"], json(2));
}

TEST(AttemptTimeout, ARunCancelledDuringAnAttemptIsCancelledNotTimedOut) {
    const auto name   = unique_probe("cancelled");
    auto       engine = build(single(name, json::array({json{{"wait", 5000}}}), timed(3000, 3), true));
    RunConfig  config;
    config.cancel_token = std::make_shared<CancelToken>();
    std::thread canceller([token = config.cancel_token] {
        std::this_thread::sleep_for(milliseconds(150));
        token->cancel();
    });
    const auto started = Clock::now();
    EXPECT_THROW((void)engine->run(config), CancelledException);
    canceller.join();
    EXPECT_LT(Clock::now() - started, milliseconds(1500));
    EXPECT_EQ(calls(name), 1) << "no retry and no handler after a cancellation";
}

// ---------------------------------------------------------------------------
// Crash and resume (SQLite)
// ---------------------------------------------------------------------------

namespace {

// Delegates to a SQLite store and "crashes" (throws) on the first save of a
// committed super-step at `crash_step`: the node's pending write is durable,
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

// START -> PREP -> N -> END (N in step 1).
json prep_then(const std::string& probe_name, json plan, json policy, bool handler) {
    auto definition = single(probe_name, std::move(plan), std::move(policy), handler);
    definition["nodes"]["PREP"] = node(probe_name + "-prep", json::array({json::object()}));
    definition["edges"]         = json::array({edge("__start__", "PREP"), edge("PREP", "N"), edge("N", "__end__")});
    if (handler) definition["edges"].push_back(edge("H", "__end__"));
    return definition;
}

}  // namespace

TEST(AttemptTimeout, CrashResumeReplaysTheOutcomeAfterTimeoutsWithoutRunningAgain) {
    const TempDir dir;
    const auto    name       = unique_probe("crash");
    // Two timeouts, then the third attempt completes; the process is lost
    // before the step commits.
    const auto    definition = prep_then(
        name, json::array({json{{"wait", 5000}}, json{{"wait", 5000}}, json::object()}), timed(100, 2), false);
    RunConfig run;
    run.thread_id = "crash";
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<CrashingStore>(
            std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite")), 1);
        auto engine = build(definition, std::move(config));
        EXPECT_THROW((void)engine->run(run), std::runtime_error);
        EXPECT_EQ(calls(name), 3);
    }
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite"));
        auto       engine  = build(definition, std::move(config));
        const auto resumed = engine->resume("crash");
        EXPECT_EQ(resumed.status(), RunStatus::Completed);
        EXPECT_EQ(calls(name), 3) << "the completed attempt replays from its pending write";
        EXPECT_EQ(channel(resumed, "trace"), json::array({"PREP:0", "N:2"}));
    }
}

TEST(AttemptTimeout, CrashResumeReplaysATimeoutRouteWithItsRecord) {
    const TempDir dir;
    const auto    name       = unique_probe("crash-route");
    const auto    definition = prep_then(name, json::array({json{{"wait", 5000}}}), timed(100, 1), true);
    RunConfig     run;
    run.thread_id = "crash-route";
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<CrashingStore>(
            std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite")), 1);
        auto engine = build(definition, std::move(config));
        EXPECT_THROW((void)engine->run(run), std::runtime_error);
        EXPECT_EQ(calls(name), 2);
    }
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite"));
        auto       engine  = build(definition, std::move(config));
        const auto resumed = engine->resume("crash-route");
        EXPECT_EQ(resumed.status(), RunStatus::Completed);
        EXPECT_EQ(calls(name), 3) << "N never runs again; H runs once";
        ASSERT_EQ(channel(resumed, "failures").size(), 1U);
        EXPECT_EQ(channel(resumed, "failures")[0]["code"], json("NG.Timeout"));
        EXPECT_EQ(channel(resumed, "failures")[0]["attempts"], json(2));
        EXPECT_EQ(resumed.execution_trace, (std::vector<std::string>{"N", "H"}));
    }
}

TEST(AttemptTimeout, AResumeContinuesTheAttemptThatFollowedATimeout) {
    // Attempt 0 times out; attempt 1 (namespace s1:N#a1) pauses with a
    // NodeInterrupt. A new engine on the same file resumes it in its own
    // namespace, with the timeout still applying, and the late completion of
    // attempt 0 never appears.
    register_types();
    const TempDir dir;
    const auto    name = unique_probe("resume");
    static std::atomic<int> next_type{0};
    const auto              type = "at.asking." + std::to_string(++next_type);
    struct Asking final : GraphNode {
        explicit Asking(std::string probe_name) : probe_name_(std::move(probe_name)) {}
        asio::awaitable<NodeOutput> run(NodeInput in) override {
            auto        seen = probe(probe_name_);
            std::size_t call = 0;
            {
                std::lock_guard lock(seen->mutex);
                call = seen->calls.size();
                Call record;
                record.node             = "N";
                record.effect_namespace = make_tool_execution_context(in.ctx).effect_task_id;
                record.started          = Clock::now();
                seen->calls.push_back(std::move(record));
            }
            if (call == 0) {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(milliseconds(5000));
                co_await timer.async_wait(
                    asio::bind_cancellation_slot(in.ctx.cancel_token->slot(), asio::use_awaitable));
            }
            if (!in.ctx.resume_value) throw NodeInterrupt("needs an answer");
            NodeOutput out;
            out.writes.push_back(ChannelWrite{"trace", json::array({"N:" + std::to_string(call)})});
            co_return out;
        }
        std::string get_name() const override { return "N"; }
        std::string probe_name_;
    };
    NodeFactory::instance().register_type(
        type,
        [name](const std::string&, const json&, const NodeContext&) { return std::make_unique<Asking>(name); },
        json{{"type", "object"}});
    auto definition        = prep_then(name, json::array({json::object()}), timed(150, 2), false);
    definition["nodes"]["N"] = json{{"type", type}};
    RunConfig run;
    run.thread_id = "resume";
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite"));
        const auto result = build(definition, std::move(config))->run(run);
        EXPECT_TRUE(result.interrupted);
        EXPECT_EQ(result.interrupt_node, "N");
    }
    {
        EngineConfig config;
        config.checkpoint_store = std::make_shared<SqliteCheckpointStore>(dir.file("checkpoints.sqlite"));
        const auto resumed = build(definition, std::move(config))->resume("resume", json("yes"));
        EXPECT_EQ(resumed.status(), RunStatus::Completed);
        EXPECT_EQ(channel(resumed, "trace"), json::array({"PREP:0", "N:2"}));
    }
    auto            seen = probe(name);
    std::lock_guard lock(seen->mutex);
    ASSERT_EQ(seen->calls.size(), 3U);
    EXPECT_EQ(seen->calls[0].effect_namespace, "s1:N");
    EXPECT_EQ(seen->calls[1].effect_namespace, "s1:N#a1");
    EXPECT_EQ(seen->calls[2].effect_namespace, "s1:N#a1") << "the resume continues attempt 1";
}
