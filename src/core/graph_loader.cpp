#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <vector>
#include <string>

// Stamped into export_schema() so external tooling can detect when its
// cached schema is older than the engine. Defined by CMake
// (target_compile_definitions); the fallback keeps a stray TU compiling.
#ifndef NEOGRAPH_VERSION_STR
#define NEOGRAPH_VERSION_STR "unknown"
#endif

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

// Sorted key list from any registry map. Backs both the "Unknown
// <thing>: foo" error messages (joined to a comma string) and the
// public names()/registered_types() introspection accessors used by
// external tooling. One source of truth for "what IS registered".
template <typename Map>
static std::vector<std::string> registry_names(const Map& m) {
    std::vector<std::string> names;
    names.reserve(m.size());
    for (const auto& kv : m) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

// DX helper: comma-separated sorted names. Used by the "Unknown
// <thing>: foo" error messages so users see what IS available without
// having to grep the source.
template <typename Map>
static std::string registry_name_list(const Map& m) {
    std::string out;
    for (const auto& n : registry_names(m)) {
        if (!out.empty()) out += ", ";
        out += n;
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

std::vector<std::string> ReducerRegistry::names() const {
    return registry_names(registry_);
}

// =========================================================================
// ConditionRegistry
// =========================================================================

ConditionRegistry& ConditionRegistry::instance() {
    static ConditionRegistry inst;
    return inst;
}

ConditionRegistry::ConditionRegistry() {
    // Built-in: has_tool_calls — closed contract: exactly true/false.
    register_condition("has_tool_calls",
        [](const GraphState& state) -> std::string {
            auto messages = state.get_messages();
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == "assistant") {
                    return it->tool_calls.empty() ? "false" : "true";
                }
            }
            return "false";
        },
        ConditionSpec{{"false", "true"}, /*open=*/false});

    // Built-in: route_channel
    // Reads the "__route__" channel and returns its string value.
    // Used with IntentClassifierNode for dynamic intent-based routing.
    // Open contract: returns arbitrary channel content — but "default"
    // is a KNOWN label (returned whenever __route__ is missing or not
    // a string), so the validator can warn when it is unrouted.
    register_condition("route_channel",
        [](const GraphState& state) -> std::string {
            auto route = state.get("__route__");
            if (route.is_string()) return route.get<std::string>();
            return "default";
        },
        ConditionSpec{{"default"}, /*open=*/true});
}

void ConditionRegistry::register_condition(const std::string& name, ConditionFn fn) {
    registry_[name] = std::move(fn);
    specs_.erase(name);   // re-registration without a spec clears the old one
}

void ConditionRegistry::register_condition(const std::string& name, ConditionFn fn,
                                           ConditionSpec spec) {
    registry_[name] = std::move(fn);
    specs_[name]    = std::move(spec);
}

std::optional<ConditionSpec> ConditionRegistry::condition_spec(
    const std::string& name) const {
    auto it = specs_.find(name);
    if (it == specs_.end()) return std::nullopt;
    return it->second;
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

std::vector<std::string> ConditionRegistry::names() const {
    return registry_names(registry_);
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
        },
        json::parse(R"JSON({
            "type": "object",
            "properties": {},
            "description": "LLM call node. Uses the NodeContext provider/model; no type-specific config fields."
        })JSON"),
        json::parse(R"JSON({"reads":["messages"],"writes":["messages"]})JSON"));

    register_type("tool_dispatch",
        [](const std::string& name, const json& /*config*/,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            return std::make_unique<ToolDispatchNode>(name, ctx);
        },
        json::parse(R"JSON({
            "type": "object",
            "properties": {},
            "description": "Executes tool calls from the last assistant message using NodeContext tools; no type-specific config fields."
        })JSON"),
        json::parse(R"JSON({"reads":["messages"],"writes":["messages"]})JSON"));

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
        },
        json::parse(R"JSON({
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "Classification prompt shown to the LLM."
                },
                "routes": {
                    "type": "array",
                    "items": { "type": "string" },
                    "description": "Allowed route keys the classifier may emit. Written to the __route__ channel; pair with the route_channel condition on an outgoing conditional edge."
                }
            },
            "required": ["routes"]
        })JSON"),
        json::parse(R"JSON({"reads":["messages"],"writes":["__route__"]})JSON"));

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
        },
        json::parse(R"JSON({
            "type": "object",
            "properties": {
                "definition": {
                    "description": "Inner graph: an inline topology object, or a string path to a .json topology file.",
                    "oneOf": [ { "type": "object" }, { "type": "string" } ]
                },
                "input_map": {
                    "type": "object",
                    "additionalProperties": { "type": "string" },
                    "description": "Map outer channel name -> inner channel name for inputs."
                },
                "output_map": {
                    "type": "object",
                    "additionalProperties": { "type": "string" },
                    "description": "Map inner channel name -> outer channel name for outputs."
                }
            },
            "required": ["definition"]
        })JSON"));
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn) {
    // Permissive default: any config object accepted. Tooling that
    // consumes export_schema() will render a free-form config for a
    // type registered without a declared schema.
    register_type(type, std::move(fn),
                  json::parse(R"JSON({
                      "type": "object",
                      "description": "No declared config schema; any object accepted."
                  })JSON"));
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn,
                                json config_schema) {
    registry_[type] = std::move(fn);
    schemas_[type]  = std::move(config_schema);
    effects_.erase(type);   // re-registration without effects clears them
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn,
                                json config_schema, json effects) {
    registry_[type] = std::move(fn);
    schemas_[type]  = std::move(config_schema);
    effects_[type]  = std::move(effects);
}

json NodeFactory::node_effects(const std::string& type) const {
    auto it = effects_.find(type);
    if (it != effects_.end()) return it->second;
    return json();   // null: no declared effect contract
}

std::vector<std::string> NodeFactory::registered_types() const {
    return registry_names(registry_);
}

json NodeFactory::config_schema(const std::string& type) const {
    auto it = schemas_.find(type);
    if (it != schemas_.end()) return it->second;
    return json::parse(R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
}

json NodeFactory::export_schema() const {
    // Fixed top-level envelope. This mirrors exactly what
    // GraphCompiler::compile reads (src/core/graph_compiler.cpp);
    // keep the two in sync when the loader grows new top-level keys.
    static const char* kTopologySchema = R"JSON({
        "type": "object",
        "description": "NeoGraph topology definition consumed by GraphEngine::compile / the JSON loader.",
        "properties": {
            "schema_version": { "type": "integer", "description": "Topology schema version. 1 opts this document into strict compilation: every key must be consumed by the parser (unknown keys, typos, and silently-droppable constructs are hard errors), and translation validation failures throw instead of warning. Absent or 0 = legacy lenient parsing. Keys prefixed '_' or 'x-' are annotations and always allowed." },
            "name": { "type": "string", "description": "Optional graph name." },
            "channels": {
                "type": "object",
                "description": "State channels. Key = channel name.",
                "additionalProperties": {
                    "type": "object",
                    "properties": {
                        "reducer": { "type": "string", "default": "overwrite", "description": "Reducer name (see top-level 'reducers')." },
                        "initial": { "description": "Initial value (any JSON)." }
                    }
                }
            },
            "nodes": {
                "type": "object",
                "description": "Graph nodes. Key = unique node name. 'type' selects a node type from 'node_types'; remaining fields are that type's config.",
                "additionalProperties": {
                    "type": "object",
                    "properties": {
                        "type": { "type": "string", "description": "Node type name (key in 'node_types')." },
                        "barrier": {
                            "type": "object",
                            "description": "Opt into AND-join: fire only after every listed upstream node has signaled.",
                            "properties": {
                                "wait_for": { "type": "array", "items": { "type": "string" } }
                            }
                        }
                    },
                    "required": ["type"]
                }
            },
            "edges": {
                "type": "array",
                "description": "Edges. '__start__' and '__end__' are sentinel endpoints. An edge carrying a 'condition' (or type:'conditional') is a conditional edge (legacy inline form).",
                "items": {
                    "type": "object",
                    "properties": {
                        "from": { "type": "string" },
                        "to": { "type": "string" },
                        "type": { "type": "string", "enum": ["conditional"] },
                        "condition": { "type": "string", "description": "Condition name (see 'conditions'); makes this a branch." },
                        "routes": { "type": "object", "additionalProperties": { "type": "string" }, "description": "route key -> target node name." }
                    },
                    "required": ["from"]
                }
            },
            "conditional_edges": {
                "type": "array",
                "description": "Top-level conditional (branch) edges; LangGraph add_conditional_edges parity. NOTE: a compiler regression silently dropped this block in v0.1.0-v0.1.7 (fixed v0.1.8) — tooling round-trip tests MUST assert these survive loader->compile.",
                "items": {
                    "type": "object",
                    "properties": {
                        "from": { "type": "string" },
                        "condition": { "type": "string", "description": "Condition name (see 'conditions')." },
                        "routes": { "type": "object", "additionalProperties": { "type": "string" }, "description": "route key -> target node name." }
                    },
                    "required": ["from", "condition"]
                }
            },
            "interrupt_before": {
                "type": "array", "items": { "type": "string" },
                "description": "Node names to pause before (human-in-the-loop / checkpoint resume point)."
            },
            "interrupt_after": {
                "type": "array", "items": { "type": "string" },
                "description": "Node names to pause after (human-in-the-loop / checkpoint resume point)."
            },
            "retry_policy": {
                "type": "object",
                "description": "Optional engine retry-policy override."
            }
        },
        "required": ["nodes"]
    })JSON";

    json node_types = json::object();
    for (const auto& kv : registry_) {
        auto sit = schemas_.find(kv.first);
        node_types[kv.first] = (sit != schemas_.end())
            ? sit->second
            : json::parse(R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
    }

    // Per-type channel-effect contracts (only types that declared one).
    json node_effects = json::object();
    for (const auto& kv : registry_) {
        auto eit = effects_.find(kv.first);
        if (eit != effects_.end()) node_effects[kv.first] = eit->second;
    }

    // Per-condition output-label contracts (only conditions that
    // declared one). `conditions` stays a plain name array for
    // backward compatibility with existing tooling.
    json condition_specs = json::object();
    auto& creg = ConditionRegistry::instance();
    for (const auto& cname : creg.names()) {
        if (auto spec = creg.condition_spec(cname)) {
            json labels = json::array();
            for (const auto& l : spec->labels) labels.push_back(l);
            condition_specs[cname] = json{{"labels", std::move(labels)},
                                          {"open", spec->open}};
        }
    }

    json doc;
    doc["neograph_version"] = NEOGRAPH_VERSION_STR;
    doc["$schema"]          = "https://json-schema.org/draft/2020-12/schema";
    doc["topology"]         = json::parse(kTopologySchema);
    doc["node_types"]       = std::move(node_types);
    doc["node_effects"]     = std::move(node_effects);
    doc["reducers"]         = ReducerRegistry::instance().names();
    doc["conditions"]       = creg.names();
    doc["condition_specs"]  = std::move(condition_specs);
    return doc;
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
