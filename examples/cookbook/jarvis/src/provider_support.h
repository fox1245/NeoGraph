#pragma once

#include "../../../provider_example_support.h"
#include <neograph/async/endpoint.h>
#include <neograph/graph/run_context.h>

namespace jarvis::providers {

inline std::shared_ptr<neograph::Provider> live(std::string api_key,
                                              const std::string& base_url = "https://openrouter.ai/api") {
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
    neograph::llm::SchemaProvider::Defaults defaults;
    defaults.provider.emplace();
    defaults.provider->zdr = true;
    return neograph::llm::SchemaProvider::create(examples::admitted_descriptor(
        origin, "openai.chat", path, "jarvis-openrouter-chat"), std::move(options), std::move(defaults));
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

inline neograph::ProviderRequest request(const neograph::Provider& provider,
                                         std::vector<sp::Message> messages,
                                         double temperature,
                                         std::optional<std::uint64_t> output_cap = {},
                                         neograph::ProviderMode mode = neograph::ProviderMode::Collect,
                                         std::string model = examples::openrouter_model) {
    neograph::ProviderControls controls;
    controls.temperature = temperature;
    controls.max_output_tokens = output_cap;
    return neograph::make_provider_request(provider, std::move(model),
                                           std::move(messages), {}, controls, mode);
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
