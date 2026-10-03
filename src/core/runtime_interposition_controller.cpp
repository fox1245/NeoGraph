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
    std::shared_ptr<Provider> provider;
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
        , controlled(provider, std::move(receipts), std::move(binding))
        , provider(std::move(provider)) {}
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

asio::awaitable<sp::runtime::Result> RuntimeInterpositionController::invoke_async(
    ProviderRequest request,
    std::vector<sp::Message> host_instructions,
    std::vector<sp::Message> trusted_supplemental,
    graph::ProviderCallIdentity identity) {
    // Capture ownership before returning a lazy awaitable; callers may destroy
    // or move the controller before the executor resumes the operation.
    return [](std::shared_ptr<Impl> impl, ProviderRequest request,
              std::vector<sp::Message> host_instructions,
              std::vector<sp::Message> trusted_supplemental,
              graph::ProviderCallIdentity identity)
        -> asio::awaitable<sp::runtime::Result> {
    std::string owner_id;
    std::uint64_t generation;
    std::shared_ptr<HookRuntime> hooks;
    ContextEpoch epoch = [&] {
        std::lock_guard lock(impl->mutex);
        if (!impl->epoch) {
            throw std::runtime_error("Strict provider dispatch requires an active context epoch and assembly receipt");
        }
        owner_id = impl->owner_id;
        generation = impl->generation;
        hooks = impl->hooks;
        return *impl->epoch;
    }();
    // The admitted RAW feed is authoritative; explicit instruction/task slots
    // are supplied by the host, never recovered from caller presentation state.
    clear_provider_request_messages(request);
    const auto cancellation = request.cancel_token;
    std::optional<RuntimeTurn> assembled_turn;
    try {
        assembled_turn.emplace(impl->assembler.assemble(
            *impl->provider, owner_id, epoch, std::move(request),
            std::move(host_instructions), std::move(trusted_supplemental)));
    } catch (const ProviderFailure& failure) {
        co_return failure.outcome();
    }
    auto& turn = *assembled_turn;
    ProviderBudgetClaim claim;
    try {
        claim = reserve_provider_dispatch(turn.request,
            ProviderDispatchBudget{identity.usage, identity.model_token_budget,
                identity.budget_exhausted, identity.budget_cancel_token,
                identity.managed_budget_lease, identity.managed_budget_store,
                graph::managed_provider_effect_id(identity)});
    } catch (const ProviderFailure& failure) {
        co_return failure.outcome();
    }
    const auto remaining = turn.request.deadline() - std::chrono::steady_clock::now();
    const auto hook_deadline = std::chrono::system_clock::now() +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
    std::string dispatch_id;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->epoch || impl->generation != generation ||
            impl->owner_id != owner_id || impl->epoch->id() != epoch.id()) {
            throw std::runtime_error("Context epoch changed before provider dispatch admission");
        }
        dispatch_id = epoch.run_id() + "-dispatch-" + std::to_string(++impl->dispatch_sequence);
    }
    if (hooks) {
        // GCC 13 ICEs when the returned awaitable is consumed as a temporary.
        auto emission = hooks->emit_async(HookPhase::BeforeProviderRequest, "provider_request", owner_id,
                                           epoch.run_id(), json{{"dispatch_id", dispatch_id}, {"epoch_id", epoch.id()}},
                                           cancellation, hook_deadline);
        co_await std::move(emission);
    }
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->epoch || impl->generation != generation ||
            impl->owner_id != owner_id || impl->epoch->id() != epoch.id()) {
            throw std::runtime_error("Context epoch changed during provider dispatch hooks");
        }
    }
    auto outcome = co_await impl->controlled.dispatch_prepared_async(
        owner_id, std::move(dispatch_id), turn.assembly_receipt, std::move(turn.request),
        std::move(claim));
    if (hooks) {
        std::size_t tool_call_count = 0;
        for (const auto& message : outcome_messages(*outcome)) {
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<sp::ToolCall>(&part);
                    call && call->kind == sp::ToolCallKind::ClientExecuted) ++tool_call_count;
            }
        }
        try {
            auto emission = hooks->emit_async(
                HookPhase::AfterProviderResponse, "provider_response", owner_id, epoch.run_id(),
                json{{"epoch_id", epoch.id()}, {"success", std::holds_alternative<sp::Completion>(*outcome)},
                     {"tool_call_count", tool_call_count}},
                cancellation, hook_deadline);
            co_await std::move(emission);
        } catch (...) {
            throw ProviderOutcomeError("Provider response hook failed", outcome, std::current_exception());
        }
    }
    co_return outcome;
    }(impl_, std::move(request), std::move(host_instructions),
      std::move(trusted_supplemental), std::move(identity));
}

sp::runtime::Result RuntimeInterpositionController::invoke(
    ProviderRequest request, graph::ProviderCallIdentity identity) {
    const auto cancellation = request.cancel_token;
    return async::run_sync(invoke_async(std::move(request), {}, {}, std::move(identity)),
                           cancellation.get());
}

}  // namespace neograph
