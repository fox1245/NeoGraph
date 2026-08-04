#include <neograph/program/transition_store.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#endif
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <future>
#include <string>

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
    data.binding_fingerprint = digest('3'); data.invocation = {{{"input", 1}}, budget(), "trace-1"};
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
ProgramTransitionPublication terminal_publication(const ProgramTransitionPublication& start,
                                                  ProgramTerminalStatus status,
                                                  ContinuationState state,
                                                  std::int64_t time = 20) {
    const auto cp = checkpoint();
    auto journal = ProgramJournalRecord::create({start.journal_record.id, "run-1", digest('1'),
        digest('2'), 2, {"root", state, 1}, budget(), {}, cp, time});
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
    data.terminal_result = result; data.journal_head = journal.id; data.event_sequence = 2;
    data.created_at_ms = 10; data.updated_at_ms = time;
    return {ProgramRunRecord::create(std::move(data)), journal,
            {event(2, ProgramEventKind::Terminal, ProgramTerminalEvent{status}, time)}, {}};
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
#endif
