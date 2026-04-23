// NeoGraph Example 32: Fully-local C++ agent (in-process Gemma via TransformerCPP)
//
// Sibling of example 31. Where 31 talks to any OpenAI-compatible HTTP
// server over the network (TransformerCPP, llama.cpp, vLLM, …), this
// example links TransformerCPP directly into the NeoGraph process and
// drives a local GGUF without any HTTP round trip.
//
// Both halves of a C++ LLM-agent stack are visible in one file:
//
//   * TransformerCPP side  — GGUF loading, tokeniser, Flash-Attention
//     inference on GPU (`transformercpp::AutoModel`, `Model::generate`).
//     https://github.com/fox1245/TransformerCPP
//
//   * NeoGraph side        — agent orchestration surface: a `Provider`
//     implementation that adapts TransformerCPP's streaming generate
//     into NeoGraph's `ChatCompletion` / `complete_stream` contract.
//
// The adapter is written inline below so the plug-in point is fully
// visible — that's the promotional angle: any C++ inference runtime
// can plug into NeoGraph by implementing 2 virtuals and ~60 lines of
// glue.
//
// Deployment-shape contrast with example 31:
//   31 (two-process, HTTP)    — agent RSS stays ~7 MB regardless of model size.
//                               Matches multi-tenant / multi-language.
//   32 (single-process, link) — no TCP, no JSON round trip. ~15 ms lower
//                               TTFT per call, but the binary now hosts
//                               the GGUF loader + CUDA kernels and runs
//                               exactly one model. Matches edge / embedded.
//
// ## Build
//
// Requires a built sibling clone of TransformerCPP. Configure NeoGraph:
//
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
//         -DNEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE=ON \
//         -DTRANSFORMERCPP_DIR=/path/to/TransformerCPP
//   cmake --build build --target example_inproc_gemma
//
// ## Run
//
//   ./build/example_inproc_gemma /path/to/gemma-4-E2B-it-Q4_K_M.gguf \
//                                 "Explain L3 cache in one sentence."

#include <transformercpp/model.h>
#include <transformercpp/generation.h>
#include <transformercpp/tokenizer.h>

#include <neograph/provider.h>
#include <neograph/llm/agent.h>
#include <neograph/tool.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
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

// =========================================================================
// Inline Provider adapter — the cross-project plug-in point.
//
// NeoGraph expects a `Provider` with complete() / complete_stream(). We
// satisfy both by calling TransformerCPP's `Model::generate` with its
// `GenerationConfig::streamer` callback. A minimal chat-template renders
// the `ChatMessage` list into a prompt; production deployments should
// use the model's own template (Gemma-4: `<start_of_turn>user\n…`).
// =========================================================================
class LocalTransformerProvider : public neograph::Provider {
public:
    explicit LocalTransformerProvider(std::shared_ptr<transformercpp::Model> model)
        : model_(std::move(model)) {}

    neograph::ChatCompletion complete(
        const neograph::CompletionParams& params) override {
        return complete_stream(params, nullptr);
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& params,
        const neograph::StreamCallback& on_chunk) override {

        const std::string prompt = render_gemma_chat(params.messages);
        const auto input = model_->tokenizer().encode(prompt);

        transformercpp::GenerationConfig cfg;
        cfg.max_new_tokens = (params.max_tokens > 0) ? params.max_tokens : 256;
        cfg.temperature    = params.temperature > 0 ? params.temperature : 0.3f;
        cfg.stop_sequences = {"<end_of_turn>", "<eos>"};

        std::string full_text;
        cfg.streamer = [&](const std::string& tok) {
            full_text += tok;
            if (on_chunk) on_chunk(tok);
        };

        (void)model_->generate(input, cfg);

        neograph::ChatCompletion out;
        out.message.role    = "assistant";
        out.message.content = std::move(full_text);
        // Token-level usage fields are left at default; TransformerCPP
        // exposes them via `generate_batch`/`BatchResult` but we'd need
        // to plumb that through for full parity — omitted for brevity.
        return out;
    }

    std::string get_name() const override { return "transformercpp-local"; }

private:
    // Gemma-4 chat template, minimal. Real deployments should pull the
    // template out of the GGUF metadata (`tokenizer.chat_template`) and
    // render with a Jinja2-lite engine; this inline shortcut is enough
    // for a single-turn demo.
    static std::string render_gemma_chat(
        const std::vector<neograph::ChatMessage>& msgs) {
        std::ostringstream os;
        os << "<bos>";
        for (const auto& m : msgs) {
            os << "<start_of_turn>" << m.role << "\n"
               << m.content
               << "<end_of_turn>\n";
        }
        os << "<start_of_turn>model\n";
        return os.str();
    }

    std::shared_ptr<transformercpp::Model> model_;
};

} // namespace

// =========================================================================
// Main — wires TransformerCPP loader + the inline Provider + a NeoGraph
// Agent that streams tokens to stdout with full telemetry.
// =========================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <model.gguf> [question]\n";
        return 1;
    }
    const std::string model_path = argv[1];
    const std::string question   = (argc > 2) ? argv[2]
        : "Explain L3 cache in one short sentence.";

    std::cout << "=== NeoGraph + TransformerCPP (in-process) ===\n"
              << "model   : " << model_path << "\n"
              << "question: " << question << "\n\n";

    // 1. Load the GGUF — TransformerCPP does the heavy lifting.
    std::cout << "[load] opening GGUF + uploading weights...\n";
    const auto t_load = std::chrono::steady_clock::now();
    auto model_uptr = transformercpp::AutoModel::from_pretrained(model_path);
    std::shared_ptr<transformercpp::Model> model(model_uptr.release());
    const long load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_load).count();
    std::cout << "[load] done in " << load_ms
              << " ms, RSS " << read_vmhwm_kb() / 1024.0 << " MB\n\n";

    // 2. Wrap as a NeoGraph Provider (adapter defined above).
    std::shared_ptr<neograph::Provider> provider =
        std::make_shared<LocalTransformerProvider>(model);

    // 3. Build a NeoGraph Agent. No tools on this run — keeping the
    //    comparison fair against example 31's single-turn bench.
    neograph::llm::Agent agent(
        provider,
        /*tools=*/{},
        /*system_prompt=*/"You are concise. Answer in one short sentence.");

    std::vector<neograph::ChatMessage> messages;
    messages.push_back({"user", question});

    // 4. Stream tokens.
    std::cout << "Agent: ";
    const auto t_run = std::chrono::steady_clock::now();
    long ttft = -1;
    int  tokens = 0;
    (void)agent.run_stream(messages, [&](const std::string& tok) {
        if (ttft < 0) {
            ttft = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_run).count();
        }
        ++tokens;
        std::cout << tok << std::flush;
    });
    const long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_run).count();

    std::cout << "\n\n=== Telemetry ===\n"
              << "model load       : " << load_ms << " ms\n"
              << "agent ttft       : " << ttft    << " ms\n"
              << "agent total wall : " << total_ms << " ms\n"
              << "tokens streamed  : " << tokens  << "\n"
              << "tok/s            : "
              << (tokens * 1000.0 / std::max(1L, total_ms)) << "\n"
              << "RSS              : " << read_vmhwm_kb() / 1024.0
              << " MB (includes the model — contrast with example 31 at ~7 MB)\n";
    return 0;
}
