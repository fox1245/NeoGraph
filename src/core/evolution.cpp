#include <neograph/graph/evolution.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/loader.h>

#include <algorithm>
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

bool chance(int pct, std::mt19937& rng) {
    return std::uniform_int_distribution<int>(0, 99)(rng) < pct;
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

std::vector<std::string> object_keys_vec(const json& obj) {
    std::vector<std::string> keys;
    if (obj.is_object()) {
        for (auto it = obj.begin(); it != obj.end(); ++it)
            keys.push_back(it.key());
    }
    return keys;
}

// ── mutation operators ──────────────────────────────────────────────

MutationResult op_swap_template(const json& core, std::mt19937& rng) {
    if (!core.contains("templates") || !core["templates"].is_object())
        return {std::nullopt, "swap_template: no templates"};
    const auto& tmpls = core["templates"];
    auto tmpl_keys = object_keys_vec(tmpls);
    if (tmpl_keys.size() < 2)
        return {std::nullopt, "swap_template: need ≥2 templates"};
    if (!core.contains("use") || !core["use"].is_array() || core["use"].empty())
        return {std::nullopt, "swap_template: no use entries"};

    json mutated = deep_copy(core);
    size_t idx = std::uniform_int_distribution<size_t>(0, mutated["use"].size() - 1)(rng);

    // Read current use entry via items() emulation.
    std::string current;
    {
        json entry = mutated["use"][idx];
        current = entry["template"].get<std::string>();
    }

    size_t current_nparams = 0;
    if (tmpls.contains(current) && tmpls[current].contains("params") &&
        tmpls[current]["params"].is_array()) {
        current_nparams = tmpls[current]["params"].size();
    }

    std::vector<std::string> candidates;
    for (const auto& k : tmpl_keys) {
        if (k == current) continue;
        size_t nparams = 0;
        if (tmpls[k].contains("params") && tmpls[k]["params"].is_array())
            nparams = tmpls[k]["params"].size();
        if (nparams == current_nparams)
            candidates.push_back(k);
    }
    if (candidates.empty())
        return {std::nullopt, "swap_template: no compatible target"};

    std::string chosen = pick(candidates, rng);
    mutated["use"][idx]["template"] = chosen;
    mutated["use"][idx]["args"] = json::object();

    return {std::move(mutated),
            "swap_template: use[" + std::to_string(idx) + "] " +
                current + " → " + chosen};
}

MutationResult op_add_use(const json& core, std::mt19937& rng) {
    if (!core.contains("templates") || !core["templates"].is_object())
        return {std::nullopt, "add_use: no templates"};
    const auto& tmpls = core["templates"];
    auto tmpl_keys = object_keys_vec(tmpls);
    if (tmpl_keys.empty())
        return {std::nullopt, "add_use: empty templates"};

    json mutated = deep_copy(core);
    if (!mutated.contains("use") || !mutated["use"].is_array())
        mutated["use"] = json::array();

    const std::string& tname = pick(tmpl_keys, rng);
    const json tmpl = tmpls[tname];

    json new_use;
    new_use["template"] = tname;
    new_use["prefix"] = tname + "_" + std::to_string(mutated["use"].size());
    new_use["when"] = true;

    if (tmpl.contains("params") && tmpl["params"].is_array()) {
        json args = json::object();
        for (auto pit = tmpl["params"].begin(); pit != tmpl["params"].end(); ++pit) {
            std::string pname = (*pit).get<std::string>();
            args[pname] = "evolved_" + pname;
        }
        new_use["args"] = std::move(args);
    }

    mutated["use"].push_back(std::move(new_use));
    return {std::move(mutated),
            "add_use: template '" + tname + "' instantiated"};
}

MutationResult op_remove_use(const json& core, std::mt19937& rng) {
    if (!core.contains("use") || !core["use"].is_array())
        return {std::nullopt, "remove_use: no use array"};
    size_t n = core["use"].size();
    if (n <= 1)
        return {std::nullopt, "remove_use: need >1 use entries"};

    size_t idx = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
    json mutated = json::parse(core.dump());
    json new_uses = json::array();
    for (size_t i = 0; i < n; ++i) {
        if (i == idx) continue;
        new_uses.push_back(mutated["use"][i]);
    }
    mutated["use"] = std::move(new_uses);
    return {std::move(mutated),
            "remove_use: removed use[" + std::to_string(idx) + "]"};
}

MutationResult op_tune_param(const json& core, std::mt19937& rng) {
    if (!core.contains("use") || !core["use"].is_array() || core["use"].empty())
        return {std::nullopt, "tune_param: no use entries"};

    json mutated = deep_copy(core);
    size_t idx = std::uniform_int_distribution<size_t>(0, mutated["use"].size() - 1)(rng);
    json entry = mutated["use"][idx];
    if (!entry.contains("args") || !entry["args"].is_object() || entry["args"].empty())
        return {std::nullopt, "tune_param: no args in use[" + std::to_string(idx) + "]"};

    auto arg_keys = object_keys_vec(entry["args"]);
    const std::string& pname = pick(arg_keys, rng);
    json old_val = entry["args"][pname];
    std::string old_str = old_val.dump();

    if (old_val.is_string()) {
        std::string s = old_val.get<std::string>();
        if (s == "true")       mutated["use"][idx]["args"][pname] = "false";
        else if (s == "false") mutated["use"][idx]["args"][pname] = "true";
        else                   mutated["use"][idx]["args"][pname] = s + "_mut";
    } else if (old_val.is_number_integer()) {
        int v = old_val.get<int>();
        v += chance(50, rng) ? 1 : -1;
        mutated["use"][idx]["args"][pname] = std::max(0, v);
    } else if (old_val.is_number_float()) {
        double v = old_val.get<double>();
        v += chance(50, rng) ? 0.5 : -0.5;
        mutated["use"][idx]["args"][pname] = v;
    } else {
        return {std::nullopt, "tune_param: unsupported type for " + pname};
    }

    return {std::move(mutated),
            "tune_param: use[" + std::to_string(idx) + "]." + pname +
                " " + old_str + " → " + mutated["use"][idx]["args"][pname].dump()};
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

} // anonymous namespace

// =========================================================================
// Public API
// =========================================================================

std::vector<MutationOp> all_operators() {
    return {
        op_swap_template,
        op_add_use,
        op_remove_use,
        op_tune_param,
        op_toggle_conditional_edge,
        op_toggle_barrier,
        op_add_edge,
        op_remove_edge,
    };
}

Score evaluate(const json& core, const Task& task, const NodeContext& ctx) {
    Score s;
    s.compiled = false;
    s.validated = false;
    s.executed = false;
    s.correct = false;
    s.cost = -1.0;

    CompiledGraph cg;
    try {
        cg = GraphCompiler::compile(core, ctx);
        s.compiled = true;
    } catch (const std::exception& e) {
        s.summary = "compile failed: " + std::string(e.what());
        return s;
    }

    {
        auto report = GraphValidator::validate(cg);
        if (report.has_errors()) {
            s.summary = "validation failed: " + report.summary();
            return s;
        }
        s.validated = true;
    }

    s.executed = true;
    s.cost = 0.0;
    s.correct = true;
    s.summary = "gate passed";
    return s;
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
    seed_ind.score = evaluate(seed_core, task, ctx);
    seed_ind.core = deep_copy(seed_core);

    try {
        auto elab = Elaborator::elaborate(seed_core);
        seed_ind.core = std::move(elab.core);
        seed_ind.sourcemap = std::move(elab.sourcemap);
    } catch (...) {}

    result.population.push_back(std::move(seed_ind));

    for (int gen = 1; gen <= config.max_generations; ++gen) {
        std::vector<Individual> offspring;

        for (int i = 0; i < config.offspring_per_gen; ++i) {
            const auto& pop = result.population;
            size_t a = std::uniform_int_distribution<size_t>(0, pop.size() - 1)(rng);
            size_t b = std::uniform_int_distribution<size_t>(0, pop.size() - 1)(rng);
            const auto& parent = (pop[a].score.cost < pop[b].score.cost) ? pop[a] : pop[b];

            const auto& op = pick(ops, rng);
            auto mr = op(parent.core, rng);
            if (!mr.core) continue;
            result.total_offspring++;

            Individual child;
            child.core = std::move(*mr.core);
            child.generation = gen;
            child.parent_index = &parent - &pop[0];
            child.mutation_description = std::move(mr.description);

            CompiledGraph cg;
            try { cg = GraphCompiler::compile(child.core, ctx); }
            catch (...) { continue; }

            auto vr = GraphValidator::validate(cg);
            if (vr.has_errors()) continue;
            result.compile_passed++;

            if (config.run_evaluation) {
                child.score = evaluate(child.core, task, ctx);
                if (child.score.compiled && child.score.validated)
                    result.execute_passed++;
            } else {
                child.score.compiled = true;
                child.score.validated = true;
                child.score.executed = true;
                child.score.cost = 0.0;
                child.score.correct = true;
                child.score.summary = "compile gate passed";
            }

            offspring.push_back(std::move(child));
        }

        std::sort(offspring.begin(), offspring.end(),
                  [](const Individual& a, const Individual& b) {
                      return a.score.cost < b.score.cost;
                  });
        size_t keep = std::min<size_t>(config.survivors_per_gen, offspring.size());
        for (size_t i = 0; i < keep; ++i) {
            result.population.push_back(std::move(offspring[i]));
        }
    }

    auto best_it = std::min_element(
        result.population.begin(), result.population.end(),
        [](const Individual& a, const Individual& b) {
            return a.score.cost < b.score.cost;
        });
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
        entry["correct"] = ind.score.correct;
        entry["summary"] = ind.score.summary;
        pop.push_back(std::move(entry));
    }
    out["population"] = std::move(pop);

    json best = json::object();
    best["generation"] = result.best.generation;
    best["cost"] = result.best.score.cost;
    best["summary"] = result.best.score.summary;
    if (result.best.score.cost >= 0.0 && result.best.core.is_object()) {
        best["lockfile"] = result.best.core;
        if (result.best.sourcemap.is_object())
            best["sourcemap"] = result.best.sourcemap;
    }
    out["best"] = std::move(best);

    // Genealogy: population index -> lineage for every non-seed survivor.
    json lineages = json::array();
    for (size_t i = 0; i < result.population.size(); ++i) {
        const auto& ind = result.population[i];
        if (ind.generation == 0) continue;
        if (ind.score.cost < 0.0) continue;
        json lineage = json::object();
        lineage["index"] = static_cast<int64_t>(i);
        lineage["generation"] = ind.generation;
        lineage["parent_index"] = ind.parent_index;
        lineage["mutation"] = ind.mutation_description;
        if (ind.core.is_object())
            lineage["lockfile"] = ind.core;
        lineages.push_back(std::move(lineage));
    }
    out["genealogy"] = std::move(lineages);

    return out;
}

} // namespace neograph::graph