// A host-bound provider for Program llm_call nodes, priced against the run's
// monetary budget (#372).
//
// Every run goes through ProgramRuntime with the binder and the brokers a host
// uses: program::detail::single_provider_binder (what the Python LocalProgramHost
// installs), bind_priced_provider_call for in-memory hosts and the SQLite
// provider journal for durable ones. The provider is a local double speaking
// through a real SDK client, so origin, model, output cap and input limits come
// from an admitted descriptor exactly as they do for a hosted model.
#include "fixtures/fork_share.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/program.h>
#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>

#include "fixtures/typed_provider.h"
#include "fixtures/endpoint_peer.h"
#include "fixtures/typed_wire_peer.h"
#include "monetary.h"
#include "provider_binding.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    explicit TempDir(const std::string& name)
        : dir("neograph-provider-binding-" + name + "-"), path(dir.path()) {}
    std::string provider_db() const { return (path / "provider.sqlite").string(); }
    std::string program_db() const { return (path / "program.sqlite").string(); }
    std::string checkpoint_db() const { return (path / "checkpoint.sqlite").string(); }
    neograph::graph::test_support::UniqueTempDir dir;
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

// The prices a host's catalogue layer gives the fixture models. kModel lists
// an OpenRouter route ceiling of 0.44 and 1.32 USD per million tokens; the
// router alias lists none.
json prices_of(std::string_view model) {
    if (model == kModel || model == "~deepseek/deepseek-v4-flash-latest") {
        json prices{{"input", "0.000000018"}, {"cache_read", "0.000000018"}, {"output", "0.00000064"}};
        if (model == kModel) {
            prices["ceiling_input"]  = "0.00000044";
            prices["ceiling_output"] = "0.00000132";
        }
        return prices;
    }
    if (model == "gpt-4.1-nano")
        return json{{"input", "0.0000001"}, {"cache_read", "0.000000025"}, {"output", "0.0000004"}};
    if (model == "vendor/limitless")
        return json{{"input", "0.000001"}, {"output", "0.000002"},
                    {"ceiling_input", "0.000003"}, {"ceiling_output", "0.000004"}};
    return json::object();
}

// A fixture model with its prices, scoped to the origins the tests price.
neograph::test::FixtureModel priced_model(std::string family, const std::string& model,
                                          std::optional<std::uint64_t> input_limit,
                                          std::vector<std::string> origins = {std::string(kOpenRouter),
                                                                              "https://api.openai.com"}) {
    neograph::test::FixtureModel fixture{std::move(family), model, input_limit};
    fixture.pricing = prices_of(model);
    fixture.origins = std::move(origins);
    return fixture;
}

// Where the openai.chat family is in a descriptor policy document.
std::size_t chat_family_index(const json& policy) {
    const json families = policy["families"];
    for (std::size_t i = 0; i < families.size(); ++i)
        if (families[i]["family"].get<std::string>() == "openai.chat") return i;
    throw std::logic_error("the policy has no openai.chat family");
}

// Edits of the descriptor policy that change its identity, which covers the facts that decide how sealed
// history is written to and read from the wire: the stop reasons and the member names a request is written to.
std::function<void(json&)> another_stop_reason(std::string raw) {
    return [raw = std::move(raw)](json& policy) {
        policy["families"][chat_family_index(policy)]["stop_reasons"][raw] = "EndTurn";
    };
}
std::function<void(json&)> another_output_cap_member(std::string member) {
    return [member = std::move(member)](json& policy) {
        policy["families"][chat_family_index(policy)]["bindings"]["max_output_tokens"] = member;
    };
}

// The SDK policy with an input limit and prices for `model`, edited by `edit`; `origin` is the base URL.
std::shared_ptr<sp::runtime::Client> client_for(std::string origin, const std::string& model,
                                                std::optional<std::uint64_t> input_limit,
                                                const std::function<void(json&)>& edit = {}) {
    std::vector<neograph::test::FixtureModel> models;
    if (input_limit || !prices_of(model).empty()) models.push_back(priced_model("openai.chat", model, input_limit));
    auto policy = json::parse(sp::config_defaults::descriptor_policy_json);
    if (edit) edit(policy);
    return std::make_shared<sp::runtime::Client>(neograph::test::descriptor(
        "openai.chat", std::move(origin), neograph::test::policy_with_models(policy, models)));
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
    // The encoded body each request sent, and its Provider::request_digest, request_content_digest and
    // request_policy_identity.
    std::vector<std::string> bodies;
    std::vector<std::string> digests;
    std::vector<std::string> contents;
    std::vector<std::string> policies;
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
        [peer](ProviderRequest request, const PreparedProviderRequest& prepared, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            const int ordinal = ++peer->transport;
            {
                std::lock_guard lock(peer->mutex);
                peer->models.push_back(std::get<sp::chat::Request>(request.payload).model);
                peer->bodies.emplace_back(prepared.encoded_body());
                peer->digests.push_back(Provider::request_digest(prepared));
                peer->contents.push_back(Provider::request_content_digest(prepared));
                peer->policies.push_back(Provider::request_policy_identity(prepared));
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
    // #387: the "check" node also asks on this run of an unanswered check
    // (1-based; 0 never), whatever its thread.
    std::atomic<int> ask_on_check_run{0};
    std::atomic<int> check_runs{0};
    std::atomic<int> checks_asked{0};
    std::atomic<int> checks_finished{0};
    std::atomic<int> checks_answered{0};
    // #541: what the "file_upload" node uploads: `upload_size` bytes of the file at `upload_path`, or
    // `upload_bytes` from memory when there are any.
    std::string   upload_path;
    std::uint64_t upload_size = 0;
    std::string   upload_bytes;
    // #541: the "chunked_upload" node sends the same file in parts of `chunk_size` bytes, and asks for approval
    // after this many of its calls (the one that opens the session counts) until it is resumed.
    std::uint64_t chunk_size = 0;
    std::atomic<int> interrupt_after_calls{0};
    // #543: what the "brokered_call" node asks, and the system text it puts before it (none when empty).
    std::string   prompt = "probe";
    std::string   system;
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
        std::vector<sp::Message> messages;
        if (!script_->system.empty()) messages.push_back(neograph::test::message(script_->system, sp::Role::System));
        messages.push_back(neograph::test::message(script_->prompt, sp::Role::User));
        auto request  = make_provider_request(*provider_, std::string(kModel), std::move(messages));
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

// #541: uploads a file through the Program provider broker, then asks until it is resumed.
class UploadCall final : public graph::GraphNode {
public:
    UploadCall(std::string name, std::shared_ptr<Provider> provider, std::shared_ptr<Script> script)
        : name_(std::move(name)), provider_(std::move(provider)), script_(std::move(script)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        ++script_->node_runs;
        sp::files::Upload upload;
        if (script_->upload_bytes.empty())
            upload.source.file = {script_->upload_path, 0, script_->upload_size};
        else
            upload.source.bytes = std::make_shared<const std::string>(script_->upload_bytes);
        upload.filename = "data.bin";
        upload.mime     = "application/octet-stream";
        upload.purpose  = "user_data";
        const auto broker = graph::provider_call_broker(in.ctx);
        if (!broker) throw std::logic_error("the Program provider broker is absent");
        auto result = co_await graph::observe_provider_result(
            in.ctx, broker->invoke(graph::make_provider_call_identity(in.ctx, name_), provider_,
                                   make_endpoint_request(std::move(upload))));
        if (!result) throw std::runtime_error("the upload returned no outcome");
        const auto outcome = sp::files::typed(*result);
        if (const auto* failure = std::get_if<sp::Failure>(&outcome)) throw std::runtime_error(failure->error.safe_message);
        if (script_->interrupt_after_call.load() && !in.ctx.resume_value)
            throw graph::NodeInterrupt("approve the upload", json{{"request", "approve"}});
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", std::get<sp::files::File>(outcome).id});
        co_return output;
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    std::shared_ptr<Script>   script_;
};

// #541: uploads a file in parts through the Program provider broker. One call opens the session and one sends each
// part, at call ordinals 0, 1, ... The session, which holds the upload's secret URL, lives only in this frame.
class ChunkedUploadCall final : public graph::GraphNode {
public:
    ChunkedUploadCall(std::string name, std::shared_ptr<Provider> provider, std::shared_ptr<Script> script)
        : name_(std::move(name)), provider_(std::move(provider)), script_(std::move(script)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        ++script_->node_runs;
        const bool          in_memory = !script_->upload_bytes.empty();
        const std::uint64_t total     = in_memory ? script_->upload_bytes.size() : script_->upload_size;
        int                 calls     = 0;
        const auto          pause     = [&] {
            if (++calls == script_->interrupt_after_calls.load() && !in.ctx.resume_value)
                throw graph::NodeInterrupt("approve the upload", json{{"request", "approve"}});
        };
        auto progress = co_await step(in, 0, make_endpoint_request(sp::files::Start{
            "data.bin", "application/octet-stream", total}));
        pause();
        for (std::uint64_t ordinal = 1; progress.session.sent < progress.session.size; ++ordinal) {
            sp::files::Part part;
            part.session      = progress.session;
            const auto size   = std::min<std::uint64_t>(script_->chunk_size, total - part.session.sent);
            if (in_memory)
                part.source.bytes = std::make_shared<const std::string>(
                    script_->upload_bytes.substr(part.session.sent, size));
            else
                part.source.file = {script_->upload_path, part.session.sent, size};
            progress = co_await step(in, ordinal, make_endpoint_request(std::move(part)));
            pause();
        }
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", progress.file ? progress.file->id : std::string()});
        co_return output;
    }

private:
    asio::awaitable<sp::files::Progress> step(graph::NodeInput& in, std::uint64_t ordinal, ProviderRequest request) {
        const auto broker = graph::provider_call_broker(in.ctx);
        if (!broker) throw std::logic_error("the Program provider broker is absent");
        auto result = co_await graph::observe_provider_result(
            in.ctx, broker->invoke(graph::make_provider_call_identity(in.ctx, name_, ordinal), provider_,
                                   std::move(request)));
        if (!result) throw std::runtime_error("the upload returned no outcome");
        auto outcome = sp::files::typed_progress(*result);
        if (const auto* failure = std::get_if<sp::Failure>(&outcome))
            throw std::runtime_error(failure->error.safe_message);
        co_return std::get<sp::files::Progress>(std::move(outcome));
    }

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
        const int run = ++script_->check_runs;
        if (in.ctx.thread_id == script_->ask_thread ||
            (script_->ask_on_check_run.load() != 0 && run == script_->ask_on_check_run.load())) {
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
    PlanRoute,
    // #390: the model and a check in one fan-out step.
    LlmBesideCheck,
    // #387: the model, then a check.
    LlmThenCheck,
    // #420: three model calls in a row.
    ThreeLlmsInARow,
    // #531: an endpoint node and a check in one fan-out step.
    EmbedBesideCheck,
    // #531: an endpoint node alone.
    Embed,
    // #531: a video job submitted, then collected.
    VideoJob,
    // #541: a file uploaded by a host node.
    FileUpload,
    // #541: a file uploaded in parts by a host node.
    ChunkedUpload
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
    // #531: the durable journal's bind_priced, or bind, which journals calls without pricing them.
    bool          priced        = true;
    int           node_retries  = 0;
    // #543: edits the descriptor policy the provider's client is admitted under, which changes its identity.
    std::function<void(json&)> edit_policy;
    // #543: RuntimeConfig::fork_may_resend_provider_calls.
    bool          fork_may_resend = false;
    // Replaces the provider double: a real SDK client at a loopback peer.
    std::function<std::shared_ptr<Provider>()> provider;
    // The trusted native custody a durable journal needs for a real SDK client.
    std::shared_ptr<sp::NativeArchive> archive;
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
        builder.add_core_endpoint_node(
            manifest({ExecutableKind::Node, "embed", "1.0.0", digest('a')}, {kProviderIdentity}), {});
        builder.add_core_endpoint_node(
            manifest({ExecutableKind::Node, "job_submit", "1.0.0", digest('b')}, {kProviderIdentity}), {});
        builder.add_core_endpoint_node(
            manifest({ExecutableKind::Node, "job_collect", "1.0.0", digest('c')}, {kProviderIdentity}), {});
        builder.add_host_brokered_node(
            manifest({ExecutableKind::Node, "file_upload", "1.0.0", digest('d')}, {kProviderIdentity}),
            [script = script_](const std::string& name, const json&, const graph::NodeContext& context) {
                return std::make_unique<UploadCall>(name, context.provider, script);
            },
            json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
        builder.add_host_brokered_node(
            manifest({ExecutableKind::Node, "chunked_upload", "1.0.0", digest('e')}, {kProviderIdentity}),
            [script = script_](const std::string& name, const json&, const graph::NodeContext& context) {
                return std::make_unique<ChunkedUploadCall>(name, context.provider, script);
            },
            json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
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
            journal_     = std::make_shared<SQLiteProgramProviderCallJournal>(files_->provider_db(),
                                                                              options_.archive);
        } else {
            store        = std::make_shared<InMemoryProgramStore>();
            transitions_ = std::make_shared<InMemoryProgramTransitionStore>();
            checkpoints  = std::make_shared<graph::InMemoryCheckpointStore>();
        }

        std::optional<graph::NodeContext> context;
        if (options_.bind_provider) {
            context.emplace();
            context->provider = options_.provider
                ? options_.provider()
                : make_provider(peer_, client_for(options_.origin, options_.model, options_.input_limit,
                                                  options_.edit_policy));
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
        runtime.fork_may_resend_provider_calls    = options_.fork_may_resend;
        if (journal_) {
            runtime.core_provider_calls_journaled = true;
            runtime.core_provider_call_resolver =
                [journal = journal_, deployment = program::detail::provider_deployment_identity(options_.label),
                 priced = options_.priced](
                    const ProgramCoreProviderCallContext& context)
                -> std::optional<ProgramCoreProviderCallBinding> {
                return priced ? journal->bind_priced(context, deployment) : journal->bind(context, deployment);
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
            case Shape::LlmThenCheck:
                definition["nodes"] = {{"llm", {{"type", "llm_call"}}}, {"check", {{"type", "check"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "llm"), edge("llm", "check"), edge("check", "__end__")});
                break;
            case Shape::LlmBesideCheck:
                definition["nodes"] = {{"llm", {{"type", "llm_call"}}}, {"check", {{"type", "check"}}}};
                definition["edges"] = json::array({edge("__start__", "llm"), edge("__start__", "check"),
                                                   edge("llm", "__end__"), edge("check", "__end__")});
                break;
            case Shape::ThreeLlmsInARow:
                definition["nodes"] = {{"first", {{"type", "llm_call"}}},
                                       {"second", {{"type", "llm_call"}}},
                                       {"third", {{"type", "llm_call"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "first"), edge("first", "second"), edge("second", "third"),
                     edge("third", "__end__")});
                break;
            case Shape::SendsToBrokered:
                definition["nodes"] = {{"fan", {{"type", "send_fan"}}}, {"work", {{"type", "brokered_call"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "fan"), edge("fan", "__end__"), edge("work", "__end__")});
                break;
            case Shape::EmbedBesideCheck:
            case Shape::Embed: {
                definition["channels"]["text"] = {{"reducer", "overwrite"}, {"initial", "a cup of tea"}};
                definition["channels"]["embeddings"] = {{"reducer", "overwrite"}, {"initial", json::array()}};
                const json embed = {{"type", "embed"}, {"model", "fixture-embed"}, {"input", "text"},
                                    {"output", "embeddings"}};
                if (shape == Shape::Embed) {
                    definition["nodes"] = {{"embed", embed}};
                    definition["edges"] = json::array({edge("__start__", "embed"), edge("embed", "__end__")});
                } else {
                    definition["nodes"] = {{"embed", embed}, {"check", {{"type", "check"}}}};
                    definition["edges"] = json::array({edge("__start__", "embed"), edge("__start__", "check"),
                                                       edge("embed", "__end__"), edge("check", "__end__")});
                }
                break;
            }
            case Shape::FileUpload:
                definition["nodes"] = {{"upload", {{"type", "file_upload"}}}};
                definition["edges"] = json::array({edge("__start__", "upload"), edge("upload", "__end__")});
                break;
            case Shape::ChunkedUpload:
                definition["nodes"] = {{"upload", {{"type", "chunked_upload"}}}};
                definition["edges"] = json::array({edge("__start__", "upload"), edge("upload", "__end__")});
                break;
            case Shape::VideoJob:
                definition["channels"]["prompt"] = {{"reducer", "overwrite"}, {"initial", "waves at dusk"}};
                definition["channels"]["job"] = {{"reducer", "overwrite"}, {"initial", json::object()}};
                definition["channels"]["video"] = {{"reducer", "overwrite"}, {"initial", json::array()}};
                definition["nodes"] = {{"submit", {{"type", "job_submit"}, {"model", "fixture-video"}, {"duration", 4}}},
                                       {"collect", {{"type", "job_collect"}}}};
                definition["edges"] = json::array(
                    {edge("__start__", "submit"), edge("submit", "collect"), edge("collect", "__end__")});
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

// The model's price entry, as the catalogue of a priced fixture policy holds it.
sp::pricing::ModelPrice fixture_price(std::string_view model, std::uint64_t input) {
    const auto catalogue = neograph::test::fixture_catalogue({priced_model("openai.chat", std::string(model), input)});
    return sp::pricing::dearest_price_of(*catalogue->entry(model), {}, input);
}

// The hold at OpenRouter's route ceiling, rounded up once to whole microunits.
std::uint64_t expected_hold(std::string_view model = kModel, std::uint64_t input = kInputLimit,
                            std::uint64_t output = kOutputCap) {
    const auto bound = sp::pricing::upper_bound(fixture_price(model, input), {input, output, true, {}});
    return (bound.total->nano_usd + 999) / 1000;
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
    EXPECT_EQ(record.origin, kOpenRouter);
    EXPECT_EQ(record.model, kModel);
    EXPECT_EQ(record.node, "llm");
    EXPECT_FALSE(record.call_id.empty());
    EXPECT_FALSE(record.replayed);
    EXPECT_EQ(record.catalog_version, "sp-model-catalogue/1") << "the record names the catalogue its hold used";
    EXPECT_EQ(record.pricing.at("entry"), std::string(kModel));
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
    const auto& record = result.monetary_records().front();
    // At the route ceiling: 21 input tokens at 440 and 3 output tokens at 1320
    // nano-USD are 13,200 nano-USD, rounded up to 14 microunits.
    EXPECT_EQ(record.evidence, ProgramMonetaryEvidence::Estimated);
    EXPECT_EQ(record.microunits, 14U);
    EXPECT_LE(record.microunits, record.reserved_microunits);
    EXPECT_EQ(record.basis, "route_ceiling");
    EXPECT_TRUE(record.conservative) << "an unknown endpoint is priced at the ceiling";
    EXPECT_EQ(record.catalog_version, "sp-model-catalogue/1");
    const auto catalogue = neograph::test::fixture_catalogue({priced_model("openai.chat", std::string(kModel), kInputLimit)});
    EXPECT_EQ(record.catalog_digest, catalogue->digest_hex());
    EXPECT_EQ(record.pricing.at("entry"), std::string(kModel));
    EXPECT_EQ(record.pricing.at("units").at("ceiling_input"), "0.00000044");
    EXPECT_EQ(record.pricing.at("assumptions"), json::array({"route ceiling"}));
    EXPECT_EQ(result.usage().monetary_microunits, 14U);
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
// HTTP answers from a real SDK client at a loopback peer
// ---------------------------------------------------------------------------

namespace wire = neograph::test::wire;

// The loopback origin is declared an OpenRouter origin in the SDK policy, as
// the Python host does for a local peer: the SDK then admits OpenRouter
// routing for it and the catalog prices it as OpenRouter.
sp::descriptor::ValidatedDescriptor loopback_openrouter_descriptor(const std::string& origin) {
    auto policy = json::parse(sp::config_defaults::descriptor_policy_json);
    for (auto family : policy.at("families"))
        if (family.at("family") == "openai.chat") family.at("openrouter_origins").push_back(origin);
    return neograph::test::descriptor("openai.chat", origin, neograph::test::policy_with_models(
        policy, {priced_model("openai.chat", std::string(kModel), kInputLimit, {origin})}));
}

// A real SDK client at the loopback peer. A durable journal needs the trusted
// native custody such a client asks for: an archive provisioned for the same
// descriptor, in an owner-only directory next to the journal.
HostOptions loopback_options(const std::string& origin, const TempDir* durable_files) {
    auto descriptor = loopback_openrouter_descriptor(origin);
    HostOptions options;
    if (durable_files) {
        const auto custody = durable_files->path / "custody";
        std::filesystem::create_directory(custody);
        std::filesystem::permissions(custody, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        auto provisioned = sp::NativeArchive::provision((custody / "records").string(),
                                                        (custody / "key").string(), kOwner, descriptor);
        if (!std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(provisioned))
            throw std::runtime_error("native archive provisioning failed");
        options.archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(provisioned));
    }
    options.provider = [descriptor] {
        sp::runtime::Options runtime;
        runtime.workers                 = 1;
        runtime.default_timeout         = std::chrono::seconds(5);
        runtime.retry_tokens            = 0;
        runtime.retry_tokens_per_second = 0;
        return std::shared_ptr<Provider>(llm::SchemaProvider::create(descriptor, std::move(runtime)));
    };
    return options;
}

std::string error_body(int status) {
    return json{{"error", {{"message", "marker-not-for-logs"}, {"type", "fixture_error"},
                           {"code", status}}}}.dump();
}

std::size_t requests_seen(wire::Peer& peer) {
    std::lock_guard lock(peer.state->mutex);
    return peer.state->entered;
}

// The reservation one call makes against the model-token budget: the admitted
// input limit plus the output cap.
constexpr std::uint64_t kTokenHold = kInputLimit + kOutputCap;

TEST_P(Priced, ACompletedHttp4xxAnswerReleasesTheHoldAndChargesNothing) {
#ifdef _WIN32
    if (durable()) GTEST_SKIP() << "NativeArchive custody needs an owner-only protected directory";
#endif
    for (const int status : {400, 401, 404}) {
        SCOPED_TRACE(status);
        TempDir      files("rejection");
        wire::Peer   remote(error_body(status));
        remote.state->status = status;
        auto options = loopback_options(remote.origin(), durable() ? &files : nullptr);
        Host       h(std::make_shared<Peer>(), options, durable() ? &files : nullptr);
        const auto result = h.run(Shape::Llm);
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed) << failure_text(result);
        ASSERT_TRUE(result.failure());
        // The durable run keeps the outcome in the journal row, not in the failure.
        if (!durable()) {
            ASSERT_TRUE(result.failure()->provider_outcome);
            const auto& failed = std::get<sp::Failure>(*result.failure()->provider_outcome);
            EXPECT_EQ(failed.error.http_status, status);
            EXPECT_EQ(failed.error.retry_safety, sp::RetrySafety::RejectedBeforeOutput);
        }
        // The provider declined the request: the money hold and the token
        // hold are released, and nothing is charged.
        EXPECT_TRUE(result.monetary_records().empty());
        EXPECT_EQ(result.usage().monetary_microunits, 0U);
        EXPECT_EQ(result.usage().model_tokens, 0U);
        EXPECT_EQ(result.remaining_budget().monetary_microunits, 1'000'000U);
        EXPECT_EQ(result.remaining_budget().model_tokens, options.model_tokens);
        EXPECT_EQ(requests_seen(remote), 1U);
        if (durable()) EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"failed"});
    }
}

TEST_P(Priced, AnAnswerThatCanFollowStartedWorkKeepsTheHold) {
#ifdef _WIN32
    if (durable()) GTEST_SKIP() << "NativeArchive custody needs an owner-only protected directory";
#endif
    for (const int status : {429, 503}) {
        SCOPED_TRACE(status);
        TempDir      files("started");
        wire::Peer   remote(error_body(status));
        remote.state->status = status;
        auto options = loopback_options(remote.origin(), durable() ? &files : nullptr);
        Host       h(std::make_shared<Peer>(), options, durable() ? &files : nullptr);
        const auto result = h.run(Shape::Llm);
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed) << failure_text(result);
        ASSERT_TRUE(result.failure());
        if (!durable()) {
            ASSERT_TRUE(result.failure()->provider_outcome);
            const auto& failed = std::get<sp::Failure>(*result.failure()->provider_outcome);
            EXPECT_EQ(failed.error.http_status, status);
            EXPECT_EQ(failed.error.retry_safety, sp::RetrySafety::PossiblyAccepted);
        }
        ASSERT_EQ(result.monetary_records().size(), 1U);
        EXPECT_EQ(result.monetary_records().front().evidence, ProgramMonetaryEvidence::Held);
        EXPECT_EQ(result.usage().monetary_microunits, expected_hold());
        EXPECT_EQ(result.usage().model_tokens, kTokenHold);
        EXPECT_EQ(requests_seen(remote), 1U);
        if (durable())
            EXPECT_EQ(journal_states(files.provider_db()),
                      std::vector<std::string>{"reconciliation_required"});
    }
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
        {"a price scoped to other origins", [](HostOptions& o) { o.origin = "https://fixture.invalid"; },
         "P_MONEY_UNKNOWN_MODEL", ProgramTerminalStatus::Failed},
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
    // A catalogue entry with prices and a route ceiling but no limits.
    auto peer     = std::make_shared<Peer>();
    auto provider = make_provider(peer, client_for(std::string(kOpenRouter), "vendor/limitless", std::nullopt));
    auto request  = make_provider_request(*provider, "vendor/limitless",
                                          {neograph::test::message("q", sp::Role::User)});
    std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
    const auto prepared = provider->prepare(std::move(request));
    ASSERT_TRUE(prepared.valid());
    const auto quote = program::detail::assess_monetary_call({}, prepared);
    EXPECT_EQ(quote.blocker, "P_MONEY_NO_INPUT_BOUND");
    EXPECT_EQ(quote.hold, 0U);
}

TEST(ProgramProviderBinding, TheRouteCeilingIsTheReservationAndThePinnedPrice) {
    auto peer     = std::make_shared<Peer>();
    auto provider = make_provider(peer, client_for(std::string(kOpenRouter), std::string(kModel), kInputLimit));
    auto request  = make_provider_request(*provider, std::string(kModel),
                                          {neograph::test::message("probe", sp::Role::User)});
    std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
    const auto quote = program::detail::assess_monetary_call({}, provider->prepare(request));
    EXPECT_TRUE(quote.blocker.empty()) << quote.blocker_message;
    // The catalogue's 0.44 and 1.32 USD per million tokens, in nanoUSD.
    ASSERT_TRUE(quote.pin);
    EXPECT_EQ(quote.pin->prompt_nano_usd, 440'000'000U);
    EXPECT_EQ(quote.pin->completion_nano_usd, 1'320'000'000U);
    EXPECT_FALSE(quote.pin->image_nano_usd || quote.pin->audio_nano_usd || quote.pin->request_nano_usd);
    // 100,000 input tokens at 440 and 64 output tokens at 1,320 nano-USD.
    EXPECT_EQ(quote.hold, (100'000U * 440 + 64 * 1'320 + 999) / 1000);
    EXPECT_EQ(quote.hold, expected_hold());
    EXPECT_EQ(quote.hold_basis, "route_ceiling");
    EXPECT_EQ(quote.origin, kOpenRouter);
    EXPECT_EQ(quote.catalog_version, "sp-model-catalogue/1");
    EXPECT_EQ(quote.pricing.at("entry"), std::string(kModel));
    EXPECT_TRUE(program::detail::pin_route_max_price(request, *quote.pin));
    EXPECT_FALSE(program::detail::pin_route_max_price(request, *quote.pin))
        << "a request that already carries the pin is unchanged";
    const auto pinned = provider->prepare(std::move(request));
    ASSERT_TRUE(pinned.valid());
    EXPECT_EQ(std::string(pinned.encoded_body()),
              R"({"model":"deepseek/deepseek-v4-flash-0731",)"
              R"("messages":[{"role":"user","content":[{"type":"text","text":"probe"}]}],)"
              R"("stream":false,"max_tokens":64,)"
              R"("provider":{"max_price":{"prompt":0.44,"completion":1.32}}})");
}

namespace {

constexpr std::string_view kPinnedCeiling =
    R"("provider":{"max_price":{"prompt":0.44,"completion":1.32}})";

sp::OpenRouterMaxPrice max_price(std::optional<std::uint64_t> prompt,
                                 std::optional<std::uint64_t> completion,
                                 std::optional<std::uint64_t> image   = std::nullopt,
                                 std::optional<std::uint64_t> request = std::nullopt) {
    sp::OpenRouterMaxPrice price;
    price.prompt_nano_usd     = prompt;
    price.completion_nano_usd = completion;
    price.image_nano_usd      = image;
    price.request_nano_usd    = request;
    return price;
}

// A SchemaProvider at OpenRouter for `family`; nothing is dispatched.
std::shared_ptr<Provider> openrouter_schema_provider(const std::string&                      family,
                                                     std::optional<sp::OpenRouterRouting> defaults = {}) {
    llm::SchemaProvider::Defaults provider_defaults;
    provider_defaults.provider = std::move(defaults);
    return llm::SchemaProvider::create(
        neograph::test::descriptor(family, std::string(kOpenRouter),
                                   neograph::test::policy_with_models(
                                       json::parse(sp::config_defaults::descriptor_policy_json),
                                       {priced_model(family, std::string(kModel), kInputLimit)})),
        {}, std::move(provider_defaults));
}

ProviderRequest capped_request(const Provider& provider, ProviderControls controls = {}) {
    controls.max_output_tokens = kOutputCap;
    return make_provider_request(provider, std::string(kModel),
                                 {neograph::test::message("probe", sp::Role::User)}, {},
                                 std::move(controls));
}

// The body a pricing broker sends for `request`.
std::string pinned_body(Provider& provider, ProviderRequest request) {
    const auto priced = program::detail::prepare_priced_call(provider, std::move(request), true);
    if (!priced.quote || !priced.quote->pin) throw std::logic_error("no pin quoted");
    if (!priced.prepared.valid()) throw std::logic_error("the pinned request is invalid");
    return std::string(priced.prepared.encoded_body());
}

}  // namespace

TEST(ProgramProviderBinding, ThePinReachesEveryFamilyOpenRouterServes) {
    for (const auto* family : {"openai.chat", "openai.responses", "anthropic.messages"}) {
        SCOPED_TRACE(family);
        auto provider = openrouter_schema_provider(family);
        EXPECT_NE(pinned_body(*provider, capped_request(*provider)).find(kPinnedCeiling), std::string::npos);
    }
    // Messages takes no routing defaults, so a pin always rides on the request.
    auto messages = capped_request(*openrouter_schema_provider("anthropic.messages"));
    EXPECT_TRUE(program::detail::pin_route_max_price(messages, max_price(1, 2)));
    const auto& routing = std::get<sp::messages::Request>(messages.payload).provider;
    ASSERT_TRUE(routing && routing->max_price);
    EXPECT_EQ(routing->max_price->completion_nano_usd, 2U);
    // A family OpenRouter does not serve cannot carry the pin.
    ProviderRequest gemini;
    gemini.payload = sp::gemini::Request{};
    EXPECT_THROW((void)program::detail::pin_route_max_price(gemini, max_price(1, 2)), std::logic_error);
}

TEST(ProgramProviderBinding, AHostMaxPriceKeepsTheLowerAmountPerField) {
    // A host cap in the provider defaults: 0.30 for prompts is below the 0.44
    // ceiling and is kept, completion takes the ceiling, and the per-request
    // fee the catalog does not cap stays the host's. The other defaults stay.
    sp::OpenRouterRouting defaults;
    defaults.zdr       = true;
    defaults.max_price = max_price(300'000'000, std::nullopt, std::nullopt, 100'000);
    for (const auto* family : {"openai.chat", "openai.responses"}) {
        SCOPED_TRACE(family);
        auto provider = openrouter_schema_provider(family, defaults);
        EXPECT_NE(pinned_body(*provider, capped_request(*provider))
                      .find(R"("provider":{"zdr":true,"max_price":{"prompt":0.3,"completion":1.32,"request":0.0001}})"),
                  std::string::npos);

        // A cap in the request controls replaces the defaults' cap, as every
        // routing field does, and is lowered to the ceiling where it is dearer.
        ProviderControls controls;
        controls.provider.emplace();
        controls.provider->max_price = max_price(500'000'000, 1'000'000'000, 2'000'000);
        EXPECT_NE(pinned_body(*provider, capped_request(*provider, controls))
                      .find(R"("provider":{"zdr":true,"max_price":{"prompt":0.44,"completion":1,"image":0.002}})"),
                  std::string::npos);
    }
    // Messages has no routing defaults: the host's cap is its request controls'.
    auto             messages = openrouter_schema_provider("anthropic.messages");
    ProviderControls controls;
    controls.provider.emplace();
    controls.provider->max_price = max_price(2'000'000'000, 600'000'000);
    EXPECT_NE(pinned_body(*messages, capped_request(*messages, controls))
                  .find(R"("provider":{"max_price":{"prompt":0.44,"completion":0.6}})"),
              std::string::npos);
    // A host cap at or below the ceiling everywhere is the pin already.
    controls.provider->max_price = max_price(100'000'000, 1'320'000'000);
    auto       own   = capped_request(*messages, controls);
    const auto quote = program::detail::assess_monetary_call({}, messages->prepare(own));
    ASSERT_TRUE(quote.pin);
    EXPECT_FALSE(program::detail::pin_route_max_price(own, *quote.pin));
    EXPECT_NE(pinned_body(*messages, std::move(own))
                  .find(R"("provider":{"max_price":{"prompt":0.1,"completion":1.32}})"),
              std::string::npos);
}

TEST(ProgramProviderBinding, OnlyAnOpenRouterOriginIsPinned) {
    // A direct origin routes nowhere else: it is priced but gets no max_price.
    auto peer   = std::make_shared<Peer>();
    auto direct = make_provider(peer, client_for("https://api.openai.com", "gpt-4.1-nano", kInputLimit));
    auto request = make_provider_request(*direct, "gpt-4.1-nano",
                                         {neograph::test::message("probe", sp::Role::User)});
    std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
    const auto prepared = direct->prepare(std::move(request));
    const auto quote = program::detail::assess_monetary_call({}, prepared);
    EXPECT_TRUE(quote.blocker.empty()) << quote.blocker_message;
    EXPECT_EQ(quote.origin, "https://api.openai.com");
    EXPECT_EQ(quote.hold_basis, "list_rates");
    EXPECT_FALSE(quote.pin);
    EXPECT_EQ(prepared.encoded_body().find("max_price"), std::string_view::npos);

    // An OpenRouter URL whose SDK policy admits no OpenRouter routing cannot
    // be pinned, so its route ceiling bounds nothing and the call is refused.
    auto source = json::parse(sp::config_defaults::descriptor_policy_json);
    // Gateway facts (media, plugins, server tools) need a declared OpenRouter origin.
    auto families = json::array();
    for (const auto& family : source.at("families")) {
        auto unrouted = json::object();
        for (const auto& [key, value] : family.items())
            if (key != "openrouter_plugins" && key != "openrouter_server_tools") unrouted[key] = value;
        unrouted["openrouter_origins"] = json::array();
        unrouted["openrouter_media"] = nullptr;
        families.push_back(std::move(unrouted));
    }
    source["families"] = std::move(families);
    auto admitted = sp::descriptor::load_policy(
        source.dump(), sp::config_defaults::codec_defaults_json,
        neograph::test::fixture_catalogue({priced_model("openai.chat", std::string(kModel), kInputLimit)}));
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        FAIL() << error->pointer << ": " << error->message;
    auto unrouted = make_provider(peer, std::make_shared<sp::runtime::Client>(neograph::test::descriptor(
        "openai.chat", std::string(kOpenRouter),
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted)))));
    auto routed_request = make_provider_request(*unrouted, std::string(kModel),
                                                {neograph::test::message("probe", sp::Role::User)});
    std::get<sp::chat::Request>(routed_request.payload).max_output_tokens = kOutputCap;
    const auto refused = program::detail::assess_monetary_call({}, unrouted->prepare(std::move(routed_request)));
    EXPECT_EQ(refused.blocker, "P_MONEY_NO_UPPER_BOUND");
    EXPECT_FALSE(refused.pin);
}

// Refuses a one-message conversation, as a provider that checks history might.
class ProbeRefusingProvider final : public Provider {
public:
    explicit ProbeRefusingProvider(std::shared_ptr<Provider> inner) : inner_(std::move(inner)) {}
    std::string      get_name() const override { return inner_->get_name(); }
    std::string_view family() const noexcept override { return inner_->family(); }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        ++prepared;
        if (provider_request_messages(request).size() < 2) {
            sp::Error error;
            error.kind         = sp::ErrorKind::InvalidRequest;
            error.safe_message = "a conversation needs history";
            return reject_preparation(std::move(error));
        }
        return inner_->prepare(std::move(request));
    }
    int prepared = 0;

private:
    std::shared_ptr<Provider> inner_;
};

TEST(ProgramProviderBinding, APricedCallIsPinnedBeforeItIsPreparedToBeSent) {
    auto peer     = std::make_shared<Peer>();
    auto provider = make_provider(peer, client_for(std::string(kOpenRouter), std::string(kModel), kInputLimit));
    const auto conversation = [&] {
        auto request = make_provider_request(
            *provider, std::string(kModel),
            {neograph::test::message("q", sp::Role::User), neograph::test::message("a"),
             neograph::test::message("q again", sp::Role::User)});
        std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
        return request;
    };
    // Priced from a one-message probe, then pinned and prepared once.
    auto priced = program::detail::prepare_priced_call(*provider, conversation(), true);
    ASSERT_TRUE(priced.prepared.valid());
    ASSERT_TRUE(priced.quote && priced.quote->pin);
    EXPECT_TRUE(priced.quote->blocker.empty()) << priced.quote->blocker_message;
    EXPECT_EQ(priced.quote->hold, expected_hold());
    const std::string body(priced.prepared.encoded_body());
    EXPECT_NE(body.find(kPinnedCeiling), std::string::npos) << body;
    EXPECT_NE(body.find("q again"), std::string::npos) << "the sent request keeps its conversation";

    // A provider that refuses the probe is priced from the request's own
    // preparation, which is then pinned and prepared again.
    ProbeRefusingProvider picky(provider);
    auto fallback = program::detail::prepare_priced_call(picky, conversation(), true);
    ASSERT_TRUE(fallback.prepared.valid());
    ASSERT_TRUE(fallback.quote && fallback.quote->pin);
    EXPECT_EQ(fallback.quote->hold, priced.quote->hold);
    EXPECT_EQ(std::string(fallback.prepared.encoded_body()), body);
    EXPECT_EQ(Provider::request_digest(fallback.prepared), Provider::request_digest(priced.prepared));
    EXPECT_EQ(picky.prepared, 3);

    // A request the provider refuses has no quote, and an unpriced one no pin.
    auto refused = program::detail::prepare_priced_call(
        picky,
        make_provider_request(*provider, std::string(kModel), {neograph::test::message("q", sp::Role::User)}),
        true);
    EXPECT_FALSE(refused.prepared.valid());
    EXPECT_FALSE(refused.quote);
    auto unpriced = program::detail::prepare_priced_call(*provider, conversation(), false);
    ASSERT_TRUE(unpriced.prepared.valid());
    EXPECT_FALSE(unpriced.quote);
    EXPECT_EQ(unpriced.prepared.encoded_body().find("max_price"), std::string_view::npos);
}

TEST_P(Priced, TheSentRequestCarriesThePinnedCeilingAndTheJournalItsDigest) {
    auto       peer   = std::make_shared<Peer>();
    auto       h      = host(peer);
    const auto result = h->run(Shape::Llm);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    ASSERT_EQ(peer->bodies.size(), 1U);
    EXPECT_NE(peer->bodies[0].find(kPinnedCeiling), std::string::npos) << peer->bodies[0];
    if (durable()) {
        ASSERT_EQ(result.monetary_records().size(), 1U);
        const auto row = h->journal().inspect(kOwner, result.monetary_records().front().call_id);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->content_digest, peer->contents[0])
            << "the journal records the pinned request that was sent, so a replay matches it";
        EXPECT_EQ(row->policy_identity, peer->policies[0]);
    }

    // A direct origin is priced without max_price.
    auto        direct = std::make_shared<Peer>();
    HostOptions options;
    options.origin = "https://api.openai.com";
    options.model  = "gpt-4.1-nano";
    TempDir    files("direct-origin");
    Host       other(direct, options, durable() ? &files : nullptr);
    const auto priced = other.run(Shape::Llm);
    ASSERT_EQ(priced.status(), ProgramTerminalStatus::Completed) << failure_text(priced);
    ASSERT_EQ(priced.monetary_records().size(), 1U);
    EXPECT_EQ(priced.monetary_records().front().origin, "https://api.openai.com");
    ASSERT_EQ(direct->bodies.size(), 1U);
    EXPECT_EQ(direct->bodies[0].find("max_price"), std::string::npos) << direct->bodies[0];
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

// ---------------------------------------------------------------------------
// #420: the model-token budget ends a run in one shape
// ---------------------------------------------------------------------------

namespace {

// Either the last reply takes the run over its model-token budget, or the next
// call cannot be reserved and is refused before it is sent. A call reserves the
// model's input limit and the output cap: 300 tokens here, of a budget of 1,000.
struct TokenBudgetCase {
    const char*   name;
    std::uint64_t reply_tokens;
    int           sent;
    std::uint64_t used;
};

HostOptions token_budget_options() {
    HostOptions options;
    options.model_tokens = 1'000;
    options.input_limit  = 100;
    options.output_cap   = 200;
    return options;
}

}  // namespace

TEST(ProgramProviderBinding, ExhaustingTheModelTokenBudgetIsOneTerminalShape) {
    for (const auto& c : {TokenBudgetCase{"the last reply crosses the budget", 600, 2, 1'200},
                          TokenBudgetCase{"the next call is refused before sending", 400, 2, 800}}) {
        SCOPED_TRACE(c.name);
        auto peer   = std::make_shared<Peer>();
        peer->reply = [tokens = c.reply_tokens](int, const ProviderRequest&) {
            return neograph::test::success("answer", with_cost(final_usage(tokens - 100, 100), 1'000'000));
        };
        Host       h(peer, token_budget_options());
        const auto result = h.run(Shape::ThreeLlmsInARow);
        EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
        ASSERT_TRUE(result.failure());
        EXPECT_EQ(result.failure()->code,
                  neograph::graph::node_failure_code(neograph::graph::NodeFailureClass::BudgetExhausted));
        EXPECT_EQ(result.failure()->code, "NG.BudgetExhausted");
        EXPECT_EQ(peer->transport.load(), c.sent);
        EXPECT_EQ(result.usage().model_tokens, c.used);
    }
}

TEST(ProgramProviderBinding, TheOvershootRecordSaysHowFarTheLastReplyWentOver) {
    auto peer   = std::make_shared<Peer>();
    peer->reply = [](int, const ProviderRequest&) {
        return neograph::test::success("answer", with_cost(final_usage(500, 100), 1'000'000));
    };
    Host       h(peer, token_budget_options());
    const auto result = h.run(Shape::ThreeLlmsInARow);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->witness["model_tokens"],
              (json{{"granted", 1'000}, {"used", 1'200}, {"over_by", 200}}));
    EXPECT_NE(result.failure()->message.find("1200"), std::string::npos) << failure_text(result);
    EXPECT_NE(result.failure()->message.find("1000"), std::string::npos) << failure_text(result);
    EXPECT_EQ(peer->transport.load(), 2) << "the run stops at the step boundary after the overshoot";
}

// ---------------------------------------------------------------------------
// #443: a run that goes over several budgets names every one of them
//
// The witness has an entry for each budget the run is over; the status and the
// code are those of the first of them in the order of the RunBudget fields:
// wall time, model tokens, money, Core steps.
// ---------------------------------------------------------------------------

TEST(ProgramProviderBinding, ARunThatCompletesOverItsTokensAndItsMoneyNamesBoth) {
    // One call reserves 300 tokens and `hold` microunits, and is sent. Its
    // reply is 1,200 tokens and costs 600 microunits more than the hold.
    const auto hold = expected_hold(kModel, 100, 200);
    auto       peer = std::make_shared<Peer>();
    peer->reply     = [hold](int, const ProviderRequest&) {
        return neograph::test::success("answer",
                                       with_cost(final_usage(1'100, 100), (hold + 600) * 1'000));
    };
    auto options     = token_budget_options();
    options.monetary = hold + 500;
    Host       h(peer, options);
    const auto result = h.run(Shape::Llm);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "NG.BudgetExhausted") << "tokens come before money";
    EXPECT_EQ(result.failure()->witness["model_tokens"],
              (json{{"granted", 1'000}, {"used", 1'200}, {"over_by", 200}}));
    EXPECT_EQ(result.failure()->witness["monetary"],
              (json{{"granted", hold + 500}, {"used", hold + 600}, {"over_by", 100}}));
    EXPECT_EQ(peer->transport.load(), 1);
}

TEST(ProgramProviderBinding, ARunStoppedOverItsTokensAndItsMoneyNamesBoth) {
    // Two replies of 600 tokens and hold + 500 microunits each, with a budget of
    // 1,000 tokens and 2 * hold + 500 microunits: the second call fits both
    // (its reservation is made against what the first call was charged), and its
    // reply takes the run over both, so the third call is never sent.
    const auto hold = expected_hold(kModel, 100, 200);
    auto       peer = std::make_shared<Peer>();
    peer->reply     = [hold](int, const ProviderRequest&) {
        return neograph::test::success("answer",
                                       with_cost(final_usage(500, 100), (hold + 500) * 1'000));
    };
    auto options     = token_budget_options();
    options.monetary = 2 * hold + 500;
    Host       h(peer, options);
    const auto result = h.run(Shape::ThreeLlmsInARow);
    ASSERT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "NG.BudgetExhausted");
    EXPECT_EQ(result.failure()->witness["model_tokens"],
              (json{{"granted", 1'000}, {"used", 1'200}, {"over_by", 200}}));
    EXPECT_EQ(result.failure()->witness["monetary"],
              (json{{"granted", 2 * hold + 500}, {"used", 2 * hold + 1'000}, {"over_by", 500}}));
    EXPECT_EQ(peer->transport.load(), 2);
}

TEST_P(Priced, AReportedCostAboveTheCeilingExhaustsTheRun) {
    auto peer   = std::make_shared<Peer>();
    const auto ceiling = expected_hold();
    peer->reply = [ceiling](int, const ProviderRequest&) {
        // A reported cost above the hold. max_price caps the endpoint's listed
        // token prices; a BYOK upstream charge or a fee it does not cap can
        // still make a reported cost exceed the reservation.
        return neograph::test::success("answer", with_cost(final_usage(21, 3), (ceiling + 10) * 1000));
    };
    HostOptions options;
    options.monetary  = ceiling;
    auto       h      = host(peer, options);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::BudgetExhausted) << failure_text(result);
    EXPECT_EQ(result.usage().monetary_microunits, ceiling + 10);
    EXPECT_EQ(result.remaining_budget().monetary_microunits, 0U);
    // #443: the run ends with the record of the budget it is over.
    ASSERT_TRUE(result.failure()) << "a money overshoot carries its failure record";
    EXPECT_EQ(result.failure()->code, "P_MONEY_BUDGET");
    EXPECT_EQ(result.failure()->witness["monetary"],
              (json{{"granted", ceiling}, {"used", ceiling + 10}, {"over_by", 10}}));
    EXPECT_FALSE(result.failure()->witness.contains("model_tokens"));
    EXPECT_EQ(result.failure()->operation_id, "root");
    EXPECT_NE(result.failure()->message.find(std::to_string(ceiling + 10)), std::string::npos)
        << failure_text(result);
}

TEST_P(Priced, ACostWithinTheCeilingLeavesNoFailureRecord) {
    auto peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary  = expected_hold();
    auto       h      = host(peer, options);
    const auto result = h->run(Shape::Llm);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    EXPECT_FALSE(result.failure());
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
    // journaled calls replay instead of being sent and charged again. #388:
    // the first resume answers the first Send only, and the second asks its
    // own question again; the next resume answers it.
    TempDir     files("send-replay");
    auto        peer = std::make_shared<Peer>();
    HostOptions options;
    options.monetary = 100'000;
    options.workers  = 2;
    Host h(peer, options, &files);
    h.script().interrupt_after_call = true;
    const auto first = h.run(Shape::SendsToBrokered, "send-run");
    ASSERT_EQ(first.status(), ProgramTerminalStatus::Interrupted) << failure_text(first);
    ASSERT_TRUE(first.interrupt().has_value() && first.interrupt()->pending_input);
    EXPECT_EQ(first.interrupt()->core_node, "work");
    const auto first_send = first.interrupt()->pending_input->core_interrupt_id();
    EXPECT_NE(first_send.find(":send[0]:work:"), std::string::npos) << first_send;
    EXPECT_EQ(peer->transport.load(), 2);
    EXPECT_EQ(first.monetary_records().size(), 2U);
    EXPECT_EQ(first.usage().monetary_microunits, 2U * 1235U);

    const auto second =
        h.runtime().resume(kOwner, "send-run", resume_after(first, "trace-send-resume")).wait();
    ASSERT_EQ(second.status(), ProgramTerminalStatus::Interrupted) << failure_text(second);
    ASSERT_TRUE(second.interrupt().has_value() && second.interrupt()->pending_input);
    EXPECT_EQ(second.interrupt()->pending_input->core_interrupt_id(), "")
        << "the second Send asks again, now the only pending interrupt";
    EXPECT_EQ(peer->transport.load(), 2) << "each Send's journaled call replays; nothing is resent";
    EXPECT_TRUE(second.monetary_records().empty()) << "attempt 1 already charged both calls";
    EXPECT_EQ(h.script().node_runs.load(), 4) << "both Sends ran once more";

    const auto third =
        h.runtime().resume(kOwner, "send-run", resume_after(second, "trace-send-resume-2")).wait();
    ASSERT_EQ(third.status(), ProgramTerminalStatus::Completed) << failure_text(third);
    EXPECT_EQ(peer->transport.load(), 2) << "the second Send's journaled call replays again";
    EXPECT_TRUE(third.monetary_records().empty());
    EXPECT_EQ(third.usage().monetary_microunits, 0U);
    EXPECT_EQ(third.remaining_budget().monetary_microunits, 100'000U - 2U * 1235U);
    EXPECT_EQ(h.script().node_runs.load(), 5) << "the answered second Send ran once more";
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

TEST(ProgramProviderBindingJournal, AForkOfAPausedFanOutNeverResendsOrRechargesTheFinishedCall) {
    // #390: the Core runs llm and check in one step; llm calls the model and
    // finishes, check asks. A fork of the paused run answers check. The fork
    // carries the step's pending writes, so llm replays from them: its request
    // is not sent, and its tokens and money are not charged, again.
    for (const bool durable : {false, true}) {
        SCOPED_TRACE(durable ? "SQLite journal" : "in memory");
        TempDir     files("fork-fan-out");
        auto        peer = std::make_shared<Peer>();
        HostOptions options;
        options.monetary = 100'000;
        options.workers  = 2;
        Host       h(peer, options, durable ? &files : nullptr);
        const auto version = h.admit(Shape::LlmBesideCheck);
        h.script().ask_thread = program_root_core_thread_id(
            "fork-source",
            version.core_materialization_receipt().plans.front().compiled_plan_identity);

        const auto paused = h.runtime().run(kOwner, version, h.invocation("fork-source"));
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
        ASSERT_TRUE(paused.interrupt() && paused.checkpoint());
        EXPECT_EQ(paused.interrupt()->core_node, "check");
        EXPECT_EQ(peer->transport.load(), 1);
        EXPECT_EQ(paused.usage().model_tokens, 24U);
        EXPECT_EQ(paused.usage().monetary_microunits, 1235U);

        auto invocation   = h.invocation("fork-target");
        invocation.budget = neograph::test::fork_share(paused.remaining_budget());
        const auto forked =
            h.runtime()
                .fork(kOwner,
                      ExactProgramCheckpointReference{paused.run_id(),
                                                      paused.checkpoint()->checkpoint_id},
                      version, std::move(invocation), resume_after(paused, "trace-fork-resume"))
                .wait();
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_text(forked);
        EXPECT_EQ(peer->transport.load(), 1) << "the finished call replays in the fork";
        EXPECT_EQ(forked.usage().model_tokens, 0U);
        EXPECT_EQ(forked.usage().monetary_microunits, 0U);
        EXPECT_TRUE(forked.monetary_records().empty());
        EXPECT_EQ(h.script().checks_asked.load(), 1);
        EXPECT_EQ(h.script().checks_answered.load(), 1);
        EXPECT_NE(forked.output().dump().find("answer"), std::string::npos)
            << "the fork keeps the reply of the call it replayed";
    }
}

TEST(ProgramProviderBindingJournal, AForkReplaysAPlanBranchsProviderFailureUnderItsOwnCustody) {
    // #398: a plan quorum of three branches, each calling the model and then a
    // check. The first branch's call fails with the provider's outcome as
    // evidence (#399: a failed branch); the second's check asks, which pauses
    // the quorum. The paused run records the failed branch under the source
    // run's custody. A fork of it replays that record: it used to restore the
    // evidence under the fork's own custody, which the record does not match,
    // so the fork failed. It now re-binds the evidence to the fork, answers
    // the check, runs the third branch and completes, without sending the
    // failed call again.
    for (const bool durable : {false, true}) {
        SCOPED_TRACE(durable ? "SQLite journal" : "in memory");
        TempDir     files("fork-provider-failure");
        auto        peer = std::make_shared<Peer>();
        peer->reply      = [](int ordinal, const ProviderRequest&) -> sp::runtime::Result {
            if (ordinal == 1) {
                sp::Failure failure;
                failure.error.kind         = sp::ErrorKind::InvalidRequest;
                failure.error.safe_message = "refused by the test peer";
                failure.error.http_status  = 400;
                failure.error.retry_safety = sp::RetrySafety::RejectedBeforeOutput;
                return std::make_shared<const sp::Outcome>(std::move(failure));
            }
            return neograph::test::success("answer", with_cost(final_usage(21, 3), 1'234'567));
        };
        HostOptions options;
        options.monetary = 100'000;
        Host h(peer, options, durable ? &files : nullptr);
        h.script().ask_on_check_run = 1;
        const auto call    = json{{"op", "call_core"}};
        const auto version = h.admit_plan(
            json{{"op", "quorum"}, {"branches", json::array({call, call, call})}, {"min_success", 2}},
            Shape::LlmThenCheck);

        const auto paused = h.run_plan(version, "fork-failure-source");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
        ASSERT_TRUE(paused.interrupt() && paused.checkpoint());
        EXPECT_EQ(paused.interrupt()->pending_input->operation_id(), "root.1");
        EXPECT_EQ(peer->transport.load(), 2);

        auto invocation   = h.invocation("fork-failure-target");
        invocation.budget = neograph::test::fork_share(paused.remaining_budget());
        // The fork runs the whole plan again, replaying what the source finished: each
        // operation costs one Program operation, and a branch that calls the model again
        // reserves tokens and money. This test is about custody, so it keeps them whole.
        invocation.budget.max_program_operations = paused.remaining_budget().max_program_operations;
        invocation.budget.model_tokens           = paused.remaining_budget().model_tokens;
        invocation.budget.monetary_microunits    = paused.remaining_budget().monetary_microunits;
        const auto forked =
            h.runtime()
                .fork(kOwner,
                      ExactProgramCheckpointReference{paused.run_id(),
                                                      paused.checkpoint()->checkpoint_id},
                      version, std::move(invocation), resume_after(paused, "trace-fork-failure"))
                .wait();
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed)
            << "the fork could not replay the recorded failure: " << failure_text(forked);
        EXPECT_EQ(peer->transport.load(), 3) << "the failed call is not sent again";
        EXPECT_EQ(h.script().checks_answered.load(), 1);
        EXPECT_EQ(h.script().checks_finished.load(), 1);
    }
}

TEST(ProgramProviderBindingJournal, APlanQuorumResumeNeverResendsOrRechargesAFinishedBranch) {
    // #387: a plan quorum of two branches, each calling the model and then a
    // check. The first branch finishes; the second's check asks, which pauses
    // the quorum. Resume answers it: the first branch replays from its record,
    // so its request is not sent, and its tokens and money are not charged,
    // again.
    for (const bool durable : {false, true}) {
        SCOPED_TRACE(durable ? "SQLite journal" : "in memory");
        TempDir     files("plan-quorum");
        auto        peer = std::make_shared<Peer>();
        HostOptions options;
        options.monetary = 100'000;
        Host h(peer, options, durable ? &files : nullptr);
        h.script().ask_on_check_run = 2;
        const auto call    = json{{"op", "call_core"}};
        const auto version = h.admit_plan(
            json{{"op", "quorum"}, {"branches", json::array({call, call})}, {"min_success", 2}},
            Shape::LlmThenCheck);

        const auto paused = h.run_plan(version, "quorum-run");
        ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
        ASSERT_TRUE(paused.interrupt() && paused.interrupt()->pending_input);
        EXPECT_EQ(paused.interrupt()->pending_input->operation_id(), "root.1");
        EXPECT_EQ(peer->transport.load(), 2);
        EXPECT_EQ(paused.usage().model_tokens, 48U);
        EXPECT_EQ(paused.usage().monetary_microunits, 2U * 1235U);

        const auto done =
            h.runtime().resume(kOwner, "quorum-run", resume_after(paused, "trace-quorum")).wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
        ASSERT_TRUE(done.output().is_array());
        EXPECT_EQ(done.output().size(), 2U);
        EXPECT_EQ(peer->transport.load(), 2) << "the finished branch's request is never sent again";
        EXPECT_EQ(done.usage().model_tokens, 0U);
        EXPECT_EQ(done.usage().monetary_microunits, 0U);
        EXPECT_TRUE(done.monetary_records().empty());
        EXPECT_EQ(h.script().checks_finished.load(), 1) << "the finished branch is replayed";
        EXPECT_EQ(h.script().checks_answered.load(), 1);
    }
}

}  // namespace

namespace {
// #531: a host whose provider is a real SDK client at a loopback peer that admits the endpoint facts of
// `providers`' origins.
HostOptions endpoint_options(const std::string& origin,
                             std::vector<std::string> providers = {"https://api.openai.com"}) {
    HostOptions options;
    options.provider = [origin, providers] {
        return std::shared_ptr<Provider>(neograph::test::endpoints::provider_at(origin, true, providers));
    };
    options.model_tokens = 0;
    options.monetary     = 0;
    options.priced       = false;
    return options;
}
}  // namespace

TEST(ProgramProviderBinding, AnEndpointNodeDeclaresNoEffectContractSoNoChannelAnalysisRuns) {
    // #531: an endpoint node's channels are its configuration. Registered with an empty effects object, a
    // Program compile counted it as a node that reads and writes nothing, and reported its channels as dead.
    // Without an effect contract the validator skips the analysis, as it does for the same node in a Core graph.
    Host       h(std::make_shared<Peer>(), HostOptions{});
    const auto bundle = h.compile(Shape::EmbedBesideCheck);
    for (const auto& diagnostic : bundle.diagnostics())
        EXPECT_NE(diagnostic.code, "P_CORE_E6") << diagnostic.message << " (sealed)";
    for (const auto& diagnostic : bundle.compile_diagnostics())
        EXPECT_NE(diagnostic.code, "P_CORE_E6") << diagnostic.message;
}

TEST(ProgramProviderBindingJournal, AnEndpointCallIsJournaledAndAForkReplaysIt) {
    // #531: an endpoint node takes the Core provider path. The journal keeps its untyped outcome, and a fork of
    // the paused run replays it instead of sending it again.
    TempDir                    files("endpoint-fork");
    neograph::test::wire::Peer peer(neograph::test::endpoints::embedding_body());
    auto                       options = endpoint_options(peer.origin());
    options.workers                    = 2;
    Host       h(std::make_shared<Peer>(), options, &files);
    const auto version = h.admit(Shape::EmbedBesideCheck);
    h.script().ask_thread = program_root_core_thread_id(
        "endpoint-source", version.core_materialization_receipt().plans.front().compiled_plan_identity);

    const auto paused = h.runtime().run(kOwner, version, h.invocation("endpoint-source"));
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
    ASSERT_TRUE(paused.interrupt() && paused.checkpoint());
    EXPECT_EQ(peer.state->entered, 1U);
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"});

    auto invocation   = h.invocation("endpoint-fork");
    invocation.budget = neograph::test::fork_share(paused.remaining_budget());
    const auto forked =
        h.runtime()
            .fork(kOwner, ExactProgramCheckpointReference{paused.run_id(), paused.checkpoint()->checkpoint_id},
                  version, std::move(invocation), resume_after(paused, "trace-endpoint-fork"))
            .wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_text(forked);
    EXPECT_EQ(peer.state->entered, 1U) << "the finished endpoint call replays in the fork";
    EXPECT_NE(forked.output().dump().find("-0.5"), std::string::npos) << "the fork keeps the replayed vectors";
}

TEST(ProgramProviderBindingJournal, APricedBindingRefusesAnEndpointCallBeforeItIsSent) {
    // #531: the model catalogue cannot bound an endpoint call yet (SchemaProvider#124).
    TempDir                    files("endpoint-priced");
    neograph::test::wire::Peer peer(neograph::test::endpoints::embedding_body());
    auto                       options = endpoint_options(peer.origin());
    options.priced                     = true;
    options.monetary                   = 100'000;
    Host       h(std::make_shared<Peer>(), options, &files);
    const auto result = h.run(Shape::Embed, "endpoint-priced");
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_NE(failure_text(result).find("P_MONEY_UNMODELED"), std::string::npos) << failure_text(result);
    EXPECT_EQ(peer.state->entered, 0U);
    EXPECT_TRUE(journal_states(files.provider_db()).empty()) << "no marker";
}

TEST(ProgramProviderBindingJournal, AModelTokenBudgetRefusesAnEndpointCallItCannotBound) {
    // #531: an endpoint call has no output cap, so a run with a model-token ceiling cannot reserve it.
    TempDir                    files("endpoint-tokens");
    neograph::test::wire::Peer peer(neograph::test::endpoints::embedding_body());
    auto                       options = endpoint_options(peer.origin());
    options.model_tokens               = 1'000'000;
    Host       h(std::make_shared<Peer>(), options, &files);
    const auto result = h.run(Shape::Embed, "endpoint-tokens");
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_NE(failure_text(result).find("ceilings"), std::string::npos) << failure_text(result);
    EXPECT_EQ(peer.state->entered, 0U);
    EXPECT_TRUE(journal_states(files.provider_db()).empty()) << "no marker";
}

TEST(ProgramProviderBindingJournal, AVideoJobPausesTheProgramAndAResumePollsItAgain) {
    // #531: the submit is journaled once. A poll is a read the provider never charges, so the journal sends it
    // each time: the resumed run sees the job finished instead of replaying the first answer forever.
    TempDir                              files("endpoint-job");
    neograph::test::endpoints::VideoPeer peer({"in_progress", "completed"});
    Host h(std::make_shared<Peer>(), endpoint_options(peer.origin(), {"https://openrouter.ai"}), &files);
    const auto paused = h.run(Shape::VideoJob, "job-run");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
    ASSERT_TRUE(paused.interrupt());
    EXPECT_EQ(paused.interrupt()->core_node, "collect");
    EXPECT_EQ(peer.submits.load(), 1);
    EXPECT_EQ(peer.polls_seen.load(), 1U);
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"}) << "the submit only";

    const auto done = h.runtime().resume(kOwner, "job-run", resume_after(paused, "trace-job")).wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
    EXPECT_EQ(peer.submits.load(), 1) << "the submit is not sent again";
    EXPECT_EQ(peer.polls_seen.load(), 2U);
    EXPECT_EQ(peer.downloads.load(), 1);
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"});
    EXPECT_NE(done.output().dump().find("video/mp4"), std::string::npos) << done.output().dump();
}

namespace {
void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}
}  // namespace

TEST(ProgramProviderBindingJournal, AnUploadIsReplayedAfterAResumeWhereverItsFileIsNow) {
    // #541: the journal names an upload by the bytes it sends, not by the path they were read from.
    TempDir                              files("endpoint-upload-moved");
    neograph::test::endpoints::UploadPeer peer;
    Host h(std::make_shared<Peer>(), endpoint_options(peer.origin()), &files);
    const auto source = files.path / "source.bin";
    write_file(source, "the first contents");
    h.script().upload_path           = source.string();
    h.script().upload_size           = 18;
    h.script().interrupt_after_call  = true;
    const auto paused = h.run(Shape::FileUpload, "upload-moved");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
    EXPECT_EQ(peer.received(), std::vector<std::string>{"the first contents"});
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"});

    const auto moved = files.path / "moved.bin";
    std::filesystem::rename(source, moved);
    h.script().upload_path = moved.string();
    const auto done = h.runtime().resume(kOwner, "upload-moved", resume_after(paused, "trace-upload-moved")).wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
    EXPECT_EQ(peer.received().size(), 1U) << "the journaled upload replays from its new path";
    EXPECT_NE(done.output().dump().find("file-1"), std::string::npos) << done.output().dump();
}

TEST(ProgramProviderBindingJournal, AnUploadWhoseFileChangedBeforeTheResumeIsNotReplayed) {
    // #541: the file keeps its path and its size, but not its bytes, so the resumed call is not the journaled one.
    TempDir                              files("endpoint-upload-changed");
    neograph::test::endpoints::UploadPeer peer;
    Host h(std::make_shared<Peer>(), endpoint_options(peer.origin()), &files);
    const auto source = files.path / "source.bin";
    write_file(source, "the first contents");
    h.script().upload_path           = source.string();
    h.script().upload_size           = 18;
    h.script().interrupt_after_call  = true;
    const auto paused = h.run(Shape::FileUpload, "upload-changed");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
    ASSERT_EQ(peer.received().size(), 1U);

    write_file(source, "the other contents");
    const auto resumed =
        h.runtime().resume(kOwner, "upload-changed", resume_after(paused, "trace-upload-changed")).wait();
    ASSERT_EQ(resumed.status(), ProgramTerminalStatus::Failed) << failure_text(resumed);
    EXPECT_NE(failure_text(resumed).find("binding conflict"), std::string::npos) << failure_text(resumed);
    EXPECT_EQ(peer.received().size(), 1U) << "the changed file is not sent under the journaled call";
}

TEST(ProgramProviderBindingJournal, AnUploadFromMemoryIsReplayedAfterAResume) {
    // #541: the SDK's multipart boundary is random, so the body of an upload from memory differs on every
    // preparation; its identity must not.
    TempDir                              files("endpoint-upload-memory");
    neograph::test::endpoints::UploadPeer peer;
    Host h(std::make_shared<Peer>(), endpoint_options(peer.origin()), &files);
    h.script().upload_bytes          = "bytes in memory";
    h.script().interrupt_after_call  = true;
    const auto paused = h.run(Shape::FileUpload, "upload-memory");
    ASSERT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_text(paused);
    const auto done = h.runtime().resume(kOwner, "upload-memory", resume_after(paused, "trace-upload-memory")).wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
    EXPECT_EQ(peer.received(), std::vector<std::string>{"bytes in memory"}) << "uploaded once";
}

TEST(ProgramProviderBindingJournal, TheJournalAndCheckpointsHoldNoCredential) {
    // #531: an endpoint call's API key travels in its authorization header, never in what is stored.
    TempDir                    files("endpoint-secrets");
    neograph::test::wire::Peer peer(neograph::test::endpoints::embedding_body());
    Host                       h(std::make_shared<Peer>(), endpoint_options(peer.origin()), &files);
    const auto                 result = h.run(Shape::Embed, "secret-run");
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_text(result);
    ASSERT_EQ(peer.state->authorization.back(), "Bearer fixture-key");
    h.close();
    std::size_t scanned = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(files.path)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(bytes.find("fixture-key"), std::string::npos) << entry.path();
        EXPECT_EQ(bytes.find("Bearer"), std::string::npos) << entry.path();
        ++scanned;
    }
    EXPECT_GE(scanned, 3U) << "the program, checkpoint and provider databases";
}

// ---------------------------------------------------------------------------
// #543: a call is named by what it asks for, not by the policy that admitted it
// ---------------------------------------------------------------------------

namespace {
using Journal = SQLiteProgramProviderCallJournal;
using Refused = ProgramProviderForkResendRefusedError;

void exec_sql(const std::string& file, const std::string& sql) {
    Connection db(file, SQLITE_OPEN_READWRITE);
    char*      error = nullptr;
    const auto code  = sqlite3_exec(db.db, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "";
    sqlite3_free(error);
    if (code != SQLITE_OK) throw std::runtime_error("sql failed: " + message);
}

// A row as 0.16.0 and 0.15 wrote it: its request_digest is the dispatch digest, which includes the policy, and it
// has no content, policy or producer.
void as_written_before_content_identity(const std::string& provider_db, const std::string& dispatch_digest) {
    exec_sql(provider_db, "UPDATE ng_program_provider_calls SET identity_version='1', content_digest='', "
                          "policy_identity='', producer='', request_digest='" + dispatch_digest + "'");
}

// What the request of one built-in call asks: the same one whatever the policy.
ProviderRequest ask(const Provider& provider, std::string text = "probe", std::string system = {},
                    ProviderMode mode = ProviderMode::Collect) {
    std::vector<sp::Message> messages;
    if (!system.empty()) messages.push_back(neograph::test::message(std::move(system), sp::Role::System));
    messages.push_back(neograph::test::message(std::move(text), sp::Role::User));
    auto request = make_provider_request(provider, std::string(kModel), std::move(messages), {}, {}, mode);
    std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
    return request;
}

// A provider over a client admitted under the policy `edit` makes.
std::shared_ptr<Provider> provider_under(std::shared_ptr<Peer> peer, const std::function<void(json&)>& edit = {},
                                         std::string origin = std::string(kOpenRouter)) {
    return make_provider(std::move(peer), client_for(std::move(origin), std::string(kModel), kInputLimit, edit));
}

// Where one call is made: a run, a Core thread and an attempt, and for a fork the run it repeats the calls of.
struct Position {
    std::string run, thread, operation = "root";
    std::uint64_t attempt = 1;
    std::optional<ProgramCoreProviderCallReplaySource> replay;
    // What the broker told the host.
    std::shared_ptr<std::vector<ProgramCoreProviderCallDiagnostic>> told =
        std::make_shared<std::vector<ProgramCoreProviderCallDiagnostic>>();

    ProgramCoreProviderCallContext context() const {
        ProgramCoreProviderCallContext context{kOwner, "version:identity", run, operation, attempt};
        context.replay_source = replay;
        context.diagnostic    = [told = told](ProgramCoreProviderCallDiagnostic value) { told->push_back(std::move(value)); };
        return context;
    }
    graph::ProviderCallIdentity identity() const {
        return graph::ProviderCallIdentity{kOwner, run, thread, "s1:work", "work", 0};
    }
    std::string id() const { return Journal::logical_call_id(context(), identity()); }
    std::vector<std::string> codes() const {
        std::vector<std::string> codes;
        for (const auto& value : *told) codes.push_back(value.code);
        return codes;
    }
};

Position source_position() { return Position{"run:source", "thread:source"}; }

// The same position at a later attempt, with a record of what is told of its own.
Position at_attempt(Position position, std::uint64_t attempt) {
    position.attempt = attempt;
    position.told    = std::make_shared<std::vector<ProgramCoreProviderCallDiagnostic>>();
    return position;
}

// A fork of the source: its calls on `thread:fork` repeat the source's on `thread:source`.
Position fork_position(bool may_resend = false) {
    Position fork{"run:fork", "thread:fork"};
    fork.replay = ProgramCoreProviderCallReplaySource{"run:source", "version:identity",
                                                      {{"thread:fork", "thread:source"}}, 1, may_resend};
    return fork;
}

sp::runtime::Result make_call(Journal& journal, const Position& at, std::shared_ptr<Provider> provider,
                              ProviderRequest request, std::string deployment = digest('a')) {
    auto binding = journal.bind(at.context(), std::move(deployment));
    return neograph::async::run_sync(binding.broker->invoke(at.identity(), std::move(provider), std::move(request)));
}

std::string text_of(const sp::runtime::Result& result) { return neograph::test::text(result); }

bool is_sha256(const std::string& value) {
    return value.size() == 7 + 64 && value.starts_with("sha256:") &&
           value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}
}  // namespace

TEST(ProgramProviderIdentity, TheContentDigestNamesTheRequestAndTheDispatchDigestAlsoNamesThePolicy) {
    auto peer = std::make_shared<Peer>();
    const auto prepare = [&](const std::function<void(json&)>& edit, std::string text = "probe",
                             std::string system = {}, ProviderMode mode = ProviderMode::Collect,
                             std::string origin = std::string(kOpenRouter)) {
        auto provider = provider_under(peer, edit, std::move(origin));
        auto prepared = provider->prepare(ask(*provider, std::move(text), std::move(system), mode));
        EXPECT_TRUE(prepared.valid());
        return prepared;
    };
    const auto base  = prepare({});
    const auto again = prepare({});
    EXPECT_EQ(Provider::request_content_digest(base), Provider::request_content_digest(again));
    EXPECT_EQ(Provider::request_digest(base), Provider::request_digest(again));
    EXPECT_EQ(Provider::request_policy_identity(base), Provider::request_policy_identity(again));
    EXPECT_TRUE(is_sha256(Provider::request_content_digest(base)));
    EXPECT_TRUE(is_sha256(Provider::request_policy_identity(base)));

    // The policy changes: its identity and the dispatch digest change, what the request asks does not.
    const auto stop = prepare(another_stop_reason("x_test_stop"));
    EXPECT_NE(Provider::request_policy_identity(stop), Provider::request_policy_identity(base));
    EXPECT_NE(Provider::request_digest(stop), Provider::request_digest(base));
    EXPECT_EQ(Provider::request_content_digest(stop), Provider::request_content_digest(base));

    // A policy that writes the output cap to another member changes the encoded body, and still not the content.
    const auto cap = prepare(another_output_cap_member("max_completion_tokens"));
    EXPECT_NE(cap.encoded_body(), base.encoded_body());
    EXPECT_NE(Provider::request_policy_identity(cap), Provider::request_policy_identity(base));
    EXPECT_EQ(Provider::request_content_digest(cap), Provider::request_content_digest(base));

    // Anything the request says changes the content: the user's text, system text, the mode and the origin.
    const auto content = Provider::request_content_digest(base);
    EXPECT_NE(Provider::request_content_digest(prepare({}, "another question")), content);
    EXPECT_NE(Provider::request_content_digest(prepare({}, "probe", "Answer in French.")), content);
    EXPECT_NE(Provider::request_content_digest(prepare({}, "probe", {}, ProviderMode::Stream)), content);
    EXPECT_NE(Provider::request_content_digest(prepare({}, "probe", {}, ProviderMode::Collect, "https://api.openai.com")),
              content);
    EXPECT_NE(Provider::request_content_digest(prepare({}, "probe", "Answer in French.")),
              Provider::request_content_digest(prepare({}, "probe", "Answer in German.")))
        << "the system text NeoGraph supplies is part of the request, so changing it changes the content";
}

TEST(ProgramProviderIdentity, ThePromptNeoGraphSuppliesIsPartOfTheContentOfTheCall) {
    // The intent classifier writes its own system prompt when the host gives none. That text is part of the
    // request, so it is part of the call's content: a NeoGraph that words it differently, or a host that
    // replaces it, is another call, and a journaled call is not replayed for it.
    auto peer = std::make_shared<Peer>();
    const auto classify = [&](std::string prompt, std::vector<std::string> routes) {
        graph::NodeContext context;
        context.provider = provider_under(peer);
        context.model    = std::string(kModel);
        graph::IntentClassifierNode node("router", context, prompt, std::move(routes));
        graph::GraphState           state;
        state.init_channel("messages", graph::ReducerType::APPEND, graph::ReducerRegistry::instance().get("append"));
        state.write("messages", json::array({json{{"role", "user"}, {"content", "where can I buy X?"}}}));
        graph::RunContext run;
        (void)neograph::async::run_sync(node.run(graph::NodeInput{state, run, nullptr}));
        return peer->contents.back();
    };
    const auto built_in = classify("", {"shopping", "support"});
    EXPECT_EQ(classify("", {"shopping", "support"}), built_in) << "the same prompt is the same content";
    EXPECT_NE(classify("", {"shopping", "billing"}), built_in) << "the built-in prompt names the routes";
    EXPECT_NE(classify("Pick one route.", {"shopping", "support"}), built_in);
    EXPECT_EQ(peer->transport.load(), 4);
}

TEST(ProgramProviderIdentity, AReplayUnderAnotherPolicyIdentityIsTheSameCallAndSaysSo) {
    TempDir files("identity-replay");
    auto    peer = std::make_shared<Peer>();
    auto    at   = source_position();
    {
        Journal journal(files.provider_db());
        const auto first = make_call(journal, at, provider_under(peer), ask(*provider_under(peer)));
        EXPECT_EQ(text_of(first), "answer");
        const auto row = journal.inspect(kOwner, at.id());
        ASSERT_TRUE(row);
        EXPECT_EQ(row->identity_version, 2U);
        EXPECT_TRUE(is_sha256(row->content_digest));
        EXPECT_TRUE(is_sha256(row->policy_identity));
        EXPECT_FALSE(row->producer.empty());
        EXPECT_TRUE(at.told->empty());
    }
    // The next attempt runs under another policy identity: the same call, replayed and not sent again.
    auto       later = at_attempt(at, 2);
    Journal    reopened(files.provider_db());
    auto       other = std::make_shared<Peer>();
    const auto under_other = provider_under(other, another_stop_reason("x_test_stop"));
    const auto replayed    = make_call(reopened, later, under_other, ask(*under_other));
    EXPECT_EQ(text_of(replayed), "answer");
    EXPECT_EQ(other->transport.load(), 0) << "the journaled call is replayed under the new policy";
    EXPECT_EQ(peer->transport.load(), 1);
    ASSERT_EQ(later.codes(), std::vector<std::string>{"P_PROVIDER_CALL_POLICY_CHANGED"});
    const auto& witness = later.told->front().witness;
    EXPECT_EQ(witness["via"].get<std::string>(), "replay");
    EXPECT_EQ(witness["call_id"].get<std::string>(), at.id());
    const auto changes = reopened.policy_changes(kOwner, at.id());
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes[0].via, Journal::PolicyChange::Via::Replay);
    EXPECT_EQ(changes[0].attempt, 2U);
    EXPECT_EQ(changes[0].journaled_policy_identity, reopened.inspect(kOwner, at.id())->policy_identity);
    EXPECT_NE(changes[0].observed_policy_identity, changes[0].journaled_policy_identity);
    EXPECT_EQ(changes[0].observed_policy_identity, Provider::request_policy_identity(under_other->prepare(ask(*under_other))));
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"}) << "the row is not rewritten";

    // The same policy again tells nothing, and a second change under the same policy is not noted twice.
    auto third = at_attempt(at, 3);
    (void)make_call(reopened, third, under_other, ask(*under_other));
    EXPECT_EQ(third.codes(), std::vector<std::string>{"P_PROVIDER_CALL_POLICY_CHANGED"}) << "it is told each time";
    EXPECT_EQ(reopened.policy_changes(kOwner, at.id()).size(), 1U) << "and noted once";
    auto same = at_attempt(at, 4);
    (void)make_call(reopened, same, provider_under(peer), ask(*provider_under(peer)));
    EXPECT_TRUE(same.codes().empty());
}

TEST(ProgramProviderIdentity, ARequestWhoseContentChangedIsAConflictWhateverThePolicy) {
    TempDir files("identity-conflict");
    auto    peer = std::make_shared<Peer>();
    auto    at   = source_position();
    Journal journal(files.provider_db());
    (void)make_call(journal, at, provider_under(peer), ask(*provider_under(peer)));
    at.attempt = 2;
    const auto count = [&](const std::function<void(json&)>& edit, std::string text, std::string system,
                           std::string deployment = digest('a')) {
        auto provider = provider_under(peer, edit);
        EXPECT_THROW(make_call(journal, at, provider, ask(*provider, std::move(text), std::move(system)),
                               std::move(deployment)),
                     std::runtime_error);
    };
    count({}, "another question", {});
    count(another_stop_reason("x_test_stop"), "another question", {});
    count({}, "probe", "Answer in French.");
    count({}, "probe", {}, digest('b'));
    try {
        auto provider = provider_under(peer);
        (void)make_call(journal, at, provider, ask(*provider, "another question"));
        FAIL() << "a changed question must not replay";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("binding conflict"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find("content differs"), std::string::npos) << error.what();
    }
    EXPECT_EQ(peer->transport.load(), 1);
    EXPECT_TRUE(at.told->empty());
    EXPECT_TRUE(journal.policy_changes(kOwner, at.id()).empty());
}

TEST(ProgramProviderIdentity, AForkAdoptsTheSourcesCallUnderAnotherPolicyIdentity) {
    TempDir files("identity-fork");
    auto    peer = std::make_shared<Peer>();
    auto    source = source_position();
    Journal journal(files.provider_db());
    (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));

    auto       fork  = fork_position();
    auto       other = std::make_shared<Peer>();
    const auto under_other = provider_under(other, another_stop_reason("x_test_stop"));
    const auto adopted     = make_call(journal, fork, under_other, ask(*under_other));
    EXPECT_EQ(text_of(adopted), "answer");
    EXPECT_EQ(other->transport.load(), 0) << "the call was paid for by the source and is not sent again";
    EXPECT_EQ(peer->transport.load(), 1);
    EXPECT_EQ(journal_states(files.provider_db()), (std::vector<std::string>{"succeeded", "succeeded"}));
    const auto row = journal.inspect(kOwner, fork.id());
    ASSERT_TRUE(row);
    EXPECT_EQ(row->original_attempt, "1") << "the fork's row keeps the source's attempt, so it is not charged again";
    EXPECT_EQ(row->identity_version, 2U);
    EXPECT_EQ(row->content_digest, journal.inspect(kOwner, source.id())->content_digest);
    EXPECT_NE(row->policy_identity, journal.inspect(kOwner, source.id())->policy_identity);
    ASSERT_EQ(fork.codes(), std::vector<std::string>{"P_PROVIDER_CALL_POLICY_CHANGED"});
    EXPECT_EQ(fork.told->front().witness["via"].get<std::string>(), "adoption");
    EXPECT_EQ(fork.told->front().witness["source_call_id"].get<std::string>(), source.id());
    const auto changes = journal.policy_changes(kOwner, fork.id());
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes[0].via, Journal::PolicyChange::Via::Adoption);
    EXPECT_EQ(changes[0].journaled_policy_identity, journal.inspect(kOwner, source.id())->policy_identity);
    EXPECT_EQ(changes[0].observed_policy_identity, row->policy_identity);
    EXPECT_TRUE(journal.policy_changes(kOwner, source.id()).empty());
}

TEST(ProgramProviderIdentity, AForkSendsACallWhoseRequestDiffersFromTheSources) {
    // Another request at the same position is a new call: nothing the source paid for is sent again.
    TempDir files("identity-fork-new");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    Journal journal(files.provider_db());
    (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));

    auto fork = fork_position();
    const auto provider = provider_under(peer);
    EXPECT_EQ(text_of(make_call(journal, fork, provider, ask(*provider, "another question"))), "answer");
    EXPECT_EQ(peer->transport.load(), 2);
    EXPECT_TRUE(fork.told->empty());
    EXPECT_EQ(journal_states(files.provider_db()), (std::vector<std::string>{"succeeded", "succeeded"}));

    // A call the source never made at its position is new too.
    auto elsewhere = fork_position();
    elsewhere.thread = "thread:elsewhere";
    elsewhere.replay->threads = {{"thread:elsewhere", "thread:never-used"}};
    EXPECT_EQ(text_of(make_call(journal, elsewhere, provider, ask(*provider))), "answer");
    EXPECT_EQ(peer->transport.load(), 3);
    EXPECT_TRUE(elsewhere.told->empty());
}

TEST(ProgramProviderIdentity, ARowFromBeforeContentIdentityMatchesWhileThePolicyIsUnchanged) {
    // Journals written by 0.16.0 keep resuming: their request_digest is the dispatch digest, which the journal
    // reads as it did.
    TempDir files("identity-0160");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    {
        Journal journal(files.provider_db());
        (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));
    }
    ASSERT_EQ(peer->digests.size(), 1U);
    as_written_before_content_identity(files.provider_db(), peer->digests.front());
    Journal journal(files.provider_db());
    const auto row = journal.inspect(kOwner, source.id());
    ASSERT_TRUE(row);
    EXPECT_EQ(row->identity_version, 1U);
    EXPECT_EQ(row->request_digest, peer->digests.front());
    EXPECT_TRUE(row->content_digest.empty() && row->policy_identity.empty() && row->producer.empty());

    // The policy is unchanged: the resumed attempt replays the call and the fork adopts it.
    auto later = at_attempt(source, 2);
    auto none  = std::make_shared<Peer>();
    EXPECT_EQ(text_of(make_call(journal, later, provider_under(none), ask(*provider_under(none)))), "answer");
    auto fork = fork_position();
    EXPECT_EQ(text_of(make_call(journal, fork, provider_under(none), ask(*provider_under(none)))), "answer");
    EXPECT_EQ(none->transport.load(), 0);
    EXPECT_TRUE(later.told->empty() && fork.told->empty());
    EXPECT_EQ(journal.inspect(kOwner, fork.id())->identity_version, 2U) << "the fork's own row is written now";

    // The policy changed since the row was written: its digest no longer matches, and it cannot say whether the
    // policy or the request changed. A resume is a conflict.
    auto       changed = at_attempt(source, 3);
    const auto under_other = provider_under(none, another_stop_reason("x_test_stop"));
    try {
        (void)make_call(journal, changed, under_other, ask(*under_other));
        FAIL() << "a row that cannot be matched must not replay";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("binding conflict"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find("predates content identity"), std::string::npos) << error.what();
    }
    EXPECT_EQ(none->transport.load(), 0);
}

TEST(ProgramProviderIdentity, AForkRefusesAJournaledCallItCannotMatchAndSendsItOnlyWhenAllowed) {
    // A journal from 0.15, or one from 0.16.0 whose policy has changed since: the source's digest names the old
    // policy, and the fork cannot tell the same call under a new policy from a different call.
    TempDir files("identity-refuse");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    {
        Journal journal(files.provider_db());
        (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));
    }
    as_written_before_content_identity(files.provider_db(), digest('7'));
    Journal journal(files.provider_db());
    auto    none = std::make_shared<Peer>();
    const auto provider = provider_under(none);

    auto refused = fork_position();
    try {
        (void)make_call(journal, refused, provider, ask(*provider));
        FAIL() << "the fork must not send the call again";
    } catch (const Refused& error) {
        EXPECT_EQ(error.reason(), Refused::Reason::Unverifiable);
        EXPECT_EQ(error.call_id(), refused.id());
        EXPECT_EQ(error.source_run_id(), "run:source");
        EXPECT_EQ(error.source_call_id(), source.id());
        EXPECT_NE(std::string(error.what()).find("fork_may_resend_provider_calls"), std::string::npos) << error.what();
        EXPECT_NE(dynamic_cast<const graph::ConfigurationFailure*>(&error), nullptr)
            << "inside a Core node it is a configuration verdict: never retried, never routed";
    }
    EXPECT_EQ(none->transport.load(), 0);
    EXPECT_FALSE(journal.inspect(kOwner, refused.id())) << "nothing is journaled for a refused call";
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"succeeded"});

    // The host allows it: the call is sent, journaled under the fork and reported.
    auto allowed = fork_position(true);
    EXPECT_EQ(text_of(make_call(journal, allowed, provider, ask(*provider))), "answer");
    EXPECT_EQ(none->transport.load(), 1);
    ASSERT_EQ(allowed.codes(), std::vector<std::string>{"P_FORK_PROVIDER_CALL_RESENT"});
    const auto& witness = allowed.told->front().witness;
    EXPECT_EQ(witness["reason"].get<std::string>(), "unverifiable");
    EXPECT_EQ(witness["source_run_id"].get<std::string>(), "run:source");
    EXPECT_EQ(witness["call_id"].get<std::string>(), allowed.id());
    EXPECT_EQ(journal.inspect(kOwner, allowed.id())->original_attempt, "1");
    EXPECT_EQ(journal_states(files.provider_db()), (std::vector<std::string>{"succeeded", "succeeded"}));

    // The resume of the source itself is a conflict, as it was.
    auto later = at_attempt(source, 2);
    EXPECT_THROW(make_call(journal, later, provider, ask(*provider)), std::runtime_error);
}

TEST(ProgramProviderIdentity, AForkTellsAChangedRequestFromAChangedEncoder) {
    // A request that differs from the source's is a new call only when the same NeoGraph build encoded both. A
    // source written by another build cannot show that the request, and not its encoding, changed.
    TempDir files("identity-encoder");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    {
        Journal journal(files.provider_db());
        (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));
    }
    exec_sql(files.provider_db(), "UPDATE ng_program_provider_calls SET producer='neograph/0.0.1'");
    Journal journal(files.provider_db());
    auto    none = std::make_shared<Peer>();
    const auto provider = provider_under(none);

    // The same content is adopted, whichever build encoded it.
    auto same = fork_position();
    EXPECT_EQ(text_of(make_call(journal, same, provider, ask(*provider))), "answer");
    EXPECT_EQ(none->transport.load(), 0);

    // Different content from another build: refused.
    auto refused = fork_position();
    refused.run = "run:fork-2";
    try {
        (void)make_call(journal, refused, provider, ask(*provider, "another question"));
        FAIL() << "the fork must not send a call that may be the source's";
    } catch (const Refused& error) {
        EXPECT_EQ(error.reason(), Refused::Reason::EncoderDiffers);
    }
    EXPECT_EQ(none->transport.load(), 0);
    auto allowed = fork_position(true);
    allowed.run  = "run:fork-3";
    (void)make_call(journal, allowed, provider, ask(*provider, "another question"));
    EXPECT_EQ(none->transport.load(), 1);
    EXPECT_EQ(allowed.told->front().witness["reason"].get<std::string>(), "encoder_differs");

    // The resume of the source is a conflict that names both builds.
    auto later = at_attempt(source, 2);
    try {
        (void)make_call(journal, later, provider, ask(*provider, "another question"));
        FAIL() << "a changed question must not replay";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("neograph/0.0.1"), std::string::npos) << error.what();
    }
}

TEST(ProgramProviderIdentity, AForkRefusesACallTheSourceJournaledUnderAnotherDeployment) {
    TempDir files("identity-deployment");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    Journal journal(files.provider_db());
    (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));
    auto       none = std::make_shared<Peer>();
    const auto provider = provider_under(none);
    auto       refused  = fork_position();
    try {
        (void)make_call(journal, refused, provider, ask(*provider), digest('b'));
        FAIL() << "the fork must not pay for the call again under another deployment";
    } catch (const Refused& error) {
        EXPECT_EQ(error.reason(), Refused::Reason::DeploymentDiffers);
    }
    EXPECT_EQ(none->transport.load(), 0);
    auto allowed = fork_position(true);
    EXPECT_EQ(text_of(make_call(journal, allowed, provider, ask(*provider), digest('b'))), "answer");
    EXPECT_EQ(none->transport.load(), 1);
    EXPECT_EQ(allowed.told->front().witness["reason"].get<std::string>(), "deployment_differs");
}

TEST(ProgramProviderIdentity, AForkNeverSendsAgainACallWhoseSourceOutcomeIsUnknown) {
    TempDir files("identity-uncertain");
    auto    peer   = std::make_shared<Peer>();
    peer->reply    = [](int, const ProviderRequest&) -> sp::runtime::Result {
        throw std::runtime_error("transport disconnected after send");
    };
    auto source = source_position();
    {
        Journal journal(files.provider_db());
        EXPECT_THROW(make_call(journal, source, provider_under(peer), ask(*provider_under(peer))), std::runtime_error);
    }
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"reconciliation_required"});
    Journal journal(files.provider_db());
    auto    none = std::make_shared<Peer>();
    const auto provider = provider_under(none);
    // The same call: uncertain in the fork too, whatever the host allows.
    for (const bool allowed : {false, true}) {
        auto fork = fork_position(allowed);
        fork.run  = allowed ? "run:fork-allowed" : "run:fork";
        EXPECT_THROW(make_call(journal, fork, provider, ask(*provider)), ProgramProviderDispatchUncertainError);
    }
    EXPECT_EQ(none->transport.load(), 0);
    // A different request is a new call.
    auto fork = fork_position();
    fork.run  = "run:fork-new";
    EXPECT_EQ(text_of(make_call(journal, fork, provider, ask(*provider, "another question"))), "answer");
    EXPECT_EQ(none->transport.load(), 1);
    // A source call that could not be matched, and whose outcome is unknown, is refused.
    as_written_before_content_identity(files.provider_db(), digest('7'));
    Journal legacy(files.provider_db());
    auto refused = fork_position();
    refused.run  = "run:fork-legacy";
    EXPECT_THROW(make_call(legacy, refused, provider, ask(*provider)), Refused);
}

TEST(ProgramProviderIdentity, AFailureTheProviderDidNotAcceptIsSentAgainByAFork) {
    // A failure proven not accepted was not paid for, so a fork sends the call again without asking.
    TempDir files("identity-failed");
    auto    peer = std::make_shared<Peer>();
    peer->reply  = [](int ordinal, const ProviderRequest&) -> sp::runtime::Result {
        if (ordinal == 1) {
            sp::Failure failure;
            failure.error.kind         = sp::ErrorKind::Transport;
            failure.error.retry_safety = sp::RetrySafety::NotSent;
            return std::make_shared<const sp::Outcome>(std::move(failure));
        }
        return neograph::test::success("answer", with_cost(final_usage(21, 3), 1'234'567));
    };
    auto source = source_position();
    {
        Journal journal(files.provider_db());
        const auto provider = provider_under(peer);
        (void)make_call(journal, source, provider, ask(*provider));
    }
    ASSERT_EQ(journal_states(files.provider_db()), std::vector<std::string>{"failed"});
    as_written_before_content_identity(files.provider_db(), digest('7'));
    Journal journal(files.provider_db());
    const auto provider = provider_under(peer);
    auto       fork     = fork_position();
    EXPECT_EQ(text_of(make_call(journal, fork, provider, ask(*provider))), "answer");
    EXPECT_EQ(peer->transport.load(), 2);
}

TEST(ProgramProviderIdentity, ATamperedIdentityIsCorruptNotAConflict) {
    TempDir files("identity-tamper");
    auto    peer   = std::make_shared<Peer>();
    auto    source = source_position();
    {
        Journal journal(files.provider_db());
        (void)make_call(journal, source, provider_under(peer), ask(*provider_under(peer)));
    }
    for (const auto& sql : std::vector<std::string>{
             "UPDATE ng_program_provider_calls SET request_digest='" + digest('e') + "'",
             "UPDATE ng_program_provider_calls SET content_digest='" + digest('e') + "'",
             "UPDATE ng_program_provider_calls SET policy_identity='" + digest('e') + "'",
             "UPDATE ng_program_provider_calls SET producer=''",
             "UPDATE ng_program_provider_calls SET identity_version='1'",
             "UPDATE ng_program_provider_calls SET identity_version='3'"}) {
        SCOPED_TRACE(sql);
        TempDir copy("identity-tamper-copy");
        crash_image(files.path, copy.path);
        exec_sql(copy.provider_db(), sql);
        Journal journal(copy.provider_db());
        auto at = at_attempt(source, 2);
        EXPECT_ANY_THROW(make_call(journal, at, provider_under(peer), ask(*provider_under(peer))));
        EXPECT_EQ(peer->transport.load(), 1);
    }
}

namespace {
// The notice with `code` on a handle, or null.
const ProgramNotice* notice_with(const std::vector<ProgramNotice>& notices, std::string_view code) {
    for (const auto& notice : notices)
        if (notice.code == code) return &notice;
    return nullptr;
}

// A run paused by its node after the model call was journaled, in a directory of its own.
struct PausedWithJournaledCall {
    explicit PausedWithJournaledCall(const std::string& name) : files(name), peer(std::make_shared<Peer>()) {
        HostOptions options;
        options.monetary = 100'000;
        Host h(peer, options, &files);
        h.script().interrupt_after_call = true;
        result.emplace(h.run(Shape::Brokered, "identity-source"));
    }
    const ProgramResult& paused() const { return *result; }
    TempDir                    files;
    std::shared_ptr<Peer>      peer;
    std::optional<ProgramResult> result;
};

ProgramHandle fork_from(Host& h, const ProgramResult& paused, const std::string& run_id) {
    auto invocation   = h.invocation(run_id);
    invocation.budget = neograph::test::fork_share(paused.remaining_budget());
    return h.runtime().fork(kOwner,
                            ExactProgramCheckpointReference{paused.run_id(), paused.checkpoint()->checkpoint_id},
                            h.admit(Shape::Brokered), std::move(invocation),
                            resume_after(paused, "trace-" + run_id));
}
}  // namespace

TEST(ProgramProviderBindingJournal, AResumeUnderAnotherPolicyIdentityReplaysItsJournaledCall) {
    // #543: SchemaProvider's descriptor policy changed between the journal and the resume. The request is the
    // same, so the journaled call is replayed and not sent again, and the run says that the policy changed.
    PausedWithJournaledCall source("policy-resume");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    ASSERT_EQ(source.peer->transport.load(), 1);

    auto        other = std::make_shared<Peer>();
    HostOptions options;
    options.monetary    = 100'000;
    options.edit_policy = another_stop_reason("x_test_stop");
    Host h(other, options, &source.files);
    auto handle = h.runtime().resume(kOwner, "identity-source", resume_after(source.paused(), "trace-policy"));
    const auto done = handle.wait();
    ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
    EXPECT_EQ(other->transport.load(), 0) << "the journaled call is replayed under the new policy";
    EXPECT_TRUE(done.monetary_records().empty()) << "the first attempt charged it";
    const auto notices = handle.notices();
    const auto* notice = notice_with(notices, "P_PROVIDER_CALL_POLICY_CHANGED");
    ASSERT_NE(notice, nullptr);
    EXPECT_EQ(notice->witness["via"].get<std::string>(), "replay");
    const auto changes = h.journal().policy_changes(kOwner, notice->witness["call_id"].get<std::string>());
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes[0].attempt, 2U);
    EXPECT_NE(changes[0].journaled_policy_identity, changes[0].observed_policy_identity);
    EXPECT_EQ(journal_states(source.files.provider_db()), std::vector<std::string>{"succeeded"});
}

TEST(ProgramProviderBindingJournal, AResumeThatWouldSendDifferentTextIsAConflictUnderEveryPolicy) {
    // #543: only the policy is outside the call's identity. The question, and the system text that precedes it,
    // are the request: a resume that would send other text finds a conflict and sends nothing.
    PausedWithJournaledCall source("policy-text");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    struct Change {
        const char* name;
        std::function<void(Script&)> apply;
    };
    for (const auto& change : std::vector<Change>{
             {"question", [](Script& script) { script.prompt = "another question"; }},
             {"system text", [](Script& script) { script.system = "Answer in French."; }}}) {
        for (const bool policy_changed : {false, true}) {
            SCOPED_TRACE(std::string(change.name) + (policy_changed ? " and policy" : ""));
            TempDir copy("policy-text-copy");
            crash_image(source.files.path, copy.path);
            auto        other = std::make_shared<Peer>();
            HostOptions options;
            options.monetary = 100'000;
            if (policy_changed) options.edit_policy = another_stop_reason("x_test_stop");
            Host h(other, options, &copy);
            change.apply(h.script());
            const auto failed =
                h.runtime().resume(kOwner, "identity-source", resume_after(source.paused(), "trace-text")).wait();
            ASSERT_EQ(failed.status(), ProgramTerminalStatus::Failed) << failure_text(failed);
            EXPECT_NE(failure_text(failed).find("binding conflict"), std::string::npos) << failure_text(failed);
            EXPECT_EQ(other->transport.load(), 0);
            EXPECT_EQ(journal_states(copy.provider_db()), std::vector<std::string>{"succeeded"});
        }
    }
}

TEST(ProgramProviderBindingJournal, AForkUnderAnotherPolicyIdentityAdoptsTheSourcesCall) {
    // #543: before, the fork did not recognise the source's call under the new policy and sent it again, so the
    // call the source had paid for was paid for twice.
    PausedWithJournaledCall source("policy-fork");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    ASSERT_TRUE(source.paused().checkpoint());

    auto        other = std::make_shared<Peer>();
    HostOptions options;
    options.monetary    = 100'000;
    options.edit_policy = another_stop_reason("x_test_stop");
    Host h(other, options, &source.files);
    auto       handle = fork_from(h, source.paused(), "identity-fork");
    const auto forked = handle.wait();
    ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_text(forked);
    EXPECT_EQ(other->transport.load(), 0) << "the source's call is adopted, not sent again";
    EXPECT_EQ(source.peer->transport.load(), 1);
    EXPECT_TRUE(forked.monetary_records().empty()) << "and not charged again";
    EXPECT_EQ(forked.usage().monetary_microunits, 0U);
    EXPECT_EQ(journal_states(source.files.provider_db()), (std::vector<std::string>{"succeeded", "succeeded"}));
    const auto notices = handle.notices();
    EXPECT_EQ(notice_with(notices, "P_FORK_PROVIDER_CALLS_NOT_JOURNALED"), nullptr);
    EXPECT_EQ(notice_with(notices, "P_FORK_PROVIDER_CALL_RESENT"), nullptr);
    const auto* notice = notice_with(notices, "P_PROVIDER_CALL_POLICY_CHANGED");
    ASSERT_NE(notice, nullptr);
    EXPECT_EQ(notice->witness["via"].get<std::string>(), "adoption");
}

TEST(ProgramProviderBindingJournal, AJournalFromBeforeContentIdentityResumesAndForksWhileThePolicyIsUnchanged) {
    // #543: the journal of 0.16.0 holds only the dispatch digest, which includes the policy. Under the policy it
    // was written under, it resumes and a fork adopts its calls.
    PausedWithJournaledCall source("policy-0160");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    ASSERT_EQ(source.peer->digests.size(), 1U);
    as_written_before_content_identity(source.files.provider_db(), source.peer->digests.front());

    HostOptions options;
    options.monetary = 100'000;
    {
        TempDir copy("policy-0160-resume");
        crash_image(source.files.path, copy.path);
        auto other = std::make_shared<Peer>();
        Host h(other, options, &copy);
        auto handle = h.runtime().resume(kOwner, "identity-source", resume_after(source.paused(), "trace-0160"));
        const auto done = handle.wait();
        ASSERT_EQ(done.status(), ProgramTerminalStatus::Completed) << failure_text(done);
        EXPECT_EQ(other->transport.load(), 0);
        EXPECT_TRUE(handle.notices().empty());
    }
    {
        auto other = std::make_shared<Peer>();
        Host h(other, options, &source.files);
        auto handle = fork_from(h, source.paused(), "identity-fork");
        const auto forked = handle.wait();
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_text(forked);
        EXPECT_EQ(other->transport.load(), 0);
        EXPECT_TRUE(handle.notices().empty());
    }
}

TEST(ProgramProviderBindingJournal, AForkOfAJournalItCannotMatchIsRefusedUnlessTheHostAllowsIt) {
    // #543: a journal written by 0.15 names its calls by a digest that includes the policy of 0.15, which 0.16
    // cannot recompute. The fork cannot tell the source's call under the new policy from a different call, so it
    // does not send it: the provider would charge it again.
    PausedWithJournaledCall source("policy-0150");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    as_written_before_content_identity(source.files.provider_db(), digest('7'));

    {
        TempDir copy("policy-0150-refused");
        crash_image(source.files.path, copy.path);
        auto        other = std::make_shared<Peer>();
        HostOptions options;
        options.monetary = 100'000;
        Host h(other, options, &copy);
        auto handle = fork_from(h, source.paused(), "identity-fork");
        const auto forked = handle.wait();
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Failed) << failure_text(forked);
        EXPECT_NE(failure_text(forked).find("would send a provider call its source run already journaled"),
                  std::string::npos)
            << failure_text(forked);
        EXPECT_NE(failure_text(forked).find("fork_may_resend_provider_calls"), std::string::npos);
        ASSERT_TRUE(forked.failure());
        EXPECT_EQ(forked.failure()->code, "NG.ConfigError") << "a verdict, never retried";
        EXPECT_EQ(other->transport.load(), 0);
        EXPECT_EQ(journal_states(copy.provider_db()), std::vector<std::string>{"succeeded"});
    }
    {
        TempDir copy("policy-0150-allowed");
        crash_image(source.files.path, copy.path);
        auto        other = std::make_shared<Peer>();
        HostOptions options;
        options.monetary        = 100'000;
        options.fork_may_resend = true;
        Host h(other, options, &copy);
        auto handle = fork_from(h, source.paused(), "identity-fork");
        const auto forked = handle.wait();
        ASSERT_EQ(forked.status(), ProgramTerminalStatus::Completed) << failure_text(forked);
        EXPECT_EQ(other->transport.load(), 1) << "the host allowed the second payment";
        const auto notices = handle.notices();
        const auto* notice = notice_with(notices, "P_FORK_PROVIDER_CALL_RESENT");
        ASSERT_NE(notice, nullptr);
        EXPECT_EQ(notice->witness["reason"].get<std::string>(), "unverifiable");
        EXPECT_EQ(journal_states(copy.provider_db()), (std::vector<std::string>{"succeeded", "succeeded"}));
    }
}

TEST(ProgramProviderBindingJournal, AResumeOfAJournalItCannotMatchIsAConflict) {
    // #543: the same journal resumed by its own run: it fails closed, as 0.16.0 does, and sends nothing.
    PausedWithJournaledCall source("policy-0150-resume");
    ASSERT_EQ(source.paused().status(), ProgramTerminalStatus::Interrupted) << failure_text(source.paused());
    as_written_before_content_identity(source.files.provider_db(), digest('7'));
    auto        other = std::make_shared<Peer>();
    HostOptions options;
    options.monetary = 100'000;
    Host h(other, options, &source.files);
    const auto failed =
        h.runtime().resume(kOwner, "identity-source", resume_after(source.paused(), "trace-0150")).wait();
    ASSERT_EQ(failed.status(), ProgramTerminalStatus::Failed) << failure_text(failed);
    EXPECT_NE(failure_text(failed).find("binding conflict"), std::string::npos) << failure_text(failed);
    EXPECT_NE(failure_text(failed).find("predates content identity"), std::string::npos) << failure_text(failed);
    EXPECT_EQ(other->transport.load(), 0);
}

// ---------------------------------------------------------------------------
// #541: a chunked upload whose session URL only the live call holds
// ---------------------------------------------------------------------------

namespace {
using neograph::test::endpoints::ChunkedUploadPeer;
constexpr const char* kGoogleOrigin = "https://generativelanguage.googleapis.com";

// A host over a loopback peer, admitted for Google's resumable upload or, with `google` false, OpenAI's Uploads.
HostOptions chunked_options(const ChunkedUploadPeer& peer, bool google = true) {
    return endpoint_options(peer.origin(), {google ? kGoogleOrigin : "https://api.openai.com"});
}

// Bytes that tell their offset.
std::string pattern(std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i != size; ++i) bytes[i] = static_cast<char>('a' + (i * 7 + i / 5) % 26);
    return bytes;
}

// Every file under `directory` holds none of `needles`. Returns how many files it read.
std::size_t files_holding_none_of(const std::filesystem::path& directory, const std::vector<std::string>& needles) {
    std::size_t scanned = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream     in(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const auto& needle : needles)
            EXPECT_EQ(bytes.find(needle), std::string::npos) << needle << " in " << entry.path();
        ++scanned;
    }
    return scanned;
}

// What no persisted byte may hold after a chunked upload: the upload URLs the peer handed out and the parts they
// are made of, the API key, and the path of a file the upload read.
std::vector<std::string> secrets_of(const ChunkedUploadPeer& peer, std::string path = {}) {
    std::vector<std::string> needles = peer.urls();
    for (const char* part : {"SECRET-session", "upload_id", "upload_protocol", "fixture-key", "Bearer"})
        needles.emplace_back(part);
    if (!path.empty()) needles.push_back(std::move(path));
    return needles;
}
}  // namespace

TEST(ProgramProviderBindingJournal, NoUploadSessionUrlIsPersistedAnywhere) {
    // #541: a chunked upload's session URL lets anyone write the file without the API key. Only the live call
    // holds it: not the provider journal, the Program's records or the checkpoints.
    TempDir           files("upload-session-url");
    ChunkedUploadPeer peer;
    Host              h(std::make_shared<Peer>(), chunked_options(peer), &files);
    const auto        source = files.path / "source.bin";
    const auto        bytes  = pattern(100);
    write_file(source, bytes);
    h.script().chunk_size = 40;

    // From a file, in three parts, then from memory.
    h.script().upload_path = source.string();
    h.script().upload_size = bytes.size();
    const auto from_file   = h.run(Shape::ChunkedUpload, "upload-file");
    ASSERT_EQ(from_file.status(), ProgramTerminalStatus::Completed) << failure_text(from_file);
    h.script().upload_path.clear();
    h.script().upload_bytes = bytes;
    const auto from_memory  = h.run(Shape::ChunkedUpload, "upload-memory");
    ASSERT_EQ(from_memory.status(), ProgramTerminalStatus::Completed) << failure_text(from_memory);

    const auto held = peer.sessions();
    ASSERT_EQ(held.size(), 2U);
    EXPECT_EQ(held[0].bytes, bytes);
    EXPECT_EQ(held[1].bytes, bytes);
    EXPECT_NE(from_file.output().dump().find(held[0].file), std::string::npos) << from_file.output().dump();
    EXPECT_EQ(journal_states(files.provider_db()), std::vector<std::string>(8, "succeeded")) << "two uploads of four calls";

    const auto needles = secrets_of(peer, source.string());
    EXPECT_GE(files_holding_none_of(files.path, needles), 3U) << "the program, checkpoint and provider databases";
    for (const auto* result : {&from_file, &from_memory}) {
        const auto text = result->output().dump();
        for (const auto& needle : needles) EXPECT_EQ(text.find(needle), std::string::npos) << needle;
    }
    h.close();
    EXPECT_GE(files_holding_none_of(files.path, needles), 3U) << "and after the write-ahead logs are folded in";
}

TEST(ProgramProviderBindingJournal, NoUploadSessionUrlIsPersistedWhenAPartFails) {
    // #541: a failed part is journaled with its error and the evidence the SDK kept. Neither names the session.
    TempDir           files("upload-session-failed");
    ChunkedUploadPeer peer;
    peer.fail_chunks = 1;
    Host h(std::make_shared<Peer>(), chunked_options(peer), &files);
    h.script().upload_bytes = pattern(100);
    h.script().chunk_size   = 40;
    const auto failed       = h.run(Shape::ChunkedUpload, "upload-failed");
    ASSERT_EQ(failed.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(peer.chunks(), 0);
    EXPECT_EQ(journal_states(files.provider_db()).size(), 2U) << "the session's opening and the part that failed";

    const auto needles = secrets_of(peer);
    const auto text    = failure_text(failed);
    for (const auto& needle : needles) EXPECT_EQ(text.find(needle), std::string::npos) << needle << " in " << text;
    EXPECT_GE(files_holding_none_of(files.path, needles), 3U);
    h.close();
    EXPECT_GE(files_holding_none_of(files.path, needles), 3U);
}

TEST(ProgramProviderBindingJournal, AnUploadInOneRequestPersistsNeitherItsPathNorACredential) {
    // #541: an upload from a file or from memory, sent in one request, leaves no path and no key in a record.
    TempDir                              files("upload-persisted");
    neograph::test::endpoints::UploadPeer peer;
    Host h(std::make_shared<Peer>(), endpoint_options(peer.origin()), &files);
    const auto source = files.path / "source.bin";
    write_file(source, "the file's contents");
    h.script().upload_path = source.string();
    h.script().upload_size = 19;
    const auto from_file   = h.run(Shape::FileUpload, "upload-one-file");
    ASSERT_EQ(from_file.status(), ProgramTerminalStatus::Completed) << failure_text(from_file);
    h.script().upload_bytes = "bytes in memory";
    const auto from_memory  = h.run(Shape::FileUpload, "upload-one-memory");
    ASSERT_EQ(from_memory.status(), ProgramTerminalStatus::Completed) << failure_text(from_memory);
    ASSERT_EQ(peer.received().size(), 2U);
    h.close();
    EXPECT_GE(files_holding_none_of(files.path, {source.string(), "fixture-key", "Bearer"}), 3U);
}
