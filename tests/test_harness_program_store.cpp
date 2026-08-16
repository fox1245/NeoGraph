#include <neograph/graph/checkpoint.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_program_store.h>
#include <neograph/mcp/sqlite_harness_store.h>
#include <neograph/program/registry.h>
#include <neograph/program/replay.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#if defined(__linux__)
#include <sys/wait.h>
#endif


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

ProgramBundle bundle(const RegistrySnapshot& registry,
                     char                    source_hash = '1',
                     char                    program_hash = '2') {
    const json definition = {{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                             {"nodes", json{{"main", json{{"type", "persist-node"}}}}}};
    ProgramBundleData data;
    data.source_kind                   = SourceKind::CppBuilder;
    data.source_hash                   = digest(source_hash);
    data.canonical_program_hash        = digest(program_hash);
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
        .allow_source_kind(SourceKind::CppBuilder)
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

RunBudget budget() {
    return {1000, 100, 100, 1, 10, 100, 1, 1, 1};
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
    HarnessProgramArtifactRecord artifact = HarnessProgramArtifactRecord::create(
        "artifact-one", policy_value.owner_scope(), bundle_value, version_value,
        HarnessInvocationTemplate{json{{"request", "hello"}}, budget()},
        json{{"status", "queued"}});
};

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
                                                  std::string    run_id = "run-one",
                                                  RunBudget     granted = budget(),
                                                  std::int64_t  timestamp = 10) {
    auto                 journal = ProgramJournalRecord::create({{},
                                                                 run_id,
                                                                 fixture.version_value.id(),
                                                                 fixture.bundle_value.id(),
                                                                 1,
                                                                 {"root", ContinuationState::Running, 1},
                                                                 granted,
                                                                 {},
                                                                 std::nullopt,
                                                                 timestamp});
    ProgramRunRecordData data;
    data.owner_scope         = fixture.policy_value.owner_scope();
    data.run_id              = run_id;
    data.program_version_id  = fixture.version_value.id();
    data.bundle_id           = fixture.bundle_value.id();
    data.binding_fingerprint = digest('6');
    auto invocation = bind_harness_invocation(
        fixture.artifact.invocation_template(), fixture.artifact.owner_scope(),
        fixture.version_value.id(), run_id, "trace-one");
    invocation.budget        = granted;
    data.invocation          = std::move(invocation);
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.journal_head        = journal.id;
    data.event_sequence      = 1;
    data.created_at_ms       = timestamp;
    data.updated_at_ms       = timestamp;
    return {
        ProgramRunRecord::create(std::move(data)),
        journal,
        {event(fixture, run_id, 1, ProgramEventKind::Started, ProgramStartedEvent{granted},
               timestamp)},
        {}};
}

void attach_initial_lineage(ProgramTransitionPublication& publication) {
    const auto& run         = publication.run_record;
    const auto  lineage_id  = program_run_lineage_id(run.owner_scope(), run.run_id());
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        run.owner_scope(), lineage_id, 1, run.run_id(), run.program_version_id(), run.bundle_id(),
        run.id(), run.journal_head(), std::nullopt, run.created_at_ms()});
    auto lineage = ProgramRunLineage::create(ProgramRunLineageData{
        run.owner_scope(), lineage_id, run.run_id(), 1, generation.id(), run.id(),
        run.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, std::nullopt, run.created_at_ms(),
        run.updated_at_ms()});
    publication.run_generation = std::move(generation);
    publication.run_lineage    = std::move(lineage);
}

ProgramTransitionPublication initial_lineage_publication(const Fixture& fixture,
                                                          std::string    run_id = "run-one") {
    auto publication = initial_publication(fixture, std::move(run_id));
    attach_initial_lineage(publication);
    return publication;
}

void attach_same_generation_lineage(ProgramTransitionPublication& publication,
                                    const ProgramRunLineage&      previous) {
    const auto& run = publication.run_record;
    publication.run_generation.reset();
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        previous.active_generation(), previous.active_generation_id(), run.id(),
        run.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        run.updated_at_ms(), previous.committed_descendant_budget()});
}

void attach_fork_receipt(ProgramTransitionPublication& publication,
                         const ProgramRunRecord&       source,
                         bool                          retain_source_pending = true) {
    std::optional<std::string>          pending_id;
    json                                resume_value = json::object();
    std::optional<ProgramPendingInput>  target_pending_input;
    std::optional<ProgramPendingEffect> target_pending_effect;
    const auto source_pending_input = retain_source_pending ? source.pending_input() : std::nullopt;
    const auto source_pending_effect =
        retain_source_pending ? source.pending_effect() : std::nullopt;
    if (const auto pending = source_pending_input) {
        pending_id         = pending->call_id();
        const auto applied = pending->submit(*pending_id, resume_value, 20);
        if (applied.disposition != ProgramPendingDisposition::Applied) {
            throw std::logic_error("Test fork pending input could not be consumed");
        }
        target_pending_input = applied.value;
    } else if (const auto pending = source_pending_effect) {
        pending_id         = pending->call_id();
        resume_value       = json{{"result", "forked"}};
        const auto applied = pending->submit(*pending_id, pending->effect_id(), resume_value, 20);
        if (applied.disposition != ProgramPendingDisposition::Applied) {
            throw std::logic_error("Test fork pending effect could not be consumed");
        }
        target_pending_effect = applied.value;
    }
    const auto receipt = ForkCompatibilityReceipt(ForkCompatibilityReceiptData{
                                                      source.owner_scope(),
                                                      source.run_id(),
                                                      source.program_version_id(),
                                                      source.exact_checkpoint()->checkpoint_id,
                                                      publication.run_record.program_version_id(),
                                                      ForkCompatibilityStatus::Compatible,
                                                      {}})
                             .with_initial_resume_binding(std::move(pending_id), resume_value);
    const auto run = publication.run_record;
    ProgramRunRecordData data;
    data.owner_scope                    = run.owner_scope();
    data.run_id                         = run.run_id();
    data.program_version_id             = run.program_version_id();
    data.bundle_id                      = run.bundle_id();
    data.binding_fingerprint            = run.binding_fingerprint();
    data.invocation                     = run.invocation();
    data.continuation                   = run.continuation();
    data.remaining_budget               = run.remaining_budget();
    data.exact_checkpoint               = run.exact_checkpoint();
    data.pending_input                  = std::move(target_pending_input);
    data.pending_effect                 = std::move(target_pending_effect);
    data.fork_receipt                   = receipt;
    data.fork_source_run_id             = receipt.source_run_id();
    data.fork_source_program_version_id = receipt.source_program_version_id();
    data.fork_source_checkpoint_id      = receipt.source_checkpoint_id();
    data.journal_head                   = run.journal_head();
    data.event_sequence                 = run.event_sequence();
    data.effect_sequence                = run.effect_sequence();
    data.created_at_ms                  = run.created_at_ms();
    data.updated_at_ms                  = run.updated_at_ms();
    publication.run_record              = ProgramRunRecord::create(std::move(data));
}

void attach_successor_lineage(ProgramTransitionPublication& publication,
                              const ProgramRunLineage&      previous) {
    const auto& run = publication.run_record;
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        run.owner_scope(), previous.lineage_id(), previous.active_generation() + 1, run.run_id(),
        run.program_version_id(), run.bundle_id(), run.id(), run.journal_head(),
        previous.active_generation_id(), run.created_at_ms()});
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        generation.generation(), generation.id(), run.id(), run.journal_head(),
        publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        run.updated_at_ms(), previous.committed_descendant_budget()});
    publication.run_generation = std::move(generation);
}

std::vector<ProgramJavaScriptCommandJournalEntry> replacement_checkpoint_entries(
    const Fixture& fixture, const json& handoff) {
    const auto command = JavaScriptCommand::checkpoint("replacement:harness", handoff);
    return {
        ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
            1, fixture.bundle_value.id(), 1, command, digest('9'), std::nullopt}),
        ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
            2, fixture.bundle_value.id(), 1, command, digest('9'),
            json{{"status", "completed"},
                 {"output", handoff},
                 {"failure", nullptr},
                 {"execution_trace", json::array()},
                 {"usage", json{{"wall_time_ms", 0},
                                {"model_tokens", 0},
                                {"monetary_microunits", 0},
                                {"program_operations", 0},
                                {"core_steps", 0},
                                {"peak_concurrency", 0}}}}})};
}

void attach_replacement_successor(
    ProgramTransitionPublication&                 publication,
    const ProgramRunLineage&                      previous,
    const ProgramRunGeneration&                   predecessor,
    const ProgramRunRecord&                       source,
    const ProgramJavaScriptCommandJournalEntry& checkpoint) {
    const auto handoff = checkpoint.command().arguments().at("value");
    auto receipt = ProgramReplacementReceipt(ProgramReplacementReceiptData{
        source.owner_scope(),
        previous.lineage_id(),
        predecessor.generation(),
        predecessor.id(),
        previous.id(),
        source.run_id(),
        source.id(),
        source.journal_head(),
        source.program_version_id(),
        source.bundle_id(),
        checkpoint.coordinate_id(),
        checkpoint.id(),
        program_replacement_handoff_identity(handoff),
        predecessor.generation() + 1,
        publication.run_record.run_id(),
        publication.run_record.program_version_id(),
        publication.run_record.bundle_id(),
        program_replacement_input_identity(publication.run_record.invocation().input),
        publication.run_record.invocation().canonical_identity(),
        publication.run_record.binding_fingerprint(),
        publication.run_record.id(),
        publication.run_record.journal_head()});
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        publication.run_record.owner_scope(), previous.lineage_id(),
        predecessor.generation() + 1, publication.run_record.run_id(),
        publication.run_record.program_version_id(), publication.run_record.bundle_id(),
        publication.run_record.id(), publication.run_record.journal_head(), predecessor.id(),
        publication.run_record.created_at_ms(), publication.run_record.child_depth(),
        std::move(receipt)});
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        generation.generation(), generation.id(), publication.run_record.id(),
        publication.run_record.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        publication.run_record.updated_at_ms(), previous.committed_descendant_budget()});
    publication.run_generation = std::move(generation);
}

ProgramPendingEffect pending_effect() {
    ProgramPendingEffectData data;
    data.operation_id         = "root";
    data.call_id              = "effect-call";
    data.effect_id            = "effect-one";
    data.result_schema        = json{{"type", "object"}, {"additionalProperties", true}};
    data.payload              = json{{"kind", "publish"}};
    data.expires_at_unix_ms   = 5000;
    data.effect_mode          = EffectMode::Brokered;
    data.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    data.core_node            = "main";
    data.core_interrupt_value = json{{"value", "effect"}};
    return ProgramPendingEffect(std::move(data));
}

ProgramTransitionPublication publication_with_effect(
    const Fixture& fixture, const ProgramTransitionPublication& initial) {
    auto pending = pending_effect();
    const CoreCheckpointIdentity checkpoint{"main", digest('5'), "core-thread", "checkpoint-one",
                                            graph::CHECKPOINT_SCHEMA_VERSION};
    auto journal = ProgramJournalRecord::create({initial.journal_record.id,
                                                 initial.run_record.run_id(),
                                                 fixture.version_value.id(),
                                                 fixture.bundle_value.id(),
                                                 2,
                                                 {"root", ContinuationState::Interrupted, 1},
                                                 budget(),
                                                 {},
                                                 checkpoint,
                                                 10});

    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = initial.run_record.run_id();
    result_data.program_version_id = fixture.version_value.id();
    result_data.bundle_id          = fixture.bundle_value.id();
    result_data.attempt            = 1;
    result_data.output             = json::object();
    result_data.remaining_budget   = budget();
    result_data.checkpoint         = checkpoint;
    result_data.interrupt = ProgramInterrupt{pending.core_node(), pending.core_interrupt_value(),
                                              std::nullopt, pending};

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
    data.pending_effect      = pending;
    data.terminal_result     = ProgramResult::create(std::move(result_data));
    data.journal_head        = journal.id;
    data.event_sequence      = 2;
    data.effect_sequence     = 1;
    data.created_at_ms       = initial.run_record.created_at_ms();
    data.updated_at_ms       = 10;

    auto publication           = initial;
    publication.run_record     = ProgramRunRecord::create(std::move(data));
    publication.journal_record = std::move(journal);
    publication.events = {event(fixture, initial.run_record.run_id(), 2,
                                 ProgramEventKind::Terminal,
                                 ProgramTerminalEvent{ProgramTerminalStatus::Interrupted}, 10)};
    publication.effects.emplace_back(1, std::move(pending));
    return publication;
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
        fixture.artifact.invocation_template(), fixture.artifact.projection());
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
    auto missing_run = without_field(wrapped.serialize(), "invocation");
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

TEST(HarnessProgramStoreTest, FileStorePersistsLongContentAddressedIdentifiers) {
    TempDb directory;
    const auto root = directory.path.string() + "-long-id-files";
    auto       file = std::make_shared<FileHarnessRecordStore>(root);
    const auto component = [](char value) { return "sha256-" + std::string(64, value); };
    const auto id =
        "artifact-" + component('1') + "-" + component('2') + "-" + component('3');
    const json record{{"artifact_id", id}, {"value", "stored"}};

    file->save_artifact(id, record);
    EXPECT_EQ(file->load_artifact(id), record);

    file.reset();
    std::error_code ignored;
#ifdef _WIN32
    const auto absolute_root =
        std::filesystem::absolute(std::filesystem::path(root)).lexically_normal();
    std::filesystem::remove_all(std::filesystem::path(LR"(\\?\)" + absolute_root.native()),
                                ignored);
#else
    std::filesystem::remove_all(root, ignored);
#endif
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
            fixture.artifact.invocation_template(), fixture.artifact.projection());
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
            fixture.version_value,
            HarnessInvocationTemplate{json{{"request", "hello"}}, budget()},
            json{{"status", "queued"}});
        HarnessBoundedProgramStore target_bounded(
            store, target_artifact.artifact_id(), target_artifact.owner_scope(),
            target_artifact.invocation_template(), target_artifact.projection());
        target_bounded.publish_admitted(fixture.bundle_value, fixture.version_value);
        const auto target_transitions = capability->bind_program_transitions(target_artifact);
        EXPECT_TRUE(target_transitions->load(fixture.artifact.owner_scope(), "run-one"));

        ASSERT_TRUE(store->load_run("legacy-run"));
        EXPECT_EQ(store->list_events("legacy-run").size(), 1U);
    }
}

TEST(HarnessProgramStoreTest, SqliteRejectsReceiptlessSuccessorAcrossReconnect) {
    TempDb  db;
    Fixture fixture;
    auto       source      = initial_lineage_publication(fixture, "lineage-source");
    const auto lineage_id  = source.run_lineage->lineage_id();
    const auto head_one_id = source.run_lineage->id();

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, source),
                  ProgramTransitionPublishResult::Published);
        ASSERT_TRUE(transitions->load_lineage(fixture.artifact.owner_scope(), lineage_id));
        EXPECT_FALSE(transitions->load_lineage("tenant-other", lineage_id));
    }

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = require_harness_program_adapter_store(store)
                               ->bind_program_transitions(fixture.artifact);
        const auto previous = transitions->load_lineage(fixture.artifact.owner_scope(), lineage_id);
        ASSERT_TRUE(previous);
        ASSERT_EQ(previous->id(), head_one_id);
        ASSERT_TRUE(transitions->load_generation(fixture.artifact.owner_scope(), lineage_id, 1));

        auto successor = initial_publication(fixture, "lineage-successor");
        attach_successor_lineage(successor, *previous);
        EXPECT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, successor),
                   ProgramTransitionPublishResult::Conflict);
        EXPECT_FALSE(transitions->load(fixture.artifact.owner_scope(), "lineage-successor"));
    }

    auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions = require_harness_program_adapter_store(reopened)
                           ->bind_program_transitions(fixture.artifact);
    const auto head = transitions->load_lineage(fixture.artifact.owner_scope(), lineage_id);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->id(), head_one_id);
    EXPECT_EQ(head->active_generation(), 1U);
    EXPECT_EQ(head->active_generation_id(), source.run_generation->id());
    EXPECT_EQ(head->active_journal_head(), source.journal_record.id);
    EXPECT_EQ(transitions
                  ->load_run_lineage(fixture.artifact.owner_scope(), "lineage-source")
                  ->id(),
              head_one_id);
    EXPECT_FALSE(transitions->load_run_lineage(fixture.artifact.owner_scope(),
                                               "lineage-successor"));
    ASSERT_TRUE(transitions->load_lineage_head(fixture.artifact.owner_scope(), lineage_id,
                                               head_one_id));
    const auto generation_one =
        transitions->load_generation(fixture.artifact.owner_scope(), lineage_id, 1);
    const auto generation_two =
        transitions->load_generation(fixture.artifact.owner_scope(), lineage_id, 2);
    ASSERT_TRUE(generation_one);
    EXPECT_EQ(generation_one->id(), source.run_generation->id());
    EXPECT_FALSE(generation_two);
}

TEST(HarnessProgramStoreTest, SqliteReplacementFaultRollsBackAndReceiptSurvivesReconnect) {
    TempDb  db;
    Fixture source_fixture;
    Fixture target_fixture;
    const json handoff{{"cursor", 7}, {"state", "ready"}};
    const json input{{"handoff", handoff}, {"previous_run_id", "lineage-source"}};
    target_fixture.bundle_value = bundle(target_fixture.registry_value, 'a', 'b');
    target_fixture.version_value = ProgramVersion(ProgramVersionData(
        target_fixture.bundle_value.id(), target_fixture.profile_value,
        target_fixture.policy_value, {}, target_fixture.policy_value.owner_scope(),
        CoreMaterializationReceipt{"persist-compiler/v1",
                                   target_fixture.registry_value.fingerprint(),
                                   target_fixture.bundle_value.core_plan_identities(), {}}));
    target_fixture.artifact = HarnessProgramArtifactRecord::create(
        "artifact-two", target_fixture.policy_value.owner_scope(), target_fixture.bundle_value,
        target_fixture.version_value, HarnessInvocationTemplate{input, budget()},
        json{{"status", "queued"}});

    auto source = initial_lineage_publication(source_fixture, "lineage-source");
    source.commands = replacement_checkpoint_entries(source_fixture, handoff);
    const auto lineage_id = source.run_lineage->lineage_id();
    const auto successor_budget = program_replacement_remaining_budget(
        source.run_record, *source.run_lineage, 40);
    auto target =
        initial_publication(target_fixture, "lineage-successor", successor_budget, 40);
    attach_replacement_successor(target, *source.run_lineage, *source.run_generation,
                                 source.run_record, source.commands.back());
    const auto target_generation_id = target.run_generation->id();
    const auto receipt_id = target.run_generation->replacement_receipt()->id();

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto source_transitions = persist_and_bind(store, source_fixture);
        ASSERT_EQ(source_transitions->compare_publish(source_fixture.artifact.owner_scope(), {},
                                                      source),
                  ProgramTransitionPublishResult::Published);
        auto target_transitions = persist_and_bind(store, target_fixture);
        EXPECT_FALSE(source_transitions->process_coordination_key().empty());
        EXPECT_EQ(source_transitions->process_coordination_key(),
                  target_transitions->process_coordination_key());
        store->fail_next_program_transition_for_testing(
            SqliteHarnessProgramFaultPoint::AfterRunWrite);
        EXPECT_THROW((void)target_transitions->compare_publish(
                         target_fixture.artifact.owner_scope(), {}, target),
                      std::runtime_error);
    }

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = require_harness_program_adapter_store(store)
                               ->bind_program_transitions(target_fixture.artifact);
        EXPECT_FALSE(
            transitions->load(target_fixture.artifact.owner_scope(), "lineage-successor"));
        EXPECT_FALSE(transitions->load_generation(target_fixture.artifact.owner_scope(),
                                                   lineage_id, 2));
        EXPECT_FALSE(transitions->load_generation_initial_publication(
            target_fixture.artifact.owner_scope(), lineage_id, 2));
        const auto old_head =
            transitions->load_lineage(target_fixture.artifact.owner_scope(), lineage_id);
        ASSERT_TRUE(old_head);
        EXPECT_EQ(old_head->id(), source.run_lineage->id());
        ASSERT_EQ(transitions->compare_publish(target_fixture.artifact.owner_scope(), {}, target),
                  ProgramTransitionPublishResult::Published);
    }

    sqlite_exec(db.path,
                "DELETE FROM neograph_harness_program_generation_publications; "
                "UPDATE neograph_harness_schema SET version=6 WHERE singleton=1;");

    auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions = require_harness_program_adapter_store(store)
                           ->bind_program_transitions(target_fixture.artifact);
    const auto head =
        transitions->load_lineage(target_fixture.artifact.owner_scope(), lineage_id);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->active_generation(), 2U);
    EXPECT_EQ(head->active_generation_id(), target_generation_id);
    const auto generation =
        transitions->load_generation(target_fixture.artifact.owner_scope(), lineage_id, 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->replacement_receipt());
    EXPECT_EQ(generation->replacement_receipt()->id(), receipt_id);
    EXPECT_EQ(transitions
                  ->load_run_lineage(target_fixture.artifact.owner_scope(), "lineage-source")
                  ->id(),
              head->id());
    EXPECT_EQ(transitions
                  ->load_run_lineage(target_fixture.artifact.owner_scope(), "lineage-successor")
                  ->id(),
              head->id());
    EXPECT_EQ(transitions
                  ->load(target_fixture.artifact.owner_scope(), "lineage-successor")
                  ->remaining_budget(),
              successor_budget);
    const auto source_initial = transitions->load_generation_initial_publication(
        target_fixture.artifact.owner_scope(), lineage_id, 1);
    const auto target_initial = transitions->load_generation_initial_publication(
        target_fixture.artifact.owner_scope(), lineage_id, 2);
    ASSERT_TRUE(source_initial);
    ASSERT_TRUE(target_initial);
    EXPECT_EQ(source_initial->run_record.id(), source.run_record.id());
    EXPECT_EQ(target_initial->run_record.id(), target.run_record.id());
    const auto chain = inspect_program_replacement_chain(
        *transitions, target_fixture.artifact.owner_scope(), "lineage-source");
    ASSERT_EQ(chain.replacements().size(), 1U);
    EXPECT_EQ(chain.replacements().front().target_generation().id(), target_generation_id);
}

TEST(HarnessProgramStoreTest, SqliteReplacementRejectsMissingPredecessorArtifact) {
    TempDb  db;
    Fixture source_fixture;
    Fixture target_fixture;
    const json handoff{{"cursor", 8}, {"state", "ready"}};
    const json input{{"handoff", handoff}, {"previous_run_id", "lineage-source"}};
    target_fixture.bundle_value = bundle(target_fixture.registry_value, 'a', 'b');
    target_fixture.version_value = ProgramVersion(ProgramVersionData(
        target_fixture.bundle_value.id(), target_fixture.profile_value,
        target_fixture.policy_value, {}, target_fixture.policy_value.owner_scope(),
        CoreMaterializationReceipt{"persist-compiler/v1",
                                   target_fixture.registry_value.fingerprint(),
                                   target_fixture.bundle_value.core_plan_identities(), {}}));
    target_fixture.artifact = HarnessProgramArtifactRecord::create(
        "artifact-two", target_fixture.policy_value.owner_scope(), target_fixture.bundle_value,
        target_fixture.version_value, HarnessInvocationTemplate{input, budget()},
        json{{"status", "queued"}});

    auto source = initial_lineage_publication(source_fixture, "lineage-source");
    source.commands = replacement_checkpoint_entries(source_fixture, handoff);
    const auto successor_budget = program_replacement_remaining_budget(
        source.run_record, *source.run_lineage, 40);
    auto target = initial_publication(target_fixture, "lineage-successor", successor_budget, 40);
    attach_replacement_successor(target, *source.run_lineage, *source.run_generation,
                                 source.run_record, source.commands.back());

    auto store = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto source_transitions = persist_and_bind(store, source_fixture);
    ASSERT_EQ(source_transitions->compare_publish(source_fixture.artifact.owner_scope(), {}, source),
              ProgramTransitionPublishResult::Published);
    auto target_transitions = persist_and_bind(store, target_fixture);
    sqlite_exec(db.path,
                "PRAGMA foreign_keys=OFF; DELETE FROM neograph_harness_artifacts "
                "WHERE artifact_id='artifact-one';");

    EXPECT_THROW((void)target_transitions->compare_publish(
                     target_fixture.artifact.owner_scope(), {}, target),
                 std::invalid_argument);
    EXPECT_FALSE(
        target_transitions->load_generation(target_fixture.artifact.owner_scope(),
                                            source.run_lineage->lineage_id(), 2));
    const auto head = target_transitions->load_lineage(target_fixture.artifact.owner_scope(),
                                                       source.run_lineage->lineage_id());
    ASSERT_TRUE(head);
    EXPECT_EQ(head->id(), source.run_lineage->id());
}

TEST(HarnessProgramStoreTest, SqliteForkAtomicallyDebitsSourceAndCreatesTargetLineage) {
    TempDb  db;
    Fixture fixture;
    auto    source_start = initial_lineage_publication(fixture, "fork-source");
    std::string source_lineage_id;
    std::string debited_source_head_id;
    std::string target_lineage_id;

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, source_start),
                  ProgramTransitionPublishResult::Published);
        auto source = publication_with_effect(fixture, source_start);
        attach_same_generation_lineage(source, *source_start.run_lineage);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(),
                                               source_start.journal_record.id, source),
                  ProgramTransitionPublishResult::Published);

        auto dropped = initial_publication(fixture, "fork-dropped-pending");
        attach_fork_receipt(dropped, source.run_record, false);
        dropped.migration_plan =
            MigrationPlan::create(MigrationPlanData{fixture.version_value.id(),
                                                    fixture.version_value.id(),
                                                    fixture.artifact.owner_scope(),
                                                    MigrationCompatibility::ForkCompatible,
                                                    {},
                                                    {},
                                                    {}});
        attach_initial_lineage(dropped);
        dropped.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
            source.run_lineage->owner_scope(), source.run_lineage->lineage_id(),
            source.run_lineage->root_run_id(), source.run_lineage->active_generation(),
            source.run_lineage->active_generation_id(), source.run_lineage->active_run_record_id(),
            source.run_lineage->active_journal_head(), RunBudget{},
            source.run_lineage->inflight_reservation(), source.run_lineage->id(),
            source.run_lineage->created_at_ms(), dropped.run_record.updated_at_ms(),
            source.run_lineage->committed_descendant_budget()});
        EXPECT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, dropped),
                  ProgramTransitionPublishResult::Conflict);
        EXPECT_FALSE(transitions->load(fixture.artifact.owner_scope(), "fork-dropped-pending"));

        auto target = initial_publication(fixture, "fork-target");
        attach_fork_receipt(target, source.run_record);
        target.migration_plan = MigrationPlan::create(MigrationPlanData{
            fixture.version_value.id(), fixture.version_value.id(),
            fixture.artifact.owner_scope(), MigrationCompatibility::ForkCompatible, {}, {}, {}});
        attach_initial_lineage(target);
        target.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
            source.run_lineage->owner_scope(), source.run_lineage->lineage_id(),
            source.run_lineage->root_run_id(), source.run_lineage->active_generation(),
            source.run_lineage->active_generation_id(),
            source.run_lineage->active_run_record_id(), source.run_lineage->active_journal_head(),
            RunBudget{}, source.run_lineage->inflight_reservation(), source.run_lineage->id(),
            source.run_lineage->created_at_ms(), target.run_record.updated_at_ms(),
            source.run_lineage->committed_descendant_budget()});

        source_lineage_id      = source.run_lineage->lineage_id();
        debited_source_head_id = target.fork_source_lineage->id();
        target_lineage_id      = target.run_lineage->lineage_id();
        ASSERT_NE(source_lineage_id, target_lineage_id);
        store->fail_next_program_transition_for_testing(
            SqliteHarnessProgramFaultPoint::AfterRunWrite);
        EXPECT_THROW((void)transitions->compare_publish(fixture.artifact.owner_scope(), {}, target),
                     std::runtime_error);
        EXPECT_FALSE(transitions->load(fixture.artifact.owner_scope(), "fork-target"));
        EXPECT_EQ(transitions
                      ->load_run_lineage(fixture.artifact.owner_scope(), "fork-source")
                      ->id(),
                  source.run_lineage->id());
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, target),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, target),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }

    auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions = require_harness_program_adapter_store(reopened)
                           ->bind_program_transitions(fixture.artifact);
    const auto source =
        transitions->load_run_lineage(fixture.artifact.owner_scope(), "fork-source");
    const auto target =
        transitions->load_run_lineage(fixture.artifact.owner_scope(), "fork-target");
    ASSERT_TRUE(source);
    ASSERT_TRUE(target);
    EXPECT_EQ(source->lineage_id(), source_lineage_id);
    EXPECT_EQ(source->id(), debited_source_head_id);
    EXPECT_EQ(source->remaining_budget(), RunBudget{});
    EXPECT_EQ(target->lineage_id(), target_lineage_id);
    EXPECT_NE(source->lineage_id(), target->lineage_id());
    EXPECT_EQ(source->active_generation(), 1U);
    EXPECT_EQ(target->active_generation(), 1U);
    ASSERT_TRUE(transitions->load_migration_plan(fixture.artifact.owner_scope(), "fork-target"));
}
TEST(HarnessProgramStoreTest, SqliteReopenSurvivesNewProcessExec) {
#if !defined(__linux__)
    GTEST_SKIP() << "The exec-based SQLite restart proof is Linux-only";
#else
    if (const auto* child_db = std::getenv("NEOGRAPH_SQLITE_REOPEN_DB")) {
        Fixture fixture;
        auto    store = std::make_shared<SqliteHarnessRecordStore>(child_db);
        auto    capability = require_harness_program_adapter_store(store);
        const auto resolved =
            capability->resolve_program_run(fixture.artifact.owner_scope(), "run-exec");
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->run_record().run_id(), "run-exec");
        auto transitions = capability->bind_program_transitions(fixture.artifact);
        ASSERT_TRUE(transitions->load(fixture.artifact.owner_scope(), "run-exec"));
        EXPECT_EQ(transitions->load_events(fixture.artifact.owner_scope(), "run-exec").size(), 1U);
        return;
    }

    TempDb  db;
    Fixture fixture;
    auto    initial = initial_publication(fixture, "run-exec");
    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::Published);
    }

    const auto quote_shell = [](std::string value) {
        std::string quoted = "'";
        for (const char character : value) {
            if (character == '\'') quoted += "'\\''";
            else quoted += character;
        }
        quoted += '\'';
        return quoted;
    };
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();

    const auto command =
        "/usr/bin/env NEOGRAPH_SQLITE_REOPEN_DB=" + quote_shell(db.path.string()) +
        " NEOGRAPH_SQLITE_REOPEN_CHILD=1 " + quote_shell(executable) + " "
        "--gtest_color=no "
        "--gtest_filter=HarnessProgramStoreTest.SqliteReopenSurvivesNewProcessExec";
    EXPECT_EQ(std::system(command.c_str()), 0);
#endif
}

TEST(HarnessProgramStoreTest, SqliteCrashDuringTransitionReopensWithCommittedPrefix) {
#if !defined(__linux__)
    GTEST_SKIP() << "The SIGKILL SQLite crash proof is Linux-only";
#else
    if (const auto* child_db = std::getenv("NEOGRAPH_SQLITE_CRASH_DB")) {
        Fixture fixture;
        auto    store = std::make_shared<SqliteHarnessRecordStore>(child_db);
        auto    transitions = persist_and_bind(store, fixture);
        const auto initial = initial_publication(fixture, "run-crash");
        auto terminal = terminal_publication(
            fixture, initial, ProgramTerminalStatus::Completed, ContinuationState::Completed, 20);
        store->crash_next_program_transition_for_testing(
            SqliteHarnessProgramFaultPoint::AfterEventWrite);
        const auto before = transitions->load(fixture.artifact.owner_scope(), "run-crash");
        ASSERT_TRUE(before);
        ASSERT_EQ(before->journal_head(), initial.journal_record.id);
        const auto result = transitions->compare_publish(
            fixture.artifact.owner_scope(), initial.journal_record.id, std::move(terminal));
        ASSERT_EQ(result, ProgramTransitionPublishResult::Published);
        FAIL() << "SIGKILL crash hook returned";
    }

    TempDb  db;
    Fixture fixture;
    const auto initial = initial_publication(fixture, "run-crash");
    {
        auto store = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::Published);
    }

    const auto quote_shell = [](std::string value) {
        std::string quoted = "'";
        for (const char character : value) {
            if (character == '\'') quoted += "'\\''";
            else quoted += character;
        }
        quoted += '\'';
        return quoted;
    };
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    const auto command = "/usr/bin/env NEOGRAPH_SQLITE_CRASH_DB=" + quote_shell(db.path.string()) +
                         " " + quote_shell(executable) +
                         " --gtest_color=no "
                         "--gtest_filter=HarnessProgramStoreTest."
                         "SqliteCrashDuringTransitionReopensWithCommittedPrefix";
    const int status = std::system(command.c_str());
    ASSERT_NE(status, -1);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 128 + SIGKILL);

    auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions = persist_and_bind(reopened, fixture);
    const auto latest = transitions->latest(fixture.artifact.owner_scope(), "run-crash");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, initial.journal_record.id);
    EXPECT_FALSE(transitions->load(fixture.artifact.owner_scope(), "run-crash")
                     ->terminal_result());
    EXPECT_EQ(transitions->load_events(fixture.artifact.owner_scope(), "run-crash").size(), 1U);

    EXPECT_EQ(transitions->compare_publish(
                  fixture.artifact.owner_scope(), initial.journal_record.id,
                  terminal_publication(fixture, initial, ProgramTerminalStatus::Completed,
                                       ContinuationState::Completed, 20)),
              ProgramTransitionPublishResult::Published);
    ASSERT_TRUE(transitions->load(fixture.artifact.owner_scope(), "run-crash")->terminal_result());
#endif
}

TEST(HarnessProgramStoreTest, SqliteProgramTransitionsEnforceOwnerScope) {
    TempDb  db;
    Fixture fixture;
    auto    store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto    transitions = persist_and_bind(store, fixture);
    auto    initial     = initial_publication(fixture);

    ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
              ProgramTransitionPublishResult::Published);

    // A run ID is only meaningful inside its admitted owner scope. Wrong-owner
    // reads must be indistinguishable from absence, and publication must not
    // cross the owner boundary.
    EXPECT_FALSE(transitions->load("tenant-other", "run-one").has_value());
    EXPECT_FALSE(transitions->latest("tenant-other", "run-one").has_value());
    EXPECT_TRUE(transitions->load_events("tenant-other", "run-one").empty());
    EXPECT_TRUE(transitions->load_effects("tenant-other", "run-one").empty());
    EXPECT_EQ(transitions->compare_publish("tenant-other", {}, initial),
              ProgramTransitionPublishResult::Conflict);
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

TEST(HarnessProgramStoreTest, SqlitePublicationFaultsRollbackAllRowsAcrossReconnect) {
    Fixture fixture;
    const auto points = {
        SqliteHarnessProgramFaultPoint::AfterBegin,
        SqliteHarnessProgramFaultPoint::AfterRunWrite,
        SqliteHarnessProgramFaultPoint::AfterJournalWrite,
        SqliteHarnessProgramFaultPoint::AfterEventWrite,
        SqliteHarnessProgramFaultPoint::AfterEffectWrite,
        SqliteHarnessProgramFaultPoint::BeforeCommit,
    };

    for (const auto point : points) {
        TempDb db;
        auto   store = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        auto initial     = initial_publication(fixture, "run-effect");
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::Published);
        auto publication = publication_with_effect(fixture, initial);
        store->fail_next_program_transition_for_testing(point);

        EXPECT_THROW(
            (void)transitions->compare_publish(fixture.artifact.owner_scope(),
                                                initial.journal_record.id, publication),
            std::runtime_error);

        transitions.reset();
        store.reset();
        auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto recovered =
            require_harness_program_adapter_store(reopened)->bind_program_transitions(
                fixture.artifact);
        EXPECT_TRUE(recovered->load(fixture.artifact.owner_scope(), "run-effect"));
        EXPECT_EQ(recovered->latest(fixture.artifact.owner_scope(), "run-effect")->id,
                  initial.journal_record.id);
        EXPECT_EQ(recovered->load_events(fixture.artifact.owner_scope(), "run-effect").size(), 1U);
        EXPECT_TRUE(recovered->load_effects(fixture.artifact.owner_scope(), "run-effect").empty());

        EXPECT_EQ(
            recovered->compare_publish(fixture.artifact.owner_scope(),
                                       initial.journal_record.id, publication),
            ProgramTransitionPublishResult::Published);
        EXPECT_EQ(recovered->load_events(fixture.artifact.owner_scope(), "run-effect").size(), 2U);
        EXPECT_EQ(recovered->load_effects(fixture.artifact.owner_scope(), "run-effect").size(), 1U);
        EXPECT_EQ(
            recovered->compare_publish(fixture.artifact.owner_scope(),
                                       initial.journal_record.id, publication),
            ProgramTransitionPublishResult::AlreadyPresent);
        EXPECT_EQ(recovered->load_events(fixture.artifact.owner_scope(), "run-effect").size(), 2U);
        EXPECT_EQ(recovered->load_effects(fixture.artifact.owner_scope(), "run-effect").size(), 1U);
    }
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

TEST(HarnessProgramStoreTest, SqliteDurablyLoadsJavaScriptCommandJournalByOwnerAndSequence) {
    TempDb  db;
    Fixture fixture;
    auto    initial = initial_publication(fixture, "run-command");
    initial.commands.emplace_back(ProgramJavaScriptCommandJournalEntryData{
        .sequence        = 1,
        .bundle_id       = fixture.bundle_value.id(),
        .command_ordinal = 1,
        .command         = JavaScriptCommand::emit("store:command:1", json{{"value", "first"}}),
    });
    const auto expected = initial.commands.front().serialize_canonical();

    {
        auto store       = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
        auto transitions = persist_and_bind(store, fixture);
        ASSERT_EQ(transitions->compare_publish(fixture.artifact.owner_scope(), {}, initial),
                  ProgramTransitionPublishResult::Published);
    }

    auto reopened = std::make_shared<SqliteHarnessRecordStore>(db.path.string());
    auto transitions =
        require_harness_program_adapter_store(reopened)->bind_program_transitions(fixture.artifact);
    const auto commands =
        transitions->load_javascript_commands(fixture.artifact.owner_scope(), "run-command");
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands.front().serialize_canonical(), expected);
    EXPECT_TRUE(
        transitions->load_javascript_commands(fixture.artifact.owner_scope(), "run-command", 1)
            .empty());
    EXPECT_TRUE(transitions->load_javascript_commands("tenant-other", "run-command").empty());
}
