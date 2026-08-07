#include <neograph/program/plan.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::array<std::string_view, 17> kOperationNames = {
    "call_core", "sequence", "branch", "loop", "retry", "parallel", "race", "quorum",
    "map",       "spawn",    "await",  "emit",  "checkpoint", "cancel", "return",
    "parallel_map", "expand_task_graph"};
std::string require_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty())
        throw std::invalid_argument("Program plan field '" + key + "' must be a nonempty string");
    return value.at(key).get<std::string>();
}

std::uint64_t require_bound(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned() ||
        value.at(key).get<std::uint64_t>() == 0)
        throw std::invalid_argument("Program plan field '" + key + "' must be positive");
    return value.at(key).get<std::uint64_t>();
}

std::vector<std::string> require_refs(const json& value, std::string_view field,
                                      std::size_t minimum) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_array() || value.at(key).size() < minimum)
        throw std::invalid_argument("Program plan field '" + key + "' has too few references");
    std::vector<std::string> result;
    result.reserve(value.at(key).size());
    for (const auto& item : value.at(key)) {
        if (!item.is_string() || item.get<std::string>().empty())
            throw std::invalid_argument("Program plan references must be nonempty strings");
        result.push_back(item.get<std::string>());
    }
    return result;
}

void reject_unknown_fields(const json& value, ProgramOperationKind operation) {
    std::set<std::string, std::less<>> allowed{"id", "op", "source_pointer"};
    const auto add = [&](std::initializer_list<std::string_view> fields) {
        for (const auto field : fields) allowed.emplace(field);
    };
    switch (operation) {
        case ProgramOperationKind::CallCore: add({"core"}); break;
        case ProgramOperationKind::Sequence: add({"children"}); break;
        case ProgramOperationKind::Branch: add({"condition", "then", "else"}); break;
        case ProgramOperationKind::Loop: add({"condition", "body", "max_iterations"}); break;
        case ProgramOperationKind::Retry: add({"body", "max_attempts"}); break;
        case ProgramOperationKind::Parallel:
        case ProgramOperationKind::Race: add({"branches"}); break;
        case ProgramOperationKind::Quorum: add({"branches", "min_success"}); break;
        case ProgramOperationKind::Map: add({"items", "body"}); break;
        case ProgramOperationKind::Spawn: add({"child_binding"}); break;
        case ProgramOperationKind::Await: add({"body", "timeout_ms"}); break;
        case ProgramOperationKind::Emit:
        case ProgramOperationKind::Return: add({"value"}); break;
        case ProgramOperationKind::Checkpoint: add({"body"}); break;
        case ProgramOperationKind::Cancel: add({"scope", "reason"}); break;
        case ProgramOperationKind::ParallelMap:
            add({"item_source", "child_binding", "input_binding", "output_binding",
                 "max_items", "max_in_flight", "max_output_bytes", "failure_policy"});
            break;
        case ProgramOperationKind::ExpandTaskGraph:
            add({"proposal_source", "max_tasks", "max_edges", "max_depth",
                 "max_dynamic_compiles", "max_total_children", "max_concurrency",
                 "failure_policy"});
            break;
    }
    for (const auto& [field, ignored] : value.items()) {
        (void)ignored;
        if (!allowed.contains(field))
            throw std::invalid_argument("Unknown field in Program plan operation: " + field);
    }
}

void validate_condition(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Program plan condition must be an object");
    static constexpr std::array<std::string_view, 6> predicates = {
        "equals", "not_equals", "exists", "all", "any", "not"};
    for (const auto& [field, ignored] : value.items()) {
        (void)ignored;
        if (field != "path" &&
            std::find(predicates.begin(), predicates.end(), field) == predicates.end())
            throw std::invalid_argument("Unknown field in Program plan condition: " + field);
    }
    std::size_t alternatives = 0;
    for (const auto predicate : predicates)
        if (value.contains(std::string(predicate))) ++alternatives;
    if (alternatives != 1)
        throw std::invalid_argument("Program condition requires exactly one predicate");
    if (value.contains("path") &&
        (!value["path"].is_string() || value["path"].get<std::string>().empty()))
        throw std::invalid_argument("Program condition path must be a nonempty string");
    if (value.contains("path")) detail::validate_json_pointer(value["path"].get<std::string>());
    if ((value.contains("equals") || value.contains("not_equals") || value.contains("exists")) &&
        !value.contains("path"))
        throw std::invalid_argument("Program condition comparison requires path");
    if (value.contains("exists") && !value["exists"].is_boolean())
        throw std::invalid_argument("Program condition exists must be boolean");
    for (const auto field : {"all", "any"}) {
        if (!value.contains(field)) continue;
        if (!value[field].is_array() || value[field].empty())
            throw std::invalid_argument("Program condition conjunction must be nonempty");
        for (const auto& child : value[field]) validate_condition(child);
    }
    if (value.contains("not")) validate_condition(value["not"]);
}

struct NodeData {
    std::string                   id;
    ProgramOperationKind          operation = ProgramOperationKind::CallCore;
    std::string                   source_pointer;
    std::optional<std::string>    core;
    std::vector<std::string>      children;
    std::optional<std::string>    then_id;
    std::optional<std::string>    else_id;
    std::optional<std::string>    body;
    std::optional<std::string>    child_binding;
    std::vector<std::string>      branches;
    std::optional<std::uint64_t>  max_iterations;
    std::optional<std::uint64_t>  max_attempts;
    std::optional<std::uint64_t>  min_success;
    std::optional<std::uint64_t>  timeout_ms;
    std::optional<std::string>    scope;
    std::optional<std::string>    reason;
    std::optional<ProgramParallelMapSpec>      parallel_map;
    std::optional<ProgramExpandTaskGraphSpec> expand_task_graph;
    json                          condition = json::object();
    json                          value     = json();
    json                          items     = json::array();
    bool                          has_value = false;
};

bool valid_json_pointer(std::string_view pointer) {
    if (pointer.empty()) return true;
    if (pointer.front() != '/') return false;
    for (std::size_t index = 0; index < pointer.size(); ++index) {
        if (pointer[index] != '~') continue;
        if (++index == pointer.size() || (pointer[index] != '0' && pointer[index] != '1'))
            return false;
    }
    return true;
}

std::string require_json_pointer(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string())
        throw std::invalid_argument("Program parallel_map field " + key + " must be a string");
    const auto pointer = value.at(key).get<std::string>();
    if (!valid_json_pointer(pointer))
        throw std::invalid_argument("Program parallel_map field " + key +
                                    " must be a JSON Pointer");
    return pointer;
}

void reject_unknown_object_fields(const json& value,
                                  std::initializer_list<std::string_view> allowed,
                                  std::string_view context) {
    if (!value.is_object())
        throw std::invalid_argument("Program parallel_map " + std::string(context) +
                                    " must be an object");
    for (const auto& [field, ignored] : value.items()) {
        if (std::find(allowed.begin(), allowed.end(), field) == allowed.end())
            throw std::invalid_argument("Unknown Program parallel_map " + std::string(context) +
                                        " field: " + field);
    }
}

ProgramParallelMapSpec parse_parallel_map_spec(const json& encoded) {
    constexpr std::uint64_t kMaxItems       = 1'000'000;
    constexpr std::uint64_t kMaxOutputBytes = 1ULL << 30;

    ProgramParallelMapSpec result;
    const auto& item_source = encoded.at("item_source");
    reject_unknown_object_fields(item_source, {"literal", "artifact", "field"}, "item_source");
    if (item_source.contains("literal")) {
        if (item_source.size() != 1 || !item_source.at("literal").is_array())
            throw std::invalid_argument(
                "Program parallel_map item_source.literal must be the only array source");
        result.item_source   = ProgramParallelMapItemSource::Literal;
        result.literal_items = item_source.at("literal");
    } else {
        if (item_source.size() != 2 || !item_source.contains("artifact") ||
            !item_source.contains("field") || require_string(item_source, "artifact") != "input")
            throw std::invalid_argument(
                "Program parallel_map item_source must be literal items or input artifact field");
        result.item_source = ProgramParallelMapItemSource::InputField;
        result.input_field = require_json_pointer(item_source, "field");
    }

    result.child_binding = require_string(encoded, "child_binding");
    if (result.child_binding.empty())
        throw std::invalid_argument("Program parallel_map child_binding must be nonempty");

    const auto parse_endpoint = [](const json& endpoint, std::string_view context) {
        reject_unknown_object_fields(endpoint, {"field"}, context);
        if (!endpoint.contains("field"))
            throw std::invalid_argument("Program parallel_map " + std::string(context) +
                                        " requires field");
        return require_json_pointer(endpoint, "field");
    };
    const auto& input_binding = encoded.at("input_binding");
    reject_unknown_object_fields(input_binding, {"from", "to"}, "input_binding");
    if (!input_binding.contains("from") || !input_binding.contains("to"))
        throw std::invalid_argument("Program parallel_map input_binding requires from and to");
    result.input_from_field = parse_endpoint(input_binding.at("from"), "input_binding.from");
    result.input_to_field   = parse_endpoint(input_binding.at("to"), "input_binding.to");

    const auto& output_binding = encoded.at("output_binding");
    reject_unknown_object_fields(output_binding, {"from"}, "output_binding");
    if (!output_binding.contains("from"))
        throw std::invalid_argument("Program parallel_map output_binding requires from");
    result.output_from_field = parse_endpoint(output_binding.at("from"), "output_binding.from");

    result.max_items = require_bound(encoded, "max_items");
    if (result.max_items > kMaxItems)
        throw std::invalid_argument("Program parallel_map max_items exceeds supported limit");
    if (result.item_source == ProgramParallelMapItemSource::Literal &&
        result.literal_items.size() > result.max_items)
        throw std::invalid_argument(
            "Program parallel_map literal item count exceeds max_items");
    const auto in_flight = require_bound(encoded, "max_in_flight");
    if (in_flight > result.max_items ||
        in_flight > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(
            "Program parallel_map max_in_flight exceeds max_items or supported limit");
    result.max_in_flight = static_cast<std::uint32_t>(in_flight);
    result.max_output_bytes = require_bound(encoded, "max_output_bytes");
    if (result.max_output_bytes > kMaxOutputBytes)
        throw std::invalid_argument("Program parallel_map max_output_bytes exceeds supported limit");

    const auto failure_policy = require_string(encoded, "failure_policy");
    if (failure_policy == "fail_fast") {
        result.failure_policy = ProgramParallelMapFailurePolicy::FailFast;
    } else if (failure_policy == "collect") {
        result.failure_policy = ProgramParallelMapFailurePolicy::Collect;
    } else {
        throw std::invalid_argument("Program parallel_map failure_policy is invalid");
    }
    return result;
}

ProgramExpandTaskGraphSpec parse_expand_task_graph_spec(const json& encoded) {
    ProgramExpandTaskGraphSpec result;
    if (!encoded.contains("proposal_source") || !encoded["proposal_source"].is_object())
        throw std::invalid_argument("expand_task_graph proposal_source must be an object");
    const auto& source = encoded["proposal_source"];
    if (source.contains("inline")) {
        reject_unknown_object_fields(source, {"inline"}, "proposal_source");
        if (!source["inline"].is_object())
            throw std::invalid_argument("expand_task_graph inline proposal must be an object");
        result.proposal_source = detail::owned_json_copy(source);
    } else {
        reject_unknown_object_fields(source, {"artifact", "field"}, "proposal_source");
        if (!source.contains("artifact") || require_string(source, "artifact") != "input")
            throw std::invalid_argument(
                "expand_task_graph proposal_source artifact must be 'input'");
        result.proposal_source = detail::owned_json_copy(source);
        (void)require_json_pointer(source, "field");
    }
    result.max_tasks = require_bound(encoded, "max_tasks");
    result.max_edges = require_bound(encoded, "max_edges");
    result.max_depth = require_bound(encoded, "max_depth");
    result.max_dynamic_compiles = require_bound(encoded, "max_dynamic_compiles");
    result.max_total_children = require_bound(encoded, "max_total_children");
    const auto max_concurrency = require_bound(encoded, "max_concurrency");
    if (max_concurrency > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("expand_task_graph max_concurrency exceeds uint32 range");
    result.max_concurrency = static_cast<std::uint32_t>(max_concurrency);
    const auto failure_policy = require_string(encoded, "failure_policy");
    if (failure_policy == "fail_fast") {
        result.failure_policy = ProgramTaskGraphFailurePolicy::FailFast;
    } else if (failure_policy == "collect") {
        result.failure_policy = ProgramTaskGraphFailurePolicy::Collect;
    } else {
        throw std::invalid_argument("expand_task_graph failure_policy is invalid");
    }
    return result;
}

}  // namespace

std::string_view to_string(ProgramOperationKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kOperationNames.size() ? kOperationNames[index] : "unknown";
}

ProgramOperationKind program_operation_kind_from_string(std::string_view value) {
    const auto found = std::find(kOperationNames.begin(), kOperationNames.end(), value);
    if (found == kOperationNames.end())
        throw std::invalid_argument("Unknown Program operation: " + std::string(value));
    return static_cast<ProgramOperationKind>(std::distance(kOperationNames.begin(), found));
}

struct ProgramPlanNode::Impl {
    NodeData                     data;
    ProgramPlanDispatchDescriptor dispatch;
};

ProgramPlanNode::ProgramPlanNode(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
ProgramPlanNode::~ProgramPlanNode() = default;
const std::string& ProgramPlanNode::id() const noexcept { return impl_->data.id; }
ProgramOperationKind ProgramPlanNode::operation() const noexcept { return impl_->dispatch.operation; }
const ProgramPlanDispatchDescriptor& ProgramPlanNode::dispatch() const noexcept {
    return impl_->dispatch;
}
const ProgramPlanDispatchDescriptor& ProgramPlanNode::dispatch_descriptor() const noexcept {
    return dispatch();
}
const std::string& ProgramPlanNode::source_pointer() const noexcept {
    return impl_->dispatch.source_pointer;
}
const std::optional<std::string>& ProgramPlanNode::core() const noexcept {
    return impl_->data.core;
}
const std::vector<std::string>& ProgramPlanNode::children() const noexcept {
    return impl_->dispatch.children;
}
const std::optional<std::string>& ProgramPlanNode::then_id() const noexcept {
    return impl_->dispatch.then_id;
}
const std::optional<std::string>& ProgramPlanNode::else_id() const noexcept {
    return impl_->dispatch.else_id;
}
const std::optional<std::string>& ProgramPlanNode::child_binding() const noexcept {
    return impl_->dispatch.child_binding;
}
const std::optional<std::string>& ProgramPlanNode::body() const noexcept {
    return impl_->dispatch.body;
}
const std::vector<std::string>& ProgramPlanNode::branches() const noexcept {
    return impl_->dispatch.branches;
}
const std::optional<std::uint64_t>& ProgramPlanNode::max_iterations() const noexcept {
    return impl_->data.max_iterations;
}
const std::optional<std::uint64_t>& ProgramPlanNode::max_attempts() const noexcept {
    return impl_->data.max_attempts;
}
const std::optional<std::uint64_t>& ProgramPlanNode::min_success() const noexcept {
    return impl_->data.min_success;
}
const std::optional<std::uint64_t>& ProgramPlanNode::timeout_ms() const noexcept {
    return impl_->data.timeout_ms;
}
const std::optional<std::string>& ProgramPlanNode::scope() const noexcept {
    return impl_->data.scope;
}
const std::optional<ProgramParallelMapSpec>& ProgramPlanNode::parallel_map() const noexcept {
    return impl_->data.parallel_map;
}
const std::optional<ProgramExpandTaskGraphSpec>&
ProgramPlanNode::expand_task_graph() const noexcept {
    return impl_->data.expand_task_graph;
}
const std::optional<std::string>& ProgramPlanNode::reason() const noexcept {
    return impl_->data.reason;
}
json ProgramPlanNode::condition() const { return detail::owned_json_copy(impl_->data.condition); }
json ProgramPlanNode::value() const { return detail::owned_json_copy(impl_->data.value); }
json ProgramPlanNode::items() const { return detail::owned_json_copy(impl_->data.items); }

json ProgramPlanNode::to_json() const {
    const auto& data = impl_->data;
    json result{{"id", data.id}, {"op", std::string(to_string(data.operation))},
                {"source_pointer", data.source_pointer}};
    if (data.core) result["core"] = *data.core;
    if (!data.children.empty()) result["children"] = data.children;
    if (data.then_id) result["then"] = *data.then_id;
    if (data.else_id) result["else"] = *data.else_id;
    if (data.body) result["body"] = *data.body;
    if (data.child_binding) result["child_binding"] = *data.child_binding;
    if (!data.branches.empty()) result["branches"] = data.branches;
    if (data.max_iterations) result["max_iterations"] = *data.max_iterations;
    if (data.max_attempts) result["max_attempts"] = *data.max_attempts;
    if (data.min_success) result["min_success"] = *data.min_success;
    if (data.timeout_ms) result["timeout_ms"] = *data.timeout_ms;
    if (data.scope) result["scope"] = *data.scope;
    if (data.reason) result["reason"] = *data.reason;
    if (!data.condition.empty() && data.operation == ProgramOperationKind::Branch)
        result["condition"] = data.condition;
    if (!data.condition.empty() && data.operation == ProgramOperationKind::Loop)
        result["condition"] = data.condition;
    if (data.has_value && (data.operation == ProgramOperationKind::Emit ||
                           data.operation == ProgramOperationKind::Return))
        result["value"] = data.value;
    if (!data.items.empty() && data.operation == ProgramOperationKind::Map)
        result["items"] = data.items;
    if (data.parallel_map) {
        const auto& map = *data.parallel_map;
        if (map.item_source == ProgramParallelMapItemSource::Literal) {
            result["item_source"] = json{{"literal", map.literal_items}};
        } else {
            result["item_source"] = json{{"artifact", "input"}, {"field", map.input_field}};
        }
        result["child_binding"] = map.child_binding;
        result["input_binding"] =
            json{{"from", json{{"field", map.input_from_field}}},
                 {"to", json{{"field", map.input_to_field}}}};
        result["output_binding"] = json{{"from", json{{"field", map.output_from_field}}}};
        result["max_items"] = map.max_items;
        result["max_in_flight"] = map.max_in_flight;
        result["max_output_bytes"] = map.max_output_bytes;
        result["failure_policy"] =
            map.failure_policy == ProgramParallelMapFailurePolicy::FailFast ? "fail_fast" : "collect";
    }
    if (data.expand_task_graph) {
        const auto& expand = *data.expand_task_graph;
        result["proposal_source"] = expand.proposal_source;
        result["max_tasks"] = expand.max_tasks;
        result["max_edges"] = expand.max_edges;
        result["max_depth"] = expand.max_depth;
        result["max_dynamic_compiles"] = expand.max_dynamic_compiles;
        result["max_total_children"] = expand.max_total_children;
        result["max_concurrency"] = expand.max_concurrency;
        result["failure_policy"] =
            expand.failure_policy == ProgramTaskGraphFailurePolicy::FailFast ? "fail_fast" : "collect";
    }
    return result;
}

struct ProgramPlan::Impl {
    std::uint32_t                schema_version = SCHEMA_VERSION;
    std::string                  root_id;
    std::vector<ProgramPlanNode> nodes;
    std::map<std::string, std::size_t, std::less<>> index;
};

ProgramPlan::ProgramPlan(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
ProgramPlan::~ProgramPlan() = default;

ProgramPlan ProgramPlan::from_json(const json& plan) {
    if (!plan.is_object() || !plan.contains("root") || !plan["root"].is_string() ||
        !plan.contains("operations") || !plan["operations"].is_array())
        throw std::invalid_argument("Program plan envelope is malformed");
    for (const auto& [field, ignored] : plan.items()) {
        (void)ignored;
        if (field != "root" && field != "operations")
            throw std::invalid_argument("Unknown field in Program plan envelope: " + field);
    }
    auto impl = std::make_shared<Impl>();
    impl->root_id = require_string(plan, "root");
    for (const auto& encoded : plan["operations"]) {
        if (!encoded.is_object()) throw std::invalid_argument("Program plan operation is not an object");
        NodeData data;
        data.id = require_string(encoded, "id");
        data.operation = program_operation_kind_from_string(require_string(encoded, "op"));
        reject_unknown_fields(encoded, data.operation);
        data.source_pointer = require_string(encoded, "source_pointer");
        if (data.source_pointer.front() != '/')
            throw std::invalid_argument("Program plan source_pointer must be a JSON pointer");
        detail::validate_json_pointer(data.source_pointer);

        if (encoded.contains("core")) data.core = require_string(encoded, "core");
        if (encoded.contains("children")) data.children = require_refs(encoded, "children", 1);
        if (encoded.contains("then")) data.then_id = require_string(encoded, "then");
        if (encoded.contains("else")) data.else_id = require_string(encoded, "else");
        if (encoded.contains("body")) data.body = require_string(encoded, "body");
        if (encoded.contains("child_binding"))
            data.child_binding = require_string(encoded, "child_binding");
        if (encoded.contains("branches")) data.branches = require_refs(encoded, "branches", 2);
        if (encoded.contains("condition")) {
            if (!encoded["condition"].is_object())
                throw std::invalid_argument("Program plan condition must be an object");
            data.condition = detail::owned_json_copy(encoded["condition"]);
        }
        if (encoded.contains("value")) {
            data.has_value = true;
            data.value = detail::owned_json_copy(encoded["value"]);
        }
        if (encoded.contains("items")) {
            if (!encoded["items"].is_array() || encoded["items"].empty())
                throw std::invalid_argument("Program plan items must be a nonempty array");
            data.items = detail::owned_json_copy(encoded["items"]);
        }
        if (encoded.contains("max_iterations"))
            data.max_iterations = require_bound(encoded, "max_iterations");
        if (encoded.contains("max_attempts"))
            data.max_attempts = require_bound(encoded, "max_attempts");
        if (encoded.contains("min_success"))
            data.min_success = require_bound(encoded, "min_success");
        if (encoded.contains("timeout_ms"))
            data.timeout_ms = require_bound(encoded, "timeout_ms");
        if (data.operation == ProgramOperationKind::ParallelMap)
            data.parallel_map = parse_parallel_map_spec(encoded);
        if (data.operation == ProgramOperationKind::ExpandTaskGraph)
            data.expand_task_graph = parse_expand_task_graph_spec(encoded);
        if (encoded.contains("scope")) data.scope = require_string(encoded, "scope");
        if (encoded.contains("reason")) data.reason = require_string(encoded, "reason");
        if (impl->index.contains(data.id))
            throw std::invalid_argument("Program plan operation id is duplicated: " + data.id);
        impl->index.emplace(data.id, impl->nodes.size());
        auto node_impl = std::make_shared<ProgramPlanNode::Impl>();
        node_impl->dispatch.operation      = data.operation;
        node_impl->dispatch.source_pointer = data.source_pointer;
        node_impl->dispatch.children       = data.children;
        node_impl->dispatch.then_id        = data.then_id;
        node_impl->dispatch.else_id        = data.else_id;
        node_impl->dispatch.body           = data.body;
        node_impl->dispatch.child_binding  = data.child_binding;
        node_impl->dispatch.branches       = data.branches;
        node_impl->data = std::move(data);
        impl->nodes.emplace_back(ProgramPlanNode(std::move(node_impl)));
    }
    if (!impl->index.contains(impl->root_id))
        throw std::invalid_argument("Program plan root operation is missing");

    const auto require_ref = [&](const std::string& id, const std::string& field) {
        if (!impl->index.contains(id))
            throw std::invalid_argument("Program plan reference '" + id + "' in " + field +
                                        " is dangling");
    };
    for (const auto& node : impl->nodes) {
        const auto op = node.operation();
        if (op == ProgramOperationKind::CallCore) {
            if (!node.core()) throw std::invalid_argument("call_core requires a core reference");
        } else if (op == ProgramOperationKind::Sequence) {
            if (node.children().empty())
                throw std::invalid_argument("sequence requires at least one child");
            for (const auto& child : node.children()) require_ref(child, node.id());
        } else if (op == ProgramOperationKind::Branch) {
            if (!node.condition().is_object() || !node.then_id())
                throw std::invalid_argument("branch requires condition and then reference");
            validate_condition(node.condition());
            require_ref(*node.then_id(), node.id());
            if (node.else_id()) require_ref(*node.else_id(), node.id());
        } else if (op == ProgramOperationKind::Loop) {
            if (!node.condition().is_object() || !node.body() || !node.max_iterations())
                throw std::invalid_argument("loop requires condition, body, and max_iterations");
            validate_condition(node.condition());
            require_ref(*node.body(), node.id());
        } else if (op == ProgramOperationKind::Retry) {
            if (!node.body() || !node.max_attempts())
                throw std::invalid_argument("retry requires body and max_attempts");
            require_ref(*node.body(), node.id());
        } else if (op == ProgramOperationKind::Parallel || op == ProgramOperationKind::Race ||
                   op == ProgramOperationKind::Quorum) {
            if (node.branches().size() < 2)
                throw std::invalid_argument("parallel family requires at least two branches");
            for (const auto& child : node.branches()) require_ref(child, node.id());
            if (op == ProgramOperationKind::Race && node.branches().size() != 2)
                throw std::invalid_argument("race requires exactly two branches");
            if (op == ProgramOperationKind::Quorum &&
                (!node.min_success() || *node.min_success() > node.branches().size()))
                throw std::invalid_argument("quorum min_success exceeds branch count");
        } else if (op == ProgramOperationKind::Map || op == ProgramOperationKind::Await) {
            if (!node.body()) throw std::invalid_argument("operation requires body");
            require_ref(*node.body(), node.id());
            if (op == ProgramOperationKind::Map && node.items().empty())
                throw std::invalid_argument("map requires nonempty items");
        } else if (op == ProgramOperationKind::ParallelMap) {
            if (!node.parallel_map())
                throw std::invalid_argument("parallel_map requires a complete bounded map contract");
        } else if (op == ProgramOperationKind::ExpandTaskGraph) {
            if (!node.expand_task_graph())
                throw std::invalid_argument(
                    "expand_task_graph requires a complete bounded expansion contract");
        } else if (op == ProgramOperationKind::Spawn) {
            if (!node.child_binding())
                throw std::invalid_argument("spawn requires a verified child_binding");
            if (node.body()) require_ref(*node.body(), node.id());
        } else if (op == ProgramOperationKind::Checkpoint) {
            if (node.body()) require_ref(*node.body(), node.id());
        } else if (op == ProgramOperationKind::Cancel) {
            if (node.scope() && *node.scope() != "run")
                throw std::invalid_argument(
                    "Program cancel currently supports only the run scope");
        } else if (op == ProgramOperationKind::Emit || op == ProgramOperationKind::Return) {
            if (!node.to_json().contains("value"))
                throw std::invalid_argument("value operation requires value");
        }
    }
    return ProgramPlan(std::move(impl));
}

std::uint32_t ProgramPlan::schema_version() const noexcept { return impl_->schema_version; }
const std::string& ProgramPlan::root_id() const noexcept { return impl_->root_id; }
const std::vector<ProgramPlanNode>& ProgramPlan::nodes() const noexcept { return impl_->nodes; }
const ProgramPlanNode& ProgramPlan::root() const { return impl_->nodes.at(impl_->index.at(impl_->root_id)); }
const ProgramPlanNode* ProgramPlan::find(std::string_view id) const noexcept {
    const auto found = impl_->index.find(id);
    return found == impl_->index.end() ? nullptr : &impl_->nodes[found->second];
}
json ProgramPlan::to_json() const {
    json result{{"root", impl_->root_id}, {"operations", json::array()}};
    for (const auto& node : impl_->nodes) result["operations"].push_back(node.to_json());
    return result;
}

namespace {

constexpr const char* kStaticBudgetOverflow =
    "Program static budget requirements exceed the supported integer range";

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        throw std::overflow_error(kStaticBudgetOverflow);
    return left + right;
}

std::uint64_t checked_multiply(std::uint64_t value, std::uint64_t multiplier) {
    if (value != 0 && multiplier > std::numeric_limits<std::uint64_t>::max() / value)
        throw std::overflow_error(kStaticBudgetOverflow);
    return value * multiplier;
}

std::uint32_t checked_concurrency(std::uint64_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error(kStaticBudgetOverflow);
    return static_cast<std::uint32_t>(value);
}

ProgramStaticBudgetRequirements add_requirements(
    ProgramStaticBudgetRequirements left, const ProgramStaticBudgetRequirements& right) {
    left.max_program_operations =
        checked_add(left.max_program_operations, right.max_program_operations);
    left.max_core_steps = checked_add(left.max_core_steps, right.max_core_steps);
    left.max_core_invocations =
        checked_add(left.max_core_invocations, right.max_core_invocations);
    left.max_total_children = checked_add(left.max_total_children, right.max_total_children);
    left.max_dynamic_compiles =
        checked_add(left.max_dynamic_compiles, right.max_dynamic_compiles);
    left.max_concurrency =
        checked_concurrency(static_cast<std::uint64_t>(left.max_concurrency) + right.max_concurrency);
    left.max_child_depth = std::max(left.max_child_depth, right.max_child_depth);
    return left;
}

ProgramStaticBudgetRequirements scale_requirements(ProgramStaticBudgetRequirements value,
                                                   std::uint64_t multiplier) {
    value.max_program_operations = checked_multiply(value.max_program_operations, multiplier);
    value.max_core_steps         = checked_multiply(value.max_core_steps, multiplier);
    value.max_core_invocations   = checked_multiply(value.max_core_invocations, multiplier);
    value.max_total_children     = checked_multiply(value.max_total_children, multiplier);
    value.max_dynamic_compiles   = checked_multiply(value.max_dynamic_compiles, multiplier);
    // Loop/retry/map execute their bodies serially.
    return value;
}

const ProgramPlanNode& require_target(const ProgramPlan& plan, const std::optional<std::string>& id,
                                      std::string_view field) {
    if (!id) throw std::invalid_argument("Program plan operation is missing " + std::string(field));
    const auto* target = plan.find(*id);
    if (!target)
        throw std::invalid_argument("Program plan operation references an unknown " +
                                    std::string(field));
    return *target;
}

class StaticBudgetDeriver final {
public:
    explicit StaticBudgetDeriver(const ProgramPlan& plan) : plan_(plan) {}

    ProgramStaticBudgetRequirements derive() { return visit(plan_.root()); }

private:
    const ProgramPlan& plan_;
    std::set<std::string_view, std::less<>> active_;

    ProgramStaticBudgetRequirements visit(const ProgramPlanNode& operation) {
        if (!active_.emplace(operation.id()).second)
            throw std::invalid_argument("Program plan operation graph contains a cycle");
        try {
            auto result = derive(operation);
            active_.erase(operation.id());
            return result;
        } catch (...) {
            active_.erase(operation.id());
            throw;
        }
    }

    ProgramStaticBudgetRequirements derive(const ProgramPlanNode& operation) {
        using Kind = ProgramOperationKind;
        const auto one = ProgramStaticBudgetRequirements{1, 1, 0, 0, 0, 0, 0};
        const auto append_serial = [](ProgramStaticBudgetRequirements& result,
                                      const ProgramStaticBudgetRequirements& nested) {
            result.max_program_operations =
                checked_add(result.max_program_operations, nested.max_program_operations);
            result.max_core_steps = checked_add(result.max_core_steps, nested.max_core_steps);
            result.max_core_invocations =
                checked_add(result.max_core_invocations, nested.max_core_invocations);
            result.max_total_children =
                checked_add(result.max_total_children, nested.max_total_children);
            result.max_dynamic_compiles =
                checked_add(result.max_dynamic_compiles, nested.max_dynamic_compiles);
            result.max_child_depth = std::max(result.max_child_depth, nested.max_child_depth);
            result.max_concurrency = std::max(result.max_concurrency, nested.max_concurrency);
        };
        switch (operation.operation()) {
        case Kind::CallCore:
            return ProgramStaticBudgetRequirements{1, 1, 1, 1, 0, 0, 0};
        case Kind::Emit:
        case Kind::Cancel:
        case Kind::Return:
            return one;
        case Kind::Spawn:
            return ProgramStaticBudgetRequirements{1, 1, 0, 0, 1, 1, 0};
        case Kind::ParallelMap: {
            const auto& map = operation.parallel_map();
            if (!map)
                throw std::invalid_argument("parallel_map has no bounded map contract");
            return ProgramStaticBudgetRequirements{1, map->max_in_flight, 0, 0, 1,
                                                   map->max_items, 0};
        }
        case Kind::ExpandTaskGraph: {
            const auto& expand = operation.expand_task_graph();
            if (!expand)
                throw std::invalid_argument(
                    "expand_task_graph has no bounded expansion contract");
            return ProgramStaticBudgetRequirements{
                1,
                expand->max_concurrency,
                0,
                0,
                checked_concurrency(expand->max_depth),
                expand->max_total_children,
                expand->max_dynamic_compiles};
        }
        case Kind::Sequence: {
            auto result = one;
            for (const auto& child : operation.children()) {
                const auto* target = plan_.find(child);
                if (!target)
                    throw std::invalid_argument("Program plan sequence has an unknown child");
                append_serial(result, visit(*target));
            }
            return result;
        }
        case Kind::Branch: {
            auto selected = visit(require_target(plan_, operation.then_id(), "then"));
            if (operation.else_id()) {
                const auto alternative =
                    visit(require_target(plan_, operation.else_id(), "else"));
                selected.max_program_operations =
                    std::max(selected.max_program_operations, alternative.max_program_operations);
                selected.max_concurrency =
                    std::max(selected.max_concurrency, alternative.max_concurrency);
                selected.max_core_steps =
                    std::max(selected.max_core_steps, alternative.max_core_steps);
                selected.max_core_invocations =
                    std::max(selected.max_core_invocations, alternative.max_core_invocations);
                selected.max_child_depth =
                    std::max(selected.max_child_depth, alternative.max_child_depth);
                selected.max_total_children =
                    std::max(selected.max_total_children, alternative.max_total_children);
                selected.max_dynamic_compiles =
                    std::max(selected.max_dynamic_compiles, alternative.max_dynamic_compiles);
            }
            selected.max_program_operations =
                checked_add(one.max_program_operations, selected.max_program_operations);
            selected.max_concurrency = std::max(one.max_concurrency, selected.max_concurrency);
            return selected;
        }
        case Kind::Loop:
        case Kind::Retry:
        case Kind::Map: {
            const auto multiplier =
                operation.operation() == Kind::Loop ? operation.max_iterations()
                : operation.operation() == Kind::Retry ? operation.max_attempts()
                                                       : std::optional<std::uint64_t>{
                                                             operation.items().size()};
            if (!multiplier || *multiplier == 0)
                throw std::invalid_argument("Program bounded operation has no positive bound");
            auto result = scale_requirements(
                visit(require_target(plan_, operation.body(), "body")), *multiplier);
            result.max_program_operations =
                checked_add(one.max_program_operations, result.max_program_operations);
            result.max_concurrency = std::max(one.max_concurrency, result.max_concurrency);
            return result;
        }
        case Kind::Parallel:
        case Kind::Race: {
            auto result = one;
            result.max_concurrency = 0;
            for (const auto& branch : operation.branches()) {
                const auto* target = plan_.find(branch);
                if (!target)
                    throw std::invalid_argument("Program plan fan-out has an unknown branch");
                result = add_requirements(result, visit(*target));
            }
            result.max_concurrency =
                std::max(result.max_concurrency, checked_concurrency(operation.branches().size()));
            return result;
        }
        case Kind::Quorum: {
            auto result = one;
            for (const auto& branch : operation.branches()) {
                const auto* target = plan_.find(branch);
                if (!target)
                    throw std::invalid_argument("Program plan quorum has an unknown branch");
                append_serial(result, visit(*target));
            }
            return result;
        }
        case Kind::Await:
        case Kind::Checkpoint: {
            auto result = one;
            if (operation.body())
                append_serial(result, visit(require_target(plan_, operation.body(), "body")));
            return result;
        }
        }
        throw std::invalid_argument("Program plan operation kind is unsupported");
    }
};

}  // namespace

ProgramStaticBudgetRequirements derive_static_budget_requirements(const ProgramPlan& plan) {
    return StaticBudgetDeriver(plan).derive();
}

}  // namespace neograph::program
