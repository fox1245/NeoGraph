#pragma once

#include "../../../provider_example_support.h"
#include <neograph/async/endpoint.h>
#include <neograph/graph/run_context.h>
#include <sp/config_defaults.h>

#include <iostream>

namespace jarvis::providers {

constexpr const char* openrouter_origin = "https://openrouter.ai";

// The SDK admits routing controls (ZDR) only on a declared OpenRouter origin.
// A TLS gateway that forwards to OpenRouter (the proxy benchmark) is not one by
// default, so the caller's own base URL is declared one explicitly. Credential
// transport is unaffected: live() still requires TLS for every key-bearing URL.
inline sp::descriptor::PolicySnapshot policy_declaring_openrouter_origin(const std::string& origin) {
    auto policy = neograph::json::parse(sp::config_defaults::descriptor_policy_json);
    for (auto family : policy.at("families"))
        if (family.at("family") == "openai.chat") family.at("openrouter_origins").push_back(origin);
    auto loaded = sp::descriptor::load_policy(policy.dump(), sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument("gateway origin rejected at " + error->pointer + ": " + error->message);
    return std::get<sp::descriptor::PolicySnapshot>(std::move(loaded));
}

// `ca_file` selects a PEM CA bundle for a private TLS gateway. Leave it empty
// to use libcurl's default trust configuration for openrouter.ai.
inline std::shared_ptr<neograph::Provider> live(std::string api_key,
                                              const std::string& base_url = "https://openrouter.ai/api",
                                              std::string ca_file = {}) {
    const auto endpoint = neograph::async::validate_credential_endpoint(base_url, true, false);
    const auto host = endpoint.host.find(':') == std::string::npos
        ? endpoint.host : "[" + endpoint.host + "]";
    const bool default_port = endpoint.port == (endpoint.tls ? "443" : "80");
    const auto origin = std::string(endpoint.tls ? "https://" : "http://") + host +
        (default_port ? std::string{} : ":" + endpoint.port);
    auto prefix = endpoint.prefix;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    const auto path = prefix + (prefix.ends_with("/v1") ? "/chat/completions" : "/v1/chat/completions");
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    options.ca_file = std::move(ca_file);
    neograph::llm::SchemaProvider::Defaults defaults;
    defaults.provider.emplace();
    defaults.provider->zdr = true;
    auto policy = origin == openrouter_origin ? sp::descriptor::PolicySnapshot{}
                                              : policy_declaring_openrouter_origin(origin);
    return neograph::llm::SchemaProvider::create(examples::admitted_descriptor(
        origin, "openai.chat", path, "jarvis-openrouter-chat", std::move(policy)),
        std::move(options), std::move(defaults));
}

enum class Fixture { Jarvis, Coder, Researcher };

class MockProvider final : public neograph::Provider {
public:
    explicit MockProvider(Fixture fixture = Fixture::Jarvis)
        : fixture_(fixture), client_(examples::make_local_client()) {}

    std::string get_name() const override {
        return fixture_ == Fixture::Jarvis ? "jarvis_mock" : "mock";
    }
    std::string_view family() const noexcept override { return "openai.chat"; }

    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        bool router = false;
        std::string user;
        bool saw_user = false;
        for (const auto& message : examples::request_messages(request)) {
            std::string text;
            for (const auto& part : message.parts)
                if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
            if (message.role == sp::Role::System &&
                text.find("IMPORTANT: respond with a single JSON") != std::string::npos)
                router = true;
            if (message.role == sp::Role::User &&
                (fixture_ != Fixture::Coder || !saw_user)) {
                user = std::move(text);
                saw_user = true;
            }
        }
        std::string text;
        if (fixture_ == Fixture::Researcher) {
            text = "이건 mock 응답입니다. 실제 연구 결과가 여기에 들어갑니다.\n\n"
                   "[SUMMARY] mock researcher reply — no real data available";
        } else if (fixture_ == Fixture::Coder) {
            text = "[mock coder] " + user + "\n[SUMMARY] mock coder reply";
        } else if (router) {
            text = R"({"mode":"direct","tool_calls":[],"delegate_to":null,"skip_synthesis":true,"reasoning_short":"mock router"})";
        } else {
            text = "[mock] " + user;
        }
        sp::Completion completion;
        completion.messages.push_back(examples::message(sp::Role::Assistant, std::move(text)));
        completion.stop = {sp::StopKind::EndTurn, "local-fixture"};
        auto result = std::make_shared<const sp::Outcome>(std::move(completion));
        return prepare_local(client_, std::move(request),
            [result](const neograph::PreparedProviderRequest&,
                     const std::function<void(const sp::Event&)>& on_event)
                -> asio::awaitable<sp::runtime::Result> {
                examples::emit_local_events(std::get<sp::Completion>(*result), on_event);
                co_return result;
            });
    }
private:
    Fixture fixture_;
    std::shared_ptr<sp::runtime::Client> client_;
};

// Reasoning models may count hidden reasoning against the same output cap as
// visible text. Reserve headroom above the nominal spoken-reply budget and
// request low effort; this is not a guarantee that any particular model fits.
inline constexpr std::uint64_t reasoning_allowance = 1024;

// Only the default cap doubles. An explicit caller cap remains identical on
// every semantic call, including the separately admitted empty-output re-ask.
inline std::uint64_t output_budget(std::uint64_t visible_tokens, unsigned attempt = 0) {
    if (std::getenv("NG_EXAMPLE_MAX_TOKENS")) return examples::output_cap(0);
    return (visible_tokens + reasoning_allowance) << attempt;
}

inline neograph::ProviderRequest request(const neograph::Provider& provider,
                                         std::vector<sp::Message> messages,
                                         double temperature,
                                         std::optional<std::uint64_t> output_cap = {},
                                         neograph::ProviderMode mode = neograph::ProviderMode::Collect,
                                         std::string model = examples::openrouter_model) {
    neograph::ProviderControls controls;
    controls.temperature = temperature;
    controls.max_output_tokens = output_cap;
    controls.reasoning_effort = "low";
    return neograph::make_provider_request(provider, std::move(model),
                                           std::move(messages), {}, controls, mode);
}

inline bool has_visible_text(const sp::Outcome& outcome) {
    const auto* completion = std::get_if<sp::Completion>(&outcome);
    if (!completion) return false;
    for (const auto& message : completion->messages)
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<sp::Text>(&part))
                for (const char c : text->value)
                    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return true;
    return false;
}

inline bool has_tool_call_output(const sp::Outcome& outcome) {
    const auto* completion = std::get_if<sp::Completion>(&outcome);
    if (!completion) return false;
    for (const auto& message : completion->messages)
        for (const auto& part : message.parts)
            if (std::holds_alternative<sp::ToolCall>(part) ||
                std::holds_alternative<sp::InvalidToolCall>(part)) return true;
    return false;
}

inline bool token_limit_without_text(const sp::Outcome& outcome) {
    const auto* completion = std::get_if<sp::Completion>(&outcome);
    return completion && completion->stop.kind == sp::StopKind::MaxTokens &&
           !has_visible_text(outcome) && !has_tool_call_output(outcome);
}

// Only a completed empty MaxTokens reply without tool-call output is eligible
// for at most one re-ask. Each attempt has its own node-owned broker ordinal.
// Provider failures and all other empty replies remain errors, never fallback
// success. Both attempts' reported usage is retained.
template <class MakeRequest, class Invoke>
asio::awaitable<sp::runtime::Result> complete_with_visible_text(
    const neograph::graph::RunContext& ctx, MakeRequest make_request, Invoke invoke) {
    const auto reasks = examples::empty_reasks(1, 1);
    for (unsigned attempt = 0;; ++attempt) {
        auto reply = co_await neograph::graph::observe_provider_result(
            ctx, invoke(make_request(attempt), attempt));
        neograph::graph::record_usage(ctx, reply);
        reply = neograph::outcome_or_throw(std::move(reply));
        if (has_visible_text(*reply)) co_return reply;
        if (attempt == reasks || !token_limit_without_text(*reply)) {
            const std::string reason = token_limit_without_text(*reply)
                ? "output-token limit reached without visible text; configured re-asks exhausted"
                : "provider completed without visible text";
            throw neograph::ProviderOutcomeError(reason.c_str(), reply,
                std::make_exception_ptr(std::runtime_error(reason)));
        }
        std::cerr << "[jarvis] empty MaxTokens completion; one additional semantic call "
                     "(explicit output cap unchanged, default cap doubles)\n";
    }
}

// Structured routing must not convert provider truncation or client-call output
// into a normal chat decision via the malformed-JSON fallback.
inline void require_completed_router_output(const sp::runtime::Result& reply) {
    const auto* completion = reply ? std::get_if<sp::Completion>(reply.get()) : nullptr;
    if (completion &&
        (completion->stop.kind == sp::StopKind::EndTurn ||
         completion->stop.kind == sp::StopKind::StopSequence) &&
        !has_tool_call_output(*reply)) return;
    const std::string reason = "router did not produce a completed text-only decision";
    throw neograph::ProviderOutcomeError(reason.c_str(), reply,
        std::make_exception_ptr(std::runtime_error(reason)));
}

// Preserve partial text in its Outcome, but never publish a truncated/refused
// synthesis as a completed spoken turn or dispatch another call for it.
inline void require_completed_spoken_output(const sp::runtime::Result& reply) {
    const auto* completion = reply ? std::get_if<sp::Completion>(reply.get()) : nullptr;
    if (completion &&
        (completion->stop.kind == sp::StopKind::EndTurn ||
         completion->stop.kind == sp::StopKind::StopSequence) &&
        has_visible_text(*reply) && !has_tool_call_output(*reply)) return;
    throw neograph::ProviderOutcomeError(
        "synthesizer did not produce a complete text-only spoken reply", reply, {});
}

inline neograph::ProviderRequest contextual_request(
    neograph::ProviderRequest request, const neograph::graph::RunContext& ctx) {
    request.cancel_token = ctx.cancel_token;
    if (ctx.deadline) request.options.deadline = ctx.deadline;
    if (ctx.on_provider_event) {
        auto local_observer = std::move(request.on_event);
        request.on_event = [local_observer = std::move(local_observer),
                            observer = ctx.on_provider_event](const sp::Event& event) {
            if (local_observer) local_observer(event);
            observer(event);
        };
    }
    return request;
}


} // namespace jarvis::providers
