// jarvis/src/orchestrator/intent_router_node.cpp
//
// IntentRouterNode implementation — the brain of JARVIS.
// Takes the user utterance and memory context and makes a single small LLM call
// to produce the JSON decision written to the route_decision channel.
//
// Output JSON shape:
//   {
//     "mode": "chat" | "direct" | "delegate" | "parallel",
//     "tool_calls": [ { "tool": "<name>", "args": {...} } ],
//     "delegate_to": "<agent_name>" | null,
//     "skip_synthesis": bool,
//     "reasoning_short": "<1-sentence English log>"
//   }
//
// "chat" = the synthesizer answers directly from its own knowledge + memory
// without tools or delegation; unknown catalog/registry targets demote to chat.

#include "intent_router_node.h"
#include "mcp_catalog.h"
#include "agent_dispatcher.h"
#include "../provider_support.h"

#include <neograph/neograph.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace jarvis::orchestrator {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

IntentRouterNode::IntentRouterNode(std::string             name,
                                   const neograph::json&   cfg,
                                   const McpCatalog&       catalog,
                                   const AgentDispatcher&  dispatcher,
                                   std::shared_ptr<neograph::Provider> provider)
    : name_(std::move(name))
    , catalog_(catalog)
    , dispatcher_(dispatcher)
    , provider_(std::move(provider))
{
    // The live Jarvis profile is intentionally pinned to one OpenRouter model.
    // This prevents graph JSON or process environment overrides from routing
    // a supposedly equivalent example through a different provider/model.
    model_ = "~deepseek/deepseek-v4-flash-latest";
    output_channel_ = cfg.value("output_channel", "route_decision");
    prompt_file_    = cfg.value("prompt_file",    "config/persona.txt");
    prompt_section_ = cfg.value("prompt_section", "router");
}

// ─────────────────────────────────────────────────────────────────────────────
// compose_system_prompt()
// Extract the ===<section>=== block from persona.txt and append tool/agent lists
// ─────────────────────────────────────────────────────────────────────────────

std::string IntentRouterNode::compose_system_prompt() const
{
    // ① Read persona.txt
    std::ifstream file(prompt_file_);
    if (!file.is_open()) {
        // Keep the graph running even if the file is missing — continue with an
        // empty string so the LLM can still operate from the schema alone.
    }

    std::string section_text;
    if (file.is_open()) {
        // ② Extract the requested section delimited by ===<section>===
        // Rule: a line consisting solely of ===<name>=== starts the section;
        // the next ===...=== line or EOF ends it.
        const std::string target_header = "===" + prompt_section_ + "===";

        std::string line;
        bool in_section = false;
        std::ostringstream buf;

        while (std::getline(file, line)) {
            // Blank lines are included when inside the active section
            bool is_section_header = (line.size() >= 6 &&
                                      line.front() == '=' &&
                                      line.back()  == '=');

            if (is_section_header) {
                if (in_section) {
                    // Start of another section → end of the current section
                    break;
                }
                if (line == target_header) {
                    in_section = true;
                }
                // Skip other section headers
                continue;
            }

            if (in_section) {
                buf << line << '\n';
            }
        }

        section_text = buf.str();
        // Strip trailing newlines
        while (!section_text.empty() && section_text.back() == '\n') {
            section_text.pop_back();
        }
    }

    // ③ Append MCP tool list
    std::string tool_block = catalog_.render_for_router_prompt();

    // ④ Append specialist agent list
    std::string agent_block = dispatcher_.render_for_router_prompt();

    // ⑤ Append routing_hints (if present)
    std::string hints = catalog_.routing_hints_text();

    // ⑥ Assemble the full system prompt
    std::ostringstream out;

    if (!section_text.empty()) {
        out << section_text << "\n\n";
    }

    if (!tool_block.empty()) {
        out << "=== Available Tools (MCP) ===\n"
            << tool_block << "\n\n";
    }

    if (!agent_block.empty()) {
        out << "=== Specialist Agents (A2A) ===\n"
            << agent_block << "\n\n";
    }

    if (!hints.empty()) {
        out << "=== Routing Hints ===\n"
            << hints << "\n\n";
    }

    // Reinforce JSON-only output (also stated in persona.txt)
    out << "IMPORTANT: respond with a single JSON object only. "
           "No markdown, no prose, no code fence.";

    return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// safe_parse_or_fallback()
// Parse the LLM reply as JSON; return a safe fallback if parsing fails or required fields are missing.
// ─────────────────────────────────────────────────────────────────────────────

neograph::json IntentRouterNode::safe_parse_or_fallback(const std::string& raw) const
{
    // Safe fallback JSON — demote to chat mode so the synthesizer answers directly.
    // (Previously this fell back to direct+skip_synthesis=true and salvaged raw
    // router text straight into TTS, which leaked unfiltered router output when
    // parsing failed. Demoting to chat is structurally safe.)
    auto fallback = []() -> neograph::json {
        return {
            {"mode",             "chat"},
            {"tool_calls",       neograph::json::array()},
            {"delegate_to",      nullptr},
            {"skip_synthesis",   false},
            {"reasoning_short",  "router parse failed, fallback to chat"},
            {"_parser_fallback", true}
        };
    };

    if (raw.empty()) return fallback();

    // ① Strip markdown code fences (```json ... ``` or ``` ... ```)
    std::string text = raw;
    {
        // Trim leading and trailing whitespace
        auto ltrim = text.find_first_not_of(" \t\r\n");
        if (ltrim != std::string::npos) text = text.substr(ltrim);
        auto rtrim = text.find_last_not_of(" \t\r\n");
        if (rtrim != std::string::npos) text = text.substr(0, rtrim + 1);

        // If it starts with ```json or ```, strip the first line and the closing ```
        if (text.size() >= 3 && text.substr(0, 3) == "```") {
            auto first_newline = text.find('\n');
            if (first_newline != std::string::npos) {
                text = text.substr(first_newline + 1);
            }
            // Strip trailing ```
            auto last_fence = text.rfind("```");
            if (last_fence != std::string::npos) {
                text = text.substr(0, last_fence);
            }
            // Trim leading and trailing whitespace again
            ltrim = text.find_first_not_of(" \t\r\n");
            if (ltrim != std::string::npos) text = text.substr(ltrim);
            rtrim = text.find_last_not_of(" \t\r\n");
            if (rtrim != std::string::npos) text = text.substr(0, rtrim + 1);
        }
    }

    // ② Attempt JSON parsing
    neograph::json parsed;
    try {
        parsed = neograph::json::parse(text);
    } catch (...) {
        return fallback();
    }

    // ③ Check required field "mode"
    if (!parsed.contains("mode") || !parsed["mode"].is_string()) {
        return fallback();
    }

    // ④ Check that mode is in the allowed set
    const std::string mode = parsed["mode"].get<std::string>();
    if (mode != "chat" && mode != "direct" && mode != "delegate" &&
        mode != "parallel") {
        return fallback();
    }

    // ⑤ Fill missing optional fields with defaults
    if (!parsed.contains("tool_calls") || !parsed["tool_calls"].is_array()) {
        parsed["tool_calls"] = neograph::json::array();
    }
    if (!parsed.contains("delegate_to")) {
        parsed["delegate_to"] = nullptr;
    }
    if (!parsed.contains("skip_synthesis") || !parsed["skip_synthesis"].is_boolean()) {
        parsed["skip_synthesis"] = false;
    }
    if (!parsed.contains("reasoning_short")) {
        parsed["reasoning_short"] = "";
    }

    return parsed;
}

// ─────────────────────────────────────────────────────────────────────────────
// run() — coroutine entry point. Reads NodeInput and writes the decision to route_decision.
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
IntentRouterNode::run(neograph::graph::NodeInput in)
{
    // ① Read inputs from state
    // state.get() returns neograph::json; use fallback if null or not a string.
    const auto& state = in.state;

    auto json_to_str = [](const neograph::json& j,
                          const std::string& fallback) -> std::string {
        if (j.is_string()) return j.get<std::string>();
        return fallback;
    };

    const std::string user_text = json_to_str(state.get("user_text"), "");
    const std::string user_lang = json_to_str(state.get("user_lang"), "en");

    // MemoryLookupNode writes memory_context as an object ({recent_turns, prefs,
    // last_topic}) — accepting strings only would always yield "" and hide prior
    // turns from the router. Serialize non-null objects with dump().
    std::string memory_context;
    {
        const auto& mc = state.get("memory_context");
        if (mc.is_string())     memory_context = mc.get<std::string>();
        else if (!mc.is_null()) memory_context = mc.dump();
    }

    // ①.5 Empty-turn guard — on a pre-EOF shutdown cycle or empty input, exit via
    //     no-op routing without calling the LLM. Otherwise shutdown would run a
    //     phantom turn through router + synth + TTS saying "your message was empty".
    //     (tool_dispatch echoes empty user_text → final_text "" → TTS skips empty
    //      text, so this path is completely silent.)
    {
        const auto& sd = state.get("__shutdown__");
        const bool shutting_down = sd.is_boolean() && sd.get<bool>();
        if (shutting_down || user_text.empty()) {
            neograph::json noop = {
                {"mode",            "direct"},
                {"tool_calls",      neograph::json::array()},
                {"delegate_to",     nullptr},
                {"skip_synthesis",  true},
                {"reasoning_short", "empty turn — no-op"}
            };
            neograph::graph::NodeOutput out;
            out.writes.push_back({output_channel_, noop});
            co_return out;
        }
    }

    // ② Compose system prompt (file I/O + tool list rendering)
    // Built on each call since the header keeps no cached prompt member and
    // the tool list may change at runtime.
    const std::string system_prompt = compose_system_prompt();
    // [DEBUG] Only when JARVIS_DEBUG=1 — verify that the router actually
    //         receives the tool catalog.
    static const bool debug_on = [] {
        const char* v = std::getenv("JARVIS_DEBUG");
        return v && v[0] && v[0] != '0';
    }();
    if (debug_on) {
        std::cerr << "[router][debug] sys_prompt(" << system_prompt.size()
                  << " bytes) head=\n--------\n"
                  << system_prompt.substr(0, 800)
                  << (system_prompt.size() > 800 ? "\n...[truncated]" : "")
                  << "\n--------\n";
    }

    // ③ Build user message
    std::string user_msg =
        "User said: " + user_text +
        "\nLanguage: " + user_lang +
        "\nMemory: " + memory_context;

    // ④ Actual typed request; routing consumes a text projection only.
    // Reasoning models share the output budget between hidden reasoning and the
    // router JSON — add reasoning headroom and re-ask once at double the budget
    // on an empty MaxTokens reply. Do not demote empty completions or provider failures to chat.
    auto completion = co_await jarvis::providers::complete_with_visible_text(in.ctx,
        [&](unsigned attempt) {
            std::vector<sp::Message> messages;
            messages.push_back(examples::message(sp::Role::System, system_prompt));
            messages.push_back(examples::message(sp::Role::User, user_msg));
            return jarvis::providers::contextual_request(
                jarvis::providers::request(*provider_, std::move(messages), 0.1,
                                          jarvis::providers::output_budget(300, attempt),
                                          neograph::ProviderMode::Collect, model_), in.ctx);
        },
        [&](neograph::ProviderRequest request, unsigned attempt) {
            return invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, name_, attempt));
        });
    jarvis::providers::require_completed_router_output(completion);

    // ⑥ Extract response text → validate JSON
    const std::string raw_content = neograph::outcome_text(*completion);
    if (debug_on) {
        std::cerr << "[router][debug] raw="
                  << raw_content.substr(0, 240) << "\n";
    }
    neograph::json decision = safe_parse_or_fallback(raw_content);

    // ⑥.5 Validate decision targets — check not only schema but whether the target
    //     exists. Hallucinated agents (including the literal string "null") or
    //     unknown tool names are filtered out here, and a decision with no valid
    //     target is demoted to chat so the synthesizer answers directly from
    //     knowledge and memory (preventing delegate_to:"null" error strings from polluting context).
    {
        const std::string mode = decision.value("mode", "chat");

        if (mode == "delegate") {
            std::string agent;
            if (decision.contains("delegate_to") &&
                decision["delegate_to"].is_string()) {
                agent = decision["delegate_to"].get<std::string>();
            }
            const auto* entry = dispatcher_.find(agent);
            if (agent.empty() || agent == "null" || !entry || !entry->client) {
                std::cerr << "[router] delegate 강등 → chat — 에이전트 '"
                          << agent << "' 은 레지스트리에 없음\n";
                decision["mode"] = "chat";
            }
        } else if (mode == "direct" || mode == "parallel") {
            neograph::json valid_calls = neograph::json::array();
            if (decision.contains("tool_calls") &&
                decision["tool_calls"].is_array()) {
                for (const auto& call : decision["tool_calls"]) {
                    const std::string t = call.value("tool", "");
                    if (!t.empty() && catalog_.find_tool(t)) {
                        valid_calls.push_back(call);
                    } else if (!t.empty()) {
                        std::cerr << "[router] 도구 호출 제거 — '" << t
                                  << "' 은 카탈로그에 없음\n";
                    }
                    // direct takes at most 1 call
                    if (mode == "direct" && !valid_calls.empty()) break;
                }
            }
            if (valid_calls.empty()) {
                std::cerr << "[router] " << mode
                          << " 강등 → chat — 유효한 도구 호출 0개\n";
                decision["mode"] = "chat";
            }
            decision["tool_calls"] = valid_calls;
        }

        // Normalize chat — no tools, delegation, or synthesis bypass
        if (decision["mode"] == "chat") {
            decision["tool_calls"]    = neograph::json::array();
            decision["delegate_to"]   = nullptr;
            decision["skip_synthesis"] = false;
        }
    }

    // Enforce skip_synthesis rule — tools declared with skip_synthesis_hint=false
    // in the catalog always go through synthesis regardless of the LLM decision.
    // Prevents raw tool output (ISO dates/numbers) from being spoken directly by
    // TTS; the hint is also in the prompt, but is enforced deterministically
    // here in case the LLM ignores it.
    if (decision.value("skip_synthesis", false) &&
        decision.contains("tool_calls") && decision["tool_calls"].is_array()) {
        for (const auto& call : decision["tool_calls"]) {
            const std::string t = call.value("tool", "");
            if (!t.empty() && !catalog_.allows_skip_synthesis(t)) {
                decision["skip_synthesis"] = false;
                std::cerr << "[router] skip_synthesis 강제 해제 — '" << t
                          << "' 은 카탈로그 hint=false\n";
                break;
            }
        }
    }

    if (debug_on) {
        std::cerr << "[router][debug] parsed=" << decision.dump() << "\n";
    }

    // ⑦ Write result to NodeOutput
    // (The old Fix E salvage path — pushing raw router text into tool_results for
    //  verbatim TTS on parser fallback — was replaced by the chat fallback.)
    neograph::graph::NodeOutput out;
    out.writes.push_back({output_channel_, decision});

    co_return out;
}

}  // namespace jarvis::orchestrator
