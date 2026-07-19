// Issue #9 — parity coverage for the C++ OpenInference layer.
//
// Mirrors the assertions in `bindings/python/tests/test_openinference.py`:
//   - openinference_tracer opens a CHAIN root + per-node CHAIN children
//   - OpenInferenceProvider opens an LLM-kind child span carrying the
//     full LLM attribute set (model, params, in/out messages, usage)
//   - Streaming overloads append per-token events on the LLM span
//   - Provider exceptions surface ERROR status on the span and re-raise
//
// The Tracer adapter under test is an in-memory recorder (no
// opentelemetry-cpp dependency).

#include <gtest/gtest.h>

#include <neograph/observability/openinference.h>
#include <neograph/observability/tracer.h>
#include <neograph/async/run_sync.h>
#include <neograph/provider.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using neograph::ChatCompletion;
using neograph::ChatMessage;
using neograph::CompletionParams;
using neograph::Provider;
using neograph::StreamCallback;
using neograph::graph::GraphEvent;

namespace obs = neograph::observability;

// ---------------------------------------------------------------------------
// In-memory tracer adapter.
// ---------------------------------------------------------------------------

namespace {

struct RecordedEvent {
    std::string name;
    std::string payload;
};

struct RecordedSpan {
    std::string name;
    obs::Span* parent = nullptr;
    std::unordered_map<std::string, std::string> attrs_str;
    std::unordered_map<std::string, int64_t> attrs_int;
    std::unordered_map<std::string, bool> attrs_bool;
    std::vector<RecordedEvent> events;
    std::string status = "unset";  // "unset" | "ok" | "error"
    std::string status_message;
    bool ended = false;
    int end_calls = 0;
};

class InMemorySpan : public obs::Span {
public:
    explicit InMemorySpan(RecordedSpan* rec) : rec_(rec) {}

    void set_attribute(std::string_view k, std::string_view v) override {
        rec_->attrs_str[std::string(k)] = std::string(v);
    }
    void set_attribute(std::string_view k, int64_t v) override {
        rec_->attrs_int[std::string(k)] = v;
    }
    void set_attribute(std::string_view k, double v) override {
        rec_->attrs_str[std::string(k)] = std::to_string(v);
    }
    void set_attribute_bool(std::string_view k, bool v) override {
        rec_->attrs_bool[std::string(k)] = v;
    }
    void add_event(std::string_view name, std::string_view payload) override {
        rec_->events.push_back({std::string(name), std::string(payload)});
    }
    void set_status_ok() override { rec_->status = "ok"; }
    void set_status_error(std::string_view msg) override {
        rec_->status = "error";
        rec_->status_message = std::string(msg);
    }
    void end() override {
        rec_->ended = true;
        ++rec_->end_calls;
    }

private:
    RecordedSpan* rec_;
};

class InMemoryTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span* parent) override {
        std::lock_guard<std::mutex> lock(mu_);
        spans_.push_back(std::make_unique<RecordedSpan>());
        spans_.back()->name = std::string(name);
        spans_.back()->parent = parent;
        // Map the underlying record to its handle so the wrapper Span
        // returned to the caller stays in sync with the recorded one.
        return std::make_unique<InMemorySpan>(spans_.back().get());
    }

    std::vector<RecordedSpan*> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<RecordedSpan*> out;
        out.reserve(spans_.size());
        for (const auto& s : spans_) out.push_back(s.get());
        return out;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<RecordedSpan>> spans_;
};

// ---------------------------------------------------------------------------
// Fake provider — records calls, emits configurable response.
// ---------------------------------------------------------------------------

class FakeProvider : public Provider {
public:
    std::string reply = "ok";
    int prompt_tokens = 7;
    int completion_tokens = 3;
    std::vector<std::string> stream_chunks;
    std::atomic<int> calls{0};

    ChatCompletion complete(const CompletionParams& /*p*/) override {
        ++calls;
        return make_completion();
    }

    ChatCompletion complete_stream(const CompletionParams& /*p*/,
                                   const StreamCallback& on_chunk) override {
        ++calls;
        std::string acc;
        for (const auto& c : stream_chunks) {
            acc += c;
            if (on_chunk) on_chunk(c);
        }
        if (acc.empty()) acc = reply;
        auto comp = make_completion();
        comp.message.content = acc;
        return comp;
    }

    std::string get_name() const override { return "fake"; }

private:
    ChatCompletion make_completion() const {
        ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = reply;
        c.usage.prompt_tokens = prompt_tokens;
        c.usage.completion_tokens = completion_tokens;
        c.usage.total_tokens = prompt_tokens + completion_tokens;
        return c;
    }
};

class ThrowingProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams&) override {
        throw std::runtime_error("boom");
    }
    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback&) override {
        return {};
    }
    std::string get_name() const override { return "throw"; }
};

class ThrowingStartTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view,
                                          obs::Span*) override {
        throw std::runtime_error("trace start failed");
    }
};

class ThrowingSpan : public obs::Span {
public:
    explicit ThrowingSpan(std::atomic<int>& end_calls)
        : end_calls_(end_calls) {}

    void set_attribute(std::string_view, std::string_view) override {
        throw std::runtime_error("trace attribute failed");
    }
    void set_attribute(std::string_view, int64_t) override {
        throw std::runtime_error("trace attribute failed");
    }
    void set_attribute(std::string_view, double) override {
        throw std::runtime_error("trace attribute failed");
    }
    void set_attribute_bool(std::string_view, bool) override {
        throw std::runtime_error("trace attribute failed");
    }
    void add_event(std::string_view, std::string_view) override {
        throw std::runtime_error("trace event failed");
    }
    void set_status_ok() override {
        throw std::runtime_error("trace status failed");
    }
    void set_status_error(std::string_view) override {
        throw std::runtime_error("trace status failed");
    }
    void end() override {
        ++end_calls_;
        throw std::runtime_error("trace end failed");
    }

private:
    std::atomic<int>& end_calls_;
};

class ThrowingSpanTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view,
                                          obs::Span*) override {
        return std::make_unique<ThrowingSpan>(end_calls);
    }

    std::atomic<int> end_calls{0};
};

class BlockingTracer : public InMemoryTracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span* parent) override {
        if (name.starts_with("node.")) {
            std::unique_lock lock(mu);
            node_start_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_node_start; });
        }
        return InMemoryTracer::start_span(name, parent);
    }

    std::mutex mu;
    std::condition_variable cv;
    bool node_start_entered = false;
    bool release_node_start = false;
};

class NonStdAsyncProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams&) override { return {}; }

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams&) override {
        throw 42;
        co_return ChatCompletion{};
    }

    std::string get_name() const override { return "non-std-async"; }
};

class SuspendedAsyncProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams&) override { return {}; }

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams&) override {
        auto executor = co_await asio::this_coro::executor;
        {
            std::lock_guard lock(mu);
            entered = true;
        }
        cv.notify_all();
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::hours(1));
        co_await timer.async_wait(asio::use_awaitable);
        co_return ChatCompletion{};
    }

    std::string get_name() const override { return "suspended-async"; }

    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
};

} // namespace

// ---------------------------------------------------------------------------
// openinference_tracer — root + per-node spans
// ---------------------------------------------------------------------------

TEST(OpenInferenceCpp, TracerOpensChainRootAndPerNodeChild) {
    InMemoryTracer tracer;
    auto session = obs::openinference_tracer(tracer);

    GraphEvent start{GraphEvent::Type::NODE_START, "researcher", neograph::json::object()};
    GraphEvent end{GraphEvent::Type::NODE_END, "researcher",
                   neograph::json{{"summary", "found 3 hits"}}};
    session.cb(start);
    session.cb(end);
    session.close();

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 2u);

    // Root: graph.run, CHAIN, ended
    EXPECT_EQ(spans[0]->name, "graph.run");
    EXPECT_EQ(spans[0]->attrs_str["openinference.span.kind"], "CHAIN");
    EXPECT_TRUE(spans[0]->ended);

    // Node child: name "node.researcher", parent = root, CHAIN
    EXPECT_EQ(spans[1]->name, "node.researcher");
    EXPECT_NE(spans[1]->parent, nullptr);
    EXPECT_EQ(spans[1]->attrs_str["openinference.span.kind"], "CHAIN");
    EXPECT_EQ(spans[1]->attrs_str["neograph.node"], "researcher");
    EXPECT_EQ(spans[1]->attrs_str["input.mime_type"], "application/json");
    EXPECT_EQ(spans[1]->attrs_str["output.mime_type"], "application/json");
    EXPECT_EQ(spans[1]->status, "ok");
    EXPECT_TRUE(spans[1]->ended);

    // input.value JSON contains node name; output.value JSON contains the data.
    auto in_json = neograph::json::parse(spans[1]->attrs_str["input.value"]);
    EXPECT_EQ(in_json["node"].get<std::string>(), "researcher");
    auto out_json = neograph::json::parse(spans[1]->attrs_str["output.value"]);
    EXPECT_EQ(out_json["summary"].get<std::string>(), "found 3 hits");
}

TEST(OpenInferenceCpp, TracerSurfacesErrorAndInterrupt) {
    InMemoryTracer tracer;
    auto session = obs::openinference_tracer(tracer);

    session.cb({GraphEvent::Type::NODE_START, "a", neograph::json::object()});
    session.cb({GraphEvent::Type::ERROR, "a", neograph::json("kaboom")});

    session.cb({GraphEvent::Type::NODE_START, "b", neograph::json::object()});
    session.cb({GraphEvent::Type::INTERRUPT, "b", neograph::json::object()});

    session.close();

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 3u);  // root + a + b

    // a: ERROR
    EXPECT_EQ(spans[1]->name, "node.a");
    EXPECT_EQ(spans[1]->status, "error");
    EXPECT_EQ(spans[1]->status_message, "kaboom");
    EXPECT_EQ(spans[1]->attrs_str["neograph.error"], "kaboom");
    EXPECT_TRUE(spans[1]->ended);

    // b: INTERRUPT
    EXPECT_EQ(spans[2]->name, "node.b");
    EXPECT_TRUE(spans[2]->attrs_bool["neograph.interrupted"]);
    EXPECT_TRUE(spans[2]->ended);
}

TEST(OpenInferenceCpp, TracerRecordsLLMTokenAsSpanEvent) {
    InMemoryTracer tracer;
    auto session = obs::openinference_tracer(tracer);

    session.cb({GraphEvent::Type::NODE_START, "chat", neograph::json::object()});
    session.cb({GraphEvent::Type::LLM_TOKEN, "chat", neograph::json("Hello")});
    session.cb({GraphEvent::Type::LLM_TOKEN, "chat", neograph::json(" world")});
    session.cb({GraphEvent::Type::NODE_END, "chat", neograph::json::object()});

    session.close();

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 2u);
    const auto& node_span = *spans[1];
    ASSERT_EQ(node_span.events.size(), 2u);
    EXPECT_EQ(node_span.events[0].name, "llm.token");
    EXPECT_EQ(node_span.events[0].payload, "Hello");
    EXPECT_EQ(node_span.events[1].payload, " world");
}

TEST(OpenInferenceCpp, TracerCloseAlsoEndsStragglerNodeSpan) {
    InMemoryTracer tracer;
    auto session = obs::openinference_tracer(tracer);

    session.cb({GraphEvent::Type::NODE_START, "a", neograph::json::object()});
    // No NODE_END before close — close() must still end the open span.
    session.close();

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_TRUE(spans[0]->ended);  // root
    EXPECT_TRUE(spans[1]->ended);  // straggler node span
}

TEST(OpenInferenceCpp, TerminalEventsRestorePreviousCurrentParent) {
    for (auto terminal : {GraphEvent::Type::NODE_END,
                          GraphEvent::Type::ERROR,
                          GraphEvent::Type::INTERRUPT}) {
        InMemoryTracer tracer;
        auto session = obs::openinference_tracer(tracer);
        auto* root = session.current_parent();
        ASSERT_NE(root, nullptr);

        session.cb({GraphEvent::Type::NODE_START, "outer",
                    neograph::json::object()});
        auto* outer = session.current_parent();
        ASSERT_NE(outer, root);
        session.cb({GraphEvent::Type::NODE_START, "inner",
                    neograph::json::object()});
        ASSERT_NE(session.current_parent(), outer);

        neograph::json data = terminal == GraphEvent::Type::ERROR
            ? neograph::json("boom") : neograph::json::object();
        session.cb({terminal, "inner", std::move(data)});
        EXPECT_EQ(session.current_parent(), outer);

        session.cb({GraphEvent::Type::NODE_END, "outer",
                    neograph::json::object()});
        EXPECT_EQ(session.current_parent(), root);
    }
}

TEST(OpenInferenceCpp, CopiedCallbackIsSafeAfterSessionDestruction) {
    InMemoryTracer tracer;
    neograph::graph::GraphStreamCallback copied;
    {
        auto session = obs::openinference_tracer(tracer);
        copied = session.cb;
    }

    EXPECT_NO_THROW(copied({GraphEvent::Type::NODE_START, "late",
                            neograph::json::object()}));
    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
}

TEST(OpenInferenceCpp, CopiedCallbackIsSafeAfterExplicitClose) {
    InMemoryTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    auto copied = session.cb;
    session.close();

    EXPECT_NO_THROW(copied({GraphEvent::Type::NODE_START, "late",
                            neograph::json::object()}));
    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0]->end_calls, 1);
}

TEST(OpenInferenceCpp, CloseWaitsForInFlightCallbackAndEndsItsSpan) {
    using namespace std::chrono_literals;

    BlockingTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    auto copied = session.cb;
    std::thread callback_thread([&] {
        copied({GraphEvent::Type::NODE_START, "blocked",
                neograph::json::object()});
    });

    {
        std::unique_lock lock(tracer.mu);
        ASSERT_TRUE(tracer.cv.wait_for(lock, 2s, [&] {
            return tracer.node_start_entered;
        }));
    }

    auto closing = std::async(std::launch::async, [&] { session.close(); });
    EXPECT_EQ(closing.wait_for(50ms), std::future_status::timeout);

    {
        std::lock_guard lock(tracer.mu);
        tracer.release_node_start = true;
    }
    tracer.cv.notify_all();
    callback_thread.join();
    ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_TRUE(spans[1]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
    EXPECT_EQ(spans[1]->end_calls, 1);
}

TEST(OpenInferenceCpp, MoveAssignmentClosesTargetSession) {
    InMemoryTracer target_tracer;
    InMemoryTracer source_tracer;
    auto target = obs::openinference_tracer(target_tracer);
    target.cb({GraphEvent::Type::NODE_START, "open",
               neograph::json::object()});
    auto source = obs::openinference_tracer(source_tracer);

    target = std::move(source);

    auto old_spans = target_tracer.snapshot();
    ASSERT_EQ(old_spans.size(), 2u);
    EXPECT_TRUE(old_spans[0]->ended);
    EXPECT_TRUE(old_spans[1]->ended);
    EXPECT_EQ(old_spans[0]->end_calls, 1);
    EXPECT_EQ(old_spans[1]->end_calls, 1);
}

// ---------------------------------------------------------------------------
// OpenInferenceProvider — LLM span attributes
// ---------------------------------------------------------------------------

TEST(OpenInferenceCpp, ProviderEmitsLLMSpanWithFullAttributes) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    inner->reply = "hello world";
    inner->prompt_tokens = 7;
    inner->completion_tokens = 3;
    obs::OpenInferenceProvider wrapped(inner, tracer);

    CompletionParams params;
    params.model = "fake-model-1";
    params.temperature = 0.5f;
    params.max_tokens = 100;
    params.messages.push_back({"system", "be helpful"});
    params.messages.push_back({"user", "hi"});

    auto result = wrapped.complete(params);
    EXPECT_EQ(result.message.content, "hello world");

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    const auto& s = *spans[0];
    EXPECT_EQ(s.name, "llm.complete");
    EXPECT_EQ(s.attrs_str.at("openinference.span.kind"), "LLM");
    EXPECT_EQ(s.attrs_str.at("llm.model_name"), "fake-model-1");
    EXPECT_EQ(s.attrs_str.at("llm.input_messages.0.message.role"), "system");
    EXPECT_EQ(s.attrs_str.at("llm.input_messages.0.message.content"), "be helpful");
    EXPECT_EQ(s.attrs_str.at("llm.input_messages.1.message.role"), "user");
    EXPECT_EQ(s.attrs_str.at("llm.input_messages.1.message.content"), "hi");
    EXPECT_EQ(s.attrs_str.at("llm.output_messages.0.message.role"), "assistant");
    EXPECT_EQ(s.attrs_str.at("llm.output_messages.0.message.content"), "hello world");
    EXPECT_EQ(s.attrs_int.at("llm.token_count.prompt"), 7);
    EXPECT_EQ(s.attrs_int.at("llm.token_count.completion"), 3);
    EXPECT_EQ(s.attrs_int.at("llm.token_count.total"), 10);
    EXPECT_EQ(s.attrs_str.at("input.mime_type"), "application/json");
    EXPECT_EQ(s.attrs_str.at("output.mime_type"), "text/plain");
    EXPECT_EQ(s.status, "ok");
    EXPECT_TRUE(s.ended);
}

TEST(OpenInferenceCpp, ProviderStreamAppendsPerTokenEvents) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    inner->stream_chunks = {"foo", "bar", "baz"};
    obs::OpenInferenceProvider wrapped(inner, tracer);

    CompletionParams params;
    params.model = "fake-stream";
    params.messages.push_back({"user", "hi"});

    std::vector<std::string> user_received;
    auto result = wrapped.complete_stream(
        params, [&](const std::string& c) { user_received.push_back(c); });

    EXPECT_EQ(result.message.content, "foobarbaz");
    EXPECT_EQ(user_received, (std::vector<std::string>{"foo", "bar", "baz"}));

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    const auto& s = *spans[0];
    EXPECT_EQ(s.events.size(), 3u);
    EXPECT_EQ(s.events[0].name, "llm.token");
    EXPECT_EQ(s.events[0].payload, "foo");
    EXPECT_EQ(s.events[2].payload, "baz");
    EXPECT_EQ(s.attrs_str.at("output.value"), "foobarbaz");
    EXPECT_TRUE(s.ended);
}

TEST(OpenInferenceCpp, ProviderPropagatesExceptionAndMarksSpanError) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    CompletionParams params;
    params.model = "x";
    params.messages.push_back({"user", "hi"});

    EXPECT_THROW(wrapped.complete(params), std::runtime_error);

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0]->status, "error");
    EXPECT_NE(spans[0]->status_message.find("boom"), std::string::npos);
    EXPECT_TRUE(spans[0]->ended);
}

TEST(OpenInferenceCpp, ProviderRejectsNullInnerProvider) {
    InMemoryTracer tracer;
    EXPECT_THROW(obs::OpenInferenceProvider(nullptr, tracer),
                 std::invalid_argument);
}

TEST(OpenInferenceCpp, ParentLookupFailureDoesNotBlockInnerCall) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(
        inner, tracer, []() -> obs::Span* {
            throw std::runtime_error("parent lookup failed");
        });

    auto result = wrapped.complete({});
    EXPECT_EQ(result.message.content, "ok");
    EXPECT_EQ(inner->calls.load(), 1);

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0]->parent, nullptr);
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
}

TEST(OpenInferenceCpp, StartSpanFailureDoesNotBlockInnerCall) {
    ThrowingStartTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    auto result = wrapped.complete({});
    EXPECT_EQ(result.message.content, "ok");
    EXPECT_EQ(inner->calls.load(), 1);
}

TEST(OpenInferenceCpp, TracingFailureDoesNotReplaceInnerException) {
    ThrowingStartTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    try {
        (void)wrapped.complete({});
        FAIL() << "inner provider exception was not propagated";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "boom");
    }
}

TEST(OpenInferenceCpp, SpanMethodFailuresDoNotAffectInnerCall) {
    ThrowingSpanTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    auto result = wrapped.complete({});

    EXPECT_EQ(result.message.content, "ok");
    EXPECT_EQ(inner->calls.load(), 1);
    EXPECT_EQ(tracer.end_calls.load(), 1);
}

TEST(OpenInferenceCpp, SpanMethodFailuresDoNotReplaceInnerException) {
    ThrowingSpanTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    try {
        (void)wrapped.complete({});
        FAIL() << "inner provider exception was not propagated";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "boom");
    }
    EXPECT_EQ(tracer.end_calls.load(), 1);
}

TEST(OpenInferenceCpp, AsyncNonStdExceptionEndsSpanExactlyOnce) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<NonStdAsyncProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    EXPECT_THROW(
        (void)neograph::async::run_sync(wrapped.complete_async({})), int);

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0]->status, "error");
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
}

TEST(OpenInferenceCpp, AbandonedAsyncCallEndsSpanExactlyOnce) {
    using namespace std::chrono_literals;

    InMemoryTracer tracer;
    auto inner = std::make_shared<SuspendedAsyncProvider>();
    auto wrapped = std::make_shared<obs::OpenInferenceProvider>(inner, tracer);
    auto io = std::make_unique<asio::io_context>();
    asio::co_spawn(
        *io,
        [wrapped]() -> asio::awaitable<void> {
            (void)co_await wrapped->complete_async({});
        },
        asio::detached);

    std::thread runner([&] { io->run(); });
    {
        std::unique_lock lock(inner->mu);
        ASSERT_TRUE(inner->cv.wait_for(lock, 2s, [&] { return inner->entered; }));
    }

    io->stop();
    runner.join();
    io.reset();

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
}
