#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <neograph/graph/registry.h>

#include "canonical_json.h"
#include "graph_v2_keys.h"
#include "json_walk.h"
#include "routing_policy.h"
#include "text_safety.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using neograph::detail::check_walk_depth;

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
json sort_keys(const json& v, std::size_t depth = 0) {
    if (v.is_object()) {
        check_walk_depth(depth, "graph canonicalization");
        std::vector<std::string> keys;
        for (const auto& [k, val] : v.items()) {
            (void)val;
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        json out = json::object();
        for (const auto& k : keys)
            out[k] = sort_keys(v[k], depth + 1);
        return out;
    }
    if (v.is_array()) {
        check_walk_depth(depth, "graph canonicalization");
        json out = json::array();
        for (const auto& e : v)
            out.push_back(sort_keys(e, depth + 1));
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
                        std::vector<CompilerDiagnostic>& diagnostics,
                        std::size_t                      depth = 0) {
    constexpr size_t kMax = 8;
    if (diagnostics.size() >= kMax || declared == compiled) return;
    if (declared.is_object() && compiled.is_object()) {
        check_walk_depth(depth, "graph round-trip check");
        for (const auto& [k, v] : declared.items()) {
            if (diagnostics.size() >= kMax) return;
            const std::string child = pointer + "/" + pointer_token(k);
            if (!compiled.contains(k)) {
                diagnostics.push_back({"GC_ROUNDTRIP", child, child + ": lost in compilation",
                                       json{{"kind", "lost"}, {"declared", v}}});
            } else {
                collect_mismatches(v, compiled[k], child, diagnostics, depth + 1);
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
        check_walk_depth(depth, "graph round-trip check");
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
                               diagnostics, depth + 1);
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

// ---- #362 metadata rules ---------------------------------------------------

json unsafe_text_witness(std::string_view text) {
    std::vector<neograph::detail::UnsafeCodePoint> found;
    try {
        found = neograph::detail::find_unsafe_code_points(text);
    } catch (const neograph::detail::InvalidUtf8Error& error) {
        return json{{"invalid_utf8_byte_offset", error.byte_offset()}};
    }
    if (found.empty()) return nullptr;
    json        code_points = json::array();
    json        classes     = json::array();
    std::set<std::string> seen_points, seen_classes;
    for (const auto& item : found) {
        auto point = neograph::detail::format_code_point(item.code_point);
        if (seen_points.insert(point).second) code_points.push_back(point);
        const std::string text_class(neograph::detail::to_string(item.text_class));
        if (seen_classes.insert(text_class).second) classes.push_back(text_class);
    }
    return json{{"code_points", std::move(code_points)}, {"classes", std::move(classes)}};
}

std::size_t metadata_members(const json& value) {
    std::size_t count = 0;
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            (void)key;
            count += 1 + metadata_members(child);
        }
    } else if (value.is_array()) {
        for (const auto& child : value) count += metadata_members(child);
    }
    return count;
}

std::size_t metadata_depth(const json& value) {
    std::size_t deepest = 0;
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            (void)key;
            deepest = std::max(deepest, 1 + metadata_depth(child));
        }
    } else if (value.is_array()) {
        for (const auto& child : value) deepest = std::max(deepest, 1 + metadata_depth(child));
    }
    return deepest;
}

void walk_metadata(const json&                      value,
                   const std::string&               pointer,
                   std::size_t                      depth,
                   std::size_t                      total_depth,
                   bool&                            depth_reported,
                   std::vector<CompilerDiagnostic>& diagnostics) {
    if (depth > detail::kMetadataMaxDepth) {
        if (!depth_reported) {
            depth_reported = true;
            diagnostics.push_back(
                {"GC_METADATA_LIMIT", pointer,
                 "metadata nests " + std::to_string(total_depth) + " levels deep; at most " +
                     std::to_string(detail::kMetadataMaxDepth) + " are allowed",
                 json{{"kind", "depth"}, {"limit", detail::kMetadataMaxDepth}, {"actual", total_depth}}});
        }
        return;
    }
    if (value.is_string()) {
        auto witness = unsafe_text_witness(value.get<std::string>());
        if (!witness.is_null())
            diagnostics.push_back({"GC_METADATA_TEXT", pointer,
                                   "metadata text at " + pointer +
                                       " contains invisible or ill-formed characters",
                                   std::move(witness)});
        return;
    }
    if (value.is_array()) {
        std::size_t index = 0;
        for (const auto& child : value)
            walk_metadata(child, pointer + "/" + std::to_string(index++), depth + 1, total_depth,
                          depth_reported, diagnostics);
        return;
    }
    if (!value.is_object()) return;
    for (const auto& [key, child] : value.items()) {
        const auto child_pointer = pointer + "/" + pointer_token(key);
        if (key.empty()) {
            diagnostics.push_back({"GC_FIELD_VALUE", child_pointer, "metadata keys must not be empty",
                                   json{{"expected", "key of 1 to 128 bytes"}, {"actual", key}}});
            continue;
        }
        if (key.size() > detail::kMetadataMaxKeyBytes) {
            diagnostics.push_back({"GC_METADATA_LIMIT", child_pointer,
                                   "metadata key is " + std::to_string(key.size()) +
                                       " bytes long; at most 128 are allowed",
                                   json{{"kind", "key_bytes"},
                                        {"limit", detail::kMetadataMaxKeyBytes},
                                        {"actual", key.size()}}});
            continue;
        }
        if (depth == 0 && key.rfind("neograph.", 0) == 0) {
            diagnostics.push_back({"GC_FIELD_VALUE", child_pointer,
                                   "metadata key '" + key + "' uses the reserved prefix 'neograph.'",
                                   json{{"expected", "a key without the reserved prefix 'neograph.'"},
                                        {"actual", key}}});
            continue;
        }
        if (auto witness = unsafe_text_witness(key); !witness.is_null()) {
            diagnostics.push_back({"GC_METADATA_TEXT", child_pointer,
                                   "metadata key at " + child_pointer +
                                       " contains invisible or ill-formed characters",
                                   std::move(witness)});
            continue;
        }
        walk_metadata(child, child_pointer, depth + 1, total_depth, depth_reported, diagnostics);
    }
}

}  // namespace

void detail::append_metadata_diagnostics(const json&                      metadata,
                                         std::vector<CompilerDiagnostic>& diagnostics) {
    if (!metadata.is_object()) {
        diagnostics.push_back({"GC_FIELD_TYPE", "/metadata",
                               "topology '$.metadata' must be an object, got " +
                                   json_type_name(metadata),
                               json{{"expected", "object"}, {"actual", json_type_name(metadata)}}});
        return;
    }
    const auto bytes = neograph::detail::canonical_json_bytes(metadata).size();
    if (bytes > kMetadataMaxBytes) {
        diagnostics.push_back({"GC_METADATA_LIMIT", "/metadata",
                               "metadata is " + std::to_string(bytes) +
                                   " canonical bytes; at most 8192 are allowed",
                               json{{"kind", "bytes"}, {"limit", kMetadataMaxBytes}, {"actual", bytes}}});
    }
    const auto members = metadata_members(metadata);
    if (members > kMetadataMaxMembers) {
        diagnostics.push_back({"GC_METADATA_LIMIT", "/metadata",
                               "metadata has " + std::to_string(members) +
                                   " object members; at most 64 are allowed",
                               json{{"kind", "members"}, {"limit", kMetadataMaxMembers}, {"actual", members}}});
    }
    bool depth_reported = false;
    walk_metadata(metadata, "/metadata", 0, metadata_depth(metadata), depth_reported, diagnostics);
}

std::vector<std::set<std::string>> detail::canonical_slots(std::vector<std::set<std::string>> slots) {
    // std::string compares bytes as unsigned char, which is UTF-8 order.
    std::sort(slots.begin(), slots.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.empty() || rhs.empty()) return lhs.size() < rhs.size();
        return *lhs.begin() < *rhs.begin();
    });
    return slots;
}

bool detail::legacy_slots(const std::vector<std::set<std::string>>& slots) {
    return std::all_of(slots.begin(), slots.end(),
                       [](const std::set<std::string>& slot) { return slot.size() == 1; });
}

json detail::print_wait_for(const std::vector<std::set<std::string>>& slots) {
    json wait_for = json::array();
    for (const auto& slot : canonical_slots(slots)) {
        if (slot.size() == 1) {
            wait_for.push_back(*slot.begin());
            continue;
        }
        json alternatives = json::array();
        for (const auto& source : slot) alternatives.push_back(source);
        wait_for.push_back(json{{"any_of", std::move(alternatives)}});
    }
    return wait_for;
}

bool detail::uses_slot_syntax(const json& wait_for) {
    if (!wait_for.is_array()) return false;
    for (const auto& entry : wait_for)
        if (entry.is_object()) return true;
    return false;
}

std::optional<std::vector<std::set<std::string>>> detail::parse_wait_for_slots(
    const json&                      wait_for,
    const std::string&               display,
    const std::string&               pointer,
    std::vector<CompilerDiagnostic>& diagnostics) {
    const auto before = diagnostics.size();
    const auto fail   = [&](const std::string& at, const std::string& message, const json& entry) {
        diagnostics.push_back({"GC_BARRIER", at, display + ": " + message,
                               json{{"entry", entry}, {"wait_for", wait_for}}});
    };
    std::vector<std::set<std::string>> slots;
    std::set<std::string>              seen;
    std::size_t                        index = 0;
    for (const auto& entry : wait_for) {
        const auto at = pointer + "/wait_for/" + std::to_string(index++);
        std::set<std::string> slot;
        if (entry.is_string()) {
            if (entry.get<std::string>().empty()) {
                fail(at, "wait_for entries must not be empty", entry);
                continue;
            }
            slot.insert(entry.get<std::string>());
        } else if (entry.is_object()) {
            bool shaped = true;
            for (const auto& [key, unused] : entry.items()) {
                (void)unused;
                if (key == "any_of") continue;
                fail(at, "a wait_for slot holds only 'any_of', not '" + key + "'", entry);
                shaped = false;
            }
            if (!entry.contains("any_of") || !entry["any_of"].is_array() || entry["any_of"].empty()) {
                fail(at, "'any_of' must be a nonempty array of node names", entry);
                continue;
            }
            for (const auto& source : entry["any_of"]) {
                if (source.is_object() && source.contains("any_of")) {
                    fail(at, "'any_of' slots cannot be nested", entry);
                    shaped = false;
                } else if (!source.is_string() || source.get<std::string>().empty()) {
                    fail(at, "'any_of' alternatives must be nonempty node names", entry);
                    shaped = false;
                } else if (!slot.insert(source.get<std::string>()).second) {
                    fail(at, "'" + source.get<std::string>() + "' is repeated in one slot", entry);
                    shaped = false;
                }
            }
            if (!shaped) continue;
        } else {
            fail(at, "wait_for entries must be node names or {\"any_of\": [node names]}", entry);
            continue;
        }
        for (const auto& source : slot) {
            if (!seen.insert(source).second)
                fail(at, "'" + source + "' appears in more than one slot", entry);
        }
        slots.push_back(std::move(slot));
    }
    if (diagnostics.size() != before) return std::nullopt;
    if (slots.empty()) {
        fail(pointer, "'wait_for' is empty", json::array());
        return std::nullopt;
    }
    return canonical_slots(std::move(slots));
}

// ---- #364 failure routing keys ---------------------------------------------

bool detail::is_known_failure_code(std::string_view code) noexcept {
    static constexpr std::string_view kKnown[] = {
        "NG.BudgetExhausted", "NG.ConfigError",   "NG.EffectCommitted", "NG.EffectUncertain",
        "NG.LogicError",      "NG.ProviderFailed", "NG.SafetyBlocked",   "NG.SubgraphFailed",
        "NG.TaskFailed",      "NG.Timeout",        "NG.UnknownFailure"};
    return std::find(std::begin(kKnown), std::end(kKnown), code) != std::end(kKnown);
}

namespace {

struct FailureKeyContext {
    std::set<std::string>              nodes;
    std::set<std::string>              with_successors;  // static edges or routes
    std::set<std::string>              joins;
    std::map<std::string, std::string> channel_reducers;
};

FailureKeyContext failure_key_context(const json& definition) {
    FailureKeyContext context;
    if (definition.contains("nodes") && definition["nodes"].is_object()) {
        for (const auto& [name, config] : definition["nodes"].items()) {
            context.nodes.insert(name);
            if (config.is_object() && config.contains("barrier")) context.joins.insert(name);
        }
    }
    for (const auto* field : {"edges", "conditional_edges"}) {
        if (!definition.contains(field) || !definition[field].is_array()) continue;
        for (const auto& edge : definition[field])
            if (edge.is_object() && edge.contains("from") && edge["from"].is_string())
                context.with_successors.insert(edge["from"].get<std::string>());
    }
    if (definition.contains("channels") && definition["channels"].is_object()) {
        for (const auto& [name, channel] : definition["channels"].items()) {
            std::string reducer = "overwrite";
            if (channel.is_object() && channel.contains("reducer") && channel["reducer"].is_string())
                reducer = channel["reducer"].get<std::string>();
            context.channel_reducers[name] = reducer;
        }
    }
    return context;
}

std::optional<int> bounded_integer(const json& value, const std::string& pointer, long long minimum,
                                   long long maximum, std::vector<CompilerDiagnostic>& diagnostics) {
    if (!value.is_number_integer()) {
        diagnostics.push_back({"GC_FIELD_TYPE", pointer, pointer + " must be an integer",
                               json{{"expected", "integer"}, {"actual", json_type_name(value)}}});
        return std::nullopt;
    }
    const long long number =
        value.is_number_unsigned()
            ? static_cast<long long>(std::min<std::uint64_t>(value.get<std::uint64_t>(),
                                                             std::numeric_limits<long long>::max()))
            : value.get<std::int64_t>();
    if (number < minimum || number > maximum) {
        diagnostics.push_back({"GC_FIELD_VALUE", pointer,
                               pointer + " must be between " + std::to_string(minimum) + " and " +
                                   std::to_string(maximum),
                               json{{"min", minimum}, {"max", maximum}, {"actual", value}}});
        return std::nullopt;
    }
    return static_cast<int>(number);
}

void unknown_fields(const json& object, const std::set<std::string>& known, const std::string& pointer,
                    std::vector<CompilerDiagnostic>& diagnostics) {
    for (const auto& [key, unused] : object.items()) {
        (void)unused;
        if (known.count(key) || is_annotation_key(key)) continue;
        diagnostics.push_back({"GC_UNKNOWN_FIELD", pointer + "/" + pointer_token(key),
                               pointer + ": unknown or unconsumed key '" + key + "'",
                               json{{"field", key}}});
    }
}

NodeRetryPolicySpec parse_node_retry_policy(const json& policy, const std::string& pointer,
                                            std::vector<CompilerDiagnostic>& diagnostics) {
    NodeRetryPolicySpec spec;
    unknown_fields(policy,
                   {"max_retries", "initial_delay_ms", "backoff_multiplier", "max_delay_ms",
                    "non_retryable", "max_visits", "error_text", "attempt_timeout_ms"},
                   pointer, diagnostics);
    if (policy.contains("max_retries"))
        spec.max_retries = bounded_integer(policy["max_retries"], pointer + "/max_retries", 0, 10, diagnostics);
    if (policy.contains("initial_delay_ms"))
        spec.initial_delay_ms =
            bounded_integer(policy["initial_delay_ms"], pointer + "/initial_delay_ms", 10, 60000, diagnostics);
    if (policy.contains("max_delay_ms"))
        spec.max_delay_ms = bounded_integer(policy["max_delay_ms"], pointer + "/max_delay_ms",
                                            spec.initial_delay_ms.value_or(10), 300000, diagnostics);
    if (policy.contains("max_visits"))
        spec.max_visits = bounded_integer(policy["max_visits"], pointer + "/max_visits", 1, 100, diagnostics);
    if (policy.contains("attempt_timeout_ms"))
        spec.attempt_timeout_ms =
            bounded_integer(policy["attempt_timeout_ms"], pointer + "/attempt_timeout_ms",
                            kMinAttemptTimeoutMs, kMaxAttemptTimeoutMs, diagnostics);
    if (policy.contains("backoff_multiplier")) {
        const auto& value = policy["backoff_multiplier"];
        if (!value.is_number()) {
            diagnostics.push_back({"GC_FIELD_TYPE", pointer + "/backoff_multiplier",
                                   pointer + "/backoff_multiplier must be a number",
                                   json{{"expected", "number"}, {"actual", json_type_name(value)}}});
        } else if (const auto number = value.get<double>();
                   !std::isfinite(number) || number < 1.0 || number > 10.0) {
            diagnostics.push_back({"GC_FIELD_VALUE", pointer + "/backoff_multiplier",
                                   pointer + "/backoff_multiplier must be between 1.0 and 10.0",
                                   json{{"min", 1.0}, {"max", 10.0}, {"actual", value}}});
        } else {
            spec.backoff_multiplier = number;
        }
    }
    if (policy.contains("error_text")) {
        if (!policy["error_text"].is_boolean())
            diagnostics.push_back({"GC_FIELD_TYPE", pointer + "/error_text",
                                   pointer + "/error_text must be a boolean",
                                   json{{"expected", "boolean"}, {"actual", json_type_name(policy["error_text"])}}});
        else
            spec.error_text = policy["error_text"].get<bool>();
    }
    if (policy.contains("non_retryable")) {
        const auto& codes        = policy["non_retryable"];
        const auto  list_pointer = pointer + "/non_retryable";
        if (!codes.is_array()) {
            diagnostics.push_back({"GC_FIELD_TYPE", list_pointer, list_pointer + " must be an array of codes",
                                   json{{"expected", "array"}, {"actual", json_type_name(codes)}}});
        } else if (codes.size() > 16) {
            diagnostics.push_back({"GC_FIELD_VALUE", list_pointer, list_pointer + " lists at most 16 codes",
                                   json{{"min", 0}, {"max", 16}, {"actual", codes.size()}}});
        } else {
            std::set<std::string> seen;
            std::size_t           index = 0;
            for (const auto& code : codes) {
                const auto at = list_pointer + "/" + std::to_string(index++);
                if (!code.is_string()) {
                    diagnostics.push_back({"GC_FIELD_TYPE", at, at + " must be a string",
                                           json{{"expected", "string"}, {"actual", json_type_name(code)}}});
                    continue;
                }
                const auto text = code.get<std::string>();
                if (!is_author_failure_code(text) && !detail::is_known_failure_code(text)) {
                    diagnostics.push_back(
                        {"GC_FIELD_VALUE", at,
                         at + ": '" + text +
                             "' is neither an author code ([a-z][a-z0-9_]*(.[a-z0-9_]+)+) nor a known NG code",
                         json{{"expected", "author code or known NG code"}, {"actual", text}}});
                    continue;
                }
                if (!seen.insert(text).second) {
                    diagnostics.push_back({"GC_FIELD_VALUE", at, at + ": '" + text + "' is listed twice",
                                           json{{"expected", "unique codes"}, {"actual", text}}});
                }
            }
            spec.non_retryable.assign(seen.begin(), seen.end());
        }
    }
    return spec;
}

}  // namespace

detail::FailureRoutingKeys detail::parse_failure_routing_keys(
    const json& definition, std::vector<CompilerDiagnostic>& diagnostics) {
    FailureRoutingKeys keys;
    const auto         context = failure_key_context(definition);
    const auto         type_error = [&](const std::string& pointer, const char* expected, const json& value) {
        diagnostics.push_back({"GC_FIELD_TYPE", pointer,
                               "topology '" + pointer + "' must be an " + expected + ", got " +
                                   json_type_name(value),
                               json{{"expected", expected}, {"actual", json_type_name(value)}}});
    };

    if (definition.contains("node_retry_policies")) {
        const auto& policies = definition["node_retry_policies"];
        if (!policies.is_object()) {
            type_error("/node_retry_policies", "object", policies);
        } else {
            for (const auto& [name, policy] : policies.items()) {
                const auto pointer = "/node_retry_policies/" + pointer_token(name);
                if (!policy.is_object()) {
                    type_error(pointer, "object", policy);
                    continue;
                }
                auto spec = parse_node_retry_policy(policy, pointer, diagnostics);
                if (!spec.empty()) keys.node_retry_policies[name] = std::move(spec);
            }
        }
    }

    // Exits first: a failure exit is a valid target although it has no successors.
    if (definition.contains("failure_exits")) {
        const auto& exits = definition["failure_exits"];
        if (!exits.is_object()) {
            type_error("/failure_exits", "object", exits);
        } else {
            for (const auto& [name, exit] : exits.items()) {
                const auto pointer = "/failure_exits/" + pointer_token(name);
                if (!exit.is_object()) {
                    type_error(pointer, "object", exit);
                    continue;
                }
                unknown_fields(exit, {"code"}, pointer, diagnostics);
                FailureExit parsed;
                if (exit.contains("code")) {
                    const auto& code = exit["code"];
                    if (!code.is_string()) {
                        type_error(pointer + "/code", "string", code);
                        continue;
                    }
                    const auto text = code.get<std::string>();
                    if (text != "NG.Aborted" && !is_author_failure_code(text)) {
                        diagnostics.push_back(
                            {"GC_FIELD_VALUE", pointer + "/code",
                             pointer + "/code: '" + text +
                                 "' must be an author code ([a-z][a-z0-9_]*(.[a-z0-9_]+)+); the NG. prefix is reserved",
                             json{{"expected", "author code"}, {"actual", text}}});
                        continue;
                    }
                    if (text != "NG.Aborted") parsed.code = text;
                }
                if (context.with_successors.count(name)) {
                    diagnostics.push_back({"GC_FIELD_VALUE", pointer,
                                           "failure exit '" + name +
                                               "' has outgoing edges or routes; a failure exit ends the run",
                                           json{{"expected", "a node without outgoing edges or routes"},
                                                {"actual", name}}});
                    continue;
                }
                keys.failure_exits[name] = std::move(parsed);
            }
        }
    }

    if (definition.contains("failure_edges")) {
        const auto& edges = definition["failure_edges"];
        if (!edges.is_array()) {
            type_error("/failure_edges", "array", edges);
        } else {
            std::set<std::string> sources;
            std::size_t           index = 0;
            for (const auto& edge : edges) {
                const auto pointer = "/failure_edges/" + std::to_string(index++);
                if (!edge.is_object()) {
                    type_error(pointer, "object", edge);
                    continue;
                }
                unknown_fields(edge, {"from", "to", "error_channel"}, pointer, diagnostics);
                FailureEdge parsed;
                bool        shaped = true;
                for (const auto* field : {"from", "to"}) {
                    if (!edge.contains(field)) {
                        diagnostics.push_back({"GC_FIELD_REQUIRED", pointer,
                                               pointer + ": required field '" + std::string(field) + "' is missing",
                                               json{{"field", field}}});
                        shaped = false;
                    } else if (!edge[field].is_string() || edge[field].get<std::string>().empty()) {
                        diagnostics.push_back({"GC_FIELD_VALUE", pointer + "/" + field,
                                               pointer + "/" + field + " must be a node name",
                                               json{{"expected", "non-empty string"}, {"actual", edge[field]}}});
                        shaped = false;
                    }
                }
                if (edge.contains("error_channel")) {
                    if (!edge["error_channel"].is_string() || edge["error_channel"].get<std::string>().empty()) {
                        diagnostics.push_back({"GC_FIELD_VALUE", pointer + "/error_channel",
                                               pointer + "/error_channel must be a channel name",
                                               json{{"expected", "non-empty string"}, {"actual", edge["error_channel"]}}});
                        shaped = false;
                    } else {
                        parsed.error_channel = edge["error_channel"].get<std::string>();
                    }
                }
                if (!shaped) continue;
                parsed.from = edge["from"].get<std::string>();
                parsed.to   = edge["to"].get<std::string>();
                const auto reject = [&](const char* reason, const std::string& why) {
                    diagnostics.push_back({"GC_FAILURE_EDGE", pointer,
                                           "failure edge '" + parsed.from + "' -> '" + parsed.to + "': " + why,
                                           json{{"from", parsed.from}, {"to", parsed.to}, {"reason", reason}}});
                };
                if (parsed.from == START_NODE || parsed.from == END_NODE) {
                    reject("sentinel_source", "only nodes fail");
                } else if (!sources.insert(parsed.from).second) {
                    reject("duplicate_from", "a node has at most one failure edge");
                } else if (parsed.to == START_NODE || parsed.to == END_NODE) {
                    reject("sentinel_target",
                           "route failures to a node; end the run with a failure exit instead");
                } else if (parsed.to == parsed.from) {
                    reject("self_target", "a node cannot handle its own failure; use max_retries to retry it");
                } else if (context.nodes.count(parsed.to) && context.joins.count(parsed.to)) {
                    reject("join_target",
                           "a failure arrival never fills a join slot, so the join could only stall");
                } else if (context.nodes.count(parsed.to) && !context.with_successors.count(parsed.to) &&
                           !keys.failure_exits.count(parsed.to)) {
                    reject("implicit_end_target",
                           "the target has no edges or routes; end it with an explicit exit or make it a failure exit");
                } else if (parsed.error_channel && context.channel_reducers.count(*parsed.error_channel) &&
                           context.channel_reducers.at(*parsed.error_channel) != "append") {
                    reject("error_channel_reducer",
                           "error_channel '" + *parsed.error_channel + "' must use the built-in append reducer");
                } else {
                    keys.failure_edges.push_back(std::move(parsed));
                }
            }
            std::sort(keys.failure_edges.begin(), keys.failure_edges.end(),
                      [](const FailureEdge& lhs, const FailureEdge& rhs) { return lhs.from < rhs.from; });
        }
    }
    return keys;
}

json detail::print_node_retry_policies(const std::map<std::string, NodeRetryPolicySpec>& policies) {
    json result = json::object();
    for (const auto& [name, spec] : policies) {
        if (spec.empty()) continue;
        json policy = json::object();
        if (spec.max_retries) policy["max_retries"] = *spec.max_retries;
        if (spec.initial_delay_ms) policy["initial_delay_ms"] = *spec.initial_delay_ms;
        if (spec.backoff_multiplier) policy["backoff_multiplier"] = *spec.backoff_multiplier;
        if (spec.max_delay_ms) policy["max_delay_ms"] = *spec.max_delay_ms;
        if (!spec.non_retryable.empty()) {
            json codes = json::array();
            for (const auto& code : spec.non_retryable) codes.push_back(code);
            policy["non_retryable"] = std::move(codes);
        }
        if (spec.max_visits) policy["max_visits"] = *spec.max_visits;
        if (spec.error_text) policy["error_text"] = *spec.error_text;
        if (spec.attempt_timeout_ms) policy["attempt_timeout_ms"] = *spec.attempt_timeout_ms;
        result[name] = std::move(policy);
    }
    return result.empty() ? json() : result;
}

json detail::print_failure_edges(const std::vector<FailureEdge>& edges) {
    if (edges.empty()) return json();
    auto sorted = edges;
    std::sort(sorted.begin(), sorted.end(),
              [](const FailureEdge& lhs, const FailureEdge& rhs) { return lhs.from < rhs.from; });
    json result = json::array();
    for (const auto& edge : sorted) {
        json value{{"from", edge.from}, {"to", edge.to}};
        if (edge.error_channel) value["error_channel"] = *edge.error_channel;
        result.push_back(std::move(value));
    }
    return result;
}

json detail::print_failure_exits(const std::map<std::string, FailureExit>& exits) {
    if (exits.empty()) return json();
    json result = json::object();
    for (const auto& [name, exit] : exits) {
        json value = json::object();
        if (!exit.code.empty()) value["code"] = exit.code;
        result[name] = std::move(value);
    }
    return result;
}

bool ParseReport::has_errors() const noexcept {
    return !diagnostics.empty();
}

std::string ParseReport::summary() const {
    if (diagnostics.empty()) return {};
    if (diagnostics.size() == 1 && (diagnostics.front().code == "GC_SCHEMA_VERSION" ||
                                    diagnostics.front().code == "GC_REGISTRY_MISS")) {
        return diagnostics.front().message;
    }
    // Only unknown keys and node config schemas are checked by strict parsing
    // alone, so only they carry the lenient fallback hint. A reserved node name
    // or a missing entry is an error at every schema version (#375).
    bool        lenient_relaxes = false;
    std::string fixes;
    for (const auto& diagnostic : diagnostics) {
        const auto& code = diagnostic.code;
        if (code == "GC_UNKNOWN_FIELD" || code == "GC_NODE_CONFIG") lenient_relaxes = true;
        const char* fix = code == "GC_RESERVED_NODE"
            ? "Rename the node: \"__start__\" and \"__end__\" mark the graph's entry and "
              "exit and may appear only in edges."
            : code == "GC_NO_ENTRY"
            ? "Add an edge from \"__start__\" to the node the run starts at, e.g. "
              "{\"from\": \"__start__\", \"to\": \"<node>\"}."
            : nullptr;
        if (fix && fixes.find(fix) == std::string::npos) fixes += std::string("\n") + fix;
    }
    std::string message = std::string(lenient_relaxes ? "strict " : "") +
        "topology validation failed, " + std::to_string(diagnostics.size()) + " error(s):";
    for (const auto& diagnostic : diagnostics) {
        message += "\n  - " + diagnostic.message;
    }
    message += fixes;
    if (lenient_relaxes) {
        message +=
            "\nAnnotation keys ('_'/'x-' prefixes) are always allowed. "
            "Remove 'schema_version' to fall back to lenient parsing. "
            "See docs/troubleshooting.md \"Graph definitions and registration\".";
    }
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
    auto selected = registry.snapshot();
    NodeContext context = default_context;
    context.registry = selected;
    return link(parse(definition, *selected), context, *selected);
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
    auto invalid_channel_value = [&](const std::string& pointer,
                                     const std::string& display,
                                     const std::string& expected,
                                     const json& actual) {
        diagnostics.push_back(
            {"GC_FIELD_VALUE", pointer, display + " has an invalid value",
             json{{"expected", expected}, {"actual", actual}}});
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

            if (channel.contains("retention")) {
                const auto& retention = channel["retention"];
                if (!retention.is_string()) {
                    field_type(pointer + "/retention", "channels." + name + ".retention",
                               "string", retention);
                } else if (retention == "latest") {
                    parsed.retention = ChannelRetentionPolicy::Latest;
                } else if (retention == "bounded") {
                    parsed.retention = ChannelRetentionPolicy::Bounded;
                } else if (retention != "unbounded") {
                    invalid_channel_value(pointer + "/retention",
                                          "channels." + name + ".retention",
                                          "unbounded, latest, or bounded", retention);
                }
            }
            if (channel.contains("retention_limit")) {
                const auto& limit = channel["retention_limit"];
                if (!limit.is_number_unsigned() &&
                    !(limit.is_number_integer() && limit.get<std::int64_t>() >= 0)) {
                    field_type(pointer + "/retention_limit",
                               "channels." + name + ".retention_limit",
                               "non-negative integer", limit);
                } else {
                    parsed.retention_limit = limit.get<std::uint64_t>();
                }
            }
            if (parsed.retention == ChannelRetentionPolicy::Bounded &&
                parsed.retention_limit == 0) {
                invalid_channel_value(pointer + "/retention_limit",
                                      "channels." + name + ".retention_limit",
                                      "positive integer when retention is bounded",
                                      json(parsed.retention_limit));
            }
            if (channel.contains("persistence")) {
                const auto& persistence = channel["persistence"];
                if (!persistence.is_string()) {
                    field_type(pointer + "/persistence", "channels." + name + ".persistence",
                               "string", persistence);
                } else if (persistence == "ephemeral") {
                    parsed.persistence = ChannelPersistencePolicy::Ephemeral;
                } else if (persistence != "checkpoint") {
                    invalid_channel_value(pointer + "/persistence",
                                          "channels." + name + ".persistence",
                                          "checkpoint or ephemeral", persistence);
                }
            }
            if (strict) {
                enforce_consumed(channel,
                                 {"reducer", "initial", "retention", "retention_limit",
                                  "persistence"},
                                 "channels." + name, pointer, diagnostics);
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
            // The routing sentinels shadow any node of the same name: edges to
            // __end__ end the run and __start__ is never scheduled as a node.
            if (name == START_NODE || name == END_NODE) {
                diagnostics.push_back(
                    {"GC_RESERVED_NODE", pointer,
                     "node name '" + name + "' is reserved for the graph's entry/exit sentinel",
                     json{{"node", name}}});
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

            if (node.contains("barrier") && strict && node["barrier"].is_object() &&
                node["barrier"].contains("wait_for") &&
                detail::uses_slot_syntax(node["barrier"]["wait_for"])) {
                // #363 any_of slots (strict documents only).
                consumed.insert("barrier");
                const auto&       barrier         = node["barrier"];
                const std::string barrier_pointer = pointer + "/barrier";
                if (auto slots = detail::parse_wait_for_slots(
                        barrier["wait_for"], "nodes." + name + ".barrier", barrier_pointer,
                        diagnostics)) {
                    if (detail::legacy_slots(*slots)) {
                        std::set<std::string> members;
                        for (const auto& slot : *slots) members.insert(*slot.begin());
                        topology.barrier_specs[name] = std::move(members);
                    } else {
                        topology.join_specs[name] = JoinSpec{std::move(*slots)};
                    }
                }
                enforce_consumed(barrier, {"wait_for"}, "nodes." + name + ".barrier",
                                 barrier_pointer, diagnostics);
            } else if (node.contains("barrier")) {
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
                const auto before = diagnostics.size();
                enforce_consumed(
                    retry,
                    {"max_retries", "initial_delay_ms", "backoff_multiplier", "max_delay_ms"},
                    "retry_policy", "/retry_policy", diagnostics);
                // #364: failure routes are per node, not graph-wide.
                for (auto index = before; index < diagnostics.size(); ++index) {
                    const auto field = diagnostics[index].witness.value("field", std::string());
                    if (field == "on_failure" || field == "on_exhausted")
                        diagnostics[index].message +=
                            "; route a node's failures with node_retry_policies and failure_edges "
                            "(JavaScript: graph.retryPolicy(node, {on_failure: ...}))";
                }
            }
            topology.retry_policy = policy;
        }
    }

    // #362: graph metadata, read under schema_version >= 1 only. {} is absent.
    if (strict && definition.contains("metadata")) {
        top_consumed.insert("metadata");
        const auto& metadata = definition["metadata"];
        const auto  before   = diagnostics.size();
        detail::append_metadata_diagnostics(metadata, diagnostics);
        if (diagnostics.size() == before && !metadata.empty())
            topology.metadata = neograph::detail::owned_json_copy(metadata);
    }

    // #364: failure routing keys, read under schema_version >= 1 only.
    if (strict && (definition.contains("node_retry_policies") ||
                   definition.contains("failure_edges") || definition.contains("failure_exits"))) {
        for (const auto* key : {"node_retry_policies", "failure_edges", "failure_exits"})
            if (definition.contains(key)) top_consumed.insert(key);
        auto keys                    = detail::parse_failure_routing_keys(definition, diagnostics);
        topology.node_retry_policies = std::move(keys.node_retry_policies);
        topology.failure_edges       = std::move(keys.failure_edges);
        topology.failure_exits       = std::move(keys.failure_exits);
    }

    // Only static edges from __start__ start a run; without one nothing runs and
    // the run would still report Completed.
    if ((edges_ok || !definition.contains("edges")) && !topology.node_defs.empty() &&
        std::none_of(topology.edges.begin(), topology.edges.end(),
                     [](const Edge& edge) { return edge.from == START_NODE; })) {
        diagnostics.push_back({"GC_NO_ENTRY", "/edges",
                               "graph has nodes but no edge from __start__, so nothing would run",
                               json{{"nodes", topology.node_defs.size()}}});
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
    cg.tools             = default_context.tools;
    cg.name              = std::move(topology.name);
    cg.channel_defs      = std::move(topology.channel_defs);
    cg.edges             = std::move(topology.edges);
    cg.conditional_edges = std::move(topology.conditional_edges);
    cg.barrier_specs     = std::move(topology.barrier_specs);
    cg.join_specs        = std::move(topology.join_specs);
    cg.interrupt_before  = std::move(topology.interrupt_before);
    cg.interrupt_after   = std::move(topology.interrupt_after);
    cg.retry_policy      = std::move(topology.retry_policy);
    cg.schema_version    = topology.schema_version;
    cg.metadata          = std::move(topology.metadata);
    cg.node_retry_policies = std::move(topology.node_retry_policies);
    cg.failure_edges       = std::move(topology.failure_edges);
    cg.failure_exits       = std::move(topology.failure_exits);
    if (cg.schema_version == 0) {
        for (auto& edge : cg.conditional_edges) {
            if (edge.routes.empty()) continue;
            const std::string historical_target      = edge.routes.rbegin()->second;
            edge.routes[detail::kLegacyDefaultRoute] = historical_target;
        }
    }
    NodeContext context = default_context;
    if (context.registry.get() != &registry) context.registry = registry.snapshot();
    for (const auto& [name, node_def] : topology.node_defs) {
        const auto type = node_def.value("type", "");
        cg.nodes[name]  = registry.create(type, name, node_def, context);

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
    cg.tools             = default_context.tools;
    cg.name              = std::move(topology.name);
    cg.channel_defs      = std::move(topology.channel_defs);
    cg.edges             = std::move(topology.edges);
    cg.conditional_edges = std::move(topology.conditional_edges);
    cg.barrier_specs     = std::move(topology.barrier_specs);
    cg.join_specs        = std::move(topology.join_specs);
    cg.interrupt_before  = std::move(topology.interrupt_before);
    cg.interrupt_after   = std::move(topology.interrupt_after);
    cg.retry_policy      = std::move(topology.retry_policy);
    cg.schema_version    = topology.schema_version;
    cg.metadata          = std::move(topology.metadata);
    cg.node_retry_policies = std::move(topology.node_retry_policies);
    cg.failure_edges       = std::move(topology.failure_edges);
    cg.failure_exits       = std::move(topology.failure_exits);
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
            if (cd.retention != ChannelRetentionPolicy::Unbounded) {
                c["retention"] =
                    cd.retention == ChannelRetentionPolicy::Latest ? "latest" : "bounded";
                if (cd.retention == ChannelRetentionPolicy::Bounded) {
                    c["retention_limit"] = cd.retention_limit;
                }
            }
            if (cd.persistence == ChannelPersistencePolicy::Ephemeral) {
                c["persistence"] = "ephemeral";
            }
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
            auto jit = join_specs.find(nname);
            if (jit != join_specs.end())
                n["barrier"] = json{{"wait_for", detail::print_wait_for(jit->second.slots)}};
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

    // Absent unless used, so definitions without metadata keep their bytes.
    if (metadata.is_object() && !metadata.empty()) j["metadata"] = metadata;

    // #364, each absent unless used.
    if (auto policies = detail::print_node_retry_policies(node_retry_policies); !policies.is_null())
        j["node_retry_policies"] = std::move(policies);
    if (auto routes = detail::print_failure_edges(failure_edges); !routes.is_null())
        j["failure_edges"] = std::move(routes);
    if (auto exits = detail::print_failure_exits(failure_exits); !exits.is_null())
        j["failure_exits"] = std::move(exits);

    return j;
}

TopologySpec CompiledGraph::topology() const {
    TopologySpec result;
    result.name              = name;
    result.channel_defs      = channel_defs;
    result.edges             = edges;
    result.conditional_edges = conditional_edges;
    result.barrier_specs     = barrier_specs;
    result.join_specs        = join_specs;
    result.interrupt_before  = interrupt_before;
    result.interrupt_after   = interrupt_after;
    result.retry_policy      = retry_policy;
    result.schema_version    = schema_version;
    result.node_defs         = node_defs;
    result.metadata          = metadata;
    result.node_retry_policies = node_retry_policies;
    result.failure_edges       = failure_edges;
    result.failure_exits       = failure_exits;
    for (const auto& [name, wait_for_set] : barrier_specs) {
        auto node = result.node_defs.find(name);
        if (node == result.node_defs.end()) continue;
        json wait_for = json::array();
        for (const auto& upstream : wait_for_set)
            wait_for.push_back(upstream);
        node->second["barrier"] = json{{"wait_for", std::move(wait_for)}};
    }
    for (const auto& [name, join] : join_specs) {
        auto node = result.node_defs.find(name);
        if (node == result.node_defs.end()) continue;
        node->second["barrier"] = json{{"wait_for", detail::print_wait_for(join.slots)}};
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
        "metadata",
        "node_retry_policies",
        "failure_edges",
        "failure_exits",
    };
    for (const auto& [k, v] : definition.items()) {
        if (is_annotation_key(k) || owned.count(k)) continue;
        out[k] = v;
    }
    // #364: the canonical form the printer writes. Defaults are absent, codes
    // and edges sorted; anything unreadable is kept so TV still flags it.
    if (definition.contains("node_retry_policies")) {
        const auto& policies = definition["node_retry_policies"];
        if (!policies.is_object()) {
            out["node_retry_policies"] = policies;
        } else {
            json canonical_policies = json::object();
            for (const auto& [name, policy] : policies.items()) {
                if (!policy.is_object()) {
                    canonical_policies[name] = policy;
                    continue;
                }
                json p = json::object();
                for (const auto& [key, value] : policy.items()) {
                    if (is_annotation_key(key)) continue;
                    if (key == "backoff_multiplier" && value.is_number()) {
                        p[key] = value.get<double>();
                    } else if (key == "non_retryable" && value.is_array()) {
                        std::set<std::string> codes;
                        bool                  strings = true;
                        for (const auto& code : value) {
                            if (code.is_string()) codes.insert(code.get<std::string>());
                            else strings = false;
                        }
                        if (!strings) {
                            p[key] = value;
                        } else if (!codes.empty()) {
                            json sorted = json::array();
                            for (const auto& code : codes) sorted.push_back(code);
                            p[key] = std::move(sorted);
                        }
                    } else {
                        p[key] = value;
                    }
                }
                if (!p.empty()) canonical_policies[name] = std::move(p);
            }
            if (!canonical_policies.empty()) out["node_retry_policies"] = std::move(canonical_policies);
        }
    }
    if (definition.contains("failure_edges")) {
        const auto& edges = definition["failure_edges"];
        if (!edges.is_array()) {
            out["failure_edges"] = edges;
        } else if (!edges.empty()) {
            std::vector<json> sorted;
            for (const auto& edge : edges) {
                json e = json::object();
                if (edge.is_object()) {
                    for (const auto& [key, value] : edge.items())
                        if (!is_annotation_key(key)) e[key] = value;
                } else {
                    e = edge;
                }
                sorted.push_back(sort_keys(e));
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const json& lhs, const json& rhs) {
                const auto from = [](const json& value) {
                    return value.is_object() && value.contains("from") && value["from"].is_string()
                               ? value["from"].get<std::string>()
                               : std::string();
                };
                return from(lhs) < from(rhs);
            });
            json arr = json::array();
            for (auto& e : sorted) arr.push_back(std::move(e));
            out["failure_edges"] = std::move(arr);
        }
    }
    if (definition.contains("failure_exits")) {
        const auto& exits = definition["failure_exits"];
        if (!exits.is_object()) {
            out["failure_exits"] = exits;
        } else if (!exits.empty()) {
            json canonical_exits = json::object();
            for (const auto& [name, exit] : exits.items()) {
                json x = json::object();
                if (exit.is_object()) {
                    for (const auto& [key, value] : exit.items()) {
                        if (is_annotation_key(key)) continue;
                        if (key == "code" && value.is_string() && value.get<std::string>() == "NG.Aborted")
                            continue;
                        x[key] = value;
                    }
                } else {
                    x = exit;
                }
                canonical_exits[name] = std::move(x);
            }
            out["failure_exits"] = std::move(canonical_exits);
        }
    }

    // Metadata is compared as a value; an empty object is the same as none.
    if (definition.contains("metadata") &&
        !(definition["metadata"].is_object() && definition["metadata"].empty())) {
        out["metadata"] = definition["metadata"];
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
                    json                  arr = json::array();
                    if (v.is_object() && v.contains("wait_for") &&
                        detail::uses_slot_syntax(v["wait_for"])) {
                        // #363: the printer's canonical form; entries it
                        // cannot read are kept so TV still flags them.
                        std::vector<std::set<std::string>> slots;
                        json                               unreadable = json::array();
                        for (const auto& entry : v["wait_for"]) {
                            if (entry.is_string()) {
                                slots.push_back({entry.get<std::string>()});
                                continue;
                            }
                            std::set<std::string> slot;
                            bool                  readable = entry.is_object() && entry.size() == 1 &&
                                            entry.contains("any_of") && entry["any_of"].is_array();
                            if (readable)
                                for (const auto& source : entry["any_of"]) {
                                    if (!source.is_string()) readable = false;
                                    else slot.insert(source.get<std::string>());
                                }
                            if (readable && !slot.empty())
                                slots.push_back(std::move(slot));
                            else
                                unreadable.push_back(entry);
                        }
                        if (detail::legacy_slots(slots) && unreadable.empty()) {
                            for (const auto& slot : slots) wait_for.insert(*slot.begin());
                        } else {
                            arr = detail::print_wait_for(slots);
                            for (const auto& entry : unreadable) arr.push_back(entry);
                        }
                    } else if (v.is_object() && v.contains("wait_for")) {
                        for (const auto& up : v["wait_for"]) {
                            wait_for.insert(up.get<std::string>());
                        }
                    }
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
