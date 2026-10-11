#include <neograph/program/javascript_capabilities.h>

#include <neograph/program/command.h>
#include <neograph/program/source.h>

#include <cstdint>
#include <string>
#include <utility>

namespace neograph::program {
namespace {

json method(std::string name,
            std::string signature,
            std::string classification,
            std::uint32_t minimum_arity,
            std::uint32_t maximum_arity,
            std::string dts) {
    return {{"name", std::move(name)},
            {"signature", std::move(signature)},
            {"classification", std::move(classification)},
            {"minimum_arity", minimum_arity},
            {"maximum_arity", maximum_arity},
            {"dts", std::move(dts)}};
}

json argument(std::string name, json schema, bool optional = false, bool rest = false) {
    json value{{"name", std::move(name)}, {"schema", std::move(schema)}};
    if (optional) value["optional"] = true;
    if (rest) value["rest"] = true;
    return value;
}

// A graph-builder method: the general fields plus the feature flag it belongs
// to and a JSON Schema per argument (X4).
json builder(std::string   name,
             std::string   signature,
             std::string   classification,
             std::uint32_t minimum_arity,
             std::uint32_t maximum_arity,
             std::string   dts,
             std::string   feature,
             json          arguments) {
    auto value         = method(std::move(name), std::move(signature), std::move(classification),
                                minimum_arity, maximum_arity, std::move(dts));
    value["feature"]   = std::move(feature);
    value["arguments"] = std::move(arguments);
    return value;
}

json name_schema() { return json{{"type", "string"}, {"minLength", 1}}; }

json node_name_schema() {
    return json{{"type", "string"},
                {"minLength", 1},
                {"not", json{{"enum", json::array({"__start__", "__end__"})}}}};
}

json target_schema() {
    return json{{"type", "string"}, {"minLength", 1}, {"not", json{{"const", "__start__"}}}};
}

json node_list_schema() {
    return json{{"anyOf", json::array({node_name_schema(),
                                       json{{"type", "array"}, {"items", node_name_schema()}}})}};
}

json wait_for_schema() {
    // A node name, or an any_of slot whose alternatives the join accepts (#363).
    const json slot{{"type", "object"},
                    {"additionalProperties", false},
                    {"required", json::array({"any_of"})},
                    {"properties", json{{"any_of", json{{"type", "array"},
                                                        {"minItems", 1},
                                                        {"uniqueItems", true},
                                                        {"items", node_name_schema()}}}}}};
    return json{{"type", "array"},
                {"minItems", 1},
                {"uniqueItems", true},
                {"items", json{{"oneOf", json::array({node_name_schema(), slot})}}}};
}

json retry_policy_schema() {
    const json count{{"type", "integer"}, {"minimum", 0}, {"maximum", 2147483647}};
    return json{{"type", "object"},
                {"additionalProperties", false},
                {"properties", json{{"max_retries", count},
                                    {"initial_delay_ms", count},
                                    {"backoff_multiplier", json{{"type", "number"}}},
                                    {"max_delay_ms", count}}}};
}

// #362: at most 8 KiB canonical, depth 4, 64 members, keys of 1-128 bytes, no
// invisible characters, no top-level "neograph." keys. The schema states what
// a JSON Schema can; the compiler checks the rest.
json metadata_schema() {
    return json{{"type", "object"},
                {"maxProperties", 64},
                {"propertyNames",
                 json{{"type", "string"},
                      {"minLength", 1},
                      {"maxLength", 128},
                      {"not", json{{"pattern", "^neograph\\."}}}}}};
}

json graph_builder_methods() {
    return json::array({
        builder("node", "node(name, {type, ...config})", "direct", 2, 2,
                "node(name: string, config: NodeConfig): GraphBuilder;", "graph.core",
                json::array({argument("name", node_name_schema()),
                             argument("config",
                                      json{{"type", "object"},
                                           {"required", json::array({"type"})},
                                           {"properties", json{{"type", name_schema()}}},
                                           {"additionalProperties", true}})})),
        builder("channel", "channel(name, {reducer, initial})", "direct", 2, 2,
                "channel(name: string, config: ChannelConfig): GraphBuilder;", "graph.channels",
                json::array({argument("name", name_schema()),
                             argument("config",
                                      json{{"type", "object"},
                                           {"additionalProperties", false},
                                           {"properties", json{{"reducer", name_schema()},
                                                               {"initial", json::object()}}}})})),
        builder("edge", "edge(from, to)", "direct", 2, 2,
                "edge(from: string, to: string): GraphBuilder;", "graph.core",
                json::array({argument("from", node_name_schema()), argument("to", target_schema())})),
        builder("conditionalEdge", "conditionalEdge(from, condition, routes)",
                "direct_with_registry_reference", 3, 3,
                "conditionalEdge(from: string, condition: string, routes: Record<string, string>): "
                "GraphBuilder;",
                "graph.conditional_edges",
                json::array({argument("from", node_name_schema()),
                             argument("condition", name_schema()),
                             argument("routes", json{{"type", "object"},
                                                     {"minProperties", 1},
                                                     {"propertyNames", name_schema()},
                                                     {"additionalProperties", target_schema()}})})),
        builder("barrier", "barrier(node, waitFor)", "direct", 2, 2,
                "barrier(node: string, waitFor: Array<string | { any_of: string[] }>): GraphBuilder;",
                "graph.barrier",
                json::array({argument("node", node_name_schema()),
                             argument("waitFor", wait_for_schema())})),
        builder("interruptBefore", "interruptBefore(...nodeNames)", "direct", 0, 0xffffffffu,
                "interruptBefore(...nodeNames: Array<string | string[]>): GraphBuilder;",
                "graph.interrupts",
                json::array({argument("nodeNames", node_list_schema(), false, true)})),
        builder("interruptAfter", "interruptAfter(...nodeNames)", "direct", 0, 0xffffffffu,
                "interruptAfter(...nodeNames: Array<string | string[]>): GraphBuilder;",
                "graph.interrupts",
                json::array({argument("nodeNames", node_list_schema(), false, true)})),
        builder("retryPolicy",
                "retryPolicy({max_retries, initial_delay_ms, backoff_multiplier, max_delay_ms})",
                "direct", 1, 1, "retryPolicy(policy: RetryPolicy): GraphBuilder;",
                "graph.retry_policy", json::array({argument("policy", retry_policy_schema())})),
        builder("metadata", "metadata({...claims})", "direct", 1, 1,
                "metadata(claims: Record<string, Json>): GraphBuilder;", "graph.metadata",
                json::array({argument("claims", metadata_schema())})),
        builder("entry", "entry(nodeName)", "direct", 1, 1,
                "entry(nodeName: string): GraphBuilder;", "graph.core",
                json::array({argument("nodeName", node_name_schema())})),
        builder("exit", "exit(nodeName)", "direct", 1, 1, "exit(nodeName: string): GraphBuilder;",
                "graph.core", json::array({argument("nodeName", node_name_schema())})),
    });
}

// Declarations the per-method dts signatures use.
constexpr const char* kDtsPrelude =
    "type Json = null | boolean | number | string | Json[] | { [key: string]: Json };\n"
    "interface NodeConfig { type: string; [key: string]: Json; }\n"
    "interface ChannelConfig { reducer?: string; initial?: Json; }\n"
    "interface RetryPolicy { max_retries?: number; initial_delay_ms?: number;"
    " backoff_multiplier?: number; max_delay_ms?: number; }\n"
    "interface GraphBuilder {\n"
    "  node(name: string, config: NodeConfig): GraphBuilder;\n"
    "  channel(name: string, config: ChannelConfig): GraphBuilder;\n"
    "  edge(from: string, to: string): GraphBuilder;\n"
    "  conditionalEdge(from: string, condition: string, routes: Record<string, string>):"
    " GraphBuilder;\n"
    "  barrier(node: string, waitFor: Array<string | { any_of: string[] }>): GraphBuilder;\n"
    "  interruptBefore(...nodeNames: Array<string | string[]>): GraphBuilder;\n"
    "  interruptAfter(...nodeNames: Array<string | string[]>): GraphBuilder;\n"
    "  retryPolicy(policy: RetryPolicy): GraphBuilder;\n"
    "  metadata(claims: Record<string, Json>): GraphBuilder;\n"
    "  entry(nodeName: string): GraphBuilder;\n"
    "  exit(nodeName: string): GraphBuilder;\n"
    "}\n"
    "interface Command { readonly kind: string; }\n";

}  // namespace

json javascript_authoring_capability_manifest() {
    json commands = json::array({
        method("callCore", "callCore(coreName, input?, sourceSite?)", "direct", 1, 3,
               "callCore(coreName: string, input?: Json, sourceSite?: string): Command;"),
        method("spawn", "spawn(childBinding, input?, sourceSite?)", "direct", 1, 3,
               "spawn(childBinding: string, input?: Json, sourceSite?: string): Command;"),
        method("await", "await(sealedCommand, timeoutMs?, sourceSite?)", "direct", 1, 3,
               "await(sealedCommand: Command, timeoutMs?: number, sourceSite?: string): Command;"),
        method("join", "join(members, modeOrOptions?, options?, sourceSite?)", "direct", 1, 5,
               "join(members: Command[], modeOrOptions?: string | Json, options?: Json,"
               " sourceSite?: string): Command;"),
        method("all", "all(members, options?, sourceSite?)", "direct", 1, 3,
               "all(members: Command[], options?: Json, sourceSite?: string): Command;"),
        method("parallel", "parallel(members, options?, sourceSite?)", "alias_of_all", 1, 3,
               "parallel(members: Command[], options?: Json, sourceSite?: string): Command;"),
        method("race", "race(members, options?, sourceSite?)", "direct", 1, 3,
               "race(members: Command[], options?: Json, sourceSite?: string): Command;"),
        method("quorum", "quorum(members, requiredSuccessesOrOptions, sourceSite?)", "direct", 2,
               4,
               "quorum(members: Command[], requiredSuccessesOrOptions: number | Json,"
               " sourceSite?: string): Command;"),
        method("emit", "emit(value, sourceSite?)", "direct", 1, 2,
               "emit(value: Json, sourceSite?: string): Command;"),
        method("checkpoint", "checkpoint(value, sourceSite?)", "direct", 1, 2,
               "checkpoint(value: Json, sourceSite?: string): Command;"),
        method("cancelScope", "cancelScope(scope, reason?, sourceSite?)", "direct", 1, 3,
               "cancelScope(scope: string, reason?: string, sourceSite?: string): Command;"),
        method("hostCapability", "hostCapability(importSlot, input?, sourceSite?)",
               "direct_constructor_host_mediated_execution", 1, 3,
               "hostCapability(importSlot: number, input?: Json, sourceSite?: string): Command;"),
    });
    // One flag per authoring feature (X4). A work package that adds a feature
    // flips its flag in the same change.
    json features{{"graph.core", true},
                  {"graph.channels", true},
                  {"graph.conditional_edges", true},
                  {"graph.barrier", true},
                  {"graph.interrupts", true},
                  {"graph.retry_policy", true},
                  {"graph.metadata", true},
                  {"barrier.any_of", true},
                  {"retry_policy.per_node", false},
                  {"failure_edges", false},
                  {"failure_exits", false},
                  {"retry.attempt_timeout", false},
                  {"diagnostics.help", true},
                  {"diagnostics.echo_routes", true}};
    return {{"schema_version", 1},
            {"javascript_profile", std::string(ProgramSource::JAVASCRIPT_PROFILE)},
            {"javascript_profile_version", ProgramSource::JAVASCRIPT_PROFILE_VERSION},
            {"ng_api_version", ProgramSource::JAVASCRIPT_NG_API_VERSION},
            {"features", std::move(features)},
            {"schemas",
             {{"manifest",
               "https://neograph.dev/schemas/javascript-capability-manifest-v1.schema.json"},
              {"core_definition", "https://neograph.dev/schemas/core-definition-v1.schema.json"}}},
            {"dts_prelude", kDtsPrelude},
            {"define",
             {{"entry", "export function define()"},
              {"ng_properties", json::array({json{{"name", "apiVersion"},
                                                    {"kind", "constant"}}})},
              {"ng_methods",
               json::array({builder("graph", "graph(name)", "direct", 1, 1,
                                    "graph(name: string): GraphBuilder;", "graph.core",
                                    json::array({argument("name", name_schema())}))})},
              {"graph_builder_methods", graph_builder_methods()}}},
            {"main",
             {{"entry", "export function* main(input)"},
              {"ng_properties", json::array({json{{"name", "apiVersion"},
                                                    {"kind", "constant"}}})},
              {"command_methods", std::move(commands)},
              {"yield_contract", "every non-final yield is a sealed ng command"}}},
            {"structured_command_limits",
             {{"maximum_depth", JAVASCRIPT_COMMAND_MAX_STRUCTURED_DEPTH},
              {"maximum_aggregate_members", JAVASCRIPT_COMMAND_MAX_AGGREGATE_MEMBERS}}},
            {"constraints",
             json::array({"define returns exactly one open graph builder",
                          "graph mutators return the same builder and never node handles",
                          "main is a synchronous generator",
                          "canonical JSON values only",
                          "no ambient filesystem, network, process, environment, clock, random, "
                          "eval, dynamic import, Promise, or async authority"})}};
}

}  // namespace neograph::program
