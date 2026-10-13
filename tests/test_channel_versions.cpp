// Channel versions: one write counter shared by all channels (#416).
//
// A channel's `version` is the value of the state's single write counter at the
// channel's last write, and `global_version` is the counter after the last
// write. They order writes; they are not a count of one channel's writes and not
// a super-step number. The run input's writes count. A checkpoint stores both
// numbers, and a resumed or forked run goes on counting from them.
//
// The graph is the one of examples/104_channels_reducers.cpp: three nodes in
// turn, each writing `last` (overwrite) and `all` (append).

#include <gtest/gtest.h>

#include <neograph/neograph.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Writes its own name to `last` (overwrite) and appends it to `all` (append).
class VersionsWriter : public SyncGraphNode {
public:
    explicit VersionsWriter(std::string name) : SyncGraphNode(std::move(name)) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"last", json(get_name())},
                           ChannelWrite{"all", json::array({get_name()})}}};
    }
};

// Reads the counters a node sees and writes them to `seen`.
class VersionsProbe : public SyncGraphNode {
public:
    explicit VersionsProbe(std::string name) : SyncGraphNode(std::move(name)) {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        json seen = json::array({in.state.channel_version("last"), in.state.channel_version("all"),
                                 in.state.global_version(), in.state.channel_version("seen")});
        return NodeOutput{{ChannelWrite{"seen", std::move(seen)}}};
    }
};

void register_types() {
    static std::once_flag once;
    std::call_once(once, [] {
        NodeFactory::instance().register_type(
            "versions_writer", [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<VersionsWriter>(name);
            });
        NodeFactory::instance().register_type(
            "versions_probe", [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<VersionsProbe>(name);
            });
    });
}

json chain(const std::vector<std::pair<std::string, std::string>>& nodes, json channels) {
    json node_map = json::object();
    json edges    = json::array();
    std::string previous = "__start__";
    for (const auto& [name, type] : nodes) {
        node_map[name] = json{{"type", type}};
        edges.push_back(json{{"from", previous}, {"to", name}});
        previous = name;
    }
    edges.push_back(json{{"from", previous}, {"to", "__end__"}});
    return json{{"schema_version", TOPOLOGY_SCHEMA_VERSION},
                {"name", "channel_versions"},
                {"channels", std::move(channels)},
                {"nodes", std::move(node_map)},
                {"edges", std::move(edges)}};
}

json three_writers(const std::vector<std::string>& interrupt_before = {}) {
    json definition = chain({{"first", "versions_writer"},
                             {"second", "versions_writer"},
                             {"third", "versions_writer"}},
                            json{{"last", {{"reducer", "overwrite"}}},
                                 {"all", {{"reducer", "append"}}},
                                 {"budget", {{"reducer", "overwrite"}, {"initial", 100}}}});
    if (!interrupt_before.empty()) definition["interrupt_before"] = interrupt_before;
    return definition;
}

std::uint64_t version_of(const json& output, const std::string& channel) {
    return output["channels"][channel]["version"].get<std::uint64_t>();
}

std::uint64_t global_of(const json& output) { return output["global_version"].get<std::uint64_t>(); }

}  // namespace

TEST(ChannelVersions, ThreeWritersLeaveOneCounterSharedByEveryChannel) {
    register_types();
    auto engine = GraphEngine::build(three_writers(), EngineConfig{});
    const auto result = engine->run(RunConfig{});

    // Six writes in order: last 1, all 2, last 3, all 4, last 5, all 6.
    EXPECT_EQ(version_of(result.output, "last"), 5u);
    EXPECT_EQ(version_of(result.output, "all"), 6u);
    EXPECT_EQ(global_of(result.output), 6u);
    EXPECT_EQ(result.channel_raw("all"), json::array({"first", "second", "third"}));
    // `last` was written three times, but its version is not 3.
    EXPECT_EQ(result.channel_raw("last"), json("third"));
    // A channel nobody wrote keeps version 0, also with an initial value.
    EXPECT_EQ(version_of(result.output, "budget"), 0u);
    EXPECT_EQ(result.channel_raw("budget"), json(100));
}

TEST(ChannelVersions, ANodeSeesTheSharedCounterNotASumOfVersions) {
    register_types();
    json definition = chain({{"first", "versions_writer"}, {"second", "versions_writer"}, {"probe", "versions_probe"}},
                            json{{"last", {{"reducer", "overwrite"}}},
                                 {"all", {{"reducer", "append"}}},
                                 {"seen", {{"reducer", "overwrite"}}}});
    auto engine = GraphEngine::build(definition, EngineConfig{});
    const auto result = engine->run(RunConfig{});

    // The probe runs after four writes (last 3, all 4): the counter is 4, not
    // the sum of the channels' versions (7). `seen` was never written yet.
    EXPECT_EQ(result.channel_raw("seen"), json::array({3, 4, 4, 0}));
    EXPECT_EQ(version_of(result.output, "seen"), 5u);
    EXPECT_EQ(global_of(result.output), 5u);
}

TEST(ChannelVersions, TheRunInputCountsAsWritesAndUnknownKeysDoNot) {
    register_types();
    auto engine = GraphEngine::build(three_writers(), EngineConfig{});
    RunConfig config;
    config.input = json{{"last", "seed"}, {"not_a_channel", 1}};
    const auto result = engine->run(config);

    // The input is one write (counter 1); the nodes' six writes follow.
    EXPECT_EQ(version_of(result.output, "last"), 6u);
    EXPECT_EQ(version_of(result.output, "all"), 7u);
    EXPECT_EQ(global_of(result.output), 7u);
    EXPECT_FALSE(result.output["channels"].contains("not_a_channel"));
}

TEST(ChannelVersions, AWriteThatLeavesTheValueUnchangedStillCounts) {
    GraphState state;
    state.init_channel("a", ReducerType::OVERWRITE, ReducerRegistry::instance().get("overwrite"), json(0));
    state.init_channel("b", ReducerType::OVERWRITE, ReducerRegistry::instance().get("overwrite"), json(0));
    EXPECT_EQ(state.global_version(), 0u);
    EXPECT_EQ(state.channel_version("a"), 0u);
    EXPECT_EQ(state.channel_version("missing"), 0u);

    state.write("a", json(1));
    state.write("a", json(1));
    EXPECT_EQ(state.channel_version("a"), 2u);
    state.apply_writes({ChannelWrite{"b", json(1)}, ChannelWrite{"a", json(1)}});
    EXPECT_EQ(state.channel_version("b"), 3u);
    EXPECT_EQ(state.channel_version("a"), 4u);
    EXPECT_EQ(state.global_version(), 4u);
}

TEST(ChannelVersions, VersionsRiseAcrossCheckpointsAndNoTwoChannelsShareOne) {
    register_types();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(three_writers(), NodeContext{}, store);
    RunConfig config;
    config.thread_id = "versions-history";
    (void)engine->run(config);

    auto history = store->list("versions-history", 100);
    std::sort(history.begin(), history.end(), [](const Checkpoint& l, const Checkpoint& r) { return l.step < r.step; });
    ASSERT_GE(history.size(), 3u);

    std::uint64_t previous_global = 0;
    std::map<std::string, std::uint64_t> previous_version;
    for (const auto& checkpoint : history) {
        const auto& values = checkpoint.channel_values;
        const auto global  = values["global_version"].get<std::uint64_t>();
        EXPECT_GE(global, previous_global) << "step " << checkpoint.step;
        std::set<std::uint64_t> seen;
        for (const std::string name : {"last", "all"}) {
            const auto version = values["channels"][name]["version"].get<std::uint64_t>();
            EXPECT_GE(version, previous_version[name]) << name << " at step " << checkpoint.step;
            EXPECT_LE(version, global);
            if (version != 0) EXPECT_TRUE(seen.insert(version).second) << "two channels share version " << version;
            previous_version[name] = version;
        }
        previous_global = global;
    }
    EXPECT_EQ(previous_global, 6u);
    EXPECT_EQ(previous_version["last"], 5u);
    EXPECT_EQ(previous_version["all"], 6u);
    // The engine keeps the numbers in channel_values; it never sets channel_versions.
    for (const auto& checkpoint : history) EXPECT_TRUE(checkpoint.channel_versions.is_null()) << "step " << checkpoint.step;
}

TEST(ChannelVersions, AResumedRunGoesOnCountingFromTheCheckpoint) {
    register_types();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(three_writers({"third"}), NodeContext{}, store);
    RunConfig config;
    config.thread_id = "versions-resume";

    const auto paused = engine->run(config);
    ASSERT_TRUE(paused.interrupted);
    // `first` and `second` ran: four writes.
    EXPECT_EQ(version_of(paused.output, "last"), 3u);
    EXPECT_EQ(version_of(paused.output, "all"), 4u);
    EXPECT_EQ(global_of(paused.output), 4u);
    const auto checkpoint = store->load_latest("versions-resume");
    ASSERT_TRUE(checkpoint.has_value());
    EXPECT_EQ(checkpoint->channel_values["global_version"].get<std::uint64_t>(), 4u);
    EXPECT_EQ(checkpoint->channel_values["channels"]["last"]["version"].get<std::uint64_t>(), 3u);

    // The resumed run restores them and counts on: the numbers are those of a
    // run that never paused.
    const auto resumed = engine->resume("versions-resume");
    ASSERT_FALSE(resumed.interrupted);
    EXPECT_EQ(version_of(resumed.output, "last"), 5u);
    EXPECT_EQ(version_of(resumed.output, "all"), 6u);
    EXPECT_EQ(global_of(resumed.output), 6u);
    EXPECT_EQ(version_of(resumed.output, "budget"), 0u);
}

TEST(ChannelVersions, AForkedRunGoesOnCountingFromTheCheckpoint) {
    register_types();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(three_writers({"third"}), NodeContext{}, store);
    RunConfig config;
    config.thread_id = "versions-source";
    ASSERT_TRUE(engine->run(config).interrupted);

    (void)engine->fork("versions-source", "versions-branch");
    const auto branch = engine->resume("versions-branch");
    ASSERT_FALSE(branch.interrupted);
    EXPECT_EQ(version_of(branch.output, "last"), 5u);
    EXPECT_EQ(version_of(branch.output, "all"), 6u);
    EXPECT_EQ(global_of(branch.output), 6u);

    // The source is untouched and still resumes to the same numbers.
    const auto source = store->load_latest("versions-source");
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->channel_values["global_version"].get<std::uint64_t>(), 4u);
    const auto resumed = engine->resume("versions-source");
    EXPECT_EQ(global_of(resumed.output), 6u);
}

TEST(ChannelVersions, ANewInputOnAResumedThreadCountsAfterTheCheckpoint) {
    register_types();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(three_writers(), NodeContext{}, store);
    RunConfig config;
    config.thread_id = "versions-continue";
    const auto first = engine->run(config);
    ASSERT_EQ(global_of(first.output), 6u);

    // resume_if_exists restores the counter (6) before it applies the input,
    // so the input is write 7 and the second pass's six writes are 8 to 13.
    config.resume_if_exists = true;
    config.input            = json{{"last", "again"}};
    const auto second       = engine->run(config);
    EXPECT_EQ(version_of(second.output, "last"), 12u);
    EXPECT_EQ(version_of(second.output, "all"), 13u);
    EXPECT_EQ(global_of(second.output), 13u);
}
