// X3: the opt-in soundness check of a topology's control skeleton
// (Program API v2, WP6; #363, #364, #365). graph_soundness.h states the model.
//
// The state graph is explored breadth first, so every witness is a shortest
// route sequence. A state is encoded as bytes: the ready set, then the signals
// each join holds, then the visit counts of nodes that declare max_visits.
// Each state records the transition that first reached it, so witnesses are
// rebuilt only for the states a finding names.

#include "graph_soundness.h"

#include <neograph/graph/registry.h>

#include "graph_v2_keys.h"
#include "routing_policy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::graph {

namespace {

constexpr const char*   kStart          = "__start__";
constexpr const char*   kEnd            = "__end__";
constexpr int           kEndTarget      = -1;
constexpr std::uint64_t kCombosPerState = 64;
constexpr std::size_t   kMaxTraceSteps  = 256;
constexpr std::size_t   kMessageSteps   = 6;

// Reasons a point of no return cannot complete (S1): a stall at join j (one
// bit per join, the last bit shared by joins past it), a failure, a loop.
constexpr int           kSharedStallBit = 61;
constexpr std::uint64_t kFailedBit      = std::uint64_t{1} << 62;
constexpr std::uint64_t kLoopBit        = std::uint64_t{1} << 63;

std::uint64_t stall_bit(std::size_t join) {
    return std::uint64_t{1} << std::min<std::size_t>(join, kSharedStallBit);
}

std::string pointer_token(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '~')
            escaped += "~0";
        else if (ch == '/')
            escaped += "~1";
        else
            escaped.push_back(ch);
    }
    return escaped;
}

json string_array(const std::vector<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) result.push_back(value);
    return result;
}

json string_array(const std::set<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) result.push_back(value);
    return result;
}

using Words = std::vector<std::uint64_t>;

bool bit_at(const Words& words, int index) {
    return (words[static_cast<std::size_t>(index) >> 6] >> (index & 63)) & 1U;
}

void set_bit(Words& words, int index) {
    words[static_cast<std::size_t>(index) >> 6] |= std::uint64_t{1} << (index & 63);
}

bool no_bits(const Words& words) {
    return std::all_of(words.begin(), words.end(), [](std::uint64_t word) { return word == 0; });
}

bool intersects(const Words& left, const Words& right) {
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i] & right[i]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

enum class Kind {
    Normal,      // the node runs and takes its edges or one conditional route
    Failure,     // the node fails and takes its failure edge (#364)
    VisitLimit,  // max_visits reached: the node does not run, its failure edge is taken
    Fails,       // the run fails: a visit limit without a failure edge, or no route
};

struct Option {
    Kind             kind = Kind::Normal;
    std::string      label;  // shown in witnesses; empty for a node's static edges
    std::vector<int> targets;
    bool             explicit_end = false;
    bool             failure_exit = false;
};

struct NodeModel {
    std::string              name;
    std::string              type;
    std::vector<Option>      normal;
    std::optional<int>       failure_target;
    std::string              route_pointer;
    std::string              failure_pointer;
    bool                     failure_exit = false;
    int                      max_visits   = 0;
    int                      visit_slot   = -1;
    std::vector<int>         resets;   // visit slots its completion resets
    std::vector<std::string> control;  // declared "goto" and "send"
};

struct JoinModel {
    int                                node = -1;
    std::vector<std::set<std::string>> slots;
    std::vector<Words>                 slot_bits;
    Words                              members;
};

struct State {
    Words                     ready;
    std::vector<Words>        pending;
    std::vector<std::uint8_t> visits;
};

enum class Terminal { None, Completed, CompletedOverPending, Stalled, FailureExit, Failed };

struct Transition {
    State              next;
    Terminal           terminal = Terminal::None;
    std::vector<int>   fired;
    std::vector<int>   signalled;
    std::vector<Words> signals;
    int                failed_node = -1;
};

struct StateRecord {
    std::uint32_t parent        = 0;
    std::uint64_t combo         = 0;
    bool          opaque        = false;
    bool          complete_exit = false;
    bool          any_exit      = false;
    std::uint64_t reasons       = 0;
};

struct Arrival {  // a transition: the state it leaves and its route combination
    std::uint32_t state = 0;
    std::uint64_t combo = 0;
};

struct PendingEnd {  // S2: a run that ends while a join holds signals
    Arrival     at;
    Words       pending;
    std::string kind;  // "stalled" or "completed"
};

class Checker {
public:
    Checker(const TopologySpec& topology, const GraphRegistry& registry, const SoundnessOptions& options)
        : topology_(topology), registry_(registry), options_(options) {}

    SoundnessReport run() {
        build_model();
        explore();
        return report();
    }

private:
    // ---- model ------------------------------------------------------------

    json effects_of(const std::string& type) const {
        if (options_.local_only)
            return registry_.contains_type(type) ? registry_.local_node_effects(type) : json();
        return registry_.node_effects(type);
    }

    std::optional<ConditionSpec> condition_spec(const std::string& name) const {
        if (options_.local_only)
            return registry_.contains_condition(name) ? registry_.local_condition_spec(name)
                                                      : std::optional<ConditionSpec>{};
        return registry_.condition_spec(name);
    }

    int index_of(const std::string& name) const {
        if (name == kEnd) return kEndTarget;
        const auto found = index_.find(name);
        return found == index_.end() ? std::numeric_limits<int>::min() : found->second;
    }

    void build_model() {
        for (const auto& [name, definition] : topology_.node_defs) {
            index_[name] = static_cast<int>(nodes_.size());
            NodeModel node;
            node.name = name;
            if (definition.is_object() && definition.contains("type") && definition["type"].is_string())
                node.type = definition["type"].get<std::string>();
            nodes_.push_back(std::move(node));
        }
        words_ = std::max<std::size_t>(1, (nodes_.size() + 63) / 64);

        // Effect contracts: declared control (goto, send) and writes.
        std::map<std::string, ReducerType> reducer_of;
        for (const auto& channel : topology_.channel_defs) reducer_of[channel.name] = channel.type;
        std::map<std::string, Words> writers;
        for (auto& node : nodes_) {
            const json effects = effects_of(node.type);
            if (!effects.is_object()) {
                effects_known_ = false;
                continue;
            }
            if (effects.contains("control") && effects["control"].is_array()) {
                std::set<std::string> declared;
                for (const auto& item : effects["control"]) {
                    if (!item.is_string()) continue;
                    const auto value = item.get<std::string>();
                    if (value == "goto" || value == "send") declared.insert(value);
                }
                node.control.assign(declared.begin(), declared.end());
            }
            if (effects.contains("writes") && effects["writes"].is_array()) {
                for (const auto& item : effects["writes"]) {
                    if (!item.is_string()) continue;
                    const auto channel = item.get<std::string>();
                    const auto reducer = reducer_of.find(channel);
                    if (reducer == reducer_of.end() || reducer->second != ReducerType::OVERWRITE) continue;
                    auto& bits = writers[channel];
                    if (bits.empty()) bits.assign(words_, 0);
                    set_bit(bits, index_.at(node.name));
                }
            }
        }
        for (auto& [channel, bits] : writers) {
            overwrite_channels_.push_back(channel);
            channel_writers_.push_back(std::move(bits));
        }
        // E5 already reports writers that are direct static fan-out siblings.
        std::map<std::string, std::set<int>> plain_out;
        for (const auto& edge : topology_.edges) {
            const int to = index_of(edge.to);
            if (to >= 0) plain_out[edge.from].insert(to);
        }
        for (const auto& [from, targets] : plain_out) {
            (void)from;
            for (const auto a : targets)
                for (const auto b : targets)
                    if (a < b) e5_pairs_.insert({a, b});
        }

        // Routes, as the scheduler takes them: a node's first conditional edge
        // replaces its static edges.
        std::set<std::string> routed;
        for (std::size_t i = 0; i < topology_.conditional_edges.size(); ++i) {
            const auto& edge = topology_.conditional_edges[i];
            const auto  from = index_.find(edge.from);
            if (from == index_.end() || !routed.insert(edge.from).second) continue;
            auto& node         = nodes_[static_cast<std::size_t>(from->second)];
            node.route_pointer = "/conditional_edges/" + std::to_string(i);
            const auto spec    = condition_spec(edge.condition);
            const bool closed  = spec && !spec->open;
            std::set<int> seen;
            for (const auto& [key, target] : edge.routes) {
                if (closed && std::find(spec->labels.begin(), spec->labels.end(), key) == spec->labels.end())
                    continue;
                const int to = index_of(target);
                if (to == std::numeric_limits<int>::min() || !seen.insert(to).second) continue;
                Option option;
                option.label        = key == detail::kLegacyDefaultRoute ? std::string(DEFAULT_ROUTE) : key;
                option.targets      = {to};
                option.explicit_end = to == kEndTarget;
                node.normal.push_back(std::move(option));
            }
        }
        std::map<int, std::set<int>> statics;
        std::set<int>                start;
        for (const auto& edge : topology_.edges) {
            const int to = index_of(edge.to);
            if (to == std::numeric_limits<int>::min()) continue;
            if (edge.from == kStart) {
                if (to >= 0) start.insert(to);
                continue;
            }
            const auto from = index_.find(edge.from);
            if (from == index_.end() || routed.count(edge.from)) continue;
            statics[from->second].insert(to);
        }
        start_.assign(start.begin(), start.end());
        for (std::size_t u = 0; u < nodes_.size(); ++u) {
            auto& node = nodes_[u];
            if (routed.count(node.name)) continue;
            Option option;
            const auto found = statics.find(static_cast<int>(u));
            if (found != statics.end()) option.targets.assign(found->second.begin(), found->second.end());
            option.explicit_end = std::find(option.targets.begin(), option.targets.end(), kEndTarget) !=
                                  option.targets.end();
            node.normal.push_back(std::move(option));
        }

        // #364: failure exits, failure edges and visit limits.
        for (const auto& [name, exit] : topology_.failure_exits) {
            (void)exit;
            const auto found = index_.find(name);
            if (found == index_.end()) continue;
            auto& node        = nodes_[static_cast<std::size_t>(found->second)];
            node.failure_exit = true;
            for (auto& option : node.normal) option.failure_exit = true;
        }
        for (std::size_t i = 0; i < topology_.failure_edges.size(); ++i) {
            const auto& edge = topology_.failure_edges[i];
            const auto  from = index_.find(edge.from);
            const int   to   = index_of(edge.to);
            if (from == index_.end() || to < 0) continue;
            auto& node           = nodes_[static_cast<std::size_t>(from->second)];
            node.failure_target  = to;
            node.failure_pointer = "/failure_edges/" + std::to_string(i);
        }
        for (const auto& [name, policy] : topology_.node_retry_policies) {
            const auto found = index_.find(name);
            if (found == index_.end() || !policy.max_visits || *policy.max_visits <= 0) continue;
            auto& node      = nodes_[static_cast<std::size_t>(found->second)];
            node.max_visits = std::min(*policy.max_visits, 255);
            node.visit_slot = static_cast<int>(visit_nodes_.size());
            visit_nodes_.push_back(found->second);
        }
        for (const auto u : visit_nodes_) {
            const auto& node = nodes_[static_cast<std::size_t>(u)];
            if (node.failure_target)
                nodes_[static_cast<std::size_t>(*node.failure_target)].resets.push_back(node.visit_slot);
        }

        // Joins: legacy barriers (singleton slots) and any_of joins.
        join_of_.assign(nodes_.size(), -1);
        const auto add_join = [&](const std::string& name, std::vector<std::set<std::string>> slots) {
            const auto found = index_.find(name);
            if (found == index_.end() || join_of_[static_cast<std::size_t>(found->second)] >= 0) return;
            JoinModel join;
            join.node  = found->second;
            join.slots = std::move(slots);
            join.members.assign(words_, 0);
            for (const auto& slot : join.slots) {
                Words bits(words_, 0);
                for (const auto& member : slot) {
                    const int index = index_of(member);
                    if (index < 0) continue;
                    set_bit(bits, index);
                    set_bit(join.members, index);
                }
                join.slot_bits.push_back(std::move(bits));
            }
            join_of_[static_cast<std::size_t>(found->second)] = static_cast<int>(joins_.size());
            joins_.push_back(std::move(join));
        };
        for (const auto& [name, members] : topology_.barrier_specs) {
            std::vector<std::set<std::string>> slots;
            for (const auto& member : members) slots.push_back({member});
            add_join(name, std::move(slots));
        }
        for (const auto& [name, spec] : topology_.join_specs) add_join(name, spec.slots);
        received_.assign(joins_.size(), Words(words_, 0));
        fired_.assign(joins_.size(), false);
    }

    // ---- states -----------------------------------------------------------

    std::string encode(const State& state) const {
        std::string key;
        key.resize((words_ * (1 + joins_.size())) * sizeof(std::uint64_t) + state.visits.size());
        char* out = key.data();
        std::memcpy(out, state.ready.data(), words_ * sizeof(std::uint64_t));
        out += words_ * sizeof(std::uint64_t);
        for (const auto& pending : state.pending) {
            std::memcpy(out, pending.data(), words_ * sizeof(std::uint64_t));
            out += words_ * sizeof(std::uint64_t);
        }
        if (!state.visits.empty()) std::memcpy(out, state.visits.data(), state.visits.size());
        return key;
    }

    State decode(const std::string& key) const {
        State       state;
        const char* in = key.data();
        state.ready.assign(words_, 0);
        std::memcpy(state.ready.data(), in, words_ * sizeof(std::uint64_t));
        in += words_ * sizeof(std::uint64_t);
        state.pending.assign(joins_.size(), Words(words_, 0));
        for (auto& pending : state.pending) {
            std::memcpy(pending.data(), in, words_ * sizeof(std::uint64_t));
            in += words_ * sizeof(std::uint64_t);
        }
        state.visits.assign(visit_nodes_.size(), 0);
        if (!state.visits.empty()) std::memcpy(state.visits.data(), in, state.visits.size());
        return state;
    }

    std::vector<int> ready_nodes(const State& state) const {
        std::vector<int> ready;
        for (std::size_t u = 0; u < nodes_.size(); ++u)
            if (bit_at(state.ready, static_cast<int>(u))) ready.push_back(static_cast<int>(u));
        return ready;
    }

    std::vector<std::vector<Option>> options_for(const State& state, const std::vector<int>& ready) const {
        std::vector<std::vector<Option>> options;
        options.reserve(ready.size());
        for (const auto u : ready) {
            const auto&         node = nodes_[static_cast<std::size_t>(u)];
            std::vector<Option> list;
            if (node.visit_slot >= 0 &&
                state.visits[static_cast<std::size_t>(node.visit_slot)] >= node.max_visits) {
                Option limit;
                limit.kind  = node.failure_target ? Kind::VisitLimit : Kind::Fails;
                limit.label = "visit_limit";
                if (node.failure_target) limit.targets = {*node.failure_target};
                list.push_back(std::move(limit));
            } else {
                list = node.normal;
                if (list.empty()) {
                    Option none;
                    none.kind  = Kind::Fails;
                    none.label = "no_route";
                    list.push_back(std::move(none));
                }
                if (node.failure_target) {
                    Option failure;
                    failure.kind    = Kind::Failure;
                    failure.label   = "on_failure";
                    failure.targets = {*node.failure_target};
                    list.push_back(std::move(failure));
                }
            }
            options.push_back(std::move(list));
        }
        return options;
    }

    // One super-step: every ready node takes the option the combination picks.
    Transition apply(const State&                            state,
                     const std::vector<int>&                 ready,
                     const std::vector<std::vector<Option>>& options,
                     std::uint64_t                           combo) const {
        Transition step;
        step.next.ready.assign(words_, 0);
        step.next.pending = state.pending;
        step.next.visits  = state.visits;
        step.signals.assign(joins_.size(), Words(words_, 0));
        std::vector<char> signalled(joins_.size(), 0);
        bool              explicit_end = false;
        bool              failure_exit = false;
        std::vector<int>  ran;
        for (std::size_t k = 0; k < ready.size(); ++k) {
            const int   u      = ready[k];
            const auto& choice = options[k];
            const auto& option = choice[combo % choice.size()];
            combo /= choice.size();
            if (option.kind == Kind::Fails) {
                if (step.failed_node < 0) step.failed_node = u;
                continue;
            }
            const auto& node = nodes_[static_cast<std::size_t>(u)];
            if (option.kind != Kind::VisitLimit) {
                ran.push_back(u);
                if (node.visit_slot >= 0) {
                    auto& visits = step.next.visits[static_cast<std::size_t>(node.visit_slot)];
                    visits       = static_cast<std::uint8_t>(std::min(visits + 1, node.max_visits));
                }
            }
            if (option.kind == Kind::Normal && option.failure_exit) failure_exit = true;
            for (const auto target : option.targets) {
                if (target == kEndTarget) {
                    explicit_end = true;
                    continue;
                }
                const int join = join_of_[static_cast<std::size_t>(target)];
                if (join < 0) {
                    set_bit(step.next.ready, target);
                } else {
                    set_bit(step.signals[static_cast<std::size_t>(join)], u);
                    signalled[static_cast<std::size_t>(join)] = 1;
                }
            }
        }
        for (const auto u : ran)
            for (const auto slot : nodes_[static_cast<std::size_t>(u)].resets)
                step.next.visits[static_cast<std::size_t>(slot)] = 0;
        for (std::size_t j = 0; j < joins_.size(); ++j) {
            if (!signalled[j]) continue;
            step.signalled.push_back(static_cast<int>(j));
            auto& pending = step.next.pending[j];
            for (std::size_t w = 0; w < words_; ++w) pending[w] |= step.signals[j][w];
            const auto& join   = joins_[j];
            const bool  filled = std::all_of(join.slot_bits.begin(), join.slot_bits.end(),
                                             [&](const Words& slot) { return intersects(slot, pending); });
            if (!filled) continue;
            set_bit(step.next.ready, join.node);
            std::fill(pending.begin(), pending.end(), 0);
            step.fired.push_back(static_cast<int>(j));
        }
        if (step.failed_node >= 0) {
            step.terminal = Terminal::Failed;
        } else if (failure_exit) {
            step.terminal = Terminal::FailureExit;
        } else if (no_bits(step.next.ready)) {
            const bool waiting = std::any_of(step.next.pending.begin(), step.next.pending.end(),
                                             [](const Words& pending) { return !no_bits(pending); });
            step.terminal = !waiting        ? Terminal::Completed
                            : explicit_end ? Terminal::CompletedOverPending
                                           : Terminal::Stalled;
        }
        return step;
    }

    std::uint32_t intern(std::string key, std::uint32_t parent, std::uint64_t combo) {
        const auto index         = static_cast<std::uint32_t>(keys_.size());
        const auto [found, _]    = lookup_.emplace(std::move(key), index);
        keys_.push_back(&found->first);
        StateRecord record;
        record.parent = parent;
        record.combo  = combo;
        records_.push_back(record);
        successors_.emplace_back();
        return index;
    }

    void explore() {
        State initial;
        initial.ready.assign(words_, 0);
        for (const auto u : start_) set_bit(initial.ready, u);
        initial.pending.assign(joins_.size(), Words(words_, 0));
        initial.visits.assign(visit_nodes_.size(), 0);
        intern(encode(initial), 0, 0);
        if (start_.empty()) {  // nothing runs: the run completes at once
            outcomes_.completed        = true;
            records_[0].complete_exit = records_[0].any_exit = true;
            return;
        }
        const std::uint64_t budget = static_cast<std::uint64_t>(std::max<std::size_t>(1, options_.state_cap)) *
                                     kCombosPerState;
        for (std::uint32_t i = 0; i < keys_.size() && !capped_; ++i) {
            const State state = decode(*keys_[i]);
            const auto  ready = ready_nodes(state);
            check_co_ready(i, ready);
            bool opaque = false;
            for (const auto u : ready) {
                if (nodes_[static_cast<std::size_t>(u)].control.empty()) continue;
                opaque = true;
                opaque_first_.emplace(u, i);
            }
            if (opaque) {
                records_[i].opaque = true;
                continue;
            }
            const auto          options   = options_for(state, ready);
            const std::uint64_t remaining = budget > transitions_ ? budget - transitions_ : 0;
            std::uint64_t       combos    = 1;
            bool                over      = false;
            for (const auto& choice : options) {
                if (combos > remaining / choice.size()) {
                    over = true;
                    break;
                }
                combos *= choice.size();
            }
            if (over || combos > remaining) {
                capped_ = true;
                break;
            }
            for (std::uint64_t combo = 0; combo < combos; ++combo) {
                ++transitions_;
                auto step = apply(state, ready, options, combo);
                for (const auto j : step.signalled) {
                    auto& received = received_[static_cast<std::size_t>(j)];
                    for (std::size_t w = 0; w < words_; ++w) received[w] |= step.signals[static_cast<std::size_t>(j)][w];
                    first_signal_.emplace(j, Arrival{i, combo});
                }
                for (const auto j : step.fired) fired_[static_cast<std::size_t>(j)] = true;
                if (step.terminal != Terminal::None) {
                    record_terminal(i, combo, step);
                    continue;
                }
                auto          key   = encode(step.next);
                const auto    found = lookup_.find(key);
                std::uint32_t next  = 0;
                if (found != lookup_.end()) {
                    next = found->second;
                } else {
                    if (keys_.size() >= options_.state_cap) {
                        capped_ = true;
                        break;
                    }
                    next = intern(std::move(key), i, combo);
                }
                successors_[i].push_back(next);
            }
            auto& successors = successors_[i];
            std::sort(successors.begin(), successors.end());
            successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
        }
    }

    void record_terminal(std::uint32_t state, std::uint64_t combo, const Transition& step) {
        auto& record    = records_[state];
        record.any_exit = true;
        switch (step.terminal) {
            case Terminal::Completed:
                outcomes_.completed  = true;
                record.complete_exit = true;
                break;
            case Terminal::CompletedOverPending:
                outcomes_.completed_over_pending = true;
                record.complete_exit             = true;
                record_pending_end(state, combo, step, "completed");
                break;
            case Terminal::Stalled:
                outcomes_.stalled = true;
                for (std::size_t j = 0; j < joins_.size(); ++j)
                    if (!no_bits(step.next.pending[j])) record.reasons |= stall_bit(j);
                record_pending_end(state, combo, step, "stalled");
                break;
            case Terminal::FailureExit:
                outcomes_.failure_exit = true;
                record.complete_exit   = true;
                break;
            case Terminal::Failed:
                outcomes_.failed = true;
                record.reasons |= kFailedBit;
                break;
            case Terminal::None:
                break;
        }
    }

    void record_pending_end(std::uint32_t state, std::uint64_t combo, const Transition& step,
                            const char* kind) {
        for (std::size_t j = 0; j < joins_.size(); ++j) {
            if (no_bits(step.next.pending[j])) continue;
            pending_ends_.emplace(std::pair{static_cast<int>(j), std::string(kind)},
                                  PendingEnd{{state, combo}, step.next.pending[j], kind});
        }
    }

    // S5: writers of one overwrite channel that are ready together, unless
    // every such pair is a direct fan-out pair, which E5 reports.
    void check_co_ready(std::uint32_t state, const std::vector<int>& ready) {
        if (!effects_known_ || ready.size() < 2) return;
        for (std::size_t c = 0; c < overwrite_channels_.size(); ++c) {
            if (co_ready_.count(c)) continue;
            std::vector<int> writers;
            for (const auto u : ready)
                if (bit_at(channel_writers_[c], u)) writers.push_back(u);
            bool beyond_e5 = false;
            for (std::size_t a = 0; a < writers.size() && !beyond_e5; ++a)
                for (std::size_t b = a + 1; b < writers.size() && !beyond_e5; ++b)
                    beyond_e5 = !e5_pairs_.count({writers[a], writers[b]});
            if (beyond_e5) co_ready_.emplace(c, std::pair{state, writers});
        }
    }

    // ---- analysis ---------------------------------------------------------

    // Strongly connected components of the explored state graph (Tarjan,
    // iterative). Components are numbered in reverse topological order.
    void components() {
        const auto n = keys_.size();
        component_.assign(n, -1);
        std::vector<int>                                    index(n, -1), low(n, 0);
        std::vector<char>                                   on_stack(n, 0);
        std::vector<std::uint32_t>                          stack;
        std::vector<std::pair<std::uint32_t, std::size_t>> calls;
        int                                                 counter = 0;
        for (std::uint32_t root = 0; root < n; ++root) {
            if (index[root] >= 0) continue;
            index[root] = low[root] = counter++;
            stack.push_back(root);
            on_stack[root] = 1;
            calls.push_back({root, 0});
            while (!calls.empty()) {
                const auto v = calls.back().first;
                if (calls.back().second < successors_[v].size()) {
                    const auto w = successors_[v][calls.back().second++];
                    if (index[w] < 0) {
                        index[w] = low[w] = counter++;
                        stack.push_back(w);
                        on_stack[w] = 1;
                        calls.push_back({w, 0});
                    } else if (on_stack[w]) {
                        low[v] = std::min(low[v], index[w]);
                    }
                    continue;
                }
                if (low[v] == index[v]) {
                    const int id = static_cast<int>(component_count_++);
                    while (true) {
                        const auto w = stack.back();
                        stack.pop_back();
                        on_stack[w]   = 0;
                        component_[w] = id;
                        if (w == v) break;
                    }
                }
                calls.pop_back();
                if (!calls.empty()) {
                    const auto parent = calls.back().first;
                    low[parent]       = std::min(low[parent], low[v]);
                }
            }
        }
    }

    // States from which a seed state is reachable.
    std::vector<char> backward_closure(const std::vector<char>& seeds) const {
        std::vector<char>          reached = seeds;
        std::vector<std::uint32_t> queue;
        for (std::uint32_t s = 0; s < reached.size(); ++s)
            if (reached[s]) queue.push_back(s);
        while (!queue.empty()) {
            const auto s = queue.back();
            queue.pop_back();
            for (const auto p : predecessors_[s]) {
                if (reached[p]) continue;
                reached[p] = 1;
                queue.push_back(p);
            }
        }
        return reached;
    }

    void analyze() {
        const auto n = keys_.size();
        predecessors_.assign(n, {});
        for (std::uint32_t s = 0; s < n; ++s)
            for (const auto t : successors_[s]) predecessors_[t].push_back(s);
        components();
        std::vector<std::size_t> sizes(component_count_, 0);
        for (std::uint32_t s = 0; s < n; ++s) ++sizes[static_cast<std::size_t>(component_[s])];
        for (std::uint32_t s = 0; s < n && !outcomes_.cyclic; ++s) {
            if (sizes[static_cast<std::size_t>(component_[s])] > 1) outcomes_.cyclic = true;
            for (const auto t : successors_[s])
                if (t == s) outcomes_.cyclic = true;
        }

        std::vector<char> complete_seeds(n, 0), end_seeds(n, 0);
        for (std::uint32_t s = 0; s < n; ++s) {
            // A state that is not analyzable may complete or end for all we know.
            complete_seeds[s] = records_[s].complete_exit || records_[s].opaque;
            end_seeds[s]      = records_[s].any_exit || records_[s].opaque;
        }
        can_complete_ = backward_closure(complete_seeds);
        can_end_      = backward_closure(end_seeds);

        // Reasons: the non-completing ends reachable from each state, plus the
        // loop bit when it can reach a state that never ends.
        reasons_.assign(n, 0);
        for (std::uint32_t s = 0; s < n; ++s)
            reasons_[s] = records_[s].reasons | (can_end_[s] ? 0 : kLoopBit);
        std::vector<std::uint32_t> queue;
        for (std::uint32_t s = 0; s < n; ++s)
            if (reasons_[s]) queue.push_back(s);
        while (!queue.empty()) {
            const auto s = queue.back();
            queue.pop_back();
            for (const auto p : predecessors_[s]) {
                const auto merged = reasons_[p] | reasons_[s];
                if (merged == reasons_[p]) continue;
                reasons_[p] = merged;
                queue.push_back(p);
            }
        }

        // S4: components that runs enter but never leave and in which they never end.
        std::vector<char> leaves(component_count_, 0);
        for (std::uint32_t s = 0; s < n; ++s) {
            if (records_[s].any_exit || records_[s].opaque) leaves[static_cast<std::size_t>(component_[s])] = 1;
            for (const auto t : successors_[s])
                if (component_[t] != component_[s]) leaves[static_cast<std::size_t>(component_[s])] = 1;
        }
        std::map<std::string, std::uint32_t> loops;  // loop nodes -> smallest entry state
        std::map<int, std::uint32_t>         entry_of;
        for (std::uint32_t s = 0; s < n; ++s) {
            const auto c = component_[s];
            if (leaves[static_cast<std::size_t>(c)]) continue;
            if (!entry_of.count(c)) entry_of[c] = s;
        }
        for (const auto& [c, entry] : entry_of) {
            Words nodes(words_, 0);
            for (std::uint32_t s = 0; s < n; ++s) {
                if (component_[s] != c) continue;
                const auto state = decode(*keys_[s]);
                for (std::size_t w = 0; w < words_; ++w) nodes[w] |= state.ready[w];
            }
            const std::string key(reinterpret_cast<const char*>(nodes.data()), words_ * sizeof(std::uint64_t));
            const auto found = loops.find(key);
            if (found == loops.end() || entry < found->second) loops[key] = entry;
        }
        for (const auto& [key, entry] : loops) {
            (void)key;
            livelocks_.push_back(entry);
        }
        std::sort(livelocks_.begin(), livelocks_.end());

        // S1: points of no return, grouped by what their continuations do.
        std::map<std::uint64_t, std::uint32_t> traps;
        for (std::uint32_t s = 0; s < n; ++s) {
            if (can_complete_[s] || (reasons_[s] & ~kLoopBit) == 0) continue;
            if (s != 0 && !can_complete_[records_[s].parent]) continue;
            traps.emplace(reasons_[s], s);  // states are in breadth-first order
        }
        for (const auto& [reasons, state] : traps) {
            (void)reasons;
            dead_ends_.push_back(state);
        }
        std::sort(dead_ends_.begin(), dead_ends_.end());
    }

    // ---- witnesses --------------------------------------------------------

    json step_json(std::uint32_t state, std::uint64_t combo) const {
        const auto decoded = decode(*keys_[state]);
        const auto ready   = ready_nodes(decoded);
        const auto options = options_for(decoded, ready);
        json       names   = json::array();
        json       routes  = json::object();
        for (std::size_t k = 0; k < ready.size(); ++k) {
            const auto& node = nodes_[static_cast<std::size_t>(ready[k])];
            names.push_back(node.name);
            const auto& choice = options[k];
            const auto& option = choice[combo % choice.size()];
            combo /= choice.size();
            if (!option.label.empty()) routes[node.name] = option.label;
        }
        return json{{"ready", std::move(names)}, {"routes", std::move(routes)}};
    }

    std::vector<Arrival> path_to(std::uint32_t state) const {
        std::vector<Arrival> path;
        for (auto s = state; s != 0; s = records_[s].parent) path.push_back({records_[s].parent, records_[s].combo});
        std::reverse(path.begin(), path.end());
        return path;
    }

    // The super-steps of `path` as a witness trace; long traces keep their end.
    void put_trace(json& witness, std::vector<Arrival> path, const char* field = "trace") const {
        if (path.size() > kMaxTraceSteps) {
            witness[std::string(field) + "_omitted_steps"] = path.size() - kMaxTraceSteps;
            path.erase(path.begin(), path.end() - static_cast<std::ptrdiff_t>(kMaxTraceSteps));
        }
        json steps = json::array();
        for (const auto& arrival : path) steps.push_back(step_json(arrival.state, arrival.combo));
        witness[field] = std::move(steps);
    }

    json names_of(const Words& bits) const {
        json result = json::array();
        for (std::size_t u = 0; u < nodes_.size(); ++u)
            if (bit_at(bits, static_cast<int>(u))) result.push_back(nodes_[u].name);
        return result;
    }

    json state_json(std::uint32_t index) const {
        const auto state   = decode(*keys_[index]);
        json       result  = json{{"ready", names_of(state.ready)}};
        json       pending = json::object();
        for (std::size_t j = 0; j < joins_.size(); ++j)
            if (!no_bits(state.pending[j]))
                pending[nodes_[static_cast<std::size_t>(joins_[j].node)].name] = names_of(state.pending[j]);
        if (!pending.empty()) result["pending"] = std::move(pending);
        json visits = json::object();
        for (std::size_t slot = 0; slot < visit_nodes_.size(); ++slot)
            if (state.visits[slot])
                visits[nodes_[static_cast<std::size_t>(visit_nodes_[slot])].name] = static_cast<int>(state.visits[slot]);
        if (!visits.empty()) result["visits"] = std::move(visits);
        return result;
    }

    // Labelled route choices of a trace, each once, with the Core pointer of
    // the edge that declares it.
    json choices_of(const json& steps) const {
        json                  choices = json::array();
        std::set<std::string> seen;
        for (const auto& step : steps) {
            for (const auto& [name, label] : step["routes"].items()) {
                const auto route = label.get<std::string>();
                if (!seen.insert(name + '\x1f' + route).second) continue;
                const auto& node    = nodes_[static_cast<std::size_t>(index_.at(name))];
                std::string pointer = node.route_pointer;
                if (route == "on_failure" || route == "visit_limit")
                    pointer = node.failure_pointer.empty() ? "/node_retry_policies/" + pointer_token(name)
                                                           : node.failure_pointer;
                choices.push_back(json{{"node", name}, {"route", route}, {"pointer", pointer}});
            }
        }
        return choices;
    }

    // "A -> B ('fail'), C -> FIX": the last few super-steps of a trace.
    static std::string describe(const json& steps) {
        std::string text;
        const auto  count = steps.size();
        const auto  first = count > kMessageSteps ? count - kMessageSteps : 0;
        if (first > 0) text = "... -> ";
        for (std::size_t i = first; i < count; ++i) {
            const auto& step = steps[i];
            if (i > first) text += " -> ";
            std::string ran;
            for (const auto& name : step["ready"]) {
                const auto node = name.get<std::string>();
                if (!ran.empty()) ran += ", ";
                ran += node;
                if (step["routes"].contains(node))
                    ran += " ('" + step["routes"][node].get<std::string>() + "')";
            }
            text += ran;
        }
        return text;
    }

    static std::string quoted(const std::vector<std::string>& names, const char* conjunction) {
        std::string text;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i > 0) text += i + 1 == names.size() ? std::string(" ") + conjunction + " " : ", ";
            text += "'" + names[i] + "'";
        }
        return text;
    }

    // The route combination that leads from one explored state to another.
    std::uint64_t combo_between(std::uint32_t from, std::uint32_t to) const {
        const auto state   = decode(*keys_[from]);
        const auto ready   = ready_nodes(state);
        const auto options = options_for(state, ready);
        std::uint64_t combos = 1;
        for (const auto& choice : options) combos *= choice.size();
        for (std::uint64_t combo = 0; combo < combos; ++combo) {
            const auto step = apply(state, ready, options, combo);
            if (step.terminal == Terminal::None && encode(step.next) == *keys_[to]) return combo;
        }
        return 0;
    }

    // The shortest cycle from `entry` back to itself inside its component.
    std::vector<Arrival> cycle_from(std::uint32_t entry) const {
        std::map<std::uint32_t, std::uint32_t> parent;
        std::vector<std::uint32_t>             queue{entry};
        std::optional<std::uint32_t>           closing;
        for (std::size_t head = 0; head < queue.size() && !closing; ++head) {
            const auto v = queue[head];
            for (const auto w : successors_[v]) {
                if (w == entry) {
                    closing = v;
                    break;
                }
                if (component_[w] != component_[entry] || parent.count(w)) continue;
                parent[w] = v;
                queue.push_back(w);
            }
        }
        std::vector<std::uint32_t> states{entry};
        if (closing) {
            std::vector<std::uint32_t> back;
            for (auto v = *closing; v != entry; v = parent.at(v)) back.push_back(v);
            states.insert(states.end(), back.rbegin(), back.rend());
        }
        std::vector<Arrival> cycle;
        for (std::size_t i = 0; i < states.size(); ++i) {
            const auto to = i + 1 < states.size() ? states[i + 1] : entry;
            cycle.push_back({states[i], combo_between(states[i], to)});
        }
        return cycle;
    }

    // ---- findings ---------------------------------------------------------

    Diagnostic warning(std::string code, std::string path, std::string pointer, std::string message,
                       json witness) const {
        return Diagnostic{std::move(code), "warning", std::move(path), std::move(message),
                          std::move(witness), std::move(pointer)};
    }

    std::string join_name(std::size_t join) const {
        return nodes_[static_cast<std::size_t>(joins_[join].node)].name;
    }

    std::vector<std::string> stall_joins(std::uint64_t reasons) const {
        std::vector<std::string> names;
        for (std::size_t j = 0; j < joins_.size(); ++j)
            if (reasons & stall_bit(j)) names.push_back(join_name(j));
        return names;
    }

    void add_dead_ends(std::vector<Diagnostic>& out) const {
        for (const auto state : dead_ends_) {
            const auto reasons = reasons_[state];
            json       witness = json::object();
            put_trace(witness, path_to(state));
            witness["state"]   = state_json(state);
            witness["choices"] = choices_of(witness["trace"]);
            json ends          = json::array();
            const auto stalls  = stall_joins(reasons);
            if (!stalls.empty()) {
                ends.push_back("stalled");
                witness["stalls_at"] = string_array(stalls);
            }
            if (reasons & kFailedBit) ends.push_back("failed");
            if (reasons & kLoopBit) ends.push_back("no_end");
            witness["ends"] = std::move(ends);
            std::vector<std::string> how;
            if (!stalls.empty()) how.push_back("ends barrier-stalled at " + quoted(stalls, "or"));
            if (reasons & kFailedBit) how.push_back("fails at a visit limit");
            if (reasons & kLoopBit) how.push_back("loops without an exit");
            std::string ways;
            for (std::size_t i = 0; i < how.size(); ++i)
                ways += (i == 0 ? "" : i + 1 == how.size() ? " or " : ", ") + how[i];
            const auto& steps   = witness["trace"];
            const auto  pointer = witness["choices"].empty()
                                      ? std::string()
                                      : witness["choices"][witness["choices"].size() - 1]["pointer"].get<std::string>();
            const auto  message = steps.empty()
                                      ? "No run can complete: every run " + ways
                                      : "After " + describe(steps) +
                                            " the run can no longer complete: every continuation " + ways;
            out.push_back(warning("S1", "$", pointer, message, std::move(witness)));
        }
    }

    void add_pending_ends(std::vector<Diagnostic>& out) const {
        for (const auto& [key, end] : pending_ends_) {
            const auto  j       = static_cast<std::size_t>(key.first);
            const auto& join    = joins_[j];
            const auto  name    = join_name(j);
            json        witness = json{{"barrier", name}, {"kind", end.kind}};
            auto        path    = path_to(end.at.state);
            path.push_back(end.at);
            put_trace(witness, std::move(path));
            witness["choices"]  = choices_of(witness["trace"]);
            witness["received"] = names_of(end.pending);
            std::vector<std::set<std::string>> missing;
            for (std::size_t k = 0; k < join.slots.size(); ++k)
                if (!intersects(join.slot_bits[k], end.pending)) missing.push_back(join.slots[k]);
            witness["missing"] = detail::print_wait_for(detail::canonical_slots(missing));
            // A stall that misses one slot while one node outside the slots
            // signalled: that node can stand in for the slot (the #357 fix).
            std::vector<std::string> outsiders;
            for (std::size_t u = 0; u < nodes_.size(); ++u)
                if (bit_at(end.pending, static_cast<int>(u)) && !bit_at(join.members, static_cast<int>(u)) &&
                    static_cast<int>(u) != join.node)
                    outsiders.push_back(nodes_[u].name);
            if (end.kind == "stalled" && missing.size() == 1 && outsiders.size() == 1) {
                auto slots = join.slots;
                for (auto& slot : slots)
                    if (slot == missing.front()) slot.insert(outsiders.front());
                witness["substitute"]         = outsiders.front();
                witness["suggested_wait_for"] = detail::print_wait_for(detail::canonical_slots(slots));
            }
            const auto& steps    = witness["trace"];
            const auto  received = witness["received"].dump();
            const auto  message =
                end.kind == "stalled"
                    ? "After " + describe(steps) + " the run ends barrier-stalled: barrier '" + name +
                          "' holds signals from " + received + " but misses " + witness["missing"].dump()
                    : "After " + describe(steps) + " the run reaches __end__ while barrier '" + name +
                          "' still holds signals from " + received +
                          "; the explicit end discards them and the run reports completed";
            out.push_back(warning("S2", "nodes." + name + ".barrier",
                                  "/nodes/" + pointer_token(name) + "/barrier", message, std::move(witness)));
        }
    }

    void add_dead_joins(std::vector<Diagnostic>& out) const {
        for (std::size_t j = 0; j < joins_.size(); ++j) {
            if (fired_[j]) continue;
            const auto& join    = joins_[j];
            const auto  name    = join_name(j);
            const auto& seen    = received_[j];
            json        witness = json{{"barrier", name}};
            std::vector<std::set<std::string>> silent;
            for (std::size_t k = 0; k < join.slots.size(); ++k)
                if (!intersects(join.slot_bits[k], seen)) silent.push_back(join.slots[k]);
            std::string message;
            if (no_bits(seen)) {
                witness["kind"] = "dead";
                message         = "Barrier '" + name + "' never fires: no run signals it";
            } else {
                witness["kind"]     = "unfireable";
                witness["received"] = names_of(seen);
                const auto first    = first_signal_.at(static_cast<int>(j));
                auto       path     = path_to(first.state);
                path.push_back(first);
                put_trace(witness, std::move(path));
                witness["choices"] = choices_of(witness["trace"]);
                message = "Barrier '" + name + "' never fires: no run fills all of its slots" +
                          (silent.empty() ? std::string(" together")
                                          : "; " + detail::print_wait_for(detail::canonical_slots(silent)).dump() +
                                                " never signal it");
            }
            witness["slots_never_filled"] = detail::print_wait_for(detail::canonical_slots(silent));
            out.push_back(warning("S3", "nodes." + name + ".barrier",
                                  "/nodes/" + pointer_token(name) + "/barrier", message, std::move(witness)));
        }
    }

    void add_livelocks(std::vector<Diagnostic>& out) const {
        for (const auto entry : livelocks_) {
            std::set<std::string> loop;
            for (std::uint32_t s = 0; s < keys_.size(); ++s) {
                if (component_[s] != component_[entry]) continue;
                for (const auto u : ready_nodes(decode(*keys_[s]))) loop.insert(nodes_[static_cast<std::size_t>(u)].name);
            }
            json witness = json{{"nodes", string_array(loop)}};
            put_trace(witness, path_to(entry));
            witness["state"] = state_json(entry);
            put_trace(witness, cycle_from(entry), "cycle");
            // Built step by step: a json taken from operator[] would write through.
            json all = json::array();
            for (const auto& step : witness["trace"]) all.push_back(step);
            for (const auto& step : witness["cycle"]) all.push_back(step);
            witness["choices"] = choices_of(all);
            const auto& steps  = witness["trace"];
            const std::vector<std::string> names(loop.begin(), loop.end());
            const auto message = (steps.empty() ? std::string("The run") : "After " + describe(steps) + " the run") +
                                 " enters a loop through " + quoted(names, "and") +
                                 " that it never leaves; it stops only at the step limit";
            const auto first = *loop.begin();
            out.push_back(warning("S4", "nodes." + first, "/nodes/" + pointer_token(first), message,
                                  std::move(witness)));
        }
    }

    void add_co_ready(std::vector<Diagnostic>& out) const {
        for (const auto& [c, found] : co_ready_) {
            const auto& [state, writers] = found;
            const auto&              channel = overwrite_channels_[c];
            std::vector<std::string> names;
            for (const auto u : writers) names.push_back(nodes_[static_cast<std::size_t>(u)].name);
            json witness = json{{"channel", channel}, {"writers", string_array(names)}};
            put_trace(witness, path_to(state));
            witness["state"]   = state_json(state);
            witness["choices"] = choices_of(witness["trace"]);
            const auto& steps  = witness["trace"];
            const auto  message =
                "Overwrite channel '" + channel + "' can be written by " + quoted(names, "and") +
                " in the same super-step" + (steps.empty() ? std::string() : " (after " + describe(steps) + ")") +
                "; last-writer-wins is nondeterministic under parallel dispatch, so use an append "
                "reducer or serialize the writers";
            out.push_back(warning("S5", "channels." + channel, "/channels/" + pointer_token(channel), message,
                                  std::move(witness)));
        }
    }

    SoundnessReport report() {
        SoundnessReport result;
        result.state_cap   = options_.state_cap;
        result.states      = keys_.size();
        result.transitions = static_cast<std::size_t>(transitions_);
        result.status      = capped_                 ? SoundnessStatus::NotChecked
                             : !opaque_first_.empty() ? SoundnessStatus::Partial
                                                      : SoundnessStatus::Checked;
        if (!capped_) analyze();
        result.outcomes = outcomes_;

        std::vector<Diagnostic> s1, s2, s3, s4, s5;
        if (!capped_) {
            add_dead_ends(s1);
            add_livelocks(s4);
            if (result.status == SoundnessStatus::Checked) add_dead_joins(s3);
        }
        add_pending_ends(s2);
        add_co_ready(s5);
        for (auto* group : {&s1, &s2, &s3, &s4, &s5})
            for (auto& finding : *group) result.findings.push_back(std::move(finding));

        json not_analyzable = json::array();
        for (const auto& [u, state] : opaque_first_) {
            const auto& node = nodes_[static_cast<std::size_t>(u)];
            result.not_analyzable.push_back(node.name);
            json entry = json{{"node", node.name}, {"type", node.type}, {"declares", string_array(node.control)}};
            put_trace(entry, path_to(state));
            not_analyzable.push_back(std::move(entry));
        }
        json outcomes = json::array();
        if (outcomes_.completed) outcomes.push_back("completed");
        if (outcomes_.completed_over_pending) outcomes.push_back("completed_over_pending");
        if (outcomes_.stalled) outcomes.push_back("stalled");
        if (outcomes_.failure_exit) outcomes.push_back("failure_exit");
        if (outcomes_.failed) outcomes.push_back("failed");
        if (outcomes_.cyclic) outcomes.push_back("step_limit");
        json counts = json::object();
        for (const auto* code : {"S1", "S2", "S3", "S4", "S5"}) {
            const auto count = std::count_if(result.findings.begin(), result.findings.end(),
                                             [&](const Diagnostic& d) { return d.code == code; });
            if (count > 0) counts[code] = static_cast<std::size_t>(count);
        }
        const char* status = result.status == SoundnessStatus::Checked   ? "checked"
                             : result.status == SoundnessStatus::Partial ? "partial"
                                                                         : "not_checked";
        json witness = json{{"status", status},
                            {"states", result.states},
                            {"transitions", result.transitions},
                            {"state_cap", result.state_cap},
                            {"outcomes", std::move(outcomes)},
                            {"findings", std::move(counts)}};
        if (!not_analyzable.empty()) witness["not_analyzable"] = std::move(not_analyzable);

        const auto  findings = result.findings.size();
        std::string tally    = findings == 0 ? "no findings" : std::to_string(findings) + " finding(s)";
        std::string message;
        if (result.status == SoundnessStatus::Checked) {
            message = "Soundness check explored every run: " + std::to_string(result.states) + " states, " +
                      std::to_string(result.transitions) + " transitions, " + tally;
        } else if (result.status == SoundnessStatus::Partial) {
            message = "Soundness check is partial: runs after " + quoted(result.not_analyzable, "and") +
                      " were not analyzable because their node types declare Command.goto or Send, so "
                      "findings that depend on them are not reported (" +
                      std::to_string(result.states) + " states explored, " + tally + ")";
        } else {
            message = "Soundness not checked: the state cap of " + std::to_string(result.state_cap) +
                      " was reached; only findings reached before it are reported (" + tally + ")";
        }
        result.summary = Diagnostic{"SOUNDNESS",
                                    result.status == SoundnessStatus::Checked ? "note" : "warning",
                                    "$",
                                    std::move(message),
                                    std::move(witness),
                                    ""};
        return result;
    }

    // ---- members ----------------------------------------------------------

    const TopologySpec&     topology_;
    const GraphRegistry&    registry_;
    const SoundnessOptions& options_;

    std::vector<NodeModel>     nodes_;
    std::map<std::string, int> index_;
    std::vector<JoinModel>     joins_;
    std::vector<int>           join_of_;
    std::vector<int>           start_;
    std::vector<int>           visit_nodes_;
    std::size_t                words_ = 1;

    bool                          effects_known_ = true;
    std::vector<std::string>      overwrite_channels_;
    std::vector<Words>            channel_writers_;
    std::set<std::pair<int, int>> e5_pairs_;

    std::unordered_map<std::string, std::uint32_t> lookup_;
    std::vector<const std::string*>                keys_;
    std::vector<StateRecord>                       records_;
    std::vector<std::vector<std::uint32_t>>        successors_;
    std::vector<std::vector<std::uint32_t>>        predecessors_;
    std::uint64_t                                  transitions_ = 0;
    bool                                           capped_      = false;

    SoundnessOutcomes                                       outcomes_;
    std::map<int, std::uint32_t>                            opaque_first_;
    std::map<std::pair<int, std::string>, PendingEnd>       pending_ends_;
    std::map<int, Arrival>                                  first_signal_;
    std::vector<Words>                                      received_;
    std::vector<bool>                                       fired_;
    std::map<std::size_t, std::pair<std::uint32_t, std::vector<int>>> co_ready_;

    std::vector<int>           component_;
    std::size_t                component_count_ = 0;
    std::vector<char>          can_complete_;
    std::vector<char>          can_end_;
    std::vector<std::uint64_t> reasons_;
    std::vector<std::uint32_t> dead_ends_;
    std::vector<std::uint32_t> livelocks_;
};

}  // namespace

SoundnessReport check_soundness(const TopologySpec&     topology,
                                const GraphRegistry&    registry,
                                const SoundnessOptions& options) {
    return Checker(topology, registry, options).run();
}

}  // namespace neograph::graph
