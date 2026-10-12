// NeoGraph Example 20: approve an MCP tool call before it runs
//
// MCP tools do things in another process, so an agent often needs a person to approve a call
// first. The graph below is the ReAct loop with interrupt_before on the tool node: it saves a
// checkpoint and stops before any MCP tool is invoked, the operator reads the pending call,
// and resume() continues from that checkpoint. The earlier model call is not repeated: the
// scripted model is asked exactly twice. The server is examples/mcp_demo_server.py.
//
// Usage:
//   python3 examples/mcp_demo_server.py --http 8765          # terminal 1
//   ./example_mcp_hitl http://127.0.0.1:8765/mcp             # terminal 2
//   ./example_mcp_hitl http://127.0.0.1:8765/mcp --live      # real model (OPENROUTER_API_KEY; billed)
//
// smoke-companion: @PYTHON@ @SRC@/examples/mcp_demo_server.py --http @PORT@
// smoke-wait-port: @PORT@
// smoke-args: http://127.0.0.1:@PORT@/mcp
// smoke-expect: paused before node: tools
// smoke-expect: pending tool: get_weather {"city":"Tokyo"}
// smoke-expect: answer: Tokyo: 22C, clear
// smoke-expect: model calls: 2
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/mcp/client.h>

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    std::string url = "http://127.0.0.1:8765/mcp";
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]).rfind("--", 0) != 0) url = argv[i];

    try {
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [](const examples::ScriptedTurn& turn) {
                if (!turn.has_tool_result()) return examples::call_tool("get_weather", {{"city", "Tokyo"}});
                return examples::say("answer: " + turn.last_tool_result());
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        neograph::mcp::MCPClient client(url);
        client.initialize("example-mcp-hitl");

        // ReAct graph, with a checkpoint store and an interrupt before the tool node.
        const neograph::json definition = {
            {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
            {"name", "mcp_hitl"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"llm", {{"type", "llm_call"}}}, {"tools", {{"type", "tool_dispatch"}}}}},
            {"edges", neograph::json::array({
                {{"from", "__start__"}, {"to", "llm"}},
                {{"from", "llm"}, {"condition", "has_tool_calls"},
                 {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
                {{"from", "tools"}, {"to", "llm"}}})},
            {"interrupt_before", neograph::json::array({"tools"})},
        };
        neograph::graph::NodeContext context;
        context.provider = provider;
        context.model = examples::openrouter_model;
        context.instructions = "Call a tool when the question can be answered by one.";

        neograph::graph::EngineResources resources;
        resources.tools = neograph::ToolSet(client.get_tools());
        auto engine = neograph::graph::GraphEngine::build(
            definition,
            neograph::graph::EngineConfig{
                .node_context = context,
                .checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>()},
            std::move(resources));

        // Phase 1: run until the gate.
        neograph::graph::RunConfig run;
        run.thread_id = "hitl-001";
        run.provider_messages = std::vector<sp::Message>{
            examples::message(sp::Role::User, "What is the weather in Tokyo?")};
        const auto paused = engine->run(run);
        if (!paused.interrupted) {
            std::cerr << "expected the run to pause before the tool node\n";
            return 1;
        }
        std::cout << "paused before node: " << paused.interrupt_node << "\n";
        for (const auto& call : neograph::client_tool_calls(paused.native_messages.back()))
            std::cout << "pending tool: " << call.name << " " << call.arguments << "\n";

        // Phase 2: the operator approves. Resume without a value: a value would be inserted as a
        // user message between the tool call and its result, which providers reject.
        std::cout << "operator: approved\n";
        const auto done = engine->resume("hitl-001", neograph::json());
        std::string answer;
        for (const auto& part : done.native_messages.back().parts)
            if (const auto* text = std::get_if<sp::Text>(&part)) answer += text->value;
        std::cout << answer << "\n";
        if (auto* local = dynamic_cast<examples::ScriptedProvider*>(provider.get()))
            std::cout << "model calls: " << local->call_count() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
