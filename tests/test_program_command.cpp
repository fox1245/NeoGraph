#include <neograph/program/command.h>

#include <gtest/gtest.h>

#include <limits>
#include <string>

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

}  // namespace
}  // namespace neograph::program
