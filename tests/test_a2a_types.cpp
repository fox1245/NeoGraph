// Round-trip serialization tests for A2A types.
//
// These don't go on the wire — they verify that to_json / from_json
// preserve the spec field names and discriminators (kind="message",
// kind="task", role="user"|"agent", state="kebab-case", etc.).

#include <neograph/a2a/types.h>

#include <gtest/gtest.h>

using namespace neograph::a2a;
using neograph::json;

namespace {

TEST(A2ATypes, TaskStateRoundTrip) {
    EXPECT_EQ(task_state_from_string("submitted"),    TaskState::Submitted);
    EXPECT_EQ(task_state_from_string("input-required"), TaskState::InputRequired);
    EXPECT_EQ(task_state_from_string("auth-required"),  TaskState::AuthRequired);
    EXPECT_EQ(task_state_from_string("completed"),    TaskState::Completed);
    EXPECT_EQ(task_state_from_string("garbage"),      TaskState::Unknown);

    EXPECT_EQ(task_state_to_string(TaskState::Submitted),     "submitted");
    EXPECT_EQ(task_state_to_string(TaskState::InputRequired), "input-required");
}

TEST(A2ATypes, TextPartRoundTrips) {
    Part p = Part::text_part("hello");
    json j;
    to_json(j, p);
    EXPECT_EQ(j.value("kind", std::string()), "text");
    EXPECT_EQ(j.value("text", std::string()), "hello");

    Part decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.kind, "text");
    EXPECT_EQ(decoded.text, "hello");
}

TEST(A2ATypes, MessageHasKindMessageDiscriminator) {
    Message m;
    m.message_id = "msg-1";
    m.role       = Role::User;
    m.parts.push_back(Part::text_part("hi"));
    m.task_id    = "task-42";

    json j;
    to_json(j, m);
    EXPECT_EQ(j.value("kind", std::string()),      "message");
    EXPECT_EQ(j.value("messageId", std::string()), "msg-1");
    EXPECT_EQ(j.value("role", std::string()),      "user");
    EXPECT_EQ(j.value("taskId", std::string()),    "task-42");
    EXPECT_TRUE(j["parts"].is_array());

    Message decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.message_id, "msg-1");
    EXPECT_EQ(decoded.role,       Role::User);
    ASSERT_EQ(decoded.parts.size(), 1u);
    EXPECT_EQ(decoded.parts[0].text, "hi");
    EXPECT_TRUE(decoded.task_id.has_value());
    EXPECT_EQ(*decoded.task_id, "task-42");
}

TEST(A2ATypes, TaskParsesStatusAndHistory) {
    auto sample = json::parse(R"({
        "kind": "task",
        "id": "T1",
        "contextId": "ctx-1",
        "status": {
            "state": "working",
            "timestamp": "2026-04-29T00:00:00Z"
        },
        "history": [
            {
                "kind": "message",
                "messageId": "m1",
                "role": "user",
                "parts": [{"kind": "text", "text": "ping"}]
            },
            {
                "kind": "message",
                "messageId": "m2",
                "role": "agent",
                "parts": [{"kind": "text", "text": "pong"}]
            }
        ]
    })");

    Task t;
    from_json(sample, t);

    EXPECT_EQ(t.id,         "T1");
    EXPECT_EQ(t.context_id, "ctx-1");
    EXPECT_EQ(t.status.state, TaskState::Working);
    ASSERT_TRUE(t.status.timestamp.has_value());
    EXPECT_EQ(*t.status.timestamp, "2026-04-29T00:00:00Z");
    ASSERT_EQ(t.history.size(), 2u);
    EXPECT_EQ(t.history[0].role, Role::User);
    EXPECT_EQ(t.history[1].role, Role::Agent);
    EXPECT_EQ(t.history[1].parts[0].text, "pong");
}

TEST(A2ATypes, MessageSendParamsSerializes) {
    MessageSendParams p;
    p.message.message_id = "abc";
    p.message.role       = Role::User;
    p.message.parts.push_back(Part::text_part("Hello agent"));

    MessageSendConfiguration cfg;
    cfg.blocking = true;
    cfg.accepted_output_modes = {"text/plain", "application/json"};
    p.configuration = cfg;

    json j;
    to_json(j, p);
    EXPECT_TRUE(j.contains("message"));
    EXPECT_TRUE(j.contains("configuration"));
    auto cfg_j = j["configuration"];
    EXPECT_EQ(cfg_j.value("blocking", false), true);
    auto modes = cfg_j["acceptedOutputModes"];
    ASSERT_TRUE(modes.is_array());
    EXPECT_EQ(modes.size(), 2u);
}

TEST(A2ATypes, AgentCardParsesCanonicalFields) {
    auto sample = json::parse(R"({
        "name": "demo-agent",
        "description": "A demo A2A agent for testing.",
        "url": "https://demo.example/a2a",
        "version": "1.0.0",
        "protocolVersion": "0.3.0",
        "preferredTransport": "JSONRPC",
        "capabilities": {
            "streaming": true,
            "pushNotifications": false,
            "extendedAgentCard": false
        },
        "defaultInputModes": ["text/plain"],
        "defaultOutputModes": ["text/plain"],
        "skills": [
            {"id": "echo", "name": "echo", "description": "echo input"}
        ]
    })");

    AgentCard c;
    from_json(sample, c);

    EXPECT_EQ(c.name,                "demo-agent");
    EXPECT_EQ(c.url,                 "https://demo.example/a2a");
    EXPECT_EQ(c.protocol_version,    "0.3.0");
    EXPECT_EQ(c.preferred_transport, "JSONRPC");
    EXPECT_TRUE(c.streaming);
    EXPECT_FALSE(c.push_notifications);
    ASSERT_EQ(c.default_input_modes.size(), 1u);
    EXPECT_EQ(c.default_input_modes[0], "text/plain");
    ASSERT_EQ(c.skill_names.size(), 1u);
    EXPECT_EQ(c.skill_names[0], "echo");
}

TEST(A2ATypes, AgentCardPreservesUnknownFieldsInRaw) {
    auto sample = json::parse(R"({
        "name": "n",
        "description": "d",
        "url": "https://x",
        "version": "0",
        "protocolVersion": "0.3.0",
        "capabilities": {},
        "defaultInputModes": [],
        "defaultOutputModes": [],
        "skills": [],
        "x-future-extension": {"hint": "client should keep this"}
    })");

    AgentCard c;
    from_json(sample, c);
    ASSERT_TRUE(c.raw.contains("x-future-extension"));
    EXPECT_EQ(c.raw["x-future-extension"].value("hint", std::string()),
              "client should keep this");
}


// --- A2A 1.0 (protobuf-JSON) dialect -------------------------------------

TEST(A2ATypesV1, MessageHasNoKindAndUsesRoleEnum) {
    Message m;
    m.message_id = "m-1";
    m.role       = Role::User;
    m.parts.push_back(Part::text_part("hi"));
    m.context_id = "c-1";

    json j;
    to_json(j, m, WireDialect::V1_0);
    EXPECT_FALSE(j.contains("kind"));
    EXPECT_EQ(j["role"], "ROLE_USER");
    ASSERT_EQ(j["parts"].size(), 1u);
    EXPECT_FALSE(j["parts"][0].contains("kind"));
    EXPECT_EQ(j["parts"][0]["text"], "hi");

    // The 0.3 overload is unchanged.
    json legacy;
    to_json(legacy, m);
    EXPECT_EQ(legacy["kind"], "message");
    EXPECT_EQ(legacy["role"], "user");
    EXPECT_EQ(legacy["parts"][0]["kind"], "text");

    Message back;
    from_json(j, back);
    EXPECT_EQ(back.role, Role::User);
    EXPECT_EQ(back.parts[0].kind, "text");
    EXPECT_EQ(back.parts[0].text, "hi");
    EXPECT_EQ(back.context_id.value_or(""), "c-1");
}

TEST(A2ATypesV1, TaskStatesUseProtoEnumNames) {
    EXPECT_EQ(task_state_to_string(TaskState::InputRequired, WireDialect::V1_0),
              "TASK_STATE_INPUT_REQUIRED");
    EXPECT_EQ(task_state_to_string(TaskState::Unknown, WireDialect::V1_0),
              "TASK_STATE_UNSPECIFIED");
    EXPECT_EQ(task_state_to_string(TaskState::Working, WireDialect::V0_3), "working");
    for (auto st : {TaskState::Submitted, TaskState::Working, TaskState::InputRequired,
                    TaskState::Completed, TaskState::Canceled, TaskState::Failed,
                    TaskState::Rejected, TaskState::AuthRequired}) {
        EXPECT_EQ(task_state_from_string(task_state_to_string(st, WireDialect::V1_0)), st);
        EXPECT_EQ(task_state_from_string(task_state_to_string(st)), st);
    }
    EXPECT_EQ(role_to_string(Role::Agent, WireDialect::V1_0), "ROLE_AGENT");
    EXPECT_EQ(role_from_string("ROLE_AGENT"), Role::Agent);
}

TEST(A2ATypesV1, FilePartMapsToRawUrlFilenameMediaType) {
    Part bytes;
    bytes.kind = "file";
    bytes.file = {{"bytes", "aGk="}, {"name", "a.txt"}, {"mimeType", "text/plain"}};
    json j;
    to_json(j, bytes, WireDialect::V1_0);
    EXPECT_EQ(j, json::parse(R"({"raw":"aGk=","filename":"a.txt","mediaType":"text/plain"})"));

    Part decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.kind, "file");
    EXPECT_EQ(decoded.file["bytes"], "aGk=");
    EXPECT_EQ(decoded.file["name"], "a.txt");
    EXPECT_EQ(decoded.file["mimeType"], "text/plain");

    Part uri;
    from_json(json::parse(R"({"url":"https://x/y.png","mediaType":"image/png"})"), uri);
    EXPECT_EQ(uri.kind, "file");
    EXPECT_EQ(uri.file["uri"], "https://x/y.png");

    Part data;
    from_json(json::parse(R"({"data":{"k":1},"mediaType":"application/json"})"), data);
    EXPECT_EQ(data.kind, "data");
    EXPECT_EQ(data.data["k"], 1);
    json dj;
    to_json(dj, data, WireDialect::V1_0);
    EXPECT_EQ(dj, json::parse(R"({"data":{"k":1},"mediaType":"application/json"})"));
}

TEST(A2ATypesV1, TaskRoundTripWithoutKind) {
    auto j = json::parse(R"({
        "id": "t", "contextId": "c",
        "status": {"state": "TASK_STATE_WORKING", "timestamp": "2026-01-01T00:00:00Z"},
        "artifacts": [{"artifactId": "a", "parts": [{"text": "x"}]}],
        "history": [{"messageId": "m", "role": "ROLE_USER", "parts": [{"text": "q"}]}]
    })");
    Task t;
    from_json(j, t);
    EXPECT_EQ(t.status.state, TaskState::Working);
    ASSERT_EQ(t.history.size(), 1u);
    EXPECT_EQ(t.history[0].role, Role::User);

    json out;
    to_json(out, t, WireDialect::V1_0);
    EXPECT_FALSE(out.contains("kind"));
    EXPECT_EQ(out["status"]["state"], "TASK_STATE_WORKING");
    EXPECT_EQ(out["history"][0]["role"], "ROLE_USER");
    EXPECT_FALSE(out["history"][0].contains("kind"));
}

TEST(A2ATypesV1, SendConfigurationInvertsBlockingToReturnImmediately) {
    MessageSendConfiguration c;
    c.blocking = true;
    c.history_length = 4;
    json j;
    to_json(j, c, WireDialect::V1_0);
    EXPECT_EQ(j["returnImmediately"], false);
    EXPECT_FALSE(j.contains("blocking"));
    EXPECT_EQ(j["historyLength"], 4);

    MessageSendConfiguration back;
    from_json(j, back);
    ASSERT_TRUE(back.blocking.has_value());
    EXPECT_TRUE(*back.blocking);
}

TEST(A2ATypesV1, StreamEventsDecodeWrappersAndDeriveFinal) {
    auto status = parse_stream_event(json::parse(R"({"statusUpdate": {
        "taskId": "t", "contextId": "c", "status": {"state": "TASK_STATE_COMPLETED"}}})"));
    ASSERT_EQ(status.type, StreamEvent::Type::StatusUpdate);
    EXPECT_TRUE(status.is_final());

    auto working = parse_stream_event(json::parse(R"({"statusUpdate": {
        "taskId": "t", "contextId": "c", "status": {"state": "TASK_STATE_WORKING"}}})"));
    EXPECT_FALSE(working.is_final());

    auto artifact = parse_stream_event(json::parse(R"({"artifactUpdate": {
        "taskId": "t", "contextId": "c", "append": true, "lastChunk": true,
        "artifact": {"artifactId": "a", "parts": [{"text": "z"}]}}})"));
    ASSERT_EQ(artifact.type, StreamEvent::Type::ArtifactUpdate);
    EXPECT_TRUE(artifact.artifact_update->append);
    EXPECT_EQ(artifact.artifact_update->artifact.parts[0].text, "z");

    auto message = parse_stream_event(json::parse(R"({"message": {
        "messageId": "m", "role": "ROLE_AGENT", "parts": [{"text": "only"}]}})"));
    ASSERT_EQ(message.type, StreamEvent::Type::Task);
    EXPECT_EQ(message.task->history[0].parts[0].text, "only");

    // An opening Task snapshot is not the end of the stream.
    auto opening = parse_stream_event(json::parse(R"({"task": {
        "id": "t", "status": {"state": "TASK_STATE_SUBMITTED"}}})"));
    EXPECT_FALSE(opening.is_final());
    EXPECT_TRUE(message.is_final());

    // 0.3 frames still decode (final carried explicitly).
    auto legacy = parse_stream_event(json::parse(R"({"kind": "status-update",
        "taskId": "t", "contextId": "c", "final": false,
        "status": {"state": "completed"}})"));
    EXPECT_FALSE(legacy.is_final());
}

TEST(A2ATypesV1, TaskFromResultAcceptsBothGenerations) {
    EXPECT_EQ(task_from_result(json::parse(
        R"({"task": {"id": "a", "status": {"state": "TASK_STATE_FAILED"}}})")).status.state,
        TaskState::Failed);
    EXPECT_EQ(task_from_result(json::parse(
        R"({"id": "b", "status": {"state": "TASK_STATE_CANCELED"}})")).id, "b");
    EXPECT_EQ(task_from_result(json::parse(
        R"({"kind": "task", "id": "c", "status": {"state": "completed"}})")).id, "c");
    EXPECT_EQ(task_from_result(json()).status.state, TaskState::Failed);
    EXPECT_EQ(task_from_result(json::parse(R"({"weird": 1})")).status.state,
              TaskState::Unknown);
}

TEST(A2ATypesV1, AgentCardParsesSupportedInterfaces) {
    AgentCard c;
    from_json(json::parse(R"({
        "name": "n", "description": "d", "version": "1",
        "supportedInterfaces": [
            {"url": "https://a/rpc", "protocolBinding": "JSONRPC",
             "protocolVersion": "1.0", "tenant": "t1"},
            {"url": "grpc.a:443", "protocolBinding": "GRPC", "protocolVersion": "1.0"}],
        "capabilities": {"streaming": true},
        "defaultInputModes": [], "defaultOutputModes": [], "skills": []
    })"), c);
    ASSERT_EQ(c.supported_interfaces.size(), 2u);
    EXPECT_EQ(c.supported_interfaces[0].tenant, "t1");
    EXPECT_EQ(c.url, "https://a/rpc");
    EXPECT_EQ(c.protocol_version, "1.0");
    EXPECT_EQ(c.preferred_transport, "JSONRPC");

    AgentCard legacy;
    from_json(json::parse(R"({
        "name": "n", "url": "https://b/", "protocolVersion": "0.3.0",
        "preferredTransport": "JSONRPC",
        "additionalInterfaces": [{"url": "https://b/grpc", "transport": "GRPC"}]
    })"), legacy);
    ASSERT_EQ(legacy.supported_interfaces.size(), 2u);
    EXPECT_EQ(legacy.supported_interfaces[0].protocol_version, "0.3.0");
    EXPECT_EQ(legacy.supported_interfaces[1].protocol_binding, "GRPC");
}

}  // namespace
