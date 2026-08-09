// NeoGraph Cookbook — "The Beast", LIVE
// =================================================================
// The offline the_beast.cpp mocks the author. THIS one is live: a real
// LLM (DeepSeek v4 flash via OpenRouter) is handed the engine's exported
// schema and asked to WRITE a harness in the DSL surface. Whatever it
// produces is forced through the three coherence gates; on rejection the
// gate's diagnostics are fed straight back into the conversation and the
// model rewrites — a genuine self-repair loop. Only a harness that
// survives all three gates is compiled and run (with a checkpointer, so
// its execution can be rolled back).
//
// The load-bearing idea: the model may hallucinate freely, but it can
// NEVER get an incoherent harness past the compiler. Generation is
// creative; coherence is proven.
//
// Setup:  put OPENROUTER_API_KEY=sk-or-... in .env beside the binary
//         (or export it). Pins DeepSeek V4 Flash 0731 to Morph's advertised
//         US ZDR endpoint; an unavailable endpoint fails rather than falling back.
// Build:  cmake --build build --target cookbook_the_beast_live
// Run:    ./build/cookbook_the_beast_live

#include <neograph/async/run_sync.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/node.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/neograph.h>

#include "beast_common.h"
#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

using neograph::json;
namespace ng    = neograph::graph;
namespace beast = neograph::cookbook::beast;

// Deterministic worker node — appends its name to "trail" so a live
// harness runs for free (no per-node LLM calls) and its execution is
// observable/rollback-able. The LLM authors topology *around* these.
struct BeastNode : ng::GraphNode {
    std::string name_;
    explicit BeastNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        ng::NodeOutput out;
        out.writes.push_back({"trail", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

void register_beast_node() {
    static bool once = [] {
        ng::NodeFactory::instance().register_type(
            "beast_node",
            [](const std::string& name, const json&, const ng::NodeContext&) {
                return std::unique_ptr<ng::GraphNode>(new BeastNode(name));
            },
            // BeastNode ignores its input and only appends to trail, so the
            // contract declares reads:[] — matching the offline the_beast.cpp.
            // This deliberately lets E6 (written but never read) surface on
            // `trail`, the precise-lint behaviour the README documents.
            json::object(), json::parse(R"({"reads":[],"writes":["trail"]})"));
        return true;
    }();
    (void)once;
}

json channel_of(const json& serialized, const std::string& name) {
    if (serialized.contains("channels") && serialized["channels"].contains(name))
        return serialized["channels"][name].value("value", json::array());
    return json::array();
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!key || !*key) {
        std::cerr << "OPENROUTER_API_KEY not set (env or .env beside the binary)\n";
        return 2;
    }
    register_beast_node();

    const std::string model            = "deepseek/deepseek-v4-flash-0731";
    const json        provider_routing = {
        {"zdr", true},
        {"only", json::array({"morph"})},
        {"allow_fallbacks", false},
    };
    // `zdr` enforces retention policy; the `only` constraint pins this sample
    // to Morph. OpenRouter's public provider metadata advertised Morph's
    // datacenters as US on 2026-08-08. This is not a generic residency API.
    // Source: https://openrouter.ai/docs/guides/routing/provider-selection
    // Source: https://openrouter.ai/docs/guides/features/zdr
    auto provider =
        neograph::llm::OpenAIProvider::create_shared({.api_key       = key,
                                                      .base_url      = "https://openrouter.ai/api",
                                                      .default_model = model,
                                                      .timeout_seconds = 180});

    ng::NodeContext ctx;
    ctx.provider = provider;

    std::cout << "============ THE BEAST (live) ============\n"
                 "Author: "
              << model << " via OpenRouter (Morph US ZDR route).\n"
              << "The model writes the harness; the compiler proves it coherent.\n\n";

    // Hand the model the engine's real schema — the exact palette this
    // build accepts. It cannot drift because it IS the engine's schema.
    const auto schema     = ng::NodeFactory::instance().export_schema();
    json       node_types = json::array();
    for (auto it = schema["node_types"].begin(); it != schema["node_types"].end(); ++it)
        node_types.push_back(it.key());

    const std::string sys =
        "You are the architect of a NeoGraph agent harness. A harness is a "
        "graph TOPOLOGY described in JSON (the DSL surface). You output ONLY a "
        "single JSON object — no prose, no markdown fences.\n\n"
        "DSL surface rules:\n"
        "- Top level: \"schema_version\": 1, \"name\", \"channels\", "
        "\"templates\", \"use\", \"nodes\", \"edges\".\n"
        "- channels: { \"<name>\": { \"reducer\": \"append\"|\"overwrite\" } }.\n"
        "- templates: { \"<t>\": { \"params\": [..], \"nodes\": { \"<local>\": "
        "{ \"type\": \"<nodetype>\" } }, \"edges\": [..] } }. Inside a template, "
        "reference a param as \"@{param}\".\n"
        "- use: [ { \"template\": \"<t>\", \"prefix\": \"<p>\", \"args\": {..} } ]. "
        "Each template node <local> becomes \"<p>_<local>\".\n"
        "- edges: [ { \"from\": \"__start__\"|<node>, \"to\": <node>|\"__end__\" } ]. "
        "Every edge endpoint MUST be a node that exists after `use` expansion, "
        "or __start__/__end__.\n"
        "- Available node types (use ONLY these): " +
        node_types.dump() +
        ". "
        "Prefer \"beast_node\" for workers; it reads/writes the \"trail\" channel.\n\n"
        "Any dangling edge, undeclared channel, or unknown node type will be "
        "REJECTED by the compiler.";

    std::string task = (argc > 1)
                           ? argv[1]
                           : "Design a 3-stage pipeline: research -> critique -> summarize. Use a "
                             "single template instantiated three times via `use` (prefixes r/c/s), "
                             "each stage one beast_node worker, wired in a line from __start__ to "
                             "__end__. Declare a \"trail\" channel with the append reducer.";

    std::vector<neograph::ChatMessage> convo = {{"system", sys}, {"user", task}};

    // ---- Author → gate → (reject → repair)* loop, LIVE ----
    json accepted_core;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        std::cout << "── Attempt #" << attempt << ": asking the model to write a harness ──\n";
        neograph::CompletionParams p;
        p.model        = model;
        p.messages     = convo;
        p.temperature  = 0.2f;
        p.max_tokens   = 4000;  // reasoning model: leave room for thinking + JSON
        p.extra_fields = {{"provider", provider_routing}};

        neograph::ChatCompletion resp;
        // invoke(params, nullptr) is the single-dispatch v1.0 entry; run_sync
        // drives it to completion on a private io_context — the right sync
        // bridge for a straight-line CLI driver like this.
        try {
            resp = neograph::async::run_sync(provider->invoke(p, nullptr));
        } catch (const std::exception& e) {
            std::cerr << "  LLM error: " << e.what() << "\n";
            return 1;
        }
        const std::string reply = resp.message.content;
        std::cout << "  model returned " << reply.size() << " chars of JSON.\n";

        json dsl;
        try {
            dsl = beast::extract_json_object(reply);
        } catch (const std::exception& e) {
            std::cout << "  UNPARSEABLE (" << e.what() << "); asking again.\n\n";
            convo.push_back({"assistant", reply});
            convo.push_back(
                {"user", "That was not valid JSON. Output ONLY the JSON harness object."});
            continue;
        }

        const beast::HarnessVerdict v = beast::validate_harness(dsl, ctx);
        if (!v.ok) {
            std::cout << "  REJECTED at gate '" << v.gate << "':\n";
            std::cout << "    " << v.report.substr(0, 400) << "\n";
            std::cout << "  → feeding the compiler's diagnostics back to the model.\n\n";
            convo.push_back({"assistant", dsl.dump()});
            convo.push_back({"user", "The compiler REJECTED that harness at the '" + v.gate +
                                         "' gate:\n" + v.report +
                                         "\nFix ONLY what the diagnostics name. Output ONLY the "
                                         "corrected JSON harness."});
            continue;
        }

        std::cout << "  ACCEPTED — all three gates passed.\n";
        accepted_core = v.core;
        std::cout << "  Core lockfile nodes: ";
        for (auto it = accepted_core["nodes"].begin(); it != accepted_core["nodes"].end(); ++it)
            std::cout << it.key() << " ";
        std::cout << "\n\n";
        break;
    }

    if (accepted_core.is_null()) {
        std::cout << "The model could not produce a coherent harness in 3 tries.\n";
        return 1;
    }

    // ---- Spawn the model's harness with a checkpointer; run + roll back ----
    std::cout << "── Spawning the model's harness (checkpointed) ──\n";
    auto store  = std::make_shared<ng::InMemoryCheckpointStore>();
    auto engine = ng::GraphEngine::build(
        accepted_core, ng::EngineConfig{.node_context = ctx, .checkpoint_store = store});

    ng::RunConfig rc;
    rc.thread_id = "beast-live";
    rc.input     = {{"trail", json::array()}};
    rc.max_steps = 20;
    auto result  = engine->run(rc);

    json final_trail = result.has_channel("trail") ? result.channel<json>("trail") : json::array();
    std::cout << "  ran to completion, trail = " << final_trail.dump() << "\n";

    auto history = store->list("beast-live");
    std::cout << "  checkpoint timeline (" << history.size() << " snapshots):\n";
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        std::cout << "    step " << it->step
                  << "  trail=" << channel_of(it->channel_values, "trail").dump() << "\n";

    if (history.size() >= 2) {
        const auto& earlier  = history[history.size() - 2];
        auto        restored = store->load_by_id(earlier.id);
        if (restored)
            std::cout << "  >> ROLLBACK to step " << earlier.step << ": restored trail = "
                      << channel_of(restored->channel_values, "trail").dump() << "\n";
    }

    std::cout << "\nThe model wrote it. The compiler proved it. The Beast ran it.\n";
    return 0;
}
