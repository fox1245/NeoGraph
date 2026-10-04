#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>

#include "run_context_runtime.h"
#include "canonical_json.h"
#include "managed_budget_journal.h"
#include <neograph/graph/loader.h>
#include <neograph/graph/validator.h>
#include <neograph/graph/coordinator.h>
#include <neograph/hook_runtime.h>

#include <neograph/async/run_sync.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/use_awaitable.hpp>

#include <stdexcept>
#include <utility>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace neograph::graph {

namespace {
class GraphGenerationIdentityCarrier final : public Tool {
public:
    explicit GraphGenerationIdentityCarrier(GraphGenerationIdentity value)
        : identity(std::move(value)) {}

    ChatTool get_definition() const override {
        return {"__neograph_internal_generation_identity__", "", json::object()};
    }
    std::string execute(const json&) override {
        throw std::logic_error("Graph generation identity is not an executable tool");
    }
    std::string get_name() const override {
        return "__neograph_internal_generation_identity__";
    }

    GraphGenerationIdentity identity;
};

// Default fan-out pool size. hardware_concurrency() can return 0 on
// platforms that fail to detect; fall back to 4 so we always have
// real parallelism instead of the old single-thread default that
// pinned multi-Send fan-out behind one worker.
std::size_t default_worker_count() {
    auto n = std::thread::hardware_concurrency();
    return n > 0 ? static_cast<std::size_t>(n) : 4u;
}

// Preserve every policy boundary when a parent graph runs a child graph. The
// parent decides first; only an Allow reaches the child policy. A parent rewrite
// becomes the child's input, while a child rewrite can further narrow it.
ToolGate compose_tool_gates(ToolGate parent, ToolGate local) {
    if (!parent) return local;
    if (!local) return parent;

    return [parent = std::move(parent), local = std::move(local)](
               ToolCall call, ToolGateContext context) -> asio::awaitable<ToolDecision> {
        auto parent_decision = co_await parent(call, context);
        if (parent_decision.kind != ToolDecision::Kind::Allow) {
            co_return parent_decision;
        }

        if (parent_decision.args) {
            call.arguments = parent_decision.args->dump();
        }

        auto local_decision = co_await local(std::move(call), std::move(context));
        if (local_decision.kind != ToolDecision::Kind::Allow || local_decision.args) {
            co_return local_decision;
        }
        co_return parent_decision;
    };
}

void report_validation(const ValidationReport& report, int schema_version) {
    if (schema_version >= 1) {
        if (report.has_errors()) {
            throw std::runtime_error(
                "graph validation failed (schema_version "
                + std::to_string(schema_version) + "):\n" + report.summary());
        }
        for (const auto* warning : report.warnings()) {
            std::fprintf(stderr, "[neograph] lint [%s] %s: %s\n",
                         warning->code.c_str(), warning->path.c_str(),
                         warning->message.c_str());
        }
    } else {
        for (const auto* error : report.errors()) {
            std::fprintf(stderr, "[neograph] warning: [%s] %s: %s\n",
                         error->code.c_str(), error->path.c_str(),
                         error->message.c_str());
        }
    }
}
json checkpoint_ephemeral_guard(const json& metadata) {
    return metadata.is_object() && metadata.contains("_neograph_ephemeral_guard")
               ? metadata["_neograph_ephemeral_guard"] : json();
}

bool checkpoint_has_managed_bank(const json& state) {
    return state.is_object() && state.contains("provider_managed_budget");
}
[[noreturn]] void reject_missing_original_managed_bank() {
    throw std::invalid_argument("Checkpoint namespace requires original authenticated managed-bank custody");
}

ManagedBudgetLeaseScope original_managed_budget_scope(
    const Checkpoint* source, std::uint64_t original_ceiling, std::string_view owner,
    const std::string& thread, const std::string& graph,
    std::optional<std::chrono::steady_clock::time_point>& deadline) {
    ManagedBudgetLeaseScope scope;
    scope.owner_scope = owner;
    scope.thread_id = thread;
    scope.graph_identity = graph;
    scope.original_ceiling = original_ceiling;
    if (!source) {
        if (deadline) {
            scope.original_deadline_ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline->time_since_epoch()).count();
            scope.deadline_clock_identity = detail::managed_budget_deadline_clock_identity();
        }
        return scope;
    }
    if (!source->metadata.is_object() || !source->metadata.contains("_neograph_managed_budget_scope"))
        throw std::invalid_argument("Managed checkpoint lacks its original journal scope");
    const auto& data = source->metadata.at("_neograph_managed_budget_scope");
    if (!data.is_object() || data.size() != 8 ||
        data.at("schema") != "neograph.graph-managed-bank-scope/v1" ||
        data.at("owner_scope") != owner || data.at("thread_id") != thread ||
        data.at("graph_identity") != graph || data.at("original_ceiling").get<std::uint64_t>() != original_ceiling ||
        !data.at("bank_generation").is_string() || data.at("bank_generation").get<std::string>().size() != 64)
        throw std::invalid_argument("Managed checkpoint original scope differs from authenticated bank");
    scope.deadline_clock_identity = data.at("deadline_clock_identity").get<std::string>();
    if (!data.at("original_deadline_ticks").is_null()) {
        scope.original_deadline_ticks = data.at("original_deadline_ticks").get<std::int64_t>();
        if (*scope.original_deadline_ticks < 0 ||
            scope.deadline_clock_identity != detail::managed_budget_deadline_clock_identity())
            throw std::invalid_argument("Managed checkpoint original deadline clock is unavailable");
        const auto original_deadline = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(*scope.original_deadline_ticks)));
        if (!deadline || original_deadline < *deadline) deadline = original_deadline;
    } else if (!scope.deadline_clock_identity.empty()) {
        throw std::invalid_argument("Managed checkpoint has a clock without an original deadline");
    }
    return scope;
}

sp::runtime::Result exception_provider_outcome(const std::exception_ptr& error) {
    if (!error) return {};
    try { std::rethrow_exception(error); }
    catch (const ProviderOutcomeError& failure) {
        return failure.outcome() ? failure.outcome() : exception_provider_outcome(failure.cause());
    }
    catch (const ProviderFailure& failure) { return failure.outcome(); }
    catch (const NodeExecutionError& failure) { return exception_provider_outcome(failure.cause()); }
    catch (...) { return {}; }
}

} // namespace

// =========================================================================
// compile(): compatibility facade over the canonical build() path
//
// Keep the original signature and behavior while routing construction through
// EngineConfig so both entry points share one implementation.
// =========================================================================
std::unique_ptr<GraphEngine> GraphEngine::compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store) {
    EngineConfig config;
    config.node_context     = default_context;
    config.checkpoint_store = std::move(store);
    return build(definition, std::move(config));
}

std::unique_ptr<GraphEngine> GraphEngine::build(const json& definition, EngineConfig config) {
    return build(definition, std::move(config), {});
}

std::unique_ptr<GraphEngine> GraphEngine::build(const json&     definition,
                                                 EngineConfig    config,
                                                 EngineResources resources) {
    if (!resources.tools.empty()) {
        if (!config.node_context.tools.empty()) {
            throw std::invalid_argument(
                "GraphEngine::build received both EngineResources::tools and "
                "NodeContext::tools; supply one owned ToolSet");
        }
        config.node_context.tools = std::move(resources.tools);
    }

    resources.registry = resources.registry ? resources.registry->snapshot()
                                            : GraphRegistry::global().snapshot();
    config.node_context.registry = resources.registry;
    const auto& registry = resources.registry ? *resources.registry : GraphRegistry::global();
    auto topology = GraphCompiler::parse(definition, registry);

    // Translation validation: assert nothing was silently dropped or
    // rewired between the JSON definition and the compiled graph.
    // Strict documents (schema_version >= 1) fail hard; legacy
    // documents get a stderr warning. Must run before the moves below.
    GraphCompiler::verify_roundtrip(definition, topology);

    // Static semantics operate on the declarative topology before factories
    // create runtime nodes. Legacy documents keep warning-only behavior.
    report_validation(GraphValidator::validate(topology, registry),
                      topology.schema_version);

    auto cg = GraphCompiler::link(std::move(topology), config.node_context, registry);
    return link_impl(std::move(cg), std::move(config), std::move(resources), false);
}

std::unique_ptr<GraphEngine> GraphEngine::build_strict(const json& definition,
                                                        EngineConfig config) {
    return build_strict(definition, std::move(config), {});
}

std::unique_ptr<GraphEngine> GraphEngine::build_strict(const json&     definition,
                                                        EngineConfig    config,
                                                        EngineResources resources) {
    json strict_definition = definition;
    if (!strict_definition.contains("schema_version")
        || (strict_definition["schema_version"].is_number_integer()
            && strict_definition["schema_version"].get<int>() == 0)) {
        strict_definition["schema_version"] = 1;
    }
    return build(strict_definition, std::move(config), std::move(resources));
}

std::unique_ptr<GraphEngine> GraphEngine::link(CompiledGraph cg, EngineConfig config) {
    return link(std::move(cg), std::move(config), {});
}

std::unique_ptr<GraphEngine> GraphEngine::link(CompiledGraph   cg,
                                               EngineConfig    config,
                                               EngineResources resources) {
    resources.registry = resources.registry ? resources.registry->snapshot()
                                            : GraphRegistry::global().snapshot();
    return link_impl(std::move(cg), std::move(config), std::move(resources), true);
}

std::optional<std::chrono::system_clock::time_point> hook_deadline_for(
    const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    if (!deadline) return std::nullopt;
    const auto remaining = std::max(*deadline - std::chrono::steady_clock::now(),
                                    std::chrono::steady_clock::duration::zero());
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::system_clock::now() + remaining);
}

std::unique_ptr<GraphEngine> GraphEngine::link(
    CompiledGraph cg,
    EngineConfig config,
    EngineResources resources,
    GraphGenerationIdentity generation) {
    if (generation.core_name.empty() || generation.core_generation_id.empty() ||
        generation.core_name != cg.name) {
        throw std::invalid_argument(
            "GraphEngine generation identity must match the compiled graph");
    }
    resources.registry = resources.registry ? resources.registry->snapshot()
                                            : GraphRegistry::global().snapshot();
    auto engine = link_impl(
        std::move(cg), std::move(config), std::move(resources), true);
    engine->owned_tools_.push_back(
        std::make_unique<GraphGenerationIdentityCarrier>(std::move(generation)));
    return engine;
}

std::unique_ptr<GraphEngine> GraphEngine::link(ValidatedTopology topology,
                                               EngineConfig config) {
    return link(std::move(topology), std::move(config), {});
}

std::unique_ptr<GraphEngine> GraphEngine::link(ValidatedTopology topology,
                                               EngineConfig config,
                                               EngineResources resources) {
    if (!resources.tools.empty()) {
        if (!config.node_context.tools.empty()) {
            throw std::invalid_argument(
                "GraphEngine::link received both EngineResources::tools and "
                "NodeContext::tools; supply one owned ToolSet");
        }
        config.node_context.tools = std::move(resources.tools);
    }

    resources.registry = resources.registry ? resources.registry->snapshot()
                                            : GraphRegistry::global().snapshot();
    config.node_context.registry = resources.registry;
    const auto& registry = resources.registry ? *resources.registry : GraphRegistry::global();
    report_validation(topology.report(), topology.topology().schema_version);
    auto cg = GraphCompiler::link(std::move(topology).release(),
                                  config.node_context, registry);
    return link_impl(std::move(cg), std::move(config), std::move(resources), false);
}

std::unique_ptr<GraphEngine> GraphEngine::link_impl(CompiledGraph   cg,
                                                    EngineConfig    config,
                                                    EngineResources resources,
                                                    bool validate) {
    if (!cg.tools.empty() && !resources.tools.empty()) {
        throw std::invalid_argument(
            "GraphEngine::link received tools in both CompiledGraph and "
            "EngineResources; compiled nodes retain their original ToolSet");
    }

    // Static semantic analysis (issue #75 M2). Strict documents:
    // errors throw, warnings go to stderr. Legacy documents: only
    // errors are surfaced (as stderr warnings — they were silent
    // breakage before, e.g. a dangling edge or an empty route map),
    // heuristic lint is suppressed to keep existing graphs quiet.
    if (validate) {
        const auto& registry = resources.registry ? *resources.registry : GraphRegistry::global();
        report_validation(GraphValidator::validate(cg, registry), cg.schema_version);
    }

    auto engine = std::unique_ptr<GraphEngine>(new GraphEngine());
    engine->budget_graph_identity_ = neograph::detail::sha256_identity(
        "NeoGraph", "graph-managed-budget/v1", neograph::detail::canonical_json_bytes(cg.to_json()));
    engine->name_              = std::move(cg.name);
    engine->channel_defs_      = std::move(cg.channel_defs);
    engine->nodes_             = std::move(cg.nodes);
    engine->has_stateful_subgraph_ = std::any_of(
        engine->nodes_.begin(), engine->nodes_.end(), [](const auto& entry) {
            const auto* child = dynamic_cast<const SubgraphNode*>(entry.second.get());
            return child && (child->persistence() == SubgraphPersistence::PerInvocation ||
                             child->persistence() == SubgraphPersistence::PerThread);
        });
    engine->edges_             = std::move(cg.edges);
    engine->conditional_edges_ = std::move(cg.conditional_edges);
    engine->interrupt_before_  = std::move(cg.interrupt_before);
    engine->interrupt_after_   = std::move(cg.interrupt_after);
    if (cg.retry_policy) {
        engine->default_retry_policy_ = *cg.retry_policy;
    }
    if (config.retry_policy) {
        engine->default_retry_policy_ = *config.retry_policy;
    }
    engine->node_retry_policies_ = std::move(config.node_retry_policies);
    engine->checkpoint_store_    = std::move(config.checkpoint_store);
    engine->store_               = std::move(config.store);
    engine->tool_gate_           = std::move(config.tool_gate);
    engine->tool_execution_controller_ = std::move(config.tool_execution_controller);
    engine->hook_runtime_        = std::move(config.hook_runtime);
    engine->native_history_archive_ = std::move(config.native_history_archive);
    engine->tools_               = cg.tools.empty() ? std::move(resources.tools)
                                                   : std::move(cg.tools);
    engine->node_cache_.set_max_entries(config.node_cache_max_entries);
    for (const auto& node_name : config.cached_nodes) {
        engine->set_node_cache_enabled(
            node_name, true, CacheKeyPolicy{CacheScope::Reusable, {}});
    }
    for (auto& [node_name, policy] : config.node_cache_policies) {
        engine->set_node_cache_enabled(node_name, true, std::move(policy));
    }
    if (config.runtime_interposition) {
        engine->set_runtime_interposition(std::move(config.runtime_interposition));
    }

    // Signal-based dispatch — see Scheduler. A node becomes ready in
    // super-step S+1 iff some node in step S routed to it (regular edge,
    // conditional-edge branch, Command goto, or Send). No static
    // predecessor map: that would conflate XOR routing with AND fan-in
    // and deadlock conditional self-loops. Explicitly-declared barriers
    // (via the node's "barrier": {"wait_for": [...]} field) opt back
    // into AND-join semantics for those specific nodes.
    engine->scheduler_ =
        std::make_unique<Scheduler>(engine->edges_, engine->conditional_edges_,
                                    std::move(cg.barrier_specs), resources.registry);

    GraphEngine* self         = engine.get();
    auto         retry_lookup = [self](const std::string& node_name) {
        return self->get_retry_policy(node_name);
    };
    engine->executor_ = std::make_unique<NodeExecutor>(
        engine->nodes_, engine->channel_defs_, std::move(retry_lookup),
        std::move(resources.registry), nullptr, &engine->node_cache_);

    // NodeExecutor owns retry + fan-out + Send invocation. Bind the
    // retry-policy lookup to this engine's per-node override map so
    // set_node_retry_policy continues to work after compile() returns.
    //
    // v1.0: default is `set_worker_count(1)` — no engine-owned
    // thread_pool, fan-out branches dispatch inline on the coroutine's
    // own executor. The pre-b59444f default. Two reasons:
    //
    //   1. **Sequential / single-Send workloads pay nothing.** A
    //      tight engine-only loop (the par micro-bench, every
    //      production graph whose fan-out is dwarfed by LLM latency)
    //      avoided the ~6-7 µs cross-thread submit per fan-out task
    //      when the pool was off. The hardware_concurrency default
    //      introduced in b59444f (v0.1.4) lost that — par 11.6 µs →
    //      44 µs — for no real-world win, since fan-out tasks at the
    //      shape NeoGraph cares about (LLM call ≫ µs) absorb the
    //      submit cost in the wash. The default flip was driven by
    //      one specific bench (dr_compare) where FANOUT = 5 against
    //      LangGraph hid the submit cost behind LLM latency.
    //
    //   2. **Cross-thread submit + non-thread-safe node state is the
    //      footgun**, not the safe path. b59444f silently exposed
    //      every default-configured user's nodes to concurrent
    //      execution on the engine's pool.
    //
    // Users who need true parallel fan-out (CPU-bound work in node
    // bodies, large fan-out, etc.) call `set_worker_count_auto()` or
    // `set_worker_count(N)` explicitly. The opt-in surface is
    // documented in `docs/migration-v0.4-to-v1.0.md`.
    engine->set_worker_count(config.worker_count);
    return engine;
}

// =========================================================================
// Configuration helpers
// =========================================================================


const GraphGenerationIdentity* GraphEngine::bound_generation_identity() const noexcept {
    for (const auto& tool : owned_tools_) {
        const auto* carrier =
            dynamic_cast<const GraphGenerationIdentityCarrier*>(tool.get());
        if (carrier) return &carrier->identity;
    }
    return nullptr;
}

std::string GraphEngine::managed_budget_graph_identity() const {
    const auto* generation = bound_generation_identity();
    return generation ? budget_graph_identity_ + ":" + generation->core_generation_id : budget_graph_identity_;
}

void GraphEngine::set_checkpoint_store(std::shared_ptr<CheckpointStore> store) {
    checkpoint_store_ = std::move(store);
}

void GraphEngine::set_store(std::shared_ptr<Store> store) {
    store_ = std::move(store);
}

void GraphEngine::set_runtime_interposition(
    std::shared_ptr<::neograph::RuntimeInterpositionController> controller) {
    runtime_interposition_ = std::move(controller);
    for (auto& [name, node] : nodes_) {
        (void)name;
        if (auto* consumer = dynamic_cast<::neograph::RuntimeInterpositionConsumer*>(node.get())) {
            consumer->set_runtime_interposition(runtime_interposition_);
        }
    }
}

void GraphEngine::set_retry_policy(const RetryPolicy& policy) {
    default_retry_policy_ = policy;
}

void GraphEngine::set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy) {
    node_retry_policies_[node_name] = policy;
}

void GraphEngine::set_worker_count(std::size_t n) {
    if (n < 1) n = 1;
    // The same admission gate as state administration closes the old
    // load-then-swap race: no run may enter until the old pool is joined
    // and the new executor is installed.
    AdministrationGuard guard(*this);
    // n == 1 means "no engine-owned thread pool" — fan-out branches
    // dispatch on whichever executor drives the coroutine (single-
    // thread io_context for run_sync, the caller's pool for
    // run_async). This matches the pre-b59444f fast path and keeps
    // worker=1 cheap for CPU-tiny / non-thread-safe workloads
    // (par micro-bench: ~12µs vs the cross-thread-submit path's
    // ~94µs at the same worker count). For n >= 2 we wire a real
    // thread_pool — rebuild it because asio::thread_pool isn't
    // resizable. Dtor joins the old pool's workers, so callers must
    // not resize across an in-flight run() (documented on declaration).
    GraphEngine* self = this;
    auto         registry     = executor_ ? executor_->registry_ : nullptr;
    auto retry_lookup = [self](const std::string& node_name) {
        return self->get_retry_policy(node_name);
    };
    if (n == 1) {
        pool_.reset();
        executor_ = std::make_unique<NodeExecutor>(nodes_, channel_defs_, std::move(retry_lookup),
                                                   std::move(registry), nullptr, &node_cache_);
        return;
    }
    pool_ = std::make_unique<asio::thread_pool>(n);
    executor_ = std::make_unique<NodeExecutor>(nodes_, channel_defs_, std::move(retry_lookup),
                                               std::move(registry), pool_.get(), &node_cache_);
}

void GraphEngine::set_worker_count_auto() {
    set_worker_count(default_worker_count());
}

void GraphEngine::set_node_cache_enabled(const std::string& node_name,
                                          bool enabled) {
    node_cache_.set_policy(node_name, CacheKeyPolicy{});
    node_cache_.set_enabled(node_name, enabled);
}

void GraphEngine::clear_node_cache() {
    node_cache_.clear();
}

void GraphEngine::set_node_cache_max_entries(std::size_t max_entries) {
    node_cache_.set_max_entries(max_entries);
}

RetryPolicy GraphEngine::get_retry_policy(const std::string& node_name) const {
    auto it = node_retry_policies_.find(node_name);
    if (it != node_retry_policies_.end()) return it->second;
    return default_retry_policy_;
}

void GraphEngine::enter_execution() {
    int count = active_runs_.load(std::memory_order_relaxed);
    for (;;) {
        if (count < 0) {
            throw std::logic_error("Cannot execute: engine administration is in progress");
        }
        if (active_runs_.compare_exchange_weak(
                count, count + 1, std::memory_order_acq_rel,
                std::memory_order_relaxed)) return;
    }
}

void GraphEngine::leave_execution() noexcept {
    active_runs_.fetch_sub(1, std::memory_order_release);
}

void GraphEngine::enter_administration() const {
    int idle = 0;
    if (!active_runs_.compare_exchange_strong(
            idle, -1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        throw std::logic_error("Cannot administer: engine execution or administration is in progress");
    }
}

void GraphEngine::leave_administration() const noexcept {
    active_runs_.store(0, std::memory_order_release);
}

GraphEngine::ExecutionGuard::ExecutionGuard(GraphEngine& owner) : engine(owner) {
    engine.enter_execution();
}
GraphEngine::ExecutionGuard::~ExecutionGuard() { engine.leave_execution(); }
GraphEngine::AdministrationGuard::AdministrationGuard(const GraphEngine& owner) : engine(owner) {
    engine.enter_administration();
}
GraphEngine::AdministrationGuard::~AdministrationGuard() { engine.leave_administration(); }

// =========================================================================
// get_state / get_state_history / update_state / fork
// =========================================================================

GraphAdmin GraphEngine::admin() noexcept {
    return GraphAdmin(*this);
}

std::optional<json> GraphAdmin::get_state(const std::string& thread_id) const {
    return engine_->get_state(thread_id);
}

std::vector<Checkpoint> GraphAdmin::get_state_history(
    const std::string& thread_id, int limit) const {
    return engine_->get_state_history(thread_id, limit);
}

void GraphAdmin::update_state(const std::string& thread_id,
                              const json& channel_writes,
                              const std::string& as_node) const {
    engine_->update_state(thread_id, channel_writes, as_node);
}

void GraphAdmin::update_state_writes(
    const std::string& thread_id,
    const std::vector<ChannelWrite>& channel_writes,
    const std::string& as_node) const {
    engine_->update_state_writes(thread_id, channel_writes, as_node);
}

std::string GraphAdmin::fork(const std::string& source_thread_id,
                             const std::string& new_thread_id,
                             const std::string& checkpoint_id) const {
    return engine_->fork(source_thread_id, new_thread_id, checkpoint_id);
}

std::optional<json> GraphEngine::get_state(const std::string& thread_id) const {
    AdministrationGuard guard(*this);
    if (!checkpoint_store_) return std::nullopt;
    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt) return std::nullopt;
    return cp_opt->channel_values;
}

std::vector<Checkpoint> GraphEngine::get_state_history(
    const std::string& thread_id, int limit) const {
    AdministrationGuard guard(*this);
    if (!checkpoint_store_) return {};
    return checkpoint_store_->list(thread_id, limit);
}

std::optional<NestedCheckpoint> GraphEngine::inspect_nested_checkpoint(
    const std::string& root_thread_id,
    const std::vector<SubgraphPathStep>& path,
    std::shared_ptr<CheckpointStore> run_checkpoint_store) const {
    if (path.empty())
        throw std::invalid_argument("Nested checkpoint lookup requires a graph path");
    const GraphEngine* engine = this;
    auto store = run_checkpoint_store ? std::move(run_checkpoint_store) : checkpoint_store_;
    std::string thread_id = root_thread_id;
    NestedCheckpoint nested;
    nested.graph_path.reserve(path.size());
    for (const auto& step : path) {
        const auto node = engine->nodes_.find(step.node_name);
        if (node == engine->nodes_.end())
            throw std::out_of_range("Unknown subgraph path node: " + step.node_name);
        const auto* child = dynamic_cast<const SubgraphNode*>(node->second.get());
        if (!child)
            throw std::invalid_argument("Graph path node is not a subgraph: " + step.node_name);
        if (child->persistence() == SubgraphPersistence::Stateless)
            throw std::invalid_argument("Stateless subgraph has no checkpoint to inspect");

        std::optional<Checkpoint> parent_cp;
        if (store) {
            parent_cp = step.parent_checkpoint_id.empty()
                ? store->load_latest(thread_id)
                : store->load_by_id(step.parent_checkpoint_id);
            if (parent_cp && parent_cp->thread_id != thread_id)
                throw std::invalid_argument("Checkpoint does not belong to the graph path");
        }
        std::string graph_invocation_id;
        if (child->persistence() == SubgraphPersistence::PerInvocation) {
            if (!parent_cp) return std::nullopt;
            const auto& metadata = parent_cp->metadata;
            if (!metadata.is_object() || !metadata.contains("_neograph") ||
                !metadata["_neograph"].is_object() ||
                !metadata["_neograph"].contains("subgraph_invocation_id") ||
                !metadata["_neograph"]["subgraph_invocation_id"].is_string())
                throw std::runtime_error(
                    "PerInvocation parent checkpoint lacks graph invocation identity");
            graph_invocation_id =
                metadata["_neograph"]["subgraph_invocation_id"].get<std::string>();
        }
        thread_id = child->checkpoint_thread_id(
            thread_id, step.parent_step, step.task_id, graph_invocation_id);
        engine = &child->child_engine();
        if (!store) store = engine->checkpoint_store_;
        nested.graph_path.push_back(step.node_name);
    }
    if (!store) return std::nullopt;
    auto checkpoint = store->load_latest(thread_id);
    if (!checkpoint) return std::nullopt;
    nested.thread_id = std::move(thread_id);
    nested.checkpoint = std::move(*checkpoint);
    return nested;
}

void GraphEngine::update_state(const std::string& thread_id,
                               const json& channel_writes,
                               const std::string& as_node) {
    std::vector<ChannelWrite> writes;
    if (channel_writes.is_object()) {
        GraphState state;
        init_state(state);
        auto known = state.channel_names();
        for (const auto& [key, value] : channel_writes.items()) {
            if (std::find(known.begin(), known.end(), key) != known.end()) {
                writes.push_back({key, value});
            }
        }
    }
    update_state_writes(thread_id, writes, as_node);
}

void GraphEngine::update_state_writes(
    const std::string& thread_id,
    const std::vector<ChannelWrite>& channel_writes,
    const std::string& as_node) {
    AdministrationGuard admin_guard(*this);
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot update_state: no checkpoint store configured");

    auto cp_opt = checkpoint_store_->load_latest(thread_id);
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for thread: " + thread_id);
    auto& cp = *cp_opt;
    if (!checkpoint_has_managed_bank(cp.channel_values) &&
        checkpoint_store_->requires_managed_budget(thread_id))
        reject_missing_original_managed_bank();

    GraphState state;
    init_state(state);
    state.defer_budget_authority_restore();
    state.restore_checkpoint(cp.channel_values, checkpoint_ephemeral_guard(cp.metadata), cp.native_history);
    state.validate_budget_context(managed_budget_graph_identity(), thread_id);
    std::shared_ptr<OwnedManagedBudgetLease> lease;
    if (state.budget_original_ceiling() != 0) {
        const auto& bank_data = cp.channel_values.at("provider_managed_budget").at("data");
        const auto owner = bank_data.at("owner_scope").get<std::string>();
        if (native_history_archive_ && native_history_archive_->owner_scope() != owner)
            throw std::invalid_argument("Managed archive differs from the original bank owner scope");
        std::optional<std::chrono::steady_clock::time_point> deadline;
        auto scope = original_managed_budget_scope(&cp, state.budget_original_ceiling(),
            owner, state.budget_original_thread_id(), managed_budget_graph_identity(), deadline);
        lease = checkpoint_store_->acquire_managed_budget_lease(scope, cp.id, managed_budget_checkpoint_commitment(cp));
        if (!lease) throw std::invalid_argument("Checkpoint backend returned no owned budget lease");
    }
    std::exception_ptr update_error;
    try {
    if (lease) {
        if (lease->bank_generation() != cp.metadata.at("_neograph_managed_budget_scope").at("bank_generation").get<std::string>())
            throw std::invalid_argument("Managed checkpoint generation differs from its original journal");
        if (native_history_archive_)
            detail::ManagedBudgetJournalAccess::bind_native_archive(lease, native_history_archive_);
    }
    state.activate_budget_authority();

    state.apply_writes(channel_writes);
    const auto ephemeral_guard = state.ephemeral_checkpoint_guard();
    if (!ephemeral_guard.is_null()) {
        for (const auto& [name, written] : ephemeral_guard.items()) {
            if (written == true)
                throw std::runtime_error("Cannot update checkpoint with ephemeral channel write: " +
                                         name);
        }
    }

    Checkpoint new_cp;
    new_cp.id              = Checkpoint::generate_id();
    new_cp.thread_id       = thread_id;
    if (lease ? detail::ManagedBudgetJournalAccess::retains_native_checkpoint(lease)
              : checkpoint_store_->retains_native_checkpoint()) {
        auto snapshot = state.checkpoint_snapshot();
        new_cp.channel_values = std::move(snapshot.first);
        new_cp.native_history = std::move(snapshot.second);
    } else new_cp.channel_values = state.serialize();
    new_cp.native_subgraph_writes = cp.native_subgraph_writes;
    new_cp.metadata        = cp.metadata;
    new_cp.metadata["_neograph"]["admin_resume_phase"] = to_string(
        as_node.empty() ? detail::checkpoint_resume_phase(cp) : CheckpointPhase::Updated);
    if (!ephemeral_guard.is_null() ||
        (new_cp.metadata.is_object() && new_cp.metadata.contains("_neograph_ephemeral_guard")))
        new_cp.metadata["_neograph_ephemeral_guard"] = ephemeral_guard;
    new_cp.parent_id       = cp.id;
    new_cp.current_node    = as_node.empty() ? cp.current_node : as_node;
    new_cp.next_nodes      = cp.next_nodes;
    new_cp.interrupt_phase = CheckpointPhase::Updated;
    // Barrier accumulators must survive an admin update: if a user
    // update_states during an in-flight AND-join, dropping barrier_state
    // would silently discard partial arrivals. Coordinator-driven
    // super-step saves propagate this for the same reason.
    new_cp.barrier_state   = cp.barrier_state;
    new_cp.step            = cp.step;
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    // update_state preserves the parent's step, so millisecond timestamp ties
    // would leave durable stores unable to identify the newer checkpoint.
    new_cp.timestamp       = std::max(now, cp.timestamp + 1);

    if (lease) checkpoint_store_->publish_managed_budget_checkpoint(lease, new_cp);
    else checkpoint_store_->save(new_cp);
    } catch (...) {
        update_error = std::current_exception();
    }
    if (lease) {
        try { checkpoint_store_->release_managed_budget_lease(lease); }
        catch (...) {
            auto outcomes = state.provider_outcomes();
            throw ManagedBudgetLeaseReleaseError(outcomes.empty() ? sp::runtime::Result{} : outcomes.back(),
                update_error, std::current_exception());
        }
    }
    if (update_error) std::rethrow_exception(update_error);
}

std::string GraphEngine::fork(const std::string& source_thread_id,
                               const std::string& new_thread_id,
                               const std::string& checkpoint_id) {
    AdministrationGuard guard(*this);
    if (!checkpoint_store_)
        throw std::runtime_error("Cannot fork: no checkpoint store configured");

    std::optional<Checkpoint> cp_opt;
    if (checkpoint_id.empty()) {
        cp_opt = checkpoint_store_->load_latest(source_thread_id);
    } else {
        cp_opt = checkpoint_store_->load_by_id(checkpoint_id);
    }
    if (!cp_opt)
        throw std::runtime_error("No checkpoint found for fork source");
    if (cp_opt->thread_id != source_thread_id) {
        throw std::runtime_error(
            "Checkpoint does not belong to fork source thread: " +
            source_thread_id);
    }
    if (!checkpoint_has_managed_bank(cp_opt->channel_values) &&
        checkpoint_store_->requires_managed_budget(source_thread_id))
        reject_missing_original_managed_bank();

    GraphState fork_state;
    init_state(fork_state);
    fork_state.defer_budget_authority_restore();
    fork_state.restore_checkpoint(cp_opt->channel_values, checkpoint_ephemeral_guard(cp_opt->metadata), cp_opt->native_history);
    fork_state.validate_budget_context(managed_budget_graph_identity(), source_thread_id);
    fork_state.rebind_fork_budget_thread(new_thread_id, bool(cp_opt->native_history));
    fork_state.activate_budget_authority();
    Checkpoint forked;
    forked.id              = Checkpoint::generate_id();
    forked.thread_id       = new_thread_id;
    if (checkpoint_store_->retains_native_checkpoint()) {
        auto snapshot = fork_state.checkpoint_snapshot();
        forked.channel_values = std::move(snapshot.first);
        forked.native_history = std::move(snapshot.second);
    } else forked.channel_values = fork_state.serialize();
    forked.native_subgraph_writes = cp_opt->native_subgraph_writes;
    forked.channel_versions = cp_opt->channel_versions;
    forked.parent_id       = cp_opt->id;
    forked.current_node    = cp_opt->current_node;
    forked.next_nodes      = cp_opt->next_nodes;
    forked.interrupt_phase = cp_opt->interrupt_phase;
    // Copy barrier_state so a fork taken mid-AND-join resumes with the
    // same partial-arrival accumulator as its source.
    forked.barrier_state   = cp_opt->barrier_state;
    forked.metadata        = cp_opt->metadata.is_object() ? cp_opt->metadata : json::object();
    forked.metadata["forked_from"] = {
        {"thread_id", source_thread_id},
        {"checkpoint_id", cp_opt->id}
    };
    forked.step            = cp_opt->step;
    forked.timestamp       = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (fork_state.budget_managed() && fork_state.budget_original_ceiling() != 0)
        checkpoint_store_->publish_managed_budget_fork(*cp_opt, forked);
    else checkpoint_store_->save(forked);
    return forked.id;
}

// =========================================================================
// State helpers
// =========================================================================

void GraphEngine::init_state(GraphState& state) const {
    executor_->init_state(state);
    state.set_native_history_archive(native_history_archive_);
}

void GraphEngine::apply_input(GraphState& state, const json& input) const {
    if (!input.is_object()) return;
    auto known = state.channel_names();
    for (const auto& [key, value] : input.items()) {
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            state.write(key, value);
        }
    }
}

// =========================================================================
// run / run_stream / resume
// =========================================================================

RunResult GraphEngine::run(const RunConfig& config) {
    return run(config, {});
}

RunResult GraphEngine::run(const RunConfig& config, const RunMetadata& metadata) {
    // Drive the async super-step loop on a single-threaded io_context
    // owned by the caller's stack — the coroutine machinery adds
    // roughly a promise/future per call, so we skip the extra
    // thread-pool hop. Parallel fan-out inside run_parallel_async /
    // run_sends_async still uses pool_ explicitly for CPU
    // parallelism (see NodeExecutor::fan_out_pool_).
    RunConfig operation_config = config;
    if (config.cancel_token) {
        operation_config.cancel_token = config.cancel_token->fork();
    }
    return neograph::async::detail::run_sync_operation(
        execute_graph_async(operation_config, nullptr, {}, nullptr, metadata),
        operation_config.cancel_token);
}

// Public async entry — takes RunConfig BY VALUE so the coroutine frame
// owns its own copy. Callers commonly co_spawn this awaitable, defer
// the await, and let the source RunConfig go out of scope before the
// engine actually drives the run — that pattern would dangling-
// reference the config if we took const& here. ASan caught this in
// the concurrent-stress test (commit-introducing-this-fix). The
// internal `execute_graph_async` still takes const& because its
// callers (run_sync, run_async, resume_async) all keep the config
// alive on their own stack frame.
asio::awaitable<RunResult>
GraphEngine::run_async(RunConfig config) {
    co_return co_await run_async(std::move(config), {});
}

asio::awaitable<RunResult>
GraphEngine::run_async(RunConfig config, RunMetadata metadata) {
    co_return co_await run_async_with_runtime(
        std::move(config), nullptr, std::move(metadata), {});
}

asio::awaitable<RunResult> GraphEngine::run_async(
    RunConfig config, RunMetadata metadata, RunResources resources) {
    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store = std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller =
        std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker =
        std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    co_return co_await run_async_with_runtime(
        std::move(config), nullptr, std::move(metadata),
        std::move(runtime_resources));
}

asio::awaitable<RunResult> GraphEngine::run_async_with_runtime(
    RunConfig config,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RuntimeResources resources) {
    struct SafePointRequestCloseGuard {
        std::shared_ptr<GraphSafePointRequest> request;
        ~SafePointRequestCloseGuard() {
            if (request) request->reject();
        }
    } safe_point_close_guard{resources.safe_point_request};
    auto operation = config.cancel_token
        ? config.cancel_token->fork()
        : std::shared_ptr<CancelToken>{};
    config.cancel_token = operation;
    if (!operation) {
        co_return co_await execute_graph_async(
            config, cb, {}, nullptr, metadata, &resources);
    }

    auto executor = co_await asio::this_coro::executor;
    const auto operation_executor = operation->bind_executor(executor);
    CancelExecutorLease operation_lease(operation);
    operation->throw_if_cancelled("run_async entry");

    // ``operation`` is the token exposed through RunContext. Keep the
    // wrapper's co_spawn on a separate child signal: an async node is allowed
    // to bind operation->slot() for its own timer/socket, and Asio cancellation
    // slots have only one mutable handler.
    auto execution = operation->fork();
    const auto execution_executor = execution->bind_executor(operation_executor);
    CancelExecutorLease execution_lease(execution);
    co_await asio::post(execution_executor, asio::use_awaitable);
    execution->throw_if_cancelled("run_async execution entry");
    try {
        co_return co_await asio::co_spawn(
            execution_executor,
            execute_graph_async(config, cb, {}, nullptr, metadata, &resources),
            asio::bind_cancellation_slot(
                execution->slot(), asio::use_awaitable));
    } catch (const asio::system_error& error) {
        if (operation->is_cancelled() &&
            error.code() == asio::error::operation_aborted) {
            throw CancelledException("run_async operation aborted");
        }
        throw;
    }
}

RunResult GraphEngine::run_stream(const RunConfig& config,
                                   const GraphStreamCallback& cb) {
    return run_stream(config, cb, {});
}

RunResult GraphEngine::run_stream(const RunConfig& config,
                                   const GraphStreamCallback& cb,
                                   const RunMetadata& metadata) {
    RunConfig operation_config = config;
    if (config.cancel_token) {
        operation_config.cancel_token = config.cancel_token->fork();
    }
    return neograph::async::detail::run_sync_operation(
        execute_graph_async(operation_config, cb, {}, nullptr, metadata),
        operation_config.cancel_token);
}

asio::awaitable<RunResult>
GraphEngine::run_stream_async(RunConfig config,
                               GraphStreamCallback cb) {
    co_return co_await run_stream_async(std::move(config), std::move(cb), {});
}

asio::awaitable<RunResult>
GraphEngine::run_stream_async(RunConfig config,
                              GraphStreamCallback cb,
                              RunMetadata metadata) {
    co_return co_await run_async_with_runtime(
        std::move(config), std::move(cb), std::move(metadata), {});
}

asio::awaitable<RunResult>
GraphEngine::run_stream_async(RunConfig config,
                              GraphStreamCallback cb,
                              RunMetadata metadata,
                              RunResources resources) {
    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store =
            std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller = std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker = std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    co_return co_await run_async_with_runtime(
        std::move(config), std::move(cb), std::move(metadata),
        std::move(runtime_resources));
}

asio::awaitable<RunResult> GraphEngine::run_until_safe_point_async(
    RunConfig config,
    std::shared_ptr<GraphSafePointRequest> request,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RunResources resources) {
    if (!request) {
        throw std::invalid_argument(
            "run_until_safe_point_async requires a safe-point request");
    }
    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store = std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller = std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker = std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    runtime_resources.safe_point_request = std::move(request);
    co_return co_await run_async_with_runtime(
        std::move(config), std::move(cb), std::move(metadata),
        std::move(runtime_resources));
}

RunResult GraphEngine::resume(const std::string& thread_id,
                               const json& resume_value,
                               const GraphStreamCallback& cb) {
    RunConfig config;
    config.thread_id = thread_id;
    return resume(config, resume_value, cb);
}

RunResult GraphEngine::resume(const RunConfig&           config,
                               const json&                resume_value,
                               const GraphStreamCallback& cb) {
    return neograph::async::run_sync(resume_async(config, resume_value, cb));
}

RunResult GraphEngine::resume(const RunConfig&           config,
                               const json&                resume_value,
                               const GraphStreamCallback& cb,
                               const RunMetadata&         metadata) {
    return neograph::async::run_sync(
        resume_async(config, resume_value, cb, metadata));
}

asio::awaitable<RunResult> GraphEngine::resume_async(const std::string&         thread_id,
                                                     const json&                resume_value,
                                                     const GraphStreamCallback& cb) {
    RunConfig config;
    config.thread_id = thread_id;
    return resume_async(
        std::move(config), json(resume_value), GraphStreamCallback(cb));
}

asio::awaitable<RunResult> GraphEngine::resume_async(RunConfig           config,
                                                       json                resume_value,
                                                       GraphStreamCallback cb) {
    co_return co_await resume_async_with_runtime(
        std::move(config), std::move(resume_value), std::move(cb), {}, {});
}

asio::awaitable<RunResult> GraphEngine::resume_async(
    RunConfig config,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata) {
    co_return co_await resume_async_with_runtime(
        std::move(config), std::move(resume_value), std::move(cb),
        std::move(metadata), {});
}

asio::awaitable<RunResult> GraphEngine::resume_async(
    RunConfig config,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RunResources resources) {
    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store =
            std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller = std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker = std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    co_return co_await resume_async_with_runtime(
        std::move(config), std::move(resume_value), std::move(cb),
        std::move(metadata), std::move(runtime_resources));
}

RunResult GraphEngine::resume_from(
    RunConfig config,
    std::string checkpoint_id,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RunResources resources) {
    return neograph::async::run_sync(resume_from_async(
        std::move(config), std::move(checkpoint_id), std::move(resume_value),
        std::move(cb), std::move(metadata), std::move(resources)));
}

asio::awaitable<RunResult> GraphEngine::resume_from_async(
    RunConfig config,
    std::string checkpoint_id,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RunResources resources) {
    if (checkpoint_id.empty()) {
        throw std::invalid_argument(
            "Cannot resume from checkpoint: checkpoint_id is required");
    }

    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store =
            std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller = std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker = std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    co_return co_await resume_async_with_runtime(
        std::move(config), std::move(resume_value), std::move(cb),
        std::move(metadata), std::move(runtime_resources),
        std::move(checkpoint_id));
}

asio::awaitable<RunResult> GraphEngine::resume_from_until_safe_point_async(
    RunConfig config,
    std::string checkpoint_id,
    std::shared_ptr<GraphSafePointRequest> request,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RunResources resources) {
    if (!request) {
        throw std::invalid_argument(
            "resume_from_until_safe_point_async requires a safe-point request");
    }
    if (checkpoint_id.empty()) {
        request->reject();
        throw std::invalid_argument(
            "Cannot resume to a safe point without a checkpoint id");
    }
    RuntimeResources runtime_resources;
    if (resources.checkpoint_store) {
        runtime_resources.checkpoint_store = std::move(resources.checkpoint_store);
    }
    if (resources.store) {
        runtime_resources.store = std::move(resources.store);
    }
    if (resources.tool_gate) {
        runtime_resources.parent_tool_gate = std::move(resources.tool_gate);
    }
    runtime_resources.tool_execution_controller = std::move(resources.tool_execution_controller);
    runtime_resources.provider_call_broker = std::move(resources.provider_call_broker);
    runtime_resources.tool_effect_broker = std::move(resources.tool_effect_broker);
    runtime_resources.tool_effect_grant = std::move(resources.tool_effect_grant);
    runtime_resources.safe_point_request = std::move(request);
    co_return co_await resume_async_with_runtime(
        std::move(config), std::move(resume_value), std::move(cb),
        std::move(metadata), std::move(runtime_resources),
        std::move(checkpoint_id));
}

asio::awaitable<RunResult> GraphEngine::resume_async_with_runtime(
    RunConfig config,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RuntimeResources resources,
    std::optional<std::string> checkpoint_id) {
    struct SafePointRequestCloseGuard {
        std::shared_ptr<GraphSafePointRequest> request;
        ~SafePointRequestCloseGuard() {
            if (request) request->reject();
        }
    } safe_point_close_guard{resources.safe_point_request};
    auto operation = config.cancel_token
        ? config.cancel_token->fork()
        : std::shared_ptr<CancelToken>{};
    config.cancel_token = operation;
    if (!operation) {
        co_return co_await resume_execute_async(
            std::move(config), std::move(resume_value), std::move(cb),
            std::move(metadata), std::move(resources),
            std::move(checkpoint_id));
    }

    auto executor = co_await asio::this_coro::executor;
    const auto operation_executor = operation->bind_executor(executor);
    CancelExecutorLease operation_lease(operation);
    operation->throw_if_cancelled("resume_async entry");

    // Keep the RunContext token and the resume wrapper's co_spawn on
    // independent cancellation signals; nested node consumers may bind the
    // former without replacing the latter's awaitable handler.
    auto execution = operation->fork();
    const auto execution_executor = execution->bind_executor(operation_executor);
    CancelExecutorLease execution_lease(execution);
    co_await asio::post(execution_executor, asio::use_awaitable);
    execution->throw_if_cancelled("resume_async execution entry");
    try {
        co_return co_await asio::co_spawn(
            execution_executor,
            resume_execute_async(
                std::move(config), std::move(resume_value), std::move(cb),
                std::move(metadata), std::move(resources),
                std::move(checkpoint_id)),
            asio::bind_cancellation_slot(
                execution->slot(), asio::use_awaitable));
    } catch (const asio::system_error& error) {
        if (operation->is_cancelled() &&
            error.code() == asio::error::operation_aborted) {
            throw CancelledException("resume_async operation aborted");
        }
        throw;
    }
}

asio::awaitable<RunResult> GraphEngine::resume_execute_async(
    RunConfig config,
    json resume_value,
    GraphStreamCallback cb,
    RunMetadata metadata,
    RuntimeResources resources,
    std::optional<std::string> checkpoint_id) {
    struct SafePointRequestCloseGuard {
        std::shared_ptr<GraphSafePointRequest> request;
        ~SafePointRequestCloseGuard() {
            if (request) request->reject();
        }
    } safe_point_close_guard{resources.safe_point_request};
    // Hold admission across checkpoint lookup and the resumed super-step loop.
    ExecutionGuard execution_guard(*this);
    auto checkpoint_store = resources.checkpoint_store
        ? *resources.checkpoint_store
        : checkpoint_store_;
    if (!checkpoint_store) {
        throw std::runtime_error(
            "Cannot resume: no checkpoint store configured");
    }
    if (config.thread_id.empty()) {
        throw std::invalid_argument("Cannot resume: thread_id is required");
    }

    CheckpointHookContext hook_context;
    if (hook_runtime_) {
        hook_context = {metadata.owner_scope, metadata.run_id, metadata.trace_id,
                        config.cancel_token, hook_deadline_for(metadata.deadline)};
    }
    CheckpointCoordinator coordinator(checkpoint_store, config.thread_id, hook_runtime_,
                                      std::move(hook_context),
                                      config.native_history_archive ? config.native_history_archive : native_history_archive_);
    ResumeContext resume_context;
    if (checkpoint_id) {
        resume_context =
            co_await coordinator.load_for_resume_by_id_async(*checkpoint_id);
    } else {
        resume_context = co_await coordinator.load_for_resume_async();
    }
    if (!resume_context.have_cp) {
        if (checkpoint_id) {
            throw std::runtime_error(
                "No checkpoint found for id: " + *checkpoint_id);
        }
        throw std::runtime_error(
            "No checkpoint found for thread: " + config.thread_id);
    }

    if (resume_context.next_nodes.size() == 1 &&
        resume_context.next_nodes[0] == std::string(END_NODE)) {
        if (!checkpoint_has_managed_bank(resume_context.channel_values) &&
            co_await checkpoint_store->requires_managed_budget_async(config.thread_id))
            reject_missing_original_managed_bank();
        GraphState completed_state;
        init_state(completed_state);
        if (config.native_history_archive) completed_state.set_native_history_archive(config.native_history_archive);
        const auto archive = config.native_history_archive ? config.native_history_archive : native_history_archive_;
        const bool finite_standalone = config.model_token_budget != 0 && !config.usage && !resources.provider_call_broker;
        const std::string_view budget_owner = metadata.owner_scope.empty() && archive && finite_standalone
            ? archive->owner_scope() : std::string_view(metadata.owner_scope);
        auto bank = config.usage ? config.usage : std::make_shared<UsageAccumulator>();
        completed_state.configure_budget_bank(bank, config.model_token_budget,
            !config.usage && !resources.provider_call_broker, std::string(budget_owner), config.thread_id,
            managed_budget_graph_identity());
        completed_state.defer_budget_authority_restore();
        completed_state.restore_checkpoint(
            resume_context.channel_values, checkpoint_ephemeral_guard(resume_context.metadata), resume_context.native_history);
        if (checkpoint_has_managed_bank(resume_context.channel_values) &&
            completed_state.budget_original_ceiling() != 0) {
            auto deadline = metadata.deadline;
            const std::string_view original_owner = metadata.owner_scope.empty() && archive
                ? archive->owner_scope() : std::string_view(metadata.owner_scope);
            (void)original_managed_budget_scope(resume_context.managed_budget_source.get(),
                completed_state.budget_original_ceiling(), original_owner, completed_state.budget_original_thread_id(),
                managed_budget_graph_identity(), deadline);
        }
        completed_state.activate_budget_authority(true);
        RunResult result;
        result.output = resume_context.channel_values;
        result.native_messages = completed_state.captured_provider_messages().value_or(std::vector<sp::Message>{});
        result.provider_outcomes = completed_state.provider_outcomes();
        if (const auto bank = completed_state.budget_bank()) result.usage = bank->snapshot();

        result.checkpoint_id = resume_context.checkpoint_id;
        co_return result;
    }

    // Keep the historical latest-resume behavior for snapshots with no
    // continuation: the legacy path treated an empty next_nodes vector as a
    // fresh run. Exact resume remains pinned to the requested snapshot.
    if (!checkpoint_id && resume_context.next_nodes.empty()) {
        co_return co_await execute_graph_async(
            config, cb, std::nullopt, &resume_value, metadata, &resources);
    }

    co_return co_await execute_graph_async(
        config, cb, std::move(resume_context), &resume_value,
        metadata, &resources);
}

asio::awaitable<GraphEngine::SubgraphRunResult> GraphEngine::run_subgraph_async(
    RunConfig config,
    const RunContext& parent,
    GraphStreamCallback cb,
    SubgraphPersistence persistence) {
    RunMetadata metadata;
    metadata.deadline            = parent.deadline;
    metadata.trace_id            = parent.trace_id;
    metadata.run_id              = parent.run_id;
    metadata.owner_scope        = parent.tool_execution_identity.owner_scope;
    metadata.budget_cancel_token = parent.budget_cancel_token;

    RuntimeResources resources;
    auto parent_runtime = detail::runtime_for(parent);
    if (parent_runtime && parent_runtime->checkpoint_store) {
        resources.checkpoint_store = parent_runtime->checkpoint_store;
    }
    if (parent.store) {
        resources.store = parent.store;
    }
    resources.parent_tool_gate = parent.tool_gate;
    resources.tool_execution_controller = parent.tool_execution_controller;
    if (parent_runtime) {
        resources.provider_call_broker = parent_runtime->provider_call_broker;
        resources.tool_effect_broker = parent_runtime->tool_effect_broker;
        resources.tool_effect_grant = parent_runtime->tool_effect_grant;
    }
    if (persistence == SubgraphPersistence::Stateless) {
        if (!interrupt_before_.empty() || !interrupt_after_.empty())
            throw std::runtime_error("Stateless subgraph does not support static interrupts");
        if (parent_runtime && parent_runtime->is_resume)
            throw std::runtime_error("Stateless subgraph cannot resume a parent invocation");
        resources.checkpoint_store = std::shared_ptr<CheckpointStore>{};
    }
    auto journal = std::make_shared<detail::SubgraphWriteJournal>();
    if (persistence == SubgraphPersistence::PerThread && parent_runtime) {
        if (parent_runtime->graph_invocation_id.empty())
            throw std::runtime_error("PerThread subgraph requires a parent invocation identity");
        journal->parent_call_id = parent_runtime->graph_invocation_id + "/" +
            std::to_string(parent.step) + "/" + parent_runtime->invocation_id;
    }
    resources.subgraph_write_journal = journal;

    if (parent_runtime && parent_runtime->is_resume &&
        resources.checkpoint_store && *resources.checkpoint_store) {
        auto checkpoint = co_await (*resources.checkpoint_store)->load_latest_async(
            config.thread_id);
        if (checkpoint) {
            bool same_call = true;
            if (persistence == SubgraphPersistence::PerThread) {
                const auto& stored = checkpoint->metadata;
                if (!stored.is_object() || !stored.contains("_neograph") ||
                    !stored["_neograph"].is_object() ||
                    !stored["_neograph"].contains("subgraph_parent_call_id") ||
                    !stored["_neograph"]["subgraph_parent_call_id"].is_string() ||
                    stored["_neograph"]["subgraph_parent_call_id"] == "")
                    throw std::runtime_error(
                        "Cannot resume PerThread subgraph without persisted parent call identity");
                same_call = stored["_neograph"]["subgraph_parent_call_id"] == journal->parent_call_id;
            }
            if (same_call) {
                detail::restore_subgraph_write_journal(*checkpoint, journal, parent.native_history_archive);
                const json resume_value = parent.resume_value
                    ? *parent.resume_value
                    : json();
                auto result = co_await resume_async_with_runtime(
                    std::move(config), resume_value, std::move(cb),
                    std::move(metadata), std::move(resources));
                co_return SubgraphRunResult{
                    std::move(result), std::move(journal->writes)};
            }
        }
    }

    auto result = co_await run_async_with_runtime(
        std::move(config), std::move(cb), std::move(metadata), std::move(resources));
    co_return SubgraphRunResult{
        std::move(result), std::move(journal->writes)};
}

// =========================================================================
// execute_graph_async — super-step loop (coroutine)
// =========================================================================
//   Owns: state init, interrupt_before/after gates, resume load,
//         super-step commit, routing via Scheduler.
//   Delegates: node invocation (single/parallel/Send) → NodeExecutor,
//              checkpoint lifecycle → CheckpointCoordinator,
//              routing decisions → Scheduler.
//
// Sync entry points (run / run_stream / resume) drive this through
// `block_on_pool`, which co_spawns onto the engine's thread_pool
// and blocks via std::future. Async callers on their own executor
// co_await it directly.

asio::awaitable<RunResult>
GraphEngine::execute_graph_async(
    const RunConfig& config,
    const GraphStreamCallback& cb,
    std::optional<ResumeContext> resume_context,
    const json* resume_value,
    const RunMetadata& metadata,
    const RuntimeResources* resources) {
    const bool is_resume = resume_context.has_value();

    // The RAII guard releases admission after every normal, cancelled, or
    // exceptional completion; the resume path holds a second count while
    // loading its checkpoint.
    ExecutionGuard active_run_guard(*this);
    std::shared_ptr<OwnedManagedBudgetLease> owned_lease;
    std::shared_ptr<CheckpointStore> lease_store;
    std::shared_ptr<ProviderOutcomes> owned_outcomes;
    std::size_t source_outcome_count = 0;
    auto execute = [&]() -> asio::awaitable<RunResult> {

    GraphState state;
    init_state(state);
    if (config.native_history_archive) state.set_native_history_archive(config.native_history_archive);

    // v1.0 (9d): the `state.set_run_cancel_token` smuggling channel is
    // gone — cancel flows through `RunContext::cancel_token` (set just
    // below) on every NodeInput.

    StreamMode stream_mode = config.stream_mode;
    auto checkpoint_store = resources && resources->checkpoint_store
        ? *resources->checkpoint_store
        : checkpoint_store_;
    CheckpointHookContext hook_context;
    if (hook_runtime_) {
        hook_context = {metadata.owner_scope, metadata.run_id, metadata.trace_id,
                        config.cancel_token, hook_deadline_for(metadata.deadline)};
    }
    CheckpointCoordinator coord(checkpoint_store, config.thread_id, hook_runtime_,
                                std::move(hook_context),
                                config.native_history_archive ? config.native_history_archive : native_history_archive_,
                                resources ? resources->subgraph_write_journal : nullptr);
    auto safe_point_request = resources ? resources->safe_point_request : nullptr;
    struct SafePointOperationGuard {
        std::shared_ptr<GraphSafePointRequest> request;
        bool attached = false;
        ~SafePointOperationGuard() {
            if (request && attached) request->close();
        }
    } safe_point_guard{safe_point_request};
    const GraphGenerationIdentity* safe_point_generation = nullptr;
    if (safe_point_request) {
        safe_point_generation = bound_generation_identity();
        if (!safe_point_generation ||
            !safe_point_request->attach(*safe_point_generation)) {
            safe_point_request->reject();
            throw std::runtime_error(
                "Graph safe-point request does not match this engine generation");
        }
        safe_point_guard.attached = true;
        if (!coord.enabled()) {
            throw std::runtime_error(
                "Graph safe-point capture requires a checkpoint store and thread id");
        }
    }

    // PR 1 (v0.4.0): build the per-run RunContext from RunConfig and
    // carry it by reference through every NodeExecutor hop. Plumbing-
    // only — consumers (the new ``GraphNode::run(NodeInput)`` virtual,
    // the deprecation of ``state.run_cancel_token`` smuggling, etc.)
    // land in subsequent PRs. ``ctx.step`` is updated at the top of
    // each super-step iteration below so per-step consumers see a
    // consistent value without the engine threading ``int step``
    // separately. See ROADMAP_v1.md "Execution plan" → PR 1.
    RunContext ctx;
    ctx.cancel_token = config.cancel_token;
    ctx.provider_outcomes = config.provider_outcomes ? config.provider_outcomes : std::make_shared<ProviderOutcomes>();
    ctx.native_history_archive = config.native_history_archive ? config.native_history_archive : native_history_archive_;
    ctx.on_provider_event = config.on_provider_event;
    ctx.provider_loop_history = config.provider_loop_history ? config.provider_loop_history : std::make_shared<ProviderLoopHistory>();
    owned_outcomes = ctx.provider_outcomes;
    state.set_provider_run_history(ctx.provider_loop_history, ctx.provider_outcomes);
    // Initialize an accumulator if the caller did not supply one. Checkpoint
    // restoration may replace it with the original bank and its prior reports.
    // Node code always receives a non-null ctx.usage.
    ctx.usage        = config.usage ? config.usage
                                     : std::make_shared<UsageAccumulator>();
    ctx.model_token_budget = config.model_token_budget;
    const bool finite_standalone = config.model_token_budget != 0 && !config.usage &&
        !(resources && resources->provider_call_broker);
    const std::string_view budget_owner = metadata.owner_scope.empty() && ctx.native_history_archive && finite_standalone
        ? ctx.native_history_archive->owner_scope() : std::string_view(metadata.owner_scope);
    state.configure_budget_bank(ctx.usage, ctx.model_token_budget,
        !config.usage && !(resources && resources->provider_call_broker),
        std::string(budget_owner), config.thread_id, managed_budget_graph_identity());
    state.defer_budget_authority_restore();
    ctx.budget_exhausted   = config.budget_exhausted;
    ctx.run_id             = metadata.run_id;
    ctx.budget_cancel_token =
        metadata.budget_cancel_token
            ? metadata.budget_cancel_token
            : (ctx.cancel_token ? ctx.cancel_token->fork()
                                 : std::make_shared<CancelToken>());
    ctx.deadline     = metadata.deadline;
    ctx.trace_id     = metadata.trace_id;
    ctx.thread_id    = config.thread_id;
    ctx.stream_mode  = stream_mode;
    ctx.cache_execution_id = next_cache_execution_id_.fetch_add(
        1, std::memory_order_relaxed);
    ctx.store = resources && resources->store ? *resources->store : store_;
    // issue #94 — the human's answer, readable by any node. Left empty on a
    // fresh run, which is how a node distinguishes "nobody has answered yet"
    // from "the answer was no". Only engaged on an actual resume, so the
    // common path pays no json allocation.
    if (resume_value && !resume_value->is_null()) ctx.resume_value = *resume_value;
    auto parent_tool_gate = resources && resources->parent_tool_gate
        ? *resources->parent_tool_gate
        : ToolGate{};
    ctx.tool_gate = compose_tool_gates(std::move(parent_tool_gate), tool_gate_);
    ctx.tool_execution_controller = resources && resources->tool_execution_controller
        ? resources->tool_execution_controller : tool_execution_controller_;
    ctx.tool_execution_identity.owner_scope = metadata.owner_scope;
    ctx.tool_execution_identity.root_run_id = metadata.run_id;
    ctx.tool_execution_identity.thread_id = config.thread_id;

    auto runtime = std::make_shared<detail::RunContextRuntime>();
    if (has_stateful_subgraph_) {
        if (is_resume) {
            const auto& stored = resume_context->metadata;
            if (!stored.is_object() || !stored.contains("_neograph") ||
                !stored["_neograph"].is_object() ||
                !stored["_neograph"].contains("subgraph_invocation_id") ||
                !stored["_neograph"]["subgraph_invocation_id"].is_string() ||
                stored["_neograph"]["subgraph_invocation_id"].get<std::string>().empty())
                throw std::runtime_error(
                    "Cannot resume stateful subgraph without persisted invocation identity");
            runtime->graph_invocation_id =
                stored["_neograph"]["subgraph_invocation_id"].get<std::string>();
        } else {
            runtime->graph_invocation_id = Checkpoint::generate_id();
        }
    }
    runtime->checkpoint_store = checkpoint_store;
    if (resources) {
        runtime->provider_call_broker = resources->provider_call_broker;
        runtime->tool_effect_broker = resources->tool_effect_broker;
        runtime->tool_effect_grant = resources->tool_effect_grant;
    }
    if (resources) {
        runtime->subgraph_write_journal = resources->subgraph_write_journal;
    }
    runtime->is_resume = is_resume;
    detail::ScopedRunContextRuntime runtime_scope(ctx, std::move(runtime));

    std::string last_checkpoint_id;
    std::shared_ptr<const Checkpoint> budget_source;
    int start_step = 0;

    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;

    std::vector<std::string> ready;
    if (is_resume) {
        auto& loaded = *resume_context;
        if (!checkpoint_has_managed_bank(loaded.channel_values) && checkpoint_store &&
            co_await checkpoint_store->requires_managed_budget_async(config.thread_id))
            reject_missing_original_managed_bank();
        state.restore_checkpoint(
            loaded.channel_values, checkpoint_ephemeral_guard(loaded.metadata), loaded.native_history);
        budget_source = loaded.managed_budget_source;
        last_checkpoint_id = loaded.checkpoint_id;
        start_step         = loaded.start_step;
        ready              = std::move(loaded.next_nodes);
        replay_results     = std::move(loaded.replay_results);
        barrier_state      = std::move(loaded.barrier_state);

        // Chat-shaped graphs have always received the resume value as a
        // user turn on the "messages" channel, and still do. But a graph
        // without that channel used to *throw* here ("Write to unknown
        // channel: 'messages'"), which made resume-with-a-value unusable
        // for anything that isn't a chat — including the approval prompt
        // dynamic interrupts exist for. Every node now reads the answer
        // from ``ctx.resume_value`` regardless; this write stays for the
        // graphs that were relying on it (issue #94).
        if (resume_value && !resume_value->is_null()
            && state.has_channel("messages")) {
            // Build the resume message outside the brace-init that
            // would otherwise nest inside the coroutine body. Same
            // GCC 13 ICE shape; same workaround.
            std::string content = resume_value->is_string()
                ? resume_value->get<std::string>()
                : resume_value->dump();
            json resume_msg;
            resume_msg["role"]    = "user";
            resume_msg["content"] = content;
            state.write("messages", json::array({resume_msg}));
        }
    } else {
        // v0.3.1: opt-in auto-resume. When the caller asks to continue
        // a thread (multi-turn chat), seed the state from the latest
        // checkpoint *before* applying input — so APPEND-reduced
        // channels (messages) grow by the new turn instead of being
        // overwritten. start_step continues monotonically from the
        // checkpoint's recorded step so per-thread step numbering
        // stays meaningful in the trace.
        if (config.resume_if_exists && checkpoint_store) {
            auto cp_opt =
                co_await checkpoint_store->load_latest_async(config.thread_id);
            if (cp_opt) {
                if (cp_opt->step < 0 ||
                    cp_opt->step >= std::numeric_limits<int>::max()) {
                    throw std::runtime_error(
                        "Checkpoint step exceeds the executable range");
                }
                if (!checkpoint_has_managed_bank(cp_opt->channel_values) &&
                    co_await checkpoint_store->requires_managed_budget_async(config.thread_id))
                    reject_missing_original_managed_bank();
                state.restore_checkpoint(
                    cp_opt->channel_values, checkpoint_ephemeral_guard(cp_opt->metadata), cp_opt->native_history);
                last_checkpoint_id = cp_opt->id;
                start_step = static_cast<int>(cp_opt->step + 1);
                budget_source = std::make_shared<const Checkpoint>(std::move(*cp_opt));
            }
        }
        if (last_checkpoint_id.empty() && checkpoint_store &&
            co_await checkpoint_store->requires_managed_budget_async(config.thread_id))
            reject_missing_original_managed_bank();
        apply_input(state, config.input);
    }
    if (state.budget_managed() && state.budget_original_ceiling() != 0 && coord.enabled()) {
        if (!last_checkpoint_id.empty() && !budget_source)
            throw std::invalid_argument("Managed resume lacks the full original checkpoint source");
        const std::string_view original_owner = metadata.owner_scope.empty() && ctx.native_history_archive
            ? ctx.native_history_archive->owner_scope() : std::string_view(metadata.owner_scope);
        if (ctx.native_history_archive && ctx.native_history_archive->owner_scope() != original_owner)
            throw std::invalid_argument("Managed archive differs from the original bank owner scope");
        auto scope = original_managed_budget_scope(budget_source.get(), state.budget_original_ceiling(),
            original_owner, state.budget_original_thread_id(), managed_budget_graph_identity(), ctx.deadline);
        lease_store = checkpoint_store;
        owned_lease = co_await checkpoint_store->acquire_managed_budget_lease_async(
            std::move(scope), budget_source ? budget_source->id : std::string{},
            budget_source ? managed_budget_checkpoint_commitment(*budget_source) : std::string{});
        if (!owned_lease) throw std::invalid_argument("Checkpoint backend returned no owned budget lease");
        if (budget_source && owned_lease->bank_generation() !=
            budget_source->metadata.at("_neograph_managed_budget_scope").at("bank_generation").get<std::string>())
            throw std::invalid_argument("Managed checkpoint generation differs from its original journal");
        if (ctx.native_history_archive)
            detail::ManagedBudgetJournalAccess::bind_native_archive(owned_lease, ctx.native_history_archive);
        ctx.managed_budget_lease = owned_lease;
        ctx.managed_budget_store = checkpoint_store;
        coord.set_managed_budget_lease(owned_lease);
        ctx.tool_execution_identity.owner_scope = owned_lease->scope().owner_scope;
    }
    state.activate_budget_authority();
    if (owned_lease) {
        std::lock_guard lock(owned_outcomes->mutex);
        source_outcome_count = owned_outcomes->values.size();
    }
    if (const auto bank = state.budget_bank()) {
        ctx.usage = bank;
        ctx.model_token_budget = state.budget_ceiling();
    }
    if (config.provider_messages) {
        json projected = json::array();
        for (const auto& message : *config.provider_messages) {
            json item;
            to_json(item, project_message(message));
            projected.push_back(std::move(item));
        }
        state.apply_writes({ChannelWrite{"messages", std::move(projected), ChannelWrite::Mode::Overwrite,
            std::make_shared<const std::vector<sp::Message>>(*config.provider_messages)}});
    }

    if (!is_resume) {
        ready = scheduler_->plan_start_step();
    }
    if (owned_lease && !budget_source) {
        last_checkpoint_id = co_await coord.save_super_step_async(
            state, "__managed_bank_root__", ready, CheckpointPhase::Before, start_step,
            last_checkpoint_id, barrier_state, detail::checkpoint_metadata_for(ctx));
    }

    std::vector<std::string> trace;
    bool hit_end = false;
    std::vector<Send> pending_sends;
    if (safe_point_request &&
        start_step >= std::numeric_limits<int>::max() - 1) {
        throw std::runtime_error(
            "Checkpoint step cannot reach another capturable safe point");
    }
    const auto end_step = static_cast<std::int64_t>(start_step) +
                          std::max(config.max_steps, 0);
    for (std::int64_t step_value = start_step;
         step_value < end_step; ++step_value) {
        if (step_value > std::numeric_limits<int>::max()) {
            throw std::runtime_error("Graph super-step index exceeds the executable range");
        }
        const int step = static_cast<int>(step_value);
        ctx.step = step;
        // A budget-aware node cancels the shared sibling scope, not the
        // operation token that owns this graph. Finish the current
        // super-step, persist its exact state, and return a step-limit
        // result so the Program layer can publish a durable terminal record.
        if (ctx.budget_exhausted &&
            ctx.budget_exhausted->load(std::memory_order_acquire)) {
            if (coord.enabled()) {
                const std::string trace_tag =
                    trace.empty() ? (std::string("__budget__") + std::to_string(step))
                                  : trace.back();
                const std::string parent_cp_id = last_checkpoint_id;
                auto checkpoint = co_await coord.commit_super_step_async(
                    state, trace_tag, ready, step, parent_cp_id, barrier_state,
                    detail::checkpoint_metadata_for(ctx));
                last_checkpoint_id = std::move(checkpoint.id);
            }
            RunResult result;
            result.usage = ctx.usage->snapshot();
            result.output = state.serialize_runtime();
            result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
            result.provider_outcomes = ctx.provider_outcomes->snapshot();
            if (!ready.empty()) result.output["_neograph"] = json{{"max_steps_exhausted", true}};
            result.checkpoint_id = last_checkpoint_id;
            result.execution_trace = std::move(trace);
            co_return result;
        }
        if (ctx.cancel_token) {
            ctx.cancel_token->throw_if_cancelled(
                "step " + std::to_string(step));
        }

        // hit_end is informational only — a Send target with no outgoing
        // edges resolves to __end__ in the scheduler and flips hit_end,
        // but that shouldn't kill the loop when other ready paths still
        // have work (e.g. dispatch → supervisor static edge fires after
        // dispatch's researcher Sends complete; previously the
        // researchers' implicit __end__ resolution shadowed supervisor
        // and ended the run early — visible on example 25 deep_research).
        // Command{__end__} terminates by leaving ready empty (plan_impl
        // pass 1, scheduler.cpp:79-83), so ready.empty() is sufficient.
        if (ready.empty()) break;

        // --- interrupt_before check ---
        bool is_resume_entry = (is_resume && step == start_step);
        if (!is_resume_entry) {
            for (const auto& node_name : ready) {
                if (interrupt_before_.count(node_name) && coord.enabled()) {
                    auto cp_id = co_await coord.save_super_step_async(state,
                        node_name, ready,
                        CheckpointPhase::Before, step, last_checkpoint_id,
                        barrier_state, detail::checkpoint_metadata_for(ctx));

                    RunResult result;
                    result.usage = ctx.usage->snapshot();   // #88
                    result.output = state.serialize_runtime();
                    result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
                    result.provider_outcomes = ctx.provider_outcomes->snapshot();
                    result.interrupted     = true;
                    result.interrupt_node  = node_name;
                    json iv;
                    iv["message"] = "Interrupt before node: " + node_name;
                    result.interrupt_value = iv;
                    result.checkpoint_id   = cp_id;
                    result.execution_trace = std::move(trace);

                    if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                        json data;
                        data["phase"]         = "before";
                        data["checkpoint_id"] = cp_id;
                        cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                                      std::move(data)});
                    }
                    co_return result;
                }
            }
        }

        // --- Execute ready nodes ---
        pending_sends.clear();
        std::vector<NodeResult> step_results;

        // Capture NodeInterrupt outside the catch (GCC-13-safe) so the
        // checkpoint lookup that follows can do its own work.
        // One empty optional, not a bool + two strings + a json: this runs on
        // every super-step of every run, and a default-constructed `json`
        // allocates a yyjson document. An empty optional allocates nothing,
        // and the NodeInterrupt inside it is only ever built on the cold path.
        std::optional<NodeInterrupt> interrupt;

        try {
            if (ready.size() == 1) {
                step_results.push_back(co_await executor_->run_one_async(
                    ready[0], step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode, ctx));
            } else {
                // Sem 3.7: full async fan-out via
                // asio::experimental::make_parallel_group. The
                // io_context's worker thread now stays free for other
                // coroutines while the parallel branches run.
                step_results = co_await executor_->run_parallel_async(
                    ready, step, state, replay_results,
                    coord, last_checkpoint_id, barrier_state,
                    trace, cb, stream_mode, ctx);
            }
        } catch (const NodeInterrupt& ni) {
            interrupt = ni;
        }

        if (interrupt) {
            RunResult result;
            result.usage = ctx.usage->snapshot();   // #88
            result.output = state.serialize_runtime();
            result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
            result.provider_outcomes = ctx.provider_outcomes->snapshot();
            result.interrupted     = true;
            // issue #94: the node that paused — not its reason. The executor
            // stamps ni.node(); the fallback covers a NodeInterrupt that
            // reached here without passing through it, which would otherwise
            // leave the caller with an empty string.
            result.interrupt_node  = interrupt->node().empty()
                                       ? interrupt->reason()
                                       : interrupt->node();
            json iv;
            iv["reason"] = interrupt->reason();
            iv["type"]   = "NodeInterrupt";
            // Absent — not null — when the node attached no payload, so
            // `contains("value")` answers "did the node hand me something".
            if (!interrupt->value().is_null()) iv["value"] = interrupt->value();
            result.interrupt_value = iv;
            result.execution_trace = std::move(trace);

            if (coord.enabled()) {
                auto cp_opt = co_await coord.store()->load_latest_async(coord.thread_id());
                if (cp_opt) result.checkpoint_id = cp_opt->id;
            }
            co_return result;
        }

        // --- Collect Send requests ---
        for (std::size_t i = 0; i < step_results.size(); ++i) {
            auto& nr = step_results[i];
            for (auto& s : nr.sends) {
                s.source_node = ready[i];
                pending_sends.push_back(std::move(s));
            }
        }

        // --- Execute pending Sends BEFORE interrupt_after ---
        // Sem 3.7.6: async fan-out, parallel_group-backed.
        // 4.x: per-task StepRoutings flow back so each spawned task's
        //      Command.goto / default outgoing edge contribute to the
        //      next super-step routing decision (LangGraph parity).
        auto send_routings = co_await executor_->run_sends_async(
            pending_sends, step, state, replay_results,
            coord, last_checkpoint_id, trace, cb, stream_mode, ctx);

        if (cb && has_mode(stream_mode, StreamMode::VALUES)) {
            cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, "__state__",
                          state.serialize_runtime()});
        }

        // --- interrupt_after check ---
        for (const auto& node_name : ready) {
            if (interrupt_after_.count(node_name) && coord.enabled()) {
                std::set<std::string> union_next;
                for (const auto& rn : ready) {
                    for (const auto& nx : scheduler_->resolve_next_nodes(rn, state)) {
                        union_next.insert(nx);
                    }
                }
                // Send-spawned tasks also influence the next super-step;
                // include their goto / default edges in the snapshot so
                // a checkpoint resumed after the interrupt knows the
                // full successor set.
                for (const auto& sr : send_routings) {
                    if (sr.command_goto) {
                        union_next.insert(*sr.command_goto);
                    } else {
                        for (const auto& nx :
                                scheduler_->resolve_next_nodes(sr.node_name, state)) {
                            union_next.insert(nx);
                        }
                    }
                }
                std::vector<std::string> nexts(union_next.begin(), union_next.end());
                if (nexts.empty()) nexts.push_back(std::string(END_NODE));

                auto cp_id = co_await coord.save_super_step_async(state,
                    node_name, nexts, CheckpointPhase::After, step,
                    last_checkpoint_id, barrier_state,
                    detail::checkpoint_metadata_for(ctx));

                RunResult result;
                result.usage = ctx.usage->snapshot();   // #88
                result.output = state.serialize_runtime();
                result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
                result.provider_outcomes = ctx.provider_outcomes->snapshot();
                result.interrupted     = true;
                result.interrupt_node  = node_name;
                json iv;
                iv["message"] = "Interrupt after node: " + node_name;
                result.interrupt_value = iv;
                result.checkpoint_id   = cp_id;
                result.execution_trace = std::move(trace);

                if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                    json data;
                    data["phase"]         = "after";
                    data["checkpoint_id"] = cp_id;
                    cb(GraphEvent{GraphEvent::Type::INTERRUPT, node_name,
                                  std::move(data)});
                }
                co_return result;
            }
        }

        // Build the unified routing list: original ready-set first,
        // then Send-spawned tasks in fan-in order. plan_impl unions
        // everyone's edges and applies barrier gating. Per-task
        // command_goto preempts via plan_impl's Pass 1, matching the
        // documented "any command_goto wins" semantic.
        std::vector<StepRouting> unified_routings;
        unified_routings.reserve(step_results.size() + send_routings.size());
        for (size_t i = 0; i < step_results.size(); ++i) {
            StepRouting r;
            r.node_name = ready[i];
            if (step_results[i].command
                && !step_results[i].command->goto_node.empty()) {
                r.command_goto = step_results[i].command->goto_node;
            }
            unified_routings.push_back(std::move(r));
        }
        for (auto& sr : send_routings) {
            unified_routings.push_back(std::move(sr));
        }

        auto plan = scheduler_->plan_next_step(
            unified_routings, state, barrier_state);
        hit_end = hit_end || plan.hit_end;
        ready  = std::move(plan.ready);

        if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
            if (plan.winning_command_goto) {
                json data;
                data["command_goto"] = *plan.winning_command_goto;
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                              std::move(data)});
            }
            if (!ready.empty()) {
                json next_nodes_arr = json::array();
                for (const auto& n : ready) next_nodes_arr.push_back(n);
                json data;
                data["next_nodes"] = next_nodes_arr;
                data["step"]       = step;
                cb(GraphEvent{GraphEvent::Type::NODE_START, "__routing__",
                              std::move(data)});
            }
        }

        if (!coord.enabled()) continue;

        std::vector<std::string> next_nodes_for_cp =
            ready.empty() ? std::vector<std::string>{std::string(END_NODE)}
                          : ready;
        const std::string parent_cp_id = last_checkpoint_id;
        // Guard against an executor path that returns without pushing to
        // trace. Every committed super-step still receives a stable node key.
        const std::string trace_tag = trace.empty()
            ? (std::string("__step__") + std::to_string(step))
            : trace.back();
        auto checkpoint = co_await coord.commit_super_step_async(
            state, trace_tag, next_nodes_for_cp, step, parent_cp_id,
            barrier_state, detail::checkpoint_metadata_for(ctx));
        last_checkpoint_id = checkpoint.id;
        replay_results.clear();

        if (safe_point_request &&
            safe_point_request->capture(*safe_point_generation,
                                        std::move(checkpoint),
                                        ctx.cancel_token.get())) {
            RunResult result;
            result.usage = ctx.usage->snapshot();
            result.output = state.serialize_runtime();
            result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
            result.provider_outcomes = ctx.provider_outcomes->snapshot();
            result.output["_neograph"] = json{{"safe_point", true}};
            result.checkpoint_id = last_checkpoint_id;
            result.execution_trace = std::move(trace);
            co_return result;
        }
    }

    RunResult result;
    result.usage = ctx.usage->snapshot();   // #88
    result.output = state.serialize_runtime();
    result.native_messages = state.captured_provider_messages().value_or(std::vector<sp::Message>{});
    result.provider_outcomes = ctx.provider_outcomes->snapshot();
    result.execution_trace = std::move(trace);
    if (!ready.empty()) {
        result.output["_neograph"] = json{
            {"max_steps_exhausted", true}
        };
    }
    if (!last_checkpoint_id.empty()) {
        result.checkpoint_id = last_checkpoint_id;
    }

    if (!result.native_messages.empty() && result.native_messages.back().role == sp::Role::Assistant) {
        std::size_t bytes = 0;
        for (const auto& part : result.native_messages.back().parts)
            if (const auto* text = std::get_if<sp::Text>(&part)) bytes += text->value.size();
        std::string response;
        response.reserve(bytes);
        for (const auto& part : result.native_messages.back().parts)
            if (const auto* text = std::get_if<sp::Text>(&part)) response += text->value;
        result.output["final_response"] = std::move(response);
    }

    co_return result;
    };
    std::optional<RunResult> result;
    std::exception_ptr execution_error;
    try {
        result.emplace(co_await execute());
    } catch (...) {
        execution_error = std::current_exception();
    }
    std::exception_ptr release_error;
    if (owned_lease) {
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        try {
            co_await lease_store->release_managed_budget_lease_async(owned_lease);
        } catch (...) {
            release_error = std::current_exception();
        }
    }
    if (release_error) {
        auto outcome = exception_provider_outcome(execution_error);
        if (!outcome && owned_outcomes) {
            std::lock_guard lock(owned_outcomes->mutex);
            if (owned_outcomes->values.size() > source_outcome_count)
                outcome = owned_outcomes->values.back();
        }
        throw ManagedBudgetLeaseReleaseError(std::move(outcome), execution_error, release_error);
    }
    if (execution_error) {
        try { std::rethrow_exception(execution_error); }
        catch (const ProviderOutcomeError&) { throw; }
        catch (const ProviderFailure&) { throw; }
        catch (const NodeExecutionError&) { throw; }
        catch (...) {
            if (owned_lease && owned_outcomes) {
                std::lock_guard lock(owned_outcomes->mutex);
                if (owned_outcomes->values.size() > source_outcome_count)
                    throw ProviderOutcomeError("Graph failed after an owned provider outcome",
                        owned_outcomes->values.back(), execution_error);
            }
            throw;
        }
    }
    co_return std::move(*result);
}

void GraphEngine::set_node_cache_enabled(const std::string& node_name,
                                         bool enabled,
                                         CacheKeyPolicy policy) {
    node_cache_.set_policy(node_name, std::move(policy));
    node_cache_.set_enabled(node_name, enabled);
}

} // namespace neograph::graph
