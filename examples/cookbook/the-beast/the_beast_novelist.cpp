// NeoGraph Cookbook — "The Beast", NOVELIST (prompt in → a light-novel-length
// manuscript out, as plain .txt)
// =================================================================
// The simplest genuinely-useful writing harness: give it a premise, get back a
// whole light-novel-sized manuscript. The point is the cure for "lost in the
// middle": a long story is NOT written in one giant context. It is a small graph
// over an EXPLICIT story state —
//
//   channels:  premise · outline · bible · summary · book · idx · total
//
// so each chapter is generated fresh against the *compact* externalized state
// (the outline beat, the story bible, and a running summary) instead of the
// whole prior text. The model never has to re-read 60k characters to remember
// who a character is — it reads the bible channel.
//
// The graph:  __start__ → planner → writer ⟲ (self-loop via goto until the last
// chapter, then falls through to __end__). Two native nodes:
//   * planner — premise → an outline (chapter beats) + an initial story bible.
//   * writer  — reads {outline[idx], bible, summary} → writes chapter idx into
//     `book`, and UPDATES `summary` and `bible` so the next iteration stays
//     grounded. Loops itself with a Command goto until idx+1 == total.
// Effect contracts are declared, so the coherence gate proves the wiring before
// a word is written (no dangling stage, every state channel actually consumed).
//
// Offline (no key): a deterministic STUB planner/writer runs the exact same
// graph — so the pipeline (state threading, the goto loop, accumulation, the
// .txt output) is verifiable without a network. With OPENROUTER_API_KEY: the
// model writes the actual prose.
//
// Build:  cmake --build build --target cookbook_the_beast_novelist
// Run:    ./build/cookbook_the_beast_novelist "your premise here"   [chapters]
//         OPENROUTER_API_KEY=... ./build/cookbook_the_beast_novelist "..." 12

#include <neograph/neograph.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/async/run_sync.h>

#include <cppdotenv/dotenv.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// Provider shared by the nodes (null → offline stub mode).
static std::shared_ptr<neograph::Provider> g_prov;

static std::string ask(const std::string& system, const std::string& user, int max_tokens) {
    neograph::CompletionParams p;
    p.model = "deepseek/deepseek-v4-flash";
    p.temperature = 0.8f;   // prose wants some warmth
    p.max_tokens = max_tokens;
    p.messages = {{"system", system}, {"user", user}};
    return neograph::async::run_sync(g_prov->invoke(p, nullptr)).message.content;
}
// Extract the block after marker `a` and before marker `b` (or end).
static std::string slice(const std::string& s, const std::string& a, const std::string& b) {
    auto i = s.find(a);
    if (i == std::string::npos) return "";
    i += a.size();
    auto j = b.empty() ? std::string::npos : s.find(b, i);
    return s.substr(i, j == std::string::npos ? std::string::npos : j - i);
}
static std::string trim(std::string x) {
    auto ns = x.find_first_not_of(" \t\r\n");
    auto ne = x.find_last_not_of(" \t\r\n");
    return ns == std::string::npos ? "" : x.substr(ns, ne - ns + 1);
}

// ---------------------------------------------- per-chapter style evolution --
// To give each chapter a distinct FEEL, the writer evolves a small STYLE GENOME
// per chapter: a point in a 5-dimensional style space. The fitness is NOVELTY —
// distance from the styles already used — so a batch of candidates is evolved
// (a mini-GA) to be maximally UNLIKE the chapters before it. Batch-generate,
// batch-evolve, commit the winner: the memetic loop from `baldwin`, aimed at
// variety instead of a target. Deterministic (seeded per chapter).
static const std::vector<std::vector<const char*>> STYLE_DIMS = {
    {"first-person", "close third-person", "omniscient third", "epistolary/journal"},
    {"past tense", "present tense"},
    {"tense and foreboding", "melancholic and lyrical", "wry and whimsical",
     "warm and intimate", "cold and clinical"},
    {"dialogue-driven", "introspective interiority", "kinetic action",
     "atmospheric sensory detail"},
    {"slow-burn", "brisk and propulsive", "staccato fragments"}};
using Style = std::array<int, 5>;
static int sdist(const Style& a, const Style& b) {
    int d = 0; for (int i = 0; i < 5; ++i) d += (a[i] != b[i]); return d;
}
// Novelty = min distance to any prior style (higher = more different).
static int novelty(const Style& g, const std::vector<Style>& prior) {
    if (prior.empty()) return 5;
    int m = 5; for (const auto& p : prior) m = std::min(m, sdist(g, p)); return m;
}
static Style rand_style(std::mt19937& rng) {
    Style g; for (int i = 0; i < 5; ++i) g[i] = rng() % (int)STYLE_DIMS[i].size(); return g;
}
// Mini-GA: evolve a batch of style genomes to maximize novelty vs `prior`.
static Style evolve_style(const std::vector<Style>& prior, uint32_t seed) {
    std::mt19937 rng(seed);
    const int P = 12, G = 8;
    std::vector<Style> pop(P);
    for (auto& g : pop) g = rand_style(rng);
    Style best = pop[0]; int best_f = -1;
    for (int gen = 0; gen < G; ++gen) {
        std::sort(pop.begin(), pop.end(), [&](const Style& a, const Style& b) {
            return novelty(a, prior) > novelty(b, prior); });
        if (novelty(pop[0], prior) > best_f) { best_f = novelty(pop[0], prior); best = pop[0]; }
        std::vector<Style> next(pop.begin(), pop.begin() + P / 2);   // elite half
        while ((int)next.size() < P) {                               // mutate elites
            Style c = next[rng() % (P / 2)];
            int dim = rng() % 5; c[dim] = rng() % (int)STYLE_DIMS[dim].size();
            next.push_back(c);
        }
        pop = std::move(next);
    }
    return best;
}
static std::string describe(const Style& g) {
    std::string s;
    for (int i = 0; i < 5; ++i) s += (i ? ", " : "") + std::string(STYLE_DIMS[i][g[i]]);
    return s;
}
static std::string style_sig(const Style& g) {
    std::ostringstream o; for (int i = 0; i < 5; ++i) o << (i ? "," : "") << g[i]; return o.str();
}
static std::vector<Style> parse_styles(const std::string& s) {
    std::vector<Style> out;
    std::istringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ';')) {
        if (tok.empty()) continue;
        Style g{}; std::istringstream ts(tok); std::string n; int i = 0;
        while (std::getline(ts, n, ',') && i < 5) g[i++] = std::atoi(n.c_str());
        if (i == 5) out.push_back(g);
    }
    return out;
}

// ------------------------------------------------------------------ planner --
struct PlannerNode : ng::GraphNode {
    std::string name_;
    explicit PlannerNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        std::string premise = in.state.get("premise").is_string()
                                  ? in.state.get("premise").get<std::string>() : "";
        int total = in.state.get("total").is_number() ? in.state.get("total").get<int>() : 12;
        std::string outline, bible;
        if (g_prov) {
            std::string reply = ask(
                "You are a light-novel author's planning assistant. Reply in the LANGUAGE OF "
                "THE PREMISE. Produce two blocks and nothing else:\n"
                "###OUTLINE###\nA numbered list of exactly " + std::to_string(total) +
                " chapter beats (one line each), a clear arc with setup, rising tension, "
                "climax, resolution.\n"
                "###BIBLE###\nThe story bible: main characters (name, one-line trait), the "
                "world/setting, and the tone. Concise, factual, the canon later chapters must "
                "not contradict.",
                "Premise:\n" + premise, 2000);
            outline = trim(slice(reply, "###OUTLINE###", "###BIBLE###"));
            bible = trim(slice(reply, "###BIBLE###", ""));
        }
        if (outline.empty()) {   // offline stub or parse failure
            for (int i = 1; i <= total; ++i)
                outline += std::to_string(i) + ". [stub beat " + std::to_string(i) +
                           "] the story advances one step.\n";
            bible = "[stub bible] Protagonist: A. World: the premise's world. Tone: earnest.";
        }
        ng::NodeOutput o;
        o.writes.push_back({"outline", outline});
        o.writes.push_back({"bible", bible});
        o.writes.push_back({"idx", 0});
        // static edge planner→writer carries us on; no goto needed.
        co_return o;
    }
    std::string get_name() const override { return name_; }
};

// ------------------------------------------------------------------- writer --
struct WriterNode : ng::GraphNode {
    std::string name_;
    explicit WriterNode(std::string n) : name_(std::move(n)) {}
    static std::string sget(ng::NodeInput& in, const char* k) {
        auto v = in.state.get(k); return v.is_string() ? v.get<std::string>() : "";
    }
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput in) override {
        int idx = in.state.get("idx").is_number() ? in.state.get("idx").get<int>() : 0;
        int total = in.state.get("total").is_number() ? in.state.get("total").get<int>() : 12;
        std::string premise = sget(in, "premise"), outline = sget(in, "outline"),
                    bible = sget(in, "bible"), summary = sget(in, "summary"), book = sget(in, "book");

        // Batch-generate + batch-evolve a STYLE for this chapter, maximizing
        // novelty vs the styles already used → each chapter gets a distinct feel.
        std::vector<Style> prior = parse_styles(sget(in, "styles_used"));
        Style style = evolve_style(prior, (uint32_t)(idx + 1) * 2654435761u);
        std::string style_desc = describe(style);

        std::string chapter, new_summary = summary, new_bible = bible;
        if (g_prov) {
            std::string reply = ask(
                "You are writing one chapter of a light novel, in the LANGUAGE OF THE PREMISE. "
                "Stay strictly consistent with the story bible and the summary so far — do not "
                "contradict established facts. Write vivid, scene-driven prose of about "
                "3000-5000 characters. Output exactly three blocks:\n"
                "###CHAPTER###\n<the chapter prose>\n"
                "###SUMMARY###\n<a rewritten running summary of the WHOLE story so far, <=200 "
                "words, that the next chapter will rely on>\n"
                "###BIBLE###\n<the full updated story bible incorporating any new canon; keep it "
                "concise>",
                "Premise:\n" + premise + "\n\nOutline:\n" + outline +
                "\n\nStory bible (canon):\n" + bible + "\n\nSummary so far:\n" +
                (summary.empty() ? "(none yet — this is the opening)" : summary) +
                "\n\nWrite CHAPTER " + std::to_string(idx + 1) + " of " + std::to_string(total) +
                ". Follow beat " + std::to_string(idx + 1) + " of the outline."
                "\n\nRender THIS chapter in a deliberately distinct STYLE so it FEELS different "
                "from the other chapters — commit fully to: " + style_desc +
                ". (Keep the STORY consistent; only the texture changes.)", 6000);
            chapter = trim(slice(reply, "###CHAPTER###", "###SUMMARY###"));
            std::string s = trim(slice(reply, "###SUMMARY###", "###BIBLE###"));
            std::string b = trim(slice(reply, "###BIBLE###", ""));
            if (!s.empty()) new_summary = s;
            if (!b.empty()) new_bible = b;
            if (chapter.empty()) chapter = trim(reply);   // fallback: whole reply is the chapter
        } else {
            chapter = "[stub chapter " + std::to_string(idx + 1) + "] Grounded in the bible and "
                      "the summary, the story advances one beat without contradiction.";
            new_summary = summary + (summary.empty() ? "" : " ") + "Ch." + std::to_string(idx + 1) +
                          " happened.";
        }

        std::string header = "\n\n\t\t— Chapter " + std::to_string(idx + 1) + " —\n\n";
        ng::NodeOutput o;
        o.writes.push_back({"book", book + header + chapter});
        o.writes.push_back({"summary", new_summary});
        o.writes.push_back({"bible", new_bible});
        o.writes.push_back({"idx", idx + 1});
        std::string styles_used = sget(in, "styles_used");
        o.writes.push_back({"styles_used",
                            styles_used + (styles_used.empty() ? "" : ";") + style_sig(style)});
        std::cerr << "  … chapter " << (idx + 1) << "/" << total << "  [style: " << style_desc
                  << "]  (" << chapter.size() << " chars)\n";
        if (idx + 1 < total) o.command = ng::Command{"writer", {}};   // loop; else → __end__
        co_return o;
    }
    std::string get_name() const override { return name_; }
};

static void register_nodes() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "planner",
            [](const std::string& n, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new PlannerNode(n)); },
            json::object(),
            json::parse(R"({"reads":["premise","total"],"writes":["outline","bible","idx"]})"));
        ng::NodeFactory::instance().register_type(
            "writer",
            [](const std::string& n, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new WriterNode(n)); },
            json::object(),
            json::parse(R"({"reads":["premise","outline","bible","summary","book","idx","total","styles_used"],
                            "writes":["book","summary","bible","idx","styles_used"]})"));
        return true;
    }();
    (void)once;
}

static json ch(const json& initial) { return {{"reducer", "overwrite"}, {"initial", initial}}; }

int main(int argc, char** argv) {
    register_nodes();
    ng::NodeContext ctx;

    std::string premise = argc > 1 ? argv[1]
        : "A quiet high-school librarian discovers the returned books whisper the futures of "
          "whoever borrows them next — and one prophecy is her own death in a week.";
    int total = argc > 2 ? std::max(2, std::atoi(argv[2])) : 12;

    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (key && *key)
        g_prov = neograph::llm::OpenAIProvider::create_shared(
            {.api_key = key, .base_url = "https://openrouter.ai/api",
             .default_model = "deepseek/deepseek-v4-flash"});

    std::cout << "===== THE BEAST (novelist · prompt → light-novel .txt) =====\n"
                 "Premise: " << premise.substr(0, 100) << (premise.size() > 100 ? "…" : "") << "\n"
                 "Chapters: " << total << "   Learner: "
              << (g_prov ? "the model (deepseek-v4-flash)" : "deterministic stub (no key)") << "\n"
                 "State channels (the cure for lost-in-the-middle): premise·outline·bible·"
                 "summary·book·idx·total\n\n";

    // The harness. planner → writer, writer self-loops via goto until total.
    json core = {
        {"schema_version", 1}, {"name", "novelist"},
        {"channels", {
            {"premise", ch(premise)}, {"total", ch(total)}, {"idx", ch(0)},
            {"outline", ch("")}, {"bible", ch("")}, {"summary", ch("")}, {"book", ch("")},
            {"styles_used", ch("")}}},
        {"nodes", {{"planner", {{"type", "planner"}}}, {"writer", {{"type", "writer"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "planner"}},
                                {{"from", "planner"}, {"to", "writer"}} })}};

    // Coherence gate: prove the wiring before a word is written.
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);
        auto rep = ng::GraphValidator::validate(cg);
        if (rep.has_errors()) { std::cerr << "harness rejected by the gate:\n" << rep.summary() << "\n"; return 1; }
    } catch (const std::exception& e) { std::cerr << "compile failed: " << e.what() << "\n"; return 1; }
    std::cout << "harness passed the coherence gate. writing";
    std::cout << (g_prov ? " (live — this takes a few minutes)…\n" : " (stub)…\n");

    auto engine = ng::GraphEngine::compile(core, ctx);
    ng::RunConfig rc; rc.max_steps = total + 5;
    auto res = engine->run(rc);
    std::string book = res.has_channel("book") ? res.channel<json>("book").get<std::string>() : "";

    // Emit the manuscript as plain .txt.
    std::string fname = "novel_" + std::to_string(total) + "ch" + (g_prov ? "" : "_stub") + ".txt";
    std::ofstream out(fname);
    out << premise << "\n" << std::string(60, '=') << "\n" << book << "\n";
    out.close();
    std::error_code ec;
    auto abs = std::filesystem::absolute(fname, ec);
    std::cout << "\ndone — " << book.size() << " characters across " << total << " chapters.\n"
                 "manuscript: " << (ec ? fname : abs.string()) << "\n";
    if (!g_prov)
        std::cout << "(stub run — set OPENROUTER_API_KEY for real prose; the pipeline above is "
                     "the same graph either way.)\n";
    return 0;
}
