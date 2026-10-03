// NeoGraph Example 24: Human Feedback Loop over MCP
//
// The agent produces a draft answer without using tools; the operator
// reads it, decides it's insufficient, and adds a follow-up turn telling
// the agent to actually call an MCP tool. The second run feeds the agent
// the complete conversation — prior draft + feedback — so the LLM sees
// the feedback in context and revises with a real tool call.
//
// Compared to example 20 (binary approve/reject before a tool fires),
// this shows feedback as new conversational content that the agent must
// *accept and incorporate*.
//
// Usage (after starting examples/demo_mcp_server.py):
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_mcp_feedback
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <neograph/mcp/client.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <cstdlib>

// Explicit display projection; messages remain the full trusted history.
static std::string last_assistant(const std::vector<sp::Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != sp::Role::Assistant) continue;
        std::string text;
        for (const auto& part : it->parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
        if (!text.empty()) return text;
    }
    return "(empty)";
}

static bool trace_used_tools(const std::vector<std::string>& trace) {
    for (const auto& n : trace) if (n == "tools") return true;
    return false;
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    try {
    const std::string mcp_url  = (argc >= 2) ? argv[1] : "http://localhost:8000";
    const std::string question = (argc >= 3) ? argv[2]
        : "What's the weather in Seoul right now?";

    const char* key_env = std::getenv("OPENROUTER_API_KEY");
    if (!key_env) {
        std::cerr << "OPENROUTER_API_KEY missing (set it or put it in .env)\n";
        return 1;
    }
    std::string api_key = key_env;

    neograph::mcp::MCPClient mcp_client(mcp_url);
    mcp_client.initialize("example-mcp-feedback");
    auto tools = mcp_client.get_tools();
    std::cout << "[*] " << tools.size()
              << " MCP tools available (agent may or may not call them)\n\n";

    std::shared_ptr<neograph::Provider> provider =
        examples::make_openrouter_provider(api_key);

    neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "feedback_loop"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}
        })}
    };

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model = examples::openrouter_model;
    ctx.instructions =
        "You are an assistant. Answer from your own knowledge first. "
        "Use the provided tools only if the user explicitly asks for "
        "real-time or external data.";

    auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    neograph::graph::EngineResources resources;
    resources.tools = neograph::ToolSet(std::move(tools));
    auto engine     = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx, .checkpoint_store = store},
        std::move(resources));

    // ---------- Round 1 ----------
    std::cout << "User: " << question << "\n\n";
    std::cout << "=== Round 1 — draft (agent tends to guess) ===\n";

    neograph::graph::RunConfig run1;
    run1.thread_id = "fb-001";
    run1.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};
    auto r1 = engine->run(run1);

    std::cout << "Assistant draft:\n  "
              << last_assistant(r1.native_messages) << "\n\n";

    // ---------- Human feedback ----------
    const std::string feedback =
        "That's not good enough. Please actually call the MCP weather tool "
        "and give me the real current reading.";
    std::cout << ">>> Operator feedback:\n    \"" << feedback << "\"\n\n";

    // ---------- Round 2 — feed full history + feedback to a new run ----------
    std::cout << "=== Round 2 — agent incorporates the feedback ===\n";

    neograph::graph::RunConfig run2;
    run2.thread_id = "fb-002";
    // Retain the engine-owned checkpoint context and native seals by value.
    // No JSON projection is interpreted as replay authority.
    run2.provider_messages = r1.native_messages;
    run2.provider_messages->push_back(examples::message(sp::Role::User, feedback));
    auto r2 = engine->run(run2);

    std::cout << "Assistant (revised):\n  "
              << last_assistant(r2.native_messages) << "\n\n";

    std::cout << "Round-2 trace: ";
    for (size_t i = 0; i < r2.execution_trace.size(); ++i) {
        std::cout << r2.execution_trace[i];
        if (i + 1 < r2.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    std::cout << "\nResult: round-1 used tools? "
              << (trace_used_tools(r1.execution_trace) ? "yes" : "no")
              << "    |    round-2 used tools? "
              << (trace_used_tools(r2.execution_trace)
                      ? "yes — feedback accepted"
                      : "no — feedback was ignored")
              << "\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
