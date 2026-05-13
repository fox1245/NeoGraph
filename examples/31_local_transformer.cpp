// NeoGraph Example 31: Local LLM backend (OpenAI-compatible server)
//
// Demonstrates pointing NeoGraph's OpenAIProvider at a local OpenAI-
// compatible inference server — such as TransformerCPP's http_server_demo
// running a Gemma/Llama GGUF, llama.cpp's server, vLLM's OpenAI-compat
// mode, etc. Zero NeoGraph code changes — just override `base_url`.
//
// The two-process topology (NeoGraph agent ←HTTP→ inference server)
// keeps the model weights out of the agent's address space. The agent
// stays small (~7 MB RSS, ~465 KB L3 working set) even when driving a
// multi-GB model, because the inference process is a separate
// virtual-address world.
//
// Expected setup:
//   1. Start an OpenAI-compatible server on localhost. With TransformerCPP:
//        ./http_server_demo path/to/model.gguf 8090
//      With llama.cpp server, vLLM --served-model-name, etc. the same
//      /v1/chat/completions shape works.
//   2. Run this example. By default it hits http://localhost:8090 and
//      expects streaming chat/completions.
//
// Usage:
//   ./example_local_transformer [base_url] [N]
//
//   ./example_local_transformer                          # defaults: http://localhost:8090, N=3
//   ./example_local_transformer http://localhost:8080 10 # 10 sequential calls
//
// The measurement loop reports per-call TTFT + total wall + tok/s, then
// summarizes p50/p95. Useful for verifying that an in-house inference
// server is actually serving OpenAI-compatible streaming before wiring
// up a real agent graph.

#include <neograph/llm/openai_provider.h>
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
    const std::string base_url = (argc > 1) ? argv[1] : "http://localhost:8090";
    const int N = (argc > 2) ? std::atoi(argv[2]) : 3;

    neograph::llm::OpenAIProvider::Config config;
    config.api_key       = "local-no-key";     // inference server ignores auth
    config.base_url      = base_url;           // ← the only change from stock cloud usage
    config.default_model = "local-llm";        // server may ignore this (already loaded)

    auto provider = neograph::llm::OpenAIProvider::create(config);

    std::cerr << "[bench] base_url=" << base_url << "  N=" << N << "\n";
    std::cerr << "[bench] warmup (1 token ping)...\n";
    {
        neograph::CompletionParams p;
        p.model = config.default_model;
        p.max_tokens = 4;
        neograph::ChatMessage m; m.role = "user"; m.content = "hi";
        p.messages = {m};
        try { (void)neograph::async::run_sync(provider->invoke(p, nullptr)); }
        catch (const std::exception& e) {
            std::cerr << "[bench] warmup failed: " << e.what()
                      << "\n[bench] is the server up on " << base_url << " ?\n";
            return 1;
        }
    }

    std::vector<long> total_us, ttft_us;
    int total_tokens = 0;
    long total_wall_us = 0;

    const std::string prompt =
        "In one short sentence, name one interesting fact about cache memory.";

    for (int i = 0; i < N; ++i) {
        neograph::CompletionParams p;
        p.model = config.default_model;
        p.max_tokens = 64;
        neograph::ChatMessage m; m.role = "user"; m.content = prompt;
        p.messages = {m};

        const auto t0 = std::chrono::steady_clock::now();
        long ttft = -1;
        int  n_tok = 0;
        auto on_tok = [&](const std::string&) {
            if (ttft < 0) {
                ttft = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            }
            ++n_tok;
        };

        try {
            (void)neograph::async::run_sync(provider->invoke(p, on_tok));
        } catch (const std::exception& e) {
            std::cerr << "[bench] iter " << i << " failed: " << e.what() << "\n";
            return 2;
        }

        const long total = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();

        total_us.push_back(total);
        ttft_us.push_back(ttft);
        total_tokens += n_tok;
        total_wall_us += total;

        std::cerr << "[bench] iter " << i
                  << "  ttft=" << ttft / 1000.0 << " ms"
                  << "  total=" << total / 1000.0 << " ms"
                  << "  tokens=" << n_tok
                  << "  tok/s=" << (n_tok * 1e6 / std::max(1L, total))
                  << "\n";
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
              << "total tokens    : " << total_tokens << "\n"
              << "aggregate tok/s : "
              << (total_tokens * 1e6 / std::max(1L, total_wall_us)) << "\n"
              << "NeoGraph RSS    : " << (read_vmhwm_kb() / 1024.0) << " MB\n";

    return 0;
}
