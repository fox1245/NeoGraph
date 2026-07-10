// NeoGraph Cookbook — "The Beast", EVOLVE (memetic, real-task fitness)
// =================================================================
// evolution.h's evaluate() is gate-only: it compiles/validates a mutant
// and calls it "cost 0" — it never RUNS the harness or scores its output.
// So fitness is flat and nothing climbs.
//
// This supplies a REAL, output-scored fitness and drives a memetic loop.
//
// The task (a genuine one, not a structural proxy): assemble an ARITHMETIC
// PIPELINE that computes a target number. Five operation nodes exist —
// add2(+2), add3(+3), mul5(*5), mul2(*2), sub1(-1) — each reads the `acc`
// channel (init 0), applies its op, writes it back. The harness's answer is
// whatever `acc` holds after execution; fitness = -(|acc - TARGET|). The
// *topology* (which ops run, in what order) determines the number, so
// evolving the wiring evolves the computation.
//
//   * Darwinian: random rewiring (evolution.h all_operators()) + selection
//     by measured output — stumbles toward the target.
//   * Lamarckian: an LLM does the arithmetic, wires a chain that hits the
//     target exactly, and injects that acquired solution as a heritable
//     seed.
//
// Offline (no key): pure Darwinian, deterministic. With OPENROUTER_API_KEY:
// adds the Lamarckian injection.
//
// Build:  cmake --build build --target cookbook_the_beast_evolve
// Run:    ./build/cookbook_the_beast_evolve   [--darwin-only]

#include <neograph/neograph.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/evolution.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/llm/openai_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

static const double kTarget = 20.0;

// An arithmetic worker: acc <- acc <op> k. Deterministic, no subprocess.
struct OpNode : ng::GraphNode {
    std::string name_, op_;
    double k_;
    OpNode(std::string n, const json& cfg)
        : name_(std::move(n)), op_(cfg.value("op", "+")), k_(cfg.value("k", 0.0)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        json av = in.state.get("acc");
        double a = av.is_number() ? av.get<double>() : 0.0;
        double r = op_ == "+" ? a + k_ : op_ == "*" ? a * k_ : op_ == "-" ? a - k_ : a;
        ng::NodeOutput o;
        o.writes.push_back({"acc", r});
        co_return o;
    }
    std::string get_name() const override { return name_; }
};
void register_op_node() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "op_node",
            [](const std::string& n, const json& cfg, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new OpNode(n, cfg)); },
            json::parse(R"({"type":"object","properties":{
              "op":{"type":"string"},"k":{"type":"number"}},"required":["op","k"]})"),
            json::parse(R"({"reads":["acc"],"writes":["acc"]})"));
        return true;
    }();
    (void)once;
}

// Op pool. Target 20 is reachable, e.g. add2 -> mul5 -> mul2: (0+2)*5*2 = 20.
struct Op { const char* name; const char* op; double k; };
static const std::vector<Op> kOps = {
    {"add2", "+", 2}, {"add3", "+", 3}, {"mul5", "*", 5}, {"mul2", "*", 2}, {"sub1", "-", 1}};

static json seed_core() {
    json nodes = json::object();
    for (const auto& o : kOps) nodes[o.name] = {{"type", "op_node"}, {"op", o.op}, {"k", o.k}};
    return {
        {"schema_version", 1},
        {"name", "evolve_arith"},
        {"channels", {{"acc", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes", nodes},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "add2"}},
            {{"from", "add2"}, {"to", "add3"}}   // seed computes (0+2)+3 = 5
        })}
    };
}

// REAL fitness: gate, then RUN the harness and read the computed acc.
struct Fit { bool valid = false; double acc = 0; double score = -1e9; };
static Fit fitness(const json& core, const ng::NodeContext& ctx) {
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);
        if (ng::GraphValidator::validate(cg).has_errors()) return {};
    } catch (const std::exception&) { return {}; }
    try {
        auto engine = ng::GraphEngine::compile(core, ctx);
        ng::RunConfig rc; rc.max_steps = 40; rc.input = {{"acc", 0}};
        auto res = engine->run(rc);
        json av = res.has_channel("acc") ? res.channel<json>("acc") : json(0);
        double a = av.is_number() ? av.get<double>() : 0.0;
        return {true, a, -std::abs(a - kTarget)};
    } catch (const std::exception&) { return {}; }
}

struct Ind { json core; double score; double acc; std::string origin; };

static json extract_json(const std::string& t) {
    auto a = t.find('{'), b = t.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) throw std::runtime_error("no JSON");
    return json::parse(t.substr(a, b - a + 1));
}

// Lamarckian: the LLM does the arithmetic and wires a chain hitting TARGET.
static std::optional<json> llm_refine(std::shared_ptr<neograph::Provider> prov, const Ind& elite) {
    neograph::CompletionParams p;
    p.model = "deepseek/deepseek-v4-flash";
    p.temperature = 0.2f; p.max_tokens = 1500;
    p.messages = {
        {"system",
         "You repair a NeoGraph arithmetic harness (JSON). Output ONLY the corrected "
         "JSON object. `acc` starts at 0; each op_node applies acc<-acc<op>k in "
         "execution (edge) order. Available nodes: add2(+2) add3(+3) mul5(*5) mul2(*2) "
         "sub1(-1). Rewire `edges` into a single chain __start__->...-> so that acc "
         "becomes exactly 20. Keep the nodes, channels, and schema_version 1 unchanged; "
         "change only edges. Example: (0+2)*5*2=20."},
        {"user", "Current harness computes acc=" + std::to_string(elite.acc) +
                 " (target 20):\n" + elite.core.dump()}};
    std::string reply;
    try { reply = prov->complete_stream(p, [](const std::string&){}).message.content; }
    catch (const std::exception& e) { std::cerr << "   [refine] LLM error: " << e.what() << "\n"; return std::nullopt; }
    try { return extract_json(reply); }
    catch (const std::exception&) {
        std::cerr << "   [refine] unparseable reply (" << reply.size() << " chars): "
                  << reply.substr(0, 120) << "\n";
        return std::nullopt;
    }
}

int main(int argc, char** argv) {
    register_op_node();
    const bool darwin_only = (argc > 1 && std::string(argv[1]) == "--darwin-only");
    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    std::shared_ptr<neograph::Provider> provider;
    const bool lamarck = !darwin_only && key && *key;
    if (lamarck)
        provider = neograph::llm::OpenAIProvider::create_shared(
            {.api_key = key, .base_url = "https://openrouter.ai/api",
             .default_model = "deepseek/deepseek-v4-flash"});

    ng::NodeContext ctx;
    std::cout << "======= THE BEAST (evolve · memetic · real task) =======\n"
                 "Task: wire the op-nodes so acc (from 0) computes " << (int)kTarget << ".\n"
                 "Fitness = -(|acc - " << (int)kTarget << "|), scored by RUNNING the harness.\n"
                 "Darwinian mutation + selection"
              << (lamarck ? " + Lamarckian LLM injection." : " (Darwinian-only).") << "\n\n";

    auto ops = ng::all_operators();
    std::mt19937 rng(7);

    Fit sf = fitness(seed_core(), ctx);
    std::vector<Ind> pop = {{seed_core(), sf.score, sf.acc, "seed"}};
    std::cout << "gen 0  seed acc=" << sf.acc << "  fitness " << sf.score << "\n";

    const int kGens = 14, kKeep = 8, kChildren = 5;
    for (int gen = 1; gen <= kGens; ++gen) {
        std::vector<Ind> next = pop;
        for (const auto& parent : pop)
            for (int c = 0; c < kChildren; ++c) {
                auto mr = ops[rng() % ops.size()](parent.core, rng);
                if (!mr.core) continue;
                Fit f = fitness(*mr.core, ctx);
                if (f.valid) next.push_back({*mr.core, f.score, f.acc, "mut"});
            }
        std::sort(next.begin(), next.end(), [](const Ind& a, const Ind& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return (a.origin == "LLM") && (b.origin != "LLM");  // honest tie-break: credit Lamarckian
                  });
        std::vector<Ind> keep;
        for (const auto& ind : next) {
            bool dup = false;
            for (const auto& k : keep) if (k.core == ind.core) { dup = true; break; }
            if (!dup) keep.push_back(ind);
            if ((int)keep.size() >= kKeep) break;
        }
        pop = keep;

        Ind& best = pop.front();
        std::cout << "gen " << gen << "  best acc=" << best.acc << "  fitness " << best.score
                  << "  (" << best.origin << ")\n";
        if (best.score == 0.0) { std::cout << "\nSolved — the pipeline computes " << (int)kTarget << ".\n"; break; }

        if (lamarck && gen % 3 == 0 && best.score < 0.0) {
            auto imp = llm_refine(provider, best);
            if (!imp) { std::cout << "   [Lamarckian] LLM returned no parseable harness.\n"; }
            if (imp) {
                Fit f = fitness(*imp, ctx);
                std::cout << "   [Lamarckian] LLM refinement acc=" << f.acc << "  fitness " << f.score;
                if (f.valid && f.score > best.score) {
                    pop.push_back({*imp, f.score, f.acc, "LLM"});
                    std::sort(pop.begin(), pop.end(), [](const Ind& a, const Ind& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return (a.origin == "LLM") && (b.origin != "LLM");  // honest tie-break: credit Lamarckian
                  });
                    std::cout << "  → injected (heritable)\n";
                } else std::cout << "  → not better, discarded\n";
                if (pop.front().score == 0.0) { std::cout << "\nSolved via Lamarckian injection.\n"; break; }
            }
        }
    }

    const Ind& champ = pop.front();
    std::cout << "\nchampion: acc=" << champ.acc << ", origin '" << champ.origin << "'. ";
    if (champ.origin == "LLM")
        std::cout << "The winner is a Lamarckian acquired trait (LLM refinement), injected "
                     "and heritable.\n";
    else if (lamarck)
        std::cout << "The winner came from Darwinian mutation; Lamarckian refinement was in "
                     "the loop but did not produce the champion this run.\n";
    else
        std::cout << "Pure Darwinian mutation + selection.\n";
    return 0;
}
