// Verifies that InMemoryCheckpointStore deduplicates channel values by
// (thread_id, channel, version). Each test exercises one property the
// dedup contract has to satisfy: storage savings on unchanged channels,
// transparency of the dedup to callers (round-trip identity), and
// correct cleanup on delete_thread / fork.

#include <gtest/gtest.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph::graph;
using json = neograph::json;

namespace {

// Build a Checkpoint whose channel_values matches the shape produced by
// GraphState::serialize() — one "channels" object keyed by name, each
// entry carrying value+version. Tests use this so they exercise the
// same on-the-wire shape the engine actually saves.
Checkpoint make_state_cp(const std::string& thread_id,
                         int step,
                         const std::map<std::string, std::pair<json, uint64_t>>& channels) {
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.step = step;
    cp.timestamp = step * 1000;
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = CheckpointPhase::Completed;

    json cv = json::object();
    json chs = json::object();
    for (const auto& [name, vv] : channels) {
        json entry = json::object();
        entry["value"] = vv.first;
        entry["version"] = vv.second;
        chs[name] = entry;
    }
    cv["channels"] = chs;
    cv["global_version"] = static_cast<uint64_t>(channels.size());
    cp.channel_values = cv;
    return cp;
}

} // namespace

// Three super-steps where only one channel ("counter") changes each
// step. The other channel ("config") stays at version=1 across all
// three. Without dedup that would be 6 stored values; with dedup the
// "config" entry is shared, so we expect 4 distinct blobs (3 counter +
// 1 config).
TEST(IncrementalCheckpoint, UnchangedChannelDedupedAcrossSteps) {
    InMemoryCheckpointStore store;
    json config = json::object();
    config["model"] = "claude";

    store.save(make_state_cp("t", 0, {{"counter", {1, 2}}, {"config", {config, 1}}}));
    store.save(make_state_cp("t", 1, {{"counter", {2, 3}}, {"config", {config, 1}}}));
    store.save(make_state_cp("t", 2, {{"counter", {3, 4}}, {"config", {config, 1}}}));

    EXPECT_EQ(store.size(), 3u);
    EXPECT_EQ(store.blob_count(), 4u)
        << "expected 3 counter blobs + 1 shared config blob";
}

// Saving the same logical state twice (same versions, same values)
// must collapse to exactly one blob per channel. This is the strongest
// dedup guarantee — back-to-back idempotent saves cost no extra storage.
TEST(IncrementalCheckpoint, IdenticalStateAcrossCpsSharesBlobs) {
    InMemoryCheckpointStore store;
    store.save(make_state_cp("t", 0, {{"x", {42, 5}}, {"y", {"hi", 7}}}));
    store.save(make_state_cp("t", 1, {{"x", {42, 5}}, {"y", {"hi", 7}}}));

    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.blob_count(), 2u);
}

// The full inline shape callers passed to save() must come back out of
// load_latest() unchanged. Dedup is a storage detail; the caller-facing
// Checkpoint must look identical to what they wrote.
TEST(IncrementalCheckpoint, RoundTripPreservesInlineShape) {
    InMemoryCheckpointStore store;
    json msgs = json::array();
    json m = json::object();
    m["role"] = "user";
    m["content"] = "hello";
    msgs.push_back(m);

    auto cp = make_state_cp("t", 0,
        {{"messages", {msgs, 3}}, {"counter", {7, 4}}});
    store.save(cp);

    auto loaded = store.load_latest("t");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->channel_values.contains("channels"));
    auto chs = loaded->channel_values["channels"];

    EXPECT_TRUE(chs.contains("messages"));
    EXPECT_TRUE(chs.contains("counter"));
    EXPECT_EQ(chs["counter"]["value"].get<int>(), 7);
    EXPECT_EQ(chs["counter"]["version"].get<uint64_t>(), 4u);
    EXPECT_EQ(chs["messages"]["value"][0]["content"].get<std::string>(), "hello");
}

// load_by_id and load_latest must produce the same materialized cp —
// they take different lookup paths but both must rehydrate from blobs.
TEST(IncrementalCheckpoint, LoadByIdMaterializesValues) {
    InMemoryCheckpointStore store;
    auto cp = make_state_cp("t", 0, {{"x", {99, 1}}});
    store.save(cp);

    auto by_id = store.load_by_id(cp.id);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->channel_values["channels"]["x"]["value"].get<int>(), 99);
}

// list() returns the most-recent N cps; each must be rehydrated, not
// returned as bare shells — older code reading list() relied on inline
// channel_values being present.
TEST(IncrementalCheckpoint, ListMaterializesEachCheckpoint) {
    InMemoryCheckpointStore store;
    store.save(make_state_cp("t", 0, {{"x", {1, 1}}}));
    store.save(make_state_cp("t", 1, {{"x", {2, 2}}}));

    auto cps = store.list("t");
    ASSERT_EQ(cps.size(), 2u);
    // list returns newest first
    EXPECT_EQ(cps[0].channel_values["channels"]["x"]["value"].get<int>(), 2);
    EXPECT_EQ(cps[1].channel_values["channels"]["x"]["value"].get<int>(), 1);
}

// delete_thread must drop the thread's blobs too. Otherwise a
// long-running process accumulates dead blobs from deleted threads —
// the very leak that motivates dedup in the first place.
TEST(IncrementalCheckpoint, DeleteThreadDropsBlobs) {
    InMemoryCheckpointStore store;
    store.save(make_state_cp("ta", 0, {{"x", {1, 1}}, {"y", {2, 2}}}));
    store.save(make_state_cp("tb", 0, {{"x", {3, 1}}, {"y", {4, 2}}}));
    EXPECT_EQ(store.blob_count(), 4u);

    store.delete_thread("ta");
    EXPECT_EQ(store.blob_count(), 2u)
        << "tb's blobs must remain; only ta's must be evicted";

    auto loaded = store.load_latest("tb");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 3);
}

// Forking re-saves the source cp under a new thread_id. The blob layer
// re-keys by the new thread_id, so the fork's blobs are independent of
// the source's — deleting the source thread must not corrupt the fork.
TEST(IncrementalCheckpoint, ForkBlobsIndependentOfSource) {
    InMemoryCheckpointStore store;
    auto src_cp = make_state_cp("src", 0, {{"x", {123, 1}}});
    store.save(src_cp);

    // Simulate fork: load source cp (now fully inline), reassign thread_id,
    // save under new thread. This is what GraphEngine::fork does.
    auto loaded = store.load_by_id(src_cp.id);
    ASSERT_TRUE(loaded.has_value());
    Checkpoint forked = *loaded;
    forked.id = Checkpoint::generate_id();
    forked.thread_id = "fork";
    store.save(forked);

    // Now blobs exist under both ("src", "x", 1) and ("fork", "x", 1).
    EXPECT_EQ(store.blob_count(), 2u);

    // Deleting the source must not corrupt the fork.
    store.delete_thread("src");
    auto fork_loaded = store.load_latest("fork");
    ASSERT_TRUE(fork_loaded.has_value());
    EXPECT_EQ(fork_loaded->channel_values["channels"]["x"]["value"].get<int>(), 123);
}

// Channels carrying inline value+version that the caller passes in
// without dedup-friendly shape (e.g. legacy v1/v2 blobs reconstructed
// by a migration tool) must still round-trip — the join path falls
// through to the inline value when no blob is present.
TEST(IncrementalCheckpoint, LegacyInlineShapeStillRoundTrips) {
    InMemoryCheckpointStore store;
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = "legacy";
    cp.step = 0;
    cp.timestamp = 1;
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = CheckpointPhase::Completed;

    // Caller hands us a cp with channel_values shaped like serialize()
    // output. The dedup splits it on save; on load the value is read
    // back from the blob. This is the same code path as fresh cps — no
    // legacy branch in the store. The "legacy" framing is conceptual:
    // any caller whose data already carries explicit version+value
    // (whether freshly written or migrated) goes through this path.
    cp.channel_values = make_state_cp("legacy", 0, {{"x", {7, 1}}}).channel_values;
    store.save(cp);

    auto loaded = store.load_latest("legacy");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->channel_values["channels"]["x"]["value"].get<int>(), 7);
}
