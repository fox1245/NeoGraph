#include <gtest/gtest.h>

#include <neograph/program/runtime_instruction.h>
#include <neograph/program/synthesis.h>

using namespace neograph;
using namespace neograph::program;

namespace {
std::string sha(char value) { return "sha256:" + std::string(64, value); }
}

TEST(RuntimeDeveloperInstruction, CanonicalizesRequestsWithoutGrantingAuthority) {
    RuntimeDeveloperInstructionData data;
    data.owner_scope = "tenant";
    data.source_run_id = "run";
    data.feed_id = "runtime-feed";
    data.sequence = 1;
    data.submitted_at_ms = 7;
    data.text = "Use the safer topology";
    data.payload = json{{"reason", "new evidence"}};
    data.requested_capabilities = {"workspace.write", "workspace.read"};
    data.requested_effects = {"edit"};
    const auto value = RuntimeDeveloperInstruction::create(std::move(data));
    const auto parsed = RuntimeDeveloperInstruction::parse(value.serialize_canonical());
    EXPECT_EQ(parsed.id(), value.id());
    EXPECT_EQ(parsed.data().requested_capabilities,
              (std::vector<std::string>{"workspace.read", "workspace.write"}));
    EXPECT_EQ(parsed.data().text, "Use the safer topology");
}

TEST(RuntimeInstructionDecision, RequiresExactTargetOnlyForTransitions) {
    RuntimeInstructionDecisionData transition;
    transition.instruction_id = sha('a');
    transition.source_run_id = "run";
    transition.expected_lineage_head_id = sha('b');
    transition.policy_identity = sha('c');
    transition.action = RuntimeInstructionAction::MigrateGraph;
    transition.target_program_version_id = sha('d');
    transition.target_run_id = "run-successor";
    transition.reason = "topology change required";
    const auto value = RuntimeInstructionDecision::create(std::move(transition));
    EXPECT_EQ(RuntimeInstructionDecision::parse(value.serialize_canonical()).id(),
              value.id());

    RuntimeInstructionDecisionData invalid;
    invalid.instruction_id = sha('a');
    invalid.source_run_id = "run";
    invalid.expected_lineage_head_id = sha('b');
    invalid.policy_identity = sha('c');
    invalid.action = RuntimeInstructionAction::Rejected;
    invalid.target_program_version_id = sha('d');
    invalid.target_run_id = "forged";
    invalid.reason = "rejected";
    EXPECT_THROW(RuntimeInstructionDecision::create(std::move(invalid)),
                 std::invalid_argument);
}

TEST(ProgramSynthesisContracts, CanonicalizeProposalReservationAndReceipt) {
    ProgramSynthesisProposalData proposal_data;
    proposal_data.owner_scope = "tenant";
    proposal_data.lineage_id = sha('1');
    proposal_data.parent_run_id = "run";
    proposal_data.source = ProgramSource::from_javascript(
        "generated.js", "export function* main() { return {}; }");
    proposal_data.requested_budget = RunBudget{100, 10, 1, 1, 1, 1, 0, 0, 0};
    proposal_data.created_at_ms = 1;
    const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));
    EXPECT_EQ(ProgramSynthesisProposal::parse(proposal.serialize_canonical()).id(),
              proposal.id());

    auto before = RunBudget{100, 10, 1, 1, 1, 1, 1, 0, 0};
    auto after = before;
    after.max_dynamic_compiles = 0;
    const auto reservation = ProgramSynthesisReservation::create(
        {proposal.id(), proposal.data().lineage_id, sha('2'), sha('3'),
         before, after});
    EXPECT_EQ(ProgramSynthesisReservation::parse(
                  reservation.serialize_canonical()).id(),
              reservation.id());
    const auto receipt = ProgramSynthesisReceipt::create(
        {proposal.id(), reservation.id(), sha('4'), sha('5'), sha('6')});
    EXPECT_EQ(ProgramSynthesisReceipt::parse(receipt.serialize_canonical()).id(),
              receipt.id());

    after.max_dynamic_compiles = 1;
    EXPECT_THROW(ProgramSynthesisReservation::create(
                     {proposal.id(), proposal.data().lineage_id, sha('2'), sha('3'),
                      before, after}),
                 std::invalid_argument);
}
