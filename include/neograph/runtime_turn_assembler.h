#pragma once

#include <neograph/api.h>
#include <neograph/completion_provider.h>
#include <neograph/context_store.h>
#include <neograph/runtime_context.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph {

class NEOGRAPH_API ContextBudgetBlocked final : public std::runtime_error {
public:
    ContextBudgetBlocked();
};

/** A normalized request and its immutable evidence of context assembly. */
struct NEOGRAPH_API RuntimeTurn {
    CompletionRequest request;
    ContextAssemblyReceipt assembly_receipt;
};

struct RuntimeContextRequirements {
    /** Every identity must appear in the active epoch and name a required artifact. */
    std::vector<std::string> required_artifact_ids;
    /** Subset of required_artifact_ids that must be RequiredSkill artifacts. */
    std::vector<std::string> required_skill_artifact_ids;
};

/**
 * Hydrates and verifies a ContextEpoch before constructing a provider request.
 *
 * The supplied request is a transport template: its messages must be empty so
 * every prompt message is bound to the epoch's raw feed or selected artifacts.
 */
class NEOGRAPH_API RuntimeTurnAssembler final {
public:
    RuntimeTurnAssembler(ContextStore& store,
                          std::vector<std::string> static_required_skill_artifact_ids = {},
                          std::uint64_t max_input_tokens = 0);
    RuntimeTurnAssembler(ContextStore& store,
                         std::uint64_t max_input_tokens,
                         RuntimeContextRequirements requirements);
    ~RuntimeTurnAssembler();
    RuntimeTurnAssembler(RuntimeTurnAssembler&&) noexcept;
    RuntimeTurnAssembler& operator=(RuntimeTurnAssembler&&) noexcept;
    RuntimeTurnAssembler(const RuntimeTurnAssembler&) = delete;
    RuntimeTurnAssembler& operator=(const RuntimeTurnAssembler&) = delete;

    RuntimeTurn assemble(std::string owner_id,
                          const ContextEpoch& epoch,
                          CompletionRequest request) const;
    RuntimeTurn assemble(std::string owner_id,
                         const ContextEpoch& epoch,
                         CompletionRequest request,
                         std::vector<ChatMessage> host_instructions,
                         std::vector<ChatMessage> trusted_supplemental) const;

    /** Conservative, provider-neutral estimate: three canonical bytes per token. */
    static std::uint64_t estimate_input_tokens(const CompletionParams& params);
    /** Canonical digest of dispatch-relevant request data, excluding callback and cancellation identity. */
    static std::string normalized_request_digest(const CompletionRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph
