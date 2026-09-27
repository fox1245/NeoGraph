#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/state.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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
        for (const auto& item : incoming)
            result.push_back(item);
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
    std::lock_guard lock(mutex_);
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
    for (const auto& kv : m)
        names.push_back(kv.first);
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
    std::lock_guard lock(mutex_);
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        throw std::runtime_error("Unknown reducer: '" + name +
                                 "'. "
                                 "Available: " +
                                 registry_name_list(registry_) +
                                 ". "
            "Register a custom reducer before compile via "
            "ReducerRegistry::instance().register_reducer(name, fn). "
            "See docs/troubleshooting.md \"Unknown reducer\".");
    }
    return it->second;
}

std::vector<std::string> ReducerRegistry::names() const {
    std::lock_guard lock(mutex_);
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
    register_condition(
        "has_tool_calls",
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
    register_condition(
        "route_channel",
        [](const GraphState& state) -> std::string {
            auto route = state.get("__route__");
            if (route.is_string()) return route.get<std::string>();
            return "default";
        },
        ConditionSpec{{"default"}, /*open=*/true});
}

void ConditionRegistry::register_condition(const std::string& name, ConditionFn fn) {
    std::lock_guard lock(mutex_);
    registry_[name] = std::move(fn);
    specs_.erase(name);   // re-registration without a spec clears the old one
}

void ConditionRegistry::register_condition(const std::string& name,
                                           ConditionFn        fn,
                                           ConditionSpec spec) {
    std::lock_guard lock(mutex_);
    registry_[name] = std::move(fn);
    specs_[name]    = std::move(spec);
}

std::optional<ConditionSpec> ConditionRegistry::condition_spec(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = specs_.find(name);
    if (it == specs_.end()) return std::nullopt;
    return it->second;
}

ConditionFn ConditionRegistry::get(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        throw std::runtime_error("Unknown condition: '" + name +
                                 "'. "
                                 "Available: " +
                                 registry_name_list(registry_) +
                                 ". "
            "Register a custom condition before compile via "
            "ConditionRegistry::instance().register_condition(name, fn). "
            "See docs/troubleshooting.md \"Unknown condition\".");
    }
    return it->second;
}

std::vector<std::string> ConditionRegistry::names() const {
    std::lock_guard lock(mutex_);
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
    register_type(
        "llm_call",
        [](const std::string& name, const json& /*config*/, const NodeContext& ctx)
            -> std::unique_ptr<GraphNode> { return std::make_unique<LLMCallNode>(name, ctx); },
        json::parse(R"JSON({
            "type": "object",
            "properties": {},
            "description": "LLM call node. Uses the NodeContext provider/model; no type-specific config fields."
        })JSON"),
        json::parse(R"JSON({"reads":["messages"],"writes":["messages"]})JSON"));

    register_type(
        "tool_dispatch",
        [](const std::string& name, const json& /*config*/, const NodeContext& ctx)
            -> std::unique_ptr<GraphNode> { return std::make_unique<ToolDispatchNode>(name, ctx); },
        json::parse(R"JSON({
            "type": "object",
            "properties": {},
            "description": "Executes tool calls from the last assistant message using NodeContext tools; no type-specific config fields."
        })JSON"),
        json::parse(R"JSON({"reads":["messages"],"writes":["messages"]})JSON"));

    // intent_classifier: LLM-based intent routing
    register_type(
        "intent_classifier",
        [](const std::string& name, const json& config,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            std::string prompt = config.value("prompt", "");
            std::vector<std::string> routes;
            if (config.contains("routes") && config["routes"].is_array()) {
                for (const auto& r : config["routes"]) {
                    routes.push_back(r.get<std::string>());
                }
            }
            return std::make_unique<IntentClassifierNode>(name, ctx, prompt, std::move(routes));
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
    register_type(
        "subgraph",
        [](const std::string& name, const json& config,
           const NodeContext& ctx) -> std::unique_ptr<GraphNode> {
            if (!config.contains("definition")) {
                throw std::runtime_error("subgraph node '" + name +
                                         "': missing 'definition' field");
            }

            // Load definition: inline JSON or external file path
            json definition;
            if (config["definition"].is_string()) {
                std::string path = config["definition"].get<std::string>();
                std::ifstream f(path);
                if (!f.is_open()) {
                    throw std::runtime_error("subgraph node '" + name +
                                             "': cannot open file: " + path);
                }
                definition = json::parse(f);
            } else {
                definition = config["definition"];
            }

            // Compile inner graph with parent's context
            EngineConfig inner_config;
            inner_config.node_context = ctx;
            EngineResources inner_resources;
            inner_resources.registry = ctx.registry;
            auto inner = GraphEngine::build(definition, std::move(inner_config),
                                            std::move(inner_resources));

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
            SubgraphPersistence persistence = SubgraphPersistence::Legacy;
            if (config.contains("persistence")) {
                const auto policy = config["persistence"].get<std::string>();
                if (policy == "per_invocation") persistence = SubgraphPersistence::PerInvocation;
                else if (policy == "per_thread") persistence = SubgraphPersistence::PerThread;
                else if (policy == "stateless") persistence = SubgraphPersistence::Stateless;
                else if (policy != "legacy")
                    throw std::invalid_argument("subgraph node '" + name +
                                                "': unknown persistence policy: " + policy);
            }


            return std::make_unique<SubgraphNode>(
                name, std::shared_ptr<GraphEngine>(inner.release()),
                std::move(input_map), std::move(output_map), persistence);
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
                },
                "persistence": {
                    "type": "string",
                    "enum": ["legacy", "per_invocation", "per_thread", "stateless"],
                    "description": "Child checkpoint lifetime; legacy preserves existing identities."
                }
            },
            "required": ["definition"]
        })JSON"));
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn) {
    // Permissive default: any config object accepted. Tooling that
    // consumes export_schema() will render a free-form config for a
    // type registered without a declared schema.
    register_type(type, std::move(fn), json::parse(R"JSON({
                      "type": "object",
                      "description": "No declared config schema; any object accepted."
                  })JSON"));
}

void NodeFactory::register_type(const std::string& type, NodeFactoryFn fn, json config_schema) {
    std::lock_guard lock(mutex_);
    registry_[type] = std::move(fn);
    schemas_[type]  = std::move(config_schema);
    effects_.erase(type);   // re-registration without effects clears them
}

void NodeFactory::register_type(const std::string& type,
                                NodeFactoryFn      fn,
                                json               config_schema,
                                json               effects) {
    std::lock_guard lock(mutex_);
    registry_[type] = std::move(fn);
    schemas_[type]  = std::move(config_schema);
    effects_[type]  = std::move(effects);
}

json NodeFactory::node_effects(const std::string& type) const {
    std::lock_guard lock(mutex_);
    auto it = effects_.find(type);
    if (it != effects_.end()) return it->second;
    return json();   // null: no declared effect contract
}

std::vector<std::string> NodeFactory::registered_types() const {
    std::lock_guard lock(mutex_);
    return registry_names(registry_);
}

json NodeFactory::config_schema(const std::string& type) const {
    std::lock_guard lock(mutex_);
    auto it = schemas_.find(type);
    if (it != schemas_.end()) return it->second;
    return json::parse(
        R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
}

json NodeFactory::export_schema() const {
    std::lock_guard lock(mutex_);
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
                        "routes": { "type": "object", "additionalProperties": { "type": "string" }, "description": "Route key -> target node name. The reserved 'default' key handles unknown outputs from open conditions; without it, an unknown label is a runtime error." }
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
                        "routes": { "type": "object", "additionalProperties": { "type": "string" }, "description": "Route key -> target node name. The reserved 'default' key handles unknown outputs from open conditions; without it, an unknown label is a runtime error." }
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
        node_types[kv.first] =
            (sit != schemas_.end())
            ? sit->second
                : json::parse(
                      R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
    }

    // Per-type channel-effect contracts (only types that declared one).
    // Preserve optional fields such as exports for external tooling.
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
            for (const auto& l : spec->labels)
                labels.push_back(l);
            condition_specs[cname] = json{{"labels", std::move(labels)}, {"open", spec->open}};
        }
    }

    json doc;
    doc["neograph_version"] = NEOGRAPH_VERSION_STR;
    doc["$schema"]          = "https://json-schema.org/draft/2020-12/schema";
    doc["compiler_validation_keywords"] = json::array({"required", "type", "enum"});
    doc["topology"]         = json::parse(kTopologySchema);
    doc["node_types"]       = std::move(node_types);
    doc["node_effects"]     = std::move(node_effects);
    doc["reducers"]         = ReducerRegistry::instance().names();
    doc["conditions"]       = creg.names();
    doc["condition_specs"]  = std::move(condition_specs);
    return doc;
}

std::unique_ptr<GraphNode> NodeFactory::create(const std::string& type,
    const std::string& name,
    const json& config,
    const NodeContext& ctx) const {
    NodeFactoryFn factory;
    {
        std::lock_guard lock(mutex_);
        auto it = registry_.find(type);
        if (it == registry_.end()) {
            throw std::runtime_error("Unknown node type: '" + type + "' (referenced by node '" + name +
                                     "'). Available: " + registry_name_list(registry_) +
                ". Register a custom type before compile via "
                "NodeFactory::instance().register_type(type, factory). "
                "See docs/troubleshooting.md \"Unknown node type\".");
        }
        factory = it->second;
    }
    return factory(name, config, ctx);
}

// =========================================================================
// GraphRegistry
// =========================================================================

GraphRegistry::GraphRegistry(Fallback fallback) : fallback_(fallback) {}
GraphRegistry::GraphRegistry(const GraphRegistry& other) {
    std::lock_guard lock(other.mutex_);
    fallback_ = other.fallback_;
    reducers_ = other.reducers_;
    conditions_ = other.conditions_;
    condition_specs_ = other.condition_specs_;
    node_factories_ = other.node_factories_;
    node_schemas_ = other.node_schemas_;
    node_effects_ = other.node_effects_;
}

GraphRegistry& GraphRegistry::operator=(const GraphRegistry& other) {
    if (this == &other) return *this;
    GraphRegistry copy(other);
    *this = std::move(copy);
    return *this;
}

GraphRegistry::GraphRegistry(GraphRegistry&& other) {
    *this = std::move(other);
}

GraphRegistry& GraphRegistry::operator=(GraphRegistry&& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    std::swap(fallback_, other.fallback_);
    reducers_.swap(other.reducers_);
    conditions_.swap(other.conditions_);
    condition_specs_.swap(other.condition_specs_);
    node_factories_.swap(other.node_factories_);
    node_schemas_.swap(other.node_schemas_);
    node_effects_.swap(other.node_effects_);
    return *this;
}


GraphRegistry::GraphRegistry(bool include_builtins) {
    if (!include_builtins) return;
    ReducerRegistry reducers;
    ConditionRegistry conditions;
    NodeFactory nodes;
    reducers_        = std::move(reducers.registry_);
    conditions_      = std::move(conditions.registry_);
    condition_specs_ = std::move(conditions.specs_);
    node_factories_  = std::move(nodes.registry_);
    node_schemas_    = std::move(nodes.schemas_);
    node_effects_    = std::move(nodes.effects_);
}

const GraphRegistry& GraphRegistry::builtins() {
    static const GraphRegistry registry(true);
    return registry;
}

std::shared_ptr<const GraphRegistry> GraphRegistry::snapshot() const {
    auto copy = std::make_shared<GraphRegistry>();
    if (fallback_ == Fallback::GlobalFallback) {
        auto& reducers = ReducerRegistry::instance();
        auto& conditions = ConditionRegistry::instance();
        auto& nodes = NodeFactory::instance();
        std::scoped_lock lock(reducers.mutex_, conditions.mutex_, nodes.mutex_);
        copy->reducers_ = reducers.registry_;
        copy->conditions_ = conditions.registry_;
        copy->condition_specs_ = conditions.specs_;
        copy->node_factories_ = nodes.registry_;
        copy->node_schemas_ = nodes.schemas_;
        copy->node_effects_ = nodes.effects_;
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, fn] : reducers_) copy->reducers_[name] = fn;
        for (const auto& [name, fn] : conditions_) {
            copy->conditions_[name] = fn;
            copy->condition_specs_.erase(name);
            if (auto it = condition_specs_.find(name); it != condition_specs_.end())
                copy->condition_specs_[name] = it->second;
        }
        for (const auto& [type, fn] : node_factories_) {
            copy->node_factories_[type] = fn;
            copy->node_schemas_[type] = node_schemas_.at(type);
            copy->node_effects_.erase(type);
            if (auto it = node_effects_.find(type); it != node_effects_.end())
                copy->node_effects_[type] = it->second;
        }
    }
    return copy;
}

const GraphRegistry& GraphRegistry::global() {
    static const GraphRegistry registry(Fallback::GlobalFallback);
    return registry;
}

void GraphRegistry::register_reducer(const std::string& name, ReducerFn fn) {
    std::lock_guard lock(mutex_);
    reducers_[name] = std::move(fn);
}

void GraphRegistry::register_condition(const std::string& name, ConditionFn fn) {
    std::lock_guard lock(mutex_);
    conditions_[name] = std::move(fn);
    condition_specs_.erase(name);
}

void GraphRegistry::register_condition(const std::string& name,
                                       ConditionFn        fn,
                                       ConditionSpec      spec) {
    std::lock_guard lock(mutex_);
    conditions_[name]      = std::move(fn);
    condition_specs_[name] = std::move(spec);
}

void GraphRegistry::register_type(const std::string& type, NodeFactoryFn fn) {
    register_type(type, std::move(fn), json::parse(R"JSON({
                      "type": "object",
                      "description": "No declared config schema; any object accepted."
                  })JSON"));
}

void GraphRegistry::register_type(const std::string& type, NodeFactoryFn fn, json config_schema) {
    std::lock_guard lock(mutex_);
    node_factories_[type] = std::move(fn);
    node_schemas_[type]   = std::move(config_schema);
    node_effects_.erase(type);
}

void GraphRegistry::register_type(const std::string& type,
                                  NodeFactoryFn      fn,
                                  json               config_schema,
                                  json               effects) {
    std::lock_guard lock(mutex_);
    node_factories_[type] = std::move(fn);
    node_schemas_[type]   = std::move(config_schema);
    node_effects_[type]   = std::move(effects);
}

ReducerFn GraphRegistry::reducer(const std::string& name) const {
    {
        std::lock_guard lock(mutex_);
        if (auto it = reducers_.find(name); it != reducers_.end()) return it->second;
    }
    if (fallback_ == Fallback::GlobalFallback)
        return ReducerRegistry::instance().get(name);
    if (builtins().contains_reducer(name)) return builtins().local_reducer(name);
    throw std::runtime_error("Unknown reducer: '" + name + "' in engine registry");
}

ConditionFn GraphRegistry::condition(const std::string& name) const {
    {
        std::lock_guard lock(mutex_);
        if (auto it = conditions_.find(name); it != conditions_.end()) return it->second;
    }
    if (fallback_ == Fallback::GlobalFallback)
        return ConditionRegistry::instance().get(name);
    if (builtins().contains_condition(name)) return builtins().local_condition(name);
    throw std::runtime_error("Unknown condition: '" + name + "' in engine registry");
}

std::optional<ConditionSpec> GraphRegistry::condition_spec(const std::string& name) const {
    {
        std::lock_guard lock(mutex_);
        if (conditions_.contains(name)) {
            auto it = condition_specs_.find(name);
            return it == condition_specs_.end() ? std::nullopt
                                                : std::optional<ConditionSpec>(it->second);
        }
    }
    if (fallback_ == Fallback::GlobalFallback)
        return ConditionRegistry::instance().condition_spec(name);
    return builtins().contains_condition(name) ? builtins().local_condition_spec(name)
                                               : std::nullopt;
}

std::unique_ptr<GraphNode> GraphRegistry::create(const std::string& type,
                                                 const std::string& name,
                                                 const json& config,
                                                 const NodeContext& ctx) const {
    NodeFactoryFn factory;
    {
        std::lock_guard lock(mutex_);
        if (auto it = node_factories_.find(type); it != node_factories_.end())
            factory = it->second;
    }
    if (factory) return factory(name, config, ctx);
    if (fallback_ == Fallback::GlobalFallback)
        return NodeFactory::instance().create(type, name, config, ctx);
    if (builtins().contains_type(type))
        return builtins().create_local(type, name, config, ctx);
    throw std::runtime_error("Unknown node type: '" + type + "' (referenced by node '" +
                             name + "') in engine registry");
}

json GraphRegistry::config_schema(const std::string& type) const {
    {
        std::lock_guard lock(mutex_);
        if (node_factories_.contains(type)) return node_schemas_.at(type);
    }
    if (fallback_ == Fallback::GlobalFallback)
        return NodeFactory::instance().config_schema(type);
    if (builtins().contains_type(type)) return builtins().local_config_schema(type);
    return json::parse(
        R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
}

json GraphRegistry::node_effects(const std::string& type) const {
    {
        std::lock_guard lock(mutex_);
        if (node_factories_.contains(type)) {
            auto it = node_effects_.find(type);
            return it == node_effects_.end() ? json() : it->second;
        }
    }
    if (fallback_ == Fallback::GlobalFallback)
        return NodeFactory::instance().node_effects(type);
    return builtins().contains_type(type) ? builtins().local_node_effects(type) : json();
}

ReducerFn GraphRegistry::local_reducer(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const auto it = reducers_.find(name);
    if (it == reducers_.end()) {
        throw std::out_of_range("Local graph registry has no reducer '" + name + "'");
    }
    return it->second;
}

ConditionFn GraphRegistry::local_condition(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const auto it = conditions_.find(name);
    if (it == conditions_.end()) {
        throw std::out_of_range("Local graph registry has no condition '" + name + "'");
    }
    return it->second;
}

std::optional<ConditionSpec> GraphRegistry::local_condition_spec(const std::string& name) const {
    std::lock_guard lock(mutex_);
    if (!conditions_.contains(name)) {
        throw std::out_of_range("Local graph registry has no condition '" + name + "'");
    }
    const auto it = condition_specs_.find(name);
    return it == condition_specs_.end() ? std::nullopt : std::optional<ConditionSpec>(it->second);
}

std::unique_ptr<GraphNode> GraphRegistry::create_local(const std::string& type,
                                                       const std::string& name,
                                                       const json& config,
                                                       const NodeContext& ctx) const {
    NodeFactoryFn factory;
    {
        std::lock_guard lock(mutex_);
        const auto it = node_factories_.find(type);
        if (it == node_factories_.end())
            throw std::out_of_range("Local graph registry has no node type '" + type + "'");
        factory = it->second;
    }
    return factory(name, config, ctx);
}

json GraphRegistry::local_config_schema(const std::string& type) const {
    std::lock_guard lock(mutex_);
    if (!node_factories_.contains(type)) {
        throw std::out_of_range("Local graph registry has no node type '" + type + "'");
    }
    const auto it = node_schemas_.find(type);
    return it != node_schemas_.end()
               ? it->second
               : json::parse(
                     R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
}

json GraphRegistry::local_node_effects(const std::string& type) const {
    std::lock_guard lock(mutex_);
    if (!node_factories_.contains(type)) {
        throw std::out_of_range("Local graph registry has no node type '" + type + "'");
    }
    const auto it = node_effects_.find(type);
    return it != node_effects_.end() ? it->second : json();
}

bool GraphRegistry::contains_reducer(const std::string& name) const {
    std::lock_guard lock(mutex_);
    return reducers_.count(name) != 0;
}

bool GraphRegistry::contains_condition(const std::string& name) const {
    std::lock_guard lock(mutex_);
    return conditions_.count(name) != 0;
}

bool GraphRegistry::contains_type(const std::string& type) const {
    std::lock_guard lock(mutex_);
    return node_factories_.count(type) != 0;
}

json GraphRegistry::export_schema() const {
    // Reuse the topology grammar envelope, but replace every executable
    // palette with this registry's local entries. Reading the global envelope
    // here does not grant fallback authority: none of its registry entries
    // survive in the returned document.
    json doc = NodeFactory::instance().export_schema();
    std::lock_guard lock(mutex_);

    json node_types = json::object();
    json node_effects = json::object();
    for (const auto& [type, factory] : node_factories_) {
        (void)factory;
        auto schema = node_schemas_.find(type);
        node_types[type] =
            schema != node_schemas_.end()
                               ? schema->second
                               : json::parse(
                                     R"JSON({"type":"object","description":"No declared config schema; any object accepted."})JSON");
        auto effects = node_effects_.find(type);
        if (effects != node_effects_.end()) node_effects[type] = effects->second;
    }

    json condition_specs = json::object();
    for (const auto& [name, spec] : condition_specs_) {
        json labels = json::array();
        for (const auto& label : spec.labels)
            labels.push_back(label);
        condition_specs[name] = {{"labels", std::move(labels)}, {"open", spec.open}};
    }

    doc["node_types"]      = std::move(node_types);
    doc["node_effects"]    = std::move(node_effects);
    doc["reducers"]        = registry_names(reducers_);
    doc["conditions"]      = registry_names(conditions_);
    doc["condition_specs"] = std::move(condition_specs);
    return doc;
}

json GraphRegistry::export_effective_schema() const {
    // Capture one consistent registry generation before assembling its palette.
    // GlobalFallback snapshots legacy entries too, so no live singleton lookup
    // can add or remove a name halfway through this export.
    auto selected = snapshot();
    json doc = builtins().export_schema();
    auto local = selected->export_schema();
    json effects = json::object();
    for (const auto& [name, value] : doc["node_effects"].items())
        if (!local["node_types"].contains(name)) effects[name] = value;
    for (const auto& [name, value] : local["node_effects"].items()) effects[name] = value;
    doc["node_effects"] = std::move(effects);
    for (const auto& [name, schema] : local["node_types"].items()) doc["node_types"][name] = schema;

    json specs = json::object();
    const auto local_names = local["conditions"].get<std::vector<std::string>>();
    for (const auto& [name, value] : doc["condition_specs"].items()) {
        if (std::find(local_names.begin(), local_names.end(), name) == local_names.end())
            specs[name] = value;
    }
    for (const auto& [name, value] : local["condition_specs"].items()) specs[name] = value;
    doc["condition_specs"] = std::move(specs);
    for (const auto& field : {"reducers", "conditions"}) {
        std::vector<std::string> names = doc[field].get<std::vector<std::string>>();
        for (const auto& name : local[field]) names.push_back(name.get<std::string>());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        doc[field] = std::move(names);
    }
    return doc;
}

} // namespace neograph::graph
