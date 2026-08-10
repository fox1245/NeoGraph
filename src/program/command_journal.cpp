#include <neograph/program/command_journal.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORMAT = "neograph-program-javascript-command-journal-entry";

std::string require_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("JavaScript command journal field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw std::invalid_argument("JavaScript command journal field '" + key +
                                    "' must be unsigned");
    }
    return value.at(key).get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view field) {
    const auto number = require_uint64(value, field);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("JavaScript command journal integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

json coordinate_body(const ProgramJavaScriptCommandJournalEntryData& data) {
    const auto command = data.command.to_json();
    return json{{"bundle_id", data.bundle_id},
                {"command_ordinal", data.command_ordinal},
                {"protocol_version", command.at("protocol_version")},
                {"kind", command.at("kind")},
                {"import_slot", command.at("import_slot")},
                {"source_site", command.at("source_site")},
                {"arguments", command.at("arguments")},
                {"effect_identity",
                 data.effect_identity ? json(*data.effect_identity) : json(nullptr)}};
}

std::string coordinate_id_for(const ProgramJavaScriptCommandJournalEntryData& data) {
    return detail::sha256_identity("program-javascript-command-coordinate/v1",
                                   detail::canonical_json_bytes(coordinate_body(data)));
}

json entry_body(const ProgramJavaScriptCommandJournalEntryData& data) {
    return json{{"format", std::string(FORMAT)},
                {"storage_schema_version",
                 ProgramJavaScriptCommandJournalEntry::STORAGE_SCHEMA_VERSION},
                {"sequence", data.sequence},
                {"bundle_id", data.bundle_id},
                {"command_ordinal", data.command_ordinal},
                {"command", data.command.to_json()},
                {"effect_identity",
                 data.effect_identity ? json(*data.effect_identity) : json(nullptr)},
                {"terminal_result",
                 data.terminal_result ? detail::owned_json_copy(*data.terminal_result)
                                      : json(nullptr)}};
}

std::string entry_id_for(const ProgramJavaScriptCommandJournalEntryData& data) {
    return detail::sha256_identity("program-javascript-command-journal-entry/v1",
                                   detail::canonical_json_bytes(entry_body(data)));
}

void validate_data(const ProgramJavaScriptCommandJournalEntryData& data) {
    if (data.sequence == 0 || data.command_ordinal == 0) {
        throw std::invalid_argument(
            "JavaScript command journal sequence and ordinal must be positive");
    }
    if (!detail::is_sha256_identity(data.bundle_id)) {
        throw std::invalid_argument("JavaScript command journal bundle_id must be a sha256 identity");
    }
    if (data.effect_identity && !detail::is_sha256_identity(*data.effect_identity)) {
        throw std::invalid_argument(
            "JavaScript command journal effect_identity must be a sha256 identity");
    }
    if (data.terminal_result) {
        // Results are intentionally opaque to this storage value.  The
        // runtime validates the command-specific envelope before replay; the
        // journal only guarantees that the retained value is owned JSON.
        (void)detail::canonical_json_bytes(*data.terminal_result);
    }
}

}  // namespace

struct ProgramJavaScriptCommandJournalEntry::Impl {
    explicit Impl(ProgramJavaScriptCommandJournalEntryData value)
        : data(std::move(value)), coordinate_id(coordinate_id_for(data)),
          id(entry_id_for(data)) {}

    ProgramJavaScriptCommandJournalEntryData data;
    std::string                            coordinate_id;
    std::string                            id;
};

ProgramJavaScriptCommandJournalEntry::ProgramJavaScriptCommandJournalEntry(
    ProgramJavaScriptCommandJournalEntryData data) {
    validate_data(data);
    data.terminal_result = data.terminal_result
                               ? std::optional<json>(detail::owned_json_copy(*data.terminal_result))
                               : std::nullopt;
    impl_ = std::make_shared<const Impl>(std::move(data));
}

ProgramJavaScriptCommandJournalEntry::ProgramJavaScriptCommandJournalEntry(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramJavaScriptCommandJournalEntry ProgramJavaScriptCommandJournalEntry::parse(
    std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != FORMAT) {
        throw std::invalid_argument("Stored JavaScript command journal entry has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored JavaScript command journal entry",
        {"format", "storage_schema_version", "id", "sequence", "bundle_id",
         "command_ordinal", "command", "effect_identity", "terminal_result"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored JavaScript command journal schema unsupported");
    }
    const auto& effect = value.at("effect_identity");
    std::optional<std::string> effect_identity;
    if (!effect.is_null()) effect_identity = require_string(value, "effect_identity");
    const auto& result = value.at("terminal_result");
    std::optional<json> terminal_result;
    if (!result.is_null()) terminal_result = detail::owned_json_copy(result);

    ProgramJavaScriptCommandJournalEntry parsed(
        ProgramJavaScriptCommandJournalEntryData{
            require_uint64(value, "sequence"),
            require_string(value, "bundle_id"),
            require_uint64(value, "command_ordinal"),
            JavaScriptCommand::from_json(value.at("command")),
            std::move(effect_identity),
            std::move(terminal_result)});
    if (parsed.id() != require_string(value, "id")) {
        throw std::invalid_argument("Stored JavaScript command journal entry id mismatch");
    }
    return parsed;
}

std::uint64_t ProgramJavaScriptCommandJournalEntry::sequence() const noexcept {
    return impl_->data.sequence;
}

const std::string& ProgramJavaScriptCommandJournalEntry::bundle_id() const noexcept {
    return impl_->data.bundle_id;
}

std::uint64_t ProgramJavaScriptCommandJournalEntry::command_ordinal() const noexcept {
    return impl_->data.command_ordinal;
}

const JavaScriptCommand& ProgramJavaScriptCommandJournalEntry::command() const noexcept {
    return impl_->data.command;
}

std::optional<std::string> ProgramJavaScriptCommandJournalEntry::effect_identity() const {
    return impl_->data.effect_identity;
}

std::optional<json> ProgramJavaScriptCommandJournalEntry::terminal_result() const {
    return impl_->data.terminal_result
               ? std::optional<json>(detail::owned_json_copy(*impl_->data.terminal_result))
               : std::nullopt;
}

bool ProgramJavaScriptCommandJournalEntry::pending() const noexcept {
    return !impl_->data.terminal_result.has_value();
}

bool ProgramJavaScriptCommandJournalEntry::completed() const noexcept {
    return impl_->data.terminal_result.has_value();
}

const std::string& ProgramJavaScriptCommandJournalEntry::coordinate_id() const noexcept {
    return impl_->coordinate_id;
}

const std::string& ProgramJavaScriptCommandJournalEntry::id() const noexcept {
    return impl_->id;
}

std::string ProgramJavaScriptCommandJournalEntry::serialize_canonical() const {
    validate_data(impl_->data);
    if (coordinate_id_for(impl_->data) != impl_->coordinate_id ||
        entry_id_for(impl_->data) != impl_->id) {
        throw std::invalid_argument("JavaScript command journal entry identity mismatch");
    }
    auto value = entry_body(impl_->data);
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}

}  // namespace neograph::program
