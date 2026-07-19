#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/completion_provider.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace neograph;

namespace {

ChatCompletion completion(std::string text) {
    ChatCompletion result;
    result.message.role = "assistant";
    result.message.content = std::move(text);
    return result;
}

class RequestProvider final : public CompletionProvider {
  public:
    int calls = 0;
    CompletionMode last_mode = CompletionMode::COLLECT;
    bool had_callback = false;
    std::string last_model;

    std::string get_name() const override { return "request-provider"; }

  protected:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        ++calls;
        last_mode = request.mode();
        had_callback = static_cast<bool>(request.on_chunk());
        last_model = request.params().model;
        if (request.streaming() && request.on_chunk()) {
            request.on_chunk()("A");
            request.on_chunk()("B");
        }
        co_return completion(request.streaming() ? "stream" : "collect");
    }
};

class LegacyStreamProvider final : public Provider {
  public:
    int complete_calls = 0;
    int stream_calls = 0;

    ChatCompletion complete(const CompletionParams&) override {
        ++complete_calls;
        return completion("legacy-collect");
    }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback& on_chunk) override {
        ++stream_calls;
        on_chunk("legacy-token");
        return completion("legacy-stream");
    }

    std::string get_name() const override { return "legacy-stream"; }
};

class SuspendedRequestProvider final : public CompletionProvider {
  public:
    std::string observed_model;

    std::string get_name() const override { return "suspended-request"; }

  protected:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        observed_model = request.params().model;
        if (request.on_chunk()) request.on_chunk()(observed_model);
        co_return completion(observed_model);
    }
};

} // namespace

TEST(CompletionRequest, NamedFactoriesKeepModeIndependentOfCallback) {
    CompletionParams params;
    params.model = "model-a";

    auto collect = CompletionRequest::collect(params);
    auto stream = CompletionRequest::stream(params);

    EXPECT_FALSE(collect.streaming());
    EXPECT_FALSE(collect.on_chunk());
    EXPECT_TRUE(stream.streaming());
    EXPECT_FALSE(stream.on_chunk());
    EXPECT_EQ(stream.params().model, "model-a");
}

TEST(CompletionProvider, ExplicitStreamWithoutObserverUsesStreamMode) {
    RequestProvider provider;
    CompletionParams params;
    params.model = "stream-model";

    auto result = async::run_sync(provider.invoke_request(
        CompletionRequest::stream(params)));

    EXPECT_EQ(result.message.content, "stream");
    EXPECT_EQ(provider.calls, 1);
    EXPECT_EQ(provider.last_mode, CompletionMode::STREAM);
    EXPECT_FALSE(provider.had_callback);
    EXPECT_EQ(provider.last_model, "stream-model");
}

TEST(CompletionProvider, ExplicitCollectIgnoresStreamingInference) {
    RequestProvider provider;
    CompletionParams params;
    std::vector<std::string> chunks;

    auto result = async::run_sync(provider.invoke_request(
        CompletionRequest::collect(params)));

    EXPECT_EQ(result.message.content, "collect");
    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(provider.calls, 1);
    EXPECT_EQ(provider.last_mode, CompletionMode::COLLECT);
}

TEST(CompletionProvider, ExplicitStreamDeliversOrderedChunks) {
    RequestProvider provider;
    CompletionParams params;
    std::vector<std::string> chunks;

    auto result = async::run_sync(provider.invoke_request(
        CompletionRequest::stream(params, [&](const std::string& chunk) {
            chunks.push_back(chunk);
        })));

    EXPECT_EQ(result.message.content, "stream");
    EXPECT_EQ(chunks, (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(provider.calls, 1);
}

TEST(CompletionProvider, RequestOwnsParamsAndCallbackAcrossSuspension) {
    SuspendedRequestProvider provider;
    std::vector<std::string> chunks;

    auto pending = [&]() {
        CompletionParams params;
        params.model = "owned-after-suspend";
        StreamCallback callback = [&](const std::string& chunk) {
            chunks.push_back(chunk);
        };
        return provider.invoke_request(
            CompletionRequest::stream(std::move(params), std::move(callback)));
    }();

    auto result = async::run_sync(std::move(pending));

    EXPECT_EQ(result.message.content, "owned-after-suspend");
    EXPECT_EQ(provider.observed_model, "owned-after-suspend");
    EXPECT_EQ(chunks, (std::vector<std::string>{"owned-after-suspend"}));
}

TEST(CompletionProvider, EveryLegacyEntryIsAOneHopAdapter) {
    RequestProvider provider;
    CompletionParams params;

    EXPECT_EQ(provider.complete(params).message.content, "collect");
    EXPECT_EQ(async::run_sync(provider.complete_async(params)).message.content,
              "collect");
    EXPECT_EQ(provider.complete_stream(params, {}).message.content, "stream");
    EXPECT_EQ(async::run_sync(provider.complete_stream_async(params, {}))
                  .message.content,
              "stream");
    EXPECT_EQ(async::run_sync(provider.invoke(params, {})).message.content,
              "collect");
    EXPECT_EQ(provider.calls, 5);
}

TEST(CompletionProvider, FreeAdapterPreservesNewProviderRequest) {
    RequestProvider provider;
    Provider& base = provider;
    CompletionParams params;

    auto result = async::run_sync(invoke_completion(
        base, CompletionRequest::stream(params)));

    EXPECT_EQ(result.message.content, "stream");
    EXPECT_EQ(provider.last_mode, CompletionMode::STREAM);
    EXPECT_FALSE(provider.had_callback);
}

TEST(CompletionProvider, FreeAdapterPreservesLegacyStreamWithoutObserver) {
    LegacyStreamProvider provider;
    Provider& base = provider;
    CompletionParams params;

    auto result = async::run_sync(invoke_completion(
        base, CompletionRequest::stream(params)));

    EXPECT_EQ(result.message.content, "legacy-stream");
    EXPECT_EQ(provider.stream_calls, 1);
    EXPECT_EQ(provider.complete_calls, 0);
}

TEST(CompletionProvider, LegacyAsyncStreamAcceptsEmptyCallback) {
    LegacyStreamProvider provider;
    CompletionParams params;

    EXPECT_NO_THROW({
        auto result = async::run_sync(
            provider.complete_stream_async(params, StreamCallback{}));
        EXPECT_EQ(result.message.content, "legacy-stream");
    });
}
