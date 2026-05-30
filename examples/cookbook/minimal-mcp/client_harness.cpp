// minimal-mcp cookbook — drive NeoGraph's built-in MCP client with NO LLM.
//
// Spawns an MCP stdio server (subprocess), discovers its tools, then calls
// each tool directly and prints the result. This isolates the MCP protocol
// round-trip from any OpenAI dependency: the other bundled MCP examples
// (03 / 20 / 22) wrap the client in a ReAct loop that needs OPENAI_API_KEY,
// which hides the fact that the protocol layer itself is key-free and
// peer-agnostic.
//
// Pair it with min_stdio_server.py (a ~60-line stdlib server, no fastmcp):
//
//   ./cookbook_minimal_mcp python3 examples/cookbook/minimal-mcp/min_stdio_server.py
//
// Any executable that speaks MCP over stdio works in place of that server.

#include <neograph/neograph.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <python> <server.py>\n"
                  << "  e.g. " << argv[0]
                  << " python3 examples/cookbook/minimal-mcp/min_stdio_server.py\n";
        return 1;
    }
    std::vector<std::string> server_argv{argv[1], argv[2]};
    std::cout << "[*] Spawning stdio MCP server: " << argv[1] << " " << argv[2] << "\n";

    try {
        neograph::mcp::MCPClient client(server_argv);
        if (!client.initialize("minimal-mcp-cookbook")) {
            std::cerr << "[!] initialize failed\n";
            return 1;
        }
        std::cout << "[*] initialize OK\n";

        auto tools = client.get_tools();
        std::cout << "[*] tools/list -> " << tools.size() << " tools:\n";
        for (const auto& t : tools) {
            auto def = t->get_definition();
            std::cout << "    - " << def.name << ": " << def.description << "\n";
        }

        struct Call { std::string tool; neograph::json args; };
        std::vector<Call> calls = {
            {"get_current_time", {{"timezone", "UTC"}}},
            {"calculate",        {{"expression", "2 ** 16 + 1"}}},
            {"get_weather",      {{"city", "Tokyo"}}},
        };

        std::cout << "\n[*] tools/call round-trips:\n";
        int ok = 0;
        for (const auto& c : calls) {
            neograph::json res = client.call_tool(c.tool, c.args);
            std::string text;
            if (res.contains("content") && res["content"].is_array()) {
                for (const auto& item : res["content"])
                    if (item.value("type", "") == "text") text += item.value("text", "");
            } else {
                text = res.dump();
            }
            std::cout << "    " << c.tool << "(" << c.args.dump() << ") -> " << text << "\n";
            ++ok;
        }
        std::cout << "\n[*] " << ok << "/" << calls.size()
                  << " MCP tool calls succeeded (no LLM, no fastmcp)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
