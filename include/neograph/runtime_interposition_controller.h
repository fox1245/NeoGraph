#pragma once

#include <neograph/api.h>
#include <neograph/controlled_provider.h>
#include <neograph/runtime_turn_assembler.h>
#include <neograph/hook_runtime.h>
#include <neograph/graph/provider_call_broker.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph {

/**
 * Explicit provider interposition for runtimes that have admitted context.
 * Direct Provider use remains outside this boundary by design.
 */
class NEOGRAPH_API RuntimeInterpositionController final {
public:
    RuntimeInterpositionController(std::shared_ptr<Provider> provider,
                                   std::shared_ptr<ContextStore> context_store,
                                    std::shared_ptr<ProviderDispatchReceiptStore> dispatch_store,
                                    std::string provider_binding_identity,
                                    std::uint64_t max_input_tokens = 0,
                                    std::vector<std::string> static_required_skill_artifact_ids = {});
    RuntimeInterpositionController(
        std::shared_ptr<Provider> provider,
        std::shared_ptr<ContextStore> context_store,
        std::shared_ptr<ProviderDispatchReceiptStore> dispatch_store,
        std::string provider_binding_identity,
        RuntimeContextRequirements requirements,
        std::uint64_t max_input_tokens);
    ~RuntimeInterpositionController();
    RuntimeInterpositionController(RuntimeInterpositionController&&) noexcept;
    RuntimeInterpositionController& operator=(RuntimeInterpositionController&&) noexcept;
    RuntimeInterpositionController(const RuntimeInterpositionController&) = delete;
    RuntimeInterpositionController& operator=(const RuntimeInterpositionController&) = delete;

    /// Select the immutable context generation used by subsequent dispatches.
    ///
    /// An epoch that claims to extend its predecessor is verified first
    /// (verify_context_epoch_claim). The predecessor is the one given, or else
    /// the epoch active for this owner when its id is the epoch's predecessor_id.
    /// A claim without a predecessor of either kind, or one that does not hold, is
    /// refused with ContextStabilityError and the active epoch stays as it was.
    /// An epoch that claims nothing, or that breaks, is activated as before.
    void activate(std::string owner_id, ContextEpoch epoch);
    void activate(std::string owner_id, ContextEpoch epoch,
                  std::optional<ContextEpoch> predecessor);
    void clear() noexcept;
    bool active() const noexcept;
    /// Additive host resource; absent preserves legacy dispatch behavior.
    void set_hook_runtime(std::shared_ptr<HookRuntime> hooks);

    /// Assemble, journal, and dispatch. Active epoch history is authoritative.
    ///
    /// Host-stepped: the prompt comes from the epoch's fixed RAW range, so the
    /// host appends RAW records and activates a new epoch for every provider
    /// call. A call that presents assistant or tool messages added since the
    /// epoch's first dispatch (a graph or agent loop that advanced inside one
    /// run) throws std::logic_error naming this contract. The same presented
    /// messages again (a retry of the turn) are accepted; activate() and
    /// clear() forget what was presented.
    asio::awaitable<sp::runtime::Result> invoke_async(
        ProviderRequest request,
        std::vector<sp::Message> host_instructions = {},
        std::vector<sp::Message> trusted_supplemental = {},
        graph::ProviderCallIdentity identity = {});
    sp::runtime::Result invoke(
        ProviderRequest request, graph::ProviderCallIdentity identity = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph
