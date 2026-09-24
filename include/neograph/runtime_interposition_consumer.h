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
    asio::awaitable<ChatCompletion> invoke_provider(
        const std::shared_ptr<Provider>& provider,
        CompletionParams params,
        StreamCallback on_chunk = {},
        std::vector<ChatMessage> host_instructions = {},
        std::vector<ChatMessage> trusted_supplemental = {},
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
            co_return co_await broker->invoke(std::move(identity), provider,
                                              std::move(params), std::move(on_chunk));
        }
        if (runtime_interposition_) {
            co_return co_await runtime_interposition_->invoke_async(
                std::move(params), std::move(on_chunk), std::move(host_instructions),
                std::move(trusted_supplemental));
        }
        co_return co_await provider->invoke(params, std::move(on_chunk));
    }

private:
    std::shared_ptr<RuntimeInterpositionController> runtime_interposition_;
};

}  // namespace neograph
