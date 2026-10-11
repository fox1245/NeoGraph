// #362 graph metadata (Program API v2, WP3).
//
// `ng.graph(...).metadata({...})` stores host-readable author claims in the
// Core definition under a top-level "metadata" object. It is hashed into the
// definition, bounded (8 KiB canonical, depth 4, 64 members, keys of 1-128
// bytes), free of invisible characters, never part of state, checkpoints,
// output or events, and invisible to node factories.

#include "program_v2_test_support.h"

#include "../src/core/canonical_json.h"

#include <neograph/graph/engine.h>

#include <atomic>
#include <mutex>

namespace {

using namespace program_v2_test;

json definition_with(json metadata, int schema_version = 1) {
    json definition{{"name", "main"},
                    {"nodes", json{{"work", json{{"type", "t.task"}}}}},
                    {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                           json{{"from", "work"}, {"to", "__end__"}}})}};
    if (schema_version > 0) definition["schema_version"] = schema_version;
    if (!metadata.is_null()) definition["metadata"] = std::move(metadata);
    return definition;
}

std::vector<CompilerDiagnostic> parse_errors(const json& definition) {
    GraphRegistry registry;
    return GraphCompiler::parse_report(definition, registry).diagnostics;
}

std::vector<std::string> codes_of(const std::vector<CompilerDiagnostic>& diagnostics) {
    std::vector<std::string> result;
    for (const auto& diagnostic : diagnostics) result.push_back(diagnostic.code + "@" + diagnostic.json_pointer);
    return result;
}

std::string definition_hash(const Outcome& outcome) {
    if (!outcome.ok()) return "error:" + codes(outcome.returned);
    return outcome.bundle->sealed_core_definitions().at(0).definition_hash;
}

std::string text(std::size_t size, char fill = 'x') { return std::string(size, fill); }

// {"a": {"a": ... {"a": 1}}} with `depth` containers below metadata.
json nested(int depth) {
    json value = 1;
    for (int level = 1; level < depth; ++level) value = json{{"a", value}};
    return json{{"a", value}};
}

}  // namespace

TEST(GraphMetadata, StrictDefinitionsKeepMetadataAndLegacyOnesIgnoreIt) {
    const json metadata{{"owner", "agent:planner"}, {"requirements", json::array({"REQ-12"})}};
    auto       strict = GraphCompiler::parse(definition_with(metadata));
    EXPECT_EQ(canonical(strict.metadata), canonical(metadata));
    EXPECT_EQ(canonical(strict.to_json().at("metadata")), canonical(metadata));
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(definition_with(metadata), strict));

    const auto legacy = definition_with(metadata, 0);
    EXPECT_TRUE(GraphCompiler::parse(legacy).metadata.is_null()) << "v0 documents ignore the key";

    const auto empty = GraphCompiler::parse(definition_with(json::object()));
    EXPECT_TRUE(empty.metadata.is_null()) << "{} canonicalizes to absent";
    EXPECT_FALSE(empty.to_json().contains("metadata"));
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(definition_with(json::object()), empty));
}

TEST(GraphMetadata, MetadataMustBeAnObject) {
    EXPECT_EQ(codes_of(parse_errors(definition_with(json::array({1})))),
              (std::vector<std::string>{"GC_FIELD_TYPE@/metadata"}));
    EXPECT_EQ(codes_of(parse_errors(definition_with(json("owner")))),
              (std::vector<std::string>{"GC_FIELD_TYPE@/metadata"}));
}

TEST(GraphMetadata, SizeDepthMemberAndKeyLimitsHoldAtTheBoundary) {
    // 8 KiB of canonical bytes: {"k":"..."} is 8 bytes plus the string.
    EXPECT_TRUE(parse_errors(definition_with(json{{"k", text(8184)}})).empty());
    const auto bytes = parse_errors(definition_with(json{{"k", text(8185)}}));
    ASSERT_EQ(codes_of(bytes), (std::vector<std::string>{"GC_METADATA_LIMIT@/metadata"}));
    EXPECT_EQ(canonical(bytes[0].witness),
              canonical(json{{"kind", "bytes"}, {"limit", 8192}, {"actual", 8193}}));

    EXPECT_TRUE(parse_errors(definition_with(nested(4))).empty());
    const auto depth = parse_errors(definition_with(nested(5)));
    ASSERT_EQ(codes_of(depth), (std::vector<std::string>{"GC_METADATA_LIMIT@/metadata/a/a/a/a/a"}));
    EXPECT_EQ(canonical(depth[0].witness), canonical(json{{"kind", "depth"}, {"limit", 4}, {"actual", 5}}));

    json members = json::object();
    for (int index = 0; index < 64; ++index) members["k" + std::to_string(100 + index)] = index;
    EXPECT_TRUE(parse_errors(definition_with(members)).empty());
    members["k999"] = 1;
    const auto count = parse_errors(definition_with(members));
    ASSERT_EQ(codes_of(count), (std::vector<std::string>{"GC_METADATA_LIMIT@/metadata"}));
    EXPECT_EQ(canonical(count[0].witness), canonical(json{{"kind", "members"}, {"limit", 64}, {"actual", 65}}));
    // Nested objects count toward the same total.
    json spread{{"outer", members}};
    EXPECT_FALSE(parse_errors(definition_with(spread)).empty());

    EXPECT_TRUE(parse_errors(definition_with(json{{text(128, 'k'), 1}})).empty());
    const auto key = parse_errors(definition_with(json{{text(129, 'k'), 1}}));
    ASSERT_EQ(key.size(), 1U);
    EXPECT_EQ(key[0].code, "GC_METADATA_LIMIT");
    EXPECT_EQ(canonical(key[0].witness), canonical(json{{"kind", "key_bytes"}, {"limit", 128}, {"actual", 129}}));
}

TEST(GraphMetadata, EmptyAndReservedKeysAreRejected) {
    EXPECT_EQ(codes_of(parse_errors(definition_with(json{{"", 1}}))),
              (std::vector<std::string>{"GC_FIELD_VALUE@/metadata/"}));
    EXPECT_EQ(codes_of(parse_errors(definition_with(json{{"neograph.policy", 1}}))),
              (std::vector<std::string>{"GC_FIELD_VALUE@/metadata/neograph.policy"}));
    // Only the top-level prefix is reserved.
    EXPECT_TRUE(parse_errors(definition_with(json{{"plan", json{{"neograph.policy", 1}}}})).empty());
}

TEST(GraphMetadata, InvisibleCharactersAreErrorsInKeysAndStrings) {
    struct Case {
        const char* encoded;
        const char* code_point;
    };
    const std::vector<Case> cases{{"\x07", "U+0007"},
                                  {"\xE2\x80\x8E", "U+200E"},       // bidi: LEFT-TO-RIGHT MARK
                                  {"\xE2\x80\xAE", "U+202E"},       // bidi: RIGHT-TO-LEFT OVERRIDE
                                  {"\xE2\x81\xA6", "U+2066"},       // bidi isolate
                                  {"\xD8\x9C", "U+061C"},           // ARABIC LETTER MARK
                                  {"\xE2\x80\x8B", "U+200B"},       // zero width space
                                  {"\xE2\x81\xA0", "U+2060"},       // word joiner
                                  {"\xEF\xBB\xBF", "U+FEFF"},       // byte order mark
                                  {"\xF3\xA0\x81\x81", "U+E0041"}};  // tag letter A
    for (const auto& item : cases) {
        const std::string hidden = std::string("ok") + item.encoded + "ok";
        const auto        value  = parse_errors(definition_with(json{{"owner", hidden}}));
        ASSERT_EQ(codes_of(value), (std::vector<std::string>{"GC_METADATA_TEXT@/metadata/owner"}))
            << item.code_point;
        EXPECT_EQ(canonical(value[0].witness.at("code_points")), canonical(json::array({item.code_point})))
            << item.code_point;
        const auto key = parse_errors(definition_with(json{{hidden, 1}}));
        ASSERT_EQ(key.size(), 1U) << item.code_point;
        EXPECT_EQ(key[0].code, "GC_METADATA_TEXT") << item.code_point;
    }
    // Tab, newline and carriage return are ordinary text.
    EXPECT_TRUE(parse_errors(definition_with(json{{"note", "a\tb\nc\rd"}})).empty());
    // Nested strings and array elements are checked too.
    EXPECT_EQ(codes_of(parse_errors(definition_with(json{{"list", json::array({"a", "b\xE2\x80\x8B"})}}))),
              (std::vector<std::string>{"GC_METADATA_TEXT@/metadata/list/1"}));
}

TEST(GraphMetadata, MetadataNeverReachesAFactoryStateOrOutput) {
    static std::mutex                 mutex;
    static std::vector<json>          configs;
    static std::atomic<bool>          saw_metadata_in_context{false};
    NodeFactory::instance().register_type(
        "metadata_probe.node",
        [](const std::string& name, const json& config, const NodeContext&) {
            std::lock_guard lock(mutex);
            configs.push_back(config);
            return std::make_unique<QuietNode>(name);
        },
        json{{"type", "object"}});
    auto definition                 = definition_with(json{{"secret_marker", "metadata-value-7f3a"}});
    definition["nodes"]["work"]["type"] = "metadata_probe.node";
    definition["channels"]          = json{{"value", json{{"reducer", "overwrite"}, {"initial", 0}}}};
    auto engine = GraphEngine::compile(definition, NodeContext{});
    RunConfig config;
    config.input = json::object();
    const auto result = engine->run(config);
    EXPECT_EQ(result.output.dump().find("metadata-value-7f3a"), std::string::npos);
    EXPECT_EQ(result.output.dump().find("secret_marker"), std::string::npos);
    std::lock_guard lock(mutex);
    ASSERT_FALSE(configs.empty());
    for (const auto& seen : configs) EXPECT_FALSE(seen.contains("metadata")) << seen.dump();
}

TEST(GraphMetadata, HashCoversMetadataButNotItsKeyOrderOrAnEmptyObject) {
    const auto plain = compile(cpp_source(program_document(definition_with(nullptr))));
    const auto empty = compile(cpp_source(program_document(definition_with(json::object()))));
    const auto ab    = compile(cpp_source(program_document(definition_with(json::parse(R"({"a":1,"b":2})")))));
    const auto ba    = compile(cpp_source(program_document(definition_with(json::parse(R"({"b":2,"a":1})")))));
    EXPECT_EQ(definition_hash(empty), definition_hash(plain));
    EXPECT_EQ(definition_hash(ab), definition_hash(ba));
    EXPECT_NE(definition_hash(ab), definition_hash(plain));
    ASSERT_TRUE(ab.ok());
    EXPECT_EQ(canonical(ab.bundle->sealed_core_definitions().at(0).definition.at("metadata")),
              canonical(json::parse(R"({"a":1,"b":2})")));
}

TEST(GraphMetadata, ProgramDiagnosticsUseTheMetadataCodes) {
    const auto text_error = compile(cpp_source(program_document(definition_with(json{{"owner", "a\xE2\x80\x8B"}}))));
    ASSERT_FALSE(text_error.ok());
    ASSERT_EQ(with_code(text_error.returned, "P_CORE_GC_METADATA_TEXT").size(), 1U) << codes(text_error.returned);
    EXPECT_EQ(with_code(text_error.returned, "P_CORE_GC_METADATA_TEXT")[0].primary.json_pointer,
              "/root/definition/metadata/owner");
    EXPECT_EQ(diagnostic_doc_link("P_CORE_GC_METADATA_TEXT"), "docs/diagnostics.md#p_core_gc_metadata_text");
    EXPECT_EQ(diagnostic_doc_link("P_CORE_GC_METADATA_LIMIT"), "docs/diagnostics.md#p_core_gc_metadata_limit");
}

// A definition that uses a v2 key seals no validator warnings: it has no
// earlier id to keep and the original rules do not know the new keys.
TEST(GraphMetadata, FeatureBundlesSealNoValidatorWarnings) {
    auto definition               = definition_with(json{{"owner", "agent:planner"}});
    definition["nodes"]["orphan"] = json{{"type", "t.task"}};  // E7 under both rule sets
    const auto with_metadata      = compile(cpp_source(program_document(definition)));
    ASSERT_TRUE(with_metadata.ok()) << codes(with_metadata.returned);
    EXPECT_EQ(with_code(with_metadata.returned, "P_CORE_E7").size(), 1U);
    EXPECT_TRUE(with_metadata.bundle->diagnostics().empty()) << codes(with_metadata.bundle->diagnostics());

    auto plain                = definition_with(nullptr);
    plain["nodes"]["orphan"]  = json{{"type", "t.task"}};
    const auto without        = compile(cpp_source(program_document(plain)));
    ASSERT_TRUE(without.ok());
    EXPECT_EQ(with_code(without.bundle->diagnostics(), "P_CORE_E7").size(), 1U)
        << "programs without v2 keys keep sealing original-rule warnings";
}

TEST(GraphMetadata, SealedBundlesValidateMetadataAndOldRuntimesRejectIt) {
    const auto outcome = compile(cpp_source(program_document(definition_with(json{{"owner", "x"}}))));
    ASSERT_TRUE(outcome.ok());
    const auto& bundle = *outcome.bundle;
    EXPECT_EQ(ProgramBundle::parse(bundle.serialize_canonical()).id(), bundle.id());

    // The pre-v2 sealed validator closed the top-level key set; an old
    // runtime therefore rejects a bundle that uses metadata (fail closed).
    const auto definition = bundle.sealed_core_definitions().at(0).definition;
    EXPECT_THROW(neograph::detail::reject_unknown_fields(
                     definition, "Sealed Core definition",
                     {"schema_version", "name", "channels", "nodes", "edges", "conditional_edges",
                      "interrupt_before", "interrupt_after", "retry_policy"}),
                 std::invalid_argument);

    // A sealed definition whose metadata breaks the rules is rejected here too.
    auto hidden                     = definition;
    hidden["metadata"]["owner"]     = "a\xE2\x80\x8B";
    SealedCoreDefinition tampered{"main", sealed_core_definition_hash(hidden), hidden};
    EXPECT_THROW((void)core_compiled_plan_identity(tampered, "build", bundle.registry_snapshot_fingerprint(),
                                                   bundle.executable_registry_identities()),
                 std::invalid_argument);
}

TEST(GraphMetadata, CapabilityManifestPublishesTheMethod) {
    const auto manifest = javascript_authoring_capability_manifest();
    EXPECT_TRUE(manifest.at("features").at("graph.metadata").get<bool>());
    bool found = false;
    for (const auto& method : manifest.at("define").at("graph_builder_methods"))
        found = found || method.at("name").get<std::string>() == "metadata";
    EXPECT_TRUE(found);
}

#if defined(NEOGRAPH_V2_TESTS_HAVE_QUICKJS)

// The #362 repro.
TEST(GraphMetadata, JavaScriptBuilderStoresMetadata) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").metadata({owner: "x", requirements: ["REQ-12"]})
            .node("A", {type: "t.task"}).entry("A").exit("A");
    })JS"));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    EXPECT_EQ(canonical(outcome.bundle->sealed_core_definitions().at(0).definition.at("metadata")),
              canonical(json{{"owner", "x"}, {"requirements", json::array({"REQ-12"})}}));
}

TEST(GraphMetadata, JavaScriptBuilderRejectsASecondCallAndNonObjects) {
    for (const auto* body : {R"(.metadata({a: 1}).metadata({b: 2}))", R"(.metadata(["a"]))",
                             R"(.metadata("a"))", R"(.metadata())"}) {
        const auto outcome = compile(js_source(std::string(R"JS(export function define() {
            return ng.graph("main").node("A", {type: "t.task"}))JS") + body +
                                               R"JS(.entry("A").exit("A");
        })JS"));
        ASSERT_FALSE(outcome.ok()) << body;
        EXPECT_EQ(outcome.returned.at(0).code, "P_JS_GRAPH_ARGUMENT") << body << codes(outcome.returned);
    }
}

TEST(GraphMetadata, JavaScriptMetadataErrorsPointAtTheCall) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"})
            .metadata({"neograph.owner": "x"}).entry("A").exit("A");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    const auto reserved = with_code(outcome.returned, "P_CORE_GC_FIELD_VALUE");
    ASSERT_EQ(reserved.size(), 1U) << codes(outcome.returned);
    EXPECT_TRUE(reserved[0].primary.span.has_value());
    EXPECT_EQ(reserved[0].primary.span->line_begin, 3U);
}

#endif
