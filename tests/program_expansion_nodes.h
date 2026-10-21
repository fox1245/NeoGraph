// Shared fixture of the task graph expansion suites: deterministic child Programs, a registry
// that holds them, a fragment store that logs every task transition it makes durable, and a
// harness that runs a proposal through ExpandTaskGraph.
#pragma once

#include "program_expansion_fixture.h"

#include <neograph/program/step_plan_repair.h>

#include "canonical_json.h"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace neograph::program_test::expansion {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

// ---- Child nodes: every task is a child Program over one deterministic Core node. ----

inline std::mutex               node_log_mutex;
inline std::vector<std::string> node_log;
inline std::atomic<int>         gate_active{0};
inline std::atomic<int>         gate_peak{0};
inline std::atomic<int>         gate_arrived{0};
inline std::atomic<int>         gate_target{1};
inline std::atomic<int>         planner_calls{0};
inline std::mutex               planner_mutex;
inline std::vector<std::string> planner_replies;       // by attempt number
inline std::vector<std::uint64_t> planner_tokens;      // tokens to charge for each reply
inline std::vector<json>        planner_seen;          // the messages each call saw
inline std::atomic<int>         flaky_failures_left{0};
inline std::atomic<int>         flaky_calls{0};
inline std::atomic<int>         logic_calls{0};

inline void log_node(std::string entry) {
    std::lock_guard lock(node_log_mutex);
    node_log.push_back(std::move(entry));
}

inline std::vector<std::string> node_log_copy() {
    std::lock_guard lock(node_log_mutex);
    return node_log;
}

class EchoNode final : public GraphNode {
public:
    explicit EchoNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        log_node("echo:start");
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        log_node("echo:end");
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class SlowNode final : public GraphNode {
public:
    explicit SlowNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        log_node("slow:start");
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(150));
        asio::error_code ended;
        co_await timer.async_wait(asio::bind_cancellation_slot(
            input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
        if (ended) throw asio::system_error(ended);
        log_node("slow:end");
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class FailNode final : public GraphNode {
public:
    explicit FailNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        log_node("fail:start");
        throw std::runtime_error("scripted task failure");
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Counts how many gate nodes are inside at once. A gate waits, up to a bound, until
// `gate_target` gates have arrived, so overlap is observed and not left to chance.
class GateNode final : public GraphNode {
public:
    explicit GateNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto active = gate_active.fetch_add(1) + 1;
        gate_arrived.fetch_add(1);
        auto       peak   = gate_peak.load();
        while (peak < active && !gate_peak.compare_exchange_weak(peak, active)) {}
        for (int waited = 0; waited < 150 && gate_arrived.load() < gate_target.load(); ++waited) {
            auto timer = asio::steady_timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(10));
            asio::error_code ended;
            co_await timer.async_wait(asio::bind_cancellation_slot(
                input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
            if (ended) break;
        }
        gate_active.fetch_sub(1);
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Writes both inputs, so a test can see a binding and a literal arrive at different fields.
class JoinNode final : public GraphNode {
public:
    explicit JoinNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        NodeOutput output;
        output.writes.push_back(ChannelWrite{
            "value", json{{"item", input.state.get("item")}, {"extra", input.state.get("extra")}}});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Fails the first `flaky_failures_left` times it runs (an ordinary failure, which the node
// failure classifier lets a caller retry), then echoes its input.
class FlakyNode final : public GraphNode {
public:
    explicit FlakyNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++flaky_calls;
        if (flaky_failures_left.fetch_sub(1) > 0) throw std::runtime_error("scripted flake");
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.state.get("item")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A logic error: the classifier does not let anyone retry it.
class LogicFailNode final : public GraphNode {
public:
    explicit LogicFailNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++logic_calls;
        throw std::logic_error("scripted integrity failure");
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Numbers in, a number out.
class AddNode final : public GraphNode {
public:
    explicit AddNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto a = input.state.get("a");
        const auto b = input.state.get("b");
        if (!a.is_number() || !b.is_number()) throw std::invalid_argument("exp-add needs numbers");
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", a.get<double>() + b.get<double>()});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class UpperNode final : public GraphNode {
public:
    explicit UpperNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get("text").get<std::string>();
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", text});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A planner whose answers are scripted by attempt number. It reads how many answers the intake
// node has already judged (`repair.attempts`), so its choice is a function of the checkpointed
// state, not of how often the process called it.
class ScriptedPlannerNode final : public GraphNode {
public:
    explicit ScriptedPlannerNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto repair   = input.state.get("repair");
        const auto messages = input.state.get("messages");
        std::size_t attempt = 0;
        if (repair.is_object() && repair.contains("attempts") && repair["attempts"].is_number_unsigned())
            attempt = repair["attempts"].get<std::size_t>();
        std::string   reply;
        std::uint64_t tokens = 0;
        {
            std::lock_guard lock(planner_mutex);
            ++planner_calls;
            planner_seen.push_back(messages);
            if (attempt < planner_replies.size()) reply = planner_replies[attempt];
            if (attempt < planner_tokens.size()) tokens = planner_tokens[attempt];
        }
        if (tokens != 0 && input.ctx.usage) {
            // A complete report: a quarter of the tokens were output.
            sp::Usage usage;
            usage.stage        = sp::UsageStage::Final;
            usage.output_total = sp::Count{tokens / 4};
            usage.input_total  = sp::Count{tokens - tokens / 4};
            usage.total        = sp::Count{tokens};
            input.ctx.usage->add(usage);
        }
        json next = json::array();
        if (messages.is_array())
            for (std::size_t i = 0; i < messages.size(); ++i) next.push_back(messages[i]);
        next.push_back(json{{"role", "assistant"}, {"content", reply}});
        NodeOutput output;
        ChannelWrite write;
        write.channel = "messages";
        write.mode    = ChannelWrite::Mode::Overwrite;
        write.value   = std::move(next);
        output.writes.push_back(std::move(write));
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class CompletedNode final : public GraphNode {
public:
    explicit CompletedNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", "completed"});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

template <class Node>
inline void add_node_type(RegistrySnapshotBuilder& builder, const char* type, char implementation) {
    builder.add_node(
        manifest(ExecutableKind::Node, type, implementation),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<Node>(name);
        },
        json{{"type", "object"}}, json::object());
}

// Limits of the expansions the suites run, shared by the policy and the step plan intake.
inline TaskGraphProposalLimits expansion_limits() {
    TaskGraphProposalLimits limits;
    limits.max_tasks               = 6;
    limits.max_edges               = 12;
    limits.max_depth               = 6;
    limits.per_task_budget_ceiling = TaskGraphBudget{1000, 100, 100, 4096};
    limits.total_budget_ceiling    = TaskGraphBudget{6000, 600, 600, 24576};
    limits.max_attempts            = 3;
    limits.max_literal_bytes       = 256;
    limits.max_total_literal_bytes = 1024;
    return limits;
}

inline std::vector<TaskGraphTemplateContract> expansion_allowlist();

// What the planner is allowed to ask for: the trusted facts the intake node and the expansion
// policy both use.
inline StepPlanRepairNodeOptions plan_repair_options() {
    StepPlanRepairNodeOptions options;
    options.intake.source_id                      = "planner:plan-1";
    options.intake.limits.max_steps               = 8;
    options.intake.proposal_limits                = expansion_limits();
    // The planner is offered two of the templates the policy allows.
    for (const auto& contract : expansion_allowlist())
        if (contract.template_id == "calc.add/v1" || contract.template_id == "text.upper/v1")
            options.intake.template_allowlist.push_back(contract);
    options.intake.template_info = {{"calc.add/v1", {"a", "b"}, "Add two numbers."},
                                    {"text.upper/v1", {"text"}, "Upper-case a text."}};
    return options;
}

inline void add_expansion_nodes(RegistrySnapshotBuilder& builder);

inline RegistrySnapshot expansion_registry(ExecutionGuarantee = ExecutionGuarantee::Strict) {
    RegistrySnapshotBuilder builder;
    add_expansion_nodes(builder);
    add_step_plan_repair(builder, plan_repair_options());
    builder.add_node(
        manifest(ExecutableKind::Node, "test.scripted_planner", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ScriptedPlannerNode>(name);
        },
        json{{"type", "object"}}, json{{"writes", json::array({"messages"})}});
    return std::move(builder).build();
}

inline void add_expansion_nodes(RegistrySnapshotBuilder& builder) {
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-completed", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CompletedNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    add_node_type<EchoNode>(builder, "exp-echo", 'a');
    add_node_type<SlowNode>(builder, "exp-slow", 'b');
    add_node_type<FailNode>(builder, "exp-fail", 'c');
    add_node_type<GateNode>(builder, "exp-gate", 'd');
    add_node_type<JoinNode>(builder, "exp-join", 'e');
    add_node_type<FlakyNode>(builder, "exp-flaky", 'f');
    add_node_type<LogicFailNode>(builder, "exp-logic", '9');
    add_node_type<AddNode>(builder, "exp-add", '8');
    add_node_type<UpperNode>(builder, "exp-upper", '7');
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
}

using ExpansionRuntime = BasicAdmittedRuntime<expansion_registry>;

// ---- A fragment store that logs every task transition it is asked to make durable. ----

class RecordingFragmentStore final : public SyncTaskGraphFragmentStore {
public:
    explicit RecordingFragmentStore(std::shared_ptr<TaskGraphFragmentStore> inner)
        : inner_(std::make_shared<BlockingTaskGraphFragmentStore>(std::move(inner))) {}

    TaskGraphPublishResult publish_sync(const TaskGraphFragmentRecord& record) override {
        return inner_->publish(record);
    }
    std::optional<TaskGraphFragmentRecord> load_sync(std::string_view id) const override {
        return inner_->load(id);
    }
    std::optional<TaskGraphFragmentRecord> find_published_lineage_sync(
        std::string_view owner, std::string_view parent, std::string_view operation) const override {
        return inner_->find_published_lineage(owner, parent, operation);
    }
    std::optional<TaskGraphFragmentRecord> find_published_sync(
        std::string_view owner, std::string_view parent, std::string_view operation,
        std::string_view proposal) const override {
        return inner_->find_published(owner, parent, operation, proposal);
    }
    TaskGraphPublishResult compare_update_sync(std::string_view id, std::uint64_t revision,
                                         const TaskGraphFragmentRecord& record) override {
        std::lock_guard lock(mutex_);
        auto previous = last_.find(std::string(id));
        if (previous == last_.end()) {
            const auto loaded = inner_->load(id);
            if (loaded) previous = last_.emplace(std::string(id), *loaded).first;
        }
        const auto result = inner_->compare_update(id, revision, record);
        if (result == TaskGraphPublishResult::Published && previous != last_.end()) {
            for (std::size_t index = 0; index < record.tasks.size(); ++index) {
                const auto& now    = record.tasks[index];
                const auto& before = previous->second.tasks[index];
                if (now.state == before.state && now.attempt == before.attempt) continue;
                events_.push_back(now.task_id + ":" + std::string(to_string(now.state)) + ":" +
                                  std::to_string(now.attempt));
                images_.push_back(record);
            }
            if (record.terminal && !previous->second.terminal) {
                events_.push_back("terminal");
                images_.push_back(record);
            }
            previous->second = record;
        }
        return result;
    }

    std::vector<std::string> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    /// The record as it was after each event of events(), in the same order.
    std::vector<TaskGraphFragmentRecord> images() const {
        std::lock_guard lock(mutex_);
        return images_;
    }

private:
    std::shared_ptr<BlockingTaskGraphFragmentStore> inner_;
    mutable std::mutex                             mutex_;
    std::map<std::string, TaskGraphFragmentRecord> last_;
    std::vector<std::string>                       events_;
    std::vector<TaskGraphFragmentRecord>           images_;
};

// ---- Fixture helpers. ----

inline TaskGraphTemplateContract template_contract(std::string id, char identity, std::string binding,
                                                   std::vector<std::string> inputs = {"/item"},
                                                   std::string artifact = "result") {
    TaskGraphTemplateContract contract;
    contract.template_id      = std::move(id);
    contract.content_identity = digest(identity);
    contract.input_fields     = std::move(inputs);
    contract.output_artifacts = {TaskGraphArtifactContract{std::move(artifact), {"/channels/value/value"}}};
    contract.budget_ceiling   = TaskGraphBudget{1000, 100, 100, 4096};
    contract.child_binding    = std::move(binding);
    contract.kind             = "program";
    return contract;
}

inline std::vector<TaskGraphTemplateContract> expansion_allowlist() {
    return {template_contract("ok/v1", 'a', "ok"),
            template_contract("slow/v1", 'b', "slow"),
            template_contract("fail/v1", 'c', "fail"),
            template_contract("gate/v1", 'd', "gate"),
            template_contract("join/v1", 'e', "join", {"/item", "/extra"}),
            template_contract("flaky/v1", 'f', "flaky"),
            template_contract("logic/v1", '9', "logic"),
            // The templates a step plan may ask for: a plan names them, so they take inputs
            // by name and answer on one artifact.
            template_contract("calc.add/v1", '8', "add", {"/a", "/b"}, "sum"),
            template_contract("text.upper/v1", '7', "upper", {"/text"}, "upper")};
}

inline json child_document(const char* node_type, std::vector<std::string> extra_channels = {}) {
    auto document = program_document(node_type);
    for (const auto& channel : extra_channels)
        document["root"]["definition"]["channels"][channel] =
            json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}};
    document["declared_budget_requirements"][0]["maximum"] = 3333;
    document["declared_budget_requirements"][1]["maximum"] = 333;
    document["declared_budget_requirements"][2]["maximum"] = 333;
    document["declared_budget_requirements"][5]["maximum"] = 3;
    document["root"]["definition"]["channels"]["item"] =
        json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}};
    return document;
}

inline json explicit_budget() {
    return json{{"wall_time_ms", 1000}, {"model_tokens", 1}, {"monetary_microunits", 1},
                {"output_bytes", 512}};
}

// A v1 task whose inputs are the `/channels/value/value` of each dependency, bound to /item.
inline json task(const std::string& id, const std::string& template_id,
          std::vector<std::string> dependencies = {}) {
    json bindings = json::array();
    if (!dependencies.empty()) {
        bindings.push_back(json{{"from", json{{"task", dependencies.front()},
                                              {"artifact", "result"},
                                              {"field", "/channels/value/value"}}},
                                {"to", json{{"field", "/item"}}}});
    }
    return json{{"id", id},
                {"template", template_id},
                {"input_bindings", std::move(bindings)},
                {"depends_on", std::move(dependencies)},
                {"budget", explicit_budget()}};
}

inline json proposal_of(const std::vector<json>& tasks, std::uint32_t schema_version = 1) {
    json encoded = json::array();
    for (const auto& task : tasks) encoded.push_back(task);
    return json{{"schema_version", schema_version},
                {"tasks", std::move(encoded)},
                {"join", json{{"kind", "all"}}}};
}

struct ExpansionRun {
    ProgramResult                          result;
    std::vector<std::string>               events;
    std::optional<TaskGraphFragmentRecord> record;
    std::string                            fragment_id;
    /// The budget each child run was launched with, by child run id.
    std::map<std::string, RunBudget>       child_budgets;
    /// The record after each event, in the order of `events`.
    std::vector<TaskGraphFragmentRecord>   images;
};

struct RunOptions {
    std::uint64_t max_tasks       = 4;
    std::uint64_t max_depth       = 4;
    std::uint32_t concurrency     = 1;
    const char*   failure_policy  = "collect";
    /// Program operations the run may start; 0 means `max_tasks`.
    std::uint64_t max_operations  = 0;
    /// False for a Program that does not expand a task graph: it gets no compile or child budget.
    bool          expands         = true;
};

class ExpansionHarness {
public:
    ExpansionHarness() : fixture(4) {
        auto& policy = *fixture.expansion;
        policy.limits             = expansion_limits();
        policy.template_allowlist = expansion_allowlist();
        store = std::make_shared<RecordingFragmentStore>(fixture.task_graph_fragments);
        fixture.fragment_store_override = store;
        fixture.runtime                 = fixture.make_runtime();
    }

    ExpansionRun run(json proposal, RunOptions options = {}) {
        auto document = expand_task_graph_document(std::move(proposal), options.max_tasks,
                                                   options.max_tasks * 2, options.max_depth,
                                                   options.concurrency);
        document["root"]["failure_policy"] = options.failure_policy;
        return run_program(std::move(document), json{{"item", "seed"}}, options);
    }

    /// Admits `document`, links the template children and runs it to its end.
    ExpansionRun run_program(json document, json input, RunOptions options = {}) {
        const auto parent = fixture.admit_document(std::move(document));
        link_children(fixture, parent,
                      {{"ok", fixture.admit_document(child_document("exp-echo"))},
                       {"slow", fixture.admit_document(child_document("exp-slow"))},
                       {"fail", fixture.admit_document(child_document("exp-fail"))},
                       {"gate", fixture.admit_document(child_document("exp-gate"))},
                       {"join", fixture.admit_document(child_document("exp-join", {"extra"}))},
                       {"flaky", fixture.admit_document(child_document("exp-flaky"))},
                       {"logic", fixture.admit_document(child_document("exp-logic"))},
                       {"add", fixture.admit_document(child_document("exp-add", {"a", "b"}))},
                       {"upper", fixture.admit_document(child_document("exp-upper", {"text"}))}},
                      BudgetLimits{3333, 333, 333, 1, 1, 3, 0, 0, 0});
        auto handle = fixture.runtime->start(
            "tenant:runtime", parent,
            ProgramInvocation{std::move(input),
                              RunBudget{10000, 1000, 1000, options.concurrency,
                                        options.max_operations ? options.max_operations : options.max_tasks, 20,
                                        options.expands ? 1U : 0U,
                                        options.expands ? static_cast<std::uint32_t>(options.max_depth) : 0U,
                                        options.expands ? options.max_tasks : 0U},
                              "trace-expansion", {}});
        ExpansionRun run{handle.wait(), store->events(), std::nullopt, {}, {}, store->images()};
        if (run.result.output().is_object() && run.result.output().contains("fragment_id")) {
            run.fragment_id = run.result.output()["fragment_id"].get<std::string>();
            run.record      = fixture.task_graph_fragments->load(run.fragment_id);
        }
        for (const auto& child : handle.snapshot().children())
            run.child_budgets[child.child_run_id] = child.invocation.granted_budget;
        return run;
    }

    void reset_observations() {
        {
            std::lock_guard lock(node_log_mutex);
            node_log.clear();
        }
        gate_active.store(0);
        gate_arrived.store(0);
        gate_peak.store(0);
        gate_target.store(1);
        flaky_failures_left.store(0);
        flaky_calls.store(0);
        logic_calls.store(0);
        planner_calls.store(0);
        std::lock_guard lock(planner_mutex);
        planner_replies.clear();
        planner_tokens.clear();
        planner_seen.clear();
    }

    ExpansionRuntime                         fixture;
    std::shared_ptr<RecordingFragmentStore> store;
};

using Events = std::vector<std::string>;

inline std::string attempt_identity(const std::string& fragment_id, const std::string& task_id,
                             std::uint32_t attempt) {
    return neograph::program::detail::sha256_identity(
        "program-task-graph-attempt/v1",
        neograph::program::detail::canonical_json_bytes(
            json{{"fragment_id", fragment_id}, {"task_id", task_id}, {"attempt", attempt}}));
}


}  // namespace neograph::program_test::expansion
