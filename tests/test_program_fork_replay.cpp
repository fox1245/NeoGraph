#include "fixtures/typed_provider.h"
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#include <neograph/provider_outcome_codec.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <limits>
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


class ReplayProvider final : public neograph::test::LocalProvider {
public:
    ReplayProvider(std::shared_ptr<std::atomic<unsigned>> calls, std::string content, bool fail)
        : LocalProvider([calls = std::move(calls), content = std::move(content), fail](
                            auto, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              ++*calls;
              if (fail) co_return neograph::test::failure(sp::ErrorKind::RemoteFailure);
              co_return neograph::test::success(content);
          }, "recorded-provider") {}
};

class ReplayProviderNode final : public GraphNode {
public:
    ReplayProviderNode(std::string name, std::shared_ptr<neograph::Provider> provider)
        : name_(std::move(name)), provider_(std::move(provider)) {
        if (!provider_) throw std::invalid_argument("ReplayProviderNode requires a provider");
    }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto result = co_await observe_provider_result(
            in.ctx, provider_->invoke_async(neograph::test::request()));
        if (!result) throw std::runtime_error("Provider returned no outcome");
        record_usage(in.ctx, result);
        NodeOutput output;
        output.writes.push_back(ChannelWrite{
            "value", neograph::provider_codec::encode_outcome(*result)});
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
    auto recorded_node = replay_manifest(
        ExecutableIdentity{ExecutableKind::Node, "recorded-node", "1.0.0", digest('f')},
        {provider});
    recorded_node.effect_mode = EffectMode::TrustedNative;
    recorded_node.attestation_id = "host:recorded-runtime-test";
    builder.add_node(
        std::move(recorded_node),
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
    std::shared_ptr<std::atomic<unsigned>> live_provider_calls =
        std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<std::atomic<unsigned>> recorded_provider_calls =
        std::make_shared<std::atomic<unsigned>>(0);
    RegistrySnapshot                                registry;
    AdmissionProfile                                profile;
    PolicySnapshot                                  policy;
    std::shared_ptr<InMemoryProgramStore>           store;
    std::shared_ptr<InMemoryCheckpointStore>        checkpoints;
    std::shared_ptr<InMemoryProgramTransitionStore> transitions;
    std::shared_ptr<EngineGenerationCache>          engines;
    std::shared_ptr<ProgramCatalog>                 catalog;
    std::unique_ptr<ProgramRuntime>                 runtime;
    bool fail_live = false;
    std::shared_ptr<neograph::Provider> live_override;

    RecordedCapabilityMaterialization captured(
        const ProgramVersion& version, const std::vector<ProgramEvent>& events) {
        const auto receipt = version.core_materialization_receipt().capability_bindings.front();
        for (const auto& event : events) {
            const auto* typed = std::get_if<neograph::graph::TypedGraphEvent>(&event.payload);
            if (!typed) continue;
            const auto* write = std::get_if<neograph::graph::ChannelWriteEvent>(typed);
            if (!write || write->channel != "value") continue;
            auto reference = call_reference(receipt, 1, "source-provider-call");
            reference.operation_id = event.operation_id;
            RecordedBindingSet evidence(
                {receipt}, {reference},
                {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
                    reference, RecordedEvidenceCoverage::Full, false,
                    consumed_input(reference, write->value), std::nullopt, std::nullopt})});
            const auto outcome = neograph::provider_codec::decode_outcome(write->value);
            CatalogCapabilityBinding captured_binding;
            captured_binding.receipts = {receipt};
            captured_binding.node_context.provider =
                std::make_shared<neograph::test::LocalProvider>(
                    [outcome, calls = recorded_provider_calls](auto, const auto&, const auto&)
                        -> asio::awaitable<sp::runtime::Result> {
                        ++*calls;
                        co_return outcome;
                    }, "recorded-provider");
            return {std::move(evidence), std::move(captured_binding)};
        }
        throw std::invalid_argument("Source has no captured provider outcome");
    }

    explicit RecordedRuntimeFixture(std::uint64_t operation_limit = 3)
        : registry(replay_registry()),
          profile(make_profile(registry)),
          policy(make_policy(profile, operation_limit)),
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
            .mode(AdmissionMode::TrustedEmbedding)
            .max_program_schema_version(2)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered)
            .allow_effect_mode(EffectMode::TrustedNative);
        for (const auto& identity : registry.identities())
            builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(const AdmissionProfile& profile, std::uint64_t operation_limit) {
        PolicySnapshotBuilder builder;
        builder.id("recorded-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:recorded")
            .admission_profile(profile)
            .allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY))
            .budget_ceiling(BudgetLimits{10000, 1000, 1000, 1, operation_limit, 20, 1, 1, 1});
        return std::move(builder).build();
    }

    CatalogCapabilityBinding make_live_binding() {
        CatalogCapabilityBinding result;
        result.node_context.provider = live_override ? live_override
            : std::make_shared<ReplayProvider>(live_provider_calls, "observed-source", fail_live);
        result.receipts = {binding()};
        return result;
    }

    void restart(bool captured_dispatch = true) {
        runtime.reset();
        catalog.reset();
        engines = std::make_shared<EngineGenerationCache>();
        CatalogConfig config{store, registry, engines, "recorded-runtime-test/v1",
                             [this](const std::vector<ExecutableIdentity>&) {
                                 ++live_binder_calls;
                                 return make_live_binding();
                             },
                             1, {}, "host:recorded-runtime-test"};
        if (captured_dispatch)
            config.recorded_capability_binder =
                [this](const ProgramVersion& version, const std::vector<ProgramEvent>& events) {
                    return captured(version, events);
                };
        catalog = std::make_shared<ProgramCatalog>(std::move(config));
        runtime = std::make_unique<ProgramRuntime>(
            RuntimeConfig{catalog, checkpoints, {}, transitions, 1});
    }

    ProgramVersion admit(json document = replay_program_document()) {
        ProgramCompiler compiler(registry, {"recorded-runtime-test/v1"});
        const auto schema = document.at("program_schema_version").get<std::uint32_t>();
        auto            source =
            ProgramSource::from_cpp_builder("test:recorded", schema, std::move(document));
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

TEST(ProgramRecordedReplayValuesTest, DurableBankKeepsWideChargeAndSeparateUnknownHold) {
    neograph::UsageAccumulator bank;
    ASSERT_TRUE(bank.try_reserve(1, 2));
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    bank.add(neograph::test::usage(0, maximum, maximum));
    ASSERT_TRUE(bank.remember_provider_effect("owner:source:wide-effect"));
    ProgramResultData data;
    data.status = ProgramTerminalStatus::Completed;
    data.run_id = "wide-provider-custody";
    data.program_version_id = digest('a');
    data.bundle_id = digest('b');
    data.attempt = 1;
    data.usage.model_tokens = bank.total_tokens_wide();
    data.provider_budget_authority = bank.authority_snapshot();
    const auto result = ProgramResult::create(data);
    const auto restored = ProgramResult::parse(result.serialize_canonical());
    ASSERT_TRUE(restored.provider_budget_authority());
    const auto& custody = *restored.provider_budget_authority();
    EXPECT_EQ(custody.charged, maximum);
    EXPECT_EQ(custody.reserved, 1U);
    ASSERT_TRUE(custody.reports.output_total);
    EXPECT_EQ(custody.reports.output_total->value, maximum);
    EXPECT_EQ(custody.provider_effects,
              std::vector<std::string>({"owner:source:wide-effect"}));
    EXPECT_EQ(restored.usage().model_tokens, maximum);
    data.usage.model_tokens = 0;
    EXPECT_THROW(ProgramResult::create(std::move(data)), std::invalid_argument);
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

TEST(ProgramRecordedReplayValuesTest, RedactedMissingUnorderedAndWrongBindingReject) {
    const auto receipt = binding();
    const auto other   = binding('d', 'e');
    const auto first   = call_reference(receipt, 1, "call-first");
    const auto second  = call_reference(receipt, 2, "call-second");

    EXPECT_THROW(
        (RecordedBindingSet(
            {receipt}, {first},
            {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
                first, RecordedEvidenceCoverage::Full, true, std::nullopt, std::nullopt,
                RecordedCapabilityFailure{"P_REDACTED", "recorded", "hidden", json::object()}})})),
        std::invalid_argument);

    EXPECT_THROW((RecordedBindingSet({receipt}, {first}, {})),
                 std::invalid_argument);

    std::vector<RecordedCapabilityEvidence> reversed;
    reversed.emplace_back(RecordedCapabilityEvidenceData{second, RecordedEvidenceCoverage::Full,
                                                         false, consumed_input(second, 2),
                                                         std::nullopt, std::nullopt});
    reversed.emplace_back(RecordedCapabilityEvidenceData{first, RecordedEvidenceCoverage::Full,
                                                         false, consumed_input(first, 1),
                                                         std::nullopt, std::nullopt});
    EXPECT_THROW((RecordedBindingSet({receipt}, {first, second}, std::move(reversed))),
                 std::invalid_argument);

    const auto wrong_ref = call_reference(other, 1, "call-first");
    EXPECT_THROW(
        (RecordedBindingSet({receipt}, {wrong_ref},
                            {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
                                wrong_ref, RecordedEvidenceCoverage::Full, false,
                                consumed_input(wrong_ref, 1), std::nullopt, std::nullopt})})),
        std::invalid_argument);
}

TEST(ProgramRecordedReplayRuntimeTest,
     RegisteredCapturedDispatchRetainsActualSuccessAndFailureAcrossRestart) {
    for (const bool fail : {false, true}) {
        RecordedRuntimeFixture fixture;
        fixture.fail_live = fail;
        const auto version = fixture.admit();
        const auto source_invocation = replay_invocation(version, "captured-source", "source");
        const auto source = fixture.runtime->start(source_invocation).wait();
        ASSERT_EQ(source.status(), ProgramTerminalStatus::Completed);
        ASSERT_EQ(fixture.live_provider_calls->load(), 1U);
        const auto source_events = fixture.transitions->load_events("tenant:recorded", source.run_id());
        fixture.restart();
        fixture.live_binder_calls.store(0);
        fixture.live_provider_calls->store(0);
        auto evidence = fixture.captured(version, source_events).evidence;
        const auto fingerprint = evidence.fingerprint();
        auto invocation = source_invocation;
        invocation.run_id = "captured-replay";
        invocation.correlation_id = "replay";
        const auto replay =
            fixture.runtime->replay_recorded(source.run_id(), invocation, std::move(evidence)).wait();
        ASSERT_EQ(replay.status(), ProgramTerminalStatus::Completed);
        EXPECT_EQ(replay.output(), source.output());
        EXPECT_GT(replay.usage().core_steps, 0U);
        EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
        EXPECT_EQ(fixture.live_provider_calls->load(), 0U);
        EXPECT_EQ(fixture.recorded_provider_calls->load(), 1U);
        const auto record = fixture.transitions->load("tenant:recorded", replay.run_id());
        ASSERT_TRUE(record && record->recorded_binding_set_fingerprint());
        EXPECT_EQ(*record->recorded_binding_set_fingerprint(), fingerprint);
        EXPECT_EQ(record->invocation(), invocation);
        fixture.restart();
        const auto reconnected = fixture.runtime->reconnect("tenant:recorded", replay.run_id()).wait();
        EXPECT_EQ(reconnected.output(), replay.output());
        EXPECT_EQ(fixture.live_provider_calls->load(), 0U);
    }
}

TEST(ProgramRecordedReplayRuntimeTest,
     UnregisteredLiveExecutorRejectsBeforeEffectAndSourceDebit) {
    RecordedRuntimeFixture fixture;
    const auto version = fixture.admit();
    const auto source_invocation = replay_invocation(version, "unsafe-source", "source");
    const auto source = fixture.runtime->start(source_invocation).wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Completed);
    auto evidence = fixture.captured(
        version, fixture.transitions->load_events("tenant:recorded", source.run_id())).evidence;
    const auto source_head = fixture.transitions->load_run_lineage("tenant:recorded", source.run_id());
    ASSERT_TRUE(source_head);
    fixture.restart(false);
    fixture.live_binder_calls.store(0);
    fixture.live_provider_calls->store(0);
    auto invocation = source_invocation;
    invocation.run_id = "unsafe-target";
    EXPECT_THROW(fixture.runtime->replay_recorded(
        source.run_id(), invocation, std::move(evidence)), ProgramDiagnosticError);
    EXPECT_EQ(fixture.live_provider_calls->load(), 0U)
        << "the live executor effects before any reservation; it must never be called";
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_FALSE(fixture.transitions->load("tenant:recorded", invocation.run_id));
    EXPECT_EQ(fixture.transitions->load_run_lineage("tenant:recorded", source.run_id())->id(),
              source_head->id());
}

TEST(ProgramRecordedReplayRuntimeTest,
     ChangedCapturedOutcomeCannotAuthorizeReplayWithMatchingReceipts) {
    RecordedRuntimeFixture fixture;
    const auto version = fixture.admit();
    const auto source_invocation = replay_invocation(version, "tamper-source", "source");
    const auto source = fixture.runtime->start(source_invocation).wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Completed);
    auto captured = fixture.captured(
        version, fixture.transitions->load_events("tenant:recorded", source.run_id()));
    const auto reference = captured.evidence.expected_calls().front();
    RecordedBindingSet modified(
        captured.evidence.exact_bindings(), {reference},
        {RecordedCapabilityEvidence(RecordedCapabilityEvidenceData{
            reference, RecordedEvidenceCoverage::Full, false,
            consumed_input(reference, json{{"replacement", "not observed"}}),
            std::nullopt, std::nullopt})});
    const auto lineage = fixture.transitions->load_run_lineage("tenant:recorded", source.run_id());
    ASSERT_TRUE(lineage);
    const auto live_calls = fixture.live_provider_calls->load();
    auto invocation = source_invocation;
    invocation.run_id = "tamper-target";
    EXPECT_THROW(fixture.runtime->replay_recorded(
        source.run_id(), invocation, std::move(modified)), std::invalid_argument);
    EXPECT_EQ(fixture.live_provider_calls->load(), live_calls);
    EXPECT_EQ(fixture.recorded_provider_calls->load(), 0U);
    EXPECT_FALSE(fixture.transitions->load("tenant:recorded", invocation.run_id));
    EXPECT_EQ(fixture.transitions->load_run_lineage("tenant:recorded", source.run_id())->id(),
              lineage->id());
}

TEST(ProgramRecordedReplayRuntimeTest, WrongTargetBindingRejectsBeforeRunAndNeverFallsBackLive) {
    RecordedRuntimeFixture fixture;
    const auto             version = fixture.admit();
    const auto source_invocation = replay_invocation(version, "wrong-source", "source");
    const auto source = fixture.runtime->start(source_invocation).wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Completed);
    fixture.restart();
    fixture.live_binder_calls.store(0);
    fixture.live_provider_calls->store(0);
    fixture.recorded_provider_calls->store(0);

    const auto wrong_receipt = binding('d', 'e');
    const auto wrong_ref     = call_reference(wrong_receipt, 1, "wrong-target-call");
    std::vector<RecordedCapabilityEvidence> evidence;
    evidence.emplace_back(RecordedCapabilityEvidenceData{
        wrong_ref, RecordedEvidenceCoverage::Full, false,
        consumed_input(wrong_ref, json{{"content", "wrong"}}), std::nullopt, std::nullopt});
    RecordedBindingSet wrong_set({wrong_receipt}, {wrong_ref}, std::move(evidence));

    auto invocation = source_invocation;
    invocation.run_id = "recorded-wrong-run";
    EXPECT_THROW(
        (void)fixture.runtime->replay_recorded(source.run_id(), invocation, std::move(wrong_set)),
        std::invalid_argument);
    EXPECT_FALSE(fixture.transitions->load("tenant:recorded", "recorded-wrong-run").has_value());
    EXPECT_EQ(fixture.live_binder_calls.load(), 0U);
    EXPECT_EQ(fixture.live_provider_calls->load(), 0U);
    EXPECT_EQ(fixture.recorded_provider_calls->load(), 0U);
}

TEST(ProgramRecordedReplayRuntimeTest,
     PostEffectFailureRetainsOwnedCompletionAndOriginalCauseWithoutProgramRetry) {
    auto completion = std::get<sp::Completion>(*neograph::test::success(
        "actual response", neograph::test::usage(std::nullopt, 3, std::nullopt)));
    completion.attempt = {true, 91, true, 1, 2, true};
    completion.raw_events = {
        {"unknown.first", neograph::test::document(R"({"future":[null,18446744073709551615]})")},
        {"unknown.second", neograph::test::document(R"({"tail":true})")}};
    const auto actual = std::make_shared<const sp::Outcome>(std::move(completion));
    const auto original_cause = std::make_exception_ptr(std::runtime_error("settlement storage failed"));
    auto effects = std::make_shared<std::atomic<unsigned>>(0);
    // One retry operator plus up to three actual Core calls is a four-operation
    // admitted workload, even though the owned provider failure stops after one.
    RecordedRuntimeFixture fixture(4);
    fixture.live_override = std::make_shared<neograph::test::LocalProvider>(
        [actual, original_cause, effects](auto, const auto&, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            ++*effects;
            throw neograph::ProviderOutcomeError(
                "actual post-response settlement fault", actual, original_cause);
        }, "recorded-provider");
    auto document = replay_program_document();
    document["program_schema_version"] = 2U;
    document["declared_budget_requirements"][4]["maximum"] = 4U;
    auto definition = std::move(document["root"]["definition"]);
    document["root"] = json{{"op", "retry"}, {"name", "main"}, {"definition", std::move(definition)},
                            {"max_attempts", 3U}, {"body", json{{"op", "call_core"}}}};
    const auto version = fixture.admit(std::move(document));
    auto invocation = replay_invocation(version, "provider-failure-source", "source");
    invocation.budget.max_program_operations = 4;
    const auto result = fixture.runtime->start(invocation).wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    const auto failure = result.failure();
    ASSERT_TRUE(failure && failure->provider_outcome && failure->provider_cause);
    EXPECT_EQ(failure->provider_outcome, actual);
    EXPECT_EQ(effects->load(), 1U);
    try {
        std::rethrow_exception(failure->provider_cause);
    } catch (const neograph::ProviderOutcomeError& error) {
        EXPECT_EQ(error.outcome(), actual);
        EXPECT_EQ(error.cause(), original_cause);
    }
    const auto restored = ProgramResult::parse(result.serialize_canonical());
    const auto restored_failure = restored.failure();
    ASSERT_TRUE(restored_failure && restored_failure->provider_outcome);
    EXPECT_FALSE(restored_failure->provider_cause);
    EXPECT_EQ(neograph::provider_codec::observe_outcome(*restored_failure->provider_outcome),
              neograph::provider_codec::observe_outcome(*actual));
    EXPECT_EQ(restored.id(), result.id());
    fixture.restart();
    const auto reconnected = fixture.runtime->reconnect("tenant:recorded", result.run_id()).wait();
    ASSERT_TRUE(reconnected.failure() && reconnected.failure()->provider_outcome);
    EXPECT_EQ(neograph::provider_codec::observe_outcome(*reconnected.failure()->provider_outcome),
              neograph::provider_codec::observe_outcome(*actual));
    EXPECT_EQ(effects->load(), 1U);
}
}  // namespace
