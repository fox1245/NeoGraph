#include <neograph/context_store.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace neograph;

namespace {

std::string sha(char digit) { return "sha256:" + std::string(64, digit); }

RuntimeHistoryRecord record(std::string feed_id,
                            std::uint64_t sequence,
                            std::optional<std::string> predecessor = std::nullopt) {
    RuntimeHistoryRecordData data;
    data.feed_id = std::move(feed_id);
    data.sequence = sequence;
    data.message_id = "message_" + std::to_string(sequence);
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = ChatMessage{"user", "message " + std::to_string(sequence)};
    data.predecessor_id = std::move(predecessor);
    return RuntimeHistoryRecord::create(std::move(data));
}

ContextArtifact artifact() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::DerivedContext;
    data.producer_id = "test-producer";
    data.source_digest = sha('a');
    data.media_type = "application/json";
    data.content = json{{"summary", "exact"}};
    return ContextArtifact::create(std::move(data));
}

}  // namespace

TEST(ContextStore, AppendsWithCasAndTreatsExactRetryAsIdempotent) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"tenant_a", "feed_a"};
    const auto first = record(feed.feed_id, 1);
    EXPECT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    EXPECT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::AlreadyPresent);

    const auto second = record(feed.feed_id, 2, first.id());
    EXPECT_EQ(store.append_history(feed, second, sha('b')), ContextStoreAppendResult::Conflict);
    EXPECT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
    const auto third = record(feed.feed_id, 3, second.id());
    EXPECT_EQ(store.append_history(feed, third, second.id()), ContextStoreAppendResult::Appended);
    EXPECT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::AlreadyPresent);
    EXPECT_EQ(store.history_head(feed).sequence, 3u);
    EXPECT_EQ(store.history_head(feed).record_id, third.id());
}

TEST(ContextStore, RejectsNonContiguousAndBrokenPredecessorAppends) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"tenant_a", "feed_a"};
    const auto first = record(feed.feed_id, 1);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    const auto skipped = record(feed.feed_id, 3, first.id());
    EXPECT_EQ(store.append_history(feed, skipped, first.id()), ContextStoreAppendResult::Conflict);

    const auto unrelated = record("other_feed", 1);
    const auto broken = record(feed.feed_id, 2, unrelated.id());
    EXPECT_EQ(store.append_history(feed, broken, first.id()), ContextStoreAppendResult::Conflict);

    const ContextStoreFeed untouched{"tenant_a", "failed_feed"};
    const auto wrong_first = record(untouched.feed_id, 1);
    EXPECT_EQ(store.append_history(untouched, wrong_first, sha('f')),
              ContextStoreAppendResult::Conflict);
    EXPECT_EQ(store.history_head(untouched).sequence, 0u);
    EXPECT_EQ(store.append_history(untouched, wrong_first, std::nullopt),
              ContextStoreAppendResult::Appended);
}

TEST(ContextStore, IsolatesOwnersAndHydratesExactCanonicalJsonl) {
    InMemoryContextStore store;
    const ContextStoreFeed first_owner{"tenant_a", "feed_a"};
    const ContextStoreFeed second_owner{"tenant_b", "feed_a"};
    const auto first = record("feed_a", 1);
    const auto second = record("feed_a", 2, first.id());
    ASSERT_EQ(store.append_history(first_owner, first, std::nullopt), ContextStoreAppendResult::Appended);
    ASSERT_EQ(store.append_history(first_owner, second, first.id()), ContextStoreAppendResult::Appended);
    EXPECT_EQ(store.history_head(second_owner).sequence, 0u);

    const auto snapshot = store.snapshot_history(first_owner, 1, 2);
    EXPECT_EQ(snapshot.feed_id, "feed_a");
    EXPECT_EQ(snapshot.from_sequence, 1u);
    EXPECT_EQ(snapshot.through_sequence, 2u);
    EXPECT_FALSE(snapshot.digest.empty());
    EXPECT_FALSE(snapshot.artifact_uri.empty());
    EXPECT_EQ(snapshot.artifact_uri.find(first_owner.owner_id), std::string::npos);
    EXPECT_EQ(snapshot.artifact_uri.find(snapshot.feed_id), std::string::npos);
    EXPECT_EQ(store.hydrate_history(snapshot),
              first.serialize_canonical() + "\n" + second.serialize_canonical());
    EXPECT_THROW(store.hydrate_history(ContextHistoryRange{second_owner.owner_id, snapshot.feed_id,
                                                            1, 2, snapshot.digest,
                                                            snapshot.artifact_uri}),
                 std::invalid_argument);

    auto tampered = snapshot;
    tampered.digest = sha('f');
    EXPECT_THROW(store.hydrate_history(tampered), std::invalid_argument);
    tampered = snapshot;
    tampered.artifact_uri += "tampered";
    EXPECT_THROW(store.hydrate_history(tampered), std::invalid_argument);
}

TEST(ContextStore, UsesOpaqueUriForValidTokenIdentifiersWithUriDelimiters) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"tenant/with?delimiters#fragment", "feed with space/%"};
    const auto first = record(feed.feed_id, 1);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    const auto snapshot = store.snapshot_history(feed, 1, 1);
    EXPECT_EQ(snapshot.artifact_uri.rfind("neograph://context/history/", 0), 0u);
    EXPECT_EQ(snapshot.artifact_uri.find(feed.owner_id), std::string::npos);
    EXPECT_EQ(snapshot.artifact_uri.find(feed.feed_id), std::string::npos);
    EXPECT_EQ(store.hydrate_history(snapshot), first.serialize_canonical());
}

TEST(ContextStore, StoresArtifactsIdempotentlyAndScopesReadsToOwner) {
    InMemoryContextStore store;
    const auto value = artifact();
    EXPECT_EQ(store.put_artifact("tenant_a", value), ContextArtifactPutResult::Stored);
    EXPECT_EQ(store.put_artifact("tenant_a", value), ContextArtifactPutResult::AlreadyPresent);
    ASSERT_TRUE(store.get_artifact("tenant_a", value.id()).has_value());
    EXPECT_EQ(store.get_artifact("tenant_a", value.id())->serialize_canonical(),
              value.serialize_canonical());
    EXPECT_FALSE(store.get_artifact("tenant_b", value.id()).has_value());
}

TEST(ContextStore, RejectsInputsBeyondContractBounds) {
    InMemoryContextStore store;
    const ContextStoreFeed valid_feed{"tenant_a", "feed_a"};
    const auto first = record(valid_feed.feed_id, 1);
    EXPECT_THROW(store.append_history(
                     {std::string(InMemoryContextStore::MAX_OWNER_ID_BYTES + 1, 'a'), "feed_a"},
                     first, std::nullopt),
                 std::invalid_argument);
    EXPECT_THROW(store.history_head(
                     {"tenant_a", std::string(InMemoryContextStore::MAX_FEED_ID_BYTES + 1, 'a')}),
                 std::invalid_argument);
}

TEST(ContextStore, ConcurrentCasAllowsOneAppendAndPreservesHead) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"tenant_a", "feed_a"};
    const auto first = record(feed.feed_id, 1);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    const auto second = record(feed.feed_id, 2, first.id());
    std::atomic<int> appended{0};
    std::vector<std::thread> workers;
    for (int i = 0; i != 8; ++i) {
        workers.emplace_back([&] {
            if (store.append_history(feed, second, first.id()) == ContextStoreAppendResult::Appended) {
                ++appended;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(appended, 1);
    EXPECT_EQ(store.history_head(feed).record_id, second.id());
}
