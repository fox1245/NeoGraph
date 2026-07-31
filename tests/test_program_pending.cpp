#include <neograph/program/pending.h>
#include <neograph/program/result.h>

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace {

using neograph::json;

std::string sha(char digit) {
    return "sha256:" + std::string(64, digit);
}
using namespace neograph::program;

json result_schema() {
    return json{{"additionalProperties", false},
                {"properties", json{{"answer", json{{"type", "string"}}}}},
                {"required", json::array({"answer"})},
                {"type", "object"}};
}

ProgramPendingInput make_input(std::optional<std::uint64_t> expiry = 5000) {
    ProgramPendingInputData data;
    data.operation_id         = "root";
    data.call_id              = "call:17";
    data.kind                 = ProgramPendingInputKind::CapabilityResult;
    data.result_schema        = result_schema();
    data.payload              = json{{"capability", "review"}, {"arguments", json{{"x", 1}}}};
    data.expires_at_unix_ms   = expiry;
    data.core_node            = "call_core";
    data.core_interrupt_value = json{{"value", json{{"legacy", true}}}};
    return ProgramPendingInput(std::move(data));
}

ProgramPendingEffect make_effect(std::optional<std::uint64_t> expiry = 5000) {
    ProgramPendingEffectData data;
    data.operation_id         = "root";
    data.call_id              = "call:18";
    data.effect_id            = "effect:18";
    data.result_schema        = result_schema();
    data.payload              = json{{"capability", "publish"}, {"arguments", json{{"x", 2}}}};
    data.expires_at_unix_ms   = expiry;
    data.effect_mode          = EffectMode::Brokered;
    data.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    data.core_node            = "call_core";
    data.core_interrupt_value = json{{"value", json{{"legacy", "effect"}}}};
    return ProgramPendingEffect(std::move(data));
}

TEST(ProgramPendingInputTest, CanonicalRoundTripRetainsSchemaPayloadAndCoreProjection) {
    auto       pending = make_input();
    const auto bytes   = pending.serialize_canonical();
    auto       parsed  = ProgramPendingInput::parse(bytes);

    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    EXPECT_EQ(parsed.operation_id(), "root");
    EXPECT_EQ(parsed.call_id(), "call:17");
    EXPECT_EQ(parsed.kind(), ProgramPendingInputKind::CapabilityResult);
    EXPECT_EQ(parsed.result_schema(), result_schema());
    EXPECT_EQ(parsed.payload()["capability"], "review");
    EXPECT_EQ(parsed.core_node(), "call_core");
    EXPECT_EQ(parsed.core_interrupt_value(), json({{"value", json{{"legacy", true}}}}));
    EXPECT_EQ(parsed.state(), ProgramPendingState::Awaiting);
    EXPECT_FALSE(parsed.consumed_result().has_value());

    EXPECT_EQ(bytes.find("transport"), std::string::npos);
    EXPECT_EQ(bytes.find("server"), std::string::npos);
    EXPECT_EQ(bytes.find("session"), std::string::npos);
    EXPECT_EQ(bytes.find("executor"), std::string::npos);
    EXPECT_EQ(bytes.find("url"), std::string::npos);
    auto transport_tainted      = json::parse(bytes);
    transport_tainted["server"] = "adapter-only";
    EXPECT_THROW(ProgramPendingInput::parse(transport_tainted.dump()), std::invalid_argument);
}

TEST(ProgramPendingInputTest, ExactIdSchemaAndDuplicateRulesPreserveFirstResult) {
    auto       pending = make_input();
    const json first{{"answer", "first"}};

    auto wrong_id = pending.submit("call:other", first, 1000);
    EXPECT_EQ(wrong_id.disposition, ProgramPendingDisposition::WrongPendingId);
    EXPECT_FALSE(wrong_id.state_changed());
    EXPECT_EQ(wrong_id.value.state(), ProgramPendingState::Awaiting);

    auto wrong_schema = pending.submit("call:17", json{{"answer", 9}}, 1000);
    EXPECT_EQ(wrong_schema.disposition, ProgramPendingDisposition::SchemaMismatch);
    EXPECT_FALSE(wrong_schema.state_changed());

    auto accepted = pending.submit("call:17", first, 1000);
    ASSERT_EQ(accepted.disposition, ProgramPendingDisposition::Applied);
    EXPECT_TRUE(accepted.state_changed());
    ASSERT_EQ(accepted.value.state(), ProgramPendingState::Consumed);
    ASSERT_TRUE(accepted.value.consumed_result());
    EXPECT_EQ(*accepted.value.consumed_result(), first);
    auto reconnected = ProgramPendingInput::parse(accepted.value.serialize_canonical());
    EXPECT_EQ(reconnected, accepted.value);

    auto duplicate = reconnected.submit("call:17", first, 1001);
    EXPECT_EQ(duplicate.disposition, ProgramPendingDisposition::Duplicate);
    EXPECT_FALSE(duplicate.state_changed());

    auto conflict = reconnected.submit("call:17", json{{"answer", "second"}}, 1001);
    EXPECT_EQ(conflict.disposition, ProgramPendingDisposition::Conflict);
    ASSERT_TRUE(conflict.value.consumed_result());
    EXPECT_EQ(*conflict.value.consumed_result(), first);
}

TEST(ProgramPendingInputTest, ExpiryRejectsLateResultAndCancellationAppliesOnce) {
    auto pending = make_input(2000);
    auto expired = pending.submit("call:17", json{{"answer", "late"}}, 2000);
    EXPECT_EQ(expired.disposition, ProgramPendingDisposition::Expired);
    EXPECT_TRUE(expired.state_changed());
    EXPECT_EQ(expired.value.state(), ProgramPendingState::Expired);
    EXPECT_EQ(ProgramPendingInput::parse(expired.value.serialize_canonical()), expired.value);
    EXPECT_EQ(expired.value.submit("call:17", json{{"answer", "late"}}, 2001).disposition,
              ProgramPendingDisposition::Expired);

    auto cancellation = make_input(std::nullopt).cancel();
    EXPECT_EQ(cancellation.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(cancellation.value.state(), ProgramPendingState::Cancelled);
    EXPECT_EQ(ProgramPendingInput::parse(cancellation.value.serialize_canonical()),
              cancellation.value);
    auto repeated = cancellation.value.cancel();
    EXPECT_EQ(repeated.disposition, ProgramPendingDisposition::Duplicate);
    EXPECT_FALSE(repeated.state_changed());
    EXPECT_EQ(cancellation.value.submit("call:17", json{{"answer", "late"}}, 3000).disposition,
              ProgramPendingDisposition::Cancelled);
}

TEST(ProgramPendingResultTest, TypedPendingIsAuthorityAndCoreProjectionRoundTrips) {
    auto              pending = make_input();
    ProgramResultData data;
    data.status             = ProgramTerminalStatus::Interrupted;
    data.run_id             = "run:pending";
    data.program_version_id = sha('1');
    data.bundle_id          = sha('2');
    data.operation_id       = "root";
    data.attempt            = 1;
    data.interrupt = ProgramInterrupt{pending.core_node(), pending.core_interrupt_value(), pending,
                                      std::nullopt};

    auto result = ProgramResult::create(data);
    auto parsed = ProgramResult::parse(result.serialize_canonical());
    ASSERT_TRUE(parsed.interrupt());
    ASSERT_TRUE(parsed.interrupt()->pending_input);
    EXPECT_EQ(*parsed.interrupt()->pending_input, pending);
    EXPECT_EQ(parsed.interrupt()->core_node, pending.core_node());
    EXPECT_EQ(parsed.interrupt()->value, pending.core_interrupt_value());

    data.interrupt =
        ProgramInterrupt{"call_core", json{{"legacy", true}}, std::nullopt, std::nullopt};
    EXPECT_THROW(ProgramResult::create(data), std::invalid_argument);

    auto ambiguous = make_effect().mark_outcome_unknown(1000).value;
    data.status    = ProgramTerminalStatus::AmbiguousEffect;
    data.interrupt = ProgramInterrupt{ambiguous.core_node(), ambiguous.core_interrupt_value(),
                                      std::nullopt, ambiguous};
    auto ambiguous_result = ProgramResult::create(data);
    auto ambiguous_parsed = ProgramResult::parse(ambiguous_result.serialize_canonical());
    ASSERT_TRUE(ambiguous_parsed.interrupt()->pending_effect);
    EXPECT_EQ(ambiguous_parsed.interrupt()->pending_effect->state(),
              ProgramPendingState::Ambiguous);
}

TEST(ProgramPendingEffectTest, KnownResultUsesExactIdentitySchemaAndDedupe) {
    auto       pending = make_effect();
    const json result{{"answer", "known"}};

    EXPECT_EQ(pending.submit("wrong", "effect:18", result, 1000).disposition,
              ProgramPendingDisposition::WrongPendingId);
    EXPECT_EQ(pending.submit("call:18", "effect:18", json{{"answer", 5}}, 1000).disposition,
              ProgramPendingDisposition::SchemaMismatch);

    auto accepted = pending.submit("call:18", "effect:18", result, 1000);
    ASSERT_EQ(accepted.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(accepted.value.state(), ProgramPendingState::Consumed);
    EXPECT_EQ(accepted.value.reconciliation(), ProgramEffectReconciliation::Completed);
    EXPECT_EQ(*accepted.value.reconciled_result(), result);
    EXPECT_EQ(accepted.value.submit("call:18", "effect:18", result, 1001).disposition,
              ProgramPendingDisposition::Duplicate);
    EXPECT_EQ(accepted.value.submit("call:18", "effect:18", json{{"answer", "different"}}, 1001)
                  .disposition,
              ProgramPendingDisposition::Conflict);
}

TEST(ProgramPendingEffectTest, NonIdempotentUnknownIsCanonicalAmbiguousAndNeverReawaits) {
    auto ambiguous = make_effect().mark_outcome_unknown(1000);
    ASSERT_EQ(ambiguous.disposition, ProgramPendingDisposition::Applied);
    ASSERT_EQ(ambiguous.value.state(), ProgramPendingState::Ambiguous);
    EXPECT_EQ(ambiguous.value.reconciliation(), ProgramEffectReconciliation::None);

    const auto stored      = ambiguous.value.serialize_canonical();
    auto       reconnected = ProgramPendingEffect::parse(stored);
    EXPECT_EQ(reconnected.serialize_canonical(), stored);
    EXPECT_EQ(reconnected.state(), ProgramPendingState::Ambiguous);
    EXPECT_EQ(reconnected.mark_outcome_unknown(1001).disposition,
              ProgramPendingDisposition::Duplicate);

    auto unknown = reconnected.reconcile("call:18", "effect:18",
                                         ProgramEffectReconciliation::Unknown, std::nullopt, 1001);
    ASSERT_EQ(unknown.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(unknown.value.state(), ProgramPendingState::Ambiguous);
    EXPECT_EQ(unknown.value.reconciliation(), ProgramEffectReconciliation::Unknown);
    EXPECT_EQ(unknown.value
                  .reconcile("call:18", "effect:18", ProgramEffectReconciliation::Unknown,
                             std::nullopt, 1002)
                  .disposition,
              ProgramPendingDisposition::Duplicate);
    EXPECT_EQ(ProgramPendingEffect::parse(unknown.value.serialize_canonical()).state(),
              ProgramPendingState::Ambiguous);

    auto unknown_after_deadline = make_effect(1000).mark_outcome_unknown(1001);
    ASSERT_EQ(unknown_after_deadline.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(unknown_after_deadline.value.state(), ProgramPendingState::Ambiguous);
}

TEST(ProgramPendingEffectTest, ReconcileValidatesIdentitySchemaAndFirstTerminalAuthority) {
    auto       ambiguous = make_effect().mark_outcome_unknown(1000).value;
    const json completed_result{{"answer", "published"}};

    EXPECT_EQ(ambiguous
                  .reconcile("wrong", "effect:18", ProgramEffectReconciliation::Completed,
                             completed_result, 1001)
                  .disposition,
              ProgramPendingDisposition::WrongPendingId);
    EXPECT_EQ(ambiguous
                  .reconcile("call:18", "effect:18", ProgramEffectReconciliation::Completed,
                             json{{"answer", 4}}, 1001)
                  .disposition,
              ProgramPendingDisposition::SchemaMismatch);
    auto failed = ambiguous.reconcile("call:18", "effect:18", ProgramEffectReconciliation::Failed,
                                      std::nullopt, 1001);
    ASSERT_EQ(failed.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(failed.value.state(), ProgramPendingState::Consumed);
    EXPECT_EQ(failed.value.reconciliation(), ProgramEffectReconciliation::Failed);
    EXPECT_FALSE(failed.value.reconciled_result());

    auto completed = ambiguous.reconcile(
        "call:18", "effect:18", ProgramEffectReconciliation::Completed, completed_result, 1001);
    ASSERT_EQ(completed.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(completed.value.state(), ProgramPendingState::Consumed);
    EXPECT_EQ(completed.value.reconciliation(), ProgramEffectReconciliation::Completed);
    ASSERT_TRUE(completed.value.reconciled_result());
    EXPECT_EQ(*completed.value.reconciled_result(), completed_result);

    EXPECT_EQ(completed.value
                  .reconcile("call:18", "effect:18", ProgramEffectReconciliation::Completed,
                             completed_result, 1002)
                  .disposition,
              ProgramPendingDisposition::Duplicate);
    auto conflict = completed.value.reconcile(
        "call:18", "effect:18", ProgramEffectReconciliation::Failed, std::nullopt, 1002);
    EXPECT_EQ(conflict.disposition, ProgramPendingDisposition::Conflict);
    EXPECT_EQ(conflict.value.reconciliation(), ProgramEffectReconciliation::Completed);
    EXPECT_EQ(*conflict.value.reconciled_result(), completed_result);
}

TEST(ProgramPendingEffectTest, PendingExpiryAndCancellationDoNotEraseAmbiguity) {
    auto expired = make_effect(2000).submit("call:18", "effect:18", json{{"answer", "late"}}, 2000);
    EXPECT_EQ(expired.disposition, ProgramPendingDisposition::Expired);
    EXPECT_EQ(expired.value.state(), ProgramPendingState::Expired);

    auto cancellation = make_effect(std::nullopt).cancel();
    ASSERT_EQ(cancellation.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(cancellation.value.state(), ProgramPendingState::Cancelled);
    EXPECT_EQ(cancellation.value.cancel().disposition, ProgramPendingDisposition::Duplicate);

    auto ambiguous = make_effect(2000).mark_outcome_unknown(1000).value;
    EXPECT_EQ(ambiguous.expire(2000).disposition, ProgramPendingDisposition::NotAwaiting);
    EXPECT_EQ(ambiguous.cancel().disposition, ProgramPendingDisposition::NotAwaiting);
    auto reconciled =
        ambiguous.reconcile("call:18", "effect:18", ProgramEffectReconciliation::Completed,
                            json{{"answer", "confirmed later"}}, 3000);
    EXPECT_EQ(reconciled.disposition, ProgramPendingDisposition::Applied);
    EXPECT_EQ(reconciled.value.state(), ProgramPendingState::Consumed);
}

}  // namespace
