#include <neograph/strict_runtime.h>

#include <neograph/graph/engine.h>

#include <stdexcept>

namespace neograph {

StrictRuntimeProfile::StrictRuntimeProfile(StrictRuntimeProfileConfig config)
    : context_store_(std::move(config.context_store)),
      dispatch_store_(std::move(config.dispatch_store)),
      hook_runtime_(std::move(config.hook_runtime)) {
    if (!config.provider || !context_store_ || !dispatch_store_ || !hook_runtime_) {
        throw std::invalid_argument(
            "Strict runtime requires provider, durable stores, and Hook runtime");
    }
    if (config.max_input_tokens == 0) {
        throw std::invalid_argument("Strict runtime requires a non-zero input token budget");
    }
    auto required = std::move(config.required_context_artifact_ids);
    required.insert(required.end(), config.required_skill_artifact_ids.begin(),
                    config.required_skill_artifact_ids.end());
    interposition_ = std::make_shared<RuntimeInterpositionController>(
        std::move(config.provider), context_store_, dispatch_store_,
        std::move(config.provider_binding_identity),
        RuntimeContextRequirements{std::move(required),
                                   std::move(config.required_skill_artifact_ids)},
        config.max_input_tokens);
    interposition_->set_hook_runtime(hook_runtime_);
}

void StrictRuntimeProfile::activate(std::string owner_id, ContextEpoch epoch) {
    if (epoch.guarantee_profile() != RuntimeGuaranteeProfile::Strict) {
        throw std::invalid_argument(
            "Strict runtime accepts only a Strict ContextEpoch");
    }
    interposition_->activate(std::move(owner_id), std::move(epoch));
}

void StrictRuntimeProfile::clear() noexcept { interposition_->clear(); }
bool StrictRuntimeProfile::active() const noexcept { return interposition_->active(); }

void StrictRuntimeProfile::attach(graph::GraphEngine& engine) const {
    engine.set_hook_runtime(hook_runtime_);
    engine.set_runtime_interposition(interposition_);
}

std::shared_ptr<RuntimeInterpositionController>
StrictRuntimeProfile::interposition() const noexcept {
    return interposition_;
}
std::shared_ptr<HookRuntime> StrictRuntimeProfile::hooks() const noexcept {
    return hook_runtime_;
}
std::shared_ptr<DurableContextStore>
StrictRuntimeProfile::context_store() const noexcept {
    return context_store_;
}
std::shared_ptr<DurableProviderDispatchReceiptStore>
StrictRuntimeProfile::dispatch_store() const noexcept {
    return dispatch_store_;
}

}  // namespace neograph
