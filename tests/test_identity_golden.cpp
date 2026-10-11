// Golden identity corpus for Program bundles (Program API v2, WP-G; #365).
//
// Every entry in tests/fixtures/identity_corpus/manifest.json is compiled
// through the real ProgramCompiler, and its identities are compared with the
// pins in golden.json. Later work packages add keys to Core definitions; the
// shared rule is that a program which does not use them keeps exactly today's
// definition bytes, definition_hash, compiled-plan identity and bundle id.
// This test is that guard, and it must stay green across every package.
//
// Platform rule. A JavaScript source envelope seals
// ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS, which differs on the MSVC
// port. Everything derived from that envelope (the JavaScript source_hash, the
// bundle id, and canonical_program_hash when the bundle retains main() as its
// control source) is pinned for the POSIX QuickJS build identity only. The
// definition, definition_hash, compiled-plan identity and sealed diagnostics
// do not read the envelope and are asserted on every platform; so is every
// identity of a C++ builder program. DefinitionIdentityDoesNotDependOnTheFrontend
// checks that claim by sealing each JavaScript definition again without QuickJS.
//
// Regeneration (never in CI). After an intended identity change:
//
//     NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
//         <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
//
// rewrites golden.json deterministically from the manifest. It refuses to run
// when CI or GITHUB_ACTIONS is set, and on builds without the POSIX QuickJS
// identity. Review the diff: a changed pin for a program that does not use a
// new feature is a compatibility break, not a fixture update.

#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "../src/core/canonical_json.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef NEOGRAPH_IDENTITY_CORPUS_DIR
#error "NEOGRAPH_IDENTITY_CORPUS_DIR must name tests/fixtures/identity_corpus"
#endif

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;
namespace fs = std::filesystem;

const fs::path kCorpusDir{NEOGRAPH_IDENTITY_CORPUS_DIR};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open corpus file " + path.generic_string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

json read_json(const fs::path& path) { return json::parse(read_file(path)); }

std::string canonical(const json& value) { return neograph::detail::canonical_json_bytes(value); }

bool env_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

bool regenerating() {
    const char* value = std::getenv("NEOGRAPH_REGENERATE_IDENTITY_CORPUS");
    return value != nullptr && std::string_view(value) == "1";
}

bool posix_quickjs_identity(const json& golden) {
    return golden.contains("posix_quickjs_build_options") &&
           golden.at("posix_quickjs_build_options").get<std::string>() ==
               ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS;
}

class NoopNode final : public GraphNode {
public:
    explicit NoopNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

ExecutableManifest manifest(ExecutableKind kind, const json& entry, const std::string& attestation) {
    return ExecutableManifest{{kind, entry.at("name").get<std::string>(),
                               entry.at("semantic_version").get<std::string>(),
                               entry.at("implementation_digest").get<std::string>()},
                              EffectMode::Brokered,
                              attestation,
                              {},
                              {},
                              {},
                              ExecutionGuarantee::Strict};
}

// The snapshot a Python host builds with ProgramRegistryBuilder for the same
// names (bindings/python/tests/test_identity_golden.py): Python-registered
// node types get NodeFactory's default config schema and no effect contract,
// and Python conditions have no closed label set. Both languages therefore pin
// the same registry fingerprint and the same ids.
RegistrySnapshot corpus_registry(const json& registry) {
    const auto attestation = registry.at("attestation").get<std::string>();
    // NodeFactory answers an unregistered type with the default schema that a
    // Python register_type() call stores.
    const auto default_schema =
        NodeFactory::instance().config_schema("neograph.identity-corpus.unregistered-type");
    RegistrySnapshotBuilder builder;
    for (const auto& node : registry.at("nodes"))
        builder.add_node(manifest(ExecutableKind::Node, node, attestation),
                         [](const std::string& name, const json&, const NodeContext&) {
                             return std::make_unique<NoopNode>(name);
                         },
                         default_schema, json::object());
    for (const auto& reducer : registry.at("reducers"))
        builder.add_reducer(manifest(ExecutableKind::Reducer, reducer, attestation),
                            [](const json&, const json& incoming) { return json(incoming); });
    for (const auto& condition : registry.at("conditions"))
        builder.add_condition(manifest(ExecutableKind::Condition, condition, attestation),
                              [](const GraphState&) { return std::string("unused"); },
                              std::nullopt);
    return std::move(builder).build();
}

std::string source_id(const std::string& entry_id, const std::string& frontend) {
    return "identity-corpus/" + entry_id + (frontend == "javascript" ? ".js" : ".json");
}

ProgramSource corpus_source(const json& entry) {
    const auto id       = entry.at("id").get<std::string>();
    const auto frontend = entry.at("frontend").get<std::string>();
    const auto path     = kCorpusDir / entry.at("source").get<std::string>();
    if (frontend == "javascript")
        return ProgramSource::from_javascript(source_id(id, frontend), read_file(path));
    if (frontend != "cpp_builder") throw std::runtime_error("unknown corpus frontend " + frontend);
    auto document             = read_json(path);
    const auto schema_version = document.at("program_schema_version").get<std::uint32_t>();
    return ProgramSource::from_cpp_builder(source_id(id, frontend), schema_version,
                                           std::move(document));
}

json sealed_diagnostics(const ProgramBundle& bundle) {
    json result = json::array();
    for (const auto& diagnostic : bundle.diagnostics())
        result.push_back(json{{"code", diagnostic.code},
                              {"json_pointer", diagnostic.primary.json_pointer}});
    return result;
}

// The pins of one entry, split by the platform rule in the file comment.
json entry_identity(const ProgramCompiler& compiler, const json& entry) {
    const auto source      = corpus_source(entry);
    const auto bundle      = compiler.compile(source);
    const auto definitions = bundle.sealed_core_definitions();
    if (definitions.size() != 1 || bundle.core_plan_identities().size() != 1)
        throw std::runtime_error("corpus programs seal exactly one Core definition");
    const bool javascript  = source.kind() == SourceKind::JavaScript;
    const bool has_control = bundle.control_source().has_value();

    json everywhere{{"definition_name", definitions[0].name},
                    {"definition_hash", definitions[0].definition_hash},
                    {"compiled_plan_identity",
                     bundle.core_plan_identities()[0].compiled_plan_identity},
                    {"control_source", has_control},
                    {"sealed_diagnostics", sealed_diagnostics(bundle)}};
    json posix = json::object();
    if (javascript) {
        posix["source_hash"] = bundle.source_hash();
        posix["bundle_id"]   = bundle.id();
        (has_control ? posix : everywhere)["canonical_program_hash"] =
            bundle.canonical_program_hash();
    } else {
        everywhere["source_hash"]            = bundle.source_hash();
        everywhere["bundle_id"]              = bundle.id();
        everywhere["canonical_program_hash"] = bundle.canonical_program_hash();
    }
    json result{{"id", entry.at("id")},
                {"frontend", entry.at("frontend")},
                {"definition", definitions[0].definition},
                {"all_platforms", std::move(everywhere)}};
    if (javascript) result["posix_quickjs"] = std::move(posix);
    return result;
}

json compute_golden(const json& manifest_document) {
    const auto      registry = corpus_registry(manifest_document.at("registry"));
    ProgramCompiler compiler(registry,
                             {manifest_document.at("compiler_build_id").get<std::string>()});
    json entries = json::array();
    for (const auto& entry : manifest_document.at("entries"))
        entries.push_back(entry_identity(compiler, entry));
    return json{{"format", "neograph-identity-golden"},
                {"format_version", 1},
                {"compiler_build_id", manifest_document.at("compiler_build_id")},
                {"registry_fingerprint", registry.fingerprint()},
                {"posix_quickjs_build_options",
                 std::string(ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS)},
                {"entries", std::move(entries)}};
}

// Deterministic, diff-friendly rendering: canonical key order and canonical
// scalars, two-space indentation, one trailing newline.
void render(const json& value, int depth, std::string& out) {
    const std::string pad(static_cast<std::size_t>(depth + 1) * 2, ' ');
    const std::string close(static_cast<std::size_t>(depth) * 2, ' ');
    if (value.is_object() && value.size() > 0) {
        out += "{\n";
        std::size_t index = 0;
        for (const auto& [key, child] : value.items()) {
            out += pad + canonical(json(key)) + ": ";
            render(child, depth + 1, out);
            out += ++index == value.size() ? "\n" : ",\n";
        }
        out += close + "}";
    } else if (value.is_array() && value.size() > 0) {
        out += "[\n";
        std::size_t index = 0;
        for (const auto& child : value) {
            out += pad;
            render(child, depth + 1, out);
            out += ++index == value.size() ? "\n" : ",\n";
        }
        out += close + "]";
    } else {
        out += canonical(value);
    }
}

std::string render_golden(const json& golden) {
    std::string out;
    render(json::parse(canonical(golden)), 0, out);  // reparse to get canonical key order
    out += "\n";
    return out;
}

const json& manifest_document() {
    static const json value = read_json(kCorpusDir / "manifest.json");
    return value;
}

void expect_field(const json& actual, const json& pinned, const std::string& key,
                  const std::string& entry_id) {
    ASSERT_TRUE(pinned.contains(key)) << entry_id << ": golden.json lacks " << key;
    ASSERT_TRUE(actual.contains(key)) << entry_id << ": computed identity lacks " << key;
    EXPECT_EQ(canonical(actual.at(key)), canonical(pinned.at(key)))
        << entry_id << ": " << key << " changed";
}

}  // namespace

TEST(IdentityGolden, Regenerate) {
    if (!regenerating()) GTEST_SKIP() << "set NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 to regenerate";
    ASSERT_FALSE(env_set("CI") || env_set("GITHUB_ACTIONS"))
        << "the identity corpus is never regenerated in CI";
#if !defined(NEOGRAPH_IDENTITY_CORPUS_HAVE_QUICKJS)
    FAIL() << "regeneration needs NEOGRAPH_BUILD_QUICKJS_CONTROL=ON";
#else
#if defined(_MSC_VER)
    FAIL() << "regenerate on a build with the POSIX QuickJS identity";
#endif
    const auto rendered = render_golden(compute_golden(manifest_document()));
    ASSERT_EQ(rendered, render_golden(compute_golden(manifest_document())))
        << "regeneration is not deterministic";
    std::ofstream output(kCorpusDir / "golden.json", std::ios::binary | std::ios::trunc);
    output << rendered;
    ASSERT_TRUE(output.good());
    std::cout << "[ identity ] rewrote golden.json; review the diff\n";
#endif
}

#if defined(NEOGRAPH_IDENTITY_CORPUS_HAVE_QUICKJS)

TEST(IdentityGolden, PosixBuildsCarryThePinnedQuickJsIdentity) {
    const auto golden = read_json(kCorpusDir / "golden.json");
    // Guards against silently skipping the POSIX pins on a POSIX build.
#if defined(_MSC_VER)
    EXPECT_FALSE(posix_quickjs_identity(golden));
#else
    EXPECT_TRUE(posix_quickjs_identity(golden));
#endif
}

TEST(IdentityGolden, CorpusMatchesThePinnedIdentities) {
    const auto  golden  = read_json(kCorpusDir / "golden.json");
    const auto& entries = manifest_document().at("entries");
    ASSERT_EQ(golden.at("entries").size(), entries.size())
        << "golden.json and manifest.json list different entries; regenerate";
    ASSERT_GE(entries.size(), 20U);

    const auto registry = corpus_registry(manifest_document().at("registry"));
    EXPECT_EQ(registry.fingerprint(), golden.at("registry_fingerprint").get<std::string>());
    ProgramCompiler compiler(registry,
                             {manifest_document().at("compiler_build_id").get<std::string>()});
    const bool posix = posix_quickjs_identity(golden);

    std::size_t index = 0;
    for (const auto& entry : entries) {
        const auto& pinned = golden.at("entries")[index++];
        const auto  id     = entry.at("id").get<std::string>();
        ASSERT_EQ(pinned.at("id").get<std::string>(), id) << "entry order changed; regenerate";
        json actual;
        try {
            actual = entry_identity(compiler, entry);
        } catch (const ProgramCompileError& error) {
            std::string codes;
            for (const auto& diagnostic : error.diagnostics())
                codes += " " + diagnostic.code + "@" + diagnostic.primary.json_pointer;
            ADD_FAILURE() << id << " no longer compiles:" << codes;
            continue;
        }
        EXPECT_EQ(canonical(actual.at("definition")), canonical(pinned.at("definition")))
            << id << ": sealed definition bytes changed";
        for (const auto& [key, unused] : pinned.at("all_platforms").items()) {
            (void)unused;
            expect_field(actual.at("all_platforms"), pinned.at("all_platforms"), key, id);
        }
        EXPECT_EQ(actual.at("all_platforms").size(), pinned.at("all_platforms").size()) << id;
        if (actual.contains("posix_quickjs") && posix) {
            for (const auto& [key, unused] : pinned.at("posix_quickjs").items()) {
                (void)unused;
                expect_field(actual.at("posix_quickjs"), pinned.at("posix_quickjs"), key, id);
            }
        }
    }
}

TEST(IdentityGolden, GoldenFileIsTheDeterministicRegeneration) {
    const auto golden = read_json(kCorpusDir / "golden.json");
    if (!posix_quickjs_identity(golden))
        GTEST_SKIP() << "golden.json is rendered from the POSIX QuickJS identity";
    const auto first = render_golden(compute_golden(manifest_document()));
    EXPECT_EQ(first, render_golden(compute_golden(manifest_document())));
    EXPECT_EQ(first, read_file(kCorpusDir / "golden.json"))
        << "golden.json is not what regeneration writes; see the header of this file";
}

// The definition-level identities read only the sealed Core definition, the
// compiler build id and the registry. Sealing each JavaScript definition again
// through the C++ builder frontend, where QuickJS plays no part, must give the
// same definition_hash and compiled-plan identity. This is why those pins hold
// on every platform although the JavaScript bundle ids do not.
TEST(IdentityGolden, DefinitionIdentityDoesNotDependOnTheFrontend) {
    const auto      registry = corpus_registry(manifest_document().at("registry"));
    ProgramCompiler compiler(registry,
                             {manifest_document().at("compiler_build_id").get<std::string>()});
    const auto      budgets = read_json(kCorpusDir / "sources" / "cpp_minimal.json")
                             .at("declared_budget_requirements");
    std::size_t checked = 0;
    for (const auto& entry : manifest_document().at("entries")) {
        if (entry.at("frontend").get<std::string>() != "javascript") continue;
        const auto id     = entry.at("id").get<std::string>();
        const auto bundle = compiler.compile(corpus_source(entry));
        const auto sealed = bundle.sealed_core_definitions().at(0);
        json       document{
            {"program_schema_version", 1},
            {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"root", json{{"op", "call_core"}, {"name", sealed.name}, {"definition", sealed.definition}}},
            {"declared_budget_requirements", budgets}};
        const auto twin = compiler.compile(
            ProgramSource::from_cpp_builder("identity-corpus/twin/" + id + ".json", 1, document));
        const auto twin_sealed = twin.sealed_core_definitions().at(0);
        EXPECT_EQ(twin_sealed.definition_hash, sealed.definition_hash) << id;
        EXPECT_EQ(canonical(twin_sealed.definition), canonical(sealed.definition)) << id;
        EXPECT_EQ(twin.core_plan_identities().at(0).compiled_plan_identity,
                  bundle.core_plan_identities().at(0).compiled_plan_identity)
            << id;
        EXPECT_EQ(sealed_core_definition_hash(sealed.definition), sealed.definition_hash) << id;
        ++checked;
    }
    EXPECT_GE(checked, 15U);
}

TEST(IdentityGolden, ParsedBundlesKeepTheirIds) {
    const auto      registry = corpus_registry(manifest_document().at("registry"));
    ProgramCompiler compiler(registry,
                             {manifest_document().at("compiler_build_id").get<std::string>()});
    for (const auto& entry : manifest_document().at("entries")) {
        const auto bundle = compiler.compile(corpus_source(entry));
        EXPECT_EQ(ProgramBundle::parse(bundle.serialize_canonical()).id(), bundle.id())
            << entry.at("id").get<std::string>();
    }
}

#else

TEST(IdentityGolden, RequiresQuickJs) {
    GTEST_SKIP() << "the identity corpus includes JavaScript programs; build with "
                    "NEOGRAPH_BUILD_QUICKJS_CONTROL=ON";
}

#endif
