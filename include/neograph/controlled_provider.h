#pragma once

#include <neograph/api.h>
#include <neograph/completion_provider.h>
#include <neograph/runtime_context.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph {

struct ProviderDispatchReceiptData {
    std::string dispatch_id;
    std::string provider_binding_identity;
    std::string assembly_receipt_id;
    std::string normalized_request_digest;
    std::string model;
    CompletionMode mode = CompletionMode::COLLECT;
};

/** Immutable proof that an assembled request was admitted to provider dispatch. */
class NEOGRAPH_API ProviderDispatchReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ProviderDispatchReceipt create(ProviderDispatchReceiptData data);
    static ProviderDispatchReceipt parse(std::string_view stored_bytes);

    const std::string& dispatch_id() const noexcept;
    const std::string& provider_binding_identity() const noexcept;
    const std::string& assembly_receipt_id() const noexcept;
    const std::string& normalized_request_digest() const noexcept;
    const std::string& model() const noexcept;
    CompletionMode mode() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProviderDispatchReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

enum class ProviderDispatchReceiptPutResult : std::uint8_t { Stored, AlreadyPresent, Conflict };

/** Durable state of a previously admitted provider dispatch. */
enum class ProviderDispatchState : std::uint8_t {
    Unavailable, Missing, AdmittedPending, Succeeded, Failed, ReconciliationRequired,
};

/** Durable write-ahead journal keyed by owner scope and dispatch_id; AlreadyPresent requires exact receipt bytes. */
class NEOGRAPH_API ProviderDispatchReceiptStore {
public:
    virtual ~ProviderDispatchReceiptStore() = default;
    virtual ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& receipt) = 0;
    /// Scoped operations prevent one owner's dispatch id from colliding with another's.
    /// Existing stores retain their legacy behavior until they opt into scope handling.
    virtual ProviderDispatchReceiptPutResult persist(std::string_view owner_scope,
                                                     const ProviderDispatchReceipt& receipt) {
        (void)owner_scope;
        return persist(receipt);
    }
    /// Returns durable progress for a dispatch id. Unavailable means this store
    /// cannot make a safe post-crash retry decision.
    virtual ProviderDispatchState state(std::string_view dispatch_id) const {
        (void)dispatch_id;
        return ProviderDispatchState::Unavailable;
    }
    virtual ProviderDispatchState state(std::string_view owner_scope,
                                        std::string_view dispatch_id) const {
        (void)owner_scope;
        return state(dispatch_id);
    }
};

/** Opt-in marker for receipt stores backed by durable storage. */
class NEOGRAPH_API DurableProviderDispatchReceiptStore : public ProviderDispatchReceiptStore {
public:
    ~DurableProviderDispatchReceiptStore() override = default;
};

/** Thread-safe in-memory receipt journal for standalone use and tests. */
class NEOGRAPH_API InMemoryProviderDispatchReceiptStore final : public ProviderDispatchReceiptStore {
public:
    InMemoryProviderDispatchReceiptStore();
    ~InMemoryProviderDispatchReceiptStore() override;
    InMemoryProviderDispatchReceiptStore(InMemoryProviderDispatchReceiptStore&&) noexcept;
    InMemoryProviderDispatchReceiptStore& operator=(InMemoryProviderDispatchReceiptStore&&) noexcept;
    InMemoryProviderDispatchReceiptStore(const InMemoryProviderDispatchReceiptStore&) = delete;
    InMemoryProviderDispatchReceiptStore& operator=(const InMemoryProviderDispatchReceiptStore&) = delete;
    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt& receipt) override;
    ProviderDispatchReceiptPutResult persist(std::string_view owner_scope,
                                             const ProviderDispatchReceipt& receipt) override;
    ProviderDispatchState state(std::string_view dispatch_id) const override;
    ProviderDispatchState state(std::string_view owner_scope,
                                std::string_view dispatch_id) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Standalone write-ahead dispatch boundary. It is not wired into graph, Agent,
 * or Harness call sites; callers explicitly supply an assembled turn.
 */
class NEOGRAPH_API ControlledProvider final {
public:
    ControlledProvider(std::shared_ptr<Provider> provider,
                       std::shared_ptr<ProviderDispatchReceiptStore> receipts,
                       std::string provider_binding_identity);
    ~ControlledProvider();
    ControlledProvider(ControlledProvider&&) noexcept;
    ControlledProvider& operator=(ControlledProvider&&) noexcept;
    ControlledProvider(const ControlledProvider&) = delete;
    ControlledProvider& operator=(const ControlledProvider&) = delete;

    ChatCompletion dispatch(std::string dispatch_id,
                            const ContextAssemblyReceipt& assembly,
                            CompletionRequest request);
    ChatCompletion dispatch(std::string owner_scope, std::string dispatch_id,
                            const ContextAssemblyReceipt& assembly,
                            CompletionRequest request);
    asio::awaitable<ChatCompletion> dispatch_async(std::string dispatch_id,
                                                    const ContextAssemblyReceipt& assembly,
                                                    CompletionRequest request);
    asio::awaitable<ChatCompletion> dispatch_async(std::string owner_scope, std::string dispatch_id,
                                                    const ContextAssemblyReceipt& assembly,
                                                    CompletionRequest request);

private:
    struct Impl;
    static asio::awaitable<ChatCompletion> dispatch_impl(
        std::shared_ptr<Impl> impl,
        std::string owner_scope,
        std::string dispatch_id,
        ContextAssemblyReceipt assembly,
        CompletionRequest request);
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph
