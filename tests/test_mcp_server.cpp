#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

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
#include <streambuf>
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
    std::vector<json> unanswered;  ///< ids that ended without a response

    MCPServer::ResponseSink sink() {
        return [this](const json& response) {
            std::lock_guard lock(mutex);
            responses.push_back(response);
            cv.notify_all();
        };
    }

    MCPServer::NoResponseSink no_response() {
        return [this](const json& id) {
            std::lock_guard lock(mutex);
            unanswered.push_back(id);
            cv.notify_all();
        };
    }

    bool wait_for_no_response(const json& id, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] {
            return std::find(unanswered.begin(), unanswered.end(), id) != unanswered.end();
        });
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
    EXPECT_EQ(init["result"]["protocolVersion"], "2024-11-05");
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

TEST(MCPServerTest, ArgumentsAreValidatedAsJsonSchema2020_12) {
    // MCP-S-03: `minimum` is enforced and 1.0 is an integer, as the official
    // SDKs' 2020-12 validators decide.
    CapturingSink captured;
    MCPServer server(config());
    server.set_response_sink(captured.sink());
    std::atomic<int> invoked{0};
    ToolDefinition definition;
    definition.name = "count";
    definition.input_schema = json::parse(R"({
        "type": "object",
        "$defs": {"N": {"type": "integer", "minimum": 0}},
        "properties": {"n": {"$ref": "#/$defs/N"},
                       "tag": {"anyOf": [{"type": "string"}, {"type": "null"}]}},
        "required": ["n"]})");
    server.register_tool(definition, [&invoked](const json&, const auto&) {
        ++invoked;
        CallToolResult result;
        result.content = json::array({{{"type", "text"}, {"text", "ok"}}});
        return result;
    });
    initialize(server);

    auto negative = server.handle_message(request(50, "tools/call", {
        {"name", "count"}, {"arguments", {{"n", -1}}}}));
    ASSERT_TRUE(negative.contains("result"));
    EXPECT_TRUE(negative["result"]["isError"].get<bool>());
    EXPECT_NE(negative["result"]["content"][0]["text"].get<std::string>().find("minimum"),
              std::string::npos);

    auto wrong_branch = server.handle_message(request(51, "tools/call", {
        {"name", "count"}, {"arguments", {{"n", 1}, {"tag", 3}}}}));
    ASSERT_TRUE(wrong_branch.contains("result"));
    EXPECT_TRUE(wrong_branch["result"]["isError"].get<bool>());
    EXPECT_EQ(invoked.load(), 0);

    EXPECT_TRUE(server.handle_message(request(52, "tools/call", {
        {"name", "count"}, {"arguments", json::parse(R"({"n": 1.0, "tag": null})")}})).is_null());
    auto accepted = captured.wait_for(52);
    ASSERT_TRUE(accepted.contains("result"));
    EXPECT_FALSE(accepted["result"].value("isError", false));
    EXPECT_EQ(invoked.load(), 1);
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

    // An unknown tool is a protocol error (MCP-S-06).
    auto missing = server.handle_message(request(31, "tools/call", {
        {"name", "missing"}, {"arguments", json::object()},
    }));
    ASSERT_TRUE(missing.contains("error")) << missing.dump();
    EXPECT_EQ(missing["id"], 31);
    EXPECT_EQ(missing["error"]["code"], -32602);
    EXPECT_EQ(missing["error"]["message"], "Unknown tool: missing");
    EXPECT_EQ(missing["error"]["data"]["code"], "NG.ToolNotFound");

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
    server.set_response_sink(captured.sink(), captured.no_response());
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

    // A call the client cancelled gets no response on any transport (MCP:
    // the receiver SHOULD NOT respond; reference, "Protocol failure
    // mapping"). The sink's owner learns that the call ended without one.
    EXPECT_TRUE(captured.wait_for_no_response("cancel-me"));
    EXPECT_TRUE(captured.wait_for("cancel-me", 200ms).is_null())
        << captured.wait_for("cancel-me").dump();
}

TEST(MCPServerTest, AResponseSinkAloneGetsNothingForAClientCancelledCall) {
    CapturingSink captured;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> finished;
    auto finished_future = finished.get_future();
    MCPServer server(config(1, 1));
    server.set_response_sink(captured.sink());
    server.register_tool(echo_definition(),
        [&](const json& arguments, const auto& cancel) {
            if (arguments["value"] == "after") return echo_result(arguments);
            entered.set_value();
            while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
            finished.set_value();
            cancel->throw_if_cancelled("inside test tool");
            return CallToolResult{};
        });
    initialize(server);
    EXPECT_TRUE(server.handle_message(request("gone", "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "x"}}},
    })).is_null());
    ASSERT_EQ(entered_future.wait_for(2s), std::future_status::ready);
    server.handle_message(notification("notifications/cancelled", {{"requestId", "gone"}}));
    ASSERT_EQ(finished_future.wait_for(2s), std::future_status::ready);
    // The worker is free again; the next call is answered, the cancelled one
    // never is.
    EXPECT_TRUE(server.handle_message(request("after", "tools/call", {
        {"name", "echo"}, {"arguments", {{"value", "after"}}},
    })).is_null());
    EXPECT_TRUE(captured.wait_for("after").contains("result"));
    EXPECT_TRUE(captured.wait_for("gone", 100ms).is_null());
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
// The stdio loop sends nothing for it either (official clients report a
// response to a cancelled id as an error).
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
            // A transient provider failure may succeed when sent again, as on
            // every protocol (graph::FailureVerdict::retryable).
            EXPECT_EQ(response["result"]["_meta"]["io.neograph/failure"]["retryable"].get<bool>(),
                      kind == "provider" || kind == "ordinary" || kind == "timeout")
                << kind;
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

TEST(MCPServerTest, ToolsWithoutASchemaAreListedWithAnObjectSchema) {
    // MCP-S-01: `inputSchema: {}` made the official clients reject the whole
    // tools/list (pydantic: "inputSchema.type Field required"; zod: expected
    // "object"). The default and an explicit empty object both list as
    // {"type": "object"}.
    MCPServer server(config());
    ToolDefinition untyped;
    untyped.name = "untyped";
    server.register_tool(untyped, [](const json&, const auto&) { return CallToolResult{}; });
    ToolDefinition empty;
    empty.name = "empty_schema";
    empty.input_schema = json::object();
    server.register_tool(empty, [](const json&, const auto&) { return CallToolResult{}; });
    initialize(server);

    auto response = server.handle_message(request("list", "tools/list"));
    const auto tools = response["result"]["tools"];
    ASSERT_EQ(tools.size(), 2u);
    for (std::size_t i = 0; i < tools.size(); ++i) {
        EXPECT_EQ(tools[i]["inputSchema"], json({{"type", "object"}})) << tools[i].dump();
    }
}

TEST(MCPServerTest, RefusesSchemasAndNamesOfficialClientsReject) {
    MCPServer server(config());
    auto with = [](std::string name, json input, json output = nullptr) {
        ToolDefinition definition;
        definition.name = std::move(name);
        definition.input_schema = std::move(input);
        definition.output_schema = std::move(output);
        return definition;
    };
    const auto handler = [](const json&, const auto&) { return CallToolResult{}; };
    EXPECT_THROW(server.register_tool(with("a", {{"type", "string"}}), handler),
                 std::invalid_argument);
    EXPECT_THROW(server.register_tool(with("b", {{"properties", json::object()}}), handler),
                 std::invalid_argument);
    EXPECT_THROW(server.register_tool(with("c", {{"type", "object"}}, {{"type", "array"}}), handler),
                 std::invalid_argument);
    EXPECT_THROW(server.register_tool(with("d", json::parse(
                     R"({"type":"object","$schema":"http://json-schema.org/draft-07/schema#"})")),
                     handler),
                 std::invalid_argument);
    // SEP-986 tool names: 1-128 of [A-Za-z0-9_.-].
    EXPECT_THROW(server.register_tool(with("has space", {{"type", "object"}}), handler),
                 std::invalid_argument);
    EXPECT_THROW(server.register_tool(with("", {{"type", "object"}}), handler),
                 std::invalid_argument);
    EXPECT_THROW(server.register_tool(with(std::string(129, 'x'), {{"type", "object"}}), handler),
                 std::invalid_argument);
    EXPECT_NO_THROW(server.register_tool(with("ok.name-1_x", {{"type", "object"}}), handler));
    EXPECT_NO_THROW(server.register_tool(with(std::string(128, 'y'), {{"type", "object"}}), handler));
}

TEST(MCPServerTest, NegotiatesTheOlderRevisionsItSupports) {
    // D2: the server answers 2025-06-18, 2025-03-26 and 2024-11-05 when a
    // client asks for them, as the official SDK servers do, with the fields
    // those revisions define, and the newest revision for any other request.
    auto serve = [](const std::string& requested, std::string& negotiated, json& tools,
                    json& structured_call, json& plain_call) {
        CapturingSink captured;
        MCPServer server(config());
        server.set_response_sink(captured.sink());
        server.register_tool(echo_definition(),
            [](const json& arguments, const auto&) { return echo_result(arguments); });
        auto only_structured = echo_definition();
        only_structured.name = "structured_only";
        server.register_tool(only_structured, [](const json& arguments, const auto&) {
            CallToolResult result;
            result.structured_content = {{"echoed", arguments.at("value")}};
            return result;
        });
        auto init = server.handle_message(request(1, "initialize", {
            {"protocolVersion", requested},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "old-client"}, {"version", "1"}}},
        }));
        negotiated = init["result"]["protocolVersion"].get<std::string>();
        EXPECT_EQ(server.protocol_version(), negotiated);
        server.handle_message(notification("notifications/initialized"));
        tools = server.handle_message(request(2, "tools/list"))["result"]["tools"];
        server.handle_message(request(3, "tools/call", {{"name", "echo"},
                                                        {"arguments", {{"value", "a"}}}}));
        server.handle_message(request(4, "tools/call", {{"name", "structured_only"},
                                                        {"arguments", {{"value", "b"}}}}));
        structured_call = captured.wait_for(3)["result"];
        plain_call = captured.wait_for(4)["result"];
    };
    std::string negotiated;
    json tools, echo, only;

    serve("2025-11-25", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2025-11-25");
    EXPECT_TRUE(tools[0].contains("icons"));
    EXPECT_TRUE(tools[0].contains("execution"));

    serve("2025-06-18", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2025-06-18");
    EXPECT_FALSE(tools[0].contains("icons"));
    EXPECT_FALSE(tools[0].contains("execution"));
    EXPECT_TRUE(tools[0].contains("title"));
    EXPECT_TRUE(tools[0].contains("outputSchema"));
    EXPECT_EQ(echo["structuredContent"]["echoed"], "a");

    serve("2025-03-26", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2025-03-26");
    for (const char* field : {"icons", "execution", "title", "outputSchema", "_meta"})
        EXPECT_FALSE(tools[0].contains(field)) << field;
    EXPECT_TRUE(tools[0].contains("annotations"));
    EXPECT_FALSE(echo.contains("structuredContent"));
    EXPECT_EQ(echo["content"][0]["text"], "a");
    // A result with only structured data keeps it as text.
    EXPECT_FALSE(only.contains("structuredContent"));
    EXPECT_EQ(json::parse(only["content"][0]["text"].get<std::string>()), json({{"echoed", "b"}}));

    serve("2024-11-05", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2024-11-05");
    // 2024-11-05 also has no tool annotations.
    for (const char* field : {"icons", "execution", "title", "outputSchema", "_meta", "annotations"})
        EXPECT_FALSE(tools[0].contains(field)) << field;
    EXPECT_EQ(tools[0]["name"], "echo");
    EXPECT_TRUE(tools[0].contains("inputSchema"));
    EXPECT_FALSE(echo.contains("structuredContent"));
    EXPECT_EQ(echo["content"][0]["text"], "a");
    EXPECT_FALSE(only.contains("structuredContent"));
    EXPECT_EQ(json::parse(only["content"][0]["text"].get<std::string>()), json({{"echoed", "b"}}));

    serve("2024-10-07", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2025-11-25");
    serve("1999-01-01", negotiated, tools, echo, only);
    EXPECT_EQ(negotiated, "2025-11-25");
}

TEST(MCPServerTest, ContentBlocksARevisionLacksAreSentAsText) {
    // A content block whose type the negotiated revision does not define
    // would fail that revision's schema: audio came with 2025-03-26 and
    // resource_link with 2025-06-18. Such a block is sent as a text block
    // holding its JSON, so nothing is lost.
    const json audio = {{"type", "audio"}, {"data", "AA=="}, {"mimeType", "audio/wav"}};
    const json link = {{"type", "resource_link"}, {"uri", "file:///report.txt"}, {"name", "report"}};
    auto call = [&](const std::string& revision) {
        CapturingSink captured;
        MCPServer server(config());
        server.set_response_sink(captured.sink());
        ToolDefinition media;
        media.name = "media";
        server.register_tool(media, [&](const json&, const auto&) {
            CallToolResult result;
            result.content = json::array({{{"type", "text"}, {"text", "first"}}, audio, link});
            return result;
        });
        server.handle_message(request(1, "initialize", {
            {"protocolVersion", revision},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "old-client"}, {"version", "1"}}},
        }));
        server.handle_message(notification("notifications/initialized"));
        server.handle_message(request(2, "tools/call", {{"name", "media"},
                                                        {"arguments", json::object()}}));
        return captured.wait_for(2)["result"]["content"];
    };
    const auto newest = call("2025-11-25");
    ASSERT_EQ(newest.size(), 3u);
    EXPECT_EQ(newest[1], audio);
    EXPECT_EQ(newest[2], link);

    const auto june = call("2025-06-18");
    EXPECT_EQ(june[1], audio);
    EXPECT_EQ(june[2], link);

    const auto march = call("2025-03-26");
    ASSERT_EQ(march.size(), 3u);
    EXPECT_EQ(march[0]["text"], "first");
    EXPECT_EQ(march[1], audio);
    EXPECT_EQ(march[2]["type"], "text");
    EXPECT_EQ(json::parse(march[2]["text"].get<std::string>()), link);

    const auto november = call("2024-11-05");
    ASSERT_EQ(november.size(), 3u);
    EXPECT_EQ(november[1]["type"], "text");
    EXPECT_EQ(json::parse(november[1]["text"].get<std::string>()), audio);
    EXPECT_EQ(november[2]["type"], "text");
    EXPECT_EQ(json::parse(november[2]["text"].get<std::string>()), link);
}

namespace {

// The outcome handle_message(message, reply) delivers, at once or later.
struct ReplyCapture {
    std::promise<std::optional<json>> promise;
    std::future<std::optional<json>> future = promise.get_future();
    std::atomic<int> calls{0};

    MCPServer::Reply reply() {
        return [this](std::optional<json> outcome) {
            if (calls.fetch_add(1) == 0) promise.set_value(std::move(outcome));
        };
    }
    std::optional<json> get() {
        if (future.wait_for(5s) != std::future_status::ready) {
            ADD_FAILURE() << "no outcome";
            return json("timed out");
        }
        return future.get();
    }
};

void initialize_as(MCPServer& server, const std::string& revision) {
    auto response = server.handle_message(request("init", "initialize", {
        {"protocolVersion", revision},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "batch-client"}, {"version", "1"}}},
    }));
    ASSERT_EQ(response["result"]["protocolVersion"], revision);
    server.handle_message(notification("notifications/initialized"));
}

// The response in @p batch with @p id, or null.
json by_id(const json& batch, const json& id) {
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const json item = batch[i];
        if (item.contains("id") && item["id"] == id) return item;
    }
    return nullptr;
}

}  // namespace

// MCP 2025-03-26 (basic, "Batching"): implementations MUST support receiving
// JSON-RPC batches. 2024-11-05 defined none; its peers are answered the same
// way. The answer is one array with a response for each request, in any
// order; notifications get none.
TEST(MCPServerTest, BatchesAreAnsweredForTheRevisionsThatHaveThem) {
    for (const std::string revision : {"2025-03-26", "2024-11-05"}) {
        MCPServer server(config());
        server.register_tool(echo_definition(),
            [](const json& arguments, const auto&) { return echo_result(arguments); });
        initialize_as(server, revision);
        const json batch = json::array({
            request("p", "ping"),
            notification("notifications/progress", {{"progressToken", 1}, {"progress", 1}}),
            request("l", "tools/list"),
            request("c", "tools/call", {{"name", "echo"}, {"arguments", {{"value", "x"}}}}),
            json(7),
        });
        ReplyCapture captured;
        server.handle_message(batch, captured.reply());
        const auto outcome = captured.get();
        ASSERT_TRUE(outcome && outcome->is_array()) << (outcome ? outcome->dump() : "none");
        EXPECT_EQ(outcome->size(), 4u) << outcome->dump();
        EXPECT_EQ(by_id(*outcome, "p")["result"], json::object());
        EXPECT_EQ(by_id(*outcome, "l")["result"]["tools"][0]["name"], "echo");
        EXPECT_EQ(by_id(*outcome, "c")["result"]["content"][0]["text"], "x");
        bool invalid_answered = false;
        for (std::size_t i = 0; i < outcome->size(); ++i) {
            const json item = (*outcome)[i];
            if (!item.contains("id")) invalid_answered = item["error"]["code"] == -32600;
        }
        EXPECT_TRUE(invalid_answered) << outcome->dump();
        EXPECT_EQ(captured.calls.load(), 1);

        // Only notifications: nothing to answer.
        ReplyCapture quiet;
        server.handle_message(json::array({notification("notifications/progress",
                                                        {{"progressToken", 1}, {"progress", 2}})}),
                              quiet.reply());
        EXPECT_FALSE(quiet.get().has_value());

        // An empty batch is one invalid request.
        ReplyCapture empty;
        server.handle_message(json::array(), empty.reply());
        const auto refused = empty.get();
        ASSERT_TRUE(refused && refused->is_object());
        EXPECT_EQ((*refused)["error"]["code"], -32600);
        EXPECT_FALSE(refused->contains("id"));

        // The one-argument form answers a batch without tool calls at once,
        // and one with tool calls through the response sink.
        const auto immediate =
            server.handle_message(json::array({request("q", "ping"), request("r", "tools/list")}));
        ASSERT_TRUE(immediate.is_array()) << immediate.dump();
        EXPECT_EQ(immediate.size(), 2u);
        CapturingSink sink;
        server.set_response_sink(sink.sink());
        EXPECT_TRUE(server.handle_message(json::array({request("s", "ping"),
            request("t", "tools/call", {{"name", "echo"}, {"arguments", {{"value", "y"}}}})}))
                        .is_null());
        std::unique_lock lock(sink.mutex);
        ASSERT_TRUE(sink.cv.wait_for(lock, 5s, [&] { return !sink.responses.empty(); }));
        ASSERT_TRUE(sink.responses[0].is_array()) << sink.responses[0].dump();
        EXPECT_EQ(by_id(sink.responses[0], "t")["result"]["content"][0]["text"], "y");
        EXPECT_EQ(by_id(sink.responses[0], "s")["result"], json::object());
    }
}

// A batch with a tool call is answered through the response sink even when
// the call finishes before the server has read the rest of the batch: how a
// batch is answered must not depend on timing.
TEST(MCPServerTest, ABatchWhoseToolCallFinishesFirstIsStillAnsweredThroughTheSink) {
    MCPServer server(config());
    std::promise<void> returned;
    std::shared_future<void> call_returned = returned.get_future().share();
    server.register_tool(echo_definition(), [&returned](const json& arguments, const auto&) {
        auto result = echo_result(arguments);
        returned.set_value();
        return result;
    });
    // The method after the call runs on this thread and waits until the call
    // has returned and settled, so the call's answer is in before the batch
    // ends.
    server.register_method("test/after_call", [call_returned](const json&, const json&) {
        call_returned.wait();
        std::this_thread::sleep_for(200ms);
        return json::object();
    });
    initialize_as(server, "2025-03-26");
    CapturingSink sink;
    server.set_response_sink(sink.sink());
    const json batch = json::array({
        request("c", "tools/call", {{"name", "echo"}, {"arguments", {{"value", "z"}}}}),
        request("a", "test/after_call"),
    });
    EXPECT_TRUE(server.handle_message(batch).is_null());
    std::unique_lock lock(sink.mutex);
    ASSERT_TRUE(sink.cv.wait_for(lock, 5s, [&] { return !sink.responses.empty(); }));
    ASSERT_EQ(sink.responses.size(), 1u);
    ASSERT_TRUE(sink.responses[0].is_array()) << sink.responses[0].dump();
    EXPECT_EQ(by_id(sink.responses[0], "c")["result"]["content"][0]["text"], "z");
    EXPECT_EQ(by_id(sink.responses[0], "a")["result"], json::object());
}

// MCP 2025-06-18 removed JSON-RPC batching: a batch from such a client, or
// from one that has not initialized (initialize is never part of a batch), is
// one invalid request.
TEST(MCPServerTest, BatchesAreRefusedWhereTheRevisionHasNone) {
    for (const std::string revision : {"2025-06-18", "2025-11-25"}) {
        MCPServer server(config());
        initialize_as(server, revision);
        ReplyCapture captured;
        server.handle_message(json::array({request("p", "ping")}), captured.reply());
        const auto refused = captured.get();
        ASSERT_TRUE(refused && refused->is_object()) << (refused ? refused->dump() : "none");
        EXPECT_EQ((*refused)["error"]["code"], -32600);
        EXPECT_FALSE(refused->contains("id"));
    }
    MCPServer fresh(config());
    const auto before = fresh.handle_message(json::array({request(1, "initialize", {
        {"protocolVersion", "2025-03-26"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "batch-client"}, {"version", "1"}}},
    })}));
    ASSERT_TRUE(before.is_object()) << before.dump();
    EXPECT_EQ(before["error"]["code"], -32600);
    EXPECT_FALSE(fresh.initialized());
    EXPECT_TRUE(fresh.protocol_version().empty());
}

TEST(MCPServerTest, ACancelledCallInABatchIsLeftOut) {
    MCPServer server(config(2, 2));
    std::atomic<int> entered{0};
    server.register_tool(echo_definition(), [&](const json&, const auto& cancel) {
        entered.fetch_add(1);
        while (!cancel->is_cancelled()) std::this_thread::sleep_for(1ms);
        cancel->throw_if_cancelled("inside test tool");
        return CallToolResult{};
    });
    initialize_as(server, "2025-03-26");
    ReplyCapture mixed;
    server.handle_message(json::array({
        request("slow", "tools/call", {{"name", "echo"}, {"arguments", {{"value", "x"}}}}),
        request("p", "ping")}), mixed.reply());
    for (int i = 0; i < 400 && entered.load() < 1; ++i) std::this_thread::sleep_for(5ms);
    server.handle_message(notification("notifications/cancelled", {{"requestId", "slow"}}));
    const auto outcome = mixed.get();
    ASSERT_TRUE(outcome && outcome->is_array());
    ASSERT_EQ(outcome->size(), 1u) << outcome->dump();
    EXPECT_EQ((*outcome)[0]["id"], "p");

    // Every request cancelled: nothing at all, not an empty array.
    ReplyCapture alone;
    server.handle_message(json::array({request("gone", "tools/call",
        {{"name", "echo"}, {"arguments", {{"value", "x"}}}})}), alone.reply());
    for (int i = 0; i < 400 && entered.load() < 2; ++i) std::this_thread::sleep_for(5ms);
    server.handle_message(notification("notifications/cancelled", {{"requestId", "gone"}}));
    EXPECT_FALSE(alone.get().has_value());
}

namespace {

// stdin that blocks until fed or closed, so the run loop does not reach EOF
// (which stops the server) before the outcome is written.
class FeedBuf final : public std::streambuf {
  public:
    void feed(const std::string& text) {
        std::lock_guard lock(mu_);
        data_ += text;
        cv_.notify_all();
    }
    void close() {
        std::lock_guard lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

  protected:
    int_type underflow() override {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return pos_ < data_.size() || closed_; });
        if (pos_ >= data_.size()) return traits_type::eof();
        current_ = data_[pos_++];
        setg(&current_, &current_, &current_ + 1);
        return traits_type::to_int_type(current_);
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::string data_;
    std::size_t pos_ = 0;
    bool closed_ = false;
    char current_ = 0;
};

// stdout whose lines the test can wait for while the run loop writes.
class LineSink final : public std::streambuf {
  public:
    std::vector<json> wait_for_lines(std::size_t count) {
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, 5s, [&] { return lines_.size() >= count; });
        std::vector<json> out;
        for (const auto& line : lines_) out.push_back(json::parse(line));
        return out;
    }

  protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        std::lock_guard lock(mu_);
        if (traits_type::to_char_type(ch) == '\n') {
            if (!partial_.empty()) lines_.push_back(std::move(partial_));
            partial_.clear();
            cv_.notify_all();
        } else {
            partial_.push_back(traits_type::to_char_type(ch));
        }
        return ch;
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::string partial_;
    std::vector<std::string> lines_;
};

}  // namespace

TEST(MCPServerTest, StdioAnswersABatchWithOneArray) {
    MCPServer server(config());
    server.register_tool(echo_definition(),
        [](const json& arguments, const auto&) { return echo_result(arguments); });
    FeedBuf input_buffer;
    LineSink output_buffer;
    std::istream in(&input_buffer);
    std::ostream out(&output_buffer);
    std::thread runner([&] { server.run(in, out); });
    input_buffer.feed(request(1, "initialize", {{"protocolVersion", "2025-03-26"},
                                                {"capabilities", json::object()},
                                                {"clientInfo", {{"name", "c"}, {"version", "1"}}}})
                          .dump() + "\n" +
                      notification("notifications/initialized").dump() + "\n" +
                      json::array({request(2, "ping"),
                                   request(3, "tools/call", {{"name", "echo"},
                                                             {"arguments", {{"value", "z"}}}})})
                          .dump() + "\n");
    const auto frames = output_buffer.wait_for_lines(2);
    input_buffer.close();
    runner.join();
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0]["id"], 1);
    ASSERT_TRUE(frames[1].is_array()) << frames[1].dump();
    EXPECT_EQ(frames[1].size(), 2u);
    EXPECT_EQ(by_id(frames[1], 2)["result"], json::object());
    EXPECT_EQ(by_id(frames[1], 3)["result"]["content"][0]["text"], "z");
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
    // No id could be read: the response has none, never "id": null (MCP-S-07).
    EXPECT_FALSE(frames[2].contains("id")) << frames[2].dump();
    EXPECT_NE(errors.str().find("parse error"), std::string::npos);
}

TEST(MCPServerTest, InvalidEnvelopesEchoAReadableIdAndNeverANullOne) {
    // MCP-S-05: the error of an invalid envelope carries its id when the id
    // could be read; MCP-S-07: otherwise it has no id at all.
    MCPServer server(config());
    initialize(server);

    auto wrong_version = server.handle_message(json::parse(R"({"jsonrpc":"1.0","id":42,"method":"ping"})"));
    EXPECT_EQ(wrong_version["error"]["code"], -32600);
    EXPECT_EQ(wrong_version["id"], 42);

    auto no_method = server.handle_message(json::parse(R"({"jsonrpc":"2.0","id":"s-7"})"));
    EXPECT_EQ(no_method["error"]["code"], -32600);
    EXPECT_EQ(no_method["id"], "s-7");

    auto numeric_method = server.handle_message(json::parse(R"({"jsonrpc":"2.0","id":8,"method":3})"));
    EXPECT_EQ(numeric_method["id"], 8);

    for (const char* unreadable : {R"({"jsonrpc":"2.0","id":null,"method":"ping"})",
                                   R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})",
                                   R"([{"jsonrpc":"2.0","id":9,"method":"ping"}])",
                                   R"({"jsonrpc":"1.0","id":{"x":1},"method":"ping"})"}) {
        auto response = server.handle_message(json::parse(unreadable));
        EXPECT_EQ(response["error"]["code"], -32600) << unreadable;
        EXPECT_FALSE(response.contains("id")) << response.dump();
    }
}

} // namespace
