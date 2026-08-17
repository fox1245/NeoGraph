#pragma once

#include <neograph/api.h>
#include <neograph/controlled_provider.h>
#include <neograph/runtime_turn_assembler.h>
#include <neograph/hook_runtime.h>

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
    ~RuntimeInterpositionController();
    RuntimeInterpositionController(RuntimeInterpositionController&&) noexcept;
    RuntimeInterpositionController& operator=(RuntimeInterpositionController&&) noexcept;
    RuntimeInterpositionController(const RuntimeInterpositionController&) = delete;
    RuntimeInterpositionController& operator=(const RuntimeInterpositionController&) = delete;

    /// Select the immutable context generation used by subsequent dispatches.
    void activate(std::string owner_id, ContextEpoch epoch);
    void clear() noexcept;
    bool active() const noexcept;
    /// Additive host resource; absent preserves legacy dispatch behavior.
    void set_hook_runtime(std::shared_ptr<HookRuntime> hooks);

    /// Assemble, journal, and dispatch. Active epoch history is authoritative.
    asio::awaitable<ChatCompletion> invoke_async(CompletionParams params,
                                                   StreamCallback on_chunk = {});
    /// Built-in consumers may retain host-owned instructions and add trusted task
    /// input while replacing caller conversation with admitted epoch RAW history.
    asio::awaitable<ChatCompletion> invoke_async(CompletionParams params,
                                                   StreamCallback on_chunk,
                                                   std::vector<ChatMessage> host_instructions,
                                                   std::vector<ChatMessage> trusted_supplemental);
    ChatCompletion invoke(CompletionParams params, StreamCallback on_chunk = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph
