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

constexpr const char* SUPERVISOR_SYSTEM = R"(You are a lead research supervisor. Given a research brief, your job is to decompose it into focused sub-topics and dispatch parallel researchers.

You have three tools:
  - conduct_research(research_topic: string): dispatch a sub-researcher to investigate ONE focused topic. Call this multiple times in a single turn to fan out in parallel.
  - think_tool(reflection: string): pause and record your reasoning. Use this between research rounds to plan the next step.
  - research_complete(): stop researching. Call ONLY when you have enough material to write a comprehensive report.

Guidelines:
  * In the FIRST turn, issue 2–4 parallel `conduct_research` calls covering distinct angles of the brief.
  * Each `research_topic` must be a self-contained paragraph: include the question, the context, and what specifically to look for.
  * After researchers return, decide whether coverage is sufficient. If gaps remain, issue more `conduct_research` calls.
  * Call `research_complete` as soon as the brief is adequately covered. Do NOT over-research.
  * Total budget: a few rounds. Quality over quantity.
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

constexpr const char* COMPRESS_SYSTEM = R"(You are compressing raw research notes into a dense summary. Preserve every concrete fact, number, and URL citation. Drop only meta-commentary ("let me search for...", "I'll now..."). Output only the compressed summary as markdown prose.)";

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

        // Partition tool calls by intent.
        std::vector<ToolCall> research_calls;
        std::vector<ToolCall> think_calls;
        bool saw_complete = false;

        for (const auto& tc : last->tool_calls) {
            if (tc.name == "conduct_research") {
                research_calls.push_back(tc);
            } else if (tc.name == "think_tool") {
                think_calls.push_back(tc);
            } else if (tc.name == "research_complete") {
                saw_complete = true;
            }
            // Unknown tool names are silently dropped.
        }

        // research_complete wins (user-requested short-circuit).
        if (saw_complete) {
            // Tool-result echoes keep the Anthropic tool_use/tool_result
            // pairing valid in case the supervisor also chose other tools,
            // though we won't run them.
            json echo_msgs = json::array();
            for (const auto& tc : last->tool_calls) {
                auto m = tool_result_msg(tc.id, tc.name, "acknowledged");
                json j; to_json(j, m);
                echo_msgs.push_back(j);
            }
            nr.writes.push_back(
                ChannelWrite{"supervisor_messages", echo_msgs});
            nr.command = Command{"final_report", {}};
            return nr;
        }

        // Iteration cap: if we'd be entering another round, force finish.
        if (iter >= max_iter_) {
            json echo_msgs = json::array();
            for (const auto& tc : last->tool_calls) {
                auto m = tool_result_msg(tc.id, tc.name,
                    "iteration budget exhausted; terminating");
                json j; to_json(j, m);
                echo_msgs.push_back(j);
            }
            nr.writes.push_back(
                ChannelWrite{"supervisor_messages", echo_msgs});
            nr.command = Command{"final_report", {}};
            return nr;
        }

        // Cap fan-out width.
        if ((int)research_calls.size() > max_concurrent_) {
            research_calls.resize(max_concurrent_);
        }

        // 1. Echo think_tool results back into the supervisor conversation.
        //    (These ARE paired with the supervisor's tool_use ids.)
        // 2. For conduct_research calls: Send to researcher with the topic
        //    plus the corresponding tool_call_id so the researcher can
        //    write its compressed result back keyed by that id.
        // 3. We do NOT pre-emit tool-result messages for conduct_research
        //    here — the researcher fan-in writes them after it runs, so
        //    that the paired tool_result carries the actual findings.

        json sv_updates = json::array();
        for (const auto& tc : think_calls) {
            // Extract the reflection text for a useful echo; fall back to
            // "noted" if parsing fails.
            std::string content = "noted";
            try {
                auto args = json::parse(tc.arguments);
                if (args.contains("reflection") &&
                    args["reflection"].is_string()) {
                    content = "reflection logged: "
                        + args["reflection"].get<std::string>();
                }
            } catch (...) { /* keep fallback */ }
            auto m = tool_result_msg(tc.id, tc.name, content);
            json j; to_json(j, m);
            sv_updates.push_back(j);
        }

        // If the supervisor ONLY called think_tool (no research), loop back
        // to give it another chance to dispatch.
        if (research_calls.empty()) {
            if (!sv_updates.empty()) {
                nr.writes.push_back(
                    ChannelWrite{"supervisor_messages", sv_updates});
            }
            nr.command = Command{"supervisor", {}};
            return nr;
        }

        // Emit Sends for parallel research. The researcher node reads
        // `current_topic` + `current_call_id` on execute and writes back
        // `raw_notes` (appended) and `supervisor_messages` (appended
        // with the paired tool_result).
        for (const auto& tc : research_calls) {
            std::string topic;
            try {
                auto args = json::parse(tc.arguments);
                if (args.contains("research_topic") &&
                    args["research_topic"].is_string()) {
                    topic = args["research_topic"].get<std::string>();
                }
            } catch (...) { /* empty topic will cause researcher to skip */ }

            nr.sends.push_back(Send{"researcher", json{
                {"current_topic",    topic},
                {"current_call_id",  tc.id}
            }});
        }

        // Any think_tool echoes written now so the post-Send supervisor
        // round sees a consistent conversation.
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
            return completion.message.content;
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
// `final_report`. Single LLM call.
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

        if (findings.str().empty()) {
            // No researcher output at all — write a terse placeholder so
            // downstream consumers see *something*.
            return {
                ChannelWrite{"final_report", json(std::string(
                    "# Research Report\n\n"
                    "No researcher findings were collected. "
                    "The supervisor terminated without dispatching any "
                    "`conduct_research` calls."))}
            };
        }

        std::vector<ChatMessage> convo;
        {
            ChatMessage s; s.role = "system"; s.content = FINAL_REPORT_SYSTEM;
            convo.push_back(std::move(s));
            ChatMessage u; u.role = "user";
            u.content = "## Research brief\n" + brief +
                        "\n\n## Collected findings\n" + findings.str();
            convo.push_back(std::move(u));
        }

        CompletionParams params;
        params.model = model_;
        params.messages = std::move(convo);
        params.temperature = 0.4f;
        params.max_tokens = 4096;

        auto completion = provider_->complete(params);
        return {
            ChannelWrite{"final_report", json(completion.message.content)}
        };
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::string               model_;
};

// =========================================================================
// BriefNode — trivial pass-through for MVP: copies `user_query` into
// `research_brief`. A later iteration can promote this to an LLM call that
// rewrites the user's message into a structured brief.
// =========================================================================
class BriefNode : public GraphNode {
public:
    explicit BriefNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto q = state.get("user_query");
        std::string brief = q.is_string() ? q.get<std::string>() : "";
        return {ChannelWrite{"research_brief", json(brief)}};
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

        nf.register_type("__dr_brief",
            [](const std::string& name, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<BriefNode>(name);
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

    json definition = {
        {"name", "deep_research_agent"},
        {"channels", {
            {"user_query",             {{"reducer", "overwrite"}}},
            {"research_brief",         {{"reducer", "overwrite"}}},
            {"supervisor_messages",    {{"reducer", "append"}}},
            {"supervisor_iterations",  {{"reducer", "overwrite"}}},
            {"current_topic",          {{"reducer", "overwrite"}}},
            {"current_call_id",        {{"reducer", "overwrite"}}},
            {"raw_notes",              {{"reducer", "append"}}},
            {"final_report",           {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
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
        }},
        // Edges:
        //   start → brief → supervisor → dispatch
        //   dispatch Sends → researcher (parallel fan-out/fan-in)
        //   after fan-in completes, dispatch's outgoing edge fires → supervisor
        //   dispatch emits Command(final_report) to short-circuit when done
        //   final_report → end
        {"edges", json::array({
            {{"from", "__start__"},    {"to", "brief"}},
            {{"from", "brief"},        {"to", "supervisor"}},
            {{"from", "supervisor"},   {"to", "dispatch"}},
            {{"from", "dispatch"},     {"to", "supervisor"}},
            {{"from", "final_report"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));
    return engine;
}

} // namespace neograph::graph
