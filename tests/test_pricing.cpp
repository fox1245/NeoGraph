// The internal pricing registry (#372): catalog loading and validation, tier
// selection, cache bands, rounding, reported versus estimated settlement,
// reservation upper bounds and unknown models. No network.

#include "pricing.h"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <variant>

using namespace neograph::program::pricing;

namespace {

constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

const PriceCatalog& shipped() {
    static const PriceCatalog catalog = PriceCatalog::load(NEOGRAPH_PRICING_CATALOG);
    return catalog;
}

// A synthetic catalog: one direct origin with a two-tier model shaped like a
// prompt-length tier, and one aggregator. Prices here are test values only.
constexpr std::string_view kTiered = R"({
  "schema": "neograph.pricing.catalog",
  "schema_version": 1,
  "version": "test-1",
  "currency": "USD",
  "rate_unit": "usd_per_million_tokens",
  "origins": {
    "example": {"url": "https://api.example.com", "routing": "direct"},
    "router": {"url": "https://router.example.com", "routing": "aggregator"}
  },
  "entries": [
    {
      "origin": "example", "model": "tiered-model", "aliases": ["tiered-model-2026"],
      "source_url": "https://example.com/pricing", "retrieved": "2026-10-11",
      "tiers": [
        {"up_to_input_tokens": 100000, "input": "0.10", "cache_write": "0.125",
         "cache_write_1h": "0.20", "cache_read": "0.01", "output": "0.50"},
        {"up_to_input_tokens": null, "input": "0.50", "cache_write": "0.625",
         "cache_write_1h": "1", "cache_read": "0.05", "output": "2.50"}
      ]
    },
    {
      "origin": "router", "model": "vendor/model",
      "source_url": "https://example.com/models", "retrieved": "2026-10-11",
      "tiers": [{"input": "0.10", "output": "0.20"}],
      "route_ceiling": {"source_url": "https://example.com/endpoints",
                        "retrieved": "2026-10-11", "input": "0.40", "output": "0.80"}
    },
    {
      "origin": "router", "model": "~vendor/model-latest",
      "source_url": "https://example.com/models", "retrieved": "2026-10-11",
      "tiers": [{"input": "0.10", "output": "0.20"}]
    }
  ]
})";

const PriceCatalog& tiered() {
    static const PriceCatalog catalog = PriceCatalog::parse(kTiered);
    return catalog;
}

sp::Count count(std::uint64_t value, sp::Evidence evidence = sp::Evidence::Reported) {
    return sp::Count{value, evidence};
}

// Final usage with the given inclusive input total and output total; bands are set by the caller.
sp::Usage usage(std::uint64_t input_total, std::uint64_t output_total) {
    sp::Usage result;
    result.input_total  = count(input_total);
    result.output_total = count(output_total);
    result.stage        = sp::UsageStage::Final;
    return result;
}

sp::Usage banded(std::uint64_t uncached, std::uint64_t read, std::uint64_t write,
                 std::uint64_t output) {
    auto result           = usage(uncached + read + write, output);
    result.input_uncached = count(uncached, sp::Evidence::Derived);
    result.cache_read     = count(read);
    result.cache_write    = count(write);
    return result;
}

template <class T>
const T& value_of(const PricingResult<T>& result) {
    if (const auto* error = std::get_if<PricingError>(&result)) {
        ADD_FAILURE() << "unexpected pricing error: " << to_string(*error);
        static const T empty{};
        return empty;
    }
    return std::get<T>(result);
}

template <class T>
PricingError error_of(const PricingResult<T>& result) {
    EXPECT_TRUE(std::holds_alternative<PricingError>(result));
    return std::holds_alternative<PricingError>(result) ? std::get<PricingError>(result)
                                                        : PricingError::Overflow;
}

sp::ProviderReportedCost openrouter_cost(std::uint64_t total_nano) {
    sp::ProviderReportedCost cost;
    cost.source    = sp::CostSource::OpenRouterUsd;
    cost.total     = sp::UsdAmount{total_nano};
    cost.status[0] = sp::CostStatus::Available;
    cost.is_byok     = false;
    cost.byok_status = sp::CostStatus::Available;
    return cost;
}

std::string with(std::string_view base, std::string_view from, std::string_view to) {
    std::string text(base);
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

void expect_rejected(const std::string& text, std::string_view fragment) {
    try {
        (void)PriceCatalog::parse(text);
        ADD_FAILURE() << "catalog accepted; expected an error mentioning " << fragment;
    } catch (const CatalogError& error) {
        EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos) << error.what();
    }
}

}  // namespace

// ---------------------------------------------------------------- loading

TEST(PricingCatalog, ShippedCatalogLoadsWithSourcesForEveryEntry) {
    const auto& catalog = shipped();
    EXPECT_FALSE(catalog.version().empty());
    EXPECT_EQ(catalog.digest().rfind("sha256:", 0), 0u) << catalog.digest();
    ASSERT_FALSE(catalog.entries().empty());
    for (const auto& entry : catalog.entries()) {
        EXPECT_EQ(entry.source_url.rfind("https://", 0), 0u) << entry.model;
        EXPECT_EQ(entry.retrieved.size(), 10u) << entry.model;
        EXPECT_EQ(catalog.find(entry.origin, entry.model), &entry);
        for (const auto& alias : entry.aliases) EXPECT_EQ(catalog.find(entry.origin, alias), &entry);
        const auto* origin = catalog.find_origin(entry.origin);
        ASSERT_NE(origin, nullptr);
        // Route ceilings exist only where an aggregator chooses among endpoints.
        if (entry.route_ceiling) {
            EXPECT_TRUE(origin->aggregator) << entry.model;
        }
    }
}

TEST(PricingCatalog, ShippedCatalogKeysTheSameModelPerOrigin) {
    const auto& catalog = shipped();
    const auto* studio  = catalog.find("google-ai-studio", "gemini-3.1-flash-lite-image");
    const auto* vertex  = catalog.find("google-vertex-global", "gemini-3.1-flash-lite-image");
    ASSERT_NE(studio, nullptr);
    ASSERT_NE(vertex, nullptr);
    EXPECT_NE(studio, vertex);
    // The two origins publish different cache terms for the same model.
    EXPECT_FALSE(studio->tiers.front().rates.cache_read.has_value());
    EXPECT_EQ(vertex->tiers.front().rates.cache_read, 25'000u);
    EXPECT_EQ(catalog.find("openai", "gemini-3.1-flash-lite-image"), nullptr);

    ASSERT_NE(catalog.find("anthropic", "claude-haiku-4-5"), nullptr);
    EXPECT_EQ(catalog.find("anthropic", "claude-haiku-4-5"),
              catalog.find("anthropic", "claude-haiku-4-5-20251001"));
    EXPECT_EQ(catalog.find("openrouter", "deepseek/deepseek-v4-flash-20260731"),
              catalog.find("openrouter", "deepseek/deepseek-v4-flash-0731"));
}

TEST(PricingCatalog, OriginLookupUsesSchemeAndHostOnly) {
    const auto& catalog = shipped();
    const auto* router  = catalog.find_origin_by_url("https://OpenRouter.ai/api/v1/chat/completions");
    ASSERT_NE(router, nullptr);
    EXPECT_EQ(router->id, "openrouter");
    EXPECT_TRUE(router->aggregator);
    ASSERT_NE(catalog.find_origin_by_url("https://generativelanguage.googleapis.com/v1beta"), nullptr);
    EXPECT_EQ(catalog.find_origin_by_url("https://generativelanguage.googleapis.com")->id,
              "google-ai-studio");
    // A regional Vertex endpoint is a different origin with different prices.
    EXPECT_EQ(catalog.find_origin_by_url("https://us-central1-aiplatform.googleapis.com/v1"), nullptr);
    EXPECT_EQ(catalog.find_origin_by_url("http://openrouter.ai"), nullptr);
    EXPECT_EQ(catalog.find_origin_by_url("not a url"), nullptr);
}

TEST(PricingCatalog, DigestIsCanonical) {
    const auto spaced = with(kTiered, R"("version": "test-1",)", "\"version\":   \"test-1\" ,");
    EXPECT_EQ(PriceCatalog::parse(spaced).digest(), tiered().digest());
    const auto changed = with(kTiered, R"("input": "0.40")", R"("input": "0.41")");
    EXPECT_NE(PriceCatalog::parse(changed).digest(), tiered().digest());
}

TEST(PricingCatalog, RatesAreExactDecimals) {
    EXPECT_EQ(parse_rate("0.10"), 100'000u);
    EXPECT_EQ(parse_rate("30"), 30'000'000u);
    EXPECT_EQ(parse_rate("30.00"), 30'000'000u);
    EXPECT_EQ(parse_rate("0.00686"), 6'860u);
    EXPECT_EQ(parse_rate("0.000001"), 1u);
    EXPECT_EQ(parse_rate("0"), 0u);
    EXPECT_EQ(parse_rate("10000"), 10'000'000'000u);
    for (const auto* bad : {"", "0.0000001", "-1", "+1", "1e3", ".5", "1.", "01", "0x10", " 1",
                            "1 ", "1,5", "10000.000001", "123456"}) {
        EXPECT_FALSE(parse_rate(bad).has_value()) << bad;
    }
}

TEST(PricingCatalog, ValidationNamesTheOffendingField) {
    const std::string base(kTiered);
    expect_rejected(with(base, R"("schema_version": 1)", R"("schema_version": 2)"), "schema_version");
    expect_rejected(with(base, R"("currency": "USD")", R"("currency": "EUR")"), "currency");
    expect_rejected(with(base, R"("version": "test-1",)", R"("version": "test-1", "extra": 1,)"),
                    "undeclared field 'extra'");
    expect_rejected(with(base, R"("version": "test-1",)", R"("version": "a", "version": "b",)"),
                    "duplicate");
    expect_rejected(with(base, R"("url": "https://api.example.com")", R"("url": "http://api.example.com")"),
                    "origins.example.url");
    expect_rejected(with(base, R"("url": "https://api.example.com")",
                         R"("url": "https://api.example.com/v1")"),
                    "origins.example.url");
    expect_rejected(with(base, R"("source_url": "https://example.com/pricing")", R"("source_url": "")"),
                    "entries[0].source_url");
    expect_rejected(with(base, R"("source_url": "https://example.com/pricing", )", ""),
                    "missing field 'source_url'");
    expect_rejected(with(base, R"("retrieved": "2026-10-11",
      "tiers")", R"("retrieved": "2026-13-01",
      "tiers")"), "entries[0].retrieved");
    expect_rejected(with(base, R"("input": "0.10", "cache_write")", R"("input": 0.1, "cache_write")"),
                    "entries[0].tiers[0].input");
    expect_rejected(with(base, R"("origin": "example", "model")", R"("origin": "nowhere", "model")"),
                    "entries[0].origin");
    expect_rejected(with(base, R"("aliases": ["tiered-model-2026"])", R"("aliases": ["tiered-model"])"),
                    "already priced");
    expect_rejected(with(base, R"("up_to_input_tokens": null)", R"("up_to_input_tokens": 200000)"),
                    "last tier must be unbounded");
    expect_rejected(with(base, R"("up_to_input_tokens": 100000)", R"("up_to_input_tokens": null)"),
                    "only the last tier may be unbounded");
    expect_rejected(with(base, R"("up_to_input_tokens": 100000)", R"("up_to_input_tokens": 0)"),
                    "positive integer");
    // A higher tier that is cheaper would make a bound at the larger input unsound.
    expect_rejected(with(base, R"("cache_read": "0.05")", R"("cache_read": "0.001")"),
                    "must not be cheaper");
    expect_rejected(with(base, R"("input": "0.40", "output": "0.80")", R"("input": "0.05", "output": "0.80")"),
                    "route ceiling is below a listed rate");
    expect_rejected(with(base, R"("routing": "aggregator")", R"("routing": "direct")"),
                    "only aggregator origins have a route ceiling");
    expect_rejected(with(base, R"("tiers": [{"input": "0.10", "output": "0.20"}]
    }
  ])", R"("tiers": []
    }
  ])"), "1 to 8 tiers");
    expect_rejected("{", "document");
}

TEST(PricingCatalog, LoadReportsMissingFiles) {
    EXPECT_THROW((void)PriceCatalog::load("/nonexistent/neograph/pricing.json"), CatalogError);
}

// ------------------------------------------------------- tiers and rounding

TEST(PricingTiers, InputTotalSelectsTheTierForTheWholeRequest) {
    const auto& catalog = tiered();
    const auto* entry   = catalog.find("example", "tiered-model");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(&entry->tier_for(0), &entry->tiers[0]);
    EXPECT_EQ(&entry->tier_for(100'000), &entry->tiers[0]);
    EXPECT_EQ(&entry->tier_for(100'001), &entry->tiers[1]);

    // 100000 input tokens at 0.10/M plus 1000 output tokens at 0.50/M.
    auto at_threshold = value_of(catalog.estimate("example", "tiered-model", banded(100'000, 0, 0, 1'000)));
    EXPECT_EQ(at_threshold.microunits, 10'000u + 500u);
    EXPECT_EQ(at_threshold.basis, Basis::ListRates);
    EXPECT_FALSE(at_threshold.conservative);
    // One more input token moves every token, output included, to the higher tier:
    // 100001 * 0.50 = 50000.5 and 1000 * 2.50 = 2500, rounded up once.
    auto above = value_of(catalog.estimate("example", "tiered-model", banded(100'001, 0, 0, 1'000)));
    EXPECT_EQ(above.microunits, 52'501u);
    // Cache reads count toward the prompt length that selects the tier.
    auto cached = value_of(catalog.estimate("example", "tiered-model", banded(1, 100'000, 0, 0)));
    EXPECT_EQ(cached.microunits, 5'001u);  // 1 * 0.50 + 100000 * 0.05 = 5000.5
}

TEST(PricingRounding, OneCeilingPerCallInWholeMicrounits) {
    const auto& catalog = shipped();
    // gpt-4.1-nano input is 0.10 USD per million tokens: one token is 0.1 microunit.
    EXPECT_EQ(value_of(catalog.estimate("openai", "gpt-4.1-nano", banded(1, 0, 0, 0))).microunits, 1u);
    EXPECT_EQ(value_of(catalog.estimate("openai", "gpt-4.1-nano", banded(10, 0, 0, 0))).microunits, 1u);
    EXPECT_EQ(value_of(catalog.estimate("openai", "gpt-4.1-nano", banded(11, 0, 0, 0))).microunits, 2u);
    EXPECT_EQ(value_of(catalog.estimate("openai", "gpt-4.1-nano", banded(0, 0, 0, 0))).microunits, 0u);
    // 0.1 (uncached) + 0.025 (cache read) + 0.4 (output) = 0.525: one microunit, not three.
    EXPECT_EQ(value_of(catalog.estimate("openai", "gpt-4.1-nano", banded(1, 1, 0, 1))).microunits, 1u);
    EXPECT_EQ(value_of(catalog.reserve("openai", "gpt-4.1-nano", 1, 1)).microunits, 1u);
}

// ---------------------------------------------------------------- cache bands

TEST(PricingCache, AnthropicBandsAndTheTtlSplit) {
    const auto& catalog = shipped();
    auto split = banded(1'000, 2'000, 3'000, 500);
    split.extra["cache_creation.ephemeral_5m_input_tokens"] = count(1'000);
    split.extra["cache_creation.ephemeral_1h_input_tokens"] = count(2'000);
    // 1000*1 + 2000*0.10 + 1000*1.25 + 2000*2 + 500*5
    auto exact = value_of(catalog.estimate("anthropic", "claude-haiku-4-5", split));
    EXPECT_EQ(exact.microunits, 1'000u + 200u + 1'250u + 4'000u + 2'500u);
    EXPECT_FALSE(exact.conservative);
    EXPECT_EQ(exact.evidence, Evidence::Estimated);
    EXPECT_EQ(exact.catalog_version, catalog.version());
    EXPECT_EQ(exact.catalog_digest, catalog.digest());

    // Without the split every written token is priced at the dearer 1-hour rate.
    auto unsplit = value_of(catalog.estimate("anthropic", "claude-haiku-4-5", banded(1'000, 2'000, 3'000, 500)));
    EXPECT_EQ(unsplit.microunits, 1'000u + 200u + 6'000u + 2'500u);
    EXPECT_TRUE(unsplit.conservative);

    // A split that does not add up to the written total is not trusted.
    auto wrong = split;
    wrong.extra["cache_creation.ephemeral_1h_input_tokens"] = count(1'999);
    EXPECT_EQ(value_of(catalog.estimate("anthropic", "claude-haiku-4-5", wrong)).microunits,
              unsplit.microunits);
}

TEST(PricingCache, OpenAiCacheReadsWithoutAWriteCounter) {
    const auto& catalog = shipped();
    // Chat on a direct origin has no cache-write counter; the SDK derives uncached input.
    auto chat           = usage(1'000'000, 100'000);
    chat.cache_read     = count(400'000);
    chat.input_uncached = count(600'000, sp::Evidence::Derived);
    auto settled        = value_of(catalog.settle("openai", "gpt-4.1-nano", chat));
    EXPECT_EQ(settled.microunits, 60'000u + 10'000u + 40'000u);
    EXPECT_FALSE(settled.conservative);
}

TEST(PricingCache, UnreportedBandsArePricedAtTheirHighestRate) {
    const auto& catalog = shipped();
    // Only the inclusive total is known: every input token could have been a 1-hour write.
    auto totals = value_of(catalog.estimate("anthropic", "claude-haiku-4-5", usage(1'000, 0)));
    EXPECT_EQ(totals.microunits, 2'000u);
    EXPECT_TRUE(totals.conservative);

    // Reads known, writes unknown: the remainder is uncached or written.
    auto reads       = usage(1'000, 0);
    reads.cache_read = count(400);
    auto partial     = value_of(catalog.estimate("anthropic", "claude-haiku-4-5", reads));
    EXPECT_EQ(partial.microunits, 40u + 1'200u);  // 400*0.10 + 600*2
    EXPECT_TRUE(partial.conservative);

    // Gemini omits the cached count when nothing was read; the input rate is still the ceiling.
    auto gemini = value_of(catalog.estimate("google-vertex-global", "gemini-3.1-flash-lite-image",
                                            usage(1'000'000, 0)));
    EXPECT_EQ(gemini.microunits, 250'000u);
}

TEST(PricingCache, ContradictoryBandsAreRejected) {
    const auto& catalog = shipped();
    auto over       = usage(100, 0);
    over.cache_read = count(101);
    EXPECT_EQ(error_of(catalog.estimate("openai", "gpt-4.1-nano", over)), PricingError::InconsistentUsage);

    auto gap = banded(10, 10, 10, 0);
    gap.input_total = count(31);  // every band reported, one token unaccounted for
    EXPECT_EQ(error_of(catalog.estimate("openai", "gpt-4.1-nano", gap)), PricingError::InconsistentUsage);

    auto flagged    = banded(10, 0, 0, 0);
    flagged.quality = sp::UsageQuality::Inconsistent;
    EXPECT_EQ(error_of(catalog.estimate("openai", "gpt-4.1-nano", flagged)),
              PricingError::InconsistentUsage);
}

TEST(PricingOutput, GeminiImageTokensUseTheImageRate) {
    const auto& catalog = shipped();
    auto image = usage(1'000, 2'000);
    image.extra["candidatesTokensDetails.IMAGE"] = count(1'120);
    // 1000*0.25 + 1120*30 + 880*1.50
    auto exact = value_of(catalog.estimate("google-ai-studio", "gemini-3.1-flash-lite-image", image));
    EXPECT_EQ(exact.microunits, 250u + 33'600u + 1'320u);
    // Without the modality breakdown all output is priced as image output.
    auto unknown = value_of(catalog.estimate("google-ai-studio", "gemini-3.1-flash-lite-image",
                                             usage(1'000, 2'000)));
    EXPECT_EQ(unknown.microunits, 250u + 60'000u);
    EXPECT_TRUE(unknown.conservative);

    image.extra["candidatesTokensDetails.IMAGE"] = count(2'001);
    EXPECT_EQ(error_of(catalog.estimate("google-ai-studio", "gemini-3.1-flash-lite-image", image)),
              PricingError::InconsistentUsage);
}

// ------------------------------------------------------- reported settlement

TEST(PricingSettle, ReportedCostWinsAndRoundsUp) {
    const auto& catalog = shipped();
    auto call           = banded(1'000, 0, 0, 100);
    call.provider_cost  = openrouter_cost(1'234'567);  // 0.001234567 USD
    auto settled        = value_of(catalog.settle("openrouter", "deepseek/deepseek-v4-flash-0731", call));
    EXPECT_EQ(settled.evidence, Evidence::Reported);
    EXPECT_EQ(settled.basis, Basis::ProviderReported);
    EXPECT_EQ(settled.microunits, 1'235u);
    EXPECT_TRUE(settled.catalog_version.empty());
    // A reported cost needs no catalog entry.
    EXPECT_EQ(value_of(catalog.settle("openrouter", "vendor/unlisted", call)).microunits, 1'235u);
    call.provider_cost = openrouter_cost(2'000);
    EXPECT_EQ(value_of(catalog.settle("openrouter", "vendor/unlisted", call)).microunits, 2u);
    call.provider_cost = openrouter_cost(0);
    EXPECT_EQ(value_of(catalog.settle("openrouter", "vendor/unlisted", call)).microunits, 0u);
}

TEST(PricingSettle, ByokAddsTheUpstreamCharge) {
    auto cost              = openrouter_cost(50'000);
    cost.is_byok           = true;
    cost.upstream_total    = sp::UsdAmount{1'000'000};
    cost.status[1]         = sp::CostStatus::Available;
    EXPECT_EQ(reported_cost_microunits(cost), 1'050u);

    auto missing_upstream    = openrouter_cost(50'000);
    missing_upstream.is_byok = true;
    EXPECT_FALSE(reported_cost_microunits(missing_upstream).has_value());

    // Unknown BYOK flag with an upstream amount: add it rather than risk under-counting.
    auto unknown        = cost;
    unknown.is_byok     = std::nullopt;
    unknown.byok_status = sp::CostStatus::Missing;
    EXPECT_EQ(reported_cost_microunits(unknown), 1'050u);
    unknown.upstream_total = std::nullopt;
    unknown.status[1]      = sp::CostStatus::Missing;
    EXPECT_EQ(reported_cost_microunits(unknown), 50u);

    // Not BYOK: OpenRouter's total already includes the upstream inference.
    auto direct           = openrouter_cost(50'000);
    direct.upstream_total = sp::UsdAmount{40'000};
    direct.status[1]      = sp::CostStatus::Available;
    EXPECT_EQ(reported_cost_microunits(direct), 50u);
}

TEST(PricingSettle, UnusableReportsFallBackToTheCatalog) {
    const auto& catalog = shipped();
    auto call           = banded(1'000'000, 0, 0, 0);
    auto expect_estimate = [&](const sp::ProviderReportedCost& cost) {
        call.provider_cost = cost;
        auto settled = value_of(catalog.settle("openrouter", "deepseek/deepseek-v4-flash-0731", call));
        EXPECT_EQ(settled.evidence, Evidence::Estimated);
        // No reported cost: the served endpoint is unknown, so the route ceiling prices it.
        EXPECT_EQ(settled.basis, Basis::RouteCeiling);
        EXPECT_EQ(settled.microunits, 440'000u);
    };
    expect_estimate(sp::ProviderReportedCost{});
    auto unknown_currency   = openrouter_cost(1);
    unknown_currency.source = sp::CostSource::UnknownCurrency;
    expect_estimate(unknown_currency);
    auto inconsistent    = openrouter_cost(1);
    inconsistent.quality = sp::UsageQuality::Inconsistent;
    expect_estimate(inconsistent);
    auto missing_total      = openrouter_cost(1);
    missing_total.total     = std::nullopt;
    missing_total.status[0] = sp::CostStatus::Malformed;
    expect_estimate(missing_total);
}

TEST(PricingSettle, PartialOrMissingUsageKeepsTheHold) {
    const auto& catalog = shipped();
    auto partial          = banded(10, 0, 0, 10);
    partial.stage         = sp::UsageStage::Partial;
    partial.provider_cost = openrouter_cost(5'000);
    EXPECT_EQ(error_of(catalog.settle("openrouter", "deepseek/deepseek-v4-flash-0731", partial)),
              PricingError::IncompleteUsage);
    auto no_output         = banded(10, 0, 0, 0);
    no_output.output_total = std::nullopt;
    EXPECT_EQ(error_of(catalog.settle("openai", "gpt-4.1-mini", no_output)), PricingError::IncompleteUsage);
    auto no_input        = banded(10, 0, 0, 0);
    no_input.input_total = std::nullopt;
    EXPECT_EQ(error_of(catalog.estimate("openai", "gpt-4.1-mini", no_input)), PricingError::IncompleteUsage);
}

// ------------------------------------------------- reservations and bounds

TEST(PricingReserve, PricesEveryTokenAtItsHighestRate) {
    const auto& catalog = shipped();
    // gpt-4.1-nano: 1M input at 0.10 plus 100k output at 0.40.
    auto nano = value_of(catalog.reserve("openai", "gpt-4.1-nano", 1'000'000, 100'000));
    EXPECT_EQ(nano.microunits, 100'000u + 40'000u);
    EXPECT_EQ(nano.basis, Basis::ListRates);
    EXPECT_EQ(nano.catalog_version, catalog.version());
    // Haiku 4.5: the 1-hour cache write (2/M) is the dearest input band.
    EXPECT_EQ(value_of(catalog.reserve("anthropic", "claude-haiku-4-5", 1'000, 1'000)).microunits,
              2'000u + 5'000u);
    // Gemini image model: output is bounded at the image rate.
    EXPECT_EQ(value_of(catalog.reserve("google-ai-studio", "gemini-3.1-flash-lite-image", 0, 1'000))
                  .microunits,
              30'000u);
    // The tier follows the input bound.
    EXPECT_EQ(value_of(tiered().reserve("example", "tiered-model", 100'000, 1'000)).microunits,
              20'000u + 500u);
    EXPECT_EQ(value_of(tiered().reserve("example", "tiered-model", 100'001, 1'000)).microunits,
              100'001u + 2'500u);
}

TEST(PricingReserve, AggregatorsReserveAtTheRouteCeiling) {
    const auto& catalog = shipped();
    auto dated = value_of(catalog.reserve("openrouter", "deepseek/deepseek-v4-flash-0731", 1'000'000, 1'000'000));
    EXPECT_EQ(dated.basis, Basis::RouteCeiling);
    EXPECT_EQ(dated.microunits, 440'000u + 1'320'000u);
    // A router alias lists no endpoints, so nothing bounds it.
    EXPECT_EQ(error_of(catalog.reserve("openrouter", "~deepseek/deepseek-v4-flash-latest", 1, 1)),
              PricingError::NoUpperBound);
    auto listed = value_of(catalog.estimate("openrouter", "~deepseek/deepseek-v4-flash-latest",
                                            banded(1'000'000, 0, 0, 0)));
    EXPECT_EQ(listed.basis, Basis::ListRates);
    EXPECT_TRUE(listed.may_undercount);
    EXPECT_EQ(listed.microunits, 18'000u);
}

TEST(PricingReserve, OverflowIsAnErrorNotAWrap) {
    const auto& catalog = shipped();
    EXPECT_EQ(error_of(catalog.reserve("anthropic", "claude-opus-5", kMax, kMax)), PricingError::Overflow);
    EXPECT_EQ(error_of(catalog.estimate("anthropic", "claude-opus-5", banded(kMax, 0, 0, kMax))),
              PricingError::Overflow);
    // A huge amount that still fits is exact: (2^64 - 1) tokens at 0.40 USD per million.
    EXPECT_EQ(value_of(tiered().reserve("router", "vendor/model", kMax, 0)).microunits,
              7'378'697'629'483'820'646u);
    EXPECT_EQ(value_of(tiered().reserve("router", "vendor/model", 0, 0)).microunits, 0u);
}

TEST(PricingReserve, UnknownModelsHaveNoPrice) {
    const auto& catalog = shipped();
    EXPECT_EQ(catalog.find("openai", "gpt-unknown"), nullptr);
    EXPECT_EQ(error_of(catalog.reserve("openai", "gpt-unknown", 1, 1)), PricingError::UnknownModel);
    EXPECT_EQ(error_of(catalog.estimate("openai", "gpt-unknown", banded(1, 0, 0, 1))),
              PricingError::UnknownModel);
    // Without a reported cost, settlement needs an entry too.
    EXPECT_EQ(error_of(catalog.settle("openai", "gpt-unknown", banded(1, 0, 0, 1))),
              PricingError::UnknownModel);
    // An entry under another origin is not a match.
    EXPECT_EQ(error_of(catalog.reserve("openrouter", "gpt-4.1-nano", 1, 1)), PricingError::UnknownModel);
    EXPECT_EQ(error_of(catalog.reserve("nowhere", "gpt-4.1-nano", 1, 1)), PricingError::UnknownModel);
}

TEST(PricingReserve, EstimatesNeverExceedTheirReservation) {
    std::mt19937_64 random(366);
    const auto draw = [&](std::uint64_t upper) {
        return std::uniform_int_distribution<std::uint64_t>(0, upper)(random);
    };
    const auto coin = [&] { return (random() & 1) != 0; };
    std::size_t checked = 0;
    for (const auto* catalog : {&shipped(), &tiered()}) {
        for (const auto& entry : catalog->entries()) {
            if (catalog->find_origin(entry.origin)->aggregator && !entry.route_ceiling) continue;
            for (int round = 0; round < 400; ++round) {
                const auto input_bound  = draw(coin() ? 300'000 : 2'000'000);
                const auto output_bound = draw(coin() ? 4'096 : 200'000);
                const auto reserved = catalog->reserve(entry.origin, entry.model, input_bound, output_bound);
                ASSERT_TRUE(std::holds_alternative<Reservation>(reserved));

                const auto input  = draw(input_bound);
                const auto read   = draw(input);
                const auto write  = draw(input - read);
                const auto output = draw(output_bound);
                auto call         = usage(input, output);
                if (coin()) call.input_uncached = count(input - read - write, sp::Evidence::Derived);
                if (coin()) call.cache_read = count(read);
                if (coin()) {
                    call.cache_write = count(write);
                    if (coin()) {
                        const auto five = draw(write);
                        call.extra["cache_creation.ephemeral_5m_input_tokens"] = count(five);
                        call.extra["cache_creation.ephemeral_1h_input_tokens"] = count(write - five);
                    }
                }
                if (coin()) call.extra["candidatesTokensDetails.IMAGE"] = count(draw(output));
                // With some bands unreported the remainder may still be unattributable.
                const auto estimated = catalog->estimate(entry.origin, entry.model, call);
                if (const auto* error = std::get_if<PricingError>(&estimated)) {
                    EXPECT_EQ(*error, PricingError::InconsistentUsage);
                    continue;
                }
                EXPECT_LE(std::get<Settlement>(estimated).microunits,
                          std::get<Reservation>(reserved).microunits)
                    << entry.origin << " " << entry.model << " in=" << input << "/" << input_bound
                    << " out=" << output << "/" << output_bound;
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 3'000u);
}
