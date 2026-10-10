#pragma once

#include <neograph/json.h>
#include <core/native_archive.h>
#include <json/json.h>
#include <runtime/client.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace neograph::provider_codec {

static_assert(sp::EXPECTED_INTERFACE_REVISION == 6,
              "NeoGraph requires SchemaProvider 0.3.0 interface 6 headers");

// A versioned observational projection is not native replay authority. Only a
// verified NativeArchive record can restore private provider continuation seals.
inline constexpr std::string_view outcome_schema = "neograph.provider-outcome/v2";
inline constexpr std::string_view message_schema = "neograph.provider-message/v2";

namespace detail {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Enum> inline Enum enumeration(const json& value, Enum last) {
    require(value.is_number_integer(), "Invalid provider enum encoding");
    const auto number = value.get<std::int64_t>();
    require(number >= 0 && number <= static_cast<std::int64_t>(last),
            "Unknown provider enum encoding");
    return static_cast<Enum>(number);
}
inline std::uint64_t counter(const json& value) {
    require(value.is_number_unsigned() ||
                (value.is_number_integer() && value.get<std::int64_t>() >= 0),
            "Invalid provider uint64 counter");
    return value.get<std::uint64_t>();
}
inline json document(const std::shared_ptr<const sp::json::Document>& value) {
    if (!value) return nullptr;
    require(value->root().valid(), "Invalid owned provider JSON document");
    return json{{"json", value->root().dump()}};
}
inline std::shared_ptr<const sp::json::Document> parse_document(const json& value,
                                                             bool diagnostic = false) {
    if (value.is_null()) return {};
    require(value.is_object() && value.size() == 1 && value.at("json").is_string(),
            "Invalid provider JSON document encoding");
    const auto bytes = value.at("json").get<std::string_view>();
    // Reconstruct an already owned serialized document, not a fresh codec input.
    // Its exact extent bounds this iterative parse; the enclosing authenticated
    // archive independently enforces its admitted byte/depth/storage ceilings.
    auto parsed = sp::json::parse(bytes, {bytes.size(), bytes.size()});
    if (auto* accepted = std::get_if<sp::json::Document>(&parsed))
        return std::make_shared<const sp::json::Document>(std::move(*accepted));
    auto& rejected = std::get<sp::json::ParseError>(parsed);
    require(diagnostic && rejected.code == sp::json::ParseCode::DuplicateKey &&
                rejected.context.root().valid(), "Invalid provider JSON document");
    return std::make_shared<const sp::json::Document>(std::move(rejected.context));
}
inline json optional_string(const std::optional<std::string>& value) {
    return value ? json(*value) : json(nullptr);
}
inline std::optional<std::string> parse_string(const json& value) {
    return value.is_null() ? std::nullopt
                          : std::optional<std::string>(value.get<std::string>());
}
inline json count(const std::optional<sp::Count>& value) {
    return value ? json{{"value", value->value},
                        {"evidence", static_cast<int>(value->evidence)}}
                 : json(nullptr);
}
inline std::optional<sp::Count> parse_count(const json& value) {
    if (value.is_null()) return std::nullopt;
    return sp::Count{counter(value.at("value")),
                    enumeration(value.at("evidence"), sp::Evidence::Derived)};
}
inline json tool_host_metadata(const std::optional<sp::ToolResultHostMetadata>& value) {
    return value ? json{{"name", value->name}, {"status", value->status},
                        {"retryable", value->retryable}, {"effect_uncertain", value->effect_uncertain}}
                 : json(nullptr);
}
inline std::optional<sp::ToolResultHostMetadata> parse_tool_host_metadata(const json& value) {
    if (value.is_null()) return {};
    return sp::ToolResultHostMetadata{
        value.at("name").get<std::string>(), value.at("status").get<std::string>(),
        value.at("retryable").get<bool>(), value.at("effect_uncertain").get<bool>()};
}
inline json part(const sp::Part& value) {
    return std::visit([](const auto& item) -> json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, sp::Text>)
            return {{"type", "text"}, {"value", item.value}};
        else if constexpr (std::is_same_v<T, sp::Refusal>)
            return {{"type", "refusal"}, {"text", item.text}, {"raw_code", item.raw_code}};
        else if constexpr (std::is_same_v<T, sp::ToolCall>)
            return {{"type", "tool_call"}, {"id", item.id}, {"name", item.name},
                    {"kind", static_cast<int>(item.kind)}, {"input", document(item.input)},
                    {"wire_type", item.wire_type}, {"wire_metadata", document(item.wire_metadata)}};
        else if constexpr (std::is_same_v<T, sp::InvalidToolCall>)
            return {{"type", "invalid_tool_call"}, {"id", item.id}, {"name", item.name},
                    {"kind", static_cast<int>(item.kind)}, {"raw_fragment", item.raw_fragment},
                    {"reason", static_cast<int>(item.reason)}, {"wire_type", item.wire_type},
                    {"wire_metadata", document(item.wire_metadata)}};
        else if constexpr (std::is_same_v<T, sp::Thinking>)
            return {{"type", "thinking"}, {"text", item.text},
                    {"signature", optional_string(item.signature)}};
        else if constexpr (std::is_same_v<T, sp::RedactedThinking>)
            return {{"type", "redacted_thinking"}, {"data", item.data}};
        else if constexpr (std::is_same_v<T, sp::ServerToolResult>)
            return {{"type", "server_tool_result"}, {"tool_use_id", item.tool_use_id},
                    {"wire_type", item.wire_type}, {"content", document(item.content)}};
        else if constexpr (std::is_same_v<T, sp::ToolResult>)
            return {{"type", "tool_result"}, {"tool_use_id", item.tool_use_id},
                    {"content", item.content}, {"is_error", item.is_error},
                    {"host", tool_host_metadata(item.host)}};
        else if constexpr (std::is_same_v<T, sp::Reasoning>)
            return {{"type", "reasoning"}, {"id", item.id}, {"summary", item.summary},
                    {"encrypted_content", optional_string(item.encrypted_content)},
                    {"status", optional_string(item.status)}, {"content", item.content}};
        else if constexpr (std::is_same_v<T, sp::Opaque>)
            return {{"type", "opaque"}, {"wire_type", item.wire_type},
                    {"wire_metadata", document(item.wire_metadata)}};
        else if constexpr (std::is_same_v<T, sp::Image>)
            return {{"type", "image"}, {"mime", item.mime},
                    {"data", item.data ? json(*item.data) : json(nullptr)},
                    {"detail", static_cast<int>(item.detail)}};
        else if constexpr (std::is_same_v<T, sp::Thought>)
            return {{"type", "thought"}, {"summary", item.summary},
                    {"signature", optional_string(item.signature)}};
    }, value);
}
inline sp::Part parse_part(const json& value) {
    const auto type = value.at("type").get<std::string>();
    const auto text = [&](const char* key) { return value.at(key).get<std::string>(); };
    sp::Part result;
    if (type == "text") result = sp::Text{text("value")};
    else if (type == "refusal") result = sp::Refusal{text("text"), text("raw_code")};
    else if (type == "tool_call") result = sp::ToolCall{
        text("id"), text("name"), enumeration(value.at("kind"), sp::ToolCallKind::ApprovalRequest),
        parse_document(value.at("input")), text("wire_type"), parse_document(value.at("wire_metadata"), true)};
    else if (type == "invalid_tool_call") result = sp::InvalidToolCall{
        text("id"), text("name"), enumeration(value.at("kind"), sp::ToolCallKind::ApprovalRequest),
        text("raw_fragment"), enumeration(value.at("reason"), sp::InvalidReason::Other),
        text("wire_type"), parse_document(value.at("wire_metadata"), true)};
    else if (type == "thinking") result = sp::Thinking{text("text"), parse_string(value.at("signature"))};
    else if (type == "redacted_thinking") result = sp::RedactedThinking{text("data")};
    else if (type == "server_tool_result") result = sp::ServerToolResult{
        text("tool_use_id"), text("wire_type"), parse_document(value.at("content"), true)};
    else if (type == "tool_result") result = sp::ToolResult{
        text("tool_use_id"), text("content"), value.at("is_error").get<bool>(),
        parse_tool_host_metadata(value.at("host"))};
    else if (type == "reasoning") result = sp::Reasoning{
        text("id"), value.at("summary").get<std::vector<std::string>>(),
        parse_string(value.at("encrypted_content")), parse_string(value.at("status")),
        value.at("content").get<std::vector<std::string>>()};
    else if (type == "opaque") result = sp::Opaque{
        text("wire_type"), parse_document(value.at("wire_metadata"), true)};
    else if (type == "image") result = sp::Image{
        text("mime"), value.at("data").is_null() ? nullptr
            : std::make_shared<const std::string>(text("data")),
        enumeration(value.at("detail"), sp::ImageDetail::Original)};
    else if (type == "thought") result = sp::Thought{
        value.at("summary").get<std::vector<std::string>>(), parse_string(value.at("signature"))};
    else throw std::runtime_error("Unknown provider part encoding");
    require(part(result) == value, "Noncanonical provider part encoding");
    return result;
}
inline json stop(const sp::StopReason& value) {
    return {{"kind", static_cast<int>(value.kind)}, {"raw", value.raw},
            {"sequence", optional_string(value.sequence)}, {"details", document(value.details)}};
}
inline sp::StopReason parse_stop(const json& value) {
    sp::StopReason result{enumeration(value.at("kind"), sp::StopKind::Unknown),
                         value.at("raw").get<std::string>(),
                         parse_string(value.at("sequence")), parse_document(value.at("details"), true)};
    require(stop(result) == value, "Noncanonical provider stop encoding");
    return result;
}
inline json attempt(const sp::AttemptEvidence& value) {
    return {{"request_may_have_left", value.request_may_have_left},
            {"request_body_bytes", value.request_body_bytes},
            {"response_head_seen", value.response_head_seen},
            {"transport_internal_resends", value.transport_internal_resends},
            {"attempts", value.attempts}, {"prior_usage_unknown", value.prior_usage_unknown}};
}
inline sp::AttemptEvidence parse_attempt(const json& value) {
    sp::AttemptEvidence result;
    result.request_may_have_left = value.at("request_may_have_left").get<bool>();
    result.request_body_bytes = value.at("request_body_bytes").get<std::int64_t>();
    result.response_head_seen = value.at("response_head_seen").get<bool>();
    const auto resends = counter(value.at("transport_internal_resends"));
    const auto attempts = counter(value.at("attempts"));
    require(resends <= UINT32_MAX && attempts <= UINT32_MAX, "Invalid provider attempt counter");
    result.transport_internal_resends = static_cast<std::uint32_t>(resends);
    result.attempts = static_cast<std::uint32_t>(attempts);
    result.prior_usage_unknown = value.at("prior_usage_unknown").get<bool>();
    require(attempt(result) == value, "Noncanonical provider attempt encoding");
    return result;
}
inline json raw_events(const std::vector<sp::RawWire>& events) {
    auto result = json::array();
    for (const auto& event : events)
        result.push_back({{"type", event.type}, {"payload", document(event.payload)}});
    return result;
}
inline std::vector<sp::RawWire> parse_raw_events(const json& value) {
    require(value.is_array(), "Invalid provider raw event sequence");
    std::vector<sp::RawWire> result;
    result.reserve(value.size());
    for (const auto& event : value)
        result.push_back({event.at("type").get<std::string>(), parse_document(event.at("payload"), true)});
    require(raw_events(result) == value, "Noncanonical provider raw event encoding");
    return result;
}
inline json error(const sp::Error& value) {
    return {{"kind", static_cast<int>(value.kind)}, {"safe_message", value.safe_message},
            {"retry_class", static_cast<int>(value.retry_class)},
            {"retry_safety", static_cast<int>(value.retry_safety)}, {"http_status", value.http_status},
            {"vendor_code", value.vendor_code},
            {"retry_after_ms", value.retry_after ? json(value.retry_after->count()) : json(nullptr)},
            {"attempt", attempt(value.attempt)}};
}
inline sp::Error parse_error(const json& value) {
    sp::Error result;
    result.kind = enumeration(value.at("kind"), sp::ErrorKind::Overloaded);
    result.safe_message = value.at("safe_message").get<std::string>();
    result.retry_class = enumeration(value.at("retry_class"), sp::RetryClass::Unknown);
    result.retry_safety = enumeration(value.at("retry_safety"), sp::RetrySafety::OutputObserved);
    result.http_status = value.at("http_status").get<int>();
    result.vendor_code = value.at("vendor_code").get<std::string>();
    if (!value.at("retry_after_ms").is_null())
        result.retry_after = std::chrono::milliseconds(value.at("retry_after_ms").get<std::int64_t>());
    result.attempt = parse_attempt(value.at("attempt"));
    require(error(result) == value, "Noncanonical provider error encoding");
    return result;
}
} // namespace detail

inline json encode_message(const sp::Message& value) {
    auto parts = json::array();
    for (const auto& part : value.parts) parts.push_back(detail::part(part));
    return {{"schema", std::string(message_schema)}, {"id", value.id},
            {"role", static_cast<int>(value.role)}, {"parts", std::move(parts)},
            {"native", static_cast<bool>(value.native)}, {"wire_output", detail::document(value.wire_output)}};
}
inline sp::Message decode_message(const json& value) {
    detail::require(value.at("schema") == message_schema, "Incompatible provider message encoding");
    detail::require(!value.at("native").get<bool>(), "Native provider message requires trusted archive");
    sp::Message result;
    result.id = value.at("id").get<std::string>();
    result.role = detail::enumeration(value.at("role"), sp::Role::Tool);
    detail::require(value.at("parts").is_array(), "Invalid provider message parts");
    result.parts.reserve(value.at("parts").size());
    for (const auto& part : value.at("parts")) result.parts.push_back(detail::parse_part(part));
    result.wire_output = detail::parse_document(value.at("wire_output"), true);
    detail::require(encode_message(result) == value, "Noncanonical provider message encoding");
    return result;
}
// The SDK's fixed-size default carries no monetary observation. Preserve the
// absence-only v2 bytes without constructing a JSON monetary object.
inline bool provider_cost_absent(const sp::ProviderReportedCost& value) noexcept {
    return !value.total && !value.upstream_total && !value.upstream_input && !value.upstream_output &&
        !value.is_byok && value.byok_status == sp::CostStatus::Missing &&
        value.source == sp::CostSource::None && value.quality == sp::UsageQuality::Consistent &&
        std::all_of(value.status.begin(), value.status.end(),
                    [](sp::CostStatus status) { return status == sp::CostStatus::Missing; });
}
namespace detail {
inline void validate_provider_cost(const sp::ProviderReportedCost& value) {
    const auto valid = [](auto item, auto last) {
        return static_cast<int>(item) >= 0 && static_cast<int>(item) <= static_cast<int>(last);
    };
    require(valid(value.source, sp::CostSource::UnknownCurrency) &&
                valid(value.quality, sp::UsageQuality::Inconsistent),
            "Invalid provider cost source or quality");
    const std::optional<sp::UsdAmount>* amounts[]{
        &value.total, &value.upstream_total, &value.upstream_input, &value.upstream_output};
    bool inconsistent = false;
    for (std::size_t i = 0; i < value.status.size(); ++i) {
        const auto status = value.status[i];
        require(valid(status, sp::CostStatus::Conflict), "Invalid provider cost status");
        require((status == sp::CostStatus::Available) == amounts[i]->has_value(),
                "Provider cost amount/status mismatch");
        if (*amounts[i]) {
            const auto& amount = **amounts[i];
            require(value.source == sp::CostSource::OpenRouterUsd &&
                        amount.nano_usd <= (std::uint64_t{1} << 53) &&
                        valid(amount.evidence, sp::Evidence::Derived) &&
                        amount.rounding == sp::CostRounding::CeilingParsedBinary64,
                    "Invalid provider USD amount");
        }
        require(status != sp::CostStatus::UnknownCurrency ||
                    value.source == sp::CostSource::UnknownCurrency,
                "Provider cost currency/status mismatch");
        inconsistent |= status != sp::CostStatus::Missing && status != sp::CostStatus::Available;
    }
    require(value.byok_status == sp::CostStatus::Missing ||
                value.byok_status == sp::CostStatus::Available ||
                value.byok_status == sp::CostStatus::Malformed,
            "Invalid provider BYOK status");
    require((value.byok_status == sp::CostStatus::Available) == value.is_byok.has_value(),
            "Provider BYOK value/status mismatch");
    inconsistent |= value.byok_status == sp::CostStatus::Malformed;
    require(value.quality == (inconsistent ? sp::UsageQuality::Inconsistent : sp::UsageQuality::Consistent),
            "Provider cost quality/status mismatch");
    require(value.source != sp::CostSource::None || provider_cost_absent(value),
            "Provider cost metadata lacks a source");
}
inline json usd_amount(const std::optional<sp::UsdAmount>& value) {
    return value ? json{{"nano_usd", value->nano_usd},
                        {"evidence", static_cast<int>(value->evidence)},
                        {"rounding", static_cast<int>(value->rounding)}} : json(nullptr);
}
inline std::optional<sp::UsdAmount> parse_usd_amount(const json& value) {
    if (value.is_null()) return {};
    return sp::UsdAmount{counter(value.at("nano_usd")),
        enumeration(value.at("evidence"), sp::Evidence::Derived),
        enumeration(value.at("rounding"), sp::CostRounding::CeilingParsedBinary64)};
}
inline json provider_cost(const sp::ProviderReportedCost& value) {
    validate_provider_cost(value);
    auto status = json::array();
    for (const auto item : value.status) status.push_back(static_cast<int>(item));
    return {{"total", usd_amount(value.total)}, {"upstream_total", usd_amount(value.upstream_total)},
            {"upstream_input", usd_amount(value.upstream_input)}, {"upstream_output", usd_amount(value.upstream_output)},
            {"status", std::move(status)}, {"is_byok", value.is_byok ? json(*value.is_byok) : json(nullptr)},
            {"byok_status", static_cast<int>(value.byok_status)}, {"source", static_cast<int>(value.source)},
            {"quality", static_cast<int>(value.quality)}};
}
inline sp::ProviderReportedCost parse_provider_cost(const json& value) {
    sp::ProviderReportedCost result;
    result.total = parse_usd_amount(value.at("total"));
    result.upstream_total = parse_usd_amount(value.at("upstream_total"));
    result.upstream_input = parse_usd_amount(value.at("upstream_input"));
    result.upstream_output = parse_usd_amount(value.at("upstream_output"));
    const auto& status = value.at("status");
    require(status.is_array() && status.size() == result.status.size(), "Invalid provider cost status array");
    for (std::size_t i = 0; i < result.status.size(); ++i)
        result.status[i] = enumeration(status.at(i), sp::CostStatus::Conflict);
    if (!value.at("is_byok").is_null()) result.is_byok = value.at("is_byok").get<bool>();
    result.byok_status = enumeration(value.at("byok_status"), sp::CostStatus::Conflict);
    result.source = enumeration(value.at("source"), sp::CostSource::UnknownCurrency);
    result.quality = enumeration(value.at("quality"), sp::UsageQuality::Inconsistent);
    require(!provider_cost_absent(result), "Absent provider cost must be omitted");
    require(provider_cost(result) == value, "Noncanonical provider cost encoding");
    return result;
}
} // namespace detail
inline json encode_usage(const sp::Usage& value) {
    auto extra = json::object();
    for (const auto& [name, count] : value.extra) extra[name] = detail::count(count);
    auto conflicts = json::array();
    for (const auto& conflict : value.conflicts)
        conflicts.push_back({{"counter", conflict.counter}, {"detail", conflict.detail}});
    json result{{"input_total", detail::count(value.input_total)},
            {"output_total", detail::count(value.output_total)}, {"total", detail::count(value.total)},
            {"provider_reported_total", detail::count(value.provider_reported_total)},
            {"input_uncached", detail::count(value.input_uncached)}, {"cache_read", detail::count(value.cache_read)},
            {"cache_write", detail::count(value.cache_write)}, {"reasoning", detail::count(value.reasoning)},
            {"extra", std::move(extra)}, {"stage", static_cast<int>(value.stage)},
            {"quality", static_cast<int>(value.quality)}, {"conflicts", std::move(conflicts)}};
    if (!provider_cost_absent(value.provider_cost))
        result["provider_cost"] = detail::provider_cost(value.provider_cost);
    return result;
}
inline sp::Usage decode_usage(const json& value) {
    sp::Usage result;
    result.input_total = detail::parse_count(value.at("input_total"));
    result.output_total = detail::parse_count(value.at("output_total"));
    result.total = detail::parse_count(value.at("total"));
    result.provider_reported_total = detail::parse_count(value.at("provider_reported_total"));
    result.input_uncached = detail::parse_count(value.at("input_uncached"));
    result.cache_read = detail::parse_count(value.at("cache_read"));
    result.cache_write = detail::parse_count(value.at("cache_write"));
    result.reasoning = detail::parse_count(value.at("reasoning"));
    detail::require(value.at("extra").is_object() && value.at("conflicts").is_array(),
                    "Invalid provider usage encoding");
    for (const auto& [name, count] : value.at("extra").items()) {
        auto parsed = detail::parse_count(count);
        detail::require(parsed.has_value(), "Missing named provider usage counter");
        result.extra.emplace(name, *parsed);
    }
    result.stage = detail::enumeration(value.at("stage"), sp::UsageStage::Final);
    result.quality = detail::enumeration(value.at("quality"), sp::UsageQuality::Inconsistent);
    for (const auto& conflict : value.at("conflicts"))
        result.conflicts.push_back({conflict.at("counter").get<std::string>(),
                                   conflict.at("detail").get<std::string>()});
    if (value.contains("provider_cost"))
        result.provider_cost = detail::parse_provider_cost(value.at("provider_cost"));
    detail::require(encode_usage(result) == value, "Noncanonical provider usage encoding");
    return result;
}
inline json observe_outcome(const sp::Outcome& value) {
    return std::visit([](const auto& item) -> json {
        using T = std::decay_t<decltype(item)>;
        json result{{"schema", std::string(outcome_schema)}};
        const auto& messages = [&]() -> const std::vector<sp::Message>& {
            if constexpr (std::is_same_v<T, sp::Completion>) return item.messages;
            else return item.partial.messages;
        }();
        auto encoded = json::array();
        for (const auto& message : messages) encoded.push_back(encode_message(message));
        result["messages"] = std::move(encoded);
        if constexpr (std::is_same_v<T, sp::Completion>) {
            result["kind"] = "completion";
            result["usage"] = encode_usage(item.usage);
            result["stop"] = detail::stop(item.stop);
            result["wire_envelope"] = detail::document(item.wire_envelope);
            result["attempt"] = detail::attempt(item.attempt);
            result["raw_events"] = detail::raw_events(item.raw_events);
        } else {
            result["kind"] = "failure";
            result["usage"] = encode_usage(item.partial.usage);
            result["stop"] = item.partial.stop ? detail::stop(*item.partial.stop) : json(nullptr);
            result["error"] = detail::error(item.error);
            result["wire_envelope"] = detail::document(item.partial.wire_envelope);
            result["raw_events"] = detail::raw_events(item.partial.raw_events);
        }
        return result;
    }, value);
}
inline json encode_outcome(const sp::Outcome& value,
                           const std::shared_ptr<sp::NativeArchive>& archive = {},
                           std::string_view binding = {}) {
    auto result = observe_outcome(value);
    const auto& messages = std::visit([](const auto& item) -> const std::vector<sp::Message>& {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, sp::Completion>) return item.messages;
        else return item.partial.messages;
    }, value);
    if (archive) {
        std::vector<sp::Message> retained;
        retained.reserve(messages.size() + 1);
        sp::Message envelope;
        auto metadata = json::object();
        for (const auto& [key, field] : result.items())
            if (key != "messages") metadata[key] = field;
        envelope.parts.emplace_back(sp::Opaque{
            std::string(outcome_schema), detail::parse_document(json{{"json", metadata.dump()}})});
        retained.push_back(std::move(envelope));
        retained.insert(retained.end(), messages.begin(), messages.end());
        auto saved = archive->save(retained, binding);
        if (const auto* error = std::get_if<sp::Error>(&saved))
            throw std::runtime_error(error->safe_message);
        result["native_archive_reference"] = std::get<std::string>(std::move(saved));
    } else {
        for (const auto& message : messages)
            detail::require(!message.native, "Provider outcome requires trusted native archive");
        result["native_archive_reference"] = nullptr;
    }
    return result;
}
inline sp::runtime::Result decode_outcome(const json& value,
                                         const std::shared_ptr<sp::NativeArchive>& archive = {},
                                         std::string_view binding = {}) {
    detail::require(value.at("schema") == outcome_schema, "Incompatible provider outcome encoding");
    detail::require(value.at("messages").is_array(), "Invalid provider outcome messages");
    const auto reference = value.at("native_archive_reference");
    auto metadata = json::object();
    for (const auto& [key, field] : value.items())
        if (key != "native_archive_reference") metadata[key] = field;
    std::vector<sp::Message> messages;
    if (!reference.is_null()) {
        detail::require(static_cast<bool>(archive), "Provider outcome trusted archive is unavailable");
        auto authenticated_metadata = json::object();
        for (const auto& [key, field] : metadata.items())
            if (key != "messages") authenticated_metadata[key] = field;
        auto loaded = archive->load(reference.get<std::string>(), binding);
        if (const auto* error = std::get_if<sp::Error>(&loaded))
            throw std::runtime_error(error->safe_message);
        auto retained = std::get<std::vector<sp::Message>>(std::move(loaded));
        detail::require(!retained.empty(), "Provider outcome archive lacks authenticated metadata");
        const auto& envelope = retained.front();
        const auto* opaque = envelope.parts.size() == 1 ? std::get_if<sp::Opaque>(&envelope.parts.front()) : nullptr;
        detail::require(!envelope.native && envelope.id.empty() && envelope.role == sp::Role::Assistant &&
                            !envelope.wire_output && opaque && opaque->wire_type == outcome_schema &&
                            opaque->wire_metadata &&
                            json::parse(opaque->wire_metadata->root().dump()) == authenticated_metadata,
                        "Provider outcome authenticated metadata mismatch");
        messages.reserve(retained.size() - 1);
        std::move(std::next(retained.begin()), retained.end(), std::back_inserter(messages));
    } else {
        messages.reserve(value.at("messages").size());
        for (const auto& message : value.at("messages")) messages.push_back(decode_message(message));
    }
    sp::Outcome outcome;
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "completion") outcome = sp::Completion{
        std::move(messages), detail::parse_stop(value.at("stop")), decode_usage(value.at("usage")),
        detail::parse_document(value.at("wire_envelope"), true), detail::parse_attempt(value.at("attempt")),
        detail::parse_raw_events(value.at("raw_events"))};
    else if (kind == "failure") outcome = sp::Failure{
        detail::parse_error(value.at("error")), sp::PartialCompletion{
            std::move(messages), decode_usage(value.at("usage")), value.at("stop").is_null()
                ? std::nullopt : std::optional<sp::StopReason>(detail::parse_stop(value.at("stop"))),
            detail::parse_document(value.at("wire_envelope"), true), detail::parse_raw_events(value.at("raw_events"))}};
    else throw std::runtime_error("Unknown provider outcome encoding");
    detail::require(observe_outcome(outcome) == metadata, "Noncanonical provider outcome encoding");
    return std::make_shared<const sp::Outcome>(std::move(outcome));
}

} // namespace neograph::provider_codec
