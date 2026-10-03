// NeoGraph Example 13: actual typed OpenRouter Responses SSE stream.
// Usage: ./example_openrouter_responses_sse (OPENROUTER_API_KEY in env/.env)
// Typed semantic events drive the live text display. Complete wire events,
// including unknown types, and the immutable terminal Outcome remain owned.

#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

int main() {
    cppdotenv::auto_load_dotenv();
    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY (env or .env)\n";
            return 1;
        }
        const std::string prompt =
            "In two short sentences, explain why streaming LLM responses "
            "improves perceived latency.";
        auto provider = examples::make_openrouter_provider(
            api_key, "responses", std::chrono::seconds(180));
        sp::responses::Request payload;
        payload.model = examples::openrouter_model;
        payload.messages.push_back(examples::message(sp::Role::User, prompt));
        payload.temperature = 0.0;
        payload.max_output_tokens = 512;
        neograph::ProviderRequest request;
        request.payload = std::move(payload);
        request.mode = neograph::ProviderMode::Stream;
        request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        std::vector<sp::RawWire> wire_events;
        std::size_t semantic_events = 0;
        request.on_event = [&](const sp::Event& event) {
            if (const auto* wire = std::get_if<sp::RawWire>(&event)) {
                // RawWire owns its document. Never retain PartDelta's borrowed
                // string_view beyond the callback, or reconstruct an Outcome.
                wire_events.push_back(*wire);
            } else {
                ++semantic_events;
                if (const auto* delta = std::get_if<sp::PartDelta>(&event);
                    delta && delta->payload.kind == sp::PartKind::Text &&
                    delta->payload.channel == sp::DeltaChannel::Content)
                    std::cout << delta->payload.bytes << std::flush;
            }
        };
        std::cout << "User: " << prompt << "\nAssistant: " << std::flush;
        const std::shared_ptr<const sp::Outcome> outcome = provider->invoke(std::move(request));
        if (!outcome) throw std::runtime_error("Provider returned no owned Outcome");
        std::cout << "\n\nTyped semantic events: " << semantic_events << "\n"
                  << "--- Complete ordered wire events (including unknown types) ---\n";
        for (const auto& wire : wire_events)
            std::cout << wire.type << "\n"
                      << (wire.payload ? wire.payload->root().dump() : "null") << "\n";
        std::cout << "--- Full terminal Outcome ---\n"
                  << neograph::outcome_projection_json(*outcome).dump(2) << "\n";
        if (const auto* failure = std::get_if<sp::Failure>(outcome.get())) {
            std::cerr << "Error: " << failure->error.safe_message << "\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "\nError: " << error.what() << "\n";
        return 1;
    }
}
