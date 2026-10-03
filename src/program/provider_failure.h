#pragma once

#include <neograph/program/result.h>
#include <neograph/provider_outcome_codec.h>
#include "canonical_json.h"

namespace neograph::program::detail {

inline json provider_failure_custody(
    std::string_view owner, std::string_view run, std::string_view version,
    std::string_view bundle, std::string_view operation, std::uint64_t attempt) {
    return json{{"format", "program-provider-failure/v1"}, {"owner_scope", owner},
                {"run_id", run}, {"program_version_id", version}, {"bundle_id", bundle},
                {"operation_id", operation}, {"attempt", attempt}};
}

inline bool has_provider_outcome(const ProgramFailure& failure) {
    return failure.witness.is_object() && failure.witness.contains("provider_outcome");
}

inline void persist_provider_failure(
    ProgramFailure& failure, const json& custody,
    const std::shared_ptr<sp::NativeArchive>& archive) {
    if (!failure.provider_outcome) return;
    if (!failure.witness.is_object()) failure.witness = json::object();
    if (archive && archive->owner_scope() != custody.at("owner_scope").get<std::string>())
        throw std::invalid_argument("Program provider failure archive owner mismatch");
    if (has_provider_outcome(failure) &&
        !failure.witness.at("provider_outcome").at("native_archive_reference").is_null() && !archive)
        throw std::invalid_argument("Reused provider evidence requires its authentic archive");
    auto encoded = provider_codec::encode_outcome(
        *failure.provider_outcome, archive, canonical_json_bytes(custody));
    failure.witness["provider_outcome"] = std::move(encoded);
    failure.witness["provider_outcome_custody"] = custody;
}

inline void restore_provider_failure(
    ProgramFailure& failure, const json& custody,
    const std::shared_ptr<sp::NativeArchive>& archive) {
    if (!has_provider_outcome(failure) || failure.provider_outcome) return;
    if (!failure.witness.contains("provider_outcome_custody") ||
        failure.witness.at("provider_outcome_custody") != custody)
        throw std::invalid_argument("Stored Program provider failure custody mismatch");
    const auto& encoded = failure.witness.at("provider_outcome");
    if (!encoded.at("native_archive_reference").is_null() &&
        (!archive || archive->owner_scope() != custody.at("owner_scope").get<std::string>()))
        throw std::invalid_argument("Stored Program provider failure requires its owner archive");
    failure.provider_outcome = provider_codec::decode_outcome(
        encoded, archive, canonical_json_bytes(custody));
}

}  // namespace neograph::program::detail
