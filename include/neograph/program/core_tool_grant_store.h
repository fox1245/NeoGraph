#pragma once

#include <neograph/api.h>
#include <neograph/program/store_call.h>
#include <neograph/program/core_tool_grant.h>

#include <asio/awaitable.hpp>

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
 *
 * Every operation is awaitable (#556); a store whose backend is synchronous completes it at once
 * (SyncProgramCoreToolGrantStore). The arguments of a call, string views included, must stay alive until the awaitable
 * completes: await it in the full expression that makes the call.
 */
class NEOGRAPH_PROGRAM_API ProgramCoreToolGrantStore {
public:
    virtual ~ProgramCoreToolGrantStore() = default;

    [[nodiscard]] virtual asio::awaitable<ProgramCoreToolGrantAdmission> admit(
        const ProgramCoreToolGrantRecord& record) = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(
        const ProgramCoreToolGrantContext& context) const = 0;
    [[nodiscard]] virtual asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext& context) = 0;
};

/**
 * Base of a ProgramCoreToolGrantStore whose backend is synchronous. A derived class implements the `..._sync`
 * operations and gets the awaitable interface from them, as SyncProgramTransitionStore does.
 */
class NEOGRAPH_PROGRAM_API SyncProgramCoreToolGrantStore : public ProgramCoreToolGrantStore {
public:
    // The synchronous operations. Each has the contract of the awaitable operation of the same name.
    virtual ProgramCoreToolGrantAdmission admit_sync(const ProgramCoreToolGrantRecord& record) = 0;
    virtual std::optional<ProgramCoreToolGrantRecord> load_sync(
        const ProgramCoreToolGrantContext& context) const = 0;
    virtual bool revoke_sync(const ProgramCoreToolGrantContext& context) = 0;

    // The awaitable interface, completed by the operations above: at once on the calling thread,
    // or on a thread of a bounded pool when offloads() is true.
    [[nodiscard]] asio::awaitable<ProgramCoreToolGrantAdmission> admit(
        const ProgramCoreToolGrantRecord& record) final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(
        const ProgramCoreToolGrantContext& context) const final;
    [[nodiscard]] asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext& context) final;

protected:
    /** True for a backend that blocks for long: each operation then runs on a pool thread. */
    virtual bool offloads() const noexcept { return false; }
};

/**
 * A ProgramCoreToolGrantStore for a caller that cannot suspend. It waits for a store that is not synchronous as
 * BlockingProgramTransitionStore does.
 */
class NEOGRAPH_PROGRAM_API BlockingProgramCoreToolGrantStore {
public:
    BlockingProgramCoreToolGrantStore(std::shared_ptr<ProgramCoreToolGrantStore> store, BlockingStorePolicy policy = {});
    /** A view of a store that outlives it. */
    BlockingProgramCoreToolGrantStore(ProgramCoreToolGrantStore& store, BlockingStorePolicy policy = {});

    const std::shared_ptr<ProgramCoreToolGrantStore>& store() const noexcept { return store_; }

    ProgramCoreToolGrantAdmission admit(const ProgramCoreToolGrantRecord& record) const;
    std::optional<ProgramCoreToolGrantRecord> load(const ProgramCoreToolGrantContext& context) const;
    bool revoke(const ProgramCoreToolGrantContext& context) const;

private:
    std::shared_ptr<ProgramCoreToolGrantStore> store_;
    SyncProgramCoreToolGrantStore* sync_ = nullptr;
    BlockingStorePolicy policy_;
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
