#pragma once

#include <neograph/api.h>
#include <neograph/types.h>
#include <runtime/client.h>
#include <core/request_controls.h>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <atomic>
#include <functional>
#include <exception>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>

namespace neograph::graph {
class CancelToken;
class CheckpointStore;
class OwnedManagedBudgetLease;
class ManagedBudgetEffectReceipt;
}

namespace neograph {

struct ProviderDispatchBudget {
    std::shared_ptr<UsageAccumulator> usage;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic<bool>> budget_exhausted;
    std::shared_ptr<graph::CancelToken> budget_cancel_token;
    std::shared_ptr<graph::OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<graph::CheckpointStore> managed_budget_store;
    std::string managed_effect_id;
};

enum class ProviderMode : std::uint8_t { Collect, Stream };

struct ProviderControls {
    std::optional<std::uint64_t> max_output_tokens;
    std::optional<std::uint64_t> max_tool_calls;
    std::optional<double> temperature, top_p;
    std::optional<std::string> reasoning_effort, reasoning_summary;
    std::optional<std::uint64_t> thinking_budget;
    std::optional<bool> include_thoughts;
    std::optional<std::string> thinking_level;
    std::optional<bool> thinking_summaries;
    std::optional<std::string> service_tier, required_tool;
    std::optional<sp::OpenRouterRouting> provider;
    std::optional<sp::ResponseFormat> response_format;
    std::optional<bool> store;
    std::string account_scope, system;
};

/// Host-only delivery caps; explicit values may tighten admitted SDK resources.
struct ProviderObserverLimits {
    std::optional<std::size_t> max_events;
    std::optional<std::size_t> max_bytes;
};

// Portable request controls remain the SDK's declared, family-specific fields.
// No raw JSON field override or callback-selected streaming mode is admitted.
struct ProviderRequest {
    sp::runtime::Request payload;
    ProviderMode mode = ProviderMode::Collect;
    sp::runtime::RunOptions options;
    std::shared_ptr<graph::CancelToken> cancel_token;
    std::function<void(const sp::Event&)> on_event;
    ProviderObserverLimits observer_limits;
};

/// Post-dispatch host failure retaining the exact drained provider outcome.
/// The owned outcome is evidence, not success or permission to redispatch.
class NEOGRAPH_API ProviderOutcomeError : public std::runtime_error {
public:
    ProviderOutcomeError(const char* message, sp::runtime::Result outcome,
                         std::exception_ptr cause)
        : std::runtime_error(message), outcome_(std::move(outcome)), cause_(std::move(cause)) {}
    const sp::runtime::Result& outcome() const noexcept { return outcome_; }
    const std::exception_ptr& cause() const noexcept { return cause_; }
private:
    sp::runtime::Result outcome_;
    std::exception_ptr cause_;
};

class NEOGRAPH_API ProviderBudgetSettlementError final : public ProviderOutcomeError {
public:
    ProviderBudgetSettlementError(sp::runtime::Result outcome, std::exception_ptr cause,
                                  std::exception_ptr dispatch_error = {})
        : ProviderOutcomeError("Provider budget settlement could not be persisted",
                               std::move(outcome), std::move(cause)),
          dispatch_error_(std::move(dispatch_error)) {}
    const std::exception_ptr& dispatch_error() const noexcept { return dispatch_error_; }
private:
    std::exception_ptr dispatch_error_;
};

class NEOGRAPH_API ProviderObserverError final : public ProviderOutcomeError {
public:
    ProviderObserverError(sp::runtime::Result result, std::exception_ptr cause,
                          sp::ErrorKind kind = sp::ErrorKind::Misuse)
        : ProviderOutcomeError(kind == sp::ErrorKind::ResourceLimit
              ? "Provider event retention resource limit exhausted" : "Provider event observer failed",
              std::move(result), std::move(cause)), kind_(kind) {}
    sp::ErrorKind error_kind() const noexcept { return kind_; }
private:
    sp::ErrorKind kind_;
};

class Provider;
class NEOGRAPH_API PreparedProviderRequest final {
public:
    PreparedProviderRequest();
    ~PreparedProviderRequest();
    PreparedProviderRequest(PreparedProviderRequest&&) noexcept;
    PreparedProviderRequest& operator=(PreparedProviderRequest&&) noexcept;
    PreparedProviderRequest(const PreparedProviderRequest&) = delete;
    PreparedProviderRequest& operator=(const PreparedProviderRequest&) = delete;
    bool valid() const noexcept;
    const sp::Error* error() const noexcept;
    std::string_view family() const noexcept;
    std::string_view model() const noexcept;
    std::string_view encoded_body() const noexcept;
    sp::runtime::SteadyTime deadline() const noexcept;
    ProviderMode mode() const noexcept;
    bool is_cancelled() const noexcept;
    std::optional<std::uint64_t> max_output_tokens() const noexcept;
    bool requires_native_custody() const noexcept;
    const sp::descriptor::ValidatedDescriptor* admitted_descriptor() const noexcept;
private:
    struct Impl;
    explicit PreparedProviderRequest(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend class Provider;
};

class NEOGRAPH_API ProviderBudgetClaim final {
public:
    ProviderBudgetClaim();
    ~ProviderBudgetClaim();
    ProviderBudgetClaim(ProviderBudgetClaim&&) noexcept;
    ProviderBudgetClaim& operator=(ProviderBudgetClaim&&) noexcept;
    ProviderBudgetClaim(const ProviderBudgetClaim&) = delete;
    ProviderBudgetClaim& operator=(const ProviderBudgetClaim&) = delete;
    bool active() const noexcept;
    std::uint64_t amount() const noexcept;
    asio::awaitable<void> begin_managed_effect(
        const PreparedProviderRequest& request, std::string effect_id = {});
    asio::awaitable<void> settle_managed(
        sp::runtime::Result result, std::exception_ptr dispatch_error = {});
    void mark_dispatched();
    std::uint64_t settle(sp::runtime::Result result);
private:
    ProviderBudgetClaim(ProviderDispatchBudget budget, std::uint64_t amount);
    void release_undispatched() noexcept;
    std::uint64_t settle_accounting(sp::runtime::Result result);
    ProviderDispatchBudget budget_;
    std::uint64_t amount_ = 0;
    std::unique_ptr<graph::ManagedBudgetEffectReceipt> managed_effect_;
    bool dispatched_ = false, settled_ = false, writeahead_committed_ = false;
    friend ProviderBudgetClaim reserve_provider_dispatch(
        const PreparedProviderRequest&, ProviderDispatchBudget);
};

// Capability identity plus a single prepared, owned operation contract.
// Returned awaitables own all operation state, including the runtime client;
// neither request nor Provider object must survive coroutine scheduling.
class NEOGRAPH_API Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(const PreparedProviderRequest& request);
protected:
    using LocalDispatch = std::function<asio::awaitable<sp::runtime::Result>(
        const PreparedProviderRequest&, const std::function<void(const sp::Event&)>&)>;
    static PreparedProviderRequest prepare_local(
        std::shared_ptr<sp::runtime::Client> client, ProviderRequest request, LocalDispatch dispatch);
    static PreparedProviderRequest prepare_runtime(
        std::shared_ptr<sp::runtime::Client> client, ProviderRequest request);
    static PreparedProviderRequest reject_preparation(sp::Error error);
    static PreparedProviderRequest observe_prepared(
        PreparedProviderRequest request,
        std::function<void(const PreparedProviderRequest&)> before,
        std::function<void(sp::runtime::Result)> after,
        std::function<void(const sp::Event&)> event = {},
        std::function<void(std::exception_ptr)> on_error = {}) noexcept;
private:
    static asio::awaitable<sp::runtime::Result> dispatch_impl(PreparedProviderRequest request);
    static asio::awaitable<sp::runtime::Result> dispatch_operation(PreparedProviderRequest request);
};

NEOGRAPH_API ProviderRequest make_provider_request(
    const Provider& provider, std::string model, std::vector<sp::Message> messages,
    std::vector<ChatTool> tools = {}, ProviderControls controls = {},
    ProviderMode mode = ProviderMode::Collect);
NEOGRAPH_API void set_provider_request_messages(ProviderRequest& request, std::vector<sp::Message> messages);
NEOGRAPH_API void clear_provider_request_messages(ProviderRequest& request);
NEOGRAPH_API const std::vector<sp::Message>& provider_request_messages(const ProviderRequest& request);
NEOGRAPH_API ProviderBudgetClaim reserve_provider_dispatch(
    const PreparedProviderRequest& request, ProviderDispatchBudget budget);

} // namespace neograph
