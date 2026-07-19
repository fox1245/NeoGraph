#include <neograph/completion_provider.h>

#include <neograph/async/run_sync.h>

namespace neograph {

CompletionProvider::~CompletionProvider() = default;

asio::awaitable<ChatCompletion>
CompletionProvider::invoke_request(CompletionRequest request) {
    co_return co_await do_invoke(std::move(request));
}

ChatCompletion
CompletionProvider::complete(const CompletionParams& params) {
    auto* token = params.cancel_token ? params.cancel_token.get() : nullptr;
    return async::run_sync(
        do_invoke(CompletionRequest::collect(params)), token);
}

asio::awaitable<ChatCompletion>
CompletionProvider::complete_async(const CompletionParams& params) {
    co_return co_await do_invoke(CompletionRequest::collect(params));
}

ChatCompletion
CompletionProvider::complete_stream(const CompletionParams& params,
                                    const StreamCallback& on_chunk) {
    auto* token = params.cancel_token ? params.cancel_token.get() : nullptr;
    return async::run_sync(
        do_invoke(CompletionRequest::stream(params, on_chunk)), token);
}

asio::awaitable<ChatCompletion>
CompletionProvider::complete_stream_async(
    const CompletionParams& params,
    const StreamCallback& on_chunk) {
    co_return co_await do_invoke(
        CompletionRequest::stream(params, on_chunk));
}

asio::awaitable<ChatCompletion>
CompletionProvider::invoke(const CompletionParams& params,
                           StreamCallback on_chunk) {
    if (on_chunk) {
        co_return co_await do_invoke(
            CompletionRequest::stream(params, std::move(on_chunk)));
    }
    co_return co_await do_invoke(CompletionRequest::collect(params));
}

asio::awaitable<ChatCompletion>
invoke_completion(CompletionProvider& provider, CompletionRequest request) {
    co_return co_await provider.invoke_request(std::move(request));
}

asio::awaitable<ChatCompletion>
invoke_completion(Provider& provider, CompletionRequest request) {
    if (auto* completion_provider =
            dynamic_cast<CompletionProvider*>(&provider)) {
        co_return co_await completion_provider->invoke_request(
            std::move(request));
    }
    if (request.streaming()) {
        auto sink = std::move(request.on_chunk());
        if (!sink) sink = [](const std::string&) {};
        co_return co_await provider.complete_stream_async(
            request.params(), sink);
    }
    co_return co_await provider.complete_async(request.params());
}

} // namespace neograph
