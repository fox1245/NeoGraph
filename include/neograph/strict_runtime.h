/** @file strict_runtime.h @brief Sealed assembly of NeoGraph's strict provider path. */
#pragma once

#include <neograph/context_store.h>
#include <neograph/controlled_provider.h>
#include <neograph/hook_runtime.h>
#include <neograph/runtime_interposition_controller.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph { class GraphEngine; }

namespace neograph {

struct StrictRuntimeProfileConfig {
    std::shared_ptr<Provider> provider;
    std::shared_ptr<DurableContextStore> context_store;
    std::shared_ptr<DurableProviderDispatchReceiptStore> dispatch_store;
    std::shared_ptr<HookRuntime> hook_runtime;
    std::string provider_binding_identity;
    /// Input ceiling for one assembled request. It is compared with
    /// RuntimeTurnAssembler::estimate_input_tokens, which is the encoded
    /// request's bytes divided by three: a heuristic over the whole encoded
    /// request (messages, tool schemas, provider framing), not only the visible
    /// text, and not a proven upper bound of what a provider counts or bills.
    std::uint64_t max_input_tokens = 0;
    std::vector<std::string> required_context_artifact_ids;
    std::vector<std::string> required_skill_artifact_ids;
};

/**
 * Host-owned strict runtime assembly.
 *
 * Generated graphs and models never receive the raw provider or stores. The
 * trusted host constructs this profile, activates an exact Strict ContextEpoch,
 * and attaches it to a GraphEngine before execution. Built-in provider and tool
 * consumers then share the same interposition and Hook boundaries.
 *
 * Host-authored custom nodes remain trusted native code; a host that gives such
 * a node a raw Provider has deliberately left this profile's guarantee.
 *
 * Host-stepped contract: a provider call is built from the active epoch's
 * fixed RAW range, never from the messages a graph or agent presents. The host
 * therefore appends RAW records and activates a new epoch for every provider
 * call: the model's reply and the tool results of the previous turn first, then
 * the epoch that covers them. Attaching the profile to a plain GraphEngine does
 * not do this for a loop inside one run (LLM call, tool dispatch, LLM call, as
 * in create_react_graph). When a call arrives for an epoch whose turn the graph
 * has already advanced, that is, the presented messages hold assistant or tool
 * messages added since the epoch's first dispatch, the call is refused with a
 * std::logic_error that names this contract instead of silently resending the
 * first turn's context. A retry of the same turn (same presented messages) is
 * still accepted, and activating an epoch starts the next turn.
 *
 * See docs/STRICT_RUNTIME_INTERPOSITION.md for what the messages channel,
 * history::compact_history, the recorded profile and this profile each
 * guarantee.
 */
class NEOGRAPH_API StrictRuntimeProfile final {
public:
    explicit StrictRuntimeProfile(StrictRuntimeProfileConfig config);

    /** See RuntimeInterpositionController::activate: an epoch that claims to extend its
     *  predecessor is verified against the given predecessor or the active epoch. */
    void activate(std::string owner_id, ContextEpoch epoch);
    void activate(std::string owner_id, ContextEpoch epoch,
                  std::optional<ContextEpoch> predecessor);
    void clear() noexcept;
    bool active() const noexcept;

    void attach(graph::GraphEngine& engine) const;

    std::shared_ptr<RuntimeInterpositionController> interposition() const noexcept;
    std::shared_ptr<HookRuntime> hooks() const noexcept;
    std::shared_ptr<DurableContextStore> context_store() const noexcept;
    std::shared_ptr<DurableProviderDispatchReceiptStore> dispatch_store() const noexcept;

private:
    std::shared_ptr<DurableContextStore> context_store_;
    std::shared_ptr<DurableProviderDispatchReceiptStore> dispatch_store_;
    std::shared_ptr<HookRuntime> hook_runtime_;
    std::shared_ptr<RuntimeInterpositionController> interposition_;
};

}  // namespace neograph
