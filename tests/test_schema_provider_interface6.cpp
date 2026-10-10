#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <neograph/provider_outcome_codec.h>
#include <core/interface_contract.h>
#include <core/native.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

using namespace neograph;
using namespace std::chrono_literals;
namespace wire = neograph::test::wire;
namespace {

// The host explicitly admits this loopback origin as USD. Production defaults
// are untouched; unknown origins must not inherit OpenRouter currency facts.
sp::descriptor::ValidatedDescriptor cost_descriptor(std::string family, const std::string& origin,
                                                    bool usd = true) {
    if (!usd) return test::descriptor(std::move(family), origin);
    auto policy = json::parse(sp::config_defaults::descriptor_policy_json);
    json defaults;
    for (auto row : policy.at("families")) {
        if (row.at("family") == family) {
            row.at("openrouter_origins").push_back(origin);
            defaults = row.at("defaults");
        }
    }
    policy.at("models").push_back({{"family", family}, {"model", "fixture-model"},
        {"defaults", std::move(defaults)}, {"output_limit", 64}, {"input_limit", 100}});
    auto admitted = sp::descriptor::load_policy(policy.dump(), sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::runtime_error(error->pointer + ": " + error->message);
    return test::descriptor(std::move(family), origin,
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted)));
}
sp::runtime::Options runtime_options() {
    sp::runtime::Options options;
    options.workers = 1;
    options.default_timeout = 5s;
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    return options;
}
void body(wire::Peer& peer, const std::string& value) {
    std::lock_guard lock(peer.state->mutex);
    peer.state->body = value;
}
std::string response_frame(std::string type, json payload) {
    payload["type"] = type;
    return "event: " + type + "\ndata: " + payload.dump() + "\n\n";
}
std::string response_prefix() {
    auto stream = wire::responses_sse();
    const auto terminal = stream.find("event: response.completed\n");
    if (terminal == std::string::npos) throw std::runtime_error("fixture terminal missing");
    stream.resize(terminal);
    return stream;
}
json response_with_usage(json usage) {
    auto value = json::parse(wire::responses_body());
    value["usage"] = std::move(usage);
    return value;
}
sp::runtime::Result prepared_run(Provider& provider, std::string_view family,
                                ProviderMode mode = ProviderMode::Collect) {
    auto prepared = provider.prepare(wire::request(family, mode));
    if (!prepared.valid() || prepared.error()) throw std::runtime_error("fixture preparation rejected");
    return async::run_sync(provider.dispatch_async(std::move(prepared)));
}

class ArchiveDirectory {
public:
    ArchiveDirectory() {
        static std::atomic<unsigned> sequence{0};
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            root_ = parent / ("ng-interface6-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(root_)) {
                std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace);
                return;
            }
        }
        throw std::runtime_error("cannot create archive fixture directory");
    }
    ~ArchiveDirectory() { std::error_code error; std::filesystem::remove_all(root_, error); }
    std::string records() const { return (root_ / "records").string(); }
    std::string key() const { return (root_ / "activation").string(); }
private:
    std::filesystem::path root_;
};
std::shared_ptr<sp::NativeArchive> archive_activation(sp::NativeArchive::Activation value) {
    if (const auto* error = std::get_if<sp::Error>(&value)) throw std::runtime_error(error->safe_message);
    return std::get<std::shared_ptr<sp::NativeArchive>>(std::move(value));
}
void expect_usage_roundtrip(const sp::Usage& usage) {
    const auto encoded = provider_codec::encode_usage(usage);
    const auto restored = provider_codec::decode_usage(json::parse(encoded.dump()));
    EXPECT_EQ(provider_codec::encode_usage(restored), encoded);
}
void expect_archive_roundtrip(const sp::runtime::Result& result,
                              const std::shared_ptr<sp::NativeArchive>& archive,
                              std::string_view binding) {
    const auto observed = provider_codec::observe_outcome(*result);
    const auto encoded = provider_codec::encode_outcome(*result, archive, binding);
    const auto restored = provider_codec::decode_outcome(json::parse(encoded.dump()), archive, binding);
    EXPECT_EQ(provider_codec::observe_outcome(*restored), observed);
    expect_usage_roundtrip(outcome_usage(*restored));
    EXPECT_THROW(provider_codec::decode_outcome(encoded, archive, "wrong-binding"), std::runtime_error);
}

// No sleep fence: the watchdog waits on completion state. If a blocking wrapper
// mistakenly waits for its own callback, fail the process instead of hanging CI.
class CallbackWatchdog {
public:
    CallbackWatchdog() : worker_([this] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, 10s, [this] { return finished_; })) {
            std::fputs("SchemaProvider callback re-entry deadlocked\n", stderr);
            std::abort();
        }
    }) {}
    ~CallbackWatchdog() {
        { std::lock_guard lock(mutex_); finished_ = true; }
        cv_.notify_all();
        worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool finished_ = false;
    std::thread worker_;
};
} // namespace

TEST(SchemaProviderInterface6, LinkedAbiGateAdmitsRealPreparedDispatch) {
    EXPECT_EQ(sp::core_interface_contract().revision, 6U);
    EXPECT_EQ(sp::codec_interface_revision(), 6U);
    EXPECT_EQ(sp::runtime::interface_contract().revision, 6U);
    EXPECT_NO_THROW(sp::runtime::require_interface_contract(6, sp::capability::RequiredProvider));
    EXPECT_THROW(sp::runtime::require_interface_contract(5, sp::capability::RequiredProvider),
                 sp::runtime::InterfaceContractError);
    wire::Peer peer(wire::chat_response());
    auto provider = wire::provider("openai.chat", peer.origin());
    const auto result = prepared_run(*provider, "openai.chat");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "pong");
}

TEST(SchemaProviderInterface6, MissingCostsKeepOldCanonicalUsageBytesAndZeroIsEvidence) {
    wire::Peer peer(wire::responses_body());
    auto descriptor = cost_descriptor("openai.responses", peer.origin());
    auto provider = llm::SchemaProvider::create(descriptor, runtime_options());
    const auto missing = prepared_run(*provider, "openai.responses");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*missing));
    const auto encoded_missing = provider_codec::encode_usage(outcome_usage(*missing));
    EXPECT_FALSE(encoded_missing.contains("provider_cost"));
    const auto restored_missing = provider_codec::decode_usage(json::parse(encoded_missing.dump()));
    EXPECT_EQ(restored_missing.provider_cost.source, sp::CostSource::None);
    for (const auto status : restored_missing.provider_cost.status) EXPECT_EQ(status, sp::CostStatus::Missing);
    EXPECT_FALSE(restored_missing.provider_cost.total);
    EXPECT_EQ(provider_codec::encode_usage(restored_missing), encoded_missing);

    body(peer, response_with_usage({{"cost", 0}, {"is_byok", false}}).dump());
    const auto zero = prepared_run(*provider, "openai.responses");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*zero));
    const auto& usage = outcome_usage(*zero);
    ASSERT_TRUE(usage.provider_cost.total);
    EXPECT_EQ(usage.provider_cost.total->nano_usd, 0U);
    EXPECT_EQ(usage.provider_cost.status[0], sp::CostStatus::Available);
    ASSERT_TRUE(usage.provider_cost.is_byok.has_value());
    EXPECT_FALSE(*usage.provider_cost.is_byok);
    EXPECT_EQ(usage.provider_cost.byok_status, sp::CostStatus::Available);
    EXPECT_EQ(usage.provider_cost.source, sp::CostSource::OpenRouterUsd);
    EXPECT_FALSE(usage.total);
    EXPECT_TRUE(provider_codec::encode_usage(usage).contains("provider_cost"));
    expect_usage_roundtrip(usage);
    ArchiveDirectory directory;
    auto archive = archive_activation(sp::NativeArchive::provision(
        directory.records(), directory.key(), "zero-owner", descriptor));
    expect_archive_roundtrip(missing, archive, "missing");
    expect_archive_roundtrip(zero, archive, "available-zero");

    const auto encoded_zero = provider_codec::encode_usage(usage);
    for (const auto& corrupted : std::vector<json>{
        [&] { auto value = encoded_zero; value["provider_cost"]["status"][0] = 99; return value; }(),
        [&] { auto value = encoded_zero; value["provider_cost"]["total"] = nullptr; return value; }(),
        [&] { auto value = encoded_zero; value["provider_cost"]["source"] = static_cast<int>(sp::CostSource::UnknownCurrency); return value; }(),
        [&] { auto value = encoded_zero; value["provider_cost"]["is_byok"] = nullptr; return value; }(),
        [&] { auto value = encoded_zero; value["provider_cost"]["total"]["rounding"] = 99; return value; }()})
        EXPECT_THROW(provider_codec::decode_usage(corrupted), std::runtime_error);

    UsageAccumulator bank;
    ASSERT_TRUE(bank.try_reserve(7, 7));
    bank.settle_reservation(7, usage);
    EXPECT_EQ(bank.total_tokens_wide(), 7U);
    EXPECT_FALSE(bank.try_reserve(1, 7));
    const auto before = bank.authority_snapshot();
    bank.observe(provider_codec::decode_usage(provider_codec::encode_usage(usage)));
    EXPECT_EQ(bank.authority_snapshot().charged, before.charged);
    EXPECT_EQ(bank.authority_snapshot().reserved, before.reserved);
    EXPECT_THROW(bank.restore_authority({}), std::logic_error);

    // Isolate the not-sent predicate from unrelated message/wire evidence. The
    // monetary evidence itself comes from the real prepared SDK result above.
    sp::Failure contradictory;
    contradictory.error.retry_safety = sp::RetrySafety::NotSent;
    EXPECT_TRUE(provider_failure_proves_not_sent(contradictory));
    contradictory.partial.usage.provider_cost = usage.provider_cost;
    EXPECT_FALSE(provider_failure_proves_not_sent(contradictory));
}

TEST(SchemaProviderInterface6, EveryMonetaryFieldStatusSurvivesObservationAndTrustedArchive) {
    wire::Peer peer(wire::responses_body());
    auto descriptor = cost_descriptor("openai.responses", peer.origin());
    auto provider = llm::SchemaProvider::create(descriptor, runtime_options());
    ArchiveDirectory directory;
    auto archive = archive_activation(sp::NativeArchive::provision(
        directory.records(), directory.key(), "interface6-owner", descriptor));
    constexpr std::array<const char*, 4> names{
        "cost", "upstream_inference_cost", "upstream_inference_input_cost", "upstream_inference_output_cost"};
    struct Case { const char* amount; sp::CostStatus status; std::uint64_t nanos; };
    const std::array cases{
        Case{"null", sp::CostStatus::Missing, 0}, Case{"0", sp::CostStatus::Available, 0},
        Case{"0.1", sp::CostStatus::Available, 100000001},
        Case{"1e-10", sp::CostStatus::Available, 1},
        Case{"\"invalid\"", sp::CostStatus::Malformed, 0},
        Case{"9007199.5", sp::CostStatus::PrecisionExceeded, 0},
        Case{"1e100", sp::CostStatus::Overflow, 0}};
    for (std::size_t field = 0; field != names.size(); ++field) {
        for (const auto& scenario : cases) {
            SCOPED_TRACE(std::string(names[field]) + ": " + scenario.amount);
            json usage;
            if (field == 0) usage = {{names[field], json::parse(scenario.amount)}};
            else usage = {{"cost_details", {{names[field], json::parse(scenario.amount)}}}};
            usage["is_byok"] = false;
            body(peer, response_with_usage(usage).dump());
            const auto result = prepared_run(*provider, "openai.responses");
            ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
            const auto& cost = outcome_usage(*result).provider_cost;
            const std::array amounts{cost.total, cost.upstream_total, cost.upstream_input, cost.upstream_output};
            EXPECT_EQ(cost.status[field], scenario.status);
            EXPECT_EQ(cost.source, sp::CostSource::OpenRouterUsd);
            EXPECT_EQ(cost.quality, scenario.status == sp::CostStatus::Available || scenario.status == sp::CostStatus::Missing
                ? sp::UsageQuality::Consistent : sp::UsageQuality::Inconsistent);
            if (scenario.status == sp::CostStatus::Available) {
                ASSERT_TRUE(amounts[field]);
                EXPECT_EQ(amounts[field]->nano_usd, scenario.nanos);
                EXPECT_EQ(amounts[field]->evidence, sp::Evidence::Reported);
                EXPECT_EQ(amounts[field]->rounding, sp::CostRounding::CeilingParsedBinary64);
            } else EXPECT_FALSE(amounts[field]);
            expect_archive_roundtrip(result, archive, names[field]);
        }
    }
    body(peer, response_with_usage(json::parse(R"({"cost":4e-6,"is_byok":false,"cost_details":{"upstream_inference_cost":4e-6,"upstream_inference_input_cost":1e-10,"upstream_inference_prompt_cost":2e-10,"upstream_inference_output_cost":2.4e-6,"upstream_inference_completions_cost":2.5e-6}})")).dump());
    const auto conflict = prepared_run(*provider, "openai.responses");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*conflict));
    const auto& cost = outcome_usage(*conflict).provider_cost;
    EXPECT_EQ(cost.status[2], sp::CostStatus::Conflict);
    EXPECT_EQ(cost.status[3], sp::CostStatus::Conflict);
    EXPECT_FALSE(cost.upstream_input);
    EXPECT_FALSE(cost.upstream_output);
    EXPECT_EQ(cost.quality, sp::UsageQuality::Inconsistent);
    expect_archive_roundtrip(conflict, archive, "conflicts");
    auto stored = provider_codec::encode_outcome(*conflict, archive, "reopen");
    archive.reset();
    archive = archive_activation(sp::NativeArchive::open(
        directory.records(), directory.key(), "interface6-owner", descriptor));
    EXPECT_EQ(provider_codec::observe_outcome(*provider_codec::decode_outcome(stored, archive, "reopen")),
              provider_codec::observe_outcome(*conflict));
    stored["usage"]["provider_cost"]["is_byok"] = true;
    EXPECT_THROW(provider_codec::decode_outcome(stored, archive, "reopen"), std::runtime_error);
}

TEST(SchemaProviderInterface6, UnknownCurrencyAndMalformedByokRemainTypedEvidence) {
    wire::Peer peer(response_with_usage(json::parse(R"({"cost":0,"is_byok":false,"cost_details":{"upstream_inference_cost":0,"upstream_inference_input_cost":0,"upstream_inference_output_cost":0}})")).dump());
    auto provider = wire::provider("openai.responses", peer.origin());
    const auto result = prepared_run(*provider, "openai.responses");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& cost = outcome_usage(*result).provider_cost;
    EXPECT_EQ(cost.source, sp::CostSource::UnknownCurrency);
    for (const auto status : cost.status) EXPECT_EQ(status, sp::CostStatus::UnknownCurrency);
    EXPECT_FALSE(cost.total);
    EXPECT_FALSE(cost.upstream_total);
    EXPECT_FALSE(cost.upstream_input);
    EXPECT_FALSE(cost.upstream_output);
    EXPECT_EQ(cost.quality, sp::UsageQuality::Inconsistent);
    expect_usage_roundtrip(outcome_usage(*result));
    ArchiveDirectory unknown_directory;
    auto unknown_archive = archive_activation(sp::NativeArchive::provision(
        unknown_directory.records(), unknown_directory.key(), "unknown-owner",
        test::descriptor("openai.responses", peer.origin())));
    expect_archive_roundtrip(result, unknown_archive, "unknown-currency");

    provider = llm::SchemaProvider::create(cost_descriptor("openai.responses", peer.origin()), runtime_options());
    body(peer, response_with_usage({{"is_byok", "false"}, {"cost_details", json::array()}}).dump());
    const auto malformed = prepared_run(*provider, "openai.responses");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*malformed));
    EXPECT_EQ(outcome_usage(*malformed).provider_cost.byok_status, sp::CostStatus::Malformed);
    EXPECT_FALSE(outcome_usage(*malformed).provider_cost.is_byok);
    expect_usage_roundtrip(outcome_usage(*malformed));
    ArchiveDirectory malformed_directory;
    auto malformed_archive = archive_activation(sp::NativeArchive::provision(
        malformed_directory.records(), malformed_directory.key(), "malformed-owner",
        cost_descriptor("openai.responses", peer.origin())));
    expect_archive_roundtrip(malformed, malformed_archive, "malformed-byok");
}

TEST(SchemaProviderInterface6, ResponsesFinalSnapshotDoesNotRetainEarlierCostEvidence) {
    auto initial = response_with_usage({{"cost", 4e-6}, {"is_byok", false}});
    initial["status"] = "in_progress";
    initial["output"] = json::array();
    auto final = json::parse(wire::responses_body());
    final["output"] = json::array();
    const auto created = response_frame("response.created", {{"response", initial}});
    wire::Peer peer(created + response_frame("response.done", {{"response", final}}), true);
    auto provider = llm::SchemaProvider::create(cost_descriptor("openai.responses", peer.origin()), runtime_options());
    const auto result = prepared_run(*provider, "openai.responses", ProviderMode::Stream);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& completion = test::completion(result);
    EXPECT_EQ(completion.usage.provider_cost.source, sp::CostSource::None);
    EXPECT_FALSE(completion.usage.provider_cost.total);
    EXPECT_FALSE(provider_codec::encode_usage(completion.usage).contains("provider_cost"));
    ASSERT_EQ(completion.raw_events.size(), 2U);
    EXPECT_TRUE(completion.raw_events.front().payload->root().get("response").get("usage").get("cost").is_number());
    expect_usage_roundtrip(completion.usage);
}

TEST(SchemaProviderInterface6, PartialFailureRetainsCostsWithoutRefundingUnknownTokens) {
    auto initial = response_with_usage({{"cost", 0}, {"is_byok", false}});
    initial["status"] = "in_progress";
    initial["output"] = json::array();
    wire::Peer peer(response_frame("response.created", {{"response", initial}}), true);
    auto descriptor = cost_descriptor("openai.responses", peer.origin());
    auto provider = llm::SchemaProvider::create(descriptor, runtime_options());
    auto prepared = provider->prepare(wire::request("openai.responses", ProviderMode::Stream));
    ASSERT_TRUE(prepared.valid());
    ASSERT_EQ(prepared.error(), nullptr);
    auto bank = std::make_shared<UsageAccumulator>();
    auto exhausted = std::make_shared<std::atomic_bool>(false);
    const auto ceiling = Provider::conservative_token_upper_bound(prepared);
    ASSERT_TRUE(ceiling);
    auto claim = reserve_provider_dispatch(prepared, ProviderDispatchBudget{bank, *ceiling, exhausted});
    ASSERT_TRUE(claim.active());
    const auto reservation = claim.amount();
    claim.mark_dispatched();
    const auto result = async::run_sync(provider->dispatch_async(std::move(prepared)));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::Truncated);
    const auto& usage = outcome_usage(*result);
    EXPECT_EQ(usage.stage, sp::UsageStage::Partial);
    ASSERT_TRUE(usage.provider_cost.total);
    EXPECT_EQ(usage.provider_cost.total->nano_usd, 0U);
    EXPECT_FALSE(provider_failure_proves_not_sent(wire::failure(result)));
    EXPECT_EQ(claim.settle(result), reservation);
    EXPECT_EQ(bank->total_tokens_wide(), reservation);
    EXPECT_FALSE(bank->try_reserve(1, *ceiling));
    ArchiveDirectory directory;
    auto archive = archive_activation(sp::NativeArchive::provision(
        directory.records(), directory.key(), "partial-owner", descriptor));
    expect_archive_roundtrip(result, archive, "partial-call");
}

TEST(SchemaProviderInterface6, ThinBlockingWrappersRejectRuntimeCallbackWorkerReentry) {
    wire::Peer peer(wire::chat_response());
    { std::lock_guard lock(peer.state->mutex); peer.state->hold = true; }
    sp::runtime::Client client(test::descriptor("openai.chat", peer.origin()), runtime_options());
    auto payload = [] { return wire::request().payload; };
    sp::runtime::RunOptions run;
    run.streaming = false;
    sp::runtime::Result nested_request, nested_prepared, nested_join, callback_result;
    unsigned callbacks = 0;
    sp::runtime::Operation operation;
    sp::runtime::Callbacks callback;
    callback.on_outcome = [&](sp::runtime::Result result) {
        ++callbacks;
        callback_result = std::move(result);
        nested_request = client.complete(payload(), run);
        nested_prepared = client.complete(client.prepare(payload(), run));
        nested_join = operation.join();
    };
    CallbackWatchdog watchdog;
    operation = client.start(client.prepare(payload(), run), std::move(callback));
    ASSERT_TRUE(peer.await_requests(1));
    // Peer release is the state fence: operation has been assigned before its
    // first response can reach the single runtime callback worker.
    peer.release();
    const auto result = operation.join();
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "pong");
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(callback_result, result);
    for (const auto& nested : {nested_request, nested_prepared, nested_join}) {
        ASSERT_TRUE(nested);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*nested));
        EXPECT_EQ(wire::failure(nested).error.kind, sp::ErrorKind::Misuse);
    }
    EXPECT_EQ(test::text(client.complete(payload(), run)), "pong");
    EXPECT_EQ(test::text(client.complete(client.prepare(payload(), run))), "pong");
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->entered, 3U) << "rejected callback wrappers must not issue HTTP requests";
}

TEST(SchemaProviderInterface6, ResponseDoneAliasRequiresFullTerminalResponsesEnvelope) {
    wire::Peer peer("", true);
    auto provider = wire::provider("openai.responses", peer.origin());
    for (const auto& status : {"completed", "incomplete"}) {
        auto response = json::parse(wire::responses_body());
        response["status"] = status;
        if (std::string_view(status) == "incomplete") response["incomplete_details"] = {{"reason", "max_output_tokens"}};
        body(peer, response_prefix() + response_frame("response.done", {{"response", response}}));
        const auto result = prepared_run(*provider, "openai.responses", ProviderMode::Stream);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        EXPECT_EQ(test::text(result), "pong");
        EXPECT_EQ(test::completion(result).stop.kind, std::string_view(status) == "completed"
            ? sp::StopKind::EndTurn : sp::StopKind::MaxTokens);
        ASSERT_FALSE(test::completion(result).raw_events.empty());
        EXPECT_EQ(test::completion(result).raw_events.back().type, "response.done");
    }
    std::vector<json> invalid;
    auto nonterminal = json::parse(wire::responses_body());
    nonterminal["status"] = "in_progress";
    invalid.push_back(nonterminal);
    auto missing_output = json::object();
    const auto original_response = json::parse(wire::responses_body());
    for (const auto& [key, value] : original_response.items()) {
        if (key != "output") missing_output[key] = value;
    }
    invalid.push_back(missing_output);
    // Realtime shares the event name, not the Responses API envelope contract.
    invalid.push_back({{"id", "rt-1"}, {"object", "realtime.response"}, {"status", "completed"},
                       {"output", json::array()}, {"usage", {{"total_tokens", 0}}}});
    invalid.push_back({{"id", "rt-2"}, {"object", "response"}, {"status", "completed"},
                       {"output", json::array()}, {"usage", {{"total_tokens", 0}}}});
    for (const auto& envelope : invalid) {
        SCOPED_TRACE(envelope.dump());
        body(peer, response_prefix() + response_frame("response.done", {{"response", envelope}}));
        const auto result = prepared_run(*provider, "openai.responses", ProviderMode::Stream);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
        EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ProtocolCorrupt);
    }
    for (const auto& event : {"response.content_part.delta", "response.future_event"}) {
        body(peer, response_prefix() + response_frame(event, {{"output_index", 0}, {"content_index", 0}, {"delta", "late"}}));
        const auto result = prepared_run(*provider, "openai.responses", ProviderMode::Stream);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
        EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::Unsupported);
    }
}

TEST(SchemaProviderInterface6, ChatTerminalUsageTailIsIdempotentAndNeverReopensOutput) {
    auto tail = wire::chat_frame({{"choices", json::array({{{"index", 0},
        {"delta", {{"content", ""}, {"role", "assistant"}}}, {"finish_reason", "stop"}}})},
        {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}, {"cost", 0}, {"is_byok", false}}}});
    const auto initial = wire::chat_frame({{"choices", json::array({{{"index", 0},
        {"delta", {{"content", "pong"}, {"role", "assistant"}}}, {"finish_reason", "stop"}}})}});
    wire::Peer peer(initial + tail + tail + "data: [DONE]\n\n", true);
    auto provider = llm::SchemaProvider::create(cost_descriptor("openai.chat", peer.origin()), runtime_options());
    auto request = wire::request("openai.chat", ProviderMode::Stream);
    std::string deltas;
    request.on_event = [&](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event)) deltas += delta->payload.bytes;
    };
    const auto result = async::run_sync(provider->dispatch_async(provider->prepare(std::move(request))));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "pong");
    EXPECT_EQ(deltas, "pong");
    EXPECT_EQ(outcome_usage(*result).total->value, 5U);
    EXPECT_EQ(outcome_usage(*result).provider_cost.total->nano_usd, 0U);
    EXPECT_EQ(test::completion(result).raw_events.size(), 3U);
    expect_usage_roundtrip(outcome_usage(*result));
    for (const auto& delta : {json{{"content", "late"}}, json{{"vendor", "late"}},
                              json{{"tool_calls", json::array({{{"index", 0}, {"function", {{"arguments", "{}"}}}}})}}}) {
        body(peer, initial + wire::chat_frame({{"choices", json::array({{{"index", 0},
            {"delta", delta}, {"finish_reason", "stop"}}})}, {"usage", {{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}}}})
            + "data: [DONE]\n\n");
        const auto rejected = prepared_run(*provider, "openai.chat", ProviderMode::Stream);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*rejected));
        EXPECT_EQ(wire::failure(rejected).error.kind, sp::ErrorKind::ProtocolCorrupt);
    }
}

TEST(SchemaProviderInterface6, GeminiOtherIsUnknownWhileImageSafetyAndMalformedCallStayNormalized) {
    wire::Peer peer("");
    auto provider = wire::provider("google.generate", peer.origin());
    for (const auto& raw : {"OTHER", "MALFORMED_FUNCTION_CALL", "IMAGE_SAFETY"}) {
        body(peer, json{{"candidates", json::array({{{"content", {{"role", "model"},
            {"parts", json::array({{{"text", "partial"}}})}}}, {"finishReason", raw}}})},
            {"usageMetadata", {{"promptTokenCount", 3}, {"candidatesTokenCount", 2}, {"totalTokenCount", 5}}}}.dump());
        const auto result = prepared_run(*provider, "google.generate");
        if (std::string_view(raw) == "IMAGE_SAFETY") {
            ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
            EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::RemoteFailure);
            ASSERT_TRUE(wire::failure(result).partial.stop);
            EXPECT_EQ(wire::failure(result).partial.stop->kind, sp::StopKind::ContentFilter);
            EXPECT_EQ(wire::failure(result).partial.stop->raw, raw);
        } else {
            ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
            EXPECT_EQ(test::completion(result).stop.kind, std::string_view(raw) == "OTHER"
                ? sp::StopKind::Unknown : sp::StopKind::MalformedCall);
            EXPECT_NE(test::completion(result).stop.kind, sp::StopKind::EndTurn);
            EXPECT_EQ(test::completion(result).stop.raw, raw);
        }
        ArchiveDirectory directory;
        auto archive = archive_activation(sp::NativeArchive::provision(directory.records(), directory.key(),
            "gemini-owner", test::descriptor("google.generate", peer.origin())));
        expect_archive_roundtrip(result, archive, raw);
    }
}


// Responses keeps one atomic native replay group, but its typed parts and native
// output items must preserve commentary -> function call -> final answer order.
TEST(SchemaProviderInterface6, ResponsesCommentaryToolFinalAnswerRetainIdentityPhaseAndOrder) {
    for (const auto mode : {ProviderMode::Collect, ProviderMode::Stream}) {
        SCOPED_TRACE(mode == ProviderMode::Stream ? "native SSE" : "buffered");
        const json commentary{{"id", "msg-commentary"}, {"type", "message"},
            {"role", "assistant"}, {"status", "completed"}, {"phase", "commentary"},
            {"content", json::array({{{"type", "output_text"}, {"text", "Checking the ledger."},
                {"annotations", json::array()}}})}};
        const json tool{{"id", "fc-ledger"}, {"type", "function_call"}, {"status", "completed"},
            {"call_id", "call-ledger"}, {"name", "lookup"}, {"arguments", R"({"x":7})"}};
        const json final_answer{{"id", "msg-final"}, {"type", "message"},
            {"role", "assistant"}, {"status", "completed"}, {"phase", "final_answer"},
            {"content", json::array({{{"type", "output_text"}, {"text", "The ledger value is seven."},
                {"annotations", json::array()}}})}};
        auto response = json::parse(wire::responses_body());
        response["id"] = "response-ordered";
        response["output"] = json::array({commentary, tool, final_answer});
        std::string stream;
        if (mode == ProviderMode::Stream) {
            auto initial = response;
            initial["status"] = "in_progress";
            initial["output"] = json::array();
            initial["usage"] = nullptr;
            stream = response_frame("response.created", {{"response", initial}});
            const auto append_text = [&](unsigned index, const json& item) {
                auto added = item;
                added["status"] = "in_progress";
                added["content"] = json::array();
                const auto id = item.at("id");
                const auto part = item.at("content").at(0);
                auto empty_part = part;
                empty_part["text"] = "";
                stream += response_frame("response.output_item.added", {{"output_index", index}, {"item", added}});
                stream += response_frame("response.content_part.added", {{"output_index", index},
                    {"item_id", id}, {"content_index", 0}, {"part", empty_part}});
                stream += response_frame("response.output_text.delta", {{"output_index", index},
                    {"item_id", id}, {"content_index", 0}, {"delta", part.at("text")}});
                stream += response_frame("response.output_text.done", {{"output_index", index},
                    {"item_id", id}, {"content_index", 0}, {"text", part.at("text")}});
                stream += response_frame("response.content_part.done", {{"output_index", index},
                    {"item_id", id}, {"content_index", 0}, {"part", part}});
                stream += response_frame("response.output_item.done", {{"output_index", index}, {"item", item}});
            };
            append_text(0, commentary);
            auto added_tool = tool;
            added_tool["status"] = "in_progress";
            added_tool["arguments"] = "";
            stream += response_frame("response.output_item.added", {{"output_index", 1}, {"item", added_tool}});
            stream += response_frame("response.function_call_arguments.delta", {{"output_index", 1},
                {"item_id", "fc-ledger"}, {"delta", R"({"x":)"}});
            stream += response_frame("response.function_call_arguments.delta", {{"output_index", 1},
                {"item_id", "fc-ledger"}, {"delta", "7}"}});
            stream += response_frame("response.function_call_arguments.done", {{"output_index", 1},
                {"item_id", "fc-ledger"}, {"arguments", R"({"x":7})"}});
            stream += response_frame("response.output_item.done", {{"output_index", 1}, {"item", tool}});
            append_text(2, final_answer);
            stream += response_frame("response.completed", {{"response", response}});
        }
        wire::Peer peer(mode == ProviderMode::Stream ? stream : response.dump(), mode == ProviderMode::Stream);
        auto descriptor = test::descriptor("openai.responses", peer.origin());
        auto provider = llm::SchemaProvider::create(descriptor, runtime_options());
        auto request = wire::request("openai.responses", mode);
        auto& payload = std::get<sp::responses::Request>(request.payload);
        payload.tools.push_back({"lookup", "Read a ledger entry", test::document(
            R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})"), true});
        auto prepared = provider->prepare(request);
        ASSERT_TRUE(prepared.valid());
        ASSERT_EQ(prepared.error(), nullptr);
        const auto result = async::run_sync(provider->dispatch_async(std::move(prepared)));
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto assert_ordered = [](const sp::runtime::Result& outcome) {
            const auto& completion = test::completion(outcome);
            EXPECT_EQ(completion.stop.kind, sp::StopKind::ToolUse);
            EXPECT_EQ(completion.stop.raw, "completed");
            ASSERT_EQ(completion.messages.size(), 1U);
            const auto& message = completion.messages.front();
            EXPECT_EQ(message.id, "response-ordered");
            EXPECT_EQ(message.role, sp::Role::Assistant);
            ASSERT_EQ(message.parts.size(), 3U);
            ASSERT_TRUE(std::holds_alternative<sp::Text>(message.parts[0]));
            EXPECT_EQ(std::get<sp::Text>(message.parts[0]).value, "Checking the ledger.");
            ASSERT_TRUE(std::holds_alternative<sp::ToolCall>(message.parts[1]));
            const auto& call = std::get<sp::ToolCall>(message.parts[1]);
            EXPECT_EQ(call.id, "call-ledger");
            EXPECT_EQ(call.name, "lookup");
            EXPECT_EQ(call.kind, sp::ToolCallKind::ClientExecuted);
            ASSERT_TRUE(call.input);
            EXPECT_EQ(call.input->root().get("x").as_uint(), 7U);
            ASSERT_TRUE(call.wire_metadata);
            EXPECT_EQ(call.wire_metadata->root().get("id").as_string(), "fc-ledger");
            ASSERT_TRUE(std::holds_alternative<sp::Text>(message.parts[2]));
            EXPECT_EQ(std::get<sp::Text>(message.parts[2]).value, "The ledger value is seven.");
            ASSERT_TRUE(message.native);
            EXPECT_TRUE(message.native->complete());
            ASSERT_TRUE(message.wire_output);
            const auto output = message.wire_output->root();
            ASSERT_EQ(output.size(), 3U);
            EXPECT_EQ(output.at(0).get("type").as_string(), "message");
            EXPECT_EQ(output.at(0).get("id").as_string(), "msg-commentary");
            EXPECT_EQ(output.at(0).get("phase").as_string(), "commentary");
            EXPECT_EQ(output.at(1).get("type").as_string(), "function_call");
            EXPECT_EQ(output.at(1).get("id").as_string(), "fc-ledger");
            EXPECT_EQ(output.at(1).get("call_id").as_string(), "call-ledger");
            EXPECT_EQ(output.at(2).get("type").as_string(), "message");
            EXPECT_EQ(output.at(2).get("id").as_string(), "msg-final");
            EXPECT_EQ(output.at(2).get("phase").as_string(), "final_answer");
        };
        assert_ordered(result);
        const auto observed = provider_codec::observe_outcome(*result);
        const auto& parts = observed.at("messages").at(0).at("parts");
        ASSERT_EQ(parts.size(), 3U);
        EXPECT_EQ(parts.at(0).at("type"), "text");
        EXPECT_EQ(parts.at(0).at("value"), "Checking the ledger.");
        EXPECT_EQ(parts.at(1).at("type"), "tool_call");
        EXPECT_EQ(parts.at(1).at("id"), "call-ledger");
        EXPECT_EQ(parts.at(2).at("type"), "text");
        EXPECT_EQ(parts.at(2).at("value"), "The ledger value is seven.");
        ArchiveDirectory directory;
        auto archive = archive_activation(sp::NativeArchive::provision(
            directory.records(), directory.key(), "responses-order-owner", descriptor));
        const auto encoded = provider_codec::encode_outcome(*result, archive, "ordered-output");
        const auto restored = provider_codec::decode_outcome(json::parse(encoded.dump()), archive, "ordered-output");
        EXPECT_EQ(provider_codec::observe_outcome(*restored), observed);
        EXPECT_THROW(provider_codec::decode_outcome(encoded, archive, "wrong-binding"), std::runtime_error);
        assert_ordered(restored);

        // Restore authentic runtime-issued custody, then exercise real request
        // encoding and dispatch. Compare semantic fields, not a JSON text echo.
        request.mode = ProviderMode::Collect;
        payload.messages.push_back(test::completion(restored).messages.front());
        sp::Message tool_result;
        tool_result.role = sp::Role::Tool;
        tool_result.parts.emplace_back(sp::ToolResult{"call-ledger", "seven"});
        payload.messages.push_back(std::move(tool_result));
        { std::lock_guard lock(peer.state->mutex);
          peer.state->body = wire::responses_body("Replay accepted.");
          peer.state->content_type = "application/json"; }
        auto replay_prepared = provider->prepare(std::move(request));
        ASSERT_TRUE(replay_prepared.valid());
        ASSERT_EQ(replay_prepared.error(), nullptr);
        const auto replay = async::run_sync(provider->dispatch_async(std::move(replay_prepared)));
        ASSERT_TRUE(replay);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*replay));
        EXPECT_EQ(test::text(replay), "Replay accepted.");
        std::lock_guard lock(peer.state->mutex);
        ASSERT_EQ(peer.state->requests.size(), 2U);
        const auto& input = peer.state->requests.back().at("input");
        ASSERT_EQ(input.size(), 5U);
        EXPECT_EQ(input.at(1).at("id"), "msg-commentary");
        EXPECT_EQ(input.at(1).at("phase"), "commentary");
        EXPECT_EQ(input.at(1).at("content").at(0).at("text"), "Checking the ledger.");
        EXPECT_EQ(input.at(2).at("type"), "function_call");
        EXPECT_EQ(input.at(2).at("id"), "fc-ledger");
        EXPECT_EQ(input.at(2).at("call_id"), "call-ledger");
        EXPECT_EQ(input.at(2).at("name"), "lookup");
        EXPECT_EQ(json::parse(input.at(2).at("arguments").get<std::string>()).at("x"), 7);
        EXPECT_EQ(input.at(3).at("id"), "msg-final");
        EXPECT_EQ(input.at(3).at("phase"), "final_answer");
        EXPECT_EQ(input.at(3).at("content").at(0).at("text"), "The ledger value is seven.");
        EXPECT_EQ(input.at(4).at("type"), "function_call_output");
        EXPECT_EQ(input.at(4).at("call_id"), "call-ledger");
        EXPECT_EQ(input.at(4).at("output"), "seven");
    }
}

TEST(SchemaProviderInterface6, ResponsesRefusalRemainsTypedThroughObservationArchiveAndReplay) {
    for (const auto mode : {ProviderMode::Collect, ProviderMode::Stream}) {
        SCOPED_TRACE(mode == ProviderMode::Stream ? "native SSE" : "buffered");
        const json refusal_part{{"type", "refusal"}, {"refusal", "I cannot provide that instruction."}};
        const json item{{"id", "msg-refusal"}, {"type", "message"}, {"role", "assistant"},
            {"status", "completed"}, {"phase", "final_answer"}, {"content", json::array({refusal_part})}};
        auto response = json::parse(wire::responses_body());
        response["id"] = "response-refusal";
        response["output"] = json::array({item});
        std::string stream;
        if (mode == ProviderMode::Stream) {
            auto initial = response;
            initial["status"] = "in_progress";
            initial["output"] = json::array();
            initial["usage"] = nullptr;
            auto added = item;
            added["status"] = "in_progress";
            added["content"] = json::array();
            stream = response_frame("response.created", {{"response", initial}})
                + response_frame("response.output_item.added", {{"output_index", 0}, {"item", added}})
                + response_frame("response.content_part.added", {{"output_index", 0}, {"item_id", "msg-refusal"},
                    {"content_index", 0}, {"part", {{"type", "refusal"}, {"refusal", ""}}}})
                + response_frame("response.refusal.delta", {{"output_index", 0}, {"item_id", "msg-refusal"},
                    {"content_index", 0}, {"delta", "I cannot "}})
                + response_frame("response.refusal.delta", {{"output_index", 0}, {"item_id", "msg-refusal"},
                    {"content_index", 0}, {"delta", "provide that instruction."}})
                + response_frame("response.refusal.done", {{"output_index", 0}, {"item_id", "msg-refusal"},
                    {"content_index", 0}, {"refusal", "I cannot provide that instruction."}})
                + response_frame("response.content_part.done", {{"output_index", 0}, {"item_id", "msg-refusal"},
                    {"content_index", 0}, {"part", refusal_part}})
                + response_frame("response.output_item.done", {{"output_index", 0}, {"item", item}})
                + response_frame("response.completed", {{"response", response}});
        }
        wire::Peer peer(mode == ProviderMode::Stream ? stream : response.dump(), mode == ProviderMode::Stream);
        auto descriptor = test::descriptor("openai.responses", peer.origin());
        auto provider = llm::SchemaProvider::create(descriptor, runtime_options());
        auto request = wire::request("openai.responses", mode);
        std::string refusal_deltas;
        std::vector<std::string> refusal_ids;
        request.on_event = [&](const sp::Event& event) {
            if (const auto* begin = std::get_if<sp::PartBegin>(&event); begin && begin->kind == sp::PartKind::Refusal)
                refusal_ids.push_back(begin->header.wire_id);
            if (const auto* delta = std::get_if<sp::PartDelta>(&event); delta && delta->payload.kind == sp::PartKind::Refusal)
                refusal_deltas += delta->payload.bytes;
        };
        auto prepared = provider->prepare(request);
        ASSERT_TRUE(prepared.valid());
        ASSERT_EQ(prepared.error(), nullptr);
        const auto result = async::run_sync(provider->dispatch_async(std::move(prepared)));
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto assert_refusal = [](const sp::runtime::Result& outcome) {
            const auto& completion = test::completion(outcome);
            EXPECT_EQ(completion.stop.kind, sp::StopKind::Refusal);
            EXPECT_EQ(completion.stop.raw, "completed");
            ASSERT_EQ(completion.messages.size(), 1U);
            const auto& message = completion.messages.front();
            EXPECT_EQ(message.id, "response-refusal");
            EXPECT_EQ(message.role, sp::Role::Assistant);
            ASSERT_EQ(message.parts.size(), 1U);
            ASSERT_TRUE(std::holds_alternative<sp::Refusal>(message.parts.front()));
            EXPECT_EQ(std::get<sp::Refusal>(message.parts.front()).text, "I cannot provide that instruction.");
            ASSERT_TRUE(message.native);
            EXPECT_TRUE(message.native->complete());
            ASSERT_TRUE(message.wire_output);
            const auto output = message.wire_output->root();
            ASSERT_EQ(output.size(), 1U);
            EXPECT_EQ(output.at(0).get("id").as_string(), "msg-refusal");
            EXPECT_EQ(output.at(0).get("phase").as_string(), "final_answer");
            const auto content = output.at(0).get("content");
            ASSERT_EQ(content.size(), 1U);
            EXPECT_EQ(content.at(0).get("type").as_string(), "refusal");
            EXPECT_EQ(content.at(0).get("refusal").as_string(), "I cannot provide that instruction.");
        };
        assert_refusal(result);
        if (mode == ProviderMode::Stream) {
            EXPECT_EQ(refusal_deltas, "I cannot provide that instruction.");
            EXPECT_EQ(refusal_ids, (std::vector<std::string>{"msg-refusal"}));
        }
        const auto observed = provider_codec::observe_outcome(*result);
        const auto& observed_parts = observed.at("messages").at(0).at("parts");
        ASSERT_EQ(observed_parts.size(), 1U);
        EXPECT_EQ(observed_parts.at(0).at("type"), "refusal");
        EXPECT_EQ(observed_parts.at(0).at("text"), "I cannot provide that instruction.");
        EXPECT_EQ(observed.at("stop").at("kind"), static_cast<int>(sp::StopKind::Refusal));
        ArchiveDirectory directory;
        auto archive = archive_activation(sp::NativeArchive::provision(
            directory.records(), directory.key(), "responses-refusal-owner", descriptor));
        const auto encoded = provider_codec::encode_outcome(*result, archive, "refusal-output");
        const auto restored = provider_codec::decode_outcome(json::parse(encoded.dump()), archive, "refusal-output");
        EXPECT_EQ(provider_codec::observe_outcome(*restored), observed);
        EXPECT_THROW(provider_codec::decode_outcome(encoded, archive, "wrong-binding"), std::runtime_error);
        assert_refusal(restored);

        request.mode = ProviderMode::Collect;
        request.on_event = {};
        auto& payload = std::get<sp::responses::Request>(request.payload);
        payload.messages.push_back(test::completion(restored).messages.front());
        payload.messages.push_back(test::message("Explain the refusal.", sp::Role::User));
        { std::lock_guard lock(peer.state->mutex);
          peer.state->body = wire::responses_body("Replay accepted.");
          peer.state->content_type = "application/json"; }
        auto replay_prepared = provider->prepare(std::move(request));
        ASSERT_TRUE(replay_prepared.valid());
        ASSERT_EQ(replay_prepared.error(), nullptr);
        const auto replay = async::run_sync(provider->dispatch_async(std::move(replay_prepared)));
        ASSERT_TRUE(replay);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*replay));
        EXPECT_EQ(test::text(replay), "Replay accepted.");
        std::lock_guard lock(peer.state->mutex);
        ASSERT_EQ(peer.state->requests.size(), 2U);
        const auto& input = peer.state->requests.back().at("input");
        ASSERT_EQ(input.size(), 3U);
        EXPECT_EQ(input.at(1).at("id"), "msg-refusal");
        EXPECT_EQ(input.at(1).at("phase"), "final_answer");
        ASSERT_EQ(input.at(1).at("content").size(), 1U);
        EXPECT_EQ(input.at(1).at("content").at(0).at("type"), "refusal");
        EXPECT_EQ(input.at(1).at("content").at(0).at("refusal"), "I cannot provide that instruction.");
        EXPECT_EQ(input.at(2).at("role"), "user");
    }
}
