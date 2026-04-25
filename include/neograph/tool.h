/**
 * @file tool.h
 * @brief Abstract tool interface for callable functions.
 *
 * Defines the Tool base class. Implement this to create tools that
 * LLM agents can discover and invoke during the ReAct loop.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/types.h>

#include <asio/awaitable.hpp>

#include <string>

namespace neograph {

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
    /// Async work — override this. Default would infinitely recurse
    /// against execute(), so an override is mandatory; left non-pure
    /// only because Tool's contract requires execute() to be present
    /// and providing both pure-virtual would force every Tool subclass
    /// to acknowledge AsyncTool, which we don't want.
    virtual asio::awaitable<std::string> execute_async(const json& arguments) = 0;

    /// Sync facade — drives execute_async on a private io_context.
    /// Implemented out-of-line in src/core/tool.cpp so the run_sync
    /// helper isn't pulled into every translation unit that includes
    /// `<neograph/tool.h>`.
    std::string execute(const json& arguments) final;
};

} // namespace neograph
