#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>  // RunContext (forward-declared in executor.h)
#include <neograph/graph/executor.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/state.h>

#include "canonical_json.h"
#include "failure_routing.h"
#include "run_context_runtime.h"
#include "text_safety.h"
#include "tool_failure.h"
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/error.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace neograph::graph {

namespace {

std::string safe_exception_message(const std::exception& error) {
    const std::string_view bytes(error.what());
    try {
        neograph::detail::validate_utf8(bytes);
        return std::string(bytes);
    } catch (const std::invalid_argument&) {
        // std::system_error::what() can use the Windows active code page.
        // Graph events are also Program journal input, which requires UTF-8.
        if (const auto* system = dynamic_cast<const std::system_error*>(&error))
            return "system error code " + std::to_string(system->code().value()) +
                   " (non-UTF-8 platform message)";
        return "exception message contained non-UTF-8 bytes (length " +
               std::to_string(bytes.size()) + ")";
    }
}

bool has_provider_outcome(std::exception_ptr error) noexcept {
    while (error) {
        try {
            std::rethrow_exception(error);
        } catch (const ProviderOutcomeError&) {
            return true;
        } catch (const ProviderFailure&) {
            return true;
        } catch (const NodeExecutionError& node_error) {
            error = node_error.cause();
        } catch (...) {
            return false;
        }
    }
    return false;
}

// ── FNV-1a 64-bit for Send task_id hashing ─────────────────────────────
// Deterministic, 0 deps, sufficient for resume's replay map key.
// Moved here from graph_engine.cpp along with the Send execution path.
inline uint64_t fnv1a_64(const std::string& s) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}
inline std::string fnv1a_hex(const std::string& s) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fnv1a_64(s)));
    return std::string(buf, 16);
}

// Stable task_id for a static node in a given super-step. Used by
// run_one_async and run_parallel_async; both must agree on the
// scheme so resume replay matches across restarts.
inline std::string make_static_task_id(int step, const std::string& node_name) {
    return "s" + std::to_string(step) + ":" + node_name;
}

// Stable task_id for a Send: includes index + target + deterministic
// hash of the input payload so the same logical Send rehydrates under
// the same key on resume.
inline std::string make_send_task_id(int                step,
                                     size_t             idx,
                                     const std::string& target,
                                     const json&        input) {
    return "s" + std::to_string(step) + ":send[" + std::to_string(idx) + "]:" + target + ":" +
           fnv1a_hex(input.dump());
}

// Every node invocation gets a RunContext of its own, bound to its task id, so
// nodes running side by side (fan-out branches, or overlapping calls into one
// NodeExecutor) never see each other's identity and keep value semantics for
// callable state such as `on_provider_event`.
//
// The scope is per attempt (#364): attempt k of task "s3:act" is bound to the
// effect namespace "s3:act#a<k>" (attempt 0 to "s3:act" itself), and records
// the mediated Tool effects that attempt performs.
class ScopedInvocationContext {
public:
    ScopedInvocationContext(const RunContext& parent, std::string_view invocation_id)
        : context_(parent), runtime_scope_(parent, context_, invocation_id, true) {}

    const RunContext& context() const { return context_; }
    const std::shared_ptr<detail::AttemptEffectLog>& effects() const {
        return runtime_scope_.effects();
    }

private:
    RunContext                       context_;
    detail::ScopedInvocationRuntime  runtime_scope_;
};

int next_delay_ms(int delay_ms, const RetryPolicy& policy) {
    // Compute in double and clamp before converting: casting a product
    // outside int range to int is undefined behaviour.
    double next = static_cast<double>(delay_ms) * static_cast<double>(policy.backoff_multiplier);
    if (std::isnan(next) || next > policy.max_delay_ms) next = policy.max_delay_ms;
    if (next < 0.0) next = 0.0;
    return static_cast<int>(next);
}


// `failure_routes`: the engine has failure edges, so a result may carry the
// #364 marker write, which is never applied to state.
void apply_node_result(GraphState& state, const NodeResult& result, const RunContext& context,
                       bool failure_routes) {
    std::vector<ChannelWrite> storage;
    const auto& writes = failure_routes ? detail::state_writes(result.writes, storage)
                                        : result.writes;
    state.apply_writes(writes);
    detail::append_applied_writes(context, writes);
    if (result.command) {
        state.apply_writes(result.command->updates);
        detail::append_applied_writes(context, result.command->updates);
    }
}

// Counts one activation of `node` for its max_visits (#364). False when the
// node is past its limit: it does not run and takes NG.VisitLimit instead.
// Every dispatch path counts its activations in a fixed order (ready order,
// then Send order) before dispatching any, replayed ones included, so a
// resumed step counts exactly what the interrupted one did.
bool take_activation(const detail::FailureRouting* routing, const RunContext& ctx,
                     const std::string& node) {
    if (!routing) return true;
    const auto* plan = routing->plan(node);
    if (!plan || !plan->max_visits) return true;
    const auto runtime = detail::runtime_for(ctx);
    if (!runtime || !runtime->failure_state) return true;
    return runtime->failure_state->activate(*routing, node);
}

}  // namespace

namespace detail {

struct InvocationSlot {
    /// The invocation's stable task id; attempt 0's effect namespace. Points
    /// into the caller's frame, which outlives the retry loop.
    std::string_view task_id;
    int step = 0;
    /// Pending writes of the super-step being resumed. A pending write keyed
    /// by an attempt's effect namespace records that the attempt started.
    const std::unordered_map<std::string, NodeResult>* replay = nullptr;
    const CheckpointCoordinator* coord = nullptr;
    const std::string* parent_cp_id = nullptr;
    /// Out: the attempt that was running when the node paused.
    int* interrupted_attempt = nullptr;
    /// #364: this activation is past the node's max_visits; it does not run.
    bool visit_limited = false;
};

}  // namespace detail

namespace {

// Records that attempt `attempt` of `slot` started, under `checkpoint_id`.
// Stores need no schema change: it is an empty pending write keyed by the
// attempt's effect namespace, which no node's task id equals.
asio::awaitable<void> record_attempt_start(const CheckpointCoordinator& coord,
                                           const std::string& checkpoint_id,
                                           std::string_view task_id,
                                           const std::string& node_name, int attempt,
                                           int step) {
    if (attempt <= 0 || !coord.records_pending_writes(checkpoint_id)) co_return;
    const auto effect_namespace = detail::attempt_effect_namespace(task_id, attempt);
    NodeResult started;
    co_await coord.record_pending_write_async(checkpoint_id, effect_namespace, effect_namespace,
                                              node_name, started, step);
}

}  // namespace

// =========================================================================
// NodeExecutor construction + helpers
// =========================================================================

NodeExecutor::NodeExecutor(const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
                           const std::vector<ChannelDef>&                           channel_defs,
                           RetryPolicyLookup  retry_policy_for,
                           asio::thread_pool* fan_out_pool,
                           NodeCache*         node_cache)
    : NodeExecutor(
          nodes, channel_defs, std::move(retry_policy_for), nullptr, fan_out_pool, node_cache) {}

NodeExecutor::NodeExecutor(const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
                           const std::vector<ChannelDef>&                           channel_defs,
                           RetryPolicyLookup                    retry_policy_for,
                           std::shared_ptr<const GraphRegistry> registry,
                           asio::thread_pool*                   fan_out_pool,
                           NodeCache*                           node_cache)
    : nodes_(nodes),
      channel_defs_(channel_defs),
      retry_policy_for_(std::move(retry_policy_for)),
      registry_(std::move(registry)),
      fan_out_pool_(fan_out_pool),
      node_cache_(node_cache) {}

void NodeExecutor::init_state(GraphState& state) const {
    for (const auto& cd : channel_defs_) {
        auto reducer = registry_ ? registry_->reducer(cd.reducer_name)
                                 : ReducerRegistry::instance().get(cd.reducer_name);
        const ChannelLifecyclePolicy lifecycle{cd.retention, cd.retention_limit, cd.persistence};
        if (cd.type == ReducerType::APPEND && cd.initial_value.is_null()) {
            state.init_channel(cd.name, cd.type, reducer, json::array(), lifecycle);
        } else {
            state.init_channel(cd.name, cd.type, reducer, cd.initial_value, lifecycle);
        }
    }
}

void NodeExecutor::maybe_warn_serial_fanout(std::size_t width) const {
    // Only relevant when the engine has no owned pool — that's the
    // build() default (EngineConfig::worker_count == 1) and the case where a
    // fan-out of width >= 2 silently runs sequentially. With a pool
    // installed, real parallelism is in effect and nothing to warn
    // about.
    if (fan_out_pool_ != nullptr || width < 2) return;
    // One-shot: first call that finds the flag clear wins the CAS,
    // every subsequent fan-out on this executor is silent. The flag
    // resets along with the executor on the next set_worker_count(N).
    bool expected = false;
    if (!warned_serial_fanout_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }
    // Opt-out for users who knowingly drive serial fan-out (the
    // worker=1 fast path) or for CI scenarios where stderr is
    // assertion-checked. Accepts "1" / "true" / "yes" as on, anything
    // else (or unset) as off.
    if (const char* env = std::getenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING")) {
        if (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 ||
            std::strcmp(env, "yes") == 0) {
            return;
        }
    }
    // Write directly to the stderr FILE* rather than std::cerr — on
    // Windows the MSVC CRT does not always keep std::cerr's underlying
    // buffer in sync with the OS-level fd 2 that test harnesses (and
    // pytest's `capfd`) redirect, so a `std::cerr << "..."` message
    // can be swallowed entirely in Windows wheel CI even though it
    // shows up on POSIX. `std::fputs` + `std::fflush(stderr)` hits
    // fd 2 directly and is portable across the three platforms we
    // ship wheels for.
    std::ostringstream oss;
    oss << "[neograph] warning: fan-out of width " << width
        << " is executing serially on a single thread because no "
        << "engine-owned worker pool is installed (build() default "
        << "is EngineConfig::worker_count == 1).\n"
        << "    Set EngineConfig::worker_count before build(), or call "
           "set_worker_count(N) / set_worker_count_auto() before the first run(), "
           "to enable real parallel fan-out.\n"
        << "    Suppress this warning with the environment variable "
        << "NEOGRAPH_SUPPRESS_FANOUT_WARNING=1.\n";
    std::fputs(oss.str().c_str(), stderr);
    std::fflush(stderr);
}

void NodeExecutor::apply_input(GraphState& state, const json& input) const {
    if (!input.is_object()) return;
    auto known = state.channel_names();
    for (const auto& [key, value] : input.items()) {
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            state.write(key, value);
        }
    }
}

// =========================================================================
// execute_node_with_retry_async: inner retry loop
// =========================================================================
//
// Drives node->run(NodeInput) via co_await so a node whose run body
// issues real non-blocking I/O lets the executor
// serve other coroutines while this one is waiting. Backoff uses an
// asio::steady_timer.async_wait so retry waits do not freeze workers.
// NodeInterrupt and exception semantics match the pre-3.0 sync
// retry loop bit-for-bit; the GCC-13-safe shape (no co_await inside a
// catch block) requires capturing the result/error into std::optional
// inside try, then deciding what to do outside.

asio::awaitable<NodeResult> NodeExecutor::execute_node_with_retry_async(
    const std::string&         node_name,
    GraphState&                state,
    const GraphStreamCallback& cb,
    StreamMode                 stream_mode,
    const RunContext&          ctx) {
    co_return co_await execute_attempts_async(node_name, state, cb, stream_mode, ctx, nullptr);
}

asio::awaitable<NodeResult> NodeExecutor::execute_attempts_async(
    const std::string&         node_name,
    GraphState&                state,
    const GraphStreamCallback& cb,
    StreamMode                 stream_mode,
    const RunContext&          ctx,
    detail::InvocationSlot*    slot) {
    // Dispatch through the unified ``run(NodeInput)`` virtual. ``ctx``
    // and the optional streaming callback reach the user-overridable
    // surface directly.

    auto node_it = nodes_.find(node_name);
    if (node_it == nodes_.end()) {
        throw std::runtime_error("Node not found: " + node_name);
    }

    // #364: the node's failure policy (non_retryable codes, failure edge,
    // error_text) and the run's retry ceiling.
    const detail::NodeFailurePlan* failure_plan =
        failure_routing_ && !failure_routing_->nodes.empty() ? failure_routing_->plan(node_name)
                                                             : nullptr;
    const int record_step = slot ? slot->step : ctx.step;

    // Give up on the node. A routable failure of a node with a failure edge
    // is handled: the invocation yields the failure record (in its
    // error_channel and in the marker write) and routes to the edge's target.
    // Anything else is reported and thrown as the NodeExecutionError that
    // carries the classified record (graph::node_failure_of).
    auto give_up = [&](std::exception_ptr cause, int attempts,
                       const ::neograph::detail::NodeFailureVerdict& verdict,
                       NodeFailureKind kind,
                       const std::shared_ptr<detail::AttemptEffectLog>& effects) -> NodeResult {
        auto record            = std::make_shared<NodeFailure>();
        record->node           = node_name;
        record->code           = verdict.code;
        record->public_message = verdict.public_message;
        record->failure_class  = verdict.failure_class;
        record->kind           = kind;
        record->retryable      = verdict.retryable;
        record->routable       = verdict.routable;
        record->attempts       = attempts;
        if (effects) {
            for (auto& entry : effects->entries())
                record->effects.push_back(NodeFailureEffect{
                    std::move(entry.tool), entry.ordinal, std::string(to_string(entry.status))});
        }

        std::string what = "unknown";
        try {
            std::rethrow_exception(cause);
        } catch (const std::exception& e) {
            what = safe_exception_message(e);
        } catch (...) {}

        if (record->routable && failure_plan && !failure_plan->target.empty()) {
            // Free text only on opt-in, only from typed safe sources, and only
            // what the host's redactor keeps.
            std::string text;
            if (failure_plan->error_text && failure_routing_->redactor &&
                !record->public_message.empty()) {
                try {
                    text = ::neograph::detail::sanitize_untrusted_text(
                        failure_routing_->redactor(*record), detail::kFailureTextMaxBytes);
                } catch (...) {
                    text.clear();
                }
            }
            const auto failure_record =
                detail::failure_record_json(*record, record_step, failure_plan->target, text);
            if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
                cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                              json{{"error", what},
                                   {"attempts", attempts},
                                   {"handled", true},
                                   {"routed_to", failure_plan->target}}});
            }
            // Failed attempts write nothing; the record is the only write.
            NodeResult handled;
            if (failure_plan->error_channel)
                handled.writes.push_back(
                    ChannelWrite{*failure_plan->error_channel, json::array({failure_record})});
            handled.writes.push_back(
                ChannelWrite{std::string(detail::kFailureRouteChannel), failure_record});
            return handled;
        }

        std::shared_ptr<const NodeFailure> failure = std::move(record);

        // A subgraph may fail inside this node. The outer retry policy has now
        // been honored; keep the innermost dispatch context (node name,
        // attempts, cause) as before, now with this node's record.
        std::string error_node = node_name;
        int         error_attempts = attempts;
        std::exception_ptr error_cause = cause;
        try {
            std::rethrow_exception(cause);
        } catch (const NodeExecutionError& inner) {
            // An inner error that nests a secondary failure keeps its exact
            // shape; it is rethrown as it was.
            if (const auto* nested = dynamic_cast<const std::nested_exception*>(&inner);
                nested && nested->nested_ptr())
                error_cause = nullptr;
            else {
                error_node     = inner.node_name();
                error_attempts = inner.attempts();
                error_cause    = inner.cause();
            }
        } catch (...) {}

        if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
            try {
                cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                              json{{"error", what}, {"attempts", attempts}, {"handled", false}}});
            } catch (...) {
                if (!has_provider_outcome(cause)) throw;
                // Keep provider evidence as the primary node cause; a failing
                // terminal observer remains inspectable as a nested exception.
                std::throw_with_nested(::neograph::detail::ClassifiedNodeExecutionError(
                    node_name, attempts, std::move(cause), std::move(failure)));
            }
        }
        if (!error_cause) std::rethrow_exception(cause);
        throw ::neograph::detail::ClassifiedNodeExecutionError(
            std::move(error_node), error_attempts, std::move(error_cause), std::move(failure));
    };

    // #364: an activation past the node's max_visits does not run. It is a
    // routable failure with code NG.VisitLimit and no attempts.
    if (slot && slot->visit_limited) {
        const auto limit = failure_plan && failure_plan->max_visits ? *failure_plan->max_visits : 0;
        auto cause = std::make_exception_ptr(std::runtime_error(
            "Node '" + node_name + "' reached its visit limit (max_visits " +
            std::to_string(limit) + ")"));
        ::neograph::detail::NodeFailureVerdict verdict;
        verdict.failure_class = NodeFailureClass::Ordinary;
        verdict.code          = kVisitLimitCode;
        verdict.retryable     = false;
        verdict.routable      = true;
        co_return give_up(std::move(cause), 0, verdict, NodeFailureKind::VisitLimit, nullptr);
    }

    // Node-level cache: opt-in per node, only consulted when caller is
    // not streaming (cached hits cannot replay LLM_TOKEN events). Hash
    // is computed once per call so retries reuse the same lookup key.
    const bool  cache_eligible = node_cache_ && !cb && node_cache_->is_enabled(node_name);
    std::string cache_state_hash;
    std::string cache_scope_key;
    if (cache_eligible) {
        cache_state_hash = hash_state_for_cache(state.serialize_cache());
        const auto policy = node_cache_->policy_for(node_name);
        if (policy.scope == CacheScope::Execution) {
            cache_scope_key = "execution:" + std::to_string(ctx.cache_execution_id);
        } else if (policy.context_key) {
            cache_scope_key = "reusable:" + policy.context_key(ctx);
        } else {
            cache_scope_key = "reusable";
        }
        if (auto cached = node_cache_->lookup(node_name, cache_state_hash, cache_scope_key);
            cached) {
            co_return std::move(*cached);
        }
    }

    auto policy = retry_policy_for_(node_name);

    // A resumed invocation continues the attempt that was in flight: the
    // latest attempt whose start was recorded. Its effects replay from the
    // broker; only the attempts after it are new.
    int first_attempt = 0;
    if (slot && slot->replay && !slot->replay->empty()) {
        while (first_attempt < std::numeric_limits<int>::max() &&
               slot->replay->count(detail::attempt_effect_namespace(slot->task_id, first_attempt + 1)))
            ++first_attempt;
    }
    const int last_attempt = std::max(policy.max_retries, first_attempt);
    int       delay_ms     = policy.initial_delay_ms;
    for (int waited = 0; waited < first_attempt; ++waited) delay_ms = next_delay_ms(delay_ms, policy);

    auto ex = co_await asio::this_coro::executor;

    for (int attempt = first_attempt; attempt <= last_attempt; ++attempt) {
        // Every attempt has its own effect namespace (#364): attempt 0 the
        // task id itself, attempt k "task#a<k>". A new attempt first records
        // that it started, so a crash-resume continues it instead of starting
        // over in an earlier attempt's slots.
        std::string      later_attempt_namespace;
        std::string_view effect_namespace;
        if (slot) {
            effect_namespace = slot->task_id;
            if (attempt > 0) {
                later_attempt_namespace = detail::attempt_effect_namespace(slot->task_id, attempt);
                effect_namespace        = later_attempt_namespace;
                // A node literally named "<node>#a<k>" would share this
                // namespace in the same super-step; refuse instead.
                const auto colon = later_attempt_namespace.find(':');
                if (colon != std::string::npos &&
                    nodes_.count(later_attempt_namespace.substr(colon + 1))) {
                    auto collision = std::make_exception_ptr(std::logic_error(
                        "Node '" + later_attempt_namespace.substr(colon + 1) +
                        "' is named like retry attempt " + std::to_string(attempt) + " of node '" +
                        node_name + "'; rename it"));
                    co_return give_up(collision, attempt,
                                      ::neograph::detail::classify_node_failure(collision, nullptr),
                                      NodeFailureKind::NonRetryable, nullptr);
                }
            }
            if (attempt > first_attempt && slot->coord && slot->parent_cp_id)
                co_await record_attempt_start(*slot->coord, *slot->parent_cp_id, slot->task_id,
                                              node_name, attempt, slot->step);
        }

        if (cb && has_mode(stream_mode, StreamMode::EVENTS)) {
            json data;
            data["step"] = ctx.step;
            if (attempt > 0) data["retry_attempt"] = attempt;
            cb(GraphEvent{GraphEvent::Type::NODE_START, node_name, std::move(data)});
        }

        // Capture the outcome of one attempt without co_await inside a
        // catch block (GCC 13 ICEs on that shape).
        std::optional<NodeResult>                 ok_result;
        std::exception_ptr                        failure;
        std::shared_ptr<detail::AttemptEffectLog> effects;
        {
            std::optional<ScopedInvocationContext> scope;
            if (slot) scope.emplace(ctx, effect_namespace);
            if (scope) effects = scope->effects();
            try {
                // Single dispatch point. NodeInput bundles state, per-run
                // context, and the optional streaming callback.
                NodeInput in{state, scope ? scope->context() : ctx, cb ? &cb : nullptr};
                ok_result.emplace(co_await node_it->second->run(in));
            } catch (...) {
                failure = std::current_exception();
            }
        }

        if (ok_result) {
            auto& nr = *ok_result;
            if (cb) {
                if (has_mode(stream_mode, StreamMode::UPDATES)) {
                    for (const auto& w : nr.writes) {
                        cb(GraphEvent{GraphEvent::Type::CHANNEL_WRITE, node_name,
                                      json{{"channel", w.channel}, {"value", w.value}}});
                    }
                }
                if (has_mode(stream_mode, StreamMode::EVENTS)) {
                    json end_data;
                    if (nr.command) end_data["command_goto"] = nr.command->goto_node;
                    if (!nr.sends.empty()) end_data["sends"] = (int)nr.sends.size();
                    cb(GraphEvent{GraphEvent::Type::NODE_END, node_name, std::move(end_data)});
                }
            }
            if (cache_eligible) {
                node_cache_->store(node_name, cache_state_hash, cache_scope_key, nr);
            }
            co_return std::move(nr);
        }

        // Control flow leaves unwrapped, as before: an interrupt pauses the
        // run, a cancellation stops it, and OOM is not a node failure.
        try {
            std::rethrow_exception(failure);
        } catch (const NodeInterrupt&) {
            // Resume continues this attempt.
            if (slot && slot->interrupted_attempt) *slot->interrupted_attempt = attempt;
            throw;
        } catch (const CancelledException&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const asio::system_error& error) {
            // Socket/timer cancellation enters node code as
            // operation_aborted. Once this operation's token is set, that is
            // cancellation control flow, not a retryable transport failure.
            if (ctx.cancel_token && ctx.cancel_token->is_cancelled() &&
                error.code() == asio::error::operation_aborted) {
                throw CancelledException("node " + node_name + " operation aborted");
            }
        } catch (...) {}

        // An effect of this attempt whose outcome is unknown is never retried
        // or routed: the run pauses for reconciliation, as mediated dispatch
        // does when it sees one, even if the node turned that into an error.
        if (effects && effects->uncertain()) {
            if (slot && slot->interrupted_attempt) *slot->interrupted_attempt = attempt;
            throw NodeInterrupt("Mediated Tool effect requires reconciliation",
                                json{{"kind", "tool_effect_reconciliation"},
                                     {"thread_id", ctx.thread_id},
                                     {"task_id", std::string(effect_namespace)}});
        }

        // Every other decision is the shared classifier's (tool_failure.h):
        // ordinary failures are retried unless the author said otherwise;
        // nothing else is. An attempt that committed a non-idempotent effect
        // is not retried either, or the next attempt would repeat the effect.
        auto verdict = ::neograph::detail::classify_node_failure(failure, ctx.cancel_token.get());
        if (effects && effects->committed() &&
            verdict.failure_class < NodeFailureClass::EffectCommitted) {
            verdict.failure_class = NodeFailureClass::EffectCommitted;
            verdict.code          = node_failure_code(NodeFailureClass::EffectCommitted);
            verdict.public_message.clear();
            verdict.retryable = false;
            verdict.routable  = node_failure_routable(NodeFailureClass::EffectCommitted);
        }
        // #364: a code the node's policy lists as non_retryable is not retried.
        if (verdict.retryable && failure_plan && failure_plan->non_retryable.count(verdict.code))
            verdict.retryable = false;
        if (!verdict.retryable || attempt >= last_attempt) {
            const auto kind = verdict.failure_class == NodeFailureClass::EffectCommitted
                                  ? NodeFailureKind::EffectCommitted
                              : verdict.retryable ? NodeFailureKind::Exhausted
                                                  : NodeFailureKind::NonRetryable;
            co_return give_up(std::move(failure), attempt + 1, verdict, kind, effects);
        }
        // #364: the run-level retry ceiling. Reaching it stops retrying.
        if (const auto run_runtime = detail::runtime_for(ctx);
            run_runtime && run_runtime->node_retry_ceiling &&
            !run_runtime->node_retry_ceiling->try_take())
            co_return give_up(std::move(failure), attempt + 1, verdict, NodeFailureKind::Ceiling,
                              effects);

        if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
            std::string what;
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& e) {
                what = safe_exception_message(e);
            } catch (...) {
                what = "unknown";
            }
            cb(GraphEvent{GraphEvent::Type::ERROR, node_name,
                          json{{"retry", attempt + 1},
                               {"max_retries", policy.max_retries},
                               {"delay_ms", delay_ms},
                               {"error", what}}});
        }

        // Apply jitter to break thundering-herd lockstep when N
        // parallel branches retry against the same upstream. The
        // policy default is jitter_pct=0 (deterministic, back-compat);
        // callers configuring fan-out workloads should set ~0.2.
        int actual_delay = delay_ms;
        if (policy.jitter_pct > 0.0f && delay_ms > 0) {
            // Per-thread RNG so concurrent branches don't share state.
            thread_local std::mt19937             rng{std::random_device{}()};
            float                                 pct = std::min(1.0f, policy.jitter_pct);
            std::uniform_real_distribution<float> dist(-pct, pct);
            const double scaled = static_cast<double>(delay_ms) * (1.0 + dist(rng));
            actual_delay = static_cast<int>(std::clamp(
                scaled, 0.0, static_cast<double>(std::numeric_limits<int>::max())));
        }

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(actual_delay));
        co_await timer.async_wait(asio::use_awaitable);

        delay_ms = next_delay_ms(delay_ms, policy);
    }

    throw std::runtime_error("Unreachable: async retry loop exited without return or throw");
}

// =========================================================================
// run_one_async: single-node path
// =========================================================================

asio::awaitable<NodeResult> NodeExecutor::run_one_async(
    const std::string&                                 node_name,
    int                                                step,
    GraphState&                                        state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator&                             coord,
    const std::string&                                 parent_cp_id,
    const BarrierState&                                barrier_state,
    std::vector<std::string>&                          trace,
    const GraphStreamCallback&                         cb,
    StreamMode                                         stream_mode,
    const RunContext&                                  ctx) {
    const std::string task_id = make_static_task_id(step, node_name);

    // GCC-13-safe outcome capture: collect result or NodeInterrupt
    // marker inside try, decide outside. Other exceptions propagate
    // out of the coroutine untouched.
    std::optional<NodeResult>    ok_result;
    std::optional<NodeInterrupt> interrupt;
    int                          interrupted_attempt = 0;
    detail::InvocationSlot slot{task_id, step, &replay, &coord, &parent_cp_id, &interrupted_attempt};
    slot.visit_limited = !take_activation(failure_routing_, ctx, node_name);

    try {
        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            // Replayed from a previous partial run — do NOT re-execute,
            // do NOT re-record. Just apply the recorded writes.
            ok_result.emplace(replay_it->second);
        } else {
            ok_result.emplace(co_await execute_attempts_async(
                node_name, state, cb, stream_mode, ctx, &slot));

            // Record BEFORE apply_writes so a crash between the two
            // still leaves a durable log for resume to replay.
            if (coord.records_pending_writes(parent_cp_id))
                co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, node_name,
                                                          *ok_result, step);
        }
    } catch (const NodeInterrupt& ni) {
        // Copy the whole interrupt, not just the fact that one happened.
        // This used to record a bool and rethrow `NodeInterrupt(node_name)`,
        // which silently replaced the node's reason (and, later, its payload)
        // with the node's own name — so a caller asking "why did you stop?"
        // got back "risky" (issue #94).
        interrupt = ni;
    }

    if (interrupt) {
        // NodeInterrupt pauses this specific node — resume must
        // re-enter exactly here. Save a phase=NodeInterrupt cp with
        // next_nodes={node_name} so load_for_resume restarts the super
        // step at this step with only this node ready.
        // GCC-13 ICE workaround: build the next_nodes vector outside
        // the co_await arg list (nested brace-init in a coroutine
        // body trips build_special_member_call).
        std::vector<std::string> next_nodes;
        next_nodes.push_back(node_name);
        const auto interrupt_cp_id = co_await coord.save_super_step_async(
            state, node_name, next_nodes, CheckpointPhase::NodeInterrupt, step, parent_cp_id,
            barrier_state,
            detail::checkpoint_metadata_for(ctx, detail::CheckpointTiming::DuringStep));
        // Without a parent checkpoint the attempt's start was not recorded;
        // record it under the interrupt's own checkpoint so resume continues it.
        if (parent_cp_id.empty())
            co_await record_attempt_start(coord, interrupt_cp_id, task_id, node_name,
                                          interrupted_attempt, step);
        // The executor is the only layer that knows the graph's name for
        // this node — the node body does not.
        interrupt->set_node(node_name);
        throw *interrupt;
    }

    auto& nr = *ok_result;
    apply_node_result(state, nr, ctx, failure_routing_ && failure_routing_->routes());

    trace.push_back(node_name);
    co_return std::move(nr);
}

// =========================================================================
// run_parallel_async: fan-out via make_parallel_group
// =========================================================================
//
// Uses asio::experimental::make_parallel_group on a vector of
// co_spawn-deferred workers. wait_for_all gives back a
// (completion_order, exception_ptrs, results) triple — we ignore
// order (apply writes in ready-order so scheduler pairs ready[i] ↔
// results[i]) and fold exceptions down to first_exception.
//
// GCC-13-safe: no co_await inside a catch; no nested brace-init in
// the coroutine body. The first-exception classifier uses a separate
// try/catch to tag NodeInterrupt; the cp save happens outside.

asio::awaitable<std::vector<NodeResult>> NodeExecutor::run_parallel_async(
    const std::vector<std::string>&                    ready,
    int                                                step,
    GraphState&                                        state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator&                             coord,
    const std::string&                                 parent_cp_id,
    const BarrierState&                                barrier_state,
    std::vector<std::string>&                          trace,
    const GraphStreamCallback&                         cb,
    StreamMode                                         stream_mode,
    const RunContext&                                  ctx) {
    // Single-ready bypass: skip the parallel_group machinery entirely.
    // When the super-step has only one ready node, building deferred
    // ops, allocating excs/values/order vectors, and wrapping in
    // wait_for_all is pure overhead. Run the worker inline on the
    // outer executor instead. Hit by every linear-chain step (seq
    // bench's 3-step path, par bench's summarizer fan-in step) and
    // by any super-step where conditional routing collapses to one
    // target.
    const bool failure_routes = failure_routing_ && failure_routing_->routes();
    if (ready.size() == 1) {
        const auto&                  node_name = ready[0];
        const std::string            task_id   = make_static_task_id(step, node_name);
        auto                         replay_it = replay.find(task_id);
        NodeResult                   nr;
        std::optional<NodeInterrupt> interrupt;
        const bool visit_limited = !take_activation(failure_routing_, ctx, node_name);
        if (replay_it != replay.end()) {
            nr = replay_it->second;
        } else {
            int                    interrupted_attempt = 0;
            detail::InvocationSlot slot{task_id,       step, &replay, &coord, &parent_cp_id,
                                        &interrupted_attempt};
            slot.visit_limited = visit_limited;
            // GCC-13: cannot co_await inside a catch handler. Classify
            // here, save + rethrow outside the try/catch.
            try {
                nr = co_await execute_attempts_async(node_name, state, cb, stream_mode, ctx, &slot);
            } catch (const NodeInterrupt& ni) {
                interrupt = ni;
            }
            if (interrupt) {
                std::vector<std::string> next_nodes;
                next_nodes.push_back(node_name);
                const auto interrupt_cp_id = co_await coord.save_super_step_async(
                    state, node_name, next_nodes, CheckpointPhase::NodeInterrupt, step,
                    parent_cp_id, barrier_state,
                    detail::checkpoint_metadata_for(ctx, detail::CheckpointTiming::DuringStep));
                if (parent_cp_id.empty())
                    co_await record_attempt_start(coord, interrupt_cp_id, task_id, node_name,
                                                  interrupted_attempt, step);
                interrupt->set_node(node_name);
                throw *interrupt;
            }
            if (coord.records_pending_writes(parent_cp_id))
                co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, node_name, nr,
                                                          step);
        }
        apply_node_result(state, nr, ctx, failure_routes);
        trace.push_back(node_name);
        std::vector<NodeResult> result;
        result.push_back(std::move(nr));
        co_return result;
    }

    maybe_warn_serial_fanout(ready.size());
    auto outer_ex = co_await asio::this_coro::executor;
    // Branches dispatch to the engine's owned pool when available so
    // CPU-bound fan-out actually parallelizes even if the outer
    // coroutine is driven on a single-threaded executor (e.g. sync
    // run() routing through run_sync). With no pool, branches share
    // the outer executor — matches the single-threaded contract the
    // async peer tests assert.
    asio::any_io_executor branch_ex = fan_out_pool_
                                          ? asio::any_io_executor(fan_out_pool_->get_executor())
                                          : asio::any_io_executor(outer_ex);

    // Per-branch worker. Captures by ref — all captured refs outlive
    // co_await below (stack scope), and state reads are
    // shared_mutex-guarded inside GraphState. No worker writes to
    // shared state; the NodeResults are aggregated out-of-band by the
    // parallel_group and applied in-order after wait_for_all returns.
    // INVARIANT (capture-by-reference safety): the outer awaitable
    // suspends at make_parallel_group::async_wait below and stays
    // suspended until every worker finishes. The captured locals
    // (state, replay, coord, parent_cp_id, trace, cb, stream_mode)
    // therefore live for the full lifetime of every worker frame.
    // No worker mutates the captured state — NodeResult outputs are
    // aggregated out-of-band and applied in-order after wait_for_all
    // returns. If a future change introduces opportunistic
    // cancellation that abandons the parallel_group early, switch
    // these captures to by-value copies first.
    // The attempt each branch was in when it paused; written by that branch
    // only, read after the group has finished.
    std::vector<int> interrupted_attempts(ready.size(), 0);
    // Activations are counted in ready order before any branch starts.
    std::vector<char> visit_limited(ready.size(), 0);
    for (std::size_t index = 0; index < ready.size(); ++index)
        visit_limited[index] = !take_activation(failure_routing_, ctx, ready[index]);
    auto worker = [&, this](std::size_t index) -> asio::awaitable<NodeResult> {
        const std::string& node_name = ready[index];
        const std::string task_id   = make_static_task_id(step, node_name);
        auto              replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            co_return replay_it->second;
        }
        detail::InvocationSlot slot{task_id,       step, &replay, &coord, &parent_cp_id,
                                    &interrupted_attempts[index]};
        slot.visit_limited = visit_limited[index] != 0;
        auto nr = co_await execute_attempts_async(node_name, state, cb, stream_mode, ctx, &slot);
        if (coord.records_pending_writes(parent_cp_id))
            co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, node_name, nr,
                                                       step);
        co_return nr;
    };

    // Build deferred ops, one per ready node. co_spawn-with-deferred
    // returns an op that, when awaited, runs the worker coroutine and
    // completes with (exception_ptr, NodeResult).
    // Each branch gets its own strand. Distinct strands retain branch-level
    // parallelism while keeping a coroutine's Asio cancellation state on one
    // serial executor; a bare multi-threaded pool lets cancellation emission
    // race the frame's next await_transform.
    //
    // Without a fan-out pool the branches share the outer executor. When that
    // executor already is a strand (every cancellable run is driven through
    // CancelToken::bind_executor), every branch handler is serialised on it,
    // so a strand per branch adds a queue hop and a strand_impl allocation but
    // no ordering the outer strand does not already give.
    const bool branch_ex_serialised =
        fan_out_pool_ == nullptr &&
        (branch_ex.target<asio::strand<asio::any_io_executor>>() != nullptr ||
         &asio::query(branch_ex, asio::execution::context) ==
             neograph::async::detail::serial_io_context());
    auto make_branch_executor = [&]() -> asio::any_io_executor {
        return branch_ex_serialised ? branch_ex
                                    : asio::any_io_executor(asio::make_strand(branch_ex));
    };
    using DeferredOp = decltype(asio::co_spawn(
        std::declval<asio::any_io_executor>(), worker(std::size_t{0}),
        asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(ready.size());
    for (std::size_t index = 0; index < ready.size(); ++index) {
        ops.push_back(asio::co_spawn(make_branch_executor(), worker(index), asio::deferred));
    }

    // wait_for_all returns:
    //   completion_order : std::vector<std::size_t>
    //   excs             : std::vector<std::exception_ptr>  (per-branch)
    //   values           : std::vector<NodeResult>          (per-branch)
    auto [order, excs, values] =
        co_await asio::experimental::make_parallel_group(std::move(ops))
            .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;  // we apply in ready-order regardless of completion-order

    // Find first exception + classify. NodeInterrupt gets a dedicated
    // cp-save with the full ready set. Successful siblings have pending
    // writes; resume replays those and retries only the interrupted node.
    //
    // TSan-safe NodeInterrupt path: extract the reason as a plain
    // ``std::string`` while we still hold the exception_ptr, then
    // throw a *fresh* NodeInterrupt on the main thread. The original
    // worker-allocated eptr (and its libstdc++-internal atomic
    // refcount) never crosses the ``co_await save_super_step_async``
    // suspend boundary — that's where TSan was flagging a benign
    // libstdc++ ``__exception_ptr::_M_release`` race against the
    // worker thread's eptr destructor. Non-interrupt
    // exceptions still rethrow the original eptr on the same thread
    // they were classified, no co_await between, so no race window.
    std::exception_ptr first_exception;
    std::string        first_exception_node;
    // A whole copy of the interrupt (reason + payload), not just its reason.
    // Copying it here is what keeps the TSan property described above: the
    // strings and json are deep-copied out while we still hold the eptr, so
    // what we throw later is wholly owned by this thread.
    std::optional<NodeInterrupt> interrupt;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        if (excs[i] && !first_exception) {
            first_exception      = excs[i];
            first_exception_node = ready[i];
            try {
                std::rethrow_exception(first_exception);
            } catch (const NodeInterrupt& ni) {
                interrupt = ni;
            } catch (...) {
                // non-interrupt; leave `interrupt` empty
            }
        }
    }

    if (first_exception) {
        if (interrupt) {
            // GCC-13 workaround: build the next_nodes vector outside
            // the co_await arg list (nested brace-init trips the ICE).
            std::vector<std::string> next_nodes;
            next_nodes = ready;
            const auto interrupt_cp_id = co_await coord.save_super_step_async(
                state, first_exception_node, next_nodes, CheckpointPhase::NodeInterrupt, step,
                parent_cp_id, barrier_state,
                detail::checkpoint_metadata_for(ctx, detail::CheckpointTiming::DuringStep));
            if (parent_cp_id.empty()) {
                for (std::size_t i = 0; i < ready.size(); ++i) {
                    const auto branch_task_id = make_static_task_id(step, ready[i]);
                    co_await record_attempt_start(coord, interrupt_cp_id, branch_task_id, ready[i],
                                                  interrupted_attempts[i], step);
                }
            }
            // Drop the worker eptr and throw a fresh NodeInterrupt —
            // see comment above the loop. The fresh exception is
            // wholly owned by the current (main) thread.
            first_exception = nullptr;
            interrupt->set_node(first_exception_node);
            throw *interrupt;
        }
        std::rethrow_exception(first_exception);
    }

    // Apply results in ready-order (scheduler pairs ready[i] ↔ results[i]).
    std::vector<NodeResult> step_results;
    step_results.reserve(ready.size());
    for (std::size_t i = 0; i < ready.size(); ++i) {
        apply_node_result(state, values[i], ctx, failure_routes);
        step_results.push_back(std::move(values[i]));
        trace.push_back(ready[i]);
    }
    co_return step_results;
}

// =========================================================================
// run_sends_async: single-send + multi-send dynamic fan-out
// =========================================================================
//
// Two-branch shape:
//   * single Send — sequential on the shared state, with retry.
//   * multi Send — make_parallel_group on deferred workers, each
//     with its own isolated GraphState copy. Results fan back to
//     the shared state after wait_for_all.
//
// GCC-13-safe: no co_await inside a catch. The multi-send worker
// captures exceptions into the parallel_group's excs array; the
// first exception (if any) is rethrown after the wait resolves.

asio::awaitable<std::vector<StepRouting>> NodeExecutor::run_sends_async(
    const std::vector<Send>&                           sends,
    int                                                step,
    GraphState&                                        state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator&                             coord,
    const std::string&                                 parent_cp_id,
    std::vector<std::string>&                          trace,
    const GraphStreamCallback&                         cb,
    StreamMode                                         stream_mode,
    const RunContext&                                  ctx) {
    co_return co_await run_sends_impl_async(sends, step, state, replay, coord, parent_cp_id, trace,
                                            cb, stream_mode, ctx, nullptr);
}

asio::awaitable<std::vector<StepRouting>> NodeExecutor::run_sends_impl_async(
    const std::vector<Send>&                           sends,
    int                                                step,
    GraphState&                                        state,
    const std::unordered_map<std::string, NodeResult>& replay,
    CheckpointCoordinator&                             coord,
    const std::string&                                 parent_cp_id,
    std::vector<std::string>&                          trace,
    const GraphStreamCallback&                         cb,
    StreamMode                                         stream_mode,
    const RunContext&                                  ctx,
    std::vector<json>*                                 failure_records) {
    std::vector<StepRouting> routings;
    if (failure_records) failure_records->assign(sends.size(), json());
    if (sends.empty()) co_return routings;
    const bool failure_routes = failure_routing_ && failure_routing_->routes();

    // Validate the whole batch before any Send can run or record a pending
    // write. This keeps single- and multi-Send failures identical and makes a
    // mixed batch all-or-nothing at the dispatch boundary.
    for (std::size_t si = 0; si < sends.size(); ++si) {
        if (nodes_.find(sends[si].target_node) == nodes_.end()) {
            throw std::runtime_error("Send from '" + sends[si].source_node +
                                     "' targets unknown node '" + sends[si].target_node +
                                     "' (Send invocation index " + std::to_string(si) + ")");
        }
    }

    if (cb && has_mode(stream_mode, StreamMode::DEBUG)) {
        json send_info = json::array();
        for (const auto& s : sends) {
            json item;
            item["target"] = s.target_node;
            item["input"]  = s.input;
            send_info.push_back(item);
        }
        json data;
        data["sends"] = send_info;
        cb(GraphEvent{GraphEvent::Type::NODE_START, "__send__", std::move(data)});
    }

    // Activations are counted in Send order before any Send runs (#364).
    std::vector<char> visit_limited(sends.size(), 0);
    for (std::size_t si = 0; si < sends.size(); ++si)
        visit_limited[si] = !take_activation(failure_routing_, ctx, sends[si].target_node);

    // --- Single Send: sequential on shared state, with retry. ---
    if (sends.size() == 1) {
        const auto& s = sends[0];

        const std::string task_id = make_send_task_id(step, 0, s.target_node, s.input);
        NodeResult        nr;

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            nr = replay_it->second;
        } else {
            apply_input(state, s.input);
            detail::InvocationSlot slot{task_id, step, &replay, &coord, &parent_cp_id, nullptr};
            slot.visit_limited = visit_limited[0] != 0;
            nr = co_await execute_attempts_async(s.target_node, state, cb, stream_mode, ctx, &slot);
            if (coord.records_pending_writes(parent_cp_id))
                co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, s.target_node,
                                                          nr, step);
        }
        apply_node_result(state, nr, ctx, failure_routes);
        trace.push_back(s.target_node + "[send]");
        if (failure_records && failure_routes)
            if (const auto* record = detail::failure_route_record(nr)) (*failure_records)[0] = *record;

        StepRouting r;
        r.node_name = s.target_node;
        if (nr.command && !nr.command->goto_node.empty()) {
            r.command_goto = nr.command->goto_node;
        }
        routings.push_back(std::move(r));
        co_return routings;
    }

    // --- Multi Send: isolated per-target state + parallel_group. ---
    //
    // Each worker coroutine builds its own GraphState from a
    // serialize/restore round-trip, applies the Send's input, then
    // drives execute_node_with_retry_async. Isolated state can't be
    // shared across concurrent Sends — each gets its own copy.
    maybe_warn_serial_fanout(sends.size());
    //
    // PERF: serialize the source state once and have every worker
    // restore from the same snapshot. Pre-fix every worker called
    // state.serialize() inside its lambda — N workers paid N full
    // serializations of the entire channel set, which dominated the
    // par micro-bench (callgrind: yyjson_mut_doc_new + val_pool_grow
    // + mut_val_mut_copy = ~9% of total instructions). The state is
    // logically immutable across the worker batch (state mutations
    // happen post-join, line ~568 below) so a single snapshot is
    // safe to share.
    auto state_snapshot      = state.serialize_runtime();
    auto outer_ex            = co_await asio::this_coro::executor;
    asio::any_io_executor ex = fan_out_pool_ ? asio::any_io_executor(fan_out_pool_->get_executor())
                                             : asio::any_io_executor(outer_ex);

    auto worker = [&, this](std::size_t si) -> asio::awaitable<NodeResult> {
        const auto& s = sends[si];

        const std::string task_id = make_send_task_id(step, si, s.target_node, s.input);

        auto replay_it = replay.find(task_id);
        if (replay_it != replay.end()) {
            co_return replay_it->second;
        }

        GraphState send_state;
        init_state(send_state);
        send_state.copy_provider_history_from(state);
        send_state.restore_runtime(state_snapshot);
        apply_input(send_state, s.input);
        // v1.0 (9d): the old `run_cancel_token` smuggling channel is
        // gone. Cancel flows through `ctx.cancel_token` instead, which
        // is captured by value into this worker lambda — no
        // serialize/restore hop, no manual forward.

        detail::InvocationSlot slot{task_id, step, &replay, &coord, &parent_cp_id, nullptr};
        slot.visit_limited = visit_limited[si] != 0;
        auto nr = co_await execute_attempts_async(s.target_node, send_state, cb, stream_mode, ctx,
                                                  &slot);
        if (coord.records_pending_writes(parent_cp_id))
            co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, s.target_node, nr,
                                                      step);
        co_return nr;
    };

    // With an engine fan-out pool a branch can resume on any pool thread, and the
    // parallel group forwards a cancellation by dispatching it on the branch's
    // executor. On the bare pool executor that emit can run on one thread while the
    // branch (installing and clearing the cancellation handlers of its awaited
    // operations) runs on another, and Asio's cancellation slots are not thread-safe.
    // A strand per branch keeps each coroutine's cancellation state on one serial
    // executor, as 654bcd0 did for static fan-out (#344). Without a pool the branches
    // share the outer executor, which is already serial, so they stay on it.
    auto branch_executor = [&]() -> asio::any_io_executor {
        return fan_out_pool_ ? asio::any_io_executor(asio::make_strand(ex)) : ex;
    };
    using DeferredOp = decltype(asio::co_spawn(std::declval<asio::any_io_executor>(),
                                               worker(std::size_t{0}), asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(sends.size());
    for (std::size_t si = 0; si < sends.size(); ++si) {
        ops.push_back(asio::co_spawn(branch_executor(), worker(si), asio::deferred));
    }

    auto [order, excs, values] =
        co_await asio::experimental::make_parallel_group(std::move(ops))
            .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;

    // First-exception pass — same shape as run_parallel_async, but
    // a Send's NodeInterrupt does not emit a dedicated cp: Send
    // targets are fan-out one-shots without an obvious resume
    // scope, so interrupts propagate for the caller to surface.
    for (std::size_t i = 0; i < sends.size(); ++i) {
        if (excs[i]) {
            std::rethrow_exception(excs[i]);
        }
    }

    // Fan back in send order. Each Send-spawned task contributes one
    // StepRouting so its `Command.goto` and / or default outgoing edges
    // flow into the next super-step's routing decision.
    routings.reserve(sends.size());
    for (std::size_t si = 0; si < sends.size(); ++si) {
        apply_node_result(state, values[si], ctx, failure_routes);
        trace.push_back(sends[si].target_node + "[send]");
        if (failure_records && failure_routes)
            if (const auto* record = detail::failure_route_record(values[si]))
                (*failure_records)[si] = *record;

        StepRouting r;
        r.node_name = sends[si].target_node;
        if (values[si].command && !values[si].command->goto_node.empty()) {
            r.command_goto = values[si].command->goto_node;
        }
        routings.push_back(std::move(r));
    }
    co_return routings;
}

}  // namespace neograph::graph
