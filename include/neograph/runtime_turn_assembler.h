#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <neograph/context_store.h>
#include <neograph/runtime_context.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

class NEOGRAPH_API ContextBudgetBlocked final : public std::runtime_error {
public:
    ContextBudgetBlocked();
};

/** A normalized request and its immutable evidence of context assembly. */
struct NEOGRAPH_API RuntimeTurn {
    PreparedProviderRequest request;
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

    RuntimeTurn assemble(Provider& provider, std::string owner_id,
                          const ContextEpoch& epoch,
                          ProviderRequest request) const;
    RuntimeTurn assemble(Provider& provider, std::string owner_id,
                         const ContextEpoch& epoch,
                         ProviderRequest request,
                         std::vector<sp::Message> host_instructions,
                         std::vector<sp::Message> trusted_supplemental) const;

    /**
     * Provider-neutral heuristic: the encoded request body's bytes, three per
     * token. It counts the whole encoded request (messages, tool schemas and
     * provider framing), not only the visible text, and it is not a proven
     * upper bound of the tokens a provider counts or bills.
     */
    static std::uint64_t estimate_input_tokens(const PreparedProviderRequest& request);
    /** Canonical digest of dispatch-relevant request data, excluding callback and cancellation identity. */
    static std::string normalized_request_digest(const PreparedProviderRequest& request);
    /**
     * Digest of the `tools` member of an encoded request body, which every family names `tools`. A body
     * without that member has no tools and gives no digest. A body that is not a JSON object cannot be
     * read, and that is an error and not the absence of tools: std::invalid_argument.
     */
    static std::optional<std::string> tools_digest(std::string_view encoded_body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph
