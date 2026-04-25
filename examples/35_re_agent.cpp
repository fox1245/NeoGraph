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
  2. SKIP these — they are libc / CRT scaffolding, NOT user code, even if
     list_methods returns them with no underscore prefix:
       _start, _init, _fini, _INIT_0, _FINI_0, _DT_INIT, _DT_FINI,
       entry, __libc_start_main, __libc_csu_init, __libc_csu_fini,
       frame_dummy, register_tm_clones, deregister_tm_clones,
       __do_global_dtors_aux, __do_global_ctors_aux, __cxa_finalize,
       __gmon_start__, __stack_chk_fail, __printf_chk,
       _dl_*, _ITM_*, puts, strlen, fwrite,
       init_function, empty_function_*  (these names appear in stripped
         ELF binaries as small CRT stubs — they are NOT user code; do not
         rename them and do not include them in your final JSON),
       any FUN_xxxxxx that decompiles to a single jump/return/thunk.
     If unsure: decompile first; if the body has no real business logic
     (only a tail-call to __libc_start_main, a few register moves, or just
     `return;`) — SKIP it.
  3. For EACH remaining USER function (typically 5–10 in a small binary):
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
     Include ONLY the user functions you renamed; do NOT list the CRT
     scaffolding you skipped.

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
        std::cerr << "\n--- final JSON (stdout) ---\n";

        // Primary path: ReAct sets result.output["final_response"] when the
        // last message in the channel is an assistant message (see
        // graph_engine.cpp). Other examples (02, 04, 06) use the same path.
        if (result.output.contains("final_response") &&
            result.output["final_response"].is_string()) {
            std::cout << result.output["final_response"].get<std::string>() << "\n";
        } else if (result.output.contains("channels") &&
                   result.output["channels"].contains("messages") &&
                   result.output["channels"]["messages"].contains("value")) {
            // Fallback: walk messages backward for the last non-empty assistant
            // content. Only reached if final_response wasn't set (e.g. the run
            // hit max_steps mid-tool-loop and the last message is a tool result).
            auto msgs = result.output["channels"]["messages"]["value"];
            if (msgs.is_array()) {
                for (size_t i = msgs.size(); i-- > 0;) {
                    auto m = msgs[i];
                    if (m.contains("role") && m["role"] == "assistant" &&
                        m.contains("content") && m["content"].is_string() &&
                        !m["content"].get<std::string>().empty()) {
                        std::cerr << "[*] final_response missing — using "
                                     "channels.messages["
                                  << i << "] fallback\n";
                        std::cout << m["content"].get<std::string>() << "\n";
                        break;
                    }
                }
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
