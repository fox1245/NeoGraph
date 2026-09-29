/**
 * @file mcp/types.h
 * @brief Complete typed values for the supported MCP tools client surface.
 *
 * Wire target: MCP 2025-11-25. Tool content stays as JSON so text, image,
 * audio, resource, and resource_link blocks survive without a lossy projection.
 * Source: https://modelcontextprotocol.io/specification/2025-11-25/server/tools
 * Reviewed 2026-07-20 for NeoGraph issue #147.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/types.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neograph::mcp {

using HeaderList = std::vector<std::pair<std::string, std::string>>;
using HeaderProvider = std::function<HeaderList()>;

struct NEOGRAPH_API MCPClientConfig {
    std::chrono::milliseconds request_timeout{std::chrono::seconds(30)};
    HeaderList                headers;
    HeaderProvider            header_provider;
};

/** Explicit subprocess boundary for local credentialless MCP adoption. */
struct NEOGRAPH_API StdioClientConfig {
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    /// Replacement environment. An empty list is intentionally empty; names
    /// must be unique (case-insensitively on Windows), nonempty, and contain
    /// neither '=' nor NUL. Values and argv must not contain NUL.
    std::vector<std::pair<std::string, std::string>> environment;
    bool replace_environment = false;
    std::size_t max_frame_bytes = 16 * 1024 * 1024;
    /// Cumulative stderr capture budget; exceeding it aborts the process tree.
    /// Captured bytes are never copied into exception messages or host stderr.
    std::size_t max_stderr_bytes = 64 * 1024;
    /// Startup covers launch through the initialized notification. Hardened
    /// timeout/cancellation/protocol failures permanently terminate the session.
    std::chrono::milliseconds startup_timeout{10000};
    std::chrono::milliseconds request_timeout{30000};
};

class NEOGRAPH_API MCPError : public std::runtime_error {
  public:
    MCPError(int code, std::string message, json data = nullptr);

    int         code() const noexcept { return code_; }
    const json& data() const noexcept { return data_; }

  private:
    int  code_;
    json data_;
};
/// Transport and wire failures have one classification on both HTTP and stdio.
/// A JSON-RPC error response is instead MCPError, preserving its server code.
enum class MCPFailure {
    connection, timeout, cancelled, shutdown, http_status, protocol
};

class NEOGRAPH_API MCPTransportError : public std::runtime_error {
  public:
    MCPTransportError(MCPFailure failure, std::string message, int http_status = 0);
    MCPFailure failure() const noexcept { return failure_; }
    int http_status() const noexcept { return http_status_; }

  private:
    MCPFailure failure_;
    int http_status_;
};


struct NEOGRAPH_API InitializeResult {
    std::string protocol_version;
    json        capabilities = json::object();
    json        server_info = json::object();
    std::string instructions;
    json        raw = json::object();

    static InitializeResult from_json(const json& value);
};

struct NEOGRAPH_API ToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    json        icons = json::array();
    json        input_schema = json::object();
    json        output_schema;
    json        annotations = json::object();
    json        execution = json::object();
    json        meta = json::object();
    json        raw = json::object();

    static ToolDefinition from_json(const json& value);
    json to_json() const;
};

struct NEOGRAPH_API ListToolsPage {
    std::vector<ToolDefinition> tools;
    std::optional<std::string>  next_cursor;
    json                        meta = json::object();
    json                        raw = json::object();

    static ListToolsPage from_json(const json& value);
};

struct NEOGRAPH_API CallToolResult {
    json content = json::array();
    json structured_content;
    bool is_error = false;
    json meta = json::object();
    json raw = json::object();

    static CallToolResult from_json(const json& value);
    json to_json() const;
};

} // namespace neograph::mcp
