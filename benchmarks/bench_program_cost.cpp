// Diagnostic costs, not a performance gate. One JSON record per fresh process.
// The generator case uses synthetic responses and excludes Runtime/store work.
// Lifecycle cases use real Core runs, real stores, and check every final value.
#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#ifdef NEOGRAPH_COST_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#endif
#ifdef NEOGRAPH_COST_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#include <neograph/program/postgres_store.h>
#include <neograph/program/postgres_transition_store.h>
#endif
#include "javascript.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {
using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;
using neograph::program::detail::JavaScriptGenerator;
using Clock = std::chrono::steady_clock;

double us(Clock::time_point begin, Clock::time_point end = Clock::now()) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

std::uint64_t resident_bytes() {
#ifdef __linux__
    std::ifstream input("/proc/self/statm");
    std::uint64_t total = 0, resident = 0;
    if (input >> total >> resident)
        return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}

std::uint64_t peak_resident_bytes() {
#ifdef __linux__
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    return 0;
}

struct Options {
    std::string case_id = "lifecycle", backend = "memory", mode = "javascript", storage;
    std::size_t iterations = 100, warmup = 10, payload = 0, commands = 1, residents = 1;
};

std::size_t number(std::string_view value) {
    std::size_t result      = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result > 1000000)
        throw std::invalid_argument("numeric option must be in [0, 1000000]");
    return result;
}

Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (++i >= argc) throw std::invalid_argument("option needs a value: " + name);
        const std::string value = argv[i];
        if (name == "--case")
            o.case_id = value;
        else if (name == "--backend")
            o.backend = value;
        else if (name == "--mode")
            o.mode = value;
        else if (name == "--storage")
            o.storage = value;
        else if (name == "--iterations")
            o.iterations = number(value);
        else if (name == "--warmup")
            o.warmup = number(value);
        else if (name == "--payload-bytes")
            o.payload = number(value);
        else if (name == "--commands")
            o.commands = number(value);
        else if (name == "--residents")
            o.residents = number(value);
        else
            throw std::invalid_argument("unknown option: " + name);
    }
    if (o.iterations == 0 || o.commands == 0 || o.commands > 32 || o.residents == 0 ||
        o.residents > 256 || o.payload > 65536)
        throw std::invalid_argument("invalid iterations, commands, residents, or payload bound");
    if (o.case_id != "lifecycle" && o.case_id != "direct" && o.case_id != "generator" &&
        o.case_id != "resident")
        throw std::invalid_argument("case must be lifecycle, direct, generator, or resident");
    if (o.mode != "javascript" && o.mode != "cpp")
        throw std::invalid_argument("mode must be javascript or cpp");
    if (o.mode == "cpp" && o.commands != 1)
        throw std::invalid_argument("cpp comparison supports exactly one Core call");
    if (o.backend != "memory" && o.backend != "sqlite" && o.backend != "postgres")
        throw std::invalid_argument("unknown backend");
    return o;
}

class Increment final : public GraphNode {
public:
    explicit Increment(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto value = input.state.get("counter").get<int>();
        co_return  NodeOutput{{ChannelWrite{"counter", value + 1}}};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

ExecutableManifest manifest(ExecutableKind kind, std::string name, char id) {
    return {{kind, std::move(name), "1.0.0", "sha256:" + std::string(64, id)},
            EffectMode::Brokered,
            "attestation:program-cost",
            {},
            {},
            {}};
}

RegistrySnapshot registry() {
    RegistrySnapshotBuilder b;
    b.add_node(
        manifest(ExecutableKind::Node, "cost-inc", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<Increment>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"writes", json::array({"counter"})}});
    b.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '2'),
                  [](const json&, const json& incoming) { return json(incoming); });
    return std::move(b).build();
}

json graph() {
    return {{"schema_version", 1},
            {"name", "cost-core"},
            {"channels",
             {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}},
              {"payload", {{"reducer", "overwrite"}, {"initial", ""}}}}},
            {"nodes",
             {{"a", {{"type", "cost-inc"}}},
              {"b", {{"type", "cost-inc"}}},
              {"c", {{"type", "cost-inc"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "a"}},
                                   {{"from", "a"}, {"to", "b"}},
                                   {{"from", "b"}, {"to", "c"}},
                                   {{"from", "c"}, {"to", "__end__"}}})},
            {"conditional_edges", json::array()}};
}

const char* javascript = R"JS(
export function define() {
    const g = ng.graph("cost-core");
    g.channel("counter", {reducer: "overwrite", initial: 0});
    g.channel("payload", {reducer: "overwrite", initial: ""});
    for (const name of ["a", "b", "c"]) g.node(name, {type: "cost-inc"});
    g.entry("a"); g.edge("a", "b"); g.edge("b", "c"); g.exit("c");
    return g;
}
export function* main(input) {
    let result;
    for (let i = 0; i < input.count; ++i)
        result = yield ng.callCore("cost-core", {counter: 0, payload: input.payload}, "cost:call");
    return result;
}
)JS";

ProgramSource source(const Options& o) {
    if (o.mode == "javascript")
        return ProgramSource::from_javascript("bench:program-cost.js", javascript);
    const std::vector<std::pair<std::string, std::uint64_t>> limits{
        {"wall_time_ms", 60000},        {"model_tokens", 0},
        {"monetary_microunits", 0},     {"max_concurrency", 1},
        {"max_program_operations", 1}, {"max_core_steps", 128},
        {"max_dynamic_compiles", 0},    {"max_child_depth", 0},
        {"max_total_children", 0}};
    auto requirements = json::array();
    for (const auto& [name, maximum] : limits)
        requirements.push_back({{"resource", name}, {"minimum", maximum}, {"maximum", maximum}});
    return ProgramSource::from_cpp_builder(
        "bench:program-cost", 1,
        {{"program_schema_version", 1},
         {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
         {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
         {"root", {{"op", "call_core"}, {"name", "cost-core"}, {"definition", graph()}}},
         {"declared_budget_requirements", std::move(requirements)}});
}

struct Stores {
    std::shared_ptr<ProgramStore>           programs;
    std::shared_ptr<CheckpointStore>        checkpoints;
    std::shared_ptr<ProgramTransitionStore> transitions;
    explicit Stores(const Options& o) {
        if (o.backend == "memory") {
            programs    = std::make_shared<InMemoryProgramStore>();
            checkpoints = std::make_shared<InMemoryCheckpointStore>();
            transitions = std::make_shared<InMemoryProgramTransitionStore>();
        } else if (o.backend == "sqlite") {
#ifdef NEOGRAPH_COST_SQLITE
            if (o.storage.empty())
                throw std::invalid_argument("sqlite needs a fresh --storage path");
            std::ifstream existing(o.storage);
            if (existing.good())
                throw std::invalid_argument("sqlite benchmark database already exists");
            programs    = std::make_shared<SQLiteProgramStore>(o.storage);
            checkpoints = std::make_shared<SqliteCheckpointStore>(o.storage);
            transitions = std::make_shared<SQLiteProgramTransitionStore>(o.storage);
#else
            throw std::invalid_argument("SQLite is not enabled in this build");
#endif
        } else {
#ifdef NEOGRAPH_COST_POSTGRES
            const char* url = std::getenv("NEOGRAPH_COST_POSTGRES_URL");
            if (!url || !*url)
                throw std::invalid_argument("NEOGRAPH_COST_POSTGRES_URL is required");
            programs    = std::make_shared<PostgreSQLProgramStore>(url);
            checkpoints = std::make_shared<PostgresCheckpointStore>(url, 4);
            transitions = std::make_shared<PostgreSQLProgramTransitionStore>(url);
#else
            throw std::invalid_argument("Postgres is not enabled in this build");
#endif
        }
    }
};

// These wall-clock markers partition one sequential invocation. They are not
// CPU profiles: Core event spans include waits and checkpoint work, and start()
// may overlap the first Core event. start_us is therefore a separate metric.
class Events final : public ProgramEventSink {
public:
    std::atomic<std::int64_t> first{0}, last{0}, terminal{0};
    static std::int64_t       now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    }
    void on_event(const ProgramEvent& event) override {
        const auto stamp = now();
        if (event.kind == ProgramEventKind::Core) {
            std::int64_t missing = 0;
            first.compare_exchange_strong(missing, stamp);
            last.store(stamp);
        } else if (event.kind == ProgramEventKind::Terminal)
            terminal.store(stamp);
    }
};

void check(const json& output, const Options& o) {
    if (output.at("channels").at("counter").at("value") != 3 ||
        output.at("channels").at("payload").at("value") != std::string(o.payload, 'x'))
        throw std::runtime_error("Core output mismatch");
}

json lifecycle(const Options& o) {
    json   cold;
    auto   t = Clock::now();
    Stores stores(o);
    cold["store_open_us"]       = us(t);
    const auto              reg = registry();
    AdmissionProfileBuilder pb;
    pb.id("program-cost")
        .semantic_version("1.0.0")
        .registry(reg)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : reg.identities())
        pb.allow_executable(identity);
    const auto            profile = std::move(pb).build();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("program-cost")
        .semantic_version("1.0.0")
        .owner_scope("program-cost")
        .admission_profile(profile)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .budget_ceiling(BudgetLimits{60000, 1000, 1000, 1, 64, 128, 1, 0, 0});
    const auto      policy = std::move(policy_builder).build();
    ProgramCompiler compiler(reg, {"program-cost/v1"});
    const auto      program_source = source(o);
    const RunBudget budget{60000, 0, 0, 1, o.mode == "cpp" ? 1u : 64u, 128, 0, 0, 0};
    t                  = Clock::now();
    const auto bundle  = o.mode == "javascript" ? compiler.compile(program_source, budget)
                                                : compiler.compile(program_source);
    cold["compile_us"] = us(t);
    const auto cache   = std::make_shared<EngineGenerationCache>();
    const auto catalog = std::make_shared<ProgramCatalog>(
        CatalogConfig{stores.programs, reg, cache, "program-cost/v1"});
    t = Clock::now();
    const auto version =
        catalog->admit(bundle, ProgramAdmission{"program-cost", profile, policy, {}});
    cold["admit_us"] = us(t);
    t                = Clock::now();
    ProgramRuntime runtime({catalog, stores.checkpoints, {}, stores.transitions, 1});
    cold["runtime_create_us"] = us(t);
    const json input{
        {"counter", 0}, {"count", o.commands}, {"payload", std::string(o.payload, 'x')}};
    auto          samples = json::array();
    json          first_run;
    std::uint64_t before = 0;
    for (std::size_t i = 0; i < o.warmup + o.iterations; ++i) {
        if (i == o.warmup) before = resident_bytes();
        const auto              events = std::make_shared<Events>();
        const ProgramInvocation invocation{input, budget, "program-cost-" + std::to_string(i),
                                           events};
        const auto              begin   = Events::now();
        auto                    handle  = runtime.start("program-cost", version, invocation);
        const auto              started = Events::now();
        const auto              result  = handle.wait();
        const auto              end     = Events::now();
        if (result.status() != ProgramTerminalStatus::Completed)
            throw std::runtime_error("Program failed: " + result.serialize_canonical());
        check(result.output(), o);
        const auto first = events->first.load(), last = events->last.load(),
                   terminal = events->terminal.load();
        if (!(begin <= first && first <= last && last <= terminal && terminal <= end))
            throw std::runtime_error("incomplete or unordered event markers");
        json sample{{"total_us", (end - begin) / 1000.0},
                    {"start_us", (started - begin) / 1000.0},
                    {"post_start_us", (end - started) / 1000.0},
                    {"before_core_us", (first - begin) / 1000.0},
                    {"core_span_us", (last - first) / 1000.0},
                    {"after_core_us", (terminal - last) / 1000.0},
                    {"wake_us", (end - terminal) / 1000.0}};
        if (i == 0) first_run = sample;
        if (i >= o.warmup) samples.push_back(std::move(sample));
    }
    return {{"cold", cold},
            {"first_run", first_run},
            {"samples", samples},
            {"rss_before_samples_bytes", before},
            {"rss_after_samples_bytes", resident_bytes()},
            {"bundle_bytes", bundle.serialize_canonical().size()},
            {"version_bytes", version.serialize_canonical().size()}};
}

json direct(const Options& o) {
    NodeFactory::instance().register_type(
        "cost-inc", [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<Increment>(name);
        });
    const auto t          = Clock::now();
    auto       engine     = GraphEngine::compile(graph(), NodeContext{});
    const auto compile_us = us(t);
    auto       samples    = json::array();
    for (std::size_t i = 0; i < o.warmup + o.iterations; ++i) {
        RunConfig config;
        config.input       = json{{"counter", 0}, {"payload", std::string(o.payload, 'x')}};
        const auto begin   = Clock::now();
        const auto result  = engine->run(config);
        const auto elapsed = us(begin);
        check(result.output, o);
        if (i >= o.warmup) samples.push_back({{"total_us", elapsed}});
    }
    return {{"cold", {{"compile_us", compile_us}}}, {"samples", samples}};
}

json generator(const Options& o) {
    const auto s = ProgramSource::from_javascript("bench:program-cost.js", javascript);
    const json input{{"count", o.iterations + 1}, {"payload", std::string(o.payload, 'x')}};
    const auto begin   = Clock::now();
    auto       g       = JavaScriptGenerator::open(s, input, JavaScriptCompileLimits{});
    const auto open_us = us(begin);
    if (!g) throw std::runtime_error("generator missing");
    const auto first_begin = Clock::now();
    const auto first       = g->next();
    const auto first_us    = us(first_begin);
    if (!first.command || first.done) throw std::runtime_error("first command missing");
    const json response{
        {"channels",
         {{"counter", {{"value", 3}}}, {"payload", {{"value", std::string(o.payload, 'x')}}}}}};
    const auto warm_begin = Clock::now();
    for (std::size_t i = 0; i < o.iterations; ++i) {
        const auto step = g->next(response);
        if (!step.command || step.done || step.command->kind() != JavaScriptCommandKind::CallCore)
            throw std::runtime_error("warm command missing");
    }
    const auto warm_us        = us(warm_begin) / static_cast<double>(o.iterations);
    const auto terminal_begin = Clock::now();
    const auto terminal       = g->next(response);
    const auto terminal_us    = us(terminal_begin);
    if (!terminal.done || terminal.value != response)
        throw std::runtime_error("generator result mismatch");
    const auto close_begin = Clock::now();
    g.reset();
    const auto close_us = us(close_begin);
    return {{"open_us", open_us},
            {"first_next_us", first_us},
            {"warm_next_us", warm_us},
            {"terminal_next_us", terminal_us},
            {"close_us", close_us},
            {"scope",
             "synthetic Core responses; includes JS/C++ conversion and host command creation; no "
             "scheduling or storage"}};
}

json resident(const Options& o) {
    const auto s = ProgramSource::from_javascript("bench:program-cost.js", javascript);
    std::vector<JavaScriptGenerator> generators;
    generators.reserve(o.residents);
    const auto before = resident_bytes();
    const auto begin  = Clock::now();
    for (std::size_t i = 0; i < o.residents; ++i) {
        auto g = JavaScriptGenerator::open(
            s, json{{"count", 1}, {"payload", std::string(o.payload, 'x')}},
            JavaScriptCompileLimits{});
        if (!g || !g->next().command)
            throw std::runtime_error("resident generator command missing");
        generators.push_back(std::move(*g));
    }
    const auto open_us = us(begin);
    const auto after   = resident_bytes();
    return {
        {"resident_generators", o.residents},
        {"rss_before_bytes", before},
        {"rss_after_bytes", after},
        {"open_all_us", open_us},
        {"scope",
         "suspended QuickJS generators only; excludes ProgramRuntime, journal, and Core engines"}};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto o = options(argc, argv);
        json       result;
        if (o.case_id == "lifecycle")
            result = lifecycle(o);
        else if (o.case_id == "direct")
            result = direct(o);
        else if (o.case_id == "generator")
            result = generator(o);
        else
            result = resident(o);
        result["schema_version"]   = 1;
        result["case"]             = o.case_id;
        result["backend"]          = o.backend;
        result["mode"]             = o.mode;
        result["payload_bytes"]    = o.payload;
        result["commands_per_run"] = o.commands;
        result["iterations"]       = o.iterations;
        result["warmup"]           = o.warmup;
        result["peak_rss_bytes"]   = peak_resident_bytes();
        result["build_type"]       = NEOGRAPH_BENCH_BUILD_TYPE;
        result["status"]           = "ok";
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const ProgramAdmissionError& error) {
        for (const auto& diagnostic : error.diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    } catch (const ProgramCompileError& error) {
        for (const auto& diagnostic : error.diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    } catch (const std::exception& error) {
        // Connection errors can include credentials. Never emit the configured URL.
        std::string message = error.what();
        const char* url     = std::getenv("NEOGRAPH_COST_POSTGRES_URL");
        if (url && *url) {
            std::size_t at;
            while ((at = message.find(url)) != std::string::npos)
                message.replace(at, std::string(url).size(), "[redacted]");
        }
        std::cerr << "error: " << message << '\n';
    }
    return 2;
}
