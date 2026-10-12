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

/// A2A wire dialect — the JSON encoding of the data model.
///
/// - `V0_3`: A2A 0.3 JSON Schema form. `kind` discriminators on Message /
///   Task / Part / stream events, lower-case role (`user`, `agent`) and
///   kebab-case task states (`input-required`), slash-form JSON-RPC methods
///   (`message/send`), `FilePart.file = {bytes|uri, mimeType, name}`.
/// - `V1_0`: A2A 1.0 protobuf ProtoJSON form (`lf.a2a.v1`). No `kind`
///   discriminators, flat Parts (`{"text"}` / `{"raw"|"url", "filename",
///   "mediaType"}` / `{"data"}`), `ROLE_USER` / `TASK_STATE_WORKING` enums,
///   PascalCase methods (`SendMessage`) and `{"task"}` / `{"message"}` /
///   `{"statusUpdate"}` / `{"artifactUpdate"}` result wrappers.
///
/// Serialisation is dialect-selected (`to_json(j, value, dialect)`); the
/// two-argument `to_json` overloads emit `V0_3`. Parsing (`from_json`) is
/// dialect-tolerant and accepts either encoding.
enum class WireDialect { V0_3, V1_0 };

/// TaskState — kebab-case strings on the 0.3 wire, `TASK_STATE_*` on 1.0.
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
NEOGRAPH_API std::string task_state_to_string(TaskState s, WireDialect dialect);
/// Accepts both the kebab-case (0.3) and `TASK_STATE_*` (1.0) spellings.
NEOGRAPH_API TaskState   task_state_from_string(std::string_view s);

/// Message role (spec §6.4).
enum class Role { User, Agent };

NEOGRAPH_API std::string role_to_string(Role r);
NEOGRAPH_API std::string role_to_string(Role r, WireDialect dialect);
/// Accepts both `user`/`agent` (0.3) and `ROLE_USER`/`ROLE_AGENT` (1.0).
NEOGRAPH_API Role        role_from_string(std::string_view s);

/// One content fragment of a Message or Artifact.
/// Discriminated by `kind` ∈ {"text", "file", "data"} in the model; on the
/// 1.0 wire the discriminator is implicit (which of text/raw/url/data is set).
struct NEOGRAPH_API Part {
    std::string kind;        ///< "text" | "file" | "data"
    std::string text;        ///< populated when kind == "text"
    /// populated when kind == "file". Always the 0.3 `FileWithBytes`/
    /// `FileWithUri` shape ({"bytes"|"uri", "mimeType", "name"}); the 1.0
    /// codec maps it to/from `raw` / `url` / `mediaType` / `filename`.
    json        file;
    json        data;        ///< populated when kind == "data"  (arbitrary JSON)
    json        metadata;    ///< optional extension bag
    /// 1.0 `Part.media_type` for text/data parts (e.g. "text/plain"). Empty
    /// when absent; never emitted on the 0.3 wire.
    std::string media_type;

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

/// SSE event emitted by `message/stream` and `tasks/resubscribe`
/// while a Task is running. Discriminated by `kind` ∈
/// {"status-update", "artifact-update"}.
///
/// `final` set to true on the last event of the stream — clients use
/// this to stop reading without closing the underlying SSE socket.
struct NEOGRAPH_API TaskStatusUpdateEvent {
    std::string  task_id;
    std::string  context_id;
    TaskStatus   status;
    bool         final = false;
    json         metadata;
    std::string  kind = "status-update";
};

struct NEOGRAPH_API TaskArtifactUpdateEvent {
    std::string  task_id;
    std::string  context_id;
    Artifact     artifact;
    bool         append    = false;
    bool         last_chunk = false;
    json         metadata;
    std::string  kind = "artifact-update";
};

/// Tagged union over the two streaming event types + the terminal
/// Task message. The client's stream callback receives one of these
/// per SSE frame.
struct NEOGRAPH_API StreamEvent {
    enum class Type { StatusUpdate, ArtifactUpdate, Task };
    Type                                    type = Type::Task;
    std::optional<TaskStatusUpdateEvent>    status_update;
    std::optional<TaskArtifactUpdateEvent>  artifact_update;
    std::optional<a2a::Task>                task;

    bool is_final() const noexcept {
        if (type == Type::StatusUpdate && status_update) return status_update->final;
        // A Task frame is terminal unless it is the opening snapshot of a
        // 1.0 / 0.3 task lifecycle stream (submitted or still working).
        if (type == Type::Task) {
            if (!task) return true;
            return task->status.state != TaskState::Submitted
                && task->status.state != TaskState::Working;
        }
        return false;
    }
};

/// One protocol binding + version + URL an agent exposes (1.0
/// `AgentInterface`; 0.3 cards contribute `additionalInterfaces` entries).
struct NEOGRAPH_API AgentInterface {
    std::string url;
    std::string protocol_binding;   ///< "JSONRPC" | "GRPC" | "HTTP+JSON" | custom
    std::string protocol_version;   ///< "Major.Minor", e.g. "1.0" / "0.3"
    std::string tenant;             ///< optional routing id (1.0 only)
};

/// One capability the agent advertises (1.0 `AgentSkill`). `tags` is
/// REQUIRED and non-empty on the 1.0 wire; a card built without tags
/// carries the skill id as its only tag.
struct NEOGRAPH_API AgentSkill {
    std::string              id;
    std::string              name;
    std::string              description;
    std::vector<std::string> tags;
    std::vector<std::string> examples;
    std::vector<std::string> input_modes;
    std::vector<std::string> output_modes;
};

/// Subset of AgentCard required to interact (spec §5.5).
/// We deserialise only fields the client actually consumes; unknown
/// fields are kept verbatim in `raw` for forward-compat.
///
/// A2AServer serves one card per protocol version (see
/// `to_json(json&, const AgentCard&, WireDialect)`): a GET with
/// `A2A-Version: 1.0` receives the pure 1.0 card; a GET without the header
/// (spec §3.6.2: 0.3) receives the 0.3 card, which also lists the 1.0
/// interface in `supportedInterfaces`.
struct NEOGRAPH_API AgentCard {
    std::string             name;
    std::string             description;
    /// Public endpoint of the JSON-RPC binding. Empty on a server card: the
    /// server then advertises the address the client used (the request's
    /// Host), or its bound address.
    std::string             url;
    std::string             version;
    /// The 0.3 card's `protocolVersion`; A2AServer always serves "0.3.0" in
    /// the 0.3 card and 1.0 interfaces in the 1.0 card.
    std::string             protocol_version;
    std::string             preferred_transport = "JSONRPC";
    std::vector<std::string> default_input_modes;
    std::vector<std::string> default_output_modes;

    /// A2AServer supports streaming (SendStreamingMessage, SubscribeToTask)
    /// unless this is false; then those methods answer
    /// UnsupportedOperationError (-32004).
    bool                    streaming                       = true;
    /// Push notifications are not implemented by A2AServer: the server
    /// always declares them unsupported, whatever this field says.
    bool                    push_notifications              = false;
    /// The authenticated extended card is not implemented by A2AServer: the
    /// server always declares it unsupported, whatever these fields say.
    bool                    extended_card                   = false;
    bool                    supports_authenticated_extended = false;

    /// 1.0 `supportedInterfaces` in card order (preferred first). For 0.3
    /// cards, the primary url/preferredTransport is first and any
    /// `additionalInterfaces` follow, all with the card's `protocolVersion`.
    std::vector<AgentInterface> supported_interfaces;

    /// Parsed skill ids. A card built in code may list only ids here; the
    /// serialised skill then uses the id as its name and only tag.
    std::vector<std::string> skill_names;
    /// Full skill objects (parsed, or set by a server). Take precedence over
    /// `skill_names` when serialising.
    std::vector<AgentSkill> skills;

    /// 1.0 `securitySchemes`: an object mapping a scheme name to a 1.0
    /// SecurityScheme in ProtoJSON form, e.g.
    /// `{"bearer": {"httpAuthSecurityScheme": {"scheme": "Bearer"}}}`.
    /// The 0.3 card carries the equivalent 0.3 form. Null when none.
    json security_schemes;
    /// 1.0 `securityRequirements`: an array of
    /// `{"schemes": {"<name>": {"list": [<scope>...]}}}`. Null when none.
    json security_requirements;
    /// `capabilities.extensions` (both versions): an array of AgentExtension
    /// objects `{uri, description, required, params}`. Null when none. A
    /// server that accepts NeoGraph collaboration envelopes adds
    /// COLLABORATION_EXTENSION_URI (collaboration.h).
    json extensions;

    /// Full original document as received. Preserves fields we don't yet model.
    json raw;
};

// ---------------------------------------------------------------------------
// JSON adapters (defined in src/a2a/types.cpp).
//
// The two-argument to_json overloads emit the 0.3 dialect (unchanged);
// the three-argument overloads select a WireDialect explicitly.
// ---------------------------------------------------------------------------
NEOGRAPH_API void to_json(json& j, const Part& p, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const Message& m, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const Artifact& a, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const TaskStatus& s, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const Task& t, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const MessageSendConfiguration& c, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const MessageSendParams& p, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const TaskStatusUpdateEvent& e, WireDialect dialect);
NEOGRAPH_API void to_json(json& j, const TaskArtifactUpdateEvent& e, WireDialect dialect);

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

/// The card a 0.3 reader receives: the 0.3 fields plus the 1.0
/// `supportedInterfaces` when the card lists them. A card with `raw` set is
/// emitted verbatim.
NEOGRAPH_API void to_json(json& j, const AgentCard& c);
/// One card per protocol version. V1_0 emits a pure 1.0 AgentCard (no 0.3
/// fields, non-empty skill tags, `capabilities.extendedAgentCard`); V0_3 is
/// the two-argument form. A card with `raw` set is emitted verbatim.
NEOGRAPH_API void to_json(json& j, const AgentCard& c, WireDialect dialect);
NEOGRAPH_API void from_json(const json& j, AgentCard& c);

NEOGRAPH_API void to_json(json& j, const TaskStatusUpdateEvent& e);
NEOGRAPH_API void from_json(const json& j, TaskStatusUpdateEvent& e);

NEOGRAPH_API void to_json(json& j, const TaskArtifactUpdateEvent& e);
NEOGRAPH_API void from_json(const json& j, TaskArtifactUpdateEvent& e);

/// Parse a single SSE `data: {...}` JSON object into the right variant.
/// 0.3: discriminator `kind` = "status-update" | "artifact-update" | "task"
/// | "message". 1.0: the single populated key of `StreamResponse`
/// (`statusUpdate` | `artifactUpdate` | `task` | `message`). A Message
/// payload is coerced into a Task (see task_from_result). Anything else
/// falls back to Task.
///
/// 1.0 status updates carry no `final` flag; it is derived from the state
/// (terminal or interrupted => final), matching the spec's stream-close rule.
NEOGRAPH_API StreamEvent parse_stream_event(const json& j);

/// Coerce a `SendMessage` / `message/send` / `GetTask` / `CancelTask`
/// result into a Task. Accepts the 1.0 wrappers (`{"task"}` /
/// `{"message"}`), the 0.3 `kind`-discriminated objects, and a bare 1.0
/// Task (object with `id` + `status`). A Message becomes a Completed Task
/// carrying it as `status.message` and `history`. Null yields a Failed Task;
/// anything unrecognised yields an Unknown Task with the payload in
/// `metadata`.
NEOGRAPH_API Task task_from_result(const json& result);

} // namespace neograph::a2a
