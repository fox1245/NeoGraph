// NeoGraph Example 40: Async-streaming ReAct agent
//
// Runs a bounded calculator ReAct graph from an outer asio::io_context.
// The provider dispatches an admitted typed HTTP/SSE request; semantic
// events are observed on the awaiting executor. The graph retains full
// outcomes and native conversation state while stdout shows text deltas.
//
// Two-node graph:
//   __start__ → llm → (tool_calls?) → tools → llm → ... → __end__
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_react_async_streaming

#include <neograph/neograph.h>
#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Tiny recursive-descent + - * / parser. Same shape as example 01;
// extracted here so the example stays self-contained.
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

std::string format_number(double v) {
    std::ostringstream os;
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15)
        os << static_cast<long long>(v);
    else
        os << v;
    return os.str();
}

} // namespace

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
                    {"expression", {{"type", "string"},
                                    {"description", "Math expression to evaluate"}}}
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
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        std::shared_ptr<neograph::Provider> provider =
            examples::make_openrouter_provider(api_key, "responses", std::chrono::seconds(30));

        std::vector<std::unique_ptr<neograph::Tool>> owned_tools;
        owned_tools.push_back(std::make_unique<CalculatorTool>());
        // Standard 2-node ReAct loop: llm → (has_tool_calls?) → tools → llm.
        neograph::json definition = {
            {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
            {"name", "react_async_streaming"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {
                {"llm",   {{"type", "llm_call"}}},
                {"tools", {{"type", "tool_dispatch"}}}
            }},
            {"edges", neograph::json::array({
                {{"from", "__start__"}, {"to", "llm"}},
                {{"from", "llm"}, {"condition", "has_tool_calls"},
                 {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
                {{"from", "tools"}, {"to", "llm"}}
            })}
        };

        neograph::graph::NodeContext ctx;
        ctx.provider = provider;
        ctx.model = examples::openrouter_model;
        ctx.instructions =
            "You are a ReAct agent. You MUST use the calculator tool for "
            "every arithmetic operation, even if the answer seems obvious "
            "to you. Do NOT compute anything in your head. Before each "
            "tool call, emit one short 'Thought:' line in plain text. "
            "After the tool returns, emit the final answer as plain text "
            "(no tool call).";

        auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
        neograph::graph::EngineResources resources;
        resources.tools = neograph::ToolSet(std::move(owned_tools));
        auto engine     = neograph::graph::GraphEngine::build(
            definition,
            neograph::graph::EngineConfig{.node_context = ctx, .checkpoint_store = store},
            std::move(resources));

        const std::string question = "What is 15 * 28 + 7?";

        std::cout << "User: " << question << "\n";
        std::cout << "Assistant: " << std::flush;

        // Typed provider events are observed on the awaiting executor.
        // The outer run keeps the callback and all captured state alive.
        int token_count = 0;
        auto event_cb = [](const neograph::graph::GraphEvent& ev) {
            using T = neograph::graph::GraphEvent::Type;
            if (ev.type == T::NODE_START && ev.node_name == "tools") {
                std::cout << "\n[tool] " << std::flush;
            } else if (ev.type == T::NODE_END && ev.node_name == "tools") {
                std::cout << "\nAssistant: " << std::flush;
            }
        };

        neograph::graph::RunConfig config;
        config.thread_id = "react-async-001";
        config.provider_messages = std::vector<sp::Message>{examples::message(sp::Role::User, question)};
        config.on_provider_event = [&token_count](const sp::Event& event) {
            if (const auto* delta = std::get_if<sp::PartDelta>(&event);
                delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content) {
                std::cout << delta->payload.bytes << std::flush;
                ++token_count;
            }
        };

        auto start = std::chrono::steady_clock::now();

        // Keep engine, config, callback and result alive through io.run().
        asio::io_context io;
        neograph::graph::RunResult result;
        std::exception_ptr caught;
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                try {
                    result = co_await engine->run_stream_async(config, event_cb);
                } catch (...) {
                    caught = std::current_exception();
                }
            },
            asio::detached);
        io.run();

        if (caught) std::rethrow_exception(caught);

        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        std::cout << "\n\n";
        std::cout << "Execution trace: ";
        for (size_t i = 0; i < result.execution_trace.size(); ++i) {
            std::cout << result.execution_trace[i];
            if (i + 1 < result.execution_trace.size()) std::cout << " → ";
        }
        std::cout << " → __end__\n";
        std::cout << "Total elapsed: " << total_ms << "ms";
        std::cout << "  (typed text-delta events: " << token_count << ")\n";

        // Final assistant message — useful when the agent skipped the
        // tool path and the streamed text is the entire reply.
        for (auto it = result.native_messages.rbegin(); it != result.native_messages.rend(); ++it) {
            if (it->role != sp::Role::Assistant) continue;
            std::string text;
            for (const auto& part : it->parts)
                if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
            if (text.empty()) continue;
            std::cout << "\nFinal assistant message:\n  " << text << "\n";
            break;
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
