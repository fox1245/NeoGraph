// NeoGraph Example 31: Local LLM backend (OpenAI-compatible server)
//
// Demonstrates an admitted typed SchemaProvider Chat descriptor pointed at
// a local OpenAI-compatible inference server, such as llama.cpp or vLLM.
//
// The two-process topology (NeoGraph agent ←HTTP→ inference server)
// keeps the model weights out of the agent's address space. The agent
// stays small (~7 MB RSS, ~465 KB L3 working set) even when driving a
// multi-GB model, because the inference process is a separate
// virtual-address world.
//
// Expected setup:
//   1. Start an OpenAI-compatible server on localhost. llama.cpp server,
//      vLLM --served-model-name, and equivalent runtimes expose the same
//      /v1/chat/completions shape.
//   2. Run this example. By default it hits http://localhost:8090 and
//      expects streaming chat/completions.
//
// Usage:
//   ./example_local_transformer [base_url] [N]
//
//   ./example_local_transformer                          # defaults: http://localhost:8090, N=3
//   ./example_local_transformer http://localhost:8080 10 # 10 sequential calls
//
// The measurement loop reports first-content latency, total wall time and
// callback chunks/s, then summarizes p50/p95. Callback chunks are not billed
// model tokens; nullable reported usage is printed separately.

#include "provider_example_support.h"
#include <neograph/async/run_sync.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

long read_vmhwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str(), "VmHWM: %ld kB", &kb);
            return kb;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string base_url = (argc > 1) ? argv[1] : "http://127.0.0.1:8090";
    const int N = (argc > 2) ? std::atoi(argv[2]) : 3;

    const std::string model = "local-llm";
    sp::runtime::Options options;
    options.api_key = "local-no-key";  // inference server ignores auth
    // The descriptor loader admits plaintext only for loopback hosts.
    auto provider = neograph::llm::SchemaProvider::create(
        examples::admitted_descriptor(base_url, "openai.chat",
            "/v1/chat/completions", "neograph-example-local-transformer"),
        std::move(options));

    std::cerr << "[bench] base_url=" << base_url << "  N=" << N << "\n";
    std::cerr << "[bench] warmup (1 token ping)...\n";
    {
        neograph::ProviderControls controls;
        controls.max_output_tokens = 4;
        auto request = neograph::make_provider_request(*provider, model,
            {examples::message(sp::Role::User, "hi")}, {}, controls);
        try { (void)examples::require_outcome(provider->invoke(std::move(request))); }
        catch (const std::exception& e) {
            std::cerr << "[bench] warmup failed: " << e.what()
                      << "\n[bench] is the server up on " << base_url << " ?\n";
            return 1;
        }
    }

    std::vector<long> total_us, ttft_us;
    std::uint64_t total_chunks = 0;
    long total_wall_us = 0;

    const std::string prompt =
        "In one short sentence, name one interesting fact about cache memory.";

    for (int i = 0; i < N; ++i) {
        neograph::ProviderControls controls;
        controls.max_output_tokens = 64;
        auto request = neograph::make_provider_request(*provider, model,
            {examples::message(sp::Role::User, prompt)}, {}, controls,
            neograph::ProviderMode::Stream);

        const auto t0 = std::chrono::steady_clock::now();
        long ttft = -1;
        std::uint64_t chunks = 0;
        request.on_event = [&](const sp::Event& event) {
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (!delta || delta->payload.kind != sp::PartKind::Text ||
                delta->payload.channel != sp::DeltaChannel::Content ||
                delta->payload.bytes.empty()) return;
            if (ttft < 0) {
                ttft = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            }
            ++chunks;
        };

        sp::runtime::Result outcome;
        try {
            outcome = examples::require_outcome(neograph::async::run_sync(
                provider->invoke_async(std::move(request))));
        } catch (const std::exception& e) {
            std::cerr << "[bench] iter " << i << " failed: " << e.what() << "\n";
            return 2;
        }

        const long total = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();

        total_us.push_back(total);
        ttft_us.push_back(ttft);
        total_chunks += chunks;
        total_wall_us += total;

        std::cerr << "[bench] iter " << i
                  << "  ttft=" << ttft / 1000.0 << " ms"
                  << "  total=" << total / 1000.0 << " ms"
                  << "  chunks=" << chunks
                  << "  chunks/s=" << (chunks * 1e6 / std::max(1L, total));
        const auto& usage = neograph::outcome_usage(*outcome);
        const auto print_count = [](const std::optional<sp::Count>& count) {
            return count ? std::to_string(count->value) : std::string("unknown");
        };
        std::cerr << "  reported_input=" << print_count(usage.input_total)
                  << "  reported_output=" << print_count(usage.output_total)
                  << "  reported_reasoning=" << print_count(usage.reasoning) << "\n";
    }

    std::sort(total_us.begin(), total_us.end());
    std::sort(ttft_us.begin(),  ttft_us.end());
    auto pct = [](const std::vector<long>& v, double p) -> long {
        if (v.empty()) return 0;
        return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * p))];
    };

    std::cout << "\n=== Summary (" << base_url << ") ===\n"
              << "N               : " << N << "\n"
              << "ttft  p50/p95   : " << pct(ttft_us, 0.50)  / 1000.0
              << " / " << pct(ttft_us, 0.95)  / 1000.0 << " ms\n"
              << "total p50/p95   : " << pct(total_us, 0.50) / 1000.0
              << " / " << pct(total_us, 0.95) / 1000.0 << " ms\n"
              << "callback chunks : " << total_chunks << "\n"
              << "aggregate chunks/s : "
              << (total_chunks * 1e6 / std::max(1L, total_wall_us)) << "\n"
              << "NeoGraph RSS    : " << (read_vmhwm_kb() / 1024.0) << " MB\n";

    return 0;
}
