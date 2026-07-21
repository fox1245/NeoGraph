#include <neograph/mcp/json_schema.h>
#include <neograph/mcp/server.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::mcp {
namespace {

json rpc_result(json result, const json& id) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpc_error(int code, std::string message, const json& id, json data = nullptr) {
    json error = {{"code", code}, {"message", std::move(message)}};
    if (!data.is_null()) error["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

CallToolResult tool_error(std::string message) {
    CallToolResult result;
    result.content = json::array({{{"type", "text"}, {"text", std::move(message)}}});
    result.is_error = true;
    return result;
}

bool valid_request_id(const json& id) {
    return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

std::string request_key(const json& id) {
    if (id.is_string()) return "s:" + id.get<std::string>();
    return "n:" + id.dump();
}

void validate_definition(const ToolDefinition& definition) {
    ToolDefinition::from_json(definition.to_json());
    validate_json_schema(definition.input_schema, "inputSchema");
    if (!definition.output_schema.is_null()) {
        validate_json_schema(definition.output_schema, "outputSchema");
    }
}

} // namespace

struct MCPServer::Impl {
    struct RegisteredTool {
        ToolDefinition definition;
        ToolHandler handler;
        RawToolHandler raw_handler;
    };

    struct CallTask {
        json id;
        std::string key;
        json arguments;
        json                                meta;
        RegisteredTool tool;
        std::shared_ptr<graph::CancelToken> cancel;
    };

    explicit Impl(MCPServerConfig value) : config(std::move(value)) {}

    MCPServerConfig config;
    std::atomic<bool> initialize_seen{false};
    std::atomic<bool> operational{false};
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::mutex tools_mu;
    std::map<std::string, RegisteredTool> tools;
    std::map<std::string, MethodHandler>  methods;
    json                                  extensions        = json::object();
    json                                  client_extensions = json::object();

    std::shared_ptr<ResponseSink> sink;

    std::mutex calls_mu;
    std::condition_variable calls_cv;
    std::deque<std::shared_ptr<CallTask>> queue;
    std::unordered_map<std::string, std::shared_ptr<CallTask>> active;
    std::vector<std::thread> workers;

    void emit(const json& envelope) noexcept {
        try {
            auto current = std::atomic_load_explicit(&sink, std::memory_order_acquire);
            if (current && *current) (*current)(envelope);
        } catch (...) {
            // A transport failure cannot safely be reported through itself.
        }
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
                        if (result.structured_content.is_null()) {
                            throw std::invalid_argument(
                                "tool advertised outputSchema but returned no structuredContent");
                        }
                        validate_json_value(result.structured_content,
                                            task->tool.definition.output_schema,
                                            "MCP tool structuredContent", "$");
                    }
                    result_json = result.to_json();
                }
                task->cancel->throw_if_cancelled("after tool execution");
            } catch (const graph::CancelledException& e) {
                result_json = tool_error(e.what()).to_json();
            } catch (const std::exception& e) {
                result_json =
                    tool_error(std::string("Tool execution failed: ") + e.what()).to_json();
            } catch (...) {
                result_json = tool_error("Tool execution failed: unknown exception").to_json();
            }

            {
                std::lock_guard lk(calls_mu);
                active.erase(task->key);
            }
            emit(rpc_result(std::move(result_json), task->id));
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

        json result = {
            {"protocolVersion", MCP_PROTOCOL_VERSION},
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

        json listed = json::array();
        {
            std::lock_guard lk(tools_mu);
            for (const auto& [name, tool] : tools) {
                (void)name;
                listed.push_back(tool.definition.to_json());
            }
        }
        return rpc_result({{"tools", std::move(listed)}}, id);
    }

    json handle_tools_call(const json& params, const json& id) {
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
                return rpc_result(tool_error("Unknown tool: " + name).to_json(), id);
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
        if (task) task->cancel->cancel();
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
    validate_definition(definition);
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
    validate_definition(definition);
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
    if (!envelope.is_object() || envelope.value("jsonrpc", "") != "2.0") {
        return rpc_error(-32600, "Invalid JSON-RPC 2.0 request", nullptr);
    }
    if (!envelope.contains("method")) {
        // The server currently sends no requests, so inbound responses have no
        // waiter. Valid response envelopes are ignored rather than answered.
        if (envelope.contains("id") &&
            (envelope.contains("result") || envelope.contains("error"))) {
            return nullptr;
        }
        return rpc_error(-32600, "JSON-RPC request is missing method", nullptr);
    }
    if (!envelope["method"].is_string()) {
        return rpc_error(-32600, "JSON-RPC method must be a string", nullptr);
    }

    const bool notification = !envelope.contains("id");
    json id = notification ? json(nullptr) : envelope["id"];
    if (!notification && !valid_request_id(id)) {
        return rpc_error(-32600, "JSON-RPC id must be a string or integer", nullptr);
    }
    const auto method = envelope["method"].get<std::string>();
    const json params = envelope.value("params", json(nullptr));

    if (notification) {
        if (method == "notifications/initialized") {
            if (impl_->initialize_seen.load(std::memory_order_acquire)) {
                impl_->operational.store(true, std::memory_order_release);
            }
        } else if (method == "notifications/cancelled") {
            impl_->handle_cancel(params);
        }
        return nullptr;
    }

    if (method == "ping") return rpc_result(json::object(), id);
    if (method == "initialize") return impl_->handle_initialize(params, id);
    if (!impl_->operational.load(std::memory_order_acquire)) {
        return rpc_error(-32002, "MCP server is not initialized", id);
    }
    if (method == "tools/list") return impl_->handle_tools_list(params, id);
    if (method == "tools/call") return impl_->handle_tools_call(params, id);
    MCPServer::MethodHandler extension_handler;
    {
        std::lock_guard lk(impl_->tools_mu);
        auto            it = impl_->methods.find(method);
        if (it != impl_->methods.end()) extension_handler = it->second;
    }
    if (extension_handler) {
        try {
            json context = {{"extensions", impl_->client_extensions}};
            return rpc_result(extension_handler(params, context), id);
        } catch (const std::invalid_argument& error) {
            return rpc_error(-32602, error.what(), id);
        } catch (const std::exception& error) {
            return rpc_error(-32603, error.what(), id);
        }
    }
    return rpc_error(-32601, "Method not found: " + method, id);
}

void MCPServer::run(std::istream& in, std::ostream& out) {
    if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("MCP server run loop is already active");
    }
    std::mutex output_mu;
    set_response_sink([&out, &output_mu](const json& response) {
        std::lock_guard lk(output_mu);
        out << response.dump() << '\n';
        out.flush();
    });

    std::string line;
    while (!impl_->stopping.load(std::memory_order_acquire) && std::getline(in, line)) {
        if (line.empty()) continue;
        json response;
        try {
            response = handle_message(json::parse(line));
        } catch (const json::parse_error& e) {
            std::cerr << "neograph MCP stdio parse error: " << e.what() << '\n';
            response = rpc_error(-32700, "Parse error", nullptr);
        } catch (const std::exception& e) {
            std::cerr << "neograph MCP stdio internal error: " << e.what() << '\n';
            response = rpc_error(-32603, "Internal error", nullptr);
        }
        if (!response.is_null()) {
            std::lock_guard lk(output_mu);
            out << response.dump() << '\n';
            out.flush();
        }
    }
    stop();
    impl_->running.store(false, std::memory_order_release);
}

void MCPServer::run() {
    run(std::cin, std::cout);
}

void MCPServer::set_response_sink(ResponseSink sink) {
    std::atomic_store_explicit(&impl_->sink, std::make_shared<ResponseSink>(std::move(sink)),
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

} // namespace neograph::mcp
