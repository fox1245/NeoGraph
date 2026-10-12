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
 *
 * emit_async() is the one boundary every emitter shares (tool calls on every
 * path, checkpoints, providers, Program lifecycle events): a mandatory Hook
 * that does not succeed throws HookBoundaryBlocked; one that cannot even run
 * (an invalid event such as one without an owner scope, an unusable journal)
 * throws HookBoundaryBlocked with state `failed`; and when `cancellation` was
 * cancelled while the Hooks ran, graph::CancelledException wins.
 */
class NEOGRAPH_API HookRuntime final {
public:
    explicit HookRuntime(std::shared_ptr<MandatoryHookRunner> runner,
                         std::chrono::milliseconds default_timeout = std::chrono::seconds(30));

    /**
     * Both runtimes of a nested run, outer first: a subgraph whose engine has
     * its own HookRuntime under a parent run that has one. Every event goes
     * through `outer`'s Hooks and then `inner`'s, and a block from either
     * blocks. A null side yields the other one, and the same runtime on both
     * sides is that runtime, so its Hooks run once.
     */
    [[nodiscard]] static std::shared_ptr<HookRuntime> compose(std::shared_ptr<HookRuntime> outer,
                                                              std::shared_ptr<HookRuntime> inner);

    asio::awaitable<void> emit_async(HookPhase phase, std::string type,
                                       std::string owner_scope, std::string run_id, json data,
                                       std::shared_ptr<graph::CancelToken> cancellation = {},
                                       std::optional<std::chrono::system_clock::time_point> parent_deadline = {},
                                       std::optional<std::uint64_t> event_sequence = {});

    /**
     * Emits an event whose `data` carries a durable identity, such as a tool
     * call's (run, thread, task with its node attempt, call ordinal), at most
     * once for that identity and content: a resume or a replay that emits it
     * again finds it delivered in the journal and runs no Hook. An earlier
     * emission that did not pass (its Hook failed, timed out or was
     * cancelled, which stopped its run) is not a delivery, so the next one
     * runs the Hooks again under a new event identity. One that still needs
     * reconciliation blocks as before.
     */
    asio::awaitable<void> emit_once_async(HookPhase phase, std::string type,
                                            std::string owner_scope, std::string run_id,
                                            json data,
                                            std::shared_ptr<graph::CancelToken> cancellation = {},
                                            std::optional<std::chrono::system_clock::time_point> parent_deadline = {});

    /// Retained for source and binary compatibility. Hook-target origin is now
    /// explicit in NativeHookExecutionAdapter's direct execution path.
    [[nodiscard]] static bool executing_hook_target() noexcept;

private:
    HookRuntime(std::shared_ptr<HookRuntime> outer, std::shared_ptr<HookRuntime> inner);

    std::shared_ptr<MandatoryHookRunner> runner_;
    std::chrono::milliseconds default_timeout_;
    std::atomic<std::uint64_t> next_sequence_{0};
    /// A composed runtime (compose()) has no runner of its own: it emits
    /// through `outer_` and then `inner_`.
    std::shared_ptr<HookRuntime> outer_;
    std::shared_ptr<HookRuntime> inner_;
};

} // namespace neograph
