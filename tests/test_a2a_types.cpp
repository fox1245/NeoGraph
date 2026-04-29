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

}  // namespace
