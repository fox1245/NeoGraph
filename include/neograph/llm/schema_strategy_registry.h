/**
 * @file llm/schema_strategy_registry.h
 * @brief Explicit-injection registry for SchemaProvider wire primitives.
 *
 * SchemaProvider keeps the implementation of each wire shape in the library,
 * while this registry lets an application give a schema a stable, local name
 * for one of those primitives. The registry deliberately contains no
 * callbacks: adding a name cannot smuggle arbitrary code into request or
 * response handling, and Python bindings do not need to keep foreign callable
 * objects alive across provider threads.
 */
#pragma once

#include <neograph/api.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::llm {

/**
 * @brief The part of a SchemaProvider schema that owns a wire primitive name.
 */
enum class SchemaStrategyFamily : std::uint8_t {
    SystemPrompt,
    ToolCall,
    ToolResult,
    ToolDefinition,
    Response,
    Stream,
};

/**
 * @brief Immutable-after-injection mapping from schema names to built-in wire primitives.
 *
 * A default-constructed registry contains every primitive understood by the
 * current SchemaProvider implementation. `register_alias` may be used while
 * assembling a provider configuration to add an application-specific name,
 * but only to an existing primitive in the same family. SchemaProvider copies
 * the injected registry before parsing, so later mutations of the caller's
 * object cannot change a live provider.
 *
 * This is intentionally a C++ value-level extension seam. It does not expose
 * function callbacks or Python registration; a new wire shape still requires
 * a reviewed library implementation and a corresponding primitive.
 */
class NEOGRAPH_API SchemaStrategyRegistry {
  public:
    SchemaStrategyRegistry();

    /**
     * @brief Return a registry containing the standard primitive names.
     */
    static SchemaStrategyRegistry standard();

    SchemaStrategyRegistry(const SchemaStrategyRegistry&) = default;
    SchemaStrategyRegistry& operator=(const SchemaStrategyRegistry&) = default;
    SchemaStrategyRegistry(SchemaStrategyRegistry&&) noexcept = default;
    SchemaStrategyRegistry& operator=(SchemaStrategyRegistry&&) noexcept = default;
    ~SchemaStrategyRegistry() = default;

    /**
     * @brief Add an application-specific alias for a built-in primitive.
     *
     * @param family Schema section that consumes the name.
     * @param alias Name used by the JSON schema.
     * @param primitive Canonical built-in primitive in the same family.
     * @throws std::invalid_argument for empty/oversized names, an unknown
     *         primitive, or an alias that is already registered.
     */
    void register_alias(SchemaStrategyFamily family,
                        std::string alias,
                        std::string primitive);

    /**
     * @brief Resolve a schema name to its canonical primitive name.
     * @throws std::invalid_argument when the name is not registered.
     */
    std::string resolve(SchemaStrategyFamily family,
                        std::string_view name) const;

    /**
     * @brief Test whether a name is registered in a family.
     */
    bool contains(SchemaStrategyFamily family, std::string_view name) const;

    /**
     * @brief Return all registered names in deterministic order.
     */
    std::vector<std::string> names(SchemaStrategyFamily family) const;

  private:
    using AliasMap = std::map<std::string, std::string>;
    std::map<SchemaStrategyFamily, AliasMap> aliases_;

    static const char* family_name(SchemaStrategyFamily family) noexcept;
    static bool is_builtin(SchemaStrategyFamily family,
                           std::string_view primitive) noexcept;
    static void validate_name(std::string_view value, std::string_view label);
};

}  // namespace neograph::llm
