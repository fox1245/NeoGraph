// Issue #25 — RunResult::channel<T>(name) / channel_raw / has_channel
//
// Two coexisting output shapes:
//   * canonical channels-wrapped: output["channels"][name]["value"]
//   * flat-key projection:        output[name]
// The accessor must handle both transparently.
//
// Tests construct RunResult directly with hand-built output JSON to
// keep the assertions focused on the accessor; engine end-to-end is
// covered in test_graph_engine.cpp.

#include <gtest/gtest.h>

#include <neograph/graph/engine.h>
#include <neograph/json.h>

using neograph::json;
using neograph::graph::RunResult;

namespace {

RunResult make_wrapped(const std::string& name, const json& value, int version = 1) {
    RunResult r;
    r.output = json{
        {"channels", {
            {name, {{"value", value}, {"version", version}}}
        }},
        {"global_version", version}
    };
    return r;
}

RunResult make_flat(const std::string& name, const json& value) {
    RunResult r;
    r.output = json{{name, value}};
    return r;
}

RunResult make_both(const std::string& name, const json& wrapped_value, const json& flat_value) {
    // Channels wrapper + flat-key projection (react_graph-style coexistence).
    RunResult r;
    r.output = json{
        {"channels", {
            {name, {{"value", wrapped_value}, {"version", 1}}}
        }},
        {"global_version", 1},
        {name, flat_value},
    };
    return r;
}

}  // namespace

TEST(RunResultChannel, ChannelWrappedScalar) {
    auto r = make_wrapped("counter", json(42));
    EXPECT_EQ(r.channel<int>("counter"), 42);
    EXPECT_TRUE(r.has_channel("counter"));
}

TEST(RunResultChannel, FlatKeyScalar) {
    auto r = make_flat("final_response", json("hello"));
    EXPECT_EQ(r.channel<std::string>("final_response"), "hello");
    EXPECT_TRUE(r.has_channel("final_response"));
}

TEST(RunResultChannel, ChannelWrappedTakesPrecedenceOverFlatKey) {
    // When both shapes coexist (react_graph), the wrapper is the
    // source of truth. Flat-key would shadow it if we read flat first.
    auto r = make_both("counter", json(7), json(99));
    EXPECT_EQ(r.channel<int>("counter"), 7);
}

TEST(RunResultChannel, MissingChannelThrowsOutOfRange) {
    RunResult r;
    r.output = json::object();
    EXPECT_THROW(r.channel<int>("nope"), json::out_of_range);
    EXPECT_FALSE(r.has_channel("nope"));
}

TEST(RunResultChannel, MissingChannelEmptyChannelsBlockThrows) {
    // channels exists but the named one isn't there → out_of_range.
    RunResult r;
    r.output = json{{"channels", json::object()}, {"global_version", 0}};
    EXPECT_THROW(r.channel<int>("nope"), json::out_of_range);
    EXPECT_FALSE(r.has_channel("nope"));
}

TEST(RunResultChannel, ChannelRawReturnsJsonNode) {
    // Channel value is itself a compound (array of strings).
    json arr = json::array();
    arr.push_back(json("alpha"));
    arr.push_back(json("beta"));
    auto r = make_wrapped("tags", arr);

    json got = r.channel_raw("tags");
    ASSERT_TRUE(got.is_array());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].get<std::string>(), "alpha");
    EXPECT_EQ(got[1].get<std::string>(), "beta");
}

TEST(RunResultChannel, ChannelRawFlatFallback) {
    auto r = make_flat("payload", json{{"k", "v"}});
    json got = r.channel_raw("payload");
    ASSERT_TRUE(got.is_object());
    EXPECT_EQ(got["k"].get<std::string>(), "v");
}

TEST(RunResultChannel, WrongTypeConversionPropagates) {
    // Channel exists but its value is a string; asking for int via
    // channel<int> should propagate json::type_error from get<int>.
    auto r = make_wrapped("text", json("not a number"));
    EXPECT_THROW(r.channel<int>("text"), json::type_error);
}

TEST(RunResultChannel, WrappedWithoutValueKeyFallsThroughToFlatLookup) {
    // Edge case: channels wrapper exists for the name but has no
    // ["value"] sub-key. Should fall through to flat-key check; if
    // neither, throw.
    RunResult r;
    r.output = json{
        {"channels", {
            {"weird", {{"version", 1}}}      // no "value" key
        }},
        {"weird", json("from-flat")}
    };
    EXPECT_EQ(r.channel<std::string>("weird"), "from-flat");
}
