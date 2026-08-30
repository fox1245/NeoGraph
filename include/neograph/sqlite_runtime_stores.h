#pragma once

#include <neograph/context_store.h>
#include <neograph/controlled_provider.h>
#include <neograph/hook_outbox.h>

#include <memory>
#include <string>

namespace neograph {

/** Optional SQLite implementations of the runtime durability contracts. */
class NEOGRAPH_API SQLiteContextStore final : public DurableContextStore {
public:
    explicit SQLiteContextStore(std::string database_path);
    ~SQLiteContextStore() override;
    SQLiteContextStore(SQLiteContextStore&&) noexcept;
    SQLiteContextStore& operator=(SQLiteContextStore&&) noexcept;
    SQLiteContextStore(const SQLiteContextStore&) = delete;
    SQLiteContextStore& operator=(const SQLiteContextStore&) = delete;
    ContextStoreAppendResult append_history(const ContextStoreFeed&, const RuntimeHistoryRecord&, const std::optional<std::string>&) override;
    ContextStoreHead history_head(const ContextStoreFeed&) const override;
    std::optional<RuntimeHistoryRecord> history_record_by_message_id(
        const ContextStoreFeed&, std::string_view) const override;
    ContextHistoryRange snapshot_history(const ContextStoreFeed&, std::uint64_t, std::uint64_t) const override;
    std::string hydrate_history(const ContextHistoryRange&) const override;
    ContextArtifactPutResult put_artifact(std::string_view, const ContextArtifact&) override;
    std::optional<ContextArtifact> get_artifact(std::string_view, std::string_view) const override;
private: struct Impl; std::unique_ptr<Impl> impl_;
};

class NEOGRAPH_API SQLiteHookJournal final : public HookJournal {
public:
    explicit SQLiteHookJournal(std::string database_path);
    ~SQLiteHookJournal() override;
    SQLiteHookJournal(SQLiteHookJournal&&) noexcept;
    SQLiteHookJournal& operator=(SQLiteHookJournal&&) noexcept;
    SQLiteHookJournal(const SQLiteHookJournal&) = delete;
    SQLiteHookJournal& operator=(const SQLiteHookJournal&) = delete;
    HookOutboxEntry enqueue(HookInvocation, RuntimeEvent, std::uint32_t, std::chrono::system_clock::time_point) override;
    HookOutboxEntry publish(std::string_view) override;
    std::optional<HookLease> claim(std::string_view, std::string_view, std::chrono::system_clock::time_point, std::chrono::milliseconds) override;
    std::optional<HookOutboxEntry> settle(std::string_view, std::uint64_t, HookExecutionReceipt, std::chrono::system_clock::time_point) override;
    std::optional<HookOutboxEntry> reconcile(std::string_view, HookExecutionState, std::string_view, std::uint64_t, HookExecutionReceipt) override;
    std::vector<HookOutboxEntry> pending() const override;
    std::vector<HookOutboxEntry> reconciliation_required() const override;
    std::optional<HookOutboxEntry> get(std::string_view) const override;
private: struct Impl; std::unique_ptr<Impl> impl_;
};

class NEOGRAPH_API SQLiteProviderDispatchReceiptStore final : public DurableProviderDispatchReceiptStore {
public:
    explicit SQLiteProviderDispatchReceiptStore(std::string database_path);
    ~SQLiteProviderDispatchReceiptStore() override;
    SQLiteProviderDispatchReceiptStore(SQLiteProviderDispatchReceiptStore&&) noexcept;
    SQLiteProviderDispatchReceiptStore& operator=(SQLiteProviderDispatchReceiptStore&&) noexcept;
    SQLiteProviderDispatchReceiptStore(const SQLiteProviderDispatchReceiptStore&) = delete;
    SQLiteProviderDispatchReceiptStore& operator=(const SQLiteProviderDispatchReceiptStore&) = delete;
    ProviderDispatchReceiptPutResult persist(const ProviderDispatchReceipt&) override;
    ProviderDispatchReceiptPutResult persist(std::string_view owner_scope,
                                             const ProviderDispatchReceipt&) override;
    ProviderDispatchState state(std::string_view) const override;
    ProviderDispatchState state(std::string_view owner_scope,
                                std::string_view) const override;
    ProviderDispatchOutcomePutResult settle(
        std::string_view owner_scope,
        const ProviderDispatchOutcomeReceipt&) override;
    std::optional<ProviderDispatchOutcomeReceipt> outcome(
        std::string_view owner_scope,
        std::string_view dispatch_id) const override;
private: struct Impl; std::unique_ptr<Impl> impl_;
};

}  // namespace neograph
