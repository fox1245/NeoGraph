// Records written before SchemaProvider interface 7 (provider-message/v2, provider-outcome/v2,
// runtime history storage version 2) stay readable; new records are written as v3.
#include <gtest/gtest.h>

#include <neograph/provider_outcome_codec.h>
#include <neograph/runtime_context.h>
#include "../src/core/canonical_json.h"
#include "fixtures/typed_provider.h"

namespace {
using namespace neograph;
namespace codec = neograph::provider_codec;

constexpr const char* kPng = "iVBORw0KGgo=";

// The v2 form of a v3 message projection that holds no v3-only member.
json legacy_message(const json& current) {
    auto parts = json::array();
    for (const auto& part : current.at("parts")) {
        auto legacy = json::object();
        for (const auto& [key, value] : part.items())
            if (key != "citations" && key != "content_parts") legacy[key] = value;
        parts.push_back(std::move(legacy));
    }
    auto result = current;
    result["parts"] = std::move(parts);
    result["schema"] = std::string(codec::legacy_message_schema);
    return result;
}

json legacy_outcome(const json& current) {
    auto result = json::object();
    for (const auto& [key, value] : current.items())
        if (key != "model") result[key] = value;
    auto messages = json::array();
    for (const auto& message : current.at("messages")) messages.push_back(legacy_message(message));
    result["messages"] = std::move(messages);
    if (current.contains("error")) {
        auto error = json::object();
        for (const auto& [key, value] : current.at("error").items())
            if (key != "reason") error[key] = value;
        result["error"] = std::move(error);
    }
    result["schema"] = std::string(codec::legacy_outcome_schema);
    return result;
}

json v2_message_with_image() {
    return json{{"schema", "neograph.provider-message/v2"}, {"id", "m-1"}, {"role", 2},
                {"parts", json::array({json{{"type", "text"}, {"value", "look"}},
                                       json{{"type", "image"}, {"mime", "image/png"}, {"data", kPng}, {"detail", 2}},
                                       json{{"type", "tool_result"}, {"tool_use_id", "call-1"}, {"content", "ok"},
                                            {"is_error", false}, {"host", nullptr}}})},
                {"native", false}, {"wire_output", nullptr}};
}
}  // namespace

TEST(ProviderFormatUpgrade, LegacyImagePartReadsAsInlineImageMedia) {
    const auto message = codec::decode_message(v2_message_with_image());
    ASSERT_EQ(message.parts.size(), 3u);
    const auto& text = std::get<sp::Text>(message.parts[0]);
    EXPECT_EQ(text.value, "look");
    EXPECT_TRUE(text.citations.empty());
    const auto& image = std::get<sp::Media>(message.parts[1]);
    EXPECT_EQ(image.kind, sp::MediaKind::Image);
    EXPECT_EQ(image.source, sp::MediaSource::Inline);
    EXPECT_EQ(image.mime, "image/png");
    ASSERT_TRUE(image.data);
    EXPECT_EQ(*image.data, kPng);
    EXPECT_EQ(image.detail, sp::ImageDetail::High);
    EXPECT_TRUE(std::get<sp::ToolResult>(message.parts[2]).content_parts.empty());
    // Re-encoding writes v3, which is the upgraded form of the stored record.
    const auto encoded = codec::encode_message(message);
    EXPECT_EQ(encoded.at("schema"), std::string(codec::message_schema));
    EXPECT_EQ(encoded, codec::current_message(v2_message_with_image()));
    EXPECT_EQ(codec::current_message(encoded), encoded);
}

TEST(ProviderFormatUpgrade, LegacyRecordHoldingV3MembersIsRefused) {
    auto with_citations = v2_message_with_image();
    with_citations["parts"] = json::array({json{{"type", "text"}, {"value", "x"}, {"citations", json::array()}}});
    EXPECT_THROW(codec::decode_message(with_citations), std::runtime_error);
    auto with_media = v2_message_with_image();
    with_media["parts"] = json::array({json{{"type", "media"}}});
    EXPECT_THROW(codec::decode_message(with_media), std::runtime_error);
    auto extra_image_member = v2_message_with_image();
    extra_image_member["parts"] = json::array({json{{"type", "image"}, {"mime", "image/png"}, {"data", kPng},
                                                    {"detail", 0}, {"name", "x"}}});
    EXPECT_THROW(codec::decode_message(extra_image_member), std::runtime_error);
}

TEST(ProviderFormatUpgrade, LegacyOutcomesReadWithEmptyV3Members) {
    sp::Completion completion;
    completion.messages.push_back(test::message("answer"));
    completion.stop = {sp::StopKind::EndTurn, "stop"};
    completion.usage = test::usage(1, 2, 3);
    completion.model = {"requested-model", "served-model", sp::ServedRelation::Routed};
    const auto current = codec::encode_outcome(sp::Outcome(completion));
    EXPECT_EQ(current.at("model").at("served"), "served-model");
    const auto restored = codec::decode_outcome(legacy_outcome(current));
    const auto& read = std::get<sp::Completion>(*restored);
    EXPECT_EQ(read.messages.at(0).joined_text(), "answer");
    EXPECT_EQ(read.model.relation, sp::ServedRelation::NotReported);
    EXPECT_TRUE(read.model.requested.empty());

    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::RateLimited;
    failure.error.safe_message = "rate limited";
    failure.error.reason = "runtime.retry.exhausted";
    failure.partial.messages.push_back(test::message("partial"));
    const auto failed = codec::encode_outcome(sp::Outcome(failure));
    EXPECT_EQ(failed.at("error").at("reason"), "runtime.retry.exhausted");
    const auto legacy_failure = codec::decode_outcome(legacy_outcome(failed));
    EXPECT_TRUE(std::get<sp::Failure>(*legacy_failure).error.reason.empty());
    // A v2 record that already names a model is not a v2 record.
    auto forged = legacy_outcome(current);
    forged["model"] = current.at("model");
    EXPECT_THROW(codec::decode_outcome(forged), std::runtime_error);
}

TEST(ProviderFormatUpgrade, LegacyRuntimeHistoryRecordKeepsItsBytesAndId) {
    RuntimeHistoryRecordData data;
    data.feed_id = "legacy-feed";
    data.sequence = 1;
    data.message_id = "u-1";
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = sp::Message::user_text("hello");
    const auto current = RuntimeHistoryRecord::create(data);
    auto stored = detail::parse_json_strict(current.serialize_canonical());
    EXPECT_EQ(stored.at("storage_schema_version"), RuntimeHistoryRecord::STORAGE_SCHEMA_VERSION);

    // The same record as written by 0.15: storage version 2, a v2 message, an id sealed over that body.
    auto body = json::object();
    for (const auto& [key, value] : stored.items())
        if (key != "id") body[key] = value;
    body["storage_schema_version"] = RuntimeHistoryRecord::LEGACY_STORAGE_SCHEMA_VERSION;
    body["message"] = legacy_message(stored.at("message"));
    const auto legacy_id = detail::sha256_identity("NeoGraph Runtime identity v1", "runtime-history-record/v2",
                                                   detail::canonical_json_bytes(body));
    auto legacy = body;
    legacy["id"] = legacy_id;
    const auto bytes = detail::canonical_json_bytes(legacy);

    const auto read = RuntimeHistoryRecord::parse(bytes);
    EXPECT_EQ(read.id(), legacy_id);
    EXPECT_NE(read.id(), current.id());
    EXPECT_EQ(read.serialize_canonical(), bytes);
    EXPECT_EQ(read.message().joined_text(), "hello");

    auto edited = legacy;
    edited["message_id"] = "u-2";
    EXPECT_THROW(RuntimeHistoryRecord::parse(detail::canonical_json_bytes(edited)), std::invalid_argument);
}
