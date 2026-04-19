// NeoGraph Example 26: Postgres-backed Deep Research with HITL
//
// Demonstrates two NeoGraph features end-to-end:
//
//   1. PostgresCheckpointStore — durable checkpoints in real PostgreSQL.
//      The CLI is split into `run` and `resume` subcommands so the
//      whole agent process exits between phases. Resume re-loads the
//      thread's state from PG, proving the checkpoint actually survived
//      across processes.
//
//   2. NodeInterrupt-driven HITL — the Deep Research graph is built
//      with `enable_human_review = true`, which inserts a HumanReviewNode
//      between `final_report` and `__end__`. After the agent produces
//      its first report it pauses; the user reads the report (e.g.
//      notices "no URL citations!") and resumes with feedback. The
//      supervisor then dispatches more research to address the feedback
//      and produces a new report.
//
// Tooling reused from example 25 (Crawl4AI-backed web_search +
// fetch_url) so the agent can actually reach the web.
//
// Usage:
//   ./example_postgres_react_hitl run    "research question"
//   ./example_postgres_react_hitl resume <thread_id> "approve|feedback"
//   ./example_postgres_react_hitl status <thread_id>
//
// Required environment (read from process env or ./.env via cppdotenv):
//   ANTHROPIC_API_KEY  Claude API key
//   POSTGRES_URL       libpq connection string
//                      e.g. postgresql://postgres:test@localhost:55432/neograph
//
// Optional environment:
//   CRAWL4AI_URL       defaults to http://localhost:11235
//   DR_MODEL           defaults to claude-sonnet-4-5

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/graph/deep_research_graph.h>
#include <neograph/graph/postgres_checkpoint.h>

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
// URL / HTTP helpers (reused from example 25 — small, self-contained,
// not worth promoting to a library yet).
// =========================================================================

static std::pair<std::string, std::string> split_url(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {url, "/"};
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {url, "/"};
    return {url.substr(0, path_start), url.substr(path_start)};
}

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
            out << std::dec << std::hex;
        }
    }
    return out.str();
}

class Crawl4AIClient {
public:
    explicit Crawl4AIClient(std::string base_url) : base_url_(std::move(base_url)) {}
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
        if (!res) throw std::runtime_error(
            "Crawl4AI connection failed: " + httplib::to_string(res.error()) +
            " (is the crawl4ai container reachable at " + base_url_ + "?)");
        if (res->status != 200) throw std::runtime_error(
            "Crawl4AI /md returned HTTP " + std::to_string(res->status) +
            ": " + res->body);
        auto resp = json::parse(res->body);
        if (!resp.value("success", false))
            throw std::runtime_error("Crawl4AI reported failure for " + url);
        return resp.value("markdown", std::string{});
    }
private:
    std::string base_url_;
};

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(std::shared_ptr<Crawl4AIClient> client)
        : client_(std::move(client)) {}
    std::string get_name() const override { return "web_search"; }
    ChatTool get_definition() const override {
        return {
            "web_search",
            "Search the web. Returns a markdown list of results "
            "(URLs + titles + snippets).",
            json{{"type", "object"},
                 {"properties", {{"query",
                    {{"type", "string"},
                     {"description", "Search query."}}}}},
                 {"required", json::array({"query"})}}
        };
    }
    std::string execute(const json& arguments) override {
        std::string query = arguments.value("query", std::string{});
        if (query.empty()) return R"({"error":"empty query"})";
        std::string ddg_url = "https://duckduckgo.com/html/?q=" + url_encode(query);
        try {
            std::string md = client_->fetch_markdown(ddg_url, query);
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

class FetchUrlTool : public Tool {
public:
    explicit FetchUrlTool(std::shared_ptr<Crawl4AIClient> client)
        : client_(std::move(client)) {}
    std::string get_name() const override { return "fetch_url"; }
    ChatTool get_definition() const override {
        return {
            "fetch_url",
            "Fetch a specific URL and return its cleaned markdown body.",
            json{{"type", "object"},
                 {"properties", {{"url",
                    {{"type", "string"},
                     {"description", "Absolute http/https URL."}}}}},
                 {"required", json::array({"url"})}}
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
// Engine factory — same wiring for run / resume so the resume call
// re-uses the exact same graph layout the cp was saved against.
// =========================================================================

struct AppCtx {
    std::shared_ptr<graph::PostgresCheckpointStore> store;
    std::unique_ptr<graph::GraphEngine>             engine;
    std::string model;
};

static AppCtx build_app(bool with_human_review) {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key) throw std::runtime_error(
        "ANTHROPIC_API_KEY missing (process env or ./.env)");
    const char* pg_url = std::getenv("POSTGRES_URL");
    if (!pg_url) throw std::runtime_error(
        "POSTGRES_URL missing (process env or ./.env). "
        "Example: postgresql://postgres:test@localhost:55432/neograph");

    const char* crawl_env = std::getenv("CRAWL4AI_URL");
    std::string crawl_url = crawl_env ? crawl_env : "http://localhost:11235";

    const char* model_env = std::getenv("DR_MODEL");
    std::string model = model_env ? model_env : "claude-sonnet-4-5";

    auto raw_claude = llm::SchemaProvider::create({
        .schema_path     = "claude",
        .api_key         = api_key,
        .default_model   = model,
        .timeout_seconds = 120
    });
    std::shared_ptr<Provider> provider =
        llm::RateLimitedProvider::create(std::move(raw_claude));

    auto crawl = std::make_shared<Crawl4AIClient>(crawl_url);
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<WebSearchTool>(crawl));
    tools.push_back(std::make_unique<FetchUrlTool>(crawl));

    graph::DeepResearchConfig cfg;
    cfg.model = model;
    cfg.max_supervisor_iterations  = 2;
    cfg.max_concurrent_researchers = 2;
    cfg.max_researcher_iterations  = 2;
    cfg.enable_human_review = with_human_review;

    AppCtx app;
    app.model = model;
    app.engine = graph::create_deep_research_graph(
        provider, std::move(tools), cfg);

    app.store = std::make_shared<graph::PostgresCheckpointStore>(pg_url);
    app.engine->set_checkpoint_store(app.store);

    return app;
}

// =========================================================================
// Stream callback — mirrors example 25's logging.
// =========================================================================

static graph::GraphStreamCallback make_logger() {
    return [](const graph::GraphEvent& event) {
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
    };
}

// =========================================================================
// Sub-commands
// =========================================================================

static int print_usage() {
    std::cerr <<
        "Usage:\n"
        "  example_postgres_react_hitl run    \"<query>\"\n"
        "  example_postgres_react_hitl resume <thread_id> \"approve|feedback\"\n"
        "  example_postgres_react_hitl status <thread_id>\n"
        "\n"
        "Required env: ANTHROPIC_API_KEY, POSTGRES_URL.\n"
        "Optional env: CRAWL4AI_URL (default http://localhost:11235),\n"
        "              DR_MODEL (default claude-sonnet-4-5).\n";
    return 2;
}

// Read latest cp's final_report channel value (if any) so the user can
// see what the agent produced before they decide whether to approve.
static std::string load_latest_report(graph::PostgresCheckpointStore& store,
                                       const std::string& thread_id) {
    auto cp = store.load_latest(thread_id);
    if (!cp) return "(no checkpoint found)";
    if (!cp->channel_values.is_object() ||
        !cp->channel_values.contains("channels")) return "(empty cp)";
    auto chs = cp->channel_values["channels"];
    if (!chs.contains("final_report")) return "(no report channel)";
    auto fr = chs["final_report"];
    if (!fr.is_object() || !fr.contains("value")) return "(no report value)";
    auto v = fr["value"];
    return v.is_string() ? v.get<std::string>() : "(non-string report)";
}

static int cmd_run(const std::string& query) {
    auto app = build_app(/*with_human_review=*/true);

    // Use a fresh thread id every `run` so users can fan multiple
    // demos through the same DB without cross-talk. Print it so the
    // caller can pass it to `resume`.
    std::string thread_id = "dr-hitl-" + graph::Checkpoint::generate_id().substr(0, 8);

    std::cout << "=== Postgres HITL Deep Research ===\n"
              << "Thread:  " << thread_id << "\n"
              << "Model:   " << app.model << "\n"
              << "Query:   " << query << "\n\n";

    graph::RunConfig rc;
    rc.thread_id   = thread_id;
    rc.max_steps   = 40;
    rc.stream_mode = graph::StreamMode::EVENTS | graph::StreamMode::DEBUG;
    rc.input       = {{"user_query", query}};

    graph::RunResult result;
    try {
        result = app.engine->run_stream(rc, make_logger());
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 1;
    }

    // The engine catches NodeInterrupt internally and returns a normal
    // RunResult with `interrupted = true`; it does NOT re-throw. So the
    // HITL branch is a flag check, not an exception catch.
    if (result.interrupted) {
        std::cout << "\n--- HUMAN REVIEW REQUESTED ---\n"
                  << "Interrupt reason:\n" << result.interrupt_node << "\n\n"
                  << "To approve: ./example_postgres_react_hitl resume "
                  << thread_id << " approve\n"
                  << "To send feedback: ./example_postgres_react_hitl resume "
                  << thread_id << " \"give me URL citations\"\n";
        return 0;
    }

    // Reached __end__ without interrupting — usually means HITL was
    // disabled or the supervisor terminated before final_report fired.
    std::cout << "\n--- Final report (run completed without interrupt) ---\n"
              << load_latest_report(*app.store, thread_id) << "\n";
    return 0;
}

static int cmd_resume(const std::string& thread_id,
                      const std::string& feedback) {
    auto app = build_app(/*with_human_review=*/true);

    std::cout << "=== Resuming thread " << thread_id << " ===\n"
              << "Feedback: " << (feedback.empty() ? "(empty → approve)" : feedback)
              << "\n\n";

    graph::RunResult result;
    try {
        result = app.engine->resume(thread_id, json(feedback), make_logger());
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 1;
    }

    if (result.interrupted) {
        // Supervisor produced a NEW report; human_review fires again.
        std::cout << "\n--- HUMAN REVIEW REQUESTED (round 2+) ---\n"
                  << "Interrupt reason:\n" << result.interrupt_node << "\n\n"
                  << "To approve: ./example_postgres_react_hitl resume "
                  << thread_id << " approve\n"
                  << "To send more feedback: ./example_postgres_react_hitl resume "
                  << thread_id << " \"...\"\n";
        return 0;
    }

    std::cout << "\n--- Final report (approved) ---\n"
              << load_latest_report(*app.store, thread_id) << "\n";
    return 0;
}

static int cmd_status(const std::string& thread_id) {
    auto app = build_app(/*with_human_review=*/true);
    auto cp = app.store->load_latest(thread_id);
    if (!cp) {
        std::cout << "No checkpoint for thread " << thread_id << "\n";
        return 1;
    }
    std::cout << "Thread:        " << cp->thread_id << "\n"
              << "Checkpoint:    " << cp->id << "\n"
              << "Phase:         " << graph::to_string(cp->interrupt_phase) << "\n"
              << "Step:          " << cp->step << "\n"
              << "Next nodes:    ";
    for (size_t i = 0; i < cp->next_nodes.size(); ++i) {
        std::cout << cp->next_nodes[i];
        if (i + 1 < cp->next_nodes.size()) std::cout << ", ";
    }
    std::cout << "\nBlob count:    " << app.store->blob_count() << "\n";
    return 0;
}

// =========================================================================

int main(int argc, char** argv) {
    cppdotenv::load_dotenv(".env");

    if (argc < 2) return print_usage();
    std::string sub = argv[1];

    try {
        if (sub == "run") {
            if (argc < 3) return print_usage();
            std::string query;
            for (int i = 2; i < argc; ++i) {
                if (i > 2) query += ' ';
                query += argv[i];
            }
            return cmd_run(query);
        }
        if (sub == "resume") {
            if (argc < 3) return print_usage();
            std::string thread_id = argv[2];
            std::string feedback;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) feedback += ' ';
                feedback += argv[i];
            }
            return cmd_resume(thread_id, feedback);
        }
        if (sub == "status") {
            if (argc < 3) return print_usage();
            return cmd_status(argv[2]);
        }
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 1;
    }
    return print_usage();
}
