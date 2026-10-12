// One failure contract for every path that runs a model's tool calls.
//
// The contract (reference, "Tool failure contract"):
//
//   - Run control signals stop the run and never become a tool message:
//     HookBoundaryBlocked, NodeInterrupt (from a ToolGate or from tool code)
//     and cancellation of the run's CancelToken. Their type and fields
//     (invocation id, state, interrupt payload) survive.
//   - Every other failure (tool exceptions, unknown tools, lookup failures,
//     typed ToolExecutionController results such as timed_out) becomes a tool
//     result with a typed status that the model sees.
//   - With a ToolEffectBroker, a failure whose effect may already have happened
//     pauses the run for reconciliation instead of reaching the model.
//
// The paths grew separately and drifted. A batch turned a HookBoundaryBlocked
// into an {"error": ...} message the model could route around while a single
// call stopped the run; the ToolExecutionController turned a NodeInterrupt or a
// HookBoundaryBlocked raised by tool code into a "failed" result on every path.
// The table below runs every category through every path so they cannot drift
// again.

#include <gtest/gtest.h>

#include <neograph/neograph.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/deep_research_graph.h>
#include <neograph/graph/plan_execute_graph.h>
#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/hook_runtime.h>
#include <neograph/llm/agent.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_effect_broker.h>
#include <neograph/tool_execution.h>
#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
#include <neograph/mcp/client.h>
#endif
#include "fixtures/typed_provider.h"

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

constexpr const char* kTargetCall = "call-target";
constexpr const char* kSiblingCall = "call-sibling";

// ── Tools ──────────────────────────────────────────────────────────────────

enum class Act { Succeed, Throw, ThrowNonStd, ThrowHookBlock, ThrowInterrupt, CancelRun, Sleep };

struct Probe {
    std::shared_ptr<CancelToken> run_token;
    std::atomic<int> target_runs{0};
    std::atomic<int> sibling_runs{0};
};

class TargetTool final : public Tool, public ContextualAsyncTool {
public:
    TargetTool(Act act, std::shared_ptr<Probe> probe) : act_(act), probe_(std::move(probe)) {}

    ChatTool get_definition() const override {
        return {"target", "contract probe", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string get_name() const override { return "target"; }
    std::string execute(const json&) override {
        throw std::logic_error("the context-aware overload must be used");
    }

    asio::awaitable<std::string> execute_async(const json&, ToolExecutionContext) override {
        probe_->target_runs.fetch_add(1);
        switch (act_) {
            case Act::Succeed:
                co_return "target ok";
            case Act::Throw:
                throw std::runtime_error("target failed: \"quoted\"");
            case Act::ThrowNonStd:
                throw 42;
            case Act::ThrowHookBlock:
                // What a tool that runs a nested agent or graph sees when that
                // inner run's mandatory Hook blocks.
                throw HookBoundaryBlocked("nested-invocation", HookExecutionState::TimedOut,
                                          "nested hook blocked");
            case Act::ThrowInterrupt:
                throw NodeInterrupt("target needs approval", json{{"cmd", "deploy"}});
            case Act::CancelRun:
                probe_->run_token->cancel();
                co_return "finished after the cancel";
            case Act::Sleep: {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(2s);
                co_await timer.async_wait(asio::use_awaitable);
                co_return "late";
            }
        }
        co_return "unreachable";
    }

private:
    Act act_;
    std::shared_ptr<Probe> probe_;
};

class SiblingTool final : public Tool {
public:
    explicit SiblingTool(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    ChatTool get_definition() const override {
        return {"sibling", "harmless sibling", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string get_name() const override { return "sibling"; }
    std::string execute(const json&) override {
        probe_->sibling_runs.fetch_add(1);
        return "sibling ok";
    }

private:
    std::shared_ptr<Probe> probe_;
};

// A tool whose name cannot even be read: resolving any call that reaches it fails.
class ThrowingNameTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return {"broken", "lookup probe", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string get_name() const override { throw std::runtime_error("name lookup failed"); }
    std::string execute(const json&) override { return "unreachable"; }
};

// Hook target: fails (so a FailClosed Hook blocks) or records what it saw.
class HookTarget final : public Tool {
public:
    explicit HookTarget(bool fail) : fail_(fail) {}
    ChatTool get_definition() const override { return {"audit", "", json::object()}; }
    std::string get_name() const override { return "audit"; }
    std::string execute(const json& arguments) override {
        if (fail_) throw std::runtime_error("audit sink unavailable");
        std::lock_guard lock(mutex_);
        seen_.push_back(arguments);
        return "audited";
    }
    std::vector<json> seen() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    bool fail_;
    mutable std::mutex mutex_;
    std::vector<json> seen_;
};

// Mandatory FailClosed tool Hooks on the given phases. Each Hook's target sees
// {"phase", "tool_name", "tool_call_id", "status"} of the event.
std::shared_ptr<HookRuntime> tool_hooks(std::shared_ptr<HookTarget> target,
                                        std::vector<HookPhase> phases) {
    HookTargetResolver resolver = [target](std::string_view id) -> std::optional<HookTargetContract> {
        if (id != "audit") return std::nullopt;
        return HookTargetContract{target.get(), {"audit"}, ToolEffectClass::ReadOnly};
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    for (const auto phase : phases) {
        HookDefinitionData data;
        data.priority = 1;
        data.phase = phase;
        data.target_id = "audit";
        data.required_capabilities = {"audit"};
        data.effect = ToolEffectClass::ReadOnly;
        data.input_mapper = {HookInputMapperKind::Template, {},
                             json{{"phase", std::string(to_string(phase))},
                                  {"tool_name", json{{"$event", "/tool_name"}}},
                                  {"tool_call_id", json{{"$event", "/tool_call_id"}}}},
                             {}};
        if (phase == HookPhase::AfterToolExecution)
            data.input_mapper.value_template["status"] = json{{"$event", "/status"}};
        registry->admit(HookDefinition::create(std::move(data)));
    }
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(
        resolver, std::make_shared<ToolExecutionController>());
    auto runner = std::make_shared<MandatoryHookRunner>(
        std::make_shared<InMemoryHookJournal>(), std::move(registry), std::move(adapter));
    // The hook target closure keeps the target alive with the runtime.
    return std::make_shared<HookRuntime>(std::move(runner));
}

std::shared_ptr<HookRuntime> before_tool_hooks(std::shared_ptr<HookTarget> target) {
    return tool_hooks(std::move(target), {HookPhase::BeforeToolExecution});
}

// Mediated dispatch, as Program's Core tool_dispatch uses it: the broker owns
// the effect and runs the tool through the controller.
class PassThroughBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(ToolEffectIdentity, Tool& tool, json arguments,
                                                 ToolExecutionContext context) override {
        auto controller = context.controller ? context.controller
                                             : default_tool_execution_controller();
        co_return co_await controller->execute_result_async(tool, std::move(arguments),
                                                            std::move(context));
    }
};

// ── What a path did ────────────────────────────────────────────────────────

// One tool result as the model receives it.
struct Seen {
    std::string call_id;
    std::string status;
    std::string content;
    bool is_error = false;
};

void collect_seen(const sp::Message& message, std::vector<Seen>& out) {
    for (const auto& part : message.parts) {
        const auto* result = std::get_if<sp::ToolResult>(&part);
        if (!result) continue;
        Seen seen;
        seen.call_id = result->tool_use_id;
        seen.content = result->content;
        seen.is_error = result->is_error;
        if (result->host) seen.status = result->host->status;
        out.push_back(std::move(seen));
    }
}

void collect_seen(const ChatMessage& message, std::vector<Seen>& out) {
    if (message.role != "tool") return;
    collect_seen(portable_message(message), out);
}

struct Outcome {
    // What escaped the path, as its caller sees it. Nothing is unwrapped: a
    // run control signal must leave a graph run as its own type, exactly as it
    // leaves llm::Agent (a HookBoundaryBlocked used to leave a graph run
    // wrapped in a NodeExecutionError).
    std::exception_ptr error;
    std::optional<std::string> interrupt;   // reason of a NodeInterrupt or a paused run
    json interrupt_payload;
    std::vector<Seen> seen;                 // tool results that reached the model

    void capture(std::exception_ptr raised) {
        try {
            std::rethrow_exception(raised);
        } catch (const NodeInterrupt& paused) {
            interrupt = paused.reason();
            interrupt_payload = paused.value();
        } catch (...) {
            error = raised;
        }
    }
    const Seen* target() const {
        for (const auto& item : seen)
            if (item.call_id == kTargetCall) return &item;
        return nullptr;
    }
};

std::string describe(const Outcome& outcome) {
    std::string text;
    if (outcome.error) {
        try {
            std::rethrow_exception(outcome.error);
        } catch (const std::exception& error) {
            text += std::string("threw ") + typeid(error).name() + ": " + error.what();
        } catch (...) {
            text += "threw a non-standard exception";
        }
    }
    if (outcome.interrupt) text += " interrupted: " + *outcome.interrupt;
    for (const auto& item : outcome.seen)
        text += " seen[" + item.call_id + "]=" + item.status + ":" + item.content;
    return text.empty() ? "nothing" : text;
}

// ── Scenarios ──────────────────────────────────────────────────────────────

struct Expect {
    enum class Kind { ToolResult, HookBlocked, Cancelled, Interrupted } kind;
    std::string status;   // ToolResult
    std::string text;     // ToolResult: error substring; HookBlocked: invocation id ("*": any
                          // non-empty id); Interrupted: reason
    std::optional<HookExecutionState> state;  // HookBlocked
    std::string payload_key;                  // Interrupted: a payload member to check
    std::string payload_value;
};

Expect tool_result(std::string status, std::string text) {
    return {Expect::Kind::ToolResult, std::move(status), std::move(text), {}, {}, {}};
}
Expect hook_blocked(std::string invocation, std::optional<HookExecutionState> state) {
    return {Expect::Kind::HookBlocked, {}, std::move(invocation), state, {}, {}};
}
Expect cancelled() { return {Expect::Kind::Cancelled, {}, {}, {}, {}, {}}; }
Expect interrupted(std::string reason, std::string key, std::string value) {
    return {Expect::Kind::Interrupted, {}, std::move(reason), {}, std::move(key), std::move(value)};
}
Expect reconciliation() {
    return interrupted("Mediated Tool effect requires reconciliation", "kind",
                       "tool_effect_reconciliation");
}

struct Scenario {
    const char* name;
    Act act = Act::Succeed;
    bool call_unknown = false;     // the model names a tool nobody registered
    bool throwing_lookup = false;  // a registered tool's get_name() throws
    bool gate_interrupt = false;   // a ToolGate asks a human first
    bool failing_hook = false;     // a FailClosed BeforeToolExecution Hook fails
    bool expired_deadline = false; // the run deadline passed before the Hook could run
    bool policy_timeout = false;   // the tool's execution policy times out
    bool read_only = false;        // the tool's policy declares a read-only effect
    bool anonymous = false;        // Hooks are configured but the run has no owner scope
    Expect expect;
    std::optional<Expect> brokered;  // override for paths with a ToolEffectBroker
};

const std::vector<Scenario>& scenarios() {
    static const std::vector<Scenario> table = [] {
        std::vector<Scenario> rows;
        auto row = [&rows](Scenario s) { rows.push_back(std::move(s)); };
        // A read-only tool that throws has had no effect worth reconciling.
        row({.name = "tool_exception", .act = Act::Throw, .read_only = true,
             .expect = tool_result("failed", "target failed: \"quoted\"")});
        // The default policy does not know the effect, so a tool that threw
        // after it started may have acted: the model is told so, and a
        // brokered run pauses for reconciliation instead.
        row({.name = "tool_exception_unknown_effect", .act = Act::Throw,
             .expect = tool_result("reconciliation_required", "target failed"),
             .brokered = reconciliation()});
        row({.name = "non_std_exception", .act = Act::ThrowNonStd,
             .expect = tool_result("reconciliation_required", "unknown exception"),
             .brokered = reconciliation()});
        row({.name = "unknown_tool", .call_unknown = true,
             .expect = tool_result("rejected", "Tool not found: missing")});
        row({.name = "lookup_failure", .throwing_lookup = true,
             .expect = tool_result("failed", "name lookup failed")});
        row({.name = "policy_timeout", .act = Act::Sleep, .policy_timeout = true,
             .expect = tool_result("timed_out", "deadline expired"),
             .brokered = reconciliation()});
        row({.name = "tool_raises_hook_block", .act = Act::ThrowHookBlock,
             .expect = hook_blocked("nested-invocation", HookExecutionState::TimedOut)});
        // A failed Hook with no attempt left reports failed: only a deadline
        // that elapsed is timed_out.
        row({.name = "hook_blocks_before_tool", .failing_hook = true,
             .expect = hook_blocked("*", HookExecutionState::Failed)});
        // A call that would start after the run's deadline does not start,
        // and none of its Hooks run, with or without Hooks configured.
        row({.name = "run_deadline_elapsed", .expired_deadline = true,
             .expect = tool_result("expired", "run deadline elapsed before the tool started")});
        row({.name = "run_deadline_elapsed_with_hooks", .failing_hook = true,
             .expired_deadline = true,
             .expect = tool_result("expired", "run deadline elapsed before the tool started")});
        // A Hook that cannot even run fails closed too, instead of turning
        // into an error message for every call.
        row({.name = "hook_cannot_run", .anonymous = true,
             .expect = hook_blocked({}, HookExecutionState::Failed)});
        row({.name = "run_cancelled", .act = Act::CancelRun, .expect = cancelled()});
        row({.name = "gate_interrupt", .gate_interrupt = true,
             .expect = interrupted("target needs a human", "tool", "target")});
        row({.name = "tool_raises_interrupt", .act = Act::ThrowInterrupt,
             .expect = interrupted("target needs approval", "cmd", "deploy")});
        return rows;
    }();
    return table;
}

// ── Paths ──────────────────────────────────────────────────────────────────

enum class PathKind {
    DispatchSingle, DispatchBatch, BrokeredSingle, BrokeredBatch,
    GraphNodeSingle, GraphNodeBatch, GraphRunResources, GraphSubgraph,
    AgentRun, AgentRunStream, PlanExecute, DeepResearch,
};

struct Path {
    PathKind kind;
    const char* name;
    bool batch;
    bool brokered = false;
    // The calls are dispatched before any provider request of this path, so an
    // already-expired run deadline reaches the tool boundary first.
    bool dispatch_first = false;
};

// Every path applies the same tool Hooks: direct dispatch through the
// ToolExecutionContext, llm::Agent through set_hook_runtime, and graph runs
// through the engine's HookRuntime (or RunResources::hook_runtime), including
// a subgraph's calls under its parent's HookRuntime.
const std::vector<Path>& paths() {
    static const std::vector<Path> all = {
        {PathKind::DispatchSingle, "dispatch_single", false, false, true},
        {PathKind::DispatchBatch, "dispatch_batch", true, false, true},
        {PathKind::BrokeredSingle, "brokered_single", false, true, true},
        {PathKind::BrokeredBatch, "brokered_batch", true, true, true},
        {PathKind::GraphNodeSingle, "graph_tool_dispatch_single", false, false, true},
        {PathKind::GraphNodeBatch, "graph_tool_dispatch_batch", true, false, true},
        {PathKind::GraphRunResources, "graph_run_resources_hooks_batch", true, false, true},
        {PathKind::GraphSubgraph, "graph_subgraph_tool_dispatch_batch", true, false, true},
        {PathKind::AgentRun, "agent_run", false, false, false},
        // Dispatched first, but the run's next step is a provider call, which
        // an expired deadline refuses before the model sees the results.
        {PathKind::AgentRunStream, "agent_run_stream_batch", true, false, false},
        {PathKind::PlanExecute, "plan_execute_batch", true},
        {PathKind::DeepResearch, "deep_research_researcher", false},
    };
    return all;
}

bool applies(const Scenario& scenario, const Path& path) {
    if (scenario.expired_deadline && !path.dispatch_first) return false;
    return true;
}

// Everything one run needs, rebuilt per (path, scenario).
struct Setup {
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    std::vector<std::unique_ptr<Tool>> tools;
    std::vector<ToolCall> calls;
    ToolGate gate;
    std::shared_ptr<HookRuntime> hooks;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::shared_ptr<ToolExecutionController> controller;
    std::shared_ptr<CancelToken> token = std::make_shared<CancelToken>();
    std::string owner_scope = "tenant";

    std::vector<Tool*> view() const {
        std::vector<Tool*> out;
        for (const auto& tool : tools) out.push_back(tool.get());
        return out;
    }
};

Setup make_setup(const Scenario& scenario, const Path& path) {
    Setup setup;
    setup.probe->run_token = setup.token;
    setup.tools.push_back(std::make_unique<SiblingTool>(setup.probe));
    if (scenario.throwing_lookup) setup.tools.push_back(std::make_unique<ThrowingNameTool>());
    setup.tools.push_back(std::make_unique<TargetTool>(scenario.act, setup.probe));
    setup.calls.push_back(ToolCall{kTargetCall, scenario.call_unknown ? "missing" : "target", "{}"});
    if (path.batch) setup.calls.push_back(ToolCall{kSiblingCall, "sibling", "{}"});
    if (scenario.gate_interrupt) {
        setup.gate = [](ToolCall call, ToolGateContext context) -> asio::awaitable<ToolDecision> {
            if (call.name == "target" && !context.resume_value)
                co_return ToolDecision::interrupt("target needs a human", json{{"tool", "target"}});
            co_return ToolDecision::allow();
        };
    }
    if (scenario.failing_hook) setup.hooks = before_tool_hooks(std::make_shared<HookTarget>(true));
    if (scenario.anonymous) {
        setup.hooks = before_tool_hooks(std::make_shared<HookTarget>(false));
        setup.owner_scope.clear();
    }
    if (scenario.expired_deadline) setup.deadline = std::chrono::steady_clock::now() - 1ms;
    if (scenario.policy_timeout || scenario.read_only) {
        auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
        ToolExecutionPolicy policy;
        if (scenario.policy_timeout) {
            policy.execution_timeout = 20ms;
            policy.queue_timeout = 5s;
        }
        if (scenario.read_only) policy.effect = ToolEffectClass::ReadOnly;
        policies->upsert("target", policy);
        setup.controller = std::make_shared<ToolExecutionController>(
            ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    }
    return setup;
}

sp::Message assistant_calls(const std::vector<ToolCall>& calls) {
    sp::Message message;
    message.role = sp::Role::Assistant;
    for (const auto& call : calls)
        message.parts.emplace_back(sp::ToolCall{call.id, call.name, sp::ToolCallKind::ClientExecuted,
                                                test::document(call.arguments)});
    return message;
}

sp::runtime::Result calls_reply(const std::vector<ToolCall>& calls) {
    sp::Completion completion;
    completion.messages.push_back(assistant_calls(calls));
    completion.stop = {sp::StopKind::ToolUse, "tool_calls"};
    return std::make_shared<const sp::Outcome>(std::move(completion));
}

std::string system_text(const ProviderRequest& request) {
    const auto& payload = std::get<sp::chat::Request>(request.payload);
    std::string text;
    if (payload.canonical_messages.empty()) return text;
    for (const auto& part : payload.canonical_messages.front().parts)
        if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
    return text;
}

// Tool results in a provider request: what the model is about to read.
std::vector<Seen> seen_in(const ProviderRequest& request) {
    std::vector<Seen> out;
    for (const auto& message : std::get<sp::chat::Request>(request.payload).canonical_messages)
        if (message.role == sp::Role::Tool) collect_seen(message, out);
    return out;
}

struct Recorder {
    std::mutex mutex;
    std::vector<Seen> seen;
    void add(std::vector<Seen> items) {
        std::lock_guard lock(mutex);
        for (auto& item : items) seen.push_back(std::move(item));
    }
};

Outcome run_dispatch(Setup setup, bool brokered) {
    ToolExecutionContext execution;
    execution.cancel_token = setup.token;
    execution.controller = setup.controller;
    execution.hook_runtime = setup.hooks;
    execution.deadline = setup.deadline;
    execution.identity = {setup.owner_scope, "root-run", "thread", ""};
    if (brokered) {
        execution.effect_broker = std::make_shared<PassThroughBroker>();
        execution.effect_task_id = "task";
    }
    Outcome outcome;
    try {
        const auto messages = neograph::async::run_sync(dispatch_tool_calls(
            setup.calls, setup.view(), setup.gate, {}, std::move(execution)));
        for (const auto& message : messages) collect_seen(message, outcome.seen);
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    return outcome;
}

class EmitCallsNode final : public GraphNode {
public:
    explicit EmitCallsNode(json calls) : calls_(std::move(calls)) {}
    asio::awaitable<NodeResult> run(NodeInput) override {
        json assistant{{"role", "assistant"}, {"content", ""}, {"tool_calls", calls_}};
        NodeResult out;
        out.writes.push_back(ChannelWrite{"messages", json::array({assistant})});
        co_return out;
    }
    std::string get_name() const override { return "model"; }

private:
    json calls_;
};

RunMetadata run_metadata(const Setup& setup) {
    RunMetadata metadata;
    metadata.owner_scope = setup.owner_scope;
    metadata.run_id = "root-run";
    metadata.deadline = setup.deadline;
    return metadata;
}

json tool_graph(const std::vector<ToolCall>& tool_calls, const std::string& name) {
    json calls = json::array();
    for (const auto& call : tool_calls)
        calls.push_back(json{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    return json{
        {"name", name},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"model", {{"type", "tool_failure_contract_model"}, {"calls", calls}}},
                   {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "model"}},
                               json{{"from", "model"}, {"to", "tools"}},
                               json{{"from", "tools"}, {"to", "__end__"}}})}};
}

void register_emit_node() {
    NodeFactory::instance().register_type("tool_failure_contract_model",
        [](const std::string&, const json& config, const NodeContext&) {
            return std::make_unique<EmitCallsNode>(config.at("calls"));
        });
}

// kind: GraphNode*: hooks on the engine; GraphRunResources: hooks only in the
// run's RunResources; GraphSubgraph: the tool_dispatch node runs in a
// subgraph whose engine has no Hooks, under a parent engine that has them.
Outcome run_graph_node(Setup setup, PathKind kind) {
    register_emit_node();
    NodeContext context;
    context.tools = ToolSet(std::move(setup.tools));
    std::unique_ptr<GraphEngine> engine;
    std::shared_ptr<GraphEngine> child;
    if (kind == PathKind::GraphSubgraph) {
        child = GraphEngine::compile(tool_graph(setup.calls, "tool_failure_child"), context);
        if (setup.controller) child->set_tool_execution_controller(setup.controller);
        NodeFactory::instance().register_type("tool_failure_contract_subgraph",
            [child](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<SubgraphNode>(name, child);
            });
        const json parent{
            {"name", "tool_failure_parent"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"child", {{"type", "tool_failure_contract_subgraph"}}}}},
            {"edges", json::array({json{{"from", "__start__"}, {"to", "child"}},
                                   json{{"from", "child"}, {"to", "__end__"}}})}};
        engine = GraphEngine::compile(parent, NodeContext{},
                                      std::make_shared<InMemoryCheckpointStore>());
    } else {
        engine = GraphEngine::compile(tool_graph(setup.calls, "tool_failure_contract"), context,
                                      std::make_shared<InMemoryCheckpointStore>());
    }
    if (setup.gate) engine->set_tool_gate(setup.gate);
    if (setup.controller) engine->set_tool_execution_controller(setup.controller);
    RunResources resources;
    if (kind == PathKind::GraphRunResources) resources.hook_runtime = setup.hooks;
    else engine->set_hook_runtime(setup.hooks);
    RunConfig config;
    config.thread_id = "tool-failure-contract";
    config.cancel_token = setup.token;
    Outcome outcome;
    try {
        const auto result = neograph::async::run_sync(engine->run_async(
            config, run_metadata(setup), std::move(resources)));
        if (result.interrupted) {
            outcome.interrupt = result.interrupt_value.value("reason", "");
            if (result.interrupt_value.contains("value"))
                outcome.interrupt_payload = result.interrupt_value["value"];
        }
        for (const auto& item : result.output["channels"]["messages"]["value"]) {
            ChatMessage message;
            from_json(item, message);
            collect_seen(message, outcome.seen);
        }
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    return outcome;
}

Outcome run_agent(Setup setup, bool stream) {
    auto recorder = std::make_shared<Recorder>();
    const bool loop_dispatch = !stream;
    auto calls = setup.calls;
    auto provider = std::make_shared<test::LocalProvider>(
        [recorder, calls, loop_dispatch](ProviderRequest request, const PreparedProviderRequest&,
                                         const test::LocalProvider::EventCallback&)
            -> asio::awaitable<sp::runtime::Result> {
            auto seen = seen_in(request);
            if (seen.empty() && loop_dispatch) co_return calls_reply(calls);
            recorder->add(std::move(seen));
            co_return test::success("done");
        }, "contract-agent");
    llm::Agent agent(provider, std::move(setup.tools), "", "fixture-model");
    // The Agent path keeps its own Hook binding: set_hook_runtime.
    if (setup.gate) agent.set_tool_gate(setup.gate);
    if (setup.hooks) agent.set_hook_runtime(setup.hooks);
    if (setup.controller) agent.set_tool_execution_controller(setup.controller);
    std::vector<sp::Message> history{test::message("go", sp::Role::User)};
    // run_stream resumes a turn whose calls are still pending: they are
    // dispatched before any provider request.
    if (stream) history.push_back(assistant_calls(setup.calls));
    ToolExecutionContext execution;
    execution.cancel_token = setup.token;
    execution.deadline = setup.deadline;
    execution.identity.owner_scope = setup.owner_scope;
    execution.identity.root_run_id = "root-run";
    Outcome outcome;
    try {
        if (stream) (void)agent.run_stream(history, [](const sp::Event&) {}, 4, std::move(execution));
        else (void)agent.run(history, 4, std::move(execution));
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    outcome.seen = recorder->seen;
    return outcome;
}

Outcome run_plan_execute(Setup setup) {
    auto recorder = std::make_shared<Recorder>();
    auto calls = setup.calls;
    auto provider = std::make_shared<test::LocalProvider>(
        [recorder, calls](ProviderRequest request, const PreparedProviderRequest&,
                          const test::LocalProvider::EventCallback&)
            -> asio::awaitable<sp::runtime::Result> {
            const auto system = system_text(request);
            if (system == "PLANNER") co_return test::success(R"(["use the tools"])");
            if (system == "RESPONDER") co_return test::success("final");
            auto seen = seen_in(request);
            if (seen.empty()) co_return calls_reply(calls);
            recorder->add(std::move(seen));
            co_return test::success("step done");
        }, "contract-plan");
    auto engine = create_plan_execute_graph(provider, std::move(setup.tools), "PLANNER", "EXECUTOR",
                                            "RESPONDER", "fixture-model", 3);
    engine->set_checkpoint_store(std::make_shared<InMemoryCheckpointStore>());
    if (setup.gate) engine->set_tool_gate(setup.gate);
    if (setup.controller) engine->set_tool_execution_controller(setup.controller);
    engine->set_hook_runtime(setup.hooks);
    RunConfig config;
    config.thread_id = "tool-failure-plan";
    config.input = json{{"messages", json::array({json{{"role", "user"}, {"content", "go"}}})}};
    config.max_steps = 20;
    config.cancel_token = setup.token;
    Outcome outcome;
    try {
        const auto result = engine->run(config, run_metadata(setup));
        if (result.interrupted) {
            outcome.interrupt = result.interrupt_value.value("reason", "");
            if (result.interrupt_value.contains("value"))
                outcome.interrupt_payload = result.interrupt_value["value"];
        }
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    outcome.seen = recorder->seen;
    return outcome;
}

sp::runtime::Result research_reply(std::string text, sp::StopKind stop = sp::StopKind::EndTurn,
                                   std::vector<ToolCall> calls = {}) {
    if (!calls.empty()) return calls_reply(calls);
    sp::Completion completion;
    completion.messages.push_back(test::message(std::move(text)));
    completion.stop = {stop, "stop"};
    return std::make_shared<const sp::Outcome>(std::move(completion));
}

Outcome run_deep_research(Setup setup) {
    auto recorder = std::make_shared<Recorder>();
    auto calls = setup.calls;
    auto supervisor_turns = std::make_shared<std::atomic<int>>(0);
    auto provider = std::make_shared<test::LocalProvider>(
        [recorder, calls, supervisor_turns](ProviderRequest request, const PreparedProviderRequest&,
                                            const test::LocalProvider::EventCallback&)
            -> asio::awaitable<sp::runtime::Result> {
            const auto system = system_text(request);
            if (system.find("lead research supervisor") != std::string::npos) {
                if (supervisor_turns->fetch_add(1) == 0)
                    co_return calls_reply({ToolCall{"s1", "conduct_research",
                                                    R"({"research_topic":"topic A"})"}});
                co_return calls_reply({ToolCall{"s2", "research_complete", "{}"}});
            }
            if (system.find("focused researcher") != std::string::npos) {
                auto seen = seen_in(request);
                if (seen.empty()) co_return calls_reply(calls);
                recorder->add(std::move(seen));
                co_return research_reply("raw findings");
            }
            if (system.find("Compress raw research notes") != std::string::npos)
                co_return research_reply("COMPRESSED");
            if (system.find("research brief") != std::string::npos) co_return research_reply("brief");
            co_return research_reply("## report");
        }, "contract-research");
    DeepResearchConfig settings;
    settings.model = "fixture-model";
    auto engine = create_deep_research_graph(provider, std::move(setup.tools), settings);
    engine->set_checkpoint_store(std::make_shared<InMemoryCheckpointStore>());
    if (setup.gate) engine->set_tool_gate(setup.gate);
    if (setup.controller) engine->set_tool_execution_controller(setup.controller);
    engine->set_hook_runtime(setup.hooks);
    RunConfig config;
    config.thread_id = "tool-failure-research";
    config.input = {{"user_query", "q"}};
    config.max_steps = 40;
    config.cancel_token = setup.token;
    Outcome outcome;
    try {
        const auto result = engine->run(config, run_metadata(setup));
        if (result.interrupted) {
            outcome.interrupt = result.interrupt_value.value("reason", "");
            if (result.interrupt_value.contains("value"))
                outcome.interrupt_payload = result.interrupt_value["value"];
        }
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    outcome.seen = recorder->seen;
    return outcome;
}

Outcome run_path(const Path& path, Setup setup) {
    switch (path.kind) {
        case PathKind::DispatchSingle:
        case PathKind::DispatchBatch:
            return run_dispatch(std::move(setup), false);
        case PathKind::BrokeredSingle:
        case PathKind::BrokeredBatch:
            return run_dispatch(std::move(setup), true);
        case PathKind::GraphNodeSingle:
        case PathKind::GraphNodeBatch:
        case PathKind::GraphRunResources:
        case PathKind::GraphSubgraph:
            return run_graph_node(std::move(setup), path.kind);
        case PathKind::AgentRun:
            return run_agent(std::move(setup), false);
        case PathKind::AgentRunStream:
            return run_agent(std::move(setup), true);
        case PathKind::PlanExecute:
            return run_plan_execute(std::move(setup));
        case PathKind::DeepResearch:
            return run_deep_research(std::move(setup));
    }
    return {};
}

void check(const Expect& expect, const Outcome& outcome) {
    SCOPED_TRACE(describe(outcome));
    switch (expect.kind) {
        case Expect::Kind::ToolResult: {
            EXPECT_FALSE(outcome.error) << "an ordinary tool failure must not stop the run";
            EXPECT_FALSE(outcome.interrupt);
            const auto* target = outcome.target();
            ASSERT_NE(target, nullptr) << "the model never saw the failed call";
            EXPECT_EQ(target->status, expect.status);
            EXPECT_TRUE(target->is_error) << "a failure must reach the model as an error";
            const auto body = json::parse(target->content);
            ASSERT_TRUE(body.is_object()) << target->content;
            EXPECT_NE(body.value("error", "").find(expect.text), std::string::npos) << target->content;
            EXPECT_EQ(body.value("status", ""), expect.status) << target->content;
            break;
        }
        case Expect::Kind::HookBlocked: {
            EXPECT_EQ(outcome.target(), nullptr) << "a Hook block must never reach the model";
            ASSERT_TRUE(outcome.error) << "a Hook block must stop the run";
            try {
                std::rethrow_exception(outcome.error);
            } catch (const HookBoundaryBlocked& blocked) {
                // "*": the real invocation, whatever its content-derived id is.
                if (expect.text == "*") EXPECT_FALSE(blocked.invocation_id().empty());
                else EXPECT_EQ(blocked.invocation_id(), expect.text);
                if (expect.state) EXPECT_EQ(blocked.state(), *expect.state);
            } catch (...) {
                ADD_FAILURE() << "expected HookBoundaryBlocked";
            }
            break;
        }
        case Expect::Kind::Cancelled:
            EXPECT_EQ(outcome.target(), nullptr);
            ASSERT_TRUE(outcome.error) << "cancellation must stop the run";
            EXPECT_THROW(std::rethrow_exception(outcome.error), CancelledException);
            break;
        case Expect::Kind::Interrupted:
            EXPECT_FALSE(outcome.error);
            EXPECT_EQ(outcome.target(), nullptr) << "an interrupt must never reach the model";
            ASSERT_TRUE(outcome.interrupt) << "the run must pause";
            EXPECT_EQ(*outcome.interrupt, expect.text);
            if (!expect.payload_key.empty()) {
                ASSERT_TRUE(outcome.interrupt_payload.is_object());
                EXPECT_EQ(outcome.interrupt_payload.value(expect.payload_key, ""), expect.payload_value);
            }
            break;
    }
}

}  // namespace

// Every path × every failure category. One loop rather than one TEST per cell
// so that adding a path or a category forces it through the whole table.
TEST(ToolFailureContract, EveryPathAppliesTheSameContract) {
    for (const auto& path : paths()) {
        for (const auto& scenario : scenarios()) {
            if (!applies(scenario, path)) continue;
            SCOPED_TRACE(std::string(path.name) + " / " + scenario.name);
            auto setup = make_setup(scenario, path);
            auto probe = setup.probe;
            const auto outcome = run_path(path, std::move(setup));
            const auto& expect = path.brokered && scenario.brokered ? *scenario.brokered
                                                                   : scenario.expect;
            check(expect, outcome);
            if (scenario.gate_interrupt) {
                EXPECT_EQ(probe->target_runs.load(), 0) << "the gate must decide before anything runs";
                EXPECT_EQ(probe->sibling_runs.load(), 0);
            }
        }
    }
}

// Every path that runs a model's tool calls crosses the same tool Hooks: one
// BeforeToolExecution and one AfterToolExecution event per call, with the same
// payload, whether the calls come from direct dispatch, llm::Agent, a graph's
// tool_dispatch node (engine Hooks, RunResources Hooks, or a parent's Hooks
// reaching a subgraph), the plan-and-execute executor or the deep-research
// researcher.
TEST(ToolFailureContract, EveryPathEmitsTheSameToolHooks) {
    for (const auto& path : paths()) {
        if (path.brokered) continue;  // the broker path is dispatch with a broker
        SCOPED_TRACE(path.name);
        Scenario scenario{.name = "hooks_observe", .act = Act::Succeed};
        auto setup = make_setup(scenario, path);
        auto target = std::make_shared<HookTarget>(false);
        setup.hooks = tool_hooks(target, {HookPhase::BeforeToolExecution,
                                          HookPhase::AfterToolExecution});
        const auto calls = setup.calls;
        const auto outcome = run_path(path, std::move(setup));
        ASSERT_FALSE(outcome.error) << describe(outcome);
        const auto seen = target->seen();
        for (const auto& call : calls) {
            int before = 0;
            int after = 0;
            for (const auto& event : seen) {
                if (event.value("tool_call_id", "") != call.id) continue;
                EXPECT_EQ(event.value("tool_name", ""), call.name);
                if (event.value("phase", "") == "before_tool_execution") ++before;
                if (event.value("phase", "") == "after_tool_execution") {
                    ++after;
                    EXPECT_EQ(event.value("status", ""), "succeeded");
                }
            }
            EXPECT_EQ(before, 1) << call.id;
            EXPECT_EQ(after, 1) << call.id;
        }
    }
}

// The precedence when several calls of one batch stop the run is fixed, not
// a matter of which worker finished first: cancellation, then a Hook block,
// then an interrupt.
TEST(ToolFailureContract, BatchPrecedenceIsDeterministic) {
    auto probe = std::make_shared<Probe>();
    probe->run_token = std::make_shared<CancelToken>();
    TargetTool interrupting(Act::ThrowInterrupt, probe);
    class Blocking final : public Tool {
    public:
        ChatTool get_definition() const override { return {"blocking", "", json::object()}; }
        std::string get_name() const override { return "blocking"; }
        std::string execute(const json&) override {
            throw HookBoundaryBlocked("inner", HookExecutionState::Failed, "inner hook blocked");
        }
    } blocking;
    const std::vector<ToolCall> calls{{"call-1", "target", "{}"}, {"call-2", "blocking", "{}"}};
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(calls, {&interrupting, &blocking})),
                 HookBoundaryBlocked);
    const std::vector<ToolCall> reversed{{"call-2", "blocking", "{}"}, {"call-1", "target", "{}"}};
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(reversed, {&interrupting, &blocking})),
                 HookBoundaryBlocked);
}

// The controller is the boundary every path shares, so it is where tool code's
// control signals have to survive.
TEST(ToolFailureContract, ControllerPropagatesControlSignalsFromToolCode) {
    auto probe = std::make_shared<Probe>();
    TargetTool blocking(Act::ThrowHookBlock, probe);
    TargetTool interrupting(Act::ThrowInterrupt, probe);
    TargetTool failing(Act::Throw, probe);
    auto controller = std::make_shared<ToolExecutionController>();
    try {
        (void)neograph::async::run_sync(controller->execute_result_async(blocking, json::object(), {}));
        ADD_FAILURE() << "a HookBoundaryBlocked from tool code became a typed result";
    } catch (const HookBoundaryBlocked& blocked) {
        EXPECT_EQ(blocked.invocation_id(), "nested-invocation");
        EXPECT_EQ(blocked.state(), HookExecutionState::TimedOut);
    }
    EXPECT_THROW((void)neograph::async::run_sync(
                     controller->execute_result_async(interrupting, json::object(), {})),
                 NodeInterrupt);
    const auto failed = neograph::async::run_sync(
        controller->execute_result_async(failing, json::object(), {}));
    EXPECT_FALSE(failed.succeeded());
    EXPECT_NE(failed.error.find("target failed"), std::string::npos);
}

// A single-flight leader that stops the run must still release its followers.
TEST(ToolFailureContract, SingleFlightFollowersAreReleasedWhenTheLeaderStopsTheRun) {
    class SlowBlocking final : public Tool, public ContextualAsyncTool {
    public:
        ChatTool get_definition() const override { return {"shared", "", json::object()}; }
        std::string get_name() const override { return "shared"; }
        std::string execute(const json&) override { return "unused"; }
        asio::awaitable<std::string> execute_async(const json&, ToolExecutionContext) override {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(50ms);
            co_await timer.async_wait(asio::use_awaitable);
            throw HookBoundaryBlocked("shared-inner", HookExecutionState::Failed, "inner blocked");
        }
    } tool;
    auto policies = std::make_shared<ToolExecutionPolicyRegistry>();
    ToolExecutionPolicy policy;
    policy.concurrency = ToolConcurrency::SingleFlight;
    policy.effect = ToolEffectClass::ReadOnly;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.queue_timeout = 10s;
    policies->upsert("shared", policy);
    ToolExecutionContext context;
    context.controller = std::make_shared<ToolExecutionController>(
        ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>()});
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(
                     {{"a", "shared", "{}"}, {"b", "shared", "{}"}}, {&tool}, {}, {}, context)),
                 HookBoundaryBlocked);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s)
        << "the follower waited for its queue timeout";
    // The key is free again: a later call becomes a new leader instead of
    // waiting on the abandoned one.
    const auto later = std::chrono::steady_clock::now();
    EXPECT_THROW(neograph::async::run_sync(dispatch_tool_calls(
                     {{"c", "shared", "{}"}}, {&tool}, {}, {}, context)),
                 HookBoundaryBlocked);
    EXPECT_LT(std::chrono::steady_clock::now() - later, 5s);
}

// A Hook target is not a model's tool call. A control signal raised inside it
// (say, a nested Hook block) is a failed attempt of that Hook, settled in the
// journal exactly like any other exception from the target; it must not leave
// the attempt's lease unsettled.
TEST(ToolFailureContract, HookTargetControlSignalsSettleLikeOtherTargetFailures) {
    class Target final : public Tool {
    public:
        explicit Target(bool blocked) : blocked_(blocked) {}
        ChatTool get_definition() const override { return {"audit", "", json::object()}; }
        std::string get_name() const override { return "audit"; }
        std::string execute(const json&) override {
            if (blocked_) throw HookBoundaryBlocked("inner", HookExecutionState::Failed, "inner hook blocked");
            throw std::runtime_error("audit sink unavailable");
        }

    private:
        bool blocked_;
    };
    auto settle = [](bool blocked) {
        Target target(blocked);
        HookTargetResolver resolver = [&target](std::string_view id) -> std::optional<HookTargetContract> {
            if (id != "audit") return std::nullopt;
            return HookTargetContract{&target, {"audit"}, ToolEffectClass::ReadOnly};
        };
        auto registry = std::make_shared<HookRegistry>(resolver);
        HookDefinitionData data;
        data.priority = 1;
        data.phase = HookPhase::BeforeToolExecution;
        data.target_id = "audit";
        data.required_capabilities = {"audit"};
        data.input_mapper = {HookInputMapperKind::JsonPointer, "/arguments", json(), {}};
        registry->admit(HookDefinition::create(std::move(data)));
        auto journal = std::make_shared<InMemoryHookJournal>();
        MandatoryHookRunner runner(journal, registry, std::make_shared<NativeHookExecutionAdapter>(
            resolver, std::make_shared<ToolExecutionController>()));
        const auto event = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution,
            "tool_execution", "owner", "run", json{{"arguments", json::object()}}});
        const auto invocation = registry->plan(event).front();
        try {
            neograph::async::run_sync(runner.run_async(event, std::chrono::system_clock::now() + 1min));
            ADD_FAILURE() << "a FailClosed Hook whose target failed must block";
        } catch (const HookBoundaryBlocked& outer) {
            EXPECT_EQ(outer.invocation_id(), invocation.id()) << "the outer Hook owns the block";
        }
        const auto entry = journal->get(invocation.id());
        EXPECT_TRUE(entry);
        return entry ? entry->data().state : HookExecutionState::Dispatched;
    };
    const auto ordinary = settle(false);
    EXPECT_NE(ordinary, HookExecutionState::Dispatched);
    EXPECT_EQ(settle(true), ordinary) << "a nested block settled differently from a target failure";
}

// A Hook block is a policy decision. A node retry policy re-running the node
// would re-run the whole tool batch to ask the same question again.
TEST(ToolFailureContract, NodeRetryPolicyDoesNotRetryAHookBlock) {
    auto probe = std::make_shared<Probe>();
    probe->run_token = std::make_shared<CancelToken>();
    NodeFactory::instance().register_type("tool_failure_contract_model",
        [](const std::string&, const json& config, const NodeContext&) {
            return std::make_unique<EmitCallsNode>(config.at("calls"));
        });
    const json definition{
        {"name", "tool_failure_retry"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"model", {{"type", "tool_failure_contract_model"},
                              {"calls", json::array({json{{"id", kTargetCall}, {"name", "target"},
                                                          {"arguments", "{}"}},
                                                     json{{"id", kSiblingCall}, {"name", "sibling"},
                                                          {"arguments", "{}"}}})}}},
                   {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "model"}},
                               json{{"from", "model"}, {"to", "tools"}},
                               json{{"from", "tools"}, {"to", "__end__"}}})}};
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<SiblingTool>(probe));
    tools.push_back(std::make_unique<TargetTool>(Act::ThrowHookBlock, probe));
    NodeContext context;
    context.tools = ToolSet(std::move(tools));
    auto engine = GraphEngine::compile(definition, context);
    RetryPolicy retry;
    retry.max_retries = 2;
    retry.initial_delay_ms = 1;
    engine->set_node_retry_policy("tools", retry);
    RunConfig config;
    config.thread_id = "tool-failure-retry";
    Outcome outcome;
    try {
        (void)engine->run(config);
    } catch (...) {
        outcome.capture(std::current_exception());
    }
    ASSERT_TRUE(outcome.error);
    EXPECT_THROW(std::rethrow_exception(outcome.error), HookBoundaryBlocked);
    EXPECT_EQ(probe->target_runs.load(), 1) << "the blocked batch was retried";
    EXPECT_EQ(probe->sibling_runs.load(), 1) << "the sibling's effect was applied again";
}

#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
// MCP-backed tools follow the same contract: a tool-level isError result and a
// JSON-RPC error are ordinary failures the model sees, in one call or a batch.
TEST(ToolFailureContract, McpErrorsAreToolResults) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "mcp_stdio_failures.py";
#ifdef _WIN32
    const std::string python = "python";
#else
    const std::string python = "python3";
#endif
    mcp::MCPClient client({python, fixture.string()});
    client.initialize();
    auto owned = client.get_tools();
    std::vector<Tool*> tools;
    for (const auto& tool : owned) tools.push_back(tool.get());
    for (const bool batch : {false, true}) {
        SCOPED_TRACE(batch ? "batch" : "single");
        std::vector<ToolCall> calls{{"call-is-error", "is_error", "{}"}};
        if (batch) {
            calls.push_back({"call-rpc-error", "rpc_error", "{}"});
            calls.push_back({"call-ok", "ok", "{}"});
        }
        const auto messages = neograph::async::run_sync(dispatch_tool_calls(calls, tools));
        ASSERT_EQ(messages.size(), calls.size());
        // The default policy does not know what a remote tool did before it
        // failed, so the status says the effect needs reconciling.
        EXPECT_EQ(messages[0].tool_status, "reconciliation_required");
        EXPECT_NE(json::parse(messages[0].content).value("error", "").find("remote tool refused"),
                  std::string::npos) << messages[0].content;
        if (batch) {
            EXPECT_EQ(messages[1].tool_status, "reconciliation_required");
            EXPECT_NE(json::parse(messages[1].content).value("error", "").find("server exploded"),
                      std::string::npos) << messages[1].content;
            EXPECT_EQ(messages[2].tool_status, "succeeded");
            EXPECT_EQ(messages[2].content, "fine");
        }
    }
}
#endif
