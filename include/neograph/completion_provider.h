/**
 * @file completion_provider.h
 * @brief Additive, explicit completion request API for new providers.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <utility>

namespace neograph {

enum class CompletionMode : std::uint8_t {
    COLLECT,
    STREAM,
};

/**
 * @brief Owns one completion request and explicitly selects its transport mode.
 *
 * Use the named factories instead of inferring streaming from callback presence.
 * A STREAM request with no callback still selects the provider's streaming
 * transport and returns the fully assembled completion.
 */
class NEOGRAPH_API CompletionRequest {
  public:
    static CompletionRequest collect(CompletionParams params) {
        return CompletionRequest(
            std::move(params), CompletionMode::COLLECT, {});
    }

    static CompletionRequest stream(CompletionParams params,
                                    StreamCallback on_chunk = {}) {
        return CompletionRequest(
            std::move(params), CompletionMode::STREAM, std::move(on_chunk));
    }

    CompletionMode mode() const noexcept { return mode_; }
    bool streaming() const noexcept { return mode_ == CompletionMode::STREAM; }

    CompletionParams& params() noexcept { return params_; }
    const CompletionParams& params() const noexcept { return params_; }

    StreamCallback& on_chunk() noexcept { return on_chunk_; }
    const StreamCallback& on_chunk() const noexcept { return on_chunk_; }

  private:
    CompletionRequest(CompletionParams params, CompletionMode mode,
                      StreamCallback on_chunk)
        : params_(std::move(params))
        , mode_(mode)
        , on_chunk_(std::move(on_chunk)) {}

    CompletionParams params_;
    CompletionMode mode_;
    StreamCallback on_chunk_;
};

/**
 * @brief Recommended base class for new Provider implementations.
 *
 * Implement only do_invoke() and get_name(). The existing Provider entry
 * points remain final adapters so old engine code and direct callers retain
 * their behavior while new code can select STREAM independently of callback
 * presence through invoke_request().
 *
 * This is a separate derived interface: Provider's object layout and vtable
 * are unchanged, and existing Provider subclasses require no source changes.
 */
class NEOGRAPH_API CompletionProvider : public Provider {
  public:
    ~CompletionProvider() override;

    asio::awaitable<ChatCompletion>
    invoke_request(CompletionRequest request);

    [[deprecated("use invoke_request(CompletionRequest::collect(...))")]]
    ChatCompletion complete(const CompletionParams& params) final override;

    [[deprecated("use invoke_request(CompletionRequest::collect(...))")]]
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) final override;

    [[deprecated("use invoke_request(CompletionRequest::stream(...))")]]
    ChatCompletion complete_stream(
        const CompletionParams& params,
        const StreamCallback& on_chunk) final override;

    [[deprecated("use invoke_request(CompletionRequest::stream(...))")]]
    asio::awaitable<ChatCompletion>
    complete_stream_async(
        const CompletionParams& params,
        const StreamCallback& on_chunk) final override;

    [[deprecated("use invoke_request() with an explicit CompletionRequest")]]
    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params,
           StreamCallback on_chunk = nullptr) final override;

  protected:
    virtual asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) = 0;
};

/**
 * @brief Invoke a new provider while preserving the explicit request mode.
 */
NEOGRAPH_API asio::awaitable<ChatCompletion>
invoke_completion(CompletionProvider& provider, CompletionRequest request);

/**
 * @brief Adapt an arbitrary legacy Provider to an explicit request.
 *
 * STREAM with no observer still takes the legacy streaming path. The legacy
 * bridge remains subject to its documented lifetime limitations; new native
 * providers should derive from CompletionProvider instead.
 */
NEOGRAPH_API asio::awaitable<ChatCompletion>
invoke_completion(Provider& provider, CompletionRequest request);

} // namespace neograph
