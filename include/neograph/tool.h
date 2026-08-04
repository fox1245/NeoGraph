/**
 * @file tool.h
 * @brief Abstract tool interface for callable functions.
 *
 * Defines the Tool base class. Implement this to create tools that
 * LLM agents can discover and invoke during the ReAct loop.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/tool_execution.h>
#include <neograph/types.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace neograph::graph {
class CancelToken;
}

namespace neograph {

/**
 * @brief Per-invocation context for a cooperative asynchronous tool.
 *
 * Existing `Tool::execute_async(arguments)` overrides remain valid. Tools that
 * need to stop in-flight work can also implement `ContextualAsyncTool` and
 * poll or propagate `cancel_token` to their transport. Dispatchers populate
 * controller and identity so the invocation remains inside the host's
 * resource-admission boundary rather than bypassing it.
 */
struct ToolExecutionContext {
    std::shared_ptr<graph::CancelToken> cancel_token;
    std::shared_ptr<ToolExecutionController> controller;
    ToolExecutionIdentity identity;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

/**
 * @brief Optional async Tool extension that receives per-invocation context.
 *
 * This intentionally lives outside `Tool` so adding cooperative cancellation
 * does not change the frozen `Tool` vtable. A tool that needs to observe a
 * cancellation request should inherit both `Tool` and `ContextualAsyncTool`.
 */
class NEOGRAPH_API ContextualAsyncTool {
  public:
    virtual ~ContextualAsyncTool() = default;

    virtual asio::awaitable<std::string> execute_async(
        const json& arguments, ToolExecutionContext context) = 0;
};

/**
 * @brief Abstract base class for tools that agents can call.
 *
 * A tool provides its definition (name, description, parameter schema)
 * so the LLM knows when and how to call it, and an execute() method
 * that performs the actual work.
 *
 * @see neograph::mcp::MCPTool for remote MCP server tools.
 */
class NEOGRAPH_API Tool {
  public:
    virtual ~Tool() = default;

    /**
     * @brief Get the tool definition metadata.
     *
     * Returns a ChatTool containing the tool's name, description, and
     * JSON Schema for its parameters. This is sent to the LLM so it
     * can decide when to call the tool.
     *
     * @return Tool definition including name, description, and parameter schema.
     */
    virtual ChatTool get_definition() const = 0;

    /**
     * @brief Execute the tool with the given arguments.
     * @param arguments JSON object containing the tool's input parameters.
     * @return Result string to be fed back to the LLM.
     */
    virtual std::string execute(const json& arguments) = 0;

    /**
     * @brief Get the tool name.
     * @return Tool name string (must match the name in get_definition()).
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief Canonical async entry point for tool execution.
     *
     * The default offloads the sync execute() implementation to NeoGraph's
     * bounded blocking pool, so a legacy synchronous tool no longer pins the
     * graph event loop or serializes unrelated tool calls. I/O-bound tools
     * (HTTP fetch, MCP RPC) override this with a real coroutine. Dispatchers
     * apply ToolExecutionController policy before calling either path.
     */
    virtual asio::awaitable<std::string> execute_async(const json& arguments);
};

/**
 * @brief Adapter base class for tools whose work is naturally
 *        coroutine-shaped (HTTP fetch, MCP RPC, async DB query).
 *
 * Stage 3 / Sem 4.2. The Tool interface is intentionally sync — the
 * Stage 3 plan freezes it so users don't have to migrate every tool
 * to a new signature. AsyncTool keeps that contract: subclasses
 * implement `execute_async` returning `asio::awaitable<std::string>`
 * and AsyncTool's sync `execute` drives it through
 * `neograph::async::run_sync`. Each invocation gets its own private
 * io_context so the adapter is safe to call from any thread,
 * including from inside an existing run loop.
 *
 * @code
 * class FetchTool : public neograph::AsyncTool {
 * public:
 *     ChatTool get_definition() const override { ... }
 *     std::string get_name() const override { return "fetch"; }
 *
 *     asio::awaitable<std::string>
 *     execute_async(const json& args) override {
 *         auto ex = co_await asio::this_coro::executor;
 *         auto res = co_await neograph::async::async_post(ex, ...);
 *         co_return res.body;
 *     }
 * };
 * @endcode
 *
 * Implementation is in async/run_sync.h via the run_sync template,
 * so this header only needs the asio::awaitable forward declaration.
 */
class NEOGRAPH_API AsyncTool : public Tool {
  public:
    /// Async work — override this. Kept pure so AsyncTool subclasses
    /// must supply a real coroutine: relying on Tool's default (which
    /// bridges to execute()) would recurse here, since AsyncTool's
    /// execute() drives execute_async() in turn.
    asio::awaitable<std::string> execute_async(const json& arguments) override = 0;

    /// Sync facade — drives execute_async on a private io_context.
    /// Implemented out-of-line in src/core/tool.cpp so the run_sync
    /// helper isn't pulled into every translation unit that includes
    /// `<neograph/tool.h>`.
    std::string execute(const json& arguments) final;
};

} // namespace neograph
