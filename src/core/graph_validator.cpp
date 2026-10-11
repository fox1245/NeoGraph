#include <neograph/graph/loader.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/validator.h>

#include "graph_v2_keys.h"
#include "routing_policy.h"

#include <algorithm>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>

namespace neograph::graph {

namespace {

constexpr const char* kStart = "__start__";
constexpr const char* kEnd   = "__end__";

json string_set_to_array(const std::set<std::string>& s) {
    json arr = json::array();
    for (const auto& v : s)
        arr.push_back(v);
    return arr;
}

json string_vector_to_array(const std::vector<std::string>& v) {
    json arr = json::array();
    for (const auto& item : v)
        arr.push_back(item);
    return arr;
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

struct Ctx {
    const TopologySpec&   cg;
    const GraphRegistry&  registry;
    bool                  local_only = false;
    std::set<std::string> node_names;
    // Successor map shared by E7/E8/E11: the routes the scheduler takes, or
    // under the original rules every declared edge and route target.
    std::map<std::string, std::set<std::string>> succ;
    std::vector<Diagnostic>                      out;

    void error(
        std::string code, std::string path, std::string pointer, std::string msg, json witness) {
        out.push_back({std::move(code), "error", std::move(path), std::move(msg),
                       std::move(witness), std::move(pointer)});
    }
    void warn(
        std::string code, std::string path, std::string pointer, std::string msg, json witness) {
        out.push_back({std::move(code), "warning", std::move(path), std::move(msg),
                       std::move(witness), std::move(pointer)});
    }

    bool original_rules = false;

    bool is_node(const std::string& n) const { return node_names.count(n) > 0; }

    // The conditional edge that routes a source: the scheduler consults only
    // the first one, and then never fires the source's static edges.
    const ConditionalEdge* routing_edge(const std::string& source) const {
        for (const auto& ce : cg.conditional_edges)
            if (ce.from == source) return &ce;
        return nullptr;
    }

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
        const std::string path    = "edges[" + std::to_string(i) + "]";
        const std::string pointer = "/edges/" + std::to_string(i++);
        if (e.from != kStart && !c.is_node(e.from)) {
            c.error("E3", path, pointer, "edge 'from' references unknown node '" + e.from + "'",
                    json{{"edge_from", e.from}, {"edge_to", e.to}, {"unknown", e.from}});
        }
        if (e.to != kEnd && !c.is_node(e.to)) {
            c.error("E3", path, pointer, "edge 'to' references unknown node '" + e.to + "'",
                    json{{"edge_from", e.from}, {"edge_to", e.to}, {"unknown", e.to}});
        }
    }
    i = 0;
    for (const auto& ce : c.cg.conditional_edges) {
        const std::string path    = "conditional_edges[" + std::to_string(i) + "]";
        const std::string pointer = "/conditional_edges/" + std::to_string(i++);
        if (!c.is_node(ce.from)) {
            c.error("E3", path, pointer,
                    "conditional edge 'from' references unknown node '" + ce.from + "'",
                    json{{"from", ce.from}, {"unknown", ce.from}});
        }
        for (const auto& [key, target] : ce.routes) {
            if (key == detail::kLegacyDefaultRoute) continue;
            if (target != kEnd && !c.is_node(target)) {
                c.error("E3", path + ".routes." + key, pointer + "/routes/" + pointer_token(key),
                        "route '" + key + "' targets unknown node '" + target + "'",
                        json{{"from", ce.from},
                             {"route", key},
                             {"target", target},
                             {"unknown", target}});
            }
        }
    }
    auto check_interrupts = [&](const char* which, const std::set<std::string>& s) {
        for (const auto& n : s) {
            if (!c.is_node(n)) {
                c.error("E3", which, "/" + std::string(which),
                        std::string(which) + " references unknown node '" + n + "'",
                        json{{"node", n}, {"unknown", n}});
            }
        }
    };
    check_interrupts("interrupt_before", c.cg.interrupt_before);
    check_interrupts("interrupt_after", c.cg.interrupt_after);

    for (const auto& [bnode, join] : c.cg.join_specs) {
        const std::string path    = "nodes." + bnode + ".barrier";
        const std::string pointer = "/nodes/" + pointer_token(bnode) + "/barrier";
        for (const auto& slot : join.slots) {
            for (const auto& u : slot) {
                if (u == bnode) {
                    c.error("E3", path, pointer,
                            "barrier slot " + string_set_to_array(slot).dump() +
                                " lists the join itself ('" + u +
                                "') — it can never receive its own signal before firing",
                            json{{"barrier", bnode},
                                 {"waits_for", u},
                                 {"slot", string_set_to_array(slot)}});
                } else if (!c.is_node(u)) {
                    c.error("E3", path, pointer,
                            "barrier slot references unknown node '" + u + "'",
                            json{{"barrier", bnode},
                                 {"waits_for", u},
                                 {"slot", string_set_to_array(slot)},
                                 {"unknown", u}});
                }
            }
        }
    }

    for (const auto& [bnode, wait_for] : c.cg.barrier_specs) {
        for (const auto& u : wait_for) {
            const std::string path    = "nodes." + bnode + ".barrier";
            const std::string pointer = "/nodes/" + pointer_token(bnode) + "/barrier";
            if (u == bnode) {
                c.error("E3", path, pointer,
                        "barrier waits for the node itself ('" + u +
                            "') — it can never receive its own signal before firing",
                        json{{"barrier", bnode}, {"waits_for", u}});
            } else if (!c.is_node(u)) {
                c.error("E3", path, pointer, "barrier wait_for references unknown node '" + u + "'",
                        json{{"barrier", bnode}, {"waits_for", u}, {"unknown", u}});
            }
        }
    }
}

// ---- successor map (shared by E7/E8/E11) --------------------------------

void build_successors(Ctx& c) {
    if (c.original_rules) {
        for (const auto& e : c.cg.edges)
            c.succ[e.from].insert(e.to);
        for (const auto& ce : c.cg.conditional_edges) {
            for (const auto& [key, target] : ce.routes) {
                if (key == detail::kLegacyDefaultRoute) continue;
                c.succ[ce.from].insert(target);
            }
        }
        return;
    }
    // Route the way the scheduler does: a source with a conditional edge goes
    // only where its first conditional edge sends it (a schema-v0 legacy
    // default included); any other source fires every static edge. __start__
    // is entered through static edges only.
    std::set<std::string> routed;
    for (const auto& ce : c.cg.conditional_edges) {
        if (ce.from == kStart || !routed.insert(ce.from).second) continue;
        auto& successors = c.succ[ce.from];
        for (const auto& [key, target] : ce.routes) {
            (void)key;
            successors.insert(target);
        }
    }
    for (const auto& e : c.cg.edges)
        if (!routed.count(e.from)) c.succ[e.from].insert(e.to);
}

// ---- E7: reachability from __start__ -------------------------------------

std::set<std::string> reachable_from_start(const Ctx& c) {
    std::set<std::string>   seen;
    std::queue<std::string> q;
    q.push(kStart);
    seen.insert(kStart);
    while (!q.empty()) {
        auto cur = q.front();
        q.pop();
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
        c.warn("E7", "$", "",
               "node(s) unreachable from __start__ via static edges/routes: " +
                   string_set_to_array(unreachable).dump() +
                   " (only Command.goto / Send can reach them at runtime — "
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
    bool                  grew                 = true;
    auto                  reaches_end_directly = [&](const std::string& n) {
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
                    if (can_end.count(s)) {
                        ok = true;
                        break;
                    }
                }
            }
            if (ok) {
                can_end.insert(n);
                grew = true;
            }
        }
    }
    std::set<std::string> trapped;
    for (const auto& n : c.node_names) {
        if (reachable.count(n) && !can_end.count(n)) trapped.insert(n);
    }
    if (!trapped.empty()) {
        c.warn("E11", "$", "",
               "node(s) with no static path to __end__ (cycle without exit): " +
                   string_set_to_array(trapped).dump() +
                   " (a Command.goto could still exit at runtime; otherwise "
                   "the run only stops at the recursion limit)",
               json{{"trapped", string_set_to_array(trapped)}});
    }
}

// ---- E8: barrier liveness -------------------------------------------------

void check_barriers(Ctx& c) {
    for (const auto& [bnode, wait_for] : c.cg.barrier_specs) {
        for (const auto& u : wait_for) {
            if (u == bnode || !c.is_node(u)) continue;  // reported as E3
            auto       it      = c.succ.find(u);
            const bool signals = it != c.succ.end() && it->second.count(bnode) > 0;
            if (!signals) {
                c.error("E8", "nodes." + bnode + ".barrier",
                        "/nodes/" + pointer_token(bnode) + "/barrier",
                        "barrier waits for '" + u + "', but '" + u +
                            "' has no edge or conditional route into '" + bnode +
                            "' — the AND-join can never be satisfied "
                            "(Command.goto bypasses barrier accounting, so no "
                            "dynamic mechanism can rescue this)",
                        json{{"barrier", bnode}, {"waits_for", u}});
            }
        }
    }
    // #363: a slot is live when at least one alternative routes into the join.
    for (const auto& [bnode, join] : c.cg.join_specs) {
        for (const auto& slot : join.slots) {
            bool known  = false;
            bool signal = false;
            for (const auto& u : slot) {
                if (u == bnode || !c.is_node(u)) continue;  // reported as E3
                known       = true;
                const auto it = c.succ.find(u);
                signal = signal || (it != c.succ.end() && it->second.count(bnode) > 0);
            }
            if (!known || signal) continue;
            c.error("E8", "nodes." + bnode + ".barrier", "/nodes/" + pointer_token(bnode) + "/barrier",
                    "barrier waits for any of " + string_set_to_array(slot).dump() +
                        ", but none of them has an edge or conditional route into '" + bnode +
                        "' — the join can never be satisfied",
                    json{{"barrier", bnode}, {"slot", string_set_to_array(slot)}});
        }
    }
}

// ---- E13/E14: transitions the scheduler never takes (issue #358) ----------
//
// Errors for strict documents (schema_version >= 1); legacy documents warn.

void check_shadowed_transitions(Ctx& c) {
    const bool strict = c.cg.schema_version >= 1;
    auto       report = [&](std::string code, std::string path, std::string pointer,
                      std::string message, json witness) {
        if (strict)
            c.error(std::move(code), std::move(path), std::move(pointer), std::move(message),
                    std::move(witness));
        else
            c.warn(std::move(code), std::move(path), std::move(pointer), std::move(message),
                   std::move(witness));
    };
    size_t i = 0;
    for (const auto& ce : c.cg.conditional_edges) {
        const std::string path    = "conditional_edges[" + std::to_string(i) + "]";
        const std::string pointer = "/conditional_edges/" + std::to_string(i++);
        const auto*       routing = c.routing_edge(ce.from);
        if (ce.from == kStart || routing == &ce) continue;
        report("E14", path, pointer,
               "'" + ce.from + "' already routes through condition '" + routing->condition +
                   "'; only a node's first conditional edge routes, so this one never fires",
               json{{"from", ce.from},
                    {"condition", ce.condition},
                    {"routing_condition", routing->condition}});
    }
    i = 0;
    for (const auto& e : c.cg.edges) {
        const std::string path    = "edges[" + std::to_string(i) + "]";
        const std::string pointer = "/edges/" + std::to_string(i++);
        const auto*       routing = e.from == kStart ? nullptr : c.routing_edge(e.from);
        if (!routing) continue;
        report("E13", path, pointer,
               "edge '" + e.from + "' -> '" + e.to + "' never fires: '" + e.from +
                   "' routes through its conditional edge (condition '" + routing->condition +
                   "'), which replaces its static edges",
               json{{"from", e.from}, {"to", e.to}, {"routing_condition", routing->condition}});
    }
}

// ---- E15: barrier member that can route around its join (issue #357) -----

// X2: the shortest routed path from `from` to `to`, or empty.
std::vector<std::string> shortest_path(const Ctx& c, const std::string& from, const std::string& to) {
    std::map<std::string, std::string> parent;
    std::queue<std::string>            q;
    q.push(from);
    parent[from] = from;
    while (!q.empty()) {
        const auto cur = q.front();
        q.pop();
        if (cur == to) break;
        const auto it = c.succ.find(cur);
        if (it == c.succ.end()) continue;
        for (const auto& next : it->second) {
            if (next == kEnd || parent.count(next)) continue;
            parent[next] = cur;
            q.push(next);
        }
    }
    if (!parent.count(to)) return {};
    std::vector<std::string> path{to};
    while (path.back() != from) path.push_back(parent[path.back()]);
    std::reverse(path.begin(), path.end());
    return path;
}

// X2: for each bypass route, the route sequence from the member through it:
// back into the join when it re-enters, otherwise just the first hop. When a
// sequence re-enters, the node before the join can stand in for the member,
// so the witness carries the computed any_of fix, unless that node already
// belongs to another slot.
void add_bypass_paths(const Ctx&                                c,
                      const std::string&                        bnode,
                      const std::string&                        member,
                      const json&                               bypass,
                      const std::vector<std::set<std::string>>& slots,
                      json&                                     witness) {
    json paths = json::object();
    std::optional<std::string> substitute;
    for (const auto& [label, target] : bypass.items()) {
        const auto name = target.get<std::string>();
        auto       tail = c.is_node(name) ? shortest_path(c, name, bnode) : std::vector<std::string>{};
        std::vector<std::string> path{member};
        if (tail.empty()) {
            path.push_back(name);
        } else {
            path.insert(path.end(), tail.begin(), tail.end());
            if (!substitute) substitute = path[path.size() - 2];
        }
        paths[label] = string_vector_to_array(path);
    }
    witness["paths"] = std::move(paths);
    if (!substitute || *substitute == member || *substitute == bnode) return;
    std::vector<std::set<std::string>> fixed = slots;
    for (auto& slot : fixed) {
        if (slot.count(*substitute)) return;  // already fills another slot
    }
    for (auto& slot : fixed)
        if (slot.count(member)) slot.insert(*substitute);
    witness["substitute"]         = *substitute;
    witness["suggested_wait_for"] = detail::print_wait_for(fixed);
}

void check_barrier_bypass(Ctx& c) {
    for (const auto& [bnode, wait_for] : c.cg.barrier_specs) {
        for (const auto& u : wait_for) {
            const auto* routing = c.is_node(u) ? c.routing_edge(u) : nullptr;
            if (!routing) continue;
            json bypass  = json::object();
            bool signals = false;
            for (const auto& [key, target] : routing->routes) {
                if (target == bnode) signals = true;
                if (target == bnode || target == kEnd) continue;
                bypass[key == detail::kLegacyDefaultRoute ? std::string(DEFAULT_ROUTE) : key] =
                    target;
            }
            if (!signals || bypass.empty()) continue;  // no signal at all is E8
            // Built before the witness takes `bypass`: argument order is unspecified.
            auto message = "barrier '" + bnode + "' waits for '" + u + "', but route(s) " +
                           bypass.dump() + " of '" + u + "' lead elsewhere; when one is taken '" +
                           bnode + "' never fires and the run ends as barrier-stalled";
            std::vector<std::set<std::string>> slots;
            for (const auto& member : wait_for) slots.push_back({member});
            json witness{{"barrier", bnode}, {"member", u}, {"routes", bypass}};
            add_bypass_paths(c, bnode, u, bypass, slots, witness);
            c.warn("E15", "nodes." + bnode + ".barrier",
                   "/nodes/" + pointer_token(bnode) + "/barrier", std::move(message),
                   std::move(witness));
        }
    }
    // #363: per slot. A route to another alternative of the same slot is not
    // a bypass: that alternative can still fill the slot.
    for (const auto& [bnode, join] : c.cg.join_specs) {
        for (const auto& slot : join.slots) {
            for (const auto& u : slot) {
                const auto* routing = c.is_node(u) ? c.routing_edge(u) : nullptr;
                if (!routing) continue;
                json bypass  = json::object();
                bool signals = false;
                for (const auto& [key, target] : routing->routes) {
                    if (target == bnode) signals = true;
                    if (target == bnode || target == kEnd || slot.count(target)) continue;
                    bypass[key == detail::kLegacyDefaultRoute ? std::string(DEFAULT_ROUTE) : key] =
                        target;
                }
                if (!signals || bypass.empty()) continue;
                auto message = "barrier '" + bnode + "' waits for any of " +
                               string_set_to_array(slot).dump() + ", but route(s) " +
                               bypass.dump() + " of '" + u +
                               "' lead elsewhere; when one is taken and no other alternative "
                               "arrives, '" +
                               bnode + "' never fires and the run ends as barrier-stalled";
                json witness{{"barrier", bnode},
                             {"member", u},
                             {"slot", string_set_to_array(slot)},
                             {"routes", bypass}};
                add_bypass_paths(c, bnode, u, bypass, join.slots, witness);
                c.warn("E15", "nodes." + bnode + ".barrier",
                       "/nodes/" + pointer_token(bnode) + "/barrier", std::move(message),
                       std::move(witness));
            }
        }
    }
}

// ---- E17: co-active alternatives of one any_of slot (#363) -----------------
//
// Two alternatives of one slot that can both run make the second arrival open
// a new round of the join that never completes, so the run ends
// barrier-stalled. Static fan-out is the one place that runs successors
// together: either one node fans out to two branches that reach different
// alternatives without passing the join, or an alternative fans out both
// into the join and on to another alternative.

std::vector<std::string> static_fan_out(const Ctx& c, const std::string& node) {
    std::vector<std::string> targets;
    if (c.routing_edge(node)) return targets;  // a routed node takes one route
    for (const auto& e : c.cg.edges)
        if (e.from == node && e.to != kEnd &&
            std::find(targets.begin(), targets.end(), e.to) == targets.end())
            targets.push_back(e.to);
    std::sort(targets.begin(), targets.end());
    return targets;
}

// Shortest routed paths from `from` to each node it reaches without passing `avoid`.
std::map<std::string, std::vector<std::string>> paths_avoiding(const Ctx&         c,
                                                               const std::string& from,
                                                               const std::string& avoid) {
    std::map<std::string, std::vector<std::string>> paths;
    if (from == avoid || !c.is_node(from)) return paths;
    paths[from] = {from};
    std::queue<std::string> q;
    q.push(from);
    while (!q.empty()) {
        const auto cur = q.front();
        q.pop();
        const auto it = c.succ.find(cur);
        if (it == c.succ.end()) continue;
        for (const auto& next : it->second) {
            if (next == avoid || next == kEnd || !c.is_node(next) || paths.count(next)) continue;
            auto path = paths[cur];
            path.push_back(next);
            paths.emplace(next, std::move(path));
            q.push(next);
        }
    }
    return paths;
}

void check_slot_exclusivity(Ctx& c) {
    for (const auto& [bnode, join] : c.cg.join_specs) {
        for (const auto& slot : join.slots) {
            if (slot.size() < 2) continue;
            std::optional<json> found;
            // A fan-out node whose branches reach two different alternatives.
            for (const auto& f : c.node_names) {
                if (found) break;
                const auto targets = static_fan_out(c, f);
                for (std::size_t i = 0; i < targets.size() && !found; ++i) {
                    const auto left = paths_avoiding(c, targets[i], bnode);
                    for (std::size_t j = i + 1; j < targets.size() && !found; ++j) {
                        const auto right = paths_avoiding(c, targets[j], bnode);
                        for (const auto& a : slot) {
                            if (found) break;
                            if (!left.count(a)) continue;
                            for (const auto& b : slot) {
                                if (a == b || !right.count(b)) continue;
                                auto first  = left.at(a);
                                auto second = right.at(b);
                                first.insert(first.begin(), f);
                                second.insert(second.begin(), f);
                                found = json{{"fan_out", f},
                                             {"alternatives", json::array({a, b})},
                                             {"paths", json::array({string_vector_to_array(first),
                                                                    string_vector_to_array(second)})}};
                                break;
                            }
                        }
                    }
                }
            }
            // An alternative that fans out into the join and on to another one.
            for (const auto& a : slot) {
                if (found) break;
                if (!c.is_node(a)) continue;
                const auto targets = static_fan_out(c, a);
                if (std::find(targets.begin(), targets.end(), bnode) == targets.end()) continue;
                for (const auto& t : targets) {
                    if (found || t == bnode) continue;
                    const auto onward = paths_avoiding(c, t, bnode);
                    for (const auto& b : slot) {
                        if (b == a || !onward.count(b)) continue;
                        auto second = onward.at(b);
                        second.insert(second.begin(), a);
                        found = json{{"fan_out", a},
                                     {"alternatives", json::array({a, b})},
                                     {"paths", json::array({json::array({a, bnode}),
                                                            string_vector_to_array(second)})}};
                        break;
                    }
                }
            }
            if (!found) continue;
            auto witness       = std::move(*found);
            const auto a       = witness["alternatives"][0].get<std::string>();
            const auto b       = witness["alternatives"][1].get<std::string>();
            const auto fan_out = witness["fan_out"].get<std::string>();
            witness["barrier"] = bnode;
            witness["slot"]    = string_set_to_array(slot);
            c.warn("E17", "nodes." + bnode + ".barrier", "/nodes/" + pointer_token(bnode) + "/barrier",
                   "slot alternatives should be route-exclusive: '" + a + "' and '" + b +
                       "' of barrier '" + bnode + "' can both run (fan-out at '" + fan_out +
                       "'); the second arrival opens a round that never completes and the run "
                       "ends barrier-stalled",
                   std::move(witness));
        }
    }
}

// ---- E16: cycles, for hosts that require a DAG ------------------------------

void check_acyclic(Ctx& c) {
    std::map<std::string, std::set<std::string>> in, out;
    for (const auto& n : c.node_names) {
        in[n];
        out[n];
    }
    for (const auto& [from, targets] : c.succ) {
        if (!c.is_node(from)) continue;
        for (const auto& to : targets) {
            if (!c.is_node(to)) continue;
            out[from].insert(to);
            in[to].insert(from);
        }
    }
    // Peel nodes with no remaining predecessor, then nodes with no remaining
    // successor; whatever is left lies on or between cycles.
    std::set<std::string> remaining = c.node_names;
    for (const bool forward : {true, false}) {
        auto& degree  = forward ? in : out;
        auto& reverse = forward ? out : in;
        std::vector<std::string> ready;
        for (const auto& n : remaining) {
            std::erase_if(degree[n], [&](const std::string& m) { return !remaining.count(m); });
            if (degree[n].empty()) ready.push_back(n);
        }
        while (!ready.empty()) {
            const auto n = ready.back();
            ready.pop_back();
            if (!remaining.erase(n)) continue;
            for (const auto& m : reverse[n]) {
                if (degree[m].erase(n) && degree[m].empty() && remaining.count(m))
                    ready.push_back(m);
            }
        }
    }
    if (remaining.empty()) return;
    c.error("E16", "$", "",
            "graph has a cycle through " + string_set_to_array(remaining).dump() +
                " but this compile requires an acyclic graph",
            json{{"nodes", string_set_to_array(remaining)}});
}

// ---- E9: unsynchronized plain fan-in --------------------------------------

void check_fan_in(Ctx& c) {
    std::map<std::string, std::set<std::string>> plain_in;
    for (const auto& e : c.cg.edges) {
        if (e.from != kStart && e.to != kEnd) plain_in[e.to].insert(e.from);
    }
    for (const auto& [node, sources] : plain_in) {
        if (sources.size() < 2 || c.cg.barrier_specs.count(node) || c.cg.join_specs.count(node))
            continue;
        c.warn("E9", "nodes." + node, "/nodes/" + pointer_token(node),
               "'" + node + "' has " + std::to_string(sources.size()) +
                   " plain in-edges and no barrier — if those sources can be "
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
        const std::string path    = "conditional_edges[" + std::to_string(i) + "]";
        const std::string pointer = "/conditional_edges/" + std::to_string(i++);

        if (ce.routes.empty()) {
            c.error("E10", path, pointer,
                    "conditional edge from '" + ce.from +
                        "' has no routes — "
                        "dispatch would dereference an empty route map (UB)",
                    json{{"from", ce.from}, {"condition", ce.condition}});
            continue;
        }

        const auto spec = c.local_only ? (c.registry.contains_condition(ce.condition)
                                              ? c.registry.local_condition_spec(ce.condition)
                                              : std::optional<ConditionSpec>{})
                                       : c.registry.condition_spec(ce.condition);
        if (!spec) continue;  // no declared local/overlay contract — skip

        const std::set<std::string> labels(spec->labels.begin(), spec->labels.end());
        std::set<std::string>       keys;
        for (const auto& [k, v] : ce.routes) {
            (void)v;
            if (k != detail::kLegacyDefaultRoute) keys.insert(k);
        }

        std::set<std::string> dead, uncovered;
        for (const auto& k : keys) {
            if (!labels.count(k) && k != DEFAULT_ROUTE) dead.insert(k);
        }
        for (const auto& l : labels)
            if (!keys.count(l)) uncovered.insert(l);

        if (!spec->open && !dead.empty()) {
            c.error("E10", path, pointer,
                    "route key(s) " + string_set_to_array(dead).dump() +
                        " can never be produced by condition '" + ce.condition +
                        "' (declared labels: " + string_set_to_array(labels).dump() +
                        ") — dead route",
                    json{{"from", ce.from},
                         {"condition", ce.condition},
                         {"dead_keys", string_set_to_array(dead)}});
        }
        const bool has_default = keys.count(DEFAULT_ROUTE) > 0;
        if (!uncovered.empty() && (!spec->open || !has_default)) {
            const std::string msg =
                "label(s) " + string_set_to_array(uncovered).dump() + " of condition '" +
                ce.condition + "' have no exact route — " +
                (spec->open ? "unknown labels will fail at runtime; add an explicit "
                              "'default' route to handle them"
                            : "closed conditions must route every declared label");
            json witness = json{{"from", ce.from},
                                {"condition", ce.condition},
                                {"uncovered", string_set_to_array(uncovered)},
                                {"has_default", has_default}};
            if (spec->open) {
                c.warn("E10", path, pointer, msg, std::move(witness));
            } else {
                c.error("E10", path, pointer, msg, std::move(witness));
            }
        }
    }
}

// ---- E4/E5/E6: channel effects ---------------------------------------------

void check_effects(Ctx& c) {
    // Gate: every node's type must declare effects, else skip the
    // whole family (an unknown type could touch anything).
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>>
                          node_rw;  // node -> (reads, writes)
    std::set<std::string> exported;
    bool                  undeclared = false;
    for (const auto& n : c.node_names) {
        const json eff = c.local_only ? (c.registry.contains_type(c.node_type(n))
                                             ? c.registry.local_node_effects(c.node_type(n))
                                             : json())
                                      : c.registry.node_effects(c.node_type(n));
        if (eff.is_null() || !eff.is_object()) return;  // gate: skip family
        // An effects object listing neither reads nor writes declares nothing
        // (bindings substitute {} for an undeclared node type). E4/E5 remain
        // per-node facts, but E6 can no longer prove a channel unused.
        if (!c.original_rules && !eff.contains("reads") && !eff.contains("writes"))
            undeclared = true;
        std::set<std::string> reads, writes;
        if (eff.contains("reads"))
            for (const auto& ch : eff["reads"])
                reads.insert(ch.get<std::string>());
        if (eff.contains("writes"))
            for (const auto& ch : eff["writes"])
                writes.insert(ch.get<std::string>());
        if (eff.contains("exports")) {
            for (const auto& ch : eff["exports"]) {
                const auto name = ch.get<std::string>();
                if (writes.count(name)) exported.insert(name);
            }
        }
        node_rw[n] = {std::move(reads), std::move(writes)};
    }

    std::set<std::string>              declared;
    std::map<std::string, ReducerType> reducer_of;
    std::set<std::string>              has_initial;
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
                c.warn("E4", "nodes." + n, "/nodes/" + pointer_token(n),
                       "'" + n + "' (type " + c.node_type(n) + ") reads channel '" + ch +
                           "' which is not declared — the read silently "
                           "yields null",
                       json{{"node", n}, {"channel", ch}});
            }
        }
        for (const auto& ch : rw.second) {
            writers[ch].insert(n);
            if (!declared.count(ch)) {
                c.error("E4", "nodes." + n, "/nodes/" + pointer_token(n),
                        "'" + n + "' (type " + c.node_type(n) + ") writes channel '" + ch +
                            "' which is not declared — the engine throws "
                            "\"Write to unknown channel\" on every execution of "
                            "this node; declare it under \"channels\"",
                        json{{"node", n}, {"channel", ch}});
            }
        }
    }

    // E6: dead / write-only / initial-less read-only channels.
    for (const auto& ch : undeclared ? std::set<std::string>{} : declared) {
        const bool r = readers.count(ch) && !readers[ch].empty();
        const bool w = writers.count(ch) && !writers[ch].empty();
        // Conditions read channels too (route_channel reads __route__)
        // but have no effect contracts — treat __-prefixed engine
        // channels as read by the routing layer.
        const bool engine_channel = ch.rfind("__", 0) == 0;
        if (!r && !w && !engine_channel) {
            c.warn("E6", "channels." + ch, "/channels/" + pointer_token(ch),
                   "channel '" + ch +
                       "' is declared but no node reads or "
                       "writes it (dead channel)",
                   json{{"channel", ch}});
        } else if (!r && w && !engine_channel && !exported.count(ch)) {
            c.warn("E6", "channels." + ch, "/channels/" + pointer_token(ch),
                   "channel '" + ch + "' is written but never read",
                   json{{"channel", ch}, {"writers", string_set_to_array(writers[ch])}});
        } else if (r && !w && !has_initial.count(ch)) {
            c.warn("E6", "channels." + ch, "/channels/" + pointer_token(ch),
                   "channel '" + ch +
                       "' is read but never written and has "
                       "no initial value — every read yields null",
                   json{{"channel", ch}, {"readers", string_set_to_array(readers[ch])}});
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
            c.warn("E5", "channels." + ch, "/channels/" + pointer_token(ch),
                   "overwrite channel '" + ch + "' is written by " + std::to_string(ws.size()) +
                       " concurrently-active nodes " + string_set_to_array(ws).dump() +
                       " (fan-out siblings of '" + src +
                       "') — last-writer-wins is nondeterministic under "
                       "parallel dispatch; use an append reducer or serialize "
                       "the writers",
                   json{{"channel", ch},
                        {"fan_out_source", src},
                        {"writers", string_set_to_array(ws)}});
        }
    }
}

}  // namespace

// ---- report helpers ---------------------------------------------------------

bool ValidationReport::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == "error"; });
}

std::vector<const Diagnostic*> ValidationReport::errors() const {
    std::vector<const Diagnostic*> v;
    for (const auto& d : diagnostics)
        if (d.severity == "error") v.push_back(&d);
    return v;
}

std::vector<const Diagnostic*> ValidationReport::warnings() const {
    std::vector<const Diagnostic*> v;
    for (const auto& d : diagnostics)
        if (d.severity == "warning") v.push_back(&d);
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

ValidationReport GraphValidator::validate_local(const CompiledGraph& cg,
                                                const GraphRegistry& registry) {
    return validate_local(cg.topology(), registry);
}

ValidationReport GraphValidator::validate(const TopologySpec& topology) {
    return validate(topology, GraphRegistry::global());
}

namespace {

std::vector<Diagnostic> run_checks(const TopologySpec&      topology,
                                   const GraphRegistry&     registry,
                                   bool                     local_only,
                                   const ValidationOptions& options) {
    Ctx c{topology, registry, local_only, {}, {}, {}};
    c.original_rules = options.original_rules;
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
    if (!c.original_rules) {
        check_shadowed_transitions(c);
        check_barrier_bypass(c);
        check_slot_exclusivity(c);
    }
    check_fan_in(c);
    check_routes(c);
    check_effects(c);
    if (options.require_acyclic) check_acyclic(c);
    return std::move(c.out);
}

}  // namespace

ValidationReport GraphValidator::validate(const TopologySpec&  topology,
                                          const GraphRegistry& registry) {
    return validate(topology, registry, ValidationOptions{});
}

std::map<std::string, std::set<std::string>> GraphValidator::routed_successors(
    const TopologySpec& topology) {
    static GraphRegistry unused_registry;
    Ctx c{topology, unused_registry, true, {}, {}, {}};
    for (const auto& [name, node_def] : topology.node_defs) {
        (void)node_def;
        c.node_names.insert(name);
    }
    build_successors(c);
    return std::move(c.succ);
}

ValidationReport GraphValidator::validate(const TopologySpec&      topology,
                                          const GraphRegistry&     registry,
                                          const ValidationOptions& options) {
    return ValidationReport{run_checks(topology, registry, false, options)};
}

ValidationReport GraphValidator::validate(const CompiledGraph&     cg,
                                          const GraphRegistry&     registry,
                                          const ValidationOptions& options) {
    return validate(cg.topology(), registry, options);
}

ValidationReport GraphValidator::validate_local(const TopologySpec&  topology,
                                                const GraphRegistry& registry) {
    return validate_local(topology, registry, ValidationOptions{});
}

ValidationReport GraphValidator::validate_local(const TopologySpec&      topology,
                                                const GraphRegistry&     registry,
                                                const ValidationOptions& options) {
    ValidationReport report{run_checks(topology, registry, true, options)};
    for (const auto& channel : topology.channel_defs) {
        if (!registry.contains_reducer(channel.reducer_name)) {
            report.diagnostics.push_back(
                {"E12", "error", "channels." + channel.name,
                 "Local graph registry has no reducer '" + channel.reducer_name + "'",
                 json{{"kind", "reducer"}, {"name", channel.reducer_name}},
                 "/channels/" + pointer_token(channel.name) + "/reducer"});
        }
    }
    for (const auto& [name, node_def] : topology.node_defs) {
        const std::string type =
            node_def.is_object() && node_def.contains("type") && node_def["type"].is_string()
                ? node_def["type"].get<std::string>()
                : std::string();
        if (!registry.contains_type(type)) {
            report.diagnostics.push_back({"E12", "error", "nodes." + name,
                                          "Local graph registry has no node type '" + type + "'",
                                          json{{"kind", "node"}, {"name", type}},
                                          "/nodes/" + pointer_token(name) + "/type"});
        }
    }
    std::size_t index = 0;
    for (const auto& edge : topology.conditional_edges) {
        if (!registry.contains_condition(edge.condition)) {
            report.diagnostics.push_back(
                {"E12", "error", "conditional_edges[" + std::to_string(index) + "]",
                 "Local graph registry has no condition '" + edge.condition + "'",
                 json{{"kind", "condition"}, {"name", edge.condition}},
                 "/conditional_edges/" + std::to_string(index) + "/condition"});
        }
        ++index;
    }
    return report;
}

ValidatedTopology GraphValidator::require_valid(TopologySpec topology) {
    return require_valid(std::move(topology), GraphRegistry::global());
}

ValidatedTopology GraphValidator::require_valid(TopologySpec         topology,
                                                const GraphRegistry& registry) {
    auto report = validate(topology, registry);
    if (report.has_errors()) {
        throw std::runtime_error("graph validation failed (schema_version " +
                                 std::to_string(topology.schema_version) + "):\n" +
                                 report.summary());
    }
    return ValidatedTopology(std::move(topology), std::move(report));
}

ValidatedTopology GraphValidator::require_valid_local(TopologySpec         topology,
                                                      const GraphRegistry& registry) {
    auto report = validate_local(topology, registry);
    if (report.has_errors()) {
        throw std::runtime_error("graph validation failed (schema_version " +
                                 std::to_string(topology.schema_version) + "):\n" +
                                 report.summary());
    }
    return ValidatedTopology(std::move(topology), std::move(report));
}

}  // namespace neograph::graph
