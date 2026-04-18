// NeoGraph Example 01: Simple ReAct Agent
//
// A minimal example that creates a ReAct agent with a calculator tool.
// The agent calls the LLM, detects tool calls, executes them, and loops.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_react_agent

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
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

int main() {
    // 1. Create provider
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Set OPENAI_API_KEY environment variable\n";
        return 1;
    }

    neograph::llm::OpenAIProvider::Config config;
    config.api_key = api_key;
    config.default_model = "gpt-4o-mini";
    auto provider = neograph::llm::OpenAIProvider::create(config);

    // 2. Create tools
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<CalculatorTool>());

    // 3. Create agent
    neograph::llm::Agent agent(
        std::move(provider),
        std::move(tools),
        "You are a helpful assistant with a calculator tool."
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
}
