/**
 * @file monetary.h
 * @brief Internal: prices Program provider calls against a run's monetary budget.
 *
 * A Program run's RunBudget::monetary_microunits is its monetary ceiling in
 * micro-USD (1e-6 USD). Like every RunBudget field it is an amount granted to
 * the run, and 0 grants nothing: child and fork budgets are carved out of
 * their parent's amounts, and a run granted model_tokens = 0 sends no model
 * call (it ends BudgetExhausted). Nothing charged money
 * before the provider binding (#372); the binding keeps that meaning, so a run
 * with monetary_microunits = 0 refuses every priced call before it is sent.
 *
 * Both pricing brokers, bind_priced_provider_call() and
 * SQLiteProgramProviderCallJournal::bind_priced(), treat one call the same way:
 *
 *  1. Assess. Prices come only from the SchemaProvider model catalogue of the
 *     provider's admitted descriptor policy: the embedded catalogue and the
 *     layers the host merged over it (sp::descriptor::merge_catalogues,
 *     DescriptorPolicy::with_catalogue). The hold is the SDK's worst case of
 *     the prepared request over that catalogue (Provider::worst_case_cost,
 *     sp::runtime::worst_case_cost): every model invocation and transport
 *     attempt the request may bill, the service tier it bills under (its
 *     priced extensions, such as Messages' speed, included), its fallback
 *     models, its server tools at their use caps, what it carries (an image)
 *     and, on an OpenRouter origin, the route ceiling, rounded up once to
 *     whole microunits. Every reason the SDK gives for no bound refuses the
 *     call with a P_MONEY_* code. A request that may carry image input
 *     (RequestFacts::image_input) is held at the dearest input rate of its
 *     entry, input_image included. On an
 *     OpenRouter origin the request is pinned before it is prepared to be
 *     sent: its provider.max_price becomes the route ceiling, or the host's
 *     own cap where that is lower, so the request that is digested, journaled
 *     and sent is the pinned one (prepare_priced_call()).
 *  2. Preflight. A call that cannot be bounded is refused before any request
 *     is sent, with a P_MONEY_* code; so is a call whose hold does not fit the
 *     remaining budget (P_MONEY_BUDGET).
 *  3. Settle exactly once from the outcome: the provider's reported cost when
 *     the response carries one, else the catalogue estimate
 *     (sp::pricing::estimate) from the snapshot the hold was priced with,
 *     under the tier the hold was bounded at (the request facts' billed tier,
 *     which the record keeps as billed_tier), plus the reserved server-tool
 *     fees. A usage that does not count its image tokens, of a request that
 *     may have carried an image, bills its input at the dearer of input and
 *     input_image (the assumption "input at input_image"). An unknown outcome, resent transport
 *     attempts, or partial or inconsistent usage keep the hold as the charge.
 *     A failure that proves the provider accepted nothing (no request was
 *     sent, or a completed HTTP 4xx answer declined it) releases it.
 *
 * Units. One microunit is 1e-6 USD. Catalogue amounts are exact nano-USD and
 * round up once to whole microunits, so for usage within the reserved bounds a
 * settlement never exceeds its hold.
 *
 * Exported for the SQLite journal library and the Python binding only; no
 * compatibility promise.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/provider_call_broker.h>
#include <neograph/json.h>
#include <neograph/program/core_provider_call.h>
#include <neograph/program/result.h>
#include <neograph/provider.h>

#include <core/pricing.h>
#include <descriptor/catalogue.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program {

namespace detail {

/** A call refused before dispatch: it cannot be bounded, or its hold does not fit. */
struct MonetaryRefusal {
    std::string code;
    std::string message;
    json        witness = json::object();
    /// The remaining budget is too small (BudgetExhausted), rather than the
    /// call being impossible to bound.
    bool        budget = false;
};

}  // namespace detail

class NEOGRAPH_PROGRAM_API ProgramMonetaryAccount final {
public:
    /// `ceiling` is the attempt's granted monetary_microunits; 0 grants nothing.
    ProgramMonetaryAccount(std::uint64_t ceiling, std::uint64_t attempt);

    std::uint64_t ceiling() const {
        std::lock_guard lock(mutex_);
        return ceiling_;
    }
    /// Sets the amount the attempt may still charge in all: the grant less what a running command leaves to the
    /// parent's children. A hold already placed is not recalled; the next one is held against the new ceiling.
    void set_ceiling(std::uint64_t ceiling) {
        std::lock_guard lock(mutex_);
        ceiling_ = ceiling;
    }
    std::uint64_t attempt() const noexcept { return attempt_; }

    /// Hold `amount` for a call about to be dispatched; fails when charged +
    /// held + amount would exceed the ceiling.
    bool try_reserve(std::uint64_t amount);
    /// Return the hold of a call the provider did not accept.
    void release(std::uint64_t amount) noexcept;
    /// Settle one dispatched call: drop its hold and charge the record's amount.
    void settle(std::uint64_t held, ProgramMonetaryRecord record);
    /// Mark a journaled call as accounted; false when it already was.
    bool remember(std::string key);
    /// Charge a call that this attempt journaled before a restart, once.
    bool restore(std::string key, ProgramMonetaryRecord record);

    /// Charged plus held micro-USD.
    std::uint64_t used() const;
    /// Charged and held amounts exceed the ceiling (a reported cost above its hold).
    bool exceeded() const;
    std::vector<ProgramMonetaryRecord> records() const;

    void note_refusal(sp::runtime::Result outcome, detail::MonetaryRefusal refusal);
    /// The refusal that produced this exact outcome, if this account refused it.
    std::optional<detail::MonetaryRefusal> refusal_for(const sp::runtime::Result& outcome) const;

private:
    mutable std::mutex                 mutex_;
    std::uint64_t                      ceiling_;
    const std::uint64_t                attempt_;
    std::uint64_t                      charged_  = 0;
    std::uint64_t                      reserved_ = 0;
    std::set<std::string>              accounted_;
    std::vector<ProgramMonetaryRecord> records_;
    std::vector<std::pair<sp::runtime::Result, detail::MonetaryRefusal>> refusals_;
};

namespace detail {

/** Where a priced call ran. */
struct MonetaryCallSite {
    std::string call_id;
    std::string operation_id;
    std::string node;
};

/** The scheme of catalog_version for amounts priced from a SchemaProvider model catalogue. */
inline constexpr std::string_view kCatalogueScheme = "sp-model-catalogue/1";

/**
 * What a typed request needs before it is priced. Everything that decides the
 * amount (service tier, priced extensions, fallback models, server tools and
 * their caps, routing, image input) is read by the SDK from the prepared
 * request (Provider::request_facts()).
 */
struct MonetaryRequestFacts {
    /// A call the hold does not price at all yet (a SchemaProvider endpoint call).
    std::optional<std::string> unpriced;
};

/** The request facts that decide a call's price, read from the typed request. */
NEOGRAPH_PROGRAM_API MonetaryRequestFacts monetary_request_facts(const ProviderRequest& request);

/** What the catalogue says about one prepared call. */
struct MonetaryQuote {
    /// The provider's base URL, the origin the catalogue entry matched at.
    std::string   origin;
    std::string   model;
    /// Reservation for one dispatch: the SDK's worst case of the prepared
    /// request (its model invocations and attempts included), rounded up once
    /// to whole microunits. 0 when it is refused (`blocker` is then set).
    std::uint64_t hold = 0;
    std::string   hold_basis;
    std::string   catalog_version;
    std::string   catalog_digest;
    /// Why the call cannot be bounded (a P_MONEY_* code); empty when it can.
    std::string   blocker;
    std::string   blocker_message;
    /// OpenRouter's provider.max_price for a routed request: the route
    /// ceiling, lowered field by field to any cap the host already sends.
    std::optional<sp::OpenRouterMaxPrice> pin;
    /// The prepared request already carries `pin` as its provider.max_price.
    bool          pinned = false;
    json          witness = json::object();
    /// What settles the call: the catalogue snapshot the hold was priced
    /// from, the family, the service tier ("" for the standard tier), whether
    /// the served endpoint is unknown (a routing gateway), and the server-tool
    /// fees the hold reserved, in nano-USD.
    sp::descriptor::CatalogueSnapshot catalogue;
    std::string                       family;
    std::string                       service_tier;
    bool                              route_ceiling = false;
    std::uint64_t                     tool_fees_nano_usd = 0;
    /// The facts of the request that is sent (Provider::request_facts()). Settlement prices the usage with them
    /// (sp::pricing::estimate), so it bills under the tier the hold was bounded at: the service tier together with
    /// the tiers the request's priced extensions name.
    sp::RequestFacts                  request_facts;
    /// That tier (sp::pricing::billed_tier()); "" for the standard rates. Every record of the call keeps it
    /// (ProgramMonetaryRecord::billed_tier).
    std::string                       billed_tier;
    /// The entry and price rows the hold was priced with (ProgramMonetaryRecord::pricing).
    json pricing = json::object();
};

/**
 * Assess one prepared call. `facts` is monetary_request_facts() of the typed
 * request it was prepared from.
 */
NEOGRAPH_PROGRAM_API MonetaryQuote assess_monetary_call(const MonetaryRequestFacts&    facts,
                                                        const PreparedProviderRequest& prepared);

/**
 * Pin provider.max_price on an OpenRouter request (Chat Completions, Responses
 * or Messages) so the router cannot pick an endpoint dearer than the ceiling
 * the call was reserved at. `price` is MonetaryQuote::pin, which already keeps
 * the lower of the host's own cap and the ceiling. Returns true when the
 * request changed. Throws std::logic_error for a family that cannot carry
 * OpenRouter routing; assess_monetary_call() never quotes a pin for one.
 */
NEOGRAPH_PROGRAM_API bool pin_route_max_price(ProviderRequest&               request,
                                              const sp::OpenRouterMaxPrice& price);

/** A call prepared by a pricing broker, with its quote when it is priced. */
struct PricedPreparation {
    PreparedProviderRequest      prepared;
    std::optional<MonetaryQuote> quote;
};

/**
 * Prepare one call the way both pricing brokers do. Unpriced, `request` is
 * prepared as it is. Priced, it is assessed first and, on an aggregator,
 * pinned before it is prepared to be sent, once. Pinning first matters:
 * captured native history binds the routing its turn was sent with, the pin
 * included, so a later turn prepared without the pin would be refused as
 * ReplayIneligible.
 *
 * The pin comes from a probe of the request whose conversation is one user
 * message; the probe is released unsent. The hold is then the bound of the
 * request that is sent, whose images and continuation the bound reads, and it
 * is refused when that request does not carry the pin. A request whose probe
 * the provider does not admit is priced from its own preparation and prepared
 * again when that has to be pinned.
 *
 * `prepared` may be invalid: the broker returns preparation_failure_outcome().
 * `quote` is empty when the call is not priced or could not be prepared.
 */
NEOGRAPH_PROGRAM_API PricedPreparation prepare_priced_call(Provider&       provider,
                                                           ProviderRequest request,
                                                           bool            priced);

/**
 * The not-sent failure a pricing broker returns for a preparation that is not
 * valid: the preparation's own error, else InvalidRequest.
 */
NEOGRAPH_PROGRAM_API sp::runtime::Result preparation_failure_outcome(
    const PreparedProviderRequest& prepared);

/** The synthetic not-sent failure a refused call returns instead of dispatching. */
NEOGRAPH_PROGRAM_API sp::runtime::Result monetary_refusal_outcome(const MonetaryRefusal& refusal);

/**
 * The record for one dispatched call's outcome; empty when the outcome proves
 * the provider accepted nothing (provider_failure_proves_not_accepted), so the
 * hold is released and nothing is charged.
 */
NEOGRAPH_PROGRAM_API std::optional<ProgramMonetaryRecord> settle_monetary_call(
    const MonetaryQuote&       quote,
    const MonetaryCallSite&    site,
    const sp::runtime::Result& outcome);

/**
 * The provider-reported cost of one call in microunits, if the SDK admitted
 * one: an OpenRouter USD amount with consistent quality and an available
 * total. With BYOK the upstream provider bills the caller's own key, so the
 * call cost is the OpenRouter total plus the reported upstream total; a BYOK
 * response without an upstream amount has no reported cost. When the BYOK
 * flag itself is missing and an upstream amount is present, both are added
 * (it may double count; it cannot under-count).
 */
NEOGRAPH_PROGRAM_API std::optional<std::uint64_t> reported_cost_microunits(
    const sp::ProviderReportedCost& cost) noexcept;

/** The record for a dispatched call whose outcome is unknown: its hold is charged. */
NEOGRAPH_PROGRAM_API ProgramMonetaryRecord unknown_outcome_record(
    const MonetaryQuote& quote, const MonetaryCallSite& site, std::string reason);

NEOGRAPH_PROGRAM_API json encode_monetary_record(const ProgramMonetaryRecord& record);
NEOGRAPH_PROGRAM_API ProgramMonetaryRecord decode_monetary_record(const json& value);
NEOGRAPH_PROGRAM_API void validate_monetary_record(const ProgramMonetaryRecord& record);

/**
 * Stable logical slot of one Core provider call; the provider journal keys its
 * rows by it and monetary records name calls by it.
 */
NEOGRAPH_PROGRAM_API std::string provider_call_slot_id(
    const ProgramCoreProviderCallContext& context, const graph::ProviderCallIdentity& call);

/// The code of the notice for a call charged more than its reservation (#428).
inline constexpr std::string_view kOverHoldNotice = "P_MONEY_OVER_HOLD";

/**
 * The notice for a settled call that was charged more than its reservation:
 * a `reported` or `estimated` record whose amount is above its
 * reserved_microunits. A `held` record is charged its hold and is never above
 * it; neither is a record that is within its reservation. The witness names
 * the call and its amounts, and the record's served_by when it has one; it
 * holds nothing else of its request or reply.
 */
NEOGRAPH_PROGRAM_API std::optional<ProgramCoreProviderCallDiagnostic> over_hold_notice(
    const ProgramMonetaryRecord& record);

/**
 * The hold of one dispatch on a monetary account: reserved at preflight and
 * then either released (not sent) or settled exactly once. When it is settled
 * with a record charged above the reservation, the hold reports
 * over_hold_notice() to `diagnostic` after the charge is made; a sink that
 * throws changes nothing.
 */
class NEOGRAPH_PROGRAM_API MonetaryHold final {
public:
    using DiagnosticSink = std::function<void(ProgramCoreProviderCallDiagnostic)>;

    MonetaryHold() = default;
    MonetaryHold(std::shared_ptr<ProgramMonetaryAccount> account, MonetaryQuote quote,
                 MonetaryCallSite site, DiagnosticSink diagnostic = {});
    MonetaryHold(MonetaryHold&& other) noexcept;
    MonetaryHold& operator=(MonetaryHold&& other) noexcept;
    MonetaryHold(const MonetaryHold&)            = delete;
    MonetaryHold& operator=(const MonetaryHold&) = delete;
    /// An undispatched hold is released; a dispatched, unsettled one is charged.
    ~MonetaryHold();

    bool active() const noexcept { return account_ != nullptr; }
    const MonetaryQuote& quote() const noexcept { return quote_; }
    std::uint64_t reserved() const noexcept { return reserved_; }

    /**
     * Reserve the hold. A call that cannot be bounded or does not fit is
     * refused: the result is the outcome to return instead of dispatching.
     * Null when the call may be dispatched.
     */
    sp::runtime::Result preflight();
    void mark_dispatched() noexcept { dispatched_ = true; }
    /// The record settle() will charge for this outcome.
    std::optional<ProgramMonetaryRecord> record_for(const sp::runtime::Result& outcome) const;
    /// Settle from a known outcome (or release when it proves nothing was accepted).
    void settle(const sp::runtime::Result& outcome);
    /// Settle with a precomputed record (empty: release).
    void settle_record(std::optional<ProgramMonetaryRecord> record);
    /// The outcome is unknown: charge the hold.
    void settle_unknown(std::string reason) noexcept;
    void release() noexcept;
    /// Mark the call's journal key as accounted on the account.
    void remember(std::string key);

private:
    void finish() noexcept;

    std::shared_ptr<ProgramMonetaryAccount> account_;
    MonetaryQuote                           quote_;
    MonetaryCallSite                        site_;
    DiagnosticSink                          diagnostic_;
    std::uint64_t                           reserved_   = 0;
    bool                                    dispatched_ = false;
    bool                                    done_       = true;
};

}  // namespace detail
}  // namespace neograph::program
