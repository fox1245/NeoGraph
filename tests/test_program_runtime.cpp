#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/hook_runtime.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
#include <neograph/graph/sqlite_checkpoint.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#endif

#include "javascript.h"
#include "registry_access.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
neograph::json orchestration_document(neograph::json root, std::string node_type);
namespace {

using neograph::json;
using neograph::ChatTool;
using neograph::HookDefinition;
using neograph::HookDefinitionData;
using neograph::HookInputMapperKind;
using neograph::HookPhase;
using neograph::HookRegistry;
using neograph::HookRuntime;
using neograph::HookTargetContract;
using neograph::HookTargetResolver;
using neograph::InMemoryHookJournal;
using neograph::MandatoryHookRunner;
using neograph::NativeHookExecutionAdapter;
using neograph::Tool;
using neograph::ToolEffectClass;
using neograph::ToolExecutionController;
using namespace neograph::graph;
using namespace neograph::program;

std::atomic<unsigned>               completed_calls{0};
std::atomic<unsigned>               interrupt_calls{0};
std::atomic<unsigned>               blocking_calls{0};
std::atomic<unsigned>               blocking_active{0};
std::atomic<unsigned>               blocking_peak{0};
std::atomic<unsigned>               followup_calls{0};
std::atomic<unsigned>               stubborn_calls{0};
std::atomic<unsigned>               scheduler_blocking_calls{0};
std::atomic<unsigned>               map_fail_first_calls{0};
std::shared_ptr<std::barrier<>>     overlap_barrier;
std::shared_ptr<std::promise<void>> overlap_ready;
std::atomic<unsigned>               map_fail_first_active{0};
std::atomic<unsigned>               window_calls{0};
std::atomic<unsigned>               window_active{0};
std::atomic<unsigned>               window_peak{0};
std::atomic<unsigned>               window_completed{0};
std::atomic<unsigned>               window_second_started_before_completion{0};
std::shared_ptr<std::barrier<>>     window_first_barrier;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:test",
                              {},
                              {},
                              {}};
}

class CompletedNode final : public GraphNode {
public:
    explicit CompletedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++completed_calls;
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", "completed"});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class MapEchoNode final : public GraphNode {
public:
    explicit MapEchoNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++completed_calls;
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class InterruptNode final : public GraphNode {
public:
    explicit InterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt("approval required", json{{"request", "approve"}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("decision")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class AlwaysInterruptNode final : public GraphNode {
public:
    explicit AlwaysInterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++interrupt_calls;
        throw NodeInterrupt("second approval required", json{{"request", "approve-second"}});
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class EffectInterruptNode final : public GraphNode {
public:
    explicit EffectInterruptNode(std::string name, bool non_idempotent = false)
        : name_(std::move(name)), non_idempotent_(non_idempotent) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-effect-1"},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect",
                      json{{"effect_id", "effect-search-1"},
                           {"idempotency", non_idempotent_ ? "non_idempotent" : "supported"},
                           {"tool", "search"},
                           {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("result")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        non_idempotent_ = false;
};

class FailingNode final : public GraphNode {
public:
    explicit FailingNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        throw std::runtime_error("classified core failure");
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class ActiveCallGuard final {
public:
    explicit ActiveCallGuard(std::atomic<unsigned>& active) : active_(active) {}
    ~ActiveCallGuard() { active_.fetch_sub(1, std::memory_order_relaxed); }

private:
    std::atomic<unsigned>& active_;
};

class MapFailFirstNode final : public GraphNode {
public:
    explicit MapFailFirstNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto call = map_fail_first_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 1) {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            for (unsigned attempt = 0; map_fail_first_active.load(std::memory_order_relaxed) == 0;
                 ++attempt) {
                if (attempt == 2000)
                    throw std::runtime_error("map fail-fast sibling did not become active");
                timer.expires_after(std::chrono::milliseconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
            throw std::runtime_error("map fail-fast sentinel");
        }
        map_fail_first_active.fetch_add(1, std::memory_order_relaxed);
        ActiveCallGuard active_guard(map_fail_first_active);
        auto            timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class BlockingNode final : public GraphNode {
public:
    explicit BlockingNode(std::string               name,
                          std::chrono::milliseconds duration = std::chrono::seconds(5))
        : name_(std::move(name)), duration_(duration) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++blocking_calls;
        const auto active = blocking_active.fetch_add(1, std::memory_order_relaxed) + 1;
        auto       peak   = blocking_peak.load(std::memory_order_relaxed);
        while (peak < active &&
               !blocking_peak.compare_exchange_weak(peak, active, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {}
        ActiveCallGuard active_guard(blocking_active);
        auto            timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(duration_);
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string               name_;
    std::chrono::milliseconds duration_;
};
class BarrierNode final : public GraphNode {
public:
    explicit BarrierNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++blocking_calls;
        const auto active = blocking_active.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = blocking_peak.load(std::memory_order_relaxed);
        while (peak < active &&
               !blocking_peak.compare_exchange_weak(peak, active, std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {}
        if (active == 2 && overlap_ready) overlap_ready->set_value();
        ActiveCallGuard active_guard(blocking_active);
        if (overlap_barrier) overlap_barrier->arrive_and_wait();
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::seconds(5));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class WindowedNode final : public GraphNode {
public:
    explicit WindowedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto call   = window_calls.fetch_add(1) + 1;
        const auto active = window_active.fetch_add(1) + 1;
        auto       peak   = window_peak.load();
        while (peak < active && !window_peak.compare_exchange_weak(peak, active)) {}
        ActiveCallGuard active_guard(window_active);
        if (call <= 2 && window_first_barrier) window_first_barrier->arrive_and_wait();
        if (call == 3 && window_completed.load() == 0) ++window_second_started_before_completion;

        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        if (call <= 2) ++window_completed;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class StubbornNode final : public GraphNode {
public:
    explicit StubbornNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++stubborn_calls;
        // Simulate a late sibling/provider budget signal. The enclosing
        // runtime must preserve an earlier user/timeout cancellation cause.
        if (input.ctx.budget_exhausted) {
            input.ctx.budget_exhausted->store(true, std::memory_order_release);
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await  timer.async_wait(asio::use_awaitable);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class SchedulerBlockingNode final : public GraphNode {
public:
    explicit SchedulerBlockingNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++scheduler_blocking_calls;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class FollowupNode final : public GraphNode {
public:
    explicit FollowupNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++followup_calls;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot runtime_registry(
    ExecutionGuarantee completed_guarantee = ExecutionGuarantee::Strict) {
    RegistrySnapshotBuilder builder;
    auto                    completed = manifest(ExecutableKind::Node, "runtime-completed", '1');
    completed.execution_guarantee     = completed_guarantee;
    builder.add_node(
        std::move(completed),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CompletedNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-map-echo", 'e'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MapEchoNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<InterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-always-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<AlwaysInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-interrupt", 'a'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-nonidempotent", 'c'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name, true);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-failing", '4'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-map-fail-first", 'd'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<MapFailFirstNode>(name);
        },
        json{{"type", "object"}}, json::object());

    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-blocking", '5'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-barrier", 'f'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BarrierNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-windowed", '0'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WindowedNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-short-blocking", 'b'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name, std::chrono::milliseconds(500));
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-stubborn", '7'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StubbornNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-scheduler-blocking", '8'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SchedulerBlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-followup", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FollowupNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-alternate", '9'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

json budget_requirements() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}
json                      program_document(std::string node_type);
TaskGraphTemplateContract expansion_template_contract() {
    TaskGraphTemplateContract contract;
    contract.template_id      = "runtime-map/v1";
    contract.content_identity = digest('a');
    contract.input_fields     = {"/item"};
    contract.output_artifacts = {TaskGraphArtifactContract{"result", {"/channels/value/value"}}};
    contract.budget_ceiling   = TaskGraphBudget{1000, 100, 100, 4096};
    contract.child_binding    = "child";
    contract.kind             = "program";
    return contract;
}

TaskGraphExpansionPolicy expansion_policy() {
    TaskGraphExpansionPolicy policy;
    policy.limits.max_tasks               = 4;
    policy.limits.max_edges               = 8;
    policy.limits.max_depth               = 4;
    policy.limits.per_task_budget_ceiling = TaskGraphBudget{1000, 100, 100, 4096};
    policy.limits.total_budget_ceiling    = TaskGraphBudget{4000, 400, 400, 8192};
    policy.template_allowlist             = {expansion_template_contract()};
    return policy;
}

json expansion_proposal() {
    return json{
        {"schema_version", 1},
        {"tasks", json::array({
                      json{{"id", "dependent"},
                           {"template", "runtime-map/v1"},
                           {"input_bindings",
                            json::array({json{{"from", json{{"task", "seed"},
                                                            {"artifact", "result"},
                                                            {"field", "/channels/value/value"}}},
                                              {"to", json{{"field", "/item"}}}}})},
                           {"depends_on", json::array({"seed"})},
                           {"budget", json{{"wall_time_ms", 100}, {"model_tokens", 1}}}},
                      json{{"id", "seed"},
                           {"template", "runtime-map/v1"},
                           {"input_bindings", json::array()},
                           {"depends_on", json::array()},
                           {"budget", json{{"wall_time_ms", 100}, {"model_tokens", 1}}}},
                  })},
        {"join", json{{"kind", "all"}}},
    };
}

json expand_task_graph_document(json          proposal,
                                std::uint64_t max_tasks       = 2,
                                std::uint64_t max_edges       = 4,
                                std::uint64_t max_depth       = 2,
                                std::uint64_t max_concurrency = 1) {
    auto document                                          = program_document("runtime-completed");
    document["program_schema_version"]                     = PROGRAM_SCHEMA_VERSION_V4;
    document["declared_budget_requirements"][3]["maximum"] = max_concurrency;
    document["declared_budget_requirements"][4]["maximum"] = max_tasks;
    document["declared_budget_requirements"][6]["minimum"] = 1;
    document["declared_budget_requirements"][6]["maximum"] = 1;
    document["declared_budget_requirements"][7]["minimum"] = max_depth;
    document["declared_budget_requirements"][7]["maximum"] = max_depth;
    document["declared_budget_requirements"][8]["minimum"] = max_tasks;
    document["declared_budget_requirements"][8]["maximum"] = max_tasks;
    const auto definition                                  = document["root"]["definition"];
    document["root"]                                       = json{{"op", "expand_task_graph"},
                                                                  {"name", "main"},
                                                                  {"definition", definition},
                                                                  {"proposal_source", json{{"inline", std::move(proposal)}}},
                                                                  {"max_tasks", max_tasks},
                                                                  {"max_edges", max_edges},
                                                                  {"max_depth", max_depth},
                                                                  {"max_dynamic_compiles", 1},
                                                                  {"max_total_children", max_tasks},
                                                                  {"max_concurrency", max_concurrency},
                                                                  {"failure_policy", "collect"}};
    return document;
}

json program_document(std::string node_type) {
    json nodes{{"work", json{{"type", node_type}}}};
    json edges = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "__end__"}},
    });
    if (node_type == "runtime-blocking" || node_type == "runtime-short-blocking" ||
        node_type == "runtime-scheduler-blocking") {
        nodes["followup"] = json{{"type", "runtime-followup"}};
        edges             = json::array({
            json{{"from", "__start__"}, {"to", "work"}},
            json{{"from", "work"}, {"to", "followup"}},
            json{{"from", "followup"}, {"to", "__end__"}},
        });
    }
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", std::move(nodes)},
        {"edges", std::move(edges)},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

json two_interrupt_program_document() {
    auto document                  = program_document("runtime-interrupt");
    auto definition                = document["root"]["definition"];
    definition["nodes"]["second"]  = json{{"type", "runtime-always-interrupt"}};
    definition["edges"]            = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "second"}},
        json{{"from", "second"}, {"to", "__end__"}},
    });
    document["root"]["definition"] = std::move(definition);
    return document;
}

json parallel_map_child_document(std::string node_type) {
    auto document                                          = program_document(std::move(node_type));
    document["declared_budget_requirements"][0]["maximum"] = 3333;
    document["declared_budget_requirements"][1]["maximum"] = 333;
    document["declared_budget_requirements"][2]["maximum"] = 333;
    document["declared_budget_requirements"][5]["maximum"] = 6;
    return document;
}
json parallel_map_echo_child_document() {
    auto document = parallel_map_child_document("runtime-map-echo");
    document["root"]["definition"]["channels"]["item"] =
        json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}};
    return document;
}
json failing_fanout_program_document(std::string completed_name = "completed",
                                     std::string failing_name   = "failing") {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{completed_name, json{{"type", "runtime-completed"}}},
                       {failing_name, json{{"type", "runtime-failing"}}}}},
        {"edges", json::array({
                      json{{"from", "__start__"}, {"to", completed_name}},
                      json{{"from", "__start__"}, {"to", failing_name}},
                      json{{"from", completed_name}, {"to", "__end__"}},
                      json{{"from", failing_name}, {"to", "__end__"}},
                  })},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

json step_limited_program_document() {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"work", json{{"type", "runtime-completed"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                               json{{"from", "work"}, {"to", "work"}}})},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

struct ChildBindingRegistry {
    using Key = std::pair<std::string, std::string>;

    void bind(std::string                parent_version_id,
              std::string                binding_name,
              ProgramRuntimeChildBinding binding) {
        std::lock_guard lock(mutex);
        bindings.insert_or_assign(Key{std::move(parent_version_id), std::move(binding_name)},
                                  std::move(binding));
    }

    std::optional<ProgramRuntimeChildBinding> resolve(std::string_view owner_scope,
                                                      std::string_view parent_version_id,
                                                      std::string_view binding_name) const {
        if (owner_scope != "tenant:runtime") return std::nullopt;
        std::lock_guard lock(mutex);
        const auto      found =
            bindings.find(Key{std::string(parent_version_id), std::string(binding_name)});
        return found == bindings.end() ? std::nullopt
                                       : std::optional<ProgramRuntimeChildBinding>{found->second};
    }

    void clear() {
        std::lock_guard lock(mutex);
        bindings.clear();
    }

private:
    mutable std::mutex                        mutex;
    std::map<Key, ProgramRuntimeChildBinding> bindings;
};

class LifecycleHookTool final : public Tool {
public:
    explicit LifecycleHookTool(std::string value, bool block = false)
        : name(std::move(value)), block_terminal(block) {}
    ChatTool get_definition() const override { return {name, "", json::object()}; }
    std::string get_name() const override { return name; }
    std::string execute(const json&) override {
        ++calls;
        if (block_terminal) throw std::runtime_error("terminal hook blocked");
        return "ok";
    }
    std::string name;
    std::atomic<unsigned> calls{0};
    bool block_terminal = false;
};

std::shared_ptr<HookRuntime> lifecycle_hooks(LifecycleHookTool& admitted,
                                             LifecycleHookTool& before_terminal,
                                             LifecycleHookTool& failed) {
    HookTargetResolver resolver = [&admitted, &before_terminal, &failed](std::string_view target)
        -> std::optional<HookTargetContract> {
        LifecycleHookTool* tool = target == admitted.name ? &admitted
                                : target == before_terminal.name ? &before_terminal
                                : target == failed.name ? &failed : nullptr;
        return tool ? std::optional<HookTargetContract>{
                          HookTargetContract{tool, {}, ToolEffectClass::ReadOnly}}
                    : std::nullopt;
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    for (const auto& [phase, target] : std::vector<std::pair<HookPhase, std::string>>{
             {HookPhase::MessageAdmitted, admitted.name},
             {HookPhase::BeforeTerminalPublication, before_terminal.name},
             {HookPhase::RunFailed, failed.name}}) {
        HookDefinitionData definition;
        definition.phase = phase;
        definition.target_id = target;
        definition.effect = ToolEffectClass::ReadOnly;
        definition.input_mapper = {HookInputMapperKind::Template, {}, json::object(), {}};
        registry->admit(HookDefinition::create(std::move(definition)));
    }
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(resolver,
        std::make_shared<ToolExecutionController>());
    return std::make_shared<HookRuntime>(std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry), std::move(adapter)));
}

struct AdmittedRuntime {
    RegistrySnapshot                                registry;
    AdmissionProfile                                profile;
    PolicySnapshot                                  policy;
    std::shared_ptr<InMemoryProgramStore>           store;
    std::shared_ptr<EngineGenerationCache>          engines;
    std::shared_ptr<ProgramCatalog>                 catalog;
    std::shared_ptr<CheckpointStore>                checkpoints;
    std::shared_ptr<ProgramTransitionStore>         journal;
    std::shared_ptr<ChildBindingRegistry>           child_bindings;
    std::shared_ptr<InMemoryTaskGraphFragmentStore> task_graph_fragments;
    ProgramChildQuotaConfig                         child_quota;
    std::size_t                                     scheduler_thread_count;
    std::shared_ptr<HookRuntime>                    hook_runtime;
    std::unique_ptr<ProgramRuntime>                 runtime;

    explicit AdmittedRuntime(std::size_t                             scheduler_threads  = 1,
                             std::shared_ptr<CheckpointStore>        checkpoint_backend = {},
                             std::shared_ptr<ProgramTransitionStore> journal_backend    = {},
                              ProgramChildQuotaConfig                 quota              = {},
                              ExecutionGuarantee minimum_guarantee = ExecutionGuarantee::Strict,
                              bool               allow_javascript  = false,
                              std::shared_ptr<HookRuntime> hooks = {})
        : registry(runtime_registry(minimum_guarantee)),
          profile(make_profile(registry, minimum_guarantee, allow_javascript)),
          policy(make_policy(profile, minimum_guarantee, allow_javascript)),
          store(std::make_shared<InMemoryProgramStore>()),
          engines(std::make_shared<EngineGenerationCache>()),
          catalog(std::make_shared<ProgramCatalog>(
              CatalogConfig{store, registry, engines, "program-runtime-test/v1"})),
          checkpoints(checkpoint_backend ? std::move(checkpoint_backend)
                                         : std::make_shared<InMemoryCheckpointStore>()),
          journal(journal_backend ? std::move(journal_backend)
                                  : std::make_shared<InMemoryProgramTransitionStore>()),
          child_bindings(std::make_shared<ChildBindingRegistry>()),
          task_graph_fragments(std::make_shared<InMemoryTaskGraphFragmentStore>()),
          child_quota(quota),
           scheduler_thread_count(scheduler_threads),
           hook_runtime(std::move(hooks)),
           runtime(make_runtime()) {}

    static AdmissionProfile make_profile(
        const RegistrySnapshot& registry,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        AdmissionProfileBuilder builder;
        builder.id("runtime-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(PROGRAM_SCHEMA_VERSION_V4)
            .minimum_execution_guarantee(minimum_guarantee)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        if (allow_javascript) builder.allow_source_kind(SourceKind::JavaScript);
        for (const auto& identity : registry.identities())
            builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(
        const AdmissionProfile& profile,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        const auto            budget_ceiling = allow_javascript
                                                   ? BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32}
                                                   : BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32};
        PolicySnapshotBuilder builder;
        builder.id("runtime-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:runtime")
            .admission_profile(profile)
            .budget_ceiling(budget_ceiling)
            .minimum_execution_guarantee(minimum_guarantee);
        return std::move(builder).build();
    }
    std::unique_ptr<ProgramRuntime> make_runtime_with_transitions(
        std::shared_ptr<ProgramTransitionStore> transitions,
        ProgramRuntimeRecoveryHandler recovery_handler = {}) const {
        RuntimeConfig config{catalog,
                              checkpoints,
                              {},
                              std::move(transitions),
                             scheduler_thread_count,
                             [bindings = child_bindings](std::string_view owner_scope,
                                                         std::string_view parent_version_id,
                                                         std::string_view binding_name) {
                                 return bindings->resolve(owner_scope, parent_version_id,
                                                          binding_name);
                             }};
        config.task_graph_fragments = task_graph_fragments;
        config.task_graph_policy_resolver =
            [](std::string_view owner_scope, std::string_view,
               std::string_view) -> std::optional<TaskGraphExpansionPolicy> {
            if (owner_scope != "tenant:runtime") return std::nullopt;
            return expansion_policy();
        };
        config.child_quota = child_quota;
        config.hook_runtime = hook_runtime;
        config.runtime_recovery_handler = std::move(recovery_handler);
        return std::make_unique<ProgramRuntime>(std::move(config));
    }
    std::unique_ptr<ProgramRuntime> make_runtime() const {
        return make_runtime_with_transitions(journal);
    }

    ProgramVersion admit(std::string node_type) {
        return admit_document(program_document(std::move(node_type)));
    }

    ProgramVersion admit_document(json document) {
        const auto schema_version = document.contains("program_schema_version") &&
                                            document["program_schema_version"].is_number_unsigned()
                                        ? document["program_schema_version"].get<std::uint32_t>()
                                        : PROGRAM_SCHEMA_VERSION_V1;
        return admit_source(
            ProgramSource::from_cpp_builder("test:runtime", schema_version, std::move(document)));
    }

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
    ProgramVersion admit_javascript(std::string                   source,
                                    std::optional<ContractRecord> output_contract = std::nullopt) {
        return admit_source(
            ProgramSource::from_javascript("test:runtime-control.js", std::move(source)),
            std::move(output_contract));
    }
#endif

    ProgramVersion admit_source(ProgramSource                 source,
                                std::optional<ContractRecord> output_contract = std::nullopt) {
        ProgramCompiler              compiler(registry, {"program-runtime-test/v1"});
        std::optional<ProgramBundle> bundle;
        try {
            if (output_contract) {
                bundle = compiler.compile(source, RunBudget{10000, 1000, 1000, 1, 32, 20, 0, 0, 0},
                                          ContractRecord{}, *output_contract);
            } else {
                bundle = compiler.compile(source);
            }
        } catch (const ProgramCompileError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
        try {
            return catalog->admit(*bundle, ProgramAdmission{"tenant:runtime", profile, policy, {}});
        } catch (const ProgramAdmissionError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
    }

    void bind_child(const ProgramVersion&      parent,
                    std::string                binding_name,
                    ProgramRuntimeChildBinding binding) {
        child_bindings->bind(parent.id(), std::move(binding_name), std::move(binding));
    }

    void clear_child_bindings() { child_bindings->clear(); }

    void recreate_catalog_and_runtime() {
        runtime.reset();
        catalog.reset();
        engines = std::make_shared<EngineGenerationCache>();
        catalog = std::make_shared<ProgramCatalog>(
            CatalogConfig{store, registry, engines, "program-runtime-test/v1"});
        runtime = make_runtime();
    }
};

class DurableInstructionContextStore final : public neograph::DurableContextStore {
public:
    neograph::ContextStoreAppendResult append_history(
        const neograph::ContextStoreFeed& feed,
        const neograph::RuntimeHistoryRecord& record,
        const std::optional<std::string>& expected) override {
        return inner.append_history(feed, record, expected);
    }
    neograph::ContextStoreHead history_head(
        const neograph::ContextStoreFeed& feed) const override {
        return inner.history_head(feed);
    }
    neograph::ContextHistoryRange snapshot_history(
        const neograph::ContextStoreFeed& feed, std::uint64_t from,
        std::uint64_t through) const override {
        return inner.snapshot_history(feed, from, through);
    }
    std::string hydrate_history(
        const neograph::ContextHistoryRange& range) const override {
        return inner.hydrate_history(range);
    }
    neograph::ContextArtifactPutResult put_artifact(
        std::string_view owner,
        const neograph::ContextArtifact& artifact) override {
        return inner.put_artifact(owner, artifact);
    }
    std::optional<neograph::ContextArtifact> get_artifact(
        std::string_view owner, std::string_view id) const override {
        return inner.get_artifact(owner, id);
    }
private:
    neograph::InMemoryContextStore inner;
};

struct LinkedChildAdmission {
    ProgramVersion    parent_version;
    ProgramVersion    child_version;
    ModuleLinkReceipt receipt;
};

LinkedChildAdmission link_child_versions(
    AdmittedRuntime&   fixture,
    ProgramVersion     parent_version,
    ProgramVersion     child_version,
    BudgetLimits       child_budget       = BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
    ExecutionGuarantee accepted_guarantee = ExecutionGuarantee::Strict) {
    const auto child_bundle = fixture.store->get_bundle(child_version.bundle_id());
    if (!child_bundle) throw std::runtime_error("child bundle was not admitted");

    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:runtime";
    module_data.coordinate     = ModuleCoordinate{"runtime", "parent", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    module_data.children.push_back(
        ChildProgramDescriptor{"child",
                               child_version.id(),
                               {ModulePort{"input", child_bundle->input_contract()}},
                               {ModulePort{"output", child_bundle->output_contract()}},
                               child_bundle->capability_effect_closure().capabilities,
                               child_bundle->capability_effect_closure().effects,
                               std::move(child_budget),
                               accepted_guarantee});
    module_data.allowed_capabilities = child_bundle->capability_effect_closure().capabilities;
    module_data.declared_effects     = child_bundle->capability_effect_closure().effects;
    const auto       parent_module   = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = parent_module.coordinate();
    resolution.modules.push_back(parent_module);
    auto receipt =
        link_module_child(resolution, parent_module, "child", *child_bundle, child_version);
    fixture.bind_child(parent_version, "child", ProgramRuntimeChildBinding{receipt, child_version});
    return LinkedChildAdmission{std::move(parent_version), std::move(child_version),
                                std::move(receipt)};
}

LinkedChildAdmission make_linked_child(AdmittedRuntime& fixture,
                                       std::string      parent_node_type = "runtime-blocking",
                                       std::string      child_node_type  = "runtime-blocking") {
    auto parent_document                      = program_document(std::move(parent_node_type));
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)));
}
LinkedChildAdmission make_recursive_linked_child(AdmittedRuntime& fixture) {
    auto parent_document                      = program_document("runtime-blocking");
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 4;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 4;

    auto child_document = program_document("runtime-blocking");
    child_document["declared_budget_requirements"][7]["minimum"] = 0;
    child_document["declared_budget_requirements"][7]["maximum"] = 4;
    child_document["declared_budget_requirements"][8]["minimum"] = 0;
    child_document["declared_budget_requirements"][8]["maximum"] = 4;
    auto linked = link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                                      fixture.admit_document(std::move(child_document)),
                                      BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 4, 4});
    fixture.bind_child(linked.child_version, "child",
                       ProgramRuntimeChildBinding{linked.receipt, linked.child_version});
    return linked;
}

LinkedChildAdmission make_durable_spawn_child(
    AdmittedRuntime&             fixture,
    std::string                  parent_node_type   = "runtime-completed",
    std::string                  child_node_type    = "runtime-completed",
    std::optional<std::uint64_t> timeout_ms         = std::nullopt,
    ExecutionGuarantee           accepted_guarantee = ExecutionGuarantee::Strict) {
    json await = json{{"op", "await"}, {"body", json{{"op", "spawn"}, {"child_binding", "child"}}}};
    if (timeout_ms) await["timeout_ms"] = *timeout_ms;
    auto parent_document = orchestration_document(std::move(await), std::move(parent_node_type));
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)),
                               BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                               accepted_guarantee);
}

json parallel_map_document(json          item_source,
                           std::uint64_t max_items,
                           std::string   failure_policy   = "fail_fast",
                           std::uint64_t max_output_bytes = 65536) {
    auto document                      = program_document("runtime-completed");
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V3;
    const auto definition              = document["root"]["definition"];
    document["root"]                   = json{
                          {"op", "parallel_map"},
                          {"name", "main"},
                          {"definition", definition},
                          {"item_source", std::move(item_source)},
                          {"child_binding", "child"},
                          {"input_binding", json{{"from", json{{"field", ""}}}, {"to", json{{"field", "/item"}}}}},
                          {"output_binding", json{{"from", json{{"field", "/channels/value/value"}}}}},
                          {"max_items", max_items},
                          {"max_in_flight", std::min<std::uint64_t>(2, max_items)},
                          {"max_output_bytes", max_output_bytes},
                          {"failure_policy", std::move(failure_policy)}};
    document["declared_budget_requirements"][3]["minimum"] = std::min<std::uint64_t>(2, max_items);
    document["declared_budget_requirements"][3]["maximum"] = std::min<std::uint64_t>(2, max_items);
    document["declared_budget_requirements"][7]["minimum"] = 1;
    document["declared_budget_requirements"][7]["maximum"] = 1;
    document["declared_budget_requirements"][8]["minimum"] = max_items;
    document["declared_budget_requirements"][8]["maximum"] = max_items;
    return document;
}

RunBudget grant() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};
}
std::string pending_call_id(const ProgramResult& result) {
    const auto interrupt = result.interrupt();
    if (!interrupt) throw std::invalid_argument("Program result is not interrupted");
    if (interrupt->pending_input) return interrupt->pending_input->call_id();
    if (interrupt->pending_effect) return interrupt->pending_effect->call_id();
    throw std::invalid_argument("Interrupted Program result has no pending call");
}

ProgramResume resume_for(const ProgramResult&              result,
                         json                              value,
                         std::string                       trace_id,
                         std::shared_ptr<ProgramEventSink> events = {}) {
    return ProgramResume{std::move(value), std::move(trace_id), std::move(events),
                         pending_call_id(result)};
}

class ThrowingSink final : public ProgramEventSink {
public:
    explicit ThrowingSink(ProgramEventKind fail_on) : fail_on_(fail_on) {}

    void on_event(const ProgramEvent& event) override {
        seen.push_back(event.kind);
        if (event.kind == fail_on_) {
            throw std::runtime_error("sink rejected event");
        }
    }

    std::vector<ProgramEventKind> seen;

private:
    ProgramEventKind fail_on_;
};
class CountingSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent&) override { calls.fetch_add(1); }

    std::atomic<unsigned> calls{0};
};

class CallbackSink final : public ProgramEventSink {
public:
    void set_callback(std::function<void(const ProgramEvent&)> callback) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(callback);
    }

    void on_event(const ProgramEvent& event) override {
        std::function<void(const ProgramEvent&)> callback;
        {
            std::lock_guard lock(mutex_);
            callback = callback_;
        }
        if (callback) callback(event);
    }

private:
    std::mutex                               mutex_;
    std::function<void(const ProgramEvent&)> callback_;
};

class JournalObservingSink final : public ProgramEventSink {
public:
    explicit JournalObservingSink(std::shared_ptr<ProgramTransitionStore> journal)
        : journal_(std::move(journal)) {}

    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::CheckpointPublished &&
            event.kind != ProgramEventKind::Terminal) {
            return;
        }
        const auto latest    = journal_->latest("tenant:runtime", event.run_id);
        const bool committed = latest &&
                               latest->continuation.state == ContinuationState::Completed &&
                               latest->core_checkpoint.has_value();
        if (event.kind == ProgramEventKind::CheckpointPublished) {
            checkpoint_observed_committed.store(committed);
        } else {
            terminal_observed_committed.store(committed);
        }
    }

    std::atomic<bool> checkpoint_observed_committed{false};
    std::atomic<bool> terminal_observed_committed{false};

private:
    std::shared_ptr<ProgramTransitionStore> journal_;
};

class ConflictOnceJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation(owner, lineage_id, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view owner,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return inner_.load_graph_migration_capsule(
            owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        if (publication.journal_record.continuation.state != ContinuationState::Running &&
            !injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return inner_.compare_publish(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        if (publication.journal_record.continuation.state != ContinuationState::Running &&
            !injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return inner_.compare_publish_execution(
            owner, expected, std::move(publication), expected_lease, next_lease);
    }
    bool injected() const noexcept { return injected_.load(); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
};

class IsolatedProcessTransitionStore final : public ProgramTransitionStore {
public:
    explicit IsolatedProcessTransitionStore(
        std::shared_ptr<ProgramTransitionStore> target)
        : target_(std::move(target)) {}

    std::optional<ProgramRunRecord> load(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(
        std::string_view owner, std::string_view run_id) const override {
        return target_->latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return target_->load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        if (recovery_run_id_ == run_id) {
            std::vector<ProgramContextPublication> result;
            for (const auto& context : recovery_contexts_) {
                if (context.epoch.sequence() > sequence) result.push_back(context);
            }
            return result;
        }
        return target_->load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner, std::string_view run_id) const override {
        if (recovery_run_id_ == run_id) return recovery_hooks_;
        return target_->load_hook_outbox_entries(owner, run_id);
    }
    std::optional<MigrationPlan> load_migration_plan(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_migration_plan(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage(
        std::string_view owner, std::string_view lineage_id) const override {
        return target_->load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return target_->load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return target_->load_generation(owner, lineage_id, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return target_->load_generation_initial_publication(
            owner, lineage_id, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view owner, std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return target_->load_graph_migration_capsule(
            owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner, std::string_view run_id) const override {
        return target_->load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication) override {
        return target_->compare_publish(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        const bool terminal_release = expected_lease && !next_lease &&
            publication.run_record.continuation().state != ContinuationState::Running;
        if (terminal_release) {
            std::unique_lock lock(execution_release_mutex_);
            if (block_execution_release_) {
                execution_release_observed_ = true;
                execution_release_condition_.notify_all();
                execution_release_condition_.wait_for(
                    lock, std::chrono::seconds(5),
                    [this] { return execution_release_released_; });
            }
        }
        return target_->compare_publish_execution(
            owner, expected, std::move(publication), expected_lease, next_lease);
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override {
        return target_->compare_publish_graph_safe_point(
            owner, expected, std::move(publication), evidence, capsule,
            execution_lease);
    }
    void block_execution_release() {
        std::lock_guard lock(execution_release_mutex_);
        block_execution_release_ = true;
        execution_release_observed_ = false;
        execution_release_released_ = false;
    }
    bool wait_for_execution_release(std::chrono::milliseconds timeout) {
        std::unique_lock lock(execution_release_mutex_);
        return execution_release_condition_.wait_for(
            lock, timeout, [this] { return execution_release_observed_; });
    }
    void release_execution_release() {
        std::lock_guard lock(execution_release_mutex_);
        execution_release_released_ = true;
        execution_release_condition_.notify_all();
    }
    void set_runtime_recovery_state(
        std::string run_id, std::vector<ProgramContextPublication> contexts,
        std::vector<neograph::HookOutboxEntry> hooks) {
        recovery_run_id_ = std::move(run_id);
        recovery_contexts_ = std::move(contexts);
        recovery_hooks_ = std::move(hooks);
    }

private:
    std::shared_ptr<ProgramTransitionStore> target_;
    std::mutex execution_release_mutex_;
    std::condition_variable execution_release_condition_;
    bool block_execution_release_ = false;
    bool execution_release_observed_ = false;
    bool execution_release_released_ = false;
    std::string recovery_run_id_;
    std::vector<ProgramContextPublication> recovery_contexts_;
    std::vector<neograph::HookOutboxEntry> recovery_hooks_;
};

ProgramContextPublication recovery_context(std::string run_id) {
    neograph::ContextEpochData epoch_data;
    epoch_data.run_id = std::move(run_id);
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = digest('9');
    epoch_data.guarantee_profile = neograph::RuntimeGuaranteeProfile::Recorded;
    auto epoch = neograph::ContextEpoch::create(std::move(epoch_data));
    neograph::ContextAssemblyReceiptData receipt_data;
    receipt_data.context_epoch_id = epoch.id();
    receipt_data.normalized_request_digest = digest('a');
    receipt_data.message_window_digest = digest('b');
    auto receipt = neograph::ContextAssemblyReceipt::create(
        std::move(receipt_data), epoch, {});
    return {std::move(epoch), {}, std::move(receipt)};
}

neograph::HookOutboxEntry recovery_hook(std::string run_id) {
    const auto event = neograph::RuntimeEvent::create(
        {{}, 1, HookPhase::BeforeToolExecution, "tool_execution",
         "tenant:runtime", std::move(run_id), json::object()});
    const auto invocation = neograph::HookInvocation::create(
        {{}, digest('f'), event.id(), "audit", HookPhase::BeforeToolExecution,
         neograph::HookDelivery::BlockingMandatory, neograph::HookFailureMode::FailClosed,
         neograph::HookIdempotency::Idempotent, ToolEffectClass::ReadOnly,
         {}, {}, json::object()});
    return neograph::HookOutboxEntry::create(
        {invocation, event, neograph::HookExecutionState::Pending, 0, 1,
         std::chrono::system_clock::now() + std::chrono::minutes(1)});
}

class BlockAfterJavaScriptResultJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        if (throw_next_load_.exchange(false)) {
            throw std::runtime_error("simulated Program run read failure");
        }
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner,
                                                       std::string_view run_id) const override {
        if (consume_replacement_readback_failure()) {
            throw std::runtime_error("simulated replacement lineage readback failure");
        }
        return inner_.load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        if (consume_replacement_readback_failure()) {
            throw std::runtime_error("simulated replacement generation readback failure");
        }
        return inner_.load_generation(owner, lineage_id, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view owner,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        return inner_.load_graph_migration_capsule(
            owner, source_run_id, source_lineage_head_id);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const bool command_result =
            !publication.commands.empty() && publication.commands.back().completed();
        const bool replacement =
            publication.run_generation && publication.run_generation->replacement_receipt();
        if (replacement && throw_before_replacement_.exchange(false)) {
            std::function<void()> publish_collision;
            {
                std::lock_guard lock(mutex_);
                publish_collision = std::move(publish_collision_);
            }
            if (publish_collision) publish_collision();
            throw std::runtime_error("simulated replacement failure before publication");
        }
        const auto published = inner_.compare_publish(owner, expected, std::move(publication));
        if (replacement && published == ProgramTransitionPublishResult::Published &&
            crash_after_replacement_.exchange(false)) {
            if (unreadable_replacement_commit_.exchange(false)) {
                replacement_readback_failures_.store(2);
            }
            throw std::runtime_error("simulated crash after replacement publication");
        }
        if (!command_result || published != ProgramTransitionPublishResult::Published) {
            return published;
        }
        std::unique_lock lock(mutex_);
        observed_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
        return published;
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        const bool replacement =
            publication.run_generation && publication.run_generation->replacement_receipt();
        if (replacement && throw_before_replacement_.exchange(false)) {
            throw std::runtime_error("simulated replacement failure before publication");
        }
        const auto published = inner_.compare_publish_execution(
            owner, expected, std::move(publication), expected_lease, next_lease);
        if (replacement && published == ProgramTransitionPublishResult::Published &&
            crash_after_replacement_.exchange(false)) {
            if (unreadable_replacement_commit_.exchange(false)) {
                replacement_readback_failures_.store(2);
            }
            throw std::runtime_error("simulated crash after replacement publication");
        }
        return published;
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner,
        std::string_view expected,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override {
        const auto published = inner_.compare_publish_graph_safe_point(
            owner, expected, std::move(publication), evidence, capsule,
            execution_lease);
        if (published == ProgramTransitionPublishResult::Published &&
            crash_after_graph_safe_point_.exchange(false)) {
            if (unreadable_graph_safe_point_commit_.exchange(false)) {
                throw_next_load_.store(true);
            }
            throw std::runtime_error("simulated crash after graph safe-point publication");
        }
        return published;
    }
    bool wait_for_result(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return observed_; });
    }
    void release_result() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }
    void crash_after_next_replacement() { crash_after_replacement_.store(true); }
    void crash_after_next_replacement_with_unreadable_commit() {
        unreadable_replacement_commit_.store(true);
        crash_after_replacement_.store(true);
    }
    void crash_after_next_graph_safe_point_with_unreadable_commit() {
        unreadable_graph_safe_point_commit_.store(true);
        crash_after_graph_safe_point_.store(true);
    }
    void throw_on_next_load() { throw_next_load_.store(true); }
    void throw_before_next_replacement_with_occupied_target(
        std::function<void()> publish_collision) {
        {
            std::lock_guard lock(mutex_);
            publish_collision_ = std::move(publish_collision);
        }
        throw_before_replacement_.store(true);
    }

private:
    bool consume_replacement_readback_failure() const noexcept {
        auto remaining = replacement_readback_failures_.load();
        while (remaining > 0) {
            if (replacement_readback_failures_.compare_exchange_weak(remaining, remaining - 1)) {
                return true;
            }
        }
        return false;
    }

    InMemoryProgramTransitionStore inner_;
    mutable std::mutex             mutex_;
    std::condition_variable        condition_;
    bool                           observed_ = false;
    bool                           released_ = false;
    std::atomic<bool>              crash_after_replacement_{false};
    std::atomic<bool>              unreadable_replacement_commit_{false};
    std::atomic<bool>              crash_after_graph_safe_point_{false};
    std::atomic<bool>              unreadable_graph_safe_point_commit_{false};
    mutable std::atomic<int>       replacement_readback_failures_{0};
    std::atomic<bool>              throw_before_replacement_{false};
    mutable std::atomic<bool>      throw_next_load_{false};
    std::function<void()>          publish_collision_;
};

class FailChildDispatchOnceJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation(owner, lineage_id, generation);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const auto children = publication.run_record.children();
        if (children.empty() || children.back().state != ProgramChildState::Dispatched ||
            !block_dispatch_.load()) {
            return inner_.compare_publish(owner, expected, std::move(publication));
        }
        if (!injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return ProgramTransitionPublishResult::Conflict;
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        return inner_.compare_publish_execution(
            owner, expected, std::move(publication), std::move(expected_lease),
            std::move(next_lease));
    }
    void allow_dispatch() noexcept { block_dispatch_.store(false); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
    std::atomic<bool>              block_dispatch_{true};
};
class ThrowOnLatestJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view, std::string_view) const override {
        throw std::runtime_error("journal latest failed");
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner, std::string_view run_id,
        std::uint64_t sequence) const override {
        return inner_.load_context_publications(owner, run_id, sequence);
    }
    std::vector<neograph::HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_hook_outbox_entries(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner,
                                                    std::string_view lineage_id) const override {
        return inner_.load_lineage(owner, lineage_id);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner,
                                                       std::string_view run_id) const override {
        return inner_.load_run_lineage(owner, run_id);
    }
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner, std::string_view lineage_id,
        std::string_view head_id) const override {
        return inner_.load_lineage_head(owner, lineage_id, head_id);
    }
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner, std::string_view lineage_id,
        std::uint64_t generation) const override {
        return inner_.load_generation(owner, lineage_id, generation);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner, std::string_view run_id) const override {
        return inner_.load_execution_lease(owner, run_id);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        return inner_.compare_publish(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        return inner_.compare_publish_execution(
            owner, expected, std::move(publication), std::move(expected_lease),
            std::move(next_lease));
    }
    std::optional<ProgramJournalRecord> stored_latest(std::string_view run_id) const {
        return inner_.latest("tenant:runtime", run_id);
    }

private:
    InMemoryProgramTransitionStore inner_;
};

class AdversarialCheckpointStore final : public CheckpointStore {
public:
    enum class Mode {
        Normal,
        WrongThread,
        WrongThreadAfterPrecheck,
        WrongSchemaAfterPrecheck,
        WrongSchemaAtCoreLoad,
        WrongIdAtCoreLoad,
        MissingAfterFirstExactLoad,
        MissingAfterCoreLoad,
        FailMigrationTargetSave,
        WrongMigrationContentAtConsumingLoad,
        DelayMigrationTargetSave
    };

    void arm(Mode mode) {
        mode_.store(mode);
        exact_loads_.store(0);
        migration_target_save_failures_.store(0);
        migration_target_loads_.store(0);
        migration_target_save_started_.store(false);
    }
    std::uint32_t exact_loads() const noexcept { return exact_loads_.load(); }
    std::uint32_t migration_target_save_failures() const noexcept {
        return migration_target_save_failures_.load();
    }
    bool migration_target_save_started() const noexcept {
        return migration_target_save_started_.load();
    }

    void save(const Checkpoint& checkpoint) override {
        if (mode_.load() == Mode::DelayMigrationTargetSave &&
            checkpoint.metadata.is_object() &&
            checkpoint.metadata.contains("migrated_from")) {
            migration_target_save_started_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (mode_.load() == Mode::FailMigrationTargetSave &&
            checkpoint.metadata.is_object() &&
            checkpoint.metadata.contains("migrated_from")) {
            migration_target_save_failures_.fetch_add(1);
            throw std::runtime_error("injected migration target save failure");
        }
        inner_->save(checkpoint);
    }

    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return inner_->load_latest(thread_id);
    }

    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        auto       checkpoint = inner_->load_by_id(id);
        const auto call       = exact_loads_.fetch_add(1);
        const auto mode       = mode_.load();
        if (checkpoint && checkpoint->metadata.is_object() &&
            checkpoint->metadata.contains("migrated_from")) {
            const auto migration_call = migration_target_loads_.fetch_add(1);
            if (mode == Mode::WrongMigrationContentAtConsumingLoad && migration_call >= 3) {
                checkpoint->channel_values["channels"]["value"]["value"] = "tampered";
            }
        }
        if (mode == Mode::MissingAfterFirstExactLoad && call > 0) {
            return std::nullopt;
        }
        if (mode == Mode::MissingAfterCoreLoad && call > 2) {
            return std::nullopt;
        }
        if (checkpoint &&
            (mode == Mode::WrongThread || (mode == Mode::WrongThreadAfterPrecheck && call > 0))) {
            checkpoint->thread_id += "-tampered";
        }
        if (checkpoint && mode == Mode::WrongSchemaAfterPrecheck && call > 0) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongSchemaAtCoreLoad && call > 1) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongIdAtCoreLoad && call > 1) {
            checkpoint->id += "-tampered";
        }
        return checkpoint;
    }

    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        return inner_->list(thread_id, limit);
    }

    void delete_thread(const std::string& thread_id) override { inner_->delete_thread(thread_id); }

    void put_writes(const std::string&  thread_id,
                    const std::string&  parent_checkpoint_id,
                    const PendingWrite& write) override {
        inner_->put_writes(thread_id, parent_checkpoint_id, write);
    }

    std::vector<PendingWrite> get_writes(const std::string& thread_id,
                                         const std::string& parent_checkpoint_id) override {
        return inner_->get_writes(thread_id, parent_checkpoint_id);
    }

    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        inner_->clear_writes(thread_id, parent_checkpoint_id);
    }

private:
    std::shared_ptr<InMemoryCheckpointStore> inner_ = std::make_shared<InMemoryCheckpointStore>();
    std::atomic<Mode>                        mode_{Mode::Normal};
    std::atomic<std::uint32_t>               exact_loads_{0};
    std::atomic<std::uint32_t>               migration_target_save_failures_{0};
    std::atomic<std::uint32_t>               migration_target_loads_{0};
    std::atomic<bool>                        migration_target_save_started_{false};
};
json typed_event_value(const TypedGraphEvent& event) {
    json value{{"index", event.index()}};
    switch (event.index()) {
        case 0: {
            const auto& typed  = std::get<NodeStartEvent>(event);
            value["node_name"] = typed.node_name;
            value["retry_attempt"] =
                typed.retry_attempt ? json(*typed.retry_attempt) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 1: {
            const auto& typed     = std::get<NodeEndEvent>(event);
            value["node_name"]    = typed.node_name;
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["send_count"]   = typed.send_count;
            value["data"]         = typed.data;
            break;
        }
        case 2: {
            const auto& typed  = std::get<LlmTokenEvent>(event);
            value["node_name"] = typed.node_name;
            value["token"]     = typed.token;
            break;
        }
        case 3: {
            const auto& typed  = std::get<ChannelWriteEvent>(event);
            value["node_name"] = typed.node_name;
            value["channel"]   = typed.channel;
            value["value"]     = typed.value;
            break;
        }
        case 4:
            value["state"] = std::get<StateSnapshotEvent>(event).state;
            break;
        case 5: {
            const auto& typed     = std::get<RoutingEvent>(event);
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["next_nodes"]   = typed.next_nodes;
            value["step"]         = typed.step ? json(*typed.step) : json(nullptr);
            value["data"]         = typed.data;
            break;
        }
        case 6: {
            value["sends"] = json::array();
            for (const auto& send : std::get<SendDispatchEvent>(event).sends) {
                value["sends"].push_back({{"target_node", send.target_node},
                                          {"input", send.input},
                                          {"source_node", send.source_node}});
            }
            break;
        }
        case 7: {
            const auto& typed  = std::get<InterruptEvent>(event);
            value["node_name"] = typed.node_name;
            value["phase"]     = typed.phase ? json(*typed.phase) : json(nullptr);
            value["checkpoint_id"] =
                typed.checkpoint_id ? json(*typed.checkpoint_id) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 8: {
            const auto& typed  = std::get<ErrorEvent>(event);
            value["node_name"] = typed.node_name;
            value["message"]   = typed.message;
            value["data"]      = typed.data;
            break;
        }
        case 9: {
            const auto& typed  = std::get<RawGraphEvent>(event).event;
            value["type"]      = static_cast<int>(typed.type);
            value["node_name"] = typed.node_name;
            value["data"]      = typed.data;
            break;
        }
        default:
            throw std::logic_error("Unknown TypedGraphEvent alternative");
    }
    return value;
}
}  // namespace

TEST(ProgramRuntimeTest, CompletedRunPinsAdmittedIdentitiesAndPublishesOrderedEvents) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-completed", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
    EXPECT_EQ(result.bundle_id(), version.bundle_id());
    EXPECT_EQ(result.operation_id(), "root");
    EXPECT_EQ(result.attempt(), 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(result.remaining_budget().max_concurrency, 1U);
    ASSERT_TRUE(result.checkpoint().has_value());
    EXPECT_EQ(result.checkpoint()->core_name, "main");
    EXPECT_EQ(result.checkpoint()->core_generation_id,
              version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->program_version_id, version.id());
    ASSERT_TRUE(journal->core_checkpoint.has_value());
    EXPECT_EQ(journal->core_checkpoint->core_name, result.checkpoint()->core_name);
    EXPECT_EQ(journal->core_checkpoint->core_thread_id, result.checkpoint()->core_thread_id);
    EXPECT_EQ(journal->core_checkpoint->checkpoint_id, result.checkpoint()->checkpoint_id);
    const auto lineage_id = program_run_lineage_id("tenant:runtime", result.run_id());
    const auto lineage    = fixture.journal->load_lineage("tenant:runtime", lineage_id);
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_EQ(lineage->remaining_budget(), journal->remaining_budget);
    EXPECT_EQ(lineage->inflight_reservation(), journal->inflight_reservation);
    const auto generation = fixture.journal->load_generation("tenant:runtime", lineage_id, 1);
    ASSERT_TRUE(generation.has_value());
    EXPECT_EQ(generation->program_version_id(), version.id());
    EXPECT_EQ(generation->bundle_id(), version.bundle_id());
    EXPECT_EQ(generation->run_id(), result.run_id());

    const auto events = handle.events_after(0);
    ASSERT_GE(events.size(), 4U);
    EXPECT_EQ(events.front().kind, ProgramEventKind::Started);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished;
                            }),
              1);
    for (std::size_t index = 0; index < events.size(); ++index) {
        EXPECT_EQ(events[index].sequence, index + 1);

        EXPECT_EQ(events[index].run_id, result.run_id());
        EXPECT_EQ(events[index].program_version_id, version.id());
        EXPECT_EQ(events[index].attempt, 1U);
        EXPECT_EQ(events[index].trace_id, "trace-completed");
    }
}

TEST(ProgramRuntimeTest, DeveloperInstructionIsDurableBeforeActiveGenerationDecision) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "instruction-source", {}});
    const auto result = handle.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto contexts = std::make_shared<DurableInstructionContextStore>();
    RuntimeInstructionController controller({
        contexts,
        fixture.journal,
        fixture.catalog,
        [](const RuntimeDeveloperInstruction& instruction,
           const ProgramRunLineage& lineage,
           const ProgramRunGeneration&) {
            RuntimeInstructionDecisionData decision;
            decision.instruction_id = instruction.id();
            decision.source_run_id = instruction.data().source_run_id;
            decision.expected_lineage_head_id = lineage.id();
            decision.policy_identity = digest('9');
            decision.action = RuntimeInstructionAction::SatisfiedInPlace;
            decision.reason = "current topology already satisfies the instruction";
            return decision;
        }});
    RuntimeDeveloperInstructionData data;
    data.owner_scope = "tenant:runtime";
    data.source_run_id = result.run_id();
    data.feed_id = "developer-instructions";
    data.sequence = 1;
    data.submitted_at_ms = 1;
    data.text = "Keep the current topology";
    const auto instruction = RuntimeDeveloperInstruction::create(std::move(data));
    const auto plan = controller.submit_and_plan(instruction, std::nullopt);
    EXPECT_EQ(plan.decision.data().action,
              RuntimeInstructionAction::SatisfiedInPlace);
    EXPECT_EQ(plan.decision.data().instruction_id, instruction.id());
    const auto head = contexts->history_head(
        {"tenant:runtime", "developer-instructions"});
    ASSERT_TRUE(head.record_id);
    EXPECT_EQ(*head.record_id, plan.history_record.id());
    ASSERT_TRUE(contexts->get_artifact("tenant:runtime",
                                       plan.decision_artifact.id()));
    EXPECT_TRUE(plan.decision_artifact.required());
}

TEST(ProgramRuntimeTest, ReconnectFailsClosedWithoutDurableRuntimeRecoveryBoundary) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-recovery-required", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto overlay = std::make_shared<IsolatedProcessTransitionStore>(fixture.journal);
    overlay->set_runtime_recovery_state(
        result.run_id(), {recovery_context(result.run_id())},
        {recovery_hook(result.run_id())});
    auto runtime = fixture.make_runtime_with_transitions(overlay);

    try {
        (void)runtime->reconnect("tenant:runtime", result.run_id());
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RUNTIME_RECOVERY_UNAVAILABLE");
    }
}

TEST(ProgramRuntimeTest, ReconnectRestoresValidatedContextAndHookHeadsBeforeReturning) {
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-completed");
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-runtime-recovery", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);

    auto context = recovery_context(result.run_id());
    auto hook = recovery_hook(result.run_id());
    const auto context_id = context.epoch.id();
    const auto hook_id = hook.id();
    auto overlay = std::make_shared<IsolatedProcessTransitionStore>(fixture.journal);
    overlay->set_runtime_recovery_state(result.run_id(), {context}, {hook});
    unsigned restores = 0;
    auto runtime = fixture.make_runtime_with_transitions(
        overlay, [&](const ProgramRuntimeRecoveryState& state) {
            ++restores;
            EXPECT_EQ(state.owner_scope, "tenant:runtime");
            EXPECT_EQ(state.run_id, result.run_id());
            ASSERT_EQ(state.context_publications.size(), 1u);
            ASSERT_EQ(state.hook_outbox_entries.size(), 1u);
            EXPECT_EQ(state.context_publications.front().epoch.id(), context_id);
            EXPECT_EQ(state.hook_outbox_entries.front().id(), hook_id);
        });

    const auto reconnected = runtime->reconnect("tenant:runtime", result.run_id()).wait();
    EXPECT_EQ(reconnected.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(restores, 1u);
}

TEST(ProgramRuntimeTest, LifecycleHooksPublishBeforeSpawnAndFailClosedBeforeTerminalCas) {
    LifecycleHookTool admitted{"program-admitted"};
    LifecycleHookTool terminal{"program-before-terminal", true};
    LifecycleHookTool failed{"program-failed"};
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-hook-lifecycle", {}});

    EXPECT_EQ(admitted.calls.load(), 1U);
    // The blocked completed terminal is replaced by P_HOOK_BLOCKED and that
    // replacement event receives its own deterministic terminal-hook pass.
    EXPECT_EQ(terminal.calls.load(), 2U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_HOOK_BLOCKED");
    // RunFailed is emitted only after the terminal compare-publish wins.
    EXPECT_EQ(failed.calls.load(), 1U);
    const auto durable = fixture.journal->load("tenant:runtime", result.run_id());
    ASSERT_TRUE(durable.has_value());
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, LifecycleHookIdentityIsReusedAcrossTerminalCasRetry) {
    LifecycleHookTool admitted{"program-admitted-retry"};
    LifecycleHookTool terminal{"program-before-terminal-retry"};
    LifecycleHookTool failed{"program-failed-retry"};
    auto journal = std::make_shared<ConflictOnceJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-hook-retry", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(journal->injected());
    EXPECT_EQ(admitted.calls.load(), 1U);
    // The terminal publication was retried, but its Program event sequence and
    // therefore its durable hook event identity did not change.
    EXPECT_EQ(terminal.calls.load(), 1U);
    EXPECT_EQ(failed.calls.load(), 0U);
}

TEST(ProgramRuntimeTest, BlockedAdmissionHookReturnsRecoverableTerminalHandleAfterReconnect) {
    completed_calls.store(0);
    LifecycleHookTool admitted{"program-admitted-blocked", true};
    LifecycleHookTool terminal{"program-before-terminal-after-admission-block"};
    LifecycleHookTool failed{"program-failed-after-admission-block"};
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Strict, false,
                            lifecycle_hooks(admitted, terminal, failed));
    const auto version = fixture.admit("runtime-completed");
    ProgramInvocation invocation{json::object(), grant(), "trace-admission-block", {},
                                 "admission-hook-blocked"};

    // The Started transition is durable before the hook executes. A failure
    // must return a handle whose terminal state is already recoverable.
    const auto result = fixture.runtime->start("tenant:runtime", version, invocation).wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_HOOK_BLOCKED");
    EXPECT_EQ(completed_calls.load(), 0U);

    fixture.recreate_catalog_and_runtime();
    const auto recovered = fixture.runtime->reconnect("tenant:runtime", "admission-hook-blocked").wait();
    EXPECT_EQ(recovered.id(), result.id());
    EXPECT_EQ(recovered.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(recovered.failure());
    EXPECT_EQ(recovered.failure()->code, "P_HOOK_BLOCKED");
    const auto durable = fixture.journal->load("tenant:runtime", "admission-hook-blocked");
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, InterruptedCancellationHookBlockPersistsFailureAfterReconnect) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto version = fixture.admit("runtime-interrupt");
    const auto interrupted = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupted-cancel-hook", {},
                          "interrupted-cancel-hook"}).wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    LifecycleHookTool admitted{"program-admitted-interrupted-cancel"};
    LifecycleHookTool terminal{"program-terminal-interrupted-cancel", true};
    LifecycleHookTool failed{"program-failed-interrupted-cancel"};
    fixture.hook_runtime = lifecycle_hooks(admitted, terminal, failed);
    fixture.recreate_catalog_and_runtime();
    auto handle = fixture.runtime->reconnect("tenant:runtime", interrupted.run_id());

    EXPECT_TRUE(handle.cancel());
    const auto blocked = handle.wait();
    EXPECT_EQ(blocked.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(blocked.failure());
    EXPECT_EQ(blocked.failure()->code, "P_HOOK_BLOCKED");
    const auto durable = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Failed);
}
TEST(ProgramRuntimeTest, RejectsCrossScopeStartBeforePublishingRun) {
    completed_calls.store(0);
    AdmittedRuntime   fixture;
    const auto        version = fixture.admit("runtime-completed");
    ProgramInvocation invocation{json::object(), grant(), "trace-cross-scope", {}};
    invocation.requested_run_id = "cross-scope-run";

    EXPECT_THROW((void)fixture.runtime->start("tenant:other", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_FALSE(fixture.journal->load("tenant:other", "cross-scope-run").has_value());
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", "cross-scope-run").has_value());
}

TEST(ProgramRuntimeTest, RequestedRunIdIsUsedExactlyAndCollisionNeverDispatches) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto invocation             = ProgramInvocation{json::object(), grant(), "trace-requested", {}};
    invocation.requested_run_id = "harness-run-exact";
    const auto first = fixture.runtime->start("tenant:runtime", version, invocation).wait();
    EXPECT_EQ(first.run_id(), "harness-run-exact");
    EXPECT_EQ(completed_calls.load(), 1U);

    EXPECT_THROW((void)fixture.runtime->start("tenant:runtime", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CanonicalRunInvocationIsRetainedExactlyAndAcceptsRuntimeEventSink) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-canonical-adapter";
    invocation.program_version_id = version.id();
    invocation.run_id             = "canonical-runtime-run";
    invocation.budget             = grant();
    invocation.input              = json{{"request", "canonical"}};
    invocation.message_sequence   = 17;
    invocation.idempotency_key    = "test-canonical-adapter:17";
    invocation.correlation_id     = "trace-canonical-runtime";
    invocation.validate();
    auto sink = std::make_shared<CountingSink>();

    auto       handle = fixture.runtime->start(invocation, sink);
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.run_id(), invocation.run_id);
    EXPECT_EQ(handle.snapshot().invocation(), invocation);
    EXPECT_EQ(handle.snapshot().child_depth(), 0U);
    EXPECT_GT(sink->calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ReconnectTerminalAfterCatalogRecreationIsByteExactAndNonMutating) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version   = fixture.admit("runtime-completed");
    const auto      completed = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-terminal-reconnect", {}});
    const auto run_id = completed.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), completed.serialize_canonical());
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal_after = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after.has_value());
    EXPECT_EQ(journal_after->id, journal_before->id);
    EXPECT_EQ(journal_after->sequence, journal_before->sequence);

    const auto diagnostic_code = [&](std::string_view owner, std::string_view candidate) {
        try {
            (void)fixture.runtime->reconnect(owner, candidate);
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code("tenant:other", run_id),
              diagnostic_code("tenant:other", "absent-run"));
}

TEST(ProgramRuntimeTest, ReconnectInterruptedResumesExactCheckpointOnceWithoutInputReplay) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"original", "must-not-replay"}}, grant(), "trace-interrupted-reconnect", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto run_id = interrupted.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    const auto journal_after_reconnect = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after_reconnect.has_value());
    EXPECT_EQ(journal_after_reconnect->id, journal_before->id);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", run_id,
                     resume_for(interrupted, json{{"decision", "approved"}}, "trace-exact-resume"))
            .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, TypedPendingEffectPublishesOnceAndResumesByExactCallIdentity) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-effect-interrupt");

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    EXPECT_FALSE(interrupted.interrupt()->pending_input.has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.call_id(), "call-effect-1");
    EXPECT_EQ(pending.payload()["effect"]["tool"], "search");
    EXPECT_EQ(pending.state(), ProgramPendingState::Awaiting);

    const auto effects = fixture.journal->load_effects("tenant:runtime", interrupted.run_id());
    ASSERT_EQ(effects.size(), 1U);
    EXPECT_EQ(effects.front().sequence(), 1U);
    EXPECT_EQ(effects.front().effect(), pending);

    fixture.recreate_catalog_and_runtime();
    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", interrupted.run_id()).wait();
    EXPECT_EQ(reconnected.serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted.run_id(),
            ProgramResume{json{{"result", "unused"}}, "trace-wrong-effect", {}, "wrong-call"});
        FAIL() << "wrong pending call identity was accepted";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_PENDING_ID_MISMATCH");
    }
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"result", "recorded"}}, "trace-effect-resume"))
            .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message + " " +
                                    resumed.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "recorded");
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(persisted->effect_sequence(), 1U);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
}

TEST(ProgramRuntimeTest, UnknownNonIdempotentEffectBlocksCancelAndRedispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-effect-nonidempotent");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-ambiguous", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    const auto run_id  = interrupted.run_id();

    auto ambiguous = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Unknown,
                                                             std::nullopt,
                                                             "trace-effect-unknown",
                                                             {}})
                         .wait();
    EXPECT_EQ(ambiguous.status(), ProgramTerminalStatus::AmbiguousEffect);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    auto persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::AmbiguousEffect);
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Ambiguous);
    const auto ambiguous_journal = persisted->journal_head();
    ASSERT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);

    fixture.recreate_catalog_and_runtime();
    auto recovered = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_FALSE(recovered.cancel());
    EXPECT_EQ(fixture.journal->latest("tenant:runtime", run_id)->id, ambiguous_journal);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    auto completed = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Completed,
                                                             json{{"result", "reconciled"}},
                                                             "trace-effect-reconciled",
                                                             {}})
                         .wait();
    EXPECT_EQ(completed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed.output()["channels"]["value"]["value"], "reconciled");
    EXPECT_EQ(interrupt_calls.load(), 2U);
    persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);
}

TEST(ProgramRuntimeTest, CrossOwnerResumeIsNoOracleAndNeverReadsCheckpoint) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-owner-scope", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    checkpoint_store->arm(AdversarialCheckpointStore::Mode::Normal);

    const auto diagnostic_code = [&](std::string_view run_id) {
        try {
            (void)fixture.runtime->resume(
                "tenant:other", run_id,
                resume_for(interrupted, json{{"decision", "unused"}}, "trace-cross-owner"));
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code(interrupted.run_id()), diagnostic_code("absent-run"));
    EXPECT_EQ(checkpoint_store->exact_loads(), 0U);
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RetriesOneSpuriousTerminalJournalConflict) {
    auto            journal = std::make_shared<ConflictOnceJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-cas-retry", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(journal->injected());
    const auto latest = journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
}

TEST(ProgramRuntimeTest, JournalCommitPrecedesCheckpointAndTerminalEventDelivery) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            sink    = std::make_shared<JournalObservingSink>(fixture.journal);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-order", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(sink->checkpoint_observed_committed.load());
    EXPECT_TRUE(sink->terminal_observed_committed.load());
}

TEST(ProgramRuntimeTest, ProgramEnvelopePreservesDirectCoreBehavior) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            program_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-equivalence", {}});
    const auto program_result = program_handle.wait();

    const auto definition = program_document("runtime-completed")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    auto core_registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();

    EngineResources engine_resources;
    engine_resources.registry = core_registry;
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    direct_engine->set_checkpoint_store(direct_checkpoints);
    RunConfig direct_config;
    direct_config.thread_id = "direct-equivalence";
    direct_config.input     = json::object();
    direct_config.max_steps = 20;
    std::vector<json> direct_events;
    const auto        direct_result =
        direct_engine->run_stream(direct_config, [&](const GraphEvent& event) {
            direct_events.push_back(typed_event_value(to_typed_event(event)));
        });

    std::vector<json> program_events;
    for (const auto& event : program_handle.events_after(0)) {
        if (event.kind == ProgramEventKind::Core) {
            program_events.push_back(typed_event_value(std::get<TypedGraphEvent>(event.payload)));
        }
    }
    ASSERT_TRUE(program_result.checkpoint().has_value());
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_result.checkpoint_id);
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_result.checkpoint()->checkpoint_id);

    EXPECT_EQ(program_result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_result.status(), RunStatus::Completed);
    EXPECT_EQ(program_result.output(), direct_result.output);
    EXPECT_EQ(program_result.execution_trace(), direct_result.execution_trace);
    EXPECT_EQ(program_events, direct_events);
    EXPECT_EQ(program_result.usage().model_tokens,
              static_cast<std::uint64_t>(direct_result.usage.total_tokens));
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_result.checkpoint()->checkpoint_schema_version,
              direct_checkpoint->schema_version);
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(completed_calls.load(), 2U);
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
std::string javascript_runtime_source(std::string node_type, std::string body);
RunBudget javascript_budget(std::uint64_t max_concurrency        = 2,
                            std::uint64_t max_program_operations = 32,
                            std::uint64_t max_child_depth        = 0,
                            std::uint64_t max_total_children     = 0);

TEST(ProgramRuntimeTest, JavaScriptGeneratorPreservesLocalYieldValue) {
    const auto source = ProgramSource::from_javascript("test:generator-lifetime.js",
                                                       R"JS(
            export function* main(input) {
                const command = {protocol_version: 1, op: "call_core", name: "main",
                                 input: {requested: input.requested}};
                yield command;
            }
        )JS");

    const auto step = std::async(std::launch::async, [source] {
                          auto generator = neograph::program::detail::JavaScriptGenerator::open(
                              source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
                          if (!generator)
                              throw std::runtime_error("JavaScript generator was not exported");
                          std::optional<json> response;
                          return generator->next(std::move(response));
                      }).get();
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step.value, (json{{"protocol_version", 1},
                                {"op", "call_core"},
                                {"name", "main"},
                                {"input", json{{"requested", "draft"}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorLoadsReceiptBoundSealedModule) {
    const std::string module_name   = "sealed:command-input@1.0.0";
    const std::string module_source = R"JS(
        export function commandInput(input) {
            return {requested: input.requested + "-sealed"};
        }
    )JS";
    const auto        source        = ProgramSource::from_javascript(
        "test:generator-sealed-module.js",
        "import { commandInput } from \"sealed:command-input@1.0.0\";\n"
                      "export function* main(input) {\n"
                      "  yield ng.callCore(\"main\", commandInput(input), \"sealed:1\");\n"
                      "}\n",
        {{module_name, digest('a')}}, {}, {{module_name, digest('a'), module_source}});

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();

    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->source_site(), "sealed:1");
    EXPECT_EQ(step.command->to_json().at("arguments").at("input"),
              (json{{"requested", "draft-sealed"}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExposesOnlyControlCommandBinding) {
    const auto source = ProgramSource::from_javascript("test:generator-host-surface.js",
                                                       R"JS(
            export function* main() {
                return {
                    graph: typeof ng.graph,
                    callCore: typeof ng.callCore,
                    frozen: Object.isFrozen(ng),
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_TRUE(step.done);
    EXPECT_EQ(step.value,
              (json{{"graph", "undefined"}, {"callCore", "function"}, {"frozen", true}}));
}

TEST(ProgramRuntimeTest, JavaScriptCapabilityManifestMatchesTheInstalledCommandKernel) {
    const auto source = ProgramSource::from_javascript(
        "test:command-capability-manifest.js",
        "export function* main() { return Object.keys(ng).sort(); }");
    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_TRUE(step.done);

    std::vector<std::string> installed;
    for (const auto& name : step.value) installed.push_back(name.get<std::string>());
    std::vector<std::string> declared;
    const auto manifest = javascript_authoring_capability_manifest();
    for (const auto& method : manifest.at("main").at("command_methods"))
        declared.push_back(method.at("name").get<std::string>());
    for (const auto& property : manifest.at("main").at("ng_properties"))
        declared.push_back(property.at("name").get<std::string>());
    std::sort(declared.begin(), declared.end());
    EXPECT_EQ(installed, declared);
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorProducesSealedTypedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript("test:typed-command.js",
                                                       R"JS(
            export function* main() {
                const call = ng.callCore("main", {requested: "draft"}, "main:12");
                if (!Object.isFrozen(call)) {
                    throw new Error("command was not sealed");
                }
                yield call;
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_FALSE(step.done);
    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->kind(), JavaScriptCommandKind::CallCore);
    EXPECT_EQ(step.command->import_slot(), JAVASCRIPT_IMPORT_SLOT_CALL_CORE);
    EXPECT_EQ(step.command->source_site(), "main:12");
    EXPECT_EQ(step.value, step.command->to_json());
}

TEST(ProgramRuntimeTest, JavaScriptRetainedCommandsShareTheQuickJsMemoryCeiling) {
    const auto              source = ProgramSource::from_javascript("test:native-live-commands.js",
                                                                    R"JS(
            export function* main() {
                const retained = [];
                for (let index = 0; index < 100000; ++index) {
                    retained.push(ng.callCore(
                        "main", {index, padding: "x".repeat(64)}, "retained:" + index));
                }
                yield retained[0];
            }
        )JS");
    JavaScriptCompileLimits limits;
    limits.memory_limit_bytes  = 2u * 1024u * 1024u;
    limits.max_stack_bytes     = 128u * 1024u;
    limits.max_interrupt_polls = 100'000'000u;
    auto generator =
        neograph::program::detail::JavaScriptGenerator::open(source, json::object(), limits);
    ASSERT_TRUE(generator.has_value());
    try {
        (void)generator->next();
        FAIL() << "expected cumulative native-memory rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_RESOURCE_LIMIT");
        ASSERT_TRUE(error.witness().contains("allocator_limit_denied_count"));
        EXPECT_GT(error.witness().at("allocator_limit_denied_count").get<std::size_t>(), 0U);
    }
}

TEST(ProgramRuntimeTest, JavaScriptCommandFinalizersReleaseNativeMemoryCharges) {
    const auto source = ProgramSource::from_javascript("test:native-command-release.js",
                                                       R"JS(
            export function* main() {
                for (let batch = 0; batch < 2000; ++batch) {
                    let retained = [];
                    for (let index = 0; index < 16; ++index) {
                        retained.push(ng.callCore(
                            "main", {index, padding: "x".repeat(64)}, "temporary:" + index));
                    }
                    retained = null;
                }
                yield ng.callCore("main", {}, "survived");
            }
        )JS");
    JavaScriptCompileLimits limits;
    limits.memory_limit_bytes  = 2u * 1024u * 1024u;
    limits.max_stack_bytes     = 128u * 1024u;
    limits.max_interrupt_polls = 100'000'000u;
    // This is a finalizer/accounting stress test, not a wall-time test. Keep
    // the 32,000 temporary native commands and tight 2 MiB memory ceiling,
    // while allowing for sanitizer allocator overhead on Windows runners.
    limits.max_wall_time_ms    = 180'000u;
    auto generator =
        neograph::program::detail::JavaScriptGenerator::open(source, json::object(), limits);
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->source_site(), "survived");
}

TEST(ProgramRuntimeTest, JavaScriptRuntimeRejectsImportsBeforeQuickJsEvaluation) {
    const auto source = ProgramSource::from_javascript(
        "test:runtime-import.js",
        "import \"sealed:approved@1.0.0\";\n"
        "export function* main() { yield {}; }\n",
        {{"sealed:approved@1.0.0", std::string("sha256:") + std::string(64, 'a')}});
    try {
        (void)neograph::program::detail::JavaScriptGenerator::open(source, json::object(),
                                                                   JavaScriptCompileLimits{});
        FAIL() << "expected pre-evaluation import rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_IMPORT_UNAVAILABLE");
        ASSERT_TRUE(error.source_span().has_value());
        EXPECT_EQ(error.source_span()->line_begin, 1U);
        EXPECT_EQ(error.witness().at("receipt_bound_source_available"), false);
    }
}

TEST(ProgramRuntimeTest, JavaScriptRuntimeRejectsDynamicImportsBeforeQuickJsEvaluation) {
    const auto source = ProgramSource::from_javascript("test:runtime-dynamic-import.js",
                                                       "export function* main() {\n"
                                                       "  yield import(\"sealed:unknown@1.0.0\");\n"
                                                       "}\n");
    try {
        (void)neograph::program::detail::JavaScriptGenerator::open(source, json::object(),
                                                                   JavaScriptCompileLimits{});
        FAIL() << "expected pre-evaluation dynamic import rejection";
    } catch (const neograph::program::detail::JavaScriptCompileError& error) {
        EXPECT_EQ(error.code(), "P_JS_IMPORT_UNAVAILABLE");
        ASSERT_TRUE(error.source_span().has_value());
        EXPECT_EQ(error.source_span()->line_begin, 2U);
    }
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorRejectsForgedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript("test:forged-command.js",
                                                       R"JS(
            export function* main() {
                yield {
                    protocol_version: 1,
                    kind: "call_core",
                    import_slot: 0,
                    source_site: "main:1",
                    arguments: {name: "main", input: {}}
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_FALSE(step.done);
    EXPECT_FALSE(step.command.has_value());
}

TEST(ProgramRuntimeTest, JavaScriptNgExposesClosedConstructorSet) {
    const auto source = ProgramSource::from_javascript("test:command-constructors.js",
                                                       R"JS(
            export function* main() {
                const call = ng.callCore("main", {x: 1}, "call:1");
                const spawn = ng.spawn("child", {x: 2}, "spawn:1");
                yield call;
                yield spawn;
                yield ng.await(spawn, 50, "await:1");
                yield ng.join([call, spawn], "all", undefined, "join:1");
                yield ng.all([call, spawn], "all:1");
                yield ng.race([call, spawn], "race:1");
                yield ng.quorum([call, spawn], 1, "quorum:1");
                yield ng.emit({kind: "event"}, "emit:1");
                yield ng.checkpoint({checkpoint: true}, "checkpoint:1");
                yield ng.cancelScope("run", "stop", "cancel:1");
                yield ng.hostCapability(42, {request: "approved"}, "host:1");
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const std::vector<JavaScriptCommandKind> expected = {
        JavaScriptCommandKind::CallCore,      JavaScriptCommandKind::Spawn,
        JavaScriptCommandKind::Await,         JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,          JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,          JavaScriptCommandKind::Emit,
        JavaScriptCommandKind::Checkpoint,    JavaScriptCommandKind::CancelScope,
        JavaScriptCommandKind::HostCapability};
    for (const auto kind : expected) {
        const auto step = generator->next(json::object());
        ASSERT_FALSE(step.done);
        ASSERT_TRUE(step.command.has_value());
        EXPECT_EQ(step.command->kind(), kind);
        EXPECT_EQ(step.value, step.command->to_json());
    }
    const auto done = generator->next(json::object());
    EXPECT_TRUE(done.done);
    EXPECT_EQ(done.value, json::object());
}

TEST(ProgramRuntimeTest, JavaScriptControlCancellationInterruptsBeforeAnyLateDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main() {
                while (true) {}
            }
        )JS");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-javascript-cancel", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Cancelled)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExecutesYieldedCoreCommand) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const core = yield ng.callCore("main", {requested: input.requested});
                return {requested: input.requested, value: core.value};
            }
        )JS");
    const auto bundle = fixture.store->get_bundle("tenant:runtime", version.bundle_id());
    ASSERT_TRUE(bundle.has_value());
    const auto control_source = bundle->control_source();
    ASSERT_TRUE(control_source.has_value());
    auto restored_generator = neograph::program::detail::JavaScriptGenerator::open(
        *control_source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
    ASSERT_TRUE(restored_generator.has_value());
    const auto restored_step = restored_generator->next();
    ASSERT_FALSE(restored_step.done);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-control", {}});

    EXPECT_EQ(version.execution_guarantee(), ExecutionGuarantee::Unmanaged);
    const auto failure = result.failure();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (failure ? failure->code + ": " + failure->message + " " + failure->witness.dump()
                    : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"requested", "draft"}, {"value", "completed"}}));
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCommandJournalPersistsCoordinateAndResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "journal:1");
                return {value: output.value};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "journal"}}, grant(), "trace-javascript-command-journal", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", result.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].command_ordinal(), 1U);
    EXPECT_TRUE(entries[0].pending());
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("status"), "completed");
    EXPECT_EQ(entries[1].command().source_site(), "journal:1");
}

TEST(ProgramRuntimeTest, JavaScriptCompletedHeadFreshRuntimeReplaysRecordedResult) {
    completed_calls.store(0);
    auto            journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:result");
                return {value: output.value};
            }
        )JS");

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "recorded"}}, grant(), "trace-javascript-result-crash", {}});
    if (!journal->wait_for_result(std::chrono::seconds(2))) {
        const auto failed = original.wait();
        FAIL() << (failed.failure() ? failed.failure()->code + ": " + failed.failure()->message +
                                          " " + failed.failure()->witness.dump()
                                    : "command result was not journaled");
    }
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto durable_commands =
        journal->load_javascript_commands("tenant:runtime", original.run_id(), 0);
    ASSERT_EQ(durable_commands.size(), 2U);
    EXPECT_TRUE(durable_commands.back().completed());
    EXPECT_FALSE(journal->load_execution_lease(
        "tenant:runtime", original.run_id()));
    const auto durable_snapshot = journal->load("tenant:runtime", original.run_id());
    ASSERT_TRUE(durable_snapshot.has_value());
    ASSERT_TRUE(durable_snapshot->exact_checkpoint().has_value());
    const auto durable_events = journal->load_events("tenant:runtime", original.run_id(), 0);
    ASSERT_FALSE(durable_events.empty());
    EXPECT_EQ(durable_snapshot->event_sequence(), durable_events.back().sequence);
    EXPECT_TRUE(std::any_of(durable_events.begin(), durable_events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint ==
                   *durable_snapshot->exact_checkpoint();
    }));

    auto       fresh_runtime = fixture.make_runtime();
    const auto replayed      = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(replayed.output(), (json{{"value", "completed"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto replayed_events = journal->load_events("tenant:runtime", original.run_id(), 0);
    for (const auto& durable_event : durable_events) {
        EXPECT_EQ(std::count_if(replayed_events.begin(), replayed_events.end(),
                                [&](const auto& replayed_event) {
                                    return replayed_event.id == durable_event.id;
                                }),
                  1);
    }

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, GraphMigrationFencesSourceAndExactResumesGenerationTwo) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto version = fixture.admit("runtime-short-blocking");

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-source", {}});
    auto target = fixture.runtime->migrate_graph(
        source, ProgramGraphMigrationTarget{version.id(), "graph-migration-target", {}});
    const auto result = target.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    EXPECT_EQ(lineage->active_run_record_id(), target.snapshot().id());
    EXPECT_EQ(lineage->remaining_budget(), result.remaining_budget());
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->graph_migration_receipt());
    EXPECT_FALSE(generation->replacement_receipt());
    EXPECT_EQ(generation->graph_migration_receipt()->capsule().source_run_id(),
              source.run_id());
    const auto durable_capsule = fixture.journal->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(),
        generation->graph_migration_receipt()->capsule().source_lineage_head_id());
    ASSERT_TRUE(durable_capsule);
    EXPECT_EQ(durable_capsule->id(),
              generation->graph_migration_receipt()->capsule().id());
    const auto initial = fixture.journal->load_generation_initial_publication(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(initial->run_record.exact_checkpoint());
    ASSERT_TRUE(initial->run_record.exact_checkpoint_content_id());
    const auto target_checkpoint = fixture.checkpoints->load_by_id(
        initial->run_record.exact_checkpoint()->checkpoint_id);
    ASSERT_TRUE(target_checkpoint);
    EXPECT_EQ(*initial->run_record.exact_checkpoint_content_id(),
              graph_migration_checkpoint_content_id(*target_checkpoint));

    const auto reconnected = fixture.runtime->reconnect(
        "tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(reconnected.id(), result.id());
    EXPECT_EQ(reconnected.run_id(), target.run_id());
    const auto retried = fixture.runtime
                             ->migrate_graph(
                                 source,
                                 ProgramGraphMigrationTarget{
                                     version.id(), "graph-migration-target", {}})
                             .wait();
    EXPECT_EQ(retried.id(), result.id());
    EXPECT_EQ(retried.run_id(), target.run_id());
    (void)source.wait();
    EXPECT_EQ(followup_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterAdmitsShapePreservingSuccessor) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(1);

    const auto source_version = fixture.admit("runtime-short-blocking");
    auto       target_document = program_document("runtime-short-blocking");
    // The target has a different immutable Core definition/plan identity, but
    // preserves every checkpointed channel, node id, edge, and barrier member.
    target_document["root"]["definition"]["nodes"]["work"]["migration_epoch"] = 2;
    const auto target_version = fixture.admit_document(std::move(target_document));
    ASSERT_NE(source_version.id(), target_version.id());
    ASSERT_NE(source_version.core_materialization_receipt().plans.front().compiled_plan_identity,
              target_version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto source_bundle = fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle = fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);
    EXPECT_FALSE(MigrationPlan::between(source_version, *source_bundle, target_version, *target_bundle)
                     .is_compatible());
    const auto adapter = GraphSemanticMigrationAdapter::prepare(
        source_version, *source_bundle, target_version, *target_bundle);
    EXPECT_TRUE(adapter.binds(source_version, *source_bundle, target_version, *target_bundle));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), grant(), "trace-semantic-graph-migration-source", {}});
    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source, ProgramGraphMigrationTarget{target_version.id(),
                                                "semantic-graph-migration-without-adapter", {}}),
        ProgramDiagnosticError);
    auto target = fixture.runtime->migrate_graph(
        source, ProgramGraphMigrationTarget{target_version.id(), "semantic-graph-migration-target",
                                             {}, adapter});
    const auto result = target.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);

    const auto lineage = fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    const auto receipt = generation->graph_migration_receipt();
    ASSERT_TRUE(receipt);
    ASSERT_TRUE(receipt->semantic_adapter());
    EXPECT_EQ(receipt->semantic_adapter()->id(), adapter.id());
    EXPECT_EQ(receipt->target_version().id(), target_version.id());
    const auto parsed = ProgramGraphMigrationReceipt::parse(receipt->serialize_canonical());
    ASSERT_TRUE(parsed.semantic_adapter());
    EXPECT_EQ(parsed.semantic_adapter()->id(), adapter.id());
    EXPECT_EQ(parsed.target_checkpoint().metadata.at("semantic_migration_adapter_id"), adapter.id());

    auto recovered = fixture.make_runtime();
    EXPECT_EQ(recovered->reconnect("tenant:runtime", source.run_id()).wait().status(),
              ProgramTerminalStatus::Completed);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, GraphSemanticMigrationAdapterRejectsFrontierRename) {
    AdmittedRuntime fixture(1);
    const auto source_version = fixture.admit("runtime-short-blocking");
    auto       target_document = program_document("runtime-short-blocking");
    auto       definition = target_document["root"]["definition"];
    definition["nodes"] = json{{"work", json{{"type", "runtime-short-blocking"}}},
                               {"after", json{{"type", "runtime-followup"}}}};
    definition["edges"] = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "after"}},
        json{{"from", "after"}, {"to", "__end__"}},
    });
    target_document["root"]["definition"] = std::move(definition);
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto source_bundle = fixture.store->get_bundle("tenant:runtime", source_version.bundle_id());
    const auto target_bundle = fixture.store->get_bundle("tenant:runtime", target_version.bundle_id());
    ASSERT_TRUE(source_bundle);
    ASSERT_TRUE(target_bundle);

    EXPECT_THROW((void)GraphSemanticMigrationAdapter::prepare(
                     source_version, *source_bundle, target_version, *target_bundle),
                 std::invalid_argument);
}

TEST(ProgramRuntimeTest, GraphMigrationTargetSaveFailureResumesHeldSource) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::FailMigrationTargetSave);
    AdmittedRuntime fixture(1, checkpoints);
    const auto version = fixture.admit("runtime-short-blocking");

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-failed", {}});
    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{version.id(), "graph-migration-failed-target", {}}),
        std::runtime_error);

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    EXPECT_EQ(checkpoints->migration_target_save_failures(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_EQ(lineage->active_run_record_id(), source.snapshot().id());
    EXPECT_FALSE(fixture.journal->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(), lineage->id()));
    EXPECT_FALSE(fixture.journal->load(
        "tenant:runtime", "graph-migration-failed-target"));
}

TEST(ProgramRuntimeTest, GraphMigrationRejectsContentChangedAtConsumingLoad) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(
        AdversarialCheckpointStore::Mode::WrongMigrationContentAtConsumingLoad);
    AdmittedRuntime fixture(1, checkpoints);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-content-race", {}});

    const auto result = fixture.runtime
                            ->migrate_graph(
                                source,
                                ProgramGraphMigrationTarget{
                                    version.id(), "graph-migration-content-race-target", {}})
                            .wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, UnreadableSafePointCommitFencesSourceDurably) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-uncertain-source", {}});
    journal->crash_after_next_graph_safe_point_with_unreadable_commit();

    EXPECT_THROW(
        (void)fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{
                version.id(), "graph-migration-uncertain-target", {}}),
        std::runtime_error);
    const auto source_result = source.wait();
    EXPECT_EQ(source_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto durable = journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->continuation().state, ContinuationState::Cancelled);
    ASSERT_TRUE(durable->exact_checkpoint());
    ASSERT_TRUE(durable->exact_checkpoint_content_id());
    EXPECT_FALSE(journal->load(
        "tenant:runtime", "graph-migration-uncertain-target"));

    auto fresh_runtime = fixture.make_runtime();
    EXPECT_EQ(fresh_runtime->reconnect("tenant:runtime", source.run_id()).wait().status(),
              ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, CancellationDuringGraphMigrationFencePublishesNoSuccessor) {
    blocking_calls.store(0);
    blocking_active.store(0);
    AdmittedRuntime fixture(2);
    const auto version = fixture.admit("runtime-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-cancel", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);

    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime->migrate_graph(
            source,
            ProgramGraphMigrationTarget{version.id(), "graph-migration-cancelled-target", {}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(source.cancel());
    EXPECT_THROW((void)migration.get(), std::runtime_error);
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_FALSE(fixture.journal->load(
        "tenant:runtime", "graph-migration-cancelled-target"));
}

TEST(ProgramRuntimeTest, CancellationAfterHeldSourceLinearizesBehindMigrationCommit) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::DelayMigrationTargetSave);
    AdmittedRuntime fixture(1, checkpoints);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-linearized-cancel", {}});
    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime
            ->migrate_graph(
                source,
                ProgramGraphMigrationTarget{
                    version.id(), "graph-migration-linearized-target", {}})
            .wait();
    });
    for (unsigned attempt = 0;
         !checkpoints->migration_target_save_started() && attempt < 3000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(checkpoints->migration_target_save_started());

    EXPECT_FALSE(source.cancel());
    const auto target = migration.get();
    EXPECT_EQ(target.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    const auto generation = fixture.journal->load_generation(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    const auto receipt = generation->graph_migration_receipt();
    ASSERT_TRUE(receipt);
    const auto source_record = fixture.journal->load(
        "tenant:runtime", source.run_id());
    const auto source_head = fixture.journal->load_lineage_head(
        "tenant:runtime", lineage->lineage_id(),
        receipt->capsule().source_lineage_head_id());
    const auto initial = fixture.journal->load_generation_initial_publication(
        "tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(source_record);
    ASSERT_TRUE(source_head);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->run_record.invocation().budget,
              program_replacement_remaining_budget(
                  *source_record, *source_head,
                  initial->run_record.created_at_ms()));
    EXPECT_LE(initial->run_record.invocation().budget.wall_time_ms + 150,
              source_record->remaining_budget().wall_time_ms);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, RemoteReconnectCannotStealLiveRootExecutionLease) {
    blocking_calls.store(0);
    blocking_active.store(0);
    auto backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    AdmittedRuntime fixture(2, {}, local_view);
    const auto version = fixture.admit("runtime-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-live-root-execution-lease", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    const auto lease = backend->load_execution_lease(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lease);

    auto remote_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto remote_runtime = fixture.make_runtime_with_transitions(remote_view);
    try {
        (void)remote_runtime->reconnect("tenant:runtime", source.run_id());
        FAIL() << "remote reconnect unexpectedly stole a live root execution lease";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RUN_LEASE_HELD");
    }
    EXPECT_EQ(blocking_calls.load(), 1U);

    EXPECT_TRUE(source.cancel());
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_FALSE(backend->load_execution_lease(
        "tenant:runtime", source.run_id()));
}

TEST(ProgramRuntimeTest, ExpiredRootExecutionLeaseFencesWithoutRedispatch) {
    blocking_calls.store(0);
    blocking_active.store(0);
    auto backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    local_view->block_execution_release();
    AdmittedRuntime fixture(2, {}, local_view);
    const auto version = fixture.admit("runtime-blocking");
    auto short_budget = grant();
    short_budget.wall_time_ms = 100;
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), short_budget,
                          "trace-expired-root-execution-lease", {}});
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    if (!local_view->wait_for_execution_release(std::chrono::seconds(2))) {
        local_view->release_execution_release();
        FAIL() << "root execution did not reach its blocked lease release";
    }

    auto first_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto second_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto first_runtime = fixture.make_runtime_with_transitions(first_view);
    auto second_runtime = fixture.make_runtime_with_transitions(second_view);
    std::barrier ready(3);
    const auto recover = [&](ProgramRuntime& runtime) {
        ready.arrive_and_wait();
        try {
            return runtime.reconnect("tenant:runtime", source.run_id()).wait().status();
        } catch (const ProgramDiagnosticError&) {
            return ProgramTerminalStatus::Failed;
        }
    };
    auto first = std::async(std::launch::async, [&] { return recover(*first_runtime); });
    auto second = std::async(std::launch::async, [&] { return recover(*second_runtime); });
    ready.arrive_and_wait();
    const auto first_status = first.get();
    const auto second_status = second.get();

    const auto durable = backend->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(durable);
    ASSERT_TRUE(durable->terminal_result());
    EXPECT_EQ(durable->terminal_result()->status(), ProgramTerminalStatus::TimedOut);
    EXPECT_TRUE(first_status == ProgramTerminalStatus::TimedOut ||
                second_status == ProgramTerminalStatus::TimedOut);
    EXPECT_FALSE(backend->load_execution_lease("tenant:runtime", source.run_id()));
    EXPECT_EQ(blocking_calls.load(), 1U);

    local_view->release_execution_release();
    (void)source.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RemoteReconnectCannotDispatchDurablyHeldGraphMigrationSource) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto checkpoints = std::make_shared<AdversarialCheckpointStore>();
    checkpoints->arm(AdversarialCheckpointStore::Mode::DelayMigrationTargetSave);
    auto backend = std::make_shared<InMemoryProgramTransitionStore>();
    auto local_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    AdmittedRuntime fixture(1, checkpoints, local_view);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(),
                          "trace-graph-migration-remote-fence", {}});
    auto migration = std::async(std::launch::async, [&] {
        return fixture.runtime
            ->migrate_graph(
                source,
                ProgramGraphMigrationTarget{
                    version.id(), "graph-migration-remote-fence-target", {}})
            .wait();
    });
    for (unsigned attempt = 0;
         !checkpoints->migration_target_save_started() && attempt < 3000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(checkpoints->migration_target_save_started());
    const auto held_lineage = backend->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(held_lineage);
    ASSERT_TRUE(backend->load_graph_migration_capsule(
        "tenant:runtime", source.run_id(), held_lineage->id()));

    auto remote_view = std::make_shared<IsolatedProcessTransitionStore>(backend);
    auto remote_runtime = fixture.make_runtime_with_transitions(remote_view);
    EXPECT_THROW(
        (void)remote_runtime->reconnect("tenant:runtime", source.run_id()),
        ProgramDiagnosticError);
    EXPECT_EQ(followup_calls.load(), 0U);

    const auto target = migration.get();
    EXPECT_EQ(target.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    (void)source.wait();
}

TEST(ProgramRuntimeTest, ConcurrentGraphMigrationRequestsPublishOneSuccessor) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto version = fixture.admit("runtime-short-blocking");
    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-graph-migration-race", {}});
    auto second_runtime = fixture.make_runtime();
    std::barrier ready(3);
    const auto migrate = [&](ProgramRuntime& runtime, std::string run_id) {
        ready.arrive_and_wait();
        try {
            const auto result = runtime
                                    .migrate_graph(
                                        source,
                                        ProgramGraphMigrationTarget{
                                            version.id(), std::move(run_id), {}})
                                    .wait();
            return result.status() == ProgramTerminalStatus::Completed ? 1 : -1;
        } catch (const std::exception&) {
            return 0;
        }
    };
    auto first = std::async(std::launch::async, [&] {
        return migrate(*fixture.runtime, "graph-migration-race-first");
    });
    auto second = std::async(std::launch::async, [&] {
        return migrate(*second_runtime, "graph-migration-race-second");
    });
    ready.arrive_and_wait();

    EXPECT_EQ(first.get() + second.get(), 1);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 1U);
    const auto lineage = fixture.journal->load_run_lineage(
        "tenant:runtime", source.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    (void)source.wait();
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE_CHECKPOINT
TEST(ProgramRuntimeTest, GraphMigrationSurvivesSqliteRuntimeReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto id = sequence.fetch_add(1);
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-graph-migration-" +
          std::to_string(id) + ".db"))
            .string();
    const auto checkpoint_path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-graph-migration-checkpoints-" +
          std::to_string(id) + ".db"))
            .string();
    std::filesystem::remove(path);
    std::filesystem::remove(checkpoint_path);
    blocking_calls.store(0);
    followup_calls.store(0);

    {
        auto transitions = std::make_shared<SQLiteProgramTransitionStore>(path);
        auto checkpoints = std::make_shared<SqliteCheckpointStore>(checkpoint_path);
        AdmittedRuntime fixture(1, checkpoints, transitions);
        const auto version = fixture.admit("runtime-short-blocking");
        std::string source_run_id;
        std::string target_run_id;
        std::string target_result_id;
        {
            auto source = fixture.runtime->start(
                "tenant:runtime", version,
                ProgramInvocation{json::object(), grant(),
                                  "trace-sqlite-graph-migration", {}});
            const auto target = fixture.runtime
                                    ->migrate_graph(
                                        source,
                                        ProgramGraphMigrationTarget{
                                            version.id(), "sqlite-graph-migration-target", {}})
                                    .wait();
            ASSERT_EQ(target.status(), ProgramTerminalStatus::Completed);
            (void)source.wait();
            source_run_id = source.run_id();
            target_run_id = target.run_id();
            target_result_id = target.id();
        }
        EXPECT_EQ(blocking_calls.load(), 1U);
        EXPECT_EQ(followup_calls.load(), 1U);

        fixture.runtime.reset();
        fixture.journal.reset();
        fixture.checkpoints.reset();
        transitions.reset();
        checkpoints.reset();
        fixture.journal = std::make_shared<SQLiteProgramTransitionStore>(path);
        fixture.checkpoints = std::make_shared<SqliteCheckpointStore>(checkpoint_path);
        fixture.runtime = fixture.make_runtime();
        const auto lineage = fixture.journal->load_run_lineage(
            "tenant:runtime", source_run_id);
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 2U);
        const auto generation = fixture.journal->load_generation(
            "tenant:runtime", lineage->lineage_id(), 2);
        ASSERT_TRUE(generation);
        ASSERT_TRUE(generation->graph_migration_receipt());
        const auto capsule = fixture.journal->load_graph_migration_capsule(
            "tenant:runtime", source_run_id,
            generation->graph_migration_receipt()->capsule().source_lineage_head_id());
        ASSERT_TRUE(capsule);
        EXPECT_EQ(capsule->id(),
                  generation->graph_migration_receipt()->capsule().id());
        const auto recovered = fixture.runtime->reconnect(
            "tenant:runtime", source_run_id).wait();
        EXPECT_EQ(recovered.id(), target_result_id);
        EXPECT_EQ(recovered.run_id(), target_run_id);
        EXPECT_EQ(followup_calls.load(), 1U);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(checkpoint_path);
}
#endif
#endif

TEST(ProgramRuntimeTest, JavaScriptCheckpointReplacementPublishesAndReconnectsGenerationTwo) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const handoff = {cursor: 7, state: "ready"};
    yield ng.checkpoint(handoff, "replacement:boundary");
    yield ng.callCore("main", {}, "replacement:old-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {
        generation: "target",
        handoff: input.handoff,
        previous_run_id: input.previous_run_id
    };
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 7}, {"state", "ready"}}));
    const auto handoff_reference = handoff.reference();
    const auto source_boundary = source.snapshot();
    const auto source_lineage =
        journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_lineage);
    EXPECT_EQ(source_lineage->active_generation(), 1U);

    const auto target_input =
        json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};
    auto replacement_runtime = fixture.make_runtime();
    auto target = replacement_runtime->replace(
        "tenant:runtime", std::move(handoff), target_version,
        ProgramInvocation{target_input, source_boundary.remaining_budget(),
                          "trace-replacement-target", {}});
    const auto result = target.wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(),
              (json{{"generation", "target"},
                    {"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                    {"previous_run_id", source.run_id()}}));
    EXPECT_EQ(completed_calls.load(), 0U);

    const auto lineage = journal->load_run_lineage("tenant:runtime", target.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->lineage_id(), source_lineage->lineage_id());
    EXPECT_EQ(lineage->active_generation(), 2U);
    const auto target_record = journal->load("tenant:runtime", target.run_id());
    ASSERT_TRUE(target_record);
    EXPECT_EQ(target_record->invocation().budget,
              program_replacement_remaining_budget(source_boundary, *source_lineage,
                                                   target_record->created_at_ms()));
    EXPECT_LT(target_record->invocation().budget.wall_time_ms,
              source_boundary.remaining_budget().wall_time_ms);
    EXPECT_EQ(lineage->remaining_budget(), result.remaining_budget());
    const auto generation =
        journal->load_generation("tenant:runtime", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->replacement_receipt());
    EXPECT_EQ(generation->replacement_receipt()->source_run_id(), source.run_id());
    EXPECT_EQ(generation->replacement_receipt()->checkpoint_entry_id(),
              handoff_reference.command_entry_id);
    EXPECT_EQ(generation->replacement_receipt()->target_run_id(), target.run_id());

    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(reconnected.id(), result.id());
    EXPECT_EQ(reconnected.run_id(), target.run_id());
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ReplacementCommitBeforeDispatchRecoversExactSuccessor) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 11}, "replacement:crash-boundary");
    yield ng.callCore("main", {}, "replacement:stale-source");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const output = yield ng.callCore("main", input.handoff, "replacement:target");
    return {generation: "target", value: output.value, handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-crash-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    const auto source_boundary = source.snapshot();
    const auto target_input =
        json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};

    journal->crash_after_next_replacement();
    EXPECT_THROW(
        (void)fixture.runtime->replace(
            "tenant:runtime", std::move(handoff), target_version,
            ProgramInvocation{target_input, source_boundary.remaining_budget(),
                              "trace-replacement-crash-target", {}}),
        std::runtime_error);
    const auto committed_lineage =
        journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(committed_lineage);
    EXPECT_EQ(committed_lineage->active_generation(), 2U);
    EXPECT_EQ(completed_calls.load(), 0U);

    auto first_runtime  = fixture.make_runtime();
    auto second_runtime = fixture.make_runtime();
    std::barrier reconnect_ready(3);
    const auto reconnect = [&](ProgramRuntime& runtime) {
        reconnect_ready.arrive_and_wait();
        return runtime.reconnect("tenant:runtime", source.run_id());
    };
    auto first_reconnect =
        std::async(std::launch::async, [&] { return reconnect(*first_runtime); });
    auto second_reconnect =
        std::async(std::launch::async, [&] { return reconnect(*second_runtime); });
    reconnect_ready.arrive_and_wait();
    auto first_handle = first_reconnect.get();
    auto second_handle = second_reconnect.get();
    const auto recovered = first_handle.wait();
    const auto duplicate = second_handle.wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed)
        << (recovered.failure()
                ? recovered.failure()->code + ": " + recovered.failure()->message + " " +
                      recovered.failure()->witness.dump()
                : "no failure detail");
    EXPECT_EQ(recovered.output(),
              (json{{"generation", "target"},
                    {"value", "completed"},
                    {"handoff", json{{"cursor", 11}}}}));
    EXPECT_EQ(duplicate.id(), recovered.id());
    EXPECT_EQ(completed_calls.load(), 1U);
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, DifferentTransitionStoreCannotConsumeHandoffLease) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 23}, "handoff:wrong-store");
    yield ng.callCore("main", {}, "handoff:source-continues");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-wrong-store-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        const auto source_boundary = source.snapshot();
        const auto input =
            json{{"handoff", handoff.value()}, {"previous_run_id", source.run_id()}};
        AdmittedRuntime other_store(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
        EXPECT_THROW(
            (void)other_store.runtime->replace(
                "tenant:runtime", std::move(handoff), target_version,
                ProgramInvocation{input, source_boundary.remaining_budget(),
                                  "trace-handoff-wrong-store-target", {}}),
            std::invalid_argument);
        EXPECT_TRUE(handoff);
    }

    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, UnreadableCommittedReplacementNeverReleasesHeldSource) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 19}, "handoff:ambiguous-commit");
    yield ng.callCore("main", {}, "handoff:ambiguous-source-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    const output = yield ng.callCore("main", input.handoff, "handoff:ambiguous-target");
    return {generation: "target", value: output.value};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-ambiguous-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 19}}));
    const auto source_boundary = source.snapshot();

    journal->crash_after_next_replacement_with_unreadable_commit();
    EXPECT_THROW(
        (void)fixture.runtime->replace(
            "tenant:runtime", std::move(handoff), target_version,
            ProgramInvocation{
                json{{"handoff", json{{"cursor", 19}}},
                     {"previous_run_id", source.run_id()}},
                source_boundary.remaining_budget(), "trace-handoff-ambiguous-target", {}}),
        std::runtime_error);
    EXPECT_FALSE(handoff);
    const auto committed = journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed->active_generation(), 2U);

    auto recovery_runtime = fixture.make_runtime();
    const auto recovered = recovery_runtime->reconnect("tenant:runtime", source.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(recovered.output(), (json{{"generation", "target"}, {"value", "completed"}}));
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ReplacementFailureBeforeCommitLeavesSourceExecutable) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 12}, "replacement:failed-boundary");
    yield ng.callCore("main", {}, "replacement:source-continues");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-replacement-failed-source", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));

    const std::string target_run_id = "replacement-occupied-target";
    auto              collider_runtime = fixture.make_runtime();
    std::optional<ProgramHandle> collider;
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        EXPECT_EQ(handoff.value(), (json{{"cursor", 12}}));
        EXPECT_EQ(completed_calls.load(), 0U);
        journal->throw_before_next_replacement_with_occupied_target([&] {
            collider.emplace(collider_runtime->start(
                "tenant:runtime", target_version,
                ProgramInvocation{json{{"handoff", handoff.value()},
                                       {"previous_run_id", source.run_id()},
                                       {"collision", true}},
                                  javascript_budget(1, 4),
                                  "trace-replacement-collision", {}, target_run_id}));
        });
        EXPECT_THROW(
            (void)fixture.runtime->replace(
                "tenant:runtime", std::move(handoff), target_version,
                ProgramInvocation{
                    json{{"handoff", json{{"cursor", 12}}},
                         {"previous_run_id", source.run_id()}},
                    source.snapshot().remaining_budget(), "trace-replacement-failed-target", {},
                    target_run_id}),
            std::runtime_error);
        ASSERT_TRUE(collider);
        EXPECT_EQ(collider->wait().status(), ProgramTerminalStatus::Completed);
        const auto lineage = journal->load_run_lineage("tenant:runtime", source.run_id());
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 1U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"generation", "source"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, AbandonedHandoffLeaseResumesSourceExactlyOnce) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 14}, "handoff:abandon");
    yield ng.callCore("main", {}, "handoff:continued");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-abandon", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        EXPECT_THROW((void)source.next_handoff(), std::logic_error);
        auto reached = std::async(std::launch::async, [&] {
            return neograph::async::run_sync(handoff.wait_async());
        });
        journal->release_result();
        EXPECT_EQ(reached.get().value, (json{{"cursor", 14}}));
        EXPECT_FALSE(source.try_result());
        EXPECT_EQ(completed_calls.load(), 0U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"status", "continued"}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, DetachedAsyncHandoffWaitRemainsSafeAfterLeaseRelease) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 20}, "handoff:detached-async");
    yield ng.callCore("main", {}, "handoff:detached-continued");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-detached-async", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    std::optional<asio::awaitable<ExactProgramHandoff>> pending;
    {
        auto handoff = source.next_handoff();
        pending.emplace(handoff.wait_async());
    }
    journal->release_result();
    EXPECT_THROW((void)neograph::async::run_sync(std::move(*pending)), std::runtime_error);

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, HeldHandoffCannotBeBypassedBySecondRuntime) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 21}, "handoff:process-dedupe");
    yield ng.callCore("main", {}, "handoff:one-continuation");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-process-dedupe", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    {
        auto handoff = source.next_handoff();
        journal->release_result();
        EXPECT_EQ(handoff.value(), (json{{"cursor", 21}}));
        auto second_runtime = fixture.make_runtime();
        auto duplicate = second_runtime->reconnect("tenant:runtime", source.run_id());
        EXPECT_EQ(duplicate.run_id(), source.run_id());
        EXPECT_FALSE(duplicate.try_result());
        EXPECT_EQ(completed_calls.load(), 0U);
    }

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, HandoffRejectsAnAlreadyDuplicatedProcessControl) {
    blocking_calls.store(0);
    blocking_active.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-blocking", R"JS(
    yield ng.checkpoint({cursor: 22}, "handoff:duplicate-first");
    yield ng.callCore("main", {}, "handoff:blocking-continuation");
    return {status: "continued"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-duplicate-first", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto second_runtime = fixture.make_runtime();
    auto duplicate = second_runtime->reconnect("tenant:runtime", source.run_id());
    for (unsigned attempt = 0; blocking_active.load() == 0 && attempt < 2000; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(blocking_active.load(), 1U);
    EXPECT_THROW((void)source.next_handoff(), std::runtime_error);

    duplicate.cancel();
    source.cancel();
    journal->release_result();
    EXPECT_EQ(duplicate.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_NE(source.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, CancellationWakesHeldHandoffWithoutResumingJavaScript) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 15}, "handoff:cancel");
    yield ng.callCore("main", {}, "handoff:must-not-run");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-cancel", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 15}}));
    EXPECT_TRUE(source.cancel());

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, StoreReadFailureStillCancelsHeldHandoffLocally) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 16}, "handoff:store-failure");
    yield ng.callCore("main", {}, "handoff:must-not-run-after-store-failure");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-handoff-store-failure", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 16}}));
    journal->throw_on_next_load();
    EXPECT_TRUE(source.cancel());

    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RuntimeShutdownWakesHeldHandoff) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 17}, "handoff:shutdown");
    return {status: "unexpected"};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 3),
                          "trace-handoff-shutdown", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto handoff = source.next_handoff();
    journal->release_result();
    EXPECT_EQ(handoff.value(), (json{{"cursor", 17}}));

    fixture.runtime.reset();
    EXPECT_EQ(source.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, CheckpointEventCanSynchronouslyCommitArmedHandoff) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    auto sink    = std::make_shared<CallbackSink>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.callCore("main", {}, "handoff:event-prime");
    yield ng.checkpoint({cursor: 18}, "handoff:event-boundary");
    yield ng.callCore("main", {}, "handoff:event-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 6),
                          "trace-handoff-event-source", sink});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    auto replacement_runtime = fixture.make_runtime();
    auto handoff = std::make_shared<std::optional<ProgramHandoff>>(source.next_handoff());
    std::promise<ProgramHandle> replacement_promise;
    auto replacement_future = replacement_promise.get_future();
    auto invoked = std::make_shared<std::atomic_bool>(false);
    sink->set_callback([&, handoff, invoked](const ProgramEvent& event) {
        if (event.kind != ProgramEventKind::CheckpointPublished ||
            event.operation_id != "root.javascript.2" || invoked->exchange(true)) {
            return;
        }
        try {
            auto target = replacement_runtime->replace(
                "tenant:runtime", std::move(**handoff), target_version,
                ProgramInvocation{
                    json{{"handoff", json{{"cursor", 18}}},
                         {"previous_run_id", source.run_id()}},
                    source.snapshot().remaining_budget(), "trace-handoff-event-target", {}});
            handoff->reset();
            replacement_promise.set_value(std::move(target));
        } catch (...) {
            replacement_promise.set_exception(std::current_exception());
        }
    });

    journal->release_result();
    ASSERT_EQ(replacement_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto result = replacement_future.get().wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(),
              (json{{"generation", "target"}, {"handoff", json{{"cursor", 18}}}}));
    (void)source.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RecordedReplayCheckpointCannotAuthorizeLiveReplacement) {
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto source_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    yield ng.checkpoint({cursor: 13}, "replacement:recorded-boundary");
    yield ng.callCore("main", {}, "replacement:recorded-must-not-run");
    return {generation: "source"};
)JS"));
    const auto target_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed", R"JS(
    return {generation: "target", handoff: input.handoff};
)JS"));

    auto source = fixture.runtime->start_recorded(
        "tenant:runtime", source_version,
        ProgramInvocation{json::object(), javascript_budget(1, 4),
                          "trace-recorded-replacement-source", {}},
        RecordedBindingSet({}, {}, CatalogCapabilityBinding{}, {}));
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    const auto handoff = source.latest_handoff();
    ASSERT_TRUE(handoff);

    try {
        (void)fixture.runtime->replace(
            "tenant:runtime", handoff->reference, target_version,
            ProgramInvocation{
                json{{"handoff", handoff->value}, {"previous_run_id", source.run_id()}},
                source.snapshot().remaining_budget(), "trace-recorded-replacement-target", {}});
        FAIL() << "recorded replay checkpoint authorized a live replacement";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_REPLACEMENT_RECORDED_SOURCE");
    }

    journal->release_result();
    (void)source.wait();
}

TEST(ProgramRuntimeTest, JavaScriptPendingHeadFreshRuntimeResumesWithoutRedispatch) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-blocking"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:1");
                return {value: output.value};
            }
        )JS");

    // Keep the first Core dispatch in flight at the crash boundary.  A second
    // runtime is intentionally attached to the same durable transition store,
    // as a fresh process would be after the first one stopped after publishing
    // the yielded-command head.
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"requested", "crash"}}, grant(), "trace-javascript-crash-boundary", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);
    ASSERT_FALSE(original.try_result().has_value());

    const auto pending_entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(pending_entries.size(), 1U);
    ASSERT_TRUE(pending_entries.front().pending());

    auto       fresh_runtime = fixture.make_runtime();
    auto       recovered     = fresh_runtime->reconnect("tenant:runtime", original.run_id());
    const auto interrupted   = recovered.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.idempotency(), ProgramEffectIdempotency::NonIdempotent);
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto resumed = fresh_runtime
                             ->resume("tenant:runtime", original.run_id(),
                                      resume_for(interrupted, json{{"value", "resumed"}},
                                                 "trace-javascript-crash-resume"))
                             .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message + " " +
                                    resumed.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "resumed"}}));
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("output"), (json{{"value", "resumed"}}));

    // Stop the original process simulation after the fresh runtime has
    // durably completed.  Its result loses the transition race and must not
    // cause a second Core dispatch.
    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptControlRejectsUnknownCommandFieldsBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                yield {
                    protocol_version: 1,
                    op: "call_core",
                    name: "main",
                    input: {requested: input.requested},
                    unexpected: true,
                };
                return {};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-invalid", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

std::string javascript_runtime_source(std::string node_type, std::string body) {
    return "export function define() {\n"
           "  const graph = ng.graph(\"main\");\n"
           "  graph.channel(\"value\", {reducer: \"runtime-overwrite\", initial: \"\"});\n"
           "  graph.node(\"work\", {type: \"" +
           std::move(node_type) +
           "\"});\n"
           "  graph.entry(\"work\");\n"
           "  graph.exit(\"work\");\n"
           "  return graph;\n"
           "}\n\n"
           "export function* main(input) {\n" +
           std::move(body) + "\n}\n";
}

RunBudget javascript_budget(std::uint64_t max_concurrency,
                            std::uint64_t max_program_operations,
                            std::uint64_t max_child_depth,
                            std::uint64_t max_total_children) {
    return RunBudget{10000,
                     1000,
                     1000,
                     static_cast<std::uint32_t>(max_concurrency),
                     max_program_operations,
                     20,
                     0,
                     static_cast<std::uint32_t>(max_child_depth),
                     max_total_children};
}

TEST(ProgramRuntimeTest, JavaScriptBlockedCommandHonorsExternalCancellation) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    return yield ng.callCore("main", {}, "cancel:blocked");
)JS"));

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 1), "trace-js-cancel-blocked", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_EQ(result.usage().program_operations, 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCrashRecoveryDoesNotRecoverReservedResourceBudget) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    const first = yield ng.callCore("main", {}, "budget:first");
    yield ng.emit({unreachable: first.value}, "budget:second");
    return first;
)JS"));

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1, 1), "trace-js-budget-recovery", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto       fresh_runtime = fixture.make_runtime();
    const auto interrupted   = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    EXPECT_EQ(interrupted.remaining_budget().wall_time_ms, 0U);
    EXPECT_EQ(interrupted.remaining_budget().model_tokens, 0U);
    EXPECT_EQ(interrupted.remaining_budget().monetary_microunits, 0U);
    EXPECT_EQ(interrupted.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(interrupted.remaining_budget().max_program_operations, 0U);
    const auto durable = fixture.journal->latest("tenant:runtime", original.run_id());
    ASSERT_TRUE(durable.has_value());
    EXPECT_GT(durable->inflight_reservation.wall_time_ms, 0U);
    EXPECT_GT(durable->inflight_reservation.model_tokens, 0U);
    EXPECT_GT(durable->inflight_reservation.max_core_steps, 0U);
    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptAllJoinsActuallyOverlapCoreCommands) {
    blocking_calls.store(0);
    blocking_peak.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-overlap", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
}

TEST(ProgramRuntimeTest, JavaScriptAllKeepsFastInitialMembersAliveUntilJoinSetupFinishes) {
    completed_calls.store(0);
    AdmittedRuntime fixture(4, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const results = yield ng.all([
        ng.callCore("main", {}, "fast:first"),
        ng.callCore("main", {}, "fast:second")
    ], {max_in_flight: 2}, "fast:all");
    return results;
)JS"));

    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        const auto result =
            fixture.runtime->run("tenant:runtime", version,
                                 ProgramInvocation{json::object(),
                                                   javascript_budget(2),
                                                   "trace-js-fast-all-" + std::to_string(iteration),
                                                   {}});
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
            << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                                 : "no failure detail");
    }
    EXPECT_EQ(completed_calls.load(), 512U);
}

TEST(ProgramRuntimeTest, JavaScriptAllKeepsFastReplacementMembersAliveUntilLaunchesRegister) {
    completed_calls.store(0);
    AdmittedRuntime fixture(4, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const results = yield ng.all([
        ng.callCore("main", {}, "fast:first"),
        ng.callCore("main", {}, "fast:second"),
        ng.callCore("main", {}, "fast:third")
    ], {max_in_flight: 1}, "fast:all");
    return results;
)JS"));

    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        const auto result = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(),
                              javascript_budget(2),
                              "trace-js-fast-replacement-" + std::to_string(iteration),
                              {}});
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
            << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                                 : "no failure detail");
    }
    EXPECT_EQ(completed_calls.load(), 768U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinEnforcesMaxInFlightCap) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second"),
        ng.callCore("main", {}, "third")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(3), "trace-js-cap", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 3U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinValidatesEveryMemberBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.all([
        ng.callCore("main", {}, "valid:first"),
        ng.callCore("not-admitted", {}, "invalid:second")
    ], {max_in_flight: 2}, "validate");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-validate", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinReservesAggregateOperationBudgetBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.all([
        ng.callCore("main", {}, "budget:first"),
        ng.callCore("main", {}, "budget:second")
    ], {max_in_flight: 2}, "budget");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2, 2), "trace-js-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PROGRAM_OPERATION_BUDGET");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinUsesDeclarationOrderForResultsAndReadyRaceTies) {
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    const all_result = yield ng.all([
        ng.emit({id: 1}, "all:first"),
        ng.emit({id: 2}, "all:second")
    ], {max_in_flight: 2, collect: true}, "all");
    const race_result = yield ng.race([
        ng.emit({id: 1}, "race:first"),
        ng.emit({id: 2}, "race:second")
    ], "race");
    return {all: all_result, race: race_result};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-order", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"all", json::array({json{{"id", 1}}, json{{"id", 2}}})},
                                     {"race", json{{"id", 1}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptStructuredCommandReplaysAfterFreshRuntimeWithoutRedispatch) {
    completed_calls.store(0);
    auto            journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    const results = yield ng.all([
        ng.callCore("main", {member: 1}, "replay:first"),
        ng.callCore("main", {member: 2}, "replay:second")
    ], {max_in_flight: 2}, "replay:join");
    return {values: results.map((result) => result.value)};
)JS"));

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{
            json::object(), javascript_budget(2), "trace-js-structured-result-crash", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    EXPECT_EQ(completed_calls.load(), 2U);

    auto       fresh_runtime = fixture.make_runtime();
    const auto replayed      = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed)
        << (replayed.failure() ? replayed.failure()->code + ": " + replayed.failure()->message
                               : "no failure detail");
    EXPECT_EQ(replayed.output(), (json{{"values", json::array({"completed", "completed"})}}));
    EXPECT_EQ(completed_calls.load(), 2U);

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinCollectsFailuresInDeclarationOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-failing",
                                                                                 R"JS(
    const result = yield ng.all([
        ng.emit({id: "ok"}, "collect:ok"),
        ng.callCore("main", {}, "collect:failure")
    ], {max_in_flight: 2, collect: true}, "collect");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-collect", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.output(), json::array({json{{"id", "ok"}}, nullptr}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptAwaitTimeoutCancelsDirectCoreWork) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(javascript_runtime_source("runtime-blocking",
                                                                                 R"JS(
    const result = yield ng.await(ng.callCore("main", {}, "await:core"), 10, "await");
    return result;
)JS"));

    const auto started = std::chrono::steady_clock::now();
    const auto result  = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-await-timeout", {}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_LT(elapsed.count(), 2);
}

TEST(ProgramRuntimeTest, JavaScriptQuorumOrdersSuccessfulMembers) {
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-short-blocking",
                                                           R"JS(
    const result = yield ng.quorum([
        ng.callCore("main", {}, "quorum:blocked"),
        ng.emit({id: 1}, "quorum:first"),
        ng.emit({id: 2}, "quorum:second")
    ], {required_successes: 2, max_in_flight: 2}, "quorum");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-quorum", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"id", 1}}, json{{"id", 2}}}));
}

TEST(ProgramRuntimeTest, JavaScriptCancelScopeCancelsTheOwningRun) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                                            R"JS(
    yield ng.cancelScope("current", "stop", "scope stopped");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-cancel-scope", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
}

TEST(ProgramRuntimeTest, JavaScriptRaceCancelsLoserChildrenWithoutOrphans) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      parent_version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed",
                                                           R"JS(
    const winner = yield ng.race([
        ng.await(ng.spawn("child", {}, "spawn"), 5000, "await"),
        ng.emit({winner: true}, "winner")
    ], {max_in_flight: 2}, "race");
    return winner;
)JS"));
    const auto child_version = fixture.admit("runtime-blocking");
    const auto linked        = link_child_versions(fixture, parent_version, child_version,
                                                   BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1});

    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{
            json::object(), javascript_budget(2, 32, 1, 1), "trace-js-cancel-child", {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"winner", true}}));
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    EXPECT_EQ(children.front().state, ProgramChildState::Cancelled);
}

TEST(ProgramRuntimeTest, JavaScriptInterruptedCallCoreResumesItsExactPendingCoordinate) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.callCore("main", {}, "resume:call-core");
    return {value: output.value};
)JS"));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-exact-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_input.has_value());

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-js-exact-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "approved"}}));
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto commands =
        fixture.journal->load_javascript_commands("tenant:runtime", interrupted.run_id());
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_EQ(commands.front().coordinate_id(), commands.back().coordinate_id());
    EXPECT_TRUE(commands.back().completed());
}

TEST(ProgramRuntimeTest, JavaScriptExactResumeStillValidatesMalformedTerminalOutput) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    yield ng.callCore("main", {}, "resume:malformed-output");
    return "not-an-object";
)JS"),
                                 ContractRecord{1, json{{"type", "object"}}});

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-malformed-first", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted)
        << (interrupted.failure()
                ? interrupted.failure()->code + ": " + interrupted.failure()->message + " " +
                      interrupted.failure()->witness.dump()
                : "no failure detail");

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-js-malformed-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_OUTPUT_CONTRACT");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptRaceSelectionDoesNotReplenishLateMembers) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    const winner = yield ng.race([
        ng.emit({winner: 0}, "race:first"),
        ng.emit({winner: 1}, "race:second"),
        ng.callCore("main", {}, "race:must-not-launch")
    ], {max_in_flight: 2}, "race:no-replenish");
    return winner;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-race-fence", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"winner", 0}}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptMissingSpawnBindingPreflightBlocksEverySibling) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    yield ng.all([
        ng.callCore("main", {}, "spawn-preflight:valid"),
        ng.spawn("missing-child", {}, "spawn-preflight:missing")
    ], {max_in_flight: 2}, "spawn-preflight:join");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json::object(), javascript_budget(2, 32, 1, 1), "trace-js-spawn-preflight", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptAwaitedCallCoreResumesItsNestedExactCoordinate) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-interrupt", R"JS(
    const output = yield ng.await(
        ng.callCore("main", {}, "resume:nested-call-core"),
        undefined,
        "resume:nested-await");
    return {value: output.value};
)JS"));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-nested-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_input.has_value());

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "nested-approved"}},
                                                 "trace-js-nested-resume"))
                             .wait();

    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message
                              : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "nested-approved"}}));
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto events = fixture.journal->load_events("tenant:runtime", interrupted.run_id(), 0);
    const auto checkpoint_events = std::count_if(
        events.begin(), events.end(),
        [](const auto& event) { return event.kind == ProgramEventKind::CheckpointPublished; });
    EXPECT_EQ(checkpoint_events, 2);
}

TEST(ProgramRuntimeTest, JavaScriptEventSinkFailureSettlesPublishedCommandReservation) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version =
        fixture.admit_javascript(javascript_runtime_source("runtime-completed", R"JS(
    yield ng.emit({published: true}, "sink:emit");
    return {unreachable: true};
)JS"));
    auto sink = std::make_shared<ThrowingSink>(ProgramEventKind::Emit);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-sink", sink});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_EVENT_SINK");
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(journal->inflight_reservation, RunBudget{});
    const auto commands =
        fixture.journal->load_javascript_commands("tenant:runtime", result.run_id());
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_TRUE(commands.back().completed());
    EXPECT_LE(result.remaining_budget().wall_time_ms + result.usage().wall_time_ms,
              javascript_budget(1).wall_time_ms);
}
#endif

TEST(ProgramRuntimeTest, ProgramInterruptAndExactResumePreserveDirectCoreBehavior) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    const auto program_interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-program-interrupt", {}});
    const auto program_resumed =
        fixture.runtime
            ->resume("tenant:runtime", program_interrupted.run_id(),
                     resume_for(program_interrupted, json{{"decision", "approved"}},
                                "trace-program-resume"))
            .wait();

    const auto definition = program_document("runtime-interrupt")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    EngineResources engine_resources;
    engine_resources.registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();
    direct_engine->set_checkpoint_store(direct_checkpoints);

    RunConfig direct_config;
    direct_config.thread_id       = "direct-interrupt-equivalence";
    direct_config.input           = json::object();
    direct_config.max_steps       = 20;
    const auto direct_interrupted = direct_engine->run(direct_config);
    const auto direct_resumed     = direct_engine->resume_from(
        direct_config, direct_interrupted.checkpoint_id, json{{"decision", "approved"}});

    ASSERT_EQ(program_interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_EQ(direct_interrupted.status(), RunStatus::Interrupted);
    ASSERT_TRUE(program_interrupted.interrupt().has_value());
    EXPECT_EQ(program_interrupted.interrupt()->core_node, direct_interrupted.interrupt_node);
    EXPECT_EQ(program_interrupted.interrupt()->value, direct_interrupted.interrupt_value);
    EXPECT_EQ(program_interrupted.execution_trace(), direct_interrupted.execution_trace);
    EXPECT_EQ(program_resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_resumed.status(), RunStatus::Completed);
    EXPECT_EQ(program_resumed.output(), direct_resumed.output);
    EXPECT_EQ(program_resumed.execution_trace(), direct_resumed.execution_trace);

    ASSERT_TRUE(program_resumed.checkpoint().has_value());
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_resumed.checkpoint()->checkpoint_id);
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_resumed.checkpoint_id);
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(interrupt_calls.load(), 4U);
}

TEST(ProgramRuntimeTest, ExactResumeAtEndChargesNoAdditionalCoreStep) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document                          = program_document("runtime-completed");
    document["root"]["definition"]["interrupt_after"] = json::array({"work"});
    const auto version                                = fixture.admit_document(std::move(document));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-after", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json::object(), "trace-after-resume"))
                             .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.usage().core_steps, 0U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ExactResumeToInterruptBeforeChargesOnlyExecutedSteps) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document       = program_document("runtime-completed");
    auto            core           = document["root"]["definition"];
    core["nodes"]                  = json{{"a", json{{"type", "runtime-completed"}}},
                                          {"b", json{{"type", "runtime-completed"}}},
                                          {"c", json{{"type", "runtime-completed"}}}};
    core["edges"]                  = json::array({
        json{{"from", "__start__"}, {"to", "a"}},
        json{{"from", "a"}, {"to", "b"}},
        json{{"from", "b"}, {"to", "c"}},
        json{{"from", "c"}, {"to", "__end__"}},
    });
    core["interrupt_after"]        = json::array({"a"});
    core["interrupt_before"]       = json::array({"c"});
    document["root"]["definition"] = std::move(core);
    const auto version             = fixture.admit_document(std::move(document));
    auto       budget              = grant();
    budget.max_core_steps          = 3;

    const auto first =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-sequence-1", {}});
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(first.usage().core_steps, 1U);
    EXPECT_EQ(first.remaining_budget().max_core_steps, 2U);

    const auto second = fixture.runtime
                            ->resume("tenant:runtime", first.run_id(),
                                     resume_for(first, json::object(), "trace-sequence-2"))
                            .wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(second.usage().core_steps, 1U);
    EXPECT_EQ(second.remaining_budget().max_core_steps, 1U);

    const auto third = fixture.runtime
                           ->resume("tenant:runtime", second.run_id(),
                                    resume_for(second, json::object(), "trace-sequence-3"))
                           .wait();
    EXPECT_EQ(third.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(third.usage().core_steps, 1U);
    EXPECT_EQ(third.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(completed_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, InterruptAndAsyncResumeUseOnePublishedCheckpointAndOneDispatchEach) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    auto interrupted_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-interrupt", {}});
    const auto interrupted = interrupted_handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed = neograph::async::run_sync(fixture.runtime->resume_async(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-resume")));

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.run_id(), interrupted.run_id());
    EXPECT_EQ(resumed.program_version_id(), version.id());
    EXPECT_EQ(resumed.attempt(), 2U);
    EXPECT_EQ(interrupt_calls.load(), 2U);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    ASSERT_TRUE(resumed.checkpoint().has_value());
    EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
    const auto interrupted_budget = interrupted.remaining_budget();
    const auto resumed_budget     = resumed.remaining_budget();
    EXPECT_LE(resumed_budget.wall_time_ms, interrupted_budget.wall_time_ms);
    EXPECT_LE(resumed_budget.model_tokens, interrupted_budget.model_tokens);
    EXPECT_EQ(interrupted_budget.max_program_operations, 0U);
    EXPECT_EQ(resumed_budget.max_program_operations, 0U);
    EXPECT_EQ(interrupted_budget.max_concurrency, 1U);
    EXPECT_EQ(resumed_budget.max_concurrency, 1U);
    EXPECT_LE(resumed_budget.monetary_microunits, interrupted_budget.monetary_microunits);
    EXPECT_LE(resumed_budget.max_concurrency, interrupted_budget.max_concurrency);
    EXPECT_LE(resumed_budget.max_program_operations, interrupted_budget.max_program_operations);
    EXPECT_LE(resumed_budget.max_core_steps, interrupted_budget.max_core_steps);
    EXPECT_LE(resumed_budget.max_dynamic_compiles, interrupted_budget.max_dynamic_compiles);
    EXPECT_LE(resumed_budget.max_child_depth, interrupted_budget.max_child_depth);
    EXPECT_LE(resumed_budget.max_total_children, interrupted_budget.max_total_children);

    const auto journal = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, AsyncWaitFromTemporaryHandleRetainsRunControl) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto pending =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-temporary-handle", {}})
            .wait_async();
    const auto result = neograph::async::run_sync(std::move(pending));

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
}

TEST(ProgramRuntimeTest, RepeatedAsyncWaitNeverMissesConcurrentCompletion) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    for (int iteration = 0; iteration < 2000; ++iteration) {
        auto pending =
            fixture.runtime
                ->start("tenant:runtime", version,
                        ProgramInvocation{json::object(), grant(), "trace-wait-race", {}})
                .wait_async();
        const auto result = neograph::async::run_sync(std::move(pending));
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << iteration;
    }
}

TEST(ProgramRuntimeTest, ConcurrentResumeHasOneWinnerAndOneCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-race-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    std::barrier ready(3);
    const auto   resume_once = [&](std::string decision) {
        ready.arrive_and_wait();
        try {
            auto handle = fixture.runtime->resume(
                "tenant:runtime", interrupted.run_id(),
                resume_for(interrupted, json{{"decision", std::move(decision)}}, "trace-race"));
            return handle.wait().status() == ProgramTerminalStatus::Completed ? 1 : -1;
        } catch (const ProgramDiagnosticError& error) {
            const auto& code = error.diagnostic().code;
            return code == "P_RESUME_CONFLICT" || code == "P_RESUME_STATE" ? 0 : -2;
        } catch (...) {
            return -3;
        }
    };

    auto first  = std::async(std::launch::async, [&] { return resume_once("first"); });
    auto second = std::async(std::launch::async, [&] { return resume_once("second"); });
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();

    EXPECT_EQ(first_result + second_result, 1);
    EXPECT_NE(first_result, second_result);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, EventSinkFailureCannotRewriteAnAtomicallyCommittedTerminalTransition) {
    for (const auto fail_on : {ProgramEventKind::Started, ProgramEventKind::Core,
                               ProgramEventKind::CheckpointPublished, ProgramEventKind::Terminal}) {
        AdmittedRuntime fixture;
        const auto      version = fixture.admit("runtime-completed");
        auto            sink    = std::make_shared<ThrowingSink>(fail_on);

        auto handle =
            fixture.runtime->start("tenant:runtime", version,
                                   ProgramInvocation{json::object(), grant(), "trace-sink", sink});
        const auto result = handle.wait();
        const bool pre_commit =
            fail_on == ProgramEventKind::Started || fail_on == ProgramEventKind::Core;

        EXPECT_EQ(result.status(),
                  pre_commit ? ProgramTerminalStatus::Failed : ProgramTerminalStatus::Completed);
        EXPECT_EQ(result.failure().has_value(), pre_commit);
        if (pre_commit) EXPECT_EQ(result.failure()->code, "P_EVENT_SINK");
        EXPECT_EQ(result.usage().program_operations, 1U);
        EXPECT_EQ(result.usage().core_steps, fail_on == ProgramEventKind::Started ? 0U : 1U);
        EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
        EXPECT_EQ(result.remaining_budget().max_core_steps,
                  fail_on == ProgramEventKind::Started ? 20U : 19U);

        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state,
                  pre_commit ? ContinuationState::Failed : ContinuationState::Completed);

        const auto events   = handle.events_after(0);
        const auto terminal = std::find_if(
            events.begin(), events.end(),
            [](const ProgramEvent& event) { return event.kind == ProgramEventKind::Terminal; });
        ASSERT_NE(terminal, events.end());
        EXPECT_EQ(std::get<ProgramTerminalEvent>(terminal->payload).status, result.status());
        EXPECT_EQ(std::count_if(events.begin(), events.end(),
                                [](const ProgramEvent& event) {
                                    return event.kind == ProgramEventKind::Terminal;
                                }),
                  1);

        const auto terminal_count =
            std::count(sink->seen.begin(), sink->seen.end(), ProgramEventKind::Terminal);
        EXPECT_LE(terminal_count, 1);
    }
}

TEST(ProgramRuntimeTest, ResumeStartedSinkFailurePreservesPublishedCheckpointAndJournal) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-resume-sink-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    auto sink = std::make_shared<ThrowingSink>(ProgramEventKind::Started);

    const auto failed = fixture.runtime
                            ->resume("tenant:runtime", interrupted.run_id(),
                                     resume_for(interrupted, json{{"decision", "unused"}},
                                                "trace-resume-sink-failed", sink))
                            .wait();

    EXPECT_EQ(failed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(failed.failure().has_value());
    EXPECT_EQ(failed.failure()->code, "P_EVENT_SINK");
    ASSERT_TRUE(failed.checkpoint().has_value());
    EXPECT_EQ(failed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", failed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
}

TEST(ProgramRuntimeTest, InterruptedCheckpointSinkFailurePreservesCommittedResumeState) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            sink    = std::make_shared<ThrowingSink>(ProgramEventKind::CheckpointPublished);

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupt-sink", sink});

    EXPECT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_FALSE(interrupted.failure().has_value());
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Interrupted);
    EXPECT_EQ(latest->continuation.attempt, 1U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-resume-after-observer-failure"))
                             .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JournalLatestFailureIsContainedAsTerminalPublicationFailure) {
    auto            journal = std::make_shared<ThrowOnLatestJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-read-failure", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JOURNAL_CONFLICT");
    const auto stored = journal->stored_latest(result.run_id());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->continuation.state, ContinuationState::Running);
    const auto events = handle.events_after(0);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished ||
                                       event.kind == ProgramEventKind::Terminal;
                            }),
              0);
}

TEST(ProgramRuntimeTest, ResumeUsesRequestedCheckpointWhenANewerOrphanExists) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-orphan", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id         = Checkpoint::generate_id();
    orphan->parent_id  = interrupted.checkpoint()->checkpoint_id;
    orphan->next_nodes = {"__end__"};
    orphan->step += 1;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "exact"}}, "trace-orphan-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "exact");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, MissingPublishedCheckpointIsTypedIncompatibleWithoutCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-missing", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    fixture.checkpoints->delete_thread(interrupted.checkpoint()->core_thread_id);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-missing-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, MismatchedPublishedCheckpointIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-mismatch", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::WrongThread);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-mismatch-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CheckpointDeletedAfterPrecheckIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-race", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterFirstExactLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-race-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
}

TEST(ProgramRuntimeTest, PostPrecheckIdentityChangesPreservePublishedCheckpoint) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongThreadAfterPrecheck,
                            AdversarialCheckpointStore::Mode::WrongSchemaAfterPrecheck}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-post-precheck", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-post-precheck-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
        EXPECT_EQ(resumed.checkpoint()->checkpoint_schema_version,
                  interrupted.checkpoint()->checkpoint_schema_version);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
        ASSERT_TRUE(latest->core_checkpoint.has_value());
        EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    }
}

TEST(ProgramRuntimeTest, CoreExactResumeRejectsIdentityChangedAtConsumingLoad) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongIdAtCoreLoad,
                            AdversarialCheckpointStore::Mode::WrongSchemaAtCoreLoad}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-core-identity", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-core-identity-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    }
}

TEST(ProgramRuntimeTest, FailedResumeUsesAttemptLocalCoreStepAccounting) {
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version     = fixture.admit_document(std::move(document));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failure-before", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterCoreLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-failure-after"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutChargesOneCoreSuperstep) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit_document(failing_fanout_program_document());

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failing-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutCountsLegalDoubleUnderscoreNodeNames) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version =
        fixture.admit_document(failing_fanout_program_document("__completed", "__failing"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-prefixed-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedExactResumeIgnoresNewerOrphanForUsage) {
    AdmittedRuntime fixture;
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));
    const auto interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-orphan-source", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id = "unpublished-newer-orphan";
    orphan->step += 10;
    orphan->timestamp += 10;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-orphan-failure"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, CoreFailureIsClassifiedWithNodeAndAttempt) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-failing");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-failure", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->core_node, "work");
    EXPECT_EQ(result.failure()->attempts, 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, CoreStepLimitMapsToBudgetExhausted) {
    completed_calls.store(0);

    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(step_limited_program_document());
    auto            budget  = grant();
    budget.max_core_steps   = 1;

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-step-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::BudgetExhausted);
}

TEST(ProgramRuntimeTest, RejectsCoreStepBudgetThatCannotFitCoreRunConfig) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            budget  = grant();
    budget.max_core_steps   = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RejectsWallTimeBudgetThatCannotFitTimerDuration) {
    completed_calls.store(0);
    AdmittedRuntime       fixture;
    const auto            maximum = std::numeric_limits<std::uint64_t>::max();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("runtime-policy-large-wall")
        .semantic_version("1.0.0")
        .owner_scope("tenant:runtime")
        .admission_profile(fixture.profile)
        .budget_ceiling(BudgetLimits{maximum, 1000, 1000, 1, 1, 20, 1, 1, 1});
    fixture.policy                                         = std::move(policy_builder).build();
    auto document                                          = program_document("runtime-completed");
    document["declared_budget_requirements"][0]["maximum"] = maximum;
    const auto version  = fixture.admit_document(std::move(document));
    auto       budget   = grant();
    budget.wall_time_ms = maximum;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-wall-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, WallTimeBudgetCancelsCoreAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-timeout", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_FALSE(handle.cancel());
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ImmediateCancellationSuppressesAllLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-cancel-immediate", {}});

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_LE(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationWinsAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-cancel", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    handle.cancel();
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, HostAdmissionQueuesProgramAttemptsUntilLeaseRelease) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-short-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});

    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext& context) {
        EXPECT_EQ(context.owner_scope, "tenant:runtime");
        EXPECT_EQ(context.operation_id, "root");
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        request.priority             = 17;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-first", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto second =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-second", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(host->snapshot().queued, 1U);

    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(second.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(followup_calls.load(), 2U);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionDeadlineCancelsQueuedAttemptBeforeCoreDispatch) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-timeout-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-host-timeout-holder", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto budget         = grant();
    budget.wall_time_ms = 30;
    auto queued         = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-host-timeout-queued", {}});
    const auto queued_result = queued.wait();

    EXPECT_EQ(queued_result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued_result.failure().has_value());
    EXPECT_EQ(queued_result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued_result.usage().core_steps, 0U);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_TRUE(host->snapshot().reserved.cpu_millis == 1);
    EXPECT_EQ(host->snapshot().queued, 0U);

    EXPECT_TRUE(first.cancel());
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionRequiresControllerAndResolverTogether) {
    AdmittedRuntime fixture;
    auto            host = std::make_shared<neograph::HostAdmissionController>();

    RuntimeConfig controller_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    controller_only.host_admission = host;
    EXPECT_THROW({ ProgramRuntime runtime(std::move(controller_only)); }, std::invalid_argument);

    RuntimeConfig resolver_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    resolver_only.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        return neograph::HostAdmissionRequest{};
    };
    EXPECT_THROW({ ProgramRuntime runtime(std::move(resolver_only)); }, std::invalid_argument);
}

TEST(ProgramRuntimeTest, QueueWaitCountsAgainstWallTimeBudget) {
    scheduler_blocking_calls.store(0);
    completed_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      blocker_version = fixture.admit("runtime-scheduler-blocking");
    const auto      queued_version  = fixture.admit("runtime-completed");

    auto blocker_budget         = grant();
    blocker_budget.wall_time_ms = 5000;
    auto blocker                = fixture.runtime->start(
        "tenant:runtime", blocker_version,
        ProgramInvocation{json::object(), blocker_budget, "trace-queue-blocker", {}});
    for (int i = 0; i < 100 && scheduler_blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(scheduler_blocking_calls.load(), 1U);

    auto queued_budget         = grant();
    queued_budget.wall_time_ms = 20;
    const auto queued =
        fixture.runtime->run("tenant:runtime", queued_version,
                             ProgramInvocation{json::object(), queued_budget, "trace-queued", {}});

    EXPECT_EQ(queued.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued.failure().has_value());
    EXPECT_EQ(queued.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued.usage().core_steps, 0U);
    EXPECT_EQ(queued.remaining_budget().wall_time_ms, 0U);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_EQ(blocker.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, WallTimeExpiresWhenSingleSchedulerThreadIsBlocked) {
    scheduler_blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-scheduler-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-scheduler-blocked", {}});

    EXPECT_EQ(scheduler_blocking_calls.load(), 1U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, budget.max_core_steps - 1);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationRemainsFirstCauseAfterTimeoutAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), budget, "trace-user-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(stubborn_calls.load(), 1U);
    ASSERT_TRUE(handle.cancel());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, TimeoutRemainsFirstCauseAfterUserCancellationAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 30;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-timeout-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A 30ms budget may expire before the scheduler reaches the first node
    // on a cold or sanitizer-instrumented process. That is still the
    // timeout-first path under test; the user-first companion above covers
    // the case where the stubborn node has entered.
    EXPECT_LE(stubborn_calls.load(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, HandleCopiesShareOneResultAndRuntimeTeardownDrainsWork) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-teardown", sink});
    auto second = first;

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    fixture.runtime.reset();

    const auto first_result  = first.wait();
    const auto second_result = second.wait();
    EXPECT_EQ(first_result.run_id(), second_result.run_id());
    EXPECT_EQ(first_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(second_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto callbacks_after_teardown = sink->calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(sink->calls.load(), callbacks_after_teardown);
}

TEST(ProgramRuntimeTest, MoveAssignmentDrainsTheReplacedRuntime) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-move-assign", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    ProgramRuntime replacement(
        RuntimeConfig{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1});
    *fixture.runtime = std::move(replacement);

    const auto result = handle.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ConcurrentAttemptsShareConfiguredSchedulerWithoutCrossTalk) {
    completed_calls.store(0);
    AdmittedRuntime            fixture(4);
    const auto                 version = fixture.admit("runtime-completed");
    std::vector<ProgramHandle> handles;
    handles.reserve(24);
    for (int i = 0; i < 24; ++i) {
        handles.push_back(fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{
                json::object(), grant(), "trace-concurrent-" + std::to_string(i), {}}));
    }

    for (auto& handle : handles) {
        const auto result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    }
    EXPECT_EQ(completed_calls.load(), 24U);
}

TEST(ProgramRuntimeTest, RuntimeShutdownPreservesDeliveredInterruptedRun) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-shutdown-interrupt", {}});
    const auto interrupted = handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    fixture.recreate_catalog_and_runtime();

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(persisted->terminal_result().has_value());
    EXPECT_EQ(persisted->terminal_result()->status(), ProgramTerminalStatus::Interrupted);
    EXPECT_TRUE(persisted->pending_input().has_value());
}

TEST(ProgramRuntimeTest, ExactForkAfterRestartResumesPublishedCheckpointAndPersistsLineage) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    ASSERT_EQ(interrupt_calls.load(), 1U);
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record.has_value());
    ASSERT_EQ(source_record->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(source_record->pending_input().has_value());
    const auto source_pending_id = source_record->pending_input()->call_id();
    const auto source_lineage_before =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_lineage_before);
    ASSERT_EQ(source_lineage_before->active_generation(), 1U);

    fixture.recreate_catalog_and_runtime();
    RunInvocation fork_invocation;
    fork_invocation.owner_scope        = "tenant:runtime";
    fork_invocation.agent_id           = "test-fork";
    fork_invocation.program_version_id = version.id();
    fork_invocation.run_id             = "fork-target-run";
    fork_invocation.budget             = source.remaining_budget();
    --fork_invocation.budget.wall_time_ms;
    fork_invocation.input              = json::object();
    fork_invocation.message_sequence   = 19;
    fork_invocation.idempotency_key    = "test-fork:19";
    fork_invocation.correlation_id     = "trace-fork-target";
    fork_invocation.validate();
    const auto forked =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();

    EXPECT_EQ(forked.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(forked.run_id(), "fork-target-run");
    EXPECT_NE(forked.run_id(), source.run_id());
    EXPECT_EQ(forked.output()["channels"]["value"]["value"], "forked");
    ASSERT_TRUE(forked.checkpoint().has_value());
    EXPECT_NE(forked.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto retried =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();
    EXPECT_EQ(retried.run_id(), forked.run_id());
    EXPECT_EQ(retried.status(), forked.status());
    EXPECT_EQ(interrupt_calls.load(), 2U);
    EXPECT_THROW((void)fixture.runtime->fork(
                     ExactProgramCheckpointReference{source.run_id(),
                                                     source.checkpoint()->checkpoint_id},
                     fork_invocation,
                     ProgramResume{json{{"decision", "different"}}, "trace-fork-conflict", {},
                                   source_pending_id}),
                 ProgramDiagnosticError);

    const auto target_record = fixture.journal->load("tenant:runtime", forked.run_id());
    ASSERT_TRUE(target_record.has_value());
    ASSERT_TRUE(target_record->fork_receipt().has_value());
    EXPECT_TRUE(target_record->fork_receipt()->compatible());
    ASSERT_TRUE(target_record->fork_receipt()->initial_resume_binding().has_value());
    EXPECT_EQ(target_record->fork_receipt()->initial_resume_binding()->target_pending_id,
              source_pending_id);
    EXPECT_TRUE(target_record->fork_receipt()->matches_initial_resume(
        source_pending_id, json{{"decision", "forked"}}));
    EXPECT_EQ(target_record->fork_source_run_id(), source.run_id());
    EXPECT_EQ(target_record->fork_source_program_version_id(), version.id());
    EXPECT_EQ(target_record->fork_source_checkpoint_id(), source.checkpoint()->checkpoint_id);
    EXPECT_EQ(target_record->invocation(), fork_invocation);

    const auto source_lineage_after =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    const auto target_lineage =
        fixture.journal->load_run_lineage("tenant:runtime", forked.run_id());
    ASSERT_TRUE(source_lineage_after);
    ASSERT_TRUE(target_lineage);
    EXPECT_EQ(source_lineage_after->lineage_id(), source_lineage_before->lineage_id());
    EXPECT_NE(target_lineage->lineage_id(), source_lineage_after->lineage_id());
    EXPECT_EQ(source_lineage_after->active_generation(), 1U);
    EXPECT_EQ(target_lineage->active_generation(), 1U);
    const auto source_generation = fixture.journal->load_generation(
        "tenant:runtime", source_lineage_after->lineage_id(), 1);
    const auto target_generation = fixture.journal->load_generation(
        "tenant:runtime", target_lineage->lineage_id(), 1);
    ASSERT_TRUE(source_generation);
    ASSERT_TRUE(target_generation);
    EXPECT_EQ(source_generation->run_id(), source.run_id());
    EXPECT_EQ(target_generation->run_id(), forked.run_id());
    auto expected_source_budget = source.remaining_budget();
    expected_source_budget.wall_time_ms -= fork_invocation.budget.wall_time_ms;
    expected_source_budget.model_tokens -= fork_invocation.budget.model_tokens;
    expected_source_budget.monetary_microunits -= fork_invocation.budget.monetary_microunits;
    expected_source_budget.max_concurrency -= fork_invocation.budget.max_concurrency;
    expected_source_budget.max_program_operations -=
        fork_invocation.budget.max_program_operations;
    expected_source_budget.max_core_steps -= fork_invocation.budget.max_core_steps;
    expected_source_budget.max_dynamic_compiles -= fork_invocation.budget.max_dynamic_compiles;
    expected_source_budget.max_child_depth -= fork_invocation.budget.max_child_depth;
    expected_source_budget.max_total_children -= fork_invocation.budget.max_total_children;
    EXPECT_EQ(source_lineage_after->remaining_budget(), expected_source_budget);
    EXPECT_EQ(source_lineage_after->remaining_budget().wall_time_ms, 1U);
    EXPECT_EQ(target_lineage->remaining_budget(), target_record->remaining_budget());

    const auto source_after = fixture.runtime->reconnect("tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(source_after.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(source_after.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
    const auto resumed_source =
        fixture.runtime
            ->resume("tenant:runtime", source.run_id(),
                     ProgramResume{json{{"decision", "source"}}, "trace-source-after-fork", {},
                                   source_pending_id})
            .wait();
    EXPECT_NE(resumed_source.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto retried_after_source_progress =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();
    EXPECT_EQ(retried_after_source_progress.id(), forked.id());
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, ForkRetryAfterLaterInterruptUsesImmutableInitialResumeBinding) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(two_interrupt_program_document());
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-later-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto source_pending_id = pending_call_id(source);

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-fork-later-interrupt";
    invocation.program_version_id = version.id();
    invocation.run_id             = "fork-later-interrupt-target";
    invocation.budget             = source.remaining_budget();
    invocation.input              = json::object();
    invocation.message_sequence   = 23;
    invocation.idempotency_key    = "test-fork-later-interrupt:23";
    invocation.correlation_id     = "trace-fork-later-target";
    invocation.validate();
    const ExactProgramCheckpointReference exact_source{source.run_id(),
                                                       source.checkpoint()->checkpoint_id};
    const ProgramResume                   initial_resume{
        json{{"decision", "first"}}, "trace-fork-later-resume", {}, source_pending_id};

    const auto forked = fixture.runtime->fork(exact_source, invocation, initial_resume).wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Interrupted);
    const auto later_pending_id = pending_call_id(forked);
    EXPECT_NE(later_pending_id, source_pending_id);
    EXPECT_EQ(interrupt_calls.load(), 3U);

    fixture.recreate_catalog_and_runtime();
    const auto retried = fixture.runtime->fork(exact_source, invocation, initial_resume).wait();
    EXPECT_EQ(retried.id(), forked.id());
    EXPECT_EQ(retried.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(pending_call_id(retried), later_pending_id);
    EXPECT_EQ(interrupt_calls.load(), 3U);

    const auto expect_conflict = [&](ProgramResume resume) {
        try {
            (void)fixture.runtime->fork(exact_source, invocation, std::move(resume));
            FAIL() << "Conflicting fork retry unexpectedly reconnected";
        } catch (const ProgramDiagnosticError& error) {
            EXPECT_EQ(error.diagnostic().code, "P_RUN_CONFLICT");
        }
    };
    expect_conflict(ProgramResume{
        json{{"decision", "changed"}}, "trace-fork-later-value", {}, source_pending_id});
    expect_conflict(
        ProgramResume{json{{"decision", "first"}}, "trace-fork-later-id", {}, later_pending_id});
}

TEST(ProgramRuntimeTest, ConcurrentExactForksAtomicallyDebitSourceOnce) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-race-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record);
    ASSERT_TRUE(source_record->pending_input());
    const auto pending_id = source_record->pending_input()->call_id();

    fixture.recreate_catalog_and_runtime();
    const auto make_invocation = [&](std::string run_id) {
        RunInvocation invocation;
        invocation.owner_scope        = "tenant:runtime";
        invocation.agent_id           = "test-fork-race";
        invocation.program_version_id = version.id();
        invocation.run_id             = std::move(run_id);
        invocation.budget             = source.remaining_budget();
        invocation.input              = json::object();
        invocation.message_sequence   = 20;
        invocation.idempotency_key    = "test-fork-race:" + invocation.run_id;
        invocation.correlation_id     = "trace-" + invocation.run_id;
        invocation.validate();
        return invocation;
    };
    std::barrier ready(3);
    auto fork = [&](std::string run_id) {
        ready.arrive_and_wait();
        try {
            const auto result = fixture.runtime
                                    ->fork(ExactProgramCheckpointReference{
                                               source.run_id(),
                                               source.checkpoint()->checkpoint_id},
                                           make_invocation(run_id),
                                           ProgramResume{json{{"decision", run_id}},
                                                         "trace-fork-race-resume", {}, pending_id})
                                    .wait();
            return result.status() == ProgramTerminalStatus::Completed;
        } catch (const ProgramDiagnosticError&) {
            return false;
        }
    };
    auto first  = std::async(std::launch::async, [&] { return fork("fork-race-first"); });
    auto second = std::async(std::launch::async, [&] { return fork("fork-race-second"); });
    ready.arrive_and_wait();
    const auto first_won  = first.get();
    const auto second_won = second.get();

    EXPECT_NE(first_won, second_won);
    const auto first_record = fixture.journal->load("tenant:runtime", "fork-race-first");
    const auto second_record = fixture.journal->load("tenant:runtime", "fork-race-second");
    EXPECT_NE(first_record.has_value(), second_record.has_value());
    const auto winner_id = first_record ? "fork-race-first" : "fork-race-second";
    const auto source_lineage =
        fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
    const auto target_lineage = fixture.journal->load_run_lineage("tenant:runtime", winner_id);
    ASSERT_TRUE(source_lineage);
    ASSERT_TRUE(target_lineage);
    EXPECT_EQ(source_lineage->active_generation(), 1U);
    EXPECT_EQ(source_lineage->remaining_budget(), RunBudget{});
    EXPECT_EQ(target_lineage->active_generation(), 1U);
    EXPECT_NE(target_lineage->lineage_id(), source_lineage->lineage_id());
    EXPECT_EQ(fixture.journal
                  ->load_generation("tenant:runtime", target_lineage->lineage_id(), 1)
                  ->run_id(),
              winner_id);
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, ConcurrentIdenticalForksReconnectOneTargetDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-identical-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());
    const auto pending_id = pending_call_id(source);

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-identical-fork";
    invocation.program_version_id = version.id();
    invocation.run_id             = "identical-fork-target";
    invocation.budget             = source.remaining_budget();
    invocation.input              = json::object();
    invocation.message_sequence   = 22;
    invocation.idempotency_key    = "test-identical-fork:22";
    invocation.correlation_id     = "trace-identical-fork-target";
    invocation.validate();

    std::barrier ready(3);
    auto fork = [&] {
        ready.arrive_and_wait();
        return fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   invocation,
                   ProgramResume{json{{"decision", "same"}}, "trace-identical-fork-resume", {},
                                 pending_id})
            .wait();
    };
    auto first  = std::async(std::launch::async, fork);
    auto second = std::async(std::launch::async, fork);
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();
    EXPECT_EQ(first_result.run_id(), "identical-fork-target");
    EXPECT_EQ(second_result.run_id(), "identical-fork-target");
    EXPECT_EQ(first_result.id(), second_result.id());
    EXPECT_EQ(interrupt_calls.load(), 2U);
    const auto events = fixture.journal->load_events("tenant:runtime", "identical-fork-target");
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const ProgramEvent& event) {
                  return event.kind == ProgramEventKind::Started;
              }),
              1);
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramRuntimeTest, ExactForkLineageSurvivesSqliteRuntimeReconnect) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-runtime-fork-lineage-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    interrupt_calls.store(0);
    {
        auto transitions = std::make_shared<SQLiteProgramTransitionStore>(path);
        AdmittedRuntime fixture(1, {}, transitions);
        const auto      version = fixture.admit("runtime-interrupt");
        const auto      source =
            fixture.runtime
                ->start("tenant:runtime", version,
                        ProgramInvocation{json::object(), grant(), "trace-sqlite-fork-source", {}})
                .wait();
        ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(source.checkpoint());

        RunInvocation invocation;
        invocation.owner_scope        = "tenant:runtime";
        invocation.agent_id           = "test-sqlite-fork";
        invocation.program_version_id = version.id();
        invocation.run_id             = "sqlite-fork-target";
        invocation.budget             = source.remaining_budget();
        invocation.input              = json::object();
        invocation.message_sequence   = 21;
        invocation.idempotency_key    = "test-sqlite-fork:21";
        invocation.correlation_id     = "trace-sqlite-fork-target";
        invocation.validate();
        const auto target =
            fixture.runtime
                ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                       source.checkpoint()->checkpoint_id},
                       invocation,
                       ProgramResume{json{{"decision", "sqlite"}}, "trace-sqlite-fork-resume", {},
                                     pending_call_id(source)})
                .wait();
        ASSERT_EQ(target.status(), ProgramTerminalStatus::Completed);

        fixture.runtime.reset();
        fixture.journal.reset();
        fixture.journal = std::make_shared<SQLiteProgramTransitionStore>(path);
        fixture.runtime = fixture.make_runtime();
        const auto lineage =
            fixture.journal->load_run_lineage("tenant:runtime", target.run_id());
        ASSERT_TRUE(lineage);
        const auto source_lineage =
            fixture.journal->load_run_lineage("tenant:runtime", source.run_id());
        ASSERT_TRUE(source_lineage);
        EXPECT_EQ(lineage->active_generation(), 1U);
        EXPECT_EQ(source_lineage->active_generation(), 1U);
        EXPECT_NE(source_lineage->lineage_id(), lineage->lineage_id());
        EXPECT_EQ(source_lineage->remaining_budget(), RunBudget{});
        EXPECT_EQ(fixture.journal
                      ->load_generation("tenant:runtime", lineage->lineage_id(), 1)
                      ->run_id(),
                  target.run_id());
    }
    std::filesystem::remove(path);
}
#endif

TEST(ProgramRuntimeTest, ForkAllowsBudgetOnlyMigrationButEnforcesTargetDeclaredBounds) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-budget-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    auto target_document = program_document("runtime-interrupt");
    target_document["declared_budget_requirements"][1]["maximum"] = 10;
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto plan =
        fixture.catalog->plan_migration("tenant:runtime", source_version.id(), target_version.id());
    EXPECT_TRUE(plan.is_compatible());

    const std::string rejected_run_id = "fork-budget-rejected";
    try {
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            target_version,
            ProgramInvocation{json::object(),
                              source.remaining_budget(),
                              "trace-fork-budget-target",
                              {},
                              rejected_run_id},
            ProgramResume{json{{"decision", "must-not-dispatch"}},
                          "trace-fork-budget-resume",
                          {},
                          pending_call_id(source)});
        FAIL() << "budget outside target admitted bounds unexpectedly forked";
    } catch (const std::exception& error) {
        EXPECT_NE(std::string(error.what()).find("target admitted bounds"), std::string::npos);
    }

    EXPECT_FALSE(fixture.journal->load("tenant:runtime", rejected_run_id).has_value());
    const auto source_after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(source_after->id(), source_before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ForkRejectsSourceRunIdBeforeCloningCheckpoint) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-same-run", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(before.has_value());
    const auto source_thread             = before->exact_checkpoint()->core_thread_id;
    const auto source_checkpoints_before = fixture.checkpoints->list(source_thread, 100).size();
    ProgramInvocation invocation{json::object(),
                                 source.remaining_budget(),
                                 "trace-fork-same-run-target",
                                 {},
                                 source.run_id()};
    EXPECT_THROW(
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            version, std::move(invocation),
            ProgramResume{json{{"decision", "must-not-clone"}},
                          "trace-fork-same-run-resume",
                          {},
                          before->pending_input()->call_id()}),
        std::exception);

    const auto after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->id(), before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(fixture.checkpoints->list(source_thread, 100).size(), source_checkpoints_before);
}

TEST(ProgramRuntimeTest, DirectForkOverloadRejectsUntrackedChildTopology) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-child-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint());

    ProgramInvocation invocation{json::object(),
                                 source.remaining_budget(),
                                 "trace-fork-child-target",
                                 {},
                                 "fork-child-target",
                                 "untracked-parent",
                                 1};
    EXPECT_THROW((void)fixture.runtime->fork(
                     "tenant:runtime",
                     ExactProgramCheckpointReference{source.run_id(),
                                                     source.checkpoint()->checkpoint_id},
                     version, std::move(invocation),
                     ProgramResume{json{{"decision", "must-not-fork"}},
                                   "trace-fork-child-resume", {}, pending_call_id(source)}),
                 std::invalid_argument);
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", "fork-child-target"));
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ForkMismatchesRejectBeforeTargetRunAndLeaveSourceUnchanged) {
    interrupt_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-reject-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    struct RejectionCase {
        std::string            run_id;
        ForkCompatibilityField field;
        json                   document;
    };
    std::vector<RejectionCase> cases;
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["extra"] =
            json{{"reducer", "runtime-overwrite"}, {"initial", nullptr}};
        cases.push_back(
            {"fork-reject-channel", ForkCompatibilityField::Channel, std::move(document)});
    }
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["value"]["reducer"] = "runtime-alternate";
        cases.push_back(
            {"fork-reject-reducer", ForkCompatibilityField::Reducer, std::move(document)});
    }
    {
        auto document                                       = program_document("runtime-interrupt");
        document["root"]["definition"]["nodes"]["followup"] = json{{"type", "runtime-followup"}};
        document["root"]["definition"]["edges"] =
            json::array({json{{"from", "__start__"}, {"to", "work"}},
                         json{{"from", "work"}, {"to", "followup"}},
                         json{{"from", "followup"}, {"to", "__end__"}}});
        cases.push_back({"fork-reject-continuation", ForkCompatibilityField::Continuation,
                         std::move(document)});
    }

    for (auto& item : cases) {
        const auto target = fixture.admit_document(std::move(item.document));
        try {
            (void)fixture.runtime->fork(
                "tenant:runtime",
                ExactProgramCheckpointReference{source.run_id(),
                                                source.checkpoint()->checkpoint_id},
                target,
                ProgramInvocation{json::object(),
                                  source.remaining_budget(),
                                  "trace-fork-reject",
                                  {},
                                  item.run_id},
                ProgramResume{
                    json{{"decision", "must-not-dispatch"}}, "trace-fork-reject-resume", {}});
            FAIL() << "incompatible fork unexpectedly created a target run";
        } catch (const ProgramForkCompatibilityError& error) {
            EXPECT_TRUE(
                std::any_of(error.receipt().witnesses().begin(), error.receipt().witnesses().end(),
                            [&](const auto& witness) { return witness.field == item.field; }));
        }
        EXPECT_FALSE(fixture.journal->load("tenant:runtime", item.run_id).has_value());
        const auto source_now = fixture.journal->load("tenant:runtime", source.run_id());
        ASSERT_TRUE(source_now.has_value());
        EXPECT_EQ(source_now->id(), source_before->id());
    }

    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

json orchestration_document(json root, std::string node_type = "runtime-completed") {
    auto document                      = program_document(std::move(node_type));
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    root["name"]                       = "main";
    root["definition"]                 = std::move(document["root"]["definition"]);
    document["declared_budget_requirements"][3]["maximum"] = 4;
    document["declared_budget_requirements"][4]["maximum"] = 32;
    document["root"]                                       = std::move(root);
    return document;
}

json orchestration_document() {
    return orchestration_document(
        json{{"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "emit"}, {"value", json{{"kind", "core_done"}}}},
                                       json{{"op", "return"}, {"value", json{{"ok", true}}}}})}});
}

ProgramResult run_orchestration(AdmittedRuntime& fixture,
                                json             document,
                                json             input    = json::object(),
                                std::string      trace_id = "trace-orchestration") {
    const auto version = fixture.admit_document(std::move(document));
    return fixture.runtime->run("tenant:runtime", version,
                                ProgramInvocation{std::move(input),
                                                  RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                                  std::move(trace_id),
                                                  {}});
}

TEST(ProgramRuntimeTest, BranchSelectsWithoutDispatchingTheUnselectedCore) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "branch"},
                      {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                      {"then", json{{"op", "call_core"}}},
                      {"else", json{{"op", "return"}, {"value", json{{"selected", "else"}}}}}}),
        json{{"route", "else"}}, "trace-branch");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"selected", "else"}}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, BoundedLoopReportsBudgetExhaustionAfterTheLastAllowedIteration) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "loop"},
                                         {"condition", json{{"path", "/never"}, {"exists", false}}},
                                         {"max_iterations", 2},
                                         {"body", json{{"op", "call_core"}}}}),
        json::object(), "trace-loop");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_LOOP_BOUND");
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RetryRetriesCoreFailuresAndPreservesFailureClassification) {
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "retry"}, {"max_attempts", 2}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-retry");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(result.failure()->attempts, 2U);
}

TEST(ProgramRuntimeTest, DeepNestedCoreFailurePreservesOperationAndSourceCoordinate) {
    AdmittedRuntime fixture;
    const auto      nested_root = json{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "branch"},
                                            {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                                            {"then", json{{"op", "retry"},
                                                          {"max_attempts", 1},
                                                          {"body", json{{"op", "call_core"}}}}}}})}};
    const auto result =
        run_orchestration(fixture, orchestration_document(nested_root, "runtime-failing"),
                          json{{"route", "then"}}, "trace-deep-nested-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0.0.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/children/0/then/body");
}

TEST(ProgramRuntimeTest, ParallelJoinsAllBranchesInPlanOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result =
        run_orchestration(fixture,
                          orchestration_document(
                              json{{"op", "parallel"}, {"branches", json::array({first, second})}}),
                          json::object(), "trace-parallel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"branch", 1}}, json{{"branch", 2}}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ParallelImmediateBranchesRetainTheirCompletionSignal) {
    AdmittedRuntime fixture(2);
    const json      unreachable_core{{"op", "branch"},
                                     {"condition", json{{"path", "/unused"}, {"exists", true}}},
                                     {"then", json{{"op", "call_core"}}},
                                     {"else", json{{"op", "return"}, {"value", 3}}}};
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "parallel"},
                 {"branches", json::array({json{{"op", "return"}, {"value", 1}},
                                           json{{"op", "return"}, {"value", 2}}, unreachable_core})}}),
        json::object(), "trace-parallel-immediate");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({1, 2, 3}));
}

TEST(ProgramRuntimeTest, NestedParallelCannotExceedRunConcurrencyGrant) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const json      nested{
             {"op", "parallel"},
             {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}};
    const auto version = fixture.admit_document(orchestration_document(
        json{{"op", "parallel"}, {"branches", json::array({nested, nested})}},
        "runtime-short-blocking"));

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 2, 32, 20, 0, 0, 0},
                                               "trace-nested-concurrency",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CONCURRENCY_BUDGET");
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_EQ(blocking_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RaceReturnsTheFirstCompletedBranchAndCancelsTheLoser) {
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "race"}, {"branches", json::array({first, second})}},
                               "runtime-short-blocking"),
        json::object(), "trace-race");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"branch", 2}}));
}

TEST(ProgramRuntimeTest, RaceImmediateBranchesRetainTheirCompletionSignal) {
    AdmittedRuntime fixture(1);
    const json      unreachable_core{{"op", "branch"},
                                     {"condition", json{{"path", "/unused"}, {"exists", true}}},
                                     {"then", json{{"op", "call_core"}}},
                                     {"else", json{{"op", "return"}, {"value", 2}}}};
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "race"},
                 {"branches", json::array({json{{"op", "return"}, {"value", 1}}, unreachable_core})}}),
        json::object(), "trace-race-immediate");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), 1);
}

TEST(ProgramRuntimeTest, RaceUsesDeclarationOrderForAReadyTie) {
    // A ready tie is a single scheduler-turn condition. Use one scheduler
    // thread so this verifies declaration-order arbitration rather than
    // treating cross-worker completion timing as a logical tie.
    AdmittedRuntime fixture(1);
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "race"},
                      {"branches",
                       json::array(
                      {json{{"op", "sequence"},
                                 {"children", json::array({json{{"op", "call_core"}},
                                                           json{{"op", "return"}, {"value", 1}}})}},
                            json{{"op", "sequence"},
                                 {"children", json::array({json{{"op", "call_core"}},
                                                           json{{"op", "return"}, {"value", 2}}})}}})}}),
        json::object(), "trace-race-tie");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), 1);
}

TEST(ProgramRuntimeTest, QuorumStopsAfterRequiredSuccesses) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "quorum"},
                                         {"branches", json::array({json{{"op", "return"}, {"value", 1}},
                                                                   json{{"op", "return"}, {"value", 2}},
                                                                   json{{"op", "call_core"}}})},
                                         {"min_success", 2}}),
        json::object(), "trace-quorum");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({1, 2}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, MapEvaluatesEveryItemAndCollectsOutputs) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "map"},
                 {"items", json::array({1, 2})},
                 {"body", json{{"op", "sequence"},
                               {"children", json::array({json{{"op", "call_core"}},
                                                         json{{"op", "return"},
                                                              {"value", json{{"mapped", true}}}}})}}}}),
        json::object(), "trace-map");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"mapped", true}}, json{{"mapped", true}}}));
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, ExpandTaskGraphPublishesAndBindsDependentChildTasks) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = link_child_versions(
        fixture, fixture.admit_document(expand_task_graph_document(expansion_proposal())),
        fixture.admit_document(parallel_map_echo_child_document()),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json{{"item", "seed"}},
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                                                 "trace-expand-task-graph",
                                                 {}});
    const auto result = parent.wait();
    if (result.status() != ProgramTerminalStatus::Completed) {
        std::cerr << "expand failure=" << (result.failure() ? result.failure()->code : "<none>")
                  << " message=" << (result.failure() ? result.failure()->message : "<none>")
                  << " output=" << result.output().dump() << '\n';
    }

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 2U);
    EXPECT_EQ(result.remaining_budget().max_dynamic_compiles, 0U);
    const auto lineage_id = program_run_lineage_id("tenant:runtime", result.run_id());
    const auto lineage    = fixture.journal->load_lineage("tenant:runtime", lineage_id);
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->remaining_budget().max_dynamic_compiles, 0U);
    EXPECT_GT(lineage->committed_descendant_budget().max_total_children, 0U);
    const auto output = result.output();
    ASSERT_TRUE(output.is_object());
    ASSERT_TRUE(output.contains("fragment_id"));
    ASSERT_TRUE(output.contains("tasks"));
    ASSERT_TRUE(output["tasks"].is_array());
    ASSERT_EQ(output["tasks"].size(), 2U);
    EXPECT_EQ(output["tasks"][0]["task_id"], "seed");
    EXPECT_EQ(output["tasks"][0]["state"], "completed");
    EXPECT_EQ(output["tasks"][1]["task_id"], "dependent");
    EXPECT_EQ(output["tasks"][1]["state"], "completed");
    EXPECT_EQ(output["tasks"][0]["output"]["channels"]["value"]["value"], "seed");
    EXPECT_EQ(output["tasks"][1]["output"]["channels"]["value"]["value"], "seed");

    const auto fragment_id = output["fragment_id"].get<std::string>();
    const auto record      = fixture.task_graph_fragments->load(fragment_id);
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->published);
    EXPECT_TRUE(record->terminal);
    EXPECT_EQ(record->revision, 6U);
    ASSERT_EQ(record->tasks.size(), 2U);
    for (const auto& task : record->tasks) {
        EXPECT_EQ(task.state, TaskGraphTaskState::Completed);
        EXPECT_EQ(task.attempt, 1U);
    }
}

TEST(ProgramRuntimeTest, MapExecutesItemsSerially) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    AdmittedRuntime fixture(2);
    const auto      result =
        run_orchestration(fixture,
                          orchestration_document(json{{"op", "map"},
                                                      {"items", json::array({1, 2})},
                                                      {"body", json{{"op", "call_core"}}}},
                                                 "runtime-short-blocking"),
                          json::object(), "trace-map-serial");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 1U);
    EXPECT_EQ(blocking_active.load(), 0U);
}

TEST(ProgramRuntimeTest, AwaitTimeoutCancelsTheChildOperation) {
    AdmittedRuntime fixture(2);
    const auto      started = std::chrono::steady_clock::now();
    const auto      result  = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "await"}, {"timeout_ms", 10}, {"body", json{{"op", "call_core"}}}},
            "runtime-blocking"),
        json::object(), "trace-await");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started)
            .count(),
        2);
}

TEST(ProgramRuntimeTest, AwaitPropagatesChildFailureBeforeTimeout) {
    AdmittedRuntime fixture;
    const auto      started = std::chrono::steady_clock::now();
    const auto      result  = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "await"}, {"timeout_ms", 1000}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-await-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count(),
              500);
}

TEST(ProgramRuntimeTest, ExplicitCancelStopsTheFollowingOperation) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "sequence"},
                      {"children", json::array({json{{"op", "cancel"}}, json{{"op", "call_core"}}})}}),
        json::object(), "trace-cancel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ExplicitCheckpointPublishesCheckpointEventAfterCore) {
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(
        orchestration_document(json{{"op", "checkpoint"}, {"body", json{{"op", "call_core"}}}}));
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                          "trace-checkpoint", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(result.checkpoint().has_value());
    EXPECT_GE(sink->calls.load(), 1U);
}
TEST(ProgramRuntimeTest, SequenceEmitAndReturnHaveDeterministicTraceAndOutput) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(orchestration_document());

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                               "trace-orchestration",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"ok", true}}));
    EXPECT_EQ(result.execution_trace(), std::vector<std::string>({"work"}));

    EXPECT_EQ(result.usage().program_operations, 4U);
}
TEST(ProgramRuntimeTest, ChildStartPinsReceiptAndPropagatesParentCancellation) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1}, "trace-parent", {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}, "trace-child", {}});

    const auto child_record = child.snapshot();
    EXPECT_EQ(child_record.invocation().parent_run_id, parent.run_id());
    EXPECT_EQ(child_record.child_depth(), 1U);
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, PlannerCannotOverrideResolvedChildBudget) {
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-planner-budget-parent",
                                                 {}});

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 21, 0, 0, 0},
                              "trace-planner-budget-override",
                              {}});
        FAIL() << "Expected the planner-supplied child budget to be rejected";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_BUDGET");
    }

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, RecursiveChildGrantsAttenuateToZeroAtTheConfiguredBoundary) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 4, 4},
                                                 "trace-recursive-parent",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 3},
                          "trace-recursive-child",
                          {}});
    auto grandchild = fixture.runtime->start_child(
        "tenant:runtime", child, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 2, 2},
                          "trace-recursive-grandchild",
                          {}});
    auto great_grandchild = fixture.runtime->start_child(
        "tenant:runtime", grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                          "trace-recursive-great-grandchild",
                          {}});
    auto leaf = fixture.runtime->start_child(
        "tenant:runtime", great_grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-recursive-leaf",
                          {}});

    EXPECT_EQ(child.snapshot().child_depth(), 1U);
    EXPECT_EQ(grandchild.snapshot().child_depth(), 2U);
    EXPECT_EQ(great_grandchild.snapshot().child_depth(), 3U);
    EXPECT_EQ(leaf.snapshot().child_depth(), 4U);

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 0},
                              "trace-recursive-sibling-after-subtree-reservation",
                              {}});
        FAIL() << "Expected the reserved descendant quota to reject a sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_COUNT");
    }

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", leaf, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                              "trace-recursive-overflow",
                              {}});
        FAIL() << "Expected recursive child depth to fail closed";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_DEPTH");
    }

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(great_grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(leaf.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, GlobalChildQuotaRejectsFragmentationAndReleasesOnTerminal) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children                  = 1;
    quota.max_active_children_per_owner        = 1;
    quota.max_pending_spawn_requests           = 4;
    quota.max_pending_spawn_requests_per_owner = 4;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 2, 20, 0, 4, 4},
                                                 "trace-global-quota-parent",
                                                 {}});
    auto child =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-a",
                                                       {}});

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                              "trace-global-quota-child-b",
                              {}});
        FAIL() << "Expected the global active-child quota to reject a fragmented sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(child.cancel());
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto replacement =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-c",
                                                       {}});
    EXPECT_EQ(replacement.snapshot().child_depth(), 1U);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(replacement.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ChildResumeReclaimsGlobalQuotaAfterACompetingChildStops) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children           = 1;
    quota.max_active_children_per_owner = 1;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      interrupted_link =
        make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    const auto active_link = make_linked_child(fixture, "runtime-blocking", "runtime-blocking");

    auto interrupted_parent =
        fixture.runtime->start("tenant:runtime", interrupted_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-resume-parent",
                                                 {}});
    auto interrupted_child = fixture.runtime->start_child(
        "tenant:runtime", interrupted_parent, interrupted_link.receipt,
        interrupted_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-resume-child",
                          {}});
    const auto interrupted = interrupted_child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    auto active_parent =
        fixture.runtime->start("tenant:runtime", active_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-active-parent",
                                                 {}});
    auto active_child = fixture.runtime->start_child(
        "tenant:runtime", active_parent, active_link.receipt, active_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-active-child",
                          {}});

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted_child.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
        FAIL() << "Expected child resume to honor the active global quota";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(active_child.cancel());
    EXPECT_EQ(active_child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted_child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
    EXPECT_EQ(resumed.wait().status(), ProgramTerminalStatus::Completed);

    EXPECT_TRUE(interrupted_parent.cancel());
    EXPECT_EQ(interrupted_parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(active_parent.cancel());
    EXPECT_EQ(active_parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, InterruptedChildReleasesParentConcurrencyUntilItsResumeIsActive) {
    interrupt_calls.store(0);
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);

    auto parent_document                      = program_document("runtime-blocking");
    parent_document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 2;
    parent_document["declared_budget_requirements"][8]["maximum"] = 2;
    const auto parent_version = fixture.admit_document(std::move(parent_document));
    const auto interrupted_version =
        fixture.admit_document(parallel_map_child_document("runtime-interrupt"));
    const auto blocking_version =
        fixture.admit_document(parallel_map_child_document("runtime-blocking"));
    const auto child_budget = BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0};
    const auto interrupted_link =
        link_child_versions(fixture, parent_version, interrupted_version, child_budget);
    const auto blocking_link =
        link_child_versions(fixture, parent_version, blocking_version, child_budget);

    auto parent =
        fixture.runtime->start("tenant:runtime", parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 2},
                                                 "trace-parent-concurrency-reclaim",
                                                 {}});
    auto interrupted_child = fixture.runtime->start_child(
        "tenant:runtime", parent, interrupted_link.receipt, interrupted_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0},
                          "trace-interrupted-concurrency-child",
                          {}});
    const auto interrupted = interrupted_child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    auto active_child = fixture.runtime->start_child(
        "tenant:runtime", parent, blocking_link.receipt, blocking_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0},
                          "trace-active-concurrency-child",
                          {}});

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted_child.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-concurrency-resume"));
        FAIL() << "Expected active sibling concurrency to block child resume";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_CONCURRENCY");
    }

    EXPECT_TRUE(active_child.cancel());
    EXPECT_EQ(active_child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted_child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-concurrency-resume"));
    EXPECT_EQ(resumed.wait().status(), ProgramTerminalStatus::Completed);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DuplicateChildRecoveryReturnsTheExistingRun) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-duplicate",
                                                 {}});
    ProgramInvocation invocation{json::object(),
                                 RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                 "trace-child-duplicate",
                                 {}};
    auto              first = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);
    auto duplicate          = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);

    EXPECT_EQ(duplicate.run_id(), first.run_id());
    EXPECT_EQ(duplicate.snapshot().id(), first.snapshot().id());
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(duplicate.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, InterruptedChildRemainsInFlightAndResumeJoinsDurably) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-interrupted-child",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-interrupted",
                          {}});

    const auto interrupted = child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto parent_after_interrupt = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_interrupt.has_value());
    ASSERT_EQ(parent_after_interrupt->children().size(), 1U);
    EXPECT_EQ(parent_after_interrupt->children().front().state, ProgramChildState::Dispatched);
    ASSERT_TRUE(parent_after_interrupt->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_interrupt->children().front().terminal_result->status(),
              ProgramTerminalStatus::Interrupted);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-child-resumed"));
    const auto result = resumed.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.snapshot().child_depth(), 1U);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "approved");
    const auto parent_after_resume = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_resume.has_value());
    ASSERT_EQ(parent_after_resume->children().size(), 1U);
    EXPECT_EQ(parent_after_resume->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(parent_after_resume->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_resume->children().front().terminal_result->status(),
              ProgramTerminalStatus::Completed);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, FailedDispatchIsRetriedFromPublishingBoundary) {
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-dispatch-recovery",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-dispatch-recovery",
                                       {}};
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);

    const auto publishing = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                  linked.child_version, child_invocation);
    EXPECT_EQ(recovered.wait().status(), ProgramTerminalStatus::Completed);
    const auto completed = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(completed->children().front().terminal_result.has_value());

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, RecoverChildrenRetriesPublishingBoundaryWithoutDuplicateDispatch) {
    completed_calls.store(0);
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-recover-children",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-recover-children",
                                       {}};

    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);
    const auto publishing = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->recover_children("tenant:runtime", parent.run_id());
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto completed = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DurableDslSpawnDispatchesTheLinkedChildAndJoinsItsResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "completed");
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto record   = parent.snapshot();
    const auto children = record.children();
    ASSERT_EQ(children.size(), 1U);
    const auto& child = children.front();
    EXPECT_EQ(child.link_id, linked.receipt.id());
    EXPECT_EQ(child.invocation.parent_run_id, parent.run_id());
    EXPECT_EQ(child.invocation.child_depth, 1U);
    EXPECT_EQ(child.state, ProgramChildState::Completed);
    ASSERT_TRUE(child.terminal_result.has_value());
    EXPECT_EQ(child.terminal_result->status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, DurableDslSpawnFailsClosedWhenItsBindingCannotBeResolved) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    fixture.clear_child_bindings();

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-missing-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsAResolverReceiptForAnotherBinding) {
    completed_calls.store(0);
    AdmittedRuntime         fixture(2);
    const auto              linked     = make_durable_spawn_child(fixture);
    const ModuleLinkReceipt mismatched = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), "different-child",
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget()});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{mismatched, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsReceiptWhoseGuaranteeExceedsChildVersion) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Recorded);
    const auto linked = make_durable_spawn_child(fixture, "runtime-completed", "runtime-completed",
                                                 std::nullopt, ExecutionGuarantee::Recorded);
    ASSERT_EQ(linked.child_version.execution_guarantee(), ExecutionGuarantee::Recorded);

    const ModuleLinkReceipt forged = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), linked.receipt.child_name(),
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget(), ExecutionGuarantee::Strict});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{forged, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-guarantee-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_GUARANTEE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslAwaitTimeoutCancelsAndRecordsItsChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked =
        make_durable_spawn_child(fixture, "runtime-completed", "runtime-blocking", 10);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn-timeout",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");

    const auto pending          = parent.snapshot();
    const auto pending_children = pending.children();
    ASSERT_EQ(pending_children.size(), 1U);
    auto child =
        fixture.runtime->reconnect("tenant:runtime", pending_children.front().child_run_id);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto settled          = parent.snapshot();
    const auto settled_children = settled.children();
    ASSERT_EQ(settled_children.size(), 1U);
    EXPECT_EQ(settled_children.front().state, ProgramChildState::Cancelled);
    // Parent timeout and child cancellation publish through independent
    // scheduler paths.  The parent may snapshot the cancelled relation before
    // or after the child's matching terminal result is durably attached.
    if (const auto& terminal = settled_children.front().terminal_result) {
        EXPECT_EQ(terminal->status(), ProgramTerminalStatus::Cancelled);
        EXPECT_EQ(terminal->run_id(), pending_children.front().child_run_id);
    }
}
TEST(ProgramRuntimeTest, ParentCompletionCancelsAttachedChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-complete",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-complete",
                          {}});

    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ParentTerminalReconnectRetainsDurableChildRecords) {
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reopen",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-reopen",
                          {}});

    ASSERT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto parent_id = parent.run_id();
    const auto child_id  = child.run_id();
    const auto before    = parent.snapshot();
    ASSERT_EQ(before.children().size(), 1U);
    EXPECT_EQ(before.children().front().child_run_id, child_id);

    fixture.recreate_catalog_and_runtime();
    auto       reconnected = fixture.runtime->reconnect("tenant:runtime", parent_id);
    const auto after       = reconnected.snapshot();
    ASSERT_EQ(after.children().size(), 1U);
    EXPECT_EQ(after.children().front().child_run_id, child_id);
    EXPECT_EQ(after.children().front().invocation.parent_run_id, parent_id);
    EXPECT_EQ(reconnected.wait().status(), ProgramTerminalStatus::Completed);
}
TEST(ProgramRuntimeTest, FailedChildPublicationReleasesParentReservation) {
    blocking_calls.store(0);
    AdmittedRuntime   fixture(2);
    const auto        linked = make_linked_child(fixture);
    ProgramInvocation occupied_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    occupied_invocation.requested_run_id = "run-child-conflict";
    auto occupied =
        fixture.runtime->start("tenant:runtime", linked.child_version, occupied_invocation);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reservation",
                                                 {}});

    ProgramInvocation conflict_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    conflict_invocation.requested_run_id = occupied.run_id();
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, conflict_invocation),
                 std::exception);
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-after-conflict",
                          {}});

    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(occupied.cancel(), true);
    EXPECT_EQ(occupied.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, ParallelMapBindsInputAndReturnsOrderedBoundedChildOutputs) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      items = json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}});
    auto            parent_document = parallel_map_document(
        json{{"literal", json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}})}}, 3);
    parent_document["root"]["item_source"] = json{{"artifact", "input"}, {"field", "/items"}};
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked                                             = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json{{"items", items}},
                                                 RunBudget{10000, 1000, 1000, 2, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-input-output",
                                                 {}});
    const auto result = parent.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    const auto output = result.output();
    ASSERT_TRUE(output.is_array());
    ASSERT_EQ(output.size(), 3U);
    EXPECT_EQ(output.at(0).get<std::string>(), "completed");
    EXPECT_EQ(output.at(1).get<std::string>(), "completed");
    EXPECT_EQ(output.at(2).get<std::string>(), "completed");
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_EQ(completed_calls.load(), 3U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 3U);
    for (std::size_t index = 0; index < children.size(); ++index) {
        EXPECT_EQ(children[index].state, ProgramChildState::Completed);
        const json expected_input{{"item", items.at(index)}};
        EXPECT_EQ(children[index].invocation.input, expected_input);
    }
}
TEST(ProgramRuntimeTest, ParallelMapCollectPreservesInputOrdinalOutputOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      items = json::array({json{{"id", 1}}, json{{"id", 2}}, json{{"id", 3}}});
    auto            parent_document = parallel_map_document(json{{"literal", items}}, 3, "collect");
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_echo_child_document()),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-collect-order",
                                                 {}});
    const auto result = parent.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message : "");
    ASSERT_TRUE(result.output().is_array());
    ASSERT_EQ(result.output().size(), items.size());
    for (std::size_t index = 0; index < items.size(); ++index)
        EXPECT_EQ(result.output().at(index), items.at(index));
    EXPECT_EQ(completed_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, ParallelMapRejectsDynamicCollectionBeforeLaunchingChildren) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"artifact", "input"}, {"field", "/items"}}, 2);
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json{{"items", json::array({1, 2, 3})}},
                                               RunBudget{10000, 1000, 1000, 2, 1, 20, 0, 1, 2},
                                               "trace-parallel-map-oversized-input",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_BOUND");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_TRUE(fixture.runtime->reconnect("tenant:runtime", result.run_id())
                    .snapshot()
                    .children()
                    .empty());
}
TEST(ProgramRuntimeTest, ParallelMapRejectsMissingInputArtifactFieldBeforeChildLaunch) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"artifact", "input"}, {"field", "/missing"}}, 1);
    const auto linked = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-missing-input",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_SOURCE");
    EXPECT_TRUE(fixture.runtime->reconnect("tenant:runtime", result.run_id())
                    .snapshot()
                    .children()
                    .empty());
}

TEST(ProgramRuntimeTest, ParallelMapRejectsMissingChildOutputField) {
    AdmittedRuntime fixture(2);
    auto            parent_document = parallel_map_document(json{{"literal", json::array({1})}}, 1);
    parent_document["root"]["output_binding"]["from"]["field"] = "/missing";
    const auto linked                                          = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-missing-output",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_OUTPUT");
    ASSERT_EQ(
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children().size(),
        1U);
}
TEST(ProgramRuntimeTest, ParallelMapRejectsChildOutputContractMismatch) {
    AdmittedRuntime fixture(2);
    auto            parent_document = parallel_map_document(json{{"literal", json::array({1})}}, 1);
    auto            child_document  = parallel_map_child_document("runtime-completed");
    child_document["output_contract"]["schema"] =
        json{{"type", "object"}, {"required", json::array({"must"})}};
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(std::move(child_document)),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-output-contract",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_OUTPUT_CONTRACT");
    ASSERT_EQ(
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children().size(),
        1U);
}

TEST(ProgramRuntimeTest, ParallelMapBuildsNestedArrayInputBindings) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({json{{"id", 7}}})}}, 1);
    parent_document["root"]["input_binding"]["to"]["field"] = "/payload/0";
    const auto linked                                       = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-completed")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-array-input",
                                               {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    const auto children =
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    const json expected_input{{"payload", json::array({json{{"id", 7}}})}};
    EXPECT_EQ(children.front().invocation.input, expected_input);
}

TEST(ProgramRuntimeTest, ParallelMapCollectWaitsForEveryChildFailure) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "collect");
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-failing")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 3, 4, 20, 0, 1, 3},
                                                 "trace-parallel-map-collect",
                                                 {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_EQ(result.failure()->witness["item_index"], 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 3U);
    for (const auto& child : children)
        EXPECT_EQ(child.state, ProgramChildState::Failed);
}

TEST(ProgramRuntimeTest, ParallelMapFailFastStopsLaunchingAfterFirstFailedItem) {
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "fail_fast");
    parent_document["root"]["max_in_flight"]                      = 1;
    parent_document["declared_budget_requirements"][3]["minimum"] = 1;
    parent_document["declared_budget_requirements"][3]["maximum"] = 1;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-failing")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 3},
                                               "trace-parallel-map-fail-fast",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->witness["item_index"], 0U);
    const auto children =
        fixture.runtime->reconnect("tenant:runtime", result.run_id()).snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    EXPECT_EQ(children.front().state, ProgramChildState::Failed);
}

TEST(ProgramRuntimeTest, ParallelMapFailFastCancelsActiveSiblingAfterFailure) {
    map_fail_first_calls.store(0);
    map_fail_first_active.store(0);
    AdmittedRuntime fixture(2);
    auto            parent_document =
        parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3, "fail_fast");
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 3;
    const auto linked                                             = link_child_versions(
        fixture, fixture.admit_document(std::move(parent_document)),
        fixture.admit_document(parallel_map_child_document("runtime-map-fail-first")),
        BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 3, 20, 0, 1, 3},
                                                 "trace-parallel-map-fail-fast-active",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(map_fail_first_calls.load(), 2U);
    EXPECT_EQ(map_fail_first_active.load(), 0U);

    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    std::size_t failed    = 0;
    std::size_t cancelled = 0;
    for (const auto& child : children) {
        failed += child.state == ProgramChildState::Failed;
        cancelled += child.state == ProgramChildState::Cancelled;
    }
    EXPECT_EQ(failed, 1U);
    EXPECT_EQ(cancelled, 1U);
}

TEST(ProgramRuntimeTest, ParallelMapRejectsOutputBeyondItsImmutableByteBound) {
    AdmittedRuntime fixture(2);
    const auto      linked = link_child_versions(
        fixture,
        fixture.admit_document(
            parallel_map_document(json{{"literal", json::array({1})}}, 1, "fail_fast", 2)),
        fixture.admit("runtime-completed"), BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                                               "trace-parallel-map-output-cap",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PARALLEL_MAP_OUTPUT");
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_TRUE(result.output().is_array());
    EXPECT_TRUE(result.output().empty());
    const auto record = fixture.journal->load("tenant:runtime", result.run_id());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->terminal_result().has_value());
    EXPECT_TRUE(record->terminal_result()->output().is_array());
    EXPECT_TRUE(record->terminal_result()->output().empty());
}

TEST(ProgramRuntimeTest, ParallelMapProvidesBoundedChildOverlapAndCancelsActiveChildren) {
    blocking_calls.store(0);
    blocking_active.store(0);
    blocking_peak.store(0);
    overlap_barrier                = std::make_shared<std::barrier<>>(2);
    overlap_ready                  = std::make_shared<std::promise<void>>();
    auto            overlap_future = overlap_ready->get_future();
    AdmittedRuntime fixture(4);
    auto parent_document = parallel_map_document(json{{"literal", json::array({1, 2, 3})}}, 3);
    parent_document["declared_budget_requirements"][3]["maximum"] = 3;
    parent_document["declared_budget_requirements"][4]["maximum"] = 3;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(parallel_map_child_document("runtime-barrier")),
                            BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 3, 3, 20, 0, 1, 3},
                                                 "trace-parallel-map-overlap-cancel",
                                                 {}});
    ASSERT_EQ(overlap_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(blocking_calls.load(), 2U);
    ASSERT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(blocking_active.load(), 2U);
    EXPECT_EQ(blocking_peak.load(), 2U);
    ASSERT_TRUE(parent.cancel());

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(blocking_active.load(), 0U);
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 2U);
    for (const auto& child : children)
        EXPECT_EQ(child.state, ProgramChildState::Cancelled);
    overlap_barrier.reset();
    overlap_ready.reset();
}
TEST(ProgramRuntimeTest, ParallelMapLaunchesNextWindowAfterPriorCompletion) {
    window_calls.store(0);
    window_active.store(0);
    window_peak.store(0);
    window_completed.store(0);
    window_second_started_before_completion.store(0);
    window_first_barrier = std::make_shared<std::barrier<>>(2);
    AdmittedRuntime fixture(4);
    auto parent_document = parallel_map_document(json{{"literal", json::array({1, 2, 3, 4})}}, 4);
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 5;
    parent_document["declared_budget_requirements"][5]["maximum"] = 20;
    auto child_document = parallel_map_child_document("runtime-windowed");
    child_document["declared_budget_requirements"][0]["maximum"] = 2000;
    child_document["declared_budget_requirements"][1]["maximum"] = 200;
    child_document["declared_budget_requirements"][2]["maximum"] = 200;
    child_document["declared_budget_requirements"][5]["maximum"] = 4;
    const auto linked =
        link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                            fixture.admit_document(std::move(child_document)),
                            BudgetLimits{2000, 200, 200, 1, 1, 4, 0, 0, 0});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 2, 5, 20, 0, 1, 4},
                                               "trace-parallel-map-windowed",
                                               {}});

    ASSERT_FALSE(result.failure().has_value());
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(window_calls.load(), 4U);
    EXPECT_EQ(window_completed.load(), 2U);
    EXPECT_EQ(window_active.load(), 0U);
    EXPECT_EQ(window_peak.load(), 2U);
    EXPECT_EQ(window_second_started_before_completion.load(), 0U);
    window_first_barrier.reset();
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramSynthesisGateway, RequiresSemanticValidatorBeforeAcceptingProposals) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    ProgramSynthesisGatewayConfig config;
    config.compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    config.catalog = fixture.catalog;
    config.reserve = [](const ProgramSynthesisProposal&) -> ProgramSynthesisReservation {
        throw std::logic_error("reservation must not be called");
    };
    config.admission = [](const ProgramSynthesisProposal&, const ProgramBundle&,
                          const ProgramSynthesisReservation&) -> ProgramAdmission {
        throw std::logic_error("admission must not be called");
    };
    EXPECT_THROW(ProgramSynthesisGateway(std::move(config)), std::invalid_argument);
}

TEST(ProgramSynthesisGateway, ReservesBeforeCompilingAndAdmitsExactSuccessor) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    auto compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    const auto source = ProgramSource::from_javascript(
        "generated-successor.js",
        javascript_runtime_source("runtime-completed", "return {ok: true};"));
    ProgramSynthesisProposalData proposal_data;
    proposal_data.owner_scope = "tenant:runtime";
    proposal_data.lineage_id = digest('1');
    proposal_data.parent_run_id = "parent-run";
    proposal_data.source = source;
    proposal_data.requested_budget = javascript_budget(1, 1, 0, 0);
    proposal_data.created_at_ms = 1;
    const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

    bool reserved = false;
    ProgramSynthesisGateway gateway({
        compiler,
        fixture.catalog,
        [&](const ProgramSynthesisProposal& value) {
            reserved = true;
            auto before = javascript_budget(1, 1, 0, 0);
            before.max_dynamic_compiles = 1;
            auto after = before;
            after.max_dynamic_compiles = 0;
            return ProgramSynthesisReservation::create(
                {value.id(), value.data().lineage_id, digest('2'), digest('3'),
                 before, after});
        },
        [&](const ProgramSynthesisProposal&, const ProgramBundle&,
            const ProgramSynthesisReservation&) {
            EXPECT_TRUE(reserved);
            return ProgramAdmission{"tenant:runtime", fixture.profile,
                                    fixture.policy, {}};
        },
        1024 * 1024,
        [](const ProgramSynthesisProposal&, const ProgramBundle& bundle,
           const ProgramSynthesisReservation&) {
            const auto definitions = bundle.sealed_core_definitions();
            return ProgramSynthesisSemanticDecision{
                digest('7'), digest('8'),
                definitions.size() == 1 && definitions.front().name == "main",
                json{{"core_count", definitions.size()}}};
        },
        1024 * 1024});
    const auto result = gateway.synthesize(proposal);
    EXPECT_TRUE(reserved);
    EXPECT_EQ(result.receipt.data().proposal_id, proposal.id());
    EXPECT_EQ(result.receipt.data().bundle_id, result.bundle.id());
    EXPECT_EQ(result.receipt.data().program_version_id, result.version.id());
    EXPECT_TRUE(result.validation.data().accepted);
    EXPECT_EQ(result.validation.data().proposal_id, proposal.id());
    EXPECT_EQ(result.validation.data().reservation_id, result.reservation.id());
    EXPECT_EQ(result.validation.data().bundle_id, result.bundle.id());
    EXPECT_EQ(ProgramSynthesisValidationReceipt::parse(
                  result.validation.serialize_canonical()).id(),
              result.validation.id());
    EXPECT_NO_THROW(validate_program_synthesis_validation_evidence(
        result.validation, result.validation_evidence));
    auto tampered_evidence = result.validation_evidence;
    tampered_evidence["core_count"] = 99;
    EXPECT_THROW(validate_program_synthesis_validation_evidence(
                     result.validation, tampered_evidence),
                 std::invalid_argument);
    ASSERT_TRUE(fixture.catalog->find_version("tenant:runtime", result.version.id()));

    const auto& remaining = result.reservation.data().remaining_after_reservation;
    const std::map<std::string, std::uint64_t> expected_maximum = {
        {"wall_time_ms", remaining.wall_time_ms},
        {"model_tokens", remaining.model_tokens},
        {"monetary_microunits", remaining.monetary_microunits},
        {"max_concurrency", remaining.max_concurrency},
        {"max_program_operations", remaining.max_program_operations},
        {"max_core_steps", remaining.max_core_steps},
        {"max_dynamic_compiles", remaining.max_dynamic_compiles},
        {"max_child_depth", remaining.max_child_depth},
        {"max_total_children", remaining.max_total_children},
    };
    const std::map<std::string, std::uint64_t> expected_minimum = {
        {"wall_time_ms", 1},
        {"model_tokens", 0},
        {"monetary_microunits", 0},
        {"max_concurrency", 1},
        {"max_program_operations", 1},
        {"max_core_steps", 1},
        {"max_dynamic_compiles", 0},
        {"max_child_depth", 0},
        {"max_total_children", 0},
    };
    ASSERT_EQ(result.bundle.declared_budget_requirements().size(), expected_maximum.size());
    for (const auto& requirement : result.bundle.declared_budget_requirements()) {
        ASSERT_TRUE(expected_minimum.contains(requirement.resource));
        EXPECT_EQ(requirement.minimum, expected_minimum.at(requirement.resource));
        EXPECT_EQ(requirement.maximum, expected_maximum.at(requirement.resource));
    }
}

TEST(ProgramSynthesisGateway, SemanticRejectionConsumesReservationAndPreventsAdmission) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    auto compiler = std::make_shared<ProgramCompiler>(
        fixture.registry, ProgramCompilerConfig{"program-runtime-test/v1"});
    const auto source = ProgramSource::from_javascript(
        "generated-wrong-successor.js",
        javascript_runtime_source(
            "runtime-completed",
            "return yield ng.callCore(\"wrong-core\", input, \"semantic:wrong-core\");"));
    ProgramSynthesisProposalData proposal_data;
    proposal_data.owner_scope = "tenant:runtime";
    proposal_data.lineage_id = digest('1');
    proposal_data.parent_run_id = "parent-run";
    proposal_data.source = source;
    proposal_data.requested_budget = javascript_budget(1, 1, 0, 0);
    proposal_data.created_at_ms = 1;
    const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

    bool reserved = false;
    bool admission_called = false;
    ProgramSynthesisGateway gateway({
        compiler,
        fixture.catalog,
        [&](const ProgramSynthesisProposal& value) {
            reserved = true;
            auto before = javascript_budget(1, 1, 0, 0);
            before.max_dynamic_compiles = 1;
            auto after = before;
            after.max_dynamic_compiles = 0;
            return ProgramSynthesisReservation::create(
                {value.id(), value.data().lineage_id, digest('2'), digest('3'), before, after});
        },
        [&](const ProgramSynthesisProposal&, const ProgramBundle&,
            const ProgramSynthesisReservation&) {
            admission_called = true;
            return ProgramAdmission{"tenant:runtime", fixture.profile, fixture.policy, {}};
        },
        1024 * 1024,
        [](const ProgramSynthesisProposal&, const ProgramBundle& bundle,
           const ProgramSynthesisReservation&) {
            std::string observed;
            bool accepted = false;
            if (const auto control = bundle.control_source()) {
                auto generator = neograph::program::detail::JavaScriptGenerator::open(
                    *control, json::object(), JavaScriptCompileLimits{});
                if (generator) {
                    const auto step = generator->next();
                    if (step.command && step.command->kind() == JavaScriptCommandKind::CallCore) {
                        observed = step.command->arguments().value("name", "");
                        accepted = observed == "main";
                    }
                }
            }
            return ProgramSynthesisSemanticDecision{
                digest('7'), digest('8'), accepted,
                json{{"code", "P_SYNTHESIS_SEMANTIC_CORE_BINDING"},
                     {"expected", "main"},
                     {"observed", observed},
                     {"observed_bundle", bundle.id()}}};
        },
        1024 * 1024});

    try {
        (void)gateway.synthesize(proposal);
        FAIL() << "expected semantic rejection";
    } catch (const ProgramSynthesisValidationError& error) {
        EXPECT_TRUE(reserved);
        EXPECT_FALSE(admission_called);
        EXPECT_FALSE(error.receipt().data().accepted);
        EXPECT_EQ(error.receipt().data().proposal_id, proposal.id());
        EXPECT_EQ(error.evidence().at("code"), "P_SYNTHESIS_SEMANTIC_CORE_BINDING");
        EXPECT_EQ(error.evidence().at("observed"), "wrong-core");
        EXPECT_FALSE(fixture.store->get_bundle(
            "tenant:runtime", error.receipt().data().bundle_id));
        EXPECT_EQ(ProgramSynthesisValidationReceipt::parse(
                      error.receipt().serialize_canonical()).id(),
                  error.receipt().id());
    }
}
#endif
