#pragma once

#include <neograph/provider.h>
#include <neograph/runtime_interposition_controller.h>

#include <memory>
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
        std::vector<ChatMessage> trusted_supplemental = {}) const {
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
