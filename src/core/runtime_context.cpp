#include <neograph/runtime_context.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph {
namespace {

constexpr std::string_view RUNTIME_IDENTITY_PREAMBLE = "NeoGraph Runtime identity v1";

std::string runtime_identity(std::string_view domain, const json& body) {
    return detail::sha256_identity(
        RUNTIME_IDENTITY_PREAMBLE, domain, detail::canonical_json_bytes(body));
}

void require_object(const json& value, std::string_view name) {
    if (!value.is_object()) throw std::invalid_argument(std::string(name) + " must be an object");
}

std::string required_string(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::uint64_t required_u64(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be unsigned");
    }
    return value.at(key).get<unsigned long long>();
}

std::int32_t required_i32(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_number_integer()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be an integer");
    }
    const auto parsed = value.at(key).get<long long>();
    if (parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' is out of range");
    }
    return static_cast<std::int32_t>(parsed);
}

bool required_bool(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_boolean()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be boolean");
    }
    return value.at(key).get<bool>();
}

void require_sha256(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void require_format(const json& value,
                    std::string_view expected_format,
                    std::uint32_t expected_schema,
                    std::string_view name) {
    if (required_string(value, "format") != expected_format) {
        throw std::invalid_argument(std::string(name) + " has an unknown format");
    }
    if (required_u64(value, "storage_schema_version") != expected_schema) {
        throw std::invalid_argument(std::string(name) + " schema version is unsupported");
    }
}

std::optional<std::string> optional_string(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key)) return std::nullopt;
    if (!value.at(key).is_string()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::vector<std::string> required_string_array(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value.at(key).is_array()) {
        throw std::invalid_argument("Stored runtime value field '" + key + "' must be an array");
    }
    std::vector<std::string> result;
    result.reserve(value.at(key).size());
    for (const auto& item : value.at(key)) {
        if (!item.is_string()) {
            throw std::invalid_argument("Stored runtime value field '" + key +
                                        "' must contain strings");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

void normalize_id_set(std::vector<std::string>& values, std::string_view field) {
    for (const auto& value : values) require_sha256(value, field);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate identity");
    }
}

void validate_range(std::string_view feed_id,
                    std::uint64_t from,
                    std::uint64_t through,
                    std::string_view name) {
    if (from == 0 && through == 0) {
        if (!feed_id.empty()) {
            throw std::invalid_argument(std::string(name) + " has a feed without a sequence range");
        }
        return;
    }
    if (feed_id.empty() || from == 0 || through < from) {
        throw std::invalid_argument(std::string(name) + " has an invalid feed sequence range");
    }
    detail::validate_token(feed_id, std::string(name) + " feed_id");
}

json message_body(const ChatMessage& message) {
    json value;
    to_json(value, message);
    return value;
}

void validate_message(const ChatMessage& message) {
    static const std::set<std::string, std::less<>> roles = {
        "assistant", "system", "tool", "user"};
    if (roles.find(message.role) == roles.end()) {
        throw std::invalid_argument("Runtime history message has an unsupported role");
    }
    detail::validate_utf8(message.content);
    if (message.role == "tool" && message.tool_call_id.empty()) {
        throw std::invalid_argument("Runtime history tool message requires tool_call_id");
    }
    if (message.role != "assistant" && !message.tool_calls.empty()) {
        throw std::invalid_argument("Only assistant runtime history messages may contain tool_calls");
    }
    for (const auto& call : message.tool_calls) {
        detail::validate_token(call.id, "Runtime history tool call id");
        detail::validate_token(call.name, "Runtime history tool call name");
        detail::validate_utf8(call.arguments);
    }
    if (!message.tool_call_id.empty())
        detail::validate_token(message.tool_call_id, "Runtime history tool_call_id");
    if (!message.tool_name.empty())
        detail::validate_token(message.tool_name, "Runtime history tool_name");
    if (!message.tool_status.empty())
        detail::validate_token(message.tool_status, "Runtime history tool_status");
    for (const auto& image_url : message.image_urls) detail::validate_utf8(image_url);
    (void)detail::canonical_json_bytes(message_body(message));
}

void validate_history_authority(RuntimeTrustClass trust, std::string_view role) {
    const bool valid =
        (trust == RuntimeTrustClass::UntrustedInput && role == "user") ||
        (trust == RuntimeTrustClass::ModelOutput && role == "assistant") ||
        (trust == RuntimeTrustClass::ToolOutput && role == "tool") ||
        ((trust == RuntimeTrustClass::Developer || trust == RuntimeTrustClass::HostPolicy) &&
         role == "system");
    if (!valid) {
        throw std::invalid_argument("Runtime history trust class cannot assert this message role");
    }
}

template <typename Enum>
void require_known_enum(Enum value, std::string_view name) {
    if (to_string(value) == "unknown") {
        throw std::invalid_argument(std::string(name) + " is invalid");
    }
}

ChatMessage parse_message(const json& value) {
    require_object(value, "Stored RuntimeHistoryRecord message");
    detail::reject_unknown_fields(
        value, "Stored RuntimeHistoryRecord message",
        {"role", "content", "tool_calls", "tool_call_id", "tool_name", "tool_status",
         "tool_retryable", "tool_effect_uncertain", "image_urls"});
    if (!value.contains("role") || !value.at("role").is_string() ||
        !value.contains("content") || !value.at("content").is_string()) {
        throw std::invalid_argument("Stored RuntimeHistoryRecord message requires role and content");
    }
    if (value.contains("tool_calls")) {
        if (!value.at("tool_calls").is_array())
            throw std::invalid_argument("Stored runtime tool_calls must be an array");
        for (const auto& call : value.at("tool_calls")) {
            require_object(call, "Stored runtime tool call");
            detail::reject_unknown_fields(call, "Stored runtime tool call", {"id", "name", "arguments"});
            (void)required_string(call, "id");
            (void)required_string(call, "name");
            (void)required_string(call, "arguments");
        }
    }
    if (value.contains("image_urls")) (void)required_string_array(value, "image_urls");
    if (value.contains("tool_retryable") && !value.at("tool_retryable").is_boolean())
        throw std::invalid_argument("Stored runtime tool_retryable must be boolean");
    if (value.contains("tool_effect_uncertain") &&
        !value.at("tool_effect_uncertain").is_boolean())
        throw std::invalid_argument("Stored runtime tool_effect_uncertain must be boolean");
    ChatMessage result;
    from_json(value, result);
    validate_message(result);
    return result;
}

json seal(json body, std::string_view domain, std::string& id) {
    id = runtime_identity(domain, body);
    body["id"] = id;
    return body;
}

}  // namespace

std::string_view to_string(RuntimeTrustClass value) noexcept {
    switch (value) {
        case RuntimeTrustClass::UntrustedInput: return "untrusted_input";
        case RuntimeTrustClass::ModelOutput: return "model_output";
        case RuntimeTrustClass::ToolOutput: return "tool_output";
        case RuntimeTrustClass::Developer: return "developer";
        case RuntimeTrustClass::HostPolicy: return "host_policy";
    }
    return "unknown";
}

std::string_view to_string(ContextArtifactKind value) noexcept {
    switch (value) {
        case ContextArtifactKind::RawHistory: return "raw_history";
        case ContextArtifactKind::DerivedContext: return "derived_context";
        case ContextArtifactKind::RequiredSkill: return "required_skill";
        case ContextArtifactKind::HookOutput: return "hook_output";
        case ContextArtifactKind::HardConstraint: return "hard_constraint";
    }
    return "unknown";
}

std::string_view to_string(ContextPlacement value) noexcept {
    switch (value) {
        case ContextPlacement::BeforeLatestUser: return "before_latest_user";
        case ContextPlacement::AfterLatestUser: return "after_latest_user";
    }
    return "unknown";
}

std::string_view to_string(RuntimeGuaranteeProfile value) noexcept {
    switch (value) {
        case RuntimeGuaranteeProfile::Legacy: return "legacy";
        case RuntimeGuaranteeProfile::Recorded: return "recorded";
        case RuntimeGuaranteeProfile::Strict: return "strict";
    }
    return "unknown";
}

RuntimeTrustClass runtime_trust_class_from_string(std::string_view value) {
    if (value == "untrusted_input") return RuntimeTrustClass::UntrustedInput;
    if (value == "model_output") return RuntimeTrustClass::ModelOutput;
    if (value == "tool_output") return RuntimeTrustClass::ToolOutput;
    if (value == "developer") return RuntimeTrustClass::Developer;
    if (value == "host_policy") return RuntimeTrustClass::HostPolicy;
    throw std::invalid_argument("Unknown runtime trust class");
}

ContextArtifactKind context_artifact_kind_from_string(std::string_view value) {
    if (value == "raw_history") return ContextArtifactKind::RawHistory;
    if (value == "derived_context") return ContextArtifactKind::DerivedContext;
    if (value == "required_skill") return ContextArtifactKind::RequiredSkill;
    if (value == "hook_output") return ContextArtifactKind::HookOutput;
    if (value == "hard_constraint") return ContextArtifactKind::HardConstraint;
    throw std::invalid_argument("Unknown context artifact kind");
}

ContextPlacement context_placement_from_string(std::string_view value) {
    if (value == "before_latest_user") return ContextPlacement::BeforeLatestUser;
    if (value == "after_latest_user") return ContextPlacement::AfterLatestUser;
    throw std::invalid_argument("Unknown context placement");
}

RuntimeGuaranteeProfile runtime_guarantee_profile_from_string(std::string_view value) {
    if (value == "legacy") return RuntimeGuaranteeProfile::Legacy;
    if (value == "recorded") return RuntimeGuaranteeProfile::Recorded;
    if (value == "strict") return RuntimeGuaranteeProfile::Strict;
    throw std::invalid_argument("Unknown runtime guarantee profile");
}

struct RuntimeHistoryRecord::Impl {
    RuntimeHistoryRecordData data;
    std::string id;
    std::string canonical;
};

RuntimeHistoryRecord::RuntimeHistoryRecord(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

RuntimeHistoryRecord RuntimeHistoryRecord::create(RuntimeHistoryRecordData data) {
    require_known_enum(data.trust, "Runtime history trust class");
    detail::validate_token(data.feed_id, "Runtime history feed_id");
    detail::validate_token(data.message_id, "Runtime history message_id");
    if (data.sequence == 0) throw std::invalid_argument("Runtime history sequence must be positive");
    if ((data.sequence == 1) != !data.predecessor_id) {
        throw std::invalid_argument("Runtime history predecessor does not match its sequence");
    }
    if (data.predecessor_id) require_sha256(*data.predecessor_id, "Runtime history predecessor_id");
    if (data.source_media_type) detail::validate_token(*data.source_media_type, "source_media_type");
    validate_message(data.message);
    validate_history_authority(data.trust, data.message.role);
    if (data.source_payload) data.source_payload = detail::owned_json_copy(*data.source_payload);

    json body{{"format", "neograph-runtime-history-record"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"feed_id", data.feed_id},
              {"sequence", data.sequence},
              {"message_id", data.message_id},
              {"trust", std::string(to_string(data.trust))},
              {"message", message_body(data.message)}};
    if (data.source_payload) body["source_payload"] = *data.source_payload;
    if (data.source_media_type) body["source_media_type"] = *data.source_media_type;
    if (data.predecessor_id) body["predecessor_id"] = *data.predecessor_id;
    auto impl = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(seal(std::move(body), "runtime-history-record/v1", impl->id));
    return RuntimeHistoryRecord(std::move(impl));
}

RuntimeHistoryRecord RuntimeHistoryRecord::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored RuntimeHistoryRecord");
    detail::reject_unknown_fields(
        value, "Stored RuntimeHistoryRecord",
        {"format", "storage_schema_version", "id", "feed_id", "sequence", "message_id",
         "trust", "message", "source_payload", "source_media_type", "predecessor_id"});
    require_format(value, "neograph-runtime-history-record", STORAGE_SCHEMA_VERSION,
                   "Stored RuntimeHistoryRecord");
    const auto stored_id = required_string(value, "id");
    RuntimeHistoryRecordData data;
    data.feed_id = required_string(value, "feed_id");
    data.sequence = required_u64(value, "sequence");
    data.message_id = required_string(value, "message_id");
    data.trust = runtime_trust_class_from_string(required_string(value, "trust"));
    if (!value.contains("message")) throw std::invalid_argument("Stored history record has no message");
    data.message = parse_message(value.at("message"));
    if (value.contains("source_payload")) data.source_payload = detail::owned_json_copy(value.at("source_payload"));
    data.source_media_type = optional_string(value, "source_media_type");
    data.predecessor_id = optional_string(value, "predecessor_id");
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored RuntimeHistoryRecord id mismatch");
    return result;
}

const std::string& RuntimeHistoryRecord::feed_id() const noexcept { return impl_->data.feed_id; }
std::uint64_t RuntimeHistoryRecord::sequence() const noexcept { return impl_->data.sequence; }
const std::string& RuntimeHistoryRecord::message_id() const noexcept { return impl_->data.message_id; }
RuntimeTrustClass RuntimeHistoryRecord::trust() const noexcept { return impl_->data.trust; }
const ChatMessage& RuntimeHistoryRecord::message() const noexcept { return impl_->data.message; }
std::optional<json> RuntimeHistoryRecord::source_payload() const { return impl_->data.source_payload; }
const std::optional<std::string>& RuntimeHistoryRecord::source_media_type() const noexcept { return impl_->data.source_media_type; }
const std::optional<std::string>& RuntimeHistoryRecord::predecessor_id() const noexcept { return impl_->data.predecessor_id; }
const std::string& RuntimeHistoryRecord::id() const noexcept { return impl_->id; }
std::string RuntimeHistoryRecord::serialize_canonical() const { return impl_->canonical; }

struct ContextArtifact::Impl {
    ContextArtifactData data;
    std::string content_digest;
    std::string id;
    std::string canonical;
};

ContextArtifact::ContextArtifact(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ContextArtifact ContextArtifact::create(ContextArtifactData data) {
    require_known_enum(data.kind, "Context artifact kind");
    require_known_enum(data.placement, "Context artifact placement");
    detail::validate_token(data.producer_id, "Context artifact producer_id");
    require_sha256(data.source_digest, "Context artifact source_digest");
    detail::validate_token(data.media_type, "Context artifact media_type");
    validate_range(data.source_feed_id, data.covers_from_sequence,
                   data.covers_through_sequence, "Context artifact");
    if (data.kind == ContextArtifactKind::RequiredSkill &&
        (!data.required || !data.source_feed_id.empty())) {
        throw std::invalid_argument(
            "Required skill artifacts must be required and independent of runtime history");
    }
    if (data.kind == ContextArtifactKind::HardConstraint && !data.required) {
        throw std::invalid_argument("Hard constraint artifacts must be required");
    }
    data.content = detail::owned_json_copy(data.content);
    auto impl = std::make_shared<Impl>();
    impl->content_digest = runtime_identity("context-artifact-content/v1", data.content);
    json body{{"format", "neograph-context-artifact"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"kind", std::string(to_string(data.kind))},
              {"producer_id", data.producer_id},
              {"source_digest", data.source_digest},
              {"source_feed_id", data.source_feed_id},
              {"covers_from_sequence", data.covers_from_sequence},
              {"covers_through_sequence", data.covers_through_sequence},
              {"media_type", data.media_type},
              {"placement", std::string(to_string(data.placement))},
              {"priority", data.priority},
              {"required", data.required},
              {"content", data.content},
              {"content_digest", impl->content_digest}};
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(seal(std::move(body), "context-artifact/v1", impl->id));
    return ContextArtifact(std::move(impl));
}

ContextArtifact ContextArtifact::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ContextArtifact");
    detail::reject_unknown_fields(
        value, "Stored ContextArtifact",
        {"format", "storage_schema_version", "id", "kind", "producer_id", "source_digest",
         "source_feed_id", "covers_from_sequence", "covers_through_sequence", "media_type",
         "placement", "priority", "required", "content", "content_digest"});
    require_format(value, "neograph-context-artifact", STORAGE_SCHEMA_VERSION,
                   "Stored ContextArtifact");
    const auto stored_id = required_string(value, "id");
    const auto stored_content_digest = required_string(value, "content_digest");
    ContextArtifactData data;
    data.kind = context_artifact_kind_from_string(required_string(value, "kind"));
    data.producer_id = required_string(value, "producer_id");
    data.source_digest = required_string(value, "source_digest");
    data.source_feed_id = required_string(value, "source_feed_id");
    data.covers_from_sequence = required_u64(value, "covers_from_sequence");
    data.covers_through_sequence = required_u64(value, "covers_through_sequence");
    data.media_type = required_string(value, "media_type");
    data.placement = context_placement_from_string(required_string(value, "placement"));
    data.priority = required_i32(value, "priority");
    data.required = required_bool(value, "required");
    if (!value.contains("content")) throw std::invalid_argument("Stored ContextArtifact has no content");
    data.content = detail::owned_json_copy(value.at("content"));
    auto result = create(std::move(data));
    if (result.id() != stored_id || result.content_digest() != stored_content_digest)
        throw std::invalid_argument("Stored ContextArtifact digest mismatch");
    return result;
}

ContextArtifactKind ContextArtifact::kind() const noexcept { return impl_->data.kind; }
const std::string& ContextArtifact::producer_id() const noexcept { return impl_->data.producer_id; }
const std::string& ContextArtifact::source_digest() const noexcept { return impl_->data.source_digest; }
const std::string& ContextArtifact::source_feed_id() const noexcept { return impl_->data.source_feed_id; }
std::uint64_t ContextArtifact::covers_from_sequence() const noexcept { return impl_->data.covers_from_sequence; }
std::uint64_t ContextArtifact::covers_through_sequence() const noexcept { return impl_->data.covers_through_sequence; }
const std::string& ContextArtifact::media_type() const noexcept { return impl_->data.media_type; }
ContextPlacement ContextArtifact::placement() const noexcept { return impl_->data.placement; }
std::int32_t ContextArtifact::priority() const noexcept { return impl_->data.priority; }
bool ContextArtifact::required() const noexcept { return impl_->data.required; }
json ContextArtifact::content() const { return impl_->data.content; }
const std::string& ContextArtifact::content_digest() const noexcept { return impl_->content_digest; }
const std::string& ContextArtifact::id() const noexcept { return impl_->id; }
std::string ContextArtifact::serialize_canonical() const { return impl_->canonical; }

struct ContextEpoch::Impl {
    ContextEpochData data;
    std::string id;
    std::string canonical;
};

ContextEpoch::ContextEpoch(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ContextEpoch ContextEpoch::create(ContextEpochData data) {
    require_known_enum(data.guarantee_profile, "Context epoch guarantee profile");
    detail::validate_token(data.run_id, "Context epoch run_id");
    if (data.sequence == 0) throw std::invalid_argument("Context epoch sequence must be positive");
    if ((data.sequence == 1) != !data.predecessor_id)
        throw std::invalid_argument("Context epoch predecessor does not match its sequence");
    if (data.predecessor_id) require_sha256(*data.predecessor_id, "Context epoch predecessor_id");
    validate_range(data.feed_id, data.raw_from_sequence, data.raw_through_sequence, "Context epoch");
    require_sha256(data.raw_window_digest, "Context epoch raw_window_digest");
    normalize_id_set(data.artifact_ids, "Context epoch artifact_ids");
    json body{{"format", "neograph-context-epoch"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"run_id", data.run_id},
              {"sequence", data.sequence},
              {"feed_id", data.feed_id},
              {"raw_from_sequence", data.raw_from_sequence},
              {"raw_through_sequence", data.raw_through_sequence},
              {"raw_window_digest", data.raw_window_digest},
              {"artifact_ids", data.artifact_ids},
              {"guarantee_profile", std::string(to_string(data.guarantee_profile))}};
    if (data.predecessor_id) body["predecessor_id"] = *data.predecessor_id;
    auto impl = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(seal(std::move(body), "context-epoch/v1", impl->id));
    return ContextEpoch(std::move(impl));
}

ContextEpoch ContextEpoch::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ContextEpoch");
    detail::reject_unknown_fields(
        value, "Stored ContextEpoch",
        {"format", "storage_schema_version", "id", "run_id", "sequence", "predecessor_id",
         "feed_id", "raw_from_sequence", "raw_through_sequence", "raw_window_digest",
         "artifact_ids", "guarantee_profile"});
    require_format(value, "neograph-context-epoch", STORAGE_SCHEMA_VERSION, "Stored ContextEpoch");
    const auto stored_id = required_string(value, "id");
    ContextEpochData data;
    data.run_id = required_string(value, "run_id");
    data.sequence = required_u64(value, "sequence");
    data.predecessor_id = optional_string(value, "predecessor_id");
    data.feed_id = required_string(value, "feed_id");
    data.raw_from_sequence = required_u64(value, "raw_from_sequence");
    data.raw_through_sequence = required_u64(value, "raw_through_sequence");
    data.raw_window_digest = required_string(value, "raw_window_digest");
    data.artifact_ids = required_string_array(value, "artifact_ids");
    data.guarantee_profile = runtime_guarantee_profile_from_string(
        required_string(value, "guarantee_profile"));
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored ContextEpoch id mismatch");
    return result;
}

const std::string& ContextEpoch::run_id() const noexcept { return impl_->data.run_id; }
std::uint64_t ContextEpoch::sequence() const noexcept { return impl_->data.sequence; }
const std::optional<std::string>& ContextEpoch::predecessor_id() const noexcept { return impl_->data.predecessor_id; }
const std::string& ContextEpoch::feed_id() const noexcept { return impl_->data.feed_id; }
std::uint64_t ContextEpoch::raw_from_sequence() const noexcept { return impl_->data.raw_from_sequence; }
std::uint64_t ContextEpoch::raw_through_sequence() const noexcept { return impl_->data.raw_through_sequence; }
const std::string& ContextEpoch::raw_window_digest() const noexcept { return impl_->data.raw_window_digest; }
const std::vector<std::string>& ContextEpoch::artifact_ids() const noexcept { return impl_->data.artifact_ids; }
RuntimeGuaranteeProfile ContextEpoch::guarantee_profile() const noexcept { return impl_->data.guarantee_profile; }
const std::string& ContextEpoch::id() const noexcept { return impl_->id; }
std::string ContextEpoch::serialize_canonical() const { return impl_->canonical; }

struct ContextAssemblyReceipt::Impl {
    ContextAssemblyReceiptData data;
    std::string id;
    std::string canonical;
};

ContextAssemblyReceipt::ContextAssemblyReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ContextAssemblyReceipt ContextAssemblyReceipt::create_structural(ContextAssemblyReceiptData data) {
    require_sha256(data.context_epoch_id, "Context assembly context_epoch_id");
    require_sha256(data.normalized_request_digest, "Context assembly normalized_request_digest");
    require_sha256(data.message_window_digest, "Context assembly message_window_digest");
    normalize_id_set(data.artifact_ids, "Context assembly artifact_ids");
    normalize_id_set(data.required_skill_artifact_ids,
                     "Context assembly required_skill_artifact_ids");
    if (!std::includes(data.artifact_ids.begin(), data.artifact_ids.end(),
                       data.required_skill_artifact_ids.begin(),
                       data.required_skill_artifact_ids.end())) {
        throw std::invalid_argument("Required skill artifacts must be included in assembled artifacts");
    }
    validate_range(data.raw_from_sequence == 0 ? std::string_view{} : std::string_view{"receipt"},
                   data.raw_from_sequence, data.raw_through_sequence, "Context assembly");
    if (data.mandatory_input_tokens > data.estimated_input_tokens)
        throw std::invalid_argument("Mandatory token count exceeds estimated input tokens");
    json body{{"format", "neograph-context-assembly-receipt"},
              {"storage_schema_version", STORAGE_SCHEMA_VERSION},
              {"context_epoch_id", data.context_epoch_id},
              {"normalized_request_digest", data.normalized_request_digest},
              {"message_window_digest", data.message_window_digest},
              {"artifact_ids", data.artifact_ids},
              {"required_skill_artifact_ids", data.required_skill_artifact_ids},
              {"raw_from_sequence", data.raw_from_sequence},
              {"raw_through_sequence", data.raw_through_sequence},
              {"estimated_input_tokens", data.estimated_input_tokens},
              {"mandatory_input_tokens", data.mandatory_input_tokens}};
    auto impl = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(
        seal(std::move(body), "context-assembly-receipt/v1", impl->id));
    return ContextAssemblyReceipt(std::move(impl));
}

ContextAssemblyReceipt ContextAssemblyReceipt::create(
    ContextAssemblyReceiptData data,
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts) {
    auto result = create_structural(std::move(data));
    validate_context_assembly_receipt(result, epoch, artifacts);
    return result;
}

ContextAssemblyReceipt ContextAssemblyReceipt::parse(
    std::string_view stored_bytes,
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ContextAssemblyReceipt");
    detail::reject_unknown_fields(
        value, "Stored ContextAssemblyReceipt",
        {"format", "storage_schema_version", "id", "context_epoch_id",
         "normalized_request_digest", "message_window_digest", "artifact_ids",
         "required_skill_artifact_ids", "raw_from_sequence", "raw_through_sequence",
         "estimated_input_tokens", "mandatory_input_tokens"});
    require_format(value, "neograph-context-assembly-receipt", STORAGE_SCHEMA_VERSION,
                   "Stored ContextAssemblyReceipt");
    const auto stored_id = required_string(value, "id");
    ContextAssemblyReceiptData data;
    data.context_epoch_id = required_string(value, "context_epoch_id");
    data.normalized_request_digest = required_string(value, "normalized_request_digest");
    data.message_window_digest = required_string(value, "message_window_digest");
    data.artifact_ids = required_string_array(value, "artifact_ids");
    data.required_skill_artifact_ids = required_string_array(value, "required_skill_artifact_ids");
    data.raw_from_sequence = required_u64(value, "raw_from_sequence");
    data.raw_through_sequence = required_u64(value, "raw_through_sequence");
    data.estimated_input_tokens = required_u64(value, "estimated_input_tokens");
    data.mandatory_input_tokens = required_u64(value, "mandatory_input_tokens");
    auto result = create_structural(std::move(data));
    if (result.id() != stored_id)
        throw std::invalid_argument("Stored ContextAssemblyReceipt id mismatch");
    validate_context_assembly_receipt(result, epoch, artifacts);
    return result;
}

const std::string& ContextAssemblyReceipt::context_epoch_id() const noexcept { return impl_->data.context_epoch_id; }
const std::string& ContextAssemblyReceipt::normalized_request_digest() const noexcept { return impl_->data.normalized_request_digest; }
const std::string& ContextAssemblyReceipt::message_window_digest() const noexcept { return impl_->data.message_window_digest; }
const std::vector<std::string>& ContextAssemblyReceipt::artifact_ids() const noexcept { return impl_->data.artifact_ids; }
const std::vector<std::string>& ContextAssemblyReceipt::required_skill_artifact_ids() const noexcept { return impl_->data.required_skill_artifact_ids; }
std::uint64_t ContextAssemblyReceipt::raw_from_sequence() const noexcept { return impl_->data.raw_from_sequence; }
std::uint64_t ContextAssemblyReceipt::raw_through_sequence() const noexcept { return impl_->data.raw_through_sequence; }
std::uint64_t ContextAssemblyReceipt::estimated_input_tokens() const noexcept { return impl_->data.estimated_input_tokens; }
std::uint64_t ContextAssemblyReceipt::mandatory_input_tokens() const noexcept { return impl_->data.mandatory_input_tokens; }
const std::string& ContextAssemblyReceipt::id() const noexcept { return impl_->id; }
std::string ContextAssemblyReceipt::serialize_canonical() const { return impl_->canonical; }

void validate_context_assembly_receipt(
    const ContextAssemblyReceipt& receipt,
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts) {
    if (receipt.context_epoch_id() != epoch.id() ||
        receipt.raw_from_sequence() != epoch.raw_from_sequence() ||
        receipt.raw_through_sequence() != epoch.raw_through_sequence() ||
        receipt.artifact_ids() != epoch.artifact_ids()) {
        throw std::invalid_argument("Context assembly receipt does not bind its exact epoch");
    }
    std::vector<std::string> artifact_ids;
    std::vector<std::string> required_skill_ids;
    artifact_ids.reserve(artifacts.size());
    for (const auto& artifact : artifacts) {
        artifact_ids.push_back(artifact.id());
        if (artifact.kind() == ContextArtifactKind::RequiredSkill) {
            if (!artifact.required()) {
                throw std::invalid_argument("Selected skill artifact is not required");
            }
            required_skill_ids.push_back(artifact.id());
        }
    }
    normalize_id_set(artifact_ids, "Context assembly evidence artifacts");
    normalize_id_set(required_skill_ids, "Context assembly evidence skills");
    if (artifact_ids != receipt.artifact_ids() ||
        required_skill_ids != receipt.required_skill_artifact_ids()) {
        throw std::invalid_argument("Context assembly receipt artifact evidence does not match");
    }
}

}  // namespace neograph
