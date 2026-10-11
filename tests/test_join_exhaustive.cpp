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
//  6. A node that fails and has a failure edge (#364) routes to its failure
//     target only; its own routes are not taken. The target is never a join
//     and never __end__, so a failure arrival never fills a slot (a handler
//     that runs may stand in for the failed member). Command.goto preempts a
//     failure route like every other route (rule 3).
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
// slots. At most one node may fail, along a failure edge to a node that is
// neither itself, the join, nor an implicit end (the compiler's target rules).
// A failure edge is combined with a Command.goto ender on graphs of up to
// three nodes (ci) or four (extended), engine runs combine them on two-node
// graphs only, and an engine path fails at most once.
// NEOGRAPH_JOIN_MODEL_FAILERS=0 leaves failure edges out of the family.
// Duplicates up to node renaming are kept. NEOGRAPH_JOIN_MODEL_NODES,
// _ENGINE_NODES and _STEPS override single limits.
//
// Two switches:
//
//  - NEOGRAPH_JOIN_MODEL_ANY_OF (default 1 since #363 WP4): any_of slot graphs
//    are parsed by the Core and compared at the scheduler and engine levels.
//    0 runs them in the reference only, as before the Core knew slots.
//  - NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS (default 1): the engine reports
//    a stalled run as RunStatus::BarrierStalled with a barrier_stalled witness
//    (#357), so every engine run, stalled ones included, has to match the
//    reference, and a stall's witness has to name the signals the reference
//    leaves waiting. Setting it to 0 counts a run that stalls in the reference
//    but ends Completed in the engine as a known disagreement instead of a
//    mismatch; that is only for measuring a base without stall detection.

#include <neograph/graph/compiler.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/validator.h>

#include "join_model.h"

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
#define NEOGRAPH_JOIN_MODEL_ANY_OF 1
#endif
#ifndef NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS
#define NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS 1
#endif

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace join_model;

/// The shared bound, with the #364 failure routes the harness also checks
/// (NEOGRAPH_JOIN_MODEL_FAILERS=0 leaves them out).
Bounds harness_bounds() {
    auto limits    = bounds();
    limits.failers = env_int("NEOGRAPH_JOIN_MODEL_FAILERS", 1) != 0;
    return limits;
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
        // A failed visit leaves no event, only its failure record.
        for (const auto& failure : input.state.get("failures"))
            if (failure["node"].get<std::string>() == name_) ++visit;
        const auto choice = input.state.get("script")[name_][visit].get<std::string>();
        if (choice == "x") throw std::runtime_error("scripted failure of " + name_);
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
std::unique_ptr<Scheduler> scheduler_for(const TopologySpec& topology) {
    std::map<std::string, std::string> failure_targets;
    for (const auto& edge : topology.failure_edges) failure_targets[edge.from] = edge.to;
    return std::make_unique<Scheduler>(topology.edges, topology.conditional_edges,
                                       topology.barrier_specs, topology.join_specs,
                                       std::move(failure_targets), nullptr);
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
        std::vector<bool>         failed;
        for (int u = 0; u < graph_.n; ++u) {
            if (!(ready & bit(u))) continue;
            if (graph_.routes[u].conditional)
                labels.push_back(ChannelWrite{"route_" + node_name(u),
                                              json((choice.label_b & bit(u)) ? "b" : "a")});
            StepRouting routing{node_name(u), std::nullopt};
            if (choice.end & bit(u)) routing.command_goto = "__end__";
            routings.push_back(std::move(routing));
            failed.push_back((choice.fail & bit(u)) != 0);
        }
        if (!labels.empty()) state_.apply_writes(labels);
        BarrierState barrier;
        const auto   join = node_name(graph_.join());
        if (pending) barrier[join] = names(pending);
        const auto plan = choice.fail ? scheduler_->plan_next_step(routings, state_, barrier, failed)
                                      : scheduler_->plan_next_step(routings, state_, barrier);

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
    std::uint64_t graphs = 0, legacy_graphs = 0, any_of_graphs = 0, failure_graphs = 0;
    std::uint64_t states = 0, transitions = 0;
    std::uint64_t completed = 0, stalled = 0, explicit_end_completions = 0;
    std::uint64_t cyclic_graphs = 0, late_alternative_stalls = 0;
    std::uint64_t engine_graphs = 0, engine_skipped_invalid = 0, engine_runs = 0;
    std::uint64_t engine_stalls = 0;
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


std::string describe_step(const Graph& graph, std::uint32_t ready, std::uint32_t pending,
                          Choice choice) {
    std::ostringstream out;
    out << "graph " << describe(graph) << "\n  ready " << mask_names(ready, graph.n)
        << " pending " << mask_names(pending, graph.n) << " label_b "
        << mask_names(choice.label_b, graph.n) << " end " << mask_names(choice.end, graph.n)
        << " fail " << mask_names(choice.fail, graph.n);
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
    // The failer fails at most once per path: enough to drive every failure
    // route through the engine without multiplying looping paths.
    bool failed = false;
    std::function<void(std::uint32_t, std::uint32_t)> walk = [&](std::uint32_t ready,
                                                                  std::uint32_t pending) {
        if (static_cast<int>(path.ready_sets.size()) == max_steps) {
            path.outcome = Outcome::StepLimit;
            visit(path);
            return;
        }
        path.ready_sets.push_back(ready);
        for_each_choice(graph, ready, [&](Choice choice) {
            if (choice.fail && failed) return;
            const bool failing = choice.fail != 0;
            if (failing) failed = true;
            for (int u = 0; u < graph.n; ++u) {
                if (!(ready & bit(u))) continue;
                std::string entry = graph.routes[u].conditional
                                        ? ((choice.label_b & bit(u)) ? "b" : "a")
                                        : "-";
                if (choice.end & bit(u)) entry += "!";
                if (choice.fail & bit(u)) entry = "x";
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
            if (failing) failed = false;
        });
        path.ready_sets.pop_back();
    };
    walk(bit(0), 0);
}

/// How the engine reports a run: RunResult::status(), where a stalled run is
/// RunStatus::BarrierStalled (#357).
Outcome engine_outcome(const RunResult& result) {
    switch (result.status()) {
        case RunStatus::Completed:
            return Outcome::Completed;
        case RunStatus::BarrierStalled:
            return Outcome::Stalled;
        case RunStatus::StepLimit:
            return Outcome::StepLimit;
        default:
            return Outcome::Unexpected;
    }
}

/// The signals a stalled run's witness says the join still holds, or nullopt
/// when the witness does not list exactly the model's one join.
std::optional<std::uint32_t> stalled_signals(const Graph& graph, const RunResult& result) {
    const auto& stalls = result.output["_neograph"]["barrier_stalled"];
    if (!stalls.is_array() || stalls.size() != 1) return std::nullopt;
    const auto& stall = stalls[0];
    if (stall.value("node", "") != node_name(graph.join())) return std::nullopt;
    std::uint32_t received = 0;
    for (const auto& signaler : stall["received"])
        received |= bit(std::stoi(signaler.get<std::string>().substr(1)));
    return received;
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
        const auto mark = [&](std::size_t step, const std::string& name) {
            if (ready_sets.size() <= step) ready_sets.resize(step + 1, 0);
            ready_sets[step] |= bit(std::stoi(name.substr(1)));
        };
        for (const auto& event : result.output["channels"]["events"]["value"])
            mark(event[0].get<std::size_t>(), event[1].get<std::string>());
        // A failed node ran in its super-step too; its record says which.
        for (const auto& failure : result.output["channels"]["failures"]["value"])
            mark(failure["step"].get<std::size_t>(), failure["node"].get<std::string>());
        const auto actual = engine_outcome(result);
        if (path.outcome == Outcome::StepLimit) ++totals.engine_step_limits;
        if (path.outcome == Outcome::Stalled) ++totals.engine_stalls;
        const bool same_trace = ready_sets == path.ready_sets;
        std::string witness_problem;
        if (same_trace && actual == Outcome::Stalled && path.outcome == Outcome::Stalled) {
            const auto received = stalled_signals(graph, result);
            if (received != path.final_pending)
                witness_problem = "\n  barrier_stalled witness " +
                                  result.output["_neograph"]["barrier_stalled"].dump() +
                                  ", reference waiting " +
                                  mask_names(path.final_pending, graph.n);
        }
        if (same_trace && actual == path.outcome && witness_problem.empty()) return;
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
        out << witness_problem;
        totals.fail(out.str());
    });
}

void report(const char* title, const Bounds& limits, const Totals& totals, double seconds) {
    std::cout << "[ join model ] " << title << " (bound " << limits.name << ": nodes<="
              << limits.max_nodes << ", conditionals<=" << limits.max_conditionals
              << ", fan-out<=" << limits.max_fanout << ", back edges<=" << limits.max_back_edges
              << ", enders<=" << (limits.enders ? 1 : 0) << ", steps<=" << limits.max_steps
              << ")\n  graphs " << totals.graphs << " (legacy " << totals.legacy_graphs
              << ", any_of " << totals.any_of_graphs << "; with a failure edge "
              << totals.failure_graphs << ")\n";
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
                  << "), step limits " << totals.engine_step_limits << ", stalls "
                  << totals.engine_stalls
                  << (NEOGRAPH_JOIN_MODEL_ENGINE_DETECTS_STALLS ? " (all compared)" : "")
                  << "\n";
    if (totals.engine_known_stall_disagreements)
        std::cout << "  known #357 disagreements (reference stalls, engine completes) "
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

    // #364, rule 6: B fails instead of picking a label, along a failure edge to
    // FIX. A failure arrival never fills a slot: with the legacy barrier the run
    // stalls; with [{any_of: ["B", "FIX"]}, "C"] FIX stands in for B.
    const Choice b_fails{0, 0, bit(1)};
    auto legacy_failing = legacy;
    legacy_failing.failer      = 1;
    legacy_failing.fail_target = 3;
    const auto failed_legacy = reference_run(legacy_failing, {none, b_fails, none, none});
    EXPECT_EQ(failed_legacy.outcome, Outcome::Stalled);
    EXPECT_EQ(failed_legacy.ready_sets,
              (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3)}));
    auto slot_failing = slot;
    slot_failing.failer      = 1;
    slot_failing.fail_target = 3;
    const auto failed_slot = reference_run(slot_failing, {none, b_fails, none, none, none});
    EXPECT_EQ(failed_slot.outcome, Outcome::Completed);
    EXPECT_EQ(failed_slot.ready_sets,
              (std::vector<std::uint32_t>{bit(0), bit(1) | bit(2), bit(3), bit(4)}));
    // A static fan-out node that fails takes none of its edges.
    EXPECT_EQ(reference_step(legacy_failing, bit(1), 0, b_fails).ready, bit(3));
    // Command.goto(__end__) by a sibling preempts the failure route.
    auto ending_sibling = legacy_failing;
    ending_sibling.ender = 2;
    const auto preempted = reference_step(ending_sibling, bit(1) | bit(2), 0, Choice{0, bit(2), bit(1)});
    EXPECT_EQ(preempted.ready, 0U);
    EXPECT_TRUE(preempted.explicit_end);

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
    const auto limits = harness_bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (graph.any_of()) return;
        ++totals.graphs;
        if (graph.failer >= 0) ++totals.failure_graphs;
        ++totals.legacy_graphs;
        RealScheduler real(graph);
        explore(graph, &real, totals);
    });
    report("Scheduler::plan_next_step vs reference, legacy barriers", limits, totals,
           seconds_since(start));
    EXPECT_GT(totals.graphs, 1000U);
    if (limits.failers) EXPECT_GT(totals.failure_graphs, 1000U) << "the family must include failure edges (#364)";
    expect_clean(totals);
}

TEST(JoinModel, ReferenceInvariantsHoldForAnyOfSlots) {
    const auto limits = harness_bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (!graph.any_of()) return;
        ++totals.graphs;
        if (graph.failer >= 0) ++totals.failure_graphs;
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
    const auto limits = harness_bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (!graph.any_of()) return;
        ++totals.graphs;
        if (graph.failer >= 0) ++totals.failure_graphs;
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
    auto limits      = harness_bounds();
    limits.max_nodes = std::min(limits.max_nodes, limits.engine_max_nodes);
    // The scheduler level already combines failure edges with enders on
    // larger graphs; engine runs do so only on two-node graphs.
    limits.failers_with_enders_max_nodes = 2;
    const auto start = std::chrono::steady_clock::now();
    Totals     totals;
    for_each_graph(limits, [&](const Graph& graph) {
        if (graph.any_of() && !NEOGRAPH_JOIN_MODEL_ANY_OF) return;
        ++totals.graphs;
        if (graph.failer >= 0) ++totals.failure_graphs;
        (graph.any_of() ? totals.any_of_graphs : totals.legacy_graphs)++;
        run_engine(graph, limits, totals);
    });
    report("GraphEngine runs vs reference", limits, totals, seconds_since(start));
    EXPECT_GT(totals.engine_runs, 1000U);
    if (limits.failers) EXPECT_GT(totals.failure_graphs, 100U) << "engine runs must include failure edges (#364)";
    expect_clean(totals);
}

// #357 regression: runs that stall in the reference. The engine used to end
// them Completed; it now stops them as RunStatus::BarrierStalled (an Error
// invocation, a Failed Program run) with a witness naming the join, the
// signals it holds and the members it still misses.
TEST(JoinModel, Issue357StalledRunsFailBarrierStalled) {
    register_model_executables();
    struct Case {
        const char*         name;
        Graph               graph;
        json                script;
        std::vector<Choice> choices;
        std::size_t         trace_size;
        std::uint32_t       received;
        std::uint32_t       missing;
    };
    const Choice none{0, 0}, fail{bit(1), 0};
    const std::vector<Case> cases{
        // #357: A, B, C, FIX run; D waits for B and C and never runs. C and the
        // non-member FIX signaled it.
        {"issue 357, fail route", issue_357({bit(1), bit(2)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"b"})},
              {"n2", json::array({"-"})}, {"n3", json::array({"-"})}},
         {none, fail, none, none}, 4, bit(2) | bit(3), bit(1)},
        // A=n0 -> {B=n1, J=n3}; B routes "b" to X=n2, which has no successors (an
        // implicit end). J waits for A and B and keeps A's signal.
        {"member waiting after an implicit end",
         graph_from(4, {Route{false, {1, 3}}, Route{true, {3, 2}}, Route{false, {}},
                        Route{false, {kEnd}}},
                    {bit(0), bit(1)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"b"})}, {"n2", json::array({"-"})}},
         {none, fail, none}, 3, bit(0), bit(1)},
        // n0 -> n1; the join n1 waits for n0 and routes only to itself, so its
        // own (non-member) signal is left waiting.
        {"only a non-member signal waiting",
         graph_from(2, {Route{false, {1}}, Route{false, {1}}}, {bit(0)}),
         json{{"n0", json::array({"-"})}, {"n1", json::array({"-"})}}, {none, none}, 2, bit(1),
         bit(0)},
    };
    const auto names = [](std::uint32_t value, int n) {
        json result = json::array();
        for (int u = 0; u < n; ++u)
            if (value & bit(u)) result.push_back(node_name(u));
        return result;
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
        EXPECT_EQ(result.status(), RunStatus::BarrierStalled) << item.name;
        EXPECT_EQ(engine_outcome(result), Outcome::Stalled) << item.name;
        const auto& stalls = result.output["_neograph"]["barrier_stalled"];
        ASSERT_TRUE(stalls.is_array()) << item.name;
        ASSERT_EQ(stalls.size(), 1U) << item.name;
        EXPECT_EQ(stalls[0]["node"], node_name(item.graph.join())) << item.name;
        EXPECT_EQ(stalls[0]["received"], names(item.received, item.graph.n)) << item.name;
        EXPECT_EQ(stalls[0]["missing"], names(item.missing, item.graph.n)) << item.name;
    }
}
