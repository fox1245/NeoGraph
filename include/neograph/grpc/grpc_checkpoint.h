// neograph::grpc — remote CheckpointStore over gRPC.
//
// Ported from the predecessor NexaGraph (src/nexagraph/grpc_checkpoint
// .{h,cpp}), which already inherited neograph::graph::CheckpointStore.
// Two halves:
//
//   * GrpcCheckpointStore  — a CheckpointStore the engine SETS; every
//     save/load round-trips to a remote gRPC CheckpointService instead
//     of a local DB. The agent process carries no DB driver.
//   * CheckpointServiceImpl + run_checkpoint_server — the SERVER side:
//     wraps any real CheckpointStore (InMemory/Sqlite/Postgres) and
//     exposes it over gRPC. Lives in a separate process, owns the DB.
//
// Opt-in (NEOGRAPH_BUILD_GRPC=ON, default OFF). Whole header behind
// NEOGRAPH_HAVE_GRPC so the umbrella include never pulls grpc++.

#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <neograph/graph/checkpoint.h>

namespace neograph::grpc {

// ── Client side ──────────────────────────────────────────────────────
//
// Set this on the engine just like SqliteCheckpointStore:
//   auto store = std::make_shared<neograph::grpc::GrpcCheckpointStore>(
//       "checkpoint-host:50071");
//   auto engine = GraphEngine::compile(def, ctx, store);
//
// Every checkpoint then save/loads over gRPC. The sync checkpoint
// operations, managed-budget denial query, and owned journal operations are
// overridden; async peers inherit the base facade (run_sync over the sync
// impl), and pending-writes stay the legacy no-op default.
class NEOGRAPH_API GrpcCheckpointStore : public neograph::graph::CheckpointStore {
public:
    /// target e.g. "localhost:50071" / "cp-host:50071". Insecure
    /// channel (wire your own TLS via the stub ctor in a wrapper if
    /// needed — same posture as GraphService::run_server).
    explicit GrpcCheckpointStore(const std::string& target);
    /// Settlement requires actual shared trusted archive custody reachable
    /// from both endpoints. Portable outcome JSON cannot authorize a refund.
    GrpcCheckpointStore(const std::string& target,
                        std::shared_ptr<sp::NativeArchive> native_archive);
    ~GrpcCheckpointStore() override;

    void save(const neograph::graph::Checkpoint& cp) override;
    std::optional<neograph::graph::Checkpoint>
        load_latest(const std::string& thread_id) override;
    std::optional<neograph::graph::Checkpoint>
        load_by_id(const std::string& id) override;
    std::vector<neograph::graph::Checkpoint>
        list(const std::string& thread_id, int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;
    bool requires_managed_budget(const std::string& thread_id) override;
    std::shared_ptr<neograph::graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const neograph::graph::ManagedBudgetLeaseScope& scope,
        const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override;
    neograph::graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<neograph::graph::OwnedManagedBudgetLease>& lease,
        const std::string& effect_id, std::uint64_t exact_claim_amount,
        const std::string& prepared_request_digest) override;
    void settle_managed_budget_effect(
        const std::shared_ptr<neograph::graph::OwnedManagedBudgetLease>& lease,
        const neograph::graph::ManagedBudgetEffectReceipt& effect,
        sp::runtime::Result genuine_outcome,
        const neograph::UsageAccumulator::AuthoritySnapshot& authority) override;
    void publish_managed_budget_checkpoint(
        const std::shared_ptr<neograph::graph::OwnedManagedBudgetLease>& lease,
        const neograph::graph::Checkpoint& checkpoint) override;
    void release_managed_budget_lease(
        const std::shared_ptr<neograph::graph::OwnedManagedBudgetLease>& lease) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Server side ──────────────────────────────────────────────────────
//
// Build + run a blocking gRPC CheckpointService backed by `backend`
// (e.g. a SqliteCheckpointStore) on `address` ("0.0.0.0:50071"), until
// the process is signalled. Insecure credentials — front with mTLS in
// prod via your own ServerBuilder if needed.
NEOGRAPH_API void run_checkpoint_server(
    const std::string& address,
    std::shared_ptr<neograph::graph::CheckpointStore> backend);
/// This overload resolves settlement references using real trusted native
/// custody shared with clients. Missing/unverifiable custody fails closed.
NEOGRAPH_API void run_checkpoint_server(
    const std::string& address,
    std::shared_ptr<neograph::graph::CheckpointStore> backend,
    std::shared_ptr<sp::NativeArchive> native_archive);

// ── Serialization (exposed for tests / payload-size measurement) ─────
//
// Full Checkpoint ⇄ JSON. Handles the fields NexaGraph's flat mapping
// didn't: next_nodes (vector), interrupt_phase (enum via to_string /
// parse_checkpoint_phase), barrier_state (nested map), schema_version.
NEOGRAPH_API std::string checkpoint_to_json(
    const neograph::graph::Checkpoint& cp);
NEOGRAPH_API neograph::graph::Checkpoint checkpoint_from_json(
    const std::string& s);

}  // namespace neograph::grpc

#endif  // NEOGRAPH_HAVE_GRPC
