#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/llm/endpoint_types.h>
#include <asio/awaitable.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace neograph::llm {

// Guidance and state are protocol-defined string/object/array JSON, not an
// endpoint descriptor. Choice criteria alone may also contain null guidance.
struct DecisionsNoulCriteria { json when_false, when_true; };
struct DecisionsNoulQuestion {
    json instructions;
    std::optional<DecisionsNoulCriteria> criteria;
};
struct DecisionsChoiceQuestion {
    json instructions;
    std::map<std::string, json> criteria;
};
struct DecisionsScoreQuestion {
    json instructions;
    std::vector<json> criteria;
};
using DecisionsQuestion = std::variant<DecisionsNoulQuestion,
    DecisionsChoiceQuestion, DecisionsScoreQuestion>;

enum class DecisionsDataCollection { Allow, Deny };
enum class DecisionsSortBy { Price, Throughput, Latency, Exacto };
enum class DecisionsPartition { Model, None };
enum class DecisionsQuantization {
    Int4, Int8, Fp4, Mxfp4, Nvfp4, Fp6, Fp8, Mxfp8, Fp16, Bf16, Fp32, Unknown
};
struct DecisionsSort {
    std::optional<DecisionsSortBy> by;
    std::optional<DecisionsPartition> partition;
};
struct DecisionsPercentiles {
    std::optional<double> p50, p75, p90, p99;
};
using DecisionsPreference = std::variant<double, DecisionsPercentiles>;
struct DecisionsMaxPrice {
    std::optional<std::string> prompt, completion, image, audio, request;
};
struct DecisionsProviderPreferences {
    std::optional<bool> allow_fallbacks, enforce_distillable_text,
        require_parameters, zdr;
    std::optional<DecisionsDataCollection> data_collection;
    std::optional<std::vector<std::string>> order, only, ignore;
    std::optional<DecisionsMaxPrice> max_price;
    std::optional<std::vector<DecisionsQuantization>> quantizations;
    std::optional<std::variant<DecisionsSortBy, DecisionsSort>> sort;
    std::optional<DecisionsPreference> preferred_max_latency,
        preferred_min_throughput;
    // Current Jev catalogue advertises no supported parameters. Nonempty
    // provider-specific options fail before dispatch instead of being dropped.
    std::map<std::string, json> options;
};
struct DecisionsRequest {
    std::optional<std::string> model;
    json state;
    std::map<std::string, DecisionsQuestion> questions;
    std::optional<DecisionsProviderPreferences> provider;
    std::optional<std::string> session_id, user;
    std::optional<json> trace;
};

struct DecisionsNoulAnswer { double probability = 0; };
struct DecisionsChoiceAnswer {
    std::string choice;
    std::optional<double> confidence;
    std::optional<std::map<std::string, double>> probabilities;
};
struct DecisionsScoreAnswer {
    double score = 0;
    std::optional<double> confidence;
    std::optional<std::map<std::string, double>> probabilities;
    std::optional<std::map<std::string, json>> legend;
};
using DecisionsAnswer = std::variant<DecisionsNoulAnswer,
    DecisionsChoiceAnswer, DecisionsScoreAnswer>;
struct DecisionsResponse {
    std::optional<std::string> id, provider;
    std::string model;
    std::map<std::string, DecisionsAnswer> answers;
    EndpointUsage usage;
    std::optional<double> cost_usd;
};
using DecisionsResult = std::variant<DecisionsResponse, EndpointFailure>;

class NEOGRAPH_API DecisionsConfig final {
public:
    // Closed version-1 operational JSON. No credentials in the document.
    static std::variant<DecisionsConfig, EndpointFailure> from_json(const json& document);
private:
    struct Data;
    explicit DecisionsConfig(std::shared_ptr<const Data> data);
    std::shared_ptr<const Data> data_;
    friend class DecisionsClient;
};

class NEOGRAPH_API DecisionsClient final {
public:
    DecisionsClient(DecisionsConfig config, std::string credential);
    // Deadline is absolute; omitting it establishes the external config's
    // timeout at this call (not when the coroutine is eventually scheduled).
    // The client may be destroyed after this call: its awaitable owns state.
    asio::awaitable<DecisionsResult> submit_async(
        DecisionsRequest request,
        std::shared_ptr<graph::CancelToken> cancellation = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = {}) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    static asio::awaitable<DecisionsResult> submit_owned(
        std::shared_ptr<const Impl> impl, DecisionsRequest request,
        std::shared_ptr<graph::CancelToken> cancellation,
        std::chrono::steady_clock::time_point deadline);
};

} // namespace neograph::llm
