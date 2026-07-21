/**
 * @file mcp/json_schema.h
 * @brief Small JSON Schema validator shared by MCP server services.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <string>

namespace neograph::mcp {

/// Validate the supported JSON Schema subset and throw invalid_argument.
NEOGRAPH_API void validate_json_schema(const json& schema,
                                       const std::string& path = "$schema");

/// Validate a value against the supported JSON Schema subset.
NEOGRAPH_API void validate_json_value(const json& value, const json& schema,
                                      const std::string& subject = "JSON value",
                                      const std::string& path = "$");

} // namespace neograph::mcp
