#include <neograph/mcp/hook_rpc.h>

#include <neograph/json.h>

#include <stdexcept>

namespace neograph::mcp { namespace {
asio::awaitable<std::string> forward(MCPClient& client, RpcRequest request) {
    if (request.cancel_token && request.cancel_token->is_cancelled()) {
        throw std::runtime_error("hook RPC cancelled before transport dispatch");
    }
    if (std::chrono::steady_clock::now() >= request.deadline) {
        throw std::runtime_error("hook RPC deadline elapsed before transport dispatch");
    }
    json envelope;
    try { envelope = json::parse(request.request); }
    catch (const std::exception&) { throw RpcProtocolError("Hook RPC transport received invalid request JSON"); }
    if (!envelope.is_object() || envelope.value("jsonrpc", "") != "2.0"
        || !envelope["id"].is_string() || !envelope["method"].is_string()
        || !envelope.contains("params")) {
        throw RpcProtocolError("Hook RPC transport received invalid JSON-RPC envelope");
    }
    const auto id = envelope["id"];
    json result;
    try {
        result = co_await client.rpc_call_async(envelope["method"].get<std::string>(), envelope["params"],
                                                request.deadline, request.cancel_token);
    } catch (const std::exception& error) {
        if ((request.cancel_token && request.cancel_token->is_cancelled())
            || std::chrono::steady_clock::now() >= request.deadline) {
            throw;
        }
        // Preserve the outer hook protocol even when the MCP server uses a
        // transport-specific error representation.
        co_return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {
            {"code", -32000}, {"message", error.what()}}}}.dump();
    }
    if (request.cancel_token && request.cancel_token->is_cancelled()) {
        throw std::runtime_error("hook RPC cancelled");
    }
    if (std::chrono::steady_clock::now() >= request.deadline) {
        throw std::runtime_error("hook RPC deadline elapsed");
    }
    co_return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}
} // namespace

StdioJsonRpcTransport::StdioJsonRpcTransport(std::shared_ptr<MCPClient> client)
    : client_(std::move(client)) {
    if (!client_) throw std::invalid_argument("Hook RPC transport requires an MCP client");
}
asio::awaitable<std::string> StdioJsonRpcTransport::request_async(RpcRequest request) {
    co_return co_await forward(*client_, std::move(request));
}

HttpJsonRpcTransport::HttpJsonRpcTransport(std::shared_ptr<MCPClient> client)
    : client_(std::move(client)) {
    if (!client_) throw std::invalid_argument("Hook RPC transport requires an MCP client");
}
asio::awaitable<std::string> HttpJsonRpcTransport::request_async(RpcRequest request) {
    co_return co_await forward(*client_, std::move(request));
}
} // namespace neograph::mcp
