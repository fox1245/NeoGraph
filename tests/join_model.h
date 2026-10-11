// The bounded-exhaustive join model (Program API v2, WP7; #363): the graph
// family, its enumeration and the reference interpreter of the approved join
// semantics. tests/test_join_exhaustive.cpp states the rules and drives the
// real Scheduler and GraphEngine against it; tests/test_graph_soundness.cpp
// checks the soundness check (X3) against it. NEOGRAPH_JOIN_MODEL_BOUND and
// the other NEOGRAPH_JOIN_MODEL_* variables select the bound for both.
#pragma once

#include <neograph/json.h>

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace join_model {

using neograph::json;

inline constexpr int kEnd      = -1;
inline constexpr int kMaxNodes = 6;

// ---------------------------------------------------------------------------
// Graphs
// ---------------------------------------------------------------------------

inline std::string node_name(int index) { return "n" + std::to_string(index); }
inline std::string target_name(int target) { return target == kEnd ? "__end__" : node_name(target); }
inline std::uint32_t bit(int index) { return std::uint32_t{1} << index; }
inline int popcount(std::uint32_t value) { return static_cast<int>(std::bitset<32>(value).count()); }

struct Route {
    bool             conditional = false;
    std::vector<int> targets;  // static fan-out (empty: implicit end), or {label a, label b}
};

struct Graph {
    int                        n = 0;
    std::vector<Route>         routes;
    int                        ender = -1;  // node that may return Command.goto(__end__)
    std::vector<std::uint32_t> slots;       // join wait_for: one alternative mask per slot

    int           join() const { return n - 1; }
    std::uint32_t members() const {
        std::uint32_t all = 0;
        for (const auto slot : slots) all |= slot;
        return all;
    }
    bool any_of() const {
        for (const auto slot : slots)
            if (popcount(slot) > 1) return true;
        return false;
    }
    std::uint32_t conditional_nodes() const {
        std::uint32_t result = 0;
        for (int u = 0; u < n; ++u)
            if (routes[u].conditional) result |= bit(u);
        return result;
    }
};

inline json wait_for_json(const Graph& graph) {
    json wait_for = json::array();
    for (const auto slot : graph.slots) {
        if (popcount(slot) == 1) {
            for (int u = 0; u < graph.n; ++u)
                if (slot & bit(u)) wait_for.push_back(node_name(u));
            continue;
        }
        json alternatives = json::array();
        for (int u = 0; u < graph.n; ++u)
            if (slot & bit(u)) alternatives.push_back(node_name(u));
        wait_for.push_back(json{{"any_of", std::move(alternatives)}});
    }
    return wait_for;
}

// The Core JSON of a graph. Engine runs add the script and trace channels.
inline json definition(const Graph& graph, int schema_version, bool engine) {
    json channels = json::object();
    if (engine) {
        channels["events"] = json{{"reducer", "append"}, {"initial", json::array()}};
        channels["script"] = json{{"reducer", "overwrite"}, {"initial", json::object()}};
    }
    json nodes      = json::object();
    json edges      = json::array({json{{"from", "__start__"}, {"to", node_name(0)}}});
    json conditions = json::array();
    for (int u = 0; u < graph.n; ++u) {
        const auto& route = graph.routes[u];
        json        node{{"type", "join_model.node"},
                         {"conditional", route.conditional},
                         {"may_end", graph.ender == u}};
        if (u == graph.join()) node["barrier"] = json{{"wait_for", wait_for_json(graph)}};
        nodes[node_name(u)] = std::move(node);
        if (route.conditional) {
            channels["route_" + node_name(u)] = json{{"reducer", "overwrite"}, {"initial", "a"}};
            conditions.push_back(json{{"from", node_name(u)},
                                      {"condition", "join_model.route." + node_name(u)},
                                      {"routes", json{{"a", target_name(route.targets[0])},
                                                      {"b", target_name(route.targets[1])}}}});
        } else {
            for (const auto target : route.targets)
                edges.push_back(json{{"from", node_name(u)}, {"to", target_name(target)}});
        }
    }
    json result{{"name", "join_model"},
                {"channels", std::move(channels)},
                {"nodes", std::move(nodes)},
                {"edges", std::move(edges)},
                {"conditional_edges", std::move(conditions)}};
    if (schema_version > 0) result["schema_version"] = schema_version;
    return result;
}

inline std::string describe(const Graph& graph) { return definition(graph, 1, false).dump(); }

// ---------------------------------------------------------------------------
// Reference interpreter
// ---------------------------------------------------------------------------

/// One super-step's decisions: bit u of label_b sends conditional node u along
/// label "b" (else "a"); bit u of end makes node u return Command.goto(__end__).
struct Choice {
    std::uint32_t label_b = 0;
    std::uint32_t end     = 0;
};

struct Step {
    std::uint32_t ready        = 0;
    std::uint32_t pending      = 0;
    bool          hit_end      = false;  // some route reached __end__ (the scheduler's flag)
    bool          explicit_end = false;  // an explicit route or Command.goto reached __end__
    bool          fired        = false;  // the join fires next super-step
    std::uint32_t arrivals     = 0;      // nodes that signaled the join this super-step
};

inline Step reference_step(const Graph& graph, std::uint32_t ready, std::uint32_t pending, Choice choice) {
    Step out;
    out.pending = pending;
    if (choice.end & ready) {  // rule 3: preempts routes, joins keep their signals
        out.hit_end      = true;
        out.explicit_end = true;
        return out;
    }
    std::uint32_t to_join = 0;
    for (int u = 0; u < graph.n; ++u) {
        if (!(ready & bit(u))) continue;
        const auto& route = graph.routes[u];
        if (route.conditional) {
            const int target = route.targets[(choice.label_b >> u) & 1U];
            if (target == kEnd)
                out.hit_end = out.explicit_end = true;
            else if (target == graph.join())
                to_join |= bit(u);
            else
                out.ready |= bit(target);
            continue;
        }
        if (route.targets.empty()) out.hit_end = true;  // implicit end: never explicit
        for (const auto target : route.targets) {
            if (target == kEnd)
                out.hit_end = out.explicit_end = true;
            else if (target == graph.join())
                to_join |= bit(u);
            else
                out.ready |= bit(target);
        }
    }
    out.arrivals = to_join;
    if (to_join) {
        out.pending |= to_join;  // rule 2: non-members are recorded too
        bool complete = true;    // rule 1
        for (const auto slot : graph.slots) complete = complete && (slot & out.pending) != 0;
        if (complete) {
            out.ready |= bit(graph.join());
            out.pending = 0;
            out.fired   = true;
        }
    }
    return out;
}

// Unexpected is never a reference outcome: an engine status the model has no
// counterpart for (Interrupted, SafePoint) always mismatches.
enum class Outcome { Completed, Stalled, StepLimit, Unexpected };

inline const char* to_string(Outcome outcome) {
    switch (outcome) {
        case Outcome::Completed:
            return "Completed";
        case Outcome::Stalled:
            return "Failed(barrier stalled)";
        case Outcome::StepLimit:
            return "StepLimit";
        case Outcome::Unexpected:
            return "unexpected status";
    }
    return "?";
}

/// Rule 4, applied to the final super-step: the one that leaves nothing ready.
inline Outcome quiescent_outcome(const Step& last) {
    return last.pending != 0 && !last.explicit_end ? Outcome::Stalled : Outcome::Completed;
}

template <class Fn>
inline void for_each_choice(const Graph& graph, std::uint32_t ready, Fn&& visit) {
    const std::uint32_t labels = ready & graph.conditional_nodes();
    const std::uint32_t enders = graph.ender >= 0 ? ready & bit(graph.ender) : 0;
    // Enumerate every submask of each decision set.
    std::uint32_t label = labels;
    while (true) {
        std::uint32_t end = enders;
        while (true) {
            visit(Choice{label, end});
            if (end == 0) break;
            end = (end - 1) & enders;
        }
        if (label == 0) break;
        label = (label - 1) & labels;
    }
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

struct Bounds {
    std::string name;
    int         max_nodes        = 4;
    int         max_conditionals = 2;
    int         max_fanout       = 2;
    int         max_back_edges   = 1;
    bool        enders           = true;
    int         engine_max_nodes = 3;
    int         max_steps        = 8;
};

inline int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
}

inline Bounds bounds() {
    Bounds result;
    const char* selected = std::getenv("NEOGRAPH_JOIN_MODEL_BOUND");
    if (selected && std::string(selected) == "extended") {
        result.name             = "extended";
        result.max_nodes        = 5;
        result.engine_max_nodes = 4;
        result.max_steps        = 10;
    } else {
        result.name = "ci";
    }
    result.max_nodes        = std::min(env_int("NEOGRAPH_JOIN_MODEL_NODES", result.max_nodes), kMaxNodes);
    result.engine_max_nodes = env_int("NEOGRAPH_JOIN_MODEL_ENGINE_NODES", result.engine_max_nodes);
    result.max_steps        = env_int("NEOGRAPH_JOIN_MODEL_STEPS", result.max_steps);
    return result;
}

inline void append_route_options(int u, int n, const Bounds& limits, std::vector<std::pair<Route, int>>& out) {
    std::vector<int> targets;   // later nodes and __end__
    std::vector<int> backwards; // a back edge: this node or an earlier one
    for (int v = u + 1; v < n; ++v) targets.push_back(v);
    targets.push_back(kEnd);
    if (limits.max_back_edges > 0)
        for (int v = 0; v <= u; ++v) backwards.push_back(v);
    std::vector<int> all = targets;
    all.insert(all.end(), backwards.begin(), backwards.end());
    const auto back_count = [u](const std::vector<int>& chosen) {
        int count = 0;
        for (const auto target : chosen) count += target != kEnd && target <= u;
        return count;
    };
    // Static: every subset of at most max_fanout targets.
    out.push_back({Route{false, {}}, 0});
    for (std::size_t i = 0; i < all.size(); ++i) {
        out.push_back({Route{false, {all[i]}}, back_count({all[i]})});
        if (limits.max_fanout < 2) continue;
        for (std::size_t j = i + 1; j < all.size(); ++j)
            out.push_back({Route{false, {all[i], all[j]}}, back_count({all[i], all[j]})});
    }
    // Conditional: two distinct targets (the labels are symmetric).
    if (limits.max_conditionals > 0)
        for (std::size_t i = 0; i < all.size(); ++i)
            for (std::size_t j = i + 1; j < all.size(); ++j)
                out.push_back({Route{true, {all[i], all[j]}}, back_count({all[i], all[j]})});
}

inline bool all_reachable(const Graph& graph) {
    std::uint32_t seen = bit(0), frontier = bit(0);
    while (frontier) {
        std::uint32_t next = 0;
        for (int u = 0; u < graph.n; ++u) {
            if (!(frontier & bit(u))) continue;
            for (const auto target : graph.routes[u].targets)
                if (target != kEnd) next |= bit(target);
        }
        frontier = next & ~seen;
        seen |= next;
    }
    return seen == (bit(graph.n) - 1);
}

/// Every partition of `members` into slots (set partitions, by restricted growth).
inline void for_each_partition(std::uint32_t members, const std::function<void(std::vector<std::uint32_t>)>& visit) {
    std::vector<int> elements;
    for (int u = 0; u < 32; ++u)
        if (members & bit(u)) elements.push_back(u);
    std::vector<std::uint32_t> blocks;
    std::function<void(std::size_t)> place = [&](std::size_t index) {
        if (index == elements.size()) {
            visit(blocks);
            return;
        }
        // By index: the recursion below appends to `blocks`.
        for (std::size_t block = 0; block < blocks.size(); ++block) {
            blocks[block] |= bit(elements[index]);
            place(index + 1);
            blocks[block] &= ~bit(elements[index]);
        }
        blocks.push_back(bit(elements[index]));
        place(index + 1);
        blocks.pop_back();
    };
    place(0);
}

template <class Fn>
inline void for_each_graph(const Bounds& limits, Fn&& visit) {
    for (int n = 2; n <= limits.max_nodes; ++n) {
        std::vector<std::vector<std::pair<Route, int>>> options(static_cast<std::size_t>(n));
        for (int u = 0; u < n; ++u) append_route_options(u, n, limits, options[u]);
        Graph graph;
        graph.n = n;
        graph.routes.resize(static_cast<std::size_t>(n));
        std::function<void(int, int, int)> assign = [&](int u, int conditionals, int back_edges) {
            if (u == n) {
                if (!all_reachable(graph)) return;
                std::uint32_t predecessors = 0;
                for (int v = 0; v < n - 1; ++v)
                    for (const auto target : graph.routes[v].targets)
                        if (target == graph.join()) predecessors |= bit(v);
                if (!predecessors) return;
                for (std::uint32_t members = predecessors; members;
                     members      = (members - 1) & predecessors) {
                    for_each_partition(members, [&](std::vector<std::uint32_t> slots) {
                        graph.slots = std::move(slots);
                        for (int ender = -1; ender < (limits.enders ? n : 0); ++ender) {
                            graph.ender = ender;
                            visit(static_cast<const Graph&>(graph));
                        }
                    });
                }
                return;
            }
            for (const auto& [route, backs] : options[u]) {
                const int c = conditionals + (route.conditional ? 1 : 0);
                const int b = back_edges + backs;
                if (c > limits.max_conditionals || b > limits.max_back_edges) continue;
                graph.routes[u] = route;
                assign(u + 1, c, b);
            }
        };
        assign(0, 0, 0);
    }
}

inline std::string mask_names(std::uint32_t value, int n) {
    std::string out = "{";
    for (int u = 0; u < n; ++u)
        if (value & bit(u)) out += (out.size() > 1 ? "," : "") + node_name(u);
    return out + "}";
}

inline Graph graph_from(int n, std::vector<Route> routes, std::vector<std::uint32_t> slots, int ender = -1) {
    Graph graph;
    graph.n      = n;
    graph.routes = std::move(routes);
    graph.slots  = std::move(slots);
    graph.ender  = ender;
    return graph;
}

}  // namespace join_model
