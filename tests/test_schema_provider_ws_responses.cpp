// Tests for SchemaProvider's use_websocket dispatch path.
//
// Two layers of coverage:
//   1. Misconfiguration guard — runs everywhere. Setting use_websocket
//      with a non-Responses schema (e.g. plain "openai") throws on
//      complete_stream rather than silently making an HTTP request.
//      Catches refactor regressions to the dispatch branch in
//      complete_stream.
//
//   2. Live OpenAI round-trip — gated on `NEOGRAPH_LIVE_OPENAI=1`
//      and `OPENAI_API_KEY`. Connects to wss://api.openai.com and
//      drives one response.create → response.completed cycle, asserts
//      content non-empty and usage tokens > 0. This is the only test
//      that actually exercises the OpenAI WS endpoint; CI runs it
//      manually, never on the offline default ctest.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>
#include <neograph/async/run_sync.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <thread>

#include <cstdlib>
#include <string>

using namespace neograph;

TEST(SchemaProviderWs, ThrowsForNonResponsesSchema) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path   = "openai";       // chat-completions, NOT responses
    cfg.api_key       = "sk-fake-key";  // unused, never reaches the wire
    cfg.default_model = "gpt-4o-mini";
    cfg.use_websocket = true;

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.messages.push_back({"user", "hi"});

    EXPECT_THROW(
        provider->complete_stream(params, [](const std::string&){}),
        std::runtime_error);
}

TEST(SchemaProviderWs, LiveRoundTripIfEnabled) {
    if (const char* gate = std::getenv("NEOGRAPH_LIVE_OPENAI");
        !gate || std::string(gate) != "1") {
        GTEST_SKIP() << "NEOGRAPH_LIVE_OPENAI != 1, skipping live OpenAI test";
    }
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        GTEST_SKIP() << "OPENAI_API_KEY not set, skipping live OpenAI test";
    }

    llm::SchemaProvider::Config cfg;
    cfg.schema_path   = "openai_responses";
    cfg.api_key       = key;
    cfg.default_model = "gpt-4o-mini";
    cfg.use_websocket = true;
    cfg.timeout_seconds = 60;

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.messages.push_back(
        {"user", "Reply with the single word: pong."});
    params.max_tokens = 16;
    // No temperature override — the provider must strip it internally
    // for the WS path. Default CompletionParams.temperature=0.7f used
    // to make OpenAI close the socket with code=1000 + zero events.

    std::string streamed;
    auto completion = provider->complete_stream(
        params, [&](const std::string& tok) { streamed += tok; });

    EXPECT_FALSE(completion.message.content.empty())
        << "expected non-empty assistant content";
    EXPECT_EQ(completion.message.content, streamed)
        << "streamed tokens should reassemble into the final content";
    EXPECT_GT(completion.usage.prompt_tokens, 0)
        << "response.completed should populate prompt_tokens";
    EXPECT_GT(completion.usage.completion_tokens, 0)
        << "response.completed should populate completion_tokens";
    EXPECT_EQ(completion.stop_reason, "end_turn")
        << "response.completed should normalize to end_turn";
}

TEST(SchemaProviderWs, LiveToolCallIfEnabled) {
    // Verifies that the WS event dispatcher correctly assembles
    // tool_calls from response.output_item.added (block_start) +
    // response.function_call_arguments.delta (tool_args_delta) +
    // response.output_item.done (block_stop). Forces the model to
    // emit a function_call by combining a system prompt with a tool
    // whose description claims it's required for arithmetic.
    if (const char* gate = std::getenv("NEOGRAPH_LIVE_OPENAI");
        !gate || std::string(gate) != "1") {
        GTEST_SKIP() << "NEOGRAPH_LIVE_OPENAI != 1, skipping live OpenAI test";
    }
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        GTEST_SKIP() << "OPENAI_API_KEY not set, skipping live OpenAI test";
    }

    llm::SchemaProvider::Config cfg;
    cfg.schema_path     = "openai_responses";
    cfg.api_key         = key;
    cfg.default_model   = "gpt-4o-mini";
    cfg.use_websocket   = true;
    cfg.timeout_seconds = 60;
    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.tools.push_back({
        "calculator",
        "Evaluate a mathematical expression. Required for any arithmetic.",
        json{
            {"type", "object"},
            {"properties", {{"expression", {{"type", "string"}}}}},
            {"required", json::array({"expression"})}
        }
    });
    params.messages.push_back(
        {"system",
         "You MUST use the calculator tool for ANY arithmetic — "
         "never compute results yourself."});
    params.messages.push_back(
        {"user", "What is 1234567 multiplied by 89?"});

    auto completion = provider->complete_stream(
        params, [](const std::string&){});

    ASSERT_FALSE(completion.message.tool_calls.empty())
        << "expected the model to emit a function_call";
    const auto& tc = completion.message.tool_calls.front();
    EXPECT_EQ(tc.name, "calculator");
    EXPECT_FALSE(tc.id.empty()) << "call_id should be lifted from output_item.added";
    EXPECT_FALSE(tc.arguments.empty())
        << "args should be assembled from function_call_arguments.delta events";
    // Args should be valid JSON containing an "expression" field.
    auto args = json::parse(tc.arguments);
    EXPECT_TRUE(args.contains("expression"));
    EXPECT_GT(completion.usage.completion_tokens, 0);
}

TEST(SchemaProviderWs, LoopbackTerminalArtifactsArePreservedOnce) {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io,
        {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&](const asio::error_code& error) {
        if (!error) {
            asio::error_code ignored;
            acceptor.close(ignored);
            socket.close(ignored);
        }
    });
    auto serve = [&]() -> asio::awaitable<void> {
        try {
            co_await acceptor.async_accept(socket, asio::use_awaitable);
            std::string buffer;
            char bytes[4096];
            while (buffer.find("\r\n\r\n") == std::string::npos) {
                const auto count = co_await socket.async_read_some(
                    asio::buffer(bytes), asio::use_awaitable);
                buffer.append(bytes, count);
            }
            const auto key_start = buffer.find("Sec-WebSocket-Key: ") + 19;
            const auto key_end = buffer.find("\r\n", key_start);
            const auto accept = async::detail::compute_sec_websocket_accept(
                buffer.substr(key_start, key_end - key_start));
            const std::string handshake =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(handshake), asio::use_awaitable);
            buffer.erase(0, buffer.find("\r\n\r\n") + 4);
            bool sent = false;
            for (;;) {
                auto frame = async::detail::parse_frame_header(buffer);
                if (!frame || buffer.size() < frame->header_size + frame->payload_len) {
                    const auto count = co_await socket.async_read_some(
                        asio::buffer(bytes), asio::use_awaitable);
                    buffer.append(bytes, count);
                    continue;
                }
                const bool close = frame->opcode == async::WsOpcode::Close;
                buffer.erase(0, frame->header_size + frame->payload_len);
                std::string output;
                if (close) {
                    async::detail::encode_frame_header(output, async::WsOpcode::Close,
                        true, false, 0);
                } else if (!sent) {
                    sent = true;
                    for (const auto& event : {
                        R"({"type":"response.output_item.added","item":{"type":"message","id":"msg-1"}})",
                        R"({"type":"response.output_text.delta","delta":"ready"})",
                        R"({"type":"response.output_item.done","item":{"type":"image_generation_call","id":"img-ws","result":"UE5H"}})",
                        R"({"type":"response.completed","response":{"status":"completed","output":[{"type":"image_generation_call","id":"img-ws","result":"UE5H"}],"usage":{"input_tokens":3,"output_tokens":4,"total_tokens":7}}})"}) {
                        const std::string payload(event);
                        async::detail::encode_frame_header(output, async::WsOpcode::Text,
                            true, false, payload.size());
                        output += payload;
                    }
                }
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
                if (close) break;
            }
        } catch (const std::exception&) {
            // The watchdog closes stalled requests, bounding failing regressions.
        }
        watchdog.cancel();
    };
    const auto port = acceptor.local_endpoint().port();
    asio::co_spawn(io, serve(), asio::detached);
    std::thread worker([&] { io.run(); });
    struct Stop {
        asio::io_context& io;
        std::thread& worker;
        ~Stop() { io.stop(); worker.join(); }
    } stop{io, worker};
    llm::SchemaProvider::Config config;
    config.schema_path = "openai_responses";
    config.api_key = "test-key";
    config.base_url_override = "http://127.0.0.1:" + std::to_string(port);
    config.allow_insecure_loopback = true;
    config.use_websocket = true;
    config.timeout_seconds = 2;
    auto provider = llm::SchemaProvider::create(config);
    CompletionParams params;
    params.messages.push_back({"user", "Draw"});
    std::string chunks;
    const auto result = async::run_sync(provider->invoke(params,
        [&](const std::string& chunk) { chunks += chunk; }));
    EXPECT_EQ(chunks, "ready");
    EXPECT_EQ(result.message.content, chunks);
    EXPECT_EQ(result.usage.total_tokens, 7);
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].base64_data, "UE5H");
    EXPECT_EQ(result.artifacts[0].metadata, "img-ws");
}
