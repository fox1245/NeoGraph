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
//   OPENAI_API_KEY=sk-... ./example_mcp_feedback

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <fstream>
#include <cstdlib>

static std::string load_env(const std::string& k) {
    if (const char* v = std::getenv(k.c_str())) return v;
    std::ifstream f(".env"); std::string line;
    while (std::getline(f, line)) {
        auto e = line.find('=');
        if (e != std::string::npos && line.substr(0, e) == k) return line.substr(e + 1);
    }
    return {};
}

static neograph::json channel_messages(neograph::graph::GraphEngine* eng,
                                       const std::string& thread) {
    auto st = eng->get_state(thread);
    if (!st.has_value()) return neograph::json::array();
    auto channels = st->value("channels", neograph::json::object());
    auto entry    = channels.value("messages", neograph::json::object());
    return entry.value("value", neograph::json::array());
}

static std::string last_assistant(const neograph::json& messages) {
    if (!messages.is_array()) return {};
    for (size_t i = messages.size(); i-- > 0; ) {
        auto m = messages[i];
        if (m.value("role", "") == "assistant") {
            auto c = m.value("content", "");
            if (!c.empty()) return c;
        }
    }
    return "(empty)";
}

static bool trace_used_tools(const std::vector<std::string>& trace) {
    for (const auto& n : trace) if (n == "tools") return true;
    return false;
}

int main(int argc, char** argv) {
    const std::string mcp_url  = (argc >= 2) ? argv[1] : "http://localhost:8000";
    const std::string question = (argc >= 3) ? argv[2]
        : "What's the weather in Seoul right now?";

    std::string api_key = load_env("OPENAI_API_KEY");
    if (api_key.empty()) { std::cerr << "OPENAI_API_KEY missing\n"; return 1; }

    neograph::mcp::MCPClient mcp_client(mcp_url);
    auto tools = mcp_client.get_tools();
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());
    std::cout << "[*] " << tools.size()
              << " MCP tools available (agent may or may not call them)\n\n";

    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = api_key;
    cfg.default_model = "gpt-4o-mini";
    std::shared_ptr<neograph::Provider> provider =
        neograph::llm::OpenAIProvider::create(cfg);

    neograph::json definition = {
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
    ctx.tools    = tool_ptrs;
    ctx.instructions =
        "You are an assistant. Answer from your own knowledge first. "
        "Use the provided tools only if the user explicitly asks for "
        "real-time or external data.";

    auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);
    engine->own_tools(std::move(tools));

    // ---------- Round 1 ----------
    std::cout << "User: " << question << "\n\n";
    std::cout << "=== Round 1 — draft (agent tends to guess) ===\n";

    neograph::graph::RunConfig run1;
    run1.thread_id = "fb-001";
    run1.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};
    auto r1 = engine->run(run1);

    auto msgs_r1 = channel_messages(engine.get(), "fb-001");
    std::cout << "Assistant draft:\n  "
              << last_assistant(msgs_r1) << "\n\n";

    // ---------- Human feedback ----------
    const std::string feedback =
        "That's not good enough. Please actually call the MCP weather tool "
        "and give me the real current reading.";
    std::cout << ">>> Operator feedback:\n    \"" << feedback << "\"\n\n";

    // ---------- Round 2 — feed full history + feedback to a new run ----------
    std::cout << "=== Round 2 — agent incorporates the feedback ===\n";

    neograph::json round2_messages = msgs_r1;
    round2_messages.push_back(neograph::json{
        {"role", "user"}, {"content", feedback}
    });

    neograph::graph::RunConfig run2;
    run2.thread_id = "fb-002";   // fresh thread so state starts from our fed-in history
    run2.input = {{"messages", round2_messages}};
    auto r2 = engine->run(run2);

    auto msgs_r2 = channel_messages(engine.get(), "fb-002");
    std::cout << "Assistant (revised):\n  "
              << last_assistant(msgs_r2) << "\n\n";

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
}
