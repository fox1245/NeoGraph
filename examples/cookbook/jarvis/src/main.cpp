// jarvis/src/main.cpp — JARVIS entry point
//
// Flow:
//   1. Load .env / environment variables (OPENROUTER_API_KEY, etc.)
//   2. config/mcp_catalog.json    → McpCatalog (connect MCP servers + cache tools)
//   3. config/agent_registry.json → AgentDispatcher (fetch A2A agent cards)
//   4. Select LLM Provider: OpenAI-compatible OpenRouterProvider when
//      OPENROUTER_API_KEY is set, otherwise MockProvider
//   5. Register all custom JARVIS node types with NodeFactory
//   6. Compile config/jarvis_graph.json → GraphEngine
//   7. Expose JARVIS itself as an A2A server (when agent_registry.json self.enabled=true)
//   8. Main loop: one turn = engine->run() — the voice_in node reads stdin;
//      exit the loop when the __shutdown__ channel is true or on SIGINT/SIGTERM
//
// smoke-args: @SRC@/examples/cookbook/jarvis/config-demo/mock
// smoke-env: JARVIS_MEMORY_FILE=@TMP@/jarvis_memory.json
// smoke-stdin: hello there
// smoke-stdin: what time is it
// smoke-expect: [jarvis:tts][en] [mock] hello there
// smoke-expect: [jarvis:tts][en] [mock] what time is it

#include <neograph/neograph.h>
#include "provider_support.h"
#include <neograph/graph/store.h>
#include <cppdotenv/dotenv.hpp>

#include "audio/mic_input.h"
#include "audio/tts_output.h"
#include "stt/whisper_node.h"
#include "stt/moonshine_node.h"
#include "orchestrator/mcp_catalog.h"
#include "orchestrator/agent_dispatcher.h"
#include "orchestrator/intent_router_node.h"
#include "memory/conversation_store.h"
#include "memory/json_file_store.h"

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Global shutdown flag — set by the SIGINT / SIGTERM handlers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::atomic<bool> g_shutdown_requested{false};

void install_signal_handlers() {
    std::signal(SIGINT,  [](int) { g_shutdown_requested.store(true); });
    std::signal(SIGTERM, [](int) { g_shutdown_requested.store(true); });
}

// ─────────────────────────────────────────────────────────────────────────────
// MockProvider — echo provider used when OPENROUTER_API_KEY is not set.
//
// When called by IntentRouterNode:
//   - Always returns a mock routing JSON regardless of the system prompt.
//   - Routes the graph along direct + skip_synthesis=true straight to TTS.
// ─────────────────────────────────────────────────────────────────────────────

using MockProvider = jarvis::providers::MockProvider;

// ─────────────────────────────────────────────────────────────────────────────
// load_prompt_section() — extract the ===<section>=== block from persona.txt.
// Same rule as IntentRouterNode::compose_system_prompt; empty string if missing.
// ─────────────────────────────────────────────────────────────────────────────

std::string load_prompt_section(const std::string& path,
                                const std::string& section)
{
    std::ifstream file(path);
    if (!file.is_open()) return "";

    const std::string target_header = "===" + section + "===";
    std::string line;
    bool in_section = false;
    std::ostringstream buf;

    while (std::getline(file, line)) {
        bool is_section_header = (line.size() >= 6 &&
                                  line.front() == '=' &&
                                  line.back()  == '=');
        if (is_section_header) {
            if (in_section) break;
            if (line == target_header) in_section = true;
            continue;
        }
        if (in_section) buf << line << '\n';
    }

    std::string text = buf.str();
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

// ─────────────────────────────────────────────────────────────────────────────
// register_custom_node_types() — register JARVIS node types with NodeFactory.
//
// Built-in NeoGraph types ("llm_call", "intent_classifier", etc.) are already
// registered. The JARVIS-specific "intent_classifier" is registered under the
// same name to override the built-in one (last registration wins).
// ─────────────────────────────────────────────────────────────────────────────

void register_custom_node_types(
    const jarvis::orchestrator::McpCatalog&       catalog,
    const jarvis::orchestrator::AgentDispatcher&  dispatcher,
    std::shared_ptr<neograph::Provider>           router_provider,
    std::shared_ptr<neograph::Provider>           synth_provider)
{
    using namespace neograph::graph;
    auto& factory = NodeFactory::instance();

    // ── 0) Register two conditional-edge routing functions ──────────────────
    //     - Used by the router branches in jarvis_graph.json.
    //     - The router writes a JSON object to the route_decision channel;
    //       NeoGraph's default route_channel handles scalar values only, so
    //       custom conditions extract the nested fields.
    auto& cond_reg = neograph::graph::ConditionRegistry::instance();
    cond_reg.register_condition("route_by_mode",
        [](const GraphState& state) -> std::string {
            auto rd = state.get("route_decision");
            if (rd.is_object() && rd.contains("mode") && rd["mode"].is_string()) {
                const std::string m = rd["mode"].get<std::string>();
                if (m == "chat" || m == "direct" || m == "delegate" ||
                    m == "parallel") {
                    return m;
                }
            }
            // Safe fallback — chat (synthesizer answers directly) is the least harmful path
            return "chat";
        });
    cond_reg.register_condition("route_by_skip_synth",
        [](const GraphState& state) -> std::string {
            auto rd = state.get("route_decision");
            if (rd.is_object() && rd.contains("skip_synthesis"))
                return rd["skip_synthesis"].get<bool>() ? "true" : "false";
            return "false";
        });

    // ── 1) Voice input (mock: stdin) ────────────────────────────────────────
    factory.register_type("voice_in",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::audio::MicInputNode>(name, cfg);
        });

    // ── 2) STT (mock: mock_text passthrough + language detection) ───────────
    // Moonshine STT (ONNX, edge/low-latency alternative to whisper_stt)
    factory.register_type("moonshine_stt",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::stt::MoonshineSttNode>(name, cfg);
        });

    factory.register_type("whisper_stt",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::stt::WhisperSttNode>(name, cfg);
        });

    // ── 3) TTS (mock: console output) ───────────────────────────────────────
    factory.register_type("supertonic_tts",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::audio::SupertonicTtsNode>(name, cfg);
        });

    // ── 4) Memory lookup ────────────────────────────────────────────────────
    factory.register_type("memory_lookup",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::memory::MemoryLookupNode>(name, cfg);
        });

    // ── 5) Memory commit ────────────────────────────────────────────────────
    factory.register_type("memory_commit",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&) {
            return std::make_unique<jarvis::memory::MemoryCommitNode>(name, cfg);
        });

    // ── 6) Intent router — override built-in intent_classifier with JARVIS version ──
    factory.register_type("intent_classifier",
        [&catalog, &dispatcher, router_provider](
            const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            return std::make_unique<jarvis::orchestrator::IntentRouterNode>(
                name, cfg, catalog, dispatcher, router_provider);
        });

    // ── 7) Single MCP tool call (direct path) ───────────────────────────────
    factory.register_type("tool_dispatch",
        [&catalog](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            // Inline GraphNode — captures catalog and executes tool_calls[0]
            class ToolDispatchNode : public neograph::graph::GraphNode {
            public:
                ToolDispatchNode(std::string n,
                                 const neograph::json& c,
                                 const jarvis::orchestrator::McpCatalog& cat)
                    : name_(std::move(n)), cfg_(c), catalog_(cat) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // Read the first tool_call from the route_decision channel
                    const auto& rd = in.state.get("route_decision");
                    if (!rd.is_object() || !rd.contains("tool_calls") ||
                        !rd["tool_calls"].is_array() || rd["tool_calls"].empty()) {
                        // Zero tool calls. If IntentRouterNode already populated
                        // tool_results via fallback salvage, preserve it: because the
                        // channel uses an append reducer, pushing a user_text echo here
                        // would bury the salvaged text under the last element.
                        const auto& existing = in.state.get("tool_results");
                        if (existing.is_array() && !existing.empty()) {
                            co_return out;  // leave writes empty — keep router salvage
                        }
                        // If tool_results is also empty, fall back to echoing user_text for mock runs.
                        const auto& ut = in.state.get("user_text");
                        std::string user_text = ut.is_string() ? ut.get<std::string>() : "";
                        out.writes.push_back({"tool_results",
                            neograph::json::array({neograph::json{{"text", user_text}}})});
                        co_return out;
                    }

                    const auto& call = rd["tool_calls"][0];
                    std::string tool_name = call.value("tool", "");
                    neograph::json args   = call.value("args", neograph::json::object());

                    neograph::Tool* tool = catalog_.find_tool(tool_name);
                    if (!tool) {
                        out.writes.push_back({"tool_results",
                            neograph::json::array({neograph::json{
                                {"error", "tool not found: " + tool_name}}})});
                        co_return out;
                    }

                    neograph::json result;
                    try {
                        std::string raw = co_await tool->execute_async(args);
                        result = raw;
                    } catch (const std::exception& e) {
                        result = neograph::json{{"error", std::string(e.what())}};
                    }

                    out.writes.push_back({"tool_results",
                        neograph::json::array({neograph::json{
                            {"tool",   tool_name},
                            {"result", result}}})});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                const jarvis::orchestrator::McpCatalog& catalog_;
            };

            return std::make_unique<ToolDispatchNode>(name, cfg, catalog);
        });

    // ── 8) Parallel MCP tool fan-out (parallel path) ────────────────────────
    // execute_async + make_parallel_group — same idiom as Core's ToolDispatchNode.
    // Overlaps I/O across N calls so wall time ≈ max(latency) per max_concurrent window.
    factory.register_type("parallel_tool_fanout",
        [&catalog](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class ParallelFanoutNode : public neograph::graph::GraphNode {
            public:
                ParallelFanoutNode(std::string n,
                                   const neograph::json& c,
                                   const jarvis::orchestrator::McpCatalog& cat)
                    : name_(std::move(n)), cfg_(c), catalog_(cat) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    const auto& rd = in.state.get("route_decision");
                    if (!rd.is_object() || !rd.contains("tool_calls") ||
                        !rd["tool_calls"].is_array()) {
                        out.writes.push_back({"tool_results", neograph::json::array()});
                        co_return out;
                    }

                    // Snapshot the call list so worker coroutines own their inputs
                    // (neograph::json iterators do not satisfy std iterator traits,
                    //  so copy elements in a loop instead of using a range constructor)
                    std::vector<neograph::json> calls;
                    calls.reserve(rd["tool_calls"].size());
                    for (const auto& c : rd["tool_calls"]) calls.push_back(c);

                    // Worker: resolve one tool and run execute_async. Absorb exceptions
                    // into the result JSON so the worker never throws.
                    auto worker = [this](neograph::json call)
                        -> asio::awaitable<neograph::json> {
                        std::string tool_name = call.value("tool", "");
                        neograph::json args   = call.value("args", neograph::json::object());

                        neograph::json result;
                        neograph::Tool* tool = catalog_.find_tool(tool_name);
                        if (!tool) {
                            result = neograph::json{{"error", "tool not found: " + tool_name}};
                        } else {
                            try {
                                std::string raw = co_await tool->execute_async(args);
                                result = raw;
                            } catch (const std::exception& e) {
                                result = neograph::json{{"error", std::string(e.what())}};
                            }
                        }
                        // Avoid GCC 13 coroutine ICE — co_return a named local variable
                        neograph::json entry = neograph::json{
                            {"tool",   tool_name},
                            {"result", result}};
                        co_return entry;
                    };

                    neograph::json results = neograph::json::array();

                    if (calls.size() == 1) {
                        // Run a single call inline without parallel group machinery
                        neograph::json one = co_await worker(calls[0]);
                        results.push_back(std::move(one));
                    } else if (!calls.empty()) {
                        const std::size_t max_conc = static_cast<std::size_t>(
                            std::max(1, cfg_.value("max_concurrent", 4)));
                        auto ex = co_await asio::this_coro::executor;

                        for (std::size_t base = 0; base < calls.size(); base += max_conc) {
                            const std::size_t end =
                                std::min(base + max_conc, calls.size());

                            using DeferredOp = decltype(asio::co_spawn(
                                ex, worker(std::declval<neograph::json>()),
                                asio::deferred));
                            std::vector<DeferredOp> ops;
                            ops.reserve(end - base);
                            for (std::size_t i = base; i < end; ++i) {
                                ops.push_back(asio::co_spawn(
                                    ex, worker(calls[i]), asio::deferred));
                            }

                            auto [order, excs, values] =
                                co_await asio::experimental::make_parallel_group(
                                    std::move(ops))
                                    .async_wait(asio::experimental::wait_for_all(),
                                                asio::use_awaitable);
                            (void)order;  // preserve call order rather than completion order

                            for (std::size_t i = 0; i < values.size(); ++i) {
                                if (excs[i]) {
                                    // Defensive fallback (e.g. bad_alloc) even though worker
                                    // absorbs tool exceptions — key the error to the original call
                                    std::string err;
                                    try {
                                        std::rethrow_exception(excs[i]);
                                    } catch (const std::exception& e) {
                                        err = e.what();
                                    } catch (...) {
                                        err = "unknown tool failure";
                                    }
                                    results.push_back(neograph::json{
                                        {"tool", calls[base + i].value("tool", "")},
                                        {"result", neograph::json{{"error", err}}}});
                                } else {
                                    results.push_back(std::move(values[i]));
                                }
                            }
                        }
                    }

                    out.writes.push_back({"tool_results", results});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                const jarvis::orchestrator::McpCatalog& catalog_;
            };

            return std::make_unique<ParallelFanoutNode>(name, cfg, catalog);
        });

    // ── 9) A2A delegation (delegate path) ───────────────────────────────────
    factory.register_type("a2a_delegate",
        [&dispatcher](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class A2ADelegateNode : public neograph::graph::GraphNode {
            public:
                A2ADelegateNode(std::string n,
                                const neograph::json& c,
                                jarvis::orchestrator::AgentDispatcher& disp)
                    : name_(std::move(n)), cfg_(c), dispatcher_(disp) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // Extract the agent name from route_decision.delegate_to
                    const auto& rd = in.state.get("route_decision");
                    std::string agent_name;
                    if (rd.is_object() && rd.contains("delegate_to") &&
                        rd["delegate_to"].is_string()) {
                        agent_name = rd["delegate_to"].get<std::string>();
                    }

                    // Extract user_text
                    const auto& ut = in.state.get("user_text");
                    std::string user_text = ut.is_string() ? ut.get<std::string>() : "";

                    auto* entry = dispatcher_.find(agent_name);
                    if (!entry || !entry->client) {
                        // Return a mock reply if the agent is not found
                        out.writes.push_back({"delegated_reply",
                            "[mock-delegate] 에이전트 '" + agent_name +
                            "' 를 찾을 수 없음. 입력: " + user_text});
                        co_return out;
                    }

                    std::string reply;
                    try {
                        // Async A2A call — does not block the event loop, so background
                        // triggers and self-server requests still make progress while waiting.
                        neograph::a2a::MessageSendParams params;
                        static std::atomic<std::uint64_t> msg_seq{0};
                        params.message.message_id =
                            "jarvis-delegate-" + std::to_string(++msg_seq);
                        params.message.role = neograph::a2a::Role::User;
                        params.message.parts.push_back(
                            neograph::a2a::Part::text_part(user_text));

                        neograph::a2a::Task task = co_await
                            entry->client->send_message_async(params);

                        // Extract text reply from Task
                        // Priority: status.message.parts[0].text
                        //           → artifacts[0].parts[0].text
                        //           → fallback when empty
                        if (task.status.message.has_value() &&
                            !task.status.message->parts.empty()) {
                            reply = task.status.message->parts[0].text;
                        } else if (!task.artifacts.empty() &&
                                   !task.artifacts[0].parts.empty()) {
                            reply = task.artifacts[0].parts[0].text;
                        } else {
                            reply = "(a2a 응답 없음)";
                        }
                    } catch (const std::exception& e) {
                        reply = std::string("[a2a 오류] ") + e.what();
                    }

                    out.writes.push_back({"delegated_reply", reply});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
                jarvis::orchestrator::AgentDispatcher& dispatcher_;
            };

            // dispatcher is passed as const &, but find() is non-const;
            // const_cast is safe here because find() only performs a lookup
            return std::make_unique<A2ADelegateNode>(
                name, cfg,
                const_cast<jarvis::orchestrator::AgentDispatcher&>(dispatcher));
        });

    // ── 10) channel_merge — copy the first non-null input channel value to output ──
    factory.register_type("channel_merge",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class ChannelMergeNode : public neograph::graph::GraphNode {
            public:
                ChannelMergeNode(std::string n, const neograph::json& c)
                    : name_(std::move(n)), cfg_(c) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    // cfg.inputs: array of candidate channel names
                    // cfg.output: destination channel name
                    std::string output_ch = cfg_.value("output", std::string(""));
                    if (output_ch.empty() || !cfg_.contains("inputs") ||
                        !cfg_["inputs"].is_array()) {
                        co_return out;
                    }

                    for (const auto& ch_name_j : cfg_["inputs"]) {
                        if (!ch_name_j.is_string()) continue;
                        const std::string ch = ch_name_j.get<std::string>();
                        const auto& val = in.state.get(ch);
                        // Pick the first non-null value
                        if (!val.is_null()) {
                            out.writes.push_back({output_ch, val});
                            co_return out;
                        }
                    }
                    // All inputs are null — write an empty string to output
                    out.writes.push_back({output_ch, std::string("")});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
            };

            return std::make_unique<ChannelMergeNode>(name, cfg);
        });

    // ── 10.5) llm_call — override NeoGraph's built-in llm_call with the JARVIS synthesizer.
    //         The built-in llm_call expects a messages channel, which the JARVIS
    //         graph does not populate. Instead, assemble conversation history as a
    //         **messages array** (user/assistant roles) — inlining history as a JSON
    //         dump inside the user message caused the model to treat it as text to copy
    //         and repeat earlier replies verbatim. Proper role turns eliminate that
    //         failure mode.
    factory.register_type("llm_call",
        [synth_provider](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class JarvisSynthNode : public neograph::graph::GraphNode,
                                    public neograph::RuntimeInterpositionConsumer {
            public:
                JarvisSynthNode(std::string n, neograph::json c,
                                std::shared_ptr<neograph::Provider> p)
                    : name_(std::move(n)), cfg_(std::move(c)), provider_(std::move(p)) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    auto get_str = [&](const char* ch, const std::string& dflt = "") {
                        auto v = in.state.get(ch);
                        return v.is_string() ? v.get<std::string>() : dflt;
                    };
                    std::string user_text = get_str("user_text");
                    std::string user_lang = get_str("user_lang", "en");
                    std::string delegated = get_str("delegated_reply");

                    std::string tool_summary;
                    auto tr = in.state.get("tool_results");
                    if (tr.is_array() && !tr.empty()) tool_summary = tr.dump();

                    // ── System prompt — wire up the [synth] section of persona.txt.
                    //    (Previously a hardcoded prompt was used and persona [synth]
                    //     was unused; fall back to the minimal default only if missing.)
                    std::string sys = load_prompt_section(
                        cfg_.value("prompt_file",    std::string("config/persona.txt")),
                        cfg_.value("prompt_section", std::string("synth")));
                    if (sys.empty()) {
                        sys = "You are JARVIS — Tony Stark's terse, witty AI butler. "
                              "One or two sentences max. No markdown, no JSON, "
                              "plain speech suitable for text-to-speech.";
                    }
                    sys += "\n\nReply in the user's language code (" + user_lang + "). "
                           "Spell numbers, dates and times naturally in that language.";

                    // ── Collect conversation history (recent_turns from memory_lookup) ──
                    struct PastTurn { std::string user; std::string assistant; };
                    std::vector<PastTurn> history;
                    std::int64_t last_ts = 0;
                    auto mc = in.state.get("memory_context");
                    if (mc.is_object() && mc.contains("recent_turns") &&
                        mc["recent_turns"].is_array()) {
                        for (const auto& t : mc["recent_turns"]) {
                            if (!t.is_object()) continue;
                            history.push_back({t.value("user_text",  std::string("")),
                                               t.value("final_text", std::string(""))});
                            if (t.contains("ts") && t["ts"].is_number_integer()) {
                                last_ts = t["ts"].get<std::int64_t>();
                            }
                        }
                    }

                    // Session boundary note — if the last remembered turn is old, mark it
                    // so deictic references like "just now" do not resolve to an old session.
                    if (last_ts > 0) {
                        using namespace std::chrono;
                        const std::int64_t now_s = duration_cast<seconds>(
                            system_clock::now().time_since_epoch()).count();
                        const std::int64_t gap_min = (now_s - last_ts) / 60;
                        if (gap_min >= 15) {
                            sys += "\n(Note: the conversation history above your "
                                   "current turn ended about "
                                   + std::to_string(gap_min) +
                                   " minutes ago — treat it as an earlier session, "
                                   "not something the user said just now.)";
                        }
                    }

                    // ── Assemble messages array: system → history (user/assistant) → current turn ──
                    // All live Jarvis calls use the pinned OpenRouter model;
                    // mock mode is selected before this node is invoked.
                    // Persistent memory is an explicit speech projection, not native replay.
                    std::vector<sp::Message> messages;
                    messages.push_back(examples::message(sp::Role::System, sys));
                    for (const auto& turn : history) {
                        if (!turn.user.empty())
                            messages.push_back(examples::message(sp::Role::User, turn.user));
                        if (!turn.assistant.empty())
                            messages.push_back(examples::message(sp::Role::Assistant, turn.assistant));
                    }

                    std::string usr = user_text;
                    if (!tool_summary.empty())
                        usr += "\n\n[Tool result JSON — use the key fact]: " + tool_summary;
                    if (!delegated.empty())
                        usr += "\n\n[Specialist reply — speak it as your own]: " + delegated;
                    messages.push_back(examples::message(sp::Role::User, usr));

                    // ── Streaming synthesis — emit [jarvis:ttft] once on stdout when the
                    //    first token arrives. The benchmark driver uses this timestamp to
                    //    measure TTFT (when the user starts hearing the answer). Modern LLM
                    //    services stream by default, so the benchmark aligns with that.
                    bool ttft_emitted = false;
                    auto on_tok = [&ttft_emitted](const sp::Event& event) {
                        const auto* delta = std::get_if<sp::PartDelta>(&event);
                        if (!ttft_emitted && delta && delta->payload.kind == sp::PartKind::Text &&
                            delta->payload.channel == sp::DeltaChannel::Content &&
                            !delta->payload.bytes.empty()) {
                            ttft_emitted = true;
                            std::cout << "[jarvis:ttft]" << std::endl;
                        }
                    };
                    // Add headroom for hidden reasoning. Only a completed empty MaxTokens
                    // reply is re-asked once at double the budget (ordinals 0 / 2; 1 is verbatim retry).
                    auto reply = co_await jarvis::providers::complete_with_visible_text(in.ctx,
                        [&](unsigned attempt) {
                            auto request = jarvis::providers::request(
                                *provider_, messages, 0.4,
                                jarvis::providers::output_budget(220, attempt),
                                neograph::ProviderMode::Stream);
                            request.on_event = on_tok;
                            return jarvis::providers::contextual_request(std::move(request), in.ctx);
                        },
                        [&](neograph::ProviderRequest request, unsigned attempt) {
                            return invoke_provider(provider_, std::move(request), {}, {},
                                neograph::graph::provider_call_broker(in.ctx),
                                neograph::graph::make_provider_call_identity(
                                    in.ctx, name_, attempt == 0 ? 0 : 2));
                        });
                    jarvis::providers::require_completed_spoken_output(reply);
                    std::string final_text = neograph::outcome_text(*reply);

                    // ── Verbatim repeat guard — regenerate once if the trimmed reply matches
                    //    an earlier assistant turn word-for-word. Last line of defense before
                    //    commit so repeated text does not enter the Store and self-reinforce.
                    auto trim = [](const std::string& s) -> std::string {
                        const char* ws = " \t\r\n";
                        auto b = s.find_first_not_of(ws);
                        if (b == std::string::npos) return "";
                        auto e = s.find_last_not_of(ws);
                        return s.substr(b, e - b + 1);
                    };
                    const std::string trimmed = trim(final_text);
                    bool verbatim = false;
                    if (!trimmed.empty()) {
                        for (const auto& turn : history) {
                            if (!turn.assistant.empty() &&
                                trim(turn.assistant) == trimmed) {
                                verbatim = true;
                                break;
                            }
                        }
                    }
                    if (verbatim) {
                        std::cerr << "[synth] 복창 감지 — 과거 답변과 verbatim 일치, "
                                     "1회 재생성\n";
                        const auto& generated = neograph::outcome_messages(*reply);
                        messages.insert(messages.end(), generated.begin(), generated.end());
                        messages.push_back(examples::message(sp::Role::User,
                            "Your draft repeated one of your earlier replies "
                            "word-for-word. Compose a fresh answer to the user's "
                            "CURRENT message, in their language."));
                        auto retry = co_await jarvis::providers::complete_with_visible_text(in.ctx,
                            [&](unsigned attempt) {
                                return jarvis::providers::contextual_request(
                                    jarvis::providers::request(*provider_, messages, 0.4,
                                        jarvis::providers::output_budget(220, attempt)), in.ctx);
                            },
                            [&](neograph::ProviderRequest request, unsigned attempt) {
                                return invoke_provider(provider_, std::move(request), {}, {},
                                    neograph::graph::provider_call_broker(in.ctx),
                                    neograph::graph::make_provider_call_identity(
                                        in.ctx, name_, attempt == 0 ? 1 : 3));
                            });
                        jarvis::providers::require_completed_spoken_output(retry);
                        final_text = neograph::outcome_text(*retry);
                    }

                    std::string out_ch = cfg_.value("output_channel",
                                                    std::string("final_text"));
                    out.writes.push_back({out_ch, final_text});
                    co_return out;
                }

                std::string get_name() const override { return name_; }
            private:
                std::string name_;
                neograph::json cfg_;
                std::shared_ptr<neograph::Provider> provider_;
            };
            return std::make_unique<JarvisSynthNode>(name, cfg, synth_provider);
        });

    // ── 11) passthrough — copy cfg.from channel value to cfg.to channel ──────
    factory.register_type("passthrough",
        [](const std::string& name, const neograph::json& cfg, const NodeContext&)
        {
            class PassthroughNode : public neograph::graph::GraphNode {
            public:
                PassthroughNode(std::string n, const neograph::json& c)
                    : name_(std::move(n)), cfg_(c) {}

                asio::awaitable<neograph::graph::NodeOutput>
                run(neograph::graph::NodeInput in) override {
                    neograph::graph::NodeOutput out;

                    std::string from_ch = cfg_.value("from", std::string(""));
                    std::string to_ch   = cfg_.value("to",   std::string(""));

                    if (from_ch.empty() || to_ch.empty()) co_return out;

                    const auto& val = in.state.get(from_ch);
                    // If the value is an array, take its last element; otherwise copy as-is
                    neograph::json elem = (val.is_array() && !val.empty())
                                         ? val[val.size() - 1]
                                         : val;

                    // If a tool_results item has the shape {"text": "..."}, extract the string.
                    // When tool_calls is empty, tool_dispatch writes {"text": user_text}, so
                    // this path forwards the user utterance directly to TTS.
                    if (elem.is_object() && elem.contains("text") && elem["text"].is_string()) {
                        elem = elem["text"];
                    }
                    // Likewise, if it has the shape {"result": ...}, extract the result value
                    // (serializing structured tool results as a string for TTS).
                    else if (elem.is_object() && elem.contains("result")) {
                        const auto& res = elem["result"];
                        elem = res.is_string() ? res : res.dump();
                    }

                    out.writes.push_back({to_ch, elem});
                    co_return out;
                }

                std::string get_name() const override { return name_; }

            private:
                std::string name_;
                neograph::json cfg_;
            };

            return std::make_unique<PassthroughNode>(name, cfg);
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON file loading helper
// ─────────────────────────────────────────────────────────────────────────────

neograph::json load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("JSON 파일 열기 실패: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return neograph::json::parse(ss.str());
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Auto-load .env file (OPENROUTER_API_KEY, etc.)
    cppdotenv::auto_load_dotenv();
    install_signal_handlers();

    const std::string config_root =
        (argc >= 2) ? argv[1] : "examples/cookbook/jarvis/config";

    try {
        // ── 1) Load MCP catalog ──────────────────────────────────────────────
        std::cerr << "[jarvis] MCP 카탈로그 로드 중...\n";
        auto catalog = jarvis::orchestrator::McpCatalog::load(
            config_root + "/mcp_catalog.json");

        // ── 2) Load A2A agent registry ───────────────────────────────────────
        std::cerr << "[jarvis] A2A 에이전트 레지스트리 로드 중...\n";
        auto dispatcher = jarvis::orchestrator::AgentDispatcher::load(
            config_root + "/agent_registry.json");

        // ── 3) Select LLM Provider ───────────────────────────────────────────
        //    Use live OpenRouter when OPENROUTER_API_KEY is set, otherwise mock
        std::shared_ptr<neograph::Provider> router_provider;
        std::shared_ptr<neograph::Provider> synth_provider;

        const char* api_key_env = std::getenv("OPENROUTER_API_KEY");
        const char* base_url_env = std::getenv("OPENROUTER_BASE_URL");
        const char* ca_file_env = std::getenv("OPENROUTER_CA_FILE");
        if (api_key_env && std::string(api_key_env).size() > 0) {
            std::cerr << "[jarvis] OpenRouter Provider 사용 (OPENROUTER_API_KEY 감지됨)\n";
            const std::string base_url = (base_url_env && *base_url_env)
                ? base_url_env : "https://openrouter.ai/api";
            // OPENROUTER_CA_FILE: additional trust anchor (PEM) for a local TLS benchmark proxy.
            const std::string ca_file = ca_file_env ? ca_file_env : "";
            router_provider = jarvis::providers::live(api_key_env, base_url, ca_file);
            synth_provider = jarvis::providers::live(api_key_env, base_url, ca_file);
        } else {
            std::cerr << "[jarvis] Mock Provider 사용 (OPENROUTER_API_KEY 없음)\n";
            router_provider = std::make_shared<MockProvider>();
            synth_provider  = std::make_shared<MockProvider>();
        }

        // ── 4) Register custom node types ────────────────────────────────────
        std::cerr << "[jarvis] 노드 타입 등록 중...\n";
        register_custom_node_types(catalog, dispatcher,
                                   router_provider, synth_provider);

        // ── 5) Compile jarvis_graph.json ─────────────────────────────────────
        std::cerr << "[jarvis] 그래프 컴파일 중: "
                  << config_root + "/jarvis_graph.json\n";
        neograph::json graph_def =
            load_json_file(config_root + "/jarvis_graph.json");

        neograph::graph::NodeContext ctx;
        ctx.provider = synth_provider;  // used by the llm_call node (response_synth)

        // ── 5.5) Create file-backed Store and inject into the engine ─────────
        // MemoryLookupNode and MemoryCommitNode read and write conversation
        // history through ctx.store. JsonFileStore persists memory across process
        // restarts ("what did I ask earlier?" works after a restart). Relative to
        // cwd — run_jarvis.sh changes into the cookbook directory before running.
        const char* mem_path_env = std::getenv("JARVIS_MEMORY_FILE");
        const std::string mem_path =
            (mem_path_env && mem_path_env[0]) ? mem_path_env
                                              : "jarvis_memory.json";
        auto jarvis_store =
            std::make_shared<jarvis::memory::JsonFileStore>(mem_path);
        auto engine_uptr = neograph::graph::GraphEngine::build(
            graph_def, neograph::graph::EngineConfig{.node_context = ctx, .store = jarvis_store});
        auto engine = std::shared_ptr<neograph::graph::GraphEngine>(engine_uptr.release());
        std::cerr << "[jarvis] 파일 영속 Store 주입 완료 (" << mem_path << ")\n";

        // ── 6) Start JARVIS A2A self-server ──────────────────────────────────
        auto self_server = dispatcher.start_self_server(engine);
        if (self_server) {
            std::cerr << "[jarvis] A2A self-server 기동 완료\n";
        }

        // ── 7) Optional background trigger graph (not enabled in this build) ─

        std::cout << "[jarvis] 온라인. 텍스트를 입력하거나 ^C 로 종료.\n";
        std::cout.flush();

        // ── 8) Main loop — run one graph turn per iteration ──────────────────
        while (!g_shutdown_requested.load()) {
            neograph::graph::RunConfig run_cfg;
            // Fixed thread_id — single-user mode
            run_cfg.thread_id = "jarvis.tony";

            auto result = engine->run(run_cfg);

            // Exit the loop when the __shutdown__ channel is true (EOF from MicInputNode)
            const auto& shutdown_val =
                result.output["channels"]["__shutdown__"]["value"];
            if (shutdown_val.is_boolean() && shutdown_val.get<bool>()) {
                std::cerr << "[jarvis] __shutdown__ 신호 수신 — 루프 종료\n";
                break;
            }
        }

        std::cout << "[jarvis] 종료 중...\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[jarvis] 치명적 오류: " << e.what() << "\n";
        return 1;
    }
}
