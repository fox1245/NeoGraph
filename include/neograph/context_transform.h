/** @file context_transform.h @brief Verified transformation of admitted context artifacts. */
#pragma once

#include <neograph/api.h>
#include <neograph/runtime_context.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

struct ContextTransformReceiptData {
    std::string source_context_epoch_id;
    std::string transformer_identity;
    std::vector<std::string> input_artifact_ids;
    std::vector<std::string> output_artifact_ids;
    std::vector<std::string> preserved_required_artifact_ids;
};

/**
 * Evidence that a transform preserved every required input artifact exactly.
 *
 * v1 deliberately permits arbitrary derived evidence but does not accept a
 * semantic paraphrase as proof for required content. Required Skill and hard
 * constraint identities must appear byte-identically in the output set.
 */
class NEOGRAPH_API ContextTransformReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ContextTransformReceipt create(
        ContextTransformReceiptData data,
        const ContextEpoch& source_epoch,
        const std::vector<ContextArtifact>& input_artifacts,
        const std::vector<ContextArtifact>& output_artifacts);
    static ContextTransformReceipt parse(
        std::string_view stored_bytes,
        const ContextEpoch& source_epoch,
        const std::vector<ContextArtifact>& input_artifacts,
        const std::vector<ContextArtifact>& output_artifacts);

    const std::string& source_context_epoch_id() const noexcept;
    const std::string& transformer_identity() const noexcept;
    const std::vector<std::string>& input_artifact_ids() const noexcept;
    const std::vector<std::string>& output_artifact_ids() const noexcept;
    const std::vector<std::string>& preserved_required_artifact_ids() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContextTransformReceipt(std::shared_ptr<const Impl> impl);
    static ContextTransformReceipt create_structural(ContextTransformReceiptData data);
    std::shared_ptr<const Impl> impl_;
};

NEOGRAPH_API void validate_context_transform_receipt(
    const ContextTransformReceipt& receipt,
    const ContextEpoch& source_epoch,
    const std::vector<ContextArtifact>& input_artifacts,
    const std::vector<ContextArtifact>& output_artifacts);

}  // namespace neograph

