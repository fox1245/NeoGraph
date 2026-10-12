#include <gtest/gtest.h>

#include <neograph/mcp/server.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>
#include <neograph/provider.h>

#include <asio/error.hpp>
#include <asio/system_error.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;

namespace {

using neograph::json;
using neograph::mcp::CallToolResult;
using neograph::mcp::MCPServer;
using neograph::mcp::MCPServerConfig;
using neograph::mcp::ToolDefinition;

json request(json id, std::string method, json params = json::object()) {
    return {
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"method", std::move(method)},
        {"params", std::move(params)},
    };
}

json notification(std::string method, json params = json::object()) {
    return {
        {"jsonrpc", "2.0"},
        {"method", std::move(method)},
        {"params", std::move(params)},
    };
}

MCPServerConfig config(std::size_t concurrent = 2,
                       std::size_t pending = 4) {
    MCPServerConfig value;
    value.server_info = {{"name", "test-server"}, {"version", "1.2.3"}};
    value.instructions = "Use test tools carefully.";
    value.max_concurrent_calls = concurrent;
    value.max_pending_calls = pending;
    return value;
}

void initialize(MCPServer& server) {
    auto response = server.handle_message(request(1, "initialize", {
        {"protocolVersion", "2025-11-25"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "test-client"}, {"version", "1.0"}}},
    }));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(server.handle_message(
        notification("notifications/initialized")).is_null());
    ASSERT_TRUE(server.initialized());
}

ToolDefinition echo_definition() {
    ToolDefinition definition;
    definition.name = "echo";
    definition.title = "Echo title";
    definition.description = "Echo a value";
    definition.icons = json::array({{{"src", "data:image/png;base64,AA=="},
                                     {"mimeType", "image/png"}}});
    definition.input_schema = {
        {"type", "object"},
        {"required", json::array({"value"})},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    definition.output_schema = {
        {"type", "object"},
        {"required", json::array({"echoed"})},
        {"properties", {{"echoed", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    definition.annotations = {{"readOnlyHint", true}};
    definition.execution = {{"taskSupport", "forbidden"}};
    definition.meta = {{"catalog", "test"}};
    return definition;
}

CallToolResult echo_result(const json& arguments) {
    const auto value = arguments.at("value").get<std::string>();
    CallToolResult result;
    result.content = json::array({{{"type", "text"}, {"text", value}}});
    result.structured_content = {{"echoed", value}};
    result.meta = {{"source", "unit"}};
    return result;
}

struct CapturingSink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<json> responses;

    MCPServer::ResponseSink sink() {
        return [this](const json& response) {
            std::lock_guard lock(mutex);
            responses.push_back(response);
            cv.notify_all();
        };
    }

    json wait_for(const json& id, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, timeout, [&] {
            for (const auto& response : responses) {
                if (response.value("id", json(nullptr)) == id) return true;
            }
            return false;
        });
        for (const auto& response : responses) {
            if (response.value("id", json(nullptr)) == id) return response;
        }
        return nullptr;
    }
};

TEST(MCPServerTest, RequiresInitializedNotificationBeforeTools) {
    MCPServer server(config());

    auto before = server.handle_message(request(7, "tools/list"));
    ASSERT_TRUE(before.contains("error"));
    EXPECT_EQ(before["error"]["code"], -32002);

    auto init = server.handle_message(request(8, "initialize", {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "client"}, {"version", "1"}}},
    }));
    ASSERT_TRUE(init.contains("result"));
    EXPECT_EQ(init["result"]["protocolVersion"], "2025-11-25");
    EXPECT_EQ(init["result"]["serverInfo"]["name"], "test-server");
    EXPECT_EQ(init["result"]["instructions"], "Use test tools carefully.");
    EXPECT_FALSE(server.initialized());

    auto between = server.handle_message(request(9, "tools/list"));
    EXPECT_EQ(between["error"]["code"], -32002);

    server.handle_message(notification("notifications/initialized"));
    EXPECT_TRUE(server.initialized());
    EXPECT_TRUE(server.handle_message(request(10, "tools/list")).contains("result"));

    auto repeated = server.handle_message(request(11, "initialize", {
        {"protocolVersion", "2025-11-25"},
        {"capabilities", json::object()},
        {"clientInfo", json::object()},
    }));
    EXPECT_EQ(repeated["error"]["code"], -32600);
}

TEST(MCPServerTest, ListsCompleteTypedToolMetadata) {
    MCPServer server(config());
    server.register_tool(echo_definition(),
        [](const json& arguments, const auto&) { return echo_result(arguments); });
    initialize(server);

    auto response = server.handle_message(request("list", "tools/list"));
    ASSERT_TRUE(response.contains("result"));
    const auto& tools = response["result"]["tools"];
    ASSERT_EQ(tools.size(), 1u);
    const auto& tool = tools[0];
    EXPECT_EQ(tool["name"], "echo");
    EXPECT_EQ(tool["title"], "Echo title");
    EXPECT_EQ(tool["icons"].size(), 1u);
    EXPECT_EQ(tool["outputSchema"]["type"], "object");
    EXPECT_TRUE(tool["annotations"]["readOnlyHint"].get<bool>());
    EXPECT_EQ(tool["execution"]["taskSupport"], "forbidden");
    EXPECT_EQ(tool["_meta"]["catalog"], "test");
}

TEST(MCPServerTest, CallsToolAndPreservesStructuredResult) {
    CapturingSink captured;
    MCPServer server(config());
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [](const json& arguments, const auto&) { return echo_result(arguments); });
    initialize(server);

    auto immediate = server.handle_message(request("call-1", "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "hello"}}},
    }));
    EXPECT_TRUE(immediate.is_null());

    auto response = captured.wait_for("call-1");
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["result"]["content"][0]["text"], "hello");
    EXPECT_EQ(response["result"]["structuredContent"]["echoed"], "hello");
    EXPECT_EQ(response["result"]["_meta"]["source"], "unit");
    EXPECT_FALSE(response["result"].value("isError", false));
}

TEST(MCPServerTest, InputSchemaFailureIsToolErrorWithoutInvocation) {
    bool invoked = false;
    MCPServer server(config());
    server.register_tool(echo_definition(),
        [&invoked](const json& arguments, const auto&) {
            invoked = true;
            return echo_result(arguments);
        });
    initialize(server);

    auto response = server.handle_message(request(20, "tools/call", {
        {"name", "echo"}, {"arguments", {{"wrong", true}}},
    }));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response["result"]["isError"].get<bool>());
    EXPECT_FALSE(invoked);
}

TEST(MCPServerTest, ProtocolAndToolExecutionErrorsRemainDistinct) {
    CapturingSink captured;
    MCPServer server(config());
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [](const json&, const auto&) -> CallToolResult {
            throw std::runtime_error("handler exploded");
        });
    initialize(server);

    auto malformed = server.handle_message(request(30, "tools/call", json::object()));
    ASSERT_TRUE(malformed.contains("error"));
    EXPECT_EQ(malformed["error"]["code"], -32602);

    auto missing = server.handle_message(request(31, "tools/call", {
        {"name", "missing"}, {"arguments", json::object()},
    }));
    ASSERT_TRUE(missing.contains("result"));
    EXPECT_TRUE(missing["result"]["isError"].get<bool>());

    EXPECT_TRUE(server.handle_message(request(32, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "x"}}},
    })).is_null());
    auto thrown = captured.wait_for(32);
    ASSERT_TRUE(thrown.contains("result"));
    EXPECT_TRUE(thrown["result"]["isError"].get<bool>());
    // The public message and the stable code reach the client, never the
    // exception text.
    EXPECT_EQ(thrown["result"]["content"][0]["text"]
                  .get<std::string>().find("handler exploded"),
              std::string::npos);
    EXPECT_EQ(thrown["result"]["_meta"]["io.neograph/failure"]["code"], "NG.TaskFailed");
}

TEST(MCPServerTest, OutputSchemaFailureBecomesToolError) {
    CapturingSink captured;
    MCPServer server(config());
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [](const json&, const auto&) {
            CallToolResult result;
            result.content = json::array({{{"type", "text"}, {"text", "bad"}}});
            result.structured_content = {{"wrong", 1}};
            return result;
        });
    initialize(server);

    EXPECT_TRUE(server.handle_message(request(40, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "x"}}},
    })).is_null());
    auto response = captured.wait_for(40);
    ASSERT_TRUE(response.contains("result"));
    EXPECT_TRUE(response["result"]["isError"].get<bool>());
    EXPECT_NE(response["result"]["content"][0]["text"]
                  .get<std::string>().find("missing required property echoed"),
              std::string::npos);
}

TEST(MCPServerTest, CancellationReachesRequestScopedToken) {
    CapturingSink captured;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    MCPServer server(config(1, 1));
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [&entered](const json&, const auto& cancel) {
            entered.set_value();
            while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
            cancel->throw_if_cancelled("inside test tool");
            return CallToolResult{};
        });
    initialize(server);

    EXPECT_TRUE(server.handle_message(request("cancel-me", "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "x"}}},
    })).is_null());
    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(server.handle_message(notification("notifications/cancelled", {
        {"requestId", "cancel-me"}, {"reason", "test"},
    })).is_null());

    // A cancelled call is a JSON-RPC error with the cancellation code, never
    // a tool result (reference, "Protocol failure mapping").
    auto response = captured.wait_for("cancel-me");
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["code"], -32800);
    EXPECT_EQ(response["error"]["data"]["code"], "NG.Cancelled");
}

TEST(MCPServerTest, BoundedQueueRejectsExcessAndDuplicateRequestIds) {
    CapturingSink captured;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    MCPServer server(config(1, 0));
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [&entered, release_future](const json& arguments, const auto&) {
            entered.set_value();
            release_future.wait();
            return echo_result(arguments);
        });
    initialize(server);

    EXPECT_TRUE(server.handle_message(request(50, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "first"}}},
    })).is_null());
    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);

    auto duplicate = server.handle_message(request(50, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "duplicate"}}},
    }));
    EXPECT_EQ(duplicate["error"]["code"], -32600);

    auto excess = server.handle_message(request(51, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "second"}}},
    }));
    EXPECT_EQ(excess["error"]["code"], -32000);
    EXPECT_EQ(excess["error"]["data"]["capacity"], 1);

    release.set_value();
    EXPECT_TRUE(captured.wait_for(50).contains("result"));
}

TEST(MCPServerTest, StopCancelsRunningAndPendingCalls) {
    CapturingSink captured;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    MCPServer server(config(1, 2));
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [&entered](const json& arguments, const auto& cancel) {
            if (arguments["value"] == "running") entered.set_value();
            while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
            cancel->throw_if_cancelled("server stopping");
            return echo_result(arguments);
        });
    initialize(server);

    EXPECT_TRUE(server.handle_message(request(60, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "running"}}},
    })).is_null());
    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(server.handle_message(request(61, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "pending"}}},
    })).is_null());

    server.stop();
    // A server-side cancellation is a server error, not a tool result.
    auto running = captured.wait_for(60);
    auto pending = captured.wait_for(61);
    ASSERT_TRUE(running.contains("error")) << running.dump();
    ASSERT_TRUE(pending.contains("error")) << pending.dump();
    EXPECT_EQ(running["error"]["code"], -32000);
    EXPECT_EQ(pending["error"]["code"], -32000);
    EXPECT_EQ(running["error"]["data"]["code"], "NG.Cancelled");
}

// MCP: the receiver of notifications/cancelled SHOULD NOT answer the request.
// The stdio loop sends nothing for it (official clients report a response to
// a cancelled id as an error), while a sink-driven transport such as HTTP
// still ends the open request with -32800 (above).
TEST(MCPServerTest, StdioSendsNoResponseForAClientCancelledCall) {
    MCPServer server(config(1, 1));
    std::atomic<bool> entered{false};
    server.register_tool(echo_definition(),
        [&entered](const json&, const auto& cancel) -> CallToolResult {
            entered = true;
            while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
            cancel->throw_if_cancelled("inside test tool");
            return CallToolResult{};
        });
    std::stringstream in;
    in << request(1, "initialize", {{"protocolVersion", "2025-11-25"},
                                    {"capabilities", json::object()},
                                    {"clientInfo", {{"name", "c"}, {"version", "1"}}}}).dump()
       << "\n" << notification("notifications/initialized").dump() << "\n"
       << request("cancel-me", "tools/call",
                  {{"name", "echo"}, {"arguments", {{"value", "x"}}}}).dump()
       << "\n" << notification("notifications/cancelled", {{"requestId", "cancel-me"}}).dump()
       << "\n";
    std::stringstream out;
    server.run(in, out);
    std::string line;
    int responses = 0;
    while (std::getline(out, line)) {
        if (line.empty()) continue;
        const auto envelope = json::parse(line);
        EXPECT_NE(envelope.value("id", json(nullptr)), json("cancel-me")) << line;
        ++responses;
    }
    EXPECT_EQ(responses, 1) << "only initialize is answered";
}

// Each typed failure class a handler raises maps to a distinct, documented
// response (reference, "Protocol failure mapping"): run control signals are
// JSON-RPC errors with their own codes; every other failure is an isError
// tool result whose `_meta["io.neograph/failure"].code` names its class.
TEST(MCPServerTest, TypedFailuresMapToDistinctResponses) {
    CapturingSink captured;
    MCPServer server(config(2, 16));
    server.set_response_sink(captured.sink());
    ToolDefinition definition;
    definition.name = "fail";
    definition.description = "raises the failure it is asked for";
    definition.input_schema = {{"type", "object"},
                               {"properties", {{"kind", {{"type", "string"}}}}}};
    server.register_tool(definition, [](const json& arguments, const auto&) -> CallToolResult {
        const auto kind = arguments.at("kind").get<std::string>();
        auto provider = [](sp::ErrorKind error) {
            sp::Failure failure;
            failure.error.kind = error;
            failure.error.safe_message = "provider failed";
            return neograph::ProviderFailure(std::make_shared<const sp::Outcome>(std::move(failure)));
        };
        if (kind == "ordinary") throw std::runtime_error("plain failure");
        if (kind == "author") throw neograph::graph::NodeFailureError("app.bad_input", "bad", false);
        if (kind == "timeout") throw asio::system_error(asio::error::timed_out, "too slow");
        if (kind == "provider") throw provider(sp::ErrorKind::Overloaded);
        if (kind == "budget") throw provider(sp::ErrorKind::QuotaExhausted);
        if (kind == "hook")
            throw neograph::HookBoundaryBlocked("sha256:" + std::string(64, 'a'),
                                                neograph::HookExecutionState::Failed,
                                                "audit hook blocked");
        if (kind == "interrupt")
            throw neograph::graph::NodeInterrupt("needs approval", json{{"cmd", "deploy"}});
        if (kind == "cancelled") throw neograph::graph::CancelledException("inner run cancelled");
        return CallToolResult{};
    });
    initialize(server);
    const std::vector<std::tuple<std::string, std::string, int>> table{
        // kind, NG code, JSON-RPC error code (0: an isError tool result)
        {"ordinary", "NG.TaskFailed", 0},
        {"author", "app.bad_input", 0},
        {"timeout", "NG.Timeout", 0},
        {"provider", "NG.ProviderFailed", 0},
        {"budget", "NG.BudgetExhausted", 0},
        {"hook", "NG.HookBlocked", -32010},
        {"interrupt", "NG.Interrupted", -32011},
        // The handler's own cancellation is a server-side condition.
        {"cancelled", "NG.Cancelled", -32603},
    };
    int id = 100;
    for (const auto& [kind, code, rpc] : table) {
        SCOPED_TRACE(kind);
        ++id;
        EXPECT_TRUE(server.handle_message(request(id, "tools/call", {
            {"name", "fail"}, {"arguments", {{"kind", kind}}},
        })).is_null());
        const auto response = captured.wait_for(id);
        if (rpc == 0) {
            ASSERT_TRUE(response.contains("result")) << response.dump();
            EXPECT_TRUE(response["result"]["isError"].get<bool>());
            EXPECT_EQ(response["result"]["_meta"]["io.neograph/failure"]["code"], code);
        } else {
            ASSERT_TRUE(response.contains("error")) << response.dump();
            EXPECT_EQ(response["error"]["code"], rpc);
            EXPECT_EQ(response["error"]["data"]["code"], code);
        }
        if (kind == "hook") {
            EXPECT_EQ(response["error"]["data"]["invocation_id"], "sha256:" + std::string(64, 'a'));
            EXPECT_EQ(response["error"]["data"]["state"], "failed");
        }
        if (kind == "interrupt") {
            EXPECT_EQ(response["error"]["data"]["reason"], "needs approval");
            EXPECT_EQ(response["error"]["data"]["value"]["cmd"], "deploy");
        }
    }
}

// Ill-formed UTF-8 in a handler's failure text still produces a response line
// (it used to serialize to an empty line the client skipped until timeout).
TEST(MCPServerTest, IllFormedUtf8NeverEmptiesAResponse) {
    CapturingSink captured;
    MCPServer server(config());
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(), [](const json&, const auto&) -> CallToolResult {
        // The public message is the text a peer may read; even it crosses as
        // well-formed UTF-8.
        throw neograph::graph::NodeFailureError("app.bad_text", "bad \xff byte");
    });
    initialize(server);
    EXPECT_TRUE(server.handle_message(request(7, "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "x"}}},
    })).is_null());
    const auto response = captured.wait_for(7);
    ASSERT_TRUE(response.contains("result"));
    const auto line = response.dump();
    ASSERT_FALSE(line.empty());
    EXPECT_NE(json::parse(line)["result"]["content"][0]["text"].get<std::string>().find(
                  "bad \xEF\xBF\xBD byte"),
              std::string::npos);
}

TEST(MCPServerTest, RejectsInvalidDefinitionsAndLateRegistration) {
    MCPServer server(config());
    auto invalid = echo_definition();
    invalid.input_schema["type"] = "imaginary";
    EXPECT_THROW(server.register_tool(invalid,
        [](const json&, const auto&) { return CallToolResult{}; }),
        std::invalid_argument);

    initialize(server);
    EXPECT_THROW(server.register_tool(echo_definition(),
        [](const json&, const auto&) { return CallToolResult{}; }),
        std::logic_error);
}

TEST(MCPServerTest, StdioWritesOnlyJsonRpcFramesAndLogsToStderr) {
    MCPServer server(config());
    server.register_tool(echo_definition(),
        [](const json& arguments, const auto&) { return echo_result(arguments); });

    std::stringstream input;
    input << request(1, "initialize", {
        {"protocolVersion", "2025-11-25"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "stdio"}, {"version", "1"}}},
    }).dump() << '\n';
    input << notification("notifications/initialized").dump() << '\n';
    input << request(2, "tools/list").dump() << '\n';
    input << "{not-json\n";

    std::stringstream output;
    std::stringstream errors;
    auto* previous = std::cerr.rdbuf(errors.rdbuf());
    server.run(input, output);
    std::cerr.rdbuf(previous);

    std::string line;
    std::vector<json> frames;
    while (std::getline(output, line)) {
        ASSERT_NO_THROW(frames.push_back(json::parse(line)));
    }
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0]["id"], 1);
    EXPECT_EQ(frames[1]["id"], 2);
    EXPECT_EQ(frames[2]["error"]["code"], -32700);
    EXPECT_NE(errors.str().find("parse error"), std::string::npos);
}

} // namespace
