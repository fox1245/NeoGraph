// The last reference to a GraphEngine can be dropped by code running on one of
// the engine's own fan-out workers: a failed branch parks its exception in the
// coroutine frame that is still unwinding on a worker after the run has
// returned, and when that exception owns the host object that owned the engine
// (a Python exception whose traceback holds the engine, say), the worker
// releases the engine. The engine's destructor then destroys its thread pool
// on a thread of that pool. Joining a pool from one of its own threads skips
// that thread, so the pool was freed under the worker, which went on to use it
// as it left its handler.
//
// The pool of an engine destroyed on one of its workers is destroyed by a
// thread outside it, once the worker has left its handler.

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>

#include <asio/awaitable.hpp>
#include <asio/execution/context.hpp>
#include <asio/post.hpp>
#include <asio/query.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

namespace {

struct Observer {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    run_returned = false;   // the test's run() has returned
    bool                    released     = false;   // the engine's destructor has returned
    bool                    pending_destroyed = false;
    std::thread::id         releaser;                // the worker that dropped the last reference
    std::thread::id         pending_destroyer;       // whoever destroyed the task left in the pool
    std::shared_ptr<GraphEngine> engine;             // the only reference

    template <typename Predicate>
    bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 20s, predicate);
    }
    void update(void (*change)(Observer&)) {
        {
            std::lock_guard lock(mutex);
            change(*this);
        }
        changed.notify_all();
    }
};

// A task the pool still holds when the engine goes away. A pool destroys such
// a task when it shuts down, on the thread that destroys the pool.
struct PendingTask {
    std::shared_ptr<Observer> observer;
    explicit PendingTask(std::shared_ptr<Observer> o) : observer(std::move(o)) {}
    ~PendingTask() {
        {
            std::lock_guard lock(observer->mutex);
            observer->pending_destroyed = true;
            observer->pending_destroyer = std::this_thread::get_id();
        }
        observer->changed.notify_all();
    }
};

std::shared_ptr<Observer> g_observer;

// One fan-out branch. It leaves a task on the engine's pool which, once the
// test has finished its run and holds no reference, drops the engine's last
// one: on a worker of the engine's pool, as the unwinding of a failed branch
// does. (Not on the branch's strand: the branch's own completion queues behind
// it there.)
class ReleasingNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        auto branch = co_await asio::this_coro::executor;
        auto& pool  = static_cast<asio::thread_pool&>(
            asio::query(branch, asio::execution::context));
        auto executor = pool.get_executor();
        asio::post(executor, [executor, observer = g_observer] {
            if (!observer->wait([&] { return observer->run_returned; })) return;
            {
                std::lock_guard lock(observer->mutex);
                observer->releaser = std::this_thread::get_id();
            }
            asio::post(executor, [task = std::make_shared<PendingTask>(observer)] {});
            std::shared_ptr<GraphEngine> engine;
            {
                std::lock_guard lock(observer->mutex);
                engine = std::move(observer->engine);
            }
            engine.reset();
            observer->update([](Observer& o) { o.released = true; });
        });
        co_return NodeOutput{};
    }
    std::string get_name() const override { return "releasing"; }
};

class PlainNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return "plain"; }
};

}  // namespace

TEST(EngineThreadPoolOwner, DestroyedOnItsOwnWorkerTheEnginePoolIsDestroyedOutsideThePool) {
    g_observer = std::make_shared<Observer>();
    auto& factory = NodeFactory::instance();
    factory.register_type("pool_owner.releasing",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ReleasingNode>();
        });
    factory.register_type("pool_owner.plain",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlainNode>();
        });

    const json definition = {
        {"name", "pool_owner"},
        {"channels", {{"unused", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"releasing", {{"type", "pool_owner.releasing"}}},
                   {"plain", {{"type", "pool_owner.plain"}}}}},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "releasing"}},
            json{{"from", "__start__"}, {"to", "plain"}},
            json{{"from", "releasing"}, {"to", "__end__"}},
            json{{"from", "plain"}, {"to", "__end__"}}})}};

    auto observer = g_observer;
    {
        NodeContext context;
        auto        compiled = GraphEngine::compile(definition, context);
        observer->engine     = std::shared_ptr<GraphEngine>(compiled.release());
    }
    std::weak_ptr<GraphEngine> engine_alive = observer->engine;
    GraphEngine*               engine       = observer->engine.get();
    // A pool with several workers: the fan-out runs its two branches on it.
    engine->set_worker_count(4);

    RunConfig config;
    config.thread_id = "pool-owner";
    const auto result = engine->run(config);
    EXPECT_FALSE(result.interrupted);

    // The test holds no reference and uses the engine no more; the worker's
    // task owns the only reference and drops it now.
    observer->update([](Observer& o) { o.run_returned = true; });
    ASSERT_TRUE(observer->wait([&] { return observer->released; }))
        << "the worker never returned from destroying the engine";
    ASSERT_TRUE(observer->wait([&] { return observer->pending_destroyed; }))
        << "the pool never destroyed the task it still held";

    EXPECT_TRUE(engine_alive.expired());
    std::lock_guard lock(observer->mutex);
    EXPECT_NE(observer->pending_destroyer, observer->releaser)
        << "the pool was destroyed on one of its own threads";
    g_observer.reset();
}
