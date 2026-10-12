// NeoGraph Example 03: MCP tools over Streamable HTTP
//
// An MCP server publishes tools. MCPClient discovers them, and get_tools() hands each one to a
// ReAct graph as an ordinary neograph::Tool, so the graph does not know the tools run in
// another process. The server is examples/mcp_demo_server.py: standard library only, nothing
// to install. A scripted model plays the LLM, so no key is needed.
//
// Usage:
//   python3 examples/mcp_demo_server.py --http 8765                       # terminal 1
//   ./example_mcp_agent http://127.0.0.1:8765/mcp                         # terminal 2
//   ./example_mcp_agent http://127.0.0.1:8765/mcp "What is 6 * 7?" --live # real model
//                                      (needs OPENROUTER_API_KEY in the environment; billed)
//
// smoke-companion: @PYTHON@ @SRC@/examples/mcp_demo_server.py --http @PORT@
// smoke-wait-port: @PORT@
// smoke-args: http://127.0.0.1:@PORT@/mcp
// smoke-expect: tools: get_current_time calculate get_weather kb_lookup save_note list_notes
// smoke-expect: [calculate] {"expression":"15 * 28 + 7"} -> 427
// smoke-expect: Final answer: 427
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// The text of the last assistant message that has any.
static std::string final_text(const std::vector<sp::Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != sp::Role::Assistant) continue;
        std::string text;
        for (const auto& part : it->parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
        if (!text.empty()) return text;
    }
    return "(empty)";
}

int main(int argc, char** argv) {
    // Positional arguments only; flags such as --live are read by choose_provider.
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]).rfind("--", 0) != 0) args.emplace_back(argv[i]);
    const std::string url = args.size() > 0 ? args[0] : "http://127.0.0.1:8765/mcp";
    const std::string question = args.size() > 1 ? args[1] : "What is 15 * 28 + 7?";

    try {
        // The scripted model asks the server's calculator, then answers from its reply.
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [](const examples::ScriptedTurn& turn) {
                if (!turn.has_tool_result())
                    return examples::call_tool("calculate", {{"expression", "15 * 28 + 7"}});
                return examples::say("Final answer: " + turn.last_tool_result());
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        // Handshake, then discover the server's tools (tools/list).
        neograph::mcp::MCPClient client(url);
        client.initialize("example-mcp-agent");
        auto tools = client.get_tools();
        std::cout << "tools:";
        for (const auto& tool : tools) std::cout << " " << tool->get_name();
        std::cout << "\n";

        // The MCP tools go into a ReAct graph exactly like local ones.
        auto engine = neograph::graph::create_react_graph(
            provider, std::move(tools),
            "You are a helpful assistant. Use the available tools to answer the question.",
            examples::openrouter_model);

        neograph::graph::RunConfig run;
        run.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};
        run.max_steps = 20;
        const auto result = engine->run(run);

        // Show what crossed the wire: each tool call and the text the server returned.
        std::string last_call;
        for (const auto& message : result.native_messages)
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<sp::ToolCall>(&part))
                    last_call = call->name + " " + (call->input ? call->input->root().dump() : "{}");
                else if (const auto* reply = std::get_if<sp::ToolResult>(&part))
                    std::cout << "[" << last_call.substr(0, last_call.find(' ')) << "] "
                              << last_call.substr(last_call.find(' ') + 1) << " -> " << reply->content << "\n";
            }
        std::cout << final_text(result.native_messages) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n"
                  << "Is an MCP server listening at " << url
                  << "?  python3 examples/mcp_demo_server.py --http 8765\n";
        return 1;
    }
}
