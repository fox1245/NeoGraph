#include <neograph/mcp/tenant.h>
#include <neograph/mcp/sqlite_harness_store.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using neograph::json;

namespace {

std::filesystem::path unique_temp_path(const std::string& stem) {
    return std::filesystem::temp_directory_path() /
           (stem + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

class TempDirectoryCleanup {
public:
    explicit TempDirectoryCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

private:
    std::filesystem::path path_;
};

json tenant_artifact(const std::string& owner) {
    return {{"artifact_id", "artifact_same"},
            {"request", {{"task", owner}, {"artifact_id", "user-data-not-an-owned-id"}}}};
}

json tenant_run(const std::string& owner, bool derived = false) {
    json record = {{"run_id", derived ? "run_derived" : "run_same"},
                   {"artifact_id", "artifact_same"},
                   {"revision_digest", "revision"},
                   {"protocol_version", "protocol"},
                   {"profile", "profile"},
                   {"status", "completed"},
                   {"result", {{"owner", owner}, {"run_id", "user-data-not-an-owned-id"}}}};
    if (derived) {
        record["source_run_id"] = "run_same";
        record["source_checkpoint_id"] = "checkpoint_same";
    }
    return record;
}

void write_tenant_records(neograph::mcp::ScopedHarnessStore& store, const std::string& owner) {
    store.save_artifact("artifact_same", tenant_artifact(owner));
    store.save_run("run_same", tenant_run(owner));
    store.save_run("run_derived", tenant_run(owner, true));
}

void expect_tenant_records(neograph::mcp::ScopedHarnessStore& store, const std::string& owner) {
    EXPECT_EQ(store.load_artifact("artifact_same"), std::optional<json>(tenant_artifact(owner)));
    EXPECT_EQ(store.load_run("run_same"), std::optional<json>(tenant_run(owner)));
    EXPECT_EQ(store.load_run("run_derived"), std::optional<json>(tenant_run(owner, true)));
}

}  // namespace

TEST(HarnessTenantPersistence, FileRoundTripsSamePublicIdsAndReferences) {
    const auto root = unique_temp_path("neograph-tenant-file");
    TempDirectoryCleanup cleanup(root);
    const neograph::mcp::TenantScope alice_scope("tenant/a:_", "alice");
    const neograph::mcp::TenantScope bob_scope("tenant/a:__", "bob");
    {
        auto backend = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
        neograph::mcp::ScopedHarnessStore alice(alice_scope, backend);
        neograph::mcp::ScopedHarnessStore bob(bob_scope, backend);
        ASSERT_NO_THROW(write_tenant_records(alice, "alice"));
        ASSERT_NO_THROW(write_tenant_records(bob, "bob"));
    }
    auto backend = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    neograph::mcp::ScopedHarnessStore alice(alice_scope, backend);
    neograph::mcp::ScopedHarnessStore bob(bob_scope, backend);
    expect_tenant_records(alice, "alice");
    expect_tenant_records(bob, "bob");
    const auto private_run = backend->load_run(
        neograph::mcp::ScopedHarnessStore::private_id(alice_scope, "run_derived"));
    ASSERT_TRUE(private_run);
    EXPECT_EQ(private_run->at("run_id"),
              neograph::mcp::ScopedHarnessStore::private_id(alice_scope, "run_derived"));
    EXPECT_EQ(private_run->at("artifact_id"),
              neograph::mcp::ScopedHarnessStore::private_id(alice_scope, "artifact_same"));
    EXPECT_EQ(private_run->at("source_run_id"),
              neograph::mcp::ScopedHarnessStore::private_id(alice_scope, "run_same"));
}

TEST(HarnessTenantPersistence, FileReopensLongIdsAndCaseDistinctTenants) {
    const auto root = unique_temp_path("neograph-tenant-long-file");
    TempDirectoryCleanup cleanup(root);
    const neograph::mcp::TenantScope upper("Tenant", "upper");
    const neograph::mcp::TenantScope lower("tenant", "lower");
    const auto digest = [](char value) { return "sha256-" + std::string(64, value); };
    const auto artifact_id = "artifact-" + digest('1') + "-" + digest('2') + "-" + digest('3');
    auto artifact_upper = tenant_artifact("upper");
    artifact_upper["artifact_id"] = artifact_id;
    auto artifact_lower = tenant_artifact("lower");
    artifact_lower["artifact_id"] = artifact_id;
    auto run_upper = tenant_run("upper");
    run_upper["artifact_id"] = artifact_id;
    auto run_lower = tenant_run("lower");
    run_lower["artifact_id"] = artifact_id;
    {
        auto backend = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
        neograph::mcp::ScopedHarnessStore alice(upper, backend);
        neograph::mcp::ScopedHarnessStore bob(lower, backend);
        alice.save_artifact(artifact_id, artifact_upper);
        bob.save_artifact(artifact_id, artifact_lower);
        alice.save_run("run_same", run_upper);
        bob.save_run("run_same", run_lower);
    }
    auto backend = std::make_shared<neograph::mcp::FileHarnessRecordStore>(root.string());
    neograph::mcp::ScopedHarnessStore alice(upper, backend);
    neograph::mcp::ScopedHarnessStore bob(lower, backend);
    EXPECT_EQ(alice.load_artifact(artifact_id), std::optional<json>(artifact_upper));
    EXPECT_EQ(bob.load_artifact(artifact_id), std::optional<json>(artifact_lower));
    EXPECT_EQ(alice.load_run("run_same"), std::optional<json>(run_upper));
    EXPECT_EQ(bob.load_run("run_same"), std::optional<json>(run_lower));
}

TEST(HarnessTenantPersistence, SqliteRoundTripsJournalAndRetainsOnlySelectedTenant) {
    const auto root = unique_temp_path("neograph-tenant-sqlite");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    const auto database = (root / "runs.db").string();
    const neograph::mcp::TenantScope alice_scope("tenant/a:_", "alice");
    const neograph::mcp::TenantScope bob_scope("tenant/a:__", "bob");
    const json payload = {{"result", {{"owner", "alice"}, {"run_id", "opaque-user-id"}}}};
    {
        neograph::mcp::SqliteHarnessJournalConfig journal;
        journal.mode = neograph::mcp::HarnessJournalPayloadMode::FULL;
        auto backend = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(database, 5s, journal);
        neograph::mcp::ScopedHarnessStore alice(alice_scope, backend, backend, backend);
        neograph::mcp::ScopedHarnessStore bob(bob_scope, backend, backend, backend);
        ASSERT_NO_THROW(write_tenant_records(alice, "alice"));
        ASSERT_NO_THROW(write_tenant_records(bob, "bob"));
        auto event = json::object();
        for (const auto& [key, value] : tenant_run("alice", true).items()) {
            if (key != "result") event[key] = value;
        }
        event["event_type"] = "worker.completed";
        event["payload"] = payload;
        alice.append_event(event);
        event["payload"] = {{"result", {{"owner", "bob"}}}};
        bob.append_event(event);
    }
    auto backend = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(database);
    neograph::mcp::ScopedHarnessStore alice(alice_scope, backend, backend, backend);
    neograph::mcp::ScopedHarnessStore bob(bob_scope, backend, backend, backend);
    expect_tenant_records(alice, "alice");
    expect_tenant_records(bob, "bob");
    const auto events = alice.list_events("run_derived");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].at("run_id"), "run_derived");
    EXPECT_EQ(events[0].at("artifact_id"), "artifact_same");
    EXPECT_EQ(events[0].at("payload"), payload);
    EXPECT_EQ(bob.list_events("run_derived")[0].at("payload").at("result").at("owner"), "bob");

    neograph::mcp::HarnessRetentionPolicy policy;
    policy.max_runs = 2;
    policy.max_artifacts = 1;
    const auto within_limit = alice.cleanup_retained(policy);
    EXPECT_TRUE(within_limit.run_ids.empty());
    EXPECT_TRUE(within_limit.artifact_ids.empty());
    expect_tenant_records(alice, "alice");
    expect_tenant_records(bob, "bob");
    policy.max_runs = 0;
    policy.max_artifacts = 0;
    policy.protected_run_ids = {"run_derived"};
    auto retained = alice.cleanup_retained(policy);
    EXPECT_TRUE(retained.run_ids.empty());
    EXPECT_TRUE(retained.artifact_ids.empty());
    expect_tenant_records(alice, "alice");
    expect_tenant_records(bob, "bob");
    policy.protected_run_ids.clear();
    policy.protected_artifact_ids = {"artifact_same"};
    auto removed = alice.cleanup_retained(policy);
    EXPECT_EQ(removed.run_ids, (std::vector<std::string>{"run_derived", "run_same"}));
    EXPECT_TRUE(removed.artifact_ids.empty());
    EXPECT_EQ(alice.load_artifact("artifact_same"),
              std::optional<json>(tenant_artifact("alice")));
    policy.protected_artifact_ids.clear();
    const auto removed_artifact = alice.cleanup_retained(policy);
    EXPECT_TRUE(removed_artifact.run_ids.empty());
    EXPECT_EQ(removed_artifact.artifact_ids, (std::vector<std::string>{"artifact_same"}));
    EXPECT_FALSE(alice.load_run("run_same"));
    EXPECT_FALSE(alice.load_run("run_derived"));
    EXPECT_FALSE(alice.load_artifact("artifact_same"));
    EXPECT_TRUE(alice.list_events("run_derived").empty());
    expect_tenant_records(bob, "bob");
    ASSERT_EQ(bob.list_events("run_derived").size(), 1u);
    EXPECT_EQ(bob.list_events("run_derived")[0].at("payload").at("result").at("owner"), "bob");
}

TEST(HarnessTenantPersistence, EmptyTenantRetentionCannotDeleteUnscopedRecords) {
    const auto root = unique_temp_path("neograph-tenant-empty-retention");
    TempDirectoryCleanup cleanup(root);
    std::filesystem::create_directories(root);
    auto backend = std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(
        (root / "runs.db").string());
    const auto artifact = tenant_artifact("unscoped");
    const auto run = tenant_run("unscoped");
    backend->save_artifact("artifact_same", artifact);
    backend->save_run("run_same", run);
    neograph::mcp::ScopedHarnessStore alice(
        neograph::mcp::TenantScope("alice", "alice"), backend, backend, backend);
    neograph::mcp::HarnessRetentionPolicy policy;
    policy.max_runs = 0;
    policy.max_artifacts = 0;
    const auto removed = alice.cleanup_retained(policy);
    EXPECT_TRUE(removed.run_ids.empty());
    EXPECT_TRUE(removed.artifact_ids.empty());
    EXPECT_EQ(backend->load_run("run_same"), std::optional<json>(run));
    EXPECT_EQ(backend->load_artifact("artifact_same"), std::optional<json>(artifact));
}
