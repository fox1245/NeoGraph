/** @file hook_rpc.h @brief Optional transport-neutral JSON-RPC hook execution. */
#pragma once

#include <neograph/graph/cancel.h>
#include <neograph/hook_outbox.h>
#include <neograph/runtime_context.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace neograph {

/** A raw JSON-RPC exchange. The transport must observe both bounds. */
struct RpcRequest {
    std::string request;
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<graph::CancelToken> cancel_token;
};

/**
 * Injectable wire boundary for hook RPC. Returning bytes, rather than a parsed
 * JSON value, lets the common executor enforce response-size and nesting limits.
 */
class NEOGRAPH_API RpcTransport {
public:
    virtual ~RpcTransport() = default;
    virtual asio::awaitable<std::string> request_async(RpcRequest request) = 0;
};

class NEOGRAPH_API RpcProtocolError final : public std::runtime_error {
public:
    explicit RpcProtocolError(const std::string& message) : std::runtime_error(message) {}
};

struct HookRpcExecution {
    HookExecutionReceipt receipt;
    std::vector<ContextArtifact> artifacts;
};

/** Strict JSON-RPC 2.0 client for the fixed `hooks/invoke` method. */
class NEOGRAPH_API HookRpcExecutor final {
public:
    struct Limits {
        std::size_t max_response_bytes = 64U * 1024U;
        unsigned max_json_nesting = 16;
    };

    explicit HookRpcExecutor(std::shared_ptr<RpcTransport> transport);
    HookRpcExecutor(std::shared_ptr<RpcTransport> transport, Limits limits);

    asio::awaitable<HookRpcExecution> execute_async(
        const HookInvocation& invocation, std::uint32_t attempt,
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token = {}) const;

private:
    std::shared_ptr<RpcTransport> transport_;
    Limits limits_;
};

/** Binds the strict JSON-RPC executor to MandatoryHookRunner's backend contract. */
class NEOGRAPH_API RpcHookExecutionAdapter final {
public:
    explicit RpcHookExecutionAdapter(std::shared_ptr<HookRpcExecutor> executor);

    asio::awaitable<HookExecutionAttempt> execute_async(
        const HookInvocation& invocation,
        const RuntimeEvent& event,
        std::uint32_t attempt,
        ToolExecutionContext context) const;

private:
    std::shared_ptr<HookRpcExecutor> executor_;
};

} // namespace neograph
