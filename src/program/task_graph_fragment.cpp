#include <neograph/program/task_graph_fragment.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace neograph::program {
namespace {

using detail::canonical_json_bytes;
using detail::owned_json_copy;

void require_token(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void require_identity(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value))
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
}

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs, std::string_view field) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
        throw std::invalid_argument(std::string(field) + " exceeds uint64 range");
    return lhs + rhs;
}

TaskGraphBudget add_budget(const TaskGraphBudget& lhs, const TaskGraphBudget& rhs) {
    return {checked_add(lhs.wall_time_ms, rhs.wall_time_ms, "Task graph wall_time_ms"),
            checked_add(lhs.model_tokens, rhs.model_tokens, "Task graph model_tokens"),
            checked_add(lhs.monetary_microunits, rhs.monetary_microunits,
                        "Task graph monetary_microunits"),
            checked_add(lhs.output_bytes, rhs.output_bytes, "Task graph output_bytes")};
}

bool budget_leq(const TaskGraphBudget& value, const TaskGraphBudget& ceiling) {
    return value.wall_time_ms <= ceiling.wall_time_ms &&
           value.model_tokens <= ceiling.model_tokens &&
           value.monetary_microunits <= ceiling.monetary_microunits &&
           value.output_bytes <= ceiling.output_bytes;
}

TaskGraphBudget budget_from_limits(const BudgetLimits& limits) {
    return {limits.wall_time_ms, limits.model_tokens, limits.monetary_microunits,
            std::numeric_limits<std::uint64_t>::max()};
}

json budget_to_json(const TaskGraphBudget& budget) {
    return json{{"wall_time_ms", budget.wall_time_ms},
                {"model_tokens", budget.model_tokens},
                {"monetary_microunits", budget.monetary_microunits},
                {"output_bytes", budget.output_bytes}};
}

json binding_to_json(const TaskGraphInputBinding& binding) {
    return json{{"from", { {"task", binding.from.task_id},
                             {"artifact", binding.from.artifact},
                             {"field", binding.from.field} }},
                {"to", {{"field", binding.to.field}}}};
}

json artifact_contract_to_json(const TaskGraphArtifactContract& contract) {
    return json{{"artifact", contract.artifact}, {"fields", contract.fields}};
}

json artifact_contracts_to_json(const std::vector<TaskGraphArtifactContract>& contracts) {
    json encoded = json::array();
    for (const auto& contract : contracts) encoded.push_back(artifact_contract_to_json(contract));
    return encoded;
}

json task_to_json(const CompiledTaskGraphTask& task) {
    json bindings = json::array();
    for (const auto& binding : task.input_bindings) bindings.push_back(binding_to_json(binding));
    return json{{"id", task.task_id},
                {"operation_id", task.operation_id},
                {"template_id", task.template_id},
                {"template_identity", task.template_identity},
                {"child_binding", task.child_binding},
                {"input_bindings", std::move(bindings)},
                {"depends_on", task.depends_on},
                {"budget", budget_to_json(task.requested_budget)},
                {"reserved_budget", budget_to_json(task.reserved_budget)},
                {"capabilities", task.capabilities},
                {"effects", task.effects},
                {"input_fields", task.input_fields},
                {"output_artifacts", artifact_contracts_to_json(task.output_artifacts)}};
}

json receipt_to_json(const TaskGraphTemplateReceipt& receipt) {
    return json{{"template_id", receipt.template_id},
                {"content_identity", receipt.content_identity},
                {"child_binding", receipt.child_binding},
                {"executable_identity", receipt.executable_identity},
                {"kind", receipt.kind},
                {"capabilities", receipt.capabilities},
                {"effects", receipt.effects},
                {"input_fields", receipt.input_fields},
                {"output_artifacts", artifact_contracts_to_json(receipt.output_artifacts)}};
}

json limits_to_json(const TaskGraphBudget& budget) { return budget_to_json(budget); }

std::string task_operation_id(std::string_view fragment_id, std::string_view task_id) {
    return detail::sha256_identity(
        "program-task-graph-operation/v1",
        canonical_json_bytes(json{{"fragment_id", std::string(fragment_id)},
                                  {"task_id", std::string(task_id)}}));
}

bool is_subset(const std::vector<std::string>& requested,
               const std::set<std::string, std::less<>>& allowed) {
    return std::all_of(requested.begin(), requested.end(), [&](const auto& value) {
        return allowed.contains(value);
    });
}

std::set<std::string, std::less<>> intersection(const std::vector<std::string>& lhs,
                                                 const std::vector<std::string>& rhs) {
    std::set<std::string, std::less<>> left(lhs.begin(), lhs.end());
    std::set<std::string, std::less<>> right(rhs.begin(), rhs.end());
    std::set<std::string, std::less<>> result;
    std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                          std::inserter(result, result.end()));
    return result;
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

json fragment_identity_json(const TaskGraphFragmentCompileOptions& options,
                            const TaskGraphProposal& proposal,
                            const std::vector<CompiledTaskGraphTask>& tasks,
                            const TaskGraphJoinPolicy& join,
                            const std::vector<TaskGraphTemplateReceipt>& receipts,
                            const std::vector<std::string>& closure,
                            const TaskGraphBudget& aggregate) {
    json task_json = json::array();
    for (const auto& task : tasks) task_json.push_back(task_to_json(task));
    json receipt_json = json::array();
    for (const auto& receipt : receipts) receipt_json.push_back(receipt_to_json(receipt));
    return json{{"schema_version", CompiledTaskGraphFragment::SCHEMA_VERSION},
                {"owner_scope", options.owner_scope},
                {"parent_run_id", options.parent_run_id},
                {"expansion_operation_id", options.expansion_operation_id},
                {"parent_program_version_id", options.parent_program_version_id},
                {"child_depth", options.child_depth},
                {"proposal_hash", proposal.proposal_hash()},
                {"tasks", std::move(task_json)},
                {"join", {{"kind", to_string(join.kind)}}},
                {"aggregate_budget", budget_to_json(aggregate)},
                {"template_receipts", std::move(receipt_json)},
                {"capability_effect_closure", closure}};
}

namespace {

std::string required_string(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_string())
        throw std::invalid_argument("Task graph fragment field '" + key + "' must be a string");
    return value.at(key).get<std::string>();
}

std::uint64_t required_uint64(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned())
        throw std::invalid_argument("Task graph fragment field '" + key + "' must be unsigned");
    return value.at(key).get<std::uint64_t>();
}

std::uint32_t required_uint32(const json& value, std::string_view field) {
    const auto number = required_uint64(value, field);
    if (number > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Task graph fragment field '" + std::string(field) +
                                    "' exceeds uint32 range");
    return static_cast<std::uint32_t>(number);
}

std::vector<std::string> required_strings(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_array())
        throw std::invalid_argument("Task graph fragment field '" + key + "' must be an array");
    std::vector<std::string> result;
    result.reserve(value.at(key).size());
    for (const auto& item : value.at(key)) {
        if (!item.is_string())
            throw std::invalid_argument("Task graph fragment field '" + key +
                                        "' must contain strings");
        result.push_back(item.get<std::string>());
    }
    return result;
}

TaskGraphBudget budget_from_json(const json& value, std::string_view label) {
    if (!value.is_object())
        throw std::invalid_argument(std::string(label) + " must be an object");
    detail::reject_unknown_fields(
        value, label, {"wall_time_ms", "model_tokens", "monetary_microunits", "output_bytes"});
    return {required_uint64(value, "wall_time_ms"),
            required_uint64(value, "model_tokens"),
            required_uint64(value, "monetary_microunits"),
            required_uint64(value, "output_bytes")};
}

TaskGraphInputBinding binding_from_json(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Task graph fragment input binding must be an object");
    detail::reject_unknown_fields(value, "Task graph fragment input binding", {"from", "to"});
    if (!value.contains("from") || !value.contains("to"))
        throw std::invalid_argument("Task graph fragment input binding requires from and to");

    const auto& from = value.at("from");
    const auto& to = value.at("to");
    if (!from.is_object() || !to.is_object())
        throw std::invalid_argument("Task graph fragment binding endpoints must be objects");
    detail::reject_unknown_fields(from, "Task graph fragment binding source",
                                  {"task", "artifact", "field"});
    detail::reject_unknown_fields(to, "Task graph fragment binding target", {"field"});
    TaskGraphInputBinding result;
    result.from.task_id = required_string(from, "task");
    result.from.artifact = required_string(from, "artifact");
    result.from.field = required_string(from, "field");
    result.to.field = required_string(to, "field");
    return result;
}
void validate_field_list(const std::vector<std::string>& fields, std::string_view label) {
    std::set<std::string, std::less<>> unique;
    for (const auto& field : fields) {
        detail::validate_json_pointer(field);
        if (field.empty())
            throw std::invalid_argument(std::string(label) + " must not contain empty fields");
        if (!unique.insert(field).second)
            throw std::invalid_argument(std::string(label) + " must contain unique fields");
    }
}

TaskGraphArtifactContract artifact_contract_from_json(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Task graph artifact contract must be an object");
    detail::reject_unknown_fields(value, "Task graph artifact contract", {"artifact", "fields"});
    TaskGraphArtifactContract result;
    result.artifact = required_string(value, "artifact");
    result.fields = required_strings(value, "fields");
    require_token(result.artifact, "Task graph artifact");
    validate_field_list(result.fields, "Task graph artifact fields");
    return result;
}

std::vector<TaskGraphArtifactContract> artifact_contracts_from_json(const json& value) {
    if (!value.is_array())
        throw std::invalid_argument("Task graph output artifacts must be an array");
    std::vector<TaskGraphArtifactContract> result;
    std::set<std::string, std::less<>> artifacts;
    result.reserve(value.size());
    for (const auto& item : value) {
        auto contract = artifact_contract_from_json(item);
        if (!artifacts.insert(contract.artifact).second)
            throw std::invalid_argument("Task graph output artifacts must be unique");
        result.push_back(std::move(contract));
    }
    return result;
}

CompiledTaskGraphTask task_from_json(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Task graph compiled task must be an object");
    detail::reject_unknown_fields(
        value, "Task graph compiled task",
        {"id", "operation_id", "template_id", "template_identity", "child_binding",
         "input_bindings", "depends_on", "budget", "reserved_budget", "capabilities", "effects",
         "input_fields", "output_artifacts"});
    for (const auto field : {"input_bindings", "depends_on", "capabilities", "effects"}) {
        if (!value.contains(field) || !value.at(field).is_array())
            throw std::invalid_argument("Task graph compiled task field '" + std::string(field) +
                                        "' must be an array");
    }
    CompiledTaskGraphTask result;
    result.task_id = required_string(value, "id");
    result.operation_id = required_string(value, "operation_id");
    result.template_id = required_string(value, "template_id");
    result.template_identity = required_string(value, "template_identity");
    result.child_binding = required_string(value, "child_binding");
    require_token(result.task_id, "Task graph task id");
    require_token(result.operation_id, "Task graph task operation_id");
    require_token(result.template_id, "Task graph task template_id");
    require_identity(result.template_identity, "Task graph task template_identity");
    require_token(result.child_binding, "Task graph task child_binding");
    for (const auto& binding : value.at("input_bindings"))
        result.input_bindings.push_back(binding_from_json(binding));
    for (const auto& dependency : value.at("depends_on")) {
        if (!dependency.is_string())
            throw std::invalid_argument("Task graph task depends_on must contain strings");
        result.depends_on.push_back(dependency.get<std::string>());
    }
    result.requested_budget = budget_from_json(value.at("budget"), "Task graph task budget");
    result.reserved_budget =
        budget_from_json(value.at("reserved_budget"), "Task graph task reserved_budget");
    result.capabilities = required_strings(value, "capabilities");
    result.effects = required_strings(value, "effects");
    if (value.contains("input_fields")) {
        result.input_fields = required_strings(value, "input_fields");
        validate_field_list(result.input_fields, "Task graph input_fields");
    }
    if (value.contains("output_artifacts"))
        result.output_artifacts = artifact_contracts_from_json(value.at("output_artifacts"));
    return result;
}

TaskGraphTemplateReceipt receipt_from_json(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Task graph template receipt must be an object");
    detail::reject_unknown_fields(value, "Task graph template receipt",
                                  {"template_id", "content_identity", "child_binding",
                                   "executable_identity", "kind", "capabilities", "effects",
                                   "input_fields", "output_artifacts"});
    TaskGraphTemplateReceipt result;
    result.template_id = required_string(value, "template_id");
    result.content_identity = required_string(value, "content_identity");
    result.child_binding = required_string(value, "child_binding");
    result.executable_identity = required_string(value, "executable_identity");
    result.kind = required_string(value, "kind");
    result.capabilities = required_strings(value, "capabilities");
    result.effects = required_strings(value, "effects");
    if (value.contains("input_fields")) {
        result.input_fields = required_strings(value, "input_fields");
        validate_field_list(result.input_fields, "Task graph receipt input_fields");
    }
    if (value.contains("output_artifacts"))
        result.output_artifacts = artifact_contracts_from_json(value.at("output_artifacts"));
    require_token(result.template_id, "Task graph receipt template_id");
    require_identity(result.content_identity, "Task graph receipt content_identity");
    require_token(result.child_binding, "Task graph receipt child_binding");
    if (!result.executable_identity.empty())
        require_token(result.executable_identity, "Task graph receipt executable_identity");
    if (!result.kind.empty()) require_token(result.kind, "Task graph receipt kind");
    return result;
}

} // namespace
} // namespace

struct CompiledTaskGraphFragment::Impl {
    std::uint32_t                         schema_version = SCHEMA_VERSION;
    std::string                           fragment_id;
    std::string                           owner_scope;
    std::string                           parent_run_id;
    std::string                           expansion_operation_id;
    std::string                           parent_program_version_id;
    std::uint32_t                         child_depth = 0;
    std::string                           proposal_hash;
    std::vector<CompiledTaskGraphTask>    tasks;
    TaskGraphJoinPolicy                   join;
    TaskGraphBudget                       aggregate_budget;
    std::vector<TaskGraphTemplateReceipt> template_receipts;
    std::vector<std::string>              capability_effect_closure;
    std::vector<SourceMapEntry>            source_map;
    json                                  document;
    std::string                           canonical_bytes;
};

CompiledTaskGraphFragment::CompiledTaskGraphFragment(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
CompiledTaskGraphFragment::~CompiledTaskGraphFragment() = default;
std::uint32_t CompiledTaskGraphFragment::schema_version() const noexcept {
    return impl_->schema_version;
}
const std::string& CompiledTaskGraphFragment::fragment_id() const noexcept {
    return impl_->fragment_id;
}
const std::string& CompiledTaskGraphFragment::owner_scope() const noexcept {
    return impl_->owner_scope;
}
const std::string& CompiledTaskGraphFragment::parent_run_id() const noexcept {
    return impl_->parent_run_id;
}
const std::string& CompiledTaskGraphFragment::parent_program_version_id() const noexcept {
    return impl_->parent_program_version_id;
}
std::uint32_t CompiledTaskGraphFragment::child_depth() const noexcept {
    return impl_->child_depth;
}
const std::string& CompiledTaskGraphFragment::expansion_operation_id() const noexcept {
    return impl_->expansion_operation_id;
}
const std::string& CompiledTaskGraphFragment::proposal_hash() const noexcept {
    return impl_->proposal_hash;
}
const std::vector<CompiledTaskGraphTask>& CompiledTaskGraphFragment::tasks() const noexcept {
    return impl_->tasks;
}
const TaskGraphJoinPolicy& CompiledTaskGraphFragment::join() const noexcept { return impl_->join; }
const TaskGraphBudget& CompiledTaskGraphFragment::aggregate_budget() const noexcept {
    return impl_->aggregate_budget;
}
const std::vector<TaskGraphTemplateReceipt>&
CompiledTaskGraphFragment::template_receipts() const noexcept {
    return impl_->template_receipts;
}
const std::vector<std::string>&
CompiledTaskGraphFragment::capability_effect_closure() const noexcept {
    return impl_->capability_effect_closure;
}
const std::vector<SourceMapEntry>& CompiledTaskGraphFragment::source_map() const noexcept {
    return impl_->source_map;
}
json CompiledTaskGraphFragment::to_json() const { return owned_json_copy(impl_->document); }
const std::string& CompiledTaskGraphFragment::serialize_canonical() const noexcept {
    return impl_->canonical_bytes;
}

bool CompiledTaskGraphFragment::operator==(
    const CompiledTaskGraphFragment& other) const noexcept {
    return serialize_canonical() == other.serialize_canonical();
}

CompiledTaskGraphFragment CompiledTaskGraphFragment::parse(std::string_view stored_bytes) {
    json document;
    try {
        document = json::parse(std::string(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument("Invalid stored task graph fragment JSON: " +
                                    std::string(error.what()));
    }
    if (!document.is_object())
        throw std::invalid_argument("Stored task graph fragment must be an object");
    detail::reject_unknown_fields(
        document, "Stored task graph fragment",
        {"schema_version", "fragment_id", "owner_scope", "parent_run_id",
         "expansion_operation_id", "parent_program_version_id", "child_depth", "proposal_hash",
         "tasks", "join", "aggregate_budget", "template_receipts", "capability_effect_closure",
         "source_map"});
    if (required_uint32(document, "schema_version") != SCHEMA_VERSION)
        throw std::invalid_argument("Unsupported stored task graph fragment schema_version");
    if (!document.contains("tasks") || !document.at("tasks").is_array())
        throw std::invalid_argument("Stored task graph fragment tasks must be an array");
    if (!document.contains("template_receipts") ||
        !document.at("template_receipts").is_array())
        throw std::invalid_argument("Stored task graph fragment template_receipts must be an array");
    if (!document.contains("join") || !document.at("join").is_object())
        throw std::invalid_argument("Stored task graph fragment join must be an object");
    detail::reject_unknown_fields(document.at("join"), "Stored task graph fragment join", {"kind"});
    const auto join_kind = required_string(document.at("join"), "kind");
    TaskGraphJoinPolicy join;
    join.kind = task_graph_join_kind_from_string(join_kind);
    if (!document.contains("capability_effect_closure") ||
        !document.at("capability_effect_closure").is_array())
        throw std::invalid_argument(
            "Stored task graph fragment capability_effect_closure must be an array");

    const auto fragment_id = required_string(document, "fragment_id");
    const auto owner_scope = required_string(document, "owner_scope");
    const auto parent_run_id = required_string(document, "parent_run_id");
    const auto expansion_operation_id = required_string(document, "expansion_operation_id");
    const auto parent_program_version_id = required_string(document, "parent_program_version_id");
    const auto proposal_hash = required_string(document, "proposal_hash");
    require_identity(fragment_id, "Stored task graph fragment fragment_id");
    require_token(owner_scope, "Stored task graph fragment owner_scope");
    require_token(parent_run_id, "Stored task graph fragment parent_run_id");
    require_token(expansion_operation_id, "Stored task graph fragment expansion_operation_id");
    require_token(parent_program_version_id,
                  "Stored task graph fragment parent_program_version_id");
    require_identity(proposal_hash, "Stored task graph fragment proposal_hash");

    std::vector<CompiledTaskGraphTask> tasks;
    tasks.reserve(document.at("tasks").size());
    for (const auto& encoded : document.at("tasks")) tasks.push_back(task_from_json(encoded));
    std::set<std::string, std::less<>> task_ids;
    for (const auto& task : tasks) {
        if (!task_ids.insert(task.task_id).second)
            throw std::invalid_argument("Stored task graph fragment has duplicate task id");
        if (task.operation_id != task_operation_id(fragment_id, task.task_id))
            throw std::invalid_argument("Stored task graph fragment task operation_id mismatch");
    }

    std::vector<TaskGraphTemplateReceipt> receipts;
    receipts.reserve(document.at("template_receipts").size());
    for (const auto& encoded : document.at("template_receipts"))
        receipts.push_back(receipt_from_json(encoded));

    const auto aggregate =
        budget_from_json(document.at("aggregate_budget"), "Stored task graph aggregate_budget");
    const auto closure = required_strings(document, "capability_effect_closure");
    std::map<std::string, const TaskGraphTemplateReceipt*, std::less<>> receipt_by_template;
    for (const auto& receipt : receipts) {
        if (!receipt_by_template.emplace(receipt.template_id, &receipt).second)
            throw std::invalid_argument("Stored task graph fragment has duplicate template receipt");
    }
    TaskGraphBudget reserved_total;
    const std::set<std::string, std::less<>> closure_set(closure.begin(), closure.end());
    for (const auto& task : tasks) {
        const auto receipt = receipt_by_template.find(task.template_id);
        if (receipt == receipt_by_template.end())
            throw std::invalid_argument("Stored task graph task has no template receipt");
        if (task.template_identity != receipt->second->content_identity ||
            task.child_binding != receipt->second->child_binding ||
            task.capabilities != receipt->second->capabilities ||
            task.effects != receipt->second->effects ||
            task.input_fields != receipt->second->input_fields ||
            task.output_artifacts != receipt->second->output_artifacts)
            throw std::invalid_argument("Stored task graph task does not match its template receipt");
        if (!is_subset(task.capabilities, closure_set) ||
            !is_subset(task.effects, closure_set))
            throw std::invalid_argument("Stored task graph task exceeds capability/effect closure");
        reserved_total = add_budget(reserved_total, task.reserved_budget);
    }
    if (!(reserved_total == aggregate))
        throw std::invalid_argument("Stored task graph aggregate budget does not match reservations");

    std::vector<SourceMapEntry> source_map;
    if (document.contains("source_map")) {
        if (!document.at("source_map").is_array())
            throw std::invalid_argument("Stored task graph fragment source_map must be an array");
        source_map.reserve(document.at("source_map").size());
        for (const auto& encoded : document.at("source_map")) {
            SourceMapEntry entry;
            from_json(encoded, entry);
            source_map.push_back(std::move(entry));
        }
    }

    json identity = json::object();
    for (const auto field : {"schema_version", "owner_scope", "parent_run_id",
                             "expansion_operation_id", "parent_program_version_id", "child_depth",
                             "proposal_hash", "tasks", "join", "aggregate_budget",
                             "template_receipts", "capability_effect_closure"}) {
        identity[field] = owned_json_copy(document.at(field));
    }
    for (std::size_t index = 0; index < identity["tasks"].size(); ++index)
        identity["tasks"][index]["operation_id"] = "";
    const auto computed_fragment_id =
        detail::sha256_identity("program-task-graph-fragment/v1", canonical_json_bytes(identity));
    if (computed_fragment_id != fragment_id)
        throw std::invalid_argument("Stored task graph fragment fragment_id does not match content");

    auto impl = std::make_shared<CompiledTaskGraphFragment::Impl>();
    impl->fragment_id = fragment_id;
    impl->owner_scope = owner_scope;
    impl->parent_run_id = parent_run_id;
    impl->expansion_operation_id = expansion_operation_id;
    impl->parent_program_version_id = parent_program_version_id;
    impl->child_depth = required_uint32(document, "child_depth");
    impl->proposal_hash = proposal_hash;
    impl->tasks = std::move(tasks);
    impl->join = join;
    impl->aggregate_budget = aggregate;
    impl->template_receipts = std::move(receipts);
    impl->capability_effect_closure = closure;
    impl->source_map = std::move(source_map);
    impl->document = std::move(document);
    impl->canonical_bytes = canonical_json_bytes(impl->document);
    return CompiledTaskGraphFragment(std::move(impl));
}
namespace {

json task_record_to_json(const TaskGraphTaskRecord& task) {
    if (task.task_id.empty() || task.operation_id.empty())
        throw std::invalid_argument("Task graph task record requires task_id and operation_id");
    require_token(task.task_id, "Task graph task record task_id");
    require_token(task.operation_id, "Task graph task record operation_id");
    if (task.attempt_identity.empty() != (task.attempt == 0))
        throw std::invalid_argument(
            "Task graph task record attempt_identity must be present exactly when attempt is non-zero");
    if (!task.attempt_identity.empty())
        require_identity(task.attempt_identity, "Task graph task record attempt_identity");
    if (task.child_run_id) require_token(*task.child_run_id, "Task graph task record child_run_id");
    if (task.state == TaskGraphTaskState::Pending && (task.output || task.failure))
        throw std::invalid_argument(
            "Pending task graph task record cannot contain a terminal result");
    if (task.state == TaskGraphTaskState::Active &&
        (task.attempt == 0 || task.attempt_identity.empty()))
        throw std::invalid_argument(
            "Active task graph task record requires a bounded attempt identity");
    if (task.state == TaskGraphTaskState::Active && task.output)
        throw std::invalid_argument("Active task graph task record cannot contain output");
    if (task.output && task.failure)
        throw std::invalid_argument("Task graph task record cannot have output and failure");
    json result{{"task_id", task.task_id},
                {"operation_id", task.operation_id},
                {"state", to_string(task.state)},
                {"attempt", task.attempt}};
    if (task.output) result["output"] = owned_json_copy(*task.output);
    if (task.failure) result["failure"] = owned_json_copy(*task.failure);
    if (!task.attempt_identity.empty()) result["attempt_identity"] = task.attempt_identity;
    if (task.child_run_id) result["child_run_id"] = *task.child_run_id;
    return result;
}

TaskGraphTaskRecord task_record_from_json(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Stored task graph task record must be an object");
    detail::reject_unknown_fields(value, "Stored task graph task record",
                                  {"task_id", "operation_id", "state", "attempt", "output",
                                   "failure", "attempt_identity", "child_run_id"});
    TaskGraphTaskRecord result;
    result.task_id = required_string(value, "task_id");
    result.operation_id = required_string(value, "operation_id");
    result.state = task_graph_task_state_from_string(required_string(value, "state"));
    result.attempt = required_uint32(value, "attempt");
    if (value.contains("output")) result.output = owned_json_copy(value.at("output"));
    if (value.contains("failure")) result.failure = owned_json_copy(value.at("failure"));
    if (value.contains("attempt_identity"))
        result.attempt_identity = required_string(value, "attempt_identity");
    if (value.contains("child_run_id"))
        result.child_run_id = required_string(value, "child_run_id");
    require_token(result.task_id, "Stored task graph task record task_id");
    require_token(result.operation_id, "Stored task graph task record operation_id");
    if (result.output && result.failure)
        throw std::invalid_argument("Stored task graph task record cannot have output and failure");
    (void)task_record_to_json(result);
    return result;
}

} // namespace

json TaskGraphFragmentRecord::to_json() const {
    if (published && revision == 0)
        throw std::invalid_argument("Published task graph record must have a positive revision");
    if (terminal && !published)
        throw std::invalid_argument("Terminal task graph record must be published");
    json encoded_tasks = json::array();
    std::map<std::string, const CompiledTaskGraphTask*, std::less<>> compiled;
    for (const auto& compiled_task : fragment.tasks()) {
        if (!compiled.emplace(compiled_task.task_id, &compiled_task).second)
            throw std::invalid_argument("Task graph fragment has duplicate compiled task_id");
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& task : tasks) {
        if (!ids.insert(task.task_id).second)
            throw std::invalid_argument("Task graph fragment record has duplicate task_id");
        const auto found = compiled.find(task.task_id);
        if (found == compiled.end() || found->second->operation_id != task.operation_id)
            throw std::invalid_argument(
                "Task graph fragment record task identity does not match its fragment");
        encoded_tasks.push_back(task_record_to_json(task));
    }
    if (tasks.size() != compiled.size())
        throw std::invalid_argument("Task graph fragment record task set does not match fragment");
    return json{{"storage_schema_version", STORAGE_SCHEMA_VERSION},
                {"fragment", fragment.to_json()},
                {"tasks", std::move(encoded_tasks)},
                {"revision", revision},
                {"published", published},
                {"terminal", terminal}};
}

std::string TaskGraphFragmentRecord::serialize_canonical() const {
    return canonical_json_bytes(to_json());
}

TaskGraphFragmentRecord TaskGraphFragmentRecord::parse(std::string_view stored_bytes) {
    json document;
    try {
        document = json::parse(std::string(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument("Invalid stored task graph record JSON: " +
                                    std::string(error.what()));
    }
    if (!document.is_object())
        throw std::invalid_argument("Stored task graph record must be an object");
    detail::reject_unknown_fields(document, "Stored task graph record",
                                  {"storage_schema_version", "fragment", "tasks", "revision",
                                   "published", "terminal"});
    if (required_uint32(document, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Unsupported stored task graph record schema version");
    if (!document.contains("fragment") || !document.at("fragment").is_object())
        throw std::invalid_argument("Stored task graph record requires fragment");
    if (!document.contains("tasks") || !document.at("tasks").is_array())
        throw std::invalid_argument("Stored task graph record tasks must be an array");
    if (!document.contains("published") || !document.at("published").is_boolean())
        throw std::invalid_argument("Stored task graph record published must be boolean");
    if (!document.contains("terminal") || !document.at("terminal").is_boolean())
        throw std::invalid_argument("Stored task graph record terminal must be boolean");

    auto fragment = CompiledTaskGraphFragment::parse(
        canonical_json_bytes(document.at("fragment")));
    TaskGraphFragmentRecord result{std::move(fragment),
                                   {},
                                   required_uint64(document, "revision"),
                                   document.at("published").get<bool>(),
                                   document.at("terminal").get<bool>()};
    result.tasks.reserve(document.at("tasks").size());
    for (const auto& encoded : document.at("tasks"))
        result.tasks.push_back(task_record_from_json(encoded));

    std::map<std::string, const CompiledTaskGraphTask*, std::less<>> compiled_tasks;
    for (const auto& task : result.fragment.tasks()) compiled_tasks.emplace(task.task_id, &task);
    if (compiled_tasks.size() != result.fragment.tasks().size())
        throw std::invalid_argument("Stored task graph fragment has duplicate compiled task ids");
    if (result.tasks.size() != compiled_tasks.size())
        throw std::invalid_argument("Stored task graph record task set does not match fragment");
    for (const auto& task : result.tasks) {
        const auto found = compiled_tasks.find(task.task_id);
        if (found == compiled_tasks.end() ||
            task.operation_id != found->second->operation_id) {
            throw std::invalid_argument("Stored task graph record operation identity mismatch");
        }
    }
    if (result.published && result.revision == 0)
        throw std::invalid_argument("Published task graph record must have a positive revision");
    (void)result.to_json();
    return result;
}

CompiledTaskGraphFragment TaskGraphFragmentCompiler::compile(
    const json& proposal, TaskGraphProposalOptions proposal_options,
    const TaskGraphFragmentCompileOptions& options) {
    if (proposal_options.source_id.empty()) proposal_options.source_id = "task-graph-proposal";
    if (proposal_options.limits.max_tasks == 0) proposal_options.limits = options.limits;
    if (proposal_options.template_allowlist.empty())
        proposal_options.template_allowlist = options.template_allowlist;
    return compile(TaskGraphProposal::parse(proposal, std::move(proposal_options)), options);
}

CompiledTaskGraphFragment TaskGraphFragmentCompiler::compile(
    const TaskGraphProposal& proposal, const TaskGraphFragmentCompileOptions& options) {
    require_token(options.owner_scope, "Task graph owner_scope");
    require_token(options.parent_run_id, "Task graph parent_run_id");
    require_token(options.expansion_operation_id, "Task graph expansion_operation_id");
    require_token(options.parent_program_version_id, "Task graph parent_program_version_id");
    if (options.child_depth >= options.remaining_budget.max_child_depth) {
        throw std::invalid_argument("Task graph child depth exceeds the parent budget ceiling");
    }

    std::map<std::string, const TaskGraphTemplateContract*, std::less<>> templates;
    for (const auto& contract : options.template_allowlist) templates.emplace(contract.template_id, &contract);
    if (templates.size() != options.template_allowlist.size())
        throw std::invalid_argument("Task graph template allow-list contains duplicate template_id");

    const auto effective_capabilities = intersection(options.parent_capabilities,
                                                      options.host_capabilities);
    const auto effective_effects = intersection(options.parent_effects, options.host_effects);
    std::set<std::string, std::less<>> closure = effective_capabilities;
    closure.insert(effective_effects.begin(), effective_effects.end());

    std::vector<CompiledTaskGraphTask> tasks;
    tasks.reserve(proposal.tasks().size());
    std::vector<TaskGraphTemplateReceipt> receipts;
    std::set<std::string, std::less<>> seen_templates;
    TaskGraphBudget aggregate;
    std::uint64_t edge_count = 0;
    for (const auto& task : proposal.tasks()) {
        const auto found = templates.find(task.template_id);
        if (found == templates.end())
            throw std::invalid_argument("Task graph task selected a template outside the trusted allow-list");
        const auto& contract = *found->second;
        require_identity(contract.content_identity, "Task graph template content_identity");
        require_token(contract.child_binding, "Task graph template child_binding");
        if (!contract.executable_identity.empty())
            require_token(contract.executable_identity, "Task graph template executable_identity");
        if (!contract.kind.empty()) require_token(contract.kind, "Task graph template kind");
        if (!is_subset(contract.capabilities, effective_capabilities) ||
            !is_subset(contract.effects, effective_effects)) {
            throw std::invalid_argument(
                "Task graph template authority exceeds the parent/host capability intersection");
        }
        if (!budget_leq(task.budget, contract.budget_ceiling))
            throw std::invalid_argument("Task graph task budget exceeds its template ceiling");
        aggregate = add_budget(aggregate, task.budget);
        edge_count += task.depends_on.size();

        CompiledTaskGraphTask compiled;
        compiled.task_id = task.id;
        compiled.template_id = task.template_id;
        compiled.template_identity = contract.content_identity;
        compiled.child_binding = contract.child_binding;
        compiled.input_bindings = task.input_bindings;
        compiled.depends_on = task.depends_on;
        compiled.requested_budget = task.budget;
        compiled.reserved_budget = task.budget;
        compiled.capabilities = sorted_unique(contract.capabilities);
        compiled.effects = sorted_unique(contract.effects);
        compiled.input_fields = contract.input_fields;
        compiled.output_artifacts = contract.output_artifacts;
        tasks.push_back(std::move(compiled));

        if (seen_templates.insert(contract.template_id).second) {
            TaskGraphTemplateReceipt receipt;
            receipt.template_id = contract.template_id;
            receipt.content_identity = contract.content_identity;
            receipt.child_binding = contract.child_binding;
            receipt.executable_identity = contract.executable_identity;
            receipt.kind = contract.kind;
            receipt.capabilities = sorted_unique(contract.capabilities);
            receipt.effects = sorted_unique(contract.effects);
            receipt.input_fields = contract.input_fields;
            receipt.output_artifacts = contract.output_artifacts;
            receipts.push_back(std::move(receipt));
        }
    }
    if (edge_count > options.limits.max_edges)
        throw std::invalid_argument("Task graph edge count exceeds the compiler ceiling");
    if (!budget_leq(aggregate, options.limits.total_budget_ceiling))
        throw std::invalid_argument("Task graph total budget exceeds the compiler ceiling");
    if (!budget_leq(aggregate, budget_from_limits(options.remaining_budget)))
        throw std::invalid_argument("Task graph total budget exceeds the remaining run budget");
    if (options.remaining_budget.max_total_children != 0 &&
        tasks.size() > options.remaining_budget.max_total_children)
        throw std::invalid_argument("Task graph task count exceeds the remaining child ceiling");
    std::vector<std::string> closure_values(closure.begin(), closure.end());
    const auto identity_input = fragment_identity_json(options, proposal, tasks, proposal.join(),
                                                       receipts, closure_values, aggregate);
    const auto fragment_id = detail::sha256_identity(
        "program-task-graph-fragment/v1", canonical_json_bytes(identity_input));
    for (auto& task : tasks) task.operation_id = task_operation_id(fragment_id, task.task_id);
    auto document = identity_input;
    document["fragment_id"] = fragment_id;
    document["source_map"] = json::array();
    for (const auto& entry : proposal.source_map()) {
        json encoded;
        to_json(encoded, entry);
        document["source_map"].push_back(std::move(encoded));
    }
    document["tasks"] = json::array();
    for (const auto& task : tasks) document["tasks"].push_back(task_to_json(task));

    auto impl = std::make_shared<CompiledTaskGraphFragment::Impl>();
    impl->fragment_id = fragment_id;
    impl->owner_scope = options.owner_scope;
    impl->parent_run_id = options.parent_run_id;
    impl->expansion_operation_id = options.expansion_operation_id;
    impl->parent_program_version_id = options.parent_program_version_id;
    impl->child_depth = options.child_depth;
    impl->proposal_hash = proposal.proposal_hash();
    impl->tasks = std::move(tasks);
    impl->join = proposal.join();
    impl->aggregate_budget = aggregate;
    impl->template_receipts = std::move(receipts);
    impl->capability_effect_closure = std::move(closure_values);
    impl->source_map = proposal.source_map();
    impl->document = std::move(document);
    impl->canonical_bytes = canonical_json_bytes(impl->document);
    return CompiledTaskGraphFragment(std::move(impl));
}

std::string_view to_string(TaskGraphTaskState state) noexcept {
    switch (state) {
    case TaskGraphTaskState::Pending: return "pending";
    case TaskGraphTaskState::Active: return "active";
    case TaskGraphTaskState::Completed: return "completed";
    case TaskGraphTaskState::Failed: return "failed";
    case TaskGraphTaskState::Cancelled: return "cancelled";
    }
    return "unknown";
}

TaskGraphTaskState task_graph_task_state_from_string(std::string_view value) {
    if (value == "pending") return TaskGraphTaskState::Pending;
    if (value == "active") return TaskGraphTaskState::Active;
    if (value == "completed") return TaskGraphTaskState::Completed;
    if (value == "failed") return TaskGraphTaskState::Failed;
    if (value == "cancelled") return TaskGraphTaskState::Cancelled;
    throw std::invalid_argument("Unknown task graph task state: " + std::string(value));
}

std::string_view to_string(TaskGraphPublishResult result) noexcept {
    switch (result) {
    case TaskGraphPublishResult::Published: return "published";
    case TaskGraphPublishResult::AlreadyPresent: return "already_present";
    case TaskGraphPublishResult::Conflict: return "conflict";
    case TaskGraphPublishResult::Missing: return "missing";
    }
    return "unknown";
}

bool same_record_identity(const TaskGraphFragmentRecord& lhs,
                          const TaskGraphFragmentRecord& rhs) {
    if (!(lhs.fragment == rhs.fragment) || lhs.tasks.size() != rhs.tasks.size()) return false;
    std::map<std::string, std::string, std::less<>> left;
    std::map<std::string, std::string, std::less<>> right;
    for (const auto& task : lhs.tasks) {
        if (!left.emplace(task.task_id, task.operation_id).second) return false;
    }
    for (const auto& task : rhs.tasks) {
        if (!right.emplace(task.task_id, task.operation_id).second) return false;
    }
    return left == right;
}

struct InMemoryTaskGraphFragmentStore::Impl {
    mutable std::mutex mutex;
    std::map<std::string, TaskGraphFragmentRecord, std::less<>> records;
};

InMemoryTaskGraphFragmentStore::InMemoryTaskGraphFragmentStore()
    : impl_(std::make_unique<Impl>()) {}
InMemoryTaskGraphFragmentStore::InMemoryTaskGraphFragmentStore(
    InMemoryTaskGraphFragmentStore&&) noexcept = default;
InMemoryTaskGraphFragmentStore& InMemoryTaskGraphFragmentStore::operator=(
    InMemoryTaskGraphFragmentStore&&) noexcept = default;
InMemoryTaskGraphFragmentStore::~InMemoryTaskGraphFragmentStore() = default;

TaskGraphPublishResult InMemoryTaskGraphFragmentStore::publish(
    const TaskGraphFragmentRecord& record) {
    auto copy = TaskGraphFragmentRecord::parse(record.serialize_canonical());
    const auto id = copy.fragment.fragment_id();
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->records.find(id);
    if (found != impl_->records.end()) {
        if (same_record_identity(found->second, copy)) {
            return TaskGraphPublishResult::AlreadyPresent;
        }
        return TaskGraphPublishResult::Conflict;
    }
    copy.revision = 1;
    copy.published = true;
    impl_->records.emplace(id, std::move(copy));
    return TaskGraphPublishResult::Published;
}

std::optional<TaskGraphFragmentRecord> InMemoryTaskGraphFragmentStore::load(
    std::string_view fragment_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->records.find(std::string(fragment_id));
    if (found == impl_->records.end()) return std::nullopt;
    return found->second;
}
std::optional<TaskGraphFragmentRecord> InMemoryTaskGraphFragmentStore::find_published_lineage(
    std::string_view owner_scope,
    std::string_view parent_run_id,
    std::string_view expansion_operation_id) const {
    std::lock_guard lock(impl_->mutex);
    std::optional<TaskGraphFragmentRecord> result;
    for (const auto& [fragment_id, record] : impl_->records) {
        (void)fragment_id;
        const auto& fragment = record.fragment;
        if (!record.published || fragment.owner_scope() != owner_scope ||
            fragment.parent_run_id() != parent_run_id ||
            fragment.expansion_operation_id() != expansion_operation_id)
            continue;
        if (result) {
            throw std::runtime_error(
                "Multiple published task graph fragments share one parent operation lineage");
        }
        result = record;
    }
    return result;
}

std::optional<TaskGraphFragmentRecord> InMemoryTaskGraphFragmentStore::find_published(
    std::string_view owner_scope,
    std::string_view parent_run_id,
    std::string_view expansion_operation_id,
    std::string_view proposal_hash) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto& [fragment_id, record] : impl_->records) {
        (void)fragment_id;
        const auto& fragment = record.fragment;
        if (!record.published || fragment.owner_scope() != owner_scope ||
            fragment.parent_run_id() != parent_run_id ||
            fragment.expansion_operation_id() != expansion_operation_id ||
            fragment.proposal_hash() != proposal_hash)
            continue;
        return record;
    }
    return std::nullopt;
}
TaskGraphPublishResult InMemoryTaskGraphFragmentStore::compare_update(
    std::string_view fragment_id, std::uint64_t expected_revision,
    const TaskGraphFragmentRecord& record) {
    auto copy = TaskGraphFragmentRecord::parse(record.serialize_canonical());
    if (copy.fragment.fragment_id() != fragment_id) return TaskGraphPublishResult::Conflict;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->records.find(std::string(fragment_id));
    if (found == impl_->records.end()) return TaskGraphPublishResult::Missing;
    if (!same_record_identity(found->second, copy)) return TaskGraphPublishResult::Conflict;
    if (found->second.revision != expected_revision) return TaskGraphPublishResult::Conflict;
    if (expected_revision == std::numeric_limits<std::uint64_t>::max())
        return TaskGraphPublishResult::Conflict;
    copy.revision = expected_revision + 1;
    copy.published = true;
    found->second = std::move(copy);
    return TaskGraphPublishResult::Published;
}

} // namespace neograph::program
