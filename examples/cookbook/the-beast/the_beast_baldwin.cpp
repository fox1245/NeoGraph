// NeoGraph Cookbook — "The Beast", BALDWIN (Darwinian vs Baldwinian vs
// Lamarckian memetic evolution of harness topologies)
// =================================================================
// The `evolve` variant showed a memetic loop (Darwinian mutation + a
// Lamarckian LLM injection). Every reviewer asked the sharper question:
//   "Is there a task where blind evolution AND a one-shot solver BOTH fail,
//    but the memetic combination succeeds — and does *how* you inherit the
//    learned trait (Lamarck: write it into the genome; Baldwin: don't) change
//    the outcome the way the literature predicts?"
// (Whitley 1994, "Lamarckian Evolution, The Baldwin Effect and Function
//  Optimization"; Hinton & Nowlan 1987, "How Learning Can Guide Evolution".)
//
// This program is that experiment, run over REAL NeoGraph harnesses.
//
// THE GENOME is the wiring of an N-stage affine pipeline: each stage picks one
// of K op-nodes (acc <- a*acc + b), or is left PLASTIC ("?") to be resolved by
// lifetime learning. Each genome compiles to an ACTUAL NeoGraph topology, and a
// startup cross-check runs 200 of them through the compiled engine to prove the
// engine executes each exactly as the analytic model predicts (the same
// validator-vs-execution discipline as gate_eval) — so the substrate is a real,
// faithfully-executed harness, not an abstract bitstring.
//
// THE OBJECTIVE the GA optimizes is a DECEPTIVE landscape defined over that
// wiring (a controlled testbed for the memetic dynamics — Hamming distance to
// two target topologies, below — NOT the raw execution output). The point is
// the evolutionary dynamics, so the landscape is designed rather than emergent.
//
// THE LANDSCAPE is deceptive, with two features:
//   * DECOY  — a broad, shallow hill (peak 0.85) visible EVERYWHERE. Blind
//     committed-space selection and greedy write-back climb it.
//   * GLOBAL — a narrow, flat plateau (1.0 within GRADIUS genes of the target)
//     with NO surrounding gradient. It is invisible to a single committed
//     genome; only lifetime learning, which searches the neighborhood spanned
//     by the plastic genes, can land on it.
//
// THREE MODES, one question — where blind search AND a one-shot solver both
// stall, does memetic search win, and does HOW you inherit the learned trait
// matter?
//   * Darwinian : genes all committed, no learning. Sees only the decoy → trapped.
//   * Baldwinian: plastic genes, learn each life, DON'T write the result back.
//                 Selection on learned fitness assimilates the global genetically.
//   * Lamarckian: plastic genes, learn, WRITE the learned phenotype into the genome.
//
// Fully offline, deterministic (seeded). CI gates ONLY the robust claim (blind
// Darwin stays near the chance floor; learning assimilates the global by a wide
// margin; fitness is faithful). The Baldwinian control — whether non-inheritance
// beats write-back (Whitley's reversal) — is REPORTED as a measured multi-seed
// statistic, never gated: on this deceptive-but-not-adversarial landscape the
// two are comparable, and the reversal is left as honestly-noted future work.
//
// Build:  cmake --build build --target cookbook_the_beast_baldwin
// Run:    ./build/cookbook_the_beast_baldwin

#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// ---------------------------------------------------------------- op pool ---
constexpr int N = 8;   // pipeline stages (genome length)
constexpr int K = 4;   // op choices per stage
struct Aff { double a, b; };
static const std::array<Aff, K> OPS = {{{2, 1}, {3, 1}, {5, 2}, {7, 3}}};
constexpr int PLASTIC = -1;

// A stage node: acc <- a*acc + b. Deterministic, no subprocess.
struct AffineNode : ng::GraphNode {
    std::string name_;
    double a_, b_;
    AffineNode(std::string n, const json& cfg)
        : name_(std::move(n)), a_(cfg.value("a", 1.0)), b_(cfg.value("b", 0.0)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        json av = in.state.get("acc");
        double a = av.is_number() ? av.get<double>() : 0.0;
        ng::NodeOutput o;
        o.writes.push_back({"acc", a_ * a + b_});
        co_return o;
    }
    std::string get_name() const override { return name_; }
};
static void register_affine() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "affine",
            [](const std::string& n, const json& cfg, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new AffineNode(n, cfg)); },
            json::parse(R"({"type":"object","properties":{
              "a":{"type":"number"},"b":{"type":"number"}},"required":["a","b"]})"),
            json::parse(R"({"reads":["acc"],"writes":["acc"]})"));
        return true;
    }();
    (void)once;
}

using Genome = std::vector<int>;   // length N; each in [0,K) committed, or PLASTIC

// Build the real NeoGraph topology for a fully-committed assignment.
static json build_core(const Genome& g) {
    json nodes = json::object();
    json edges = json::array();
    std::string prev = "__start__";
    for (int i = 0; i < N; ++i) {
        std::string nm = "s" + std::to_string(i);
        nodes[nm] = {{"type", "affine"}, {"a", OPS[g[i]].a}, {"b", OPS[g[i]].b}};
        edges.push_back({{"from", prev}, {"to", nm}});
        prev = nm;
    }
    return {{"schema_version", 1}, {"name", "affine_chain"},
            {"channels", {{"acc", {{"reducer", "overwrite"}, {"initial", 0}}}}},
            {"nodes", nodes}, {"edges", edges}};
}

using Sig = std::pair<double, double>;   // outputs on the input battery {0, 1}

// Analytic composition (hot path): apply the chain to x=0 and x=1.
static Sig sig_fast(const Genome& g) {
    double f0 = 0, f1 = 1;
    for (int i = 0; i < N; ++i) {
        const Aff& o = OPS[g[i]];
        f0 = o.a * f0 + o.b;
        f1 = o.a * f1 + o.b;
    }
    return {f0, f1};
}

// Ground truth: compile the topology and RUN it on one battery input.
static double run_engine(const json& core, const ng::NodeContext& ctx, double x) {
    auto          engine = ng::GraphEngine::build(core, ng::EngineConfig{.node_context = ctx});
    ng::RunConfig rc;
    rc.max_steps = N + 4;
    rc.input = {{"acc", x}};
    auto res = engine->run(rc);
    json av = res.has_channel("acc") ? res.channel<json>("acc") : json(0);
    return av.is_number() ? av.get<double>() : 0.0;
}
static Sig sig_engine(const Genome& g, const ng::NodeContext& ctx) {
    json core = build_core(g);
    return {run_engine(core, ctx, 0.0), run_engine(core, ctx, 1.0)};
}

// ---------------------------------------------------------- fitness models ---
// Signatures are recovered exactly by the {0,1} battery: f(0)=B, f(1)=A+B.
static bool sig_eq(const Sig& a, const Sig& b) {
    return std::abs(a.first - b.first) < 1e-9 && std::abs(a.second - b.second) < 1e-9;
}
static int match(const Genome& a, const Genome& b) {
    int m = 0;
    for (int i = 0; i < N; ++i) m += (a[i] == b[i]);
    return m;
}

// Global radius: a phenotype within GRADIUS mismatches of the global target is
// "on the global plateau" (reward 1.0). Small, so learning must find it nearly
// exactly — there is NO committed-space gradient leading to it.
constexpr int GRADIUS = 2;

struct Land {
    Genome tg, td;   // global target, decoy
};

static int hamming(const Genome& a, const Genome& b) { return N - match(a, b); }

// Reward of a FULLY-committed phenotype on the deceptive landscape:
//   * DECOY  — a broad, shallow hill: 0.85 * (1 - ham(·,Td)/N). Visible
//     everywhere, so blind selection and greedy write-back climb it.
//   * GLOBAL — a narrow, flat plateau: 1.0 within GRADIUS of Tg, else 0. It has
//     NO surrounding gradient, so it is invisible to committed-space search;
//     only lifetime learning (guessing plastic genes) can land on it, and only
//     from a genome whose committed genes don't already conflict with Tg.
// Td is placed a full N mismatches from Tg, so a genome pulled onto the decoy
// hill can never reach the global plateau without un-committing its genes.
static double reward(const Land& L, const Genome& full) {
    double decoy = 0.85 * (1.0 - (double)hamming(full, L.td) / N);
    double global = hamming(full, L.tg) <= GRADIUS ? 1.0 : 0.0;
    return std::max(decoy, global);
}
static bool is_global(const Land& L, const Genome& full) {
    return hamming(full, L.tg) <= GRADIUS;
}

// Lifetime learning: up to `trials` random guesses of the PLASTIC genes.
// Returns the best reward found and the phenotype that achieved it (for the
// Lamarckian write-back). Committed genes are held fixed — a committed gene
// that conflicts with Tg permanently blocks the global plateau, which is the
// whole point: Lamarckian write-back of a decoy-ward guess commits such genes
// and forecloses the global, while Baldwinian non-inheritance keeps the gene
// plastic and the global reachable.
struct Learned { double reward = 0; Genome best; bool hit_global = false; };
static Learned learn(const Land& L, const Genome& g, std::mt19937& rng, int trials) {
    std::vector<int> plastic;
    for (int i = 0; i < N; ++i) if (g[i] == PLASTIC) plastic.push_back(i);
    Learned out;
    out.reward = -1e9;   // first full phenotype always wins; out.best stays committed
    int T = plastic.empty() ? 1 : trials;
    for (int t = 0; t < T; ++t) {
        Genome full = g;
        for (int i : plastic) full[i] = rng() % K;
        double r = reward(L, full);
        if (r > out.reward) { out.reward = r; out.best = full; }
        if (is_global(L, full)) out.hit_global = true;
    }
    return out;
}

enum class Mode { Darwin, Baldwin, Lamarck };
static const char* mode_name(Mode m) {
    return m == Mode::Darwin ? "Darwinian " : m == Mode::Baldwin ? "Baldwinian" : "Lamarckian";
}

// One evolutionary run on the deceptive landscape.
//   assim_g : fraction of COMMITTED genes that equal the global target at the
//             END of the run (genetic assimilation of the global — a DURABLE
//             outcome, unlike a transient plateau hit that init luck can fake).
//   assim_d : fraction of COMMITTED genes that equal the decoy (trapped).
struct RunOut { double assim_g = 0, assim_d = 0; };
static RunOut evolve_run(const Land& L, Mode mode, uint32_t seed,
                         const ng::NodeContext& ctx) {
    (void)ctx;
    std::mt19937 rng(seed);
    const int P = 200, kGens = 100, kTrials = 30, kElite = 4;
    const bool plastic_ok = (mode != Mode::Darwin);

    auto rand_gene = [&](bool allow_plastic) -> int {
        if (allow_plastic && (rng() % 2 == 0)) return PLASTIC;   // ~50% "?"
        return rng() % K;
    };

    std::vector<Genome> pop(P, Genome(N));
    for (auto& ind : pop) for (int i = 0; i < N; ++i) ind[i] = rand_gene(plastic_ok);

    RunOut out;
    for (int gen = 0; gen < kGens; ++gen) {
        // (fitness, random tie-key, index). Fitness takes only a handful of
        // distinct values, so the top-half selection boundary falls deep inside
        // a tie group. Breaking ties by INDEX (or relying on std::sort's
        // unstable order) is an arbitrary, FIXED bias that can dominate the
        // outcome — so ties are broken by a per-individual draw from the run's
        // seeded rng. The bias is therefore fresh each generation and each seed,
        // and averages out across the 24-seed sweep: the reported means measure
        // the phenomenon, not one lucky ordering. Deterministic per seed.
        std::vector<std::tuple<double, uint32_t, int>> scored(P);
        for (int i = 0; i < P; ++i) {
            double fit;
            if (mode == Mode::Darwin) {
                fit = reward(L, pop[i]);
            } else {
                Learned lr = learn(L, pop[i], rng, kTrials);
                fit = lr.reward;
                // Lamarckian: the learned phenotype is written back into the
                // heritable genome — including decoy-ward guesses, which is
                // exactly what commits conflicting genes and forecloses the
                // global. Baldwinian: nothing is written; genes stay plastic.
                if (mode == Mode::Lamarck) pop[i] = lr.best;
            }
            scored[i] = {fit, (uint32_t)rng(), i};
        }

        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
            return std::get<1>(a) < std::get<1>(b);   // random, seeded tie-break
        });
        std::vector<Genome> parents;
        for (int i = 0; i < P / 2; ++i) parents.push_back(pop[std::get<2>(scored[i])]);
        std::vector<Genome> next;
        for (int i = 0; i < kElite; ++i) next.push_back(parents[i]);   // elitism
        while ((int)next.size() < P) {
            const Genome& pa = parents[rng() % parents.size()];
            const Genome& pb = parents[rng() % parents.size()];
            int cut = rng() % N;
            Genome child(N);
            for (int i = 0; i < N; ++i) child[i] = (i < cut ? pa[i] : pb[i]);
            for (int i = 0; i < N; ++i)                     // mutation
                if (rng() % 100 < 6) child[i] = rand_gene(plastic_ok);
            next.push_back(std::move(child));
        }
        pop = std::move(next);
    }

    long cg = 0, cd = 0, committed = 0;
    for (const auto& ind : pop)
        for (int i = 0; i < N; ++i)
            if (ind[i] != PLASTIC) { ++committed; cg += (ind[i] == L.tg[i]); cd += (ind[i] == L.td[i]); }
    out.assim_g = committed ? (double)cg / committed : 0.0;
    out.assim_d = committed ? (double)cd / committed : 0.0;
    return out;
}

// -------------------------------------------------------------- landscape ---
// Global Tg random; decoy Td placed a full N mismatches away (every gene
// differs), so a genome drawn onto the broad decoy hill cannot reach the
// global plateau without un-committing genes.
static Land make_land() {
    std::mt19937 rng(4242u);
    Land L;
    L.tg.resize(N);
    L.td.resize(N);
    for (int i = 0; i < N; ++i) L.tg[i] = rng() % K;
    for (int i = 0; i < N; ++i) L.td[i] = (L.tg[i] + 1 + (int)(rng() % (K - 1))) % K;  // Td[i] != Tg[i]
    return L;
}

int main() {
    register_affine();
    ng::NodeContext ctx;

    std::cout << "======= THE BEAST (baldwin · Darwin/Baldwin/Lamarck memetic) =======\n"
                 "Genome = wiring of an " << N << "-stage affine pipeline ("
              << K << " ops/stage), each a REAL NeoGraph topology. The GA optimizes a\n"
                 "DECEPTIVE landscape over the wiring: a broad decoy hill (0.85) vs a narrow,\n"
                 "gradient-free global plateau (1.0, within " << GRADIUS
              << " genes of the target).\n\n";

    Land L = make_land();

    // Honesty: prove each genome is a genuine harness the engine runs exactly as
    // the analytic model predicts (this validates the SUBSTRATE — that we evolve
    // real, faithfully-executed topologies — not the objective, which is the
    // Hamming landscape below).
    std::mt19937 cr(99);
    int agree = 0, samples = 200;
    for (int s = 0; s < samples; ++s) {
        Genome g(N);
        for (int i = 0; i < N; ++i) g[i] = cr() % K;
        if (sig_eq(sig_fast(g), sig_engine(g, ctx))) ++agree;
    }
    bool xcheck_ok = sig_eq(sig_fast(L.tg), sig_engine(L.tg, ctx)) && agree == samples;
    std::cout << "engine/analytic cross-check: " << agree << "/" << samples
              << " random topologies + the target execute exactly as modeled → "
              << (xcheck_ok ? "the substrate is a real, faithfully-run harness.\n\n"
                            : "MISMATCH!\n\n");

    const int kSeeds = 24;
    // A run has "assimilated the global" if the population's committed genes end
    // up mostly equal to Tg — a durable outcome, immune to init-luck plateau
    // hits. Threshold sits well above chance (1/K) and below full fixation.
    const double kSolved = 0.60;
    struct Agg { int solved = 0; double assim_g = 0, assim_d = 0; };
    auto sweep = [&](Mode m) {
        Agg a;
        for (uint32_t s = 0; s < (uint32_t)kSeeds; ++s) {
            RunOut r = evolve_run(L, m, 1000 + s, ctx);
            a.solved += r.assim_g >= kSolved ? 1 : 0;
            a.assim_g += r.assim_g;
            a.assim_d += r.assim_d;
        }
        printf("  %s | assimilated global %2d/%2d | mean committed → global %3.0f%%  "
               "decoy %3.0f%%\n", mode_name(m), a.solved, kSeeds,
               100 * a.assim_g / kSeeds, 100 * a.assim_d / kSeeds);
        return a;
    };

    std::cout << "----- deceptive landscape, " << kSeeds << " seeds -----\n"
                 "  (chance ≈ " << (int)(100.0 / K)
              << "% per target; 'assimilated global' = final committed genes ≥ "
              << (int)(100 * kSolved) << "% on Tg —\n"
                 "   a durable genome outcome, not a transient plateau hit.)\n";
    Agg dar = sweep(Mode::Darwin);
    Agg bal = sweep(Mode::Baldwin);
    Agg lam = sweep(Mode::Lamarck);

    double dg = dar.assim_g / kSeeds, bg = bal.assim_g / kSeeds, lg = lam.assim_g / kSeeds;
    double chance = 1.0 / K;

    // ---------------------------------------------------------- findings ---
    std::cout << "\n===== findings =====\n";
    // (1) The ROBUST claim: learning reaches a gradient-free global that blind
    // evolution cannot. Measured by the MEAN assimilation margin — far more
    // stable than a per-run threshold count, which init luck can nudge across.
    bool memetic_beats_blind = (bg > dg + 0.25) && (lg > dg + 0.25);
    std::cout << "1. Memetic vs blind: blind Darwin's committed genes assimilate the global "
              << (int)std::round(100 * dg) << "%\n   (barely above the "
              << (int)std::round(100 * chance) << "% chance floor — trapped on the decoy at "
              << (int)std::round(100 * dar.assim_d / kSeeds) << "%), while learning reaches\n   "
              << (int)std::round(100 * bg) << "% (Baldwin) / " << (int)std::round(100 * lg)
              << "% (Lamarck). The plateau has no committed-space gradient, so only\n"
                 "   lifetime learning — which searches a neighborhood — exposes it. Memetic "
              << (memetic_beats_blind ? "BEATS" : "does NOT beat") << " blind.\n";

    // (2) The Baldwinian control (Whitley): reported as measured, never gated.
    std::cout << "2. Baldwinian control (Whitley): final committed assimilation toward the "
                 "GLOBAL\n   was Baldwin " << (int)std::round(100 * bg) << "% vs Lamarck "
              << (int)std::round(100 * lg) << "%; toward the DECOY, Baldwin "
              << (int)std::round(100 * bal.assim_d / kSeeds) << "% vs Lamarck "
              << (int)std::round(100 * lam.assim_d / kSeeds) << "%.\n   ";
    if (bg > lg + 0.03)
        std::cout << "Non-inheritance assimilated the global MORE and the decoy LESS — the "
                     "Whitley\n   reversal reproduces here: Lamarckian write-back commits "
                     "decoy-ward guesses and\n   converges prematurely; Baldwinian keeps genes "
                     "plastic and diverse.\n";
    else
        std::cout << "Lamarckian did as well or slightly better — the expected outcome on a "
                     "landscape\n   that is deceptive but not adversarial: write-back's speed "
                     "outweighs its diversity\n   cost. Reproducing Whitley's Baldwin>Lamarck "
                     "REVERSAL needs a specifically\n   adversarial landscape (future work). "
                     "Reported as measured, not massaged.\n";

    // CI gate: ONLY the robust claim — blind Darwin is trapped near chance,
    // learning assimilates the global by a wide margin, and fitness is faithful.
    bool gate = xcheck_ok && (dg < chance + 0.20) && (bg > 0.65) && (lg > 0.65)
                && memetic_beats_blind;
    std::cout << "\nCI gate (robust claim — blind Darwin near the " << (int)std::round(100*chance)
              << "% chance floor, learners assimilate >65%\n  by a >25-pt margin, faithful "
                 "fitness): " << (gate ? "PASS" : "FAIL") << "\n";
    return gate ? 0 : 1;
}
