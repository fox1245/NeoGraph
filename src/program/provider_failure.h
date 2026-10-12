#pragma once

#include <neograph/program/result.h>
#include <neograph/provider_outcome_codec.h>
#include "canonical_json.h"

#include <memory>
#include <type_traits>
#include <variant>
#include <vector>

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

/// @p outcome without the provider's native replay state: the same messages,
/// usage, stop reason, error and raw events, with each message's native seal
/// left out. `changed` tells whether any message had one.
inline std::shared_ptr<const sp::Outcome> without_native_state(const sp::Outcome& outcome,
                                                               bool& changed) {
    sp::Outcome copy = outcome;
    changed          = false;
    std::visit(
        [&changed](auto& item) {
            using Item = std::decay_t<decltype(item)>;
            auto& messages = [&item]() -> std::vector<sp::Message>& {
                if constexpr (std::is_same_v<Item, sp::Completion>) return item.messages;
                else return item.partial.messages;
            }();
            for (auto& message : messages) {
                if (!message.native) continue;
                message.native.reset();
                changed = true;
            }
        },
        copy);
    return std::make_shared<const sp::Outcome>(std::move(copy));
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
    if (!archive) {
        // Native replay state is kept only under an archive's authenticated
        // custody. A host without one still publishes the failure, with the
        // portable part of its evidence and a witness that the native part was
        // not kept (#426); it never writes the native state without custody.
        bool        changed  = false;
        auto        portable = without_native_state(*failure.provider_outcome, changed);
        if (changed) {
            failure.provider_outcome = std::move(portable);
            failure.witness["provider_outcome_native_retained"] = false;
        }
    }
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
