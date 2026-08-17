#include <gtest/gtest.h>

#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/async/run_sync.h>

#include <atomic>
#include <thread>

using namespace neograph;

namespace {
class RecordingTool final : public Tool {
public:
    ChatTool get_definition() const override { return {"audit", "", json::object()}; }
    std::string get_name() const override { return "audit"; }
    std::string execute(const json& arguments) override { seen = arguments; return "ok"; }
    json seen;
};

class LostSettleJournal final : public HookJournal {
public:
    HookOutboxEntry enqueue(HookInvocation invocation, RuntimeEvent event, std::uint32_t attempts,
                            std::chrono::system_clock::time_point deadline) override {
        return inner.enqueue(std::move(invocation), std::move(event), attempts, deadline);
    }
    HookOutboxEntry publish(std::string_view id) override { return inner.publish(id); }
    std::optional<HookLease> claim(std::string_view id, std::string_view worker,
                                   std::chrono::system_clock::time_point now,
                                   std::chrono::milliseconds lease) override {
        return inner.claim(id, worker, now, lease);
    }
    std::optional<HookOutboxEntry> settle(std::string_view, std::uint64_t,
                                          HookExecutionReceipt,
                                          std::chrono::system_clock::time_point) override { return std::nullopt; }
    std::optional<HookOutboxEntry> reconcile(std::string_view id, HookExecutionState state,
                                             std::string_view head, std::uint64_t fence,
                                             HookExecutionReceipt receipt) override {
        return inner.reconcile(id, state, head, fence, std::move(receipt));
    }
    std::vector<HookOutboxEntry> pending() const override { return inner.pending(); }
    std::vector<HookOutboxEntry> reconciliation_required() const override { return inner.reconciliation_required(); }
    std::optional<HookOutboxEntry> get(std::string_view id) const override { return inner.get(id); }
private:
    InMemoryHookJournal inner;
};

RuntimeEvent event() {
    return RuntimeEvent::create({{}, 7, HookPhase::BeforeToolExecution, "tool_requested",
                                 "owner", "run", json{{"subject", "report"}, {"allowed", true}}});
}

HookDefinition definition(std::uint32_t priority, std::string target = "audit") {
    HookDefinitionData data;
    data.priority = priority;
    data.phase = HookPhase::BeforeToolExecution;
    data.target_id = std::move(target);
    data.required_capabilities = {"audit"};
    data.effect = ToolEffectClass::ReadOnly;
    data.predicate = {HookPredicateKind::Equals, RuntimeEventField::DataPointer, "/allowed", true, {}};
    data.input_mapper = {HookInputMapperKind::Template, {}, json{{"name", json{{"$event", "/subject"}}}}, {}};
    return HookDefinition::create(std::move(data));
}
} // namespace

TEST(HookRegistry, PlansCanonicalInvocationsInPriorityThenIdentityOrder) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view id) -> std::optional<HookTargetContract> {
        if (id != "audit") return std::nullopt;
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    const auto later = definition(9);
    const auto earlier = definition(2);
    registry.admit(later);
    registry.admit(earlier);
    const auto first = registry.plan(event());
    const auto second = registry.plan(event());
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].id(), second[0].id());
    EXPECT_EQ(first[1].id(), second[1].id());
    EXPECT_EQ(first[0].data().arguments, (json{{"name", "report"}}));
    EXPECT_EQ(first[0].data().definition_id, earlier.id());
}

TEST(HookRegistry, RejectsAuthorityAndEffectExpansion) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    auto authority = definition(1);
    auto data = authority.data();
    data.definition_id.clear();
    data.required_capabilities.insert("write");
    EXPECT_THROW(registry.admit(HookDefinition::create(std::move(data))), std::invalid_argument);
    auto effect = definition(1);
    data = effect.data();
    data.definition_id.clear();
    data.effect = ToolEffectClass::ExternalWrite;
    EXPECT_THROW(registry.admit(HookDefinition::create(std::move(data))), std::invalid_argument);
}

TEST(HookDefinition, CanonicalRoundTripRetainsDeclarativeContract) {
    const auto original = definition(4);
    const auto restored = HookDefinition::parse(original.serialize_canonical());
    EXPECT_EQ(restored.id(), original.id());
    EXPECT_EQ(restored.data().input_mapper.value_template,
              (json{{"name", json{{"$event", "/subject"}}}}));
}

TEST(HookContracts, RejectInvalidEnumsMandatoryContinuationAndMalformedPredicates) {
    auto data = definition(1).data();
    data.definition_id.clear();
    data.phase = static_cast<HookPhase>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.delivery = static_cast<HookDelivery>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.failure_mode = static_cast<HookFailureMode>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.idempotency = static_cast<HookIdempotency>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.effect = static_cast<ToolEffectClass>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.input_mapper.kind = static_cast<HookInputMapperKind>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.predicate.kind = static_cast<HookPredicateKind>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.predicate.field = static_cast<RuntimeEventField>(99);
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.failure_mode = HookFailureMode::Continue;
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.predicate = {HookPredicateKind::Not, RuntimeEventField::Type, {}, json(), {}};
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.predicate.pointer = "not-a-pointer";
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
    data = definition(1).data();
    data.definition_id.clear();
    data.predicate.children.assign(65, HookPredicate{});
    data.predicate.kind = HookPredicateKind::All;
    data.predicate.value = json();
    EXPECT_THROW(HookDefinition::create(data), std::invalid_argument);
}

TEST(HookContracts, ObservationalHooksMayExplicitlyContinue) {
    auto data = definition(1).data();
    data.definition_id.clear();
    data.delivery = HookDelivery::DurableObservational;
    data.failure_mode = HookFailureMode::Continue;
    EXPECT_NO_THROW(HookDefinition::create(std::move(data)));
}

TEST(RuntimeEvent, CanonicalRoundTripPreservesTypedFields) {
    const auto original = event();
    const auto restored = RuntimeEvent::parse(original.serialize_canonical());
    EXPECT_EQ(restored.id(), original.id());
    EXPECT_EQ(restored.sequence(), 7u);
    EXPECT_EQ(restored.phase(), HookPhase::BeforeToolExecution);
    EXPECT_EQ(restored.data(), original.data());
}

TEST(HookInvocation, CanonicalRoundTripRejectsInvalidEnumsAndRetainsLogicalIdentity) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    registry.admit(definition(1));
    const auto invocation = registry.plan(event()).front();
    const auto restored = HookInvocation::parse(invocation.serialize_canonical());
    EXPECT_EQ(restored.id(), invocation.id());
    auto invalid = invocation.data();
    invalid.invocation_id.clear();
    invalid.idempotency = static_cast<HookIdempotency>(77);
    EXPECT_THROW(HookInvocation::create(std::move(invalid)), std::invalid_argument);
}

TEST(HookRegistry, PlanningIsEventDrivenAndDoesNotNeedModelToolCalls) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    registry.admit(definition(1));
    auto other_data = event();
    RuntimeEventData unmatched_data{{}, other_data.sequence(), HookPhase::BeforeProviderRequest,
                               other_data.type(), other_data.owner_scope(), other_data.run_id(), other_data.data()};
    // A new phase is a distinct immutable event, independent of model output.
    const auto unmatched = RuntimeEvent::create(std::move(unmatched_data));
    EXPECT_TRUE(registry.plan(unmatched).empty());
}

TEST(NativeHookExecutionAdapter, CallsControllerDirectlyWithoutToolCalls) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    registry.admit(definition(1));
    const auto invocation = registry.plan(event()).front();
    NativeHookExecutionAdapter adapter(resolver, std::make_shared<ToolExecutionController>());
    const auto result = async::run_sync(adapter.execute_result_async(invocation, event()));
    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(tool.seen, (json{{"name", "report"}}));
}

TEST(NativeHookExecutionAdapter, RejectsInvocationEventAndPhaseMismatch) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver);
    registry.admit(definition(1));
    const auto invocation = registry.plan(event()).front();
    NativeHookExecutionAdapter adapter(resolver, std::make_shared<ToolExecutionController>());
    auto changed = RuntimeEvent::create({{}, 8, HookPhase::BeforeToolExecution, "tool_requested", "owner", "run", json::object()});
    EXPECT_THROW(async::run_sync(adapter.execute_result_async(invocation, changed)), std::invalid_argument);
    changed = RuntimeEvent::create({{}, 7, HookPhase::AfterToolExecution, "tool_requested", "owner", "run", json::object()});
    EXPECT_THROW(async::run_sync(adapter.execute_result_async(invocation, changed)), std::invalid_argument);
}

TEST(NativeHookExecutionAdapter, CanonicalizesAdmittedHostMapperOutput) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    auto data = definition(1).data();
    data.definition_id.clear();
    data.input_mapper = {HookInputMapperKind::HostMapper, {}, json(), "mapper"};
    HookRegistry registry(resolver, {"mapper"});
    registry.admit(HookDefinition::create(std::move(data)));
    const auto invocation = registry.plan(event()).front();
    NativeHookExecutionAdapter adapter(resolver, std::make_shared<ToolExecutionController>(),
        [](std::string_view, const RuntimeEvent&) { return json{{"z", 1}, {"a", 2}}; }, {"mapper"});
    EXPECT_TRUE(async::run_sync(adapter.execute_result_async(invocation, event())).succeeded());
    EXPECT_EQ(tool.seen.dump(), "{\"a\":2,\"z\":1}");
}

TEST(HookOutbox, ReclaimsExpiredLeaseAndFencesTheCrashedWorker) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    HookRegistry registry(resolver); registry.admit(definition(1));
    const auto invocation = registry.plan(event()).front();
    InMemoryHookJournal journal;
    const auto now = std::chrono::system_clock::now();
    journal.enqueue(invocation, event(), 2, now + std::chrono::minutes(1));
    journal.publish(invocation.id());
    EXPECT_EQ(journal.enqueue(invocation, event(), 2, now + std::chrono::minutes(1)).data().state,
              HookExecutionState::Pending);
    EXPECT_THROW(journal.enqueue(invocation, event(), 2, now + std::chrono::minutes(2)), std::invalid_argument);
    const auto first = *journal.claim(invocation.id(), "crashed", now, std::chrono::milliseconds(1));
    const auto second = *journal.claim(invocation.id(), "reclaimer", now + std::chrono::seconds(1), std::chrono::seconds(1));
    EXPECT_GT(second.fencing_token, first.fencing_token);
    const auto stale = HookExecutionReceipt::create({invocation.id(), first.entry.data().attempt_count,
        HookExecutionState::Succeeded, {"effect", true, true, {}}, {}});
    EXPECT_FALSE(journal.settle(invocation.id(), first.fencing_token, stale, now + std::chrono::seconds(1)));
}

TEST(HookOutbox, OnlyOneConcurrentWorkerClaimsAndAmbiguousNonIdempotentNeedsReconciliation) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ExternalWrite};
    };
    auto data = definition(1).data(); data.definition_id.clear(); data.effect = ToolEffectClass::ExternalWrite;
    data.idempotency = HookIdempotency::NonIdempotent;
    HookRegistry registry(resolver); registry.admit(HookDefinition::create(std::move(data)));
    const auto invocation = registry.plan(event()).front();
    InMemoryHookJournal journal;
    const auto now = std::chrono::system_clock::now();
    journal.enqueue(invocation, event(), 2, now + std::chrono::minutes(1)); journal.publish(invocation.id());
    std::atomic<unsigned> claims{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        if (journal.claim(invocation.id(), "worker", now, std::chrono::seconds(1))) ++claims;
    });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(claims.load(), 1u);
    const auto lease = *journal.get(invocation.id());
    const auto ambiguous = HookExecutionReceipt::create({invocation.id(), lease.data().attempt_count,
        HookExecutionState::ReconciliationRequired, {"unknown", false, false, {}}, "connection lost"});
    const auto settled = *journal.settle(invocation.id(), lease.data().fencing_token, ambiguous, now);
    EXPECT_EQ(settled.data().state, HookExecutionState::ReconciliationRequired);
    EXPECT_FALSE(journal.claim(invocation.id(), "retry", now + std::chrono::seconds(2), std::chrono::seconds(1)));
    const auto known = HookExecutionReceipt::create({invocation.id(), settled.data().attempt_count,
        HookExecutionState::Succeeded, {"confirmed-effect", true, true, {}}, {}});
    EXPECT_FALSE(journal.reconcile(invocation.id(), HookExecutionState::ReconciliationRequired,
                                  "sha256:" + std::string(64, '0'), settled.data().fencing_token, known));
    const auto reconciled = journal.reconcile(invocation.id(), HookExecutionState::ReconciliationRequired,
        settled.id(), settled.data().fencing_token, known);
    ASSERT_TRUE(reconciled);
    EXPECT_EQ(reconciled->data().state, HookExecutionState::Succeeded);
    EXPECT_TRUE(journal.reconciliation_required().empty());
}

TEST(HookExecutionReceipt, RejectsAmbiguousSuccessAndRoundTripsCanonically) {
    const auto invocation_id = HookInvocation::create({{}, "sha256:" + std::string(64, '1'), "sha256:" + std::string(64, '2'), "audit", HookPhase::BeforeToolExecution, HookDelivery::BlockingMandatory, HookFailureMode::FailClosed, HookIdempotency::Idempotent, ToolEffectClass::ReadOnly, {}, {}, json::object()}).id();
    EXPECT_THROW(HookExecutionReceipt::create({invocation_id, 1, HookExecutionState::Succeeded, {"", false, true, {}}, {}}), std::invalid_argument);
    const auto receipt = HookExecutionReceipt::create({invocation_id, 1, HookExecutionState::Succeeded, {"effect", true, true, {}}, {}});
    EXPECT_EQ(HookExecutionReceipt::parse(receipt.serialize_canonical()).serialize_canonical(), receipt.serialize_canonical());
    EXPECT_FALSE(receipt.id().empty());
}

TEST(MandatoryHookRunner, NativeSuccessSettlesWithStableKnownEffectReceipt) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    registry->admit(definition(1));
    auto journal = std::make_shared<InMemoryHookJournal>();
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(resolver,
        std::make_shared<ToolExecutionController>());
    MandatoryHookRunner runner(journal, registry, adapter);
    const auto runtime_event = event();
    const auto invocation = registry->plan(runtime_event).front();

    EXPECT_NO_THROW(async::run_sync(runner.run_async(
        runtime_event, std::chrono::system_clock::now() + std::chrono::minutes(1))));
    const auto settled = journal->get(invocation.id());
    ASSERT_TRUE(settled);
    ASSERT_TRUE(settled->data().receipt);
    EXPECT_EQ(settled->data().state, HookExecutionState::Succeeded);
    EXPECT_EQ(settled->data().receipt->data().state, HookExecutionState::Succeeded);
    EXPECT_TRUE(settled->data().receipt->data().external_effect.outcome_known);
    EXPECT_TRUE(settled->data().receipt->data().external_effect.succeeded);
    EXPECT_NE(settled->data().receipt->data().external_effect.receipt_id, "ok");
}

TEST(HookOutbox, CancelledStateRequiresCancelledReceipt) {
    const auto now = std::chrono::system_clock::now();
    auto invocation = HookInvocation::create({{}, "sha256:" + std::string(64, '1'),
        "sha256:" + std::string(64, '2'), "audit", HookPhase::BeforeToolExecution,
        HookDelivery::BlockingMandatory, HookFailureMode::FailClosed,
        HookIdempotency::Idempotent, ToolEffectClass::ReadOnly, {}, {}, json::object()});
    const auto runtime = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution,
        "event", "owner", "run", json::object()});
    auto data = invocation.data();
    data.invocation_id.clear();
    data.event_id = runtime.id();
    invocation = HookInvocation::create(std::move(data));
    const auto failed = HookExecutionReceipt::create({invocation.id(), 1,
        HookExecutionState::Failed, {}, "cancelled"});
    EXPECT_THROW(HookOutboxEntry::create({invocation, runtime, HookExecutionState::Cancelled,
        1, 1, now + std::chrono::minutes(1), 1, {}, failed}), std::invalid_argument);
}

TEST(HookOutbox, RejectsMalformedStatesAndNonIdempotentLeaseReclaimRequiresReconciliation) {
    const auto now = std::chrono::system_clock::now();
    auto invocation = HookInvocation::create({{}, "sha256:" + std::string(64, '1'), "sha256:" + std::string(64, '2'), "audit", HookPhase::BeforeToolExecution, HookDelivery::BlockingMandatory, HookFailureMode::FailClosed, HookIdempotency::NonIdempotent, ToolEffectClass::ExternalWrite, {}, {}, json::object()});
    const auto runtime = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json::object()});
    auto changed = invocation.data(); changed.invocation_id.clear(); changed.event_id = runtime.id(); invocation = HookInvocation::create(std::move(changed));
    EXPECT_THROW(HookOutboxEntry::create({invocation, runtime, HookExecutionState::Pending, 1, 2, now + std::chrono::minutes(1)}), std::invalid_argument);
    InMemoryHookJournal journal;
    journal.enqueue(invocation, runtime, 2, now + std::chrono::minutes(1)); journal.publish(invocation.id());
    ASSERT_TRUE(journal.claim(invocation.id(), "worker", now, std::chrono::milliseconds(1)));
    EXPECT_FALSE(journal.claim(invocation.id(), "reclaimer", now + std::chrono::seconds(1), std::chrono::seconds(1)));
    const auto blocked = *journal.get(invocation.id());
    EXPECT_EQ(blocked.data().state, HookExecutionState::ReconciliationRequired);
    ASSERT_EQ(journal.reconciliation_required().size(), 1u);
    EXPECT_TRUE(journal.pending().empty());
}

TEST(MandatoryHookRunner, BlocksOnLostSettlementAndExpiredDeadline) {
    RecordingTool tool;
    HookTargetResolver resolver = [&tool](std::string_view) -> std::optional<HookTargetContract> {
        return HookTargetContract{&tool, {"audit"}, ToolEffectClass::ReadOnly};
    };
    auto registry = std::make_shared<HookRegistry>(resolver);
    registry->admit(definition(1));
    auto adapter = std::make_shared<NativeHookExecutionAdapter>(resolver,
        std::make_shared<ToolExecutionController>());
    auto journal = std::make_shared<LostSettleJournal>();
    MandatoryHookRunner runner(journal, registry, adapter);
    EXPECT_THROW(async::run_sync(runner.run_async(event(), std::chrono::system_clock::now() + std::chrono::minutes(1))), HookBoundaryBlocked);
    EXPECT_THROW(async::run_sync(runner.run_async(event(), std::chrono::system_clock::now())), std::invalid_argument);
}
