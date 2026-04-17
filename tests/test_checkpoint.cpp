#include <gtest/gtest.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph::graph;
using json = neograph::json;

class CheckpointTest : public ::testing::Test {
protected:
    InMemoryCheckpointStore store;

    Checkpoint make_cp(const std::string& thread_id, int step,
                       const std::string& node = "test_node") {
        Checkpoint cp;
        cp.id = Checkpoint::generate_id();
        cp.thread_id = thread_id;
        cp.channel_values = json{{"data", step}};
        cp.current_node = node;
        cp.next_nodes = {"__end__"};
        cp.interrupt_phase = CheckpointPhase::Completed;
        cp.step = step;
        cp.timestamp = step * 1000;  // deterministic ordering
        return cp;
    }
};

TEST_F(CheckpointTest, SaveAndLoadLatest) {
    auto cp = make_cp("thread-1", 0);
    store.save(cp);

    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->channel_values["data"], 0);
}

TEST_F(CheckpointTest, LoadLatestReturnsNewest) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    auto cp3 = make_cp("thread-1", 2);
    store.save(cp3);

    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp3.id);
    EXPECT_EQ(loaded->step, 2);
}

TEST_F(CheckpointTest, LoadByIdSuccess) {
    auto cp = make_cp("thread-1", 0);
    store.save(cp);

    auto loaded = store.load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->thread_id, "thread-1");
}

TEST_F(CheckpointTest, LoadByIdNotFound) {
    auto loaded = store.load_by_id("nonexistent-id");
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(CheckpointTest, LoadLatestNonExistentThread) {
    auto loaded = store.load_latest("nonexistent");
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(CheckpointTest, ListCheckpoints) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-1", 2));

    auto list = store.list("thread-1");
    EXPECT_EQ(list.size(), 3);
}

TEST_F(CheckpointTest, ListWithLimit) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-1", 2));

    auto list = store.list("thread-1", 2);
    EXPECT_EQ(list.size(), 2);
}

TEST_F(CheckpointTest, ListIsolatedByThread) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-2", 0));

    auto list1 = store.list("thread-1");
    auto list2 = store.list("thread-2");
    EXPECT_EQ(list1.size(), 1);
    EXPECT_EQ(list2.size(), 1);
}

TEST_F(CheckpointTest, DeleteThread) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-2", 0));

    store.delete_thread("thread-1");

    EXPECT_FALSE(store.load_latest("thread-1").has_value());
    EXPECT_TRUE(store.load_latest("thread-2").has_value());
}

TEST_F(CheckpointTest, SizeCount) {
    EXPECT_EQ(store.size(), 0);
    store.save(make_cp("t1", 0));
    store.save(make_cp("t2", 0));
    EXPECT_EQ(store.size(), 2);
}

TEST_F(CheckpointTest, GenerateIdUnique) {
    auto id1 = Checkpoint::generate_id();
    auto id2 = Checkpoint::generate_id();
    EXPECT_NE(id1, id2);
    EXPECT_GT(id1.size(), 10);  // UUID-like length
}

// A fresh Checkpoint (either default-constructed or built via make_cp)
// must carry the current schema version, so persistent stores serialize
// the field instead of writing 0 and later mis-classifying it as a
// pre-versioned blob needing migration.
TEST_F(CheckpointTest, FreshCheckpointCarriesCurrentSchemaVersion) {
    Checkpoint fresh;
    EXPECT_EQ(fresh.schema_version, CHECKPOINT_SCHEMA_VERSION);

    auto cp = make_cp("thread-1", 0);
    EXPECT_EQ(cp.schema_version, CHECKPOINT_SCHEMA_VERSION);

    store.save(cp);
    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->schema_version, CHECKPOINT_SCHEMA_VERSION);
}

// Confirms the contract that stores loading legacy blobs (which predate
// the field) can distinguish them by seeing schema_version == 0 and
// take a migration path — the in-memory store round-trips an explicit
// 0 unchanged.
TEST(CheckpointPhaseStrings, AllEnumValuesRoundTrip) {
    // Every enum value must survive to_string → parse_checkpoint_phase
    // so persistent stores can serialize phases as strings and load
    // them back losslessly. Guards against "I added an enum value but
    // forgot one of the two functions" drift.
    for (auto p : {CheckpointPhase::Before, CheckpointPhase::After,
                   CheckpointPhase::Completed, CheckpointPhase::NodeInterrupt,
                   CheckpointPhase::Updated}) {
        EXPECT_EQ(parse_checkpoint_phase(to_string(p)), p)
            << "phase " << to_string(p) << " failed round-trip";
    }
}

TEST(CheckpointPhaseStrings, ParseUnknownThrows) {
    EXPECT_THROW(parse_checkpoint_phase("garbage"), std::invalid_argument);
    EXPECT_THROW(parse_checkpoint_phase(""), std::invalid_argument);
}

TEST_F(CheckpointTest, PreVersionedSentinelRoundTrips) {
    auto cp = make_cp("legacy-thread", 0);
    cp.schema_version = 0;  // simulate a blob deserialized without the field
    store.save(cp);

    auto loaded = store.load_latest("legacy-thread");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->schema_version, 0)
        << "store must not silently upgrade — migration is the caller's "
           "responsibility so the caller can log / one-shot convert";
}
