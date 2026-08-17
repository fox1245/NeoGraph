#include <neograph/program/lineage.h>

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace {

using namespace neograph::program;
using neograph::json;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

RunBudget budget() {
    return {1000, 900, 800, 7, 600, 500, 40, 3, 20};
}

ProgramRunGeneration generation_one() {
    return ProgramRunGeneration::create(ProgramRunGenerationData{
        "owner-a", program_run_lineage_id("owner-a", "run-1"), 1, "run-1", digest('1'),
        digest('2'), digest('3'), digest('4'), std::nullopt, 10, 0});
}

ProgramRunLineage initial_lineage(const ProgramRunGeneration& generation) {
    return ProgramRunLineage::create(ProgramRunLineageData{generation.owner_scope(),
                                                           generation.lineage_id(),
                                                           generation.run_id(),
                                                           1,
                                                           generation.id(),
                                                           generation.initial_run_record_id(),
                                                           generation.initial_journal_head(),
                                                           budget(),
                                                           {},
                                                           std::nullopt,
                                                           generation.created_at_ms(),
                                                           generation.created_at_ms()});
}

ProgramReplacementReceipt replacement_receipt(const ProgramRunGeneration& source,
                                              std::string source_generation_id = {}) {
    return ProgramReplacementReceipt(ProgramReplacementReceiptData{
        source.owner_scope(), source.lineage_id(), source.generation(),
        source_generation_id.empty() ? source.id() : std::move(source_generation_id), digest('a'),
        source.run_id(), digest('b'), digest('c'), source.program_version_id(), source.bundle_id(),
        digest('d'), digest('e'), digest('f'), source.generation() + 1, "run-2", digest('7'),
        digest('8'), digest('9'), digest('a'), digest('b'), digest('9'), digest('b')});
}

ProgramRuntimeStateTransferReceipt transfer_receipt(const ProgramRunGeneration& source) {
    return ProgramRuntimeStateTransferReceipt(ProgramRuntimeStateTransferReceiptData{
        source.owner_scope(), source.lineage_id(), source.id(), digest('a'), source.run_id(), digest('b'),
        digest('c'), source.generation() + 1, "run-2", digest('9'), digest('b'),
        {digest('d'), digest('e')}, digest('f'), {{digest('1'), digest('2'), digest('3')}}});
}

ProgramRunLineage next_head(const ProgramRunLineage& previous,
                            RunBudget                remaining,
                            RunBudget                reserved  = {},
                            RunBudget                committed = {}) {
    return ProgramRunLineage::create(
        ProgramRunLineageData{previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
                               previous.active_generation(), previous.active_generation_id(),
                               digest('5'), digest('6'), remaining, reserved, previous.id(),
                               previous.created_at_ms(), previous.updated_at_ms() + 1, committed});
}

TEST(ProgramRunLineageTest, StableIdentityIsOwnerScopedAndDeterministic) {
    const auto first = program_run_lineage_id("owner-a", "run-1");
    EXPECT_EQ(first, program_run_lineage_id("owner-a", "run-1"));
    EXPECT_NE(first, program_run_lineage_id("owner-b", "run-1"));
    EXPECT_NE(first, program_run_lineage_id("owner-a", "run-2"));
    EXPECT_THROW((void)program_run_lineage_id("", "run-1"), std::invalid_argument);
    EXPECT_THROW((void)program_run_lineage_id("owner-a", " bad-run"), std::invalid_argument);
}

TEST(ProgramRunLineageTest, CanonicalValuesRoundTripAndRejectTamper) {
    const auto generation = generation_one();
    const auto lineage    = initial_lineage(generation);

    EXPECT_EQ(ProgramRunGeneration::parse(generation.serialize_canonical()).id(), generation.id());
    EXPECT_EQ(ProgramRunLineage::parse(lineage.serialize_canonical()).id(), lineage.id());

    auto generation_bytes = generation.serialize_canonical();
    generation_bytes.replace(generation_bytes.find("run-1"), 5, "run-2");
    EXPECT_THROW((void)ProgramRunGeneration::parse(generation_bytes), std::invalid_argument);

    auto lineage_bytes = lineage.serialize_canonical();
    lineage_bytes.replace(lineage_bytes.find("owner-a"), 7, "owner-b");
    EXPECT_THROW((void)ProgramRunLineage::parse(lineage_bytes), std::invalid_argument);
}

TEST(ProgramRunLineageTest, LegacyGenerationRoundTripPreservesStoredIdentity) {
    const std::string bytes =
        R"({"bundle_id":"sha256:2222222222222222222222222222222222222222222222222222222222222222","child_depth":0,"created_at_ms":10,"format":"neograph-program-run-generation","generation":1,)"
        R"("id":"sha256:78d8f5d67f9b533b2b8370cc464b1d6c12aa321ea53d0ab149c5e4a39c2a2c5d","initial_journal_head":"sha256:4444444444444444444444444444444444444444444444444444444444444444",)"
        R"("initial_run_record_id":"sha256:3333333333333333333333333333333333333333333333333333333333333333","lineage_id":"sha256:be6d40e4efc288ec895d6db85c84e1d6fee70b592bcb50d42efb6ff362bf3ce3",)"
        R"("owner_scope":"owner-a","predecessor_generation_id":null,"program_version_id":"sha256:1111111111111111111111111111111111111111111111111111111111111111","run_id":"run-1","storage_schema_version":1})";
    const auto parsed = ProgramRunGeneration::parse(bytes);
    EXPECT_EQ(parsed.id(),
              "sha256:78d8f5d67f9b533b2b8370cc464b1d6c12aa321ea53d0ab149c5e4a39c2a2c5d");
    EXPECT_FALSE(parsed.replacement_receipt());
    EXPECT_EQ(parsed.serialize_canonical(), bytes);
}

TEST(ProgramRunLineageTest, RuntimeStateTransferReceiptRoundTripsAndRejectsTamper) {
    const auto source = generation_one();
    const auto receipt = transfer_receipt(source);
    const auto parsed = ProgramRuntimeStateTransferReceipt::parse(receipt.serialize_canonical());
    EXPECT_EQ(parsed.id(), receipt.id());
    EXPECT_EQ(parsed.source_context_epoch_ids(), std::vector<std::string>({digest('d'), digest('e')}));
    ASSERT_EQ(parsed.hook_references().size(), 1U);
    EXPECT_EQ(parsed.hook_references().front().event_id, digest('3'));

    auto tampered = receipt.serialize_canonical();
    tampered.replace(tampered.find(digest('d')), digest('d').size(), digest('9'));
    EXPECT_THROW((void)ProgramRuntimeStateTransferReceipt::parse(tampered), std::invalid_argument);

    auto duplicate_epochs = ProgramRuntimeStateTransferReceiptData{
        "owner-a", source.lineage_id(), source.id(), digest('a'), "run-1", digest('b'), digest('c'), 2,
        "run-2", digest('9'), digest('b'), {digest('d'), digest('d')}, std::nullopt, {}};
    EXPECT_THROW((void)ProgramRuntimeStateTransferReceipt(duplicate_epochs), std::invalid_argument);

    auto duplicate_hooks = ProgramRuntimeStateTransferReceiptData{
        "owner-a", source.lineage_id(), source.id(), digest('a'), "run-1", digest('b'), digest('c'), 2,
        "run-2", digest('9'), digest('b'), {}, std::nullopt,
        {{digest('1'), digest('2'), digest('3')}, {digest('1'), digest('4'), digest('5')}}};
    EXPECT_THROW((void)ProgramRuntimeStateTransferReceipt(duplicate_hooks), std::invalid_argument);

    auto oversized_epochs = ProgramRuntimeStateTransferReceiptData{
        "owner-a", source.lineage_id(), source.id(), digest('a'), "run-1", digest('b'), digest('c'), 2,
        "run-2", digest('9'), digest('b'), std::vector<std::string>(4097, digest('d')), std::nullopt, {}};
    EXPECT_THROW((void)ProgramRuntimeStateTransferReceipt(oversized_epochs), std::invalid_argument);
}

TEST(ProgramRunLineageTest, InitialHeadBindsExactGenerationAndRunHeads) {
    const auto generation = generation_one();
    const auto lineage    = initial_lineage(generation);
    EXPECT_TRUE(is_valid_program_run_lineage_initial(lineage, generation));

    auto wrong = ProgramRunLineage::create(ProgramRunLineageData{lineage.owner_scope(),
                                                                 lineage.lineage_id(),
                                                                 lineage.root_run_id(),
                                                                 1,
                                                                 generation.id(),
                                                                 digest('9'),
                                                                 generation.initial_journal_head(),
                                                                 budget(),
                                                                 {},
                                                                 std::nullopt,
                                                                 10,
                                                                 10});
    EXPECT_FALSE(is_valid_program_run_lineage_initial(wrong, generation));
}

TEST(ProgramRunLineageTest, SameGenerationHeadMayOnlyConsumeOrReserveExistingBudget) {
    const auto generation = generation_one();
    const auto initial    = initial_lineage(generation);
    auto       remaining  = budget();
    remaining.wall_time_ms -= 100;
    remaining.max_dynamic_compiles -= 2;
    remaining.max_total_children -= 3;
    RunBudget reserved;
    reserved.wall_time_ms         = 50;
    reserved.max_dynamic_compiles = 1;
    reserved.max_total_children   = 2;
    const auto next               = next_head(initial, remaining, reserved);

    EXPECT_TRUE(is_valid_program_run_lineage_transition(initial, next));
}

TEST(ProgramRunLineageTest, EveryBudgetDimensionIsNonrenewableAcrossHeads) {
    const auto generation = generation_one();
    const auto initial    = initial_lineage(generation);
    using Increase        = std::function<void(RunBudget&)>;
    const std::vector<Increase> increases{
        [](auto& value) { ++value.wall_time_ms; },
        [](auto& value) { ++value.model_tokens; },
        [](auto& value) { ++value.monetary_microunits; },
        [](auto& value) { ++value.max_concurrency; },
        [](auto& value) { ++value.max_program_operations; },
        [](auto& value) { ++value.max_core_steps; },
        [](auto& value) { ++value.max_dynamic_compiles; },
        [](auto& value) { ++value.max_child_depth; },
        [](auto& value) { ++value.max_total_children; },
    };

    for (const auto& increase : increases) {
        auto replenished = budget();
        increase(replenished);
        EXPECT_FALSE(
            is_valid_program_run_lineage_transition(initial, next_head(initial, replenished)));
    }
}

TEST(ProgramRunLineageTest, SuccessorMustBeContiguousAndBindExpectedGeneration) {
    const auto first     = generation_one();
    const auto initial   = initial_lineage(first);
    const auto successor = ProgramRunGeneration::create(
        ProgramRunGenerationData{first.owner_scope(), first.lineage_id(), 2, "run-2", digest('7'),
                                  digest('8'), digest('9'), digest('b'), first.id(), 20, 0,
                                  replacement_receipt(first), std::nullopt, transfer_receipt(first)});
    auto remaining = budget();
    remaining.wall_time_ms -= 1;
    const auto next =
        ProgramRunLineage::create(ProgramRunLineageData{initial.owner_scope(),
                                                        initial.lineage_id(),
                                                        initial.root_run_id(),
                                                        2,
                                                        successor.id(),
                                                        successor.initial_run_record_id(),
                                                        successor.initial_journal_head(),
                                                        remaining,
                                                        {},
                                                        initial.id(),
                                                        initial.created_at_ms(),
                                                        20});

    EXPECT_TRUE(is_valid_program_run_lineage_transition(initial, next, successor));
    EXPECT_FALSE(is_valid_program_run_lineage_transition(initial, next));

    const auto unrelated = ProgramRunGeneration::create(
        ProgramRunGenerationData{first.owner_scope(), first.lineage_id(), 2, "run-2", digest('7'),
                                  digest('8'), digest('9'), digest('b'), digest('c'), 20, 0,
                                  replacement_receipt(first, digest('c'))});
    EXPECT_FALSE(is_valid_program_run_lineage_transition(initial, next, unrelated));
}

TEST(ProgramRunLineageTest, CommittedDescendantBudgetCannotBeReset) {
    const auto generation = generation_one();
    const auto initial    = initial_lineage(generation);
    RunBudget committed;
    committed.max_total_children = 3;
    const auto with_children = next_head(initial, budget(), {}, committed);
    ASSERT_TRUE(is_valid_program_run_lineage_transition(initial, with_children));

    const auto reset = next_head(with_children, budget());
    EXPECT_FALSE(is_valid_program_run_lineage_transition(with_children, reset));
}

}  // namespace
