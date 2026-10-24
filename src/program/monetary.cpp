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

// Service tier names that mean the standard tier, priced by the catalogue
// units that name no tier.
bool standard_service_tier(std::string_view tier) {
    return tier == "default" || tier == "standard" || tier == "standard_only";
}

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

// False for every T: a tool variant without a branch below fails to compile
// instead of being priced as another tool.
template <class>
inline constexpr bool kUnpricedToolVariant = false;

// The wire type of a Responses hosted tool, the `tool:<type>` its fee is listed under.
std::string hosted_tool_type(const sp::responses::HostedTool& tool) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, sp::responses::WebSearchTool>) return "web_search";
            else if constexpr (std::is_same_v<T, sp::responses::ImageGenerationTool>) return "image_generation";
            else if constexpr (std::is_same_v<T, sp::responses::FileSearchTool>) return "file_search";
            else if constexpr (std::is_same_v<T, sp::responses::ToolSearchTool>) return "tool_search";
            else if constexpr (std::is_same_v<T, sp::responses::ShellTool>) return "shell";
            else if constexpr (std::is_same_v<T, sp::responses::CodeInterpreterTool>) return "code_interpreter";
            else if constexpr (std::is_same_v<T, sp::responses::McpTool>) return "mcp";
            else if constexpr (std::is_same_v<T, sp::responses::ComputerTool>) return "computer";
            else if constexpr (std::is_same_v<T, sp::responses::ComputerUsePreviewTool>) return "computer_use_preview";
            else if constexpr (std::is_same_v<T, sp::responses::OpenRouterServerTool> ||
                               std::is_same_v<T, sp::responses::GenericHostedTool>) return item.type;
            else static_assert(kUnpricedToolVariant<T>, "a Responses hosted tool has no wire type here");
        },
        tool);
}

// The wire type of a Gemini built-in tool (Generate or Interactions).
template <class Tool>
std::string builtin_tool_type(const Tool& tool) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, sp::gemini::GoogleSearchTool> ||
                          std::is_same_v<T, sp::interactions::GoogleSearchTool>) return "google_search";
            else if constexpr (std::is_same_v<T, sp::gemini::UrlContextTool> ||
                               std::is_same_v<T, sp::interactions::UrlContextTool>) return "url_context";
            else if constexpr (std::is_same_v<T, sp::gemini::CodeExecutionTool> ||
                               std::is_same_v<T, sp::interactions::CodeExecutionTool>) return "code_execution";
            else if constexpr (std::is_same_v<T, sp::gemini::GoogleMapsTool>) return "google_maps";
            else if constexpr (std::is_same_v<T, sp::gemini::ComputerUseTool>) return "computer_use";
            else if constexpr (std::is_same_v<T, sp::gemini::FileSearchTool>) return "file_search";
            else static_assert(kUnpricedToolVariant<T>, "a Gemini built-in tool has no wire type here");
        },
        tool);
}

// The policy's fact for a server tool type of the family, if it lists one.
const sp::descriptor::ServerToolFact* server_tool_fact(const sp::descriptor::FamilyPolicy* family,
                                                       std::string_view                    type) {
    if (!family) return nullptr;
    const auto found = std::find_if(family->server_tools.begin(), family->server_tools.end(),
                                    [&](const auto& fact) { return fact.type == type; });
    return found == family->server_tools.end() ? nullptr : &*found;
}

json price_rows(const sp::descriptor::CatalogueEntry& entry) {
    json units = json::object();
    for (const auto& [unit, text] : entry.pricing) units[unit] = text;
    return units;
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
    MonetaryRequestFacts facts;
    // The catalogue prices chat tokens only; nothing bounds an endpoint request yet (SchemaProvider#124).
    if (request.endpoint) {
        facts.unpriced = "SchemaProvider endpoint calls";
        return facts;
    }
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (requires { payload.service_tier; }) facts.service_tier = payload.service_tier;
            if constexpr (requires { payload.plugins; }) {
                if (!payload.plugins.empty()) facts.unpriced = "OpenRouter plugins";
            }
            if constexpr (std::is_same_v<T, sp::chat::Request>) facts.fallback_models = payload.models;
            if constexpr (std::is_same_v<T, sp::responses::Request>) {
                if (!payload.hosted_tools.empty()) {
                    MonetaryRequestFacts::ToolCap cap;
                    cap.uses   = payload.max_tool_calls;
                    cap.member = "max_tool_calls";
                    for (const auto& tool : payload.hosted_tools) cap.types.push_back(hosted_tool_type(tool));
                    facts.tools.push_back(std::move(cap));
                }
            }
            if constexpr (std::is_same_v<T, sp::messages::Request>) {
                // A typed tool is a server tool unless the family policy marks it a client tool;
                // assess_monetary_call() reads that fact, as the request is not prepared yet.
                for (const auto& tool : payload.tools)
                    if (!tool.type.empty()) facts.tools.push_back({{tool.type}, tool.max_uses, "max_uses"});
            }
            if constexpr (std::is_same_v<T, sp::gemini::Request> || std::is_same_v<T, sp::interactions::Request>) {
                // Gemini's built-in tools take no use cap.
                for (const auto& tool : payload.builtin_tools)
                    facts.tools.push_back({{builtin_tool_type(tool)}, std::nullopt, {}});
            }
            if constexpr (std::is_same_v<T, sp::gemini::Request>) {
                for (const auto& server : payload.mcp_servers)
                    facts.tools.push_back({{"mcp:" + server.name}, std::nullopt, {}});
            }
        },
        request.payload);
    return facts;
}

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
    quote.route_ceiling     = sp::descriptor::routed_gateway(*descriptor);
    quote.witness["origin"]          = quote.origin;
    quote.witness["catalog_version"] = quote.catalog_version;
    quote.witness["catalog_digest"]  = quote.catalog_digest;
    if (facts.unpriced) {
        quote.witness["unmodeled"] = *facts.unpriced;
        block(quote, "P_MONEY_UNMODELED",
              "the request uses " + *facts.unpriced + ", which the model catalogue cannot price");
        return quote;
    }
    auto tier = facts.service_tier;
    if (!tier) {
        const auto defaults = sp::descriptor::effective_defaults(*descriptor, quote.model);
        if (defaults.service_tier) tier = std::string(*defaults.service_tier);
    }
    if (tier && !standard_service_tier(*tier)) {
        quote.service_tier             = *tier;
        quote.witness["service_tier"] = quote.service_tier;
    }
    const auto output = prepared.max_output_tokens();
    if (!output) {
        block(quote, "P_MONEY_NO_OUTPUT_CAP",
              "the request has no output token cap, so its cost has no upper bound");
        return quote;
    }
    // The tools the service runs. The family policy marks the ones the caller runs
    // (Messages' memory, bash and toolsets): they bill only tokens, and take no cap.
    const auto* family_policy = policy.family(quote.family);
    std::vector<MonetaryRequestFacts::ToolCap> server_tools;
    for (const auto& cap : facts.tools) {
        MonetaryRequestFacts::ToolCap kept{{}, cap.uses, cap.member};
        for (const auto& type : cap.types) {
            const auto* fact = server_tool_fact(family_policy, type);
            if (!fact || !fact->client) kept.types.push_back(type);
        }
        if (!kept.types.empty()) server_tools.push_back(std::move(kept));
    }
    for (const auto& cap : server_tools) {
        if (cap.uses) continue;
        const auto& type = cap.types.front();
        const auto* fact = server_tool_fact(family_policy, type);
        quote.witness["unmodeled"] = "server tool '" + type + "'";
        std::string reason;
        if (cap.member.empty())
            reason = "takes no use cap";
        else if (cap.member == "max_uses" && fact && !fact->max_uses)
            reason = "admits no max_uses";
        else
            reason = "has no use cap (" + cap.member + ")";
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "server tool '" + type + "' " + reason + ", so its fees have no upper bound");
        return quote;
    }
    const auto invocations = Provider::invocation_upper_bound(prepared);
    if (!invocations) {
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the prepared request does not state how many invocations it can bill");
        return quote;
    }

    // Every model that may serve the call: the admitted one, then OpenRouter's fallbacks.
    struct Candidate {
        std::string                           model;
        const sp::descriptor::CatalogueEntry* entry = nullptr;
        std::uint64_t                         input = 0;
        sp::pricing::ModelPrice               price;
    };
    std::vector<Candidate> candidates;
    std::vector<std::string> models{quote.model};
    for (const auto& model : facts.fallback_models)
        if (std::find(models.begin(), models.end(), model) == models.end()) models.push_back(model);
    for (const auto& model : models) {
        const auto* entry = policy.model(quote.family, quote.origin, model).entry;
        if (!entry || entry->pricing.empty()) {
            block(quote, "P_MONEY_UNKNOWN_MODEL",
                  "the model catalogue has no price for model '" + model + "' at " + quote.origin);
            return quote;
        }
        const auto input = min_known({policy.input_limit(quote.family, quote.origin, model), entry->context_window});
        if (!input) {
            block(quote, "P_MONEY_NO_INPUT_BOUND",
                  "no input token limit is known for model '" + model + "', so its cost has no upper bound");
            return quote;
        }
        auto price = sp::pricing::dearest_price_of(*entry, quote.service_tier, *input);
        if (price.empty()) {
            block(quote, "P_MONEY_UNMODELED",
                  "catalogue entry '" + entry->id + "' prices no service tier '" + quote.service_tier + "'");
            return quote;
        }
        const bool has_ceiling = price.ceiling_input || price.ceiling_output || price.ceiling_request;
        if (!quote.route_ceiling && has_ceiling) {
            // The bound holds only if the router cannot pick a dearer endpoint,
            // so the ceiling must be pinned as OpenRouter's provider.max_price.
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the catalogue gives model '" + model + "' a route ceiling, but the SDK policy does not admit "
                  "OpenRouter routing at " + quote.origin + ", so it cannot be pinned as provider.max_price");
            return quote;
        }
        if (quote.route_ceiling && (!price.ceiling_input || !price.ceiling_output)) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "model '" + model + "' at routing gateway " + quote.origin +
                      " has no route ceiling (ceiling_input, ceiling_output), so the router may pick any price");
            return quote;
        }
        candidates.push_back({model, entry, *input, std::move(price)});
    }

    if (quote.route_ceiling) {
        // One pin covers every candidate, so each is bounded at the dearest ceiling.
        sp::OpenRouterMaxPrice ceiling;
        for (const auto& candidate : candidates) {
            ceiling.prompt_nano_usd = (std::max)(ceiling.prompt_nano_usd.value_or(0), *candidate.price.ceiling_input);
            ceiling.completion_nano_usd =
                (std::max)(ceiling.completion_nano_usd.value_or(0), *candidate.price.ceiling_output);
        }
        if (*ceiling.prompt_nano_usd > kMaxPriceNanoUsd || *ceiling.completion_nano_usd > kMaxPriceNanoUsd) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the route ceiling of model '" + quote.model + "' exceeds the largest provider.max_price amount");
            return quote;
        }
        const auto own = sent_max_price(prepared.encoded_body());
        if (own.malformed) {
            block(quote, "P_MONEY_NO_UPPER_BOUND", "the request's provider.max_price is not an exact USD amount");
            return quote;
        }
        quote.pin = lower_max_price(ceiling, own.price);
        for (auto& candidate : candidates) {
            candidate.price.ceiling_input  = ceiling.prompt_nano_usd;
            candidate.price.ceiling_output = ceiling.completion_nano_usd;
        }
    }

    const Candidate*                                    dearest = nullptr;
    std::uint64_t                                       bound   = 0;
    std::vector<std::pair<std::string, std::uint64_t>>  reserved_tools;
    for (const auto& candidate : candidates) {
        // Each group of tools that shares a cap is bounded at its dearest fee.
        std::vector<std::pair<std::string, std::uint64_t>> uses;
        sp::pricing::ModelPrice                            fees;
        for (const auto& cap : server_tools) {
            std::optional<std::pair<std::string, std::uint64_t>> group;
            for (const auto& type : cap.types) {
                const auto found = std::find_if(candidate.price.tool_fees.begin(), candidate.price.tool_fees.end(),
                                                [&](const auto& fee) { return fee.first == type; });
                if (found == candidate.price.tool_fees.end()) {
                    quote.witness["unmodeled"] = "server tool '" + type + "'";
                    block(quote, "P_MONEY_UNMODELED",
                          "catalogue entry '" + candidate.entry->id + "' lists no fee for server tool '" + type +
                              "' (unit tool:" + type + ")");
                    return quote;
                }
                if (!group || found->second > group->second) group = *found;
            }
            uses.emplace_back(group->first, *cap.uses);
            fees.tool_fees.push_back(*group);
        }
        std::sort(fees.tool_fees.begin(), fees.tool_fees.end());
        const auto total = sp::pricing::upper_bound(
            candidate.price, {candidate.input, *output, quote.route_ceiling, uses});
        if (!total.total) {
            block(quote, "P_MONEY_NO_UPPER_BOUND",
                  "the bound for model '" + candidate.model + "' has no price for " + joined(total.unpriced));
            return quote;
        }
        if (dearest && total.total->nano_usd <= bound) continue;
        const auto tools = sp::pricing::upper_bound(fees, {0, 0, false, uses});
        dearest                  = &candidate;
        bound                    = total.total->nano_usd;
        reserved_tools           = uses;
        quote.tool_fees_nano_usd = tools.total ? tools.total->nano_usd : bound;
    }
    const auto per_dispatch = ceil_microunits(bound);
    if (per_dispatch > kMax / *invocations) {
        block(quote, "P_MONEY_NO_UPPER_BOUND",
              "the reservation for model '" + quote.model + "' does not fit 64-bit microunits");
        return quote;
    }
    quote.hold       = per_dispatch * *invocations;
    quote.hold_basis = quote.route_ceiling ? "route_ceiling" : "list_rates";
    quote.pricing    = json{{"entry", dearest->entry->id}, {"units", price_rows(*dearest->entry)}};
    if (!quote.service_tier.empty()) quote.pricing["service_tier"] = quote.service_tier;
    if (!reserved_tools.empty()) {
        auto tools = json::object();
        for (const auto& [type, uses] : reserved_tools) tools[type] = uses;
        quote.pricing["tool_uses"] = std::move(tools);
    }
    quote.witness["entry"]               = dearest->entry->id;
    quote.witness["input_tokens_bound"]  = dearest->input;
    quote.witness["output_tokens_bound"] = *output;
    quote.witness["invocations"]         = *invocations;
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

PricedPreparation prepare_priced_call(Provider& provider, ProviderRequest request, bool priced) {
    if (!priced) return {provider.prepare(std::move(request)), std::nullopt};
    const auto facts = monetary_request_facts(request);
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
        if (probed.valid()) quote = assess_monetary_call(facts, probed);
    }  // The probe's operation slot is released here, unsent.
    if (quote) {
        if (quote->pin) (void)pin_route_max_price(request, *quote->pin);
        return {provider.prepare(std::move(request)), std::move(quote)};
    }
    // The provider does not admit the probe: price the request's own preparation.
    auto pinnable = request;
    auto prepared = provider.prepare(std::move(request));
    if (!prepared.valid()) return {std::move(prepared), std::nullopt};
    quote = assess_monetary_call(facts, prepared);
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
    // The snapshot the hold was priced with, never a newer one. A response
    // that names no model was answered by the admitted one.
    auto model = completion.model;
    if (model.requested.empty()) model.requested = quote.model;
    const auto estimate = sp::pricing::estimate(*quote.catalogue, quote.family, quote.origin, model, usage,
                                                quote.service_tier, quote.route_ceiling);
    if (!estimate.total)
        return held_record(quote, site, "the catalogue has no price for " + joined(estimate.unpriced));
    // Server tools are charged at their reserved cap: the usage does not count their uses.
    auto nano = estimate.total->nano_usd;
    if (!checked_add(nano, quote.tool_fees_nano_usd)) return held_record(quote, site, "overflow");
    auto record            = base_record(quote, site);
    record.evidence        = ProgramMonetaryEvidence::Estimated;
    record.microunits      = ceil_microunits(nano);
    record.basis           = quote.hold_basis;
    const auto assumed = [&](std::string_view rule) {
        return std::find(estimate.assumptions.begin(), estimate.assumptions.end(), rule) != estimate.assumptions.end();
    };
    record.conservative = quote.tool_fees_nano_usd != 0 || assumed("cache_read at input") ||
                          assumed("cache_write at cache_write_1h") || assumed("output at output_image") ||
                          assumed("route ceiling");
    record.may_undercount = assumed("cache_write at input") || assumed("cache_write_1h at cache_write");
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
    return value;
}

ProgramMonetaryRecord decode_monetary_record(const json& value) {
    if (!value.is_object())
        throw std::invalid_argument("Program monetary record must be an object");
    neograph::detail::reject_unknown_fields(
        value, "Program monetary record",
        {"call_id", "operation_id", "node", "origin", "model", "evidence", "microunits",
         "reserved_microunits", "basis", "catalog_version", "catalog_digest", "conservative",
         "may_undercount", "replayed", "reason", "pricing", "served_by"});
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
