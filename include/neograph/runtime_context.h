#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

enum class RuntimeTrustClass : std::uint8_t {
    UntrustedInput,
    ModelOutput,
    ToolOutput,
    Developer,
    HostPolicy,
};

enum class ContextArtifactKind : std::uint8_t {
    RawHistory,
    DerivedContext,
    RequiredSkill,
    HookOutput,
    HardConstraint,
};

enum class ContextPlacement : std::uint8_t {
    BeforeLatestUser,
    AfterLatestUser,
};

enum class RuntimeGuaranteeProfile : std::uint8_t {
    Legacy,
    Recorded,
    Strict,
};

NEOGRAPH_API std::string_view to_string(RuntimeTrustClass value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextArtifactKind value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextPlacement value) noexcept;
NEOGRAPH_API std::string_view to_string(RuntimeGuaranteeProfile value) noexcept;
NEOGRAPH_API RuntimeTrustClass runtime_trust_class_from_string(std::string_view value);
NEOGRAPH_API ContextArtifactKind context_artifact_kind_from_string(std::string_view value);
NEOGRAPH_API ContextPlacement context_placement_from_string(std::string_view value);
NEOGRAPH_API RuntimeGuaranteeProfile runtime_guarantee_profile_from_string(
    std::string_view value);

struct RuntimeHistoryRecordData {
    std::string                 feed_id;
    std::uint64_t               sequence = 0;
    std::string                 message_id;
    RuntimeTrustClass           trust = RuntimeTrustClass::UntrustedInput;
    ChatMessage                 message;
    std::optional<json>         source_payload;
    std::optional<std::string>  source_media_type;
    std::optional<std::string>  predecessor_id;
};

/** One immutable, content-addressed line in the authoritative RAW history feed. */
class NEOGRAPH_API RuntimeHistoryRecord final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static RuntimeHistoryRecord create(RuntimeHistoryRecordData data);
    static RuntimeHistoryRecord parse(std::string_view stored_bytes);

    const std::string& feed_id() const noexcept;
    std::uint64_t sequence() const noexcept;
    const std::string& message_id() const noexcept;
    RuntimeTrustClass trust() const noexcept;
    const ChatMessage& message() const noexcept;
    std::optional<json> source_payload() const;
    const std::optional<std::string>& source_media_type() const noexcept;
    const std::optional<std::string>& predecessor_id() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit RuntimeHistoryRecord(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ContextArtifactData {
    ContextArtifactKind kind = ContextArtifactKind::DerivedContext;
    std::string          producer_id;
    std::string          source_digest;
    std::string          source_feed_id;
    std::uint64_t        covers_from_sequence = 0;
    std::uint64_t        covers_through_sequence = 0;
    std::string          media_type;
    ContextPlacement     placement = ContextPlacement::BeforeLatestUser;
    std::int32_t         priority = 0;
    bool                 required = false;
    json                 content;
};

/** Immutable context material. It is evidence and never grants runtime authority. */
class NEOGRAPH_API ContextArtifact final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ContextArtifact create(ContextArtifactData data);
    static ContextArtifact parse(std::string_view stored_bytes);

    ContextArtifactKind kind() const noexcept;
    const std::string& producer_id() const noexcept;
    const std::string& source_digest() const noexcept;
    const std::string& source_feed_id() const noexcept;
    std::uint64_t covers_from_sequence() const noexcept;
    std::uint64_t covers_through_sequence() const noexcept;
    const std::string& media_type() const noexcept;
    ContextPlacement placement() const noexcept;
    std::int32_t priority() const noexcept;
    bool required() const noexcept;
    json content() const;
    const std::string& content_digest() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContextArtifact(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ContextEpochData {
    std::string                run_id;
    std::uint64_t              sequence = 0;
    std::optional<std::string> predecessor_id;
    std::string                feed_id;
    std::uint64_t              raw_from_sequence = 0;
    std::uint64_t              raw_through_sequence = 0;
    std::string                raw_window_digest;
    std::vector<std::string>   artifact_ids;
    RuntimeGuaranteeProfile    guarantee_profile = RuntimeGuaranteeProfile::Recorded;
};

/** Immutable selection of exact RAW and derived material for provider turns. */
class NEOGRAPH_API ContextEpoch final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ContextEpoch create(ContextEpochData data);
    static ContextEpoch parse(std::string_view stored_bytes);

    const std::string& run_id() const noexcept;
    std::uint64_t sequence() const noexcept;
    const std::optional<std::string>& predecessor_id() const noexcept;
    const std::string& feed_id() const noexcept;
    std::uint64_t raw_from_sequence() const noexcept;
    std::uint64_t raw_through_sequence() const noexcept;
    const std::string& raw_window_digest() const noexcept;
    const std::vector<std::string>& artifact_ids() const noexcept;
    RuntimeGuaranteeProfile guarantee_profile() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContextEpoch(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ContextAssemblyReceiptData {
    std::string              context_epoch_id;
    std::string              normalized_request_digest;
    std::string              message_window_digest;
    std::vector<std::string> artifact_ids;
    std::vector<std::string> required_skill_artifact_ids;
    std::uint64_t            raw_from_sequence = 0;
    std::uint64_t            raw_through_sequence = 0;
    std::uint64_t            estimated_input_tokens = 0;
    std::uint64_t            mandatory_input_tokens = 0;
};

/** Evidence that one normalized provider request was assembled from one exact epoch. */
class NEOGRAPH_API ContextAssemblyReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ContextAssemblyReceipt create(
        ContextAssemblyReceiptData data,
        const ContextEpoch& epoch,
        const std::vector<ContextArtifact>& artifacts);
    static ContextAssemblyReceipt parse(
        std::string_view stored_bytes,
        const ContextEpoch& epoch,
        const std::vector<ContextArtifact>& artifacts);

    const std::string& context_epoch_id() const noexcept;
    const std::string& normalized_request_digest() const noexcept;
    const std::string& message_window_digest() const noexcept;
    const std::vector<std::string>& artifact_ids() const noexcept;
    const std::vector<std::string>& required_skill_artifact_ids() const noexcept;
    std::uint64_t raw_from_sequence() const noexcept;
    std::uint64_t raw_through_sequence() const noexcept;
    std::uint64_t estimated_input_tokens() const noexcept;
    std::uint64_t mandatory_input_tokens() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContextAssemblyReceipt(std::shared_ptr<const Impl> impl);
    static ContextAssemblyReceipt create_structural(ContextAssemblyReceiptData data);
    std::shared_ptr<const Impl> impl_;
};

/** Revalidate parsed receipt evidence against the exact epoch and artifacts. */
NEOGRAPH_API void validate_context_assembly_receipt(
    const ContextAssemblyReceipt& receipt,
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts);

}  // namespace neograph
