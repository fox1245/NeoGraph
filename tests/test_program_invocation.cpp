#include <neograph/program/invocation.h>

#include <gtest/gtest.h>

namespace {
using neograph::json;
using namespace neograph::program;

RunInvocation invocation() {
    RunInvocation value;
    value.owner_scope = "tenant:alpha";
    value.agent_id = "agent:planner";
    value.program_version_id = "sha256:" + std::string(64, 'a');
    value.run_id = "run-1";
    value.budget.max_core_steps = 20;
    value.input = json{{"z", 2}, {"a", json{{"prompt", "hello"}}}};
    value.message_sequence = 1;
    value.idempotency_key = "message-1";
    value.correlation_id = "corr-1";
    return value;
}
}  // namespace

TEST(RunInvocationTest, CanonicalRoundTripAndObjectOrder) {
    auto first = invocation();
    first.validate();
    auto second = first;
    second.input = json{{"a", json{{"prompt", "hello"}}}, {"z", 2}};

    EXPECT_EQ(first.canonical_identity(), second.canonical_identity());
    const auto stored = first.serialize_canonical();
    const auto parsed = RunInvocation::parse(stored);
    EXPECT_EQ(parsed.protocol_revision, first.protocol_revision);
    EXPECT_EQ(parsed.owner_scope, first.owner_scope);
    EXPECT_EQ(parsed.agent_id, first.agent_id);
    EXPECT_EQ(parsed.program_version_id, first.program_version_id);
    EXPECT_EQ(parsed.run_id, first.run_id);
    EXPECT_EQ(parsed.parent_run_id, first.parent_run_id);
    EXPECT_EQ(parsed.budget, first.budget);
    EXPECT_EQ(parsed.canonical_payload(), first.canonical_payload());
    EXPECT_EQ(parsed.message_sequence, first.message_sequence);
    EXPECT_EQ(parsed.idempotency_key, first.idempotency_key);
    EXPECT_EQ(parsed.correlation_id, first.correlation_id);
    EXPECT_EQ(parsed.artifact, first.artifact);
    EXPECT_EQ(parsed, first);
    EXPECT_EQ(parsed.serialize_canonical(), stored);
    json encoded;
    to_json(encoded, first);
    RunInvocation decoded;
    from_json(encoded, decoded);
    EXPECT_EQ(decoded, first);
}

TEST(RunInvocationTest, RequiredOwnerAndSequenceFieldsAreFailClosed) {
    auto missing_owner = invocation();
    missing_owner.owner_scope.clear();
    EXPECT_THROW(missing_owner.validate(), std::invalid_argument);

    auto zero_sequence = invocation();
    zero_sequence.message_sequence = 0;
    EXPECT_THROW(zero_sequence.validate(), std::invalid_argument);
}

TEST(RunInvocationTest, IdempotencyIsStrictAndOwnerScoped) {
    const auto first = invocation();
    EXPECT_EQ(compare_invocations(first, first), InvocationIdempotencyResult::Duplicate);

    auto changed_input = first;
    changed_input.input["z"] = 3;
    EXPECT_EQ(compare_invocations(first, changed_input), InvocationIdempotencyResult::Conflict);

    auto changed_owner = first;
    changed_owner.owner_scope = "tenant:other";
    EXPECT_EQ(compare_invocation_idempotency(first, changed_owner),
              InvocationIdempotencyResult::Conflict);

    auto changed_sequence = first;
    changed_sequence.message_sequence = 2;
    EXPECT_EQ(compare_invocations(first, changed_sequence), InvocationIdempotencyResult::Conflict);

    auto different_key = first;
    different_key.idempotency_key = "message-2";
    EXPECT_EQ(compare_invocations(first, different_key), InvocationIdempotencyResult::Distinct);
}

TEST(InvocationTerminalTest, UnknownStatusAndDiagnosticNeverBecomeSuccess) {
    InvocationDiagnostic diagnostic;
    diagnostic.code = "REMOTE_STATE";
    diagnostic.message = "Peer returned an extension status";
    diagnostic.severity_value = "vendor_warning";
    diagnostic.witness = json{{"raw", true}};

    const auto terminal = InvocationTerminal::create(
        "vendor_terminal_state", json{{"partial", true}}, {diagnostic});
    EXPECT_EQ(terminal.status, InvocationTerminalStatus::Unknown);
    EXPECT_TRUE(terminal.failed());
    EXPECT_FALSE(terminal.succeeded());

    const auto parsed = InvocationTerminal::parse(terminal.serialize_canonical());
    ASSERT_EQ(parsed.diagnostics.size(), 1U);
    EXPECT_EQ(parsed.status, InvocationTerminalStatus::Unknown);
    EXPECT_EQ(parsed.status_value, "vendor_terminal_state");
    EXPECT_EQ(parsed.diagnostics.front().severity, InvocationDiagnosticSeverity::Unknown);
    EXPECT_EQ(parsed.diagnostics.front().severity_value, "vendor_warning");
}

TEST(InvocationTerminalTest, CompletedIsTheOnlySuccessfulTerminal) {
    const auto terminal = InvocationTerminal::create("completed");
    EXPECT_TRUE(terminal.succeeded());
    EXPECT_FALSE(terminal.failed());
    EXPECT_FALSE(InvocationTerminal::create("unknown").succeeded());
}
