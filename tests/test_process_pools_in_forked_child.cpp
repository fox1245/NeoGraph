// The pools and threads that the library keeps for the whole process, in a child made by fork. The child inherits the
// parent's pool object and none of its threads: work posted to it never runs, and destroying it at exit joins threads
// the child does not have. Each test uses one pool in the parent, makes a child that runs work on that pool and then
// leaves the way a program does (exit handlers and static destructors run), and uses the pool in the parent again.
//
//   * the pool of the synchronous checkpoint stores and of the blocking Program stores
//   * the pool of blocking tool calls
//   * the pool of timed node attempts, also with an attempt of the parent still running at the fork
//   * the pool that starts the connections of the PostgreSQL checkpoint store
//   * the holder of such an object: it makes no system call on the path of a process that already has its object
//
// A child reports through its exit status. No test waits for a bounded time: a child that does not end is ended with
// its test by the test's TIMEOUT property.
#include "fork_child.h"

#if defined(NEOGRAPH_TESTS_HAVE_FORK)

#include "../src/core/process_owned.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/executor.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/tool.h>
#if defined(NEOGRAPH_POOL_FORK_TESTS_HAVE_POSTGRES)
#include <neograph/graph/postgres_checkpoint.h>

#include <libpq-fe.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define NEOGRAPH_POOL_FORK_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NEOGRAPH_POOL_FORK_HAS_ASAN 1
#endif
#endif

#if defined(NEOGRAPH_POOL_FORK_HAS_ASAN)
// Tests in this binary exercise ProcessOwned pools across fork() (including ForkChildLeaves::Normally),
// where LeakSanitizer's StopTheWorld and exit leak scans fail on absent parent threads and
// intentionally abandoned ProcessOwned slots in the child; ASan and UBSan remain enabled.
extern "C" __attribute__((used, visibility("default"))) int __lsan_is_turned_off(void) { return 1; }
#endif

using namespace neograph;
using neograph::test::ForkChildLeaves;
using neograph::test::run_in_forked_child;

namespace {

// ---- Work on each pool: true when it ran to the end. ----

bool work_on_the_checkpoint_pool() {
    std::atomic<bool> ran{false};
    async::run_sync(graph::detail::run_on_blocking_pool([&ran] { ran.store(true); }));
    return ran.load();
}

class EchoTool final : public Tool {
public:
    ChatTool get_definition() const override {
        ChatTool definition;
        definition.name = "echo";
        return definition;
    }
    // Synchronous: Tool::execute_async runs it on the pool of blocking tool calls.
    std::string execute(const json&) override { return "echoed"; }
    std::string get_name() const override { return "echo"; }
};

bool work_on_the_tool_pool() {
    EchoTool tool;
    return async::run_sync(tool.execute_async(json::object())) == "echoed";
}

// A node whose body waits at a gate that the test opens, so that an attempt can be running at the fork.
struct Gate {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false;
    bool                    open    = true;

    void await_entered() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return entered; });
    }
    void let_go() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex);
        open    = false;
        entered = false;
    }
};

// Never destroyed: a child that is made while a thread of the parent waits at the gate inherits a waiter that no thread
// of the child will wake, and destroying a condition variable waits for its waiters.
Gate& gate() {
    static Gate* const instance = new Gate;
    return *instance;
}

class AttemptNode final : public graph::GraphNode {
public:
    AttemptNode(std::string name, bool gated) : name_(std::move(name)), gated_(gated) {}

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        if (gated_) {
            auto&            at = gate();
            std::unique_lock lock(at.mutex);
            at.entered = true;
            at.changed.notify_all();
            at.changed.wait(lock, [&at] { return at.open; });
        }
        graph::NodeOutput out;
        out.writes.push_back(graph::ChannelWrite{"value", name_});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        gated_;
};

std::unique_ptr<graph::GraphEngine> timed_engine(bool gated) {
    static const bool registered = [] {
        graph::NodeFactory::instance().register_type(
            "pool.fork.node",
            [](const std::string& name, const json& config, const graph::NodeContext&) {
                return std::make_unique<AttemptNode>(name, config.value("gated", false));
            },
            json{{"type", "object"}});
        return true;
    }();
    static_cast<void>(registered);
    const json definition{
        {"schema_version", 1},
        {"name", "pools"},
        {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", nullptr}}}}},
        {"nodes", json{{"N", json{{"type", "pool.fork.node"}, {"gated", gated}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "N"}}, json{{"from", "N"}, {"to", "__end__"}}})},
        // An attempt with a timeout runs on a worker of the attempt pool; the timeout itself is never reached.
        {"node_retry_policies", json{{"N", json{{"attempt_timeout_ms", 3600000}, {"max_retries", 0}}}}}};
    graph::EngineConfig config;
    config.stderr_notices = false;
    return graph::GraphEngine::build(definition, std::move(config));
}

bool work_on_the_attempt_pool() {
    const auto engine = timed_engine(false);
    return engine->run(graph::RunConfig{}).status() == graph::RunStatus::Completed;
}

// The parent used the pool; a child runs work on it and leaves normally; the parent uses it again.
template <class Work>
void a_child_works_on_the_pool_and_leaves_normally(Work work) {
    ASSERT_TRUE(work()) << "the parent's work before the fork";
    const auto end = run_in_forked_child([&work] { return work() ? 0 : 1; }, ForkChildLeaves::Normally);
    EXPECT_TRUE(end.ok()) << end;
    EXPECT_TRUE(work()) << "the parent's work after the child had gone";
}

}  // namespace

TEST(ProcessPoolsInForkedChild, TheCheckpointPoolServesTheChildAndTheChildLeavesNormally) {
    a_child_works_on_the_pool_and_leaves_normally(work_on_the_checkpoint_pool);
}

TEST(ProcessPoolsInForkedChild, TheToolPoolServesTheChildAndTheChildLeavesNormally) {
    a_child_works_on_the_pool_and_leaves_normally(work_on_the_tool_pool);
}

TEST(ProcessPoolsInForkedChild, TheAttemptPoolServesTheChildAndTheChildLeavesNormally) {
    ASSERT_TRUE(work_on_the_attempt_pool()) << "the parent's work before the fork";
    // Wait for the parent attempt's asynchronous teardown on attempt_pool to release its
    // ProcessAttemptLease before fork(), so fork() does not snapshot an active worker thread
    // inside allocator locks (e.g. ASan SizeClassAllocator64).
    ASSERT_TRUE(graph::wait_for_node_attempts()) << "the parent's attempt was not released before fork";
    const auto end = run_in_forked_child([] { return work_on_the_attempt_pool() ? 0 : 1; },
                                         ForkChildLeaves::Normally);
    EXPECT_TRUE(end.ok()) << end;
    EXPECT_TRUE(work_on_the_attempt_pool()) << "the parent's work after the child had gone";
    EXPECT_TRUE(graph::wait_for_node_attempts()) << "the parent's attempt was not released after child";
}

// An attempt of the parent is running on a worker of the pool when the child is made: the pool is busy in the parent
// and has no thread in the child.
TEST(ProcessPoolsInForkedChild, TheAttemptPoolServesTheChildWhileAnAttemptOfTheParentIsRunning) {
    gate().close();
    const auto  blocked = timed_engine(true);
    const auto  quick   = timed_engine(false);
    graph::RunStatus outcome = graph::RunStatus::Interrupted;
    std::thread      parents_run([&] { outcome = blocked->run(graph::RunConfig{}).status(); });
    gate().await_entered();

    const auto end = run_in_forked_child(
        [&quick] { return quick->run(graph::RunConfig{}).status() == graph::RunStatus::Completed ? 0 : 1; },
        ForkChildLeaves::Normally);
    EXPECT_TRUE(end.ok()) << end;

    gate().let_go();
    parents_run.join();
    EXPECT_EQ(outcome, graph::RunStatus::Completed);
    EXPECT_TRUE(graph::wait_for_node_attempts()) << "the parent's attempt was not released";
    EXPECT_TRUE(work_on_the_attempt_pool());
}

// The holder of a process-wide object: a child never uses the parent's object, and the path of a process that has its
// object makes no system call. Strict seccomp ends the process on any call except read, write, exit and sigreturn.
TEST(ProcessOwnedHolder, ACopyInTheChildIsNotTheParentsObject) {
    detail::ProcessOwned<int> holder;
    const auto                make = [] { return std::make_unique<int>(7); };
    int&                      mine = holder.get(make);
    ASSERT_EQ(holder.current(), &mine);
    const auto end = run_in_forked_child([&] {
        if (holder.current() != nullptr) return 1;  // the parent's object is not this process's
        int& own = holder.get(make);
        if (&own == &mine) return 2;                // the child made one of its own
        if (holder.current() != &own || &holder.get(make) != &own) return 3;
        return 0;
    });
    EXPECT_TRUE(end.ok()) << end;
    EXPECT_EQ(holder.current(), &mine);
    EXPECT_EQ(&holder.get(make), &mine);
}

#if defined(__linux__) && defined(PR_SET_SECCOMP) && defined(SECCOMP_MODE_STRICT)
TEST(ProcessOwnedHolder, ThePathOfAProcessThatHasItsObjectMakesNoSystemCall) {
    constexpr int kNoSeccomp = 77;
    detail::ProcessOwned<int> holder;
    const auto                make = [] { return std::make_unique<int>(7); };
    holder.get(make);
    const auto end = run_in_forked_child([&] {
        // The child's own object, and every symbol the loop needs, are made and resolved before the filter.
        long checked = holder.get(make) + (holder.current() ? 1 : 0);
        static_cast<void>(::syscall(SYS_getpid));
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0) return kNoSeccomp;
        long sum = 0;
        for (int i = 0; i < 100000; ++i) sum += holder.get(make) + (holder.current() ? 1 : 0);
        // exit_group is not allowed in this mode, so the status goes out through the thread's own exit.
        ::syscall(SYS_exit, sum == checked * 100000 ? 0 : 1);
        return 2;
    });
    if (end.exited && end.status == kNoSeccomp) GTEST_SKIP() << "strict seccomp is not available here";
    EXPECT_TRUE(end.ok()) << end << " (a signal means a system call was made)";
}
#endif

#if defined(NEOGRAPH_POOL_FORK_TESTS_HAVE_POSTGRES)
// ---- The pool that starts the PostgreSQL connections. Its tests run only with the test database's URL. ----

namespace {

// A store whose only connection has been ended by the server reconnects on its next asynchronous operation, and that
// connection is started on the pool. The store's connection carries a name of its own, so only it is ended.
bool reconnect_through_the_connection_pool(const char* url) {
    const std::string name = "neograph_pool_fork_" + std::to_string(::getpid());
    setenv("PGAPPNAME", name.c_str(), 1);
    graph::PostgresCheckpointStore store(url, 1);
    unsetenv("PGAPPNAME");
    const auto reconnects = store.reconnect_count();

    PGconn* killer = PQconnectdb(url);
    if (PQstatus(killer) != CONNECTION_OK) {
        PQfinish(killer);
        return false;
    }
    const std::string ended = "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = '" +
                              name + "' AND pid <> pg_backend_pid()";
    const std::string alive = "SELECT count(*) FROM pg_stat_activity WHERE application_name = '" + name +
                              "' AND pid <> pg_backend_pid()";
    PQclear(PQexec(killer, ended.c_str()));
    // The server ends the session on its own time; the loop asks until the session is gone, with no pause of its own.
    for (;;) {
        PGresult* count = PQexec(killer, alive.c_str());
        const bool gone = PQresultStatus(count) == PGRES_TUPLES_OK && std::string(PQgetvalue(count, 0, 0)) == "0";
        PQclear(count);
        if (gone) break;
    }
    PQfinish(killer);

    static_cast<void>(async::run_sync(store.load_latest_async("neograph-pool-fork")));
    return store.reconnect_count() == reconnects + 1;
}

class PostgresConnectionPoolInForkedChild : public ::testing::Test {
protected:
    void SetUp() override {
        url_ = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
        if (!url_ || !*url_) GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set";
    }
    const char* url_ = nullptr;
};

}  // namespace

TEST_F(PostgresConnectionPoolInForkedChild, TheChildStartsItsConnectionsOnAPoolOfItsOwn) {
    ASSERT_TRUE(reconnect_through_the_connection_pool(url_)) << "the parent's reconnect before the fork";
    const auto end = run_in_forked_child(
        [this] { return reconnect_through_the_connection_pool(url_) ? 0 : 1; }, ForkChildLeaves::Normally);
    EXPECT_TRUE(end.ok()) << end;
    EXPECT_TRUE(reconnect_through_the_connection_pool(url_)) << "the parent's reconnect after the child had gone";
}
#endif

#endif
