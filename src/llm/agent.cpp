#include <neograph/llm/agent.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace neograph::llm {

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
    return provider_->complete(params);
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

        auto completion = provider_->complete(params);
        auto& msg = completion.message;

        // Append assistant message to history
        messages.push_back(msg);

        // No tool calls -> final response
        if (msg.tool_calls.empty()) {
            return msg.content;
        }

        // Execute each tool call (O(1) lookup via tools_by_name_).
        for (const auto& tc : msg.tool_calls) {
            auto idx_it = tools_by_name_.find(tc.name);

            ChatMessage tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.tool_name = tc.name;

            if (idx_it == tools_by_name_.end()) {
                tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
            } else {
                try {
                    auto args = json::parse(tc.arguments);
                    tool_msg.content = idx_it->second->execute(args);
                } catch (const std::exception& e) {
                    tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
                }
            }

            messages.push_back(tool_msg);
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
            auto completion = provider_->complete_stream(params, on_chunk);
            messages.push_back(completion.message);

            if (completion.message.tool_calls.empty()) {
                return completion.message.content;
            }
            // Rare: another tool call after streaming — fall through to execute
        } else {
            // Non-streaming: reliable tool call detection
            auto completion = provider_->complete(params);
            messages.push_back(completion.message);

            if (completion.message.tool_calls.empty()) {
                // No tools needed at all — stream the response
                // Remove the non-streamed message, re-do with streaming
                messages.pop_back();
                auto streamed = provider_->complete_stream(params, on_chunk);
                messages.push_back(streamed.message);
                return streamed.message.content;
            }
        }

        auto& msg = messages.back();

        // Execute tool calls
        for (const auto& tc : msg.tool_calls) {
            auto idx_it = tools_by_name_.find(tc.name);

            ChatMessage tool_msg;
            tool_msg.role = "tool";
            tool_msg.tool_call_id = tc.id;
            tool_msg.tool_name = tc.name;

            if (idx_it == tools_by_name_.end()) {
                tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
            } else {
                try {
                    auto args = json::parse(tc.arguments);
                    tool_msg.content = idx_it->second->execute(args);
                } catch (const std::exception& e) {
                    tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
                }
            }

            messages.push_back(tool_msg);
        }

        has_done_tool_calls = true;
    }

    throw std::runtime_error("Agent exceeded max iterations (" +
                             std::to_string(max_iterations) + ")");
}

} // namespace neograph::llm
