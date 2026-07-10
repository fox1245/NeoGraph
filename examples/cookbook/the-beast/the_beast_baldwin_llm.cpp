// NeoGraph Cookbook — "The Beast", BALDWIN-LLM (the LLM as the learning
// operator; the Baldwin/Lamarck toggle = "does the model's fix become
// heritable?")
// =================================================================
// the_beast_baldwin{,_adv} used a mechanical learning operator (random guess /
// hill-climb) so the population dynamics could be measured over many seeds. The
// operator was always the *slot* an LLM refiner plugs into. This variant plugs
// it in.
//
// THE TASK is one an LLM can actually reason about: assemble an arithmetic
// pipeline that computes a TARGET number. Each stage applies acc <- op(acc);
// some stages are COMMITTED, some are PLASTIC ("?"). The LEARNING OPERATOR is
// the model: given the partial harness and the target, it chooses ops for the
// "?" stages. Fitness is the assembled harness RUN (acc vs target).
//
// The Baldwin/Lamarck toggle is then literal and concrete:
//   * Baldwinian — the model's chosen ops are used to SCORE the individual, but
//     are NOT written into its genome: the "?" stays "?", and the model must be
//     consulted again next generation. Learning is not inherited.
//   * Lamarckian — the model's chosen ops are WRITTEN into the genome: the "?"
//     becomes committed. The acquired trait is heritable; the model need not be
//     consulted again for those stages.
//
// The observable consequence is an economy, not a fitness reversal: Lamarck
// BANKS the model's work into heredity (committed genes rise, LLM calls fall);
// Baldwin RE-LEARNS every generation (LLM calls stay high) but keeps its genome
// plastic. That is the honest, concrete meaning of "does the fix become
// heritable?" — shown as a trace, not asserted.
//
// Offline (no key): a deterministic ORACLE learner (enumerates the small fill
// space) stands in for the model, so the mechanism runs and prints identically
// minus the network. With OPENROUTER_API_KEY: the model is the learner.
//
// Build:  cmake --build build --target cookbook_the_beast_baldwin_llm
// Run:    ./build/cookbook_the_beast_baldwin_llm            # oracle learner
//         OPENROUTER_API_KEY=... ./build/cookbook_the_beast_baldwin_llm

#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/async/run_sync.h>

#include <cppdotenv/dotenv.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// ---------------------------------------------------------------- op pool ---
struct Op { const char* name; char kind; double k; };  // kind: + * -
static const std::array<Op, 5> OPS = {{
    {"add2", '+', 2}, {"add3", '+', 3}, {"mul2", '*', 2}, {"mul5", '*', 5}, {"sub1", '-', 1}}};
constexpr int K = 5;
constexpr int N = 4;                 // pipeline stages
constexpr double TARGET = 20.0;      // reachable, e.g. add3,sub1,mul5,mul2: ((3-1)*5)*2 = 20
constexpr int PLASTIC = -1;

using Genome = std::vector<int>;     // length N; op index or PLASTIC

struct ArithNode : ng::GraphNode {
    std::string name_; char kind_; double k_;
    ArithNode(std::string n, const json& c)
        : name_(std::move(n)), kind_(c.value("kind", "+").at(0)), k_(c.value("k", 0.0)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        json av = in.state.get("acc");
        double a = av.is_number() ? av.get<double>() : 0.0;
        double r = kind_ == '+' ? a + k_ : kind_ == '*' ? a * k_ : a - k_;
        ng::NodeOutput o; o.writes.push_back({"acc", r}); co_return o;
    }
    std::string get_name() const override { return name_; }
};
static void register_arith() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "arith",
            [](const std::string& n, const json& c, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new ArithNode(n, c)); },
            json::parse(R"({"type":"object","properties":{
              "kind":{"type":"string"},"k":{"type":"number"}},"required":["kind","k"]})"),
            json::parse(R"({"reads":["acc"],"writes":["acc"]})"));
        return true;
    }();
    (void)once;
}

// Build + RUN the harness for a fully-committed genome; return computed acc.
static double run_acc(const Genome& g, const ng::NodeContext& ctx) {
    json nodes = json::object(), edges = json::array();
    std::string prev = "__start__";
    for (int i = 0; i < N; ++i) {
        std::string nm = "s" + std::to_string(i);
        nodes[nm] = {{"type", "arith"}, {"kind", std::string(1, OPS[g[i]].kind)}, {"k", OPS[g[i]].k}};
        edges.push_back({{"from", prev}, {"to", nm}});
        prev = nm;
    }
    json core = {{"schema_version", 1}, {"name", "arith"},
                 {"channels", {{"acc", {{"reducer", "overwrite"}, {"initial", 0}}}}},
                 {"nodes", nodes}, {"edges", edges}};
    auto engine = ng::GraphEngine::compile(core, ctx);
    ng::RunConfig rc; rc.max_steps = N + 4; rc.input = {{"acc", 0}};
    auto res = engine->run(rc);
    json av = res.has_channel("acc") ? res.channel<json>("acc") : json(0);
    return av.is_number() ? av.get<double>() : 0.0;
}
static double fitness(const Genome& full, const ng::NodeContext& ctx) {
    return -std::abs(run_acc(full, ctx) - TARGET);   // 0 == exact hit
}

static std::string show(const Genome& g) {
    std::string s = "[";
    for (int i = 0; i < N; ++i) s += (i ? " " : "") + std::string(g[i] == PLASTIC ? "?" : OPS[g[i]].name);
    return s + "]";
}

// ---- the learning operator: fill the PLASTIC stages ------------------------
// Result: the chosen op index per plastic position, or nullopt on failure.
struct Learner {
    int invocations = 0;   // times the learner actually ran (plastic present)
    virtual ~Learner() = default;
    virtual const char* label() const = 0;
    // returns a fully-committed genome (plastic filled), or nullopt
    virtual std::optional<Genome> fill(const Genome& g, const ng::NodeContext& ctx) = 0;
};

// Deterministic oracle: enumerate fills of the plastic stages, pick the best.
struct OracleLearner : Learner {
    const char* label() const override { return "oracle"; }
    std::optional<Genome> fill(const Genome& g, const ng::NodeContext& ctx) override {
        std::vector<int> plastic;
        for (int i = 0; i < N; ++i) if (g[i] == PLASTIC) plastic.push_back(i);
        if (plastic.empty()) return g;
        ++invocations;
        long combos = 1; for (size_t i = 0; i < plastic.size(); ++i) combos *= K;
        Genome best = g; double bestf = -1e9;
        for (long c = 0; c < combos; ++c) {
            Genome f = g; long cc = c;
            for (int idx : plastic) { f[idx] = cc % K; cc /= K; }
            double fit = fitness(f, ctx);
            if (fit > bestf) { bestf = fit; best = f; }
        }
        return best;
    }
};

static std::optional<Genome> parse_ops(const Genome& g, const std::string& reply) {
    // find the op names for the plastic positions, in order, anywhere in reply
    std::vector<int> plastic;
    for (int i = 0; i < N; ++i) if (g[i] == PLASTIC) plastic.push_back(i);
    Genome out = g;
    size_t from = 0;
    for (int idx : plastic) {
        int chosen = -1; size_t at = std::string::npos;
        for (int k = 0; k < K; ++k) {
            size_t p = reply.find(OPS[k].name, from);
            if (p != std::string::npos && p < at) { at = p; chosen = k; }
        }
        if (chosen < 0) return std::nullopt;
        out[idx] = chosen; from = at + 1;
    }
    return out;
}

// LLM learner: the model chooses ops for the plastic stages to hit TARGET.
struct LLMLearner : Learner {
    std::shared_ptr<neograph::Provider> prov;
    explicit LLMLearner(std::shared_ptr<neograph::Provider> p) : prov(std::move(p)) {}
    const char* label() const override { return "LLM(deepseek-v4-flash)"; }
    std::optional<Genome> fill(const Genome& g, const ng::NodeContext& ctx) override {
        std::vector<int> plastic;
        for (int i = 0; i < N; ++i) if (g[i] == PLASTIC) plastic.push_back(i);
        if (plastic.empty()) return g;
        ++invocations;
        neograph::CompletionParams p;
        p.model = "deepseek/deepseek-v4-flash"; p.temperature = 0.2f; p.max_tokens = 800;
        std::string committed;
        for (int i = 0; i < N; ++i)
            committed += "  stage " + std::to_string(i) + ": " +
                         (g[i] == PLASTIC ? "?" : OPS[g[i]].name) + "\n";
        p.messages = {
            {"system",
             "You assemble an arithmetic pipeline. acc starts at 0; each stage applies "
             "acc<-op(acc) in order. Ops: add2(+2) add3(+3) mul2(*2) mul5(*5) sub1(-1). "
             "Choose an op for each '?' stage so the final acc equals the target. Reply "
             "with just the op name(s) for the '?' stage(s), in stage order."},
            {"user", "Target acc = " + std::to_string((int)TARGET) + ". Pipeline:\n" + committed}};
        std::string reply;
        try { reply = neograph::async::run_sync(prov->invoke(p, nullptr)).message.content; }
        catch (const std::exception& e) { std::cerr << "   [LLM] error: " << e.what() << "\n"; return std::nullopt; }
        auto out = parse_ops(g, reply);
        if (!out) std::cerr << "   [LLM] unparseable: " << reply.substr(0, 80) << "\n";
        return out;
    }
};

enum class Mode { Baldwin, Lamarck };

// A small memetic run: prints the per-generation trace of the toggle's effect.
static void run(Mode mode, Learner& learner,
                const ng::NodeContext& ctx, std::mt19937& rng) {
    const int P = 6, kGens = 4;
    // init: every individual has stage 0 committed (add3) and 2-3 plastic
    std::vector<Genome> pop(P);
    for (auto& ind : pop) {
        ind.assign(N, PLASTIC);
        ind[0] = 1;  // add3 committed as a seed
        for (int i = 1; i < N; ++i) if (rng() % 2 == 0) ind[i] = rng() % K;  // some committed
    }
    std::cout << "  " << (mode == Mode::Baldwin ? "Baldwinian" : "Lamarckian")
              << " (learner = " << learner.label() << "):\n";
    for (int gen = 0; gen < kGens; ++gen) {
        int calls0 = learner.invocations;
        double best = -1e9; int committed_total = 0, hits = 0;
        std::vector<std::pair<double,int>> scored;
        for (int i = 0; i < P; ++i) {
            auto filled = learner.fill(pop[i], ctx);
            Genome phen = filled.value_or(pop[i]);
            // if a plastic gene remained (learner failed), skip scoring cleanly
            bool full = true; for (int v : phen) if (v == PLASTIC) full = false;
            double fit = full ? fitness(phen, ctx) : -1e9;
            if (mode == Mode::Lamarck && filled && full) pop[i] = phen;  // WRITE BACK
            scored.push_back({fit, i});
            best = std::max(best, fit);
            if (fit == 0.0) ++hits;
        }
        for (const auto& ind : pop) for (int v : ind) if (v != PLASTIC) ++committed_total;
        printf("    gen %d: best fitness %5.1f | exact hits %d/%d | committed genes %2d/%2d"
               " | learner calls %d\n",
               gen, best, hits, P, committed_total, P * N, learner.invocations - calls0);
        // selection + crossover + light mutation (keeps plastic alleles alive)
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b){ return a.first > b.first; });
        std::vector<Genome> parents;
        for (int i = 0; i < P / 2; ++i) parents.push_back(pop[scored[i].second]);
        std::vector<Genome> next = {parents[0]};
        while ((int)next.size() < P) {
            const Genome& pa = parents[rng() % parents.size()];
            const Genome& pb = parents[rng() % parents.size()];
            int cut = 1 + rng() % (N - 1);
            Genome ch(N);
            // Crossover only recombines existing alleles; it never commits a
            // gene that learning left plastic. So in Baldwin the '?' persists
            // (learning is not banked); in Lamarck the write-back has already
            // committed it. This is the toggle's whole effect on the genome.
            for (int i = 0; i < N; ++i) ch[i] = (i < cut ? pa[i] : pb[i]);
            next.push_back(std::move(ch));
        }
        pop = std::move(next);
    }
}

int main(int argc, char** argv) {
    register_arith();
    ng::NodeContext ctx;
    const bool want_llm = (argc > 1 && std::string(argv[1]) == "--llm");
    const char* key = want_llm ? (cppdotenv::auto_load_dotenv(), std::getenv("OPENROUTER_API_KEY"))
                               : nullptr;
    const bool live = want_llm && key && *key;
    if (want_llm && !live)
        std::cout << "(--llm given but no OPENROUTER_API_KEY — falling back to oracle.)\n";

    std::cout << "===== THE BEAST (baldwin-llm · the model IS the learning operator) =====\n"
                 "Task: fill the '?' stages of a " << N << "-stage arithmetic pipeline so acc "
              << "reaches " << (int)TARGET << ".\n"
                 "Toggle: Baldwinian scores the learner's fill but keeps the gene '?'; Lamarckian\n"
                 "writes the fill into the genome (heritable).\n"
                 "Learner = "
              << (live ? "the model (deepseek-v4-flash).\n\n"
                       : "deterministic oracle (default; pass --llm with a key for the model).\n\n");

    std::shared_ptr<neograph::Provider> provider;
    if (live)
        provider = neograph::llm::OpenAIProvider::create_shared(
            {.api_key = key, .base_url = "https://openrouter.ai/api",
             .default_model = "deepseek/deepseek-v4-flash"});

    // A fresh learner per mode so the invocation counters are independent.
    auto make_learner = [&]() -> std::unique_ptr<Learner> {
        if (live) return std::make_unique<LLMLearner>(provider);
        return std::make_unique<OracleLearner>();
    };
    auto bald = make_learner();
    auto lam = make_learner();

    // Same seed for both modes so the only difference is the heritability toggle.
    std::mt19937 rb(7); run(Mode::Baldwin, *bald, ctx, rb);
    std::cout << "\n";
    std::mt19937 rl(7); run(Mode::Lamarck, *lam, ctx, rl);

    printf("\ntotal learner invocations: Baldwin %d vs Lamarck %d%s\n",
           bald->invocations, lam->invocations,
           lam->invocations < bald->invocations ? "  (Lamarck banked its way to fewer)" : "");
    std::cout << "\n===== reading =====\n"
                 "Lamarckian BANKS the learner's fix into heredity: committed genes jump to full\n"
                 "and the learner is consulted less as genomes fill in — the acquired trait is\n"
                 "inherited. Baldwinian RE-LEARNS: committed genes stay low, the '?' persists,\n"
                 "and the learner is invoked every generation — the fix is expressed for fitness\n"
                 "but never written to the genome. Same task, same seed; the only difference is\n"
                 "whether the learner's fix becomes heritable. With the model as the learner\n"
                 "(--llm), those invocations are real API calls — so heredity is also an economy:\n"
                 "Lamarck pays the model once and banks it; Baldwin re-pays every generation.\n";
    return 0;
}
