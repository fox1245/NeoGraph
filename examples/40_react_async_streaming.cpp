// NeoGraph Example 40: Async-streaming ReAct agent
//
// Drives a real ReAct loop (LLM + tool dispatch + loop until done)
// from inside an outer `asio::io_context` + `co_spawn`, with the LLM
// node streaming tokens to stdout in real time via
// `co_await provider->complete_stream_async(...)`. The streamed
// tokens are bridged onto the engine's GraphEvent stream as
// `LLM_TOKEN` events, which the main thread prints character-by-
// character.
//
// ## Why this example exists
//
// This is the **exact shape** that segfaulted in issue #4 pre-v0.7:
//
//     asio::io_context io;
//     asio::co_spawn(io, [&]() -> asio::awaitable<void> {
//         result = co_await engine->run_stream_async(config, cb);
//     }, asio::detached);
//     io.run();
//
// against a `SchemaProvider("openai_responses")` with sync
// `complete_stream` httplib + async `complete_async`. Pre-PR-#10 the
// default `Provider::complete_stream_async` bridge ran the sync
// `complete_stream` inline on the engine's io_context worker thread,
// and for the WebSocket Responses branch it ALSO nested a fresh
// `run_sync` io_context on top of that worker — both modes racing
// on shared SchemaProvider state (`schema_mutex_`, `ConnPool`) and
// segfaulting after the first few tokens.
//
// PR #10 closed it: the base default now spawns a dedicated worker
// thread for the sync `complete_stream` and dispatches tokens back
// onto the awaiter's executor (so `on_chunk` runs single-threaded
// with the awaiting coroutine — no reentrancy into the engine
// worker), and `SchemaProvider` adds a native override that skips
// even the worker thread for the WS path. Verified end-to-end by
// `tests/test_schema_provider_stream_async_outer_io.cpp` against
// a local httplib SSE mock + Korean unicode payload + 6 concurrent
// outer coroutines + Valgrind clean. This example is the
// **runnable** counterpart — point it at a real OpenAI key and
// watch tokens stream out.
//
// ## What it does
//
// Two-node ReAct graph:
//
//     __start__ → llm → (tool_calls?) → tools → llm → ... → __end__
//
// LLM emits a "Thought: ..." + a calculator tool call. Tool node
// runs the calculator. LLM emits a final "The answer is N." Each
// LLM call streams tokens via the post-#10 path.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_react_async_streaming
//
// Compare with example 01 (sync `agent.run_stream`) — same agent
// behaviour, but the run loop is async-native and the streaming
// path is the post-#10 worker-thread bridge instead of inline
// blocking.

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

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
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        // SchemaProvider("openai_responses") is the exact transport
        // that segfaulted in issue #4. The HTTP/SSE path lands on
        // Provider::complete_stream_async's post-#10 default (worker
        // thread + dispatch back onto awaiter executor). Flip
        // use_websocket=true to drive the openai-responses WS path,
        // which uses SchemaProvider's native complete_stream_async
        // override (skips even the worker thread; pure co_await).
        neograph::llm::SchemaProvider::Config provider_cfg;
        provider_cfg.schema_path     = "openai_responses";
        provider_cfg.api_key         = api_key;
        provider_cfg.default_model   = "gpt-4o-mini";
        provider_cfg.timeout_seconds = 30;
        std::shared_ptr<neograph::Provider> provider =
            neograph::llm::SchemaProvider::create(provider_cfg);

        std::vector<std::unique_ptr<neograph::Tool>> owned_tools;
        owned_tools.push_back(std::make_unique<CalculatorTool>());
        std::vector<neograph::Tool*> tool_ptrs;
        for (auto& t : owned_tools) tool_ptrs.push_back(t.get());

        // Standard 2-node ReAct loop: llm → (has_tool_calls?) → tools → llm.
        neograph::json definition = {
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
        ctx.tools    = tool_ptrs;
        ctx.instructions =
            "You are a ReAct agent. You MUST use the calculator tool for "
            "every arithmetic operation, even if the answer seems obvious "
            "to you. Do NOT compute anything in your head. Before each "
            "tool call, emit one short 'Thought:' line in plain text. "
            "After the tool returns, emit the final answer as plain text "
            "(no tool call).";

        auto store  = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
        auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);
        engine->own_tools(std::move(owned_tools));

        const std::string question = "What is 15 * 28 + 7?";

        std::cout << "User: " << question << "\n";
        std::cout << "Assistant: " << std::flush;

        // Engine stream callback. The interesting branch is LLM_TOKEN:
        // each token has been dispatched onto the awaiter's executor
        // by Provider::complete_stream_async (post-#10), so writing
        // to stdout here is safe and single-threaded with the
        // engine's run coroutine.
        int token_count = 0;
        auto event_cb = [&token_count](const neograph::graph::GraphEvent& ev) {
            using T = neograph::graph::GraphEvent::Type;
            if (ev.type == T::LLM_TOKEN && ev.data.is_string()) {
                std::cout << ev.data.get<std::string>() << std::flush;
                ++token_count;
            } else if (ev.type == T::NODE_START && ev.node_name == "tools") {
                std::cout << "\n[tool] " << std::flush;
            } else if (ev.type == T::NODE_END && ev.node_name == "tools") {
                std::cout << "\nAssistant: " << std::flush;
            }
        };

        neograph::graph::RunConfig config;
        config.thread_id = "react-async-001";
        config.input = {{"messages", neograph::json::array({
            {{"role", "user"}, {"content", question}}
        })}};

        auto start = std::chrono::steady_clock::now();

        // === The shape that used to segfault. Now it works. ===
        //
        // Outer `asio::io_context` + `co_spawn` + `co_await
        // engine->run_stream_async()`. Inside, LLMCallNode does
        // `co_await provider_->complete_stream_async(params, on_token)`,
        // and on_token is the per-token cb that bridges to the
        // engine's GraphEvent stream as LLM_TOKEN — which the
        // event_cb above prints to stdout.
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
        std::cout << "  (LLM_TOKEN events: " << token_count << ")\n";

        // Final assistant message — useful when the agent skipped the
        // tool path and the streamed text is the entire reply.
        if (result.output.contains("channels")
            && result.output["channels"].contains("messages")) {
            const auto& msgs = result.output["channels"]["messages"]["value"];
            if (msgs.is_array() && !msgs.empty()) {
                std::string last_assistant_content;
                for (const auto& m : msgs) {
                    if (m.value("role", "") == "assistant"
                        && m.contains("content")
                        && m["content"].is_string()) {
                        auto c = m["content"].get<std::string>();
                        if (!c.empty()) last_assistant_content = c;
                    }
                }
                if (!last_assistant_content.empty()) {
                    std::cout << "\nFinal assistant message:\n  "
                              << last_assistant_content << "\n";
                }
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
