// The bounded-exhaustive resume/replay model: the graph family, its
// enumeration and the reference interpreter of the documented interrupt,
// resume and replay contract. tests/test_resume_model.cpp drives the real
// GraphEngine against it through every answer order, a crash at every durable
// point and a fork of every pause. NEOGRAPH_RESUME_MODEL_BOUND=extended selects
// the larger bound; the other NEOGRAPH_RESUME_MODEL_* variables override
// single limits (tests/model/README.md lists them).
#pragma once

#include <neograph/json.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace resume_model {

using neograph::json;

inline constexpr int kEnd = -1;

// ---------------------------------------------------------------------------
// Graphs
// ---------------------------------------------------------------------------

/// What a node's body does.
///  - Ok:    appends [step, node, "ok"] to "log".
///  - Ask:   raises NodeInterrupt until its invocation is answered, then appends
///           [step, node, answer] (and writes its route label when conditional).
///  - Fail:  throws an ordinary error: along its failure edge when it has one
///           (#364), otherwise the run fails.
///  - Flaky: attempt 0 throws, attempt 1 (a node retry) completes like Ok.
///  - Map:   appends [step, node, "map"] and emits `sends` Sends to the worker.
enum class Kind { Ok, Ask, Fail, Flaky, Map };

/// What the worker (the Send target "w") does for Send item i.
enum class Work { Ok, Ask, Fail };

inline const char* to_string(Kind kind) {
    switch (kind) {
        case Kind::Ok: return "ok";
        case Kind::Ask: return "ask";
        case Kind::Fail: return "fail";
        case Kind::Flaky: return "flaky";
        case Kind::Map: return "map";
    }
    return "?";
}
inline const char* to_string(Work work) {
    switch (work) {
        case Work::Ok: return "ok";
        case Work::Ask: return "ask";
        case Work::Fail: return "fail";
    }
    return "?";
}

struct Node {
    Kind             kind = Kind::Ok;
    std::vector<int> targets;            // static successors (kEnd: an explicit __end__); none: implicit end
    bool             conditional = false;  // Ask: routes on its answer, targets = {label a, label b}
    int              fail_target = -1;     // Fail: its failure edge's target; -1 has none
    bool             gate        = false;  // interrupt_before
    bool             gate_after  = false;  // interrupt_after
};

/// Main nodes n0..n{n-1} (n0 the entry) form a DAG of forward edges. A graph
/// with a Map node also has the worker "w", the target of its Sends, whose
/// item i behaves as work[i] and which routes to `worker_target` (or ends).
/// `join` is a barrier node that waits for every node with a static edge to it.
struct Graph {
    int               n = 0;
    std::vector<Node> nodes;
    int               join = -1;
    std::vector<Work> work;               // one per Send; empty without a Map node
    int               worker_target = -1;  // -1: the worker ends implicitly
    bool              has_worker() const { return !work.empty(); }
    int               worker() const { return n; }
};

inline std::string name_of(const Graph& graph, int index) {
    return index == graph.worker() ? std::string("w") : "n" + std::to_string(index);
}
inline std::string target_name(const Graph& graph, int target) {
    return target == kEnd ? std::string("__end__") : name_of(graph, target);
}

/// Every node with a static (or worker) edge to `target`, by index.
inline std::vector<int> static_predecessors(const Graph& graph, int target) {
    std::vector<int> out;
    for (int u = 0; u < graph.n; ++u) {
        const auto& node = graph.nodes[u];
        if (node.conditional) continue;
        if (std::find(node.targets.begin(), node.targets.end(), target) != node.targets.end())
            out.push_back(u);
    }
    if (graph.has_worker() && graph.worker_target == target) out.push_back(graph.worker());
    return out;
}

/// Node config the test's node factory reads. `addressed` asks accept only an
/// answer addressed to their own invocation id.
inline json node_config(const Graph& graph, int index, bool addressed) {
    json config = json::object();
    config["type"] = "resume_model.node";
    if (index == graph.worker()) {
        json work = json::array();
        for (const auto item : graph.work) work.push_back(to_string(item));
        config["kind"] = "work";
        config["work"] = std::move(work);
    } else {
        const auto& node = graph.nodes[index];
        config["kind"] = to_string(node.kind);
        if (node.kind == Kind::Map) config["sends"] = static_cast<int>(graph.work.size());
        if (node.conditional) config["conditional"] = true;
    }
    config["addressed"] = addressed;
    return config;
}

/// The Core JSON of a graph.
inline json definition(const Graph& graph, bool addressed) {
    json channels = json::object();
    channels["log"]  = json{{"reducer", "append"}, {"initial", json::array()}};
    channels["item"] = json{{"reducer", "overwrite"}, {"initial", -1}};
    json nodes       = json::object();
    json edges       = json::array({json{{"from", "__start__"}, {"to", "n0"}}});
    json conditions  = json::array();
    json failures    = json::array();
    json gates       = json::array();
    json after_gates = json::array();
    const int count  = graph.n + (graph.has_worker() ? 1 : 0);
    for (int u = 0; u < count; ++u) {
        json node = node_config(graph, u, addressed);
        if (u == graph.join) {
            json wait_for = json::array();
            for (const auto from : static_predecessors(graph, u)) wait_for.push_back(name_of(graph, from));
            node["barrier"] = json{{"wait_for", std::move(wait_for)}};
        }
        nodes[name_of(graph, u)] = std::move(node);
        if (u == graph.worker()) {
            if (graph.worker_target >= 0)
                edges.push_back(json{{"from", "w"}, {"to", target_name(graph, graph.worker_target)}});
            continue;
        }
        const auto& spec = graph.nodes[u];
        if (spec.gate) gates.push_back(name_of(graph, u));
        if (spec.gate_after) after_gates.push_back(name_of(graph, u));
        if (spec.conditional) {
            channels["route_" + name_of(graph, u)] = json{{"reducer", "overwrite"}, {"initial", "a"}};
            conditions.push_back(json{{"from", name_of(graph, u)},
                                      {"condition", "resume_model.route." + name_of(graph, u)},
                                      {"routes", json{{"a", target_name(graph, spec.targets[0])},
                                                      {"b", target_name(graph, spec.targets[1])}}}});
        } else {
            for (const auto target : spec.targets)
                edges.push_back(json{{"from", name_of(graph, u)}, {"to", target_name(graph, target)}});
        }
        if (spec.kind == Kind::Fail && spec.fail_target >= 0)
            failures.push_back(json{{"from", name_of(graph, u)},
                                    {"to", name_of(graph, spec.fail_target)},
                                    {"error_channel", "failures"}});
    }
    if (!failures.empty()) channels["failures"] = json{{"reducer", "append"}, {"initial", json::array()}};
    json result{{"name", "resume_model"},
                {"channels", std::move(channels)},
                {"nodes", std::move(nodes)},
                {"edges", std::move(edges)},
                {"conditional_edges", std::move(conditions)}};
    if (!failures.empty()) result["failure_edges"] = std::move(failures);
    if (!gates.empty()) result["interrupt_before"] = std::move(gates);
    if (!after_gates.empty()) result["interrupt_after"] = std::move(after_gates);
    return result;
}

inline std::string describe(const Graph& graph) {
    std::ostringstream out;
    for (int u = 0; u < graph.n; ++u) {
        const auto& node = graph.nodes[u];
        out << (u ? " " : "") << name_of(graph, u) << ":" << to_string(node.kind);
        if (node.gate) out << "+gate";
        if (node.gate_after) out << "+after";
        if (node.conditional) out << "?";
        out << "->";
        if (node.targets.empty()) out << "()";
        for (std::size_t i = 0; i < node.targets.size(); ++i)
            out << (i ? (node.conditional ? "|" : ",") : "") << target_name(graph, node.targets[i]);
        if (node.fail_target >= 0) out << " !->" << name_of(graph, node.fail_target);
        if (u == graph.join) out << " [join]";
    }
    if (graph.has_worker()) {
        out << " w[";
        for (std::size_t i = 0; i < graph.work.size(); ++i) out << (i ? "," : "") << to_string(graph.work[i]);
        out << "]->" << (graph.worker_target >= 0 ? name_of(graph, graph.worker_target) : "()");
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Invocation ids (the executor's task ids)
// ---------------------------------------------------------------------------

inline std::string fnv1a_hex(const std::string& text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer, 16);
}
inline json send_input(int item) { return json{{"item", item}}; }
inline std::string static_id(int step, const std::string& node) {
    return "s" + std::to_string(step) + ":" + node;
}
inline std::string send_id(int step, int index, int item) {
    return "s" + std::to_string(step) + ":send[" + std::to_string(index) + "]:w:" +
           fnv1a_hex(send_input(item).dump());
}

// ---------------------------------------------------------------------------
// Questions and answers
// ---------------------------------------------------------------------------

/// One pending question as the host sees it: a NodeInterrupt (its id, node
/// and payload) or a static gate (no id, `gate`, payload "before" for
/// interrupt_before or "after" for interrupt_after).
struct Question {
    std::string id;
    std::string node;
    json        payload;
    bool        gate = false;
    bool operator==(const Question& other) const {
        return id == other.id && node == other.node && gate == other.gate &&
               payload.dump() == other.payload.dump();
    }
};

inline std::string describe(const std::vector<Question>& pause) {
    std::string out = "[";
    for (std::size_t i = 0; i < pause.size(); ++i) {
        out += (i ? ", " : "");
        out += pause[i].gate ? pause[i].payload.get<std::string>() + " " + pause[i].node : pause[i].id;
    }
    return out + "]";
}

/// How the host answers a pause: the pending question it names (`index` into
/// the pause, or -1 for none, which answers the first), and the label of the
/// answer ("a" or "b"; a conditional Ask routes on it). The answer's value is
/// "<label><sequence>", sequence counting the schedule's answers from 1.
struct Answer {
    int         index = -1;
    std::string label = "a";
};

inline std::string answer_tag(const Answer& answer, int sequence) {
    return answer.label + std::to_string(sequence);
}

// ---------------------------------------------------------------------------
// Reference interpreter
// ---------------------------------------------------------------------------

enum class Status { Completed, Interrupted, Failed, Stalled, StepLimit };

inline const char* to_string(Status status) {
    switch (status) {
        case Status::Completed: return "Completed";
        case Status::Interrupted: return "Interrupted";
        case Status::Failed: return "Failed";
        case Status::Stalled: return "BarrierStalled";
        case Status::StepLimit: return "StepLimit";
    }
    return "?";
}

/// The documented contract, as one BSP machine:
///
///  1. A super-step runs its ready nodes (in name order), then the Sends they
///     emitted (in ready order, then emission order). Every Send runs on its
///     own copy of the state with its payload (#389). Writes fold in that
///     order once every invocation of the step has finished.
///  2. Before a step that is not re-entered by a resume, a ready node with
///     interrupt_before pauses the run (the first such node in ready order).
///     Only resuming that pause runs it (#382), and its answer reaches that
///     node and no other (#393). After the step, a ready node with
///     interrupt_after pauses the run; resuming it goes on to the next step,
///     whose gates still ask, and its answer reaches no node.
///  3. A step whose ready nodes raise NodeInterrupt pauses on all of them, in
///     ready order; a step whose Sends raise it (all ready nodes having
///     finished) pauses on those Sends (#385, #388). What finished is kept.
///  4. A resume re-enters the paused step. Finished invocations replay and do
///     not run again. The answer reaches exactly the invocation it names (or
///     the first pending one) in that step; every other invocation runs
///     without it, so a paused one asks again (#388). No node of a later step
///     sees it (#393).
///  5. A failing node with a failure edge routes to its target only (#364);
///     without one the run fails, also when a sibling of its step asked:
///     a failure outranks an ordinary interrupt (#392).
///  6. Barrier joins, implicit and explicit ends, and stalls follow the join
///     model (tests/join_model.h, #357).
struct Outcome {
    Status                             status = Status::Completed;
    std::vector<std::vector<Question>> pauses;
    json                               log      = json::array();
    json                               failures = json::array();  // [node, step, routed_to]
    std::map<std::string, std::string> routes;                    // conditional node -> label
    /// Every invocation that completed, by id, in commit order.
    std::vector<std::string> completed;
    /// The answer each answered Ask invocation consumed.
    std::map<std::string, std::string> consumed;
};

class Reference {
public:
    /// `addressed`: an Ask takes only an answer addressed to its own
    /// invocation, so a gate's approval (addressed to the gate) is not its
    /// answer; a plain Ask takes any resume value it receives.
    explicit Reference(const Graph& graph, bool addressed = true, int max_steps = 20)
        : graph_(&graph), addressed_(addressed), max_steps_(max_steps) {
        ready_ = {0};
    }

    /// Runs until the next pause or the end. Returns true when paused.
    bool start() { return advance(); }

    const std::vector<Question>& pending() const { return pending_; }
    bool                         paused() const { return paused_; }
    const Outcome&               outcome() const { return outcome_; }

    /// Answers the current pause and runs on. Returns true when paused again.
    bool resume(const Answer& answer, int sequence) {
        if (!paused_) throw std::logic_error("resume without a pause");
        paused_ = false;
        // #382: resuming a NodeInterrupt or an interrupt_before pause re-enters
        // its step past the gates; resuming an interrupt_after pause goes on
        // to the next step, whose gates still ask.
        resume_entry_ = !after_pause_;
        after_pause_  = false;
        const auto tag = answer_tag(answer, sequence);
        if (gate_pause_) {
            // An interrupt_before approval reaches the node it held (#393),
            // once every gate of the step is answered (#396); an
            // interrupt_after approval reaches no node.
            if (resume_entry_) {
                const auto node = pending_.front().node;
                gates_answered_.insert(node);
                if (!addressed_) answers_[static_id(step_, node)] = {tag, answer.label};
            }
            // The step's other gates still ask before it runs.
            resume_entry_ = false;
        } else {
            const auto index = answer.index < 0 ? 0 : answer.index;
            answers_.clear();
            answers_[pending_.at(static_cast<std::size_t>(index)).id] = {tag, answer.label};
        }
        gate_pause_ = false;
        pending_.clear();
        return advance();
    }

private:
    struct Result {
        enum Kind { Done, Interrupt, Handled, Unhandled } kind = Done;
        json              entry;  // log entry, null for none
        std::string       label;  // a conditional Ask's route label
        int               sends = 0;
        json              payload;  // an interrupt's payload
    };

    bool advance() {
        if (paused_) return true;
        while (true) {
            if (ready_.empty()) {
                finish_quiescent();
                return false;
            }
            if (step_ >= max_steps_) {
                outcome_.status = Status::StepLimit;
                return false;
            }
            if (!resume_entry_) {
                for (const auto u : ready_) {
                    if (u != graph_->worker() && graph_->nodes[u].gate &&
                        !gates_answered_.count(name_of(*graph_, u))) {
                        pending_    = {Question{"", name_of(*graph_, u), json("before"), true}};
                        paused_     = true;
                        gate_pause_ = true;
                        outcome_.status = Status::Interrupted;
                        outcome_.pauses.push_back(pending_);
                        return true;
                    }
                }
            }
            resume_entry_ = false;
            if (!run_step()) return paused_;
        }
    }

    Result execute(int node, const std::string& id, int item) {
        Result result;
        const auto name = name_of(*graph_, node);
        const auto answer   = answers_.find(id);
        const bool answered = answer != answers_.end();
        const auto ask = [&](json payload) {
            if (answered) {
                const auto& [tag, label] = answer->second;
                result.kind  = Result::Done;
                result.entry = item < 0 ? json::array({step_, name, tag}) : json::array({step_, name, item, tag});
                result.label = label;
                outcome_.consumed[id] = tag;
            } else {
                result.kind    = Result::Interrupt;
                result.payload = std::move(payload);
            }
        };
        if (node == graph_->worker()) {
            switch (graph_->work.at(static_cast<std::size_t>(item))) {
                case Work::Ok:
                    result.entry = json::array({step_, name, item, "ok"});
                    break;
                case Work::Ask:
                    ask(json{{"node", name}, {"step", step_}, {"item", item}});
                    break;
                case Work::Fail:
                    result.kind = Result::Unhandled;
                    break;
            }
            return result;
        }
        const auto& spec = graph_->nodes[node];
        switch (spec.kind) {
            case Kind::Ok:
            case Kind::Flaky:
                result.entry = json::array({step_, name, "ok"});
                break;
            case Kind::Ask:
                ask(json{{"node", name}, {"step", step_}});
                break;
            case Kind::Fail:
                result.kind = spec.fail_target >= 0 ? Result::Handled : Result::Unhandled;
                break;
            case Kind::Map:
                result.entry = json::array({step_, name, "map"});
                result.sends = static_cast<int>(graph_->work.size());
                break;
        }
        return result;
    }

    /// One super-step. False when the run paused or ended.
    bool run_step() {
        struct Invocation {
            int         node;
            std::string id;
            int         item;
            Result      result;
        };
        std::vector<Invocation> batch;
        for (const auto u : ready_) batch.push_back(Invocation{u, static_id(step_, name_of(*graph_, u)), -1, {}});
        if (!run_batch(batch)) return false;
        std::vector<Invocation> sends;
        int index = 0;
        for (const auto& invocation : batch)
            for (int item = 0; item < invocation.result.sends; ++item, ++index)
                sends.push_back(Invocation{graph_->worker(), send_id(step_, index, item), item, {}});
        if (!sends.empty() && !run_batch(sends)) return false;

        // Fold in ready order, then Send order, and route.
        std::map<int, std::set<int>> signals;
        bool hit_explicit_end = false;
        const auto route = [&](int from, const std::vector<int>& targets, bool implicit) {
            if (targets.empty() && implicit) return;
            for (const auto target : targets) {
                if (target == kEnd)
                    hit_explicit_end = true;
                else
                    signals[target].insert(from);
            }
        };
        const auto fold = [&](const Invocation& invocation) {
            const auto& result = invocation.result;
            outcome_.completed.push_back(invocation.id);
            if (!result.entry.is_null()) outcome_.log.push_back(result.entry);
            if (invocation.node == graph_->worker()) {
                if (graph_->worker_target >= 0) route(invocation.node, {graph_->worker_target}, false);
                return;
            }
            const auto& spec = graph_->nodes[invocation.node];
            if (result.kind == Result::Handled) {
                outcome_.failures.push_back(json::array(
                    {name_of(*graph_, invocation.node), step_, name_of(*graph_, spec.fail_target)}));
                route(invocation.node, {spec.fail_target}, false);
                return;
            }
            if (spec.conditional) {
                outcome_.routes[name_of(*graph_, invocation.node)] = result.label;
                route(invocation.node, {spec.targets[result.label == "b" ? 1 : 0]}, false);
                return;
            }
            route(invocation.node, spec.targets, true);
        };
        for (const auto& invocation : batch) fold(invocation);
        for (const auto& invocation : sends) fold(invocation);

        std::vector<int> next;
        for (const auto& [target, from] : signals) {
            if (target != graph_->join) {
                next.push_back(target);
                continue;
            }
            barrier_.insert(from.begin(), from.end());
            const auto members = static_predecessors(*graph_, target);
            if (std::all_of(members.begin(), members.end(), [&](int m) { return barrier_.count(m) > 0; })) {
                next.push_back(target);
                barrier_.clear();
            }
        }
        std::sort(next.begin(), next.end(), [&](int a, int b) { return name_of(*graph_, a) < name_of(*graph_, b); });
        ready_            = std::move(next);
        last_explicit_end_ = hit_explicit_end;
        finished_.clear();
        answers_.clear();
        gates_answered_.clear();
        ++step_;
        // interrupt_after: the first ready node of the step that carries it
        // pauses the run once the step has routed.
        for (const auto& invocation : batch) {
            if (!graph_->nodes[invocation.node].gate_after) continue;
            pending_    = {Question{"", name_of(*graph_, invocation.node), json("after"), true}};
            paused_     = true;
            gate_pause_ = after_pause_ = true;
            outcome_.status = Status::Interrupted;
            outcome_.pauses.push_back(pending_);
            return false;
        }
        return true;
    }

    template <class Invocations>
    bool run_batch(Invocations& batch) {
        bool interrupted = false, failed = false;
        for (auto& invocation : batch) {
            const auto found = finished_.find(invocation.id);
            if (found != finished_.end()) {
                invocation.result = found->second;
                continue;
            }
            invocation.result = execute(invocation.node, invocation.id, invocation.item);
            interrupted = interrupted || invocation.result.kind == Result::Interrupt;
            failed      = failed || invocation.result.kind == Result::Unhandled;
        }
        // #392: a failure outranks the ordinary interrupts of its step.
        if (failed) {
            outcome_.status = Status::Failed;
            ready_.clear();
            return false;
        }
        if (interrupted) {
            pending_.clear();
            for (const auto& invocation : batch) {
                if (invocation.result.kind == Result::Interrupt)
                    pending_.push_back(Question{invocation.id, name_of(*graph_, invocation.node),
                                                invocation.result.payload, false});
                else
                    finished_[invocation.id] = invocation.result;
            }
            paused_         = true;
            outcome_.status = Status::Interrupted;
            outcome_.pauses.push_back(pending_);
            answers_.clear();
            return false;
        }
        for (const auto& invocation : batch) finished_[invocation.id] = invocation.result;
        return true;
    }

    void finish_quiescent() {
        outcome_.status = !barrier_.empty() && !last_explicit_end_ ? Status::Stalled : Status::Completed;
    }

    const Graph*                  graph_;
    bool                          addressed_;
    int                           max_steps_;
    int                           step_ = 0;
    std::vector<int>              ready_;
    std::set<int>                 barrier_;
    bool                          last_explicit_end_ = false;
    bool                          paused_            = false;
    bool                          gate_pause_        = false;
    bool                          after_pause_       = false;
    bool                          resume_entry_      = false;
    std::vector<Question>         pending_;
    std::map<std::string, Result> finished_;
    /// The answers the invocations of the current step receive, by id: the
    /// one a NodeInterrupt resume names, or each answered gate's node.
    std::map<std::string, std::pair<std::string, std::string>> answers_;  // id -> {tag, label}
    std::set<std::string>                                      gates_answered_;  // in the current step
    Outcome                       outcome_;
};

// ---------------------------------------------------------------------------
// Bounds and enumeration
// ---------------------------------------------------------------------------

struct Bounds {
    std::string name        = "ci";
    int         max_nodes   = 3;  // main nodes
    int         max_special = 2;  // nodes that are not plain Ok (a gate counts)
    /// The largest graphs (max_nodes main nodes) have at most this many.
    int         max_special_largest = 2;
    int         max_asks    = 2;
    int         max_sends   = 2;
    int         max_answers = 4;  // resumes per schedule
    int         max_crashes = 1;  // crashes per schedule
    bool        gates       = true;
    bool        sqlite      = true;  // also drive every schedule through SQLite
    /// Every item pattern of 1..max_sends Sends (Ok, Ask or Fail each), or a
    /// fixed list that covers one and two Sends, asks among them and #392.
    bool every_work_pattern = false;
    /// The worker routes to every later node, or only to the last one.
    bool every_worker_target = false;
    /// A Fail node's failure edge goes to every valid target, or the first.
    bool every_fail_target = false;
    /// A crash after every durable operation too. A crash after operation k
    /// leaves the durable state a crash before k+1 does, with fewer bodies
    /// run, so the CI bound crashes before every operation and after the last.
    bool crash_after_every_op = false;
    /// A gate on a node of every kind, or on Ok and Ask nodes only.
    bool gates_on_every_kind = false;
    /// Two interrupt_before gates in one graph too (#396: each gate of a step
    /// is answered on its own).
    bool two_gates = false;
    /// SQLite runs crash at every durable point on graphs up to this size;
    /// larger ones run on SQLite crash-free.
    int sqlite_crash_max_nodes = 2;
    /// With two crashes, the second one falls on one of this many durable
    /// operations after the first: a crash during the recovery.
    int second_crash_window = 4;
};

/// The item behaviours of a Map node's Sends the bound enumerates.
inline std::vector<std::vector<Work>> work_patterns(const Bounds& limits) {
    if (!limits.every_work_pattern)
        return {{Work::Ok}, {Work::Ask}, {Work::Ask, Work::Ok}, {Work::Ask, Work::Ask}, {Work::Ask, Work::Fail}};
    std::vector<std::vector<Work>> out;
    for (int sends = 1; sends <= limits.max_sends; ++sends) {
        std::vector<Work> work(static_cast<std::size_t>(sends), Work::Ok);
        std::function<void(int)> items = [&](int i) {
            if (i == sends) {
                out.push_back(work);
                return;
            }
            for (const auto w : {Work::Ok, Work::Ask, Work::Fail}) {
                work[static_cast<std::size_t>(i)] = w;
                items(i + 1);
            }
        };
        items(0);
    }
    return out;
}

inline int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::atoi(value) : fallback;
}

inline Bounds bounds() {
    Bounds result;
    const char* selected = std::getenv("NEOGRAPH_RESUME_MODEL_BOUND");
    if (selected && std::string(selected) == "extended") {
        result.name                 = "extended";
        result.max_nodes            = 4;
        result.max_special          = 3;
        result.max_special_largest  = 2;
        result.max_answers          = 6;
        result.max_crashes          = 2;
        result.every_work_pattern   = true;
        result.every_worker_target  = true;
        result.every_fail_target    = true;
        result.crash_after_every_op = true;
        result.gates_on_every_kind  = true;
        result.two_gates            = true;
        result.sqlite_crash_max_nodes = 3;
    }
    result.max_nodes   = std::min(env_int("NEOGRAPH_RESUME_MODEL_NODES", result.max_nodes), 6);
    result.max_special = env_int("NEOGRAPH_RESUME_MODEL_SPECIAL", result.max_special);
    result.max_special_largest =
        std::min(result.max_special, env_int("NEOGRAPH_RESUME_MODEL_SPECIAL_LARGEST", result.max_special_largest));
    result.max_answers = env_int("NEOGRAPH_RESUME_MODEL_ANSWERS", result.max_answers);
    result.max_crashes = env_int("NEOGRAPH_RESUME_MODEL_CRASHES", result.max_crashes);
    result.sqlite      = env_int("NEOGRAPH_RESUME_MODEL_SQLITE", result.sqlite ? 1 : 0) != 0;
    return result;
}

inline bool all_reachable(const Graph& graph) {
    std::set<int> seen{0};
    std::vector<int> frontier{0};
    while (!frontier.empty()) {
        const int u = frontier.back();
        frontier.pop_back();
        std::vector<int> next;
        if (u == graph.worker()) {
            if (graph.worker_target >= 0) next.push_back(graph.worker_target);
        } else {
            next = graph.nodes[u].targets;
            if (graph.nodes[u].fail_target >= 0) next.push_back(graph.nodes[u].fail_target);
            if (graph.nodes[u].kind == Kind::Map) next.push_back(graph.worker());
        }
        for (const auto v : next)
            if (v != kEnd && seen.insert(v).second) frontier.push_back(v);
    }
    return static_cast<int>(seen.size()) == graph.n + (graph.has_worker() ? 1 : 0);
}

/// Every graph of the family, smallest first:
///  - main nodes n0..n{N-1}, N <= max_nodes, each routing statically to at most
///    two later nodes (none: an implicit end), every node reachable;
///  - at most max_special nodes are not Ok: at most max_asks Ask nodes (an Ask
///    with two targets may route on its answer instead, its label "b" possibly
///    to __end__), at most one Fail (with or without a failure edge to a later
///    node that is not the join and does not end implicitly), one Flaky and
///    one Map, whose 1..max_sends Sends go to the worker, each item Ok, Ask or
///    Fail, the worker ending implicitly or routing to a later node;
///  - optionally the last node with two or more static predecessors is a
///    barrier join waiting for all of them;
///  - optionally one Ok or Ask node carries interrupt_before or interrupt_after
///    (one gate; a gate counts as a special node), or, extended, two nodes
///    carry interrupt_before.
template <class Fn>
inline void for_each_graph(const Bounds& limits, Fn&& visit) {
    for (int n = 1; n <= limits.max_nodes; ++n) {
        std::vector<std::vector<std::vector<int>>> options(static_cast<std::size_t>(n));
        for (int u = 0; u < n; ++u) {
            options[u].push_back({});
            for (int v = u + 1; v < n; ++v) {
                options[u].push_back({v});
                for (int w = v + 1; w < n; ++w) options[u].push_back({v, w});
            }
        }
        Graph graph;
        graph.n = n;
        graph.nodes.assign(static_cast<std::size_t>(n), Node{});
        const int max_special = n == limits.max_nodes ? limits.max_special_largest : limits.max_special;
        std::function<void(int)> topology = [&](int u) {
            if (u < n) {
                for (const auto& targets : options[u]) {
                    graph.nodes[u].targets = targets;
                    topology(u + 1);
                }
                return;
            }
            // Kinds, specials first.
            std::function<void(int, int, int, bool, bool, bool)> kinds =
                [&](int v, int special, int asks, bool fail, bool flaky, bool map) {
                    if (v == n) {
                        // Joins and gates on top of a complete assignment.
                        std::vector<int> joins{-1};
                        for (int j = n - 1; j > 0; --j) {
                            if (static_predecessors(graph, j).size() >= 2) {
                                joins.push_back(j);
                                break;
                            }
                        }
                        for (const auto join : joins) {
                            graph.join = join;
                            bool valid = all_reachable(graph);
                            for (int u = 0; u < n; ++u)
                                if (graph.nodes[u].fail_target >= 0 && graph.nodes[u].fail_target == join)
                                    valid = false;
                            if (!valid) continue;
                            visit(static_cast<const Graph&>(graph));
                            if (!limits.gates || special >= max_special) continue;
                            for (int gate = 0; gate < n; ++gate) {
                                // A gate pauses before or after its node runs, whatever the node does.
                                if (!limits.gates_on_every_kind && graph.nodes[gate].kind != Kind::Ok &&
                                    graph.nodes[gate].kind != Kind::Ask)
                                    continue;
                                graph.nodes[gate].gate = true;
                                visit(static_cast<const Graph&>(graph));
                                graph.nodes[gate].gate       = false;
                                graph.nodes[gate].gate_after = true;
                                visit(static_cast<const Graph&>(graph));
                                graph.nodes[gate].gate_after = false;
                            }
                            if (!limits.two_gates || special + 2 > max_special) continue;
                            for (int first = 0; first < n; ++first)
                                for (int second = first + 1; second < n; ++second) {
                                    const auto gatable = [&](int u) {
                                        return limits.gates_on_every_kind || graph.nodes[u].kind == Kind::Ok ||
                                               graph.nodes[u].kind == Kind::Ask;
                                    };
                                    if (!gatable(first) || !gatable(second)) continue;
                                    graph.nodes[first].gate = graph.nodes[second].gate = true;
                                    visit(static_cast<const Graph&>(graph));
                                    graph.nodes[first].gate = graph.nodes[second].gate = false;
                                }
                        }
                        graph.join = -1;
                        return;
                    }
                    auto& node = graph.nodes[v];
                    const auto saved_targets = node.targets;
                    node = Node{Kind::Ok, saved_targets};
                    kinds(v + 1, special, asks, fail, flaky, map);
                    if (special >= max_special) {
                        node = Node{Kind::Ok, saved_targets};
                        return;
                    }
                    if (asks < limits.max_asks) {
                        node = Node{Kind::Ask, saved_targets};
                        kinds(v + 1, special + 1, asks + 1, fail, flaky, map);
                        if (saved_targets.size() == 2) {
                            node.conditional = true;
                            kinds(v + 1, special + 1, asks + 1, fail, flaky, map);
                            node.targets = {saved_targets[0], kEnd};
                            kinds(v + 1, special + 1, asks + 1, fail, flaky, map);
                        }
                    }
                    if (!fail) {
                        node = Node{Kind::Fail, saved_targets};
                        kinds(v + 1, special + 1, asks, true, flaky, map);
                        for (int t = v + 1; t < n; ++t) {
                            if (graph.nodes[t].targets.empty()) continue;
                            node.fail_target = t;
                            kinds(v + 1, special + 1, asks, true, flaky, map);
                            if (!limits.every_fail_target) break;
                        }
                    }
                    if (!flaky) {
                        node = Node{Kind::Flaky, saved_targets};
                        kinds(v + 1, special + 1, asks, fail, true, map);
                    }
                    if (!map) {
                        node = Node{Kind::Map, saved_targets};
                        for (const auto& work : work_patterns(limits)) {
                            for (int target = -1; target < n; ++target) {
                                if (target >= 0 && target <= v) continue;
                                if (!limits.every_worker_target && target >= 0 && target != n - 1) continue;
                                graph.work          = work;
                                graph.worker_target = target;
                                kinds(v + 1, special + 1, asks, fail, flaky, true);
                            }
                        }
                        graph.work.clear();
                        graph.worker_target = -1;
                    }
                    node = Node{Kind::Ok, saved_targets};
                };
            kinds(0, 0, 0, false, false, false);
        };
        topology(0);
    }
}

}  // namespace resume_model
