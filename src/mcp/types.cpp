#include <neograph/mcp/types.h>

namespace neograph::mcp {

MCPError::MCPError(int code, std::string message, json data)
  : std::runtime_error(std::move(message))
  , code_(code)
  , data_(std::move(data))
{
}

InitializeResult InitializeResult::from_json(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("MCP initialize result must be an object");
    }
    InitializeResult result;
    result.raw = value;
    if (!value.contains("protocolVersion")
        || !value["protocolVersion"].is_string()
        || value["protocolVersion"].get<std::string>().empty()) {
        throw std::invalid_argument(
            "MCP initialize result must contain a non-empty protocolVersion");
    }
    if (!value.contains("capabilities") || !value["capabilities"].is_object()
        || !value.contains("serverInfo") || !value["serverInfo"].is_object()) {
        throw std::invalid_argument(
            "MCP initialize result must contain capabilities and serverInfo objects");
    }
    if (value.contains("instructions") && !value["instructions"].is_string()) {
        throw std::invalid_argument("MCP initialize instructions must be a string");
    }
    result.protocol_version = value["protocolVersion"].get<std::string>();
    result.capabilities = value["capabilities"];
    result.server_info = value["serverInfo"];
    result.instructions = value.value("instructions", "");
    return result;
}

ToolDefinition ToolDefinition::from_json(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("MCP tool definition must be an object");
    }
    ToolDefinition result;
    result.raw = value;
    result.name = value.value("name", "");
    result.title = value.value("title", "");
    result.description = value.value("description", "");
    result.icons = value.value("icons", json::array());
    result.input_schema = value.value("inputSchema", json::object());
    if (value.contains("outputSchema")) result.output_schema = value["outputSchema"];
    result.annotations = value.value("annotations", json::object());
    result.execution = value.value("execution", json::object());
    result.meta = value.value("_meta", json::object());
    if (result.name.empty()) {
        throw std::invalid_argument("MCP tool definition is missing name");
    }
    if (!result.input_schema.is_object()) {
        throw std::invalid_argument("MCP tool inputSchema must be an object");
    }
    if (!result.output_schema.is_null() && !result.output_schema.is_object()) {
        throw std::invalid_argument("MCP tool outputSchema must be an object");
    }
    if (!result.icons.is_array() || !result.annotations.is_object()
        || !result.execution.is_object() || !result.meta.is_object()) {
        throw std::invalid_argument("MCP tool metadata has an invalid type");
    }
    return result;
}

json ToolDefinition::to_json() const {
    json value = json::object();
    if (raw.is_object()) {
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            const auto& key = it.key();
            if (key != "name" && key != "title" && key != "description"
                && key != "icons" && key != "inputSchema"
                && key != "outputSchema" && key != "annotations"
                && key != "execution" && key != "_meta") {
                value[key] = it.value();
            }
        }
    }
    value["name"] = name;
    value["inputSchema"] = input_schema;
    if (!title.empty()) value["title"] = title;
    if (!description.empty()) value["description"] = description;
    if (!icons.empty()) value["icons"] = icons;
    if (!output_schema.is_null()) value["outputSchema"] = output_schema;
    if (!annotations.empty()) value["annotations"] = annotations;
    if (!execution.empty()) value["execution"] = execution;
    if (!meta.empty()) value["_meta"] = meta;
    return value;
}

ListToolsPage ListToolsPage::from_json(const json& value) {
    if (!value.is_object() || !value.contains("tools")
        || !value["tools"].is_array()) {
        throw std::invalid_argument("MCP tools/list result must contain a tools array");
    }
    ListToolsPage result;
    result.raw = value;
    for (const auto& tool : value["tools"]) {
        result.tools.push_back(ToolDefinition::from_json(tool));
    }
    if (value.contains("nextCursor") && !value["nextCursor"].is_null()) {
        if (!value["nextCursor"].is_string()) {
            throw std::invalid_argument("MCP tools/list nextCursor must be a string");
        }
        result.next_cursor = value["nextCursor"].get<std::string>();
    }
    result.meta = value.value("_meta", json::object());
    if (!result.meta.is_object()) {
        throw std::invalid_argument("MCP tools/list _meta must be an object");
    }
    return result;
}

CallToolResult CallToolResult::from_json(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("MCP tools/call result must be an object");
    }
    CallToolResult result;
    result.raw = value;
    result.content = value.value("content", json::array());
    if (value.contains("structuredContent")) {
        result.structured_content = value["structuredContent"];
    }
    result.is_error = value.value("isError", false);
    result.meta = value.value("_meta", json::object());
    if (!result.content.is_array()) {
        throw std::invalid_argument("MCP tools/call content must be an array");
    }
    if (!result.structured_content.is_null()
        && !result.structured_content.is_object()) {
        throw std::invalid_argument(
            "MCP tools/call structuredContent must be an object");
    }
    if (!result.meta.is_object()) {
        throw std::invalid_argument("MCP tools/call _meta must be an object");
    }
    return result;
}

json CallToolResult::to_json() const {
    json value = json::object();
    if (raw.is_object()) {
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            const auto& key = it.key();
            if (key != "content" && key != "structuredContent"
                && key != "isError" && key != "_meta") {
                value[key] = it.value();
            }
        }
    }
    value["content"] = content;
    if (!structured_content.is_null()) {
        value["structuredContent"] = structured_content;
    }
    value["isError"] = is_error;
    if (!meta.empty()) value["_meta"] = meta;
    return value;
}

} // namespace neograph::mcp
