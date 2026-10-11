// A small JSON Schema (draft 2020-12) checker for tests: the keywords the
// NeoGraph schemas under schemas/ use. Unknown keywords are annotations.
// Only local references ("#/$defs/...") are resolved.
#pragma once

#include <neograph/json.h>

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace json_schema_lite {

using neograph::json;

class Checker {
public:
    explicit Checker(json root) : root_(std::move(root)) {}

    /// Every violation, each as "<instance pointer>: <keyword> ...".
    std::vector<std::string> check(const json& value) const {
        std::vector<std::string> errors;
        visit(value, root_, "", errors, 0);
        return errors;
    }

private:
    static std::string type_of(const json& value) {
        if (value.is_null()) return "null";
        if (value.is_boolean()) return "boolean";
        if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
        if (value.is_number()) {
            const auto number = value.get<double>();
            return std::trunc(number) == number ? "integer" : "number";
        }
        if (value.is_string()) return "string";
        if (value.is_array()) return "array";
        return "object";
    }

    static bool type_matches(const json& value, const std::string& type) {
        const auto actual = type_of(value);
        return actual == type || (type == "number" && actual == "integer");
    }

    static std::size_t code_points(const std::string& text) {
        std::size_t count = 0;
        for (const unsigned char byte : text)
            if ((byte & 0xC0U) != 0x80U) ++count;
        return count;
    }

    json resolve(const std::string& reference) const {
        if (reference.rfind("#/", 0) != 0)
            throw std::invalid_argument("json_schema_lite resolves only local references: " + reference);
        json node = root_;
        std::size_t begin = 2;
        while (begin <= reference.size()) {
            const auto end   = reference.find('/', begin);
            auto       token = reference.substr(begin, end == std::string::npos ? std::string::npos
                                                                                 : end - begin);
            for (std::size_t at = 0; (at = token.find("~1", at)) != std::string::npos;) token.replace(at, 2, "/");
            for (std::size_t at = 0; (at = token.find("~0", at)) != std::string::npos;) token.replace(at, 2, "~");
            if (!node.contains(token)) throw std::invalid_argument("unresolved reference " + reference);
            json next = node[token];
            node      = std::move(next);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return node;
    }

    bool valid(const json& value, const json& schema, int depth) const {
        std::vector<std::string> errors;
        visit(value, schema, "", errors, depth);
        return errors.empty();
    }

    void visit(const json& value, const json& schema, const std::string& at,
               std::vector<std::string>& errors, int depth) const {
        if (depth > 64) throw std::invalid_argument("json_schema_lite: reference cycle");
        if (schema.is_boolean()) {
            if (!schema.get<bool>()) errors.push_back(at + ": false schema");
            return;
        }
        if (!schema.is_object()) throw std::invalid_argument("schema must be an object or boolean");
        const auto fail = [&](const std::string& message) { errors.push_back(at + ": " + message); };

        if (schema.contains("$ref")) visit(value, resolve(schema["$ref"].get<std::string>()), at, errors, depth + 1);
        if (schema.contains("type")) {
            const auto& type = schema["type"];
            bool        ok   = false;
            if (type.is_string()) {
                ok = type_matches(value, type.get<std::string>());
            } else {
                for (const auto& option : type) ok = ok || type_matches(value, option.get<std::string>());
            }
            if (!ok) {
                fail("type " + type.dump() + " expected, got " + type_of(value));
                return;
            }
        }
        if (schema.contains("const") && !(schema["const"] == value)) fail("const " + schema["const"].dump());
        if (schema.contains("enum")) {
            bool found = false;
            for (const auto& option : schema["enum"]) found = found || option == value;
            if (!found) fail("enum " + schema["enum"].dump());
        }
        for (const char* keyword : {"allOf", "anyOf", "oneOf"}) {
            if (!schema.contains(keyword)) continue;
            std::size_t matched = 0;
            for (const auto& option : schema[keyword]) matched += valid(value, option, depth + 1) ? 1 : 0;
            const auto  count = schema[keyword].size();
            const std::string name(keyword);
            if (name == "allOf" && matched != count) fail("allOf");
            if (name == "anyOf" && matched == 0) fail("anyOf");
            if (name == "oneOf" && matched != 1) fail("oneOf matched " + std::to_string(matched));
        }
        if (schema.contains("not") && valid(value, schema["not"], depth + 1)) fail("not");
        if (schema.contains("if")) {
            if (valid(value, schema["if"], depth + 1)) {
                if (schema.contains("then")) visit(value, schema["then"], at, errors, depth + 1);
            } else if (schema.contains("else")) {
                visit(value, schema["else"], at, errors, depth + 1);
            }
        }
        if (value.is_number()) {
            const auto number = value.get<double>();
            if (schema.contains("minimum") && number < schema["minimum"].get<double>()) fail("minimum");
            if (schema.contains("maximum") && number > schema["maximum"].get<double>()) fail("maximum");
            if (schema.contains("exclusiveMinimum") && number <= schema["exclusiveMinimum"].get<double>())
                fail("exclusiveMinimum");
            if (schema.contains("exclusiveMaximum") && number >= schema["exclusiveMaximum"].get<double>())
                fail("exclusiveMaximum");
        }
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            if (schema.contains("minLength") && code_points(text) < schema["minLength"].get<std::size_t>())
                fail("minLength");
            if (schema.contains("maxLength") && code_points(text) > schema["maxLength"].get<std::size_t>())
                fail("maxLength");
            if (schema.contains("pattern") &&
                !std::regex_search(text, std::regex(schema["pattern"].get<std::string>(), std::regex::ECMAScript)))
                fail("pattern " + schema["pattern"].get<std::string>());
        }
        if (value.is_array()) {
            if (schema.contains("minItems") && value.size() < schema["minItems"].get<std::size_t>()) fail("minItems");
            if (schema.contains("maxItems") && value.size() > schema["maxItems"].get<std::size_t>()) fail("maxItems");
            if (schema.contains("uniqueItems") && schema["uniqueItems"].get<bool>()) {
                std::set<std::string> seen;
                for (const auto& item : value)
                    if (!seen.insert(item.dump()).second) fail("uniqueItems");
            }
            std::size_t prefix = 0;
            if (schema.contains("prefixItems")) {
                prefix = schema["prefixItems"].size();
                for (std::size_t index = 0; index < std::min(prefix, value.size()); ++index)
                    visit(value[index], schema["prefixItems"][index], at + "/" + std::to_string(index),
                          errors, depth + 1);
            }
            if (schema.contains("items"))
                for (std::size_t index = prefix; index < value.size(); ++index)
                    visit(value[index], schema["items"], at + "/" + std::to_string(index), errors, depth + 1);
        }
        if (value.is_object()) {
            if (schema.contains("minProperties") && value.size() < schema["minProperties"].get<std::size_t>())
                fail("minProperties");
            if (schema.contains("maxProperties") && value.size() > schema["maxProperties"].get<std::size_t>())
                fail("maxProperties");
            if (schema.contains("required"))
                for (const auto& name : schema["required"])
                    if (!value.contains(name.get<std::string>())) fail("required " + name.get<std::string>());
            for (const auto& [key, item] : value.items()) {
                const auto child   = at + "/" + key;
                bool       matched = false;
                if (schema.contains("properties") && schema["properties"].contains(key)) {
                    matched = true;
                    visit(item, schema["properties"][key], child, errors, depth + 1);
                }
                if (schema.contains("patternProperties")) {
                    for (const auto& [pattern, sub] : schema["patternProperties"].items()) {
                        if (!std::regex_search(key, std::regex(pattern, std::regex::ECMAScript))) continue;
                        matched = true;
                        visit(item, sub, child, errors, depth + 1);
                    }
                }
                if (!matched && schema.contains("additionalProperties"))
                    visit(item, schema["additionalProperties"], child, errors, depth + 1);
                if (schema.contains("propertyNames"))
                    visit(json(key), schema["propertyNames"], child + " (name)", errors, depth + 1);
            }
        }
    }

    json root_;
};

}  // namespace json_schema_lite
