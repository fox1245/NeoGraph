#include <neograph/llm/agent.h>
#include <neograph/async/run_sync.h>
#include <neograph/tool_dispatch.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace neograph::llm {

std::vector<Tool*> Agent::tool_ptrs() const {
    std::vector<Tool*> ptrs;
    ptrs.reserve(tools_.size());
    for (const auto& t : tools_) ptrs.push_back(t.get());
    return ptrs;
}

Agent::Agent(std::shared_ptr<Provider> provider,
             std::vector<std::unique_ptr<Tool>> tools,
             const std::string& instructions,
             const std::string& model)
  : provider_(std::move(provider))
  , tools_(std::move(tools))
  , instructions_(instructions)
  , model_(model)
{
    // Build the name → tool* index once. Subsequent tool_call
    // dispatches are O(1) on this map instead of O(n) std::find_if
    // over `tools_`. Last-write-wins on duplicate names so the
    // behaviour matches the previous find_if (which returned the
    // first match).
    tools_by_name_.reserve(tools_.size());
    for (auto& t : tools_) {
        if (!t) continue;
        tools_by_name_[t->get_name()] = t.get();
    }
}

void
Agent::ensure_system_message(std::vector<ChatMessage>& messages)
{
    if (instructions_.empty()) return;

    bool has_system = !messages.empty() &&
                      messages[0].role == "system" &&
                      messages[0].content == instructions_;

    if (!has_system) {
        ChatMessage sys;
        sys.role = "system";
        sys.content = instructions_;
        messages.insert(messages.begin(), sys);
    }
}

std::vector<ChatTool>
Agent::get_tool_definitions() const
{
    std::vector<ChatTool> defs;
    defs.reserve(tools_.size());
    for (const auto& tool : tools_) {
        defs.push_back(tool->get_definition());
    }
    return defs;
}

ChatCompletion
Agent::complete(const std::vector<ChatMessage>& messages)
{
    CompletionParams params;
    params.model = model_;
    params.messages = messages;
    // Candidate 6 PR3: dispatch via invoke() so the v1.0 single-
    // dispatch surface is end-to-end. run_sync drives the awaitable
    // synchronously (Agent::complete is the public sync API).
    return neograph::async::run_sync(provider_->invoke(params, nullptr));
}

std::string
Agent::run(std::vector<ChatMessage>& messages, int max_iterations)
{
    ensure_system_message(messages);
    auto tool_defs = get_tool_definitions();

    for (int i = 0; i < max_iterations; ++i) {
        CompletionParams params;
        params.model = model_;
        params.messages = messages;
        params.tools = tool_defs;

        auto completion = neograph::async::run_sync(provider_->invoke(params, nullptr));
        auto& msg = completion.message;

        // Append assistant message to history
        messages.push_back(msg);

        // No tool calls -> final response
        if (msg.tool_calls.empty()) {
            return msg.content;
        }

        // Tool execution is not implemented here — it lives in exactly one
        // place, shared with graph::ToolDispatchNode (issue #87). This loop
        // used to call the blocking execute() one tool at a time while the
        // node fanned the same calls out concurrently; three 300 ms async
        // tools took 900 ms here and 300 ms there.
        auto tool_msgs = neograph::async::run_sync(
            dispatch_tool_calls(msg.tool_calls, tool_ptrs()));
        for (auto& tm : tool_msgs) {
            messages.push_back(std::move(tm));
        }
    }

    throw std::runtime_error("Agent exceeded max iterations (" +
                             std::to_string(max_iterations) + ")");
}

std::string
Agent::run_stream(std::vector<ChatMessage>& messages,
                  const StreamCallback& on_chunk,
                  int max_iterations)
{
    ensure_system_message(messages);
    auto tool_defs = get_tool_definitions();
    bool has_done_tool_calls = false;

    for (int i = 0; i < max_iterations; ++i) {
        CompletionParams params;
        params.model = model_;
        params.messages = messages;
        params.tools = tool_defs;

        // After tool execution, use streaming for the final response
        if (has_done_tool_calls) {
            auto completion = neograph::async::run_sync(
                provider_->invoke(params, on_chunk));
            messages.push_back(completion.message);

            if (completion.message.tool_calls.empty()) {
                return completion.message.content;
            }
            // Rare: another tool call after streaming — fall through to execute
        } else {
            // Non-streaming: reliable tool call detection
            auto completion = neograph::async::run_sync(
                provider_->invoke(params, nullptr));
            messages.push_back(completion.message);

            if (completion.message.tool_calls.empty()) {
                // No tools needed at all — stream the response
                // Remove the non-streamed message, re-do with streaming
                messages.pop_back();
                auto streamed = neograph::async::run_sync(
                    provider_->invoke(params, on_chunk));
                messages.push_back(streamed.message);
                return streamed.message.content;
            }
        }

        // Copy the calls out before touching `messages`. The previous code held
        // `auto& msg = messages.back()` and push_back'ed tool results into the
        // same vector while iterating msg.tool_calls — a reallocation there
        // leaves the reference dangling and the next iteration reads freed
        // memory. It only bites with two or more tool calls, which is why it
        // survived this long.
        auto calls = messages.back().tool_calls;

        auto tool_msgs = neograph::async::run_sync(
            dispatch_tool_calls(std::move(calls), tool_ptrs()));
        for (auto& tm : tool_msgs) {
            messages.push_back(std::move(tm));
        }

        has_done_tool_calls = true;
    }

    throw std::runtime_error("Agent exceeded max iterations (" +
                             std::to_string(max_iterations) + ")");
}

} // namespace neograph::llm
