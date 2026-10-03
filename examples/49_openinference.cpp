// NeoGraph Example 44: OpenInference observability (v0.6.0 feature)
//
// Wires a minimal Tracer adapter (prints spans to stderr) into the
// engine via openinference_tracer() and wraps a mock Provider in
// OpenInferenceProvider so each LLM call shows up as a child span.
//
// In a real deployment the Tracer would forward to opentelemetry-cpp →
// OTLP → Phoenix / Arize / Langfuse. Here we just print the recorded
// span tree so the example stays dep-free.
//
// Usage: ./example_openinference

#include <neograph/neograph.h>
#include <neograph/observability/openinference.h>
#include <neograph/observability/tracer.h>
#include "provider_example_support.h"

#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
namespace obs = neograph::observability;

// ── Minimal stderr-printing tracer ──────────────────────────────────
//
// IMPORTANT pattern: the data record (RecordedSpan) is owned by the
// tracer, NOT by the wrapper returned to the OpenInference layer.
// OpenInferenceTracerSession::close() releases its
// `unique_ptr<obs::Span>` over the root span — the same applies to
// pending node spans. If the tracer adapter handed out raw pointers
// into caller-owned span storage, those raw pointers become
// dangling after close(), and any post-close introspection
// (printing, attribute inspection in tests) reads freed memory and
// either crashes or hangs.
//
// The shape below splits the data (`RecordedSpan`, owned by the
// tracer in `unique_ptr`s, lifetime = tracer's lifetime) from the
// wrapper (`PrintedSpan`, owned by the OpenInference layer,
// destruction is harmless because the data lives elsewhere). This
// is the same pattern used by `tests/test_openinference_cpp.cpp`'s
// InMemoryTracer — copy it for any tracer adapter you write.

struct RecordedSpan {
    std::string name;
    RecordedSpan* parent = nullptr;
    std::map<std::string, std::string> attrs;
    int event_count = 0;
    std::string status = "unset";
    bool ended = false;
};

class PrintedSpan : public obs::Span {
public:
    explicit PrintedSpan(RecordedSpan* rec) : rec_(rec) {}
    void set_attribute(std::string_view k, std::string_view v) override {
        rec_->attrs[std::string(k)] = std::string(v);
    }
    void set_attribute(std::string_view k, int64_t v) override {
        rec_->attrs[std::string(k)] = std::to_string(v);
    }
    void set_attribute(std::string_view k, double v) override {
        rec_->attrs[std::string(k)] = std::to_string(v);
    }
    void set_attribute_bool(std::string_view k, bool v) override {
        rec_->attrs[std::string(k)] = v ? "true" : "false";
    }
    void add_event(std::string_view, std::string_view) override { ++rec_->event_count; }
    void set_status_ok() override { rec_->status = "ok"; }
    void set_status_error(std::string_view m) override { rec_->status = "error: " + std::string(m); }
    void end() override { rec_->ended = true; }
private:
    RecordedSpan* rec_;
};

class PrintTracer : public obs::Tracer {
public:
    std::unique_ptr<obs::Span> start_span(std::string_view name, obs::Span* parent) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto rec = std::make_unique<RecordedSpan>();
        rec->name = std::string(name);
        if (parent) {
            auto it = wrapper_to_record_.find(parent);
            if (it != wrapper_to_record_.end()) rec->parent = it->second;
        }
        auto wrap = std::make_unique<PrintedSpan>(rec.get());
        wrapper_to_record_[wrap.get()] = rec.get();
        records_.push_back(std::move(rec));
        return wrap;
    }
    void print_tree() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::cerr << "─── span tree ──────────────────────\n";
        std::map<RecordedSpan*, std::vector<RecordedSpan*>> children;
        std::vector<RecordedSpan*> roots;
        for (auto& r : records_) {
            if (r->parent) children[r->parent].push_back(r.get());
            else roots.push_back(r.get());
        }
        for (auto* r : roots) print_one(r, children, 0);
    }
    size_t span_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return records_.size();
    }
private:
    void print_one(RecordedSpan* s,
                   const std::map<RecordedSpan*, std::vector<RecordedSpan*>>& children,
                   int depth) const {
        std::cerr << std::string(depth * 2, ' ') << "• " << s->name
                  << " [" << s->status << (s->ended ? ", ended" : "") << "]";
        if (s->event_count) std::cerr << " events=" << s->event_count;
        std::cerr << "\n";
        for (auto& [k, v] : s->attrs) {
            std::string short_v = v.size() > 60 ? v.substr(0, 57) + "..." : v;
            std::cerr << std::string(depth * 2 + 2, ' ') << k << " = " << short_v << "\n";
        }
        auto it = children.find(s);
        if (it != children.end()) {
            for (auto* c : it->second) print_one(c, children, depth + 1);
        }
    }
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<RecordedSpan>> records_;
    std::map<obs::Span*, RecordedSpan*> wrapper_to_record_;
};

// ── Mock LLM provider ───────────────────────────────────────────────

class MockProvider : public Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    PreparedProviderRequest prepare(ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                bool greeted = false;
                for (const auto& message : *history)
                    if (message.role == sp::Role::User)
                        for (const auto& part : message.parts)
                            if (const auto* text = std::get_if<sp::Text>(&part))
                                greeted |= text->value.find("hi") != std::string::npos;
                sp::Completion completion;
                completion.messages.push_back(examples::message(sp::Role::Assistant,
                    greeted ? "Hello from the observed provider!"
                            : "The observed provider is ready."));
                completion.stop.kind = sp::StopKind::EndTurn;
                if (prepared.mode() == ProviderMode::Stream)
                    examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
    std::string get_name() const override { return "mock"; }
};

// ── Simple non-streaming LLM node ───────────────────────────────────

class TalkNode : public GraphNode, public neograph::RuntimeInterpositionConsumer {
public:
    explicit TalkNode(std::shared_ptr<Provider> p) : prov_(std::move(p)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto request = make_provider_request(*prov_, "gpt-mock",
            {examples::message(sp::Role::User, "say hi")});
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        request.on_event = in.ctx.on_provider_event;
        auto outcome = co_await observe_provider_result(in.ctx,
            invoke_provider(prov_, std::move(request), {}, {}, provider_call_broker(in.ctx),
                make_provider_call_identity(in.ctx, get_name())));
        // The graph's synchronized sink retains the immutable full outcome.
        // The reply channel is only an explicit display projection.
        record_usage(in.ctx, outcome);
        outcome = examples::require_outcome(std::move(outcome));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"reply", json(examples::visible_text(*outcome))});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
private:
    std::shared_ptr<Provider> prov_;
};

int main() {
    PrintTracer tracer;

    // The session opens the CHAIN root span at construction. It must
    // outlive the run. The provider copies a safe child-span starter from it.
    auto session = std::make_shared<obs::OpenInferenceTracerSession>(
        obs::openinference_tracer(tracer));

    auto inner = std::make_shared<MockProvider>();
    auto traced = std::make_shared<obs::OpenInferenceProvider>(
        inner, tracer, *session);

    NodeFactory::instance().register_type("talk",
        [traced](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<TalkNode>(traced);
        });

    json def = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "obs_demo"},
        {"channels", {{"reply", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"talk", {{"type", "talk"}}}}},
        {"edges", {
            {{"from", "__start__"}, {"to", "talk"}},
            {{"from", "talk"}, {"to", "__end__"}},
        }},
    };
    NodeContext ctx;
    auto        engine = GraphEngine::build(def, EngineConfig{.node_context = ctx});

    // Use run_stream so the session's cb fires NODE_START/END events.
    RunConfig cfg;
    cfg.input = {{"reply", ""}};
    cfg.stream_mode = StreamMode::ALL;
    auto result = engine->run_stream(cfg, session->cb);
    session->close();

    tracer.print_tree();
    auto total = tracer.span_count();
    std::cout << "total spans=" << total << "\n";

    if (total < 3) {
        std::cerr << "FAIL: expected ≥ 3 spans (root + node + llm), got " << total << "\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
