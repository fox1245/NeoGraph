// NeoGraph Example 01: Simple ReAct Agent
//
// Implements ReAct (Yao et al. 2022, "ReAct: Synergizing Reasoning and
// Acting in Language Models", arXiv:2210.03629). ReAct's defining
// feature is the *interleaved reasoning trace*: at every step the agent
// emits a Thought before deciding on an Action, and the Observation
// from the previous tool call feeds back into the next Thought.
//
// Runs offline by default: a scripted provider (scripted_provider.h) plays the
// model, so the loop, the tool call and the streamed text are the engine's real
// code paths and no API key is needed. The calculator tool is real: the final
// answer is built from its output, so "Final answer: 427" can only appear if the
// tool computed 427.
//
// Usage:
//   ./example_react_agent           scripted model, no key, no network
//   ./example_react_agent --live    real model through OpenRouter
//                                   (needs OPENROUTER_API_KEY in the environment; billed)
//
// smoke-expect: provider: scripted
// smoke-expect: Thought: I need 15 * 28 first
// smoke-expect: [tool] calculator({"expression":"15 * 28 + 7"})
// smoke-expect: Final answer: 427

#include <neograph/neograph.h>
#include "scripted_provider.h"
#include <neograph/llm/agent.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

// Tiny recursive-descent evaluator for + - * / and parentheses.
// Leading unary minus is supported. Whitespace is skipped.
struct ExprParser {
    const char* p;

    void skip() { while (*p == ' ' || *p == '\t') ++p; }

    double number() {
        skip();
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) throw std::runtime_error("expected number");
        p = end;
        return v;
    }

    double factor() {
        skip();
        if (*p == '(') { ++p; double v = expr(); skip();
            if (*p != ')') throw std::runtime_error("missing ')'"); ++p; return v; }
        if (*p == '-') { ++p; return -factor(); }
        if (*p == '+') { ++p; return  factor(); }
        return number();
    }

    double term() {
        double v = factor();
        while (true) {
            skip();
            if (*p == '*')      { ++p; v *= factor(); }
            else if (*p == '/') { ++p; double d = factor();
                                  if (d == 0) throw std::runtime_error("division by zero");
                                  v /= d; }
            else break;
        }
        return v;
    }

    double expr() {
        double v = term();
        while (true) {
            skip();
            if (*p == '+')      { ++p; v += term(); }
            else if (*p == '-') { ++p; v -= term(); }
            else break;
        }
        return v;
    }

    double parse() {
        double v = expr();
        skip();
        if (*p != '\0') throw std::runtime_error(std::string("unexpected '") + *p + "'");
        return v;
    }
};

// Render as an integer when the value is exactly representable as one.
std::string format_number(double v) {
    std::ostringstream os;
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15)
        os << static_cast<long long>(v);
    else
        os << v;
    return os.str();
}

} // namespace

// A calculator tool with a real + - * / expression evaluator.
class CalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {
            "calculator",
            "Evaluate an arithmetic expression with + - * / and parentheses. "
            "Input: {\"expression\": \"15 * 28 + 7\"}",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"expression", {{"type", "string"}, {"description", "Math expression to evaluate"}}}
                }},
                {"required", neograph::json::array({"expression"})}
            }
        };
    }

    std::string execute(const neograph::json& args) override {
        auto expression = args.value("expression", "");
        try {
            ExprParser parser{expression.c_str()};
            double result = parser.parse();
            return neograph::json{
                {"result", format_number(result)},
                {"expression", expression}
            }.dump();
        } catch (const std::exception& e) {
            return neograph::json{
                {"error", e.what()},
                {"expression", expression}
            }.dump();
        }
    }

    std::string get_name() const override { return "calculator"; }
};

int main(int argc, char** argv) {
    try {
        // 1. Provider. The script is what the "model" says: ask the calculator first, then
        //    answer from the tool result (turn.last_tool_result()).
        auto scripted = std::make_shared<examples::ScriptedProvider>(
            [](const examples::ScriptedTurn& turn) {
                if (!turn.has_tool_result())
                    return examples::call_tool("calculator", {{"expression", "15 * 28 + 7"}})
                        .with_text("Thought: I need 15 * 28 first, then add 7. I will use the calculator.");
                const auto observation = neograph::json::parse(turn.last_tool_result());
                return examples::say("Final answer: " + observation.value("result", std::string("?")));
            });
        auto provider = examples::choose_provider(argc, argv, scripted);

        // 2. Create tools
        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<CalculatorTool>());

        // 3. Create agent.
        //
        // The system prompt elicits ReAct's Thought line before every
        // tool call. Deliberately NO example trajectory: textual
        // renderings of an action can be copied into content instead of
        // the tool_calls channel. Keep the wire contract explicit.
        const std::string react_system =
            "You are a ReAct agent. Before every tool call, first write one\n"
            "plain-text line:\n"
            "  Thought: <one sentence — what you need next and why>\n"
            "Then invoke the tool through the function-calling interface (never\n"
            "write the call as text). After a tool returns, its result is your\n"
            "Observation; use it to form the next Thought. When you have enough\n"
            "to answer, reply with 'Final answer: <answer>' as plain text. The\n"
            "Thought lines are load-bearing — do not skip them, even when the\n"
            "next step is obvious.";

        neograph::llm::Agent agent(
            std::move(provider),
            std::move(tools),
            react_system,
            examples::openrouter_model
        );
        agent.set_tool_detection_timeout_seconds(180);

        // 4. Run
        std::vector<sp::Message> messages;
        messages.push_back(examples::message(sp::Role::User, "What is 15 * 28 + 7?"));

        std::cout << "User: What is 15 * 28 + 7?\n";
        std::cout << "Assistant:\n" << std::flush;

        auto response = agent.run_stream(messages, [](const sp::Event& event) {
            if (const auto* delta = std::get_if<sp::PartDelta>(&event);
                delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content) {
                std::cout << delta->payload.bytes << std::flush;
            } else if (std::holds_alternative<sp::MessageSeal>(event)) {
                std::cout << "\n";          // one line per assistant message (Thought, then answer)
            }
        });
        auto outcome = examples::require_outcome(std::move(response));

        // The tool really ran: show each call and what the calculator returned.
        for (const auto& message : messages)
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<sp::ToolCall>(&part))
                    std::cout << "[tool] " << call->name << "("
                              << (call->input ? call->input->root().dump() : "{}") << ")\n";
                else if (const auto* result = std::get_if<sp::ToolResult>(&part))
                    std::cout << "[tool result] " << result->content << "\n";
            }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
