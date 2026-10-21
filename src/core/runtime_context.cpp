#include <neograph/runtime_context.h>
#include <neograph/provider_outcome_codec.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <map>
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

// The schema version of a stored value that exists in more than one version.
std::uint32_t require_format_version(const json& value,
                                     std::string_view expected_format,
                                     std::uint32_t legacy,
                                     std::uint32_t current,
                                     std::string_view name) {
    if (required_string(value, "format") != expected_format) {
        throw std::invalid_argument(std::string(name) + " has an unknown format");
    }
    const auto version = required_u64(value, "storage_schema_version");
    if (version != legacy && version != current) {
        throw std::invalid_argument(std::string(name) + " schema version is unsupported");
    }
    return static_cast<std::uint32_t>(version);
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

// A set of identities that keeps the order it was given.
void require_id_sequence(const std::vector<std::string>& values, std::string_view field) {
    auto sorted = values;
    normalize_id_set(sorted, field);
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

json message_body(const sp::Message& message) {
    return provider_codec::encode_message(message);
}

void validate_message(const sp::Message& message) {
    switch (message.role) {
        case sp::Role::Assistant:
        case sp::Role::System:
        case sp::Role::Developer:
        case sp::Role::Tool:
        case sp::Role::User: break;
        default: throw std::invalid_argument("Runtime history message has an unsupported role");
    }
    (void)detail::canonical_json_bytes(message_body(message));
}

void validate_history_authority(RuntimeTrustClass trust, sp::Role role) {
    const bool valid =
        (trust == RuntimeTrustClass::UntrustedInput && role == sp::Role::User) ||
        (trust == RuntimeTrustClass::ModelOutput && role == sp::Role::Assistant) ||
        (trust == RuntimeTrustClass::ToolOutput && role == sp::Role::Tool) ||
        (trust == RuntimeTrustClass::Developer &&
         (role == sp::Role::Developer || role == sp::Role::System)) ||
        (trust == RuntimeTrustClass::HostPolicy && role == sp::Role::System);
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


bool prefix_aware(const ContextAssemblyReceiptData& data) noexcept {
    return data.assembled_artifact_ids || data.encoder_family || data.stable_boundary ||
           data.prefix_boundary || data.prefix_digest || data.tools_digest || data.break_reason;
}

json boundary_body(const ContextPrefixBoundary& boundary) {
    return json{{"message_index", boundary.message_index}, {"part_index", boundary.part_index}};
}

ContextPrefixBoundary parse_boundary(const json& value, std::string_view field) {
    const std::string name = "Stored runtime value field '" + std::string(field) + "'";
    if (!value.is_object()) throw std::invalid_argument(name + " must be an object");
    detail::reject_unknown_fields(value, name, {"message_index", "part_index"});
    return {required_u64(value, "message_index"), required_u64(value, "part_index")};
}

std::optional<ContextPrefixBoundary> optional_boundary(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key)) return std::nullopt;
    return parse_boundary(value.at(key), field);
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
        case ContextArtifactKind::UntrustedSupplemental: return "untrusted_supplemental";
    }
    return "unknown";
}

std::string_view to_string(ContextPlacement value) noexcept {
    switch (value) {
        case ContextPlacement::BeforeHistory: return "before_history";
        case ContextPlacement::BeforeLatestUser: return "before_latest_user";
        case ContextPlacement::AfterLatestUser: return "after_latest_user";
        case ContextPlacement::AfterHistory: return "after_history";
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

std::string_view to_string(StabilityClass value) noexcept {
    switch (value) {
        case StabilityClass::Stable: return "stable";
        case StabilityClass::Slow: return "slow";
        case StabilityClass::Volatile: return "volatile";
    }
    return "unknown";
}

std::string_view to_string(AnchorMode value) noexcept {
    switch (value) {
        case AnchorMode::Replace: return "replace";
        case AnchorMode::KeepInPlace: return "keep_in_place";
    }
    return "unknown";
}

std::string_view to_string(ContextBreakReason value) noexcept {
    switch (value) {
        case ContextBreakReason::Compaction: return "compaction";
        case ContextBreakReason::StableChange: return "stable_change";
        case ContextBreakReason::LayoutChange: return "layout_change";
        case ContextBreakReason::AnchorDrop: return "anchor_drop";
        case ContextBreakReason::OrderChange: return "order_change";
        case ContextBreakReason::Explicit: return "explicit";
    }
    return "unknown";
}

std::string_view to_string(ContextPrefixClaim value) noexcept {
    switch (value) {
        case ContextPrefixClaim::Extends: return "extends";
        case ContextPrefixClaim::Breaks: return "breaks";
    }
    return "unknown";
}

std::string_view to_string(AssemblyLayout value) noexcept {
    switch (value) {
        case AssemblyLayout::Legacy: return "legacy";
        case AssemblyLayout::StableFirst: return "stable_first";
    }
    return "unknown";
}

std::string_view to_string(ClassSource value) noexcept {
    switch (value) {
        case ClassSource::Declared: return "declared";
        case ClassSource::Derived: return "derived";
    }
    return "unknown";
}

std::string_view to_string(ContextStabilityRefusal value) noexcept {
    switch (value) {
        case ContextStabilityRefusal::VolatileSystemArtifact: return "volatile_system_artifact";
        case ContextStabilityRefusal::PredecessorUnavailable: return "predecessor_unavailable";
        case ContextStabilityRefusal::PredecessorOrderUnknown: return "predecessor_order_unknown";
        case ContextStabilityRefusal::ArtifactsMismatch: return "artifacts_mismatch";
        case ContextStabilityRefusal::RawWindowMoved: return "raw_window_moved";
        case ContextStabilityRefusal::RawWindowNotGrown: return "raw_window_not_grown";
        case ContextStabilityRefusal::StablePrefixChanged: return "stable_prefix_changed";
        case ContextStabilityRefusal::AnchorDropped: return "anchor_dropped";
        case ContextStabilityRefusal::AnchorOutOfRange: return "anchor_out_of_range";
        case ContextStabilityRefusal::LayoutChangeUndeclared: return "layout_change_undeclared";
        case ContextStabilityRefusal::StableOutsideHistoryHead: return "stable_outside_history_head";
        case ContextStabilityRefusal::VolatileOutsideTail: return "volatile_outside_tail";
        case ContextStabilityRefusal::TierOrderViolated: return "tier_order_violated";
        case ContextStabilityRefusal::VolatileSystemText: return "volatile_system_text";
        case ContextStabilityRefusal::PrefixBoundaryUnavailable: return "prefix_boundary_unavailable";
        case ContextStabilityRefusal::AmbiguousOrder: return "ambiguous_order";
    }
    return "unknown";
}

ContextStabilityError::ContextStabilityError(ContextStabilityRefusal code, const std::string& message)
    : std::invalid_argument(message), code_(code) {}

ContextStabilityRefusal ContextStabilityError::code() const noexcept { return code_; }

bool context_artifact_kind_renders_as_system(ContextArtifactKind kind) noexcept {
    return kind == ContextArtifactKind::RequiredSkill || kind == ContextArtifactKind::HardConstraint;
}

StabilityClass derive_stability_class(ContextArtifactKind kind, std::string_view producer_id) noexcept {
    if (context_artifact_kind_renders_as_system(kind)) return StabilityClass::Stable;
    if (kind == ContextArtifactKind::DerivedContext && producer_id == COMPACTION_PRODUCER_ID) {
        return StabilityClass::Slow;
    }
    return StabilityClass::Volatile;
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
    if (value == "untrusted_supplemental") return ContextArtifactKind::UntrustedSupplemental;
    throw std::invalid_argument("Unknown context artifact kind");
}

ContextPlacement context_placement_from_string(std::string_view value) {
    if (value == "before_history") return ContextPlacement::BeforeHistory;
    if (value == "before_latest_user") return ContextPlacement::BeforeLatestUser;
    if (value == "after_latest_user") return ContextPlacement::AfterLatestUser;
    if (value == "after_history") return ContextPlacement::AfterHistory;
    throw std::invalid_argument("Unknown context placement");
}

RuntimeGuaranteeProfile runtime_guarantee_profile_from_string(std::string_view value) {
    if (value == "legacy") return RuntimeGuaranteeProfile::Legacy;
    if (value == "recorded") return RuntimeGuaranteeProfile::Recorded;
    if (value == "strict") return RuntimeGuaranteeProfile::Strict;
    throw std::invalid_argument("Unknown runtime guarantee profile");
}

StabilityClass stability_class_from_string(std::string_view value) {
    if (value == "stable") return StabilityClass::Stable;
    if (value == "slow") return StabilityClass::Slow;
    if (value == "volatile") return StabilityClass::Volatile;
    throw std::invalid_argument("Unknown stability class");
}

AnchorMode anchor_mode_from_string(std::string_view value) {
    if (value == "replace") return AnchorMode::Replace;
    if (value == "keep_in_place") return AnchorMode::KeepInPlace;
    throw std::invalid_argument("Unknown anchor mode");
}

ContextBreakReason context_break_reason_from_string(std::string_view value) {
    if (value == "compaction") return ContextBreakReason::Compaction;
    if (value == "stable_change") return ContextBreakReason::StableChange;
    if (value == "layout_change") return ContextBreakReason::LayoutChange;
    if (value == "anchor_drop") return ContextBreakReason::AnchorDrop;
    if (value == "order_change") return ContextBreakReason::OrderChange;
    if (value == "explicit") return ContextBreakReason::Explicit;
    throw std::invalid_argument("Unknown context break reason");
}

ContextPrefixClaim context_prefix_claim_from_string(std::string_view value) {
    if (value == "extends") return ContextPrefixClaim::Extends;
    if (value == "breaks") return ContextPrefixClaim::Breaks;
    throw std::invalid_argument("Unknown context prefix claim");
}

AssemblyLayout assembly_layout_from_string(std::string_view value) {
    if (value == "legacy") return AssemblyLayout::Legacy;
    if (value == "stable_first") return AssemblyLayout::StableFirst;
    throw std::invalid_argument("Unknown assembly layout");
}

ClassSource class_source_from_string(std::string_view value) {
    if (value == "declared") return ClassSource::Declared;
    if (value == "derived") return ClassSource::Derived;
    throw std::invalid_argument("Unknown class source");
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
    impl->canonical = detail::canonical_json_bytes(seal(std::move(body), "runtime-history-record/v3", impl->id));
    return RuntimeHistoryRecord(std::move(impl));
}

RuntimeHistoryRecord RuntimeHistoryRecord::parse(
    std::string_view stored_bytes, const std::shared_ptr<sp::NativeArchive>& archive,
    std::string_view owner_id) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored RuntimeHistoryRecord");
    detail::reject_unknown_fields(
        value, "Stored RuntimeHistoryRecord",
        {"format", "storage_schema_version", "id", "feed_id", "sequence", "message_id",
         "trust", "message", "source_payload", "source_media_type", "predecessor_id",
         "native_archive_reference"});
    const bool legacy = value.contains("storage_schema_version") &&
        value.at("storage_schema_version") == json(LEGACY_STORAGE_SCHEMA_VERSION);
    require_format(value, "neograph-runtime-history-record",
                   legacy ? LEGACY_STORAGE_SCHEMA_VERSION : STORAGE_SCHEMA_VERSION,
                   "Stored RuntimeHistoryRecord");
    const auto stored_id = required_string(value, "id");
    RuntimeHistoryRecordData data;
    data.feed_id = required_string(value, "feed_id");
    data.sequence = required_u64(value, "sequence");
    data.message_id = required_string(value, "message_id");
    data.trust = runtime_trust_class_from_string(required_string(value, "trust"));
    if (!value.contains("message")) throw std::invalid_argument("Stored history record has no message");
    const auto reference = optional_string(value, "native_archive_reference");
    if (reference) {
        if (!archive || archive->owner_scope() != owner_id) {
            throw std::invalid_argument("Runtime native history requires its owner-scoped archive");
        }
        auto loaded = archive->load(*reference,
            std::string(owner_id) + "/runtime-history/" + stored_id);
        if (const auto* error = std::get_if<sp::Error>(&loaded)) {
            throw std::invalid_argument(error->safe_message);
        }
        auto& messages = std::get<std::vector<sp::Message>>(loaded);
        if (messages.size() != 1 ||
            message_body(messages.front()) != provider_codec::current_message(value.at("message"))) {
            throw std::invalid_argument("Runtime native history archive projection mismatch");
        }
        data.message = std::move(messages.front());
    } else {
        data.message = provider_codec::decode_message(value.at("message"));
    }
    if (value.contains("source_payload")) data.source_payload = detail::owned_json_copy(value.at("source_payload"));
    data.source_media_type = optional_string(value, "source_media_type");
    data.predecessor_id = optional_string(value, "predecessor_id");
    auto result = create(std::move(data));
    if (legacy) {
        // The id was sealed over the body as stored, with its v2 message.
        auto body = json::object();
        for (const auto& [key, field] : value.items())
            if (key != "id" && key != "native_archive_reference") body[key] = field;
        if (runtime_identity("runtime-history-record/v2", body) != stored_id)
            throw std::invalid_argument("Stored RuntimeHistoryRecord id mismatch");
        auto impl = std::make_shared<Impl>(*result.impl_);
        impl->id = stored_id;
        impl->canonical = detail::canonical_json_bytes(value);
        return RuntimeHistoryRecord(std::move(impl));
    }
    if (result.id() != stored_id) throw std::invalid_argument("Stored RuntimeHistoryRecord id mismatch");
    if (reference) {
        auto impl = std::make_shared<Impl>(*result.impl_);
        impl->canonical = detail::canonical_json_bytes(value);
        result = RuntimeHistoryRecord(std::move(impl));
    }
    return result;
}

const std::string& RuntimeHistoryRecord::feed_id() const noexcept { return impl_->data.feed_id; }
std::uint64_t RuntimeHistoryRecord::sequence() const noexcept { return impl_->data.sequence; }
const std::string& RuntimeHistoryRecord::message_id() const noexcept { return impl_->data.message_id; }
RuntimeTrustClass RuntimeHistoryRecord::trust() const noexcept { return impl_->data.trust; }
const sp::Message& RuntimeHistoryRecord::message() const noexcept { return impl_->data.message; }
std::optional<json> RuntimeHistoryRecord::source_payload() const { return impl_->data.source_payload; }
const std::optional<std::string>& RuntimeHistoryRecord::source_media_type() const noexcept { return impl_->data.source_media_type; }
const std::optional<std::string>& RuntimeHistoryRecord::predecessor_id() const noexcept { return impl_->data.predecessor_id; }
const std::string& RuntimeHistoryRecord::id() const noexcept { return impl_->id; }
std::string RuntimeHistoryRecord::serialize_canonical() const { return impl_->canonical; }

std::string RuntimeHistoryRecord::serialize_canonical(
    const std::shared_ptr<sp::NativeArchive>& archive, std::string_view owner_id) const {
    if (!message().native) return impl_->canonical;
    if (!archive || archive->owner_scope() != owner_id) {
        throw std::invalid_argument("Runtime native history requires its owner-scoped archive");
    }
    auto body = detail::parse_json_strict(impl_->canonical);
    auto saved = archive->save({message()},
        std::string(owner_id) + "/runtime-history/" + id());
    if (const auto* error = std::get_if<sp::Error>(&saved)) {
        throw std::invalid_argument(error->safe_message);
    }
    body["native_archive_reference"] = std::get<std::string>(std::move(saved));
    // The record identity binds the entire full-message projection. The archive
    // authenticates that identity and owner; references grant no JSON authority.
    return detail::canonical_json_bytes(body);
}

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
    if (data.stability_class) require_known_enum(*data.stability_class, "Context artifact stability class");
    if (data.anchor_mode) require_known_enum(*data.anchor_mode, "Context artifact anchor mode");
    if (data.stability_class == StabilityClass::Volatile &&
        context_artifact_kind_renders_as_system(data.kind)) {
        throw ContextStabilityError(
            ContextStabilityRefusal::VolatileSystemArtifact,
            "A Volatile context artifact cannot be of a kind that renders as a System message");
    }
    data.content = detail::owned_json_copy(data.content);
    auto impl = std::make_shared<Impl>();
    impl->content_digest = runtime_identity("context-artifact-content/v1", data.content);
    // An artifact that declares neither field is a version 1 artifact, byte for byte.
    const bool declared = data.stability_class || data.anchor_mode;
    json body{{"format", "neograph-context-artifact"},
              {"storage_schema_version", declared ? STORAGE_SCHEMA_VERSION : LEGACY_STORAGE_SCHEMA_VERSION},
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
    if (data.stability_class) body["stability_class"] = std::string(to_string(*data.stability_class));
    if (data.anchor_mode) body["anchor_mode"] = std::string(to_string(*data.anchor_mode));
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(
        seal(std::move(body), declared ? "context-artifact/v2" : "context-artifact/v1", impl->id));
    return ContextArtifact(std::move(impl));
}

ContextArtifact ContextArtifact::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ContextArtifact");
    detail::reject_unknown_fields(
        value, "Stored ContextArtifact",
        {"format", "storage_schema_version", "id", "kind", "producer_id", "source_digest",
         "source_feed_id", "covers_from_sequence", "covers_through_sequence", "media_type",
         "placement", "priority", "required", "content", "content_digest", "stability_class",
         "anchor_mode"});
    const auto version = require_format_version(
        value, "neograph-context-artifact", LEGACY_STORAGE_SCHEMA_VERSION, STORAGE_SCHEMA_VERSION,
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
    if (const auto stability = optional_string(value, "stability_class")) {
        data.stability_class = stability_class_from_string(*stability);
    }
    if (const auto anchor = optional_string(value, "anchor_mode")) {
        data.anchor_mode = anchor_mode_from_string(*anchor);
    }
    const bool declared = data.stability_class || data.anchor_mode;
    auto result = create(std::move(data));
    if (result.id() != stored_id || result.content_digest() != stored_content_digest)
        throw std::invalid_argument("Stored ContextArtifact digest mismatch");
    if (declared != (version == STORAGE_SCHEMA_VERSION)) {
        throw std::invalid_argument("Stored ContextArtifact version does not match its fields");
    }
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
StabilityClass ContextArtifact::stability_class() const noexcept {
    return impl_->data.stability_class.value_or(
        derive_stability_class(impl_->data.kind, impl_->data.producer_id));
}
const std::optional<StabilityClass>& ContextArtifact::declared_stability_class() const noexcept {
    return impl_->data.stability_class;
}
AnchorMode ContextArtifact::anchor_mode() const noexcept {
    return impl_->data.anchor_mode.value_or(AnchorMode::Replace);
}
const std::optional<AnchorMode>& ContextArtifact::declared_anchor_mode() const noexcept {
    return impl_->data.anchor_mode;
}
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
    if (data.ordered_artifact_ids) {
        require_id_sequence(*data.ordered_artifact_ids, "Context epoch ordered_artifact_ids");
        auto sorted = *data.ordered_artifact_ids;
        std::sort(sorted.begin(), sorted.end());
        if (sorted != data.artifact_ids) {
            throw std::invalid_argument(
                "Context epoch ordered_artifact_ids must order exactly its artifact_ids");
        }
    }
    require_known_enum(data.assembly_layout, "Context epoch assembly layout");
    if (data.assembly_layout == AssemblyLayout::StableFirst && !data.ordered_artifact_ids) {
        throw std::invalid_argument("Context epoch with the stable-first layout must record ordered_artifact_ids");
    }
    if (data.prefix_claim) require_known_enum(*data.prefix_claim, "Context epoch prefix claim");
    if (data.break_reason) require_known_enum(*data.break_reason, "Context epoch break reason");
    if (data.prefix_claim == ContextPrefixClaim::Extends) {
        if (!data.predecessor_id) {
            throw ContextStabilityError(ContextStabilityRefusal::PredecessorUnavailable,
                                        "Context epoch cannot extend a predecessor it does not have");
        }
        if (!data.ordered_artifact_ids) {
            throw std::invalid_argument("Context epoch that extends its predecessor must record ordered_artifact_ids");
        }
    }
    if ((data.prefix_claim == ContextPrefixClaim::Breaks) != data.break_reason.has_value()) {
        throw std::invalid_argument("Context epoch break_reason belongs to a claim that breaks, and only to it");
    }
    // An epoch that sets none of the version 2 fields is a version 1 epoch, byte for byte.
    const bool ordered = data.ordered_artifact_ids || data.prefix_claim || data.break_reason ||
                         data.assembly_layout != AssemblyLayout::Legacy;
    json body{{"format", "neograph-context-epoch"},
              {"storage_schema_version", ordered ? STORAGE_SCHEMA_VERSION : LEGACY_STORAGE_SCHEMA_VERSION},
              {"run_id", data.run_id},
              {"sequence", data.sequence},
              {"feed_id", data.feed_id},
              {"raw_from_sequence", data.raw_from_sequence},
              {"raw_through_sequence", data.raw_through_sequence},
              {"raw_window_digest", data.raw_window_digest},
              {"artifact_ids", data.artifact_ids},
              {"guarantee_profile", std::string(to_string(data.guarantee_profile))}};
    if (data.predecessor_id) body["predecessor_id"] = *data.predecessor_id;
    if (data.ordered_artifact_ids) body["ordered_artifact_ids"] = *data.ordered_artifact_ids;
    if (data.prefix_claim) body["prefix_claim"] = std::string(to_string(*data.prefix_claim));
    if (data.break_reason) body["break_reason"] = std::string(to_string(*data.break_reason));
    if (data.assembly_layout != AssemblyLayout::Legacy) {
        body["assembly_layout"] = std::string(to_string(data.assembly_layout));
    }
    auto impl = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(
        seal(std::move(body), ordered ? "context-epoch/v2" : "context-epoch/v1", impl->id));
    return ContextEpoch(std::move(impl));
}

ContextEpoch ContextEpoch::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ContextEpoch");
    detail::reject_unknown_fields(
        value, "Stored ContextEpoch",
        {"format", "storage_schema_version", "id", "run_id", "sequence", "predecessor_id",
         "feed_id", "raw_from_sequence", "raw_through_sequence", "raw_window_digest",
         "artifact_ids", "guarantee_profile", "ordered_artifact_ids", "prefix_claim",
         "break_reason", "assembly_layout"});
    const auto version = require_format_version(
        value, "neograph-context-epoch", LEGACY_STORAGE_SCHEMA_VERSION, STORAGE_SCHEMA_VERSION,
        "Stored ContextEpoch");
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
    if (value.contains("ordered_artifact_ids")) {
        data.ordered_artifact_ids = required_string_array(value, "ordered_artifact_ids");
    }
    if (const auto claim = optional_string(value, "prefix_claim")) {
        data.prefix_claim = context_prefix_claim_from_string(*claim);
    }
    if (const auto reason = optional_string(value, "break_reason")) {
        data.break_reason = context_break_reason_from_string(*reason);
    }
    if (const auto layout = optional_string(value, "assembly_layout")) {
        data.assembly_layout = assembly_layout_from_string(*layout);
        if (data.assembly_layout == AssemblyLayout::Legacy) {
            throw std::invalid_argument("Stored ContextEpoch records its assembly layout only when it is not legacy");
        }
    }
    const bool ordered = data.ordered_artifact_ids || data.prefix_claim || data.break_reason ||
                         data.assembly_layout != AssemblyLayout::Legacy;
    auto result = create(std::move(data));
    if (result.id() != stored_id) throw std::invalid_argument("Stored ContextEpoch id mismatch");
    if (ordered != (version == STORAGE_SCHEMA_VERSION)) {
        throw std::invalid_argument("Stored ContextEpoch version does not match its fields");
    }
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
const std::optional<std::vector<std::string>>& ContextEpoch::ordered_artifact_ids() const noexcept { return impl_->data.ordered_artifact_ids; }
const std::optional<ContextPrefixClaim>& ContextEpoch::prefix_claim() const noexcept { return impl_->data.prefix_claim; }
const std::optional<ContextBreakReason>& ContextEpoch::break_reason() const noexcept { return impl_->data.break_reason; }
AssemblyLayout ContextEpoch::assembly_layout() const noexcept { return impl_->data.assembly_layout; }
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
        throw std::invalid_argument(
            "Mandatory input tokens (" + std::to_string(data.mandatory_input_tokens) +
            ") exceed the estimated input tokens (" +
            std::to_string(data.estimated_input_tokens) + ") they are part of");
    if (data.assembled_artifact_ids.has_value() != data.artifact_classes.has_value() ||
        data.assembled_artifact_ids.has_value() != data.class_sources.has_value() ||
        data.assembled_artifact_ids.has_value() != data.class_rule_version.has_value()) {
        throw std::invalid_argument(
            "Context assembly assembled_artifact_ids, artifact_classes, class_sources and class_rule_version "
            "are recorded together");
    }
    if (data.assembled_artifact_ids) {
        require_id_sequence(*data.assembled_artifact_ids, "Context assembly assembled_artifact_ids");
        auto sorted = *data.assembled_artifact_ids;
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::string> classified;
        classified.reserve(data.artifact_classes->size());
        for (const auto& [id, stability] : *data.artifact_classes) {
            require_known_enum(stability, "Context assembly artifact class");
            classified.push_back(id);
        }
        require_id_sequence(classified, "Context assembly artifact_classes");
        std::vector<std::string> sourced;
        sourced.reserve(data.class_sources->size());
        for (const auto& [id, source] : *data.class_sources) {
            require_known_enum(source, "Context assembly class source");
            sourced.push_back(id);
        }
        require_id_sequence(sourced, "Context assembly class_sources");
        if (sorted != data.artifact_ids || classified != data.artifact_ids || sourced != data.artifact_ids) {
            throw std::invalid_argument(
                "Context assembly order, classes and class sources must cover exactly its artifact_ids");
        }
    }
    if (data.encoder_family) detail::validate_token(*data.encoder_family, "Context assembly encoder_family");
    if (data.prefix_digest) require_sha256(*data.prefix_digest, "Context assembly prefix_digest");
    if (data.prefix_digest && !data.prefix_boundary) {
        throw std::invalid_argument("Context assembly prefix_digest needs its prefix_boundary");
    }
    if (data.stable_boundary && data.prefix_boundary && *data.prefix_boundary < *data.stable_boundary) {
        throw std::invalid_argument("Context assembly stable_boundary lies behind its prefix_boundary");
    }
    if (data.tools_digest) require_sha256(*data.tools_digest, "Context assembly tools_digest");
    if (data.break_reason) require_known_enum(*data.break_reason, "Context assembly break reason");
    // A receipt that sets none of the version 2 fields is a version 1 receipt, byte for byte.
    const bool versioned = prefix_aware(data);
    json body{{"format", "neograph-context-assembly-receipt"},
              {"storage_schema_version", versioned ? STORAGE_SCHEMA_VERSION : LEGACY_STORAGE_SCHEMA_VERSION},
              {"context_epoch_id", data.context_epoch_id},
              {"normalized_request_digest", data.normalized_request_digest},
              {"message_window_digest", data.message_window_digest},
              {"artifact_ids", data.artifact_ids},
              {"required_skill_artifact_ids", data.required_skill_artifact_ids},
              {"raw_from_sequence", data.raw_from_sequence},
              {"raw_through_sequence", data.raw_through_sequence},
              {"estimated_input_tokens", data.estimated_input_tokens},
              {"mandatory_input_tokens", data.mandatory_input_tokens}};
    if (data.assembled_artifact_ids) {
        body["assembled_artifact_ids"] = *data.assembled_artifact_ids;
        json classes = json::object();
        for (const auto& [id, stability] : *data.artifact_classes) classes[id] = std::string(to_string(stability));
        body["artifact_classes"] = std::move(classes);
        json sources = json::object();
        for (const auto& [id, source] : *data.class_sources) sources[id] = std::string(to_string(source));
        body["class_sources"] = std::move(sources);
        body["class_rule_version"] = *data.class_rule_version;
    }
    if (data.encoder_family) body["encoder_family"] = *data.encoder_family;
    if (data.stable_boundary) body["stable_boundary"] = boundary_body(*data.stable_boundary);
    if (data.prefix_boundary) body["prefix_boundary"] = boundary_body(*data.prefix_boundary);
    if (data.prefix_digest) body["prefix_digest"] = *data.prefix_digest;
    if (data.tools_digest) body["tools_digest"] = *data.tools_digest;
    if (data.break_reason) body["break_reason"] = std::string(to_string(*data.break_reason));
    auto impl = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(seal(
        std::move(body), versioned ? "context-assembly-receipt/v2" : "context-assembly-receipt/v1",
        impl->id));
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
         "estimated_input_tokens", "mandatory_input_tokens", "assembled_artifact_ids",
         "artifact_classes", "class_sources", "class_rule_version", "encoder_family",
         "stable_boundary", "prefix_boundary",
         "prefix_digest", "tools_digest", "break_reason"});
    const auto version = require_format_version(
        value, "neograph-context-assembly-receipt", LEGACY_STORAGE_SCHEMA_VERSION,
        STORAGE_SCHEMA_VERSION, "Stored ContextAssemblyReceipt");
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
    if (value.contains("assembled_artifact_ids")) {
        data.assembled_artifact_ids = required_string_array(value, "assembled_artifact_ids");
    }
    if (value.contains("artifact_classes")) {
        const auto& classes = value.at("artifact_classes");
        if (!classes.is_object()) {
            throw std::invalid_argument("Stored runtime value field 'artifact_classes' must be an object");
        }
        std::map<std::string, StabilityClass> parsed;
        for (const auto& [id, name] : classes.items()) {
            if (!name.is_string()) {
                throw std::invalid_argument("Stored runtime value field 'artifact_classes' must name classes");
            }
            parsed.emplace(id, stability_class_from_string(name.get<std::string>()));
        }
        data.artifact_classes = std::move(parsed);
    }
    if (value.contains("class_sources")) {
        const auto& sources = value.at("class_sources");
        if (!sources.is_object()) {
            throw std::invalid_argument("Stored runtime value field 'class_sources' must be an object");
        }
        std::map<std::string, ClassSource> parsed;
        for (const auto& [id, name] : sources.items()) {
            if (!name.is_string()) {
                throw std::invalid_argument("Stored runtime value field 'class_sources' must name sources");
            }
            parsed.emplace(id, class_source_from_string(name.get<std::string>()));
        }
        data.class_sources = std::move(parsed);
    }
    if (value.contains("class_rule_version")) {
        const auto version = required_u64(value, "class_rule_version");
        if (version > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Stored runtime value field 'class_rule_version' is out of range");
        }
        data.class_rule_version = static_cast<std::uint32_t>(version);
    }
    data.encoder_family = optional_string(value, "encoder_family");
    data.stable_boundary = optional_boundary(value, "stable_boundary");
    data.prefix_boundary = optional_boundary(value, "prefix_boundary");
    data.prefix_digest = optional_string(value, "prefix_digest");
    data.tools_digest = optional_string(value, "tools_digest");
    if (const auto reason = optional_string(value, "break_reason")) {
        data.break_reason = context_break_reason_from_string(*reason);
    }
    const bool versioned = prefix_aware(data);
    auto result = create_structural(std::move(data));
    if (result.id() != stored_id)
        throw std::invalid_argument("Stored ContextAssemblyReceipt id mismatch");
    if (versioned != (version == STORAGE_SCHEMA_VERSION)) {
        throw std::invalid_argument("Stored ContextAssemblyReceipt version does not match its fields");
    }
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
const std::optional<std::vector<std::string>>& ContextAssemblyReceipt::assembled_artifact_ids() const noexcept { return impl_->data.assembled_artifact_ids; }
const std::optional<std::map<std::string, StabilityClass>>& ContextAssemblyReceipt::artifact_classes() const noexcept { return impl_->data.artifact_classes; }
const std::optional<std::map<std::string, ClassSource>>& ContextAssemblyReceipt::class_sources() const noexcept { return impl_->data.class_sources; }
const std::optional<std::uint32_t>& ContextAssemblyReceipt::class_rule_version() const noexcept { return impl_->data.class_rule_version; }
const std::optional<std::string>& ContextAssemblyReceipt::encoder_family() const noexcept { return impl_->data.encoder_family; }
const std::optional<ContextPrefixBoundary>& ContextAssemblyReceipt::stable_boundary() const noexcept { return impl_->data.stable_boundary; }
const std::optional<ContextPrefixBoundary>& ContextAssemblyReceipt::prefix_boundary() const noexcept { return impl_->data.prefix_boundary; }
const std::optional<std::string>& ContextAssemblyReceipt::prefix_digest() const noexcept { return impl_->data.prefix_digest; }
const std::optional<std::string>& ContextAssemblyReceipt::tools_digest() const noexcept { return impl_->data.tools_digest; }
const std::optional<ContextBreakReason>& ContextAssemblyReceipt::break_reason() const noexcept { return impl_->data.break_reason; }
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
    if (receipt.artifact_classes()) {
        // A class an artifact declares is part of the artifact and has to match strictly. A class the receipt
        // derived is checked by deriving it again only under the rule version in force: the rule can change
        // after the receipt was stored, and a receipt written under an older one is read as it was written.
        const bool current_rule = receipt.class_rule_version() == STABILITY_DERIVATION_RULE_VERSION;
        for (const auto& artifact : artifacts) {
            const auto recorded = receipt.artifact_classes()->find(artifact.id());
            const auto source = receipt.class_sources()->find(artifact.id());
            if (recorded == receipt.artifact_classes()->end() || source == receipt.class_sources()->end()) {
                throw std::invalid_argument(
                    "Context assembly receipt artifact classes do not match the artifacts");
            }
            const auto& declared = artifact.declared_stability_class();
            if ((source->second == ClassSource::Declared) != declared.has_value()) {
                throw std::invalid_argument(
                    "Context assembly receipt class source does not match what the artifact declares");
            }
            if (declared ? recorded->second != *declared
                         : current_rule && recorded->second != artifact.stability_class()) {
                throw std::invalid_argument(
                    "Context assembly receipt artifact classes do not match the artifacts");
            }
        }
    }
    const bool versioned = receipt.assembled_artifact_ids() || receipt.encoder_family() ||
        receipt.stable_boundary() || receipt.prefix_boundary() || receipt.prefix_digest() ||
        receipt.tools_digest() || receipt.break_reason();
    if (versioned && receipt.break_reason() != epoch.break_reason()) {
        throw std::invalid_argument("Context assembly receipt break reason does not match its epoch");
    }
}

namespace {

// The artifacts an epoch selects, checked against the ids it records.
std::map<std::string, const ContextArtifact*> selected_artifacts(
    const ContextEpoch& epoch, const std::vector<ContextArtifact>& artifacts) {
    std::map<std::string, const ContextArtifact*> by_id;
    for (const auto& artifact : artifacts) by_id.emplace(artifact.id(), &artifact);
    if (by_id.size() != artifacts.size() || by_id.size() != epoch.artifact_ids().size() ||
        !std::all_of(epoch.artifact_ids().begin(), epoch.artifact_ids().end(),
                     [&](const std::string& id) { return by_id.contains(id); })) {
        throw ContextStabilityError(ContextStabilityRefusal::ArtifactsMismatch,
                                    "Context epoch claim needs exactly the artifacts the epoch selects");
    }
    return by_id;
}

// Stable and Slow artifacts in the order the epoch fixes.
std::vector<std::string> stable_prefix(
    const ContextEpoch& epoch, const std::map<std::string, const ContextArtifact*>& by_id) {
    std::vector<std::string> result;
    for (const auto& id : *epoch.ordered_artifact_ids()) {
        if (by_id.at(id)->stability_class() != StabilityClass::Volatile) result.push_back(id);
    }
    return result;
}

}  // namespace

std::vector<std::string> order_context_artifacts(const std::vector<ContextArtifactOrderEntry>& entries) {
    struct Keyed {
        StabilityClass tier;
        std::int32_t   priority;
        const std::string* producer;
        const std::string* order_key;
        const std::string* id;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(entries.size());
    std::set<std::string> seen;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.artifact.id()).second) {
            throw ContextStabilityError(ContextStabilityRefusal::AmbiguousOrder,
                                        "Context artifact order lists an artifact twice");
        }
        keyed.push_back({entry.artifact.stability_class(), entry.artifact.priority(),
                         &entry.artifact.producer_id(), &entry.order_key, &entry.artifact.id()});
    }
    // Higher priority first. The identity is never a key: it is a hash of the content.
    const auto before = [](const Keyed& a, const Keyed& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.priority != b.priority) return a.priority > b.priority;
        if (*a.producer != *b.producer) return *a.producer < *b.producer;
        return *a.order_key < *b.order_key;
    };
    std::stable_sort(keyed.begin(), keyed.end(), before);
    for (std::size_t index = 1; index < keyed.size(); ++index) {
        if (!before(keyed[index - 1], keyed[index])) {
            throw ContextStabilityError(
                ContextStabilityRefusal::AmbiguousOrder,
                "Two context artifacts have the same stability class, priority, producer and order key");
        }
    }
    std::vector<std::string> result;
    result.reserve(keyed.size());
    for (const auto& item : keyed) result.push_back(*item.id);
    return result;
}

void validate_context_layout(const ContextEpoch& epoch, const std::vector<ContextArtifact>& artifacts) {
    if (epoch.assembly_layout() != AssemblyLayout::StableFirst) return;
    const auto by_id = selected_artifacts(epoch, artifacts);
    StabilityClass tier = StabilityClass::Stable;
    for (const auto& id : *epoch.ordered_artifact_ids()) {
        const auto& artifact = *by_id.at(id);
        const auto cls = artifact.stability_class();
        if (cls == StabilityClass::Volatile) {
            if (artifact.placement() != ContextPlacement::AfterHistory) {
                throw ContextStabilityError(
                    ContextStabilityRefusal::VolatileOutsideTail,
                    "A Volatile context artifact must be placed at AfterHistory in the stable-first layout, not at " +
                        std::string(to_string(artifact.placement())));
            }
        } else if (artifact.placement() != ContextPlacement::BeforeHistory) {
            throw ContextStabilityError(
                ContextStabilityRefusal::StableOutsideHistoryHead,
                "A " + std::string(to_string(cls)) +
                    " context artifact must be placed at BeforeHistory in the stable-first layout, not at " +
                    std::string(to_string(artifact.placement())));
        }
        if (cls < tier) {
            throw ContextStabilityError(
                ContextStabilityRefusal::TierOrderViolated,
                "The recorded artifact order puts a " + std::string(to_string(cls)) +
                    " artifact behind a " + std::string(to_string(tier)) + " one");
        }
        tier = cls;
    }
}

void verify_context_epoch_claim(
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts,
    const ContextEpoch* predecessor,
    const std::vector<ContextArtifact>& predecessor_artifacts) {
    // The layout decides every byte of the request, so a change of it is a break the epoch has to name.
    if (predecessor && epoch.predecessor_id() && predecessor->id() == *epoch.predecessor_id() &&
        predecessor->assembly_layout() != epoch.assembly_layout() &&
        !(epoch.prefix_claim() == ContextPrefixClaim::Breaks &&
          epoch.break_reason() == ContextBreakReason::LayoutChange)) {
        throw ContextStabilityError(
            ContextStabilityRefusal::LayoutChangeUndeclared,
            "Context epoch changes the assembly layout of its predecessor without declaring Breaks(LayoutChange)");
    }
    if (epoch.prefix_claim() != ContextPrefixClaim::Extends) return;
    if (!predecessor || !epoch.predecessor_id() || predecessor->id() != *epoch.predecessor_id()) {
        throw ContextStabilityError(
            ContextStabilityRefusal::PredecessorUnavailable,
            "Context epoch extends a predecessor that is not available");
    }
    if (!predecessor->ordered_artifact_ids() || !epoch.ordered_artifact_ids()) {
        throw ContextStabilityError(
            ContextStabilityRefusal::PredecessorOrderUnknown,
            "Context epoch extends a predecessor that records no artifact order");
    }
    const auto current = selected_artifacts(epoch, artifacts);
    const auto previous = selected_artifacts(*predecessor, predecessor_artifacts);
    // Under the stable-first layout the recorded order is the order of the request, so a claim to extend is
    // only as good as a layout the assembler accepts: a Volatile artifact in front of a Stable one, for
    // instance, passes the comparison below and is still no extension.
    if (epoch.assembly_layout() == AssemblyLayout::StableFirst) validate_context_layout(epoch, artifacts);
    if (predecessor->run_id() != epoch.run_id() || predecessor->feed_id() != epoch.feed_id() ||
        predecessor->raw_from_sequence() != epoch.raw_from_sequence()) {
        throw ContextStabilityError(
            ContextStabilityRefusal::RawWindowMoved,
            "Context epoch extends a predecessor whose RAW window starts elsewhere");
    }
    if (epoch.raw_through_sequence() <= predecessor->raw_through_sequence()) {
        throw ContextStabilityError(
            ContextStabilityRefusal::RawWindowNotGrown,
            "Context epoch extends a predecessor without growing its RAW window");
    }
    if (stable_prefix(epoch, current) != stable_prefix(*predecessor, previous)) {
        throw ContextStabilityError(
            ContextStabilityRefusal::StablePrefixChanged,
            "Context epoch extends a predecessor but changes its Stable or Slow artifacts");
    }
    for (const auto& [id, artifact] : previous) {
        if (artifact->anchor_mode() == AnchorMode::KeepInPlace && !current.contains(id)) {
            throw ContextStabilityError(
                ContextStabilityRefusal::AnchorDropped,
                "Context epoch extends a predecessor but drops one of its anchored artifacts");
        }
    }
    for (const auto& [id, artifact] : current) {
        if (artifact->anchor_mode() != AnchorMode::KeepInPlace) continue;
        if (artifact->source_feed_id() != epoch.feed_id() ||
            artifact->covers_through_sequence() < epoch.raw_from_sequence() ||
            artifact->covers_through_sequence() > epoch.raw_through_sequence()) {
            throw ContextStabilityError(
                ContextStabilityRefusal::AnchorOutOfRange,
                "Context epoch selects an anchored artifact that covers records outside its RAW window");
        }
    }
}

}  // namespace neograph
