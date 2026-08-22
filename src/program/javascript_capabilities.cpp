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
            std::uint32_t maximum_arity) {
    return {{"name", std::move(name)},
            {"signature", std::move(signature)},
            {"classification", std::move(classification)},
            {"minimum_arity", minimum_arity},
            {"maximum_arity", maximum_arity}};
}

}  // namespace

json javascript_authoring_capability_manifest() {
    json graph = json::array({
        method("node", "node(name, {type, ...config})", "direct", 2, 2),
        method("channel", "channel(name, {reducer, initial})", "direct", 2, 2),
        method("edge", "edge(from, to)", "direct", 2, 2),
        method("conditionalEdge", "conditionalEdge(from, condition, routes)",
               "direct_with_registry_reference", 3, 3),
        method("barrier", "barrier(node, waitFor)", "direct", 2, 2),
        method("interruptBefore", "interruptBefore(...nodeNames)", "direct", 0,
               0xffffffffu),
        method("interruptAfter", "interruptAfter(...nodeNames)", "direct", 0,
               0xffffffffu),
        method("retryPolicy", "retryPolicy({max_retries, initial_delay_ms, "
                              "backoff_multiplier, max_delay_ms})",
               "direct", 1, 1),
        method("entry", "entry(nodeName)", "direct", 1, 1),
        method("exit", "exit(nodeName)", "direct", 1, 1),
    });
    json commands = json::array({
        method("callCore", "callCore(coreName, input?, sourceSite?)", "direct", 1, 3),
        method("spawn", "spawn(childBinding, input?, sourceSite?)", "direct", 1, 3),
        method("await", "await(sealedCommand, timeoutMs?, sourceSite?)", "direct", 1, 3),
        method("join", "join(members, modeOrOptions?, options?, sourceSite?)", "direct", 1, 5),
        method("all", "all(members, options?, sourceSite?)", "direct", 1, 3),
        method("parallel", "parallel(members, options?, sourceSite?)", "alias_of_all", 1, 3),
        method("race", "race(members, options?, sourceSite?)", "direct", 1, 3),
        method("quorum", "quorum(members, requiredSuccessesOrOptions, sourceSite?)", "direct", 2,
               4),
        method("emit", "emit(value, sourceSite?)", "direct", 1, 2),
        method("checkpoint", "checkpoint(value, sourceSite?)", "direct", 1, 2),
        method("cancelScope", "cancelScope(scope, reason?, sourceSite?)", "direct", 1, 3),
        method("hostCapability", "hostCapability(importSlot, input?, sourceSite?)",
               "direct_constructor_host_mediated_execution", 1, 3),
    });
    return {{"schema_version", 1},
            {"javascript_profile", std::string(ProgramSource::JAVASCRIPT_PROFILE)},
            {"javascript_profile_version", ProgramSource::JAVASCRIPT_PROFILE_VERSION},
            {"ng_api_version", ProgramSource::JAVASCRIPT_NG_API_VERSION},
            {"define",
             {{"entry", "export function define()"},
              {"ng_properties", json::array({json{{"name", "apiVersion"},
                                                    {"kind", "constant"}}})},
              {"ng_methods", json::array({method("graph", "graph(name)", "direct", 1, 1)})},
              {"graph_builder_methods", std::move(graph)}}},
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
