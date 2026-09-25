/**
 * @file tool_effect_broker.h
 * @brief Host-owned replay boundary for one mediated Tool effect.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <string>

namespace neograph {

/** Stable Core call slot. The host adds its Program, job, and grant scope. */
struct ToolEffectIdentity {
    std::string owner_scope;
    std::string run_id;
    std::string thread_id;
    std::string task_id;
    /// Index in the gated assistant Tool batch, independent of worker order.
    std::uint64_t call_ordinal = 0;
    /// Provider-supplied correlation value; never use as the sole replay key.
    std::string tool_call_id;
};

/**
 * The trusted host persists a dispatch marker before calling the controller,
 * replays exact completed receipts, and blocks unresolved effects. The broker
 * receives the selected Tool and rewritten arguments after the full batch has
 * passed ToolGate. It may call context.controller->execute_result_async() to
 * perform the effect, or return a verified stored result without executing.
 *
 * An ordinary thrown exception is conservatively exposed as an uncertain
 * effect; cancellation remains graph control flow. A confirmed pre-dispatch
 * refusal should return an explicit failed result.
 * Core does not claim that a particular host broker is durable or exactly once.
 */
class NEOGRAPH_API ToolEffectBroker {
public:
    virtual ~ToolEffectBroker() = default;

    virtual asio::awaitable<ToolExecutionResult> execute(
        ToolEffectIdentity identity, Tool& tool, json arguments,
        ToolExecutionContext context) = 0;
};

}  // namespace neograph
