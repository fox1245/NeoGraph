#include <neograph/runtime_interposition_controller.h>

#include <neograph/async/run_sync.h>

#include <mutex>
#include <stdexcept>

namespace neograph {

struct RuntimeInterpositionController::Impl {
    std::shared_ptr<ContextStore> store;
    std::shared_ptr<ProviderDispatchReceiptStore> receipts;
    RuntimeTurnAssembler assembler;
    ControlledProvider controlled;
    std::mutex mutex;
    std::string owner_id;
    std::optional<ContextEpoch> epoch;
    std::uint64_t generation = 0;
    std::uint64_t dispatch_sequence = 0;

    Impl(std::shared_ptr<ContextStore> store_, std::shared_ptr<Provider> provider,
          std::shared_ptr<ProviderDispatchReceiptStore> receipts, std::string binding,
          std::uint64_t max_input_tokens, std::vector<std::string> static_required_skill_artifact_ids)
        : store(std::move(store_))
        , receipts(receipts)
        , assembler(*store, std::move(static_required_skill_artifact_ids), max_input_tokens)
        , controlled(std::move(provider), std::move(receipts), std::move(binding)) {}
};

RuntimeInterpositionController::RuntimeInterpositionController(
    std::shared_ptr<Provider> provider, std::shared_ptr<ContextStore> context_store,
    std::shared_ptr<ProviderDispatchReceiptStore> dispatch_store,
    std::string provider_binding_identity, std::uint64_t max_input_tokens,
    std::vector<std::string> static_required_skill_artifact_ids) {
    if (!context_store || !dispatch_store) {
        throw std::invalid_argument("Runtime interposition requires durable context and dispatch stores");
    }
    impl_ = std::make_shared<Impl>(std::move(context_store), std::move(provider),
                                    std::move(dispatch_store), std::move(provider_binding_identity),
                                    max_input_tokens, std::move(static_required_skill_artifact_ids));
}
RuntimeInterpositionController::~RuntimeInterpositionController() = default;
RuntimeInterpositionController::RuntimeInterpositionController(RuntimeInterpositionController&&) noexcept = default;
RuntimeInterpositionController& RuntimeInterpositionController::operator=(RuntimeInterpositionController&&) noexcept = default;

void RuntimeInterpositionController::activate(std::string owner_id, ContextEpoch epoch) {
    if (owner_id.empty()) throw std::invalid_argument("Runtime interposition owner_id must not be empty");
    if (epoch.guarantee_profile() == RuntimeGuaranteeProfile::Strict &&
        dynamic_cast<DurableProviderDispatchReceiptStore*>(impl_->receipts.get()) == nullptr) {
        throw std::invalid_argument("Strict provider dispatch requires a durable dispatch receipt store");
    }
    std::lock_guard lock(impl_->mutex);
    impl_->owner_id = std::move(owner_id);
    impl_->epoch = std::move(epoch);
    ++impl_->generation;
    impl_->dispatch_sequence = 0;
}
void RuntimeInterpositionController::clear() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->owner_id.clear();
    impl_->epoch.reset();
    ++impl_->generation;
    impl_->dispatch_sequence = 0;
}
bool RuntimeInterpositionController::active() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->epoch.has_value();
}

asio::awaitable<ChatCompletion> RuntimeInterpositionController::invoke_async(
    CompletionParams params, StreamCallback on_chunk) {
    std::string owner_id;
    std::uint64_t generation;
    ContextEpoch epoch = [&] {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->epoch) {
            throw std::runtime_error("Strict provider dispatch requires an active context epoch and assembly receipt");
        }
        owner_id = impl_->owner_id;
        generation = impl_->generation;
        return *impl_->epoch;
    }();
    // Caller-provided messages are legacy presentation state. The admitted
    // epoch's feed is the only prompt authority on the controlled path.
    params.messages.clear();
    CompletionRequest template_request = on_chunk ? CompletionRequest::stream(std::move(params), std::move(on_chunk))
                                                   : CompletionRequest::collect(std::move(params));
    auto turn = impl_->assembler.assemble(owner_id, epoch, std::move(template_request));
    std::string dispatch_id;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->epoch || impl_->generation != generation ||
            impl_->owner_id != owner_id || impl_->epoch->id() != epoch.id()) {
            throw std::runtime_error("Context epoch changed before provider dispatch admission");
        }
        dispatch_id = epoch.run_id() + "-dispatch-" + std::to_string(++impl_->dispatch_sequence);
    }
    co_return co_await impl_->controlled.dispatch_async(std::move(dispatch_id), turn.assembly_receipt,
                                                         std::move(turn.request));
}

ChatCompletion RuntimeInterpositionController::invoke(CompletionParams params, StreamCallback on_chunk) {
    auto* token = params.cancel_token ? params.cancel_token.get() : nullptr;
    return async::run_sync(invoke_async(std::move(params), std::move(on_chunk)), token);
}

}  // namespace neograph
