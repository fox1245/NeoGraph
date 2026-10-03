/**
 * @file tenant.h
 * @brief Trusted-ingress tenant identity, quotas, and scoped persistence.
 *
 * A TenantScope is an immutable value created by a trusted ingress adapter. It
 * deliberately contains no bearer credential, provider secret, or topology
 * data. Store wrappers use it to create a private backend namespace while
 * preserving the public session/thread/checkpoint identifiers for each tenant.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::tenant {

/** Immutable identity and authorization binding supplied by trusted ingress. */
class NEOGRAPH_API TenantScope final {
public:
    /** Construct only from already-authenticated, non-secret logical IDs. */
    TenantScope(std::string tenant_id, std::string authorization_scope);

    const std::string& tenant_id() const noexcept { return tenant_id_; }
    const std::string& authorization_scope() const noexcept { return authorization_scope_; }

    /** Exact-match check used by an ingress adapter before dispatch. */
    bool authorizes(std::string_view tenant_id, std::string_view authorization_scope) const noexcept;

private:
    std::string tenant_id_;
    std::string authorization_scope_;
};

/** Per-tenant admission and retention limits. Zero means unlimited. */
struct NEOGRAPH_API TenantQuotas {
    std::uint64_t max_concurrency = 0;
    std::uint64_t max_queue = 0;
    std::uint64_t max_model_tokens = 0;
    std::uint64_t max_cost_microunits = 0;
    std::uint64_t max_artifacts = 0;
    std::uint64_t max_retained_artifacts = 0;
};

class NEOGRAPH_API TenantQuotaExceeded final : public std::runtime_error {
public:
    explicit TenantQuotaExceeded(const char* resource);
};

/**
 * Thread-safe quota ledger. Reservations are fail-closed and leases release
 * concurrency/artifact capacity on every exit path, including exceptions.
 */
class NEOGRAPH_API TenantQuota final {
public:
    explicit TenantQuota(TenantQuotas limits = {});
    TenantQuota(const TenantQuota&) = delete;
    TenantQuota& operator=(const TenantQuota&) = delete;

    const TenantQuotas& limits() const noexcept { return limits_; }
    std::uint64_t active() const noexcept;
    std::uint64_t queued() const noexcept;
    std::uint64_t model_tokens() const noexcept;
    std::uint64_t cost_microunits() const noexcept;
    std::uint64_t artifacts() const noexcept;
    std::uint64_t retained_artifacts() const noexcept;

    /** Reserve one running slot. */
    bool try_acquire_concurrency() noexcept;
    void release_concurrency() noexcept;
    /** Reserve one queued request. */
    bool try_acquire_queue() noexcept;
    void release_queue() noexcept;
    /** Atomically reserve bounded token/cost budget. */
    bool try_reserve_usage(std::uint64_t model_tokens,
                           std::uint64_t cost_microunits) noexcept;
    /** Roll back a usage reservation (for failed admission). */
    void release_usage(std::uint64_t model_tokens,
                       std::uint64_t cost_microunits) noexcept;
    bool try_acquire_artifact() noexcept;
    void release_artifact() noexcept;
    bool try_acquire_retained_artifact() noexcept;
    void release_retained_artifact() noexcept;

private:
    bool try_increment(std::atomic<std::uint64_t>& value, std::uint64_t limit) noexcept;
    static void decrement(std::atomic<std::uint64_t>& value, std::uint64_t amount) noexcept;

    const TenantQuotas limits_;
    std::atomic<std::uint64_t> active_{0};
    std::atomic<std::uint64_t> queued_{0};
    std::atomic<std::uint64_t> model_tokens_{0};
    std::atomic<std::uint64_t> cost_microunits_{0};
    std::atomic<std::uint64_t> artifacts_{0};
    std::atomic<std::uint64_t> retained_artifacts_{0};
};

/** Move-only RAII reservation for a request and its optional artifact. */
class NEOGRAPH_API TenantQuotaLease final {
public:
    TenantQuotaLease() noexcept = default;
    static TenantQuotaLease acquire(TenantQuota& quota, bool artifact = false);
    TenantQuotaLease(TenantQuotaLease&& other) noexcept;
    TenantQuotaLease& operator=(TenantQuotaLease&& other) noexcept;
    TenantQuotaLease(const TenantQuotaLease&) = delete;
    TenantQuotaLease& operator=(const TenantQuotaLease&) = delete;
    ~TenantQuotaLease();
    void release() noexcept;
    explicit operator bool() const noexcept { return quota_ != nullptr; }

private:
    TenantQuotaLease(TenantQuota* quota, bool artifact) noexcept
        : quota_(quota), artifact_(artifact) {}
    TenantQuota* quota_ = nullptr;
    bool artifact_ = false;
};

/**
 * Store namespace owned by one trusted tenant. The backend can be shared by
 * tenants; all namespace operations are prefixed with an unambiguous length
 * encoded tenant namespace.
 */
class NEOGRAPH_API ScopedStore final : public graph::Store {
public:
    ScopedStore(TenantScope scope, std::shared_ptr<graph::Store> backend);
    const TenantScope& scope() const noexcept { return scope_; }

    void put(const graph::Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<graph::StoreItem> get(const graph::Namespace& ns,
                                         const std::string& key) const override;
    std::vector<graph::StoreItem> search(const graph::Namespace& ns_prefix,
                                          int limit = 100) const override;
    void delete_item(const graph::Namespace& ns, const std::string& key) override;
    std::vector<graph::Namespace>
    list_namespaces(const graph::Namespace& prefix = {}) const override;

private:
    graph::Namespace private_namespace(const graph::Namespace& ns) const;
    graph::StoreItem public_item(graph::StoreItem item) const;
    TenantScope scope_;
    std::shared_ptr<graph::Store> backend_;
};

/** Checkpoint namespace owned by one trusted tenant. */
class NEOGRAPH_API ScopedCheckpointStore final : public graph::CheckpointStore {
public:
    ScopedCheckpointStore(TenantScope scope, std::shared_ptr<graph::CheckpointStore> backend);
    const TenantScope& scope() const noexcept { return scope_; }

    void save(const graph::Checkpoint& cp) override;
    std::optional<graph::Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override;
    std::vector<graph::Checkpoint> list(const std::string& thread_id,
                                        int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;
    void put_writes(const std::string& thread_id, const std::string& parent_checkpoint_id,
                    const graph::PendingWrite& write) override;
    std::vector<graph::PendingWrite>
    get_writes(const std::string& thread_id, const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override;
    bool requires_managed_budget(const std::string& thread_id) override;
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override;
    std::shared_ptr<graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const graph::ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override;
    asio::awaitable<std::shared_ptr<graph::OwnedManagedBudgetLease>>
    acquire_managed_budget_lease_async(graph::ManagedBudgetLeaseScope scope,
        std::string expected_checkpoint_id, std::string expected_checkpoint_commitment) override;
    graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override;
    asio::awaitable<graph::ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest) override;
    void settle_managed_budget_effect(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
        const graph::ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority) override;
    asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease,
        graph::ManagedBudgetEffectReceipt effect, sp::runtime::Result genuine_outcome,
        UsageAccumulator::AuthoritySnapshot authority) override;
    void publish_managed_budget_checkpoint(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
        const graph::Checkpoint& checkpoint) override;
    asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease, graph::Checkpoint checkpoint) override;
    void release_managed_budget_lease(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease) override;
    asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease) override;
    bool retains_native_checkpoint() const noexcept override;
    void publish_managed_budget_fork(const graph::Checkpoint& authenticated_source,
        const graph::Checkpoint& genuine_shared_bank_fork) override;
    asio::awaitable<void> publish_managed_budget_fork_async(
        graph::Checkpoint source, graph::Checkpoint forked) override;

private:
    std::string private_id(std::string_view id) const;
    graph::Checkpoint private_checkpoint(const graph::Checkpoint& cp) const;
    graph::Checkpoint public_checkpoint(graph::Checkpoint cp) const;
    graph::ManagedBudgetLeaseScope private_scope(graph::ManagedBudgetLeaseScope scope) const;
    std::string private_source_commitment(
        const graph::Checkpoint& checkpoint, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) const;
    TenantScope scope_;
    std::shared_ptr<graph::CheckpointStore> backend_;
};

}  // namespace neograph::tenant

namespace neograph::graph {
using TenantScope = tenant::TenantScope;
using TenantQuotas = tenant::TenantQuotas;
using TenantQuota = tenant::TenantQuota;
using TenantQuotaLease = tenant::TenantQuotaLease;
using TenantQuotaExceeded = tenant::TenantQuotaExceeded;
using ScopedStore = tenant::ScopedStore;
using ScopedCheckpointStore = tenant::ScopedCheckpointStore;
}  // namespace neograph::graph
