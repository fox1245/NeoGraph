#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

SealedCoreDefinition sealed_definition() {
    json definition{{"schema_version", 1},
                    {"name", "main"},
                    {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
                    {"nodes", json{{"work", json{{"type", "recorded-worker"}}}}},
                    {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "__end__"}}})},
                    {"conditional_edges", json::array()}};
    return SealedCoreDefinition{"main", sealed_core_definition_hash(definition),
                                std::move(definition)};
}

ExactForkCompatibilityFacts compatible_facts() {
    auto       definition = sealed_definition();
    Checkpoint checkpoint;
    checkpoint.id               = "checkpoint-source";
    checkpoint.thread_id        = "core-thread-source";
    checkpoint.channel_values   = json{{"value", "before"}};
    checkpoint.channel_versions = json{{"value", 1}};
    checkpoint.current_node     = "work";
    checkpoint.next_nodes       = {"work"};
    checkpoint.interrupt_phase  = CheckpointPhase::NodeInterrupt;
    checkpoint.schema_version   = CHECKPOINT_SCHEMA_VERSION;

    ExactForkCompatibilityFacts facts;
    facts.owner_scope                        = "tenant:fork";
    facts.source_owner_scope                 = "tenant:fork";
    facts.target_owner_scope                 = "tenant:fork";
    facts.requested_source                   = {"source-run", "checkpoint-source"};
    facts.stored_source_run_id               = "source-run";
    facts.source_program_version_id          = digest('1');
    facts.target_program_version_id          = digest('2');
    facts.resolved_target_program_version_id = facts.target_program_version_id;
    facts.published_checkpoint               = CoreCheckpointIdentity{
        "main", digest('a'), "core-thread-source", "checkpoint-source", CHECKPOINT_SCHEMA_VERSION};
    facts.loaded_checkpoint      = std::move(checkpoint);
    facts.source_core_plan       = CorePlanIdentity{"main", digest('a')};
    facts.target_core_plan       = CorePlanIdentity{"main", digest('a')};
    facts.source_core_definition = definition;
    facts.target_core_definition = std::move(definition);
    facts.source_continuation =
        ProgramContinuation{"call_core:main", ContinuationState::Interrupted, 1};
    return facts;
}

bool has_field(const ForkCompatibilityReceipt& receipt, ForkCompatibilityField field) {
    return std::any_of(receipt.witnesses().begin(), receipt.witnesses().end(),
                       [field](const auto& witness) { return witness.field == field; });
}

CapabilityBindingReceipt binding(char implementation = 'b', char resource = 'c') {
    return CapabilityBindingReceipt{
        ExecutableIdentity{ExecutableKind::Provider, "recorded-provider", "1.0.0",
                           digest(implementation)},
        digest(resource)};
}

RecordedCapabilityCallReference call_reference(
    CapabilityBindingReceipt   receipt,
    std::uint64_t              sequence,
    std::string                call_id,
    std::optional<std::string> effect_id = std::nullopt) {
    return RecordedCapabilityCallReference{sequence, std::move(receipt), "call_core:main",
                                           std::move(call_id), std::move(effect_id)};
}

ProgramPendingInput consumed_input(const RecordedCapabilityCallReference& reference, json result) {
    ProgramPendingInputData data;
    data.operation_id         = reference.operation_id;
    data.call_id              = reference.call_id;
    data.kind                 = ProgramPendingInputKind::CapabilityResult;
    data.result_schema        = json::object();
    data.payload              = json::object();
    data.core_node            = "work";
    data.core_interrupt_value = json::object();
    data.state                = ProgramPendingState::Consumed;
    data.consumed_result      = std::move(result);
    return ProgramPendingInput(std::move(data));
}

ProgramPendingEffect failed_effect(const RecordedCapabilityCallReference& reference) {
    ProgramPendingEffectData data;
    data.operation_id         = reference.operation_id;
    data.call_id              = reference.call_id;
    data.effect_id            = *reference.effect_id;
    data.result_schema        = json::object();
    data.payload              = json::object();
    data.effect_mode          = EffectMode::Brokered;
    data.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    data.core_node            = "work";
    data.core_interrupt_value = json::object();
    data.state                = ProgramPendingState::Consumed;
    data.reconciliation       = ProgramEffectReconciliation::Failed;
    return ProgramPendingEffect(std::move(data));
}

CatalogCapabilityBinding owned_binding(std::vector<CapabilityBindingReceipt> receipts) {
    CatalogCapabilityBinding result;
    result.receipts = std::move(receipts);
    return result;
}

class ReplayProvider final : public neograph::Provider {
public:
    ReplayProvider(std::atomic<unsigned>& calls, std::string content, bool fail)
        : calls_(calls), content_(std::move(content)), fail_(fail) {}

    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        ++calls_;
        if (fail_) throw std::runtime_error("recorded provider failure");
        neograph::ChatCompletion completion;
        completion.message.role    = "assistant";
        completion.message.content = content_;
        return completion;
    }

    std::string get_name() const override { return "recorded-test"; }

private:
    std::atomic<unsigned>& calls_;
    std::string            content_;
    bool                   fail_;
};

class ReplayProviderNode final : public GraphNode {
public:
    ReplayProviderNode(std::string name, std::shared_ptr<neograph::Provider> provider)
        : name_(std::move(name)), provider_(std::move(provider)) {
        if (!provider_) throw std::invalid_argument("ReplayProviderNode requires a provider");
    }

    asio::awaitable<NodeOutput> run(NodeInput) override {
        neograph::CompletionParams params;
        const auto completion = co_await provider_->complete_async(params);
        NodeOutput                       output;
        output.writes.push_back(ChannelWrite{"value", completion.message.content});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string                         name_;
    std::shared_ptr<neograph::Provider> provider_;
};

ExecutableManifest replay_manifest(ExecutableIdentity              identity,
                                   std::vector<ExecutableIdentity> dependencies = {}) {
    return ExecutableManifest{
        std::move(identity),    EffectMode::Brokered, "attestation:recorded", {}, {},
        std::move(dependencies)};
}

RegistrySnapshot replay_registry() {
    const ExecutableIdentity provider{ExecutableKind::Provider, "recorded-provider", "1.0.0",
                                      digest('b')};
    RegistrySnapshotBuilder  builder;
    builder.add_provider(replay_manifest(provider),
                         ProviderMetadata{json::object(), json::object()});
    builder.add_node(
        replay_manifest(
            ExecutableIdentity{ExecutableKind::Node, "recorded-node", "1.0.0", digest('f')},
            {provider}),
        [](const std::string& name, const json&, const NodeContext& context) {
            return std::make_unique<ReplayProviderNode>(name, context.provider);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(replay_manifest(ExecutableIdentity{
                            ExecutableKind::Reducer, "recorded-overwrite", "1.0.0", digest('e')}),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

json replay_program_document() {
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root", json{{"op", "call_core"},
                      {"name", "main"},
                      {"definition",
                       json{{"schema_version", 1},
                            {"name", "main"},
                            {"channels", json{{"value", json{{"reducer", "recorded-overwrite"},
                                                             {"initial", ""}}}}},
                            {"nodes", json{{"work", json{{"type", "recorded-node"}}}}},
                            {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                                   json{{"from", "work"}, {"to", "__end__"}}})},
                            {"conditional_edges", json::array()}}}}},
        {"declared_budget_requirements",
         json::array({
             json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
             json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
             json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
             json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
             json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
             json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
             json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
             json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
         })}};
}

RunBudget replay_budget() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};
}

RunInvocation replay_invocation(const ProgramVersion& version,
                                std::string           run_id,
                                std::string           correlation_id) {
    RunInvocation invocation;
    invocation.owner_scope        = "tenant:recorded";
    invocation.agent_id           = "recorded-replay";
    invocation.program_version_id = version.id();
    invocation.run_id             = std::move(run_id);
    invocation.budget             = replay_budget();
    invocation.input              = json::object();
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "recorded-replay:" + invocation.run_id;
    invocation.correlation_id     = std::move(correlation_id);
    invocation.validate();
    return invocation;
}

struct RecordedRuntimeFixture {
    std::atomic<unsigned>                           live_binder_calls{0};
    std::atomic<unsigned>                           live_provider_calls{0};
    std::atomic<unsigned>                           recorded_provider_calls{0};
    RegistrySnapshot                                registry;
    AdmissionProfile                                profile;
    PolicySnapshot                                  policy;
    std::shared_ptr<InMemoryProgramStore>           store;
    std::shared_ptr<InMemoryCheckpointStore>        checkpoints;
    std::shared_ptr<InMemoryProgramTransitionStore> transitions;
    std::shared_ptr<EngineGenerationCache>          engines;
    std::shared_ptr<ProgramCatalog>                 catalog;
    std::unique_ptr<ProgramRuntime>                 runtime;

    RecordedRuntimeFixture()
        : registry(replay_registry()),
          profile(make_profile(registry)),
          policy(make_policy(profile)),
          store(std::make_shared<InMemoryProgramStore>()),
          checkpoints(std::make_shared<InMemoryCheckpointStore>()),
          transitions(std::make_shared<InMemoryProgramTransitionStore>()) {
        restart();
    }

    static AdmissionProfile make_profile(const RegistrySnapshot& registry) {
        AdmissionProfileBuilder builder;
        builder.id("recorded-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(1)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry.identities())
            builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(const AdmissionProfile& profile) {
        PolicySnapshotBuilder builder;
        builder.id("recorded-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:recorded")
            .admission_profile(profile)
            .budget_ceiling(BudgetLimits{10000, 1000, 1000, 1, 1, 20, 1, 1, 1});
        return std::move(builder).build();
    }

    CatalogCapabilityBinding make_binding(bool recorded, bool failure) {
        CatalogCapabilityBinding result;
        result.node_context.provider = std::make_shared<ReplayProvider>(
            recorded ? recorded_provider_calls : live_provider_calls,
            failure ? "unused" : "recorded-success", failure);
        result.receipts = {binding()};
        return result;
    }

    void restart() {
        runtime.reset();
        catalog.reset();
        engines = std::make_shared<EngineGenerationCache>();
        catalog = std::make_shared<ProgramCatalog>(
            CatalogConfig{store, registry, engines, "recorded-runtime-test/v1",
                          [this](const std::vector<ExecutableIdentity>&) {
                              ++live_binder_calls;
                              return make_binding(false, false);
                          },
                          1});
        runtime = std::make_unique<ProgramRuntime>(
            RuntimeConfig{catalog, checkpoints, {}, transitions, 1});
    }

    ProgramVersion admit() {
        ProgramCompiler compiler(registry, {"recorded-runtime-test/v1"});
        auto            source =
            ProgramSource::from_cpp_builder("test:recorded", 1, replay_program_document());
        auto bundle = compiler.compile(source);
        return catalog->admit(bundle, ProgramAdmission{"tenant:recorded", profile, policy, {}});
    }
};

TEST(ProgramForkValuesTest, CompatibleExactCheckpointProducesCanonicalReceipt) {
    const auto receipt = check_exact_fork_compatibility(compatible_facts());

    ASSERT_TRUE(receipt.compatible());
    EXPECT_TRUE(receipt.witnesses().empty());
    EXPECT_EQ(receipt.source_run_id(), "source-run");
    EXPECT_EQ(receipt.source_checkpoint_id(), "checkpoint-source");

    const auto reparsed = ForkCompatibilityReceipt::parse(receipt.serialize_canonical());
    EXPECT_EQ(reparsed.id(), receipt.id());
    EXPECT_TRUE(reparsed.compatible());
}

TEST(ProgramForkValuesTest, InitialResumeBindingIsCanonicalImmutableAndTamperEvident) {
    const auto compatibility = check_exact_fork_compatibility(compatible_facts());
    ASSERT_FALSE(compatibility.initial_resume_binding().has_value());

    const auto bound = compatibility.with_initial_resume_binding(
        std::string("pending-source"), json{{"approved", true}, {"reason", "fork"}});
    ASSERT_TRUE(bound.initial_resume_binding().has_value());
    ASSERT_TRUE(bound.initial_resume_binding()->target_pending_id.has_value());
    EXPECT_EQ(*bound.initial_resume_binding()->target_pending_id, "pending-source");
    EXPECT_NE(bound.id(), compatibility.id());
    EXPECT_TRUE(bound.matches_initial_resume("pending-source",
                                             json{{"reason", "fork"}, {"approved", true}}));
    EXPECT_FALSE(bound.matches_initial_resume("pending-source",
                                              json{{"approved", false}, {"reason", "fork"}}));
    EXPECT_FALSE(bound.matches_initial_resume("pending-other",
                                              json{{"approved", true}, {"reason", "fork"}}));
    EXPECT_THROW((void)bound.with_initial_resume_binding(std::string("pending-other"),
                                                         json{{"approved", true}}),
                 std::invalid_argument);

    const auto reparsed = ForkCompatibilityReceipt::parse(bound.serialize_canonical());
    EXPECT_EQ(reparsed.id(), bound.id());
    EXPECT_EQ(reparsed.initial_resume_binding(), bound.initial_resume_binding());

    auto tampered = json::parse(bound.serialize_canonical());
    tampered["initial_resume_binding"]["resume_value_identity"] = digest('f');
    EXPECT_THROW((void)ForkCompatibilityReceipt::parse(tampered.dump()), std::invalid_argument);
}

TEST(ProgramForkValuesTest, LegacyReceiptRoundTripPreservesStoredIdentity) {
    const auto legacy_id =
        "sha256:2134081484672066b89f738c7a827c8a111e10d2e5f41fd99b3552c2df782d09";
    json       legacy{{"format", "neograph-program-fork-compatibility"},
                      {"id", legacy_id},
                      {"owner_scope", "tenant:fork"},
                      {"source_checkpoint_id", "checkpoint-source"},
                      {"source_program_version_id", digest('1')},
                      {"source_run_id", "source-run"},
                      {"status", "compatible"},
                      {"storage_schema_version", 1},
                      {"target_program_version_id", digest('2')},
                      {"witnesses", json::array()}};
    const auto stored_bytes = legacy.dump();

    const auto reparsed = ForkCompatibilityReceipt::parse(stored_bytes);
    EXPECT_EQ(reparsed.storage_schema_version(), 1U);
    EXPECT_EQ(reparsed.id(), legacy_id);
    EXPECT_FALSE(reparsed.initial_resume_binding().has_value());
    EXPECT_EQ(reparsed.serialize_canonical(), stored_bytes);
}

TEST(ProgramForkValuesTest, ChannelReducerAndContinuationMismatchesAreTyped) {
    {
        auto facts = compatible_facts();
        facts.target_core_definition.definition["channels"]["extra"] =
            json{{"reducer", "overwrite"}, {"initial", nullptr}};
        facts.target_core_definition.definition_hash =
            sealed_core_definition_hash(facts.target_core_definition.definition);
        const auto receipt = check_exact_fork_compatibility(std::move(facts));
        EXPECT_FALSE(receipt.compatible());
        EXPECT_TRUE(has_field(receipt, ForkCompatibilityField::Channel));
    }
    {
        auto facts = compatible_facts();
        facts.target_core_definition.definition["channels"]["value"]["reducer"] = "append";
        facts.target_core_definition.definition_hash =
            sealed_core_definition_hash(facts.target_core_definition.definition);
        const auto receipt = check_exact_fork_compatibility(std::move(facts));
        EXPECT_FALSE(receipt.compatible());
        EXPECT_TRUE(has_field(receipt, ForkCompatibilityField::Reducer));
    }
    {
        auto facts = compatible_facts();
        facts.target_core_definition.definition["edges"] =
            json::array({json{{"from", "__start__"}, {"to", "__end__"}}});
        facts.target_core_definition.definition_hash =
            sealed_core_definition_hash(facts.target_core_definition.definition);
        const auto receipt = check_exact_fork_compatibility(std::move(facts));
        EXPECT_FALSE(receipt.compatible());
        EXPECT_TRUE(has_field(receipt, ForkCompatibilityField::Continuation));
    }
}

TEST(ProgramRecordedReplayValuesTest, FullSuccessAndFailureEvidenceRoundTrip) {
    const auto                 receipt     = binding();
    const auto                 success_ref = call_reference(receipt, 1, "call-success");
    RecordedCapabilityEvidence success(RecordedCapabilityEvidenceData{
        success_ref, RecordedEvidenceCoverage::Full, false,
        consumed_input(success_ref, json{{"answer", 42}}), std::nullopt, std::nullopt});
    const auto success_reparsed = RecordedCapabilityEvidence::parse(success.serialize_canonical());
    ASSERT_TRUE(success_reparsed.input_outcome().has_value());
    EXPECT_EQ(success_reparsed.input_outcome()->consumed_result(), (json{{"answer", 42}}));

    const auto failure_ref =
        call_reference(receipt, 2, "call-failed", std::string("effect-failed"));
    RecordedCapabilityEvidence failure(
        RecordedCapabilityEvidenceData{failure_ref, RecordedEvidenceCoverage::Full, false,
                                       std::nullopt, failed_effect(failure_ref), std::nullopt});
    const auto failure_reparsed = RecordedCapabilityEvidence::parse(failure.serialize_canonical());
    ASSERT_TRUE(failure_reparsed.effect_outcome().has_value());
    EXPECT_EQ(failure_reparsed.effect_outcome()->reconciliation(),
              ProgramEffectReconciliation::Failed);
}

TEST(ProgramRecordedReplayValuesTest, ExactFullOrderedSetOwnsBindingWithoutLiveBinder) {
    const auto                              receipt = binding();
    const auto                              first   = call_reference(receipt, 1, "call-first");
    const auto                              second  = call_reference(receipt, 2, "call-second");
    std::vector<RecordedCapabilityEvidence> evidence;
    evidence.emplace_back(RecordedCapabilityEvidenceData{
        first, RecordedEvidenceCoverage::Full, false, consumed_input(first, json{{"value", 1}}),
        std::nullopt, std::nullopt});
    evidence.emplace_back(RecordedCapabilityEvidenceData{
        second, RecordedEvidenceCoverage::Full, false, consumed_input(second, json{{"value", 2}}),
        std::nullopt, std::nullopt});

    RecordedBindingSet set({receipt}, {first, second}, owned_binding({receipt}),
                           std::move(evidence));
    EXPECT_TRUE(set.fingerprint().starts_with("sha256:"));
    EXPECT_EQ(set.evidence().size(), 2U);
    auto released = std::move(set).release_owned_binding();
    EXPECT_EQ(released.receipts, std::vector<CapabilityBindingReceipt>{receipt});
}

TEST(ProgramRecordedReplayValuesTest, RedactedMissingUnorderedAndWrongBindingReject) {
    const auto receipt = binding();
    const auto other   = binding('d', 'e');
    const auto first   = call_reference(receipt, 1, "call-first");
    const auto second  = call_reference(receipt, 2, "call-second");

    EXPECT_THROW(
        (RecordedBindingSet(
            {receipt}, {first}, owned_binding({receipt}),
            {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
                first, RecordedEvidenceCoverage::Full, true, std::nullopt, std::nullopt,
                RecordedCapabilityFailure{"P_REDACTED", "recorded", "hidden", json::object()}})})),
        std::invalid_argument);

    EXPECT_THROW((RecordedBindingSet({receipt}, {first}, owned_binding({receipt}), {})),
                 std::invalid_argument);

    std::vector<RecordedCapabilityEvidence> reversed;
    reversed.emplace_back(RecordedCapabilityEvidenceData{second, RecordedEvidenceCoverage::Full,
                                                         false, consumed_input(second, 2),
                                                         std::nullopt, std::nullopt});
    reversed.emplace_back(RecordedCapabilityEvidenceData{first, RecordedEvidenceCoverage::Full,
                                                         false, consumed_input(first, 1),
                                                         std::nullopt, std::nullopt});
    EXPECT_THROW((RecordedBindingSet({receipt}, {first, second}, owned_binding({receipt}),
                                     std::move(reversed))),
                 std::invalid_argument);

    const auto wrong_ref = call_reference(other, 1, "call-first");
    EXPECT_THROW(
        (RecordedBindingSet({receipt}, {wrong_ref}, owned_binding({receipt}),
                            {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
                                wrong_ref, RecordedEvidenceCoverage::Full, false,
                                consumed_input(wrong_ref, 1), std::nullopt, std::nullopt})})),
        std::invalid_argument);
}

TEST(ProgramRecordedReplayRuntimeTest,
     RecordedSuccessAndFailureSurviveRestartWithoutLiveBindingCalls) {
    RecordedRuntimeFixture fixture;
    const auto             version = fixture.admit();
    ASSERT_EQ(version.core_materialization_receipt().capability_bindings.size(), 1U);
    const auto exact_receipt = version.core_materialization_receipt().capability_bindings.front();

    fixture.restart();
    fixture.live_binder_calls.store(0);
    fixture.live_provider_calls.store(0);
    fixture.recorded_provider_calls.store(0);
    const auto success_ref = call_reference(exact_receipt, 1, "recorded-success-call");
    std::vector<RecordedCapabilityEvidence> success_evidence;
    success_evidence.emplace_back(RecordedCapabilityEvidenceData{
        success_ref, RecordedEvidenceCoverage::Full, false,
        consumed_input(success_ref, json{{"content", "recorded-success"}}), std::nullopt,
        std::nullopt});
    RecordedBindingSet success_set({exact_receipt}, {success_ref},
                                   fixture.make_binding(true, false), std::move(success_evidence));
    const auto         success_fingerprint = success_set.fingerprint();

    const auto success_invocation =
        replay_invocation(version, "recorded-success-run", "trace-recorded-success");
    const auto success =
        fixture.runtime->start_recorded(success_invocation, std::move(success_set)).wait();
    EXPECT_EQ(success.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(success.output()["channels"]["value"]["value"], "recorded-success");
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls.load(), 0U);
    EXPECT_EQ(fixture.recorded_provider_calls.load(), 1U);
    const auto success_record = fixture.transitions->load("tenant:recorded", success.run_id());
    ASSERT_TRUE(success_record.has_value());
    ASSERT_TRUE(success_record->recorded_binding_set_fingerprint().has_value());
    EXPECT_EQ(*success_record->recorded_binding_set_fingerprint(), success_fingerprint);
    EXPECT_EQ(success_record->invocation(), success_invocation);

    fixture.restart();
    const auto reconnected_success =
        fixture.runtime->reconnect("tenant:recorded", success.run_id()).wait();
    EXPECT_EQ(reconnected_success.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(reconnected_success.output(), success.output());
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls.load(), 0U);

    fixture.recorded_provider_calls.store(0);
    const auto failure_ref = call_reference(exact_receipt, 1, "recorded-failure-call");
    std::vector<RecordedCapabilityEvidence> failure_evidence;
    failure_evidence.emplace_back(RecordedCapabilityEvidenceData{
        failure_ref, RecordedEvidenceCoverage::Full, false, std::nullopt, std::nullopt,
        RecordedCapabilityFailure{"P_RECORDED_PROVIDER", "provider", "recorded provider failure",
                                  json::object()}});
    RecordedBindingSet failure_set({exact_receipt}, {failure_ref}, fixture.make_binding(true, true),
                                   std::move(failure_evidence));

    const auto failure_invocation =
        replay_invocation(version, "recorded-failure-run", "trace-recorded-failure");
    const auto failure =
        fixture.runtime->start_recorded(failure_invocation, std::move(failure_set)).wait();
    EXPECT_EQ(failure.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls.load(), 0U);
    EXPECT_EQ(fixture.recorded_provider_calls.load(), 1U);
    const auto failure_record = fixture.transitions->load("tenant:recorded", failure.run_id());
    ASSERT_TRUE(failure_record.has_value());
    EXPECT_EQ(failure_record->invocation(), failure_invocation);

    fixture.restart();
    const auto reconnected_failure =
        fixture.runtime->reconnect("tenant:recorded", failure.run_id()).wait();
    EXPECT_EQ(reconnected_failure.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(reconnected_failure.failure().has_value());
    EXPECT_EQ(reconnected_failure.failure()->code, failure.failure()->code);
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls.load(), 0U);
}

TEST(ProgramRecordedReplayRuntimeTest, WrongTargetBindingRejectsBeforeRunAndNeverFallsBackLive) {
    RecordedRuntimeFixture fixture;
    const auto             version = fixture.admit();
    fixture.restart();
    fixture.live_binder_calls.store(0);
    fixture.live_provider_calls.store(0);
    fixture.recorded_provider_calls.store(0);

    const auto wrong_receipt = binding('d', 'e');
    const auto wrong_ref     = call_reference(wrong_receipt, 1, "wrong-target-call");
    std::vector<RecordedCapabilityEvidence> evidence;
    evidence.emplace_back(RecordedCapabilityEvidenceData{
        wrong_ref, RecordedEvidenceCoverage::Full, false,
        consumed_input(wrong_ref, json{{"content", "wrong"}}), std::nullopt, std::nullopt});
    RecordedBindingSet wrong_set({wrong_receipt}, {wrong_ref}, owned_binding({wrong_receipt}),
                                 std::move(evidence));

    EXPECT_THROW(
        (void)fixture.runtime->start_recorded(
            "tenant:recorded", version,
            ProgramInvocation{
                json::object(), replay_budget(), "trace-wrong-recorded", {}, "recorded-wrong-run"},
            std::move(wrong_set)),
        std::invalid_argument);
    EXPECT_FALSE(fixture.transitions->load("tenant:recorded", "recorded-wrong-run").has_value());
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls.load(), 0U);
    EXPECT_EQ(fixture.recorded_provider_calls.load(), 0U);
}
}  // namespace
