// NeoGraph Example 24: operator feedback as new conversation content
//
// Round 1: the agent answers from its own knowledge and does not call a tool. The operator
// reads the draft, finds it too vague, and appends a message that asks for the real reading.
// Round 2 runs on the full history, draft plus feedback, so the model sees the feedback in
// context and calls the MCP weather tool. Compare example 20, where the operator only approves
// or stops a call: here the feedback itself is content the model must take into account.
// The scripted model branches on what it was sent, so the second round can only call the
// tool if the feedback really reached it. The server is examples/mcp_demo_server.py.
//
// Usage:
//   python3 examples/mcp_demo_server.py --http 8765          # terminal 1
//   ./example_mcp_feedback http://127.0.0.1:8765/mcp         # terminal 2
//   (add --live for a real model; OPENROUTER_API_KEY in the environment, billed)
//
// smoke-companion: @PYTHON@ @SRC@/examples/mcp_demo_server.py --http @PORT@
// smoke-wait-port: @PORT@
// smoke-args: http://127.0.0.1:@PORT@/mcp
// smoke-expect: round 1 used tools: no
// smoke-expect: round 2 used tools: yes
// smoke-expect: revised: Seoul: 18C, cloudy
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/graph/react_graph.h>
#include <neograph/mcp/client.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

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

static bool used_tools(const std::vector<std::string>& trace) {
    return std::find(trace.begin(), trace.end(), "tools") != trace.end();
}

int main(int argc, char** argv) {
    std::string url = "http://127.0.0.1:8765/mcp";
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]).rfind("--", 0) != 0) url = argv[i];

    try {
        const std::string feedback = "That is a guess. Please call the weather tool and give me the real reading.";
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [feedback](const examples::ScriptedTurn& turn) {
                if (turn.last_user_text() != feedback)  // round 1: no feedback yet
                    return examples::say("draft: it is probably mild and clear in Seoul.");
                if (!turn.has_tool_result()) return examples::call_tool("get_weather", {{"city", "Seoul"}});
                return examples::say("revised: " + turn.last_tool_result());
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        neograph::mcp::MCPClient client(url);
        client.initialize("example-mcp-feedback");
        auto engine = neograph::graph::create_react_graph(
            provider, client.get_tools(),
            "Answer from your own knowledge. Use a tool only when the user asks for real data.",
            examples::openrouter_model);

        // Round 1.
        neograph::graph::RunConfig first;
        first.provider_messages = std::vector<sp::Message>{
            examples::message(sp::Role::User, "What is the weather in Seoul right now?")};
        const auto draft = engine->run(first);
        std::cout << final_text(draft.native_messages) << "\n";
        std::cout << "round 1 used tools: " << (used_tools(draft.execution_trace) ? "yes" : "no") << "\n";

        // The operator's feedback joins the conversation: the whole first round, then one more
        // user message. native_messages is the trusted history, so it is passed on unchanged.
        std::cout << "operator: " << feedback << "\n";
        neograph::graph::RunConfig second;
        second.provider_messages = draft.native_messages;
        second.provider_messages->push_back(examples::message(sp::Role::User, feedback));
        const auto revised = engine->run(second);
        std::cout << final_text(revised.native_messages) << "\n";
        std::cout << "round 2 used tools: " << (used_tools(revised.execution_trace) ? "yes" : "no") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
