// NeoGraph Example 20: MCP + Checkpoint HITL
//
// Combines MCP tool discovery with interrupt_before-based human approval.
// The graph pauses right before any MCP tool is invoked, a checkpoint is
// persisted, and the operator inspects the pending tool call. Resuming
// re-enters the same super-step and fires the tool; pending-writes
// machinery keeps earlier successful steps from re-executing.
//
// Usage (after starting examples/demo_mcp_server.py on port 8000):
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_mcp_hitl
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
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

    const char* key_env = std::getenv("OPENAI_API_KEY");
    if (!key_env) {
        std::cerr << "OPENAI_API_KEY missing (set it or put it in .env)\n";
        return 1;
    }
    std::string api_key = key_env;

    // --- Discover MCP tools ---
    neograph::mcp::MCPClient mcp_client(mcp_url);
    auto tools = mcp_client.get_tools();
    std::cout << "[*] Discovered " << tools.size() << " MCP tools from " << mcp_url << "\n";

    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    // --- LLM ---
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = api_key;
    cfg.default_model = "gpt-4o-mini";
    std::shared_ptr<neograph::Provider> provider =
        neograph::llm::OpenAIProvider::create(cfg);

    // --- Graph: ReAct with interrupt_before tools ---
    neograph::json definition = {
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
    ctx.tools = tool_ptrs;
    ctx.instructions =
        "You are a helpful assistant. Always call a tool when the user's "
        "question can be answered by one.";

    auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);
    engine->own_tools(std::move(tools));

    // --- Phase 1: run until tool approval gate ---
    std::cout << "\n=== Phase 1 — run until interrupt_before(\"tools\") ===\n";
    neograph::graph::RunConfig run;
    run.thread_id = "hitl-001";
    run.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};

    auto r1 = engine->run(run);

    if (!r1.interrupted) {
        std::cout << "(no tool call was needed; the LLM answered directly)\n";
        return 0;
    }

    std::cout << "Paused before node: " << r1.interrupt_node
              << "   checkpoint=" << r1.checkpoint_id.substr(0, 8) << "...\n";

    // Inspect the pending tool call the LLM asked for.
    // State JSON nests channels: { channels: { messages: { value: [...] } } }
    auto channel_value = [](const std::optional<neograph::json>& st,
                            const std::string& ch) -> neograph::json {
        if (!st.has_value()) return neograph::json::array();
        auto channels = st->value("channels", neograph::json::object());
        auto entry    = channels.value(ch, neograph::json::object());
        return entry.value("value", neograph::json::array());
    };

    auto state_opt = engine->get_state("hitl-001");
    auto messages  = channel_value(state_opt, "messages");
    if (messages.size() > 0) {
        auto last = messages[messages.size() - 1];
        if (last.contains("tool_calls") && last["tool_calls"].size() > 0) {
            auto tc = last["tool_calls"][0];
            std::cout << "Pending tool: " << tc.value("name", "?") << "\n"
                      << "Arguments:    "
                      << tc.value("arguments", neograph::json::object()).dump()
                      << "\n";
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

    auto final_state = engine->get_state("hitl-001");
    auto final_msgs  = channel_value(final_state, "messages");
    if (final_msgs.size() > 0) {
        auto last = final_msgs[final_msgs.size() - 1];
        std::cout << "Assistant: " << last.value("content", "") << "\n";
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
