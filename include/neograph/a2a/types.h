/**
 * @file a2a/types.h
 * @brief Core data types for the Agent-to-Agent (A2A) protocol.
 *
 * Mirrors the canonical proto / JSON schema at
 * https://github.com/a2aproject/A2A (specification/a2a.proto).
 *
 * Only the surface needed by the client is modelled here. The wire
 * format is JSON; structs round-trip through nlohmann::json via
 * `to_json`/`from_json` ADL hooks defined in src/a2a/types.cpp.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <optional>
#include <string>
#include <vector>

namespace neograph::a2a {

using json = neograph::json;

/// TaskState — exact strings from spec §6.3 (kebab-case).
enum class TaskState {
    Submitted,
    Working,
    InputRequired,
    Completed,
    Canceled,
    Failed,
    Rejected,
    AuthRequired,
    Unknown,
};

NEOGRAPH_API std::string task_state_to_string(TaskState s);
NEOGRAPH_API TaskState   task_state_from_string(std::string_view s);

/// Message role (spec §6.4).
enum class Role { User, Agent };

NEOGRAPH_API std::string role_to_string(Role r);
NEOGRAPH_API Role        role_from_string(std::string_view s);

/// One content fragment of a Message or Artifact.
/// Discriminated by `kind` ∈ {"text", "file", "data"}.
struct NEOGRAPH_API Part {
    std::string kind;        ///< "text" | "file" | "data"
    std::string text;        ///< populated when kind == "text"
    json        file;        ///< populated when kind == "file"  (FileWithBytes/Uri)
    json        data;        ///< populated when kind == "data"  (arbitrary JSON)
    json        metadata;    ///< optional extension bag

    /// Convenience: TextPart of given content.
    static Part text_part(std::string s);
};

/// A single conversational turn (spec §6.4).
struct NEOGRAPH_API Message {
    std::string         message_id;            ///< client-generated UUID, REQUIRED
    Role                role = Role::User;     ///< REQUIRED
    std::vector<Part>   parts;                 ///< REQUIRED, ≥1 element
    std::optional<std::string> task_id;        ///< server task id (omit on first send)
    std::optional<std::string> context_id;     ///< grouping id
    std::vector<std::string>   reference_task_ids;
    std::vector<std::string>   extensions;
    json                metadata;              ///< optional bag
    std::string         kind = "message";      ///< discriminator, fixed
};

/// Generated output (spec §6.7).
struct NEOGRAPH_API Artifact {
    std::string         artifact_id;
    std::vector<Part>   parts;
    std::optional<std::string> name;
    std::optional<std::string> description;
    json                metadata;
};

/// Status snapshot of a running Task (spec §6.5).
struct NEOGRAPH_API TaskStatus {
    TaskState              state = TaskState::Submitted;
    std::optional<Message> message;
    std::optional<std::string> timestamp;   ///< RFC3339 UTC
};

/// A unit of work tracked by the agent (spec §6.1).
struct NEOGRAPH_API Task {
    std::string             id;             ///< server-generated, REQUIRED
    std::string             context_id;     ///< server-generated grouping id
    TaskStatus              status;
    std::vector<Artifact>   artifacts;
    std::vector<Message>    history;
    json                    metadata;
    std::string             kind = "task";
};

/// Optional knobs for `message/send` (spec §7.2).
struct NEOGRAPH_API MessageSendConfiguration {
    std::vector<std::string>   accepted_output_modes;
    std::optional<bool>        blocking;
    std::optional<int>         history_length;
    /// Push notification config omitted — not relevant for the C++ client yet.
};

/// Params object for `message/send` and `message/stream`.
struct NEOGRAPH_API MessageSendParams {
    Message                                  message;
    std::optional<MessageSendConfiguration>  configuration;
    json                                     metadata;
};

/// Subset of AgentCard required to interact (spec §5.5).
/// We deserialise only fields the client actually consumes; unknown
/// fields are kept verbatim in `raw` for forward-compat.
struct NEOGRAPH_API AgentCard {
    std::string             name;
    std::string             description;
    std::string             url;                ///< primary endpoint
    std::string             version;
    std::string             protocol_version;   ///< e.g. "0.3.0"
    std::string             preferred_transport = "JSONRPC";
    std::vector<std::string> default_input_modes;
    std::vector<std::string> default_output_modes;

    bool                    streaming                       = false;
    bool                    push_notifications              = false;
    bool                    extended_card                   = false;
    bool                    supports_authenticated_extended = false;

    /// Parsed skill names — full skill objects available in `raw["skills"]`.
    std::vector<std::string> skill_names;

    /// Full original document as received. Preserves fields we don't yet model.
    json raw;
};

// ---------------------------------------------------------------------------
// JSON adapters (defined in src/a2a/types.cpp).
// ---------------------------------------------------------------------------
NEOGRAPH_API void to_json(json& j, const Part& p);
NEOGRAPH_API void from_json(const json& j, Part& p);

NEOGRAPH_API void to_json(json& j, const Message& m);
NEOGRAPH_API void from_json(const json& j, Message& m);

NEOGRAPH_API void to_json(json& j, const Artifact& a);
NEOGRAPH_API void from_json(const json& j, Artifact& a);

NEOGRAPH_API void to_json(json& j, const TaskStatus& s);
NEOGRAPH_API void from_json(const json& j, TaskStatus& s);

NEOGRAPH_API void to_json(json& j, const Task& t);
NEOGRAPH_API void from_json(const json& j, Task& t);

NEOGRAPH_API void to_json(json& j, const MessageSendConfiguration& c);
NEOGRAPH_API void from_json(const json& j, MessageSendConfiguration& c);

NEOGRAPH_API void to_json(json& j, const MessageSendParams& p);
NEOGRAPH_API void from_json(const json& j, MessageSendParams& p);

NEOGRAPH_API void to_json(json& j, const AgentCard& c);
NEOGRAPH_API void from_json(const json& j, AgentCard& c);

} // namespace neograph::a2a
