// The adapters from a JavaScript stores object to the five awaitable Program store interfaces (js_stores.h).
//
// A store call is a method call on the host's object, made through functions that wasm/stores.mjs installs on the module
// (installStoreBridge). The call answers in one of three ways:
//   a value     the method returned something that is not a Promise: the awaiting coroutine goes on at once, without
//               suspending, so a Blocking...Store view over the adapter waits for nothing;
//   a failure   the method threw: a C++ exception is thrown from the awaiting call, nothing is defaulted;
//   a Promise   the coroutine waits on an Asio timer that never expires and is cancelled when the Promise settles.
//               The bridge reports the outcome through ng_store_resolve / ng_store_reject, which post a task that
//               cancels the timer. Everything the coroutine owns lives in its frame, so a coroutine that is destroyed
//               while it waits (its loop closed, its run cancelled) takes the wait with it, and an outcome that arrives
//               afterwards finds nothing and is dropped.
#include "js_stores.h"

#include "js_store_codec.h"

// Step one of the state machine that every backend of the transition store runs (src/program/transition_validator.h).
#include "transition_validator.h"

#include <emscripten.h>

#include <asio/awaitable.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace neograph::wasm {
namespace {

// The JavaScript half. Module.ngStore* are installed by installStoreBridge (wasm/stores.mjs). A missing bridge answers
// -3 instead of throwing: a JavaScript exception must not cross into the module.
EM_JS(int, ng_js_store_has, (int handle, const char* member, const char* method), {
  if (typeof Module.ngStoreHas !== 'function') return -3;
  return Module.ngStoreHas(handle, UTF8ToString(member), method ? UTF8ToString(method) : null);
});

// 0: the method returned a value (the reply is its JSON text); 1: it failed (the reply is {"kind", "message"}); 2: it
// returned a Promise, which reports through ng_store_resolve / ng_store_reject; -3: no bridge.
EM_JS(int, ng_js_store_call, (int handle, const char* member, const char* method, const char* args, int call), {
  if (typeof Module.ngStoreCall !== 'function') return -3;
  return Module.ngStoreCall(handle, UTF8ToString(member), UTF8ToString(method), UTF8ToString(args), call);
});

// The reply, in memory that the module allocated (ng_store_alloc) and take_reply frees.
EM_JS(char*, ng_js_store_reply, (), {
  const text = Module.ngStoreReply ?? "";
  Module.ngStoreReply = "";
  const size = lengthBytesUTF8(text) + 1;
  const pointer = _ng_store_alloc(size);
  stringToUTF8(text, pointer, size);
  return pointer;
});
EM_JS_DEPS(ng_js_store_deps, "$UTF8ToString,$lengthBytesUTF8,$stringToUTF8");

std::string take_reply() {
    char* reply = ng_js_store_reply();
    std::string text = reply ? reply : "";
    ::operator delete(reply);
    return text;
}

// How a call ended in JavaScript.
struct Outcome {
    bool        ok = true;
    std::string text;  // the result's JSON text, or the message of a failure
    std::string kind;  // a failure's C++ exception class
};

// One call that returned a Promise. The awaiting coroutine owns the shared pointer; the table holds a weak one.
struct PendingCall {
    std::optional<Outcome> outcome;
    asio::steady_timer*    wait = nullptr;  // the timer that the coroutine waits on; null once it stopped waiting
    asio::any_io_executor  home;            // the executor the coroutine runs on
};

struct Bridge {
    std::unordered_map<int, std::weak_ptr<PendingCall>> calls;
    int                                                 last_call = 0;
    asio::any_io_executor                               executor;
    JsStoreStats                                        stats;
};

// Never destroyed: a coroutine that is destroyed during exit still unregisters its call.
Bridge& bridge() {
    static Bridge* instance = new Bridge;
    return *instance;
}

// The C++ exception class that a JavaScript failure names. A host's own errors name none, and are runtime errors.
[[noreturn]] void throw_failure(const std::string& kind, const std::string& message) {
    if (kind == "invalid_argument") throw std::invalid_argument(message);
    if (kind == "out_of_range") throw std::out_of_range(message);
    if (kind == "domain_error") throw std::domain_error(message);
    if (kind == "length_error") throw std::length_error(message);
    if (kind == "logic_error") throw std::logic_error(message);
    throw std::runtime_error(message);
}

[[noreturn]] void throw_failure_reply(const std::string& reply) {
    std::string kind, message = reply;
    try {
        const auto parsed = json::parse(reply);
        kind              = parsed.value("kind", "");
        message           = parsed.value("message", "");
    } catch (const std::exception&) {
        // The reply is the message itself.
    }
    throw_failure(kind, message);
}

// Makes the call and returns its result's JSON text. A value completes it without suspending.
asio::awaitable<std::string> exchange(int handle, std::string member, const char* method, std::string arguments) {
    auto& state = bridge();
    const int call = ++state.last_call;
    ++state.stats.calls;
    const int status = ng_js_store_call(handle, member.c_str(), method, arguments.c_str(), call);
    if (status == 0) {
        ++state.stats.immediate;
        co_return take_reply();
    }
    if (status == 1) {
        ++state.stats.immediate;
        throw_failure_reply(take_reply());
    }
    if (status != 2)
        throw std::runtime_error("the JavaScript store bridge is not installed (installStoreBridge): " + member + "." +
                                 method);

    // A Promise. The call is registered before this coroutine can suspend, and JavaScript settles it from a later
    // task, so the outcome always finds the entry.
    ++state.stats.pending;
    auto pending  = std::make_shared<PendingCall>();
    pending->home = co_await asio::this_coro::executor;
    state.calls.emplace(call, pending);
    struct Registration {
        int call;
        ~Registration() {
            auto& table = bridge();
            table.calls.erase(call);
        }
    } registration{call};

    asio::steady_timer wait(pending->home, asio::steady_timer::time_point::max());
    pending->wait = &wait;
    asio::error_code ec;
    co_await wait.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    pending->wait = nullptr;
    if (!pending->outcome) throw asio::system_error(ec ? ec : asio::error::operation_aborted);
    ++bridge().stats.completed;
    Outcome outcome = std::move(*pending->outcome);
    if (!outcome.ok) throw_failure(outcome.kind, outcome.text);
    co_return std::move(outcome.text);
}

// Hands the outcome of call `call` to its coroutine, from a task of the executor it runs on.
void settle(int call, Outcome outcome) {
    auto& state = bridge();
    std::shared_ptr<PendingCall> pending;
    if (const auto found = state.calls.find(call); found != state.calls.end()) pending = found->second.lock();
    if (!pending || pending->outcome) {
        ++state.stats.dropped;  // the coroutine is gone, or the call already has its outcome
        return;
    }
    pending->outcome = std::move(outcome);
    const auto target = state.executor ? state.executor : pending->home;
    asio::post(target, [weak = std::weak_ptr<PendingCall>(pending)] {
        if (const auto call = weak.lock(); call && call->wait) call->wait->cancel();
    });
}

template <class R>
R decode_reply(const std::string& member, const char* method, const std::string& text) {
    try {
        return Codec<R>::decode(json::parse(text));
    } catch (const std::exception& error) {
        throw std::runtime_error("JavaScript store method '" + member + "." + method +
                                 "' returned a malformed value: " + error.what());
    }
}

template <class R>
asio::awaitable<R> invoke(int handle, std::string member, const char* method, std::string arguments) {
    const std::string text = co_await exchange(handle, member, method, std::move(arguments));
    if constexpr (std::is_void_v<R>) {
        co_return;
    } else {
        co_return decode_reply<R>(member, method, text);
    }
}

// One member of the host's object: the methods it has, and how to call them.
class Member {
public:
    using Methods = std::set<std::string, std::less<>>;

    Member(int handle, std::string name, std::shared_ptr<const Methods> methods, std::shared_ptr<const void> lease)
        : handle_(handle), name_(std::move(name)), methods_(std::move(methods)), lease_(std::move(lease)) {}

    template <class R, class... A>
    bool has(const Op<R, A...>& op) const {
        return methods_->find(std::string_view(op.method)) != methods_->end();
    }

    /// The arguments are copied into the call here, before the coroutine exists.
    template <class R, class... A, class... Given>
    asio::awaitable<R> call(const Op<R, A...>& op, const Given&... given) const {
        return invoke<R>(handle_, name_, op.method, encode_call(op, given...));
    }

    std::string coordination_key() const { return "neograph-js-stores:" + std::to_string(handle_); }

private:
    int                            handle_;
    std::string                    name_;
    std::shared_ptr<const Methods> methods_;
    std::shared_ptr<const void>    lease_;  // held for the registration of the object, if the caller gave one
};

namespace tr = transition_ops;
namespace pr = program_ops;
namespace gr = grant_ops;
namespace fr = fragment_ops;
namespace mo = module_ops;

// ---------------------------------------------------------------------------------------------------------------
// The adapters. An operation that the interface does not require and the object lacks keeps the interface's default.

class JsTransitionStore final : public program::ProgramTransitionStore {
public:
    explicit JsTransitionStore(Member member) : member_(std::move(member)) {}

    std::string process_coordination_key() const override { return member_.coordination_key(); }

    asio::awaitable<std::optional<program::ProgramRunRecord>> load(std::string_view owner_scope,
                                                                    std::string_view run_id) const override {
        return member_.call(tr::load, owner_scope, run_id);
    }
    asio::awaitable<std::optional<program::ProgramJournalRecord>> latest(std::string_view owner_scope,
                                                                          std::string_view run_id) const override {
        return member_.call(tr::latest, owner_scope, run_id);
    }
    asio::awaitable<std::optional<program::ProgramRunPublicationHead>> load_run_publication_head(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_run_publication_head))
            return ProgramTransitionStore::load_run_publication_head(owner_scope, run_id);
        return member_.call(tr::load_run_publication_head, owner_scope, run_id);
    }
    asio::awaitable<std::vector<program::ProgramEvent>> load_events(std::string_view owner_scope,
                                                                     std::string_view run_id,
                                                                     std::uint64_t after_sequence) const override {
        return member_.call(tr::load_events, owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<program::ProgramEffectOutboxEntry>> load_effects(
        std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        return member_.call(tr::load_effects, owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<program::ProgramJavaScriptCommandJournalEntry>> load_javascript_commands(
        std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        if (!member_.has(tr::load_javascript_commands))
            return ProgramTransitionStore::load_javascript_commands(owner_scope, run_id, after_sequence);
        return member_.call(tr::load_javascript_commands, owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::optional<program::ProgramCommandPublicationHead>> load_command_publication_head(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_command_publication_head))
            return ProgramTransitionStore::load_command_publication_head(owner_scope, run_id);
        return member_.call(tr::load_command_publication_head, owner_scope, run_id);
    }
    asio::awaitable<std::vector<program::ProgramContextPublication>> load_context_publications(
        std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        if (!member_.has(tr::load_context_publications))
            return ProgramTransitionStore::load_context_publications(owner_scope, run_id, after_sequence);
        return member_.call(tr::load_context_publications, owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<HookOutboxEntry>> load_hook_outbox_entries(std::string_view owner_scope,
                                                                            std::string_view run_id) const override {
        if (!member_.has(tr::load_hook_outbox_entries))
            return ProgramTransitionStore::load_hook_outbox_entries(owner_scope, run_id);
        return member_.call(tr::load_hook_outbox_entries, owner_scope, run_id);
    }
    asio::awaitable<program::ProgramEffectiveRuntimeState> load_effective_runtime_state(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_effective_runtime_state))
            return ProgramTransitionStore::load_effective_runtime_state(owner_scope, run_id);
        return member_.call(tr::load_effective_runtime_state, owner_scope, run_id);
    }
    asio::awaitable<std::vector<program::ProgramChildSynthesisRecord>> load_child_syntheses(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_child_syntheses))
            return ProgramTransitionStore::load_child_syntheses(owner_scope, run_id);
        return member_.call(tr::load_child_syntheses, owner_scope, run_id);
    }
    asio::awaitable<std::optional<program::MigrationPlan>> load_migration_plan(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_migration_plan))
            return ProgramTransitionStore::load_migration_plan(owner_scope, run_id);
        return member_.call(tr::load_migration_plan, owner_scope, run_id);
    }
    asio::awaitable<std::optional<program::ProgramRunLineage>> load_lineage(
        std::string_view owner_scope, std::string_view lineage_id) const override {
        if (!member_.has(tr::load_lineage)) return ProgramTransitionStore::load_lineage(owner_scope, lineage_id);
        return member_.call(tr::load_lineage, owner_scope, lineage_id);
    }
    asio::awaitable<std::optional<program::ProgramRunLineage>> load_run_lineage(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_run_lineage)) return ProgramTransitionStore::load_run_lineage(owner_scope, run_id);
        return member_.call(tr::load_run_lineage, owner_scope, run_id);
    }
    asio::awaitable<std::optional<program::ProgramRunLineage>> load_lineage_head(
        std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const override {
        if (!member_.has(tr::load_lineage_head))
            return ProgramTransitionStore::load_lineage_head(owner_scope, lineage_id, head_id);
        return member_.call(tr::load_lineage_head, owner_scope, lineage_id, head_id);
    }
    asio::awaitable<std::optional<program::ProgramRunGeneration>> load_generation(
        std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        if (!member_.has(tr::load_generation))
            return ProgramTransitionStore::load_generation(owner_scope, lineage_id, generation);
        return member_.call(tr::load_generation, owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<program::ProgramTransitionPublication>> load_generation_initial_publication(
        std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        if (!member_.has(tr::load_generation_initial_publication))
            return ProgramTransitionStore::load_generation_initial_publication(owner_scope, lineage_id, generation);
        return member_.call(tr::load_generation_initial_publication, owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<program::GraphMigrationCapsule>> load_graph_migration_capsule(
        std::string_view owner_scope, std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override {
        if (!member_.has(tr::load_graph_migration_capsule))
            return ProgramTransitionStore::load_graph_migration_capsule(owner_scope, source_run_id,
                                                                        source_lineage_head_id);
        return member_.call(tr::load_graph_migration_capsule, owner_scope, source_run_id, source_lineage_head_id);
    }
    asio::awaitable<std::optional<program::ProgramExecutionLease>> load_execution_lease(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (!member_.has(tr::load_execution_lease))
            return ProgramTransitionStore::load_execution_lease(owner_scope, run_id);
        return member_.call(tr::load_execution_lease, owner_scope, run_id);
    }
    // Every backend of the transition store begins a publication with prepare_transition: the checks that need no stored
    // state, which also write the publication's canonical text. A publication it refuses is a Conflict, and never reaches
    // JavaScript; the text it returns is what crosses. What the stored state says about the publication is the store's.
    asio::awaitable<program::ProgramTransitionPublishResult> compare_publish(
        std::string_view owner_scope, std::string_view expected_journal_head, std::uint64_t expected_fence,
        program::ProgramTransitionPublication publication) override {
        if (expected_fence == 0 ||
            publication.run_record.continuation().fence != expected_fence ||
            publication.journal_record.continuation.fence != expected_fence)
            return refused();
        const auto text = program::detail::transition::prepare_transition(
            owner_scope, expected_journal_head, publication, nullptr, nullptr, nullptr, nullptr);
        if (!text) return refused();
        return member_.call(tr::compare_publish, owner_scope, expected_journal_head, expected_fence,
                            PreparedText{*text});
    }
    asio::awaitable<program::ProgramTransitionPublishResult> compare_publish_execution(
        std::string_view owner_scope, std::string_view expected_journal_head, std::uint64_t expected_fence,
        program::ProgramTransitionPublication publication, std::optional<program::ProgramExecutionLease> expected_lease,
        std::optional<program::ProgramExecutionLease> next_lease) override {
        if (!member_.has(tr::compare_publish_execution))
            return ProgramTransitionStore::compare_publish_execution(
                owner_scope, expected_journal_head, expected_fence, std::move(publication),
                std::move(expected_lease), std::move(next_lease));
        if (expected_fence == 0 ||
            publication.run_record.continuation().fence != expected_fence ||
            publication.journal_record.continuation.fence != expected_fence)
            return refused();
        const auto text = program::detail::transition::prepare_transition(
            owner_scope, expected_journal_head, publication, nullptr, nullptr,
            expected_lease ? &*expected_lease : nullptr, next_lease ? &*next_lease : nullptr);
        if (!text) return refused();
        return member_.call(tr::compare_publish_execution, owner_scope, expected_journal_head, expected_fence,
                            PreparedText{*text}, expected_lease, next_lease);
    }
    asio::awaitable<program::ProgramTransitionPublishResult> compare_publish_takeover(
        std::string_view owner_scope, std::string_view run_id, std::string_view expected_journal_head,
        std::uint64_t expected_fence) override {
        if (!program::detail::transition::valid_takeover_request(owner_scope, run_id,
                                                                 expected_journal_head, expected_fence))
            return refused();
        return member_.call(tr::compare_publish_takeover, owner_scope, run_id, expected_journal_head,
                            expected_fence);
    }
    // compare_publish_graph_safe_point carries evidence that is never serialized: the base class answers Conflict.

private:
    static asio::awaitable<program::ProgramTransitionPublishResult> refused() {
        co_return program::ProgramTransitionPublishResult::Conflict;
    }

    Member member_;
};

class JsProgramStore final : public program::ProgramStore {
public:
    explicit JsProgramStore(Member member) : member_(std::move(member)) {}

    asio::awaitable<void> publish_admitted(const program::ProgramBundle& bundle,
                                           const program::ProgramVersion& version) override {
        return member_.call(pr::publish_admitted, bundle, version);
    }
    asio::awaitable<std::optional<program::ProgramBundle>> get_bundle(std::string_view id) const override {
        return member_.call(pr::get_bundle, id);
    }
    asio::awaitable<std::optional<program::ProgramVersion>> get_version(std::string_view id) const override {
        return member_.call(pr::get_version, id);
    }
    // The owner-qualified overloads call the same methods with the owner first.
    asio::awaitable<std::optional<program::ProgramBundle>> get_bundle(std::string_view owner_scope,
                                                                       std::string_view id) const override {
        return member_.call(pr::get_bundle_owned, owner_scope, id);
    }
    asio::awaitable<std::optional<program::ProgramVersion>> get_version(std::string_view owner_scope,
                                                                         std::string_view id) const override {
        return member_.call(pr::get_version_owned, owner_scope, id);
    }
    asio::awaitable<std::optional<program::ProgramActivation>> get_activation(
        std::string_view owner_scope) const override {
        if (!member_.has(pr::get_activation)) return ProgramStore::get_activation(owner_scope);
        return member_.call(pr::get_activation, owner_scope);
    }
    asio::awaitable<std::optional<program::ProgramActivationBinding>> get_active_binding(
        std::string_view owner_scope) const override {
        if (!member_.has(pr::get_active_binding)) return ProgramStore::get_active_binding(owner_scope);
        return member_.call(pr::get_active_binding, owner_scope);
    }
    asio::awaitable<program::ProgramActivationResult> compare_activate(
        std::string_view owner_scope, std::uint64_t expected_generation, std::string_view version_id,
        std::string_view policy_snapshot_hash) override {
        if (!member_.has(pr::compare_activate))
            return ProgramStore::compare_activate(owner_scope, expected_generation, version_id, policy_snapshot_hash);
        return member_.call(pr::compare_activate, owner_scope, expected_generation, version_id,
                            policy_snapshot_hash);
    }
    asio::awaitable<std::vector<program::ProgramVersion>> list_versions(std::string_view owner_scope) const override {
        if (!member_.has(pr::list_versions)) return ProgramStore::list_versions(owner_scope);
        return member_.call(pr::list_versions, owner_scope);
    }
    asio::awaitable<program::ProgramRetentionReport> collect_garbage(
        std::string_view owner_scope, const std::vector<std::string>& pinned_version_ids) override {
        if (!member_.has(pr::collect_garbage)) return ProgramStore::collect_garbage(owner_scope, pinned_version_ids);
        return member_.call(pr::collect_garbage, owner_scope, pinned_version_ids);
    }

private:
    Member member_;
};

class JsGrantStore final : public program::ProgramCoreToolGrantStore {
public:
    explicit JsGrantStore(Member member) : member_(std::move(member)) {}

    asio::awaitable<program::ProgramCoreToolGrantAdmission> admit(
        const program::ProgramCoreToolGrantRecord& record) override {
        return member_.call(gr::admit, record);
    }
    asio::awaitable<std::optional<program::ProgramCoreToolGrantRecord>> load(
        const program::ProgramCoreToolGrantContext& context) const override {
        return member_.call(gr::load, context);
    }
    asio::awaitable<bool> revoke(const program::ProgramCoreToolGrantContext& context) override {
        return member_.call(gr::revoke, context);
    }

private:
    Member member_;
};

class JsFragmentStore final : public program::TaskGraphFragmentStore {
public:
    explicit JsFragmentStore(Member member) : member_(std::move(member)) {}

    asio::awaitable<program::TaskGraphPublishResult> publish(const program::TaskGraphFragmentRecord& record) override {
        return member_.call(fr::publish, record);
    }
    asio::awaitable<std::optional<program::TaskGraphFragmentRecord>> load(std::string_view fragment_id) const override {
        return member_.call(fr::load, fragment_id);
    }
    asio::awaitable<std::optional<program::TaskGraphFragmentRecord>> find_published_lineage(
        std::string_view owner_scope, std::string_view parent_run_id,
        std::string_view expansion_operation_id) const override {
        if (!member_.has(fr::find_published_lineage))
            return TaskGraphFragmentStore::find_published_lineage(owner_scope, parent_run_id, expansion_operation_id);
        return member_.call(fr::find_published_lineage, owner_scope, parent_run_id, expansion_operation_id);
    }
    asio::awaitable<std::optional<program::TaskGraphFragmentRecord>> find_published(
        std::string_view owner_scope, std::string_view parent_run_id, std::string_view expansion_operation_id,
        std::string_view proposal_hash) const override {
        if (!member_.has(fr::find_published))
            return TaskGraphFragmentStore::find_published(owner_scope, parent_run_id, expansion_operation_id,
                                                          proposal_hash);
        return member_.call(fr::find_published, owner_scope, parent_run_id, expansion_operation_id, proposal_hash);
    }
    asio::awaitable<program::TaskGraphPublishResult> compare_update(
        std::string_view fragment_id, std::uint64_t expected_revision,
        const program::TaskGraphFragmentRecord& record) override {
        return member_.call(fr::compare_update, fragment_id, expected_revision, record);
    }

private:
    Member member_;
};

class JsModuleStore final : public program::ModuleStore {
public:
    explicit JsModuleStore(Member member) : member_(std::move(member)) {}

    asio::awaitable<void> publish(const program::ProgramModule& module) override {
        return member_.call(mo::publish, module);
    }
    asio::awaitable<std::optional<program::ProgramModule>> get(std::string_view content_identity) const override {
        return member_.call(mo::get, content_identity);
    }
    asio::awaitable<std::optional<program::ProgramModule>> get(std::string_view owner_scope,
                                                                std::string_view content_identity) const override {
        return member_.call(mo::get_owned, owner_scope, content_identity);
    }
    asio::awaitable<void> set_lifecycle(std::string_view content_identity,
                                        program::ModuleLifecycle lifecycle) override {
        return member_.call(mo::set_lifecycle, content_identity, lifecycle);
    }

private:
    Member member_;
};

// The member `name` of the host's object, or nothing when the object has none.
std::optional<Member> member_of(int handle, const char* name, std::span<const OpInfo> rows,
                                const std::shared_ptr<const void>& lease) {
    const int present = ng_js_store_has(handle, name, nullptr);
    if (present == -3)
        throw std::invalid_argument("the JavaScript store bridge is not installed (installStoreBridge)");
    if (present == -2) throw std::invalid_argument("no stores are registered under handle " + std::to_string(handle));
    if (present == 0) return std::nullopt;
    if (present < 0) throw std::invalid_argument(std::string("stores.") + name + " is not an object");
    auto methods = std::make_shared<Member::Methods>();
    for (const auto& row : rows) {
        const int has = ng_js_store_has(handle, name, row.method);
        if (has == 1) throw std::invalid_argument(std::string("stores.") + name + "." + row.method + " is not a function");
        if (has == 2) {
            methods->emplace(row.method);
        } else if (row.required) {
            throw std::invalid_argument(std::string("stores.") + name + "." + row.method + " is required");
        }
    }
    return Member(handle, name, std::move(methods), lease);
}

program::ProgramSynthesisSemanticDecision decode_semantic_decision(const json& value) {
    if (!value.is_object()) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            "JavaScript synthesis validator must return an object {validatorIdentity, contractIdentity, accepted, evidence?}");
    }
    for (const auto& [key, _] : value.items()) {
        if (key != "validatorIdentity" && key != "contractIdentity" && key != "accepted" && key != "evidence") {
            throw program::ProgramChildSynthesisError(
                "P_CHILD_SYNTHESIS_SEMANTICS",
                "JavaScript synthesis validator returned unknown field: " + key);
        }
    }
    if (!value.contains("validatorIdentity") || !value.at("validatorIdentity").is_string() ||
        value.at("validatorIdentity").get<std::string>().empty()) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            "JavaScript synthesis validator decision.validatorIdentity must be a non-empty string");
    }
    if (!value.contains("contractIdentity") || !value.at("contractIdentity").is_string() ||
        value.at("contractIdentity").get<std::string>().empty()) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            "JavaScript synthesis validator decision.contractIdentity must be a non-empty string");
    }
    if (!value.contains("accepted") || !value.at("accepted").is_boolean()) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            "JavaScript synthesis validator decision.accepted must be a boolean");
    }
    json evidence = json::object();
    if (value.contains("evidence")) {
        if (!value.at("evidence").is_object()) {
            throw program::ProgramChildSynthesisError(
                "P_CHILD_SYNTHESIS_SEMANTICS",
                "JavaScript synthesis validator decision.evidence must be an object");
        }
        evidence = value.at("evidence");
    }
    return program::ProgramSynthesisSemanticDecision{
        value.at("validatorIdentity").get<std::string>(),
        value.at("contractIdentity").get<std::string>(),
        value.at("accepted").get<bool>(),
        std::move(evidence)};
}

asio::awaitable<program::ProgramSynthesisSemanticDecision> invoke_js_synthesis_validator(
    int handle, std::shared_ptr<const void> lease, std::string arguments) {
    const auto keep_alive = std::move(lease);
    (void)keep_alive;
    std::string text;
    try {
        text = co_await exchange(handle, "synthesis", "validate", std::move(arguments));
    } catch (const std::exception& error) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            std::string("JavaScript synthesis validator failed: ") + error.what());
    }
    json parsed;
    try {
        parsed = program::detail::parse_json_strict(text);
    } catch (const std::exception& error) {
        throw program::ProgramChildSynthesisError(
            "P_CHILD_SYNTHESIS_SEMANTICS",
            std::string("JavaScript synthesis validator returned invalid JSON: ") + error.what());
    }
    co_return decode_semantic_decision(parsed);
}

}  // namespace

JsStoreSet adapt_js_stores(int handle, std::shared_ptr<const void> lease) {
    JsStoreSet set;
    if (auto member = member_of(handle, "transitions", transition_ops::all, lease))
        set.transitions = std::make_shared<JsTransitionStore>(std::move(*member));
    if (auto member = member_of(handle, "programs", program_ops::all, lease))
        set.programs = std::make_shared<JsProgramStore>(std::move(*member));
    if (auto member = member_of(handle, "grants", grant_ops::all, lease))
        set.grants = std::make_shared<JsGrantStore>(std::move(*member));
    if (auto member = member_of(handle, "fragments", fragment_ops::all, lease))
        set.fragments = std::make_shared<JsFragmentStore>(std::move(*member));
    if (auto member = member_of(handle, "modules", module_ops::all, lease))
        set.modules = std::make_shared<JsModuleStore>(std::move(*member));
    return set;
}

program::ProgramSynthesisAsyncSemanticValidator adapt_js_synthesis_validator(
    int handle, std::shared_ptr<const void> lease) {
    const int has = ng_js_store_has(handle, "synthesis", "validate");
    if (has == -3)
        throw std::invalid_argument("the JavaScript store bridge is not installed (installStoreBridge)");
    if (has == -2)
        throw std::invalid_argument("no synthesis validator is registered under handle " + std::to_string(handle));
    if (has != 2)
        throw std::invalid_argument("synthesis validator must be a function");
    return [handle, lease = std::move(lease)](
               const program::ProgramSynthesisProposal&    proposal,
               const program::ProgramBundle&               bundle,
               const program::ProgramSynthesisReservation& reservation)
               -> asio::awaitable<program::ProgramSynthesisSemanticDecision> {
        std::string arguments =
            json::array({program::detail::parse_json_strict(proposal.serialize_canonical()),
                         program::detail::parse_json_strict(bundle.serialize_canonical()),
                         program::detail::parse_json_strict(reservation.serialize_canonical())})
                .dump();
        return invoke_js_synthesis_validator(handle, lease, std::move(arguments));
    };
}

void set_js_store_executor(asio::any_io_executor executor) { bridge().executor = std::move(executor); }

JsStoreStats js_store_stats() {
    auto stats    = bridge().stats;
    stats.waiting = bridge().calls.size();
    return stats;
}

}  // namespace neograph::wasm

// The bridge calls these from a later task of the host's event loop, with the outcome of a Promise.
extern "C" {

EMSCRIPTEN_KEEPALIVE void* ng_store_alloc(int size) { return ::operator new(static_cast<std::size_t>(size)); }
EMSCRIPTEN_KEEPALIVE void ng_store_free(void* pointer) { ::operator delete(pointer); }

// `text` is the JSON text of the method's result.
EMSCRIPTEN_KEEPALIVE void ng_store_resolve(int call, const char* text) {
    try {
        neograph::wasm::settle(call, neograph::wasm::Outcome{true, text ? text : "null", {}});
    } catch (...) {
    }
}

// `kind` names the C++ exception class that the failure becomes (empty: std::runtime_error).
EMSCRIPTEN_KEEPALIVE void ng_store_reject(int call, const char* message, const char* kind) {
    try {
        neograph::wasm::settle(call, neograph::wasm::Outcome{false, message ? message : "", kind ? kind : ""});
    } catch (...) {
    }
}

}  // extern "C"
