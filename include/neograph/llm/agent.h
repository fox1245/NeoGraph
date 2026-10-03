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
#include <neograph/tool_dispatch.h>   // ToolGate (issue #89)
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neograph { class RuntimeInterpositionController; }
namespace neograph { class HookRuntime; }
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
 * std::vector<sp::Message> messages = {{.role = sp::Role::User, .parts = {sp::Text{"What's 2+2?"}}}};
 * auto outcome = agent.run(messages);
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
     * @return The full immutable final provider outcome.
     */
    sp::runtime::Result run(std::vector<sp::Message>& messages,
                    int max_iterations = 10);
    /// Per-run host Tool broker context; caller supplies stable owner/run/thread
    /// and grant identity. An interrupted batch is replayed from messages on
    /// reconnect before another provider turn is requested.
    sp::runtime::Result run(std::vector<sp::Message>& messages, int max_iterations,
                    ToolExecutionContext effect_context);

    /**
     * @brief Run the agent loop with the explicit typed streaming mode.
     * Every provider turn streams typed events exactly once; no response is
     * discarded and re-requested merely to produce display text.
     * @param[in,out] messages Full history, extended with every returned message.
     * @param on_event Callback receiving borrowed SDK event views.
     * @param max_iterations Maximum LLM call iterations.
     * @return The full immutable final provider outcome.
     */
    sp::runtime::Result run_stream(std::vector<sp::Message>& messages,
                           const std::function<void(const sp::Event&)>& on_event,
                           int max_iterations = 10);
    sp::runtime::Result run_stream(std::vector<sp::Message>& messages,
                           const std::function<void(const sp::Event&)>& on_event, int max_iterations,
                           ToolExecutionContext effect_context);

    /**
     * @brief Perform a single LLM completion (no tool loop).
     * @param messages Conversation history (not modified).
     * @return The full completion response.
     */
    sp::runtime::Result complete(const std::vector<sp::Message>& messages);

    /**
     * @brief Token usage accumulated across every call this agent has made (#88).
     *
     * The graph path reports usage through ``RunResult::usage``. Agent is not a
     * graph run and has no ``RunContext``, so it keeps its own running total —
     * otherwise token accounting would exist on one of the two ways to drive an
     * LLM and not the other, which is precisely the split that #87 was about.
     *
     * Cumulative over the agent's lifetime, not per ``run()``: an agent loop
     * makes several LLM calls per run and the interesting number is what the
     * whole conversation cost.
     */
    sp::Usage usage() const { return usage_->snapshot(); }
    const std::vector<sp::runtime::Result>& outcomes() const noexcept { return outcomes_; }

    /**
     * @brief Intercept every tool call before it runs (issue #89).
     *
     * The same gate the graph path takes from ``EngineConfig::tool_gate`` or
     * ``GraphEngine::set_tool_gate``. Both
     * route through one dispatcher (issue #87), which is what stops a
     * capability like this from landing in one path and silently missing the
     * other — as concurrency once did.
     *
     * An ``Interrupt`` verdict throws ``graph::NodeInterrupt`` out of
     * ``run()``: the Agent is standalone and has no checkpoint machinery to
     * pause into. Callers wanting pause-and-resume want the graph path.
     */
    void set_tool_gate(ToolGate gate) { tool_gate_ = std::move(gate); }

    /// Inject a host-shared resource admission boundary for tool execution.
    /// Empty restores the conservative process-default controller.
    void set_tool_execution_controller(
        std::shared_ptr<ToolExecutionController> controller) {
        tool_execution_controller_ = std::move(controller);
    }

    /// Override the initial streamed tool-detection turn's absolute deadline.
    /// Positive values are seconds; -1 keeps the provider's default deadline.
    void set_tool_detection_timeout_seconds(int timeout_seconds) {
        tool_detection_timeout_seconds_ = timeout_seconds;
    }

    /// Opt into assembled, receipt-journaled provider dispatch for this agent.
    void set_runtime_interposition(std::shared_ptr<::neograph::RuntimeInterpositionController> controller);

    /// Install the host-owned lifecycle boundary used by standalone tool dispatch.
    void set_hook_runtime(std::shared_ptr<::neograph::HookRuntime> runtime) {
        hook_runtime_ = std::move(runtime);
    }

  private:
    std::shared_ptr<Provider> provider_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::string instructions_;
    std::string model_;

    /// #88 — Agent's own token accounting; see usage().
    std::shared_ptr<UsageAccumulator> usage_ = std::make_shared<UsageAccumulator>();
    std::vector<sp::runtime::Result> outcomes_;

    /// #89 — tool interception; empty means every call runs.
    ToolGate tool_gate_;

    /// Optional host-shared resource admission controller. The dispatcher uses
    /// the process default when this is empty.
    std::shared_ptr<ToolExecutionController> tool_execution_controller_;

    int tool_detection_timeout_seconds_ = -1;
    std::shared_ptr<::neograph::RuntimeInterpositionController> runtime_interposition_;
    std::shared_ptr<::neograph::HookRuntime> hook_runtime_;

    void ensure_system_message(std::vector<sp::Message>& messages);
    sp::runtime::Result run_loop(std::vector<sp::Message>& messages, int max_iterations,
                         ToolExecutionContext execution, ProviderMode mode,
                         std::function<void(const sp::Event&)> on_event);
    std::vector<ChatTool> get_tool_definitions() const;

    /// Non-owning view of `tools_` for `dispatch_tool_calls`, the one place
    /// tool execution is implemented (shared with graph::ToolDispatchNode).
    std::vector<Tool*> tool_ptrs() const;
};

} // namespace neograph::llm
