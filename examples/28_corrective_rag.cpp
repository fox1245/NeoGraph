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
// The web-search branch hits OpenAI's built-in web_search tool over
// /v1/responses (developers.openai.com/api/docs/guides/tools-web-search)
// — a hosted tool the model invokes server-side, so this example needs
// only OPENAI_API_KEY (no Brave / Tavily / DuckDuckGo key). To swap in
// a different search backend, only web_search() below changes — the
// routing logic is unchanged.
//
// Implementation note: the LLM steps that take *function* tools
// (evaluate / refine / generate — all zero-tool here) go through
// SchemaProvider("openai_responses"). The web-search call uses a
// *hosted* built-in tool, which SchemaProvider's function-shape
// abstraction doesn't model, so it's a direct POST to /v1/responses
// via neograph::async::async_post. Both code paths hit the same
// endpoint and exercise the chunked-response handling in the
// async HTTP client.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_corrective_rag
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/async/http_client.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/run_sync.h>

#include <cppdotenv/dotenv.hpp>

#include <asio/this_coro.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
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
     "NeoGraph is a C++17 graph-based agent orchestration engine that "
     "brings LangGraph-level capabilities to C++ with no Python dependency. "
     "Workflows are defined as JSON; the engine executes them with "
     "Pregel-style super-steps."},

    {"NeoGraph Modules",
     "NeoGraph ships four modules: neograph::core (graph engine, JSON "
     "loader, scheduler), neograph::llm (OpenAIProvider + SchemaProvider "
     "for any vendor), neograph::mcp (MCP client over HTTP and stdio), "
     "and neograph::util (lock-free RequestQueue for backpressure)."},

    {"Send and Command",
     "Send enables dynamic fan-out: a node returns N Send objects to spawn "
     "N parallel tasks at runtime. Command lets a node update state and "
     "override routing in a single return value, bypassing static edges."},

    {"Checkpointing and HITL",
     "Every super-step snapshots full state through CheckpointStore "
     "(InMemory and Postgres backends shipped). Human-in-the-loop is "
     "supported via interrupt_before / interrupt_after declarations and "
     "dynamic NodeInterrupt exceptions; resume() continues from any "
     "saved checkpoint."},

    {"Performance"
,
     "Engine overhead is measured at ~5 us per super-step on a Release "
     "-O3 -DNDEBUG build. In the burst-concurrency benchmark "
     "(1 CPU / 512 MB cgroup), 10000 parallel agent runs complete in "
     "52 ms with a 5.5 MB peak RSS — the asio::thread_pool dispatch "
     "model scales linearly until the CPU quota becomes the bottleneck."},

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

// Evaluator: grades whether `context` actually answers `question`.
// CRAG's key insight — the LLM that *answers* shouldn't be the one that
// trusts retrieval blindly; insert a dedicated grader that says "this
// retrieval is good / partial / garbage".
static Verdict evaluate(Provider& p, const std::string& question,
                        const std::string& context) {
    CompletionParams cp;
    cp.messages.push_back({"system",
        "You grade retrieved documents against a question. Reply with "
        "EXACTLY one word.\n"
        " - CORRECT: every part of the question is fully answered by the "
        "documents, with no missing facts.\n"
        " - AMBIGUOUS: the documents address some part of the question "
        "but at least one specific fact, comparison, or sub-question "
        "is missing or only vaguely covered.\n"
        " - INCORRECT: the documents are off-topic for the question.\n"
        "Be strict. If the question asks for two things and the "
        "documents only cover one, the answer is AMBIGUOUS, not CORRECT."});
    cp.messages.push_back({"user",
        "Question: " + question + "\n\nDocuments:\n" + context});
    cp.temperature = 0.0f;
    auto raw = p.complete(cp).message.content;

    // Tolerant parsing — small models occasionally pad ("CORRECT.",
    // "ambiguous - the docs..."). Substring is enough.
    std::string lc = lowercase(raw);
    if (lc.find("correct")   != std::string::npos &&
        lc.find("incorrect") == std::string::npos) return Verdict::Correct;
    if (lc.find("ambiguous") != std::string::npos) return Verdict::Ambiguous;
    return Verdict::Incorrect;
}

// Refine: pull just the relevant sentences out of the retrieved docs so
// the generator isn't distracted by peripheral content. The paper calls
// this "knowledge refinement".
static std::string refine(Provider& p, const std::string& question,
                          const std::string& kb_context) {
    CompletionParams cp;
    cp.messages.push_back({"system",
        "Extract from the documents only the sentences that directly "
        "address the question. Return them as a bulleted list. Quote "
        "verbatim — do not paraphrase or invent."});
    cp.messages.push_back({"user",
        "Question: " + question + "\n\nDocuments:\n" + kb_context});
    cp.temperature = 0.0f;
    return p.complete(cp).message.content;
}

// Coroutine wrapped in a free function so the GCC frontend doesn't
// trip on `co_await` inside a function-scope lambda (verified ICE in
// gcc-13 on the build matrix). main() wraps this in run_sync.
//
// Headers are pre-built and bound to a local before the first
// co_await — gcc-13 ICEs on certain initializer-list temporaries
// crossing a coroutine suspension; matching the OpenAIProvider
// pattern (build first, std::move at the call site) sidesteps it.
static asio::awaitable<neograph::async::HttpResponse>
post_web_search(neograph::async::AsyncEndpoint endpoint,
                std::string body,
                std::string auth_header_value) {
    namespace na = neograph::async;
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", std::move(auth_header_value)},
        {"Content-Type",  "application/json"},
    };
    na::RequestOptions opts;
    opts.timeout = std::chrono::seconds(60);

    auto ex = co_await asio::this_coro::executor;
    co_return co_await na::async_post(
        ex,
        endpoint.host,
        endpoint.port,
        endpoint.prefix + "/v1/responses",
        std::move(body),
        std::move(headers),
        endpoint.tls,
        opts);
}

// Real web search via OpenAI's built-in `web_search` tool, hosted
// inside /v1/responses. The model decides when to invoke it, runs
// the search server-side, and folds the citations back into its
// final assistant message — we just read the resulting text out of
// output[].
//
// Bypasses SchemaProvider because hosted built-in tools don't follow
// the function-tool shape (no name/parameters), so they don't fit
// the schema's tool_definition wrapper.
static std::string web_search(const std::string& api_key,
                              const std::string& model,
                              const std::string& question) {
    namespace na = neograph::async;

    json body;
    body["model"] = model;
    body["input"] = question;
    body["tools"] = json::array({json{{"type", "web_search"}}});

    auto endpoint = na::split_async_endpoint("https://api.openai.com");
    auto resp = na::run_sync(post_web_search(
        endpoint, body.dump(), "Bearer " + api_key));

    if (resp.status != 200) {
        throw std::runtime_error(
            "web_search HTTP " + std::to_string(resp.status) +
            ": " + resp.body);
    }

    // Walk the output[] envelope. A web-search-augmented response
    // typically contains:
    //   - one or more {type: "web_search_call", ...} items (the
    //     tool invocations themselves — useful for debugging,
    //     ignored here),
    //   - one {type: "message", content: [{type: "output_text",
    //     text: "..."}]} item carrying the model's synthesised answer
    //     plus inline annotations / citations.
    auto j = json::parse(resp.body);
    std::string answer;
    if (j.contains("output") && j["output"].is_array()) {
        for (const auto& item : j["output"]) {
            if (item.value("type", "") != "message") continue;
            const auto& content = item["content"];
            if (!content.is_array()) continue;
            for (const auto& part : content) {
                if (part.value("type", "") == "output_text") {
                    answer += part.value("text", "");
                }
            }
        }
    }
    if (answer.empty()) {
        throw std::runtime_error(
            "web_search returned no output_text; raw body: " +
            resp.body.substr(0, 500));
    }
    return answer;
}

// Final answer composer.
static std::string generate(Provider& p, const std::string& question,
                            const std::string& context) {
    CompletionParams cp;
    cp.messages.push_back({"system",
        "Answer the question using ONLY the provided context. Be concise. "
        "If the context is insufficient, say so explicitly."});
    cp.messages.push_back({"user",
        "Question: " + question + "\n\nContext:\n" + context});
    cp.temperature = 0.2f;
    return p.complete(cp).message.content;
}

// =========================================================================
// Main
// =========================================================================
int main() {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        // SchemaProvider with the built-in "openai_responses" schema —
        // every LLM call below hits /v1/responses, exercising the
        // input[] request shape, output[] response parsing, and the
        // SSE-events streaming format declared in
        // schemas/openai_responses.json.
        const std::string model = "gpt-5.4-mini";

        llm::SchemaProvider::Config cfg;
        cfg.schema_path     = "openai_responses";
        cfg.api_key         = api_key;
        cfg.default_model   = model;
        cfg.timeout_seconds = 60;
        auto provider = llm::SchemaProvider::create(cfg);

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
                : evaluate(*provider, q, kb_ctx);
            const char* tag = v == Verdict::Correct   ? "CORRECT"
                            : v == Verdict::Ambiguous ? "AMBIGUOUS"
                                                      : "INCORRECT";
            std::cout << "[evaluate] " << tag << "\n";

            // 3. Route + assemble final context
            std::string final_ctx;
            switch (v) {
                case Verdict::Correct:
                    std::cout << "[route   ] refine(KB) → generate\n";
                    final_ctx = refine(*provider, q, kb_ctx);
                    break;
                case Verdict::Incorrect:
                    std::cout << "[route   ] web → generate (KB rejected)\n";
                    final_ctx = web_search(api_key, model, q);
                    break;
                case Verdict::Ambiguous:
                    std::cout << "[route   ] refine(KB) + web → generate\n";
                    final_ctx =
                        "## From the local knowledge base\n"
                        + refine(*provider, q, kb_ctx) +
                        "\n\n## From external web search\n"
                        + web_search(api_key, model, q);
                    break;
            }

            // 4. Generate
            auto answer = generate(*provider, q, final_ctx);
            std::cout << "\nA: " << answer << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
