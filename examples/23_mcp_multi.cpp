// NeoGraph Example 23: Multi-MCP Routing
//
// One ReAct agent wired to *two* MCP servers at once — an HTTP server
// (time / weather / calculator) and a stdio subprocess (kb_lookup /
// save_note / list_notes). get_tools() from each client is concatenated
// into a single tool list, so the LLM picks across both servers
// transparently.
//
// The two clients each own their own transport; the merged tool vector
// keeps them alive (each MCPTool retains the right back-reference).
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_mcp_multi \
//       http://localhost:8000 \
//       /tmp/mcp_venv/bin/python examples/demo_mcp_stdio_server.py

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/client.h>
#include <neograph/graph/react_graph.h>

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

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <http-url> <python-path> <stdio-server.py> [question]\n";
        return 1;
    }
    const std::string http_url   = argv[1];
    const std::string py         = argv[2];
    const std::string stdio_srv  = argv[3];
    const std::string question   = (argc >= 5) ? argv[4]
        : "What is the weather in Tokyo, and then save a note summarizing the answer.";

    std::string api_key = load_env("OPENAI_API_KEY");
    if (api_key.empty()) { std::cerr << "OPENAI_API_KEY missing\n"; return 1; }

    // --- Two MCP clients, different transports ---
    std::cout << "[*] Connecting HTTP MCP: " << http_url << "\n";
    neograph::mcp::MCPClient http_client(http_url);
    auto http_tools = http_client.get_tools();

    std::cout << "[*] Spawning stdio MCP:  " << py << " " << stdio_srv << "\n";
    neograph::mcp::MCPClient stdio_client(std::vector<std::string>{py, stdio_srv});
    auto stdio_tools = stdio_client.get_tools();

    // --- Merge tool lists ---
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.reserve(http_tools.size() + stdio_tools.size());
    for (auto& t : http_tools)  tools.push_back(std::move(t));
    for (auto& t : stdio_tools) tools.push_back(std::move(t));

    std::cout << "[*] Combined tools exposed to the agent (" << tools.size() << "):\n";
    for (const auto& t : tools) {
        auto d = t->get_definition();
        std::cout << "    - " << d.name << "\n";
    }

    // --- LLM ---
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = api_key;
    cfg.default_model = "gpt-4o-mini";
    std::shared_ptr<neograph::Provider> provider =
        neograph::llm::OpenAIProvider::create(cfg);

    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools),
        "You are an agent with tools from two different MCP servers. "
        "Use whichever tools are appropriate.");

    neograph::graph::RunConfig run;
    run.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};

    std::cout << "\nUser: " << question << "\n\nAssistant: \n";

    auto result = engine->run_stream(run,
        [](const neograph::graph::GraphEvent& e) {
            if (e.type == neograph::graph::GraphEvent::Type::LLM_TOKEN)
                std::cout << e.data.get<std::string>() << std::flush;
            else if (e.type == neograph::graph::GraphEvent::Type::NODE_START &&
                     e.node_name == "tools")
                std::cout << "\n[tool call...]\n";
        });

    std::cout << "\n\n[*] Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";
    return 0;
}
