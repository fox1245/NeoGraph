/**
 * @file program/command_journal.h
 * @brief Durable coordinates and results for JavaScript control commands.
 *
 * A command journal entry is deliberately independent from the JavaScript
 * VM.  The command coordinate is derived from the admitted bundle and the
 * exact sealed command value; a later runtime can therefore validate the
 * value before consuming a recorded result.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/command.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/** One append-only journal entry for one yielded JavaScript command. */
struct ProgramJavaScriptCommandJournalEntryData {
    std::uint64_t              sequence = 0;
    std::string                bundle_id;
    std::uint64_t              command_ordinal = 0;
    JavaScriptCommand          command = JavaScriptCommand::call_core(
        "journal:default", "journal:default", json::object());
    std::optional<std::string> effect_identity;
    /** Null for a durable pending head; present for an authoritative result. */
    std::optional<json>        terminal_result;
};

/**
 * Immutable, content-addressed command-journal value.
 *
 * The `coordinate_id()` is stable across the pending and completed append for
 * one command.  `id()` also includes the append sequence and result, so the
 * append-only transition log can retain both states without an in-place
 * update.
 */
class NEOGRAPH_PROGRAM_API ProgramJavaScriptCommandJournalEntry final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData data);
    static ProgramJavaScriptCommandJournalEntry parse(std::string_view stored_bytes);

    std::uint64_t              sequence() const noexcept;
    const std::string&         bundle_id() const noexcept;
    std::uint64_t              command_ordinal() const noexcept;
    const JavaScriptCommand&   command() const noexcept;
    std::optional<std::string> effect_identity() const;
    std::optional<json>        terminal_result() const;
    bool                       pending() const noexcept;
    bool                       completed() const noexcept;
    /** Stable identity for the command coordinate, excluding the result. */
    const std::string&         coordinate_id() const noexcept;
    /** Content identity for this append-only entry. */
    const std::string&         id() const noexcept;
    std::string                serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramJavaScriptCommandJournalEntry(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
