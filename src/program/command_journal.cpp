#include <neograph/program/command_journal.h>

#include "canonical_json.h"
#include "../core/json_builder.h"

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

// Coordinate and entry bodies are built in one document (#478). Each make()
// takes the argument the json initializer list took, so the members keep
// their types.
using Builder = ::neograph::detail::JsonBuilder;
json coordinate_body(const ProgramJavaScriptCommandJournalEntryData& data) {
    const auto command = data.command.to_json();
    Builder    j;
    auto*      body = j.object(
        {{"bundle_id", j.make(data.bundle_id)},
         {"command_ordinal", j.make(data.command_ordinal)},
         {"protocol_version", j.make(command.at("protocol_version"))},
         {"kind", j.make(command.at("kind"))},
         {"import_slot", j.make(command.at("import_slot"))},
         {"source_site", j.make(command.at("source_site"))},
         {"arguments", j.make(command.at("arguments"))},
         {"effect_identity", data.effect_identity ? j.make(*data.effect_identity) : j.null()}});
    return std::move(j).finish(body);
}

std::string coordinate_id_for(const ProgramJavaScriptCommandJournalEntryData& data) {
    return detail::sha256_identity("program-javascript-command-coordinate/v1",
                                   detail::canonical_json_bytes(coordinate_body(data)));
}

constexpr std::uint32_t SETTLED_MEMBERS_SCHEMA_VERSION = 2;
constexpr std::uint32_t AWAIT_STARTS_SCHEMA_VERSION    = 3;

// An entry uses the lowest version that holds its fields: one that carries
// await starts uses version 3 (#391), one that carries only settled members
// version 2 (#380), and every other entry keeps version 1, its bytes and id.
std::uint32_t storage_schema_version(const ProgramJavaScriptCommandJournalEntryData& data) {
    if (data.await_started_at) return AWAIT_STARTS_SCHEMA_VERSION;
    return data.settled_members ? SETTLED_MEMBERS_SCHEMA_VERSION : 1U;
}

json entry_body(const ProgramJavaScriptCommandJournalEntryData& data) {
    Builder j;
    auto*   body = j.object(
        {{"format", j.make(std::string(FORMAT))},
         {"storage_schema_version", j.make(storage_schema_version(data))},
         {"sequence", j.make(data.sequence)},
         {"bundle_id", j.make(data.bundle_id)},
         {"command_ordinal", j.make(data.command_ordinal)},
         {"command", j.make(data.command.to_json())},
         {"effect_identity", data.effect_identity ? j.make(*data.effect_identity) : j.null()},
         {"terminal_result",
          data.terminal_result ? j.make(detail::owned_json_copy(*data.terminal_result))
                               : j.null()}});
    if (data.settled_members)
        j.set(body, "settled_members", j.make(detail::owned_json_copy(*data.settled_members)));
    if (data.await_started_at)
        j.set(body, "await_started_at", j.make(detail::owned_json_copy(*data.await_started_at)));
    return std::move(j).finish(body);
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
    if (data.settled_members) {
        // Member results are opaque here as well; the runtime validates each
        // one before it replays the member.
        if (data.terminal_result || data.command.kind() != JavaScriptCommandKind::Join ||
            !data.settled_members->is_object() || data.settled_members->empty()) {
            throw std::invalid_argument(
                "JavaScript command journal settled members belong to a pending join");
        }
        (void)detail::canonical_json_bytes(*data.settled_members);
    }
    if (data.await_started_at) {
        const auto& starts = *data.await_started_at;
        if (data.terminal_result || !starts.is_object() || starts.empty()) {
            throw std::invalid_argument(
                "JavaScript command journal await starts belong to a pending command");
        }
        for (const auto& [operation_id, started_at] : starts.items()) {
            const bool milliseconds =
                started_at.is_number_unsigned() ||
                (started_at.is_number_integer() && started_at.get<std::int64_t>() >= 0);
            if (operation_id.empty() || !milliseconds) {
                throw std::invalid_argument(
                    "JavaScript command journal await start must be unsigned milliseconds");
            }
        }
    }
}

}  // namespace

struct ProgramJavaScriptCommandJournalEntry::Impl {
    explicit Impl(ProgramJavaScriptCommandJournalEntryData value)
        : data(std::move(value)), coordinate_id(coordinate_id_for(data)),
          id(entry_id_for(data)) {
        auto canonical = entry_body(data);
        canonical["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(canonical);
    }

    ProgramJavaScriptCommandJournalEntryData data;
    std::string                            coordinate_id;
    std::string                            id;
    std::string                            canonical_bytes;
};

ProgramJavaScriptCommandJournalEntry::ProgramJavaScriptCommandJournalEntry(
    ProgramJavaScriptCommandJournalEntryData data) {
    validate_data(data);
    data.terminal_result = data.terminal_result
                               ? std::optional<json>(detail::owned_json_copy(*data.terminal_result))
                               : std::nullopt;
    data.settled_members = data.settled_members
                               ? std::optional<json>(detail::owned_json_copy(*data.settled_members))
                               : std::nullopt;
    data.await_started_at = data.await_started_at
                                ? std::optional<json>(detail::owned_json_copy(*data.await_started_at))
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
         "command_ordinal", "command", "effect_identity", "terminal_result",
         "settled_members", "await_started_at"});
    const auto schema_version = require_uint32(value, "storage_schema_version");
    if (schema_version == 0 || schema_version > STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored JavaScript command journal schema unsupported");
    }
    // Each version holds exactly the fields that select it.
    if ((schema_version == AWAIT_STARTS_SCHEMA_VERSION) != value.contains("await_started_at") ||
        (schema_version == SETTLED_MEMBERS_SCHEMA_VERSION && !value.contains("settled_members")) ||
        (schema_version == 1 && value.contains("settled_members"))) {
        throw std::invalid_argument(
            "Stored JavaScript command journal settled members or await starts disagree with its "
            "schema");
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
            std::move(terminal_result),
            value.contains("settled_members")
                ? std::optional<json>(detail::owned_json_copy(value.at("settled_members")))
                : std::nullopt,
            value.contains("await_started_at")
                ? std::optional<json>(detail::owned_json_copy(value.at("await_started_at")))
                : std::nullopt});
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

std::optional<json> ProgramJavaScriptCommandJournalEntry::settled_members() const {
    return impl_->data.settled_members
               ? std::optional<json>(detail::owned_json_copy(*impl_->data.settled_members))
               : std::nullopt;
}

std::optional<json> ProgramJavaScriptCommandJournalEntry::await_started_at() const {
    return impl_->data.await_started_at
               ? std::optional<json>(detail::owned_json_copy(*impl_->data.await_started_at))
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
    return impl_->canonical_bytes;
}

}  // namespace neograph::program
