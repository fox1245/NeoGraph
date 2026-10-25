// The engine on a host's event loop (#535): async::HostLoop pumps an io_context from tasks the host runs, and a
// SchemaProvider built over the SDK's host seam sends through a transport the host supplies. The host here is a
// manual executor that the test runs from its own thread, as a JavaScript host runs its microtasks, so nothing
// waits and the loop's work runs on that thread only. The last tests hand work to the loop from other threads, as a
// blocking tool's completion does (#602). No network, no libcurl, no key.
#include <gtest/gtest.h>

#include <neograph/async/host_loop.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/tool.h>
#include <neograph/tool_execution.h>

#include "../src/core/host_timer.h"
#include "../src/core/loop_handoff.h"
#include "fixtures/manual_executor.h"

#include <descriptor/descriptor.h>
#include <runtime/host.h>

#include <asio/co_spawn.hpp>
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
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

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

// ---- Work on the process's pools that outlives the loop (#605) -------------------------------------------------------
//
// A blocking tool and a process tool run on a pool of the process, and complete by posting to the executor of the run
// that awaits them. The host cannot join that pool, so it can destroy the loop while such a tool still runs. The loop
// takes over the coroutine that awaits it, destroys it on the host's thread, and the late result is dropped: nothing
// of the loop is touched from the pool after that.

namespace {

// What the frame of a coroutine that awaits pool work reports when it is destroyed.
struct FrameProbe {
    std::atomic<int> destroyed{0};
    std::atomic<int> destroyed_on_host{0};
    std::atomic<int> started{0};
    std::thread::id host = std::this_thread::get_id();
};

struct FrameSentinel {
    FrameProbe* probe;
    explicit FrameSentinel(FrameProbe* p) : probe(p) { ++probe->started; }
    ~FrameSentinel() {
        ++probe->destroyed;
        if (std::this_thread::get_id() == probe->host) ++probe->destroyed_on_host;
    }
};

// A pool tool that waits for the test to let it go, and says when it is done with its own object: after `finished`
// the pool thread does not touch the tool again, so the test may destroy it.
class GatedTool final : public Tool {
public:
    ChatTool get_definition() const override { return ChatTool{"gated", "waits on the pool", json{{"type", "object"}}}; }
    std::string execute(const json&) override {
        entered = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (!release.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        finished = true;
        return "late";
    }
    std::string get_name() const override { return "gated"; }
    std::atomic<bool> entered{false}, release{false}, finished{false};
};

// Lets a gated tool go and waits for it before the tool is destroyed, whether or not the test body got that far.
class ReleaseOnExit {
public:
    explicit ReleaseOnExit(GatedTool& tool) : tool_(tool) {}
    ~ReleaseOnExit() {
        tool_.release = true;
        if (!armed_) return;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (!tool_.finished.load() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(100us);
    }
    void arm() { armed_ = true; }

private:
    GatedTool& tool_;
    bool armed_ = false;
};

// Returns once every pool task that opened a LoopHandoff has finished `handoff->finish()` and released its handoff.
void let_the_pool_thread_post() {
    const auto limit = std::chrono::steady_clock::now() + 30s;
    while (async::detail::LoopHandoff::active_count() != 0 && std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
}

asio::awaitable<void> await_a_pool_tool(Tool* tool, FrameProbe* probe, std::string* result = nullptr) {
    FrameSentinel sentinel(probe);
    auto text = co_await tool->execute_async(json::object());
    if (result) *result = std::move(text);
}

}  // namespace

TEST(HostLoopPoolWork, ALoopDestroyedWhileABlockingToolRunsTakesItsCoroutineAndDropsTheLateResult) {
    auto executor = std::make_shared<ManualExecutor>();
    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe probe;
    std::string result;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_pool_tool(&tool, &probe, &result), [&](std::exception_ptr) { completed = true; });
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
        EXPECT_EQ(probe.destroyed.load(), 0);
    }
    EXPECT_EQ(probe.destroyed.load(), 1) << "the destructor destroyed the coroutine that awaits the pool";
    EXPECT_EQ(probe.destroyed_on_host.load(), 1) << "on the host's thread";
    const auto queued = executor->pending();
    tool.release = true;
    ASSERT_TRUE(executor->run_until([&] { return tool.finished.load(); }));
    let_the_pool_thread_post();
    EXPECT_FALSE(completed) << "the run's completion is never called once its loop is gone";
    EXPECT_TRUE(result.empty()) << "the late result went nowhere";
    EXPECT_EQ(probe.destroyed.load(), 1);
    EXPECT_EQ(executor->pending(), queued) << "the late completion scheduled no turn of the dead loop";
    while (executor->step()) {}
    EXPECT_FALSE(completed);
}

TEST(HostLoopPoolWork, ABlockingToolThatFinishesBeforeTheLoopIsDestroyedIsDelivered) {
    auto executor = std::make_shared<ManualExecutor>();
    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe probe;
    std::string result;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_pool_tool(&tool, &probe, &result), [&](std::exception_ptr error) { completed = !error; });
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
        tool.release = true;
        ASSERT_TRUE(executor->run_until([&] { return completed; }));
        let_the_pool_thread_post();
    }
    EXPECT_EQ(result, "late");
    EXPECT_EQ(probe.destroyed.load(), 1);
}

TEST(HostLoopPoolWork, ARunOnAStrandOfTheLoopIsTakenOverToo) {
    auto executor = std::make_shared<ManualExecutor>();
    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe probe;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        asio::co_spawn(asio::make_strand(loop.executor()), await_a_pool_tool(&tool, &probe),
                       [&](std::exception_ptr) { completed = true; });
        loop.wake();
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
    }
    EXPECT_EQ(probe.destroyed.load(), 1);
    tool.release = true;
    ASSERT_TRUE(executor->run_until([&] { return tool.finished.load(); }));
    let_the_pool_thread_post();
    EXPECT_FALSE(completed);
    while (executor->step()) {}
}

TEST(HostLoopPoolWork, ARunOnOneLoopIsUntouchedWhenAnotherLoopIsDestroyed) {
    // Two runs await the pool; one of two loops is destroyed. The run of the loop that stays gets its result.
    auto kept_host = std::make_shared<ManualExecutor>();
    auto dropped_host = std::make_shared<ManualExecutor>();
    GatedTool kept_tool, dropped_tool;
    ReleaseOnExit release_kept(kept_tool), release_dropped(dropped_tool);
    FrameProbe kept_probe, dropped_probe;
    std::string kept_result;
    bool kept_done = false, dropped_done = false;
    async::HostLoop kept(kept_host);
    {
        async::HostLoop dropped(dropped_host);
        kept.spawn(await_a_pool_tool(&kept_tool, &kept_probe, &kept_result),
                   [&](std::exception_ptr error) { kept_done = !error; });
        dropped.spawn(await_a_pool_tool(&dropped_tool, &dropped_probe), [&](std::exception_ptr) { dropped_done = true; });
        release_kept.arm();
        release_dropped.arm();
        ASSERT_TRUE(kept_host->run_until([&] { return kept_tool.entered.load(); }));
        ASSERT_TRUE(dropped_host->run_until([&] { return dropped_tool.entered.load(); }));
    }
    EXPECT_EQ(dropped_probe.destroyed.load(), 1);
    EXPECT_EQ(kept_probe.destroyed.load(), 0);
    dropped_tool.release = true;
    kept_tool.release = true;
    ASSERT_TRUE(kept_host->run_until([&] { return kept_done; }));
    EXPECT_EQ(kept_result, "late");
    EXPECT_FALSE(dropped_done);
    let_the_pool_thread_post();
}

TEST(HostLoopPoolWork, ALoopDestroyedAsPoolToolsFinishTakesOrDeliversEachOfThem) {
    // Deterministically exercise both outcomes on one loop: `first` finishes and is delivered when the host steps the
    // executor before destroying the loop, while `second` is taken by the destructor.
    {
        auto executor = std::make_shared<ManualExecutor>();
        GatedTool first, second;
        ReleaseOnExit release_first(first), release_second(second);
        FrameProbe probe;
        std::string first_result, second_result;
        bool first_done = false, second_done = false;
        {
            async::HostLoop loop(executor);
            loop.spawn(await_a_pool_tool(&first, &probe, &first_result),
                       [&](std::exception_ptr error) { first_done = !error; });
            loop.spawn(await_a_pool_tool(&second, &probe, &second_result),
                       [&](std::exception_ptr error) { second_done = !error; });
            release_first.arm();
            release_second.arm();
            ASSERT_TRUE(executor->run_until([&] { return first.entered.load() && second.entered.load(); }));
            first.release = true;
            ASSERT_TRUE(executor->run_until([&] { return first_done; }));
            second.release = true;
        }
        EXPECT_TRUE(first_done);
        EXPECT_EQ(first_result, "late");
        EXPECT_FALSE(second_done);
        EXPECT_TRUE(second_result.empty());
        EXPECT_EQ(probe.destroyed.load(), 2);
        EXPECT_EQ(probe.destroyed_on_host.load(), 2);
        ASSERT_TRUE(executor->run_until([&] { return second.finished.load(); }));
        let_the_pool_thread_post();
        while (executor->step()) {}
    }

    // Two tools are let go and the loop is stepped or destroyed at once, a little earlier or later each round, so that
    // the pool threads post, or find the coroutine taken, around the moment the destructor takes the gate.
    for (unsigned round = 0; round < 150; ++round) {
        auto executor = std::make_shared<ManualExecutor>();
        GatedTool first, second;
        ReleaseOnExit release_first(first), release_second(second);
        FrameProbe probe;
        {
            async::HostLoop loop(executor);
            loop.spawn(await_a_pool_tool(&first, &probe), [](std::exception_ptr) {});
            loop.spawn(await_a_pool_tool(&second, &probe), [](std::exception_ptr) {});
            release_first.arm();
            release_second.arm();
            ASSERT_TRUE(executor->run_until([&] { return first.entered.load() && second.entered.load(); }));
            first.release = true;
            second.release = true;
            for (volatile unsigned spin = 0; spin < (round % 60) * 40; spin = spin + 1) {}
            if (round % 2 == 0) (void)executor->step();
        }
        EXPECT_EQ(probe.destroyed.load(), 2) << "round " << round;
        EXPECT_EQ(probe.destroyed_on_host.load(), 2) << "round " << round;
        ASSERT_TRUE(executor->run_until([&] { return first.finished.load() && second.finished.load(); }));
        let_the_pool_thread_post();
        while (executor->step()) {}
    }
}

TEST(HostLoopPoolWork, ThePoolThreadAsksTheHostForTheTurnWithNoLockOfTheLoopHeld) {
    // The pool thread has posted the result and is inside the host executor's `post`. The host destroys the loop
    // meanwhile. If the pool thread asked the host with `handoff_mutex_` held, `close()` would block at `take_handoffs`
    // before `io_.reset()` and `probe.destroyed` would never reach 1 while `post` waits for it; once `io_.reset()` has
    // destroyed the coroutine, `post` returns and `~HostLoop()` finishes waiting for the in-progress wake.
    auto executor = std::make_shared<ManualExecutor>();
    const auto host_thread = std::this_thread::get_id();
    FrameProbe probe;
    std::atomic<bool> in_post{false}, post_finished{false};
    executor->before_post = [&]() -> bool {
        if (std::this_thread::get_id() == host_thread) return false;
        in_post = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (probe.destroyed.load() == 0 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        post_finished = true;
        return false;
    };
    GatedTool tool;
    ReleaseOnExit release(tool);
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_pool_tool(&tool, &probe), [&](std::exception_ptr) { completed = true; });
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
        tool.release = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (!in_post.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        ASSERT_TRUE(in_post.load()) << "the pool thread never asked the host for a turn";
    }
    EXPECT_TRUE(post_finished.load()) << "the destructor waited for the in-progress pool wake before returning";
    EXPECT_EQ(probe.destroyed.load(), 1) << "the queued continuation went with the loop";
    EXPECT_EQ(probe.destroyed_on_host.load(), 1);
    ASSERT_TRUE(executor->run_until([&] { return tool.finished.load(); }));
    let_the_pool_thread_post();
    EXPECT_FALSE(completed);
    while (executor->step()) {}
    EXPECT_FALSE(completed) << "the turn the pool thread asked for finds the loop closed";
    executor->before_post = nullptr;
}

TEST(HostLoopPoolWork, NoPoolWakeCallsTheHostExecutorAfterTheLoopDestructorReturns) {
    // While `waking_tool` is inside `host->post` (with `relied_` set so a refused post would re-ask) and `running_tool`
    // is still executing on the pool, the host destroys the loop. `~HostLoop()` must not wait for `running_tool`, must
    // wait for the in-progress `host->post` of `waking_tool` to finish, and must prevent any further `host->post` call
    // after `~HostLoop()` returns.
    auto executor = std::make_shared<ManualExecutor>();
    const auto host_thread = std::this_thread::get_id();
    FrameProbe probe;
    std::atomic<bool> in_post{false}, wake_returned{false}, destructor_returned{false};
    std::atomic<int> posts_after_destroy{0};
    executor->before_post = [&]() -> bool {
        if (destructor_returned.load()) {
            ++posts_after_destroy;
            return true;
        }
        if (std::this_thread::get_id() == host_thread) return false;
        in_post = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (probe.destroyed.load() < 2 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        std::this_thread::sleep_for(5ms);
        wake_returned = true;
        return true;  // refuse: without a closed_ check before re-asking, relied_ would trigger another post
    };
    GatedTool waking_tool, running_tool;
    ReleaseOnExit release_waking(waking_tool), release_running(running_tool);
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_pool_tool(&waking_tool, &probe), [](std::exception_ptr) {});
        loop.spawn(await_a_pool_tool(&running_tool, &probe), [](std::exception_ptr) {});
        release_waking.arm();
        release_running.arm();
        ASSERT_TRUE(executor->run_until([&] { return waking_tool.entered.load() && running_tool.entered.load(); }));
        waking_tool.release = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (!in_post.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        ASSERT_TRUE(in_post.load());
        // Set `relied_` while `scheduled_` is held by the in-flight pool wake.
        loop.wake();
    }
    destructor_returned = true;
    EXPECT_TRUE(wake_returned.load()) << "~HostLoop() returned only after the in-progress pool wake finished";
    EXPECT_FALSE(running_tool.finished.load()) << "~HostLoop() did not wait for the still-running pool tool";
    EXPECT_EQ(probe.destroyed.load(), 2);
    EXPECT_EQ(probe.destroyed_on_host.load(), 2);
    running_tool.release = true;
    let_the_pool_thread_post();
    EXPECT_EQ(posts_after_destroy.load(), 0) << "no pool thread called host->post after ~HostLoop() returned";
    executor->before_post = nullptr;
    while (executor->step()) {}
}

TEST(HostLoopPoolWork, APoolToolFinishingOnAnAlreadyQueuedStrandStillWakesTheLoopAfterARefusedWake) {
    // An earlier handler is posted to a strand while the host refuses `post`, leaving the strand locked in `io_` with
    // no turn scheduled on the host. When a pool tool bound to the same strand finishes after the host accepts posts
    // again, `asio::post` to the already-locked strand queues inside the strand without calling
    // `PumpedExecutor::execute`; `post_handoff` must still wake the loop so both handlers run.
    auto executor = std::make_shared<ManualExecutor>();
    std::atomic<bool> refuse{false};
    std::atomic<int> refused{0};
    executor->before_post = [&]() -> bool {
        if (!refuse.load()) return false;
        ++refused;
        return true;
    };
    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe probe;
    std::string result;
    bool earlier_ran = false, completed = false;
    {
        async::HostLoop loop(executor);
        auto strand = asio::make_strand(loop.executor());
        asio::co_spawn(strand, await_a_pool_tool(&tool, &probe, &result),
                       [&](std::exception_ptr error) { completed = !error; });
        loop.wake();
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
        ASSERT_EQ(executor->pending(), 0u);

        refuse = true;
        asio::post(strand, [&] { earlier_ran = true; });
        EXPECT_GE(refused.load(), 1);
        EXPECT_EQ(executor->pending(), 0u) << "the wake for the strand's first handler was refused";

        refuse = false;
        tool.release = true;
        let_the_pool_thread_post();
        EXPECT_GT(executor->pending(), 0u) << "post_handoff scheduled a turn even though the strand was already queued";
        ASSERT_TRUE(executor->run_until([&] { return completed; }));
        EXPECT_TRUE(earlier_ran);
        EXPECT_EQ(result, "late");
    }
    executor->before_post = nullptr;
}

TEST(HostLoopPoolWork, ARefusedPostOfThePoolThreadAsksAFewTimesAndTheNextWakeDeliversTheResult) {
    // The host refuses every post that the pool thread makes. The wake that follows the pool thread's post asks as often
    // as any wake does and then gives up, and the result stays queued in the loop until the next wake, which delivers it.
    auto executor = std::make_shared<ManualExecutor>();
    const auto host_thread = std::this_thread::get_id();
    std::atomic<bool> refuse{true};
    std::atomic<int> refused{0};
    executor->before_post = [&]() -> bool {
        if (std::this_thread::get_id() == host_thread || !refuse.load()) return false;
        ++refused;
        return true;
    };
    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe probe;
    std::string result;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_pool_tool(&tool, &probe, &result), [&](std::exception_ptr error) { completed = !error; });
        release.arm();
        ASSERT_TRUE(executor->run_until([&] { return tool.entered.load(); }));
        tool.release = true;
        ASSERT_TRUE(executor->run_until([&] { return tool.finished.load() && refused.load() > 0; }));
        let_the_pool_thread_post();
        EXPECT_EQ(executor->pending(), 0u) << "the host took nothing";
        EXPECT_GE(refused.load(), 1);
        EXPECT_LE(refused.load(), 3) << "a wake asks the host a few times at most";
        EXPECT_FALSE(completed);
        refuse = false;
        loop.wake();
        ASSERT_TRUE(executor->run_until([&] { return completed; }));
        EXPECT_EQ(result, "late");
    }
    executor->before_post = nullptr;
}

namespace {
// A synchronous checkpoint store whose save waits for the test: the engine's async form of it runs on the pool.
class GatedStore final : public graph::CheckpointStore {
public:
    void save(const graph::Checkpoint&) override {
        entered = true;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (!release.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        finished = true;
    }
    std::atomic<bool> entered{false}, release{false}, finished{false};
};

asio::awaitable<void> save_a_checkpoint(GatedStore* store, FrameProbe* probe) {
    FrameSentinel sentinel(probe);
    co_await store->save_async(graph::Checkpoint{});
}
}  // namespace

TEST(HostLoopPoolWork, ALoopDestroyedWhileASynchronousCheckpointStoreSavesTakesItsCoroutine) {
    auto executor = std::make_shared<ManualExecutor>();
    GatedStore store;
    FrameProbe probe;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(save_a_checkpoint(&store, &probe), [&](std::exception_ptr) { completed = true; });
        ASSERT_TRUE(executor->run_until([&] { return store.entered.load(); }));
    }
    EXPECT_EQ(probe.destroyed.load(), 1);
    EXPECT_EQ(probe.destroyed_on_host.load(), 1);
    const auto queued = executor->pending();
    store.release = true;
    ASSERT_TRUE(executor->run_until([&] { return store.finished.load(); }));
    let_the_pool_thread_post();
    EXPECT_FALSE(completed);
    EXPECT_EQ(executor->pending(), queued);
    while (executor->step()) {}
}

TEST(HostLoopPoolWork, ASynchronousCheckpointStoreThatSavesBeforeTheLoopIsDestroyedIsDelivered) {
    auto executor = std::make_shared<ManualExecutor>();
    GatedStore store;
    FrameProbe probe;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(save_a_checkpoint(&store, &probe), [&](std::exception_ptr error) { completed = !error; });
        ASSERT_TRUE(executor->run_until([&] { return store.entered.load(); }));
        store.release = true;
        ASSERT_TRUE(executor->run_until([&] { return completed; }));
        let_the_pool_thread_post();
    }
    EXPECT_TRUE(completed);
}

namespace {
// A tool that finishes at once, or after a short random spin, on the pool.
class QuickTool final : public Tool {
public:
    ChatTool get_definition() const override { return ChatTool{"quick", "finishes soon", json{{"type", "object"}}}; }
    std::string execute(const json&) override {
        if (hold.load()) {
            // Held: the call stays on the pool thread until the test releases it.
            holding = true;
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (hold.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        }
        const unsigned spin = ++seed_ * 2654435761u >> 24;  // 0..255
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
        while (std::chrono::steady_clock::now() < until) {}
        ++finished;
        return "quick";
    }
    std::string get_name() const override { return "quick"; }
    std::atomic<int> finished{0};
    std::atomic<bool> hold{false}, holding{false};

private:
    std::atomic<unsigned> seed_{1};
};

asio::awaitable<void> call_quick_tools(Tool* tool, FrameProbe* probe, int calls, std::atomic<int>* submitted,
                                       std::atomic<int>* delivered) {
    FrameSentinel sentinel(probe);
    for (int i = 0; i < calls; ++i) {
        ++*submitted;
        co_await tool->execute_async(json::object());
        ++*delivered;
    }
}
}  // namespace

TEST(HostLoopPoolWork, LoopsDestroyedWhilePoolToolsCompleteLeaveNothingBehind) {
    QuickTool tool;
    const unsigned rounds = 80;
    std::atomic<int> submitted_total{0}, delivered_total{0};
    int frames_made = 0, frames_destroyed = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        auto executor = std::make_shared<ManualExecutor>();
        FrameProbe probe;
        std::atomic<int> submitted{0}, delivered{0};
        int pumped_steps = static_cast<int>(round % 9);
        {
            async::HostLoop loop(executor);
            for (int run = 0; run < 4; ++run)
                loop.spawn(call_quick_tools(&tool, &probe, 3, &submitted, &delivered), [](std::exception_ptr) {});
            for (int step = 0; step < pumped_steps; ++step) {
                if (!executor->step()) std::this_thread::sleep_for(20us);
            }
        }
        // Every call that was handed to the pool has run to its end: its result was delivered or dropped.
        const int handed = submitted.load();
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (tool.finished.load() < submitted_total.load() + handed && std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(100us);
        submitted_total += handed;
        delivered_total += delivered.load();
        frames_made += probe.started.load();
        frames_destroyed += probe.destroyed.load();
        EXPECT_EQ(probe.destroyed.load(), probe.started.load()) << "round " << round << ": a frame was left behind";
        EXPECT_EQ(probe.destroyed_on_host.load(), probe.destroyed.load()) << "round " << round;
        while (executor->step()) {}
    }
    // Whether a round above drops a result depends on how far the pumping got when its loop died. In this last
    // round the call is still running on the pool when the loop is destroyed, so its result is always dropped.
    {
        auto executor = std::make_shared<ManualExecutor>();
        FrameProbe probe;
        std::atomic<int> submitted{0}, delivered{0};
        // No call of an earlier round may still be on its way to the pool when the gate closes.
        ASSERT_EQ(tool.finished.load(), submitted_total.load());
        let_the_pool_thread_post();
        tool.hold = true;
        {
            async::HostLoop loop(executor);
            loop.spawn(call_quick_tools(&tool, &probe, 1, &submitted, &delivered), [](std::exception_ptr) {});
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (!tool.holding.load() && std::chrono::steady_clock::now() < limit) {
                if (!executor->step()) std::this_thread::sleep_for(20us);
            }
            ASSERT_TRUE(tool.holding.load()) << "the held call did not reach the pool";
        }
        tool.hold = false;
        const auto limit = std::chrono::steady_clock::now() + 30s;
        while (tool.finished.load() < submitted_total.load() + submitted.load() &&
               std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(100us);
        let_the_pool_thread_post();
        while (executor->step()) {}
        EXPECT_EQ(submitted.load(), 1);
        EXPECT_EQ(delivered.load(), 0) << "the loop died before the held call finished";
        EXPECT_EQ(probe.destroyed.load(), probe.started.load()) << "the held call's frame was left behind";
        EXPECT_EQ(probe.destroyed_on_host.load(), probe.destroyed.load());
        submitted_total += submitted.load();
        delivered_total += delivered.load();
        frames_made += probe.started.load();
        frames_destroyed += probe.destroyed.load();
    }
    let_the_pool_thread_post();
    EXPECT_EQ(tool.finished.load(), submitted_total.load());
    EXPECT_EQ(frames_destroyed, frames_made);
    EXPECT_LT(delivered_total.load(), submitted_total.load()) << "some results were dropped with their loops";
}

namespace {
class SequencedGatedTool final : public Tool {
public:
    ChatTool get_definition() const override { return ChatTool{"gated", "two calls", json{{"type", "object"}}}; }
    std::string execute(const json&) override {
        const int call = ++calls;
        const int active = ++in_flight;
        int prev_max = max_in_flight.load();
        while (active > prev_max && !max_in_flight.compare_exchange_weak(prev_max, active)) {}
        if (call == 1) {
            first_entered = true;
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (!release_first.load() && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
            --in_flight;
            first_finished = true;
            return "first";
        }
        second_entered = true;
        --in_flight;
        second_finished = true;
        return "second";
    }
    std::string get_name() const override { return "gated"; }

    std::atomic<int> calls{0}, in_flight{0}, max_in_flight{0};
    std::atomic<bool> first_entered{false}, release_first{false}, first_finished{false};
    std::atomic<bool> second_entered{false}, second_finished{false};
};

asio::awaitable<void> execute_via_controller(ToolExecutionController* controller, Tool* tool, FrameProbe* probe,
                                             ToolExecutionResult* out) {
    FrameSentinel sentinel(probe);
    *out = co_await controller->execute_result_async(*tool, json::object(), ToolExecutionContext{});
}
}  // namespace

TEST(HostLoopPoolWork, AnAbandonedExclusivePoolToolKeepsItsLeaseUntilThePoolWorkFinishes) {
    // When a HostLoop is destroyed while a non-reentrant pool tool is still running, the awaiting coroutine frame dies
    // on the host thread, but its ResourceLease (and HostResourceLease) stays with the pool task until the tool itself
    // returns, so a second loop cannot enter the tool concurrently.
    HostResourceProfileData profile_data;
    profile_data.profile_id = "test-profile";
    profile_data.capacity.cpu_millis = 1000;
    profile_data.evidence.source = "test";
    HostAdmissionControllerConfig admission_config;
    admission_config.profile = HostResourceProfile::create(std::move(profile_data));
    ToolExecutionControllerConfig config;
    config.host_admission = std::make_shared<HostAdmissionController>(std::move(admission_config));
    ToolExecutionController controller(std::move(config));

    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::Exclusive;
    policy.capacity = 1;
    policy.host_resources.cpu_millis = 1000;
    controller.policies()->upsert("gated", policy);

    SequencedGatedTool tool;
    struct ReleaseFirstOnExit {
        SequencedGatedTool& tool;
        ~ReleaseFirstOnExit() {
            tool.release_first = true;
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (tool.in_flight.load() != 0 && std::chrono::steady_clock::now() < limit)
                std::this_thread::sleep_for(100us);
            let_the_pool_thread_post();
        }
    } release_first_on_exit{tool};
    auto first_host = std::make_shared<ManualExecutor>();
    auto second_host = std::make_shared<ManualExecutor>();
    FrameProbe first_probe, second_probe;
    ToolExecutionResult first_result, second_result;
    bool first_done = false, second_done = false;
    {
        async::HostLoop first_loop(first_host);
        first_loop.spawn(execute_via_controller(&controller, &tool, &first_probe, &first_result),
                         [&](std::exception_ptr) { first_done = true; });
        ASSERT_TRUE(first_host->run_until([&] { return tool.first_entered.load(); }));
    }
    EXPECT_EQ(first_probe.destroyed.load(), 1);
    EXPECT_FALSE(first_done);

    async::HostLoop second_loop(second_host);
    second_loop.spawn(execute_via_controller(&controller, &tool, &second_probe, &second_result),
                      [&](std::exception_ptr error) { second_done = !error; });
    for (int i = 0; i < 5; ++i) {
        if (!second_host->step()) std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(tool.second_entered.load()) << "the second call must wait while the abandoned pool call still runs";
    EXPECT_EQ(tool.max_in_flight.load(), 1);

    tool.release_first = true;
    ASSERT_TRUE(second_host->run_until([&] { return tool.first_finished.load(); }));
    let_the_pool_thread_post();
    ASSERT_TRUE(second_host->run_until([&] { return second_done; }));
    EXPECT_EQ(second_result.status, ToolTerminalStatus::Succeeded);
    EXPECT_EQ(second_result.output, "second");
    EXPECT_EQ(tool.max_in_flight.load(), 1);
}

TEST(HostLoopPoolWork, AnAbandonedSingleFlightLeaderPublishesAFailureToItsFollowers) {
    // Destroying the HostLoop of a SingleFlight leader while the pool tool still runs destroys the leader's coroutine
    // frame on the host thread, which publishes an ordinary failure to waiting followers instead of leaving them
    // waiting until queue_timeout.
    ToolExecutionController controller;
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::BlockingThread;
    policy.concurrency = ToolConcurrency::SingleFlight;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.effect = ToolEffectClass::ReadOnly;
    policy.queue_timeout = 30s;
    controller.policies()->upsert("gated", policy);

    GatedTool tool;
    ReleaseOnExit release(tool);
    auto leader_host = std::make_shared<ManualExecutor>();
    auto follower_host = std::make_shared<ManualExecutor>();
    FrameProbe leader_probe, same_loop_follower_probe, follower_probe;
    ToolExecutionResult leader_result, same_loop_follower_result, follower_result;
    bool same_loop_follower_done = false, follower_done = false;

    async::HostLoop follower_loop(follower_host);
    {
        async::HostLoop leader_loop(leader_host);
        leader_loop.spawn(execute_via_controller(&controller, &tool, &leader_probe, &leader_result),
                          [](std::exception_ptr) {});
        release.arm();
        ASSERT_TRUE(leader_host->run_until([&] { return tool.entered.load(); }));

        leader_loop.spawn(
            execute_via_controller(&controller, &tool, &same_loop_follower_probe, &same_loop_follower_result),
            [&](std::exception_ptr) { same_loop_follower_done = true; });
        ASSERT_TRUE(leader_host->step());
        EXPECT_FALSE(same_loop_follower_done);

        follower_loop.spawn(execute_via_controller(&controller, &tool, &follower_probe, &follower_result),
                            [&](std::exception_ptr error) { follower_done = !error; });
        ASSERT_TRUE(follower_host->step());
        EXPECT_FALSE(follower_done);
    }
    EXPECT_EQ(leader_probe.destroyed.load(), 1);
    EXPECT_EQ(same_loop_follower_probe.destroyed.load(), 1);
    EXPECT_FALSE(same_loop_follower_done);
    ASSERT_TRUE(follower_host->run_until([&] { return follower_done; }));
    EXPECT_EQ(follower_result.status, ToolTerminalStatus::Failed);
    EXPECT_NE(follower_result.error.find("abandoned"), std::string::npos);
    EXPECT_FALSE(tool.finished.load()) << "the follower received the failure while the abandoned leader still ran";
    tool.release = true;
    let_the_pool_thread_post();
}

TEST(HostLoopPoolWork, NativeExecutorsDoNotUseLoopHandoff) {
    // Non-HostLoop executors must not open a LoopHandoff or add extra work guards around pool hops: a pool-held work
    // guard or handoff that retains a copy of an asio::strand can destroy that strand on the pool thread after the
    // caller's io_context has returned from run() and been destroyed on the caller thread (that post-asio::post
    // scheduling race is non-deterministic without an Asio hook, so this test checks directly that LoopHandoff::open
    // rejects native executors and that active_count() stays 0 across native pool hops).
    asio::io_context io;
    auto strand = asio::make_strand(io.get_executor());
    EXPECT_FALSE(async::is_host_pumped(asio::any_io_executor(io.get_executor())));
    EXPECT_FALSE(async::is_host_pumped(asio::any_io_executor(strand)));
    EXPECT_THROW(async::detail::LoopHandoff::open(asio::any_io_executor(io.get_executor()), [] {}), std::logic_error);
    EXPECT_THROW(async::detail::LoopHandoff::open(asio::any_io_executor(strand), [] {}), std::logic_error);

    GatedTool tool;
    ReleaseOnExit release(tool);
    FrameProbe tool_probe;
    std::string tool_result;
    bool tool_done = false;
    asio::co_spawn(strand, await_a_pool_tool(&tool, &tool_probe, &tool_result),
                   [&](std::exception_ptr error) { tool_done = !error; });
    release.arm();
    while (!tool.entered.load()) {
        io.poll_one();
        std::this_thread::yield();
    }
    EXPECT_EQ(async::detail::LoopHandoff::active_count(), 0u);
    tool.release = true;
    io.run();
    EXPECT_TRUE(tool_done);
    EXPECT_EQ(tool_result, "late");

    io.restart();
    GatedStore store;
    FrameProbe store_probe;
    bool store_done = false;
    asio::co_spawn(strand, save_a_checkpoint(&store, &store_probe),
                   [&](std::exception_ptr error) { store_done = !error; });
    while (!store.entered.load()) {
        io.poll_one();
        std::this_thread::yield();
    }
    EXPECT_EQ(async::detail::LoopHandoff::active_count(), 0u);
    store.release = true;
    io.run();
    EXPECT_TRUE(store_done);
}

TEST(HostLoopPoolWork, ADiscardedHandoffDestroysItsContinuationOnTheHostThread) {
    auto executor = std::make_shared<ManualExecutor>();
    FrameProbe probe;
    {
        async::HostLoop loop(executor);
        auto sentinel = std::make_shared<FrameSentinel>(&probe);
        auto handoff = async::detail::LoopHandoff::open(loop.executor(), [sentinel = std::move(sentinel)] {});
        std::thread([handoff = std::move(handoff)]() mutable { handoff.reset(); }).join();
        EXPECT_EQ(probe.destroyed.load(), 0) << "the pool thread must not destroy an unfinished continuation";
    }
    EXPECT_EQ(probe.destroyed.load(), 1);
    EXPECT_EQ(probe.destroyed_on_host.load(), 1);
}

namespace {

class DiscardedHandoffTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return ChatTool{"discarded", "drops its first handoff", json{{"type", "object"}}};
    }
    std::string execute(const json&) override { return "unused"; }
    asio::awaitable<std::string> execute_async(const json&) override {
        if (++calls == 1) {
            const auto caller_executor = co_await asio::this_coro::executor;
            co_await asio::async_initiate<decltype(asio::use_awaitable), void()>(
                [this, caller_executor](auto handler) mutable {
                    handoff = async::detail::LoopHandoff::open(
                        caller_executor, [handler = std::move(handler)]() mutable { handler(); });
                    first_entered = true;
                },
                asio::use_awaitable);
            co_return "first";
        }
        second_entered = true;
        co_return "second";
    }
    std::string get_name() const override { return "discarded"; }

    std::shared_ptr<async::detail::LoopHandoff> handoff;
    std::atomic<int> calls{0};
    std::atomic<bool> first_entered{false}, second_entered{false};
};

template <typename Executor>
class ThrowingExecutor {
public:
    ThrowingExecutor(Executor executor, std::shared_ptr<std::atomic<bool>> throw_on_execute) noexcept
        : executor_(std::move(executor)), throw_on_execute_(std::move(throw_on_execute)) {}

    template <typename Function>
    void execute(Function&& function) const {
        if (throw_on_execute_ && throw_on_execute_->load()) throw std::bad_alloc();
        executor_.execute(std::forward<Function>(function));
    }

    template <typename Property>
    auto query(const Property& property) const
        noexcept(noexcept(asio::query(std::declval<const Executor&>(), property)))
        -> decltype(asio::query(std::declval<const Executor&>(), property)) {
        return asio::query(executor_, property);
    }

    template <typename Property>
        requires asio::can_require<const Executor&, const Property&>::value
    auto require(const Property& property) const
        -> ThrowingExecutor<std::decay_t<asio::require_result_t<const Executor&, const Property&>>> {
        return {asio::require(executor_, property), throw_on_execute_};
    }

    template <typename Property>
        requires asio::can_prefer<const Executor&, const Property&>::value
    auto prefer(const Property& property) const
        -> ThrowingExecutor<std::decay_t<asio::prefer_result_t<const Executor&, const Property&>>> {
        return {asio::prefer(executor_, property), throw_on_execute_};
    }

    friend bool operator==(const ThrowingExecutor& left, const ThrowingExecutor& right) noexcept {
        return left.throw_on_execute_ == right.throw_on_execute_ && left.executor_ == right.executor_;
    }
    friend bool operator!=(const ThrowingExecutor& left, const ThrowingExecutor& right) noexcept {
        return !(left == right);
    }

private:
    Executor executor_;
    std::shared_ptr<std::atomic<bool>> throw_on_execute_;
};

}  // namespace

TEST(HostLoopPoolWork, ADiscardedHandoffOnAnIdleLoopSchedulesATurnThatDestroysTheFrameAndReleasesItsLease) {
    HostResourceProfileData profile_data;
    profile_data.profile_id = "test-profile";
    profile_data.capacity.cpu_millis = 1000;
    profile_data.evidence.source = "test";
    HostAdmissionControllerConfig admission_config;
    admission_config.profile = HostResourceProfile::create(std::move(profile_data));
    ToolExecutionControllerConfig config;
    config.host_admission = std::make_shared<HostAdmissionController>(std::move(admission_config));
    ToolExecutionController controller(std::move(config));

    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::NativeAsync;
    policy.concurrency = ToolConcurrency::Exclusive;
    policy.capacity = 1;
    policy.host_resources.cpu_millis = 1000;
    controller.policies()->upsert("discarded", policy);

    DiscardedHandoffTool tool;
    auto first_host = std::make_shared<ManualExecutor>();
    auto second_host = std::make_shared<ManualExecutor>();
    FrameProbe first_probe, second_probe;
    ToolExecutionResult first_result, second_result;
    bool first_done = false, second_done = false;

    async::HostLoop first_loop(first_host);
    first_loop.spawn(execute_via_controller(&controller, &tool, &first_probe, &first_result),
                     [&](std::exception_ptr) { first_done = true; });
    ASSERT_TRUE(first_host->run_until([&] { return tool.first_entered.load(); }));
    EXPECT_TRUE(first_loop.idle());
    EXPECT_EQ(first_host->pending(), 0u);

    async::HostLoop second_loop(second_host);
    second_loop.spawn(execute_via_controller(&controller, &tool, &second_probe, &second_result),
                      [&](std::exception_ptr error) { second_done = !error; });
    for (int i = 0; i < 5; ++i) {
        if (!second_host->step()) std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(tool.second_entered.load());

    std::thread([handoff = std::move(tool.handoff)]() mutable { handoff.reset(); }).join();
    EXPECT_EQ(first_probe.destroyed.load(), 0) << "the worker thread must not destroy the continuation";
    EXPECT_EQ(first_host->pending(), 1u) << "dropping an unfinished handoff schedules a turn on the idle loop";
    EXPECT_FALSE(first_loop.idle());

    while (first_host->step()) {}
    EXPECT_EQ(first_probe.destroyed.load(), 1);
    EXPECT_EQ(first_probe.destroyed_on_host.load(), 1);
    EXPECT_FALSE(first_done);

    ASSERT_TRUE(second_host->run_until([&] { return second_done; }, 200ms));
    EXPECT_EQ(second_result.status, ToolTerminalStatus::Succeeded);
    EXPECT_EQ(second_result.output, "second");
}

TEST(HostLoopPoolWork, ADiscardedHandoffOfASingleFlightLeaderOnAnIdleLoopPublishesAFailureToItsFollowers) {
    ToolExecutionController controller;
    ToolExecutionPolicy policy;
    policy.implementation = ToolExecutionImplementation::NativeAsync;
    policy.concurrency = ToolConcurrency::SingleFlight;
    policy.idempotency = ToolIdempotency::Idempotent;
    policy.effect = ToolEffectClass::ReadOnly;
    policy.queue_timeout = 30s;
    controller.policies()->upsert("discarded", policy);

    DiscardedHandoffTool tool;
    auto leader_host = std::make_shared<ManualExecutor>();
    auto follower_host = std::make_shared<ManualExecutor>();
    FrameProbe leader_probe, follower_probe;
    ToolExecutionResult leader_result, follower_result;
    bool leader_done = false, follower_done = false;

    async::HostLoop leader_loop(leader_host);
    async::HostLoop follower_loop(follower_host);
    leader_loop.spawn(execute_via_controller(&controller, &tool, &leader_probe, &leader_result),
                      [&](std::exception_ptr) { leader_done = true; });
    ASSERT_TRUE(leader_host->run_until([&] { return tool.first_entered.load(); }));
    EXPECT_TRUE(leader_loop.idle());
    EXPECT_EQ(leader_host->pending(), 0u);

    follower_loop.spawn(execute_via_controller(&controller, &tool, &follower_probe, &follower_result),
                        [&](std::exception_ptr error) { follower_done = !error; });
    ASSERT_TRUE(follower_host->step());
    EXPECT_FALSE(follower_done);

    std::thread([handoff = std::move(tool.handoff)]() mutable { handoff.reset(); }).join();
    EXPECT_EQ(leader_probe.destroyed.load(), 0);
    EXPECT_EQ(leader_host->pending(), 1u) << "dropping the leader's handoff schedules a turn on the idle loop";

    while (leader_host->step()) {}
    EXPECT_EQ(leader_probe.destroyed.load(), 1);
    EXPECT_EQ(leader_probe.destroyed_on_host.load(), 1);
    EXPECT_FALSE(leader_done);

    ASSERT_TRUE(follower_host->run_until([&] { return follower_done; }, 200ms));
    EXPECT_EQ(follower_result.status, ToolTerminalStatus::Failed);
    EXPECT_NE(follower_result.error.find("abandoned"), std::string::npos);
}

TEST(HostLoopPoolWork, AFailedHandoffPostAndADiscardedHandoffWakeTheLoopAndWaitOnClose) {
    {
        auto executor = std::make_shared<ManualExecutor>();
        async::HostLoop loop(executor);
        FrameProbe probe;
        auto throw_on_execute = std::make_shared<std::atomic<bool>>(true);
        asio::any_io_executor throwing(ThrowingExecutor(loop.executor(), throw_on_execute));
        auto sentinel = std::make_shared<FrameSentinel>(&probe);
        auto handoff = async::detail::LoopHandoff::open(throwing, [sentinel = std::move(sentinel)] {});
        EXPECT_TRUE(loop.idle());
        EXPECT_EQ(executor->pending(), 0u);

        std::thread([handoff = std::move(handoff)]() mutable {
            handoff->finish();
            handoff.reset();
        }).join();
        EXPECT_EQ(probe.destroyed.load(), 0) << "a failed post must not destroy the continuation on the pool thread";
        EXPECT_EQ(executor->pending(), 1u) << "a failed post schedules a turn to destroy the abandoned continuation";
        while (executor->step()) {}
        EXPECT_EQ(probe.destroyed.load(), 1);
        EXPECT_EQ(probe.destroyed_on_host.load(), 1);
    }

    {
        auto executor = std::make_shared<ManualExecutor>();
        const auto host_thread = std::this_thread::get_id();
        FrameProbe probe;
        std::atomic<bool> in_post{false}, reset_returned{false}, wake_returned{false}, destructor_returned{false};
        std::atomic<int> posts_after_destroy{0};
        executor->before_post = [&]() -> bool {
            if (destructor_returned.load()) {
                ++posts_after_destroy;
                return true;
            }
            if (std::this_thread::get_id() == host_thread) return false;
            in_post = true;
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (probe.destroyed.load() < 1 && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
            std::this_thread::sleep_for(5ms);
            wake_returned = true;
            return true;
        };
        std::thread dropper;
        {
            async::HostLoop loop(executor);
            auto sentinel = std::make_shared<FrameSentinel>(&probe);
            auto handoff = async::detail::LoopHandoff::open(loop.executor(), [sentinel = std::move(sentinel)] {});
            dropper = std::thread([handoff = std::move(handoff), &reset_returned]() mutable {
                handoff.reset();
                reset_returned = true;
            });
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (!in_post.load() && !reset_returned.load() && std::chrono::steady_clock::now() < limit) {
                std::this_thread::yield();
            }
            EXPECT_TRUE(in_post.load());
            if (in_post.load()) loop.wake();
        }
        destructor_returned = true;
        dropper.join();
        EXPECT_TRUE(wake_returned.load()) << "~HostLoop() waited for the discarded handoff's in-progress wake";
        EXPECT_EQ(probe.destroyed.load(), 1);
        EXPECT_EQ(probe.destroyed_on_host.load(), 1);
        EXPECT_EQ(posts_after_destroy.load(), 0);
        executor->before_post = nullptr;
        while (executor->step()) {}
    }
}

namespace {

// Forwards every sp::runtime::Executor call to `inner` and records which thread ran the destructor.
class DestructionTrackingExecutor final : public sp::runtime::Executor {
public:
    DestructionTrackingExecutor(ManualExecutor* inner, FrameProbe* probe) noexcept : inner_(inner), probe_(probe) {
        ++probe_->started;
    }

    ~DestructionTrackingExecutor() override {
        ++probe_->destroyed;
        if (std::this_thread::get_id() == probe_->host) ++probe_->destroyed_on_host;
    }

    void post(Task task) override { inner_->post(std::move(task)); }
    Timer schedule(sp::runtime::SteadyTime at, Task task) override { return inner_->schedule(at, std::move(task)); }
    void cancel(Timer timer) noexcept override { inner_->cancel(timer); }
    sp::runtime::SteadyTime now() const noexcept override { return inner_->now(); }
    sp::runtime::WallTime wall_now() const noexcept override { return inner_->wall_now(); }
    bool in_thread() const noexcept override { return inner_->in_thread(); }
    bool host_pumped() const noexcept override { return inner_->host_pumped(); }
    void shutdown() noexcept override { inner_->shutdown(); }

private:
    ManualExecutor* inner_;
    FrameProbe* probe_;
};

}  // namespace

TEST(HostLoopPoolWork, TheHostExecutorIsDestroyedOnTheHostThreadWhenTheLoopIsDestroyedWhileABlockingToolRuns) {
    ManualExecutor driver;
    FrameProbe executor_probe, on_error_probe, frame_probe;
    GatedTool tool;
    ReleaseOnExit release(tool);
    {
        auto executor = std::make_shared<DestructionTrackingExecutor>(&driver, &executor_probe);
        auto on_error_sentinel = std::make_shared<FrameSentinel>(&on_error_probe);
        async::HostLoop::Options options;
        options.on_error = [on_error_sentinel = std::move(on_error_sentinel)](std::exception_ptr) {};
        async::HostLoop loop(std::move(executor), std::move(options));
        loop.spawn(await_a_pool_tool(&tool, &frame_probe), [](std::exception_ptr) {});
        release.arm();
        ASSERT_TRUE(driver.run_until([&] { return tool.entered.load(); }));
        EXPECT_EQ(executor_probe.destroyed.load(), 0);
        EXPECT_EQ(on_error_probe.destroyed.load(), 0);
    }
    const int destroyed_by_loop = executor_probe.destroyed.load();
    const int on_error_destroyed_by_loop = on_error_probe.destroyed.load();
    tool.release = true;
    ASSERT_TRUE(driver.run_until([&] { return tool.finished.load(); }));
    let_the_pool_thread_post();
    EXPECT_EQ(destroyed_by_loop, 1) << "~HostLoop() released the host executor before returning";
    EXPECT_EQ(on_error_destroyed_by_loop, 1) << "~HostLoop() released Options::on_error before returning";
    EXPECT_EQ(executor_probe.destroyed.load(), 1);
    EXPECT_EQ(executor_probe.destroyed_on_host.load(), 1)
        << "the host executor was destroyed on the host thread, never on the pool thread";
    EXPECT_EQ(on_error_probe.destroyed.load(), 1);
    EXPECT_EQ(on_error_probe.destroyed_on_host.load(), 1);
    EXPECT_EQ(frame_probe.destroyed.load(), 1);
    EXPECT_EQ(frame_probe.destroyed_on_host.load(), 1);
    while (driver.step()) {}
}

#if defined(__unix__) || defined(__APPLE__)
namespace {
// Removes the files that synchronize the process tool, on every exit from the test.
struct FlagFile {
    std::string path;
    std::string ready_path;
    FlagFile() {
        char name[] = "/tmp/neograph-pool-flag-XXXXXX";
        const int fd = ::mkstemp(name);
        if (fd >= 0) ::close(fd);
        path = name;
        ready_path = path + ".ready";
        std::remove(path.c_str());  // the process waits for it to exist
        std::remove(ready_path.c_str());
    }
    void create() const { std::ofstream(path) << "go"; }
    bool is_ready() const { return std::ifstream(ready_path).good(); }
    ~FlagFile() {
        create();  // a process still waiting ends
        let_the_pool_thread_post();
        std::remove(path.c_str());
        std::remove(ready_path.c_str());
    }
};

// A process tool that signals `ready_path` once the child shell is running and waits until `path` exists.
class WaitsForAFileTool final : public Tool, public ProcessTool {
public:
    explicit WaitsForAFileTool(const FlagFile& flag) : path_(flag.path), ready_path_(flag.ready_path) {}
    ChatTool get_definition() const override { return ChatTool{"waits", "waits for a file", json{{"type", "object"}}}; }
    std::string execute(const json&) override { return "sync-not-used"; }
    std::string get_name() const override { return "waits"; }
    ToolProcessSpec prepare_process(const json&, const ToolExecutionContext&) const override {
        started = true;
        ToolProcessSpec spec;
        spec.argv = {"/bin/sh", "-c",
                     ": > \"$2\"; while [ ! -e \"$1\" ]; do sleep 0.01; done; printf late",
                     "sh", path_, ready_path_};
        return spec;
    }
    mutable std::atomic<bool> started{false};

private:
    std::string path_;
    std::string ready_path_;
};

asio::awaitable<void> await_a_process_tool(WaitsForAFileTool* tool, FrameProbe* probe, std::string* result) {
    FrameSentinel sentinel(probe);
    *result = co_await detail::execute_process_tool_async(*tool, json::object(), ToolExecutionContext{}, 4096, 30s);
}
}  // namespace

TEST(HostLoopPoolWork, ALoopDestroyedWhileAProcessToolRunsTakesItsCoroutineAndDropsTheLateResult) {
    auto executor = std::make_shared<ManualExecutor>();
    FlagFile flag;
    WaitsForAFileTool tool(flag);
    FrameProbe probe;
    std::string result;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_process_tool(&tool, &probe, &result), [&](std::exception_ptr) { completed = true; });
        ASSERT_TRUE(executor->run_until([&] { return flag.is_ready(); }));
    }
    EXPECT_EQ(probe.destroyed.load(), 1);
    EXPECT_EQ(probe.destroyed_on_host.load(), 1);
    const auto queued = executor->pending();
    flag.create();
    let_the_pool_thread_post();  // the process ends and its pool thread finishes LoopHandoff::finish()
    EXPECT_FALSE(completed);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(executor->pending(), queued);
    while (executor->step()) {}
    EXPECT_FALSE(completed);
}

TEST(HostLoopPoolWork, AProcessToolThatEndsBeforeTheLoopIsDestroyedIsDelivered) {
    auto executor = std::make_shared<ManualExecutor>();
    FlagFile flag;
    WaitsForAFileTool tool(flag);
    FrameProbe probe;
    std::string result;
    bool completed = false;
    {
        async::HostLoop loop(executor);
        loop.spawn(await_a_process_tool(&tool, &probe, &result), [&](std::exception_ptr error) { completed = !error; });
        ASSERT_TRUE(executor->run_until([&] { return flag.is_ready(); }));
        flag.create();
        ASSERT_TRUE(executor->run_until([&] { return completed; }));
        let_the_pool_thread_post();
    }
    EXPECT_NE(result.find("late"), std::string::npos);

    asio::io_context io;
    auto strand = asio::make_strand(io.get_executor());
    FlagFile native_flag;
    WaitsForAFileTool native_tool(native_flag);
    FrameProbe native_probe;
    std::string native_result;
    bool native_done = false;
    asio::co_spawn(strand, await_a_process_tool(&native_tool, &native_probe, &native_result),
                   [&](std::exception_ptr error) { native_done = !error; });
    while (!native_flag.is_ready()) {
        io.poll_one();
        std::this_thread::yield();
    }
    EXPECT_EQ(async::detail::LoopHandoff::active_count(), 0u);
    native_flag.create();
    io.run();
    EXPECT_TRUE(native_done);
    EXPECT_NE(native_result.find("late"), std::string::npos);
}
#endif
