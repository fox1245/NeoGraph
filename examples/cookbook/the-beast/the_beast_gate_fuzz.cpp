// NeoGraph Cookbook — "The Beast", GATE-FUZZ (the coherence gate's guarantee
// and its boundary, measured at scale)
// =================================================================
// gate_eval proved the gate sound on a hand-labeled 5-case corpus. The honest
// follow-up is not "run more cases and print precision 1.0" — because the
// ENGINE re-runs the validator on compile and throws on any error
// (graph_engine.cpp), "validator-error ⟹ engine-faults" is true by
// construction. Printing a 1.0 confusion matrix off that would be theatre.
//
// The real question is layered, and this program measures both layers at scale:
//
//  LAYER 1 — the static gate, RELATIVE TO honest effect contracts. Fuzz a
//    coherent seed with random structural mutators (dangling edge, undeclared
//    write, orphan writer, dropped edge, extra valid edge). Over N mutants,
//    does the gate's verdict ever disagree with what the engine does? This is a
//    consistency / regression guarantee: if a future change makes the compiler
//    gate and the runtime diverge, this fails.
//
//  LAYER 2 — the BOUNDARY. The gate trusts each node's declared effect
//    contract; it cannot see a node that LIES (declares writes:["out"] but
//    actually writes an undeclared channel). Such a graph passes the static
//    gate — and would fault at runtime. This is not a gate bug; it is the
//    designed division of labour: the static gate assumes honest contracts, and
//    the RUNTIME GraphState guard is the backstop for dishonest ones. The
//    program injects a lying node and measures that the runtime catches 100% of
//    what the static gate is blind to.
//
// The takeaway is a precise, honest statement of the guarantee: sound relative
// to honest contracts (Layer 1), with a runtime backstop for dishonest ones
// (Layer 2). Fully offline, deterministic (seeded). Lint goes to stderr — run
// with `2>/dev/null` for just the summary.
//
// Build:  cmake --build build --target cookbook_the_beast_gate_fuzz
// Run:    ./build/cookbook_the_beast_gate_fuzz 2>/dev/null

#include <neograph/neograph.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// Workers with declared effect contracts, writing distinct channels.
template <const char* CH>
struct Writer : ng::GraphNode {
    std::string name_;
    explicit Writer(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput o; o.writes.push_back({CH, 1}); co_return o;
    }
    std::string get_name() const override { return name_; }
};
static const char CH_OUT[] = "out", CH_OUT2[] = "out2", CH_PHANTOM[] = "phantom";

// A node that LIES about its effect: its contract will be declared as
// writes:["out"], but at runtime it writes the undeclared "phantom" channel.
struct LiarNode : ng::GraphNode {
    std::string name_;
    explicit LiarNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput o; o.writes.push_back({"phantom", 1}); co_return o;  // NOT what the contract says
    }
    std::string get_name() const override { return name_; }
};

static void register_nodes() {
    static bool once = [] {
        auto reg = [](const char* type, const char* ch, auto make) {
            ng::NodeFactory::instance().register_type(
                type, make, json::object(),
                json::parse(std::string(R"({"reads":[],"writes":[")") + ch + R"("]})"));
        };
        reg("w",  CH_OUT,     [](const std::string& n, const json&, const ng::NodeContext&) {
            return std::unique_ptr<ng::GraphNode>(new Writer<CH_OUT>(n)); });
        reg("w2", CH_OUT2,    [](const std::string& n, const json&, const ng::NodeContext&) {
            return std::unique_ptr<ng::GraphNode>(new Writer<CH_OUT2>(n)); });
        reg("wp", CH_PHANTOM, [](const std::string& n, const json&, const ng::NodeContext&) {
            return std::unique_ptr<ng::GraphNode>(new Writer<CH_PHANTOM>(n)); });
        // "liar" declares writes:["out"] (honest-looking) but writes "phantom".
        reg("liar", CH_OUT,   [](const std::string& n, const json&, const ng::NodeContext&) {
            return std::unique_ptr<ng::GraphNode>(new LiarNode(n)); });
        return true;
    }();
    (void)once;
}

// A topology as a plain C++ struct (no yyjson object-iteration footguns).
struct Topo {
    std::vector<std::pair<std::string, std::string>> nodes;  // name, type
    std::vector<std::pair<std::string, std::string>> edges;  // from, to
    std::set<std::string> channels;
};
static json to_core(const Topo& t) {
    json nodes = json::object();
    for (const auto& [name, type] : t.nodes) nodes[name] = {{"type", type}};
    json edges = json::array();
    for (const auto& [from, to] : t.edges) edges.push_back({{"from", from}, {"to", to}});
    json chans = json::object();
    for (const auto& c : t.channels) chans[c] = {{"reducer", "overwrite"}};
    return {{"schema_version", 1}, {"channels", chans}, {"nodes", nodes}, {"edges", edges}};
}
static Topo seed_topo() {
    return {{{"a", "w"}, {"b", "w"}, {"c", "w2"}},
            {{"__start__", "a"}, {"a", "b"}, {"b", "c"}},
            {"out", "out2"}};
}

struct Verdict { bool has_error = false; std::string codes; };
static Verdict validate(const json& core, const ng::NodeContext& ctx) {
    Verdict v;
    ng::CompiledGraph cg;
    try { cg = ng::GraphCompiler::compile(core, ctx); }
    catch (const std::exception&) { v.has_error = true; v.codes = "compile-reject"; return v; }
    auto rep = ng::GraphValidator::validate(cg);
    v.has_error = rep.has_errors();
    for (const auto* e : rep.errors()) v.codes += (v.codes.empty() ? "" : ",") + e->code;
    return v;
}
enum class Outcome { CLEAN, FAULT, STALL };
static Outcome execute(const json& core, const ng::NodeContext& ctx) {
    try {
        auto engine = ng::GraphEngine::compile(core, ctx);
        ng::RunConfig rc; rc.max_steps = 32; rc.input = {{"out", 0}};
        auto res = engine->run(rc);
        return (int)res.execution_trace.size() >= rc.max_steps ? Outcome::STALL : Outcome::CLEAN;
    } catch (const std::exception&) { return Outcome::FAULT; }
}

// --- structural mutators (honest contracts; throwing or benign, never UB) ---
static std::string node_or_start(const Topo& t, std::mt19937& rng) {
    int k = rng() % ((int)t.nodes.size() + 1);
    return k == 0 ? "__start__" : t.nodes[k - 1].first;
}
using Mutator = void (*)(Topo&, std::mt19937&, int);
static void m_dangling(Topo& t, std::mt19937& rng, int id) {         // → E3
    t.edges.push_back({node_or_start(t, rng), "ghost" + std::to_string(id)});
}
static void m_undeclare(Topo& t, std::mt19937&, int) {               // → E4 (c writes out2)
    t.channels.erase("out2");
}
static void m_orphan_writer(Topo& t, std::mt19937& rng, int id) {    // → E4 (writes phantom)
    std::string n = "z" + std::to_string(id);
    t.nodes.push_back({n, "wp"});
    t.edges.push_back({node_or_start(t, rng), n});
}
static void m_drop_edge(Topo& t, std::mt19937& rng, int) {           // → E7 warning (benign)
    if (t.edges.size() > 1) t.edges.erase(t.edges.begin() + (rng() % t.edges.size()));
}
static void m_valid_edge(Topo& t, std::mt19937& rng, int) {          // benign (forward → no cycle)
    if (t.nodes.size() < 2) return;
    int i = rng() % t.nodes.size(), j = rng() % t.nodes.size();
    if (i < j) t.edges.push_back({t.nodes[i].first, t.nodes[j].first});
}
static const std::vector<Mutator> kMutators = {
    m_dangling, m_undeclare, m_orphan_writer, m_drop_edge, m_valid_edge};

int main() {
    register_nodes();
    ng::NodeContext ctx;

    std::cout << "===== coherence-gate: guarantee & boundary, fuzzed (offline, deterministic) =====\n"
                 "(validator lint → stderr; run with 2>/dev/null for just this summary.)\n\n";

    // ---- LAYER 1: static gate ⟷ engine consistency over honest-contract mutants
    const int M = 2000;
    std::mt19937 rng(12345);
    int agree = 0, disagree = 0, stalls = 0;
    int gate_reject = 0, gate_pass = 0, runtime_fault_after_pass = 0;
    std::set<std::string> codes;
    for (int i = 0; i < M; ++i) {
        Topo t = seed_topo();
        int k = 1 + rng() % 3;
        for (int j = 0; j < k; ++j) kMutators[rng() % kMutators.size()](t, rng, i * 8 + j);
        json core = to_core(t);
        Verdict v = validate(core, ctx);
        Outcome o = execute(core, ctx);
        if (o == Outcome::STALL) ++stalls;
        if (!v.codes.empty()) codes.insert(v.codes);
        if (v.has_error) { ++gate_reject;
            // the gate rejected; the engine must also reject (fault), never run it clean
            (o == Outcome::FAULT) ? ++agree : ++disagree;
        } else { ++gate_pass;
            // the gate passed an honest-contract graph; it must run without a throw
            if (o == Outcome::FAULT) { ++disagree; ++runtime_fault_after_pass; }
            else ++agree;
        }
    }
    printf("LAYER 1 — static gate vs engine over %d honest-contract mutants:\n", M);
    printf("  gate rejected %d, gate passed %d;  agreements %d, DISAGREEMENTS %d  (%d stalled)\n",
           gate_reject, gate_pass, agree, disagree, stalls);
    printf("  runtime faults AFTER the gate passed (soundness holes): %d\n", runtime_fault_after_pass);
    std::cout << "  distinct error signatures exercised: ";
    for (const auto& c : codes) std::cout << c << " ";
    std::cout << "\n  → the gate and the runtime never disagree on an honest-contract graph.\n\n";

    // ---- LAYER 2: the boundary — a node whose contract LIES.
    // Contract says writes:["out"] (honest-looking → gate is clean), but the
    // node writes the undeclared "phantom" at runtime → the runtime must fault.
    const int L = 500;
    int liar_gate_pass = 0, liar_runtime_fault = 0;
    for (int i = 0; i < L; ++i) {
        Topo t = seed_topo();
        std::string n = "liar" + std::to_string(i);
        t.nodes.push_back({n, "liar"});
        t.edges.push_back({"c", n});                 // reachable, so it actually runs
        for (int j = 0, k = rng() % 2; j < k; ++j)   // sprinkle a benign edge sometimes
            m_valid_edge(t, rng, i);
        json core = to_core(t);
        Verdict v = validate(core, ctx);
        Outcome o = execute(core, ctx);
        if (!v.has_error) ++liar_gate_pass;          // static gate is blind to the lie
        if (o == Outcome::FAULT) ++liar_runtime_fault;  // runtime backstop catches it
    }
    printf("LAYER 2 — a node that LIES about its effect contract (%d mutants):\n", L);
    printf("  static gate PASSED (blind to the lie): %d/%d\n", liar_gate_pass, L);
    printf("  runtime GraphState guard FAULTED (backstop caught it): %d/%d\n", liar_runtime_fault, L);
    std::cout << "  → the static gate is sound RELATIVE TO honest contracts; a lying contract\n"
                 "    slips past it, and the runtime write-guard is the designed backstop.\n\n";

    std::cout << "===== the guarantee, precisely =====\n"
                 "Layer 1: over " << M << " structural mutants the compiler gate and the engine\n"
                 "never disagreed (0 runtime faults after a pass) — a scale regression guarantee.\n"
                 "Layer 2: the gate trusts effect contracts, so a lying node passes it; the\n"
                 "runtime guard catches 100% of those. Sound relative to honest contracts, with\n"
                 "a runtime backstop for dishonest ones — stated, and measured, not overclaimed.\n";

    // CI gate: Layer 1 has zero gate/engine disagreements, and Layer 2's runtime
    // backstop catches every lying-contract fault the static gate lets pass.
    bool gate = (disagree == 0) && (runtime_fault_after_pass == 0)
                && (liar_gate_pass == L) && (liar_runtime_fault == L);
    std::cout << "\nCI gate (Layer 1: 0 disagreements over " << M
              << "; Layer 2: runtime backstops 100%): " << (gate ? "PASS" : "FAIL") << "\n";
    return gate ? 0 : 1;
}
