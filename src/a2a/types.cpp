#include <neograph/a2a/types.h>

namespace neograph::a2a {

namespace {
template <typename Vec, typename Fn>
void parse_array_of(const json& arr, Vec& dst, Fn from_one) {
    if (!arr.is_array()) return;
    for (auto v : arr) {
        typename Vec::value_type elem;
        from_one(v, elem);
        dst.push_back(std::move(elem));
    }
}

// An optional member sent as JSON null is absent (ProtoJSON and the 0.3
// JSON schema both read it so), not an empty or default value.
bool present(const json& j, const char* key) {
    return j.contains(key) && !j[key].is_null();
}
}

// ---------------------------------------------------------------------------
// TaskState <-> string
// ---------------------------------------------------------------------------
std::string task_state_to_string(TaskState s, WireDialect dialect) {
    if (dialect == WireDialect::V0_3) return task_state_to_string(s);
    switch (s) {
        case TaskState::Submitted:     return "TASK_STATE_SUBMITTED";
        case TaskState::Working:       return "TASK_STATE_WORKING";
        case TaskState::InputRequired: return "TASK_STATE_INPUT_REQUIRED";
        case TaskState::Completed:     return "TASK_STATE_COMPLETED";
        case TaskState::Canceled:      return "TASK_STATE_CANCELED";
        case TaskState::Failed:        return "TASK_STATE_FAILED";
        case TaskState::Rejected:      return "TASK_STATE_REJECTED";
        case TaskState::AuthRequired:  return "TASK_STATE_AUTH_REQUIRED";
        case TaskState::Unknown:       return "TASK_STATE_UNSPECIFIED";
    }
    return "TASK_STATE_UNSPECIFIED";
}

std::string task_state_to_string(TaskState s) {
    switch (s) {
        case TaskState::Submitted:     return "submitted";
        case TaskState::Working:       return "working";
        case TaskState::InputRequired: return "input-required";
        case TaskState::Completed:     return "completed";
        case TaskState::Canceled:      return "canceled";
        case TaskState::Failed:        return "failed";
        case TaskState::Rejected:      return "rejected";
        case TaskState::AuthRequired:  return "auth-required";
        case TaskState::Unknown:       return "unknown";
    }
    return "unknown";
}

TaskState task_state_from_string(std::string_view s) {
    if (s == "submitted")      return TaskState::Submitted;
    if (s == "working")        return TaskState::Working;
    if (s == "input-required") return TaskState::InputRequired;
    if (s == "completed")      return TaskState::Completed;
    if (s == "canceled")       return TaskState::Canceled;
    if (s == "failed")         return TaskState::Failed;
    if (s == "rejected")       return TaskState::Rejected;
    if (s == "auth-required")  return TaskState::AuthRequired;
    if (s == "TASK_STATE_SUBMITTED")      return TaskState::Submitted;
    if (s == "TASK_STATE_WORKING")        return TaskState::Working;
    if (s == "TASK_STATE_INPUT_REQUIRED") return TaskState::InputRequired;
    if (s == "TASK_STATE_COMPLETED")      return TaskState::Completed;
    if (s == "TASK_STATE_CANCELED")       return TaskState::Canceled;
    if (s == "TASK_STATE_FAILED")         return TaskState::Failed;
    if (s == "TASK_STATE_REJECTED")       return TaskState::Rejected;
    if (s == "TASK_STATE_AUTH_REQUIRED")  return TaskState::AuthRequired;
    return TaskState::Unknown;
}

std::string role_to_string(Role r) {
    return r == Role::Agent ? "agent" : "user";
}

std::string role_to_string(Role r, WireDialect dialect) {
    if (dialect == WireDialect::V0_3) return role_to_string(r);
    return r == Role::Agent ? "ROLE_AGENT" : "ROLE_USER";
}

Role role_from_string(std::string_view s) {
    return (s == "agent" || s == "ROLE_AGENT") ? Role::Agent : Role::User;
}

// ---------------------------------------------------------------------------
// Part
// ---------------------------------------------------------------------------
Part Part::text_part(std::string s) {
    Part p;
    p.kind = "text";
    p.text = std::move(s);
    return p;
}

void to_json(json& j, const Part& p, WireDialect dialect) {
    if (dialect == WireDialect::V0_3) {
        to_json(j, p);
        return;
    }
    // 1.0: flat oneof content; no `kind` discriminator.
    j = json::object();
    if (p.kind == "text") {
        j["text"] = p.text;
        if (!p.media_type.empty()) j["mediaType"] = p.media_type;
    } else if (p.kind == "file" && p.file.is_object()) {
        const auto& f = p.file;
        if (f.contains("bytes")) j["raw"] = f["bytes"];
        else if (f.contains("uri")) j["url"] = f["uri"];
        if (f.contains("name"))     j["filename"]  = f["name"];
        if (f.contains("mimeType")) j["mediaType"] = f["mimeType"];
    } else if (p.kind == "data" && !p.data.is_null()) {
        j["data"] = p.data;
        if (!p.media_type.empty()) j["mediaType"] = p.media_type;
    }
    if (!p.metadata.is_null() && !p.metadata.empty()) {
        j["metadata"] = p.metadata;
    }
}

void to_json(json& j, const Part& p) {
    j = json::object();
    j["kind"] = p.kind;
    if (p.kind == "text") {
        j["text"] = p.text;
    } else if (p.kind == "file" && !p.file.is_null()) {
        j["file"] = p.file;
    } else if (p.kind == "data" && !p.data.is_null()) {
        j["data"] = p.data;
    }
    if (!p.metadata.is_null() && !p.metadata.empty()) {
        j["metadata"] = p.metadata;
    }
}

void from_json(const json& j, Part& p) {
    if (j.is_object() && !j.contains("kind")
        && (j.contains("text") || j.contains("raw") || j.contains("url")
            || j.contains("data"))) {
        // 1.0 flat Part: the populated oneof member is the discriminator.
        if (j.contains("text")) {
            p.kind = "text";
            p.text = j.value("text", std::string());
        } else if (j.contains("raw") || j.contains("url")) {
            p.kind = "file";
            p.file = json::object();
            if (j.contains("raw")) p.file["bytes"] = j["raw"];
            else                   p.file["uri"]   = j["url"];
            auto filename = j.value("filename", std::string());
            if (!filename.empty()) p.file["name"] = filename;
            auto media = j.value("mediaType", std::string());
            if (!media.empty()) p.file["mimeType"] = media;
        } else {
            p.kind = "data";
            p.data = j["data"];
        }
        if (p.kind != "file") p.media_type = j.value("mediaType", std::string());
        if (j.contains("metadata")) p.metadata = j["metadata"];
        return;
    }
    p.kind = j.value("kind", std::string("text"));
    if (p.kind == "text") {
        p.text = j.value("text", std::string());
    } else if (p.kind == "file") {
        if (j.contains("file")) p.file = j["file"];
    } else if (p.kind == "data") {
        if (j.contains("data")) p.data = j["data"];
    }
    if (j.contains("metadata")) p.metadata = j["metadata"];
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
void to_json(json& j, const Message& m) { to_json(j, m, WireDialect::V0_3); }

void to_json(json& j, const Message& m, WireDialect dialect) {
    j = json::object();
    if (dialect == WireDialect::V0_3) j["kind"] = "message";
    j["messageId"] = m.message_id;
    j["role"]      = role_to_string(m.role, dialect);
    auto parts = json::array();
    for (auto& part : m.parts) {
        json pj;
        to_json(pj, part, dialect);
        parts.push_back(std::move(pj));
    }
    j["parts"] = std::move(parts);
    if (m.task_id)    j["taskId"]    = *m.task_id;
    if (m.context_id) j["contextId"] = *m.context_id;
    if (!m.reference_task_ids.empty()) {
        auto a = json::array();
        for (auto& s : m.reference_task_ids) a.push_back(s);
        j["referenceTaskIds"] = std::move(a);
    }
    if (!m.extensions.empty()) {
        auto a = json::array();
        for (auto& s : m.extensions) a.push_back(s);
        j["extensions"] = std::move(a);
    }
    if (!m.metadata.is_null() && !m.metadata.empty()) j["metadata"] = m.metadata;
}

void from_json(const json& j, Message& m) {
    m.kind       = j.value("kind", std::string("message"));
    m.message_id = j.value("messageId", std::string());
    m.role       = role_from_string(j.value("role", std::string("user")));
    if (j.contains("parts")) {
        parse_array_of(j["parts"], m.parts,
                       [](const json& v, Part& out) { from_json(v, out); });
    }
    if (present(j, "taskId"))    m.task_id    = j.value("taskId", std::string());
    if (present(j, "contextId")) m.context_id = j.value("contextId", std::string());
    if (j.contains("referenceTaskIds")) {
        auto arr = j["referenceTaskIds"];
        if (arr.is_array()) for (auto v : arr) m.reference_task_ids.push_back(v.get<std::string>());
    }
    if (j.contains("extensions")) {
        auto arr = j["extensions"];
        if (arr.is_array()) for (auto v : arr) m.extensions.push_back(v.get<std::string>());
    }
    if (j.contains("metadata")) m.metadata = j["metadata"];
}

// ---------------------------------------------------------------------------
// Artifact
// ---------------------------------------------------------------------------
void to_json(json& j, const Artifact& a) { to_json(j, a, WireDialect::V0_3); }

void to_json(json& j, const Artifact& a, WireDialect dialect) {
    j = json::object();
    j["artifactId"] = a.artifact_id;
    auto parts = json::array();
    for (auto& part : a.parts) {
        json pj;
        to_json(pj, part, dialect);
        parts.push_back(std::move(pj));
    }
    j["parts"] = std::move(parts);
    if (a.name)        j["name"]        = *a.name;
    if (a.description) j["description"] = *a.description;
    if (!a.metadata.is_null() && !a.metadata.empty()) j["metadata"] = a.metadata;
}

void from_json(const json& j, Artifact& a) {
    a.artifact_id = j.value("artifactId", std::string());
    if (j.contains("parts")) {
        parse_array_of(j["parts"], a.parts,
                       [](const json& v, Part& out) { from_json(v, out); });
    }
    if (present(j, "name"))        a.name        = j.value("name", std::string());
    if (present(j, "description")) a.description = j.value("description", std::string());
    if (j.contains("metadata"))    a.metadata    = j["metadata"];
}

// ---------------------------------------------------------------------------
// TaskStatus
// ---------------------------------------------------------------------------
void to_json(json& j, const TaskStatus& s) { to_json(j, s, WireDialect::V0_3); }

void to_json(json& j, const TaskStatus& s, WireDialect dialect) {
    j = json::object();
    j["state"] = task_state_to_string(s.state, dialect);
    if (s.message) {
        json mj;
        to_json(mj, *s.message, dialect);
        j["message"] = std::move(mj);
    }
    if (s.timestamp) j["timestamp"] = *s.timestamp;
}

void from_json(const json& j, TaskStatus& s) {
    s.state = task_state_from_string(j.value("state", std::string("unknown")));
    if (j.contains("message")) {
        auto mj = j["message"];
        if (!mj.is_null()) {
            Message m;
            from_json(mj, m);
            s.message = std::move(m);
        }
    }
    if (j.contains("timestamp")) {
        auto ts = j.value("timestamp", std::string());
        if (!ts.empty()) s.timestamp = ts;
    }
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
void to_json(json& j, const Task& t) { to_json(j, t, WireDialect::V0_3); }

void to_json(json& j, const Task& t, WireDialect dialect) {
    j = json::object();
    if (dialect == WireDialect::V0_3) j["kind"] = "task";
    j["id"]        = t.id;
    j["contextId"] = t.context_id;
    json sj;
    to_json(sj, t.status, dialect);
    j["status"] = std::move(sj);
    if (!t.artifacts.empty()) {
        auto arr = json::array();
        for (auto& a : t.artifacts) {
            json aj;
            to_json(aj, a, dialect);
            arr.push_back(std::move(aj));
        }
        j["artifacts"] = std::move(arr);
    }
    if (!t.history.empty()) {
        auto arr = json::array();
        for (auto& m : t.history) {
            json mj;
            to_json(mj, m, dialect);
            arr.push_back(std::move(mj));
        }
        j["history"] = std::move(arr);
    }
    if (!t.metadata.is_null() && !t.metadata.empty()) j["metadata"] = t.metadata;
}

void from_json(const json& j, Task& t) {
    t.kind       = j.value("kind", std::string("task"));
    t.id         = j.value("id", std::string());
    t.context_id = j.value("contextId", std::string());
    if (j.contains("status")) from_json(j["status"], t.status);
    if (j.contains("artifacts")) {
        parse_array_of(j["artifacts"], t.artifacts,
                       [](const json& v, Artifact& out) { from_json(v, out); });
    }
    if (j.contains("history")) {
        parse_array_of(j["history"], t.history,
                       [](const json& v, Message& out) { from_json(v, out); });
    }
    if (j.contains("metadata")) t.metadata = j["metadata"];
}

// ---------------------------------------------------------------------------
// MessageSendConfiguration / Params
// ---------------------------------------------------------------------------
void to_json(json& j, const MessageSendConfiguration& c) {
    to_json(j, c, WireDialect::V0_3);
}

void to_json(json& j, const MessageSendConfiguration& c, WireDialect dialect) {
    j = json::object();
    if (!c.accepted_output_modes.empty()) {
        auto arr = json::array();
        for (auto& s : c.accepted_output_modes) arr.push_back(s);
        j["acceptedOutputModes"] = std::move(arr);
    }
    if (c.blocking) {
        // 1.0 replaced `blocking` with the inverted `returnImmediately`.
        if (dialect == WireDialect::V0_3) j["blocking"] = *c.blocking;
        else                              j["returnImmediately"] = !*c.blocking;
    }
    if (c.history_length) j["historyLength"] = *c.history_length;
}

void from_json(const json& j, MessageSendConfiguration& c) {
    if (j.contains("acceptedOutputModes")) {
        auto arr = j["acceptedOutputModes"];
        if (arr.is_array())
            for (auto v : arr) c.accepted_output_modes.push_back(v.get<std::string>());
    }
    if (present(j, "blocking"))      c.blocking       = j.value("blocking", false);
    else if (present(j, "returnImmediately"))
        c.blocking = !j.value("returnImmediately", false);
    if (present(j, "historyLength")) c.history_length = j.value("historyLength", 0);
}

void to_json(json& j, const MessageSendParams& p) {
    to_json(j, p, WireDialect::V0_3);
}

void to_json(json& j, const MessageSendParams& p, WireDialect dialect) {
    j = json::object();
    json mj;
    to_json(mj, p.message, dialect);
    j["message"] = std::move(mj);
    if (p.configuration) {
        json cj;
        to_json(cj, *p.configuration, dialect);
        j["configuration"] = std::move(cj);
    }
    if (!p.metadata.is_null() && !p.metadata.empty()) j["metadata"] = p.metadata;
}

void from_json(const json& j, MessageSendParams& p) {
    if (j.contains("message")) from_json(j["message"], p.message);
    if (present(j, "configuration")) {
        MessageSendConfiguration c;
        from_json(j["configuration"], c);
        p.configuration = c;
    }
    if (j.contains("metadata")) p.metadata = j["metadata"];
}

// ---------------------------------------------------------------------------
// AgentCard
// ---------------------------------------------------------------------------
namespace {

json string_array(const std::vector<std::string>& values) {
    auto out = json::array();
    for (const auto& v : values) out.push_back(v);
    return out;
}

json modes_or_text(const std::vector<std::string>& modes) {
    // Both card generations list at least one media type; an agent built
    // without one speaks plain text.
    return modes.empty() ? json::array({"text/plain"}) : string_array(modes);
}

/// The skills a card serialises: the full objects when set, else one per
/// skill id, else a single default skill (1.0 requires a non-empty list).
/// Every skill carries at least one tag (1.0 REQUIRED, spec §5.7).
std::vector<AgentSkill> card_skills(const AgentCard& c) {
    std::vector<AgentSkill> skills = c.skills;
    if (skills.empty()) {
        for (const auto& id : c.skill_names) {
            AgentSkill s;
            s.id = id;
            s.name = id;
            s.description = "Skill: " + id;
            skills.push_back(std::move(s));
        }
    }
    if (skills.empty()) {
        AgentSkill s;
        s.id = "default";
        s.name = c.name.empty() ? std::string("default") : c.name;
        s.description = c.description.empty() ? std::string("General requests")
                                              : c.description;
        skills.push_back(std::move(s));
    }
    for (auto& s : skills) {
        if (s.name.empty()) s.name = s.id;
        if (s.description.empty()) s.description = "Skill: " + s.id;
        if (s.tags.empty()) s.tags.push_back(s.id.empty() ? std::string("default") : s.id);
    }
    return skills;
}

json skill_to_json(const AgentSkill& s) {
    json j = json::object();
    j["id"]          = s.id;
    j["name"]        = s.name;
    j["description"] = s.description;
    j["tags"]        = string_array(s.tags);
    if (!s.examples.empty())     j["examples"]    = string_array(s.examples);
    if (!s.input_modes.empty())  j["inputModes"]  = string_array(s.input_modes);
    if (!s.output_modes.empty()) j["outputModes"] = string_array(s.output_modes);
    return j;
}

json interfaces_to_json(const std::vector<AgentInterface>& interfaces) {
    auto arr = json::array();
    for (const auto& i : interfaces) {
        json ij = {{"url", i.url},
                   {"protocolBinding", i.protocol_binding},
                   {"protocolVersion", i.protocol_version}};
        if (!i.tenant.empty()) ij["tenant"] = i.tenant;
        arr.push_back(std::move(ij));
    }
    return arr;
}

/// 1.0 ProtoJSON SecurityScheme -> 0.3 discriminated SecurityScheme. A scheme
/// with no 0.3 equivalent is omitted from the 0.3 card.
std::optional<json> security_scheme_v03(const json& scheme) {
    if (!scheme.is_object()) return std::nullopt;
    auto copy_description = [&](const json& from, json& to) {
        if (from.contains("description") && from["description"].is_string())
            to["description"] = from["description"];
    };
    if (scheme.contains("httpAuthSecurityScheme")) {
        const auto s = scheme["httpAuthSecurityScheme"];
        json out = {{"type", "http"}, {"scheme", s.value("scheme", std::string("Bearer"))}};
        if (s.contains("bearerFormat")) out["bearerFormat"] = s["bearerFormat"];
        copy_description(s, out);
        return out;
    }
    if (scheme.contains("apiKeySecurityScheme")) {
        const auto s = scheme["apiKeySecurityScheme"];
        json out = {{"type", "apiKey"}, {"in", s.value("location", std::string("header"))},
                    {"name", s.value("name", std::string())}};
        copy_description(s, out);
        return out;
    }
    if (scheme.contains("oauth2SecurityScheme")) {
        const auto s = scheme["oauth2SecurityScheme"];
        json out = {{"type", "oauth2"}, {"flows", s.contains("flows") ? s["flows"] : json::object()}};
        if (s.contains("oauth2MetadataUrl")) out["oauth2MetadataUrl"] = s["oauth2MetadataUrl"];
        copy_description(s, out);
        return out;
    }
    if (scheme.contains("openIdConnectSecurityScheme")) {
        const auto s = scheme["openIdConnectSecurityScheme"];
        json out = {{"type", "openIdConnect"},
                    {"openIdConnectUrl", s.value("openIdConnectUrl", std::string())}};
        copy_description(s, out);
        return out;
    }
    if (scheme.contains("mtlsSecurityScheme")) {
        json out = {{"type", "mutualTLS"}};
        copy_description(scheme["mtlsSecurityScheme"], out);
        return out;
    }
    return std::nullopt;
}

void security_to_v03(const AgentCard& c, json& j) {
    if (c.security_schemes.is_object() && !c.security_schemes.empty()) {
        json schemes = json::object();
        for (auto [name, scheme] : c.security_schemes.items()) {
            if (auto converted = security_scheme_v03(scheme)) schemes[name] = std::move(*converted);
        }
        if (!schemes.empty()) j["securitySchemes"] = std::move(schemes);
    }
    if (c.security_requirements.is_array() && !c.security_requirements.empty()) {
        auto security = json::array();
        for (auto requirement : c.security_requirements) {
            if (!requirement.is_object() || !requirement.contains("schemes")) continue;
            json entry = json::object();
            for (auto [name, scopes] : requirement["schemes"].items()) {
                entry[name] = scopes.is_object() && scopes.contains("list") ? scopes["list"]
                                                                            : json::array();
            }
            security.push_back(std::move(entry));
        }
        if (!security.empty()) j["security"] = std::move(security);
    }
}

}  // namespace

void to_json(json& j, const AgentCard& c) {
    if (!c.raw.is_null()) {
        j = c.raw;
        return;
    }
    // The 0.3 card (spec v0.3.0 AgentCard). `supportedInterfaces` is the 1.0
    // field, listed so a 1.0 reader that sent no A2A-Version still finds the
    // 1.0 interface; 0.3 readers ignore it.
    j = json::object();
    j["name"]                = c.name;
    j["description"]         = c.description;
    j["url"]                 = c.url;
    j["version"]             = c.version;
    j["protocolVersion"]     = c.protocol_version.empty() ? std::string("0.3.0")
                                                          : c.protocol_version;
    j["preferredTransport"]  = c.preferred_transport;
    j["defaultInputModes"]   = modes_or_text(c.default_input_modes);
    j["defaultOutputModes"]  = modes_or_text(c.default_output_modes);

    json caps = json::object();
    caps["streaming"]         = c.streaming;
    caps["pushNotifications"] = c.push_notifications;
    if (c.extensions.is_array() && !c.extensions.empty()) caps["extensions"] = c.extensions;
    j["capabilities"] = std::move(caps);

    if (!c.supported_interfaces.empty()) {
        j["supportedInterfaces"] = interfaces_to_json(c.supported_interfaces);
    }

    // 0.3 signals the authenticated extended card with this top-level flag;
    // 1.0 moved it to capabilities.extendedAgentCard.
    j["supportsAuthenticatedExtendedCard"] = c.supports_authenticated_extended;
    security_to_v03(c, j);

    auto skills = json::array();
    for (const auto& s : card_skills(c)) skills.push_back(skill_to_json(s));
    j["skills"] = std::move(skills);
}

void to_json(json& j, const AgentCard& c, WireDialect dialect) {
    if (dialect == WireDialect::V0_3 || !c.raw.is_null()) {
        to_json(j, c);
        return;
    }
    // Pure 1.0 AgentCard (lf.a2a.v1.AgentCard, ProtoJSON): no `url`,
    // `protocolVersion`, `preferredTransport` or
    // `supportsAuthenticatedExtendedCard`.
    j = json::object();
    j["name"]        = c.name;
    j["description"] = c.description;
    auto interfaces = c.supported_interfaces;
    if (interfaces.empty() && !c.url.empty()) {
        interfaces.push_back(AgentInterface{c.url, "JSONRPC", "1.0", {}});
    }
    j["supportedInterfaces"] = interfaces_to_json(interfaces);
    j["version"] = c.version;

    json caps = json::object();
    caps["streaming"]         = c.streaming;
    caps["pushNotifications"] = c.push_notifications;
    caps["extendedAgentCard"] = c.extended_card || c.supports_authenticated_extended;
    if (c.extensions.is_array() && !c.extensions.empty()) caps["extensions"] = c.extensions;
    j["capabilities"] = std::move(caps);

    if (c.security_schemes.is_object() && !c.security_schemes.empty()) {
        j["securitySchemes"] = c.security_schemes;
    }
    if (c.security_requirements.is_array() && !c.security_requirements.empty()) {
        j["securityRequirements"] = c.security_requirements;
    }
    j["defaultInputModes"]  = modes_or_text(c.default_input_modes);
    j["defaultOutputModes"] = modes_or_text(c.default_output_modes);

    auto skills = json::array();
    for (const auto& s : card_skills(c)) skills.push_back(skill_to_json(s));
    j["skills"] = std::move(skills);
}

void from_json(const json& j, AgentCard& c) {
    c.raw                  = j;
    c.name                 = j.value("name", std::string());
    c.description          = j.value("description", std::string());
    c.url                  = j.value("url", std::string());
    c.version              = j.value("version", std::string());
    c.protocol_version     = j.value("protocolVersion", std::string());
    c.preferred_transport  = j.value("preferredTransport", std::string("JSONRPC"));
    if (j.contains("defaultInputModes")) {
        auto arr = j["defaultInputModes"];
        if (arr.is_array())
            for (auto v : arr) c.default_input_modes.push_back(v.get<std::string>());
    }
    if (j.contains("defaultOutputModes")) {
        auto arr = j["defaultOutputModes"];
        if (arr.is_array())
            for (auto v : arr) c.default_output_modes.push_back(v.get<std::string>());
    }

    // A card that omits a capability does not have it.
    c.streaming = false;
    c.push_notifications = false;
    c.extended_card = false;
    if (j.contains("capabilities")) {
        auto cap = j["capabilities"];
        if (cap.is_object()) {
            c.streaming          = cap.value("streaming", false);
            c.push_notifications = cap.value("pushNotifications", false);
            // 1.0 `capabilities.extendedAgentCard`; a 0.3 card uses the
            // top-level `supportsAuthenticatedExtendedCard` instead.
            c.extended_card      = cap.value("extendedAgentCard", false);
            if (cap.contains("extensions") && cap["extensions"].is_array())
                c.extensions = cap["extensions"];
        }
    }
    c.supports_authenticated_extended = j.value("supportsAuthenticatedExtendedCard", false);

    // 1.0 cards declare `supportedInterfaces` (preferred first). 0.3 cards
    // declare a primary url/preferredTransport plus `additionalInterfaces`
    // ({url, transport}); the primary is normalised into the first entry so
    // consumers see one ordered list for both generations.
    c.supported_interfaces.clear();
    const bool has_v1_interfaces =
        j.contains("supportedInterfaces") && j["supportedInterfaces"].is_array()
        && !j["supportedInterfaces"].empty();
    if (!has_v1_interfaces) {
        AgentInterface primary;
        primary.url              = c.url;
        primary.protocol_binding = c.preferred_transport;
        primary.protocol_version = c.protocol_version;
        c.supported_interfaces.push_back(std::move(primary));
    }
    if (has_v1_interfaces) {
        for (const auto& ij : j["supportedInterfaces"]) {
            if (!ij.is_object()) continue;
            AgentInterface i;
            i.url              = ij.value("url", std::string());
            i.protocol_binding = ij.value("protocolBinding", std::string());
            i.protocol_version = ij.value("protocolVersion", std::string());
            i.tenant           = ij.value("tenant", std::string());
            c.supported_interfaces.push_back(std::move(i));
        }
    }
    if (j.contains("additionalInterfaces") && j["additionalInterfaces"].is_array()) {
        for (const auto& ij : j["additionalInterfaces"]) {
            if (!ij.is_object()) continue;
            AgentInterface i;
            i.url              = ij.value("url", std::string());
            i.protocol_binding = ij.value("transport", std::string());
            i.protocol_version = c.protocol_version;
            c.supported_interfaces.push_back(std::move(i));
        }
    }
    if (has_v1_interfaces && !c.supported_interfaces.empty()) {
        const auto& first = c.supported_interfaces.front();
        if (c.url.empty())              c.url = first.url;
        if (c.protocol_version.empty()) c.protocol_version = first.protocol_version;
        if (!j.contains("preferredTransport") && !first.protocol_binding.empty())
            c.preferred_transport = first.protocol_binding;
    }

    if (j.contains("skills")) {
        auto arr = j["skills"];
        if (arr.is_array()) {
            auto strings = [](const json& value) {
                std::vector<std::string> out;
                if (value.is_array())
                    for (auto v : value)
                        if (v.is_string()) out.push_back(v.get<std::string>());
                return out;
            };
            for (auto s : arr) {
                if (!s.is_object() || !s.contains("id")) continue;
                AgentSkill skill;
                skill.id           = s.value("id", std::string());
                skill.name         = s.value("name", std::string());
                skill.description  = s.value("description", std::string());
                skill.tags         = strings(s.contains("tags") ? s["tags"] : json());
                skill.examples     = strings(s.contains("examples") ? s["examples"] : json());
                skill.input_modes  = strings(s.contains("inputModes") ? s["inputModes"] : json());
                skill.output_modes = strings(s.contains("outputModes") ? s["outputModes"] : json());
                c.skill_names.push_back(skill.id);
                c.skills.push_back(std::move(skill));
            }
        }
    }
    if (j.contains("securitySchemes") && j["securitySchemes"].is_object()) {
        c.security_schemes = j["securitySchemes"];
    }
    if (j.contains("securityRequirements") && j["securityRequirements"].is_array()) {
        c.security_requirements = j["securityRequirements"];
    }
}

// ---------------------------------------------------------------------------
// Streaming events
// ---------------------------------------------------------------------------
void to_json(json& j, const TaskStatusUpdateEvent& e) {
    to_json(j, e, WireDialect::V0_3);
}

void to_json(json& j, const TaskStatusUpdateEvent& e, WireDialect dialect) {
    j = json::object();
    if (dialect == WireDialect::V0_3) j["kind"] = "status-update";
    j["taskId"]    = e.task_id;
    j["contextId"] = e.context_id;
    json sj;
    to_json(sj, e.status, dialect);
    j["status"] = std::move(sj);
    // 1.0 dropped `final`: the stream simply closes on a terminal state.
    if (dialect == WireDialect::V0_3) j["final"] = e.final;
    if (!e.metadata.is_null() && !e.metadata.empty()) j["metadata"] = e.metadata;
}

void from_json(const json& j, TaskStatusUpdateEvent& e) {
    e.kind       = j.value("kind", std::string("status-update"));
    e.task_id    = j.value("taskId", std::string());
    e.context_id = j.value("contextId", std::string());
    if (j.contains("status")) from_json(j["status"], e.status);
    if (j.contains("final") || j.contains("kind")) {
        e.final = j.value("final", false);
    } else {
        // 1.0 has no `final` flag; the stream ends on a terminal or
        // interrupted state.
        switch (e.status.state) {
            case TaskState::Completed: case TaskState::Canceled:
            case TaskState::Failed:    case TaskState::Rejected:
            case TaskState::InputRequired: case TaskState::AuthRequired:
                e.final = true;
                break;
            default:
                e.final = false;
        }
    }
    if (j.contains("metadata")) e.metadata = j["metadata"];
}

void to_json(json& j, const TaskArtifactUpdateEvent& e) {
    to_json(j, e, WireDialect::V0_3);
}

void to_json(json& j, const TaskArtifactUpdateEvent& e, WireDialect dialect) {
    j = json::object();
    if (dialect == WireDialect::V0_3) j["kind"] = "artifact-update";
    j["taskId"]    = e.task_id;
    j["contextId"] = e.context_id;
    json aj;
    to_json(aj, e.artifact, dialect);
    j["artifact"] = std::move(aj);
    j["append"]    = e.append;
    j["lastChunk"] = e.last_chunk;
    if (!e.metadata.is_null() && !e.metadata.empty()) j["metadata"] = e.metadata;
}

void from_json(const json& j, TaskArtifactUpdateEvent& e) {
    e.kind       = j.value("kind", std::string("artifact-update"));
    e.task_id    = j.value("taskId", std::string());
    e.context_id = j.value("contextId", std::string());
    if (j.contains("artifact")) from_json(j["artifact"], e.artifact);
    e.append     = j.value("append", false);
    e.last_chunk = j.value("lastChunk", false);
    if (j.contains("metadata")) e.metadata = j["metadata"];
}

Task task_from_result(const json& result) {
    Task t;
    if (result.is_null()) {
        t.status.state = TaskState::Failed;
        return t;
    }
    auto message_to_task = [](const json& mj) {
        Task task;
        Message msg;
        from_json(mj, msg);
        task.id             = msg.task_id.value_or("");
        task.context_id     = msg.context_id.value_or("");
        task.status.state   = TaskState::Completed;
        task.status.message = msg;
        task.history.push_back(std::move(msg));
        return task;
    };
    if (result.is_object()) {
        // 1.0 wrappers: SendMessageResponse / StreamResponse payloads.
        if (result.contains("task") && result["task"].is_object()) {
            from_json(result["task"], t);
            return t;
        }
        if (result.contains("message") && result["message"].is_object()) {
            return message_to_task(result["message"]);
        }
        auto kind = result.value("kind", std::string());
        if (kind == "task") {
            from_json(result, t);
            return t;
        }
        if (kind == "message") return message_to_task(result);
        if (kind.empty()) {
            // Bare 1.0 Task (GetTask / CancelTask result) or Message.
            if (result.contains("id") && result.contains("status")) {
                from_json(result, t);
                return t;
            }
            if (result.contains("messageId") && result.contains("parts")) {
                return message_to_task(result);
            }
        }
    }
    t.status.state = TaskState::Unknown;
    t.metadata     = result;
    return t;
}

StreamEvent parse_stream_event(const json& j) {
    StreamEvent ev;
    auto kind = j.value("kind", std::string());
    auto is_wrapped = [&](const char* key) {
        return j.is_object() && j.contains(key) && j[key].is_object();
    };
    if (kind == "status-update" || is_wrapped("statusUpdate")) {
        ev.type = StreamEvent::Type::StatusUpdate;
        TaskStatusUpdateEvent s;
        from_json(kind.empty() ? j["statusUpdate"] : j, s);
        ev.status_update = std::move(s);
    } else if (kind == "artifact-update" || is_wrapped("artifactUpdate")) {
        ev.type = StreamEvent::Type::ArtifactUpdate;
        TaskArtifactUpdateEvent a;
        from_json(kind.empty() ? j["artifactUpdate"] : j, a);
        ev.artifact_update = std::move(a);
    } else if (kind == "message" || is_wrapped("message") || is_wrapped("task")) {
        ev.type = StreamEvent::Type::Task;
        ev.task = task_from_result(j);
    } else {
        ev.type = StreamEvent::Type::Task;
        Task t;
        from_json(j, t);
        ev.task = std::move(t);
    }
    return ev;
}

}  // namespace neograph::a2a
