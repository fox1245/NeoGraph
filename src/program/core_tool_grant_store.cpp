#include <neograph/program/core_tool_grant_store.h>
#include "lock_audit.h"

#include <stdexcept>
#include <utility>

namespace neograph::program {

ProgramCoreToolGrantResolver make_durable_core_tool_grant_resolver(
    std::shared_ptr<ProgramCoreToolGrantStore> store,
    ProgramCoreToolGrantPolicyFactory policy_factory) {
    if (!store || !policy_factory)
        throw std::invalid_argument("Durable Core Tool grants require a store and host policy");
    // The resolver is a synchronous callback of the runtime: it reads the store through a blocking view.
    return [store = std::make_shared<BlockingProgramCoreToolGrantStore>(std::move(store)),
            policy_factory = std::move(policy_factory)](
               const ProgramCoreToolGrantContext& context)
               -> std::optional<ProgramCoreToolGrant> {
        const auto record = store->load(context);
        if (!record || record->owner_scope != context.owner_scope ||
            record->program_version_id != context.program_version_id ||
            record->run_id != context.run_id ||
            record->operation_id != context.operation_id ||
            record->attempt != context.attempt ||
            record->binding_fingerprint != context.binding_fingerprint ||
            record->grant_id.empty())
            return std::nullopt;
        auto grant = policy_factory(*record);
        if (!grant || grant->owner_scope != record->owner_scope ||
            grant->program_version_id != record->program_version_id ||
            grant->run_id != record->run_id ||
            grant->operation_id != record->operation_id ||
            grant->attempt != record->attempt ||
            grant->binding_fingerprint != record->binding_fingerprint ||
            grant->grant_id != record->grant_id ||
            !grant->gate || !grant->controller)
            return std::nullopt;
        return grant;
    };
}


// ---- ProgramCoreToolGrantStore : the synchronous implementer base and the blocking facade (#556) ----

asio::awaitable<ProgramCoreToolGrantAdmission> SyncProgramCoreToolGrantStore::admit(
    const ProgramCoreToolGrantRecord& record) {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::admit");
    if (!offloads()) co_return admit_sync(record);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<ProgramCoreToolGrantAdmission>([this, record = record]() mutable -> ProgramCoreToolGrantAdmission {
        return admit_sync(record);
    });
}

asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> SyncProgramCoreToolGrantStore::load(
    const ProgramCoreToolGrantContext& context) const {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::load");
    if (!offloads()) co_return load_sync(context);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<std::optional<ProgramCoreToolGrantRecord>>([this, context = context]() mutable -> std::optional<ProgramCoreToolGrantRecord> {
        return load_sync(context);
    });
}

asio::awaitable<bool> SyncProgramCoreToolGrantStore::revoke(
    const ProgramCoreToolGrantContext& context) {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::revoke");
    if (!offloads()) co_return revoke_sync(context);
    // The work runs on a pool thread and may outlive this frame: it owns its arguments.
    co_return co_await detail::offload_sync_call<bool>([this, context = context]() mutable -> bool {
        return revoke_sync(context);
    });
}

BlockingProgramCoreToolGrantStore::BlockingProgramCoreToolGrantStore(std::shared_ptr<ProgramCoreToolGrantStore> store, BlockingStorePolicy policy)
    : store_(std::move(store)), policy_(std::move(policy)) {
    if (!store_) throw std::invalid_argument("BlockingProgramCoreToolGrantStore requires a store");
    if (!policy_.await_synchronous_stores) sync_ = dynamic_cast<SyncProgramCoreToolGrantStore*>(store_.get());
}

BlockingProgramCoreToolGrantStore::BlockingProgramCoreToolGrantStore(ProgramCoreToolGrantStore& store, BlockingStorePolicy policy)
    : BlockingProgramCoreToolGrantStore(std::shared_ptr<ProgramCoreToolGrantStore>(std::shared_ptr<ProgramCoreToolGrantStore>{}, &store), std::move(policy)) {}

ProgramCoreToolGrantAdmission BlockingProgramCoreToolGrantStore::admit(
    const ProgramCoreToolGrantRecord& record) const {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::admit");
    if (sync_) return sync_->admit_sync(record);
    return detail::block_on_store(store_->admit(record), policy_, "ProgramCoreToolGrantStore::admit");
}

std::optional<ProgramCoreToolGrantRecord> BlockingProgramCoreToolGrantStore::load(
    const ProgramCoreToolGrantContext& context) const {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::load");
    if (sync_) return sync_->load_sync(context);
    return detail::block_on_store(store_->load(context), policy_, "ProgramCoreToolGrantStore::load");
}

bool BlockingProgramCoreToolGrantStore::revoke(const ProgramCoreToolGrantContext& context) const {
    detail::assert_no_pure_lock_held("ProgramCoreToolGrantStore::revoke");
    if (sync_) return sync_->revoke_sync(context);
    return detail::block_on_store(store_->revoke(context), policy_, "ProgramCoreToolGrantStore::revoke");
}

}  // namespace neograph::program
