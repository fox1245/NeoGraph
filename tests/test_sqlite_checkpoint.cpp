// Unit tests for SqliteCheckpointStore.
//
// Portable backend contract tests use isolated in-memory databases. The native
// history restart regression uses a temporary file and independently protected
// archive/key custody so a new engine must restore genuine provider seals.

#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/graph/engine.h>
#include <neograph/provider_outcome_codec.h>
#include "fixtures/typed_provider.h"
#include <codecs/messages.h>
#include <codecs/responses.h>
#include <core/native.h>
#include <atomic>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <barrier>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace neograph::graph;
using json = neograph::json;

TEST(SqliteCheckpointConcurrencyTest, IndependentConnectionsAppendWithoutSnapshotUpgradeLoss) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("neograph-pending-" + Checkpoint::generate_id() + ".sqlite");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto& suffix : {"", "-wal", "-shm"})
                std::filesystem::remove(path.string() + suffix, ignored);
        }
    } cleanup{path};
    SqliteCheckpointStore first(path.string()), second(path.string());
    std::barrier          rendezvous(2);
    auto append = [&](SqliteCheckpointStore& connection, const std::string& prefix) {
        unsigned failures = 0;
        for (int i = 0; i < 40; ++i) {
            PendingWrite write;
            write.task_id   = prefix + std::to_string(i);
            write.node_name = "writer";
            write.writes    = json::array();
            write.step      = i;
            rendezvous.arrive_and_wait();
            try {
                connection.put_writes("shared", "parent", write);
            } catch (const std::exception&) {
                ++failures;
            }
        }
        return failures;
    };
    auto one = std::async(std::launch::async, [&] { return append(first, "a"); });
    auto two = std::async(std::launch::async, [&] { return append(second, "b"); });
    EXPECT_EQ(one.get() + two.get(), 0U);
    const auto writes = first.get_writes("shared", "parent");
    ASSERT_EQ(writes.size(), 80U);
    std::set<std::string> tasks;
    for (const auto& write : writes)
        tasks.insert(write.task_id);
    EXPECT_EQ(tasks.size(), 80U);
}

namespace {

int current_process_id() {
#ifdef _WIN32
    return ::_getpid();
#else
    return ::getpid();
#endif
}

// Build a Checkpoint whose channel_values matches GraphState::serialize().
Checkpoint make_state_cp(const std::string& thread_id,
                         int step,
                         const std::map<std::string, std::pair<json, uint64_t>>& channels,
                         CheckpointPhase phase = CheckpointPhase::Completed) {
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.step = step;
    cp.timestamp = step * 1000 + 1;
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = phase;
    cp.current_node = "test_node";

    json cv = json::object();
    json chs = json::object();
    for (const auto& [name, vv] : channels) {
        json entry = json::object();
        entry["value"] = vv.first;
        entry["version"] = vv.second;
        chs[name] = entry;
    }
    cv["channels"] = chs;
    cv["global_version"] = static_cast<uint64_t>(channels.size());
    cp.channel_values = cv;
    return cp;
}

class SqliteCheckpointTest : public ::testing::Test {
protected:
    std::unique_ptr<SqliteCheckpointStore> store;

    void SetUp() override {
        // ":memory:" gives every test its own private DB. No coordination
        // with other tests, no leftover state, no filesystem touch.
        store = std::make_unique<SqliteCheckpointStore>(":memory:");
    }
};

} // namespace

TEST_F(SqliteCheckpointTest, SaveAndLoadLatestRoundTrip) {
    auto cp = make_state_cp("t", 0, {{"x", {42, 1}}, {"msg", {"hi", 2}}});
    store->save(cp);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 42);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["version"].get<uint64_t>(), 1u);
    EXPECT_EQ(loaded->channel_values["channels"]["msg"]["value"].get<std::string>(), "hi");
}

TEST_F(SqliteCheckpointTest, EphemeralResumeGuardSurvivesSaveAndLoad) {
    const ChannelLifecyclePolicy ephemeral{
        ChannelRetentionPolicy::Unbounded, 0, ChannelPersistencePolicy::Ephemeral};
    GraphState resumed;
    resumed.init_channel("scratch", ReducerType::OVERWRITE, nullptr, "initial", ephemeral);
    auto cp = make_state_cp("t", 0, {{"durable", {42, 1}}});
    cp.metadata = {{"_neograph_ephemeral_guard", {{"scratch", false}}}};
    store->save(cp);
    auto safe = store->load_latest("t");
    ASSERT_TRUE(safe);
    EXPECT_NO_THROW(resumed.restore_checkpoint(
        safe->channel_values, safe->metadata["_neograph_ephemeral_guard"]));

    cp = make_state_cp("t", 1, {{"durable", {43, 2}}});
    cp.metadata = {{"_neograph_ephemeral_guard", {{"scratch", true}}}};
    store->save(cp);
    auto unsafe = store->load_latest("t");
    ASSERT_TRUE(unsafe);
    EXPECT_THROW(resumed.restore_checkpoint(
        unsafe->channel_values, unsafe->metadata["_neograph_ephemeral_guard"]),
        std::runtime_error);
}

TEST(SqliteCheckpointWriteGuardTest, RejectsStaleWritesInsideWriterTransaction) {
    bool allowed = true;
    int guarded_writes = 0;
    SqliteCheckpointStore guarded(":memory:", std::chrono::seconds(5),
        [&](sqlite3* db, const std::string& thread_id) {
            EXPECT_EQ(thread_id, "t");
            EXPECT_EQ(sqlite3_txn_state(db, "main"), SQLITE_TXN_WRITE);
            ++guarded_writes;
            if (!allowed) throw std::runtime_error("stale checkpoint owner");
        });
    const auto first = make_state_cp("t", 0, {{"x", {1, 1}}});
    guarded.save(first);
    PendingWrite write;
    write.task_id = "task-1";
    write.node_name = "reason";
    write.writes = json::array();
    guarded.put_writes("t", first.id, write);
    const auto blobs_before = guarded.blob_count();
    ASSERT_EQ(guarded.get_writes("t", first.id).size(), 1U);

    allowed = false;
    EXPECT_THROW(guarded.save(make_state_cp("t", 1, {{"x", {2, 2}}})),
                 std::runtime_error);
    EXPECT_THROW(guarded.put_writes("t", first.id, write), std::runtime_error);
    EXPECT_THROW(guarded.clear_writes("t", first.id), std::runtime_error);
    EXPECT_THROW(guarded.delete_thread("t"), std::runtime_error);
    EXPECT_THROW(guarded.drop_schema(), std::logic_error);
    ASSERT_TRUE(guarded.load_latest("t"));
    EXPECT_EQ(guarded.load_latest("t")->id, first.id);
    EXPECT_EQ(guarded.get_writes("t", first.id).size(), 1U);
    EXPECT_EQ(guarded.blob_count(), blobs_before);
    EXPECT_EQ(guarded_writes, 6);

    allowed = true;
    guarded.clear_writes("t", first.id);
    EXPECT_TRUE(guarded.get_writes("t", first.id).empty());
}

TEST(SqliteCheckpointTransactionTest, RolledBackByTriggerPreservesOriginalError) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("neograph-transaction-" + Checkpoint::generate_id() + ".sqlite");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto& suffix : {"", "-wal", "-shm"})
                std::filesystem::remove(path.string() + suffix, ignored);
        }
    } cleanup{path};

    SqliteCheckpointStore store(path.string());
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open(path.string().c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> observer(raw, &sqlite3_close);
    ASSERT_EQ(opened, SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(observer.get(),
        "CREATE TRIGGER reject_checkpoint BEFORE INSERT ON neograph_checkpoints "
        "BEGIN SELECT RAISE(ROLLBACK, 'checkpoint blocked'); END;",
        nullptr, nullptr, nullptr), SQLITE_OK);

    const auto cp = make_state_cp("t", 0, {{"x", {42, 1}}});
    try {
        store.save(cp);
        FAIL() << "trigger should reject the checkpoint";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("checkpoint insert failed"),
                  std::string::npos);
    }
    EXPECT_EQ(store.blob_count(), 0U);
    EXPECT_FALSE(store.load_latest("t").has_value());

    ASSERT_EQ(sqlite3_exec(observer.get(), "DROP TRIGGER reject_checkpoint",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    store.save(cp);
    ASSERT_TRUE(store.load_latest("t"));
    EXPECT_EQ(store.load_latest("t")->id, cp.id);
}

TEST_F(SqliteCheckpointTest, LoadByIdReturnsCheckpoint) {
    auto cp = make_state_cp("t", 0, {{"x", {99, 5}}});
    store->save(cp);

    auto loaded = store->load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 99);
}

TEST_F(SqliteCheckpointTest, LoadLatestReturnsNewest) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    auto cp3 = make_state_cp("t", 2, {{"x", {3, 3}}});
    store->save(cp3);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp3.id);
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 3);
}

// Headline feature parity with PostgresCheckpointStore: the same dedup
// guarantee. 3 saves where one channel changes per step → 3 distinct
// counter blobs + 1 shared config blob = 4 total, not 6.
TEST_F(SqliteCheckpointTest, BlobsDedupedAcrossSteps) {
    json config = json::object();
    config["model"] = "claude";

    store->save(make_state_cp("t", 0, {{"counter", {1, 1}}, {"config", {config, 2}}}));
    store->save(make_state_cp("t", 1, {{"counter", {2, 3}}, {"config", {config, 2}}}));
    store->save(make_state_cp("t", 2, {{"counter", {3, 4}}, {"config", {config, 2}}}));

    EXPECT_EQ(store->blob_count(), 4u);
}

TEST_F(SqliteCheckpointTest, IdenticalSavesShareBlobs) {
    store->save(make_state_cp("t", 0, {{"x", {7, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {7, 1}}}));  // same val+ver
    EXPECT_EQ(store->blob_count(), 1u);
}

TEST_F(SqliteCheckpointTest, ListReturnsNewestFirst) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    store->save(make_state_cp("t", 2, {{"x", {3, 3}}}));

    auto cps = store->list("t");
    ASSERT_EQ(cps.size(), 3u);
    EXPECT_EQ(cps[0].channel_values["channels"]["x"]["value"].get<int>(), 3);
    EXPECT_EQ(cps[2].channel_values["channels"]["x"]["value"].get<int>(), 1);
}

TEST_F(SqliteCheckpointTest, ListRespectsLimit) {
    store->save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("t", 1, {{"x", {2, 2}}}));
    store->save(make_state_cp("t", 2, {{"x", {3, 3}}}));

    auto cps = store->list("t", 2);
    EXPECT_EQ(cps.size(), 2u);
}

TEST_F(SqliteCheckpointTest, ListIsolatedByThread) {
    store->save(make_state_cp("ta", 0, {{"x", {1, 1}}}));
    store->save(make_state_cp("tb", 0, {{"x", {2, 1}}}));
    EXPECT_EQ(store->list("ta").size(), 1u);
    EXPECT_EQ(store->list("tb").size(), 1u);
}

TEST_F(SqliteCheckpointTest, DeleteThreadDropsCheckpointsAndBlobs) {
    store->save(make_state_cp("ta", 0, {{"x", {1, 1}}, {"y", {2, 2}}}));
    store->save(make_state_cp("tb", 0, {{"y", {2, 2}}}));
    EXPECT_EQ(store->blob_count(), 3u);

    store->delete_thread("ta");

    EXPECT_FALSE(store->load_latest("ta").has_value());
    EXPECT_TRUE(store->load_latest("tb").has_value());
    EXPECT_EQ(store->blob_count(), 1u);
}

TEST_F(SqliteCheckpointTest, BarrierStateRoundTrips) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    cp.barrier_state["join_node"] = {"upstream_a", "upstream_b"};
    cp.barrier_state["another"] = {"only_one"};
    store->save(cp);

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->barrier_state.size(), 2u);
    EXPECT_EQ(loaded->barrier_state["join_node"].size(), 2u);
    EXPECT_EQ(loaded->barrier_state["join_node"].count("upstream_a"), 1u);
    EXPECT_EQ(loaded->barrier_state["another"].count("only_one"), 1u);
}

TEST_F(SqliteCheckpointTest, AllPhasesRoundTrip) {
    int step = 0;
    for (auto p : {CheckpointPhase::Before, CheckpointPhase::After,
                   CheckpointPhase::Completed, CheckpointPhase::NodeInterrupt,
                   CheckpointPhase::Updated}) {
        auto cp = make_state_cp("t", step++, {{"x", {step, 1}}}, p);
        store->save(cp);
        auto loaded = store->load_by_id(cp.id);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->interrupt_phase, p)
            << "phase " << to_string(p) << " did not round-trip";
    }
}

TEST_F(SqliteCheckpointTest, NextNodesRoundTrip) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    cp.next_nodes = {"a", "b", "c"};
    store->save(cp);
    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->next_nodes.size(), 3u);
    EXPECT_EQ(loaded->next_nodes[0], "a");
    EXPECT_EQ(loaded->next_nodes[2], "c");
}

// ── Pending writes ────────────────────────────────────────────────────

TEST_F(SqliteCheckpointTest, PutGetClearWritesRoundTrip) {
    PendingWrite pw;
    pw.task_id = "task-1";
    pw.task_path = "s0:executor_1";
    pw.node_name = "executor";
    json writes = json::array();
    json w = json::object();
    w["channel"] = "messages";
    w["value"] = "hello";
    writes.push_back(w);
    pw.writes = writes;
    pw.command = json();
    pw.sends = json::array();
    pw.step = 5;
    pw.timestamp = 12345;

    store->put_writes("t", "parent-cp", pw);

    auto loaded = store->get_writes("t", "parent-cp");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].task_id, "task-1");
    EXPECT_EQ(loaded[0].step, 5);
    EXPECT_EQ(loaded[0].writes[0]["channel"].get<std::string>(), "messages");

    store->clear_writes("t", "parent-cp");
    EXPECT_EQ(store->get_writes("t", "parent-cp").size(), 0u);
}

TEST_F(SqliteCheckpointTest, PendingWritesPreserveOrder) {
    for (int i = 0; i < 5; ++i) {
        PendingWrite pw;
        pw.task_id = "task-" + std::to_string(i);
        pw.task_path = "p" + std::to_string(i);
        pw.node_name = "n";
        pw.writes = json::array();
        pw.command = json();
        pw.sends = json::array();
        pw.step = 0;
        pw.timestamp = i;
        store->put_writes("t", "parent", pw);
    }
    auto loaded = store->get_writes("t", "parent");
    ASSERT_EQ(loaded.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(loaded[i].task_id, "task-" + std::to_string(i));
    }
}

TEST_F(SqliteCheckpointTest, DeleteThreadClearsPendingWrites) {
    PendingWrite pw;
    pw.task_id = "t1";
    pw.task_path = "p";
    pw.node_name = "n";
    pw.writes = json::array();
    pw.command = json();
    pw.sends = json::array();
    pw.step = 0;
    pw.timestamp = 0;
    store->put_writes("ta", "parent", pw);
    store->put_writes("tb", "parent", pw);

    store->delete_thread("ta");
    EXPECT_EQ(store->get_writes("ta", "parent").size(), 0u);
    EXPECT_EQ(store->get_writes("tb", "parent").size(), 1u);
}

TEST_F(SqliteCheckpointTest, ResaveSameIdUpdatesInPlace) {
    auto cp = make_state_cp("t", 0, {{"x", {1, 1}}});
    store->save(cp);

    cp.next_nodes = {"new_target"};
    store->save(cp);

    auto loaded = store->load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->next_nodes.size(), 1u);
    EXPECT_EQ(loaded->next_nodes[0], "new_target");
}

TEST_F(SqliteCheckpointTest, LoadLatestEmptyReturnsNullopt) {
    EXPECT_FALSE(store->load_latest("never-saved").has_value());
}

TEST_F(SqliteCheckpointTest, LoadByIdMissingReturnsNullopt) {
    EXPECT_FALSE(store->load_by_id("nonexistent-uuid").has_value());
}

TEST_F(SqliteCheckpointTest, NestedJsonRoundTrips) {
    json msgs = json::array();
    json m1 = json::object();
    m1["role"] = "user";
    m1["content"] = "안녕하세요";
    msgs.push_back(m1);
    json m2 = json::object();
    m2["role"] = "assistant";
    m2["content"] = "Hi!";
    msgs.push_back(m2);

    auto cp = make_state_cp("t", 0, {{"messages", {msgs, 1}}});
    store->save(cp);
    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    auto loaded_msgs = loaded->channel_values["channels"]["messages"]["value"];
    ASSERT_EQ(loaded_msgs.size(), 2u);
    EXPECT_EQ(loaded_msgs[0]["content"].get<std::string>(), "안녕하세요");
    EXPECT_EQ(loaded_msgs[1]["role"].get<std::string>(), "assistant");
}

// File-based variant — proves the same code path works against a real
// fs file, not just :memory:. Uses a tmpfile path that's cleaned up
// after the test.
TEST(SqliteCheckpointTest_File, FileBackedRoundTrip) {
    const auto filename =
        "neograph_test_" + std::to_string(current_process_id()) + "_" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count()) +
        ".db";
    std::string path = (std::filesystem::temp_directory_path() / filename).string();
    // RAII cleanup so an early ASSERT_* doesn't orphan /tmp files.
    // Pre-fix this only ran on the happy path (after the assertions).
    struct PathCleanup {
        std::string path;
        ~PathCleanup() {
            std::remove(path.c_str());
            std::remove((path + "-wal").c_str());
            std::remove((path + "-shm").c_str());
        }
    };
    PathCleanup cleanup{path};

    {
        SqliteCheckpointStore s(path);
        Checkpoint cp;
        cp.id = Checkpoint::generate_id();
        cp.thread_id = "tf";
        cp.step = 0;
        cp.timestamp = 1;
        cp.next_nodes = {"__end__"};
        cp.interrupt_phase = CheckpointPhase::Completed;
        json chs = json::object();
        json entry = json::object();
        entry["value"] = "persisted";
        entry["version"] = 1;
        chs["x"] = entry;
        cp.channel_values = json::object();
        cp.channel_values["channels"] = chs;
        cp.channel_values["global_version"] = 1;
        s.save(cp);
    }
    // Re-open the same file and verify the cp is still there.
    {
        SqliteCheckpointStore s(path);
        auto loaded = s.load_latest("tf");
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"]
                      .get<std::string>(), "persisted");
    }
    // RAII PathCleanup runs at scope exit (covering both happy path
    // and ASSERT_* early-return).
}

#if defined(__unix__) || defined(__APPLE__)
namespace {

class NativeCheckpointFiles {
public:
    NativeCheckpointFiles() {
        path_ = std::filesystem::temp_directory_path() /
                ("ng-native-checkpoint-" + Checkpoint::generate_id());
        if (!std::filesystem::create_directory(path_))
            throw std::runtime_error("cannot reserve native checkpoint fixture directory");
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    ~NativeCheckpointFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string database() const { return (path_ / "checkpoint.sqlite").string(); }
    std::string archive() const { return (path_ / "archive").string(); }
    std::string key() const { return (path_ / "archive.key").string(); }
private:
    std::filesystem::path path_;
};

struct DurableNativeProbe {
    unsigned provider_calls = 0;
    std::vector<sp::messages::Request> requests;
};

// Capture real SDK authority before any graph dispatch. The graph's provider
// operation later prepares exactly once with these same immutable controls.
sp::runtime::Result durable_native_outcome(const sp::descriptor::ValidatedDescriptor& descriptor) {
    sp::messages::Request request;
    request.model = "fixture-model";
    request.account_scope = "fixture-account";
    request.max_tokens = 64;
    request.messages = {neograph::test::message("message", sp::Role::User)};
    request.tools = {{"read", "read fixture", neograph::test::document(R"({"type":"object"})"), {}, {}}};
    auto encoded = sp::messages::encode(descriptor, request, false);
    if (const auto* error = std::get_if<sp::Error>(&encoded))
        throw std::logic_error(error->safe_message);
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator,
        std::get<sp::messages::EncodedRequest>(std::move(encoded)).context);
    codec.buffered(R"({"id":"msg-native","type":"message","role":"assistant","model":"fixture-model",
        "content":[{"type":"thinking","thinking":"inspect","signature":"fixture-signature"},
                   {"type":"tool_use","id":"call-native","name":"read","input":{"path":"a"}},
                   {"type":"text","text":"pending"}],
        "stop_reason":"tool_use","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}})", {});
    codec.finish();
    auto outcome = accumulator.take_outcome();
    if (!outcome || !std::holds_alternative<sp::Completion>(*outcome))
        throw std::logic_error("native checkpoint fixture response was not admitted");
    return std::make_shared<const sp::Outcome>(std::move(*outcome));
}

class DurableNativeProvider final : public neograph::test::LocalProvider {
public:
    DurableNativeProvider(std::shared_ptr<DurableNativeProbe> probe,
                          sp::descriptor::ValidatedDescriptor descriptor,
                          sp::runtime::Result result)
        : LocalProvider(
            [probe, result = std::move(result)](
                neograph::ProviderRequest request, const neograph::PreparedProviderRequest&,
                const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                if (++probe->provider_calls > 1)
                    throw std::logic_error("native checkpoint replay dispatched an extra provider turn");
                probe->requests.push_back(std::get<sp::messages::Request>(std::move(request.payload)));
                co_return result;
            }, "durable-native-checkpoint",
            std::make_shared<sp::runtime::Client>(descriptor)) {}
    std::string_view family() const noexcept override { return "anthropic.messages"; }
};

class DurableNativeTool final : public neograph::Tool {
public:
    explicit DurableNativeTool(std::shared_ptr<std::atomic<unsigned>> effects)
        : effects_(std::move(effects)) {}
    neograph::ChatTool get_definition() const override {
        return {"read", "read fixture", json{{"type", "object"}}};
    }
    std::string execute(const json& args) override {
        if (args.at("path") != "a") throw std::logic_error("unexpected native tool argument");
        ++*effects_;
        return "read-result";
    }
    std::string get_name() const override { return "read"; }
private:
    std::shared_ptr<std::atomic<unsigned>> effects_;
};

json durable_native_graph() {
    return {
        {"name", "durable-native-checkpoint"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"llm", {{"type", "llm_call"}}}, {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"type", "conditional"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}})},
        {"interrupt_after", json::array({"tools"})}};
}

EngineConfig durable_native_config(const std::shared_ptr<neograph::Provider>& provider,
                                   const std::shared_ptr<std::atomic<unsigned>>& effects,
                                   const std::shared_ptr<SqliteCheckpointStore>& checkpoints,
                                   std::shared_ptr<sp::NativeArchive> archive = {}) {
    EngineConfig config;
    config.node_context.provider = provider;
    config.node_context.model = "fixture-model";
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<DurableNativeTool>(effects));
    config.node_context.tools = neograph::ToolSet(std::move(tools));
    config.node_context.provider_controls.account_scope = "fixture-account";
    config.node_context.provider_controls.max_output_tokens = 64;
    config.checkpoint_store = checkpoints;
    config.native_history_archive = std::move(archive);
    return config;
}

sp::descriptor::ValidatedDescriptor managed_responses_descriptor() {
    auto source = json::parse(sp::config_defaults::descriptor_policy_json);
    json defaults;
    for (const auto& family : source.at("families"))
        if (family.at("family") == "openai.responses") defaults = family.at("defaults");
    const json model{{"family", "openai.responses"}, {"model", "fixture-model"},
        {"defaults", std::move(defaults)}, {"output_limit", nullptr}, {"input_limit", 1}};
    bool replaced = false;
    for (auto row : source.at("models")) {
        if (row.at("family") == "openai.responses" && row.at("model") == "fixture-model") {
            row = model;
            replaced = true;
            break;
        }
    }
    if (!replaced) source.at("models").push_back(model);
    auto policy = sp::descriptor::load_policy(source.dump(), sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&policy))
        throw std::logic_error(error->pointer + ": " + error->message);
    return neograph::test::descriptor("openai.responses", "https://fixture.invalid",
        std::get<sp::descriptor::PolicySnapshot>(std::move(policy)));
}

sp::runtime::Result managed_missing_output_outcome(const sp::descriptor::ValidatedDescriptor& descriptor) {
    sp::responses::Request request;
    request.model = "fixture-model";
    request.account_scope = "fixture-account";
    request.max_output_tokens = 64;
    request.messages = {neograph::test::message("message", sp::Role::User)};
    request.tools = {{"read", "read fixture", neograph::test::document(R"({"type":"object"})"), {}}};
    auto encoded = sp::responses::encode(descriptor, request, false);
    if (const auto* error = std::get_if<sp::Error>(&encoded))
        throw std::logic_error(error->safe_message);
    sp::Accumulator accumulator;
    sp::responses::Codec codec(descriptor, sp::responses::Mode::Buffered, accumulator,
        std::get<sp::responses::EncodedRequest>(std::move(encoded)).context);
    codec.buffered(R"({"id":"resp-native","object":"response","created_at":1,"model":"fixture-model",
        "status":"completed","output":[
            {"id":"rs-1","type":"reasoning","status":"completed",
             "summary":[{"type":"summary_text","text":"inspect"}],"encrypted_content":"fixture-encrypted"},
            {"id":"fc-1","type":"function_call","status":"completed",
             "call_id":"call-native","name":"read","arguments":"{\"path\":\"a\"}"},
            {"id":"msg-1","type":"message","status":"completed","role":"assistant",
             "content":[{"type":"output_text","text":"pending","annotations":[]}]}],
        "usage":{"input_tokens":2},"incomplete_details":null,"error":null})", {});
    codec.finish();
    auto outcome = accumulator.take_outcome();
    if (!outcome || !std::holds_alternative<sp::Completion>(*outcome))
        throw std::logic_error("missing-output Responses fixture was not admitted");
    return std::make_shared<const sp::Outcome>(std::move(*outcome));
}

struct ManagedNativeProbe {
    unsigned calls = 0;
    std::vector<sp::responses::Request> requests;
};

class ManagedNativeProvider final : public neograph::test::LocalProvider {
public:
    ManagedNativeProvider(std::shared_ptr<ManagedNativeProbe> probe,
                          sp::descriptor::ValidatedDescriptor descriptor,
                          sp::runtime::Result result)
        : LocalProvider([probe, result = std::move(result)](
            neograph::ProviderRequest request, const neograph::PreparedProviderRequest&,
            const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                if (++probe->calls > 1) throw std::logic_error("managed restart dispatched twice");
                probe->requests.push_back(std::get<sp::responses::Request>(std::move(request.payload)));
                co_return result;
            }, "managed-native-checkpoint", std::make_shared<sp::runtime::Client>(descriptor)) {}
    std::string_view family() const noexcept override { return "openai.responses"; }
};

} // namespace

TEST(SqliteCheckpointNativeHistory, RestartRestoresAuthenticToolHistoryAndOwnedReportsWithoutRenewingBudget) {
    NativeCheckpointFiles files;
    const auto descriptor = neograph::test::descriptor("anthropic.messages");
    const std::string owner = "native-checkpoint-owner";
    auto activation = sp::NativeArchive::provision(files.archive(), files.key(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(activation));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(activation));
    auto effects = std::make_shared<std::atomic<unsigned>>(0);
    auto usage = std::make_shared<neograph::UsageAccumulator>();
    ASSERT_TRUE(usage->try_reserve(17, 100));
    RunMetadata metadata;
    metadata.owner_scope = owner;
    metadata.run_id = "native-checkpoint-run";
    RunConfig run;
    run.thread_id = "native-checkpoint-thread";
    run.input = {{"messages", json::array({{{"role", "user"}, {"content", "message"}}})}};
    run.usage = usage;
    std::string checkpoint_id;
    json original_report;
    json original_message;
    {
        auto probe = std::make_shared<DurableNativeProbe>();
        const auto outcome = durable_native_outcome(descriptor);
        original_report = neograph::provider_codec::observe_outcome(*outcome);
        original_message = neograph::provider_codec::encode_message(
            std::get<sp::Completion>(*outcome).messages.at(0));
        auto provider = std::make_shared<DurableNativeProvider>(probe, descriptor, outcome);
        auto checkpoints = std::make_shared<SqliteCheckpointStore>(files.database());
        auto engine = GraphEngine::build(durable_native_graph(),
            durable_native_config(provider, effects, checkpoints, archive));
        const auto interrupted = engine->run(run, metadata);
        ASSERT_TRUE(interrupted.interrupted);
        checkpoint_id = interrupted.checkpoint_id;
        EXPECT_EQ(probe->provider_calls, 1u);
        EXPECT_EQ(effects->load(), 1u);
        EXPECT_EQ(usage->total_tokens_wide(), 22u);
        const auto checkpoint = checkpoints->load_by_id(checkpoint_id);
        ASSERT_TRUE(checkpoint);
        EXPECT_FALSE(checkpoint->native_history);
        ASSERT_EQ(interrupted.native_messages.size(), 3u);
        EXPECT_EQ(neograph::provider_codec::encode_message(interrupted.native_messages[1]),
                  original_message);
    }
    // All provider, graph, database and archive owners from the first run die.
    // The host keeps its nonrenewable usage ledger, not a replay-created budget.
    archive.reset();
    run.input = json();
    auto wrong_owner = sp::NativeArchive::open(files.archive(), files.key(), "other-owner", descriptor);
    ASSERT_TRUE(std::holds_alternative<sp::Error>(wrong_owner));
    EXPECT_EQ(std::get<sp::Error>(wrong_owner).kind, sp::ErrorKind::Permission);
    auto reopened = sp::NativeArchive::open(files.archive(), files.key(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(reopened));
    archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(reopened));
    EXPECT_EQ(archive->owner_scope(), owner);
    auto probe = std::make_shared<DurableNativeProbe>();
    const auto final_outcome = neograph::test::success("finished", neograph::test::usage(2, 3, 5));
    auto provider = std::make_shared<DurableNativeProvider>(probe, descriptor, final_outcome);
    auto checkpoints = std::make_shared<SqliteCheckpointStore>(files.database());
    {
        auto without_archive = GraphEngine::build(durable_native_graph(),
            durable_native_config(provider, effects, checkpoints));
        EXPECT_THROW(without_archive->resume_from(run, checkpoint_id, {}, {}, metadata), std::exception);
        EXPECT_EQ(probe->provider_calls, 0u);
        EXPECT_EQ(effects->load(), 1u);
        EXPECT_EQ(usage->total_tokens_wide(), 22u);
    }
    auto engine = GraphEngine::build(durable_native_graph(),
        durable_native_config(provider, effects, checkpoints, archive));
    const auto completed = engine->resume_from(run, checkpoint_id, {}, {}, metadata);
    EXPECT_FALSE(completed.interrupted);
    EXPECT_EQ(probe->provider_calls, 1u);
    EXPECT_EQ(effects->load(), 1u);
    ASSERT_EQ(probe->requests.size(), 1u);
    const auto& replayed = probe->requests.front().messages;
    ASSERT_EQ(replayed.size(), 3u);
    ASSERT_TRUE(replayed[1].native);
    EXPECT_TRUE(replayed[1].native->complete());
    EXPECT_EQ(neograph::provider_codec::encode_message(replayed[1]), original_message);
    ASSERT_EQ(replayed[1].parts.size(), 3u);
    EXPECT_EQ(std::get<sp::Thinking>(replayed[1].parts[0]).signature,
              std::optional<std::string>("fixture-signature"));
    const auto& call = std::get<sp::ToolCall>(replayed[1].parts[1]);
    EXPECT_EQ(call.id, "call-native");
    EXPECT_EQ(call.input->root().get("path").as_string(), "a");
    EXPECT_EQ(std::get<sp::Text>(replayed[1].parts[2]).value, "pending");
    ASSERT_EQ(replayed[2].parts.size(), 1u);
    const auto& result = std::get<sp::ToolResult>(replayed[2].parts[0]);
    EXPECT_EQ(result.tool_use_id, "call-native");
    EXPECT_EQ(result.content, "read-result");
    EXPECT_FALSE(result.is_error);
    ASSERT_EQ(completed.provider_outcomes.size(), 2u);
    EXPECT_EQ(neograph::provider_codec::observe_outcome(*completed.provider_outcomes[0]), original_report);
    EXPECT_EQ(completed.provider_outcomes[1], final_outcome);
    ASSERT_EQ(completed.native_messages.size(), 4u);
    EXPECT_EQ(neograph::provider_codec::encode_message(completed.native_messages[1]), original_message);
    EXPECT_EQ(std::get<sp::Text>(completed.native_messages.back().parts.at(0)).value, "finished");
    EXPECT_EQ(usage->total_tokens_wide(), 27u);
    ASSERT_TRUE(completed.usage.input_total);
    ASSERT_TRUE(completed.usage.output_total);
    EXPECT_EQ(completed.usage.input_total->value, 4u);
    EXPECT_EQ(completed.usage.output_total->value, 6u);
    engine.reset();
    provider.reset();
    checkpoints.reset();
    archive.reset();
    const auto& retained = std::get<sp::Completion>(*completed.provider_outcomes.front());
    ASSERT_TRUE(retained.messages.front().native);
    EXPECT_TRUE(retained.messages.front().native->complete());
    EXPECT_EQ(neograph::provider_codec::observe_outcome(*completed.provider_outcomes.front()), original_report);
    EXPECT_EQ(usage->total_tokens_wide(), 27u);
    EXPECT_EQ(effects->load(), 1u);
}

TEST(SqliteCheckpointNativeHistory, FreshManagedBankAuthenticatesHeldClaimsScopeAndCapBeforeResumeOrFork) {
    NativeCheckpointFiles files;
    const auto descriptor = managed_responses_descriptor();
    const std::string owner = "managed-checkpoint-owner";
    auto provisioned = sp::NativeArchive::provision(files.archive(), files.key(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(provisioned));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(provisioned));
    auto effects = std::make_shared<std::atomic<unsigned>>(0);
    RunMetadata metadata;
    metadata.owner_scope = owner;
    metadata.run_id = "managed-checkpoint-run";
    RunConfig run;
    run.thread_id = "managed-checkpoint-thread";
    run.model_token_budget = 130; // Admitted input limit 1 + explicit output 64 => each real claim is 65.
    run.input = {{"messages", json::array({{{"role", "user"}, {"content", "message"}}})}};
    std::string checkpoint_id;
    json original_report;
    json original_authority;
    {
        auto probe = std::make_shared<ManagedNativeProbe>();
        const auto outcome = managed_missing_output_outcome(descriptor);
        const auto& report = std::get<sp::Completion>(*outcome).usage;
        ASSERT_TRUE(report.input_total);
        EXPECT_EQ(report.input_total->value, 2u);
        EXPECT_FALSE(report.output_total);
        EXPECT_FALSE(report.total);
        original_report = neograph::provider_codec::observe_outcome(*outcome);
        auto provider = std::make_shared<ManagedNativeProvider>(probe, descriptor, outcome);
        auto checkpoints = std::make_shared<SqliteCheckpointStore>(files.database());
        auto engine = GraphEngine::build(durable_native_graph(),
            durable_native_config(provider, effects, checkpoints, archive));
        const auto interrupted = engine->run(run, metadata);
        ASSERT_TRUE(interrupted.interrupted);
        checkpoint_id = interrupted.checkpoint_id;
        EXPECT_EQ(probe->calls, 1u);
        EXPECT_EQ(effects->load(), 1u);
        const auto saved = checkpoints->load_by_id(checkpoint_id);
        ASSERT_TRUE(saved);
        EXPECT_FALSE(saved->native_history);
        original_authority = saved->channel_values.at("provider_managed_budget").at("data");
        EXPECT_EQ(original_authority.at("charged").get<std::uint64_t>(), 0u);
        EXPECT_EQ(original_authority.at("reserved").get<std::uint64_t>(), 65u);
        EXPECT_EQ(original_authority.at("ceiling").get<std::uint64_t>(), 130u);
        EXPECT_EQ(original_authority.at("owner_scope"), owner);
        EXPECT_EQ(original_authority.at("thread_id"), run.thread_id);
        EXPECT_EQ(original_authority.at("reports"), neograph::provider_codec::encode_usage(report));
        EXPECT_TRUE(original_authority.at("has_report").get<bool>());
    }
    // No original managed bank, provider outcome, engine or connection survives.
    archive.reset();
    auto opened = sp::NativeArchive::open(files.archive(), files.key(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(opened));
    archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(opened));
    auto probe = std::make_shared<ManagedNativeProbe>();
    const auto final_outcome = neograph::test::success("finished", neograph::test::usage(2, 3, 5));
    auto provider = std::make_shared<ManagedNativeProvider>(probe, descriptor, final_outcome);
    auto checkpoints = std::make_shared<SqliteCheckpointStore>(files.database());
    auto engine = GraphEngine::build(durable_native_graph(),
        durable_native_config(provider, effects, checkpoints, archive));
    run.input = json();
    run.model_token_budget = 0; // Must inherit the authenticated original finite cap.
    const auto authentic = checkpoints->load_by_id(checkpoint_id);
    ASSERT_TRUE(authentic);
    EXPECT_THROW(engine->fork(run.thread_id, "managed-illegal-fork", checkpoint_id), std::invalid_argument);
    EXPECT_FALSE(checkpoints->load_latest("managed-illegal-fork"));
    EXPECT_EQ(checkpoints->load_by_id(checkpoint_id)->channel_values, authentic->channel_values);
    EXPECT_EQ(probe->calls, 0u);
    EXPECT_EQ(effects->load(), 1u);

    const std::vector<std::pair<std::string, json>> changes{
        {"charged", 1u}, {"reserved", 0u}, {"ceiling", 131u},
        {"owner_scope", "other-owner"}, {"thread_id", "other-thread"},
        {"graph_identity", "other-graph"},
        {"provider_effects", json::array({"forged-dedup-effect"})}};
    for (const auto& [field, value] : changes) {
        auto tampered = *authentic;
        tampered.id = Checkpoint::generate_id();
        tampered.channel_values["provider_managed_budget"]["data"][field] = value;
        checkpoints->save(tampered);
        EXPECT_THROW(engine->resume_from(run, tampered.id, {}, {}, metadata), std::exception)
            << "tampered authority field " << field;
        EXPECT_EQ(probe->calls, 0u);
        EXPECT_EQ(effects->load(), 1u);
        EXPECT_EQ(checkpoints->load_by_id(checkpoint_id)->channel_values, authentic->channel_values);
    }
    auto downgraded = *authentic;
    downgraded.id = Checkpoint::generate_id();
    downgraded.channel_values["provider_managed_budget"]["data"]["reserved"] = 0u;
    downgraded.channel_values["provider_managed_budget"]["custody"]["native_archive_reference"] = nullptr;
    checkpoints->save(downgraded);
    EXPECT_THROW(engine->resume_from(run, downgraded.id, {}, {}, metadata), std::exception);
    EXPECT_EQ(probe->calls, 0u);
    EXPECT_EQ(effects->load(), 1u);
    for (const bool remove_bank : {false, true}) {
        auto missing_custody = *authentic;
        missing_custody.id = Checkpoint::generate_id();
        if (remove_bank) {
            auto channels = json::object();
            for (const auto& [name, value] : missing_custody.channel_values.items())
                if (name != "provider_managed_budget") channels[name] = value;
            missing_custody.channel_values = std::move(channels);
        } else {
            missing_custody.channel_values["provider_history_durable"] = false;
            missing_custody.channel_values["provider_managed_budget"]["data"]["reserved"] = 0u;
        }
        checkpoints->save(missing_custody);
        EXPECT_THROW(engine->resume_from(run, missing_custody.id, {}, {}, metadata), std::exception);
        EXPECT_EQ(probe->calls, 0u);
        EXPECT_EQ(effects->load(), 1u);
        EXPECT_EQ(checkpoints->load_by_id(checkpoint_id)->channel_values, authentic->channel_values);
    }
    auto raised = run;
    raised.model_token_budget = 131;
    EXPECT_THROW(engine->resume_from(raised, checkpoint_id, {}, {}, metadata), std::invalid_argument);
    auto wrong_owner = metadata;
    wrong_owner.owner_scope = "other-owner";
    EXPECT_THROW(engine->resume_from(run, checkpoint_id, {}, {}, wrong_owner), std::invalid_argument);
    auto wrong_thread = run;
    wrong_thread.thread_id = "other-thread";
    EXPECT_THROW(engine->resume_from(wrong_thread, checkpoint_id, {}, {}, metadata), std::exception);
    auto changed_definition = durable_native_graph();
    changed_definition["name"] = "another-authenticated-graph";
    auto changed_graph = GraphEngine::build(changed_definition,
        durable_native_config(provider, effects, checkpoints, archive));
    EXPECT_THROW(changed_graph->resume_from(run, checkpoint_id, {}, {}, metadata), std::invalid_argument);
    EXPECT_EQ(probe->calls, 0u);
    EXPECT_EQ(effects->load(), 1u);

    // Lowering a ceiling is permitted, but the unknown earlier hold is not
    // released to fit it. A second claim of 65 cannot fit alongside held 65.
    auto lowered = run;
    lowered.model_token_budget = 129;
    try {
        (void)engine->resume_from(lowered, checkpoint_id, {}, {}, metadata);
        FAIL() << "the original held claim must remain spent authority";
    } catch (const NodeExecutionError& error) {
        try {
            std::rethrow_exception(error.cause());
        } catch (const neograph::ProviderFailure& failure) {
            EXPECT_EQ(std::get<sp::Failure>(*failure.outcome()).error.kind, sp::ErrorKind::QuotaExhausted);
        }
    }
    EXPECT_EQ(probe->calls, 0u);
    EXPECT_EQ(effects->load(), 1u);
    const auto completed = engine->resume_from(run, checkpoint_id, {}, {}, metadata);
    EXPECT_FALSE(completed.interrupted);
    EXPECT_EQ(probe->calls, 1u);
    EXPECT_EQ(effects->load(), 1u);
    ASSERT_EQ(completed.provider_outcomes.size(), 2u);
    EXPECT_EQ(neograph::provider_codec::observe_outcome(*completed.provider_outcomes.front()), original_report);
    EXPECT_EQ(completed.provider_outcomes.back(), final_outcome);
    ASSERT_TRUE(completed.usage.input_total);
    EXPECT_EQ(completed.usage.input_total->value, 4u);
    EXPECT_FALSE(completed.usage.output_total);
    EXPECT_FALSE(completed.usage.total);
    const auto saved_final = checkpoints->load_by_id(completed.checkpoint_id);
    ASSERT_TRUE(saved_final);
    const auto& restored_authority = saved_final->channel_values.at("provider_managed_budget").at("data");
    EXPECT_EQ(restored_authority.at("charged").get<std::uint64_t>(), 5u);
    EXPECT_EQ(restored_authority.at("reserved").get<std::uint64_t>(), 65u);
    EXPECT_EQ(restored_authority.at("ceiling").get<std::uint64_t>(), 130u);
    EXPECT_EQ(restored_authority.at("owner_scope"), owner);
    EXPECT_EQ(restored_authority.at("thread_id"), run.thread_id);
    EXPECT_EQ(restored_authority.at("graph_identity"), original_authority.at("graph_identity"));
    const auto accounted_effects = restored_authority.at("provider_effects");
    for (const auto effect : original_authority.at("provider_effects")) {
        bool remembered = false;
        for (const auto accounted : accounted_effects) {
            if (accounted == effect) {
                remembered = true;
                break;
            }
        }
        EXPECT_TRUE(remembered) << effect.get<std::string_view>();
    }
    EXPECT_EQ(restored_authority.at("reports"), neograph::provider_codec::encode_usage(completed.usage));
}
#endif
