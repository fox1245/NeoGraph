// JSON Schema 2020-12 validation for MCP tool schemas (MCP 2025-11-25,
// "JSON Schema Usage": a schema without $schema is 2020-12).
//
// One validator serves the server's inputSchema / outputSchema checks, the
// client's structuredContent check and the Harness. Before it, the server and
// the client each kept a keyword subset that ignored $ref, anyOf, minimum and
// others and rejected 1.0 as an integer, so invalid arguments reached handlers
// and results NeoGraph accepted could fail the official SDKs' validators.
//
// Supported: the core, applicator, unevaluated and validation vocabularies of
// 2020-12, boolean schemas, and local $ref (JSON Pointer fragments, $anchor
// names, $id resources in the same document). `format` and the content and
// meta-data keywords are annotations, as 2020-12 specifies by default.
// Refused when the schema is checked: another $schema dialect, $dynamicRef or
// $recursiveRef, a $ref to another document, and a pattern std::regex cannot
// compile (for example \p{...}).

#include <neograph/mcp/json_schema.h>

#include "../core/json_walk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using neograph::detail::check_walk_depth;

namespace neograph::mcp {
namespace {

constexpr std::size_t kMaxSchemaDepth = 64;
constexpr std::size_t kMaxEvaluationDepth = 256;
// std::regex matches recursively: a longer string fails its pattern instead
// of risking the matcher's stack on untrusted input.
constexpr std::size_t kMaxPatternSubject = 16 * 1024;
constexpr std::string_view kDialect = "https://json-schema.org/draft/2020-12/schema";

bool supported_dialect(const std::string& uri) {
    return uri == kDialect || uri == std::string(kDialect) + "#";
}

std::shared_ptr<const std::regex> compiled_pattern(const std::string& pattern) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::shared_ptr<const std::regex>> cache;
    {
        std::lock_guard lock(mu);
        if (auto it = cache.find(pattern); it != cache.end()) return it->second;
    }
    auto compiled = std::make_shared<const std::regex>(pattern, std::regex::ECMAScript);
    std::lock_guard lock(mu);
    if (cache.size() >= 512) cache.clear();
    cache.emplace(pattern, compiled);
    return compiled;
}

bool is_integral(const json& value) {
    if (value.is_number_integer()) return true;
    if (!value.is_number_float()) return false;
    const double number = value.get<double>();
    return std::isfinite(number) && std::floor(number) == number;
}

long double number_of(const json& value) {
    if (value.is_number_unsigned()) return static_cast<long double>(value.get<unsigned long long>());
    if (value.is_number_integer()) return static_cast<long double>(value.get<long long>());
    return static_cast<long double>(value.get<double>());
}

// JSON Schema equality: numbers compare by value (1 equals 1.0), objects by
// their members whatever their order.
bool json_equal(const json& a, const json& b, std::size_t depth = 0) {
    if (a.is_number() && b.is_number()) return number_of(a) == number_of(b);
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (a.is_boolean() || b.is_boolean())
        return a.is_boolean() && b.is_boolean() && a.get<bool>() == b.get<bool>();
    if (a.is_string() || b.is_string())
        return a.is_string() && b.is_string() && a.get<std::string>() == b.get<std::string>();
    if (a.is_array() || b.is_array()) {
        if (!a.is_array() || !b.is_array() || a.size() != b.size()) return false;
        check_walk_depth(depth, "JSON Schema equality");
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!json_equal(a[i], b[i], depth + 1)) return false;
        return true;
    }
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size()) return false;
        check_walk_depth(depth, "JSON Schema equality");
        for (auto it = a.begin(); it != a.end(); ++it) {
            const std::string key = it.key();
            if (!b.contains(key) || !json_equal(it.value(), b[key], depth + 1)) return false;
        }
        return true;
    }
    return false;
}

bool type_matches(const json& value, const std::string& type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "number") return value.is_number();
    if (type == "integer") return is_integral(value);
    if (type == "string") return value.is_string();
    return false;
}

std::size_t code_points(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char byte : text)
        if ((byte & 0xC0) != 0x80) ++count;
    return count;
}

std::string percent_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// The schemas a keyword holds, by the keyword's shape.
enum class Shape { Schema, SchemaMap, SchemaList };

const std::vector<std::pair<std::string, Shape>>& subschema_keywords() {
    static const std::vector<std::pair<std::string, Shape>> keywords = {
        {"$defs", Shape::SchemaMap},
        {"definitions", Shape::SchemaMap},
        {"properties", Shape::SchemaMap},
        {"patternProperties", Shape::SchemaMap},
        {"dependentSchemas", Shape::SchemaMap},
        {"additionalProperties", Shape::Schema},
        {"unevaluatedProperties", Shape::Schema},
        {"propertyNames", Shape::Schema},
        {"items", Shape::Schema},
        {"unevaluatedItems", Shape::Schema},
        {"contains", Shape::Schema},
        {"not", Shape::Schema},
        {"if", Shape::Schema},
        {"then", Shape::Schema},
        {"else", Shape::Schema},
        {"prefixItems", Shape::SchemaList},
        {"allOf", Shape::SchemaList},
        {"anyOf", Shape::SchemaList},
        {"oneOf", Shape::SchemaList},
    };
    return keywords;
}

// The $id resources and $anchor names of one schema document. json copies are
// deep, so resources are held once here and passed around by pointer.
class SchemaIndex {
public:
    explicit SchemaIndex(const json& root) { collect(root, &root, 0); }

    // The resource a subschema opens with its $id, or `current`.
    const json* resource_of(const json& schema, const json* current) const {
        if (!schema.is_object() || !schema.contains("$id") || !schema["$id"].is_string())
            return current;
        const auto id = schema["$id"].get<std::string>();
        auto it = ids_.find(id.substr(0, id.find('#')));
        return it == ids_.end() ? current : &it->second;
    }

    struct Target {
        const json* resource = nullptr;
        const json* stored = nullptr;
        std::optional<json> handle;
        const json& schema() const { return handle ? *handle : stored ? *stored : *resource; }
    };

    std::optional<Target> resolve(const std::string& reference, const json* resource) const {
        const auto hash = reference.find('#');
        const std::string base = reference.substr(0, hash);
        const std::string fragment =
            hash == std::string::npos ? std::string() : reference.substr(hash + 1);
        const json* document = resource;
        if (!base.empty()) {
            auto it = ids_.find(base);
            if (it == ids_.end()) return std::nullopt;
            document = &it->second;
        }
        Target target;
        target.resource = document;
        if (fragment.empty()) return target;
        if (fragment.front() != '/') {
            auto it = anchors_.find(fragment);
            if (it == anchors_.end()) return std::nullopt;
            target.resource = it->second.second;
            target.stored = &it->second.first;
            return target;
        }
        std::optional<json> current;
        std::size_t start = 1;
        for (;;) {
            const auto end = fragment.find('/', start);
            std::string token = percent_decode(fragment.substr(
                start, end == std::string::npos ? std::string::npos : end - start));
            for (std::size_t at = 0; (at = token.find('~', at)) != std::string::npos; ++at) {
                if (at + 1 >= token.size()) return std::nullopt;
                if (token[at + 1] == '1') token.replace(at, 2, "/");
                else if (token[at + 1] == '0') token.replace(at, 2, "~");
                else return std::nullopt;
            }
            const json& parent = current ? *current : *document;
            if (parent.is_object()) {
                if (!parent.contains(token)) return std::nullopt;
                current = parent[token];
            } else if (parent.is_array()) {
                if (token.empty() || token.size() > 9 ||
                    token.find_first_not_of("0123456789") != std::string::npos)
                    return std::nullopt;
                const auto index = static_cast<std::size_t>(std::stoul(token));
                if (index >= parent.size()) return std::nullopt;
                current = parent[index];
            } else {
                return std::nullopt;
            }
            target.resource = resource_of(*current, target.resource);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        target.handle = std::move(current);
        return target;
    }

private:
    void collect(const json& schema, const json* resource, std::size_t depth) {
        if (!schema.is_object() || depth > kMaxSchemaDepth) return;
        const json* current = resource;
        if (schema.contains("$id") && schema["$id"].is_string()) {
            const auto id = schema["$id"].get<std::string>();
            const auto base = id.substr(0, id.find('#'));
            if (!base.empty()) current = &ids_.emplace(base, schema).first->second;
        }
        if (schema.contains("$anchor") && schema["$anchor"].is_string())
            anchors_.emplace(schema["$anchor"].get<std::string>(), std::make_pair(schema, current));
        for (const auto& [keyword, shape] : subschema_keywords()) {
            if (!schema.contains(keyword)) continue;
            const json value = schema[keyword];
            if (shape == Shape::Schema) {
                collect(value, current, depth + 1);
            } else if (shape == Shape::SchemaMap && value.is_object()) {
                for (auto it = value.begin(); it != value.end(); ++it)
                    collect(it.value(), current, depth + 1);
            } else if (shape == Shape::SchemaList && value.is_array()) {
                for (std::size_t i = 0; i < value.size(); ++i)
                    collect(value[i], current, depth + 1);
            }
        }
    }

    std::map<std::string, json> ids_;
    std::map<std::string, std::pair<json, const json*>> anchors_;
};

[[noreturn]] void schema_error(const std::string& path, const std::string& message) {
    throw std::invalid_argument("JSON Schema " + message + " at " + path);
}

void require_count(const json& schema, const char* keyword, const std::string& path) {
    if (!schema.contains(keyword)) return;
    const json value = schema[keyword];
    if (!is_integral(value) || number_of(value) < 0)
        schema_error(path, std::string(keyword) + " must be a non-negative integer");
}

void require_number(const json& schema, const char* keyword, const std::string& path) {
    if (schema.contains(keyword) && !schema[keyword].is_number())
        schema_error(path, std::string(keyword) + " must be a number");
}

void check_pattern(const json& pattern, const std::string& path) {
    if (!pattern.is_string()) schema_error(path, "pattern must be a string");
    try {
        (void)compiled_pattern(pattern.get<std::string>());
    } catch (const std::regex_error&) {
        schema_error(path, "pattern " + pattern.dump() + " is not a supported regular expression");
    }
}

void check_type_name(const json& type, const std::string& path) {
    static const std::set<std::string> names = {"null",   "boolean", "object", "array",
                                                "number", "integer", "string"};
    if (!type.is_string() || !names.contains(type.get<std::string>()))
        schema_error(path, "type must name a JSON type");
}

void check_schema(const json& schema, const std::string& path, std::size_t depth,
                  const SchemaIndex& index, const json* resource) {
    if (depth > kMaxSchemaDepth)
        throw std::invalid_argument("JSON Schema nesting exceeds 64 levels at " + path);
    if (schema.is_boolean()) return;
    if (!schema.is_object()) schema_error(path, "must be an object or a boolean");
    if (schema.contains("$id") && !schema["$id"].is_string())
        schema_error(path, "$id must be a string");
    const json* current_resource = depth > 0 ? index.resource_of(schema, resource) : resource;
    if (schema.contains("$schema") &&
        (!schema["$schema"].is_string() || !supported_dialect(schema["$schema"].get<std::string>())))
        schema_error(path, "dialect " + schema["$schema"].dump() + " is not supported (only " +
                               std::string(kDialect) + " is)");
    for (const char* unsupported : {"$dynamicRef", "$recursiveRef"}) {
        if (schema.contains(unsupported))
            schema_error(path, std::string(unsupported) + " is not supported");
    }
    if (schema.contains("$anchor") && !schema["$anchor"].is_string())
        schema_error(path, "$anchor must be a string");
    if (schema.contains("$ref")) {
        if (!schema["$ref"].is_string()) schema_error(path, "$ref must be a string");
        if (!index.resolve(schema["$ref"].get<std::string>(), current_resource))
            schema_error(path, "$ref " + schema["$ref"].dump() + " does not resolve inside this schema");
    }
    if (schema.contains("type")) {
        const json type = schema["type"];
        if (type.is_array()) {
            if (type.size() == 0) schema_error(path, "type must not be empty");
            for (std::size_t i = 0; i < type.size(); ++i) check_type_name(type[i], path);
        } else {
            check_type_name(type, path);
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array())
        schema_error(path, "enum must be an array");
    if (schema.contains("multipleOf")) {
        const json multiple = schema["multipleOf"];
        if (!multiple.is_number() || number_of(multiple) <= 0)
            schema_error(path, "multipleOf must be a number greater than 0");
    }
    for (const char* keyword : {"maximum", "exclusiveMaximum", "minimum", "exclusiveMinimum"})
        require_number(schema, keyword, path);
    for (const char* keyword : {"maxLength", "minLength", "maxItems", "minItems", "maxContains",
                                "minContains", "maxProperties", "minProperties"})
        require_count(schema, keyword, path);
    if (schema.contains("pattern")) check_pattern(schema["pattern"], path);
    if (schema.contains("uniqueItems") && !schema["uniqueItems"].is_boolean())
        schema_error(path, "uniqueItems must be a boolean");
    if (schema.contains("required")) {
        const json required = schema["required"];
        if (!required.is_array()) schema_error(path, "required must be an array");
        for (std::size_t i = 0; i < required.size(); ++i)
            if (!required[i].is_string()) schema_error(path, "required must contain strings");
    }
    if (schema.contains("dependentRequired")) {
        const json dependent = schema["dependentRequired"];
        if (!dependent.is_object()) schema_error(path, "dependentRequired must be an object");
        for (auto it = dependent.begin(); it != dependent.end(); ++it) {
            const json names = it.value();
            if (!names.is_array()) schema_error(path, "dependentRequired values must be arrays");
            for (std::size_t i = 0; i < names.size(); ++i)
                if (!names[i].is_string())
                    schema_error(path, "dependentRequired values must contain strings");
        }
    }
    if (schema.contains("items") && schema["items"].is_array())
        schema_error(path, "items must be a schema (2020-12 describes tuples with prefixItems)");
    if (schema.contains("patternProperties") && schema["patternProperties"].is_object()) {
        const json patterns = schema["patternProperties"];
        for (auto it = patterns.begin(); it != patterns.end(); ++it)
            check_pattern(json(it.key()), path + "/patternProperties");
    }
    for (const auto& [keyword, shape] : subschema_keywords()) {
        if (!schema.contains(keyword)) continue;
        const json value = schema[keyword];
        const std::string at = path + "/" + keyword;
        if (shape == Shape::Schema) {
            check_schema(value, at, depth + 1, index, current_resource);
        } else if (shape == Shape::SchemaMap) {
            if (!value.is_object()) schema_error(path, keyword + " must be an object");
            for (auto it = value.begin(); it != value.end(); ++it)
                check_schema(it.value(), at + "/" + it.key(), depth + 1, index, current_resource);
        } else {
            if (!value.is_array() || value.size() == 0)
                schema_error(path, keyword + " must be a non-empty array");
            for (std::size_t i = 0; i < value.size(); ++i)
                check_schema(value[i], at + "/" + std::to_string(i), depth + 1, index,
                             current_resource);
        }
    }
}

// What the keywords of a valid schema evaluated, for unevaluatedProperties and
// unevaluatedItems. A subschema that fails contributes nothing.
struct Annotations {
    std::set<std::string> properties;
    std::size_t prefix_items = 0;
    bool all_items = false;
    std::set<std::size_t> contained;

    void merge(const Annotations& other) {
        properties.insert(other.properties.begin(), other.properties.end());
        prefix_items = std::max(prefix_items, other.prefix_items);
        all_items = all_items || other.all_items;
        contained.insert(other.contained.begin(), other.contained.end());
    }
};

bool pattern_matches(const std::string& text, const std::string& pattern) {
    if (text.size() > kMaxPatternSubject) return false;
    try {
        return std::regex_search(text, *compiled_pattern(pattern));
    } catch (const std::regex_error&) {
        return false;
    }
}

class Evaluator {
public:
    Evaluator(const SchemaIndex& index, std::string subject)
        : index_(index), subject_(std::move(subject)) {}

    bool evaluate(const json& value, const json& schema, const std::string& path,
                  const json* resource, std::size_t depth, Annotations& out, std::string& error) {
        if (depth > kMaxEvaluationDepth) {
            error = at(path) + " exceeds the JSON Schema evaluation depth";
            return false;
        }
        if (schema.is_boolean()) {
            if (schema.get<bool>()) return true;
            error = at(path) + " is not allowed by its schema";
            return false;
        }
        if (!schema.is_object()) {
            error = at(path) + " has an invalid schema";
            return false;
        }
        const json* current_resource = depth > 0 ? index_.resource_of(schema, resource) : resource;
        Annotations local;
        if (schema.contains("$ref")) {
            auto target = index_.resolve(schema["$ref"].get<std::string>(), current_resource);
            if (!target) {
                error = at(path) + " uses an unresolvable $ref";
                return false;
            }
            Annotations referenced;
            if (!evaluate(value, target->schema(), path, target->resource, depth + 1, referenced,
                          error))
                return false;
            local.merge(referenced);
        }
        if (!check_assertions(value, schema, path, error)) return false;
        if (value.is_array() &&
            !check_array(value, schema, path, current_resource, depth, local, error))
            return false;
        if (value.is_object() &&
            !check_object(value, schema, path, current_resource, depth, local, error))
            return false;
        if (!check_applicators(value, schema, path, current_resource, depth, local, error))
            return false;
        if (!check_unevaluated(value, schema, path, current_resource, depth, local, error))
            return false;
        out.merge(local);
        return true;
    }

private:
    std::string at(const std::string& path) const { return subject_ + " at " + path; }

    bool fail(std::string& error, const std::string& path, const std::string& message) const {
        error = at(path) + " " + message;
        return false;
    }

    bool check_assertions(const json& value, const json& schema, const std::string& path,
                          std::string& error) const {
        if (schema.contains("type")) {
            const json type = schema["type"];
            bool matched = false;
            if (type.is_string()) {
                matched = type_matches(value, type.get<std::string>());
            } else {
                for (std::size_t i = 0; i < type.size() && !matched; ++i)
                    matched = type_matches(value, type[i].get<std::string>());
            }
            if (!matched) return fail(error, path, "has the wrong JSON type");
        }
        if (schema.contains("const") && !json_equal(value, schema["const"]))
            return fail(error, path, "does not match const");
        if (schema.contains("enum")) {
            const json options = schema["enum"];
            bool matched = false;
            for (std::size_t i = 0; i < options.size() && !matched; ++i)
                matched = json_equal(value, options[i]);
            if (!matched) return fail(error, path, "is not in enum");
        }
        if (value.is_number()) {
            const long double number = number_of(value);
            if (schema.contains("minimum") && number < number_of(schema["minimum"]))
                return fail(error, path, "is less than the minimum " + schema["minimum"].dump());
            if (schema.contains("maximum") && number > number_of(schema["maximum"]))
                return fail(error, path, "is greater than the maximum " + schema["maximum"].dump());
            if (schema.contains("exclusiveMinimum") &&
                number <= number_of(schema["exclusiveMinimum"]))
                return fail(error, path,
                            "is not greater than exclusiveMinimum " + schema["exclusiveMinimum"].dump());
            if (schema.contains("exclusiveMaximum") &&
                number >= number_of(schema["exclusiveMaximum"]))
                return fail(error, path,
                            "is not less than exclusiveMaximum " + schema["exclusiveMaximum"].dump());
            if (schema.contains("multipleOf")) {
                const json divisor = schema["multipleOf"];
                bool multiple = false;
                if (value.is_number_integer() && divisor.is_number_integer() &&
                    !value.is_number_unsigned() && !divisor.is_number_unsigned()) {
                    multiple = value.get<long long>() % divisor.get<long long>() == 0;
                } else {
                    const long double quotient = number / number_of(divisor);
                    multiple = std::isfinite(static_cast<double>(quotient)) &&
                               std::floor(quotient) == quotient;
                }
                if (!multiple) return fail(error, path, "is not a multiple of " + divisor.dump());
            }
        }
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            const auto length = static_cast<long double>(code_points(text));
            if (schema.contains("minLength") && length < number_of(schema["minLength"]))
                return fail(error, path, "is shorter than minLength " + schema["minLength"].dump());
            if (schema.contains("maxLength") && length > number_of(schema["maxLength"]))
                return fail(error, path, "is longer than maxLength " + schema["maxLength"].dump());
            if (schema.contains("pattern") &&
                !pattern_matches(text, schema["pattern"].get<std::string>()))
                return fail(error, path, "does not match pattern " + schema["pattern"].dump());
        }
        return true;
    }

    bool check_array(const json& value, const json& schema, const std::string& path,
                     const json* resource, std::size_t depth, Annotations& local,
                     std::string& error) {
        const std::size_t size = value.size();
        const auto count = static_cast<long double>(size);
        if (schema.contains("minItems") && count < number_of(schema["minItems"]))
            return fail(error, path, "has fewer than minItems " + schema["minItems"].dump());
        if (schema.contains("maxItems") && count > number_of(schema["maxItems"]))
            return fail(error, path, "has more than maxItems " + schema["maxItems"].dump());
        if (schema.contains("uniqueItems") && schema["uniqueItems"].get<bool>()) {
            for (std::size_t i = 0; i < size; ++i)
                for (std::size_t j = i + 1; j < size; ++j)
                    if (json_equal(value[i], value[j]))
                        return fail(error, path, "has duplicate items");
        }
        std::size_t prefix = 0;
        if (schema.contains("prefixItems")) {
            const json items = schema["prefixItems"];
            prefix = std::min(size, items.size());
            for (std::size_t i = 0; i < prefix; ++i) {
                Annotations ignored;
                if (!evaluate(value[i], items[i], path + "/" + std::to_string(i), resource,
                              depth + 1, ignored, error))
                    return false;
            }
            local.prefix_items = std::max(local.prefix_items, prefix);
        }
        if (schema.contains("items")) {
            const json items = schema["items"];
            for (std::size_t i = prefix; i < size; ++i) {
                Annotations ignored;
                if (!evaluate(value[i], items, path + "/" + std::to_string(i), resource,
                              depth + 1, ignored, error))
                    return false;
            }
            local.all_items = true;
        }
        if (schema.contains("contains")) {
            const json contains = schema["contains"];
            std::size_t matched = 0;
            for (std::size_t i = 0; i < size; ++i) {
                Annotations ignored;
                std::string ignored_error;
                if (evaluate(value[i], contains, path + "/" + std::to_string(i), resource,
                             depth + 1, ignored, ignored_error)) {
                    ++matched;
                    local.contained.insert(i);
                }
            }
            const long double minimum =
                schema.contains("minContains") ? number_of(schema["minContains"]) : 1;
            if (static_cast<long double>(matched) < minimum)
                return fail(error, path, "does not contain enough items matching contains");
            if (schema.contains("maxContains") &&
                static_cast<long double>(matched) > number_of(schema["maxContains"]))
                return fail(error, path, "contains more than maxContains matching items");
        }
        return true;
    }

    bool check_object(const json& value, const json& schema, const std::string& path,
                      const json* resource, std::size_t depth, Annotations& local,
                      std::string& error) {
        const auto count = static_cast<long double>(value.size());
        if (schema.contains("minProperties") && count < number_of(schema["minProperties"]))
            return fail(error, path, "has fewer than minProperties " + schema["minProperties"].dump());
        if (schema.contains("maxProperties") && count > number_of(schema["maxProperties"]))
            return fail(error, path, "has more than maxProperties " + schema["maxProperties"].dump());
        if (schema.contains("required")) {
            const json required = schema["required"];
            for (std::size_t i = 0; i < required.size(); ++i) {
                const auto name = required[i].get<std::string>();
                if (!value.contains(name))
                    return fail(error, path, "is missing required property " + name);
            }
        }
        if (schema.contains("dependentRequired")) {
            const json dependent = schema["dependentRequired"];
            for (auto it = dependent.begin(); it != dependent.end(); ++it) {
                const std::string trigger = it.key();
                if (!value.contains(trigger)) continue;
                const json names = it.value();
                for (std::size_t i = 0; i < names.size(); ++i) {
                    const auto name = names[i].get<std::string>();
                    if (!value.contains(name))
                        return fail(error, path,
                                    "is missing property " + name + " required by " + trigger);
                }
            }
        }
        const json properties =
            schema.contains("properties") ? schema["properties"] : json::object();
        const json patterns =
            schema.contains("patternProperties") ? schema["patternProperties"] : json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key = it.key();
            const json member = it.value();
            const std::string member_path = path + "/" + key;
            bool evaluated = false;
            if (properties.contains(key)) {
                Annotations ignored;
                if (!evaluate(member, properties[key], member_path, resource, depth + 1, ignored,
                              error))
                    return false;
                evaluated = true;
            }
            for (auto pattern = patterns.begin(); pattern != patterns.end(); ++pattern) {
                if (!pattern_matches(key, pattern.key())) continue;
                Annotations ignored;
                if (!evaluate(member, pattern.value(), member_path, resource, depth + 1, ignored,
                              error))
                    return false;
                evaluated = true;
            }
            if (!evaluated && schema.contains("additionalProperties")) {
                const json additional = schema["additionalProperties"];
                if (additional.is_boolean() && !additional.get<bool>())
                    return fail(error, path, "has unexpected property " + key);
                Annotations ignored;
                if (!evaluate(member, additional, member_path, resource, depth + 1, ignored, error))
                    return false;
                evaluated = true;
            }
            if (evaluated) local.properties.insert(key);
            if (schema.contains("propertyNames")) {
                Annotations ignored;
                std::string ignored_error;
                if (!evaluate(json(key), schema["propertyNames"], member_path, resource, depth + 1,
                              ignored, ignored_error))
                    return fail(error, path,
                                "has a property name " + json(key).dump() +
                                    " that its propertyNames schema rejects");
            }
        }
        return true;
    }

    bool check_applicators(const json& value, const json& schema, const std::string& path,
                           const json* resource, std::size_t depth, Annotations& local,
                           std::string& error) {
        if (schema.contains("allOf")) {
            const json all = schema["allOf"];
            for (std::size_t i = 0; i < all.size(); ++i) {
                Annotations branch;
                if (!evaluate(value, all[i], path, resource, depth + 1, branch, error)) return false;
                local.merge(branch);
            }
        }
        if (schema.contains("anyOf")) {
            const json any = schema["anyOf"];
            bool matched = false;
            std::string first_error;
            for (std::size_t i = 0; i < any.size(); ++i) {
                Annotations branch;
                std::string branch_error;
                if (evaluate(value, any[i], path, resource, depth + 1, branch, branch_error)) {
                    matched = true;
                    local.merge(branch);
                } else if (first_error.empty()) {
                    first_error = branch_error;
                }
            }
            if (!matched)
                return fail(error, path, "does not match any schema in anyOf (" + first_error + ")");
        }
        if (schema.contains("oneOf")) {
            const json one = schema["oneOf"];
            std::size_t matched = 0;
            Annotations chosen;
            std::string first_error;
            for (std::size_t i = 0; i < one.size(); ++i) {
                Annotations branch;
                std::string branch_error;
                if (evaluate(value, one[i], path, resource, depth + 1, branch, branch_error)) {
                    if (++matched == 1) chosen = std::move(branch);
                } else if (first_error.empty()) {
                    first_error = branch_error;
                }
            }
            if (matched == 0)
                return fail(error, path, "does not match any schema in oneOf (" + first_error + ")");
            if (matched > 1) return fail(error, path, "matches more than one schema in oneOf");
            local.merge(chosen);
        }
        if (schema.contains("not")) {
            Annotations ignored;
            std::string ignored_error;
            if (evaluate(value, schema["not"], path, resource, depth + 1, ignored, ignored_error))
                return fail(error, path, "matches the schema in not");
        }
        if (schema.contains("if")) {
            Annotations condition;
            std::string ignored_error;
            if (evaluate(value, schema["if"], path, resource, depth + 1, condition, ignored_error)) {
                local.merge(condition);
                if (schema.contains("then")) {
                    Annotations branch;
                    if (!evaluate(value, schema["then"], path, resource, depth + 1, branch, error))
                        return false;
                    local.merge(branch);
                }
            } else if (schema.contains("else")) {
                Annotations branch;
                if (!evaluate(value, schema["else"], path, resource, depth + 1, branch, error))
                    return false;
                local.merge(branch);
            }
        }
        if (value.is_object() && schema.contains("dependentSchemas")) {
            const json dependent = schema["dependentSchemas"];
            for (auto it = dependent.begin(); it != dependent.end(); ++it) {
                if (!value.contains(it.key())) continue;
                Annotations branch;
                if (!evaluate(value, it.value(), path, resource, depth + 1, branch, error))
                    return false;
                local.merge(branch);
            }
        }
        return true;
    }

    bool check_unevaluated(const json& value, const json& schema, const std::string& path,
                           const json* resource, std::size_t depth, Annotations& local,
                           std::string& error) {
        if (value.is_array() && schema.contains("unevaluatedItems") && !local.all_items) {
            const json unevaluated = schema["unevaluatedItems"];
            for (std::size_t i = local.prefix_items; i < value.size(); ++i) {
                if (local.contained.contains(i)) continue;
                if (unevaluated.is_boolean() && !unevaluated.get<bool>())
                    return fail(error, path,
                                "has an unevaluated item at index " + std::to_string(i));
                Annotations ignored;
                if (!evaluate(value[i], unevaluated, path + "/" + std::to_string(i), resource,
                              depth + 1, ignored, error))
                    return false;
            }
            local.all_items = true;
        }
        if (value.is_object() && schema.contains("unevaluatedProperties")) {
            const json unevaluated = schema["unevaluatedProperties"];
            for (auto it = value.begin(); it != value.end(); ++it) {
                const std::string key = it.key();
                if (local.properties.contains(key)) continue;
                if (unevaluated.is_boolean() && !unevaluated.get<bool>())
                    return fail(error, path, "has unexpected property " + key);
                Annotations ignored;
                if (!evaluate(it.value(), unevaluated, path + "/" + key, resource, depth + 1,
                              ignored, error))
                    return false;
                local.properties.insert(key);
            }
        }
        return true;
    }

    const SchemaIndex& index_;
    std::string subject_;
};

} // namespace

void validate_json_schema(const json& schema, const std::string& path) {
    const SchemaIndex index(schema);
    check_schema(schema, path, 0, index, &schema);
}

void validate_json_value(const json& value, const json& schema, const std::string& subject,
                         const std::string& path) {
    const SchemaIndex index(schema);
    check_schema(schema, path, 0, index, &schema);
    Evaluator evaluator(index, subject);
    Annotations annotations;
    std::string error;
    if (!evaluator.evaluate(value, schema, path, &schema, 0, annotations, error))
        throw std::invalid_argument(error);
}

} // namespace neograph::mcp
