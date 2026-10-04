// NeoGraph Example 28: Corrective Retrieval-Augmented Generation (CRAG)
//
// Pattern from "Corrective Retrieval Augmented Generation" (Yan et al.,
// arXiv:2401.15884). Vanilla RAG retrieves once and generates blindly,
// even when the retrieved context is irrelevant. CRAG inserts an
// evaluator step that grades the retrieval and routes accordingly:
//
//     retrieve  ->  evaluate  ->  CORRECT    ->  refine(KB)        ->  generate
//                              \  AMBIGUOUS  ->  refine(KB) + web  ->  generate
//                               \ INCORRECT  ->  web only          ->  generate
//
// The web-search branch hits OpenRouter's built-in web_search tool over
// /api/v1/responses (OpenRouter-compatible Responses API).
// It is a hosted tool the model invokes server-side, so this example needs
// only OPENROUTER_API_KEY (no Brave / Tavily / DuckDuckGo key). To swap in
// a different search backend, only web_search() below changes — the routing
// logic is unchanged.
// Every LLM stage, including hosted web_search, uses the typed SDK
// Responses path. The complete ordered Outcome is retained for each call;
// text is projected only for the CRAG grader/refinement/composer inputs.
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_corrective_rag
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace neograph;

// =========================================================================
// Tiny knowledge base — six docs about NeoGraph. Hardcoded to keep the
// example single-file; a real deployment would back this with vector
// search (see example 12) or a SQL/document store.
// =========================================================================
struct Doc { std::string title; std::string body; };

static const std::vector<Doc> KB = {
    {"NeoGraph Overview",
     "NeoGraph is a C++20 graph execution and agent orchestration engine. "
     "Its C++ runtime does not require a Python runtime. Core executes "
     "admitted JSON graphs in Pregel-style supersteps: nodes read the "
     "step's state, and reducers merge their writes at the barrier."},

    {"NeoGraph Modules",
     "neograph::core provides graph loading, state, reducers and execution. "
     "neograph::program runs compiled Harness Programs with journaled "
     "effects. neograph::llm integrates the typed SchemaProvider SDK through "
     "pinned descriptors. Optional modules provide MCP, A2A and ACP "
     "protocols, utility queues, and PostgreSQL or SQLite persistence."},

    {"Send and Command",
     "Send enables dynamic fan-out: a node returns N Send objects to spawn "
     "N parallel tasks at runtime. Command lets a node update state and "
     "override routing in a single return value, bypassing static edges."},

    {"Checkpointing and HITL",
     "CheckpointStore persists graph state and its continuation; in-memory, "
     "PostgreSQL and SQLite backends are available. Human-in-the-loop uses "
     "interrupt_before, interrupt_after and dynamic NodeInterrupt. "
     "resume() follows the saved pending nodes; resuming a completed "
     "terminal continuation does not schedule another user turn."},

    {"Performance",
     "Engine overhead depends on graph shape, scheduling, compiler and "
     "host resources. The benchmarks directory records workloads and "
     "measurement conditions. Empty-graph timings do not establish "
     "model-call latency, throughput or memory use for an agent workload."},

    {"License",
     "NeoGraph itself is MIT-licensed. Vendored dependencies: asio "
     "(Boost Software License), yyjson (MIT), cpp-httplib (MIT), "
     "moodycamel::concurrentqueue (BSD-2-Clause), cppdotenv (MIT), "
     "Clay (zlib)."}
};

// =========================================================================
// Token utilities
// =========================================================================
static std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// =========================================================================
// Retrieval — keyword-overlap scoring. Real CRAG uses dense embeddings;
// this example focuses on the routing logic, not retrieval quality.
// =========================================================================
struct Hit { std::size_t idx; int score; };

static std::vector<Hit> retrieve(const std::string& q, std::size_t top_k = 2) {
    auto query_tokens = tokenize(q);
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < KB.size(); ++i) {
        std::string body_lc = lowercase(KB[i].title + " " + KB[i].body);
        int score = 0;
        for (const auto& t : query_tokens) {
            // Skip stopword-ish short tokens — pure noise here.
            if (t.size() >= 4 && body_lc.find(t) != std::string::npos) ++score;
        }
        if (score > 0) hits.push_back({i, score});
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b){ return a.score > b.score; });
    if (hits.size() > top_k) hits.resize(top_k);
    return hits;
}

static std::string format_hits(const std::vector<Hit>& hits) {
    if (hits.empty()) return "(no documents matched the query)";
    std::ostringstream os;
    for (const auto& h : hits) {
        os << "## " << KB[h.idx].title << "\n" << KB[h.idx].body << "\n\n";
    }
    return os.str();
}

// =========================================================================
// LLM-driven steps
// =========================================================================
enum class Verdict { Correct, Ambiguous, Incorrect };

// Retain every complete native result, including failure partials. The text
// views below are algorithm inputs, never substitutes for replay authority.
using StageOutcomes = std::vector<sp::runtime::Result>;
static std::shared_ptr<const sp::Outcome> run_stage(
    Provider& provider, ProviderRequest request, StageOutcomes& outcomes) {
    outcomes.push_back(provider.invoke(std::move(request)));
    return examples::require_outcome(outcomes.back());
}

// Evaluator: grades whether `context` actually answers `question`.
// CRAG's key insight — the LLM that *answers* shouldn't be the one that
// trusts retrieval blindly; insert a dedicated grader that says "this
// retrieval is good / partial / garbage".
static Verdict evaluate(Provider& p, const std::string& question,
                        const std::string& context, StageOutcomes& outcomes) {
    std::vector<sp::Message> messages;
    messages.push_back(examples::message(sp::Role::System,
        "You grade retrieved documents against a question. Reply with "
        "EXACTLY one word.\n"
        " - CORRECT: every part of the question is fully answered by the "
        "documents, with no missing facts.\n"
        " - AMBIGUOUS: the documents address some part of the question "
        "but at least one specific fact, comparison, or sub-question "
        "is missing or only vaguely covered.\n"
        " - INCORRECT: the documents are off-topic for the question.\n"
        "Be strict. If the question asks for two things and the "
        "documents only cover one, the answer is AMBIGUOUS, not CORRECT."));
    messages.push_back(examples::message(sp::Role::User,
        "Question: " + question + "\n\nDocuments:\n" + context));
    ProviderControls controls;
    controls.temperature = 0.0;
    const auto outcome = run_stage(p, make_provider_request(
        p, "~deepseek/deepseek-v4-flash-latest", std::move(messages), {}, controls),
        outcomes);
    auto raw = examples::visible_text(*outcome);

    // Tolerant parsing — small models occasionally pad ("CORRECT.",
    // "ambiguous - the docs..."). Substring is enough.
    std::string lc = lowercase(raw);
    if (lc.find("correct")   != std::string::npos &&
        lc.find("incorrect") == std::string::npos) return Verdict::Correct;
    if (lc.find("ambiguous") != std::string::npos) return Verdict::Ambiguous;
    return Verdict::Incorrect;
}

// Split docs into atomic strips. The paper's "decompose" step uses
// strip-level granularity (a strip = "one or two sentences" in
// Yan et al. §3.4). We split on sentence terminators (. ! ?) plus
// paragraph breaks, preserving doc-section markers ("## title") as
// their own strips so they survive recomposition unchanged.
static std::vector<std::string> split_into_strips(const std::string& text) {
    std::vector<std::string> strips;
    std::string cur;
    auto flush = [&]() {
        // Trim leading whitespace.
        size_t a = 0;
        while (a < cur.size() && (cur[a] == ' ' || cur[a] == '\t' || cur[a] == '\n')) ++a;
        std::string s = cur.substr(a);
        if (!s.empty()) strips.push_back(std::move(s));
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        cur.push_back(c);
        if (c == '\n') {
            // Section header on its own line — keep as separate strip.
            if (cur.size() >= 2 && cur[0] == '#') {
                flush();
            } else if (i + 1 < text.size() && text[i + 1] == '\n') {
                // Paragraph break.
                flush();
            }
        } else if (c == '.' || c == '!' || c == '?') {
            // End of sentence — flush after the punctuation.
            if (i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\n')) {
                flush();
            }
        }
    }
    if (!cur.empty()) flush();
    return strips;
}

// Refine (Yan et al. §3.4 "knowledge refinement"): decompose docs into
// strips, score each strip's relevance to the question with the same
// kind of evaluator that ran in evaluate(), drop low-scoring strips,
// recompose the survivors in original order.
//
// In the paper the strip evaluator is the same fine-tuned T5 used for
// retrieval grading; here we use one bulk-classify LLM call (one
// KEEP/DROP per strip) for cost — the algorithmic shape (per-strip
// independent decision, original-order recomposition) matches the
// paper. This is structurally different from "ask the LLM to extract
// relevant sentences" — that fold-into-one-prompt approach was the
// pre-audit form and let the model paraphrase or invent.
static std::string refine(Provider& p, const std::string& question,
                          const std::string& kb_context, StageOutcomes& outcomes) {
    auto strips = split_into_strips(kb_context);
    if (strips.empty()) return "";

    std::ostringstream listing;
    for (size_t i = 0; i < strips.size(); ++i) {
        listing << "[" << (i + 1) << "] " << strips[i] << "\n";
    }

    std::vector<sp::Message> messages;
    messages.push_back(examples::message(sp::Role::System,
        "You are scoring text strips for relevance to a question. For "
        "EACH numbered strip, output exactly one line of the form:\n"
        "  N. KEEP    (strip directly helps answer the question)\n"
        "  N. DROP    (strip is off-topic or only loosely related)\n"
        "Be strict — only KEEP strips that contain a fact, definition, "
        "or relationship the answerer would actually quote. Output the "
        "verdicts in numerical order, one per line, nothing else."));
    messages.push_back(examples::message(sp::Role::User,
        "Question: " + question + "\n\nStrips:\n" + listing.str()));
    ProviderControls controls;
    controls.temperature = 0.0;
    const auto outcome = run_stage(p, make_provider_request(
        p, "~deepseek/deepseek-v4-flash-latest", std::move(messages), {}, controls),
        outcomes);
    auto verdicts = examples::visible_text(*outcome);

    // Parse "N. KEEP" / "N. DROP" lines tolerantly.
    std::set<size_t> kept;
    std::istringstream vs(verdicts);
    std::string line;
    while (std::getline(vs, line)) {
        // Find the leading integer.
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t num_start = i;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i == num_start) continue;
        size_t n = std::stoul(line.substr(num_start, i - num_start));
        std::string lc = lowercase(line);
        if (lc.find("keep") != std::string::npos) kept.insert(n);
    }

    // Recompose in original order.
    std::ostringstream out;
    bool first = true;
    for (size_t i = 0; i < strips.size(); ++i) {
        if (!kept.count(i + 1)) continue;
        if (!first) out << ' ';
        out << strips[i];
        first = false;
    }
    auto result = out.str();
    return result.empty() ? "(no strip survived refinement)" : result;
}

// Query rewriter (Yan et al. §3.5) — rewrites the user question into a
// concise keyword query suitable for a web search engine. The paper's
// INCORRECT branch routes through this *before* invoking external
// search; previously this example fed the raw question to OpenRouter's
// hosted web_search tool, which works but skips the documented rewriting
// step.
static std::string rewrite_query(Provider& p, const std::string& question,
                                 StageOutcomes& outcomes) {
    std::vector<sp::Message> messages;
    messages.push_back(examples::message(sp::Role::System,
        "Rewrite the user's question as a concise keyword query for a "
        "web search engine. Drop articles, modal verbs, polite framing. "
        "Output ONLY the keyword query — no quotes, no commentary, no "
        "trailing punctuation."));
    messages.push_back(examples::message(sp::Role::User, question));
    ProviderControls controls;
    controls.temperature = 0.0;
    controls.max_output_tokens = 512;
    controls.reasoning_effort = "low";
    const auto outcome = run_stage(p, make_provider_request(
        p, "~deepseek/deepseek-v4-flash-latest", std::move(messages), {}, controls),
        outcomes);
    auto out = examples::visible_text(*outcome);
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return question;
    const auto last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

// The SDK declares hosted tools independently from client-executed functions.
// Keep the native hosted calls, citations, ordered parts, and nullable usage
// in outcomes; project text only as input to the strip-refinement algorithm.
static std::string web_search(Provider& provider, const std::string& question,
                              StageOutcomes& outcomes) {
    auto request = make_provider_request(provider, examples::openrouter_model,
        {examples::message(sp::Role::User, question)});
    auto& responses = std::get<sp::responses::Request>(request.payload);
    responses.hosted_tools.emplace_back(sp::responses::WebSearchTool{});
    const auto outcome = run_stage(provider, std::move(request), outcomes);
    auto answer = examples::visible_text(*outcome);
    if (answer.empty())
        throw std::runtime_error("web_search returned no visible text");
    return answer;
}

// Final answer composer.
static std::string generate(Provider& p, const std::string& question,
                            const std::string& context, StageOutcomes& outcomes) {
    std::vector<sp::Message> messages;
    messages.push_back(examples::message(sp::Role::System,
        "Answer the question using ONLY the provided context. Be concise. "
        "If the context is insufficient, say so explicitly."));
    messages.push_back(examples::message(sp::Role::User,
        "Question: " + question + "\n\nContext:\n" + context));
    ProviderControls controls;
    controls.temperature = 0.2;
    const auto outcome = run_stage(p, make_provider_request(
        p, "~deepseek/deepseek-v4-flash-latest", std::move(messages), {}, controls),
        outcomes);
    return examples::visible_text(*outcome);
}

// =========================================================================
// Main
// =========================================================================
int main() {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        auto provider = examples::make_openrouter_provider(
            api_key, "responses", std::chrono::seconds(180));

        // Three questions chosen to exercise each branch of the router.
        // The verdict is LLM-driven, so the route a given run takes can
        // vary slightly; what's stable is that questions whose answer
        // sits cleanly inside the KB go through CORRECT, questions the
        // KB doesn't touch at all go through INCORRECT, and partial-fit
        // questions trip AMBIGUOUS.
        const std::vector<std::string> questions = {
            "What modules does NeoGraph have, and what does each do?",
            "Who won the men's FIFA World Cup in 2018?",
            "What checkpoint backends does NeoGraph ship, "
            "and what's their per-row write throughput in inserts per second?",
        };

        std::cout << "\n╔═══════════════════════════════════════════════════════╗\n"
                  <<   "║  Example 28: Corrective RAG over /v1/responses        ║\n"
                  <<   "║  arXiv:2401.15884                                     ║\n"
                  <<   "╚═══════════════════════════════════════════════════════╝\n";

        for (const auto& q : questions) {
            std::cout << "\n─────────────────────────────────────────────────────────\n"
                      << "Q: " << q << "\n";
            StageOutcomes outcomes;

            // 1. Retrieve from KB
            auto hits = retrieve(q);
            std::string kb_ctx = format_hits(hits);
            std::cout << "[retrieve] " << hits.size() << " hit(s)";
            if (!hits.empty())
                std::cout << " — top=\"" << KB[hits.front().idx].title << "\"";
            std::cout << "\n";

            // 2. Evaluate
            Verdict v = hits.empty()
                ? Verdict::Incorrect
                : evaluate(*provider, q, kb_ctx, outcomes);
            const char* tag = v == Verdict::Correct   ? "CORRECT"
                            : v == Verdict::Ambiguous ? "AMBIGUOUS"
                                                      : "INCORRECT";
            std::cout << "[evaluate] " << tag << "\n";

            // 3. Route + assemble final context
            //
            // Per Yan et al. §3.5 "Algorithm 1":
            //   - CORRECT   → refine(KB) → generate
            //   - INCORRECT → rewrite query → web search → refine(web) → generate
            //   - AMBIGUOUS → refine(KB) ∪ refine(web) → generate
            //
            // External (web) knowledge goes through the same refinement as
            // KB knowledge — the previous form fed raw web_search output
            // straight to the generator.
            std::string final_ctx;
            switch (v) {
                case Verdict::Correct:
                    std::cout << "[route   ] refine(KB) → generate\n";
                    final_ctx = refine(*provider, q, kb_ctx, outcomes);
                    break;
                case Verdict::Incorrect: {
                    auto rewritten = rewrite_query(*provider, q, outcomes);
                    std::cout << "[rewrite ] '" << rewritten << "'\n";
                    std::cout << "[route   ] web → refine(web) → generate (KB rejected)\n";
                    auto web_raw = web_search(*provider, rewritten, outcomes);
                    final_ctx    = refine(*provider, q, web_raw, outcomes);
                    break;
                }
                case Verdict::Ambiguous: {
                    auto rewritten = rewrite_query(*provider, q, outcomes);
                    std::cout << "[rewrite ] '" << rewritten << "'\n";
                    std::cout << "[route   ] refine(KB) + refine(web) → generate\n";
                    auto web_raw = web_search(*provider, rewritten, outcomes);
                    final_ctx =
                        "## From the local knowledge base\n"
                        + refine(*provider, q, kb_ctx, outcomes) +
                        "\n\n## From external web search\n"
                        + refine(*provider, q, web_raw, outcomes);
                    break;
                }
            }

            // 4. Generate
            auto answer = generate(*provider, q, final_ctx, outcomes);
            std::cout << "\nA: " << answer << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
