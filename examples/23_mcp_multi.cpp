// NeoGraph Example 23: one agent, two MCP servers
//
// An agent can use tools from several MCP servers at once, over different transports. Each
// MCPClient owns its own connection; the tool lists they return are concatenated, and the model
// sees one flat list. Here a Streamable HTTP server offers calculate, get_weather and
// get_current_time, and a stdio child process offers kb_lookup, save_note and list_notes
// (both are examples/mcp_demo_server.py with a different --tools list). A scripted model plays
// the LLM, and its two calls go to different servers.
//
// Usage:
//   python3 examples/mcp_demo_server.py --http 8765 --tools get_current_time,calculate,get_weather
//   ./example_mcp_multi http://127.0.0.1:8765/mcp python3 examples/mcp_demo_server.py
//   (add --live for a real model; OPENROUTER_API_KEY in the environment, billed)
//
// smoke-companion: @PYTHON@ @SRC@/examples/mcp_demo_server.py --http @PORT@ --tools get_current_time,calculate,get_weather
// smoke-wait-port: @PORT@
// smoke-args: http://127.0.0.1:@PORT@/mcp @PYTHON@ @SRC@/examples/mcp_demo_server.py
// smoke-expect: tools (6): calculate get_current_time get_weather kb_lookup list_notes save_note
// smoke-expect: [get_weather] -> Tokyo: 22C, clear
// smoke-expect: [save_note] -> note#1 saved
// smoke-expect: Final answer: Tokyo: 22C, clear (note#1 saved)
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;  // positional arguments; flags such as --live are skipped
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]).rfind("--", 0) != 0) args.emplace_back(argv[i]);
    if (args.size() < 3) {
        std::cerr << "Usage: " << argv[0] << " <http-url> <python> <path/to/mcp_demo_server.py> [--live]\n";
        return 1;
    }

    try {
        // Weather comes from the HTTP server, then the note goes to the stdio server.
        auto weather = std::make_shared<std::string>();
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [weather](const examples::ScriptedTurn& turn) {
                switch (turn.tool_result_count()) {
                    case 0: return examples::call_tool("get_weather", {{"city", "Tokyo"}});
                    case 1:
                        *weather = turn.last_tool_result();
                        return examples::call_tool("save_note", {{"content", *weather}});
                    default: return examples::say("Final answer: " + *weather + " (" + turn.last_tool_result() + ")");
                }
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        // Two clients, two transports. Each client keeps its own connection alive.
        neograph::mcp::MCPClient http_client(args[0]);
        http_client.initialize("example-mcp-multi-http");
        neograph::mcp::MCPClient stdio_client(std::vector<std::string>{
            args[1], args[2], "--tools", "kb_lookup,save_note,list_notes"});
        stdio_client.initialize("example-mcp-multi-stdio");

        // One flat tool list for the agent.
        std::vector<std::unique_ptr<neograph::Tool>> tools;
        for (auto& tool : http_client.get_tools()) tools.push_back(std::move(tool));
        for (auto& tool : stdio_client.get_tools()) tools.push_back(std::move(tool));
        std::vector<std::string> names;
        for (const auto& tool : tools) names.push_back(tool->get_name());
        std::sort(names.begin(), names.end());
        std::cout << "tools (" << names.size() << "):";
        for (const auto& name : names) std::cout << " " << name;
        std::cout << "\n";

        auto engine = neograph::graph::create_react_graph(
            provider, std::move(tools),
            "You have tools from two servers. Weather first, then save a note about it.",
            examples::openrouter_model);

        neograph::graph::RunConfig run;
        run.provider_messages = std::vector<sp::Message>{examples::message(
            sp::Role::User, "What is the weather in Tokyo? Then save a note with the answer.")};
        run.max_steps = 20;
        const auto result = engine->run(run);

        std::string last_tool;
        std::string answer = "(empty)";
        for (const auto& message : result.native_messages)
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<sp::ToolCall>(&part)) last_tool = call->name;
                else if (const auto* reply = std::get_if<sp::ToolResult>(&part))
                    std::cout << "[" << last_tool << "] -> " << reply->content << "\n";
                else if (const auto* text = std::get_if<sp::Text>(&part))
                    if (message.role == sp::Role::Assistant) answer = text->value;
            }
        std::cout << answer << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
