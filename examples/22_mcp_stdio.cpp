// NeoGraph Example 22: MCP over stdio
//
// The server is a child process: MCPClient starts it, speaks newline-delimited JSON-RPC over
// its stdin and stdout, and ends it when the client goes away. No port, no network stack.
// Only the way the client is constructed differs from example 03 (a command line instead of a
// URL); the tools, the graph and the loop are the same. The server is
// examples/mcp_demo_server.py, standard library only, and a scripted model plays the LLM.
//
// Usage:
//   ./example_mcp_stdio python3 examples/mcp_demo_server.py
//   ./example_mcp_stdio python3 examples/mcp_demo_server.py "Look up MCP." --live   # real model
//                                      (needs OPENROUTER_API_KEY in the environment; billed)
//
// smoke-args: @PYTHON@ @SRC@/examples/mcp_demo_server.py
// smoke-expect: tools: get_current_time calculate get_weather kb_lookup save_note list_notes
// smoke-expect: [kb_lookup] {"topic":"neograph"} -> NeoGraph is a C++ graph engine for agents.
// smoke-expect: [save_note] {"content":"NeoGraph is a C++ graph engine for agents."} -> note#1 saved
// smoke-expect: Final answer: note#1 saved
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;  // positional arguments; flags such as --live are skipped
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]).rfind("--", 0) != 0) args.emplace_back(argv[i]);
    if (args.size() < 2) {
        std::cerr << "Usage: " << argv[0] << " <python> <path/to/mcp_demo_server.py> [question] [--live]\n";
        return 1;
    }
    const std::string question = args.size() > 2 ? args[2] : "Look up what NeoGraph is and save a note about it.";

    try {
        // Two tool calls in a row: look the topic up, then save what came back as a note.
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [](const examples::ScriptedTurn& turn) {
                switch (turn.tool_result_count()) {
                    case 0: return examples::call_tool("kb_lookup", {{"topic", "neograph"}});
                    case 1: return examples::call_tool("save_note", {{"content", turn.last_tool_result()}});
                    default: return examples::say("Final answer: " + turn.last_tool_result());
                }
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        // The command line starts the server as a child process; it lives as long as the client
        // (or any tool it produced) is alive.
        neograph::mcp::MCPClient client(std::vector<std::string>{args[0], args[1]});
        client.initialize("example-mcp-stdio");
        auto tools = client.get_tools();
        std::cout << "tools:";
        for (const auto& tool : tools) std::cout << " " << tool->get_name();
        std::cout << "\n";

        auto engine = neograph::graph::create_react_graph(
            provider, std::move(tools),
            "You are an assistant that uses local tools. Call a tool whenever you need information.",
            examples::openrouter_model);

        neograph::graph::RunConfig run;
        run.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};
        run.max_steps = 20;
        const auto result = engine->run(run);

        std::string last_call;
        std::string answer = "(empty)";
        for (const auto& message : result.native_messages)
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<sp::ToolCall>(&part))
                    last_call = call->name + " " + (call->input ? call->input->root().dump() : "{}");
                else if (const auto* reply = std::get_if<sp::ToolResult>(&part))
                    std::cout << "[" << last_call.substr(0, last_call.find(' ')) << "] "
                              << last_call.substr(last_call.find(' ') + 1) << " -> " << reply->content << "\n";
                else if (const auto* text = std::get_if<sp::Text>(&part))
                    if (message.role == sp::Role::Assistant) answer = text->value;
            }
        std::cout << answer << "\n";
        return 0;  // the client's destructor ends the child process
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
