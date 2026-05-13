// v0.3.2 — C++ API live verification for #7 (multi-Send cancel
// propagation at the socket layer).
//
// Sister test to bindings/python/tests/test_async_cancel_live_llm_fanout.py
// (which verifies the Python entry). This one drives the same scenario
// from pure C++:
//
//   Dispatcher node emits 3 Sends → live_worker (real OpenAI complete()).
//   Once all 3 workers have entered Provider::complete(), the test
//   thread calls cancel_token->cancel(). With v0.3.2's per-consumer
//   cancel hooks, every worker's HTTP socket aborts and finished_at
//   stays at 0 (or completes within ~3 s of cancel for in-flight bytes).
//   Pre-fix the single asio cancellation_signal was overwritten by the
//   last-bound run_sync, so 2 of 3 workers streamed for 5–7 s of
//   billable post-cancel work — exactly the regression
//   test_async_cancel_live_llm_fanout.py caught at the Python boundary.
//
// Skipped unless NEOGRAPH_LIVE_LLM=1 + OPENAI_API_KEY (loaded from .env
// via cppdotenv::auto_load_dotenv).

#include <gtest/gtest.h>

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/cancel.h>

#include <cppdotenv/dotenv.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

constexpr int WIDTH = 3;

struct LiveTimings {
    std::array<double, WIDTH> entered{};
    std::array<double, WIDTH> finished{};
    std::mutex mu;
};

double now_seconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class LiveWorkerNode : public GraphNode {
public:
    LiveWorkerNode(std::string name,
                   std::shared_ptr<Provider> provider,
                   LiveTimings* t)
        : name_(std::move(name)), provider_(std::move(provider)), t_(t) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto i_v = in.state.get("i");
        int i = i_v.is_number_integer() ? i_v.get<int>() : 0;
        if (i < 0 || i >= WIDTH) i = 0;

        {
            std::lock_guard<std::mutex> lk(t_->mu);
            t_->entered[i] = now_seconds();
        }

        CompletionParams params;
        params.model       = std::getenv("OPENAI_MODEL")
                                ? std::getenv("OPENAI_MODEL")
                                : "gpt-4o-mini";
        params.temperature = 0.7;
        params.max_tokens  = 400;
        ChatMessage msg;
        msg.role    = "user";
        msg.content = "Write a 300-word essay about historical event #"
                    + std::to_string(i)
                    + ". Be detailed and specific.";
        params.messages = {msg};

        // This is the call that should abort at the socket layer when
        // the parent cancel_token fires. v1.0 invoke() picks up
        // in.ctx.cancel_token (or the engine's thread-local cancel scope)
        // and stamps it into the underlying HTTP call.
        try {
            auto result = co_await provider_->invoke(params, nullptr);
            (void)result;
        } catch (...) {
            // CancelledException / asio operation_aborted unwind here.
            // We DON'T record finished_at — the abort is the success
            // signal in this test.
            throw;
        }

        {
            std::lock_guard<std::mutex> lk(t_->mu);
            t_->finished[i] = now_seconds();
        }
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string                name_;
    std::shared_ptr<Provider>  provider_;
    LiveTimings*               t_;
};

class DispatcherNode : public GraphNode {
public:
    explicit DispatcherNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < WIDTH; ++i) {
            json input;
            input["i"] = i;
            out.sends.push_back(Send{"worker", input});
        }
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

bool live_llm_enabled() {
    const char* key = std::getenv("OPENAI_API_KEY");
    const char* en  = std::getenv("NEOGRAPH_LIVE_LLM");
    return key && *key && en && std::string(en) == "1";
}

} // namespace

// =========================================================================
// LiveLLMCancelFanout.MultiSendBranchesAbortAtSocketLayer
//
// What this test proves that the in-process MultiSendFanOutSeesParentToken
// + MidFlightCancelAbortsSendSiblings tests cannot:
//
//   - parent cancel really propagates into 3 concurrent run_syncs
//     each owning their own ConnPool socket op. The asio
//     cancellation_signal in each isolated send_state must reach the
//     OpenAI HTTPS read loop and trip operation_aborted. The unit
//     tests only proved the cancel FLAG was visible; they did not
//     prove the asio signal mechanism worked.
//
//   - the v0.3.2 per-consumer cancel-hook fix actually closes the
//     last-writer-wins gap. Pre-fix this test would have shown 2/3
//     branches with finished[i] > 0 and 5–7 s of post-cancel
//     wall-clock streaming.
//
// Skipped unless NEOGRAPH_LIVE_LLM=1 + OPENAI_API_KEY in env (or .env).
// =========================================================================
TEST(LiveLLMCancelFanout, MultiSendBranchesAbortAtSocketLayer) {
    cppdotenv::auto_load_dotenv();
    if (!live_llm_enabled()) {
        GTEST_SKIP() << "set NEOGRAPH_LIVE_LLM=1 + OPENAI_API_KEY "
                        "(or put it in .env at the test cwd) to run "
                        "live LLM tests";
    }

    LiveTimings timings;

    neograph::llm::OpenAIProvider::Config provider_cfg;
    provider_cfg.api_key = std::getenv("OPENAI_API_KEY");
    if (const char* base = std::getenv("OPENAI_API_BASE")) {
        provider_cfg.base_url = base;
    }
    provider_cfg.default_model = std::getenv("OPENAI_MODEL")
                                    ? std::getenv("OPENAI_MODEL")
                                    : "gpt-4o-mini";
    auto provider = neograph::llm::OpenAIProvider::create_shared(provider_cfg);

    NodeFactory::instance().register_type("live_dispatcher",
        [](const std::string& name, const json&,
           const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<DispatcherNode>(name);
        });

    NodeFactory::instance().register_type("live_worker",
        [provider, &timings](const std::string& name, const json&,
                             const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<LiveWorkerNode>(name, provider, &timings);
        });

    json channels = json::object();
    channels["i"] = {{"reducer", "overwrite"}};
    json graph = {
        {"name", "live_fanout_cpp"},
        {"channels", channels},
        {"nodes", {
            {"dispatcher", {{"type", "live_dispatcher"}}},
            {"worker",     {{"type", "live_worker"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "dispatcher"}}
            // worker has no outgoing edge — Send target → implicit __end__.
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});

    auto cancel_token = std::make_shared<CancelToken>();
    RunConfig cfg;
    cfg.thread_id    = "live-fanout-cpp";
    cfg.cancel_token = cancel_token;

    // Drive the run on a background thread so the test thread can
    // observe timings + trip cancel mid-flight.
    std::atomic<bool> run_done{false};
    std::exception_ptr run_err;
    std::thread runner([&] {
        try {
            engine->run(cfg);
        } catch (const CancelledException&) {
            // expected
        } catch (...) {
            run_err = std::current_exception();
        }
        run_done.store(true, std::memory_order_release);
    });

    // Wait until every worker has entered Provider::complete().
    auto wait_deadline = std::chrono::steady_clock::now() + 12s;
    auto min_entered = [&]() {
        std::lock_guard<std::mutex> lk(timings.mu);
        double m = timings.entered[0];
        for (int i = 1; i < WIDTH; ++i) {
            m = std::min(m, timings.entered[i]);
        }
        return m;
    };
    while (min_entered() == 0.0 &&
           std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(50ms);
    }

    {
        // Snapshot under lock — std::mutex is non-recursive so we
        // can't call min_entered() (which locks too) inside a held
        // scope.
        std::array<double, WIDTH> snap;
        {
            std::lock_guard<std::mutex> lk(timings.mu);
            snap = timings.entered;
        }
        ASSERT_GT(min_entered(), 0.0)
            << "not every worker entered complete() within 12 s — "
               "entered={" << snap[0] << "," << snap[1] << "," << snap[2] << "}";
    }

    // Hold so OpenAI has actually begun streaming on every branch.
    std::this_thread::sleep_for(700ms);

    const double cancel_t = now_seconds();
    cancel_token->cancel();

    // Give the run up to 25 s after cancel to wind down. With the
    // v0.3.2 fix this is sub-second; pre-fix two of three workers
    // streamed the full ~6-8 s.
    auto post_cancel_deadline = std::chrono::steady_clock::now() + 25s;
    while (!run_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < post_cancel_deadline) {
        std::this_thread::sleep_for(100ms);
    }

    if (!run_done.load()) {
        // Force-detach the runner to avoid join hang on a regression.
        runner.detach();
        FAIL() << "engine->run() did not return within 25 s of cancel";
    }
    runner.join();

    if (run_err) {
        std::rethrow_exception(run_err);
    }

    std::lock_guard<std::mutex> lk(timings.mu);
    std::vector<std::pair<int, double>> leaks;
    for (int i = 0; i < WIDTH; ++i) {
        const double f = timings.finished[i];
        const double e = timings.entered[i];
        std::cerr << "[live-fanout-cpp] branch " << i
                  << ": entered=+" << (e - cancel_t) << "s  "
                  << "finished="
                  << (f == 0.0 ? "aborted"
                              : "+" + std::to_string(f - cancel_t) + "s after cancel")
                  << "\n";
        if (f > 0.0) {
            const double leak = f - cancel_t;
            if (leak >= 3.0) {
                leaks.emplace_back(i, leak);
            }
        }
    }

    EXPECT_TRUE(leaks.empty())
        << "cost-leak regression on Send fan-out (C++): "
        << leaks.size() << " branches streamed for >3 s after cancel. "
        << "The v0.3.2 per-consumer cancel-hook chain "
        << "(CancelToken::add_cancel_hook → local cancellation_signal "
        << "→ ConnPool socket abort) is not reaching every concurrent "
        << "Send worker.";
}
