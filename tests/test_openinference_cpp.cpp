// Issue #9 — parity coverage for the C++ OpenInference layer.
//
// Covers the owned typed C++ provider boundary:
//   - graph tracing opens a CHAIN root + per-node CHAIN children
//   - provider dispatch records public text/control scalars, never private replay
//   - streaming forwards typed events but traces only visible text deltas
//   - nullable usage, partial failures, and original exceptions remain distinct
//   - tracer failures and teardown never replace provider behavior
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
#include "fixtures/typed_provider.h"

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

using neograph::ProviderRequest;
using neograph::PreparedProviderRequest;
using neograph::ProviderMode;
namespace typed = neograph::test;
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

class FakeProvider : public typed::LocalProvider {
    struct State {
        std::string reply = "ok";
        std::uint64_t prompt_tokens = 7, completion_tokens = 3;
        std::vector<std::string> stream_chunks;
        std::atomic<int> calls{0};
    };
    explicit FakeProvider(std::shared_ptr<State> state)
        : LocalProvider([state](ProviderRequest, const PreparedProviderRequest& prepared,
                               const EventCallback& on_event) -> asio::awaitable<sp::runtime::Result> {
            ++state->calls;
            std::string text = state->reply;
            if (prepared.mode() == ProviderMode::Stream) {
                text.clear();
                if (on_event) {
                    on_event(sp::Begin{"observability"});
                    on_event(sp::MessageBegin{{0}, {}, sp::Role::Assistant});
                    on_event(sp::PartBegin{{0}, {0}, sp::PartKind::Text});
                }
                for (const auto& chunk : state->stream_chunks) {
                    text += chunk;
                    if (on_event) on_event(sp::PartDelta{{0}, {sp::PartKind::Text, chunk}});
                }
                if (text.empty()) text = state->reply;
                if (on_event) {
                    on_event(sp::PartSeal{{0}, {}});
                    on_event(sp::MessageSeal{{0}});
                }
            }
            co_return typed::success(std::move(text), typed::usage(
                state->prompt_tokens, state->completion_tokens,
                state->prompt_tokens + state->completion_tokens));
        }, "fake"), state_(std::move(state)), reply(state_->reply),
          prompt_tokens(state_->prompt_tokens), completion_tokens(state_->completion_tokens),
          stream_chunks(state_->stream_chunks), calls(state_->calls) {}
    std::shared_ptr<State> state_;
public:
    FakeProvider() : FakeProvider(std::make_shared<State>()) {}
    std::string& reply;
    std::uint64_t& prompt_tokens;
    std::uint64_t& completion_tokens;
    std::vector<std::string>& stream_chunks;
    std::atomic<int>& calls;
};

class ThrowingProvider : public typed::LocalProvider {
public:
    ThrowingProvider()
        : LocalProvider([](ProviderRequest, const PreparedProviderRequest&,
                           const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            throw std::runtime_error("boom");
            co_return typed::success("");
        }, "throw") {}
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

struct ParentLifetimeState {
    std::atomic<bool> alive{true};
    std::atomic<bool> ended{false};
};

class ParentLifetimeSpan : public obs::Span {
public:
    explicit ParentLifetimeSpan(std::shared_ptr<ParentLifetimeState> state)
        : state_(std::move(state)) {}
    ~ParentLifetimeSpan() override { state_->alive.store(false); }

    void set_attribute(std::string_view, std::string_view) override {}
    void set_attribute(std::string_view, int64_t) override {}
    void set_attribute(std::string_view, double) override {}
    void set_attribute_bool(std::string_view, bool) override {}
    void add_event(std::string_view, std::string_view) override {}
    void set_status_ok() override {}
    void set_status_error(std::string_view) override {}
    void end() override { state_->ended.store(true); }

private:
    std::shared_ptr<ParentLifetimeState> state_;
};

class BlockingParentLifetimeTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span* parent) override {
        std::shared_ptr<ParentLifetimeState> parent_state;
        {
            std::lock_guard lock(mu);
            if (parent) parent_state = state_by_span.at(parent);
        }

        if (name == "llm.complete") {
            std::unique_lock lock(window_mu);
            child_start_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_child_start; });
            parent_usable_during_start = parent_state
                && parent_state->alive.load() && !parent_state->ended.load();
        }

        auto state = std::make_shared<ParentLifetimeState>();
        auto span = std::make_unique<ParentLifetimeSpan>(state);
        {
            std::lock_guard lock(mu);
            state_by_span.emplace(span.get(), std::move(state));
        }
        return span;
    }

    std::mutex window_mu;
    std::condition_variable cv;
    bool child_start_entered = false;
    bool release_child_start = false;
    bool parent_usable_during_start = false;

private:
    std::mutex mu;
    std::unordered_map<obs::Span*, std::shared_ptr<ParentLifetimeState>>
        state_by_span;
};

class ReentrantCloseTracer : public InMemoryTracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span* parent) override {
        if (name == "llm.complete" && session) session->close();
        return InMemoryTracer::start_span(name, parent);
    }

    obs::OpenInferenceTracerSession* session = nullptr;
};

struct BlockingDestructionState {
    std::mutex mu;
    std::condition_variable cv;
    bool child_start_entered = false;
    bool release_child_start = false;
    bool root_destructor_entered = false;
    bool release_root_destructor = false;
};

class BlockingDestructorSpan : public obs::Span {
public:
    BlockingDestructorSpan(std::shared_ptr<BlockingDestructionState> state,
                           bool block_destructor)
        : state_(std::move(state)), block_destructor_(block_destructor) {}

    ~BlockingDestructorSpan() override {
        if (!block_destructor_) return;
        std::unique_lock lock(state_->mu);
        state_->root_destructor_entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&] { return state_->release_root_destructor; });
    }

    void set_attribute(std::string_view, std::string_view) override {}
    void set_attribute(std::string_view, int64_t) override {}
    void set_attribute(std::string_view, double) override {}
    void set_attribute_bool(std::string_view, bool) override {}
    void add_event(std::string_view, std::string_view) override {}
    void set_status_ok() override {}
    void set_status_error(std::string_view) override {}
    void end() override {}

private:
    std::shared_ptr<BlockingDestructionState> state_;
    bool block_destructor_;
};

class BlockingWrapperDestructionTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span*) override {
        if (name == "llm.complete") {
            std::unique_lock lock(state->mu);
            state->child_start_entered = true;
            state->cv.notify_all();
            state->cv.wait(lock, [&] { return state->release_child_start; });
        }
        return std::make_unique<BlockingDestructorSpan>(
            state, name == "graph.run");
    }

    std::shared_ptr<BlockingDestructionState> state =
        std::make_shared<BlockingDestructionState>();
};

struct BlockingTerminalState {
    std::mutex mu;
    std::condition_variable cv;
    bool end_called = false;
    bool destructor_entered = false;
    bool release_destructor = false;
};

class BlockingTerminalSpan : public obs::Span {
public:
    BlockingTerminalSpan(std::shared_ptr<BlockingTerminalState> state,
                         bool block_destructor)
        : state_(std::move(state)), block_destructor_(block_destructor) {}

    ~BlockingTerminalSpan() override {
        if (!block_destructor_) return;
        std::unique_lock lock(state_->mu);
        state_->destructor_entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&] { return state_->release_destructor; });
    }

    void set_attribute(std::string_view, std::string_view) override {}
    void set_attribute(std::string_view, int64_t) override {}
    void set_attribute(std::string_view, double) override {}
    void set_attribute_bool(std::string_view, bool) override {}
    void add_event(std::string_view, std::string_view) override {}
    void set_status_ok() override {}
    void set_status_error(std::string_view) override {}
    void end() override {
        if (!block_destructor_) return;
        std::lock_guard lock(state_->mu);
        state_->end_called = true;
    }

private:
    std::shared_ptr<BlockingTerminalState> state_;
    bool block_destructor_;
};

class BlockingTerminalTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name,
                                          obs::Span*) override {
        return std::make_unique<BlockingTerminalSpan>(
            state, name.starts_with("node."));
    }

    std::shared_ptr<BlockingTerminalState> state =
        std::make_shared<BlockingTerminalState>();
};

class NonStdAsyncProvider : public typed::LocalProvider {
public:
    NonStdAsyncProvider()
        : LocalProvider([](ProviderRequest, const PreparedProviderRequest&,
                           const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            throw 42;
            co_return typed::success("");
        }, "non-std-async") {}
};

class SuspendedAsyncProvider : public typed::LocalProvider {
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
    };
    explicit SuspendedAsyncProvider(std::shared_ptr<State> state)
        : LocalProvider([state](ProviderRequest, const PreparedProviderRequest&,
                               const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            auto executor = co_await asio::this_coro::executor;
            {
                std::lock_guard lock(state->mutex);
                state->entered = true;
            }
            state->condition.notify_all();
            asio::steady_timer timer(executor);
            timer.expires_after(std::chrono::hours(1));
            co_await timer.async_wait(asio::use_awaitable);
            co_return typed::success("");
        }, "suspended-async"), state_(std::move(state)), mu(state_->mutex),
          cv(state_->condition), entered(state_->entered) {}
    std::shared_ptr<State> state_;
public:
    SuspendedAsyncProvider() : SuspendedAsyncProvider(std::make_shared<State>()) {}
    std::mutex& mu;
    std::condition_variable& cv;
    bool& entered;
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
    EXPECT_EQ(spans[0]->end_calls, 1);

    // Node child: name "node.researcher", parent = root, CHAIN
    EXPECT_EQ(spans[1]->name, "node.researcher");
    EXPECT_NE(spans[1]->parent, nullptr);
    EXPECT_EQ(spans[1]->attrs_str["openinference.span.kind"], "CHAIN");
    EXPECT_EQ(spans[1]->attrs_str["neograph.node"], "researcher");
    EXPECT_EQ(spans[1]->attrs_str["input.mime_type"], "application/json");
    EXPECT_EQ(spans[1]->attrs_str["output.mime_type"], "application/json");
    EXPECT_EQ(spans[1]->status, "ok");
    EXPECT_TRUE(spans[1]->ended);
    EXPECT_EQ(spans[1]->end_calls, 1);

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
    EXPECT_EQ(spans[1]->end_calls, 1);

    // b: INTERRUPT
    EXPECT_EQ(spans[2]->name, "node.b");
    EXPECT_TRUE(spans[2]->attrs_bool["neograph.interrupted"]);
    EXPECT_TRUE(spans[2]->ended);
    EXPECT_EQ(spans[2]->end_calls, 1);
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

    auto request = typed::request("fake-model-1");
    auto& payload = std::get<sp::chat::Request>(request.payload);
    payload.temperature = 0.5;
    payload.max_output_tokens = 100;
    payload.canonical_messages = {typed::message("be helpful", sp::Role::System),
                                  typed::message("hi", sp::Role::User)};

    auto result = wrapped.invoke(std::move(request));
    EXPECT_EQ(typed::text(result), "hello world");

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
    const auto invocation = neograph::json::parse(s.attrs_str.at("llm.invocation_parameters"));
    EXPECT_EQ(invocation.at("temperature"), 0.5);
    EXPECT_EQ(invocation.at("max_tokens"), 100);
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

    auto request = typed::request("fake-stream", "hi", ProviderMode::Stream);
    std::vector<std::string> user_received;
    request.on_event = [&user_received](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event);
            delta && delta->payload.kind == sp::PartKind::Text)
            user_received.emplace_back(delta->payload.bytes);
    };
    auto result = wrapped.invoke(std::move(request));

    EXPECT_EQ(typed::text(result), "foobarbaz");
    EXPECT_EQ(user_received, (std::vector<std::string>{"foo", "bar", "baz"}));

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    const auto& s = *spans[0];
    std::vector<std::string> deltas;
    for (const auto& event : s.events) {
        EXPECT_EQ(event.name, "llm.token");
        deltas.push_back(event.payload);
    }
    EXPECT_EQ(deltas, (std::vector<std::string>{"foo", "bar", "baz"}));
    EXPECT_EQ(s.attrs_str.at("output.value"), "foobarbaz");
    EXPECT_TRUE(s.ended);
}

TEST(OpenInferenceCpp, ProviderPropagatesExceptionAndMarksSpanError) {
    InMemoryTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    auto request = typed::request("x", "hi");
    EXPECT_THROW(wrapped.invoke(std::move(request)), std::runtime_error);

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

    auto result = wrapped.invoke(typed::request());
    EXPECT_EQ(typed::text(result), "ok");
    EXPECT_EQ(inner->calls.load(), 1);

    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0]->parent, nullptr);
    EXPECT_TRUE(spans[0]->ended);
    EXPECT_EQ(spans[0]->end_calls, 1);
}

TEST(OpenInferenceCpp, LegacyParentLookupPreservesCallerOwnedHierarchy) {
    InMemoryTracer tracer;
    auto parent = tracer.start_span("caller.parent", nullptr);
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(
        inner, tracer, [&]() -> obs::Span* { return parent.get(); });

    auto result = wrapped.invoke(typed::request());

    EXPECT_EQ(typed::text(result), "ok");
    auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[1]->parent, parent.get());
    EXPECT_EQ(spans[1]->end_calls, 1);
}

TEST(OpenInferenceCpp, ParentLeaseSurvivesConcurrentCloseThroughStartSpan) {
    using namespace std::chrono_literals;

    BlockingParentLifetimeTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer, session);

    auto completion = std::async(std::launch::async, [&] {
        return wrapped.invoke(typed::request());
    });
    {
        std::unique_lock lock(tracer.window_mu);
        ASSERT_TRUE(tracer.cv.wait_for(lock, 2s, [&] {
            return tracer.child_start_entered;
        }));
    }

    auto closing = std::async(std::launch::async, [&] { session.close(); });
    EXPECT_EQ(closing.wait_for(50ms), std::future_status::timeout);

    {
        std::lock_guard lock(tracer.window_mu);
        tracer.release_child_start = true;
    }
    tracer.cv.notify_all();

    EXPECT_EQ(completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(tracer.parent_usable_during_start);
    EXPECT_EQ(typed::text(completion.get()), "ok");
}

TEST(OpenInferenceCpp, ReentrantCloseFromStartSpanDoesNotDeadlock) {
    using namespace std::chrono_literals;

    ReentrantCloseTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    tracer.session = &session;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer, session);

    auto completion = std::async(std::launch::async, [&] {
        return wrapped.invoke(typed::request());
    });

    ASSERT_EQ(completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(typed::text(completion.get()), "ok");
    EXPECT_EQ(session.current_parent(), nullptr);
}

TEST(OpenInferenceCpp, CloseWaitsForFinalSpanWrapperDestruction) {
    using namespace std::chrono_literals;

    BlockingWrapperDestructionTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer, session);

    auto completion = std::async(std::launch::async, [&] {
        return wrapped.invoke(typed::request());
    });
    {
        std::unique_lock lock(tracer.state->mu);
        ASSERT_TRUE(tracer.state->cv.wait_for(lock, 2s, [&] {
            return tracer.state->child_start_entered;
        }));
    }

    auto closing = std::async(std::launch::async, [&] { session.close(); });
    {
        std::lock_guard lock(tracer.state->mu);
        tracer.state->release_child_start = true;
    }
    tracer.state->cv.notify_all();

    {
        std::unique_lock lock(tracer.state->mu);
        ASSERT_TRUE(tracer.state->cv.wait_for(lock, 2s, [&] {
            return tracer.state->root_destructor_entered;
        }));
    }
    EXPECT_EQ(closing.wait_for(50ms), std::future_status::timeout);

    {
        std::lock_guard lock(tracer.state->mu);
        tracer.state->release_root_destructor = true;
    }
    tracer.state->cv.notify_all();

    EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(typed::text(completion.get()), "ok");
}

TEST(OpenInferenceCpp, CloseWaitsForTerminalSpanTeardown) {
    using namespace std::chrono_literals;

    BlockingTerminalTracer tracer;
    auto session = obs::openinference_tracer(tracer);
    session.cb({GraphEvent::Type::NODE_START, "blocked",
                neograph::json::object()});

    auto terminal = std::async(std::launch::async, [&] {
        session.cb({GraphEvent::Type::NODE_END, "blocked",
                    neograph::json::object()});
    });
    {
        std::unique_lock lock(tracer.state->mu);
        ASSERT_TRUE(tracer.state->cv.wait_for(lock, 2s, [&] {
            return tracer.state->destructor_entered;
        }));
        EXPECT_TRUE(tracer.state->end_called);
    }

    auto closing = std::async(std::launch::async, [&] { session.close(); });
    EXPECT_EQ(closing.wait_for(50ms), std::future_status::timeout);

    {
        std::lock_guard lock(tracer.state->mu);
        tracer.state->release_destructor = true;
    }
    tracer.state->cv.notify_all();

    EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(terminal.wait_for(2s), std::future_status::ready);
}

TEST(OpenInferenceCpp, StartSpanFailureDoesNotBlockInnerCall) {
    ThrowingStartTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    auto result = wrapped.invoke(typed::request());
    EXPECT_EQ(typed::text(result), "ok");
    EXPECT_EQ(inner->calls.load(), 1);
}

TEST(OpenInferenceCpp, TracingFailureDoesNotReplaceInnerException) {
    ThrowingStartTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    try {
        (void)wrapped.invoke(typed::request());
        FAIL() << "inner provider exception was not propagated";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "boom");
    }
}

TEST(OpenInferenceCpp, SpanMethodFailuresDoNotAffectInnerCall) {
    ThrowingSpanTracer tracer;
    auto inner = std::make_shared<FakeProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    auto result = wrapped.invoke(typed::request());

    EXPECT_EQ(typed::text(result), "ok");
    EXPECT_EQ(inner->calls.load(), 1);
    EXPECT_EQ(tracer.end_calls.load(), 1);
}

TEST(OpenInferenceCpp, SpanMethodFailuresDoNotReplaceInnerException) {
    ThrowingSpanTracer tracer;
    auto inner = std::make_shared<ThrowingProvider>();
    obs::OpenInferenceProvider wrapped(inner, tracer);

    try {
        (void)wrapped.invoke(typed::request());
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
        (void)neograph::async::run_sync(wrapped.invoke_async(typed::request())), int);

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
            (void)co_await wrapped->invoke_async(typed::request());
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

TEST(OpenInferenceCpp, ProviderKeepsPrivatePartsOutOfTelemetryAndPreservesNullableUsage) {
    InMemoryTracer tracer;
    sp::Message message;
    message.parts = {
        sp::Thinking{"private-thought", std::string("signature")},
        sp::Text{"visible"},
        sp::Opaque{"vendor-extension", typed::document(R"({"opaque":"retained"})")},
        sp::InvalidToolCall{"broken", "lookup", sp::ToolCallKind::ClientExecuted,
                            "{PRIVATE_ARGUMENT", sp::InvalidReason::Truncated}};
    sp::Completion completion;
    completion.messages = {std::move(message)};
    completion.stop = {sp::StopKind::MaxTokens, "length"};
    completion.usage = typed::usage(0, std::nullopt, std::nullopt, sp::UsageStage::Partial);
    auto owned = std::make_shared<const sp::Outcome>(std::move(completion));
    auto inner = std::make_shared<typed::LocalProvider>(
        [owned](ProviderRequest, const PreparedProviderRequest&,
                const typed::LocalProvider::EventCallback& on_event) -> asio::awaitable<sp::runtime::Result> {
            if (on_event) {
                const auto& value = std::get<sp::Completion>(*owned);
                const auto& parts = value.messages[0].parts;
                const auto& thinking = std::get<sp::Thinking>(parts[0]);
                on_event(sp::Begin{"private-stream"});
                on_event(sp::MessageBegin{{0}, {}, sp::Role::Assistant});
                on_event(sp::PartBegin{{0}, {0}, sp::PartKind::Thinking});
                on_event(sp::PartDelta{{0}, {sp::PartKind::Thinking, thinking.text}});
                on_event(sp::PartDelta{{0}, {sp::PartKind::Thinking, *thinking.signature,
                                            sp::DeltaChannel::Signature}});
                on_event(sp::PartSeal{{0}, {}});
                on_event(sp::PartBegin{{0}, {1}, sp::PartKind::Text, {}, 1});
                on_event(sp::PartDelta{{1}, {sp::PartKind::Text, std::get<sp::Text>(parts[1]).value}});
                on_event(sp::PartSeal{{1}, {}});
                const auto& opaque = std::get<sp::Opaque>(parts[2]);
                sp::PartHeader header;
                header.wire_type = opaque.wire_type;
                header.wire_metadata = opaque.wire_metadata;
                on_event(sp::PartBegin{{0}, {2}, sp::PartKind::Opaque, header, 2});
                on_event(sp::PartSeal{{2}, {}, opaque.wire_metadata});
                const auto& invalid = std::get<sp::InvalidToolCall>(parts[3]);
                header = {};
                header.wire_id = invalid.id;
                header.name = invalid.name;
                on_event(sp::PartBegin{{0}, {3}, sp::PartKind::ToolCall, header, 3});
                on_event(sp::PartDelta{{3}, {sp::PartKind::ToolCall, invalid.raw_fragment}});
                on_event(sp::PartSeal{{3}, {}});
                on_event(sp::MessageSeal{{0}});
                on_event(sp::UsageUpdate{value.usage});
                on_event(sp::Stop{value.stop});
            }
            co_return owned;
        });
    obs::OpenInferenceProvider wrapped(inner, tracer);
    auto request = typed::request("fixture-model", "hello", ProviderMode::Stream);
    struct ReceivedDelta {
        sp::PartKind kind;
        sp::DeltaChannel channel;
        std::string bytes;
    };
    auto received = std::make_shared<std::vector<ReceivedDelta>>();
    request.on_event = [received](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event))
            received->push_back({delta->payload.kind, delta->payload.channel,
                                 std::string(delta->payload.bytes)});
    };
    std::get<sp::chat::Request>(request.payload).tools = {
        {"lookup", "private-tool-description",
         typed::document(R"({"type":"object","properties":{"path":{"type":"string","description":"private-tool-schema"}}})")}};
    const auto result = wrapped.invoke(std::move(request));
    ASSERT_EQ(result, owned);
    ASSERT_EQ(received->size(), 4u);
    EXPECT_EQ((*received)[0].kind, sp::PartKind::Thinking);
    EXPECT_EQ((*received)[0].bytes, "private-thought");
    EXPECT_EQ((*received)[1].channel, sp::DeltaChannel::Signature);
    EXPECT_EQ((*received)[1].bytes, "signature");
    EXPECT_EQ((*received)[2].kind, sp::PartKind::Text);
    EXPECT_EQ((*received)[2].bytes, "visible");
    EXPECT_EQ((*received)[3].kind, sp::PartKind::ToolCall);
    EXPECT_EQ((*received)[3].bytes, "{PRIVATE_ARGUMENT");
    const auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    const auto& span = *spans[0];
    const auto& parts = typed::completion(result).messages[0].parts;
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(std::get<sp::Thinking>(parts[0]).text, "private-thought");
    EXPECT_EQ(std::get<sp::Thinking>(parts[0]).signature, "signature");
    EXPECT_EQ(std::get<sp::Text>(parts[1]).value, "visible");
    EXPECT_EQ(std::get<sp::Opaque>(parts[2]).wire_type, "vendor-extension");
    EXPECT_EQ(std::get<sp::InvalidToolCall>(parts[3]).raw_fragment, "{PRIVATE_ARGUMENT");
    EXPECT_EQ(span.attrs_str.at("output.value"), "visible");
    EXPECT_EQ(span.attrs_str.at("llm.output_messages.0.message.content"), "visible");
    for (const auto& [key, value] : span.attrs_str) {
        for (const auto* secret : {"private-thought", "signature", "vendor-extension",
                                  "retained", "PRIVATE_ARGUMENT", "private-tool-description",
                                  "private-tool-schema"})
            EXPECT_EQ(value.find(secret), std::string::npos) << key;
    }
    ASSERT_EQ(span.events.size(), 1u);
    EXPECT_EQ(span.events[0].name, "llm.token");
    EXPECT_EQ(span.events[0].payload, "visible");
    EXPECT_EQ(span.attrs_int.at("llm.token_count.prompt"), 0);
    EXPECT_FALSE(span.attrs_int.contains("llm.token_count.completion"));
    EXPECT_FALSE(span.attrs_int.contains("llm.token_count.total"));
    EXPECT_FALSE(typed::completion(result).usage.output_total.has_value());
    EXPECT_EQ(typed::completion(result).usage.stage, sp::UsageStage::Partial);
}

TEST(OpenInferenceCpp, ProviderFailureKeepsPartialOutcomeAndEndsErrorSpan) {
    InMemoryTracer tracer;
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::Truncated;
    failure.error.safe_message = "stream interrupted";
    failure.error.retry_safety = sp::RetrySafety::OutputObserved;
    failure.partial.messages = {typed::message("retained-prefix")};
    failure.partial.usage = typed::usage(4, std::nullopt, std::nullopt, sp::UsageStage::Partial);
    auto owned = std::make_shared<const sp::Outcome>(std::move(failure));
    auto inner = std::make_shared<typed::LocalProvider>(
        [owned](ProviderRequest, const PreparedProviderRequest&,
                const typed::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            co_return owned;
        });
    obs::OpenInferenceProvider wrapped(inner, tracer);
    const auto result = wrapped.invoke(typed::request());
    ASSERT_EQ(result, owned);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    const auto spans = tracer.snapshot();
    ASSERT_EQ(spans.size(), 1u);
    const auto& span = *spans[0];
    EXPECT_EQ(span.status, "error");
    EXPECT_TRUE(span.ended);
    EXPECT_EQ(span.end_calls, 1);
    const auto& retained = std::get<sp::Failure>(*result);
    EXPECT_EQ(retained.error.kind, sp::ErrorKind::Truncated);
    EXPECT_EQ(retained.error.retry_safety, sp::RetrySafety::OutputObserved);
    EXPECT_EQ(std::get<sp::Text>(retained.partial.messages[0].parts[0]).value, "retained-prefix");
    EXPECT_EQ(span.attrs_str.at("output.value"), "retained-prefix");
    EXPECT_FALSE(span.attrs_int.contains("llm.token_count.completion"));
}
