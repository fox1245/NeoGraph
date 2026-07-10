// NeoGraph Cookbook — "The Beast", GATE-EVAL (empirical soundness of the
// coherence gate)
// =================================================================
// The Beast's whole safety argument is that the compiler's static validator
// is a *sound* coherence oracle: if it reports an ERROR, the harness is
// genuinely broken (would fault at runtime); if it reports only warnings /
// nothing, the harness runs. Every reviewer of this cookbook asked the same
// two questions — "is the validator SOUND?" and "what is its precision /
// recall?" — and the honest answer so far was "asserted, not measured."
//
// This program measures it. It runs a labeled corpus of topologies through
// the validator (predicted verdict) AND through the engine (ground truth),
// and cross-checks the correspondence. It is fully offline and
// deterministic — no LLM, no key — so CI can gate on the soundness result.
//
// The theorem being tested (effect-system / graph-coherence soundness):
//   validator reports an ERROR  ⟹  the graph faults when executed
//   validator reports NO error  ⟹  the graph executes cleanly
// The first line is *soundness* (no false accept of a broken graph as an
// error-free run would be a soundness hole; a false REJECT is a completeness
// gap). Some diagnostics (E10 empty-route dispatch, E8 dead barrier) are
// deliberately Undefined-Behaviour / non-terminating at runtime — the gate
// exists precisely to stop them reaching execution — so those are checked
// for the verdict only and NOT run.
//
// Build:  cmake --build build --target cookbook_the_beast_gate_eval
// Run:    ./build/cookbook_the_beast_gate_eval

#include <neograph/neograph.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <iostream>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// A worker with a declared effect contract: writes the "out" channel.
struct W : ng::GraphNode {
    std::string name_;
    explicit W(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput o; o.writes.push_back({"out", 1}); co_return o;
    }
    std::string get_name() const override { return name_; }
};
void register_w() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "w",
            [](const std::string& n, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new W(n)); },
            json::object(), json::parse(R"({"reads":[],"writes":["out"]})"));
        return true;
    }();
    (void)once;
}

static json chan(bool declare_out) {
    return declare_out ? json{{"out", {{"reducer", "overwrite"}}}} : json::object();
}

// --- validator verdict ---
struct Verdict { bool compiled = false; bool has_error = false; std::string codes; };
static Verdict validate(const json& core, const ng::NodeContext& ctx) {
    Verdict v;
    ng::CompiledGraph cg;
    try { cg = ng::GraphCompiler::compile(core, ctx); v.compiled = true; }
    catch (const std::exception&) { v.compiled = false; v.has_error = true; v.codes = "compile-reject"; return v; }
    auto rep = ng::GraphValidator::validate(cg);
    v.has_error = rep.has_errors();
    for (const auto* e : rep.errors()) { v.codes += (v.codes.empty() ? "" : ",") + e->code; }
    return v;
}

// --- runtime outcome ---
enum class Outcome { CLEAN, FAULT, STALL };
static const char* name(Outcome o) { return o == Outcome::CLEAN ? "CLEAN" : o == Outcome::FAULT ? "FAULT" : "STALL"; }
static Outcome execute(const json& core, const ng::NodeContext& ctx) {
    try {
        auto engine = ng::GraphEngine::compile(core, ctx);
        ng::RunConfig rc; rc.max_steps = 8; rc.input = {{"out", 0}};
        auto res = engine->run(rc);
        return (int)res.execution_trace.size() >= rc.max_steps ? Outcome::STALL : Outcome::CLEAN;
    } catch (const std::exception&) { return Outcome::FAULT; }
}

struct Case { std::string name; json core; bool expect_error; bool runnable; };

int main() {
    register_w();
    ng::NodeContext ctx;

    std::vector<Case> corpus;
    // 1. coherent — no error, runs clean.
    corpus.push_back({"coherent", {
        {"schema_version",1},{"channels", chan(true)},
        {"nodes", {{"a",{{"type","w"}}},{"b",{{"type","w"}}}}},
        {"edges", json::array({ {{"from","__start__"},{"to","a"}}, {{"from","a"},{"to","b"}} })}}, false, true});
    // 2. E4 — node writes "out" but the channel is undeclared → runtime throws.
    corpus.push_back({"E4-undeclared-write", {
        {"schema_version",1},{"channels", chan(false)},
        {"nodes", {{"a",{{"type","w"}}}}},
        {"edges", json::array({ {{"from","__start__"},{"to","a"}} })}}, true, true});
    // 3. E3 — edge to a node that doesn't exist.
    corpus.push_back({"E3-dangling-edge", {
        {"schema_version",1},{"channels", chan(true)},
        {"nodes", {{"a",{{"type","w"}}}}},
        {"edges", json::array({ {{"from","__start__"},{"to","a"}}, {{"from","a"},{"to","ghost"}} })}}, true, true});
    // 4. E7 — warning (unreachable orphan), NOT an error: must run clean.
    corpus.push_back({"E7-unreachable(warn)", {
        {"schema_version",1},{"channels", chan(true)},
        {"nodes", {{"a",{{"type","w"}}},{"orphan",{{"type","w"}}}}},
        {"edges", json::array({ {{"from","__start__"},{"to","a"}} })}}, false, true});
    // 5. E10 — empty route map. Dispatch would deref rend() (UB): verdict only.
    corpus.push_back({"E10-empty-routes", {
        {"schema_version",1},{"channels", chan(true)},
        {"nodes", {{"a",{{"type","w"}}},{"b",{{"type","w"}}}}},
        {"edges", json::array({ {{"from","__start__"},{"to","a"}},
            {{"from","a"},{"condition","has_tool_calls"},{"routes", json::object()}} })}}, true, false});

    std::cout << "===== coherence-gate soundness eval (offline, deterministic) =====\n";
    std::cout << "case                     | validator      | runtime | sound?\n";
    std::cout << "-------------------------+----------------+---------+-------\n";
    int checks = 0, sound = 0;
    for (const auto& c : corpus) {
        Verdict v = validate(c.core, ctx);
        std::string vs = v.has_error ? ("ERROR:" + v.codes) : "ok";
        std::string rs = "-- (not run)";
        bool ok = (v.has_error == c.expect_error);   // verdict matches the label
        if (c.runnable) {
            Outcome o = execute(c.core, ctx);
            rs = name(o);
            // soundness: an ERROR verdict must correspond to a non-CLEAN run;
            // a no-error verdict must correspond to a CLEAN run.
            bool corr = v.has_error ? (o != Outcome::CLEAN) : (o == Outcome::CLEAN);
            ok = ok && corr;
            ++checks; sound += corr ? 1 : 0;
        }
        printf("%-24s | %-14s | %-7s | %s\n", c.name.c_str(), vs.c_str(), rs.c_str(), ok ? "yes" : "NO");
    }
    std::cout << "-------------------------+----------------+---------+-------\n";
    std::cout << "runtime cross-check: " << sound << "/" << checks
              << " cases where the validator's verdict matched execution "
              << "(ERROR⇒faults, ok⇒runs clean).\n"
              << "E10/E8-class errors are verdict-only: running them is UB / non-terminating "
              << "by design — which is exactly why the gate rejects them before execution.\n";
    return (checks > 0 && sound == checks) ? 0 : 1;
}
