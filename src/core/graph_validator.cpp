#include <neograph/graph/loader.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/validator.h>

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>

namespace neograph::graph {

namespace {

constexpr const char* kStart = "__start__";
constexpr const char* kEnd   = "__end__";

json string_set_to_array(const std::set<std::string>& s) {
    json arr = json::array();
    for (const auto& v : s) arr.push_back(v);
    return arr;
}

struct Ctx {
    const TopologySpec& cg;
    const GraphRegistry&  registry;
    std::set<std::string> node_names;
    // Static successor map: plain edges + every conditional route
    // target. The scheduler's fallback route is always one of the
    // declared routes, so this over-approximates nothing.
    std::map<std::string, std::set<std::string>> succ;
    std::vector<Diagnostic> out;

    void error(std::string code, std::string path, std::string msg, json witness) {
        out.push_back({std::move(code), "error", std::move(path),
                       std::move(msg), std::move(witness)});
    }
    void warn(std::string code, std::string path, std::string msg, json witness) {
        out.push_back({std::move(code), "warning", std::move(path),
                       std::move(msg), std::move(witness)});
    }

    bool is_node(const std::string& n) const { return node_names.count(n) > 0; }

    std::string node_type(const std::string& n) const {
        auto it = cg.node_defs.find(n);
        if (it == cg.node_defs.end()) return "";
        return it->second.value("type", "");
    }
};

// ---- E3: dangling references --------------------------------------------

void check_references(Ctx& c) {
    size_t i = 0;
    for (const auto& e : c.cg.edges) {
        const std::string path = "edges[" + std::to_string(i++) + "]";
        if (e.from != kStart && !c.is_node(e.from)) {
            c.error("E3", path, "edge 'from' references unknown node '" + e.from + "'",
                    json{{"edge_from", e.from}, {"edge_to", e.to}});
        }
        if (e.to != kEnd && !c.is_node(e.to)) {
            c.error("E3", path, "edge 'to' references unknown node '" + e.to + "'",
                    json{{"edge_from", e.from}, {"edge_to", e.to}});
        }
    }
    i = 0;
    for (const auto& ce : c.cg.conditional_edges) {
        const std::string path = "conditional_edges[" + std::to_string(i++) + "]";
        if (!c.is_node(ce.from)) {
            c.error("E3", path, "conditional edge 'from' references unknown node '"
                    + ce.from + "'", json{{"from", ce.from}});
        }
        for (const auto& [key, target] : ce.routes) {
            if (target != kEnd && !c.is_node(target)) {
                c.error("E3", path + ".routes." + key,
                        "route '" + key + "' targets unknown node '" + target + "'",
                        json{{"from", ce.from}, {"route", key}, {"target", target}});
            }
        }
    }
    auto check_interrupts = [&](const char* which, const std::set<std::string>& s) {
        for (const auto& n : s) {
            if (!c.is_node(n)) {
                c.error("E3", which, std::string(which)
                        + " references unknown node '" + n + "'",
                        json{{"node", n}});
            }
        }
    };
    check_interrupts("interrupt_before", c.cg.interrupt_before);
    check_interrupts("interrupt_after", c.cg.interrupt_after);

    for (const auto& [bnode, wait_for] : c.cg.barrier_specs) {
        for (const auto& u : wait_for) {
            const std::string path = "nodes." + bnode + ".barrier";
            if (u == bnode) {
                c.error("E3", path, "barrier waits for the node itself ('"
                        + u + "') — it can never receive its own signal before firing",
                        json{{"barrier", bnode}, {"waits_for", u}});
            } else if (!c.is_node(u)) {
                c.error("E3", path, "barrier wait_for references unknown node '"
                        + u + "'", json{{"barrier", bnode}, {"waits_for", u}});
            }
        }
    }
}

// ---- successor map (shared by E7/E8/E11) --------------------------------

void build_successors(Ctx& c) {
    for (const auto& e : c.cg.edges) c.succ[e.from].insert(e.to);
    for (const auto& ce : c.cg.conditional_edges) {
        for (const auto& [key, target] : ce.routes) {
            (void)key;
            c.succ[ce.from].insert(target);
        }
    }
}

// ---- E7: reachability from __start__ -------------------------------------

std::set<std::string> reachable_from_start(const Ctx& c) {
    std::set<std::string> seen;
    std::queue<std::string> q;
    q.push(kStart);
    seen.insert(kStart);
    while (!q.empty()) {
        auto cur = q.front(); q.pop();
        auto it = c.succ.find(cur);
        if (it == c.succ.end()) continue;
        for (const auto& nxt : it->second) {
            if (seen.insert(nxt).second) q.push(nxt);
        }
    }
    return seen;
}

void check_reachability(Ctx& c, const std::set<std::string>& reachable) {
    std::set<std::string> unreachable;
    for (const auto& n : c.node_names) {
        if (!reachable.count(n)) unreachable.insert(n);
    }
    if (!unreachable.empty()) {
        c.warn("E7", "$",
               "node(s) unreachable from __start__ via static edges/routes: "
               + string_set_to_array(unreachable).dump()
               + " (only Command.goto / Send can reach them at runtime — "
               "if none of your nodes emits those, this is dead topology)",
               json{{"unreachable", string_set_to_array(unreachable)}});
    }
}

// ---- E11: no path to __end__ ---------------------------------------------

void check_termination(Ctx& c, const std::set<std::string>& reachable) {
    // Forward closure under the scheduler's implicit rule: a node with
    // no outgoing edges at all routes to __end__ — so only genuinely
    // trapped cycles fail this.
    std::set<std::string> can_end;
    bool grew = true;
    auto reaches_end_directly = [&](const std::string& n) {
        auto it = c.succ.find(n);
        if (it == c.succ.end() || it->second.empty()) return true;  // implicit __end__
        return it->second.count(kEnd) > 0;
    };
    while (grew) {
        grew = false;
        for (const auto& n : c.node_names) {
            if (can_end.count(n)) continue;
            bool ok = reaches_end_directly(n);
            if (!ok) {
                auto it = c.succ.find(n);
                for (const auto& s : it->second) {
                    if (can_end.count(s)) { ok = true; break; }
                }
            }
            if (ok) { can_end.insert(n); grew = true; }
        }
    }
    std::set<std::string> trapped;
    for (const auto& n : c.node_names) {
        if (reachable.count(n) && !can_end.count(n)) trapped.insert(n);
    }
    if (!trapped.empty()) {
        c.warn("E11", "$",
               "node(s) with no static path to __end__ (cycle without exit): "
               + string_set_to_array(trapped).dump()
               + " (a Command.goto could still exit at runtime; otherwise "
               "the run only stops at the recursion limit)",
               json{{"trapped", string_set_to_array(trapped)}});
    }
}

// ---- E8: barrier liveness -------------------------------------------------

void check_barriers(Ctx& c) {
    for (const auto& [bnode, wait_for] : c.cg.barrier_specs) {
        for (const auto& u : wait_for) {
            if (u == bnode || !c.is_node(u)) continue;   // reported as E3
            auto it = c.succ.find(u);
            const bool signals = it != c.succ.end() && it->second.count(bnode) > 0;
            if (!signals) {
                c.error("E8", "nodes." + bnode + ".barrier",
                        "barrier waits for '" + u + "', but '" + u
                        + "' has no edge or conditional route into '" + bnode
                        + "' — the AND-join can never be satisfied "
                        "(Command.goto bypasses barrier accounting, so no "
                        "dynamic mechanism can rescue this)",
                        json{{"barrier", bnode}, {"waits_for", u}});
            }
        }
    }
}

// ---- E9: unsynchronized plain fan-in --------------------------------------

void check_fan_in(Ctx& c) {
    std::map<std::string, std::set<std::string>> plain_in;
    for (const auto& e : c.cg.edges) {
        if (e.from != kStart && e.to != kEnd) plain_in[e.to].insert(e.from);
    }
    for (const auto& [node, sources] : plain_in) {
        if (sources.size() < 2 || c.cg.barrier_specs.count(node)) continue;
        c.warn("E9", "nodes." + node,
               "'" + node + "' has " + std::to_string(sources.size())
               + " plain in-edges and no barrier — if those sources can be "
               "concurrently active (AND fan-out upstream), it fires once "
               "per arrival super-step instead of once; declare "
               "\"barrier\": {\"wait_for\": [...]} for AND-join, or ignore "
               "if the sources are mutually exclusive (XOR merge)",
               json{{"node", node}, {"sources", string_set_to_array(sources)}});
    }
}

// ---- E10: route completeness ----------------------------------------------

void check_routes(Ctx& c) {
    size_t i = 0;
    for (const auto& ce : c.cg.conditional_edges) {
        const std::string path = "conditional_edges[" + std::to_string(i++) + "]";

        if (ce.routes.empty()) {
            c.error("E10", path,
                    "conditional edge from '" + ce.from + "' has no routes — "
                    "dispatch would dereference an empty route map (UB)",
                    json{{"from", ce.from}, {"condition", ce.condition}});
            continue;
        }

        auto spec = c.registry.condition_spec(ce.condition);
        if (!spec) continue;   // no declared contract — skip

        const std::set<std::string> labels(spec->labels.begin(), spec->labels.end());
        std::set<std::string> keys;
        for (const auto& [k, v] : ce.routes) { (void)v; keys.insert(k); }

        std::set<std::string> dead, uncovered;
        for (const auto& k : keys)   if (!labels.count(k)) dead.insert(k);
        for (const auto& l : labels) if (!keys.count(l))   uncovered.insert(l);

        if (!spec->open && !dead.empty()) {
            c.error("E10", path,
                    "route key(s) " + string_set_to_array(dead).dump()
                    + " can never be produced by condition '" + ce.condition
                    + "' (declared labels: " + string_set_to_array(labels).dump()
                    + ") — dead route",
                    json{{"from", ce.from}, {"condition", ce.condition},
                         {"dead_keys", string_set_to_array(dead)}});
        }
        if (!uncovered.empty()) {
            const std::string fallback = ce.routes.rbegin()->first;
            const std::string msg =
                "label(s) " + string_set_to_array(uncovered).dump()
                + " of condition '" + ce.condition + "' have no route — the "
                "scheduler falls back to the lexicographically-last route ('"
                + fallback + "' -> '" + ce.routes.rbegin()->second
                + "'), which is order-dependent, not intent";
            json witness = json{{"from", ce.from}, {"condition", ce.condition},
                                {"uncovered", string_set_to_array(uncovered)},
                                {"fallback_route", fallback}};
            if (spec->open) {
                c.warn("E10", path, msg, std::move(witness));
            } else {
                c.error("E10", path, msg, std::move(witness));
            }
        }
    }
}

// ---- E4/E5/E6: channel effects ---------------------------------------------

void check_effects(Ctx& c) {
    // Gate: every node's type must declare effects, else skip the
    // whole family (an unknown type could touch anything).
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>>
        node_rw;   // node -> (reads, writes)
    for (const auto& n : c.node_names) {
        const json eff = c.registry.node_effects(c.node_type(n));
        if (eff.is_null() || !eff.is_object()) return;   // gate: skip family
        std::set<std::string> reads, writes;
        if (eff.contains("reads"))
            for (const auto& ch : eff["reads"]) reads.insert(ch.get<std::string>());
        if (eff.contains("writes"))
            for (const auto& ch : eff["writes"]) writes.insert(ch.get<std::string>());
        node_rw[n] = {std::move(reads), std::move(writes)};
    }

    std::set<std::string> declared;
    std::map<std::string, ReducerType> reducer_of;
    std::set<std::string> has_initial;
    for (const auto& cd : c.cg.channel_defs) {
        declared.insert(cd.name);
        reducer_of[cd.name] = cd.type;
        if (cd.has_initial) has_initial.insert(cd.name);
    }

    std::map<std::string, std::set<std::string>> readers, writers;
    for (const auto& [n, rw] : node_rw) {
        for (const auto& ch : rw.first) {
            readers[ch].insert(n);
            if (!declared.count(ch)) {
                c.warn("E4", "nodes." + n,
                       "'" + n + "' (type " + c.node_type(n) + ") reads channel '"
                       + ch + "' which is not declared — the read silently "
                       "yields null",
                       json{{"node", n}, {"channel", ch}});
            }
        }
        for (const auto& ch : rw.second) {
            writers[ch].insert(n);
            if (!declared.count(ch)) {
                c.error("E4", "nodes." + n,
                        "'" + n + "' (type " + c.node_type(n) + ") writes channel '"
                        + ch + "' which is not declared — the engine throws "
                        "\"Write to unknown channel\" on every execution of "
                        "this node; declare it under \"channels\"",
                        json{{"node", n}, {"channel", ch}});
            }
        }
    }

    // E6: dead / write-only / initial-less read-only channels.
    for (const auto& ch : declared) {
        const bool r = readers.count(ch) && !readers[ch].empty();
        const bool w = writers.count(ch) && !writers[ch].empty();
        // Conditions read channels too (route_channel reads __route__)
        // but have no effect contracts — treat __-prefixed engine
        // channels as read by the routing layer.
        const bool engine_channel = ch.rfind("__", 0) == 0;
        if (!r && !w && !engine_channel) {
            c.warn("E6", "channels." + ch,
                   "channel '" + ch + "' is declared but no node reads or "
                   "writes it (dead channel)",
                   json{{"channel", ch}});
        } else if (!r && w && !engine_channel) {
            c.warn("E6", "channels." + ch,
                   "channel '" + ch + "' is written but never read",
                   json{{"channel", ch},
                        {"writers", string_set_to_array(writers[ch])}});
        } else if (r && !w && !has_initial.count(ch)) {
            c.warn("E6", "channels." + ch,
                   "channel '" + ch + "' is read but never written and has "
                   "no initial value — every read yields null",
                   json{{"channel", ch},
                        {"readers", string_set_to_array(readers[ch])}});
        }
    }

    // E5: overwrite race between direct fan-out siblings. Direct plain
    // fan-out from a common source is the one place static analysis
    // knows two nodes run in the same super-step; deeper concurrency
    // is left to the runtime backstop (under-approximation, no false
    // positives on sequential graphs).
    std::map<std::string, std::set<std::string>> plain_out;
    for (const auto& e : c.cg.edges) {
        if (e.to != kEnd) plain_out[e.from].insert(e.to);
    }
    std::set<std::string> reported;
    for (const auto& [src, targets] : plain_out) {
        if (targets.size() < 2) continue;
        std::map<std::string, std::set<std::string>> channel_writers;
        for (const auto& t : targets) {
            auto it = node_rw.find(t);
            if (it == node_rw.end()) continue;
            for (const auto& ch : it->second.second) {
                if (reducer_of.count(ch) && reducer_of[ch] == ReducerType::OVERWRITE) {
                    channel_writers[ch].insert(t);
                }
            }
        }
        for (const auto& [ch, ws] : channel_writers) {
            if (ws.size() < 2) continue;
            const std::string key = src + "/" + ch;
            if (!reported.insert(key).second) continue;
            c.warn("E5", "channels." + ch,
                   "overwrite channel '" + ch + "' is written by "
                   + std::to_string(ws.size()) + " concurrently-active nodes "
                   + string_set_to_array(ws).dump() + " (fan-out siblings of '"
                   + src + "') — last-writer-wins is nondeterministic under "
                   "parallel dispatch; use an append reducer or serialize "
                   "the writers",
                   json{{"channel", ch}, {"fan_out_source", src},
                        {"writers", string_set_to_array(ws)}});
        }
    }
}

} // namespace

// ---- report helpers ---------------------------------------------------------

bool ValidationReport::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == "error"; });
}

std::vector<const Diagnostic*> ValidationReport::errors() const {
    std::vector<const Diagnostic*> v;
    for (const auto& d : diagnostics) if (d.severity == "error") v.push_back(&d);
    return v;
}

std::vector<const Diagnostic*> ValidationReport::warnings() const {
    std::vector<const Diagnostic*> v;
    for (const auto& d : diagnostics) if (d.severity == "warning") v.push_back(&d);
    return v;
}

std::string ValidationReport::summary() const {
    std::string s;
    for (const auto& d : diagnostics) {
        if (!s.empty()) s += "\n";
        s += "  [" + d.code + "/" + d.severity + "] " + d.path + ": " + d.message;
    }
    return s;
}

ValidationReport GraphValidator::validate(const CompiledGraph& cg) {
    return validate(cg.topology(), GraphRegistry::global());
}

ValidationReport GraphValidator::validate(const CompiledGraph& cg, const GraphRegistry& registry) {
    return validate(cg.topology(), registry);
}

ValidationReport GraphValidator::validate(const TopologySpec& topology) {
    return validate(topology, GraphRegistry::global());
}

ValidationReport GraphValidator::validate(const TopologySpec& topology,
                                          const GraphRegistry& registry) {
    Ctx c{topology, registry, {}, {}, {}};
    for (const auto& [name, node_def] : topology.node_defs) {
        (void)node_def;
        c.node_names.insert(name);
    }

    check_references(c);
    build_successors(c);
    const auto reachable = reachable_from_start(c);
    check_reachability(c, reachable);
    check_termination(c, reachable);
    check_barriers(c);
    check_fan_in(c);
    check_routes(c);
    check_effects(c);

    return ValidationReport{std::move(c.out)};
}

ValidatedTopology GraphValidator::require_valid(TopologySpec topology) {
    return require_valid(std::move(topology), GraphRegistry::global());
}

ValidatedTopology GraphValidator::require_valid(TopologySpec topology,
                                                 const GraphRegistry& registry) {
    auto report = validate(topology, registry);
    if (report.has_errors()) {
        throw std::runtime_error(
            "graph validation failed (schema_version "
            + std::to_string(topology.schema_version) + "):\n"
            + report.summary());
    }
    return ValidatedTopology(std::move(topology), std::move(report));
}

} // namespace neograph::graph
