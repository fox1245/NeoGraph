#include <neograph/program/program.h>
#include <neograph/graph/engine.h>

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <climits>
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

ProgramVersion program_version(std::string core_generation_id = digest('5')) {
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
        .budget_ceiling(BudgetLimits{1000, 1000, 1000, 1, 1, 20, 1, 1, 1});
    auto policy = std::move(policy_builder).build();
    return ProgramVersion(ProgramVersionData{
        digest('2'), admission, policy, {}, "capsule-owner",
        CoreMaterializationReceipt{
            "capsule-tests", registry.fingerprint(),
            {CorePlanIdentity{"graph_migration_capsule", std::move(core_generation_id)}}, {}}});
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

}  // namespace

TEST(GraphMigrationCapsule, CanonicalRoundTripBindsSourceAndCheckpoint) {
    const auto capsule = make_capsule();
    const auto parsed = GraphMigrationCapsule::parse(capsule.serialize_canonical());

    EXPECT_EQ(parsed.id(), capsule.id());
    EXPECT_EQ(parsed.serialize_canonical(), capsule.serialize_canonical());
    EXPECT_EQ(parsed.owner_scope(), "capsule-owner");
    EXPECT_EQ(parsed.source_generation(), 1U);
    EXPECT_EQ(parsed.source_run_id(), "capsule-run");
    EXPECT_EQ(parsed.source_bundle_id(), digest('2'));
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
