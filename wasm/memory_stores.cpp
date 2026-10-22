// The reference store of the JavaScript store contract: the in-memory C++ stores, executed from JavaScript.
//
// ng_memory_create makes a set of the five stores. ng_memory_call executes one operation of one of them from the name of
// the member (`transitions`, `programs`, `grants`, `fragments`, `modules`), the name of the method and a JSON array of
// arguments: it decodes the arguments with the codecs the adapters encode with, calls the store's `..._sync` operation,
// and returns the result encoded with the codec the adapters decode with. wasm/stores.mjs wraps these in objects that
// implement the contract (memoryStores).
#include "memory_stores.h"

#include "js_store_codec.h"

#include <emscripten.h>

#include <cstdlib>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace neograph::wasm {
namespace {

// The grant store has no in-memory form in the library, so the reference keeps the same decisions as the SQLite one:
// an identity is admitted once, a grant id names one identity, revocation is permanent, and a read or a revocation
// matches the exact binding.
class MemoryCoreToolGrantStore final : public program::SyncProgramCoreToolGrantStore {
public:
    program::ProgramCoreToolGrantAdmission admit_sync(const program::ProgramCoreToolGrantRecord& record) override {
        if (record.owner_scope.empty() || record.program_version_id.empty() || record.run_id.empty() ||
            record.operation_id.empty() || record.attempt == 0 || record.binding_fingerprint.empty() ||
            record.grant_id.empty())
            throw std::invalid_argument("Core Tool grant identity must be complete");
        for (const auto& [key, entry] : grants_) {
            if (std::get<0>(key) != record.owner_scope || entry.grant_id != record.grant_id) continue;
            if (std::get<1>(key) != record.program_version_id || std::get<2>(key) != record.run_id ||
                std::get<3>(key) != record.operation_id || entry.binding_fingerprint != record.binding_fingerprint ||
                entry.revoked)
                return program::ProgramCoreToolGrantAdmission::Conflict;
            break;
        }
        const auto key = std::make_tuple(record.owner_scope, record.program_version_id, record.run_id,
                                         record.operation_id, record.attempt);
        const auto [position, inserted] = grants_.emplace(key, Entry{record.binding_fingerprint, record.grant_id, false});
        if (inserted) return program::ProgramCoreToolGrantAdmission::Admitted;
        const auto& existing = position->second;
        return existing.binding_fingerprint == record.binding_fingerprint && existing.grant_id == record.grant_id &&
                       !existing.revoked
                   ? program::ProgramCoreToolGrantAdmission::AlreadyPresent
                   : program::ProgramCoreToolGrantAdmission::Conflict;
    }

    std::optional<program::ProgramCoreToolGrantRecord> load_sync(
        const program::ProgramCoreToolGrantContext& context) const override {
        const auto* entry = find(context);
        if (!entry) return std::nullopt;
        return program::ProgramCoreToolGrantRecord{
            std::string(context.owner_scope), std::string(context.program_version_id), std::string(context.run_id),
            std::string(context.operation_id), context.attempt, std::string(context.binding_fingerprint),
            entry->grant_id};
    }

    bool revoke_sync(const program::ProgramCoreToolGrantContext& context) override {
        auto* entry = const_cast<Entry*>(find(context));
        if (!entry) return false;
        entry->revoked = true;
        return true;
    }

private:
    struct Entry {
        std::string binding_fingerprint;
        std::string grant_id;
        bool        revoked = false;
    };
    using Key = std::tuple<std::string, std::string, std::string, std::string, std::uint64_t>;

    const Entry* find(const program::ProgramCoreToolGrantContext& context) const {
        const auto found = grants_.find(Key{std::string(context.owner_scope), std::string(context.program_version_id),
                                            std::string(context.run_id), std::string(context.operation_id),
                                            context.attempt});
        if (found == grants_.end() || found->second.revoked ||
            found->second.binding_fingerprint != context.binding_fingerprint)
            return nullptr;
        return &found->second;
    }

    std::map<Key, Entry> grants_;
};

// Runs one operation: decodes the arguments, calls `fn` with them and encodes what it returns.
template <class R, class... A, class Fn>
std::string run(const json& arguments, Fn&& fn) {
    auto owned = decode_arguments<A...>(arguments);
    if constexpr (std::is_void_v<R>) {
        std::apply(fn, owned);
        return "null";
    } else {
        return Codec<R>::encode(std::apply(fn, owned)).dump();
    }
}

// The operations of one kind of store, found by the JavaScript name and the number of arguments.
template <class Store>
using Handlers = std::map<std::pair<std::string_view, std::size_t>, std::string (*)(Store&, const json&)>;

#define NEOGRAPH_MEMORY_HANDLER(row, cxx, js, required, R, ...)                                                      \
    table.emplace(std::pair<std::string_view, std::size_t>{js, ::neograph::wasm::Op<R, __VA_ARGS__>::arity},       \
                  +[](Store& store, const json& arguments) -> std::string {                                       \
                      return run<R, __VA_ARGS__>(arguments, [&store](auto&... argument) -> decltype(auto) {       \
                          return store.cxx##_sync(argument...);                                                    \
                      });                                                                                          \
                  });

const Handlers<program::SyncProgramTransitionStore>& transition_handlers() {
    using Store = program::SyncProgramTransitionStore;
    static const auto handlers = [] {
        Handlers<Store> table;
        NEOGRAPH_JS_TRANSITION_OPS(NEOGRAPH_MEMORY_HANDLER)
        return table;
    }();
    return handlers;
}
const Handlers<program::SyncProgramStore>& program_handlers() {
    using Store = program::SyncProgramStore;
    static const auto handlers = [] {
        Handlers<Store> table;
        NEOGRAPH_JS_PROGRAM_OPS(NEOGRAPH_MEMORY_HANDLER)
        return table;
    }();
    return handlers;
}
const Handlers<program::SyncProgramCoreToolGrantStore>& grant_handlers() {
    using Store = program::SyncProgramCoreToolGrantStore;
    static const auto handlers = [] {
        Handlers<Store> table;
        NEOGRAPH_JS_GRANT_OPS(NEOGRAPH_MEMORY_HANDLER)
        return table;
    }();
    return handlers;
}
const Handlers<program::SyncTaskGraphFragmentStore>& fragment_handlers() {
    using Store = program::SyncTaskGraphFragmentStore;
    static const auto handlers = [] {
        Handlers<Store> table;
        NEOGRAPH_JS_FRAGMENT_OPS(NEOGRAPH_MEMORY_HANDLER)
        return table;
    }();
    return handlers;
}
const Handlers<program::SyncModuleStore>& module_handlers() {
    using Store = program::SyncModuleStore;
    static const auto handlers = [] {
        Handlers<Store> table;
        NEOGRAPH_JS_MODULE_OPS(NEOGRAPH_MEMORY_HANDLER)
        return table;
    }();
    return handlers;
}

template <class Store>
std::string dispatch(const Handlers<Store>& handlers, std::type_identity_t<Store>& store, std::string_view member,
                     std::string_view method,
                     const json& arguments) {
    const auto found = handlers.find({method, arguments.is_array() ? arguments.size() : 0});
    if (found == handlers.end())
        throw std::invalid_argument("stores." + std::string(member) + " has no method '" + std::string(method) +
                                    "' for " + std::to_string(arguments.is_array() ? arguments.size() : 0) +
                                    " arguments");
    return found->second(store, arguments);
}

std::span<const OpInfo> rows_of(std::string_view member) {
    if (member == "transitions") return transition_ops::all;
    if (member == "programs") return program_ops::all;
    if (member == "grants") return grant_ops::all;
    if (member == "fragments") return fragment_ops::all;
    if (member == "modules") return module_ops::all;
    throw std::invalid_argument("no store named '" + std::string(member) + "'");
}

std::map<int, std::unique_ptr<MemoryStoreSet>>& sets() {
    static auto* table = new std::map<int, std::unique_ptr<MemoryStoreSet>>;
    return *table;
}

}  // namespace

int create_memory_stores() {
    static int last = 0;
    auto       set  = std::make_unique<MemoryStoreSet>();
    set->transitions = std::make_shared<program::InMemoryProgramTransitionStore>();
    set->programs    = std::make_shared<program::InMemoryProgramStore>();
    set->grants      = std::make_shared<MemoryCoreToolGrantStore>();
    set->fragments   = std::make_shared<program::InMemoryTaskGraphFragmentStore>();
    set->modules     = std::make_shared<program::InMemoryModuleStore>();
    sets().emplace(++last, std::move(set));
    return last;
}

void release_memory_stores(int set) { sets().erase(set); }

MemoryStoreSet* find_memory_stores(int set) {
    const auto found = sets().find(set);
    return found == sets().end() ? nullptr : found->second.get();
}

std::string memory_store_methods(std::string_view member) {
    json names = json::array();
    std::set<std::string_view> seen;
    for (const auto& row : rows_of(member))
        if (seen.insert(row.method).second) names.push_back(json(std::string(row.method)));
    return names.dump();
}

std::string call_memory_store(MemoryStoreSet& set, std::string_view member, std::string_view method,
                              std::string_view arguments) {
    const auto parsed = json::parse(arguments);
    if (member == "transitions") return dispatch(transition_handlers(), *set.transitions, member, method, parsed);
    if (member == "programs") return dispatch(program_handlers(), *set.programs, member, method, parsed);
    if (member == "grants") return dispatch(grant_handlers(), *set.grants, member, method, parsed);
    if (member == "fragments") return dispatch(fragment_handlers(), *set.fragments, member, method, parsed);
    if (member == "modules") return dispatch(module_handlers(), *set.modules, member, method, parsed);
    throw std::invalid_argument("no store named '" + std::string(member) + "'");
}

}  // namespace neograph::wasm

namespace {

std::string& reply() {
    static auto* text = new std::string;
    return *text;
}

// The C++ exception class a failure is reported as; wasm/stores.mjs puts it on the Error as `kind`.
std::string kind_of(const std::exception& error) {
    if (dynamic_cast<const std::invalid_argument*>(&error)) return "invalid_argument";
    if (dynamic_cast<const std::out_of_range*>(&error)) return "out_of_range";
    if (dynamic_cast<const std::domain_error*>(&error)) return "domain_error";
    if (dynamic_cast<const std::length_error*>(&error)) return "length_error";
    if (dynamic_cast<const std::logic_error*>(&error)) return "logic_error";
    return "runtime_error";
}

void fail(const std::string& kind, const std::string& message) {
    neograph::json out = neograph::json::object();
    out["kind"]        = neograph::json(kind);
    out["message"]     = neograph::json(message);
    reply()            = out.dump();
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int ng_memory_create() { return neograph::wasm::create_memory_stores(); }

EMSCRIPTEN_KEEPALIVE void ng_memory_release(int set) { neograph::wasm::release_memory_stores(set); }

// Executes one operation. Returns 0 and leaves the result's JSON text in ng_memory_reply(), or returns 1 and leaves
// {"kind", "message"} there.
EMSCRIPTEN_KEEPALIVE int ng_memory_call(int set, const char* member, const char* method, const char* arguments) {
    try {
        auto* stores = neograph::wasm::find_memory_stores(set);
        if (!stores) throw std::invalid_argument("no in-memory stores under id " + std::to_string(set));
        reply() = neograph::wasm::call_memory_store(*stores, member, method, arguments);
        return 0;
    } catch (const std::exception& error) {
        fail(kind_of(error), error.what());
    } catch (...) {
        fail("runtime_error", "unknown failure");
    }
    return 1;
}

// The names of the methods of one member, as a JSON array, in ng_memory_reply().
EMSCRIPTEN_KEEPALIVE int ng_memory_methods(const char* member) {
    try {
        reply() = neograph::wasm::memory_store_methods(member);
        return 0;
    } catch (const std::exception& error) {
        fail(kind_of(error), error.what());
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char* ng_memory_reply() { return reply().c_str(); }

}  // extern "C"
