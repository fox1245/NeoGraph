// NeoGraph Example 07: Intent-based Dynamic Routing
//
// The LLM classifies user intent and dynamically routes
// to specialized expert subgraphs based on the result.
//
// Scenario: Panel of Experts
//   classifier → (math? → math_expert, translate? → translate_expert, else → general)
//
// No API key required (uses Mock Provider)
//
// Usage: ./example_intent_routing

#include <neograph/neograph.h>

#include <iostream>

// Mock Provider: intent classification + expert responses
class RoutingMockProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& params) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        // First call: intent classification (called by IntentClassifierNode)
        // → if system prompt contains "Classify", return classification response
        bool is_classifier = false;
        for (const auto& msg : params.messages) {
            if (msg.role == "system" && msg.content.find("Classify") != std::string::npos) {
                is_classifier = true;
                break;
            }
        }

        if (is_classifier) {
            // Determine intent from user message
            std::string user_msg;
            for (const auto& msg : params.messages) {
                if (msg.role == "user") user_msg = msg.content;
            }

            if (user_msg.find("calculate") != std::string::npos ||
                user_msg.find("plus") != std::string::npos ||
                user_msg.find("+") != std::string::npos) {
                result.message.content = "math";
            } else if (user_msg.find("translate") != std::string::npos ||
                       user_msg.find("translate") != std::string::npos) {
                result.message.content = "translate";
            } else {
                result.message.content = "general";
            }
        } else {
            // Expert response — inspect user message to determine which expert branch ran.
            // (Subgraph nodes inherit parent's NodeContext, so system-prompt differentiation
            //  is not available here; user-message inspection is the reliable signal.)
            std::string user_msg;
            for (const auto& msg : params.messages) {
                if (msg.role == "user") user_msg = msg.content;
            }

            if (user_msg.find("plus") != std::string::npos ||
                user_msg.find("+") != std::string::npos ||
                user_msg.find("calculate") != std::string::npos) {
                result.message.content = "I'm a math expert. 42 + 58 = 100.";
            } else if (user_msg.find("translate") != std::string::npos) {
                result.message.content = "I'm a translation expert. 'Hello' -> 'Bonjour'";
            } else {
                result.message.content = "I'm a general assistant. I can help with anything.";
            }
        }

        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
    std::string get_name() const override { return "routing_mock"; }
};

int main() {
    auto provider = std::make_shared<RoutingMockProvider>();

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;

    // Graph definition
    neograph::json definition = {
        {"name", "intent_router"},
        {"channels", {
            {"messages",  {{"reducer", "append"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            // Intent classifier
            {"classifier", {
                {"type", "intent_classifier"},
                {"routes", neograph::json::array({"math", "translate", "general"})},
                {"prompt", "Classify the user's intent. Respond with ONLY one of: math, translate, general"}
            }},
            // Expert subgraphs
            {"math_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "math_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }},
            {"translate_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "translate_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }},
            {"general_expert", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "general_agent"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
                    {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                    {"edges", neograph::json::array({
                        {{"from", "__start__"}, {"to", "llm"}},
                        {{"from", "llm"}, {"to", "__end__"}}
                    })}
                }}
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "classifier"}},
            // Route based on intent
            {{"from", "classifier"}, {"condition", "route_channel"},
             {"routes", {
                 {"math", "math_expert"},
                 {"translate", "translate_expert"},
                 {"general", "general_expert"}
             }}},
            {{"from", "math_expert"}, {"to", "__end__"}},
            {{"from", "translate_expert"}, {"to", "__end__"}},
            {{"from", "general_expert"}, {"to", "__end__"}}
        })}
    };

    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);

    // Run 3 test cases
    struct TestCase {
        std::string question;
        std::string expected_route;
    };

    std::vector<TestCase> cases = {
        {"What is 42 plus 58?", "math"},
        {"translate Hello to French", "translate"},
        {"What should I do today?", "general"}
    };

    for (const auto& tc : cases) {
        std::cout << "=== Q: " << tc.question << " ===\n";

        neograph::graph::RunConfig config;
        config.input = {{"messages", neograph::json::array({
            {{"role", "user"}, {"content", tc.question}}
        })}};

        auto result = engine->run_stream(config,
            [](const neograph::graph::GraphEvent& event) {
                if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN)
                    std::cout << event.data.get<std::string>();
            });

        std::cout << "\nTrace: ";
        for (size_t i = 0; i < result.execution_trace.size(); ++i) {
            std::cout << result.execution_trace[i];
            if (i + 1 < result.execution_trace.size()) std::cout << " → ";
        }
        std::cout << " → END\n\n";
    }

    return 0;
}
