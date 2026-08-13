// Q1/Q2 JavaScript control-path benchmark driver.
// One invocation emits one JSON sample. The Python runner owns fresh-process
// repetition, statistics, immutable thresholds, and cross-build pairing.

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include "canonical_json.h"
#include "javascript.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;
using neograph::program::detail::JavaScriptGenerator;
using Clock = std::chrono::steady_clock;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

class IncrementNode final : public GraphNode {
public:
    explicit IncrementNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto current = input.state.get("counter");
        const auto value   = current.is_number_integer() ? current.get<int>() : 0;
        co_return  NodeOutput{{ChannelWrite{"counter", value + 1}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:quickjs-performance",
                              {},
                              {},
                              {}};
}

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "quickjs-bench-inc", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncrementNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"writes", json::array({"counter"})}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '2'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

json core_definition(std::string name = "quickjs-bench-core") {
    return {
        {"schema_version", 1},
        {"name", std::move(name)},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes",
         {{"a", {{"type", "quickjs-bench-inc"}}},
          {"b", {{"type", "quickjs-bench-inc"}}},
          {"c", {{"type", "quickjs-bench-inc"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "a"}},
                               {{"from", "a"}, {"to", "b"}},
                               {{"from", "b"}, {"to", "c"}},
                               {{"from", "c"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
}

json budget_requirements(std::uint64_t minimum_operations  = 1,
                         std::uint64_t minimum_concurrency = 1) {
    return json::array({
        {{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 60000}},
        {{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_concurrency"}, {"minimum", minimum_concurrency}, {"maximum", 4}},
        {{"resource", "max_program_operations"}, {"minimum", minimum_operations}, {"maximum", 32}},
        {{"resource", "max_core_steps"}, {"minimum", 3}, {"maximum", 100}},
        {{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

AdmissionProfile make_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("quickjs-performance-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities())
        builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot make_policy(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("quickjs-performance-policy")
        .semantic_version("1.0.0")
        .owner_scope("quickjs-performance")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32})
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    return std::move(builder).build();
}

std::runtime_error compile_error(const ProgramCompileError& error) {
    std::string message = error.what();
    for (const auto& diagnostic : error.diagnostics()) {
        message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                   diagnostic.message + " " + diagnostic.witness.dump();
    }
    return std::runtime_error(message);
}

std::runtime_error admission_error(const ProgramAdmissionError& error) {
    std::string message = error.what();
    for (const auto& diagnostic : error.diagnostics()) {
        message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                   diagnostic.message + " " + diagnostic.witness.dump();
    }
    return std::runtime_error(message);
}

class RuntimeFixture final {
public:
    explicit RuntimeFixture(std::size_t scheduler_threads = 4)
        : registry_(registry_snapshot()),
          profile_(make_profile(registry_)),
          policy_(make_policy(profile_)),
          store_(std::make_shared<InMemoryProgramStore>()),
          cache_(std::make_shared<EngineGenerationCache>()),
          catalog_(std::make_shared<ProgramCatalog>(
              CatalogConfig{store_, registry_, cache_, "quickjs-performance/v1"})),
          checkpoints_(std::make_shared<InMemoryCheckpointStore>()),
          transitions_(std::make_shared<InMemoryProgramTransitionStore>()),
          runtime_(std::make_unique<ProgramRuntime>(
              RuntimeConfig{catalog_, checkpoints_, {}, transitions_, scheduler_threads})) {}

    ProgramVersion admit(ProgramSource source) {
        ProgramCompiler              compiler(registry_, {"quickjs-performance/v1"});
        std::optional<ProgramBundle> bundle;
        try {
            bundle = compiler.compile(source);
        } catch (const ProgramCompileError& error) {
            throw compile_error(error);
        }
        try {
            return catalog_->admit(*bundle,
                                   ProgramAdmission{"quickjs-performance", profile_, policy_, {}});
        } catch (const ProgramAdmissionError& error) {
            throw admission_error(error);
        }
    }

    ProgramRuntime&         runtime() noexcept { return *runtime_; }
    const RegistrySnapshot& registry() const noexcept { return registry_; }

private:
    RegistrySnapshot                                registry_;
    AdmissionProfile                                profile_;
    PolicySnapshot                                  policy_;
    std::shared_ptr<InMemoryProgramStore>           store_;
    std::shared_ptr<EngineGenerationCache>          cache_;
    std::shared_ptr<ProgramCatalog>                 catalog_;
    std::shared_ptr<InMemoryCheckpointStore>        checkpoints_;
    std::shared_ptr<InMemoryProgramTransitionStore> transitions_;
    std::unique_ptr<ProgramRuntime>                 runtime_;
};

const RunBudget kBudget{60000, 0, 0, 4, 32, 100, 0, 0, 0};
const RunBudget kJavaScriptBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0};

std::string javascript_control_source() {
    return R"JS(
        export function define() {
            const graph = ng.graph("quickjs-bench-core");
            graph.channel("counter", {reducer: "overwrite", initial: 0});
            for (const name of ["a", "b", "c"])
                graph.node(name, {type: "quickjs-bench-inc"});
            graph.entry("a");
            graph.edge("a", "b");
            graph.edge("b", "c");
            graph.exit("c");
            return graph;
        }
        export function* main(input) {
            return yield ng.callCore("quickjs-bench-core", input);
        }
    )JS";
}

std::string generator_source(bool host_bridge) {
    if (host_bridge) {
        return R"JS(
            export function* main(input) {
                for (let index = 0; index < input.count; ++index)
                    yield ng.callCore("quickjs-bench-core", {index, payload: input.payload});
                return {done: true};
            }
        )JS";
    }
    return R"JS(
        export function* main(input) {
            for (let index = 0; index < input.count; ++index)
                yield {protocol_version: 1, op: "call_core", name: "quickjs-bench-core",
                       input: {index, payload: input.payload}};
            return {done: true};
        }
    )JS";
}

std::string replay_source() {
    return R"JS(
        export function* main(input) {
            for (let index = 0; index < input.count; ++index) {
                const output =
                    yield ng.callCore("quickjs-bench-core", {index, payload: input.payload});
                if (output.value !== index)
                    throw new Error("recorded command response did not round-trip");
            }
            return {done: true};
        }
    )JS";
}

std::string large_builder_source() {
    return R"JS(
        export function define() {
            const graph = ng.graph("quickjs-builder-memory");
            graph.channel("counter", {reducer: "overwrite", initial: 0});
            for (let index = 0; index < 64; ++index)
                graph.node(`node-${index}`, {type: "quickjs-bench-inc"});
            graph.entry("node-0");
            for (let index = 0; index < 63; ++index)
                graph.edge(`node-${index}`, `node-${index + 1}`);
            graph.exit("node-63");
            return graph;
        }
    )JS";
}

std::size_t parse_positive(std::string_view text, const char* name) {
    std::size_t value       = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    return value;
}

std::size_t parse_nonnegative(std::string_view text, const char* name) {
    std::size_t value       = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument(std::string(name) + " must be a nonnegative integer");
    return value;
}

std::size_t peak_resident_bytes() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;
#endif
#else
    return 0;
#endif
}

const char* build_type() noexcept {
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
    return NEOGRAPH_BENCH_BUILD_TYPE;
#else
    return "unspecified";
#endif
}

double elapsed_us(Clock::time_point started) {
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

void require_command(const neograph::program::detail::JavaScriptGeneratorStep& step,
                     bool                                                      host_bridge) {
    const bool valid =
        host_bridge
            ? step.command.has_value() && step.command->kind() == JavaScriptCommandKind::CallCore
            : step.value.is_object() && step.value.value("op", "") == "call_core";
    if (step.done || !valid)
        throw std::runtime_error("generator benchmark did not yield call_core");
}

void print_sample(std::string_view case_id,
                  double           value,
                  std::string_view unit,
                  json             components = json::object()) {
    json output{{"schema_version", 1},
                {"case", case_id},
                {"status", "ok"},
                {"value", value},
                {"unit", unit},
                {"peak_allocated_bytes", peak_resident_bytes()},
                {"memory_scope", "process_peak_resident_set"},
                {"build_type", build_type()}};
    if (!components.empty()) output["components"] = std::move(components);
    std::cout << output.dump() << '\n';
}

void benchmark_define_lowering() {
    auto            registry = registry_snapshot();
    ProgramCompiler compiler(registry, {"quickjs-performance/define/v1"});
    const auto      source =
        ProgramSource::from_javascript("bench:define.js", javascript_control_source());
    const auto started = Clock::now();
    const auto bundle  = compiler.compile(source);
    const auto value   = elapsed_us(started);
    if (bundle.sealed_core_definitions().size() != 1)
        throw std::runtime_error("define benchmark did not lower one Core definition");
    print_sample("define_lowering", value, "us");
}

void benchmark_generator(std::string_view case_id, std::size_t iterations, bool host_bridge) {
    const auto source =
        ProgramSource::from_javascript("bench:generator.js", generator_source(host_bridge));
    auto generator =
        JavaScriptGenerator::open(source, json{{"count", iterations + 1}, {"payload", "benchmark"}},
                                  JavaScriptCompileLimits{});
    if (!generator) throw std::runtime_error("generator benchmark main() was not exported");

    if (case_id == "generator_first_command") {
        const auto started = Clock::now();
        const auto step    = generator->next();
        const auto value   = elapsed_us(started);
        require_command(step, host_bridge);
        print_sample(case_id, value, "us");
        return;
    }

    require_command(generator->next(), host_bridge);
    const auto started = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto step = generator->next(json{{"counter", index + 1}});
        require_command(step, host_bridge);
    }
    const auto per_command = elapsed_us(started) / static_cast<double>(iterations);
    print_sample(case_id, per_command, "us");
}

std::string structured_join_source(std::string_view mode) {
    std::string body;
    if (mode == "sequence") {
        body = R"JS(
            yield ng.callCore("quickjs-bench-core", {}, "sequence:first");
            yield ng.callCore("quickjs-bench-core", {}, "sequence:second");
        )JS";
    } else if (mode == "parallel") {
        body = R"JS(
            yield ng.all([
                ng.callCore("quickjs-bench-core", {}, "parallel:first"),
                ng.callCore("quickjs-bench-core", {}, "parallel:second")
            ], {max_in_flight: 2}, "parallel:join");
        )JS";
    } else if (mode == "race") {
        body = R"JS(
            yield ng.race([
                ng.callCore("quickjs-bench-core", {}, "race:first"),
                ng.callCore("quickjs-bench-core", {}, "race:second")
            ], {max_in_flight: 2}, "race:join");
        )JS";
    } else if (mode == "quorum") {
        body = R"JS(
            yield ng.quorum([
                ng.callCore("quickjs-bench-core", {}, "quorum:first"),
                ng.callCore("quickjs-bench-core", {}, "quorum:second")
            ], 2, {max_in_flight: 2}, "quorum:join");
        )JS";
    } else if (mode == "await") {
        body = R"JS(
            yield ng.await(ng.callCore("quickjs-bench-core", {}, "await:core"), 1000,
                           "await:join");
        )JS";
    } else {
        throw std::invalid_argument("unknown structured join mode: " + std::string(mode));
    }
    return R"JS(
        export function define() {
            const graph = ng.graph("quickjs-bench-core");
            graph.channel("counter", {reducer: "overwrite", initial: 0});
            for (const name of ["a", "b", "c"])
                graph.node(name, {type: "quickjs-bench-inc"});
            graph.entry("a");
            graph.edge("a", "b");
            graph.edge("b", "c");
            graph.exit("c");
            return graph;
        }
        export function* main(input) {
)JS" + body + R"JS(
            return {done: true};
        }
    )JS";
}

void require_completed(const ProgramResult& result, std::string_view context) {
    if (result.status() != ProgramTerminalStatus::Completed)
        throw std::runtime_error(std::string(context) + " did not complete");
}

void benchmark_structured_join(std::string_view case_id,
                               std::string_view mode,
                               std::size_t      iterations) {
    RuntimeFixture fixture;
    const auto     version = fixture.admit(ProgramSource::from_javascript(
        "bench:structured-join.js", structured_join_source(mode)));
    require_completed(
        fixture.runtime().run("quickjs-performance", version,
                              ProgramInvocation{json::object(), kBudget, "warmup", {}}),
        "structured join warm-up");

    const auto started = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        require_completed(
            fixture.runtime().run(
                "quickjs-performance", version,
                ProgramInvocation{
                    json::object(), kBudget, "structured-" + std::to_string(index), {}}),
            "structured join sample");
    }
    print_sample(case_id, elapsed_us(started) / static_cast<double>(iterations), "us");
}

void benchmark_wrapped_core(std::size_t iterations) {
    RuntimeFixture fixture;
    const auto     version = fixture.admit(
        ProgramSource::from_javascript("bench:wrapped-core.js", javascript_control_source()));
    auto direct = GraphEngine::compile(core_definition(), NodeContext{});

    (void)direct->run(RunConfig{});
    require_completed(
        fixture.runtime().run(
            "quickjs-performance", version,
            ProgramInvocation{json::object(), kJavaScriptBudget, "wrapped-warmup", {}}),
        "JavaScript-wrapped warm-up");

    const auto direct_started = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index)
        (void)direct->run(RunConfig{});
    const auto direct_us = elapsed_us(direct_started) / static_cast<double>(iterations);

    const auto wrapped_started = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        require_completed(
            fixture.runtime().run(
                "quickjs-performance", version,
                ProgramInvocation{
                    json::object(), kJavaScriptBudget, "wrapped-" + std::to_string(index), {}}),
            "JavaScript-wrapped Core sample");
    }
    const auto wrapped_us = elapsed_us(wrapped_started) / static_cast<double>(iterations);
    if (direct_us <= 0.0) throw std::runtime_error("direct Core timing was not positive");
    print_sample("javascript_wrapped_core_ratio", wrapped_us / direct_us, "ratio",
                 {{"direct_core_us", direct_us}, {"javascript_wrapped_core_us", wrapped_us}});
}

void benchmark_builder_memory() {
    auto            registry = registry_snapshot();
    ProgramCompiler compiler(registry, {"quickjs-performance/builder-memory/v1"});
    const auto      bundle = compiler.compile(
        ProgramSource::from_javascript("bench:builder-memory.js", large_builder_source()));
    if (bundle.sealed_core_definitions().size() != 1)
        throw std::runtime_error("builder memory benchmark did not lower one Core definition");
    const auto bytes = peak_resident_bytes();
    print_sample("runtime_builder_peak_memory", static_cast<double>(bytes), "bytes");
}

json replay_response(std::size_t index) {
    return json{{"value", static_cast<std::uint64_t>(index)}};
}

std::vector<std::string> record_replay_command_stream(const ProgramSource& source,
                                                       std::size_t          replay_count) {
    auto session = JavaScriptGenerator::open(
        source, json{{"count", replay_count}, {"payload", "recorded-command-replay"}},
        JavaScriptCompileLimits{});
    if (!session) throw std::runtime_error("replay benchmark main() was not exported");

    std::vector<std::string> commands;
    commands.reserve(replay_count);
    std::optional<json> response;
    for (std::size_t index = 0; index < replay_count; ++index) {
        const auto step = session->next(std::move(response));
        require_command(step, true);
        commands.push_back(neograph::program::detail::canonical_json_bytes(step.command->to_json()));
        response = replay_response(index);
    }
    const auto terminal = session->next(std::move(response));
    if (!terminal.done || terminal.value != json{{"done", true}})
        throw std::runtime_error("recording replay command stream did not terminate");
    return commands;
}

void benchmark_replay_growth(std::size_t replay_count) {
    const auto source = ProgramSource::from_javascript("bench:replay-growth.js", replay_source());
    const auto recorded_commands = record_replay_command_stream(source, replay_count);

    const auto started = Clock::now();
    auto session = JavaScriptGenerator::open(
        source, json{{"count", replay_count}, {"payload", "recorded-command-replay"}},
        JavaScriptCompileLimits{});
    if (!session) throw std::runtime_error("replay benchmark main() was not exported");

    std::optional<json> response;
    for (std::size_t index = 0; index < recorded_commands.size(); ++index) {
        const auto step = session->next(std::move(response));
        require_command(step, true);
        if (neograph::program::detail::canonical_json_bytes(step.command->to_json()) !=
            recorded_commands[index])
            throw std::runtime_error("replayed command did not match its recorded command");
        response = replay_response(index);
    }
    const auto terminal = session->next(std::move(response));
    if (!terminal.done || terminal.value != json{{"done", true}})
        throw std::runtime_error("replayed command stream did not terminate");

    print_sample("replay_growth", elapsed_us(started), "us",
                 {{"replay_command_count", replay_count},
                  {"recorded_command_count", recorded_commands.size()},
                  {"scope",
                   "fresh QuickJS source re-execution with canonical recorded-command matching; "
                   "excludes ProgramRuntime scheduling, journal I/O, and Core dispatch"}});
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::optional<std::string> case_id;
        std::size_t                iterations = 1;
        std::optional<std::size_t> replay_count;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--case" && index + 1 < argc) {
                case_id = argv[++index];
            } else if (argument == "--iterations" && index + 1 < argc) {
                iterations = parse_positive(argv[++index], "iterations");
            } else if (argument == "--replay-count" && index + 1 < argc) {
                replay_count = parse_nonnegative(argv[++index], "replay-count");
            } else {
                throw std::invalid_argument(
                    "usage: bench_quickjs_control --case <id> [--iterations N] "
                    "[--replay-count N]");
            }
        }
        if (!case_id) throw std::invalid_argument("--case is required");

        NodeFactory::instance().register_type(
            "quickjs-bench-inc", [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<IncrementNode>(name);
            });

        if (*case_id == "define_lowering") {
            benchmark_define_lowering();
        } else if (*case_id == "generator_first_command") {
            benchmark_generator(*case_id, 1, false);
        } else if (*case_id == "generator_warm_command") {
            benchmark_generator(*case_id, iterations, false);
        } else if (*case_id == "host_bridge_round_trip") {
            benchmark_generator(*case_id, iterations, true);
        } else if (*case_id == "runtime_builder_peak_memory") {
            benchmark_builder_memory();
        } else if (*case_id == "structured_join_sequence") {
            benchmark_structured_join(*case_id, "sequence", iterations);
        } else if (*case_id == "structured_join_parallel") {
            benchmark_structured_join(*case_id, "parallel", iterations);
        } else if (*case_id == "structured_join_race") {
            benchmark_structured_join(*case_id, "race", iterations);
        } else if (*case_id == "structured_join_quorum") {
            benchmark_structured_join(*case_id, "quorum", iterations);
        } else if (*case_id == "structured_join_await") {
            benchmark_structured_join(*case_id, "await", iterations);
        } else if (*case_id == "javascript_wrapped_core_ratio") {
            benchmark_wrapped_core(iterations);
        } else if (*case_id == "replay_growth") {
            if (!replay_count) throw std::invalid_argument("replay_growth requires --replay-count");
            benchmark_replay_growth(*replay_count);
        } else {
            throw std::invalid_argument("unknown control benchmark case: " + *case_id);
        }
        return EXIT_SUCCESS;
    } catch (const ProgramCompileError& error) {
        std::cerr << "error: " << compile_error(error).what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
