/**
 * @file program/command.h
 * @brief Closed, versioned command values emitted by JavaScript control code.
 *
 * A JavaScript command is a host-owned value.  JavaScript can obtain one only
 * from a sealed `ng` constructor; the runtime never treats an arbitrary JSON
 * object with command-shaped fields as executable.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class JavaScriptCommandKind : std::uint8_t {
    CallCore,
    Spawn,
    Await,
    Join,
    Emit,
    Checkpoint,
    CancelScope,
    HostCapability,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(JavaScriptCommandKind kind) noexcept;
NEOGRAPH_PROGRAM_API JavaScriptCommandKind
javascript_command_kind_from_string(std::string_view value);

/** Protocol version carried by every JavaScript command. */
inline constexpr std::uint32_t JAVASCRIPT_COMMAND_PROTOCOL_VERSION = 1;
/** Maximum number of nested await/join command envelopes, including the root. */
inline constexpr std::size_t JAVASCRIPT_COMMAND_MAX_STRUCTURED_DEPTH = 32;
/** Maximum total command envelopes reachable through one structured command. */
inline constexpr std::size_t JAVASCRIPT_COMMAND_MAX_AGGREGATE_MEMBERS = 4096;

/** Built-in import slots owned by the versioned `ng` command module. */
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_CALL_CORE       = 0;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_SPAWN          = 1;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_AWAIT          = 2;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_JOIN           = 3;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_EMIT           = 4;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_CHECKPOINT     = 5;
inline constexpr std::uint32_t JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE   = 6;

/**
 * Immutable host-owned JavaScript command.
 *
 * `from_json` is the strict interchange boundary used by compatibility and
 * diagnostic code.  The QuickJS bridge additionally requires the opaque
 * sealed value created by an `ng` constructor before it will dispatch a
 * command yielded by a generator.
 */
class NEOGRAPH_PROGRAM_API JavaScriptCommand final {
public:
    static constexpr std::uint32_t PROTOCOL_VERSION = JAVASCRIPT_COMMAND_PROTOCOL_VERSION;

    JavaScriptCommand(const JavaScriptCommand&)            = default;
    JavaScriptCommand& operator=(const JavaScriptCommand&) = default;
    JavaScriptCommand(JavaScriptCommand&&) noexcept        = default;
    JavaScriptCommand& operator=(JavaScriptCommand&&) noexcept = default;
    ~JavaScriptCommand();

    /** Strictly decode a canonical command object. */
    static JavaScriptCommand from_json(const json& value);

    /** Construct a host-owned command after applying the same validation. */
    static JavaScriptCommand make(std::uint32_t       protocol_version,
                                  JavaScriptCommandKind kind,
                                  std::uint32_t       import_slot,
                                  std::string         source_site,
                                  json                arguments);

    static JavaScriptCommand call_core(std::string source_site,
                                       std::string core_name,
                                       json        input);
    static JavaScriptCommand spawn(std::string source_site,
                                   std::string child_binding,
                                   json        input);
    static JavaScriptCommand await(std::string source_site,
                                   JavaScriptCommand child,
                                   std::uint64_t timeout_ms = 0);
    static JavaScriptCommand join(std::string                   source_site,
                                  std::string                   mode,
                                  std::vector<JavaScriptCommand> members,
                                  std::uint64_t                 required_successes = 0,
                                  std::uint64_t                 max_in_flight = 0,
                                  std::string                   failure_policy = {});
    static JavaScriptCommand emit(std::string source_site, json value);
    static JavaScriptCommand checkpoint(std::string source_site, json value);
    static JavaScriptCommand cancel_scope(std::string source_site,
                                          std::string scope,
                                          std::string reason = {});
    static JavaScriptCommand host_capability(std::string source_site,
                                             std::uint32_t import_slot,
                                             json            input);

    std::uint32_t          protocol_version() const noexcept;
    JavaScriptCommandKind  kind() const noexcept;
    std::uint32_t          import_slot() const noexcept;
    const std::string&     source_site() const noexcept;
    /** Returns an owned copy, so callers cannot mutate the sealed command. */
    json                    arguments() const;
    /** Canonical JSON representation with all required protocol fields. */
    json                    to_json() const;

    bool operator==(const JavaScriptCommand& other) const;
    bool operator!=(const JavaScriptCommand& other) const { return !(*this == other); }

private:
    struct Impl;
    explicit JavaScriptCommand(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
