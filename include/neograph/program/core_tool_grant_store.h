#pragma once

#include <neograph/api.h>
#include <neograph/program/core_tool_grant.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/** Host-admitted identity, not an executable capability or a Program proposal. */
struct ProgramCoreToolGrantRecord {
    std::string owner_scope;
    std::string program_version_id;
    std::string run_id;
    std::string operation_id;
    std::uint64_t attempt = 0;
    std::string binding_fingerprint;
    std::string grant_id;
};

enum class ProgramCoreToolGrantAdmission {
    Admitted,
    AlreadyPresent,
    Conflict,
};

/**
 * Trusted hosts admit exact grants before starting an effectful Core operation.
 * Revocation is permanent for this identity. Wrong-owner reads look absent.
 * The record contains no Tool pointer, credential, policy callback or execution result.
 */
class NEOGRAPH_PROGRAM_API ProgramCoreToolGrantStore {
public:
    virtual ~ProgramCoreToolGrantStore() = default;

    virtual ProgramCoreToolGrantAdmission admit(
        const ProgramCoreToolGrantRecord& record) = 0;
    virtual std::optional<ProgramCoreToolGrantRecord> load(
        const ProgramCoreToolGrantContext& context) const = 0;
    virtual bool revoke(const ProgramCoreToolGrantContext& context) = 0;
};

/** Host-supplied, freshly rebound policy for a verified durable grant. */
using ProgramCoreToolGrantPolicyFactory = std::function<
    std::optional<ProgramCoreToolGrant>(const ProgramCoreToolGrantRecord&)>;

/**
 * Re-read host authority on every operation, including after reconnect. The
 * factory cannot change the recorded identity or executable binding. Missing,
 * revoked or mismatched records never authorize mediated Tool dispatch.
 */
NEOGRAPH_PROGRAM_API ProgramCoreToolGrantResolver make_durable_core_tool_grant_resolver(
    std::shared_ptr<ProgramCoreToolGrantStore> store,
    ProgramCoreToolGrantPolicyFactory policy_factory);

}  // namespace neograph::program
