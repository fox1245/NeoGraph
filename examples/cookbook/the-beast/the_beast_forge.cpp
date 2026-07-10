// NeoGraph Cookbook — "The Beast", FORGE
// =================================================================
// The apex Beast is handed a fixed tool catalog. THIS one goes further:
// when it needs a tool it doesn't have, it FORGES one — the architect
// LLM writes a Python MCP server implementing the missing tool, we
// materialize it to disk, launch it, and DISCOVER it back over the real
// MCP protocol. Then it authors a ReAct agent around the combined
// (discovered + forged) catalog, the compiler gates it, and the harness
// calls every tool autonomously.
//
//   DISCOVER (live MCP)  →  FORGE missing tools (LLM writes Python,
//   we launch + re-discover)  →  AUTHOR harness (3 gates + self-repair)
//   →  SPAWN with all tools bound  →  autonomous tool-calling.
//
// Real, not theatre: two live MCP stdio subprocesses (one stock, one the
// Beast wrote this run), a real tools/list round-trip on each, and a real
// ReAct loop. Only the authoring model is remote (deepseek/deepseek-v4-flash).
//
// Setup:  OPENROUTER_API_KEY in .env ; python3 on PATH.
// Build:  cmake --build build --target cookbook_the_beast_forge
// Run:    ./build/cookbook_the_beast_forge ["task"]

#include <neograph/neograph.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/client.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// ---- three coherence gates (unchanged across the whole cookbook) ----
struct Verdict { bool ok = false; std::string gate, report; json core; };
Verdict forge_gate(const json& dsl, const ng::NodeContext& ctx) {
    json core;
    try { core = ng::Elaborator::elaborate(dsl).core; }
    catch (const std::exception& e) { return {false, "elaborate", e.what(), {}}; }
    try {
        auto cg = ng::GraphCompiler::compile(core, ctx);
        ng::GraphCompiler::verify_roundtrip(core, cg);
        auto rep = ng::GraphValidator::validate(cg);
        if (rep.has_errors()) return {false, "validate", rep.summary(), {}};
        return {true, "accepted", {}, core};
    } catch (const std::exception& e) { return {false, "compile", e.what(), {}}; }
}

static std::string ask(std::shared_ptr<neograph::Provider> prov,
                       std::vector<neograph::ChatMessage>& convo, int max_tokens = 4000) {
    neograph::CompletionParams p;
    p.model = "deepseek/deepseek-v4-flash";
    p.messages = convo;
    p.temperature = 0.2f;
    p.max_tokens = max_tokens;
    return prov->complete(p).message.content;
}

static json extract_json(const std::string& t) {
    auto a = t.find('{'), b = t.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        throw std::runtime_error("no JSON object in reply");
    return json::parse(t.substr(a, b - a + 1));
}

// Pull a fenced code block (```...```) or fall back to the whole reply.
static std::string extract_code(const std::string& t) {
    auto s = t.find("```");
    if (s == std::string::npos) return t;
    s = t.find('\n', s);                    // skip past ```python
    auto e = t.find("```", s + 1);
    if (s == std::string::npos || e == std::string::npos) return t;
    return t.substr(s + 1, e - (s + 1));
}

static std::vector<std::string> tool_names(neograph::mcp::MCPClient& c) {
    std::vector<std::string> out;
    for (auto& t : c.get_tools()) out.push_back(t->get_definition().name);
    return out;
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!key || !*key) { std::cerr << "OPENROUTER_API_KEY not set\n"; return 2; }
    auto provider = neograph::llm::OpenAIProvider::create_shared(
        {.api_key = key, .base_url = "https://openrouter.ai/api",
         .default_model = "deepseek/deepseek-v4-flash"});

    // Task deliberately needs a capability the stock server lacks (string
    // reversal), forcing the Beast to FORGE it.
    const std::string task = (argc > 1) ? argv[1]
        : "Reverse the string 'monster', and tell me the current UTC time.";

    std::cout << "============ THE BEAST (forge) ============\n"
                 "Discovers tools over MCP; writes the ones it lacks; then\n"
                 "authors an agent that uses them all. Task:\n  " << task << "\n\n";

    // ---------- PHASE 1: DISCOVER (live MCP) ----------
    const std::string base_server = "examples/cookbook/minimal-mcp/min_stdio_server.py";
    neograph::mcp::MCPClient base_client({"python3", base_server});
    if (!base_client.initialize("beast-forge")) { std::cerr << "base MCP init failed\n"; return 1; }
    auto base_names = tool_names(base_client);
    std::cout << "── DISCOVER · stock MCP server ──\n  tools: ";
    for (auto& n : base_names) std::cout << n << " ";
    std::cout << "\n\n";

    // ---------- PHASE 2+3: identify gap & FORGE the missing tool ----------
    std::cout << "── FORGE · the model writes a Python MCP server for what's missing ──\n";
    std::vector<neograph::ChatMessage> fconvo = {
        {"system",
         "You write a Model Context Protocol (MCP) stdio server in pure Python "
         "stdlib (no pip). It must speak newline-delimited JSON-RPC on stdin/stdout "
         "and implement exactly: initialize (reply protocolVersion/serverInfo/"
         "capabilities), notifications/initialized (no reply), tools/list (reply "
         "{\"tools\":[{name,description,inputSchema}...]}), tools/call (reply "
         "{\"content\":[{\"type\":\"text\",\"text\":<result>}],\"isError\":false}). "
         "Read stdin line by line; flush after every reply. Output ONLY the Python "
         "code in a single ```python fenced block."},
        {"user",
         "Task the downstream agent must solve: " + task + "\n"
         "Tools ALREADY available (do NOT reimplement these): " + json(base_names).dump() + "\n"
         "Write a server exposing ONLY the additional tool(s) needed for the task."}};

    namespace fs = std::filesystem;
    const std::string forged_path = (fs::temp_directory_path() / "beast_forged_server.py").string();
    std::vector<std::string> forged_names;
    std::unique_ptr<neograph::mcp::MCPClient> forged_client;

    for (int attempt = 1; attempt <= 2 && forged_names.empty(); ++attempt) {
        const std::string code = extract_code(ask(provider, fconvo, 3000));
        std::ofstream(forged_path) << code;
        std::cout << "  attempt #" << attempt << ": wrote " << code.size()
                  << " bytes → " << forged_path << "\n";
        try {
            forged_client = std::make_unique<neograph::mcp::MCPClient>(
                std::vector<std::string>{"python3", forged_path});
            if (forged_client->initialize("beast-forge")) {
                forged_names = tool_names(*forged_client);
            }
        } catch (const std::exception& e) { std::cout << "  launch error: " << e.what() << "\n"; }
        if (forged_names.empty()) {
            std::cout << "  forged server did not expose tools; asking model to fix.\n";
            fconvo.push_back({"assistant", code});
            fconvo.push_back({"user", "That server failed to initialize or list tools over MCP. "
                                      "Output ONLY corrected Python."});
            forged_client.reset();
        }
    }
    if (forged_names.empty()) { std::cout << "  could not forge a working tool server.\n"; return 1; }
    std::cout << "  FORGED + re-discovered over MCP: ";
    for (auto& n : forged_names) std::cout << n << " ";
    std::cout << "\n\n";

    // ---------- Bind every discovered tool (stock + forged) ----------
    std::vector<std::unique_ptr<neograph::Tool>> owned;
    for (auto& t : base_client.get_tools())    owned.push_back(std::move(t));
    for (auto& t : forged_client->get_tools()) owned.push_back(std::move(t));
    json catalog = json::array();
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : owned) {
        auto d = t->get_definition();
        catalog.push_back({{"name", d.name}, {"description", d.description}, {"parameters", d.parameters}});
        tool_ptrs.push_back(t.get());
    }
    ng::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;

    // ---------- PHASE 4: AUTHOR the ReAct harness (3 gates + self-repair) ----------
    std::cout << "── AUTHOR · the model writes a ReAct agent over the full catalog ──\n";
    std::vector<neograph::ChatMessage> hconvo = {
        {"system",
         "You author a NeoGraph agent harness — a graph TOPOLOGY in JSON. Output ONLY "
         "one JSON object. Build a ReAct tool-calling agent: schema_version 1; channels "
         "{\"messages\":{\"reducer\":\"append\"}}; a node \"agent\" of type \"llm_call\" and "
         "a node \"tools\" of type \"tool_dispatch\"; edges __start__->agent, a conditional "
         "edge {\"from\":\"agent\",\"condition\":\"has_tool_calls\",\"routes\":{\"true\":"
         "\"tools\",\"false\":\"__end__\"}}, and tools->agent. Tools available at runtime "
         "(handled by tool_dispatch, do not wire individually): " + catalog.dump()},
        {"user", "Author the harness JSON."}};

    json core;
    for (int attempt = 1; attempt <= 3 && core.is_null(); ++attempt) {
        json dsl;
        try { dsl = extract_json(ask(provider, hconvo)); }
        catch (const std::exception& e) {
            hconvo.push_back({"user", "Not valid JSON. Output ONLY the JSON harness."});
            std::cout << "  #" << attempt << " unparseable; retry.\n"; continue;
        }
        const Verdict v = forge_gate(dsl, ctx);
        if (!v.ok) {
            std::cout << "  #" << attempt << " REJECTED at '" << v.gate << "': "
                      << v.report.substr(0, 200) << " → self-repair.\n";
            hconvo.push_back({"assistant", dsl.dump()});
            hconvo.push_back({"user", "Compiler REJECTED at '" + v.gate + "':\n" + v.report +
                                      "\nFix only what it names. Output ONLY corrected JSON."});
            continue;
        }
        std::cout << "  ACCEPTED — coherent agent: ";
        for (auto it = v.core["nodes"].begin(); it != v.core["nodes"].end(); ++it)
            std::cout << it.key() << "(" << (*it).value("type", "?") << ") ";
        std::cout << "\n\n";
        core = v.core;
    }
    if (core.is_null()) { std::cout << "no coherent harness.\n"; return 1; }

    // ---------- PHASE 5: SPAWN with all tools bound ----------
    std::cout << "── SPAWN · run the agent it wrote, tools bound ──\n";
    auto engine = ng::GraphEngine::compile(core, ctx);
    engine->own_tools(std::move(owned));
    ng::RunConfig rc;
    rc.max_steps = 12;
    rc.input = {{"messages", json::array({{{"role", "user"}, {"content", task}}})}};
    auto result = engine->run_stream(rc, [](const ng::GraphEvent& ev) {
        if (ev.type == ng::GraphEvent::Type::NODE_START && ev.node_name == "tools")
            std::cout << "  [harness dispatching tools autonomously]\n";
    });

    json messages = result.has_channel("messages") ? result.channel<json>("messages") : json::array();
    std::string final_answer; int calls = 0;
    for (const auto& m : messages) {
        if (m.value("role", "") == "tool") { std::cout << "    tool → " << m.value("content", "") << "\n"; ++calls; }
        if (m.value("role", "") == "assistant" && !m.value("content", "").empty())
            final_answer = m.value("content", "");
    }
    std::cout << "\n  autonomous tool calls: " << calls << "\n  final answer: " << final_answer << "\n";
    std::cout << "\nIt discovered tools, forged the missing one, and used them all.\n";
    return 0;   // MCPClients + engine tear down here (subprocesses reaped)
}
