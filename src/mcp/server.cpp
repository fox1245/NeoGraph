#include <neograph/mcp/json_schema.h>
#include <neograph/mcp/server.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>

#include "../core/tool_failure.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::mcp {
namespace {

json rpc_result(json result, const json& id) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// An error response carries the request's id when it could be read, and no id
// otherwise: the 2025-11-25 schema allows a string, an integer or no id, never
// null (MCP-S-05, MCP-S-07).
json rpc_error(int code, std::string message, const json& id, json data = nullptr) {
    json error = {{"code", code}, {"message", std::move(message)}};
    if (!data.is_null()) error["data"] = std::move(data);
    json response = {{"jsonrpc", "2.0"}, {"error", std::move(error)}};
    if (!id.is_null()) response["id"] = id;
    return response;
}

CallToolResult tool_error(std::string message) {
    CallToolResult result;
    result.content = json::array({{{"type", "text"}, {"text", std::move(message)}}});
    result.is_error = true;
    return result;
}

// JSON-RPC error codes of the run control signals (reference, "Protocol
// failure mapping"). They never become a tool result a model could read.
constexpr int kServerError      = -32000;
constexpr int kInternalError    = -32603;
constexpr int kHookBlocked      = -32010;
constexpr int kInterrupted      = -32011;

// The response to a tools/call whose handler failed. Run control signals are
// JSON-RPC errors with distinct codes; every other failure is a tool result
// with isError and its stable code in `_meta["io.neograph/failure"]`. Only the
// public message crosses the wire, never what(): exception text can carry
// paths, payloads or secrets.
json failure_response(const std::exception_ptr& error, const graph::CancelToken& cancel,
                      bool stopping, const json& id) {
    auto verdict = graph::classify_failure(error, &cancel);
    if (stopping && cancel.is_cancelled())
        return rpc_error(kServerError, "MCP server is stopping", id, {{"code", "NG.Cancelled"}});
    const auto message = ::neograph::detail::public_failure_message(verdict);
    switch (verdict.failure_class) {
    case graph::NodeFailureClass::Cancelled:
        // The handler's own work was cancelled: a server-side condition.
        return rpc_error(kInternalError, message, id, {{"code", verdict.code}});
    case graph::NodeFailureClass::HookBlocked: {
        json data{{"code", verdict.code}};
        try {
            if (verdict.signal) std::rethrow_exception(verdict.signal);
        } catch (const HookBoundaryBlocked& blocked) {
            data["invocation_id"] = blocked.invocation_id();
            data["state"] = std::string(to_string(blocked.state()));
        } catch (...) {
        }
        return rpc_error(kHookBlocked, message, id, std::move(data));
    }
    case graph::NodeFailureClass::Interrupted: {
        json data{{"code", verdict.code}};
        try {
            if (verdict.signal) std::rethrow_exception(verdict.signal);
        } catch (const graph::NodeInterrupt& interrupt) {
            // What the interrupt's author meant a human to read.
            data["reason"] = interrupt.reason();
            data["value"] = interrupt.value();
        } catch (...) {
        }
        return rpc_error(kInterrupted, message, id, std::move(data));
    }
    default: {
        auto result = tool_error(message);
        result.meta = json{{"io.neograph/failure",
                            {{"code", verdict.code}, {"retryable", verdict.retryable}}}};
        return rpc_result(result.to_json(), id);
    }
    }
}

// A copy of an object without the named members (json has no erase).
json without(const json& object, std::initializer_list<std::string_view> names) {
    json copy = json::object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string key = it.key();
        if (std::find(names.begin(), names.end(), key) == names.end()) copy[key] = it.value();
    }
    return copy;
}

// A client that negotiated an older revision gets the fields that revision
// defines (D2): 2025-06-18 has no Tool.icons or Tool.execution; 2025-03-26 also
// no Tool.title, Tool.outputSchema, Tool._meta, CallToolResult.structuredContent
// or resource_link content; 2024-11-05 also no Tool.annotations or audio
// content. Other members a revision lacks are allowed by its schema and kept.
bool before_2025_06_18(const std::string& version) {
    return version == "2025-03-26" || version == "2024-11-05";
}

json shaped_tool(const json& tool, const std::string& version) {
    if (version.empty() || version == MCP_PROTOCOL_VERSION) return tool;
    if (version == "2025-06-18") return without(tool, {"icons", "execution"});
    if (version == "2025-03-26")
        return without(tool, {"icons", "execution", "title", "outputSchema", "_meta"});
    return without(tool, {"icons", "execution", "title", "outputSchema", "_meta", "annotations"});
}

// Whether @p version defines content blocks of @p type.
bool has_content_type(const std::string& version, const std::string& type) {
    if (type == "resource_link") return !before_2025_06_18(version);
    if (type == "audio") return version != "2024-11-05";
    return true;
}

json shaped_call_result(const json& result, const std::string& version) {
    if (!before_2025_06_18(version) || !result.is_object()) return result;
    json shaped = result;
    // A content block whose type the revision lacks would fail its schema: it
    // is sent as a text block holding its JSON, so nothing is lost.
    if (result.contains("content") && result["content"].is_array()) {
        json content = json::array();
        const json blocks = result["content"];
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            json block = blocks[i];
            if (block.is_object() && !has_content_type(version, block.value("type", "")))
                block = json{{"type", "text"}, {"text", block.dump()}};
            content.push_back(std::move(block));
        }
        shaped["content"] = std::move(content);
    }
    if (!result.contains("structuredContent")) return shaped;
    shaped = without(shaped, {"structuredContent"});
    // Keep the data: a result without a text block carries it as text, as
    // MCP asks of structured results for clients that cannot read them.
    json content = shaped.contains("content") && shaped["content"].is_array() ? shaped["content"]
                                                                               : json::array();
    bool has_text = false;
    for (std::size_t i = 0; i < content.size() && !has_text; ++i)
        has_text = content[i].is_object() && content[i].value("type", "") == "text";
    if (!has_text) {
        content.push_back({{"type", "text"}, {"text", result["structuredContent"].dump()}});
        shaped["content"] = std::move(content);
    }
    return shaped;
}

bool valid_request_id(const json& id) {
    return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

// The id of an envelope that is otherwise invalid, when it is a valid id.
json readable_id(const json& envelope) {
    if (envelope.is_object() && envelope.contains("id") && valid_request_id(envelope["id"]))
        return envelope["id"];
    return nullptr;
}

std::string request_key(const json& id) {
    if (id.is_string()) return "s:" + id.get<std::string>();
    return "n:" + id.dump();
}

// MCP 2025-11-25 server/tools: a tool name SHOULD be 1 to 128 characters of
// A-Z, a-z, 0-9, '_', '-' and '.' (SEP-986).
bool valid_tool_name(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
    });
}

void require_object_schema(const json& schema, const char* field) {
    if (!schema.is_object() || !schema.contains("type") || schema["type"] != json("object")) {
        throw std::invalid_argument(std::string("MCP tool ") + field +
                                    " must be a JSON Schema with \"type\": \"object\"");
    }
}

// Every listed tool must survive the official clients' result schemas: one
// tool with `inputSchema: {}` made pydantic and zod reject the whole
// tools/list (MCP-S-01). An empty object, the old ToolDefinition default, is
// read as {"type": "object"}; any other schema must say "type": "object"
// itself, and so must an outputSchema.
ToolDefinition validated_definition(ToolDefinition definition) {
    if (!valid_tool_name(definition.name)) {
        throw std::invalid_argument(
            "MCP tool name must be 1 to 128 characters of A-Z, a-z, 0-9, '_', '-' and '.': " +
            json(definition.name).dump());
    }
    if (definition.input_schema.is_object() && definition.input_schema.empty())
        definition.input_schema = json{{"type", "object"}};
    ToolDefinition::from_json(definition.to_json());
    require_object_schema(definition.input_schema, "inputSchema");
    validate_json_schema(definition.input_schema, "inputSchema");
    if (!definition.output_schema.is_null()) {
        require_object_schema(definition.output_schema, "outputSchema");
        validate_json_schema(definition.output_schema, "outputSchema");
    }
    return definition;
}

} // namespace

struct MCPServer::Impl {
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
        RawToolHandler raw_handler;
    };

    /// Where a deferred outcome goes: a response, or nullopt for none.
    using Deliver = std::function<void(std::optional<json>)>;

    struct CallTask {
        json id;
        std::string key;
        json arguments;
        json                                meta;
        RegisteredTool tool;
        std::shared_ptr<graph::CancelToken> cancel;
        /// Set by notifications/cancelled before the token is cancelled.
        std::atomic<bool> client_cancelled{false};
        Deliver deliver;
    };

    /// What process() knows when it returns: the response (null for none), or
    /// that the outcome reaches its Deliver later.
    struct Outcome {
        bool deferred = false;
        json response;
    };

    /// A JSON-RPC batch whose tool calls run: one array once every message
    /// has its outcome. `outstanding` counts the messages still to settle and
    /// the dispatch itself until every message was given to the server.
    struct Batch {
        std::mutex mu;
        json responses = json::array();
        std::size_t outstanding = 1;
        Deliver deliver;

        /// Adds one message's outcome; true for the last one.
        bool add(std::optional<json> response) {
            std::lock_guard lk(mu);
            if (response && !response->is_null()) responses.push_back(std::move(*response));
            return --outstanding == 0;
        }
        std::optional<json> answer() {
            std::lock_guard lk(mu);
            if (responses.empty()) return std::nullopt;
            return responses;
        }
    };

    explicit Impl(MCPServerConfig value) : config(std::move(value)) {}

    MCPServerConfig config;
    mutable std::mutex version_mu;
    std::string negotiated_version;
    std::atomic<bool> initialize_seen{false};
    std::atomic<bool> operational{false};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::mutex tools_mu;
    std::map<std::string, RegisteredTool> tools;
    std::map<std::string, MethodHandler>  methods;
    json                                  extensions        = json::object();
    json                                  client_extensions = json::object();

    /// Where asynchronous outcomes go: a response, or the id of a call that
    /// ends without one. Replaced together, read without a lock.
    struct Sinks {
        ResponseSink   response;
        NoResponseSink no_response;
    };
    std::shared_ptr<const Sinks> sinks;

    std::mutex calls_mu;
    std::condition_variable calls_cv;
    std::deque<std::shared_ptr<CallTask>> queue;
    std::unordered_map<std::string, std::shared_ptr<CallTask>> active;
    std::vector<std::thread> workers;

    std::string protocol_version() const {
        std::lock_guard lk(version_mu);
        return negotiated_version;
    }

    void emit(const json& envelope) noexcept {
        try {
            auto current = std::atomic_load_explicit(&sinks, std::memory_order_acquire);
            if (current && current->response) current->response(envelope);
        } catch (...) {
            // A transport failure cannot safely be reported through itself.
        }
    }

    // A call the client cancelled: MCP asks the receiver of
    // notifications/cancelled not to respond, and the official clients
    // report a response to an id they gave up as an error. No transport
    // answers it; one that holds the request open is told it ended.
    void end_without_response(const json& id) noexcept {
        try {
            auto current = std::atomic_load_explicit(&sinks, std::memory_order_acquire);
            if (current && current->no_response) current->no_response(id);
        } catch (...) {
        }
    }

    static void deliver(CallTask& task, std::optional<json> outcome) noexcept {
        try {
            if (task.deliver) task.deliver(std::move(outcome));
        } catch (...) {
            // A transport failure cannot safely be reported through itself.
        }
    }

    /// The Deliver of the one-argument handle_message: a response goes to the
    /// response sink, a call that ends without one to the no-response sink.
    Deliver to_sinks(const json& message) {
        const json id = message.is_object() && message.contains("id") ? message["id"] : json();
        return [this, id](std::optional<json> outcome) {
            if (outcome) emit(*outcome);
            else if (!id.is_null()) end_without_response(id);
        };
    }

    // MCP 2025-03-26 (basic, "Batching") makes receiving JSON-RPC batches a
    // MUST, and 2025-06-18 removed them. 2024-11-05 defined none; its peers
    // are answered as 2025-03-26 ones. Before initialize no revision is
    // negotiated, and initialize itself is never part of a batch.
    static bool has_batches(const std::string& version) {
        return version == "2025-03-26" || version == "2024-11-05";
    }

    Outcome process(const json& message, Deliver deliver) {
        if (!message.is_array()) return process_single(message, std::move(deliver));
        const auto version = protocol_version();
        if (!has_batches(version)) {
            return {false, rpc_error(-32600,
                                     version.empty()
                                         ? "a JSON-RPC batch cannot initialize an MCP session"
                                         : "MCP " + version + " has no JSON-RPC batches",
                                     nullptr)};
        }
        if (message.empty()) return {false, rpc_error(-32600, "empty JSON-RPC batch", nullptr)};
        auto batch = std::make_shared<Batch>();
        batch->deliver = std::move(deliver);
        for (std::size_t i = 0; i < message.size(); ++i) {
            const json element = message[i];
            {
                std::lock_guard lk(batch->mu);
                ++batch->outstanding;
            }
            Outcome outcome;
            if (element.is_array()) {
                outcome.response = rpc_error(-32600, "a JSON-RPC batch cannot hold a batch", nullptr);
            } else {
                outcome = process_single(element, [batch](std::optional<json> response) {
                    if (batch->add(std::move(response))) batch->deliver(batch->answer());
                });
            }
            if (!outcome.deferred) batch->add(std::move(outcome.response));
        }
        // Every message was given to the server: when nothing runs any more,
        // the answer is ready now; otherwise the last tool call delivers it.
        if (batch->add(std::nullopt)) {
            auto answer = batch->answer();
            return {false, answer ? std::move(*answer) : json()};
        }
        return {true, json()};
    }

    Outcome process_single(const json& envelope, Deliver deliver) {
        if (!envelope.is_object() || !envelope.contains("jsonrpc") ||
            envelope["jsonrpc"] != json("2.0")) {
            return {false, rpc_error(-32600, "Invalid JSON-RPC 2.0 request", readable_id(envelope))};
        }
        if (!envelope.contains("method")) {
            // The server currently sends no requests, so inbound responses
            // have no waiter. Valid response envelopes are ignored rather than
            // answered.
            if (envelope.contains("id") &&
                (envelope.contains("result") || envelope.contains("error"))) {
                return {};
            }
            return {false, rpc_error(-32600, "JSON-RPC request is missing method",
                                     readable_id(envelope))};
        }
        if (!envelope["method"].is_string()) {
            return {false, rpc_error(-32600, "JSON-RPC method must be a string",
                                     readable_id(envelope))};
        }

        const bool notification = !envelope.contains("id");
        json id = notification ? json(nullptr) : envelope["id"];
        if (!notification && !valid_request_id(id)) {
            return {false, rpc_error(-32600, "JSON-RPC id must be a string or integer", nullptr)};
        }
        const auto method = envelope["method"].get<std::string>();
        const json params = envelope.value("params", json(nullptr));

        if (notification) {
            if (method == "notifications/initialized") {
                if (initialize_seen.load(std::memory_order_acquire)) {
                    operational.store(true, std::memory_order_release);
                }
            } else if (method == "notifications/cancelled") {
                handle_cancel(params);
            }
            return {};
        }

        if (method == "ping") return {false, rpc_result(json::object(), id)};
        if (method == "initialize") return {false, handle_initialize(params, id)};
        if (!operational.load(std::memory_order_acquire)) {
            return {false, rpc_error(-32002, "MCP server is not initialized", id)};
        }
        if (method == "tools/list") return {false, handle_tools_list(params, id)};
        if (method == "tools/call") {
            auto response = handle_tools_call(params, id, std::move(deliver));
            return {response.is_null(), std::move(response)};
        }
        MCPServer::MethodHandler extension_handler;
        {
            std::lock_guard lk(tools_mu);
            auto            it = methods.find(method);
            if (it != methods.end()) extension_handler = it->second;
        }
        if (extension_handler) {
            try {
                json context = {{"extensions", client_extensions}};
                return {false, rpc_result(extension_handler(params, context), id)};
            } catch (const std::invalid_argument& error) {
                return {false, rpc_error(-32602, error.what(), id)};
            } catch (...) {
                // Exception text never crosses the wire (MCP-S-20).
                return {false, rpc_error(-32603, "Internal error", id)};
            }
        }
        return {false, rpc_error(-32601, "Method not found: " + method, id)};
    }

    void start_workers() {
        workers.reserve(config.max_concurrent_calls);
        try {
            for (std::size_t i = 0; i < config.max_concurrent_calls; ++i) {
                workers.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            stopping.store(true, std::memory_order_release);
            calls_cv.notify_all();
            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
            workers.clear();
            throw;
        }
    }

    void worker_loop() {
        while (true) {
            std::shared_ptr<CallTask> task;
            {
                std::unique_lock lk(calls_mu);
                calls_cv.wait(lk, [this] {
                    return stopping.load(std::memory_order_acquire) || !queue.empty();
                });
                if (queue.empty()) return;
                task = std::move(queue.front());
                queue.pop_front();
            }

            json result_json;
            std::exception_ptr failure;
            try {
                task->cancel->throw_if_cancelled("before tool execution");
                if (task->tool.raw_handler) {
                    result_json = task->tool.raw_handler(task->arguments, task->cancel, task->meta);
                    if (!result_json.is_object()) {
                        throw std::invalid_argument("raw MCP tool handler must return an object");
                    }
                } else {
                    auto result = task->tool.handler(task->arguments, task->cancel);
                    result      = CallToolResult::from_json(result.to_json());
                    if (!result.is_error && !task->tool.definition.output_schema.is_null()) {
                        // A result that breaks the tool's own outputSchema is
                        // a tool error the client may read: the validator's
                        // diagnostic names the schema path, not handler text.
                        std::string problem;
                        if (result.structured_content.is_null()) {
                            problem = "tool advertised outputSchema but returned no "
                                      "structuredContent";
                        } else {
                            try {
                                validate_json_value(result.structured_content,
                                                    task->tool.definition.output_schema,
                                                    "MCP tool structuredContent", "$");
                            } catch (const std::invalid_argument& error) {
                                problem = error.what();
                            }
                        }
                        if (!problem.empty()) {
                            result = tool_error("Tool result does not match its outputSchema: " +
                                                problem);
                            result.meta = json{{"io.neograph/failure",
                                                {{"code", "NG.LogicError"}, {"retryable", false}}}};
                        }
                    }
                    result_json = result.to_json();
                }
                task->cancel->throw_if_cancelled("after tool execution");
            } catch (...) {
                failure = std::current_exception();
            }

            {
                std::lock_guard lk(calls_mu);
                active.erase(task->key);
            }
            if (failure && task->client_cancelled.load(std::memory_order_acquire)) {
                deliver(*task, std::nullopt);
                continue;
            }
            deliver(*task,
                    failure ? failure_response(failure, *task->cancel,
                                               stopping.load(std::memory_order_acquire), task->id)
                            : rpc_result(shaped_call_result(result_json, protocol_version()),
                                         task->id));
        }
    }

    json handle_initialize(const json& params, const json& id) {
        if (initialize_seen.exchange(true, std::memory_order_acq_rel)) {
            return rpc_error(-32600, "MCP initialize may only be sent once", id);
        }
        if (!params.is_object() || !params.contains("protocolVersion") ||
            !params["protocolVersion"].is_string() || !params.contains("capabilities") ||
            !params["capabilities"].is_object() || !params.contains("clientInfo") ||
            !params["clientInfo"].is_object()) {
            initialize_seen.store(false, std::memory_order_release);
            return rpc_error(
                -32602, "initialize requires protocolVersion, capabilities, and clientInfo", id);
        }

        // D2: answer the revision the client asks for when this server
        // supports it, and the newest one otherwise (MCP lifecycle, "Version
        // Negotiation").
        const auto requested = params["protocolVersion"].get<std::string>();
        const std::string version =
            is_supported_protocol_version(requested) ? requested : MCP_PROTOCOL_VERSION;
        {
            std::lock_guard lk(version_mu);
            negotiated_version = version;
        }
        json result = {
            {"protocolVersion", version},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", config.server_info},
        };
        const auto client_capabilities = params.value("capabilities", json::object());
        client_extensions              = client_capabilities.value("extensions", json::object());
        if (!extensions.empty()) {
            result["capabilities"]["extensions"] = extensions;
        }
        if (!config.instructions.empty()) {
            result["instructions"] = config.instructions;
        }
        return rpc_result(std::move(result), id);
    }

    json handle_tools_list(const json& params, const json& id) {
        if (!params.is_null() && !params.is_object()) {
            return rpc_error(-32602, "tools/list params must be an object", id);
        }
        if (params.is_object() && params.contains("cursor")) {
            return rpc_error(-32602, "tools/list cursor is invalid: this catalogue is unpaged", id);
        }

        const auto version = protocol_version();
        json listed = json::array();
        {
            std::lock_guard lk(tools_mu);
            for (const auto& [name, tool] : tools) {
                (void)name;
                listed.push_back(shaped_tool(tool.definition.to_json(), version));
            }
        }
        return rpc_result({{"tools", std::move(listed)}}, id);
    }

    json handle_tools_call(const json& params, const json& id, Deliver deliver) {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
            params["name"].get<std::string>().empty()) {
            return rpc_error(-32602, "tools/call requires a non-empty name", id);
        }
        json arguments = params.value("arguments", json::object());
        if (!arguments.is_object()) {
            return rpc_error(-32602, "tools/call arguments must be an object", id);
        }
        json meta = params.value("_meta", json::object());
        if (!meta.is_object()) {
            return rpc_error(-32602, "tools/call _meta must be an object", id);
        }

        RegisteredTool tool;
        const auto name = params["name"].get<std::string>();
        {
            std::lock_guard lk(tools_mu);
            auto it = tools.find(name);
            if (it == tools.end()) {
                // MCP 2025-11-25 server/tools, "Error Handling": an unknown tool
                // is a protocol error (MCP-S-06).
                return rpc_error(-32602, "Unknown tool: " + name, id,
                                 {{"code", "NG.ToolNotFound"}});
            }
            tool = it->second;
        }
        try {
            validate_json_value(arguments, tool.definition.input_schema, "MCP tool arguments", "$");
        } catch (const std::exception& e) {
            return rpc_result(tool_error(e.what()).to_json(), id);
        }

        auto task = std::make_shared<CallTask>();
        task->id = id;
        task->key = request_key(id);
        task->arguments = std::move(arguments);
        task->meta                                     = std::move(meta);
        task->meta["io.neograph/negotiatedExtensions"] = client_extensions;
        task->tool = std::move(tool);
        task->cancel = std::make_shared<graph::CancelToken>();
        task->deliver = std::move(deliver);

        {
            std::lock_guard lk(calls_mu);
            if (stopping.load(std::memory_order_acquire)) {
                return rpc_error(-32000, "MCP server is stopping", id);
            }
            if (active.contains(task->key)) {
                return rpc_error(-32600, "duplicate in-flight request id", id);
            }
            const auto capacity = config.max_concurrent_calls + config.max_pending_calls;
            if (active.size() >= capacity) {
                return rpc_error(-32000, "MCP tool queue is full", id, {{"capacity", capacity}});
            }
            active.emplace(task->key, task);
            queue.push_back(task);
        }
        calls_cv.notify_one();
        return nullptr;
    }

    void handle_cancel(const json& params) {
        if (!params.is_object() || !params.contains("requestId") ||
            !valid_request_id(params["requestId"])) {
            return;
        }
        std::shared_ptr<CallTask> task;
        {
            std::lock_guard lk(calls_mu);
            auto it = active.find(request_key(params["requestId"]));
            if (it != active.end()) task = it->second;
        }
        if (task) {
            task->client_cancelled.store(true, std::memory_order_release);
            task->cancel->cancel();
        }
    }
};

MCPServer::MCPServer(MCPServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
    if (!impl_->config.server_info.is_object() || !impl_->config.server_info.contains("name") ||
        !impl_->config.server_info["name"].is_string() ||
        !impl_->config.server_info.contains("version") ||
        !impl_->config.server_info["version"].is_string()) {
        throw std::invalid_argument("MCP server_info requires string name and version fields");
    }
    if (impl_->config.max_concurrent_calls == 0) {
        throw std::invalid_argument("MCP max_concurrent_calls must be positive");
    }
    impl_->start_workers();
}

MCPServer::~MCPServer() {
    stop();
}

void MCPServer::register_tool(ToolDefinition definition, ToolHandler handler) {
    if (!handler) throw std::invalid_argument("MCP tool handler must be callable");
    if (impl_->initialize_seen.load(std::memory_order_acquire)) {
        throw std::logic_error("MCP tools must be registered before initialize");
    }
    definition = validated_definition(std::move(definition));
    const auto name = definition.name;
    std::lock_guard lk(impl_->tools_mu);
    auto [it, inserted] = impl_->tools.emplace(
        name, Impl::RegisteredTool{std::move(definition), std::move(handler), {}});
    if (!inserted) {
        throw std::invalid_argument("duplicate MCP tool name: " + it->first);
    }
}

void MCPServer::register_raw_tool(ToolDefinition definition, RawToolHandler handler) {
    if (!handler) throw std::invalid_argument("MCP raw tool handler must be callable");
    if (impl_->initialize_seen.load(std::memory_order_acquire)) {
        throw std::logic_error("MCP tools must be registered before initialize");
    }
    definition = validated_definition(std::move(definition));
    const auto      name = definition.name;
    std::lock_guard lk(impl_->tools_mu);
    auto [it, inserted] = impl_->tools.emplace(
        name, Impl::RegisteredTool{std::move(definition), {}, std::move(handler)});
    if (!inserted) {
        throw std::invalid_argument("duplicate MCP tool name: " + it->first);
    }
}

void MCPServer::register_method(std::string method, MethodHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument("MCP extension method requires a name and callable handler");
    }
    if (impl_->initialize_seen.load(std::memory_order_acquire)) {
        throw std::logic_error("MCP methods must be registered before initialize");
    }
    std::lock_guard lk(impl_->tools_mu);
    auto [it, inserted] = impl_->methods.emplace(std::move(method), std::move(handler));
    if (!inserted) {
        throw std::invalid_argument("duplicate MCP method name: " + it->first);
    }
}

void MCPServer::register_extension(std::string identifier, json settings) {
    if (identifier.empty() || !settings.is_object()) {
        throw std::invalid_argument("MCP extension requires an identifier and object settings");
    }
    if (impl_->initialize_seen.load(std::memory_order_acquire)) {
        throw std::logic_error("MCP extensions must be registered before initialize");
    }
    std::lock_guard lk(impl_->tools_mu);
    if (impl_->extensions.contains(identifier)) {
        throw std::invalid_argument("duplicate MCP extension: " + identifier);
    }
    impl_->extensions[identifier] = std::move(settings);
}

json MCPServer::handle_message(const json& envelope) {
    auto outcome = impl_->process(envelope, impl_->to_sinks(envelope));
    return outcome.deferred ? json(nullptr) : std::move(outcome.response);
}

void MCPServer::handle_message(const json& message, Reply reply) {
    auto shared = std::make_shared<Reply>(std::move(reply));
    auto outcome = impl_->process(message, [shared](std::optional<json> response) {
        (*shared)(std::move(response));
    });
    if (outcome.deferred) return;
    if (outcome.response.is_null()) (*shared)(std::nullopt);
    else (*shared)(std::move(outcome.response));
}

void MCPServer::run(std::istream& in, std::ostream& out) {
    if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("MCP server run loop is already active");
    }
    std::mutex output_mu;
    const auto write = [&out, &output_mu](const json& message) {
        std::lock_guard lk(output_mu);
        out << message.dump() << '\n';
        out.flush();
    };

    std::string line;
    while (!impl_->stopping.load(std::memory_order_acquire) && std::getline(in, line)) {
        if (line.empty()) continue;
        json message;
        try {
            message = json::parse(line);
        } catch (const json::parse_error& e) {
            std::cerr << "neograph MCP stdio parse error: " << e.what() << '\n';
            write(rpc_error(-32700, "Parse error", nullptr));
            continue;
        }
        // Every outcome is written as it comes: at once, or from a worker
        // when a tool call (or a batch holding one) completes. stop() below
        // waits for the workers, so none writes after this returns.
        try {
            handle_message(message, [&write](std::optional<json> outcome) {
                if (outcome) write(*outcome);
            });
        } catch (const std::exception& e) {
            std::cerr << "neograph MCP stdio internal error: " << e.what() << '\n';
            write(rpc_error(-32603, "Internal error", nullptr));
        }
    }
    stop();
    impl_->running.store(false, std::memory_order_release);
}

void MCPServer::run() {
    run(std::cin, std::cout);
}

void MCPServer::set_response_sink(ResponseSink sink) {
    set_response_sink(std::move(sink), NoResponseSink{});
}

void MCPServer::set_response_sink(ResponseSink sink, NoResponseSink no_response) {
    std::atomic_store_explicit(
        &impl_->sinks,
        std::shared_ptr<const Impl::Sinks>(
            std::make_shared<Impl::Sinks>(Impl::Sinks{std::move(sink), std::move(no_response)})),
        std::memory_order_release);
}

void MCPServer::stop() {
    if (!impl_ || impl_->stopping.exchange(true, std::memory_order_acq_rel)) return;
    {
        std::lock_guard lk(impl_->calls_mu);
        for (auto& [key, task] : impl_->active) {
            (void)key;
            task->cancel->cancel();
        }
    }
    impl_->calls_cv.notify_all();
    for (auto& worker : impl_->workers) {
        if (worker.joinable()) worker.join();
    }
    impl_->workers.clear();
}

bool MCPServer::initialized() const {
    return impl_->operational.load(std::memory_order_acquire);
}

bool MCPServer::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::string MCPServer::protocol_version() const {
    return impl_->protocol_version();
}

} // namespace neograph::mcp
