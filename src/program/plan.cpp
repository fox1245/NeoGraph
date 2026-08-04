#include <neograph/program/plan.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::array<std::string_view, 15> kOperationNames = {
    "call_core", "sequence", "branch", "loop", "retry", "parallel", "race", "quorum",
    "map",       "spawn",    "await",  "emit", "checkpoint", "cancel", "return"};

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
        case ProgramOperationKind::Spawn: add({"body"}); break;
        case ProgramOperationKind::Await: add({"body", "timeout_ms"}); break;
        case ProgramOperationKind::Emit:
        case ProgramOperationKind::Return: add({"value"}); break;
        case ProgramOperationKind::Checkpoint: add({"body"}); break;
        case ProgramOperationKind::Cancel: add({"scope", "reason"}); break;
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
    std::vector<std::string>      branches;
    std::optional<std::uint64_t>  max_iterations;
    std::optional<std::uint64_t>  max_attempts;
    std::optional<std::uint64_t>  min_success;
    std::optional<std::uint64_t>  timeout_ms;
    std::optional<std::string>    scope;
    std::optional<std::string>    reason;
    json                          condition = json::object();
    json                          value     = json();
    json                          items     = json::array();
    bool                          has_value = false;
};

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
ProgramOperationKind ProgramPlanNode::operation() const noexcept { return impl_->data.operation; }
const ProgramPlanDispatchDescriptor& ProgramPlanNode::dispatch() const noexcept {
    return impl_->dispatch;
}
const ProgramPlanDispatchDescriptor& ProgramPlanNode::dispatch_descriptor() const noexcept {
    return dispatch();
}
const std::string& ProgramPlanNode::source_pointer() const noexcept {
    return impl_->data.source_pointer;
}
const std::optional<std::string>& ProgramPlanNode::core() const noexcept {
    return impl_->data.core;
}
const std::vector<std::string>& ProgramPlanNode::children() const noexcept {
    return impl_->data.children;
}
const std::optional<std::string>& ProgramPlanNode::then_id() const noexcept {
    return impl_->data.then_id;
}
const std::optional<std::string>& ProgramPlanNode::else_id() const noexcept {
    return impl_->data.else_id;
}
const std::optional<std::string>& ProgramPlanNode::body() const noexcept { return impl_->data.body; }
const std::vector<std::string>& ProgramPlanNode::branches() const noexcept {
    return impl_->data.branches;
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
        if (encoded.contains("max_iterations")) data.max_iterations = require_bound(encoded, "max_iterations");
        if (encoded.contains("max_attempts")) data.max_attempts = require_bound(encoded, "max_attempts");
        if (encoded.contains("min_success")) data.min_success = require_bound(encoded, "min_success");
        if (encoded.contains("timeout_ms")) data.timeout_ms = require_bound(encoded, "timeout_ms");
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
        } else if (op == ProgramOperationKind::Map || op == ProgramOperationKind::Spawn ||
                   op == ProgramOperationKind::Await) {
            if (!node.body()) throw std::invalid_argument("operation requires body");
            require_ref(*node.body(), node.id());
            if (op == ProgramOperationKind::Map && node.items().empty())
                throw std::invalid_argument("map requires nonempty items");
        } else if (op == ProgramOperationKind::Checkpoint) {
            if (node.body()) require_ref(*node.body(), node.id());
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

}  // namespace neograph::program
