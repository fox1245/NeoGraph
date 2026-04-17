#include <neograph/graph/plan_execute_graph.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

namespace neograph::graph {
namespace {

// =========================================================================
// Helper: pull a JSON array of strings out of free-form LLM text.
// Accepts: bare JSON array, fenced ```json block, JSON inside prose,
// and a numbered/bulleted list fallback.
// =========================================================================
std::vector<std::string> extract_plan(const std::string& text) {
    auto try_parse_array = [](const std::string& candidate)
                               -> std::vector<std::string> {
        try {
            auto j = json::parse(candidate);
            if (!j.is_array()) return {};
            std::vector<std::string> out;
            for (auto it = j.begin(); it != j.end(); ++it) {
                auto v = *it;
                if (v.is_string()) out.push_back(v.get<std::string>());
            }
            return out;
        } catch (...) {
            return {};
        }
    };

    auto plan = try_parse_array(text);
    if (!plan.empty()) return plan;

    const std::string fence = "```";
    auto fo = text.find(fence);
    if (fo != std::string::npos) {
        auto body_start = text.find('\n', fo);
        if (body_start != std::string::npos) {
            auto fc = text.find(fence, body_start);
            if (fc != std::string::npos) {
                plan = try_parse_array(
                    text.substr(body_start + 1, fc - body_start - 1));
                if (!plan.empty()) return plan;
            }
        }
    }

    auto l = text.find('[');
    auto r = text.rfind(']');
    if (l != std::string::npos && r != std::string::npos && r > l) {
        plan = try_parse_array(text.substr(l, r - l + 1));
        if (!plan.empty()) return plan;
    }

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos) continue;
        size_t i = pos;
        bool accepted = false;
        if (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            while (i < line.size() &&
                   std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
            if (i < line.size() && (line[i] == '.' || line[i] == ')')) {
                ++i; accepted = true;
            }
        } else if (i < line.size() && (line[i] == '-' || line[i] == '*')) {
            ++i; accepted = true;
        }
        if (!accepted) continue;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i < line.size()) plan.push_back(line.substr(i));
    }
    return plan;
}

// Shared helper: decide whether to go to executor or responder based on
// the current plan length. Command-based routing sidesteps the BSP
// predecessor checks that would otherwise deadlock a self-loop over
// conditional edges.
Command next_after_step(size_t remaining_plan) {
    Command c;
    c.goto_node = remaining_plan > 0 ? "executor" : "responder";
    return c;
}

// =========================================================================
// PlannerNode — one LLM call, parse list, emit Command to next phase.
// =========================================================================
class PlannerNode : public GraphNode {
public:
    PlannerNode(std::string name, std::shared_ptr<Provider> provider,
                std::string model, std::string prompt)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model))
        , prompt_(std::move(prompt)) {}

    std::string name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& /*state*/) override {
        return {};  // actual work lives in execute_full
    }

    NodeResult execute_full(const GraphState& state) override {
        auto msgs = state.get_messages();
        std::string objective;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == "user") { objective = it->content; break; }
        }

        std::vector<ChatMessage> prompt_msgs;
        if (!prompt_.empty()) {
            ChatMessage s; s.role = "system"; s.content = prompt_;
            prompt_msgs.push_back(std::move(s));
        }
        for (auto& m : msgs) {
            if (m.role == "system") continue;
            prompt_msgs.push_back(m);
        }

        CompletionParams params;
        params.model = model_;
        params.messages = std::move(prompt_msgs);

        auto completion = provider_->complete(params);
        auto plan_items = extract_plan(completion.message.content);

        json plan_json = json::array();
        for (auto& s : plan_items) plan_json.push_back(json(s));

        NodeResult nr;
        nr.writes.push_back({"objective", json(objective)});
        nr.writes.push_back({"plan", plan_json});
        nr.command = next_after_step(plan_items.size());
        return nr;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::string model_;
    std::string prompt_;
};

// =========================================================================
// ExecutorNode — pops one step off plan, runs an inner ReAct loop, emits
// Command to continue (to self) or finalise (to responder).
// =========================================================================
class ExecutorNode : public GraphNode {
public:
    ExecutorNode(std::string name, std::shared_ptr<Provider> provider,
                 std::vector<Tool*> tools, std::string model,
                 std::string prompt, int max_iter)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , tools_(std::move(tools))
        , model_(std::move(model))
        , prompt_(std::move(prompt))
        , max_iter_(max_iter) {}

    std::string name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {};
    }

    NodeResult execute_full(const GraphState& state) override {
        auto plan = state.get("plan");
        if (!plan.is_array() || plan.size() == 0) {
            NodeResult nr;
            nr.command = next_after_step(0);  // responder
            return nr;
        }

        std::string step;
        {
            auto first = plan[size_t{0}];
            if (first.is_string()) step = first.get<std::string>();
        }

        std::vector<ChatMessage> convo;
        if (!prompt_.empty()) {
            ChatMessage s; s.role = "system"; s.content = prompt_;
            convo.push_back(std::move(s));
        }
        {
            ChatMessage u; u.role = "user"; u.content = step;
            convo.push_back(std::move(u));
        }

        std::vector<ChatTool> tool_defs;
        tool_defs.reserve(tools_.size());
        for (auto* t : tools_) tool_defs.push_back(t->get_definition());

        std::string result_text;
        for (int iter = 0; iter < max_iter_; ++iter) {
            CompletionParams params;
            params.model = model_;
            params.messages = convo;
            params.tools = tool_defs;

            auto completion = provider_->complete(params);
            auto& msg = completion.message;
            convo.push_back(msg);

            if (msg.tool_calls.empty()) {
                result_text = msg.content;
                break;
            }

            for (const auto& tc : msg.tool_calls) {
                auto it = std::find_if(tools_.begin(), tools_.end(),
                    [&](Tool* t) { return t->get_name() == tc.name; });
                ChatMessage tm;
                tm.role = "tool";
                tm.tool_call_id = tc.id;
                tm.tool_name = tc.name;
                if (it == tools_.end()) {
                    tm.content = R"({"error":"Tool not found: )" + tc.name + "\"}";
                } else {
                    try {
                        auto args = json::parse(tc.arguments);
                        tm.content = (*it)->execute(args);
                    } catch (const std::exception& e) {
                        tm.content = std::string(R"({"error":")") + e.what() + "\"}";
                    }
                }
                convo.push_back(std::move(tm));
            }
        }

        json new_plan = json::array();
        for (size_t i = 1; i < plan.size(); ++i) new_plan.push_back(plan[i]);

        json step_record = json::object();
        step_record["step"] = step;
        step_record["result"] = result_text;

        NodeResult nr;
        nr.writes.push_back({"plan", new_plan});
        nr.writes.push_back({"past_steps", json::array({step_record})});
        nr.command = next_after_step(new_plan.size());
        return nr;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::vector<Tool*> tools_;
    std::string model_;
    std::string prompt_;
    int max_iter_;
};

// =========================================================================
// ResponderNode — synthesise final answer from objective + past_steps.
// =========================================================================
class ResponderNode : public GraphNode {
public:
    ResponderNode(std::string name, std::shared_ptr<Provider> provider,
                  std::string model, std::string prompt)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model))
        , prompt_(std::move(prompt)) {}

    std::string name() const override { return name_; }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        std::string objective;
        auto obj = state.get("objective");
        if (obj.is_string()) objective = obj.get<std::string>();

        std::ostringstream steps_text;
        auto past = state.get("past_steps");
        if (past.is_array()) {
            for (auto it = past.begin(); it != past.end(); ++it) {
                auto rec = *it;
                std::string s = rec.is_object() && rec.contains("step") &&
                                        rec["step"].is_string()
                                    ? rec["step"].get<std::string>()
                                    : "";
                std::string r = rec.is_object() && rec.contains("result") &&
                                        rec["result"].is_string()
                                    ? rec["result"].get<std::string>()
                                    : "";
                steps_text << "- " << s << "\n  -> " << r << "\n";
            }
        }

        std::vector<ChatMessage> convo;
        if (!prompt_.empty()) {
            ChatMessage s; s.role = "system"; s.content = prompt_;
            convo.push_back(std::move(s));
        }
        {
            ChatMessage u; u.role = "user";
            u.content = "Objective:\n" + objective +
                        "\n\nCompleted steps:\n" + steps_text.str() +
                        "\nProduce the final answer for the user.";
            convo.push_back(std::move(u));
        }

        CompletionParams params;
        params.model = model_;
        params.messages = std::move(convo);

        auto completion = provider_->complete(params);

        json asst_json;
        to_json(asst_json, completion.message);

        std::vector<ChannelWrite> writes;
        writes.push_back({"final_response", json(completion.message.content)});
        writes.push_back({"messages", json::array({asst_json})});
        return writes;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::string model_;
    std::string prompt_;
};

// =========================================================================
// One-time registration of the three custom node types.
// =========================================================================
void ensure_registrations_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        NodeFactory::instance().register_type("__pe_planner",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                return std::make_unique<PlannerNode>(
                    name, ctx.provider, ctx.model,
                    config.value("prompt", std::string{}));
            });

        NodeFactory::instance().register_type("__pe_executor",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                return std::make_unique<ExecutorNode>(
                    name, ctx.provider, ctx.tools, ctx.model,
                    config.value("prompt", std::string{}),
                    config.value("max_iter", 5));
            });

        NodeFactory::instance().register_type("__pe_responder",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                return std::make_unique<ResponderNode>(
                    name, ctx.provider, ctx.model,
                    config.value("prompt", std::string{}));
            });
    });
}

} // namespace

std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model,
    int max_step_iterations) {

    ensure_registrations_once();

    // Only the __start__ → planner and responder → __end__ edges are
    // static; planner and executor drive the loop via Command routing so
    // that the BSP predecessor check doesn't deadlock on self-loops.
    json definition = {
        {"name", "plan_execute_agent"},
        {"channels", {
            {"messages",       {{"reducer", "append"}}},
            {"plan",           {{"reducer", "overwrite"}}},
            {"past_steps",     {{"reducer", "append"}}},
            {"objective",      {{"reducer", "overwrite"}}},
            {"final_response", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",   {{"type", "__pe_planner"},   {"prompt", planner_prompt}}},
            {"executor",  {{"type", "__pe_executor"},
                           {"prompt", executor_prompt},
                           {"max_iter", max_step_iterations}}},
            {"responder", {{"type", "__pe_responder"}, {"prompt", responder_prompt}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "responder"}, {"to", "__end__"}}
        })}
    };

    std::vector<Tool*> tool_ptrs;
    tool_ptrs.reserve(tools.size());
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    NodeContext ctx;
    ctx.provider     = std::move(provider);
    ctx.tools        = std::move(tool_ptrs);
    ctx.model        = model;
    ctx.instructions = std::string{};

    auto engine = GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));
    return engine;
}

} // namespace neograph::graph
