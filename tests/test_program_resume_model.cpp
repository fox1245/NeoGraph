// Bounded-exhaustive resume/replay harness at the Program level.
//
// The Core harness (tests/test_resume_model.cpp) checks the GraphEngine. This
// one drives the Program runtime as LocalProgramHost wires it: the in-memory
// stores with the in-process priced broker, or SQLite stores with the SQLite
// provider journal, a bound provider whose local transport counts every
// request it sends, and JavaScript main() programs (callCore, ng.all,
// ng.race, ng.quorum, ng.await) and typed operation plans (sequence,
// parallel, quorum) over a one-node Core whose node may call the provider and
// may ask.
//
// A reference interpreter states the documented contract for each program:
// the questions the run asks in order, the output, and what each provider
// call costs. The harness answers every question as the run asks it and
// checks, on every schedule:
//
//  P1 sent once, charged once: the transport sends each call the reference
//     makes once, and the run is charged its tokens and money once. With the
//     in-memory broker a node that runs again calls again (that is its
//     contract); with the SQLite journal a call is never sent twice.
//  P2 every question asked: the pauses and their pending inputs are the
//     reference's, and each answer reaches the call it was for.
//  P3 same result: the final status and output equal the reference.
//  P4 crash transparency (SQLite host, programs that run one call at a
//     time): the database copied at every checkpoint save, put_writes,
//     Program transition, provider dispatch and journaled call recovers
//     through reconnect() to the same P1-P3 results.
//  P5 fork equivalence: continuing a fork of a paused run equals continuing
//     the run.
//
// Schedules that follow a documented behaviour P4 or P5 does not cover are
// counted as questions for the contract instead of failing (see judge() and
// tests/model/README.md). NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended
// selects the larger bound.

#include "program_resume_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// The family of programs
// ---------------------------------------------------------------------------

enum class Beh { Ok, Ask, Llm, LlmAsk, Fail };
const char* to_string(Beh beh) {
    switch (beh) {
        case Beh::Ok: return "ok";
        case Beh::Ask: return "ask";
        case Beh::Llm: return "llm";
        case Beh::LlmAsk: return "llm-ask";
        case Beh::Fail: return "fail";
    }
    return "?";
}
bool calls(Beh beh) { return beh == Beh::Llm || beh == Beh::LlmAsk; }
bool asks(Beh beh) { return beh == Beh::Ask || beh == Beh::LlmAsk; }

struct Member {
    Beh         beh;
    std::string tag;
};

/// One yielded command of main(): a callCore, a join of callCores, or an
/// awaited callCore.
enum class Op { Call, All, Race, Quorum, Await };
struct Command {
    Op                  op = Op::Call;
    std::vector<Member> members;
    int                 in_flight = 2;  // joins
    int                 required  = 0;  // quorum
};
struct Spec {
    std::vector<Command> commands;
    /// Joins run their members one at a time: the run is sequential, so a
    /// copy of its durable state at any point is one a crash could leave.
    bool sequential() const {
        for (const auto& command : commands)
            if ((command.op == Op::All || command.op == Op::Race || command.op == Op::Quorum) &&
                command.in_flight > 1)
                return false;
        return true;
    }
};

std::string call_js(const Member& member, const std::string& label) {
    return std::string("ng.callCore(\"main\", {value: \"") + to_string(member.beh) + ":" + member.tag + "\"}, \"" +
           label + "\")";
}

std::string program_js(const Spec& spec) {
    std::ostringstream body;
    body << "  const out = [];\n";
    for (std::size_t c = 0; c < spec.commands.size(); ++c) {
        const auto& command = spec.commands[c];
        const auto  label   = "c" + std::to_string(c + 1);
        std::string members;
        for (std::size_t m = 0; m < command.members.size(); ++m)
            members += (m ? ", " : "") + call_js(command.members[m], label + "m" + std::to_string(m));
        const auto options = "{max_in_flight: " + std::to_string(command.in_flight) +
                             (command.op == Op::Quorum ? ", required_successes: " + std::to_string(command.required)
                                                       : std::string()) +
                             "}";
        switch (command.op) {
            case Op::Call:
                body << "  out.push((yield " << call_js(command.members[0], label) << ").out);\n";
                break;
            case Op::Await:
                body << "  out.push((yield ng.await(" << call_js(command.members[0], label + "m0") << ", 600000, \""
                     << label << "\")).out);\n";
                break;
            case Op::All:
                body << "  out.push((yield ng.all([" << members << "], " << options << ", \"" << label
                     << "\")).map((reply) => reply.out));\n";
                break;
            case Op::Race:
                body << "  out.push((yield ng.race([" << members << "], " << options << ", \"" << label
                     << "\")).out);\n";
                break;
            case Op::Quorum:
                body << "  out.push((yield ng.quorum([" << members << "], " << options << ", \"" << label
                     << "\")).map((reply) => reply.out));\n";
                break;
        }
    }
    body << "  return {out: out};";
    return javascript_program(body.str());
}

std::string describe(const Spec& spec) {
    std::ostringstream out;
    for (std::size_t c = 0; c < spec.commands.size(); ++c) {
        const auto& command = spec.commands[c];
        out << (c ? "; " : "")
            << (command.op == Op::Call     ? "call"
                : command.op == Op::Await  ? "await"
                : command.op == Op::All    ? "all"
                : command.op == Op::Race   ? "race"
                                           : "quorum" + std::to_string(command.required));
        if (command.op != Op::Call && command.op != Op::Await) out << "/" << command.in_flight;
        out << "(";
        for (std::size_t m = 0; m < command.members.size(); ++m)
            out << (m ? "," : "") << to_string(command.members[m].beh);
        out << ")";
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Reference interpreter
// ---------------------------------------------------------------------------

/// The documented contract (#380, #384, #387, #378, #372, #391): a command
/// runs its members; a member that asks pauses the run on it (a join on its
/// first paused member in declaration order, once the members that run have
/// finished or paused, and only if their results do not already decide the
/// join); a resume continues the paused member with the answer, replays the
/// members that finished and runs every other paused member again, which
/// asks again. A fail-fast join fails on a member's failure; a quorum that can
/// no longer reach its count fails. With the SQLite provider journal a call is
/// sent and charged once per run; with the in-process broker a node that runs
/// again calls, and pays, again.
struct Pause {
    std::string operation;
    std::string tag;  // the member that asked
    bool operator==(const Pause&) const = default;
};

struct Expected {
    ProgramTerminalStatus              status = ProgramTerminalStatus::Completed;
    std::vector<Pause>                 pauses;
    json                               out = json::array();
    std::map<std::string, int>                   runs_to_call;  // tag -> body runs that reach the call
    std::map<std::string, std::set<std::string>> call_slots;    // tag -> the invocations that call
    std::map<std::string, std::set<std::string>> consumed;  // tag -> the answers its runs consume
};

Expected reference(const Spec& spec, std::size_t max_answers) {
    Expected expected;
    int      answered = 0;
    enum class Kind { Done, Asked, Failed };
    struct Result {
        Kind        kind = Kind::Done;
        std::string value;
    };
    const auto run = [&](const Member& member, const std::string& answer) {
        if (calls(member.beh)) {
            ++expected.runs_to_call[member.tag];
            expected.call_slots[member.tag].insert(member.tag);
        }
        Result result;
        switch (member.beh) {
            case Beh::Ok: result.value = "done:" + member.tag; break;
            case Beh::Llm: result.value = "reply-" + member.tag; break;
            case Beh::Ask:
            case Beh::LlmAsk:
                if (answer.empty()) {
                    result.kind = Kind::Asked;
                } else {
                    expected.consumed[member.tag].insert(answer);
                    result.value = member.beh == Beh::Ask ? answer : "reply-" + member.tag + "/" + answer;
                }
                break;
            case Beh::Fail: result.kind = Kind::Failed; break;
        }
        return result;
    };
    // Pauses on a member; false once the schedule has no answer left.
    const auto pause = [&](const std::string& operation, const Member& member, std::string& answer) {
        expected.pauses.push_back(Pause{operation, member.tag});
        if (static_cast<std::size_t>(answered) >= max_answers) {
            expected.status = ProgramTerminalStatus::Interrupted;
            return false;
        }
        answer = "d" + std::to_string(++answered);
        return true;
    };
    for (std::size_t c = 0; c < spec.commands.size(); ++c) {
        const auto& command = spec.commands[c];
        const auto  base    = "root.javascript." + std::to_string(c + 1);
        const auto  member_op = [&](std::size_t m) { return base + "/member/" + std::to_string(m); };
        std::vector<Result> results(command.members.size());
        if (command.op == Op::Call || command.op == Op::Await) {
            const auto& member = command.members[0];
            auto        result = run(member, {});
            while (result.kind == Kind::Asked) {
                std::string answer;
                if (!pause(command.op == Op::Await ? base + "/await" : base, member, answer)) return expected;
                result = run(member, answer);
            }
            if (result.kind == Kind::Failed) {
                expected.status = ProgramTerminalStatus::Failed;
                return expected;
            }
            expected.out.push_back(result.value);
            continue;
        }
        const bool one_wins = command.op == Op::Race || (command.op == Op::Quorum && command.required == 1);
        if (one_wins) {
            // The family keeps only races with one member that finishes on its
            // own, or with every member asking: the first declared then wins.
            std::optional<std::size_t> finisher;
            for (std::size_t m = 0; m < command.members.size(); ++m)
                if (!asks(command.members[m].beh)) finisher = m;
            std::string value;
            if (finisher) {
                value = run(command.members[*finisher], {}).value;
            } else {
                for (const auto& member : command.members) (void)run(member, {});
                std::string answer;
                if (!pause(member_op(0), command.members[0], answer)) return expected;
                value = run(command.members[0], answer).value;
                // The other members run again and ask again; the race is decided.
                for (std::size_t m = 1; m < command.members.size(); ++m) (void)run(command.members[m], {});
            }
            if (command.op == Op::Race)
                expected.out.push_back(value);
            else
                expected.out.push_back(json::array({value}));
            continue;
        }
        // all, or a quorum that needs every member.
        const auto fails = [&] {
            expected.status = ProgramTerminalStatus::Failed;
            return expected;
        };
        if (command.in_flight == 1) {
            for (std::size_t m = 0; m < command.members.size(); ++m) {
                results[m] = run(command.members[m], {});
                while (results[m].kind == Kind::Asked) {
                    std::string answer;
                    if (!pause(member_op(m), command.members[m], answer)) return expected;
                    results[m] = run(command.members[m], answer);
                }
                if (results[m].kind == Kind::Failed) return fails();
            }
        } else {
            for (std::size_t m = 0; m < command.members.size(); ++m) results[m] = run(command.members[m], {});
            while (true) {
                for (const auto& result : results)
                    if (result.kind == Kind::Failed) return fails();
                std::optional<std::size_t> first;
                for (std::size_t m = 0; m < results.size() && !first; ++m)
                    if (results[m].kind == Kind::Asked) first = m;
                if (!first) break;
                std::string answer;
                if (!pause(member_op(*first), command.members[*first], answer)) return expected;
                for (std::size_t m = 0; m < results.size(); ++m)
                    if (results[m].kind == Kind::Asked)
                        results[m] = run(command.members[m], m == *first ? answer : std::string());
            }
        }
        json values = json::array();
        for (const auto& result : results) values.push_back(result.value);
        expected.out.push_back(std::move(values));
    }
    return expected;
}

/// Every program of the bound: one command (or two, extended), each a call,
/// an awaited call, or a join of two members, with every member behaviour.
/// Races and one-success quorums keep one member that finishes on its own or
/// all asking; a member that fails joins concurrent members only beside ones
/// that make no call (a cancelled member may or may not have sent its call).
std::vector<Spec> program_family(int max_commands) {
    const std::vector<Beh> all{Beh::Ok, Beh::Ask, Beh::Llm, Beh::LlmAsk, Beh::Fail};
    std::vector<Command>   commands;
    for (const auto beh : all) {
        commands.push_back(Command{Op::Call, {Member{beh, ""}}});
        commands.push_back(Command{Op::Await, {Member{beh, ""}}});
    }
    for (const auto a : all) {
        for (const auto b : all) {
            for (const int in_flight : {1, 2}) {
                const bool concurrent_failure =
                    in_flight == 2 && (a == Beh::Fail || b == Beh::Fail) && (calls(a) || calls(b));
                if (!concurrent_failure) {
                    commands.push_back(Command{Op::All, {Member{a, ""}, Member{b, ""}}, in_flight});
                    commands.push_back(Command{Op::Quorum, {Member{a, ""}, Member{b, ""}}, in_flight, 2});
                }
            }
            const bool decided = (asks(a) && asks(b)) || (asks(a) != asks(b));
            const bool plain   = a != Beh::Fail && b != Beh::Fail && a != Beh::LlmAsk && b != Beh::LlmAsk;
            if (decided && plain) {
                commands.push_back(Command{Op::Race, {Member{a, ""}, Member{b, ""}}, 2});
                commands.push_back(Command{Op::Quorum, {Member{a, ""}, Member{b, ""}}, 2, 1});
            }
        }
    }
    std::vector<Spec> family;
    const auto tagged = [](Spec spec) {
        int next = 0;
        for (auto& command : spec.commands)
            for (auto& member : command.members) member.tag = std::string(1, static_cast<char>('a' + next++));
        return spec;
    };
    for (const auto& first : commands) family.push_back(tagged(Spec{{first}}));
    if (max_commands >= 2) {
        // Two commands: the second asks or calls, after every first one.
        for (const auto& first : commands)
            for (const auto second : {Beh::Ask, Beh::LlmAsk})
                family.push_back(tagged(Spec{{first, Command{Op::Call, {Member{second, ""}}}}}));
    }
    return family;
}

}  // namespace



namespace {

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

/// A typed operation plan over the same Core: a parallel of two calls, a
/// quorum of two out of three, a sequence of two calls, or a call then a
/// parallel of two. The branches of a parallel or quorum share their input,
/// so they take the same step; a sequence's calls take the next step each.
enum class PlanOp { Parallel, Quorum, Sequence, SequenceThenParallel };
struct PlanSpec {
    PlanOp           op;
    std::vector<Beh> steps;  // one per call position
};

json core_definition() {
    return json{{"schema_version", 1},
                {"name", "main"},
                {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"k", json{{"reducer", "overwrite"}, {"initial", 0}}},
                                  {"out", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
                {"nodes", json{{"work", json{{"type", "rm.step"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                       json{{"from", "work"}, {"to", "__end__"}}})},
                {"conditional_edges", json::array()}};
}

json plan_document(const PlanSpec& plan) {
    const json call{{"op", "call_core"}};
    json       root;
    switch (plan.op) {
        case PlanOp::Parallel: root = json{{"op", "parallel"}, {"branches", json::array({call, call})}}; break;
        case PlanOp::Quorum:
            root = json{{"op", "quorum"}, {"branches", json::array({call, call, call})}, {"min_success", 2}};
            break;
        case PlanOp::Sequence: root = json{{"op", "sequence"}, {"children", json::array({call, call})}}; break;
        case PlanOp::SequenceThenParallel:
            root = json{{"op", "sequence"},
                        {"children", json::array({call, json{{"op", "parallel"},
                                                             {"branches", json::array({call, call})}}})}};
            break;
    }
    root["name"]       = "main";
    root["definition"] = core_definition();
    const auto bound   = [](const char* resource, std::uint64_t minimum, std::uint64_t maximum) {
        return json{{"resource", resource}, {"minimum", minimum}, {"maximum", maximum}};
    };
    return json{{"program_schema_version", 2U},
                {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"root", std::move(root)},
                {"declared_budget_requirements",
                 json::array({bound("wall_time_ms", 1, 100000), bound("model_tokens", 0, 10000000),
                              bound("monetary_microunits", 0, 1000000000), bound("max_concurrency", 1, 4),
                              bound("max_program_operations", 1, 32), bound("max_core_steps", 1, 64),
                              bound("max_dynamic_compiles", 0, 0), bound("max_child_depth", 0, 1),
                              bound("max_total_children", 0, 1)})}};
}

std::string plan_steps(const PlanSpec& plan) { return std::string(to_string(plan.steps[0])) + ":a"; }

std::string describe(const PlanSpec& plan) {
    std::string out = plan.op == PlanOp::Parallel   ? "plan parallel("
                      : plan.op == PlanOp::Quorum   ? "plan quorum2of3("
                      : plan.op == PlanOp::Sequence ? "plan sequence("
                                                    : "plan sequence+parallel(";
    for (std::size_t i = 0; i < plan.steps.size(); ++i) out += (i ? "," : "") + std::string(to_string(plan.steps[i]));
    return out + ")";
}

/// The reference of a plan, by the contract of #384, #386 and #387: a
/// parallel runs its branches; one that asks pauses it on its first paused
/// branch, a branch that fails fails it; resume continues the answered
/// branch, replays the ones that finished, and every other paused branch asks
/// again. A quorum starts its branches one at a time while one is paused; the
/// ones that finished decide it when they can; it fails once out of reach. A
/// sequence runs its children in turn.
Expected plan_reference(const PlanSpec& plan, std::size_t max_answers) {
    Expected expected;
    int      answered = 0;
    enum class Kind { Done, Asked, Failed };
    struct Result {
        Kind        kind = Kind::Done;
        std::string value;
    };
    const auto run = [&](Beh beh, const std::string& tag, const std::string& answer,
                         const std::string& slot = std::string()) {
        if (calls(beh)) {
            ++expected.runs_to_call[tag];
            expected.call_slots[tag].insert(slot.empty() ? tag : slot);
        }
        Result result;
        switch (beh) {
            case Beh::Ok: result.value = "done:" + tag; break;
            case Beh::Llm: result.value = "reply-" + tag; break;
            case Beh::Ask:
            case Beh::LlmAsk:
                if (answer.empty()) {
                    result.kind = Kind::Asked;
                } else {
                    expected.consumed[tag].insert(answer);
                    result.value = beh == Beh::Ask ? answer : "reply-" + tag + "/" + answer;
                }
                break;
            case Beh::Fail: result.kind = Kind::Failed; break;
        }
        return result;
    };
    const auto pause = [&](const std::string& operation, const std::string& tag, std::string& answer) {
        expected.pauses.push_back(Pause{operation, tag});
        if (static_cast<std::size_t>(answered) >= max_answers) {
            expected.status = ProgramTerminalStatus::Interrupted;
            return false;
        }
        answer = "d" + std::to_string(++answered);
        return true;
    };
    // The first call takes the plan's step "a"; the calls after it, whose
    // input sets no step, ask as "z".
    const auto tag_of = [](std::size_t position) { return std::string(position == 0 ? "a" : "z"); };
    // One call: false when the run stopped (paused past the schedule, or failed).
    const auto call = [&](const std::string& operation, std::size_t position, std::string& value) {
        const auto beh    = plan.steps[position];
        auto       result = run(beh, tag_of(position), {}, operation);
        while (result.kind == Kind::Asked) {
            std::string answer;
            if (!pause(operation, tag_of(position), answer)) return false;
            result = run(beh, tag_of(position), answer, operation);
        }
        if (result.kind == Kind::Failed) {
            expected.status = ProgramTerminalStatus::Failed;
            return false;
        }
        value = result.value;
        return true;
    };
    // A parallel of `count` branches of one step (#384): one that asks pauses
    // it on its first paused branch; a resume runs the answered branch, and
    // every other paused branch is replayed as paused: it asks again without
    // running.
    const auto parallel = [&](const std::string& base, std::size_t position, std::size_t count, json& values) {
        const auto          beh = plan.steps[position];
        std::vector<Result> results;
        for (std::size_t i = 0; i < count; ++i)
            results.push_back(run(beh, tag_of(position), {}, base + "." + std::to_string(i)));
        while (true) {
            for (const auto& result : results)
                if (result.kind == Kind::Failed) {
                    expected.status = ProgramTerminalStatus::Failed;
                    return false;
                }
            std::optional<std::size_t> first;
            for (std::size_t i = 0; i < results.size() && !first; ++i)
                if (results[i].kind == Kind::Asked) first = i;
            if (!first) break;
            std::string answer;
            if (!pause(base + "." + std::to_string(*first), tag_of(position), answer)) return false;
            results[*first] = run(beh, tag_of(position), answer, base + "." + std::to_string(*first));
        }
        values = json::array();
        for (const auto& result : results) values.push_back(result.value);
        return true;
    };
    std::string value;
    switch (plan.op) {
        case PlanOp::Parallel: {
            json values;
            if (!parallel("root", 0, 2, values)) return expected;
            expected.out = values;
            break;
        }
        case PlanOp::Quorum: {
            json values   = json::array();
            int  failures = 0;
            for (std::size_t i = 0; i < 3 && values.size() < 2; ++i) {
                const auto beh    = plan.steps[0];
                const auto slot   = "root." + std::to_string(i);
                auto       result = run(beh, tag_of(0), {}, slot);
                while (result.kind == Kind::Asked) {
                    std::string answer;
                    if (!pause(slot, tag_of(0), answer)) return expected;
                    result = run(beh, tag_of(0), answer, slot);
                }
                if (result.kind == Kind::Done) values.push_back(result.value);
                if (result.kind == Kind::Failed && 3 - ++failures < 2) {
                    expected.status = ProgramTerminalStatus::Failed;
                    return expected;
                }
            }
            expected.out = values;
            break;
        }
        case PlanOp::Sequence:
            if (!call("root.0", 0, value) || !call("root.1", 1, value)) return expected;
            expected.out = value;
            break;
        case PlanOp::SequenceThenParallel: {
            if (!call("root.0", 0, value)) return expected;
            json values;
            if (!parallel("root.1", 1, 2, values)) return expected;
            expected.out = values;
            break;
        }
    }
    return expected;
}

/// The plans of the bound: each shape with every first step.
std::vector<PlanSpec> plan_family() {
    const std::vector<Beh> all{Beh::Ok, Beh::Ask, Beh::Llm, Beh::LlmAsk, Beh::Fail};
    std::vector<PlanSpec>  family;
    for (const auto beh : all) family.push_back(PlanSpec{PlanOp::Parallel, {beh}});
    for (const auto beh : all) family.push_back(PlanSpec{PlanOp::Quorum, {beh}});
    // The second call of a sequence asks (see StepNode).
    for (const auto first : all) family.push_back(PlanSpec{PlanOp::Sequence, {first, Beh::Ask}});
    for (const auto first : {Beh::Ok, Beh::Ask, Beh::LlmAsk})
        family.push_back(PlanSpec{PlanOp::SequenceThenParallel, {first, Beh::Ask}});
    return family;
}

/// What the harness runs: a JavaScript main() or a plan, its input, and what
/// the reference expects of it.
struct ProgramCase {
    std::string description;
    bool        plan = false;
    std::string source;    // JavaScript
    json        document;  // plan
    json        input = json::object();
    bool        sequential = true;  // crash images are deterministic
    bool        crash_candidate = true;
    Expected    expected;
};

/// A plan's output, projected to the "out" channel of every Core result in it.
json project_plan_output(const json& output) {
    if (output.is_array()) {
        json values = json::array();
        for (std::size_t i = 0; i < output.size(); ++i) values.push_back(project_plan_output(output[i]));
        return values;
    }
    if (output.is_object() && output.contains("channels") && output["channels"].contains("out"))
        return output["channels"]["out"]["value"];
    return output;
}

// ---------------------------------------------------------------------------
// Driving a schedule
// ---------------------------------------------------------------------------

/// What one schedule showed.
struct Observed {
    ProgramTerminalStatus              status = ProgramTerminalStatus::Completed;
    std::vector<Pause>                 pauses;
    json                               output;
    std::string                        failure;
    std::map<std::string, int>         sent;      // tag -> requests the transport sent
    std::vector<StepJournal::Run>      runs;      // node body runs
    std::uint64_t                      tokens = 0, money = 0;  // charged over the run
    int                                ops    = 0;
    bool                               crashed = false;
    std::string                        error;  // an exception the host got
    /// The run a crash left in a JavaScript command came back asking the
    /// host to reconcile that command (a pending effect, no pending input).
    bool reconciliation = false;
    /// The durable operation the crash copy was taken at.
    std::string crash_at;
};

Pause pause_of(const ProgramResult& result) {
    const auto interrupt = *result.interrupt();
    Pause      pause{interrupt.pending_input ? interrupt.pending_input->operation_id() : std::string(), ""};
    if (interrupt.pending_input) {
        const auto value = interrupt.pending_input->core_interrupt_value();
        if (value.is_object() && value.contains("value") && value["value"].is_object())
            pause.tag = value["value"].value("ask", std::string());
    }
    return pause;
}

struct Schedule {
    std::size_t max_answers = 4;
    int         fork_at     = -1;  // fork when answering this pause
    int         crash_op    = -1;  // copy the durable state at this durable operation
    bool        crash_after = false;
};

std::string describe(const Schedule& schedule) {
    std::string out = "answers<=" + std::to_string(schedule.max_answers);
    if (schedule.fork_at >= 0) out += " fork at pause " + std::to_string(schedule.fork_at);
    if (schedule.crash_op >= 0)
        out += std::string(" crash ") + (schedule.crash_after ? "after" : "before") + " op " +
               std::to_string(schedule.crash_op);
    return out;
}

void finish(Observed& observed, const ProgramResult& result, bool plan) {
    observed.status = result.status();
    if (result.status() == ProgramTerminalStatus::Completed)
        observed.output = plan ? project_plan_output(result.output()) : result.output();
    if (const auto failure = result.failure()) observed.failure = failure->code + ": " + failure->message;
    const auto initial    = Host::budget();
    observed.tokens       = initial.model_tokens - result.remaining_budget().model_tokens;
    observed.money        = initial.monetary_microunits - result.remaining_budget().monetary_microunits;
}

/// Runs one schedule. Every pause is answered in turn ("d1", "d2", ...) until
/// `max_answers`. A crash copies the durable state at the armed operation;
/// what the original process does after it never happened, and a new process
/// on the copy reconnects the run and goes on as a host would: a pause it was
/// answering is answered again, a new one is answered next.
Observed run_schedule(const ProgramCase& program, bool durable, const Schedule& schedule,
                      const ProgramBundle& bundle) {
    TempDir live("live"), image("image");
    World   world(next_world_id());
    const auto dispatching = [&world] { world.crash.durable("provider dispatch", [] {}); };
    auto       peer        = std::make_shared<Peer>(&world.crash.alive, dispatching);
    world.crash.reset(live.path, image.path, schedule.crash_op, schedule.crash_after);
    Observed observed;
    const std::string run_id = world.run_id();
    std::string       current = run_id;
    std::size_t       answered = 0;

    const auto t0 = std::chrono::steady_clock::now();
    auto host    = std::make_unique<Host>(world, peer, durable ? std::optional<std::filesystem::path>(live.path) : std::nullopt);
    const auto t1 = std::chrono::steady_clock::now();
    const auto version = host->admit(bundle);
    const auto t2 = std::chrono::steady_clock::now();
    if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_TIMING"))
        std::cout << "    [timing] host " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, admit "
                  << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms\n";
    std::optional<ProgramResult> result;
    std::string                  error;
    const auto attempt = [&](const std::function<ProgramResult()>& call) {
        try {
            result = call();
        } catch (const std::exception& e) {
            result.reset();
            error = e.what();
        }
    };
    attempt([&] { return host->run(version, run_id, program.input); });
    bool recovering = false;
    std::shared_ptr<Peer> recovered_peer;
    while (true) {
        if (!world.crash.alive) {
            // The process died during the call; a new one reconnects.
            observed.crashed = true;
            {
                std::lock_guard lock(peer->mutex);
                for (const auto& [tag, count] : peer->sent) observed.sent[tag] += count;
            }
            host.reset();
            if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_DEBUG")) {
                SQLiteProgramTransitionStore inspect((image.path / "durable.sqlite").string());
                if (const auto record = inspect.load(kOwner, current)) {
                    std::cout << "    [image] state " << to_string(record->continuation().state) << " attempt "
                              << record->continuation().attempt << " pending input "
                              << (record->pending_input() ? record->pending_input()->operation_id() : "-")
                              << " checkpoint "
                              << (record->exact_checkpoint() ? record->exact_checkpoint()->checkpoint_id : "-")
                              << " resume " << (record->attempt_resume() ? "yes" : "no") << "\n";
                    for (const auto& entry : inspect.load_javascript_commands(kOwner, current))
                        std::cout << "    [image] command " << entry.command_ordinal() << " "
                                  << (entry.pending() ? "pending" : "completed") << " settled "
                                  << (entry.settled_members() ? entry.settled_members()->dump() : "-") << " result "
                                  << (entry.terminal_result() ? entry.terminal_result()->dump().substr(0, 80) : "-")
                                  << "\n";
                } else {
                    std::cout << "    [image] no run record\n";
                }
            }
            world.crash.alive = true;
            recovered_peer    = std::make_shared<Peer>(&world.crash.alive, dispatching);
            peer              = recovered_peer;
            host              = std::make_unique<Host>(world, peer, std::optional<std::filesystem::path>(image.path));
            error.clear();
            attempt([&] { return host->reconnect(current); });
            if (!result && (error.find("P_RUN_NOT_FOUND") != std::string::npos ||
                            error.find("run was not found") != std::string::npos)) {
                error.clear();
                attempt([&] { return host->run(version, run_id, program.input); });
            }
            recovering = true;
            continue;
        }
        if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_DEBUG"))
            std::cout << "    [debug] " << (result ? describe(*result) : "exception: " + error) << "\n";
        if (!result) {
            observed.error = error.empty() ? "no result" : error;
            break;
        }
        if (result->status() == ProgramTerminalStatus::Interrupted && result->interrupt() &&
            result->interrupt()->pending_effect && !result->interrupt()->pending_input)
            observed.reconciliation = true;
        if (result->status() != ProgramTerminalStatus::Interrupted || !result->interrupt() ||
            !result->interrupt()->pending_input) {
            finish(observed, *result, program.plan);
            break;
        }
        const auto pause = pause_of(*result);
        // After a recovery the pause the host was answering may still be pending.
        const bool repeated = recovering && !observed.pauses.empty() && observed.pauses.back() == pause &&
                              answered == observed.pauses.size();
        recovering = false;
        if (repeated) {
            --answered;
        } else {
            observed.pauses.push_back(pause);
        }
        if (answered >= schedule.max_answers) {
            finish(observed, *result, program.plan);
            break;
        }
        const auto value   = json{{"decision", "d" + std::to_string(answered + 1)}};
        const auto pending = result->interrupt()->pending_input->call_id();
        ++answered;
        if (schedule.fork_at == static_cast<int>(answered - 1)) {
            current          = world.run_id("run-fork");
            const auto paused = *result;
            attempt([&] { return host->fork(version, paused, current, value, pending); });
        } else {
            attempt([&] { return host->resume(current, value, pending); });
        }
    }
    {
        std::lock_guard lock(peer->mutex);
        for (const auto& [tag, count] : peer->sent) observed.sent[tag] += count;
    }
    {
        std::lock_guard lock(world.journal.mutex);
        observed.runs = world.journal.runs;
    }
    observed.ops      = world.crash.ops;
    observed.crash_at = world.crash.taken_at;
    const auto t3 = std::chrono::steady_clock::now();
    host.reset();
    if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_TIMING"))
        std::cout << "    [timing] runs " << std::chrono::duration<double, std::milli>(t3 - t2).count()
                  << " ms, teardown " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t3).count()
                  << " ms\n";
    if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_DEBUG")) {
        std::cout << "    [debug] body runs:";
        for (const auto& run : observed.runs)
            std::cout << " " << run.value << (run.asked ? "?" : "") << (run.answer.empty() ? "" : "=" + run.answer)
                      << "@" << run.thread.substr(0, 8) << "/" << run.task;
        std::cout << "\n";
    }
    return observed;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

std::vector<std::string> compare(const ProgramCase& program, const Observed& observed, bool durable) {
    const auto& expected = program.expected;
    const auto  want     = program.plan ? expected.out : json{{"out", expected.out}};
    std::vector<std::string> problems;
    if (!observed.error.empty()) problems.push_back("P3: the host got an exception: " + observed.error);
    if (observed.status != expected.status)
        problems.push_back("P3: status reference " + std::string(to_string(expected.status)) + ", runtime " +
                           std::string(to_string(observed.status)) +
                           (observed.failure.empty() ? "" : " (" + observed.failure + ")"));
    else if (expected.status == ProgramTerminalStatus::Completed && observed.output.dump() != want.dump())
        problems.push_back("P3: output reference " + want.dump() + ", runtime " + observed.output.dump());
    if (observed.pauses != expected.pauses) {
        std::ostringstream out;
        out << "P2: pauses reference [";
        for (const auto& pause : expected.pauses) out << " " << pause.operation << "(" << pause.tag << ")";
        out << " ], runtime [";
        for (const auto& pause : observed.pauses) out << " " << pause.operation << "(" << pause.tag << ")";
        out << " ]";
        problems.push_back(out.str());
    }
    for (const auto& run : observed.runs) {
        if (run.answer.empty()) continue;
        const auto tag      = run.value.substr(run.value.find(':') + 1);
        const auto expected_answer = expected.consumed.find(tag);
        if (expected_answer == expected.consumed.end() || !expected_answer->second.count(run.answer)) {
            std::string want;
            if (expected_answer != expected.consumed.end())
                for (const auto& answer : expected_answer->second) want += (want.empty() ? "" : "/") + answer;
            problems.push_back("P2: member " + tag + " consumed " + run.answer + ", reference " +
                               (want.empty() ? std::string("none") : want));
        }
    }
    // P1: requests and charges.
    int expected_sends = 0;
    for (const auto& [tag, runs] : expected.runs_to_call) {
        const int want = durable ? static_cast<int>(expected.call_slots.at(tag).size()) : runs;
        expected_sends += want;
        const auto found = observed.sent.find(tag);
        const int  got   = found == observed.sent.end() ? 0 : found->second;
        if (got != want)
            problems.push_back("P1: the call of " + tag + " was sent " + std::to_string(got) + " time(s), reference " +
                               std::to_string(want));
    }
    for (const auto& [tag, count] : observed.sent)
        if (!expected.runs_to_call.count(tag))
            problems.push_back("P1: " + tag + " sent a call the reference never makes");
    if (observed.error.empty() && expected.status == observed.status &&
        (observed.tokens != kCallTokens * expected_sends || observed.money != kCallMoney * expected_sends))
        problems.push_back("P1: charged " + std::to_string(observed.tokens) + " tokens and " +
                           std::to_string(observed.money) + " microunits, reference " +
                           std::to_string(kCallTokens * expected_sends) + " and " +
                           std::to_string(kCallMoney * expected_sends));
    return problems;
}

struct ProgramTotals {
    std::uint64_t programs = 0, schedules = 0, crash_runs = 0, fork_runs = 0, pauses = 0;
    std::uint64_t mismatches = 0;
    std::map<std::string, std::uint64_t> signatures;
    // First example of each kind, with its program's index: the merged report
    // keeps the smallest program, whichever worker found it.
    std::map<std::string, std::pair<std::size_t, std::string>> first;
    std::map<std::string, std::uint64_t>                       known;
    std::map<std::string, std::pair<std::size_t, std::string>> known_example;
    std::size_t                                                program_index = 0;

    static void keep(std::map<std::string, std::pair<std::size_t, std::string>>& examples, const std::string& key,
                     std::pair<std::size_t, std::string> example) {
        const auto found = examples.find(key);
        if (found == examples.end() || example.first < found->second.first) examples[key] = std::move(example);
    }
    void merge(const ProgramTotals& other) {
        programs += other.programs;
        schedules += other.schedules;
        crash_runs += other.crash_runs;
        fork_runs += other.fork_runs;
        pauses += other.pauses;
        mismatches += other.mismatches;
        for (const auto& [key, count] : other.signatures) signatures[key] += count;
        for (const auto& [key, example] : other.first) keep(first, key, example);
        for (const auto& [key, count] : other.known) known[key] += count;
        for (const auto& [key, example] : other.known_example) keep(known_example, key, example);
    }
};

void judge(ProgramTotals& totals, const ProgramCase& program, bool durable, const Schedule& schedule,
           const Observed& observed) {
    ++totals.schedules;
    const auto problems = compare(program, observed, durable);
    if (problems.empty()) return;
    // Behaviours this harness reports as questions for the contract instead of
    // failing (tests/model/README.md): a crash in a JavaScript command leaves
    // it to the host to reconcile, a plan that stopped before its first Core
    // checkpoint cannot be recovered, a fork sends again the call of a paused
    // invocation, and a crash while a request was out holds that call.
    const auto known = [&](const std::string& issue, const std::string& example) {
        ++totals.known[issue];
        ProgramTotals::keep(totals.known_example, issue,
                            {totals.program_index, program.description + " | " + describe(schedule) + " | " + example});
    };
    if (observed.reconciliation && schedule.crash_op >= 0) {
        known("JavaScript command reconciliation after a crash", problems.front());
        return;
    }
    if (schedule.crash_op >= 0 && observed.error.find("not replay-safe") != std::string::npos) {
        known("plan recovery blocked before the first Core checkpoint", problems.front());
        return;
    }
    // A crash while a request was out leaves its dispatch unresolved: the
    // journal never sends it again and holds it (#364), so the run cannot end
    // as the reference does. Only a second send is a mismatch then.
    if (observed.crash_at == "provider dispatch" &&
        std::none_of(problems.begin(), problems.end(), [](const std::string& problem) {
            return problem.rfind("P1: the call of", 0) == 0 && problem.find("sent 2") != std::string::npos;
        })) {
        known("uncertain dispatch after a crash (held, never sent again)", problems.front());
        return;
    }
    // A fork is a new run: the SQLite journal keys a call by its run, so the
    // call a paused invocation made before it asked is sent and charged again
    // in the fork, where a resume of the source replays it.
    const bool only_sends = std::all_of(problems.begin(), problems.end(), [](const std::string& problem) {
        return problem.rfind("P1: the call of", 0) == 0 || problem.rfind("P1: charged", 0) == 0;
    });
    if (schedule.fork_at >= 0 && durable && only_sends) {
        known("a fork sends again the call of a paused invocation", problems.front());
        return;
    }
    std::string kind = problems.front().substr(0, problems.front().find(':')) + " " +
                       (problems.front().find("sent") != std::string::npos      ? "sent"
                        : problems.front().find("charged") != std::string::npos ? "charged"
                        : problems.front().find("status") != std::string::npos  ? "status"
                        : problems.front().find("pauses") != std::string::npos  ? "pauses"
                        : problems.front().find("consumed") != std::string::npos ? "answer"
                        : problems.front().find("exception") != std::string::npos ? "exception"
                                                                                   : "output");
    kind += schedule.crash_op >= 0 ? " after a crash" : schedule.fork_at >= 0 ? " on a fork" : "";
    kind += durable ? ", sqlite" : ", memory";
    ++totals.mismatches;
    ++totals.signatures[kind];
    {
        std::ostringstream out;
        out << "program resume model mismatch (" << (durable ? "SQLite journal" : "in-process broker") << ")\n  program "
            << program.description << "\n  schedule " << describe(schedule) << " (" << observed.ops
            << " durable ops)\n  source\n" << (program.plan ? program.document.dump() : program.source);
        for (const auto& problem : problems) out << "\n  " << problem;
        ProgramTotals::keep(totals.first, kind, {totals.program_index, out.str()});
    }
}

struct ProgramBound {
    bool        extended    = false;
    std::size_t max_answers = 4;
    bool        crashes     = true;
    /// Fork at every pause, or at the first one.
    bool every_fork = false;
    /// Crash after every durable operation too. A crash after operation k
    /// leaves the durable state of a crash before k+1, with less work done;
    /// the CI bound crashes before every operation and after the last.
    bool crash_after_every_op = false;
    /// Crash and fork every program, or (CI) only the programs whose members
    /// ask or call: the others have no pause to recover or fork, and a call
    /// whose run crashed mid-command is reported, not compared.
    bool every_program_crashes = false;
};

/// The CI bound crashes the programs whose every member asks or calls, and
/// forks the first program of each command kind.
bool crash_candidate(const Spec& spec) {
    for (const auto& command : spec.commands)
        for (const auto& member : command.members)
            if (!asks(member.beh) && !calls(member.beh)) return false;
    return true;
}

ProgramBundle compile_program(const ProgramCase& program) {
    ProgramCompiler compiler(model_registry(), {"resume-model/v1"});
    if (program.plan) return compiler.compile(ProgramSource::from_cpp_builder("resume-model-plan", 2, program.document));
    return compiler.compile(ProgramSource::from_javascript("resume-model.js", program.source), Host::budget());
}

ProgramCase javascript_case(const Spec& spec, std::size_t max_answers) {
    ProgramCase program;
    program.description     = describe(spec);
    program.source          = program_js(spec);
    program.sequential      = spec.sequential();
    program.crash_candidate = crash_candidate(spec);
    program.expected        = reference(spec, max_answers);
    return program;
}

ProgramCase plan_case(const PlanSpec& plan, std::size_t max_answers) {
    ProgramCase program;
    program.description = describe(plan);
    program.plan        = true;
    program.document    = plan_document(plan);
    program.input       = json{{"value", plan_steps(plan)}};
    // Parallel branches run side by side: their durable writes interleave.
    program.sequential      = plan.op == PlanOp::Sequence;
    program.crash_candidate = true;
    for (const auto beh : plan.steps)
        if (!asks(beh) && !calls(beh)) program.crash_candidate = false;
    program.expected = plan_reference(plan, max_answers);
    return program;
}

void explore_program(ProgramTotals& totals, const ProgramCase& program, const ProgramBound& bound) {
    ++totals.programs;
    const auto& expected = program.expected;
    const auto  bundle   = compile_program(program);
    totals.pauses += expected.pauses.size();
    for (const bool durable : {false, true}) {
        const Schedule plain{bound.max_answers};
        const auto     base = run_schedule(program, durable, plain, bundle);
        judge(totals, program, durable, plain, base);
        // P5: a fork at the pauses the schedule answers.
        const bool forks = bound.every_program_crashes || program.crash_candidate;
        for (std::size_t k = 0; forks && k < std::min(bound.max_answers, expected.pauses.size()); ++k) {
            if (k > 0 && !bound.every_fork) break;
            Schedule forked{bound.max_answers, static_cast<int>(k)};
            ++totals.fork_runs;
            judge(totals, program, durable, forked, run_schedule(program, durable, forked, bundle));
        }
        // P4: a crash at every durable operation of a sequential run.
        if (!durable || !bound.crashes || !program.sequential || !forks) continue;
        const char* only_op = std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_CRASH_OP");
        for (int op = 0; op < base.ops; ++op) {
            if (only_op && *only_op && std::atoi(only_op) != op) continue;
            for (const bool after : {false, true}) {
                if (after && !bound.crash_after_every_op && op + 1 != base.ops) continue;
                Schedule crash{bound.max_answers, -1, op, after};
                ++totals.crash_runs;
                judge(totals, program, durable, crash, run_schedule(program, durable, crash, bundle));
            }
        }
    }
}

void report(const char* title, const ProgramTotals& totals, double seconds) {
    std::cout << "[ program resume model ] " << title << "\n  programs " << totals.programs << ", schedules "
              << totals.schedules << " (crash " << totals.crash_runs << ", fork " << totals.fork_runs
              << "), pauses " << totals.pauses << "\n  mismatches " << totals.mismatches << ", " << seconds << " s\n";
    for (const auto& [kind, count] : totals.signatures) std::cout << "    " << kind << ": " << count << "\n";
    for (const auto& [issue, count] : totals.known) {
        std::cout << "  contract question: " << issue << ": " << count << " schedules\n";
        const auto example = totals.known_example.find(issue);
        if (example != totals.known_example.end()) std::cout << "    first: " << example->second.second << "\n";
    }
}

}  // namespace

namespace {

ProgramBound selected_bound() {
    const char*  selected = std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND");
    ProgramBound bound;
    bound.extended = selected && std::string(selected) == "extended";
    if (bound.extended) {
        bound.every_fork            = true;
        bound.crash_after_every_op  = true;
        bound.every_program_crashes = true;
    }
    // NEOGRAPH_PROGRAM_RESUME_MODEL_CRASHES=0 leaves crashes out.
    const char* crashes = std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_CRASHES");
    bound.crashes       = !(crashes && std::string(crashes) == "0");
    return bound;
}

/// NEOGRAPH_PROGRAM_RESUME_MODEL_ONLY keeps the programs whose description
/// contains it.
bool selected(const std::string& description) {
    const char* only = std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_ONLY");
    return !only || !*only || description.find(only) != std::string::npos;
}

/// Explores every case on NEOGRAPH_PROGRAM_RESUME_MODEL_THREADS workers
/// (default 4), each schedule in its own world, and checks the totals.
void check_cases(const std::vector<ProgramCase>& cases, const ProgramBound& bound, const std::string& title) {
    const auto  start       = std::chrono::steady_clock::now();
    const char* threads_env = std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_THREADS");
    const auto  workers =
        static_cast<std::size_t>(std::max(1, threads_env && *threads_env ? std::atoi(threads_env) : 4));
    std::vector<ProgramTotals>      partial(workers);
    std::vector<std::exception_ptr> errors(workers);
    std::vector<std::thread>        threads;
    for (std::size_t w = 0; w < workers; ++w)
        threads.emplace_back([&, w] {
            try {
                for (std::size_t index = w; index < cases.size(); index += workers) {
                    partial[w].program_index = index;
                    explore_program(partial[w], cases[index], bound);
                }
            } catch (...) {
                errors[w] = std::current_exception();
            }
        });
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors)
        if (error) std::rethrow_exception(error);
    ProgramTotals totals;
    for (const auto& part : partial) totals.merge(part);
    report((title + (bound.extended ? ", bound extended" : ", bound ci")).c_str(), totals,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    EXPECT_EQ(totals.mismatches, 0U);
    for (const auto& [kind, message] : totals.first) ADD_FAILURE() << message.second;
}

}  // namespace

#if defined(NEOGRAPH_PROGRAM_RESUME_MODEL_HAVE_QUICKJS)
TEST(ProgramResumeModel, JavaScriptRunsMatchTheReference) {
    const auto               bound = selected_bound();
    std::vector<ProgramCase> cases;
    for (const auto& spec : program_family(bound.extended ? 2 : 1))
        if (selected(describe(spec))) cases.push_back(javascript_case(spec, bound.max_answers));
    check_cases(cases, bound, "JavaScript main()");
}
#endif

TEST(ProgramResumeModel, PlanRunsMatchTheReference) {
    const auto               bound = selected_bound();
    std::vector<ProgramCase> cases;
    for (const auto& plan : plan_family())
        if (selected(describe(plan))) cases.push_back(plan_case(plan, bound.max_answers));
    check_cases(cases, bound, "typed plans");
}

