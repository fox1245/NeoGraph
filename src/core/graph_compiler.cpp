#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <neograph/graph/registry.h>

#include "routing_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace neograph::graph {

namespace {

// Keys starting with '_' or 'x-' are annotations: for humans
// (`_comment`, used across the cookbook corpus) and external tooling
// (`x-studio-pos`). The engine never consumes them, strict mode never
// flags them, and canon() strips them before equivalence comparison.
bool is_annotation_key(const std::string& k) {
    return (!k.empty() && k[0] == '_') || k.rfind("x-", 0) == 0;
}

std::string json_type_name(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "unknown";
}

std::string pointer_token(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '~')
            escaped += "~0";
        else if (ch == '/')
            escaped += "~1";
        else
            escaped.push_back(ch);
    }
    return escaped;
}

bool schema_type_matches(const json& value, const std::string& type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") {
        return value.is_number_integer() || value.is_number_unsigned();
    }
    if (type == "number") return value.is_number();
    if (type == "string") return value.is_string();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    return false;
}

std::vector<std::string> declared_schema_types(const json& type_keyword) {
    std::vector<std::string> types;
    if (type_keyword.is_string()) {
        types.push_back(type_keyword.get<std::string>());
    } else if (type_keyword.is_array()) {
        for (const auto& type : type_keyword) {
            if (type.is_string()) types.push_back(type.get<std::string>());
        }
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

std::string join_types(const std::vector<std::string>& types) {
    std::string out;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i) out += " or ";
        out += types[i];
    }
    return out;
}

// NeoGraph intentionally enforces only required/type/enum from registered node
// schemas. `properties` is traversed to reach those keywords; descriptions,
// defaults, items, oneOf, and other JSON Schema features remain annotations.
void validate_node_config_schema(const json&                      value,
                                 const json&                      schema,
                                 const std::string&               path,
                                 const std::string&               pointer,
                                 const std::string&               node_type,
                                 std::vector<CompilerDiagnostic>& diagnostics) {
    if (!schema.is_object()) return;
    const std::string type_suffix = " [node type '" + node_type + "']";
    if (schema.contains("type")) {
        const auto types   = declared_schema_types(schema["type"]);
        bool       matched = false;
        for (const auto& type : types) {
            if (schema_type_matches(value, type)) {
                matched = true;
                break;
            }
        }
        if (types.empty()) {
            diagnostics.push_back(
                {"GC_NODE_CONFIG", pointer,
                 path + ": keyword 'type' must be a string or array of strings" + type_suffix,
                 json{{"node_type", node_type},
                      {"keyword", "type"},
                      {"expected", "string or array of strings"},
                      {"actual", schema["type"]}}});
        } else if (!matched) {
            diagnostics.push_back({"GC_NODE_CONFIG", pointer,
                                   path + ": keyword 'type' expected " + join_types(types) +
                                       ", got " + json_type_name(value) + type_suffix,
                                   json{{"node_type", node_type},
                                        {"keyword", "type"},
                                        {"expected", schema["type"]},
                                        {"actual", value}}});
        }
    }
    if (schema.contains("enum")) {
        const auto& choices = schema["enum"];
        if (!choices.is_array()) {
            diagnostics.push_back({"GC_NODE_CONFIG", pointer,
                                   path + ": keyword 'enum' must be an array" + type_suffix,
                                   json{{"node_type", node_type},
                                        {"keyword", "enum"},
                                        {"expected", "array"},
                                        {"actual", choices}}});
        } else {
            bool matched = false;
            for (const auto& choice : choices) {
                if (choice == value) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                diagnostics.push_back({"GC_NODE_CONFIG", pointer,
                                       path + ": keyword 'enum' expected one of " + choices.dump() +
                                           ", got " + value.dump() + type_suffix,
                                       json{{"node_type", node_type},
                                            {"keyword", "enum"},
                                            {"expected", choices},
                                            {"actual", value}}});
            }
        }
    }
    if (!value.is_object()) return;
    if (schema.contains("required")) {
        const auto& required = schema["required"];
        if (!required.is_array()) {
            diagnostics.push_back({"GC_NODE_CONFIG", pointer,
                                   path + ": keyword 'required' must be an array" + type_suffix,
                                   json{{"node_type", node_type},
                                        {"keyword", "required"},
                                        {"expected", "array"},
                                        {"actual", required}}});
        } else {
            std::vector<std::string> names;
            for (const auto& name : required) {
                if (name.is_string()) names.push_back(name.get<std::string>());
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            for (const auto& name : names) {
                if (!value.contains(name)) {
                    diagnostics.push_back({"GC_NODE_CONFIG", pointer + "/" + pointer_token(name),
                                           path + ": keyword 'required' missing property '" + name +
                                               "'" + type_suffix,
                                           json{{"node_type", node_type},
                                                {"keyword", "required"},
                                                {"expected", name},
                                                {"actual", "missing"}}});
                }
            }
        }
    }
    if (!schema.contains("properties") || !schema["properties"].is_object()) return;
    std::vector<std::string> property_names;
    for (const auto& [name, property_schema] : schema["properties"].items()) {
        (void)property_schema;
        property_names.push_back(name);
    }
    std::sort(property_names.begin(), property_names.end());
    for (const auto& name : property_names) {
        if (!value.contains(name)) continue;
        validate_node_config_schema(value[name], schema["properties"][name], path + "." + name,
                                    pointer + "/" + pointer_token(name), node_type, diagnostics);
    }
}

// Consumed-key accounting: every object the compiler owns gets a
// consumed-set populated AT THE READ SITE (inside the same block that
// parses the key — never a detached allowlist). If a parsing step is
// deleted, its mark disappears with it, and any strict document using
// that feature fails here instead of silently degrading. This is the
// structural fix for the v0.1.0–v0.1.7 conditional_edges drop class.
void enforce_consumed(const json&                      obj,
                      const std::set<std::string>&     consumed,
                      const std::string&               path,
                      const std::string&               pointer,
                      std::vector<CompilerDiagnostic>& diagnostics) {
    if (!obj.is_object()) return;
    for (const auto& [k, v] : obj.items()) {
        (void)v;
        if (is_annotation_key(k)) continue;
        if (!consumed.count(k)) {
            diagnostics.push_back({"GC_UNKNOWN_FIELD", pointer + "/" + pointer_token(k),
                                   path + ": unknown or unconsumed key '" + k + "'",
                                   json{{"field", k}}});
        }
    }
}

// retry_policy carries a float member; canon() must push input numbers
// through the same float truncation the parser applies, or TV would
// flag e.g. 0.1 (double) != 0.1f (parsed) as a phantom mismatch.
json canon_backoff(const json& v) {
    return json(static_cast<float>(v.get<double>()));
}

// neograph::json is yyjson-backed: objects preserve insertion order and
// operator== compares serializations, so canonical forms must rebuild
// every object with sorted keys (recursively) or two semantically equal
// documents would compare unequal.
json sort_keys(const json& v) {
    if (v.is_object()) {
        std::vector<std::string> keys;
        for (const auto& [k, val] : v.items()) {
            (void)val;
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        json out = json::object();
        for (const auto& k : keys)
            out[k] = sort_keys(v[k]);
        return out;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto& e : v)
            out.push_back(sort_keys(e));
        return out;
    }
    return v;
}

// Human-readable structural mismatch report for translation-validation
// failures (the wrapper has no json::diff). Phrased from the compiler's
// point of view: input-only = lost, reemit-only = fabricated.
void collect_mismatches(const json&                      declared,
                        const json&                      compiled,
                        const std::string&               pointer,
                        std::vector<CompilerDiagnostic>& diagnostics) {
    constexpr size_t kMax = 8;
    if (diagnostics.size() >= kMax || declared == compiled) return;
    if (declared.is_object() && compiled.is_object()) {
        for (const auto& [k, v] : declared.items()) {
            if (diagnostics.size() >= kMax) return;
            const std::string child = pointer + "/" + pointer_token(k);
            if (!compiled.contains(k)) {
                diagnostics.push_back({"GC_ROUNDTRIP", child, child + ": lost in compilation",
                                       json{{"kind", "lost"}, {"declared", v}}});
            } else {
                collect_mismatches(v, compiled[k], child, diagnostics);
            }
        }
        for (const auto& [k, v] : compiled.items()) {
            if (diagnostics.size() >= kMax) return;
            if (!declared.contains(k)) {
                const std::string child = pointer + "/" + pointer_token(k);
                diagnostics.push_back({"GC_ROUNDTRIP", child, child + ": fabricated by compilation",
                                       json{{"kind", "fabricated"}, {"compiled", v}}});
            }
        }
        return;
    }
    if (declared.is_array() && compiled.is_array()) {
        if (declared.size() != compiled.size()) {
            diagnostics.push_back(
                {"GC_ROUNDTRIP", pointer,
                 pointer + ": " + std::to_string(declared.size()) + " element(s) declared, " +
                     std::to_string(compiled.size()) + " compiled",
                 json{{"kind", "different"}, {"declared", declared}, {"compiled", compiled}}});
            return;
        }
        for (size_t i = 0; i < declared.size(); ++i) {
            if (diagnostics.size() >= kMax) return;
            collect_mismatches(declared[i], compiled[i], pointer + "/" + std::to_string(i),
                               diagnostics);
        }
        return;
    }
    diagnostics.push_back(
        {"GC_ROUNDTRIP", pointer,
         pointer + ": declared " + declared.dump() + ", compiled " + compiled.dump(),
         json{{"kind", "different"}, {"declared", declared}, {"compiled", compiled}}});
}

// Normalize one conditional edge (either the legacy inline `edges`
// form or a top-level `conditional_edges` item) for canon(). Unknown
// keys are preserved so TV flags anything the compiler ignored;
// annotations are stripped; empty routes are dropped.
json canon_conditional(const json& e) {
    json out = json::object();
    for (const auto& [k, v] : e.items()) {
        if (is_annotation_key(k)) continue;
        if (k == "type") continue;  // form marker, not meaning
        if (k == "routes") {
            if (v.is_object() && !v.empty()) out["routes"] = v;
            continue;
        }
        out[k] = v;
    }
    return out;
}

}  // namespace

bool ParseReport::has_errors() const noexcept {
    return !diagnostics.empty();
}

std::string ParseReport::summary() const {
    if (diagnostics.empty()) return {};
    if (diagnostics.size() == 1 && (diagnostics.front().code == "GC_SCHEMA_VERSION" ||
                                    diagnostics.front().code == "GC_REGISTRY_MISS")) {
        return diagnostics.front().message;
    }
    std::string message =
        "strict topology validation failed, " + std::to_string(diagnostics.size()) + " error(s):";
    for (const auto& diagnostic : diagnostics) {
        message += "\n  - " + diagnostic.message;
    }
    message +=
        "\nAnnotation keys ('_'/'x-' prefixes) are always allowed. "
        "Remove 'schema_version' to fall back to lenient parsing. "
        "See docs/troubleshooting.md \"Strict topology validation\".";
    return message;
}

bool RoundTripReport::has_errors() const noexcept {
    return !diagnostics.empty();
}

std::string RoundTripReport::summary() const {
    if (diagnostics.empty()) return {};
    std::string message =
        "translation validation failed: compiled graph does not round-trip "
        "to its definition — the compiler dropped or rewired something:";
    for (const auto& diagnostic : diagnostics) {
        message += "\n  - " + diagnostic.message;
    }
    return message;
}

CompiledGraph GraphCompiler::compile(const json& definition, const NodeContext& default_context) {
    return compile(definition, default_context, GraphRegistry::global());
}

CompiledGraph GraphCompiler::compile(const json&          definition,
                                     const NodeContext&   default_context,
                                     const GraphRegistry& registry) {
    return link(parse(definition, registry), default_context, registry);
}

CompiledGraph GraphCompiler::compile_local(const json&          definition,
                                           const NodeContext&   default_context,
                                           const GraphRegistry& registry) {
    return link_local(parse_local(definition, registry), default_context, registry);
}

TopologySpec GraphCompiler::parse(const json& definition) {
    return parse(definition, GraphRegistry::global());
}

TopologySpec GraphCompiler::parse(const json& definition, const GraphRegistry& registry) {
    auto report = parse_report(definition, registry);
    if (report.has_errors()) throw std::runtime_error(report.summary());
    return std::move(*report.topology);
}

static ParseReport parse_report_impl(const json&          definition,
                                     const GraphRegistry& registry,
                                     bool                 local_only) {
    ParseReport           report;
    TopologySpec          topology;
    auto&                 diagnostics = report.diagnostics;
    std::set<std::string> top_consumed;
    bool                  strict = false;

    auto field_type = [&](std::string pointer, std::string display, std::string expected,
                          const json& actual) {
        diagnostics.push_back(
            {"GC_FIELD_TYPE", std::move(pointer),
             std::move(display) + " must be an " + expected + ", got " + json_type_name(actual),
             json{{"expected", std::move(expected)}, {"actual", json_type_name(actual)}}});
    };
    auto required = [&](std::string pointer, const std::string& display, const std::string& field) {
        diagnostics.push_back({"GC_FIELD_REQUIRED", std::move(pointer),
                               display + ": required field '" + field + "' is missing",
                               json{{"field", field}}});
    };
    auto read_string = [&](const json& object, const std::string& field, const std::string& pointer,
                           const std::string& display, std::string& output) {
        if (!object.contains(field)) {
            required(pointer, display, field);
            return false;
        }
        const auto& value = object[field];
        if (!value.is_string()) {
            field_type(pointer, display, "string", value);
            return false;
        }
        output = value.get<std::string>();
        if (strict && output.empty()) {
            diagnostics.push_back({"GC_FIELD_VALUE", pointer, display + " must not be empty",
                                   json{{"expected", "non-empty string"}, {"actual", output}}});
            return false;
        }
        return true;
    };

    if (!definition.is_object()) {
        field_type("", "topology document", "object", definition);
        return report;
    }

    if (definition.contains("schema_version")) {
        top_consumed.insert("schema_version");
        const auto& value = definition["schema_version"];
        if (!value.is_number_integer()) {
            diagnostics.push_back(
                {"GC_SCHEMA_VERSION", "/schema_version",
                 "topology 'schema_version' must be a non-negative integer, got: " + value.dump(),
                 json{{"actual", value}, {"maximum", TOPOLOGY_SCHEMA_VERSION}}});
            return report;
        }
        const auto version = value.get<std::int64_t>();
        if (version < 0 || version > std::numeric_limits<int>::max()) {
            diagnostics.push_back(
                {"GC_SCHEMA_VERSION", "/schema_version",
                 "topology 'schema_version' must be a non-negative integer, got: " + value.dump(),
                 json{{"actual", value}, {"maximum", TOPOLOGY_SCHEMA_VERSION}}});
            return report;
        }
        topology.schema_version = static_cast<int>(version);
        if (topology.schema_version > TOPOLOGY_SCHEMA_VERSION) {
            diagnostics.push_back(
                {"GC_SCHEMA_VERSION", "/schema_version",
                 "topology schema_version " + std::to_string(topology.schema_version) +
                     " is newer than this engine supports (max " +
                     std::to_string(TOPOLOGY_SCHEMA_VERSION) +
                     "). Upgrade NeoGraph or re-export the topology.",
                 json{{"actual", topology.schema_version}, {"maximum", TOPOLOGY_SCHEMA_VERSION}}});
            return report;
        }
    }
    strict = topology.schema_version >= 1;

    auto container_ok = [&](const char* field, bool object, bool legacy_object_allowed = false) {
        if (!definition.contains(field)) return false;
        top_consumed.insert(field);
        const auto& value = definition[field];
        if (legacy_object_allowed && !strict && value.is_object()) return true;
        if (object ? !value.is_object() : !value.is_array()) {
            field_type("/" + std::string(field), "topology '$." + std::string(field) + "'",
                       object ? "object" : "array", value);
            return false;
        }
        return true;
    };
    const bool channels_ok          = container_ok("channels", true);
    const bool nodes_ok             = container_ok("nodes", true);
    const bool edges_ok             = container_ok("edges", false, true);
    const bool conditional_edges_ok = container_ok("conditional_edges", false, true);

    topology.name = "unnamed_graph";
    top_consumed.insert("name");
    if (definition.contains("name")) {
        if (definition["name"].is_string()) {
            topology.name = definition["name"].get<std::string>();
            if (strict && topology.name.empty()) {
                diagnostics.push_back(
                    {"GC_FIELD_VALUE", "/name", "topology '$.name' must not be empty",
                     json{{"expected", "non-empty string"}, {"actual", topology.name}}});
            }
        } else {
            field_type("/name", "topology '$.name'", "string", definition["name"]);
        }
    }

    if (channels_ok) {
        for (const auto& [name, channel] : definition["channels"].items()) {
            const std::string pointer = "/channels/" + pointer_token(name);
            if (strict && name.empty()) {
                diagnostics.push_back({"GC_FIELD_VALUE", pointer, "channel name must not be empty",
                                       json{{"expected", "non-empty string"}, {"actual", name}}});
            }
            if (!channel.is_object()) {
                field_type(pointer, "topology 'channels." + name + "'", "object", channel);
                continue;
            }
            ChannelDef parsed;
            parsed.name         = name;
            parsed.reducer_name = "overwrite";
            if (channel.contains("reducer")) {
                if (channel["reducer"].is_string()) {
                    parsed.reducer_name = channel["reducer"].get<std::string>();
                    if (strict && parsed.reducer_name.empty()) {
                        diagnostics.push_back({"GC_FIELD_VALUE", pointer + "/reducer",
                                               "channels." + name + ".reducer must not be empty",
                                               json{{"expected", "non-empty string"},
                                                    {"actual", parsed.reducer_name}}});
                    }
                } else {
                    field_type(pointer + "/reducer", "channels." + name + ".reducer", "string",
                               channel["reducer"]);
                }
            }
            parsed.has_initial   = channel.contains("initial");
            parsed.initial_value = parsed.has_initial ? channel["initial"] : json();
            if (parsed.reducer_name == "append")
                parsed.type = ReducerType::APPEND;
            else if (parsed.reducer_name == "overwrite")
                parsed.type = ReducerType::OVERWRITE;
            else
                parsed.type = ReducerType::CUSTOM;
            if (strict) {
                enforce_consumed(channel, {"reducer", "initial"}, "channels." + name, pointer,
                                 diagnostics);
            }
            topology.channel_defs.push_back(std::move(parsed));
        }
    }

    if (nodes_ok) {
        for (const auto& [name, node] : definition["nodes"].items()) {
            const std::string pointer = "/nodes/" + pointer_token(name);
            if (strict && name.empty()) {
                diagnostics.push_back({"GC_FIELD_VALUE", pointer, "node name must not be empty",
                                       json{{"expected", "non-empty string"}, {"actual", name}}});
            }
            if (!node.is_object()) {
                field_type(pointer, "topology 'nodes." + name + "'", "object", node);
                continue;
            }
            topology.node_defs[name]       = node;
            std::set<std::string> consumed = {"type"};
            std::string           type;
            const bool            valid_type =
                read_string(node, "type", pointer + "/type", "nodes." + name, type);

            if (node.contains("barrier")) {
                consumed.insert("barrier");
                const auto&           barrier         = node["barrier"];
                const std::string     barrier_pointer = pointer + "/barrier";
                std::set<std::string> wait_for;
                bool                  malformed = !barrier.is_object();
                if (barrier.is_object() && barrier.contains("wait_for")) {
                    if (!barrier["wait_for"].is_array()) {
                        malformed = true;
                    } else {
                        std::size_t index = 0;
                        for (const auto& upstream : barrier["wait_for"]) {
                            if (!upstream.is_string()) {
                                malformed = true;
                                field_type(barrier_pointer + "/wait_for/" + std::to_string(index),
                                           "nodes." + name + ".barrier.wait_for", "string",
                                           upstream);
                            } else if (strict && upstream.get<std::string>().empty()) {
                                malformed = true;
                                diagnostics.push_back(
                                    {"GC_FIELD_VALUE",
                                     barrier_pointer + "/wait_for/" + std::to_string(index),
                                     "nodes." + name +
                                         ".barrier.wait_for entries must not be empty",
                                     json{{"expected", "non-empty string"}, {"actual", ""}}});
                            } else {
                                wait_for.insert(upstream.get<std::string>());
                            }
                            ++index;
                        }
                    }
                }
                if (malformed) {
                    diagnostics.push_back(
                        {"GC_BARRIER", barrier_pointer,
                         "nodes." + name +
                             ".barrier: 'wait_for' is malformed — expected an array of node names",
                         json{{"barrier", barrier},
                              {"wait_for", barrier.is_object() && barrier.contains("wait_for")
                                               ? barrier["wait_for"]
                                               : json::array()}}});
                } else if (!wait_for.empty()) {
                    topology.barrier_specs[name] = std::move(wait_for);
                } else if (strict) {
                    diagnostics.push_back(
                        {"GC_BARRIER", barrier_pointer,
                         "nodes." + name +
                             ".barrier: 'wait_for' is missing or empty — the barrier would be "
                             "silently dropped. List the upstream nodes to wait for, or remove "
                             "the 'barrier' block.",
                         json{{"barrier", barrier}, {"wait_for", json::array()}}});
                }
                if (strict && barrier.is_object()) {
                    enforce_consumed(barrier, {"wait_for"}, "nodes." + name + ".barrier",
                                     barrier_pointer, diagnostics);
                }
            }

            if (strict && valid_type) {
                const json schema = local_only && !registry.contains_type(type)
                                        ? json{{"type", "object"}}
                                        : (local_only ? registry.local_config_schema(type)
                                                      : registry.config_schema(type));
                json       config = json::object();
                for (const auto& [key, value] : node.items()) {
                    if (key == "type" || key == "barrier" || is_annotation_key(key)) continue;
                    if (key.empty()) {
                        diagnostics.push_back(
                            {"GC_FIELD_VALUE", pointer + "/",
                             "nodes." + name + " configuration keys must not be empty",
                             json{{"expected", "non-empty string"}, {"actual", key}}});
                    }
                    config[key] = value;
                }
                validate_node_config_schema(config, schema, "nodes." + name, pointer, type,
                                            diagnostics);
                bool open = !schema.contains("properties");
                if (schema.contains("additionalProperties")) {
                    const auto& additional = schema["additionalProperties"];
                    if (!additional.is_boolean() || additional.get<bool>()) open = true;
                }
                if (!open) {
                    for (const auto& [property, value] : schema["properties"].items()) {
                        (void)value;
                        consumed.insert(property);
                    }
                    enforce_consumed(node, consumed, "nodes." + name, pointer, diagnostics);
                }
            }
        }
    }

    auto parse_conditional = [&](const json& edge, const std::string& path,
                                 const std::string& pointer, bool inline_form) {
        if (!edge.is_object()) {
            diagnostics.push_back({"GC_EDGE", pointer,
                                   path + ": conditional edge must be an object",
                                   json{{"edge", edge}}});
            return;
        }
        ConditionalEdge       parsed;
        std::set<std::string> consumed = {"from", "condition", "routes"};
        if (inline_form) consumed.insert("type");
        bool valid_type = true;
        if (inline_form && edge.contains("type") && edge["type"].is_string() && strict &&
            edge["type"].get<std::string>() != "conditional") {
            diagnostics.push_back(
                {"GC_FIELD_VALUE", pointer + "/type",
                 path + ".type must equal 'conditional' when an inline edge has a condition",
                 json{{"expected", "conditional"}, {"actual", edge["type"]}}});
            valid_type = false;
        }
        const bool valid_from = read_string(edge, "from", pointer + "/from", path, parsed.from);
        const bool valid_condition =
            read_string(edge, "condition", pointer + "/condition", path, parsed.condition);
        bool valid_routes = true;
        if (edge.contains("routes")) {
            if (!edge["routes"].is_object()) {
                diagnostics.push_back({"GC_EDGE", pointer + "/routes",
                                       path + ".routes must be an object", json{{"edge", edge}}});
                valid_routes = false;
            } else {
                for (const auto& [key, target] : edge["routes"].items()) {
                    if (strict && key.empty()) {
                        diagnostics.push_back(
                            {"GC_FIELD_VALUE", pointer + "/routes/",
                             path + ".routes key must not be empty",
                             json{{"expected", "non-empty string"}, {"actual", key}}});
                        valid_routes = false;
                    }
                    if (!target.is_string()) {
                        field_type(pointer + "/routes/" + pointer_token(key),
                                   path + ".routes." + key, "string", target);
                        valid_routes = false;
                    } else if (strict && target.get<std::string>().empty()) {
                        diagnostics.push_back(
                            {"GC_FIELD_VALUE", pointer + "/routes/" + pointer_token(key),
                             path + ".routes." + key + " must not be empty",
                             json{{"expected", "non-empty string"}, {"actual", ""}}});
                        valid_routes = false;
                    } else {
                        parsed.routes[key] = target.get<std::string>();
                    }
                }
            }
        }
        if (strict) enforce_consumed(edge, consumed, path, pointer, diagnostics);
        if (valid_type && valid_from && valid_condition && valid_routes) {
            topology.conditional_edges.push_back(std::move(parsed));
        }
    };

    if (edges_ok) {
        std::size_t index = 0;
        for (const auto& edge : definition["edges"]) {
            const std::string path    = "edges[" + std::to_string(index) + "]";
            const std::string pointer = "/edges/" + std::to_string(index++);
            if (!edge.is_object()) {
                diagnostics.push_back(
                    {"GC_EDGE", pointer, path + ": edge must be an object", json{{"edge", edge}}});
                continue;
            }
            bool conditional = edge.contains("condition");
            if (edge.contains("type")) {
                if (edge["type"].is_string())
                    conditional = conditional || edge["type"].get<std::string>() == "conditional";
                else
                    field_type(pointer + "/type", path + ".type", "string", edge["type"]);
            }
            if (conditional) {
                parse_conditional(edge, path, pointer, true);
            } else {
                Edge       parsed;
                const bool valid_from =
                    read_string(edge, "from", pointer + "/from", path, parsed.from);
                const bool valid_to = read_string(edge, "to", pointer + "/to", path, parsed.to);
                if (strict) {
                    enforce_consumed(edge, {"from", "to"}, path, pointer, diagnostics);
                }
                if (valid_from && valid_to) topology.edges.push_back(std::move(parsed));
            }
        }
    }
    if (conditional_edges_ok) {
        std::size_t index = 0;
        for (const auto& edge : definition["conditional_edges"]) {
            const std::string path    = "conditional_edges[" + std::to_string(index) + "]";
            const std::string pointer = "/conditional_edges/" + std::to_string(index++);
            parse_conditional(edge, path, pointer, false);
        }
    }

    auto parse_interrupts = [&](const char* field, std::set<std::string>& output) {
        if (!definition.contains(field)) return;
        top_consumed.insert(field);
        const auto&       value   = definition[field];
        const std::string pointer = "/" + std::string(field);
        if (!value.is_array()) {
            field_type(pointer, "topology '$." + std::string(field) + "'", "array", value);
            return;
        }
        std::size_t index = 0;
        for (const auto& node : value) {
            if (!node.is_string()) {
                field_type(pointer + "/" + std::to_string(index), field, "string", node);
            } else if (strict && node.get<std::string>().empty()) {
                diagnostics.push_back({"GC_FIELD_VALUE", pointer + "/" + std::to_string(index),
                                       std::string(field) + " entries must not be empty",
                                       json{{"expected", "non-empty string"}, {"actual", ""}}});
            } else {
                output.insert(node.get<std::string>());
            }
            ++index;
        }
    };
    parse_interrupts("interrupt_before", topology.interrupt_before);
    parse_interrupts("interrupt_after", topology.interrupt_after);

    if (definition.contains("retry_policy")) {
        top_consumed.insert("retry_policy");
        const auto& retry = definition["retry_policy"];
        if (!retry.is_object()) {
            field_type("/retry_policy", "topology '$.retry_policy'", "object", retry);
        } else {
            RetryPolicy policy;
            auto        read_integer = [&](const char* field, int fallback) {
                if (!retry.contains(field)) return fallback;
                const auto& value = retry[field];
                if (!value.is_number_integer()) {
                    field_type("/retry_policy/" + std::string(field),
                                      "retry_policy." + std::string(field), "integer", value);
                    return fallback;
                }
                if (!strict) {
                    const auto parsed = value.get<std::int64_t>();
                    if (parsed < std::numeric_limits<int>::min() ||
                        parsed > std::numeric_limits<int>::max()) {
                        field_type("/retry_policy/" + std::string(field),
                                          "retry_policy." + std::string(field), "integer in range", value);
                        return fallback;
                    }
                    return static_cast<int>(parsed);
                }
                if (value.is_number_unsigned()) {
                    const auto parsed = value.get<std::uint64_t>();
                    if (parsed <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                        return static_cast<int>(parsed);
                } else {
                    const auto parsed = value.get<std::int64_t>();
                    if (parsed >= 0 && parsed <= std::numeric_limits<int>::max())
                        return static_cast<int>(parsed);
                }
                diagnostics.push_back(
                    {"GC_FIELD_VALUE", "/retry_policy/" + std::string(field),
                     "retry_policy." + std::string(field) + " must be a non-negative int32",
                     json{{"minimum", 0},
                                 {"maximum", std::numeric_limits<int>::max()},
                                 {"actual", value}}});
                return fallback;
            };
            policy.max_retries      = read_integer("max_retries", 0);
            policy.initial_delay_ms = read_integer("initial_delay_ms", 100);
            policy.max_delay_ms     = read_integer("max_delay_ms", 5000);
            if (retry.contains("backoff_multiplier")) {
                if (!retry["backoff_multiplier"].is_number()) {
                    field_type("/retry_policy/backoff_multiplier",
                               "retry_policy.backoff_multiplier", "number",
                               retry["backoff_multiplier"]);
                } else {
                    const auto multiplier = retry["backoff_multiplier"].get<double>();
                    if (!strict) {
                        policy.backoff_multiplier = static_cast<float>(multiplier);
                    } else if (!std::isfinite(multiplier) ||
                               multiplier <
                                   static_cast<double>(std::numeric_limits<float>::lowest()) ||
                               multiplier >
                                   static_cast<double>(std::numeric_limits<float>::max())) {
                        diagnostics.push_back(
                            {"GC_FIELD_VALUE", "/retry_policy/backoff_multiplier",
                             "retry_policy.backoff_multiplier exceeds finite float range",
                             json{{"minimum",
                                   static_cast<double>(std::numeric_limits<float>::lowest())},
                                  {"maximum",
                                   static_cast<double>(std::numeric_limits<float>::max())},
                                  {"actual", retry["backoff_multiplier"]}}});
                    } else {
                        policy.backoff_multiplier = static_cast<float>(multiplier);
                    }
                }
            }
            if (strict) {
                enforce_consumed(
                    retry,
                    {"max_retries", "initial_delay_ms", "backoff_multiplier", "max_delay_ms"},
                    "retry_policy", "/retry_policy", diagnostics);
            }
            topology.retry_policy = policy;
        }
    }

    if (strict) enforce_consumed(definition, top_consumed, "$", "", diagnostics);
    if (diagnostics.empty()) report.topology = std::move(topology);
    return report;
}

ParseReport GraphCompiler::parse_report(const json& definition, const GraphRegistry& registry) {
    return parse_report_impl(definition, registry, false);
}

TopologySpec GraphCompiler::parse_local(const json& definition, const GraphRegistry& registry) {
    auto report = parse_local_report(definition, registry);
    if (report.has_errors()) throw std::runtime_error(report.summary());
    return std::move(*report.topology);
}

ParseReport GraphCompiler::parse_local_report(const json&          definition,
                                              const GraphRegistry& registry) {
    auto report = parse_report_impl(definition, registry, true);
    if (!definition.is_object()) return report;
    if (definition.contains("channels") && definition["channels"].is_object()) {
        for (const auto& [name, channel] : definition["channels"].items()) {
            if (!channel.is_object()) continue;
            std::string reducer = "overwrite";
            if (channel.contains("reducer")) {
                if (!channel["reducer"].is_string()) continue;
                reducer = channel["reducer"].get<std::string>();
            }
            if (!registry.contains_reducer(reducer)) {
                report.diagnostics.push_back(
                    {"GC_REGISTRY_MISS", "/channels/" + pointer_token(name) + "/reducer",
                     "Local graph registry has no reducer '" + reducer + "'",
                     json{{"kind", "reducer"}, {"name", reducer}}});
            }
        }
    }
    if (definition.contains("nodes") && definition["nodes"].is_object()) {
        for (const auto& [name, node] : definition["nodes"].items()) {
            if (!node.is_object() || !node.contains("type") || !node["type"].is_string()) {
                continue;
            }
            const auto type = node["type"].get<std::string>();
            if (!registry.contains_type(type)) {
                report.diagnostics.push_back(
                    {"GC_REGISTRY_MISS", "/nodes/" + pointer_token(name) + "/type",
                     "Local graph registry has no node type '" + type + "'",
                     json{{"kind", "node"}, {"name", type}}});
            }
        }
    }
    auto find_conditions = [&](const char* field) {
        if (!definition.contains(field)) return;
        const auto emit_missing = [&](const json& edge, const std::string& pointer) {
            if (!edge.is_object() || !edge.contains("condition") || !edge["condition"].is_string())
                return;
            const auto condition = edge["condition"].get<std::string>();
            if (!registry.contains_condition(condition)) {
                report.diagnostics.push_back(
                    {"GC_REGISTRY_MISS", pointer + "/condition",
                     "Local graph registry has no condition '" + condition + "'",
                     json{{"kind", "condition"}, {"name", condition}}});
            }
        };
        if (definition[field].is_array()) {
            std::size_t index = 0;
            for (const auto& edge : definition[field])
                emit_missing(edge, "/" + std::string(field) + "/" + std::to_string(index++));
        } else if (definition[field].is_object()) {
            for (const auto& [name, edge] : definition[field].items())
                emit_missing(edge, "/" + std::string(field) + "/" + pointer_token(name));
        }
    };
    find_conditions("edges");
    find_conditions("conditional_edges");
    if (!report.diagnostics.empty()) report.topology.reset();
    return report;
}

CompiledGraph GraphCompiler::link(TopologySpec topology, const NodeContext& default_context) {
    return link(std::move(topology), default_context, GraphRegistry::global());
}

CompiledGraph GraphCompiler::link(TopologySpec         topology,
                                  const NodeContext&   default_context,
                                  const GraphRegistry& registry) {
    CompiledGraph cg;
    cg.name              = std::move(topology.name);
    cg.channel_defs      = std::move(topology.channel_defs);
    cg.edges             = std::move(topology.edges);
    cg.conditional_edges = std::move(topology.conditional_edges);
    cg.barrier_specs     = std::move(topology.barrier_specs);
    cg.interrupt_before  = std::move(topology.interrupt_before);
    cg.interrupt_after   = std::move(topology.interrupt_after);
    cg.retry_policy      = std::move(topology.retry_policy);
    cg.schema_version    = topology.schema_version;
    if (cg.schema_version == 0) {
        for (auto& edge : cg.conditional_edges) {
            if (edge.routes.empty()) continue;
            const std::string historical_target      = edge.routes.rbegin()->second;
            edge.routes[detail::kLegacyDefaultRoute] = historical_target;
        }
    }
    for (const auto& [name, node_def] : topology.node_defs) {
        const auto type = node_def.value("type", "");
        cg.nodes[name]  = registry.create(type, name, node_def, default_context);

        json stored = json::object();
        for (const auto& [key, value] : node_def.items()) {
            if (key != "barrier") stored[key] = value;
        }
        cg.node_defs[name] = std::move(stored);
    }
    return cg;
}

CompiledGraph GraphCompiler::link_local(TopologySpec         topology,
                                        const NodeContext&   default_context,
                                        const GraphRegistry& registry) {
    CompiledGraph cg;
    cg.name              = std::move(topology.name);
    cg.channel_defs      = std::move(topology.channel_defs);
    cg.edges             = std::move(topology.edges);
    cg.conditional_edges = std::move(topology.conditional_edges);
    cg.barrier_specs     = std::move(topology.barrier_specs);
    cg.interrupt_before  = std::move(topology.interrupt_before);
    cg.interrupt_after   = std::move(topology.interrupt_after);
    cg.retry_policy      = std::move(topology.retry_policy);
    cg.schema_version    = topology.schema_version;
    if (cg.schema_version == 0) {
        for (auto& edge : cg.conditional_edges) {
            if (edge.routes.empty()) continue;
            const std::string historical_target      = edge.routes.rbegin()->second;
            edge.routes[detail::kLegacyDefaultRoute] = historical_target;
        }
    }
    for (const auto& [name, node_def] : topology.node_defs) {
        const auto type = node_def.value("type", "");
        cg.nodes[name]  = registry.create_local(type, name, node_def, default_context);

        json stored = json::object();
        for (const auto& [key, value] : node_def.items()) {
            if (key != "barrier") stored[key] = value;
        }
        cg.node_defs[name] = std::move(stored);
    }
    return cg;
}

// =========================================================================
// Re-emission + canonicalization + translation validation
// =========================================================================

json TopologySpec::to_json() const {
    json j = json::object();
    if (schema_version > 0) j["schema_version"] = schema_version;
    j["name"] = name;

    if (!channel_defs.empty()) {
        json channels = json::object();
        for (const auto& cd : channel_defs) {
            json c       = json::object();
            c["reducer"] = cd.reducer_name;
            if (cd.has_initial) c["initial"] = cd.initial_value;
            channels[cd.name] = std::move(c);
        }
        j["channels"] = std::move(channels);
    }

    if (!node_defs.empty()) {
        json nodes = json::object();
        for (const auto& [nname, ndef] : node_defs) {
            json n = json::object();
            for (const auto& [key, value] : ndef.items()) {
                if (key != "barrier") n[key] = value;
            }
            auto bit = barrier_specs.find(nname);
            if (bit != barrier_specs.end()) {
                json wait_for = json::array();
                for (const auto& up : bit->second)
                    wait_for.push_back(up);
                n["barrier"] = json{{"wait_for", std::move(wait_for)}};
            }
            nodes[nname] = std::move(n);
        }
        j["nodes"] = std::move(nodes);
    }

    if (!edges.empty()) {
        json arr = json::array();
        for (const auto& e : edges) {
            arr.push_back(json{{"from", e.from}, {"to", e.to}});
        }
        j["edges"] = std::move(arr);
    }

    if (!conditional_edges.empty()) {
        json arr = json::array();
        for (const auto& ce : conditional_edges) {
            json e         = json::object();
            e["from"]      = ce.from;
            e["condition"] = ce.condition;
            if (!ce.routes.empty()) {
                json routes = json::object();
                for (const auto& [k, v] : ce.routes) {
                    if (k != detail::kLegacyDefaultRoute) routes[k] = v;
                }
                e["routes"] = std::move(routes);
            }
            arr.push_back(std::move(e));
        }
        j["conditional_edges"] = std::move(arr);
    }

    auto emit_set = [&](const char* key, const std::set<std::string>& s) {
        if (s.empty()) return;
        json arr = json::array();
        for (const auto& n : s)
            arr.push_back(n);
        j[key] = std::move(arr);
    };
    emit_set("interrupt_before", interrupt_before);
    emit_set("interrupt_after", interrupt_after);

    if (retry_policy) {
        j["retry_policy"] = json{
            {"max_retries", retry_policy->max_retries},
            {"initial_delay_ms", retry_policy->initial_delay_ms},
            {"backoff_multiplier", retry_policy->backoff_multiplier},
            {"max_delay_ms", retry_policy->max_delay_ms},
        };
    }

    return j;
}

TopologySpec CompiledGraph::topology() const {
    TopologySpec result;
    result.name              = name;
    result.channel_defs      = channel_defs;
    result.edges             = edges;
    result.conditional_edges = conditional_edges;
    result.barrier_specs     = barrier_specs;
    result.interrupt_before  = interrupt_before;
    result.interrupt_after   = interrupt_after;
    result.retry_policy      = retry_policy;
    result.schema_version    = schema_version;
    result.node_defs         = node_defs;
    for (const auto& [name, wait_for_set] : barrier_specs) {
        auto node = result.node_defs.find(name);
        if (node == result.node_defs.end()) continue;
        json wait_for = json::array();
        for (const auto& upstream : wait_for_set)
            wait_for.push_back(upstream);
        node->second["barrier"] = json{{"wait_for", std::move(wait_for)}};
    }
    return result;
}

json CompiledGraph::to_json() const {
    return topology().to_json();
}

json GraphCompiler::canon(const json& definition) {
    json out = json::object();

    // Unknown top-level keys are preserved (minus annotations) — the
    // TV compare is precisely how a key the compiler ignored becomes
    // visible. Owned keys are rebuilt in normalized form below.
    static const std::set<std::string> owned = {
        "schema_version",
        "name",
        "channels",
        "nodes",
        "edges",
        "conditional_edges",
        "interrupt_before",
        "interrupt_after",
        "retry_policy",
    };
    for (const auto& [k, v] : definition.items()) {
        if (is_annotation_key(k) || owned.count(k)) continue;
        out[k] = v;
    }

    if (definition.contains("schema_version") && definition["schema_version"].is_number_integer() &&
        definition["schema_version"].get<int>() > 0) {
        out["schema_version"] = definition["schema_version"];
    }
    out["name"] = definition.value("name", "unnamed_graph");

    if (definition.contains("channels") && definition["channels"].is_object() &&
        !definition["channels"].empty()) {
        json channels = json::object();
        for (const auto& [name, ch] : definition["channels"].items()) {
            json c = json::object();
            if (ch.is_object()) {
                for (const auto& [k, v] : ch.items()) {
                    if (is_annotation_key(k)) continue;
                    c[k] = v;
                }
            }
            if (!c.contains("reducer")) c["reducer"] = "overwrite";
            channels[name] = std::move(c);
        }
        out["channels"] = std::move(channels);
    }

    if (definition.contains("nodes") && definition["nodes"].is_object() &&
        !definition["nodes"].empty()) {
        json nodes = json::object();
        for (const auto& [name, nd] : definition["nodes"].items()) {
            json n = json::object();
            for (const auto& [k, v] : nd.items()) {
                if (is_annotation_key(k)) continue;
                if (k == "barrier") {
                    // Normalize to {"wait_for": sorted-deduped array}.
                    // An empty/missing wait_for is deliberately KEPT
                    // (as an empty array): the parser drops such a
                    // barrier, so lenient TV flags the drop and strict
                    // mode has already refused it.
                    std::set<std::string> wait_for;
                    if (v.is_object() && v.contains("wait_for")) {
                        for (const auto& up : v["wait_for"]) {
                            wait_for.insert(up.get<std::string>());
                        }
                    }
                    json arr = json::array();
                    for (const auto& up : wait_for)
                        arr.push_back(up);
                    json b = json::object();
                    if (v.is_object()) {
                        for (const auto& [bk, bv] : v.items()) {
                            if (is_annotation_key(bk) || bk == "wait_for") continue;
                            b[bk] = bv;  // unknown barrier keys preserved
                        }
                    }
                    b["wait_for"] = std::move(arr);
                    n["barrier"]  = std::move(b);
                    continue;
                }
                n[k] = v;
            }
            nodes[name] = std::move(n);
        }
        out["nodes"] = std::move(nodes);
    }

    // Edges: split legacy inline conditionals out of `edges` into
    // `conditional_edges` (the one intentional rewrite the compiler
    // performs), then sort both arrays for order-insensitive compare.
    std::vector<json> plain, conditional;
    if (definition.contains("edges") && definition["edges"].is_array()) {
        for (const auto& e : definition["edges"]) {
            bool is_conditional = e.contains("condition") || e.value("type", "") == "conditional";
            if (is_conditional) {
                // sort_keys BEFORE the dump-based array sort: the
                // comparator must see identical serializations for
                // identical meaning regardless of input key order.
                conditional.push_back(sort_keys(canon_conditional(e)));
            } else {
                json p = json::object();
                for (const auto& [k, v] : e.items()) {
                    if (is_annotation_key(k)) continue;
                    p[k] = v;
                }
                plain.push_back(sort_keys(p));
            }
        }
    }
    if (definition.contains("conditional_edges") && definition["conditional_edges"].is_array()) {
        for (const auto& e : definition["conditional_edges"]) {
            conditional.push_back(sort_keys(canon_conditional(e)));
        }
    }
    auto by_dump = [](const json& a, const json& b) { return a.dump() < b.dump(); };
    std::sort(plain.begin(), plain.end(), by_dump);
    std::sort(conditional.begin(), conditional.end(), by_dump);
    auto to_array = [](const std::vector<json>& v) {
        json arr = json::array();
        for (const auto& e : v)
            arr.push_back(e);
        return arr;
    };
    if (!plain.empty()) out["edges"] = to_array(plain);
    if (!conditional.empty()) out["conditional_edges"] = to_array(conditional);

    auto canon_set = [&](const char* key) {
        if (!definition.contains(key) || !definition[key].is_array()) return;
        std::set<std::string> s;
        for (const auto& n : definition[key])
            s.insert(n.get<std::string>());
        if (s.empty()) return;
        json arr = json::array();
        for (const auto& n : s)
            arr.push_back(n);
        out[key] = std::move(arr);
    };
    canon_set("interrupt_before");
    canon_set("interrupt_after");

    if (definition.contains("retry_policy") && definition["retry_policy"].is_object()) {
        const auto& rp = definition["retry_policy"];
        json        p  = json::object();
        for (const auto& [k, v] : rp.items()) {
            if (is_annotation_key(k)) continue;
            p[k] = v;  // unknown keys preserved
        }
        if (!p.contains("max_retries")) p["max_retries"] = 0;
        if (!p.contains("initial_delay_ms")) p["initial_delay_ms"] = 100;
        if (!p.contains("max_delay_ms")) p["max_delay_ms"] = 5000;
        p["backoff_multiplier"] =
            p.contains("backoff_multiplier") ? canon_backoff(p["backoff_multiplier"]) : json(2.0f);
        out["retry_policy"] = std::move(p);
    }

    // Recursive key sort — see sort_keys(): equality is serialization-
    // based, so canonical form must fix object key order everywhere
    // (including inside pass-through node configs).
    return sort_keys(out);
}

json GraphCompiler::upgrade_to_latest(const json& definition) {
    if (definition.contains("schema_version") && definition["schema_version"].is_number_integer() &&
        definition["schema_version"].get<int>() >= TOPOLOGY_SCHEMA_VERSION) {
        return definition;  // already current
    }

    auto quarantine_name = [](const json& source, const json& output, const std::string& key) {
        const std::string base      = "x-upgraded-" + key;
        std::string       candidate = base;
        for (int suffix = 2; source.contains(candidate) || output.contains(candidate); ++suffix) {
            candidate = base + "-" + std::to_string(suffix);
        }
        return candidate;
    };

    // Rebuild an object keeping `consumed` keys, renaming everything
    // else (except annotations) into the x- namespace — data preserved,
    // strict mode satisfied, semantics identical to the lenient parser
    // that ignored those keys.
    auto quarantine = [&](const json& obj, const std::set<std::string>& consumed) {
        json out = json::object();
        for (const auto& [k, v] : obj.items()) {
            if (consumed.count(k) || is_annotation_key(k))
                out[k] = v;
            else
                out[quarantine_name(obj, out, k)] = v;
        }
        return out;
    };

    json up              = json::object();
    up["schema_version"] = TOPOLOGY_SCHEMA_VERSION;

    static const std::set<std::string> top_keys = {
        "name",
        "channels",
        "nodes",
        "edges",
        "conditional_edges",
        "interrupt_before",
        "interrupt_after",
        "retry_policy",
    };
    for (const auto& [k, v] : definition.items()) {
        if (k == "schema_version") continue;  // re-stamped above
        if (!top_keys.count(k) && !is_annotation_key(k)) {
            up[quarantine_name(definition, up, k)] = v;
            continue;
        }
        if (k == "channels" && v.is_object()) {
            json channels = json::object();
            for (const auto& [cn, cd] : v.items()) {
                channels[cn] = cd.is_object() ? quarantine(cd, {"reducer", "initial"}) : cd;
            }
            up["channels"] = std::move(channels);
        } else if (k == "nodes" && v.is_object()) {
            json nodes = json::object();
            for (const auto& [nn, nd] : v.items()) {
                if (!nd.is_object()) {
                    nodes[nn] = nd;
                    continue;
                }
                // Node config keys are checked against the declared
                // schema only when it is closed-world (mirrors strict
                // compile) — permissive types keep their config as-is.
                const std::string type   = nd.value("type", "");
                const json        schema = NodeFactory::instance().config_schema(type);
                bool              open   = !schema.contains("properties");
                if (schema.contains("additionalProperties")) {
                    const auto& ap = schema["additionalProperties"];
                    if (!ap.is_boolean() || ap.get<bool>()) open = true;
                }
                std::set<std::string> consumed = {"type", "barrier"};
                if (!open) {
                    for (const auto& [pk, pv] : schema["properties"].items()) {
                        (void)pv;
                        consumed.insert(pk);
                    }
                }
                json node = open ? nd : quarantine(nd, consumed);
                // Legacy: an empty/missing wait_for silently dropped
                // the barrier — make that explicit.
                if (node.contains("barrier")) {
                    const auto& b     = node["barrier"];
                    const bool  empty = !b.is_object() || !b.contains("wait_for") ||
                                       !b["wait_for"].is_array() || b["wait_for"].empty();
                    if (empty) {
                        json cleaned = json::object();
                        for (const auto& [nk, nv] : node.items()) {
                            if (nk != "barrier") cleaned[nk] = nv;
                        }
                        cleaned[quarantine_name(node, cleaned, "barrier")] = b;
                        node                                               = std::move(cleaned);
                    }
                }
                nodes[nn] = std::move(node);
            }
            up["nodes"] = std::move(nodes);
        } else if (k == "edges" && v.is_array()) {
            json edges = json::array();
            for (const auto& e : v) {
                if (!e.is_object()) {
                    edges.push_back(e);
                    continue;
                }
                const bool cond = e.contains("condition") || e.value("type", "") == "conditional";
                edges.push_back(cond ? quarantine(e, {"from", "condition", "routes", "type"})
                                     : quarantine(e, {"from", "to"}));
            }
            up["edges"] = std::move(edges);
        } else if (k == "conditional_edges" && v.is_array()) {
            json ces = json::array();
            for (const auto& e : v) {
                ces.push_back(e.is_object() ? quarantine(e, {"from", "condition", "routes"}) : e);
            }
            up["conditional_edges"] = std::move(ces);
        } else if (k == "retry_policy" && v.is_object()) {
            up["retry_policy"] = quarantine(
                v, {"max_retries", "initial_delay_ms", "backoff_multiplier", "max_delay_ms"});
        } else {
            up[k] = v;
        }
    }
    return up;
}

void GraphCompiler::verify_roundtrip(const json& definition, const CompiledGraph& cg) {
    verify_roundtrip(definition, cg.topology());
}

void GraphCompiler::verify_roundtrip(const json& definition, const TopologySpec& topology) {
    auto report = verify_roundtrip_report(definition, topology);
    if (!report.has_errors()) return;
    if (topology.schema_version >= 1) {
        throw std::runtime_error(report.summary());
    }
    // Lenient documents keep compiling (historical behavior), but the
    // silent drop is no longer silent. FILE* stderr, not std::cerr —
    // see graph_executor.cpp for the Windows capfd rationale.
    const auto message = report.summary();
    std::fprintf(stderr, "[neograph] warning: %s\n", message.c_str());
}

RoundTripReport GraphCompiler::verify_roundtrip_report(const json&         definition,
                                                       const TopologySpec& topology) {
    RoundTripReport report;
    try {
        GraphRegistry structural_registry;
        auto          structural = parse_report_impl(definition, structural_registry, true);
        if (structural.has_errors()) {
            report.diagnostics = std::move(structural.diagnostics);
            return report;
        }
        const json declared = canon(definition);
        const json compiled = canon(topology.to_json());
        collect_mismatches(declared, compiled, "", report.diagnostics);
    } catch (...) {
        report.diagnostics.push_back(
            {"GC_ROUNDTRIP_INPUT", "",
             "Translation validation could not canonicalize the authored or compiled topology",
             json{{"stage", "canonicalization"}}});
    }
    return report;
}

}  // namespace neograph::graph
