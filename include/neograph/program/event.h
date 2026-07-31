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
#include <variant>

namespace neograph::program {

enum class ProgramEventKind : std::uint8_t {
    Started,
    Core,
    CheckpointPublished,
    Terminal,
};

struct ProgramStartedEvent {
    RunBudget budget;
};

struct ProgramCheckpointEvent {
    CoreCheckpointIdentity checkpoint;
};

struct ProgramTerminalEvent {
    ProgramTerminalStatus status;
};

using ProgramEventPayload = std::variant<ProgramStartedEvent,
                                         graph::TypedGraphEvent,
                                         ProgramCheckpointEvent,
                                         ProgramTerminalEvent>;

struct ProgramEvent {
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
};

class NEOGRAPH_PROGRAM_API ProgramEventSink {
public:
    virtual ~ProgramEventSink()                      = default;
    virtual void on_event(const ProgramEvent& event) = 0;
};

}  // namespace neograph::program
