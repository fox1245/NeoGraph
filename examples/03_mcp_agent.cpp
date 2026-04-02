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

#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

// .env file loader — parses KEY=VALUE pairs and sets them as environment variables
static void load_dotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        // Also try the parent directory (for running from build/)
        file.open("../" + path);
        if (!file.is_open()) return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim leading/trailing whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        // Remove surrounding quotes
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }

        setenv(key.c_str(), val.c_str(), 0);  // 0 = do not overwrite existing values
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <MCP_SERVER_URL> \"<question>\"\n"
                  << "Example: " << argv[0] << " http://localhost:8000 \"Recommend dog food for me\"\n";
        return 1;
    }

    std::string mcp_url = argv[1];
    std::string question = argv[2];

    // 1. Load .env
    load_dotenv();

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Error: OPENAI_API_KEY not set (check .env file)\n";
        return 1;
    }

    // 2. Create LLM Provider
    auto provider = std::shared_ptr<neograph::Provider>(
        neograph::llm::OpenAIProvider::create({
            .api_key = api_key,
            .default_model = "gpt-4o-mini"
        })
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
}
