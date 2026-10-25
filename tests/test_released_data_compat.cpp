// Data written by a released version stays readable and resumable (#609).
//
// tests/fixtures/released/<release>/ holds stores written by the released
// neograph-engine wheel through its public Python API (the generator sits next
// to them) and a manifest of what the generator saw the wheel write. Every test
// copies what it opens to a temporary file, so a run never changes a committed
// file, and the fixture guard below fails if one did.
//
// What the tests hold true:
//  - The checkpoint tests read the stored checkpoints against the manifest
//    before they resume, and count the node bodies that run after, so a resume
//    that runs the whole graph again, or from another checkpoint, fails.
//  - An exact-checkpoint resume and a NodeInterrupt resume refuse a checkpoint
//    of another schema version, and a plain resume of a latest checkpoint of
//    another phase does not look at the number. The refusal tests change the
//    stored number in the temporary copy, so removing a refusal fails a test;
//    the unchanged fixture resuming guards CHECKPOINT_SCHEMA_VERSION, since a
//    change of the constant makes the released checkpoint a refused one.
//  - The runtime-store tests compare the stored bodies with the canonical bytes
//    the generator recorded, and migrate files of the older schema versions.
//  - The Program test resumes the paused run and counts the node bodies it runs.

#include "fixtures/typed_provider.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/sqlite_runtime_stores.h>

#ifdef NEOGRAPH_RELEASED_COMPAT_HAVE_PROGRAM
#include <neograph/program/program.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/program/store.h>

#include "fixtures/store_handle.h"
#include "provider_binding.h"
#endif

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef NEOGRAPH_RELEASED_FIXTURE_DIR
#error "NEOGRAPH_RELEASED_FIXTURE_DIR must name the directory of the released fixtures"
#endif

using namespace neograph;
using neograph::graph::test_support::UniqueTempDir;
namespace fs = std::filesystem;

namespace {

// The release whose directory these tests read. A later release adds its own
// directory and its own test over the same helpers.
constexpr const char* kRelease = "0.16.0";

fs::path release_dir() { return fs::path(NEOGRAPH_RELEASED_FIXTURE_DIR) / kRelease; }

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

json manifest() {
    static const json value = json::parse(read_bytes(release_dir() / "manifest.json"));
    return value;
}

std::vector<fs::path> files_under(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

// Fails a test that changed a committed fixture, whatever the test opened.
class ReleasedData : public ::testing::Test {
protected:
    void SetUp() override {
        for (const auto& path : files_under(release_dir())) before_[path.string()] = read_bytes(path);
    }
    void TearDown() override {
        for (const auto& [path, bytes] : before_)
            EXPECT_EQ(read_bytes(path), bytes) << "a test changed the committed fixture " << path;
        EXPECT_EQ(files_under(release_dir()).size(), before_.size())
            << "a test left a file beside the committed fixtures";
    }
    // A private copy of one committed file; the copy is what a test opens.
    std::string copy_of(const std::string& relative, const std::string& as = {}) {
        const auto name = as.empty() ? fs::path(relative).filename().string() : as;
        fs::copy_file(release_dir() / relative, scratch_.path() / name, fs::copy_options::overwrite_existing);
        return (scratch_.path() / name).string();
    }
    UniqueTempDir scratch_{"neograph-released-"};

private:
    std::map<std::string, std::string> before_;
};

std::string guard_message() {
    const auto& checkpoints = manifest().at("checkpoints");
    return "CHECKPOINT_SCHEMA_VERSION is " + std::to_string(graph::CHECKPOINT_SCHEMA_VERSION) +
           " and the checkpoint written by release " + manifest().at("release").get<std::string>() +
           " has schema version " + std::to_string(checkpoints.at("latest_schema_version").get<unsigned>()) +
           ". A resume refuses every other version, so a change of the constant makes the data of "
           "that release unresumable. Keep reading it (or migrate it) before changing the constant, "
           "then add the new release's fixtures.";
}

// -- SQLite helpers --------------------------------------------------------

using Rows = std::vector<std::vector<std::string>>;

// Every column of every row as bytes (a NULL as "<null>"), so a test compares
// what a table holds before and after something happened to the file.
Rows dump(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    Rows          rows;
    sqlite3_stmt* statement = nullptr;
    const auto    prepared  = sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr);
    EXPECT_EQ(prepared, SQLITE_OK) << sql << ": " << sqlite3_errmsg(db);
    if (prepared == SQLITE_OK)
        while (sqlite3_step(statement) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int column = 0; column < sqlite3_column_count(statement); ++column) {
                const auto* blob = sqlite3_column_blob(statement, column);
                row.push_back(blob ? std::string(static_cast<const char*>(blob),
                                                 static_cast<std::size_t>(sqlite3_column_bytes(statement, column)))
                                   : (sqlite3_column_type(statement, column) == SQLITE_NULL ? "<null>" : ""));
            }
            rows.push_back(std::move(row));
        }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return rows;
}

void execute(const std::string& path, const std::string& sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    char*             message = nullptr;
    const auto        status  = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    const std::string text    = message ? message : "";
    sqlite3_free(message);
    sqlite3_close(db);
    ASSERT_EQ(status, SQLITE_OK) << text;
}

// -- the graph of the checkpoint fixture ---------------------------------

// How many times each node body ran in this test.
struct NodeRuns {
    std::atomic<int> prep{0}, approve_asked{0}, approve_answered{0}, finish{0};
    void reset() { prep = approve_asked = approve_answered = finish = 0; }
};
NodeRuns& runs() {
    static NodeRuns value;
    return value;
}

class Prep final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput in) override {
        ++runs().prep;
        graph::NodeResult out;
        out.writes.push_back(graph::ChannelWrite{"note", json("prepared:" + in.state.get("seed").get<std::string>())});
        out.writes.push_back(graph::ChannelWrite{"trace", json::array({"prep"})});
        co_return out;
    }
    std::string get_name() const override { return "prep"; }
};

class Approve final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput in) override {
        const auto& answer = in.ctx.resume_value;
        if (!answer) {
            ++runs().approve_asked;
            throw graph::NodeInterrupt("needs approval", json{{"question", "ship it?"}});
        }
        ++runs().approve_answered;
        graph::NodeResult out;
        out.writes.push_back(graph::ChannelWrite{"answer", *answer});
        out.writes.push_back(graph::ChannelWrite{"trace", json::array({"approve"})});
        co_return out;
    }
    std::string get_name() const override { return "approve"; }
};

class Finish final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput) override {
        ++runs().finish;
        graph::NodeResult out;
        out.writes.push_back(graph::ChannelWrite{"result", json("done")});
        out.writes.push_back(graph::ChannelWrite{"trace", json::array({"finish"})});
        co_return out;
    }
    std::string get_name() const override { return "finish"; }
};

void register_checkpoint_nodes(const json& graph_definition) {
    const auto type_of = [&](const char* node) { return graph_definition.at("nodes").at(node).at("type").get<std::string>(); };
    auto& factory = graph::NodeFactory::instance();
    factory.register_type(type_of("prep"), [](const std::string&, const json&, const graph::NodeContext&) {
        return std::make_unique<Prep>();
    });
    factory.register_type(type_of("approve"), [](const std::string&, const json&, const graph::NodeContext&) {
        return std::make_unique<Approve>();
    });
    factory.register_type(type_of("finish"), [](const std::string&, const json&, const graph::NodeContext&) {
        return std::make_unique<Finish>();
    });
    runs().reset();
}

// A copy of the released checkpoint store and an engine over it.
struct Released {
    std::shared_ptr<graph::SqliteCheckpointStore> store;
    std::shared_ptr<graph::GraphEngine>           engine;
    std::string                                   thread;
    graph::RunConfig                              config;
};

graph::CheckpointPhase phase_named(const std::string& name) {
    static const std::map<std::string, graph::CheckpointPhase> phases{
        {"Before", graph::CheckpointPhase::Before},       {"After", graph::CheckpointPhase::After},
        {"Completed", graph::CheckpointPhase::Completed}, {"NodeInterrupt", graph::CheckpointPhase::NodeInterrupt},
        {"Updated", graph::CheckpointPhase::Updated}};
    return phases.at(name);
}

// The stored checkpoint is what the generator recorded the wheel writing.
void expect_matches(const graph::Checkpoint& stored, const json& recorded) {
    EXPECT_EQ(stored.id, recorded.at("id").get<std::string>());
    EXPECT_EQ(stored.parent_id, recorded.at("parent_id").get<std::string>());
    EXPECT_EQ(stored.interrupt_phase, phase_named(recorded.at("interrupt_phase").get<std::string>()));
    EXPECT_EQ(stored.step, recorded.at("step").get<std::int64_t>());
    EXPECT_EQ(stored.current_node, recorded.at("current_node").get<std::string>());
    EXPECT_EQ(stored.next_nodes, recorded.at("next_nodes").get<std::vector<std::string>>());
    EXPECT_EQ(stored.schema_version, recorded.at("schema_version").get<std::uint32_t>());
    EXPECT_EQ(stored.channel_values, recorded.at("channel_values"));
}

json recorded_checkpoint(std::size_t newest_first) {
    return manifest().at("checkpoints").at("checkpoints_newest_first").at(newest_first);
}
json recorded_paused() { return recorded_checkpoint(0); }
json recorded_earlier() { return recorded_checkpoint(1); }

json trace_of(const json& channel_values) { return channel_values.at("channels").at("trace").at("value"); }

// The released data, after a test changed it by `adjust` (SQL on the copy).
Released open_released(ReleasedData& test, const std::string& path, const std::string& adjust = {}) {
    const auto& expected = manifest().at("checkpoints");
    register_checkpoint_nodes(expected.at("graph"));
    if (!adjust.empty()) execute(path, adjust);
    Released released;
    released.store  = std::make_shared<graph::SqliteCheckpointStore>(path);
    released.engine = graph::GraphEngine::compile(expected.at("graph"), graph::NodeContext{}, released.store);
    released.thread = expected.at("thread_id").get<std::string>();
    released.config.thread_id = released.thread;
    (void)test;
    return released;
}

// Runs a resume of released data; a refusal is reported with the reason the
// released data is kept.
template <class Fn>
auto resumed(Fn&& resume) {
    try {
        return resume();
    } catch (const std::exception& error) {
        ADD_FAILURE() << "The resume of the released data failed: " << error.what() << "\n" << guard_message();
        throw;
    }
}

// What a resume that must be refused says, or an empty string after a failure.
template <class Fn>
std::string refusal(Fn&& resume) {
    try {
        resume();
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    ADD_FAILURE() << "the resume was not refused";
    return {};
}

json channel(const graph::RunResult& result, const std::string& name) {
    return result.output.at("channels").at(name).at("value");
}

}  // namespace

// ---------------------------------------------------------------------------
// Checkpoints
// ---------------------------------------------------------------------------

TEST_F(ReleasedData, CheckpointStoreReadsEveryCheckpointTheReleaseWrote) {
    SCOPED_TRACE(guard_message());
    const auto& expected = manifest().at("checkpoints");
    const auto  thread   = expected.at("thread_id").get<std::string>();
    graph::SqliteCheckpointStore store(copy_of("checkpoints.sqlite"));

    const auto listed = store.list(thread, 100);
    const auto& recorded = expected.at("checkpoints_newest_first");
    ASSERT_EQ(listed.size(), recorded.size());
    ASSERT_GE(listed.size(), 2u) << "the fixture needs a checkpoint before the paused one";
    for (std::size_t i = 0; i < listed.size(); ++i) {
        SCOPED_TRACE("checkpoint " + std::to_string(i) + " (newest first)");
        expect_matches(listed[i], recorded[i]);
        const auto by_id = store.load_by_id(listed[i].id);
        ASSERT_TRUE(by_id);
        expect_matches(*by_id, recorded[i]);
    }
    const auto latest = store.load_latest(thread);
    ASSERT_TRUE(latest);
    expect_matches(*latest, recorded_paused());
    EXPECT_EQ(latest->interrupt_phase, graph::CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(trace_of(latest->channel_values), json::array({"prep"}));
    EXPECT_EQ(recorded_earlier().at("interrupt_phase"), "Completed");
}

TEST_F(ReleasedData, PausedRunResumesToCompletionWithoutRunningEarlierNodesAgain) {
    SCOPED_TRACE(guard_message());
    auto released = open_released(*this, copy_of("checkpoints.sqlite"));
    expect_matches(*released.store->load_latest(released.thread), recorded_paused());

    const auto done = resumed([&] { return released.engine->resume(released.thread, json{{"approved", true}}); });
    ASSERT_FALSE(done.interrupted) << guard_message();
    EXPECT_EQ(channel(done, "answer"), (json{{"approved", true}}));
    EXPECT_EQ(channel(done, "result"), json("done"));
    EXPECT_EQ(channel(done, "note"), json("prepared:fixture"));
    // The trace channel appends: "prep" once means the stored state was kept.
    EXPECT_EQ(channel(done, "trace"), (json::array({"prep", "approve", "finish"})));
    EXPECT_EQ(runs().prep.load(), 0) << "a node before the pause ran again";
    EXPECT_EQ(runs().approve_asked.load(), 0);
    EXPECT_EQ(runs().approve_answered.load(), 1);
    EXPECT_EQ(runs().finish.load(), 1);

    // The resumed run saved its checkpoints beside the released ones.
    const auto latest = released.store->load_latest(released.thread);
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->schema_version, graph::CHECKPOINT_SCHEMA_VERSION);
    EXPECT_GT(released.store->list(released.thread, 100).size(),
              manifest().at("checkpoints").at("checkpoints_newest_first").size());
}

TEST_F(ReleasedData, ExactCheckpointResumeFromThePausedCheckpointContinuesTheAskingNode) {
    SCOPED_TRACE(guard_message());
    auto released = open_released(*this, copy_of("checkpoints.sqlite"));
    const auto paused = released.store->load_by_id(recorded_paused().at("id").get<std::string>());
    ASSERT_TRUE(paused);
    expect_matches(*paused, recorded_paused());

    const auto done = resumed([&] {
        return released.engine->resume_from(released.config, recorded_paused().at("id").get<std::string>(),
                                            json{{"approved", false}});
    });
    ASSERT_FALSE(done.interrupted) << guard_message();
    EXPECT_EQ(channel(done, "answer"), (json{{"approved", false}}));
    EXPECT_EQ(channel(done, "result"), json("done"));
    EXPECT_EQ(channel(done, "trace"), (json::array({"prep", "approve", "finish"})));
    EXPECT_EQ(runs().prep.load(), 0) << "a node before the pause ran again";
    EXPECT_EQ(runs().approve_answered.load(), 1);
    EXPECT_EQ(runs().finish.load(), 1);
}

TEST_F(ReleasedData, ExactCheckpointResumeFromTheEarlierCheckpointReplaysFromThatCheckpoint) {
    SCOPED_TRACE(guard_message());
    auto released = open_released(*this, copy_of("checkpoints.sqlite"));
    const auto earlier_id = recorded_earlier().at("id").get<std::string>();
    const auto before     = released.store->list(released.thread, 100).size();
    const auto stored     = released.store->load_by_id(earlier_id);
    ASSERT_TRUE(stored);
    expect_matches(*stored, recorded_earlier());
    EXPECT_NE(stored->interrupt_phase, graph::CheckpointPhase::NodeInterrupt);

    const auto again = resumed([&] { return released.engine->resume_from(released.config, earlier_id); });
    ASSERT_TRUE(again.interrupted) << guard_message();
    EXPECT_EQ(again.interrupt_node, manifest().at("checkpoints").at("interrupt_node").get<std::string>());
    EXPECT_EQ(again.interrupt_value, manifest().at("checkpoints").at("interrupt_value"));
    // Only the node after the earlier checkpoint ran, and it asked again.
    EXPECT_EQ(runs().prep.load(), 0);
    EXPECT_EQ(runs().approve_asked.load(), 1);
    EXPECT_EQ(runs().finish.load(), 0);

    // The new pause was saved as a child of the earlier checkpoint, not of the
    // released pause: the released pause was not just returned.
    const auto pause = released.store->load_latest(released.thread);
    ASSERT_TRUE(pause);
    EXPECT_NE(pause->id, recorded_paused().at("id").get<std::string>());
    EXPECT_EQ(pause->parent_id, earlier_id);
    EXPECT_EQ(pause->interrupt_phase, graph::CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(pause->schema_version, graph::CHECKPOINT_SCHEMA_VERSION);
    EXPECT_EQ(pause->channel_values.at("channels"), recorded_earlier().at("channel_values").at("channels"));
    EXPECT_EQ(released.store->list(released.thread, 100).size(), before + 1);
}

// A resume refuses a checkpoint whose stored number is not the engine's. The
// number is changed in the temporary copy, so these fail when a refusal is
// removed. The older and the newer value both stand for a change of the
// constant.
class CheckpointSchemaRefusal : public ReleasedData, public ::testing::WithParamInterface<int> {
protected:
    std::string stored_number() const { return std::to_string(static_cast<int>(graph::CHECKPOINT_SCHEMA_VERSION) + GetParam()); }
    std::string set_number() const { return "UPDATE neograph_checkpoints SET schema_version=" + stored_number(); }
};

TEST_P(CheckpointSchemaRefusal, AnExactCheckpointResumeRefusesTheCheckpointOfAnotherVersion) {
    auto released = open_released(*this, copy_of("checkpoints.sqlite"), set_number());
    for (const auto& recorded : {recorded_paused(), recorded_earlier()}) {
        const auto message = refusal([&] {
            released.engine->resume_from(released.config, recorded.at("id").get<std::string>(), json{{"approved", true}});
        });
        EXPECT_EQ(message, "Checkpoint schema version is incompatible") << recorded.at("interrupt_phase");
    }
    EXPECT_EQ(runs().prep.load() + runs().approve_asked.load() + runs().approve_answered.load() + runs().finish.load(), 0)
        << "a refused resume ran a node";
}

TEST_P(CheckpointSchemaRefusal, ANodeInterruptResumeRefusesAPauseWhoseAncestryHasAnotherVersion) {
    auto released = open_released(*this, copy_of("checkpoints.sqlite"), set_number());
    const auto message = refusal([&] { released.engine->resume(released.thread, json{{"approved", true}}); });
    EXPECT_EQ(message, "Incompatible checkpoint in NodeInterrupt ancestry");
    EXPECT_EQ(runs().approve_answered.load(), 0);

    // Only the pause itself carries another number: the walk that replays its
    // pending writes reaches the checkpoint the step started from, which is
    // current, so it is the pause that is refused.
    const auto only_pause = open_released(*this, copy_of("checkpoints.sqlite", "only-pause.sqlite"),
                                          set_number() + " WHERE checkpoint_id='" +
                                              recorded_paused().at("id").get<std::string>() + "'");
    EXPECT_EQ(refusal([&] { only_pause.engine->resume(only_pause.thread, json{{"approved", true}}); }),
              "Incompatible checkpoint in NodeInterrupt ancestry");
    // Only the checkpoint the pause started from: the walk reaches it.
    const auto only_parent = open_released(*this, copy_of("checkpoints.sqlite", "only-parent.sqlite"),
                                           set_number() + " WHERE checkpoint_id='" +
                                               recorded_earlier().at("id").get<std::string>() + "'");
    EXPECT_EQ(refusal([&] { only_parent.engine->resume(only_parent.thread, json{{"approved", true}}); }),
              "Missing or incompatible parent checkpoint in NodeInterrupt ancestry");
}

TEST_P(CheckpointSchemaRefusal, APlainResumeOfALatestCompletedCheckpointDoesNotLookAtTheNumber) {
    // Without the pause, the latest checkpoint is the Completed one the step
    // started from: a plain resume continues from it whatever number it holds,
    // runs the node after it and pauses there again.
    auto released = open_released(*this, copy_of("checkpoints.sqlite"),
                                  "DELETE FROM neograph_checkpoints WHERE checkpoint_id='" +
                                      recorded_paused().at("id").get<std::string>() + "';" + set_number());
    const auto again = resumed([&] { return released.engine->resume(released.thread, json()); });
    ASSERT_TRUE(again.interrupted);
    EXPECT_EQ(again.interrupt_node, manifest().at("checkpoints").at("interrupt_node").get<std::string>());
    EXPECT_EQ(runs().prep.load(), 0);
    EXPECT_EQ(runs().approve_asked.load(), 1);
}

INSTANTIATE_TEST_SUITE_P(AnotherVersion, CheckpointSchemaRefusal, ::testing::Values(-1, 1),
                         [](const ::testing::TestParamInfo<int>& info) { return info.param < 0 ? "Older" : "Newer"; });

// ---------------------------------------------------------------------------
// Runtime stores
// ---------------------------------------------------------------------------

namespace {

int schema_version(const std::string& path) {
    const auto rows = dump(path, "SELECT version FROM ng_runtime_store_schema");
    return rows.size() == 1 ? std::stoi(rows[0][0]) : -1;
}

std::vector<std::string> column_names(const std::string& path, const std::string& table) {
    std::vector<std::string> names;
    for (const auto& row : dump(path, "SELECT name FROM pragma_table_info('" + table + "') ORDER BY cid"))
        names.push_back(row[0]);
    return names;
}

bool has_schema_object(const std::string& path, const std::string& name) {
    return !dump(path, "SELECT 1 FROM sqlite_master WHERE name='" + name + "'").empty();
}

// What the three tables the migration must keep hold, byte for byte.
struct KeptTables {
    Rows history, artifacts, receipts;
    bool operator==(const KeptTables&) const = default;
};
KeptTables kept_tables(const std::string& path) {
    return {dump(path, "SELECT owner_id, feed_id, sequence, record_id, canonical FROM ng_runtime_history ORDER BY feed_id, sequence"),
            dump(path, "SELECT owner_id, artifact_id, canonical FROM ng_runtime_artifacts ORDER BY artifact_id"),
            dump(path, "SELECT owner_scope, dispatch_id, canonical FROM ng_provider_receipts ORDER BY dispatch_id")};
}

// A schema version 2 or 1 file, made from the file the release wrote. Where the
// shapes come from (git history of src/core/sqlite_runtime_stores.cpp):
//  - 78efdadd is the first commit of the file. It already creates
//    ng_provider_receipts with an owner scope, writes version 2, and carries the
//    branch that rebuilds a version 1 receipt table with
//    "SELECT '', dispatch_id, canonical FROM ng_provider_receipts".
//  - 37bf29ea adds ng_provider_outcomes and version 3. Both commits first ship
//    in v0.12.0, so a released build wrote version 3 from the start: no
//    released file has version 1 or 2, and these are the shapes of development
//    builds in between.
//  - 76555c93 (first in v0.13.0) adds the unique index
//    ng_runtime_history_message_identity, so neither older version has it.
//  - History, artifact and Hook outbox tables were created the same way at
//    every version (CREATE TABLE IF NOT EXISTS, same columns).
// Not recoverable: a CREATE TABLE of the version 1 receipt table. Its columns
// follow from the migration's SELECT (dispatch_id, canonical); the primary key
// here is a guess, and the canonical bytes are those of the version 3 file.
void reshape_to_version(const std::string& path, int version) {
    execute(path, "DROP TABLE ng_provider_outcomes; DROP INDEX ng_runtime_history_message_identity");
    if (version == 1) {
        execute(path,
                "CREATE TABLE ng_provider_receipts_v1(dispatch_id TEXT PRIMARY KEY NOT NULL, canonical BLOB NOT NULL);"
                "INSERT INTO ng_provider_receipts_v1 SELECT dispatch_id, canonical FROM ng_provider_receipts;"
                "DROP TABLE ng_provider_receipts;"
                "ALTER TABLE ng_provider_receipts_v1 RENAME TO ng_provider_receipts");
    }
    execute(path, "UPDATE ng_runtime_store_schema SET version=" + std::to_string(version));
}

}  // namespace

TEST_F(ReleasedData, RuntimeStoresReadWhatTheReleaseWrote) {
    const auto& expected = manifest().at("runtime_stores");
    const auto  path     = copy_of("runtime_stores.sqlite");
    const ContextStoreFeed feed{expected.at("owner_id").get<std::string>(), expected.at("feed_id").get<std::string>()};
    const auto record_ids  = expected.at("history_record_ids").get<std::vector<std::string>>();
    const auto message_ids = expected.at("history_message_ids").get<std::vector<std::string>>();
    const auto canonical   = expected.at("history_records_canonical").get<std::vector<std::string>>();
    const auto texts       = expected.at("history_texts").get<std::vector<std::string>>();

    SQLiteContextStore contexts(path);
    const auto         head = contexts.history_head(feed);
    EXPECT_EQ(head.sequence, record_ids.size());
    EXPECT_EQ(head.record_id, record_ids.back());
    const auto range   = contexts.snapshot_history(feed, 1, record_ids.size());
    const auto records = contexts.hydrate_records(range);
    ASSERT_EQ(records.size(), record_ids.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].id(), record_ids[i]);
        // The whole body: the message text is part of the canonical bytes.
        EXPECT_EQ(records[i].serialize_canonical(), canonical[i]);
        EXPECT_NE(canonical[i].find(texts[i]), std::string::npos) << "the recorded body lacks its text";
        const auto by_message = contexts.history_record_by_message_id(feed, message_ids[i]);
        ASSERT_TRUE(by_message);
        EXPECT_EQ(by_message->serialize_canonical(), canonical[i]);
    }
    EXPECT_EQ(contexts.hydrate_history(range), canonical[0] + "\n" + canonical[1]);

    const auto artifact_id = expected.at("artifact_id").get<std::string>();
    const auto artifact    = contexts.get_artifact(feed.owner_id, artifact_id);
    ASSERT_TRUE(artifact);
    EXPECT_EQ(artifact->id(), artifact_id);
    EXPECT_EQ(artifact->serialize_canonical(), expected.at("artifact_canonical").get<std::string>());
    EXPECT_NE(expected.at("artifact_canonical").get<std::string>().find(
                  expected.at("artifact_content").at("text").get<std::string>()),
              std::string::npos)
        << "the recorded artifact lacks its content";
    EXPECT_FALSE(contexts.get_artifact("another-owner", artifact_id));

    SQLiteProviderDispatchReceiptStore receipts(path);
    const auto settled = expected.at("settled_dispatch_id").get<std::string>();
    const auto pending = expected.at("pending_dispatch_id").get<std::string>();
    EXPECT_EQ(receipts.state({}, settled), ProviderDispatchState::Succeeded);
    EXPECT_EQ(receipts.state({}, pending), ProviderDispatchState::AdmittedPending);
    const auto outcome = receipts.outcome({}, settled);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->id(), expected.at("outcome_id").get<std::string>());
    EXPECT_EQ(outcome->serialize_canonical(), expected.at("outcome_canonical").get<std::string>());
    EXPECT_EQ(outcome->dispatch_receipt_id(), expected.at("settled_receipt_id").get<std::string>());
    // The stored receipt bodies, as the wheel serialized them.
    EXPECT_EQ(dump(path, "SELECT owner_scope, dispatch_id, canonical FROM ng_provider_receipts ORDER BY dispatch_id"),
              (Rows{{"", pending, expected.at("pending_receipt_canonical").get<std::string>()},
                    {"", settled, expected.at("settled_receipt_canonical").get<std::string>()}}));
    EXPECT_EQ(ProviderDispatchReceipt::parse(expected.at("settled_receipt_canonical").get<std::string>()).id(),
              expected.at("settled_receipt_id").get<std::string>());

    // What this proves about Hooks is only that the file opens with a Hook
    // outbox the current code can scan. The wheel cannot write an outbox row
    // (its Python API has no durable Hook journal), so the table the release
    // left is empty; opening the journal also creates the table if a file lacked
    // it, so an empty scan is not evidence about what the release wrote. The
    // next test enqueues a Hook on this file to cover writing.
    SQLiteHookJournal hooks(path);
    EXPECT_TRUE(hooks.pending().empty());
    EXPECT_TRUE(hooks.reconciliation_required().empty());
    EXPECT_EQ(expected.at("hook_outbox_rows").get<int>(), 0);
}

TEST_F(ReleasedData, RuntimeStoresTakeNewRecordsAfterTheReleasedOnes) {
    const auto& expected = manifest().at("runtime_stores");
    const auto  path     = copy_of("runtime_stores.sqlite");
    const ContextStoreFeed feed{expected.at("owner_id").get<std::string>(), expected.at("feed_id").get<std::string>()};
    const auto head_id = expected.at("history_record_ids").back().get<std::string>();

    SQLiteContextStore contexts(path);
    RuntimeHistoryRecordData data;
    data.feed_id        = feed.feed_id;
    data.sequence       = expected.at("history_record_ids").size() + 1;
    data.message_id     = "current-release-message";
    data.trust          = RuntimeTrustClass::UntrustedInput;
    data.message        = test::message("written by the current code", sp::Role::User);
    data.predecessor_id = head_id;
    const auto next = RuntimeHistoryRecord::create(std::move(data));
    EXPECT_EQ(contexts.append_history(feed, next, head_id), ContextStoreAppendResult::Appended);
    EXPECT_EQ(contexts.history_head(feed).record_id, next.id());

    // A Hook invocation enqueued on the released file is claimed and read back.
    SQLiteHookJournal hooks(path);
    const auto        now   = std::chrono::system_clock::now();
    const auto        event = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json::object()});
    const auto        call  = HookInvocation::create({{}, "sha256:" + std::string(64, 'a'), event.id(), "audit",
                                                      HookPhase::BeforeToolExecution, HookDelivery::BlockingMandatory,
                                                      HookFailureMode::FailClosed, HookIdempotency::Idempotent,
                                                      ToolEffectClass::ReadOnly, {}, {}, json::object()});
    hooks.enqueue(call, event, 2, now + std::chrono::minutes(1));
    hooks.publish(call.id());
    EXPECT_TRUE(hooks.claim(call.id(), "worker", now, std::chrono::seconds(30)));
    EXPECT_TRUE(hooks.get(call.id()));
}

// The migration the store runs when it opens a file of an older schema version.
class RuntimeStoreMigration : public ReleasedData, public ::testing::WithParamInterface<int> {};

TEST_P(RuntimeStoreMigration, AnOlderFileMigratesToTheCurrentSchemaAndKeepsHistoryArtifactsAndReceiptsByteForByte) {
    const int  older     = GetParam();
    const auto& expected = manifest().at("runtime_stores");
    const auto  path     = copy_of("runtime_stores.sqlite");
    const auto  before   = kept_tables(path);
    ASSERT_EQ(before.receipts.size(), 2u);
    ASSERT_EQ(before.history.size(), 2u);
    ASSERT_EQ(before.artifacts.size(), 1u);
    reshape_to_version(path, older);
    ASSERT_EQ(schema_version(path), older);
    ASSERT_FALSE(has_schema_object(path, "ng_provider_outcomes"));
    ASSERT_FALSE(has_schema_object(path, "ng_runtime_history_message_identity"));
    ASSERT_EQ(column_names(path, "ng_provider_receipts").front(), older == 1 ? "dispatch_id" : "owner_scope");

    {
        // Opening any store of the file runs the migration, once.
        SQLiteContextStore contexts(path);
    }
    EXPECT_EQ(schema_version(path), 3);
    EXPECT_EQ(column_names(path, "ng_provider_receipts"),
              (std::vector<std::string>{"owner_scope", "dispatch_id", "canonical"}));
    EXPECT_TRUE(has_schema_object(path, "ng_provider_outcomes"));
    EXPECT_TRUE(has_schema_object(path, "ng_runtime_history_message_identity"));
    // A version 1 receipt takes the empty owner scope, which is what the
    // release wrote; every row is otherwise the bytes it was.
    const auto migrated = kept_tables(path);
    EXPECT_EQ(migrated.receipts, before.receipts);
    EXPECT_EQ(migrated.history, before.history);
    EXPECT_EQ(migrated.artifacts, before.artifacts);
    // The outcome table did not exist in the older file; it starts empty.
    EXPECT_TRUE(dump(path, "SELECT 1 FROM ng_provider_outcomes").empty());

    // A second open migrates nothing.
    {
        SQLiteProviderDispatchReceiptStore receipts(path);
    }
    EXPECT_EQ(schema_version(path), 3);
    EXPECT_EQ(kept_tables(path), before);

    // The migrated file works: the settled dispatch has no outcome until it is
    // settled again, and the outcome the release wrote is accepted as it was.
    SQLiteProviderDispatchReceiptStore receipts(path);
    const auto settled = expected.at("settled_dispatch_id").get<std::string>();
    EXPECT_EQ(receipts.state({}, settled), ProviderDispatchState::AdmittedPending);
    EXPECT_FALSE(receipts.outcome({}, settled));
    const auto outcome = ProviderDispatchOutcomeReceipt::parse(expected.at("outcome_canonical").get<std::string>());
    EXPECT_EQ(receipts.settle({}, outcome), ProviderDispatchOutcomePutResult::Stored);
    EXPECT_EQ(receipts.state({}, settled), ProviderDispatchState::Succeeded);
    ASSERT_TRUE(receipts.outcome({}, settled));
    EXPECT_EQ(receipts.outcome({}, settled)->serialize_canonical(), expected.at("outcome_canonical").get<std::string>());
    SQLiteContextStore contexts(path);
    EXPECT_EQ(contexts.history_head({expected.at("owner_id").get<std::string>(), expected.at("feed_id").get<std::string>()})
                  .record_id,
              expected.at("history_record_ids").back().get<std::string>());
}

INSTANTIATE_TEST_SUITE_P(OlderSchemaVersions, RuntimeStoreMigration, ::testing::Values(1, 2),
                         [](const ::testing::TestParamInfo<int>& info) { return "V" + std::to_string(info.param); });

TEST_F(ReleasedData, ARuntimeStoreOfANewerSchemaIsRefusedAndTheFileIsLeftAsItWas) {
    const auto path = copy_of("runtime_stores.sqlite");
    execute(path, "UPDATE ng_runtime_store_schema SET version=1000000");
    const auto bytes = read_bytes(path);
    const auto files = files_under(scratch_.path());

    EXPECT_THROW(SQLiteContextStore{path}, std::runtime_error);
    EXPECT_THROW(SQLiteProviderDispatchReceiptStore{path}, std::runtime_error);
    EXPECT_THROW(SQLiteHookJournal{path}, std::runtime_error);

    EXPECT_EQ(read_bytes(path), bytes);
    EXPECT_EQ(files_under(scratch_.path()), files) << "a refused open left a file beside the store";
    EXPECT_EQ(schema_version(path), 1000000);
}

// ---------------------------------------------------------------------------
// Program stores
// ---------------------------------------------------------------------------

#ifdef NEOGRAPH_RELEASED_COMPAT_HAVE_PROGRAM
namespace {
using namespace neograph::program;

// How many times each Program node body ran, and how many of those answered.
struct ProgramRuns {
    std::mutex                mutex;
    std::map<std::string, int> runs;
    void count(const std::string& key) {
        std::lock_guard lock(mutex);
        ++runs[key];
    }
    int get(const std::string& key) {
        std::lock_guard lock(mutex);
        return runs[key];
    }
    void reset() {
        std::lock_guard lock(mutex);
        runs.clear();
    }
};
ProgramRuns& program_runs() {
    static ProgramRuns value;
    return value;
}

class ProgramStep final : public graph::GraphNode {
public:
    ProgramStep(std::string name, std::string ask) : name_(std::move(name)), ask_(std::move(ask)) {}
    asio::awaitable<graph::NodeResult> run(graph::NodeInput in) override {
        graph::NodeResult out;
        out.writes.push_back(graph::ChannelWrite{"trace", json::array({name_})});
        if (!ask_.empty()) {
            const auto& answer = in.ctx.resume_value;
            if (!answer) {
                program_runs().count(name_ + ":asked");
                throw graph::NodeInterrupt("needs approval", json{{"question", ask_}});
            }
            program_runs().count(name_ + ":answered");
            out.writes.push_back(graph::ChannelWrite{"answer", *answer});
        } else {
            program_runs().count(name_ + ":ran");
        }
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::string ask_;
};

// The manifest the release's registry builder gives a registered executable.
ExecutableManifest registered(ExecutableKind kind, const json& entry) {
    return ExecutableManifest{{kind, entry.at("name").get<std::string>(), entry.at("version").get<std::string>(),
                               entry.at("digest").get<std::string>()},
                              EffectMode::Brokered,
                              "python-registered-executable",
                              {},
                              {},
                              {},
                              ExecutionGuarantee::Strict};
}

// Built as the wheel's registry builder builds one: the node type is registered
// with the process first, and its manifest takes that registration's config
// schema and effect declaration.
RegistrySnapshot released_registry(const json& registry) {
    const auto type = registry.at("node").at("name").get<std::string>();
    graph::NodeFactory::instance().register_type(
        type, [](const std::string& name, const json& config, const graph::NodeContext&) {
            return std::make_unique<ProgramStep>(name, config.value("ask", std::string()));
        });
    auto schema  = graph::NodeFactory::instance().config_schema(type);
    auto effects = graph::NodeFactory::instance().node_effects(type);
    if (!schema.is_object()) schema = json{{"type", "object"}};
    if (!effects.is_object()) effects = json::object();
    RegistrySnapshotBuilder builder;
    builder.add_node(
        registered(ExecutableKind::Node, registry.at("node")),
        [type](const std::string& name, const json& config, const graph::NodeContext& context) {
            return graph::NodeFactory::instance().create(type, name, config, context);
        },
        std::move(schema), std::move(effects));
    for (const auto& reducer : registry.at("reducers")) {
        const auto name = reducer.at("name").get<std::string>();
        builder.add_reducer(registered(ExecutableKind::Reducer, reducer), graph::ReducerRegistry::instance().get(name));
    }
    return std::move(builder).build();
}

// A Program host over copies of the released Program stores.
struct ReleasedProgram {
    explicit ReleasedProgram(ReleasedData& test, const std::string& program, const std::string& checkpoints_file)
        : store(std::make_shared<SQLiteProgramStore>(program)),
          transitions(std::make_shared<SQLiteProgramTransitionStore>(program)),
          checkpoints(std::make_shared<graph::SqliteCheckpointStore>(checkpoints_file)) {
        (void)test;
        const auto& expected = manifest().at("program");
        program_runs().reset();
        CatalogConfig catalog_config{store, released_registry(expected.at("registry")),
                                     std::make_shared<EngineGenerationCache>(),
                                     expected.at("compiler_build_id").get<std::string>()};
        catalog_config.worker_count      = 1;
        catalog_config.capability_binder = program::detail::single_provider_binder(std::nullopt, {});
        RuntimeConfig config;
        config.catalog           = std::make_shared<ProgramCatalog>(std::move(catalog_config));
        config.checkpoints       = checkpoints;
        config.transitions       = transitions;
        config.scheduler_threads = 1;
        runtime = std::make_unique<ProgramRuntime>(std::move(config));
    }
    program::test_support::ProgramStoreHandle     store;
    program::test_support::TransitionStoreHandle  transitions;
    std::shared_ptr<graph::SqliteCheckpointStore> checkpoints;
    std::unique_ptr<ProgramRuntime>               runtime;
};

// Admission failures carry their diagnostics, which the message alone hides.
template <class Fn>
void with_diagnostics(Fn&& body) {
    try {
        body();
    } catch (const ProgramAdmissionError& error) {
        std::string text = error.what();
        for (const auto& diagnostic : error.diagnostics())
            text += "\n" + diagnostic.code + " " + diagnostic.message + " " + diagnostic.witness.dump();
        FAIL() << text;
    }
}
}  // namespace

TEST_F(ReleasedData, PausedProgramRunResumesToCompletionWithoutRunningItsFirstNodeAgain) {
    with_diagnostics([&] {
        const auto& expected = manifest().at("program");
        const auto  owner    = expected.at("owner_scope").get<std::string>();
        const auto  run_id   = expected.at("run_id").get<std::string>();
        ReleasedProgram host(*this, copy_of("program/program.db"), copy_of("program/checkpoints.db"));

        // The state the release left: the run is interrupted, and its Core
        // checkpoint holds the nodes that ran before the pause.
        const auto record = host.transitions->load(owner, run_id);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->continuation().state, ContinuationState::Interrupted);
        const auto core = host.checkpoints->load_by_id(expected.at("checkpoint_id").get<std::string>());
        ASSERT_TRUE(core);
        const auto before = expected.at("execution_trace").get<std::vector<std::string>>();
        EXPECT_EQ(trace_of(core->channel_values), json(before));
        EXPECT_EQ(core->interrupt_phase, graph::CheckpointPhase::NodeInterrupt);
        EXPECT_EQ(core->current_node, expected.at("core_node").get<std::string>());

        // A resume that names another pending input is refused and runs nothing.
        EXPECT_ANY_THROW(host.runtime
                             ->resume(owner, run_id,
                                      ProgramResume{json{{"approved", true}}, "released-wrong", {},
                                                    expected.at("pending_call_id").get<std::string>() + "-other"})
                             .wait());
        EXPECT_EQ(program_runs().get("confirm:answered"), 0);

        const auto result = host.runtime
                                ->resume(owner, run_id,
                                         ProgramResume{json{{"approved", true}}, "released-resume", {},
                                                       expected.at("pending_call_id").get<std::string>()})
                                .wait();
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
            << (result.failure() ? result.failure()->code + ": " + result.failure()->message : std::string("no failure"));
        EXPECT_EQ(result.run_id(), run_id);
        const auto& channels = result.output().at("channels");
        EXPECT_EQ(channels.at("answer").at("value"), (json{{"approved", true}}));
        // The trace appends: the node that ran before the pause is there once.
        auto after = before;
        after.push_back(expected.at("core_node").get<std::string>());
        EXPECT_EQ(channels.at("trace").at("value"), json(after));
        EXPECT_EQ(program_runs().get("draft:ran"), 0) << "the node before the pause ran again";
        EXPECT_EQ(program_runs().get("confirm:asked"), 0);
        EXPECT_EQ(program_runs().get("confirm:answered"), 1);
    });
}
#endif
