#pragma once

#include <neograph/provider.h>
#include <neograph/graph/provider_call_broker.h>
#include <neograph/runtime_interposition_controller.h>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neograph {

/** Additive opt-in for built-in provider-calling runtime components. */
class NEOGRAPH_API RuntimeInterpositionConsumer {
public:
    virtual ~RuntimeInterpositionConsumer() = default;

    virtual void set_runtime_interposition(
        std::shared_ptr<RuntimeInterpositionController> controller) {
        runtime_interposition_ = std::move(controller);
    }

protected:
    asio::awaitable<sp::runtime::Result> invoke_provider(
        std::shared_ptr<Provider> provider,
        ProviderRequest request,
        std::vector<sp::Message> host_instructions = {},
        std::vector<sp::Message> trusted_supplemental = {},
        std::shared_ptr<graph::ProviderCallBroker> broker = {},
        graph::ProviderCallIdentity identity = {}) const {
        // Both mechanisms claim the full dispatch boundary. Bypassing either
        // silently would discard a host policy or a durable effect receipt.
        if (broker && runtime_interposition_) {
            throw std::logic_error("Provider broker conflicts with Strict Runtime interposition");
        }
        if (broker) {
            if (identity.thread_id.empty() || identity.task_id.empty() ||
                identity.node_name.empty()) {
                throw std::invalid_argument("Provider broker requires a Core task identity");
            }
            return broker->invoke(std::move(identity), std::move(provider),
                                  std::move(request));
        }
        if (runtime_interposition_) {
            return runtime_interposition_->invoke_async(
                std::move(request), std::move(host_instructions),
                std::move(trusted_supplemental), std::move(identity));
        }
        if (identity.model_token_budget == 0 && !identity.managed_budget_lease) {
            return provider->invoke_async(std::move(request));
        }
        return [](std::shared_ptr<Provider> provider, ProviderRequest request,
                  graph::ProviderCallIdentity identity)
            -> asio::awaitable<sp::runtime::Result> {
            auto prepared = provider->prepare(std::move(request));
            if (const auto* error = prepared.error()) {
                co_return std::make_shared<const sp::Outcome>(sp::Failure{*error, {}});
            }
            ProviderBudgetClaim claim;
            try {
                claim = reserve_provider_dispatch(prepared,
                    ProviderDispatchBudget{identity.usage, identity.model_token_budget,
                        identity.budget_exhausted, identity.budget_cancel_token,
                        identity.managed_budget_lease, identity.managed_budget_store});
            } catch (const ProviderFailure& failure) {
                co_return failure.outcome();
            }
            co_await claim.begin_managed_effect(
                prepared, graph::managed_provider_effect_id(identity));
            claim.mark_dispatched();
            sp::runtime::Result outcome;
            std::exception_ptr dispatch_error;
            try {
                outcome = co_await provider->dispatch_async(std::move(prepared));
            } catch (const ProviderOutcomeError& error) {
                outcome = error.outcome();
                dispatch_error = std::current_exception();
            } catch (const ProviderFailure& error) {
                outcome = error.outcome();
                dispatch_error = std::current_exception();
            }
            try {
                co_await claim.settle_managed(outcome, dispatch_error);
            } catch (const ProviderOutcomeError&) {
                throw;
            } catch (...) {
                if (!outcome) throw;
                throw ProviderBudgetSettlementError(outcome, std::current_exception(), dispatch_error);
            }
            if (dispatch_error) std::rethrow_exception(dispatch_error);
            co_return outcome;
        }(std::move(provider), std::move(request), std::move(identity));
    }

private:
    std::shared_ptr<RuntimeInterpositionController> runtime_interposition_;
};

}  // namespace neograph
