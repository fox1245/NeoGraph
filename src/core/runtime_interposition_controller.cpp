#include <neograph/runtime_interposition_controller.h>

#include <neograph/async/run_sync.h>

#include "canonical_json.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace neograph {
namespace {

// The strict path is host-stepped: the epoch's RAW range is fixed, so a graph
// or agent loop that keeps going inside one run (LLM call, tool dispatch, LLM
// call) would resend the first turn's context and silently drop everything the
// loop produced since (#444). The controller cannot append records or
// activate an epoch on the host's behalf, so it refuses such a call instead.
constexpr const char* kHostSteppedViolation =
    "Strict provider dispatch is host-stepped: the host appends RAW records and "
    "activates a new epoch for every provider call. This call presents assistant "
    "or tool messages added since the active epoch's first dispatch, so the graph "
    "or agent loop advanced inside one run and the provider would not receive "
    "them. Append those messages to the context feed and activate a new epoch "
    "before this call.";

// Distinct first presentations remembered per epoch. Past the cap a new
// presentation is simply not tracked, so the memory an epoch holds stays
// bounded; the epoch's own activation forgets all of them.
constexpr std::size_t kMaxFirstPresentations = 16;

// The conversation a call site showed the controller on its first dispatch
// under the active epoch: how many messages, and a digest chain over them.
struct FirstPresentation {
    std::string call_site;
    std::size_t count = 0;
    std::string chain;
    bool operator==(const FirstPresentation&) const = default;
};

// An assistant reply or a tool result moves a conversation forward. Counted
// per assistant message and per ToolResult part, because the Anthropic and
// Gemini families merge adjacent tool results into one message and relabel the
// role, while the number of results stays the same.
std::size_t advance_units(const sp::Message& message) {
    std::size_t results = 0;
    for (const auto& part : message.parts)
        if (std::holds_alternative<sp::ToolResult>(part)) ++results;
    std::size_t units = results;
    if (message.role == sp::Role::Assistant) ++units;
    if (message.role == sp::Role::Tool && results == 0) ++units;
    return units;
}

// chain[i] identifies the first i messages; chain[0] is the empty presentation.
std::vector<std::string> presentation_chain(const std::vector<sp::Message>& messages) {
    std::vector<std::string> chain;
    chain.reserve(messages.size() + 1);
    chain.emplace_back();
    for (const auto& message : messages) {
        chain.push_back(detail::sha256_identity(
            "NeoGraph provider presentation v1", "prefix-chain/v1",
            chain.back() + message_digest(message)));
    }
    return chain;
}

// The conversation the caller put on the request, before the controller
// replaces it with the epoch's RAW range. Null when the request carries the
// legacy typed Chat form, which has no portable message view.
const std::vector<sp::Message>* presented_messages(const ProviderRequest& request) {
    return std::visit([](const auto& payload) -> const std::vector<sp::Message>* {
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::chat::Request>)
            return payload.messages.empty() ? &payload.canonical_messages : nullptr;
        else
            return &payload.messages;
    }, request.payload);
}

struct PresentationVerdict {
    /// The presentation grew past a first presentation of the same call site
    /// by assistant or tool messages that this dispatch does not carry.
    bool advanced = false;
    /// Set when no remembered presentation relates to this one.
    std::optional<FirstPresentation> first;
};

// Pure and lock-free: `known` is a snapshot. An identical presentation is a
// retry of the same turn. A presentation that extends a remembered one is the
// same conversation moving on; it is a violation only when the added assistant
// or tool messages are not also carried by the host slots or the trusted
// supplemental input (built-in loop nodes such as the plan executor carry
// their growing conversation there, and that is receipt-bound).
PresentationVerdict judge_presentation(
    const std::vector<FirstPresentation>& known, const std::string& call_site,
    const std::vector<sp::Message>& presented,
    const std::vector<sp::Message>& host_instructions,
    const std::vector<sp::Message>& trusted_supplemental) {
    PresentationVerdict verdict;
    // An empty presentation says nothing about a conversation.
    if (presented.empty()) return verdict;
    const auto chain = presentation_chain(presented);
    bool related = false;
    std::optional<std::size_t> carried;
    for (const auto& first : known) {
        if (first.call_site != call_site || first.count > presented.size() ||
            chain[first.count] != first.chain) continue;
        related = true;
        std::size_t grown = 0;
        for (auto index = first.count; index < presented.size(); ++index)
            grown += advance_units(presented[index]);
        if (grown == 0) continue;
        if (!carried) {
            carried = 0;
            for (const auto& message : host_instructions) *carried += advance_units(message);
            for (const auto& message : trusted_supplemental) *carried += advance_units(message);
        }
        if (grown > *carried) {
            verdict.advanced = true;
            return verdict;
        }
    }
    if (!related) verdict.first = FirstPresentation{call_site, presented.size(), chain.back()};
    return verdict;
}

}  // namespace

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
    /// First presentations under the active epoch; reset with every activation.
    std::vector<FirstPresentation> first_presentations;

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
    activate(std::move(owner_id), std::move(epoch), std::nullopt);
}

void RuntimeInterpositionController::activate(
    std::string owner_id, ContextEpoch epoch, std::optional<ContextEpoch> predecessor) {
    if (owner_id.empty()) throw std::invalid_argument("Runtime interposition owner_id must not be empty");
    if (epoch.guarantee_profile() == RuntimeGuaranteeProfile::Strict &&
        dynamic_cast<DurableProviderDispatchReceiptStore*>(impl_->receipts.get()) == nullptr) {
        throw std::invalid_argument("Strict provider dispatch requires a durable dispatch receipt store");
    }
    if (!predecessor) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->epoch && impl_->owner_id == owner_id && epoch.predecessor_id() &&
            impl_->epoch->id() == *epoch.predecessor_id()) {
            predecessor = impl_->epoch;
        }
    }
    if (epoch.prefix_claim() != ContextPrefixClaim::Extends) {
        // Without a claim to extend only a change of layout has to be declared; that needs no artifacts.
        if (predecessor) verify_context_epoch_claim(epoch, {}, &*predecessor, {});
    } else {
        const auto load = [&](const ContextEpoch& source) {
            std::vector<ContextArtifact> artifacts;
            artifacts.reserve(source.artifact_ids().size());
            for (const auto& id : source.artifact_ids()) {
                auto artifact = impl_->store->get_artifact(owner_id, id);
                if (!artifact) {
                    throw ContextStabilityError(
                        ContextStabilityRefusal::ArtifactsMismatch,
                        "Context epoch artifact is not available to this owner");
                }
                artifacts.push_back(std::move(*artifact));
            }
            return artifacts;
        };
        if (predecessor) {
            verify_context_epoch_claim(epoch, load(epoch), &*predecessor, load(*predecessor));
        } else {
            verify_context_epoch_claim(epoch, {}, nullptr, {});
        }
    }
    std::lock_guard lock(impl_->mutex);
    impl_->owner_id = std::move(owner_id);
    impl_->epoch = std::move(epoch);
    ++impl_->generation;
    impl_->dispatch_sequence = 0;
    impl_->first_presentations.clear();
}
void RuntimeInterpositionController::clear() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->owner_id.clear();
    impl_->epoch.reset();
    ++impl_->generation;
    impl_->dispatch_sequence = 0;
    impl_->first_presentations.clear();
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
    std::vector<FirstPresentation> known_presentations;
    ContextEpoch epoch = [&] {
        std::lock_guard lock(impl->mutex);
        if (!impl->epoch) {
            throw std::runtime_error("Strict provider dispatch requires an active context epoch and assembly receipt");
        }
        owner_id = impl->owner_id;
        generation = impl->generation;
        hooks = impl->hooks;
        known_presentations = impl->first_presentations;
        return *impl->epoch;
    }();
    // Refuse a turn the graph already advanced before anything is assembled.
    if (const auto* presented = presented_messages(request)) {
        auto verdict = judge_presentation(known_presentations, identity.node_name, *presented,
                                          host_instructions, trusted_supplemental);
        if (verdict.advanced) throw std::logic_error(kHostSteppedViolation);
        if (verdict.first) {
            std::lock_guard lock(impl->mutex);
            if (impl->generation == generation &&
                impl->first_presentations.size() < kMaxFirstPresentations &&
                std::find(impl->first_presentations.begin(), impl->first_presentations.end(),
                          *verdict.first) == impl->first_presentations.end())
                impl->first_presentations.push_back(std::move(*verdict.first));
        }
    }
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
        ProviderDispatchBudget budget{identity.usage, identity.model_token_budget,
            identity.budget_exhausted, identity.budget_cancel_token,
            identity.managed_budget_lease, identity.managed_budget_store,
            graph::managed_provider_effect_id(identity), identity.model_token_budget_bounded};
        claim = reserve_provider_dispatch(turn.request, std::move(budget));
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
        // The sequence restarts with every activation, so a later epoch of the
        // same run needs its own prefix or its first dispatch would reuse the
        // id of the earlier epoch's first dispatch. A run's first epoch keeps
        // the original id format.
        const auto prefix = epoch.sequence() > 1
            ? epoch.run_id() + "-epoch-" + std::to_string(epoch.sequence())
            : epoch.run_id();
        dispatch_id = prefix + "-dispatch-" + std::to_string(++impl->dispatch_sequence);
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
