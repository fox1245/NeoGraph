#include <gtest/gtest.h>
#include <neograph/json.h>

using json = neograph::json;
// ── Array endpoints (issue #26) ──

TEST(JsonArrayEndpoints, BackReturnsLast) {
    json arr = json::array();
    arr.push_back(json("alpha"));
    arr.push_back(json("beta"));
    arr.push_back(json("gamma"));
    EXPECT_EQ(arr.back().get<std::string>(), "gamma");
    EXPECT_EQ(arr.front().get<std::string>(), "alpha");
}

TEST(JsonArrayEndpoints, BackOnObjectAccess) {
    // The nlohmann muscle-memory pattern that motivated the fix:
    // msgs.back()["content"] on an array of message objects.
    json msgs = json::array();
    json m1 = {{"role", "user"},      {"content", "first"}};
    json m2 = {{"role", "assistant"}, {"content", "last"}};
    msgs.push_back(m1);
    msgs.push_back(m2);
    EXPECT_EQ(msgs.back()["content"].get<std::string>(), "last");
}

TEST(JsonArrayEndpoints, BackOnEmptyThrows) {
    json arr = json::array();
    EXPECT_THROW(arr.back(),  json::out_of_range);
    EXPECT_THROW(arr.front(), json::out_of_range);
}

TEST(JsonArrayEndpoints, BackOnNonArrayThrows) {
    json obj = {{"a", 1}};
    EXPECT_THROW(obj.back(),  json::type_error);
    EXPECT_THROW(obj.front(), json::type_error);
}

TEST(JsonEquality, ObjectOrderIsIrrelevantButArrayOrderAndValuesRemainSignificant) {
    const auto first = json::parse(R"({"nested":{"input":0,"output":null},"parts":[{"id":"a"},{"id":"b"}]})");
    const auto reordered = json::parse(R"({"parts":[{"id":"a"},{"id":"b"}],"nested":{"output":null,"input":0}})");
    EXPECT_EQ(first, reordered);
    EXPECT_NE(first, json::parse(R"({"nested":{"input":0,"output":null},"parts":[{"id":"b"},{"id":"a"}]})"));
    EXPECT_NE(first, json::parse(R"({"nested":{"input":null,"output":null},"parts":[{"id":"a"},{"id":"b"}]})"));
}

// ── Object erase (nlohmann's erase(key)) ──

TEST(JsonObjectErase, RemovesTheMemberAndCountsIt) {
    json obj = {{"id", "x"}, {"run_id", "r"}, {"n", 3}};
    EXPECT_EQ(obj.erase("id"), 1U);
    EXPECT_FALSE(obj.contains("id"));
    EXPECT_TRUE(obj.contains("run_id"));
    EXPECT_EQ(obj.size(), 2U);
    EXPECT_EQ(obj.erase("id"), 0U);
    EXPECT_EQ(obj.dump(), json({{"run_id", "r"}, {"n", 3}}).dump());
}

TEST(JsonObjectErase, RemovesNothingFromAValueThatIsNotAnObject) {
    json arr = json::array();
    arr.push_back(json("id"));
    EXPECT_EQ(arr.erase("id"), 0U);
    EXPECT_EQ(arr.size(), 1U);
    json number = 7;
    EXPECT_EQ(number.erase("id"), 0U);
}

TEST(JsonObjectErase, RemovesAMemberOfANestedObject) {
    json doc = {{"outer", {{"keep", 1}, {"drop", 2}}}};
    EXPECT_EQ(doc["outer"].erase("drop"), 1U);
    EXPECT_FALSE(doc.at("outer").contains("drop"));
    EXPECT_TRUE(doc.at("outer").contains("keep"));
}
