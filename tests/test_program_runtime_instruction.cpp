#include <gtest/gtest.h>

#include <neograph/program/runtime_instruction.h>
#include <neograph/program/synthesis.h>

using namespace neograph;
using namespace neograph::program;

namespace {
std::string sha(char value) { return "sha256:" + std::string(64, value); }

ProgramChildSynthesisGrantData child_grant_data() {
    ProgramChildSynthesisGrantData data;
    data.owner_scope                 = "tenant";
    data.parent_run_id               = "parent";
    data.parent_program_version_id   = sha('1');
    data.parent_policy_fingerprint   = sha('2');
    data.lineage_id                  = sha('3');
    data.expected_lineage_head_id    = sha('4');
    data.template_identity           = sha('5');
    data.reviewed_source_identity    = sha('6');
    data.semantic_validator_identity = sha('7');
    data.semantic_contract_identity  = sha('8');
    data.allowed_capabilities        = {"workspace.write", "workspace.read"};
    data.allowed_effects             = {"write", "read"};
    data.child_budget_ceiling        = RunBudget{100, 10, 1, 1, 2, 3, 0, 0, 0};
    return data;
}
}  // namespace

TEST(ProgramChildSynthesisGrant, CanonicalizesHostPolicyAndBindsEveryField) {
    const auto grant  = ProgramChildSynthesisGrant::create(child_grant_data());
    const auto parsed = ProgramChildSynthesisGrant::parse(grant.serialize_canonical());
    EXPECT_EQ(parsed.id(), grant.id());
    EXPECT_EQ(parsed.serialize_canonical(), grant.serialize_canonical());
    EXPECT_EQ(parsed.data().allowed_capabilities,
              (std::vector<std::string>{"workspace.read", "workspace.write"}));
    const auto original = json::parse(grant.serialize_canonical());
    for (const auto* key :
         {"parent_run_id", "owner_scope", "parent_program_version_id", "parent_policy_fingerprint",
          "lineage_id", "expected_lineage_head_id", "template_identity", "reviewed_source_identity",
          "semantic_validator_identity", "semantic_contract_identity"}) {
        auto altered = original;
        altered[key] = sha('a');
        EXPECT_THROW(ProgramChildSynthesisGrant::parse(altered.dump()), std::invalid_argument)
            << key;
    }
    auto altered                = original;
    altered["max_source_bytes"] = parsed.data().max_source_bytes + 1;
    EXPECT_THROW(ProgramChildSynthesisGrant::parse(altered.dump()), std::invalid_argument);
    altered                 = original;
    altered["self_granted"] = true;
    EXPECT_THROW(ProgramChildSynthesisGrant::parse(altered.dump()), std::invalid_argument);
}

TEST(ProgramChildSynthesisGrant, RejectsInvalidLimitsDuplicateAuthorityAndGuarantee) {
    auto data             = child_grant_data();
    data.max_source_bytes = 0;
    EXPECT_THROW(ProgramChildSynthesisGrant::create(data), std::invalid_argument);
    data = child_grant_data();
    data.allowed_capabilities.push_back(data.allowed_capabilities.front());
    EXPECT_THROW(ProgramChildSynthesisGrant::create(data), std::invalid_argument);
    data                             = child_grant_data();
    data.minimum_execution_guarantee = static_cast<ExecutionGuarantee>(255);
    EXPECT_THROW(ProgramChildSynthesisGrant::create(data), std::invalid_argument);
}

TEST(ProgramChildSynthesisGrant, RejectsIntegerWrapBeforeCanonicalIdentityVerification) {
    const auto grant   = ProgramChildSynthesisGrant::create(child_grant_data());
    auto       wrapped = json::parse(grant.serialize_canonical());
    wrapped["child_budget_ceiling"]["max_concurrency"] =
        (std::uint64_t{1} << 32) + grant.data().child_budget_ceiling.max_concurrency;
    // A narrowing uint32 cast would restore the original value and accept its ID.
    EXPECT_THROW(ProgramChildSynthesisGrant::parse(wrapped.dump()), std::invalid_argument);
    wrapped                       = json::parse(grant.serialize_canonical());
    wrapped["max_sealed_modules"] = -1;
    EXPECT_THROW(ProgramChildSynthesisGrant::parse(wrapped.dump()), std::invalid_argument);
}

TEST(ProgramSynthesisContracts, StoredBudgetCannotNormalizeInvalidNumbersIntoAnExistingId) {
    ProgramSynthesisProposalData data;
    data.owner_scope   = "tenant";
    data.parent_run_id = "parent";
    data.lineage_id    = sha('1');
    data.source =
        ProgramSource::from_javascript("numbers.js", "export function* main() { return {}; }");
    data.requested_budget                       = RunBudget{100, UINT64_MAX, 1, 1, 1, 1, 0, 0, 0};
    const auto proposal                         = ProgramSynthesisProposal::create(data);
    auto       invalid                          = json::parse(proposal.serialize_canonical());
    invalid["requested_budget"]["model_tokens"] = -1;
    EXPECT_THROW(ProgramSynthesisProposal::parse(invalid.dump()), std::invalid_argument);
    invalid                                        = json::parse(proposal.serialize_canonical());
    invalid["requested_budget"]["max_concurrency"] = (std::uint64_t{1} << 32) + 1;
    EXPECT_THROW(ProgramSynthesisProposal::parse(invalid.dump()), std::invalid_argument);
    invalid = json::parse(proposal.serialize_canonical());
    invalid["requested_budget"]["monetary_microunits"] = 1.5;
    EXPECT_THROW(ProgramSynthesisProposal::parse(invalid.dump()), std::invalid_argument);
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
    const auto validation = ProgramSynthesisValidationReceipt::create(
        {proposal.id(), reservation.id(), sha('4'), sha('7'), sha('8'), true, sha('9')});
    EXPECT_EQ(ProgramSynthesisValidationReceipt::parse(
                  validation.serialize_canonical()).id(),
              validation.id());

    after.max_dynamic_compiles = 1;
    EXPECT_THROW(ProgramSynthesisReservation::create(
                     {proposal.id(), proposal.data().lineage_id, sha('2'), sha('3'),
                      before, after}),
                 std::invalid_argument);
}
