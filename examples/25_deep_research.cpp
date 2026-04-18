// NeoGraph Example 25: Deep Research
//
// A C++ port of langchain-ai/open_deep_research.
// One supervisor Claude agent plans research, fans out parallel sub-researchers
// (each running its own ReAct loop with web tools), then synthesises a final
// markdown report.
//
// Tools are backed by a local Crawl4AI Docker container (no API keys):
//   docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
//       unclecode/crawl4ai:latest
//
// Usage:
//   ANTHROPIC_API_KEY=sk-ant-... ./example_deep_research "why did the Byzantine Empire fall"
//
// Environment:
//   ANTHROPIC_API_KEY         — required
//   CRAWL4AI_URL              — optional, defaults to http://localhost:11235
//   DR_MODEL                  — optional, defaults to claude-sonnet-4-5

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/graph/deep_research_graph.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace neograph;

// =========================================================================
// URL helpers
// =========================================================================

static std::pair<std::string, std::string> split_url(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {url, "/"};
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_start), url.substr(path_start)};
}

// URL-encode for query strings (small subset: letters/digits untouched, space→+,
// everything else percent-encoded).
static std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else if (c == ' ') {
            out << '+';
        } else {
            out << '%';
            if (c < 16) out << '0';
            out << (int)c;
            out << std::dec << std::hex; // sticky std::hex
        }
    }
    return out.str();
}

// =========================================================================
// Crawl4AIClient — thin wrapper around Crawl4AI's POST /md endpoint.
//
// /md accepts {url, f, q, c} and returns {markdown, success, ...}.
// We use the default `f=fit` extractor, no query-based BM25 filter.
// =========================================================================

class Crawl4AIClient {
public:
    explicit Crawl4AIClient(std::string base_url)
        : base_url_(std::move(base_url)) {}

    // Returns cleaned markdown of `url`. Throws on HTTP failure.
    std::string fetch_markdown(const std::string& url,
                               const std::string& query_hint = {}) const {
        json body = json::object();
        body["url"] = url;
        if (!query_hint.empty()) {
            body["f"] = "bm25";
            body["q"] = query_hint;
        }

        auto [host, prefix] = split_url(base_url_);
        httplib::Client cli(host);
        cli.set_read_timeout(60, 0);
        cli.set_connection_timeout(15, 0);

        auto res = cli.Post(prefix + "md", body.dump(), "application/json");

        if (!res) {
            throw std::runtime_error(
                "Crawl4AI connection failed: " + httplib::to_string(res.error()) +
                " (is the crawl4ai container running at " + base_url_ + "?)");
        }
        if (res->status != 200) {
            throw std::runtime_error(
                "Crawl4AI /md returned HTTP " + std::to_string(res->status) +
                ": " + res->body);
        }

        auto resp = json::parse(res->body);
        if (!resp.value("success", false)) {
            throw std::runtime_error("Crawl4AI reported failure for " + url);
        }
        return resp.value("markdown", std::string{});
    }

private:
    std::string base_url_;
};

// =========================================================================
// WebSearchTool — invokes Crawl4AI on duckduckgo.com/html to obtain a
// markdown rendering of DDG's result list, then trims for the LLM.
//
// Trade-off: this relies on DDG's HTML layout being reasonably stable.
// Good enough for a demo; a production port would plug Serper / Brave /
// Tavily behind the same Tool interface.
// =========================================================================

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(std::shared_ptr<Crawl4AIClient> client)
        : client_(std::move(client)) {}

    std::string get_name() const override { return "web_search"; }

    ChatTool get_definition() const override {
        return {
            "web_search",
            "Search the web for a query. Returns a markdown list of top "
            "results (URLs + titles + snippets). Call fetch_url on the "
            "most promising result for the full page body.",
            json{
                {"type", "object"},
                {"properties", {
                    {"query", {
                        {"type", "string"},
                        {"description", "Search query."}
                    }}
                }},
                {"required", json::array({"query"})}
            }
        };
    }

    std::string execute(const json& arguments) override {
        std::string query = arguments.value("query", std::string{});
        if (query.empty()) {
            return R"({"error":"empty query"})";
        }
        std::string ddg_url =
            "https://duckduckgo.com/html/?q=" + url_encode(query);

        try {
            std::string md = client_->fetch_markdown(ddg_url, query);
            // DDG's HTML is noisy; cap aggressively. Researcher context
            // bloat propagates to supervisor via compressed summaries; keep
            // raw tool output small so there's less for the model to quote.
            if (md.size() > 3000) md.resize(3000);
            return md;
        } catch (const std::exception& e) {
            json err = {{"error", std::string("web_search failed: ") + e.what()}};
            return err.dump();
        }
    }

private:
    std::shared_ptr<Crawl4AIClient> client_;
};

// =========================================================================
// FetchUrlTool — invokes Crawl4AI /md directly for a specific URL.
// =========================================================================

class FetchUrlTool : public Tool {
public:
    explicit FetchUrlTool(std::shared_ptr<Crawl4AIClient> client)
        : client_(std::move(client)) {}

    std::string get_name() const override { return "fetch_url"; }

    ChatTool get_definition() const override {
        return {
            "fetch_url",
            "Fetch a specific URL and return its cleaned markdown body. "
            "Use after web_search to read the full contents of a promising page.",
            json{
                {"type", "object"},
                {"properties", {
                    {"url", {
                        {"type", "string"},
                        {"description", "Absolute http/https URL."}
                    }}
                }},
                {"required", json::array({"url"})}
            }
        };
    }

    std::string execute(const json& arguments) override {
        std::string url = arguments.value("url", std::string{});
        if (url.empty()) return R"({"error":"empty url"})";

        try {
            std::string md = client_->fetch_markdown(url);
            if (md.size() > 5000) md.resize(5000);
            return md;
        } catch (const std::exception& e) {
            json err = {{"error", std::string("fetch_url failed: ") + e.what()}};
            return err.dump();
        }
    }

private:
    std::shared_ptr<Crawl4AIClient> client_;
};

// =========================================================================
// Entry point
// =========================================================================

int main(int argc, char** argv) {
    // Load .env from the current working directory (or the nearest ancestor
    // that has one). Existing environment variables win — a shell-exported
    // ANTHROPIC_API_KEY still takes precedence over the .env value.
    cppdotenv::load_dotenv(".env");

    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key) {
        std::cerr << "Set ANTHROPIC_API_KEY in the environment or in ./.env\n";
        return 1;
    }

    std::string query;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (i > 1) query += ' ';
            query += argv[i];
        }
    } else {
        query = "Summarize the current state of open-source C++ agent "
                "frameworks. Which ones are mature enough for production?";
    }

    const char* crawl4ai_url_env = std::getenv("CRAWL4AI_URL");
    std::string crawl4ai_url = crawl4ai_url_env ? crawl4ai_url_env
                                                : "http://localhost:11235";

    const char* model_env = std::getenv("DR_MODEL");
    std::string model = model_env ? model_env : "claude-sonnet-4-5";

    // Anthropic provider via built-in "claude" schema. Wrapped in the
    // RateLimitedProvider decorator because Deep Research fans out several
    // concurrent researcher calls and it's easy to trip Anthropic's
    // minute-window rate limits on low tiers. The decorator reads the
    // 429 response's Retry-After header and sleeps exactly that long
    // before retrying. Users with generous quotas (or non-Anthropic
    // backends that don't rate-limit) can drop the wrapper.
    auto raw_claude = llm::SchemaProvider::create({
        .schema_path    = "claude",
        .api_key        = api_key,
        .default_model  = model,
        .timeout_seconds = 120
    });
    std::shared_ptr<Provider> provider =
        llm::RateLimitedProvider::create(std::move(raw_claude));

    auto crawl = std::make_shared<Crawl4AIClient>(crawl4ai_url);

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<WebSearchTool>(crawl));
    tools.push_back(std::make_unique<FetchUrlTool>(crawl));

    graph::DeepResearchConfig cfg;
    cfg.model = model;
    cfg.max_supervisor_iterations  = 2;
    cfg.max_concurrent_researchers = 2;
    cfg.max_researcher_iterations  = 2;

    auto engine = graph::create_deep_research_graph(
        provider, std::move(tools), cfg);

    std::cout << "=== Deep Research (NeoGraph + Claude + Crawl4AI) ===\n";
    std::cout << "Query: " << query << "\n";
    std::cout << "Model: " << model << "\n";
    std::cout << "Crawl4AI: " << crawl4ai_url << "\n\n";

    graph::RunConfig rc;
    rc.thread_id  = "dr-1";
    rc.max_steps  = 40;
    // DEBUG enables __send__ / __routing__ meta-events so fan-out is visible.
    rc.stream_mode = graph::StreamMode::EVENTS | graph::StreamMode::DEBUG;
    rc.input = {{"user_query", query}};

    graph::RunResult result;
    try {
        result = engine->run_stream(rc,
        [](const graph::GraphEvent& event) {
            using T = graph::GraphEvent::Type;
            switch (event.type) {
                case T::NODE_START:
                    if (event.node_name == "__send__") {
                        auto sends = event.data["sends"];
                        std::cout << "  [send] fan-out to "
                                  << sends.size() << " researcher(s)\n";
                    } else if (event.node_name == "__routing__") {
                        if (event.data.contains("command_goto")) {
                            std::cout << "  [cmd]  → "
                                      << event.data["command_goto"] << "\n";
                        }
                    } else {
                        std::cout << "  [start] " << event.node_name << "\n";
                    }
                    break;
                case T::NODE_END:
                    std::cout << "  [done]  " << event.node_name << "\n";
                    break;
                case T::ERROR:
                    std::cerr << "  [err]  " << event.node_name << ": "
                              << event.data.dump() << "\n";
                    break;
                default: break;
            }
        });
    } catch (const std::exception& e) {
        std::cerr << "\n[fatal] run aborted: " << e.what() << "\n";
        std::cerr << "If this is Anthropic HTTP 429, your org's tokens-per-minute\n"
                     "budget is too tight for the default iteration counts. Try:\n"
                     "  * lowering max_supervisor_iterations / max_concurrent_researchers\n"
                     "  * shrinking Crawl4AI response caps further\n"
                     "  * upgrading your Anthropic rate tier\n";
        return 1;
    }

    std::cout << "\n--- Execution trace ---\n";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n\n";

    std::cout << "--- Final report ---\n";
    if (result.output.contains("channels") &&
        result.output["channels"].contains("final_report") &&
        result.output["channels"]["final_report"]["value"].is_string()) {
        std::cout << result.output["channels"]["final_report"]["value"]
                                  .get<std::string>() << "\n";
    } else {
        std::cout << "(no report produced — see trace above)\n";
    }

    return 0;
}
