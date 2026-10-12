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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::mcp {

/// The MCP revisions NeoGraph's server answers and its client accepts, newest
/// first, as the official SDKs do. The server negotiates the revision a client
/// asks for when it is one of these, and the newest otherwise; the client asks
/// for the first and disconnects from a server that answers any other. A
/// 2024-11-05 peer is reached over stdio or Streamable HTTP; the older
/// HTTP+SSE transport is not spoken.
inline constexpr std::array<std::string_view, 4> MCP_SUPPORTED_PROTOCOL_VERSIONS = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

inline bool is_supported_protocol_version(std::string_view version) noexcept {
    return std::find(MCP_SUPPORTED_PROTOCOL_VERSIONS.begin(),
                     MCP_SUPPORTED_PROTOCOL_VERSIONS.end(),
                     version) != MCP_SUPPORTED_PROTOCOL_VERSIONS.end();
}

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
    /// Environment entries for the server. With replace_environment, they are
    /// its whole environment (an empty list is intentionally empty). Without
    /// it, the server inherits this process's environment and these entries
    /// are added or override inherited ones of the same name. Names must be
    /// unique (case-insensitively on Windows), nonempty, and contain neither
    /// '=' nor NUL. Values and argv must not contain NUL. A PATH entry is
    /// also where argv[0] is looked up.
    std::vector<std::pair<std::string, std::string>> environment;
    bool replace_environment = false;
    std::size_t max_frame_bytes = 16 * 1024 * 1024;
    /// The session drains the server's stderr for as long as it runs and keeps
    /// its last max_stderr_bytes bytes; stderr output never ends the session
    /// (MCP: a client SHOULD NOT treat stderr as an error). Captured bytes are
    /// never copied into exception messages or host stderr.
    std::size_t max_stderr_bytes = 64 * 1024;
    /// Startup covers launch through the initialized notification. Hardened
    /// timeout/cancellation/protocol failures permanently terminate the session.
    std::chrono::milliseconds startup_timeout{10000};
    std::chrono::milliseconds request_timeout{30000};
    /// After a cancelled or timed-out request the session sends
    /// notifications/cancelled, waits this long for the server to stop, and
    /// then terminates the process tree (stdin closed first, then SIGTERM,
    /// then SIGKILL).
    std::chrono::milliseconds cancel_grace{500};
};

class NEOGRAPH_API MCPError : public std::runtime_error {
  public:
    /// @p http_status is the HTTP status that carried the error response, or
    /// 0 (stdio, or an HTTP 200 response).
    MCPError(int code, std::string message, json data = nullptr, int http_status = 0);

    int         code() const noexcept { return code_; }
    const json& data() const noexcept { return data_; }
    int         http_status() const noexcept { return http_status_; }

  private:
    int  code_;
    json data_;
    int  http_status_ = 0;
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
    /// MCP requires an object schema with `"type": "object"`; a tool that takes
    /// no arguments keeps this default. MCPServer reads an empty object as the
    /// default too.
    json        input_schema = json{{"type", "object"}};
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
