#include <neograph/graph/plan_execute_graph.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/async/run_sync.h>

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

// =========================================================================
// PlannerNode — one LLM call, parse list. Routes via the `plan_empty`
// condition on outgoing conditional edges (see factory below).
// =========================================================================
class PlannerNode : public GraphNode, public ::neograph::RuntimeInterpositionConsumer {
public:
    PlannerNode(std::string name, std::shared_ptr<Provider> provider,
                std::string model, std::string prompt, ProviderControls controls)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model))
        , prompt_(std::move(prompt)), controls_(std::move(controls)) {}

    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto msgs = in.state.get_provider_messages();
        std::string objective;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == sp::Role::User) { objective = project_message(*it).content; break; }
        }

        std::vector<sp::Message> prompt_msgs;
        if (!prompt_.empty()) prompt_msgs.push_back(portable_message(ChatMessage{"system", prompt_}));
        for (const auto& message : msgs)
            if (message.role != sp::Role::System) prompt_msgs.push_back(message);
        auto request = make_provider_request(*provider_, model_, prompt_msgs, {}, controls_);
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        request.mode = in.ctx.on_provider_event ? ProviderMode::Stream : ProviderMode::Collect;
        request.on_event = in.ctx.on_provider_event;
        std::vector<sp::Message> host;
        if (!prompt_.empty()) host.push_back(portable_message(ChatMessage{"system", prompt_}));
        std::vector<sp::Message> supplemental(prompt_msgs.begin() + (prompt_.empty() ? 0 : 1), prompt_msgs.end());
        auto completion = co_await observe_provider_result(in.ctx, invoke_provider(provider_, std::move(request), std::move(host), std::move(supplemental),
            provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, completion);
        outcome_or_throw(completion);
        auto plan_items = extract_plan(outcome_text(*completion));

        json plan_json = json::array();
        for (auto& s : plan_items) plan_json.push_back(json(s));

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"objective", json(objective)});
        out.writes.push_back(ChannelWrite{"plan", plan_json});
        co_return out;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::string model_;
    std::string prompt_;
    ProviderControls controls_;
};

// =========================================================================
// ExecutorNode — pops one step off plan, runs an inner ReAct loop, emits
// Command to continue (to self) or finalise (to responder).
// =========================================================================
class ExecutorNode : public GraphNode, public ::neograph::RuntimeInterpositionConsumer {
public:
    ExecutorNode(std::string name, std::shared_ptr<Provider> provider,
                 std::vector<Tool*> tools, std::string model,
                 std::string prompt, int max_iter, ProviderControls controls)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , tools_(std::move(tools))
        , model_(std::move(model))
        , prompt_(std::move(prompt))
        , max_iter_(max_iter), controls_(std::move(controls)) {}

    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto plan = in.state.get("plan");
        if (!plan.is_array() || plan.size() == 0) co_return NodeOutput{};

        std::string step;
        {
            auto first = plan[size_t{0}];
            if (first.is_string()) step = first.get<std::string>();
        }

        const auto task = make_tool_execution_context(in.ctx).effect_task_id + ":" + name_;
        auto histories = in.ctx.provider_loop_history ? in.ctx.provider_loop_history : std::make_shared<ProviderLoopHistory>();
        auto continuation = histories->get(task);
        auto& convo = continuation.messages;
        if (convo.empty()) {
            if (!prompt_.empty()) convo.push_back(portable_message(ChatMessage{"system", prompt_}));
            convo.push_back(portable_message(ChatMessage{"user", step}));
            histories->set(task, continuation);
        }

        std::vector<ChatTool> tool_defs;
        tool_defs.reserve(tools_.size());
        for (auto* t : tools_) tool_defs.push_back(t->get_definition());

        std::string result_text;
        for (;;) {
            auto calls = continuation.client_calls_ready ? pending_client_tool_calls(convo) : std::vector<ToolCall>{};
            if (calls.empty()) {
                if (continuation.turns >= static_cast<std::uint64_t>(std::max(0, max_iter_))) break;
                auto request = make_provider_request(*provider_, model_, convo, tool_defs, controls_);
                request.cancel_token = in.ctx.cancel_token;
                request.options.deadline = in.ctx.deadline;
                request.mode = in.ctx.on_provider_event ? ProviderMode::Stream : ProviderMode::Collect;
                request.on_event = in.ctx.on_provider_event;
                std::vector<sp::Message> host;
                if (!prompt_.empty()) host.push_back(portable_message(ChatMessage{"system", prompt_}));
                std::vector<sp::Message> supplemental(convo.begin() + (prompt_.empty() ? 0 : 1), convo.end());
                auto completion = co_await observe_provider_result(in.ctx, invoke_provider(provider_, std::move(request), std::move(host), std::move(supplemental),
                    provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_, continuation.turns)));
                record_usage(in.ctx, completion);
                const auto& returned = outcome_messages(*completion);
                convo.insert(convo.end(), returned.begin(), returned.end());
                ++continuation.turns;
                continuation.client_calls_ready = std::holds_alternative<sp::Completion>(*completion);
                histories->set(task, continuation);
                outcome_or_throw(completion);
                calls = pending_client_tool_calls(returned);
                if (calls.empty()) {
                    if (paused_turn(*completion)) continue;  // resend the history, paused turn included
                    result_text = outcome_text(*completion);
                    break;
                }
            }
            ToolGateContext gate;
            gate.resume_value = in.ctx.resume_value;
            gate.thread_id = in.ctx.thread_id;
            gate.step = in.ctx.step;
            auto execution = make_tool_execution_context(in.ctx);
            execution.effect_task_id += ":turn:" + std::to_string(continuation.turns);
            auto results = co_await dispatch_tool_calls(std::move(calls), tools_, in.ctx.tool_gate,
                                                        std::move(gate), std::move(execution));
            std::vector<sp::Message> returned;
            for (const auto& result : results) returned.push_back(portable_message(result));
            convo.insert(convo.end(), returned.begin(), returned.end());
            continuation.client_calls_ready = false;
            histories->set(task, continuation);
        }

        json new_plan = json::array();
        for (size_t i = 1; i < plan.size(); ++i) new_plan.push_back(plan[i]);

        json step_record = json::object();
        step_record["step"] = step;
        step_record["result"] = result_text;

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"plan", new_plan});
        out.writes.push_back(ChannelWrite{"past_steps", json::array({step_record})});
        co_return out;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::vector<Tool*> tools_;
    std::string model_;
    std::string prompt_;
    int max_iter_;
    ProviderControls controls_;
};

// =========================================================================
// ResponderNode — synthesise final answer from objective + past_steps.
// =========================================================================
class ResponderNode : public GraphNode, public ::neograph::RuntimeInterpositionConsumer {
public:
    ResponderNode(std::string name, std::shared_ptr<Provider> provider,
                  std::string model, std::string prompt, ProviderControls controls)
        : name_(std::move(name))
        , provider_(std::move(provider))
        , model_(std::move(model))
        , prompt_(std::move(prompt)), controls_(std::move(controls)) {}

    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string objective;
        auto obj = in.state.get("objective");
        if (obj.is_string()) objective = obj.get<std::string>();

        std::ostringstream steps_text;
        auto past = in.state.get("past_steps");
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

        std::vector<sp::Message> convo;
        if (!prompt_.empty()) convo.push_back(portable_message(ChatMessage{"system", prompt_}));
        convo.push_back(portable_message(ChatMessage{"user", "Objective:\n" + objective +
            "\n\nCompleted steps:\n" + steps_text.str() + "\nProduce the final answer for the user."}));
        auto request = make_provider_request(*provider_, model_, convo, {}, controls_);
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        request.mode = in.ctx.on_provider_event ? ProviderMode::Stream : ProviderMode::Collect;
        request.on_event = in.ctx.on_provider_event;
        std::vector<sp::Message> host;
        if (!prompt_.empty()) host.push_back(portable_message(ChatMessage{"system", prompt_}));
        std::vector<sp::Message> supplemental(convo.begin() + (prompt_.empty() ? 0 : 1), convo.end());
        auto completion = co_await observe_provider_result(in.ctx, invoke_provider(provider_, std::move(request), std::move(host), std::move(supplemental),
            provider_call_broker(in.ctx), make_provider_call_identity(in.ctx, name_)));
        record_usage(in.ctx, completion);
        outcome_or_throw(completion);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"final_response", json(outcome_text(*completion))});
        out.writes.push_back(provider_messages_write(completion));
        co_return out;
    }

private:
    std::string name_;
    std::shared_ptr<Provider> provider_;
    std::string model_;
    std::string prompt_;
    ProviderControls controls_;
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
                    config.value("prompt", std::string{}), ctx.provider_controls);
            });

        NodeFactory::instance().register_type("__pe_executor",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                return std::make_unique<ExecutorNode>(
                    name, ctx.provider, ctx.tools.view(), ctx.model,
                    config.value("prompt", std::string{}),
                    config.value("max_iter", 5), ctx.provider_controls);
            });

        NodeFactory::instance().register_type("__pe_responder",
            [](const std::string& name, const json& config,
               const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
                return std::make_unique<ResponderNode>(
                    name, ctx.provider, ctx.model,
                    config.value("prompt", std::string{}), ctx.provider_controls);
            });

        ConditionRegistry::instance().register_condition("plan_empty",
            [](const GraphState& state) -> std::string {
                auto plan = state.get("plan");
                if (!plan.is_array() || plan.size() == 0) return "true";
                return "false";
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
    int max_step_iterations, ProviderControls controls) {

    ensure_registrations_once();

    // Signal-based dispatch (no static predecessor map) lets us express
    // the loop with plain conditional edges — including the executor's
    // self-referential "keep going" branch.
    json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
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
            {{"from", "planner"},  {"type", "conditional"},
             {"condition", "plan_empty"},
             {"routes", {{"true", "responder"}, {"false", "executor"}}}},
            {{"from", "executor"}, {"type", "conditional"},
             {"condition", "plan_empty"},
             {"routes", {{"true", "responder"}, {"false", "executor"}}}},
            {{"from", "responder"}, {"to", "__end__"}}
        })}
    };

    // Factories borrow pointers only from this owned collection.

    NodeContext ctx;
    ctx.provider     = std::move(provider);
    ctx.tools        = ToolSet(std::move(tools));
    ctx.model        = model;
    ctx.instructions = std::string{};
    ctx.provider_controls = std::move(controls);

    auto engine = GraphEngine::compile(definition, ctx);
    return engine;
}

} // namespace neograph::graph
