#include <neograph/program/program.h>
#include <neograph/graph/engine.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#endif

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <climits>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using namespace neograph::program;

namespace {

std::string digest(char digit) {
    return "sha256:" + std::string(64, digit);
}

class CapsuleNode final : public GraphNode {
public:
    explicit CapsuleNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", json("captured")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

GraphSafePoint capture_safe_point(
    const std::shared_ptr<CheckpointStore>& store,
    std::string thread_id,
    GraphGenerationIdentity generation_identity) {
    NodeFactory::instance().register_type(
        "graph_migration_capsule_node",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CapsuleNode>(name);
        });
    const json graph = {
        {"name", "graph_migration_capsule"},
        {"channels", {{"value", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"capture", {{"type", "graph_migration_capsule_node"}}}}},
        {"edges",
         json::array({{{"from", "__start__"}, {"to", "capture"}},
                      {{"from", "capture"}, {"to", "__end__"}}})}};
    auto compiled = GraphCompiler::compile(graph, NodeContext{});
    EngineConfig engine_config;
    engine_config.checkpoint_store = store;
    auto engine = GraphEngine::link(
        std::move(compiled), std::move(engine_config), {}, generation_identity);
    auto request = std::make_shared<GraphSafePointRequest>(generation_identity);
    if (!request->request()) throw std::runtime_error("failed to arm safe point");
    RunConfig config;
    config.thread_id = std::move(thread_id);
    RunResources resources;
    resources.checkpoint_store = store;
    asio::io_context io;
    auto future = asio::co_spawn(
        io, engine->run_until_safe_point_async(config, request, {}, {}, resources),
        asio::use_future);
    io.run();
    const auto result = future.get();
    if (result.status() != RunStatus::SafePoint || !request->safe_point()) {
        throw std::runtime_error("safe point was not captured");
    }
    return *request->safe_point();
}

struct SourceGeneration {
    ProgramRunGeneration generation;
    ProgramRunLineage lineage;
};

struct AdmittedProgram {
    ProgramBundle bundle;
    ProgramVersion version;
};

AdmittedProgram admitted_program(
    std::string core_generation_id = digest('5'),
    bool widen_authority = false,
    std::optional<BudgetLimits> ceiling = std::nullopt) {
    RegistrySnapshotBuilder registry_builder;
    registry_builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "capsule-reducer", "1.0.0", digest('6')},
                           EffectMode::Brokered, "attestation:capsule", {}, {}},
        [](const json&, const json& incoming) { return json(incoming); });
    auto registry = std::move(registry_builder).build();

    AdmissionProfileBuilder admission_builder;
    admission_builder.id("capsule-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    auto admission = std::move(admission_builder).build();

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("capsule-policy")
        .semantic_version("1.0.0")
        .owner_scope("capsule-owner")
        .admission_profile(admission)
        .budget_ceiling(ceiling.value_or(
            BudgetLimits{1000, 1000, 1000, 1, 1, 20, 1, 1, 1}));
    if (widen_authority) policy_builder.allow_capability("capsule-expanded");
    auto policy = std::move(policy_builder).build();
    const json definition = {
        {"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
        {"nodes", json{{"capture", json{{"type", "graph_migration_capsule_node"}}}}}};
    ProgramBundleData bundle_data;
    bundle_data.source_kind = SourceKind::CppBuilder;
    bundle_data.source_hash = digest('1');
    bundle_data.canonical_program_hash = digest('2');
    bundle_data.compiler_build_id = "capsule-tests";
    bundle_data.program_schema_version = 1;
    bundle_data.registry_snapshot_fingerprint = registry.fingerprint();
    bundle_data.module_dependency_merkle_root = digest('3');
    bundle_data.input_contract = {1, json{{"type", "object"}}};
    bundle_data.output_contract = {1, json{{"type", "object"}}};
    bundle_data.orchestration_plan = {
        1,
        json{{"root", "root"},
             {"operations", json::array({
                 json{{"id", "root"}, {"op", "call_core"},
                      {"source_pointer", "/root"},
                      {"core", "graph_migration_capsule"}}})}}};
    bundle_data.sealed_core_definitions = {{
        "graph_migration_capsule", sealed_core_definition_hash(definition), definition}};
    bundle_data.core_plan_identities = {{
        "graph_migration_capsule", core_generation_id}};
    bundle_data.executable_registry_identities = registry.identities();
    bundle_data.declared_budget_requirements = {
        {"wall_time_ms", 0, 1000}, {"model_tokens", 0, 1000},
        {"monetary_microunits", 0, 1000}, {"max_concurrency", 1, 1},
        {"max_program_operations", 1, 1}, {"max_core_steps", 1, 20},
        {"max_dynamic_compiles", 0, 1}, {"max_child_depth", 0, 1},
        {"max_total_children", 0, 1}};
    auto bundle = ProgramBundle(std::move(bundle_data));
    auto version = ProgramVersion(ProgramVersionData{
        bundle.id(), admission, policy, {}, "capsule-owner",
        CoreMaterializationReceipt{
            "capsule-tests", registry.fingerprint(),
            {CorePlanIdentity{"graph_migration_capsule", std::move(core_generation_id)}}, {}}});
    return {std::move(bundle), std::move(version)};
}

ProgramVersion program_version(std::string core_generation_id = digest('5')) {
    auto admitted = admitted_program(std::move(core_generation_id));
    return std::move(admitted.version);
}

SourceGeneration source_generation(const ProgramVersion& version) {
    const std::string owner = "capsule-owner";
    const std::string run_id = "capsule-run";
    const auto lineage_id = program_run_lineage_id(owner, run_id);
    auto generation = ProgramRunGeneration::create(
        ProgramRunGenerationData{owner,
                                 lineage_id,
                                 1,
                                 run_id,
                                 version.id(),
                                 version.bundle_id(),
                                 digest('3'),
                                 digest('4'),
                                 std::nullopt,
                                 10,
                                 0,
                                 std::nullopt});
    auto lineage = ProgramRunLineage::create(
        ProgramRunLineageData{owner,
                              lineage_id,
                              run_id,
                              1,
                              generation.id(),
                              digest('3'),
                              digest('4'),
                              RunBudget{},
                              RunBudget{},
                              std::nullopt,
                              10,
                              10,
                              RunBudget{}});
    return {std::move(generation), std::move(lineage)};
}

GraphMigrationCapsule make_capsule() {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    const auto version = program_version();
    const auto source = source_generation(version);
    const GraphGenerationIdentity identity{
        "graph_migration_capsule", digest('5')};
    const auto safe_point = capture_safe_point(
        store,
        program_root_core_thread_id(source.generation.run_id(),
                                    identity.core_generation_id),
        identity);
    return GraphMigrationCapsule::seal(
        source.generation, source.lineage, version, safe_point);
}

RunBudget migration_budget() {
    return {1000, 1000, 1000, 1, 1, 20, 1, 1, 1};
}

ProgramEvent started_event(const ProgramRunRecord& run,
                           const CoreCheckpointIdentity& checkpoint,
                           std::int64_t timestamp) {
    ProgramEvent event;
    event.sequence = 1;
    event.timestamp_ms = timestamp;
    event.run_id = run.run_id();
    event.program_version_id = run.program_version_id();
    event.bundle_id = run.bundle_id();
    event.operation_id = "root";
    event.core_generation_id = checkpoint.core_generation_id;
    event.core_run_id = checkpoint.core_thread_id;
    event.trace_id = run.invocation().correlation_id;
    event.kind = ProgramEventKind::Started;
    event.payload = ProgramStartedEvent{run.remaining_budget()};
    event.attempt = run.continuation().attempt;
    return ProgramEvent::create(std::move(event));
}

ProgramTransitionPublication initial_publication(
    const ProgramVersion& version,
    std::string run_id,
    RunBudget budget,
    CoreCheckpointIdentity checkpoint,
    std::string checkpoint_content_id,
    std::int64_t timestamp,
    std::uint64_t attempt = 1) {
    auto journal = ProgramJournalRecord::create(
        {{}, run_id, version.id(), version.bundle_id(), 1,
         {"root", ContinuationState::Running, attempt}, budget, {}, checkpoint,
         timestamp});
    RunInvocation invocation;
    invocation.owner_scope = version.ownership_scope();
    invocation.agent_id = "capsule-agent";
    invocation.program_version_id = version.id();
    invocation.run_id = run_id;
    invocation.budget = budget;
    invocation.input = json{{"migration", true}};
    invocation.message_sequence = 1;
    invocation.idempotency_key = "capsule-migration";
    invocation.correlation_id = "trace-capsule-migration";

    ProgramRunRecordData data;
    data.owner_scope = version.ownership_scope();
    data.run_id = std::move(run_id);
    data.program_version_id = version.id();
    data.bundle_id = version.bundle_id();
    data.binding_fingerprint = capability_binding_receipt_root(
        version.core_materialization_receipt().capability_bindings);
    data.invocation = std::move(invocation);
    data.continuation = journal.continuation;
    data.remaining_budget = budget;
    data.exact_checkpoint = checkpoint;
    data.exact_checkpoint_content_id = std::move(checkpoint_content_id);
    data.journal_head = journal.id;
    data.event_sequence = 1;
    data.created_at_ms = timestamp;
    data.updated_at_ms = timestamp;
    auto run = ProgramRunRecord::create(std::move(data));
    auto started = started_event(run, checkpoint, timestamp);
    return {std::move(run), std::move(journal), {std::move(started)}, {}};
}

struct MigrationSourceFixture {
    ProgramBundle bundle;
    ProgramVersion version;
    std::shared_ptr<InMemoryCheckpointStore> checkpoints;
    ProgramTransitionPublication publication;
    GraphMigrationCapsule capsule;
};

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
struct TempMigrationDb {
    TempMigrationDb()
        : path(std::filesystem::temp_directory_path() /
               ("neograph-graph-migration-" + Checkpoint::generate_id() + ".db")) {
        std::filesystem::remove(path);
    }
    ~TempMigrationDb() { std::filesystem::remove(path); }

    std::filesystem::path path;
};
#endif

MigrationSourceFixture migration_source_fixture(bool valid_checkpoint_content = true) {
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto admitted = admitted_program();
    const GraphGenerationIdentity core_identity{
        "graph_migration_capsule", digest('5')};
    const auto source_thread = program_root_core_thread_id(
        "capsule-run", core_identity.core_generation_id);
    const auto safe_point = capture_safe_point(
        checkpoints, source_thread, core_identity);
    const CoreCheckpointIdentity checkpoint{
        core_identity.core_name, core_identity.core_generation_id,
        source_thread, safe_point.checkpoint().id,
        safe_point.checkpoint().schema_version};
    auto publication = initial_publication(
        admitted.version, "capsule-run", migration_budget(), checkpoint,
        valid_checkpoint_content
            ? graph_migration_checkpoint_content_id(safe_point.checkpoint())
            : digest('f'),
        safe_point.checkpoint().timestamp);
    const auto lineage_id = program_run_lineage_id(
        publication.run_record.owner_scope(), publication.run_record.run_id());
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        publication.run_record.owner_scope(), lineage_id, 1,
        publication.run_record.run_id(), publication.run_record.program_version_id(),
        publication.run_record.bundle_id(), publication.run_record.id(),
        publication.run_record.journal_head(), std::nullopt,
        publication.run_record.created_at_ms()});
    auto lineage = ProgramRunLineage::create(ProgramRunLineageData{
        publication.run_record.owner_scope(), lineage_id,
        publication.run_record.run_id(), 1, generation.id(),
        publication.run_record.id(), publication.run_record.journal_head(),
        publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, std::nullopt,
        publication.run_record.created_at_ms(), publication.run_record.updated_at_ms(),
        RunBudget{}});
    auto capsule = GraphMigrationCapsule::seal(
        generation, lineage, admitted.version, safe_point);
    publication.run_generation = std::move(generation);
    publication.run_lineage = std::move(lineage);
    return {std::move(admitted.bundle), std::move(admitted.version),
            std::move(checkpoints), std::move(publication), std::move(capsule)};
}

ProgramTransitionPublication migration_target_publication(
    const MigrationSourceFixture& source,
    std::string target_run_id,
    std::int64_t timestamp,
    std::optional<MigrationPlan> admitted_plan = std::nullopt,
    const AdmittedProgram* target_artifacts = nullptr,
    std::uint64_t target_attempt = 1) {
    const auto& predecessor = *source.publication.run_generation;
    const auto& previous_lineage = *source.publication.run_lineage;
    const auto target_timestamp = source.publication.run_record.updated_at_ms() + timestamp;
    const auto budget = program_replacement_remaining_budget(
        source.publication.run_record, previous_lineage, target_timestamp);
    auto cloned = source.capsule.checkpoint();
    cloned.id = Checkpoint::generate_id();
    cloned.thread_id = program_root_core_thread_id(
        target_run_id, source.capsule.core_checkpoint().core_generation_id);
    cloned.parent_id = source.capsule.core_checkpoint().checkpoint_id;
    cloned.metadata["migrated_from"] = source.capsule.id();
    ++cloned.timestamp;
    source.checkpoints->save(cloned);
    CoreCheckpointIdentity target_checkpoint{
        source.capsule.core_checkpoint().core_name,
        source.capsule.core_checkpoint().core_generation_id,
        cloned.thread_id, cloned.id, cloned.schema_version};
    const auto& target_bundle = target_artifacts
        ? target_artifacts->bundle : source.bundle;
    const auto& target_version = target_artifacts
        ? target_artifacts->version : source.version;
    auto publication = initial_publication(
        target_version, std::move(target_run_id), budget, target_checkpoint,
        graph_migration_checkpoint_content_id(cloned),
        target_timestamp, target_attempt);
    auto migration_plan = admitted_plan
        ? std::move(*admitted_plan)
        : MigrationPlan::between(source.version, source.bundle,
                                 target_version, target_bundle);
    auto receipt = ProgramGraphMigrationReceipt(ProgramGraphMigrationReceiptData{
        source.capsule, source.bundle, source.version, target_bundle, target_version,
        migration_plan.id(), predecessor.generation() + 1,
        publication.run_record.run_id(), publication.run_record.program_version_id(),
        publication.run_record.bundle_id(),
        publication.run_record.invocation().canonical_identity(),
        publication.run_record.binding_fingerprint(), publication.run_record.id(),
        publication.run_record.journal_head(), target_checkpoint, cloned});
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        publication.run_record.owner_scope(), previous_lineage.lineage_id(),
        predecessor.generation() + 1, publication.run_record.run_id(),
        publication.run_record.program_version_id(), publication.run_record.bundle_id(),
        publication.run_record.id(), publication.run_record.journal_head(),
        predecessor.id(), publication.run_record.created_at_ms(),
        publication.run_record.child_depth(), std::nullopt, std::move(receipt)});
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous_lineage.owner_scope(), previous_lineage.lineage_id(),
        previous_lineage.root_run_id(), generation.generation(), generation.id(),
        publication.run_record.id(), publication.run_record.journal_head(), budget,
        RunBudget{}, previous_lineage.id(), previous_lineage.created_at_ms(),
        publication.run_record.updated_at_ms(), RunBudget{}});
    publication.run_generation = std::move(generation);
    publication.migration_plan = std::move(migration_plan);
    return publication;
}

}  // namespace

TEST(GraphMigrationCapsule, CanonicalRoundTripBindsSourceAndCheckpoint) {
    const auto capsule = make_capsule();
    const auto parsed = GraphMigrationCapsule::parse(capsule.serialize_canonical());

    EXPECT_EQ(parsed.id(), capsule.id());
    EXPECT_EQ(parsed.serialize_canonical(), capsule.serialize_canonical());
    EXPECT_EQ(parsed.owner_scope(), "capsule-owner");
    EXPECT_EQ(parsed.source_generation(), 1U);
    EXPECT_EQ(parsed.source_run_id(), "capsule-run");
    EXPECT_EQ(parsed.source_bundle_id(), admitted_program().bundle.id());
    EXPECT_EQ(parsed.core_checkpoint().core_generation_id, digest('5'));
    EXPECT_EQ(parsed.core_checkpoint().checkpoint_id, parsed.checkpoint().id);
    EXPECT_EQ(parsed.checkpoint().interrupt_phase, CheckpointPhase::Completed);
    EXPECT_EQ(parsed.checkpoint().next_nodes, std::vector<std::string>{"__end__"});
    EXPECT_EQ(parsed.checkpoint().channel_values["channels"]["value"]["value"],
              "captured");
}

TEST(GraphMigrationCapsule, RejectsTamperedFrontierAndPhase) {
    const auto capsule = make_capsule();

    auto frontier = json::parse(capsule.serialize_canonical());
    frontier["checkpoint"]["next_nodes"] = json::array({"different"});
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(frontier.dump()), std::invalid_argument);

    auto phase = json::parse(capsule.serialize_canonical());
    phase["checkpoint"]["phase"] = "before";
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(phase.dump()), std::invalid_argument);
}

TEST(GraphMigrationCapsule, RejectsSafePointOutsideAdmittedVersion) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    const auto version = program_version(digest('7'));
    const auto source = source_generation(version);
    const GraphGenerationIdentity identity{
        "graph_migration_capsule", digest('5')};
    const auto safe_point = capture_safe_point(
        store,
        program_root_core_thread_id(source.generation.run_id(),
                                    identity.core_generation_id),
        identity);
    EXPECT_THROW(
        (void)GraphMigrationCapsule::seal(
            source.generation, source.lineage, version, safe_point),
        std::invalid_argument);
}

TEST(GraphMigrationCapsule, RejectsSafePointFromAnotherRootRun) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    const auto version = program_version();
    const auto source = source_generation(version);
    const GraphGenerationIdentity identity{
        "graph_migration_capsule", digest('5')};
    const auto safe_point = capture_safe_point(
        store, "another-core-thread", identity);
    EXPECT_THROW(
        (void)GraphMigrationCapsule::seal(
            source.generation, source.lineage, version, safe_point),
        std::invalid_argument);
}

TEST(GraphMigrationCapsule, RejectsMalformedRuntimeStateAndIntegerOverflow) {
    const auto capsule = make_capsule();

    auto versions = json::parse(capsule.serialize_canonical());
    versions["checkpoint"]["channel_versions"] =
        json{{"value", static_cast<unsigned long long>(99)}};
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(versions.dump()), std::invalid_argument);

    auto duplicate_barrier = json::parse(capsule.serialize_canonical());
    duplicate_barrier["checkpoint"]["barrier_state"]["join"] =
        json::array({"left", "left"});
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(duplicate_barrier.dump()),
        std::invalid_argument);

    auto overflowing_step = json::parse(capsule.serialize_canonical());
    overflowing_step["checkpoint"]["step"] = INT_MAX;
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(overflowing_step.dump()),
        std::invalid_argument);

    auto boundary_step = json::parse(capsule.serialize_canonical());
    boundary_step["checkpoint"]["step"] = INT_MAX - 1;
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(boundary_step.dump()),
        std::invalid_argument);

    auto impossible_version = json::parse(capsule.serialize_canonical());
    impossible_version["checkpoint"]["channel_values"]["channels"]["value"]["version"] =
        static_cast<unsigned long long>(2);
    impossible_version["checkpoint"]["channel_values"]["global_version"] =
        static_cast<unsigned long long>(1);
    EXPECT_THROW(
        (void)GraphMigrationCapsule::parse(impossible_version.dump()),
        std::invalid_argument);
}

TEST(ProgramGraphMigrationReceipt, CanonicalRoundTripBindsPlanAndTargetCheckpoint) {
    const auto source = migration_source_fixture();
    const auto target = migration_target_publication(source, "capsule-target", 20);
    ASSERT_TRUE(target.run_generation);
    const auto receipt = target.run_generation->graph_migration_receipt();
    ASSERT_TRUE(receipt);

    const auto parsed = ProgramGraphMigrationReceipt::parse(
        receipt->serialize_canonical());
    EXPECT_EQ(parsed.id(), receipt->id());
    EXPECT_EQ(parsed.capsule().id(), source.capsule.id());
    EXPECT_EQ(parsed.source_bundle().id(), source.bundle.id());
    EXPECT_EQ(parsed.source_version().id(), source.version.id());
    EXPECT_EQ(parsed.target_bundle().id(), source.bundle.id());
    EXPECT_EQ(parsed.target_version().id(), source.version.id());
    EXPECT_EQ(parsed.migration_plan_id(), target.migration_plan->id());
    EXPECT_EQ(parsed.target_generation(), 2U);
    EXPECT_EQ(parsed.target_run_id(), "capsule-target");
    EXPECT_EQ(parsed.target_core_checkpoint(), *target.run_record.exact_checkpoint());

    const auto parsed_generation = ProgramRunGeneration::parse(
        target.run_generation->serialize_canonical());
    ASSERT_TRUE(parsed_generation.graph_migration_receipt());
    EXPECT_FALSE(parsed_generation.replacement_receipt());
    EXPECT_EQ(parsed_generation.graph_migration_receipt()->id(), receipt->id());

    auto tampered = json::parse(receipt->serialize_canonical());
    tampered["target_binding_fingerprint"] = digest('b');
    EXPECT_THROW(
        (void)ProgramGraphMigrationReceipt::parse(tampered.dump()),
        std::invalid_argument);

    auto source_checkpoint_target = receipt->target_core_checkpoint();
    source_checkpoint_target.checkpoint_id =
        source.capsule.core_checkpoint().checkpoint_id;
    EXPECT_THROW(
        (void)ProgramGraphMigrationReceipt(ProgramGraphMigrationReceiptData{
            source.capsule, receipt->source_bundle(), receipt->source_version(),
            receipt->target_bundle(), receipt->target_version(),
            receipt->migration_plan_id(), receipt->target_generation(),
            receipt->target_run_id(), receipt->target_program_version_id(),
            receipt->target_bundle_id(), receipt->target_invocation_id(),
            receipt->target_binding_fingerprint(),
            receipt->target_initial_run_record_id(),
            receipt->target_initial_journal_head(), source_checkpoint_target,
            receipt->target_checkpoint()}),
        std::invalid_argument);

    auto altered_snapshot = receipt->target_checkpoint();
    altered_snapshot.next_nodes = {"different-frontier"};
    EXPECT_THROW(
        (void)ProgramGraphMigrationReceipt(ProgramGraphMigrationReceiptData{
            source.capsule, receipt->source_bundle(), receipt->source_version(),
            receipt->target_bundle(), receipt->target_version(),
            receipt->migration_plan_id(), receipt->target_generation(),
            receipt->target_run_id(), receipt->target_program_version_id(),
            receipt->target_bundle_id(), receipt->target_invocation_id(),
            receipt->target_binding_fingerprint(),
            receipt->target_initial_run_record_id(),
            receipt->target_initial_journal_head(),
            receipt->target_core_checkpoint(), altered_snapshot}),
        std::invalid_argument);

    const auto widened = admitted_program(digest('5'), true);
    EXPECT_THROW(
        (void)migration_target_publication(
            source, "capsule-expanded-target", 20, std::nullopt, &widened),
        std::invalid_argument);
}

TEST(ProgramGraphMigrationReceipt, AtomicPublicationRequiresDurableSourceHold) {
    const auto source = migration_source_fixture();
    auto target = migration_target_publication(source, "capsule-target", 20);
    const auto incomplete_plan = MigrationPlan::create(MigrationPlanData{
        source.version.id(), source.version.id(), source.version.ownership_scope(),
        MigrationCompatibility::ForkCompatible, {}, {}, {}});
    EXPECT_THROW(
        (void)migration_target_publication(
            source, "capsule-incomplete-target", 21, incomplete_plan),
        std::invalid_argument);
    auto stale = migration_target_publication(source, "capsule-stale-target", 22);
    auto with_command = migration_target_publication(
        source, "capsule-command-target", 21);
    with_command.commands.push_back(ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData{
            1, with_command.run_record.bundle_id(), 1,
            JavaScriptCommand::call_core(
                "migration:forbidden", "graph_migration_capsule", json::object()),
            digest('9'), std::nullopt}));
    auto wrong_started = migration_target_publication(
        source, "capsule-started-target", 21);
    auto started = wrong_started.events.front();
    started.id.clear();
    auto wrong_budget = wrong_started.run_record.remaining_budget();
    --wrong_budget.model_tokens;
    started.payload = ProgramStartedEvent{wrong_budget};
    wrong_started.events.front() = ProgramEvent::create(std::move(started));
    auto wrong_attempt = migration_target_publication(
        source, "capsule-attempt-target", 21, std::nullopt, nullptr, 2);
    const auto constrained = admitted_program(
        digest('5'), false, BudgetLimits{5, 1000, 1000, 1, 1, 20, 1, 1, 1});
    auto over_budget = migration_target_publication(
        source, "capsule-budget-target", 21, std::nullopt, &constrained);
    InMemoryProgramTransitionStore transitions;
    ASSERT_EQ(transitions.compare_publish(
                  source.publication.run_record.owner_scope(), {}, source.publication),
              ProgramTransitionPublishResult::Published);

    auto missing_plan = target;
    missing_plan.migration_plan.reset();
    EXPECT_EQ(transitions.compare_publish(
                  target.run_record.owner_scope(), {}, std::move(missing_plan)),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(transitions.load(target.run_record.owner_scope(),
                                  target.run_record.run_id()));
    const auto source_head = transitions.load_lineage(
        source.publication.run_record.owner_scope(),
        source.publication.run_lineage->lineage_id());
    ASSERT_TRUE(source_head);
    EXPECT_EQ(source_head->id(), source.publication.run_lineage->id());

    EXPECT_EQ(transitions.compare_publish(
                  wrong_attempt.run_record.owner_scope(), {}, wrong_attempt),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(transitions.compare_publish(
                  over_budget.run_record.owner_scope(), {}, over_budget),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(transitions.compare_publish(
                  with_command.run_record.owner_scope(), {}, with_command),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(transitions.compare_publish(
                  wrong_started.run_record.owner_scope(), {}, wrong_started),
              ProgramTransitionPublishResult::Conflict);

    ASSERT_TRUE(is_valid_program_graph_migration_transition(
        *source.publication.run_generation, *source.publication.run_lineage,
        source.publication.run_record, source.capsule, *target.migration_plan,
        *target.run_generation, *target.run_lineage, target.run_record));
    EXPECT_EQ(transitions.compare_publish(
                  target.run_record.owner_scope(), {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(transitions.load(target.run_record.owner_scope(),
                                  target.run_record.run_id()));

    EXPECT_EQ(transitions.compare_publish(
                  stale.run_record.owner_scope(), {}, stale),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(transitions.load(target.run_record.owner_scope(),
                                  "capsule-stale-target"));
    EXPECT_EQ(transitions
                  .load_lineage(source_head->owner_scope(), source_head->lineage_id())
                  ->id(),
              source_head->id());

    const auto forged_source = migration_source_fixture(false);
    const auto forged_target = migration_target_publication(
        forged_source, "capsule-forged-source-target", 20);
    InMemoryProgramTransitionStore forged_transitions;
    ASSERT_EQ(forged_transitions.compare_publish(
                  forged_source.publication.run_record.owner_scope(), {},
                  forged_source.publication),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(forged_transitions.compare_publish(
                  forged_target.run_record.owner_scope(), {}, forged_target),
              ProgramTransitionPublishResult::Conflict);
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramGraphMigrationReceipt, SQLiteRejectsSuccessorWithoutDurableHoldAcrossReopen) {
    TempMigrationDb database;
    const auto source = migration_source_fixture();
    const auto target = migration_target_publication(source, "capsule-sqlite-target", 20);
    const auto lineage_id = source.publication.run_lineage->lineage_id();
    {
        SQLiteProgramTransitionStore transitions(database.path.string());
        ASSERT_EQ(transitions.compare_publish(
                      source.publication.run_record.owner_scope(), {}, source.publication),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(transitions.compare_publish(
                      target.run_record.owner_scope(), {}, target),
                  ProgramTransitionPublishResult::Conflict);
    }
    {
        SQLiteProgramTransitionStore transitions(database.path.string());
        const auto head = transitions.load_lineage("capsule-owner", lineage_id);
        ASSERT_TRUE(head);
        EXPECT_EQ(head->active_generation(), 1U);
        EXPECT_EQ(head->active_generation_id(),
                  source.publication.run_generation->id());
        EXPECT_FALSE(transitions.load_generation("capsule-owner", lineage_id, 2));
        EXPECT_FALSE(transitions.load(target.run_record.owner_scope(),
                                      target.run_record.run_id()));
        EXPECT_EQ(transitions.compare_publish(
                      target.run_record.owner_scope(), {}, target),
                  ProgramTransitionPublishResult::Conflict);
    }
}
#endif
