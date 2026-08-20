/** @file strict_runtime.h @brief Sealed assembly of NeoGraph's strict provider path. */
#pragma once

#include <neograph/context_store.h>
#include <neograph/controlled_provider.h>
#include <neograph/hook_runtime.h>
#include <neograph/runtime_interposition_controller.h>

#include <cstdint>
#include <memory>
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
 */
class NEOGRAPH_API StrictRuntimeProfile final {
public:
    explicit StrictRuntimeProfile(StrictRuntimeProfileConfig config);

    void activate(std::string owner_id, ContextEpoch epoch);
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
