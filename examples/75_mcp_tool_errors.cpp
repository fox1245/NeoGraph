// NeoGraph Example 75: two kinds of MCP failure, and an agent that recovers from both
//
// MCP reports failure in two different ways. A tool that ran and failed returns a normal result
// with isError true (division by zero). A request the server cannot accept is a JSON-RPC
// error (a missing required argument, code -32602). The client keeps them apart: part 1 calls
// the server directly and shows each. Part 2 runs a ReAct loop in which the scripted model
// makes both mistakes in turn, reads the error text it is shown after each, and corrects
// itself. The server is examples/mcp_demo_server.py (standard library only) over stdio.
//
// Usage:
//   ./example_mcp_tool_errors python3 examples/mcp_demo_server.py
//
// smoke-args: @PYTHON@ @SRC@/examples/mcp_demo_server.py
// smoke-expect: direct isError: true, text: Error: division by zero
// smoke-expect: direct JSON-RPC error -32602: MCP JSON-RPC error: Invalid params: expression is required
// smoke-expect: model saw (error): {"error":"MCP tool execution error: Error: division by zero","status":"failed"
// smoke-expect: model saw (error): {"error":"MCP JSON-RPC error: Invalid params: expression is required","status":"failed"
// smoke-expect: Final answer: 2.5
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <iostream>
#include <string>
#include <vector>

namespace {
bool mentions(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <python> <path/to/mcp_demo_server.py>\n";
        return 1;
    }
    try {
        neograph::mcp::MCPClient client(std::vector<std::string>{argv[1], argv[2]});
        client.initialize("example-mcp-tool-errors");

        // Part 1: the two failures as the client API reports them.
        const auto failed = client.call_tool_result("calculate", {{"expression", "10 / 0"}});
        std::cout << "direct isError: " << std::boolalpha << failed.is_error
                  << ", text: " << failed.content[0]["text"].get<std::string>() << "\n";
        try {
            client.call_tool_result("calculate", {{"expr", "10 / 4"}});
        } catch (const neograph::mcp::MCPError& error) {
            std::cout << "direct JSON-RPC error " << error.code() << ": " << error.what() << "\n";
        }

        // Part 2: an agent that reads both errors. The responder branches on the text it was
        // shown, so each correction below happens only because the error reached the model.
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [](const examples::ScriptedTurn& turn) {
                if (!turn.has_tool_result())
                    return examples::call_tool("calculate", {{"expression", "10 / 0"}});
                const std::string seen = turn.last_tool_result();
                if (mentions(seen, "division by zero"))  // isError: wrong operands, wrong key too
                    return examples::call_tool("calculate", {{"expr", "10 / 4"}});
                if (mentions(seen, "expression is required"))  // JSON-RPC error: fix the key
                    return examples::call_tool("calculate", {{"expression", "10 / 4"}});
                return examples::say("Final answer: " + seen);
            });
        auto provider = examples::choose_provider(argc, argv, scripted);
        auto engine = neograph::graph::create_react_graph(
            provider, client.get_tools(),
            "Use the calculate tool. If it returns an error, read the message and try again.",
            examples::openrouter_model);

        // A tool with no declared effect may have changed something, so a call that ran and
        // failed (the division by zero) is reported to the model as "reconciliation_required"
        // with "effect_uncertain". The calculator only reads; say so. A request the server
        // refuses before running it (the missing argument) is a plain failure whatever the policy.
        auto policies = std::make_shared<neograph::ToolExecutionPolicyRegistry>();
        neograph::ToolExecutionPolicy read_only;
        read_only.effect = neograph::ToolEffectClass::ReadOnly;
        read_only.idempotency = neograph::ToolIdempotency::Idempotent;
        policies->upsert("calculate", read_only);
        engine->set_tool_execution_controller(std::make_shared<neograph::ToolExecutionController>(
            neograph::ToolExecutionControllerConfig{.policies = policies}));

        neograph::graph::RunConfig run;
        run.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, "What is 10 / 4?")};
        run.max_steps = 20;
        const auto result = engine->run(run);

        std::string answer = "(empty)";
        for (const auto& message : result.native_messages)
            for (const auto& part : message.parts) {
                if (const auto* reply = std::get_if<sp::ToolResult>(&part))
                    std::cout << "model saw" << (reply->is_error ? " (error)" : "") << ": " << reply->content << "\n";
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
