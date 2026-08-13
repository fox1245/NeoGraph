#include <neograph/llm/schema_strategy_registry.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace neograph::llm {
namespace {

constexpr std::size_t kMaximumStrategyNameBytes = 128;

bool is_valid_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumStrategyNameBytes) return false;
    return value.find('\0') == std::string_view::npos;
}

bool in(std::string_view value,
        std::initializer_list<std::string_view> candidates) noexcept {
    return std::find(candidates.begin(), candidates.end(), value) != candidates.end();
}

}  // namespace

SchemaStrategyRegistry::SchemaStrategyRegistry()
    : aliases_{
          {SchemaStrategyFamily::SystemPrompt,
           {{"in_messages", "in_messages"},
            {"top_level", "top_level"},
            {"top_level_parts", "top_level_parts"}}},
          {SchemaStrategyFamily::ToolCall,
           {{"tool_calls_array", "tool_calls_array"},
            {"content_array", "content_array"},
            {"parts_array", "parts_array"},
            {"flat_items", "flat_items"}}},
          {SchemaStrategyFamily::ToolResult,
           {{"flat", "flat"},
            {"content_array", "content_array"},
            {"parts_array", "parts_array"},
            {"flat_item", "flat_item"}}},
          {SchemaStrategyFamily::ToolDefinition,
           {{"function", "function"},
            {"none", "none"},
            {"function_declarations", "function_declarations"},
            {"flat_function", "flat_function"}}},
          {SchemaStrategyFamily::Response,
           {{"choices_message", "choices_message"},
            {"content_array", "content_array"},
            {"candidates_parts", "candidates_parts"},
            {"output_array", "output_array"}}},
          {SchemaStrategyFamily::Stream,
           {{"sse_data", "sse_data"}, {"sse_events", "sse_events"}}}}
{}

SchemaStrategyRegistry SchemaStrategyRegistry::standard() {
    return SchemaStrategyRegistry{};
}

void SchemaStrategyRegistry::register_alias(SchemaStrategyFamily family,
                                             std::string alias,
                                             std::string primitive) {
    validate_name(alias, "schema strategy alias");
    validate_name(primitive, "schema strategy primitive");
    if (!is_builtin(family, primitive)) {
        throw std::invalid_argument(
            "unknown " + std::string(family_name(family))
            + " schema strategy primitive: " + primitive);
    }

    auto& family_aliases = aliases_[family];
    if (family_aliases.contains(alias)) {
        throw std::invalid_argument(
            "schema strategy alias already registered for "
            + std::string(family_name(family)) + ": " + alias);
    }
    family_aliases.emplace(std::move(alias), std::move(primitive));
}

std::string SchemaStrategyRegistry::resolve(SchemaStrategyFamily family,
                                            std::string_view name) const {
    const auto family_it = aliases_.find(family);
    if (family_it != aliases_.end()) {
        const auto it = family_it->second.find(std::string(name));
        if (it != family_it->second.end()) return it->second;
    }
    throw std::invalid_argument(
        "unknown " + std::string(family_name(family))
        + " schema strategy: " + std::string(name));
}

bool SchemaStrategyRegistry::contains(SchemaStrategyFamily family,
                                       std::string_view name) const {
    const auto family_it = aliases_.find(family);
    if (family_it == aliases_.end()) return false;
    return family_it->second.find(std::string(name)) != family_it->second.end();
}

std::vector<std::string> SchemaStrategyRegistry::names(
    SchemaStrategyFamily family) const {
    std::vector<std::string> result;
    const auto family_it = aliases_.find(family);
    if (family_it == aliases_.end()) return result;
    result.reserve(family_it->second.size());
    for (const auto& [name, primitive] : family_it->second) {
        (void)primitive;
        result.push_back(name);
    }
    return result;
}

const char* SchemaStrategyRegistry::family_name(
    SchemaStrategyFamily family) noexcept {
    switch (family) {
        case SchemaStrategyFamily::SystemPrompt: return "system_prompt";
        case SchemaStrategyFamily::ToolCall: return "tool_call";
        case SchemaStrategyFamily::ToolResult: return "tool_result";
        case SchemaStrategyFamily::ToolDefinition: return "tool_definition";
        case SchemaStrategyFamily::Response: return "response";
        case SchemaStrategyFamily::Stream: return "stream";
    }
    return "unknown";
}

bool SchemaStrategyRegistry::is_builtin(SchemaStrategyFamily family,
                                        std::string_view primitive) noexcept {
    switch (family) {
        case SchemaStrategyFamily::SystemPrompt:
            return in(primitive, {"in_messages", "top_level", "top_level_parts"});
        case SchemaStrategyFamily::ToolCall:
            return in(primitive, {"tool_calls_array", "content_array", "parts_array", "flat_items"});
        case SchemaStrategyFamily::ToolResult:
            return in(primitive, {"flat", "content_array", "parts_array", "flat_item"});
        case SchemaStrategyFamily::ToolDefinition:
            return in(primitive, {"function", "none", "function_declarations", "flat_function"});
        case SchemaStrategyFamily::Response:
            return in(primitive, {"choices_message", "content_array", "candidates_parts", "output_array"});
        case SchemaStrategyFamily::Stream:
            return in(primitive, {"sse_data", "sse_events"});
    }
    return false;
}

void SchemaStrategyRegistry::validate_name(std::string_view value,
                                           std::string_view label) {
    if (!is_valid_name(value)) {
        throw std::invalid_argument(std::string(label)
                                    + " must be 1..128 bytes and contain no NUL bytes");
    }
}

}  // namespace neograph::llm
