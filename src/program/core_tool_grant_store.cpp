#include <neograph/program/core_tool_grant_store.h>

#include <stdexcept>
#include <utility>

namespace neograph::program {

ProgramCoreToolGrantResolver make_durable_core_tool_grant_resolver(
    std::shared_ptr<ProgramCoreToolGrantStore> store,
    ProgramCoreToolGrantPolicyFactory policy_factory) {
    if (!store || !policy_factory)
        throw std::invalid_argument("Durable Core Tool grants require a store and host policy");
    return [store = std::move(store), policy_factory = std::move(policy_factory)](
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

}  // namespace neograph::program
