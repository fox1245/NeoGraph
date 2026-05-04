/**
 * @file acp/types.h
 * @brief Core data types for the Agent Client Protocol (ACP).
 *
 * ACP standardises editor↔agent communication the way LSP standardised
 * editor↔language-server communication: JSON-RPC over stdio (local
 * agents) or HTTP/WebSocket (remote). NeoGraph implements the
 * **agent** side — exposing a compiled graph as an ACP server that an
 * editor like Zed or Neovim can drive.
 *
 * Spec: https://agentclientprotocol.com/protocol/schema.md
 *
 * Coverage in this header:
 *   - Capabilities (Agent / Client / Prompt / Mcp / Fs)
 *   - Initialize request / response
 *   - ContentBlock variants (text / image / audio / resource_link / resource)
 *   - session/new and session/prompt request/response shapes
 *   - SessionNotification (session/update streamed by the agent)
 *   - StopReason
 *   - CancelNotification
 *
 * Deferred (modelled as free-form `json` for forward-compat):
 *   session/load, session/resume, session/list, session/close,
 *   session/set_config_option, session/set_mode, fs/&#42; and terminal/&#42;
 *   (these are agent→client requests — added when the engine wants to
 *    use them). The wildcards are HTML-entity-escaped because the
 *    literal slash-star token opens a nested comment inside this
 *    Doxygen block — backticks and backslash escape don't help (the
 *    tokenizer runs before markdown, and ``\`` renders literally).
 *    HTML entities are the cleanest source-and-render fix.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <optional>
#include <string>
#include <vector>

namespace neograph::acp {

using json = neograph::json;

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/// Capabilities the agent advertises about which prompt content it accepts.
struct NEOGRAPH_API PromptCapabilities {
    bool image            = false;  ///< Accepts ContentBlock::image in prompts.
    bool audio            = false;  ///< Accepts ContentBlock::audio in prompts.
    bool embedded_context = false;  ///< Accepts ContentBlock::resource (embedded).
};

/// MCP server transports the agent supports (stdio is implicit / required).
struct NEOGRAPH_API McpCapabilities {
    bool http = false;
    bool sse  = false;
};

/// Optional session-management features the agent supports.
struct NEOGRAPH_API SessionCapabilities {
    bool close  = false;  ///< session/close
    bool list   = false;  ///< session/list
    bool resume = false;  ///< session/resume
};

/// Capabilities advertised in InitializeResponse.
struct NEOGRAPH_API AgentCapabilities {
    bool                load_session = false;  ///< session/load supported.
    PromptCapabilities  prompt;
    McpCapabilities     mcp;
    SessionCapabilities session;
};

/// File-system access methods the client offers (agent→client direction).
struct NEOGRAPH_API FsCapabilities {
    bool read_text_file  = false;
    bool write_text_file = false;
};

/// Capabilities advertised by the editor in InitializeRequest.
struct NEOGRAPH_API ClientCapabilities {
    FsCapabilities fs;
    bool           terminal = false;  ///< terminal/&#42; methods supported.
};

// ---------------------------------------------------------------------------
// Content blocks — the unit of payload in prompts and updates.
// Discriminated by `type` ∈ {"text","image","audio","resource","resource_link"}.
// ---------------------------------------------------------------------------

struct NEOGRAPH_API ContentBlock {
    std::string type;        ///< discriminator
    std::string text;        ///< type == "text"
    std::string data;        ///< type == "image" | "audio" — base64 payload
    std::string mime_type;   ///< type == "image" | "audio" | "resource_link"
    std::string uri;         ///< type == "resource_link" | embedded resource uri
    std::string name;        ///< type == "resource_link"
    std::optional<std::string> title;       ///< type == "resource_link"
    std::optional<std::string> description; ///< type == "resource_link"
    std::optional<std::int64_t> size;       ///< type == "resource_link"

    /// type == "resource" — embedded inline resource. Verbatim JSON
    /// (TextResourceContents or BlobResourceContents) so we don't lose
    /// fields the engine doesn't model.
    json        resource;

    /// Optional annotations bag — passed through verbatim.
    json        annotations;

    /// Convenience builder for plain-text blocks.
    static ContentBlock text_block(std::string s);

    /// Convenience builder for resource_link blocks.
    static ContentBlock resource_link(std::string uri,
                                      std::string name,
                                      std::string mime_type = {});
};

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------

/// Sent once by the editor when the connection comes up.
struct NEOGRAPH_API InitializeRequest {
    int                protocol_version = 1;
    ClientCapabilities client_capabilities;
    json               client_info;  ///< free-form metadata bag (e.g. name/version)
};

struct NEOGRAPH_API InitializeResponse {
    int               protocol_version = 1;
    AgentCapabilities agent_capabilities;
    /// Auth methods the agent supports — left empty (server unauth-only)
    /// in the baseline implementation.
    std::vector<json> auth_methods;
    json              agent_info;  ///< free-form (e.g. {"name":"NeoGraph","version":"0.2.1"})
};

// ---------------------------------------------------------------------------
// session/new
// ---------------------------------------------------------------------------

/// Single MCP server entry attached to the session. Modelled as raw
/// JSON for forward-compat — ACP carries stdio/http/sse variants.
struct NEOGRAPH_API McpServerConfig {
    json raw;
};

struct NEOGRAPH_API NewSessionRequest {
    std::string                 cwd;          ///< working dir for fs/terminal calls
    std::vector<McpServerConfig> mcp_servers;
};

struct NEOGRAPH_API NewSessionResponse {
    std::string session_id;
    /// Session config / mode descriptors — JSON pass-through, may be
    /// populated by adapters that want to expose runtime knobs.
    json        config_options;
    json        modes;
};

// ---------------------------------------------------------------------------
// session/prompt
// ---------------------------------------------------------------------------

struct NEOGRAPH_API PromptRequest {
    std::string                session_id;
    std::vector<ContentBlock>  prompt;
};

/// Why the agent stopped responding for this turn. Values mirror the
/// official ACP schema (zed-industries/agent-client-protocol,
/// schema/schema.json: StopReason). Stop reasons that don't apply to
/// NeoGraph today (max_tokens, max_turn_requests, refusal) are
/// modelled so consumers can still round-trip them through to_json /
/// from_json without loss; the runtime only ever emits EndTurn or
/// Cancelled in the baseline server.
enum class StopReason {
    EndTurn,         ///< "end_turn" — turn finished normally.
    MaxTokens,       ///< "max_tokens" — model hit the token cap.
    MaxTurnRequests, ///< "max_turn_requests" — agent reached its turn-request budget.
    Refusal,         ///< "refusal" — agent refused to continue.
    Cancelled,       ///< "cancelled" — cancelled via session/cancel.
};

NEOGRAPH_API std::string stop_reason_to_string(StopReason s);
NEOGRAPH_API StopReason  stop_reason_from_string(std::string_view s);

struct NEOGRAPH_API PromptResponse {
    StopReason stop_reason = StopReason::EndTurn;
};

// ---------------------------------------------------------------------------
// session/cancel — JSON-RPC notification (no response)
// ---------------------------------------------------------------------------

struct NEOGRAPH_API CancelNotification {
    std::string session_id;
};

// ---------------------------------------------------------------------------
// fs/read_text_file, fs/write_text_file — agent → client requests
// ---------------------------------------------------------------------------

struct NEOGRAPH_API ReadTextFileRequest {
    std::string session_id;
    std::string path;
    std::optional<int> line;   ///< 1-based start line (null = from top)
    std::optional<int> limit;  ///< max lines to return (null = all)
};

struct NEOGRAPH_API ReadTextFileResponse {
    std::string content;
};

struct NEOGRAPH_API WriteTextFileRequest {
    std::string session_id;
    std::string path;
    std::string content;
};

NEOGRAPH_API void to_json(json& j, const ReadTextFileRequest& r);
NEOGRAPH_API void from_json(const json& j, ReadTextFileRequest& r);
NEOGRAPH_API void to_json(json& j, const ReadTextFileResponse& r);
NEOGRAPH_API void from_json(const json& j, ReadTextFileResponse& r);
NEOGRAPH_API void to_json(json& j, const WriteTextFileRequest& r);
NEOGRAPH_API void from_json(const json& j, WriteTextFileRequest& r);

// ---------------------------------------------------------------------------
// ToolCallUpdate — name matches the official ACP schema
// (`session/request_permission.toolCall: ToolCallUpdate`). Modeled
// minimally: spec carries more fields, preserved verbatim in `raw`.
//
// Renamed from `ToolCall` (which collided with the LLM-side
// `neograph::ToolCall` in `<neograph/types.h>` whenever both
// namespaces were brought in via `using namespace`). The two are
// genuinely different concepts — keep them under distinct names.
// ---------------------------------------------------------------------------

struct NEOGRAPH_API ToolCallUpdate {
    std::string tool_call_id;   ///< toolCallId — REQUIRED
    std::string tool_name;      ///< toolName  — REQUIRED
    json        input;          ///< tool input (free-form object)
    std::string kind;           ///< optional: "execute"|"read"|"edit"|"think"|"search"|...
    std::string status;         ///< optional: "pending"|"in_progress"|"completed"|"failed"
    std::vector<ContentBlock> content;  ///< optional: tool output blocks
    /// Full original JSON for forward-compat (locations, rawInput, etc.).
    json        raw;
};

NEOGRAPH_API void to_json(json& j, const ToolCallUpdate& t);
NEOGRAPH_API void from_json(const json& j, ToolCallUpdate& t);

// ---------------------------------------------------------------------------
// session/request_permission — agent → client (the agent asks the editor
// to surface a permission prompt before executing a tool call)
// ---------------------------------------------------------------------------

struct NEOGRAPH_API PermissionOption {
    std::string option_id;   ///< optionId — REQUIRED
    std::string name;        ///< display name shown to the user — REQUIRED
    std::string kind;        ///< "allow_once" | "allow_always" | "reject_once" | "reject_always"
};

NEOGRAPH_API void to_json(json& j, const PermissionOption& o);
NEOGRAPH_API void from_json(const json& j, PermissionOption& o);

struct NEOGRAPH_API RequestPermissionRequest {
    std::string                 session_id;
    ToolCallUpdate                    tool_call;
    std::vector<PermissionOption> options;
};

NEOGRAPH_API void to_json(json& j, const RequestPermissionRequest& r);
NEOGRAPH_API void from_json(const json& j, RequestPermissionRequest& r);

/// The user's decision (or absence thereof).
///   - Selected  : the editor surfaced the prompt and the user picked one.
///   - Cancelled : the prompt was dismissed before the user answered
///                 (typically because the prompt turn was cancelled).
enum class PermissionOutcomeKind { Selected, Cancelled };

struct NEOGRAPH_API RequestPermissionOutcome {
    PermissionOutcomeKind kind     = PermissionOutcomeKind::Cancelled;
    std::string           option_id;  ///< populated when kind == Selected
};

NEOGRAPH_API void to_json(json& j, const RequestPermissionOutcome& o);
NEOGRAPH_API void from_json(const json& j, RequestPermissionOutcome& o);

struct NEOGRAPH_API RequestPermissionResponse {
    RequestPermissionOutcome outcome;
};

NEOGRAPH_API void to_json(json& j, const RequestPermissionResponse& r);
NEOGRAPH_API void from_json(const json& j, RequestPermissionResponse& r);

// ---------------------------------------------------------------------------
// session/update — agent-emitted streaming notification
// ---------------------------------------------------------------------------

/// Streaming event the agent sends while a prompt is in flight.
/// `session_update` is the discriminator string. Common variants we
/// model:
///   - "agent_message_chunk"  : final-answer text streamed back; uses `content`
///   - "agent_thought_chunk"  : reasoning trace; uses `content`
///   - "user_message_chunk"   : echo of inbound user text (rare)
///   - "tool_call"            : new tool invocation; uses `raw` until typed
///   - "tool_call_update"     : update to an in-flight tool call
///   - "plan"                 : structured execution plan
///
/// For unmodelled variants (and forward-compat), the full notification
/// JSON is preserved in `raw`.
struct NEOGRAPH_API SessionUpdate {
    std::string  session_update;  ///< discriminator
    ContentBlock content;         ///< populated for *_chunk variants
    json         raw;             ///< full original JSON (always populated)
};

struct NEOGRAPH_API SessionNotification {
    std::string   session_id;
    SessionUpdate update;
};

// ---------------------------------------------------------------------------
// JSON adapters (defined in src/acp/types.cpp).
// ---------------------------------------------------------------------------
NEOGRAPH_API void to_json(json& j, const PromptCapabilities& c);
NEOGRAPH_API void from_json(const json& j, PromptCapabilities& c);

NEOGRAPH_API void to_json(json& j, const McpCapabilities& c);
NEOGRAPH_API void from_json(const json& j, McpCapabilities& c);

NEOGRAPH_API void to_json(json& j, const SessionCapabilities& c);
NEOGRAPH_API void from_json(const json& j, SessionCapabilities& c);

NEOGRAPH_API void to_json(json& j, const AgentCapabilities& c);
NEOGRAPH_API void from_json(const json& j, AgentCapabilities& c);

NEOGRAPH_API void to_json(json& j, const FsCapabilities& c);
NEOGRAPH_API void from_json(const json& j, FsCapabilities& c);

NEOGRAPH_API void to_json(json& j, const ClientCapabilities& c);
NEOGRAPH_API void from_json(const json& j, ClientCapabilities& c);

NEOGRAPH_API void to_json(json& j, const ContentBlock& b);
NEOGRAPH_API void from_json(const json& j, ContentBlock& b);

NEOGRAPH_API void to_json(json& j, const InitializeRequest& r);
NEOGRAPH_API void from_json(const json& j, InitializeRequest& r);

NEOGRAPH_API void to_json(json& j, const InitializeResponse& r);
NEOGRAPH_API void from_json(const json& j, InitializeResponse& r);

NEOGRAPH_API void to_json(json& j, const McpServerConfig& c);
NEOGRAPH_API void from_json(const json& j, McpServerConfig& c);

NEOGRAPH_API void to_json(json& j, const NewSessionRequest& r);
NEOGRAPH_API void from_json(const json& j, NewSessionRequest& r);

NEOGRAPH_API void to_json(json& j, const NewSessionResponse& r);
NEOGRAPH_API void from_json(const json& j, NewSessionResponse& r);

NEOGRAPH_API void to_json(json& j, const PromptRequest& r);
NEOGRAPH_API void from_json(const json& j, PromptRequest& r);

NEOGRAPH_API void to_json(json& j, const PromptResponse& r);
NEOGRAPH_API void from_json(const json& j, PromptResponse& r);

NEOGRAPH_API void to_json(json& j, const CancelNotification& n);
NEOGRAPH_API void from_json(const json& j, CancelNotification& n);

NEOGRAPH_API void to_json(json& j, const SessionUpdate& u);
NEOGRAPH_API void from_json(const json& j, SessionUpdate& u);

NEOGRAPH_API void to_json(json& j, const SessionNotification& n);
NEOGRAPH_API void from_json(const json& j, SessionNotification& n);

}  // namespace neograph::acp
