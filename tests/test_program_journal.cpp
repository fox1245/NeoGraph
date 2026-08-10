#include <neograph/program/journal.h>

#include <gtest/gtest.h>

#include <barrier>
#include <future>
#include <string>
#include <utility>

namespace {

using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

RunBudget remaining_budget() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};
}

RunBudget empty_budget() {
    return RunBudget{};
}

CoreCheckpointIdentity checkpoint() {
    return CoreCheckpointIdentity{"main", digest('3'), "core-thread", "checkpoint-1", 1};
}

ProgramJournalRecord start_record() {
    return ProgramJournalRecord::create(
        ProgramJournalRecordData{{},
                                 "run-1",
                                 digest('1'),
                                 digest('2'),
                                 1,
                                 ProgramContinuation{"root", ContinuationState::Running, 1},
                                 remaining_budget(),
                                 empty_budget(),
                                 std::nullopt,
                                 10});
}

ProgramJournalRecord terminal_record(const ProgramJournalRecord& previous,
                                     ContinuationState           state,
                                     std::int64_t                timestamp_ms) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.id, previous.run_id, previous.program_version_id, previous.bundle_id,
        previous.sequence + 1, ProgramContinuation{"root", state, 1}, previous.remaining_budget,
        empty_budget(), checkpoint(), timestamp_ms});
}

}  // namespace

TEST(ProgramJournalTest, StoredValueRoundTripsAndRejectsMutationAfterSealing) {
    const auto record = start_record();
    const auto stored = record.serialize_canonical();
    const auto parsed = ProgramJournalRecord::parse(stored);

    EXPECT_EQ(parsed.id, record.id);
    EXPECT_EQ(parsed.serialize_canonical(), stored);
    EXPECT_EQ(parsed.continuation, record.continuation);
    EXPECT_EQ(parsed.remaining_budget.wall_time_ms, record.remaining_budget.wall_time_ms);

    auto mutated                 = record;
    mutated.continuation.attempt = 2;
    EXPECT_THROW((void)mutated.serialize_canonical(), std::invalid_argument);

    InMemoryProgramJournal journal;
    EXPECT_EQ(journal.compare_append({}, record), JournalAppendResult::Appended);
    EXPECT_EQ(journal.compare_append({}, record), JournalAppendResult::AlreadyPresent);
    EXPECT_EQ(journal.compare_append(digest('9'), record), JournalAppendResult::Conflict);
    EXPECT_EQ(journal.compare_append(record.id, std::move(mutated)), JournalAppendResult::Conflict);
}

TEST(ProgramJournalTest, ConcurrentCompareAppendHasExactlyOneWinner) {
    InMemoryProgramJournal journal;
    const auto             start = start_record();
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);

    const auto   completed   = terminal_record(start, ContinuationState::Completed, 20);
    const auto   interrupted = terminal_record(start, ContinuationState::Interrupted, 21);
    std::barrier ready(3);
    auto         append = [&](ProgramJournalRecord record) {
        ready.arrive_and_wait();
        return journal.compare_append(start.id, std::move(record));
    };

    auto first  = std::async(std::launch::async, [&] { return append(completed); });
    auto second = std::async(std::launch::async, [&] { return append(interrupted); });
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();

    EXPECT_NE(first_result, second_result);
    EXPECT_TRUE((first_result == JournalAppendResult::Appended &&
                 second_result == JournalAppendResult::Conflict) ||
                (first_result == JournalAppendResult::Conflict &&
                 second_result == JournalAppendResult::Appended));
    const auto latest = journal.latest(start.run_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_TRUE(latest->id == completed.id || latest->id == interrupted.id);
}

TEST(ProgramJournalTest, ResumeTransitionPreservesCheckpointAndIncrementsAttempt) {
    InMemoryProgramJournal journal;
    const auto             start       = start_record();
    const auto             interrupted = terminal_record(start, ContinuationState::Interrupted, 20);
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, interrupted), JournalAppendResult::Appended);

    const auto resumed = ProgramJournalRecord::create(ProgramJournalRecordData{
        interrupted.id, interrupted.run_id, interrupted.program_version_id, interrupted.bundle_id,
        3, ProgramContinuation{"root", ContinuationState::Running, 2}, interrupted.remaining_budget,
        empty_budget(), interrupted.core_checkpoint, 30});
    ASSERT_EQ(journal.compare_append(interrupted.id, resumed), JournalAppendResult::Appended);
    EXPECT_EQ(journal.compare_append(start.id, interrupted), JournalAppendResult::Conflict);
    EXPECT_EQ(journal.compare_append(interrupted.id, resumed), JournalAppendResult::AlreadyPresent);

    const auto completed = ProgramJournalRecord::create(ProgramJournalRecordData{
        resumed.id, resumed.run_id, resumed.program_version_id, resumed.bundle_id, 4,
        ProgramContinuation{"root", ContinuationState::Completed, 2}, resumed.remaining_budget,
        empty_budget(), resumed.core_checkpoint, 40});
    EXPECT_EQ(journal.compare_append(resumed.id, completed), JournalAppendResult::Appended);

    const auto latest = journal.latest(start.run_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->id, completed.id);
    EXPECT_EQ(latest->continuation.attempt, 2U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, checkpoint().checkpoint_id);
}

TEST(ProgramJournalTest, RejectsInvalidSequenceBudgetAndCheckpointTransitions) {
    const auto start = start_record();

    auto fully_reserved                   = remaining_budget();
    auto unavailable                      = remaining_budget();
    unavailable.wall_time_ms              = 0;
    fully_reserved.model_tokens           = 0;
    fully_reserved.monetary_microunits    = 0;
    fully_reserved.max_concurrency        = 0;
    fully_reserved.max_program_operations = 0;
    fully_reserved.max_core_steps         = 0;
    fully_reserved.max_dynamic_compiles   = 0;
    fully_reserved.max_child_depth        = 0;
    fully_reserved.max_total_children     = 0;
    EXPECT_NO_THROW((void)ProgramJournalRecord::create(
        ProgramJournalRecordData{{},
                                 start.run_id,
                                 start.program_version_id,
                                 start.bundle_id,
                                 1,
                                 ProgramContinuation{"root", ContinuationState::Running, 1},
                                 unavailable,
                                 fully_reserved,
                                 std::nullopt,
                                 10}));

    EXPECT_THROW((void)ProgramJournalRecord::create(ProgramJournalRecordData{
                     start.id, start.run_id, start.program_version_id, start.bundle_id, 2,
                     ProgramContinuation{"root", ContinuationState::Completed, 1},
                     remaining_budget(), remaining_budget(), checkpoint(), 20}),
                 std::invalid_argument);
    EXPECT_THROW((void)ProgramJournalRecord::create(ProgramJournalRecordData{
                     start.id, start.run_id, start.program_version_id, start.bundle_id, 2,
                     ProgramContinuation{"root", ContinuationState::Completed, 1},
                     remaining_budget(), empty_budget(), std::nullopt, 20}),
                 std::invalid_argument);

    InMemoryProgramJournal journal;
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    const auto skipped_sequence = ProgramJournalRecord::create(
        ProgramJournalRecordData{start.id, start.run_id, start.program_version_id, start.bundle_id,
                                 3, ProgramContinuation{"root", ContinuationState::Completed, 1},
                                 remaining_budget(), empty_budget(), checkpoint(), 20});
    EXPECT_EQ(journal.compare_append(start.id, skipped_sequence), JournalAppendResult::Conflict);

    auto increased_budget = remaining_budget();
    increased_budget.wall_time_ms += 1;
    const auto replenished = ProgramJournalRecord::create(
        ProgramJournalRecordData{start.id, start.run_id, start.program_version_id, start.bundle_id,
                                 2, ProgramContinuation{"root", ContinuationState::Completed, 1},
                                 increased_budget, empty_budget(), checkpoint(), 20});
    EXPECT_EQ(journal.compare_append(start.id, replenished), JournalAppendResult::Conflict);
}
TEST(ProgramJournalTest, ReservationSettlementRefundRequiresMeasuredUsage) {
    InMemoryProgramJournal journal;
    const auto             start = start_record();
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);

    auto available         = remaining_budget();
    available.wall_time_ms = 9000;
    RunBudget reservation;
    reservation.wall_time_ms = 1000;
    const auto pending       = ProgramJournalRecord::create(
        ProgramJournalRecordData{start.id, start.run_id, start.program_version_id, start.bundle_id,
                                 2, ProgramContinuation{"root", ContinuationState::Running, 1},
                                 available, reservation, std::nullopt, 20});
    ASSERT_EQ(journal.compare_append(start.id, pending), JournalAppendResult::Appended);

    auto refunded              = remaining_budget();
    refunded.wall_time_ms      = 9500;
    const auto         settled = ProgramJournalRecord::create(ProgramJournalRecordData{
        pending.id, pending.run_id, pending.program_version_id, pending.bundle_id, 3,
        ProgramContinuation{"root", ContinuationState::Running, 1}, refunded, empty_budget(),
        std::nullopt, 30});
    const ProgramUsage measured{500, 0, 0, 0, 0, 0};
    EXPECT_FALSE(is_valid_program_journal_transition(pending, settled));
    EXPECT_TRUE(is_valid_program_journal_reservation_settlement(pending, settled, measured));
    EXPECT_EQ(journal.compare_append(pending.id, settled), JournalAppendResult::Conflict);

    refunded.wall_time_ms    = 9501;
    const auto over_refunded = ProgramJournalRecord::create(ProgramJournalRecordData{
        pending.id, pending.run_id, pending.program_version_id, pending.bundle_id, 3,
        ProgramContinuation{"root", ContinuationState::Running, 1}, refunded, empty_budget(),
        std::nullopt, 30});
    EXPECT_FALSE(is_valid_program_journal_reservation_settlement(pending, over_refunded, measured));
}
TEST(ProgramJournalTest, ReservationSettlementMayAdvanceToAnotherThreadOfSameCore) {
    auto available         = remaining_budget();
    available.wall_time_ms = 9000;
    RunBudget reservation;
    reservation.wall_time_ms    = 1000;
    const auto first_checkpoint = checkpoint();
    const auto pending          = ProgramJournalRecord::create(
        ProgramJournalRecordData{{},
                                 "run-1",
                                 digest('1'),
                                 digest('2'),
                                 1,
                                 ProgramContinuation{"root", ContinuationState::Running, 1},
                                 available,
                                 reservation,
                                 first_checkpoint,
                                 20});

    auto refunded                  = remaining_budget();
    refunded.wall_time_ms          = 9500;
    auto next_checkpoint           = first_checkpoint;
    next_checkpoint.core_thread_id = "core-thread-2";
    next_checkpoint.checkpoint_id  = "checkpoint-2";
    const auto         settled     = ProgramJournalRecord::create(ProgramJournalRecordData{
        pending.id, pending.run_id, pending.program_version_id, pending.bundle_id, 2,
        ProgramContinuation{"root", ContinuationState::Running, 1}, refunded, empty_budget(),
        next_checkpoint, 30});
    const ProgramUsage measured{500, 0, 0, 0, 0, 0};
    EXPECT_TRUE(is_valid_program_journal_reservation_settlement(pending, settled, measured));

    next_checkpoint.core_generation_id = digest('9');
    const auto wrong_core              = ProgramJournalRecord::create(ProgramJournalRecordData{
        pending.id, pending.run_id, pending.program_version_id, pending.bundle_id, 2,
        ProgramContinuation{"root", ContinuationState::Running, 1}, refunded, empty_budget(),
        next_checkpoint, 30});
    EXPECT_FALSE(is_valid_program_journal_reservation_settlement(pending, wrong_core, measured));
}
TEST(ProgramJournalTest, UnresolvedExternalWorkRetainsDurableReservation) {
    const auto start     = start_record();
    auto       available = start.remaining_budget;
    available.wall_time_ms -= 1000;
    RunBudget reservation;
    reservation.wall_time_ms = 1000;
    const auto interrupted   = ProgramJournalRecord::create(ProgramJournalRecordData{
        start.id, start.run_id, start.program_version_id, start.bundle_id, 2,
        ProgramContinuation{"root", ContinuationState::Interrupted, start.continuation.attempt},
        available, reservation, checkpoint(), 20});
    EXPECT_TRUE(is_valid_program_journal_transition(start, interrupted));

    EXPECT_THROW(
        (void)ProgramJournalRecord::create(ProgramJournalRecordData{
            start.id, start.run_id, start.program_version_id, start.bundle_id, 2,
            ProgramContinuation{"root", ContinuationState::Completed, start.continuation.attempt},
            available, reservation, checkpoint(), 20}),
        std::invalid_argument);
}

TEST(ProgramJournalTest, RejectsDroppingPublishedCheckpointOnResumeTransition) {
    InMemoryProgramJournal journal;
    const auto             start       = start_record();
    const auto             interrupted = terminal_record(start, ContinuationState::Interrupted, 20);
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, interrupted), JournalAppendResult::Appended);

    EXPECT_THROW(
        (void)ProgramJournalRecord::create(ProgramJournalRecordData{
            interrupted.id, interrupted.run_id, interrupted.program_version_id,
            interrupted.bundle_id, 3, ProgramContinuation{"root", ContinuationState::Running, 2},
            interrupted.remaining_budget, empty_budget(), std::nullopt, 30}),
        std::invalid_argument);
}

TEST(ProgramJournalTest, TerminalEventFailureAppendsOneFailedCorrection) {
    InMemoryProgramJournal journal;
    const auto             start     = start_record();
    const auto             completed = terminal_record(start, ContinuationState::Completed, 20);
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, completed), JournalAppendResult::Appended);

    const auto failed = ProgramJournalRecord::create(ProgramJournalRecordData{
        completed.id, completed.run_id, completed.program_version_id, completed.bundle_id, 3,
        ProgramContinuation{"root", ContinuationState::Failed, 1}, completed.remaining_budget,
        empty_budget(), completed.core_checkpoint, 30});
    EXPECT_EQ(journal.compare_append(completed.id, failed), JournalAppendResult::Appended);

    const auto second_failed = ProgramJournalRecord::create(ProgramJournalRecordData{
        failed.id, failed.run_id, failed.program_version_id, failed.bundle_id, 4,
        ProgramContinuation{"root", ContinuationState::Failed, 1}, failed.remaining_budget,
        empty_budget(), failed.core_checkpoint, 40});
    EXPECT_EQ(journal.compare_append(failed.id, second_failed), JournalAppendResult::Conflict);
    const auto latest = journal.latest(start.run_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->id, failed.id);
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
}

TEST(ProgramJournalTest, InterruptedEventFailureCompetesAtomicallyWithResume) {
    InMemoryProgramJournal journal;
    const auto             start       = start_record();
    const auto             interrupted = terminal_record(start, ContinuationState::Interrupted, 20);
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, interrupted), JournalAppendResult::Appended);

    const auto failed  = ProgramJournalRecord::create(ProgramJournalRecordData{
        interrupted.id, interrupted.run_id, interrupted.program_version_id, interrupted.bundle_id,
        3, ProgramContinuation{"root", ContinuationState::Failed, 1}, interrupted.remaining_budget,
        empty_budget(), interrupted.core_checkpoint, 30});
    const auto resumed = ProgramJournalRecord::create(ProgramJournalRecordData{
        interrupted.id, interrupted.run_id, interrupted.program_version_id, interrupted.bundle_id,
        3, ProgramContinuation{"root", ContinuationState::Running, 2}, interrupted.remaining_budget,
        empty_budget(), interrupted.core_checkpoint, 30});

    std::barrier ready(3);
    auto         append = [&](ProgramJournalRecord record) {
        ready.arrive_and_wait();
        return journal.compare_append(interrupted.id, std::move(record));
    };
    auto failure_result = std::async(std::launch::async, append, failed);
    auto resume_result  = std::async(std::launch::async, append, resumed);
    ready.arrive_and_wait();

    const auto failure_append = failure_result.get();
    const auto resume_append  = resume_result.get();
    EXPECT_EQ((failure_append == JournalAppendResult::Appended ? 1 : 0) +
                  (resume_append == JournalAppendResult::Appended ? 1 : 0),
              1);
    EXPECT_EQ((failure_append == JournalAppendResult::Conflict ? 1 : 0) +
                  (resume_append == JournalAppendResult::Conflict ? 1 : 0),
              1);

    const auto latest = journal.latest(start.run_id);
    ASSERT_TRUE(latest.has_value());
    if (failure_append == JournalAppendResult::Appended) {
        EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
        EXPECT_EQ(latest->continuation.attempt, 1U);
    } else {
        EXPECT_EQ(latest->continuation.state, ContinuationState::Running);
        EXPECT_EQ(latest->continuation.attempt, 2U);
    }
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.core_checkpoint->checkpoint_id);
}

TEST(ProgramJournalTest, AmbiguousEffectReconciliationPreservesExactCheckpoint) {
    InMemoryProgramJournal journal;
    const auto start = start_record();
    const auto interrupted = terminal_record(start, ContinuationState::Interrupted, 20);
    const auto ambiguous = ProgramJournalRecord::create(ProgramJournalRecordData{
        interrupted.id,
        interrupted.run_id,
        interrupted.program_version_id,
        interrupted.bundle_id,
        3,
        ProgramContinuation{"root", ContinuationState::AmbiguousEffect, 1},
        interrupted.remaining_budget,
        empty_budget(),
        interrupted.core_checkpoint,
        30});
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, interrupted), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(interrupted.id, ambiguous),
              JournalAppendResult::Appended);

    const auto unknown = ProgramJournalRecord::create(ProgramJournalRecordData{
        ambiguous.id,
        ambiguous.run_id,
        ambiguous.program_version_id,
        ambiguous.bundle_id,
        4,
        ProgramContinuation{"root", ContinuationState::AmbiguousEffect, 1},
        ambiguous.remaining_budget,
        empty_budget(),
        ambiguous.core_checkpoint,
        40});
    ASSERT_EQ(journal.compare_append(ambiguous.id, unknown),
              JournalAppendResult::Appended);

    const auto resumed = ProgramJournalRecord::create(ProgramJournalRecordData{
        unknown.id, unknown.run_id, unknown.program_version_id, unknown.bundle_id, 5,
        ProgramContinuation{"root", ContinuationState::Running, 2}, unknown.remaining_budget,
        empty_budget(), unknown.core_checkpoint, 50});
    EXPECT_EQ(journal.compare_append(unknown.id, resumed),
              JournalAppendResult::Appended);
}

TEST(ProgramJournalTest, InterruptedPendingCancellationIsValid) {
    InMemoryProgramJournal journal;
    const auto start = start_record();
    const auto interrupted =
        terminal_record(start, ContinuationState::Interrupted, 20);
    ASSERT_EQ(journal.compare_append({}, start), JournalAppendResult::Appended);
    ASSERT_EQ(journal.compare_append(start.id, interrupted),
              JournalAppendResult::Appended);
    const auto cancelled = ProgramJournalRecord::create(ProgramJournalRecordData{
        interrupted.id,
        interrupted.run_id,
        interrupted.program_version_id,
        interrupted.bundle_id,
        3,
        ProgramContinuation{"root", ContinuationState::Cancelled, 1},
        interrupted.remaining_budget,
        empty_budget(),
        interrupted.core_checkpoint,
        30});
    EXPECT_EQ(journal.compare_append(interrupted.id, cancelled),
              JournalAppendResult::Appended);
}
