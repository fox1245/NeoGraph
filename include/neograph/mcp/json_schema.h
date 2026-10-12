/**
 * @file mcp/json_schema.h
 * @brief JSON Schema 2020-12 validation shared by NeoGraph's MCP server,
 *        MCP client and Harness.
 *
 * MCP 2025-11-25 ("JSON Schema Usage") makes 2020-12 the default dialect of
 * every tool schema. Supported: the core, applicator, unevaluated and
 * validation vocabularies, boolean schemas and local `$ref` (JSON Pointer
 * fragments, `$anchor` names, `$id` resources in the same document); `format`
 * and the content and meta-data keywords are annotations. A schema that names
 * another dialect in `$schema`, uses `$dynamicRef` or `$recursiveRef`,
 * references another document, or has a `pattern` std::regex cannot compile
 * is refused by validate_json_schema().
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <string>

namespace neograph::mcp {

/// Check that @p schema is a JSON Schema 2020-12 document this validator
/// supports; throw std::invalid_argument naming the offending keyword.
NEOGRAPH_API void validate_json_schema(const json& schema,
                                       const std::string& path = "$schema");

/// Validate @p value against @p schema (checked as above first). Throw
/// std::invalid_argument whose message starts with "<subject> at <path>".
NEOGRAPH_API void validate_json_value(const json& value, const json& schema,
                                      const std::string& subject = "JSON value",
                                      const std::string& path = "$");

} // namespace neograph::mcp
