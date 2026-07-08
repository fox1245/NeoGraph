#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace neograph::graph {

namespace {

// Highest schema_version this compiler understands. Documents declaring
// a higher version were written for a newer engine — refusing them is
// the whole point of carrying the field (silent reinterpretation of a
// newer document is worse than an error).
constexpr int kSupportedSchemaVersion = 1;

// Keys starting with '_' or 'x-' are annotations: for humans
// (`_comment`, used across the cookbook corpus) and external tooling
// (`x-studio-pos`). The engine never consumes them, strict mode never
// flags them, and canon() strips them before equivalence comparison.
bool is_annotation_key(const std::string& k) {
    return (!k.empty() && k[0] == '_') || k.rfind("x-", 0) == 0;
}

// Consumed-key accounting: every object the compiler owns gets a
// consumed-set populated AT THE READ SITE (inside the same block that
// parses the key — never a detached allowlist). If a parsing step is
// deleted, its mark disappears with it, and any strict document using
// that feature fails here instead of silently degrading. This is the
// structural fix for the v0.1.0–v0.1.7 conditional_edges drop class.
void enforce_consumed(const json& obj,
                      const std::set<std::string>& consumed,
                      const std::string& path,
                      std::vector<std::string>& errors) {
    if (!obj.is_object()) return;
    for (const auto& [k, v] : obj.items()) {
        (void)v;
        if (is_annotation_key(k)) continue;
        if (!consumed.count(k)) {
            errors.push_back(path + ": unknown or unconsumed key '" + k + "'");
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
        for (const auto& [k, val] : v.items()) { (void)val; keys.push_back(k); }
        std::sort(keys.begin(), keys.end());
        json out = json::object();
        for (const auto& k : keys) out[k] = sort_keys(v[k]);
        return out;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto& e : v) out.push_back(sort_keys(e));
        return out;
    }
    return v;
}

// Human-readable structural mismatch report for translation-validation
// failures (the wrapper has no json::diff). Phrased from the compiler's
// point of view: input-only = lost, reemit-only = fabricated.
void collect_mismatches(const json& declared, const json& compiled,
                        const std::string& path,
                        std::vector<std::string>& out) {
    constexpr size_t kMax = 8;
    if (out.size() >= kMax || declared == compiled) return;
    if (declared.is_object() && compiled.is_object()) {
        for (const auto& [k, v] : declared.items()) {
            if (out.size() >= kMax) return;
            if (!compiled.contains(k)) {
                out.push_back(path + "/" + k + ": lost in compilation");
            } else {
                collect_mismatches(v, compiled[k], path + "/" + k, out);
            }
        }
        for (const auto& [k, v] : compiled.items()) {
            (void)v;
            if (out.size() >= kMax) return;
            if (!declared.contains(k)) {
                out.push_back(path + "/" + k + ": fabricated by compilation");
            }
        }
        return;
    }
    if (declared.is_array() && compiled.is_array()) {
        if (declared.size() != compiled.size()) {
            out.push_back(path + ": " + std::to_string(declared.size())
                          + " element(s) declared, "
                          + std::to_string(compiled.size()) + " compiled");
            return;
        }
        for (size_t i = 0; i < declared.size(); ++i) {
            if (out.size() >= kMax) return;
            collect_mismatches(declared[i], compiled[i],
                               path + "[" + std::to_string(i) + "]", out);
        }
        return;
    }
    out.push_back(path + ": declared " + declared.dump()
                  + ", compiled " + compiled.dump());
}

// Normalize one conditional edge (either the legacy inline `edges`
// form or a top-level `conditional_edges` item) for canon(). Unknown
// keys are preserved so TV flags anything the compiler ignored;
// annotations are stripped; empty routes are dropped.
json canon_conditional(const json& e) {
    json out = json::object();
    for (const auto& [k, v] : e.items()) {
        if (is_annotation_key(k)) continue;
        if (k == "type") continue;              // form marker, not meaning
        if (k == "routes") {
            if (v.is_object() && !v.empty()) out["routes"] = v;
            continue;
        }
        out[k] = v;
    }
    return out;
}

} // namespace

CompiledGraph GraphCompiler::compile(const json& definition,
                                     const NodeContext& default_context) {
    CompiledGraph cg;
    std::vector<std::string> errors;
    std::set<std::string> top_consumed;

    // --- schema_version (strict-mode gate) ---
    if (definition.contains("schema_version")) {
        top_consumed.insert("schema_version");
        const auto& sv = definition["schema_version"];
        if (!sv.is_number_integer() || sv.get<int>() < 0) {
            throw std::runtime_error(
                "topology 'schema_version' must be a non-negative integer, got: "
                + sv.dump());
        }
        cg.schema_version = sv.get<int>();
        if (cg.schema_version > kSupportedSchemaVersion) {
            throw std::runtime_error(
                "topology schema_version " + std::to_string(cg.schema_version)
                + " is newer than this engine supports (max "
                + std::to_string(kSupportedSchemaVersion)
                + "). Upgrade NeoGraph or re-export the topology.");
        }
    }
    const bool strict = cg.schema_version >= 1;

    cg.name = definition.value("name", "unnamed_graph");
    top_consumed.insert("name");

    // --- Channels ---
    if (definition.contains("channels")) {
        top_consumed.insert("channels");
        for (const auto& [name, ch_def] : definition["channels"].items()) {
            ChannelDef cd;
            std::set<std::string> ch_consumed;

            cd.name         = name;
            cd.reducer_name = ch_def.value("reducer", "overwrite");
            ch_consumed.insert("reducer");

            cd.has_initial   = ch_def.contains("initial");
            cd.initial_value = cd.has_initial ? ch_def["initial"] : json();
            ch_consumed.insert("initial");

            if (cd.reducer_name == "append")
                cd.type = ReducerType::APPEND;
            else if (cd.reducer_name == "overwrite")
                cd.type = ReducerType::OVERWRITE;
            else
                cd.type = ReducerType::CUSTOM;

            if (strict) {
                enforce_consumed(ch_def, ch_consumed,
                                 "channels." + name, errors);
            }
            cg.channel_defs.push_back(std::move(cd));
        }
    }

    // --- Nodes + optional per-node barrier specs ---
    if (definition.contains("nodes")) {
        top_consumed.insert("nodes");
        for (const auto& [name, node_def] : definition["nodes"].items()) {
            auto type = node_def.value("type", "");
            auto node = NodeFactory::instance().create(type, name, node_def, default_context);
            cg.nodes[name] = std::move(node);

            std::set<std::string> node_consumed = {"type"};

            // {"barrier": {"wait_for": [...]}} opts this node into
            // AND-join semantics: it fires only after every listed
            // upstream has signaled (accumulated across super-steps).
            if (node_def.contains("barrier")) {
                node_consumed.insert("barrier");
                const auto& b = node_def["barrier"];
                std::set<std::string> wait_for;
                std::set<std::string> barrier_consumed;
                if (b.contains("wait_for")) {
                    barrier_consumed.insert("wait_for");
                    for (const auto& up : b["wait_for"]) {
                        wait_for.insert(up.get<std::string>());
                    }
                }
                if (!wait_for.empty()) {
                    cg.barrier_specs[name] = std::move(wait_for);
                } else if (strict) {
                    // Historically an empty/missing wait_for was
                    // silently dropped — the node quietly lost its
                    // AND-join. Strict mode refuses instead.
                    errors.push_back(
                        "nodes." + name + ".barrier: 'wait_for' is missing or "
                        "empty — the barrier would be silently dropped. "
                        "List the upstream nodes to wait for, or remove "
                        "the 'barrier' block.");
                }
                if (strict) {
                    enforce_consumed(b, barrier_consumed,
                                     "nodes." + name + ".barrier", errors);
                }
            }

            // Strict mode checks the node's config keys against the
            // type's declared schema. Enforcement is closed-world only
            // when the registration declared "properties" and did not
            // opt out via "additionalProperties": true — types
            // registered without a schema stay permissive (the
            // cookbook's custom nodes carry free-form config).
            if (strict) {
                const json schema = NodeFactory::instance().config_schema(type);
                bool open = !schema.contains("properties");
                if (schema.contains("additionalProperties")) {
                    const auto& ap = schema["additionalProperties"];
                    if (!ap.is_boolean() || ap.get<bool>()) open = true;
                }
                if (!open) {
                    for (const auto& [pk, pv] : schema["properties"].items()) {
                        (void)pv;
                        node_consumed.insert(pk);
                    }
                    enforce_consumed(node_def, node_consumed,
                                     "nodes." + name, errors);
                }
            }

            // Verbatim node definition minus "barrier" — barriers are
            // re-emitted from barrier_specs so translation validation
            // compares compiled state, not an input echo. (Copy-skip:
            // the yyjson wrapper has no erase().)
            json stored = json::object();
            for (const auto& [k, v] : node_def.items()) {
                if (k == "barrier") continue;
                stored[k] = v;
            }
            cg.node_defs[name] = std::move(stored);
        }
    }

    // --- Edges (regular + conditional) ---
    // Accept either form for conditionals:
    //   1. Inline in `edges` with a `condition` field (legacy).
    //   2. Top-level `conditional_edges` array (LangGraph parity, matches
    //      our README + Python examples). The compiler used to silently
    //      drop form 2, which made the documented Python ReAct example
    //      degenerate to a single LLM call with no tool dispatch.
    auto parse_conditional = [&](const json& edge_def, const std::string& path,
                                 bool inline_form) {
        ConditionalEdge ce;
        std::set<std::string> e_consumed = {"from", "condition", "routes"};
        if (inline_form) e_consumed.insert("type");
        ce.from      = edge_def.at("from").get<std::string>();
        ce.condition = edge_def.at("condition").get<std::string>();
        if (edge_def.contains("routes")) {
            for (const auto& [key, target] : edge_def["routes"].items()) {
                ce.routes[key] = target.get<std::string>();
            }
        }
        if (strict) enforce_consumed(edge_def, e_consumed, path, errors);
        cg.conditional_edges.push_back(std::move(ce));
    };

    if (definition.contains("edges")) {
        top_consumed.insert("edges");
        size_t i = 0;
        for (const auto& edge_def : definition["edges"]) {
            const std::string path = "edges[" + std::to_string(i++) + "]";
            bool is_conditional = edge_def.contains("condition")
                               || edge_def.value("type", "") == "conditional";

            if (is_conditional) {
                // NOTE: a 'to' on an inline conditional is NOT consumed
                // — routing goes through 'routes', so 'to' would be
                // silently ignored. Strict mode surfaces it.
                parse_conditional(edge_def, path, /*inline_form=*/true);
            } else {
                Edge e;
                e.from = edge_def.at("from").get<std::string>();
                e.to   = edge_def.at("to").get<std::string>();
                if (strict) enforce_consumed(edge_def, {"from", "to"}, path, errors);
                cg.edges.push_back(std::move(e));
            }
        }
    }
    if (definition.contains("conditional_edges")) {
        top_consumed.insert("conditional_edges");
        size_t i = 0;
        for (const auto& edge_def : definition["conditional_edges"]) {
            parse_conditional(edge_def,
                              "conditional_edges[" + std::to_string(i++) + "]",
                              /*inline_form=*/false);
        }
    }

    // --- Interrupt sets ---
    if (definition.contains("interrupt_before")) {
        top_consumed.insert("interrupt_before");
        for (const auto& n : definition["interrupt_before"]) {
            cg.interrupt_before.insert(n.get<std::string>());
        }
    }
    if (definition.contains("interrupt_after")) {
        top_consumed.insert("interrupt_after");
        for (const auto& n : definition["interrupt_after"]) {
            cg.interrupt_after.insert(n.get<std::string>());
        }
    }

    // --- Retry policy (optional; engine uses its own default otherwise) ---
    if (definition.contains("retry_policy")) {
        top_consumed.insert("retry_policy");
        auto rp = definition["retry_policy"];
        RetryPolicy policy;
        std::set<std::string> rp_consumed;
        policy.max_retries        = rp.value("max_retries", 0);
        rp_consumed.insert("max_retries");
        policy.initial_delay_ms   = rp.value("initial_delay_ms", 100);
        rp_consumed.insert("initial_delay_ms");
        policy.backoff_multiplier = rp.value("backoff_multiplier", 2.0f);
        rp_consumed.insert("backoff_multiplier");
        policy.max_delay_ms       = rp.value("max_delay_ms", 5000);
        rp_consumed.insert("max_delay_ms");
        if (strict) enforce_consumed(rp, rp_consumed, "retry_policy", errors);
        cg.retry_policy = policy;
    }

    if (strict) {
        enforce_consumed(definition, top_consumed, "$", errors);
        if (!errors.empty()) {
            std::string msg =
                "strict topology validation failed (schema_version "
                + std::to_string(cg.schema_version) + "), "
                + std::to_string(errors.size()) + " error(s):";
            for (const auto& e : errors) msg += "\n  - " + e;
            msg += "\nAnnotation keys ('_'/'x-' prefixes) are always allowed. "
                   "Remove 'schema_version' to fall back to lenient parsing. "
                   "See docs/troubleshooting.md \"Strict topology validation\".";
            throw std::runtime_error(msg);
        }
    }

    return cg;
}

// =========================================================================
// Re-emission + canonicalization + translation validation
// =========================================================================

json CompiledGraph::to_json() const {
    json j = json::object();
    if (schema_version > 0) j["schema_version"] = schema_version;
    j["name"] = name;

    if (!channel_defs.empty()) {
        json channels = json::object();
        for (const auto& cd : channel_defs) {
            json c = json::object();
            c["reducer"] = cd.reducer_name;
            if (cd.has_initial) c["initial"] = cd.initial_value;
            channels[cd.name] = std::move(c);
        }
        j["channels"] = std::move(channels);
    }

    if (!node_defs.empty()) {
        json nodes = json::object();
        for (const auto& [nname, ndef] : node_defs) {
            json n = ndef;
            auto bit = barrier_specs.find(nname);
            if (bit != barrier_specs.end()) {
                json wait_for = json::array();
                for (const auto& up : bit->second) wait_for.push_back(up);
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
            json e = json::object();
            e["from"]      = ce.from;
            e["condition"] = ce.condition;
            if (!ce.routes.empty()) {
                json routes = json::object();
                for (const auto& [k, v] : ce.routes) routes[k] = v;
                e["routes"] = std::move(routes);
            }
            arr.push_back(std::move(e));
        }
        j["conditional_edges"] = std::move(arr);
    }

    auto emit_set = [&](const char* key, const std::set<std::string>& s) {
        if (s.empty()) return;
        json arr = json::array();
        for (const auto& n : s) arr.push_back(n);
        j[key] = std::move(arr);
    };
    emit_set("interrupt_before", interrupt_before);
    emit_set("interrupt_after", interrupt_after);

    if (retry_policy) {
        j["retry_policy"] = json{
            {"max_retries",        retry_policy->max_retries},
            {"initial_delay_ms",   retry_policy->initial_delay_ms},
            {"backoff_multiplier", retry_policy->backoff_multiplier},
            {"max_delay_ms",       retry_policy->max_delay_ms},
        };
    }

    return j;
}

json GraphCompiler::canon(const json& definition) {
    json out = json::object();

    // Unknown top-level keys are preserved (minus annotations) — the
    // TV compare is precisely how a key the compiler ignored becomes
    // visible. Owned keys are rebuilt in normalized form below.
    static const std::set<std::string> owned = {
        "schema_version", "name", "channels", "nodes", "edges",
        "conditional_edges", "interrupt_before", "interrupt_after",
        "retry_policy",
    };
    for (const auto& [k, v] : definition.items()) {
        if (is_annotation_key(k) || owned.count(k)) continue;
        out[k] = v;
    }

    if (definition.contains("schema_version")
        && definition["schema_version"].is_number_integer()
        && definition["schema_version"].get<int>() > 0) {
        out["schema_version"] = definition["schema_version"];
    }
    out["name"] = definition.value("name", "unnamed_graph");

    if (definition.contains("channels") && definition["channels"].is_object()
        && !definition["channels"].empty()) {
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

    if (definition.contains("nodes") && definition["nodes"].is_object()
        && !definition["nodes"].empty()) {
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
                    for (const auto& up : wait_for) arr.push_back(up);
                    json b = json::object();
                    if (v.is_object()) {
                        for (const auto& [bk, bv] : v.items()) {
                            if (is_annotation_key(bk) || bk == "wait_for") continue;
                            b[bk] = bv;   // unknown barrier keys preserved
                        }
                    }
                    b["wait_for"] = std::move(arr);
                    n["barrier"] = std::move(b);
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
            bool is_conditional = e.contains("condition")
                               || e.value("type", "") == "conditional";
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
    if (definition.contains("conditional_edges")
        && definition["conditional_edges"].is_array()) {
        for (const auto& e : definition["conditional_edges"]) {
            conditional.push_back(sort_keys(canon_conditional(e)));
        }
    }
    auto by_dump = [](const json& a, const json& b) { return a.dump() < b.dump(); };
    std::sort(plain.begin(), plain.end(), by_dump);
    std::sort(conditional.begin(), conditional.end(), by_dump);
    auto to_array = [](const std::vector<json>& v) {
        json arr = json::array();
        for (const auto& e : v) arr.push_back(e);
        return arr;
    };
    if (!plain.empty())       out["edges"] = to_array(plain);
    if (!conditional.empty()) out["conditional_edges"] = to_array(conditional);

    auto canon_set = [&](const char* key) {
        if (!definition.contains(key) || !definition[key].is_array()) return;
        std::set<std::string> s;
        for (const auto& n : definition[key]) s.insert(n.get<std::string>());
        if (s.empty()) return;
        json arr = json::array();
        for (const auto& n : s) arr.push_back(n);
        out[key] = std::move(arr);
    };
    canon_set("interrupt_before");
    canon_set("interrupt_after");

    if (definition.contains("retry_policy") && definition["retry_policy"].is_object()) {
        const auto& rp = definition["retry_policy"];
        json p = json::object();
        for (const auto& [k, v] : rp.items()) {
            if (is_annotation_key(k)) continue;
            p[k] = v;    // unknown keys preserved
        }
        if (!p.contains("max_retries"))        p["max_retries"] = 0;
        if (!p.contains("initial_delay_ms"))   p["initial_delay_ms"] = 100;
        if (!p.contains("max_delay_ms"))       p["max_delay_ms"] = 5000;
        p["backoff_multiplier"] = p.contains("backoff_multiplier")
            ? canon_backoff(p["backoff_multiplier"])
            : json(2.0f);
        out["retry_policy"] = std::move(p);
    }

    // Recursive key sort — see sort_keys(): equality is serialization-
    // based, so canonical form must fix object key order everywhere
    // (including inside pass-through node configs).
    return sort_keys(out);
}

json GraphCompiler::upgrade_to_latest(const json& definition) {
    if (definition.contains("schema_version")
        && definition["schema_version"].is_number_integer()
        && definition["schema_version"].get<int>() >= kSupportedSchemaVersion) {
        return definition;   // already current
    }

    // Rebuild an object keeping `consumed` keys, renaming everything
    // else (except annotations) into the x- namespace — data preserved,
    // strict mode satisfied, semantics identical to the lenient parser
    // that ignored those keys.
    auto quarantine = [](const json& obj, const std::set<std::string>& consumed) {
        json out = json::object();
        for (const auto& [k, v] : obj.items()) {
            if (consumed.count(k) || is_annotation_key(k)) out[k] = v;
            else out["x-upgraded-" + k] = v;
        }
        return out;
    };

    json up = json::object();
    up["schema_version"] = kSupportedSchemaVersion;

    static const std::set<std::string> top_keys = {
        "name", "channels", "nodes", "edges", "conditional_edges",
        "interrupt_before", "interrupt_after", "retry_policy",
    };
    for (const auto& [k, v] : definition.items()) {
        if (k == "schema_version") continue;   // re-stamped above
        if (!top_keys.count(k) && !is_annotation_key(k)) {
            up["x-upgraded-" + k] = v;
            continue;
        }
        if (k == "channels" && v.is_object()) {
            json channels = json::object();
            for (const auto& [cn, cd] : v.items()) {
                channels[cn] = cd.is_object()
                    ? quarantine(cd, {"reducer", "initial"}) : cd;
            }
            up["channels"] = std::move(channels);
        } else if (k == "nodes" && v.is_object()) {
            json nodes = json::object();
            for (const auto& [nn, nd] : v.items()) {
                if (!nd.is_object()) { nodes[nn] = nd; continue; }
                // Node config keys are checked against the declared
                // schema only when it is closed-world (mirrors strict
                // compile) — permissive types keep their config as-is.
                const std::string type = nd.value("type", "");
                const json schema = NodeFactory::instance().config_schema(type);
                bool open = !schema.contains("properties");
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
                    const auto& b = node["barrier"];
                    const bool empty = !b.is_object() || !b.contains("wait_for")
                        || !b["wait_for"].is_array() || b["wait_for"].empty();
                    if (empty) {
                        json cleaned = json::object();
                        for (const auto& [nk, nv] : node.items()) {
                            if (nk != "barrier") cleaned[nk] = nv;
                        }
                        node = std::move(cleaned);
                    }
                }
                nodes[nn] = std::move(node);
            }
            up["nodes"] = std::move(nodes);
        } else if (k == "edges" && v.is_array()) {
            json edges = json::array();
            for (const auto& e : v) {
                if (!e.is_object()) { edges.push_back(e); continue; }
                const bool cond = e.contains("condition")
                               || e.value("type", "") == "conditional";
                edges.push_back(cond
                    ? quarantine(e, {"from", "condition", "routes", "type"})
                    : quarantine(e, {"from", "to"}));
            }
            up["edges"] = std::move(edges);
        } else if (k == "conditional_edges" && v.is_array()) {
            json ces = json::array();
            for (const auto& e : v) {
                ces.push_back(e.is_object()
                    ? quarantine(e, {"from", "condition", "routes"}) : e);
            }
            up["conditional_edges"] = std::move(ces);
        } else if (k == "retry_policy" && v.is_object()) {
            up["retry_policy"] = quarantine(v, {"max_retries", "initial_delay_ms",
                                                "backoff_multiplier", "max_delay_ms"});
        } else {
            up[k] = v;
        }
    }
    return up;
}

void GraphCompiler::verify_roundtrip(const json& definition,
                                     const CompiledGraph& cg) {
    const json a = canon(definition);
    const json b = canon(cg.to_json());
    if (a == b) return;

    // Structural mismatch report: what compilation lost or rewired.
    std::vector<std::string> mismatches;
    collect_mismatches(a, b, "$", mismatches);
    std::string msg =
        "translation validation failed: compiled graph does not round-trip "
        "to its definition — the compiler dropped or rewired something:";
    for (const auto& m : mismatches) msg += "\n  - " + m;

    if (cg.schema_version >= 1) {
        throw std::runtime_error(msg);
    }
    // Lenient documents keep compiling (historical behavior), but the
    // silent drop is no longer silent. FILE* stderr, not std::cerr —
    // see graph_executor.cpp for the Windows capfd rationale.
    std::fprintf(stderr, "[neograph] warning: %s\n", msg.c_str());
}

} // namespace neograph::graph
