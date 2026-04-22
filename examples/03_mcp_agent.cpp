// NeoGraph Example 03: Real MCP Agent
//
// A graph agent that uses a real LLM API + MCP server tools.
// Loads the API key from a .env file, discovers tools from the MCP server,
// and runs a ReAct graph.
//
// Prerequisites:
//   1. Set OPENAI_API_KEY in the .env file
//   2. Run an MCP server (e.g., python server.py --transport streamable-http --port 8000)
//
// Usage:
//   ./example_mcp_agent <MCP_SERVER_URL> "<question>"
//
// Example:
//   ./example_mcp_agent http://localhost:8000 "Recommend dog food for me"

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char* argv[]) {
    cppdotenv::auto_load_dotenv();

    try {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <MCP_SERVER_URL> \"<question>\"\n"
                  << "Example: " << argv[0] << " http://localhost:8000 \"Recommend dog food for me\"\n";
        return 1;
    }

    std::string mcp_url = argv[1];
    std::string question = argv[2];

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Error: OPENAI_API_KEY not set (env or .env file)\n";
        return 1;
    }

    // 2. Create LLM Provider
    neograph::llm::OpenAIProvider::Config llm_config;
    llm_config.api_key = api_key;
    llm_config.default_model = "gpt-4o-mini";
    auto provider = std::shared_ptr<neograph::Provider>(
        neograph::llm::OpenAIProvider::create(llm_config)
    );

    // 3. Discover tools from the MCP server
    std::cout << "[*] Connecting to MCP server: " << mcp_url << "\n";

    neograph::mcp::MCPClient mcp_client(mcp_url);
    auto tools = mcp_client.get_tools();

    std::cout << "[*] Discovered " << tools.size() << " tools:\n";
    for (const auto& tool : tools) {
        auto def = tool->get_definition();
        std::cout << "    - " << def.name << ": " << def.description.substr(0, 60) << "...\n";
    }
    std::cout << "\n";

    // 4. Create ReAct graph (LLM + MCP tools)
    auto engine = neograph::graph::create_react_graph(
        provider,
        std::move(tools),
        "You are a helpful assistant. Use the available tools to answer the user's question. "
        "Always respond in the same language as the user's question."
    );

    // 5. Execute
    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", question}}
    })}};
    config.max_steps = 20;

    std::cout << "User: " << question << "\n\n";
    std::cout << "Assistant: " << std::flush;

    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            switch (event.type) {
                case neograph::graph::GraphEvent::Type::NODE_START:
                    if (event.node_name == "tools") {
                        std::cerr << "\n[tool call in progress...]\n";
                    }
                    break;
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                    std::cout << event.data.get<std::string>() << std::flush;
                    break;
                default:
                    break;
            }
        });

    std::cout << "\n\n";
    std::cout << "[*] Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
