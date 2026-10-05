#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#if __has_include("runtime/testing.h")
#include <runtime/testing.h>
#endif
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/execution/outstanding_work.hpp>
#include <asio/post.hpp>
#include <asio/prefer.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <deque>
#include <map>
#include <stop_token>

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;

namespace {
void socket_cancel(ProviderMode mode) {
    wire::Peer peer(wire::chat_sse(), true);
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request("openai.chat", mode);
    auto token = std::make_shared<graph::CancelToken>();
    request.cancel_token = token;
    asio::io_context io;
    auto result = asio::co_spawn(io, provider->invoke_async(std::move(request)), asio::use_future);
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(1);
    token->cancel();
    const auto status = result.wait_for(1s);
    peer.release();
    runner.join();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    const auto outcome = result.get();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::Cancelled);
    EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
    const auto& partial = wire::failure(outcome).partial;
    EXPECT_TRUE(partial.messages.empty());
    EXPECT_TRUE(partial.raw_events.empty());
    EXPECT_EQ(partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(partial.usage.input_total);
    EXPECT_FALSE(partial.usage.output_total);
    EXPECT_FALSE(partial.usage.total);
    EXPECT_FALSE(partial.usage.provider_reported_total);
}

void raw_socket_cancel(ProviderMode mode) {
    wire::Peer peer(mode == ProviderMode::Stream ? wire::chat_sse() : wire::chat_response(),
                    mode == ProviderMode::Stream);
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request("openai.chat", mode);
    asio::cancellation_signal cancellation;
    asio::io_context io;
    bool cancellation_preserved = false;
    auto body = [&]() -> asio::awaitable<sp::runtime::Result> {
        auto outcome = co_await provider->invoke_async(std::move(request));
        const auto state = co_await asio::this_coro::cancellation_state;
        cancellation_preserved = state.cancelled() != asio::cancellation_type::none;
        co_return outcome;
    };
    auto result = asio::co_spawn(io, body(),
        asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(1);
    asio::post(io, [&] { cancellation.emit(asio::cancellation_type::terminal); });
    const auto status = result.wait_for(2s);
    peer.release();
    if (status != std::future_status::ready) io.stop();
    runner.join();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    const auto outcome = result.get();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::Cancelled);
    EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
    EXPECT_TRUE(wire::failure(outcome).partial.messages.empty());
    EXPECT_EQ(wire::failure(outcome).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_TRUE(cancellation_preserved);
}

void shared_socket_cancel(bool bind_parent_slot) {
    wire::Peer peer(wire::chat_sse(), true);
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto token = std::make_shared<graph::CancelToken>();
    asio::io_context io;
    std::future<void> parent_operation;
    bool parent_wait_cancelled = false;
    auto parent_body = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_at(std::chrono::steady_clock::time_point::max());
        asio::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        parent_wait_cancelled = ec == asio::error::operation_aborted;
    };
    if (bind_parent_slot) {
        const auto bound = token->bind_executor(io.get_executor());
        asio::post(bound, [&, bound] {
            parent_operation = asio::co_spawn(bound, parent_body(),
                asio::bind_cancellation_slot(token->slot(), asio::use_future));
        });
    }
    std::vector<std::future<sp::runtime::Result>> results;
    for (const auto mode : {ProviderMode::Collect, ProviderMode::Stream, ProviderMode::Stream}) {
        auto request = wire::request("openai.chat", mode);
        request.cancel_token = token;
        results.push_back(asio::co_spawn(io, provider->invoke_async(std::move(request)),
                                         asio::use_future));
    }
    // Establish the independent parent wait before either SDK call can finish.
    io.poll();
    io.restart();
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(3);
    token->cancel();
    std::vector<std::future_status> statuses;
    for (auto& result : results) statuses.push_back(result.wait_for(2s));
    const auto parent_status = bind_parent_slot ? parent_operation.wait_for(2s)
                                               : std::future_status::ready;
    peer.release();
    if (parent_status != std::future_status::ready ||
        std::any_of(statuses.begin(), statuses.end(), [](auto status) {
            return status != std::future_status::ready;
        })) io.stop();
    runner.join();
    token->unbind_executor();
    ASSERT_TRUE(entered);
    ASSERT_EQ(parent_status, std::future_status::ready);
    if (bind_parent_slot) {
        parent_operation.get();
        EXPECT_TRUE(parent_wait_cancelled);
    }
    for (std::size_t i = 0; i != results.size(); ++i) {
        ASSERT_EQ(statuses[i], std::future_status::ready);
        const auto outcome = results[i].get();
        ASSERT_TRUE(outcome);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
        EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::Cancelled);
        EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
        EXPECT_TRUE(wire::failure(outcome).partial.messages.empty());
    }
}

#if __has_include("runtime/testing.h")
// Control only SDK scheduling and transport bytes; request admission, codecs,
// accumulation, borrowed event views and owned outcomes are the real runtime.
class ManualSdkExecutor final : public sp::runtime::detail::Executor {
public:
    void post(Task task) override { ready_.push_back(std::move(task)); }
    Timer schedule(sp::runtime::SteadyTime, Task task) override {
        const auto id = ++last_timer_;
        timers_.emplace(id, std::move(task));
        return id;
    }
    void cancel(Timer id) noexcept override { timers_.erase(id); }
    sp::runtime::SteadyTime now() const noexcept override { return now_; }
    sp::runtime::WallTime wall_now() const noexcept override { return wall_; }
    bool in_thread() const noexcept override { return active_; }
    void shutdown() noexcept override { ready_.clear(); timers_.clear(); }
    void drain() {
        while (!ready_.empty()) {
            auto task = std::move(ready_.front());
            ready_.pop_front();
            struct Active {
                bool& flag;
                explicit Active(bool& value) : flag(value) { flag = true; }
                ~Active() { flag = false; }
            } active(active_);
            task();
        }
    }
private:
    const sp::runtime::SteadyTime now_ = std::chrono::steady_clock::now();
    const sp::runtime::WallTime wall_ = std::chrono::system_clock::now();
    std::deque<Task> ready_;
    std::map<Timer, Task> timers_;
    Timer last_timer_ = 0;
    bool active_ = false;
};

class ControlledSdkTransport final : public sp::runtime::detail::AttemptTransport {
public:
    struct Call {
        sp::transport::Callbacks callbacks;
        sp::transport::Result result;
        std::deque<std::string> chunks;
        bool done = false, ending = false;
        void head(bool streaming) {
            result.http_status = 200;
            result.attempt.response_head_seen = true;
            result.attempt.reached = sp::transport::Stage::ResponseStarted;
            result.attempt.version = sp::transport::ResponseVersion::Http1_1;
            sp::transport::ResponseHead head;
            head.status = 200;
            head.version = sp::transport::ResponseVersion::Http1_1;
            head.framing = sp::transport::BodyFraming::Chunked;
            head.headers.push_back({"content-type", streaming ? "text/event-stream" : "application/json"});
            callbacks.on_head(head);
        }
        void flush() {
            if (done) return;
            while (!chunks.empty()) {
                if (!callbacks.on_body(chunks.front())) return;
                chunks.pop_front();
            }
            if (ending) finish(sp::transport::Status::Completed);
        }
        void send(std::string bytes, bool final = false) {
            if (done) throw std::logic_error("controlled attempt already ended");
            chunks.push_back(std::move(bytes));
            ending = final;
            flush();
        }
        void finish(sp::transport::Status status) noexcept {
            if (done) return;
            done = true;
            chunks.clear();
            result.status = status;
            auto owned = std::move(callbacks);
            owned.on_done(result);
        }
    };
    class Attempt final : public sp::runtime::detail::Attempt {
    public:
        explicit Attempt(std::shared_ptr<Call> call) : call_(std::move(call)) {}
        void cancel() noexcept override { call_->finish(sp::transport::Status::Cancelled); }
        void resume() noexcept override { call_->flush(); }
    private:
        std::shared_ptr<Call> call_;
    };
    std::unique_ptr<sp::runtime::detail::Attempt> start(
        sp::transport::HttpRequest request, sp::transport::Callbacks callbacks) override {
        auto call = std::make_shared<Call>();
        call->callbacks = std::move(callbacks);
        call->result.attempt.reached = sp::transport::Stage::RequestStarted;
        call->result.attempt.request_body_bytes = static_cast<std::int64_t>(request.body.size());
        calls.push_back(call);
        return std::make_unique<Attempt>(std::move(call));
    }
    void shutdown() noexcept override {
        for (const auto& call : calls) call->finish(sp::transport::Status::Cancelled);
    }
    std::vector<std::shared_ptr<Call>> calls;
};

struct ControlledBridge {
    std::shared_ptr<ManualSdkExecutor> executor = std::make_shared<ManualSdkExecutor>();
    std::shared_ptr<ControlledSdkTransport> transport = std::make_shared<ControlledSdkTransport>();
    std::shared_ptr<sp::runtime::Client> client;
    std::unique_ptr<wire::RuntimeProvider> provider;
    std::unique_ptr<asio::io_context> io = std::make_unique<asio::io_context>();
    ControlledBridge() {
        sp::runtime::Options options;
        options.default_timeout = 5s;
        options.retry_tokens = 0;
        options.retry_tokens_per_second = 0;
        options.limits.max_operations = 1;
        client = std::make_shared<sp::runtime::Client>(sp::runtime::detail::ClientAccess::make(
            test::descriptor("openai.chat"), options, executor, transport));
        provider = std::make_unique<wire::RuntimeProvider>(client, "openai.chat");
    }
    ~ControlledBridge() {
        // Destroying the awaiting frame cancels without joining. Deliver that
        // actual SDK cancellation before its private executor/client teardown.
        io.reset();
        executor->drain();
        provider.reset();
        client.reset();
    }
    std::future<sp::runtime::Result> start(ProviderRequest request) {
        auto result = asio::co_spawn(*io, provider->invoke_async(std::move(request)), asio::use_future);
        io->poll();
        executor->drain();
        return result;
    }
    void resume() {
        io->restart();
        io->run_for(2s);
    }
};
#endif
}

TEST(SchemaProviderAsync, CancelTokenAbortsBufferedSocket) { socket_cancel(ProviderMode::Collect); }
TEST(SchemaProviderAsync, CancelTokenAbortsHttpSseSocket) { socket_cancel(ProviderMode::Stream); }

TEST(SchemaProviderAsync, RawCoroutineCancellationAbortsBufferedSocketAndRemainsVisible) {
    raw_socket_cancel(ProviderMode::Collect);
}

TEST(SchemaProviderAsync, RawCoroutineCancellationAbortsHttpSseSocketAndRemainsVisible) {
    raw_socket_cancel(ProviderMode::Stream);
}

TEST(SchemaProviderAsync, UnboundSharedTokenCancelsEveryConcurrentSdkOperation) {
    shared_socket_cancel(false);
}

TEST(SchemaProviderAsync, SharedTokenCancelsSdkOperationsWithoutReplacingParentSlot) {
    shared_socket_cancel(true);
}

TEST(SchemaProviderAsync, SignalCallbackMayRebindWithoutDeliveringOldQueuedEmit) {
    asio::io_context io;
    auto token = std::make_shared<graph::CancelToken>();
    const auto first = token->bind_executor(io.get_executor());
    asio::any_io_executor rebound;
    int signal_calls = 0;
    int native_cancellations = 0;
    std::stop_callback native_stop(token->stop_token(), [&] { ++native_cancellations; });
    asio::post(first, [&] {
        token->slot().assign([&](asio::cancellation_type) {
            ++signal_calls;
            if (signal_calls == 1) rebound = token->bind_executor(io.get_executor());
        });
    });
    io.poll();
    io.restart();
    token->cancel();
    // Leave the first binding's emit queued. Only the new binding and the
    // callback's reentrant binding may deliver their eager cancellation.
    const auto second = token->bind_executor(io.get_executor());
    io.run();
    token->unbind_executor();
    EXPECT_EQ(signal_calls, 2);
    EXPECT_TRUE(rebound);
    EXPECT_TRUE(token->is_cancelled());
    EXPECT_EQ(native_cancellations, 1);
}

TEST(SchemaProviderAsync, CompletedParentUnbindReleasesWorkBeforeRetainedTokenTeardown) {
    auto token = std::make_shared<graph::CancelToken>();
    int native_cancellations = 0;
    std::stop_callback native_stop(token->stop_token(), [&] { ++native_cancellations; });
    {
        asio::io_context io;
        auto bound = token->bind_executor(
            asio::prefer(io.get_executor(), asio::execution::outstanding_work.tracked));
        std::future<void> parent_operation;
        auto parent_body = []() -> asio::awaitable<void> { co_return; };
        asio::post(bound, [&] {
            parent_operation = asio::co_spawn(bound, parent_body(),
                asio::bind_cancellation_slot(token->slot(), asio::use_future));
        });
        io.poll();
        ASSERT_EQ(parent_operation.wait_for(0s), std::future_status::ready);
        parent_operation.get();
        bound = {};
        // The completed parent's installed handler still owns tracked work.
        // unbind must release that ownership, not merely the token's ex_.
        EXPECT_FALSE(io.stopped());
        token->unbind_executor();
        io.poll();
        EXPECT_TRUE(io.stopped())
            << "a completed parent handler must not keep its context running";
        EXPECT_FALSE(token->is_cancelled());
        EXPECT_EQ(native_cancellations, 0);
    }
    // Retaining the token past its context remains safe for native consumers.
    token->cancel();
    EXPECT_TRUE(token->is_cancelled());
    EXPECT_EQ(native_cancellations, 1);
    token->cancel();
    EXPECT_EQ(native_cancellations, 1);
}

#if __has_include("runtime/testing.h")
TEST(SchemaProviderAsync, CompletionPublishedWhileDrainingPreservesWakeAndEventFifo) {
    ControlledBridge bridge;
    std::vector<std::string> deltas;
    bool published_completion = false;
    auto request = wire::request("openai.chat", ProviderMode::Stream);
    request.on_event = [&](const sp::Event& event) {
        const auto* delta = std::get_if<sp::PartDelta>(&event);
        if (!delta || delta->payload.kind != sp::PartKind::Text) return;
        deltas.emplace_back(delta->payload.bytes);
        if (published_completion) return;
        published_completion = true;
        // The bridge has already reset/drained its first notification and
        // snapshotted a missing outcome. Publish the rest before it registers
        // its next wait: a transient notification would strand this call.
        bridge.transport->calls.front()->send(wire::chat_sse("second"), true);
        bridge.executor->drain();
    };
    auto result = bridge.start(std::move(request));
    ASSERT_EQ(bridge.transport->calls.size(), 1u);
    auto& call = *bridge.transport->calls.front();
    call.head(true);
    call.send(wire::chat_frame(json{{"choices", json::array({{{"index", 0},
        {"delta", {{"role", "assistant"}, {"content", "first"}}},
        {"finish_reason", nullptr}}})}}));
    bridge.executor->drain();
    bridge.resume();
    ASSERT_EQ(result.wait_for(0s), std::future_status::ready);
    const auto outcome = result.get();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*outcome));
    EXPECT_TRUE(published_completion);
    EXPECT_EQ(deltas, (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(test::text(outcome), "firstsecond");
    const auto& usage = test::completion(outcome).usage;
    EXPECT_EQ(usage.stage, sp::UsageStage::Final);
    ASSERT_TRUE(usage.total);
    EXPECT_EQ(usage.total->value, 5u);
}

TEST(SchemaProviderAsync, ObserverThrowAfterProducerCompletionRetainsAuthoritativeOwnedOutcome) {
    sp::runtime::Result retained;
    const auto cause = std::make_exception_ptr(std::domain_error("fixture observer rejection"));
    {
        ControlledBridge bridge;
        std::vector<std::string> deltas;
        auto request = wire::request("openai.chat", ProviderMode::Stream);
        request.on_event = [&](const sp::Event& event) {
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (!delta || delta->payload.kind != sp::PartKind::Text) return;
            deltas.emplace_back(delta->payload.bytes);
            std::rethrow_exception(cause);
        };
        auto result = bridge.start(std::move(request));
        ASSERT_EQ(bridge.transport->calls.size(), 1u);
        auto& call = *bridge.transport->calls.front();
        call.head(true);
        call.send(wire::chat_sse("owned completed response"), true);
        bridge.executor->drain();
        // Reclaiming the sole admission slot proves on_outcome returned before
        // the consumer resumes; observer cancellation must not replace it.
        { auto next = bridge.client->prepare(wire::request().payload); ASSERT_TRUE(next.valid()); }
        bridge.resume();
        ASSERT_EQ(result.wait_for(0s), std::future_status::ready);
        try {
            (void)result.get();
            FAIL() << "throwing observer must surface its retained outcome";
        } catch (const ProviderObserverError& error) {
            EXPECT_EQ(error.error_kind(), sp::ErrorKind::Misuse);
            EXPECT_EQ(error.cause(), cause);
            EXPECT_THROW(std::rethrow_exception(error.cause()), std::domain_error);
            retained = error.outcome();
        }
        EXPECT_EQ(deltas, (std::vector<std::string>{"owned completed response"}));
    }
    ASSERT_TRUE(retained);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*retained));
    EXPECT_EQ(test::text(retained), "owned completed response");
    const auto& completion = test::completion(retained);
    EXPECT_EQ(completion.usage.stage, sp::UsageStage::Final);
    ASSERT_TRUE(completion.usage.input_total);
    ASSERT_TRUE(completion.usage.output_total);
    ASSERT_TRUE(completion.usage.total);
    EXPECT_EQ(completion.usage.input_total->value, 3u);
    EXPECT_EQ(completion.usage.output_total->value, 2u);
    EXPECT_EQ(completion.usage.total->value, 5u);
    EXPECT_TRUE(completion.attempt.request_may_have_left);
    EXPECT_TRUE(completion.attempt.response_head_seen);
    EXPECT_EQ(completion.attempt.attempts, 1u);
}
TEST(SchemaProviderAsync, RepeatedDispatchKeepsOutcomesDistinctAfterObserverFailure) {
    sp::runtime::Result first_owned;
    sp::runtime::Result second_owned;
    {
        ControlledBridge bridge;
        auto first_request = wire::request("openai.chat", ProviderMode::Stream);
        first_request.on_event = [](const sp::Event& event) {
            if (std::holds_alternative<sp::PartDelta>(event))
                throw std::domain_error("first observer rejected");
        };
        auto first = bridge.start(std::move(first_request));
        ASSERT_EQ(bridge.transport->calls.size(), 1u);
        bridge.transport->calls.front()->head(true);
        bridge.transport->calls.front()->send(wire::chat_sse("first owned response"), true);
        bridge.executor->drain();
        bridge.resume();
        ASSERT_EQ(first.wait_for(0s), std::future_status::ready);
        try {
            (void)first.get();
            FAIL() << "first observer failure must retain its completed outcome";
        } catch (const ProviderObserverError& error) {
            first_owned = error.outcome();
            EXPECT_THROW(std::rethrow_exception(error.cause()), std::domain_error);
        }

        bridge.io->restart();
        auto second = bridge.start(wire::request("openai.chat", ProviderMode::Collect));
        ASSERT_EQ(bridge.transport->calls.size(), 2u);
        EXPECT_EQ(second.wait_for(0s), std::future_status::timeout);
        bridge.transport->calls.back()->head(false);
        bridge.transport->calls.back()->send(wire::chat_response("second owned response"), true);
        bridge.executor->drain();
        bridge.resume();
        ASSERT_EQ(second.wait_for(0s), std::future_status::ready);
        second_owned = second.get();
    }
    ASSERT_TRUE(first_owned);
    ASSERT_TRUE(second_owned);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*first_owned));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*second_owned));
    EXPECT_EQ(test::text(first_owned), "first owned response");
    EXPECT_EQ(test::text(second_owned), "second owned response");
    EXPECT_EQ(test::completion(first_owned).usage.stage, sp::UsageStage::Final);
    EXPECT_EQ(test::completion(second_owned).usage.stage, sp::UsageStage::Final);
}
#endif

TEST(SchemaProviderAsync, PerCallDeadlineRemainsTyped) {
    wire::Peer peer(wire::chat_response());
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request();
    request.options.deadline = std::chrono::steady_clock::now() + 100ms;
    auto result = std::async(std::launch::async, [p = provider.get(), owned = std::move(request)]() mutable {
        return p->invoke(std::move(owned));
    });
    const bool entered = peer.await_requests(1);
    const auto status = result.wait_for(1s);
    peer.release();
    const auto outcome = result.get();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::DeadlineExceeded);
    EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
    EXPECT_EQ(wire::failure(outcome).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(wire::failure(outcome).partial.usage.total);
}

TEST(SchemaProviderAsync, PreparedDeadlineIsNotRenewedAtDispatch) {
    wire::Peer peer(wire::chat_response());
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request();
    request.options.deadline = std::chrono::steady_clock::now() + 30ms;
    auto prepared = provider->prepare(std::move(request));
    ASSERT_TRUE(prepared.valid());
    std::this_thread::sleep_for(50ms);
    const auto outcome = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::DeadlineExceeded);
    EXPECT_FALSE(wire::failure(outcome).error.attempt.request_may_have_left);
    EXPECT_EQ(wire::failure(outcome).error.attempt.attempts, 0u);
    EXPECT_EQ(wire::failure(outcome).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(wire::failure(outcome).partial.usage.total);
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->entered, 0u);
}
