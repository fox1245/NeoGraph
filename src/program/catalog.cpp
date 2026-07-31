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
    const auto&                                     budgets = bundle.declared_budget_requirements();
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
        if (resource == "max_program_operations") {
            structural = budget.minimum == 1 && budget.maximum == 1;
        } else if (resource == "max_concurrency" || resource == "max_core_steps") {
            structural = budget.minimum >= 1;
        } else if (resource == "max_dynamic_compiles" || resource == "max_child_depth" ||
                   resource == "max_total_children") {
            structural = budget.minimum == 0 && budget.maximum == 0;
        }
        if (!structural) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/declared_budget_requirements",
                            "Budget record violates the one-operation Program-v1 closure",
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

        bool operator<(const Key& other) const noexcept {
            return std::tie(bundle_id, core_name, compiled_plan_identity, registry_fingerprint,
                            compiler_build_id) <
                   std::tie(other.bundle_id, other.core_name, other.compiled_plan_identity,
                            other.registry_fingerprint, other.compiler_build_id);
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
         std::string                            build_id)
        : program_store(std::move(store)),
          registry(std::move(registry_snapshot)),
          engines(std::move(engine_cache)),
          compiler_build_id(std::move(build_id)) {}

    std::shared_ptr<ProgramStore>          program_store;
    RegistrySnapshot                       registry;
    std::shared_ptr<EngineGenerationCache> engines;
    std::string                            compiler_build_id;

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

    {
        std::lock_guard lock(config.engines->impl_->mutex);
        const auto      wanted = std::pair{config.registry.fingerprint(), config.compiler_build_id};
        if (config.engines->impl_->scope && *config.engines->impl_->scope != wanted) {
            throw std::invalid_argument(
                "EngineGenerationCache is already scoped to another registry/compiler identity");
        }
        config.engines->impl_->scope = wanted;
    }

    impl_ = std::make_unique<Impl>(std::move(config.program_store), std::move(config.registry),
                                   std::move(config.engines), std::move(config.compiler_build_id));
}

ProgramCatalog::ProgramCatalog(ProgramCatalog&&) noexcept            = default;
ProgramCatalog& ProgramCatalog::operator=(ProgramCatalog&&) noexcept = default;
ProgramCatalog::~ProgramCatalog()                                    = default;

ProgramVersion ProgramCatalog::admit(const ProgramBundle& input_bundle,
                                     ProgramAdmission     admission) {
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

    const auto input_contract  = bundle.input_contract();
    const auto output_contract = bundle.output_contract();
    if (input_contract.schema_version != 1 || output_contract.schema_version != 1 ||
        input_contract.schema != json::object() || output_contract.schema != json::object()) {
        diagnostics.add("P_ADMIT_SCHEMA_UNSUPPORTED", "/input_contract",
                        "PR6 supports only schema-version-1 empty input and output contracts",
                        json{{"input_schema_version", input_contract.schema_version},
                             {"output_schema_version", output_contract.schema_version},
                             {"input_schema", input_contract.schema},
                             {"output_schema", output_contract.schema}});
    }

    validate_budgets(bundle, admission.policy, diagnostics);

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

        const json expected_plan{
            {"root", "root"},
            {"operations",
             json::array({json{{"id", "root"}, {"op", "call_core"}, {"core", definition.name}}})}};
        if (plan.schema_version != 1 || detail::canonical_json_bytes(plan.plan) !=
                                            detail::canonical_json_bytes(expected_plan)) {
            diagnostics.add("P_ADMIT_SEMANTIC_MISMATCH", "/orchestration_plan",
                            "Bundle does not contain the canonical one-call_core plan",
                            json{{"expected", expected_plan}, {"actual", plan.plan}});
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
        bool       runtime_unsupported = false;
        for (const auto& identity : closure->identities) {
            if (!contains_identity(profile_executables, identity)) {
                diagnostics.add("P_ADMIT_POLICY", "/executable_registry_identities",
                                "Exact executable identity is denied by the admission profile",
                                identity_json(identity));
            }
            if (identity.kind == ExecutableKind::Provider ||
                identity.kind == ExecutableKind::Tool ||
                identity.kind == ExecutableKind::Imported) {
                runtime_unsupported = true;
            }
        }
        for (const auto mode : closure->modes) {
            if (!contains_effect_mode(profile_modes, mode)) {
                diagnostics.add("P_ADMIT_POLICY", "/executable_registry_identities",
                                "Executable effect mode is denied by the admission profile",
                                json{{"effect_mode", std::string(to_string(mode))}});
            }
            if (mode == EffectMode::TrustedNative) runtime_unsupported = true;
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
        if (!closure->closure.capabilities.empty() || !closure->closure.effects.empty()) {
            runtime_unsupported = true;
        }
        if (runtime_unsupported) {
            diagnostics.add("P_ADMIT_RUNTIME_UNSUPPORTED", "/capability_effect_closure",
                            "PR6 materializes only pure local node/reducer/condition closures");
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

    CoreMaterializationReceipt receipt{
        impl_->compiler_build_id, registry_fingerprint, {*recomputed_plan}};
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

    std::lock_guard catalog_lock(impl_->mutex);
    const auto      existing = impl_->materialized.find(version.id());
    if (existing != impl_->materialized.end()) {
        if (existing->second->version.serialize_canonical() != version.serialize_canonical() ||
            existing->second->bundle.serialize_canonical() != bundle.serialize_canonical()) {
            AdmissionDiagnostics collision(bundle.id());
            collision.add("P_ADMIT_SEMANTIC_MISMATCH", "/id",
                          "Catalog identity collision has different canonical bytes");
            collision.throw_error();
        }
        return existing->second->version;
    }

    EngineGenerationCache::Impl::Key                    key{bundle.id(), recomputed_plan->name,
                                         recomputed_plan->compiled_plan_identity,
                                         registry_fingerprint, impl_->compiler_build_id};
    auto&                                               cache = *impl_->engines->impl_;
    std::lock_guard                                     cache_lock(cache.mutex);
    auto                                                cached = cache.generations.find(key);
    std::shared_ptr<const detail::PinnedCoreGeneration> generation;
    bool                                                inserted_generation = false;
    if (cached != cache.generations.end()) {
        generation = cached->second;
    } else {
        try {
            graph::NodeContext context;
            auto               compiled =
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

            auto runtime_registry =
                detail::RegistrySnapshotAccess::runtime_registry(impl_->registry);
            graph::EngineConfig config;
            config.worker_count = 1;
            graph::EngineResources resources;
            resources.registry = runtime_registry;
            auto unique_engine = graph::GraphEngine::link(std::move(compiled), std::move(config),
                                                          std::move(resources));
            std::shared_ptr<graph::GraphEngine> engine(std::move(unique_engine));
            generation =
                std::make_shared<detail::PinnedCoreGeneration>(detail::PinnedCoreGeneration{
                    impl_->registry, std::move(runtime_registry), recomputed_plan->name,
                    recomputed_plan->compiled_plan_identity, std::move(engine)});
            cache.generations.emplace(key, generation);
            inserted_generation = true;
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
    impl_->materialized.emplace(version.id(), materialized);
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
        publication.add("P_ADMIT_BINDING", "/id",
                        "Atomic ProgramStore publication failed with a non-standard exception");
        publication.throw_error();
    }
    return version;
}

std::optional<ProgramVersion> ProgramCatalog::find_version(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto      known = impl_->materialized.find(std::string(id));
    if (known == impl_->materialized.end()) return std::nullopt;
    const auto stored = impl_->program_store->get_version(id);
    if (!stored || stored->serialize_canonical() != known->second->version.serialize_canonical()) {
        return std::nullopt;
    }
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
    const auto stored_version = catalog.impl_->program_store->get_version(version.id());
    const auto stored_bundle = catalog.impl_->program_store->get_bundle(found->second->bundle.id());
    if (!stored_version || !stored_bundle ||
        stored_version->serialize_canonical() != found->second->version.serialize_canonical() ||
        stored_bundle->serialize_canonical() != found->second->bundle.serialize_canonical()) {
        throw ProgramDiagnosticError(start_not_admitted(version.id()));
    }
    return found->second;
}

}  // namespace neograph::program
