#include "pricing.h"

#include "canonical_json.h"

#include <neograph/json.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace neograph::program::pricing {
namespace {

constexpr std::uint64_t    kMillion  = 1'000'000;
constexpr std::uint64_t    kMaxRate  = 10'000 * kMillion;  // 10,000 USD per million tokens
constexpr std::uint64_t    kMax      = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::size_t      kMaxTiers = 8;
constexpr std::size_t      kMaxText  = 4096;
constexpr std::string_view kSchema   = "neograph.pricing.catalog";

// Usage::extra keys the SDK fills from vendor detail counters.
constexpr std::string_view kCacheWrite5m = "cache_creation.ephemeral_5m_input_tokens";
constexpr std::string_view kCacheWrite1h = "cache_creation.ephemeral_1h_input_tokens";
constexpr std::string_view kImageOutput  = "candidatesTokensDetails.IMAGE";

[[noreturn]] void fail(const std::string& where, const std::string& what) {
    throw CatalogError("price catalog: " + where + ": " + what);
}

json require(const json& object, const char* field, const std::string& where) {
    if (!object.contains(field)) fail(where, std::string("missing field '") + field + "'");
    return object.at(field);
}

std::string text(const json& value, const std::string& where) {
    if (!value.is_string()) fail(where, "must be a string");
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > kMaxText) fail(where, "must be a non-empty string");
    try {
        program::detail::validate_token(result, where);
    } catch (const std::invalid_argument& error) {
        fail(where, error.what());
    }
    return result;
}

std::string text_field(const json& object, const char* field, const std::string& where) {
    return text(require(object, field, where), where + "." + field);
}

std::uint64_t positive(const json& value, const std::string& where) {
    if (!value.is_number_unsigned()) fail(where, "must be a positive integer");
    const auto result = value.get<unsigned long long>();
    if (result == 0) fail(where, "must be a positive integer");
    return result;
}

std::optional<std::uint64_t> optional_positive(const json& object,
                                               const char* field,
                                               const std::string& where) {
    if (!object.contains(field)) return std::nullopt;
    const auto value = object.at(field);
    if (value.is_null()) return std::nullopt;
    return positive(value, where + "." + field);
}

std::string https_url(const json& object, const char* field, const std::string& where) {
    auto url = text_field(object, field, where);
    if (url.rfind("https://", 0) != 0 || url.size() <= 8) {
        fail(where + "." + field, "must be an https:// URL");
    }
    if (std::any_of(url.begin(), url.end(),
                    [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
        fail(where + "." + field, "must not contain spaces or control characters");
    }
    return url;
}

std::string date_field(const json& object, const char* field, const std::string& where) {
    auto date = text_field(object, field, where);
    const auto digit = [&](std::size_t i) {
        return std::isdigit(static_cast<unsigned char>(date[i])) != 0;
    };
    const bool shape = date.size() == 10 && date[4] == '-' && date[7] == '-' && digit(0) &&
                       digit(1) && digit(2) && digit(3) && digit(5) && digit(6) && digit(8) &&
                       digit(9);
    if (!shape) fail(where + "." + field, "must be a YYYY-MM-DD date");
    const int month = (date[5] - '0') * 10 + (date[6] - '0');
    const int day   = (date[8] - '0') * 10 + (date[9] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        fail(where + "." + field, "must be a YYYY-MM-DD date");
    }
    return date;
}

std::uint64_t rate(const json& value, const std::string& where) {
    if (!value.is_string()) fail(where, "must be a decimal string in USD per million tokens");
    const auto parsed = parse_rate(value.get<std::string>());
    if (!parsed) {
        fail(where, "must be a decimal string in USD per million tokens "
                    "(no sign or exponent, at most 6 fractional digits, at most 10000)");
    }
    return *parsed;
}

std::optional<std::uint64_t> optional_rate(const json& object,
                                           const char* field,
                                           const std::string& where) {
    if (!object.contains(field)) return std::nullopt;
    return rate(object.at(field), where + "." + field);
}

Rates rates_from(const json& object, const std::string& where) {
    Rates result;
    result.input          = rate(require(object, "input", where), where + ".input");
    result.output         = rate(require(object, "output", where), where + ".output");
    result.cache_read     = optional_rate(object, "cache_read", where);
    result.cache_write    = optional_rate(object, "cache_write", where);
    result.cache_write_1h = optional_rate(object, "cache_write_1h", where);
    result.output_image   = optional_rate(object, "output_image", where);
    return result;
}

// Every effective rate, in a fixed order, for monotonicity and ceiling checks.
std::array<std::uint64_t, 6> effective(const Rates& rates) noexcept {
    return {rates.input,
            rates.output,
            rates.effective_cache_read(),
            rates.effective_cache_write(),
            rates.effective_cache_write_1h(),
            rates.effective_output_image()};
}

bool dominates(const Rates& high, const Rates& low) noexcept {
    const auto a = effective(high);
    const auto b = effective(low);
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] < b[i]) return false;
    return true;
}

// "scheme://host[:port]" of a URL, lowercased; empty when the URL has no scheme or host.
std::string origin_of(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos || scheme == 0) return {};
    const auto host_end = url.find_first_of("/?#", scheme + 3);
    std::string result(url.substr(0, host_end));
    if (result.size() == scheme + 3) return {};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool origin_id_ok(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

/**
 * Exact running sum of tokens * rate / 1e6 microunits. `whole_` holds whole
 * microunits and `fraction_` the remainder in millionths of a microunit, so
 * ceil() is exactly the ceiling of the true sum. Every product is split by
 * the token count's quotient and remainder by one million, so no
 * intermediate can overflow before the checked whole-part addition does.
 */
class Amount {
public:
    bool add(std::uint64_t tokens, std::uint64_t rate_per_million) noexcept {
        const auto quotient  = tokens / kMillion;
        const auto remainder = tokens % kMillion;
        if (rate_per_million != 0 && quotient > kMax / rate_per_million) return overflow();
        if (!add_whole(quotient * rate_per_million)) return false;
        // remainder < 1e6 and rate <= 1e10, so the product stays below 1e16.
        fraction_ += remainder * rate_per_million;
        if (!add_whole(fraction_ / kMillion)) return false;
        fraction_ %= kMillion;
        return true;
    }

    std::optional<std::uint64_t> ceil() const noexcept {
        if (overflow_) return std::nullopt;
        if (fraction_ == 0) return whole_;
        if (whole_ == kMax) return std::nullopt;
        return whole_ + 1;
    }

private:
    bool add_whole(std::uint64_t value) noexcept {
        if (value > kMax - whole_) return overflow();
        whole_ += value;
        return true;
    }
    bool overflow() noexcept {
        overflow_ = true;
        return false;
    }

    std::uint64_t whole_    = 0;
    std::uint64_t fraction_ = 0;
    bool          overflow_ = false;
};

std::optional<std::uint64_t> extra_count(const sp::Usage& usage, std::string_view key) {
    const auto found = usage.extra.find(std::string(key));
    if (found == usage.extra.end()) return std::nullopt;
    return found->second.value;
}

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > kMax - total) return false;
    total += value;
    return true;
}

}  // namespace

std::uint64_t Rates::effective_cache_read() const noexcept { return cache_read.value_or(input); }
std::uint64_t Rates::effective_cache_write() const noexcept { return cache_write.value_or(input); }
std::uint64_t Rates::effective_cache_write_1h() const noexcept {
    return cache_write_1h.value_or(effective_cache_write());
}
std::uint64_t Rates::effective_output_image() const noexcept { return output_image.value_or(output); }
std::uint64_t Rates::max_input() const noexcept {
    return (std::max)({input, effective_cache_read(), effective_cache_write(),
                       effective_cache_write_1h()});
}
std::uint64_t Rates::max_output() const noexcept {
    return (std::max)(output, effective_output_image());
}

const Tier& Entry::tier_for(std::uint64_t input_tokens) const noexcept {
    for (const auto& tier : tiers)
        if (!tier.up_to_input_tokens || input_tokens <= *tier.up_to_input_tokens) return tier;
    return tiers.back();
}

std::string_view to_string(PricingError error) noexcept {
    switch (error) {
    case PricingError::UnknownModel: return "unknown_model";
    case PricingError::NoUpperBound: return "no_upper_bound";
    case PricingError::IncompleteUsage: return "incomplete_usage";
    case PricingError::InconsistentUsage: return "inconsistent_usage";
    case PricingError::Overflow: return "overflow";
    }
    return "unknown";
}

std::string_view to_string(Evidence evidence) noexcept {
    return evidence == Evidence::Reported ? "reported" : "estimated";
}

std::string_view to_string(Basis basis) noexcept {
    switch (basis) {
    case Basis::ProviderReported: return "provider_reported";
    case Basis::ListRates: return "list_rates";
    case Basis::RouteCeiling: return "route_ceiling";
    }
    return "unknown";
}

std::optional<std::uint64_t> parse_rate(std::string_view decimal) noexcept {
    if (decimal.empty() || decimal.size() > 24) return std::nullopt;
    const auto dot      = decimal.find('.');
    const auto integral = decimal.substr(0, dot);
    const auto fraction = dot == std::string_view::npos ? std::string_view{} : decimal.substr(dot + 1);
    const auto digits   = [](std::string_view part) {
        return std::all_of(part.begin(), part.end(),
                           [](char c) { return c >= '0' && c <= '9'; });
    };
    if (integral.empty() || integral.size() > 5 || !digits(integral)) return std::nullopt;
    if (integral.size() > 1 && integral.front() == '0') return std::nullopt;
    if (dot != std::string_view::npos && (fraction.empty() || fraction.size() > 6))
        return std::nullopt;
    if (!digits(fraction)) return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : integral) value = value * 10 + static_cast<std::uint64_t>(c - '0');
    value *= kMillion;
    std::uint64_t scale = kMillion;
    for (const char c : fraction) {
        scale /= 10;
        value += static_cast<std::uint64_t>(c - '0') * scale;
    }
    if (value > kMaxRate) return std::nullopt;
    return value;
}

std::optional<std::uint64_t> reported_cost_microunits(const sp::ProviderReportedCost& cost) noexcept {
    constexpr auto available = sp::CostStatus::Available;
    if (cost.source != sp::CostSource::OpenRouterUsd ||
        cost.quality != sp::UsageQuality::Consistent || cost.status[0] != available ||
        !cost.total) {
        return std::nullopt;
    }
    std::uint64_t nano     = cost.total->nano_usd;
    const bool    upstream = cost.status[1] == available && cost.upstream_total.has_value();
    if (cost.byok_status == available && cost.is_byok) {
        if (*cost.is_byok) {
            // The upstream provider billed the caller's own key; OpenRouter's total is its fee.
            if (!upstream || !checked_add(nano, cost.upstream_total->nano_usd)) return std::nullopt;
        }
    } else if (cost.byok_status == sp::CostStatus::Missing) {
        if (upstream && !checked_add(nano, cost.upstream_total->nano_usd)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return nano / 1000 + (nano % 1000 != 0 ? 1 : 0);
}

PriceCatalog PriceCatalog::parse(std::string_view json_text) {
    json root;
    try {
        root = program::detail::parse_json_strict(json_text);
    } catch (const std::exception& error) {
        fail("document", error.what());
    }
    const auto reject_unknown = [](const json& object, const std::string& where,
                                   std::initializer_list<std::string_view> fields) {
        try {
            program::detail::reject_unknown_fields(object, where, fields);
        } catch (const std::invalid_argument& error) {
            fail(where, error.what());
        }
    };
    reject_unknown(root, "catalog",
                   {"schema", "schema_version", "version", "currency", "rate_unit", "origins",
                    "entries"});
    if (text_field(root, "schema", "catalog") != kSchema) {
        fail("catalog.schema", "must be \"neograph.pricing.catalog\"");
    }
    const auto schema_version = require(root, "schema_version", "catalog");
    if (!schema_version.is_number_unsigned() || schema_version.get<unsigned long long>() != 1) {
        fail("catalog.schema_version", "must be 1");
    }
    if (text_field(root, "currency", "catalog") != "USD") fail("catalog.currency", "must be \"USD\"");
    if (text_field(root, "rate_unit", "catalog") != "usd_per_million_tokens") {
        fail("catalog.rate_unit", "must be \"usd_per_million_tokens\"");
    }

    PriceCatalog catalog;
    catalog.version_ = text_field(root, "version", "catalog");
    catalog.digest_  = program::detail::sha256_identity(
        "pricing-catalog", program::detail::canonical_json_bytes(root));

    const auto origins = require(root, "origins", "catalog");
    if (!origins.is_object() || origins.empty()) fail("catalog.origins", "must be a non-empty object");
    for (const auto& [id, value] : origins.items()) {
        const auto where = "origins." + id;
        if (!origin_id_ok(id)) fail(where, "origin ids use lowercase letters, digits and '-'");
        reject_unknown(value, where, {"url", "routing", "notes"});
        Origin origin;
        origin.id  = id;
        origin.url = https_url(value, "url", where);
        if (origin_of(origin.url) != origin.url) {
            fail(where + ".url", "must be a lowercase origin (https://host, no path)");
        }
        const auto routing = text_field(value, "routing", where);
        if (routing != "direct" && routing != "aggregator") {
            fail(where + ".routing", "must be \"direct\" or \"aggregator\"");
        }
        origin.aggregator = routing == "aggregator";
        if (value.contains("notes")) (void)text_field(value, "notes", where);
        for (const auto& other : catalog.origins_) {
            if (other.url == origin.url) fail(where + ".url", "duplicates origin '" + other.id + "'");
        }
        catalog.origins_.push_back(std::move(origin));
    }

    const auto entries = require(root, "entries", "catalog");
    if (!entries.is_array() || entries.empty()) fail("catalog.entries", "must be a non-empty array");
    std::set<std::pair<std::string, std::string>> keys;
    std::size_t index = 0;
    for (const auto& value : entries) {
        const auto where = "entries[" + std::to_string(index++) + "]";
        reject_unknown(value, where,
                       {"origin", "model", "aliases", "source_url", "retrieved", "tiers",
                        "route_ceiling", "limits", "notes", "drift"});
        Entry entry;
        entry.origin = text_field(value, "origin", where);
        const auto* origin = catalog.find_origin(entry.origin);
        if (!origin) fail(where + ".origin", "names no declared origin");
        entry.model      = text_field(value, "model", where);
        entry.source_url = https_url(value, "source_url", where);
        entry.retrieved  = date_field(value, "retrieved", where);
        if (value.contains("notes")) (void)text_field(value, "notes", where);
        if (value.contains("drift") && !value.at("drift").is_object()) {
            fail(where + ".drift", "must be an object");
        }

        std::vector<std::string> names{entry.model};
        if (value.contains("aliases")) {
            const auto aliases = value.at("aliases");
            if (!aliases.is_array()) fail(where + ".aliases", "must be an array of strings");
            std::size_t alias_index = 0;
            for (const auto& alias : aliases) {
                auto name = text(alias, where + ".aliases[" + std::to_string(alias_index++) + "]");
                entry.aliases.push_back(name);
                names.push_back(std::move(name));
            }
        }
        for (const auto& name : names) {
            if (!keys.emplace(entry.origin, name).second) {
                fail(where, "model or alias '" + name + "' is already priced for origin '" +
                                entry.origin + "'");
            }
        }

        const auto tiers = require(value, "tiers", where);
        if (!tiers.is_array() || tiers.empty() || tiers.size() > kMaxTiers) {
            fail(where + ".tiers", "must hold 1 to 8 tiers");
        }
        std::size_t tier_index = 0;
        for (const auto& tier_value : tiers) {
            const auto tier_where = where + ".tiers[" + std::to_string(tier_index) + "]";
            reject_unknown(tier_value, tier_where,
                           {"up_to_input_tokens", "input", "output", "cache_read", "cache_write",
                            "cache_write_1h", "output_image"});
            Tier tier;
            tier.up_to_input_tokens = optional_positive(tier_value, "up_to_input_tokens", tier_where);
            tier.rates              = rates_from(tier_value, tier_where);
            const bool last = tier_index + 1 == tiers.size();
            if (last && tier.up_to_input_tokens) {
                fail(tier_where + ".up_to_input_tokens", "the last tier must be unbounded (null)");
            }
            if (!last && !tier.up_to_input_tokens) {
                fail(tier_where + ".up_to_input_tokens", "only the last tier may be unbounded");
            }
            if (!entry.tiers.empty()) {
                const auto& previous = entry.tiers.back();
                if (tier.up_to_input_tokens &&
                    *tier.up_to_input_tokens <= *previous.up_to_input_tokens) {
                    fail(tier_where + ".up_to_input_tokens", "tier thresholds must increase");
                }
                if (!dominates(tier.rates, previous.rates)) {
                    fail(tier_where, "a higher tier must not be cheaper than the one before it");
                }
            }
            entry.tiers.push_back(std::move(tier));
            ++tier_index;
        }

        if (value.contains("route_ceiling")) {
            const auto ceiling_where = where + ".route_ceiling";
            if (!origin->aggregator) {
                fail(ceiling_where, "only aggregator origins have a route ceiling");
            }
            const auto ceiling_value = value.at("route_ceiling");
            reject_unknown(ceiling_value, ceiling_where,
                           {"source_url", "retrieved", "input", "output", "cache_read",
                            "cache_write", "cache_write_1h", "output_image"});
            RouteCeiling ceiling;
            ceiling.source_url = https_url(ceiling_value, "source_url", ceiling_where);
            ceiling.retrieved  = date_field(ceiling_value, "retrieved", ceiling_where);
            ceiling.rates      = rates_from(ceiling_value, ceiling_where);
            for (const auto& tier : entry.tiers) {
                if (!dominates(ceiling.rates, tier.rates)) {
                    fail(ceiling_where, "the route ceiling is below a listed rate");
                }
            }
            entry.route_ceiling = std::move(ceiling);
        }

        if (value.contains("limits")) {
            const auto limits_where = where + ".limits";
            const auto limits       = value.at("limits");
            reject_unknown(limits, limits_where,
                           {"context_window", "max_input_tokens", "max_output_tokens"});
            entry.limits.context_window = optional_positive(limits, "context_window", limits_where);
            entry.limits.max_input_tokens =
                optional_positive(limits, "max_input_tokens", limits_where);
            entry.limits.max_output_tokens =
                optional_positive(limits, "max_output_tokens", limits_where);
        }
        catalog.entries_.push_back(std::move(entry));
    }
    return catalog;
}

PriceCatalog PriceCatalog::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("file", "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) fail("file", "cannot read " + path.string());
    return parse(buffer.str());
}

const Origin* PriceCatalog::find_origin(std::string_view id) const noexcept {
    for (const auto& origin : origins_)
        if (origin.id == id) return &origin;
    return nullptr;
}

const Origin* PriceCatalog::find_origin_by_url(std::string_view url) const noexcept {
    try {
        const auto wanted = origin_of(url);
        if (wanted.empty()) return nullptr;
        for (const auto& origin : origins_)
            if (origin.url == wanted) return &origin;
    } catch (...) {
    }
    return nullptr;
}

const Entry* PriceCatalog::find(std::string_view origin, std::string_view model) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.origin != origin) continue;
        if (entry.model == model) return &entry;
        for (const auto& alias : entry.aliases)
            if (alias == model) return &entry;
    }
    return nullptr;
}

PricingResult<Reservation> PriceCatalog::reserve(std::string_view origin,
                                                 std::string_view model,
                                                 std::uint64_t    input_tokens_bound,
                                                 std::uint64_t    output_tokens_bound) const {
    const auto* entry = find(origin, model);
    if (!entry) return PricingError::UnknownModel;
    Reservation reservation;
    const Rates* rates = nullptr;
    if (find_origin(entry->origin)->aggregator) {
        if (!entry->route_ceiling) return PricingError::NoUpperBound;
        rates             = &entry->route_ceiling->rates;
        reservation.basis = Basis::RouteCeiling;
    } else {
        rates             = &entry->tier_for(input_tokens_bound).rates;
        reservation.basis = Basis::ListRates;
    }
    Amount amount;
    if (!amount.add(input_tokens_bound, rates->max_input()) ||
        !amount.add(output_tokens_bound, rates->max_output())) {
        return PricingError::Overflow;
    }
    const auto microunits = amount.ceil();
    if (!microunits) return PricingError::Overflow;
    reservation.microunits      = *microunits;
    reservation.catalog_version = version_;
    reservation.catalog_digest  = digest_;
    return reservation;
}

PricingResult<Settlement> PriceCatalog::settle(std::string_view origin,
                                               std::string_view model,
                                               const sp::Usage& usage) const {
    if (usage.stage != sp::UsageStage::Final) return PricingError::IncompleteUsage;
    if (const auto reported = reported_cost_microunits(usage.provider_cost)) {
        Settlement settlement;
        settlement.microunits = *reported;
        settlement.evidence   = Evidence::Reported;
        settlement.basis      = Basis::ProviderReported;
        return settlement;
    }
    return estimate(origin, model, usage);
}

PricingResult<Settlement> PriceCatalog::estimate(std::string_view origin,
                                                 std::string_view model,
                                                 const sp::Usage& usage) const {
    const auto* entry = find(origin, model);
    if (!entry) return PricingError::UnknownModel;
    if (usage.stage != sp::UsageStage::Final || !usage.input_total || !usage.output_total) {
        return PricingError::IncompleteUsage;
    }
    if (usage.quality != sp::UsageQuality::Consistent) return PricingError::InconsistentUsage;
    const auto input_total  = usage.input_total->value;
    const auto output_total = usage.output_total->value;

    Settlement settlement;
    settlement.evidence        = Evidence::Estimated;
    settlement.catalog_version = version_;
    settlement.catalog_digest  = digest_;
    const Rates* rates         = nullptr;
    if (find_origin(entry->origin)->aggregator && entry->route_ceiling) {
        rates            = &entry->route_ceiling->rates;
        settlement.basis = Basis::RouteCeiling;
    } else {
        rates                     = &entry->tier_for(input_total).rates;
        settlement.basis          = Basis::ListRates;
        settlement.may_undercount = find_origin(entry->origin)->aggregator;
    }

    Amount                     amount;
    std::uint64_t              attributed = 0;
    std::vector<std::uint64_t> unreported;  // rates of input bands the usage does not report
    const auto band = [&](std::uint64_t tokens, std::uint64_t rate_per_million) {
        return amount.add(tokens, rate_per_million) && checked_add(attributed, tokens);
    };

    bool ok = true;
    if (usage.input_uncached) {
        ok = ok && band(usage.input_uncached->value, rates->input);
    } else {
        unreported.push_back(rates->input);
    }
    if (usage.cache_read) {
        ok = ok && band(usage.cache_read->value, rates->effective_cache_read());
    } else {
        unreported.push_back(rates->effective_cache_read());
    }
    const auto write_5m = rates->effective_cache_write();
    const auto write_1h = rates->effective_cache_write_1h();
    if (usage.cache_write) {
        const auto written = usage.cache_write->value;
        const auto split5m = extra_count(usage, kCacheWrite5m);
        const auto split1h = extra_count(usage, kCacheWrite1h);
        std::uint64_t split = 0;
        if (split5m && split1h && checked_add(split, *split5m) && checked_add(split, *split1h) &&
            split == written) {
            ok = ok && band(*split5m, write_5m) && band(*split1h, write_1h);
        } else {
            ok = ok && band(written, (std::max)(write_5m, write_1h));
            if (written != 0 && write_5m != write_1h) settlement.conservative = true;
        }
    } else {
        unreported.push_back(write_5m);
        unreported.push_back(write_1h);
    }
    if (!ok) return PricingError::Overflow;
    if (attributed > input_total) return PricingError::InconsistentUsage;
    if (const auto residual = input_total - attributed; residual != 0) {
        if (unreported.empty()) return PricingError::InconsistentUsage;
        const auto [lowest, highest] = std::minmax_element(unreported.begin(), unreported.end());
        if (!amount.add(residual, *highest)) return PricingError::Overflow;
        if (*lowest != *highest) settlement.conservative = true;
    }

    const auto image_rate = rates->effective_output_image();
    if (image_rate == rates->output) {
        ok = amount.add(output_total, rates->output);
    } else if (const auto image = extra_count(usage, kImageOutput)) {
        if (*image > output_total) return PricingError::InconsistentUsage;
        ok = amount.add(*image, image_rate) && amount.add(output_total - *image, rates->output);
    } else {
        ok = amount.add(output_total, (std::max)(image_rate, rates->output));
        if (output_total != 0) settlement.conservative = true;
    }
    if (!ok) return PricingError::Overflow;

    const auto microunits = amount.ceil();
    if (!microunits) return PricingError::Overflow;
    settlement.microunits = *microunits;
    return settlement;
}

}  // namespace neograph::program::pricing
