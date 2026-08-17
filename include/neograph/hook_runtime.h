/** @file hook_runtime.h @brief Host-owned lifecycle hook boundary. */
#pragma once

#include <neograph/hook_outbox.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace neograph::graph { class CancelToken; }

namespace neograph {

/**
 * Owns lifecycle-event identity and dispatch for one admitted host runtime.
 * Hook targets execute through NativeHookExecutionAdapter, never through this
 * facade's tool boundary, so hook-target execution cannot recursively trigger
 * tool hooks.
 */
class NEOGRAPH_API HookRuntime final {
public:
    explicit HookRuntime(std::shared_ptr<MandatoryHookRunner> runner,
                         std::chrono::milliseconds default_timeout = std::chrono::seconds(30));

    asio::awaitable<void> emit_async(HookPhase phase, std::string type,
                                       std::string owner_scope, std::string run_id, json data,
                                       std::shared_ptr<graph::CancelToken> cancellation = {},
                                       std::optional<std::chrono::system_clock::time_point> parent_deadline = {},
                                       std::optional<std::uint64_t> event_sequence = {});

    /// Retained for source and binary compatibility. Hook-target origin is now
    /// explicit in NativeHookExecutionAdapter's direct execution path.
    [[nodiscard]] static bool executing_hook_target() noexcept;

private:
    std::shared_ptr<MandatoryHookRunner> runner_;
    std::chrono::milliseconds default_timeout_;
    std::atomic<std::uint64_t> next_sequence_{0};
};

} // namespace neograph
