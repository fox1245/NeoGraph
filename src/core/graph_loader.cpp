#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <vector>

namespace neograph::graph {

// =========================================================================
// Built-in reducer functions
// =========================================================================

static json reducer_overwrite(const json& /*current*/, const json& incoming) {
    return incoming;
}

static json reducer_append(const json& current, const json& incoming) {
    json result = current.is_array() ? current : json::array();
    if (incoming.is_array()) {
        for (const auto& item : incoming) result.push_back(item);
    } else {
        result.push_back(incoming);
    }
    return result;
}

// =========================================================================
// ReducerRegistry
// =========================================================================

ReducerRegistry& ReducerRegistry::instance() {
    static ReducerRegistry inst;
    return inst;
}

ReducerRegistry::ReducerRegistry() {
    register_reducer("overwrite", reducer_overwrite);
    register_reducer("append",   reducer_append);
}

void ReducerRegistry::register_reducer(const std::string& name, ReducerFn fn) {
    registry_[name] = std::move(fn);
}

// DX helper: comma-separated sorted names from a registry map. Used by the
// "Unknown <thing>: foo" error messages so users see what IS available
// without having to grep the source. Defined as a template so the same
// shape works for ReducerRegistry / ConditionRegistry / NodeFactory's
// different value types.
template <typename Map>
static std::string registry_name_list(const Map& m) {
    std::vector<std::string> names;
    names.reserve(m.size());
    for (const auto& kv : m) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

ReducerFn ReducerRegistry::get(const std::string& name) const {
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        throw std::runtime_error(
            "Unknown reducer: '" + name + "'. "
            "Available: " + registry_name_list(registry_) + ". "
            "Register a custom reducer before compile via "
            "ReducerRegistry::instance().register_reducer(name, fn). "
            "See docs/troubleshooting.md \"Unknown reducer\".");
    }
    return it->second;
}

// =========================================================================
// ConditionRegistry
// =========================================================================

ConditionRegistry& ConditionRegistry::instance() {
    static ConditionRegistry inst;
    return inst;
}

ConditionRegistry::ConditionRegistry() {
    // Built-in: has_tool_calls
    register_condition("has_tool_calls",
        [](const GraphState& state) -> std::string {
            auto messages = state.get_messages();
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == "assistant") {
                    return it->tool_calls.empty() ? "false" : "true";
                }
            }
            return "false";
        });

    // Built-in: route_channel
    // Reads the "__route__" channel and returns its string value.
    // Used with IntentClassifierNode for dynamic intent-based routing.
    register_condition("route_channel",
        [](const GraphState& state) -> std::string {
            auto route = state.get("__route__");
            if (route.is_string()) return route.get<std::string>();
            return "default";
        });
}

void ConditionRegistry::register_condition(const std::string& name, ConditionFn fn) {
    registry_[name] = std::move(fn);
}

ConditionFn ConditionRegistry::get(const std::string& name) const {
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        throw std::runtime_error(
            "Unknown condition: '" + name + "'. "
            "Available: " + registry_name_list(registry_) + ". "
            "Register a custom condition before compile via "
            "ConditionRegistry::instance().register_condition(name, fn). "
            "See docs/troubleshooting.md \"Unknown condition\".");
    }
    return it->second;
}

// =========================================================================
// NodeFactory
// =========================================================================

NodeFactory& NodeFactory::instance() {
    static NodeFactory inst;
    return inst;
}

NodeFactory::NodeFactory() {
    register_type("llm_call",
        [](const std::string& name, const json& /*config*/,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            return std::make_unique<LLMCallNode>(name, ctx);
        });

    register_type("tool_dispatch",
        [](const std::string& name, const json& /*config*/,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            return std::make_unique<ToolDispatchNode>(name, ctx);
        });

    // intent_classifier: LLM-based intent routing
    register_type("intent_classifier",
        [](const std::string& name, const json& config,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            std::string prompt = config.value("prompt", "");
            std::vector<std::string> routes;
            if (config.contains("routes") && config["routes"].is_array()) {
                for (const auto& r : config["routes"]) {
                    routes.push_back(r.get<std::string>());
                }
            }
            return std::make_unique<IntentClassifierNode>(
                name, ctx, prompt, std::move(routes));
        });

    // subgraph: recursively compile an inner graph definition as a single node
    // Supports both inline JSON and external file path:
    //   "definition": { ... }           (inline)
    //   "definition": "path/to/file.json"  (external file)
    register_type("subgraph",
        [](const std::string& name, const json& config,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            if (!config.contains("definition")) {
                throw std::runtime_error(
                    "subgraph node '" + name + "': missing 'definition' field");
            }

            // Load definition: inline JSON or external file path
            json definition;
            if (config["definition"].is_string()) {
                std::string path = config["definition"].get<std::string>();
                std::ifstream f(path);
                if (!f.is_open()) {
                    throw std::runtime_error(
                        "subgraph node '" + name + "': cannot open file: " + path);
                }
                definition = json::parse(f);
            } else {
                definition = config["definition"];
            }

            // Compile inner graph with parent's context
            auto inner = GraphEngine::compile(definition, ctx);

            // Parse optional channel mappings
            std::map<std::string, std::string> input_map, output_map;
            if (config.contains("input_map")) {
                for (const auto& [k, v] : config["input_map"].items()) {
                    input_map[k] = v.get<std::string>();
                }
            }
            if (config.contains("output_map")) {
                for (const auto& [k, v] : config["output_map"].items()) {
                    output_map[k] = v.get<std::string>();
                }
            }

            return std::make_unique<SubgraphNode>(
                name,
                std::shared_ptr<GraphEngine>(inner.release()),
                std::move(input_map),
                std::move(output_map));
        });
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn) {
    registry_[type] = std::move(fn);
}

std::unique_ptr<GraphNode> NodeFactory::create(
    const std::string& type,
    const std::string& name,
    const json& config,
    const NodeContext& ctx) const {

    auto it = registry_.find(type);
    if (it == registry_.end()) {
        throw std::runtime_error(
            "Unknown node type: '" + type + "' (referenced by node '" + name + "'). "
            "Available: " + registry_name_list(registry_) + ". "
            "Register a custom type before compile via "
            "NodeFactory::instance().register_type(type, factory). "
            "See docs/troubleshooting.md \"Unknown node type\".");
    }
    return it->second(name, config, ctx);
}

} // namespace neograph::graph
