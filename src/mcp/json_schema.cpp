#include <neograph/mcp/json_schema.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace neograph::mcp {
namespace {

constexpr std::size_t kMaxSchemaDepth = 64;

void check_depth(std::size_t depth, const std::string& path) {
    if (depth > kMaxSchemaDepth) {
        throw std::invalid_argument("JSON Schema nesting exceeds 64 levels at "
                                    + path);
    }
}

bool schema_type_matches(const json& value, const std::string& type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "string") return value.is_string();
    throw std::invalid_argument("unsupported JSON Schema type: " + type);
}

void validate_type_name(const json& type, const std::string& path) {
    if (!type.is_string()) {
        throw std::invalid_argument("JSON Schema type at " + path
                                    + " must contain strings");
    }
    static const std::vector<std::string> supported = {
        "null", "boolean", "object", "array", "number", "integer", "string"
    };
    const auto name = type.get<std::string>();
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
        throw std::invalid_argument("unsupported JSON Schema type: " + name);
    }
}

void validate_schema_impl(const json& schema, const std::string& path,
                          std::size_t depth) {
    check_depth(depth, path);
    if (!schema.is_object()) {
        throw std::invalid_argument("JSON Schema at " + path + " must be an object");
    }
    if (schema.contains("type")) {
        if (schema["type"].is_string()) {
            validate_type_name(schema["type"], path);
        } else if (schema["type"].is_array()) {
            for (const auto& type : schema["type"]) validate_type_name(type, path);
        } else {
            throw std::invalid_argument("JSON Schema type at " + path
                                        + " must be a string or array");
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        throw std::invalid_argument("JSON Schema enum at " + path
                                    + " must be an array");
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            throw std::invalid_argument("JSON Schema required at " + path
                                        + " must be an array");
        }
        for (const auto& name : schema["required"]) {
            if (!name.is_string()) {
                throw std::invalid_argument("JSON Schema required at " + path
                                            + " must contain strings");
            }
        }
    }
    if (schema.contains("properties")) {
        if (!schema["properties"].is_object()) {
            throw std::invalid_argument("JSON Schema properties at " + path
                                        + " must be an object");
        }
        for (auto it = schema["properties"].begin();
             it != schema["properties"].end(); ++it) {
            validate_schema_impl(it.value(), path + "/properties/" + it.key(),
                                 depth + 1);
        }
    }
    if (schema.contains("items")) {
        validate_schema_impl(schema["items"], path + "/items", depth + 1);
    }
    if (schema.contains("additionalProperties")
        && !schema["additionalProperties"].is_boolean()
        && !schema["additionalProperties"].is_object()) {
        throw std::invalid_argument("JSON Schema additionalProperties at " + path
                                    + " must be a boolean or object");
    }
    if (schema.contains("additionalProperties")
        && schema["additionalProperties"].is_object()) {
        validate_schema_impl(schema["additionalProperties"],
                             path + "/additionalProperties", depth + 1);
    }
}

void validate_value_impl(const json& value, const json& schema,
                         const std::string& subject, const std::string& path,
                         std::size_t depth) {
    check_depth(depth, path);
    if (schema.contains("const") && value != schema["const"]) {
        throw std::invalid_argument(subject + " at " + path
                                    + " does not match const");
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
            throw std::invalid_argument(subject + " at " + path
                                        + " is not in enum");
        }
    }
    if (schema.contains("type")) {
        bool matched = false;
        const auto& types = schema["type"];
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
        if (!matched) {
            throw std::invalid_argument(subject + " at " + path
                                        + " has the wrong JSON type");
        }
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& name : schema["required"]) {
                const auto property = name.get<std::string>();
                if (!value.contains(property)) {
                    throw std::invalid_argument(subject + " at " + path
                                                + " is missing required property "
                                                + property);
                }
            }
        }
        const json properties = schema.value("properties", json::object());
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (value.contains(it.key())) {
                validate_value_impl(value[it.key()], it.value(), subject,
                                    path + "/" + it.key(), depth + 1);
            }
        }
        if (schema.contains("additionalProperties")) {
            const auto& additional = schema["additionalProperties"];
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (properties.contains(it.key())) continue;
                if (additional.is_boolean() && !additional.get<bool>()) {
                    throw std::invalid_argument(subject + " at " + path
                                                + " has unexpected property "
                                                + it.key());
                }
                if (additional.is_object()) {
                    validate_value_impl(it.value(), additional, subject,
                                        path + "/" + it.key(), depth + 1);
                }
            }
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            validate_value_impl(value[i], schema["items"], subject,
                                path + "/" + std::to_string(i), depth + 1);
        }
    }
}

} // namespace

void validate_json_schema(const json& schema, const std::string& path) {
    validate_schema_impl(schema, path, 0);
}

void validate_json_value(const json& value, const json& schema,
                         const std::string& subject, const std::string& path) {
    validate_schema_impl(schema, path, 0);
    validate_value_impl(value, schema, subject, path, 0);
}

} // namespace neograph::mcp
