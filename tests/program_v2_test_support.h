// Shared fixtures for the Program API v2 suites (#362-#365): one registry, a
// Program document wrapper for Core definitions, and helpers that return the
// diagnostics a compilation returns whether it succeeds or fails.
#pragma once

#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "canonical_json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace program_v2_test {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

inline std::string digest(char value) { return "sha256:" + std::string(64, value); }

inline ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:program-v2-test",
                              {},
                              {},
                              {}};
}

class QuietNode final : public GraphNode {
public:
    explicit QuietNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

/// t.task and t.review are node types with an open config; t.check is a
/// closed condition with labels pass/fail; append and overwrite are reducers.
inline RegistrySnapshot registry() {
    RegistrySnapshotBuilder builder;
    const auto              factory = [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<QuietNode>(name);
    };
    builder.add_node(manifest(ExecutableKind::Node, "t.task", '1'), factory,
                     json{{"type", "object"}}, json::object());
    builder.add_node(manifest(ExecutableKind::Node, "t.review", '2'), factory,
                     json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "append", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '4'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_condition(manifest(ExecutableKind::Condition, "t.check", '5'),
                          [](const GraphState&) { return std::string("pass"); },
                          ConditionSpec{{"pass", "fail"}, false});
    return std::move(builder).build();
}

inline ProgramCompiler compiler(ProgramCompilerConfig config = {"program-v2-test/v1"}) {
    return ProgramCompiler(registry(), std::move(config));
}

/// A schema-1 Program document whose root calls the given Core definition.
inline json program_document(json definition) {
    const auto name = definition.value("name", std::string("main"));
    json       budgets = json::array();
    const std::vector<std::tuple<std::string, std::uint64_t, std::uint64_t>> limits{
        {"wall_time_ms", 1, 60000},       {"model_tokens", 0, 10000},
        {"monetary_microunits", 0, 1000000}, {"max_concurrency", 1, 1},
        {"max_program_operations", 1, 1}, {"max_core_steps", 1, 100},
        {"max_dynamic_compiles", 0, 0},   {"max_child_depth", 0, 0},
        {"max_total_children", 0, 0}};
    for (const auto& [resource, minimum, maximum] : limits)
        budgets.push_back(json{{"resource", resource}, {"minimum", minimum}, {"maximum", maximum}});
    return json{{"program_schema_version", 1},
                {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
                {"root", json{{"op", "call_core"}, {"name", name}, {"definition", std::move(definition)}}},
                {"declared_budget_requirements", std::move(budgets)}};
}

inline ProgramSource cpp_source(json document, std::string id = "program-v2/test.json") {
    return ProgramSource::from_cpp_builder(std::move(id), 1, std::move(document));
}

inline ProgramSource js_source(std::string body, std::string id = "program-v2/test.js") {
    return ProgramSource::from_javascript(std::move(id), std::move(body));
}

/// The diagnostics a compilation returns: compile_diagnostics() on success,
/// ProgramCompileError::diagnostics() on failure.
struct Outcome {
    std::optional<ProgramBundle> bundle;
    std::vector<Diagnostic>      returned;

    bool ok() const { return bundle.has_value(); }
};

inline Outcome compile(const ProgramSource& source, const ProgramCompiler& with) {
    Outcome outcome;
    try {
        auto bundle      = with.compile(source);
        outcome.returned = bundle.compile_diagnostics();
        outcome.bundle.emplace(std::move(bundle));
    } catch (const ProgramCompileError& error) {
        outcome.returned = error.diagnostics();
    }
    return outcome;
}

inline Outcome compile(const ProgramSource& source) { return compile(source, compiler()); }

inline std::vector<Diagnostic> with_code(const std::vector<Diagnostic>& diagnostics,
                                         std::string_view                code) {
    std::vector<Diagnostic> result;
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.code == code) result.push_back(diagnostic);
    return result;
}

inline std::string codes(const std::vector<Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics)
        result += " " + diagnostic.code + "@" + diagnostic.primary.json_pointer;
    return result;
}

inline std::string canonical(const json& value) {
    return neograph::program::detail::canonical_json_bytes(value);
}

}  // namespace program_v2_test
