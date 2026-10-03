// NeoGraph Example 30: typed Responses reasoning-effort tradeoff sweep.
// Usage: ./example_reasoning_effort ["your custom prompt"]
// Provider reports are evidence, not a promise of monotonic latency or accuracy.
// Every trial retains its entire immutable Outcome, including failure partials.

#include "provider_example_support.h"

#include <cppdotenv/dotenv.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
struct Trial {
    std::string effort;
    std::chrono::milliseconds wall{0};
    std::shared_ptr<const sp::Outcome> outcome;
};

Trial run_one(neograph::Provider& provider, const std::string& model,
              const std::string& question, const std::string& effort) {
    sp::responses::Request payload;
    payload.model = model;
    payload.messages.push_back(examples::message(sp::Role::User, question));
    payload.reasoning = sp::responses::ReasoningOptions{effort, std::nullopt};
    neograph::ProviderRequest request;
    request.payload = std::move(payload);
    request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const auto start = std::chrono::steady_clock::now();
    auto outcome = provider.invoke(std::move(request));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!outcome) throw std::runtime_error("Provider returned no owned Outcome");
    return {effort, elapsed, std::move(outcome)};
}

void print_count(const char* name, const std::optional<sp::Count>& count) {
    std::cout << "  " << name << " : ";
    if (!count) {
        std::cout << "unknown (missing; not zero)\n";
        return;
    }
    std::cout << count->value << " ("
              << (count->evidence == sp::Evidence::Reported ? "reported" : "derived")
              << ")\n";
}

void print_usage(const sp::Usage& usage) {
    print_count("reasoning_tok", usage.reasoning);
    print_count("output_tok   ", usage.output_total);
    print_count("input_tok    ", usage.input_total);
    print_count("total_tok    ", usage.total);
    print_count("provider_total", usage.provider_reported_total);
    const char* stage = usage.stage == sp::UsageStage::Final ? "final"
                      : usage.stage == sp::UsageStage::Partial ? "partial" : "missing";
    std::cout << "  usage stage  : " << stage << "\n"
              << "  usage quality: "
              << (usage.quality == sp::UsageQuality::Consistent ? "consistent" : "inconsistent")
              << "\n";
    for (const auto& conflict : usage.conflicts)
        std::cout << "  conflict     : " << conflict.counter << " " << conflict.detail << "\n";
    for (const auto& [name, count] : usage.extra) print_count(name.c_str(), count);
}
} // namespace

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();
    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY (env or .env)\n";
            return 1;
        }
        const std::string model = "~deepseek/deepseek-v4-flash-latest";
        const std::string question = argc >= 2 ? argv[1]
            : "A snail is at the bottom of a 30-foot well. Each day it "
              "climbs 3 feet. Each night, while it sleeps, it slides "
              "down 2 feet. On what day does the snail first reach the "
              "top of the well? Answer with just an integer day number "
              "and a one-sentence justification.";
        auto provider = examples::make_openrouter_provider(api_key, "responses");
        std::cout << "\nmodel    : " << model << "\nquestion : " << question << "\n";
        std::vector<Trial> trials;
        trials.reserve(4);
        int failed = 0;
        for (const std::string effort : {"none", "low", "medium", "high"}) {
            std::cout << "\n-- effort=" << effort << " --\n";
            try {
                trials.push_back(run_one(*provider, model, question, effort));
                const auto& trial = trials.back();
                std::cout << "  wall         : " << trial.wall.count() << " ms\n";
                if (const auto* failure = std::get_if<sp::Failure>(trial.outcome.get())) {
                    ++failed;
                    std::cout << "  ERROR        : " << failure->error.safe_message
                              << " (HTTP " << failure->error.http_status << ")\n";
                    print_usage(failure->partial.usage);
                } else {
                    print_usage(std::get<sp::Completion>(*trial.outcome).usage);
                }
                // Display projection only; the full ordered parts/native seals remain
                // owned by trial.outcome rather than replaced by the visible answer.
                std::cout << "  answer       : " << examples::visible_text(*trial.outcome) << "\n";
                std::cout << "  full outcome : "
                          << neograph::outcome_projection_json(*trial.outcome).dump(2) << "\n";
            } catch (const std::exception& error) {
                ++failed;
                std::cout << "  EXCEPTION    : " << error.what() << "\n";
            }
        }
        return failed == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "\nError: " << error.what() << "\n";
        return 1;
    }
}
