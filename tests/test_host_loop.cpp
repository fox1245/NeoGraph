// The engine on a host's event loop (#535): async::HostLoop pumps an io_context from tasks the host runs, and a
// SchemaProvider built over the SDK's host seam sends through a transport the host supplies. The host here is a
// manual executor that the test runs from its own thread, as a JavaScript host runs its microtasks, so nothing
// waits and the loop's work runs on that thread only. The last tests hand work to the loop from other threads, as a
// blocking tool's completion does (#602). No network, no libcurl, no key.
#include <gtest/gtest.h>

#include <neograph/async/host_loop.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/tool.h>

#include "../src/core/host_timer.h"
#include "fixtures/manual_executor.h"

#include <descriptor/descriptor.h>
#include <runtime/host.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/strand.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace std::chrono_literals;

namespace {

using host_test::ManualExecutor;

// The host's HTTP: each request is answered by tasks on the host executor, in the chunks given. `hold` keeps the
// answer back, as a request in flight does; a cancelled attempt ends with one Cancelled completion from a later task.
class FakeTransport final : public sp::runtime::HostTransport {
    struct Call {
        sp::transport::Callbacks callbacks;
        std::vector<std::string> chunks;
        std::string content_type;
        std::size_t next = 0;
        bool cancelled = false, finished = false, scheduled = false, head_sent = false;
    };
    struct Handle final : sp::runtime::HostAttempt {
        FakeTransport* owner;
        std::shared_ptr<Call> call;
        Handle(FakeTransport* o, std::shared_ptr<Call> c) : owner(o), call(std::move(c)) {}
        void cancel() noexcept override {
            ++owner->cancels;
            call->cancelled = true;
            owner->executor_->post([call = call] { FakeTransport::end(*call, sp::transport::Status::Cancelled); });
        }
        void resume() noexcept override {}
    };
    static void end(Call& call, sp::transport::Status status) {
        if (call.finished) return;
        call.finished = true;
        sp::transport::Result result;
        result.status = status;
        result.attempt.reached = sp::transport::Stage::RequestStarted;
        if (status == sp::transport::Status::Completed) {
            result.attempt.reached = sp::transport::Stage::ResponseStarted;
            result.attempt.response_head_seen = true;
            result.http_status = 200;
        }
        call.callbacks.on_done(result);
    }

public:
    FakeTransport(std::shared_ptr<ManualExecutor> executor, std::string content_type, std::vector<std::string> chunks)
        : executor_(std::move(executor)), content_type_(std::move(content_type)), chunks_(std::move(chunks)) {}
    std::unique_ptr<sp::runtime::HostAttempt> start(sp::transport::HttpRequest request,
                                                     sp::transport::Callbacks callbacks) override {
        started_on = std::this_thread::get_id();
        last_body = request.body;
        ++requests;
        auto call = std::make_shared<Call>();
        call->callbacks = std::move(callbacks);
        call->chunks = chunks_;
        call->content_type = content_type_;
        if (!hold) schedule(call);
        return std::make_unique<Handle>(this, call);
    }
    void shutdown() noexcept override {}
    bool hold = false;
    int requests = 0, cancels = 0;
    std::thread::id started_on;
    std::string last_body;

private:
    void schedule(const std::shared_ptr<Call>& call) {
        if (call->scheduled) return;
        call->scheduled = true;
        executor_->post([call] {
            if (call->cancelled || call->finished) return;
            if (!call->head_sent) {
                call->head_sent = true;
                sp::transport::ResponseHead head;
                head.status = 200;
                head.version = sp::transport::ResponseVersion::Http1_1;
                head.framing = sp::transport::BodyFraming::ContentLength;
                head.headers.push_back({"content-type", call->content_type});
                call->callbacks.on_head(head);
            }
            while (call->next < call->chunks.size()) {
                if (!call->callbacks.on_body(call->chunks[call->next])) return;
                ++call->next;
            }
            end(*call, sp::transport::Status::Completed);
        });
    }
    std::shared_ptr<ManualExecutor> executor_;
    std::string content_type_;
    std::vector<std::string> chunks_;
};

const char* const buffered_body =
    R"({"id":"c1","model":"fixture-model","created":7,"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"hello from the host"},"finish_reason":"stop"}],"usage":{"prompt_tokens":2,"completion_tokens":4,"total_tokens":6}})";
const std::vector<std::string> sse_body = {
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"hello \"},\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"from the host\"},\"finish_reason\":null}]}\n\n",
    "data: {\"id\":\"s\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture-model\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":4,\"total_tokens\":6}}\n\n",
    "data: [DONE]\n\n"};

const char* const embedding_body =
    R"({"object":"list","model":"fixture-embed","data":[{"object":"embedding","index":0,"embedding":[0.25,-0.5]}],"usage":{"prompt_tokens":3,"total_tokens":3}})";

sp::descriptor::ValidatedDescriptor chat_descriptor(const std::string& origin = "https://api.example.test") {
    auto loaded = sp::descriptor::load(
        R"({"descriptor_version":1,"revision":1,"id":"host-loop","family":"openai.chat","connection":{"base_url":")" +
        origin + R"(","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

// A host: the executor the engine's turns run on, the executor and transport of one provider, and a loop.
struct Host {
    std::shared_ptr<ManualExecutor> loop_executor = std::make_shared<ManualExecutor>();
    std::shared_ptr<ManualExecutor> client_executor = std::make_shared<ManualExecutor>();
    std::shared_ptr<FakeTransport> transport;
    std::unique_ptr<async::HostLoop> loop;
    std::shared_ptr<llm::SchemaProvider> provider;

    explicit Host(std::vector<std::string> chunks = {buffered_body}, const char* content_type = "application/json",
                  const std::string& origin = "https://api.example.test") {
        transport = std::make_shared<FakeTransport>(client_executor, content_type, std::move(chunks));
        loop = std::make_unique<async::HostLoop>(loop_executor);
        provider = std::make_shared<llm::SchemaProvider>(chat_descriptor(origin), sp::runtime::Options::with_api_key("test-key"),
                                                         client_executor, transport);
    }
    // The host's event loop: runs the tasks of both executors until `done` or until a second has passed.
    template <class Done>
    bool run_until(Done done) {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (!done()) {
            if (std::chrono::steady_clock::now() > limit) return false;
            const bool ran = loop_executor->step() | client_executor->step();
            if (!ran) std::this_thread::sleep_for(100us);
        }
        return true;
    }
    std::unique_ptr<graph::GraphEngine> engine() {
        const auto definition = json{
            {"name", "host_loop"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"llm", {{"type", "llm_call"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "llm"}}, {{"from", "llm"}, {"to", "__end__"}}})}};
        graph::NodeContext context;
        context.provider = provider;
        context.model = "fixture-model";
        return graph::GraphEngine::compile(definition, context);
    }
};

graph::RunConfig question() {
    graph::RunConfig config;
    config.thread_id = "host-loop";
    config.provider_messages = std::vector<sp::Message>{portable_message(ChatMessage{"user", "hello"})};
    return config;
}

struct Finished {
    bool done = false;
    std::exception_ptr error;
    graph::RunResult result;
};

asio::awaitable<graph::RunResult> drive(graph::GraphEngine* engine, graph::RunConfig config, bool stream,
                                        graph::GraphStreamCallback events) {
    if (stream) co_return co_await engine->run_stream_async(std::move(config), std::move(events));
    co_return co_await engine->run_async(std::move(config));
}

std::string assistant_text(const graph::RunResult& result) {
    for (auto it = result.native_messages.rbegin(); it != result.native_messages.rend(); ++it) {
        if (it->role != sp::Role::Assistant) continue;
        std::string text;
        for (const auto& part : it->parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
        if (!text.empty()) return text;
    }
    return {};
}

// The reason of the provider failure at the root of a failed run, or empty.
std::string reason_of(std::exception_ptr error) {
    for (int depth = 0; error && depth < 8; ++depth) {
        try {
            std::rethrow_exception(error);
        } catch (const graph::NodeExecutionError& failure) {
            error = failure.cause();
        } catch (const ProviderFailure& failure) {
            return std::get<sp::Failure>(*failure.outcome()).error.reason;
        } catch (...) {
            return {};
        }
    }
    return {};
}

}  // namespace

TEST(HostLoop, AnIdleLoopSchedulesNothing) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    EXPECT_TRUE(loop.idle());
    EXPECT_EQ(executor->pending(), 0u);
    EXPECT_EQ(executor->timers(), 0u);

    bool done = false;
    auto nothing = []() -> asio::awaitable<void> { co_return; };
    loop.spawn(nothing(), [&](std::exception_ptr) { done = true; });
    EXPECT_EQ(executor->pending(), 1u) << "spawning schedules one turn";
    EXPECT_FALSE(loop.idle());
    EXPECT_FALSE(done) << "the work does not run until the host runs its turn";

    while (executor->step()) {}
    EXPECT_TRUE(done);
    EXPECT_EQ(executor->tasks_run, 1u);
    EXPECT_EQ(executor->pending(), 0u);
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, WorkPostedByTheSdkRunsInATurnOfTheHost) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    // A coroutine that waits for something outside the loop: it is resumed by a post from outside, as the SDK's
    // completion resumes the coroutine that awaits a provider call.
    std::function<void()> resume;
    bool done = false;
    auto waiting = [&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        co_await asio::async_initiate<decltype(asio::use_awaitable), void()>(
            [&](auto handler) {
                auto shared = std::make_shared<decltype(handler)>(std::move(handler));
                resume = [ex, shared] { asio::post(ex, [shared] { (*shared)(); }); };
            },
            asio::use_awaitable);
        done = true;
    };
    loop.spawn(waiting(), [](std::exception_ptr) {});
    while (executor->step()) {}
    ASSERT_TRUE(resume);
    EXPECT_FALSE(done);
    EXPECT_TRUE(loop.idle()) << "a loop that waits for the host has nothing scheduled";
    EXPECT_EQ(executor->pending() + executor->timers(), 0u);

    resume();
    EXPECT_EQ(executor->pending(), 1u) << "the post scheduled a turn";
    while (executor->step()) {}
    EXPECT_TRUE(done);
}

TEST(HostLoop, ATurnRunsAtMostItsBatchAndSchedulesAnother) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop::Options options;
    options.turn_handlers = 4;
    async::HostLoop loop(executor, options);
    int yields = 0;
    bool done = false;
    auto yielding = [&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        for (int i = 0; i < 40; ++i) {
            co_await asio::post(ex, asio::use_awaitable);
            ++yields;
        }
    };
    loop.spawn(yielding(), [&](std::exception_ptr) { done = true; });
    std::size_t turns = 0;
    while (executor->step()) ++turns;
    EXPECT_TRUE(done);
    EXPECT_EQ(yields, 40);
    EXPECT_GE(turns, 10u) << "forty yields at four handlers a turn";
}

TEST(HostLoop, AnAsioTimerNotedWithTheLoopFiresOnTheHostsTimer) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    bool done = false;
    auto sleeping = []() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(30ms);
        async::note_deadline(ex, timer.expiry());
        co_await timer.async_wait(asio::use_awaitable);
    };
    loop.spawn(sleeping(), [&](std::exception_ptr error) { done = !error; });
    while (executor->step()) {}
    EXPECT_FALSE(done);
    EXPECT_EQ(executor->timers(), 1u) << "one host timer for the deadline, nothing else scheduled";
    EXPECT_EQ(executor->pending(), 0u);
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < limit) {
        if (!executor->step()) std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_TRUE(loop.idle());
}

// The timer that the engine waits on tells the loop in the call that arms it (#567): the loop holds one host timer
// for the deadline, and the wait ends when it fires.
template <class Arm>
asio::awaitable<void> wait_on_a_host_timer(Arm arm) {
    auto ex = co_await asio::this_coro::executor;
    async::HostTimer timer(ex);
    arm(timer);
    co_await timer.async_wait(asio::use_awaitable);
}

template <class Arm>
void expect_a_host_timer_to_fire(Arm arm) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    bool done = false;
    loop.spawn(wait_on_a_host_timer(arm), [&](std::exception_ptr error) { done = !error; });
    while (executor->step()) {}
    EXPECT_FALSE(done);
    EXPECT_EQ(executor->timers(), 1u) << "one host timer for the deadline, nothing else scheduled";
    EXPECT_EQ(executor->pending(), 0u);
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < limit) {
        if (!executor->step()) std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(done);
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, AHostTimerArmedAfterADelayFiresOnTheHostsTimer) {
    expect_a_host_timer_to_fire([](async::HostTimer& timer) { timer.arm_after(30ms); });
}

TEST(HostLoop, AHostTimerArmedAtAnInstantFiresOnTheHostsTimer) {
    expect_a_host_timer_to_fire(
        [](async::HostTimer& timer) { timer.arm_at(std::chrono::steady_clock::now() + 30ms); });
}

// Asio decides when a timer is dispatched, and the host's timer for the same deadline can run first: Asio's Windows
// reactor learns that a timer is due from a thread of its own, so a turn that polls the instant the deadline passes
// can find nothing yet, and a host whose clock runs ahead of Asio's does the same on any platform. A deadline must
// stay noted until Asio has dispatched the timer, or the loop idles and the wait never ends.
template <class Arm>
void expect_a_deadline_that_the_host_reaches_first_to_fire(std::chrono::milliseconds lead, Arm arm) {
    auto executor = std::make_shared<ManualExecutor>();
    executor->clock_lead = lead;
    async::HostLoop loop(executor);
    bool done = false;
    loop.spawn(wait_on_a_host_timer(arm), [&](std::exception_ptr error) { done = !error; });
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < limit) {
        if (!executor->step()) std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(done) << "the host's timer ran before Asio dispatched the wait, and nothing woke the loop again";
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_EQ(executor->pending(), 0u);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, ADeadlineThatTheHostsClockReachesBeforeAsioDispatchesItIsNotLost) {
    expect_a_deadline_that_the_host_reaches_first_to_fire(
        20ms, [](async::HostTimer& timer) { timer.arm_after(30ms); });
}

TEST(HostLoop, ADeadlineThatIsAlreadyDueOnTheHostsClockWhenNotedIsNotLost) {
    expect_a_deadline_that_the_host_reaches_first_to_fire(
        50ms, [](async::HostTimer& timer) { timer.arm_after(10ms); });
}

TEST(HostLoop, ARawAsioTimerNotedBeforeTheHostReachesItIsNotLost) {
    auto executor = std::make_shared<ManualExecutor>();
    executor->clock_lead = 20ms;
    async::HostLoop loop(executor);
    bool done = false;
    auto sleeping = []() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(30ms);
        async::note_deadline(ex, timer.expiry());
        co_await timer.async_wait(asio::use_awaitable);
    };
    loop.spawn(sleeping(), [&](std::exception_ptr error) { done = !error; });
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < limit) {
        if (!executor->step()) std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(done) << "the host's timer ran before Asio dispatched the wait, and nothing woke the loop again";
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, ATakenBackDeadlineLeavesNoHostTimerArmed) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    const auto at = std::chrono::steady_clock::now() + 1h;
    async::note_deadline(loop.executor(), at);
    EXPECT_EQ(executor->timers(), 1u);
    EXPECT_FALSE(loop.idle());
    // One note is taken back for one note made; a second takes back nothing.
    async::forget_deadline(loop.executor(), at);
    EXPECT_EQ(executor->timers(), 0u);
    async::forget_deadline(loop.executor(), at);
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_TRUE(loop.idle());
    // Two timers for one instant need two takes back.
    async::note_deadline(loop.executor(), at);
    async::note_deadline(loop.executor(), at);
    async::forget_deadline(loop.executor(), at);
    EXPECT_EQ(executor->timers(), 1u);
    async::forget_deadline(loop.executor(), at);
    EXPECT_EQ(executor->timers(), 0u);
}

asio::awaitable<void> wait_on_a_signal(async::HostTimer& signal) {
    asio::error_code ended;
    co_await signal.async_wait(asio::redirect_error(asio::use_awaitable, ended));
}

TEST(HostLoop, ACancelledHostTimerLeavesNoHostTimerArmed) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    {
        async::HostTimer timer(loop.executor());
        timer.arm_after(1h);
        EXPECT_EQ(executor->timers(), 1u);
        timer.cancel();
        EXPECT_EQ(executor->timers(), 0u);
        // Re-arming takes the earlier deadline back before it notes the new one.
        timer.arm_after(1h);
        timer.arm_after(2h);
        EXPECT_EQ(executor->timers(), 1u);
    }
    EXPECT_EQ(executor->timers(), 0u) << "a destroyed timer takes its deadline back";
    // A timer that never expires is a signal: the loop is told nothing and keeps no host timer for it.
    async::HostTimer signal(loop.executor());
    signal.arm_never();
    EXPECT_EQ(executor->timers(), 0u);
    bool woken = false;
    loop.spawn(wait_on_a_signal(signal), [&](std::exception_ptr) { woken = true; });
    while (executor->step()) {}
    EXPECT_FALSE(woken);
    // As in the engine, the wait is cancelled from a handler on the loop's executor: a timer cancelled from outside a
    // turn completes its wait into the io_context's queue without telling the loop.
    asio::post(loop.executor(), [&signal] { signal.cancel(); });
    while (executor->step()) {}
    EXPECT_TRUE(woken);
    EXPECT_EQ(executor->timers(), 0u);
}

TEST(HostLoop, InHostTurnIsTrueOnlyWhileATurnRunsOnTheCallingThread) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    EXPECT_FALSE(async::in_host_turn());
    bool inside = false;
    asio::post(loop.executor(), [&inside] { inside = async::in_host_turn(); });
    while (executor->step()) {}
    EXPECT_TRUE(inside);
    EXPECT_FALSE(async::in_host_turn()) << "the flag does not outlive the turn";
}

TEST(HostLoop, ALoopsExecutorIsHostPumpedAndAnotherIsNot) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    EXPECT_TRUE(async::is_host_pumped(loop.executor()));
    // An executor derived from the loop's, a strand of it, is run by the loop too.
    EXPECT_TRUE(async::is_host_pumped(asio::any_io_executor(asio::make_strand(loop.executor()))));
    asio::io_context io;
    EXPECT_FALSE(async::is_host_pumped(io.get_executor()));
    EXPECT_FALSE(async::is_host_pumped(asio::any_io_executor{}));
}

namespace {
class FlakyNode : public graph::GraphNode {
public:
    explicit FlakyNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        if (calls_.fetch_add(1) == 0) throw std::runtime_error("flaky: the first attempt fails");
        graph::NodeOutput out;
        out.writes.push_back(graph::ChannelWrite{"result", json("ok")});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int> calls_{0};
};

void expect_a_retry_backoff_to_end(std::chrono::milliseconds lead) {
    graph::NodeFactory::instance().register_type(
        "host_loop_flaky",
        [](const std::string& name, const json&, const graph::NodeContext&) -> std::unique_ptr<graph::GraphNode> {
            return std::make_unique<FlakyNode>(name);
        });
    const auto definition = json{
        {"name", "retry"},
        {"channels", {{"result", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"flaky", {{"type", "host_loop_flaky"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "flaky"}}, {{"from", "flaky"}, {"to", "__end__"}}})}};
    auto engine = graph::GraphEngine::compile(definition, graph::NodeContext());
    engine->set_retry_policy(graph::RetryPolicy{1, 40, 1.0f, 40, 0.0f});

    auto executor = std::make_shared<ManualExecutor>();
    executor->clock_lead = lead;
    async::HostLoop loop(executor);
    Finished finished;
    graph::RunConfig config;
    config.thread_id = "retry";
    const auto started = std::chrono::steady_clock::now();
    loop.spawn(drive(engine.get(), config, false, {}), [&](std::exception_ptr error, graph::RunResult result) {
        finished.done = true;
        finished.error = error;
        finished.result = std::move(result);
    });
    const auto limit = started + 3s;
    while (!finished.done && std::chrono::steady_clock::now() < limit) {
        if (!executor->step()) std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(finished.done) << "the retry's backoff never woke the loop";
    EXPECT_FALSE(finished.error);
    EXPECT_EQ(finished.result.channel<std::string>("result"), "ok");
    EXPECT_GE(std::chrono::steady_clock::now() - started, 40ms);
}
}  // namespace

TEST(HostLoop, ARetryBackoffWaitsOnTheHostsTimer) { expect_a_retry_backoff_to_end(0ms); }

// The host's timer for the backoff runs before Asio dispatches the Asio timer, as in the tests of deadlines above.
TEST(HostLoop, ARetryBackoffEndsWhenTheHostsClockRunsAheadOfAsios) { expect_a_retry_backoff_to_end(30ms); }

TEST(HostLoop, AGraphCallsTheModelThroughTheHostSeam) {
    Host host;
    EXPECT_TRUE(host.provider->host_pumped());
    auto engine = host.engine();
    Finished finished;
    host.loop->spawn(drive(engine.get(), question(), false, {}), [&](std::exception_ptr error, graph::RunResult result) {
        finished.done = true;
        finished.error = error;
        finished.result = std::move(result);
    });
    EXPECT_FALSE(finished.done) << "spawn returns before anything runs";
    EXPECT_EQ(host.transport->requests, 0);
    ASSERT_TRUE(host.run_until([&] { return finished.done; }));
    if (finished.error) std::rethrow_exception(finished.error);
    EXPECT_EQ(assistant_text(finished.result), "hello from the host");
    EXPECT_EQ(host.transport->requests, 1);
    EXPECT_EQ(host.transport->started_on, std::this_thread::get_id()) << "the request left from the host's thread";
    EXPECT_NE(host.transport->last_body.find("\"hello\""), std::string::npos);
    EXPECT_EQ(finished.result.execution_trace, std::vector<std::string>{"llm"});
    EXPECT_EQ(finished.result.usage.total_tokens_value().value_or(0), 6u);
}

TEST(HostLoop, AStreamedRunDeliversTheTokensOnTheHostsThread) {
    Host host(sse_body, "text/event-stream");
    auto engine = host.engine();
    Finished finished;
    std::string tokens;
    std::vector<std::thread::id> delivered_on;
    host.loop->spawn(
        drive(engine.get(), question(), true,
              [&](const graph::GraphEvent& event) {
                  if (event.type != graph::GraphEvent::Type::LLM_TOKEN) return;
                  tokens += event.data.get<std::string>();
                  delivered_on.push_back(std::this_thread::get_id());
              }),
        [&](std::exception_ptr error, graph::RunResult result) {
            finished.done = true;
            finished.error = error;
            finished.result = std::move(result);
        });
    ASSERT_TRUE(host.run_until([&] { return finished.done; }));
    if (finished.error) std::rethrow_exception(finished.error);
    EXPECT_EQ(tokens, "hello from the host");
    EXPECT_EQ(assistant_text(finished.result), "hello from the host");
    ASSERT_GE(delivered_on.size(), 2u);
    for (const auto thread : delivered_on) EXPECT_EQ(thread, std::this_thread::get_id());
}

TEST(HostLoop, ASynchronousRunIsRefusedBeforeAnythingIsSent) {
    Host host;
    auto engine = host.engine();
    // run() drives a private io_context to completion: it would wait for the host's loop, which this thread runs.
    std::exception_ptr error;
    try {
        engine->run(question());
    } catch (...) {
        error = std::current_exception();
    }
    ASSERT_TRUE(error) << "the run did not fail";
    EXPECT_EQ(reason_of(error), "runtime.blocking.host_pumped");
    EXPECT_EQ(host.transport->requests, 0);

    // So is the provider's own blocking call, with the SDK's reason and nothing sent.
    auto request = make_provider_request(
        *host.provider, "fixture-model", {portable_message(ChatMessage{"user", "hello"})});
    const auto outcome = host.provider->invoke(std::move(request));
    const auto* failure = std::get_if<sp::Failure>(outcome.get());
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error.kind, sp::ErrorKind::Misuse);
    EXPECT_EQ(failure->error.reason, "runtime.blocking.host_pumped");
    EXPECT_EQ(failure->error.retry_safety, sp::RetrySafety::NotSent);
    EXPECT_EQ(host.transport->requests, 0);
}

TEST(HostLoop, AProviderThatIsNotHostPumpedIsNotRefused) {
    // The refusal is about waiting for a loop that only the caller can run, not about the seam: a host executor
    // that declares itself pumped by nobody gets the blocking path (its work must then run elsewhere).
    auto executor = std::make_shared<ManualExecutor>(false);
    auto transport = std::make_shared<FakeTransport>(executor, "application/json", std::vector<std::string>{buffered_body});
    llm::SchemaProvider provider(chat_descriptor(), sp::runtime::Options::with_api_key("test-key"), executor, transport);
    EXPECT_FALSE(provider.host_pumped());
}

TEST(HostLoop, CancellingARunCancelsTheModelCall) {
    Host host;
    host.transport->hold = true;
    auto engine = host.engine();
    Finished finished;
    auto config = question();
    config.cancel_token = std::make_shared<graph::CancelToken>();
    auto token = config.cancel_token;
    host.loop->spawn(drive(engine.get(), std::move(config), false, {}), [&](std::exception_ptr error, graph::RunResult) {
        finished.done = true;
        finished.error = error;
    });
    ASSERT_TRUE(host.run_until([&] { return host.transport->requests == 1; }));
    EXPECT_FALSE(finished.done);
    token->cancel();
    host.loop->wake();
    ASSERT_TRUE(host.run_until([&] { return finished.done; }));
    ASSERT_TRUE(finished.error);
    EXPECT_GE(host.transport->cancels, 1) << "the cancelled run cancelled its HTTP attempt";
    try {
        std::rethrow_exception(finished.error);
    } catch (const graph::CancelledException&) {
    } catch (const graph::NodeExecutionError&) {
    }
}

TEST(HostLoop, DestroyingTheLoopWithARunInFlightLeavesNothingRunning) {
    Host host;
    host.transport->hold = true;
    auto engine = host.engine();
    bool completed = false;
    host.loop->spawn(drive(engine.get(), question(), false, {}),
                     [&](std::exception_ptr, graph::RunResult) { completed = true; });
    ASSERT_TRUE(host.run_until([&] { return host.transport->requests == 1; }));
    // The host drops the loop while the model call is in flight; the coroutine's frame goes with it.
    host.loop.reset();
    engine.reset();
    host.provider.reset();
    while (host.client_executor->step() || host.loop_executor->step()) {}
    EXPECT_FALSE(completed);
}

TEST(HostLoop, DestroyingTheLoopWithATimerPendingLeavesNothingScheduled) {
    auto executor = std::make_shared<ManualExecutor>();
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(wait_on_a_host_timer([](async::HostTimer& timer) { timer.arm_after(10s); }),
                   [&](std::exception_ptr) { completed = true; });
        while (executor->step()) {}
        EXPECT_EQ(executor->timers(), 1u) << "one host timer for the deadline";
    }
    EXPECT_EQ(executor->timers(), 0u) << "the loop cancelled its host timer";
    while (executor->step()) {}
    EXPECT_FALSE(completed);
}

namespace {
sp::embeddings::Request embed() {
    sp::embeddings::Request request;
    request.model = "fixture-embed";
    request.inputs.push_back({"hello"});
    return request;
}
}  // namespace

TEST(HostLoop, AnEndpointCallKeepsTheOutcomeTheSdkDelivered) {
    // The origin is one the built-in policy admits endpoints at; the fake transport answers, nothing is sent.
    Host host({embedding_body}, "application/json", "https://api.openai.com");
    std::optional<sp::embeddings::Outcome> outcome;
    bool done = false;
    auto call = [&]() -> asio::awaitable<void> {
        outcome = co_await host.provider->complete_endpoint_async(embed());
    };
    host.loop->spawn(call(), [&](std::exception_ptr) { done = true; });
    ASSERT_TRUE(host.run_until([&] { return done; }));
    ASSERT_TRUE(outcome);
    const auto* result = std::get_if<sp::embeddings::Result>(&*outcome);
    ASSERT_NE(result, nullptr) << std::get<sp::Failure>(*outcome).error.safe_message << " / "
                               << std::get<sp::Failure>(*outcome).error.reason;
    ASSERT_EQ(result->vectors.size(), 1u);
    EXPECT_EQ(result->vectors[0], (std::vector<float>{0.25f, -0.5f}));
    EXPECT_EQ(host.transport->requests, 1);
}

TEST(HostLoop, ABlockingEndpointCallIsRefusedBeforeAnythingIsSent) {
    Host host({embedding_body}, "application/json", "https://api.openai.com");
    // The SDK refuses its own blocking call.
    const auto direct = host.provider->complete_endpoint(embed());
    const auto* refused = std::get_if<sp::Failure>(&direct);
    ASSERT_NE(refused, nullptr);
    EXPECT_EQ(refused->error.reason, "runtime.blocking.host_pumped");
    // So is the async form driven by a blocking bridge: it would wait for a loop that only this thread can run.
    const auto bridged = async::run_sync(host.provider->complete_endpoint_async(embed()));
    const auto* bridged_refusal = std::get_if<sp::Failure>(&bridged);
    ASSERT_NE(bridged_refusal, nullptr);
    EXPECT_EQ(bridged_refusal->error.kind, sp::ErrorKind::Misuse);
    EXPECT_EQ(bridged_refusal->error.reason, "runtime.blocking.host_pumped");
    EXPECT_EQ(host.transport->requests, 0);
}

// ---- Work handed to the loop from other threads (#602) ---------------------------------------------------------------
//
// The loop's turns, and the handlers in them, run on the host's thread only. Handing it work does not: a post to its
// executor, and HostLoop::wake, are safe from any thread, as for an io_context. A blocking tool that runs on the
// process's pool completes by posting its continuation from the pool's thread.

namespace {

// Holds the posting threads until the host is ready for them.
class Gate {
public:
    void open() { open_.store(true); }
    void pass() const {
        while (!open_.load()) std::this_thread::yield();
    }

private:
    std::atomic<bool> open_{false};
};

// `threads` threads post `posts_each` handlers each while the test thread plays the host. Every handler runs, on the
// host thread. A post whose wake is lost leaves a handler queued with no turn scheduled, and the pump times out.
void expect_posts_from_threads_to_run(unsigned threads, unsigned posts_each, std::size_t turn_handlers) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop::Options options;
    options.turn_handlers = turn_handlers;
    async::HostLoop loop(executor, options);
    const auto host_thread = std::this_thread::get_id();
    const std::size_t total = std::size_t{threads} * posts_each;
    std::size_t ran = 0;     // written by handlers only, which run on the host thread
    bool elsewhere = false;  // a handler ran on another thread
    Gate gate;
    std::vector<std::thread> posters;
    for (unsigned t = 0; t < threads; ++t) {
        posters.emplace_back([&, ex = loop.executor()] {
            gate.pass();
            for (unsigned i = 0; i < posts_each; ++i) {
                asio::post(ex, [&] {
                    if (std::this_thread::get_id() != host_thread) elsewhere = true;
                    ++ran;
                });
            }
        });
    }
    gate.open();
    const bool finished = executor->run_until([&] { return ran == total; }, 30s);
    for (auto& poster : posters) poster.join();
    EXPECT_TRUE(finished) << "a post from another thread was left without a turn: " << ran << " of " << total;
    EXPECT_FALSE(elsewhere) << "a handler ran on a thread other than the host's";
    while (executor->step()) {}
    EXPECT_EQ(ran, total);
    EXPECT_EQ(executor->pending(), 0u);
    EXPECT_TRUE(loop.idle());
}

}  // namespace

TEST(HostLoop, PostsFromSeveralThreadsRunOnTheHostThreadWhileTheHostPumps) {
    for (int round = 0; round < 12; ++round) expect_posts_from_threads_to_run(4, 1500, 256);
}

TEST(HostLoop, PostsFromSeveralThreadsRunWhenEveryTurnTakesOneHandler) {
    // Each turn ends with more work and schedules the next one, while the other threads wake the loop too.
    for (int round = 0; round < 6; ++round) expect_posts_from_threads_to_run(3, 300, 1);
}

TEST(HostLoop, APostFromAnotherThreadWhileATurnRunsIsNotLeftWithoutATurn) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    std::atomic<bool> in_turn{false}, posted{false};
    bool second = false;
    std::thread poster([&, ex = loop.executor()] {
        while (!in_turn.load()) std::this_thread::yield();
        asio::post(ex, [&second] { second = true; });
        posted = true;
    });
    // The first handler holds its turn open until the other thread has posted and woken the loop.
    asio::post(loop.executor(), [&] {
        in_turn = true;
        while (!posted.load()) std::this_thread::yield();
    });
    const bool finished = executor->run_until([&] { return second; });
    poster.join();
    EXPECT_TRUE(finished);
    while (executor->step()) {}
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, APostToALoopFromTheTurnOfAnotherLoopNestedInItsTurnIsRunByThatTurn) {
    // A handler of one loop pumps another loop, and a handler there posts back to the first. The first loop's turn is
    // still running and polls on, so the post needs no turn of its own, as it would if the second loop did not exist.
    auto outer_host = std::make_shared<ManualExecutor>();
    auto inner_host = std::make_shared<ManualExecutor>();
    async::HostLoop outer(outer_host), inner(inner_host);
    bool back = false;
    asio::post(outer.executor(), [&] {
        asio::post(inner.executor(), [&] { asio::post(outer.executor(), [&back] { back = true; }); });
        while (inner_host->step()) {}
        EXPECT_FALSE(back) << "the post waits for the outer turn to poll on";
    });
    ASSERT_TRUE(outer_host->step()) << "the outer loop's turn";
    EXPECT_TRUE(back);
    EXPECT_EQ(outer_host->pending(), 0u) << "the outer loop's running turn ran the post; no further turn was queued";
    EXPECT_TRUE(outer.idle());
    while (inner_host->step()) {}
    EXPECT_TRUE(inner.idle());
    EXPECT_EQ(outer_host->tasks_run, 1u);
}

TEST(HostLoop, WakesFromManyThreadsQueueOneTurnAtATime) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    std::atomic<bool> stop{false};
    std::vector<std::thread> wakers;
    for (int i = 0; i < 4; ++i) wakers.emplace_back([&] { while (!stop.load()) loop.wake(); });
    // The host's queue is looked at from a thread of its own too: seen only between two steps of the host, a second
    // turn that was queued while the first one ran could not be told from the first one.
    std::atomic<std::size_t> most_queued{0};
    std::thread observer([&] {
        while (!stop.load()) {
            const auto queued = executor->pending();
            auto most = most_queued.load();
            while (queued > most && !most_queued.compare_exchange_weak(most, queued)) {}
        }
    });
    const auto until = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < until) {
        const auto queued = executor->pending();
        auto most = most_queued.load();
        while (queued > most && !most_queued.compare_exchange_weak(most, queued)) {}
        executor->step();
    }
    stop = true;
    for (auto& waker : wakers) waker.join();
    observer.join();
    while (executor->step()) {}
    EXPECT_LE(most_queued.load(), 1u) << "the host's queue holds one turn of the loop, however many threads wake it";
    EXPECT_GT(executor->tasks_run, 0u);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, AWakeThatTheHostRefusesIsRetriedByTheNextOne) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    executor->refuse_posts(true);
    std::thread([&] { loop.wake(); }).join();
    EXPECT_EQ(executor->pending(), 0u) << "the host took nothing";
    EXPECT_TRUE(loop.idle()) << "a refused turn is not left scheduled";
    executor->refuse_posts(false);
    bool ran = false;
    std::thread([&, ex = loop.executor()] { asio::post(ex, [&ran] { ran = true; }); }).join();
    EXPECT_EQ(executor->pending(), 1u) << "the next post schedules the turn";
    while (executor->step()) {}
    EXPECT_TRUE(ran);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, AWakeThatCountedOnATurnTheHostRefusedIsNotLeftWithoutOne) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    bool first = false, second = false;
    int asked = 0;
    // The host refuses the first turn the loop asks for. While it does, another thread posts and wakes the loop: it
    // finds a turn scheduled, and returns having counted on it.
    executor->before_post = [&]() -> bool {
        if (++asked != 1) return false;
        std::thread([&, ex = loop.executor()] { asio::post(ex, [&second] { second = true; }); }).join();
        return true;
    };
    asio::post(loop.executor(), [&first] { first = true; });
    executor->before_post = nullptr;
    EXPECT_EQ(asked, 2) << "the loop asked the host again for the turn that the other wake counted on";
    while (executor->step()) {}
    EXPECT_TRUE(second) << "the work of the wake that counted on the refused turn waited for a wake that never came";
    EXPECT_TRUE(first);
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, ARefusedTurnIsAskedForAgainOnlyWhileAWakeCountsOnIt) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    bool ran = false;
    int asked = 0;
    // The host refuses every time, and one other wake counts on the first turn only.
    executor->before_post = [&]() -> bool {
        if (++asked == 1)
            std::thread([&, ex = loop.executor()] { asio::post(ex, [&ran] { ran = true; }); }).join();
        return true;
    };
    loop.wake();
    EXPECT_EQ(asked, 2) << "one more ask for the wake that counted on the turn, and no spinning after that";
    EXPECT_EQ(executor->pending(), 0u);
    EXPECT_TRUE(loop.idle()) << "a refused turn is not left scheduled";
    executor->before_post = nullptr;
    EXPECT_FALSE(ran);
    loop.wake();  // the host takes posts again, and the next wake schedules the turn
    EXPECT_EQ(executor->pending(), 1u);
    while (executor->step()) {}
    EXPECT_TRUE(ran);
}

TEST(HostLoop, PostersAreNotStoppedOrSpunByAHostThatRefusesEveryThirdPost) {
    // Several threads post while the host refuses one post in three. The posters return, none asks the host over and
    // over, and once the host takes posts again one wake runs everything that was left without a turn.
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    std::atomic<unsigned> asked{0}, finished{0};
    executor->before_post = [&]() -> bool { return asked.fetch_add(1) % 3 == 0; };
    constexpr unsigned threads = 4, posts_each = 400, total = threads * posts_each;
    std::size_t ran = 0;  // written by handlers only, which run on the host thread
    Gate gate;
    std::vector<std::thread> posters;
    for (unsigned t = 0; t < threads; ++t) {
        posters.emplace_back([&, ex = loop.executor()] {
            gate.pass();
            for (unsigned i = 0; i < posts_each; ++i) asio::post(ex, [&ran] { ++ran; });
            ++finished;
        });
    }
    gate.open();
    while (finished.load() < threads) {
        if (!executor->step()) std::this_thread::yield();
    }
    for (auto& poster : posters) poster.join();
    EXPECT_LE(asked.load(), 6 * total) << "the loop asked the host again and again";
    executor->before_post = nullptr;
    loop.wake();
    EXPECT_TRUE(executor->run_until([&] { return ran == total; }, 30s)) << ran << " of " << total << " handlers ran";
    while (executor->step()) {}
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, ATurnQueuedWhileAnotherRunsAndTakenByANestedPumpLosesNoWork) {
    // A handler of the running turn pumps the host, which takes the turn that another thread's post has queued. That
    // second call finds the first turn still running and returns; the running turn polls on and runs the post.
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    bool posted_ran = false, nested_pumped = false;
    asio::post(loop.executor(), [&] {
        std::thread([&, ex = loop.executor()] { asio::post(ex, [&posted_ran] { posted_ran = true; }); }).join();
        EXPECT_EQ(executor->pending(), 1u) << "the other thread's post queued a turn";
        nested_pumped = executor->step();
        EXPECT_FALSE(posted_ran) << "the nested call did not poll";
    });
    ASSERT_TRUE(executor->step());
    EXPECT_TRUE(nested_pumped);
    EXPECT_TRUE(posted_ran) << "the running turn polled on after the handler returned";
    EXPECT_EQ(executor->pending(), 0u) << "no turn is left queued";
    EXPECT_TRUE(loop.idle());
    EXPECT_EQ(executor->tasks_run, 2u);
}

namespace {
// A tool that blocks a thread of the process's pool, as a legacy synchronous tool does.
class PoolTool final : public Tool {
public:
    ChatTool get_definition() const override { return ChatTool{"pool", "blocks a pool thread", json{{"type", "object"}}}; }
    std::string execute(const json&) override {
        std::this_thread::sleep_for(1ms);
        ran_on = std::this_thread::get_id();
        return "done";
    }
    std::string get_name() const override { return "pool"; }
    std::thread::id ran_on;
};
}  // namespace

TEST(HostLoop, ABlockingToolFinishesOnThePoolAndItsContinuationRunsOnTheHostThread) {
    auto executor = std::make_shared<ManualExecutor>();
    async::HostLoop loop(executor);
    PoolTool tool;
    const auto host_thread = std::this_thread::get_id();
    int calls = 0;
    bool resumed_elsewhere = false, ran_on_host = false;
    bool done = false;
    auto drive = [&]() -> asio::awaitable<void> {
        for (int i = 0; i < 40; ++i) {
            const auto result = co_await tool.execute_async(json::object());
            if (result != "done") co_return;
            if (std::this_thread::get_id() != host_thread) resumed_elsewhere = true;
            if (tool.ran_on == host_thread) ran_on_host = true;
            ++calls;
        }
    };
    loop.spawn(drive(), [&](std::exception_ptr error) { done = !error; });
    ASSERT_TRUE(executor->run_until([&] { return done; }, 30s)) << calls << " of 40 calls finished";
    EXPECT_EQ(calls, 40);
    EXPECT_FALSE(resumed_elsewhere) << "the coroutine continued on a thread other than the host's";
    EXPECT_FALSE(ran_on_host) << "the tool ran on the host thread";
    while (executor->step()) {}
    EXPECT_TRUE(loop.idle());
}

TEST(HostLoop, DestroyingTheLoopDropsWhatOtherThreadsQueuedAndItsScheduledTurnFindsItClosed) {
    auto executor = std::make_shared<ManualExecutor>();
    auto probe = std::make_shared<int>(0);  // every queued handler holds a copy
    std::atomic<int> ran{0};
    {
        async::HostLoop loop(executor);
        std::vector<std::thread> posters;
        for (int t = 0; t < 4; ++t) {
            posters.emplace_back([&, ex = loop.executor()] {
                for (int i = 0; i < 100; ++i) asio::post(ex, [&ran, probe] { ++ran; });
            });
        }
        for (auto& poster : posters) poster.join();
        EXPECT_EQ(executor->pending(), 1u) << "one turn is queued, however many threads posted";
        EXPECT_GT(probe.use_count(), 1);
    }
    EXPECT_EQ(probe.use_count(), 1) << "the loop destroyed the handlers it never ran";
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(executor->pending(), 1u) << "the turn it had scheduled is the host's to run";
    while (executor->step()) {}
    EXPECT_EQ(ran, 0) << "a turn of a closed loop runs nothing";
}

// The posting threads end before the loop is destroyed, as the contract requires: a post that races with the destructor
// would use an io_context that is being destroyed, and the loop cannot wait for it. What can fail here is the work that
// other threads queued while the host pumped: a handler that is neither run nor destroyed, or a turn scheduled before
// the close that runs work after it.
TEST(HostLoop, WorkQueuedByThreadsWhileTheHostPumpsIsRunOrDroppedWhenTheLoopIsDestroyedAfterThem) {
    for (int round = 0; round < 8; ++round) {
        auto executor = std::make_shared<ManualExecutor>();
        auto probe = std::make_shared<int>(0);
        std::atomic<int> ran{0}, posted{0};
        {
            async::HostLoop loop(executor);
            std::atomic<bool> stop{false};
            std::vector<std::thread> posters;
            for (int t = 0; t < 4; ++t) {
                posters.emplace_back([&, ex = loop.executor()] {
                    while (!stop.load()) {
                        asio::post(ex, [&ran, probe] { ++ran; });
                        ++posted;
                    }
                });
            }
            // Pump until the posters have been running for a while, and at least one handler has run.
            const auto started = std::chrono::steady_clock::now();
            auto elapsed = [&] { return std::chrono::steady_clock::now() - started; };
            while (elapsed() < 8ms || (ran == 0 && elapsed() < 10s)) executor->step();
            stop = true;
            for (auto& poster : posters) poster.join();
            // The host stops pumping with work still queued, and drops the loop.
        }
        EXPECT_EQ(probe.use_count(), 1) << "a handler that did not run was destroyed with the loop";
        EXPECT_GT(ran, 0);
        EXPECT_LE(ran, posted);
        const int run_before = ran;
        while (executor->step()) {}
        EXPECT_EQ(ran, run_before) << "a turn that was scheduled before the loop closed ran work";
    }
}

namespace {
// Posts to the loop's executor when its coroutine frame is destroyed, which a closing loop does to a frame that
// has not finished.
asio::awaitable<void> wait_and_post_when_destroyed(bool* ran) {
    struct PostsWhenDestroyed {
        asio::any_io_executor executor;
        bool* ran;
        ~PostsWhenDestroyed() {
            asio::post(executor, [ran = this->ran] { *ran = true; });
        }
    };
    PostsWhenDestroyed guard{co_await asio::this_coro::executor, ran};
    async::HostTimer timer(guard.executor);
    timer.arm_after(10s);
    co_await timer.async_wait(asio::use_awaitable);
}
}  // namespace

TEST(HostLoop, WorkPostedFromAFrameThatTheLoopsCloseDestroysIsDroppedWithoutATurn) {
    auto executor = std::make_shared<ManualExecutor>();
    bool ran = false, completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(wait_and_post_when_destroyed(&ran), [&](std::exception_ptr) { completed = true; });
        while (executor->step()) {}
        EXPECT_EQ(executor->timers(), 1u);
    }
    EXPECT_EQ(executor->timers(), 0u);
    EXPECT_EQ(executor->pending(), 0u) << "the closed loop schedules no turn for the post";
    while (executor->step()) {}
    EXPECT_FALSE(ran);
    EXPECT_FALSE(completed);
}
