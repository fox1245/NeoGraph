// NeoGraph RE Agent — shared building blocks (header-only).
//
// Extracted from examples/35_re_agent.cpp during commit A of the
// parallel fan-out roadmap (plan_re_agent_fanout.md). Both the
// sequential ReAct example (35) and the upcoming parallel fan-out
// example (36) reuse these helpers verbatim:
//
//   - kSystemPrompt        — the "senior RE analyst" prompt + skip rules.
//   - make_provider()      — env-driven provider selection
//                            (OPENROUTER_API_KEY → OpenRouter HTTP/SSE).
//   - spawn_ghidra_bridge()— stdio MCP bridge spawn + tool discovery
//                            + LOCAL_TOOL_SUBSET filter.
//   - extract_final_response()
//                          — pull the agent's final assistant message
//                            from RunResult, with channels.messages fallback.
//
// Header-only on purpose: keeps commit A a pure refactor (no CMake
// changes, no new translation unit). All non-trivial functions are
// `inline`; static-local linkage is fine because each example pulls
// its own copy.
#pragma once

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/mcp/client.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neograph::re_agent {

// System prompt — workflow + ELF/PE skip rules + final-JSON contract.
// Verified live on crackme01 (OpenRouter DeepSeek, matched_score 0.92,
inline constexpr const char* kSystemPrompt = R"(You are a senior binary reverse-engineering analyst.
The user has loaded a STRIPPED ELF binary into Ghidra. The ghidra-mcp tools below let you
read decompilation and write back names / comments to the live Ghidra project.

Workflow — follow it exactly:
  1. Call `list_methods` to enumerate every function.
  2. SKIP these — they are runtime / compiler scaffolding, NOT user code,
     even if list_methods returns them with no underscore prefix.

     ELF / Linux:
       _start, _init, _fini, _INIT_0, _FINI_0, _DT_INIT, _DT_FINI,
       entry, __libc_start_main, __libc_csu_init, __libc_csu_fini,
       frame_dummy, register_tm_clones, deregister_tm_clones,
       __do_global_dtors_aux, __do_global_ctors_aux, __cxa_finalize,
       __gmon_start__, __stack_chk_fail, __printf_chk,
       _dl_*, _ITM_*, puts, strlen, fwrite,
       init_function, empty_function_*.

     PE / Windows:
       _DllMainCRTStartup, __DllMainCRTStartup, DllMain (often a thin
         wrapper around _DllMainCRTStartup — skip),
       _CRT_INIT, _initterm, _initterm_e,
       __security_init_cookie, __security_check_cookie,
       __GSHandlerCheck*, __CxxFrameHandler*,
       guard_check_icall, _guard_check_icall_fptr (Control Flow Guard),
       Catch_All@*, Catch@* (C++ exception unwinder shims),
       thunk_FUN_* (linker thunks — usually a single jmp; skip unless
         the body has real logic),
       __onexit, _atexit family.

     Any FUN_xxxxxx that decompiles to a single jump / return / thunk.

     If unsure: decompile first; if the body has no real business logic
     (only a tail-call, a few register moves, or just `return;`) — SKIP.

     ALREADY-NAMED EXPORTS (e.g. J2K_*, K2J_*): these are the DLL's
     public API. Do NOT rename. You MAY include them in the final JSON
     with `"already_named": true` and a useful summary — that is the
     most valuable output for the user (they need to wrap this DLL).

     MICROSOFT DEMANGLED C++ METHODS — treat as already-named, do NOT
     rename, do NOT decompile to refine the name:
       ClassName::method (e.g. TWord::Init, CRecInfoEx::default_ctor)
       ~ClassName (destructors)
       operator=, operator new, operator delete, operator(...)
       `default_constructor_closure', `vector_deleting_destructor',
       `scalar_deleting_destructor', `vftable', `vbtable'
     If Ghidra's Demangler already gave you a class::method name,
     trust it and SKIP. Decompiling these is a token-burn trap that
     yields no new info — Ghidra already extracted the structure
     from the mangled symbol. Same rule for K2J's CWinApp / CFrameWnd
     / CCriticalSection MFC class methods.
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

/// Build the OpenRouter Responses provider for the RE agent.
///
/// OpenRouter exposes the OpenAI-compatible Responses endpoint over HTTPS/SSE.
/// The provider-routing object opts every request into Zero Data Retention.
///
/// @param api_key  OpenRouter API key; the environment value wins when set.
/// @return Shared provider; caller stores it in `std::shared_ptr<Provider>`.
inline std::shared_ptr<neograph::Provider>
make_provider(const std::string& model, const char* api_key) {
    const char* env_key = std::getenv("OPENROUTER_API_KEY");
    neograph::llm::SchemaProvider::Config llm_cfg;
    llm_cfg.schema_path       = "openai_responses";
    llm_cfg.api_key           = (env_key && *env_key) ? env_key : (api_key ? api_key : "");
    llm_cfg.base_url_override = "https://openrouter.ai/api";
    llm_cfg.default_model     = model;
    llm_cfg.timeout_seconds   = 600;
    llm_cfg.provider_routing  = {{"zdr", true}};
    llm_cfg.use_websocket     = false;
    return neograph::llm::SchemaProvider::create(llm_cfg);
}

/// Result of `spawn_ghidra_bridge` — keeps the MCP client (which owns
/// the spawned subprocess) alongside the discovered tool list. Caller
/// must move both out (or hold the MCPClient at least as long as any
/// MCPTool that still holds a session reference internally).
///
/// `client` is held via unique_ptr because MCPClient owns a std::mutex
/// (Round 3 audit, NeoGraph 7457a09) and is therefore neither copyable
/// nor movable — but `GhidraBridge` itself needs to be returned by
/// value from `spawn_ghidra_bridge`, so the heap indirection restores
/// move semantics without touching the upstream class.
struct GhidraBridge {
    std::unique_ptr<neograph::mcp::MCPClient> client;
    std::vector<std::unique_ptr<neograph::Tool>> tools;
};

/// Spawn the ghidra-mcp stdio bridge subprocess and discover its tools.
///
/// Env overrides:
///   * GHIDRA_MCP_BRIDGE  — path to bridge_mcp_ghidra.py launcher.
///   * GHIDRA_SERVER_URL  — http://host:port/ of the plugin server.
///   * LOCAL_TOOL_SUBSET  — when set, keep only the 4 RE-relevant
///                          tools (list_methods, decompile_function,
///                          rename_function, set_decompiler_comment).
///                          Use for local 4-8B models whose attention
///                          drowns in 27 tool descriptions.
///
/// Throws on subprocess spawn / handshake failure (propagated from
/// MCPClient). Returns an empty `tools` vector if the Ghidra GUI side
/// is not up — caller should treat that as an actionable error.
inline GhidraBridge spawn_ghidra_bridge() {
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

    GhidraBridge out{
        std::make_unique<neograph::mcp::MCPClient>(bridge_argv), {}};
    out.tools = out.client->get_tools();
    std::cerr << "[*] " << out.tools.size() << " ghidra-mcp tools discovered.\n";

    if (out.tools.empty()) {
        std::cerr << "[!] No tools — is the Ghidra GUI running with a project open?\n";
        return out;
    }

    if (std::getenv("LOCAL_TOOL_SUBSET")) {
        std::vector<std::unique_ptr<neograph::Tool>> kept;
        for (auto& t : out.tools) {
            const auto name = t->get_name();
            if (name == "list_methods" || name == "decompile_function" ||
                name == "rename_function" || name == "set_decompiler_comment") {
                kept.push_back(std::move(t));
            }
        }
        out.tools = std::move(kept);
        std::cerr << "[*] LOCAL_TOOL_SUBSET=1 → " << out.tools.size()
                  << " tools kept:\n";
    } else {
        std::cerr << "[*] tools listed:\n";
    }
    for (const auto& t : out.tools) {
        std::cerr << "    - " << t->get_definition().name << "\n";
    }
    return out;
}

/// Pull the agent's final assistant message out of a `RunResult.output`.
///
/// Primary path: `result.output["final_response"]` (set by
/// `graph_engine.cpp` when the last channel message is an assistant
/// turn). Same convention as examples 02, 04, 06.
///
/// Fallback: walk `channels.messages.value` backward for the last
/// non-empty assistant `content` string. Reached only when the run
/// hit `max_steps` mid-tool-loop and `final_response` wasn't set.
///
/// Returns "" if neither path yields a string. Caller decides how to
/// handle empty (the sequential example exits 0 with no stdout).
inline std::string extract_final_response(const neograph::graph::RunResult& result) {
    if (result.output.contains("final_response") &&
        result.output["final_response"].is_string()) {
        return result.output["final_response"].get<std::string>();
    }
    if (result.output.contains("channels") &&
        result.output["channels"].contains("messages") &&
        result.output["channels"]["messages"].contains("value")) {
        const auto& msgs = result.output["channels"]["messages"]["value"];
        if (msgs.is_array()) {
            for (size_t i = msgs.size(); i-- > 0;) {
                const auto& m = msgs[i];
                if (m.contains("role") && m["role"] == "assistant" &&
                    m.contains("content") && m["content"].is_string() &&
                    !m["content"].get<std::string>().empty()) {
                    std::cerr << "[*] final_response missing — using "
                                 "channels.messages["
                              << i << "] fallback\n";
                    return m["content"].get<std::string>();
                }
            }
        }
    }
    return {};
}

}  // namespace neograph::re_agent
