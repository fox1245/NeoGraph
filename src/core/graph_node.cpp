#include <neograph/graph/node.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/graph/engine.h>   // RunContext (forward-declared in node.h)
#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>

#include "run_context_runtime.h"
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace neograph::graph {

namespace {

std::string length_frame(std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

struct ProviderBrokerScope {
    std::shared_ptr<ProviderCallBroker> broker;
    ProviderCallIdentity identity;
};

ProviderBrokerScope provider_broker_scope(const RunContext& context,
                                          const std::string& node_name) {
    const auto runtime = detail::runtime_for(context);
    auto identity = make_provider_call_identity(context, node_name);
    if (runtime && runtime->provider_call_broker &&
        (identity.task_id.empty() || context.thread_id.empty()))
        throw std::invalid_argument("Provider broker requires a thread-scoped Core task identity");
    return {runtime ? runtime->provider_call_broker : nullptr, std::move(identity)};
}


}  // namespace

std::shared_ptr<ProviderCallBroker> provider_call_broker(const RunContext& context) {
    const auto runtime = detail::runtime_for(context);
    return runtime ? runtime->provider_call_broker : nullptr;
}

// v1.0 destructive removal (9b): the 8-virtual `execute*` legacy chain,
// the ExecuteDefaultGuard recursion guard, and the
// `GraphNodeMissingOverride` indirection are all gone. `GraphNode::run`
// is now pure-virtual — every subclass must implement it. The engine's
// `NodeExecutor::execute_node_with_retry_async` dispatches via run()
// directly; there is no fallback chain.
//
// What this file provides now: only the built-in node implementations
// (LLMCallNode, ToolDispatchNode, IntentClassifierNode, SubgraphNode).
// Their `run()` overrides live in their own sections below.

SyncGraphNode::SyncGraphNode(std::string name) : name_(std::move(name)) {}

SyncGraphNode::~SyncGraphNode() = default;

asio::awaitable<NodeOutput> SyncGraphNode::run(NodeInput in) {
    co_return run_sync(in);
}

std::string SyncGraphNode::get_name() const {
    return name_;
}

// =========================================================================
// LLMCallNode
// =========================================================================

LLMCallNode::LLMCallNode(const std::string& name, const NodeContext& ctx)
    : name_(name)
    , provider_(ctx.provider)
    , tools_owner_(ctx.tools)
    , tools_(tools_owner_.view())
    , model_(ctx.model)
    , instructions_(ctx.instructions)
    , controls_(ctx.provider_controls)
{}

ProviderRequest LLMCallNode::build_params(const GraphState& state) const {
    auto messages = state.get_provider_messages();
    if (!instructions_.empty()) {
        auto system = portable_message(ChatMessage{"system", instructions_});
        if (!messages.empty() && messages.front().role == sp::Role::System)
            messages.front() = std::move(system);
        else messages.insert(messages.begin(), std::move(system));
    }
    std::vector<ChatTool> tools;
    tools.reserve(tools_.size());
    for (auto* tool : tools_) tools.push_back(tool->get_definition());
    return make_provider_request(*provider_, model_, std::move(messages), std::move(tools), controls_);
}

asio::awaitable<NodeOutput> LLMCallNode::run(NodeInput in) {
    auto request = build_params(in.state);
    request.cancel_token = in.ctx.cancel_token;
    request.options.deadline = in.ctx.deadline;
    request.mode = in.stream_cb || in.ctx.on_provider_event ? ProviderMode::Stream : ProviderMode::Collect;
    if (in.stream_cb || in.ctx.on_provider_event)
        request.on_event = [observer = in.ctx.on_provider_event, cb = in.stream_cb, name = name_](const sp::Event& event) {
            if (observer) observer(event);
            if (cb) if (const auto* delta = std::get_if<sp::PartDelta>(&event);
                delta && delta->payload.kind == sp::PartKind::Text && delta->payload.channel == sp::DeltaChannel::Content)
                (*cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name, std::string(delta->payload.bytes)});
        };
    std::vector<sp::Message> host;
    if (!instructions_.empty()) host.push_back(portable_message(ChatMessage{"system", instructions_}));
    auto broker = provider_broker_scope(in.ctx, name_);
    auto result = co_await observe_provider_result(in.ctx, invoke_provider(provider_, std::move(request), std::move(host), {},
                                           std::move(broker.broker), std::move(broker.identity)));
    record_usage(in.ctx, result);
    outcome_or_throw(result);
    NodeOutput out;
    out.writes.push_back(provider_messages_write(result));
    co_return out;
}

// =========================================================================
// ToolDispatchNode
// =========================================================================

ToolDispatchNode::ToolDispatchNode(const std::string& name, const NodeContext& ctx)
    : name_(name)
    , tools_owner_(ctx.tools)
    , tools_(tools_owner_.view())
{}

asio::awaitable<NodeOutput> ToolDispatchNode::run(NodeInput in) {
    auto messages = in.state.get_provider_messages();
    if (messages.empty()) co_return NodeOutput{};
    auto calls = pending_client_tool_calls(messages);
    if (calls.empty()) co_return NodeOutput{};

    // Tool execution lives in exactly one place (issue #87): both this node and
    // llm::Agent route through dispatch_tool_calls(). When the two had separate
    // copies they drifted — this one fanned the calls out, the agent ran them
    // one at a time — and every future capability at this boundary would have
    // drifted the same way.
    // The gate (issue #89) rides along on the RunContext, so it reaches both
    // dispatch paths through the one function they share.
    ToolGateContext gctx;
    gctx.resume_value = in.ctx.resume_value;
    gctx.thread_id    = in.ctx.thread_id;
    gctx.step         = in.ctx.step;
    auto execution = make_tool_execution_context(in.ctx);
    auto tool_msgs = co_await dispatch_tool_calls(
        std::move(calls), tools_, in.ctx.tool_gate, std::move(gctx),
        std::move(execution));

    std::vector<sp::Message> native;
    native.reserve(tool_msgs.size());
    for (const auto& message : tool_msgs) native.push_back(portable_message(message));
    NodeOutput out;
    out.writes.push_back(provider_messages_write(std::move(native)));
    co_return out;
}

// =========================================================================
// IntentClassifierNode
// =========================================================================

IntentClassifierNode::IntentClassifierNode(
    const std::string& name, const NodeContext& ctx,
    const std::string& prompt, std::vector<std::string> valid_routes)
    : name_(name)
    , provider_(ctx.provider)
    , model_(ctx.model)
    , prompt_(prompt)
    , valid_routes_(std::move(valid_routes))
{}

ProviderRequest IntentClassifierNode::build_params(const GraphState& state) const {
    const auto messages = state.get_provider_messages();
    sp::Message user;
    user.role = sp::Role::User;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == sp::Role::User) { user = *it; break; }
    }

    std::string sys_prompt = prompt_;
    if (sys_prompt.empty()) {
        sys_prompt = "Classify the user's intent. Respond with ONLY one of: ";
        for (size_t i = 0; i < valid_routes_.size(); ++i) {
            sys_prompt += valid_routes_[i];
            if (i + 1 < valid_routes_.size()) sys_prompt += ", ";
        }
        sys_prompt += "\nNo explanation, just the category name.";
    }

    ProviderControls controls;
    controls.temperature = 0.0;
    controls.max_output_tokens = 20;
    return make_provider_request(*provider_, model_,
        {portable_message(ChatMessage{"system", sys_prompt}),
         std::move(user)}, {}, controls);
}

std::vector<ChannelWrite> IntentClassifierNode::route_from(const std::string& intent) const {
    // Match against valid routes (case-insensitive substring match — the
    // existing contract. `valid_routes_` order is authoritative.)
    std::string best_route = valid_routes_.empty() ? intent : valid_routes_[0];
    for (const auto& r : valid_routes_) {
        if (intent.find(r) != std::string::npos) {
            best_route = r;
            break;
        }
    }
    return {ChannelWrite{"__route__", json(best_route)}};
}

asio::awaitable<NodeOutput> IntentClassifierNode::run(NodeInput in) {
    auto params = build_params(in.state);
    params.cancel_token = in.ctx.cancel_token;

    params.options.deadline = in.ctx.deadline;
    params.mode = in.stream_cb || in.ctx.on_provider_event ? ProviderMode::Stream : ProviderMode::Collect;
    if (in.stream_cb || in.ctx.on_provider_event)
        params.on_event = [observer = in.ctx.on_provider_event, cb = in.stream_cb, name = name_](const sp::Event& event) {
            if (observer) observer(event);
            if (cb) if (const auto* delta = std::get_if<sp::PartDelta>(&event);
                delta && delta->payload.kind == sp::PartKind::Text && delta->payload.channel == sp::DeltaChannel::Content)
                (*cb)(GraphEvent{GraphEvent::Type::LLM_TOKEN, name, std::string(delta->payload.bytes)});
        };
    const auto& messages = provider_request_messages(params);
    std::vector<sp::Message> host{messages.front()};
    std::vector<sp::Message> supplemental(messages.begin() + 1, messages.end());
    auto broker = provider_broker_scope(in.ctx, name_);
    auto result = co_await observe_provider_result(in.ctx, invoke_provider(provider_, std::move(params), std::move(host), std::move(supplemental),
                                          std::move(broker.broker), std::move(broker.identity)));
    record_usage(in.ctx, result);
    outcome_or_throw(result);
    NodeOutput out;
    out.writes = route_from(outcome_text(*result));
    co_return out;
}

// =========================================================================
// SubgraphNode
// =========================================================================

SubgraphNode::SubgraphNode(const std::string& name,
                           std::shared_ptr<GraphEngine> subgraph,
                           std::map<std::string, std::string> input_map,
                           std::map<std::string, std::string> output_map,
                           SubgraphPersistence persistence)
    : name_(name)
    , subgraph_(std::move(subgraph))
    , input_map_(std::move(input_map))
    , output_map_(std::move(output_map))
    , persistence_(persistence)
{
    if (!subgraph_) throw std::invalid_argument("SubgraphNode requires a child engine");
}

std::string SubgraphNode::checkpoint_thread_id(
    const std::string& parent_thread_id, int parent_step,
    std::string_view task_id, std::string_view graph_invocation_id) const {
    if (persistence_ == SubgraphPersistence::Stateless) return {};
    if (persistence_ == SubgraphPersistence::Legacy)
        return parent_thread_id.empty() ? std::string{}
            : "subgraph/" + length_frame(parent_thread_id) + length_frame(name_)
                + length_frame(std::to_string(parent_step)) + length_frame(task_id);
    if (parent_thread_id.empty())
        throw std::invalid_argument("Stateful subgraph policy requires a parent thread ID");
    if (persistence_ == SubgraphPersistence::PerThread)
        return "subgraph/thread/" + length_frame(parent_thread_id) + length_frame(name_);
    if (graph_invocation_id.empty())
        throw std::invalid_argument("PerInvocation subgraph requires a parent graph invocation ID");
    return "subgraph/run/" + length_frame(parent_thread_id) + length_frame(name_)
        + length_frame(graph_invocation_id) + length_frame(std::to_string(parent_step))
        + length_frame(task_id);
}

json SubgraphNode::build_subgraph_input(const GraphState& state) const {
    json input;

    if (input_map_.empty()) {
        // Default: pass all channels through (same name)
        for (const auto& ch_name : state.channel_names()) {
            input[ch_name] = state.get(ch_name);
        }
    } else {
        for (const auto& [parent_ch, child_ch] : input_map_) {
            input[child_ch] = state.get(parent_ch);
        }
    }

    return input;
}

std::vector<ChannelWrite> SubgraphNode::map_output_writes(
    const std::vector<ChannelWrite>& child_writes) const {
    std::vector<ChannelWrite> writes;
    writes.reserve(child_writes.size());
    for (const auto& child_write : child_writes) {
        auto parent_channel = child_write.channel;
        if (!output_map_.empty()) {
            const auto mapped = output_map_.find(child_write.channel);
            if (mapped == output_map_.end()) continue;
            parent_channel = mapped->second;
        }
        auto parent_write = child_write;
        parent_write.channel = std::move(parent_channel);
        writes.push_back(std::move(parent_write));
    }
    return writes;
}

asio::awaitable<NodeOutput> SubgraphNode::run(NodeInput in) {
    const auto runtime = detail::runtime_for(in.ctx);
    const std::string_view task_id = runtime
        ? runtime.invocation_id() : std::string_view("root");
    RunConfig config;
    config.thread_id = checkpoint_thread_id(
        in.ctx.thread_id, in.ctx.step, task_id,
        runtime ? std::string_view(runtime->graph_invocation_id) : std::string_view{});
    if (persistence_ == SubgraphPersistence::Stateless)
        config.thread_id = "subgraph/stateless/" + Checkpoint::generate_id();
    if (persistence_ == SubgraphPersistence::PerThread)
        config.resume_if_exists = true;
    config.input = build_subgraph_input(in.state);
    if (input_map_.empty()) {
        config.provider_messages = in.state.captured_provider_messages();
    } else {
        // Match the same winning source as build_subgraph_input, including
        // resetting custody if a later mapping overwrites it with generic data.
        for (const auto& [parent_channel, child_channel] : input_map_)
            if (child_channel == "messages")
                config.provider_messages = in.state.captured_provider_messages(parent_channel);
    }
    config.on_provider_event = in.ctx.on_provider_event;
    config.provider_outcomes = in.ctx.provider_outcomes;
    config.native_history_archive = in.ctx.native_history_archive;
    config.stream_mode = in.ctx.stream_mode;
    config.cancel_token = in.ctx.cancel_token;
    config.model_token_budget = in.ctx.model_token_budget;
    config.budget_exhausted = in.ctx.budget_exhausted;
    config.usage = in.ctx.usage;

    // Refuse overlapping writes to one persistent child namespace on this
    // compiled engine instead of racing load_latest() against save().
    struct NamespaceLease {
        std::mutex& mutex;
        std::set<std::string>& active;
        std::string key;
        NamespaceLease(std::mutex& m, std::set<std::string>& a, const std::string& id)
            : mutex(m), active(a), key(id) {
            std::lock_guard<std::mutex> guard(mutex);
            if (!active.insert(key).second)
                throw std::runtime_error("Persistent subgraph namespace already running");
        }
        ~NamespaceLease() {
            std::lock_guard<std::mutex> guard(mutex);
            active.erase(key);
        }
    };
    std::optional<NamespaceLease> lease;
    if (persistence_ == SubgraphPersistence::PerThread)
        lease.emplace(active_mutex_, active_per_thread_, config.thread_id);

    // Forward the parent's stream sink without buffering child events.
    auto result = co_await subgraph_->run_subgraph_async(
        std::move(config), in.ctx,
        in.stream_cb ? *in.stream_cb : GraphStreamCallback{}, persistence_);
    if (result.result.interrupted) {
        if (persistence_ == SubgraphPersistence::Stateless)
            throw std::runtime_error("Stateless subgraph does not support interrupt/resume");
        const auto reason = result.result.interrupt_value.value(
            "reason", "subgraph interrupted");
        if (result.result.interrupt_value.contains("value"))
            throw NodeInterrupt(reason, result.result.interrupt_value["value"]);
        throw NodeInterrupt(reason);
    }
    NodeOutput out;
    out.writes = map_output_writes(result.writes);
    co_return out;
}

} // namespace neograph::graph
