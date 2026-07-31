/**
 * @file program/runtime.h
 * @brief Execution service for admitted immutable Programs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/program/catalog.h>
#include <neograph/program/handle.h>
#include <neograph/program/journal.h>

#include <asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

struct ProgramInvocation {
    json                              input;
    RunBudget                         budget;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
};

struct ProgramResume {
    json                              value;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
};

struct RuntimeConfig {
    std::shared_ptr<ProgramCatalog>         catalog;
    std::shared_ptr<graph::CheckpointStore> checkpoints;
    std::shared_ptr<graph::Store>           state_store;
    std::shared_ptr<ProgramJournal>         journal;
    std::size_t                             scheduler_threads = 1;
};

class NEOGRAPH_PROGRAM_API ProgramRuntime {
public:
    explicit ProgramRuntime(RuntimeConfig config);
    ProgramRuntime(ProgramRuntime&&) noexcept;
    ProgramRuntime& operator=(ProgramRuntime&&) noexcept;
    ProgramRuntime(const ProgramRuntime&)            = delete;
    ProgramRuntime& operator=(const ProgramRuntime&) = delete;
    ~ProgramRuntime();

    ProgramHandle start(const ProgramVersion& version, ProgramInvocation invocation);
    ProgramHandle resume(std::string_view run_id, ProgramResume resume);

    asio::awaitable<ProgramResult> run_async(ProgramVersion version, ProgramInvocation invocation);
    asio::awaitable<ProgramResult> resume_async(std::string run_id, ProgramResume resume);
    ProgramResult                  run(const ProgramVersion& version, ProgramInvocation invocation);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
