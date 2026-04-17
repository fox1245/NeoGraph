#include <gtest/gtest.h>
#include <neograph/llm/json_path.h>

using namespace neograph::llm;
using json = neograph::json;
namespace jp = neograph::llm::json_path;

// ── split_path ──

TEST(JsonPath, SplitBasic) {
    auto segs = jp::split_path("a.b.c");
    ASSERT_EQ(segs.size(), 3);
    EXPECT_EQ(segs[0], "a");
    EXPECT_EQ(segs[1], "b");
    EXPECT_EQ(segs[2], "c");
}

TEST(JsonPath, SplitEmpty) {
    auto segs = jp::split_path("");
    EXPECT_TRUE(segs.empty());
}

TEST(JsonPath, SplitSingle) {
    auto segs = jp::split_path("name");
    ASSERT_EQ(segs.size(), 1);
    EXPECT_EQ(segs[0], "name");
}

// ── is_index ──

TEST(JsonPath, IsIndexValid) {
    EXPECT_TRUE(jp::is_index("0"));
    EXPECT_TRUE(jp::is_index("123"));
}

TEST(JsonPath, IsIndexInvalid) {
    EXPECT_FALSE(jp::is_index(""));
    EXPECT_FALSE(jp::is_index("abc"));
    EXPECT_FALSE(jp::is_index("-1"));
    EXPECT_FALSE(jp::is_index("1.5"));
}

// ── at_path ──

TEST(JsonPath, AtPathObject) {
    json root = {{"a", {{"b", 42}}}};
    auto val = jp::at_path(root, "a.b");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->template get<int>(), 42);
}

TEST(JsonPath, AtPathArray) {
    json root = {{"items", json::array({10, 20, 30})}};
    auto val = jp::at_path(root, "items.1");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->template get<int>(), 20);
}

TEST(JsonPath, AtPathNested) {
    json root = {{"choices", json::array({{{"message", {{"content", "hello"}}}}})}};
    auto val = jp::at_path(root, "choices.0.message.content");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->template get<std::string>(), "hello");
}

TEST(JsonPath, AtPathNotFound) {
    json root = {{"a", 1}};
    auto val = jp::at_path(root, "b");
    EXPECT_FALSE(val.has_value());
}

TEST(JsonPath, AtPathEmptyReturnsRoot) {
    json root = {{"a", 1}};
    auto val = jp::at_path(root, "");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->dump(), root.dump());
}

TEST(JsonPath, AtPathArrayOutOfBounds) {
    json root = {{"arr", json::array({1, 2})}};
    auto val = jp::at_path(root, "arr.5");
    EXPECT_FALSE(val.has_value());
}

// ── has_path ──

TEST(JsonPath, HasPathTrue) {
    json root = {{"a", {{"b", 1}}}};
    EXPECT_TRUE(jp::has_path(root, "a.b"));
}

TEST(JsonPath, HasPathFalse) {
    json root = {{"a", 1}};
    EXPECT_FALSE(jp::has_path(root, "a.b.c"));
}

// ── get_path ──

TEST(JsonPath, GetPathWithDefault) {
    json root = {{"a", 42}};
    EXPECT_EQ(jp::get_path<int>(root, "a", 0), 42);
    EXPECT_EQ(jp::get_path<int>(root, "missing", -1), -1);
}

TEST(JsonPath, GetPathTypeMismatch) {
    json root = {{"a", "not_an_int"}};
    // Should return default when type conversion fails
    EXPECT_EQ(jp::get_path<int>(root, "a", -1), -1);
}

// ── set_path ──

TEST(JsonPath, SetPathSimple) {
    json root;
    jp::set_path(root, "a.b.c", json(42));
    EXPECT_EQ(root["a"]["b"]["c"].template get<int>(), 42);
}

TEST(JsonPath, SetPathOverwrite) {
    json root = {{"a", 1}};
    jp::set_path(root, "a", json(2));
    EXPECT_EQ(root["a"].template get<int>(), 2);
}

TEST(JsonPath, SetPathEmpty) {
    json root = {{"old", true}};
    jp::set_path(root, "", json("replaced"));
    EXPECT_EQ(root.get<std::string>(), "replaced");
}

TEST(JsonPath, SetPathCreatesIntermediates) {
    json root;
    jp::set_path(root, "deep.nested.value", json(true));
    EXPECT_TRUE(root["deep"]["nested"]["value"].get<bool>());
}
