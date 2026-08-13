#include <neograph/graph/evolution.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neograph::graph {
namespace {

json deep_copy(const json& v) { return json::parse(v.dump()); }

template<typename T>
const T& pick(const std::vector<T>& vec, std::mt19937& rng) {
    return vec[std::uniform_int_distribution<size_t>(0, vec.size() - 1)(rng)];
}

std::vector<std::string> node_names(const json& core) {
    std::vector<std::string> names;
    if (core.contains("nodes") && core["nodes"].is_object()) {
        for (auto it = core["nodes"].begin(); it != core["nodes"].end(); ++it) {
            names.push_back(it.key());
        }
    }
    return names;
}

MutationResult op_toggle_conditional_edge(const json& core, std::mt19937& rng) {
    auto names = node_names(core);
    if (names.empty())
        return {std::nullopt, "toggle_ce: no nodes"};

    json mutated = deep_copy(core);
    std::string target = pick(names, rng);

    // Scan for existing CE from this node.
    bool found = false;
    if (mutated.contains("conditional_edges") &&
        mutated["conditional_edges"].is_array()) {
        size_t n = mutated["conditional_edges"].size();
        json new_ces = json::array();
        for (size_t i = 0; i < n; ++i) {
            json ce = mutated["conditional_edges"][i];
            if (ce.contains("from") && ce["from"] == target) {
                found = true;
            } else {
                new_ces.push_back(std::move(ce));
            }
        }
        mutated["conditional_edges"] = std::move(new_ces);
    }

    if (found) {
        return {std::move(mutated),
                "toggle_ce: removed conditional edge from " + target};
    }

    // No existing CE: add one.
    if (names.size() < 2) return {std::nullopt, "toggle_ce: need ≥2 nodes"};
    std::vector<std::string> targets;
    for (const auto& n : names) {
        if (n != target) targets.push_back(n);
    }

    json ce = json::object();
    ce["from"] = target;
    ce["condition"] = "route_channel";
    json routes = json::object();
    routes["default"] = pick(targets, rng);
    ce["routes"] = std::move(routes);
    if (!mutated.contains("conditional_edges") ||
        !mutated["conditional_edges"].is_array())
        mutated["conditional_edges"] = json::array();
    mutated["conditional_edges"].push_back(std::move(ce));

    return {std::move(mutated),
            "toggle_ce: added conditional edge from " + target};
}

MutationResult op_toggle_barrier(const json& core, std::mt19937& rng) {
    auto names = node_names(core);
    if (names.empty())
        return {std::nullopt, "toggle_barrier: no nodes"};

    json mutated = deep_copy(core);
    std::string target = pick(names, rng);

    // Check if the node already has a barrier in its config.
    bool has_barrier = mutated["nodes"].contains(target) &&
                       mutated["nodes"][target].contains("barrier");

    if (has_barrier) {
        // Remove the barrier from the node config.
        json node_obj = mutated["nodes"][target];
        json new_node = json::object();
        for (auto it = node_obj.begin(); it != node_obj.end(); ++it) {
            if (it.key() != "barrier")
                new_node[it.key()] = it.value();
        }
        mutated["nodes"][target] = std::move(new_node);
        return {std::move(mutated),
                "toggle_barrier: removed barrier on " + target};
    }

    // Find upstream nodes that route INTO target.
    std::vector<std::string> signalers;
    if (core.contains("edges") && core["edges"].is_array()) {
        size_t n = core["edges"].size();
        for (size_t i = 0; i < n; ++i) {
            json e = core["edges"][i];
            if (e.contains("to") && e["to"] == target &&
                e.contains("from")) {
                signalers.push_back(e["from"].get<std::string>());
            }
        }
    }
    if (core.contains("conditional_edges") &&
        core["conditional_edges"].is_array()) {
        size_t n = core["conditional_edges"].size();
        for (size_t i = 0; i < n; ++i) {
            json ce = core["conditional_edges"][i];
            if (ce.contains("routes") && ce["routes"].is_object()) {
                for (auto it = ce["routes"].begin();
                     it != ce["routes"].end(); ++it) {
                    if (it.value() == target) {
                        signalers.push_back(ce["from"].get<std::string>());
                    }
                }
            }
        }
    }

    // Filter out __start__ and non-existent names.
    {
        std::set<std::string> valid_names(names.begin(), names.end());
        signalers.erase(
            std::remove_if(signalers.begin(), signalers.end(),
                [&](const std::string& n) {
                    return n == target || n == "__start__" ||
                           valid_names.find(n) == valid_names.end();
                }),
            signalers.end());
    }

    if (signalers.size() < 1)
        return {std::nullopt, "toggle_barrier: no valid upstream signalers"};

    std::shuffle(signalers.begin(), signalers.end(), rng);
    size_t nw = std::uniform_int_distribution<size_t>(
        1, std::min(size_t(2), signalers.size()))(rng);
    signalers.resize(nw);

    json wait_arr = json::array();
    for (const auto& p : signalers) wait_arr.push_back(p);
    json barrier_entry = json::object();
    barrier_entry["wait_for"] = std::move(wait_arr);

    mutated["nodes"][target]["barrier"] = std::move(barrier_entry);

    return {std::move(mutated),
            "toggle_barrier: added barrier on " + target};
}

MutationResult op_add_edge(const json& core, std::mt19937& rng) {
    auto names = node_names(core);
    if (names.size() < 2)
        return {std::nullopt, "add_edge: need ≥2 nodes"};

    std::string from = pick(names, rng);
    std::vector<std::string> candidates;
    for (const auto& n : names) { if (n != from) candidates.push_back(n); }
    std::string to = pick(candidates, rng);

    json mutated = deep_copy(core);
    if (!mutated.contains("edges") || !mutated["edges"].is_array())
        mutated["edges"] = json::array();

    // Check duplicates.
    size_t n = mutated["edges"].size();
    for (size_t i = 0; i < n; ++i) {
        json e = mutated["edges"][i];
        if (e.contains("from") && e["from"].get<std::string>() == from &&
            e.contains("to") && e["to"].get<std::string>() == to)
            return {std::nullopt, "add_edge: duplicate edge " + from + "→" + to};
    }

    json edge = json::object();
    edge["from"] = from;
    edge["to"] = to;
    mutated["edges"].push_back(std::move(edge));

    return {std::move(mutated), "add_edge: " + from + " → " + to};
}

MutationResult op_remove_edge(const json& core, std::mt19937& rng) {
    if (!core.contains("edges") || !core["edges"].is_array())
        return {std::nullopt, "remove_edge: no edges array"};
    size_t n = core["edges"].size();
    if (n < 2)
        return {std::nullopt, "remove_edge: need >1 edges"};

    size_t idx = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
    json mutated = json::parse(core.dump());
    json new_edges = json::array();
    for (size_t i = 0; i < n; ++i) {
        if (i == idx) continue;
        new_edges.push_back(mutated["edges"][i]);
    }
    mutated["edges"] = std::move(new_edges);
    return {std::move(mutated),
            "remove_edge: removed edges[" + std::to_string(idx) + "]"};
}

Score evaluate_impl(const json& core, const Task& task, const NodeContext& ctx,
                    bool run_behavioral_evaluation) {
    Score score;

    CompiledGraph compiled_graph;
    try {
        compiled_graph = GraphCompiler::compile(core, ctx);
        score.compiled = true;
    } catch (const std::exception& e) {
        score.summary = "compile failed: " + std::string(e.what());
        return score;
    }

    auto report = GraphValidator::validate(compiled_graph);
    if (report.has_errors()) {
        score.summary = "validation failed: " + report.summary();
        return score;
    }
    score.validated = true;

    if (!run_behavioral_evaluation) {
        score.cost = 0.0;
        score.summary = "structural gate passed; behavior not evaluated";
        return score;
    }

    std::unique_ptr<GraphEngine> engine;
    try {
        engine = GraphEngine::link(std::move(compiled_graph));
    } catch (const std::exception& e) {
        score.summary = "execution setup failed: " + std::string(e.what());
        return score;
    }

    RunConfig run_config;
    run_config.input = task.input.is_null() ? json::object() : task.input;
    run_config.stream_mode = StreamMode::VALUES;

    int super_steps = 0;
    RunResult result;
    try {
        result = engine->run_stream(
            run_config,
            [&](const GraphEvent& event) {
                if (event.type == GraphEvent::Type::CHANNEL_WRITE &&
                    event.node_name == "__state__") {
                    ++super_steps;
                }
            });
        score.executed = true;
    } catch (const CancelledException& e) {
        score.summary = "execution cancelled: " + std::string(e.what());
        return score;
    } catch (const std::exception& e) {
        score.summary = "execution failed: " + std::string(e.what());
        return score;
    }

    if (result.status() == RunStatus::StepLimit) {
        score.summary = "execution timeout: max_steps exhausted after " +
                        std::to_string(super_steps) + " super-steps";
        return score;
    }
    if (result.status() == RunStatus::Interrupted) {
        score.summary = "execution interrupted at node '" +
                        result.interrupt_node + "'";
        return score;
    }
    if (!task.expected_output.is_object()) {
        score.summary =
            "evaluation failed: expected_output must be an object of channel values";
        return score;
    }

    std::vector<std::string> mismatches;
    for (auto it = task.expected_output.begin();
         it != task.expected_output.end(); ++it) {
        if (!result.has_channel(it.key())) {
            mismatches.push_back(it.key() + " (missing)");
            continue;
        }
        if (result.channel_raw(it.key()) != it.value()) {
            mismatches.push_back(it.key());
        }
    }

    double step_cost = 0.0;
    if (task.expected_super_steps > 0) {
        const int denominator = std::max(
            {1, task.expected_super_steps, super_steps});
        step_cost = static_cast<double>(std::abs(
                        super_steps - task.expected_super_steps)) /
                    static_cast<double>(denominator);
    }

    if (mismatches.empty()) {
        score.correct = true;
        score.cost = step_cost;
        score.summary = "behavior matched; super_steps=" +
                        std::to_string(super_steps);
        if (task.expected_super_steps > 0) {
            score.summary += ", expected=" +
                             std::to_string(task.expected_super_steps);
        }
        return score;
    }

    score.cost = 1.0 + static_cast<double>(mismatches.size()) + step_cost;
    score.summary = "output mismatch in channel";
    if (mismatches.size() != 1) score.summary += "s";
    score.summary += ": ";
    for (size_t i = 0; i < mismatches.size(); ++i) {
        if (i != 0) score.summary += ", ";
        score.summary += mismatches[i];
    }
    score.summary += "; super_steps=" + std::to_string(super_steps);
    return score;
}

bool score_less(const Individual& lhs, const Individual& rhs) {
    const bool lhs_runnable = lhs.score.cost >= 0.0;
    const bool rhs_runnable = rhs.score.cost >= 0.0;
    if (lhs_runnable != rhs_runnable) return lhs_runnable;
    if (lhs.score.cost != rhs.score.cost)
        return lhs.score.cost < rhs.score.cost;
    if (lhs.score.correct != rhs.score.correct)
        return lhs.score.correct;

    const std::string lhs_core = lhs.core.dump();
    const std::string rhs_core = rhs.core.dump();
    if (lhs_core != rhs_core) return lhs_core < rhs_core;
    if (lhs.generation != rhs.generation)
        return lhs.generation < rhs.generation;
    if (lhs.parent_index != rhs.parent_index)
        return lhs.parent_index < rhs.parent_index;
    return lhs.mutation_description < rhs.mutation_description;
}

} // anonymous namespace

// =========================================================================
// Public API
// =========================================================================

std::vector<MutationOp> all_operators() {
    return {
        op_toggle_conditional_edge,
        op_toggle_barrier,
        op_add_edge,
        op_remove_edge,
    };
}

Score evaluate(const json& core, const Task& task, const NodeContext& ctx) {
    return evaluate_impl(core, task, ctx, true);
}

EvolutionResult evolve(const json& seed_core, const Task& task,
                       const EvolutionConfig& config) {
    EvolutionResult result;
    auto ops = all_operators();
    std::mt19937 rng(config.seed);

    NodeContext ctx;

    Individual seed_ind;
    seed_ind.generation = 0;
    seed_ind.parent_index = -1;
    seed_ind.score = evaluate_impl(
        seed_core, task, ctx, config.run_evaluation);
    seed_ind.core = deep_copy(seed_core);

    result.population.push_back(std::move(seed_ind));

    for (int gen = 1; gen <= config.max_generations; ++gen) {
        std::vector<Individual> offspring;

        for (int i = 0; i < config.offspring_per_gen; ++i) {
            const auto& pop = result.population;
            size_t a = std::uniform_int_distribution<size_t>(0, pop.size() - 1)(rng);
            size_t b = std::uniform_int_distribution<size_t>(0, pop.size() - 1)(rng);
            const auto& parent = score_less(pop[a], pop[b]) ? pop[a] : pop[b];

            const auto& op = pick(ops, rng);
            auto mr = op(parent.core, rng);
            if (!mr.core) continue;
            result.total_offspring++;

            Individual child;
            child.core = std::move(*mr.core);
            child.generation = gen;
            child.parent_index = &parent - &pop[0];
            child.mutation_description = std::move(mr.description);

            child.score = evaluate_impl(
                child.core, task, ctx, config.run_evaluation);
            if (!child.score.compiled || !child.score.validated) continue;
            result.compile_passed++;

            if (config.run_evaluation) {
                if (child.score.executed && child.score.cost >= 0.0)
                    result.execute_passed++;
            }

            offspring.push_back(std::move(child));
        }

        std::sort(offspring.begin(), offspring.end(), score_less);
        size_t keep = std::min<size_t>(config.survivors_per_gen, offspring.size());
        for (size_t i = 0; i < keep; ++i) {
            result.population.push_back(std::move(offspring[i]));
        }
    }

    auto best_it = std::min_element(
        result.population.begin(), result.population.end(),
        score_less);
    if (best_it != result.population.end())
        result.best = *best_it;

    return result;
}

json to_json(const EvolutionResult& result) {
    json out = json::object();
    out["total_offspring"] = result.total_offspring;
    out["compile_passed"] = result.compile_passed;
    out["execute_passed"] = result.execute_passed;

    json pop = json::array();
    for (const auto& ind : result.population) {
        json entry = json::object();
        entry["generation"] = ind.generation;
        entry["parent_index"] = ind.parent_index;
        entry["mutation"] = ind.mutation_description;
        entry["cost"] = ind.score.cost;
        entry["compiled"] = ind.score.compiled;
        entry["validated"] = ind.score.validated;
        entry["executed"] = ind.score.executed;
        entry["correct"] = ind.score.correct;
        entry["summary"] = ind.score.summary;
        pop.push_back(std::move(entry));
    }
    out["population"] = std::move(pop);

    json best = json::object();
    best["generation"] = result.best.generation;
    best["cost"] = result.best.score.cost;
    best["summary"] = result.best.score.summary;
    best["compiled"] = result.best.score.compiled;
    best["validated"] = result.best.score.validated;
    best["executed"] = result.best.score.executed;
    best["correct"] = result.best.score.correct;
    if (result.best.score.cost >= 0.0 && result.best.core.is_object()) {
        best["lockfile"] = result.best.core;
    }
    out["best"] = std::move(best);

    // Genealogy: population index -> lineage for every non-seed survivor.
    json lineages = json::array();
    for (size_t i = 0; i < result.population.size(); ++i) {
        const auto& ind = result.population[i];
        if (ind.generation == 0) continue;
        json lineage = json::object();
        lineage["index"] = static_cast<int64_t>(i);
        lineage["generation"] = ind.generation;
        lineage["parent_index"] = ind.parent_index;
        lineage["mutation"] = ind.mutation_description;
        lineage["cost"] = ind.score.cost;
        lineage["executed"] = ind.score.executed;
        lineage["correct"] = ind.score.correct;
        if (ind.core.is_object())
            lineage["lockfile"] = ind.core;
        lineages.push_back(std::move(lineage));
    }
    out["genealogy"] = std::move(lineages);

    return out;
}

} // namespace neograph::graph
