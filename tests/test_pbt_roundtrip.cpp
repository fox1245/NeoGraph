// M3 property-based harness (issue #75): schema-typed topology
// generator + canon round-trip property + seeded-bug mutation detection
// + scheduler-vs-reference-model differential execution.
//
// Why generation and not just fixtures: the v0.1.0–v0.1.7 regression
// survived seven releases precisely because no hand-written test
// exercised the dropped feature. The generator measures its own
// feature coverage (conditional_edges / barrier / interrupt occurrence
// rates must each clear 30%) so "the generator never touched it" is
// itself a test failure, not a silent gap.
//
// Everything is seeded std::mt19937 — deterministic across runs and
// platforms (no Date.now()-style flakiness).

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

class PbtNoopNode : public GraphNode {
public:
    explicit PbtNoopNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void ensure_pbt_types() {
    NodeFactory::instance().register_type(
        "pnoop",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<PbtNoopNode>(name);
        });
}

// ---------------------------------------------------------------------
// Generator: random *valid strict* topology from the schema envelope.
// Discipline mirrors the validator's error set — generated graphs must
// compile strictly (validator errors would throw), while warnings
// (E7/E9/E11 lint) are acceptable.
// ---------------------------------------------------------------------

struct GenStats {
    int total = 0, with_ce = 0, with_barrier = 0, with_interrupt = 0;
};

struct GenGraph {
    json def;
    // Nodes that carry a conditional edge (route preempts plain edges).
    std::set<std::string> ce_nodes;
    std::vector<std::string> node_names;
};

GenGraph generate_topology(std::mt19937& rng, GenStats& stats) {
    auto chance = [&](int pct) {
        return std::uniform_int_distribution<int>(0, 99)(rng) < pct;
    };
    auto pick = [&](size_t n) {
        return std::uniform_int_distribution<size_t>(0, n - 1)(rng);
    };

    GenGraph g;
    const int n_nodes = 2 + static_cast<int>(pick(6));   // 2..7
    for (int i = 0; i < n_nodes; ++i) {
        g.node_names.push_back("n" + std::to_string(i));
    }

    json nodes = json::object();
    for (const auto& n : g.node_names) nodes[n] = json{{"type", "pnoop"}};

    // Channels: __route__ always (route_channel reads it) + extras.
    json channels = json::object();
    channels["__route__"] = json{{"reducer", "overwrite"}};
    const int extra_ch = static_cast<int>(pick(3));
    for (int i = 0; i < extra_ch; ++i) {
        json ch = json{{"reducer", chance(50) ? "append" : "overwrite"}};
        if (chance(40)) ch["initial"] = static_cast<int>(pick(100));
        channels["ch" + std::to_string(i)] = std::move(ch);
    }

    // Plain-edge spine guarantees reachability: __start__ -> n0 -> ... .
    std::vector<std::pair<std::string, std::string>> edges;
    edges.push_back({"__start__", g.node_names[0]});
    for (int i = 0; i + 1 < n_nodes; ++i) {
        edges.push_back({g.node_names[i], g.node_names[i + 1]});
    }
    edges.push_back({g.node_names.back(), "__end__"});

    // Random extra plain edges (may create cycles — E11 is a warning).
    const int extra_e = static_cast<int>(pick(3));
    for (int i = 0; i < extra_e; ++i) {
        edges.push_back({g.node_names[pick(g.node_names.size())],
                         g.node_names[pick(g.node_names.size())]});
    }

    // Conditional edge: route_channel with full known-label coverage
    // ("default" included — E10-clean). The source keeps its plain
    // edges in the JSON; the scheduler ignores them (route preempts),
    // which the reference model must mirror.
    json conditional_edges = json::array();
    if (chance(60)) {
        const auto& src = g.node_names[pick(g.node_names.size())];
        g.ce_nodes.insert(src);
        json routes = json::object();
        routes["default"] = chance(50) ? "__end__"
                                       : g.node_names[pick(g.node_names.size())];
        const int n_keys = 1 + static_cast<int>(pick(2));
        for (int k = 0; k < n_keys; ++k) {
            routes["k" + std::to_string(k)] =
                chance(30) ? "__end__" : g.node_names[pick(g.node_names.size())];
        }
        conditional_edges.push_back(json{
            {"from", src}, {"condition", "route_channel"}, {"routes", routes}});
        stats.with_ce++;
    }

    // Barrier: force a fan-out + AND-join structure so wait_for members
    // provably signal the barrier (E8-clean). Members must not be
    // ce_nodes (their plain edges into the join would be preempted).
    if (chance(55) && n_nodes >= 3) {
        std::vector<std::string> cands;
        for (const auto& n : g.node_names) {
            if (!g.ce_nodes.count(n)) cands.push_back(n);
        }
        if (cands.size() >= 3) {
            const std::string join = cands[0], u1 = cands[1], u2 = cands[2];
            if (join != u1 && join != u2 && u1 != u2) {
                edges.push_back({u1, join});
                edges.push_back({u2, join});
                nodes[join] = json{{"type", "pnoop"},
                                   {"barrier", {{"wait_for", json::array({u1, u2})}}}};
                stats.with_barrier++;
            }
        }
    }

    json def = json::object();
    def["schema_version"] = 1;
    def["name"] = "pbt";
    def["channels"] = std::move(channels);
    def["nodes"] = std::move(nodes);
    json earr = json::array();
    for (const auto& [f, t] : edges) earr.push_back(json{{"from", f}, {"to", t}});
    def["edges"] = std::move(earr);
    if (!conditional_edges.empty()) def["conditional_edges"] = std::move(conditional_edges);

    if (chance(45)) {
        def["interrupt_before"] =
            json::array({g.node_names[pick(g.node_names.size())]});
        stats.with_interrupt++;
    }

    stats.total++;
    g.def = std::move(def);
    return g;
}

constexpr int kSeeds = 300;

} // namespace

// =========================================================================
// Property 1: every generated strict topology compiles, validates
// without errors, and round-trips through canon. Coverage instrumented.
// =========================================================================

TEST(PbtRoundTrip, GeneratedCorpusCompilesValidatesAndRoundTrips) {
    ensure_pbt_types();
    GenStats stats;
    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937 rng(seed);
        auto g = generate_topology(rng, stats);

        CompiledGraph cg;
        ASSERT_NO_THROW(cg = GraphCompiler::compile(g.def, NodeContext{}))
            << "seed " << seed << "\n" << g.def.dump(2);

        auto report = GraphValidator::validate(cg);
        ASSERT_FALSE(report.has_errors())
            << "seed " << seed << "\n" << report.summary()
            << "\n" << g.def.dump(2);

        ASSERT_NO_THROW(GraphCompiler::verify_roundtrip(g.def, cg))
            << "seed " << seed << "\n" << g.def.dump(2);
    }

    // Feature-coverage gate: "the generator never touched it" must be
    // a loud failure, not a silent hole (the v0.1.x lesson).
    const double ce_pct        = 100.0 * stats.with_ce / stats.total;
    const double barrier_pct   = 100.0 * stats.with_barrier / stats.total;
    const double interrupt_pct = 100.0 * stats.with_interrupt / stats.total;
    EXPECT_GE(ce_pct, 30.0)        << "conditional_edges coverage collapsed";
    EXPECT_GE(barrier_pct, 30.0)   << "barrier coverage collapsed";
    EXPECT_GE(interrupt_pct, 30.0) << "interrupt coverage collapsed";
}

// =========================================================================
// Property 2: seeded-bug mutation detection. Eight mutator classes —
// five drops, three rewires — applied to the *compiled* graph; the
// translation validator must catch every single application.
// =========================================================================

TEST(PbtMutation, AllSeededBugsAreDetected) {
    ensure_pbt_types();
    std::map<std::string, int> applied;

    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937 rng(seed);
        GenStats ignore;
        auto g = generate_topology(rng, ignore);
        const auto compile_fresh = [&]() {
            return GraphCompiler::compile(g.def, NodeContext{});
        };

        // drop 1: conditional_edges wholesale (the historical bug).
        {
            auto cg = compile_fresh();
            if (!cg.conditional_edges.empty()) {
                cg.conditional_edges.clear();
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["drop_conditional_edges"]++;
            }
        }
        // drop 2: one plain edge.
        {
            auto cg = compile_fresh();
            if (!cg.edges.empty()) {
                cg.edges.erase(cg.edges.begin()
                               + static_cast<long>(seed % cg.edges.size()));
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["drop_edge"]++;
            }
        }
        // drop 3: barrier specs.
        {
            auto cg = compile_fresh();
            if (!cg.barrier_specs.empty()) {
                cg.barrier_specs.clear();
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["drop_barrier"]++;
            }
        }
        // drop 4: interrupt sets.
        {
            auto cg = compile_fresh();
            if (!cg.interrupt_before.empty()) {
                cg.interrupt_before.clear();
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["drop_interrupt"]++;
            }
        }
        // drop 5: one channel (always applicable — __route__ exists).
        {
            auto cg = compile_fresh();
            if (!cg.channel_defs.empty()) {
                cg.channel_defs.pop_back();
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["drop_channel"]++;
            }
        }
        // rewire 6: collapse all routes of a conditional edge onto one
        // target (edge survives; wiring is wrong — presence-only
        // comparison would miss this).
        {
            auto cg = compile_fresh();
            for (auto& ce : cg.conditional_edges) {
                if (ce.routes.size() < 2) continue;
                std::set<std::string> targets;
                for (const auto& [k, v] : ce.routes) targets.insert(v);
                if (targets.size() < 2) continue;
                const std::string first = ce.routes.begin()->second;
                for (auto& [k, v] : ce.routes) v = first;
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["rewire_routes"]++;
                break;
            }
        }
        // rewire 7: retarget one plain edge to a different node.
        {
            auto cg = compile_fresh();
            if (!cg.edges.empty() && g.node_names.size() >= 2) {
                auto& e = cg.edges[seed % cg.edges.size()];
                for (const auto& cand : g.node_names) {
                    if (cand != e.to) { e.to = cand; break; }
                }
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["rewire_edge"]++;
            }
        }
        // rewire 8: rename a node in the re-emission source (one node
        // dropped + one fabricated — the counting-oracle blind spot;
        // identity mapping must catch it).
        {
            auto cg = compile_fresh();
            if (!cg.node_defs.empty()) {
                auto it = cg.node_defs.begin();
                json def_copy = it->second;
                std::string old_name = it->first;
                cg.node_defs.erase(it);
                cg.node_defs[old_name + "_impostor"] = std::move(def_copy);
                EXPECT_THROW(GraphCompiler::verify_roundtrip(g.def, cg),
                             std::runtime_error) << "seed " << seed;
                applied["rename_node"]++;
            }
        }
    }

    // Every mutator class must have fired on a healthy share of the
    // corpus — a mutator that never applies is a hole in the harness.
    for (const auto& [name, count] : applied) {
        EXPECT_GE(count, kSeeds / 10) << "mutator '" << name
                                      << "' applied too rarely";
    }
    EXPECT_EQ(applied.size(), 8u);
}

// =========================================================================
// Property 3: differential execution — Scheduler vs an independent
// reference model of the documented super-step semantics (DESIL
// lesson: verifiers alone don't catch wrong-execution bugs).
// =========================================================================

namespace {

// Independent reimplementation of the documented dispatch contract.
// Deliberately shares no code with Scheduler.
struct RefModel {
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> ces;
    BarrierSpecs barriers;
    BarrierState accum;

    std::vector<std::string> start() const {
        std::set<std::string> s;
        for (const auto& e : edges) {
            if (e.from == "__start__" && e.to != "__end__") s.insert(e.to);
        }
        return {s.begin(), s.end()};
    }

    // route_value: what the route_channel condition would return.
    std::pair<std::vector<std::string>, bool> step(
        const std::vector<StepRouting>& routings,
        const std::string& route_value) {

        bool hit_end = false;

        // Command.goto: last writer wins, preempts everything.
        std::optional<std::string> goto_target;
        for (const auto& r : routings) {
            if (r.command_goto) goto_target = r.command_goto;
        }
        if (goto_target) {
            if (*goto_target == "__end__") return {{}, true};
            return {{*goto_target}, false};
        }

        std::map<std::string, std::set<std::string>> signals;
        for (const auto& r : routings) {
            std::vector<std::string> succ;
            const ConditionalEdge* ce = nullptr;
            for (const auto& c : ces) {
                if (c.from == r.node_name) { ce = &c; break; }
            }
            if (ce) {
                auto it = ce->routes.find(route_value);
                succ.push_back(it != ce->routes.end()
                                   ? it->second
                                   : ce->routes.rbegin()->second);
            } else {
                for (const auto& e : edges) {
                    if (e.from == r.node_name) succ.push_back(e.to);
                }
                if (succ.empty()) succ.push_back("__end__");
            }
            for (const auto& s : succ) {
                if (s == "__end__") hit_end = true;
                else signals[s].insert(r.node_name);
            }
        }

        std::set<std::string> ready;
        for (const auto& [target, signalers] : signals) {
            auto bit = barriers.find(target);
            if (bit == barriers.end()) { ready.insert(target); continue; }
            auto& acc = accum[target];
            for (const auto& s : signalers) acc.insert(s);
            if (std::includes(acc.begin(), acc.end(),
                              bit->second.begin(), bit->second.end())) {
                ready.insert(target);
                acc.clear();
            }
        }
        return {{ready.begin(), ready.end()}, hit_end};
    }
};

} // namespace

TEST(PbtDifferential, SchedulerMatchesReferenceModel) {
    ensure_pbt_types();
    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937 rng(seed + 100000);   // distinct stream from gen
        GenStats ignore;
        auto g = generate_topology(rng, ignore);
        auto cg = GraphCompiler::compile(g.def, NodeContext{});

        Scheduler sch(cg.edges, cg.conditional_edges, cg.barrier_specs);
        RefModel ref{cg.edges, cg.conditional_edges, cg.barrier_specs, {}};
        BarrierState bstate;

        // Same start set.
        auto actual_start = sch.plan_start_step();
        auto ref_start = ref.start();
        std::sort(actual_start.begin(), actual_start.end());
        EXPECT_EQ(actual_start, ref_start) << "seed " << seed;

        // Random route values each super-step, junk value included so
        // the lexicographically-last fallback path is exercised too.
        std::vector<std::string> route_pool = {"default", "k0", "k1", "zzz_junk"};
        auto ready = actual_start;
        for (int step = 0; step < 12 && !ready.empty(); ++step) {
            const auto& route_value =
                route_pool[std::uniform_int_distribution<size_t>(
                    0, route_pool.size() - 1)(rng)];

            GraphState state;
            state.init_channel("__route__", ReducerType::OVERWRITE, nullptr,
                               json(route_value));

            std::vector<StepRouting> routings;
            for (const auto& n : ready) {
                StepRouting r;
                r.node_name = n;
                // Occasional Command.goto to exercise the override path.
                if (std::uniform_int_distribution<int>(0, 99)(rng) < 8) {
                    r.command_goto =
                        (std::uniform_int_distribution<int>(0, 1)(rng) == 0)
                            ? std::string("__end__")
                            : g.node_names[std::uniform_int_distribution<size_t>(
                                  0, g.node_names.size() - 1)(rng)];
                }
                routings.push_back(std::move(r));
            }

            auto plan = sch.plan_next_step(routings, state, bstate);
            auto [ref_ready, ref_end] = ref.step(routings, route_value);

            auto actual = plan.ready;
            std::sort(actual.begin(), actual.end());
            ASSERT_EQ(actual, ref_ready)
                << "seed " << seed << " step " << step
                << " route=" << route_value << "\n" << g.def.dump(2);
            ASSERT_EQ(plan.hit_end, ref_end)
                << "seed " << seed << " step " << step;

            if (plan.hit_end) break;
            ready = actual;
        }
    }
}
