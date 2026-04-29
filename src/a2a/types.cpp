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
}

// ---------------------------------------------------------------------------
// TaskState <-> string
// ---------------------------------------------------------------------------
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
    return TaskState::Unknown;
}

std::string role_to_string(Role r) {
    return r == Role::Agent ? "agent" : "user";
}

Role role_from_string(std::string_view s) {
    return s == "agent" ? Role::Agent : Role::User;
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
void to_json(json& j, const Message& m) {
    j = json::object();
    j["kind"]      = "message";
    j["messageId"] = m.message_id;
    j["role"]      = role_to_string(m.role);
    auto parts = json::array();
    for (auto& part : m.parts) {
        json pj;
        to_json(pj, part);
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
    if (j.contains("taskId"))    m.task_id    = j.value("taskId", std::string());
    if (j.contains("contextId")) m.context_id = j.value("contextId", std::string());
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
void to_json(json& j, const Artifact& a) {
    j = json::object();
    j["artifactId"] = a.artifact_id;
    auto parts = json::array();
    for (auto& part : a.parts) {
        json pj;
        to_json(pj, part);
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
    if (j.contains("name"))        a.name        = j.value("name", std::string());
    if (j.contains("description")) a.description = j.value("description", std::string());
    if (j.contains("metadata"))    a.metadata    = j["metadata"];
}

// ---------------------------------------------------------------------------
// TaskStatus
// ---------------------------------------------------------------------------
void to_json(json& j, const TaskStatus& s) {
    j = json::object();
    j["state"] = task_state_to_string(s.state);
    if (s.message) {
        json mj;
        to_json(mj, *s.message);
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
void to_json(json& j, const Task& t) {
    j = json::object();
    j["kind"]      = "task";
    j["id"]        = t.id;
    j["contextId"] = t.context_id;
    json sj;
    to_json(sj, t.status);
    j["status"] = std::move(sj);
    if (!t.artifacts.empty()) {
        auto arr = json::array();
        for (auto& a : t.artifacts) {
            json aj;
            to_json(aj, a);
            arr.push_back(std::move(aj));
        }
        j["artifacts"] = std::move(arr);
    }
    if (!t.history.empty()) {
        auto arr = json::array();
        for (auto& m : t.history) {
            json mj;
            to_json(mj, m);
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
    j = json::object();
    if (!c.accepted_output_modes.empty()) {
        auto arr = json::array();
        for (auto& s : c.accepted_output_modes) arr.push_back(s);
        j["acceptedOutputModes"] = std::move(arr);
    }
    if (c.blocking)       j["blocking"]      = *c.blocking;
    if (c.history_length) j["historyLength"] = *c.history_length;
}

void from_json(const json& j, MessageSendConfiguration& c) {
    if (j.contains("acceptedOutputModes")) {
        auto arr = j["acceptedOutputModes"];
        if (arr.is_array())
            for (auto v : arr) c.accepted_output_modes.push_back(v.get<std::string>());
    }
    if (j.contains("blocking"))      c.blocking       = j.value("blocking", false);
    if (j.contains("historyLength")) c.history_length = j.value("historyLength", 0);
}

void to_json(json& j, const MessageSendParams& p) {
    j = json::object();
    json mj;
    to_json(mj, p.message);
    j["message"] = std::move(mj);
    if (p.configuration) {
        json cj;
        to_json(cj, *p.configuration);
        j["configuration"] = std::move(cj);
    }
    if (!p.metadata.is_null() && !p.metadata.empty()) j["metadata"] = p.metadata;
}

void from_json(const json& j, MessageSendParams& p) {
    if (j.contains("message")) from_json(j["message"], p.message);
    if (j.contains("configuration")) {
        MessageSendConfiguration c;
        from_json(j["configuration"], c);
        p.configuration = c;
    }
    if (j.contains("metadata")) p.metadata = j["metadata"];
}

// ---------------------------------------------------------------------------
// AgentCard
// ---------------------------------------------------------------------------
void to_json(json& j, const AgentCard& c) {
    if (!c.raw.is_null()) {
        j = c.raw;
        return;
    }
    j = json::object();
    j["name"]                = c.name;
    j["description"]         = c.description;
    j["url"]                 = c.url;
    j["version"]             = c.version;
    j["protocolVersion"]     = c.protocol_version;
    j["preferredTransport"]  = c.preferred_transport;

    auto in = json::array();
    for (auto& s : c.default_input_modes) in.push_back(s);
    j["defaultInputModes"] = std::move(in);

    auto out_modes = json::array();
    for (auto& s : c.default_output_modes) out_modes.push_back(s);
    j["defaultOutputModes"] = std::move(out_modes);

    json caps = json::object();
    caps["streaming"]         = c.streaming;
    caps["pushNotifications"] = c.push_notifications;
    caps["extendedAgentCard"] = c.extended_card;
    j["capabilities"] = std::move(caps);

    j["supportsAuthenticatedExtendedCard"] = c.supports_authenticated_extended;

    auto skills = json::array();
    for (auto& name : c.skill_names) {
        json s = json::object();
        s["id"]   = name;
        s["name"] = name;
        skills.push_back(std::move(s));
    }
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

    if (j.contains("capabilities")) {
        auto cap = j["capabilities"];
        if (cap.is_object()) {
            c.streaming          = cap.value("streaming", false);
            c.push_notifications = cap.value("pushNotifications", false);
            c.extended_card      = cap.value("extendedAgentCard", false);
        }
    }
    c.supports_authenticated_extended = j.value("supportsAuthenticatedExtendedCard", false);

    if (j.contains("skills")) {
        auto arr = j["skills"];
        if (arr.is_array()) {
            for (auto s : arr) {
                if (s.is_object() && s.contains("id"))
                    c.skill_names.push_back(s.value("id", std::string()));
            }
        }
    }
}

}  // namespace neograph::a2a
