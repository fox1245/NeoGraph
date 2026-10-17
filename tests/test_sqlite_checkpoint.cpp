// Unit tests for SqliteCheckpointStore.
//
// Portable backend contract tests use isolated in-memory databases. The native
// history restart regression uses a temporary file and independently protected
// archive/key custody so a new engine must restore genuine provider seals.

#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/graph/engine.h>
#include <neograph/provider_outcome_codec.h>
#include "fixtures/coarse_clock_store.h"
#include "fixtures/typed_provider.h"
#include "fixtures/unique_temp_dir.h"
#include <codecs/messages.h>
#include <codecs/responses.h>
#include <core/native.h>
#include <atomic>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
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

TEST_F(SqliteCheckpointTest, LoadsAValueNestedDeeperThanTheParseLimit) {
    // A node can write a value of any depth, and json::parse's default limit
    // is for text from elsewhere: the store reads back what it saved.
    json value = json::array();
    for (std::size_t level = 1; level < json::max_parse_depth + 8; ++level)
        value = json::array({value});
    store->save(make_state_cp("t", 0, {{"deep", {value, 1}}}));

    auto loaded = store->load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["deep"]["value"], value);
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
        // Resolve the OS temp parent before creating paths subject to no-symlink custody.
        path_ = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
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

// #386, #396: a thread's latest checkpoint is the one saved last. Two runs on
// one thread inside one millisecond (a Program plan's Core calls on its root
// thread, where fsync returns at once) tie on the timestamp, and the earlier
// run's last checkpoint has the higher step; two gates of one step save the
// same step. SQLite ordered a thread's checkpoints by (timestamp_ms, step), so
// either one could be the "latest", and a resume of it replayed the earlier
// run or answered the wrong gate. Every save below carries one timestamp.
namespace {

using test_support::CoarseClockStore;
using CoarseSqliteStore = CoarseClockStore<SqliteCheckpointStore>;

std::atomic<int> coarse_gate_resumed{0};

class CoarseGateNode final : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput in) override {
        NodeResult out;
        if (in.ctx.resume_value) {
            ++coarse_gate_resumed;
            out.writes.push_back(ChannelWrite{"value", *in.ctx.resume_value});
            co_return out;
        }
        if (in.state.get("value") == json("ask")) throw NodeInterrupt("approval");
        out.writes.push_back(ChannelWrite{"value", json("passed")});
        co_return out;
    }
    std::string get_name() const override { return "gate"; }
};

class CoarseTailNode final : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput) override {
        NodeResult out;
        out.writes.push_back(ChannelWrite{"tail", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "tail"; }
};

}  // namespace

namespace {

json coarse_clock_graph() {
    NodeFactory::instance().register_type(
        "coarse_gate", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<CoarseGateNode>();
        });
    NodeFactory::instance().register_type(
        "coarse_tail", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<CoarseTailNode>();
        });
    return json{
        {"name", "coarse_clock"},
        {"channels", {{"value", {{"reducer", "overwrite"}}}, {"tail", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"gate", {{"type", "coarse_gate"}}}, {"tail", {{"type", "coarse_tail"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "gate"}},
                               json{{"from", "gate"}, {"to", "tail"}},
                               json{{"from", "tail"}, {"to", "__end__"}}})}};
}

Checkpoint coarse_cp(const std::string& thread_id, int step, int value) {
    auto cp      = make_state_cp(thread_id, step, {{"x", {value, static_cast<uint64_t>(value)}}});
    cp.timestamp = CoarseSqliteStore::kTimestamp;
    return cp;
}

std::vector<std::string> ids_of(const std::vector<Checkpoint>& checkpoints) {
    std::vector<std::string> ids;
    for (const auto& checkpoint : checkpoints) ids.push_back(checkpoint.id);
    return ids;
}

}  // namespace

TEST(SqliteCheckpointCoarseClock, ALaterRunAtStepZeroIsTheLatestAndResumes) {
    coarse_gate_resumed.store(0);
    auto store  = std::make_shared<CoarseSqliteStore>(":memory:");
    auto engine = GraphEngine::compile(coarse_clock_graph(), NodeContext{}, store);

    RunConfig first;
    first.thread_id = "coarse-clock";
    first.input     = json{{"value", "go"}};
    const auto finished = engine->run(first);
    ASSERT_FALSE(finished.interrupted);
    const auto finished_cp = store->load_by_id(finished.checkpoint_id);
    ASSERT_TRUE(finished_cp);
    ASSERT_GT(finished_cp->step, 0);

    // The second run restarts at step 0 in the same millisecond, below the
    // first run's last step. Its pause is still the thread's latest.
    RunConfig second = first;
    second.input     = json{{"value", "ask"}};
    const auto paused = engine->run(second);
    ASSERT_TRUE(paused.interrupted);
    const auto latest = store->load_latest("coarse-clock");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, paused.checkpoint_id) << "the checkpoint saved last is the latest";
    EXPECT_EQ(latest->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(latest->next_nodes, std::vector<std::string>{"gate"});
    EXPECT_EQ(latest->timestamp, finished_cp->timestamp);
    EXPECT_LT(latest->step, finished_cp->step);
    const auto history = store->list("coarse-clock");
    ASSERT_FALSE(history.empty());
    EXPECT_EQ(history.front().id, paused.checkpoint_id);

    // resume() takes the thread's latest checkpoint: the paused node runs.
    const auto resumed = engine->resume("coarse-clock", json("answered"));
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(coarse_gate_resumed.load(), 1) << "the paused node runs with the answer";
    EXPECT_EQ(resumed.output["channels"]["value"]["value"], json("answered"));
    EXPECT_EQ(resumed.output["channels"]["tail"]["value"], json(true));
}

TEST(SqliteCheckpointCoarseClock, NodeInterruptReportsTheCheckpointItSaved) {
    coarse_gate_resumed.store(0);
    auto store  = std::make_shared<CoarseSqliteStore>(":memory:");
    auto engine = GraphEngine::compile(coarse_clock_graph(), NodeContext{}, store);

    RunConfig first;
    first.thread_id = "coarse-clock";
    first.input     = json{{"value", "go"}};
    ASSERT_FALSE(engine->run(first).interrupted);

    RunConfig second = first;
    second.input     = json{{"value", "ask"}};
    const auto paused = engine->run(second);
    ASSERT_TRUE(paused.interrupted);
    const auto saved = store->load_by_id(paused.checkpoint_id);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(saved->next_nodes, std::vector<std::string>{"gate"});

    const auto resumed = engine->resume_from(second, paused.checkpoint_id, json("answered"));
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(coarse_gate_resumed.load(), 1) << "the paused node runs with the answer";
    EXPECT_EQ(resumed.output["channels"]["value"]["value"], json("answered"));
    EXPECT_EQ(resumed.output["channels"]["tail"]["value"], json(true));
}

TEST(SqliteCheckpointCoarseClock, AStepZeroSaveAfterAHigherStepIsTheLatest) {
    CoarseSqliteStore store(":memory:");
    const auto high = coarse_cp("t", 5, 1);
    const auto low  = coarse_cp("t", 0, 2);
    store.save(high);
    store.save(low);
    ASSERT_TRUE(store.load_latest("t"));
    EXPECT_EQ(store.load_latest("t")->id, low.id);
    EXPECT_EQ(ids_of(store.list("t")), (std::vector<std::string>{low.id, high.id}));
}

TEST(SqliteCheckpointCoarseClock, EqualStepSavesKeepTheirSaveOrder) {
    // Two interrupt_before gates of one step each save a Before checkpoint of
    // that step (#396), here in one millisecond.
    CoarseSqliteStore store(":memory:");
    std::vector<std::string> saved;
    for (int gate = 0; gate < 4; ++gate) {
        auto cp            = coarse_cp("t", 3, gate + 1);
        cp.interrupt_phase = CheckpointPhase::Before;
        store.save(cp);
        saved.insert(saved.begin(), cp.id);
        ASSERT_TRUE(store.load_latest("t"));
        EXPECT_EQ(store.load_latest("t")->id, cp.id) << "after gate " << gate;
    }
    EXPECT_EQ(ids_of(store.list("t")), saved);
    EXPECT_EQ(ids_of(store.list("t", 2)), (std::vector<std::string>{saved[0], saved[1]}));
}

TEST(SqliteCheckpointCoarseClock, AResavedIdBecomesTheLatestOnce) {
    // As in the in-memory store: a save of an ID the thread holds replaces it
    // and makes it the latest; the history lists it once, in its new place.
    CoarseSqliteStore store(":memory:");
    auto       first  = coarse_cp("t", 0, 1);
    const auto second = coarse_cp("t", 1, 2);
    store.save(first);
    store.save(second);
    first.next_nodes = {"edited"};
    store.save(first);
    const auto latest = store.load_latest("t");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, first.id);
    EXPECT_EQ(latest->next_nodes, std::vector<std::string>{"edited"});
    EXPECT_EQ(ids_of(store.list("t")), (std::vector<std::string>{first.id, second.id}));

    InMemoryCheckpointStore reference;
    auto reference_first = first;
    reference_first.next_nodes = {"__end__"};
    reference.save(reference_first);
    reference.save(second);
    reference.save(first);
    EXPECT_EQ(ids_of(reference.list("t")), ids_of(store.list("t")));
    ASSERT_TRUE(reference.load_latest("t"));
    EXPECT_EQ(reference.load_latest("t")->next_nodes, latest->next_nodes);
}

TEST(SqliteCheckpointCoarseClock, ThreadsKeepSeparateSaveOrders) {
    CoarseSqliteStore store(":memory:");
    const auto a0 = coarse_cp("a", 4, 1);
    const auto b0 = coarse_cp("b", 0, 2);
    const auto a1 = coarse_cp("a", 0, 3);
    store.save(a0);
    store.save(b0);
    store.save(a1);
    EXPECT_EQ(ids_of(store.list("a")), (std::vector<std::string>{a1.id, a0.id}));
    EXPECT_EQ(ids_of(store.list("b")), (std::vector<std::string>{b0.id}));
    store.delete_thread("a");
    EXPECT_FALSE(store.load_latest("a"));
    const auto a2 = coarse_cp("a", 7, 4);
    const auto a3 = coarse_cp("a", 0, 5);
    store.save(a2);
    store.save(a3);
    EXPECT_EQ(ids_of(store.list("a")), (std::vector<std::string>{a3.id, a2.id}));
}

namespace {

// neograph_checkpoints as the release before save_seq created it, with its
// index and rows that release wrote: in thread "t" its order was old-3, old-2,
// old-1. A file it left is in WAL mode; a file from another tool may not be.
constexpr const char* kPreviousSqliteSchema = R"SQL(
    CREATE TABLE neograph_checkpoints (
        thread_id TEXT NOT NULL, checkpoint_id TEXT NOT NULL,
        parent_id TEXT NOT NULL DEFAULT '', current_node TEXT NOT NULL DEFAULT '',
        next_nodes TEXT NOT NULL DEFAULT '[]',
        interrupt_phase TEXT NOT NULL DEFAULT 'completed',
        barrier_state TEXT NOT NULL DEFAULT '{}', channel_versions TEXT NOT NULL DEFAULT '{}',
        global_version INTEGER NOT NULL DEFAULT 0, metadata TEXT NOT NULL DEFAULT '{}',
        step INTEGER NOT NULL DEFAULT 0, timestamp_ms INTEGER NOT NULL DEFAULT 0,
        schema_version INTEGER NOT NULL DEFAULT 2, checkpoint_shape TEXT,
        PRIMARY KEY (thread_id, checkpoint_id));
    CREATE INDEX neograph_checkpoints_recent
        ON neograph_checkpoints (thread_id, timestamp_ms DESC, step DESC);
    INSERT INTO neograph_checkpoints (thread_id, checkpoint_id, step, timestamp_ms) VALUES
        ('t', 'old-1', 0, 1000), ('t', 'old-3', 0, 2000), ('t', 'old-2', 1, 1000),
        ('other', 'other-1', 9, 5);
)SQL";

using RawSqlite = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

// A separate connection, as another process or an older release has it.
RawSqlite open_raw(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open(path.string().c_str(), &raw);
    RawSqlite db(raw, &sqlite3_close);
    if (opened != SQLITE_OK) throw std::runtime_error("cannot open " + path.string());
    return db;
}

// "" on success, else SQLite's message.
std::string exec_raw(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    const int status = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "";
    sqlite3_free(error);
    return status == SQLITE_OK ? std::string() : message.empty() ? "error" : message;
}

std::string exec_on(const std::filesystem::path& path, const std::string& sql) {
    auto db = open_raw(path);
    return exec_raw(db.get(), sql);
}

std::string query_text(const std::filesystem::path& path, const std::string& sql) {
    auto db = open_raw(path);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return "<prepare failed>";
    std::string value = "<no row>";
    if (sqlite3_step(statement) == SQLITE_ROW)
        value = sqlite3_column_type(statement, 0) == SQLITE_NULL
                    ? "<null>"
                    : reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    return value;
}

}  // namespace

// A database written before save_seq existed: its rows keep their
// (timestamp_ms, step) order, behind every row saved since. The migration adds
// a nullable column, rewrites no row, and runs again without effect.
TEST(SqliteCheckpointSaveOrderMigration, LegacyRowsKeepTheirOrderBehindNewSaves) {
    const test_support::UniqueTempDir dir("neograph-save-order-");
    const auto path = dir.path() / "checkpoints.sqlite";
    const auto exec = [&](const std::string& sql) { return exec_on(path, sql); };
    const auto query_int = [&](const std::string& sql) { return std::stoll(query_text(path, sql)); };
    ASSERT_EQ(exec(kPreviousSqliteSchema), "");

    std::vector<std::string> legacy_order{"old-3", "old-2", "old-1"};
    {
        SqliteCheckpointStore store(path.string());
        EXPECT_EQ(ids_of(store.list("t")), legacy_order) << "legacy rows keep (timestamp_ms, step)";
        ASSERT_TRUE(store.load_latest("t"));
        EXPECT_EQ(store.load_latest("t")->id, "old-3");

        // A new save is the latest, though its timestamp and step are lower.
        auto fresh      = make_state_cp("t", 0, {{"x", {1, 1}}});
        fresh.timestamp = 1;
        store.save(fresh);
        legacy_order.insert(legacy_order.begin(), fresh.id);
        EXPECT_EQ(ids_of(store.list("t")), legacy_order);
        EXPECT_EQ(ids_of(store.list("other")), (std::vector<std::string>{"other-1"}));
    }
    EXPECT_EQ(query_int("SELECT COUNT(*) FROM neograph_checkpoints"), 5);
    EXPECT_EQ(query_int("SELECT COUNT(*) FROM neograph_checkpoints WHERE save_seq IS NULL"), 4)
        << "the migration rewrites no legacy row";
    EXPECT_EQ(query_int("SELECT COUNT(*) FROM sqlite_master WHERE name = 'neograph_checkpoints_recent'"), 0);
    EXPECT_EQ(query_int("SELECT COUNT(*) FROM sqlite_master WHERE name = 'neograph_checkpoints_save_order'"), 1);

    // A writer that does not set save_seq, as an older release does, is
    // numbered by the trigger and is the latest.
    ASSERT_EQ(exec("INSERT INTO neograph_checkpoints (thread_id, checkpoint_id, step, timestamp_ms) "
                   "VALUES ('t', 'older-writer', 0, 0)"), "");
    legacy_order.insert(legacy_order.begin(), "older-writer");
    {
        // Opening again migrates nothing and keeps the order.
        SqliteCheckpointStore store(path.string());
        EXPECT_EQ(ids_of(store.list("t")), legacy_order);
        EXPECT_EQ(query_int("SELECT COUNT(*) FROM pragma_table_info('neograph_checkpoints') "
                            "WHERE name = 'save_seq'"), 1);

        // Re-saving a legacy row numbers it: it moves to the newest place.
        auto old_two = store.load_by_id("old-2");
        ASSERT_TRUE(old_two);
        store.save(*old_two);
        legacy_order.erase(std::find(legacy_order.begin(), legacy_order.end(), "old-2"));
        legacy_order.insert(legacy_order.begin(), "old-2");
        EXPECT_EQ(ids_of(store.list("t")), legacy_order);
        ASSERT_TRUE(store.load_latest("t"));
        EXPECT_EQ(store.load_latest("t")->id, "old-2");
    }
    EXPECT_EQ(query_int("SELECT COUNT(*) FROM neograph_checkpoints WHERE save_seq IS NULL"), 3);
}

TEST(SqliteCheckpointSaveOrderMigration, TheSaveOrderNeedsNoSort) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("neograph-save-order-plan-" + Checkpoint::generate_id() + ".sqlite");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto& suffix : {"", "-wal", "-shm"})
                std::filesystem::remove(path.string() + suffix, ignored);
        }
    } cleanup{path};
    { SqliteCheckpointStore store(path.string()); }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(),
        "EXPLAIN QUERY PLAN SELECT checkpoint_id FROM neograph_checkpoints WHERE thread_id = ?1 "
        "ORDER BY save_seq DESC, timestamp_ms DESC, step DESC LIMIT 1", -1, &statement, nullptr),
        SQLITE_OK);
    std::string plan;
    while (sqlite3_step(statement) == SQLITE_ROW)
        plan += reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)) + std::string("\n");
    sqlite3_finalize(statement);
    EXPECT_NE(plan.find("neograph_checkpoints_save_order"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("TEMP B-TREE"), std::string::npos) << plan;
}

// ── Concurrent opens of one database ──────────────────────────────────
//
// Two stores, or two processes, may open one database at the same moment, as
// a restarted host does. A store that is not first to switch a new file to
// WAL got SQLITE_BUSY from PRAGMA journal_mode=WAL at once, whatever its busy
// timeout ("exec failed: database is locked", in about one round in eight of
// four stores on Linux); each open now waits for the others within the busy
// timeout, and the schema and its migrations commit once.

namespace {

// Opens `stores` stores on `path` together and saves one checkpoint from each
// into thread "shared"; returns each open's error, "" when it succeeded.
std::vector<std::string> open_together(const std::filesystem::path& path, int stores) {
    std::barrier                          start(stores);
    std::vector<std::future<std::string>> opened;
    for (int i = 0; i < stores; ++i) {
        opened.push_back(std::async(std::launch::async, [&, i] {
            start.arrive_and_wait();
            try {
                SqliteCheckpointStore store(path.string());
                store.save(make_state_cp("shared", i, {{"x", {i, static_cast<uint64_t>(i + 1)}}}));
                return std::string();
            } catch (const std::exception& error) {
                return std::string(error.what());
            }
        }));
    }
    std::vector<std::string> errors;
    for (auto& open : opened) errors.push_back(open.get());
    return errors;
}

}  // namespace

TEST(SqliteCheckpointConcurrentOpen, StoresOpeningOneNewFileTogetherAllOpen) {
    constexpr int kStores = 6;
    const test_support::UniqueTempDir dir("neograph-concurrent-open-");
    for (int round = 0; round < 40; ++round) {
        const auto path = dir.path() / ("round-" + std::to_string(round) + ".sqlite");
        EXPECT_EQ(open_together(path, kStores), std::vector<std::string>(kStores)) << "round " << round;
        SqliteCheckpointStore check(path.string());
        EXPECT_EQ(check.list("shared").size(), static_cast<size_t>(kStores)) << "round " << round;
        EXPECT_EQ(query_text(path, "PRAGMA journal_mode"), "wal");
    }
}

TEST(SqliteCheckpointConcurrentOpen, AnOpenWaitsForAReaderOfARollbackJournalFile) {
    // Another connection reads a file that is not yet in WAL mode: the switch
    // waits until it finishes, within the busy timeout.
    const test_support::UniqueTempDir dir("neograph-open-reader-");
    const auto path = dir.path() / "shared.sqlite";
    auto reader = open_raw(path);
    ASSERT_EQ(exec_raw(reader.get(), "CREATE TABLE other(x INTEGER); INSERT INTO other VALUES (1);"), "");
    ASSERT_EQ(exec_raw(reader.get(), "BEGIN; SELECT x FROM other;"), "");
    std::thread finish([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        (void)exec_raw(reader.get(), "COMMIT;");
    });
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    try {
        SqliteCheckpointStore store(path.string(), std::chrono::seconds(10));
        store.save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    const auto waited = std::chrono::steady_clock::now() - started;
    finish.join();
    EXPECT_EQ(error, "");
    EXPECT_GE(waited, std::chrono::milliseconds(200)) << "the open waited for the reader";
    EXPECT_EQ(query_text(path, "PRAGMA journal_mode"), "wal");
    EXPECT_EQ(query_text(path, "SELECT COUNT(*) FROM neograph_checkpoints"), "1");
}

TEST(SqliteCheckpointConcurrentOpen, AnOpenGivesUpWhenAReaderOutlastsTheBusyTimeout) {
    const test_support::UniqueTempDir dir("neograph-open-timeout-");
    const auto path = dir.path() / "shared.sqlite";
    auto reader = open_raw(path);
    ASSERT_EQ(exec_raw(reader.get(), "CREATE TABLE other(x INTEGER); INSERT INTO other VALUES (1);"), "");
    ASSERT_EQ(exec_raw(reader.get(), "BEGIN; SELECT x FROM other;"), "");
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    try {
        SqliteCheckpointStore store(path.string(), std::chrono::milliseconds(150));
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    const auto waited = std::chrono::steady_clock::now() - started;
    (void)exec_raw(reader.get(), "COMMIT;");
    EXPECT_NE(error.find("database is locked"), std::string::npos) << error;
    EXPECT_GE(waited, std::chrono::milliseconds(140)) << "the open used its busy timeout";
    EXPECT_LT(waited, std::chrono::seconds(5));
}

TEST(SqliteCheckpointSaveOrderMigration, StoresOpeningAnOldSchemaFileTogetherMigrateItOnce) {
    // The previous release's file, in WAL mode as it leaves one, or in
    // rollback mode as another tool may: stores opening it together each wait
    // for the migration, and it runs once.
    constexpr int kStores = 4;
    const test_support::UniqueTempDir dir("neograph-concurrent-migration-");
    for (int round = 0; round < 30; ++round) {
        const bool wal  = round % 2 == 0;
        const auto path = dir.path() / ("round-" + std::to_string(round) + ".sqlite");
        ASSERT_EQ(exec_on(path, std::string(wal ? "PRAGMA journal_mode=WAL;" : "") + kPreviousSqliteSchema),
                  "");
        std::barrier                                         start(kStores);
        std::vector<std::future<std::vector<std::string>>> opened;
        for (int i = 0; i < kStores; ++i) {
            opened.push_back(std::async(std::launch::async, [&, i] {
                start.arrive_and_wait();
                try {
                    SqliteCheckpointStore store(path.string());
                    auto ids = ids_of(store.list("t"));
                    auto cp  = make_state_cp("t", 0, {{"x", {i, static_cast<uint64_t>(i + 1)}}});
                    cp.timestamp = 1;
                    store.save(cp);
                    return ids;
                } catch (const std::exception& error) {
                    return std::vector<std::string>{std::string("error: ") + error.what()};
                }
            }));
        }
        for (auto& open : opened) {
            const auto seen = open.get();
            // Before every save, a store saw the legacy rows in their order;
            // after another store's save, that save comes first.
            ASSERT_GE(seen.size(), 3u) << "round " << round << ": " << (seen.empty() ? "" : seen[0]);
            EXPECT_EQ(std::vector<std::string>(seen.end() - 3, seen.end()),
                      (std::vector<std::string>{"old-3", "old-2", "old-1"}))
                << "round " << round;
        }
        EXPECT_EQ(query_text(path, "SELECT COUNT(*) FROM pragma_table_info('neograph_checkpoints') "
                                   "WHERE name = 'save_seq'"),
                  "1");
        EXPECT_EQ(query_text(path, "SELECT COUNT(*) FROM neograph_checkpoints WHERE save_seq IS NULL"), "4");
        EXPECT_EQ(query_text(path, "SELECT COUNT(DISTINCT save_seq) FROM neograph_checkpoints "
                                   "WHERE thread_id = 't' AND save_seq IS NOT NULL"),
                  std::to_string(kStores));
        EXPECT_EQ(query_text(path, "PRAGMA journal_mode"), "wal");
        SqliteCheckpointStore check(path.string());
        const auto order = ids_of(check.list("t"));
        ASSERT_EQ(order.size(), static_cast<size_t>(kStores + 3));
        EXPECT_EQ(std::vector<std::string>(order.end() - 3, order.end()),
                  (std::vector<std::string>{"old-3", "old-2", "old-1"}));
    }
}
