#include <gtest/gtest.h>

#include <neograph/artifact_provider.h>
#include <neograph/async/run_sync.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;

namespace {

ArtifactOperation running_operation(std::string id = "operation-1") {
    ArtifactOperation operation;
    operation.id = std::move(id);
    operation.status = ArtifactOperationStatus::Running;
    return operation;
}

ArtifactOperation succeeded_operation(std::string id = "operation-1") {
    ArtifactOperation operation;
    operation.id = std::move(id);
    operation.status = ArtifactOperationStatus::Succeeded;
    operation.artifacts.push_back(Artifact{
        .identity = "image-1",
        .uri = "artifact://image-1",
        .media_type = "image/png",
        .size_bytes = 42,
        .metadata = json{{"width", 512}, {"height", 512}},
    });
    return operation;
}

class RecordingArtifactProvider final : public ArtifactProvider {
  public:
    ArtifactExecutionMode seen_mode = ArtifactExecutionMode::OneShot;
    std::string seen_operation;
    int start_calls = 0;
    int poll_calls = 0;
    bool always_running = false;
    bool suspend_start = false;

    std::string get_name() const override { return "recording-artifact"; }

  protected:
    asio::awaitable<ArtifactOperation>
    do_start(ArtifactRequest request, ArtifactEventCallback on_event) override {
        ++start_calls;
        seen_mode = request.mode;
        if (suspend_start) {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        seen_operation = request.operation;
        if (request.mode == ArtifactExecutionMode::Stream && on_event) {
            on_event(ArtifactEvent{.kind = ArtifactEventKind::Progress,
                                   .operation_id = "operation-1",
                                   .sequence = 1,
                                   .data = json{{"percent", 50}}});
            on_event(ArtifactEvent{.kind = ArtifactEventKind::Artifact,
                                   .operation_id = "operation-1",
                                   .sequence = 2,
                                   .artifact = Artifact{
                                       .identity = "preview-1",
                                       .uri = "artifact://preview-1",
                                       .media_type = "image/png",
                                       .size_bytes = 1,
                                       .metadata = json::object()},
                                   .data = json::object()});
        }
        if (request.mode == ArtifactExecutionMode::LongRunning) {
            co_return running_operation();
        }
        co_return succeeded_operation();
    }

    asio::awaitable<ArtifactOperation>
    do_poll(ArtifactOperation operation) override {
        ++poll_calls;
        if (always_running || poll_calls == 1) {
            co_return running_operation(std::move(operation.id));
        }
        co_return succeeded_operation(std::move(operation.id));
    }
};

class SlowPollingArtifactProvider final : public ArtifactProvider {
  public:
    std::atomic<bool> started = false;
    std::atomic<int> polls = 0;

    std::string get_name() const override { return "slow-artifact"; }

  protected:
    asio::awaitable<ArtifactOperation>
    do_start(ArtifactRequest, ArtifactEventCallback) override {
        started.store(true, std::memory_order_release);
        co_return running_operation("slow-operation");
    }

    asio::awaitable<ArtifactOperation>
    do_poll(ArtifactOperation operation) override {
        ++polls;
        co_return running_operation(std::move(operation.id));
    }
};

} // namespace

TEST(ArtifactRequest, OwnsRequestAcrossSuspensionAndKeepsExplicitMode) {
    RecordingArtifactProvider provider;
    provider.suspend_start = true;

    auto pending = [&] {
        ArtifactRequest request;
        request.operation = "generate-image";
        request.input = json{{"prompt", "sunrise"}};
        request.mode = ArtifactExecutionMode::OneShot;
        return provider.execute_async(std::move(request));
    }();

    const auto result = async::run_sync(std::move(pending));

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(provider.start_calls, 1);
    EXPECT_EQ(provider.poll_calls, 0);
    EXPECT_EQ(provider.seen_mode, ArtifactExecutionMode::OneShot);
    EXPECT_EQ(provider.seen_operation, "generate-image");
}

TEST(ArtifactProvider, StreamModeDeliversOrderedProviderEventsWithoutPolling) {
    RecordingArtifactProvider provider;
    ArtifactRequest request;
    request.operation = "generate-image";
    request.mode = ArtifactExecutionMode::Stream;
    std::vector<ArtifactEventKind> kinds;
    std::vector<std::uint64_t> sequences;

    const auto result = provider.execute(std::move(request), [&](const ArtifactEvent& event) {
        kinds.push_back(event.kind);
        sequences.push_back(event.sequence);
    });

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(provider.start_calls, 1);
    EXPECT_EQ(provider.poll_calls, 0);
    EXPECT_EQ(provider.seen_mode, ArtifactExecutionMode::Stream);
    EXPECT_EQ(kinds, (std::vector<ArtifactEventKind>{ArtifactEventKind::Progress,
                                                     ArtifactEventKind::Artifact}));
    EXPECT_EQ(sequences, (std::vector<std::uint64_t>{1, 2}));
}

TEST(ArtifactProvider, LongRunningModePollsUntilTerminalResult) {
    RecordingArtifactProvider provider;
    ArtifactRequest request;
    request.operation = "generate-video";
    request.mode = ArtifactExecutionMode::LongRunning;
    request.poll.max_attempts = 3;
    request.poll.interval = std::chrono::milliseconds(1);

    const auto result = provider.execute(std::move(request));

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(result.id, "operation-1");
    EXPECT_EQ(result.artifacts.size(), 1U);
    EXPECT_EQ(provider.start_calls, 1);
    EXPECT_EQ(provider.poll_calls, 2);
}

TEST(ArtifactProvider, LongRunningModeReturnsTimedOutAfterExactPollBound) {
    RecordingArtifactProvider provider;
    provider.always_running = true;
    ArtifactRequest request;
    request.operation = "generate-video";
    request.mode = ArtifactExecutionMode::LongRunning;
    request.poll.max_attempts = 2;
    request.poll.interval = std::chrono::milliseconds(1);

    const auto result = provider.execute(std::move(request));

    EXPECT_EQ(result.status, ArtifactOperationStatus::TimedOut);
    EXPECT_EQ(result.error_code, "POLL_LIMIT_EXCEEDED");
    EXPECT_EQ(provider.start_calls, 1);
    EXPECT_EQ(provider.poll_calls, 2);
}

TEST(ArtifactProvider, CancellationInterruptsBoundedPollWait) {
    SlowPollingArtifactProvider provider;
    auto cancel = std::make_shared<graph::CancelToken>();
    ArtifactRequest request;
    request.operation = "generate-video";
    request.mode = ArtifactExecutionMode::LongRunning;
    request.poll.max_attempts = 100;
    request.poll.interval = std::chrono::seconds(10);
    request.cancel_token = cancel;

    std::jthread canceller([&] {
        while (!provider.started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        cancel->cancel();
    });

    EXPECT_THROW(provider.execute(std::move(request)), graph::CancelledException);
    EXPECT_EQ(provider.polls.load(), 0);
}

TEST(ArtifactValues, CanonicalRoundTripAndMalformedRequestAreRejected) {
    const Artifact expected{
        .identity = "artifact-1",
        .uri = "artifact://artifact-1",
        .media_type = "application/json",
        .size_bytes = 7,
        .metadata = json{{"source", "test"}},
    };
    json encoded;
    to_json(encoded, expected);
    Artifact restored;
    from_json(encoded, restored);

    EXPECT_EQ(restored, expected);

    RecordingArtifactProvider provider;
    ArtifactRequest invalid;
    invalid.operation = "generate-image";
    invalid.mode = ArtifactExecutionMode::LongRunning;
    invalid.poll.max_attempts = 0;
    EXPECT_THROW(provider.execute(std::move(invalid)), std::invalid_argument);
    EXPECT_EQ(provider.start_calls, 0);
}
