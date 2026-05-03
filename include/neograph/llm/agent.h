/**
 * @file llm/agent.h
 * @brief Simple ReAct agent loop for LLM + tool interaction.
 *
 * Provides a standalone agent that runs the ReAct loop:
 * LLM generates -> tool calls -> feed results -> repeat until done.
 * For graph-based agents, use GraphEngine with create_react_graph() instead.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace neograph::llm {

/**
 * @brief Standalone ReAct agent that loops between LLM calls and tool execution.
 *
 * The agent sends messages to the LLM, checks for tool calls, executes them,
 * feeds results back, and repeats until the LLM produces a final text response
 * or the iteration limit is reached.
 *
 * @code
 * auto agent = Agent(provider, std::move(tools), "You are a helpful assistant.");
 * std::vector<ChatMessage> messages = {{.role = "user", .content = "What's 2+2?"}};
 * std::string reply = agent.run(messages);
 * @endcode
 *
 * @see neograph::graph::create_react_graph for the graph-based equivalent.
 */
class NEOGRAPH_API Agent {
  public:
    /**
     * @brief Construct an agent with a provider and tools.
     * @param provider LLM provider for making completions.
     * @param tools Vector of tools available to the agent (ownership transferred).
     * @param instructions Optional system prompt prepended to the conversation.
     * @param model Optional model name override (empty = use provider default).
     */
    Agent(std::shared_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const std::string& instructions = "",
          const std::string& model = "");

    // Move-only — `tools_` is `vector<unique_ptr<Tool>>`. Explicit
    // declarations are required because Agent is NEOGRAPH_API and MSVC
    // eagerly instantiates all special members for dll-exported classes
    // (the implicit copy-assign tries to copy the vector → deleted →
    // C2280). GCC/Clang only instantiate on use, so the bug was Windows-
    // only and silently broke the wheel build.
    Agent(const Agent&)                     = delete;
    Agent& operator=(const Agent&)          = delete;
    Agent(Agent&&) noexcept                 = default;
    Agent& operator=(Agent&&) noexcept      = default;
    ~Agent()                                = default;

    /**
     * @brief Run the agent loop until completion.
     *
     * Iterates between LLM calls and tool execution until the LLM
     * produces a response with no tool calls, or max_iterations is reached.
     *
     * @param[in,out] messages Conversation history (modified in-place with new messages).
     * @param max_iterations Maximum number of LLM call iterations (default: 10).
     * @return The final text response from the LLM.
     */
    std::string run(std::vector<ChatMessage>& messages,
                    int max_iterations = 10);

    /**
     * @brief Run the agent loop with streaming token output.
     *
     * Same as run(), but streams the final response tokens via the callback.
     *
     * @param[in,out] messages Conversation history (modified in-place).
     * @param on_chunk Callback invoked per token during the final LLM response.
     * @param max_iterations Maximum LLM call iterations (default: 10).
     * @return The final text response.
     */
    std::string run_stream(std::vector<ChatMessage>& messages,
                           const StreamCallback& on_chunk,
                           int max_iterations = 10);

    /**
     * @brief Perform a single LLM completion (no tool loop).
     * @param messages Conversation history (not modified).
     * @return The full completion response.
     */
    ChatCompletion complete(const std::vector<ChatMessage>& messages);

  private:
    std::shared_ptr<Provider> provider_;
    std::vector<std::unique_ptr<Tool>> tools_;
    /// Name → tool* lookup built once at construction. Keeps tool
    /// dispatch O(1) per call rather than O(n) scan over `tools_` —
    /// matters when N is large (e.g. an MCP server with 30+ tools)
    /// and the run loop fires many tool_call iterations.
    std::unordered_map<std::string, Tool*> tools_by_name_;
    std::string instructions_;
    std::string model_;

    void ensure_system_message(std::vector<ChatMessage>& messages);
    std::vector<ChatTool> get_tool_definitions() const;
};

} // namespace neograph::llm
