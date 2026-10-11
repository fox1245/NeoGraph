#pragma once

#include <neograph/api.h>
#include <neograph/graph/provider_call_broker.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Monetary account of one Program attempt: the attempt's monetary_microunits
 * grant (0 grants nothing), the reservations of calls in flight, settled
 * charges and one ProgramMonetaryRecord per priced call. Opaque to hosts;
 * pricing brokers read it from the call context.
 */
class ProgramMonetaryAccount;

/** Exact Program operation requesting a host-owned Core provider broker. */
struct ProgramCoreProviderCallContext {
    std::string_view owner_scope;
    std::string_view program_version_id;
    std::string_view run_id;
    std::string_view operation_id;
    std::uint64_t attempt = 0;
    /// The attempt's monetary account. Null for recorded replay, which never
    /// spends. Brokers that price calls charge it; others may ignore it.
    std::shared_ptr<ProgramMonetaryAccount> monetary_account;
    /// Set only when the runtime settles an attempt that never runs again (a
    /// run its expired execution lease ends). Brokers that journal calls
    /// charge it, when they are bound, the tokens of the calls this attempt
    /// journaled in the operation, as they charge its monetary account.
    std::shared_ptr<UsageAccumulator> journaled_usage;
};

/** Host binding for one Program operation. No credential is stored here. */
struct ProgramCoreProviderCallBinding {
    std::string owner_scope;
    std::string program_version_id;
    std::string run_id;
    std::string operation_id;
    std::uint64_t attempt = 0;
    std::shared_ptr<graph::ProviderCallBroker> broker;
};

using ProgramCoreProviderCallResolver = std::function<
    std::optional<ProgramCoreProviderCallBinding>(
        const ProgramCoreProviderCallContext&)>;

/**
 * In-process broker for built-in Core provider calls that applies the
 * attempt's token budget and its monetary account: each call is priced from
 * the compiled-in price catalog, its upper bound is reserved before dispatch
 * (a call that cannot be bounded, or does not fit, is refused before it is
 * sent), and it is settled once from its outcome. It keeps no durable
 * journal; SQLiteProgramProviderCallJournal::bind_priced adds crash-safe replay.
 */
NEOGRAPH_PROGRAM_API ProgramCoreProviderCallBinding bind_priced_provider_call(
    const ProgramCoreProviderCallContext& context);

}  // namespace neograph::program
