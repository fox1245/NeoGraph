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

#include <chrono>
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

class NEOGRAPH_API MCPError : public std::runtime_error {
  public:
    MCPError(int code, std::string message, json data = nullptr);

    int         code() const noexcept { return code_; }
    const json& data() const noexcept { return data_; }

  private:
    int  code_;
    json data_;
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
};

} // namespace neograph::mcp
