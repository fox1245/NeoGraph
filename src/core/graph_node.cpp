#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <neograph/async/run_sync.h>
#include <algorithm>
#include <stdexcept>

namespace neograph::graph {

// --- GraphNode sync ↔ async crossover defaults (Sem 3.4) ---
// Same shape as Provider::complete / complete_async. Override one,
// the other comes free.
//
// Recursion-guard rationale: the two defaults below call each other.
// Override ONE of {execute, execute_async, execute_full,
// execute_full_async} and the cycle terminates at the user's override.
// Override NONE of them and the cycle is unbroken — without the guard,
// you'd hit a stack overflow inside asio's awaitable_thread machinery
// roughly 90,000 frames deep, with no clue where the bug is. The
// guard turns that mystery crash into a clear runtime_error pointing
// straight at the missing override (see feedback_async_bridge_required).
//
// Keying on the GraphNode pointer (not on a global depth counter) means
// the guard catches "this node's defaults call each other in a loop"
// without false-firing on legitimate nesting like a subgraph node whose
// inner engine dispatches a different node through the same defaults.
namespace {
thread_local const GraphNode* current_default_node = nullptr;

struct ExecuteDefaultGuard {
    const GraphNode* prev_;

    explicit ExecuteDefaultGuard(const GraphNode* node) : prev_(current_default_node) {
        if (current_default_node == node) {
            // v0.3.2: throw the dedicated subclass so default
            // execute_full_stream can catch *only* this case and fall
            // through to execute_stream — streaming-only nodes work
            // under run_stream without losing real user-thrown errors.
            // GraphNodeMissingOverride inherits from std::runtime_error,
            // so existing catch(std::runtime_error&) sites unchanged.
            throw GraphNodeMissingOverride(
                "GraphNode '" + node->get_name() + "': must override at least one of "
                "execute(), execute_async(), execute_full(), or "
                "execute_full_async() — or, for streaming-only nodes, "
                "override execute_stream() / execute_full_stream() and "
                "run via run_stream() / run_stream_async(). The default "
                "implementations call each other and would recurse "
                "infinitely. The most common shape for Send/Command-"
                "emitting nodes is to override the sync `execute_full(state)`; "
                "for async-native nodes, override `execute_async(state)`.");
        }
        current_default_node = node;
    }
    ~ExecuteDefaultGuard() { current_default_node = prev_; }
};
} // anonymous namespace

std::vector<ChannelWrite> GraphNode::execute(const GraphState& state) {
    ExecuteDefaultGuard guard(this);
    return neograph::async::run_sync(execute_async(state));
}

asio::awaitable<std::vector<ChannelWrite>>
GraphNode::execute_async(const GraphState& state) {
    ExecuteDefaultGuard guard(this);
    co_return execute(state);
}

// --- GraphNode default streaming: just delegates ---
std::vector<ChannelWrite> GraphNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& /*cb*/) {
    return execute(state);
}

asio::awaitable<std::vector<ChannelWrite>>
GraphNode::execute_stream_async(const GraphState& state,
                                const GraphStreamCallback& cb) {
    // Default keeps the existing async-native priority — chain
    // through execute_async so a node overriding only
    // ``execute_async`` (async-native, no execute_stream / cb need)
    // stays on the coroutine path with no run_sync detour.
    //
    // v0.3.2 fallback: if the async chain default-throws because
    // the user only overrode the sync ``execute_stream``, route
    // through execute_stream(state, cb) instead. The sync hop here
    // is acceptable — that user is already on the sync path by
    // virtue of overriding execute_stream. Without this fallback
    // run_stream() / run_stream_async() would be useless for
    // execute_stream-only subclasses (TODO_v0.3.md item #10).
    //
    // GCC-13 coroutine codegen rejects ``catch (const T&)`` directly
    // around a co_await; capture into exception_ptr and rethrow in
    // a plain non-coroutine try/catch outside the await.
    std::vector<ChannelWrite> result;
    std::exception_ptr eptr;
    try {
        result = co_await execute_async(state);
    } catch (...) {
        eptr = std::current_exception();
    }
    if (!eptr) {
        co_return result;
    }
    bool missing_override = false;
    try {
        std::rethrow_exception(eptr);
    } catch (const GraphNodeMissingOverride&) {
        missing_override = true;
    } catch (...) {
        std::rethrow_exception(eptr);
    }
    if (missing_override) {
        co_return execute_stream(state, cb);
    }
    co_return result;  // unreachable
}

// --- GraphNode default execute_full: wraps execute() ---
NodeResult GraphNode::execute_full(const GraphState& state) {
    // v0.3.2: install cancel-token thread-local for the duration of
    // the sync execute() call. Mirrors execute_full_async's scope so
    // either entry point lights up provider.complete()'s cancel
    // pickup. Engine sync run() goes through execute() → here →
    // user execute() → provider.complete; without the scope the
    // sync path saw a null current_cancel_token() and the multi-Send
    // worker's HTTP request couldn't be aborted.
    CurrentCancelTokenScope scope(state.run_cancel_token());
    return NodeResult{execute(state)};
}

asio::awaitable<NodeResult>
GraphNode::execute_full_async(const GraphState& state) {
    // Default route: call sync `execute_full(state)` directly. This is
    // the right thing to do for both common cases:
    //
    //   1. User overrode `execute_full` (sync) to return Command/Send.
    //      We pick that up and the directives reach the engine. (The
    //      previous default routed through `execute_async` → `execute`,
    //      which strips down to `vector<ChannelWrite>` only — silently
    //      dropping Command/Send. Worse, in v3.0 it stack-overflowed
    //      because execute and execute_async default into each other —
    //      see ExecuteDefaultGuard above.)
    //
    //   2. User overrode `execute_async` (async-native, no Send/Command).
    //      Default `execute_full` calls default `execute` which calls
    //      `run_sync(execute_async)` → user's override. Single
    //      sync→async hop, same as v3.0's behaviour.
    //
    // Async-native Send/Command emitters should override THIS method
    // directly with their own awaitable body — the default never has
    // to fabricate one.
    //
    // v0.3.2: install the run's cancel token as the thread-local
    // ``current_cancel_token()`` for the duration of the synchronous
    // ``execute_full(state)`` call. This is what lets a sync C++
    // node's ``provider.complete(params)`` pick up cancel propagation
    // — Provider::complete reads the same thread-local and binds it
    // to its inner run_sync's cancellation hooks. PyGraphNode does
    // this in its own override; native C++ subclasses route through
    // here. Without this, multi-Send fan-out workers each call
    // run_sync with a null token and only the engine's super-step
    // boundary polling notices the cancel — too late: the
    // parallel_group has already let every HTTP call run to
    // completion, leaking ~6-7 s of OpenAI billing per branch.
    CurrentCancelTokenScope scope(state.run_cancel_token());
    co_return execute_full(state);
}

// --- GraphNode default execute_full_stream ---
//
// Priority chain (preserves the v0.3.1 "Command/Send beats writes-only"
// semantic, plus closes the v0.3.2 streaming-only gap):
//
//   1. Call execute_full(state) — picks up any Command/Send the user
//      emits via an execute_full override.
//   2. If execute_full default-throws GraphNodeMissingOverride (user
//      didn't override any of execute / execute_async / execute_full /
//      execute_full_async), fall through to execute_stream — that's
//      the streaming-only-node case. Pre-v0.3.2 the throw escaped and
//      run_stream was useless for execute_stream-only subclasses.
//   3. Otherwise, if execute_full returned writes-only (no Command/Send),
//      replace its writes with execute_stream output so LLM_TOKEN events
//      reach cb.
//
// Real user-thrown exceptions in execute / execute_full propagate
// untouched — only the dedicated default-recursion exception triggers
// the fall-through, so we don't silently swallow errors.
NodeResult GraphNode::execute_full_stream(
    const GraphState& state, const GraphStreamCallback& cb) {
    // v0.3.2: install cancel-token scope. Note execute_full() below
    // also installs one — RAII nesting is fine, the inner scope just
    // restores the token on exit (same pointer either way).
    CurrentCancelTokenScope scope(state.run_cancel_token());
    NodeResult result;
    try {
        result = execute_full(state);
    } catch (const GraphNodeMissingOverride&) {
        // Streaming-only override path.
        return NodeResult{execute_stream(state, cb)};
    }
    if (!result.command && result.sends.empty()) {
        result.writes = execute_stream(state, cb);
    }
    return result;
}

asio::awaitable<NodeResult>
GraphNode::execute_full_stream_async(const GraphState& state,
                                     const GraphStreamCallback& cb) {
    // Async-native peer of the sync default above. Same priority
    // chain, same GraphNodeMissingOverride catch — kept on the asio
    // coroutine path (no run_sync detour) so executor overlap with
    // sibling coroutines is preserved.
    //
    // v0.3.2: install the run's cancel token as the thread-local
    // ``current_cancel_token()`` — same rationale as the non-stream
    // peer above. Streaming-specific note: also covers the
    // ``execute_stream``-only fallback path below — provider.complete
    // inside that sync hop sees the cancel token too.
    CurrentCancelTokenScope scope(state.run_cancel_token());

    // GCC-13 workaround: ``catch (const GraphNodeMissingOverride&)``
    // sitting directly around a ``co_await`` does not match the
    // exception (the same family of codegen bugs the rest of this
    // file works around — search "GCC 13" / "GCC-13"). Capture into
    // an exception_ptr and dispatch with a plain non-coroutine
    // try/catch outside the co_await — that matches reliably.
    NodeResult result;
    std::exception_ptr eptr;
    try {
        result = co_await execute_full_async(state);
    } catch (...) {
        eptr = std::current_exception();
    }
    if (eptr) {
        bool missing_override = false;
        try {
            std::rethrow_exception(eptr);
        } catch (const GraphNodeMissingOverride&) {
            missing_override = true;
        } catch (...) {
            // Legitimate user-thrown error — propagate untouched.
            std::rethrow_exception(eptr);
        }
        if (missing_override) {
            auto writes = co_await execute_stream_async(state, cb);
            co_return NodeResult{std::move(writes)};
        }
    }
    if (!result.command && result.sends.empty()) {
        result.writes = co_await execute_stream_async(state, cb);
    }
    co_return result;
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

asio::awaitable<std::vector<ChannelWrite>>
LLMCallNode::execute_async(const GraphState& state) {
    auto params = build_params(state);
    auto completion = co_await provider_->complete_async(params);

    json msg_json;
    to_json(msg_json, completion.message);

    co_return std::vector<ChannelWrite>{
        ChannelWrite{"messages", json::array({msg_json})}};
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

asio::awaitable<std::vector<ChannelWrite>>
LLMCallNode::execute_stream_async(const GraphState& state,
                                   const GraphStreamCallback& cb) {
    auto params = build_params(state);

    // Bridge Provider::StreamCallback -> GraphStreamCallback. Same
    // shape as the sync execute_stream but uses the async-native
    // provider entry so the engine's coroutine never blocks on a
    // synchronous call inside a worker. Subclasses with real async
    // streaming transport (WebSocket Responses, SSE) deliver tokens
    // directly onto the awaiter's executor.
    auto on_token = [&cb, this](const std::string& token) {
        if (cb) {
            cb(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_, json(token)});
        }
    };

    auto completion = co_await provider_->complete_stream_async(
        params, on_token);

    json msg_json;
    to_json(msg_json, completion.message);

    co_return std::vector<ChannelWrite>{
        ChannelWrite{"messages", json::array({msg_json})}};
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

asio::awaitable<std::vector<ChannelWrite>>
IntentClassifierNode::execute_async(const GraphState& state) {
    auto params = build_params(state);
    auto completion = co_await provider_->complete_async(params);
    co_return route_from(completion.message.content);
}

std::vector<ChannelWrite> IntentClassifierNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& cb) {

    auto params = build_params(state);

    auto on_token = [&cb, this](const std::string& token) {
        if (cb) {
            cb(GraphEvent{GraphEvent::Type::LLM_TOKEN, name_, json(token)});
        }
    };
    auto completion = provider_->complete_stream(params, on_token);
    return route_from(completion.message.content);
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

asio::awaitable<std::vector<ChannelWrite>>
SubgraphNode::execute_async(const GraphState& state) {
    RunConfig config;
    config.input = build_subgraph_input(state);

    auto result = co_await subgraph_->run_async(config);
    co_return extract_output(result.output);
}

std::vector<ChannelWrite> SubgraphNode::execute_stream(
    const GraphState& state, const GraphStreamCallback& cb) {

    RunConfig config;
    config.input = build_subgraph_input(state);

    auto result = subgraph_->run_stream(config, cb);
    return extract_output(result.output);
}

} // namespace neograph::graph
