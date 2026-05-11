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
#include <neograph/provider.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
    void end() override { rec_->ended = true; }

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
