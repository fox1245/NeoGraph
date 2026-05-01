#include <neograph/graph/deep_research_graph.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace neograph::graph {
namespace {

// =========================================================================
// Prompts — inspired by langchain-ai/open_deep_research/prompts.py.
//
// Kept compact on purpose: LangGraph's prompts lean heavily on Python
// formatting + dated banners that don't translate. These trim to the
// load-bearing instructions.
// =========================================================================

constexpr const char* SUPERVISOR_SYSTEM = R"(You are a lead research supervisor. Given a research brief, decompose it into focused sub-topics and dispatch parallel researchers.

Tools:
  - conduct_research(research_topic: string): dispatch one sub-researcher. Call MULTIPLE TIMES in a single assistant turn to fan out in parallel.
  - think_tool(reflection: string): private reasoning. NEVER use alone — always pair with conduct_research or research_complete.
  - research_complete(): finish researching. Call when coverage is adequate.

HARD RULES (the graph engine depends on these):
  1. On your FIRST turn you MUST call conduct_research 2–4 times in parallel. Do NOT just think_tool.
  2. Each research_topic is one SHORT paragraph (2–4 sentences) describing the question + what to look for. NOT prose-heavy — the researcher will do the reading.
  3. After researchers report back, either dispatch more conduct_research OR call research_complete. Do not loop indefinitely.
  4. Total budget: at most 2 rounds of research. Prefer quality+breadth in round 1 over many shallow rounds.
)";

constexpr const char* RESEARCHER_SYSTEM = R"(You are a focused researcher. You were given ONE topic. Investigate it using your tools and return rich, citation-backed findings.

Tools:
  - web_search(query: string): search the web. Returns a list of URLs with titles and snippets.
  - fetch_url(url: string): fetch a specific URL and return cleaned markdown of its content.
  - think_tool(reflection: string): record reasoning between steps.

Workflow:
  * Start with 1–2 web_search calls to find candidate sources.
  * Use fetch_url on the 1–3 most relevant results to read them in full.
  * When you have enough, stop calling tools and produce a final text answer: a thorough written summary of what you found, with inline URL citations.
  * Do not ask clarifying questions. Work with the topic as given.
  * Budget: a handful of tool calls. Keep moving.
)";

constexpr const char* COMPRESS_SYSTEM = R"(Compress raw research notes into a terse summary.
  * Target length: 400–700 characters. Hard ceiling: 900.
  * Keep concrete facts, numbers, and URL citations. Drop meta-commentary.
  * Output ONLY the summary, no preamble. Bulleted markdown is fine.
)";

constexpr const char* FINAL_REPORT_SYSTEM = R"(You are a senior analyst writing a final research report. You will be given:
  1. The original research brief.
  2. A collection of findings from sub-researchers (each already compressed).

Write a comprehensive markdown report that:
  * Directly answers the brief.
  * Synthesises ALL findings; do not ignore any researcher's output.
  * Uses clear section headings (`##`).
  * Preserves inline URL citations from the source notes where they appeared.
  * Does not fabricate sources. If two researchers contradict each other, present both views.

Output ONLY the final report. No preamble, no "here is the report" framing.
)";

// =========================================================================
// Helpers
// =========================================================================

// Build a ChatTool definition that Claude will emit as `tool_use`. The
// arguments JSON is interpreted downstream by SupervisorDispatchNode /
// the researcher loop.
ChatTool make_tool(const char* name, const char* description,
                   const json& parameters) {
    return {name, description, parameters};
}

std::vector<ChatTool> supervisor_tool_defs() {
    return {
        make_tool(
            "conduct_research",
            "Dispatch a focused sub-researcher on one topic. Call multiple times "
            "in one turn to fan out in parallel.",
            json{
                {"type", "object"},
                {"properties", {
                    {"research_topic", {
                        {"type", "string"},
                        {"description",
                         "A self-contained paragraph describing the topic to "
                         "research, including context and specific questions."}
                    }}
                }},
                {"required", json::array({"research_topic"})}
            }),
        make_tool(
            "think_tool",
            "Record a private reflection between research rounds. Does not "
            "dispatch any action; use this to plan.",
            json{
                {"type", "object"},
                {"properties", {
                    {"reflection", {{"type", "string"}}}
                }},
                {"required", json::array({"reflection"})}
            }),
        make_tool(
            "research_complete",
            "Signal that research coverage is sufficient and it is time to "
            "write the final report. Takes no arguments.",
            json{
                {"type", "object"},
                {"properties", json::object()}
            })
    };
}

// think_tool shared with researcher
ChatTool think_tool_def() {
    return make_tool(
        "think_tool",
        "Record a private reflection between steps. Does not call any external "
        "service; use to plan.",
        json{
            {"type", "object"},
            {"properties", {{"reflection", {{"type", "string"}}}}},
            {"required", json::array({"reflection"})}
        });
}

// Look up the last assistant message with tool_calls in the message log.
const ChatMessage* last_assistant_with_calls(
    const std::vector<ChatMessage>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) return &(*it);
    }
    return nullptr;
}

// Build a tool-result message in OpenAI-ish shape (SchemaProvider normalises
// this to Claude's {tool_use_id, content} on the wire).
ChatMessage tool_result_msg(const std::string& tool_call_id,
                            const std::string& tool_name,
                            const std::string& content) {
    ChatMessage m;
    m.role = "tool";
    m.tool_call_id = tool_call_id;
    m.tool_name = tool_name;
    m.content = content;
    return m;
}

// =========================================================================
// SupervisorLLMNode — one Claude call producing `conduct_research` /
// `think_tool` / `research_complete` tool uses. Reads/writes the
// `supervisor_messages` channel so it is isolated from the final-report
// step (which reads `notes`).
// =========================================================================
class SupervisorLLMNode : public GraphNode {
public:
    SupervisorLLMNode(std::string name, std::shared_ptr<Provider> provider,
                      std::string model)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model)) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        std::vector<ChatMessage> convo;

        // System prompt first.
        {
            ChatMessage s;
            s.role = "system";
            s.content = SUPERVISOR_SYSTEM;
            convo.push_back(std::move(s));
        }

        // Then the supervisor's running conversation.
        auto sv = state.get("supervisor_messages");
        if (sv.is_array()) {
            for (auto it = sv.begin(); it != sv.end(); ++it) {
                ChatMessage m;
                from_json(*it, m);
                convo.push_back(std::move(m));
            }
        }

        // If the conversation is empty, seed it with the research brief as
        // the first user message.
        bool has_user = false;
        for (const auto& m : convo) if (m.role == "user") { has_user = true; break; }
        if (!has_user) {
            auto brief = state.get("research_brief");
            ChatMessage u;
            u.role = "user";
            u.content = brief.is_string() ? brief.get<std::string>()
                                          : "Please research the given query.";
            convo.push_back(std::move(u));
        }

        CompletionParams params;
        params.model = model_;
        params.messages = std::move(convo);
        params.tools = supervisor_tool_defs();
        params.temperature = 0.3f;
        params.max_tokens = 2048;

        auto completion = provider_->complete(params);

        json asst;
        to_json(asst, completion.message);

        // Track how many supervisor rounds have run — dispatcher uses this
        // as a safety cap.
        int iter = 0;
        auto cur = state.get("supervisor_iterations");
        if (cur.is_number_integer()) iter = cur.get<int>();

        return {
            ChannelWrite{"supervisor_messages", json::array({asst})},
            ChannelWrite{"supervisor_iterations", json(iter + 1)}
        };
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::string               model_;
};

// =========================================================================
// SupervisorDispatchNode — reads the last assistant message in
// `supervisor_messages`, then:
//   * for each `conduct_research` call: emits a Send to "researcher"
//   * for each `think_tool` call: produces a tool-result echo and loops back
//   * for `research_complete`: Command → "final_report"
//   * if no tool calls (model bailed): Command → "final_report"
//   * if iteration cap reached: Command → "final_report"
// =========================================================================
class SupervisorDispatchNode : public GraphNode {
public:
    SupervisorDispatchNode(std::string name, int max_iterations,
                           int max_concurrent)
        : name_(std::move(name))
        , max_iter_(max_iterations)
        , max_concurrent_(max_concurrent) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    // Stage-4 async bridge: forwards to sync execute_full so Command/Send
    // still propagate on the run_async path. Without this, the Stage-4
    // async-first default would drop the Command we emit here.
    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        co_return execute_full(state);
    }

    NodeResult execute_full(const GraphState& state) override {
        NodeResult nr;

        int iter = 0;
        auto cur = state.get("supervisor_iterations");
        if (cur.is_number_integer()) iter = cur.get<int>();

        // Parse supervisor_messages and find the tail assistant tool-calls.
        std::vector<ChatMessage> sv_msgs;
        auto sv = state.get("supervisor_messages");
        if (sv.is_array()) {
            for (auto it = sv.begin(); it != sv.end(); ++it) {
                ChatMessage m;
                from_json(*it, m);
                sv_msgs.push_back(std::move(m));
            }
        }

        const ChatMessage* last = last_assistant_with_calls(sv_msgs);
        if (!last) {
            // No tool call — supervisor either answered prose or bailed.
            // Route to final report; whatever prose it produced will be
            // ignored (final report reads `notes`).
            nr.command = Command{"final_report", {}};
            return nr;
        }

        // =====================================================================
        // Anthropic invariant: every `tool_use` in the assistant message MUST
        // be paired with a `tool_result` in the very next user message, else
        // the API returns HTTP 400. Our strategy:
        //
        //   * We emit one tool_result per tool_call unconditionally. The
        //     content depends on whether this dispatcher will act on it:
        //       - conduct_research (dispatched) → researcher fan-in writes
        //         the ACTUAL result. We do NOT pre-emit here; the researcher
        //         writes the tool_result carrying the compressed findings.
        //       - conduct_research (over concurrency cap) → synthetic
        //         "skipped, concurrency cap hit" so the id is paired.
        //       - think_tool → synthetic reflection echo.
        //       - research_complete → synthetic "acknowledged".
        //       - unknown tool (model hallucination) → synthetic error.
        //
        //   * Truncating conduct_research past max_concurrent WITHOUT
        //     emitting a paired tool_result was the bug that produced
        //     "tool_use ids were found without tool_result blocks" on the
        //     next supervisor round.
        // =====================================================================

        // First pass: classify every tool_call, capture the subset that will
        // actually fan out to researchers (bounded by max_concurrent).
        std::vector<ToolCall> research_calls_to_dispatch;
        std::vector<ToolCall> research_calls_skipped;
        std::vector<ToolCall> think_calls;
        std::vector<ToolCall> research_complete_calls;
        std::vector<ToolCall> unknown_calls;
        int research_seen = 0;

        for (const auto& tc : last->tool_calls) {
            if (tc.name == "conduct_research") {
                if (research_seen < max_concurrent_) {
                    research_calls_to_dispatch.push_back(tc);
                } else {
                    research_calls_skipped.push_back(tc);
                }
                ++research_seen;
            } else if (tc.name == "think_tool") {
                think_calls.push_back(tc);
            } else if (tc.name == "research_complete") {
                research_complete_calls.push_back(tc);
            } else {
                unknown_calls.push_back(tc);
            }
        }

        // Build paired tool_result echoes for everything EXCEPT the
        // conduct_research calls we're actually dispatching — those are
        // paired by the researcher fan-in instead.
        auto build_echo = [](const ToolCall& tc, const std::string& content) {
            auto m = tool_result_msg(tc.id, tc.name, content);
            json j; to_json(j, m);
            return j;
        };

        json sv_updates = json::array();

        for (const auto& tc : think_calls) {
            std::string content = "noted";
            try {
                auto args = json::parse(tc.arguments);
                if (args.contains("reflection") &&
                    args["reflection"].is_string()) {
                    content = "reflection logged: "
                        + args["reflection"].get<std::string>();
                }
            } catch (...) { /* keep fallback */ }
            sv_updates.push_back(build_echo(tc, content));
        }

        for (const auto& tc : research_calls_skipped) {
            sv_updates.push_back(build_echo(tc,
                "skipped: conduct_research concurrency cap ("
                + std::to_string(max_concurrent_)
                + ") hit within this turn"));
        }

        for (const auto& tc : unknown_calls) {
            sv_updates.push_back(build_echo(tc,
                "unknown tool name; ignored. Valid tools: conduct_research, "
                "think_tool, research_complete"));
        }

        // research_complete wins: short-circuit to final_report, echoing
        // every tool_call (including research_complete itself + any others).
        if (!research_complete_calls.empty()) {
            for (const auto& tc : research_complete_calls) {
                sv_updates.push_back(build_echo(tc, "acknowledged"));
            }
            // The research_calls_to_dispatch will NOT run; we still need
            // their tool_results paired or the next (never-made) supervisor
            // call would 400. But since we're going to final_report and not
            // back to supervisor, no Claude call observes these messages —
            // we still write them for a clean conversation log.
            for (const auto& tc : research_calls_to_dispatch) {
                sv_updates.push_back(build_echo(tc,
                    "not dispatched: research_complete signalled in same turn"));
            }
            nr.writes.push_back(
                ChannelWrite{"supervisor_messages", sv_updates});
            nr.command = Command{"final_report", {}};
            return nr;
        }

        // Iteration cap: force finish. Pair every tool_use so the saved
        // checkpoint remains a valid Anthropic-format conversation (useful
        // for debug inspection and future resume semantics).
        if (iter >= max_iter_) {
            for (const auto& tc : research_calls_to_dispatch) {
                sv_updates.push_back(build_echo(tc,
                    "not dispatched: supervisor iteration budget exhausted"));
            }
            nr.writes.push_back(
                ChannelWrite{"supervisor_messages", sv_updates});
            nr.command = Command{"final_report", {}};
            return nr;
        }

        // If the supervisor ONLY called think_tool / unknown / skipped
        // (no research to actually run), loop back to let it try again.
        if (research_calls_to_dispatch.empty()) {
            if (!sv_updates.empty()) {
                nr.writes.push_back(
                    ChannelWrite{"supervisor_messages", sv_updates});
            }
            nr.command = Command{"supervisor", {}};
            return nr;
        }

        // Emit Sends for each dispatchable conduct_research. Researcher
        // fan-in writes the paired tool_result for each of these.
        for (const auto& tc : research_calls_to_dispatch) {
            std::string topic;
            try {
                auto args = json::parse(tc.arguments);
                if (args.contains("research_topic") &&
                    args["research_topic"].is_string()) {
                    topic = args["research_topic"].get<std::string>();
                }
            } catch (...) { /* empty topic handled by researcher */ }

            nr.sends.push_back(Send{"researcher", json{
                {"current_topic",    topic},
                {"current_call_id",  tc.id}
            }});
        }

        // Write the think/skipped/unknown echoes alongside the Sends. The
        // researcher fan-in will append its own tool_results after. The
        // Claude schema's consecutive-user-role merging concatenates them
        // into a single valid tool_result content array.
        if (!sv_updates.empty()) {
            nr.writes.push_back(
                ChannelWrite{"supervisor_messages", sv_updates});
        }

        // After Sends finish and fan-in, edge routes back to supervisor.
        return nr;
    }

private:
    std::string name_;
    int         max_iter_;
    int         max_concurrent_;
};

// =========================================================================
// ResearcherNode — invoked by Send with (`current_topic`, `current_call_id`).
// Runs its own LLM ↔ tools loop inline (not a subgraph: simpler fan-in
// semantics and avoids per-Send subgraph compilation cost).
// Writes:
//   * raw_notes (appended): one entry = { topic, call_id, summary }
//   * supervisor_messages (appended): one tool-result paired with call_id,
//     carrying the compressed summary as content.
// =========================================================================
class ResearcherNode : public GraphNode {
public:
    ResearcherNode(std::string name, std::shared_ptr<Provider> provider,
                   std::vector<Tool*> tools, std::string model,
                   int max_iterations)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , tools_(std::move(tools))
        , model_(std::move(model))
        , max_iter_(max_iterations) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        std::string topic;
        {
            auto t = state.get("current_topic");
            if (t.is_string()) topic = t.get<std::string>();
        }
        std::string call_id;
        {
            auto c = state.get("current_call_id");
            if (c.is_string()) call_id = c.get<std::string>();
        }

        if (topic.empty()) {
            // Supervisor emitted a malformed conduct_research; record a
            // terse failure note so the supervisor sees something.
            return fanin_writes(call_id, topic,
                "(researcher received empty topic; skipped)");
        }

        std::vector<ChatTool> tool_defs;
        tool_defs.reserve(tools_.size() + 1);
        for (auto* t : tools_) tool_defs.push_back(t->get_definition());
        tool_defs.push_back(think_tool_def());

        std::vector<ChatMessage> convo;
        {
            ChatMessage s; s.role = "system"; s.content = RESEARCHER_SYSTEM;
            convo.push_back(std::move(s));
            ChatMessage u; u.role = "user"; u.content = topic;
            convo.push_back(std::move(u));
        }

        std::string final_text;

        for (int iter = 0; iter < max_iter_; ++iter) {
            CompletionParams params;
            params.model = model_;
            params.messages = convo;
            params.tools = tool_defs;
            params.temperature = 0.3f;
            params.max_tokens = 2048;

            auto completion = provider_->complete(params);
            auto& msg = completion.message;
            convo.push_back(msg);

            if (msg.tool_calls.empty()) {
                final_text = msg.content;
                break;
            }

            // Execute each tool call, append paired tool-result messages.
            for (const auto& tc : msg.tool_calls) {
                ChatMessage tm;
                tm.role = "tool";
                tm.tool_call_id = tc.id;
                tm.tool_name = tc.name;

                if (tc.name == "think_tool") {
                    tm.content = "noted";
                    convo.push_back(std::move(tm));
                    continue;
                }

                auto it = std::find_if(tools_.begin(), tools_.end(),
                    [&](Tool* t) { return t->get_name() == tc.name; });

                if (it == tools_.end()) {
                    tm.content = std::string(R"({"error":"unknown tool: )")
                        + tc.name + "\"}";
                } else {
                    try {
                        auto args = json::parse(tc.arguments);
                        tm.content = (*it)->execute(args);
                    } catch (const std::exception& e) {
                        tm.content = std::string(R"({"error":")")
                            + e.what() + "\"}";
                    }
                }
                convo.push_back(std::move(tm));
            }
        }

        // Compress the transcript into a dense summary. Reuses the same
        // model; could be swapped for a cheaper one later.
        std::string compressed = compress(convo, topic);

        return fanin_writes(call_id, topic, compressed);
    }

private:
    std::string compress(const std::vector<ChatMessage>& convo,
                         const std::string& topic) {
        std::ostringstream transcript;
        for (const auto& m : convo) {
            if (m.role == "system") continue;
            transcript << "[" << m.role;
            if (!m.tool_name.empty()) transcript << ":" << m.tool_name;
            transcript << "] " << m.content << "\n";
            for (const auto& tc : m.tool_calls) {
                transcript << "  <tool_call " << tc.name << " args="
                           << tc.arguments << ">\n";
            }
        }

        std::vector<ChatMessage> compress_msgs;
        {
            ChatMessage s; s.role = "system"; s.content = COMPRESS_SYSTEM;
            compress_msgs.push_back(std::move(s));
            ChatMessage u; u.role = "user";
            u.content = "Topic: " + topic + "\n\nRaw transcript:\n"
                      + transcript.str();
            compress_msgs.push_back(std::move(u));
        }

        CompletionParams cp;
        cp.model = model_;
        cp.messages = std::move(compress_msgs);
        cp.temperature = 0.2f;
        cp.max_tokens = 2048;

        try {
            auto completion = provider_->complete(cp);
            std::string out = completion.message.content;
            // Hard cap regardless of what the model produced. Protects the
            // supervisor's accumulated context from unbounded growth across
            // research rounds — each round appends one tool_result per
            // researcher to supervisor_messages.
            constexpr size_t kMaxCompressed = 1000;
            if (out.size() > kMaxCompressed) {
                out.resize(kMaxCompressed);
                out += "\n…(truncated)";
            }
            return out;
        } catch (const std::exception& e) {
            return std::string("(compression failed: ") + e.what() + ")";
        }
    }

    std::vector<ChannelWrite> fanin_writes(const std::string& call_id,
                                           const std::string& topic,
                                           const std::string& summary) {
        // raw_notes entry
        json note = json::object();
        note["topic"] = topic;
        note["call_id"] = call_id;
        note["summary"] = summary;

        // supervisor_messages entry — paired tool-result
        ChatMessage tr = tool_result_msg(call_id, "conduct_research", summary);
        json tr_json; to_json(tr_json, tr);

        return {
            ChannelWrite{"raw_notes",           json::array({note})},
            ChannelWrite{"supervisor_messages", json::array({tr_json})}
        };
    }

    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::vector<Tool*>        tools_;
    std::string               model_;
    int                       max_iter_;
};

// =========================================================================
// FinalReportNode — reads `research_brief` + `raw_notes` and produces
// `final_report`.
//
// Token-limit retry loop (mirrors open_deep_research's
// final_report_generation): on context-length-exceeded errors from the
// provider, progressively truncate the findings text by 25% and retry
// up to MAX_RETRIES times. Without this, a successful research run
// with many lengthy researcher summaries would fail the synthesis
// stage and leave the user with no report at all — the worst possible
// outcome since the expensive work has already been done.
// =========================================================================
class FinalReportNode : public GraphNode {
public:
    FinalReportNode(std::string name, std::shared_ptr<Provider> provider,
                    std::string model)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model)) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        std::string brief;
        {
            auto b = state.get("research_brief");
            if (b.is_string()) brief = b.get<std::string>();
        }

        std::ostringstream findings;
        auto notes = state.get("raw_notes");
        if (notes.is_array()) {
            int idx = 1;
            for (auto it = notes.begin(); it != notes.end(); ++it, ++idx) {
                auto n = *it;
                std::string topic = n.is_object() && n.contains("topic")
                    && n["topic"].is_string() ? n["topic"].get<std::string>() : "";
                std::string summary = n.is_object() && n.contains("summary")
                    && n["summary"].is_string() ? n["summary"].get<std::string>() : "";
                findings << "### Finding " << idx << " — " << topic << "\n"
                         << summary << "\n\n";
            }
        }

        std::string findings_text = findings.str();
        if (findings_text.empty()) {
            return {
                ChannelWrite{"final_report", json(std::string(
                    "# Research Report\n\n"
                    "No researcher findings were collected. "
                    "The supervisor terminated without dispatching any "
                    "`conduct_research` calls."))}
            };
        }

        // Retry loop on token-limit / context-length errors. Each retry
        // truncates findings_text by 25% (keeping the prefix — earlier
        // findings come from earlier supervisor rounds and are usually
        // higher-priority since the supervisor decides what to research
        // first).
        constexpr int MAX_RETRIES = 3;
        std::string last_error;
        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            std::vector<ChatMessage> convo;
            {
                ChatMessage s; s.role = "system"; s.content = FINAL_REPORT_SYSTEM;
                convo.push_back(std::move(s));
                ChatMessage u; u.role = "user";
                u.content = "## Research brief\n" + brief +
                            "\n\n## Collected findings\n" + findings_text;
                convo.push_back(std::move(u));
            }

            CompletionParams params;
            params.model = model_;
            params.messages = std::move(convo);
            params.temperature = 0.4f;
            params.max_tokens = 4096;

            try {
                auto completion = provider_->complete(params);
                return {
                    ChannelWrite{"final_report", json(completion.message.content)}
                };
            } catch (const std::exception& e) {
                last_error = e.what();
                std::string lc = last_error;
                std::transform(lc.begin(), lc.end(), lc.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                bool is_context_overflow =
                    lc.find("context") != std::string::npos
                    || lc.find("token") != std::string::npos
                    || lc.find("length") != std::string::npos
                    || lc.find("too long") != std::string::npos
                    || lc.find("max_tokens") != std::string::npos;
                if (!is_context_overflow || attempt == MAX_RETRIES - 1) {
                    throw;  // not a token-limit issue, or out of retries
                }
                // Truncate findings by 25% and retry. Keep at least 1 KB.
                size_t new_size = std::max<size_t>(
                    1024, findings_text.size() * 3 / 4);
                if (new_size >= findings_text.size()) {
                    throw;  // can't truncate further
                }
                findings_text.resize(new_size);
                findings_text += "\n\n[... truncated to fit context window]\n";
            }
        }
        // Unreachable, but the compiler can't prove it.
        return {
            ChannelWrite{"final_report", json(
                std::string("# Research Report\n\nFinal-report synthesis "
                            "failed after retries: ") + last_error)}
        };
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::string               model_;
};

// =========================================================================
// BriefNode — LLM-driven research-brief synthesis. Rewrites the raw
// user_query into a focused, structured brief that the supervisor's
// planner can decompose more reliably. Mirrors LangGraph's
// `transform_messages_into_research_topic_prompt` step in
// open_deep_research/deep_researcher.py — without it the supervisor
// has to interpret colloquial user phrasing and tends to over-broaden
// the search.
//
// Falls back to a verbatim pass-through if the LLM call fails so a
// transient provider error doesn't sink the whole run.
// =========================================================================
class BriefNode : public GraphNode {
public:
    BriefNode(std::string name, std::shared_ptr<Provider> provider, std::string model)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model)) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto q = state.get("user_query");
        std::string user_query = q.is_string() ? q.get<std::string>() : "";

        if (user_query.empty()) {
            return {ChannelWrite{"research_brief", json(std::string{})}};
        }

        const char* BRIEF_SYSTEM = R"(You convert a user's research question into a focused research brief.

The brief must:
1. Restate the core question precisely, preserving every constraint the user named (dates, scope, comparators).
2. List the specific sub-questions a researcher should answer to produce a complete report. Aim for 3-6 sub-questions.
3. State what is OUT of scope so researchers don't go on tangents.
4. Be self-contained — no references to "the user" or to context the supervisor doesn't have.

Output ONLY the brief, in plain markdown. No preamble. Keep it under 200 words.)";

        try {
            std::vector<ChatMessage> convo;
            ChatMessage s; s.role = "system"; s.content = BRIEF_SYSTEM;
            convo.push_back(std::move(s));
            ChatMessage u; u.role = "user";
            u.content = "User research question:\n" + user_query;
            convo.push_back(std::move(u));

            CompletionParams params;
            params.model = model_;
            params.messages = std::move(convo);
            params.temperature = 0.2f;
            params.max_tokens = 800;

            auto completion = provider_->complete(params);
            std::string brief = completion.message.content;
            if (brief.empty()) brief = user_query;  // pass-through guard
            return {ChannelWrite{"research_brief", json(std::move(brief))}};
        } catch (const std::exception&) {
            // Provider failure → degrade gracefully to pass-through so
            // the rest of the graph still gets a (degraded) brief.
            return {ChannelWrite{"research_brief", json(user_query)}};
        }
    }
private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::string               model_;
};

// =========================================================================
// ClarifyNode — optional HITL gate between __start__ and brief.
//
// Mirrors langchain-ai/open_deep_research's `clarify_with_user` step:
// before the supervisor commits to a research plan, an LLM judges
// whether the user_query is specific enough to research. If yes
// (DECISION: PROCEED), it's a no-op pass-through. If no
// (DECISION: ASK <question>), it throws NodeInterrupt with the
// clarifying question; the caller is expected to resume() with the
// user's answer, which gets appended to user_query for the next pass.
//
// Two-phase like HumanReviewNode: same node body called twice (pause
// then resume) to keep the routing simple.
// =========================================================================
class ClarifyNode : public GraphNode {
public:
    ClarifyNode(std::string name, std::shared_ptr<Provider> provider, std::string model)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model)) {}

    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }

    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        co_return execute_full(state);
    }

    NodeResult execute_full(const GraphState& state) override {
        std::string query;
        {
            auto q = state.get("user_query");
            if (q.is_string()) query = q.get<std::string>();
        }

        // Phase 2: messages channel populated by engine.resume() with
        // the user's clarification reply. Append it to user_query and
        // continue.
        auto msgs = state.get("messages");
        if (msgs.is_array() && msgs.size() > 0) {
            auto latest = msgs[msgs.size() - 1];
            std::string answer;
            if (latest.is_object() && latest.contains("content")
                && latest["content"].is_string()) {
                answer = latest["content"].get<std::string>();
            }
            std::string augmented = query;
            if (!answer.empty()) {
                augmented += "\n\n[user clarification]\n" + answer;
            }
            NodeResult r;
            r.writes.push_back(ChannelWrite{"user_query", json(augmented)});
            // Clear the messages channel so a future interrupt starts fresh.
            r.writes.push_back(ChannelWrite{"messages", json::array()});
            return r;
        }

        // Phase 1: ask the LLM whether the query needs clarification.
        const char* CLARIFY_SYSTEM = R"(You decide whether a user's research request is specific enough to investigate.

If the request is concrete and self-contained, output exactly:
  DECISION: PROCEED

If a critical detail is missing (scope, time period, comparison target, definition of an ambiguous term), output exactly:
  DECISION: ASK
  QUESTION: <one short clarifying question>

Bias toward PROCEED — only ASK when the question would clearly fork the research direction. Do not ask cosmetic preference questions.)";

        std::string verdict;
        try {
            std::vector<ChatMessage> convo;
            ChatMessage s; s.role = "system"; s.content = CLARIFY_SYSTEM;
            convo.push_back(std::move(s));
            ChatMessage u; u.role = "user"; u.content = "Request:\n" + query;
            convo.push_back(std::move(u));

            CompletionParams params;
            params.model = model_;
            params.messages = std::move(convo);
            params.temperature = 0.0f;
            params.max_tokens = 200;

            verdict = provider_->complete(params).message.content;
        } catch (const std::exception&) {
            // Provider failure — degrade to PROCEED so the run isn't sunk.
            return NodeResult{};
        }

        // Parse the decision. Tolerant — accept "DECISION: PROCEED" anywhere
        // in the reply.
        std::string lc = verdict;
        std::transform(lc.begin(), lc.end(), lc.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (lc.find("proceed") != std::string::npos
            && lc.find("ask") == std::string::npos) {
            return NodeResult{};
        }

        // Extract the clarifying question (line starting with "QUESTION:").
        std::string question;
        {
            auto pos = verdict.find("QUESTION:");
            if (pos == std::string::npos) pos = verdict.find("question:");
            if (pos != std::string::npos) {
                pos += 9;
                while (pos < verdict.size()
                       && (verdict[pos] == ' ' || verdict[pos] == '\t'))
                    ++pos;
                auto end = verdict.find('\n', pos);
                question = verdict.substr(
                    pos, end == std::string::npos ? std::string::npos : end - pos);
            }
        }
        if (question.empty()) {
            question = "Could you clarify the scope of your research request?";
        }
        throw NodeInterrupt(
            "Clarification needed before research begins.\n\n"
            "QUESTION: " + question +
            "\n\nResume with the user's answer to continue.");
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::string               model_;
};

// =========================================================================
// HumanReviewNode — HITL gate after final_report.
//
// Two-phase behavior driven by the `messages` channel:
//
//   Phase 1 (fresh execution, messages empty):
//     Throws NodeInterrupt. The engine catches it, saves a Checkpoint at
//     phase NodeInterrupt with next_nodes=[this], and re-throws to the
//     caller. The caller is expected to surface the report to a human
//     and later call engine.resume(thread_id, feedback).
//
//   Phase 2 (resumed, messages contains the human's reply):
//     Engine.resume writes the resume_value into the `messages` channel
//     (overwrite reducer → single-element array). This node inspects
//     that message:
//       * empty / "approve" / "ok"  → Command(__end__), clears messages
//       * anything else             → treats it as feedback:
//           - Appends as user msg to `supervisor_messages`
//           - Resets `supervisor_iterations` so dispatch lets the
//             supervisor run another round
//           - Clears `messages` so a hypothetical next interrupt starts
//             fresh
//           - Command(supervisor) routes back into the planning loop
//
// Why both phases live in one class: the engine's interrupt model means
// the same node body is called twice — once to pause, once to resume.
// Splitting it would require a separate "decision" node, more edges,
// and a route channel the supervisor doesn't otherwise use.
// =========================================================================
class HumanReviewNode : public GraphNode {
public:
    explicit HumanReviewNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState&) override {
        // Should never be called — execute_full handles both phases and
        // emits a Command. Throwing here would mask a real issue, so
        // return empty writes; the engine will then route via the
        // (unused) `human_review → __end__` edge declared in the graph
        // definition. The Command path in execute_full is the intended
        // route.
        return {};
    }

    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        co_return execute_full(state);
    }

    NodeResult execute_full(const GraphState& state) override {
        auto msgs = state.get("messages");
        bool have_user_reply = msgs.is_array() && msgs.size() > 0;

        if (!have_user_reply) {
            // First execution. Pause for the human.
            std::string report;
            auto fr = state.get("final_report");
            if (fr.is_string()) report = fr.get<std::string>();
            throw NodeInterrupt(
                "Awaiting human review of the report. Resume with "
                "'approve' to finalize, or pass any other text as "
                "feedback to trigger another research round.\n\n"
                "--- REPORT ---\n" + report);
        }

        // Resumed. Inspect the latest message.
        auto latest = msgs[msgs.size() - 1];
        std::string content;
        if (latest.is_object() && latest.contains("content") &&
            latest["content"].is_string()) {
            content = latest["content"].get<std::string>();
        }
        std::string trimmed = content;
        // Trim ASCII whitespace; lower-case for the approve check.
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
            trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();
        std::string lower = trimmed;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return std::tolower(c); });

        NodeResult r;
        Command cmd;
        if (trimmed.empty() || lower == "approve" || lower == "ok" ||
            lower == "yes" || lower == "y") {
            cmd.goto_node = std::string(END_NODE);
            // Drain `messages` so a second resume cycle (if the user
            // re-resumes the same thread) starts from an empty channel
            // and the interrupt fires again instead of re-routing.
            cmd.updates.push_back({"messages", json::array()});
        } else {
            cmd.goto_node = "supervisor";
            json feedback_msg = json::object();
            feedback_msg["role"] = "user";
            feedback_msg["content"] =
                "[USER FOLLOW-UP after reviewing your previous report] " +
                content +
                "\n\nDispatch additional research if needed to address "
                "this, then call research_complete when done.";
            cmd.updates.push_back({"supervisor_messages",
                json::array({feedback_msg})});
            // Reset the supervisor budget so the dispatch node lets the
            // supervisor run another full round of conduct_research +
            // research_complete instead of short-circuiting on the
            // already-exhausted counter.
            cmd.updates.push_back({"supervisor_iterations", 0});
            cmd.updates.push_back({"messages", json::array()});
        }
        r.command = cmd;
        return r;
    }

private:
    std::string name_;
};

// =========================================================================
// Node registration (idempotent, thread-safe).
// =========================================================================
void register_node_types_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& nf = NodeFactory::instance();

        nf.register_type("__dr_clarify",
            [](const std::string& name, const json&, const NodeContext& ctx)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<ClarifyNode>(
                    name, ctx.provider, ctx.model);
            });

        nf.register_type("__dr_brief",
            [](const std::string& name, const json&, const NodeContext& ctx)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<BriefNode>(
                    name, ctx.provider, ctx.model);
            });

        nf.register_type("__dr_supervisor_llm",
            [](const std::string& name, const json&, const NodeContext& ctx)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<SupervisorLLMNode>(
                    name, ctx.provider, ctx.model);
            });

        nf.register_type("__dr_supervisor_dispatch",
            [](const std::string& name, const json& config, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                int max_iter = config.value("max_iterations", 4);
                int max_conc = config.value("max_concurrent", 3);
                return std::make_unique<SupervisorDispatchNode>(
                    name, max_iter, max_conc);
            });

        nf.register_type("__dr_researcher",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                int max_iter = config.value("max_iterations", 4);
                return std::make_unique<ResearcherNode>(
                    name, ctx.provider, ctx.tools, ctx.model, max_iter);
            });

        nf.register_type("__dr_final_report",
            [](const std::string& name, const json&, const NodeContext& ctx)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<FinalReportNode>(
                    name, ctx.provider, ctx.model);
            });

        nf.register_type("__dr_human_review",
            [](const std::string& name, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<HumanReviewNode>(name);
            });
    });
}

} // namespace

// =========================================================================
// Factory
// =========================================================================
std::unique_ptr<GraphEngine> create_deep_research_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    DeepResearchConfig cfg) {

    register_node_types_once();

    NodeContext ctx;
    ctx.provider = std::move(provider);
    ctx.model = cfg.model;
    for (auto& t : tools) ctx.tools.push_back(t.get());

    // Channels — common set always present.
    json channels = {
        {"user_query",             {{"reducer", "overwrite"}}},
        {"research_brief",         {{"reducer", "overwrite"}}},
        {"supervisor_messages",    {{"reducer", "append"}}},
        {"supervisor_iterations",  {{"reducer", "overwrite"}}},
        {"current_topic",          {{"reducer", "overwrite"}}},
        {"current_call_id",        {{"reducer", "overwrite"}}},
        {"raw_notes",              {{"reducer", "append"}}},
        {"final_report",           {{"reducer", "overwrite"}}}
    };

    // Nodes — common set always present.
    json nodes = {
        {"brief",        {{"type", "__dr_brief"}}},
        {"supervisor",   {{"type", "__dr_supervisor_llm"}}},
        {"dispatch",     {
            {"type", "__dr_supervisor_dispatch"},
            {"max_iterations", cfg.max_supervisor_iterations},
            {"max_concurrent", cfg.max_concurrent_researchers}
        }},
        {"researcher",   {
            {"type", "__dr_researcher"},
            {"max_iterations", cfg.max_researcher_iterations}
        }},
        {"final_report", {{"type", "__dr_final_report"}}}
    };

    // Edges — common set: start → brief → supervisor → dispatch loop;
    // dispatch emits Command(final_report) to short-circuit when done.
    // The clarify gate (when enabled) sits between __start__ and brief.
    json edges = json::array();
    if (cfg.enable_clarification) {
        // The `messages` channel is shared between clarify and human_review.
        // Both nodes consume the resume_value engine.resume() drops here.
        channels["messages"] = json{{"reducer", "overwrite"}};
        nodes["clarify"] = json{{"type", "__dr_clarify"}};
        edges.push_back({{"from", "__start__"}, {"to", "clarify"}});
        edges.push_back({{"from", "clarify"},   {"to", "brief"}});
    } else {
        edges.push_back({{"from", "__start__"}, {"to", "brief"}});
    }
    edges.push_back({{"from", "brief"},      {"to", "supervisor"}});
    edges.push_back({{"from", "supervisor"}, {"to", "dispatch"}});
    edges.push_back({{"from", "dispatch"},   {"to", "supervisor"}});

    if (cfg.enable_human_review) {
        // Add the `messages` channel that engine.resume writes the user's
        // resume_value into. Overwrite reducer keeps a single-element
        // array semantically meaning "latest human input pending review".
        channels["messages"] = json{{"reducer", "overwrite"}};
        nodes["human_review"] = json{{"type", "__dr_human_review"}};
        // Re-route final_report through the review gate. The terminal
        // edge from human_review is decorative — HumanReviewNode emits
        // a Command(__end__) or Command(supervisor) on every resume, so
        // the static edge is only consulted if the Command path ever
        // changes.
        edges.push_back({{"from", "final_report"}, {"to", "human_review"}});
        edges.push_back({{"from", "human_review"}, {"to", "__end__"}});
    } else {
        edges.push_back({{"from", "final_report"}, {"to", "__end__"}});
    }

    json definition = {
        {"name", "deep_research_agent"},
        {"channels", channels},
        {"nodes", nodes},
        {"edges", edges}
    };

    auto engine = GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));

    // Resilience against transient Anthropic 5xx and rate-limit (HTTP 429)
    // errors. Anthropic's minute-window rate limits usually clear within 60s;
    // two retries at up to 45s apart cover that. Applied to every LLM-calling
    // node. Brief/dispatch don't call the API but policy is harmless there.
    RetryPolicy llm_retry;
    llm_retry.max_retries       = 2;
    llm_retry.initial_delay_ms  = 15'000;
    llm_retry.backoff_multiplier = 2.0f;
    llm_retry.max_delay_ms      = 45'000;
    engine->set_node_retry_policy("supervisor",   llm_retry);
    engine->set_node_retry_policy("researcher",   llm_retry);
    engine->set_node_retry_policy("final_report", llm_retry);

    return engine;
}

} // namespace neograph::graph
