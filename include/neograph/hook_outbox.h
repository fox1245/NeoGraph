/** @file hook_outbox.h @brief Durable hook dispatch contracts and in-memory journal. */
#pragma once

#include <neograph/context_store.h>
#include <neograph/hook.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph {

enum class HookExecutionState : std::uint8_t {
    Triggered, Pending, Dispatched, Succeeded, Failed, TimedOut,
    ReconciliationRequired, Cancelled,
};
NEOGRAPH_API std::string_view to_string(HookExecutionState value) noexcept;

/** A receipt from the external effect boundary. `outcome_known` is required for success. */
struct ExternalEffectReceipt {
    std::string receipt_id;
    bool outcome_known = false;
    bool succeeded = false;
    std::string detail;
};

struct HookExecutionReceiptData {
    std::string invocation_id;
    std::uint32_t attempt = 0;
    HookExecutionState state = HookExecutionState::Failed;
    ExternalEffectReceipt external_effect;
    std::string error;
};

/** Immutable durable terminal-attempt record. */
class NEOGRAPH_API HookExecutionReceipt final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    static HookExecutionReceipt create(HookExecutionReceiptData data);
    static HookExecutionReceipt parse(std::string_view stored_bytes);
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const HookExecutionReceiptData& data() const noexcept { return data_; }
    [[nodiscard]] std::string serialize_canonical() const;
private:
    explicit HookExecutionReceipt(HookExecutionReceiptData data) : data_(std::move(data)) {}
    HookExecutionReceiptData data_;
    std::string id_;
};

struct HookOutboxEntryData {
    HookInvocation invocation;
    RuntimeEvent event;
    HookExecutionState state = HookExecutionState::Triggered;
    std::uint32_t attempt_count = 0;
    std::uint32_t max_attempts = 1;
    std::chrono::system_clock::time_point deadline{};
    std::uint64_t fencing_token = 0;
    std::chrono::system_clock::time_point lease_expires_at{};
    std::optional<HookExecutionReceipt> receipt;
};

/** Immutable snapshot of one logical invocation's durable outbox state. */
class NEOGRAPH_API HookOutboxEntry final {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;
    static HookOutboxEntry create(HookOutboxEntryData data);
    static HookOutboxEntry parse(std::string_view stored_bytes);
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const HookOutboxEntryData& data() const noexcept { return data_; }
    [[nodiscard]] std::string serialize_canonical() const;
private:
    explicit HookOutboxEntry(HookOutboxEntryData data) : data_(std::move(data)) {}
    HookOutboxEntryData data_;
    std::string id_;
};

struct HookLease { HookOutboxEntry entry; std::uint64_t fencing_token = 0; };

/** Typed failure of a protected runtime boundary. */
class NEOGRAPH_API HookBoundaryBlocked final : public std::runtime_error {
public:
    HookBoundaryBlocked(std::string invocation_id, HookExecutionState state, std::string message);
    [[nodiscard]] const std::string& invocation_id() const noexcept { return invocation_id_; }
    [[nodiscard]] HookExecutionState state() const noexcept { return state_; }
private:
    std::string invocation_id_;
    HookExecutionState state_;
};

/** Atomic durable-outbox abstraction. Implementations must fence stale workers.
 * Fencing is passed to contextual targets through ToolExecutionContext identity,
 * but this contract does not claim physical exactly-once unless that target
 * enforces the token and returns a known external-effect receipt. */
class NEOGRAPH_API HookJournal {
public:
    virtual ~HookJournal() = default;
    virtual HookOutboxEntry enqueue(HookInvocation invocation, RuntimeEvent event,
                                    std::uint32_t max_attempts,
                                    std::chrono::system_clock::time_point deadline) = 0;
    virtual HookOutboxEntry publish(std::string_view invocation_id) = 0;
    virtual std::optional<HookLease> claim(std::string_view invocation_id, std::string_view worker_id,
                                           std::chrono::system_clock::time_point now,
                                           std::chrono::milliseconds lease_duration) = 0;
    virtual std::optional<HookOutboxEntry> settle(std::string_view invocation_id,
                                                  std::uint64_t fencing_token,
                                                  HookExecutionReceipt receipt,
                                                  std::chrono::system_clock::time_point now) = 0;
    virtual std::optional<HookOutboxEntry> reconcile(std::string_view invocation_id,
                                                      HookExecutionState expected_state,
                                                      std::string_view expected_head_id,
                                                      std::uint64_t expected_fencing_token,
                                                      HookExecutionReceipt known_outcome) = 0;
    virtual std::vector<HookOutboxEntry> pending() const = 0;
    virtual std::vector<HookOutboxEntry> reconciliation_required() const = 0;
    virtual std::optional<HookOutboxEntry> get(std::string_view invocation_id) const = 0;
};

class NEOGRAPH_API InMemoryHookJournal final : public HookJournal {
public:
    InMemoryHookJournal();
    ~InMemoryHookJournal() override;
    HookOutboxEntry enqueue(HookInvocation, RuntimeEvent, std::uint32_t,
                            std::chrono::system_clock::time_point) override;
    HookOutboxEntry publish(std::string_view) override;
    std::optional<HookLease> claim(std::string_view, std::string_view, std::chrono::system_clock::time_point,
                                   std::chrono::milliseconds) override;
    std::optional<HookOutboxEntry> settle(std::string_view, std::uint64_t,
                                          HookExecutionReceipt,
                                           std::chrono::system_clock::time_point) override;
    std::optional<HookOutboxEntry> reconcile(std::string_view, HookExecutionState,
                                             std::string_view, std::uint64_t,
                                             HookExecutionReceipt) override;
    std::vector<HookOutboxEntry> pending() const override;
    std::vector<HookOutboxEntry> reconciliation_required() const override;
    std::optional<HookOutboxEntry> get(std::string_view) const override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/** One transport-neutral Hook execution attempt and its context evidence. */
struct HookExecutionAttempt {
    HookExecutionReceipt receipt;
    std::vector<ContextArtifact> artifacts;
};

using HookExecutionBackend = std::function<asio::awaitable<HookExecutionAttempt>(
    const HookInvocation&,
    const RuntimeEvent&,
    std::uint32_t attempt,
    ToolExecutionContext)>;

using HookArtifactPublisher = std::function<void(
    const HookInvocation&,
    const RuntimeEvent&,
    const std::vector<ContextArtifact>&)>;

/** Owner-scoped idempotent publisher for HookOutput artifacts. */
class NEOGRAPH_API ContextStoreHookArtifactPublisher final {
public:
    explicit ContextStoreHookArtifactPublisher(std::shared_ptr<ContextStore> store);
    void publish(const HookInvocation& invocation,
                 const RuntimeEvent& event,
                 const std::vector<ContextArtifact>& artifacts) const;
private:
    std::shared_ptr<ContextStore> store_;
};

/** Plans, journals, and executes mandatory hooks without owning lifecycle call sites. */
class NEOGRAPH_API MandatoryHookRunner final {
public:
    MandatoryHookRunner(std::shared_ptr<HookJournal> journal, std::shared_ptr<const HookRegistry> registry,
                        std::shared_ptr<const NativeHookExecutionAdapter> adapter,
                        std::string worker_id = "local-hook-runner");
    MandatoryHookRunner(std::shared_ptr<HookJournal> journal,
                        std::shared_ptr<const HookRegistry> registry,
                        HookExecutionBackend backend,
                        HookArtifactPublisher artifact_publisher = {},
                        std::string worker_id = "hook-backend-runner");
    asio::awaitable<void> run_async(const RuntimeEvent& event,
                                    std::chrono::system_clock::time_point deadline,
                                    std::uint32_t max_attempts = 1);
private:
    std::shared_ptr<HookJournal> journal_;
    std::shared_ptr<const HookRegistry> registry_;
    std::shared_ptr<const NativeHookExecutionAdapter> adapter_;
    HookExecutionBackend backend_;
    HookArtifactPublisher artifact_publisher_;
    std::string worker_id_;
};

} // namespace neograph
