#include <neograph/graph/elaborator.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::graph {

namespace {

bool is_annotation_key(const std::string& k) {
    return (!k.empty() && k[0] == '_') || k.rfind("x-", 0) == 0;
}

[[noreturn]] void fail(const std::string& source_coord, const std::string& msg) {
    throw std::runtime_error("elaboration failed at " + source_coord + ": " + msg);
}

// {"$var": "x"} / {"$param": "x"} detector: single-key object.
bool is_ref(const json& v, const char* key, std::string& out_name) {
    if (!v.is_object() || v.size() != 1 || !v.contains(key)) return false;
    const json name = v[key];
    if (!name.is_string()) return false;
    out_name = name.get<std::string>();
    return true;
}

// One substitution pass, parameterized so vars ({"$var"}, "${...}")
// and template params ({"$param"}, "@{...}") share the machinery.
struct Subst {
    const std::map<std::string, json>& env;
    const char* whole_key;     // "$var" | "$param"
    std::string open;          // "${"   | "@{"
    // When false, an unknown reference passes through untouched
    // (used by the var pass so template-param syntax survives until
    // instantiation; the final validation pass rejects leftovers).
    bool foreign_ok;

    json interpolate_string(const std::string& s, const std::string& path) const {
        // Exact "${name}" → whole-value substitution.
        if (s.size() > open.size() + 1 && s.rfind(open, 0) == 0 && s.back() == '}'
            && s.find('}') == s.size() - 1) {
            const std::string name = s.substr(open.size(),
                                              s.size() - open.size() - 1);
            auto it = env.find(name);
            if (it == env.end()) {
                if (foreign_ok) return json(s);
                fail(path, "unknown reference '" + name + "'");
            }
            return it->second;
        }
        // Concatenating interpolation — scalars only.
        std::string out;
        size_t pos = 0;
        while (pos < s.size()) {
            auto at = s.find(open, pos);
            if (at == std::string::npos) { out += s.substr(pos); break; }
            auto close = s.find('}', at + open.size());
            if (close == std::string::npos) { out += s.substr(pos); break; }
            const std::string name = s.substr(at + open.size(),
                                              close - at - open.size());
            auto it = env.find(name);
            if (it == env.end()) {
                if (foreign_ok) { out += s.substr(pos, close + 1 - pos);
                                  pos = close + 1; continue; }
                fail(path, "unknown reference '" + name + "' in string interpolation");
            }
            const json& val = it->second;
            std::string piece;
            if (val.is_string())       piece = val.get<std::string>();
            else if (val.is_number() || val.is_boolean()) piece = val.dump();
            else fail(path, "'" + name + "' interpolates into a string but is not a scalar");
            out += s.substr(pos, at - pos) + piece;
            pos = close + 1;
        }
        return json(out);
    }

    json apply(const json& v, const std::string& path) const {
        std::string name;
        if (is_ref(v, whole_key, name)) {
            auto it = env.find(name);
            if (it == env.end()) {
                if (foreign_ok) return v;
                fail(path, std::string("unknown ") + whole_key + " '" + name + "'");
            }
            return it->second;
        }
        if (v.is_string()) return interpolate_string(v.get<std::string>(), path);
        if (v.is_object()) {
            json out = json::object();
            for (const auto& [k, val] : v.items()) {
                // Annotations pass through verbatim — a "${...}" in a
                // _comment must not be an error.
                out[k] = is_annotation_key(k) ? val : apply(val, path + "." + k);
            }
            return out;
        }
        if (v.is_array()) {
            json out = json::array();
            size_t i = 0;
            for (const auto& e : v) {
                out.push_back(apply(e, path + "[" + std::to_string(i++) + "]"));
            }
            return out;
        }
        return v;
    }
};

// Resolve "vars" into a fully-substituted environment (DFS, cycle-checked).
std::map<std::string, json> resolve_vars(const json& vars_def) {
    std::map<std::string, json> resolved;
    std::set<std::string> visiting;

    // Recursive resolver via explicit lambda-with-self.
    struct Resolver {
        const json& defs;
        std::map<std::string, json>& done;
        std::set<std::string>& visiting;

        json resolve(const std::string& name) {
            auto dit = done.find(name);
            if (dit != done.end()) return dit->second;
            if (!defs.contains(name)) fail("vars." + name, "unknown var");
            if (!visiting.insert(name).second) {
                fail("vars." + name, "cyclic var reference");
            }
            // Substitute using every var this one references (resolved
            // on demand): walk the value; on each reference, recurse.
            json value = substitute(defs[name], "vars." + name);
            visiting.erase(name);
            done[name] = value;
            return value;
        }

        json substitute(const json& v, const std::string& path) {
            std::string ref;
            if (is_ref(v, "$var", ref)) return resolve(ref);
            if (v.is_string()) {
                // Delegate interpolation to Subst over a lazy env: build
                // the env of referenced names first.
                const std::string s = v.get<std::string>();
                std::map<std::string, json> env;
                size_t pos = 0;
                while ((pos = s.find("${", pos)) != std::string::npos) {
                    auto close = s.find('}', pos + 2);
                    if (close == std::string::npos) break;
                    const std::string name = s.substr(pos + 2, close - pos - 2);
                    env[name] = resolve(name);
                    pos = close + 1;
                }
                return Subst{env, "$var", "${", /*foreign_ok=*/false}
                    .interpolate_string(s, path);
            }
            if (v.is_object()) {
                json out = json::object();
                for (const auto& [k, val] : v.items()) {
                    out[k] = is_annotation_key(k) ? val
                                                  : substitute(val, path + "." + k);
                }
                return out;
            }
            if (v.is_array()) {
                json out = json::array();
                size_t i = 0;
                for (const auto& e : v) {
                    out.push_back(substitute(e, path + "[" + std::to_string(i++) + "]"));
                }
                return out;
            }
            return v;
        }
    };

    Resolver r{vars_def, resolved, visiting};
    for (const auto& [name, v] : vars_def.items()) {
        (void)v;
        r.resolve(name);
    }
    return resolved;
}

// Rename template-local node references under a prefix.
struct Prefixer {
    const std::set<std::string>& locals;
    std::string prefix;

    std::string map_name(const std::string& n) const {
        return locals.count(n) ? prefix + "_" + n : n;
    }
};

} // namespace

ElaborationResult Elaborator::elaborate(const json& dsl_doc) {
    if (!dsl_doc.is_object()) {
        throw std::runtime_error("elaboration failed at $: document must be an object");
    }

    const bool has_dsl = dsl_doc.contains("vars") || dsl_doc.contains("templates")
                      || dsl_doc.contains("use");

    // ---- vars environment -------------------------------------------------
    std::map<std::string, json> vars;
    if (dsl_doc.contains("vars")) {
        if (!dsl_doc["vars"].is_object()) fail("vars", "must be an object");
        vars = resolve_vars(dsl_doc["vars"]);
    }
    const Subst var_pass{vars, "$var", "${", /*foreign_ok=*/false};
    // During template-body handling the var syntax may appear too; the
    // param pass must let it through untouched for the global var pass.
    json sourcemap = json::array();
    auto map_entry = [&](const std::string& target, const std::string& source) {
        sourcemap.push_back(json{{"target", target}, {"source", source}});
    };

    // ---- start from the main document minus DSL keys ----------------------
    json core = json::object();
    for (const auto& [k, v] : dsl_doc.items()) {
        if (k == "vars" || k == "templates" || k == "use") continue;
        core[k] = v;
    }

    // ---- expand "use" ------------------------------------------------------
    if (dsl_doc.contains("use")) {
        if (!dsl_doc["use"].is_array()) fail("use", "must be an array");
        const json templates = dsl_doc.contains("templates")
                                   ? dsl_doc["templates"] : json::object();
        size_t i = 0;
        for (const auto& use : dsl_doc["use"]) {
            const std::string coord = "use[" + std::to_string(i++) + "]";
            if (!use.is_object()) fail(coord, "must be an object");
            const std::string tname = use.value("template", "");
            if (!templates.contains(tname)) {
                fail(coord, "unknown template '" + tname + "'");
            }
            const std::string prefix = use.value("prefix", "");
            if (prefix.empty()) fail(coord, "missing 'prefix'");

            // when: var-substituted, must end up boolean.
            if (use.contains("when")) {
                const json w = var_pass.apply(use["when"], coord + ".when");
                if (!w.is_boolean()) fail(coord + ".when", "must resolve to a boolean");
                if (!w.get<bool>()) continue;
            }

            const json& tmpl = templates[tname];
            // Exact param/arg matching — a typo'd arg must not vanish.
            std::set<std::string> params;
            if (tmpl.contains("params")) {
                for (const auto& p : tmpl["params"]) params.insert(p.get<std::string>());
            }
            std::map<std::string, json> args;
            if (use.contains("args")) {
                for (const auto& [k, v] : use["args"].items()) {
                    if (!params.count(k)) {
                        fail(coord + ".args", "unexpected arg '" + k
                             + "' (template '" + tname + "' declares: "
                             + (params.empty() ? "none" : "") + ")");
                    }
                    args[k] = v;
                }
            }
            for (const auto& p : params) {
                if (!args.count(p)) {
                    fail(coord + ".args", "missing arg '" + p
                         + "' required by template '" + tname + "'");
                }
            }
            const Subst param_pass{args, "$param", "@{", /*foreign_ok=*/true};

            // Local node set decides which references get prefixed.
            std::set<std::string> locals;
            if (tmpl.contains("nodes")) {
                for (const auto& [n, v] : tmpl["nodes"].items()) {
                    (void)v;
                    locals.insert(n);
                }
            }
            const Prefixer px{locals, prefix};
            const std::string src_tag =
                coord + " template '" + tname + "' prefix '" + prefix + "'";

            // nodes
            if (tmpl.contains("nodes")) {
                if (!core.contains("nodes")) core["nodes"] = json::object();
                json merged = core["nodes"];
                for (const auto& [n, nd] : tmpl["nodes"].items()) {
                    const std::string out_name = px.map_name(n);
                    if (merged.contains(out_name)) {
                        fail(coord, "node '" + out_name
                             + "' collides with an existing node");
                    }
                    json body = param_pass.apply(nd, coord + ".nodes." + n);
                    // barrier wait_for refers to node names — prefix locals.
                    if (body.contains("barrier") && body["barrier"].is_object()
                        && body["barrier"].contains("wait_for")) {
                        json wf = json::array();
                        for (const auto& u : body["barrier"]["wait_for"]) {
                            wf.push_back(px.map_name(u.get<std::string>()));
                        }
                        json b = body["barrier"];
                        b["wait_for"] = std::move(wf);
                        body["barrier"] = std::move(b);
                    }
                    merged[out_name] = std::move(body);
                    map_entry("/nodes/" + out_name, src_tag);
                }
                core["nodes"] = std::move(merged);
            }
            // channels: global shared state — merged, never prefixed.
            if (tmpl.contains("channels")) {
                json merged = core.contains("channels") ? core["channels"]
                                                        : json::object();
                for (const auto& [cn, cd] : tmpl["channels"].items()) {
                    json out_cd = param_pass.apply(cd, coord + ".channels." + cn);
                    if (merged.contains(cn) && merged[cn] != out_cd) {
                        fail(coord, "channel '" + cn
                             + "' conflicts with an existing, different definition");
                    }
                    if (!merged.contains(cn)) map_entry("/channels/" + cn, src_tag);
                    merged[cn] = std::move(out_cd);
                }
                core["channels"] = std::move(merged);
            }
            // edges / conditional_edges: appended with local refs prefixed.
            auto expand_edges = [&](const char* key) {
                if (!tmpl.contains(key)) return;
                json arr = core.contains(key) ? core[key] : json::array();
                size_t ei = 0;
                for (const auto& e : tmpl[key]) {
                    json out = param_pass.apply(
                        e, coord + "." + key + "[" + std::to_string(ei++) + "]");
                    if (out.contains("from") && out["from"].is_string()) {
                        out["from"] = json(px.map_name(out["from"].get<std::string>()));
                    }
                    if (out.contains("to") && out["to"].is_string()) {
                        out["to"] = json(px.map_name(out["to"].get<std::string>()));
                    }
                    if (out.contains("routes") && out["routes"].is_object()) {
                        json routes = json::object();
                        for (const auto& [rk, rt] : out["routes"].items()) {
                            routes[rk] = rt.is_string()
                                ? json(px.map_name(rt.get<std::string>())) : rt;
                        }
                        out["routes"] = std::move(routes);
                    }
                    map_entry(std::string("/") + key + "/"
                              + std::to_string(arr.size()), src_tag);
                    arr.push_back(std::move(out));
                }
                core[key] = std::move(arr);
            };
            expand_edges("edges");
            expand_edges("conditional_edges");
        }
    }

    // ---- global var pass over the merged document --------------------------
    core = var_pass.apply(core, "$");

    // ---- leftover-$param guard ---------------------------------------------
    // A "$param" outside any template body is a user mistake that would
    // otherwise flow into the compiler as a weird object.
    {
        struct Guard {
            static void check(const json& v, const std::string& path) {
                std::string name;
                if (is_ref(v, "$param", name)) {
                    fail(path, "'$param' reference '" + name
                         + "' outside a template body");
                }
                if (v.is_object()) {
                    for (const auto& [k, val] : v.items()) {
                        if (!is_annotation_key(k)) check(val, path + "." + k);
                    }
                } else if (v.is_array()) {
                    size_t i = 0;
                    for (const auto& e : v) {
                        check(e, path + "[" + std::to_string(i++) + "]");
                    }
                }
            }
        };
        Guard::check(core, "$");
    }

    ElaborationResult result;
    result.core = std::move(core);
    result.sourcemap = std::move(sourcemap);
    (void)has_dsl;
    return result;
}

} // namespace neograph::graph
