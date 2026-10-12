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
    /**
     * Pending entries only: the members of a paused join that already
     * settled, as an object from member operation id to that member's
     * recorded result. A paused join appends it after its pending head, and
     * the attempt that resumes the join replays these members from it.
     */
    std::optional<json>        settled_members;
    /**
     * Pending entries only (#391): when each `ng.await` with a timeout in this
     * command started, as an object from the await's operation id to Unix
     * epoch milliseconds. An await that starts appends it after the pending
     * head, every later pending entry of the coordinate carries it forward,
     * and the attempt that resumes or recovers the command enforces each
     * await's remaining time from its recorded start.
     */
    std::optional<json>        await_started_at;
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
    /// Version 2 adds settled_members and version 3 await_started_at; an
    /// entry uses the lowest version that holds its fields, so entries without
    /// them keep version 1 and their bytes and ids.
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 3;

    explicit ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData data);
    static ProgramJavaScriptCommandJournalEntry parse(std::string_view stored_bytes);

    std::uint64_t              sequence() const noexcept;
    const std::string&         bundle_id() const noexcept;
    std::uint64_t              command_ordinal() const noexcept;
    const JavaScriptCommand&   command() const noexcept;
    std::optional<std::string> effect_identity() const;
    std::optional<json>        terminal_result() const;
    std::optional<json>        settled_members() const;
    std::optional<json>        await_started_at() const;
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
