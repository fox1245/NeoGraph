/**
 * @file program/handle.h
 * @brief Shared control handle for one Program attempt.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/event.h>
#include <neograph/program/result.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::program {

namespace detail {
class RunControl;
}

class NEOGRAPH_PROGRAM_API ProgramHandle {
public:
    ProgramHandle(const ProgramHandle&) noexcept            = default;
    ProgramHandle& operator=(const ProgramHandle&) noexcept = default;
    ProgramHandle(ProgramHandle&&) noexcept                 = default;
    ProgramHandle& operator=(ProgramHandle&&) noexcept      = default;
    ~ProgramHandle();

    const std::string& run_id() const noexcept;
    const std::string& program_version_id() const noexcept;
    std::uint64_t      attempt() const noexcept;

    bool                                  cancel() noexcept;
    ProgramResult                         wait() const;
    asio::awaitable<ProgramResult>        wait_async() const;
    std::optional<ProgramResult>          try_result() const;
    std::vector<ProgramEvent>             events_after(std::uint64_t sequence) const;
    std::optional<CoreCheckpointIdentity> latest_checkpoint() const;

private:
    explicit ProgramHandle(std::shared_ptr<detail::RunControl> control);
    std::shared_ptr<detail::RunControl> control_;

    friend class ProgramRuntime;
};

}  // namespace neograph::program
