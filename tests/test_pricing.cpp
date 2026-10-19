// Program pricing from the SchemaProvider model catalogue (#520): parity with
// the 0.15 price catalog, the reservation bound, data-driven refusals,
// settlement, the journal record, and no model id in the pricing code. No
// network.

#include "monetary.h"

#include "fixtures/model_catalogue.h"
#include "fixtures/typed_provider.h"

#include <neograph/llm/schema_provider.h>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace neograph;
using namespace neograph::program;

namespace {

constexpr std::string_view kOpenRouter = "https://openrouter.ai";
constexpr std::string_view kDirect     = "https://api.example.com";

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot read ") + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

sp::descriptor::CatalogueSnapshot catalogue_of(std::string_view text) {
    auto loaded = sp::descriptor::load_catalogue(text);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::runtime_error("catalogue " + error->pointer + ": " + error->expected);
    return std::get<sp::descriptor::CatalogueSnapshot>(std::move(loaded));
}

// The 0.15 price catalog as a host's catalogue layer.
const sp::descriptor::CatalogueSnapshot& shipped_layer() {
    static const auto layer = catalogue_of(read_file(NEOGRAPH_MODEL_PRICES));
    return layer;
}

std::uint64_t microunits(const sp::UsdAmount& amount) { return (amount.nano_usd + 999) / 1000; }

sp::Count count(std::uint64_t value) { return sp::Count{value, sp::Evidence::Reported}; }

// ---------------------------------------------------------------- parity

struct Parity {
    const char*                  family;
    const char*                  origin;
    const char*                  model;
    // The 0.15 catalog's reservation for 100,000 input and 4,096 output
    // tokens, and its estimate of parity_usage(), in microunits.
    std::optional<std::uint64_t> reserved;
    std::optional<std::uint64_t> estimated;
};

const std::vector<Parity> kParity{
    // A router alias lists no endpoints: no route ceiling, so no bound.
    {"openai.chat", "https://openrouter.ai", "~deepseek/deepseek-v4-flash-latest", std::nullopt, std::nullopt},
    {"openai.chat", "https://openrouter.ai", "deepseek/deepseek-v4-flash-0731", 49'407, 6'327},
    {"openai.chat", "https://openrouter.ai", "deepseek/deepseek-v4-flash-vision-exp", 49'407, 6'327},
    {"openai.chat", "https://api.openai.com", "gpt-4.1-nano", 11'639, 1'556},
    {"openai.chat", "https://api.openai.com", "gpt-4.1-mini", 46'554, 6'223},
    {"anthropic.messages", "https://api.anthropic.com", "claude-haiku-4-5-20251001", 220'480, 21'185},
    {"anthropic.messages", "https://api.anthropic.com", "claude-sonnet-4-5-20250929", 661'440, 63'555},
    {"anthropic.messages", "https://api.anthropic.com", "claude-sonnet-5", 440'960, 42'370},
    {"anthropic.messages", "https://api.anthropic.com", "claude-opus-4-7", 1'102'400, 105'925},
    {"anthropic.messages", "https://api.anthropic.com", "claude-opus-4-8", 1'102'400, 105'925},
    {"anthropic.messages", "https://api.anthropic.com", "claude-opus-5", 1'102'400, 105'925},
    {"google.generate", "https://generativelanguage.googleapis.com", "gemini-3.1-flash-lite-image", 147'880, 18'854},
    {"google.generate", "https://aiplatform.googleapis.com", "gemini-3.1-flash-lite-image", 147'880, 18'404},
};

// 12,345 uncached input tokens and 678 output tokens; 2,000 cache reads off
// OpenRouter; 3,000 Anthropic cache writes (1,000 five-minute, 2,000 one-hour);
// 500 image output tokens for an image model.
sp::Usage parity_usage(const Parity& row) {
    const bool        routed = std::string_view(row.origin) == kOpenRouter;
    const std::uint64_t read = routed ? 0 : 2'000;
    const std::uint64_t write = std::string_view(row.family) == "anthropic.messages" ? 3'000 : 0;
    sp::Usage usage;
    usage.stage          = sp::UsageStage::Final;
    usage.quality        = sp::UsageQuality::Consistent;
    usage.input_uncached = count(12'345);
    usage.cache_read     = count(read);
    usage.cache_write    = count(write);
    usage.input_total    = count(12'345 + read + write);
    usage.output_total   = count(678);
    if (write) {
        usage.extra["cache_creation.ephemeral_5m_input_tokens"] = count(1'000);
        usage.extra["cache_creation.ephemeral_1h_input_tokens"] = count(2'000);
    }
    if (std::string_view(row.model).find("image") != std::string_view::npos)
        usage.extra["candidatesTokensDetails.IMAGE"] = count(500);
    return usage;
}

// ------------------------------------------------------- monetary layer

// A SchemaProvider for `family` at `origin` whose policy carries `models`.
std::shared_ptr<Provider> provider_at(const std::string& family, const std::string& origin,
                                      const std::vector<neograph::test::FixtureModel>& models) {
    return llm::SchemaProvider::create(
        neograph::test::descriptor(family, origin,
                                   neograph::test::policy_with_models(
                                       json::parse(sp::config_defaults::descriptor_policy_json), models)));
}

neograph::test::FixtureModel model_with(std::string model, std::uint64_t input_limit, json pricing) {
    neograph::test::FixtureModel fixture{"openai.chat", std::move(model), input_limit};
    fixture.pricing = std::move(pricing);
    return fixture;
}

PreparedProviderRequest prepared(Provider& provider, const std::string& model, std::uint64_t output = 100) {
    auto request = make_provider_request(provider, model, {neograph::test::message("q", sp::Role::User)});
    std::get<sp::chat::Request>(request.payload).max_output_tokens = output;
    return provider.prepare(std::move(request));
}

// 1 and 2 USD per million input and output tokens: 1,000 and 2,000 nano-USD per token.
const json kListed{{"input", "0.000001"}, {"output", "0.000002"}};

program::detail::MonetaryRequestFacts with_tools(std::vector<std::string> types, std::optional<std::uint64_t> uses) {
    program::detail::MonetaryRequestFacts facts;
    facts.tools.push_back({std::move(types), uses});
    return facts;
}

}  // namespace

TEST(PricingParity, TheShippedLayerLoadsAndMergesOverTheEmbeddedCatalogue) {
    const auto& layer = shipped_layer();
    EXPECT_EQ(layer->entries().size(), kParity.size());
    for (const auto& entry : layer->entries()) {
        EXPECT_FALSE(entry.pricing.empty()) << entry.id;
        EXPECT_EQ(entry.origins.size(), 1U) << entry.id << " is scoped to the origin it was priced at";
        EXPECT_TRUE(entry.source) << entry.id;
    }
    const std::array layers{sp::descriptor::builtin_catalogue(), layer};
    EXPECT_TRUE(std::holds_alternative<sp::descriptor::CatalogueSnapshot>(sp::descriptor::merge_catalogues(layers)));
    // One model at two origins: an entry per origin, the second by an alias.
    const auto* studio = layer->find("google.generate", "https://generativelanguage.googleapis.com",
                                     "gemini-3.1-flash-lite-image").entry;
    const auto* vertex = layer->find("google.generate", "https://aiplatform.googleapis.com",
                                     "gemini-3.1-flash-lite-image").entry;
    ASSERT_TRUE(studio && vertex);
    EXPECT_NE(studio, vertex);
    EXPECT_FALSE(layer->find("google.generate", "https://api.openai.com", "gemini-3.1-flash-lite-image").entry);
}

TEST(PricingParity, ReservationsAndEstimatesMatchThe015Catalog) {
    for (const auto& row : kParity) {
        SCOPED_TRACE(std::string(row.origin) + " " + row.model);
        const auto* entry = shipped_layer()->find(row.family, row.origin, row.model).entry;
        ASSERT_TRUE(entry);
        const bool routed = std::string_view(row.origin) == kOpenRouter;
        const auto bound  = sp::pricing::upper_bound(sp::pricing::dearest_price_of(*entry, {}, 100'000),
                                                     {100'000, 4'096, routed, {}});
        const auto estimate = sp::pricing::estimate(*shipped_layer(), row.family, row.origin,
                                                    sp::ServedModel{row.model, row.model, sp::ServedRelation::Exact},
                                                    parity_usage(row), {}, routed);
        if (!row.reserved) {
            EXPECT_FALSE(bound.total);
            EXPECT_EQ(bound.unpriced, std::vector<std::string>{"ceiling"});
            EXPECT_FALSE(estimate.total);
            continue;
        }
        ASSERT_TRUE(bound.total);
        EXPECT_EQ(microunits(*bound.total), *row.reserved);
        ASSERT_TRUE(estimate.total) << estimate.unpriced.size();
        EXPECT_EQ(microunits(*estimate.total), *row.estimated);
    }
}

TEST(PricingBound, AnEstimateNeverExceedsItsReservation) {
    // The shipped entries and one with a long-context band, over random bounds
    // and random usage within them.
    std::vector<std::pair<std::string, const sp::descriptor::CatalogueEntry*>> entries;
    for (const auto& row : kParity)
        if (row.reserved) entries.emplace_back(row.origin, shipped_layer()->find(row.family, row.origin, row.model).entry);
    const auto banded = catalogue_of(R"({"version":1,"models":[{"id":"banded","pricing":{
        "input":"0.000001","output":"0.000004","cache_read":"0.0000001","cache_write":"0.00000125",
        "cache_write_1h":"0.000002","input+200000":"0.000002","output+200000":"0.000006",
        "cache_read+200000":"0.0000002","cache_write_1h+200000":"0.000004","request":"0.0005"}}]})");
    entries.emplace_back(std::string(kDirect), banded->entry("banded"));
    std::mt19937_64 random(520);
    const auto below = [&](std::uint64_t limit) { return std::uniform_int_distribution<std::uint64_t>(0, limit)(random); };
    for (int i = 0; i < 4'000; ++i) {
        const auto& [origin, entry] = entries[i % entries.size()];
        const bool routed = origin == kOpenRouter;
        const auto input = below(400'000), output = below(64'000);
        const auto bound = sp::pricing::upper_bound(sp::pricing::dearest_price_of(*entry, {}, input),
                                                    {input, output, routed, {}});
        ASSERT_TRUE(bound.total);
        sp::Usage usage;
        usage.stage   = sp::UsageStage::Final;
        usage.quality = sp::UsageQuality::Consistent;
        const auto read = below(input), write = below(input - read), uncached = below(input - read - write);
        const auto hour = below(write);
        usage.input_uncached = count(uncached);
        usage.cache_read     = count(read);
        usage.cache_write    = count(write);
        usage.input_total    = count(uncached + read + write);
        usage.output_total   = count(output);
        usage.reasoning      = count(below(output));
        if (i % 2) {
            usage.extra["cache_creation.ephemeral_5m_input_tokens"] = count(write - hour);
            usage.extra["cache_creation.ephemeral_1h_input_tokens"] = count(hour);
            usage.extra["candidatesTokensDetails.IMAGE"]           = count(below(output));
        }
        const auto estimate = sp::pricing::estimate(entry == banded->entry("banded") ? *banded : *shipped_layer(),
                                                    "anthropic.messages", origin,
                                                    sp::ServedModel{entry->id, entry->id, sp::ServedRelation::Exact},
                                                    usage, {}, routed);
        ASSERT_TRUE(estimate.total) << entry->id;
        EXPECT_LE(estimate.total->nano_usd, bound.total->nano_usd) << entry->id << " input " << input;
    }
}

TEST(PricingAssess, TheHoldIsTheCatalogueBoundOfTheAdmittedRequest) {
    auto provider = provider_at("openai.chat", std::string(kDirect), {model_with("vendor/model", 1'000, kListed)});
    const auto quote = program::detail::assess_monetary_call({}, prepared(*provider, "vendor/model"));
    ASSERT_TRUE(quote.blocker.empty()) << quote.blocker_message;
    // 1,000 input tokens at 1,000 and 100 output tokens at 2,000 nano-USD.
    EXPECT_EQ(quote.hold, (1'000U * 1'000 + 100 * 2'000) / 1'000);
    EXPECT_EQ(quote.hold_basis, "list_rates");
    EXPECT_EQ(quote.origin, kDirect);
    EXPECT_EQ(quote.catalog_version, program::detail::kCatalogueScheme);
    EXPECT_EQ(quote.catalog_digest.size(), 64U);
    EXPECT_FALSE(quote.pin) << "a direct origin routes nowhere else";
    EXPECT_EQ(quote.pricing, (json{{"entry", "vendor/model"}, {"units", kListed}}));
    EXPECT_EQ(quote.witness.at("input_tokens_bound"), 1'000);
}

TEST(PricingAssess, ALongContextBandIsReservedWhenTheInputLimitReachesIt) {
    auto prices                  = kListed;
    prices["input+200000"]       = "0.000002";
    prices["output+200000"]      = "0.000003";
    auto provider = provider_at("openai.chat", std::string(kDirect),
                                {model_with("short", 200'000, prices), model_with("long", 1'000'000, prices)});
    EXPECT_EQ(program::detail::assess_monetary_call({}, prepared(*provider, "short")).hold, 200'000U + 200);
    EXPECT_EQ(program::detail::assess_monetary_call({}, prepared(*provider, "long")).hold, 2'000'000U + 300);
}

TEST(PricingAssess, RefusalsAreDataAndAddingTheFactAdmitsTheCall) {
    auto ceiling                 = kListed;
    ceiling["ceiling_input"]     = "0.000003";
    ceiling["ceiling_output"]    = "0.000004";
    auto tiered                  = kListed;
    tiered["input/flex"]         = "0.0000005";
    tiered["output/flex"]        = "0.000001";
    auto fees                    = kListed;
    fees["tool:web_search"]      = "0.01";
    fees["tool:file_search"]     = "0.0025";
    fees["tool:free_tool"]       = "0";

    // No price for the model.
    auto direct = provider_at("openai.chat", std::string(kDirect),
                              {model_with("unpriced", 1'000, json::object()), model_with("tiered", 1'000, tiered),
                               model_with("fees", 1'000, fees)});
    EXPECT_EQ(program::detail::assess_monetary_call({}, prepared(*direct, "unpriced")).blocker, "P_MONEY_UNKNOWN_MODEL");

    // A service tier the entry does not price, and one it does.
    program::detail::MonetaryRequestFacts flex;
    flex.service_tier = "flex";
    EXPECT_EQ(program::detail::assess_monetary_call(flex, prepared(*direct, "fees")).blocker, "P_MONEY_UNMODELED");
    const auto tier = program::detail::assess_monetary_call(flex, prepared(*direct, "tiered"));
    ASSERT_TRUE(tier.blocker.empty()) << tier.blocker_message;
    EXPECT_EQ(tier.hold, 500U + 100);
    EXPECT_EQ(tier.service_tier, "flex");
    EXPECT_EQ(tier.pricing.at("service_tier"), "flex");
    program::detail::MonetaryRequestFacts standard;
    standard.service_tier = "default";
    EXPECT_EQ(program::detail::assess_monetary_call(standard, prepared(*direct, "tiered")).hold, 1'000U + 200);

    // A server tool without a use cap, without a fee, and with both. Tools that
    // share one cap are bounded at the dearest fee; zero is a declared fee.
    EXPECT_EQ(program::detail::assess_monetary_call(with_tools({"web_search"}, std::nullopt), prepared(*direct, "fees")).blocker,
              "P_MONEY_NO_UPPER_BOUND");
    EXPECT_EQ(program::detail::assess_monetary_call(with_tools({"code_interpreter"}, 2), prepared(*direct, "fees")).blocker,
              "P_MONEY_UNMODELED");
    const auto tools = program::detail::assess_monetary_call(with_tools({"file_search", "web_search"}, 3), prepared(*direct, "fees"));
    ASSERT_TRUE(tools.blocker.empty()) << tools.blocker_message;
    // Three uses at 0.01 USD on top of the tokens.
    EXPECT_EQ(tools.hold, 1'000U + 200 + 30'000);
    EXPECT_EQ(tools.tool_fees_nano_usd, 30'000'000U);
    EXPECT_EQ(tools.pricing.at("tool_uses"), (json{{"web_search", 3}}));
    EXPECT_EQ(program::detail::assess_monetary_call(with_tools({"free_tool"}, 5), prepared(*direct, "fees")).hold, 1'000U + 200);

    // A feature the catalogue cannot price.
    program::detail::MonetaryRequestFacts plugins;
    plugins.unpriced = "OpenRouter plugins";
    EXPECT_EQ(program::detail::assess_monetary_call(plugins, prepared(*direct, "fees")).blocker, "P_MONEY_UNMODELED");

    // A routing gateway needs a route ceiling, and a ceiling needs a gateway.
    auto routed = provider_at("openai.chat", std::string(kOpenRouter),
                              {model_with("vendor/listed", 1'000, kListed), model_with("vendor/ceiling", 1'000, ceiling),
                               model_with("vendor/dearer", 2'000, json{{"input", "0.000001"}, {"output", "0.000002"},
                                                                       {"ceiling_input", "0.000005"},
                                                                       {"ceiling_output", "0.000003"}})});
    EXPECT_EQ(program::detail::assess_monetary_call({}, prepared(*routed, "vendor/listed")).blocker, "P_MONEY_NO_UPPER_BOUND");
    const auto pinned = program::detail::assess_monetary_call({}, prepared(*routed, "vendor/ceiling"));
    ASSERT_TRUE(pinned.blocker.empty()) << pinned.blocker_message;
    EXPECT_EQ(pinned.hold, 3'000U + 400);
    EXPECT_EQ(pinned.hold_basis, "route_ceiling");
    ASSERT_TRUE(pinned.pin);
    EXPECT_EQ(pinned.pin->prompt_nano_usd, 3'000'000'000U);
    auto misplaced = provider_at("openai.chat", std::string(kDirect), {model_with("vendor/ceiling", 1'000, ceiling)});
    EXPECT_EQ(program::detail::assess_monetary_call({}, prepared(*misplaced, "vendor/ceiling")).blocker, "P_MONEY_NO_UPPER_BOUND");

    // OpenRouter fallbacks: every candidate is priced, the pin covers the
    // dearest ceiling, and the hold is the dearest candidate's bound at it.
    program::detail::MonetaryRequestFacts fallbacks;
    fallbacks.fallback_models = {"vendor/unlisted"};
    EXPECT_EQ(program::detail::assess_monetary_call(fallbacks, prepared(*routed, "vendor/ceiling")).blocker,
              "P_MONEY_UNKNOWN_MODEL");
    fallbacks.fallback_models = {"vendor/dearer"};
    const auto fallback = program::detail::assess_monetary_call(fallbacks, prepared(*routed, "vendor/ceiling"));
    ASSERT_TRUE(fallback.blocker.empty()) << fallback.blocker_message;
    ASSERT_TRUE(fallback.pin);
    EXPECT_EQ(fallback.pin->prompt_nano_usd, 5'000'000'000U);
    EXPECT_EQ(fallback.pin->completion_nano_usd, 4'000'000'000U);
    // vendor/dearer: 2,000 input tokens at 5,000 and 100 output at 4,000 nano-USD.
    EXPECT_EQ(fallback.hold, 10'000U + 400);
    EXPECT_EQ(fallback.pricing.at("entry"), "vendor/dearer");
}

TEST(PricingAssess, RequestFactsComeFromTheTypedRequest) {
    ProviderRequest chat;
    sp::chat::Request chat_payload;
    chat_payload.models       = {"vendor/a", "vendor/b"};
    chat_payload.service_tier = "flex";
    chat.payload              = chat_payload;
    auto facts                = program::detail::monetary_request_facts(chat);
    EXPECT_EQ(facts.fallback_models, (std::vector<std::string>{"vendor/a", "vendor/b"}));
    EXPECT_EQ(facts.service_tier, "flex");
    EXPECT_FALSE(facts.unpriced);
    std::get<sp::chat::Request>(chat.payload).plugins.emplace_back();
    EXPECT_TRUE(program::detail::monetary_request_facts(chat).unpriced);

    ProviderRequest responses;
    sp::responses::Request responses_payload;
    responses_payload.hosted_tools   = {sp::responses::WebSearchTool{}, sp::responses::FileSearchTool{}};
    responses_payload.max_tool_calls = 4;
    responses.payload                = responses_payload;
    facts                            = program::detail::monetary_request_facts(responses);
    ASSERT_EQ(facts.tools.size(), 1U);
    EXPECT_EQ(facts.tools[0].types, (std::vector<std::string>{"web_search", "file_search"}));
    EXPECT_EQ(facts.tools[0].uses, 4U);

    ProviderRequest messages;
    sp::messages::Request messages_payload;
    sp::messages::ToolDefinition search;
    search.type     = "web_search_20250305";
    search.name     = "web_search";
    search.max_uses = 2;
    sp::messages::ToolDefinition client;
    client.name = "lookup";
    messages_payload.tools = {search, client};
    messages.payload       = messages_payload;
    facts                  = program::detail::monetary_request_facts(messages);
    ASSERT_EQ(facts.tools.size(), 1U) << "a client tool bills only tokens";
    EXPECT_EQ(facts.tools[0].types, (std::vector<std::string>{"web_search_20250305"}));
    EXPECT_EQ(facts.tools[0].uses, 2U);

    ProviderRequest generate;
    sp::gemini::Request generate_payload;
    generate_payload.builtin_tools = {sp::gemini::GoogleSearchTool{}};
    generate.payload               = generate_payload;
    facts                          = program::detail::monetary_request_facts(generate);
    ASSERT_EQ(facts.tools.size(), 1U);
    EXPECT_EQ(facts.tools[0].types, (std::vector<std::string>{"google_search"}));
    EXPECT_FALSE(facts.tools[0].uses) << "Gemini's built-in tools take no use cap";
}

TEST(PricingSettle, AnEstimateUsesTheHoldsSnapshotAndChargesReservedToolFees) {
    auto fees               = kListed;
    fees["tool:web_search"] = "0.01";
    auto provider = provider_at("openai.chat", std::string(kDirect), {model_with("vendor/model", 1'000, fees)});
    const auto quote = program::detail::assess_monetary_call(with_tools({"web_search"}, 2), prepared(*provider, "vendor/model"));
    ASSERT_TRUE(quote.blocker.empty()) << quote.blocker_message;
    const program::detail::MonetaryCallSite site{"call", "operation", "node"};
    auto usage    = neograph::test::usage(300, 40, 340);
    const auto record = program::detail::settle_monetary_call(quote, site, neograph::test::success("answer", usage));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->evidence, ProgramMonetaryEvidence::Estimated);
    // 300 input tokens at 1,000 and 40 output at 2,000 nano-USD, plus two uses
    // of the tool at its 0.01 USD fee.
    EXPECT_EQ(record->microunits, (300U * 1'000 + 40 * 2'000 + 20'000'000) / 1'000);
    EXPECT_LE(record->microunits, record->reserved_microunits);
    EXPECT_TRUE(record->conservative) << "tool uses are charged at their cap";
    EXPECT_EQ(record->basis, "list_rates");
    EXPECT_EQ(record->catalog_digest, quote.catalog_digest);
    EXPECT_EQ(record->pricing.at("entry"), "vendor/model");
    EXPECT_EQ(record->pricing.at("tool_uses"), (json{{"web_search", 2}}));

    // A reported cost settles without the catalogue.
    usage.provider_cost.source    = sp::CostSource::OpenRouterUsd;
    usage.provider_cost.total     = sp::UsdAmount{1'234'567};
    usage.provider_cost.status[0] = sp::CostStatus::Available;
    usage.provider_cost.is_byok   = false;
    usage.provider_cost.byok_status = sp::CostStatus::Available;
    const auto reported = program::detail::settle_monetary_call(quote, site, neograph::test::success("answer", usage));
    ASSERT_TRUE(reported);
    EXPECT_EQ(reported->evidence, ProgramMonetaryEvidence::Reported);
    EXPECT_EQ(reported->microunits, 1'235U);
    EXPECT_EQ(reported->basis, "provider_reported");
    // It still names the catalogue and rows its hold was priced with.
    EXPECT_EQ(reported->catalog_digest, quote.catalog_digest);
    EXPECT_EQ(reported->pricing, quote.pricing);

    // Partial usage keeps the hold, which names its catalogue and rows.
    auto partial  = neograph::test::usage(300, std::nullopt, std::nullopt, sp::UsageStage::Partial);
    const auto held = program::detail::settle_monetary_call(quote, site, neograph::test::success("answer", partial));
    ASSERT_TRUE(held);
    EXPECT_EQ(held->evidence, ProgramMonetaryEvidence::Held);
    EXPECT_EQ(held->microunits, quote.hold);
    EXPECT_EQ(held->pricing, quote.pricing);
}

TEST(PricingSettle, ByokAddsTheUpstreamCharge) {
    sp::ProviderReportedCost cost;
    cost.source      = sp::CostSource::OpenRouterUsd;
    cost.total       = sp::UsdAmount{50'000};
    cost.status[0]   = sp::CostStatus::Available;
    cost.is_byok     = true;
    cost.byok_status = sp::CostStatus::Available;
    cost.upstream_total = sp::UsdAmount{1'000'000};
    cost.status[1]      = sp::CostStatus::Available;
    EXPECT_EQ(program::detail::reported_cost_microunits(cost), 1'050U);
    auto missing_upstream           = cost;
    missing_upstream.upstream_total = std::nullopt;
    missing_upstream.status[1]      = sp::CostStatus::Missing;
    EXPECT_FALSE(program::detail::reported_cost_microunits(missing_upstream));
    // An unknown BYOK flag with an upstream amount adds it rather than risk under-counting.
    auto unknown        = cost;
    unknown.is_byok     = std::nullopt;
    unknown.byok_status = sp::CostStatus::Missing;
    EXPECT_EQ(program::detail::reported_cost_microunits(unknown), 1'050U);
    // Not BYOK: OpenRouter's total already includes the upstream inference.
    auto direct    = cost;
    direct.is_byok = false;
    EXPECT_EQ(program::detail::reported_cost_microunits(direct), 50U);
}

TEST(PricingRecord, A015RecordKeepsItsBytesAndANewOneCarriesItsPricing) {
    // A record journaled by 0.15 decodes and encodes to the same canonical bytes.
    const auto old_record = json::parse(
        R"({"call_id":"call","operation_id":"op","node":"llm","origin":"openrouter","model":"vendor/model",)"
        R"("evidence":"estimated","microunits":14,"reserved_microunits":1000,"basis":"route_ceiling",)"
        R"("catalog_version":"2026-10-11.1","catalog_digest":"sha256:)" + std::string(64, 'a') +
        R"(","conservative":false,"may_undercount":false,"replayed":false,"reason":""})");
    const auto decoded = program::detail::decode_monetary_record(old_record);
    EXPECT_TRUE(decoded.pricing.empty());
    EXPECT_EQ(program::detail::encode_monetary_record(decoded).dump(), old_record.dump());

    auto current    = old_record;
    current["pricing"] = json{{"entry", "vendor/model"}, {"units", kListed}, {"assumptions", json::array({"route ceiling"})}};
    const auto with_pricing = program::detail::decode_monetary_record(current);
    EXPECT_EQ(with_pricing.pricing.at("units"), kListed);
    EXPECT_EQ(program::detail::encode_monetary_record(with_pricing).dump(), current.dump());

    current["pricing"] = json::array();
    EXPECT_THROW((void)program::detail::decode_monetary_record(current), std::invalid_argument);
}

TEST(PricingSource, ThePricingCodeNamesNoModel) {
    // Model ids, prices and limits are catalogue data; the pricing code holds
    // none. Every string literal of monetary.cpp is checked for a model id.
    const auto source = read_file(NEOGRAPH_MONETARY_SOURCE);
    std::vector<std::string> literals;
    for (std::size_t at = 0; at < source.size(); ++at) {
        if (source.compare(at, 2, "//") == 0) {
            at = source.find('\n', at);
            if (at == std::string::npos) break;
        } else if (source[at] == '\'') {
            at += source[at + 1] == '\\' ? 3 : 2;
        } else if (source[at] == '"') {
            std::string literal;
            for (++at; at < source.size() && source[at] != '"'; ++at) {
                if (source[at] == '\\') ++at;
                literal += source[at];
            }
            literals.push_back(std::move(literal));
        }
    }
    EXPECT_GT(literals.size(), 50U);
    for (const auto& literal : literals)
        for (const std::string_view vendor : {"gpt-", "claude", "gemini-", "deepseek", "llama", "grok", "mistral",
                                              "qwen", "o1-", "o3-", "o4-"})
            EXPECT_EQ(literal.find(vendor), std::string::npos) << "a model id in a literal: \"" << literal << '"';
}
