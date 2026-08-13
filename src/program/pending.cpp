#include <neograph/program/pending.h>

#include "canonical_json.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

constexpr std::string_view INPUT_FORMAT  = "neograph-program-pending-input";
constexpr std::string_view EFFECT_FORMAT = "neograph-program-pending-effect";
constexpr std::size_t      MAX_SCHEMA_DEPTH = 64;

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program pending field '" + owned_key + "' must be a string");
    }
    auto result = value[owned_key].get<std::string>();
    detail::validate_utf8(result);
    return result;
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Program pending value requires field '" + owned_key + "'");
    }
    return value[owned_key];
}

std::uint64_t require_u64(const json& value, std::string_view key) {
    const auto& item = require_value(value, key);
    if (!item.is_number_unsigned()) {
        throw std::invalid_argument("Program pending field '" + std::string(key) +
                                    "' must be unsigned");
    }
    return item.get<unsigned long long>();
}

void require_object(const json& value, std::string_view name) {
    if (!value.is_object()) throw std::invalid_argument(std::string(name) + " must be an object");
}

void check_schema_depth(std::size_t depth, std::string_view path) {
    if (depth > MAX_SCHEMA_DEPTH) {
        throw std::invalid_argument("Program pending JSON Schema exceeds 64 levels at " +
                                    std::string(path));
    }
}

bool schema_type_matches(const json& value, std::string_view type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "string") return value.is_string();
    throw std::invalid_argument("Unsupported Program pending JSON Schema type: " +
                                std::string(type));
}

void validate_type_name(const json& type, std::string_view path) {
    if (!type.is_string()) {
        throw std::invalid_argument("Program pending JSON Schema type at " + std::string(path) +
                                    " must contain strings");
    }
    static const std::vector<std::string> supported = {
        "null", "boolean", "object", "array", "number", "integer", "string"};
    const auto name = type.get<std::string>();
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
        throw std::invalid_argument("Unsupported Program pending JSON Schema type: " + name);
    }
}

void validate_schema(const json& schema, const std::string& path, std::size_t depth) {
    check_schema_depth(depth, path);
    if (!schema.is_object()) {
        throw std::invalid_argument("Program pending JSON Schema at " + path +
                                    " must be an object");
    }
    if (schema.contains("type")) {
        const auto& type = schema["type"];
        if (type.is_string())
            validate_type_name(type, path);
        else if (type.is_array()) {
            if (type.empty()) {
                throw std::invalid_argument("Program pending JSON Schema type array at " + path +
                                            " must not be empty");
            }
            for (const auto& item : type) validate_type_name(item, path);
        } else {
            throw std::invalid_argument("Program pending JSON Schema type at " + path +
                                        " must be a string or array");
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        throw std::invalid_argument("Program pending JSON Schema enum at " + path +
                                    " must be an array");
    }
    if (schema.contains("required")) {
        const auto& required = schema["required"];
        if (!required.is_array()) {
            throw std::invalid_argument("Program pending JSON Schema required at " + path +
                                        " must be an array");
        }
        for (const auto& item : required) {
            if (!item.is_string()) {
                throw std::invalid_argument("Program pending JSON Schema required at " + path +
                                            " must contain strings");
            }
        }
    }
    if (schema.contains("properties")) {
        const auto& properties = schema["properties"];
        if (!properties.is_object()) {
            throw std::invalid_argument("Program pending JSON Schema properties at " + path +
                                        " must be an object");
        }
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            validate_schema(it.value(), path + "/properties/" + it.key(), depth + 1);
        }
    }
    if (schema.contains("items")) validate_schema(schema["items"], path + "/items", depth + 1);
    if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (!additional.is_boolean() && !additional.is_object()) {
            throw std::invalid_argument("Program pending JSON Schema additionalProperties at " +
                                        path + " must be a boolean or object");
        }
        if (additional.is_object()) {
            validate_schema(additional, path + "/additionalProperties", depth + 1);
        }
    }
    for (const auto* keyword : {"oneOf", "anyOf", "allOf"}) {
        if (!schema.contains(keyword)) continue;
        const auto& alternatives = schema[keyword];
        if (!alternatives.is_array() || alternatives.empty()) {
            throw std::invalid_argument("Program pending JSON Schema " + std::string(keyword) +
                                        " at " + path + " must be a non-empty array");
        }
        for (std::size_t index = 0; index < alternatives.size(); ++index) {
            validate_schema(alternatives[index],
                            path + "/" + keyword + "/" + std::to_string(index), depth + 1);
        }
    }
}

void validate_instance(const json& value, const json& schema, const std::string& path,
                       std::size_t depth) {
    check_schema_depth(depth, path);
    if (schema.contains("const") && value != schema["const"]) {
        throw std::invalid_argument("value at " + path + " does not match const");
    }
    if (schema.contains("enum")) {
        bool matched = false;
        for (const auto& candidate : schema["enum"]) {
            if (candidate == value) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::invalid_argument("value at " + path + " is not in enum");
        }
    }
    if (schema.contains("type")) {
        bool        matched = false;
        const auto& types   = schema["type"];
        if (types.is_string()) {
            matched = schema_type_matches(value, types.get<std::string>());
        } else {
            for (const auto& type : types) {
                if (schema_type_matches(value, type.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) throw std::invalid_argument("value at " + path + " has the wrong JSON type");
    }
    const auto matches_subschema = [&](const json& candidate) {
        try {
            validate_instance(value, candidate, path, depth + 1);
            return true;
        } catch (const std::invalid_argument&) {
            return false;
        }
    };
    if (schema.contains("oneOf")) {
        std::size_t matches = 0;
        for (const auto& candidate : schema["oneOf"]) matches += matches_subschema(candidate) ? 1 : 0;
        if (matches != 1) {
            throw std::invalid_argument("value at " + path +
                                        " does not match exactly one oneOf branch");
        }
    }
    if (schema.contains("anyOf")) {
        bool matched = false;
        for (const auto& candidate : schema["anyOf"]) matched = matched || matches_subschema(candidate);
        if (!matched) throw std::invalid_argument("value at " + path + " does not match any anyOf branch");
    }
    if (schema.contains("allOf")) {
        for (const auto& candidate : schema["allOf"]) validate_instance(value, candidate, path, depth + 1);
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& required : schema["required"]) {
                const auto name = required.get<std::string>();
                if (!value.contains(name)) {
                    throw std::invalid_argument("value at " + path +
                                                " is missing required property " + name);
                }
            }
        }
        const auto properties = schema.value("properties", json::object());
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (value.contains(it.key())) {
                validate_instance(value[it.key()], it.value(), path + "/" + it.key(), depth + 1);
            }
        }
        if (schema.contains("additionalProperties")) {
            const auto& additional = schema["additionalProperties"];
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (properties.contains(it.key())) continue;
                if (additional.is_boolean() && !additional.get<bool>()) {
                    throw std::invalid_argument("value at " + path +
                                                " has unexpected property " + it.key());
                }
                if (additional.is_object()) {
                    validate_instance(it.value(), additional, path + "/" + it.key(), depth + 1);
                }
            }
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            validate_instance(value[index], schema["items"], path + "/" + std::to_string(index),
                              depth + 1);
        }
    }
}

bool value_matches_schema(const json& value, const json& schema, std::string& error) {
    try {
        validate_instance(value, schema, "$", 0);
        return true;
    } catch (const std::invalid_argument& failure) {
        error = failure.what();
        return false;
    }
}

ProgramPendingInputKind input_kind_from_string(std::string_view value) {
    if (value == "input") return ProgramPendingInputKind::Input;
    if (value == "capability_result") return ProgramPendingInputKind::CapabilityResult;
    throw std::invalid_argument("Unknown Program pending input kind: " + std::string(value));
}

ProgramPendingState pending_state_from_string(std::string_view value) {
    if (value == "awaiting") return ProgramPendingState::Awaiting;
    if (value == "consumed") return ProgramPendingState::Consumed;
    if (value == "expired") return ProgramPendingState::Expired;
    if (value == "cancelled") return ProgramPendingState::Cancelled;
    if (value == "ambiguous") return ProgramPendingState::Ambiguous;
    throw std::invalid_argument("Unknown Program pending state: " + std::string(value));
}

ProgramEffectIdempotency idempotency_from_string(std::string_view value) {
    if (value == "idempotent") return ProgramEffectIdempotency::Idempotent;
    if (value == "non_idempotent") return ProgramEffectIdempotency::NonIdempotent;
    throw std::invalid_argument("Unknown Program effect idempotency: " + std::string(value));
}

ProgramEffectReconciliation reconciliation_from_string(std::string_view value) {
    if (value == "none") return ProgramEffectReconciliation::None;
    if (value == "completed") return ProgramEffectReconciliation::Completed;
    if (value == "failed") return ProgramEffectReconciliation::Failed;
    if (value == "unknown") return ProgramEffectReconciliation::Unknown;
    throw std::invalid_argument("Unknown Program effect reconciliation: " + std::string(value));
}

void normalize_input(ProgramPendingInputData& data) {
    detail::validate_token(data.operation_id, "Program pending operation_id");
    detail::validate_token(data.call_id, "Program pending call_id");
    detail::validate_token(data.core_node, "Program pending Core node");
    data.result_schema        = detail::owned_json_copy(data.result_schema);
    data.payload              = detail::owned_json_copy(data.payload);
    data.core_interrupt_value = detail::owned_json_copy(data.core_interrupt_value);
    validate_schema(data.result_schema, "$schema", 0);
    if (data.expires_at_unix_ms && *data.expires_at_unix_ms == 0) {
        throw std::invalid_argument("Program pending expiry must be positive when present");
    }
    if (data.consumed_result) data.consumed_result = detail::owned_json_copy(*data.consumed_result);
    if (data.state == ProgramPendingState::Consumed && !data.consumed_result) {
        throw std::invalid_argument("Consumed Program pending input requires its authoritative result");
    }
    if (data.consumed_result) {
        std::string schema_error;
        if (!value_matches_schema(*data.consumed_result, data.result_schema, schema_error)) {
            throw std::invalid_argument(
                "Consumed Program pending input violates result_schema: " + schema_error);
        }
    }
    if (data.state != ProgramPendingState::Consumed && data.consumed_result) {
        throw std::invalid_argument("Only consumed Program pending input may carry a result");
    }
    if (data.state == ProgramPendingState::Ambiguous) {
        throw std::invalid_argument("Program pending input cannot be ambiguous");
    }
}

void normalize_effect(ProgramPendingEffectData& data) {
    detail::validate_token(data.operation_id, "Program pending effect operation_id");
    detail::validate_token(data.call_id, "Program pending effect call_id");
    detail::validate_token(data.effect_id, "Program pending effect effect_id");
    detail::validate_token(data.core_node, "Program pending effect Core node");
    data.result_schema        = detail::owned_json_copy(data.result_schema);
    data.payload              = detail::owned_json_copy(data.payload);
    data.core_interrupt_value = detail::owned_json_copy(data.core_interrupt_value);
    validate_schema(data.result_schema, "$schema", 0);
    if (data.expires_at_unix_ms && *data.expires_at_unix_ms == 0) {
        throw std::invalid_argument("Program pending effect expiry must be positive when present");
    }
    if (data.reconciled_result) data.reconciled_result = detail::owned_json_copy(*data.reconciled_result);
    if (data.state == ProgramPendingState::Consumed) {
        if (data.reconciliation != ProgramEffectReconciliation::Completed &&
            data.reconciliation != ProgramEffectReconciliation::Failed) {
            throw std::invalid_argument("Consumed Program pending effect requires completed or failed reconciliation");
        }
        if (data.reconciliation == ProgramEffectReconciliation::Completed && !data.reconciled_result) {
            throw std::invalid_argument("Completed Program effect reconciliation requires a result");
        }
        if (data.reconciliation == ProgramEffectReconciliation::Completed) {
            std::string schema_error;
            if (!value_matches_schema(*data.reconciled_result, data.result_schema,
                                      schema_error)) {
                throw std::invalid_argument(
                    "Completed Program effect result violates result_schema: " +
                    schema_error);
            }
        }
        if (data.reconciliation == ProgramEffectReconciliation::Failed && data.reconciled_result) {
            throw std::invalid_argument("Failed Program effect reconciliation cannot carry a result");
        }
    } else if (data.state == ProgramPendingState::Ambiguous) {
        if (data.idempotency != ProgramEffectIdempotency::NonIdempotent) {
            throw std::invalid_argument("Only non-idempotent Program effects may be ambiguous");
        }
        if (data.reconciliation == ProgramEffectReconciliation::Completed ||
            data.reconciliation == ProgramEffectReconciliation::Failed || data.reconciled_result) {
            throw std::invalid_argument("Ambiguous Program effect cannot carry a terminal reconciliation");
        }
    } else if (data.reconciliation != ProgramEffectReconciliation::None || data.reconciled_result) {
        throw std::invalid_argument("Non-terminal Program pending effect cannot carry reconciliation state");
    }
}

json encode_optional_result(const std::optional<json>& result) {
    json value{{"present", result.has_value()}};
    if (result) value["value"] = *result;
    return value;
}

std::optional<json> parse_optional_result(const json& value, std::string_view name) {
    require_object(value, name);
    detail::reject_unknown_fields(value, name, {"present", "value"});
    if (!value.contains("present") || !value["present"].is_boolean()) {
        throw std::invalid_argument(std::string(name) + " present must be boolean");
    }
    const bool present = value["present"].get<bool>();
    if (present != value.contains("value")) {
        throw std::invalid_argument(std::string(name) + " value presence does not match present");
    }
    if (!present) return std::nullopt;
    return detail::owned_json_copy(value["value"]);
}

json encode_expiry(std::optional<std::uint64_t> expiry) {
    return expiry ? json(*expiry) : json(nullptr);
}

std::optional<std::uint64_t> parse_expiry(const json& value) {
    if (value.is_null()) return std::nullopt;
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("Program pending expires_at_unix_ms must be unsigned or null");
    }
    return value.get<unsigned long long>();
}

json encode_core_projection(std::string_view node, const json& value) {
    return json{{"node", node}, {"value", value}};
}

std::pair<std::string, json> parse_core_projection(const json& value) {
    require_object(value, "Program pending Core projection");
    detail::reject_unknown_fields(value, "Program pending Core projection", {"node", "value"});
    return {require_string(value, "node"), detail::owned_json_copy(require_value(value, "value"))};
}

ProgramPendingInputData input_data_from_json(const json& value) {
    const auto projection = parse_core_projection(require_value(value, "core_interrupt"));
    ProgramPendingInputData data;
    data.operation_id         = require_string(value, "operation_id");
    data.call_id              = require_string(value, "call_id");
    data.kind                 = input_kind_from_string(require_string(value, "kind"));
    data.result_schema        = detail::owned_json_copy(require_value(value, "result_schema"));
    data.payload              = detail::owned_json_copy(require_value(value, "payload"));
    data.expires_at_unix_ms   = parse_expiry(require_value(value, "expires_at_unix_ms"));
    data.core_node            = projection.first;
    data.core_interrupt_value = projection.second;
    data.state                = pending_state_from_string(require_string(value, "state"));
    data.consumed_result      = parse_optional_result(require_value(value, "consumed_result"),
                                                      "Program pending consumed_result");
    return data;
}

ProgramPendingEffectData effect_data_from_json(const json& value) {
    const auto projection = parse_core_projection(require_value(value, "core_interrupt"));
    ProgramPendingEffectData data;
    data.operation_id         = require_string(value, "operation_id");
    data.call_id              = require_string(value, "call_id");
    data.effect_id            = require_string(value, "effect_id");
    data.result_schema        = detail::owned_json_copy(require_value(value, "result_schema"));
    data.payload              = detail::owned_json_copy(require_value(value, "payload"));
    data.expires_at_unix_ms   = parse_expiry(require_value(value, "expires_at_unix_ms"));
    data.effect_mode          = effect_mode_from_string(require_string(value, "effect_mode"));
    data.idempotency          = idempotency_from_string(require_string(value, "idempotency"));
    data.core_node            = projection.first;
    data.core_interrupt_value = projection.second;
    data.state                = pending_state_from_string(require_string(value, "state"));
    data.reconciliation       = reconciliation_from_string(require_string(value, "reconciliation"));
    data.reconciled_result    = parse_optional_result(require_value(value, "reconciled_result"),
                                                      "Program effect reconciled_result");
    return data;
}

json input_envelope(const ProgramPendingInputData& data) {
    return json{{"format", std::string(INPUT_FORMAT)},
                {"storage_schema_version", ProgramPendingInput::STORAGE_SCHEMA_VERSION},
                {"operation_id", data.operation_id}, {"call_id", data.call_id},
                {"kind", std::string(to_string(data.kind))}, {"result_schema", data.result_schema},
                {"payload", data.payload},
                {"expires_at_unix_ms", encode_expiry(data.expires_at_unix_ms)},
                {"core_interrupt", encode_core_projection(data.core_node, data.core_interrupt_value)},
                {"state", std::string(to_string(data.state))},
                {"consumed_result", encode_optional_result(data.consumed_result)}};
}

json effect_envelope(const ProgramPendingEffectData& data) {
    return json{{"format", std::string(EFFECT_FORMAT)},
                {"storage_schema_version", ProgramPendingEffect::STORAGE_SCHEMA_VERSION},
                {"operation_id", data.operation_id}, {"call_id", data.call_id},
                {"effect_id", data.effect_id}, {"result_schema", data.result_schema},
                {"payload", data.payload},
                {"expires_at_unix_ms", encode_expiry(data.expires_at_unix_ms)},
                {"effect_mode", std::string(to_string(data.effect_mode))},
                {"idempotency", std::string(to_string(data.idempotency))},
                {"core_interrupt", encode_core_projection(data.core_node, data.core_interrupt_value)},
                {"state", std::string(to_string(data.state))},
                {"reconciliation", std::string(to_string(data.reconciliation))},
                {"reconciled_result", encode_optional_result(data.reconciled_result)}};
}

bool is_expired(std::optional<std::uint64_t> expiry, std::uint64_t now) {
    return expiry && now >= *expiry;
}
bool same_canonical_json(const json& lhs, const json& rhs) {
    return detail::canonical_json_bytes(lhs) == detail::canonical_json_bytes(rhs);
}

bool same_canonical_optional_json(const std::optional<json>& lhs,
                                  const std::optional<json>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    return !lhs || same_canonical_json(*lhs, *rhs);
}

ProgramPendingInputUpdate input_update(ProgramPendingDisposition disposition,
                                       ProgramPendingInput value, std::string code,
                                       std::string message) {
    return {disposition, std::move(value), std::move(code), std::move(message)};
}

ProgramPendingEffectUpdate effect_update(ProgramPendingDisposition disposition,
                                         ProgramPendingEffect value, std::string code,
                                         std::string message) {
    return {disposition, std::move(value), std::move(code), std::move(message)};
}

}  // namespace

struct ProgramPendingInput::Impl {
    explicit Impl(ProgramPendingInputData value) : data(std::move(value)) {}
    ProgramPendingInputData data;
};
struct ProgramPendingEffect::Impl {
    explicit Impl(ProgramPendingEffectData value) : data(std::move(value)) {}
    ProgramPendingEffectData data;
};

ProgramPendingInput::ProgramPendingInput(ProgramPendingInputData data) {
    normalize_input(data);
    impl_ = std::make_shared<const Impl>(std::move(data));
}
ProgramPendingInput::ProgramPendingInput(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
ProgramPendingInput ProgramPendingInput::parse(std::string_view stored_bytes) {
    json value;
    try { value = detail::parse_json_strict(stored_bytes); }
    catch (const std::exception& failure) {
        throw std::invalid_argument(std::string("Invalid stored ProgramPendingInput JSON: ") + failure.what());
    }
    require_object(value, "Stored ProgramPendingInput");
    detail::reject_unknown_fields(value, "Stored ProgramPendingInput",
        {"format", "storage_schema_version", "operation_id", "call_id", "kind", "result_schema",
         "payload", "expires_at_unix_ms", "core_interrupt", "state", "consumed_result"});
    if (require_string(value, "format") != INPUT_FORMAT)
        throw std::invalid_argument("Stored ProgramPendingInput has unknown format");
    if (require_u64(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ProgramPendingInput schema version is unsupported");
    return ProgramPendingInput(input_data_from_json(value));
}
const std::string& ProgramPendingInput::operation_id() const noexcept { return impl_->data.operation_id; }
const std::string& ProgramPendingInput::call_id() const noexcept { return impl_->data.call_id; }
ProgramPendingInputKind ProgramPendingInput::kind() const noexcept { return impl_->data.kind; }
json ProgramPendingInput::result_schema() const { return impl_->data.result_schema; }
json ProgramPendingInput::payload() const { return impl_->data.payload; }
std::optional<std::uint64_t> ProgramPendingInput::expires_at_unix_ms() const noexcept { return impl_->data.expires_at_unix_ms; }
const std::string& ProgramPendingInput::core_node() const noexcept { return impl_->data.core_node; }
json ProgramPendingInput::core_interrupt_value() const { return impl_->data.core_interrupt_value; }
ProgramPendingState ProgramPendingInput::state() const noexcept { return impl_->data.state; }
std::optional<json> ProgramPendingInput::consumed_result() const { return impl_->data.consumed_result; }

ProgramPendingInputUpdate ProgramPendingInput::submit(std::string_view submitted_call_id,
                                                      const json& result,
                                                      std::uint64_t now_unix_ms) const {
    if (submitted_call_id != call_id())
        return input_update(ProgramPendingDisposition::WrongPendingId, *this, "P_PENDING_ID_MISMATCH", "Submitted call_id does not match the exact pending call");
    if (state() == ProgramPendingState::Consumed) {
        if (same_canonical_json(*impl_->data.consumed_result, result))
            return input_update(ProgramPendingDisposition::Duplicate, *this, "P_PENDING_DUPLICATE", "Exact duplicate pending result is already authoritative");
        return input_update(ProgramPendingDisposition::Conflict, *this, "P_PENDING_CONFLICT", "Conflicting duplicate cannot replace the authoritative result");
    }
    if (state() == ProgramPendingState::Expired)
        return input_update(ProgramPendingDisposition::Expired, *this, "P_PENDING_EXPIRED", "Late result rejected because the pending input expired");
    if (state() == ProgramPendingState::Cancelled)
        return input_update(ProgramPendingDisposition::Cancelled, *this, "P_PENDING_CANCELLED", "Result rejected because the pending input was cancelled");
    if (state() != ProgramPendingState::Awaiting)
        return input_update(ProgramPendingDisposition::NotAwaiting, *this, "P_PENDING_NOT_AWAITING", "Program is not awaiting this input");
    if (is_expired(expires_at_unix_ms(), now_unix_ms)) {
        auto data = impl_->data; data.state = ProgramPendingState::Expired;
        return input_update(ProgramPendingDisposition::Expired, ProgramPendingInput(std::move(data)), "P_PENDING_EXPIRED", "Late result rejected and pending input published as expired");
    }
    std::string schema_error;
    if (!value_matches_schema(result, impl_->data.result_schema, schema_error))
        return input_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_PENDING_SCHEMA_MISMATCH", std::move(schema_error));
    auto data = impl_->data;
    data.state = ProgramPendingState::Consumed;
    data.consumed_result = detail::owned_json_copy(result);
    return input_update(ProgramPendingDisposition::Applied, ProgramPendingInput(std::move(data)), "P_PENDING_ACCEPTED", "Pending result accepted");
}
ProgramPendingInputUpdate ProgramPendingInput::expire(std::uint64_t now_unix_ms) const {
    if (state() == ProgramPendingState::Expired)
        return input_update(ProgramPendingDisposition::Duplicate, *this, "P_PENDING_EXPIRED", "Pending input is already expired");
    if (state() != ProgramPendingState::Awaiting || !is_expired(expires_at_unix_ms(), now_unix_ms))
        return input_update(ProgramPendingDisposition::NotAwaiting, *this, "P_PENDING_NOT_EXPIRED", "Pending input has no elapsed expiry");
    auto data = impl_->data; data.state = ProgramPendingState::Expired;
    return input_update(ProgramPendingDisposition::Expired, ProgramPendingInput(std::move(data)), "P_PENDING_EXPIRED", "Pending input published as expired");
}
ProgramPendingInputUpdate ProgramPendingInput::cancel() const {
    if (state() == ProgramPendingState::Cancelled)
        return input_update(ProgramPendingDisposition::Duplicate, *this, "P_PENDING_CANCELLED", "Pending cancellation was already applied");
    if (state() != ProgramPendingState::Awaiting)
        return input_update(ProgramPendingDisposition::NotAwaiting, *this, "P_PENDING_NOT_AWAITING", "Only awaiting input can be cancelled");
    auto data = impl_->data; data.state = ProgramPendingState::Cancelled;
    return input_update(ProgramPendingDisposition::Applied, ProgramPendingInput(std::move(data)), "P_PENDING_CANCELLED", "Pending cancellation applied");
}
std::string ProgramPendingInput::serialize_canonical() const { return detail::canonical_json_bytes(input_envelope(impl_->data)); }
bool ProgramPendingInput::operator==(const ProgramPendingInput& other) const {
    return serialize_canonical() == other.serialize_canonical();
}
bool ProgramPendingInputUpdate::state_changed() const noexcept {
    return disposition == ProgramPendingDisposition::Applied || disposition == ProgramPendingDisposition::Expired;
}

ProgramPendingEffect::ProgramPendingEffect(ProgramPendingEffectData data) {
    normalize_effect(data);
    impl_ = std::make_shared<const Impl>(std::move(data));
}
ProgramPendingEffect::ProgramPendingEffect(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
ProgramPendingEffect ProgramPendingEffect::parse(std::string_view stored_bytes) {
    json value;
    try { value = detail::parse_json_strict(stored_bytes); }
    catch (const std::exception& failure) {
        throw std::invalid_argument(std::string("Invalid stored ProgramPendingEffect JSON: ") + failure.what());
    }
    require_object(value, "Stored ProgramPendingEffect");
    detail::reject_unknown_fields(value, "Stored ProgramPendingEffect",
        {"format", "storage_schema_version", "operation_id", "call_id", "effect_id", "result_schema",
         "payload", "expires_at_unix_ms", "effect_mode", "idempotency", "core_interrupt", "state",
         "reconciliation", "reconciled_result"});
    if (require_string(value, "format") != EFFECT_FORMAT)
        throw std::invalid_argument("Stored ProgramPendingEffect has unknown format");
    if (require_u64(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ProgramPendingEffect schema version is unsupported");
    return ProgramPendingEffect(effect_data_from_json(value));
}
const std::string& ProgramPendingEffect::operation_id() const noexcept { return impl_->data.operation_id; }
const std::string& ProgramPendingEffect::call_id() const noexcept { return impl_->data.call_id; }
const std::string& ProgramPendingEffect::effect_id() const noexcept { return impl_->data.effect_id; }
json ProgramPendingEffect::result_schema() const { return impl_->data.result_schema; }
json ProgramPendingEffect::payload() const { return impl_->data.payload; }
std::optional<std::uint64_t> ProgramPendingEffect::expires_at_unix_ms() const noexcept { return impl_->data.expires_at_unix_ms; }
EffectMode ProgramPendingEffect::effect_mode() const noexcept { return impl_->data.effect_mode; }
ProgramEffectIdempotency ProgramPendingEffect::idempotency() const noexcept { return impl_->data.idempotency; }
const std::string& ProgramPendingEffect::core_node() const noexcept { return impl_->data.core_node; }
json ProgramPendingEffect::core_interrupt_value() const { return impl_->data.core_interrupt_value; }
ProgramPendingState ProgramPendingEffect::state() const noexcept { return impl_->data.state; }
ProgramEffectReconciliation ProgramPendingEffect::reconciliation() const noexcept { return impl_->data.reconciliation; }
std::optional<json> ProgramPendingEffect::reconciled_result() const { return impl_->data.reconciled_result; }

ProgramPendingEffectUpdate ProgramPendingEffect::submit(
    std::string_view submitted_call_id, std::string_view submitted_effect_id,
    const json& result, std::uint64_t now_unix_ms) const {
    if (submitted_call_id != call_id() || submitted_effect_id != effect_id())
        return effect_update(ProgramPendingDisposition::WrongPendingId, *this,
                             "P_PENDING_ID_MISMATCH",
                             "Submitted call/effect identity does not match the exact pending effect");
    if (state() == ProgramPendingState::Consumed) {
        if (reconciliation() == ProgramEffectReconciliation::Completed &&
            same_canonical_json(*impl_->data.reconciled_result, result))
            return effect_update(ProgramPendingDisposition::Duplicate, *this,
                                 "P_PENDING_DUPLICATE",
                                 "Exact duplicate effect result is already authoritative");
        return effect_update(ProgramPendingDisposition::Conflict, *this,
                             "P_PENDING_CONFLICT",
                             "Conflicting duplicate cannot replace the authoritative effect result");
    }
    if (state() == ProgramPendingState::Expired)
        return effect_update(ProgramPendingDisposition::Expired, *this,
                             "P_PENDING_EXPIRED",
                             "Late effect result rejected after expiry");
    if (state() == ProgramPendingState::Cancelled)
        return effect_update(ProgramPendingDisposition::Cancelled, *this,
                             "P_PENDING_CANCELLED",
                             "Effect result rejected after cancellation");
    if (state() == ProgramPendingState::Ambiguous)
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this,
                             "P_EFFECT_RECONCILIATION_REQUIRED",
                             "Ambiguous effect requires explicit reconciliation");
    if (state() != ProgramPendingState::Awaiting)
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this,
                             "P_PENDING_NOT_AWAITING",
                             "Effect is not awaiting a result");
    if (is_expired(expires_at_unix_ms(), now_unix_ms)) {
        auto data = impl_->data;
        data.state = ProgramPendingState::Expired;
        return effect_update(ProgramPendingDisposition::Expired,
                             ProgramPendingEffect(std::move(data)),
                             "P_PENDING_EXPIRED",
                             "Late effect result rejected and effect published as expired");
    }
    std::string schema_error;
    if (!value_matches_schema(result, impl_->data.result_schema, schema_error))
        return effect_update(ProgramPendingDisposition::SchemaMismatch, *this,
                             "P_PENDING_SCHEMA_MISMATCH", std::move(schema_error));
    auto data = impl_->data;
    data.state = ProgramPendingState::Consumed;
    data.reconciliation = ProgramEffectReconciliation::Completed;
    data.reconciled_result = detail::owned_json_copy(result);
    return effect_update(ProgramPendingDisposition::Applied,
                         ProgramPendingEffect(std::move(data)),
                         "P_PENDING_ACCEPTED", "Pending effect result accepted");
}

ProgramPendingEffectUpdate ProgramPendingEffect::mark_outcome_unknown(std::uint64_t now_unix_ms) const {
    if (state() == ProgramPendingState::Ambiguous)
        return effect_update(ProgramPendingDisposition::Duplicate, *this, "P_EFFECT_AMBIGUOUS", "Effect outcome is already ambiguous");
    if (state() == ProgramPendingState::Expired)
        return effect_update(ProgramPendingDisposition::Expired, *this, "P_PENDING_EXPIRED", "Expired effect cannot be dispatched again");
    if (state() == ProgramPendingState::Cancelled)
        return effect_update(ProgramPendingDisposition::Cancelled, *this, "P_PENDING_CANCELLED", "Cancelled effect cannot be dispatched again");
    if (state() != ProgramPendingState::Awaiting)
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this, "P_PENDING_NOT_AWAITING", "Effect is not awaiting an outcome");
    if (idempotency() == ProgramEffectIdempotency::NonIdempotent) {
        auto data = impl_->data;
        data.state = ProgramPendingState::Ambiguous;
        data.reconciliation = ProgramEffectReconciliation::None;
        return effect_update(ProgramPendingDisposition::Applied,
                             ProgramPendingEffect(std::move(data)),
                             "P_EFFECT_AMBIGUOUS",
                             "Non-idempotent unknown outcome published as ambiguous");
    }
    if (is_expired(expires_at_unix_ms(), now_unix_ms)) {
        auto data = impl_->data;
        data.state = ProgramPendingState::Expired;
        return effect_update(ProgramPendingDisposition::Expired,
                             ProgramPendingEffect(std::move(data)),
                             "P_PENDING_EXPIRED",
                             "Pending idempotent effect published as expired");
    }
    return effect_update(ProgramPendingDisposition::NotAwaiting, *this,
                         "P_EFFECT_RETRY_PERMITTED",
                         "Idempotent unknown outcome does not require ambiguity publication");
}

ProgramPendingEffectUpdate ProgramPendingEffect::reconcile(
    std::string_view submitted_call_id, std::string_view submitted_effect_id,
    ProgramEffectReconciliation resolution, std::optional<json> result,
    std::uint64_t now_unix_ms) const {
    (void)now_unix_ms;  // Ambiguity has no TTL; it persists until explicit reconciliation.
    if (submitted_call_id != call_id() || submitted_effect_id != effect_id())
        return effect_update(ProgramPendingDisposition::WrongPendingId, *this, "P_PENDING_ID_MISMATCH", "Submitted call/effect identity does not match the exact pending effect");
    if (state() == ProgramPendingState::Consumed) {
        if (resolution == reconciliation() &&
            same_canonical_optional_json(result, impl_->data.reconciled_result))
            return effect_update(ProgramPendingDisposition::Duplicate, *this, "P_EFFECT_RECONCILIATION_DUPLICATE", "Exact duplicate reconciliation is already authoritative");
        return effect_update(ProgramPendingDisposition::Conflict, *this, "P_EFFECT_RECONCILIATION_CONFLICT", "Conflicting reconciliation cannot replace the authoritative result");
    }
    if (state() == ProgramPendingState::Expired)
        return effect_update(ProgramPendingDisposition::Expired, *this, "P_PENDING_EXPIRED", "Late effect reconciliation rejected after expiry");
    if (state() == ProgramPendingState::Cancelled)
        return effect_update(ProgramPendingDisposition::Cancelled, *this, "P_PENDING_CANCELLED", "Effect reconciliation rejected after cancellation");
    if (state() != ProgramPendingState::Ambiguous)
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this, "P_EFFECT_NOT_AMBIGUOUS", "Only an ambiguous effect accepts explicit reconciliation");
    if (resolution == ProgramEffectReconciliation::None)
        return effect_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_EFFECT_RECONCILIATION_INVALID", "Effect reconciliation must be completed, failed, or unknown");
    if (resolution == ProgramEffectReconciliation::Unknown) {
        if (result)
            return effect_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_EFFECT_RECONCILIATION_INVALID", "Unknown effect reconciliation cannot carry a result");
        if (reconciliation() == ProgramEffectReconciliation::Unknown)
            return effect_update(ProgramPendingDisposition::Duplicate, *this, "P_EFFECT_RECONCILIATION_DUPLICATE", "Unknown reconciliation is already recorded");
        auto data = impl_->data; data.reconciliation = ProgramEffectReconciliation::Unknown;
        return effect_update(ProgramPendingDisposition::Applied, ProgramPendingEffect(std::move(data)), "P_EFFECT_RECONCILIATION_UNKNOWN", "Unknown reconciliation recorded; effect remains ambiguous");
    }
    if (resolution == ProgramEffectReconciliation::Failed) {
        if (result)
            return effect_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_EFFECT_RECONCILIATION_INVALID", "Failed effect reconciliation cannot carry a result");
        auto data = impl_->data; data.state = ProgramPendingState::Consumed;
        data.reconciliation = ProgramEffectReconciliation::Failed;
        return effect_update(ProgramPendingDisposition::Applied, ProgramPendingEffect(std::move(data)), "P_EFFECT_RECONCILED_FAILED", "Effect reconciled as failed");
    }
    if (!result)
        return effect_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_EFFECT_RECONCILIATION_INVALID", "Completed effect reconciliation requires a result");
    std::string schema_error;
    if (!value_matches_schema(*result, impl_->data.result_schema, schema_error))
        return effect_update(ProgramPendingDisposition::SchemaMismatch, *this, "P_PENDING_SCHEMA_MISMATCH", std::move(schema_error));
    auto data = impl_->data; data.state = ProgramPendingState::Consumed;
    data.reconciliation = ProgramEffectReconciliation::Completed;
    data.reconciled_result = detail::owned_json_copy(*result);
    return effect_update(ProgramPendingDisposition::Applied, ProgramPendingEffect(std::move(data)), "P_EFFECT_RECONCILED_COMPLETED", "Effect reconciled as completed");
}
ProgramPendingEffectUpdate ProgramPendingEffect::expire(std::uint64_t now_unix_ms) const {
    if (state() == ProgramPendingState::Expired)
        return effect_update(ProgramPendingDisposition::Duplicate, *this, "P_PENDING_EXPIRED", "Pending effect is already expired");
    if (state() != ProgramPendingState::Awaiting ||
        !is_expired(expires_at_unix_ms(), now_unix_ms))
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this,
                             state() == ProgramPendingState::Ambiguous
                                 ? "P_EFFECT_RECONCILIATION_REQUIRED"
                                 : "P_PENDING_NOT_EXPIRED",
                             state() == ProgramPendingState::Ambiguous
                                 ? "Ambiguous effect persists until explicit reconciliation"
                                 : "Pending effect has no elapsed expiry");
    auto data = impl_->data; data.state = ProgramPendingState::Expired;
    data.reconciliation = ProgramEffectReconciliation::None;
    return effect_update(ProgramPendingDisposition::Expired, ProgramPendingEffect(std::move(data)), "P_PENDING_EXPIRED", "Pending effect published as expired");
}
ProgramPendingEffectUpdate ProgramPendingEffect::cancel() const {
    if (state() == ProgramPendingState::Cancelled)
        return effect_update(ProgramPendingDisposition::Duplicate, *this, "P_PENDING_CANCELLED", "Pending effect cancellation was already applied");
    if (state() != ProgramPendingState::Awaiting)
        return effect_update(ProgramPendingDisposition::NotAwaiting, *this,
                             state() == ProgramPendingState::Ambiguous
                                 ? "P_EFFECT_RECONCILIATION_REQUIRED"
                                 : "P_PENDING_NOT_AWAITING",
                             state() == ProgramPendingState::Ambiguous
                                 ? "Ambiguous effect cannot be erased by cancellation"
                                 : "Only an awaiting effect can be cancelled");
    auto data = impl_->data; data.state = ProgramPendingState::Cancelled;
    data.reconciliation = ProgramEffectReconciliation::None;
    return effect_update(ProgramPendingDisposition::Applied, ProgramPendingEffect(std::move(data)), "P_PENDING_CANCELLED", "Pending effect cancellation applied");
}
std::string ProgramPendingEffect::serialize_canonical() const { return detail::canonical_json_bytes(effect_envelope(impl_->data)); }
bool ProgramPendingEffect::operator==(const ProgramPendingEffect& other) const {
    return serialize_canonical() == other.serialize_canonical();
}
bool ProgramPendingEffectUpdate::state_changed() const noexcept {
    return disposition == ProgramPendingDisposition::Applied || disposition == ProgramPendingDisposition::Expired;
}

std::string_view to_string(ProgramPendingInputKind value) noexcept {
    switch (value) { case ProgramPendingInputKind::Input: return "input"; case ProgramPendingInputKind::CapabilityResult: return "capability_result"; }
    return "unknown";
}
std::string_view to_string(ProgramPendingState value) noexcept {
    switch (value) { case ProgramPendingState::Awaiting: return "awaiting"; case ProgramPendingState::Consumed: return "consumed"; case ProgramPendingState::Expired: return "expired"; case ProgramPendingState::Cancelled: return "cancelled"; case ProgramPendingState::Ambiguous: return "ambiguous"; }
    return "unknown";
}
std::string_view to_string(ProgramEffectIdempotency value) noexcept {
    switch (value) { case ProgramEffectIdempotency::Idempotent: return "idempotent"; case ProgramEffectIdempotency::NonIdempotent: return "non_idempotent"; }
    return "unknown";
}
std::string_view to_string(ProgramEffectReconciliation value) noexcept {
    switch (value) { case ProgramEffectReconciliation::None: return "none"; case ProgramEffectReconciliation::Completed: return "completed"; case ProgramEffectReconciliation::Failed: return "failed"; case ProgramEffectReconciliation::Unknown: return "unknown"; }
    return "unknown";
}
std::string_view to_string(ProgramPendingDisposition value) noexcept {
    switch (value) { case ProgramPendingDisposition::Applied: return "applied"; case ProgramPendingDisposition::Duplicate: return "duplicate"; case ProgramPendingDisposition::Conflict: return "conflict"; case ProgramPendingDisposition::WrongPendingId: return "wrong_pending_id"; case ProgramPendingDisposition::SchemaMismatch: return "schema_mismatch"; case ProgramPendingDisposition::Expired: return "expired"; case ProgramPendingDisposition::Cancelled: return "cancelled"; case ProgramPendingDisposition::NotAwaiting: return "not_awaiting"; }
    return "unknown";
}

}  // namespace neograph::program
