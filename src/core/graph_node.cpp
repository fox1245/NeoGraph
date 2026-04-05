#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <algorithm>
#include <stdexcept>

namespace neograph::graph {

// --- GraphNode default streaming: just delegates ---
std::vector<ChannelWrite> GraphNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& /*cb*/) {
    return execute(state);
}

// --- GraphNode default execute_full: wraps execute() ---
NodeResult GraphNode::execute_full(const GraphState& state) {
    return NodeResult{execute(state)};
}

// --- GraphNode default execute_full_stream ---
// Calls execute_full() first to capture any Command/Send directives,
// then replaces the writes with execute_stream() output so that
// LLM_TOKEN events are properly emitted during graph execution.
// Nodes that need both streaming AND Command/Send should override this.
NodeResult GraphNode::execute_full_stream(
    const GraphState& state, const GraphStreamCallback& cb) {
    auto result = execute_full(state);
    // If the node didn't override execute_full (i.e., no Command/Send),
    // re-execute with streaming to emit tokens.
    if (!result.command && result.sends.empty()) {
        result.writes = execute_stream(state, cb);
    }
    return result;
}

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

std::vector<ChannelWrite> LLMCallNode::execute(const GraphState& state) {
    auto params     = build_params(state);
    auto completion = provider_->complete(params);

    // Serialize assistant message and append to messages channel
    json msg_json;
    to_json(msg_json, completion.message);

    return {ChannelWrite{"messages", json::array({msg_json})}};
}

std::vector<ChannelWrite> LLMCallNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& cb) {

    auto params = build_params(state);

    // Bridge Provider::StreamCallback -> GraphStreamCallback
    auto on_token = [&cb, this](const std::string& token) {
        if (cb) {
            cb(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_, json(token)});
        }
    };

    auto completion = provider_->complete_stream(params, on_token);

    json msg_json;
    to_json(msg_json, completion.message);

    return {ChannelWrite{"messages", json::array({msg_json})}};
}

// =========================================================================
// ToolDispatchNode
// =========================================================================

ToolDispatchNode::ToolDispatchNode(const std::string& name, const NodeContext& ctx)
    : name_(name)
    , tools_(ctx.tools)
{}

std::vector<ChannelWrite> ToolDispatchNode::execute(const GraphState& state) {
    auto messages = state.get_messages();
    if (messages.empty()) return {};

    // Find the last assistant message with tool_calls
    const ChatMessage* assistant_msg = nullptr;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) {
            assistant_msg = &(*it);
            break;
        }
    }
    if (!assistant_msg) return {};

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

    return {ChannelWrite{"messages", results}};
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

std::vector<ChannelWrite> IntentClassifierNode::execute(const GraphState& state) {
    auto messages = state.get_messages();

    // Find last user message
    std::string user_content;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            user_content = it->content;
            break;
        }
    }

    // Build classification prompt
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

    params.messages = {sys_msg, usr_msg};

    auto completion = provider_->complete(params);
    std::string intent = completion.message.content;

    // Match against valid routes (case-insensitive substring match)
    std::string best_route = valid_routes_.empty() ? intent : valid_routes_[0];
    for (const auto& r : valid_routes_) {
        if (intent.find(r) != std::string::npos) {
            best_route = r;
            break;
        }
    }

    return {ChannelWrite{"__route__", json(best_route)}};
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

std::vector<ChannelWrite> SubgraphNode::execute(const GraphState& state) {
    RunConfig config;
    config.input = build_subgraph_input(state);

    auto result = subgraph_->run(config);
    return extract_output(result.output);
}

std::vector<ChannelWrite> SubgraphNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& cb) {

    RunConfig config;
    config.input = build_subgraph_input(state);

    auto result = subgraph_->run_stream(config, cb);
    return extract_output(result.output);
}

} // namespace neograph::graph
