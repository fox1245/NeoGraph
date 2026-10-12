// Bounded-exhaustive resume/replay harness.
//
// tests/resume_model.h states the documented interrupt/resume contract as a
// reference interpreter over a family of small graphs (Ok, Ask, Fail, Flaky
// and Map nodes, a Send worker, static and answer-routed edges, failure
// edges, a barrier join, interrupt_before and interrupt_after gates). For
// every graph this harness explores every answer schedule of the reference
// (which pending question each resume answers, by id or by default, and with
// which label), and drives the real GraphEngine through the same schedule:
//
//  - crash-free, on an in-memory checkpoint store and on SQLite with a coarse
//    clock (every checkpoint saved with one timestamp);
//  - with a crash at every durable operation (each save and each put_writes)
//    of the run; the crash stops the "process", and the host recovers from
//    durable state as a real host would (see Host);
//  - with a fork of every pause, the run continuing on the fork.
//
// Every schedule runs with asks that take only an answer addressed to their
// own invocation, and with asks that take any resume value they receive.
// Properties, checked on every schedule against the reference:
//
//  P1 exactly once: no invocation body completes again after its completion
//     was durable (a pending write, or the commit of its step). A body may
//     run again only when a crash lost its completion first.
//  P2 every question asked: the pauses the host sees, and their pending
//     questions with payloads, equal the reference's; every answered Ask
//     consumed exactly the answer the schedule gave it, and none ran past an
//     unanswered question.
//  P3 same result: the final status, log, failure records and routes equal
//     the reference.
//  P4 crash transparency: P1-P3 hold for every crash schedule.
//  P5 fork equivalence: P1-P3 hold when the run continues on a fork.
//
// NEOGRAPH_RESUME_MODEL_BOUND=extended runs the larger bound; tests/model/
// README.md lists the bounds and the variables.

#include <neograph/graph/compiler.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/graph/state.h>
#include <neograph/graph/validator.h>

#include "fixtures/coarse_clock_store.h"
#include "resume_model.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace neograph;
using namespace neograph::graph;
using namespace resume_model;

// ---------------------------------------------------------------------------
// The journal: every body execution and every durable store operation
// ---------------------------------------------------------------------------

/// Thrown when the simulated process dies. Not a std::exception, so no node
/// code mistakes it for an ordinary failure it may handle.
struct ProcessDeath {};

struct Event {
    enum Type { Done, Failed, Asked, Put, Save, Crash, Unaddressed } type;
    std::string thread;
    std::string task;   // invocation id (Done/Failed/Asked/Put), checkpoint id (Save)
    int         attempt = 0;
    std::string node;
    int         step = 0;
    std::string phase;     // Save
    std::string consumed;  // Done of an Ask: the answer it consumed
};

class Journal {
public:
    void reset() {
        std::lock_guard lock(mutex_);
        events_.clear();
        dead_ = false;
    }
    void record(Event event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    std::vector<Event> events() const {
        std::lock_guard lock(mutex_);
        return events_;
    }
    bool dead() const { return dead_.load(); }
    void die() {
        std::lock_guard lock(mutex_);
        if (!dead_) events_.push_back(Event{Event::Crash});
        dead_ = true;
    }
    void revive() { dead_ = false; }

private:
    mutable std::mutex mutex_;
    std::vector<Event> events_;
    std::atomic<bool>  dead_{false};
};

// One journal per worker thread: a worker drives its engines on its own
// thread (no engine pool, store operations inline), so every node body and
// store operation of its runs happens there.
thread_local Journal* current_journal = nullptr;

Journal& journal() {
    if (!current_journal) throw std::logic_error("no resume model journal on this thread");
    return *current_journal;
}

std::pair<std::string, int> split_namespace(const std::string& ns) {
    const auto at = ns.rfind("#a");
    if (at == std::string::npos) return {ns, 0};
    return {ns.substr(0, at), std::atoi(ns.c_str() + at + 2)};
}

int step_of(const std::string& task) {
    return task.size() > 1 && task[0] == 's' ? std::atoi(task.c_str() + 1) : -1;
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

class ModelNode final : public GraphNode {
public:
    ModelNode(std::string name, const json& config) : name_(std::move(name)) {
        kind_        = config.value("kind", std::string("ok"));
        conditional_ = config.value("conditional", false);
        addressed_   = config.value("addressed", false);
        sends_       = config.value("sends", 0);
        if (config.contains("work"))
            for (std::size_t i = 0; i < config["work"].size(); ++i)
                work_.push_back(config["work"][i].get<std::string>());
    }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto& log = journal();
        if (log.dead()) throw ProcessDeath{};
        const auto [task, attempt] = split_namespace(make_tool_execution_context(in.ctx).effect_task_id);
        const int  step            = in.ctx.step;
        int        item            = -1;
        std::string behaviour      = kind_;
        if (kind_ == "work") {
            item      = in.state.get("item").get<int>();
            behaviour = work_.at(static_cast<std::size_t>(item));
        }
        const auto event = [&](Event::Type type, std::string consumed = {}) {
            log.record(Event{type, in.ctx.thread_id, task, attempt, name_, step, {}, std::move(consumed)});
        };
        NodeOutput out;
        const auto append = [&](json tag) {
            json entry = item < 0 ? json::array({step, name_, std::move(tag)})
                                  : json::array({step, name_, item, std::move(tag)});
            out.writes.push_back(ChannelWrite{"log", json::array({std::move(entry)})});
        };
        if (behaviour == "ok" || (behaviour == "flaky" && attempt > 0)) {
            event(Event::Done);
            append("ok");
        } else if (behaviour == "fail" || behaviour == "flaky") {
            event(Event::Failed);
            throw std::runtime_error("model failure of " + name_);
        } else if (behaviour == "map") {
            event(Event::Done);
            append("map");
            for (int i = 0; i < sends_; ++i) out.sends.push_back(Send{"w", send_input(i)});
        } else if (behaviour == "ask") {
            std::optional<std::string> answer;
            if (const auto& value = in.ctx.resume_value) {
                if (!addressed_) {
                    answer = value->is_string() ? value->get<std::string>() : value->dump();
                } else if (value->is_object() && value->value("to", std::string()) == task) {
                    answer = (*value)["v"].get<std::string>();
                } else {
                    event(Event::Unaddressed);
                }
            }
            if (!answer) {
                event(Event::Asked);
                json payload{{"node", name_}, {"step", step}};
                if (item >= 0) payload["item"] = item;
                throw NodeInterrupt("ask " + name_, std::move(payload));
            }
            event(Event::Done, *answer);
            append(*answer);
            if (conditional_) out.writes.push_back(ChannelWrite{"route_" + name_, json(answer->substr(0, 1))});
        } else {
            throw std::logic_error("unknown model behaviour " + behaviour);
        }
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string              name_, kind_;
    bool                     conditional_ = false, addressed_ = false;
    int                      sends_ = 0;
    std::vector<std::string> work_;
};

void register_model() {
    static const bool registered = [] {
#if defined(_WIN32)
        _putenv_s("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1");
#else
        setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1", 1);
#endif
        NodeFactory::instance().register_type(
            "resume_model.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<ModelNode>(name, config);
            },
            json{{"type", "object"}});
        for (int u = 0; u < 8; ++u) {
            const auto channel = "route_n" + std::to_string(u);
            ConditionRegistry::instance().register_condition(
                "resume_model.route.n" + std::to_string(u),
                [channel](const GraphState& state) { return state.get(channel).get<std::string>(); });
        }
        return true;
    }();
    (void)registered;
}

// ---------------------------------------------------------------------------
// The crashing checkpoint store
// ---------------------------------------------------------------------------

/// Delegates to a real store. Counts the durable operations (save and
/// put_writes) and, when armed, kills the process before or after operation
/// k: the operation is lost (before) or durable (after), and the store throws
/// ProcessDeath. A dead process reaches the store no more: every operation
/// throws until the host restarts.
class CrashStore final : public CheckpointStore {
public:
    void reset(std::shared_ptr<CheckpointStore> inner) {
        inner_ = std::move(inner);
        ops_   = 0;
        crashes_.clear();
    }
    /// Crash at durable operation `op` (counted from 0 over the whole
    /// schedule), before it applies or after.
    void arm(int op, bool after) { crashes_.push_back({op, after}); }
    int  ops() const { return ops_; }
    CheckpointStore& inner() { return *inner_; }

    void save(const Checkpoint& checkpoint) override {
        durable([&] { inner_->save(checkpoint); },
                Event{Event::Save, checkpoint.thread_id, checkpoint.id, 0, checkpoint.current_node,
                      static_cast<int>(checkpoint.step), to_string(checkpoint.interrupt_phase), {}});
    }
    void put_writes(const std::string& thread, const std::string& parent, const PendingWrite& write) override {
        durable([&] { inner_->put_writes(thread, parent, write); },
                Event{Event::Put, thread, write.task_id, 0, write.node_name, static_cast<int>(write.step), {}, {}});
    }
    std::optional<Checkpoint> load_latest(const std::string& thread) override {
        alive();
        return inner_->load_latest(thread);
    }
    std::optional<Checkpoint> load_by_id(const std::string& id) override {
        alive();
        return inner_->load_by_id(id);
    }
    std::vector<Checkpoint> list(const std::string& thread, int limit) override {
        alive();
        return inner_->list(thread, limit);
    }
    void delete_thread(const std::string& thread) override {
        alive();
        inner_->delete_thread(thread);
    }
    std::vector<PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        alive();
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override {
        alive();
        inner_->clear_writes(thread, parent);
    }
    bool requires_managed_budget(const std::string& thread) override {
        alive();
        return inner_->requires_managed_budget(thread);
    }

    // The async peers run inline: the default offloads to a worker pool,
    // which costs a thread handoff per operation and lets parallel branches
    // interleave their store operations nondeterministically.
    asio::awaitable<void> save_async(const Checkpoint& checkpoint) override {
        save(checkpoint);
        co_return;
    }
    asio::awaitable<std::optional<Checkpoint>> load_latest_async(const std::string& thread) override {
        co_return load_latest(thread);
    }
    asio::awaitable<std::optional<Checkpoint>> load_by_id_async(const std::string& id) override {
        co_return load_by_id(id);
    }
    asio::awaitable<std::vector<Checkpoint>> list_async(const std::string& thread, int limit) override {
        co_return list(thread, limit);
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread) override {
        delete_thread(thread);
        co_return;
    }
    asio::awaitable<void> put_writes_async(const std::string& thread, const std::string& parent,
                                           const PendingWrite& write) override {
        put_writes(thread, parent, write);
        co_return;
    }
    asio::awaitable<std::vector<PendingWrite>> get_writes_async(const std::string& thread,
                                                                const std::string& parent) override {
        co_return get_writes(thread, parent);
    }
    asio::awaitable<void> clear_writes_async(const std::string& thread, const std::string& parent) override {
        clear_writes(thread, parent);
        co_return;
    }
    asio::awaitable<bool> requires_managed_budget_async(std::string thread) override {
        co_return requires_managed_budget(thread);
    }

private:
    void alive() const {
        if (journal().dead()) throw ProcessDeath{};
    }
    template <class Fn>
    void durable(Fn&& apply, Event event) {
        alive();
        const int op = ops_++;
        for (const auto& [at, after] : crashes_) {
            if (at != op) continue;
            if (after) {
                apply();
                journal().record(std::move(event));
            }
            journal().die();
            throw ProcessDeath{};
        }
        apply();
        journal().record(std::move(event));
    }

    std::shared_ptr<CheckpointStore>  inner_;
    int                               ops_ = 0;
    std::vector<std::pair<int, bool>> crashes_;
};

// ---------------------------------------------------------------------------
// Engines
// ---------------------------------------------------------------------------

struct Engines {
    std::shared_ptr<CrashStore>  store = std::make_shared<CrashStore>();
    std::unique_ptr<GraphEngine> engine;
};

/// An engine for the graph, or nullptr when the validator rejects it. The
/// strict (schema 1) document is parsed for its failure edges and linked as
/// schema 0, which keeps the strict lint off stderr (as the join harness does).
std::unique_ptr<Engines> make_engines(const Graph& graph, bool addressed) {
    auto topology = GraphCompiler::parse([&] {
        auto doc              = definition(graph, addressed);
        doc["schema_version"] = 1;
        return doc;
    }());
    if (GraphValidator::validate(topology).has_errors()) return nullptr;
    topology.schema_version = 0;
    auto engines = std::make_unique<Engines>();
    EngineConfig config;
    config.checkpoint_store = engines->store;
    config.stderr_notices   = false;
    for (int u = 0; u < graph.n; ++u) {
        if (graph.nodes[u].kind != Kind::Flaky) continue;
        RetryPolicy retry;
        retry.max_retries        = 1;
        retry.initial_delay_ms   = 0;
        retry.backoff_multiplier = 1.0f;
        retry.max_delay_ms       = 0;
        config.node_retry_policies[name_of(graph, u)] = retry;
    }
    engines->engine = GraphEngine::link(GraphCompiler::link(std::move(topology), NodeContext{}), std::move(config));
    return engines;
}

// ---------------------------------------------------------------------------
// Schedules and the host
// ---------------------------------------------------------------------------

struct Schedule {
    std::vector<Answer>               answers;
    std::vector<std::pair<int, bool>> crashes;   // durable op, after
    int                               fork_at = -1;  // fork after surfacing this pause
};

std::string describe(const Schedule& schedule) {
    std::ostringstream out;
    out << "answers [";
    for (std::size_t i = 0; i < schedule.answers.size(); ++i)
        out << (i ? ", " : "") << (schedule.answers[i].index < 0 ? "default" : "#" + std::to_string(schedule.answers[i].index))
            << "/" << schedule.answers[i].label;
    out << "]";
    for (const auto& [op, after] : schedule.crashes) out << " crash " << (after ? "after" : "before") << " op " << op;
    if (schedule.fork_at >= 0) out << " fork at pause " << schedule.fork_at;
    return out.str();
}

/// What the real run showed.
struct Observed {
    Status                             status = Status::Completed;
    std::vector<std::vector<Question>> pauses;
    json                               log      = json::array();
    json                               failures = json::array();
    std::map<std::string, std::string> routes;
    std::string                        error;  // an unexpected exception
    std::vector<Event>                 events;
    int                                crashes = 0;
    int                                ops     = 0;
};

json answer_value(bool addressed, const std::string& to, const std::string& tag) {
    if (!addressed) return json(tag);
    return json{{"to", to}, {"v", tag}};
}

/// A host that keeps one durable record, the command it is executing, and
/// after a crash recovers from the store as a real host would:
///  - no checkpoint: run the input again;
///  - the latest checkpoint is the one the interrupted resume was answering:
///    issue the same resume again (same value, same interrupt id);
///  - the latest checkpoint is an interrupt_before or interrupt_after pause:
///    it is a question the host has not seen; surface it (resuming it would
///    answer it);
///  - otherwise resume the latest checkpoint without a value: a NodeInterrupt
///    step asks its questions again, a committed step continues.
class Host {
public:
    Host(const Graph& graph, Engines& engines, bool addressed, std::string thread)
        : graph_(graph), engines_(engines), addressed_(addressed), thread_(std::move(thread)) {}

    struct Outcome {
        Status                status = Status::Completed;
        std::vector<Question> pause;
        std::string           checkpoint;  // the pause's checkpoint, as the result reports it
        json                  output;
        std::string           error;
    };

    Outcome start() {
        command_ = Command{Command::Run, {}, json(), {}};
        return execute();
    }

    /// Answers `pause`, which the result (or a recovery) reported at
    /// `checkpoint`. The host resumes the thread, as resume(thread_id) does:
    /// from the store's latest checkpoint.
    Outcome answer(const std::vector<Question>& pause, const std::string& checkpoint, const Answer& answer,
                   int sequence) {
        const auto  tag    = answer_tag(answer, sequence);
        std::string target = pause.at(answer.index < 0 ? 0 : static_cast<std::size_t>(answer.index)).id;
        json        value;
        // A gate's approval reaches the node it held (#393); an addressed
        // Ask takes only an answer addressed to it, so not this one.
        if (pause.front().gate)
            value = answer_value(addressed_, "gate", tag);
        else
            value = answer_value(addressed_, target, tag);
        command_ = Command{Command::Resume, checkpoint, std::move(value),
                           answer.index < 0 ? std::string() : target};
        return execute();
    }

    /// The latest checkpoint of the thread the host continues on.
    std::string latest_checkpoint() {
        const auto latest = engines_.store->inner().load_latest(thread_);
        return latest ? latest->id : std::string();
    }

    /// Forks the latest checkpoint into a new thread and continues there.
    void fork() {
        while (true) {
            const auto target = thread_ + "-fork" + std::to_string(++forks_);
            try {
                engines_.engine->fork(thread_, target);
                if (journal().dead()) throw ProcessDeath{};
                thread_ = target;
                return;
            } catch (...) {
                if (!journal().dead()) throw;
                restart();
            }
        }
    }

    int crashes() const { return crashes_; }

private:
    struct Command {
        enum Type { Run, Resume } type;
        std::string checkpoint;  // the checkpoint a Resume answers
        json        value;
        std::string interrupt_id;
    };

    void restart() {
        ++crashes_;
        journal().revive();
    }

    Outcome execute() {
        while (true) {
            std::optional<RunResult> result;
            std::string              error;
            bool                     failed = false;
            try {
                RunConfig config;
                config.thread_id = thread_;
                config.max_steps = 20;
                if (command_.type == Command::Run) {
                    result = engines_.engine->run(config);
                } else {
                    RunMetadata metadata;
                    metadata.resume_interrupt_id = command_.interrupt_id;
                    result = engines_.engine->resume(config, command_.value, nullptr, metadata);
                }
            } catch (const NodeExecutionError& e) {
                failed = true;
                error  = e.what();
            } catch (const ProcessDeath&) {
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "unknown exception";
            }
            if (!journal().dead()) {
                Outcome outcome;
                if (failed) {
                    outcome.status = Status::Failed;
                    return outcome;
                }
                if (!result) {
                    outcome.status = Status::Failed;
                    outcome.error  = error.empty() ? "no result" : error;
                    return outcome;
                }
                return observe(*result);
            }
            // The process died: recover from durable state.
            restart();
            const auto latest = engines_.store->inner().load_latest(thread_);
            if (!latest) {
                command_ = Command{Command::Run, {}, json(), {}};
                continue;
            }
            if (command_.type == Command::Resume && latest->id == command_.checkpoint) continue;
            if (latest->interrupt_phase == CheckpointPhase::Before ||
                latest->interrupt_phase == CheckpointPhase::After) {
                Outcome outcome;
                outcome.status     = Status::Interrupted;
                outcome.pause      = {Question{"", latest->current_node,
                                          json(latest->interrupt_phase == CheckpointPhase::Before ? "before"
                                                                                                    : "after"),
                                          true}};
                outcome.checkpoint = latest->id;
                outcome.output     = latest->channel_values;  // the state the pause holds
                return outcome;
            }
            command_ = Command{Command::Resume, latest->id, json(), {}};
        }
    }

    Outcome observe(const RunResult& result) {
        Outcome outcome;
        outcome.output     = result.output;
        outcome.checkpoint = result.checkpoint_id;
        switch (result.status()) {
            case RunStatus::Completed: outcome.status = Status::Completed; break;
            case RunStatus::Interrupted: outcome.status = Status::Interrupted; break;
            case RunStatus::BarrierStalled: outcome.status = Status::Stalled; break;
            case RunStatus::StepLimit: outcome.status = Status::StepLimit; break;
            default:
                outcome.status = Status::Failed;
                outcome.error  = "unexpected run status";
        }
        if (outcome.status == Status::Interrupted) {
            if (result.interrupts.empty()) {
                const auto message = result.interrupt_value.value("message", std::string());
                outcome.pause = {Question{"", result.interrupt_node,
                                          json(message.rfind("Interrupt after", 0) == 0 ? "after" : "before"),
                                          true}};
            } else {
                for (const auto& interrupt : result.interrupts)
                    outcome.pause.push_back(Question{interrupt.id, interrupt.node,
                                                     interrupt.value.contains("value") ? interrupt.value["value"] : json(),
                                                     false});
            }
        }
        return outcome;
    }

    const Graph& graph_;
    Engines&     engines_;
    bool         addressed_;
    std::string  thread_;
    Command      command_{Command::Run, {}, json(), {}};
    int          crashes_ = 0;
    int          forks_   = 0;
};

Observed run_schedule(const Graph& graph, Engines& engines, bool addressed, const Schedule& schedule,
                      std::shared_ptr<CheckpointStore> backing, const std::string& thread) {
    journal().reset();
    engines.store->reset(std::move(backing));
    for (const auto& [op, after] : schedule.crashes) engines.store->arm(op, after);
    Host     host(graph, engines, addressed, thread);
    Observed observed;
    auto     outcome = host.start();
    for (std::size_t k = 0; outcome.status == Status::Interrupted && outcome.error.empty(); ++k) {
        observed.pauses.push_back(outcome.pause);
        if (k >= schedule.answers.size()) break;
        // The engine's pause differs from the reference's: P2 reports it.
        if (schedule.answers[k].index >= static_cast<int>(outcome.pause.size())) break;
        auto checkpoint = outcome.checkpoint;
        if (schedule.fork_at == static_cast<int>(k)) {
            host.fork();
            checkpoint = host.latest_checkpoint();
        }
        outcome = host.answer(observed.pauses.back(), checkpoint, schedule.answers[k], static_cast<int>(k) + 1);
    }
    observed.status  = outcome.status;
    observed.error   = outcome.error;
    observed.crashes = host.crashes();
    observed.ops     = engines.store->ops();
    if (outcome.output.is_object() && outcome.output.contains("channels")) {
        const auto& channels = outcome.output["channels"];
        observed.log         = channels["log"]["value"];
        if (channels.contains("failures"))
            for (const auto& record : channels["failures"]["value"])
                observed.failures.push_back(
                    json::array({record["node"], record["step"], record["routed_to"]}));
        for (int u = 0; u < graph.n; ++u)
            if (graph.nodes[u].conditional)
                observed.routes[name_of(graph, u)] =
                    channels["route_" + name_of(graph, u)]["value"].get<std::string>();
    }
    observed.events = journal().events();
    return observed;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

struct Verdict {
    std::vector<std::string> problems;  // property violations, the first diverging event first
};

/// P1 on the journal: a body that completed again after its completion was
/// durable (its pending write, or its step's commit) ran finished work again.
void check_exactly_once(const Observed& observed, Verdict& verdict) {
    std::map<std::string, std::size_t> last_done;
    const auto& events = observed.events;
    for (std::size_t j = 0; j < events.size(); ++j) {
        if (events[j].type != Event::Done) continue;
        const auto& task  = events[j].task;
        const auto  found = last_done.find(task);
        if (found != last_done.end()) {
            bool lost = false;
            for (std::size_t c = found->second + 1; c < j && !lost; ++c) {
                if (events[c].type != Event::Crash) continue;
                bool durable = false;
                for (std::size_t d = found->second + 1; d < c; ++d) {
                    if (events[d].type == Event::Put && events[d].task == task) durable = true;
                    if (events[d].type == Event::Save && events[d].phase == "completed" &&
                        events[d].step == step_of(task))
                        durable = true;
                }
                lost = !durable;
            }
            if (!lost)
                verdict.problems.push_back("P1: " + task + " (" + events[j].node +
                                           ") completed again after its completion was durable");
        }
        last_done[task] = j;
    }
}

/// P2 on the journal: each Ask consumed the answer the schedule gave it, and
/// no Ask ran past its question with an answer to another one.
void check_answers(const Outcome& reference, const Observed& observed, Verdict& verdict) {
    for (const auto& event : observed.events) {
        if (event.type != Event::Done || event.consumed.empty()) continue;
        const auto expected = reference.consumed.find(event.task);
        if (expected != reference.consumed.end() && expected->second == event.consumed) continue;
        verdict.problems.push_back("P2: " + event.task + " consumed '" + event.consumed + "', expected " +
                                   (expected == reference.consumed.end() ? std::string("no answer")
                                                                         : "'" + expected->second + "'"));
    }
}

void compare(const Outcome& reference, const Observed& observed, Verdict& verdict) {
    check_answers(reference, observed, verdict);
    const auto& expected = reference.pauses;
    const auto& actual   = observed.pauses;
    for (std::size_t k = 0; k < std::max(expected.size(), actual.size()); ++k) {
        if (k < expected.size() && k < actual.size() && expected[k] == actual[k]) continue;
        std::ostringstream out;
        out << "P2: pause " << k << " reference " << (k < expected.size() ? describe(expected[k]) : "none")
            << ", engine " << (k < actual.size() ? describe(actual[k]) : "none");
        if (k < expected.size() && k < actual.size() && describe(expected[k]) == describe(actual[k]))
            out << " (payloads differ)";
        verdict.problems.push_back(out.str());
        break;
    }
    if (reference.status != observed.status || !observed.error.empty())
        verdict.problems.push_back(std::string("P3: status reference ") + to_string(reference.status) +
                                   ", engine " + to_string(observed.status) +
                                   (observed.error.empty() ? "" : " (" + observed.error + ")"));
    if (observed.status != Status::Failed && reference.status == observed.status) {
        if (reference.log.dump() != observed.log.dump())
            verdict.problems.push_back("P3: log reference " + reference.log.dump() + ", engine " +
                                       observed.log.dump());
        if (reference.failures.dump() != observed.failures.dump())
            verdict.problems.push_back("P3: failures reference " + reference.failures.dump() + ", engine " +
                                       observed.failures.dump());
        for (const auto& [node, label] : observed.routes) {
            const auto found = reference.routes.find(node);
            const auto want  = found == reference.routes.end() ? std::string("a") : found->second;
            if (want != label)
                verdict.problems.push_back("P3: route of " + node + " reference " + want + ", engine " + label);
        }
    }
    check_exactly_once(observed, verdict);
}

// ---------------------------------------------------------------------------
// Exploration and statistics
// ---------------------------------------------------------------------------

/// An example, with the enumeration index of its graph: the merged report
/// keeps the one of the smallest graph, whichever worker found it.
using Example = std::pair<std::uint64_t, std::string>;

void keep_first(std::map<std::string, Example>& examples, const std::string& key, Example example) {
    const auto found = examples.find(key);
    if (found == examples.end() || example.first < found->second.first) examples[key] = std::move(example);
}

struct Totals {
    std::uint64_t graphs = 0, invalid = 0, schedules = 0, runs = 0;
    std::uint64_t crash_runs = 0, fork_runs = 0, sqlite_runs = 0, pauses = 0, answers = 0;
    std::uint64_t max_ops = 0;
    std::uint64_t                        mismatches = 0;
    std::map<std::string, std::uint64_t> signatures;  // kind of mismatch -> runs
    std::map<std::string, Example>       failures;    // kind of mismatch -> first example
    std::uint64_t                        unaddressed = 0;  // runs in which a value reached a node it was not for

    void fail(const std::string& signature, std::uint64_t graph, std::string message) {
        ++mismatches;
        ++signatures[signature];
        keep_first(failures, signature, Example{graph, std::move(message)});
    }

    void merge(const Totals& other) {
        graphs += other.graphs;
        invalid += other.invalid;
        schedules += other.schedules;
        runs += other.runs;
        crash_runs += other.crash_runs;
        fork_runs += other.fork_runs;
        sqlite_runs += other.sqlite_runs;
        pauses += other.pauses;
        answers += other.answers;
        max_ops = std::max(max_ops, other.max_ops);
        mismatches += other.mismatches;
        for (const auto& [key, count] : other.signatures) signatures[key] += count;
        for (const auto& [key, example] : other.failures) keep_first(failures, key, example);
        unaddressed += other.unaddressed;
    }
};

struct Context {
    const Graph&       graph;
    std::uint64_t      index;  // the graph's position in the enumeration
    const Bounds&      limits;
    Totals&            totals;
    bool               addressed;
    Engines&           engines;
    std::shared_ptr<CheckpointStore> sqlite;  // one database per graph, a thread per run
    std::uint64_t      next_thread = 0;
};

/// A short name for a kind of mismatch: its first problem, how the run got
/// there (crash, fork) and, for an engine error, the error's gist.
std::string signature_of(const Verdict& verdict, const Schedule& schedule, const Observed& observed) {
    const auto& text  = verdict.problems.front();
    const auto  colon = text.find(':');
    std::string kind  = text.substr(0, colon == std::string::npos ? text.size() : colon) + " " +
                       (text.find("completed again") != std::string::npos ? "re-executed"
                        : text.find("status") != std::string::npos        ? "status"
                        : text.find("pause") != std::string::npos         ? "pauses"
                        : text.find("consumed") != std::string::npos      ? "answer"
                                                                           : "state");
    if (!observed.error.empty()) {
        const auto& error = observed.error;
        kind += error.find("is not pending") != std::string::npos           ? " (interrupt not pending)"
                : error.find("not a NodeInterrupt pause") != std::string::npos ? " (not a NodeInterrupt pause)"
                                                                               : " (" + error.substr(0, 40) + ")";
    }
    if (!schedule.crashes.empty()) kind += " after a crash";
    if (schedule.fork_at >= 0) kind += " on a fork";
    return kind;
}

void judge(Context& context, const Outcome& reference, const Schedule& schedule, const Observed& observed,
           const char* store) {
    ++context.totals.runs;
    Verdict verdict;
    compare(reference, observed, verdict);
    if (verdict.problems.empty()) return;
    std::ostringstream out;
    out << "resume model mismatch (" << store << ", " << (context.addressed ? "addressed" : "naive")
        << " asks)\n  graph " << describe(context.graph) << "\n  json " << definition(context.graph, context.addressed).dump()
        << "\n  schedule " << describe(schedule) << " (" << observed.crashes << " crash(es), " << observed.ops
        << " durable ops)\n  reference " << to_string(reference.status) << " pauses";
    for (const auto& pause : reference.pauses) out << " " << describe(pause);
    out << " log " << reference.log.dump() << "\n  engine    " << to_string(observed.status) << " pauses";
    for (const auto& pause : observed.pauses) out << " " << describe(pause);
    out << " log " << observed.log.dump();
    for (const auto& problem : verdict.problems) out << "\n  " << problem;
    context.totals.fail(signature_of(verdict, schedule, observed) + ", " + store, context.index, out.str());
}

std::shared_ptr<CheckpointStore> fresh_memory() { return std::make_shared<InMemoryCheckpointStore>(); }

void run_variants(Context& context, const Outcome& reference, const std::vector<Answer>& answers) {
    auto& totals = context.totals;
    ++totals.schedules;
    totals.pauses += reference.pauses.size();
    totals.answers += answers.size();
    const auto thread = [&] { return "t" + std::to_string(++context.next_thread); };

    // Crash-free, on both stores.
    Schedule plain{answers, {}, -1};
    const auto base = run_schedule(context.graph, context.engines, context.addressed, plain, fresh_memory(), thread());
    totals.max_ops = std::max<std::uint64_t>(totals.max_ops, static_cast<std::uint64_t>(base.ops));
    // An addressed ask that received a value addressed elsewhere: a gate's
    // approval reaches the node the gate held (#393).
    if (std::any_of(base.events.begin(), base.events.end(),
                    [](const Event& event) { return event.type == Event::Unaddressed; }))
        ++totals.unaddressed;
    judge(context, reference, plain, base, "memory");
    if (context.sqlite) {
        ++totals.sqlite_runs;
        judge(context, reference, plain,
              run_schedule(context.graph, context.engines, context.addressed, plain, context.sqlite, thread()), "sqlite");
    }

    // A crash before and after every durable operation; with two crashes, a
    // second one at every later operation.
    const auto crash_points = [&](int ops) {
        std::vector<std::pair<int, bool>> points;
        for (int op = 0; op < ops; ++op) {
            points.push_back({op, false});
            if (context.limits.crash_after_every_op || op + 1 == ops) points.push_back({op, true});
        }
        return points;
    };
    for (const auto& [op, after] : crash_points(base.ops)) {
        {
            Schedule crash{answers, {{op, after}}, -1};
            const auto observed =
                run_schedule(context.graph, context.engines, context.addressed, crash, fresh_memory(), thread());
            ++totals.crash_runs;
            judge(context, reference, crash, observed, "memory");
            if (context.limits.max_crashes < 2) continue;
            for (const auto& [second, later] : crash_points(observed.ops)) {
                if (second <= op || second > op + context.limits.second_crash_window) continue;
                {
                    Schedule twice{answers, {{op, after}, {second, later}}, -1};
                    ++totals.crash_runs;
                    judge(context, reference, twice,
                          run_schedule(context.graph, context.engines, context.addressed, twice, fresh_memory(),
                                       thread()),
                          "memory");
                }
            }
        }
    }
    if (context.sqlite && context.graph.n <= context.limits.sqlite_crash_max_nodes) {
        // SQLite: crash at every operation, once.
        for (const auto& [op, after] : crash_points(base.ops))
            {
                Schedule crash{answers, {{op, after}}, -1};
                ++totals.crash_runs;
                ++totals.sqlite_runs;
                judge(context, reference, crash,
                      run_schedule(context.graph, context.engines, context.addressed, crash, context.sqlite, thread()),
                      "sqlite");
            }
    }

    // A fork of every pause that is answered, plain and with a crash at every
    // durable operation from the fork on.
    for (std::size_t k = 0; k < answers.size() && k < reference.pauses.size(); ++k) {
        Schedule forked{answers, {}, static_cast<int>(k)};
        const auto observed =
            run_schedule(context.graph, context.engines, context.addressed, forked, fresh_memory(), thread());
        ++totals.fork_runs;
        judge(context, reference, forked, observed, "memory");
    }
}

/// Every answer schedule of the reference, depth-first: at each pause, each
/// pending question by id and by default, with each label a conditional
/// Ask routes on.
void explore(Context& context) {
    std::vector<Answer> answers;
    std::function<void(const Reference&)> walk = [&](const Reference& run) {
        if (!run.paused() || static_cast<int>(answers.size()) >= context.limits.max_answers) {
            run_variants(context, run.outcome(), answers);
            return;
        }
        const auto& pending = run.pending();
        for (int index = -1; index < static_cast<int>(pending.size()); ++index) {
            if (pending.front().gate && index >= 0) continue;
            const auto& question = pending[index < 0 ? 0 : static_cast<std::size_t>(index)];
            std::vector<std::string> labels{"a"};
            if (!question.gate) {
                for (int u = 0; u < context.graph.n; ++u)
                    if (name_of(context.graph, u) == question.node && context.graph.nodes[u].conditional)
                        labels.push_back("b");
            }
            for (const auto& label : labels) {
                Reference next = run;
                answers.push_back(Answer{index, label});
                next.resume(answers.back(), static_cast<int>(answers.size()));
                walk(next);
                answers.pop_back();
            }
        }
    };
    Reference start(context.graph, context.addressed);
    start.start();
    walk(start);
}

void report(const char* title, const Bounds& limits, const Totals& totals, double seconds) {
    std::cout << "[ resume model ] " << title << " (bound " << limits.name << ": main nodes<=" << limits.max_nodes
              << ", special<=" << limits.max_special << " (" << limits.max_special_largest
              << " at the largest size), asks<=" << limits.max_asks << ", sends<="
              << limits.max_sends << ", answers<=" << limits.max_answers << ", crashes<=" << limits.max_crashes
              << ")\n  graphs " << totals.graphs << " (validator rejected " << totals.invalid << "), answer schedules "
              << totals.schedules << " (pauses " << totals.pauses << ", answers " << totals.answers << ")\n  runs "
              << totals.runs << ": crash " << totals.crash_runs << ", fork " << totals.fork_runs << ", sqlite "
              << totals.sqlite_runs << "; most durable ops in one run " << totals.max_ops << "\n";
    if (totals.unaddressed)
        std::cout << "  crash-free schedules in which an addressed ask received a gate's approval: "
                  << totals.unaddressed << "\n";
    std::cout << "  mismatches " << totals.mismatches << ", " << seconds << " s\n";
    for (const auto& [signature, count] : totals.signatures)
        std::cout << "    " << signature << ": " << count << " runs\n";
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/// Every graph of the family, spread over NEOGRAPH_RESUME_MODEL_THREADS
/// workers (default 4): worker w takes the graphs whose index is w modulo
/// the worker count. Each has its own journal, engines and stores.
void run_family(bool addressed, Totals& totals, const Bounds& limits) {
    register_model();
    const auto workers = static_cast<std::size_t>(std::max(1, env_int("NEOGRAPH_RESUME_MODEL_THREADS", 4)));
    std::vector<Totals>        partial(workers);
    std::vector<std::thread>   threads;
    std::vector<std::exception_ptr> errors(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        threads.emplace_back([&, w] {
            Journal own;
            current_journal = &own;
            try {
                std::uint64_t index = 0;
                for_each_graph(limits, [&](const Graph& graph) {
                    if (index++ % workers != w) return;
                    auto& mine = partial[w];
                    ++mine.graphs;
                    auto engines = make_engines(graph, addressed);
                    if (!engines) {
                        ++mine.invalid;
                        return;
                    }
                    std::shared_ptr<CheckpointStore> sqlite;
                    // Every save carries one timestamp, as on a host whose saves return
                    // inside the millisecond: the store must order saves by itself.
                    if (limits.sqlite)
                        sqlite = std::make_shared<test_support::CoarseClockStore<SqliteCheckpointStore>>(
                            ":memory:");
                    Context context{graph, index - 1, limits, mine, addressed, *engines, sqlite};
                    explore(context);
                });
            } catch (...) {
                errors[w] = std::current_exception();
            }
            current_journal = nullptr;
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors)
        if (error) std::rethrow_exception(error);
    for (const auto& part : partial) totals.merge(part);
}

}  // namespace

TEST(ResumeModel, ReferenceStatesTheContract) {
    // n0 fans out to two Asks that join at n3.
    Graph graph;
    graph.n     = 4;
    graph.nodes = {Node{Kind::Ok, {1, 2}}, Node{Kind::Ask, {3}}, Node{Kind::Ask, {3}}, Node{Kind::Ok, {}}};
    graph.join  = 3;
    Reference run(graph);
    ASSERT_TRUE(run.start());
    ASSERT_EQ(run.pending().size(), 2U);
    EXPECT_EQ(run.pending()[0].id, "s1:n1");
    EXPECT_EQ(run.pending()[1].id, "s1:n2");
    // Answer the second first: the first asks again.
    ASSERT_TRUE(run.resume(Answer{1, "a"}, 1));
    ASSERT_EQ(run.pending().size(), 1U);
    EXPECT_EQ(run.pending()[0].id, "s1:n1");
    ASSERT_FALSE(run.resume(Answer{-1, "a"}, 2));
    EXPECT_EQ(run.outcome().status, Status::Completed);
    EXPECT_EQ(run.outcome().log.dump(),
              json::array({json::array({0, "n0", "ok"}), json::array({1, "n1", "a2"}), json::array({1, "n2", "a1"}),
                           json::array({2, "n3", "ok"})})
                  .dump());

    // A Map whose two Sends ask: the Sends pause, the Map is replayed.
    Graph sends;
    sends.n     = 1;
    sends.nodes = {Node{Kind::Map, {}}};
    sends.work  = {Work::Ask, Work::Ok};
    Reference mapped(sends);
    ASSERT_TRUE(mapped.start());
    ASSERT_EQ(mapped.pending().size(), 1U);
    EXPECT_EQ(mapped.pending()[0].id, send_id(0, 0, 0));
    ASSERT_FALSE(mapped.resume(Answer{-1, "a"}, 1));
    EXPECT_EQ(mapped.outcome().log.dump(),
              json::array({json::array({0, "n0", "map"}), json::array({0, "w", 0, "a1"}),
                           json::array({0, "w", 1, "ok"})})
                  .dump());

    // #392: a failure outranks an ordinary interrupt of its step.
    Graph failing;
    failing.n     = 3;
    failing.nodes = {Node{Kind::Ok, {1, 2}}, Node{Kind::Ask, {}}, Node{Kind::Fail, {}}};
    Reference failed(failing);
    EXPECT_FALSE(failed.start());
    EXPECT_EQ(failed.outcome().status, Status::Failed);
    EXPECT_TRUE(failed.outcome().pauses.empty());

    // #393, #396: two gated Asks in one step, each gate answered on its own.
    // A plain Ask takes its gate's approval as its answer; an addressed one
    // asks its own question.
    Graph gated;
    gated.n     = 3;
    gated.nodes = {Node{Kind::Ok, {1, 2}}, Node{Kind::Ask, {}}, Node{Kind::Ask, {}}};
    gated.nodes[1].gate = gated.nodes[2].gate = true;
    Reference plain(gated, false);
    ASSERT_TRUE(plain.start());
    EXPECT_TRUE(plain.pending()[0].gate);
    EXPECT_EQ(plain.pending()[0].node, "n1");
    ASSERT_TRUE(plain.resume(Answer{}, 1));
    EXPECT_EQ(plain.pending()[0].node, "n2");
    ASSERT_FALSE(plain.resume(Answer{}, 2));
    EXPECT_EQ(plain.outcome().log.dump(),
              json::array({json::array({0, "n0", "ok"}), json::array({1, "n1", "a1"}),
                           json::array({1, "n2", "a2"})})
                  .dump());
    Reference addressed(gated, true);
    ASSERT_TRUE(addressed.start());
    ASSERT_TRUE(addressed.resume(Answer{}, 1));
    ASSERT_TRUE(addressed.resume(Answer{}, 2));
    ASSERT_EQ(addressed.pending().size(), 2U) << "both Asks ask once their gates are answered";
    EXPECT_EQ(addressed.pending()[0].id, "s1:n1");
}

TEST(ResumeModel, EngineMatchesTheReferenceWithAddressedAnswers) {
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    run_family(true, totals, limits);
    report("addressed asks: P1-P5, every answer order, crash point and fork", limits, totals, seconds_since(start));
    EXPECT_GT(totals.graphs, 100U);
    EXPECT_EQ(totals.mismatches, 0U);
    for (const auto& [signature, failure] : totals.failures) ADD_FAILURE() << failure.second;
}

// The same family with asks that take any resume value as their answer, as
// an approval node usually does. A value that reaches a node it was not for
// shows as that node running past its question (#393), and a gated Ask takes
// its gate's approval as its answer.
TEST(ResumeModel, EngineMatchesTheReferenceWithPlainAnswers) {
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    Totals     totals;
    run_family(false, totals, limits);
    report("plain asks: P1-P5, every answer order, crash point and fork", limits, totals, seconds_since(start));
    EXPECT_GT(totals.graphs, 100U);
    EXPECT_EQ(totals.mismatches, 0U);
    for (const auto& [signature, failure] : totals.failures) ADD_FAILURE() << failure.second;
}

// The size of the family and of its answer schedules at the selected bound,
// from the reference alone (no engine runs): for choosing a bound.
TEST(ResumeModel, DISABLED_FamilySize) {
    const auto limits = bounds();
    std::map<int, std::uint64_t> graphs, schedules;
    for_each_graph(limits, [&](const Graph& graph) {
        ++graphs[graph.n];
        std::function<void(const Reference&, int)> walk = [&](const Reference& run, int answered) {
            if (!run.paused() || answered >= limits.max_answers) {
                ++schedules[graph.n];
                return;
            }
            const auto& pending = run.pending();
            for (int index = -1; index < static_cast<int>(pending.size()); ++index) {
                if (pending.front().gate && index >= 0) continue;
                Reference next = run;
                next.resume(Answer{index, "a"}, answered + 1);
                walk(next, answered + 1);
            }
        };
        Reference start(graph);
        start.start();
        walk(start, 0);
    });
    for (const auto& [n, count] : graphs)
        std::cout << "  nodes " << n << ": graphs " << count << ", answer schedules " << schedules[n] << "\n";
}
