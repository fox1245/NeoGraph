// NeoGraph Example 30: reasoning_effort tradeoff sweep on /v1/responses
//
// Reasoning-capable models exposed through the OpenRouter Responses API
// may accept a top-level "reasoning": {"effort": ...} field. This example
// chain-of-thought tokens the model is allowed to spend before
// emitting the visible answer. Levels in order of cost:
//
//     "minimal"  -> near-zero CoT — direct answer
//     "low"      -> short CoT, fast
//     "medium"   -> default for most reasoning models
//     "high"     -> deep CoT, slower, best on hard problems
//
// This example runs ONE prompt (a logic puzzle by default) at four
// effort levels and prints, for each:
//
//     wall            — request latency
//     reasoning_tok   — usage.output_tokens_details.reasoning_tokens
//                       (the hidden CoT spend)
//     output_tok      — total output tokens (reasoning + visible)
//     answer          — the model's final visible reply
//
// You'll see two distinct shapes:
//
//   * reasoning_tok / output_tok / wall_ms climb monotonically with
//     effort — that part is unconditional, every prompt costs more
//     at higher effort. On the default snail puzzle below
//     at higher effort. The pinned DeepSeek model may spend more tokens and
//     time as the requested effort increases; the output reports both.
//
//   * answer correctness only diverges on hard-enough problems. The
//     snail puzzle is in-distribution for the model and lands at
//     "28" at every effort level. Swap the prompt for something the
//     model genuinely struggles with (constraint satisfaction, multi-
//     hop arithmetic with ambiguity, niche logic puzzles) and you'll
//     see "none" / "low" hedge or skip the trick that "high" catches.
//
// Note on supported values: reasoning controls are model/provider-specific.
// The pinned DeepSeek model is exercised with the conservative
// {none, low, medium, high} sweep below.
//
// Bypasses SchemaProvider because `reasoning` isn't part of
// CompletionParams's surface — built inline so the wire shape is
// fully visible (same pattern as example 29).
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_reasoning_effort                          # default puzzle
//   ./example_reasoning_effort "your custom prompt"     # any prompt

#include <neograph/neograph.h>
#include <neograph/async/http_client.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/run_sync.h>

#include <cppdotenv/dotenv.hpp>

#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace neograph;

static asio::awaitable<async::HttpResponse>
post_responses(async::AsyncEndpoint endpoint,
               std::string body,
               std::string auth_value) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", std::move(auth_value)},
        {"Content-Type",  "application/json"},
    };
    async::RequestOptions opts;
    opts.timeout = std::chrono::seconds(120);

    auto ex = co_await asio::this_coro::executor;
    co_return co_await async::async_post(
        ex,
        endpoint.host,
        endpoint.port,
        endpoint.prefix + "/v1/responses",
        std::move(body),
        std::move(headers),
        endpoint.tls,
        opts);
}

struct Trial {
    std::string effort;
    int         wall_ms          = 0;
    int         reasoning_tokens = 0;
    int         output_tokens    = 0;
    int         input_tokens     = 0;
    std::string answer;
    std::string error;
};

static Trial run_one(const std::string& api_key,
                     const std::string& model,
                     const std::string& question,
                     const std::string& effort) {
    Trial r;
    r.effort = effort;

    json body;
    body["model"]     = model;
    body["input"]     = question;
    body["reasoning"] = json{{"effort", effort}};
    body["provider"] = json{{"zdr", true}};

    auto endpoint = async::split_async_endpoint("https://openrouter.ai/api");

    auto t0 = std::chrono::steady_clock::now();
    async::HttpResponse resp;
    try {
        resp = async::run_sync(post_responses(
            endpoint, body.dump(), "Bearer " + api_key));
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    r.wall_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());

    if (resp.status != 200) {
        r.error = "HTTP " + std::to_string(resp.status) + ": "
                + resp.body.substr(0, 300);
        return r;
    }

    auto j = json::parse(resp.body);

    // Extract the visible assistant text from output[].
    if (j.contains("output") && j["output"].is_array()) {
        for (const auto& item : j["output"]) {
            if (item.value("type", "") != "message") continue;
            if (!item.contains("content") || !item["content"].is_array()) continue;
            for (const auto& part : item["content"]) {
                if (part.value("type", "") == "output_text") {
                    r.answer += part.value("text", "");
                }
            }
        }
    }

    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        r.input_tokens  = u.value("input_tokens", 0);
        r.output_tokens = u.value("output_tokens", 0);
        // Reasoning models nest the hidden-CoT count here. Older
        // non-reasoning models don't emit this — we just leave it 0.
        if (u.contains("output_tokens_details") &&
            u["output_tokens_details"].is_object()) {
            r.reasoning_tokens =
                u["output_tokens_details"].value("reasoning_tokens", 0);
        }
    }
    return r;
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY (env or .env)\n";
            return 1;
        }
        const std::string model = "deepseek/deepseek-v4-flash-0731";

        // Default problem: the snail-in-a-well puzzle. Single correct
        // answer (28), classic last-day-doesn't-slip trick. At "none"
        // models routinely return 30 (naïve 30/(3-2)) or 15 (forgetting
        // the slip). With more reasoning they catch the boundary case
        // and emit 28. Easy to eyeball whether each effort level
        // converged to the right number.
        std::string question = (argc >= 2)
            ? argv[1]
            : "A snail is at the bottom of a 30-foot well. Each day it "
              "climbs 3 feet. Each night, while it sleeps, it slides "
              "down 2 feet. On what day does the snail first reach the "
              "top of the well? Answer with just an integer day number "
              "and a one-sentence justification.";

        std::cout << "\nmodel    : " << model
                  << "\nquestion : " << question << "\n";

        const std::vector<std::string> efforts =
            {"none", "low", "medium", "high"};

        for (const auto& effort : efforts) {
            std::cout << "\n── effort=" << effort
                      << " ────────────────────────────────────\n";
            auto t = run_one(api_key, model, question, effort);
            if (!t.error.empty()) {
                std::cout << "  ERROR: " << t.error << "\n";
                continue;
            }
            std::cout
                << "  wall            : " << t.wall_ms << " ms\n"
                << "  reasoning_tok   : " << t.reasoning_tokens << "\n"
                << "  output_tok      : " << t.output_tokens
                << " (reasoning + visible)\n"
                << "  input_tok       : " << t.input_tokens << "\n"
                << "  answer          : " << t.answer << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
