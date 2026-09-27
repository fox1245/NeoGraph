#include <neograph/tenant.h>

#include <gtest/gtest.h>

using namespace neograph;
using namespace neograph::graph;

TEST(TenantIsolation, StoreAndCheckpointUseIndependentNamespaces) {
    auto store_backend = std::make_shared<InMemoryStore>();
    ScopedStore alice(TenantScope("tenant-a", "auth-a"), store_backend);
    ScopedStore bob(TenantScope("tenant-b", "auth-b"), store_backend);

    alice.put({"profiles"}, "same-session", json{{"owner", "alice"}});
    EXPECT_EQ(alice.get({"profiles"}, "same-session")->value.at("owner"), "alice");
    EXPECT_FALSE(bob.get({"profiles"}, "same-session"));

    auto checkpoint_backend = std::make_shared<InMemoryCheckpointStore>();
    ScopedCheckpointStore alice_checkpoints(TenantScope("tenant-a", "auth-a"),
                                            checkpoint_backend);
    ScopedCheckpointStore bob_checkpoints(TenantScope("tenant-b", "auth-b"),
                                          checkpoint_backend);
    Checkpoint cp;
    cp.id = "same-checkpoint";
    cp.thread_id = "same-thread";
    cp.channel_values = json::object();
    cp.channel_versions = json::object();
    cp.step = 0;
    cp.timestamp = 0;
    alice_checkpoints.save(cp);
    EXPECT_TRUE(alice_checkpoints.load_by_id("same-checkpoint"));
    EXPECT_FALSE(bob_checkpoints.load_by_id("same-checkpoint"));
    EXPECT_FALSE(bob_checkpoints.load_latest("same-thread"));
}

TEST(TenantIsolation, ScopeAuthorizationIsExactAndCredentialFree) {
    TenantScope scope("tenant-a", "scope-a");
    EXPECT_TRUE(scope.authorizes("tenant-a", "scope-a"));
    EXPECT_FALSE(scope.authorizes("tenant-b", "scope-a"));
    EXPECT_FALSE(scope.authorizes("tenant-a", "scope-b"));
    EXPECT_EQ(scope.tenant_id(), "tenant-a");
    EXPECT_EQ(scope.authorization_scope(), "scope-a");
}

TEST(TenantQuota, AdmissionAndRAIIRelease) {
    TenantQuota quota(TenantQuotas{.max_concurrency = 1, .max_artifacts = 1,
                                   .max_model_tokens = 10, .max_cost_microunits = 4});
    {
        auto lease = TenantQuotaLease::acquire(quota, true);
        EXPECT_EQ(quota.active(), 1U);
        EXPECT_EQ(quota.artifacts(), 1U);
        EXPECT_THROW(TenantQuotaLease::acquire(quota), TenantQuotaExceeded);
    }
    EXPECT_EQ(quota.active(), 0U);
    EXPECT_EQ(quota.artifacts(), 0U);
    EXPECT_TRUE(quota.try_reserve_usage(10, 4));
    EXPECT_FALSE(quota.try_reserve_usage(1, 1));
    quota.release_usage(10, 4);
    EXPECT_EQ(quota.model_tokens(), 0U);
    EXPECT_EQ(quota.cost_microunits(), 0U);
}
