// NeoGraph Cookbook — "The Beast", EVOLVE (memetic: Darwinian + Lamarckian)
// =================================================================
// The offline the_beast.cpp shows evolution.h's evolve(), but that path's
// evaluate() is gate-only — it compiles/validates a mutant and calls it
// "cost 0"; it never RUNS the harness or scores its output. So fitness is
// flat and nothing actually climbs.
//
// This one supplies the missing piece: a REAL fitness that executes the
// harness and scores its behaviour, then drives a memetic loop —
//   * Darwinian: random topology mutations (evolution.h all_operators()) +
//     selection by measured fitness.
//   * Lamarckian: between rounds, an LLM inspects the current elite,
//     writes a directed improvement (an *acquired* trait), and injects it
//     back into the population as a heritable seed.
//
// Fitness (a structural target, honestly a proxy — the point is the
// mechanism, not a semantically hard task): the harness has 5 worker
// nodes s1..s5 but the seed only wires two into the active path, so only
// two execute and append to `trail`. Fitness = -(|executed - 5|). Random
// add-edge mutations can wire more workers in (Darwinian climb); the LLM
// can wire all of them at once (Lamarckian jump).
//
// Offline (no key): pure Darwinian — deterministic, watch fitness climb.
// With OPENROUTER_API_KEY: adds the Lamarckian injection every few rounds.
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

static const int kTarget = 5;   // want all 5 workers on the active path

// Deterministic worker: appends its name to "trail" when it executes.
struct BeastNode : ng::GraphNode {
    std::string name_;
    explicit BeastNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput o;
        o.writes.push_back({"trail", json::array({name_})});
        co_return o;
    }
    std::string get_name() const override { return name_; }
};
void register_beast_node() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "beast_node",
            [](const std::string& n, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new BeastNode(n)); },
            json::object(), json::parse(R"({"reads":["trail"],"writes":["trail"]})"));
        return true;
    }();
    (void)once;
}

// Seed: 5 workers exist, only s1->s2 wired into the path (s3/s4/s5 orphans).
static json seed_core() {
    json nodes = json::object();
    for (int i = 1; i <= 5; ++i) nodes["s" + std::to_string(i)] = {{"type", "beast_node"}};
    return {
        {"schema_version", 1},
        {"name", "evolve_seed"},
        {"channels", {{"trail", {{"reducer", "append"}}}}},
        {"nodes", nodes},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "s1"}},
            {{"from", "s1"}, {"to", "s2"}}
        })}
    };
}

// REAL fitness: gate, then actually RUN the harness and count executed
// workers via the trail. Invalid harnesses score -infinity.
struct Fit { bool valid = false; int executed = 0; double score = -1e9; };
static Fit fitness(const json& core, const ng::NodeContext& ctx) {
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);
        if (ng::GraphValidator::validate(cg).has_errors()) return {};
    } catch (const std::exception&) { return {}; }
    try {
        auto engine = ng::GraphEngine::compile(core, ctx);
        ng::RunConfig rc; rc.max_steps = 40; rc.input = {{"trail", json::array()}};
        auto res = engine->run(rc);
        json trail = res.has_channel("trail") ? res.channel<json>("trail") : json::array();
        int n = static_cast<int>(trail.size());
        return {true, n, -static_cast<double>(std::abs(n - kTarget))};
    } catch (const std::exception&) { return {}; }
}

struct Ind { json core; double score; int executed; std::string origin; };

static json extract_json(const std::string& t) {
    auto a = t.find('{'), b = t.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        throw std::runtime_error("no JSON");
    return json::parse(t.substr(a, b - a + 1));
}

// Lamarckian operator: the LLM inspects the elite and writes a directed fix.
static std::optional<json> llm_refine(std::shared_ptr<neograph::Provider> prov,
                                      const Ind& elite) {
    neograph::CompletionParams p;
    p.model = "deepseek/deepseek-v4-flash";
    p.temperature = 0.2f; p.max_tokens = 1500;
    p.messages = {
        {"system",
         "You repair a NeoGraph topology (JSON). Output ONLY the corrected JSON "
         "object. Nodes s1..s5 are workers; a worker executes (and counts) only if "
         "it is reachable from __start__ via edges. Add edges so ALL of s1..s5 "
         "execute in a single chain __start__->s1->s2->s3->s4->s5. Keep channels "
         "{\"trail\":{\"reducer\":\"append\"}} and schema_version 1."},
        {"user", "Current harness (executes " + std::to_string(elite.executed) +
                 "/5 workers):\n" + elite.core.dump()}};
    try {
        return extract_json(prov->complete_stream(p, [](const std::string&){}).message.content);
    } catch (const std::exception&) { return std::nullopt; }
}

int main(int argc, char** argv) {
    register_beast_node();
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
    std::cout << "======= THE BEAST (evolve · memetic) =======\n"
                 "Real fitness = # of 5 workers the harness actually executes.\n"
                 "Darwinian mutation + selection"
              << (lamarck ? " + Lamarckian LLM injection." : " (Darwinian-only).") << "\n\n";

    auto ops = ng::all_operators();
    std::mt19937 rng(42);   // deterministic Darwinian search

    Fit sf = fitness(seed_core(), ctx);
    std::vector<Ind> pop = {{seed_core(), sf.score, sf.executed, "seed"}};
    std::cout << "gen 0  seed executes " << sf.executed << "/5  fitness " << sf.score << "\n";

    const int kGens = 12, kKeep = 6, kChildren = 4;
    for (int gen = 1; gen <= kGens; ++gen) {
        std::vector<Ind> next = pop;                 // elitism
        for (const auto& parent : pop) {
            for (int c = 0; c < kChildren; ++c) {
                auto mr = ops[rng() % ops.size()](parent.core, rng);
                if (!mr.core) continue;
                Fit f = fitness(*mr.core, ctx);
                if (f.valid) next.push_back({*mr.core, f.score, f.executed, "mut"});
            }
        }
        std::sort(next.begin(), next.end(),
                  [](const Ind& a, const Ind& b) { return a.score > b.score; });
        // de-dup identical cores, keep top-K
        std::vector<Ind> keep;
        for (const auto& ind : next) {
            bool dup = false;
            for (const auto& k : keep) if (k.core == ind.core) { dup = true; break; }
            if (!dup) keep.push_back(ind);
            if (static_cast<int>(keep.size()) >= kKeep) break;
        }
        pop = keep;

        Ind& best = pop.front();
        std::cout << "gen " << gen << "  best executes " << best.executed
                  << "/5  fitness " << best.score << "  (" << best.origin << ")\n";
        if (best.score == 0.0) { std::cout << "\nSolved — all 5 workers on the path.\n"; break; }

        // Lamarckian injection every 3 gens while stuck.
        if (lamarck && gen % 3 == 0 && best.score < 0.0) {
            auto imp = llm_refine(provider, best);
            if (imp) {
                Fit f = fitness(*imp, ctx);
                std::cout << "   [Lamarckian] LLM refinement executes " << f.executed
                          << "/5  fitness " << f.score;
                if (f.valid && f.score > best.score) {
                    pop.push_back({*imp, f.score, f.executed, "LLM"});
                    std::sort(pop.begin(), pop.end(),
                              [](const Ind& a, const Ind& b) { return a.score > b.score; });
                    std::cout << "  → injected (heritable)\n";
                } else {
                    std::cout << "  → not better, discarded\n";
                }
                if (pop.front().score == 0.0) { std::cout << "\nSolved via Lamarckian injection.\n"; break; }
            }
        }
    }

    const Ind& champ = pop.front();
    std::cout << "\nchampion: executes " << champ.executed << "/5, origin '" << champ.origin
              << "'. Darwinian mutation searched; "
              << (lamarck ? "Lamarckian LLM refinement injected acquired fixes." : "no LLM used.")
              << "\n";
    return 0;
}
