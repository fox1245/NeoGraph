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
//   - Drive a ReAct loop against OpenRouter's pinned DeepSeek model, letting
//     the LLM pick which tool to call.
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_mcp_stdio python3 examples/demo_mcp_stdio_server.py
// (auto-loads .env from the cwd or any parent directory.)
//
// Equivalent to the HTTP example 03 but with transport=stdio.

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <neograph/mcp/client.h>
#include <neograph/graph/react_graph.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    try {
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

    const char* key_env = std::getenv("OPENROUTER_API_KEY");
    if (!key_env) {
        std::cerr << "OPENROUTER_API_KEY not set (env or .env file)\n";
        return 1;
    }
    std::string api_key = key_env;

    // --- Spawn the MCP server as a subprocess ---
    std::vector<std::string> server_argv{argv[1], argv[2]};
    std::cout << "[*] Spawning stdio MCP server: "
              << argv[1] << " " << argv[2] << "\n";

    neograph::mcp::MCPClient mcp_client(server_argv);
    mcp_client.initialize("example-mcp-stdio");

    auto tools = mcp_client.get_tools();
    std::cout << "[*] Discovered " << tools.size() << " tools over stdio:\n";
    for (const auto& t : tools) {
        auto def = t->get_definition();
        std::cout << "    - " << def.name << ": "
                  << def.description.substr(0, 70)
                  << (def.description.size() > 70 ? "..." : "") << "\n";
    }

    // --- LLM provider ---
    std::shared_ptr<neograph::Provider> provider =
        examples::make_openrouter_provider(api_key);

    // --- Wire the stdio tools into a ReAct graph ---
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools),
        "You are an assistant that uses local tools to answer questions. "
        "Always call the tools when information is needed.", examples::openrouter_model);

    neograph::graph::RunConfig run_config;
    run_config.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};
    run_config.on_provider_event = [](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event);
            delta && delta->payload.kind == sp::PartKind::Text &&
            delta->payload.channel == sp::DeltaChannel::Content)
            std::cout << delta->payload.bytes << std::flush;
    };

    std::cout << "\nUser: " << question << "\n\nAssistant: \n";

    auto result = engine->run_stream(run_config,
        [](const neograph::graph::GraphEvent& event) {
            if (event.type == neograph::graph::GraphEvent::Type::NODE_START &&
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
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
