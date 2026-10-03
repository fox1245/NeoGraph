/**
 * @file graph/provider_call_broker.h
 * @brief Host-owned, invocation-scoped boundary for built-in Core provider calls.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <string_view>

namespace neograph::graph {


/** Stable Core call identity. The host adds its Program/job scope. */
struct ProviderCallIdentity {
    std::string owner_scope;
    std::string run_id;
    std::string thread_id;
    std::string task_id;
    std::string node_name;
    /// Stable per-task ordinal; inner loops assign each provider turn explicitly.
    std::uint64_t call_ordinal = 0;
    std::shared_ptr<UsageAccumulator> usage;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    std::shared_ptr<CancelToken> budget_cancel_token;
    std::shared_ptr<OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<CheckpointStore> managed_budget_store;
};

/// Stable original-bank task slot; preparation binding is stored separately.
NEOGRAPH_API std::string managed_provider_effect_id(const ProviderCallIdentity& identity);

/**
 * The trusted host implements durable admission, replay, and reconciliation.
 * `params` are the exact request passed to Provider::invoke by the built-in
 * node. A broker may return a verified stored completion without calling the
 * provider. An exception denies the node call. Core does not claim that a
 * broker implementation is durable or that the remote effect is exactly once.
 */
class NEOGRAPH_API ProviderCallBroker {
public:
    virtual ~ProviderCallBroker() = default;

    virtual asio::awaitable<sp::runtime::Result> invoke(
        ProviderCallIdentity identity,
        std::shared_ptr<Provider> provider,
        ProviderRequest request) = 0;
};

}  // namespace neograph::graph
