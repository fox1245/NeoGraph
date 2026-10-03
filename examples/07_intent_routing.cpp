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
#include "provider_example_support.h"

#include <iostream>

// Offline classification uses the full typed request, including system turns.
class RoutingMockProvider : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const neograph::PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                bool classifier = false;
                std::string user;
                for (const auto& message : *history) {
                    std::string text;
                    for (const auto& part : message.parts)
                        if (const auto* value = std::get_if<sp::Text>(&part))
                            text += value->value;
                    if (message.role == sp::Role::System)
                        classifier |= text.find("Classify") != std::string::npos;
                    if (message.role == sp::Role::User) user = std::move(text);
                }
                const bool math = user.find("calculate") != std::string::npos ||
                    user.find("plus") != std::string::npos ||
                    user.find("+") != std::string::npos;
                const bool translate = user.find("translate") != std::string::npos;
                std::string reply;
                if (classifier)
                    reply = math ? "math" : translate ? "translate" : "general";
                else if (math)
                    reply = "I'm a math expert. 42 + 58 = 100.";
                else if (translate)
                    reply = "I'm a translation expert. 'Hello' -> 'Bonjour'";
                else
                    reply = "I'm a general assistant. I can help with anything.";
                sp::Completion completion;
                completion.messages.push_back(
                    examples::message(sp::Role::Assistant, std::move(reply)));
                completion.stop.kind = sp::StopKind::EndTurn;
                if (prepared.mode() == neograph::ProviderMode::Stream)
                    examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
    std::string get_name() const override { return "routing_mock"; }
};

int main() {
    auto provider = std::make_shared<RoutingMockProvider>();

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "fixture-routing";

    // Graph definition
    neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
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

    auto engine = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx});

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
