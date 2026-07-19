// Stage 3 / Semester 3.6 (API surface) regression — GraphEngine now
// exposes run_async / run_stream_async / resume_async returning
// asio::awaitable<RunResult>. These cases pin the public contract:
//
//   * run_async resolves to the same RunResult as run() on the happy
//     path.
//   * Exceptions thrown inside a node propagate out of the awaitable
//     (i.e. caller catches via try/catch around co_await, not inside
//     the engine).
//   * resume_async honours the same checkpoint state as resume().
//   * Multiple concurrent run_async invocations on a shared io_context
//     all complete (no lockup or interleaving fault).
//
// When the engine internals get coroutinized later, these tests must
// keep passing — they encode the public contract that follow-up work
// has to preserve.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/async/http_client.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

json minimal_graph(const std::string& node_name,
                   const std::string& node_type = "custom") {
    return {
        {"name", "async_api_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {node_name, {{"type", node_type}}},
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}},
        }},
    };
}

class WriteNode : public GraphNode {
public:
    WriteNode(const std::string& name, std::string value)
        : name_(name), value_(std::move(value)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json(value_)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::string value_;
};

class ThrowingNode : public GraphNode {
public:
    explicit ThrowingNode(const std::string& name) : name_(name) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        throw std::runtime_error("intentional failure");
        co_return NodeOutput{};  // unreachable
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void register_writer(const std::string& value) {
    NodeFactory::instance().register_type("custom",
        [value](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WriteNode>(name, value);
        });
}

void register_thrower() {
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ThrowingNode>(name);
        });
}

struct ReleasableHttpServer {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io};
    std::thread worker;
    std::atomic<int> requests{0};
    std::atomic<bool> released{false};
    unsigned short port = 0;

    ReleasableHttpServer() {
        acceptor.open(asio::ip::tcp::v4());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        asio::co_spawn(io, accept_loop(), asio::detached);
        worker = std::thread([this] { io.run(); });
    }

    ~ReleasableHttpServer() {
        release();
        asio::post(io, [this] {
            asio::error_code ec;
            acceptor.close(ec);
        });
        if (worker.joinable()) worker.join();
    }

    void release() {
        released.store(true, std::memory_order_release);
        asio::post(io, [] {});
    }

    asio::awaitable<void> handle(asio::ip::tcp::socket socket) {
        try {
            asio::streambuf request;
            co_await asio::async_read_until(
                socket, request, "\r\n\r\n", asio::use_awaitable);
            requests.fetch_add(1, std::memory_order_release);

            asio::steady_timer poll(co_await asio::this_coro::executor);
            while (!released.load(std::memory_order_acquire)) {
                poll.expires_after(std::chrono::milliseconds(2));
                co_await poll.async_wait(asio::use_awaitable);
            }

            static constexpr char response[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n\r\n"
                "ok";
            co_await asio::async_write(
                socket, asio::buffer(response, sizeof(response) - 1),
                asio::use_awaitable);
        } catch (...) {
            // A cancelled client closes its socket before release(), which is
            // the expected fixed-path outcome.
        }
    }

    asio::awaitable<void> accept_loop() {
        for (;;) {
            asio::ip::tcp::socket socket{io};
            asio::error_code ec;
            co_await acceptor.async_accept(
                socket, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            asio::co_spawn(io, handle(std::move(socket)), asio::detached);
        }
    }
};

struct OperationTokensSeen {
    std::mutex mu;
    std::vector<CancelToken*> tokens;
};

class StallingHttpNode final : public GraphNode {
public:
    StallingHttpNode(std::string name, unsigned short port,
                     OperationTokensSeen* seen)
        : name_(std::move(name)), port_(port), seen_(seen) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        {
            std::lock_guard<std::mutex> lock(seen_->mu);
            seen_->tokens.push_back(in.ctx.cancel_token.get());
        }
        auto ex = co_await asio::this_coro::executor;
        co_await neograph::async::async_post(
            ex, "127.0.0.1", std::to_string(port_), "/stall", "{}",
            {}, false, {});
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    unsigned short port_;
    OperationTokensSeen* seen_;
};

class TokenObserverNode final : public GraphNode {
public:
    TokenObserverNode(std::string name, std::atomic<CancelToken*>* seen)
        : name_(std::move(name)), seen_(seen) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        seen_->store(in.ctx.cancel_token.get(), std::memory_order_release);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<CancelToken*>* seen_;
};

} // namespace

TEST(GraphEngineAsyncApi, RunAsyncMatchesSyncResult) {
    register_writer("hello");
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "t-1";

    auto sync_result = engine->run(cfg);

    asio::io_context io;
    RunResult async_result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            async_result = co_await engine->run_async(cfg);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(sync_result.output, async_result.output);
    EXPECT_FALSE(async_result.interrupted);
}

TEST(GraphEngineAsyncApi, RunAsyncPropagatesNodeException) {
    register_thrower();
    auto engine = GraphEngine::compile(minimal_graph("boom"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "t-2";

    try {
        engine->run(cfg);
        FAIL() << "expected NodeExecutionError";
    } catch (const NodeExecutionError& e) {
        EXPECT_EQ(e.node_name(), "boom");
        EXPECT_EQ(e.attempts(), 1);
        EXPECT_THROW(std::rethrow_exception(e.cause()), std::runtime_error);
    }

    asio::io_context io;
    std::exception_ptr captured;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await engine->run_async(cfg);
            } catch (...) {
                captured = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(captured);
    try {
        std::rethrow_exception(captured);
        FAIL() << "expected NodeExecutionError";
    } catch (const NodeExecutionError& e) {
        EXPECT_EQ(e.node_name(), "boom");
        EXPECT_EQ(e.attempts(), 1);
        EXPECT_THROW(std::rethrow_exception(e.cause()), std::runtime_error);
    }
}

TEST(GraphEngineAsyncApi, SubgraphFailureStillHonorsOuterRetry) {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NodeFactory::instance().register_type("subgraph_transient_123",
        [calls](const std::string& name, const json&, const NodeContext&) {
            class TransientNode final : public GraphNode {
            public:
                TransientNode(std::string name,
                              std::shared_ptr<std::atomic<int>> calls)
                    : name_(std::move(name)), calls_(std::move(calls)) {}
                asio::awaitable<NodeOutput> run(NodeInput) override {
                    if (calls_->fetch_add(1) < 2) {
                        throw std::runtime_error("inner transient failure");
                    }
                    co_return NodeOutput{};
                }
                std::string get_name() const override { return name_; }
            private:
                std::string name_;
                std::shared_ptr<std::atomic<int>> calls_;
            };
            return std::make_unique<TransientNode>(name, calls);
        });

    json inner = {
        {"name", "inner"},
        {"channels", json::object()},
        {"nodes", {{"deep", {{"type", "subgraph_transient_123"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "deep"}},
            {{"from", "deep"}, {"to", "__end__"}}
        })}
    };
    json outer = {
        {"name", "outer"},
        {"channels", json::object()},
        {"nodes", {{"shell", {{"type", "subgraph"},
                                {"definition", inner}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "shell"}},
            {{"from", "shell"}, {"to", "__end__"}}
        })},
        {"retry_policy", {{"max_retries", 2}, {"initial_delay_ms", 1}}}
    };

    auto engine = GraphEngine::compile(outer, NodeContext{});
    RunConfig cfg;
    cfg.thread_id = "subgraph-retry";
    EXPECT_NO_THROW(engine->run(cfg));
    EXPECT_EQ(calls->load(), 3);
}

TEST(GraphEngineAsyncApi, ResumeAsyncMatchesSyncResume) {
    // resume_async needs a checkpoint store + a thread that has a
    // checkpoint to resume from. Run once first to seed it, then drive
    // resume_async.
    register_writer("v");
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "t-resume";
    auto first = engine->run(cfg);
    ASSERT_FALSE(first.checkpoint_id.empty());

    asio::io_context io;
    RunResult resumed;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            resumed = co_await engine->resume_async("t-resume");
        },
        asio::detached);
    io.run();

    // Run that has already completed to END returns interrupted=false
    // with no further work — match the sync resume() contract.
    EXPECT_FALSE(resumed.interrupted);
}

TEST(GraphEngineAsyncApi, ConcurrentRunAsyncOnSharedIoContext) {
    // Multiple runs on one io_context. Today's wrapper executes them
    // serially (the inner sync run blocks the worker thread), but the
    // contract is "no lockup, no interleaving fault". When the engine
    // internals become coroutine-native, this test will start showing
    // real overlap — same assertions still hold.
    register_writer("v");
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{});

    asio::io_context io;
    std::atomic<int> done{0};
    constexpr int N = 5;

    for (int i = 0; i < N; ++i) {
        asio::co_spawn(
            io,
            [&, i]() -> asio::awaitable<void> {
                RunConfig cfg;
                cfg.thread_id = "t-" + std::to_string(i);
                auto r = co_await engine->run_async(cfg);
                if (!r.interrupted) {
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            },
            asio::detached);
    }
    io.run();

    EXPECT_EQ(done.load(), N);
}

TEST(GraphEngineAsyncApi, SharedParentCancelsBothOperationChildren) {
    ReleasableHttpServer server;
    OperationTokensSeen seen;
    NodeFactory::instance().register_type("operation_cancel_http",
        [&server, &seen](const std::string& name, const json&,
                         const NodeContext&) {
            return std::make_unique<StallingHttpNode>(
                name, server.port, &seen);
        });

    auto engine = GraphEngine::compile(
        minimal_graph("worker", "operation_cancel_http"), NodeContext{});
    auto parent = std::make_shared<CancelToken>();

    asio::io_context io;
    std::atomic<int> done{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> unexpected{0};
    std::atomic<bool> aborted_before_release{false};

    for (int i = 0; i < 2; ++i) {
        asio::co_spawn(
            io,
            [&, i]() -> asio::awaitable<void> {
                RunConfig cfg;
                cfg.thread_id = "shared-parent-" + std::to_string(i);
                cfg.cancel_token = parent;
                try {
                    (void)co_await engine->run_async(std::move(cfg));
                } catch (const CancelledException&) {
                    cancelled.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
                done.fetch_add(1, std::memory_order_release);
            },
            asio::detached);
    }

    std::thread canceller([&] {
        const auto requests_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server.requests.load(std::memory_order_acquire) < 2 &&
               std::chrono::steady_clock::now() < requests_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        parent->cancel();

        // On fixed code both operations abort before this fallback. On the
        // old code parent cancellation is not bound inside C++ run_async, so
        // release the mock response after a bounded wait to make the test fail
        // by assertion rather than hang forever.
        const auto cancel_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (done.load(std::memory_order_acquire) < 2 &&
               std::chrono::steady_clock::now() < cancel_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        aborted_before_release.store(
            done.load(std::memory_order_acquire) == 2,
            std::memory_order_release);
        server.release();
    });

    io.run();
    canceller.join();

    EXPECT_EQ(server.requests.load(), 2);
    EXPECT_EQ(done.load(), 2);
    EXPECT_EQ(cancelled.load(), 2)
        << "one parent cancel must abort both child-bound HTTP awaits";
    EXPECT_EQ(unexpected.load(), 0)
        << "operation_aborted must surface as CancelledException";
    EXPECT_TRUE(aborted_before_release.load())
        << "run_async cancellation must abort the socket await without the "
           "mock server releasing a response";

    std::lock_guard<std::mutex> lock(seen.mu);
    ASSERT_EQ(seen.tokens.size(), 2u);
    EXPECT_NE(seen.tokens[0], parent.get());
    EXPECT_NE(seen.tokens[1], parent.get());
    EXPECT_NE(seen.tokens[0], seen.tokens[1])
        << "concurrent runs must receive distinct operation children";
}

TEST(GraphEngineAsyncApi, RunUsesOperationChild) {
    std::atomic<CancelToken*> seen{nullptr};
    NodeFactory::instance().register_type("operation_cancel_run_sync",
        [&seen](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<TokenObserverNode>(name, &seen);
        });

    auto engine = GraphEngine::compile(
        minimal_graph("worker", "operation_cancel_run_sync"), NodeContext{});
    auto parent = std::make_shared<CancelToken>();
    RunConfig cfg;
    cfg.thread_id = "run-operation-child-sync";
    cfg.cancel_token = parent;

    (void)engine->run(cfg);

    ASSERT_NE(seen.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(seen.load(std::memory_order_acquire), parent.get());
}

TEST(GraphEngineAsyncApi, RunStreamUsesOperationChild) {
    std::atomic<CancelToken*> seen{nullptr};
    NodeFactory::instance().register_type("operation_cancel_stream_sync",
        [&seen](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<TokenObserverNode>(name, &seen);
        });

    auto engine = GraphEngine::compile(
        minimal_graph("worker", "operation_cancel_stream_sync"),
        NodeContext{});
    auto parent = std::make_shared<CancelToken>();
    RunConfig cfg;
    cfg.thread_id = "stream-operation-child-sync";
    cfg.cancel_token = parent;

    (void)engine->run_stream(cfg, [](const GraphEvent&) {});

    ASSERT_NE(seen.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(seen.load(std::memory_order_acquire), parent.get());
}

TEST(GraphEngineAsyncApi, RunStreamAsyncUsesOperationChild) {
    std::atomic<CancelToken*> seen{nullptr};
    NodeFactory::instance().register_type("operation_cancel_stream_async",
        [&seen](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<TokenObserverNode>(name, &seen);
        });

    auto engine = GraphEngine::compile(
        minimal_graph("worker", "operation_cancel_stream_async"),
        NodeContext{});
    auto parent = std::make_shared<CancelToken>();
    RunConfig cfg;
    cfg.thread_id = "stream-operation-child-async";
    cfg.cancel_token = parent;

    asio::io_context io;
    std::exception_ptr error;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await engine->run_stream_async(
                    cfg, [](const GraphEvent&) {});
            } catch (...) {
                error = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    EXPECT_FALSE(error);
    ASSERT_NE(seen.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(seen.load(std::memory_order_acquire), parent.get());
}
