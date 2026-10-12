// A host-bound provider for Program llm_call nodes, priced against the run's
// monetary budget (#372).
//
// Every run goes through ProgramRuntime with the binder and the brokers a host
// uses: program::detail::single_provider_binder (what the Python LocalProgramHost
// installs), bind_priced_provider_call for in-memory hosts and the SQLite
// provider journal for durable ones. The provider is a local double speaking
// through a real SDK client, so origin, model, output cap and input limits come
// from an admitted descriptor exactly as they do for a hosted model.
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/program.h>
#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>

#include "fixtures/typed_provider.h"
#include "monetary.h"
#include "pricing.h"
#include "provider_binding.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace neograph;
using namespace neograph::program;
using pricing::PriceCatalog;

constexpr std::string_view kOpenRouter = "https://openrouter.ai";
constexpr std::string_view kModel      = "deepseek/deepseek-v4-flash-0731";
constexpr std::uint64_t    kInputLimit = 100'000;
constexpr std::uint64_t    kOutputCap  = 64;
const std::string          kOwner      = "tenant:priced";

std::string digest(char c) { return "sha256:" + std::string(64, c); }

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

struct TempDir {
    explicit TempDir(const std::string& name) {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
               ("neograph-provider-binding-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string provider_db() const { return (path / "provider.sqlite").string(); }
    std::string program_db() const { return (path / "program.sqlite").string(); }
    std::string checkpoint_db() const { return (path / "checkpoint.sqlite").string(); }
    std::filesystem::path path;
};

struct Connection {
    explicit Connection(const std::string& file, int flags) {
        if (sqlite3_open_v2(file.c_str(), &db, flags, nullptr) != SQLITE_OK) {
            const std::string message = db ? sqlite3_errmsg(db) : "sqlite allocation failed";
            if (db) sqlite3_close_v2(db);
            throw std::runtime_error("cannot open " + file + ": " + message);
        }
    }
    ~Connection() {
        if (db) sqlite3_close_v2(db);
    }
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;
    sqlite3*    db                           = nullptr;
};

// The committed state of live databases at this instant: what a restart after
// the process died would find.
void crash_image(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to);
    for (const auto& entry : std::filesystem::directory_iterator(from)) {
        if (entry.path().extension() != ".sqlite") continue;
        Connection source(entry.path().string(), SQLITE_OPEN_READONLY);
        Connection target((to / entry.path().filename()).string(),
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        auto* backup = sqlite3_backup_init(target.db, "main", source.db, "main");
        if (!backup) throw std::runtime_error("sqlite backup init failed");
        const auto step   = sqlite3_backup_step(backup, -1);
        const auto finish = sqlite3_backup_finish(backup);
        if (step != SQLITE_DONE || finish != SQLITE_OK) throw std::runtime_error("sqlite backup failed");
    }
}

std::vector<std::string> journal_states(const std::string& provider_db) {
    Connection    db(provider_db, SQLITE_OPEN_READONLY);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.db, "SELECT state FROM ng_program_provider_calls ORDER BY rowid", -1,
                           &stmt, nullptr) != SQLITE_OK)
        return {};
    std::vector<std::string> states;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        states.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return states;
}

// ---------------------------------------------------------------------------
// Provider double: a real SDK client, a local transport
// ---------------------------------------------------------------------------

// The SDK policy with an input limit for `model`; `origin` is the base URL.
std::shared_ptr<sp::runtime::Client> client_for(std::string origin, const std::string& model,
                                                std::optional<std::uint64_t> input_limit) {
    auto  source = json::parse(sp::config_defaults::descriptor_policy_json);
    json  defaults;
    for (const auto& family : source.at("families"))
        if (family.at("family") == "openai.chat") defaults = family.at("defaults");
    if (input_limit)
        source.at("models").push_back(json{{"family", "openai.chat"}, {"model", model},
                                           {"defaults", defaults}, {"output_limit", nullptr},
                                           {"input_limit", *input_limit}});
    auto admitted = sp::descriptor::load_policy(source.dump(), sp::config_defaults::codec_defaults_json);
    if (auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::make_shared<sp::runtime::Client>(neograph::test::descriptor(
        "openai.chat", std::move(origin),
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted))));
}

sp::Usage final_usage(std::uint64_t input, std::uint64_t output) {
    return neograph::test::usage(input, output, input + output);
}

sp::Usage with_cost(sp::Usage usage, std::uint64_t nano_usd) {
    usage.provider_cost.source    = sp::CostSource::OpenRouterUsd;
    usage.provider_cost.total     = sp::UsdAmount{nano_usd};
    usage.provider_cost.status[0] = sp::CostStatus::Available;
    return usage;
}

struct Peer {
    std::atomic<int> transport{0};
    std::mutex       mutex;
    std::vector<std::string> models;
    // Answers request number `ordinal` (1-based).
    std::function<sp::runtime::Result(int ordinal, const ProviderRequest&)> reply =
        [](int, const ProviderRequest&) {
            return neograph::test::success("answer", with_cost(final_usage(21, 3), 1'234'567));
        };
    // Runs inside the transport before it answers; may block or throw.
    std::function<void(int ordinal, const ProviderRequest&)> on_transport;
};

std::shared_ptr<Provider> make_provider(std::shared_ptr<Peer>                peer,
                                        std::shared_ptr<sp::runtime::Client> client) {
    // The SDK descriptor id differs from the registry's provider name on purpose.
    return std::make_shared<neograph::test::LocalProvider>(
        [peer](ProviderRequest request, const auto&, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            const int ordinal = ++peer->transport;
            {
                std::lock_guard lock(peer->mutex);
                peer->models.push_back(std::get<sp::chat::Request>(request.payload).model);
            }
            if (peer->on_transport) peer->on_transport(ordinal, request);
            co_return peer->reply(ordinal, request);
        },
        "upstream-descriptor", std::move(client));
}

// ---------------------------------------------------------------------------
// Host nodes that call the bound provider through the runtime's broker
// ---------------------------------------------------------------------------

struct Script {
    // The node's first attempts raise an ordinary (retryable) error after their call.
    std::atomic<int>  fail_first_attempts{0};
    // The node interrupts for approval after (or before) its call until resumed.
    std::atomic<bool> interrupt_after_call{false};
    std::atomic<bool> interrupt_before_call{false};
    std::atomic<int>  node_runs{0};
    // Run by every "after" node with its name.
    std::function<void(const std::string&)> after;
    // #386: the "check" node asks, until resumed, on this Core thread, and
    // finishes on every other one.
    std::string      ask_thread;
    std::atomic<int> checks_asked{0};
    std::atomic<int> checks_finished{0};
    std::atomic<int> checks_answered{0};
};

class BrokeredCall final : public graph::GraphNode {
public:
    BrokeredCall(std::string name, std::shared_ptr<Provider> provider, std::shared_ptr<Script> script)
        : name_(std::move(name)), provider_(std::move(provider)), script_(std::move(script)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        const int run = ++script_->node_runs;
        if (script_->interrupt_before_call.load() && !in.ctx.resume_value)
            throw graph::NodeInterrupt("approve the model call", json{{"request", "approve"}});
        auto request  = make_provider_request(*provider_, std::string(kModel),
                                              {neograph::test::message("probe", sp::Role::User)});
        std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
        const auto broker = graph::provider_call_broker(in.ctx);
        if (!broker) throw std::logic_error("the Program provider broker is absent");
        auto result = co_await graph::observe_provider_result(
            in.ctx, broker->invoke(graph::make_provider_call_identity(in.ctx, name_), provider_,
                                   std::move(request)));
        graph::record_usage(in.ctx, result);
        if (run <= script_->fail_first_attempts.load())
            throw std::runtime_error("transient failure after the call");
        if (script_->interrupt_after_call.load() && !in.ctx.resume_value)
            throw graph::NodeInterrupt("approve the model output", json{{"request", "approve"}});
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", neograph::test::text(outcome_or_throw(result))});
        co_return output;
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::shared_ptr<Script>   script_;
};

// #385: a map step with two Sends to the brokered "work" node.
class SendFan final : public graph::GraphNode {
public:
    explicit SendFan(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        graph::NodeOutput output;
        output.sends.push_back(graph::Send{"work", json{{"value", "left"}}});
        output.sends.push_back(graph::Send{"work", json{{"value", "right"}}});
        co_return output;
    }

private:
    std::string name_;
};

// #386: asks on the Core thread the script names until it is resumed, and
// finishes everywhere else.
class CheckNode final : public graph::GraphNode {
public:
    CheckNode(std::string name, std::shared_ptr<Script> script)
        : name_(std::move(name)), script_(std::move(script)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        graph::NodeOutput output;
        if (in.ctx.resume_value) {
            ++script_->checks_answered;
            output.writes.push_back(graph::ChannelWrite{"value", *in.ctx.resume_value});
            co_return output;
        }
        if (in.ctx.thread_id == script_->ask_thread) {
            ++script_->checks_asked;
            throw graph::NodeInterrupt("approve the check", json{{"request", "approve"}});
        }
        ++script_->checks_finished;
        output.writes.push_back(graph::ChannelWrite{"value", "checked"});
        co_return output;
    }

private:
    std::string             name_;
    std::shared_ptr<Script> script_;
};

class AfterNode final : public graph::GraphNode {
public:
    AfterNode(std::string name, std::shared_ptr<Script> script)
        : name_(std::move(name)), script_(std::move(script)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        if (script_->after) script_->after(name_);
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"done", true});
        co_return output;
    }

private:
    std::string             name_;
    std::shared_ptr<Script> script_;
};

// ---------------------------------------------------------------------------
// One host: registry, binder, catalog, runtime
// ---------------------------------------------------------------------------

enum class Shape {
    AfterOnly,
    Llm,
    LlmThenAfter,
    TwoLlms,
    Brokered,
    BrokeredThenAfter,
    SendsToBrokered,
    // #386: the model on the first call of a plan, a check on every later one.
    PlanRoute
};

struct HostOptions {
    std::string   label        = "test-binding";
    std::string   origin       = std::string(kOpenRouter);
    std::string   model        = std::string(kModel);
    std::optional<std::uint64_t> input_limit = kInputLimit;
    std::optional<std::uint64_t> output_cap  = kOutputCap;
    std::optional<std::string>   service_tier;
    std::uint64_t monetary     = 1'000'000;  // run budget in micro-USD; 0 grants nothing
    std::uint64_t model_tokens = 1'000'000;
    std::uint64_t wall_time_ms = 10'000;
    bool          child_capable = false;     // recovery from checkpoints, not leases
    std::size_t   workers       = 1;
    bool          bind_provider = true;
    int           node_retries  = 0;
};

const ExecutableIdentity kProviderIdentity{ExecutableKind::Provider, "priced-provider", "1.0.0",
                                           digest('1')};

class Host {
public:
    Host(std::shared_ptr<Peer> peer, HostOptions options, const TempDir* files = nullptr)
        : peer_(std::move(peer)), script_(std::make_shared<Script>()),
          options_(std::move(options)), files_(files) {
        open();
    }
    ~Host() { close(); }

    void close() {
        runtime_.reset();
        catalog_.reset();
        journal_.reset();
    }

    ExecutableManifest manifest(ExecutableIdentity identity,
                                std::vector<ExecutableIdentity> requires_ = {}) const {
        return ExecutableManifest{std::move(identity), EffectMode::Brokered, "binding-test", {}, {},
                                  std::move(requires_)};
    }

    void open() {
        RegistrySnapshotBuilder builder;
        builder.add_provider(manifest(kProviderIdentity), ProviderMetadata{json::object(), json::object()});
        builder.add_core_llm_call(
            manifest({ExecutableKind::Node, "llm_call", "1.0.0", digest('2')}, {kProviderIdentity}), {});
        builder.add_host_brokered_node(
            manifest({ExecutableKind::Node, "brokered_call", "1.0.0", digest('3')}, {kProviderIdentity}),
            [script = script_](const std::string& name, const json&, const graph::NodeContext& context) {
                return std::make_unique<BrokeredCall>(name, context.provider, script);
            },
            json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
        builder.add_node(manifest({ExecutableKind::Node, "after", "1.0.0", digest('4')}),
                         [script = script_](const std::string& name, const json&, const graph::NodeContext&) {
                             return std::make_unique<AfterNode>(name, script);
                         },
                         json{{"type", "object"}}, json{{"writes", json::array({"done"})}});
        builder.add_node(manifest({ExecutableKind::Node, "check", "1.0.0", digest('8')}),
                         [script = script_](const std::string& name, const json&, const graph::NodeContext&) {
                             return std::make_unique<CheckNode>(name, script);
                         },
                         json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
        // A plan's first call has no final_response yet: it goes to the model.
        builder.add_condition(manifest({ExecutableKind::Condition, "plan_route", "1.0.0", digest('9')}),
                              [](const graph::GraphState& state) -> std::string {
                                  return state.get("final_response") == json("") ? "llm" : "check";
                              },
                              graph::ConditionSpec{{"check", "llm"}, false});
        builder.add_node(manifest({ExecutableKind::Node, "send_fan", "1.0.0", digest('7')}),
                         [](const std::string& name, const json&, const graph::NodeContext&) {
                             return std::make_unique<SendFan>(name);
                         },
                         json{{"type", "object"}}, json::object());
        builder.add_reducer(manifest({ExecutableKind::Reducer, "append", "1.0.0", digest('5')}),
                            [](const json& current, const json& incoming) {
                                json merged = current.is_array() ? current : json::array();
                                if (incoming.is_array())
                                    for (const auto& item : incoming) merged.push_back(item);
                                else
                                    merged.push_back(incoming);
                                return merged;
                            });
        builder.add_reducer(manifest({ExecutableKind::Reducer, "overwrite", "1.0.0", digest('6')}),
                            [](const json&, const json& incoming) { return incoming; });
        registry_.emplace(std::move(builder).build());

        AdmissionProfileBuilder profile;
        profile.id("binding-test-profile").semantic_version("1.0.0").registry(*registry_)
            .mode(AdmissionMode::MultiTenant).max_program_schema_version(2)
            .allow_source_kind(SourceKind::CppBuilder).allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry_->identities()) profile.allow_executable(identity);
        profile_.emplace(std::move(profile).build());

        std::shared_ptr<ProgramStore>           store;
        std::shared_ptr<graph::CheckpointStore> checkpoints;
        if (files_) {
            store        = std::make_shared<SQLiteProgramStore>(files_->program_db());
            transitions_ = std::make_shared<SQLiteProgramTransitionStore>(files_->program_db());
            checkpoints  = std::make_shared<graph::SqliteCheckpointStore>(files_->checkpoint_db());
            journal_     = std::make_shared<SQLiteProgramProviderCallJournal>(files_->provider_db());
        } else {
            store        = std::make_shared<InMemoryProgramStore>();
            transitions_ = std::make_shared<InMemoryProgramTransitionStore>();
            checkpoints  = std::make_shared<graph::InMemoryCheckpointStore>();
        }

        std::optional<graph::NodeContext> context;
        if (options_.bind_provider) {
            context.emplace();
            context->provider = make_provider(
                peer_, client_for(options_.origin, options_.model, options_.input_limit));
            context->model = options_.model;
            context->provider_controls.max_output_tokens = options_.output_cap;
            context->provider_controls.service_tier      = options_.service_tier;
        }
        CatalogConfig config{store, *registry_, std::make_shared<EngineGenerationCache>(),
                             "binding-test/v1"};
        config.worker_count      = options_.workers;
        config.capability_binder = program::detail::single_provider_binder(context, options_.label);
        catalog_ = std::make_shared<ProgramCatalog>(std::move(config));

        RuntimeConfig runtime;
        runtime.catalog           = catalog_;
        runtime.checkpoints       = checkpoints;
        runtime.transitions       = transitions_;
        runtime.scheduler_threads = 2;
        runtime.require_core_provider_call_broker = true;
        if (journal_) {
            runtime.core_provider_call_resolver =
                [journal = journal_, deployment = program::detail::provider_deployment_identity(options_.label)](
                    const ProgramCoreProviderCallContext& context)
                -> std::optional<ProgramCoreProviderCallBinding> {
                return journal->bind_priced(context, deployment);
            };
        } else {
            runtime.core_provider_call_resolver = [](const ProgramCoreProviderCallContext& context)
                -> std::optional<ProgramCoreProviderCallBinding> {
                return bind_priced_provider_call(context);
            };
        }
        runtime_ = std::make_unique<ProgramRuntime>(std::move(runtime));
    }

    json definition(Shape shape) const {
        json definition{{"schema_version", 1}, {"name", "main"}, {"conditional_edges", json::array()}};
        if (options_.node_retries)
            definition["retry_policy"] = {{"max_retries", options_.node_retries},
                                          {"initial_delay_ms", 0}, {"max_delay_ms", 0}};
        definition["channels"] = {{"messages", {{"reducer", "append"}}},
                                  {"value", {{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"done", {{"reducer", "overwrite"}, {"initial", false}}}};
        auto edge = [](const char* from, const char* to) { return json{{"from", from}, {"to", to}}; };
        switch (shape) {
            case Shape::AfterOnly:
                definition["nodes"] = {{"after", {{"type", "after"}}}};
                definition["edges"] = json::array({edge("__start__", "after"), edge("after", "__end__")});
                break;
            case Shape::BrokeredThenAfter:
                definition["nodes"] = {{"work", {{"type", "brokered_call"}}}, {"after", {{"type", "after"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "work"), edge("work", "after"), edge("after", "__end__")});
                break;
            case Shape::Llm:
                definition["nodes"] = {{"llm", {{"type", "llm_call"}}}};
                definition["edges"] = json::array({edge("__start__", "llm"), edge("llm", "__end__")});
                break;
            case Shape::LlmThenAfter:
                definition["nodes"] = {{"llm", {{"type", "llm_call"}}}, {"after", {{"type", "after"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "llm"), edge("llm", "after"), edge("after", "__end__")});
                break;
            case Shape::TwoLlms:
                definition["nodes"] = {{"left", {{"type", "llm_call"}}}, {"right", {{"type", "llm_call"}}}};
                definition["edges"] = json::array({edge("__start__", "left"), edge("__start__", "right"),
                                                   edge("left", "__end__"), edge("right", "__end__")});
                break;
            case Shape::Brokered:
                definition["nodes"] = {{"work", {{"type", "brokered_call"}}}};
                definition["edges"] = json::array({edge("__start__", "work"), edge("work", "__end__")});
                break;
            case Shape::PlanRoute:
                definition["channels"]["final_response"] = {{"reducer", "overwrite"}, {"initial", ""}};
                definition["nodes"] = {{"start", {{"type", "after"}}},
                                       {"llm", {{"type", "llm_call"}}},
                                       {"check", {{"type", "check"}}}};
                definition["edges"] = json::array({edge("__start__", "start"), edge("llm", "__end__"),
                                                   edge("check", "__end__")});
                definition["conditional_edges"] = json::array(
                    {json{{"from", "start"}, {"condition", "plan_route"},
                          {"routes", {{"llm", "llm"}, {"check", "check"}}}}});
                break;
            case Shape::SendsToBrokered:
                definition["nodes"] = {{"fan", {{"type", "send_fan"}}}, {"work", {{"type", "brokered_call"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "fan"), edge("fan", "__end__"), edge("work", "__end__")});
                break;
        }
        return definition;
    }

    ProgramBundle compile(Shape shape) const {
        const auto child = options_.child_capable ? 1 : 0;
        json document{
            {"program_schema_version", 2U},
            {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", definition(shape)}}},
            {"declared_budget_requirements",
             json::array({json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 100000}},
                          json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000000}},
                          json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000000}},
                          json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 2}},
                          json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
                          json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                          json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", child}},
                          json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", child}}})}};
        ProgramCompiler compiler(*registry_, {"binding-test/v1"});
        return compiler.compile(ProgramSource::from_cpp_builder("binding-test", 2, std::move(document)));
    }

    // #386: a plan over the Core of `shape`, with room for its operations.
    ProgramVersion admit_plan(json root, Shape shape) {
        root["name"]       = "main";
        root["definition"] = definition(shape);
        json document{
            {"program_schema_version", 2U},
            {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"root", std::move(root)},
            {"declared_budget_requirements",
             json::array({json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 100000}},
                          json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000000}},
                          json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000000}},
                          json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 2}},
                          json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 16}},
                          json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                          json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
        ProgramCompiler compiler(*registry_, {"binding-test/v1"});
        auto bundle = compiler.compile(ProgramSource::from_cpp_builder("binding-plan", 2, std::move(document)));
        PolicySnapshotBuilder policy;
        policy.id("binding-plan-policy").semantic_version("1.0.0").owner_scope(kOwner)
            .admission_profile(*profile_)
            .budget_ceiling(BudgetLimits{100000, 10000000, 1000000000, 2, 16, 20, 1, 0, 0});
        return catalog_->admit(bundle, ProgramAdmission{kOwner, *profile_, std::move(policy).build(), {}});
    }

    ProgramResult run_plan(const ProgramVersion& version, const std::string& run_id) {
        auto invocation   = this->invocation(run_id);
        invocation.budget = RunBudget{options_.wall_time_ms, options_.model_tokens, options_.monetary,
                                      2, 16, 20, 0, 0, 0};
        return runtime_->run(kOwner, version, std::move(invocation));
    }

    ProgramVersion admit(Shape shape) {
        const auto child = options_.child_capable ? 1U : 0U;
        PolicySnapshotBuilder policy;
        policy.id("binding-test-policy").semantic_version("1.0.0").owner_scope(kOwner)
            .admission_profile(*profile_)
            .budget_ceiling(BudgetLimits{100000, 10000000, 1000000000, 2, 1, 20, 1, child, child});
        return catalog_->admit(compile(shape), ProgramAdmission{kOwner, *profile_, std::move(policy).build(), {}});
    }

    RunBudget budget() const {
        const auto child = options_.child_capable ? 1U : 0U;
        return RunBudget{options_.wall_time_ms, options_.model_tokens, options_.monetary, 2, 1, 20, 0,
                         child, child};
    }

    ProgramInvocation invocation(const std::string& run_id) const {
        ProgramInvocation invocation{
            json{{"messages", json::array({json{{"role", "user"}, {"content", "question"}}})}},
            budget(), "trace-" + run_id, {}};
        invocation.requested_run_id = run_id;
        return invocation;
    }

    ProgramResult run(Shape shape, const std::string& run_id = "run-1") {
        const auto version = admit(shape);
        return runtime_->run(kOwner, version, invocation(run_id));
    }

    ProgramRuntime&                   runtime() { return *runtime_; }
    SQLiteProgramProviderCallJournal& journal() { return *journal_; }
    Script&                           script() { return *script_; }
    const HostOptions&                options() const { return options_; }

private:
    std::shared_ptr<Peer>                             peer_;
    std::shared_ptr<Script>                           script_;
    HostOptions                                       options_;
    const TempDir*                                    files_;
    std::optional<RegistrySnapshot>                   registry_;
    std::optional<AdmissionProfile>                   profile_;
    std::shared_ptr<ProgramTransitionStore>           transitions_;
    std::shared_ptr<SQLiteProgramProviderCallJournal> journal_;
    std::shared_ptr<ProgramCatalog>                   catalog_;
    std::unique_ptr<ProgramRuntime>                   runtime_;
};

std::string failure_text(const ProgramResult& result) {
    return result.failure() ? result.failure()->code + ": " + result.failure()->message : "no failure";
}

std::uint64_t expected_hold(std::string_view model = kModel, std::uint64_t input = kInputLimit,
                            std::uint64_t output = kOutputCap) {
    const auto reserved = PriceCatalog::builtin().reserve("openrouter", model, input, output);
    return std::get<pricing::Reservation>(reserved).microunits;
}

// ---------------------------------------------------------------------------
// Binder and identity
// ---------------------------------------------------------------------------

TEST(ProgramProviderBinding, BindingLabelIsAValidatedNonSecretName) {
    EXPECT_EQ(program::detail::validated_binding_label("openrouter-prod"), "openrouter-prod");
    EXPECT_EQ(program::detail::validated_binding_label("team.a:deepseek_1"), "team.a:deepseek_1");
    for (const auto* label : {"", "Upper", "-leading", "with space", "sk-or-v1-abc", "sk_live_1",
                              "label/slash"})
        EXPECT_THROW((void)program::detail::validated_binding_label(label), std::invalid_argument) << label;
    EXPECT_THROW((void)program::detail::validated_binding_label(std::string(65, 'a')), std::invalid_argument);
}

TEST(ProgramProviderBinding, AProgramWithoutAProviderKeepsItsIdsWhateverTheBinding) {
    auto        peer = std::make_shared<Peer>();
    HostOptions unbound;
    unbound.bind_provider = false;
    HostOptions relabeled;
    relabeled.label = "another-binding";
    Host plain(peer, unbound), bound(peer, HostOptions{}), other(peer, relabeled);
    const auto expected = plain.admit(Shape::AfterOnly);
    for (Host* host : {&bound, &other}) {
        const auto version = host->admit(Shape::AfterOnly);
        EXPECT_EQ(version.bundle_id(), expected.bundle_id());
        EXPECT_EQ(version.id(), expected.id());
        EXPECT_TRUE(version.core_materialization_receipt().capability_bindings.empty());
    }
    const auto result = bound.run(Shape::AfterOnly);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    EXPECT_TRUE(result.monetary_records().empty());
    EXPECT_EQ(result.usage().monetary_microunits, 0U);
    EXPECT_EQ(peer->transport.load(), 0);
}

TEST(ProgramProviderBinding, TheLabelEntersTheVersionIdentityTheKeyAndModelDoNot) {
    HostOptions prod;
    prod.label = "prod";
    HostOptions prod_other_model = prod;
    prod_other_model.model = "gpt-4.1-nano";
    HostOptions staging;
    staging.label = "staging";
    // Separate peers stand in for separate credentials.
    Host a(std::make_shared<Peer>(), prod), b(std::make_shared<Peer>(), prod_other_model),
        c(std::make_shared<Peer>(), staging);
    const auto va = a.admit(Shape::Llm);
    const auto vb = b.admit(Shape::Llm);
    const auto vc = c.admit(Shape::Llm);
    EXPECT_EQ(va.bundle_id(), vb.bundle_id());
    EXPECT_EQ(va.bundle_id(), vc.bundle_id());
    EXPECT_EQ(va.id(), vb.id()) << "the model and the provider object are not identity";
    EXPECT_NE(va.id(), vc.id()) << "the binding label is identity";
    const auto& receipts = va.core_materialization_receipt().capability_bindings;
    ASSERT_EQ(receipts.size(), 1U);
    EXPECT_EQ(receipts[0].executable, kProviderIdentity);
    EXPECT_EQ(receipts[0].binding_identity, program::detail::provider_binding_identity("prod", kProviderIdentity));
    const auto canonical = va.serialize_canonical();
    EXPECT_EQ(canonical.find("upstream-descriptor"), std::string::npos);
    EXPECT_EQ(canonical.find(std::string(kModel)), std::string::npos);
    EXPECT_EQ(canonical.find("prod"), std::string::npos) << "only the label's digest is stored";
}

TEST(ProgramProviderBinding, WithoutABoundProviderTheProgramIsRejectedAtAdmission) {
    HostOptions unbound;
    unbound.bind_provider = false;
    Host host(std::make_shared<Peer>(), unbound);
    try {
        (void)host.admit(Shape::Llm);
        FAIL() << "a provider-using Program must not be admitted without a binding";
    } catch (const ProgramAdmissionError& error) {
        ASSERT_FALSE(error.diagnostics().empty());
        EXPECT_EQ(error.diagnostics().front().code, "P_ADMIT_PROVIDER");
    }
}

// ---------------------------------------------------------------------------
// Reservation and settlement through the binding
// ---------------------------------------------------------------------------

class Priced : public ::testing::TestWithParam<bool> {
protected:
    bool durable() const { return GetParam(); }
    std::unique_ptr<Host> host(std::shared_ptr<Peer> peer, HostOptions options = {}) {
        return std::make_unique<Host>(std::move(peer), std::move(options), durable() ? &files_ : nullptr);
    }
    TempDir files_{"priced"};
};

TEST_P(Priced, AReportedCostSettlesTheCall) {
    auto       peer   = std::make_shared<Peer>();
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    ASSERT_EQ(result.monetary_records().size(), 1U);
    const auto& record = result.monetary_records().front();
    EXPECT_EQ(record.evidence, ProgramMonetaryEvidence::Reported);
    EXPECT_EQ(record.microunits, 1235U) << "1,234,567 nano-USD rounds up to whole microunits";
    EXPECT_EQ(record.reserved_microunits, expected_hold());
    EXPECT_EQ(record.basis, "provider_reported");
    EXPECT_EQ(record.origin, "openrouter");
    EXPECT_EQ(record.model, kModel);
    EXPECT_EQ(record.node, "llm");
    EXPECT_FALSE(record.call_id.empty());
    EXPECT_FALSE(record.replayed);
    EXPECT_TRUE(record.catalog_version.empty()) << "a reported amount needs no catalog";
    EXPECT_EQ(result.usage().monetary_microunits, 1235U);
    EXPECT_EQ(result.remaining_budget().monetary_microunits, 1'000'000U - 1235U);
    EXPECT_EQ(peer->transport.load(), 1);
    if (durable()) {
        const auto row = h->journal().inspect(kOwner, record.call_id);
        ASSERT_TRUE(row);
        ASSERT_TRUE(row->monetary_record);
        EXPECT_EQ(*row->monetary_record, record);
    }
    const auto stored = ProgramResult::parse(result.serialize_canonical());
    EXPECT_EQ(stored.monetary_records(), result.monetary_records());
}

TEST_P(Priced, WithoutAReportedCostTheEstimateNamesTheCatalog) {
    auto peer   = std::make_shared<Peer>();
    peer->reply = [](int, const ProviderRequest&) {
        return neograph::test::success("answer", final_usage(21, 3));
    };
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    ASSERT_EQ(result.monetary_records().size(), 1U);
    const auto& record   = result.monetary_records().front();
    const auto  expected = std::get<pricing::Settlement>(
        PriceCatalog::builtin().settle("openrouter", kModel, final_usage(21, 3)));
    EXPECT_EQ(record.evidence, ProgramMonetaryEvidence::Estimated);
    EXPECT_EQ(record.microunits, expected.microunits);
    EXPECT_LE(record.microunits, record.reserved_microunits);
    EXPECT_EQ(record.basis, "route_ceiling");
    EXPECT_EQ(record.catalog_version, PriceCatalog::builtin().version());
    EXPECT_EQ(record.catalog_digest, PriceCatalog::builtin().digest());
    EXPECT_EQ(result.usage().monetary_microunits, expected.microunits);
}

TEST_P(Priced, PartialUsageKeepsTheHold) {
    auto peer   = std::make_shared<Peer>();
    peer->reply = [](int, const ProviderRequest&) {
        return neograph::test::success(
            "answer", neograph::test::usage(21, std::nullopt, std::nullopt, sp::UsageStage::Partial));
    };
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    ASSERT_EQ(result.monetary_records().size(), 1U);
    EXPECT_EQ(result.monetary_records().front().evidence, ProgramMonetaryEvidence::Held);
    EXPECT_EQ(result.monetary_records().front().microunits, expected_hold());
    EXPECT_EQ(result.monetary_records().front().reason, "incomplete_usage");
    EXPECT_EQ(result.usage().monetary_microunits, expected_hold());
}

TEST_P(Priced, AFailureThatProvesNothingWasSentReleasesTheHold) {
    auto peer   = std::make_shared<Peer>();
    peer->reply = [](int, const ProviderRequest&) {
        sp::Failure failure;
        failure.error.kind         = sp::ErrorKind::Transport;
        failure.error.retry_safety = sp::RetrySafety::NotSent;
        return std::make_shared<const sp::Outcome>(std::move(failure));
    };
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_TRUE(result.monetary_records().empty());
    EXPECT_EQ(result.usage().monetary_microunits, 0U);
    EXPECT_EQ(result.remaining_budget().monetary_microunits, 1'000'000U);
}

TEST_P(Priced, AFailureAfterTheRequestMayHaveLeftKeepsTheHold) {
    auto peer   = std::make_shared<Peer>();
    peer->reply = [](int, const ProviderRequest&) {
        sp::Failure failure;
        failure.error.kind                           = sp::ErrorKind::Transport;
        failure.error.retry_safety                   = sp::RetrySafety::PossiblyAccepted;
        failure.error.attempt.request_may_have_left  = true;
        return std::make_shared<const sp::Outcome>(std::move(failure));
    };
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_EQ(result.monetary_records().size(), 1U);
    EXPECT_EQ(result.monetary_records().front().evidence, ProgramMonetaryEvidence::Held);
    EXPECT_EQ(result.usage().monetary_microunits, expected_hold());
}

// ---------------------------------------------------------------------------
// Fail closed: a budgeted run refuses a call it cannot bound, before sending it
// ---------------------------------------------------------------------------

struct Refusal {
    const char* name;
    std::function<void(HostOptions&)> change;
    const char* code;
    ProgramTerminalStatus status;
};

TEST_P(Priced, ARunWithACeilingRefusesWhatItCannotBound) {
    const std::vector<Refusal> cases{
        {"unknown model", [](HostOptions& o) { o.model = "vendor/unlisted-model"; },
         "P_MONEY_UNKNOWN_MODEL", ProgramTerminalStatus::Failed},
        {"unknown origin", [](HostOptions& o) { o.origin = "https://fixture.invalid"; },
         "P_MONEY_UNKNOWN_ORIGIN", ProgramTerminalStatus::Failed},
        {"router alias without a route ceiling",
         [](HostOptions& o) { o.model = "~deepseek/deepseek-v4-flash-latest"; },
         "P_MONEY_NO_UPPER_BOUND", ProgramTerminalStatus::Failed},
        {"no output cap", [](HostOptions& o) { o.output_cap.reset(); }, "P_MONEY_NO_OUTPUT_CAP",
         ProgramTerminalStatus::Failed},
        {"unmodeled service tier", [](HostOptions& o) { o.service_tier = "flex"; },
         "P_MONEY_UNMODELED", ProgramTerminalStatus::Failed},
        {"reservation larger than the budget",
         [](HostOptions& o) { o.monetary = expected_hold() - 1; }, "P_MONEY_BUDGET",
         ProgramTerminalStatus::BudgetExhausted},
    };
    for (const auto& refusal : cases) {
        SCOPED_TRACE(refusal.name);
        TempDir     files(std::string("refusal"));
        auto        peer = std::make_shared<Peer>();
        HostOptions options;
        refusal.change(options);
        Host       h(peer, options, durable() ? &files : nullptr);
        const auto result = h.run(Shape::Llm);
        EXPECT_EQ(result.status(), refusal.status);
        ASSERT_TRUE(result.failure()) << "the run must fail with a typed diagnostic";
        EXPECT_EQ(result.failure()->code, refusal.code) << failure_text(result);
        EXPECT_TRUE(result.failure()->witness.contains("monetary"));
        EXPECT_EQ(peer->transport.load(), 0) << "no request may reach the provider";
        EXPECT_TRUE(result.monetary_records().empty());
        EXPECT_EQ(result.usage().monetary_microunits, 0U);
        if (durable()) EXPECT_TRUE(journal_states(files.provider_db()).empty()) << "no marker";
    }
}

TEST(ProgramProviderBinding, AModelWithoutAKnownInputLimitCannotBeBounded) {
    // A catalog entry with no limits, and an SDK policy that does not know the model.
    const auto catalog = PriceCatalog::parse(R"({
      "schema": "neograph.pricing.catalog", "schema_version": 1, "version": "test.1",
      "currency": "USD", "rate_unit": "usd_per_million_tokens",
      "origins": {"openrouter": {"url": "https://openrouter.ai", "routing": "aggregator"}},
      "entries": [{"origin": "openrouter", "model": "vendor/limitless",
        "source_url": "https://openrouter.ai/api/v1/models", "retrieved": "2026-10-11",
        "tiers": [{"input": "1", "output": "2"}],
        "route_ceiling": {"source_url": "https://openrouter.ai/api/v1/models", "retrieved": "2026-10-11",
                          "input": "3", "output": "4"}}]})");
    auto peer     = std::make_shared<Peer>();
    auto provider = make_provider(peer, client_for(std::string(kOpenRouter), "vendor/limitless", std::nullopt));
    auto request  = make_provider_request(*provider, "vendor/limitless",
                                          {neograph::test::message("q", sp::Role::User)});
    std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
    const auto prepared = provider->prepare(std::move(request));
    ASSERT_TRUE(prepared.valid());
    const auto quote = program::detail::assess_monetary_call(catalog, std::nullopt, prepared);
    EXPECT_EQ(quote.blocker, "P_MONEY_NO_INPUT_BOUND");
    EXPECT_EQ(quote.hold, 0U);
    ASSERT_TRUE(quote.pin);
    EXPECT_EQ(quote.pin->prompt, "3");
    EXPECT_EQ(quote.pin->completion, "4");
}

TEST(ProgramProviderBinding, TheRouteCeilingIsTheReservationAndThePinnedPrice) {
    const auto* entry = PriceCatalog::builtin().find("openrouter", kModel);
    ASSERT_TRUE(entry && entry->route_ceiling);
    const auto price = program::detail::route_max_price(*entry);
    ASSERT_TRUE(price);
    EXPECT_EQ(price->prompt, "0.44");
    EXPECT_EQ(price->completion, "1.32");
    const auto reserved = PriceCatalog::builtin().reserve("openrouter", kModel, kInputLimit, kOutputCap);
    EXPECT_EQ(std::get<pricing::Reservation>(reserved).basis, pricing::Basis::RouteCeiling);
    // Until the pinned SDK carries provider.max_price the pin cannot be sent.
    auto request = neograph::test::request(std::string(kModel));
    EXPECT_FALSE(program::detail::pin_route_max_price(request, *price));
}

// ---------------------------------------------------------------------------
// monetary_microunits == 0 grants nothing, like every RunBudget amount
// ---------------------------------------------------------------------------

TEST_P(Priced, AZeroMonetaryBudgetRefusesEveryPricedCall) {
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary  = 0;
    auto       h      = host(peer, options);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_MONEY_BUDGET");
    EXPECT_EQ(peer->transport.load(), 0);
    EXPECT_EQ(result.usage().monetary_microunits, 0U);
}

TEST(ProgramProviderBinding, ModelTokensZeroAlreadyMeantNoneForACompletedRun) {
    // The meaning monetary_microunits = 0 keeps: a completed run that used
    // model tokens against a zero grant is reported BudgetExhausted.
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.model_tokens = 0;
    Host       h(peer, options);
    const auto result = h.run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    EXPECT_EQ(result.usage().model_tokens, 24U);
}

TEST_P(Priced, AReportedCostAboveTheCeilingExhaustsTheRun) {
    auto peer   = std::make_shared<Peer>();
    const auto ceiling = expected_hold();
    peer->reply = [ceiling](int, const ProviderRequest&) {
        // A dearer endpoint than the catalog's ceiling (possible until max_price is pinned).
        return neograph::test::success("answer", with_cost(final_usage(21, 3), (ceiling + 10) * 1000));
    };
    HostOptions options;
    options.monetary  = ceiling;
    auto       h      = host(peer, options);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    EXPECT_EQ(result.usage().monetary_microunits, ceiling + 10);
    EXPECT_EQ(result.remaining_budget().monetary_microunits, 0U);
}

// ---------------------------------------------------------------------------
// Concurrency, retries, cancellation and timeouts
// ---------------------------------------------------------------------------

TEST_P(Priced, ConcurrentCallsReserveAgainstOneBudget) {
    {
        auto        peer = std::make_shared<Peer>();
        HostOptions options;
        options.workers  = 2;
        options.monetary = expected_hold() * 2;
        TempDir    files("concurrent-fit");
        Host       h(peer, options, durable() ? &files : nullptr);
        const auto result = h.run(Shape::TwoLlms);
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
        EXPECT_EQ(peer->transport.load(), 2);
        ASSERT_EQ(result.monetary_records().size(), 2U);
        EXPECT_NE(result.monetary_records()[0].call_id, result.monetary_records()[1].call_id);
        EXPECT_EQ(result.usage().monetary_microunits, 2U * 1235U);
    }
    {
        auto        peer = std::make_shared<Peer>();
        HostOptions options;
        options.workers  = 2;
        options.monetary = expected_hold() + expected_hold() / 2;
        // Both reservations are made while both calls are in flight.
        std::atomic<int> arrived{0};
        peer->on_transport = [&arrived](int, const ProviderRequest&) {
            ++arrived;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        };
        TempDir    files("concurrent-over");
        Host       h(peer, options, durable() ? &files : nullptr);
        const auto result = h.run(Shape::TwoLlms);
        EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
        ASSERT_TRUE(result.failure());
        EXPECT_EQ(result.failure()->code, "P_MONEY_BUDGET");
        EXPECT_EQ(peer->transport.load(), 1) << "only the call that fits is sent";
        EXPECT_LE(result.usage().monetary_microunits, options.monetary);
    }
}

TEST_P(Priced, ANodeRetryIsANewCallAndIsChargedAgain) {
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.node_retries = 1;
    auto h = host(peer, options);
    h->script().fail_first_attempts = 1;
    const auto result = h->run(Shape::Brokered);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    EXPECT_EQ(peer->transport.load(), 2);
    ASSERT_EQ(result.monetary_records().size(), 2U);
    EXPECT_NE(result.monetary_records()[0].call_id, result.monetary_records()[1].call_id)
        << "attempt k calls under its own effect namespace";
    EXPECT_EQ(result.usage().monetary_microunits, 2U * 1235U);
    if (durable()) EXPECT_EQ(journal_states(files_.provider_db()).size(), 2U);
}

TEST_P(Priced, ACancelledCallKeepsItsHold) {
    auto peer = std::make_shared<Peer>();
    std::atomic<bool> in_flight{false};
    peer->on_transport = [&in_flight](int, const ProviderRequest& request) {
        in_flight = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until &&
               !(request.cancel_token && request.cancel_token->is_cancelled()))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    peer->reply = [](int, const ProviderRequest&) {
        sp::Failure failure;
        failure.error.kind                          = sp::ErrorKind::Cancelled;
        failure.error.retry_safety                  = sp::RetrySafety::PossiblyAccepted;
        failure.error.attempt.request_may_have_left = true;
        return std::make_shared<const sp::Outcome>(std::move(failure));
    };
    auto       h       = host(peer);
    const auto version = h->admit(Shape::Llm);
    auto       handle  = h->runtime().start(kOwner, version, h->invocation("cancelled-run"));
    const auto until   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!in_flight && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(in_flight);
    handle.cancel();
    const auto result = handle.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled) << failure_text(result);
    EXPECT_EQ(result.usage().monetary_microunits, expected_hold())
        << "a call that may have reached the provider is charged its hold";
    for (const auto& record : result.monetary_records())
        EXPECT_EQ(record.evidence, ProgramMonetaryEvidence::Held);
}

TEST_P(Priced, ATimedOutCallKeepsItsHold) {
    auto peer = std::make_shared<Peer>();
    peer->on_transport = [](int, const ProviderRequest& request) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until &&
               !(request.cancel_token && request.cancel_token->is_cancelled()))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    peer->reply = [](int, const ProviderRequest&) {
        sp::Failure failure;
        failure.error.kind                          = sp::ErrorKind::DeadlineExceeded;
        failure.error.retry_safety                  = sp::RetrySafety::PossiblyAccepted;
        failure.error.attempt.request_may_have_left = true;
        return std::make_shared<const sp::Outcome>(std::move(failure));
    };
    HostOptions options;
    options.wall_time_ms = 300;
    auto       h      = host(peer, options);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << failure_text(result);
    EXPECT_EQ(result.usage().monetary_microunits, expected_hold());
}

INSTANTIATE_TEST_SUITE_P(Brokers, Priced, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "SQLiteJournal" : "InProcess"; });

// ---------------------------------------------------------------------------
// The SQLite journal: replay never charges twice, a settled charge survives
// ---------------------------------------------------------------------------

ProgramResume resume_after(const ProgramResult& interrupted, std::string trace) {
    const auto interrupt = interrupted.interrupt();
    if (!interrupt || !interrupt->pending_input) throw std::logic_error("run is not waiting");
    return ProgramResume{json::object(), std::move(trace), {}, interrupt->pending_input->call_id()};
}

TEST(ProgramProviderBindingJournal, AnOutcomeReplayedByALaterAttemptIsNotChargedAgain) {
    TempDir     files("replay-attempt");
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary = 100'000;
    Host h(peer, options, &files);
    h.script().interrupt_after_call = true;
    const auto first = h.run(Shape::Brokered, "approval-run");
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_text(first);
    ASSERT_EQ(first.monetary_records().size(), 1U);
    EXPECT_EQ(first.usage().monetary_microunits, 1235U);
    EXPECT_EQ(first.remaining_budget().monetary_microunits, 100'000U - 1235U);

    const auto second =
        h.runtime().resume(kOwner, "approval-run", resume_after(first, "trace-resume")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Completed) << failure_text(second);
    EXPECT_EQ(second.attempt(), 2U);
    EXPECT_EQ(peer->transport.load(), 1) << "attempt 2 replays the journaled outcome";
    EXPECT_TRUE(second.monetary_records().empty()) << "attempt 1 already charged it";
    EXPECT_EQ(second.usage().monetary_microunits, 0U);
    EXPECT_EQ(second.remaining_budget().monetary_microunits, 100'000U - 1235U);
}

TEST(ProgramProviderBindingJournal, AResumedSendReplaysItsJournaledCall) {
    // #385: two Send targets call the model and then ask. The run pauses on
    // the first; resume runs both again under the same task ids, so their
    // journaled calls replay instead of being sent and charged again.
    TempDir     files("send-replay");
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary = 100'000;
    options.workers  = 2;
    Host h(peer, options, &files);
    h.script().interrupt_after_call = true;
    const auto first = h.run(Shape::SendsToBrokered, "send-run");
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_text(first);
    ASSERT_TRUE(first.interrupt().has_value());
    EXPECT_EQ(first.interrupt()->core_node, "work");
    EXPECT_EQ(peer->transport.load(), 2);
    EXPECT_EQ(first.monetary_records().size(), 2U);
    EXPECT_EQ(first.usage().monetary_microunits, 2U * 1235U);

    const auto second =
        h.runtime().resume(kOwner, "send-run", resume_after(first, "trace-send-resume")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Completed) << failure_text(second);
    EXPECT_EQ(peer->transport.load(), 2) << "each Send's journaled call replays; nothing is resent";
    EXPECT_TRUE(second.monetary_records().empty()) << "attempt 1 already charged both calls";
    EXPECT_EQ(second.usage().monetary_microunits, 0U);
    EXPECT_EQ(second.remaining_budget().monetary_microunits, 100'000U - 2U * 1235U);
    EXPECT_EQ(h.script().node_runs.load(), 4) << "both Sends ran once more, with the answer";
    EXPECT_EQ(journal_states(files.provider_db()),
              (std::vector<std::string>{"succeeded", "succeeded"}));
}

TEST(ProgramProviderBindingJournal, APlanResumeNeverResendsOrRechargesTheCallBeforeThePause) {
    // #386: a plan sequence[A, parallel[B, C], branch]: A's Core calls the
    // model, B asks, C finishes, and the branch reads A's reply as carried by
    // C. Resume answers B. A is replayed: its request is not sent again, its
    // tokens and money are charged once, and the branch takes the arm A's
    // reply chose. A rerun would get another reply and take the other arm.
    for (const bool durable : {false, true}) {
        SCOPED_TRACE(durable ? "SQLite journal" : "in memory");
        TempDir files("plan-resume");
        auto    peer = std::make_shared<Peer>();
        peer->reply  = [](int ordinal, const ProviderRequest&) {
            return neograph::test::success(ordinal == 1 ? "Paris." : "Rome.",
                                           with_cost(final_usage(21, 3), 1'234'567));
        };
        HostOptions options;
        options.monetary = 100'000;
        options.workers  = 2;
        Host       h(peer, options, durable ? &files : nullptr);
        const auto call    = json{{"op", "call_core"}};
        const auto version = h.admit_plan(
            json{{"op", "sequence"},
                 {"children",
                  json::array(
                      {call,
                       json{{"op", "parallel"}, {"branches", json::array({call, call})}},
                       json{{"op", "branch"},
                            {"condition", json{{"path", "/1/channels/final_response/value"},
                                               {"equals", "Paris."}}},
                            {"then", json{{"op", "return"}, {"value", json{{"arm", "then"}}}}},
                            {"else", json{{"op", "return"}, {"value", json{{"arm", "else"}}}}}}})}},
            Shape::PlanRoute);
        // B is branch 0 of a parallel at the plan's root: it runs on the root thread.
        h.script().ask_thread = program_root_core_thread_id(
            "plan-run", version.core_materialization_receipt().plans.front().compiled_plan_identity);

        const auto paused = h.run_plan(version, "plan-run");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
        ASSERT_TRUE(paused.interrupt() && paused.interrupt()->pending_input);
        EXPECT_EQ(paused.interrupt()->pending_input->operation_id(), "root.1.0");
        EXPECT_EQ(peer->transport.load(), 1);
        EXPECT_EQ(paused.usage().model_tokens, 24U);
        EXPECT_EQ(paused.usage().monetary_microunits, 1235U);
        EXPECT_EQ(h.script().checks_asked.load(), 1);
        EXPECT_EQ(h.script().checks_finished.load(), 1);

        const auto done =
            h.runtime().resume(kOwner, "plan-run", resume_after(paused, "trace-plan-resume")).wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
        EXPECT_EQ(done.output(), json({{"arm", "then"}})) << "the branch takes the arm A's reply chose";
        EXPECT_EQ(peer->transport.load(), 1) << "A's request is never sent again";
        EXPECT_EQ(done.usage().model_tokens, 0U);
        EXPECT_EQ(done.usage().monetary_microunits, 0U);
        EXPECT_TRUE(done.monetary_records().empty()) << "A was charged by the attempt that ran it";
        EXPECT_EQ(done.remaining_budget().model_tokens, options.model_tokens - 24U);
        EXPECT_EQ(done.remaining_budget().monetary_microunits, 100'000U - 1235U);
        EXPECT_EQ(h.script().checks_asked.load(), 1);
        EXPECT_EQ(h.script().checks_finished.load(), 1) << "C is replayed, not run again";
        EXPECT_EQ(h.script().checks_answered.load(), 1);
    }
}

TEST(ProgramProviderBindingJournal, InMemoryAResumedNodeThatCallsAgainPaysAgain) {
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary = 100'000;
    Host h(peer, options);
    h.script().interrupt_after_call = true;
    const auto first = h.run(Shape::Brokered, "approval-run");
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_text(first);
    const auto second =
        h.runtime().resume(kOwner, "approval-run", resume_after(first, "trace-resume")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Completed) << failure_text(second);
    EXPECT_EQ(peer->transport.load(), 2) << "without a journal the request is really sent again";
    EXPECT_EQ(second.usage().monetary_microunits, 1235U);
    EXPECT_EQ(second.remaining_budget().monetary_microunits, 100'000U - 2U * 1235U);
}

TEST(ProgramProviderBindingJournal, AProcessThatDiesAfterTheCallIsJournaledIsChargedOnce) {
    TempDir     files("crash");
    TempDir     image("crash-image");
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.child_capable = true;  // recovery resumes from the run's checkpoint
    options.monetary      = 100'000;
    {
        Host h(peer, options, &files);
        // Attempt 1 pauses before the call, so the run has a durable checkpoint.
        h.script().interrupt_before_call = true;
        bool taken = false;
        h.script().after = [&](const std::string&) {
            if (taken) return;
            taken = true;
            // The model call is journaled; the process "dies" here, mid-attempt.
            crash_image(files.path, image.path);
        };
        const auto paused = h.run(Shape::BrokeredThenAfter, "crash-run");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
        ASSERT_EQ(peer->transport.load(), 0);
        const auto done =
            h.runtime().resume(kOwner, "crash-run", resume_after(paused, "trace-crash")).wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
        ASSERT_TRUE(taken);
        ASSERT_EQ(peer->transport.load(), 1);
    }
    ASSERT_EQ(journal_states(image.provider_db()), std::vector<std::string>{"succeeded"});
    {
        SQLiteProgramTransitionStore transitions(image.program_db());
        const auto record = transitions.load(kOwner, "crash-run");
        ASSERT_TRUE(record);
        EXPECT_EQ(record->continuation().state, ContinuationState::Running);
        EXPECT_EQ(record->continuation().attempt, 2U);
    }
    auto recovery = std::make_shared<Peer>();
    Host restarted(recovery, options, &image);
    restarted.script().interrupt_before_call = true;
    const auto result = restarted.runtime().reconnect(kOwner, "crash-run").wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    EXPECT_EQ(result.attempt(), 2U);
    EXPECT_EQ(recovery->transport.load(), 0) << "the journaled request is not sent again";
    ASSERT_EQ(result.monetary_records().size(), 1U) << "charged once";
    EXPECT_TRUE(result.monetary_records().front().replayed);
    EXPECT_EQ(result.monetary_records().front().evidence, ProgramMonetaryEvidence::Reported);
    EXPECT_EQ(result.usage().monetary_microunits, 1235U) << "and not lost";
    EXPECT_EQ(result.remaining_budget().monetary_microunits, 100'000U - 1235U);
}

TEST(ProgramProviderBindingJournal, AnExpiredLeaseStillChargesTheJournaledCall) {
    // A define()-only run without child budgets recovers through its root
    // execution lease: once the lease expires the run ends TimedOut without
    // running again, and the call it journaled must still be charged.
    TempDir     files("lease");
    TempDir     image("lease-image");
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.wall_time_ms = 1500;
    {
        Host h(peer, options, &files);
        bool taken = false;
        h.script().after = [&](const std::string&) {
            if (taken) return;
            taken = true;
            crash_image(files.path, image.path);
        };
        const auto done = h.run(Shape::LlmThenAfter, "lease-run");
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
        ASSERT_TRUE(taken);
    }
    ASSERT_EQ(journal_states(image.provider_db()), std::vector<std::string>{"succeeded"});
    auto recovery = std::make_shared<Peer>();
    Host restarted(recovery, options, &image);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::optional<ProgramResult> result;
    while (!result && std::chrono::steady_clock::now() < until) {
        try {
            result = restarted.runtime().reconnect(kOwner, "lease-run").wait();
        } catch (const ProgramDiagnosticError& error) {
            ASSERT_EQ(error.diagnostic().code, "P_RUN_LEASE_HELD");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::TimedOut) << failure_text(*result);
    EXPECT_EQ(recovery->transport.load(), 0);
    ASSERT_EQ(result->monetary_records().size(), 1U);
    EXPECT_TRUE(result->monetary_records().front().replayed);
    EXPECT_EQ(result->usage().monetary_microunits, 1235U);
    EXPECT_EQ(result->remaining_budget().monetary_microunits, 1'000'000U - 1235U);
}

TEST(ProgramProviderBindingJournal, AnUncertainDispatchIsHeldAcrossARestart) {
    TempDir     files("uncertain");
    auto        peer = std::make_shared<Peer>();
    peer->on_transport = [](int, const ProviderRequest&) {
        throw std::runtime_error("link lost after the request was sent");
    };
    HostOptions options;
    options.monetary = 100'000;
    std::string call_id;
    {
        Host       h(peer, options, &files);
        const auto failed = h.run(Shape::Llm, "uncertain-run");
        EXPECT_EQ(failed.status(), ProgramTerminalStatus::Failed);
        ASSERT_EQ(failed.monetary_records().size(), 1U);
        EXPECT_EQ(failed.monetary_records().front().evidence, ProgramMonetaryEvidence::Held);
        EXPECT_EQ(failed.usage().monetary_microunits, expected_hold());
        call_id = failed.monetary_records().front().call_id;
        const auto row = h.journal().inspect(kOwner, call_id);
        ASSERT_TRUE(row && row->monetary_record);
        EXPECT_EQ(row->state, SQLiteProgramProviderCallJournal::State::ReconciliationRequired);
        EXPECT_EQ(row->monetary_record->evidence, ProgramMonetaryEvidence::Held);
        EXPECT_EQ(row->monetary_record->microunits, expected_hold());
    }
    EXPECT_EQ(peer->transport.load(), 1);
}

}  // namespace
