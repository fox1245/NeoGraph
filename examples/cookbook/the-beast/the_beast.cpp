// NeoGraph Cookbook — "The Beast"
// =================================================================
// A self-evolving agent that WRITES its own harness, EVOLVES it under
// the DSL compiler as fitness oracle, and can ROLL BACK a running
// harness to any prior super-step via the checkpointer.
//
// Three real capabilities, all offline & deterministic (no API key):
//
//   ACT I  — GENERATE + GATE.  A harness is authored in the DSL surface
//            (vars / templates / use) and forced through the three
//            coherence gates before anything runs:
//              1. Elaborator::elaborate   surface → core lockfile
//              2. GraphCompiler::compile   strict (consumed-key) + TV
//                 + verify_roundtrip
//              3. GraphValidator::validate static semantics (E3/E8/E10…)
//            A harness that fails any gate is DISCARDED.
//
//   ACT II — EVOLVE.  neograph::graph::evolve() runs REAL mutation
//            operators (swap/add/remove template use, tune params,
//            toggle barrier/conditional-edge, add/remove edge) over the
//            seed. Every offspring passes the compile gate FIRST — the
//            compiler is the fitness function, so invalid offspring die
//            for free, without execution. The run emits a diffable
//            genealogy (lockfile lineage) — that lineage IS the rollback
//            surface at the *evolutionary* scale.
//
//   ACT III— ROLL BACK.  The surviving harness is spawned with an
//            InMemoryCheckpointStore attached. The engine snapshots
//            state every super-step; store->list()/load_by_id() are
//            genuine time-travel — we roll the run back to an earlier
//            step and read the restored channel state.
//
// This is the whole monster: it generates harnesses, proves them
// coherent, evolves them under that proof, and rewinds their execution.
//
// Build:  cmake --build build --target cookbook_the_beast
// Run:    ./build/cookbook_the_beast

#include <neograph/neograph.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/evolution.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <iostream>
#include <sstream>
#include <string>

using neograph::json;
namespace ng = neograph::graph;

// -----------------------------------------------------------------
// A deterministic stub node. Every node the Beast wires is one of
// these: it appends its own name to the "trail" channel so each
// super-step's checkpoint carries visibly different state (that is what
// makes the Act III rollback observable). No provider, no network.
// -----------------------------------------------------------------
struct BeastNode : ng::GraphNode {
    std::string name_;
    explicit BeastNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput out;
        out.writes.push_back({"trail", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

void register_beast_node() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "beast_node",
            [](const std::string& name, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new BeastNode(name));
            },
            json::object(),
            // Channel-effect contract: this node writes "trail". Declaring
            // it lets GraphValidator's effect checks (E4/E6) run — the
            // harness is coherent down to which channels each node touches.
            json::parse(R"({"reads":[],"writes":["trail"]})"));
        return true;
    }();
    (void)once;
}

// -----------------------------------------------------------------
// The seed harness — authored entirely in the DSL surface. One `stage`
// template instantiated three times via `use`; elaboration expands it
// into a 3-node core chain (s1_n → s2_n → s3_n).
// -----------------------------------------------------------------
json seed_harness() {
    return {
        {"schema_version", 1},
        {"name", "beast_seed"},
        {"_comment", "authored by the Beast in the DSL surface"},
        {"channels", {{"trail", {{"reducer", "append"}}}}},
        {"templates", {
            {"stage", {
                {"params", json::array({"id"})},
                {"nodes", {{"n", {{"type", "beast_node"}, {"x-id", "@{id}"}}}}}
            }}
        }},
        {"nodes", json::object()},
        {"use", json::array({
            {{"template", "stage"}, {"prefix", "s1"}, {"args", {{"id", "1"}}}},
            {{"template", "stage"}, {"prefix", "s2"}, {"args", {{"id", "2"}}}},
            {{"template", "stage"}, {"prefix", "s3"}, {"args", {{"id", "3"}}}}
        })},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "s1_n"}},
            {{"from", "s1_n"}, {"to", "s2_n"}},
            {{"from", "s2_n"}, {"to", "s3_n"}},
            {{"from", "s3_n"}, {"to", "__end__"}}
        })}
    };
}

// -----------------------------------------------------------------
// The three coherence gates. Returns the validated core lockfile, or the
// rejecting gate + report.
// -----------------------------------------------------------------
// Serialized graph state is channel-wrapped: {"channels":{"<name>":
// {"value": ..., "version": N}}}. Pull one channel's value out.
json channel_of(const json& serialized, const std::string& name) {
    if (serialized.contains("channels") && serialized["channels"].contains(name))
        return serialized["channels"][name].value("value", json::array());
    return json::array();
}

struct Verdict { bool ok = false; std::string gate, report; json core; };

Verdict forge(const json& dsl, const ng::NodeContext& ctx) {
    json core;
    try { core = ng::Elaborator::elaborate(dsl).core; }          // Gate 1
    catch (const std::exception& e) { return {false, "elaborate", e.what(), {}}; }
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);          // Gate 2
        ng::GraphCompiler::verify_roundtrip(core, cg);
        auto rep = ng::GraphValidator::validate(cg);              // Gate 3
        if (rep.has_errors()) return {false, "validate", rep.summary(), {}};
        return {true, "accepted", {}, core};
    } catch (const std::exception& e) { return {false, "compile", e.what(), {}}; }
}

int main() {
    register_beast_node();
    std::cout << "================ THE BEAST ================\n"
                 "Generates harnesses · evolves them under the compiler ·\n"
                 "rolls their execution back through the checkpointer.\n\n";

    ng::NodeContext ctx;  // no provider needed — stub nodes are deterministic

    // ================= ACT I — GENERATE + GATE =================
    std::cout << "── ACT I · generate a harness, prove it coherent ──\n";
    const json dsl = seed_harness();
    const Verdict v = forge(dsl, ctx);
    if (!v.ok) {
        std::cout << "  REJECTED at gate '" << v.gate << "':\n" << v.report << "\n";
        return 1;
    }
    const json seed_core = v.core;
    std::cout << "  ACCEPTED — 3 gates passed. Core lockfile nodes: ";
    for (auto it = seed_core["nodes"].begin(); it != seed_core["nodes"].end(); ++it)
        std::cout << it.key() << " ";
    std::cout << "\n  (DSL surface expanded away: vars/templates/use gone.)\n\n";

    // ================= ACT II — EVOLVE =================
    // Real mutation operators + compile-gate selection + genealogy.
    std::cout << "── ACT II · evolve the harness (compiler = fitness) ──\n";
    ng::Task task;                       // structural evolution: fitness = coherence
    task.name = "beast";
    task.reference_core = seed_core;

    ng::EvolutionConfig cfg;
    cfg.offspring_per_gen = 24;
    cfg.survivors_per_gen = 6;
    cfg.max_generations   = 4;
    cfg.seed              = 42;          // deterministic
    cfg.run_evaluation    = false;       // gate-only selection (no execution)

    const ng::EvolutionResult evo = ng::evolve(seed_core, task, cfg);
    std::cout << "  generations: " << cfg.max_generations
              << " · offspring: " << evo.total_offspring
              << " · survived compile gate: " << evo.compile_passed
              << " · rejected (invalid, never run): "
              << (evo.total_offspring - evo.compile_passed) << "\n";
    // The mutation space is the DSL (M4), not raw JSON, so offspring are
    // valid by construction — the gate is the safety net, and the reject
    // count is itself a health metric on the operators. Show real variety:
    std::cout << "  sample mutations that produced offspring:\n";
    int shown = 0;
    for (const auto& ind : evo.population) {
        if (ind.parent_index < 0 || ind.mutation_description.empty()) continue;
        std::cout << "    gen " << ind.generation << ": " << ind.mutation_description
                  << "  →  " << ind.core["nodes"].size() << " nodes\n";
        if (++shown >= 5) break;
    }
    std::cout << "  (full diffable lineage via to_json(result) — the evolutionary "
                 "rollback surface: git-commit it, diff it, revert a generation.)\n\n";

    // ================= ACT III — ROLL BACK =================
    // Spawn a validated harness with a checkpointer; rewind its run.
    std::cout << "── ACT III · spawn + roll back through the checkpointer ──\n";
    auto store = std::make_shared<ng::InMemoryCheckpointStore>();
    auto engine = ng::GraphEngine::build(
        seed_core, ng::EngineConfig{.node_context = ctx, .checkpoint_store = store});

    ng::RunConfig run_cfg;
    run_cfg.thread_id = "beast-run";
    run_cfg.input = {{"trail", json::array()}};
    auto result = engine->run(run_cfg);

    json final_trail = result.has_channel("trail") ? result.channel<json>("trail") : json::array();
    std::cout << "  ran to completion, trail = " << final_trail.dump() << "\n";

    auto history = store->list("beast-run");   // newest-first
    std::cout << "  checkpoint timeline (" << history.size() << " snapshots):\n";
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        std::cout << "    step " << it->step << "  id=" << it->id.substr(0, 8)
                  << "  trail=" << channel_of(it->channel_values, "trail").dump() << "\n";
    }

    if (history.size() >= 2) {
        // Roll back: pick an earlier checkpoint (second-oldest) and load it.
        const auto& earlier = history[history.size() - 2];
        auto restored = store->load_by_id(earlier.id);
        std::cout << "  >> ROLLBACK to step " << earlier.step
                  << " (id=" << earlier.id.substr(0, 8) << ")\n";
        if (restored) {
            std::cout << "     final trail was " << final_trail.dump()
                      << "; restored trail = "
                      << channel_of(restored->channel_values, "trail").dump()
                      << "  (later steps gone — real time-travel, not a replay)\n";
        }
    }

    std::cout << "\nGenerated. Evolved. Rewound. The Beast remains.\n";
    return 0;
}
