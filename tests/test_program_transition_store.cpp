#include <neograph/program/transition_store.h>
#include "canonical_json.h"
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#include <sqlite3.h>
#endif
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
namespace {
using namespace neograph::program;
std::string digest(char c) { return "sha256:" + std::string(64, c); }
using neograph::json;
RunBudget budget() { return {1000, 100, 100, 1, 1, 10, 0, 0, 0}; }
CoreCheckpointIdentity checkpoint() {
    return {"main", digest('4'), "thread-1", "checkpoint-1", 1};
}
ProgramJournalRecord start_journal() {
    return ProgramJournalRecord::create({{}, "run-1", digest('1'), digest('2'), 1,
        {"root", ContinuationState::Running, 1}, budget(), budget(), std::nullopt, 10});
}
ProgramEvent event(std::uint64_t sequence, ProgramEventKind kind,
                   ProgramEventPayload payload, std::int64_t time = 10,
                   std::uint64_t attempt = 1) {
    ProgramEvent value;
    value.sequence = sequence; value.timestamp_ms = time; value.run_id = "run-1";
    value.program_version_id = digest('1'); value.bundle_id = digest('2');
    value.operation_id = "root"; value.core_generation_id = digest('4');
    value.core_run_id = "thread-1"; value.trace_id = "trace-1";
    value.kind = kind; value.payload = std::move(payload); value.attempt = attempt;
    return ProgramEvent::create(std::move(value));
}
ProgramRunRecord start_run(const ProgramJournalRecord& journal) {
    ProgramRunRecordData data;
    data.owner_scope = "owner-a"; data.run_id = journal.run_id;
    data.program_version_id = journal.program_version_id; data.bundle_id = journal.bundle_id;
    data.binding_fingerprint = digest('3');
    RunInvocation invocation;
    invocation.owner_scope = data.owner_scope;
    invocation.agent_id = "agent-a";
    invocation.program_version_id = data.program_version_id;
    invocation.run_id = data.run_id;
    invocation.budget = budget();
    invocation.input = {{"input", 1}};
    invocation.message_sequence = 1;
    invocation.idempotency_key = "idem-1";
    invocation.correlation_id = "trace-1";
    data.invocation = std::move(invocation);
    data.continuation = journal.continuation; data.remaining_budget = journal.remaining_budget;
    data.journal_head = journal.id; data.event_sequence = 1;
    data.created_at_ms = 10; data.updated_at_ms = 10;
    return ProgramRunRecord::create(std::move(data));
}
ProgramTransitionPublication start_publication() {
    auto journal = start_journal();
    return {start_run(journal), journal,
            {event(1, ProgramEventKind::Started, ProgramStartedEvent{budget()})}, {}};
}

ProgramJavaScriptCommandJournalEntry javascript_command_entry(std::uint64_t sequence,
                                                              bool            completed,
                                                              std::uint64_t ordinal = 1) {
    return ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData{
            sequence,
            digest('2'),
            ordinal,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}),
            digest('8'),
            completed ? std::optional<json>{json{{"status", "completed"}}} : std::nullopt});
}

ProgramTransitionPublication javascript_command_publication(
    const ProgramTransitionPublication& previous,
    ProgramJavaScriptCommandJournalEntry command,
    std::int64_t                         time) {
    const auto old_run = previous.run_record;
    auto       journal = ProgramJournalRecord::create(
        {previous.journal_record.id,
         old_run.run_id(),
         old_run.program_version_id(),
         old_run.bundle_id(),
         previous.journal_record.sequence + 1,
         old_run.continuation(),
         old_run.remaining_budget(),
         {},
         old_run.exact_checkpoint(),
         time});
    ProgramRunRecordData data;
    data.owner_scope                      = old_run.owner_scope();
    data.run_id                           = old_run.run_id();
    data.program_version_id               = old_run.program_version_id();
    data.bundle_id                        = old_run.bundle_id();
    data.binding_fingerprint              = old_run.binding_fingerprint();
    data.invocation                       = old_run.invocation();
    data.child_depth                      = old_run.child_depth();
    data.children                         = old_run.children();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = journal.remaining_budget;
    data.exact_checkpoint                 = old_run.exact_checkpoint();
    data.pending_input                    = old_run.pending_input();
    data.pending_effect                   = old_run.pending_effect();
    data.terminal_result                  = old_run.terminal_result();
    data.fork_receipt                     = old_run.fork_receipt();
    data.fork_source_run_id               = old_run.fork_source_run_id();
    data.fork_source_program_version_id   = old_run.fork_source_program_version_id();
    data.fork_source_checkpoint_id        = old_run.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = old_run.recorded_binding_set_fingerprint();
    data.journal_head                     = journal.id;
    data.event_sequence                   = old_run.event_sequence();
    data.effect_sequence                  = old_run.effect_sequence();
    data.created_at_ms                    = old_run.created_at_ms();
    data.updated_at_ms                    = time;
    return {ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}, std::nullopt,
            {std::move(command)}};
}

void exercise_javascript_command_history(ProgramTransitionStore& store) {
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto pending = javascript_command_publication(
        start, javascript_command_entry(1, false), 20);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);
    const auto out_of_order = javascript_command_publication(
        pending, javascript_command_entry(2, false, 2), 25);
    EXPECT_EQ(store.compare_publish("owner-a", pending.journal_record.id, out_of_order),
              ProgramTransitionPublishResult::Conflict);
    const auto completed = javascript_command_publication(
        pending, javascript_command_entry(2, true), 30);
    ASSERT_EQ(store.compare_publish("owner-a", pending.journal_record.id, completed),
              ProgramTransitionPublishResult::Published);
    const auto duplicate = javascript_command_publication(
        completed, javascript_command_entry(3, true), 40);
    EXPECT_EQ(store.compare_publish("owner-a", completed.journal_record.id, duplicate),
              ProgramTransitionPublishResult::Conflict);
    const auto second_pending = javascript_command_publication(
        completed, javascript_command_entry(3, false, 2), 50);
    ASSERT_EQ(store.compare_publish("owner-a", completed.journal_record.id, second_pending),
              ProgramTransitionPublishResult::Published);
    const auto second_completed = javascript_command_publication(
        second_pending, javascript_command_entry(4, true, 2), 60);
    ASSERT_EQ(store.compare_publish("owner-a", second_pending.journal_record.id,
                                    second_completed),
              ProgramTransitionPublishResult::Published);

    const auto commands = store.load_javascript_commands("owner-a", "run-1", 0);
    ASSERT_EQ(commands.size(), 4U);
    EXPECT_TRUE(commands[0].pending());
    EXPECT_TRUE(commands[1].completed());
    EXPECT_TRUE(commands[2].pending());
    EXPECT_TRUE(commands[3].completed());
    EXPECT_EQ(commands[0].coordinate_id(), commands[1].coordinate_id());
    EXPECT_EQ(commands[2].coordinate_id(), commands[3].coordinate_id());
}

ProgramTransitionPublication terminal_publication(const ProgramTransitionPublication& start,
                                                  ProgramTerminalStatus status,
                                                  ContinuationState state,
                                                  std::int64_t time = 20) {
    const auto cp = checkpoint();
    const auto event_sequence = start.run_record.event_sequence() + 1;
    auto journal = ProgramJournalRecord::create(
        {start.journal_record.id, "run-1", digest('1'), digest('2'),
         start.journal_record.sequence + 1, {"root", state, 1}, budget(), {}, cp, time});
    ProgramResultData outcome;
    outcome.status = status; outcome.run_id = "run-1"; outcome.program_version_id = digest('1');
    outcome.bundle_id = digest('2'); outcome.attempt = 1; outcome.output = {{"ok", true}};
    outcome.remaining_budget = budget(); outcome.checkpoint = cp;
    if (status == ProgramTerminalStatus::Failed) {
        outcome.failure = ProgramFailure{"P_TEST", "failed", "root", "main", 1, json::object()};
    }
    auto result = ProgramResult::create(std::move(outcome));
    ProgramRunRecordData data;
    data.owner_scope = "owner-a"; data.run_id = "run-1"; data.program_version_id = digest('1');
    data.bundle_id = digest('2'); data.binding_fingerprint = digest('3');
    data.invocation = start.run_record.invocation(); data.continuation = journal.continuation;
    data.remaining_budget = journal.remaining_budget; data.exact_checkpoint = cp;
    data.terminal_result = result; data.journal_head = journal.id; data.event_sequence = event_sequence;
    data.created_at_ms = 10; data.updated_at_ms = time;
    return {ProgramRunRecord::create(std::move(data)), journal,
            {event(event_sequence, ProgramEventKind::Terminal, ProgramTerminalEvent{status}, time)}, {}};
}

ProgramTransitionPublication child_metadata_publication(
    const ProgramTransitionPublication& start,
    ProgramChildState                    state = ProgramChildState::Publishing,
    std::int64_t                         time  = 20,
    std::string                          trace = "child-trace") {
    const auto child = ProgramChildRecord{
        "child-run-1",
        digest('7'),
        "link-receipt",
        ProgramPersistedInvocation{{{"child", true}}, budget(), std::move(trace), "run-1", 1},
        state};
    auto journal = ProgramJournalRecord::create(
        {start.journal_record.id,
         "run-1",
         digest('1'),
         digest('2'),
         start.journal_record.sequence + 1,
         {"root", ContinuationState::Running, 1},
         budget(),
         budget(),
         std::nullopt,
         time});
    auto data = ProgramRunRecordData{};
    data.owner_scope         = "owner-a";
    data.run_id              = "run-1";
    data.program_version_id  = digest('1');
    data.bundle_id           = digest('2');
    data.binding_fingerprint = digest('3');
    data.invocation          = start.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.children            = {child};
    data.journal_head        = journal.id;
    data.event_sequence      = start.run_record.event_sequence();
    data.effect_sequence     = start.run_record.effect_sequence();
    data.created_at_ms        = start.run_record.created_at_ms();
    data.updated_at_ms        = time;
    return {ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
}

ProgramPendingEffect pending_effect(std::string effect_id = "effect-one") {
    ProgramPendingEffectData data;
    data.operation_id         = "root";
    data.call_id              = "effect-call";
    data.effect_id            = std::move(effect_id);
    data.result_schema        = {{"type", "object"}, {"additionalProperties", true}};
    data.payload              = {{"kind", "publish"}};
    data.expires_at_unix_ms   = 5000;
    data.effect_mode          = EffectMode::Brokered;
    data.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    data.core_node            = "main";
    data.core_interrupt_value = {{"value", "effect"}};
    return ProgramPendingEffect(std::move(data));
}

ProgramTransitionPublication interrupted_effect_publication(
    const ProgramTransitionPublication& start) {
    auto pending = pending_effect();
    const auto cp = checkpoint();
    auto journal = ProgramJournalRecord::create(
        {start.journal_record.id, "run-1", digest('1'), digest('2'), 2,
         {"root", ContinuationState::Interrupted, 1}, budget(), {}, cp, 20});
    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = "run-1";
    result_data.program_version_id = digest('1');
    result_data.bundle_id          = digest('2');
    result_data.attempt            = 1;
    result_data.output             = json::object();
    result_data.remaining_budget   = budget();
    result_data.checkpoint         = cp;
    result_data.interrupt =
        ProgramInterrupt{"main", pending.core_interrupt_value(), std::nullopt, pending};

    ProgramRunRecordData data;
    data.owner_scope         = "owner-a";
    data.run_id              = "run-1";
    data.program_version_id  = digest('1');
    data.bundle_id           = digest('2');
    data.binding_fingerprint = digest('3');
    data.invocation          = start.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.pending_effect      = pending;
    data.terminal_result     = ProgramResult::create(std::move(result_data));
    data.journal_head        = journal.id;
    data.event_sequence      = 2;
    data.effect_sequence     = 1;
    data.created_at_ms       = start.run_record.created_at_ms();
    data.updated_at_ms       = 20;
    return {ProgramRunRecord::create(std::move(data)),
            journal,
            {event(2, ProgramEventKind::Terminal,
                   ProgramTerminalEvent{ProgramTerminalStatus::Interrupted})},
            {{1, std::move(pending)}}};
}
ProgramTransitionPublication resumed_effect_publication(
    const ProgramTransitionPublication& interrupted) {
    const auto old_run = interrupted.run_record;
    const auto pending = *old_run.pending_effect();
    const auto consumed = pending.submit(pending.call_id(), pending.effect_id(),
                                         json{{"result", "recorded"}}, 30)
                               .value;
    const auto cp = checkpoint();
    auto journal = ProgramJournalRecord::create(
        {interrupted.journal_record.id, "run-1", digest('1'), digest('2'), 3,
         {"root", ContinuationState::Running, 2}, budget(), budget(), cp, 30});
    ProgramRunRecordData data;
    data.owner_scope         = old_run.owner_scope();
    data.run_id              = old_run.run_id();
    data.program_version_id  = old_run.program_version_id();
    data.bundle_id           = old_run.bundle_id();
    data.binding_fingerprint = old_run.binding_fingerprint();
    data.invocation          = old_run.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.pending_effect      = consumed;
    data.journal_head        = journal.id;
    data.event_sequence      = 3;
    data.effect_sequence     = 1;
    data.created_at_ms       = old_run.created_at_ms();
    data.updated_at_ms       = 30;
    return {ProgramRunRecord::create(std::move(data)), std::move(journal),
            {event(3, ProgramEventKind::Started, ProgramStartedEvent{budget()}, 30, 2)}, {}};
}
ForkCompatibilityReceipt compatible_fork_receipt() {
    return ForkCompatibilityReceipt(ForkCompatibilityReceiptData{
        "owner-a", "run-1", digest('6'), "checkpoint-1", digest('1'),
        ForkCompatibilityStatus::Compatible, {}});
}

void attach_fork_receipt(ProgramTransitionPublication& publication) {
    const auto run = publication.run_record;
    ProgramRunRecordData data;
    data.owner_scope = run.owner_scope();
    data.run_id = run.run_id();
    data.program_version_id = run.program_version_id();
    data.bundle_id = run.bundle_id();
    data.binding_fingerprint = run.binding_fingerprint();
    data.invocation = run.invocation();
    data.continuation = run.continuation();
    data.remaining_budget = run.remaining_budget();
    data.exact_checkpoint = run.exact_checkpoint();
    data.pending_input = run.pending_input();
    data.pending_effect = run.pending_effect();
    data.terminal_result = run.terminal_result();
    data.fork_receipt = compatible_fork_receipt();
    data.children = run.children();
    data.journal_head = run.journal_head();
    data.recorded_binding_set_fingerprint = run.recorded_binding_set_fingerprint();
    data.event_sequence = run.event_sequence();
    data.effect_sequence = run.effect_sequence();
    data.created_at_ms = run.created_at_ms();
    data.updated_at_ms = run.updated_at_ms();
    publication.run_record = ProgramRunRecord::create(std::move(data));
}
json publication_reference_body(const ProgramTransitionPublication& publication) {
    json events = json::array();
    for (const auto& event : publication.events) {
        events.push_back(detail::parse_json_strict(event.serialize_canonical()));
    }

    json effects = json::array();
    for (const auto& effect : publication.effects) {
        effects.push_back(detail::parse_json_strict(effect.serialize_canonical()));
    }

    return {{"format", "neograph-program-transition-publication"},
            {"storage_schema_version", 1},
            {"run_record",
             detail::parse_json_strict(publication.run_record.serialize_canonical())},
            {"journal_record",
             detail::parse_json_strict(publication.journal_record.serialize_canonical())},
            {"events", std::move(events)},
            {"effects", std::move(effects)},
            {"commands", json::array()},
            {"migration_plan",
             publication.migration_plan
                 ? detail::parse_json_strict(publication.migration_plan->serialize_canonical())
                 : json(nullptr)}};
}
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
class TestSqliteDatabase final {
public:
    explicit TestSqliteDatabase(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "SQLite unavailable";
            if (db_) sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error("test SQLite open: " + error);
        }
    }

    TestSqliteDatabase(const TestSqliteDatabase&) = delete;
    TestSqliteDatabase& operator=(const TestSqliteDatabase&) = delete;

    ~TestSqliteDatabase() {
        if (db_) sqlite3_close_v2(db_);
    }

    void execute(std::string_view sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.data(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("test SQLite execute: " + message);
        }
    }

    std::int64_t scalar(std::string_view sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr) !=
            SQLITE_OK)
            throw std::runtime_error("test SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
        const int result = sqlite3_step(statement);
        if (result != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQLite scalar: " + error);
        }
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    void insert_legacy(std::string_view owner_scope,
                       std::string_view run_id,
                       std::string_view canonical_bytes,
                       std::string_view last_publication_bytes) {
        sqlite3_stmt* statement = nullptr;
        constexpr std::string_view sql =
            "INSERT INTO program_transition_runs"
            "(owner_scope, run_id, canonical_bytes, last_publication_bytes) "
            "VALUES(?1, ?2, ?3, ?4)";
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr) !=
            SQLITE_OK)
            throw std::runtime_error("test SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
        const auto bind = [&](int index, std::string_view value, bool blob) {
            const int result = blob
                                   ? sqlite3_bind_blob(statement, index, value.data(),
                                                       static_cast<int>(value.size()), SQLITE_TRANSIENT)
                                   : sqlite3_bind_text(statement, index, value.data(),
                                                       static_cast<int>(value.size()), SQLITE_TRANSIENT);
            if (result != SQLITE_OK) throw std::runtime_error("test SQLite bind");
        };
        try {
            bind(1, owner_scope, false);
            bind(2, run_id, false);
            bind(3, canonical_bytes, true);
            bind(4, last_publication_bytes, true);
            if (sqlite3_step(statement) != SQLITE_DONE)
                throw std::runtime_error("test SQLite insert: " + std::string(sqlite3_errmsg(db_)));
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        sqlite3_finalize(statement);
    }

private:
    sqlite3* db_ = nullptr;
};
#endif

} // namespace

TEST(ProgramTransitionStoreTest, CanonicalValuesRejectTamper) {
    auto publication = terminal_publication(start_publication(), ProgramTerminalStatus::Completed,
                                            ContinuationState::Completed);
    auto result_bytes = publication.run_record.terminal_result()->serialize_canonical();
    result_bytes.replace(result_bytes.find("completed"), 9, "cancelled");
    EXPECT_THROW((void)ProgramResult::parse(result_bytes), std::invalid_argument);
    auto event_bytes = publication.events.front().serialize_canonical();
    event_bytes.replace(event_bytes.find("run-1"), 5, "run-2");
    EXPECT_THROW((void)ProgramEvent::parse(event_bytes), std::invalid_argument);
    auto run_bytes = publication.run_record.serialize_canonical();
    run_bytes.replace(run_bytes.find("owner-a"), 7, "owner-b");
    EXPECT_THROW((void)ProgramRunRecord::parse(run_bytes), std::invalid_argument);
}

TEST(ProgramTransitionStoreTest, PublicationCanonicalWireMatchesReferenceTree) {
    const auto start = start_publication();
    const auto publication = interrupted_effect_publication(start);

    EXPECT_EQ(publication.serialize_canonical(),
              detail::canonical_json_bytes(publication_reference_body(publication)));
}

TEST(ProgramTransitionStoreTest, InMemoryJavaScriptCommandHistoryIsAppendOnly) {
    InMemoryProgramTransitionStore store;
    exercise_javascript_command_history(store);
}

TEST(ProgramTransitionStoreTest, FirstPublishRetryAndOwnerIsolation) {
    InMemoryProgramTransitionStore store; auto publication = start_publication();
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::AlreadyPresent);
    ASSERT_TRUE(store.load("owner-a", "run-1"));
    EXPECT_FALSE(store.load("owner-b", "run-1"));
    EXPECT_FALSE(store.latest("owner-b", "run-1"));
    EXPECT_TRUE(store.load_events("owner-b", "run-1").empty());
}

TEST(ProgramTransitionStoreTest, MigrationPublicationIsDurableAndInheritedAcrossReplay) {
    InMemoryProgramTransitionStore store;
    auto publication = start_publication();
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(
        MigrationPlanData{digest('6'), digest('1'), "owner-a",
                          MigrationCompatibility::ForkCompatible, {}, {}, {}});
    ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    const auto stored = store.load_migration_plan("owner-a", "run-1");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->id(), publication.migration_plan->id());

    const auto bytes = publication.serialize_canonical();
    const auto reparsed = ProgramTransitionPublication::parse(bytes);
    ASSERT_TRUE(reparsed.migration_plan.has_value());
    EXPECT_EQ(reparsed.migration_plan->id(), publication.migration_plan->id());
}

TEST(ProgramTransitionStoreTest, ForkPublicationRequiresDurableMigrationProof) {
    InMemoryProgramTransitionStore store;
    auto publication = start_publication();
    attach_fork_receipt(publication);
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-1").has_value());
}

TEST(ProgramTransitionStoreTest, NonForkMigrationPlanCannotPublishForkLineage) {
    InMemoryProgramTransitionStore store;
    auto publication = start_publication();
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(
        MigrationPlanData{digest('6'), digest('1'), "owner-a",
                          MigrationCompatibility::Blocked,
                          {"authority_profile"},
                          {MigrationDiagnostic{MigrationDimension::Authority,
                                               "authority_profile",
                                               "authority changed",
                                               json{{"profile", "source"}},
                                               json{{"profile", "target"}}}},
                          {}});
    EXPECT_EQ(store.compare_publish("owner-a", {}, std::move(publication)),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-1").has_value());
}

TEST(ProgramTransitionStoreTest, EffectOutboxMustBindExactAwaitingPendingEffect) {
    InMemoryProgramTransitionStore store;
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto interrupted = interrupted_effect_publication(start);
    interrupted.effects = {ProgramEffectOutboxEntry(1, pending_effect("different-effect"))};
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, start.journal_record.id);
    EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());
}

TEST(ProgramTransitionStoreTest, ConflictingCasAndInvalidPublicationRollBack) {
    InMemoryProgramTransitionStore store; auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed);
    EXPECT_EQ(store.compare_publish("owner-a", digest('9'), terminal),
              ProgramTransitionPublishResult::Conflict);

    auto bad_event = terminal;
    bad_event.events.front().sequence = 7;
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, bad_event),
              ProgramTransitionPublishResult::Conflict);
    auto latest = store.latest("owner-a", "run-1");
    ASSERT_TRUE(latest); EXPECT_EQ(latest->id, start.journal_record.id);
    EXPECT_FALSE(store.load("owner-a", "run-1")->terminal_result());
}

TEST(ProgramTransitionStoreTest, ExactTerminalRetryIsAlreadyPresent) {
    InMemoryProgramTransitionStore store;
    auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal = terminal_publication(
        start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
    ASSERT_EQ(store.compare_publish(
                  "owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish(
                  "owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::AlreadyPresent);
}

TEST(ProgramTransitionStoreTest, InMemoryHistoryRetainsOrderedEventAndEffectHistory) {
    InMemoryProgramTransitionStore store;
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    const auto interrupted = interrupted_effect_publication(start);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Published);

    const auto resumed = resumed_effect_publication(interrupted);
    ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
              ProgramTransitionPublishResult::Published);

    const auto events = store.load_events("owner-a", "run-1");
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].sequence, 1U);
    EXPECT_EQ(events[1].sequence, 2U);
    EXPECT_EQ(events[2].sequence, 3U);

    const auto after_first = store.load_events("owner-a", "run-1", 1);
    ASSERT_EQ(after_first.size(), 2U);
    EXPECT_EQ(after_first.front().sequence, 2U);
    EXPECT_EQ(after_first.back().sequence, 3U);

    const auto effects = store.load_effects("owner-a", "run-1");
    ASSERT_EQ(effects.size(), 1U);
    EXPECT_EQ(effects.front().sequence(), 1U);
    EXPECT_TRUE(store.load_effects("owner-a", "run-1", 1).empty());
}

TEST(ProgramTransitionStoreTest, FaultAtEachPublishBoundaryPreservesCommittedState) {
    const std::array faults{
        ProgramTransitionFaultPoint::AfterRunSnapshot,
        ProgramTransitionFaultPoint::AfterJournalSnapshot,
        ProgramTransitionFaultPoint::AfterEventSnapshot,
        ProgramTransitionFaultPoint::AfterEffectSnapshot,
        ProgramTransitionFaultPoint::BeforeCommit,
    };

    for (const auto fault : faults) {
        InMemoryProgramTransitionStore store;
        auto start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed);
        store.fail_next_publication_for_testing(fault);
        EXPECT_THROW(
            store.compare_publish("owner-a", start.journal_record.id, std::move(terminal)),
            std::runtime_error);

        const auto latest = store.latest("owner-a", "run-1");
        ASSERT_TRUE(latest);
        EXPECT_EQ(latest->id, start.journal_record.id);

        EXPECT_FALSE(store.load("owner-a", "run-1")->terminal_result());
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1u);
        EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());

        EXPECT_EQ(store.compare_publish(
                      "owner-a", start.journal_record.id,
                      terminal_publication(start, ProgramTerminalStatus::Completed,
                                           ContinuationState::Completed)),
                  ProgramTransitionPublishResult::Published);
    }
}
TEST(ProgramTransitionStoreTest, EffectPublicationFaultsPreserveSourceAndOutbox) {
    const std::array faults{
        ProgramTransitionFaultPoint::AfterRunSnapshot,
        ProgramTransitionFaultPoint::AfterJournalSnapshot,
        ProgramTransitionFaultPoint::AfterEventSnapshot,
        ProgramTransitionFaultPoint::AfterEffectSnapshot,
        ProgramTransitionFaultPoint::BeforeCommit,
    };
    for (const auto fault : faults) {
        InMemoryProgramTransitionStore store;
        const auto start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto interrupted = interrupted_effect_publication(start);
        store.fail_next_publication_for_testing(fault);
        EXPECT_THROW(
            store.compare_publish("owner-a", start.journal_record.id, interrupted),
            std::runtime_error);

        const auto source = store.load("owner-a", "run-1");
        ASSERT_TRUE(source.has_value());
        EXPECT_EQ(source->serialize_canonical(), start.run_record.serialize_canonical());
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, start.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
        EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());

        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
}

TEST(ProgramTransitionStoreTest, ConcurrentCasHasOneWinner) {
    InMemoryProgramTransitionStore store; auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto completed = terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed, 20);
    auto failed = terminal_publication(start, ProgramTerminalStatus::Failed,
                                       ContinuationState::Failed, 21);
    std::barrier ready(3);
    auto publish = [&](ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return store.compare_publish("owner-a", start.journal_record.id, std::move(value));
    };
    auto a = std::async(std::launch::async, [&] { return publish(completed); });
    auto b = std::async(std::launch::async, [&] { return publish(failed); });
    ready.arrive_and_wait(); auto ar = a.get(); auto br = b.get();
    EXPECT_NE(ar, br);
    EXPECT_TRUE((ar == ProgramTransitionPublishResult::Published &&
                 br == ProgramTransitionPublishResult::Conflict) ||
                (br == ProgramTransitionPublishResult::Published &&
                 ar == ProgramTransitionPublishResult::Conflict));
}

TEST(ProgramTransitionStoreTest, TerminalResultCannotPublishWithoutTerminalOutbox) {
    InMemoryProgramTransitionStore store; auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed);
    terminal.events.clear();
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Conflict);
    auto visible = store.load("owner-a", "run-1");

    ASSERT_TRUE(visible); EXPECT_FALSE(visible->terminal_result());
    EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
}

TEST(ProgramTransitionStoreTest, ChildMetadataPublicationIsIdempotentAndConflictDetecting) {
    InMemoryProgramTransitionStore store;
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    const auto publishing = child_metadata_publication(start);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, publishing),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, publishing),
              ProgramTransitionPublishResult::AlreadyPresent);
    ASSERT_TRUE(store.load("owner-a", "run-1").has_value());
    ASSERT_EQ(store.load("owner-a", "run-1")->children().size(), 1U);

    const auto conflicting =
        child_metadata_publication(publishing, ProgramChildState::Publishing, 21,
                                   "different-child-trace");
    EXPECT_EQ(store.compare_publish("owner-a", publishing.journal_record.id, conflicting),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, publishing.journal_record.id);
}
TEST(ProgramTransitionStoreTest, TerminalChildStatesRequireMatchingDurableResults) {
    const auto start = start_publication();
    EXPECT_THROW(
        (void)child_metadata_publication(start, ProgramChildState::Completed),
        std::invalid_argument);
    EXPECT_THROW(
        (void)child_metadata_publication(start, ProgramChildState::Failed),
        std::invalid_argument);
}
TEST(ProgramTransitionStoreTest, TerminalChildResultSurvivesRecordRoundTrip) {
    const auto start = start_publication();
    auto publishing = child_metadata_publication(start, ProgramChildState::Dispatched);
    auto children = publishing.run_record.children();
    ASSERT_EQ(children.size(), 1U);

    const auto child_id = children.front().child_run_id;
    ProgramResultData child_result_data;
    child_result_data.status             = ProgramTerminalStatus::Completed;
    child_result_data.run_id              = child_id;
    child_result_data.program_version_id = digest('1');
    child_result_data.bundle_id          = digest('2');
    child_result_data.operation_id       = "root";
    child_result_data.attempt            = 1;
    child_result_data.output             = json{{"child", true}};
    child_result_data.remaining_budget   = budget();
    child_result_data.checkpoint         = checkpoint();
    children.front().state = ProgramChildState::Completed;
    children.front().terminal_result = ProgramResult::create(std::move(child_result_data));

    const auto& run = publishing.run_record;
    ProgramRunRecordData data;
    data.owner_scope         = run.owner_scope();
    data.run_id              = run.run_id();
    data.program_version_id  = run.program_version_id();
    data.bundle_id           = run.bundle_id();
    data.binding_fingerprint = run.binding_fingerprint();
    data.invocation          = run.invocation();
    data.continuation        = run.continuation();
    data.remaining_budget    = run.remaining_budget();
    data.children            = std::move(children);
    data.journal_head        = run.journal_head();
    data.event_sequence      = run.event_sequence();
    data.effect_sequence     = run.effect_sequence();
    data.created_at_ms       = run.created_at_ms();
    data.updated_at_ms       = run.updated_at_ms();
    const auto parsed = ProgramRunRecord::parse(
        ProgramRunRecord::create(std::move(data)).serialize_canonical());
    ASSERT_TRUE(parsed.children().front().terminal_result.has_value());
    EXPECT_EQ(parsed.children().front().state, ProgramChildState::Completed);
    EXPECT_EQ(parsed.children().front().terminal_result->status(),
              ProgramTerminalStatus::Completed);
    EXPECT_EQ(parsed.children().front().terminal_result->run_id(), child_id);
}
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramTransitionStoreTest, SQLiteReopensAtomicPublicationAndOwnerIsolation) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    {
        SQLiteProgramTransitionStore store(path);
        const auto start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);

        const auto terminal = terminal_publication(
            start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
                  ProgramTransitionPublishResult::AlreadyPresent);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 2U);
        EXPECT_TRUE(store.load("owner-b", "run-1") == std::nullopt);
    }

    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_TRUE(run->terminal_result().has_value());
        EXPECT_EQ(run->terminal_result()->status(), ProgramTerminalStatus::Completed);
        ASSERT_TRUE(store.latest("owner-a", "run-1").has_value());
        EXPECT_EQ(store.load_events("owner-a", "run-1", 1).size(), 1U);
    }

    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteJavaScriptCommandHistorySurvivesReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-javascript-command-" +
          std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {
        SQLiteProgramTransitionStore store(path);
        exercise_javascript_command_history(store);
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto commands = store.load_javascript_commands("owner-a", "run-1", 0);
        ASSERT_EQ(commands.size(), 2U);
        EXPECT_TRUE(commands.front().pending());
        EXPECT_TRUE(commands.back().completed());
        EXPECT_EQ(commands.front().coordinate_id(), commands.back().coordinate_id());
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteReopensDurableMigrationProof) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-migration-proof-" +
          std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    auto publication = start_publication();
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(
        MigrationPlanData{digest('6'), digest('1'), "owner-a",
                          MigrationCompatibility::ForkCompatible, {}, {}, {}});
    const auto expected_id = publication.migration_plan->id();
    {
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
                  ProgramTransitionPublishResult::Published);
        ASSERT_TRUE(store.load_migration_plan("owner-a", "run-1").has_value());
        EXPECT_EQ(store.load_migration_plan("owner-a", "run-1")->id(), expected_id);
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_TRUE(run->fork_receipt().has_value());
        const auto proof = store.load_migration_plan("owner-a", "run-1");
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->id(), expected_id);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteEffectOutboxSurvivesResumePublication) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-effect-history-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto interrupted = interrupted_effect_publication(start);
    const auto resumed = resumed_effect_publication(interrupted);
    EXPECT_NO_THROW((void)resumed.serialize_canonical());
    EXPECT_TRUE(is_valid_program_journal_transition(interrupted.journal_record,
                                                    resumed.journal_record));
    auto aggregate_events = interrupted.events;
    aggregate_events.insert(aggregate_events.end(), resumed.events.begin(), resumed.events.end());
    auto aggregate_effects = interrupted.effects;
    aggregate_effects.insert(aggregate_effects.end(), resumed.effects.begin(), resumed.effects.end());
    const ProgramTransitionPublication aggregate{
        resumed.run_record, resumed.journal_record, std::move(aggregate_events),
        std::move(aggregate_effects), std::nullopt};
    EXPECT_NO_THROW((void)aggregate.serialize_canonical());
    {
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    {
        SQLiteProgramTransitionStore store(path);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
        ASSERT_TRUE(store.load("owner-a", "run-1").has_value());
        EXPECT_EQ(store.load("owner-a", "run-1")->continuation().state,
                  ContinuationState::Running);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteChildMetadataPublicationSurvivesReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-child-transition-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto child = child_metadata_publication(start);
    {
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_EQ(run->children().size(), 1U);
        EXPECT_EQ(run->children().front().child_run_id, "child-run-1");
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, child.journal_record.id);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteStoresTransitionDeltasAndReopens) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-delta-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto child = child_metadata_publication(start);
    {
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_run_heads_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_event_log_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_effect_log_v2"), 0);
        EXPECT_EQ(database.scalar(
                      "SELECT COUNT(*) FROM sqlite_master "
                      "WHERE type = 'table' AND name = 'program_transition_runs'"),
                  0);
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_FALSE(run->terminal_result().has_value());
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, child.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
        EXPECT_TRUE(store.load_events("owner-a", "run-1", 1).empty());
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteMigratesLegacySnapshotToDeltaLog) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-legacy-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto interrupted = interrupted_effect_publication(start);
    const auto resumed = resumed_effect_publication(interrupted);
    auto legacy_events = start.events;
    legacy_events.insert(legacy_events.end(), interrupted.events.begin(), interrupted.events.end());
    const ProgramTransitionPublication legacy{interrupted.run_record, interrupted.journal_record,
                                               std::move(legacy_events), interrupted.effects, std::nullopt};
    {
        TestSqliteDatabase database(path);
        database.execute("CREATE TABLE program_transition_runs ("
                         "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
                         "canonical_bytes BLOB NOT NULL, last_publication_bytes BLOB NOT NULL, "
                         "PRIMARY KEY(owner_scope, run_id))");
        database.insert_legacy("owner-a", "run-1", legacy.serialize_canonical(),
                               interrupted.serialize_canonical());
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->continuation().state, ContinuationState::Interrupted);
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, interrupted.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 2U);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::AlreadyPresent);
        EXPECT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_run_heads_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_event_log_v2"), 3);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_effect_log_v2"), 1);
    }
    {
        SQLiteProgramTransitionStore store(path);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->continuation().state, ContinuationState::Running);
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, resumed.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1", 2).size(), 1U);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteConcurrentCasAcrossConnectionsHasOneWinner) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-cas-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {

    SQLiteProgramTransitionStore first(path);
    SQLiteProgramTransitionStore second(path);
    const auto start = start_publication();
    ASSERT_EQ(first.compare_publish("owner-a", {}, start), ProgramTransitionPublishResult::Published);
    const auto completed = terminal_publication(
        start, ProgramTerminalStatus::Completed, ContinuationState::Completed, 20);
    const auto failed =
        terminal_publication(start, ProgramTerminalStatus::Failed, ContinuationState::Failed, 21);
    std::barrier ready(3);
    auto publish = [&](SQLiteProgramTransitionStore& store, ProgramTransitionPublication publication) {
        ready.arrive_and_wait();
        return store.compare_publish("owner-a", start.journal_record.id, std::move(publication));
    };
    auto a = std::async(std::launch::async, [&] { return publish(first, completed); });
    auto b = std::async(std::launch::async, [&] { return publish(second, failed); });
    ready.arrive_and_wait();
    const auto a_result = a.get();
    const auto b_result = b.get();
    EXPECT_TRUE((a_result == ProgramTransitionPublishResult::Published &&
                 b_result == ProgramTransitionPublishResult::Conflict) ||
                (b_result == ProgramTransitionPublishResult::Published &&
                 a_result == ProgramTransitionPublishResult::Conflict));

    const auto run = first.load("owner-a", "run-1");
    ASSERT_TRUE(run.has_value());
    ASSERT_TRUE(run->terminal_result().has_value());
    EXPECT_TRUE(run->terminal_result()->status() == ProgramTerminalStatus::Completed ||
                run->terminal_result()->status() == ProgramTerminalStatus::Failed);
    }
    std::filesystem::remove(path);
}
#endif
