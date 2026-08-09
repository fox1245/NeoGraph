#include <neograph/mcp/harness_program_translator.h>
#include <neograph/mcp/json_schema.h>

#include "../program/canonical_json.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace neograph::mcp {
namespace {

constexpr std::string_view                kResultChannel   = "final_result";
constexpr std::array<std::string_view, 9> kBudgetResources = {
    "wall_time_ms",         "model_tokens",           "monetary_microunits",
    "max_concurrency",      "max_program_operations", "max_core_steps",
    "max_dynamic_compiles", "max_child_depth",        "max_total_children",
};

constexpr std::string_view kInvocationTemplateFormat = "neograph-harness-invocation-template";

json template_budget_json(const program::RunBudget& budget) {
    return {{"wall_time_ms", budget.wall_time_ms},
            {"model_tokens", budget.model_tokens},
            {"monetary_microunits", budget.monetary_microunits},
            {"max_concurrency", budget.max_concurrency},
            {"max_program_operations", budget.max_program_operations},
            {"max_core_steps", budget.max_core_steps},
            {"max_dynamic_compiles", budget.max_dynamic_compiles},
            {"max_child_depth", budget.max_child_depth},
            {"max_total_children", budget.max_total_children}};
}

std::uint64_t required_template_u64(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key)) {
        throw std::invalid_argument("Harness invocation template requires field '" + key + "'");
    }
    const auto& encoded = value.at(key);
    if (encoded.is_number_unsigned()) return encoded.get<std::uint64_t>();
    if (encoded.is_number_integer()) {
        const auto signed_value = encoded.get<std::int64_t>();
        if (signed_value >= 0) return static_cast<std::uint64_t>(signed_value);
    }
    throw std::invalid_argument("Harness invocation template field '" + key +
                                "' must be an unsigned integer");
}

std::uint32_t required_template_u32(const json& value, std::string_view field) {
    const auto encoded = required_template_u64(value, field);
    if (encoded > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Harness invocation template field '" + std::string(field) +
                                    "' exceeds uint32");
    }
    return static_cast<std::uint32_t>(encoded);
}

program::RunBudget template_budget_from_json(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Harness invocation template budget must be an object");
    }
    program::detail::reject_unknown_fields(
        value, "Harness invocation template budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    return {required_template_u64(value, "wall_time_ms"),
            required_template_u64(value, "model_tokens"),
            required_template_u64(value, "monetary_microunits"),
            required_template_u32(value, "max_concurrency"),
            required_template_u64(value, "max_program_operations"),
            required_template_u64(value, "max_core_steps"),
            required_template_u64(value, "max_dynamic_compiles"),
            required_template_u32(value, "max_child_depth"),
            required_template_u64(value, "max_total_children")};
}

json invocation_template_body(const HarnessInvocationTemplate& request) {
    (void)program::detail::canonical_json_bytes(request.input);
    return {{"format", std::string(kInvocationTemplateFormat)},
            {"storage_schema_version", HarnessInvocationTemplate::STORAGE_SCHEMA_VERSION},
            {"input", request.input},
            {"budget", template_budget_json(request.budget)}};
}

HarnessInvocationTemplate invocation_template_from_json(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Harness invocation template must be an object");
    }
    program::detail::reject_unknown_fields(value, "Harness invocation template",
                                           {"format", "storage_schema_version", "input", "budget"});
    if (!value.contains("format") || !value.at("format").is_string() ||
        value.at("format").get<std::string>() != kInvocationTemplateFormat ||
        required_template_u32(value, "storage_schema_version") !=
            HarnessInvocationTemplate::STORAGE_SCHEMA_VERSION ||
        !value.contains("input") || !value.contains("budget")) {
        throw std::invalid_argument("Harness invocation template format or schema is unsupported");
    }
    HarnessInvocationTemplate request{value.at("input"),
                                      template_budget_from_json(value.at("budget"))};
    (void)invocation_template_body(request);
    return request;
}

[[noreturn]] void fail(std::string code, std::string pointer, std::string message) {
    throw HarnessTranslationError(std::move(code), std::move(pointer), std::move(message));
}

std::string escape_pointer(std::string_view value) {
    std::string result;
    for (const char character : value) {
        if (character == '~')
            result += "~0";
        else if (character == '/')
            result += "~1";
        else
            result.push_back(character);
    }
    return result;
}

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs, std::string_view pointer) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
        fail("H_BUDGET_OVERFLOW", std::string(pointer), "Harness total budget overflows uint64");
    return lhs + rhs;
}

std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs, std::string_view pointer) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
        fail("H_BUDGET_OVERFLOW", std::string(pointer), "Harness total budget overflows uint64");
    return lhs * rhs;
}

std::uint64_t unsigned_value(const json& value, std::string_view pointer) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer())
        fail("H_REQUEST_SCHEMA", std::string(pointer), "Harness budget must be an integer");
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0)
        fail("H_BUDGET_FINITE", std::string(pointer),
             "Harness budget must be finite and nonnegative");
    return static_cast<std::uint64_t>(signed_value);
}

std::uint64_t effective_budget(const json&   request,
                               const char*   name,
                               std::uint64_t fallback,
                               std::uint64_t minimum,
                               std::uint64_t maximum) {
    const auto budgets = request.value("budgets", json::object());
    const auto value   = budgets.contains(name)
                             ? unsigned_value(budgets.at(name), std::string("/budgets/") + name)
                             : fallback;
    if (value < minimum || value > maximum) {
        fail("H_BUDGET_FINITE", std::string("/budgets/") + name,
             std::string(name) + " is outside the finite host-authorized range");
    }
    return value;
}

void validate_defaults(const HarnessTranslationDefaults& defaults) {
    if (defaults.timeout_seconds == 0 || defaults.timeout_seconds > 86400 ||
        defaults.max_parallel_workers == 0 || defaults.max_parallel_workers > 64 ||
        defaults.max_core_steps == 0 || defaults.max_core_steps > 1000 ||
        defaults.max_worker_retries > 5 || defaults.provider_timeout_seconds == 0 ||
        defaults.provider_timeout_seconds > 600 || defaults.max_output_tokens == 0 ||
        defaults.max_output_tokens > 128000 || defaults.input_token_ceiling_per_round == 0 ||
        defaults.input_token_ceiling_per_round == std::numeric_limits<std::uint64_t>::max() ||
        defaults.max_provider_tool_rounds == 0 || defaults.max_provider_tool_rounds > 64 ||
        defaults.max_provider_tool_rounds == std::numeric_limits<std::uint32_t>::max() ||
        defaults.monetary_microunits == 0 ||
        defaults.monetary_microunits == std::numeric_limits<std::uint64_t>::max() ||
        defaults.source_id_prefix.empty()) {
        fail("H_BUDGET_FINITE", "/defaults",
             "Harness translation defaults must be explicit, finite, and within v1 limits");
    }
}

std::optional<program::ContractManifest> requested_contract(const json& request) {
    const bool has_contract = request.contains("contract");
    const bool has_manifest = request.contains("contract_manifest");
    if (has_contract && has_manifest)
        fail("H_CONTRACT_DUPLICATE", "/contract",
             "Harness request must provide only one frozen contract manifest");
    if (!has_contract && !has_manifest) return std::nullopt;

    const auto pointer = has_contract ? "/contract" : "/contract_manifest";
    auto       value   = request.at(has_contract ? "contract" : "contract_manifest");
    if (value.is_string()) {
        try {
            value = json::parse(value.get<std::string>());
        } catch (const std::exception& error) {
            fail("H_CONTRACT_SCHEMA", pointer,
                 std::string("Harness contract manifest JSON is invalid: ") + error.what());
        }
    }
    // Accept the direct canonical ContractManifest object as the public form,
    // while tolerating a {"manifest": ...} envelope used by older clients.
    if (value.is_object() && value.contains("manifest") &&
        !value.contains("storage_schema_version")) {
        value = value.at("manifest");
    }
    if (!value.is_object())
        fail("H_CONTRACT_SCHEMA", pointer, "Harness contract manifest must be an object");
    try {
        auto manifest =
            program::ContractManifest::parse(program::detail::canonical_json_bytes(value));
        if (manifest.lifecycle() != program::ContractManifestLifecycle::Frozen)
            fail("H_CONTRACT_NOT_FROZEN", pointer,
                 "Harness execution requires an approved frozen contract manifest");
        return manifest;
    } catch (const HarnessTranslationError&) {
        throw;
    } catch (const std::exception& error) {
        fail("H_CONTRACT_INVALID", pointer,
             std::string("Harness contract manifest is invalid: ") + error.what());
    }
}

json input_contract_schema() {
    return {
        {"type", "object"},
        {"required", json::array({"task"})},
        {"properties",
         {{"task",
           {{"type", "object"},
            {"required", json::array({"objective"})},
            {"properties",
             {{"objective", {{"type", "string"}}},
              {"acceptance", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
            {"additionalProperties", true}}}}},
        {"additionalProperties", false},
    };
}

json final_output_schema() {
    return {
        {"type", "object"},
        {"required", json::array({"outcome", "workers", "findings", "finding_sources",
                                  "valid_workers", "failed_workers"})},
        {"properties",
         {{"outcome", {{"enum", json::array({"ok", "partial", "failed", "zero_findings"})}}},
          {"workers", {{"type", "array"}}},
          {"findings", {{"type", "array"}}},
          {"finding_sources", {{"type", "array"}}},
          {"valid_workers", {{"type", "integer"}}},
          {"failed_workers", {{"type", "integer"}}}}},
        {"additionalProperties", false},
    };
}
json core_output_schema() {
    return {
        {"type", "object"},
        {"required", json::array({"channels"})},
        {"properties",
         {{"channels",
           {{"type", "object"},
            {"required", json::array({std::string(kResultChannel)})},
            {"properties",
             {{std::string(kResultChannel),
               {{"type", "object"},
                {"required", json::array({"value"})},
                {"properties", {{"value", final_output_schema()}}},
                {"additionalProperties", true}}}}},
            {"additionalProperties", true}}}}},
        {"additionalProperties", true},
    };
}

std::optional<json> registry_metadata(const program::RegistrySnapshot& registry,
                                      program::ExecutableKind          kind,
                                      std::string_view                 name) {
    const auto entries = registry.manifest().at("entries");
    for (const auto& entry : entries) {
        if (entry.value("kind", "") == program::to_string(kind) &&
            entry.value("name", "") == name) {
            return entry.at("metadata");
        }
    }
    return std::nullopt;
}

struct CatalogTool {
    json                        definition;
    program::ExecutableManifest manifest;
    json                        metadata;
};

std::map<std::string, CatalogTool> validate_tool_catalog(
    const json&                       request,
    const program::RegistrySnapshot&  registry,
    const HarnessTranslationDefaults& defaults) {
    std::map<std::string, CatalogTool> result;
    std::size_t                        index = 0;
    for (const auto& tool : request.value("tool_catalog", json::array())) {
        const auto pointer = "/tool_catalog/" + std::to_string(index++);
        const auto id      = tool.at("id").get<std::string>();
        if (id.empty()) fail("H_TOOL_MISSING", pointer + "/id", "Harness tool id is empty");
        if (result.contains(id))
            fail("H_TOOL_DUPLICATE", pointer + "/id", "Harness tool id occurs more than once");
        const auto manifest = registry.find(program::ExecutableKind::Tool, id);
        if (!manifest)
            fail("H_TOOL_MISSING", pointer + "/id",
                 "Harness tool is absent from the immutable host registry");

        const auto metadata = registry_metadata(registry, program::ExecutableKind::Tool, id);
        if (!metadata ||
            metadata->value("input_schema", json::object()) != tool.at("input_schema") ||
            (tool.contains("output_schema") &&
             metadata->value("output_schema", json::object()) != tool.at("output_schema"))) {
            fail("H_TOOL_SCHEMA_MISMATCH", pointer,
                 "Harness tool schema disagrees with the host-pinned Tool metadata");
        }

        const bool request_read_only =
            request.value("policy", json::object()).value("read_only", false) ||
            tool.value("read_only", false);
        if (request_read_only) {
            const std::set<std::string> allowed(defaults.read_only_effects.begin(),
                                                defaults.read_only_effects.end());
            for (const auto& effect : manifest->declared_effects) {
                if (!allowed.contains(effect)) {
                    fail("H_EFFECT_UNDECLARED", pointer + "/read_only",
                         "Harness read-only restriction conflicts with the host Tool effects");
                }
            }
        }
        result.emplace(id, CatalogTool{tool, *manifest, *metadata});
    }
    return result;
}

struct EffectiveLimits {
    std::uint64_t timeout_seconds;
    std::uint32_t max_parallel_workers;
    std::uint64_t max_core_steps;
    std::uint32_t max_worker_retries;
    std::uint64_t provider_timeout_seconds;
    std::uint64_t max_output_tokens;
    std::uint64_t input_token_ceiling_per_round;
};

EffectiveLimits effective_limits(const json&                                     request,
                                 const HarnessTranslationDefaults&               defaults,
                                 const std::optional<program::ContractManifest>& contract) {
    auto result = EffectiveLimits{
        effective_budget(request, "timeout_seconds", defaults.timeout_seconds, 1, 86400),
        static_cast<std::uint32_t>(effective_budget(request, "max_parallel_workers",
                                                    defaults.max_parallel_workers, 1, 64)),
        effective_budget(request, "max_steps", defaults.max_core_steps, 1, 1000),
        static_cast<std::uint32_t>(
            effective_budget(request, "max_worker_retries", defaults.max_worker_retries, 0, 5)),
        effective_budget(request, "provider_timeout_seconds", defaults.provider_timeout_seconds, 1,
                         defaults.provider_timeout_seconds),
        effective_budget(request, "max_output_tokens", defaults.max_output_tokens, 1,
                         defaults.max_output_tokens),
        defaults.input_token_ceiling_per_round,
    };
    if (contract) {
        const auto& retry = contract->spec().retry_policy;
        result.max_worker_retries =
            std::min<std::uint32_t>(result.max_worker_retries, retry.max_attempts - 1);
    }
    return result;
}

json sealed_worker(const json&                               worker,
                   const std::map<std::string, CatalogTool>& tools,
                   const json&                               policy,
                   const EffectiveLimits&                    limits,
                   const HarnessTranslationDefaults&         defaults) {
    const auto provider_timeout = worker.contains("provider_timeout_seconds")
                                      ? unsigned_value(worker.at("provider_timeout_seconds"),
                                                       "/workers/provider_timeout_seconds")
                                      : limits.provider_timeout_seconds;
    const auto output_tokens =
        worker.contains("max_output_tokens")
            ? unsigned_value(worker.at("max_output_tokens"), "/workers/max_output_tokens")
            : limits.max_output_tokens;
    if (provider_timeout == 0 || provider_timeout > limits.provider_timeout_seconds ||
        output_tokens == 0 || output_tokens > limits.max_output_tokens) {
        fail("H_WORKER_BUDGET", "/workers",
             "Worker limits must be finite and no larger than Harness-wide limits");
    }

    json                  tool_ids     = json::array();
    json                  descriptions = json::object();
    std::set<std::string> seen;
    for (const auto& id_value : worker.value("tools", json::array())) {
        const auto id = id_value.get<std::string>();
        if (!seen.insert(id).second)
            fail("H_TOOL_DUPLICATE", "/workers", "Worker selects the same tool more than once");
        const auto found = tools.find(id);
        if (found == tools.end())
            fail("H_TOOL_MISSING", "/workers", "Worker selects an unknown host Tool");
        tool_ids.push_back(id);
        descriptions[id] = found->second.definition.at("description");
    }

    return {
        {"type", std::string(HARNESS_WORKER_NODE_TYPE)},
        {"worker_id", worker.at("id")},
        {"instructions", worker.at("instructions")},
        {"tool_ids", std::move(tool_ids)},
        {"tool_descriptions", std::move(descriptions)},
        {"output_schema", worker.at("output_schema")},
        {"provider_timeout_ms", checked_mul(provider_timeout, 1000, "/workers")},
        {"max_output_tokens", output_tokens},
        {"input_token_ceiling", limits.input_token_ceiling_per_round},
        {"max_retries", limits.max_worker_retries},
        {"max_provider_tool_rounds", defaults.max_provider_tool_rounds},
        {"evidence_required", policy.value("evidence_required", json::array())},
        {"read_only", policy.value("read_only", false)},
    };
}

std::map<std::string, json> workers_by_id(const json&                               request,
                                          const std::map<std::string, CatalogTool>& tools,
                                          const EffectiveLimits&                    limits,
                                          const HarnessTranslationDefaults&         defaults) {
    std::map<std::string, json> result;
    for (const auto& worker : request.at("workers")) {
        const auto id = worker.at("id").get<std::string>();
        if (id.empty()) fail("H_WORKER_BINDING", "/workers", "Harness worker id is empty");
        if (result.contains(id))
            fail("H_WORKER_BINDING", "/workers", "Harness worker id occurs more than once");
        result.emplace(id, sealed_worker(worker, tools, request.value("policy", json::object()),
                                         limits, defaults));
    }
    return result;
}

json preset_core(const std::string&                 preset,
                 const json&                        request_workers,
                 const std::map<std::string, json>& workers) {
    json core = {
        {"schema_version", 1},
        {"name", "harness_" + preset},
        {"channels",
         {{"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
          {"worker_results", {{"reducer", "append"}, {"initial", json::array()}}},
          {std::string(kResultChannel), {{"reducer", "overwrite"}, {"initial", nullptr}}}}},
        {"nodes", json::object()},
        {"edges", json::array()},
    };
    json        wait_for = json::array();
    std::size_t index    = 0;
    for (const auto& worker : request_workers) {
        const auto& config  = workers.at(worker.at("id").get<std::string>());
        const auto  node    = "worker_" + std::to_string(index++);
        core["nodes"][node] = config;
        core["edges"].push_back({{"from", graph::START_NODE}, {"to", node}});
        core["edges"].push_back({{"from", node}, {"to", "judge"}});
        wait_for.push_back(node);
    }
    core["nodes"]["judge"] = {
        {"type", std::string(HARNESS_JUDGE_NODE_TYPE)},
        {"barrier", {{"wait_for", std::move(wait_for)}}},
    };
    core["edges"].push_back({{"from", "judge"}, {"to", graph::END_NODE}});
    return core;
}

std::vector<program::SourceMapEntry> source_map(const json&                     core,
                                                const std::string&              source_id,
                                                const std::vector<std::string>& worker_ids) {
    std::map<std::string, program::SourceCoordinate> mappings;
    mappings["/input_contract"]               = {source_id, "/task", std::nullopt};
    mappings["/output_contract"]              = {source_id, "/workers", std::nullopt};
    mappings["/declared_budget_requirements"] = {source_id, "/budgets", std::nullopt};
    mappings["/root/definition"]              = {source_id, "/harness/preset", std::nullopt};

    std::map<std::string, std::size_t> worker_indexes;
    for (std::size_t index = 0; index < worker_ids.size(); ++index)
        worker_indexes.emplace(worker_ids[index], index);
    for (const auto& [name, node] : core.at("nodes").items()) {
        if (node.value("type", "") != HARNESS_WORKER_NODE_TYPE) continue;
        const auto index = worker_indexes.find(node.value("worker_id", ""));
        if (index != worker_indexes.end())
            mappings["/root/definition/nodes/" + escape_pointer(name)] = {
                source_id, "/workers/" + std::to_string(index->second), std::nullopt};
    }
    mappings["/root/definition/nodes/judge"] = {source_id, "/harness/preset", std::nullopt};

    std::vector<program::SourceMapEntry> result;
    result.reserve(mappings.size());
    for (auto& [generated, authored] : mappings)
        result.push_back({std::move(generated), std::move(authored)});
    return result;
}

program::RunBudget derive_budget(const json&                                     core,
                                 const EffectiveLimits&                          limits,
                                 const HarnessTranslationDefaults&               defaults,
                                 const std::optional<program::ContractManifest>& contract) {
    auto wall_time_ms = checked_mul(limits.timeout_seconds, 1000, "/budgets/timeout_seconds");
    auto max_program_operations = std::uint64_t{1};
    if (contract) {
        wall_time_ms = std::min(wall_time_ms, contract->spec().retry_policy.max_wall_time_ms);
        max_program_operations = contract->spec().retry_policy.max_program_operations;
    }
    std::uint64_t model_tokens = 0;
    if (core.contains("nodes") && core["nodes"].is_object()) {
        for (const auto& [name, node] : core["nodes"].items()) {
            if (!node.is_object() || node.value("type", "") != HARNESS_WORKER_NODE_TYPE) continue;
            for (const auto field : {"max_output_tokens", "max_retries", "max_provider_tool_rounds",
                                     "provider_timeout_ms"}) {
                if (!node.contains(field))
                    fail("H_WORKER_BINDING", "/harness/definition/nodes/" + escape_pointer(name),
                         "Harness worker node is missing sealed execution limits");
            }
            const auto output = unsigned_value(node.at("max_output_tokens"), "/workers");
            const auto attempts =
                checked_add(unsigned_value(node.at("max_retries"), "/workers"), 1, "/workers");
            const auto rounds = checked_add(
                unsigned_value(node.at("max_provider_tool_rounds"), "/workers"), 1, "/workers");
            const auto per_round = checked_add(output, defaults.input_token_ceiling_per_round,
                                               "/budgets/model_tokens");
            const auto worker_total =
                checked_mul(checked_mul(attempts, rounds, "/budgets/model_tokens"), per_round,
                            "/budgets/model_tokens");
            model_tokens = checked_add(model_tokens, worker_total, "/budgets/model_tokens");
        }
    }
    return {wall_time_ms,
            model_tokens,
            defaults.monetary_microunits,
            1,
            max_program_operations,
            limits.max_core_steps,
            0,
            0,
            0};
}

json budget_records(const program::RunBudget& budget) {
    const std::array<std::uint64_t, 9> values = {
        budget.wall_time_ms,         budget.model_tokens,           budget.monetary_microunits,
        budget.max_concurrency,      budget.max_program_operations, budget.max_core_steps,
        budget.max_dynamic_compiles, budget.max_child_depth,        budget.max_total_children,
    };
    json result = json::array();
    for (std::size_t index = 0; index < kBudgetResources.size(); ++index) {
        result.push_back({{"resource", std::string(kBudgetResources[index])},
                          {"minimum", values[index]},
                          {"maximum", values[index]}});
    }
    return result;
}

program::ExecutableManifest builtin_reducer_manifest(std::string name) {
    const auto identity = program::detail::sha256_identity(
        "neograph-harness-installed-reducer/v1",
        program::detail::canonical_json_bytes(
            {{"component", "harness-reducer/v1"}, {"name", name}, {"semantic_version", "1.0.0"}}));
    return {{program::ExecutableKind::Reducer, std::move(name), "1.0.0", identity},
            program::EffectMode::Brokered,
            "neograph:harness:reducer-v1",
            {},
            {},
            {}};
}

bool identity_less(const program::ExecutableIdentity& lhs, const program::ExecutableIdentity& rhs) {
    return std::tuple{lhs.kind, lhs.name, lhs.semantic_version, lhs.implementation_digest} <
           std::tuple{rhs.kind, rhs.name, rhs.semantic_version, rhs.implementation_digest};
}

}  // namespace

HarnessTranslationError::HarnessTranslationError(std::string code,
                                                 std::string pointer,
                                                 std::string message)
    : std::invalid_argument(message), code_(std::move(code)), pointer_(std::move(pointer)) {}

const std::string& HarnessTranslationError::code() const noexcept {
    return code_;
}
const std::string& HarnessTranslationError::pointer() const noexcept {
    return pointer_;
}

std::string HarnessInvocationTemplate::serialize_canonical() const {
    return program::detail::canonical_json_bytes(invocation_template_body(*this));
}

HarnessInvocationTemplate HarnessInvocationTemplate::parse(std::string_view stored_bytes) {
    try {
        return invocation_template_from_json(program::detail::parse_json_strict(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument(
            std::string("Invalid stored Harness invocation template JSON: ") + error.what());
    }
}

program::RunInvocation bind_harness_invocation(HarnessInvocationTemplate request,
                                               std::string               owner_scope,
                                               std::string               program_version_id,
                                               std::string               run_id,
                                               std::string               correlation_id) {
    program::RunInvocation invocation;
    invocation.owner_scope        = std::move(owner_scope);
    invocation.agent_id           = std::string(HARNESS_PROGRAM_AGENT_ID);
    invocation.program_version_id = std::move(program_version_id);
    invocation.run_id             = std::move(run_id);
    invocation.budget             = std::move(request.budget);
    invocation.input              = std::move(request.input);
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "harness:" + invocation.run_id;
    invocation.correlation_id     = std::move(correlation_id);
    invocation.validate();
    return invocation;
}

json harness_program_request_schema() {
    return json::parse(R"JSON({
        "type":"object",
        "required":["task","harness","workers"],
        "properties":{
            "task":{"type":"object","required":["objective"],"properties":{
                "objective":{"type":"string"},
                "acceptance":{"type":"array","items":{"type":"string"}}
            },"additionalProperties":true},
            "contract":{},
            "contract_manifest":{},
            "workspace_revision":{"type":"string"},
            "harness":{"type":"object","required":["mode"],"properties":{
                "mode":{"enum":["preset","javascript"]},
                "preset":{"type":"string"},
                "source":{"type":"string"},
                "source_id":{"type":"string"}
            },"additionalProperties":false},
            "workers":{"type":"array","items":{"type":"object","required":["id","instructions","output_schema"],"properties":{
                "id":{"type":"string"},
                "instructions":{"type":"string"},
                "tools":{"type":"array","items":{"type":"string"}},
                "provider_timeout_seconds":{"type":"integer"},
                "max_output_tokens":{"type":"integer"},
                "output_schema":{"type":"object"}
            },"additionalProperties":false}},
            "tool_catalog":{"type":"array","items":{"type":"object","required":["id","description","input_schema","executor"],"properties":{
                "id":{"type":"string"},
                "description":{"type":"string"},
                "input_schema":{"type":"object"},
                "output_schema":{"type":"object"},
                "read_only":{"type":"boolean"},
                "path_arguments":{"type":"array","items":{"type":"string"}},
                "executor":{"type":"object","required":["kind"],"properties":{
                    "kind":{"enum":["builtin","mcp","a2a","host_brokered","script"]},
                    "tool":{"type":"string"},
                    "server_ref":{"type":"string"},
                    "agent":{"type":"string"},
                    "interaction":{"enum":["tool_result","input"]},
                    "effect":{"type":"object","required":["idempotency"],"properties":{
                        "idempotency":{"enum":["supported","unsupported"]},
                        "status_query":{"type":"boolean"},
                        "fencing":{"type":"boolean"}
                    },"additionalProperties":false}
                },"additionalProperties":true}
            },"additionalProperties":false}},
            "budgets":{"type":"object","properties":{
                "max_steps":{"type":"integer"},
                "timeout_seconds":{"type":"integer"},
                "max_parallel_workers":{"type":"integer"},
                "max_worker_retries":{"type":"integer"},
                "provider_timeout_seconds":{"type":"integer"},
                "max_output_tokens":{"type":"integer"}
            },"additionalProperties":false},
            "policy":{"type":"object","properties":{
                "read_only":{"type":"boolean"},
                "workspace_roots":{"type":"array","items":{"type":"string"}},
                "evidence_required":{"type":"array","items":{"type":"string"}},
                "workspace_revision":{"type":"string"}
            },"additionalProperties":false}
        },
        "additionalProperties":false
    })JSON");
}

json harness_program_output_schema() {
    return final_output_schema();
}

HarnessTranslation HarnessRequestTranslator::translate(const json&                       request,
                                                       const program::RegistrySnapshot&  registry,
                                                       const HarnessTranslationDefaults& defaults) {
    validate_defaults(defaults);
    if (!request.is_object() || !request.contains("harness") ||
        !request.at("harness").is_object() || !request.at("harness").contains("mode") ||
        !request.at("harness").at("mode").is_string()) {
        fail(
            "H_AUTHORING_MODE_REQUIRED", "/harness/mode",
            "Harness authoring requires an explicit mode; no legacy compiler is selected by shape");
    }
    const auto mode = request.at("harness").at("mode").get<std::string>();
    if (mode == "dsl")
        fail("H_MIGRATION_CORE_DSL", "/harness/mode",
             "Core DSL authoring is frozen; submit JavaScript define() source instead");
    if (mode == "core")
        fail("H_MIGRATION_CORE_JSON", "/harness/mode",
             "Standalone Core JSON authoring is internal/interchange only; submit JavaScript "
             "define() source instead");
    if (mode == "program" || mode == "program_json")
        fail("H_MIGRATION_PROGRAM_JSON", "/harness/mode",
             "Program JSON authoring is frozen; submit JavaScript generator main() source instead");
    try {
        validate_json_value(request, harness_program_request_schema(), "Harness request", "$");
    } catch (const std::exception& error) {
        fail("H_REQUEST_SCHEMA", "", error.what());
    }

    const auto contract = requested_contract(request);
    if (request.at("workers").empty())
        fail("H_WORKERS_EMPTY", "/workers", "At least one Harness worker is required");
    static const std::set<std::string> presets = {"fanout_judge", "pr_review_panel", "bug_triage",
                                                  "research_synthesis"};
    if (mode == "preset" && !presets.contains(request.at("harness").value("preset", "")))
        fail("H_PRESET", "/harness/preset", "Unknown Harness preset");
    if (mode == "javascript" && (!request.at("harness").contains("source") ||
                                 !request.at("harness").at("source").is_string() ||
                                 request.at("harness").at("source").get<std::string>().empty()))
        fail("H_JAVASCRIPT_SOURCE", "/harness/source",
             "Harness JavaScript mode requires non-empty UTF-8 source text");

    const auto limits  = effective_limits(request, defaults, contract);
    const auto tools   = validate_tool_catalog(request, registry, defaults);
    const auto workers = workers_by_id(request, tools, limits, defaults);

    json                                  core;
    std::optional<program::ProgramSource> authored_source;
    if (mode == "preset") {
        core = preset_core(request.at("harness").at("preset").get<std::string>(),
                           request.at("workers"), workers);
    } else {
        const auto source_id =
            request.at("harness").value("source_id", defaults.source_id_prefix + ":javascript");
        try {
            authored_source = program::ProgramSource::from_javascript(
                source_id, request.at("harness").at("source").get<std::string>());
        } catch (const program::ProgramDiagnosticError& error) {
            fail("H_JAVASCRIPT_SOURCE", "/harness/source", error.what());
        } catch (const std::exception& error) {
            fail("H_JAVASCRIPT_SOURCE", "/harness/source", error.what());
        }
        // Worker bindings remain host-owned. This synthetic Core-shaped view is
        // used only to derive finite execution budgets.
        core = json{{"nodes", json::object()}};
        for (const auto& [worker_id, worker_config] : workers)
            core["nodes"]["worker:" + worker_id] = worker_config;
    }

    bool has_worker_node = mode == "javascript" && !workers.empty();
    if (core.contains("nodes") && core["nodes"].is_object()) {
        for (const auto& [name, node] : core["nodes"].items()) {
            (void)name;
            has_worker_node = has_worker_node || (node.is_object() && node.value("type", "") ==
                                                                          HARNESS_WORKER_NODE_TYPE);
        }
    }
    if (has_worker_node && !defaults.provider)
        fail("H_PROVIDER_MISSING", "/defaults/provider",
             "Harness worker closure requires an exact host Provider identity");
    if (defaults.provider) {
        const auto found =
            registry.find(program::ExecutableKind::Provider, defaults.provider->name);
        if (!found || found->identity != *defaults.provider)
            fail("H_PROVIDER_MISSING", "/defaults/provider",
                 "Harness Provider identity is absent or mismatched in the immutable registry");
    }

    const auto budget = derive_budget(core, limits, defaults, contract);
    const auto source_id =
        authored_source ? authored_source->source_id() : defaults.source_id_prefix + ":" + mode;
    json document = json::object();
    if (!authored_source) {
        document = {
            {"program_schema_version", 1},
            {"input_contract", {{"schema_version", 1}, {"schema", input_contract_schema()}}},
            {"output_contract", {{"schema_version", 1}, {"schema", core_output_schema()}}},
            {"root",
             {{"op", "call_core"}, {"name", core.at("name")}, {"definition", std::move(core)}}},
            {"declared_budget_requirements", budget_records(budget)},
        };
    }

    HarnessWireReceipt wire;
    wire.source_id          = source_id;
    wire.mode               = mode;
    wire.authoring_frontend = authored_source ? program::AuthoringFrontend::JavaScript
                                              : program::AuthoringFrontend::TrustedCpp;
    wire.preset             = request.at("harness").value("preset", "");
    wire.workspace_revision =
        request.value("workspace_revision",
                      request.value("policy", json::object()).value("workspace_revision", ""));
    for (const auto& worker : request.at("workers"))
        wire.worker_ids.push_back(worker.at("id").get<std::string>());
    wire.projection = {
        {"task", request.at("task")},
        {"output_channel", std::string(kResultChannel)},
    };
    if (!wire.workspace_revision.empty())
        wire.projection["workspace_revision"] = wire.workspace_revision;
    if (contract) {
        wire.projection["contract_manifest"]      = json::parse(contract->serialize_canonical());
        wire.projection["contract_manifest_hash"] = contract->content_hash();
    }

    HarnessCapabilityBindingRequest bindings;
    bindings.provider            = defaults.provider;
    bindings.policy_restrictions = request.value("policy", json::object());
    std::set<std::string> selected;
    for (const auto& [id, worker] : workers) {
        (void)id;
        for (const auto& tool_id : worker.at("tool_ids"))
            selected.insert(tool_id.get<std::string>());
    }
    const auto translated_nodes =
        authored_source ? json::object()
                        : document.at("root").at("definition").value("nodes", json::object());
    if (translated_nodes.is_object()) {
        for (const auto& [name, node] : translated_nodes.items()) {
            if (!node.is_object() || node.value("type", "") != HARNESS_WORKER_NODE_TYPE) continue;
            for (const auto& tool_id : node.value("tool_ids", json::array())) {
                const auto id = tool_id.get<std::string>();
                if (!tools.contains(id))
                    fail("H_TOOL_MISSING",
                         "/harness/definition/nodes/" + escape_pointer(name) + "/tool_ids",
                         "Harness Core worker selects a Tool without a validated host binding");
                selected.insert(id);
            }
        }
    }
    for (const auto& id : selected) {
        const auto& tool               = tools.at(id);
        json        host_configuration = {
            {"id", id},
            {"executor", tool.definition.at("executor")},
        };
        if (tool.definition.contains("path_arguments"))
            host_configuration["path_arguments"] = tool.definition.at("path_arguments");
        bindings.tools.push_back({tool.manifest.identity, std::move(host_configuration)});
        wire.tool_ids.push_back(id);
    }

    auto map  = authored_source
                    ? std::vector<program::SourceMapEntry>{}
                    : source_map(document.at("root").at("definition"), source_id, wire.worker_ids);
    auto task = request.at("task");
    if (contract) task["contract_manifest"] = json::parse(contract->serialize_canonical());
    auto                      source = authored_source ? std::move(*authored_source)
                                                       : program::ProgramSource::from_cpp_builder(
                                        source_id, 1, std::move(document), {}, std::move(map));
    HarnessInvocationTemplate invocation_template{{{"task", std::move(task)}}, budget};
    return {std::move(source), std::move(invocation_template), std::move(wire), std::move(bindings),
            contract};
}

HarnessProgramSnapshots build_harness_program_snapshots(HarnessProgramSnapshotConfig config) {
    if (config.registry.worker.manifest.identity.kind != program::ExecutableKind::Node ||
        config.registry.worker.manifest.identity.name != HARNESS_WORKER_NODE_TYPE ||
        config.registry.judge.manifest.identity.kind != program::ExecutableKind::Node ||
        config.registry.judge.manifest.identity.name != HARNESS_JUDGE_NODE_TYPE) {
        throw std::invalid_argument(
            "Harness worker/judge registrations must use their exact node identities");
    }
    if (config.registry.worker.requirement_resolver || config.registry.judge.requirement_resolver) {
        throw std::invalid_argument(
            "Harness worker/judge requirement closure is fixed by the host snapshot builder");
    }
    for (const auto& dependency : config.registry.worker.manifest.required_executables) {
        if (dependency.kind == program::ExecutableKind::Provider ||
            dependency.kind == program::ExecutableKind::Tool ||
            dependency.kind == program::ExecutableKind::Imported) {
            throw std::invalid_argument(
                "Harness worker Provider/Tool/import closure must be config-derived");
        }
    }

    std::map<std::string, program::ExecutableIdentity> tool_identities;
    for (const auto& tool : config.registry.tools)
        tool_identities.emplace(tool.manifest.identity.name, tool.manifest.identity);
    const auto provider      = config.registry.provider
                                   ? std::optional<program::ExecutableIdentity>{config.registry.provider
                                                                                    ->manifest.identity}
                                   : std::nullopt;
    auto       host_resolver = [provider, tool_identities](const json& node) {
        std::vector<program::ExecutableIdentity> requirements;
        if (provider) requirements.push_back(*provider);
        if (!node.contains("tool_ids") || !node.at("tool_ids").is_array())
            throw std::invalid_argument("Harness worker config requires sealed tool_ids");
        for (const auto& id_value : node.at("tool_ids")) {
            const auto id    = id_value.get<std::string>();
            const auto found = tool_identities.find(id);
            if (found == tool_identities.end())
                throw std::invalid_argument("Harness worker config selects an absent host Tool");
            requirements.push_back(found->second);
        }
        std::sort(requirements.begin(), requirements.end(), identity_less);
        requirements.erase(std::unique(requirements.begin(), requirements.end()),
                                 requirements.end());
        return requirements;
    };

    program::RegistrySnapshotBuilder builder;
    builder.add_node(std::move(config.registry.worker.manifest),
                     std::move(config.registry.worker.factory),
                     std::move(config.registry.worker.config_schema),
                     std::move(config.registry.worker.effects), std::move(host_resolver));
    builder.add_node(
        std::move(config.registry.judge.manifest), std::move(config.registry.judge.factory),
        std::move(config.registry.judge.config_schema), std::move(config.registry.judge.effects));
    builder.add_reducer(builtin_reducer_manifest("overwrite"),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(builtin_reducer_manifest("append"),
                        [](const json& current, const json& incoming) {
                            json result = current.is_array() ? current : json::array();
                            if (incoming.is_array()) {
                                for (const auto& value : incoming)
                                    result.push_back(value);
                            } else {
                                result.push_back(incoming);
                            }
                            return result;
                        });
    for (auto& node : config.registry.additional_nodes)
        builder.add_node(std::move(node.manifest), std::move(node.factory),
                         std::move(node.config_schema), std::move(node.effects),
                         std::move(node.requirement_resolver));
    for (auto& reducer : config.registry.additional_reducers)
        builder.add_reducer(std::move(reducer.manifest), std::move(reducer.reducer));
    for (auto& condition : config.registry.conditions)
        builder.add_condition(std::move(condition.manifest), std::move(condition.condition),
                              std::move(condition.spec));
    if (config.registry.provider)
        builder.add_provider(std::move(config.registry.provider->manifest),
                             std::move(config.registry.provider->metadata));
    for (auto& tool : config.registry.tools)
        builder.add_tool(std::move(tool.manifest), std::move(tool.metadata));
    auto registry = std::move(builder).build();

    program::AdmissionProfileBuilder admission_builder;
    admission_builder.id(std::move(config.admission_profile_id))
        .semantic_version(std::move(config.admission_semantic_version))
        .registry(registry)
        .mode(config.admission_mode)
        .max_program_schema_version(1)
        .allow_source_kind(program::SourceKind::CppBuilder)
        .allow_source_kind(program::SourceKind::JavaScript);
    bool brokered = false;
    bool trusted  = false;
    for (const auto& identity : registry.identities()) {
        admission_builder.allow_executable(identity);
        const auto manifest = registry.find(identity.kind, identity.name);
        brokered            = brokered || manifest->effect_mode == program::EffectMode::Brokered;
        trusted = trusted || manifest->effect_mode == program::EffectMode::TrustedNative;
    }
    if (brokered) admission_builder.allow_effect_mode(program::EffectMode::Brokered);
    if (trusted) admission_builder.allow_effect_mode(program::EffectMode::TrustedNative);
    auto admission = std::move(admission_builder).build();

    program::PolicySnapshotBuilder policy_builder;
    policy_builder.id(std::move(config.policy_id))
        .semantic_version(std::move(config.policy_semantic_version))
        .owner_scope(std::move(config.owner_scope))
        .admission_profile(admission)
        .budget_ceiling(config.budget_ceiling);
    for (auto& capability : config.allowed_capabilities)
        policy_builder.allow_capability(std::move(capability));
    for (auto& effect : config.allowed_effects)
        policy_builder.allow_effect(std::move(effect));
    for (auto& digest : config.allowed_module_digests)
        policy_builder.allow_module_digest(std::move(digest));
    auto policy = std::move(policy_builder).build();
    return {std::move(registry), std::move(admission), std::move(policy)};
}

}  // namespace neograph::mcp
