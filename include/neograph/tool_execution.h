/**
 * @file tool_execution.h
 * @brief Resource-aware, coroutine-safe execution boundary for tools.
 *
 * A Tool is an untrusted unit of work from the scheduler's perspective: an
 * implementation may suspend, block, mutate an external system, or hold a
 * process-wide resource.  This contract keeps admission separate from Tool's
 * historical synchronous interface.  Every dispatcher enters through a
 * ToolExecutionController; the controller applies a named policy, obtains a
 * lease from a fair FIFO resource arbiter, and only then invokes the tool.
 *
 * The default policy is deliberately conservative: calls with the same tool
 * name are keyed-exclusive, while different tools may overlap.  Hosts that
 * know a tool is re-entrant or that a concrete resource supports N sessions
 * must opt in through ToolExecutionPolicyRegistry.  A policy never grants
 * more concurrency implicitly.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph {
class Tool;
struct ToolExecutionContext;

namespace graph {
class CancelToken;
}

/** How an admitted invocation enters a Tool implementation. */
enum class ToolExecutionImplementation : std::uint8_t {
    /// Call Tool::execute_async. The base implementation offloads sync work.
    NativeAsync,
    /// Invoke Tool::execute on the bounded blocking executor explicitly.
    BlockingThread,
};

/** Scheduler concurrency semantics for one policy-derived resource key. */
enum class ToolConcurrency : std::uint8_t {
    /// No arbiter lease. Only safe for a host-declared re-entrant tool.
    Reentrant,
    /// One active invocation for each derived resource key.
    KeyedExclusive,
    /// Up to capacity active invocations for each derived resource key.
    Capacity,
};

/** Stable request identity used for resource accounting and audit correlation. */
struct ToolExecutionIdentity {
    /// Stable caller or tenant scope. Empty is normalized to "anonymous".
    std::string owner_scope;
    /// Durable root Program run identity, when the call originated in Program.
    std::string root_run_id;
    /// Current graph thread identity, when available.
    std::string thread_id;
    /// Host-generated request/correlation identifier, when available.
    std::string request_id;
};

/**
 * Host-owned policy for a named tool.
 *
 * resource_key_template is a literal with only these substitutions:
 * `{tool}`, `{owner}`, `{root}`, `{thread}`, and `{arg:name}`.  Argument
 * substitutions accept scalar JSON values only; missing or structured values
 * fail closed instead of silently collapsing unrelated resources together.
 */
struct ToolExecutionPolicy {
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    std::uint32_t schema_version = SCHEMA_VERSION;
    std::string   policy_id;
    ToolExecutionImplementation implementation = ToolExecutionImplementation::NativeAsync;
    ToolConcurrency concurrency = ToolConcurrency::KeyedExclusive;
    std::uint32_t capacity = 1;
    std::uint32_t max_pending = 128;
    std::chrono::milliseconds queue_timeout{60'000};
    std::size_t output_limit_bytes = 16U * 1024U * 1024U;
    std::string resource_key_template{"tool:{tool}"};

    /// Throws std::invalid_argument for a malformed or unsafe policy.
    void validate() const;
};

/** One admission request to the fair resource arbiter. */
struct ResourceRequest {
    std::string resource_key;
    std::string owner_scope;
    std::uint32_t capacity = 1;
    std::uint32_t max_pending = 128;
    std::chrono::milliseconds queue_timeout{60'000};
};

namespace detail {
class ResourceArbiterImpl;

/// Shared bounded executor used only by synchronous Tool fallbacks.
asio::awaitable<std::string> execute_blocking_tool_async(Tool& tool, json arguments);
} // namespace detail

/** RAII proof that the caller owns one ResourceArbiter capacity slot. */
class NEOGRAPH_API ResourceLease final {
public:
    ResourceLease() = default;
    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;
    ResourceLease(ResourceLease&& other) noexcept;
    ResourceLease& operator=(ResourceLease&& other) noexcept;
    ~ResourceLease() noexcept;

    [[nodiscard]] bool held() const noexcept;
    [[nodiscard]] std::string_view resource_key() const noexcept;
    void release() noexcept;

private:
    friend class ResourceArbiter;
    friend class detail::ResourceArbiterImpl;
    ResourceLease(std::shared_ptr<detail::ResourceArbiterImpl> impl,
                  std::string resource_key) noexcept;

    std::shared_ptr<detail::ResourceArbiterImpl> impl_;
    std::string resource_key_;
};

/**
 * Thread-safe FIFO resource arbiter.
 *
 * A waiter is removed when its cancellation token is observed or when its
 * queue deadline expires. Granted leases retain the implementation, so
 * releasing a lease remains safe during host teardown.
 */
class NEOGRAPH_API ResourceArbiter final {
public:
    ResourceArbiter();
    ~ResourceArbiter();
    ResourceArbiter(const ResourceArbiter&) = delete;
    ResourceArbiter& operator=(const ResourceArbiter&) = delete;

    asio::awaitable<ResourceLease> acquire_async(
        ResourceRequest request,
        std::shared_ptr<graph::CancelToken> cancel_token = {});

private:
    std::shared_ptr<detail::ResourceArbiterImpl> impl_;
};

/** Thread-safe named policy registry owned by an application host. */
class NEOGRAPH_API ToolExecutionPolicyRegistry final {
public:
    ToolExecutionPolicyRegistry();
    ~ToolExecutionPolicyRegistry();
    ToolExecutionPolicyRegistry(const ToolExecutionPolicyRegistry&) = delete;
    ToolExecutionPolicyRegistry& operator=(const ToolExecutionPolicyRegistry&) = delete;

    /// Replaces the policy for one exact tool name after validating it.
    void upsert(std::string tool_name, ToolExecutionPolicy policy);
    void erase(std::string_view tool_name);
    void set_default(ToolExecutionPolicy policy);
    [[nodiscard]] ToolExecutionPolicy resolve(std::string_view tool_name) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/** Dependencies injected into a ToolExecutionController. */
struct ToolExecutionControllerConfig {
    std::shared_ptr<ToolExecutionPolicyRegistry> policies;
    std::shared_ptr<ResourceArbiter> arbiter;
};

/**
 * The single tool execution boundary shared by Agent and graph dispatch.
 *
 * A controller is intentionally host-shareable: injecting the same instance
 * into independently constructed engines makes their resource keys compete in
 * one queue rather than accidentally granting each engine a private limit.
 */
class NEOGRAPH_API ToolExecutionController final {
public:
    explicit ToolExecutionController(ToolExecutionControllerConfig config = {});
    ~ToolExecutionController();
    ToolExecutionController(const ToolExecutionController&) = delete;
    ToolExecutionController& operator=(const ToolExecutionController&) = delete;

    asio::awaitable<std::string> execute_async(Tool& tool,
                                               json arguments,
                                               ToolExecutionContext context);

    [[nodiscard]] std::shared_ptr<ToolExecutionPolicyRegistry> policies() const;
    [[nodiscard]] std::shared_ptr<ResourceArbiter> arbiter() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/** Process-default conservative controller for legacy callers. */
NEOGRAPH_API std::shared_ptr<ToolExecutionController> default_tool_execution_controller();

} // namespace neograph
