// NeoGraph Example 20: MCP + Checkpoint HITL
//
// Combines MCP tool discovery with interrupt_before-based human approval.
// The graph pauses right before any MCP tool is invoked, a checkpoint is
// persisted, and the operator inspects the pending tool call. Resuming
// re-enters the same super-step and fires the tool; pending-writes
// machinery keeps earlier successful steps from re-executing.
//
// Usage (after starting examples/demo_mcp_server.py on port 8000):
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_mcp_hitl
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <neograph/mcp/client.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <cstdlib>

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    try {
    const std::string mcp_url  = (argc >= 2) ? argv[1] : "http://localhost:8000";
    const std::string question = (argc >= 3) ? argv[2]
        : "What's the weather in Tokyo right now?";

    const char* key_env = std::getenv("OPENROUTER_API_KEY");
    if (!key_env) {
        std::cerr << "OPENROUTER_API_KEY missing (set it or put it in .env)\n";
        return 1;
    }
    std::string api_key = key_env;

    // --- Discover MCP tools ---
    neograph::mcp::MCPClient mcp_client(mcp_url);
    mcp_client.initialize("example-mcp-hitl");
    auto tools = mcp_client.get_tools();
    std::cout << "[*] Discovered " << tools.size() << " MCP tools from " << mcp_url << "\n";

    // --- LLM ---
    std::shared_ptr<neograph::Provider> provider =
        examples::make_openrouter_provider(api_key);

    // --- Graph: ReAct with interrupt_before tools ---
    neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "mcp_hitl"},
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
        })},
        {"interrupt_before", neograph::json::array({"tools"})}
    };

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model = examples::openrouter_model;
    ctx.instructions =
        "You are a helpful assistant. Always call a tool when the user's "
        "question can be answered by one.";

    auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    neograph::graph::EngineResources resources;
    resources.tools = neograph::ToolSet(std::move(tools));
    auto engine     = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx, .checkpoint_store = store},
        std::move(resources));

    // --- Phase 1: run until tool approval gate ---
    std::cout << "\n=== Phase 1 — run until interrupt_before(\"tools\") ===\n";
    neograph::graph::RunConfig run;
    run.thread_id = "hitl-001";
    run.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};

    auto r1 = engine->run(run);

    if (!r1.interrupted) {
        std::cout << "(no tool call was needed; the LLM answered directly)\n";
        return 0;
    }

    std::cout << "Paused before node: " << r1.interrupt_node
              << "   checkpoint=" << r1.checkpoint_id.substr(0, 8) << "...\n";

    // Inspect a portable view of the pending client tool call, while the
    // checkpoint and r1 keep the full native assistant message intact.
    if (!r1.native_messages.empty()) {
        const auto calls = neograph::client_tool_calls(r1.native_messages.back());
        if (!calls.empty()) {
            const auto& call = calls.front();
            std::cout << "Pending tool: " << call.name << "\n"
                      << "Arguments:    " << call.arguments << "\n";
        }
    }

    std::cout << ">>> Simulating human review: APPROVED <<<\n";

    // --- Phase 2: resume past the gate ---
    // NOTE: no resume_value — the library would otherwise inject it as a
    // "user" message right between the assistant-with-tool_calls and the
    // upcoming tool response, which violates OpenAI's tool-call contract.
    // Approval here is implicit in the act of calling resume().
    std::cout << "\n=== Phase 2 — resume past the gate ===\n";
    auto r2 = engine->resume("hitl-001", neograph::json());

    if (!r2.native_messages.empty()) {
        const auto& last = r2.native_messages.back();
        std::cout << "Assistant: ";
        if (last.role == sp::Role::Assistant)
            for (const auto& part : last.parts)
                if (const auto* text = std::get_if<sp::Text>(&part))
                    std::cout << text->value;
        std::cout << "\n";
    }

    std::cout << "\nExecution trace: ";
    for (size_t i = 0; i < r2.execution_trace.size(); ++i) {
        std::cout << r2.execution_trace[i];
        if (i + 1 < r2.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
