#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>

namespace neograph::graph {

CompiledGraph GraphCompiler::compile(const json& definition,
                                     const NodeContext& default_context) {
    CompiledGraph cg;
    cg.name = definition.value("name", "unnamed_graph");

    // --- Channels ---
    if (definition.contains("channels")) {
        for (const auto& [name, ch_def] : definition["channels"].items()) {
            ChannelDef cd;
            cd.name          = name;
            cd.reducer_name  = ch_def.value("reducer", "overwrite");
            cd.initial_value = ch_def.contains("initial") ? ch_def["initial"] : json();

            if (cd.reducer_name == "append")
                cd.type = ReducerType::APPEND;
            else if (cd.reducer_name == "overwrite")
                cd.type = ReducerType::OVERWRITE;
            else
                cd.type = ReducerType::CUSTOM;

            cg.channel_defs.push_back(std::move(cd));
        }
    }

    // --- Nodes + optional per-node barrier specs ---
    if (definition.contains("nodes")) {
        for (const auto& [name, node_def] : definition["nodes"].items()) {
            auto type = node_def.value("type", "");
            auto node = NodeFactory::instance().create(type, name, node_def, default_context);
            cg.nodes[name] = std::move(node);

            // {"barrier": {"wait_for": [...]}} opts this node into
            // AND-join semantics: it fires only after every listed
            // upstream has signaled (accumulated across super-steps).
            if (node_def.contains("barrier")) {
                const auto& b = node_def["barrier"];
                std::set<std::string> wait_for;
                if (b.contains("wait_for")) {
                    for (const auto& up : b["wait_for"]) {
                        wait_for.insert(up.get<std::string>());
                    }
                }
                if (!wait_for.empty()) {
                    cg.barrier_specs[name] = std::move(wait_for);
                }
            }
        }
    }

    // --- Edges (regular + conditional) ---
    if (definition.contains("edges")) {
        for (const auto& edge_def : definition["edges"]) {
            bool is_conditional = edge_def.contains("condition")
                               || edge_def.value("type", "") == "conditional";

            if (is_conditional) {
                ConditionalEdge ce;
                ce.from      = edge_def.at("from").get<std::string>();
                ce.condition = edge_def.at("condition").get<std::string>();
                if (edge_def.contains("routes")) {
                    for (const auto& [key, target] : edge_def["routes"].items()) {
                        ce.routes[key] = target.get<std::string>();
                    }
                }
                cg.conditional_edges.push_back(std::move(ce));
            } else {
                Edge e;
                e.from = edge_def.at("from").get<std::string>();
                e.to   = edge_def.at("to").get<std::string>();
                cg.edges.push_back(std::move(e));
            }
        }
    }

    // --- Interrupt sets ---
    if (definition.contains("interrupt_before")) {
        for (const auto& n : definition["interrupt_before"]) {
            cg.interrupt_before.insert(n.get<std::string>());
        }
    }
    if (definition.contains("interrupt_after")) {
        for (const auto& n : definition["interrupt_after"]) {
            cg.interrupt_after.insert(n.get<std::string>());
        }
    }

    // --- Retry policy (optional; engine uses its own default otherwise) ---
    if (definition.contains("retry_policy")) {
        auto rp = definition["retry_policy"];
        RetryPolicy policy;
        policy.max_retries        = rp.value("max_retries", 0);
        policy.initial_delay_ms   = rp.value("initial_delay_ms", 100);
        policy.backoff_multiplier = rp.value("backoff_multiplier", 2.0f);
        policy.max_delay_ms       = rp.value("max_delay_ms", 5000);
        cg.retry_policy = policy;
    }

    return cg;
}

} // namespace neograph::graph
