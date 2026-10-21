/**
 * @file program/store.h
 * @brief Atomic storage of admitted Program bundles and versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/store_call.h>
#include <neograph/program/activation.h>
#include <neograph/program/bundle.h>
#include <neograph/program/version.h>

#include <cstddef>
#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {
/** Coherent owner-scoped activation pointer and its admitted immutable target. */
struct NEOGRAPH_PROGRAM_API ProgramActivationBinding {
    ProgramActivation activation;
    ProgramVersion    version;
};

/**
 * Atomic storage of admitted Program bundles and versions, as the catalog uses it (#556). Every operation is awaitable;
 * a store whose backend is synchronous completes it at once (SyncProgramStore). The arguments of a call, string views
 * included, must stay alive until the awaitable completes: await it in the full expression that makes the call.
 */
class NEOGRAPH_PROGRAM_API ProgramStore {
public:
    virtual ~ProgramStore() = default;

    /**
     * Atomically publish one admitted bundle/version tuple.
     *
     * Implementations must provide the strong exception guarantee: if this call throws,
     * neither object may become observable through get_bundle() or get_version().
     */
    [[nodiscard]] virtual asio::awaitable<void> publish_admitted(
        const ProgramBundle& bundle,
        const ProgramVersion& version) = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramBundle>> get_bundle(std::string_view id) const = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramVersion>> get_version(std::string_view id) const = 0;

    /**
     * Owner-qualified lookups. A wrong owner is indistinguishable from an
     * absent object. The legacy unqualified overloads remain available for
     * local migration/introspection callers, but Catalog and Runtime must use
     * these overloads for authority-bearing resolution.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramBundle>> get_bundle(
        std::string_view /*owner_scope*/,
        std::string_view /*id*/) const {
        co_return std::nullopt;
    }
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramVersion>> get_version(
        std::string_view owner_scope,
        std::string_view id) const {
        if (owner_scope.empty()) co_return std::nullopt;
        const auto value = co_await get_version(id);
        if (!value || value->ownership_scope() != owner_scope) co_return std::nullopt;
        co_return value;
    }

    /**
     * Owner-scoped activation lifecycle.  The default keeps the bounded P2
     * adapter intentionally administration-inaccessible. Implementations must
     * verify that the target version and policy hash belong to owner_scope and
     * must return Conflict without publishing when the generation is stale.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramActivation>> get_activation(
        std::string_view /*owner_scope*/) const {
        return detail::failed_store_call<std::optional<ProgramActivation>>(
            std::make_exception_ptr(std::logic_error("ProgramStore does not expose activation lifecycle")));
    }
    /**
     * Return one coherent activation/version tuple. Implementations must read
     * both records under their backend lock or transaction; a missing or
     * corrupt target fails closed rather than returning a partial tuple.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramActivationBinding>> get_active_binding(
        std::string_view owner_scope) const {
        const auto active = co_await get_activation(owner_scope);
        if (!active) co_return std::nullopt;
        const auto version = co_await get_version(owner_scope, active->active_version_id());
        if (!version || version->policy_snapshot().fingerprint() != active->policy_snapshot_hash())
            throw std::runtime_error("Program activation target is missing or has a mismatched policy");
        co_return ProgramActivationBinding{*active, *version};
    }
    [[nodiscard]] virtual asio::awaitable<ProgramActivationResult> compare_activate(
        std::string_view /*owner_scope*/,
        std::uint64_t /*expected_generation*/,
        std::string_view /*version_id*/,
        std::string_view /*policy_snapshot_hash*/) {
        return detail::failed_store_call<ProgramActivationResult>(
            std::make_exception_ptr(std::logic_error("ProgramStore does not expose activation lifecycle")));
    }
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramVersion>> list_versions(
        std::string_view /*owner_scope*/) const {
        return detail::failed_store_call<std::vector<ProgramVersion>>(
            std::make_exception_ptr(std::logic_error("ProgramStore does not enumerate versions")));
    }
    [[nodiscard]] virtual asio::awaitable<ProgramRetentionReport> collect_garbage(
        std::string_view /*owner_scope*/,
        const std::vector<std::string>& /*pinned_version_ids*/) {
        return detail::failed_store_call<ProgramRetentionReport>(
            std::make_exception_ptr(std::logic_error("ProgramStore does not expose retention lifecycle")));
    }
};

/**
 * Base of a ProgramStore whose backend is synchronous: the in-memory store, SQLite and PostgreSQL. A derived class
 * implements the `..._sync` operations and gets the awaitable interface from them, as SyncProgramTransitionStore
 * does.
 */
class NEOGRAPH_PROGRAM_API SyncProgramStore : public ProgramStore {
public:
    // The synchronous operations. Each has the contract of the awaitable operation of the same name.
    virtual void publish_admitted_sync(const ProgramBundle& bundle, const ProgramVersion& version) = 0;
    virtual std::optional<ProgramBundle> get_bundle_sync(std::string_view id) const = 0;
    virtual std::optional<ProgramVersion> get_version_sync(std::string_view id) const = 0;
    virtual std::optional<ProgramBundle> get_bundle_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramVersion> get_version_sync(
        std::string_view owner_scope,
        std::string_view id) const {
        if (owner_scope.empty()) return std::nullopt;
        const auto value = get_version_sync(id);
        if (!value || value->ownership_scope() != owner_scope) return std::nullopt;
        return value;
    }
    virtual std::optional<ProgramActivation> get_activation_sync(
        std::string_view /*owner_scope*/) const {
        throw std::logic_error("ProgramStore does not expose activation lifecycle");
    }
    virtual std::optional<ProgramActivationBinding> get_active_binding_sync(
        std::string_view owner_scope) const {
        const auto active = get_activation_sync(owner_scope);
        if (!active) return std::nullopt;
        const auto version = get_version_sync(owner_scope, active->active_version_id());
        if (!version || version->policy_snapshot().fingerprint() != active->policy_snapshot_hash())
            throw std::runtime_error("Program activation target is missing or has a mismatched policy");
        return ProgramActivationBinding{*active, *version};
    }
    virtual ProgramActivationResult compare_activate_sync(
        std::string_view /*owner_scope*/,
        std::uint64_t /*expected_generation*/,
        std::string_view /*version_id*/,
        std::string_view /*policy_snapshot_hash*/) {
        throw std::logic_error("ProgramStore does not expose activation lifecycle");
    }
    virtual std::vector<ProgramVersion> list_versions_sync(
        std::string_view /*owner_scope*/) const {
        throw std::logic_error("ProgramStore does not enumerate versions");
    }
    virtual ProgramRetentionReport collect_garbage_sync(
        std::string_view /*owner_scope*/,
        const std::vector<std::string>& /*pinned_version_ids*/) {
        throw std::logic_error("ProgramStore does not expose retention lifecycle");
    }

    // The awaitable interface, completed by the operations above: at once on the calling thread,
    // or on a thread of a bounded pool when offloads() is true.
    [[nodiscard]] asio::awaitable<void> publish_admitted(
        const ProgramBundle& bundle,
        const ProgramVersion& version) final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramBundle>> get_bundle(std::string_view id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramVersion>> get_version(std::string_view id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramBundle>> get_bundle(
        std::string_view owner_scope,
        std::string_view id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramVersion>> get_version(
        std::string_view owner_scope,
        std::string_view id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramActivation>> get_activation(
        std::string_view owner_scope) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramActivationBinding>> get_active_binding(
        std::string_view owner_scope) const final;
    [[nodiscard]] asio::awaitable<ProgramActivationResult> compare_activate(
        std::string_view owner_scope,
        std::uint64_t expected_generation,
        std::string_view version_id,
        std::string_view policy_snapshot_hash) final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramVersion>> list_versions(
        std::string_view owner_scope) const final;
    [[nodiscard]] asio::awaitable<ProgramRetentionReport> collect_garbage(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids) final;

protected:
    /** True for a backend that blocks for long: each operation then runs on a pool thread. */
    virtual bool offloads() const noexcept { return false; }
};

/**
 * A ProgramStore for a caller that cannot suspend: the catalog and the synchronous Program API. It waits for a store
 * that is not synchronous as BlockingProgramTransitionStore does.
 */
class NEOGRAPH_PROGRAM_API BlockingProgramStore {
public:
    BlockingProgramStore(std::shared_ptr<ProgramStore> store, BlockingStorePolicy policy = {});
    /** A view of a store that outlives it. */
    BlockingProgramStore(ProgramStore& store, BlockingStorePolicy policy = {});

    const std::shared_ptr<ProgramStore>& store() const noexcept { return store_; }

    void publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) const;
    std::optional<ProgramBundle> get_bundle(std::string_view id) const;
    std::optional<ProgramVersion> get_version(std::string_view id) const;
    std::optional<ProgramBundle> get_bundle(std::string_view owner_scope, std::string_view id) const;
    std::optional<ProgramVersion> get_version(std::string_view owner_scope, std::string_view id) const;
    std::optional<ProgramActivation> get_activation(std::string_view owner_scope) const;
    std::optional<ProgramActivationBinding> get_active_binding(std::string_view owner_scope) const;
    ProgramActivationResult compare_activate(
        std::string_view owner_scope,
        std::uint64_t expected_generation,
        std::string_view version_id,
        std::string_view policy_snapshot_hash) const;
    std::vector<ProgramVersion> list_versions(std::string_view owner_scope) const;
    ProgramRetentionReport collect_garbage(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids) const;

private:
    std::shared_ptr<ProgramStore> store_;
    SyncProgramStore* sync_ = nullptr;
    BlockingStorePolicy policy_;
};

class NEOGRAPH_PROGRAM_API InMemoryProgramStore final : public SyncProgramStore {
public:
    InMemoryProgramStore();
    InMemoryProgramStore(InMemoryProgramStore&&) noexcept;
    InMemoryProgramStore& operator=(InMemoryProgramStore&&) noexcept;
    InMemoryProgramStore(const InMemoryProgramStore&)            = delete;
    InMemoryProgramStore& operator=(const InMemoryProgramStore&) = delete;
    ~InMemoryProgramStore() override;

    void publish_admitted_sync(const ProgramBundle& bundle, const ProgramVersion& version) override;
    std::optional<ProgramBundle>  get_bundle_sync(std::string_view id) const override;
    std::optional<ProgramVersion> get_version_sync(std::string_view id) const override;
    std::optional<ProgramBundle>  get_bundle_sync(std::string_view owner_scope,
                                             std::string_view id) const override;
    std::optional<ProgramVersion> get_version_sync(std::string_view owner_scope,
                                              std::string_view id) const override;
    std::optional<ProgramActivation>
    get_activation_sync(std::string_view owner_scope) const override;
    std::optional<ProgramActivationBinding>
    get_active_binding_sync(std::string_view owner_scope) const override;
    ProgramActivationResult compare_activate_sync(std::string_view owner_scope,
                                             std::uint64_t    expected_generation,
                                             std::string_view version_id,
                                             std::string_view policy_snapshot_hash) override;
    std::vector<ProgramVersion> list_versions_sync(std::string_view owner_scope) const override;
    ProgramRetentionReport collect_garbage_sync(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
