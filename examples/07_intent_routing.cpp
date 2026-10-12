// NeoGraph Example 07: intent routing
//
// A classifier node reads the request, writes a label, and a conditional edge sends the
// request to the expert for that label.
//
//   __start__ -> classifier -(route_channel)-> math_expert | translate_expert | general_expert
//
// `intent_classifier` is a built-in node: it asks the model to answer with one of its `routes`
// and writes the answer to the `__route__` channel; `route_channel` is the built-in condition
// that reads that channel. Each expert is a one-node subgraph. The provider is scripted
// (scripted_provider.h): it plays the classifier on the turn whose system prompt says
// "Classify" and an expert on every other turn, so no API key is needed.
//
// Usage: ./example_intent_routing
//
// smoke-expect: Trace: classifier → math_expert → END
// smoke-expect: Trace: classifier → translate_expert → END
// smoke-expect: Trace: classifier → general_expert → END
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"

#include <iostream>

using neograph::json;

// An expert is a subgraph with a single model call.
json expert(const char* name) {
    return {{"type", "subgraph"},
            {"definition", {
                {"name", name},
                {"channels", {{"messages", {{"reducer", "append"}}}}},
                {"nodes", {{"llm", {{"type", "llm_call"}}}}},
                {"edges", json::array({
                    {{"from", "__start__"}, {"to", "llm"}},
                    {{"from", "llm"}, {"to", "__end__"}}
                })}
            }}};
}

int main() {
    auto provider = std::make_shared<examples::ScriptedProvider>(
        [](const examples::ScriptedTurn& turn) {
            const std::string user = turn.last_user_text();
            auto has = [&](const char* word) { return user.find(word) != std::string::npos; };
            const bool math      = has("calculate") || has("plus") || has("+");
            const bool translate = has("translate");
            if (turn.system_text().find("Classify") != std::string::npos)   // the classifier's turn
                return examples::say(math ? "math" : translate ? "translate" : "general");
            if (math)      return examples::say("I'm a math expert. 42 + 58 = 100.");
            if (translate) return examples::say("I'm a translation expert. 'Hello' -> 'Bonjour'");
            return examples::say("I'm a general assistant. I can help with anything.");
        });

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model    = "scripted-model";

    const json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "intent_router"},
        {"channels", {
            {"messages",  {{"reducer", "append"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"classifier", {
                {"type", "intent_classifier"},
                {"routes", json::array({"math", "translate", "general"})},
                {"prompt", "Classify the user's intent. Respond with ONLY one of: math, translate, general"}
            }},
            {"math_expert",      expert("math_agent")},
            {"translate_expert", expert("translate_agent")},
            {"general_expert",   expert("general_agent")}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "classifier"}},
            {{"from", "classifier"}, {"condition", "route_channel"},
             {"routes", {
                 {"math", "math_expert"},
                 {"translate", "translate_expert"},
                 {"general", "general_expert"},
                 {"default", "general_expert"}   // any label the classifier invents
             }}},
            {{"from", "math_expert"}, {"to", "__end__"}},
            {{"from", "translate_expert"}, {"to", "__end__"}},
            {{"from", "general_expert"}, {"to", "__end__"}}
        })}
    };
    auto engine = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx});

    for (const char* question : {"What is 42 plus 58?", "translate Hello to French", "What should I do today?"}) {
        std::cout << "=== Q: " << question << " ===\n";
        neograph::graph::RunConfig config;
        config.input = {{"messages", json::array({{{"role", "user"}, {"content", question}}})}};

        // Streamed text arrives per node: the classifier's label first, then the expert's answer.
        std::string speaker;
        auto result = engine->run_stream(config, [&](const neograph::graph::GraphEvent& event) {
            if (event.type != neograph::graph::GraphEvent::Type::LLM_TOKEN) return;
            if (event.node_name != speaker)
                std::cout << (speaker.empty() ? "" : "\n") << "[" << (speaker = event.node_name) << "] ";
            std::cout << event.data.get<std::string>();
        });

        std::cout << "\nTrace: ";
        for (const auto& node : result.execution_trace) std::cout << node << " → ";
        std::cout << "END\n\n";
    }
    return 0;
}
