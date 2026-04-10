#include <gtest/gtest.h>
#include <neograph/graph/store.h>

using namespace neograph::graph;
using json = nlohmann::json;

class StoreTest : public ::testing::Test {
protected:
    InMemoryStore store;
};

TEST_F(StoreTest, PutAndGet) {
    store.put({"users", "u1"}, "name", json("Alice"));
    auto item = store.get({"users", "u1"}, "name");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->value, "Alice");
    EXPECT_EQ(item->key, "name");
}

TEST_F(StoreTest, GetNonExistent) {
    auto item = store.get({"users", "u1"}, "missing");
    EXPECT_FALSE(item.has_value());
}

TEST_F(StoreTest, PutOverwrite) {
    store.put({"ns"}, "key", json(1));
    store.put({"ns"}, "key", json(2));
    auto item = store.get({"ns"}, "key");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->value, 2);
}

TEST_F(StoreTest, DeleteItem) {
    store.put({"ns"}, "key", json("val"));
    store.delete_item({"ns"}, "key");
    EXPECT_FALSE(store.get({"ns"}, "key").has_value());
}

TEST_F(StoreTest, SearchByPrefix) {
    store.put({"users", "u1"}, "name", json("Alice"));
    store.put({"users", "u1"}, "age", json(30));
    store.put({"users", "u2"}, "name", json("Bob"));
    store.put({"orders", "o1"}, "total", json(100));

    auto results = store.search({"users"});
    EXPECT_EQ(results.size(), 3);

    auto user1_results = store.search({"users", "u1"});
    EXPECT_EQ(user1_results.size(), 2);
}

TEST_F(StoreTest, SearchWithLimit) {
    store.put({"ns"}, "a", json(1));
    store.put({"ns"}, "b", json(2));
    store.put({"ns"}, "c", json(3));

    auto results = store.search({"ns"}, 2);
    EXPECT_EQ(results.size(), 2);
}

TEST_F(StoreTest, ListNamespaces) {
    store.put({"users", "u1"}, "k", json(1));
    store.put({"users", "u2"}, "k", json(2));
    store.put({"orders", "o1"}, "k", json(3));

    auto all_ns = store.list_namespaces();
    EXPECT_GE(all_ns.size(), 3);

    auto user_ns = store.list_namespaces({"users"});
    EXPECT_EQ(user_ns.size(), 2);
}

TEST_F(StoreTest, SizeCount) {
    EXPECT_EQ(store.size(), 0);
    store.put({"ns"}, "a", json(1));
    store.put({"ns"}, "b", json(2));
    EXPECT_EQ(store.size(), 2);
}

TEST_F(StoreTest, Timestamps) {
    store.put({"ns"}, "key", json("val"));
    auto item = store.get({"ns"}, "key");
    ASSERT_TRUE(item.has_value());
    EXPECT_GT(item->created_at, 0);
    EXPECT_GE(item->updated_at, item->created_at);
}

TEST_F(StoreTest, EmptyNamespace) {
    store.put({}, "root_key", json("root_val"));
    auto item = store.get({}, "root_key");
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->value, "root_val");
}
