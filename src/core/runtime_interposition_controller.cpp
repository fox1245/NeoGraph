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
    std::shared_ptr<HookRuntime> hooks;

    Impl(std::shared_ptr<ContextStore> store_, std::shared_ptr<Provider> provider,
          std::shared_ptr<ProviderDispatchReceiptStore> receipts, std::string binding,
          std::uint64_t max_input_tokens, RuntimeContextRequirements requirements)
        : store(std::move(store_))
        , receipts(receipts)
        , assembler(*store, max_input_tokens, std::move(requirements))
        , controlled(std::move(provider), std::move(receipts), std::move(binding)) {}
};

RuntimeInterpositionController::RuntimeInterpositionController(
    std::shared_ptr<Provider> provider, std::shared_ptr<ContextStore> context_store,
    std::shared_ptr<ProviderDispatchReceiptStore> dispatch_store,
    std::string provider_binding_identity, std::uint64_t max_input_tokens,
    std::vector<std::string> static_required_skill_artifact_ids)
    : RuntimeInterpositionController(
          std::move(provider), std::move(context_store), std::move(dispatch_store),
          std::move(provider_binding_identity),
          RuntimeContextRequirements{static_required_skill_artifact_ids,
                                     std::move(static_required_skill_artifact_ids)},
          max_input_tokens) {}

RuntimeInterpositionController::RuntimeInterpositionController(
    std::shared_ptr<Provider> provider, std::shared_ptr<ContextStore> context_store,
    std::shared_ptr<ProviderDispatchReceiptStore> dispatch_store,
    std::string provider_binding_identity, RuntimeContextRequirements requirements,
    std::uint64_t max_input_tokens) {
    if (!context_store || !dispatch_store) {
        throw std::invalid_argument("Runtime interposition requires durable context and dispatch stores");
    }
    impl_ = std::make_shared<Impl>(std::move(context_store), std::move(provider),
                                    std::move(dispatch_store), std::move(provider_binding_identity),
                                    max_input_tokens, std::move(requirements));
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
void RuntimeInterpositionController::set_hook_runtime(std::shared_ptr<HookRuntime> hooks) {
    std::lock_guard lock(impl_->mutex);
    impl_->hooks = std::move(hooks);
}

asio::awaitable<ChatCompletion> RuntimeInterpositionController::invoke_async(
    CompletionParams params, StreamCallback on_chunk) {
    co_return co_await invoke_async(std::move(params), std::move(on_chunk), {}, {});
}

asio::awaitable<ChatCompletion> RuntimeInterpositionController::invoke_async(
    CompletionParams params, StreamCallback on_chunk,
    std::vector<ChatMessage> host_instructions,
    std::vector<ChatMessage> trusted_supplemental) {
    std::string owner_id;
    std::uint64_t generation;
    std::shared_ptr<HookRuntime> hooks;
    ContextEpoch epoch = [&] {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->epoch) {
            throw std::runtime_error("Strict provider dispatch requires an active context epoch and assembly receipt");
        }
        owner_id = impl_->owner_id;
        generation = impl_->generation;
        hooks = impl_->hooks;
        return *impl_->epoch;
    }();
    // Caller conversation is legacy presentation state. The admitted epoch's
    // feed is authoritative; explicit slots are host-built by consumers.
    params.messages.clear();
    const auto cancellation = params.cancel_token;
    const auto hook_deadline = [timeout_seconds = params.timeout_seconds]()
        -> std::optional<std::chrono::system_clock::time_point> {
        if (timeout_seconds <= 0) return std::nullopt;
        return std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
    };
    CompletionRequest template_request = on_chunk ? CompletionRequest::stream(std::move(params), std::move(on_chunk))
                                                   : CompletionRequest::collect(std::move(params));
    auto turn = impl_->assembler.assemble(owner_id, epoch, std::move(template_request),
                                          std::move(host_instructions),
                                          std::move(trusted_supplemental));
    std::string dispatch_id;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->epoch || impl_->generation != generation ||
            impl_->owner_id != owner_id || impl_->epoch->id() != epoch.id()) {
            throw std::runtime_error("Context epoch changed before provider dispatch admission");
        }
        dispatch_id = epoch.run_id() + "-dispatch-" + std::to_string(++impl_->dispatch_sequence);
    }
    if (hooks) {
        // GCC 13 ICEs when the returned awaitable is consumed as a temporary.
        auto emission = hooks->emit_async(HookPhase::BeforeProviderRequest, "provider_request", owner_id,
                                           epoch.run_id(), json{{"dispatch_id", dispatch_id}, {"epoch_id", epoch.id()}},
                                           cancellation, hook_deadline());
        co_await std::move(emission);
    }
    auto completion = co_await impl_->controlled.dispatch_async(owner_id, std::move(dispatch_id),
                                                                  turn.assembly_receipt,
                                                                  std::move(turn.request));
    if (hooks) {
        auto emission = hooks->emit_async(
            HookPhase::AfterProviderResponse, "provider_response", owner_id, epoch.run_id(),
            json{{"epoch_id", epoch.id()}, {"tool_call_count", completion.message.tool_calls.size()}},
            cancellation, hook_deadline());
        co_await std::move(emission);
    }
    co_return completion;
}

ChatCompletion RuntimeInterpositionController::invoke(CompletionParams params, StreamCallback on_chunk) {
    auto* token = params.cancel_token ? params.cancel_token.get() : nullptr;
    return async::run_sync(invoke_async(std::move(params), std::move(on_chunk)), token);
}

}  // namespace neograph
