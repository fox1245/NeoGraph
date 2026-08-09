#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "javascript.h"
#include "registry_access.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
neograph::json orchestration_document(neograph::json root, std::string node_type);
namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::atomic<unsigned> completed_calls{0};
std::atomic<unsigned> interrupt_calls{0};
std::atomic<unsigned> blocking_calls{0};
std::atomic<unsigned> followup_calls{0};
std::atomic<unsigned> stubborn_calls{0};
std::atomic<unsigned> scheduler_blocking_calls{0};

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:test",
                              {},
                              {},
                              {}};
}

class CompletedNode final : public GraphNode {
public:
    explicit CompletedNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++completed_calls;
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", "completed"});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class InterruptNode final : public GraphNode {
public:
    explicit InterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt("approval required", json{{"request", "approve"}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("decision")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class EffectInterruptNode final : public GraphNode {
public:
    explicit EffectInterruptNode(std::string name, bool non_idempotent = false)
        : name_(std::move(name)), non_idempotent_(non_idempotent) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-effect-1"},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect",
                      json{{"effect_id", "effect-search-1"},
                           {"idempotency", non_idempotent_ ? "non_idempotent" : "supported"},
                           {"tool", "search"},
                           {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        }
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", input.ctx.resume_value->at("result")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    bool        non_idempotent_ = false;
};

class FailingNode final : public GraphNode {
public:
    explicit FailingNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        throw std::runtime_error("classified core failure");
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class BlockingNode final : public GraphNode {
public:
    explicit BlockingNode(std::string               name,
                          std::chrono::milliseconds duration = std::chrono::seconds(5))
        : name_(std::move(name)), duration_(duration) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++blocking_calls;
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(duration_);
        co_await timer.async_wait(
            asio::bind_cancellation_slot(input.ctx.cancel_token->slot(), asio::use_awaitable));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string               name_;
    std::chrono::milliseconds duration_;
};
class StubbornNode final : public GraphNode {
public:
    explicit StubbornNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++stubborn_calls;
        // Simulate a late sibling/provider budget signal. The enclosing
        // runtime must preserve an earlier user/timeout cancellation cause.
        if (input.ctx.budget_exhausted) {
            input.ctx.budget_exhausted->store(true, std::memory_order_release);
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await  timer.async_wait(asio::use_awaitable);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};
class SchedulerBlockingNode final : public GraphNode {
public:
    explicit SchedulerBlockingNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++scheduler_blocking_calls;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class FollowupNode final : public GraphNode {
public:
    explicit FollowupNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++followup_calls;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot runtime_registry(
    ExecutionGuarantee completed_guarantee = ExecutionGuarantee::Strict) {
    RegistrySnapshotBuilder builder;
    auto                    completed = manifest(ExecutableKind::Node, "runtime-completed", '1');
    completed.execution_guarantee     = completed_guarantee;
    builder.add_node(
        std::move(completed),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CompletedNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-interrupt", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<InterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-interrupt", 'a'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-effect-nonidempotent", 'c'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EffectInterruptNode>(name, true);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-failing", '4'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FailingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-blocking", '5'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-short-blocking", 'b'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingNode>(name, std::chrono::milliseconds(500));
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-stubborn", '7'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StubbornNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-scheduler-blocking", '8'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SchedulerBlockingNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "runtime-followup", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FollowupNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-alternate", '9'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

json budget_requirements() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

json program_document(std::string node_type) {
    json nodes{{"work", json{{"type", node_type}}}};
    json edges = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "__end__"}},
    });
    if (node_type == "runtime-blocking" || node_type == "runtime-short-blocking" ||
        node_type == "runtime-scheduler-blocking") {
        nodes["followup"] = json{{"type", "runtime-followup"}};
        edges             = json::array({
            json{{"from", "__start__"}, {"to", "work"}},
            json{{"from", "work"}, {"to", "followup"}},
            json{{"from", "followup"}, {"to", "__end__"}},
        });
    }
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", std::move(nodes)},
        {"edges", std::move(edges)},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}
json failing_fanout_program_document(std::string completed_name = "completed",
                                     std::string failing_name   = "failing") {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{completed_name, json{{"type", "runtime-completed"}}},
                       {failing_name, json{{"type", "runtime-failing"}}}}},
        {"edges", json::array({
                      json{{"from", "__start__"}, {"to", completed_name}},
                      json{{"from", "__start__"}, {"to", failing_name}},
                      json{{"from", completed_name}, {"to", "__end__"}},
                      json{{"from", failing_name}, {"to", "__end__"}},
                  })},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

json step_limited_program_document() {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"work", json{{"type", "runtime-completed"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                               json{{"from", "work"}, {"to", "work"}}})},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

struct ChildBindingRegistry {
    using Key = std::pair<std::string, std::string>;

    void bind(std::string                parent_version_id,
              std::string                binding_name,
              ProgramRuntimeChildBinding binding) {
        std::lock_guard lock(mutex);
        bindings.insert_or_assign(Key{std::move(parent_version_id), std::move(binding_name)},
                                  std::move(binding));
    }

    std::optional<ProgramRuntimeChildBinding> resolve(std::string_view owner_scope,
                                                      std::string_view parent_version_id,
                                                      std::string_view binding_name) const {
        if (owner_scope != "tenant:runtime") return std::nullopt;
        std::lock_guard lock(mutex);
        const auto      found =
            bindings.find(Key{std::string(parent_version_id), std::string(binding_name)});
        return found == bindings.end() ? std::nullopt
                                       : std::optional<ProgramRuntimeChildBinding>{found->second};
    }

    void clear() {
        std::lock_guard lock(mutex);
        bindings.clear();
    }

private:
    mutable std::mutex                        mutex;
    std::map<Key, ProgramRuntimeChildBinding> bindings;
};

struct AdmittedRuntime {
    RegistrySnapshot                        registry;
    AdmissionProfile                        profile;
    PolicySnapshot                          policy;
    std::shared_ptr<InMemoryProgramStore>   store;
    std::shared_ptr<EngineGenerationCache>  engines;
    std::shared_ptr<ProgramCatalog>         catalog;
    std::shared_ptr<CheckpointStore>        checkpoints;
    std::shared_ptr<ProgramTransitionStore> journal;
    std::shared_ptr<ChildBindingRegistry>   child_bindings;
    ProgramChildQuotaConfig                 child_quota;
    std::size_t                             scheduler_thread_count;
    std::unique_ptr<ProgramRuntime>         runtime;

    explicit AdmittedRuntime(std::size_t                             scheduler_threads  = 1,
                             std::shared_ptr<CheckpointStore>        checkpoint_backend = {},
                             std::shared_ptr<ProgramTransitionStore> journal_backend    = {},
                             ProgramChildQuotaConfig                 quota              = {},
                             ExecutionGuarantee minimum_guarantee = ExecutionGuarantee::Strict,
                             bool               allow_javascript  = false)
        : registry(runtime_registry(minimum_guarantee)),
          profile(make_profile(registry, minimum_guarantee, allow_javascript)),
          policy(make_policy(profile, minimum_guarantee, allow_javascript)),
          store(std::make_shared<InMemoryProgramStore>()),
          engines(std::make_shared<EngineGenerationCache>()),
          catalog(std::make_shared<ProgramCatalog>(
              CatalogConfig{store, registry, engines, "program-runtime-test/v1"})),
          checkpoints(checkpoint_backend ? std::move(checkpoint_backend)
                                         : std::make_shared<InMemoryCheckpointStore>()),
          journal(journal_backend ? std::move(journal_backend)
                                  : std::make_shared<InMemoryProgramTransitionStore>()),
          child_bindings(std::make_shared<ChildBindingRegistry>()),
          child_quota(quota),
          scheduler_thread_count(scheduler_threads),
          runtime(make_runtime()) {}

    static AdmissionProfile make_profile(
        const RegistrySnapshot& registry,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        AdmissionProfileBuilder builder;
        builder.id("runtime-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(1)
            .minimum_execution_guarantee(minimum_guarantee)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        if (allow_javascript) builder.allow_source_kind(SourceKind::JavaScript);
        for (const auto& identity : registry.identities())
            builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(
        const AdmissionProfile& profile,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        const auto            budget_ceiling = allow_javascript
                                                   ? BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32}
                                                   : BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32};
        PolicySnapshotBuilder builder;
        builder.id("runtime-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:runtime")
            .admission_profile(profile)
            .budget_ceiling(budget_ceiling)
            .minimum_execution_guarantee(minimum_guarantee);
        return std::move(builder).build();
    }
    std::unique_ptr<ProgramRuntime> make_runtime() const {
        RuntimeConfig config{catalog,
                             checkpoints,
                             {},
                             journal,
                             scheduler_thread_count,
                             [bindings = child_bindings](std::string_view owner_scope,
                                                         std::string_view parent_version_id,
                                                         std::string_view binding_name) {
                                 return bindings->resolve(owner_scope, parent_version_id,
                                                          binding_name);
                             }};
        config.child_quota = child_quota;
        return std::make_unique<ProgramRuntime>(std::move(config));
    }

    ProgramVersion admit(std::string node_type) {
        return admit_document(program_document(std::move(node_type)));
    }

    ProgramVersion admit_document(json document) {
        return admit_source(
            ProgramSource::from_cpp_builder("test:runtime", 1, std::move(document)));
    }

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
    ProgramVersion admit_javascript(std::string source) {
        return admit_source(
            ProgramSource::from_javascript("test:runtime-control.js", std::move(source)));
    }
#endif

    ProgramVersion admit_source(ProgramSource source) {
        ProgramCompiler              compiler(registry, {"program-runtime-test/v1"});
        std::optional<ProgramBundle> bundle;
        try {
            bundle = compiler.compile(source);
        } catch (const ProgramCompileError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
        try {
            return catalog->admit(*bundle, ProgramAdmission{"tenant:runtime", profile, policy, {}});
        } catch (const ProgramAdmissionError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
    }

    void bind_child(const ProgramVersion&      parent,
                    std::string                binding_name,
                    ProgramRuntimeChildBinding binding) {
        child_bindings->bind(parent.id(), std::move(binding_name), std::move(binding));
    }

    void clear_child_bindings() { child_bindings->clear(); }

    void recreate_catalog_and_runtime() {
        runtime.reset();
        catalog.reset();
        engines = std::make_shared<EngineGenerationCache>();
        catalog = std::make_shared<ProgramCatalog>(
            CatalogConfig{store, registry, engines, "program-runtime-test/v1"});
        runtime = make_runtime();
    }
};

struct LinkedChildAdmission {
    ProgramVersion    parent_version;
    ProgramVersion    child_version;
    ModuleLinkReceipt receipt;
};

LinkedChildAdmission link_child_versions(
    AdmittedRuntime&   fixture,
    ProgramVersion     parent_version,
    ProgramVersion     child_version,
    BudgetLimits       child_budget       = BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
    ExecutionGuarantee accepted_guarantee = ExecutionGuarantee::Strict) {
    const auto child_bundle = fixture.store->get_bundle(child_version.bundle_id());
    if (!child_bundle) throw std::runtime_error("child bundle was not admitted");

    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:runtime";
    module_data.coordinate     = ModuleCoordinate{"runtime", "parent", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    module_data.children.push_back(
        ChildProgramDescriptor{"child",
                               child_version.id(),
                               {ModulePort{"input", child_bundle->input_contract()}},
                               {ModulePort{"output", child_bundle->output_contract()}},
                               child_bundle->capability_effect_closure().capabilities,
                               child_bundle->capability_effect_closure().effects,
                               std::move(child_budget),
                               accepted_guarantee});
    module_data.allowed_capabilities = child_bundle->capability_effect_closure().capabilities;
    module_data.declared_effects     = child_bundle->capability_effect_closure().effects;
    const auto       parent_module   = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = parent_module.coordinate();
    resolution.modules.push_back(parent_module);
    auto receipt =
        link_module_child(resolution, parent_module, "child", *child_bundle, child_version);
    fixture.bind_child(parent_version, "child", ProgramRuntimeChildBinding{receipt, child_version});
    return LinkedChildAdmission{std::move(parent_version), std::move(child_version),
                                std::move(receipt)};
}

LinkedChildAdmission make_linked_child(AdmittedRuntime& fixture,
                                       std::string      parent_node_type = "runtime-blocking",
                                       std::string      child_node_type  = "runtime-blocking") {
    auto parent_document = program_document(std::move(parent_node_type));
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)));
}
LinkedChildAdmission make_recursive_linked_child(AdmittedRuntime& fixture) {
    auto parent_document = program_document("runtime-blocking");
    parent_document["declared_budget_requirements"][3]["maximum"] = 2;
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 4;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 4;

    auto child_document = program_document("runtime-blocking");
    child_document["declared_budget_requirements"][7]["minimum"] = 0;
    child_document["declared_budget_requirements"][7]["maximum"] = 4;
    child_document["declared_budget_requirements"][8]["minimum"] = 0;
    child_document["declared_budget_requirements"][8]["maximum"] = 4;
    auto linked = link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                                      fixture.admit_document(std::move(child_document)),
                                      BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 4, 4});
    fixture.bind_child(linked.child_version, "child",
                       ProgramRuntimeChildBinding{linked.receipt, linked.child_version});
    return linked;
}

LinkedChildAdmission make_durable_spawn_child(
    AdmittedRuntime&             fixture,
    std::string                  parent_node_type   = "runtime-completed",
    std::string                  child_node_type    = "runtime-completed",
    std::optional<std::uint64_t> timeout_ms         = std::nullopt,
    ExecutionGuarantee           accepted_guarantee = ExecutionGuarantee::Strict) {
    json await = json{{"op", "await"}, {"body", json{{"op", "spawn"}, {"child_binding", "child"}}}};
    if (timeout_ms) await["timeout_ms"] = *timeout_ms;
    auto parent_document = orchestration_document(std::move(await), std::move(parent_node_type));
    parent_document["declared_budget_requirements"][4]["minimum"] = 2;
    parent_document["declared_budget_requirements"][4]["maximum"] = 2;
    parent_document["declared_budget_requirements"][7]["minimum"] = 1;
    parent_document["declared_budget_requirements"][7]["maximum"] = 1;
    parent_document["declared_budget_requirements"][8]["minimum"] = 1;
    parent_document["declared_budget_requirements"][8]["maximum"] = 1;
    return link_child_versions(fixture, fixture.admit_document(std::move(parent_document)),
                               fixture.admit(std::move(child_node_type)),
                               BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                               accepted_guarantee);
}

RunBudget grant() {
    return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};
}
std::string pending_call_id(const ProgramResult& result) {
    const auto interrupt = result.interrupt();
    if (!interrupt) throw std::invalid_argument("Program result is not interrupted");
    if (interrupt->pending_input) return interrupt->pending_input->call_id();
    if (interrupt->pending_effect) return interrupt->pending_effect->call_id();
    throw std::invalid_argument("Interrupted Program result has no pending call");
}

ProgramResume resume_for(const ProgramResult&              result,
                         json                              value,
                         std::string                       trace_id,
                         std::shared_ptr<ProgramEventSink> events = {}) {
    return ProgramResume{std::move(value), std::move(trace_id), std::move(events),
                         pending_call_id(result)};
}

class ThrowingSink final : public ProgramEventSink {
public:
    explicit ThrowingSink(ProgramEventKind fail_on) : fail_on_(fail_on) {}

    void on_event(const ProgramEvent& event) override {
        seen.push_back(event.kind);
        if (event.kind == fail_on_) {
            throw std::runtime_error("sink rejected event");
        }
    }

    std::vector<ProgramEventKind> seen;

private:
    ProgramEventKind fail_on_;
};
class CountingSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent&) override { calls.fetch_add(1); }

    std::atomic<unsigned> calls{0};
};

class JournalObservingSink final : public ProgramEventSink {
public:
    explicit JournalObservingSink(std::shared_ptr<ProgramTransitionStore> journal)
        : journal_(std::move(journal)) {}

    void on_event(const ProgramEvent& event) override {
        if (event.kind != ProgramEventKind::CheckpointPublished &&
            event.kind != ProgramEventKind::Terminal) {
            return;
        }
        const auto latest    = journal_->latest("tenant:runtime", event.run_id);
        const bool committed = latest &&
                               latest->continuation.state == ContinuationState::Completed &&
                               latest->core_checkpoint.has_value();
        if (event.kind == ProgramEventKind::CheckpointPublished) {
            checkpoint_observed_committed.store(committed);
        } else {
            terminal_observed_committed.store(committed);
        }
    }

    std::atomic<bool> checkpoint_observed_committed{false};
    std::atomic<bool> terminal_observed_committed{false};

private:
    std::shared_ptr<ProgramTransitionStore> journal_;
};

class ConflictOnceJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        if (publication.journal_record.continuation.state != ContinuationState::Running &&
            !injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return inner_.compare_publish(owner, expected, std::move(publication));
    }
    bool injected() const noexcept { return injected_.load(); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
};

class BlockAfterJavaScriptResultJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry>
    load_javascript_commands(std::string_view owner,
                             std::string_view run_id,
                             std::uint64_t    sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const bool command_result = !publication.commands.empty() &&
                                    publication.commands.back().completed();
        const auto published = inner_.compare_publish(owner, expected, std::move(publication));
        if (!command_result || published != ProgramTransitionPublishResult::Published) {
            return published;
        }
        std::unique_lock lock(mutex_);
        observed_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
        return published;
    }
    bool wait_for_result(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return observed_; });
    }
    void release_result() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    InMemoryProgramTransitionStore inner_;
    mutable std::mutex              mutex_;
    std::condition_variable         condition_;
    bool                            observed_ = false;
    bool                            released_ = false;
};

class FailChildDispatchOnceJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const auto children = publication.run_record.children();
        if (children.empty() || children.back().state != ProgramChildState::Dispatched ||
            !block_dispatch_.load()) {
            return inner_.compare_publish(owner, expected, std::move(publication));
        }
        if (!injected_.exchange(true)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        return ProgramTransitionPublishResult::Conflict;
    }
    void allow_dispatch() noexcept { block_dispatch_.store(false); }

private:
    InMemoryProgramTransitionStore inner_;
    std::atomic<bool>              injected_{false};
    std::atomic<bool>              block_dispatch_{true};
};
class ThrowOnLatestJournal final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view, std::string_view) const override {
        throw std::runtime_error("journal latest failed");
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        return inner_.compare_publish(owner, expected, std::move(publication));
    }
    std::optional<ProgramJournalRecord> stored_latest(std::string_view run_id) const {
        return inner_.latest("tenant:runtime", run_id);
    }

private:
    InMemoryProgramTransitionStore inner_;
};

class AdversarialCheckpointStore final : public CheckpointStore {
public:
    enum class Mode {
        Normal,
        WrongThread,
        WrongThreadAfterPrecheck,
        WrongSchemaAfterPrecheck,
        WrongSchemaAtCoreLoad,
        WrongIdAtCoreLoad,
        MissingAfterFirstExactLoad,
        MissingAfterCoreLoad
    };

    void arm(Mode mode) {
        mode_.store(mode);
        exact_loads_.store(0);
    }
    std::uint32_t exact_loads() const noexcept { return exact_loads_.load(); }

    void save(const Checkpoint& checkpoint) override { inner_->save(checkpoint); }

    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return inner_->load_latest(thread_id);
    }

    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        auto       checkpoint = inner_->load_by_id(id);
        const auto call       = exact_loads_.fetch_add(1);
        const auto mode       = mode_.load();
        if (mode == Mode::MissingAfterFirstExactLoad && call > 0) {
            return std::nullopt;
        }
        if (mode == Mode::MissingAfterCoreLoad && call > 2) {
            return std::nullopt;
        }
        if (checkpoint &&
            (mode == Mode::WrongThread || (mode == Mode::WrongThreadAfterPrecheck && call > 0))) {
            checkpoint->thread_id += "-tampered";
        }
        if (checkpoint && mode == Mode::WrongSchemaAfterPrecheck && call > 0) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongSchemaAtCoreLoad && call > 1) {
            ++checkpoint->schema_version;
        }
        if (checkpoint && mode == Mode::WrongIdAtCoreLoad && call > 1) {
            checkpoint->id += "-tampered";
        }
        return checkpoint;
    }

    std::vector<Checkpoint> list(const std::string& thread_id, int limit) override {
        return inner_->list(thread_id, limit);
    }

    void delete_thread(const std::string& thread_id) override { inner_->delete_thread(thread_id); }

    void put_writes(const std::string&  thread_id,
                    const std::string&  parent_checkpoint_id,
                    const PendingWrite& write) override {
        inner_->put_writes(thread_id, parent_checkpoint_id, write);
    }

    std::vector<PendingWrite> get_writes(const std::string& thread_id,
                                         const std::string& parent_checkpoint_id) override {
        return inner_->get_writes(thread_id, parent_checkpoint_id);
    }

    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        inner_->clear_writes(thread_id, parent_checkpoint_id);
    }

private:
    std::shared_ptr<InMemoryCheckpointStore> inner_ = std::make_shared<InMemoryCheckpointStore>();
    std::atomic<Mode>                        mode_{Mode::Normal};
    std::atomic<std::uint32_t>               exact_loads_{0};
};
json typed_event_value(const TypedGraphEvent& event) {
    json value{{"index", event.index()}};
    switch (event.index()) {
        case 0: {
            const auto& typed  = std::get<NodeStartEvent>(event);
            value["node_name"] = typed.node_name;
            value["retry_attempt"] =
                typed.retry_attempt ? json(*typed.retry_attempt) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 1: {
            const auto& typed     = std::get<NodeEndEvent>(event);
            value["node_name"]    = typed.node_name;
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["send_count"]   = typed.send_count;
            value["data"]         = typed.data;
            break;
        }
        case 2: {
            const auto& typed  = std::get<LlmTokenEvent>(event);
            value["node_name"] = typed.node_name;
            value["token"]     = typed.token;
            break;
        }
        case 3: {
            const auto& typed  = std::get<ChannelWriteEvent>(event);
            value["node_name"] = typed.node_name;
            value["channel"]   = typed.channel;
            value["value"]     = typed.value;
            break;
        }
        case 4:
            value["state"] = std::get<StateSnapshotEvent>(event).state;
            break;
        case 5: {
            const auto& typed     = std::get<RoutingEvent>(event);
            value["command_goto"] = typed.command_goto ? json(*typed.command_goto) : json(nullptr);
            value["next_nodes"]   = typed.next_nodes;
            value["step"]         = typed.step ? json(*typed.step) : json(nullptr);
            value["data"]         = typed.data;
            break;
        }
        case 6: {
            value["sends"] = json::array();
            for (const auto& send : std::get<SendDispatchEvent>(event).sends) {
                value["sends"].push_back({{"target_node", send.target_node},
                                          {"input", send.input},
                                          {"source_node", send.source_node}});
            }
            break;
        }
        case 7: {
            const auto& typed  = std::get<InterruptEvent>(event);
            value["node_name"] = typed.node_name;
            value["phase"]     = typed.phase ? json(*typed.phase) : json(nullptr);
            value["checkpoint_id"] =
                typed.checkpoint_id ? json(*typed.checkpoint_id) : json(nullptr);
            value["data"] = typed.data;
            break;
        }
        case 8: {
            const auto& typed  = std::get<ErrorEvent>(event);
            value["node_name"] = typed.node_name;
            value["message"]   = typed.message;
            value["data"]      = typed.data;
            break;
        }
        case 9: {
            const auto& typed  = std::get<RawGraphEvent>(event).event;
            value["type"]      = static_cast<int>(typed.type);
            value["node_name"] = typed.node_name;
            value["data"]      = typed.data;
            break;
        }
        default:
            throw std::logic_error("Unknown TypedGraphEvent alternative");
    }
    return value;
}
}  // namespace

TEST(ProgramRuntimeTest, CompletedRunPinsAdmittedIdentitiesAndPublishesOrderedEvents) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-completed", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
    EXPECT_EQ(result.bundle_id(), version.bundle_id());
    EXPECT_EQ(result.operation_id(), "root");
    EXPECT_EQ(result.attempt(), 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(result.remaining_budget().max_concurrency, 1U);
    ASSERT_TRUE(result.checkpoint().has_value());
    EXPECT_EQ(result.checkpoint()->core_name, "main");
    EXPECT_EQ(result.checkpoint()->core_generation_id,
              version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->program_version_id, version.id());
    ASSERT_TRUE(journal->core_checkpoint.has_value());
    EXPECT_EQ(journal->core_checkpoint->core_name, result.checkpoint()->core_name);
    EXPECT_EQ(journal->core_checkpoint->core_thread_id, result.checkpoint()->core_thread_id);
    EXPECT_EQ(journal->core_checkpoint->checkpoint_id, result.checkpoint()->checkpoint_id);

    const auto events = handle.events_after(0);
    ASSERT_GE(events.size(), 4U);
    EXPECT_EQ(events.front().kind, ProgramEventKind::Started);
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished;
                            }),
              1);
    for (std::size_t index = 0; index < events.size(); ++index) {
        EXPECT_EQ(events[index].sequence, index + 1);

        EXPECT_EQ(events[index].run_id, result.run_id());
        EXPECT_EQ(events[index].program_version_id, version.id());
        EXPECT_EQ(events[index].attempt, 1U);
        EXPECT_EQ(events[index].trace_id, "trace-completed");
    }
}
TEST(ProgramRuntimeTest, RejectsCrossScopeStartBeforePublishingRun) {
    completed_calls.store(0);
    AdmittedRuntime   fixture;
    const auto        version = fixture.admit("runtime-completed");
    ProgramInvocation invocation{json::object(), grant(), "trace-cross-scope", {}};
    invocation.requested_run_id = "cross-scope-run";

    EXPECT_THROW((void)fixture.runtime->start("tenant:other", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_FALSE(fixture.journal->load("tenant:other", "cross-scope-run").has_value());
    EXPECT_FALSE(fixture.journal->load("tenant:runtime", "cross-scope-run").has_value());
}

TEST(ProgramRuntimeTest, RequestedRunIdIsUsedExactlyAndCollisionNeverDispatches) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto invocation             = ProgramInvocation{json::object(), grant(), "trace-requested", {}};
    invocation.requested_run_id = "harness-run-exact";
    const auto first = fixture.runtime->start("tenant:runtime", version, invocation).wait();
    EXPECT_EQ(first.run_id(), "harness-run-exact");
    EXPECT_EQ(completed_calls.load(), 1U);

    EXPECT_THROW((void)fixture.runtime->start("tenant:runtime", version, std::move(invocation)),
                 ProgramDiagnosticError);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CanonicalRunInvocationIsRetainedExactlyAndAcceptsRuntimeEventSink) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    RunInvocation invocation;
    invocation.owner_scope        = "tenant:runtime";
    invocation.agent_id           = "test-canonical-adapter";
    invocation.program_version_id = version.id();
    invocation.run_id             = "canonical-runtime-run";
    invocation.budget             = grant();
    invocation.input              = json{{"request", "canonical"}};
    invocation.message_sequence   = 17;
    invocation.idempotency_key    = "test-canonical-adapter:17";
    invocation.correlation_id     = "trace-canonical-runtime";
    invocation.validate();
    auto sink = std::make_shared<CountingSink>();

    auto       handle = fixture.runtime->start(invocation, sink);
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.run_id(), invocation.run_id);
    EXPECT_EQ(handle.snapshot().invocation(), invocation);
    EXPECT_EQ(handle.snapshot().child_depth(), 0U);
    EXPECT_GT(sink->calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ReconnectTerminalAfterCatalogRecreationIsByteExactAndNonMutating) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version   = fixture.admit("runtime-completed");
    const auto      completed = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-terminal-reconnect", {}});
    const auto run_id = completed.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), completed.serialize_canonical());
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal_after = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after.has_value());
    EXPECT_EQ(journal_after->id, journal_before->id);
    EXPECT_EQ(journal_after->sequence, journal_before->sequence);

    const auto diagnostic_code = [&](std::string_view owner, std::string_view candidate) {
        try {
            (void)fixture.runtime->reconnect(owner, candidate);
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code("tenant:other", run_id),
              diagnostic_code("tenant:other", "absent-run"));
}

TEST(ProgramRuntimeTest, ReconnectInterruptedResumesExactCheckpointOnceWithoutInputReplay) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{
            json{{"original", "must-not-replay"}}, grant(), "trace-interrupted-reconnect", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto run_id = interrupted.run_id();
    const auto record_bytes =
        fixture.journal->load("tenant:runtime", run_id)->serialize_canonical();
    const auto journal_before = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_before.has_value());

    fixture.recreate_catalog_and_runtime();
    auto reconnected = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_EQ(reconnected.wait().serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(reconnected.snapshot().serialize_canonical(), record_bytes);
    const auto journal_after_reconnect = fixture.journal->latest("tenant:runtime", run_id);
    ASSERT_TRUE(journal_after_reconnect.has_value());
    EXPECT_EQ(journal_after_reconnect->id, journal_before->id);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", run_id,
                     resume_for(interrupted, json{{"decision", "approved"}}, "trace-exact-resume"))
            .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, TypedPendingEffectPublishesOnceAndResumesByExactCallIdentity) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-effect-interrupt");

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-interrupt", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    EXPECT_FALSE(interrupted.interrupt()->pending_input.has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.call_id(), "call-effect-1");
    EXPECT_EQ(pending.payload()["effect"]["tool"], "search");
    EXPECT_EQ(pending.state(), ProgramPendingState::Awaiting);

    const auto effects = fixture.journal->load_effects("tenant:runtime", interrupted.run_id());
    ASSERT_EQ(effects.size(), 1U);
    EXPECT_EQ(effects.front().sequence(), 1U);
    EXPECT_EQ(effects.front().effect(), pending);

    fixture.recreate_catalog_and_runtime();
    const auto reconnected =
        fixture.runtime->reconnect("tenant:runtime", interrupted.run_id()).wait();
    EXPECT_EQ(reconnected.serialize_canonical(), interrupted.serialize_canonical());
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted.run_id(),
            ProgramResume{json{{"result", "unused"}}, "trace-wrong-effect", {}, "wrong-call"});
        FAIL() << "wrong pending call identity was accepted";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_PENDING_ID_MISMATCH");
    }
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"result", "recorded"}}, "trace-effect-resume"))
            .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message +
                                    " " + resumed.failure()->witness.dump()
                                : "no failure detail");
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "recorded");
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(persisted->effect_sequence(), 1U);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", interrupted.run_id()).size(), 1U);
}

TEST(ProgramRuntimeTest, UnknownNonIdempotentEffectBlocksCancelAndRedispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-effect-nonidempotent");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-effect-ambiguous", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    const auto run_id  = interrupted.run_id();

    auto ambiguous = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Unknown,
                                                             std::nullopt,
                                                             "trace-effect-unknown",
                                                             {}})
                         .wait();
    EXPECT_EQ(ambiguous.status(), ProgramTerminalStatus::AmbiguousEffect);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    auto persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::AmbiguousEffect);
    ASSERT_TRUE(persisted->pending_effect().has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Ambiguous);
    const auto ambiguous_journal = persisted->journal_head();
    ASSERT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);

    fixture.recreate_catalog_and_runtime();
    auto recovered = fixture.runtime->reconnect("tenant:runtime", run_id);
    EXPECT_FALSE(recovered.cancel());
    EXPECT_EQ(fixture.journal->latest("tenant:runtime", run_id)->id, ambiguous_journal);
    EXPECT_EQ(interrupt_calls.load(), 1U);

    auto completed = fixture.runtime
                         ->reconcile("tenant:runtime", run_id,
                                     ProgramEffectResolution{pending.call_id(),
                                                             ProgramEffectReconciliation::Completed,
                                                             json{{"result", "reconciled"}},
                                                             "trace-effect-reconciled",
                                                             {}})
                         .wait();
    EXPECT_EQ(completed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed.output()["channels"]["value"]["value"], "reconciled");
    EXPECT_EQ(interrupt_calls.load(), 2U);
    persisted = fixture.journal->load("tenant:runtime", run_id);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->pending_effect()->state(), ProgramPendingState::Consumed);
    EXPECT_EQ(fixture.journal->load_effects("tenant:runtime", run_id).size(), 1U);
}

TEST(ProgramRuntimeTest, CrossOwnerResumeIsNoOracleAndNeverReadsCheckpoint) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-owner-scope", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    checkpoint_store->arm(AdversarialCheckpointStore::Mode::Normal);

    const auto diagnostic_code = [&](std::string_view run_id) {
        try {
            (void)fixture.runtime->resume(
                "tenant:other", run_id,
                resume_for(interrupted, json{{"decision", "unused"}}, "trace-cross-owner"));
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code;
        }
        return std::string{};
    };
    EXPECT_EQ(diagnostic_code(interrupted.run_id()), diagnostic_code("absent-run"));
    EXPECT_EQ(checkpoint_store->exact_loads(), 0U);
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, RetriesOneSpuriousTerminalJournalConflict) {
    auto            journal = std::make_shared<ConflictOnceJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-cas-retry", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(journal->injected());
    const auto latest = journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
}

TEST(ProgramRuntimeTest, JournalCommitPrecedesCheckpointAndTerminalEventDelivery) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            sink    = std::make_shared<JournalObservingSink>(fixture.journal);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-order", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(sink->checkpoint_observed_committed.load());
    EXPECT_TRUE(sink->terminal_observed_committed.load());
}

TEST(ProgramRuntimeTest, ProgramEnvelopePreservesDirectCoreBehavior) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            program_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-equivalence", {}});
    const auto program_result = program_handle.wait();

    const auto definition = program_document("runtime-completed")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    auto core_registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();

    EngineResources engine_resources;
    engine_resources.registry = core_registry;
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    direct_engine->set_checkpoint_store(direct_checkpoints);
    RunConfig direct_config;
    direct_config.thread_id = "direct-equivalence";
    direct_config.input     = json::object();
    direct_config.max_steps = 20;
    std::vector<json> direct_events;
    const auto        direct_result =
        direct_engine->run_stream(direct_config, [&](const GraphEvent& event) {
            direct_events.push_back(typed_event_value(to_typed_event(event)));
        });

    std::vector<json> program_events;
    for (const auto& event : program_handle.events_after(0)) {
        if (event.kind == ProgramEventKind::Core) {
            program_events.push_back(typed_event_value(std::get<TypedGraphEvent>(event.payload)));
        }
    }
    ASSERT_TRUE(program_result.checkpoint().has_value());
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_result.checkpoint_id);
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_result.checkpoint()->checkpoint_id);

    EXPECT_EQ(program_result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_result.status(), RunStatus::Completed);
    EXPECT_EQ(program_result.output(), direct_result.output);
    EXPECT_EQ(program_result.execution_trace(), direct_result.execution_trace);
    EXPECT_EQ(program_events, direct_events);
    EXPECT_EQ(program_result.usage().model_tokens,
              static_cast<std::uint64_t>(direct_result.usage.total_tokens));
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_result.checkpoint()->checkpoint_schema_version,
              direct_checkpoint->schema_version);
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(completed_calls.load(), 2U);
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramRuntimeTest, JavaScriptGeneratorPreservesLocalYieldValue) {
    const auto source = ProgramSource::from_javascript("test:generator-lifetime.js",
                                                       R"JS(
            export function* main(input) {
                const command = {protocol_version: 1, op: "call_core", name: "main",
                                 input: {requested: input.requested}};
                yield command;
            }
        )JS");

    const auto step = std::async(std::launch::async, [source] {
                          auto generator = neograph::program::detail::JavaScriptGenerator::open(
                              source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
                          if (!generator)
                              throw std::runtime_error("JavaScript generator was not exported");
                          std::optional<json> response;
                          return generator->next(std::move(response));
                      }).get();
    EXPECT_FALSE(step.done);
    EXPECT_EQ(step.value, (json{{"protocol_version", 1},
                                {"op", "call_core"},
                                {"name", "main"},
                                {"input", json{{"requested", "draft"}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExposesOnlyControlCommandBinding) {
    const auto source = ProgramSource::from_javascript("test:generator-host-surface.js",
                                                       R"JS(
            export function* main() {
                return {
                    graph: typeof ng.graph,
                    callCore: typeof ng.callCore,
                    frozen: Object.isFrozen(ng),
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_TRUE(step.done);
    EXPECT_EQ(step.value,
              (json{{"graph", "undefined"}, {"callCore", "function"}, {"frozen", true}}));
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorProducesSealedTypedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript(
        "test:typed-command.js",
        R"JS(
            export function* main() {
                const call = ng.callCore("main", {requested: "draft"}, "main:12");
                if (!Object.isFrozen(call)) {
                    throw new Error("command was not sealed");
                }
                yield call;
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    ASSERT_FALSE(step.done);
    ASSERT_TRUE(step.command.has_value());
    EXPECT_EQ(step.command->kind(), JavaScriptCommandKind::CallCore);
    EXPECT_EQ(step.command->import_slot(), JAVASCRIPT_IMPORT_SLOT_CALL_CORE);
    EXPECT_EQ(step.command->source_site(), "main:12");
    EXPECT_EQ(step.value, step.command->to_json());
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorRejectsForgedCommandEnvelope) {
    const auto source = ProgramSource::from_javascript(
        "test:forged-command.js",
        R"JS(
            export function* main() {
                yield {
                    protocol_version: 1,
                    kind: "call_core",
                    import_slot: 0,
                    source_site: "main:1",
                    arguments: {name: "main", input: {}}
                };
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const auto step = generator->next();
    EXPECT_FALSE(step.done);
    EXPECT_FALSE(step.command.has_value());
}

TEST(ProgramRuntimeTest, JavaScriptNgExposesClosedConstructorSet) {
    const auto source = ProgramSource::from_javascript(
        "test:command-constructors.js",
        R"JS(
            export function* main() {
                const call = ng.callCore("main", {x: 1}, "call:1");
                const spawn = ng.spawn("child", {x: 2}, "spawn:1");
                yield call;
                yield spawn;
                yield ng.await(spawn, 50, "await:1");
                yield ng.join([call, spawn], "all", undefined, "join:1");
                yield ng.all([call, spawn], "all:1");
                yield ng.race([call, spawn], "race:1");
                yield ng.quorum([call, spawn], 1, "quorum:1");
                yield ng.emit({kind: "event"}, "emit:1");
                yield ng.checkpoint({checkpoint: true}, "checkpoint:1");
                yield ng.cancelScope("run", "stop", "cancel:1");
                yield ng.hostCapability(42, {request: "approved"}, "host:1");
                return {};
            }
        )JS");

    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json::object(), JavaScriptCompileLimits{});
    ASSERT_TRUE(generator.has_value());
    const std::vector<JavaScriptCommandKind> expected = {
        JavaScriptCommandKind::CallCore,       JavaScriptCommandKind::Spawn,
        JavaScriptCommandKind::Await,          JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,           JavaScriptCommandKind::Join,
        JavaScriptCommandKind::Join,           JavaScriptCommandKind::Emit,
        JavaScriptCommandKind::Checkpoint,     JavaScriptCommandKind::CancelScope,
        JavaScriptCommandKind::HostCapability};
    for (const auto kind : expected) {
        const auto step = generator->next(json::object());
        ASSERT_FALSE(step.done);
        ASSERT_TRUE(step.command.has_value());
        EXPECT_EQ(step.command->kind(), kind);
        EXPECT_EQ(step.value, step.command->to_json());
    }
    const auto done = generator->next(json::object());
    EXPECT_TRUE(done.done);
    EXPECT_EQ(done.value, json::object());
}

TEST(ProgramRuntimeTest, JavaScriptControlCancellationInterruptsBeforeAnyLateDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main() {
                while (true) {}
            }
        )JS");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-javascript-cancel", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Cancelled)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message
                             : "no failure detail");
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptGeneratorExecutesYieldedCoreCommand) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const core = yield ng.callCore("main", {requested: input.requested});
                return {requested: input.requested, value: core.value};
            }
        )JS");
    const auto bundle = fixture.store->get_bundle("tenant:runtime", version.bundle_id());
    ASSERT_TRUE(bundle.has_value());
    const auto control_source = bundle->control_source();
    ASSERT_TRUE(control_source.has_value());
    auto restored_generator = neograph::program::detail::JavaScriptGenerator::open(
        *control_source, json{{"requested", "draft"}}, JavaScriptCompileLimits{});
    ASSERT_TRUE(restored_generator.has_value());
    const auto restored_step = restored_generator->next();
    ASSERT_FALSE(restored_step.done);

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-control", {}});

    EXPECT_EQ(version.execution_guarantee(), ExecutionGuarantee::Unmanaged);
    const auto failure = result.failure();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (failure ? failure->code + ": " + failure->message + " " + failure->witness.dump()
                    : "no failure detail");
    EXPECT_EQ(result.output(), (json{{"requested", "draft"}, {"value", "completed"}}));
    EXPECT_EQ(result.usage().program_operations, 1U);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCommandJournalPersistsCoordinateAndResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "journal:1");
                return {value: output.value};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "journal"}}, grant(),
                          "trace-javascript-command-journal", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", result.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].command_ordinal(), 1U);
    EXPECT_TRUE(entries[0].pending());
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("status"), "completed");
    EXPECT_EQ(entries[1].command().source_site(), "journal:1");
}

TEST(ProgramRuntimeTest, JavaScriptCompletedHeadFreshRuntimeReplaysRecordedResult) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(1, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:result");
                return {value: output.value};
            }
        )JS");

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "recorded"}}, grant(),
                          "trace-javascript-result-crash", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto durable_commands =
        journal->load_javascript_commands("tenant:runtime", original.run_id(), 0);
    ASSERT_EQ(durable_commands.size(), 2U);
    EXPECT_TRUE(durable_commands.back().completed());

    auto fresh_runtime = fixture.make_runtime();
    const auto replayed = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(replayed.output(), (json{{"value", "completed"}}));
    EXPECT_EQ(completed_calls.load(), 1U);

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptPendingHeadFreshRuntimeResumesWithoutRedispatch) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-blocking"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                const output = yield ng.callCore("main", {requested: input.requested}, "crash:1");
                return {value: output.value};
            }
        )JS");

    // Keep the first Core dispatch in flight at the crash boundary.  A second
    // runtime is intentionally attached to the same durable transition store,
    // as a fresh process would be after the first one stopped after publishing
    // the yielded-command head.
    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "crash"}}, grant(),
                          "trace-javascript-crash-boundary", {}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blocking_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(blocking_calls.load(), 1U);
    ASSERT_FALSE(original.try_result().has_value());

    const auto pending_entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(pending_entries.size(), 1U);
    ASSERT_TRUE(pending_entries.front().pending());

    auto fresh_runtime = fixture.make_runtime();
    auto recovered = fresh_runtime->reconnect("tenant:runtime", original.run_id());
    const auto interrupted = recovered.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.interrupt().has_value());
    ASSERT_TRUE(interrupted.interrupt()->pending_effect.has_value());
    const auto pending = *interrupted.interrupt()->pending_effect;
    EXPECT_EQ(pending.idempotency(), ProgramEffectIdempotency::NonIdempotent);
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto resumed = fresh_runtime
                             ->resume("tenant:runtime", original.run_id(),
                                      resume_for(interrupted, json{{"value", "resumed"}},
                                                 "trace-javascript-crash-resume"))
                             .wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Completed)
        << (resumed.failure() ? resumed.failure()->code + ": " + resumed.failure()->message +
                                    " " + resumed.failure()->witness.dump()
                                : "no failure detail");
    EXPECT_EQ(resumed.output(), (json{{"value", "resumed"}}));
    EXPECT_EQ(blocking_calls.load(), 1U);

    const auto entries =
        fixture.journal->load_javascript_commands("tenant:runtime", original.run_id());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].coordinate_id(), entries[1].coordinate_id());
    EXPECT_TRUE(entries[1].completed());
    EXPECT_EQ(entries[1].terminal_result()->at("output"),
              (json{{"value", "resumed"}}));

    // Stop the original process simulation after the fresh runtime has
    // durably completed.  Its result loses the transition race and must not
    // cause a second Core dispatch.
    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptControlRejectsUnknownCommandFieldsBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto      version = fixture.admit_javascript(
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
                graph.node("work", {type: "runtime-completed"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }

            export function* main(input) {
                yield {
                    protocol_version: 1,
                    op: "call_core",
                    name: "main",
                    input: {requested: input.requested},
                    unexpected: true,
                };
                return {};
            }
        )JS");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json{{"requested", "draft"}}, grant(), "trace-javascript-invalid", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

std::string javascript_runtime_source(std::string node_type, std::string body) {
    return "export function define() {\n"
           "  const graph = ng.graph(\"main\");\n"
           "  graph.channel(\"value\", {reducer: \"runtime-overwrite\", initial: \"\"});\n"
           "  graph.node(\"work\", {type: \"" + std::move(node_type) + "\"});\n"
           "  graph.entry(\"work\");\n"
           "  graph.exit(\"work\");\n"
           "  return graph;\n"
           "}\n\n"
           "export function* main(input) {\n" + std::move(body) + "\n}\n";
}

RunBudget javascript_budget(std::uint64_t max_concurrency = 2,
                            std::uint64_t max_program_operations = 32,
                            std::uint64_t max_child_depth = 0,
                            std::uint64_t max_total_children = 0) {
    return RunBudget{10000,
                     1000,
                     1000,
                     static_cast<std::uint32_t>(max_concurrency),
                     max_program_operations,
                     20,
                     0,
                     static_cast<std::uint32_t>(max_child_depth),
                     max_total_children};
}

TEST(ProgramRuntimeTest, JavaScriptAllJoinsActuallyOverlapCoreCommands) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-short-blocking",
        R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto started = std::chrono::steady_clock::now();
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-overlap", {}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_LT(elapsed, std::chrono::milliseconds(900));
}

TEST(ProgramRuntimeTest, JavaScriptJoinEnforcesMaxInFlightCap) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-short-blocking",
        R"JS(
    const result = yield ng.all([
        ng.callCore("main", {}, "first"),
        ng.callCore("main", {}, "second"),
        ng.callCore("main", {}, "third")
    ], {max_in_flight: 2}, "all");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(3), "trace-js-cap", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 3U);
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinValidatesEveryMemberBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    yield ng.all([
        ng.callCore("main", {}, "valid:first"),
        ng.callCore("not-admitted", {}, "invalid:second")
    ], {max_in_flight: 2}, "validate");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-validate", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_CONTROL_COMMAND");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinReservesAggregateOperationBudgetBeforeDispatch) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    yield ng.all([
        ng.callCore("main", {}, "budget:first"),
        ng.callCore("main", {}, "budget:second")
    ], {max_in_flight: 2}, "budget");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2, 2), "trace-js-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_PROGRAM_OPERATION_BUDGET");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinUsesDeclarationOrderForResultsAndReadyRaceTies) {
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    const all_result = yield ng.all([
        ng.emit({id: 1}, "all:first"),
        ng.emit({id: 2}, "all:second")
    ], {max_in_flight: 2, collect: true}, "all");
    const race_result = yield ng.race([
        ng.emit({id: 1}, "race:first"),
        ng.emit({id: 2}, "race:second")
    ], "race");
    return {all: all_result, race: race_result};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-order", {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                              : "no failure detail");
    EXPECT_EQ(result.output(),
              (json{{"all", json::array({json{{"id", 1}}, json{{"id", 2}}})},
                    {"race", json{{"id", 1}}}}));
}

TEST(ProgramRuntimeTest, JavaScriptStructuredCommandReplaysAfterFreshRuntimeWithoutRedispatch) {
    completed_calls.store(0);
    auto journal = std::make_shared<BlockAfterJavaScriptResultJournal>();
    AdmittedRuntime fixture(2, {}, journal, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    const results = yield ng.all([
        ng.callCore("main", {member: 1}, "replay:first"),
        ng.callCore("main", {member: 2}, "replay:second")
    ], {max_in_flight: 2}, "replay:join");
    return {values: results.map((result) => result.value)};
)JS"));

    auto original = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2),
                          "trace-js-structured-result-crash", {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    EXPECT_EQ(completed_calls.load(), 2U);

    auto fresh_runtime = fixture.make_runtime();
    const auto replayed = fresh_runtime->reconnect("tenant:runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed)
        << (replayed.failure() ? replayed.failure()->code + ": " +
                                     replayed.failure()->message
                               : "no failure detail");
    EXPECT_EQ(replayed.output(),
              (json{{"values", json::array({"completed", "completed"})}}));
    EXPECT_EQ(completed_calls.load(), 2U);

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JavaScriptJoinCollectsFailuresInDeclarationOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-failing",
        R"JS(
    const result = yield ng.all([
        ng.emit({id: "ok"}, "collect:ok"),
        ng.callCore("main", {}, "collect:failure")
    ], {max_in_flight: 2, collect: true}, "collect");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-collect", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.output(), json::array({json{{"id", "ok"}}, nullptr}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, JavaScriptAwaitTimeoutCancelsDirectCoreWork) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-blocking",
        R"JS(
    const result = yield ng.await(ng.callCore("main", {}, "await:core"), 10, "await");
    return result;
)JS"));

    const auto started = std::chrono::steady_clock::now();
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-await-timeout", {}});
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_LT(elapsed.count(), 2);
}

TEST(ProgramRuntimeTest, JavaScriptQuorumRunsMembersConcurrentlyAndOrdersSuccesses) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-short-blocking",
        R"JS(
    const result = yield ng.quorum([
        ng.emit({id: 1}, "quorum:first"),
        ng.emit({id: 2}, "quorum:second"),
        ng.callCore("main", {}, "quorum:third")
    ], {required_successes: 2, max_in_flight: 2}, "quorum");
    return result;
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(2), "trace-js-quorum", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"id", 1}}, json{{"id", 2}}}));
    EXPECT_EQ(blocking_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, JavaScriptCancelScopeCancelsTheOwningRun) {
    AdmittedRuntime fixture(1, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    yield ng.cancelScope("current", "stop", "scope stopped");
    return {unreachable: true};
)JS"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), javascript_budget(1), "trace-js-cancel-scope", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
}

TEST(ProgramRuntimeTest, JavaScriptRaceCancelsLoserChildrenWithoutOrphans) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(3, {}, {}, {}, ExecutionGuarantee::Unmanaged, true);
    const auto parent_version = fixture.admit_javascript(javascript_runtime_source(
        "runtime-completed",
        R"JS(
    const winner = yield ng.race([
        ng.await(ng.spawn("child", {}, "spawn"), 5000, "await"),
        ng.emit({winner: true}, "winner")
    ], {max_in_flight: 2}, "race");
    return winner;
)JS"));
    const auto child_version = fixture.admit("runtime-blocking");
    const auto linked = link_child_versions(
        fixture, parent_version, child_version,
        BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1});

    auto parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{json::object(), javascript_budget(2, 32, 1, 1),
                           "trace-js-cancel-child", {}});
    const auto result = parent.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"winner", true}}));
    const auto children = parent.snapshot().children();
    ASSERT_EQ(children.size(), 1U);
    EXPECT_EQ(children.front().state, ProgramChildState::Cancelled);
}
#endif

TEST(ProgramRuntimeTest, ProgramInterruptAndExactResumePreserveDirectCoreBehavior) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    const auto program_interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-program-interrupt", {}});
    const auto program_resumed =
        fixture.runtime
            ->resume("tenant:runtime", program_interrupted.run_id(),
                     resume_for(program_interrupted, json{{"decision", "approved"}},
                                "trace-program-resume"))
            .wait();

    const auto definition = program_document("runtime-interrupt")["root"]["definition"];
    auto topology = neograph::program::detail::RegistrySnapshotAccess::parse_local(fixture.registry,
                                                                                   definition);
    auto linked   = neograph::program::detail::RegistrySnapshotAccess::link_local(
        fixture.registry, std::move(topology), NodeContext{});
    EngineResources engine_resources;
    engine_resources.registry =
        neograph::program::detail::RegistrySnapshotAccess::runtime_registry(fixture.registry);
    auto direct_engine =
        GraphEngine::link(std::move(linked), EngineConfig{}, std::move(engine_resources));
    auto direct_checkpoints = std::make_shared<InMemoryCheckpointStore>();
    direct_engine->set_checkpoint_store(direct_checkpoints);

    RunConfig direct_config;
    direct_config.thread_id       = "direct-interrupt-equivalence";
    direct_config.input           = json::object();
    direct_config.max_steps       = 20;
    const auto direct_interrupted = direct_engine->run(direct_config);
    const auto direct_resumed     = direct_engine->resume_from(
        direct_config, direct_interrupted.checkpoint_id, json{{"decision", "approved"}});

    ASSERT_EQ(program_interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_EQ(direct_interrupted.status(), RunStatus::Interrupted);
    ASSERT_TRUE(program_interrupted.interrupt().has_value());
    EXPECT_EQ(program_interrupted.interrupt()->core_node, direct_interrupted.interrupt_node);
    EXPECT_EQ(program_interrupted.interrupt()->value, direct_interrupted.interrupt_value);
    EXPECT_EQ(program_interrupted.execution_trace(), direct_interrupted.execution_trace);
    EXPECT_EQ(program_resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(direct_resumed.status(), RunStatus::Completed);
    EXPECT_EQ(program_resumed.output(), direct_resumed.output);
    EXPECT_EQ(program_resumed.execution_trace(), direct_resumed.execution_trace);

    ASSERT_TRUE(program_resumed.checkpoint().has_value());
    const auto program_checkpoint =
        fixture.checkpoints->load_by_id(program_resumed.checkpoint()->checkpoint_id);
    const auto direct_checkpoint = direct_checkpoints->load_by_id(direct_resumed.checkpoint_id);
    ASSERT_TRUE(program_checkpoint.has_value());
    ASSERT_TRUE(direct_checkpoint.has_value());
    EXPECT_EQ(program_checkpoint->channel_values, direct_checkpoint->channel_values);
    EXPECT_EQ(program_checkpoint->channel_versions, direct_checkpoint->channel_versions);
    EXPECT_EQ(program_checkpoint->current_node, direct_checkpoint->current_node);
    EXPECT_EQ(program_checkpoint->next_nodes, direct_checkpoint->next_nodes);
    EXPECT_EQ(program_checkpoint->interrupt_phase, direct_checkpoint->interrupt_phase);
    EXPECT_EQ(program_checkpoint->barrier_state, direct_checkpoint->barrier_state);
    EXPECT_EQ(program_checkpoint->step, direct_checkpoint->step);
    EXPECT_EQ(interrupt_calls.load(), 4U);
}

TEST(ProgramRuntimeTest, ExactResumeAtEndChargesNoAdditionalCoreStep) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document                          = program_document("runtime-completed");
    document["root"]["definition"]["interrupt_after"] = json::array({"work"});
    const auto version                                = fixture.admit_document(std::move(document));

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-after", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    ASSERT_EQ(completed_calls.load(), 1U);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json::object(), "trace-after-resume"))
                             .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.usage().core_steps, 0U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps);
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ExactResumeToInterruptBeforeChargesOnlyExecutedSteps) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    auto            document       = program_document("runtime-completed");
    auto            core           = document["root"]["definition"];
    core["nodes"]                  = json{{"a", json{{"type", "runtime-completed"}}},
                                          {"b", json{{"type", "runtime-completed"}}},
                                          {"c", json{{"type", "runtime-completed"}}}};
    core["edges"]                  = json::array({
        json{{"from", "__start__"}, {"to", "a"}},
        json{{"from", "a"}, {"to", "b"}},
        json{{"from", "b"}, {"to", "c"}},
        json{{"from", "c"}, {"to", "__end__"}},
    });
    core["interrupt_after"]        = json::array({"a"});
    core["interrupt_before"]       = json::array({"c"});
    document["root"]["definition"] = std::move(core);
    const auto version             = fixture.admit_document(std::move(document));
    auto       budget              = grant();
    budget.max_core_steps          = 3;

    const auto first =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-sequence-1", {}});
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(first.usage().core_steps, 1U);
    EXPECT_EQ(first.remaining_budget().max_core_steps, 2U);

    const auto second = fixture.runtime
                            ->resume("tenant:runtime", first.run_id(),
                                     resume_for(first, json::object(), "trace-sequence-2"))
                            .wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(second.usage().core_steps, 1U);
    EXPECT_EQ(second.remaining_budget().max_core_steps, 1U);

    const auto third = fixture.runtime
                           ->resume("tenant:runtime", second.run_id(),
                                    resume_for(second, json::object(), "trace-sequence-3"))
                           .wait();
    EXPECT_EQ(third.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(third.usage().core_steps, 1U);
    EXPECT_EQ(third.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(completed_calls.load(), 3U);
}

TEST(ProgramRuntimeTest, InterruptAndAsyncResumeUseOnePublishedCheckpointAndOneDispatchEach) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");

    auto interrupted_handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-interrupt", {}});
    const auto interrupted = interrupted_handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);

    const auto resumed = neograph::async::run_sync(fixture.runtime->resume_async(
        "tenant:runtime", interrupted.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-resume")));

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.run_id(), interrupted.run_id());
    EXPECT_EQ(resumed.program_version_id(), version.id());
    EXPECT_EQ(resumed.attempt(), 2U);
    EXPECT_EQ(interrupt_calls.load(), 2U);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "approved");
    ASSERT_TRUE(resumed.checkpoint().has_value());
    EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
    const auto interrupted_budget = interrupted.remaining_budget();
    const auto resumed_budget     = resumed.remaining_budget();
    EXPECT_LE(resumed_budget.wall_time_ms, interrupted_budget.wall_time_ms);
    EXPECT_LE(resumed_budget.model_tokens, interrupted_budget.model_tokens);
    EXPECT_EQ(interrupted_budget.max_program_operations, 0U);
    EXPECT_EQ(resumed_budget.max_program_operations, 0U);
    EXPECT_EQ(interrupted_budget.max_concurrency, 1U);
    EXPECT_EQ(resumed_budget.max_concurrency, 1U);
    EXPECT_LE(resumed_budget.monetary_microunits, interrupted_budget.monetary_microunits);
    EXPECT_LE(resumed_budget.max_concurrency, interrupted_budget.max_concurrency);
    EXPECT_LE(resumed_budget.max_program_operations, interrupted_budget.max_program_operations);
    EXPECT_LE(resumed_budget.max_core_steps, interrupted_budget.max_core_steps);
    EXPECT_LE(resumed_budget.max_dynamic_compiles, interrupted_budget.max_dynamic_compiles);
    EXPECT_LE(resumed_budget.max_child_depth, interrupted_budget.max_child_depth);
    EXPECT_LE(resumed_budget.max_total_children, interrupted_budget.max_total_children);

    const auto journal = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(journal->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, AsyncWaitFromTemporaryHandleRetainsRunControl) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    auto pending =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-temporary-handle", {}})
            .wait_async();
    const auto result = neograph::async::run_sync(std::move(pending));

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.program_version_id(), version.id());
}

TEST(ProgramRuntimeTest, RepeatedAsyncWaitNeverMissesConcurrentCompletion) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");

    for (int iteration = 0; iteration < 2000; ++iteration) {
        auto pending =
            fixture.runtime
                ->start("tenant:runtime", version,
                        ProgramInvocation{json::object(), grant(), "trace-wait-race", {}})
                .wait_async();
        const auto result = neograph::async::run_sync(std::move(pending));
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << iteration;
    }
}

TEST(ProgramRuntimeTest, ConcurrentResumeHasOneCasWinnerAndOneCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-race-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    std::barrier ready(3);
    const auto   resume_once = [&](std::string decision) {
        ready.arrive_and_wait();
        try {
            auto handle = fixture.runtime->resume(
                "tenant:runtime", interrupted.run_id(),
                resume_for(interrupted, json{{"decision", std::move(decision)}}, "trace-race"));
            return handle.wait().status() == ProgramTerminalStatus::Completed ? 1 : -1;
        } catch (const ProgramDiagnosticError& error) {
            return error.diagnostic().code == "P_RESUME_CONFLICT" ? 0 : -2;
        } catch (...) {
            return -3;
        }
    };

    auto first  = std::async(std::launch::async, [&] { return resume_once("first"); });
    auto second = std::async(std::launch::async, [&] { return resume_once("second"); });
    ready.arrive_and_wait();
    const auto first_result  = first.get();
    const auto second_result = second.get();

    EXPECT_EQ(first_result + second_result, 1);
    EXPECT_NE(first_result, second_result);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, EventSinkFailureCannotRewriteAnAtomicallyCommittedTerminalTransition) {
    for (const auto fail_on : {ProgramEventKind::Started, ProgramEventKind::Core,
                               ProgramEventKind::CheckpointPublished, ProgramEventKind::Terminal}) {
        AdmittedRuntime fixture;
        const auto      version = fixture.admit("runtime-completed");
        auto            sink    = std::make_shared<ThrowingSink>(fail_on);

        auto handle =
            fixture.runtime->start("tenant:runtime", version,
                                   ProgramInvocation{json::object(), grant(), "trace-sink", sink});
        const auto result = handle.wait();
        const bool pre_commit =
            fail_on == ProgramEventKind::Started || fail_on == ProgramEventKind::Core;

        EXPECT_EQ(result.status(),
                  pre_commit ? ProgramTerminalStatus::Failed : ProgramTerminalStatus::Completed);
        EXPECT_EQ(result.failure().has_value(), pre_commit);
        if (pre_commit) EXPECT_EQ(result.failure()->code, "P_EVENT_SINK");
        EXPECT_EQ(result.usage().program_operations, 1U);
        EXPECT_EQ(result.usage().core_steps, fail_on == ProgramEventKind::Started ? 0U : 1U);
        EXPECT_EQ(result.remaining_budget().max_program_operations, 0U);
        EXPECT_EQ(result.remaining_budget().max_core_steps,
                  fail_on == ProgramEventKind::Started ? 20U : 19U);

        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state,
                  pre_commit ? ContinuationState::Failed : ContinuationState::Completed);

        const auto events   = handle.events_after(0);
        const auto terminal = std::find_if(
            events.begin(), events.end(),
            [](const ProgramEvent& event) { return event.kind == ProgramEventKind::Terminal; });
        ASSERT_NE(terminal, events.end());
        EXPECT_EQ(std::get<ProgramTerminalEvent>(terminal->payload).status, result.status());
        EXPECT_EQ(std::count_if(events.begin(), events.end(),
                                [](const ProgramEvent& event) {
                                    return event.kind == ProgramEventKind::Terminal;
                                }),
                  1);

        const auto terminal_count =
            std::count(sink->seen.begin(), sink->seen.end(), ProgramEventKind::Terminal);
        EXPECT_LE(terminal_count, 1);
    }
}

TEST(ProgramRuntimeTest, ResumeStartedSinkFailurePreservesPublishedCheckpointAndJournal) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-resume-sink-start", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    auto sink = std::make_shared<ThrowingSink>(ProgramEventKind::Started);

    const auto failed = fixture.runtime
                            ->resume("tenant:runtime", interrupted.run_id(),
                                     resume_for(interrupted, json{{"decision", "unused"}},
                                                "trace-resume-sink-failed", sink))
                            .wait();

    EXPECT_EQ(failed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(failed.failure().has_value());
    EXPECT_EQ(failed.failure()->code, "P_EVENT_SINK");
    ASSERT_TRUE(failed.checkpoint().has_value());
    EXPECT_EQ(failed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", failed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(latest->continuation.attempt, 2U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
}

TEST(ProgramRuntimeTest, InterruptedCheckpointSinkFailurePreservesCommittedResumeState) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            sink    = std::make_shared<ThrowingSink>(ProgramEventKind::CheckpointPublished);

    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-interrupt-sink", sink});

    EXPECT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_FALSE(interrupted.failure().has_value());
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::Interrupted);
    EXPECT_EQ(latest->continuation.attempt, 1U);
    ASSERT_TRUE(latest->core_checkpoint.has_value());
    EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);

    const auto resumed = fixture.runtime
                             ->resume("tenant:runtime", interrupted.run_id(),
                                      resume_for(interrupted, json{{"decision", "approved"}},
                                                 "trace-resume-after-observer-failure"))
                             .wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, JournalLatestFailureIsContainedAsTerminalPublicationFailure) {
    auto            journal = std::make_shared<ThrowOnLatestJournal>();
    AdmittedRuntime fixture(1, {}, journal);
    const auto      version = fixture.admit("runtime-completed");

    auto handle = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-journal-read-failure", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JOURNAL_CONFLICT");
    const auto stored = journal->stored_latest(result.run_id());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->continuation.state, ContinuationState::Running);
    const auto events = handle.events_after(0);
    EXPECT_EQ(std::count_if(events.begin(), events.end(),
                            [](const ProgramEvent& event) {
                                return event.kind == ProgramEventKind::CheckpointPublished ||
                                       event.kind == ProgramEventKind::Terminal;
                            }),
              0);
}

TEST(ProgramRuntimeTest, ResumeUsesRequestedCheckpointWhenANewerOrphanExists) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-orphan", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id         = Checkpoint::generate_id();
    orphan->parent_id  = interrupted.checkpoint()->checkpoint_id;
    orphan->next_nodes = {"__end__"};
    orphan->step += 1;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "exact"}}, "trace-orphan-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.output()["channels"]["value"]["value"], "exact");
    EXPECT_EQ(interrupt_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, MissingPublishedCheckpointIsTypedIncompatibleWithoutCoreDispatch) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-missing", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());
    fixture.checkpoints->delete_thread(interrupted.checkpoint()->core_thread_id);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-missing-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    EXPECT_EQ(latest->continuation.attempt, 2U);
}

TEST(ProgramRuntimeTest, MismatchedPublishedCheckpointIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-mismatch", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::WrongThread);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-mismatch-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, CheckpointDeletedAfterPrecheckIsTypedWithoutCoreDispatch) {
    interrupt_calls.store(0);
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    const auto      version     = fixture.admit("runtime-interrupt");
    const auto      interrupted = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-race", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterFirstExactLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json{{"decision", "unused"}}, "trace-race-resume"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
    EXPECT_EQ(interrupt_calls.load(), 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
}

TEST(ProgramRuntimeTest, PostPrecheckIdentityChangesPreservePublishedCheckpoint) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongThreadAfterPrecheck,
                            AdversarialCheckpointStore::Mode::WrongSchemaAfterPrecheck}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-post-precheck", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-post-precheck-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(resumed.checkpoint()->core_thread_id, interrupted.checkpoint()->core_thread_id);
        EXPECT_EQ(resumed.checkpoint()->checkpoint_schema_version,
                  interrupted.checkpoint()->checkpoint_schema_version);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
        ASSERT_TRUE(latest->core_checkpoint.has_value());
        EXPECT_EQ(latest->core_checkpoint->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
    }
}

TEST(ProgramRuntimeTest, CoreExactResumeRejectsIdentityChangedAtConsumingLoad) {
    for (const auto mode : {AdversarialCheckpointStore::Mode::WrongIdAtCoreLoad,
                            AdversarialCheckpointStore::Mode::WrongSchemaAtCoreLoad}) {
        interrupt_calls.store(0);
        auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
        AdmittedRuntime fixture(1, checkpoint_store);
        const auto      version     = fixture.admit("runtime-interrupt");
        const auto      interrupted = fixture.runtime->run(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), grant(), "trace-core-identity", {}});
        ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
        ASSERT_TRUE(interrupted.checkpoint().has_value());

        checkpoint_store->arm(mode);
        const auto resumed = fixture.runtime
                                 ->resume("tenant:runtime", interrupted.run_id(),
                                          resume_for(interrupted, json{{"decision", "unused"}},
                                                     "trace-core-identity-resume"))
                                 .wait();

        EXPECT_EQ(resumed.status(), ProgramTerminalStatus::CheckpointIncompatible);
        ASSERT_TRUE(resumed.failure().has_value());
        EXPECT_EQ(resumed.failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
        ASSERT_TRUE(resumed.checkpoint().has_value());
        EXPECT_EQ(resumed.checkpoint()->checkpoint_id, interrupted.checkpoint()->checkpoint_id);
        EXPECT_EQ(interrupt_calls.load(), 1U);
        const auto latest = fixture.journal->latest("tenant:runtime", resumed.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::CheckpointIncompatible);
    }
}

TEST(ProgramRuntimeTest, FailedResumeUsesAttemptLocalCoreStepAccounting) {
    auto            checkpoint_store = std::make_shared<AdversarialCheckpointStore>();
    AdmittedRuntime fixture(1, checkpoint_store);
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version     = fixture.admit_document(std::move(document));
    const auto interrupted = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failure-before", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    checkpoint_store->arm(AdversarialCheckpointStore::Mode::MissingAfterCoreLoad);
    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-failure-after"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutChargesOneCoreSuperstep) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit_document(failing_fanout_program_document());

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-failing-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedFanoutCountsLegalDoubleUnderscoreNodeNames) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version =
        fixture.admit_document(failing_fanout_program_document("__completed", "__failing"));

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-prefixed-fanout", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(completed_calls.load(), 1U);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, grant().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, FailedExactResumeIgnoresNewerOrphanForUsage) {
    AdmittedRuntime fixture;
    auto            document                           = program_document("runtime-failing");
    document["root"]["definition"]["interrupt_before"] = json::array({"work"});
    const auto version = fixture.admit_document(std::move(document));
    const auto interrupted =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), grant(), "trace-orphan-source", {}});
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    auto orphan = fixture.checkpoints->load_by_id(interrupted.checkpoint()->checkpoint_id);
    ASSERT_TRUE(orphan.has_value());
    orphan->id = "unpublished-newer-orphan";
    orphan->step += 10;
    orphan->timestamp += 10;
    fixture.checkpoints->save(*orphan);

    const auto resumed =
        fixture.runtime
            ->resume("tenant:runtime", interrupted.run_id(),
                     resume_for(interrupted, json::object(), "trace-orphan-failure"))
            .wait();

    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(resumed.failure().has_value());
    EXPECT_EQ(resumed.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(resumed.usage().core_steps, 1U);
    EXPECT_EQ(resumed.remaining_budget().max_core_steps,
              interrupted.remaining_budget().max_core_steps - 1);
}

TEST(ProgramRuntimeTest, CoreFailureIsClassifiedWithNodeAndAttempt) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-failing");

    const auto result = fixture.runtime->run(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-failure", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->core_node, "work");
    EXPECT_EQ(result.failure()->attempts, 1U);
    const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
}

TEST(ProgramRuntimeTest, CoreStepLimitMapsToBudgetExhausted) {
    completed_calls.store(0);

    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(step_limited_program_document());
    auto            budget  = grant();
    budget.max_core_steps   = 1;

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(), budget, "trace-step-budget", {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 0U);
    EXPECT_EQ(completed_calls.load(), 1U);
    const auto journal = fixture.journal->latest("tenant:runtime", result.run_id());
    ASSERT_TRUE(journal.has_value());
    EXPECT_EQ(journal->continuation.state, ContinuationState::BudgetExhausted);
}

TEST(ProgramRuntimeTest, RejectsCoreStepBudgetThatCannotFitCoreRunConfig) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-completed");
    auto            budget  = grant();
    budget.max_core_steps   = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, RejectsWallTimeBudgetThatCannotFitTimerDuration) {
    completed_calls.store(0);
    AdmittedRuntime       fixture;
    const auto            maximum = std::numeric_limits<std::uint64_t>::max();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("runtime-policy-large-wall")
        .semantic_version("1.0.0")
        .owner_scope("tenant:runtime")
        .admission_profile(fixture.profile)
        .budget_ceiling(BudgetLimits{maximum, 1000, 1000, 1, 1, 20, 1, 1, 1});
    fixture.policy                                         = std::move(policy_builder).build();
    auto document                                          = program_document("runtime-completed");
    document["declared_budget_requirements"][0]["maximum"] = maximum;
    const auto version  = fixture.admit_document(std::move(document));
    auto       budget   = grant();
    budget.wall_time_ms = maximum;

    try {
        (void)fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{json::object(), budget, "trace-wall-overflow", {}});
        FAIL() << "Expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_START_BUDGET");
    }
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, WallTimeBudgetCancelsCoreAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), budget, "trace-timeout", {}});
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, 19U);
    EXPECT_FALSE(handle.cancel());
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ImmediateCancellationSuppressesAllLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-cancel-immediate", {}});

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_LE(blocking_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationWinsAndSkipsLaterNodes) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version, ProgramInvocation{json::object(), grant(), "trace-cancel", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    handle.cancel();
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, HostAdmissionQueuesProgramAttemptsUntilLeaseRelease) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-short-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});

    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext& context) {
        EXPECT_EQ(context.owner_scope, "tenant:runtime");
        EXPECT_EQ(context.operation_id, "root");
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        request.priority             = 17;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-first", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto second =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-host-second", {}});
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_EQ(host->snapshot().queued, 1U);

    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(second.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(blocking_calls.load(), 2U);
    EXPECT_EQ(followup_calls.load(), 2U);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionDeadlineCancelsQueuedAttemptBeforeCoreDispatch) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      version = fixture.admit("runtime-blocking");

    const auto host_profile =
        neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
            "runtime-host-timeout-v1",
            neograph::HostResourceVector{.cpu_millis = 1},
            {},
            neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1,
                                           false}});
    auto host = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 2};
    config.host_admission          = host;
    config.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        neograph::HostAdmissionRequest request;
        request.resources.cpu_millis = 1;
        return request;
    };
    fixture.runtime = std::make_unique<ProgramRuntime>(std::move(config));

    auto first = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-host-timeout-holder", {}});
    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    auto budget         = grant();
    budget.wall_time_ms = 30;
    auto queued         = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-host-timeout-queued", {}});
    const auto queued_result = queued.wait();

    EXPECT_EQ(queued_result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued_result.failure().has_value());
    EXPECT_EQ(queued_result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued_result.usage().core_steps, 0U);
    EXPECT_EQ(blocking_calls.load(), 1U);
    EXPECT_TRUE(host->snapshot().reserved.cpu_millis == 1);
    EXPECT_EQ(host->snapshot().queued, 0U);

    EXPECT_TRUE(first.cancel());
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(host->snapshot().reserved.empty());
}

TEST(ProgramRuntimeTest, HostAdmissionRequiresControllerAndResolverTogether) {
    AdmittedRuntime fixture;
    auto            host = std::make_shared<neograph::HostAdmissionController>();

    RuntimeConfig controller_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    controller_only.host_admission = host;
    EXPECT_THROW({ ProgramRuntime runtime(std::move(controller_only)); }, std::invalid_argument);

    RuntimeConfig resolver_only{
        fixture.catalog, fixture.checkpoints, {}, fixture.journal, fixture.scheduler_thread_count};
    resolver_only.host_admission_resolver = [](const ProgramHostAdmissionContext&) {
        return neograph::HostAdmissionRequest{};
    };
    EXPECT_THROW({ ProgramRuntime runtime(std::move(resolver_only)); }, std::invalid_argument);
}

TEST(ProgramRuntimeTest, QueueWaitCountsAgainstWallTimeBudget) {
    scheduler_blocking_calls.store(0);
    completed_calls.store(0);
    AdmittedRuntime fixture(1);
    const auto      blocker_version = fixture.admit("runtime-scheduler-blocking");
    const auto      queued_version  = fixture.admit("runtime-completed");

    auto blocker_budget         = grant();
    blocker_budget.wall_time_ms = 5000;
    auto blocker                = fixture.runtime->start(
        "tenant:runtime", blocker_version,
        ProgramInvocation{json::object(), blocker_budget, "trace-queue-blocker", {}});
    for (int i = 0; i < 100 && scheduler_blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(scheduler_blocking_calls.load(), 1U);

    auto queued_budget         = grant();
    queued_budget.wall_time_ms = 20;
    const auto queued =
        fixture.runtime->run("tenant:runtime", queued_version,
                             ProgramInvocation{json::object(), queued_budget, "trace-queued", {}});

    EXPECT_EQ(queued.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(queued.failure().has_value());
    EXPECT_EQ(queued.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(queued.usage().core_steps, 0U);
    EXPECT_EQ(queued.remaining_budget().wall_time_ms, 0U);
    EXPECT_EQ(completed_calls.load(), 0U);
    EXPECT_EQ(blocker.wait().status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, WallTimeExpiresWhenSingleSchedulerThreadIsBlocked) {
    scheduler_blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-scheduler-blocking");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;

    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-scheduler-blocked", {}});

    EXPECT_EQ(scheduler_blocking_calls.load(), 1U);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(result.usage().core_steps, 1U);
    EXPECT_EQ(result.remaining_budget().max_core_steps, budget.max_core_steps - 1);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, UserCancellationRemainsFirstCauseAfterTimeoutAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 500;
    auto handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), budget, "trace-user-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(stubborn_calls.load(), 1U);
    ASSERT_TRUE(handle.cancel());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CANCELLED");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, TimeoutRemainsFirstCauseAfterUserCancellationAttempts) {
    stubborn_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-stubborn");
    auto            budget  = grant();
    budget.wall_time_ms     = 30;
    auto handle             = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), budget, "trace-timeout-first", {}});

    for (int i = 0; i < 100 && stubborn_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A 30ms budget may expire before the scheduler reaches the first node
    // on a cold or sanitizer-instrumented process. That is still the
    // timeout-first path under test; the user-first companion above covers
    // the case where the stubborn node has entered.
    EXPECT_LE(stubborn_calls.load(), 1U);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(handle.cancel());
    const auto result = handle.wait();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_FALSE(handle.cancel());
}

TEST(ProgramRuntimeTest, HandleCopiesShareOneResultAndRuntimeTeardownDrainsWork) {
    blocking_calls.store(0);
    followup_calls.store(0);
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            first =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-teardown", sink});
    auto second = first;

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);
    fixture.runtime.reset();

    const auto first_result  = first.wait();
    const auto second_result = second.wait();
    EXPECT_EQ(first_result.run_id(), second_result.run_id());
    EXPECT_EQ(first_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(second_result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
    const auto callbacks_after_teardown = sink->calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(sink->calls.load(), callbacks_after_teardown);
}

TEST(ProgramRuntimeTest, MoveAssignmentDrainsTheReplacedRuntime) {
    blocking_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-blocking");
    auto            handle =
        fixture.runtime->start("tenant:runtime", version,
                               ProgramInvocation{json::object(), grant(), "trace-move-assign", {}});

    for (int i = 0; i < 100 && blocking_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(blocking_calls.load(), 1U);

    ProgramRuntime replacement(
        RuntimeConfig{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1});
    *fixture.runtime = std::move(replacement);

    const auto result = handle.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(followup_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ConcurrentAttemptsShareConfiguredSchedulerWithoutCrossTalk) {
    completed_calls.store(0);
    AdmittedRuntime            fixture(4);
    const auto                 version = fixture.admit("runtime-completed");
    std::vector<ProgramHandle> handles;
    handles.reserve(24);
    for (int i = 0; i < 24; ++i) {
        handles.push_back(fixture.runtime->start(
            "tenant:runtime", version,
            ProgramInvocation{
                json::object(), grant(), "trace-concurrent-" + std::to_string(i), {}}));
    }

    for (auto& handle : handles) {
        const auto result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
        const auto latest = fixture.journal->latest("tenant:runtime", result.run_id());
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->continuation.state, ContinuationState::Completed);
    }
    EXPECT_EQ(completed_calls.load(), 24U);
}

TEST(ProgramRuntimeTest, RuntimeShutdownPreservesDeliveredInterruptedRun) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    auto            handle  = fixture.runtime->start(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), grant(), "trace-shutdown-interrupt", {}});
    const auto interrupted = handle.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(interrupted.checkpoint().has_value());

    fixture.recreate_catalog_and_runtime();

    const auto persisted = fixture.journal->load("tenant:runtime", interrupted.run_id());
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(persisted->terminal_result().has_value());
    EXPECT_EQ(persisted->terminal_result()->status(), ProgramTerminalStatus::Interrupted);
    EXPECT_TRUE(persisted->pending_input().has_value());
}

TEST(ProgramRuntimeTest, ExactForkAfterRestartResumesPublishedCheckpointAndPersistsLineage) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    ASSERT_EQ(interrupt_calls.load(), 1U);
    const auto source_record = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_record.has_value());
    ASSERT_EQ(source_record->continuation().state, ContinuationState::Interrupted);
    ASSERT_TRUE(source_record->pending_input().has_value());
    const auto source_pending_id = source_record->pending_input()->call_id();

    fixture.recreate_catalog_and_runtime();
    RunInvocation fork_invocation;
    fork_invocation.owner_scope        = "tenant:runtime";
    fork_invocation.agent_id           = "test-fork";
    fork_invocation.program_version_id = version.id();
    fork_invocation.run_id             = "fork-target-run";
    fork_invocation.budget             = source.remaining_budget();
    fork_invocation.input              = json::object();
    fork_invocation.message_sequence   = 19;
    fork_invocation.idempotency_key    = "test-fork:19";
    fork_invocation.correlation_id     = "trace-fork-target";
    fork_invocation.validate();
    const auto forked =
        fixture.runtime
            ->fork(ExactProgramCheckpointReference{source.run_id(),
                                                   source.checkpoint()->checkpoint_id},
                   fork_invocation,
                   ProgramResume{
                       json{{"decision", "forked"}}, "trace-fork-resume", {}, source_pending_id})
            .wait();

    EXPECT_EQ(forked.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(forked.run_id(), "fork-target-run");
    EXPECT_NE(forked.run_id(), source.run_id());
    EXPECT_EQ(forked.output()["channels"]["value"]["value"], "forked");
    ASSERT_TRUE(forked.checkpoint().has_value());
    EXPECT_NE(forked.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
    EXPECT_EQ(interrupt_calls.load(), 2U);

    const auto target_record = fixture.journal->load("tenant:runtime", forked.run_id());
    ASSERT_TRUE(target_record.has_value());
    ASSERT_TRUE(target_record->fork_receipt().has_value());
    EXPECT_TRUE(target_record->fork_receipt()->compatible());
    EXPECT_EQ(target_record->fork_source_run_id(), source.run_id());
    EXPECT_EQ(target_record->fork_source_program_version_id(), version.id());
    EXPECT_EQ(target_record->fork_source_checkpoint_id(), source.checkpoint()->checkpoint_id);
    EXPECT_EQ(target_record->invocation(), fork_invocation);

    const auto source_after = fixture.runtime->reconnect("tenant:runtime", source.run_id()).wait();
    EXPECT_EQ(source_after.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(source_after.checkpoint()->checkpoint_id, source.checkpoint()->checkpoint_id);
}

TEST(ProgramRuntimeTest, ForkAllowsBudgetOnlyMigrationButEnforcesTargetDeclaredBounds) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-budget-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    auto target_document = program_document("runtime-interrupt");
    target_document["declared_budget_requirements"][1]["maximum"] = 10;
    const auto target_version = fixture.admit_document(std::move(target_document));
    const auto plan =
        fixture.catalog->plan_migration("tenant:runtime", source_version.id(), target_version.id());
    EXPECT_TRUE(plan.is_compatible());

    const std::string rejected_run_id = "fork-budget-rejected";
    try {
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            target_version,
            ProgramInvocation{json::object(),
                              source.remaining_budget(),
                              "trace-fork-budget-target",
                              {},
                              rejected_run_id},
            ProgramResume{json{{"decision", "must-not-dispatch"}},
                          "trace-fork-budget-resume",
                          {},
                          pending_call_id(source)});
        FAIL() << "budget outside target admitted bounds unexpectedly forked";
    } catch (const std::exception& error) {
        EXPECT_NE(std::string(error.what()).find("target admitted bounds"), std::string::npos);
    }

    EXPECT_FALSE(fixture.journal->load("tenant:runtime", rejected_run_id).has_value());
    const auto source_after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(source_after->id(), source_before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, ForkRejectsSourceRunIdBeforeCloningCheckpoint) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture;
    const auto      version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-same-run", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(before.has_value());
    const auto source_thread             = before->exact_checkpoint()->core_thread_id;
    const auto source_checkpoints_before = fixture.checkpoints->list(source_thread, 100).size();
    ProgramInvocation invocation{json::object(),
                                 source.remaining_budget(),
                                 "trace-fork-same-run-target",
                                 {},
                                 source.run_id()};
    EXPECT_THROW(
        (void)fixture.runtime->fork(
            "tenant:runtime",
            ExactProgramCheckpointReference{source.run_id(), source.checkpoint()->checkpoint_id},
            version, std::move(invocation),
            ProgramResume{json{{"decision", "must-not-clone"}},
                          "trace-fork-same-run-resume",
                          {},
                          before->pending_input()->call_id()}),
        std::exception);

    const auto after = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->id(), before->id());
    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(fixture.checkpoints->list(source_thread, 100).size(), source_checkpoints_before);
}

TEST(ProgramRuntimeTest, ForkMismatchesRejectBeforeTargetRunAndLeaveSourceUnchanged) {
    interrupt_calls.store(0);
    followup_calls.store(0);
    AdmittedRuntime fixture;
    const auto      source_version = fixture.admit("runtime-interrupt");
    const auto      source =
        fixture.runtime
            ->start("tenant:runtime", source_version,
                    ProgramInvocation{json::object(), grant(), "trace-fork-reject-source", {}})
            .wait();
    ASSERT_EQ(source.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(source.checkpoint().has_value());
    const auto source_before = fixture.journal->load("tenant:runtime", source.run_id());
    ASSERT_TRUE(source_before.has_value());

    struct RejectionCase {
        std::string            run_id;
        ForkCompatibilityField field;
        json                   document;
    };
    std::vector<RejectionCase> cases;
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["extra"] =
            json{{"reducer", "runtime-overwrite"}, {"initial", nullptr}};
        cases.push_back(
            {"fork-reject-channel", ForkCompatibilityField::Channel, std::move(document)});
    }
    {
        auto document = program_document("runtime-interrupt");
        document["root"]["definition"]["channels"]["value"]["reducer"] = "runtime-alternate";
        cases.push_back(
            {"fork-reject-reducer", ForkCompatibilityField::Reducer, std::move(document)});
    }
    {
        auto document                                       = program_document("runtime-interrupt");
        document["root"]["definition"]["nodes"]["followup"] = json{{"type", "runtime-followup"}};
        document["root"]["definition"]["edges"] =
            json::array({json{{"from", "__start__"}, {"to", "work"}},
                         json{{"from", "work"}, {"to", "followup"}},
                         json{{"from", "followup"}, {"to", "__end__"}}});
        cases.push_back({"fork-reject-continuation", ForkCompatibilityField::Continuation,
                         std::move(document)});
    }

    for (auto& item : cases) {
        const auto target = fixture.admit_document(std::move(item.document));
        try {
            (void)fixture.runtime->fork(
                "tenant:runtime",
                ExactProgramCheckpointReference{source.run_id(),
                                                source.checkpoint()->checkpoint_id},
                target,
                ProgramInvocation{json::object(),
                                  source.remaining_budget(),
                                  "trace-fork-reject",
                                  {},
                                  item.run_id},
                ProgramResume{
                    json{{"decision", "must-not-dispatch"}}, "trace-fork-reject-resume", {}});
            FAIL() << "incompatible fork unexpectedly created a target run";
        } catch (const ProgramForkCompatibilityError& error) {
            EXPECT_TRUE(
                std::any_of(error.receipt().witnesses().begin(), error.receipt().witnesses().end(),
                            [&](const auto& witness) { return witness.field == item.field; }));
        }
        EXPECT_FALSE(fixture.journal->load("tenant:runtime", item.run_id).has_value());
        const auto source_now = fixture.journal->load("tenant:runtime", source.run_id());
        ASSERT_TRUE(source_now.has_value());
        EXPECT_EQ(source_now->id(), source_before->id());
    }

    EXPECT_EQ(interrupt_calls.load(), 1U);
    EXPECT_EQ(followup_calls.load(), 0U);
}

json orchestration_document(json root, std::string node_type = "runtime-completed") {
    auto document      = program_document(std::move(node_type));
    root["name"]       = "main";
    root["definition"] = std::move(document["root"]["definition"]);
    document["declared_budget_requirements"][3]["maximum"] = 4;
    document["declared_budget_requirements"][4]["maximum"] = 32;
    document["root"]                                       = std::move(root);
    return document;
}

json orchestration_document() {
    return orchestration_document(
        json{{"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "emit"}, {"value", json{{"kind", "core_done"}}}},
                                       json{{"op", "return"}, {"value", json{{"ok", true}}}}})}});
}

ProgramResult run_orchestration(AdmittedRuntime& fixture,
                                json             document,
                                json             input    = json::object(),
                                std::string      trace_id = "trace-orchestration") {
    const auto version = fixture.admit_document(std::move(document));
    return fixture.runtime->run("tenant:runtime", version,
                                ProgramInvocation{std::move(input),
                                                  RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                                  std::move(trace_id),
                                                  {}});
}

TEST(ProgramRuntimeTest, BranchSelectsWithoutDispatchingTheUnselectedCore) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "branch"},
                      {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                      {"then", json{{"op", "call_core"}}},
                      {"else", json{{"op", "return"}, {"value", json{{"selected", "else"}}}}}}),
        json{{"route", "else"}}, "trace-branch");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"selected", "else"}}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, BoundedLoopReportsBudgetExhaustionAfterTheLastAllowedIteration) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "loop"},
                                         {"condition", json{{"path", "/never"}, {"exists", false}}},
                                         {"max_iterations", 2},
                                         {"body", json{{"op", "call_core"}}}}),
        json::object(), "trace-loop");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_LOOP_BOUND");
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RetryRetriesCoreFailuresAndPreservesFailureClassification) {
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "retry"}, {"max_attempts", 2}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-retry");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(result.failure()->attempts, 2U);
}

TEST(ProgramRuntimeTest, DeepNestedCoreFailurePreservesOperationAndSourceCoordinate) {
    AdmittedRuntime fixture;
    const auto      nested_root = json{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "branch"},
                                            {"condition", json{{"path", "/route"}, {"equals", "then"}}},
                                            {"then", json{{"op", "retry"},
                                                          {"max_attempts", 1},
                                                          {"body", json{{"op", "call_core"}}}}}}})}};
    const auto result =
        run_orchestration(fixture, orchestration_document(nested_root, "runtime-failing"),
                          json{{"route", "then"}}, "trace-deep-nested-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0.0.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/children/0/then/body");
}

TEST(ProgramRuntimeTest, ParallelJoinsAllBranchesInPlanOrder) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result =
        run_orchestration(fixture,
                          orchestration_document(
                              json{{"op", "parallel"}, {"branches", json::array({first, second})}}),
                          json::object(), "trace-parallel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"branch", 1}}, json{{"branch", 2}}}));
    EXPECT_EQ(completed_calls.load(), 1U);
}

TEST(ProgramRuntimeTest, NestedParallelCannotExceedRunConcurrencyGrant) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const json      nested{
             {"op", "parallel"},
             {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}};
    const auto version = fixture.admit_document(orchestration_document(
        json{{"op", "parallel"}, {"branches", json::array({nested, nested})}},
        "runtime-short-blocking"));

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 2, 32, 20, 0, 0, 0},
                                               "trace-nested-concurrency",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CONCURRENCY_BUDGET");
    EXPECT_EQ(result.usage().peak_concurrency, 2U);
    EXPECT_EQ(blocking_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, RaceReturnsTheFirstCompletedBranchAndCancelsTheLoser) {
    AdmittedRuntime fixture(2);
    const json      first{
             {"op", "sequence"},
             {"children", json::array({json{{"op", "call_core"}},
                                       json{{"op", "return"}, {"value", json{{"branch", 1}}}}})}};
    const json second{{"op", "return"}, {"value", json{{"branch", 2}}}};
    const auto result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "race"}, {"branches", json::array({first, second})}},
                               "runtime-short-blocking"),
        json::object(), "trace-race");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"branch", 2}}));
}

TEST(ProgramRuntimeTest, RaceUsesDeclarationOrderForAReadyTie) {
    // A ready tie is a single scheduler-turn condition. Use one scheduler
    // thread so this verifies declaration-order arbitration rather than
    // treating cross-worker completion timing as a logical tie.
    AdmittedRuntime fixture(1);
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "race"},
                      {"branches",
                       json::array(
                      {json{{"op", "sequence"},
                                 {"children", json::array({json{{"op", "call_core"}},
                                                           json{{"op", "return"}, {"value", 1}}})}},
                            json{{"op", "sequence"},
                                 {"children", json::array({json{{"op", "call_core"}},
                                                           json{{"op", "return"}, {"value", 2}}})}}})}}),
        json::object(), "trace-race-tie");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), 1);
}

TEST(ProgramRuntimeTest, QuorumStopsAfterRequiredSuccesses) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{{"op", "quorum"},
                                         {"branches", json::array({json{{"op", "return"}, {"value", 1}},
                                                                   json{{"op", "return"}, {"value", 2}},
                                                                   json{{"op", "call_core"}}})},
                                         {"min_success", 2}}),
        json::object(), "trace-quorum");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({1, 2}));
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, MapEvaluatesEveryItemAndCollectsOutputs) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(json{
                 {"op", "map"},
                 {"items", json::array({1, 2})},
                 {"body", json{{"op", "sequence"},
                               {"children", json::array({json{{"op", "call_core"}},
                                                         json{{"op", "return"},
                                                              {"value", json{{"mapped", true}}}}})}}}}),
        json::object(), "trace-map");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json::array({json{{"mapped", true}}, json{{"mapped", true}}}));
    EXPECT_EQ(completed_calls.load(), 2U);
}

TEST(ProgramRuntimeTest, AwaitTimeoutCancelsTheChildOperation) {
    AdmittedRuntime fixture(2);
    const auto      started = std::chrono::steady_clock::now();
    const auto      result  = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "await"}, {"timeout_ms", 10}, {"body", json{{"op", "call_core"}}}},
            "runtime-blocking"),
        json::object(), "trace-await");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started)
            .count(),
        2);
}

TEST(ProgramRuntimeTest, AwaitPropagatesChildFailureBeforeTimeout) {
    AdmittedRuntime fixture;
    const auto      started = std::chrono::steady_clock::now();
    const auto      result  = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "await"}, {"timeout_ms", 1000}, {"body", json{{"op", "call_core"}}}},
            "runtime-failing"),
        json::object(), "trace-await-failure");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_CORE_FAILURE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count(),
              500);
}

TEST(ProgramRuntimeTest, ExplicitCancelStopsTheFollowingOperation) {
    completed_calls.store(0);
    AdmittedRuntime fixture;
    const auto      result = run_orchestration(
        fixture,
        orchestration_document(
            json{{"op", "sequence"},
                      {"children", json::array({json{{"op", "cancel"}}, json{{"op", "call_core"}}})}}),
        json::object(), "trace-cancel");

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, ExplicitCheckpointPublishesCheckpointEventAfterCore) {
    auto            sink = std::make_shared<CountingSink>();
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(
        orchestration_document(json{{"op", "checkpoint"}, {"body", json{{"op", "call_core"}}}}));
    const auto result = fixture.runtime->run(
        "tenant:runtime", version,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                          "trace-checkpoint", sink});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(result.checkpoint().has_value());
    EXPECT_GE(sink->calls.load(), 1U);
}
TEST(ProgramRuntimeTest, SequenceEmitAndReturnHaveDeterministicTraceAndOutput) {
    AdmittedRuntime fixture;
    const auto      version = fixture.admit_document(orchestration_document());

    const auto result =
        fixture.runtime->run("tenant:runtime", version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 4, 32, 20, 0, 0, 0},
                                               "trace-orchestration",
                                               {}});

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), json({{"ok", true}}));
    EXPECT_EQ(result.execution_trace(), std::vector<std::string>({"work"}));

    EXPECT_EQ(result.usage().program_operations, 4U);
}
TEST(ProgramRuntimeTest, ChildStartPinsReceiptAndPropagatesParentCancellation) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent = fixture.runtime->start(
        "tenant:runtime", linked.parent_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1}, "trace-parent", {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{
            json::object(), RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}, "trace-child", {}});

    const auto child_record = child.snapshot();
    EXPECT_EQ(child_record.invocation().parent_run_id, parent.run_id());
    EXPECT_EQ(child_record.child_depth(), 1U);
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, RecursiveChildGrantsAttenuateToZeroAtTheConfiguredBoundary) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(4);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 4, 4},
                                                 "trace-recursive-parent",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 3},
                          "trace-recursive-child",
                          {}});
    auto grandchild = fixture.runtime->start_child(
        "tenant:runtime", child, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 2, 2},
                          "trace-recursive-grandchild",
                          {}});
    auto great_grandchild = fixture.runtime->start_child(
        "tenant:runtime", grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                          "trace-recursive-great-grandchild",
                          {}});
    auto leaf = fixture.runtime->start_child(
        "tenant:runtime", great_grandchild, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-recursive-leaf",
                          {}});

    EXPECT_EQ(child.snapshot().child_depth(), 1U);
    EXPECT_EQ(grandchild.snapshot().child_depth(), 2U);
    EXPECT_EQ(great_grandchild.snapshot().child_depth(), 3U);
    EXPECT_EQ(leaf.snapshot().child_depth(), 4U);

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 3, 0},
                              "trace-recursive-sibling-after-subtree-reservation",
                              {}});
        FAIL() << "Expected the reserved descendant quota to reject a sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_COUNT");
    }

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", leaf, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                              "trace-recursive-overflow",
                              {}});
        FAIL() << "Expected recursive child depth to fail closed";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_DEPTH");
    }

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(great_grandchild.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(leaf.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, GlobalChildQuotaRejectsFragmentationAndReleasesOnTerminal) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children                  = 1;
    quota.max_active_children_per_owner        = 1;
    quota.max_pending_spawn_requests           = 4;
    quota.max_pending_spawn_requests_per_owner = 4;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      linked = make_recursive_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 2, 2, 20, 0, 4, 4},
                                                 "trace-global-quota-parent",
                                                 {}});
    auto child =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-a",
                                                       {}});

    try {
        (void)fixture.runtime->start_child(
            "tenant:runtime", parent, linked.receipt, linked.child_version,
            ProgramInvocation{json::object(),
                              RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                              "trace-global-quota-child-b",
                              {}});
        FAIL() << "Expected the global active-child quota to reject a fragmented sibling";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(child.cancel());
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto replacement =
        fixture.runtime->start_child("tenant:runtime", parent, linked.receipt, linked.child_version,
                                     ProgramInvocation{json::object(),
                                                       RunBudget{5000, 500, 500, 1, 1, 10, 0, 3, 1},
                                                       "trace-global-quota-child-c",
                                                       {}});
    EXPECT_EQ(replacement.snapshot().child_depth(), 1U);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(replacement.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ChildResumeReclaimsGlobalQuotaAfterACompetingChildStops) {
    ProgramChildQuotaConfig quota;
    quota.max_active_children           = 1;
    quota.max_active_children_per_owner = 1;

    blocking_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, quota);
    const auto      interrupted_link =
        make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    const auto active_link = make_linked_child(fixture, "runtime-blocking", "runtime-blocking");

    auto interrupted_parent =
        fixture.runtime->start("tenant:runtime", interrupted_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-resume-parent",
                                                 {}});
    auto interrupted_child = fixture.runtime->start_child(
        "tenant:runtime", interrupted_parent, interrupted_link.receipt,
        interrupted_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-resume-child",
                          {}});
    const auto interrupted = interrupted_child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);

    auto active_parent =
        fixture.runtime->start("tenant:runtime", active_link.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-quota-active-parent",
                                                 {}});
    auto active_child = fixture.runtime->start_child(
        "tenant:runtime", active_parent, active_link.receipt, active_link.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-quota-active-child",
                          {}});

    try {
        (void)fixture.runtime->resume(
            "tenant:runtime", interrupted_child.run_id(),
            resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
        FAIL() << "Expected child resume to honor the active global quota";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_CHILD_QUOTA");
    }

    EXPECT_TRUE(active_child.cancel());
    EXPECT_EQ(active_child.wait().status(), ProgramTerminalStatus::Cancelled);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", interrupted_child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-quota-retry"));
    EXPECT_EQ(resumed.wait().status(), ProgramTerminalStatus::Completed);

    EXPECT_TRUE(interrupted_parent.cancel());
    EXPECT_EQ(interrupted_parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_TRUE(active_parent.cancel());
    EXPECT_EQ(active_parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DuplicateChildRecoveryReturnsTheExistingRun) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-duplicate",
                                                 {}});
    ProgramInvocation invocation{json::object(),
                                 RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                 "trace-child-duplicate",
                                 {}};
    auto              first = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);
    auto duplicate          = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                           linked.child_version, invocation);

    EXPECT_EQ(duplicate.run_id(), first.run_id());
    EXPECT_EQ(duplicate.snapshot().id(), first.snapshot().id());
    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(duplicate.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, InterruptedChildRemainsInFlightAndResumeJoinsDurably) {
    interrupt_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-interrupt");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-interrupted-child",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-interrupted",
                          {}});

    const auto interrupted = child.wait();
    ASSERT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    const auto parent_after_interrupt = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_interrupt.has_value());
    ASSERT_EQ(parent_after_interrupt->children().size(), 1U);
    EXPECT_EQ(parent_after_interrupt->children().front().state, ProgramChildState::Dispatched);
    ASSERT_TRUE(parent_after_interrupt->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_interrupt->children().front().terminal_result->status(),
              ProgramTerminalStatus::Interrupted);

    auto resumed = fixture.runtime->resume(
        "tenant:runtime", child.run_id(),
        resume_for(interrupted, json{{"decision", "approved"}}, "trace-child-resumed"));
    const auto result = resumed.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.snapshot().child_depth(), 1U);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "approved");
    const auto parent_after_resume = fixture.journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(parent_after_resume.has_value());
    ASSERT_EQ(parent_after_resume->children().size(), 1U);
    EXPECT_EQ(parent_after_resume->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(parent_after_resume->children().front().terminal_result.has_value());
    EXPECT_EQ(parent_after_resume->children().front().terminal_result->status(),
              ProgramTerminalStatus::Completed);

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, FailedDispatchIsRetriedFromPublishingBoundary) {
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-dispatch-recovery",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-dispatch-recovery",
                                       {}};
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);

    const auto publishing = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                  linked.child_version, child_invocation);
    EXPECT_EQ(recovered.wait().status(), ProgramTerminalStatus::Completed);
    const auto completed = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    ASSERT_TRUE(completed->children().front().terminal_result.has_value());

    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, RecoverChildrenRetriesPublishingBoundaryWithoutDuplicateDispatch) {
    completed_calls.store(0);
    auto            journal = std::make_shared<FailChildDispatchOnceJournal>();
    AdmittedRuntime fixture(2, {}, journal);
    const auto      linked = make_linked_child(fixture, "runtime-blocking", "runtime-completed");
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-recover-children",
                                                 {}});
    ProgramInvocation child_invocation{json::object(),
                                       RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                       "trace-child-recover-children",
                                       {}};

    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, child_invocation),
                 ProgramDiagnosticError);
    const auto publishing = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(publishing.has_value());
    ASSERT_EQ(publishing->children().size(), 1U);
    EXPECT_EQ(publishing->children().front().state, ProgramChildState::Publishing);

    journal->allow_dispatch();
    auto recovered = fixture.runtime->recover_children("tenant:runtime", parent.run_id());
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered.front().wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto completed = journal->load("tenant:runtime", parent.run_id());
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->children().size(), 1U);
    EXPECT_EQ(completed->children().front().state, ProgramChildState::Completed);
    EXPECT_TRUE(parent.cancel());
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeTest, DurableDslSpawnDispatchesTheLinkedChildAndJoinsItsResult) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    auto            parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output()["channels"]["value"]["value"], "completed");
    EXPECT_EQ(completed_calls.load(), 1U);

    const auto record   = parent.snapshot();
    const auto children = record.children();
    ASSERT_EQ(children.size(), 1U);
    const auto& child = children.front();
    EXPECT_EQ(child.link_id, linked.receipt.id());
    EXPECT_EQ(child.invocation.parent_run_id, parent.run_id());
    EXPECT_EQ(child.invocation.child_depth, 1U);
    EXPECT_EQ(child.state, ProgramChildState::Completed);
    ASSERT_TRUE(child.terminal_result.has_value());
    EXPECT_EQ(child.terminal_result->status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRuntimeTest, DurableDslSpawnFailsClosedWhenItsBindingCannotBeResolved) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked = make_durable_spawn_child(fixture);
    fixture.clear_child_bindings();

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-missing-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(result.failure()->witness["source_pointer"], "/root/body");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsAResolverReceiptForAnotherBinding) {
    completed_calls.store(0);
    AdmittedRuntime         fixture(2);
    const auto              linked     = make_durable_spawn_child(fixture);
    const ModuleLinkReceipt mismatched = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), "different-child",
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget()});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{mismatched, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_BINDING");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslSpawnRejectsReceiptWhoseGuaranteeExceedsChildVersion) {
    completed_calls.store(0);
    AdmittedRuntime fixture(2, {}, {}, {}, ExecutionGuarantee::Recorded);
    const auto linked = make_durable_spawn_child(fixture, "runtime-completed", "runtime-completed",
                                                 std::nullopt, ExecutionGuarantee::Recorded);
    ASSERT_EQ(linked.child_version.execution_guarantee(), ExecutionGuarantee::Recorded);

    const ModuleLinkReceipt forged = ModuleLinkReceipt::create(ModuleLinkReceiptData{
        linked.receipt.owner_scope(), linked.receipt.parent_module_id(),
        linked.receipt.dependency_merkle_root(), linked.receipt.child_name(),
        linked.receipt.child_program_version_id(), linked.receipt.child_bundle_id(),
        linked.receipt.child_input_contract_fingerprint(),
        linked.receipt.child_output_contract_fingerprint(), linked.receipt.granted_capabilities(),
        linked.receipt.granted_effects(), linked.receipt.budget(), ExecutionGuarantee::Strict});
    fixture.bind_child(linked.parent_version, "child",
                       ProgramRuntimeChildBinding{forged, linked.child_version});

    const auto result =
        fixture.runtime->run("tenant:runtime", linked.parent_version,
                             ProgramInvocation{json::object(),
                                               RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                               "trace-guarantee-mismatched-durable-binding",
                                               {}});
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_CHILD_GUARANTEE");
    EXPECT_EQ(result.failure()->operation_id, "root.0");
    EXPECT_EQ(completed_calls.load(), 0U);
}

TEST(ProgramRuntimeTest, DurableDslAwaitTimeoutCancelsAndRecordsItsChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto      linked =
        make_durable_spawn_child(fixture, "runtime-completed", "runtime-blocking", 10);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-durable-spawn-timeout",
                                                 {}});

    const auto result = parent.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_AWAIT_TIMEOUT");

    const auto pending          = parent.snapshot();
    const auto pending_children = pending.children();
    ASSERT_EQ(pending_children.size(), 1U);
    auto child =
        fixture.runtime->reconnect("tenant:runtime", pending_children.front().child_run_id);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto settled          = parent.snapshot();
    const auto settled_children = settled.children();
    ASSERT_EQ(settled_children.size(), 1U);
    EXPECT_EQ(settled_children.front().state, ProgramChildState::Cancelled);
    EXPECT_FALSE(settled_children.front().terminal_result.has_value());
}
TEST(ProgramRuntimeTest, ParentCompletionCancelsAttachedChild) {
    blocking_calls.store(0);
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-complete",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-complete",
                          {}});

    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
}
TEST(ProgramRuntimeTest, ParentTerminalReconnectRetainsDurableChildRecords) {
    AdmittedRuntime fixture(2);
    const auto linked = make_linked_child(fixture, "runtime-short-blocking", "runtime-blocking");
    auto       parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reopen",
                                                 {}});
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-reopen",
                          {}});

    ASSERT_EQ(parent.wait().status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    const auto parent_id = parent.run_id();
    const auto child_id  = child.run_id();
    const auto before    = parent.snapshot();
    ASSERT_EQ(before.children().size(), 1U);
    EXPECT_EQ(before.children().front().child_run_id, child_id);

    fixture.recreate_catalog_and_runtime();
    auto       reconnected = fixture.runtime->reconnect("tenant:runtime", parent_id);
    const auto after       = reconnected.snapshot();
    ASSERT_EQ(after.children().size(), 1U);
    EXPECT_EQ(after.children().front().child_run_id, child_id);
    EXPECT_EQ(after.children().front().invocation.parent_run_id, parent_id);
    EXPECT_EQ(reconnected.wait().status(), ProgramTerminalStatus::Completed);
}
TEST(ProgramRuntimeTest, FailedChildPublicationReleasesParentReservation) {
    blocking_calls.store(0);
    AdmittedRuntime   fixture(2);
    const auto        linked = make_linked_child(fixture);
    ProgramInvocation occupied_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    occupied_invocation.requested_run_id = "run-child-conflict";
    auto occupied =
        fixture.runtime->start("tenant:runtime", linked.child_version, occupied_invocation);
    auto parent =
        fixture.runtime->start("tenant:runtime", linked.parent_version,
                               ProgramInvocation{json::object(),
                                                 RunBudget{10000, 1000, 1000, 1, 2, 20, 0, 1, 1},
                                                 "trace-parent-reservation",
                                                 {}});

    ProgramInvocation conflict_invocation{json::object(),
                                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                          "trace-child-conflict",
                                          {}};
    conflict_invocation.requested_run_id = occupied.run_id();
    EXPECT_THROW((void)fixture.runtime->start_child("tenant:runtime", parent, linked.receipt,
                                                    linked.child_version, conflict_invocation),
                 std::exception);
    auto child = fixture.runtime->start_child(
        "tenant:runtime", parent, linked.receipt, linked.child_version,
        ProgramInvocation{json::object(),
                          RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                          "trace-child-after-conflict",
                          {}});

    EXPECT_EQ(parent.cancel(), true);
    EXPECT_EQ(parent.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(child.wait().status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(occupied.cancel(), true);
    EXPECT_EQ(occupied.wait().status(), ProgramTerminalStatus::Cancelled);
}
