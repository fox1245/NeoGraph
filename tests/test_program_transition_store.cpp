#include <neograph/program/transition_store.h>

#include <gtest/gtest.h>

#include <barrier>
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
                   ProgramEventPayload payload, std::int64_t time = 10) {
    ProgramEvent value;
    value.sequence = sequence; value.timestamp_ms = time; value.run_id = "run-1";
    value.program_version_id = digest('1'); value.bundle_id = digest('2');
    value.operation_id = "root"; value.core_generation_id = digest('4');
    value.core_run_id = "thread-1"; value.trace_id = "trace-1"; value.attempt = 1;
    value.kind = kind; value.payload = std::move(payload);
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
