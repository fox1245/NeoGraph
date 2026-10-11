/**
 * @file pricing.h
 * @brief Internal: a versioned price catalog that turns provider usage into
 *        monetary microunits for Program budgets.
 *
 * A Program run may carry a `monetary_microunits` ceiling. When an `llm_call`
 * is bound to a provider, the host needs two numbers per call:
 *
 *  - a reservation before dispatch: an upper bound on what the call can cost,
 *    computed from the catalog and the call's token bounds;
 *  - a settlement after the outcome: the provider's own reported cost when
 *    the response carries one (OpenRouter `usage.cost`, parsed by the SDK into
 *    `sp::Usage::provider_cost`), otherwise an estimate from the catalog.
 *
 * Internal header: it lives under src/ and is not installed. The symbols are
 * exported only so that other NeoGraph libraries (and the Python binding) can
 * link them in shared builds; they carry no compatibility promise. The
 * provider binding that applies them to `llm_call` nodes is in monetary.h.
 *
 * Units. One microunit is 1e-6 USD; the catalog currency is USD and nothing
 * here converts currencies. Catalog rates are decimal strings in USD per
 * million tokens with at most six fractional digits, held exactly as integer
 * micro-USD per million tokens. No floating point is used.
 *
 * Rounding. Every amount rounds up to a whole microunit, once per call: an
 * estimate sums the exact products of all bands and divides once; a reported
 * nano-USD amount divides by 1000. Rounding up per call means the sum of the
 * settled amounts never under-counts the exact charges and over-counts them by
 * less than one microunit per call. A reservation rounds the same way, so for
 * usage within the reserved bounds an estimate never exceeds its reservation.
 *
 * Keys. Entries are keyed by (origin, model). The same model reached through
 * different origins (Gemini through AI Studio and through Vertex, a model
 * served directly and through OpenRouter) has separate entries, because the
 * prices, cache rates and limits differ. Aliases (for example a dateless model
 * id and its dated snapshot) resolve to the same entry within one origin.
 *
 * Unknown models. find() returns null and reserve()/settle() return
 * PricingError::UnknownModel. The catalog never guesses a price. The caller
 * chooses the policy: a run with a monetary ceiling must fail closed (refuse
 * the call); running the call unbudgeted is a host decision that the record
 * must state. The `llm_call` binding refuses on a run with a ceiling and
 * records the call as unpriced on a run without one.
 *
 * Aggregators. An origin marked "aggregator" (OpenRouter) routes one model to
 * several upstream endpoints whose prices differ, so the listed price is not a
 * bound: when the catalog was written it was the cheapest endpoint's price,
 * and the dearest endpoint charged about 24 times more per input token.
 * Reservations there use the entry's route ceiling, the highest per-token
 * price over the model's endpoints when the catalog was written. An
 * aggregator entry without a route ceiling (a router alias such as
 * `~vendor/model-latest`, whose endpoints are not listed) has no upper bound:
 * reserve() returns PricingError::NoUpperBound. The binding is expected to pin
 * the ceiling on the request (`provider.max_price`) so that the router cannot
 * pick a dearer endpoint.
 *
 * Not modeled: service tiers other than standard (batch, flex, priority),
 * Anthropic's US-only inference_geo multiplier and fast mode, server-tool fees
 * such as web search, Gemini's toolUsePromptTokenCount and context-cache
 * storage, and per-request fees. A call that uses any of them can cost more
 * than its reservation; the binding has to refuse such requests on a budgeted
 * run or price them separately.
 *
 * Organization cost reports (OpenAI organization costs, Anthropic cost
 * report) need admin keys, aggregate by time bucket and arrive late, so they
 * are not used here. A host can reconcile the per-call reported/estimated
 * records against them afterwards.
 */
#pragma once

#include <neograph/api.h>

#include <core/value.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neograph::program::pricing {

/** Rates in micro-USD per million tokens. Absent optional rates fall back as documented. */
struct Rates {
    std::uint64_t input  = 0;
    std::uint64_t output = 0;
    /// Cache reads. Absent: cache reads bill at the input rate.
    std::optional<std::uint64_t> cache_read;
    /// Cache writes (Anthropic: the 5-minute TTL). Absent: they bill at the input rate.
    std::optional<std::uint64_t> cache_write;
    /// Anthropic 1-hour TTL cache writes. Absent: they bill at the cache-write rate.
    std::optional<std::uint64_t> cache_write_1h;
    /// Image output tokens (Gemini image models). Absent: they bill at the output rate.
    std::optional<std::uint64_t> output_image;

    std::uint64_t effective_cache_read() const noexcept;
    std::uint64_t effective_cache_write() const noexcept;
    std::uint64_t effective_cache_write_1h() const noexcept;
    std::uint64_t effective_output_image() const noexcept;
    /// Highest rate any input token can bill at.
    std::uint64_t max_input() const noexcept;
    /// Highest rate any output token can bill at.
    std::uint64_t max_output() const noexcept;
};

/**
 * One price tier. A request whose total input tokens (cache reads and writes
 * included) are at most `up_to_input_tokens` uses this tier for all of its
 * tokens; the last tier is unbounded. Rates never decrease from one tier to
 * the next (checked when loading), so a bound computed at the largest input
 * also bounds every smaller input.
 */
struct Tier {
    std::optional<std::uint64_t> up_to_input_tokens;
    Rates                        rates;
};

/** Highest per-token price over an aggregator's endpoints for one model. */
struct RouteCeiling {
    Rates       rates;
    std::string source_url;
    std::string retrieved;
};

/** Limits stated by the source, when it states exact numbers. Informational. */
struct Limits {
    std::optional<std::uint64_t> context_window;
    std::optional<std::uint64_t> max_input_tokens;
    std::optional<std::uint64_t> max_output_tokens;
};

/** Where requests go: an https origin (scheme and host), as the SDK descriptor policy names it. */
struct Origin {
    std::string id;
    std::string url;
    bool        aggregator = false;
};

struct Entry {
    std::string                 origin;
    std::string                 model;
    std::vector<std::string>    aliases;
    std::vector<Tier>           tiers;
    std::optional<RouteCeiling> route_ceiling;
    Limits                      limits;
    std::string                 source_url;
    std::string                 retrieved;

    /// The tier that prices a request with this many input tokens.
    const Tier& tier_for(std::uint64_t input_tokens) const noexcept;
};

/** Where a settled amount came from. */
enum class Evidence {
    Reported,   ///< The provider's own cost for this call.
    Estimated,  ///< Computed from the catalog; carries the catalog version and digest.
};

/** Which rates produced an amount. */
enum class Basis {
    ProviderReported,  ///< Settlement only: the reported cost, no catalog rates.
    ListRates,         ///< The entry's tier rates.
    RouteCeiling,      ///< The entry's route ceiling (aggregator origins).
};

enum class PricingError {
    UnknownModel,       ///< No entry for (origin, model). The caller decides; see the file comment.
    NoUpperBound,       ///< Aggregator entry without a route ceiling: no reservation is possible.
    IncompleteUsage,    ///< Usage is not final, or total input/output tokens are unknown.
    InconsistentUsage,  ///< Usage counters contradict each other or are marked inconsistent.
    Overflow,           ///< The amount does not fit 64-bit microunits.
};

NEOGRAPH_PROGRAM_API std::string_view to_string(PricingError error) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(Evidence evidence) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(Basis basis) noexcept;

struct Reservation {
    std::uint64_t microunits = 0;
    Basis         basis      = Basis::ListRates;
    std::string   catalog_version;
    std::string   catalog_digest;
};

struct Settlement {
    std::uint64_t microunits = 0;
    Evidence      evidence   = Evidence::Estimated;
    Basis         basis      = Basis::ListRates;
    /// Set for estimates; empty for reported costs.
    std::string catalog_version;
    std::string catalog_digest;
    /// Some tokens were in an unreported band (for example the cache-write
    /// TTL split, or image versus text output) and were priced at the highest
    /// rate they could have billed at. The amount may over-count, never under.
    bool conservative = false;
    /// An aggregator estimate at the list price because the entry has no
    /// route ceiling. The served endpoint may have cost more.
    bool may_undercount = false;
};

template <class T>
using PricingResult = std::variant<T, PricingError>;

/** A catalog that failed to load. The message names the offending field. */
class NEOGRAPH_PROGRAM_API CatalogError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/**
 * Parse a decimal USD-per-million-tokens string ("0.10", "30", "0.00686")
 * into exact micro-USD per million tokens. Rejects signs, exponents, leading
 * zeros, more than six fractional digits and values above 10000 USD.
 */
NEOGRAPH_PROGRAM_API std::optional<std::uint64_t> parse_rate(std::string_view decimal) noexcept;

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

/** The loaded, validated catalog. Immutable; safe to share across threads. */
class NEOGRAPH_PROGRAM_API PriceCatalog {
public:
    /** Parse and validate catalog JSON. Throws CatalogError. */
    static PriceCatalog parse(std::string_view json_text);
    /** Read and parse a catalog file. Throws CatalogError. */
    static PriceCatalog load(const std::filesystem::path& path);
    /**
     * The catalog compiled into this library from data/pricing/catalog.json
     * at build time. Parsed once; the runtime prices calls only from it.
     */
    static const PriceCatalog& builtin();

    const std::string&         version() const noexcept { return version_; }
    /** SHA-256 identity of the canonical catalog JSON, recorded with estimates. */
    const std::string&         digest() const noexcept { return digest_; }
    const std::vector<Origin>& origins() const noexcept { return origins_; }
    const std::vector<Entry>&  entries() const noexcept { return entries_; }

    const Origin* find_origin(std::string_view id) const noexcept;
    /** The origin whose scheme and host match this URL's (case-insensitive; the path is ignored). */
    const Origin* find_origin_by_url(std::string_view url) const noexcept;
    /** Entry for a model id or alias within one origin, or null. */
    const Entry*  find(std::string_view origin, std::string_view model) const noexcept;

    /**
     * Upper bound for one call with at most `input_tokens_bound` input tokens
     * (all cache bands included) and `output_tokens_bound` output tokens
     * (reasoning included). Every input token is priced at the highest input
     * band rate and every output token at the highest output rate of the tier
     * selected by the input bound, or of the route ceiling on an aggregator.
     */
    PricingResult<Reservation> reserve(std::string_view origin,
                                       std::string_view model,
                                       std::uint64_t    input_tokens_bound,
                                       std::uint64_t    output_tokens_bound) const;

    /**
     * Post-hoc amount for one call. Requires final usage. Uses the reported
     * cost when there is one (it needs no catalog entry); otherwise estimates
     * from the catalog, as estimate() does.
     */
    PricingResult<Settlement> settle(std::string_view origin,
                                     std::string_view model,
                                     const sp::Usage& usage) const;

    /**
     * Catalog estimate from final usage, ignoring any reported cost. Input
     * bands: uncached at the input rate, cache reads at the cache-read rate,
     * cache writes at the write rate (Anthropic's 5m/1h split read from
     * `Usage::extra` when present). Tokens of the input total that no reported
     * band accounts for are priced at the highest rate among the unreported
     * bands. Output tokens bill at the output rate; Gemini image tokens
     * (`candidatesTokensDetails.IMAGE` in `Usage::extra`) at the image rate.
     * Aggregator entries use the route ceiling when they have one, since the
     * served endpoint is unknown; without one they use the list price, which
     * may under-count (Basis::ListRates on an aggregator origin says so).
     */
    PricingResult<Settlement> estimate(std::string_view origin,
                                       std::string_view model,
                                       const sp::Usage& usage) const;

private:
    PriceCatalog() = default;

    std::string         version_;
    std::string         digest_;
    std::vector<Origin> origins_;
    std::vector<Entry>  entries_;
};

}  // namespace neograph::program::pricing
