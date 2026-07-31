/**
 * @file program/event.h
 * @brief Ordered event envelope for one Program run attempt.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace neograph::program {

enum class ProgramEventKind : std::uint8_t {
    Started,
    Core,
    CheckpointPublished,
    Terminal,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramEventKind kind) noexcept;
NEOGRAPH_PROGRAM_API ProgramEventKind program_event_kind_from_string(std::string_view value);

struct ProgramStartedEvent {
    RunBudget budget;
    bool operator==(const ProgramStartedEvent&) const = default;
};

struct ProgramCheckpointEvent {
    CoreCheckpointIdentity checkpoint;
    bool operator==(const ProgramCheckpointEvent&) const = default;
};

struct ProgramTerminalEvent {
    ProgramTerminalStatus status;
    bool operator==(const ProgramTerminalEvent&) const = default;
};

using ProgramEventPayload = std::variant<ProgramStartedEvent,
                                         graph::TypedGraphEvent,
                                         ProgramCheckpointEvent,
                                         ProgramTerminalEvent>;

struct NEOGRAPH_PROGRAM_API ProgramEvent {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    std::string         id;
    std::uint64_t       sequence;
    std::int64_t        timestamp_ms;
    std::string         run_id;
    std::string         program_version_id;
    std::string         bundle_id;
    std::string         operation_id;
    std::string         core_generation_id;
    std::string         core_run_id;
    std::string         trace_id;
    std::uint64_t       attempt;
    ProgramEventKind    kind;
    ProgramEventPayload payload;
    static ProgramEvent create(ProgramEvent value);
    static ProgramEvent parse(std::string_view stored_bytes);
    std::string         serialize_canonical() const;
};

class NEOGRAPH_PROGRAM_API ProgramEventSink {
public:
    virtual ~ProgramEventSink()                      = default;
    virtual void on_event(const ProgramEvent& event) = 0;
};

}  // namespace neograph::program
