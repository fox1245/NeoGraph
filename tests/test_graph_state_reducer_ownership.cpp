// A channel owns the value it stores. A custom reducer may legally return a
// view into one of its arguments (for example `incoming["payload"]`), a handle
// that shares the argument's document. The state must detach it, otherwise the
// caller's later edits to its own value would silently change the channel
// without a write, a lock or a version bump.
#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

void add_payload_view_channel(GraphState& state) {
    state.init_channel(
        "x", ReducerType::CUSTOM,
        [](const json&, const json& incoming) { return incoming["payload"]; }, json());
}

json input_value() { return json{{"payload", json{{"field", 1}}}}; }

}  // namespace

TEST(GraphStateReducerOwnership, WriteDoesNotAliasAViewTheReducerReturns) {
    GraphState state;
    add_payload_view_channel(state);
    json input = input_value();
    state.write("x", input);

    input["payload"]["field"] = 2;  // the caller reuses its own value afterwards

    EXPECT_EQ(state.get("x")["field"].get<int>(), 1);
}

TEST(GraphStateReducerOwnership, ApplyWritesDoesNotAliasAViewTheReducerReturns) {
    GraphState state;
    add_payload_view_channel(state);
    std::vector<ChannelWrite> writes{ChannelWrite{"x", input_value()}};
    state.apply_writes(writes);

    writes.front().value["payload"]["field"] = 2;  // the write is reused after it was applied

    EXPECT_EQ(state.get("x")["field"].get<int>(), 1);
}
