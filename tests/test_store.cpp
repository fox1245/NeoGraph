#include <gtest/gtest.h>
#include <neograph/graph/store.h>

using namespace neograph::graph;
using json = neograph::json;

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

TEST_F(StoreTest, StructuredIdentityAvoidsDelimiterCollisions) {
    store.put({"a", "b"}, "c", json("nested"));
    store.put({"a"}, "b/c", json("slash-key"));

    EXPECT_EQ(store.size(), 2u);
    ASSERT_TRUE(store.get({"a", "b"}, "c").has_value());
    ASSERT_TRUE(store.get({"a"}, "b/c").has_value());
    EXPECT_EQ(store.get({"a", "b"}, "c")->value, "nested");
    EXPECT_EQ(store.get({"a"}, "b/c")->value, "slash-key");

    store.delete_item({"a", "b"}, "c");
    EXPECT_FALSE(store.get({"a", "b"}, "c").has_value());
    auto survivor = store.get({"a"}, "b/c");
    ASSERT_TRUE(survivor.has_value());
    EXPECT_EQ(survivor->value, "slash-key");
}

TEST_F(StoreTest, NamespacePrefixMatchesWholeComponents) {
    store.put({"user", "one"}, "key", json(1));
    store.put({"user", "two"}, "key", json(2));
    store.put({"user2", "three"}, "key", json(3));

    auto items = store.search({"user"});
    ASSERT_EQ(items.size(), 2u);
    for (const auto& item : items) {
        ASSERT_FALSE(item.ns.empty());
        EXPECT_EQ(item.ns.front(), "user");
    }

    auto namespaces = store.list_namespaces({"user"});
    ASSERT_EQ(namespaces.size(), 2u);
    for (const auto& ns : namespaces) {
        ASSERT_FALSE(ns.empty());
        EXPECT_EQ(ns.front(), "user");
    }
}

TEST_F(StoreTest, EmptySlashAndUtf8ComponentsRemainDistinct) {
    const std::string snowman = "\xE2\x98\x83";
    store.put({}, "root", json("empty-namespace"));
    store.put({""}, "root", json("empty-component"));
    store.put({"", "a/b", snowman}, "k/x", json("slash-component"));
    store.put({"", "a", "b", snowman}, "k/x", json("split-components"));

    EXPECT_EQ(store.size(), 4u);
    auto empty_ns = store.get({}, "root");
    auto empty_component = store.get({""}, "root");
    auto slash_component = store.get({"", "a/b", snowman}, "k/x");
    auto split_components = store.get({"", "a", "b", snowman}, "k/x");
    ASSERT_TRUE(empty_ns.has_value());
    ASSERT_TRUE(empty_component.has_value());
    ASSERT_TRUE(slash_component.has_value());
    ASSERT_TRUE(split_components.has_value());
    EXPECT_EQ(empty_ns->value, "empty-namespace");
    EXPECT_EQ(empty_component->value, "empty-component");
    EXPECT_EQ(slash_component->value, "slash-component");
    EXPECT_EQ(split_components->value, "split-components");
}
