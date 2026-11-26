#include "monetary.h"

#include "canonical_json.h"

#include <neograph/graph/cancel.h>
#include <neograph/types.h>

#include <descriptor/descriptor.h>
#include <descriptor/policy.h>


#include <algorithm>
#include <iterator>
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

// Catalogue amounts are exact nano-USD; a charge rounds up once to a whole microunit.
std::uint64_t ceil_microunits(std::uint64_t nano_usd) noexcept {
    return nano_usd / 1000 + (nano_usd % 1000 != 0 ? 1 : 0);
}

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > kMax - total) return false;
    total += value;
    return true;
}

std::string joined(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) out += (out.empty() ? "" : ", ") + value;
    return out;
}

json price_rows(const sp::descriptor::CatalogueEntry& entry) {
    json units = json::object();
    for (const auto& [unit, text] : entry.pricing) units[unit] = text;
    return units;
}

constexpr std::optional<std::uint64_t> sp::OpenRouterMaxPrice::* kMaxPriceFields[] = {
    &sp::OpenRouterMaxPrice::prompt_nano_usd, &sp::OpenRouterMaxPrice::completion_nano_usd,
    &sp::OpenRouterMaxPrice::image_nano_usd, &sp::OpenRouterMaxPrice::audio_nano_usd,
    &sp::OpenRouterMaxPrice::request_nano_usd};

bool same_max_price(const sp::OpenRouterMaxPrice& left, const sp::OpenRouterMaxPrice& right) {
    for (const auto member : kMaxPriceFields)
        if (left.*member != right.*member) return false;
    return true;
}

void block(MonetaryQuote& quote, std::string code, std::string message) {
    if (!quote.blocker.empty()) return;
    quote.blocker         = std::move(code);
    quote.blocker_message = std::move(message);
}

ProgramMonetaryRecord base_record(const MonetaryQuote& quote, const MonetaryCallSite& site) {
    ProgramMonetaryRecord record;
    record.call_id             = site.call_id;
    record.operation_id        = site.operation_id;
    record.node                = site.node;
    record.origin              = quote.origin;
    record.model               = quote.model;
    record.reserved_microunits = quote.hold;
    // Every record names the catalogue and rows its hold was priced with.
    record.catalog_version     = quote.catalog_version;
    record.catalog_digest      = quote.catalog_digest;
    record.pricing             = quote.pricing;
    // And the tier the call bills under, which settlement prices the usage at.
    record.billed_tier         = quote.billed_tier;
    return record;
}

ProgramMonetaryRecord held_record(const MonetaryQuote& quote, const MonetaryCallSite& site,
                                  std::string reason) {
    auto record            = base_record(quote, site);
    record.evidence        = ProgramMonetaryEvidence::Held;
    record.microunits      = quote.hold;
    record.basis           = quote.hold_basis;
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

// The longest upstream name the SDK records (ServedModel::served_by).
constexpr std::size_t kServedByLimit = 256;

// A name the record can hold: nonempty, at most the SDK's bound, UTF-8, no control character.
bool usable_served_by(const std::string& name) noexcept {
    if (name.empty() || name.size() > kServedByLimit) return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return false;
    try {
        neograph::detail::validate_utf8(name);
    } catch (...) {
        return false;
    }
    return true;
}

// The upstream an outcome names, from the completion or from what a failure kept. A name that does not meet the
// record's bound is left out: the metadata never changes what a billed call is charged.
std::optional<std::string> served_by_of(const sp::runtime::Result& outcome) {
    if (!outcome) return std::nullopt;
    const auto* completion = std::get_if<sp::Completion>(outcome.get());
    const auto& name = completion ? completion->model.served_by : std::get<sp::Failure>(*outcome).partial.model.served_by;
    if (!name || !usable_served_by(*name)) return std::nullopt;
    return name;
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

MonetaryRequestFacts monetary_request_facts(const ProviderRequest& request) {
    // The SDK reads what prices a call from the prepared request: a chat call's request facts, an endpoint call's
    // endpoint facts (assess_monetary_call).
    (void)request;
    return {};
}

#if defined(NEOGRAPH_PROGRAM_TESTING)
namespace {
std::mutex                   linked_capabilities_mutex;
std::optional<std::uint64_t> linked_capabilities_override;
}  // namespace

void set_linked_capabilities_for_testing(std::optional<std::uint64_t> capabilities) {
    std::lock_guard lock(linked_capabilities_mutex);
    linked_capabilities_override = capabilities;
}
#endif

namespace {

// The refusal code for the SDK's reason a prepared request has no cost bound.
std::string refusal_code(sp::pricing::UnboundedBy by) {
    using sp::pricing::UnboundedBy;
    switch (by) {
        case UnboundedBy::OutputCap: return "P_MONEY_NO_OUTPUT_CAP";
        case UnboundedBy::InputBound: return "P_MONEY_NO_INPUT_BOUND";
        case UnboundedBy::Invocations:
        case UnboundedBy::ModelPrice:
        case UnboundedBy::ToolCap:
        case UnboundedBy::RouteCeiling:
        case UnboundedBy::Overflow: return "P_MONEY_NO_UPPER_BOUND";
        case UnboundedBy::TierPrice:
        case UnboundedBy::ToolFee:
        case UnboundedBy::Unmodeled:
        case UnboundedBy::Billing:
        case UnboundedBy::Quantity: return "P_MONEY_UNMODELED";
    }
    return "P_MONEY_NO_UPPER_BOUND";
}

// Refuses `quote` by the first of the SDK's reasons. The witness keeps every reason as the SDK states it; the message
// is built from the reason's kind and subject and, for a server tool, from the cap the request facts describe.
void refuse(MonetaryQuote& quote, const sp::pricing::WorstCase& bound, const sp::RequestFacts& facts) {
    using sp::pricing::UnboundedBy;
    quote.hold    = 0;
    auto reasons  = json::array();
    std::vector<std::string> stated;
    for (const auto& reason : bound.unbounded) {
        const auto by = std::string(sp::pricing::to_string(reason.by));
        reasons.push_back(json{{"by", by}, {"subject", reason.subject}, {"detail", reason.detail}});
        stated.push_back(by + (reason.subject.empty() ? "" : " '" + reason.subject + "'") + ": " + reason.detail);
    }
    quote.witness["unbounded"] = std::move(reasons);
    if (bound.unbounded.empty()) {
        // No total and no reason: refused all the same.
        block(quote, "P_MONEY_NO_UPPER_BOUND", "the SDK gives the request no cost bound");
        return;
    }
    // A request without a cap on a server tool states no invocation limit either: the refusal names the cause, the
    // first reason of another kind, when there is one.
    const auto cause = std::find_if(bound.unbounded.begin(), bound.unbounded.end(),
                                    [](const auto& item) { return item.by != UnboundedBy::Invocations; });
    const auto& reason = cause != bound.unbounded.end() ? *cause : bound.unbounded.front();
    const auto  code   = refusal_code(reason.by);
    if (reason.by == UnboundedBy::ModelPrice) {
        const auto* entry = quote.catalogue->find(quote.family, quote.origin, reason.subject).entry;
        if (!entry || entry->pricing.empty()) {
            block(quote, "P_MONEY_UNKNOWN_MODEL",
                  "the model catalogue has no price for model '" + reason.subject + "' at " + quote.origin);
            return;
        }
    }
    if (reason.by == UnboundedBy::ToolCap || reason.by == UnboundedBy::ToolFee)
        quote.witness["unmodeled"] = "server tool '" + reason.subject + "'";
    if (reason.by == UnboundedBy::Unmodeled) quote.witness["unmodeled"] = reason.subject;
    if (reason.by == UnboundedBy::ToolCap) {
        for (const auto& group : facts.tools) {
            if (group.uses || group.types.empty() || group.types.front() != reason.subject) continue;
            const auto why = group.member.empty()   ? std::string("takes no use cap")
                             : !group.cap_admitted ? "admits no " + group.member
                                                   : "has no use cap (" + group.member + ")";
            block(quote, code, "server tool '" + reason.subject + "' " + why + ", so its fees have no upper bound");
            return;
        }
    }
    block(quote, code, "the SDK gives the request no cost bound (" + joined(stated) + ")");
}

// The capability bits of the linked SchemaProvider library.
std::uint64_t linked_capabilities() {
#if defined(NEOGRAPH_PROGRAM_TESTING)
    {
        std::lock_guard lock(linked_capabilities_mutex);
        if (linked_capabilities_override) return *linked_capabilities_override;
    }
#endif
    return sp::runtime::interface_contract().capabilities;
}

// Refuses the endpoint call `quote` by the first of the SDK's reasons, as refuse() does for a chat call. The witness
// keeps every reason as the SDK states it.
void refuse_endpoint(MonetaryQuote& quote, const sp::pricing::EndpointBound& bound, const sp::EndpointFacts& facts) {
    using sp::pricing::UnboundedBy;
    quote.hold   = 0;
    auto reasons = json::array();
    std::vector<std::string> stated;
    for (const auto& reason : bound.unbounded) {
        const auto by = std::string(sp::pricing::to_string(reason.by));
        reasons.push_back(json{{"by", by}, {"subject", reason.subject}, {"detail", reason.detail}});
        stated.push_back(by + (reason.subject.empty() ? "" : " '" + reason.subject + "'") + ": " + reason.detail);
    }
    quote.witness["unbounded"] = std::move(reasons);
    if (bound.unbounded.empty()) {
        block(quote, "P_MONEY_NO_UPPER_BOUND", "the SDK gives the endpoint call no cost bound");
        return;
    }
    const auto cause = std::find_if(bound.unbounded.begin(), bound.unbounded.end(),
                                    [](const auto& item) { return item.by != UnboundedBy::Invocations; });
    const auto& reason = cause != bound.unbounded.end() ? *cause : bound.unbounded.front();
    // A price the catalogue lacks is the model's only when the reason names the model the call bills; an endpoint
    // call's other prices are its units.
    if (reason.by == UnboundedBy::ModelPrice && !facts.model.empty() && reason.subject == facts.model) {
        const auto* entry = quote.catalogue->find(facts.family, facts.origin, facts.model).entry;
        if (!entry || entry->pricing.empty()) {
            block(quote, "P_MONEY_UNKNOWN_MODEL",
                  "the model catalogue has no price for model '" + facts.model + "' at " + quote.origin);
            return;
        }
    }
    if (reason.by == UnboundedBy::Unmodeled || reason.by == UnboundedBy::Billing)
        quote.witness["unmodeled"] = reason.subject;
    block(quote, refusal_code(reason.by), "the SDK gives the endpoint call no cost bound (" + joined(stated) + ")");
}

// The facts of an endpoint call its record keeps beside the entry and rows (ProgramMonetaryRecord::pricing): the
// policy fact and operation it reached and, for a call that bills its model, each line of its bound (the unit, the
// price key that priced it, the quantity and its rate in nano-USD per million units). A call that bills the requests
// it carries keeps how many it carries, since each of them is bounded on its own.
json endpoint_pricing(const sp::EndpointFacts& facts, const sp::pricing::EndpointBound& bound) {
    json value{{"endpoint", facts.endpoint}, {"operation", facts.operation}};
    if (facts.billing == sp::descriptor::EndpointBilling::Carried) {
        value["requests"] = static_cast<std::uint64_t>(facts.carried.size());
        return value;
    }
    auto lines = json::array();
    for (const auto& line : bound.lines)
        lines.push_back(json{{"unit", line.unit}, {"row", line.row}, {"quantity", line.quantity}, {"rate", line.rate}});
    value["lines"] = std::move(lines);
    return value;
}

// Holds the endpoint call `quote` at the SDK's bound of the prepared call.
void assess_endpoint_call(MonetaryQuote& quote, const PreparedProviderRequest& prepared) {
    const auto& facts    = Provider::endpoint_facts(prepared);
    quote.endpoint_facts = facts;
    quote.witness["endpoint"]  = facts.endpoint;
    quote.witness["operation"] = facts.operation;
    quote.billed_tier =
        facts.billing == sp::descriptor::EndpointBilling::Carried ? facts.carried_tier : facts.service_tier;
    if (facts.billing == sp::descriptor::EndpointBilling::None) return;
    // A library built before the endpoint cost bounds answers every billed endpoint call without one, so the bound
    // is read only from a library that grants them.
    if ((linked_capabilities() & sp::capability::EndpointCostBounds) == 0) {
        quote.witness["unmodeled"] = "endpoint cost bounds";
        block(quote, "P_MONEY_UNMODELED",
              "the linked SchemaProvider library does not bound endpoint calls (capability EndpointCostBounds)");
        return;
    }
    if (!facts.input_file.empty()) {
        // An OpenAI batch submit: its requests are lines of an uploaded file, which the submit does not carry.
        quote.witness["unmodeled"] = "batch requests in an uploaded file";
        block(quote, "P_MONEY_UNMODELED",
              "the batch's requests are in an uploaded file, so the submit does not bound what the batch bills");
        return;
    }
    const auto bound    = Provider::endpoint_worst_case_cost(prepared, *quote.catalogue);
    quote.route_ceiling = bound.route_ceiling;
    if (!bound.total || !bound.unbounded.empty()) {
        refuse_endpoint(quote, bound, facts);
        return;
    }
    if (bound.route_ceiling && !bound.pin) {
        // A batch at a routing gateway is bounded at the route ceilings of its requests, which no request member
        // makes the gateway keep to.
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the endpoint call is bounded at a routing gateway's route ceilings, which the request cannot pin");
        return;
    }
    quote.hold       = ceil_microunits(bound.total->nano_usd);
    quote.hold_basis = bound.route_ceiling ? "route_ceiling" : "list_rates";
    quote.pin        = bound.pin;
    quote.pinned     = bound.pinned;
    quote.pricing    = json::object();
    if (!bound.entry.empty()) {
        const auto* entry = quote.catalogue->entry(bound.entry);
        if (!entry) entry = quote.catalogue->entry(bound.entry, true);
        if (!entry) {
            quote.hold = 0;
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the cost bound of the endpoint call names no catalogue entry to hold it by");
            return;
        }
        quote.pricing["entry"] = entry->id;
        quote.pricing["units"] = price_rows(*entry);
        quote.witness["entry"] = entry->id;
    }
    if (!quote.billed_tier.empty()) quote.pricing["service_tier"] = quote.billed_tier;
    quote.pricing["endpoint"] = endpoint_pricing(facts, bound);
    // Whatever settles the call, its record keeps this pricing, which a record holds at most 16 KiB of.
    if (neograph::detail::canonical_json_bytes(quote.pricing).size() > 16384) {
        quote.hold = 0;
        block(quote, "P_MONEY_UNMODELED", "the pricing of the endpoint call is larger than its record can keep");
        return;
    }
    quote.witness["attempts"]            = bound.attempts;
    quote.witness["reserved_microunits"] = quote.hold;
    quote.witness["basis"]               = quote.hold_basis;
}

}  // namespace

MonetaryQuote assess_monetary_call(const MonetaryRequestFacts&    facts,
                                   const PreparedProviderRequest& prepared) {
    MonetaryQuote quote;
    quote.model   = std::string(prepared.model());
    quote.family  = std::string(prepared.family());
    quote.witness = json{{"model", quote.model}};
    const auto* descriptor = prepared.admitted_descriptor();
    if (!descriptor) {
        block(quote, "P_MONEY_UNKNOWN_ORIGIN",
              "the provider has no admitted descriptor, so its model catalogue is unknown");
        return quote;
    }
    const auto& policy      = *descriptor->policy();
    quote.catalogue         = policy.catalogue();
    quote.origin            = std::string(descriptor->base_url());
    quote.catalog_version   = std::string(kCatalogueScheme);
    quote.catalog_digest    = quote.catalogue->digest_hex();
    quote.witness["origin"]          = quote.origin;
    quote.witness["catalog_version"] = quote.catalog_version;
    quote.witness["catalog_digest"]  = quote.catalog_digest;
    if (facts.unpriced) {
        quote.witness["unmodeled"] = *facts.unpriced;
        block(quote, "P_MONEY_UNMODELED",
              "the request uses " + *facts.unpriced + ", which the model catalogue cannot price");
        return quote;
    }
    if (prepared.is_endpoint()) {
        assess_endpoint_call(quote, prepared);
        return quote;
    }
    // The request's service tier as the policy maps it ("" for the standard rates). Settlement prices the usage under
    // it (settle_amount).
    const auto& request = Provider::request_facts(prepared);
    quote.service_tier  = std::string(policy.billing_tier(quote.family, request.service_tier));
    if (!quote.service_tier.empty()) quote.witness["service_tier"] = quote.service_tier;
    // Settlement prices the usage with the same facts as the hold below, so under the same tier.
    quote.request_facts = request;

    // The SDK's worst case of the prepared request over the catalogue the hold names: every model invocation and
    // transport attempt the request may bill, the tier it bills under (its priced extensions included), its fallback
    // models, server tools and their caps, its routing and what it carries (an image).
    const auto bound    = Provider::worst_case_cost(prepared, *quote.catalogue);
    quote.route_ceiling = bound.route_ceiling;
    if (!bound.total || !bound.unbounded.empty()) {
        refuse(quote, bound, request);
        return quote;
    }
    const auto* entry = quote.catalogue->find(quote.family, quote.origin, bound.model).entry;
    if (!entry || entry->id != bound.entry || (bound.route_ceiling && !bound.pin)) {
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the cost bound of model '" + quote.model + "' names no catalogue entry or route ceiling pin to hold it by");
        return quote;
    }
    // A bound always has a billed tier: the SDK refuses a request whose extensions name none.
    quote.billed_tier = sp::pricing::billed_tier(request, quote.service_tier).tier;
    quote.hold               = ceil_microunits(bound.total->nano_usd);
    quote.hold_basis         = bound.route_ceiling ? "route_ceiling" : "list_rates";
    quote.pin                = bound.pin;
    quote.pinned             = bound.pinned;
    quote.tool_fees_nano_usd = bound.tool_fees_nano_usd;
    quote.pricing            = json{{"entry", entry->id}, {"units", price_rows(*entry)}};
    if (!quote.service_tier.empty()) quote.pricing["service_tier"] = quote.service_tier;
    if (!bound.tool_uses.empty()) {
        auto tools = json::object();
        for (const auto& [type, uses] : bound.tool_uses) tools[type] = uses;
        quote.pricing["tool_uses"] = std::move(tools);
    }
    quote.witness["entry"]               = entry->id;
    quote.witness["input_tokens_bound"]  = bound.input_tokens;
    quote.witness["output_tokens_bound"] = bound.output_tokens;
    quote.witness["invocations"]         = bound.invocations;
    quote.witness["attempts"]            = bound.attempts;
    quote.witness["reserved_microunits"] = quote.hold;
    quote.witness["basis"]               = quote.hold_basis;
    return quote;
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

namespace {

// The quote of a request about to be sent. A bound at the route ceiling holds only when the request carries its pin.
MonetaryQuote sent_quote(const MonetaryRequestFacts& facts, const PreparedProviderRequest& prepared) {
    auto quote = assess_monetary_call(facts, prepared);
    if (quote.blocker.empty() && quote.pin && !quote.pinned) {
        quote.hold = 0;
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the request does not carry the route ceiling it is held at as provider.max_price");
    }
    return quote;
}

}  // namespace

namespace {

// prepare_priced_call() for an endpoint call: prepared as it is, then, when its bound at a routing gateway reads a
// price cap above the route ceiling, prepared again with the cap lowered to it, once. The quote is the bound of the
// request that is sent. A call whose operation the policy states the provider does not bill is not priced.
PricedPreparation prepare_priced_endpoint_call(Provider& provider, ProviderRequest request) {
    const auto facts   = monetary_request_facts(request);
    auto       payload = request.endpoint;
    auto       pinnable = request;
    auto       prepared = provider.prepare(std::move(request));
    if (!prepared.valid()) return {std::move(prepared), std::nullopt};
    auto quote = assess_monetary_call(facts, prepared);
    if (quote.blocker.empty() && quote.pin && !quote.pinned) {
        if (auto pinned = payload->with_max_price(*quote.pin)) {
            payload           = std::move(pinned);
            pinnable.endpoint = payload;
            prepared          = PreparedProviderRequest{};  // release its operation slot first
            prepared          = provider.prepare(std::move(pinnable));
            if (!prepared.valid()) return {std::move(prepared), std::move(quote)};
        }
        quote = sent_quote(facts, prepared);
    }
    if (quote.blocker.empty() && quote.endpoint_facts &&
        quote.endpoint_facts->billing == sp::descriptor::EndpointBilling::None)
        return {std::move(prepared), std::nullopt};
    quote.endpoint_payload = std::move(payload);
    return {std::move(prepared), std::move(quote)};
}

}  // namespace

PricedPreparation prepare_priced_call(Provider& provider, ProviderRequest request, bool priced) {
    if (!priced) return {provider.prepare(std::move(request)), std::nullopt};
    if (request.endpoint) return prepare_priced_endpoint_call(provider, std::move(request));
    const auto facts = monetary_request_facts(request);
    std::optional<MonetaryQuote> probed_quote;
    {
        // A probe with a one-message conversation gives the route ceiling pin. It carries no native history, whose
        // routing binding would refuse an unpinned turn after a pinned one. It carries no image and no continuation
        // either, so the hold is the bound of the request that is sent, not the probe's.
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
        if (probed.valid()) probed_quote = assess_monetary_call(facts, probed);
    }  // The probe's operation slot is released here, unsent.
    if (probed_quote) {
        if (probed_quote->pin) (void)pin_route_max_price(request, *probed_quote->pin);
        auto prepared = provider.prepare(std::move(request));
        if (!prepared.valid()) return {std::move(prepared), std::move(probed_quote)};
        auto quote = sent_quote(facts, prepared);
        return {std::move(prepared), std::move(quote)};
    }
    // The provider does not admit the probe: price the request's own preparation.
    auto pinnable = request;
    auto prepared = provider.prepare(std::move(request));
    if (!prepared.valid()) return {std::move(prepared), std::nullopt};
    auto quote = assess_monetary_call(facts, prepared);
    if (quote.pin && pin_route_max_price(pinnable, *quote.pin)) {
        prepared = PreparedProviderRequest{};  // release its operation slot first
        prepared = provider.prepare(std::move(pinnable));
        if (!prepared.valid()) return {std::move(prepared), std::move(quote)};
    }
    quote = sent_quote(facts, prepared);
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
    return ceil_microunits(nano);
}

namespace {

bool assumption_is_conservative(std::string_view rule) noexcept {
    return rule == "cache_read at input" || rule == "cache_read at cache_read_audio" ||
           rule == "cache_write at cache_write_1h" || rule == "output at output_image" ||
           rule == "route ceiling" || rule == "input at the pin" ||
           rule == "input at input_image" || rule == "cache_read at input_image" ||
           rule == "cache_write at input_image" || rule.ends_with(" at the request's bound");
}

bool assumption_may_undercount(std::string_view rule) noexcept {
    return rule == "cache_write at input" || rule == "cache_write_1h at cache_write";
}

// The record of one dispatched endpoint call's completed outcome: the provider's reported cost, else the SDK's
// estimate of its typed result from the facts it was prepared with, else its hold.
ProgramMonetaryRecord settle_endpoint_amount(const MonetaryQuote& quote, const MonetaryCallSite& site,
                                             const sp::Outcome& outcome, const sp::Completion& completion) {
    if (completion.usage.stage == sp::UsageStage::Partial)
        return held_record(quote, site, "incomplete_usage");
    if (completion.usage.stage == sp::UsageStage::Final) {
        if (const auto reported = reported_cost_microunits(completion.usage.provider_cost)) {
            auto record       = base_record(quote, site);
            record.evidence   = ProgramMonetaryEvidence::Reported;
            record.microunits = *reported;
            record.basis      = "provider_reported";
            return record;
        }
    }
    if (!quote.catalogue) return held_record(quote, site, "the hold names no catalogue");
    if (!quote.endpoint_payload || !quote.endpoint_facts)
        return held_record(quote, site, "the hold names no endpoint request to type the result by");
    const auto estimate = quote.endpoint_payload->estimate(*quote.endpoint_facts, outcome, *quote.catalogue);
    if (!estimate) return held_record(quote, site, "the SDK has no estimate of this endpoint call's result");
    if (!estimate->total)
        return held_record(quote, site, "the catalogue has no price for " + joined(estimate->unpriced));
    if (estimate->lower_bound) return held_record(quote, site, "the provider may bill more than the result counted");
    auto record       = base_record(quote, site);
    record.evidence   = ProgramMonetaryEvidence::Estimated;
    record.microunits = ceil_microunits(estimate->total->nano_usd);
    record.basis      = quote.hold_basis;
    record.conservative =
        std::any_of(estimate->assumptions.begin(), estimate->assumptions.end(), assumption_is_conservative);
    // At a routing gateway the estimate takes the entry's listed rates, and the endpoint the gateway chose may bill
    // more, up to the cap the request was held at.
    record.may_undercount =
        quote.route_ceiling ||
        std::any_of(estimate->assumptions.begin(), estimate->assumptions.end(), assumption_may_undercount);
    if (estimate->entry != quote.pricing.value("entry", std::string())) {
        // The served model's entry priced the call.
        const auto* entry = quote.catalogue->entry(estimate->entry);
        if (!entry) entry = quote.catalogue->entry(estimate->entry, true);
        record.pricing["entry"] = estimate->entry;
        record.pricing["units"] = entry ? price_rows(*entry) : json::object();
    }
    if (!estimate->assumptions.empty()) record.pricing["assumptions"] = estimate->assumptions;
    return record;
}

// The record of one dispatched call's outcome, without the serving upstream.
std::optional<ProgramMonetaryRecord> settle_amount(const MonetaryQuote&       quote,
                                                   const MonetaryCallSite&    site,
                                                   const sp::runtime::Result& outcome) {
    if (!outcome) return unknown_outcome_record(quote, site, "no outcome");
    if (const auto* failure = std::get_if<sp::Failure>(outcome.get())) {
        if (provider_failure_proves_not_accepted(*failure)) return std::nullopt;
        return held_record(quote, site, "failed after the request may have been sent");
    }
    const auto& completion = std::get<sp::Completion>(*outcome);
    if (completion.attempt.prior_usage_unknown || completion.attempt.transport_internal_resends != 0)
        return held_record(quote, site, "the usage of resent transport attempts is unknown");
    if (quote.endpoint_facts) return settle_endpoint_amount(quote, site, *outcome, completion);
    const auto& usage = completion.usage;
    if (usage.stage != sp::UsageStage::Final) return held_record(quote, site, "incomplete_usage");
    if (const auto reported = reported_cost_microunits(usage.provider_cost)) {
        auto record       = base_record(quote, site);
        record.evidence   = ProgramMonetaryEvidence::Reported;
        record.microunits = *reported;
        record.basis      = "provider_reported";
        return record;
    }
    if (!usage.input_total || !usage.output_total) return held_record(quote, site, "incomplete_usage");
    if (usage.quality != sp::UsageQuality::Consistent) return held_record(quote, site, "inconsistent_usage");
    if (!quote.catalogue) return held_record(quote, site, "the hold names no catalogue");
    // The snapshot the hold was priced with, never a newer one, and the facts of the request that was sent, so the
    // tier its priced extensions bill under prices the usage as it bounded the hold. A response that names no model
    // was answered by the admitted one.
    auto model = completion.model;
    if (model.requested.empty()) model.requested = quote.model;
    const auto estimate = sp::pricing::estimate(*quote.catalogue, quote.family, quote.origin, model, usage,
                                                quote.request_facts, quote.service_tier, quote.route_ceiling);
    if (!estimate.total)
        return held_record(quote, site, "the catalogue has no price for " + joined(estimate.unpriced));
    // Server tools are charged at their reserved cap: the usage does not count their uses.
    auto nano = estimate.total->nano_usd;
    if (!checked_add(nano, quote.tool_fees_nano_usd)) return held_record(quote, site, "overflow");
    auto record            = base_record(quote, site);
    record.evidence        = ProgramMonetaryEvidence::Estimated;
    record.microunits      = ceil_microunits(nano);
    record.basis           = quote.hold_basis;
    record.conservative    = quote.tool_fees_nano_usd != 0 ||
                             std::any_of(estimate.assumptions.begin(), estimate.assumptions.end(),
                                         assumption_is_conservative);
    record.may_undercount =
        std::any_of(estimate.assumptions.begin(), estimate.assumptions.end(), assumption_may_undercount);
    if (estimate.entry != quote.pricing.value("entry", std::string())) {
        // The served model's entry priced the call.
        const auto* entry = quote.catalogue->entry(estimate.entry);
        if (!entry) entry = quote.catalogue->entry(estimate.entry, true);
        record.pricing["entry"] = estimate.entry;
        record.pricing["units"] = entry ? price_rows(*entry) : json::object();
    }
    if (!estimate.assumptions.empty()) record.pricing["assumptions"] = estimate.assumptions;
    return record;
}

}  // namespace

std::optional<ProgramMonetaryRecord> settle_monetary_call(const MonetaryQuote&       quote,
                                                          const MonetaryCallSite&    site,
                                                          const sp::runtime::Result& outcome) {
    auto record = settle_amount(quote, site, outcome);
    // Whatever the record charges, it also names the upstream the response named. Nothing else of the response is kept.
    if (record) record->served_by = served_by_of(outcome);
    return record;
}

ProgramMonetaryRecord unknown_outcome_record(const MonetaryQuote& quote, const MonetaryCallSite& site,
                                             std::string reason) {
    return held_record(quote, site, std::move(reason));
}

json encode_monetary_record(const ProgramMonetaryRecord& record) {
    json value{{"call_id", record.call_id},
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
    // Records before 0.16.0 have no pricing and keep their exact bytes.
    if (!record.pricing.empty()) value["pricing"] = record.pricing;
    // Likewise a record without a serving upstream keeps its exact bytes.
    if (record.served_by) value["served_by"] = *record.served_by;
    // And a call billed at the standard rates, as every record before the tier was kept.
    if (!record.billed_tier.empty()) value["billed_tier"] = record.billed_tier;
    return value;
}

ProgramMonetaryRecord decode_monetary_record(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Program monetary record must be an object");
    neograph::detail::reject_unknown_fields(
        value, "Program monetary record",
        {"call_id", "operation_id", "node", "origin", "model", "evidence", "microunits",
         "reserved_microunits", "basis", "catalog_version", "catalog_digest", "conservative",
         "may_undercount", "replayed", "reason", "pricing", "served_by", "billed_tier"});
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
    if (value.contains("pricing")) record.pricing = value.at("pricing");
    if (value.contains("served_by")) record.served_by = string_field(value, "served_by");
    // Written only when not empty: a stored empty tier is not a record this code writes.
    if (value.contains("billed_tier")) {
        record.billed_tier = string_field(value, "billed_tier");
        if (record.billed_tier.empty())
            throw std::invalid_argument("Program monetary record billed_tier is empty");
    }
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
    bounded_text(record.billed_tier, "billed_tier", true);
    if (!record.pricing.is_object() || neograph::detail::canonical_json_bytes(record.pricing).size() > 16384)
        throw std::invalid_argument("Program monetary record pricing must be an object of at most 16 KiB");
    if (record.served_by && !usable_served_by(*record.served_by))
        throw std::invalid_argument(
            "Program monetary record served_by is empty, too long, not UTF-8 or contains a control character");
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

std::optional<ProgramCoreProviderCallDiagnostic> over_hold_notice(const ProgramMonetaryRecord& record) {
    if (record.evidence == ProgramMonetaryEvidence::Held || record.microunits <= record.reserved_microunits)
        return std::nullopt;
    const auto over_by = record.microunits - record.reserved_microunits;
    json witness{{"call_id", record.call_id},
                 {"operation_id", record.operation_id},
                 {"node", record.node},
                 {"model", record.model},
                 {"evidence", std::string(to_string(record.evidence))},
                 {"reserved_microunits", record.reserved_microunits},
                 {"microunits", record.microunits},
                 {"over_by", over_by}};
    // The upstream the record names, when it names one: the one a host leaves out of its routing.
    if (record.served_by) witness["served_by"] = *record.served_by;
    return ProgramCoreProviderCallDiagnostic{
        std::string(kOverHoldNotice),
        "A priced call was charged " + std::to_string(record.microunits) +
            " microunits, above its reservation of " + std::to_string(record.reserved_microunits) + ".",
        std::move(witness)};
}

MonetaryHold::MonetaryHold(std::shared_ptr<ProgramMonetaryAccount> account, MonetaryQuote quote,
                           MonetaryCallSite site, DiagnosticSink diagnostic)
    : account_(std::move(account)), quote_(std::move(quote)), site_(std::move(site)),
      diagnostic_(std::move(diagnostic)) {}

MonetaryHold::MonetaryHold(MonetaryHold&& other) noexcept
    : account_(std::move(other.account_)), quote_(std::move(other.quote_)),
      site_(std::move(other.site_)), diagnostic_(std::move(other.diagnostic_)),
      reserved_(std::exchange(other.reserved_, 0)),
      dispatched_(other.dispatched_), done_(std::exchange(other.done_, true)) {}

MonetaryHold& MonetaryHold::operator=(MonetaryHold&& other) noexcept {
    if (this != &other) {
        finish();
        account_    = std::move(other.account_);
        quote_      = std::move(other.quote_);
        site_       = std::move(other.site_);
        diagnostic_ = std::move(other.diagnostic_);
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
    return settle_monetary_call(quote_, site_, outcome);
}

void MonetaryHold::settle(const sp::runtime::Result& outcome) { settle_record(record_for(outcome)); }

void MonetaryHold::settle_record(std::optional<ProgramMonetaryRecord> record) {
    if (!account_ || done_) return;
    done_ = true;
    if (!record) {
        account_->release(reserved_);
        return;
    }
    auto notice = diagnostic_ ? over_hold_notice(*record) : std::nullopt;
    account_->settle(reserved_, std::move(*record));
    if (!notice) return;
    try {
        diagnostic_(std::move(*notice));
    } catch (...) {
        // A notice is not part of the outcome: the call is charged either way.
    }
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
          account_(context.monetary_account), diagnostic_(context.diagnostic) {}

    asio::awaitable<sp::runtime::Result> invoke(graph::ProviderCallIdentity identity,
                                               std::shared_ptr<Provider>   provider,
                                               ProviderRequest             request) override {
        return invoke_owned(owner_, version_, run_, operation_, attempt_, account_, diagnostic_,
                            std::move(identity), std::move(provider), std::move(request));
    }

private:
    static asio::awaitable<sp::runtime::Result> invoke_owned(
        std::string owner, std::string version, std::string run, std::string operation,
        std::uint64_t attempt, std::shared_ptr<ProgramMonetaryAccount> account,
        std::function<void(ProgramCoreProviderCallDiagnostic)> diagnostic,
        graph::ProviderCallIdentity identity, std::shared_ptr<Provider> provider,
        ProviderRequest request) {
        if (!provider || identity.owner_scope != owner || identity.run_id != run ||
            identity.thread_id.empty() || identity.task_id.empty() || identity.node_name.empty())
            throw std::invalid_argument("Program provider call scope is invalid");
        auto priced = detail::prepare_priced_call(*provider, std::move(request), account != nullptr);
        auto prepared = std::move(priced.prepared);
        if (!prepared.valid()) co_return detail::preparation_failure_outcome(prepared);
        detail::MonetaryHold hold;
        if (priced.quote) {
            const ProgramCoreProviderCallContext context{owner, version, run, operation, attempt};
            hold = detail::MonetaryHold(
                account, std::move(*priced.quote),
                detail::MonetaryCallSite{detail::provider_call_slot_id(context, identity),
                                         operation, identity.node_name},
                std::move(diagnostic));
            if (auto refused = hold.preflight()) co_return refused;
        }
        ProviderBudgetClaim claim;
        try {
            ProviderDispatchBudget budget{identity.usage, identity.model_token_budget,
                                          identity.budget_exhausted, identity.budget_cancel_token};
            budget.model_token_budget_bounded = identity.model_token_budget_bounded;
            claim = reserve_provider_dispatch(prepared, std::move(budget));
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
    std::function<void(ProgramCoreProviderCallDiagnostic)> diagnostic_;
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
