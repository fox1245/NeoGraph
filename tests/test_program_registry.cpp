#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/registry.h>

#include "registry_access.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

ExecutableManifest executable(ExecutableKind                  kind,
                              std::string                     name,
                              std::string                     version,
                              std::string                     implementation_digest,
                              std::string                     attestation,
                              EffectMode                      effect_mode  = EffectMode::Brokered,
                              std::vector<std::string>        capabilities = {},
                              std::vector<std::string>        effects      = {},
                              std::vector<ExecutableIdentity> required_executables = {}) {
    return ExecutableManifest{
        {kind, std::move(name), std::move(version), std::move(implementation_digest)},
        effect_mode,
        std::move(attestation),
        std::move(capabilities),
        std::move(effects),
        std::move(required_executables)};
}

class SnapshotNode final : public GraphNode {
public:
    explicit SnapshotNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

NodeFactoryFn factory(std::atomic<int>* calls = nullptr) {
    return [calls](const std::string& name, const json&, const NodeContext&) {
        if (calls) ++*calls;
        return std::make_unique<SnapshotNode>(name);
    };
}

RegistrySnapshot build_complete_snapshot(bool reverse = false) {
    RegistrySnapshotBuilder builder;
    auto                    add_node = [&] {
        builder.add_node(
            executable(ExecutableKind::Node, "node", "1.0.0", digest('1'), "attestation:node"),
            factory(), json{{"type", "object"}}, json::object());
    };
    auto add_reducer = [&] {
        builder.add_reducer(executable(ExecutableKind::Reducer, "reducer", "1.0.0", digest('2'),
                                       "attestation:reducer"),
                            [](const json&, const json& incoming) { return json(incoming); });
    };
    if (reverse) {
        add_reducer();
        add_node();
    } else {
        add_node();
        add_reducer();
    }
    builder.add_condition(
        executable(ExecutableKind::Condition, "condition", "1.0.0", digest('3'),
                   "attestation:condition"),
        [](const GraphState&) { return std::string("yes"); }, ConditionSpec{{"yes", "no"}, false});
    builder.add_provider(
        executable(ExecutableKind::Provider, "provider", "1.0.0", digest('4'),
                   "attestation:provider", EffectMode::Brokered, {"model"}, {"network"}),
        ProviderMetadata{json{{"type", "object"}}, json{{"type", "object"}}});
    builder.add_tool(
        executable(ExecutableKind::Tool, "tool", "1.0.0", digest('5'), "attestation:test",
                   EffectMode::TrustedNative, {"filesystem"}, {"write"}),
        ToolMetadata{json{{"type", "object"}}, json{{"type", "object"}}});
    builder.add_imported(
        executable(ExecutableKind::Imported, "export", "1.0.0", digest('1'), "attestation:import"),
        ImportedExecutableMetadata{
            "module", "export", digest('6'),
            ExecutableIdentity{ExecutableKind::Node, "node", "1.0.0", digest('1')}});
    return std::move(builder).build();
}

}  // namespace

TEST(RegistrySnapshotTest, CanonicalFingerprintIsOrderIndependentAndCoversEveryKind) {
    const auto first  = build_complete_snapshot(false);
    const auto second = build_complete_snapshot(true);

    EXPECT_EQ(first.fingerprint(), second.fingerprint());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    EXPECT_EQ(first.identities().size(), 6U);
    EXPECT_TRUE(first.find(ExecutableKind::Node, "node").has_value());
    EXPECT_TRUE(first.find(ExecutableKind::Imported, "export").has_value());
    EXPECT_EQ(first.manifest()["entries"].size(), 6U);
}

TEST(RegistrySnapshotTest, DeclaredIdentityChangesAlterFingerprint) {
    const auto make_snapshot = [](std::string version, std::string implementation_digest) {
        RegistrySnapshotBuilder builder;
        builder.add_reducer(executable(ExecutableKind::Reducer, "reducer", std::move(version),
                                       std::move(implementation_digest), "attestation:reducer"),
                            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    };

    const auto baseline = make_snapshot("1.0.0", digest('1'));
    EXPECT_NE(baseline.fingerprint(), make_snapshot("1.0.1", digest('1')).fingerprint());
    EXPECT_NE(baseline.fingerprint(), make_snapshot("1.0.0", digest('2')).fingerprint());
}

TEST(RegistrySnapshotTest, ExactDependencyEdgesAreCanonicalAndFingerprintSensitive) {
    const auto tool = ExecutableIdentity{ExecutableKind::Tool, "tool", "1.0.0", digest('1')};
    const auto provider =
        ExecutableIdentity{ExecutableKind::Provider, "provider", "1.0.0", digest('2')};
    const auto alternate =
        ExecutableIdentity{ExecutableKind::Provider, "alternate", "1.0.0", digest('3')};

    const auto make_snapshot = [&](bool reverse_dependencies, bool use_alternate) {
        RegistrySnapshotBuilder builder;
        builder.add_tool(executable(ExecutableKind::Tool, tool.name, tool.semantic_version,
                                    tool.implementation_digest, "attestation:tool"),
                         ToolMetadata{json::object(), json::object()});
        builder.add_provider(
            executable(ExecutableKind::Provider, provider.name, provider.semantic_version,
                       provider.implementation_digest, "attestation:provider", EffectMode::Brokered,
                       {}, {}, {tool}),
            ProviderMetadata{json::object(), json::object()});
        builder.add_provider(
            executable(ExecutableKind::Provider, alternate.name, alternate.semantic_version,
                       alternate.implementation_digest, "attestation:alternate"),
            ProviderMetadata{json::object(), json::object()});
        std::vector<ExecutableIdentity> dependencies =
            use_alternate ? std::vector<ExecutableIdentity>{alternate, tool}
                          : std::vector<ExecutableIdentity>{provider, tool};
        if (reverse_dependencies) std::reverse(dependencies.begin(), dependencies.end());
        builder.add_node(
            executable(ExecutableKind::Node, "node", "1.0.0", digest('4'), "attestation:node",
                       EffectMode::Brokered, {}, {}, std::move(dependencies)),
            factory(), json::object(), json::object());
        return std::move(builder).build();
    };

    const auto first  = make_snapshot(false, false);
    const auto second = make_snapshot(true, false);
    EXPECT_EQ(first.fingerprint(), second.fingerprint());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    ASSERT_TRUE(first.find(ExecutableKind::Node, "node").has_value());
    EXPECT_EQ(first.find(ExecutableKind::Node, "node")->required_executables,
              (std::vector<ExecutableIdentity>{provider, tool}));
    EXPECT_NE(first.fingerprint(), make_snapshot(false, true).fingerprint());
}

TEST(RegistrySnapshotTest, RejectsInvalidExactDependencyEdges) {
    const auto provider =
        ExecutableIdentity{ExecutableKind::Provider, "provider", "1.0.0", digest('1')};

    RegistrySnapshotBuilder missing;
    missing.add_reducer(executable(ExecutableKind::Reducer, "reducer", "1.0.0", digest('2'),
                                   "attestation:reducer", EffectMode::Brokered, {}, {}, {provider}),
                        [](const json&, const json& value) { return json(value); });
    EXPECT_THROW((void)std::move(missing).build(), std::invalid_argument);

    RegistrySnapshotBuilder mismatched;
    mismatched.add_provider(
        executable(ExecutableKind::Provider, provider.name, provider.semantic_version, digest('3'),
                   "attestation:provider"),
        ProviderMetadata{json::object(), json::object()});
    mismatched.add_reducer(
        executable(ExecutableKind::Reducer, "reducer", "1.0.0", digest('2'), "attestation:reducer",
                   EffectMode::Brokered, {}, {}, {provider}),
        [](const json&, const json& value) { return json(value); });
    EXPECT_THROW((void)std::move(mismatched).build(), std::invalid_argument);

    RegistrySnapshotBuilder duplicate;
    EXPECT_THROW(
        duplicate.add_reducer(
            executable(ExecutableKind::Reducer, "reducer", "1.0.0", digest('2'),
                       "attestation:reducer", EffectMode::Brokered, {}, {}, {provider, provider}),
            [](const json&, const json& value) { return json(value); }),
        std::invalid_argument);

    auto ambiguous             = provider;
    ambiguous.semantic_version = "2.0.0";
    RegistrySnapshotBuilder ambiguous_builder;
    EXPECT_THROW(
        ambiguous_builder.add_reducer(
            executable(ExecutableKind::Reducer, "reducer", "1.0.0", digest('2'),
                       "attestation:reducer", EffectMode::Brokered, {}, {}, {provider, ambiguous}),
            [](const json&, const json& value) { return json(value); }),
        std::invalid_argument);

    const auto unrelated =
        ExecutableIdentity{ExecutableKind::Tool, "unrelated", "1.0.0", digest('3')};
    RegistrySnapshotBuilder imported_extra;
    EXPECT_THROW(imported_extra.add_imported(
                     executable(ExecutableKind::Imported, "export", provider.semantic_version,
                                provider.implementation_digest, "attestation:import",
                                EffectMode::Brokered, {}, {}, {provider, unrelated}),
                     ImportedExecutableMetadata{"module", "export", digest('4'), provider}),
                 std::invalid_argument);
}

TEST(RegistrySnapshotTest, RejectsDuplicateNamesAndInvalidTrustedNativeMetadata) {
    RegistrySnapshotBuilder duplicate;
    duplicate.add_reducer(
        executable(ExecutableKind::Reducer, "same", "1.0.0", digest('1'), "attestation:one"),
        [](const json&, const json& value) { return json(value); });
    duplicate.add_reducer(
        executable(ExecutableKind::Reducer, "same", "1.0.1", digest('2'), "attestation:two"),
        [](const json&, const json& value) { return json(value); });
    EXPECT_THROW((void)std::move(duplicate).build(), std::invalid_argument);

    RegistrySnapshotBuilder unattested;
    EXPECT_THROW(unattested.add_tool(executable(ExecutableKind::Tool, "native", "1.0.0",
                                                digest('3'), {}, EffectMode::TrustedNative),
                                     ToolMetadata{json::object(), json::object()}),
                 std::invalid_argument);

    RegistrySnapshotBuilder padded_name;
    EXPECT_THROW(
        padded_name.add_reducer(executable(ExecutableKind::Reducer, " padded", "1.0.0", digest('4'),
                                           "attestation:padded"),
                                [](const json&, const json& value) { return json(value); }),
        std::invalid_argument);

    RegistrySnapshotBuilder controlled_capability;
    EXPECT_THROW(controlled_capability.add_tool(
                     executable(ExecutableKind::Tool, "tool", "1.0.0", digest('5'),
                                "attestation:tool", EffectMode::Brokered, {"capability\n"}),
                     ToolMetadata{json::object(), json::object()}),
                 std::invalid_argument);

    RegistrySnapshotBuilder missing_import_target;
    missing_import_target.add_imported(
        executable(ExecutableKind::Imported, "export", "1.0.0", digest('7'), "attestation:import"),
        ImportedExecutableMetadata{
            "module", "export", digest('6'),
            ExecutableIdentity{ExecutableKind::Node, "missing", "1.0.0", digest('7')}});
    EXPECT_THROW((void)std::move(missing_import_target).build(), std::invalid_argument);

    RegistrySnapshotBuilder mismatched_import_target;
    mismatched_import_target.add_node(
        executable(ExecutableKind::Node, "node", "1.0.0", digest('8'), "attestation:node"),
        factory(), json::object(), json::object());
    mismatched_import_target.add_imported(
        executable(ExecutableKind::Imported, "export", "1.0.0", digest('a'), "attestation:import"),
        ImportedExecutableMetadata{
            "module", "export", digest('9'),
            ExecutableIdentity{ExecutableKind::Node, "node", "1.0.0", digest('a')}});
    EXPECT_THROW((void)std::move(mismatched_import_target).build(), std::invalid_argument);

    RegistrySnapshotBuilder recursive_import;
    EXPECT_THROW(recursive_import.add_imported(
                     executable(ExecutableKind::Imported, "export", "1.0.0", digest('c'),
                                "attestation:import"),
                     ImportedExecutableMetadata{"module", "export", digest('b'),
                                                ExecutableIdentity{ExecutableKind::Imported,
                                                                   "other", "1.0.0", digest('c')}}),
                 std::invalid_argument);
}

TEST(RegistrySnapshotTest, ImportedAliasesInheritTargetTrustAndEffects) {
    const auto target = ExecutableIdentity{ExecutableKind::Tool, "native", "1.0.0", digest('d')};

    RegistrySnapshotBuilder builder;
    builder.add_tool(
        executable(ExecutableKind::Tool, "native", "1.0.0", digest('d'), "attestation:target",
                   EffectMode::TrustedNative, {"gpu"}, {"filesystem"}),
        ToolMetadata{json::object(), json::object()});
    builder.add_imported(
        executable(ExecutableKind::Imported, "export", "1.0.0", digest('d'), "attestation:alias"),
        ImportedExecutableMetadata{"module", "export", digest('e'), target});
    const auto snapshot = std::move(builder).build();
    const auto alias    = snapshot.find(ExecutableKind::Imported, "export");
    ASSERT_TRUE(alias.has_value());
    EXPECT_EQ(alias->effect_mode, EffectMode::TrustedNative);
    EXPECT_EQ(alias->required_capabilities, std::vector<std::string>{"gpu"});
    EXPECT_EQ(alias->declared_effects, std::vector<std::string>{"filesystem"});
}

TEST(RegistrySnapshotTest, DeepOwnsMetadataAndReturnedManifests) {
    json schema =
        json{{"type", "object"}, {"properties", json{{"value", json{{"type", "integer"}}}}}};
    RegistrySnapshotBuilder builder;
    builder.add_node(executable(ExecutableKind::Node, "owned", "1.0.0", digest('1'),
                                "attestation:owned", EffectMode::Brokered, {"owned-capability"}),
                     factory(), schema, json{{"writes", json::array()}});
    auto       snapshot    = std::move(builder).build();
    const auto fingerprint = snapshot.fingerprint();
    const auto bytes       = snapshot.serialize_canonical();

    schema["properties"]["value"]["type"]                       = "string";
    auto returned                                               = snapshot.manifest();
    returned["entries"][0]["metadata"]["config_schema"]["type"] = "array";
    auto found = snapshot.find(ExecutableKind::Node, "owned");
    ASSERT_TRUE(found.has_value());
    found->required_capabilities[0] = "mutated";

    EXPECT_EQ(snapshot.fingerprint(), fingerprint);
    EXPECT_EQ(snapshot.serialize_canonical(), bytes);
    EXPECT_EQ(snapshot.manifest()["entries"][0]["metadata"]["config_schema"]["type"], "object");
    EXPECT_EQ(snapshot.find(ExecutableKind::Node, "owned")->required_capabilities[0],
              "owned-capability");
}

TEST(RegistrySnapshotTest, LocalCompilerRejectsEveryGlobalOnlyExecutableKind) {
    std::atomic<int> global_factory_calls{0};
    NodeFactory::instance().register_type("pr4-global-only", factory(&global_factory_calls),
                                          json{{"type", "object"}});
    neograph::graph::ReducerRegistry::instance().register_reducer(
        "pr4-global-reducer", [](const json&, const json& value) { return json(value); });
    neograph::graph::ConditionRegistry::instance().register_condition(
        "pr4-global-condition", [](const GraphState&) { return std::string("go"); });

    RegistrySnapshotBuilder empty_builder;
    const auto              empty_snapshot  = std::move(empty_builder).build();
    const json              node_definition = {
        {"schema_version", 1},
        {"nodes", json{{"work", json{{"type", "pr4-global-only"}}}}},
        {"edges", json::array()},
        {"conditional_edges", json::array()},
    };
    EXPECT_THROW((void)neograph::program::detail::RegistrySnapshotAccess::parse_local(
                     empty_snapshot, node_definition),
                 std::out_of_range);
    EXPECT_EQ(global_factory_calls.load(), 0);

    const json reducer_definition = {
        {"schema_version", 1},
        {"channels", json{{"value", json{{"reducer", "pr4-global-reducer"}}}}},
        {"nodes", json::object()},
        {"edges", json::array()},
        {"conditional_edges", json::array()},
    };
    EXPECT_THROW((void)neograph::program::detail::RegistrySnapshotAccess::parse_local(
                     empty_snapshot, reducer_definition),
                 std::out_of_range);

    RegistrySnapshotBuilder node_builder;
    node_builder.add_node(executable(ExecutableKind::Node, "pr4-local-node", "1.0.0", digest('2'),
                                     "attestation:local"),
                          factory(), json{{"type", "object"}}, json::object());
    const auto node_snapshot        = std::move(node_builder).build();
    const json condition_definition = {
        {"schema_version", 1},
        {"nodes", json{{"work", json{{"type", "pr4-local-node"}}}}},
        {"edges", json::array()},
        {"conditional_edges", json::array({json{{"from", "work"},
                                                {"condition", "pr4-global-condition"},
                                                {"routes", json{{"go", "__end__"}}}}})},
    };
    EXPECT_THROW((void)neograph::program::detail::RegistrySnapshotAccess::parse_local(
                     node_snapshot, condition_definition),
                 std::out_of_range);
}

TEST(RegistrySnapshotTest, CopiedCallableSurvivesBuilderAndCallerMutation) {
    std::atomic<int> calls{0};
    json             schema   = json{{"type", "object"},
                                     {"properties", json{{"value", json{{"type", "integer"}}}}},
                                     {"required", json::array({"value"})}};
    RegistrySnapshot snapshot = [&] {
        RegistrySnapshotBuilder builder;
        builder.add_node(executable(ExecutableKind::Node, "local-node", "1.0.0", digest('1'),
                                    "attestation:local"),
                         factory(&calls), schema, json::object());
        return std::move(builder).build();
    }();
    schema["properties"]["value"]["type"] = "string";

    const json definition = {
        {"schema_version", 1},
        {"nodes", json{{"work", json{{"type", "local-node"}, {"value", 7}}}}},
        {"edges", json::array()},
        {"conditional_edges", json::array()},
    };
    auto topology =
        neograph::program::detail::RegistrySnapshotAccess::parse_local(snapshot, definition);
    auto report =
        neograph::program::detail::RegistrySnapshotAccess::validate_local(snapshot, topology);
    EXPECT_FALSE(report.has_errors()) << report.summary();
    auto graph = neograph::program::detail::RegistrySnapshotAccess::link_local(
        snapshot, std::move(topology), NodeContext{});

    EXPECT_EQ(graph.nodes.size(), 1U);
    EXPECT_EQ(calls.load(), 1);
}
