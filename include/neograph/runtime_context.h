#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/types.h>
#include <core/native_archive.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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
    /** Explicitly untrusted RAW or derived data delivered as a user message. */
    UntrustedSupplemental,
};

enum class ContextPlacement : std::uint8_t {
    BeforeLatestUser,
    AfterLatestUser,
    /** Append after the complete chronological RAW message window. */
    AfterHistory,
    /** Insert supplemental context before the retained chronological history. */
    BeforeHistory,
};

enum class RuntimeGuaranteeProfile : std::uint8_t {
    Legacy,
    Recorded,
    Strict,
};

/**
 * How long an artifact's bytes stay the same inside a run, which decides whether
 * a provider's prompt cache can keep the request start in front of it.
 */
enum class StabilityClass : std::uint8_t {
    /** The same bytes for the life of the run. */
    Stable,
    /** Changes only at a compaction or another declared prefix break. */
    Slow,
    /** May change from one provider call to the next. */
    Volatile,
};

/** What happens to an artifact when its turn has passed. */
enum class AnchorMode : std::uint8_t {
    /** The artifact is replaced by the next one of its producer. The default. */
    Replace,
    /** The artifact stays where it was placed, after the RAW record it covers. */
    KeepInPlace,
};

/** Why an epoch does not extend the request prefix of its predecessor. */
enum class ContextBreakReason : std::uint8_t {
    Compaction,
    StableChange,
    LayoutChange,
    AnchorDrop,
    OrderChange,
    Explicit,
};

/** What an epoch declares about the request prefix of its predecessor. */
enum class ContextPrefixClaim : std::uint8_t {
    /** Same start, only grown at the end; checked by verify_context_epoch_claim. */
    Extends,
    /** The start changes, for the reason the epoch names. */
    Breaks,
};

NEOGRAPH_API std::string_view to_string(RuntimeTrustClass value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextArtifactKind value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextPlacement value) noexcept;
NEOGRAPH_API std::string_view to_string(RuntimeGuaranteeProfile value) noexcept;
NEOGRAPH_API std::string_view to_string(StabilityClass value) noexcept;
NEOGRAPH_API std::string_view to_string(AnchorMode value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextBreakReason value) noexcept;
NEOGRAPH_API std::string_view to_string(ContextPrefixClaim value) noexcept;
NEOGRAPH_API RuntimeTrustClass runtime_trust_class_from_string(std::string_view value);
NEOGRAPH_API ContextArtifactKind context_artifact_kind_from_string(std::string_view value);
NEOGRAPH_API ContextPlacement context_placement_from_string(std::string_view value);
NEOGRAPH_API RuntimeGuaranteeProfile runtime_guarantee_profile_from_string(
    std::string_view value);
NEOGRAPH_API StabilityClass stability_class_from_string(std::string_view value);
NEOGRAPH_API AnchorMode anchor_mode_from_string(std::string_view value);
NEOGRAPH_API ContextBreakReason context_break_reason_from_string(std::string_view value);
NEOGRAPH_API ContextPrefixClaim context_prefix_claim_from_string(std::string_view value);

/** Producer id of a DerivedContext artifact that a compaction transform writes. */
inline constexpr std::string_view COMPACTION_PRODUCER_ID = "neograph-compaction.v1";

/** True for the kinds that render as System messages: RequiredSkill and HardConstraint. */
NEOGRAPH_API bool context_artifact_kind_renders_as_system(ContextArtifactKind kind) noexcept;

/**
 * The class of an artifact that does not declare one, from fields every stored
 * artifact has. RequiredSkill and HardConstraint are Stable; DerivedContext whose
 * producer is COMPACTION_PRODUCER_ID is Slow; everything else is Volatile, so a
 * kind or producer this rule does not know fails toward the class that is never
 * assumed to stay the same.
 */
NEOGRAPH_API StabilityClass derive_stability_class(
    ContextArtifactKind kind, std::string_view producer_id) noexcept;

/** Why a stability rule refused a value. */
enum class ContextStabilityRefusal : std::uint8_t {
    /** A Volatile artifact of a kind that renders as a System message. */
    VolatileSystemArtifact,
    /** An Extends claim whose predecessor is missing or is not the epoch's predecessor. */
    PredecessorUnavailable,
    /** The predecessor records no artifact order, so it cannot be compared. */
    PredecessorOrderUnknown,
    /** The artifacts given do not match the artifact ids the epoch selects. */
    ArtifactsMismatch,
    /** raw_from_sequence, the run or the feed differs from the predecessor's. */
    RawWindowMoved,
    /** raw_through_sequence is not larger than the predecessor's. */
    RawWindowNotGrown,
    /** The Stable and Slow artifacts, or their order, differ from the predecessor's. */
    StablePrefixChanged,
    /** A KeepInPlace artifact of the predecessor is not in the epoch. */
    AnchorDropped,
    /** A KeepInPlace artifact covers a range outside the epoch's RAW window. */
    AnchorOutOfRange,
};

NEOGRAPH_API std::string_view to_string(ContextStabilityRefusal value) noexcept;

/** A stability rule or an epoch claim was refused. Also a std::invalid_argument. */
class NEOGRAPH_API ContextStabilityError final : public std::invalid_argument {
public:
    ContextStabilityError(ContextStabilityRefusal code, const std::string& message);
    ContextStabilityRefusal code() const noexcept;

private:
    ContextStabilityRefusal code_;
};

struct RuntimeHistoryRecordData {
    std::string                 feed_id;
    std::uint64_t               sequence = 0;
    std::string                 message_id;
    RuntimeTrustClass           trust = RuntimeTrustClass::UntrustedInput;
    sp::Message                 message;
    std::optional<json>         source_payload;
    std::optional<std::string>  source_media_type;
    std::optional<std::string>  predecessor_id;
};

/** One immutable, content-addressed line in the authoritative RAW history feed. */
class NEOGRAPH_API RuntimeHistoryRecord final {
public:
    // 3 since SchemaProvider interface 7 (provider-message/v3). A version 2 record is read
    // with its bytes and id as stored, so a feed's predecessor chain stays intact.
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 3;
    static constexpr std::uint32_t LEGACY_STORAGE_SCHEMA_VERSION = 2;

    static RuntimeHistoryRecord create(RuntimeHistoryRecordData data);
    static RuntimeHistoryRecord parse(
        std::string_view stored_bytes,
        const std::shared_ptr<sp::NativeArchive>& archive = {},
        std::string_view owner_id = {});

    const std::string& feed_id() const noexcept;
    std::uint64_t sequence() const noexcept;
    const std::string& message_id() const noexcept;
    RuntimeTrustClass trust() const noexcept;
    const sp::Message& message() const noexcept;
    std::optional<json> source_payload() const;
    const std::optional<std::string>& source_media_type() const noexcept;
    const std::optional<std::string>& predecessor_id() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;
    /** Persist authentic seals under the host's owner-scoped archive. */
    std::string serialize_canonical(
        const std::shared_ptr<sp::NativeArchive>& archive,
        std::string_view owner_id) const;

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
    /**
     * Declared class. Part of the identity only when set; an artifact that
     * leaves it unset derives its class (derive_stability_class). A Volatile
     * artifact of a kind that renders as System is refused by create().
     */
    std::optional<StabilityClass> stability_class;
    /** Part of the identity only when set; an artifact that leaves it unset replaces. */
    std::optional<AnchorMode>     anchor_mode;
};

/** Immutable context material. It is evidence and never grants runtime authority. */
class NEOGRAPH_API ContextArtifact final {
public:
    // 2 only for an artifact that sets stability_class or anchor_mode; every other
    // artifact keeps version 1 and the identity it always had.
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;
    static constexpr std::uint32_t LEGACY_STORAGE_SCHEMA_VERSION = 1;

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
    /** The declared class, or the derived one when none is declared. */
    StabilityClass stability_class() const noexcept;
    const std::optional<StabilityClass>& declared_stability_class() const noexcept;
    /** The declared mode, or Replace when none is declared. */
    AnchorMode anchor_mode() const noexcept;
    const std::optional<AnchorMode>& declared_anchor_mode() const noexcept;
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
    /**
     * The artifacts in the order the epoch fixes, a permutation of artifact_ids.
     * Part of the identity only when set. artifact_ids is a set sorted by
     * identity; this is the order. An epoch without it keeps the order the
     * assembler derives from the artifacts. The assembler does not read it yet.
     */
    std::optional<std::vector<std::string>> ordered_artifact_ids;
    /**
     * What the epoch declares about the request prefix of its predecessor.
     * Extends needs ordered_artifact_ids and a predecessor, and is checked by
     * verify_context_epoch_claim when the epoch is activated. Breaks needs
     * break_reason, which no other claim may carry.
     */
    std::optional<ContextPrefixClaim> prefix_claim;
    std::optional<ContextBreakReason> break_reason;
};

/** Immutable selection of exact RAW and derived material for provider turns. */
class NEOGRAPH_API ContextEpoch final {
public:
    // 2 only for an epoch that sets ordered_artifact_ids, prefix_claim or
    // break_reason; every other epoch keeps version 1 and the identity it had.
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;
    static constexpr std::uint32_t LEGACY_STORAGE_SCHEMA_VERSION = 1;

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
    const std::optional<std::vector<std::string>>& ordered_artifact_ids() const noexcept;
    const std::optional<ContextPrefixClaim>& prefix_claim() const noexcept;
    const std::optional<ContextBreakReason>& break_reason() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContextEpoch(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Where in an encoded request body a prefix ends: the message, and the part of it. */
struct ContextPrefixBoundary {
    std::uint64_t message_index = 0;
    std::uint64_t part_index = 0;
    bool operator==(const ContextPrefixBoundary&) const = default;
};

struct ContextAssemblyReceiptData {
    std::string              context_epoch_id;
    std::string              normalized_request_digest;
    std::string              message_window_digest;
    std::vector<std::string> artifact_ids;
    std::vector<std::string> required_skill_artifact_ids;
    std::uint64_t            raw_from_sequence = 0;
    std::uint64_t            raw_through_sequence = 0;
    /**
     * Bytes of the encoded request body, three per token: a heuristic over the
     * whole encoded request, not a proven upper bound of a provider's count.
     */
    std::uint64_t            estimated_input_tokens = 0;
    /** Bytes of required artifact text on the same basis; never above the estimate. */
    std::uint64_t            mandatory_input_tokens = 0;

    // Version 2. Each field is part of the identity only when set, and a receipt
    // that sets none is a version 1 receipt. A version 1 receipt knows none of
    // them: its prefix is unknown, not empty.
    /** artifact_ids in the order the assembler placed them. Needs artifact_classes. */
    std::optional<std::vector<std::string>>        assembled_artifact_ids;
    /** The class of every artifact in artifact_ids. Needs assembled_artifact_ids. */
    std::optional<std::map<std::string, StabilityClass>> artifact_classes;
    /** The encoder the request body was written by, such as the provider family. */
    std::optional<std::string>                     encoder_family;
    /** End of the Stable part of the encoded request. Null until the assembler marks it. */
    std::optional<ContextPrefixBoundary>           stable_boundary;
    /** End of the append-only part of the encoded request. Null until the assembler marks it. */
    std::optional<ContextPrefixBoundary>           prefix_boundary;
    /** Digest of the encoded request up to prefix_boundary. Null until the assembler computes it. */
    std::optional<std::string>                     prefix_digest;
    /** Digest of the tool definitions in the encoded request body. */
    std::optional<std::string>                     tools_digest;
    /** The break reason of the epoch; null when the epoch extends or claims nothing. */
    std::optional<ContextBreakReason>              break_reason;
};

/** Evidence that one normalized provider request was assembled from one exact epoch. */
class NEOGRAPH_API ContextAssemblyReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;
    static constexpr std::uint32_t LEGACY_STORAGE_SCHEMA_VERSION = 1;

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
    const std::optional<std::vector<std::string>>& assembled_artifact_ids() const noexcept;
    const std::optional<std::map<std::string, StabilityClass>>& artifact_classes() const noexcept;
    const std::optional<std::string>& encoder_family() const noexcept;
    const std::optional<ContextPrefixBoundary>& stable_boundary() const noexcept;
    const std::optional<ContextPrefixBoundary>& prefix_boundary() const noexcept;
    const std::optional<std::string>& prefix_digest() const noexcept;
    const std::optional<std::string>& tools_digest() const noexcept;
    const std::optional<ContextBreakReason>& break_reason() const noexcept;
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

/**
 * Check what an epoch declares about the request prefix of its predecessor.
 *
 * An epoch without a claim, or one that Breaks, is accepted as it stands: a break
 * only has to name its reason, and an epoch that breaks without needing to costs
 * a cache entry, not correctness. An Extends claim is structural and must hold:
 *   - the predecessor is the epoch's predecessor (same id, run and feed) and
 *     records its artifact order, which an epoch of version 1 does not;
 *   - raw_from_sequence is the same and raw_through_sequence is larger;
 *   - the Stable and Slow artifacts are the same, in the same order;
 *   - every KeepInPlace artifact of the predecessor is still selected, and each
 *     one the epoch selects covers a range inside its RAW window.
 * `artifacts` are the artifacts the epoch selects and `predecessor_artifacts` those of
 * the predecessor, each matching its epoch's artifact_ids. Pass a null predecessor
 * when none can be loaded: an Extends claim is then refused.
 * Throws ContextStabilityError.
 */
NEOGRAPH_API void verify_context_epoch_claim(
    const ContextEpoch& epoch,
    const std::vector<ContextArtifact>& artifacts,
    const ContextEpoch* predecessor,
    const std::vector<ContextArtifact>& predecessor_artifacts);

}  // namespace neograph
