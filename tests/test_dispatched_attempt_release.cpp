// A dispatched node attempt is released on a worker of the process-wide attempt pool, after its run has been woken and
// possibly after the run has ended and the executor the run ran on is gone (graph_executor.cpp, "Dispatched attempts").
// The attempt must hold nothing of that executor by then. Each test runs an engine on a strand of a thread_pool of its
// own, as the Program runtime runs its orchestrators, holds the release of the attempt through the test seam, joins and
// destroys the pool, and only then lets the release go, so that a sanitizer sees any executor, strand or timer of the
// destroyed pool that the attempt still held.
//
//   * UntimedAttempt              the attempt without a timeout, which the orchestrator waits for through one hand-off
//   * TimedAttemptByNodePolicy    a timeout from the node's retry policy; the body completes normally
//   * TimedAttemptByHostTimeout   the same timeout set through GraphEngine::set_node_attempt_timeout_ms
//   * AttemptSettledBeforeItStarted
//                                 the timeout passes before a worker picks the body up, so the body never runs
//   * DetachedAttemptOutlivesThePoolAndTheEngine
//                                 the body ignores cancellation past its stop deadline and finishes after the pool and
//                                 the engine are gone
#include <neograph/graph/engine.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include "dispatched_attempt_seam.h"

#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_future.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace neograph;
using namespace neograph::graph;
using std::chrono::seconds;

namespace {

// One worker thread serves every node attempt of this test binary, so that a test can keep a body from starting by
// occupying it. The attempt pool reads this when the first attempt is dispatched, which is after this initializer.
[[maybe_unused]] const bool attempt_pool_has_one_thread = [] {
#ifdef _WIN32
    _putenv_s("NEOGRAPH_NODE_ATTEMPT_THREADS", "1");
#else
    setenv("NEOGRAPH_NODE_ATTEMPT_THREADS", "1", 1);
#endif
    return true;
}();

// What a node saw, and the gate the test opens for a node that blocks.
struct Probe {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    gated    = false;  // the body blocks, ignoring cancellation, until the test opens the gate
    bool                    open     = false;
    bool                    started  = false;  // the body is running
    bool                    finished = false;  // the body has returned
    int                     bodies   = 0;      // how many times the body started

    template <typename Predicate>
    void await(Predicate predicate) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return predicate(*this); });
    }
    template <typename Change>
    void update(Change change) {
        {
            std::lock_guard lock(mutex);
            change(*this);
        }
        changed.notify_all();
    }
};

std::shared_ptr<Probe> probe_named(const std::string& name) {
    static std::mutex                                    mutex;
    static std::map<std::string, std::shared_ptr<Probe>> probes;
    std::lock_guard                                      lock(mutex);
    auto&                                                slot = probes[name];
    if (!slot) slot = std::make_shared<Probe>();
    return slot;
}

std::string unique_name(const char* prefix) {
    static std::atomic<int> next{0};
    return std::string(prefix) + "-" + std::to_string(++next);
}

class ReleaseNode final : public GraphNode {
public:
    ReleaseNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        const auto probe = probe_named(config_.at("probe").get<std::string>());
        probe->update([](Probe& seen) {
            seen.started = true;
            ++seen.bodies;
        });
        probe->await([](const Probe& seen) { return !seen.gated || seen.open; });
        probe->update([](Probe& seen) { seen.finished = true; });
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"value", name_});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

void register_types() {
    static const bool registered = [] {
        NodeFactory::instance().register_type(
            "release.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<ReleaseNode>(name, config);
            },
            json{{"type", "object"}});
        return true;
    }();
    (void)registered;
}

// START -> N -> END with the policy of N (an empty object for none).
json definition_of(const std::string& probe_name, json policy) {
    json definition{
        {"schema_version", 1},
        {"name", "release"},
        {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", nullptr}}}}},
        {"nodes", json{{"N", json{{"type", "release.node"}, {"probe", probe_name}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "N"}}, json{{"from", "N"}, {"to", "__end__"}}})}};
    if (!policy.empty()) definition["node_retry_policies"] = json{{"N", std::move(policy)}};
    return definition;
}

std::unique_ptr<GraphEngine> build(const json& definition) {
    register_types();
    EngineConfig config;
    config.stderr_notices = false;
    return GraphEngine::build(definition, std::move(config));
}

json timeout_of(int timeout_ms) { return json{{"attempt_timeout_ms", timeout_ms}, {"max_retries", 0}}; }

// Holds the release of the dispatched attempts of one run until the test lets it go.
struct ReleaseHold {
    std::mutex              mutex;
    std::condition_variable changed;
    std::string             run_id;
    bool                    holding  = false;  // a release of that run is held
    bool                    released = false;  // the test let the releases go

    void arm(std::string id) {
        std::lock_guard lock(mutex);
        run_id   = std::move(id);
        holding  = false;
        released = false;
    }
    void await_holding() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return holding; });
    }
    void let_go() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};
ReleaseHold release_hold;

void hold_release(std::string_view run_id) noexcept {
    auto&            hold = release_hold;
    std::unique_lock lock(hold.mutex);
    if (hold.run_id.empty() || run_id != hold.run_id) return;
    hold.holding = true;
    hold.changed.notify_all();
    hold.changed.wait(lock, [&hold] { return hold.released; });
}

// Notes when the orchestrator of the attempt of one run has decided that the attempt timed out.
struct TimeoutNote {
    std::mutex              mutex;
    std::condition_variable changed;
    std::string             run_id;
    bool                    fired = false;

    void arm(std::string id) {
        std::lock_guard lock(mutex);
        run_id = std::move(id);
        fired  = false;
    }
    void await_fired() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return fired; });
    }
    bool has_fired() {
        std::lock_guard lock(mutex);
        return fired;
    }
};
TimeoutNote timeout_note;

void note_timeout(std::string_view run_id) noexcept {
    auto&           note = timeout_note;
    std::lock_guard lock(note.mutex);
    if (note.run_id.empty() || run_id != note.run_id) return;
    note.fired = true;
    note.changed.notify_all();
}

// Installs the seams, and clears them again however the test ends, letting a held release go first.
struct Seam {
    Seam() {
        graph::detail::set_dispatched_attempt_release_observer_for_testing(&hold_release);
        graph::detail::set_dispatched_attempt_timeout_observer_for_testing(&note_timeout);
    }
    ~Seam() {
        release_hold.let_go();
        graph::detail::set_dispatched_attempt_release_observer_for_testing(nullptr);
        graph::detail::set_dispatched_attempt_timeout_observer_for_testing(nullptr);
        release_hold.arm({});
        timeout_note.arm({});
    }
};

// Opens the gate of a probe however the test ends, so that no body is left blocked.
struct OpenAtExit {
    std::shared_ptr<Probe> probe;
    ~OpenAtExit() {
        probe->update([](Probe& seen) { seen.open = true; });
    }
};

// `shared_scheduler`: a node without a timeout is dispatched to the attempt pool only for such a run (the Program
// runtime's); a node with one always is.
RunMetadata metadata_of(const std::string& run_id, bool shared_scheduler = false) {
    RunMetadata metadata;
    metadata.run_id           = run_id;
    metadata.shared_scheduler = shared_scheduler;
    return metadata;
}

// The release of the run's attempt is held, the pool of the host is joined and destroyed, and then the release goes:
// whatever the attempt still held of the pool is destroyed after the pool.
void destroy_the_pool_while_the_release_is_held(std::unique_ptr<asio::thread_pool>& pool) {
    release_hold.await_holding();
    pool->join();
    pool.reset();
    release_hold.let_go();
    EXPECT_TRUE(wait_for_node_attempts(seconds(30))) << "the attempt was not released";
}

void run_and_destroy_the_pool(GraphEngine& engine, const std::string& run_id, const std::string& probe_name,
                              bool shared_scheduler = false) {
    const Seam seam;
    release_hold.arm(run_id);
    auto pool   = std::make_unique<asio::thread_pool>(2);
    auto future = asio::co_spawn(asio::make_strand(*pool),
                                 engine.run_async(RunConfig{}, metadata_of(run_id, shared_scheduler)),
                                 asio::use_future);
    const auto result = future.get();
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_EQ(probe_named(probe_name)->bodies, 1);
    destroy_the_pool_while_the_release_is_held(pool);
}

}  // namespace

TEST(DispatchedAttemptRelease, UntimedAttempt) {
    const auto name   = unique_name("untimed");
    auto       engine = build(definition_of(name, json::object()));
    run_and_destroy_the_pool(*engine, name + "-run", name, true);
}

TEST(DispatchedAttemptRelease, TimedAttemptByNodePolicy) {
    const auto name   = unique_name("policy");
    auto       engine = build(definition_of(name, timeout_of(60000)));
    run_and_destroy_the_pool(*engine, name + "-run", name);
}

TEST(DispatchedAttemptRelease, TimedAttemptByHostTimeout) {
    const auto name   = unique_name("host");
    auto       engine = build(definition_of(name, json::object()));
    engine->set_node_attempt_timeout_ms("N", 60000);
    run_and_destroy_the_pool(*engine, name + "-run", name);
}

// The timeout (100 ms) passes while the only attempt worker is busy with another run's body, so the body of this run is
// picked up after its attempt has settled as timed out: it does not run, and it wakes the orchestrator that waits. The
// busy worker is let go only after the orchestrator has reported the timeout.
TEST(DispatchedAttemptRelease, AttemptSettledBeforeItStarted) {
    const Seam seam;
    const auto busy_name = unique_name("busy");
    const auto name      = unique_name("settled");
    const auto busy      = probe_named(busy_name);
    busy->update([](Probe& seen) { seen.gated = true; });
    const OpenAtExit open_busy{busy};
    auto             busy_engine = build(definition_of(busy_name, json::object()));
    auto             engine      = build(definition_of(name, timeout_of(100)));

    // The only attempt worker is now inside the body of the busy run.
    std::thread busy_run([&] { (void)busy_engine->run(RunConfig{}, metadata_of(busy_name + "-run", true)); });
    busy->await([](const Probe& seen) { return seen.started; });

    const auto run_id = name + "-run";
    release_hold.arm(run_id);
    timeout_note.arm(run_id);
    auto pool   = std::make_unique<asio::thread_pool>(2);
    auto future = asio::co_spawn(asio::make_strand(*pool), engine->run_async(RunConfig{}, metadata_of(run_id)),
                                 asio::use_future);
    // The attempt settles as timed out while its body waits for the worker: only then does the busy body let it go.
    timeout_note.await_fired();
    EXPECT_TRUE(timeout_note.has_fired());
    {
        std::lock_guard lock(busy->mutex);
        EXPECT_FALSE(busy->open) << "the worker was let go before the timeout was reported";
        EXPECT_FALSE(busy->finished);
    }
    busy->update([](Probe& seen) { seen.open = true; });

    EXPECT_THROW((void)future.get(), NodeExecutionError);
    EXPECT_EQ(probe_named(name)->bodies, 0) << "the body of the timed-out attempt must not have started";
    destroy_the_pool_while_the_release_is_held(pool);
    busy_run.join();
}

// The body ignores cancellation and outlives its stop deadline, so the run fails and the attempt is detached. The pool
// and the engine are destroyed while the body still runs; the body then finishes, and everything it held is released.
TEST(DispatchedAttemptRelease, DetachedAttemptOutlivesThePoolAndTheEngine) {
    const auto name  = unique_name("detached");
    const auto probe = probe_named(name);
    probe->update([](Probe& seen) { seen.gated = true; });
    const OpenAtExit open_at_exit{probe};
    auto             engine = build(definition_of(name, timeout_of(100)));

    auto pool   = std::make_unique<asio::thread_pool>(2);
    auto future = asio::co_spawn(asio::make_strand(*pool), engine->run_async(RunConfig{}, metadata_of(name + "-run")),
                                 asio::use_future);
    EXPECT_THROW((void)future.get(), NodeExecutionError);
    {
        std::lock_guard lock(probe->mutex);
        EXPECT_TRUE(probe->started);
        EXPECT_FALSE(probe->finished) << "the body must still run when the run has failed";
    }
    pool->join();
    pool.reset();
    engine.reset();

    probe->update([](Probe& seen) { seen.open = true; });
    probe->await([](const Probe& seen) { return seen.finished; });
    EXPECT_TRUE(wait_for_node_attempts(seconds(30))) << "the detached attempt was not released";
}
