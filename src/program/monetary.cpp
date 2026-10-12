#include "monetary.h"

#include "canonical_json.h"

#include <neograph/graph/cancel.h>
#include <neograph/types.h>

#include <descriptor/descriptor.h>
#include <descriptor/policy.h>

#include <yyjson.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace neograph::program {

std::string_view to_string(ProgramMonetaryEvidence evidence) noexcept {
    switch (evidence) {
        case ProgramMonetaryEvidence::Reported: return "reported";
        case ProgramMonetaryEvidence::Estimated: return "estimated";
        case ProgramMonetaryEvidence::Held: return "held";
    }
    return "unknown";
}

ProgramMonetaryEvidence program_monetary_evidence_from_string(std::string_view value) {
    if (value == "reported") return ProgramMonetaryEvidence::Reported;
    if (value == "estimated") return ProgramMonetaryEvidence::Estimated;
    if (value == "held") return ProgramMonetaryEvidence::Held;
    throw std::invalid_argument("Unknown Program monetary evidence: " + std::string(value));
}

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > kMax - left ? kMax : left + right;
}

}  // namespace

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------

ProgramMonetaryAccount::ProgramMonetaryAccount(std::uint64_t ceiling, std::uint64_t attempt)
    : ceiling_(ceiling), attempt_(attempt) {}

bool ProgramMonetaryAccount::try_reserve(std::uint64_t amount) {
    std::lock_guard lock(mutex_);
    const auto committed = saturating_add(charged_, reserved_);
    if (committed > ceiling_ || amount > ceiling_ - committed) return false;
    reserved_ = saturating_add(reserved_, amount);
    return true;
}

void ProgramMonetaryAccount::release(std::uint64_t amount) noexcept {
    std::lock_guard lock(mutex_);
    reserved_ -= std::min(reserved_, amount);
}

void ProgramMonetaryAccount::settle(std::uint64_t held, ProgramMonetaryRecord record) {
    std::lock_guard lock(mutex_);
    reserved_ -= std::min(reserved_, held);
    charged_ = saturating_add(charged_, record.microunits);
    records_.push_back(std::move(record));
}

bool ProgramMonetaryAccount::remember(std::string key) {
    std::lock_guard lock(mutex_);
    return accounted_.insert(std::move(key)).second;
}

bool ProgramMonetaryAccount::restore(std::string key, ProgramMonetaryRecord record) {
    std::lock_guard lock(mutex_);
    if (!accounted_.insert(std::move(key)).second) return false;
    charged_ = saturating_add(charged_, record.microunits);
    records_.push_back(std::move(record));
    return true;
}

std::uint64_t ProgramMonetaryAccount::used() const {
    std::lock_guard lock(mutex_);
    return saturating_add(charged_, reserved_);
}

bool ProgramMonetaryAccount::exceeded() const {
    std::lock_guard lock(mutex_);
    return saturating_add(charged_, reserved_) > ceiling_;
}

std::vector<ProgramMonetaryRecord> ProgramMonetaryAccount::records() const {
    std::lock_guard lock(mutex_);
    return records_;
}

void ProgramMonetaryAccount::note_refusal(sp::runtime::Result outcome,
                                          detail::MonetaryRefusal refusal) {
    std::lock_guard lock(mutex_);
    refusals_.emplace_back(std::move(outcome), std::move(refusal));
}

std::optional<detail::MonetaryRefusal> ProgramMonetaryAccount::refusal_for(
    const sp::runtime::Result& outcome) const {
    if (!outcome) return std::nullopt;
    std::lock_guard lock(mutex_);
    for (const auto& [refused, refusal] : refusals_)
        if (refused.get() == outcome.get()) return refusal;
    return std::nullopt;
}

namespace detail {
namespace {

// Service tier names that mean the standard tier the catalog prices.
bool standard_service_tier(std::string_view tier) {
    return tier == "default" || tier == "standard";
}

// The largest amount OpenRouter's provider.max_price admits, in nanoUSD.
constexpr std::uint64_t kMaxPriceNanoUsd = std::uint64_t{1} << 53;

// nanoUSD from the exact decimal USD number the SDK writes for max_price:
// whole dollars, then at most nine fractional digits, no sign or exponent.
std::optional<std::uint64_t> exact_nano_usd(std::string_view text) {
    const auto dot      = text.find('.');
    const auto whole    = text.substr(0, dot);
    const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || whole.size() > 10 || fraction.size() > 9 ||
        (dot != std::string_view::npos && fraction.empty()))
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char c : whole) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    for (std::size_t i = 0; i < 9; ++i) {
        const char c = i < fraction.size() ? fraction[i] : '0';
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > kMaxPriceNanoUsd) return std::nullopt;
    return value;
}

// The provider.max_price a prepared request already sends: the host's own cap
// from its request controls or its provider defaults, as the provider merged
// them. Read from the encoded body because the defaults live in the provider.
struct SentMaxPrice {
    std::optional<sp::OpenRouterMaxPrice> price;
    bool                                  malformed = false;
};

SentMaxPrice sent_max_price(std::string_view body) {
    if (body.find("\"max_price\"") == std::string_view::npos) return {};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
        yyjson_read_opts(const_cast<char*>(body.data()), body.size(), YYJSON_READ_NUMBER_AS_RAW,
                         nullptr, nullptr),
        &yyjson_doc_free);
    if (!document) return {std::nullopt, true};
    auto* price = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document.get()), "provider"),
                                 "max_price");
    if (!price) return {};
    if (!yyjson_is_obj(price)) return {std::nullopt, true};
    SentMaxPrice sent{sp::OpenRouterMaxPrice{}, false};
    const std::pair<const char*, std::optional<std::uint64_t> sp::OpenRouterMaxPrice::*> fields[] = {
        {"prompt", &sp::OpenRouterMaxPrice::prompt_nano_usd},
        {"completion", &sp::OpenRouterMaxPrice::completion_nano_usd},
        {"image", &sp::OpenRouterMaxPrice::image_nano_usd},
        {"audio", &sp::OpenRouterMaxPrice::audio_nano_usd},
        {"request", &sp::OpenRouterMaxPrice::request_nano_usd}};
    std::size_t known = 0;
    for (const auto& [key, member] : fields) {
        auto* amount = yyjson_obj_get(price, key);
        if (!amount) continue;
        ++known;
        const auto nano = yyjson_is_raw(amount)
                              ? exact_nano_usd(std::string_view(yyjson_get_raw(amount),
                                                                yyjson_get_len(amount)))
                              : std::nullopt;
        if (!nano) return {std::nullopt, true};
        (*sent.price).*member = nano;
    }
    if (known == 0 || known != yyjson_obj_size(price)) return {std::nullopt, true};
    return sent;
}

constexpr std::optional<std::uint64_t> sp::OpenRouterMaxPrice::* kMaxPriceFields[] = {
    &sp::OpenRouterMaxPrice::prompt_nano_usd, &sp::OpenRouterMaxPrice::completion_nano_usd,
    &sp::OpenRouterMaxPrice::image_nano_usd, &sp::OpenRouterMaxPrice::audio_nano_usd,
    &sp::OpenRouterMaxPrice::request_nano_usd};

// Field by field, the lower of the route ceiling and the host's own cap. A
// field only the host caps keeps the host's amount.
sp::OpenRouterMaxPrice lower_max_price(sp::OpenRouterMaxPrice                       ceiling,
                                       const std::optional<sp::OpenRouterMaxPrice>& own) {
    if (!own) return ceiling;
    for (const auto member : kMaxPriceFields) {
        const auto& host   = (*own).*member;
        auto&       pinned = ceiling.*member;
        if (host && (!pinned || *host < *pinned)) pinned = host;
    }
    return ceiling;
}

bool same_max_price(const sp::OpenRouterMaxPrice& left, const sp::OpenRouterMaxPrice& right) {
    for (const auto member : kMaxPriceFields)
        if (left.*member != right.*member) return false;
    return true;
}

std::optional<std::uint64_t> min_known(std::initializer_list<std::optional<std::uint64_t>> values) {
    std::optional<std::uint64_t> result;
    for (const auto& value : values)
        if (value && *value > 0 && (!result || *value < *result)) result = value;
    return result;
}

void block(MonetaryQuote& quote, std::string code, std::string message) {
    if (!quote.blocker.empty()) return;
    quote.blocker         = std::move(code);
    quote.blocker_message = std::move(message);
}

const pricing::Origin* resolve_origin(const pricing::PriceCatalog&            catalog,
                                      const sp::descriptor::ValidatedDescriptor& descriptor) {
    if (const auto* origin = catalog.find_origin_by_url(descriptor.base_url())) return origin;
    // The SDK policy is the authority that this base URL is an OpenRouter
    // routing gateway (it admits OpenRouter routing controls and parses
    // usage.cost for it), so it is priced as OpenRouter.
    if (sp::descriptor::routed_gateway(descriptor)) {
        const auto* origin = catalog.find_origin("openrouter");
        if (origin && origin->aggregator) return origin;
    }
    return nullptr;
}

std::string basis_name(pricing::Basis basis) { return std::string(pricing::to_string(basis)); }

ProgramMonetaryRecord base_record(const MonetaryQuote& quote, const MonetaryCallSite& site) {
    ProgramMonetaryRecord record;
    record.call_id             = site.call_id;
    record.operation_id        = site.operation_id;
    record.node                = site.node;
    record.origin              = quote.origin;
    record.model               = quote.model;
    record.reserved_microunits = quote.hold;
    return record;
}

ProgramMonetaryRecord held_record(const MonetaryQuote& quote, const MonetaryCallSite& site,
                                  std::string reason) {
    auto record            = base_record(quote, site);
    record.evidence        = ProgramMonetaryEvidence::Held;
    record.microunits      = quote.hold;
    record.basis           = quote.hold_basis;
    record.catalog_version = quote.catalog_version;
    record.catalog_digest  = quote.catalog_digest;
    record.reason          = std::move(reason);
    return record;
}

json require_field(const json& value, const char* field) {
    if (!value.contains(field))
        throw std::invalid_argument(std::string("Program monetary record requires '") + field + "'");
    return value.at(field);
}

std::string string_field(const json& value, const char* field) {
    const auto item = require_field(value, field);
    if (!item.is_string())
        throw std::invalid_argument(std::string("Program monetary record '") + field +
                                    "' must be a string");
    return item.get<std::string>();
}

std::uint64_t unsigned_field(const json& value, const char* field) {
    const auto item = require_field(value, field);
    if (!item.is_number_unsigned())
        throw std::invalid_argument(std::string("Program monetary record '") + field +
                                    "' must be unsigned");
    return item.get<std::uint64_t>();
}

bool bool_field(const json& value, const char* field) {
    const auto item = require_field(value, field);
    if (!item.is_boolean())
        throw std::invalid_argument(std::string("Program monetary record '") + field +
                                    "' must be a boolean");
    return item.get<bool>();
}

void bounded_text(const std::string& value, std::string_view field, bool may_be_empty) {
    if ((!may_be_empty && value.empty()) || value.size() > 512)
        throw std::invalid_argument("Program monetary record " + std::string(field) +
                                    " is empty or too long");
    neograph::detail::validate_utf8(value);
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7f)
            throw std::invalid_argument("Program monetary record " + std::string(field) +
                                        " contains a control character");
}

}  // namespace

std::optional<std::string> unmodeled_feature(const ProviderRequest& request) {
    std::optional<std::string> found;
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (requires { payload.service_tier; }) {
                if (payload.service_tier && !standard_service_tier(*payload.service_tier))
                    found = "service tier '" + *payload.service_tier + "'";
            }
            if constexpr (std::is_same_v<T, sp::chat::Request>) {
                if (!found && !payload.models.empty())
                    found = "OpenRouter model fallbacks (models)";
            }
            if constexpr (std::is_same_v<T, sp::responses::Request>) {
                if (!found && !payload.hosted_tools.empty()) found = "server-side tools";
            }
            if constexpr (std::is_same_v<T, sp::messages::Request>) {
                for (const auto& tool : payload.tools)
                    if (!found && !tool.type.empty()) found = "server tool '" + tool.type + "'";
            }
        },
        request.payload);
    return found;
}

MonetaryQuote assess_monetary_call(const pricing::PriceCatalog&      catalog,
                                   const std::optional<std::string>& unmodeled,
                                   const PreparedProviderRequest&    prepared) {
    MonetaryQuote quote;
    quote.model           = std::string(prepared.model());
    quote.catalog_version = catalog.version();
    quote.catalog_digest  = catalog.digest();
    quote.witness         = json{{"model", quote.model},
                                 {"catalog_version", catalog.version()},
                                 {"catalog_digest", catalog.digest()}};
    const auto* descriptor = prepared.admitted_descriptor();
    if (!descriptor) {
        block(quote, "P_MONEY_UNKNOWN_ORIGIN",
              "the provider has no admitted descriptor, so its price catalog origin is unknown");
        return quote;
    }
    quote.witness["base_url"] = std::string(descriptor->base_url());
    const auto* origin        = resolve_origin(catalog, *descriptor);
    if (!origin) {
        block(quote, "P_MONEY_UNKNOWN_ORIGIN",
              "the provider endpoint " + std::string(descriptor->base_url()) +
                  " is not an origin of price catalog " + catalog.version());
        return quote;
    }
    quote.origin            = origin->id;
    quote.witness["origin"] = origin->id;
    const auto* entry       = catalog.find(origin->id, quote.model);
    if (!entry) {
        block(quote, "P_MONEY_UNKNOWN_MODEL",
              "price catalog " + catalog.version() + " has no entry for model '" + quote.model +
                  "' at origin '" + origin->id + "'");
        return quote;
    }
    auto feature = unmodeled;
    if (!feature) {
        const auto defaults = sp::descriptor::effective_defaults(*descriptor, quote.model);
        if (defaults.service_tier && !standard_service_tier(*defaults.service_tier))
            feature = "service tier '" + std::string(*defaults.service_tier) + "'";
    }
    if (feature) {
        quote.witness["unmodeled"] = *feature;
        block(quote, "P_MONEY_UNMODELED",
              "the request uses " + *feature + ", which the price catalog does not model");
        return quote;
    }
    if (origin->aggregator && entry->route_ceiling) {
        // The reservation below is an upper bound only if the router cannot
        // pick an endpoint dearer than the route ceiling, so the ceiling is
        // pinned on the request as OpenRouter's provider.max_price.
        if (!sp::descriptor::routed_gateway(*descriptor)) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the SDK policy does not admit OpenRouter routing at " +
                      std::string(descriptor->base_url()) + ", so the route ceiling of model '" +
                      quote.model + "' cannot be pinned as provider.max_price");
            return quote;
        }
        const auto ceiling = route_max_price(*entry);
        if (!ceiling) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the route ceiling of model '" + quote.model +
                      "' exceeds the largest provider.max_price amount");
            return quote;
        }
        const auto own = sent_max_price(prepared.encoded_body());
        if (own.malformed) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the request's provider.max_price is not an exact USD amount");
            return quote;
        }
        quote.pin = lower_max_price(*ceiling, own.price);
    }

    const auto output = prepared.max_output_tokens();
    if (!output) {
        block(quote, "P_MONEY_NO_OUTPUT_CAP",
              "the request has no output token cap, so its cost has no upper bound");
        return quote;
    }
    const auto input = min_known(
        {descriptor->policy()->input_limit(prepared.family(), quote.model),
         entry->limits.max_input_tokens, entry->limits.context_window});
    if (!input) {
        block(quote, "P_MONEY_NO_INPUT_BOUND",
              "no input token limit is known for model '" + quote.model +
                  "', so its cost has no upper bound");
        return quote;
    }
    const auto invocations = Provider::invocation_upper_bound(prepared);
    if (!invocations) {
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the prepared request does not state how many invocations it can bill");
        return quote;
    }
    quote.witness["input_tokens_bound"]  = *input;
    quote.witness["output_tokens_bound"] = *output;
    quote.witness["invocations"]         = *invocations;
    const auto reserved = catalog.reserve(origin->id, quote.model, *input, *output);
    if (const auto* error = std::get_if<pricing::PricingError>(&reserved)) {
        if (*error == pricing::PricingError::NoUpperBound)
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "model '" + quote.model + "' at aggregator origin '" + origin->id +
                      "' has no route ceiling, so the router may pick any price");
        else
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the reservation for model '" + quote.model + "' does not fit (" +
                      std::string(pricing::to_string(*error)) + ")");
        return quote;
    }
    const auto& reservation = std::get<pricing::Reservation>(reserved);
    if (reservation.microunits > kMax / *invocations) {
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the reservation for model '" + quote.model + "' does not fit 64-bit microunits");
        return quote;
    }
    quote.hold                          = reservation.microunits * *invocations;
    quote.hold_basis                    = basis_name(reservation.basis);
    quote.witness["reserved_microunits"] = quote.hold;
    quote.witness["basis"]               = quote.hold_basis;
    return quote;
}

std::optional<sp::OpenRouterMaxPrice> route_max_price(const pricing::Entry& entry) {
    if (!entry.route_ceiling) return std::nullopt;
    // Catalog rates are micro-USD per million tokens; max_price takes nanoUSD.
    const auto nano = [](std::uint64_t micro) -> std::optional<std::uint64_t> {
        if (micro > kMaxPriceNanoUsd / 1000) return std::nullopt;
        return micro * 1000;
    };
    sp::OpenRouterMaxPrice price;
    price.prompt_nano_usd     = nano(entry.route_ceiling->rates.max_input());
    price.completion_nano_usd = nano(entry.route_ceiling->rates.max_output());
    if (!price.prompt_nano_usd || !price.completion_nano_usd) return std::nullopt;
    return price;
}

bool pin_route_max_price(ProviderRequest& request, const sp::OpenRouterMaxPrice& price) {
    return std::visit(
        [&](auto& payload) -> bool {
            if constexpr (requires {
                              requires std::is_same_v<std::decay_t<decltype(payload.provider)>,
                                                      std::optional<sp::OpenRouterRouting>>;
                          }) {
                // Chat Completions, Responses and Messages: the families the
                // SDK admits OpenRouter routing for. The other routing fields
                // stay as the host set them; provider defaults still fill the
                // ones it left unset.
                auto& routing = payload.provider;
                if (routing && routing->max_price && same_max_price(*routing->max_price, price))
                    return false;
                if (!routing) routing.emplace();
                routing->max_price = price;
                return true;
            } else {
                throw std::logic_error("This provider family cannot carry OpenRouter provider.max_price");
            }
        },
        request.payload);
}

PricedPreparation prepare_priced_call(const pricing::PriceCatalog& catalog, Provider& provider,
                                      ProviderRequest request, bool priced) {
    if (!priced) return {provider.prepare(std::move(request)), std::nullopt};
    const auto unmodeled = unmodeled_feature(request);
    std::optional<MonetaryQuote> quote;
    {
        // The quote never reads the conversation, so a probe with a one-message
        // conversation prices the call. It carries no native history, whose
        // routing binding would refuse an unpinned turn after a pinned one.
        sp::Message question;
        question.role = sp::Role::User;
        question.parts.emplace_back(sp::Text{"."});
        auto probe = request;
        set_provider_request_messages(probe, {std::move(question)});
        std::visit(
            [](auto& payload) {
                if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::responses::Request>) {
                    payload.previous_response_id.reset();
                    payload.previous_response_history.clear();
                }
            },
            probe.payload);
        const auto probed = provider.prepare(std::move(probe));
        if (probed.valid()) quote = assess_monetary_call(catalog, unmodeled, probed);
    }  // The probe's operation slot is released here, unsent.
    if (quote) {
        if (quote->pin) (void)pin_route_max_price(request, *quote->pin);
        return {provider.prepare(std::move(request)), std::move(quote)};
    }
    // The provider does not admit the probe: price the request's own preparation.
    auto pinnable = request;
    auto prepared = provider.prepare(std::move(request));
    if (!prepared.valid()) return {std::move(prepared), std::nullopt};
    quote = assess_monetary_call(catalog, unmodeled, prepared);
    if (quote->pin && pin_route_max_price(pinnable, *quote->pin)) {
        prepared = PreparedProviderRequest{};  // release its operation slot first
        prepared = provider.prepare(std::move(pinnable));
    }
    return {std::move(prepared), std::move(quote)};
}

sp::runtime::Result preparation_failure_outcome(const PreparedProviderRequest& prepared) {
    sp::Failure failure;
    if (prepared.error()) {
        failure.error = *prepared.error();
    } else {
        failure.error.kind         = sp::ErrorKind::InvalidRequest;
        failure.error.safe_message = "Program provider preparation is invalid";
        failure.error.retry_safety = sp::RetrySafety::NotSent;
    }
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

sp::runtime::Result monetary_refusal_outcome(const MonetaryRefusal& refusal) {
    sp::Failure failure;
    failure.error.kind = refusal.budget ? sp::ErrorKind::QuotaExhausted : sp::ErrorKind::InvalidConfig;
    failure.error.safe_message = refusal.code + ": " + refusal.message;
    failure.error.retry_class  = sp::RetryClass::Never;
    failure.error.retry_safety = sp::RetrySafety::NotSent;
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

std::optional<ProgramMonetaryRecord> settle_monetary_call(const pricing::PriceCatalog& catalog,
                                                          const MonetaryQuote&         quote,
                                                          const MonetaryCallSite&      site,
                                                          const sp::runtime::Result&   outcome) {
    if (!outcome) return unknown_outcome_record(quote, site, "no outcome");
    if (const auto* failure = std::get_if<sp::Failure>(outcome.get())) {
        if (provider_failure_proves_not_sent(*failure)) return std::nullopt;
        return held_record(quote, site, "failed after the request may have been sent");
    }
    const auto& completion = std::get<sp::Completion>(*outcome);
    if (completion.attempt.prior_usage_unknown || completion.attempt.transport_internal_resends != 0)
        return held_record(quote, site, "the usage of resent transport attempts is unknown");
    const auto settled = catalog.settle(quote.origin, quote.model, completion.usage);
    if (const auto* error = std::get_if<pricing::PricingError>(&settled))
        return held_record(quote, site, std::string(pricing::to_string(*error)));
    const auto& settlement = std::get<pricing::Settlement>(settled);
    auto        record     = base_record(quote, site);
    record.evidence        = settlement.evidence == pricing::Evidence::Reported
                                 ? ProgramMonetaryEvidence::Reported
                                 : ProgramMonetaryEvidence::Estimated;
    record.microunits      = settlement.microunits;
    record.basis           = basis_name(settlement.basis);
    record.catalog_version = settlement.catalog_version;
    record.catalog_digest  = settlement.catalog_digest;
    record.conservative    = settlement.conservative;
    record.may_undercount  = settlement.may_undercount;
    return record;
}

ProgramMonetaryRecord unknown_outcome_record(const MonetaryQuote& quote, const MonetaryCallSite& site,
                                             std::string reason) {
    return held_record(quote, site, std::move(reason));
}

json encode_monetary_record(const ProgramMonetaryRecord& record) {
    return json{{"call_id", record.call_id},
                {"operation_id", record.operation_id},
                {"node", record.node},
                {"origin", record.origin},
                {"model", record.model},
                {"evidence", std::string(to_string(record.evidence))},
                {"microunits", record.microunits},
                {"reserved_microunits", record.reserved_microunits},
                {"basis", record.basis},
                {"catalog_version", record.catalog_version},
                {"catalog_digest", record.catalog_digest},
                {"conservative", record.conservative},
                {"may_undercount", record.may_undercount},
                {"replayed", record.replayed},
                {"reason", record.reason}};
}

ProgramMonetaryRecord decode_monetary_record(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Program monetary record must be an object");
    neograph::detail::reject_unknown_fields(
        value, "Program monetary record",
        {"call_id", "operation_id", "node", "origin", "model", "evidence", "microunits",
         "reserved_microunits", "basis", "catalog_version", "catalog_digest", "conservative",
         "may_undercount", "replayed", "reason"});
    ProgramMonetaryRecord record;
    record.call_id             = string_field(value, "call_id");
    record.operation_id        = string_field(value, "operation_id");
    record.node                = string_field(value, "node");
    record.origin              = string_field(value, "origin");
    record.model               = string_field(value, "model");
    record.evidence            = program_monetary_evidence_from_string(string_field(value, "evidence"));
    record.microunits          = unsigned_field(value, "microunits");
    record.reserved_microunits = unsigned_field(value, "reserved_microunits");
    record.basis               = string_field(value, "basis");
    record.catalog_version     = string_field(value, "catalog_version");
    record.catalog_digest      = string_field(value, "catalog_digest");
    record.conservative        = bool_field(value, "conservative");
    record.may_undercount      = bool_field(value, "may_undercount");
    record.replayed            = bool_field(value, "replayed");
    record.reason              = string_field(value, "reason");
    validate_monetary_record(record);
    return record;
}

void validate_monetary_record(const ProgramMonetaryRecord& record) {
    bounded_text(record.call_id, "call_id", false);
    bounded_text(record.operation_id, "operation_id", false);
    bounded_text(record.node, "node", false);
    bounded_text(record.origin, "origin", true);
    bounded_text(record.model, "model", true);
    bounded_text(record.catalog_version, "catalog_version", true);
    bounded_text(record.catalog_digest, "catalog_digest", true);
    bounded_text(record.reason, "reason", true);
    if (!record.basis.empty() && record.basis != "provider_reported" &&
        record.basis != "list_rates" && record.basis != "route_ceiling")
        throw std::invalid_argument("Program monetary record basis is unknown");
    switch (record.evidence) {
        case ProgramMonetaryEvidence::Reported:
            if (record.basis != "provider_reported")
                throw std::invalid_argument("A reported monetary amount has provider_reported basis");
            break;
        case ProgramMonetaryEvidence::Estimated:
        case ProgramMonetaryEvidence::Held:
            if (record.catalog_version.empty() || record.catalog_digest.empty())
                throw std::invalid_argument("An estimated or held monetary amount names its catalog");
            break;
    }
}

std::string provider_call_slot_id(const ProgramCoreProviderCallContext& context,
                                  const graph::ProviderCallIdentity&    call) {
    // The provider journal keys its rows by exactly this identity. Changing a
    // byte here would make a historic dispatched slot look like a fresh call.
    return neograph::detail::sha256_identity(
        "program-core-provider-call/v1", "slot",
        neograph::detail::canonical_json_bytes(
            json{{"owner_scope", std::string(context.owner_scope)},
                 {"program_version_id", std::string(context.program_version_id)},
                 {"run_id", std::string(context.run_id)},
                 {"operation_id", std::string(context.operation_id)},
                 {"thread_id", call.thread_id},
                 {"task_id", call.task_id},
                 {"node_name", call.node_name},
                 {"call_ordinal", call.call_ordinal}}));
}

// ---------------------------------------------------------------------------
// MonetaryHold
// ---------------------------------------------------------------------------

MonetaryHold::MonetaryHold(std::shared_ptr<ProgramMonetaryAccount> account, MonetaryQuote quote,
                           MonetaryCallSite site)
    : account_(std::move(account)), quote_(std::move(quote)), site_(std::move(site)) {}

MonetaryHold::MonetaryHold(MonetaryHold&& other) noexcept
    : account_(std::move(other.account_)), quote_(std::move(other.quote_)),
      site_(std::move(other.site_)), reserved_(std::exchange(other.reserved_, 0)),
      dispatched_(other.dispatched_), done_(std::exchange(other.done_, true)) {}

MonetaryHold& MonetaryHold::operator=(MonetaryHold&& other) noexcept {
    if (this != &other) {
        finish();
        account_    = std::move(other.account_);
        quote_      = std::move(other.quote_);
        site_       = std::move(other.site_);
        reserved_   = std::exchange(other.reserved_, 0);
        dispatched_ = other.dispatched_;
        done_       = std::exchange(other.done_, true);
    }
    return *this;
}

MonetaryHold::~MonetaryHold() { finish(); }

void MonetaryHold::finish() noexcept {
    if (!account_ || done_) return;
    if (dispatched_)
        settle_unknown("the dispatch ended without an outcome");
    else
        release();
}

sp::runtime::Result MonetaryHold::preflight() {
    if (!account_) return {};
    std::optional<MonetaryRefusal> refusal;
    if (!quote_.blocker.empty()) {
        refusal = MonetaryRefusal{quote_.blocker, quote_.blocker_message, quote_.witness, false};
    } else if (!account_->try_reserve(quote_.hold)) {
        auto witness       = quote_.witness;
        witness["ceiling"] = account_->ceiling();
        witness["used"]    = account_->used();
        refusal            = MonetaryRefusal{
            "P_MONEY_BUDGET",
            "the call's reservation of " + std::to_string(quote_.hold) +
                " microunits does not fit the run's remaining monetary budget of " +
                std::to_string(account_->ceiling()) + " microunits",
            std::move(witness), true};
    }
    if (refusal) {
        auto outcome = monetary_refusal_outcome(*refusal);
        account_->note_refusal(outcome, std::move(*refusal));
        account_.reset();
        return outcome;
    }
    reserved_ = quote_.hold;
    done_     = false;
    return {};
}

std::optional<ProgramMonetaryRecord> MonetaryHold::record_for(const sp::runtime::Result& outcome) const {
    if (!account_) return std::nullopt;
    return settle_monetary_call(pricing::PriceCatalog::builtin(), quote_, site_, outcome);
}

void MonetaryHold::settle(const sp::runtime::Result& outcome) { settle_record(record_for(outcome)); }

void MonetaryHold::settle_record(std::optional<ProgramMonetaryRecord> record) {
    if (!account_ || done_) return;
    done_ = true;
    if (record)
        account_->settle(reserved_, std::move(*record));
    else
        account_->release(reserved_);
}

void MonetaryHold::settle_unknown(std::string reason) noexcept {
    if (!account_ || done_) return;
    done_ = true;
    try {
        account_->settle(reserved_, unknown_outcome_record(quote_, site_, std::move(reason)));
    } catch (...) {
        // Allocation failure: keep the hold rather than lose the charge.
    }
}

void MonetaryHold::release() noexcept {
    if (!account_ || done_) return;
    done_ = true;
    account_->release(reserved_);
}

void MonetaryHold::remember(std::string key) {
    if (account_) (void)account_->remember(std::move(key));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// In-process pricing broker
// ---------------------------------------------------------------------------

namespace {

class PricedProviderCallBroker final : public graph::ProviderCallBroker {
public:
    explicit PricedProviderCallBroker(const ProgramCoreProviderCallContext& context)
        : owner_(context.owner_scope), version_(context.program_version_id),
          run_(context.run_id), operation_(context.operation_id), attempt_(context.attempt),
          account_(context.monetary_account) {}

    asio::awaitable<sp::runtime::Result> invoke(graph::ProviderCallIdentity identity,
                                               std::shared_ptr<Provider>   provider,
                                               ProviderRequest             request) override {
        return invoke_owned(owner_, version_, run_, operation_, attempt_, account_,
                            std::move(identity), std::move(provider), std::move(request));
    }

private:
    static asio::awaitable<sp::runtime::Result> invoke_owned(
        std::string owner, std::string version, std::string run, std::string operation,
        std::uint64_t attempt, std::shared_ptr<ProgramMonetaryAccount> account,
        graph::ProviderCallIdentity identity, std::shared_ptr<Provider> provider,
        ProviderRequest request) {
        if (!provider || identity.owner_scope != owner || identity.run_id != run ||
            identity.thread_id.empty() || identity.task_id.empty() || identity.node_name.empty())
            throw std::invalid_argument("Program provider call scope is invalid");
        auto priced = detail::prepare_priced_call(pricing::PriceCatalog::builtin(), *provider,
                                                  std::move(request), account != nullptr);
        auto prepared = std::move(priced.prepared);
        if (!prepared.valid()) co_return detail::preparation_failure_outcome(prepared);
        detail::MonetaryHold hold;
        if (priced.quote) {
            const ProgramCoreProviderCallContext context{owner, version, run, operation, attempt};
            hold = detail::MonetaryHold(
                account, std::move(*priced.quote),
                detail::MonetaryCallSite{detail::provider_call_slot_id(context, identity),
                                         operation, identity.node_name});
            if (auto refused = hold.preflight()) co_return refused;
        }
        ProviderBudgetClaim claim;
        try {
            claim = reserve_provider_dispatch(
                prepared, ProviderDispatchBudget{identity.usage, identity.model_token_budget,
                                                 identity.budget_exhausted,
                                                 identity.budget_cancel_token});
        } catch (const ProviderFailure& error) {
            hold.release();
            co_return error.outcome();
        }
        claim.mark_dispatched();
        hold.mark_dispatched();
        sp::runtime::Result result;
        std::exception_ptr  observer_error;
        try {
            result = co_await provider->dispatch_async(std::move(prepared));
            if (!result) throw std::runtime_error("Program provider returned no owned outcome");
        } catch (const ProviderOutcomeError& error) {
            result         = error.outcome();
            observer_error = std::current_exception();
            if (!result) {
                hold.settle_unknown("the dispatch raised without an outcome");
                throw;
            }
        }
        // Any other exception leaves the hold to its destructor, which charges
        // it: the request may have reached the provider.
        if (claim.active()) {
            (void)claim.settle(result);
        } else if (identity.usage) {
            if (const auto* completion = std::get_if<sp::Completion>(result.get()))
                identity.usage->add(completion->usage);
            else
                identity.usage->observe(std::get<sp::Failure>(*result).partial.usage);
        }
        hold.settle(result);
        if (observer_error) std::rethrow_exception(observer_error);
        co_return result;
    }

    std::string                             owner_, version_, run_, operation_;
    std::uint64_t                           attempt_;
    std::shared_ptr<ProgramMonetaryAccount> account_;
};

}  // namespace

ProgramCoreProviderCallBinding bind_priced_provider_call(const ProgramCoreProviderCallContext& context) {
    if (context.owner_scope.empty() || context.program_version_id.empty() ||
        context.run_id.empty() || context.operation_id.empty() || !context.attempt)
        throw std::invalid_argument("Priced provider call binding requires exact Program scope and attempt");
    return {std::string(context.owner_scope), std::string(context.program_version_id),
            std::string(context.run_id),      std::string(context.operation_id),
            context.attempt, std::make_shared<PricedProviderCallBroker>(context)};
}

}  // namespace neograph::program
