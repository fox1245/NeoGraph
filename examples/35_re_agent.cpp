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
//   ./example_re_agent --max-steps 80 --model gpt-4o
//
// The agent's final tool-free message should be a JSON summary; we dump it
// to stdout for diff against ground_truth.json (human eyeball for now —
// next iteration will add an automated scorer).

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/client.h>
#include <neograph/graph/react_graph.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <string>
#include <cstdlib>

namespace {
constexpr const char* kSystemPrompt = R"(You are a senior binary reverse-engineering analyst.
The user has loaded a STRIPPED ELF binary into Ghidra. The ghidra-mcp tools below let you
read decompilation and write back names / comments to the live Ghidra project.

Workflow — follow it exactly:
  1. Call `list_methods` to enumerate every function.
  2. Filter out libc/runtime glue. Skip ANY function whose name matches:
       _start, _init, _fini, frame_dummy, register_tm_clones, deregister_tm_clones,
       __do_global_*, __libc_*, _dl_*, __cxa_finalize, __gmon_start__, __stack_chk_fail,
       and any name starting with "FUN_" that decompiles to a single jump/thunk.
     The remaining FUN_xxxxxx names are the user functions you must analyze.
  3. For EACH user function:
       a. `decompile_function` with its current name. Read the pseudo-C carefully.
       b. Decide a precise, idiomatic snake_case name (e.g. `xor_decrypt`, `compute_checksum`).
       c. `rename_function` from the old FUN_xxxxxx name to your chosen name.
       d. `set_decompiler_comment` at the function's entry address with a one-line summary
          starting with a verb (e.g. "Validates argv length and stores license string").
  4. When every user function is renamed, your FINAL message must be a JSON object:
     {
       "recovered": [
         {"original": "FUN_00101234", "renamed": "xor_decrypt",
          "summary": "...", "tags": ["crypto", "xor"]},
         ...
       ]
     }
     Output the JSON ONLY in your final message — no prose, no markdown fence.

Be decisive. Do not over-explore. The binary is small (under 10 user functions).
)";
}  // namespace

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    int max_steps = 80;
    std::string model = "gpt-4o-mini";
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
        // 1. Spawn ghidra-mcp stdio bridge. It HTTP-talks to the Ghidra
        //    plugin's embedded HTTP server. Override target via env vars:
        //      GHIDRA_MCP_BRIDGE  — path to bridge_mcp_ghidra.py launcher
        //      GHIDRA_SERVER_URL  — http://host:port/ of the plugin server
        //    Defaults match the dockerized setup (compose maps 18080→8080).
        const char* bridge_path = std::getenv("GHIDRA_MCP_BRIDGE");
        const char* server_url  = std::getenv("GHIDRA_SERVER_URL");
        std::vector<std::string> bridge_argv = {
            "/root/mcp-servers/GhidraMCP/.venv/bin/python",
            bridge_path ? bridge_path
                        : "/root/mcp-servers/GhidraMCP/bridge_mcp_ghidra.py",
            "--ghidra-server",
            server_url ? server_url : "http://127.0.0.1:18080/",
            "--transport", "stdio",
        };
        std::cerr << "[*] Spawning ghidra-mcp stdio bridge...\n";
        neograph::mcp::MCPClient mcp(bridge_argv);

        auto tools = mcp.get_tools();
        std::cerr << "[*] " << tools.size() << " ghidra-mcp tools discovered:\n";
        for (const auto& t : tools) {
            auto def = t->get_definition();
            std::cerr << "    - " << def.name << "\n";
        }
        if (tools.empty()) {
            std::cerr << "[!] No tools — is the Ghidra GUI running with a project open?\n";
            return 2;
        }

        // 2. LLM provider.
        neograph::llm::OpenAIProvider::Config llm_cfg;
        llm_cfg.api_key = api_key;
        llm_cfg.default_model = model;
        std::shared_ptr<neograph::Provider> provider =
            neograph::llm::OpenAIProvider::create(llm_cfg);

        // 3. ReAct graph with ghidra-mcp tools wired in.
        auto engine = neograph::graph::create_react_graph(
            provider, std::move(tools), kSystemPrompt);

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
                } else if (ev.type == T::CHANNEL_WRITE) {
                    std::cerr << "\n[WRITE " << ev.node_name << "] "
                              << ev.data.dump().substr(0, 800) << "\n";
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
        std::cerr << "\n\n--- final JSON (stdout) ---\n";

        // The ReAct graph appends to channel "messages" (list reducer);
        // the final assistant message is the last element.
        if (result.output.contains("channels") &&
            result.output["channels"].contains("messages") &&
            result.output["channels"]["messages"].contains("value")) {
            auto msgs = result.output["channels"]["messages"]["value"];
            if (msgs.is_array() && !msgs.empty()) {
                auto last = msgs[msgs.size() - 1];
                if (last.contains("content") && last["content"].is_string()) {
                    std::cout << last["content"].get<std::string>() << "\n";
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
