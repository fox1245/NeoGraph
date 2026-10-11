// Bounded-exhaustive join harness (Program API v2, WP7; #363).
//
// A small reference interpreter states the approved join semantics: BSP
// super-steps, an AND barrier per join whose wait_for slots may each hold
// several any_of alternatives, conditional routes as nondeterministic label
// choices, and the stall rule with its explicit-end exception. The harness
// enumerates every graph in a bounded family, explores every route-choice
// sequence, and drives the real Core against the reference:
//
//  - Scheduler level: every reachable (ready set, barrier state) transition is
//    replayed through Scheduler::plan_next_step on a Scheduler built from the
//    parsed Core JSON, and the next ready set, barrier state and hit_end must
//    equal the reference.
//  - Engine level (smaller bound): every route-choice sequence is run through
//    GraphEngine, with nodes that follow a choice script, and the per-step
//    ready sets and the outcome must equal the reference.
//
// Reference semantics (tests/model/README.md has the same rules in TLA+):
//
//  1. A slot is filled when any of its alternatives signals the join. The join
//     fires when every slot is filled; then all accumulated signals clear.
//  2. Signals from non-members are recorded and never count.
//  3. Command.goto preempts every route of the super-step and bypasses joins.
//  4. When no node is ready, the run ends. It is stalled, and Failed, when a
//     join still holds signals (from members or not), unless the final
//     super-step (the one after which nothing is ready) reached __end__ by an
//     explicit route: a static edge to __end__ (as .exit() writes), a
//     conditional route to __end__, or Command.goto(__end__). An implicit end
//     (a node without successors) never suppresses a stall, and an explicit
//     end in an earlier super-step does not either.
//  5. A run that still has ready nodes after max_steps super-steps hits the
//     step limit.
//
// Family (default "ci" bound; NEOGRAPH_JOIN_MODEL_BOUND=extended for the full
// one): nodes n0..n{N-1}, n0 is the entry and n{N-1} the join. Every node,
// the join included, routes statically to at most two targets (none: an
// implicit end) or conditionally to two distinct targets through labels "a"
// and "b"; targets are later nodes or __end__, plus at most one back edge
// (to the node itself or an earlier one). At most two nodes are conditional,
// at most one node may return Command.goto(__end__), and every node is
// reachable. The join waits for a nonempty subset of its other static
// predecessors, as legacy singleton slots or as every partition into any_of
// slots. Duplicates up to node renaming are kept. NEOGRAPH_JOIN_MODEL_NODES,
// _ENGINE_NODES and _STEPS override single limits.
//
// Two switches mark work that has not landed on this base:
//
//  - NEOGRAPH_JOIN_MODEL_ANY_OF (default 0): the Core parser and Scheduler do
//    not know any_of slots yet (#363, WP4). Slot graphs run in the reference
//    only. When WP4 lands, set it to 1 and pass the topology's join specs in
//    scheduler_for().
//  - NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS (default 0): this base ends a
//    stalled run Completed (#357). Those runs are counted as the known
//    disagreement and must differ from the reference in the outcome only.
//    When the bug-stream stall detection lands, set it to 1 and teach
//    engine_outcome() how a stalled run is reported; every counted run then
//    has to match.

#include <neograph/graph/compiler.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/validator.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NEOGRAPH_JOIN_MODEL_ANY_OF
#define NEOGRAPH_JOIN_MODEL_ANY_OF 0
#endif
#ifndef NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS
#define NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS 0
#endif

namespace {

using neograph::json;
using namespace neograph::graph;

constexpr int kEnd      = -1;
constexpr int kMaxNodes = 6;

// ---------------------------------------------------------------------------
// Graphs
// ---------------------------------------------------------------------------

std::string node_name(int index) { return "n" + std::to_string(index); }
std::string target_name(int target) { return target == kEnd ? "__end__" : node_name(target); }
std::uint32_t bit(int index) { return std::uint32_t{1} << index; }
int popcount(std::uint32_t value) { return static_cast<int>(std::bitset<32>(value).count()); }

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

json wait_for_json(const Graph& graph) {
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
json definition(const Graph& graph, int schema_version, bool engine) {
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

std::string describe(const Graph& graph) { return definition(graph, 1, false).dump(); }

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

Step reference_step(const Graph& graph, std::uint32_t ready, std::uint32_t pending, Choice choice) {
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

enum class Outcome { Completed, Stalled, StepLimit };

const char* to_string(Outcome outcome) {
    switch (outcome) {
        case Outcome::Completed:
            return "Completed";
        case Outcome::Stalled:
            return "Failed(barrier stalled)";
        case Outcome::StepLimit:
            return "StepLimit";
    }
    return "?";
}

/// Rule 4, applied to the final super-step: the one that leaves nothing ready.
Outcome quiescent_outcome(const Step& last) {
    return last.pending != 0 && !last.explicit_end ? Outcome::Stalled : Outcome::Completed;
}

template <class Fn>
void for_each_choice(const Graph& graph, std::uint32_t ready, Fn&& visit) {
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

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
}

Bounds bounds() {
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

void append_route_options(int u, int n, const Bounds& limits, std::vector<std::pair<Route, int>>& out) {
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

bool all_reachable(const Graph& graph) {
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
void for_each_partition(std::uint32_t members, const std::function<void(std::vector<std::uint32_t>)>& visit) {
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
void for_each_graph(const Bounds& limits, Fn&& visit) {
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

// ---------------------------------------------------------------------------
// The real Core
// ---------------------------------------------------------------------------

// Nodes follow a choice script: visit k of node u reads script[u][k], a label
// ("a" or "b", or "-" for static nodes) with a trailing "!" when the node
// returns Command.goto(__end__). Each run appends [step, name] to "events".
class ScriptedNode final : public GraphNode {
public:
    ScriptedNode(std::string name, bool conditional)
        : name_(std::move(name)), conditional_(conditional) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto  events = input.state.get("events");
        std::size_t visit  = 0;
        for (const auto& event : events)
            if (event[1].get<std::string>() == name_) ++visit;
        const auto choice = input.state.get("script")[name_][visit].get<std::string>();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "events", json::array({json::array({input.ctx.step, name_})})});
        if (conditional_)
            out.writes.push_back(ChannelWrite{"route_" + name_, json(choice.substr(0, 1))});
        if (choice.back() == '!') out.command = Command{"__end__", {}};
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        conditional_;
};

void register_model_executables() {
    static const bool registered = [] {
        // Engines run fan-out serially here, which is deterministic and enough
        // for routing; keep the one-time advice about worker pools off stderr.
#if defined(_WIN32)
        _putenv_s("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1");
#else
        setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1", 1);
#endif
        NodeFactory::instance().register_type(
            "join_model.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<ScriptedNode>(name, config.value("conditional", false));
            },
            json{{"type", "object"}});
        for (int u = 0; u < kMaxNodes; ++u) {
            const auto channel = "route_" + node_name(u);
            ConditionRegistry::instance().register_condition(
                "join_model.route." + node_name(u),
                [channel](const GraphState& state) { return state.get(channel).get<std::string>(); });
        }
        return true;
    }();
    (void)registered;
}

/// The one place the harness builds a Scheduler from a parsed topology.
/// WP4: pass topology.join_specs here once the Scheduler accepts them.
std::unique_ptr<Scheduler> scheduler_for(const TopologySpec& topology) {
    return std::make_unique<Scheduler>(topology.edges, topology.conditional_edges,
                                       topology.barrier_specs);
}

class RealScheduler {
public:
    explicit RealScheduler(const Graph& graph)
        : graph_(graph), topology_(GraphCompiler::parse(definition(graph, 1, false))) {
        scheduler_ = scheduler_for(topology_);
        for (int u = 0; u < graph.n; ++u)
            if (graph.routes[u].conditional)
                state_.init_channel("route_" + node_name(u), ReducerType::OVERWRITE,
                                    ReducerRegistry::instance().get("overwrite"), json("a"));
    }

    std::uint32_t start() const { return mask(scheduler_->plan_start_step()); }

    Step step(std::uint32_t ready, std::uint32_t pending, Choice choice, std::string& problem) {
        std::vector<ChannelWrite> labels;
        std::vector<StepRouting>  routings;
        for (int u = 0; u < graph_.n; ++u) {
            if (!(ready & bit(u))) continue;
            if (graph_.routes[u].conditional)
                labels.push_back(ChannelWrite{"route_" + node_name(u),
                                              json((choice.label_b & bit(u)) ? "b" : "a")});
            StepRouting routing{node_name(u), std::nullopt};
            if (choice.end & bit(u)) routing.command_goto = "__end__";
            routings.push_back(std::move(routing));
        }
        if (!labels.empty()) state_.apply_writes(labels);
        BarrierState barrier;
        const auto   join = node_name(graph_.join());
        if (pending) barrier[join] = names(pending);
        const auto plan = scheduler_->plan_next_step(routings, state_, barrier);

        Step out;
        out.ready   = mask(plan.ready);
        out.hit_end = plan.hit_end;
        for (const auto& [target, signals] : barrier) {
            if (target != join && !signals.empty())
                problem = "barrier state for a node that is not a join: " + target;
            if (target == join) out.pending = mask({signals.begin(), signals.end()});
        }
        return out;
    }

private:
    static std::uint32_t mask(const std::vector<std::string>& names) {
        std::uint32_t result = 0;
        for (const auto& name : names) result |= bit(std::stoi(name.substr(1)));
        return result;
    }
    std::set<std::string> names(std::uint32_t value) const {
        std::set<std::string> result;
        for (int u = 0; u < graph_.n; ++u)
            if (value & bit(u)) result.insert(node_name(u));
        return result;
    }

    const Graph&               graph_;
    TopologySpec               topology_;
    std::unique_ptr<Scheduler> scheduler_;
    GraphState                 state_;
};

// ---------------------------------------------------------------------------
// Statistics and failures
// ---------------------------------------------------------------------------

struct Totals {
    std::uint64_t graphs = 0, legacy_graphs = 0, any_of_graphs = 0;
    std::uint64_t states = 0, transitions = 0;
    std::uint64_t completed = 0, stalled = 0, explicit_end_completions = 0;
    std::uint64_t cyclic_graphs = 0, late_alternative_stalls = 0;
    std::uint64_t engine_graphs = 0, engine_skipped_invalid = 0, engine_runs = 0;
    std::uint64_t engine_known_stall_disagreements = 0, engine_step_limits = 0;
    std::uint64_t engine_known_member_waiting = 0, engine_known_non_member_only = 0;
    std::vector<std::string> failures;
    std::uint64_t            failure_count = 0;
    std::string              first_member_waiting, first_non_member_only;

    void fail(std::string message) {
        ++failure_count;
        if (failures.size() < 8) failures.push_back(std::move(message));
    }
};

std::string mask_names(std::uint32_t value, int n) {
    std::string out = "{";
    for (int u = 0; u < n; ++u)
        if (value & bit(u)) out += (out.size() > 1 ? "," : "") + node_name(u);
    return out + "}";
}

std::string describe_step(const Graph& graph, std::uint32_t ready, std::uint32_t pending,
                          Choice choice) {
    std::ostringstream out;
    out << "graph " << describe(graph) << "\n  ready " << mask_names(ready, graph.n)
        << " pending " << mask_names(pending, graph.n) << " label_b "
        << mask_names(choice.label_b, graph.n) << " end " << mask_names(choice.end, graph.n);
    return out.str();
}

// ---------------------------------------------------------------------------
// Exploration
// ---------------------------------------------------------------------------

struct StateKey {
    std::uint32_t ready, pending;
    bool          fired_before;
    bool operator==(const StateKey&) const = default;
};
struct StateKeyHash {
    std::size_t operator()(const StateKey& key) const {
        return std::hash<std::uint64_t>{}((std::uint64_t{key.ready} << 33) |
                                          (std::uint64_t{key.pending} << 1) |
                                          std::uint64_t{key.fired_before});
    }
};

/// Walks every reachable state of one graph once, checks the reference
/// invariants on every transition and, when `real` is given, compares it.
void explore(const Graph& graph, RealScheduler* real, Totals& totals) {
    const std::uint32_t entry = bit(0);
    if (real && real->start() != entry)
        totals.fail("plan_start_step differs from the entry for " + describe(graph));

    std::unordered_map<StateKey, int, StateKeyHash> index;  // DFS colour: 1 open, 2 done
    bool                                            cyclic = false;
    std::function<void(StateKey)>                   visit  = [&](StateKey state) {
        index[state] = 1;
        ++totals.states;
        for_each_choice(graph, state.ready, [&](Choice choice) {
            ++totals.transitions;
            const auto next = reference_step(graph, state.ready, state.pending, choice);

            // Invariant: the join fires only on a complete round, at most once.
            if (next.fired)
                for (const auto slot : graph.slots)
                    if (!(slot & (state.pending | next.arrivals)))
                        totals.fail("join fired with an empty slot: " +
                                    describe_step(graph, state.ready, state.pending, choice));
            if (real) {
                std::string problem;
                const auto  actual = real->step(state.ready, state.pending, choice, problem);
                if (!problem.empty() || actual.ready != next.ready ||
                    actual.pending != next.pending || actual.hit_end != next.hit_end) {
                    std::ostringstream out;
                    out << "scheduler differs from the reference: "
                        << describe_step(graph, state.ready, state.pending, choice)
                        << "\n  reference ready " << mask_names(next.ready, graph.n) << " pending "
                        << mask_names(next.pending, graph.n) << " hit_end " << next.hit_end
                        << "\n  scheduler ready " << mask_names(actual.ready, graph.n)
                        << " pending " << mask_names(actual.pending, graph.n) << " hit_end "
                        << actual.hit_end << (problem.empty() ? "" : "\n  " + problem);
                    totals.fail(out.str());
                }
            }
            if (next.ready == 0) {
                const auto outcome = quiescent_outcome(next);
                // Invariants: a stall fails the run; only an explicit end in the
                // final super-step suppresses it.
                if ((outcome == Outcome::Stalled) != (next.pending != 0 && !next.explicit_end))
                    totals.fail("stall classification is inconsistent for " + describe(graph));
                if (outcome == Outcome::Completed && next.pending != 0 && !next.explicit_end)
                    totals.fail("a run with a waiting join completed without a final explicit end");
                if (outcome == Outcome::Stalled) {
                    ++totals.stalled;
                    // A leftover any_of alternative: the join fired before and only
                    // alternatives of multi-source slots are waiting.
                    std::uint32_t multi = 0;
                    for (const auto slot : graph.slots)
                        if (popcount(slot) > 1) multi |= slot;
                    if (state.fired_before && (next.pending & ~multi) == 0)
                        ++totals.late_alternative_stalls;
                } else {
                    ++totals.completed;
                    if (next.pending != 0) ++totals.explicit_end_completions;
                }
                return;
            }
            const StateKey successor{next.ready, next.pending, state.fired_before || next.fired};
            const auto     found = index.find(successor);
            if (found == index.end())
                visit(successor);
            else if (found->second == 1)
                cyclic = true;
        });
        index[state] = 2;
    };
    visit(StateKey{entry, 0, false});
    if (cyclic) ++totals.cyclic_graphs;
}

/// Every route-choice sequence of at most `max_steps` super-steps, as the
/// per-step ready sets, the per-node choice script and the reference outcome.
struct Path {
    std::vector<std::uint32_t>              ready_sets;
    std::map<std::string, std::vector<std::string>> script;
    Outcome                                 outcome       = Outcome::Completed;
    std::uint32_t                           final_pending = 0;
};

void for_each_path(const Graph& graph, int max_steps, const std::function<void(const Path&)>& visit) {
    Path path;
    std::function<void(std::uint32_t, std::uint32_t)> walk = [&](std::uint32_t ready,
                                                                  std::uint32_t pending) {
        if (static_cast<int>(path.ready_sets.size()) == max_steps) {
            path.outcome = Outcome::StepLimit;
            visit(path);
            return;
        }
        path.ready_sets.push_back(ready);
        for_each_choice(graph, ready, [&](Choice choice) {
            for (int u = 0; u < graph.n; ++u) {
                if (!(ready & bit(u))) continue;
                std::string entry = graph.routes[u].conditional
                                        ? ((choice.label_b & bit(u)) ? "b" : "a")
                                        : "-";
                if (choice.end & bit(u)) entry += "!";
                path.script[node_name(u)].push_back(std::move(entry));
            }
            const auto next = reference_step(graph, ready, pending, choice);
            if (next.ready == 0) {
                path.outcome       = quiescent_outcome(next);
                path.final_pending = next.pending;
                visit(path);
            } else {
                walk(next.ready, next.pending);
            }
            for (int u = 0; u < graph.n; ++u)
                if (ready & bit(u)) path.script[node_name(u)].pop_back();
        });
        path.ready_sets.pop_back();
    };
    walk(bit(0), 0);
}

/// How this base reports a run. It has no stall detection, so a stalled run
/// reads as Completed here (see NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS).
Outcome engine_outcome(const RunResult& result) {
    if (result.output.contains("_neograph") &&
        result.output["_neograph"].value("max_steps_exhausted", false))
        return Outcome::StepLimit;
    return Outcome::Completed;
}

/// An engine for the strict (schema 1) document, or nullptr when the validator
/// rejects it. link() prints the lint of strict topologies to stderr, so the
/// parsed topology is linked as schema 0; no runtime path reads the version.
std::unique_ptr<GraphEngine> make_engine(const Graph& graph) {
    auto topology = GraphCompiler::parse(definition(graph, 1, true));
    if (GraphValidator::validate(topology).has_errors()) return nullptr;
    topology.schema_version = 0;
    return GraphEngine::link(GraphCompiler::link(std::move(topology), NodeContext{}));
}

void run_engine(const Graph& graph, const Bounds& limits, Totals& totals) {
    auto engine = make_engine(graph);
    if (!engine) {
        ++totals.engine_skipped_invalid;
        return;
    }
    ++totals.engine_graphs;
    for_each_path(graph, limits.max_steps, [&](const Path& path) {
        ++totals.engine_runs;
        json script = json::object();
        for (const auto& [name, choices] : path.script) {
            json values = json::array();
            for (const auto& choice : choices) values.push_back(choice);
            script[name] = std::move(values);
        }
        RunConfig config;
        config.input     = json{{"script", script}};
        config.max_steps = limits.max_steps;
        const auto result = engine->run(config);

        std::vector<std::uint32_t> ready_sets;
        for (const auto& event : result.output["channels"]["events"]["value"]) {
            const auto step = event[0].get<std::size_t>();
            if (ready_sets.size() <= step) ready_sets.resize(step + 1, 0);
            ready_sets[step] |= bit(std::stoi(event[1].get<std::string>().substr(1)));
        }
        const auto actual = engine_outcome(result);
        if (path.outcome == Outcome::StepLimit) ++totals.engine_step_limits;
        const bool same_trace = ready_sets == path.ready_sets;
        if (same_trace && actual == path.outcome) return;
        if (same_trace && !NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS &&
            path.outcome == Outcome::Stalled && actual == Outcome::Completed) {
            ++totals.engine_known_stall_disagreements;
            const auto example = describe(graph) + " script " + script.dump();
            if (path.final_pending & graph.members()) {
                if (totals.engine_known_member_waiting++ == 0) totals.first_member_waiting = example;
            } else if (totals.engine_known_non_member_only++ == 0) {
                totals.first_non_member_only = example;
            }
            return;
        }
        std::ostringstream out;
        out << "engine differs from the reference: graph " << describe(graph) << "\n  script "
            << script.dump() << "\n  reference " << to_string(path.outcome) << " steps";
        for (const auto ready : path.ready_sets) out << " " << mask_names(ready, graph.n);
        out << "\n  engine    " << to_string(actual) << " steps";
        for (const auto ready : ready_sets) out << " " << mask_names(ready, graph.n);
        totals.fail(out.str());
    });
}

void report(const char* title, const Bounds& limits, const Totals& totals, double seconds) {
    std::cout << "[ join model ] " << title << " (bound " << limits.name << ": nodes<="
              << limits.max_nodes << ", conditionals<=" << limits.max_conditionals
              << ", fan-out<=" << limits.max_fanout << ", back edges<=" << limits.max_back_edges
              << ", enders<=" << (limits.enders ? 1 : 0) << ", steps<=" << limits.max_steps
              << ")\n  graphs " << totals.graphs << " (legacy " << totals.legacy_graphs
              << ", any_of " << totals.any_of_graphs << ")\n";
    if (totals.states)
        std::cout << "  states " << totals.states << ", transitions " << totals.transitions
                  << ", graphs with cyclic runs " << totals.cyclic_graphs
                  << "\n  quiescent outcomes: completed " << totals.completed
                  << " (by an explicit end over a waiting join " << totals.explicit_end_completions
                  << "), stalled " << totals.stalled << " (by a leftover any_of alternative "
                  << totals.late_alternative_stalls << ")\n";
    if (totals.engine_graphs || totals.engine_skipped_invalid)
        std::cout << "  engine runs " << totals.engine_runs << " over " << totals.engine_graphs
                  << " graphs (skipped with validator errors " << totals.engine_skipped_invalid
                  << "), step limits " << totals.engine_step_limits
                  << "\n  known #357 disagreements (reference stalls, engine completes) "
                  << totals.engine_known_stall_disagreements << ": a member waiting "
                  << totals.engine_known_member_waiting << ", only non-member signals waiting "
                  << totals.engine_known_non_member_only << "\n";
    if (!totals.first_member_waiting.empty())
        std::cout << "  smallest with a member waiting: " << totals.first_member_waiting << "\n";
    if (!totals.first_non_member_only.empty())
        std::cout << "  smallest with only non-member signals waiting: "
                  << totals.first_non_member_only << "\n";
    std::cout << "  mismatches " << totals.failure_count << ", " << seconds << " s\n";
}

void expect_clean(const Totals& totals) {
    EXPECT_EQ(totals.failure_count, 0U);
    for (const auto& failure : totals.failures) ADD_FAILURE() << failure;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Graph graph_from(int n, std::vector<Route> routes, std::vector<std::uint32_t> slots, int ender = -1) {
    Graph graph;
    graph.n      = n;
    graph.routes = std::move(routes);
    graph.slots  = std::move(slots);
    graph.ender  = ender;
    return graph;
}

/// Runs one scripted path through the reference.
struct Trace {
    std::vector<std::uint32_t> ready_sets;
    Outcome                    outcome;
};
Trace reference_run(const Graph& graph, const std::vector<Choice>& choices) {
    Trace         trace{{}, Outcome::StepLimit};
    std::uint32_t ready = bit(0), pending = 0;
    for (const auto choice : choices) {
        trace.ready_sets.push_back(ready);
        const auto next = reference_step(graph, ready, pending, choice);
        if (next.ready == 0) {
            trace.outcome = quiescent_outcome(next);
            return trace;
        }
        ready   = next.ready;
        pending = next.pending;
    }
    return trace;
}

// The #357 shape. n0=A fans out to n1=B and n2=C; B routes "a" (pass) to the
// join n4=D and "b" (fail) to n3=FIX; FIX and C go to D; D exits.
Graph issue_357(std::vector<std::uint32_t> slots) {
    return graph_from(5,
                      {Route{false, {1, 2}}, Route{true, {4, 3}}, Route{false, {4}},
                       Route{false, {4}}, Route{false, {kEnd}}},
                      std::move(slots));
}

}  // namespace

// The approved semantics on hand-written cases, so a change to the reference
// interpreter is a visible change to this test.
TEST(JoinModel, ReferenceStatesTheApprovedSemantics) {
    const Choice pass{0, 0}, fail{bit(1), 0}, none{0, 0};

    // #357 with the legacy barrier ["B", "C"]: the fail route stalls.
    const auto legacy = issue_357({bit(1), bit(2)});
    EXPECT_EQ(reference_run(legacy, {none, pass, none, none}).outcome, Outcome::Completed);
    const auto stalled = reference_run(legacy, {none, fail, none, none});
    EXPECT_EQ(stalled.outcome, Outcome::Stalled);
    EXPECT_EQ(stalled.ready_sets,
              (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3)}));

    // #357 with [{any_of: ["B", "FIX"]}, "C"]: both routes reach D once.
    const auto slot = issue_357({bit(1) | bit(3), bit(2)});
    const auto on_pass = reference_run(slot, {none, pass, none, none});
    EXPECT_EQ(on_pass.outcome, Outcome::Completed);
    EXPECT_EQ(on_pass.ready_sets, (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(4)}));
    const auto on_fail = reference_run(slot, {none, fail, none, none, none});
    EXPECT_EQ(on_fail.outcome, Outcome::Completed);
    EXPECT_EQ(on_fail.ready_sets,
              (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3), bit(4)}));

    // Non-exclusive alternatives, B goes to D and to FIX. D fires with B and C,
    // then D exits in the same final super-step in which FIX's signal arrives:
    // the explicit exit suppresses the stall.
    const auto same_step = graph_from(5,
                                      {Route{false, {1, 2}}, Route{false, {4, 3}}, Route{false, {4}},
                                       Route{false, {4}}, Route{false, {kEnd}}},
                                      {bit(1) | bit(3), bit(2)});
    const auto exit_last = reference_run(same_step, {none, none, none, none});
    EXPECT_EQ(exit_last.outcome, Outcome::Completed);
    EXPECT_EQ(exit_last.ready_sets,
              (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3) | bit(4)}));

    // B goes to D and through X to FIX. D exits at step k, FIX's signal arrives
    // at k+1 with nothing ready: a leftover alternative, so the run fails closed.
    const auto delayed = graph_from(6,
                                    {Route{false, {1, 2}}, Route{false, {5, 3}}, Route{false, {5}},
                                     Route{false, {4}}, Route{false, {5}}, Route{false, {kEnd}}},
                                    {bit(1) | bit(4), bit(2)});
    const auto late = reference_run(delayed, {none, none, none, none, none});
    EXPECT_EQ(late.outcome, Outcome::Stalled);
    EXPECT_EQ(late.ready_sets, (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2),
                                                           bit(3) | bit(5), bit(4)}));

    // Both alternatives in one super-step fire the join once.
    const auto both = graph_from(4, {Route{false, {1, 2}}, Route{false, {3}}, Route{false, {3}},
                                     Route{false, {kEnd}}},
                                 {bit(1) | bit(2)});
    const auto once = reference_run(both, {none, none, none});
    EXPECT_EQ(once.outcome, Outcome::Completed);
    EXPECT_EQ(once.ready_sets, (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3)}));

    // A non-member never counts: FIX alone leaves the legacy join waiting.
    EXPECT_EQ(reference_step(legacy, bit(3), 0, none).pending, bit(3));
    EXPECT_EQ(reference_step(legacy, bit(3), 0, none).ready, 0U);

    // Command.goto(__end__) in the final super-step suppresses the stall.
    auto ended = legacy;
    ended.ender = 3;  // FIX may end the run explicitly
    EXPECT_EQ(reference_run(ended, {none, fail, Choice{0, bit(3)}}).outcome, Outcome::Completed);
    EXPECT_EQ(reference_run(ended, {none, fail, none, none}).outcome, Outcome::Stalled);

    // A=0 fans out to B=1 and the join J=3, which waits for A and B. B routes
    // to J ("a") or to X=2 ("b"). Taking "b", the final super-step runs X: an
    // explicit exit there suppresses the stall, an implicit end does not.
    const auto exits = [](std::vector<int> x_targets) {
        return graph_from(4, {Route{false, {1, 3}}, Route{true, {3, 2}}, Route{false, x_targets},
                              Route{false, {kEnd}}},
                          {bit(0), bit(1)});
    };
    const Choice skip{bit(1), 0};
    EXPECT_EQ(reference_run(exits({kEnd}), {none, skip, none}).outcome, Outcome::Completed);
    EXPECT_EQ(reference_run(exits({}), {none, skip, none}).outcome, Outcome::Stalled);

    // An explicit end in an earlier super-step does not carry over. C=2 exits
    // at step 2 while B=1 signals the join J=4 and starts X=3; X ends
    // implicitly in the final super-step, and J (waiting for B and X) stalls.
    const auto earlier = graph_from(5,
                                    {Route{false, {1, 2}}, Route{false, {4, 3}}, Route{false, {kEnd}},
                                     Route{false, {}}, Route{false, {kEnd}}},
                                    {bit(1), bit(3)});
    const auto carried = reference_run(earlier, {none, none, none});
    EXPECT_EQ(carried.outcome, Outcome::Stalled);
    EXPECT_EQ(carried.ready_sets, (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3)}));
}

TEST(JoinModel, SchedulerMatchesTheReferenceForLegacyBarriers) {
    register_model_executables();
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (graph.any_of()) return;
        ++totals.graphs;
        ++totals.legacy_graphs;
        RealScheduler real(graph);
        explore(graph, &real, totals);
    });
    report("Scheduler::plan_next_step vs reference, legacy barriers", limits, totals,
           seconds_since(start));
    EXPECT_GT(totals.graphs, 1000U);
    expect_clean(totals);
}

TEST(JoinModel, ReferenceInvariantsHoldForAnyOfSlots) {
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (!graph.any_of()) return;
        ++totals.graphs;
        ++totals.any_of_graphs;
        explore(graph, nullptr, totals);
    });
    report("reference invariants, any_of slots", limits, totals, seconds_since(start));
    EXPECT_GT(totals.graphs, 100U);
    EXPECT_GT(totals.late_alternative_stalls, 0U) << "the family must reach leftover alternatives";
    expect_clean(totals);
}

TEST(JoinModel, SchedulerMatchesTheReferenceForAnyOfSlots) {
#if !NEOGRAPH_JOIN_MODEL_ANY_OF
    GTEST_SKIP() << "pending #363 WP4: any_of slots run in the reference only; set "
                    "NEOGRAPH_JOIN_MODEL_ANY_OF=1 when the Scheduler supports them";
#else
    register_model_executables();
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (!graph.any_of()) return;
        ++totals.graphs;
        ++totals.any_of_graphs;
        RealScheduler real(graph);
        explore(graph, &real, totals);
    });
    report("Scheduler::plan_next_step vs reference, any_of slots", limits, totals,
           seconds_since(start));
    expect_clean(totals);
#endif
}

TEST(JoinModel, EngineRunsMatchTheReference) {
    register_model_executables();
    auto limits      = bounds();
    limits.max_nodes = std::min(limits.max_nodes, limits.engine_max_nodes);
    const auto start = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (graph.any_of() && !NEOGRAPH_JOIN_MODEL_ANY_OF) return;
        ++totals.graphs;
        (graph.any_of() ? totals.any_of_graphs : totals.legacy_graphs)++;
        run_engine(graph, limits, totals);
    });
    report("GraphEngine runs vs reference", limits, totals, seconds_since(start));
    EXPECT_GT(totals.engine_runs, 1000U);
    expect_clean(totals);
}

// #357 on this base: runs that stall in the reference end Completed in the
// engine. These are the smallest targets the bug-stream stall detection must
// turn into failures; flip NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS then.
TEST(JoinModel, KnownIssue357StalledRunsEndCompleted) {
    register_model_executables();
    struct Case {
        const char*              name;
        Graph                    graph;
        json                     script;
        std::vector<Choice>      choices;
        std::size_t              trace_size;
    };
    const Choice none{0, 0}, fail{bit(1), 0};
    const std::vector<Case> cases{
        // #357: A, B, C, FIX run; D waits for B and C and never runs.
        {"issue 357, fail route", issue_357({bit(1), bit(2)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"b"})},
              {"n2", json::array({"-"})}, {"n3", json::array({"-"})}},
         {none, fail, none, none}, 4},
        // A=n0 -> {B=n1, J=n3}; B routes "b" to X=n2, which has no successors (an
        // implicit end). J waits for A and B and keeps A's signal.
        {"member waiting after an implicit end",
         graph_from(4, {Route{false, {1, 3}}, Route{true, {3, 2}}, Route{false, {}},
                        Route{false, {kEnd}}},
                    {bit(0), bit(1)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"b"})}, {"n2", json::array({"-"})}},
         {none, fail, none}, 3},
        // n0 -> n1; the join n1 waits for n0 and routes only to itself, so its
        // own (non-member) signal is left waiting.
        {"only a non-member signal waiting",
         graph_from(2, {Route{false, {1}}, Route{false, {1}}}, {bit(0)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"-"})}}, {none, none}, 2},
    };
    for (const auto& item : cases) {
        EXPECT_EQ(reference_run(item.graph, item.choices).outcome, Outcome::Stalled) << item.name;
        auto engine = make_engine(item.graph);
        ASSERT_NE(engine, nullptr) << item.name;
        RunConfig config;
        config.input     = json{{"script", item.script}};
        config.max_steps = 8;
        const auto result = engine->run(config);
        EXPECT_EQ(result.execution_trace.size(), item.trace_size) << item.name;
#if NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS
        EXPECT_EQ(engine_outcome(result), Outcome::Stalled) << item.name;
#else
        EXPECT_EQ(engine_outcome(result), Outcome::Completed)
            << item.name << ": stall detection landed; flip the switch";
#endif
    }
}
