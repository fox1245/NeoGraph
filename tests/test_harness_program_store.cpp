#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>
#include <neograph/mcp/sqlite_harness_store.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/program/registry.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <barrier>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace neograph;
using namespace neograph::mcp;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

struct TempDb {
    TempDb() {
        path = std::filesystem::temp_directory_path() /
               ("neograph-harness-program-" + graph::Checkpoint::generate_id() + ".sqlite");
    }
    ~TempDb() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path;
};

void sqlite_exec(const std::filesystem::path& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "exec failed";
        sqlite3_free(error);
        sqlite3_close(db);
        throw std::runtime_error(message);
    }
    sqlite3_close(db);
}

RegistrySnapshot registry() {
    RegistrySnapshotBuilder builder;
    builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "persist-reducer", "1.0.0", digest('8')},
                           EffectMode::Brokered,
                           "attestation:test",
                           {},
                           {}},
        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

ProgramBundle bundle(const RegistrySnapshot& registry) {
    const json definition = {{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                             {"nodes", json{{"main", json{{"type", "persist-node"}}}}}};
    ProgramBundleData data;
    data.source_kind                   = SourceKind::CanonicalJson;
    data.source_hash                   = digest('1');
    data.canonical_program_hash        = digest('2');
    data.compiler_build_id             = "persist-compiler/v1";
    data.program_schema_version        = 1;
    data.registry_snapshot_fingerprint = registry.fingerprint();
    data.module_dependency_merkle_root = digest('3');
    data.input_contract                = {1, json{{"type", "object"}}};
    data.output_contract               = {1, json{{"type", "object"}}};
    data.orchestration_plan            = {1, json{{"entry", "main"}}};
    data.sealed_core_definitions = {{"main", sealed_core_definition_hash(definition), definition}};
    data.core_plan_identities    = {{"main", digest('4')}};
    data.executable_registry_identities = registry.identities();
    data.declared_budget_requirements   = {{"steps", 0, 10}};
    return ProgramBundle(std::move(data));
}

AdmissionProfile profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("persist-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    return std::move(builder).build();
}

PolicySnapshot policy(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("persist-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant-persist")
        .admission_profile(profile)
        .budget_ceiling({1000, 100, 100, 1, 10, 100, 1, 1, 1});
    return std::move(builder).build();
}

struct Fixture {
    RegistrySnapshot registry_value = registry();
    ProgramBundle    bundle_value   = bundle(registry_value);
    AdmissionProfile profile_value  = profile(registry_value);
    PolicySnapshot   policy_value   = policy(profile_value);
    ProgramVersion   version_value  = ProgramVersion(
        ProgramVersionData(bundle_value.id(),
                              profile_value,
                              policy_value,
                              {},
                              policy_value.owner_scope(),
                              CoreMaterializationReceipt{"persist-compiler/v1",
                                                      registry_value.fingerprint(),
                                                      bundle_value.core_plan_identities(),
                                                         {}}));
    HarnessProgramArtifactRecord artifact =
        HarnessProgramArtifactRecord::create("artifact-one",
                                             policy_value.owner_scope(),
                                             bundle_value,
                                             version_value,
                                             json{{"transport", "mcp"}},
                                             json{{"status", "queued"}});
};

RunBudget budget() {
    return {1000, 100, 100, 1, 10, 100, 1, 1, 1};
}

ProgramEvent event(const Fixture&      fixture,
                   std::string         run_id,
                   std::uint64_t       sequence,
                   ProgramEventKind    kind,
                   ProgramEventPayload payload,
                   std::int64_t        timestamp) {
    ProgramEvent value;
    value.sequence           = sequence;
    value.timestamp_ms       = timestamp;
    value.run_id             = std::move(run_id);
    value.program_version_id = fixture.version_value.id();
    value.bundle_id          = fixture.bundle_value.id();
    value.operation_id       = "root";
    value.core_generation_id = digest('5');
    value.core_run_id        = "core-run";
    value.trace_id           = "trace-one";
    value.attempt            = 1;
    value.kind               = kind;
    value.payload            = std::move(payload);
    return ProgramEvent::create(std::move(value));
}

ProgramTransitionPublication initial_publication(const Fixture& fixture,
                                                 std::string    run_id = "run-one") {
    auto                 journal = ProgramJournalRecord::create({{},
                                                                 run_id,
                                                                 fixture.version_value.id(),
                                                                 fixture.bundle_value.id(),
                                                                 1,
                                                                 {"root", ContinuationState::Running, 1},
                                                                 budget(),
                                                                 budget(),
                                                                 std::nullopt,
                                                                 10});
    ProgramRunRecordData data;
    data.owner_scope         = fixture.policy_value.owner_scope();
    data.run_id              = run_id;
    data.program_version_id  = fixture.version_value.id();
    data.bundle_id           = fixture.bundle_value.id();
    data.binding_fingerprint = digest('6');
    data.invocation          = {json{{"request", "hello"}}, budget(), "trace-one"};
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.journal_head        = journal.id;
    data.event_sequence      = 1;
    data.created_at_ms       = 10;
    data.updated_at_ms       = 10;
    return {
        ProgramRunRecord::create(std::move(data)),
        journal,
        {event(fixture, run_id, 1, ProgramEventKind::Started, ProgramStartedEvent{budget()}, 10)},
        {}};
}

ProgramTransitionPublication terminal_publication(const Fixture&                      fixture,
                                                  const ProgramTransitionPublication& initial,
                                                  ProgramTerminalStatus               status,
                                                  ContinuationState                   state,
                                                  std::int64_t                        timestamp) {
    const CoreCheckpointIdentity checkpoint{"main", digest('5'), "core-thread", "checkpoint-one",
                                            graph::CHECKPOINT_SCHEMA_VERSION};
    auto                         journal = ProgramJournalRecord::create({initial.journal_record.id,
                                                                         initial.run_record.run_id(),
                                                                         fixture.version_value.id(),
                                                                         fixture.bundle_value.id(),
                                                                         2,
                                                                         {"root", state, 1},
                                                                         budget(),
                                                                         {},
                                                                         checkpoint,
                                                                         timestamp});
    ProgramResultData            result_data;
    result_data.status             = status;
    result_data.run_id             = initial.run_record.run_id();
    result_data.program_version_id = fixture.version_value.id();
    result_data.bundle_id          = fixture.bundle_value.id();
    result_data.attempt            = 1;
    result_data.output             = json{{"winner", std::string(to_string(status))}};
    result_data.remaining_budget   = budget();
    result_data.checkpoint         = checkpoint;
    if (status == ProgramTerminalStatus::Failed) {
        result_data.failure = ProgramFailure{"P_TEST", "failed", "root", "main", 1, json::object()};
    }

    ProgramRunRecordData data;
    data.owner_scope         = fixture.policy_value.owner_scope();
    data.run_id              = initial.run_record.run_id();
    data.program_version_id  = fixture.version_value.id();
    data.bundle_id           = fixture.bundle_value.id();
    data.binding_fingerprint = initial.run_record.binding_fingerprint();
    data.invocation          = initial.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = checkpoint;
    data.terminal_result     = ProgramResult::create(std::move(result_data));
    data.journal_head        = journal.id;
    data.event_sequence      = 2;
    data.created_at_ms       = initial.run_record.created_at_ms();
    data.updated_at_ms       = timestamp;
    return {ProgramRunRecord::create(std::move(data)),
            journal,
            {event(fixture, initial.run_record.run_id(), 2, ProgramEventKind::Terminal,
                   ProgramTerminalEvent{status}, timestamp)},
            {}};
}

std::shared_ptr<program::ProgramTransitionStore> persist_and_bind(
    const std::shared_ptr<SqliteHarnessRecordStore>& store, const Fixture& fixture) {
    HarnessBoundedProgramStore bounded(
        store, fixture.artifact.artifact_id(), fixture.artifact.owner_scope(),
        fixture.artifact.legacy_invocation(), fixture.artifact.legacy_projection());
    bounded.publish_admitted(fixture.bundle_value, fixture.version_value);
    return require_harness_program_adapter_store(store)->bind_program_transitions(fixture.artifact);
}

json without_field(const json& value, std::string_view omitted) {
    json result = json::object();
    for (const auto& [name, item] : value.items()) {
        if (name != omitted) result[name] = item;
    }
    return result;
}

}  // namespace

TEST(HarnessProgramStoreTest, StrictRecordsRejectMissingAndTamperedIdentityFields) {
    Fixture    fixture;
    const auto artifact_bytes = fixture.artifact.serialize();
    EXPECT_EQ(HarnessProgramArtifactRecord::parse(artifact_bytes).serialize(), artifact_bytes);

    auto missing = without_field(artifact_bytes, "compiler_build_id");
    EXPECT_THROW((void)HarnessProgramArtifactRecord::parse(missing), std::invalid_argument);

    auto tampered         = artifact_bytes;
    tampered["bundle_id"] = digest('9');
    EXPECT_THROW((void)HarnessProgramArtifactRecord::parse(tampered), std::invalid_argument);

    auto initial     = initial_publication(fixture);
    auto wrapped     = HarnessProgramRunRecord::create(fixture.artifact, initial.run_record,
                                                       json{{"status", "running"}});
    auto missing_run = without_field(wrapped.serialize(), "legacy_invocation");
    EXPECT_THROW((void)HarnessProgramRunRecord::parse(missing_run), std::invalid_argument);
}

TEST(HarnessProgramStoreTest, FileStoreRejectsDurableReconnectCapability) {
    TempDb directory;
    auto   file = std::make_shared<FileHarnessRecordStore>(directory.path.string() + "-files");
    EXPECT_THROW((void)require_harness_program_adapter_store(file), std::invalid_argument);
    file.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory.path.string() + "-files", ignored);
}

TEST(HarnessProgramStoreTest, SqliteReopensExactOwnerBoundRunAndLegacyRowsStillWork) {
    TempDb  db;
    Fixture fixture;
    auto    initial = initial_publication(fixture);
    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        EXPECT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::AlreadyPresent);

        store->save_artifact("legacy-artifact", json{{"artifact_id", "legacy-artifact"}});
        const json legacy_run{{"run_id", "legacy-run"},        {"artifact_id", "legacy-artifact"},
                              {"revision_digest", "revision"}, {"protocol_version", "v1"},
                              {"profile", "default"},          {"status", "completed"}};
        store->save_run("legacy-run", legacy_run);
        store->append_event(json{{"run_id", "legacy-run"},
                                 {"artifact_id", "legacy-artifact"},
                                 {"revision_digest", "revision"},
                                 {"protocol_version", "v1"},
                                 {"profile", "default"},
                                 {"event_type", "completed"},
                                 {"payload", json{{"ok", true}}}});
    }
    {
        auto       store      = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto       capability = require_harness_program_adapter_store(store);
        const auto resolved   = capability->resolve_program_run(fixture.artifact.owner_scope(),
                                                                initial.run_record.run_id());
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->run_record().serialize_canonical(),
                  initial.run_record.serialize_canonical());
        EXPECT_FALSE(capability->resolve_program_run("other-owner", "run-one"));

        HarnessBoundedProgramStore bounded(
            store, fixture.artifact.artifact_id(), fixture.artifact.owner_scope(),
            fixture.artifact.legacy_invocation(), fixture.artifact.legacy_projection());
        const auto artifact = bounded.load_artifact();
        ASSERT_TRUE(artifact);
        auto transitions = capability->bind_program_transitions(*artifact);
        ASSERT_TRUE(transitions->load(fixture.artifact.owner_scope(), "run-one"));
        ASSERT_TRUE(transitions->latest(fixture.artifact.owner_scope(), "run-one"));
        EXPECT_EQ(transitions->load_events(fixture.artifact.owner_scope(), "run-one").size(), 1U);
        HarnessBoundedProgramJournal journal(transitions, fixture.artifact.owner_scope());
        ASSERT_TRUE(journal.latest("run-one"));
        EXPECT_EQ(journal.compare_append({}, initial.journal_record),
                  JournalAppendResult::AlreadyPresent);
        EXPECT_EQ(journal.compare_append(digest('9'), initial.journal_record),
                  JournalAppendResult::Conflict);

        const auto target_artifact = HarnessProgramArtifactRecord::create(
            "artifact-two", fixture.artifact.owner_scope(), fixture.bundle_value,
            fixture.version_value, json{{"transport", "mcp"}}, json{{"status", "queued"}});
        HarnessBoundedProgramStore target_bounded(
            store, target_artifact.artifact_id(), target_artifact.owner_scope(),
            target_artifact.legacy_invocation(), target_artifact.legacy_projection());
        target_bounded.publish_admitted(fixture.bundle_value, fixture.version_value);
        const auto target_transitions = capability->bind_program_transitions(target_artifact);
        EXPECT_TRUE(target_transitions->load(fixture.artifact.owner_scope(), "run-one"));

        ASSERT_TRUE(store->load_run("legacy-run"));
        EXPECT_EQ(store->list_events("legacy-run").size(), 1U);
    }
}

TEST(HarnessProgramStoreTest, TwoSqliteInstancesHaveOneCasWinnerAndRollbackInvalidBatch) {
    TempDb  db;
    Fixture fixture;
    auto    first_store = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto    first       = persist_and_bind(first_store, fixture);
    auto    initial     = initial_publication(fixture);
    ASSERT_EQ(first->compare_publish(fixture.artifact.owner_scope(), {}, initial),
              ProgramTransitionPublishResult::Published);

    auto invalid = terminal_publication(fixture, initial, ProgramTerminalStatus::Completed,
                                        ContinuationState::Completed, 15);
    invalid.events.front().sequence = 7;
    EXPECT_EQ(
        first->compare_publish(fixture.artifact.owner_scope(), initial.journal_record.id, invalid),
        ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(first->load_events(fixture.artifact.owner_scope(), "run-one").size(), 1U);
    EXPECT_EQ(first->latest(fixture.artifact.owner_scope(), "run-one")->id,
              initial.journal_record.id);

    auto second_store = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto second       = require_harness_program_adapter_store(second_store)
                      ->bind_program_transitions(fixture.artifact);
    auto completed = terminal_publication(fixture, initial, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed, 20);
    auto failed    = terminal_publication(fixture, initial, ProgramTerminalStatus::Failed,
                                          ContinuationState::Failed, 21);
    std::barrier ready(3);
    auto         publish = [&](const std::shared_ptr<ProgramTransitionStore>& store,
                       ProgramTransitionPublication                   value) {
        ready.arrive_and_wait();
        return store->compare_publish(fixture.artifact.owner_scope(), initial.journal_record.id,
                                              std::move(value));
    };
    auto a = std::async(std::launch::async, [&] { return publish(first, completed); });
    auto b = std::async(std::launch::async, [&] { return publish(second, failed); });
    ready.arrive_and_wait();
    const auto result_a = a.get();
    const auto result_b = b.get();
    EXPECT_TRUE((result_a == ProgramTransitionPublishResult::Published &&
                 result_b == ProgramTransitionPublishResult::Conflict) ||
                (result_b == ProgramTransitionPublishResult::Published &&
                 result_a == ProgramTransitionPublishResult::Conflict));

    first.reset();
    second.reset();
    first_store.reset();
    second_store.reset();
    auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions =
        require_harness_program_adapter_store(reopened)->bind_program_transitions(fixture.artifact);
    const auto run = transitions->load(fixture.artifact.owner_scope(), "run-one");
    ASSERT_TRUE(run);
    ASSERT_TRUE(run->terminal_result());
    EXPECT_EQ(transitions->load_events(fixture.artifact.owner_scope(), "run-one").size(), 2U);
    EXPECT_EQ(transitions->latest(fixture.artifact.owner_scope(), "run-one")->sequence, 2U);
}

TEST(HarnessProgramStoreTest, ReopenRejectsTamperAndMissingPublishedOutbox) {
    Fixture fixture;
    {
        TempDb db;
        {
            auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
            auto transitions = persist_and_bind(store, fixture);
            auto initial     = initial_publication(fixture);
            ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                      ProgramTransitionPublishResult::Published);
        }
        sqlite_exec(db.path,
                    "UPDATE neograph_harness_runs SET record_json='{}' "
                    "WHERE program_run_id<>''");
        auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        EXPECT_THROW((void)reopened->resolve_program_run(fixture.artifact.owner_scope(), "run-one"),
                     std::invalid_argument);
    }
    {
        TempDb db;
        {
            auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
            auto transitions = persist_and_bind(store, fixture);
            auto initial     = initial_publication(fixture);
            ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                      ProgramTransitionPublishResult::Published);
        }
        sqlite_exec(db.path, "DELETE FROM neograph_harness_program_events");
        auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions =
            require_harness_program_adapter_store(reopened)->bind_program_transitions(
                fixture.artifact);
        EXPECT_THROW((void)transitions->load(fixture.artifact.owner_scope(), "run-one"),
                     std::invalid_argument);
    }
}
