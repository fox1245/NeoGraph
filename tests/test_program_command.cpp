#include <neograph/program/command.h>

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <utility>

namespace neograph::program {
namespace {

TEST(JavaScriptCommandTest, FactoriesProduceClosedVersionedCanonicalValues) {
    const auto call = JavaScriptCommand::call_core(
        "main:1", "main", json{{"requested", "draft"}});
    const auto encoded = call.to_json();
    EXPECT_EQ(encoded,
              (json{{"protocol_version", 1},
                    {"kind", "call_core"},
                    {"import_slot", JAVASCRIPT_IMPORT_SLOT_CALL_CORE},
                    {"source_site", "main:1"},
                    {"arguments", json{{"name", "main"},
                                         {"input", json{{"requested", "draft"}}}}}}));
    EXPECT_EQ(JavaScriptCommand::from_json(encoded), call);

    const auto group = JavaScriptCommand::join(
        "main:2", "quorum",
        {call, JavaScriptCommand::emit("main:3", json{{"status", "ready"}})}, 1);
    EXPECT_EQ(group.kind(), JavaScriptCommandKind::Join);
    EXPECT_EQ(group.import_slot(), JAVASCRIPT_IMPORT_SLOT_JOIN);
    EXPECT_EQ(JavaScriptCommand::from_json(group.to_json()), group);
}

TEST(JavaScriptCommandTest, RejectsUnknownAndMissingProtocolFields) {
    auto encoded = JavaScriptCommand::call_core("main:1", "main", json::object()).to_json();
    encoded["unexpected"] = true;
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);

    encoded = JavaScriptCommand::call_core("main:1", "main", json::object()).to_json();
    encoded["source_site"] = nullptr;
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);

    encoded = JavaScriptCommand::call_core("main:1", "main", json::object()).to_json();
    encoded["protocol_version"] = 2;
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);

    encoded = JavaScriptCommand::call_core("main:1", "main", json::object()).to_json();
    encoded["kind"] = "not_a_command";
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);
}

TEST(JavaScriptCommandTest, RejectsNonCanonicalAndForgedNestedMembers) {
    auto encoded = JavaScriptCommand::emit("main:1", json::object()).to_json();
    encoded["arguments"]["value"] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);

    encoded = JavaScriptCommand::join(
                  "main:1", "all",
                  {JavaScriptCommand::call_core("main:2", "main", json::object())})
                  .to_json();
    encoded["arguments"]["members"][0]["kind"] = "call_core";
    encoded["arguments"]["members"][0]["unexpected"] = true;
    EXPECT_THROW((void)JavaScriptCommand::from_json(encoded), std::invalid_argument);

    EXPECT_THROW((void)JavaScriptCommand::cancel_scope("main:1", "", "reason"),
                 std::invalid_argument);
    const auto capability = JavaScriptCommand::host_capability("main:1", 42, json::object());
    EXPECT_EQ(capability.kind(), JavaScriptCommandKind::HostCapability);
    EXPECT_EQ(capability.import_slot(), 42U);
    EXPECT_EQ(JavaScriptCommand::from_json(capability.to_json()), capability);
}

TEST(JavaScriptCommandTest, RejectsStructuredDepthBeforeRecursiveDecoding) {
    json encoded = JavaScriptCommand::call_core("main:leaf", "main", json::object()).to_json();
    for (std::size_t depth = 1; depth <= JAVASCRIPT_COMMAND_MAX_STRUCTURED_DEPTH; ++depth) {
        encoded = json{{"protocol_version", JAVASCRIPT_COMMAND_PROTOCOL_VERSION},
                       {"kind", "await"},
                       {"import_slot", JAVASCRIPT_IMPORT_SLOT_AWAIT},
                       {"source_site", "main:await"},
                       {"arguments", json{{"command", std::move(encoded)}}}};
    }

    try {
        (void)JavaScriptCommand::from_json(encoded);
        FAIL() << "expected structured-depth rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "JavaScript command exceeds maximum structured nesting depth");
    }
}

TEST(JavaScriptCommandTest, RejectsAggregateJoinMembersBeforeRecursiveDecoding) {
    const auto leaf = JavaScriptCommand::call_core("main:leaf", "main", json::object()).to_json();
    json       members = json::array();
    for (std::size_t index = 0; index < JAVASCRIPT_COMMAND_MAX_AGGREGATE_MEMBERS; ++index)
        members.push_back(leaf);
    const json encoded{{"protocol_version", JAVASCRIPT_COMMAND_PROTOCOL_VERSION},
                       {"kind", "join"},
                       {"import_slot", JAVASCRIPT_IMPORT_SLOT_JOIN},
                       {"source_site", "main:join"},
                       {"arguments", json{{"mode", "all"}, {"members", std::move(members)}}}};

    try {
        (void)JavaScriptCommand::from_json(encoded);
        FAIL() << "expected aggregate-member rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "JavaScript command exceeds maximum aggregate member count");
    }
}

}  // namespace
}  // namespace neograph::program
