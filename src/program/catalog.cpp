#include <neograph/graph/engine.h>
#include <neograph/program/catalog.h>
#include <neograph/program/compiler.h>
#include <neograph/program/store.h>

#include "canonical_json.h"
#include "catalog_access.h"
#include "registry_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

constexpr std::array<std::string_view, 9> kBudgetResources = {
    "wall_time_ms",         "model_tokens",           "monetary_microunits",
    "max_concurrency",      "max_program_operations", "max_core_steps",
    "max_dynamic_compiles", "max_child_depth",        "max_total_children",
};

json identity_json(const ExecutableIdentity& identity) {
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

bool identity_less(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
    return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                      lhs.implementation_digest} < std::tuple{to_string(rhs.kind), rhs.name,
                                                              rhs.semantic_version,
                                                              rhs.implementation_digest};
}

std::string escape_pointer_segment(std::string_view segment) {
    std::string result;
    result.reserve(segment.size());
    for (const char value : segment) {
        if (value == '~')
            result += "~0";
        else if (value == '/')
            result += "~1";
        else
            result.push_back(value);
    }
    return result;
}

class AdmissionDiagnostics {
public:
    explicit AdmissionDiagnostics(std::string source_id) : source_id_(std::move(source_id)) {}

    void add(std::string  code,
             std::string  pointer,
             std::string  message,
             json         witness = json::object(),
             CompilePhase phase   = CompilePhase::Seal) {
        Diagnostic diagnostic;
        diagnostic.phase    = phase;
        diagnostic.code     = std::move(code);
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.primary  = {source_id_, std::move(pointer), std::nullopt};
        diagnostic.message  = std::move(message);
        diagnostic.witness  = detail::owned_json_copy(witness);
        diagnostics_.push_back(std::move(diagnostic));
    }

    bool empty() const noexcept { return diagnostics_.empty(); }

    [[noreturn]] void throw_error() {
        std::sort(diagnostics_.begin(), diagnostics_.end(), [](const auto& lhs, const auto& rhs) {
            return std::tuple{static_cast<int>(lhs.phase), lhs.primary.json_pointer, lhs.code,
                              detail::canonical_json_bytes(lhs.witness)} <
                   std::tuple{static_cast<int>(rhs.phase), rhs.primary.json_pointer, rhs.code,
                              detail::canonical_json_bytes(rhs.witness)};
        });
        throw ProgramAdmissionError(std::move(diagnostics_));
    }

private:
    std::string             source_id_;
    std::vector<Diagnostic> diagnostics_;
};

std::vector<ExecutableIdentity> runtime_binding_identities(
    const std::vector<ExecutableIdentity>& closure) {
    std::vector<ExecutableIdentity> result;
    for (const auto& identity : closure) {
        if (identity.kind == ExecutableKind::Provider ||
            identity.kind == ExecutableKind::Tool ||
            identity.kind == ExecutableKind::Imported) {
            result.push_back(identity);
        }
    }
    std::sort(result.begin(), result.end(), identity_less);
    return result;
}


void validate_capability_binding(const std::vector<ExecutableIdentity>& requested,
                                 CatalogCapabilityBinding&              binding,
                                 AdmissionDiagnostics&                  diagnostics) {
    for (std::size_t index = 0; index < binding.receipts.size(); ++index) {
        const auto& receipt = binding.receipts[index];
        try {
            if (receipt.executable.kind != ExecutableKind::Provider &&
                receipt.executable.kind != ExecutableKind::Tool &&
                receipt.executable.kind != ExecutableKind::Imported) {
                throw std::invalid_argument(
                    "executable kind must be Provider, Tool, or Imported");
            }
            detail::validate_token(receipt.executable.name, "Capability executable name");
            if (!detail::is_semantic_version(receipt.executable.semantic_version)) {
                throw std::invalid_argument("executable semantic version is not valid SemVer");
            }
            if (!detail::is_sha256_identity(receipt.executable.implementation_digest)) {
                throw std::invalid_argument("executable implementation identity is not sha256");
            }
        } catch (const std::exception& error) {
            diagnostics.add("P_BINDING_IDENTITY",
                            "/capability_bindings/" + std::to_string(index) + "/executable",
                            "Capability binding executable identity is malformed",
                            json{{"error", error.what()}});
        }
    }
    std::sort(binding.receipts.begin(), binding.receipts.end(),
              [](const auto& lhs, const auto& rhs) {
                  return identity_less(lhs.executable, rhs.executable);
              });

    for (std::size_t index = 0; index < binding.receipts.size(); ++index) {
        const auto& receipt = binding.receipts[index];
        if (!detail::is_sha256_identity(receipt.binding_identity)) {
            diagnostics.add(
                "P_BINDING_IDENTITY", "/capability_bindings/" + std::to_string(index),
                "Capability binding identity must be a sha256 identity",
                json{{"executable", identity_json(receipt.executable)},
                     {"binding_identity", receipt.binding_identity}});
        }
        if (index > 0 &&
            binding.receipts[index - 1].executable.kind == receipt.executable.kind &&
            binding.receipts[index - 1].executable.name == receipt.executable.name) {
            diagnostics.add("P_BINDING_DUPLICATE",
                            "/capability_bindings/" + std::to_string(index),
                            "Capability binding contains a duplicate executable identity",
                            identity_json(receipt.executable));
        }
    }

    std::vector<ExecutableIdentity> bound;
    bound.reserve(binding.receipts.size());
    for (const auto& receipt : binding.receipts)
        bound.push_back(receipt.executable);
    if (bound != requested) {
        json expected = json::array();
        json actual   = json::array();
        for (const auto& identity : requested)
            expected.push_back(identity_json(identity));
        for (const auto& identity : bound)
            actual.push_back(identity_json(identity));
        diagnostics.add("P_BINDING_COVERAGE", "/capability_bindings",
                        "Capability binding must cover every runtime executable exactly once",
                        json{{"expected", std::move(expected)}, {"actual", std::move(actual)}});
    }

    const auto provider_count = static_cast<std::size_t>(
        std::count_if(requested.begin(), requested.end(), [](const auto& identity) {
            return identity.kind == ExecutableKind::Provider;
        }));
    std::vector<std::string> expected_tool_names;
    for (const auto& identity : requested) {
        if (identity.kind == ExecutableKind::Tool) expected_tool_names.push_back(identity.name);
    }
    const auto tool_count = expected_tool_names.size();
    if (provider_count > 1) {
        diagnostics.add("P_BINDING_PROVIDER", "/capability_bindings",
                        "NodeContext can bind at most one exact Provider",
                        json{{"provider_count", provider_count}});
    } else if ((provider_count == 1) != static_cast<bool>(binding.node_context.provider)) {
        diagnostics.add("P_BINDING_PROVIDER", "/capability_bindings",
                        "Owned Provider presence does not match the exact closure",
                        json{{"provider_count", provider_count},
                             {"provider_bound", static_cast<bool>(binding.node_context.provider)}});
    }
    if (binding.tools.size() != tool_count) {
        diagnostics.add("P_BINDING_TOOL", "/capability_bindings",
                        "Owned Tool count does not match the exact closure",
                        json{{"expected", tool_count}, {"actual", binding.tools.size()}});
    }
    std::vector<std::string> bound_tool_names;
    try {
        for (const auto* tool : binding.tools.view()) {
            bound_tool_names.push_back(tool->get_name());
        }
        std::sort(bound_tool_names.begin(), bound_tool_names.end());
        std::sort(expected_tool_names.begin(), expected_tool_names.end());
        if (bound_tool_names != expected_tool_names) {
            diagnostics.add("P_BINDING_TOOL", "/capability_bindings",
                            "Owned Tool names do not match the exact Tool closure",
                            json{{"expected", expected_tool_names}, {"actual", bound_tool_names}});
        }
    } catch (const std::exception& error) {
        diagnostics.add("P_BINDING_TOOL", "/capability_bindings",
                        "Owned Tool identity inspection failed",
                        json{{"error", error.what()}});
    } catch (...) {
        diagnostics.add("P_BINDING_TOOL", "/capability_bindings",
                        "Owned Tool identity inspection failed with a non-standard exception");
    }

    binding.node_context.tools = binding.tools.view();
}

bool contains_source_kind(const std::vector<SourceKind>& values, SourceKind wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool contains_effect_mode(const std::vector<EffectMode>& values, EffectMode wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool contains_identity(const std::vector<ExecutableIdentity>& values,
                       const ExecutableIdentity&              wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool contains_string(const std::vector<std::string>& values, std::string_view wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

std::string dependency_merkle_root(std::vector<DependencyReceipt> receipts) {
    std::sort(receipts.begin(), receipts.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.dependency_id, lhs.content_identity) <
               std::tie(rhs.dependency_id, rhs.content_identity);
    });
    if (receipts.empty()) return detail::sha256_identity("program-import-empty/v1", "");

    std::vector<std::string> level;
    level.reserve(receipts.size());
    for (const auto& receipt : receipts) {
        const json leaf{{"source_id", receipt.dependency_id},
                        {"content_identity", receipt.content_identity}};
        level.push_back(
            detail::sha256_identity("program-import-leaf/v1", detail::canonical_json_bytes(leaf)));
    }
    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());
        std::vector<std::string> next;
        next.reserve(level.size() / 2);
        for (std::size_t index = 0; index < level.size(); index += 2) {
            next.push_back(
                detail::sha256_identity("program-import-node/v1", level[index] + level[index + 1]));
        }
        level = std::move(next);
    }
    return level.front();
}

struct DirectReference {
    ExecutableKind kind;
    std::string    name;
    std::string    pointer;
};

std::vector<DirectReference> direct_references(const graph::TopologySpec& topology) {
    std::vector<DirectReference> result;
    for (const auto& [node_name, definition] : topology.node_defs) {
        result.push_back({ExecutableKind::Node, definition.at("type").get<std::string>(),
                          "/nodes/" + node_name + "/type"});
    }
    for (const auto& channel : topology.channel_defs) {
        result.push_back({ExecutableKind::Reducer, channel.reducer_name,
                          "/channels/" + channel.name + "/reducer"});
    }
    for (std::size_t index = 0; index < topology.conditional_edges.size(); ++index) {
        result.push_back({ExecutableKind::Condition, topology.conditional_edges[index].condition,
                          "/conditional_edges/" + std::to_string(index) + "/condition"});
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.kind, lhs.name, lhs.pointer) <
               std::tie(rhs.kind, rhs.name, rhs.pointer);
    });
    return result;
}

struct ClosureResult {
    std::vector<ExecutableIdentity> identities;
    CapabilityEffectClosure         closure;
    std::vector<EffectMode>         modes;
};

ClosureResult resolve_closure(const RegistrySnapshot&    registry,
                              const graph::TopologySpec& topology,
                              AdmissionDiagnostics&      diagnostics) {
    struct Work {
        ExecutableIdentity                expected;
        std::string                       pointer;
        std::optional<ExecutableIdentity> required_by;
    };

    std::vector<Work> work;
    for (const auto& reference : direct_references(topology)) {
        try {
            const auto& manifest = detail::RegistrySnapshotAccess::require_manifest(
                registry, reference.kind, reference.name);
            work.push_back({manifest.identity, reference.pointer, std::nullopt});
        } catch (const std::out_of_range&) {
            diagnostics.add(
                "P_REGISTRY_MISSING", reference.pointer,
                "Referenced executable is absent from the Catalog registry",
                json{{"kind", std::string(to_string(reference.kind))}, {"name", reference.name}},
                CompilePhase::Resolve);
        }
    }
    for (const auto& [node_name, definition] : topology.node_defs) {
        const auto type = definition.at("type").get<std::string>();
        const auto pointer = "/sealed_core_definitions/0/definition/nodes/" +
                             escape_pointer_segment(node_name);
        const ExecutableManifest* node_manifest = nullptr;
        try {
            node_manifest = &detail::RegistrySnapshotAccess::require_manifest(
                registry, ExecutableKind::Node, type);
        } catch (const std::out_of_range&) {
            continue;
        }

        std::vector<ExecutableIdentity> requirements;
        try {
            requirements = detail::RegistrySnapshotAccess::resolve_node_requirements(
                registry, type, definition);
        } catch (const std::exception& error) {
            diagnostics.add("P_REGISTRY_REQUIREMENT_RESOLVER", pointer,
                            "Node executable requirement resolution failed",
                            json{{"node_type", type}, {"exception", error.what()}},
                            CompilePhase::Resolve);
            continue;
        } catch (...) {
            diagnostics.add(
                "P_REGISTRY_REQUIREMENT_RESOLVER", pointer,
                "Node executable requirement resolution failed",
                json{{"node_type", type}, {"exception", "non-standard exception"}},
                CompilePhase::Resolve);
            continue;
        }
        std::sort(requirements.begin(), requirements.end(), identity_less);
        requirements.erase(std::unique(requirements.begin(), requirements.end()),
                           requirements.end());
        for (const auto& requirement : requirements) {
            if (requirement.kind != ExecutableKind::Provider &&
                requirement.kind != ExecutableKind::Tool &&
                requirement.kind != ExecutableKind::Imported) {
                diagnostics.add(
                    "P_REGISTRY_REQUIREMENT_KIND", pointer,
                    "Node config resolver returned an unsupported executable kind",
                    json{{"node_type", type}, {"requirement", identity_json(requirement)}},
                    CompilePhase::Resolve);
                continue;
            }
            work.push_back({requirement, pointer, node_manifest->identity});
        }
    }

    std::map<std::pair<std::string, std::string>, ExecutableIdentity> visited;
    std::set<std::string>                                             capabilities;
    std::set<std::string>                                             effects;
    std::set<EffectMode>                                              modes;
    for (std::size_t index = 0; index < work.size(); ++index) {
        const auto current = work[index];
        const auto key =
            std::pair{std::string(to_string(current.expected.kind)), current.expected.name};
        const auto previous = visited.find(key);
        if (previous != visited.end()) {
            if (previous->second != current.expected) {
                json witness{{"expected", identity_json(current.expected)},
                             {"resolved", identity_json(previous->second)}};
                if (current.required_by)
                    witness["required_by"] = identity_json(*current.required_by);
                diagnostics.add("P_REGISTRY_IDENTITY_MISMATCH", current.pointer,
                                "Executable closure contains conflicting exact identities",
                                std::move(witness), CompilePhase::Resolve);
            }
            continue;
        }

        const ExecutableManifest* manifest = nullptr;
        try {
            manifest = &detail::RegistrySnapshotAccess::require_manifest(
                registry, current.expected.kind, current.expected.name);
        } catch (const std::out_of_range&) {
            json witness{{"expected", identity_json(current.expected)}};
            if (current.required_by) witness["required_by"] = identity_json(*current.required_by);
            diagnostics.add("P_REGISTRY_DEPENDENCY_MISSING", current.pointer,
                            "Required executable dependency is absent from the Catalog registry",
                            std::move(witness), CompilePhase::Resolve);
            continue;
        }
        if (manifest->identity != current.expected) {
            json witness{{"expected", identity_json(current.expected)},
                         {"resolved", identity_json(manifest->identity)}};
            if (current.required_by) witness["required_by"] = identity_json(*current.required_by);
            diagnostics.add("P_REGISTRY_IDENTITY_MISMATCH", current.pointer,
                            "Required executable dependency has a mismatched exact identity",
                            std::move(witness), CompilePhase::Resolve);
            continue;
        }

        visited.emplace(key, manifest->identity);
        capabilities.insert(manifest->required_capabilities.begin(),
                            manifest->required_capabilities.end());
        effects.insert(manifest->declared_effects.begin(), manifest->declared_effects.end());
        modes.insert(manifest->effect_mode);
        if (manifest->effect_mode == EffectMode::TrustedNative) {
            capabilities.insert(std::string(TRUSTED_NATIVE_CAPABILITY));
        }
        for (const auto& dependency : manifest->required_executables) {
            work.push_back(
                {dependency, "/sealed_core_definitions/0/definition", manifest->identity});
        }
    }

    ClosureResult result;
    result.identities.reserve(visited.size());
    for (const auto& [ignored, identity] : visited) {
        (void)ignored;
        result.identities.push_back(identity);
    }
    std::sort(result.identities.begin(), result.identities.end(), identity_less);
    result.closure.capabilities.assign(capabilities.begin(), capabilities.end());
    result.closure.effects.assign(effects.begin(), effects.end());
    result.modes.assign(modes.begin(), modes.end());
    return result;
}

json contract_json(const ContractRecord& contract) {
    return json{{"schema_version", contract.schema_version}, {"schema", contract.schema}};
}

std::string recompute_program_hash(const ProgramBundle&           bundle,
                                   const OrchestrationPlanRecord& plan,
                                   const SealedCoreDefinition&    definition) {
    json budgets = json::array();
    for (const auto& budget : bundle.declared_budget_requirements()) {
        budgets.push_back(json{{"resource", budget.resource},
                               {"minimum", budget.minimum},
                               {"maximum", budget.maximum}});
    }
    const json semantic{
        {"program_schema_version", ProgramCompiler::PROGRAM_SCHEMA_VERSION},
        {"input_contract", contract_json(bundle.input_contract())},
        {"output_contract", contract_json(bundle.output_contract())},
        {"orchestration_plan", json{{"schema_version", plan.schema_version}, {"plan", plan.plan}}},
        {"sealed_core_definitions",
         json::array({json{{"name", definition.name},
                           {"definition_hash", definition.definition_hash},
                           {"definition", definition.definition}}})},
        {"declared_budget_requirements", std::move(budgets)}};
    return detail::sha256_identity("canonical-program/v1", detail::canonical_json_bytes(semantic));
}
bool valid_orchestration_plan(const OrchestrationPlanRecord& plan,
                              std::string_view               core_name,
                              json&                          witness) {
    if (plan.schema_version != 1 || !plan.plan.is_object() ||
        !plan.plan.contains("root") || !plan.plan["root"].is_string() ||
        !plan.plan.contains("operations") || !plan.plan["operations"].is_array()) {
        witness = json{{"reason", "plan envelope is malformed"}};
        return false;
    }

    const auto root = plan.plan["root"].get<std::string>();
    std::map<std::string, json> operations;
    const auto fail = [&](std::string reason, std::string id = {},
                          std::string field = {}) {
        witness = json{{"reason", std::move(reason)}};
        if (!id.empty()) witness["id"] = std::move(id);
        if (!field.empty()) witness["field"] = std::move(field);
        return false;
    };
    const auto reference = [&](const json& operation, std::string_view field,
                              const std::string& id) {
        const auto key = std::string(field);
        if (!operation.contains(key) || !operation[key].is_string()) {
            return fail("operation reference is missing or malformed", id, key);
        }
        const auto target = operation[key].get<std::string>();
        if (!operations.contains(target))
            return fail("operation reference is dangling", id, key);
        return true;
    };
    const auto references = [&](const json& operation, std::string_view field,
                                const std::string& id, std::size_t minimum) {
        const auto key = std::string(field);
        if (!operation.contains(key) || !operation[key].is_array() ||
            operation[key].size() < minimum) {
            return fail("operation reference list is missing or too short", id, key);
        }
        for (const auto& child : operation[key]) {
            if (!child.is_string() || !operations.contains(child.get<std::string>()))
                return fail("operation reference list contains a dangling entry", id, key);
        }
        return true;
    };
    const auto positive_bound = [&](const json& operation, std::string_view field,
                                    const std::string& id) {
        const auto key = std::string(field);
        if (!operation.contains(key) || !operation[key].is_number_unsigned() ||
            operation[key].get<std::uint64_t>() == 0)
            return fail("operation bound is missing or non-positive", id, key);
        return true;
    };

    for (const auto& operation : plan.plan["operations"]) {
        if (!operation.is_object() || !operation.contains("id") ||
            !operation["id"].is_string() || !operation.contains("op") ||
            !operation["op"].is_string()) {
            return fail("operation envelope is malformed");
        }
        const auto id = operation["id"].get<std::string>();
        if (id.empty() || !operations.emplace(id, operation).second)
            return fail("operation id is empty or duplicated", id);
    }

    bool has_execution = false;
    for (const auto& [id, operation] : operations) {
        const auto op = operation["op"].get<std::string>();
        if (op != "call_core" && op != "sequence" && op != "branch" && op != "loop" &&
            op != "retry" && op != "parallel" && op != "race" && op != "quorum" &&
            op != "map" && op != "spawn" && op != "await" && op != "emit" &&
            op != "checkpoint" && op != "cancel" && op != "return") {
            return fail("unknown operation", id, "op");
        }
        if (op == "call_core") {
            if (!operation.contains("core") || !operation["core"].is_string() ||
                operation["core"].get<std::string>() != core_name)
                return fail("call_core does not bind the sealed Core", id, "core");
            has_execution = true;
        } else if (op == "sequence") {
            if (!references(operation, "children", id, 1)) return false;
        } else if (op == "branch") {
            if (!operation.contains("condition") || !operation["condition"].is_object())
                return fail("branch condition is missing or malformed", id, "condition");
            if (!reference(operation, "then", id)) return false;
            if (operation.contains("else") && !reference(operation, "else", id)) return false;
        } else if (op == "loop") {
            if (!operation.contains("condition") || !operation["condition"].is_object())
                return fail("loop condition is missing or malformed", id, "condition");
            if (!reference(operation, "body", id) ||
                !positive_bound(operation, "max_iterations", id))
                return false;
        } else if (op == "retry") {
            if (!reference(operation, "body", id) ||
                !positive_bound(operation, "max_attempts", id))
                return false;
        } else if (op == "parallel") {
            if (!references(operation, "branches", id, 2)) return false;
        } else if (op == "race") {
            if (!references(operation, "branches", id, 2) ||
                operation["branches"].size() != 2)
                return fail("race currently requires exactly two branches", id, "branches");
        } else if (op == "quorum") {
            if (!references(operation, "branches", id, 2)) return false;
            if (!positive_bound(operation, "min_success", id) ||
                operation["min_success"].get<std::uint64_t>() > operation["branches"].size())
                return fail("quorum success bound exceeds its branch count", id, "min_success");
        } else if (op == "map") {
            if (!operation.contains("items") || !operation["items"].is_array() ||
                operation["items"].empty())
                return fail("map items are missing or empty", id, "items");
            if (!reference(operation, "body", id)) return false;
        } else if (op == "spawn") {
            if (operation.contains("body"))
                return fail("spawn must not carry an inline body", id, "body");
            if (!operation.contains("child_binding") || !operation["child_binding"].is_string() ||
                operation["child_binding"].get<std::string>().empty())
                return fail("spawn child binding is missing or malformed", id, "child_binding");
            const bool directly_joined = std::any_of(
                operations.begin(), operations.end(), [&](const auto& candidate) {
                    return candidate.second["op"] == "await" &&
                           candidate.second.contains("body") &&
                           candidate.second["body"].is_string() &&
                           candidate.second["body"].template get<std::string>() == id;
                });
            if (!directly_joined)
                return fail("spawn is not directly joined by await", id, "child_binding");
            has_execution = true;
        } else if (op == "await") {
            if (!reference(operation, "body", id)) return false;
        } else if (op == "checkpoint") {
            if (operation.contains("body") && !reference(operation, "body", id)) return false;
        } else if (op == "emit" || op == "return") {
            if (!operation.contains("value"))
                return fail("value operation has no value", id, "value");
        }
    }
    if (!has_execution) return fail("operation graph contains no execution operation");
    std::set<std::string, std::less<>> active;
    std::set<std::string, std::less<>> visited;
    bool reachable_execution = false;
    std::function<bool(const std::string&)> visit = [&](const std::string& id) {
        if (active.contains(id)) return fail("operation graph contains a cycle", id);
        if (!visited.insert(id).second) return true;
        active.insert(id);
        const auto& operation = operations.at(id);
        const auto  op = operation["op"].get<std::string>();
        if (op == "call_core" || op == "spawn") {
            reachable_execution = true;
        } else {
            const auto visit_one = [&](const json& value) {
                return value.is_string() && visit(value.get<std::string>());
            };
            const auto visit_many = [&](const json& values) {
                if (!values.is_array()) return false;
                for (const auto& value : values)
                    if (!visit_one(value)) return false;
                return true;
            };
            bool valid = true;
            if (op == "sequence" || op == "parallel" || op == "race" ||
                op == "quorum") {
                valid = visit_many(operation.at(op == "sequence" ? "children" : "branches"));
            } else if (op == "branch") {
                valid = visit_one(operation.at("then"));
                if (valid && operation.contains("else")) valid = visit_one(operation.at("else"));
            } else if (op == "loop" || op == "retry" || op == "map" || op == "await") {
                valid = visit_one(operation.at("body"));
            } else if (op == "checkpoint" && operation.contains("body")) {
                valid = visit_one(operation.at("body"));
            }
            if (!valid) return false;
        }
        active.erase(id);
        return true;
    };
    if (!operations.contains(root) || !visit(root))
        return fail("operation graph is not a finite rooted DAG", root, "root");
    if (!reachable_execution)
        return fail("rooted operation graph contains no execution operation");
    if (visited.size() != operations.size())
        return fail("operation graph contains unreachable operations");
    return true;
}

std::uint64_t policy_ceiling(std::string_view resource, const BudgetLimits& limits) {
    if (resource == "wall_time_ms") return limits.wall_time_ms;
    if (resource == "model_tokens") return limits.model_tokens;
    if (resource == "monetary_microunits") return limits.monetary_microunits;
    if (resource == "max_concurrency") return limits.max_concurrency;
    if (resource == "max_program_operations") return limits.max_program_operations;
    if (resource == "max_core_steps") return limits.max_core_steps;
    if (resource == "max_dynamic_compiles") return limits.max_dynamic_compiles;
    if (resource == "max_child_depth") return limits.max_child_depth;
    if (resource == "max_total_children") return limits.max_total_children;
    return 0;
}

void validate_budgets(const ProgramBundle&  bundle,
                      const PolicySnapshot& policy,
                      AdmissionDiagnostics& diagnostics) {
    const auto& budgets = bundle.declared_budget_requirements();
    std::map<std::string, const BudgetRequirement*> by_resource;
    for (const auto& budget : budgets)
        by_resource.emplace(budget.resource, &budget);

    for (const auto resource : kBudgetResources) {
        const auto found = by_resource.find(std::string(resource));
        if (found == by_resource.end()) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/declared_budget_requirements",
                            "Program-v1 requires all nine exact budget records",
                            json{{"missing_resource", std::string(resource)}});
            continue;
        }
        const auto& budget     = *found->second;
        bool        structural = true;
        if (resource == "max_program_operations" || resource == "max_concurrency" ||
            resource == "max_core_steps") {
            structural = budget.minimum >= 1;
        } else if (resource == "max_dynamic_compiles") {
            structural = budget.minimum == 0 && budget.maximum == 0;
        } else if (resource == "max_child_depth" || resource == "max_total_children") {
            structural = (budget.minimum == 0 && budget.maximum == 0) ||
                         (budget.minimum >= 1 && budget.maximum >= budget.minimum);
        }
        if (!structural) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/declared_budget_requirements",
                            "Budget record violates the Program-v1 structural floor",
                            json{{"resource", budget.resource},
                                 {"minimum", budget.minimum},
                                 {"maximum", budget.maximum}});
        }
        const auto ceiling = policy_ceiling(resource, policy.budget_ceiling());
        if (budget.maximum > ceiling) {
            diagnostics.add("P_ADMIT_POLICY", "/declared_budget_requirements",
                            "Declared budget maximum exceeds the policy ceiling",
                            json{{"resource", budget.resource},
                                 {"maximum", budget.maximum},
                                 {"policy_ceiling", ceiling}});
        }
    }
    if (by_resource.size() != kBudgetResources.size()) {
        diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/declared_budget_requirements",
                        "Budget closure differs from the closed Program-v1 resource set",
                        json{{"actual_count", by_resource.size()},
                             {"required_count", kBudgetResources.size()}});
    }
}


void map_core_parse_diagnostics(const graph::ParseReport& report,
                                AdmissionDiagnostics&     diagnostics) {
    for (const auto& core : report.diagnostics) {
        diagnostics.add("P_CORE_" + core.code,
                        "/sealed_core_definitions/0/definition" + core.json_pointer, core.message,
                        json{{"core_code", core.code}, {"core_witness", core.witness}},
                        CompilePhase::CoreParse);
    }
}

void map_roundtrip_diagnostics(const graph::RoundTripReport& report,
                               AdmissionDiagnostics&         diagnostics) {
    for (const auto& core : report.diagnostics) {
        diagnostics.add("P_CORE_ROUNDTRIP",
                        "/sealed_core_definitions/0/definition" + core.json_pointer, core.message,
                        json{{"core_code", core.code}, {"core_witness", core.witness}});
    }
}

void map_validation_diagnostics(const graph::ValidationReport& report,
                                AdmissionDiagnostics&          diagnostics) {
    for (const auto& core : report.diagnostics) {
        if (core.severity == "warning") continue;
        diagnostics.add("P_CORE_" + core.code,
                        "/sealed_core_definitions/0/definition" + core.json_pointer, core.message,
                        json{{"core_code", core.code}, {"core_witness", core.witness}},
                        CompilePhase::CoreValidate);
    }
}

Diagnostic start_not_admitted(std::string_view version_id) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = "P_START_NOT_ADMITTED";
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"catalog", "/program_version_id", std::nullopt};
    diagnostic.message = "Program version is not an exact admitted materialization in this Catalog";
    diagnostic.witness = json{{"program_version_id", std::string(version_id)}};
    return diagnostic;
}

}  // namespace

ProgramAdmissionError::ProgramAdmissionError(std::vector<Diagnostic> diagnostics)
    : std::runtime_error("Program admission failed with " + std::to_string(diagnostics.size()) +
                         " diagnostic(s)"),
      diagnostics_(std::move(diagnostics)) {}

const std::vector<Diagnostic>& ProgramAdmissionError::diagnostics() const noexcept {
    return diagnostics_;
}

struct EngineGenerationCache::Impl {
    struct Key {
        std::string bundle_id;
        std::string core_name;
        std::string compiled_plan_identity;
        std::string registry_fingerprint;
        std::string compiler_build_id;
        std::string capability_binding_root;
        std::size_t worker_count = 1;

        bool operator<(const Key& other) const noexcept {
            return std::tie(bundle_id, core_name, compiled_plan_identity, registry_fingerprint,
                            compiler_build_id, capability_binding_root, worker_count) <
                   std::tie(other.bundle_id, other.core_name, other.compiled_plan_identity,
                            other.registry_fingerprint, other.compiler_build_id,
                            other.capability_binding_root, other.worker_count);
        }
    };

    std::mutex                                                         mutex;
    std::optional<std::pair<std::string, std::string>>                 scope;
    std::map<Key, std::shared_ptr<const detail::PinnedCoreGeneration>> generations;
};

EngineGenerationCache::EngineGenerationCache() : impl_(std::make_unique<Impl>()) {}
EngineGenerationCache::EngineGenerationCache(EngineGenerationCache&&) noexcept            = default;
EngineGenerationCache& EngineGenerationCache::operator=(EngineGenerationCache&&) noexcept = default;
EngineGenerationCache::~EngineGenerationCache()                                           = default;

struct ProgramCatalog::Impl {
    Impl(std::shared_ptr<ProgramStore>          store,
         RegistrySnapshot                       registry_snapshot,
         std::shared_ptr<EngineGenerationCache> engine_cache,
         std::string                            build_id,
         CatalogCapabilityBinder                binder,
         std::size_t                            workers,
         std::shared_ptr<const ModuleStore>     modules,
         std::string                            host)
        : program_store(std::move(store)),
          registry(std::move(registry_snapshot)),
          engines(std::move(engine_cache)),
          compiler_build_id(std::move(build_id)),
          capability_binder(std::move(binder)),
          worker_count(workers),
          module_store(std::move(modules)),
          host_identity(std::move(host)) {}

    std::shared_ptr<ProgramStore>          program_store;
    RegistrySnapshot                       registry;
    std::shared_ptr<EngineGenerationCache> engines;
    std::string                            compiler_build_id;
    CatalogCapabilityBinder                capability_binder;
    std::size_t                            worker_count = 1;
    std::shared_ptr<const ModuleStore>     module_store;
    std::string                            host_identity;

    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const detail::MaterializedProgram>>
        materialized;
};

ProgramCatalog::ProgramCatalog(CatalogConfig config) {
    if (!config.program_store) {
        throw std::invalid_argument("CatalogConfig requires a ProgramStore");
    }
    if (!config.engines) {
        throw std::invalid_argument("CatalogConfig requires an EngineGenerationCache");
    }
    detail::validate_token(config.compiler_build_id, "Catalog compiler_build_id");
    if (config.worker_count == 0) {
        throw std::invalid_argument("CatalogConfig worker_count must be positive");
    }
    if (!config.host_identity.empty()) {
        detail::validate_token(config.host_identity, "Catalog host_identity");
    }

    {
        std::lock_guard lock(config.engines->impl_->mutex);
        const auto      wanted = std::pair{config.registry.fingerprint(), config.compiler_build_id};
        if (config.engines->impl_->scope && *config.engines->impl_->scope != wanted) {
            throw std::invalid_argument(
                "EngineGenerationCache is already scoped to another registry/compiler identity");
        }
        config.engines->impl_->scope = wanted;
    }

    impl_ = std::make_unique<Impl>(
        std::move(config.program_store), std::move(config.registry), std::move(config.engines),
        std::move(config.compiler_build_id), std::move(config.capability_binder),
        config.worker_count, std::move(config.module_store), std::move(config.host_identity));
}

ProgramCatalog::ProgramCatalog(ProgramCatalog&&) noexcept            = default;
ProgramCatalog& ProgramCatalog::operator=(ProgramCatalog&&) noexcept = default;
ProgramCatalog::~ProgramCatalog()                                    = default;

ProgramVersion ProgramCatalog::admit(const ProgramBundle& input_bundle,
                                     ProgramAdmission     admission) {
    return materialize(input_bundle, std::move(admission), std::nullopt, nullptr, true);
}

ProgramVersion ProgramCatalog::admit_composed(const ProgramBundle&      input_bundle,
                                              ProgramAdmission           admission,
                                              const ProgramComposition& composition) {
    if (!impl_->module_store)
        throw std::invalid_argument("Composed admission requires a configured ModuleStore");
    if (composition.parent.owner_scope() != admission.owner_scope)
        throw std::invalid_argument("Composed parent module owner differs from admission scope");
    validate_program_composition(input_bundle, composition);
    // Child identities are authority-bearing inputs. They must already be
    // present in the owner-qualified durable catalog; a caller cannot pass a
    // merely well-formed fabricated ProgramVersion into composition.
    for (const auto& child : composition.children) {
        const auto stored_version =
            impl_->program_store->get_version(admission.owner_scope, child.version.id());
        auto stored_bundle = impl_->program_store->get_bundle(admission.owner_scope,
                                                              child.bundle.id());
        if (!stored_bundle) stored_bundle = impl_->program_store->get_bundle(child.bundle.id());
        if (!stored_version || !stored_bundle ||
            stored_version->serialize_canonical() != child.version.serialize_canonical() ||
            stored_bundle->serialize_canonical() != child.bundle.serialize_canonical())
            throw std::invalid_argument("Composed child is not an exact admitted catalog value");
    }
    const auto receipts = composition.resolution.receipts;
    if (admission.dependency_receipts.empty()) {
        admission.dependency_receipts = receipts;
    } else if (admission.dependency_receipts != receipts) {
        throw std::invalid_argument(
            "Composed admission dependency receipts differ from the resolved module closure");
    }
    return admit(input_bundle, std::move(admission));
}

ProgramVersion ProgramCatalog::materialize(
    const ProgramBundle&                    input_bundle,
    ProgramAdmission                        admission,
    std::optional<CatalogCapabilityBinding> supplied_binding,
    const ProgramVersion*                   expected_version,
    bool                                    publish,
    std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization) {
    ProgramBundle bundle = [&]() {
        try {
            return ProgramBundle::parse(input_bundle.serialize_canonical());
        } catch (const std::exception& error) {
            AdmissionDiagnostics diagnostics("catalog");
            diagnostics.add("P_ADMIT_BUNDLE", "", "Program bundle failed canonical reparse",
                            json{{"error", error.what()}});
            diagnostics.throw_error();
        }
    }();

    AdmissionDiagnostics diagnostics(bundle.id());
    const auto           registry_fingerprint = impl_->registry.fingerprint();

    try {
        (void)bundle.typed_orchestration_plan();
    } catch (const std::exception& error) {
        diagnostics.add("P_ADMIT_PLAN", "/orchestration_plan",
                        "Bundle orchestration plan is not a valid typed immutable plan",
                        json{{"error", error.what()}});
    }

    if (bundle.compiler_build_id() != impl_->compiler_build_id) {
        diagnostics.add(
            "P_ADMIT_BINDING", "/compiler_build_id",
            "Bundle compiler build identity differs from the Catalog",
            json{{"bundle", bundle.compiler_build_id()}, {"catalog", impl_->compiler_build_id}});
    }
    if (bundle.program_schema_version() > admission.profile.max_program_schema_version()) {
        diagnostics.add("P_ADMIT_BINDING", "/program_schema_version",
                        "Bundle Program schema exceeds the admission profile maximum",
                        json{{"bundle", bundle.program_schema_version()},
                             {"profile_maximum", admission.profile.max_program_schema_version()}});
    }
    if (bundle.registry_snapshot_fingerprint() != registry_fingerprint ||
        admission.profile.registry_fingerprint() != registry_fingerprint ||
        admission.policy.registry_fingerprint() != registry_fingerprint) {
        diagnostics.add(
            "P_ADMIT_BINDING", "/registry_snapshot_fingerprint",
            "Bundle, profile, policy, and Catalog must bind one exact registry snapshot",
            json{{"bundle", bundle.registry_snapshot_fingerprint()},
                 {"profile", admission.profile.registry_fingerprint()},
                 {"policy", admission.policy.registry_fingerprint()},
                 {"catalog", registry_fingerprint}});
    }
    if (admission.policy.admission_profile_fingerprint() != admission.profile.fingerprint()) {
        diagnostics.add("P_ADMIT_BINDING", "/policy",
                        "Policy snapshot does not bind the supplied admission profile",
                        json{{"policy_profile", admission.policy.admission_profile_fingerprint()},
                             {"profile", admission.profile.fingerprint()}});
    }
    if (admission.owner_scope != admission.policy.owner_scope()) {
        diagnostics.add(
            "P_ADMIT_BINDING", "/owner_scope",
            "Admission owner scope differs from the policy owner",
            json{{"admission", admission.owner_scope}, {"policy", admission.policy.owner_scope()}});
    }
    if (!contains_source_kind(admission.profile.allowed_source_kinds(), bundle.source_kind())) {
        diagnostics.add("P_ADMIT_SOURCE_KIND", "/source_kind",
                        "Bundle source kind is not allowed by the admission profile",
                        json{{"source_kind", std::string(to_string(bundle.source_kind()))}});
    }

    std::set<std::string> dependency_ids;
    const auto            policy_digests = admission.policy.allowed_module_digests();
    for (std::size_t index = 0; index < admission.dependency_receipts.size(); ++index) {
        const auto& receipt = admission.dependency_receipts[index];
        const auto  pointer = "/dependency_receipts/" + std::to_string(index);
        try {
            detail::validate_token(receipt.dependency_id, "Dependency receipt id");
            if (!detail::is_sha256_identity(receipt.content_identity)) {
                throw std::invalid_argument("content identity is not sha256");
            }
        } catch (const std::exception& error) {
            diagnostics.add("P_ADMIT_DEPENDENCY", pointer, "Dependency receipt is malformed",
                            json{{"error", error.what()}});
        }
        if (!dependency_ids.insert(receipt.dependency_id).second) {
            diagnostics.add("P_ADMIT_DEPENDENCY", pointer,
                            "Dependency receipt id occurs more than once",
                            json{{"dependency_id", receipt.dependency_id}});
        }
        if (!contains_string(policy_digests, receipt.content_identity)) {
            diagnostics.add("P_ADMIT_DEPENDENCY", pointer,
                            "Dependency digest is not allowed by the policy snapshot",
                            json{{"dependency_id", receipt.dependency_id},
                                 {"content_identity", receipt.content_identity}});
        }
    }
    const auto recomputed_dependency_root = dependency_merkle_root(admission.dependency_receipts);
    if (recomputed_dependency_root != bundle.module_dependency_merkle_root()) {
        diagnostics.add("P_ADMIT_DEPENDENCY", "/module_dependency_merkle_root",
                        "Supplied dependency receipts do not match the bundle Merkle root",
                        json{{"bundle", bundle.module_dependency_merkle_root()},
                             {"recomputed", recomputed_dependency_root}});
    }

    // A Merkle root alone is not an authority proof. Re-open every pinned
    // module through the owner-qualified store view and verify the complete
    // transitive closure before Core materialization. Empty legacy imports
    // remain valid; any non-empty dependency claim requires the immutable
    // module store and an exact coordinate/receipt set.
    const auto& module_coordinates = bundle.module_coordinates();
    if (!module_coordinates.empty() || !admission.dependency_receipts.empty()) {
        if (!impl_->module_store) {
            diagnostics.add(
                "P_ADMIT_MODULE_STORE", "/module_coordinates",
                "Program admission with module dependencies requires an immutable ModuleStore");
        } else {
            std::set<std::pair<std::string, std::string>> coordinate_keys;
            std::map<std::string, std::string, std::less<>> expected_receipts;
            for (const auto& coordinate : module_coordinates) {
                const auto key = std::pair{coordinate.qualified_name(), coordinate.content_identity};
                if (!coordinate_keys.insert(key).second) continue;
                expected_receipts.emplace(coordinate.qualified_name(), coordinate.content_identity);
                const auto module = impl_->module_store->get(admission.owner_scope,
                                                              coordinate.content_identity);
                if (!module) {
                    diagnostics.add(
                        "P_ADMIT_DEPENDENCY", "/module_coordinates",
                        "Pinned module is unavailable in the admitting owner scope",
                        json{{"coordinate", coordinate.qualified_name()},
                             {"content_identity", coordinate.content_identity}});
                    continue;
                }
                if (module->coordinate() != coordinate || module->owner_scope() != admission.owner_scope) {
                    diagnostics.add(
                        "P_ADMIT_DEPENDENCY", "/module_coordinates",
                        "Pinned module coordinate or owner scope does not match the stored module",
                        json{{"coordinate", coordinate.qualified_name()},
                             {"content_identity", coordinate.content_identity}});
                }
            }

            std::map<std::string, std::string, std::less<>> actual_receipts;
            for (const auto& receipt : admission.dependency_receipts)
                actual_receipts.emplace(receipt.dependency_id, receipt.content_identity);
            if (actual_receipts != expected_receipts) {
                json expected_json = json::object();
                json actual_json   = json::object();
                for (const auto& [id, digest] : expected_receipts) expected_json[id] = digest;
                for (const auto& [id, digest] : actual_receipts) actual_json[id] = digest;
                diagnostics.add(
                    "P_ADMIT_DEPENDENCY", "/dependency_receipts",
                    "Dependency receipts must exactly cover the sealed module coordinates",
                    json{{"expected", std::move(expected_json)},
                         {"actual", std::move(actual_json)}});
            }

            try {
                const ModuleResolver resolver(impl_->module_store);
                for (const auto& coordinate : module_coordinates) {
                    const auto resolution = resolver.resolve(coordinate, admission.policy);
                    for (const auto& module : resolution.modules) {
                        const auto key =
                            std::pair{module.coordinate().qualified_name(), module.id()};
                        if (!coordinate_keys.contains(key)) {
                            diagnostics.add(
                                "P_ADMIT_DEPENDENCY", "/module_coordinates",
                                "Sealed module coordinates omit a transitive dependency",
                                json{{"coordinate", module.coordinate().qualified_name()},
                                     {"content_identity", module.id()}});
                        }
                    }
                }
            } catch (const std::exception& error) {
                diagnostics.add("P_ADMIT_DEPENDENCY", "/module_coordinates",
                                "Pinned module dependency closure failed closed",
                                json{{"error", error.what()}});
            }
        }
    }

    const auto input_contract  = bundle.input_contract();
    const auto output_contract = bundle.output_contract();
    try {
        validate_contract_schema(input_contract, "/input_contract/schema");
    } catch (const std::exception& error) {
        diagnostics.add("P_ADMIT_SCHEMA_UNSUPPORTED", "/input_contract/schema",
                        "Program input contract schema is invalid or unsupported",
                        json{{"error", error.what()}});
    }
    try {
        validate_contract_schema(output_contract, "/output_contract/schema");
    } catch (const std::exception& error) {
        diagnostics.add("P_ADMIT_SCHEMA_UNSUPPORTED", "/output_contract/schema",
                        "Program output contract schema is invalid or unsupported",
                        json{{"error", error.what()}});
    }

    validate_budgets(bundle, admission.policy, diagnostics);
    const auto concurrency = std::find_if(
        bundle.declared_budget_requirements().begin(),
        bundle.declared_budget_requirements().end(),
        [](const auto& requirement) { return requirement.resource == "max_concurrency"; });
    if (concurrency != bundle.declared_budget_requirements().end() &&
        impl_->worker_count > concurrency->maximum) {
        diagnostics.add("P_ADMIT_POLICY", "/declared_budget_requirements",
                        "Catalog worker_count exceeds the admitted concurrency bound",
                        json{{"worker_count", impl_->worker_count},
                             {"max_concurrency", concurrency->maximum}});
    }

    const auto                         definitions = bundle.sealed_core_definitions();
    const auto                         plan        = bundle.orchestration_plan();
    std::optional<graph::TopologySpec> topology;
    std::optional<ClosureResult>       closure;
    if (definitions.size() != 1) {
        diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/sealed_core_definitions",
                        "PR6 requires exactly one sealed call_core definition",
                        json{{"actual_count", definitions.size()}});
    } else {
        const auto& definition = definitions.front();
        if (!definition.definition.is_object() || !definition.definition.contains("name") ||
            !definition.definition["name"].is_string() ||
            definition.definition["name"].get<std::string>() != definition.name) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/sealed_core_definitions/0",
                            "Sealed Core envelope and definition names differ",
                            json{{"envelope_name", definition.name}});
        }
        if (sealed_core_definition_hash(definition.definition) != definition.definition_hash) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH",
                            "/sealed_core_definitions/0/definition_hash",
                            "Sealed Core definition hash differs from its exact content");
        }

        try {
            auto parse_report = detail::RegistrySnapshotAccess::parse_local_report(
                impl_->registry, definition.definition);
            map_core_parse_diagnostics(parse_report, diagnostics);
            if (parse_report.topology) {
                topology = std::move(*parse_report.topology);
                if (topology->name != definition.name ||
                    detail::canonical_json_bytes(topology->to_json()) !=
                        detail::canonical_json_bytes(definition.definition)) {
                    diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH",
                                    "/sealed_core_definitions/0/definition",
                                    "Local Core normalization differs from the sealed definition",
                                    json{{"envelope_name", definition.name},
                                         {"normalized_name", topology->name}});
                }
                map_roundtrip_diagnostics(detail::RegistrySnapshotAccess::verify_roundtrip_report(
                                              definition.definition, *topology),
                                          diagnostics);
                map_validation_diagnostics(
                    detail::RegistrySnapshotAccess::validate_local(impl_->registry, *topology),
                    diagnostics);
                closure = resolve_closure(impl_->registry, *topology, diagnostics);
            }
        } catch (const std::exception& error) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/sealed_core_definitions/0/definition",
                            "Sealed Core definition could not be recomputed locally",
                            json{{"error", error.what()}});
        }

        json plan_witness;
        if (!valid_orchestration_plan(plan, definition.name, plan_witness)) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/orchestration_plan",
                            "Bundle orchestration plan is malformed or unbound",
                            std::move(plan_witness));
        }
        const auto recomputed_program_hash = recompute_program_hash(bundle, plan, definition);
        if (recomputed_program_hash != bundle.canonical_program_hash()) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/canonical_program_hash",
                            "Bundle canonical Program hash differs from recomputed semantics",
                            json{{"bundle", bundle.canonical_program_hash()},
                                 {"recomputed", recomputed_program_hash}});
        }
    }

    if (closure) {
        if (closure->identities != bundle.executable_registry_identities() ||
            closure->closure != bundle.capability_effect_closure()) {
            diagnostics.add(
                "P_ADMIT_SEMANTIC_MISMATCH", "/executable_registry_identities",
                "Bundle executable/capability/effect closure is not exact",
                json{{"recomputed_executable_count", closure->identities.size()},
                     {"bundle_executable_count", bundle.executable_registry_identities().size()}});
        }

        const auto profile_executables = admission.profile.allowed_executables();
        const auto profile_modes       = admission.profile.allowed_effect_modes();
        const auto policy_capabilities = admission.policy.allowed_capabilities();
        const auto policy_effects      = admission.policy.allowed_effects();
        for (const auto& identity : closure->identities) {
            if (!contains_identity(profile_executables, identity)) {
                diagnostics.add("P_ADMIT_POLICY", "/executable_registry_identities",
                                "Exact executable identity is denied by the admission profile",
                                identity_json(identity));
            }
        }
        for (const auto mode : closure->modes) {
            if (!contains_effect_mode(profile_modes, mode)) {
                diagnostics.add("P_ADMIT_POLICY", "/executable_registry_identities",
                                "Executable effect mode is denied by the admission profile",
                                json{{"effect_mode", std::string(to_string(mode))}});
            }
            if (mode == EffectMode::TrustedNative) {
                if (admission.profile.mode() != AdmissionMode::TrustedEmbedding) {
                    diagnostics.add(
                        "P_ADMIT_TRUSTED_NATIVE", "/executable_registry_identities",
                        "TrustedNative executables require TrustedEmbedding admission",
                        json{{"admission_mode", std::string(to_string(admission.profile.mode()))}});
                }
                if (impl_->host_identity.empty()) {
                    diagnostics.add(
                        "P_ADMIT_HOST_IDENTITY", "/executable_registry_identities",
                        "TrustedNative admission requires an authenticated Catalog host identity");
                }
            }
        }
        if (!impl_->host_identity.empty()) {
            for (const auto& identity : closure->identities) {
                const auto manifest = impl_->registry.find(identity.kind, identity.name);
                if (!manifest || manifest->identity != identity) continue;
                if (manifest->effect_mode == EffectMode::TrustedNative &&
                    manifest->attestation_id != impl_->host_identity) {
                    diagnostics.add(
                        "P_ADMIT_HOST_IDENTITY", "/executable_registry_identities",
                        "TrustedNative executable attestation does not match the Catalog host",
                        json{{"executable", identity_json(identity)},
                             {"attestation_id", manifest->attestation_id},
                             {"host_identity", impl_->host_identity}});
                }
            }
        }
        for (const auto& capability : closure->closure.capabilities) {
            if (!contains_string(policy_capabilities, capability)) {
                diagnostics.add("P_ADMIT_POLICY", "/capability_effect_closure/capabilities",
                                "Required capability is denied by the policy snapshot",
                                json{{"capability", capability}});
            }
        }
        for (const auto& effect : closure->closure.effects) {
            if (!contains_string(policy_effects, effect)) {
                diagnostics.add("P_ADMIT_POLICY", "/capability_effect_closure/effects",
                                "Required effect is denied by the policy snapshot",
                                json{{"effect", effect}});
            }
        }
    }

    std::optional<CorePlanIdentity> recomputed_plan;
    if (definitions.size() == 1 && closure) {
        recomputed_plan = CorePlanIdentity{
            definitions.front().name,
            core_compiled_plan_identity(definitions.front(), impl_->compiler_build_id,
                                        registry_fingerprint, closure->identities)};
        if (bundle.core_plan_identities() != std::vector<CorePlanIdentity>{*recomputed_plan}) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/core_plan_identities",
                            "Stored Core plan identity differs from exact recomputation",
                            json{{"recomputed", recomputed_plan->compiled_plan_identity}});
        }
    }

    if (!diagnostics.empty()) diagnostics.throw_error();

    const auto requested_bindings = runtime_binding_identities(closure->identities);
    CatalogCapabilityBinding binding;
    const bool isolate_binding = supplied_binding.has_value();
    if (supplied_binding) {
        binding = std::move(*supplied_binding);
        validate_capability_binding(requested_bindings, binding, diagnostics);
    } else if (!requested_bindings.empty()) {
        if (!impl_->capability_binder) {
            diagnostics.add("P_BINDING_MISSING", "/capability_bindings",
                            "Catalog has no capability binder for the runtime-bound closure");
        } else {
            try {
                binding = impl_->capability_binder(requested_bindings);
                validate_capability_binding(requested_bindings, binding, diagnostics);
            } catch (const ProgramAdmissionError&) {
                throw;
            } catch (const std::exception& error) {
                diagnostics.add("P_BINDING_FACTORY", "/capability_bindings",
                                "Catalog capability binder failed",
                                json{{"error", error.what()}});
            } catch (...) {
                diagnostics.add("P_BINDING_FACTORY", "/capability_bindings",
                                "Catalog capability binder failed with a non-standard exception");
            }
        }
    }
    if (!diagnostics.empty()) diagnostics.throw_error();

    CoreMaterializationReceipt receipt{impl_->compiler_build_id,
                                           registry_fingerprint,
                                           {*recomputed_plan},
                                           binding.receipts};
    ProgramVersion version = [&]() {
        try {
            return ProgramVersion(
                ProgramVersionData{bundle.id(), admission.profile, admission.policy,
                                   admission.dependency_receipts, admission.owner_scope, receipt});
        } catch (const std::exception& error) {
            AdmissionDiagnostics invalid(bundle.id());
            invalid.add("P_ADMIT_BINDING", "/admission",
                        "Admission values cannot form an exact ProgramVersion",
                        json{{"error", error.what()}});
            invalid.throw_error();
        }
    }();
    if (expected_version &&
        expected_version->serialize_canonical() != version.serialize_canonical()) {
        AdmissionDiagnostics mismatch(bundle.id());
        mismatch.add("P_RESOLVE_BINDING", "/core_materialization_receipt",
                     "Stored ProgramVersion does not match the current exact materialization",
                     json{{"stored_version_id", expected_version->id()},
                          {"resolved_version_id", version.id()}});
        mismatch.throw_error();
    }

    std::lock_guard catalog_lock(impl_->mutex);
    const auto      existing = impl_->materialized.find(version.id());
    if (!isolate_binding && existing != impl_->materialized.end()) {
        if (existing->second->version.serialize_canonical() != version.serialize_canonical() ||
            existing->second->bundle.serialize_canonical() != bundle.serialize_canonical()) {
            AdmissionDiagnostics collision(bundle.id());
            collision.add("P_ADMIT_SEMANTIC_MISMATCH", "/id",
                          "Catalog identity collision has different canonical bytes");
            collision.throw_error();
        }

        // Retention may remove an immutable tuple while this process still
        // holds its materialized generation for already-pinned runs.  A later
        // equivalent admission must restore the durable authority record
        // before returning the cached generation; otherwise the caller sees a
        // successful admission that cannot be recovered after restart.
        const auto stored_version =
            impl_->program_store->get_version(version.ownership_scope(), version.id());
        auto stored_bundle =
            impl_->program_store->get_bundle(version.ownership_scope(), bundle.id());
        if (!stored_bundle) stored_bundle = impl_->program_store->get_bundle(bundle.id());
        if (!stored_version || !stored_bundle ||
            stored_version->serialize_canonical() != version.serialize_canonical() ||
            stored_bundle->serialize_canonical() != bundle.serialize_canonical()) {
            try {
                impl_->program_store->publish_admitted(bundle, version);
            } catch (const std::exception& error) {
                AdmissionDiagnostics publication(bundle.id());
                publication.add("P_ADMIT_BINDING", "/id",
                                "Cached admission could not restore its durable Program tuple",
                                json{{"error", error.what()}});
                publication.throw_error();
            } catch (...) {
                AdmissionDiagnostics publication(bundle.id());
                publication.add(
                    "P_ADMIT_BINDING", "/id",
                    "Cached admission could not restore its durable Program tuple");
                publication.throw_error();
            }
        }
        return existing->second->version;
    }

    const auto binding_root =
        capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings);
    EngineGenerationCache::Impl::Key key{
        bundle.id(), recomputed_plan->name, recomputed_plan->compiled_plan_identity,
        registry_fingerprint, impl_->compiler_build_id, binding_root, impl_->worker_count};
    auto&                                               cache = *impl_->engines->impl_;
    std::lock_guard                                     cache_lock(cache.mutex);
    auto                                                cached = cache.generations.find(key);
    std::shared_ptr<const detail::PinnedCoreGeneration> generation;
    bool                                                inserted_generation = false;
    if (!isolate_binding && cached != cache.generations.end()) {
        generation = cached->second;
    } else {
        try {
            graph::NodeContext context = std::move(binding.node_context);
            auto compiled =
                detail::RegistrySnapshotAccess::link_local(impl_->registry, *topology, context);

            AdmissionDiagnostics linked(bundle.id());
            const auto           actual_definition = compiled.to_json();
            auto actual_closure = resolve_closure(impl_->registry, compiled.topology(), linked);
            const auto actual_sealed = SealedCoreDefinition{
                definitions.front().name, sealed_core_definition_hash(actual_definition),
                actual_definition};
            const auto actual_identity =
                core_compiled_plan_identity(actual_sealed, impl_->compiler_build_id,
                                            registry_fingerprint, actual_closure.identities);
            if (!linked.empty() ||
                detail::canonical_json_bytes(actual_definition) !=
                    detail::canonical_json_bytes(definitions.front().definition) ||
                actual_closure.identities != closure->identities ||
                actual_closure.closure != closure->closure ||
                actual_identity != recomputed_plan->compiled_plan_identity) {
                linked.add("P_ADMIT_SEMANTIC_MISMATCH", "/core_plan_identities",
                           "Factory-linked topology does not preserve the admitted exact plan");
                linked.throw_error();
            }

            auto provider = context.provider;
            auto runtime_registry =
                detail::RegistrySnapshotAccess::runtime_registry(impl_->registry);
            graph::EngineConfig config;
            config.worker_count = impl_->worker_count;
            graph::EngineResources resources;
            resources.registry = runtime_registry;
            resources.tools    = std::move(binding.tools);
            auto unique_engine = graph::GraphEngine::link(std::move(compiled), std::move(config),
                                                          std::move(resources));
            std::shared_ptr<graph::GraphEngine> engine(std::move(unique_engine));
            generation =
                std::make_shared<detail::PinnedCoreGeneration>(detail::PinnedCoreGeneration{
                    impl_->registry, std::move(runtime_registry), recomputed_plan->name,
                    recomputed_plan->compiled_plan_identity, std::move(provider),
                    std::move(engine)});
            if (!isolate_binding) {
                cache.generations.emplace(key, generation);
                inserted_generation = true;
            }
        } catch (const ProgramAdmissionError&) {
            throw;
        } catch (const std::exception& error) {
            AdmissionDiagnostics factory(bundle.id());
            factory.add("P_MATERIALIZE_FACTORY", "/sealed_core_definitions/0/definition",
                        "Local factory construction or engine linking failed",
                        json{{"error", error.what()}});
            factory.throw_error();
        }
    }

    auto materialized = std::make_shared<detail::MaterializedProgram>(
        detail::MaterializedProgram{bundle, version, generation});
    if (isolate_binding) {
        if (isolated_materialization) *isolated_materialization = materialized;
    } else {
        impl_->materialized.emplace(version.id(), materialized);
    }
    if (publish) {
        try {
            impl_->program_store->publish_admitted(bundle, version);
        } catch (const std::exception& error) {
            impl_->materialized.erase(version.id());
            if (inserted_generation) cache.generations.erase(key);
            AdmissionDiagnostics publication(bundle.id());
            publication.add("P_ADMIT_BINDING", "/id",
                            "Atomic ProgramStore publication rejected the admitted tuple",
                            json{{"error", error.what()}});
            publication.throw_error();
        } catch (...) {
            impl_->materialized.erase(version.id());
            if (inserted_generation) cache.generations.erase(key);
            AdmissionDiagnostics publication(bundle.id());
            publication.add(
                "P_ADMIT_BINDING", "/id",
                "Atomic ProgramStore publication failed with a non-standard exception");
            publication.throw_error();
        }
    }
    return version;
}
MigrationPlan ProgramCatalog::plan_migration(std::string_view owner_scope,
                                             std::string_view source_version_id,
                                             std::string_view target_version_id) const {
    detail::validate_token(owner_scope, "Program migration owner_scope");
    const auto source = impl_->program_store->get_version(owner_scope, source_version_id);
    const auto target = impl_->program_store->get_version(owner_scope, target_version_id);
    if (!source || !target)
        throw std::invalid_argument("Program migration references an unpublished version");
    if (source->ownership_scope() != owner_scope || target->ownership_scope() != owner_scope)
        throw std::invalid_argument("Program migration crosses an owner scope boundary");
    const auto source_bundle =
        impl_->program_store->get_bundle(owner_scope, source->bundle_id());
    const auto target_bundle =
        impl_->program_store->get_bundle(owner_scope, target->bundle_id());
    if (!source_bundle || !target_bundle)
        throw std::invalid_argument(
            "Program migration references an unpublished admitted bundle");
    return MigrationPlan::between(*source, *source_bundle, *target, *target_bundle);
}
ProgramActivationResult ProgramCatalog::activate(std::string_view owner_scope,
                                                 std::string_view version_id,
                                                 std::uint64_t expected_generation) {
    const auto version = resolve_version(owner_scope, version_id);
    if (!version)
        throw std::invalid_argument("Program activation references an unknown owner-scoped version");
    return impl_->program_store->compare_activate(owner_scope, expected_generation, version->id(),
                                                  version->policy_snapshot().fingerprint());
}

ProgramActivationResult ProgramCatalog::rollback(std::string_view owner_scope,
                                                 std::string_view version_id,
                                                 std::uint64_t expected_generation) {
    // Rollback is deliberately the same owner-scoped activation CAS: an older
    // immutable version is published for future resolutions while existing
    // runs retain their already pinned materialization.
    return activate(owner_scope, version_id, expected_generation);
}

std::optional<ProgramActivation>
ProgramCatalog::activation(std::string_view owner_scope) const {
    detail::validate_token(owner_scope, "Program activation owner_scope");
    return impl_->program_store->get_activation(owner_scope);
}

ProgramRetentionReport ProgramCatalog::collect_retention(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    detail::validate_token(owner_scope, "Program retention owner_scope");
    for (const auto& id : pinned_version_ids) {
        const auto version = impl_->program_store->get_version(owner_scope, id);
        if (!version || version->ownership_scope() != owner_scope)
            throw std::invalid_argument("Program retention pin crosses an owner scope boundary");
    }
    return impl_->program_store->collect_garbage(owner_scope, pinned_version_ids);
}


std::optional<ProgramVersion> ProgramCatalog::resolve_version(
    std::string_view owner_scope, std::string_view id) {
    return resolve_version_impl(owner_scope, id, std::nullopt);
}

std::optional<ProgramVersion> ProgramCatalog::resolve_version_with_binding(
    std::string_view owner_scope, std::string_view id, CatalogCapabilityBinding binding) {
    return resolve_version_impl(owner_scope, id, std::move(binding));
}

std::optional<ProgramVersion> ProgramCatalog::resolve_version_impl(
    std::string_view owner_scope,
    std::string_view id,
    std::optional<CatalogCapabilityBinding> supplied_binding,
    std::shared_ptr<const detail::MaterializedProgram>* isolated_materialization) {
    detail::validate_token(owner_scope, "Program resolve owner_scope");
    const auto stored_value = impl_->program_store->get_version(owner_scope, id);
    if (!stored_value) return std::nullopt;

    ProgramVersion stored = [&]() {
        try {
            return ProgramVersion::parse(stored_value->serialize_canonical());
        } catch (const std::exception& error) {
            AdmissionDiagnostics diagnostics("catalog");
            diagnostics.add("P_RESOLVE_STORE", "/program_version_id",
                            "Stored ProgramVersion failed canonical verification",
                            json{{"error", error.what()}});
            diagnostics.throw_error();
        }
    }();
    if (stored.ownership_scope() != owner_scope) return std::nullopt;

    // The version is the owner-bearing authority record. A bounded adapter
    // may expose only its one immutable bundle; after the exact owner-
    // qualified version has been established, that hash-addressed bundle is
    // safe to load through the legacy view as a compatibility fallback.
    auto bundle_value = impl_->program_store->get_bundle(owner_scope, stored.bundle_id());
    if (!bundle_value) bundle_value = impl_->program_store->get_bundle(stored.bundle_id());
    if (!bundle_value) {
        AdmissionDiagnostics diagnostics(stored.id());
        diagnostics.add("P_RESOLVE_STORE", "/bundle_id",
                        "Stored ProgramVersion references a missing ProgramBundle",
                        json{{"bundle_id", stored.bundle_id()}});
        diagnostics.throw_error();
    }
    ProgramBundle bundle = [&]() {
        try {
            return ProgramBundle::parse(bundle_value->serialize_canonical());
        } catch (const std::exception& error) {
            AdmissionDiagnostics diagnostics(stored.id());
            diagnostics.add("P_RESOLVE_STORE", "/bundle_id",
                            "Stored ProgramBundle failed canonical verification",
                            json{{"error", error.what()}});
            diagnostics.throw_error();
        }
    }();
    if (!supplied_binding) {
        std::lock_guard lock(impl_->mutex);
        const auto known = impl_->materialized.find(stored.id());
        if (known != impl_->materialized.end() &&
            known->second->version.serialize_canonical() == stored.serialize_canonical() &&
            known->second->bundle.serialize_canonical() == bundle.serialize_canonical()) {
            return known->second->version;
        }
    }

    ProgramAdmission admission{stored.ownership_scope(),
                               stored.admission_profile(),
                               stored.policy_snapshot(),
                               stored.dependency_receipts()};
    return materialize(bundle, std::move(admission), std::move(supplied_binding), &stored, false,
                       isolated_materialization);
}


std::optional<ProgramVersion> ProgramCatalog::find_version(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      known = impl_->materialized.find(std::string(id));
    if (known == impl_->materialized.end()) return std::nullopt;
    const auto stored = impl_->program_store->get_version(known->second->version.ownership_scope(), id);
    if (!stored || stored->serialize_canonical() != known->second->version.serialize_canonical()) {
        return std::nullopt;
    }
    return known->second->version;
}

std::optional<ProgramVersion> ProgramCatalog::find_version(std::string_view owner_scope,
                                                            std::string_view id) const {
    detail::validate_token(owner_scope, "Program find owner_scope");
    std::lock_guard lock(impl_->mutex);
    const auto      known = impl_->materialized.find(std::string(id));
    if (known == impl_->materialized.end() ||
        known->second->version.ownership_scope() != owner_scope)
        return std::nullopt;
    const auto stored = impl_->program_store->get_version(owner_scope, id);
    if (!stored || stored->serialize_canonical() != known->second->version.serialize_canonical())
        return std::nullopt;
    return known->second->version;
}

std::shared_ptr<const detail::MaterializedProgram> detail::CatalogRuntimeAccess::pin(
    const ProgramCatalog& catalog, const ProgramVersion& version) {
    std::lock_guard lock(catalog.impl_->mutex);
    const auto      found = catalog.impl_->materialized.find(version.id());
    if (found == catalog.impl_->materialized.end() ||
        found->second->version.serialize_canonical() != version.serialize_canonical()) {
        throw ProgramDiagnosticError(start_not_admitted(version.id()));
    }
    const auto stored_version = catalog.impl_->program_store->get_version(
        version.ownership_scope(), version.id());
    auto stored_bundle = catalog.impl_->program_store->get_bundle(
        version.ownership_scope(), found->second->bundle.id());
    if (!stored_bundle)
        stored_bundle = catalog.impl_->program_store->get_bundle(found->second->bundle.id());
    if (!stored_version || !stored_bundle ||
        stored_version->serialize_canonical() != found->second->version.serialize_canonical() ||
        stored_bundle->serialize_canonical() != found->second->bundle.serialize_canonical()) {
        throw ProgramDiagnosticError(start_not_admitted(version.id()));
    }
    return found->second;
}
std::shared_ptr<const detail::MaterializedProgram>
detail::CatalogRuntimeAccess::pin_with_binding(ProgramCatalog&            catalog,
                                               std::string_view           owner_scope,
                                               std::string_view           version_id,
                                               CatalogCapabilityBinding   binding) {
    std::shared_ptr<const detail::MaterializedProgram> isolated;
    const auto resolved = catalog.resolve_version_impl(
        owner_scope, version_id, std::move(binding), &isolated);
    if (!resolved || !isolated || isolated->version.id() != version_id) {
        throw ProgramDiagnosticError(start_not_admitted(version_id));
    }
    return isolated;
}

}  // namespace neograph::program
