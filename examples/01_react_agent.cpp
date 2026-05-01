// NeoGraph Example 01: Simple ReAct Agent
//
// Implements ReAct (Yao et al. 2022, "ReAct: Synergizing Reasoning and
// Acting in Language Models", arXiv:2210.03629). ReAct's defining
// feature is the *interleaved reasoning trace*: at every step the agent
// emits a Thought before deciding on an Action, and the Observation
// from the previous tool call feeds back into the next Thought.
//
// To make this a faithful ReAct demonstration (and not just OpenAI
// function calling) the system prompt below explicitly elicits a
// "Thought:" line before every tool call, plus one few-shot exemplar
// showing the Thought / Action / Observation pattern. Without these,
// the model can (and does) skip the reasoning trace and call the tool
// silently — that's the OpenAI tool-use baseline ReAct is meant to
// improve on.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_react_agent
// (auto-loads .env from the cwd or any parent directory; OPENAI_API_KEY
// from the process environment takes precedence if already set.)

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

#include <cppdotenv/dotenv.hpp>

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

int main() {
    cppdotenv::auto_load_dotenv();

    try {
        // 1. Create provider
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        neograph::llm::OpenAIProvider::Config config;
        config.api_key = api_key;
        config.default_model = "gpt-4o-mini";
        auto provider = neograph::llm::OpenAIProvider::create(config);

        // 2. Create tools
        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<CalculatorTool>());

        // 3. Create agent.
        //
        // The system prompt forces ReAct's interleaved Thought/Action/
        // Observation pattern. The one-shot exemplar shows the model
        // exactly what to emit before each tool call — without it,
        // strong instruction-tuned models often jump straight to a
        // silent tool call (which is OpenAI function calling, not
        // ReAct).
        const std::string react_system =
            "You are a ReAct agent. For every step, follow this format:\n"
            "  Thought: <one sentence — what you need next and why>\n"
            "  Action: <call a tool, OR write 'finish' to give the final answer>\n"
            "After a tool returns, the result is the Observation. Use it to\n"
            "form the next Thought. Stop only when you have enough to answer.\n"
            "\n"
            "Example trajectory:\n"
            "  User: What is 12 * 7 plus 5?\n"
            "  Thought: I need 12 * 7 first, then add 5. I'll compute the multiplication.\n"
            "  Action: calculator({\"expression\": \"12 * 7\"})\n"
            "  Observation: {\"result\": \"84\"}\n"
            "  Thought: Now add 5 to 84.\n"
            "  Action: calculator({\"expression\": \"84 + 5\"})\n"
            "  Observation: {\"result\": \"89\"}\n"
            "  Thought: I have the answer.\n"
            "  Final answer: 89\n"
            "\n"
            "Always emit the Thought line in plain text before any tool call,\n"
            "even when the next step is obvious. The reasoning trace is\n"
            "load-bearing — do not skip it.";

        neograph::llm::Agent agent(
            std::move(provider),
            std::move(tools),
            react_system
        );

        // 4. Run
        std::vector<neograph::ChatMessage> messages;
        messages.push_back({"user", "What is 15 * 28 + 7?"});

        std::cout << "User: What is 15 * 28 + 7?\n";
        std::cout << "Assistant: " << std::flush;

        auto response = agent.run_stream(messages,
            [](const std::string& token) { std::cout << token << std::flush; });

        std::cout << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
