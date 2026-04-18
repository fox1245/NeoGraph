// NeoGraph Example 22: MCP over stdio transport
//
// Demonstrates spawning an MCP server as a child subprocess and exchanging
// newline-delimited JSON-RPC messages over its stdin / stdout — no network
// stack involved. The subprocess lives for the lifetime of the MCPClient
// (or the last MCPTool referring back to its session); destruction sends
// SIGTERM and reaps via waitpid.
//
// Scenario:
//   - Launch examples/demo_mcp_stdio_server.py (kb_lookup / save_note / list_notes).
//   - Discover its tools via get_tools().
//   - Drive a ReAct loop against OpenAI, letting the LLM pick which tool to call.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_mcp_stdio \
//       /tmp/mcp_venv/bin/python /path/to/examples/demo_mcp_stdio_server.py
//
// Equivalent to the HTTP example 03 but with transport=stdio.

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/client.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

static std::string load_env_var(const std::string& key) {
    // Prefer environment variable. Fall back to a .env file in CWD.
    if (const char* v = std::getenv(key.c_str())) return v;
    std::ifstream f(".env");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <python-path> <path/to/demo_mcp_stdio_server.py> [question]\n"
                  << "Example: " << argv[0]
                  << " python3 examples/demo_mcp_stdio_server.py\n";
        return 1;
    }

    const std::string question = (argc >= 4)
        ? std::string(argv[3])
        : "Look up what NeoGraph is, then save a note containing the result.";

    std::string api_key = load_env_var("OPENAI_API_KEY");
    if (api_key.empty()) {
        std::cerr << "OPENAI_API_KEY not set (env or .env file)\n";
        return 1;
    }

    // --- Spawn the MCP server as a subprocess ---
    std::vector<std::string> server_argv{argv[1], argv[2]};
    std::cout << "[*] Spawning stdio MCP server: "
              << argv[1] << " " << argv[2] << "\n";

    neograph::mcp::MCPClient mcp_client(server_argv);

    auto tools = mcp_client.get_tools();
    std::cout << "[*] Discovered " << tools.size() << " tools over stdio:\n";
    for (const auto& t : tools) {
        auto def = t->get_definition();
        std::cout << "    - " << def.name << ": "
                  << def.description.substr(0, 70)
                  << (def.description.size() > 70 ? "..." : "") << "\n";
    }

    // --- LLM provider ---
    neograph::llm::OpenAIProvider::Config config;
    config.api_key       = api_key;
    config.default_model = "gpt-4o-mini";
    std::shared_ptr<neograph::Provider> provider =
        neograph::llm::OpenAIProvider::create(config);

    // --- Wire the stdio tools into a ReAct graph ---
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools),
        "You are an assistant that uses local tools to answer questions. "
        "Always call the tools when information is needed.");

    neograph::graph::RunConfig run_config;
    run_config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};

    std::cout << "\nUser: " << question << "\n\nAssistant: \n";

    auto result = engine->run_stream(run_config,
        [](const neograph::graph::GraphEvent& event) {
            if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
                std::cout << event.data.get<std::string>() << std::flush;
            } else if (event.type == neograph::graph::GraphEvent::Type::NODE_START &&
                       event.node_name == "tools") {
                std::cout << "\n[tool call via stdio...]\n";
            }
        });

    std::cout << "\n\n[*] Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    // MCPClient destructor reaps the subprocess.
    return 0;
}
