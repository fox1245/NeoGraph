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
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/error.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace neograph::graph {

namespace detail {
struct InvocationSlot;
}  // namespace detail

namespace {

// #393: gives `context`, the RunContext of the invocation `slot`, the run's
// resume value when the resume answers that invocation, and none otherwise.
// Defined after InvocationSlot.
void bind_resume_value(const RunContext& parent, RunContext& context,
                       const detail::InvocationSlot& slot);

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
//
// The context carries the run's resume value only when the invocation is the
// one the resume answers (#393): see bind_resume_value() below.
class ScopedInvocationContext {
public:
    ScopedInvocationContext(const RunContext& parent, std::string_view invocation_id,
                            const detail::InvocationSlot* slot)
        : context_(parent), runtime_scope_(parent, context_, invocation_id, true) {
        if (slot) bind_resume_value(parent, context_, *slot);
    }

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

// #385: before the writes of a step whose nodes emitted Sends are applied,
// keep the state the step started from. A Send that interrupts saves it as
// its checkpoint, so that resume re-enters the whole step.
void capture_step_start(const RunContext& ctx, const GraphState& state, bool has_sends) {
    if (!has_sends) return;
    const auto runtime = detail::runtime_for(ctx);
    if (!runtime || !runtime->step_start || !runtime->step_start->enabled ||
        runtime->step_start->state)
        return;
    runtime->step_start->state = state.serialize_runtime();
}

// #388: one PendingInterrupt for a paused invocation.
PendingInterrupt pending_interrupt(std::string task_id, const std::string& node,
                                   const NodeInterrupt& interrupt) {
    return PendingInterrupt{std::move(task_id), node, detail::interrupt_value_of(interrupt)};
}

// #388: the step's NodeInterrupts, for the engine to report; and the metadata
// of its NodeInterrupt checkpoint, which lists them when there are several,
// or always when `listed` (#392: a pause beside a failure).
json record_step_interrupts(const RunContext& ctx, std::vector<PendingInterrupt> interrupts,
                            bool listed = false) {
    const auto runtime = detail::runtime_for(ctx);
    if (runtime && runtime->step_interrupts) {
        runtime->step_interrupts->list = interrupts;
        runtime->step_interrupts->checkpoint_id.clear();
    }
    return detail::with_pending_interrupts(
        detail::checkpoint_metadata_for(ctx, detail::CheckpointTiming::DuringStep), interrupts,
        listed);
}

// #392: how one invocation of a step ended, by precedence, lowest first.
enum class InvocationExit { Interrupt, Failure, Reconciliation, Stop };

InvocationExit exit_of(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const NodeInterrupt& interrupt) {
        // The interrupt the executor and mediated dispatch raise for an effect
        // whose outcome is unknown.
        const auto& value = interrupt.value();
        return value.is_object() &&
                       value.value("kind", std::string()) == "tool_effect_reconciliation"
                   ? InvocationExit::Reconciliation
                   : InvocationExit::Interrupt;
    } catch (const CancelledException&) {
        return InvocationExit::Stop;
    } catch (const std::bad_alloc&) {
        return InvocationExit::Stop;
    } catch (...) {
        return InvocationExit::Failure;
    }
}

// #392: how a step ends when its invocations (fan-out branches, or the Sends
// of a step) ended with `errors`, null for one that finished. The same rule
// holds whatever their index or completion order:
//   1. run control (cancellation, out of memory) stops the run; `end` is the
//      first one, as inside one invocation, where it outranks an uncertain
//      effect;
//   2. otherwise an uncertain effect pauses the step for reconciliation, even
//      beside a failure, which would end the run and abandon an effect whose
//      outcome nobody knows; beside a failure the pause reports only the
//      reconciliation interrupts, and the failed invocation, which recorded
//      no result, runs again on resume;
//   3. otherwise a failure fails the run; `end` is the first in dispatch
//      order, and the step's ordinary interrupts are not reported;
//   4. otherwise the step pauses on every interrupt (#388).
// A failure routed along a failure edge (#364) is not an error here: it is the
// invocation's result, like a completion.
struct StepOutcome {
    std::optional<std::size_t> end;
    /// The interrupts the pause reports, in dispatch order.
    std::vector<std::size_t> pause;
    /// The pause holds back a failure; its checkpoint lists what it answers.
    bool beside_failure = false;
};

StepOutcome step_outcome(const std::vector<std::exception_ptr>& errors) {
    std::optional<std::size_t> stop;
    std::optional<std::size_t> failure;
    std::vector<std::size_t>   reconciliations;
    std::vector<std::size_t>   interrupts;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (!errors[index]) continue;
        switch (exit_of(errors[index])) {
            case InvocationExit::Stop:
                if (!stop) stop = index;
                break;
            case InvocationExit::Failure:
                if (!failure) failure = index;
                break;
            case InvocationExit::Reconciliation:
                reconciliations.push_back(index);
                interrupts.push_back(index);
                break;
            case InvocationExit::Interrupt:
                interrupts.push_back(index);
                break;
        }
    }
    StepOutcome outcome;
    if (stop) {
        outcome.end = stop;
    } else if (failure && reconciliations.empty()) {
        outcome.end = failure;
    } else {
        outcome.beside_failure = failure.has_value();
        outcome.pause          = failure ? std::move(reconciliations) : std::move(interrupts);
    }
    return outcome;
}

// #386: the NodeInterrupt checkpoint saved for the step's interrupts, which the
// run reports.
void record_interrupt_checkpoint(const RunContext& ctx, const std::string& checkpoint_id) {
    const auto runtime = detail::runtime_for(ctx);
    if (runtime && runtime->step_interrupts) runtime->step_interrupts->checkpoint_id = checkpoint_id;
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

std::vector<std::string> paused_task_ids(int step, const std::vector<std::string>& ready,
                                         const std::unordered_map<std::string, NodeResult>& replay) {
    std::vector<std::string> ids;
    for (const auto& node : ready) {
        auto id = make_static_task_id(step, node);
        if (!replay.count(id)) ids.push_back(std::move(id));
    }
    if (!ids.empty()) return ids;
    // Every ready node replays: the step paused in its Sends, which follow
    // in ready order and then in the order each node emitted them.
    std::size_t index = 0;
    for (const auto& node : ready) {
        const auto found = replay.find(make_static_task_id(step, node));
        if (found == replay.end()) continue;
        for (const auto& send : found->second.sends) {
            auto id = make_send_task_id(step, index++, send.target_node, send.input);
            if (!replay.count(id)) ids.push_back(std::move(id));
        }
    }
    return ids;
}

std::string static_task_id(int step, const std::string& node) {
    return make_static_task_id(step, node);
}

}  // namespace detail

namespace {

// #388, #393: a resume answers one invocation, in the step it re-enters (and,
// #396, each gated node the step had answered keeps its own answer). Only an
// answered invocation, each of its attempts included, receives a value.
// Every other invocation runs without it: one of the same step that paused
// asks again with its own payload, and a node of a later step that asks
// pauses the run again instead of taking an answer to another question.
void bind_resume_value(const RunContext& parent, RunContext& context,
                       const detail::InvocationSlot& slot) {
    const auto runtime = detail::runtime_for(parent);
    // An executor driven without a run binding keeps what its caller set.
    if (!runtime) return;
    context.resume_value.reset();
    if (slot.step != runtime->answered_step) return;
    // #396: each gated node whose gate the step had answered receives its own
    // answer.
    if (const auto answer = runtime->answers.find(slot.task_id); answer != runtime->answers.end())
        context.resume_value = answer->second;
}

}  // namespace

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

// #385: the NodeInterrupt checkpoint of a Send that interrupts. Like a ready
// node that interrupts in a fan-out step, it re-enters the whole step: the
// state the step started from, the step's ready nodes as next nodes, the
// barrier state it started with. A Send runs after the step's ready nodes have
// finished, so without a checkpoint before the step their results, and those
// of the Sends that finished, are recorded under the interrupt checkpoint
// itself; resume replays them instead of running them again. `finished[i]` is
// the result of Send i when it finished. Returns the checkpoint's id.
asio::awaitable<std::string> save_send_interrupt(const CheckpointCoordinator&          coord,
                                                 const GraphState&                     step_state,
                                                 const detail::SendStep&               send_step,
                                                 const std::vector<Send>&              sends,
                                                 const std::vector<const NodeResult*>& finished,
                                                 const std::vector<int>& interrupted_attempts,
                                                 std::size_t interrupted, int step,
                                                 const std::string& parent_cp_id,
                                                 const json&        metadata) {
    // GCC-13: build the vector outside the co_await argument list.
    std::vector<std::string> next_nodes = send_step.ready;
    const auto interrupt_cp_id = co_await coord.save_super_step_async(
        step_state, sends[interrupted].target_node, next_nodes, CheckpointPhase::NodeInterrupt,
        step, parent_cp_id, send_step.barrier_state, metadata);
    if (!parent_cp_id.empty()) co_return interrupt_cp_id;
    for (std::size_t i = 0; i < send_step.ready.size() && i < send_step.results.size(); ++i) {
        const auto task_id = make_static_task_id(step, send_step.ready[i]);
        co_await coord.record_pending_write_async(interrupt_cp_id, task_id, task_id,
                                                  send_step.ready[i], send_step.results[i], step);
    }
    for (std::size_t si = 0; si < sends.size(); ++si) {
        const auto task_id = make_send_task_id(step, si, sends[si].target_node, sends[si].input);
        if (finished[si])
            co_await coord.record_pending_write_async(interrupt_cp_id, task_id, task_id,
                                                      sends[si].target_node, *finished[si], step);
        else
            co_await record_attempt_start(coord, interrupt_cp_id, task_id, sends[si].target_node,
                                          interrupted_attempts[si], step);
    }
    co_return interrupt_cp_id;
}

}  // namespace

// =========================================================================
// Dispatched attempts (#364 attempt timeouts; the #367/#368 execution model)
// =========================================================================
//
// The orchestrator (the coroutine driving the super-step) never runs a node
// body itself when the body could hold its thread: on a shared host scheduler
// (RunContext::shared_scheduler, the Program runtime) and for every attempt
// that has a timeout. Such an attempt is one dispatched unit:
//
//   * it runs on a worker strand of attempt_pool(), with a CancelToken of its
//     own (a child of the run's) and an EventFence around its events;
//   * the first terminal event wins: either the body completes, or its
//     timeout passes. Completing at or after the timeout counts as the
//     timeout, whether or not the timer has fired yet;
//   * after a timeout the attempt is cancelled through its token and its
//     events are fenced off, and everything it returns or throws afterwards
//     (its late completion) is discarded: no writes, no events, no pending
//     write. The orchestrator waits for it to stop, so a retry never overlaps
//     the attempt it replaces;
//   * an attempt that does not stop by its stop deadline is detached: the run
//     fails with NG.EffectUncertain. A detached attempt owns everything it
//     uses except its node; an engine destroyed before it finishes hands its
//     nodes over to it (AttemptQuiescence::adopt).

namespace {

using AttemptClock = std::chrono::steady_clock;

// How long a cancelled attempt has to stop: as long as it had to run, and at
// least a second.
constexpr std::chrono::milliseconds kMinAttemptStopGrace{1000};

// Worker threads for dispatched node attempts. Process-wide, created on first
// use; NEOGRAPH_NODE_ATTEMPT_THREADS (1-1024) overrides the default of 32. A
// blocking body holds one thread while it runs.
asio::thread_pool& attempt_pool() {
    static asio::thread_pool pool([] {
        if (const char* env = std::getenv("NEOGRAPH_NODE_ATTEMPT_THREADS")) {
            const long n = std::strtol(env, nullptr, 10);
            if (n > 0 && n <= 1024) return static_cast<std::size_t>(n);
        }
        return static_cast<std::size_t>(32);
    }());
    return pool;
}

// The events one dispatched attempt emits through NodeInput::stream_cb. Once
// the attempt's terminal event is a timeout, nothing more it emits reaches the
// run's callback. Delivery holds the mutex, so closing waits for an event in
// flight and no event of the attempt can follow what the orchestrator emits
// next.
struct EventFence {
    std::mutex                 mutex;
    bool                       closed = false;
    const GraphStreamCallback* sink   = nullptr;

    void deliver(const GraphEvent& event) {
        std::lock_guard lock(mutex);
        if (!closed) (*sink)(event);
    }
    void close() {
        std::lock_guard lock(mutex);
        closed = true;
    }
};

struct QuiescenceLease {
    std::shared_ptr<detail::AttemptQuiescence> owner;
    ~QuiescenceLease() {
        if (owner) owner->leave();
    }
};

// Every dispatched attempt of the process, until it and everything it holds
// is released: wait_for_node_attempts(). Never destroyed, so it stays usable
// while the process exits.
struct RunningAttempts {
    std::mutex              mutex;
    std::condition_variable idle;
    std::size_t             count = 0;
};

RunningAttempts& running_attempts() {
    static auto* const instance = new RunningAttempts();
    return *instance;
}

struct ProcessAttemptLease {
    ProcessAttemptLease() {
        auto&           running = running_attempts();
        std::lock_guard lock(running.mutex);
        ++running.count;
    }
    ~ProcessAttemptLease() {
        auto&           running = running_attempts();
        std::lock_guard lock(running.mutex);
        if (--running.count == 0) running.idle.notify_all();
    }
    ProcessAttemptLease(const ProcessAttemptLease&)            = delete;
    ProcessAttemptLease& operator=(const ProcessAttemptLease&) = delete;
};

// One dispatched node attempt. Shared by the orchestrator, the body coroutine
// and its completion handler.
struct DispatchedAttempt {
    // Declared first, released last: the process count, then the engine's,
    // whose release may release the engine's nodes.
    ProcessAttemptLease process_lease;
    QuiescenceLease     quiescence;

    GraphNode*                  node  = nullptr;
    const GraphState*           state = nullptr;
    // A timed attempt can be detached, so it reads its own copy of the state.
    std::unique_ptr<GraphState> owned_state;
    RunContext                  context;
    // Binds `context` to the run; declared after it so it is released first.
    std::optional<detail::ScopedInvocationRuntime> scope;
    std::shared_ptr<CancelToken> token;      // context.cancel_token: the attempt's own
    std::shared_ptr<CancelToken> execution;  // the body co_spawn's cancellation slot
    std::shared_ptr<EventFence>  events;
    GraphStreamCallback          fenced_sink;

    std::mutex                               mutex;
    std::optional<AttemptClock::time_point>  timeout_at;
    bool                                     settled   = false;  // a terminal event won
    bool                                     timed_out = false;
    bool                                     stopped   = false;  // the body finished
    bool                                     detached  = false;  // nobody waits any more
    std::optional<NodeResult>                result;
    std::exception_ptr                       error;
    // Wakes the waiting orchestrator; set only while it waits.
    std::function<void()>                    wake;

    // The body finished. Its outcome counts only if it is the first terminal
    // event and it arrived before the timeout.
    void finish(std::optional<NodeResult> outcome, std::exception_ptr failure) {
        std::lock_guard lock(mutex);
        stopped = true;
        if (!settled) {
            settled = true;
            if (timeout_at && AttemptClock::now() >= *timeout_at) {
                timed_out = true;
            } else {
                result = std::move(outcome);
                error  = std::move(failure);
            }
        }
        if (wake) wake();
    }
};

asio::awaitable<void> run_dispatched_body(std::shared_ptr<DispatchedAttempt> unit) {
    {
        // Timed out before a worker picked it up: the node never runs.
        std::lock_guard lock(unit->mutex);
        if (unit->settled) {
            unit->stopped = true;
            if (unit->wake) unit->wake();
            co_return;
        }
    }
    // A run cancelled before the body starts does not start it. Its token may
    // have been cancelled before it was bound to this strand, and that emit,
    // queued first, cannot reach a slot the node has not connected yet.
    if (unit->token->is_cancelled()) {
        unit->finish(std::nullopt, std::make_exception_ptr(CancelledException(
                                       "node attempt cancelled before it started")));
        co_return;
    }
    std::optional<NodeResult> result;
    std::exception_ptr        error;
    try {
        NodeInput in{*unit->state, unit->context, unit->events ? &unit->fenced_sink : nullptr};
        result.emplace(co_await unit->node->run(in));
    } catch (...) {
        error = std::current_exception();
    }
    unit->finish(std::move(result), std::move(error));
}

void start_dispatched_body(const std::shared_ptr<DispatchedAttempt>& unit) {
    // As for a run (GraphEngine::run_async): the token the node sees is bound
    // to the serial executor the body runs on, so a node may bind its slot()
    // to what it awaits; the body's own co_spawn uses a separate child token,
    // so cancelling the attempt aborts what the body awaits either way.
    const auto token_executor =
        unit->token->bind_executor(asio::make_strand(attempt_pool().get_executor()));
    const auto body_executor = unit->execution->bind_executor(token_executor);
    asio::post(body_executor, [unit, body_executor] {
        try {
            asio::co_spawn(body_executor, run_dispatched_body(unit),
                           asio::bind_cancellation_slot(
                               unit->execution->slot(),
                               [unit, body_executor](std::exception_ptr error) {
                                   if (error) unit->finish(std::nullopt, std::move(error));
                                   asio::post(body_executor, [unit] {
                                       unit->execution->unbind_executor();
                                       unit->token->unbind_executor();
                                   });
                               }));
        } catch (...) {
            unit->finish(std::nullopt, std::current_exception());
        }
    });
}

// Runs on a strand of the orchestrator's executor until the attempt settles:
// it completed, or it timed out and then stopped, or it timed out and did not
// stop by its stop deadline (detached).
asio::awaitable<void> await_dispatched(std::shared_ptr<DispatchedAttempt> unit,
                                       std::chrono::milliseconds          stop_grace) {
    const auto executor = co_await asio::this_coro::executor;
    auto       timer    = std::make_shared<asio::steady_timer>(executor);
    {
        std::lock_guard lock(unit->mutex);
        unit->wake = [executor, timer] { asio::post(executor, [timer] { timer->cancel(); }); };
    }
    std::optional<AttemptClock::time_point> stop_deadline;
    for (;;) {
        bool                    cancel_attempt = false;
        bool                    done           = false;
        AttemptClock::time_point wake_at{};
        {
            std::lock_guard lock(unit->mutex);
            const auto      now = AttemptClock::now();
            if (unit->stopped) {
                done = true;
            } else {
                if (!unit->settled && unit->timeout_at && now >= *unit->timeout_at) {
                    unit->settled   = true;
                    unit->timed_out = true;
                }
                if (unit->timed_out && !stop_deadline) {
                    stop_deadline  = now + stop_grace;
                    cancel_attempt = true;
                }
                if (stop_deadline && now >= *stop_deadline) {
                    unit->detached = true;
                    done           = true;
                } else if (stop_deadline) {
                    wake_at = *stop_deadline;
                } else {
                    wake_at = unit->timeout_at ? *unit->timeout_at : now + std::chrono::hours(24);
                }
            }
            // No wake-up is posted once the orchestrator stops waiting.
            if (done) unit->wake = nullptr;
        }
        if (cancel_attempt) {
            if (unit->events) unit->events->close();
            unit->token->cancel();
        }
        if (done) break;
        timer->expires_at(wake_at);
        co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    }
    // A late completion settles as a timeout too; fence whatever is left.
    bool timed_out = false;
    {
        std::lock_guard lock(unit->mutex);
        timed_out = unit->timed_out;
    }
    if (timed_out && unit->events) unit->events->close();
}

}  // namespace

bool wait_for_node_attempts(std::chrono::milliseconds timeout) {
    auto&            running = running_attempts();
    std::unique_lock lock(running.mutex);
    const auto       idle = [&running] { return running.count == 0; };
    if (timeout == std::chrono::milliseconds::max()) {
        running.idle.wait(lock, idle);
        return true;
    }
    return running.idle.wait_for(lock, timeout, idle);
}

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
    // #364: each attempt may run this long (milliseconds); none without one.
    const std::optional<int> attempt_timeout =
        attempt_timeout_for_ ? attempt_timeout_for_(node_name) : std::nullopt;

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
        // False only for a timed-out attempt that did not stop by its stop
        // deadline: it may still be running.
        bool attempt_stopped   = true;
        bool attempt_timed_out = false;
        if (attempt_timeout || ctx.shared_scheduler) {
            // A dispatched unit: the body runs off this coroutine's executor
            // (see "Dispatched attempts" above).
            auto unit  = std::make_shared<DispatchedAttempt>();
            unit->node = node_it->second.get();
            if (attempt_timeout) {
                unit->owned_state = std::make_unique<GraphState>();
                init_state(*unit->owned_state);
                unit->owned_state->copy_provider_history_from(state);
                unit->owned_state->restore_runtime(state.serialize_runtime());
                unit->state = unit->owned_state.get();
            } else {
                unit->state = &state;
            }
            // The attempt's own token: run cancellation cascades into it, and
            // a timeout cancels it alone.
            const auto run_token =
                ctx.cancel_token ? ctx.cancel_token : std::make_shared<CancelToken>();
            unit->context              = ctx;
            unit->token                = run_token->fork();
            unit->execution            = unit->token->fork();
            unit->context.cancel_token = unit->token;
            if (slot) bind_resume_value(ctx, unit->context, *slot);
            if (slot) {
                unit->scope.emplace(ctx, unit->context, effect_namespace, true);
            } else {
                const auto parent = detail::runtime_for(ctx);
                unit->scope.emplace(ctx, unit->context,
                                    parent ? parent.invocation_id() : std::string_view{});
            }
            effects = unit->scope->effects();
            if (cb) {
                unit->events       = std::make_shared<EventFence>();
                unit->events->sink = &cb;
                unit->fenced_sink  = [events = unit->events](const GraphEvent& event) {
                    events->deliver(event);
                };
            }
            if (attempt_quiescence_) {
                attempt_quiescence_->enter();
                unit->quiescence.owner = attempt_quiescence_;
            }
            if (attempt_timeout)
                unit->timeout_at = AttemptClock::now() + std::chrono::milliseconds(*attempt_timeout);
            start_dispatched_body(unit);
            const auto stop_grace = std::max(std::chrono::milliseconds(attempt_timeout.value_or(0)),
                                             kMinAttemptStopGrace);
            // Run cancellation reaches the body through its token; the wait
            // itself is not cancelled, so a cancelled run still waits for
            // the body to stop, as an inline body would be waited for.
            co_await asio::co_spawn(
                asio::make_strand(ex), await_dispatched(unit, stop_grace),
                asio::bind_cancellation_slot(asio::cancellation_slot(), asio::use_awaitable));
            {
                std::lock_guard lock(unit->mutex);
                attempt_timed_out = unit->timed_out;
                attempt_stopped   = !unit->detached;
                if (!attempt_timed_out) {
                    ok_result = std::move(unit->result);
                    failure   = std::move(unit->error);
                }
            }
            if (attempt_timed_out) {
                // The late completion, if any, is discarded.
                ok_result.reset();
                failure = std::make_exception_ptr(NodeAttemptTimeout(
                    node_name, attempt, std::chrono::milliseconds(*attempt_timeout), attempt_stopped));
                // Run control wins over the attempt's own timeout.
                if (ctx.cancel_token && ctx.cancel_token->is_cancelled())
                    throw CancelledException("node " + node_name +
                                             " cancelled after its attempt timed out");
            } else if (!ok_result && !failure) {
                failure = std::make_exception_ptr(
                    std::logic_error("Node '" + node_name + "' attempt finished without an outcome"));
            }
        } else {
            std::optional<ScopedInvocationContext> scope;
            if (slot) scope.emplace(ctx, effect_namespace, slot);
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
        // An attempt still running past its stop deadline does not pause: its
        // effects are uncertain as a whole and the run fails (below).
        if (attempt_stopped && effects && effects->uncertain()) {
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

        // #364: a backoff that would cross the run's deadline (a Program's
        // wall-time budget) stops retrying now, instead of sleeping until the
        // budget stops the run.
        if (ctx.deadline &&
            AttemptClock::now() + std::chrono::milliseconds(actual_delay) >= *ctx.deadline)
            co_return give_up(std::move(failure), attempt + 1, verdict, NodeFailureKind::Deadline,
                              effects);

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

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(actual_delay));
        co_await timer.async_wait(asio::use_awaitable);
        // A run cancelled during the backoff starts no further attempt.
        if (ctx.cancel_token) ctx.cancel_token->throw_if_cancelled("node " + node_name + " retry");

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
        std::vector<PendingInterrupt> pending;
        pending.push_back(pending_interrupt(task_id, node_name, *interrupt));
        const auto metadata        = record_step_interrupts(ctx, std::move(pending));
        const auto interrupt_cp_id = co_await coord.save_super_step_async(
            state, node_name, next_nodes, CheckpointPhase::NodeInterrupt, step, parent_cp_id,
            barrier_state, metadata);
        // Without a parent checkpoint the attempt's start was not recorded;
        // record it under the interrupt's own checkpoint so resume continues it.
        if (parent_cp_id.empty())
            co_await record_attempt_start(coord, interrupt_cp_id, task_id, node_name,
                                          interrupted_attempt, step);
        // The executor is the only layer that knows the graph's name for
        // this node — the node body does not.
        interrupt->set_node(node_name);
        record_interrupt_checkpoint(ctx, interrupt_cp_id);
        throw *interrupt;
    }

    auto& nr = *ok_result;
    capture_step_start(ctx, state, !nr.sends.empty());
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
                std::vector<PendingInterrupt> pending;
                pending.push_back(pending_interrupt(task_id, node_name, *interrupt));
                const auto metadata        = record_step_interrupts(ctx, std::move(pending));
                const auto interrupt_cp_id = co_await coord.save_super_step_async(
                    state, node_name, next_nodes, CheckpointPhase::NodeInterrupt, step,
                    parent_cp_id, barrier_state, metadata);
                if (parent_cp_id.empty())
                    co_await record_attempt_start(coord, interrupt_cp_id, task_id, node_name,
                                                  interrupted_attempt, step);
                interrupt->set_node(node_name);
                record_interrupt_checkpoint(ctx, interrupt_cp_id);
                throw *interrupt;
            }
            if (coord.records_pending_writes(parent_cp_id))
                co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, node_name, nr,
                                                          step);
        }
        capture_step_start(ctx, state, !nr.sends.empty());
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

    // #392: the step's outcome does not depend on which branch has the lower
    // index (step_outcome). A failure fails the run; an interrupt pauses it,
    // with a dedicated cp-save with the full ready set, only when no branch
    // failed or when it asks to reconcile an uncertain effect. Successful
    // siblings have pending writes; resume replays those and runs again only
    // the branches that did not finish.
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
    const auto outcome = step_outcome(excs);
    if (outcome.end) std::rethrow_exception(excs[*outcome.end]);
    if (!outcome.pause.empty()) {
        // A whole copy of the interrupt (reason + payload), not just its
        // reason. Copying it here is what keeps the TSan property described
        // above: the strings and json are deep-copied out while we still hold
        // the eptr, so what we throw later is wholly owned by this thread.
        std::optional<NodeInterrupt> interrupt;
        std::string                  interrupt_node;
        // #388: every branch the pause reports, not just the first; copied
        // out of its exception_ptr here, for the same reason as the first one.
        std::vector<PendingInterrupt> pending;
        for (const auto i : outcome.pause) {
            try {
                std::rethrow_exception(excs[i]);
            } catch (const NodeInterrupt& ni) {
                if (!interrupt) {
                    interrupt      = ni;
                    interrupt_node = ready[i];
                }
                pending.push_back(
                    pending_interrupt(make_static_task_id(step, ready[i]), ready[i], ni));
            }
        }
        // GCC-13 workaround: build the next_nodes vector outside
        // the co_await arg list (nested brace-init trips the ICE).
        std::vector<std::string> next_nodes;
        next_nodes = ready;
        const auto metadata =
            record_step_interrupts(ctx, std::move(pending), outcome.beside_failure);
        const auto interrupt_cp_id = co_await coord.save_super_step_async(
            state, interrupt_node, next_nodes, CheckpointPhase::NodeInterrupt, step, parent_cp_id,
            barrier_state, metadata);
        if (parent_cp_id.empty()) {
            // No checkpoint precedes the step: record under the interrupt
            // checkpoint what a parent would hold, so that resume replays
            // a sibling that finished instead of running it again (#385).
            for (std::size_t i = 0; i < ready.size(); ++i) {
                const auto branch_task_id = make_static_task_id(step, ready[i]);
                if (!excs[i])
                    co_await coord.record_pending_write_async(interrupt_cp_id, branch_task_id,
                                                              branch_task_id, ready[i], values[i],
                                                              step);
                else
                    co_await record_attempt_start(coord, interrupt_cp_id, branch_task_id,
                                                  ready[i], interrupted_attempts[i], step);
            }
        }
        // Throw a fresh NodeInterrupt — see the comment above. The fresh
        // exception is wholly owned by the current (main) thread.
        interrupt->set_node(interrupt_node);
        record_interrupt_checkpoint(ctx, interrupt_cp_id);
        throw *interrupt;
    }

    // Apply results in ready-order (scheduler pairs ready[i] ↔ results[i]).
    capture_step_start(ctx, state,
                       std::any_of(values.begin(), values.end(),
                                   [](const NodeResult& value) { return !value.sends.empty(); }));
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
// run_sends_async: dynamic fan-out
// =========================================================================
//
// Every Send runs on its own GraphState copy: the step's state with its
// payload applied (#389), with retry. One Send runs inline; several run as a
// make_parallel_group of deferred workers. Either way the results fan back
// to the shared state in Send order once every Send has finished.
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
    std::vector<json>*                                 failure_records,
    const detail::SendStep*                            send_step) {
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

    // #385: a Send that interrupts pauses the run like a ready node: a
    // NodeInterrupt checkpoint that re-enters the step, then the interrupt.
    // The attempt each Send was in when it paused is written by that Send only.
    std::vector<int> interrupted_attempts(sends.size(), 0);
    // `pending` (#388) lists every Send that paused, the first one first.
    // `listed` (#392): the pause holds back a failure, and its checkpoint lists
    // what it answers.
    auto pause = [&](std::size_t interrupted, const std::vector<const NodeResult*>& finished,
                     std::vector<PendingInterrupt> pending,
                     bool listed) -> asio::awaitable<void> {
        const auto metadata = record_step_interrupts(ctx, std::move(pending), listed);
        if (!send_step || !coord.enabled()) co_return;
        const auto runtime = detail::runtime_for(ctx);
        if (!runtime || !runtime->step_start || !runtime->step_start->state)
            throw std::logic_error("A Send interrupted without the state its step started from");
        GraphState step_state;
        init_state(step_state);
        step_state.copy_provider_history_from(state);
        step_state.restore_runtime(*runtime->step_start->state);
        const auto checkpoint_id =
            co_await save_send_interrupt(coord, step_state, *send_step, sends, finished,
                                         interrupted_attempts, interrupted, step, parent_cp_id,
                                         metadata);
        record_interrupt_checkpoint(ctx, checkpoint_id);
    };

    // #389: every Send runs on a state of its own, one Send or many: the state
    // the Sends start from (after the step's ready nodes applied their
    // writes), with its own payload applied. A payload is visible only to its
    // own Send execution, each retry attempt of it included, and never to the
    // shared run state; what a Send leaves behind is the writes it returns,
    // folded in in Send order once every Send has finished. The step's state
    // is serialized once and every Send restores from that snapshot; a step
    // whose Sends all replay serializes nothing.
    std::vector<std::string> send_task_ids;
    send_task_ids.reserve(sends.size());
    bool any_runs = false;
    for (std::size_t si = 0; si < sends.size(); ++si) {
        send_task_ids.push_back(make_send_task_id(step, si, sends[si].target_node, sends[si].input));
        if (!replay.count(send_task_ids.back())) any_runs = true;
    }
    const auto state_snapshot = any_runs ? state.serialize_runtime() : json();

    auto run_send = [&, this](std::size_t si) -> asio::awaitable<NodeResult> {
        const auto&        s       = sends[si];
        const std::string& task_id = send_task_ids[si];

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
        // is captured by reference into this worker — no serialize/restore
        // hop, no manual forward.

        detail::InvocationSlot slot{task_id, step, &replay, &coord, &parent_cp_id,
                                    &interrupted_attempts[si]};
        slot.visit_limited = visit_limited[si] != 0;
        auto nr = co_await execute_attempts_async(s.target_node, send_state, cb, stream_mode, ctx,
                                                  &slot);
        if (coord.records_pending_writes(parent_cp_id))
            co_await coord.record_pending_write_async(parent_cp_id, task_id, task_id, s.target_node, nr,
                                                      step);
        co_return nr;
    };

    std::vector<NodeResult> values(sends.size());
    if (sends.size() == 1) {
        // One Send runs inline, without a parallel group, on the same state
        // contract as many. GCC-13: no co_await inside a catch handler.
        std::optional<NodeInterrupt> interrupt;
        try {
            values[0] = co_await run_send(0);
        } catch (const NodeInterrupt& ni) {
            interrupt = ni;
        }
        if (interrupt) {
            const std::vector<const NodeResult*> finished(sends.size(), nullptr);
            std::vector<PendingInterrupt>        pending;
            pending.push_back(
                pending_interrupt(send_task_ids[0], sends[0].target_node, *interrupt));
            co_await pause(0, finished, std::move(pending), false);
            interrupt->set_node(sends[0].target_node);
            throw *interrupt;
        }
    } else {
        maybe_warn_serial_fanout(sends.size());
        auto                  outer_ex = co_await asio::this_coro::executor;
        asio::any_io_executor ex       = fan_out_pool_
                                             ? asio::any_io_executor(fan_out_pool_->get_executor())
                                             : asio::any_io_executor(outer_ex);

        // With an engine fan-out pool a branch can resume on any pool thread, and
        // the parallel group forwards a cancellation by dispatching it on the
        // branch's executor. On the bare pool executor that emit can run on one
        // thread while the branch (installing and clearing the cancellation
        // handlers of its awaited operations) runs on another, and Asio's
        // cancellation slots are not thread-safe. A strand per branch keeps each
        // coroutine's cancellation state on one serial executor, as 654bcd0 did
        // for static fan-out (#344). Without a pool the branches share the outer
        // executor, which is already serial, so they stay on it.
        auto branch_executor = [&]() -> asio::any_io_executor {
            return fan_out_pool_ ? asio::any_io_executor(asio::make_strand(ex)) : ex;
        };
        using DeferredOp = decltype(asio::co_spawn(std::declval<asio::any_io_executor>(),
                                                   run_send(std::size_t{0}), asio::deferred));
        std::vector<DeferredOp> ops;
        ops.reserve(sends.size());
        for (std::size_t si = 0; si < sends.size(); ++si) {
            ops.push_back(asio::co_spawn(branch_executor(), run_send(si), asio::deferred));
        }

        auto [order, excs, results] =
            co_await asio::experimental::make_parallel_group(std::move(ops))
                .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
        (void)order;

        // The step's outcome, by the same rule as run_parallel_async's
        // (#392): a failure fails the run whatever its Send index, and an
        // interrupt pauses it (#385) only when no Send failed or when it asks
        // to reconcile an uncertain effect. The Sends that finished replay on
        // resume and the ones that did not run again. As there, the interrupt
        // is copied out of the worker's exception_ptr before the checkpoint
        // I/O and thrown afresh from this thread.
        const auto outcome = step_outcome(excs);
        if (outcome.end) std::rethrow_exception(excs[*outcome.end]);
        if (!outcome.pause.empty()) {
            std::size_t                  first_index = outcome.pause.front();
            std::optional<NodeInterrupt> interrupt;
            // #388: every Send the pause reports, not just the first.
            std::vector<PendingInterrupt> pending;
            for (const auto i : outcome.pause) {
                try {
                    std::rethrow_exception(excs[i]);
                } catch (const NodeInterrupt& ni) {
                    if (!interrupt) interrupt = ni;
                    pending.push_back(pending_interrupt(send_task_ids[i], sends[i].target_node, ni));
                }
            }
            std::vector<const NodeResult*> finished(sends.size(), nullptr);
            for (std::size_t i = 0; i < sends.size(); ++i)
                if (!excs[i]) finished[i] = &results[i];
            co_await pause(first_index, finished, std::move(pending), outcome.beside_failure);
            interrupt->set_node(sends[first_index].target_node);
            throw *interrupt;
        }
        values = std::move(results);
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
