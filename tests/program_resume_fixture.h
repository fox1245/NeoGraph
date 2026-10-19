// Fixtures of the Program-level resume/replay harness
// (tests/test_program_resume_model.cpp) and of the regressions it found: a
// host wired as LocalProgramHost wires one (in-memory stores with the
// in-process priced broker, or every store in one SQLite file with the SQLite
// provider journal), a bound provider whose local transport counts the
// requests it sends, a one-node Core whose node may call the provider and may
// ask, and crash images: the durable file copied at a chosen checkpoint save,
// put_writes, Program transition, provider dispatch or journaled call, which a
// new process then recovers.
#pragma once

#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/program.h>
#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>

#include "fixtures/coarse_clock_store.h"
#include "fixtures/fork_share.h"
#include "fixtures/typed_provider.h"
#include "monetary.h"
#include "provider_binding.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace neograph;
using namespace neograph::program;

constexpr std::string_view kOpenRouter = "https://openrouter.ai";
constexpr std::string_view kModel      = "deepseek/deepseek-v4-flash-0731";
constexpr std::uint64_t    kInputLimit = 1'000;
constexpr std::uint64_t    kOutputCap  = 64;
const std::string          kOwner      = "tenant:resume-model";
// What one call reports: 21 input and 3 output tokens, 1,234,567 nano-USD.
constexpr std::uint64_t kCallTokens = 24;
constexpr std::uint64_t kCallMoney  = 1235;

std::string digest(char c) { return "sha256:" + std::string(64, c); }

// ---------------------------------------------------------------------------
// Files and crash images
// ---------------------------------------------------------------------------

struct TempDir {
    explicit TempDir(const std::string& name) {
        static std::atomic<unsigned> sequence{0};
        // tmpfs when there is one: a schedule copies and reopens its databases often.
        const std::filesystem::path shm("/dev/shm");
        std::error_code             ignored;
        const auto base = std::filesystem::is_directory(shm, ignored) ? shm : std::filesystem::temp_directory_path();
        path = std::filesystem::canonical(base) /
               ("neograph-program-resume-model-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence++));
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
    Connection(const std::string& file, int flags) {
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

// The committed state of the live databases at this instant: what a restart
// after the process died here would find.
void crash_image(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to);
    for (const auto& entry : std::filesystem::directory_iterator(from)) {
        if (entry.path().extension() != ".sqlite") continue;
        Connection source(entry.path().string(), SQLITE_OPEN_READONLY);
        Connection target((to / entry.path().filename()).string(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        auto* backup = sqlite3_backup_init(target.db, "main", source.db, "main");
        if (!backup) throw std::runtime_error("sqlite backup init failed");
        const auto step   = sqlite3_backup_step(backup, -1);
        const auto finish = sqlite3_backup_finish(backup);
        if (step != SQLITE_DONE || finish != SQLITE_OK) throw std::runtime_error("sqlite backup failed");
    }
}

// ---------------------------------------------------------------------------
// The provider: a real SDK client over a local transport that counts requests
// ---------------------------------------------------------------------------

std::shared_ptr<sp::runtime::Client> client_for(std::string origin, const std::string& model,
                                                std::uint64_t input_limit) {
    // The prices and OpenRouter route ceiling a host's catalogue layer gives the model.
    neograph::test::FixtureModel priced{"openai.chat", model, input_limit};
    priced.pricing = json{{"input", "0.000000018"}, {"cache_read", "0.000000018"}, {"output", "0.00000064"},
                          {"ceiling_input", "0.00000044"}, {"ceiling_output", "0.00000132"}};
    return std::make_shared<sp::runtime::Client>(neograph::test::descriptor(
        "openai.chat", std::move(origin),
        neograph::test::policy_with_models(json::parse(sp::config_defaults::descriptor_policy_json), {priced})));
}

sp::Usage priced_usage() {
    auto usage                    = neograph::test::usage(21, 3, 24);
    usage.provider_cost.source    = sp::CostSource::OpenRouterUsd;
    usage.provider_cost.total     = sp::UsdAmount{1'234'567};
    usage.provider_cost.status[0] = sp::CostStatus::Available;
    return usage;
}

/// The transport: every request it sends, by the tag of the call that sent it.
struct Peer {
    explicit Peer(const std::atomic<bool>* process_alive = nullptr, std::function<void()> on_send = {})
        : alive(process_alive), sending(std::move(on_send)) {}
    /// Runs while a request is out: the journal holds its dispatch marker and
    /// no outcome yet (a crash point).
    std::function<void()>         sending;
    /// The process this transport serves: false once a crash copied the
    /// durable state; a request sent after that never happened.
    const std::atomic<bool>*      alive;
    std::mutex                    mutex;
    std::map<std::string, int>    sent;  // call tag -> requests
    int                           total = 0;
    std::function<void(const std::string&)> after_send;  // runs once the reply is ready

    void reset() {
        std::lock_guard lock(mutex);
        sent.clear();
        total = 0;
        after_send = nullptr;
    }
};

std::shared_ptr<Provider> make_provider(std::shared_ptr<Peer> peer) {
    return std::make_shared<neograph::test::LocalProvider>(
        [peer](ProviderRequest request, const PreparedProviderRequest&, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            const auto& chat = std::get<sp::chat::Request>(request.payload);
            std::string tag;
            for (const auto& message : chat.messages) tag = message.text;
            for (const auto& message : chat.canonical_messages)
                for (const auto& part : message.parts)
                    if (const auto* text = std::get_if<sp::Text>(&part)) tag = text->value;
            if (!peer->alive || *peer->alive) {
                {
                    std::lock_guard lock(peer->mutex);
                    ++peer->sent[tag];
                    ++peer->total;
                }
                if (peer->sending) peer->sending();
            }
            co_return neograph::test::success("reply-" + tag, priced_usage());
        },
        "upstream-descriptor", client_for(std::string(kOpenRouter), std::string(kModel), kInputLimit));
}

// ---------------------------------------------------------------------------
// Durable operations and crash images
// ---------------------------------------------------------------------------

/// Counts the durable operations of a schedule (checkpoint saves and
/// put_writes, Program transitions, and provider calls the node got back) and
/// copies the database at the armed one, before or after it applies.
struct CrashPoints {
    /// False once the durable state was copied for a crash: what the original
    /// process does after that never happened, so the transport and the node
    /// journal stop counting it. A recovered process sets it again.
    std::atomic<bool>     alive{true};
    std::mutex            mutex;
    int                   ops = 0;
    int                   armed = -1;
    bool                  after = false;
    bool                  taken = false;
    std::filesystem::path live, image;

    /// Arms by a predicate over the operations seen so far instead of an index.
    std::function<bool(const std::vector<std::string>& seen)> armed_when;
    std::vector<std::string>                                  seen;
    /// For each operation seen that saved a checkpoint, its Core thread, and
    /// "first" when it is the first checkpoint of a Core run (no parent);
    /// for each publication that appends to the JavaScript command journal,
    /// "pending" or "failed" or "result"; empty otherwise.
    std::vector<std::string> threads, firsts;
    std::string                                               taken_at;  // the operation the copy was taken at

    void reset(std::filesystem::path from, std::filesystem::path to, int at, bool after_op) {
        std::lock_guard lock(mutex);
        ops   = 0;
        armed = at;
        after = after_op;
        taken = false;
        live  = std::move(from);
        image = std::move(to);
        armed_when = nullptr;
        seen.clear();
        threads.clear();
        firsts.clear();
        taken_at.clear();
    }
    template <class Fn>
    auto durable(const char* what, Fn&& apply, std::string thread = {}, std::string note = {}) {
        std::unique_lock lock(mutex);
        const int  op    = ops++;
        if (std::getenv("NEOGRAPH_PROGRAM_RESUME_MODEL_DEBUG") && alive)
            std::cout << "    [op " << op << "] " << what << "\n";
        if (alive) {
            seen.emplace_back(what);
            threads.push_back(std::move(thread));
            firsts.push_back(std::move(note));
        }
        const bool here  = !taken && (armed_when ? armed_when(seen) : op == armed);
        if (here) taken_at = what;
        if (here && !after) {
            crash_image(live, image);
            taken = true;
            alive = false;
        }
        lock.unlock();
        if constexpr (std::is_void_v<decltype(apply())>) {
            apply();
            if (here && after) take();
        } else {
            auto result = apply();
            if (here && after) take();
            return result;
        }
    }
    void take() {
        std::lock_guard lock(mutex);
        crash_image(live, image);
        taken = true;
        alive = false;
    }
};


// ---------------------------------------------------------------------------
// The Core node
// ---------------------------------------------------------------------------

/// Every body run of the step node, by behaviour, for P1 and P2.
struct StepJournal {
    std::mutex mutex;
    struct Run {
        std::string thread, task, value, answer;  // answer: the decision it consumed
        bool        asked = false, finished = false;
    };
    std::vector<Run> runs;
    void reset() {
        std::lock_guard lock(mutex);
        runs.clear();
    }
    const std::atomic<bool>* alive = nullptr;
    void record(Run run) {
        if (alive && !*alive) return;
        std::lock_guard lock(mutex);
        runs.push_back(std::move(run));
    }
};

/// One schedule's world: its crash points, its process flag and the journal
/// of its node bodies. Schedules run side by side; a node finds its world by
/// the Program run id it runs for ("<world>.run", "<world>.run-fork").
struct World {
    explicit World(std::string name) : id(std::move(name)) {
        journal.alive = &crash.alive;
        std::lock_guard lock(registry_mutex());
        registry()[id] = this;
    }
    ~World() {
        std::lock_guard lock(registry_mutex());
        registry().erase(id);
    }
    World(const World&)            = delete;
    World& operator=(const World&) = delete;
    std::string run_id(const std::string& suffix = "run") const { return id + "." + suffix; }

    std::string  id;
    CrashPoints  crash;
    StepJournal  journal;

    static std::mutex& registry_mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static std::map<std::string, World*>& registry() {
        static std::map<std::string, World*> worlds;
        return worlds;
    }
};

World& world_of(std::string_view run_id) {
    const auto      key = std::string(run_id.substr(0, run_id.find('.')));
    std::lock_guard lock(World::registry_mutex());
    const auto      found = World::registry().find(key);
    if (found == World::registry().end()) throw std::logic_error("no resume model world for run " + key);
    return *found->second;
}

std::string next_world_id() {
    static std::atomic<std::uint64_t> next{0};
    return "w" + std::to_string(++next);
}

/// The node of the Core every call runs. Its "value" channel (the call's
/// input) lists what the calls of a plan sequence do, "<step>;<step>;...",
/// and its "k" channel says which of them this call is: a call writes its
/// result to "out" and k + 1 to "k", which a plan passes on to the next call.
/// A JavaScript callCore gives one step; a plan call whose input sets none
/// (a plan passes the previous call's result) takes "ask:z". A step is
/// "<kind>:<tag>":
///  - ok:      writes "done:<tag>";
///  - ask:     asks until answered, then writes the answer's decision;
///  - llm:     calls the provider through the run's broker, writes the reply;
///  - llm-ask: calls the provider, then asks until answered, and writes the
///             reply and the decision;
///  - fail:    throws.
class StepNode final : public graph::GraphNode {
public:
    StepNode(std::string name, std::shared_ptr<Provider> provider, bool side)
        : name_(std::move(name)), provider_(std::move(provider)), side_(side) {}
    std::string get_name() const override { return name_; }

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        // The side node runs beside "work" in the Core's one step: it takes
        // its step from "side" and does nothing when there is none.
        if (side_ && in.state.get("side").get<std::string>().empty()) co_return graph::NodeOutput{};
        // A plan passes a call's whole result as the next call's input, which
        // sets no channel: the next call of a plan sequence asks.
        auto steps = in.state.get(side_ ? "side" : "value").get<std::string>();
        if (steps.empty()) steps = "ask:z";
        const auto index = side_ ? std::size_t{0} : in.state.get("k").get<std::size_t>();
        std::string value;
        std::size_t begin = 0;
        for (std::size_t position = 0; position <= index; ++position) {
            if (begin > steps.size()) {
                value.clear();
                break;
            }
            const auto end = steps.find(';', begin);
            value          = steps.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            begin          = end == std::string::npos ? steps.size() + 1 : end + 1;
        }
        if (value.empty()) throw std::logic_error("no step " + std::to_string(index) + " in " + steps);
        const auto colon = value.find(':');
        const auto kind  = value.substr(0, colon);
        const auto tag   = colon == std::string::npos ? std::string() : value.substr(colon + 1);
        auto&            world = world_of(in.ctx.run_id);
        StepJournal::Run run{in.ctx.thread_id, graph::make_tool_execution_context(in.ctx).effect_task_id, value, {}};
        graph::NodeOutput output;
        std::string       written = "done:" + tag;
        if (kind == "fail") {
            world.journal.record(run);
            throw std::runtime_error("step failure " + tag);
        }
        if (kind == "llm" || kind == "llm-ask") {
            auto request = make_provider_request(*provider_, std::string(kModel),
                                                 {neograph::test::message(tag, sp::Role::User)});
            std::get<sp::chat::Request>(request.payload).max_output_tokens = kOutputCap;
            const auto broker = graph::provider_call_broker(in.ctx);
            if (!broker) throw std::logic_error("the Program provider broker is absent");
            auto result = co_await graph::observe_provider_result(
                in.ctx, broker->invoke(graph::make_provider_call_identity(in.ctx, name_), provider_,
                                       std::move(request)));
            graph::record_usage(in.ctx, result);
            written = neograph::test::text(outcome_or_throw(result));
            // The call is journaled: a durable point too.
            world.crash.durable("provider call journaled", [] {});
        }
        if (kind == "ask" || kind == "llm-ask") {
            const auto& answer = in.ctx.resume_value;
            if (!answer || !answer->is_object() || !answer->contains("decision")) {
                run.asked = true;
                world.journal.record(run);
                throw graph::NodeInterrupt("approve " + tag, json{{"ask", tag}});
            }
            run.answer = (*answer)["decision"].get<std::string>();
            written    = (kind == "ask" ? "" : written + "/") + run.answer;
        } else if (kind != "ok" && kind != "llm") {
            throw std::logic_error("unknown step " + value);
        }
        run.finished = true;
        world.journal.record(std::move(run));
        output.writes.push_back(graph::ChannelWrite{side_ ? "side_out" : "out", json(written)});
        if (!side_) output.writes.push_back(graph::ChannelWrite{"k", json(index + 1)});
        co_return output;
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> provider_;
    bool                      side_ = false;
};

class CountingCheckpointStore final : public graph::CheckpointStore {
public:
    CountingCheckpointStore(std::shared_ptr<graph::CheckpointStore> inner, CrashPoints& crash)
        : inner_(std::move(inner)), crash_(crash) {}
    void save(const graph::Checkpoint& checkpoint) override {
        crash_.durable(to_string(checkpoint.interrupt_phase), [&] { inner_->save(checkpoint); },
                       checkpoint.thread_id, checkpoint.parent_id.empty() ? "first" : "");
    }
    void put_writes(const std::string& thread, const std::string& parent, const graph::PendingWrite& write) override {
        crash_.durable("put_writes", [&] { inner_->put_writes(thread, parent, write); });
    }
    std::optional<graph::Checkpoint> load_latest(const std::string& thread) override { return inner_->load_latest(thread); }
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override { return inner_->load_by_id(id); }
    std::vector<graph::Checkpoint> list(const std::string& thread, int limit) override { return inner_->list(thread, limit); }
    void delete_thread(const std::string& thread) override { inner_->delete_thread(thread); }
    std::vector<graph::PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override { inner_->clear_writes(thread, parent); }
    bool requires_managed_budget(const std::string& thread) override { return inner_->requires_managed_budget(thread); }

private:
    std::shared_ptr<graph::CheckpointStore> inner_;
    CrashPoints&                            crash_;
};

/// Delegates every operation; counts the publications.
class CountingTransitionStore final : public ProgramTransitionStore {
public:
    CountingTransitionStore(std::shared_ptr<ProgramTransitionStore> inner, CrashPoints& crash)
        : inner_(std::move(inner)), crash_(crash) {}
    std::string process_coordination_key() const override { return inner_->process_coordination_key(); }
    std::optional<ProgramRunRecord> load(std::string_view owner, std::string_view run) const override {
        return inner_->load(owner, run);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner, std::string_view run) const override {
        return inner_->latest(owner, run);
    }
    std::optional<ProgramRunPublicationHead> load_run_publication_head(std::string_view owner,
                                                                       std::string_view run) const override {
        return inner_->load_run_publication_head(owner, run);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner, std::string_view run,
                                          std::uint64_t after) const override {
        return inner_->load_events(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner, std::string_view run,
                                                       std::uint64_t after) const override {
        return inner_->load_effects(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(std::string_view owner,
                                                                               std::string_view run,
                                                                               std::uint64_t after) const override {
        return inner_->load_javascript_commands(owner, run, after);
    }
    std::optional<ProgramCommandPublicationHead> load_command_publication_head(std::string_view owner,
                                                                               std::string_view run) const override {
        return inner_->load_command_publication_head(owner, run);
    }
    std::vector<ProgramContextPublication> load_context_publications(std::string_view owner, std::string_view run,
                                                                     std::uint64_t after) const override {
        return inner_->load_context_publications(owner, run, after);
    }
    std::vector<HookOutboxEntry> load_hook_outbox_entries(std::string_view owner, std::string_view run) const override {
        return inner_->load_hook_outbox_entries(owner, run);
    }
    ProgramEffectiveRuntimeState load_effective_runtime_state(std::string_view owner,
                                                              std::string_view run) const override {
        return inner_->load_effective_runtime_state(owner, run);
    }
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses(std::string_view owner,
                                                                  std::string_view run) const override {
        return inner_->load_child_syntheses(owner, run);
    }
    std::optional<MigrationPlan> load_migration_plan(std::string_view owner, std::string_view run) const override {
        return inner_->load_migration_plan(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner, std::string_view lineage) const override {
        return inner_->load_lineage(owner, lineage);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner, std::string_view run) const override {
        return inner_->load_run_lineage(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage_head(std::string_view owner, std::string_view lineage,
                                                       std::string_view head) const override {
        return inner_->load_lineage_head(owner, lineage, head);
    }
    std::optional<ProgramRunGeneration> load_generation(std::string_view owner, std::string_view lineage,
                                                        std::uint64_t generation) const override {
        return inner_->load_generation(owner, lineage, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner, std::string_view lineage, std::uint64_t generation) const override {
        return inner_->load_generation_initial_publication(owner, lineage, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(std::string_view owner, std::string_view source,
                                                                      std::string_view head) const override {
        return inner_->load_graph_migration_capsule(owner, source, head);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(std::string_view owner,
                                                              std::string_view run) const override {
        return inner_->load_execution_lease(owner, run);
    }
    ProgramTransitionPublishResult compare_publish(std::string_view owner, std::string_view head,
                                                   ProgramTransitionPublication publication) override {
        const auto what = std::string("publish ") + std::string(to_string(publication.run_record.continuation().state));
        std::string note;
        for (const auto& entry : publication.commands) {
            if (entry.pending()) {
                if (note.empty()) note = "pending";
            } else if (const auto result = entry.terminal_result();
                       result && result->value("status", std::string()) != "completed") {
                note = "failed";
            } else if (note != "failed") {
                note = "result";
            }
        }
        return crash_.durable(
            what.c_str(), [&] { return inner_->compare_publish(owner, head, std::move(publication)); }, {},
            std::move(note));
    }
    ProgramTransitionPublishResult compare_publish_execution(std::string_view owner, std::string_view head,
                                                             ProgramTransitionPublication publication,
                                                             std::optional<ProgramExecutionLease> expected,
                                                             std::optional<ProgramExecutionLease> next) override {
        return crash_.durable("publish execution", [&] {
            return inner_->compare_publish_execution(owner, head, std::move(publication), std::move(expected),
                                                     std::move(next));
        });
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner, std::string_view head, ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence, const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& lease) override {
        return crash_.durable("publish safe point", [&] {
            return inner_->compare_publish_graph_safe_point(owner, head, std::move(publication), evidence, capsule,
                                                            lease);
        });
    }

private:
    std::shared_ptr<ProgramTransitionStore> inner_;
    CrashPoints&                            crash_;
};

// ---------------------------------------------------------------------------
// One host: registry, binder, catalog, runtime
// ---------------------------------------------------------------------------

ExecutableManifest manifest(ExecutableIdentity identity, std::vector<ExecutableIdentity> requires_ = {}) {
    return ExecutableManifest{std::move(identity), EffectMode::Brokered, "resume-model", {}, {}, std::move(requires_)};
}

const ExecutableIdentity kProviderIdentity{ExecutableKind::Provider, "resume-model-provider", "1.0.0", digest('1')};

RegistrySnapshot model_registry() {
    RegistrySnapshotBuilder builder;
    builder.add_provider(manifest(kProviderIdentity), ProviderMetadata{json::object(), json::object()});
    builder.add_host_brokered_node(
        manifest({ExecutableKind::Node, "rm.step", "1.0.0", digest('2')}, {kProviderIdentity}),
        [](const std::string& name, const json& config, const graph::NodeContext& context) {
            return std::make_unique<StepNode>(name, context.provider, config.value("side", false));
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"out", "k", "side_out"})}, {"exports", json::array({"out", "k", "side_out"})}});
    builder.add_reducer(manifest({ExecutableKind::Reducer, "overwrite", "1.0.0", digest('3')}),
                        [](const json&, const json& incoming) { return incoming; });
    // The channel a failure route records its failure on must append (#364).
    builder.add_reducer(manifest({ExecutableKind::Reducer, "append", "1.0.0", digest('4')}),
                        [](const json& current, const json& incoming) {
                            auto merged = current.is_array() ? current : json::array();
                            if (incoming.is_array())
                                for (const auto& item : incoming) merged.push_back(item);
                            else
                                merged.push_back(incoming);
                            return merged;
                        });
    return std::move(builder).build();
}

/// A host as LocalProgramHost wires one: in memory, or (durable) every store in
/// one SQLite file so that a crash image is one consistent copy.
class Host {
public:
    Host(World& world, std::shared_ptr<Peer> peer, std::optional<std::filesystem::path> durable)
        : peer_(std::move(peer)), durable_(std::move(durable)), registry_(model_registry()) {
        AdmissionProfileBuilder profile;
        profile.id("resume-model-profile").semantic_version("1.0.0").registry(registry_)
            .mode(AdmissionMode::MultiTenant).max_program_schema_version(PROGRAM_SCHEMA_VERSION_V4)
            .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
            .allow_source_kind(SourceKind::CppBuilder).allow_source_kind(SourceKind::JavaScript)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry_.identities()) profile.allow_executable(identity);
        profile_.emplace(std::move(profile).build());

        std::shared_ptr<ProgramStore>           store;
        std::shared_ptr<ProgramTransitionStore> transitions;
        std::shared_ptr<graph::CheckpointStore> checkpoints;
        if (durable_) {
            const auto file = (*durable_ / "durable.sqlite").string();
            store           = std::make_shared<SQLiteProgramStore>(file);
            transitions = std::make_shared<CountingTransitionStore>(std::make_shared<SQLiteProgramTransitionStore>(file),
                                                                    world.crash);
            // Every checkpoint carries one timestamp, as on a host whose saves
            // return inside the millisecond.
            checkpoints = std::make_shared<CountingCheckpointStore>(
                std::make_shared<graph::test_support::CoarseClockStore<graph::SqliteCheckpointStore>>(file),
                world.crash);
            journal_        = std::make_shared<SQLiteProgramProviderCallJournal>(file);
        } else {
            store       = std::make_shared<InMemoryProgramStore>();
            transitions = std::make_shared<InMemoryProgramTransitionStore>();
            checkpoints = std::make_shared<graph::InMemoryCheckpointStore>();
        }
        graph::NodeContext context;
        context.provider                             = make_provider(peer_);
        context.model                                = std::string(kModel);
        context.provider_controls.max_output_tokens = kOutputCap;
        CatalogConfig catalog{store, registry_, std::make_shared<EngineGenerationCache>(), "resume-model/v1"};
        catalog.worker_count      = 2;
        catalog.capability_binder = program::detail::single_provider_binder(context, "resume-model");
        catalog_                  = std::make_shared<ProgramCatalog>(std::move(catalog));

        RuntimeConfig runtime;
        runtime.catalog                           = catalog_;
        runtime.checkpoints                       = checkpoints;
        runtime.transitions                       = transitions;
        runtime.scheduler_threads                 = 2;
        runtime.require_core_provider_call_broker = true;
        runtime.core_provider_calls_journaled     = static_cast<bool>(journal_);
        if (journal_) {
            runtime.core_provider_call_resolver =
                [journal = journal_, deployment = program::detail::provider_deployment_identity("resume-model")](
                    const ProgramCoreProviderCallContext& call) -> std::optional<ProgramCoreProviderCallBinding> {
                return journal->bind_priced(call, deployment);
            };
        } else {
            runtime.core_provider_call_resolver =
                [](const ProgramCoreProviderCallContext& call) -> std::optional<ProgramCoreProviderCallBinding> {
                return bind_priced_provider_call(call);
            };
        }
        runtime_ = std::make_unique<ProgramRuntime>(std::move(runtime));
    }
    ~Host() {
        runtime_.reset();
        catalog_.reset();
    }

    /// Compiles once per program: a bundle admits into any host's catalog.
    ProgramBundle compile(const ProgramSource& source) const {
        ProgramCompiler compiler(registry_, {"resume-model/v1"});
        return source.kind() == SourceKind::JavaScript ? compiler.compile(source, budget()) : compiler.compile(source);
    }
    ProgramVersion admit(ProgramSource source) { return admit(compile(source)); }
    ProgramVersion admit(const ProgramBundle& bundle) {
        PolicySnapshotBuilder policy;
        policy.id("resume-model-policy").semantic_version("1.0.0").owner_scope(kOwner)
            .admission_profile(*profile_).minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
            .budget_ceiling(BudgetLimits{100000, 10000000, 1000000000, 4, 64, 100, 4, 4, 32});
        try {
            return catalog_->admit(bundle, ProgramAdmission{kOwner, *profile_, std::move(policy).build(), {}});
        } catch (const ProgramAdmissionError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics())
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " + diagnostic.message +
                           " " + diagnostic.witness.dump();
            throw std::runtime_error(message);
        }
    }

    static RunBudget budget(std::optional<std::uint64_t> node_retries = std::nullopt) {
        RunBudget granted{60000, 1'000'000, 1'000'000, 4, 32, 64, 0, 1, 1};
        granted.max_node_retries = node_retries;
        return granted;
    }

    ProgramResult run(const ProgramVersion& version, const std::string& run_id, json input,
                      std::optional<std::uint64_t> node_retries = std::nullopt) {
        ProgramInvocation invocation{std::move(input), budget(node_retries), "trace-" + run_id, {}};
        invocation.requested_run_id = run_id;
        return runtime_->run(kOwner, version, std::move(invocation));
    }
    ProgramResult resume(const std::string& run_id, json value, std::string pending_id) {
        return runtime_->resume(kOwner, run_id, ProgramResume{std::move(value), "trace-resume", {}, std::move(pending_id)})
            .wait();
    }
    ProgramResult reconnect(const std::string& run_id) { return runtime_->reconnect(kOwner, run_id).wait(); }
    ProgramResult reconcile(const std::string& run_id, std::string pending_id, ProgramEffectReconciliation resolution,
                            std::optional<json> result) {
        return runtime_
            ->reconcile(kOwner, run_id,
                        ProgramEffectResolution{std::move(pending_id), resolution, std::move(result), "trace-reconcile",
                                                {}})
            .wait();
    }
    ProgramHandle start_fork(const ProgramVersion& version, const ProgramResult& paused, const std::string& target,
                             json value, std::string pending_id) {
        // The scenarios compare what the fork is charged with what the source was, so
        // the fork keeps all the model budget; it takes a share of the rest (#414).
        auto share                 = neograph::test::fork_share(paused.remaining_budget());
        share.model_tokens         = paused.remaining_budget().model_tokens;
        share.monetary_microunits  = paused.remaining_budget().monetary_microunits;
        ProgramInvocation invocation{json::object(), share, "trace-fork", {}};
        invocation.requested_run_id = target;
        return runtime_->fork(kOwner, ExactProgramCheckpointReference{paused.run_id(), paused.checkpoint()->checkpoint_id},
                              version, std::move(invocation),
                              ProgramResume{std::move(value), "trace-fork-resume", {}, std::move(pending_id)});
    }
    ProgramResult fork(const ProgramVersion& version, const ProgramResult& paused, const std::string& target,
                       json value, std::string pending_id) {
        return start_fork(version, paused, target, std::move(value), std::move(pending_id)).wait();
    }

private:
    std::shared_ptr<Peer>                             peer_;
    std::optional<std::filesystem::path>              durable_;
    RegistrySnapshot                                  registry_;
    std::optional<AdmissionProfile>                   profile_;
    std::shared_ptr<SQLiteProgramProviderCallJournal> journal_;
    std::shared_ptr<ProgramCatalog>                   catalog_;
    std::unique_ptr<ProgramRuntime>                   runtime_;
};

}  // namespace

namespace {

/// The Core of a typed plan: the "work" node over the channels a
/// JavaScript program declares.
json core_definition() {
    return json{{"schema_version", 1},
                {"name", "main"},
                {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"k", json{{"reducer", "overwrite"}, {"initial", 0}}},
                                  {"out", json{{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"side", json{{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"side_out", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
                {"nodes", json{{"work", json{{"type", "rm.step"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                       json{{"from", "work"}, {"to", "__end__"}}})},
                {"conditional_edges", json::array()}};
}

/// A typed plan document whose root is `root` (call_core operations need no
/// fields), over core_definition(), within Host::budget().
json plan_program(json root) {
    root["name"]       = "main";
    root["definition"] = core_definition();
    const auto bound   = [](const char* resource, std::uint64_t minimum, std::uint64_t maximum) {
        return json{{"resource", resource}, {"minimum", minimum}, {"maximum", maximum}};
    };
    return json{{"program_schema_version", 2U},
                {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"root", std::move(root)},
                {"declared_budget_requirements",
                 json::array({bound("wall_time_ms", 1, 100000), bound("model_tokens", 0, 10000000),
                              bound("monetary_microunits", 0, 1000000000), bound("max_concurrency", 1, 4),
                              bound("max_program_operations", 1, 32), bound("max_core_steps", 1, 64),
                              bound("max_dynamic_compiles", 0, 0), bound("max_child_depth", 0, 1),
                              bound("max_total_children", 0, 1)})}};
}

std::string javascript_program(const std::string& body) {
    return "export function define() {\n"
           "  const graph = ng.graph(\"main\");\n"
           "  graph.channel(\"value\", {reducer: \"overwrite\", initial: \"\"});\n"
           "  graph.channel(\"k\", {reducer: \"overwrite\", initial: 0});\n"
           "  graph.channel(\"out\", {reducer: \"overwrite\", initial: \"\"});\n"
           "  graph.channel(\"side\", {reducer: \"overwrite\", initial: \"\"});\n"
           "  graph.channel(\"side_out\", {reducer: \"overwrite\", initial: \"\"});\n"
           "  graph.node(\"work\", {type: \"rm.step\"});\n"
           "  graph.node(\"side\", {type: \"rm.step\", side: true});\n"
           "  graph.entry(\"work\");\n"
           "  graph.entry(\"side\");\n"
           "  graph.exit(\"work\");\n"
           "  graph.exit(\"side\");\n"
           "  return graph;\n"
           "}\n\n"
           "export function* main(input) {\n" + body + "\n}\n";
}

std::string describe(const ProgramResult& result) {
    std::ostringstream out;
    out << "status " << to_string(result.status());
    if (const auto interrupt = result.interrupt(); interrupt && interrupt->pending_input)
        out << " pending op " << interrupt->pending_input->operation_id() << " call "
            << interrupt->pending_input->call_id() << " core " << interrupt->pending_input->core_interrupt_id()
            << " node " << interrupt->core_node;
    if (const auto interrupt = result.interrupt(); interrupt && !interrupt->pending_input)
        out << " interrupt node " << interrupt->core_node << " value " << interrupt->value.dump()
            << (interrupt->pending_effect ? " pending effect " + interrupt->pending_effect->operation_id() : "");
    if (const auto failure = result.failure())
        out << " failure " << failure->code << ": " << failure->message << " " << failure->witness.dump();
    out << " output " << result.output().dump() << " usage tokens " << result.usage().model_tokens << " money "
        << result.usage().monetary_microunits << " remaining tokens " << result.remaining_budget().model_tokens
        << " money " << result.remaining_budget().monetary_microunits;
    return out.str();
}

}  // namespace
