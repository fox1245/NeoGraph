// NeoGraph Cookbook — "The Beast", APEX
// =================================================================
// the_beast.cpp proves the generated harness is *coherent*. But its
// nodes are inert stubs — it never acts. THIS is the monster: a live
// LLM (DeepSeek v4 flash) is handed the engine's node schema AND a TOOL
// CATALOG, and asked to author a ReAct agent harness — `llm_call` ⇄
// `tool_dispatch` looping on `has_tool_calls`. The harness it writes is
// gated for coherence (3 gates + self-repair), then SPAWNED WITH THE
// TOOLS BOUND. The spawned harness then decides, on its own, which tools
// to call and when — real autonomous tool-calling by a machine-authored,
// compiler-proven agent.
//
// The Beast devours the tool catalog and wires whatever it wants into
// the agent it builds. Generation is creative; tool-use is autonomous;
// coherence is proven.
//
// Setup:  OPENROUTER_API_KEY=sk-or-... in .env  (model deepseek/deepseek-v4-flash)
// Build:  cmake --build build --target cookbook_the_beast_apex
// Run:    ./build/cookbook_the_beast_apex ["your task"]

#include <neograph/neograph.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/loader.h>
#include <neograph/llm/openai_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using neograph::json;
namespace ng = neograph::graph;

// =================================================================
// The tool catalog the Beast gets to devour. Deterministic so the run
// is verifiable, but the LLM genuinely decides whether/when to call each.
// =================================================================
class CalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"calculator", "Compute a binary arithmetic operation.",
            json::parse(R"({"type":"object","properties":{
              "a":{"type":"number"},"b":{"type":"number"},
              "op":{"type":"string","enum":["+","-","*","/"]}},
              "required":["a","b","op"]})")};
    }
    std::string execute(const json& args) override {
        double a = args.value("a", 0.0), b = args.value("b", 0.0);
        std::string op = args.value("op", "+");
        double r = op == "+" ? a + b : op == "-" ? a - b
                 : op == "*" ? a * b : op == "/" && b != 0 ? a / b : 0.0;
        return json{{"result", r}}.dump();
    }
    std::string get_name() const override { return "calculator"; }
};

class WeatherTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"get_weather", "Current weather for a city.",
            json::parse(R"({"type":"object","properties":{
              "city":{"type":"string"}},"required":["city"]})")};
    }
    std::string execute(const json& args) override {
        static const std::map<std::string, std::string> db = {
            {"Seoul", "19C, clear"}, {"Tokyo", "22C, rain"}, {"Paris", "14C, cloudy"}};
        auto it = db.find(args.value("city", ""));
        return json{{"weather", it == db.end() ? "no data" : it->second}}.dump();
    }
    std::string get_name() const override { return "get_weather"; }
};

std::vector<std::unique_ptr<neograph::Tool>> make_tools() {
    std::vector<std::unique_ptr<neograph::Tool>> t;
    t.push_back(std::make_unique<CalculatorTool>());
    t.push_back(std::make_unique<WeatherTool>());
    return t;
}

// The three coherence gates.
struct Verdict { bool ok = false; std::string gate, report; json core; };

Verdict forge(const json& dsl, const ng::NodeContext& ctx) {
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

json extract_json(const std::string& text) {
    auto a = text.find('{'); auto b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        throw std::runtime_error("no JSON object in reply");
    return json::parse(text.substr(a, b - a + 1));
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    const char* key = std::getenv("OPENROUTER_API_KEY");
    if (!key || !*key) { std::cerr << "OPENROUTER_API_KEY not set\n"; return 2; }

    auto provider = neograph::llm::OpenAIProvider::create_shared(
        {.api_key = key, .base_url = "https://openrouter.ai/api",
         .default_model = "deepseek/deepseek-v4-flash"});

    // Build the tool catalog to feed the architect, and a live copy to
    // bind into the spawned harness.
    auto tools = make_tools();
    json catalog = json::array();
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) {
        auto def = t->get_definition();
        catalog.push_back({{"name", def.name}, {"description", def.description},
                           {"parameters", def.parameters}});
        tool_ptrs.push_back(t.get());
    }

    ng::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;   // ← the harness's llm_call/tool_dispatch see these

    std::cout << "============ THE BEAST (apex) ============\n"
                 "The model devours a tool catalog and writes a tool-calling\n"
                 "agent. The compiler proves it coherent. Then it runs itself.\n\n";
    std::cout << "Tool catalog offered: ";
    for (const auto& c : catalog) std::cout << c["name"].get<std::string>() << " ";
    std::cout << "\n\n";

    const std::string sys =
        "You are the architect of a NeoGraph agent harness — a graph TOPOLOGY "
        "in JSON. Output ONLY one JSON object, no prose, no fences.\n\n"
        "Build a ReAct tool-calling agent:\n"
        "- channels: { \"messages\": { \"reducer\": \"append\" } }.\n"
        "- nodes: an \"agent\" node of type \"llm_call\", and a \"tools\" node "
        "of type \"tool_dispatch\". (llm_call automatically sees the bound tool "
        "catalog and may emit tool calls; tool_dispatch executes them.)\n"
        "- edges: __start__ -> agent; then a CONDITIONAL edge from agent using "
        "condition \"has_tool_calls\" with routes EXACTLY "
        "{ \"true\": \"tools\", \"false\": \"__end__\" }; and tools -> agent "
        "(the loop). Conditional edge form: "
        "{ \"from\": \"agent\", \"condition\": \"has_tool_calls\", "
        "\"routes\": { \"true\": \"tools\", \"false\": \"__end__\" } }.\n"
        "- schema_version: 1.\n\n"
        "Tools available to the agent at runtime (for your awareness; you do not "
        "wire them individually — tool_dispatch handles all): " + catalog.dump() +
        "\n\nAny dangling node, missing route branch, or undeclared channel is "
        "REJECTED by the compiler.";

    std::vector<neograph::ChatMessage> convo = {
        {"system", sys},
        {"user", "Author the ReAct agent harness JSON."}};

    json core;
    for (int attempt = 1; attempt <= 3 && core.is_null(); ++attempt) {
        std::cout << "── Attempt #" << attempt << ": model authors a tool-calling agent ──\n";
        neograph::CompletionParams p;
        p.model = "deepseek/deepseek-v4-flash";
        p.messages = convo;
        p.temperature = 0.2f;
        p.max_tokens = 4000;
        neograph::ChatCompletion resp;
        try { resp = provider->complete(p); }
        catch (const std::exception& e) { std::cerr << "  LLM error: " << e.what() << "\n"; return 1; }

        json dsl;
        try { dsl = extract_json(resp.message.content); }
        catch (const std::exception& e) {
            convo.push_back({"assistant", resp.message.content});
            convo.push_back({"user", "Not valid JSON. Output ONLY the JSON harness."});
            std::cout << "  unparseable; retry.\n\n"; continue;
        }
        const Verdict v = forge(dsl, ctx);
        if (!v.ok) {
            std::cout << "  REJECTED at '" << v.gate << "': " << v.report.substr(0, 300) << "\n";
            std::cout << "  → feeding diagnostics back for self-repair.\n\n";
            convo.push_back({"assistant", dsl.dump()});
            convo.push_back({"user", "The compiler REJECTED that at the '" + v.gate +
                "' gate:\n" + v.report + "\nFix only what it names. Output ONLY corrected JSON."});
            continue;
        }
        std::cout << "  ACCEPTED — coherent tool-calling agent. Nodes: ";
        for (auto it = v.core["nodes"].begin(); it != v.core["nodes"].end(); ++it)
            std::cout << it.key() << "(" << (*it).value("type", "?") << ") ";
        std::cout << "\n\n";
        core = v.core;
    }
    if (core.is_null()) { std::cout << "No coherent harness in 3 tries.\n"; return 1; }

    // ---- Spawn the machine-authored agent WITH THE TOOLS BOUND ----
    const std::string task = (argc > 1) ? argv[1]
        : "What is 23 multiplied by 19, and what's the weather in Seoul?";
    std::cout << "── Spawning the agent it wrote — live, tools bound ──\n";
    std::cout << "  user task: " << task << "\n";

    auto engine = ng::GraphEngine::compile(core, ctx);
    engine->own_tools(std::move(tools));   // engine owns the tool lifetimes

    ng::RunConfig rc;
    rc.max_steps = 12;                     // bound the ReAct loop
    rc.input = {{"messages", json::array({{{"role", "user"}, {"content", task}}})}};

    int tool_calls = 0;
    auto result = engine->run_stream(rc, [&](const ng::GraphEvent& ev) {
        if (ev.type == ng::GraphEvent::Type::NODE_START && ev.node_name == "tools")
            std::cout << "  [the harness is calling tools autonomously]\n";
    });

    // Surface what the agent actually did: every tool result + final answer.
    json messages = result.has_channel("messages") ? result.channel<json>("messages") : json::array();
    std::string final_answer;
    for (const auto& m : messages) {
        const std::string role = m.value("role", "");
        if (role == "tool") { std::cout << "    tool → " << m.value("content", "") << "\n"; ++tool_calls; }
        if (role == "assistant" && !m.value("content", "").empty())
            final_answer = m.value("content", "");
    }

    std::cout << "\n  tool calls executed by the harness: " << tool_calls << "\n";
    std::cout << "  final answer: " << final_answer << "\n";
    std::cout << "\nThe model wrote the agent. The compiler proved it. "
                 "The agent ate the tools.\n";
    return 0;
}
