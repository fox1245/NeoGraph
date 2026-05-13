#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>   // RunContext (forward-declared in node.h)
#include <neograph/async/run_sync.h>
#include <algorithm>
#include <stdexcept>

namespace neograph::graph {

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

// =========================================================================
// LLMCallNode
// =========================================================================

LLMCallNode::LLMCallNode(const std::string& name, const NodeContext& ctx)
    : name_(name)
    , provider_(ctx.provider)
    , tools_(ctx.tools)
    , model_(ctx.model)
    , instructions_(ctx.instructions)
{}

CompletionParams LLMCallNode::build_params(const GraphState& state) const {
    auto messages = state.get_messages();

    // Ensure system message (mirrors Agent::ensure_system_message)
    if (!instructions_.empty()) {
        bool has_system = !messages.empty()
                          && messages[0].role == "system"
                          && messages[0].content == instructions_;
        if (!has_system) {
            ChatMessage sys;
            sys.role    = "system";
            sys.content = instructions_;
            messages.insert(messages.begin(), sys);
        }
    }

    // Build tool definitions
    std::vector<ChatTool> tool_defs;
    tool_defs.reserve(tools_.size());
    for (auto* tool : tools_) {
        tool_defs.push_back(tool->get_definition());
    }

    CompletionParams params;
    params.model    = model_;
    params.messages = std::move(messages);
    params.tools    = std::move(tool_defs);
    return params;
}

asio::awaitable<NodeOutput> LLMCallNode::run(NodeInput in) {
    auto params = build_params(in.state);
    // v0.4 PR 9a: explicit cancel propagation — no thread-local
    // smuggling. The provider binds this token's slot to its inner
    // ConnPool::async_post co_await, so a caller's cancel() aborts
    // the in-flight HTTPS socket.
    params.cancel_token = in.ctx.cancel_token;

    // ROADMAP_v1.md Candidate 6 PR2: dispatch through Provider::invoke()
    // — the v1.0 unified entry point. Same semantic as the previous
    // `if (in.stream_cb) complete_stream_async else complete_async` pair,
    // but the stream/non-stream branch lives inside the provider's own
    // default invoke() body (or its native override), not at every call
    // site. Native providers that override invoke() get one dispatch
    // path; legacy 4-virtual subclasses get the chain via the additive
    // default. See PR #40.
    StreamCallback on_token;
    if (in.stream_cb) {
        const GraphStreamCallback& cb = *in.stream_cb;
        std::string node_name = name_;
        on_token = [&cb, node_name](const std::string& token) {
            cb(GraphEvent{GraphEvent::Type::LLM_TOKEN,
                          node_name, json(token)});
        };
    }
    auto completion = co_await provider_->invoke(params, on_token);

    json msg_json;
    to_json(msg_json, completion.message);

    NodeOutput out;
    out.writes.push_back(
        ChannelWrite{"messages", json::array({msg_json})});
    co_return out;
}

// =========================================================================
// ToolDispatchNode
// =========================================================================

ToolDispatchNode::ToolDispatchNode(const std::string& name, const NodeContext& ctx)
    : name_(name)
    , tools_(ctx.tools)
{}

asio::awaitable<NodeOutput> ToolDispatchNode::run(NodeInput in) {
    auto messages = in.state.get_messages();
    if (messages.empty()) co_return NodeOutput{};

    // Find the last assistant message with tool_calls
    const ChatMessage* assistant_msg = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) {
            assistant_msg = &(*it);
            break;
        }
    }
    if (!assistant_msg) co_return NodeOutput{};

    // Execute each tool call (mirrors agent.cpp:80-104)
    json results = json::array();

    for (const auto& tc : assistant_msg->tool_calls) {
        auto it = std::find_if(tools_.begin(), tools_.end(),
            [&](Tool* t) { return t->get_name() == tc.name; });

        ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;

        if (it == tools_.end()) {
            tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
        } else {
            try {
                auto args = json::parse(tc.arguments);
                tool_msg.content = (*it)->execute(args);
            } catch (const std::exception& e) {
                tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
            }
        }

        json msg_json;
        to_json(msg_json, tool_msg);
        results.push_back(msg_json);
    }

    NodeOutput out;
    out.writes.push_back(ChannelWrite{"messages", results});
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

CompletionParams IntentClassifierNode::build_params(const GraphState& state) const {
    auto messages = state.get_messages();

    // Find last user message
    std::string user_content;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            user_content = it->content;
            break;
        }
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

    CompletionParams params;
    params.model = model_;
    params.temperature = 0.0f;
    params.max_tokens = 20;

    ChatMessage sys_msg;
    sys_msg.role = "system";
    sys_msg.content = sys_prompt;

    ChatMessage usr_msg;
    usr_msg.role = "user";
    usr_msg.content = user_content;

    params.messages = {std::move(sys_msg), std::move(usr_msg)};
    return params;
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

    // Candidate 6 PR2: same invoke() unification as LLMCallNode above.
    StreamCallback on_token;
    if (in.stream_cb) {
        const GraphStreamCallback& cb = *in.stream_cb;
        std::string node_name = name_;
        on_token = [&cb, node_name](const std::string& token) {
            cb(GraphEvent{GraphEvent::Type::LLM_TOKEN, node_name, json(token)});
        };
    }
    auto completion = co_await provider_->invoke(params, on_token);
    ChatMessage reply = std::move(completion.message);

    NodeOutput out;
    out.writes = route_from(reply.content);
    co_return out;
}

// =========================================================================
// SubgraphNode
// =========================================================================

SubgraphNode::SubgraphNode(const std::string& name,
                           std::shared_ptr<GraphEngine> subgraph,
                           std::map<std::string, std::string> input_map,
                           std::map<std::string, std::string> output_map)
    : name_(name)
    , subgraph_(std::move(subgraph))
    , input_map_(std::move(input_map))
    , output_map_(std::move(output_map))
{}

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

std::vector<ChannelWrite> SubgraphNode::extract_output(
    const json& subgraph_output) const {

    std::vector<ChannelWrite> writes;

    if (!subgraph_output.contains("channels")) return writes;
    const auto& channels = subgraph_output["channels"];

    if (output_map_.empty()) {
        // Default: write all child channels back to parent (same name)
        for (const auto& [ch_name, ch_data] : channels.items()) {
            if (ch_data.contains("value")) {
                writes.push_back(ChannelWrite{ch_name, ch_data["value"]});
            }
        }
    } else {
        for (const auto& [child_ch, parent_ch] : output_map_) {
            if (channels.contains(child_ch) && channels[child_ch].contains("value")) {
                writes.push_back(ChannelWrite{parent_ch, channels[child_ch]["value"]});
            }
        }
    }

    return writes;
}

asio::awaitable<NodeOutput> SubgraphNode::run(NodeInput in) {
    RunConfig config;
    config.input        = build_subgraph_input(in.state);
    config.cancel_token = in.ctx.cancel_token;

    json subgraph_output;
    if (in.stream_cb) {
        // Forward the parent's stream sink so subgraph events (LLM
        // tokens, node enter/exit, etc.) surface at the parent
        // graph's caller without buffering.
        auto result = co_await subgraph_->run_stream_async(config, *in.stream_cb);
        subgraph_output = std::move(result.output);
    } else {
        auto result = co_await subgraph_->run_async(config);
        subgraph_output = std::move(result.output);
    }

    NodeOutput out;
    out.writes = extract_output(subgraph_output);
    co_return out;
}

} // namespace neograph::graph
