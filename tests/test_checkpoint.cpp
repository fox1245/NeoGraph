#include <gtest/gtest.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph::graph;
using json = nlohmann::json;

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
        cp.next_node = "__end__";
        cp.interrupt_phase = "completed";
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
