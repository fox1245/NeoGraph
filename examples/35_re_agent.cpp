// NeoGraph Example 35: Reverse-Engineering Agent (self-test MVP)
//
// Drives ghidra-mcp (stdio bridge → Ghidra GUI 8080 plugin) via a ReAct loop
// to recover meaningful names + summaries for every user-defined function in
// a stripped binary.
//
// Self-test rig:
//   projects/re_agent/targets/crackme01    (stripped, 6 user functions)
//   projects/re_agent/targets/ground_truth.json
//
// Prerequisites (run once):
//   1. /root/ghidra_11.0.3_PUBLIC/ghidraRun   (start GUI, WSLg)
//   2. New project → Import the crackme01 binary → CodeBrowser
//   3. Accept auto-analysis. Plugin opens HTTP server on 127.0.0.1:8080.
//   4. echo "OPENAI_API_KEY=sk-..." > .env
//
// Usage:
//   ./example_re_agent
//   ./example_re_agent --max-steps 80 --model gpt-5.4-mini
//   # OpenAI-compat HTTP backend (OpenRouter / Ollama / vLLM / trtllm-serve):
//   LLM_BASE_URL=https://openrouter.ai/api LLM_API_KEY=sk-or-v1-... \
//     ./example_re_agent --model deepseek/deepseek-v4-pro
//
// The agent's final tool-free message should be a JSON summary; we dump it
// to stdout for diff against ground_truth.json (human eyeball for now —
// next iteration will add an automated scorer).
//
// NOTE: prompt + provider/MCP setup + final-response extraction live in
// `re_agent_common.h` so the upcoming parallel fan-out example (36)
// reuses them verbatim.

#include "re_agent_common.h"

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <cppdotenv/dotenv.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    int max_steps = 80;
    // Default to OpenAI Responses over WebSocket (commit d7c61d0). Avoids
    // v1/chat_completions REST entirely. gpt-5.4-mini chosen for stronger
    // tool-use reasoning at low cost.
    std::string model = "gpt-5.4-mini";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--max-steps" && i + 1 < argc) max_steps = std::atoi(argv[++i]);
        else if (a == "--model" && i + 1 < argc) model = argv[++i];
    }

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "OPENAI_API_KEY not set (env or .env file)\n";
        return 1;
    }

    try {
        // 1. Spawn ghidra-mcp stdio bridge + discover tools (env-controlled
        //    bridge path / server URL / LOCAL_TOOL_SUBSET filter — see
        //    re_agent_common.h::spawn_ghidra_bridge for details).
        auto bridge = neograph::re_agent::spawn_ghidra_bridge();
        if (bridge.tools.empty()) {
            return 2;
        }

        // 2. LLM provider — env-selected (LLM_BASE_URL → OpenAIProvider HTTP,
        //    else SchemaProvider WS Responses). See re_agent_common.h.
        auto provider = neograph::re_agent::make_provider(model, api_key);

        // 3. ReAct graph with ghidra-mcp tools wired in.
        auto engine = neograph::graph::create_react_graph(
            provider, std::move(bridge.tools),
            neograph::re_agent::kSystemPrompt);

        // 4. Kick it off.
        neograph::graph::RunConfig run_cfg;
        run_cfg.input = {{"messages", neograph::json::array({
            {{"role", "user"},
             {"content", "Recover names and summaries for every user-defined function "
                         "in the currently-open Ghidra project. Output the final JSON."}}
        })}};
        run_cfg.max_steps = max_steps;

        std::cerr << "\n--- agent trace ---\n";
        auto result = engine->run_stream(run_cfg,
            [](const neograph::graph::GraphEvent& ev) {
                using T = neograph::graph::GraphEvent::Type;
                if (ev.type == T::LLM_TOKEN) {
                    std::cerr << ev.data.get<std::string>() << std::flush;
                } else if (ev.type == T::NODE_START && ev.node_name == "tools") {
                    std::cerr << "\n[ghidra-mcp call]\n";
                } else if (ev.type == T::ERROR) {
                    std::cerr << "\n[ERROR " << ev.node_name << "] "
                              << ev.data.dump() << "\n";
                }
            });

        std::cerr << "\n--- /agent trace ---\n";
        std::cerr << "[*] Steps: ";
        for (size_t i = 0; i < result.execution_trace.size(); ++i) {
            std::cerr << result.execution_trace[i]
                      << (i + 1 < result.execution_trace.size() ? " → " : "");
        }
        std::cerr << "\n--- DIAG: result.output keys + msg roles ---\n";
        for (auto it = result.output.begin(); it != result.output.end(); ++it) {
            std::cerr << "  ." << it.key();
            if (it.key() == "final_response" && it.value().is_string()) {
                std::cerr << " (len=" << it.value().get<std::string>().size() << ")";
            }
            std::cerr << "\n";
        }
        if (result.output.contains("channels") &&
            result.output["channels"].contains("messages") &&
            result.output["channels"]["messages"].contains("value")) {
            auto msgs = result.output["channels"]["messages"]["value"];
            if (msgs.is_array()) {
                std::cerr << "  channels.messages.value size=" << msgs.size() << "\n";
                size_t start = msgs.size() > 4 ? msgs.size() - 4 : 0;
                for (size_t i = start; i < msgs.size(); ++i) {
                    std::cerr << "    [" << i << "] role="
                              << (msgs[i].contains("role")
                                      ? msgs[i]["role"].get<std::string>() : "?")
                              << " content_len="
                              << (msgs[i].contains("content") && msgs[i]["content"].is_string()
                                      ? msgs[i]["content"].get<std::string>().size() : 0)
                              << " tool_calls="
                              << msgs[i].contains("tool_calls") << "\n";
                }
            }
        }
        std::cerr << "\n--- final JSON (stdout) ---\n";

        // Single helper covers both the primary path
        // (result.output["final_response"]) and the channels.messages
        // backward-walk fallback. Empty string → no output (caller can
        // diff stdout against ground_truth.json either way).
        const std::string final_resp =
            neograph::re_agent::extract_final_response(result);
        if (!final_resp.empty()) {
            std::cout << final_resp << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
