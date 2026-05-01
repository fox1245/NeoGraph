#include <neograph/acp/types.h>

namespace neograph::acp {

// ---------------------------------------------------------------------------
// StopReason — wire-format strings per the official ACP schema
// (zed-industries/agent-client-protocol, schema/schema.json).
// ---------------------------------------------------------------------------
std::string stop_reason_to_string(StopReason s) {
    switch (s) {
        case StopReason::EndTurn:         return "end_turn";
        case StopReason::MaxTokens:       return "max_tokens";
        case StopReason::MaxTurnRequests: return "max_turn_requests";
        case StopReason::Refusal:         return "refusal";
        case StopReason::Cancelled:       return "cancelled";
    }
    return "end_turn";
}

StopReason stop_reason_from_string(std::string_view s) {
    if (s == "end_turn")          return StopReason::EndTurn;
    if (s == "max_tokens")        return StopReason::MaxTokens;
    if (s == "max_turn_requests") return StopReason::MaxTurnRequests;
    if (s == "refusal")           return StopReason::Refusal;
    if (s == "cancelled")         return StopReason::Cancelled;
    return StopReason::EndTurn;
}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
void to_json(json& j, const PromptCapabilities& c) {
    j = json::object();
    if (c.image)            j["image"] = true;
    if (c.audio)            j["audio"] = true;
    if (c.embedded_context) j["embeddedContext"] = true;
}
void from_json(const json& j, PromptCapabilities& c) {
    c.image            = j.value("image", false);
    c.audio            = j.value("audio", false);
    c.embedded_context = j.value("embeddedContext", false);
}

void to_json(json& j, const McpCapabilities& c) {
    j = json::object();
    if (c.http) j["http"] = true;
    if (c.sse)  j["sse"]  = true;
}
void from_json(const json& j, McpCapabilities& c) {
    c.http = j.value("http", false);
    c.sse  = j.value("sse",  false);
}

void to_json(json& j, const SessionCapabilities& c) {
    j = json::object();
    if (c.close)  j["close"]  = json::object();
    if (c.list)   j["list"]   = json::object();
    if (c.resume) j["resume"] = json::object();
}
void from_json(const json& j, SessionCapabilities& c) {
    c.close  = j.contains("close")  && !j["close"].is_null();
    c.list   = j.contains("list")   && !j["list"].is_null();
    c.resume = j.contains("resume") && !j["resume"].is_null();
}

void to_json(json& j, const AgentCapabilities& c) {
    j = json::object();
    if (c.load_session) j["loadSession"] = true;
    json p; to_json(p, c.prompt);
    if (!p.empty()) j["promptCapabilities"] = std::move(p);
    json m; to_json(m, c.mcp);
    if (!m.empty()) j["mcpCapabilities"] = std::move(m);
    json s; to_json(s, c.session);
    if (!s.empty()) j["sessionCapabilities"] = std::move(s);
}
void from_json(const json& j, AgentCapabilities& c) {
    c.load_session = j.value("loadSession", false);
    if (j.contains("promptCapabilities")) from_json(j["promptCapabilities"], c.prompt);
    if (j.contains("mcpCapabilities"))    from_json(j["mcpCapabilities"],    c.mcp);
    if (j.contains("sessionCapabilities")) from_json(j["sessionCapabilities"], c.session);
}

void to_json(json& j, const FsCapabilities& c) {
    j = json::object();
    if (c.read_text_file)  j["readTextFile"]  = true;
    if (c.write_text_file) j["writeTextFile"] = true;
}
void from_json(const json& j, FsCapabilities& c) {
    c.read_text_file  = j.value("readTextFile", false);
    c.write_text_file = j.value("writeTextFile", false);
}

void to_json(json& j, const ClientCapabilities& c) {
    j = json::object();
    json fs; to_json(fs, c.fs);
    if (!fs.empty()) j["fs"] = std::move(fs);
    if (c.terminal) j["terminal"] = true;
}
void from_json(const json& j, ClientCapabilities& c) {
    if (j.contains("fs")) from_json(j["fs"], c.fs);
    c.terminal = j.value("terminal", false);
}

// ---------------------------------------------------------------------------
// ContentBlock
// ---------------------------------------------------------------------------
ContentBlock ContentBlock::text_block(std::string s) {
    ContentBlock b;
    b.type = "text";
    b.text = std::move(s);
    return b;
}

ContentBlock ContentBlock::resource_link(std::string uri,
                                         std::string name,
                                         std::string mime_type) {
    ContentBlock b;
    b.type      = "resource_link";
    b.uri       = std::move(uri);
    b.name      = std::move(name);
    b.mime_type = std::move(mime_type);
    return b;
}

void to_json(json& j, const ContentBlock& b) {
    j = json::object();
    j["type"] = b.type;
    if (b.type == "text") {
        j["text"] = b.text;
    } else if (b.type == "image" || b.type == "audio") {
        j["data"]     = b.data;
        j["mimeType"] = b.mime_type;
        if (b.type == "image" && !b.uri.empty()) j["uri"] = b.uri;
    } else if (b.type == "resource_link") {
        j["uri"]  = b.uri;
        j["name"] = b.name;
        if (!b.mime_type.empty()) j["mimeType"]    = b.mime_type;
        if (b.title)              j["title"]       = *b.title;
        if (b.description)        j["description"] = *b.description;
        if (b.size)               j["size"]        = *b.size;
    } else if (b.type == "resource") {
        if (!b.resource.is_null()) j["resource"] = b.resource;
    }
    if (!b.annotations.is_null() && !b.annotations.empty()) {
        j["annotations"] = b.annotations;
    }
}

void from_json(const json& j, ContentBlock& b) {
    b.type = j.value("type", std::string("text"));
    if (b.type == "text") {
        b.text = j.value("text", std::string());
    } else if (b.type == "image" || b.type == "audio") {
        b.data      = j.value("data", std::string());
        b.mime_type = j.value("mimeType", std::string());
        if (j.contains("uri")) b.uri = j.value("uri", std::string());
    } else if (b.type == "resource_link") {
        b.uri  = j.value("uri",  std::string());
        b.name = j.value("name", std::string());
        if (j.contains("mimeType"))    b.mime_type   = j["mimeType"].get<std::string>();
        if (j.contains("title"))       b.title       = j["title"].get<std::string>();
        if (j.contains("description")) b.description = j["description"].get<std::string>();
        if (j.contains("size") && j["size"].is_number_integer())
            b.size = j["size"].get<std::int64_t>();
    } else if (b.type == "resource") {
        if (j.contains("resource")) b.resource = j["resource"];
    }
    if (j.contains("annotations")) b.annotations = j["annotations"];
}

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------
void to_json(json& j, const InitializeRequest& r) {
    j = json::object();
    j["protocolVersion"] = r.protocol_version;
    json caps; to_json(caps, r.client_capabilities);
    j["clientCapabilities"] = std::move(caps);
    if (!r.client_info.is_null()) j["clientInfo"] = r.client_info;
}
void from_json(const json& j, InitializeRequest& r) {
    r.protocol_version = j.value("protocolVersion", 1);
    if (j.contains("clientCapabilities")) {
        from_json(j["clientCapabilities"], r.client_capabilities);
    }
    if (j.contains("clientInfo")) r.client_info = j["clientInfo"];
}

void to_json(json& j, const InitializeResponse& r) {
    j = json::object();
    j["protocolVersion"] = r.protocol_version;
    json caps; to_json(caps, r.agent_capabilities);
    j["agentCapabilities"] = std::move(caps);
    auto am = json::array();
    for (auto& m : r.auth_methods) am.push_back(m);
    j["authMethods"] = std::move(am);
    if (!r.agent_info.is_null()) j["agentInfo"] = r.agent_info;
}
void from_json(const json& j, InitializeResponse& r) {
    r.protocol_version = j.value("protocolVersion", 1);
    if (j.contains("agentCapabilities")) {
        from_json(j["agentCapabilities"], r.agent_capabilities);
    }
    r.auth_methods.clear();
    if (j.contains("authMethods") && j["authMethods"].is_array()) {
        for (auto m : j["authMethods"]) r.auth_methods.push_back(m);
    }
    if (j.contains("agentInfo")) r.agent_info = j["agentInfo"];
}

// ---------------------------------------------------------------------------
// session/new
// ---------------------------------------------------------------------------
void to_json(json& j, const McpServerConfig& c) { j = c.raw.is_null() ? json::object() : c.raw; }
void from_json(const json& j, McpServerConfig& c) { c.raw = j; }

void to_json(json& j, const NewSessionRequest& r) {
    j = json::object();
    j["cwd"] = r.cwd;
    auto arr = json::array();
    for (auto& s : r.mcp_servers) arr.push_back(s.raw.is_null() ? json::object() : s.raw);
    j["mcpServers"] = std::move(arr);
}
void from_json(const json& j, NewSessionRequest& r) {
    r.cwd = j.value("cwd", std::string());
    r.mcp_servers.clear();
    if (j.contains("mcpServers") && j["mcpServers"].is_array()) {
        for (auto s : j["mcpServers"]) {
            McpServerConfig cfg;
            cfg.raw = s;
            r.mcp_servers.push_back(std::move(cfg));
        }
    }
}

void to_json(json& j, const NewSessionResponse& r) {
    j = json::object();
    j["sessionId"] = r.session_id;
    if (!r.config_options.is_null() && !r.config_options.empty())
        j["configOptions"] = r.config_options;
    if (!r.modes.is_null() && !r.modes.empty())
        j["modes"] = r.modes;
}
void from_json(const json& j, NewSessionResponse& r) {
    r.session_id = j.value("sessionId", std::string());
    if (j.contains("configOptions")) r.config_options = j["configOptions"];
    if (j.contains("modes"))         r.modes          = j["modes"];
}

// ---------------------------------------------------------------------------
// session/prompt
// ---------------------------------------------------------------------------
void to_json(json& j, const PromptRequest& r) {
    j = json::object();
    j["sessionId"] = r.session_id;
    auto arr = json::array();
    for (auto& b : r.prompt) {
        json bj; to_json(bj, b);
        arr.push_back(std::move(bj));
    }
    j["prompt"] = std::move(arr);
}
void from_json(const json& j, PromptRequest& r) {
    r.session_id = j.value("sessionId", std::string());
    r.prompt.clear();
    if (j.contains("prompt") && j["prompt"].is_array()) {
        for (auto b : j["prompt"]) {
            ContentBlock cb;
            from_json(b, cb);
            r.prompt.push_back(std::move(cb));
        }
    }
}

void to_json(json& j, const PromptResponse& r) {
    j = json::object();
    j["stopReason"] = stop_reason_to_string(r.stop_reason);
}
void from_json(const json& j, PromptResponse& r) {
    r.stop_reason = stop_reason_from_string(
        j.value("stopReason", std::string("completed")));
}

// ---------------------------------------------------------------------------
// session/cancel
// ---------------------------------------------------------------------------
void to_json(json& j, const CancelNotification& n) {
    j = json::object();
    j["sessionId"] = n.session_id;
}
void from_json(const json& j, CancelNotification& n) {
    n.session_id = j.value("sessionId", std::string());
}

// ---------------------------------------------------------------------------
// fs/read_text_file
// ---------------------------------------------------------------------------
void to_json(json& j, const ReadTextFileRequest& r) {
    j = json::object();
    j["sessionId"] = r.session_id;
    j["path"]      = r.path;
    if (r.line)  j["line"]  = *r.line;
    if (r.limit) j["limit"] = *r.limit;
}
void from_json(const json& j, ReadTextFileRequest& r) {
    r.session_id = j.value("sessionId", std::string());
    r.path       = j.value("path",      std::string());
    if (j.contains("line")  && j["line"].is_number_integer())  r.line  = j["line"].get<int>();
    if (j.contains("limit") && j["limit"].is_number_integer()) r.limit = j["limit"].get<int>();
}
void to_json(json& j, const ReadTextFileResponse& r) {
    j = json::object();
    j["content"] = r.content;
}
void from_json(const json& j, ReadTextFileResponse& r) {
    r.content = j.value("content", std::string());
}

// ---------------------------------------------------------------------------
// fs/write_text_file
// ---------------------------------------------------------------------------
void to_json(json& j, const WriteTextFileRequest& r) {
    j = json::object();
    j["sessionId"] = r.session_id;
    j["path"]      = r.path;
    j["content"]   = r.content;
}
void from_json(const json& j, WriteTextFileRequest& r) {
    r.session_id = j.value("sessionId", std::string());
    r.path       = j.value("path",      std::string());
    r.content    = j.value("content",   std::string());
}

// ---------------------------------------------------------------------------
// ToolCallUpdate
// ---------------------------------------------------------------------------
void to_json(json& j, const ToolCallUpdate& t) {
    j = t.raw.is_null() || t.raw.empty() ? json::object() : t.raw;
    j["toolCallId"] = t.tool_call_id;
    j["toolName"]   = t.tool_name;
    if (!t.input.is_null())     j["input"]  = t.input;
    if (!t.kind.empty())        j["kind"]   = t.kind;
    if (!t.status.empty())      j["status"] = t.status;
    if (!t.content.empty()) {
        auto arr = json::array();
        for (auto& b : t.content) {
            json bj; to_json(bj, b);
            arr.push_back(std::move(bj));
        }
        j["content"] = std::move(arr);
    }
}

void from_json(const json& j, ToolCallUpdate& t) {
    t.raw          = j;
    t.tool_call_id = j.value("toolCallId", std::string());
    t.tool_name    = j.value("toolName",   std::string());
    if (j.contains("input"))  t.input  = j["input"];
    t.kind   = j.value("kind",   std::string());
    t.status = j.value("status", std::string());
    t.content.clear();
    if (j.contains("content") && j["content"].is_array()) {
        for (auto b : j["content"]) {
            ContentBlock cb; from_json(b, cb);
            t.content.push_back(std::move(cb));
        }
    }
}

// ---------------------------------------------------------------------------
// PermissionOption
// ---------------------------------------------------------------------------
void to_json(json& j, const PermissionOption& o) {
    j = json::object();
    j["optionId"] = o.option_id;
    j["name"]     = o.name;
    j["kind"]     = o.kind;
}

void from_json(const json& j, PermissionOption& o) {
    o.option_id = j.value("optionId", std::string());
    o.name      = j.value("name",     std::string());
    o.kind      = j.value("kind",     std::string());
}

// ---------------------------------------------------------------------------
// session/request_permission
// ---------------------------------------------------------------------------
void to_json(json& j, const RequestPermissionRequest& r) {
    j = json::object();
    j["sessionId"] = r.session_id;
    json tc; to_json(tc, r.tool_call);
    j["toolCall"] = std::move(tc);
    auto arr = json::array();
    for (auto& o : r.options) {
        json oj; to_json(oj, o);
        arr.push_back(std::move(oj));
    }
    j["options"] = std::move(arr);
}

void from_json(const json& j, RequestPermissionRequest& r) {
    r.session_id = j.value("sessionId", std::string());
    if (j.contains("toolCall")) from_json(j["toolCall"], r.tool_call);
    r.options.clear();
    if (j.contains("options") && j["options"].is_array()) {
        for (auto o : j["options"]) {
            PermissionOption po; from_json(o, po);
            r.options.push_back(std::move(po));
        }
    }
}

void to_json(json& j, const RequestPermissionOutcome& o) {
    j = json::object();
    if (o.kind == PermissionOutcomeKind::Selected) {
        j["outcome"]  = "selected";
        j["optionId"] = o.option_id;
    } else {
        j["outcome"] = "cancelled";
    }
}

void from_json(const json& j, RequestPermissionOutcome& o) {
    auto kind = j.value("outcome", std::string("cancelled"));
    if (kind == "selected") {
        o.kind      = PermissionOutcomeKind::Selected;
        o.option_id = j.value("optionId", std::string());
    } else {
        o.kind = PermissionOutcomeKind::Cancelled;
        o.option_id.clear();
    }
}

void to_json(json& j, const RequestPermissionResponse& r) {
    j = json::object();
    json oj; to_json(oj, r.outcome);
    j["outcome"] = std::move(oj);
}

void from_json(const json& j, RequestPermissionResponse& r) {
    if (j.contains("outcome")) from_json(j["outcome"], r.outcome);
}

// ---------------------------------------------------------------------------
// session/update
// ---------------------------------------------------------------------------
void to_json(json& j, const SessionUpdate& u) {
    if (!u.raw.is_null() && !u.raw.empty()) {
        j = u.raw;
        j["sessionUpdate"] = u.session_update;
    } else {
        j = json::object();
        j["sessionUpdate"] = u.session_update;
    }
    bool is_chunk = (u.session_update == "agent_message_chunk"
                     || u.session_update == "agent_thought_chunk"
                     || u.session_update == "user_message_chunk");
    if (is_chunk && !u.content.type.empty()) {
        json cj; to_json(cj, u.content);
        j["content"] = std::move(cj);
    }
}
void from_json(const json& j, SessionUpdate& u) {
    u.session_update = j.value("sessionUpdate", std::string());
    u.raw = j;
    if (j.contains("content")) {
        from_json(j["content"], u.content);
    }
}

void to_json(json& j, const SessionNotification& n) {
    j = json::object();
    j["sessionId"] = n.session_id;
    json uj; to_json(uj, n.update);
    j["update"] = std::move(uj);
}
void from_json(const json& j, SessionNotification& n) {
    n.session_id = j.value("sessionId", std::string());
    if (j.contains("update")) from_json(j["update"], n.update);
}

}  // namespace neograph::acp
