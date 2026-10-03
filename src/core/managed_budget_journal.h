#pragma once

#include <neograph/graph/checkpoint.h>
#include <core/native_archive.h>

namespace neograph::graph::detail {

// A single compiled policy runs inside each backend's own locked transaction.
// Persist heads and individual effects separately; never scan/rewrite histories.
class NEOGRAPH_API ManagedBudgetJournalAccess final {
public:
    static std::string storage_key(const ManagedBudgetLeaseScope& scope);
    static bool checkpoint_requires_obligation(const Checkpoint& checkpoint);
    static std::shared_ptr<OwnedManagedBudgetLease> acquire(
        json& head, bool prior_obligation, const ManagedBudgetLeaseScope& scope,
        const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment);
    static std::shared_ptr<OwnedManagedBudgetLease> acquire_branch(
        json& head, bool prior_obligation, const ManagedBudgetLeaseScope& scope,
        const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment,
        const std::string& execution_thread_id, const std::string& execution_storage_thread_id);
    static void select_branch_head(json& head, const std::string& checkpoint_id,
                                   const std::string& commitment);
    static std::string validate_shared_bank_fork(
        const json& head, const Checkpoint& source, const Checkpoint& forked);
    static ManagedBudgetEffectReceipt begin(
        json& head, json& effect, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const std::string& effect_id, std::uint64_t amount, const std::string& request_digest);
    static void settle(
        json& head, json& stored_effect, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result outcome,
        const UsageAccumulator::AuthoritySnapshot& authority);
    static void publish(json& head, const std::shared_ptr<OwnedManagedBudgetLease>& lease,
                        const Checkpoint& checkpoint);
    static void refresh_transport(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
                                  const json& transport);
    static void bind_native_archive(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
                                    std::shared_ptr<sp::NativeArchive> archive);
    static void bind_cpp_native_retention(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& actual_owned_head);
    static bool retains_native_checkpoint(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
    static void release(json& head, const std::shared_ptr<OwnedManagedBudgetLease>& lease);
    static void refresh(const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& head);

    // Transport carries an untrusted receipt claim, not authority. Every real
    // backend operation checks it against current actor/generation/head state.
    static json lease_transport(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
    static std::shared_ptr<OwnedManagedBudgetLease> lease_from_transport(const json& value);
    static json effect_transport(const ManagedBudgetEffectReceipt& effect);
    static ManagedBudgetEffectReceipt effect_from_transport(const json& value);
};

NEOGRAPH_API std::string managed_budget_deadline_clock_identity();

} // namespace neograph::graph::detail
