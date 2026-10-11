// NeoGraph Example 29: full typed Responses envelope and ordered output dump.
// Usage: ./example_responses_envelope ["What's the weather in Tokyo?"]
// SchemaProvider owns HTTP admission/decoding. JSON below is only the function's
// declared parameter schema, never an untyped request-body override.

#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY (env or .env)\n";
            return 1;
        }
        const std::string model = examples::openrouter_model;
        const std::string question = argc >= 2 ? argv[1]
            : "What is the weather right now in Tokyo? Use the get_weather "
              "tool if you need a current value.";
        auto parameters = sp::json::parse(R"({
            "type":"object",
            "properties":{
                "city":{"type":"string","description":"City name"},
                "unit":{"type":"string","enum":["C","F"]}
            },
            "required":["city"]
        })");
        if (const auto* error = std::get_if<sp::json::ParseError>(&parameters))
            throw std::invalid_argument(error->message);
        sp::responses::ToolDefinition weather;
        weather.name = "get_weather";
        weather.description = "Look up the current weather for a city.";
        weather.parameters = std::make_shared<const sp::json::Document>(
            std::get<sp::json::Document>(std::move(parameters)));
        sp::responses::Request payload;
        payload.model = model;
        if (std::getenv("NG_EXAMPLE_MAX_TOKENS"))
            payload.max_output_tokens = examples::output_cap(0);
        payload.messages.push_back(examples::message(sp::Role::User, question));
        payload.tools.push_back(std::move(weather));
        neograph::ProviderRequest request;
        request.payload = std::move(payload);
        request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        auto provider = examples::make_openrouter_provider(api_key, "responses", std::chrono::seconds(60));
        // Retain failures as well: their partial parts and native envelopes are
        // just as important to inspection as successful completions.
        const std::shared_ptr<const sp::Outcome> outcome = provider->invoke(std::move(request));
        if (!outcome) throw std::runtime_error("Provider returned no owned Outcome");
        std::cout << "=== model    : " << model << "\n"
                  << "=== question : " << question << "\n\n";
        std::visit([](const auto& terminal) {
            const auto& complete = [&]() -> const auto& {
                if constexpr (std::is_same_v<std::decay_t<decltype(terminal)>, sp::Failure>)
                    return terminal.partial;
                else return terminal;
            }();
            std::cout << "--- Full response envelope ---\n";
            if (complete.wire_envelope)
                std::cout << complete.wire_envelope->root().dump() << "\n";
            else
                std::cout << "(no response envelope received)\n";
            std::cout << "--- Ordered native output items and typed parts ---\n";
            std::size_t message_index = 0;
            for (const auto& message : complete.messages) {
                std::cout << "message[" << message_index++ << "] id=" << message.id
                          << " native_seal=" << static_cast<bool>(message.native) << "\n";
                if (message.wire_output) {
                    std::size_t item_index = 0;
                    for (const auto item : message.wire_output->root().elements()) {
                        std::cout << "  output[" << item_index++ << "] "
                                  << item.get("type").as_string() << "\n"
                                  << "    " << item.dump() << "\n";
                    }
                }
                // Includes every ordered typed Part, function arguments,
                // citations, reasoning, opaque hosted outputs and artifacts.
                std::cout << neograph::message_projection_json(message).dump(2) << "\n";
            }
        }, *outcome);
        std::cout << "--- Full terminal Outcome ---\n"
                  << neograph::outcome_projection_json(*outcome).dump(2) << "\n";
        if (const auto* failure = std::get_if<sp::Failure>(outcome.get())) {
            std::cerr << "Error: " << failure->error.safe_message
                      << " (HTTP " << failure->error.http_status << ")\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "\nError: " << error.what() << "\n";
        return 1;
    }
}
