// NeoGraph QuickJS DSL capability conformance probe.
//
// The probe accepts a capability case and model- or human-authored JavaScript,
// compiles the source through the real ProgramCompiler, then validates the
// lowered Core IR and/or the sealed commands yielded by main(input).

#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "javascript.h"

#include <asio/awaitable.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) { return "sha256:" + std::string(64, value); }

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:dsl-capability-probe",
                              {},
                              {},
                              {}};
}

class NoopNode final : public GraphNode {
public:
    explicit NoopNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    const auto add_node = [&](std::string name, char implementation) {
        builder.add_node(
            manifest(ExecutableKind::Node, std::move(name), implementation),
            [](const std::string& node_name, const json&, const NodeContext&) {
                return std::make_unique<NoopNode>(node_name);
            },
            json{{"type", "object"}}, json::object());
    };
    add_node("probe.node", '1');
    add_node("probe.dynamic-send", '2');
    add_node("probe.dynamic-interrupt", '3');
    builder.add_reducer(manifest(ExecutableKind::Reducer, "probe.overwrite", '4'),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_condition(manifest(ExecutableKind::Condition, "probe.route", '5'),
                          [](const GraphState&) { return std::string("left"); },
                          ConditionSpec{{"left", "right"}, false});
    return std::move(builder).build();
}

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

bool array_contains(const json& array, std::string_view wanted) {
    if (!array.is_array()) return false;
    for (const auto& value : array) {
        if (value.is_string() && value.get<std::string>() == wanted) return true;
    }
    return false;
}

bool has_edge(const json& definition, std::string_view from, std::string_view to) {
    if (!definition.contains("edges") || !definition.at("edges").is_array()) return false;
    for (const auto& edge : definition.at("edges")) {
        if (edge.is_object() && edge.value("from", "") == from && edge.value("to", "") == to)
            return true;
    }
    return false;
}

const json core_definition(const ProgramBundle& bundle) {
    const auto definitions = bundle.sealed_core_definitions();
    require(definitions.size() == 1, "expected exactly one sealed Core definition");
    return definitions.front().definition;
}

void require_node(const json& definition, std::string_view name, std::string_view type) {
    require(definition.contains("nodes") && definition.at("nodes").is_object(),
            "definition is missing nodes");
    const auto key = std::string(name);
    require(definition.at("nodes").contains(key), "missing node: " + key);
    require(definition.at("nodes").at(key).value("type", "") == type,
            "node has wrong type: " + key);
}

void require_minimal_program_core(const json& definition) {
    require(definition.value("name", "") == "capability", "graph name must be capability");
    require_node(definition, "work", "probe.node");
    require(has_edge(definition, "__start__", "work"), "work must be the graph entry");
    require(has_edge(definition, "work", "__end__"), "work must be the graph exit");
}

std::set<std::string> string_set(const json& values) {
    std::set<std::string> result;
    require(values.is_array(), "expected an array of strings");
    for (const auto& value : values) {
        require(value.is_string(), "expected an array of strings");
        result.insert(value.get<std::string>());
    }
    return result;
}

json validate_graph_case(std::string_view case_id, const ProgramBundle& bundle) {
    const auto definition = core_definition(bundle);
    if (case_id == "graph_basics") {
        require(definition.value("name", "") == "capability", "graph name must be capability");
        require(definition.contains("channels") && definition.at("channels").contains("value"),
                "value channel is missing");
        const auto channel = definition.at("channels").at("value");
        require(channel.value("reducer", "") == "probe.overwrite",
                "value channel must use probe.overwrite");
        require(channel.contains("initial") && channel.at("initial") == 0,
                "value channel must start at zero");
        for (const auto name : {"input", "transform", "output"})
            require_node(definition, name, "probe.node");
        require(has_edge(definition, "__start__", "input"), "input must be the entry");
        require(has_edge(definition, "input", "transform"), "input -> transform is missing");
        require(has_edge(definition, "transform", "output"), "transform -> output is missing");
        require(has_edge(definition, "output", "__end__"), "output must be the exit");
        return {{"nodes", 3}, {"linear_path", true}};
    }
    if (case_id == "graph_routing") {
        for (const auto name : {"router", "left", "right"})
            require_node(definition, name, "probe.node");
        require(definition.contains("conditional_edges") &&
                    definition.at("conditional_edges").size() == 1,
                "expected one conditional edge");
        const auto edge = definition.at("conditional_edges").at(0);
        require(edge.value("from", "") == "router", "conditional edge must start at router");
        require(edge.value("condition", "") == "probe.route",
                "conditional edge must use probe.route");
        require(edge.at("routes").value("left", "") == "left" &&
                    edge.at("routes").value("right", "") == "right",
                "conditional routes are incomplete");
        return {{"conditional_routes", 2}};
    }
    if (case_id == "graph_fanout_barrier") {
        for (const auto name : {"split", "left", "right", "join"})
            require_node(definition, name, "probe.node");
        for (const auto& edge : std::vector<std::pair<const char*, const char*>>{
                 {"split", "left"}, {"split", "right"}, {"left", "join"},
                 {"right", "join"}})
            require(has_edge(definition, edge.first, edge.second),
                    std::string("missing fan-out/fan-in edge: ") + edge.first + " -> " + edge.second);
        const auto barrier = definition.at("nodes").at("join").at("barrier").at("wait_for");
        require(string_set(barrier) == std::set<std::string>({"left", "right"}),
                "join barrier must wait for left and right");
        return {{"fanout", 2}, {"barrier_members", 2}};
    }
    if (case_id == "graph_hitl_retry") {
        require_minimal_program_core(definition);
        require(definition.contains("interrupt_before") &&
                    array_contains(definition.at("interrupt_before"), "work"),
                "work must be interrupted before execution");
        require(definition.contains("interrupt_after") &&
                    array_contains(definition.at("interrupt_after"), "work"),
                "work must be interrupted after execution");
        const auto retry = definition.at("retry_policy");
        require(retry.value("max_retries", 0) == 2, "max_retries must be 2");
        require(retry.value("initial_delay_ms", 0) == 10, "initial_delay_ms must be 10");
        require(retry.value("backoff_multiplier", 0.0) == 2.0,
                "backoff_multiplier must be 2");
        require(retry.value("max_delay_ms", 0) == 100, "max_delay_ms must be 100");
        return {{"hitl_before", true}, {"hitl_after", true}, {"retry", true}};
    }
    if (case_id == "registry_mediated") {
        require_node(definition, "dispatch", "probe.dynamic-send");
        require_node(definition, "approval", "probe.dynamic-interrupt");
        require(has_edge(definition, "dispatch", "approval"),
                "registry-mediated nodes must be connected");
        return {{"dynamic_send_node", true}, {"dynamic_interrupt_node", true}};
    }
    throw std::runtime_error("unknown graph capability case");
}

JavaScriptCommand next_command(neograph::program::detail::JavaScriptGenerator& generator,
                               std::optional<json> response = std::nullopt) {
    const auto step = generator.next(std::move(response));
    require(!step.done, "generator completed before yielding the expected command");
    require(step.command.has_value(), "generator yielded a non-command value");
    return *step.command;
}

void require_command(const JavaScriptCommand& command, JavaScriptCommandKind kind,
                     std::string_view site) {
    require(command.kind() == kind,
            "unexpected command kind at " + std::string(site) + ": " +
                std::string(to_string(command.kind())));
}

void require_capability_call(const JavaScriptCommand& command, std::string_view site) {
    require_command(command, JavaScriptCommandKind::CallCore, site);
    require(command.arguments().value("name", "") == "capability",
            "callCore must bind the admitted Core named capability at " + std::string(site));
}

void require_done(neograph::program::detail::JavaScriptGenerator& generator,
                  std::optional<json> response, const json& expected) {
    const auto step = generator.next(std::move(response));
    require(step.done, "generator yielded an unexpected extra command");
    require(step.value == expected, "generator returned the wrong final value");
}

std::vector<JavaScriptCommand> join_members(const JavaScriptCommand& command,
                                             std::string_view expected_mode) {
    require_command(command, JavaScriptCommandKind::Join, expected_mode);
    const auto arguments = command.arguments();
    require(arguments.value("mode", "") == expected_mode, "join mode is incorrect");
    std::vector<JavaScriptCommand> members;
    for (const auto& member : arguments.at("members"))
        members.push_back(JavaScriptCommand::from_json(member));
    return members;
}

json validate_program_case(std::string_view case_id, const std::string& source_text,
                           const ProgramBundle& bundle, const ProgramSource& source) {
    require_minimal_program_core(core_definition(bundle));
    auto generator = neograph::program::detail::JavaScriptGenerator::open(
        source, json{{"task", "review"}, {"items", json::array({1, 2, 3})}},
        JavaScriptCompileLimits{});
    require(generator.has_value(), "source must export function* main(input)");

    if (case_id == "program_control_flow") {
        const auto first = next_command(*generator);
        require_capability_call(first, "first review");
        require(first.arguments().at("input").value("attempt", 99) == 0,
                "first call must carry attempt 0");
        const auto second = next_command(*generator, json{{"accepted", false}});
        require_capability_call(second, "second review");
        require(second.arguments().at("input").value("attempt", 99) == 1,
                "second call must carry attempt 1");
        require_done(*generator, json{{"accepted", true}}, json{{"attempts", 2}});
        return {{"call_core_count", 2}, {"branch_and_loop", true}};
    }
    if (case_id == "program_map") {
        const auto command = next_command(*generator);
        const auto members = join_members(command, "all");
        require(members.size() == 3, "map must create three call_core members");
        for (std::size_t index = 0; index < members.size(); ++index) {
            require_capability_call(members[index], "mapped member");
            require(members[index].arguments().at("input").value("item", 0) ==
                        static_cast<int>(index + 1),
                    "mapped member input order is wrong");
        }
        require(command.arguments().value("max_in_flight", 0) == 2,
                "mapped all join must cap max_in_flight at 2");
        require_done(*generator, json::array({1, 2, 3}), json{{"count", 3}});
        return {{"mapped_members", 3}, {"max_in_flight", 2}};
    }
    if (case_id == "program_structured_concurrency") {
        const auto all = next_command(*generator);
        const auto all_members = join_members(all, "all");
        require(all_members.size() == 2, "ng.all must contain two members");
        for (const auto& member : all_members) require_capability_call(member, "ng.all member");
        const auto parallel = next_command(*generator, json::array({1, 2}));
        const auto parallel_members = join_members(parallel, "all");
        require(parallel_members.size() == 2,
                "ng.parallel must lower to an all join with two members");
        for (const auto& member : parallel_members)
            require_capability_call(member, "ng.parallel member");
        require(source_text.find("ng.parallel") != std::string::npos,
                "case must exercise the ng.parallel alias");
        const auto generic = next_command(*generator, json::array({1, 2}));
        const auto generic_members = join_members(generic, "all");
        require(generic_members.size() == 2,
                "ng.join must construct an all join");
        for (const auto& member : generic_members)
            require_capability_call(member, "ng.join member");
        require(source_text.find("ng.join") != std::string::npos,
                "case must exercise the generic ng.join constructor");
        const auto race = next_command(*generator, json::array({1, 2}));
        const auto race_members = join_members(race, "race");
        require(race_members.size() == 2, "ng.race must contain two members");
        for (const auto& member : race_members) require_capability_call(member, "ng.race member");
        const auto quorum = next_command(*generator, json{{"winner", 1}});
        const auto quorum_members = join_members(quorum, "quorum");
        require(quorum_members.size() == 3,
                "ng.quorum must contain three members");
        for (const auto& member : quorum_members)
            require_capability_call(member, "ng.quorum member");
        require(quorum.arguments().value("required_successes", 0) == 2,
                "quorum must require two successes");
        require_done(*generator, json::array({1, 2}), json{{"completed", true}});
        return {{"all", true}, {"parallel", true}, {"join", true},
                {"race", true}, {"quorum", true}};
    }
    if (case_id == "program_spawn_await") {
        const auto awaited = next_command(*generator);
        require_command(awaited, JavaScriptCommandKind::Await, "await");
        require(awaited.arguments().value("timeout_ms", 0) == 5000,
                "await timeout must be 5000 ms");
        const auto nested = JavaScriptCommand::from_json(awaited.arguments().at("command"));
        require_command(nested, JavaScriptCommandKind::Spawn, "spawn");
        require(nested.arguments().value("child_binding", "") == "worker-child",
                "spawn must use worker-child binding");
        require_done(*generator, json{{"child", "done"}}, json{{"child", "done"}});
        return {{"spawn", true}, {"await", true}, {"timeout_ms", 5000}};
    }
    if (case_id == "program_durability") {
        const auto emitted = next_command(*generator);
        require_command(emitted, JavaScriptCommandKind::Emit, "emit");
        require(emitted.arguments().at("value") == json{{"phase", "observed"}},
                "emit value is incorrect");
        const auto checkpoint = next_command(*generator, json{{"phase", "observed"}});
        require_command(checkpoint, JavaScriptCommandKind::Checkpoint, "checkpoint");
        require(checkpoint.arguments().at("value") == json{{"cursor", 1}},
                "checkpoint value is incorrect");
        const auto cancelled = next_command(*generator, json{{"cursor", 1}});
        require_command(cancelled, JavaScriptCommandKind::CancelScope, "cancelScope");
        require(cancelled.arguments().value("scope", "") == "current" &&
                    cancelled.arguments().value("reason", "") == "stop",
                "cancelScope arguments are incorrect");
        require_done(*generator, json::object(), json{{"unreachable", true}});
        return {{"emit", true}, {"checkpoint", true}, {"cancel_scope", true}};
    }
    if (case_id == "program_host_capability") {
        const auto command = next_command(*generator);
        require_command(command, JavaScriptCommandKind::HostCapability, "hostCapability");
        require(command.import_slot() == 7, "host capability must use admitted slot 7");
        require(command.arguments().at("input") == json{{"operation", "audit"}},
                "host capability input is incorrect");
        require_done(*generator, json{{"approved", true}}, json{{"approved", true}});
        return {{"host_capability", true}, {"import_slot", 7}};
    }
    throw std::runtime_error("unknown Program capability case");
}

std::string read_source(int argc, char** argv) {
    if (argc < 2 || argc > 3)
        throw std::invalid_argument("usage: program_dsl_capability_probe <case> [source-path]");
    std::ostringstream buffer;
    if (argc == 3) {
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) throw std::invalid_argument("could not open QuickJS source path");
        buffer << input.rdbuf();
    } else {
        buffer << std::cin.rdbuf();
    }
    auto source = buffer.str();
    if (source.empty()) throw std::invalid_argument("QuickJS source is required");
    return source;
}

bool graph_case(std::string_view case_id) {
    return case_id == "graph_basics" || case_id == "graph_routing" ||
           case_id == "graph_fanout_barrier" || case_id == "graph_hitl_retry" ||
           case_id == "registry_mediated";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--manifest") {
            std::cout << javascript_authoring_capability_manifest().dump() << '\n';
            return 0;
        }
        if (argc < 2) throw std::invalid_argument("capability case is required");
        const std::string case_id = argv[1];
        const auto source_text = read_source(argc, argv);
        const auto source = ProgramSource::from_javascript(
            "dsl-capability:" + case_id + ".js", source_text);
        const auto registry = registry_snapshot();
        ProgramCompiler compiler(registry, {"dsl-capability-probe/v1"});
        const auto bundle = compiler.compile(source);

        json evidence = graph_case(case_id)
                            ? validate_graph_case(case_id, bundle)
                            : validate_program_case(case_id, source_text, bundle, source);
        const json report{{"schema_version", 1},
                          {"case", case_id},
                          {"source_hash", source.source_hash()},
                          {"bundle_id", bundle.id()},
                          {"source_kind", std::string(to_string(bundle.source_kind()))},
                          {"evidence", std::move(evidence)},
                          {"verified", true}};
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const ProgramCompileError& error) {
        json diagnostics = json::array();
        for (const auto& diagnostic : error.diagnostics()) {
            diagnostics.push_back({{"code", diagnostic.code},
                                   {"message", diagnostic.message},
                                   {"pointer", diagnostic.primary.json_pointer},
                                   {"witness", diagnostic.witness}});
        }
        std::cerr << json{{"error", error.what()}, {"diagnostics", diagnostics}}.dump() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << json{{"error", error.what()}}.dump() << '\n';
        return 3;
    }
}
