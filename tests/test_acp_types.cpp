// Round-trip JSON tests for ACP types — verifies camelCase field names
// (protocolVersion, clientCapabilities, sessionId, mimeType, sessionUpdate,
// stopReason) and discriminator strings match the wire spec at
// https://agentclientprotocol.com/protocol/schema.md.

#include <neograph/acp/types.h>

#include <gtest/gtest.h>

using namespace neograph::acp;
using neograph::json;

namespace {

TEST(ACPTypes, StopReasonRoundTrip) {
    // Wire-format strings come from the official ACP schema:
    // zed-industries/agent-client-protocol@main schema/schema.json.
    EXPECT_EQ(stop_reason_to_string(StopReason::EndTurn),         "end_turn");
    EXPECT_EQ(stop_reason_to_string(StopReason::MaxTokens),       "max_tokens");
    EXPECT_EQ(stop_reason_to_string(StopReason::MaxTurnRequests), "max_turn_requests");
    EXPECT_EQ(stop_reason_to_string(StopReason::Refusal),         "refusal");
    EXPECT_EQ(stop_reason_to_string(StopReason::Cancelled),       "cancelled");

    EXPECT_EQ(stop_reason_from_string("end_turn"),          StopReason::EndTurn);
    EXPECT_EQ(stop_reason_from_string("max_tokens"),        StopReason::MaxTokens);
    EXPECT_EQ(stop_reason_from_string("max_turn_requests"), StopReason::MaxTurnRequests);
    EXPECT_EQ(stop_reason_from_string("refusal"),           StopReason::Refusal);
    EXPECT_EQ(stop_reason_from_string("cancelled"),         StopReason::Cancelled);
    EXPECT_EQ(stop_reason_from_string("garbage"),           StopReason::EndTurn);
}

TEST(ACPTypes, TextContentBlockRoundTrip) {
    auto b = ContentBlock::text_block("hello");
    json j; to_json(j, b);
    EXPECT_EQ(j.value("type", std::string()), "text");
    EXPECT_EQ(j.value("text", std::string()), "hello");

    ContentBlock decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.type, "text");
    EXPECT_EQ(decoded.text, "hello");
}

TEST(ACPTypes, ResourceLinkContentBlockRoundTrip) {
    auto b = ContentBlock::resource_link("file:///x.md", "x.md", "text/markdown");
    b.size  = 1234;
    b.title = "Doc";

    json j; to_json(j, b);
    EXPECT_EQ(j.value("type", std::string()), "resource_link");
    EXPECT_EQ(j.value("uri",  std::string()), "file:///x.md");
    EXPECT_EQ(j.value("name", std::string()), "x.md");
    EXPECT_EQ(j.value("mimeType", std::string()), "text/markdown");
    EXPECT_EQ(j.value("size", 0), 1234);
    EXPECT_EQ(j.value("title", std::string()), "Doc");

    ContentBlock decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.type, "resource_link");
    EXPECT_EQ(decoded.uri,  "file:///x.md");
    EXPECT_EQ(decoded.name, "x.md");
    EXPECT_EQ(decoded.mime_type, "text/markdown");
    ASSERT_TRUE(decoded.size.has_value());
    EXPECT_EQ(*decoded.size, 1234);
}

TEST(ACPTypes, InitializeRequestUsesCamelCase) {
    InitializeRequest req;
    req.protocol_version       = 1;
    req.client_capabilities.fs.read_text_file = true;
    req.client_capabilities.terminal          = true;
    req.client_info = json{{"name", "vscode"}, {"version", "1.0"}};

    json j; to_json(j, req);
    EXPECT_EQ(j.value("protocolVersion", 0), 1);
    ASSERT_TRUE(j.contains("clientCapabilities"));
    EXPECT_EQ(j["clientCapabilities"]["fs"].value("readTextFile", false), true);
    EXPECT_EQ(j["clientCapabilities"].value("terminal", false), true);
    EXPECT_EQ(j["clientInfo"].value("name", std::string()), "vscode");

    InitializeRequest decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.protocol_version, 1);
    EXPECT_TRUE(decoded.client_capabilities.fs.read_text_file);
    EXPECT_TRUE(decoded.client_capabilities.terminal);
}

TEST(ACPTypes, AgentCapabilitiesNestsCamelCase) {
    AgentCapabilities caps;
    caps.load_session = true;
    caps.prompt.image = true;
    caps.prompt.embedded_context = true;
    caps.session.list = true;

    json j; to_json(j, caps);
    EXPECT_EQ(j.value("loadSession", false), true);
    ASSERT_TRUE(j.contains("promptCapabilities"));
    EXPECT_EQ(j["promptCapabilities"].value("image", false), true);
    EXPECT_EQ(j["promptCapabilities"].value("embeddedContext", false), true);
    ASSERT_TRUE(j.contains("sessionCapabilities"));
    EXPECT_TRUE(j["sessionCapabilities"].contains("list"));
}

TEST(ACPTypes, PromptRequestPreservesContentBlocks) {
    PromptRequest req;
    req.session_id = "sess-abc";
    req.prompt.push_back(ContentBlock::text_block("hi"));
    req.prompt.push_back(ContentBlock::resource_link("file:///a.txt", "a.txt"));

    json j; to_json(j, req);
    EXPECT_EQ(j.value("sessionId", std::string()), "sess-abc");
    ASSERT_TRUE(j["prompt"].is_array());
    EXPECT_EQ(j["prompt"].size(), 2u);
    EXPECT_EQ(j["prompt"][0].value("type", std::string()), "text");
    EXPECT_EQ(j["prompt"][1].value("type", std::string()), "resource_link");

    PromptRequest decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.session_id, "sess-abc");
    ASSERT_EQ(decoded.prompt.size(), 2u);
    EXPECT_EQ(decoded.prompt[0].text, "hi");
    EXPECT_EQ(decoded.prompt[1].uri,  "file:///a.txt");
}

TEST(ACPTypes, PromptResponseStopReasonField) {
    PromptResponse r;
    r.stop_reason = StopReason::Cancelled;
    json j; to_json(j, r);
    EXPECT_EQ(j.value("stopReason", std::string()), "cancelled");

    PromptResponse decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.stop_reason, StopReason::Cancelled);
}

TEST(ACPTypes, SessionNotificationCarriesUpdate) {
    SessionNotification n;
    n.session_id = "sess-1";
    n.update.session_update = "agent_message_chunk";
    n.update.content = ContentBlock::text_block("partial answer");

    json j; to_json(j, n);
    EXPECT_EQ(j.value("sessionId", std::string()), "sess-1");
    ASSERT_TRUE(j.contains("update"));
    EXPECT_EQ(j["update"].value("sessionUpdate", std::string()), "agent_message_chunk");
    ASSERT_TRUE(j["update"].contains("content"));
    EXPECT_EQ(j["update"]["content"].value("text", std::string()), "partial answer");
}

TEST(ACPTypes, ToolCallRoundTripPreservesUnknownFields) {
    // Spec carries fields we don't model (locations, rawInput, ...). The
    // `raw` bag must hold them so to_json round-trips without loss.
    json incoming = {
        {"toolCallId", "tc-1"},
        {"toolName",   "edit_file"},
        {"input",      {{"path", "x.cpp"}}},
        {"kind",       "edit"},
        {"locations",  json::array({json{{"path", "x.cpp"}, {"line", 10}}})},
    };
    ToolCall tc;
    from_json(incoming, tc);
    EXPECT_EQ(tc.tool_call_id, "tc-1");
    EXPECT_EQ(tc.tool_name,    "edit_file");
    EXPECT_EQ(tc.kind,         "edit");

    json out; to_json(out, tc);
    // Unknown field round-trips verbatim.
    ASSERT_TRUE(out.contains("locations"));
    EXPECT_EQ(out["locations"][0].value("path", std::string()), "x.cpp");
    EXPECT_EQ(out.value("toolCallId", std::string()), "tc-1");
}

TEST(ACPTypes, PermissionOptionRoundTrip) {
    PermissionOption opt;
    opt.option_id = "allow-once";
    opt.name      = "Allow once";
    opt.kind      = "allow_once";
    json j; to_json(j, opt);
    EXPECT_EQ(j.value("optionId", std::string()), "allow-once");
    EXPECT_EQ(j.value("kind",     std::string()), "allow_once");

    PermissionOption decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.option_id, "allow-once");
    EXPECT_EQ(decoded.name,      "Allow once");
    EXPECT_EQ(decoded.kind,      "allow_once");
}

TEST(ACPTypes, RequestPermissionOutcomeSelectedShape) {
    RequestPermissionOutcome out;
    out.kind      = PermissionOutcomeKind::Selected;
    out.option_id = "allow-once";
    json j; to_json(j, out);
    EXPECT_EQ(j.value("outcome",  std::string()), "selected");
    EXPECT_EQ(j.value("optionId", std::string()), "allow-once");

    RequestPermissionOutcome decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.kind,      PermissionOutcomeKind::Selected);
    EXPECT_EQ(decoded.option_id, "allow-once");
}

TEST(ACPTypes, RequestPermissionOutcomeCancelledOmitsOptionId) {
    RequestPermissionOutcome out;
    out.kind = PermissionOutcomeKind::Cancelled;
    json j; to_json(j, out);
    EXPECT_EQ(j.value("outcome", std::string()), "cancelled");
    EXPECT_FALSE(j.contains("optionId"));

    RequestPermissionOutcome decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.kind, PermissionOutcomeKind::Cancelled);
    EXPECT_TRUE(decoded.option_id.empty());
}

TEST(ACPTypes, NewSessionRequestCarriesMcpServers) {
    NewSessionRequest req;
    req.cwd = "/home/user/proj";
    McpServerConfig cfg;
    cfg.raw = json{{"name", "fs"}, {"command", "fs-mcp"}, {"args", json::array()}};
    req.mcp_servers.push_back(cfg);

    json j; to_json(j, req);
    EXPECT_EQ(j.value("cwd", std::string()), "/home/user/proj");
    ASSERT_TRUE(j["mcpServers"].is_array());
    EXPECT_EQ(j["mcpServers"][0].value("name", std::string()), "fs");

    NewSessionRequest decoded;
    from_json(j, decoded);
    EXPECT_EQ(decoded.cwd, "/home/user/proj");
    ASSERT_EQ(decoded.mcp_servers.size(), 1u);
    EXPECT_EQ(decoded.mcp_servers[0].raw.value("name", std::string()), "fs");
}

}  // namespace
