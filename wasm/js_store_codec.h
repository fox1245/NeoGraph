// How a Program store value crosses to JavaScript and back, and the methods of the five stores, written once.
//
// The adapters (js_stores.cpp) encode the arguments of a call and decode its result; the reference store
// (memory_stores.cpp) decodes the same arguments and encodes the same result. Both use the codecs and the method rows
// below, so the two directions cannot drift: a row is added here, and the adapter and the reference store both see it.
//
// A value crosses as its canonical text (serialize_canonical(), read back with the type's parse(bytes)). A struct of
// several values is a JSON object whose members are those texts. A string is a string, a counter a number, an absent
// optional null, a list an array, an enum a lower_snake_case string. wasm/README.md has the table.
#pragma once

#include <neograph/json.h>
#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace neograph::wasm {

namespace codec_detail {

[[noreturn]] inline void malformed(const std::string& what) { throw std::invalid_argument(what); }

inline const std::string& expect_string(const json& value, std::string& storage) {
    if (!value.is_string()) malformed("expected a string");
    storage = value.get<std::string>();
    return storage;
}

// The member `key` of an object that has exactly the members in `keys`.
inline json member(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) malformed(std::string("missing member '") + key + "'");
    return object[std::string(key)];
}

inline void expect_members(const json& object, std::initializer_list<const char*> keys) {
    if (!object.is_object()) malformed("expected an object");
    if (object.size() != keys.size()) malformed("unexpected members");
    for (const char* key : keys)
        if (!object.contains(key)) malformed(std::string("missing member '") + key + "'");
}

}  // namespace codec_detail

/// Names of the values of an enum on the wire.
template <class E>
struct EnumNames;

template <>
struct EnumNames<program::ProgramTransitionPublishResult> {
    static constexpr std::pair<program::ProgramTransitionPublishResult, const char*> table[] = {
        {program::ProgramTransitionPublishResult::Published, "published"},
        {program::ProgramTransitionPublishResult::AlreadyPresent, "already_present"},
        {program::ProgramTransitionPublishResult::Conflict, "conflict"},
    };
};
template <>
struct EnumNames<program::ProgramActivationResult> {
    static constexpr std::pair<program::ProgramActivationResult, const char*> table[] = {
        {program::ProgramActivationResult::Activated, "activated"},
        {program::ProgramActivationResult::AlreadyPresent, "already_present"},
        {program::ProgramActivationResult::Conflict, "conflict"},
    };
};
template <>
struct EnumNames<program::ProgramCoreToolGrantAdmission> {
    static constexpr std::pair<program::ProgramCoreToolGrantAdmission, const char*> table[] = {
        {program::ProgramCoreToolGrantAdmission::Admitted, "admitted"},
        {program::ProgramCoreToolGrantAdmission::AlreadyPresent, "already_present"},
        {program::ProgramCoreToolGrantAdmission::Conflict, "conflict"},
    };
};
template <>
struct EnumNames<program::TaskGraphPublishResult> {
    static constexpr std::pair<program::TaskGraphPublishResult, const char*> table[] = {
        {program::TaskGraphPublishResult::Published, "published"},
        {program::TaskGraphPublishResult::AlreadyPresent, "already_present"},
        {program::TaskGraphPublishResult::Conflict, "conflict"},
        {program::TaskGraphPublishResult::Missing, "missing"},
    };
};
template <>
struct EnumNames<program::ModuleLifecycle> {
    static constexpr std::pair<program::ModuleLifecycle, const char*> table[] = {
        {program::ModuleLifecycle::Active, "active"},
        {program::ModuleLifecycle::Deprecated, "deprecated"},
        {program::ModuleLifecycle::Quarantined, "quarantined"},
        {program::ModuleLifecycle::Revoked, "revoked"},
    };
};

/// A value of type T on the wire. `Owned` is what decode() returns: T itself, unless T only views storage.
template <class T, class = void>
struct Codec;

// A value with a canonical text.
template <class T>
concept CanonicalValue = requires(const T& value, std::string_view bytes) {
    { value.serialize_canonical() } -> std::convertible_to<std::string>;
    { T::parse(bytes) } -> std::same_as<T>;
};

template <CanonicalValue T>
struct Codec<T> {
    using Owned = T;
    static json encode(const T& value) { return json(std::string(value.serialize_canonical())); }
    static T decode(const json& value) {
        std::string storage;
        return T::parse(codec_detail::expect_string(value, storage));
    }
};

template <>
struct Codec<std::string> {
    using Owned = std::string;
    static json encode(const std::string& value) { return json(value); }
    static std::string decode(const json& value) {
        std::string storage;
        return codec_detail::expect_string(value, storage);
    }
};

template <>
struct Codec<std::string_view> {
    using Owned = std::string;
    // A default string_view has a null data pointer, which json(string_view) would take for null: copy it.
    static json encode(std::string_view value) { return json(std::string(value)); }
    static std::string decode(const json& value) { return Codec<std::string>::decode(value); }
};

template <>
struct Codec<std::uint64_t> {
    using Owned = std::uint64_t;
    static json encode(std::uint64_t value) { return json(static_cast<unsigned long long>(value)); }
    static std::uint64_t decode(const json& value) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) codec_detail::malformed("expected an integer");
        if (value.is_number_integer() && !value.is_number_unsigned() && value.get<long long>() < 0)
            codec_detail::malformed("expected a non-negative integer");
        return value.get<unsigned long long>();
    }
};

template <>
struct Codec<bool> {
    using Owned = bool;
    static json encode(bool value) { return json(value); }
    static bool decode(const json& value) {
        if (!value.is_boolean()) codec_detail::malformed("expected a boolean");
        return value.get<bool>();
    }
};

template <class E>
    requires requires { EnumNames<E>::table; }
struct Codec<E> {
    using Owned = E;
    static json encode(E value) {
        for (const auto& [candidate, name] : EnumNames<E>::table)
            if (candidate == value) return json(name);
        codec_detail::malformed("unknown enum value");
    }
    static E decode(const json& value) {
        std::string storage;
        const auto& name = codec_detail::expect_string(value, storage);
        for (const auto& [candidate, text] : EnumNames<E>::table)
            if (name == text) return candidate;
        codec_detail::malformed("unknown value '" + name + "'");
    }
};

template <class T>
struct Codec<std::optional<T>> {
    using Owned = std::optional<typename Codec<T>::Owned>;
    static json encode(const std::optional<T>& value) { return value ? Codec<T>::encode(*value) : json(nullptr); }
    static Owned decode(const json& value) {
        if (value.is_null()) return std::nullopt;
        return Owned(Codec<T>::decode(value));
    }
};

template <class T>
struct Codec<std::vector<T>> {
    using Owned = std::vector<typename Codec<T>::Owned>;
    static json encode(const std::vector<T>& values) {
        json out = json::array();
        for (const auto& value : values) out.push_back(Codec<T>::encode(value));
        return out;
    }
    static Owned decode(const json& value) {
        if (!value.is_array()) codec_detail::malformed("expected an array");
        Owned out;
        out.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index) out.push_back(Codec<T>::decode(value[index]));
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Structs of several values.

template <>
struct Codec<program::ProgramRunPublicationHead> {
    using Owned = program::ProgramRunPublicationHead;
    static json encode(const Owned& value) {
        json out = json::object();
        out["run_record"]     = Codec<program::ProgramRunRecord>::encode(value.run_record);
        out["journal_record"] = Codec<program::ProgramJournalRecord>::encode(value.journal_record);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"run_record", "journal_record"});
        return Owned{Codec<program::ProgramRunRecord>::decode(value["run_record"]),
                     Codec<program::ProgramJournalRecord>::decode(value["journal_record"])};
    }
};

template <>
struct Codec<program::ProgramCommandPublicationHead> {
    using Owned = program::ProgramCommandPublicationHead;
    static json encode(const Owned& value) {
        json out = json::object();
        out["run_record"]     = Codec<program::ProgramRunRecord>::encode(value.run_record);
        out["journal_record"] = Codec<program::ProgramJournalRecord>::encode(value.journal_record);
        out["latest_command"] =
            Codec<std::optional<program::ProgramJavaScriptCommandJournalEntry>>::encode(value.latest_command);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"run_record", "journal_record", "latest_command"});
        return Owned{
            Codec<program::ProgramRunRecord>::decode(value["run_record"]),
            Codec<program::ProgramJournalRecord>::decode(value["journal_record"]),
            Codec<std::optional<program::ProgramJavaScriptCommandJournalEntry>>::decode(value["latest_command"])};
    }
};

template <>
struct Codec<program::ProgramContextPublication> {
    using Owned = program::ProgramContextPublication;
    static json encode(const Owned& value) {
        json out = json::object();
        out["epoch"]            = Codec<ContextEpoch>::encode(value.epoch);
        out["artifacts"]        = Codec<std::vector<ContextArtifact>>::encode(value.artifacts);
        out["assembly_receipt"] = json(value.assembly_receipt.serialize_canonical());
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"epoch", "artifacts", "assembly_receipt"});
        // The receipt is read against the epoch and the artifacts it was assembled from, so they come first.
        auto epoch     = Codec<ContextEpoch>::decode(value["epoch"]);
        auto artifacts = Codec<std::vector<ContextArtifact>>::decode(value["artifacts"]);
        std::string storage;
        auto        receipt = ContextAssemblyReceipt::parse(codec_detail::expect_string(value["assembly_receipt"], storage),
                                                            epoch, artifacts);
        return Owned{std::move(epoch), std::move(artifacts), std::move(receipt)};
    }
};

template <>
struct Codec<program::ProgramEffectiveRuntimeState> {
    using Owned = program::ProgramEffectiveRuntimeState;
    static json encode(const Owned& value) {
        json out = json::object();
        out["context_publications"] =
            Codec<std::vector<program::ProgramContextPublication>>::encode(value.context_publications);
        out["hook_outbox_entries"] = Codec<std::vector<HookOutboxEntry>>::encode(value.hook_outbox_entries);
        out["inherited_hook_outbox_entries"] =
            Codec<std::vector<HookOutboxEntry>>::encode(value.inherited_hook_outbox_entries);
        out["transfer_receipt"] =
            Codec<std::optional<program::ProgramRuntimeStateTransferReceipt>>::encode(value.transfer_receipt);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"context_publications", "hook_outbox_entries",
                                             "inherited_hook_outbox_entries", "transfer_receipt"});
        Owned out;
        out.context_publications =
            Codec<std::vector<program::ProgramContextPublication>>::decode(value["context_publications"]);
        out.hook_outbox_entries = Codec<std::vector<HookOutboxEntry>>::decode(value["hook_outbox_entries"]);
        out.inherited_hook_outbox_entries =
            Codec<std::vector<HookOutboxEntry>>::decode(value["inherited_hook_outbox_entries"]);
        out.transfer_receipt =
            Codec<std::optional<program::ProgramRuntimeStateTransferReceipt>>::decode(value["transfer_receipt"]);
        return out;
    }
};

template <>
struct Codec<program::ProgramActivationBinding> {
    using Owned = program::ProgramActivationBinding;
    static json encode(const Owned& value) {
        json out = json::object();
        out["activation"] = Codec<program::ProgramActivation>::encode(value.activation);
        out["version"]    = Codec<program::ProgramVersion>::encode(value.version);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"activation", "version"});
        return Owned{Codec<program::ProgramActivation>::decode(value["activation"]),
                     Codec<program::ProgramVersion>::decode(value["version"])};
    }
};

template <>
struct Codec<program::ProgramRetentionReference> {
    using Owned = program::ProgramRetentionReference;
    static json encode(const Owned& value) {
        json out = json::object();
        out["version_id"] = json(value.version_id);
        out["reason"]     = json(value.reason);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"version_id", "reason"});
        return Owned{Codec<std::string>::decode(value["version_id"]), Codec<std::string>::decode(value["reason"])};
    }
};

template <>
struct Codec<program::ProgramRetentionReport> {
    using Owned = program::ProgramRetentionReport;
    static json encode(const Owned& value) {
        json out = json::object();
        out["versions_removed"] = Codec<std::uint64_t>::encode(value.versions_removed);
        out["bundles_removed"]  = Codec<std::uint64_t>::encode(value.bundles_removed);
        out["references"]       = Codec<std::vector<program::ProgramRetentionReference>>::encode(value.references);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"versions_removed", "bundles_removed", "references"});
        Owned out;
        out.versions_removed = Codec<std::uint64_t>::decode(value["versions_removed"]);
        out.bundles_removed  = Codec<std::uint64_t>::decode(value["bundles_removed"]);
        out.references = Codec<std::vector<program::ProgramRetentionReference>>::decode(value["references"]);
        return out;
    }
};

template <>
struct Codec<program::ProgramCoreToolGrantRecord> {
    using Owned = program::ProgramCoreToolGrantRecord;
    static json encode(const Owned& value) {
        json out = json::object();
        out["owner_scope"]         = json(value.owner_scope);
        out["program_version_id"]  = json(value.program_version_id);
        out["run_id"]              = json(value.run_id);
        out["operation_id"]        = json(value.operation_id);
        out["attempt"]             = Codec<std::uint64_t>::encode(value.attempt);
        out["binding_fingerprint"] = json(value.binding_fingerprint);
        out["grant_id"]            = json(value.grant_id);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"owner_scope", "program_version_id", "run_id", "operation_id",
                                             "attempt", "binding_fingerprint", "grant_id"});
        Owned out;
        out.owner_scope         = Codec<std::string>::decode(value["owner_scope"]);
        out.program_version_id  = Codec<std::string>::decode(value["program_version_id"]);
        out.run_id              = Codec<std::string>::decode(value["run_id"]);
        out.operation_id        = Codec<std::string>::decode(value["operation_id"]);
        out.attempt             = Codec<std::uint64_t>::decode(value["attempt"]);
        out.binding_fingerprint = Codec<std::string>::decode(value["binding_fingerprint"]);
        out.grant_id            = Codec<std::string>::decode(value["grant_id"]);
        return out;
    }
};

/// A grant context that owns its strings; it converts to the view the store takes.
struct OwnedGrantContext {
    std::string   owner_scope, program_version_id, run_id, operation_id;
    std::uint64_t attempt = 0;
    std::string   binding_fingerprint;
    operator program::ProgramCoreToolGrantContext() const {
        return {owner_scope, program_version_id, run_id, operation_id, attempt, binding_fingerprint};
    }
};

template <>
struct Codec<program::ProgramCoreToolGrantContext> {
    using Owned = OwnedGrantContext;
    static json encode(const program::ProgramCoreToolGrantContext& value) {
        json out = json::object();
        out["owner_scope"]         = json(value.owner_scope);
        out["program_version_id"]  = json(value.program_version_id);
        out["run_id"]              = json(value.run_id);
        out["operation_id"]        = json(value.operation_id);
        out["attempt"]             = Codec<std::uint64_t>::encode(value.attempt);
        out["binding_fingerprint"] = json(value.binding_fingerprint);
        return out;
    }
    static Owned decode(const json& value) {
        codec_detail::expect_members(value, {"owner_scope", "program_version_id", "run_id", "operation_id",
                                             "attempt", "binding_fingerprint"});
        Owned out;
        out.owner_scope         = Codec<std::string>::decode(value["owner_scope"]);
        out.program_version_id  = Codec<std::string>::decode(value["program_version_id"]);
        out.run_id              = Codec<std::string>::decode(value["run_id"]);
        out.operation_id        = Codec<std::string>::decode(value["operation_id"]);
        out.attempt             = Codec<std::uint64_t>::decode(value["attempt"]);
        out.binding_fingerprint = Codec<std::string>::decode(value["binding_fingerprint"]);
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Arguments and results.

/// The canonical text of an argument that the caller has already produced (and checked): it crosses as it is.
struct PreparedText {
    std::string_view text;
};

template <class A, class Given>
json encode_argument(const Given& given) {
    if constexpr (std::is_same_v<Given, PreparedText>) {
        return json(std::string(given.text));
    } else {
        return Codec<A>::encode(static_cast<const A&>(given));
    }
}

/// The arguments of a call as a JSON array text, in the order of the C++ operation.
template <class... A, class... Given>
std::string encode_arguments(const Given&... given) {
    json out = json::array();
    (out.push_back(encode_argument<A>(given)), ...);
    return out.dump();
}

template <class... A, std::size_t... I>
std::tuple<typename Codec<A>::Owned...> decode_arguments_at(const json& array, std::index_sequence<I...>) {
    return std::tuple<typename Codec<A>::Owned...>{Codec<A>::decode(array[I])...};
}

/// The arguments of a call read from a JSON array, in the order of the C++ operation.
template <class... A>
std::tuple<typename Codec<A>::Owned...> decode_arguments(const json& array) {
    if (!array.is_array() || array.size() != sizeof...(A))
        throw std::invalid_argument("expected " + std::to_string(sizeof...(A)) + " arguments");
    return decode_arguments_at<A...>(array, std::index_sequence_for<A...>{});
}

// ---------------------------------------------------------------------------------------------------------------
// The methods of the five stores.
//
// X(row, cxx, js, required, Result, Arguments...)
//   row       the name of the constant below; it is the C++ operation, with `_owned` for the owner-qualified overload
//   cxx       the C++ operation; the reference store calls `cxx##_sync` on the store
//   js        the name of the method on the JavaScript object. An overload pair shares one name and is told apart by
//             the number of arguments
//   required  true when the C++ operation is pure virtual: a JavaScript object must have the method
//   Result    the C++ result type (void for none)
// What is not in a table is not supported: compare_publish_graph_safe_point carries evidence that is never
// serialized, so a JavaScript store answers Conflict for it, as the base class does.

#define NEOGRAPH_JS_TRANSITION_OPS(X)                                                                                  \
    X(load, load, "load", true, std::optional<program::ProgramRunRecord>, std::string_view, std::string_view)          \
    X(latest, latest, "latest", true, std::optional<program::ProgramJournalRecord>, std::string_view, std::string_view) \
    X(load_run_publication_head, load_run_publication_head, "loadRunPublicationHead", false,                          \
      std::optional<program::ProgramRunPublicationHead>, std::string_view, std::string_view)                          \
    X(load_events, load_events, "loadEvents", true, std::vector<program::ProgramEvent>, std::string_view,             \
      std::string_view, std::uint64_t)                                                                                 \
    X(load_effects, load_effects, "loadEffects", true, std::vector<program::ProgramEffectOutboxEntry>,                \
      std::string_view, std::string_view, std::uint64_t)                                                               \
    X(load_javascript_commands, load_javascript_commands, "loadJavascriptCommands", false,                            \
      std::vector<program::ProgramJavaScriptCommandJournalEntry>, std::string_view, std::string_view, std::uint64_t)  \
    X(load_command_publication_head, load_command_publication_head, "loadCommandPublicationHead", false,              \
      std::optional<program::ProgramCommandPublicationHead>, std::string_view, std::string_view)                      \
    X(load_context_publications, load_context_publications, "loadContextPublications", false,                         \
      std::vector<program::ProgramContextPublication>, std::string_view, std::string_view, std::uint64_t)             \
    X(load_hook_outbox_entries, load_hook_outbox_entries, "loadHookOutboxEntries", false,                             \
      std::vector<HookOutboxEntry>, std::string_view, std::string_view)                                                \
    X(load_effective_runtime_state, load_effective_runtime_state, "loadEffectiveRuntimeState", false,                 \
      program::ProgramEffectiveRuntimeState, std::string_view, std::string_view)                                       \
    X(load_child_syntheses, load_child_syntheses, "loadChildSyntheses", false,                                        \
      std::vector<program::ProgramChildSynthesisRecord>, std::string_view, std::string_view)                           \
    X(load_migration_plan, load_migration_plan, "loadMigrationPlan", false, std::optional<program::MigrationPlan>,    \
      std::string_view, std::string_view)                                                                              \
    X(load_lineage, load_lineage, "loadLineage", false, std::optional<program::ProgramRunLineage>, std::string_view,  \
      std::string_view)                                                                                                \
    X(load_run_lineage, load_run_lineage, "loadRunLineage", false, std::optional<program::ProgramRunLineage>,        \
      std::string_view, std::string_view)                                                                              \
    X(load_lineage_head, load_lineage_head, "loadLineageHead", false, std::optional<program::ProgramRunLineage>,      \
      std::string_view, std::string_view, std::string_view)                                                            \
    X(load_generation, load_generation, "loadGeneration", false, std::optional<program::ProgramRunGeneration>,        \
      std::string_view, std::string_view, std::uint64_t)                                                               \
    X(load_generation_initial_publication, load_generation_initial_publication, "loadGenerationInitialPublication",   \
      false, std::optional<program::ProgramTransitionPublication>, std::string_view, std::string_view, std::uint64_t) \
    X(load_graph_migration_capsule, load_graph_migration_capsule, "loadGraphMigrationCapsule", false,                 \
      std::optional<program::GraphMigrationCapsule>, std::string_view, std::string_view, std::string_view)            \
    X(load_execution_lease, load_execution_lease, "loadExecutionLease", false,                                        \
      std::optional<program::ProgramExecutionLease>, std::string_view, std::string_view)                              \
    X(compare_publish, compare_publish, "comparePublish", true, program::ProgramTransitionPublishResult,              \
      std::string_view, std::string_view, std::uint64_t, program::ProgramTransitionPublication)                        \
    X(compare_publish_execution, compare_publish_execution, "comparePublishExecution", false,                         \
      program::ProgramTransitionPublishResult, std::string_view, std::string_view, std::uint64_t,                      \
      program::ProgramTransitionPublication, std::optional<program::ProgramExecutionLease>,                            \
      std::optional<program::ProgramExecutionLease>)                                                                   \
    X(compare_publish_takeover, compare_publish_takeover, "comparePublishTakeover", true,                             \
      program::ProgramTransitionPublishResult, std::string_view, std::string_view, std::string_view, std::uint64_t)

#define NEOGRAPH_JS_PROGRAM_OPS(X)                                                                                     \
    X(publish_admitted, publish_admitted, "publishAdmitted", true, void, program::ProgramBundle,                      \
      program::ProgramVersion)                                                                                         \
    X(get_bundle, get_bundle, "getBundle", true, std::optional<program::ProgramBundle>, std::string_view)             \
    X(get_version, get_version, "getVersion", true, std::optional<program::ProgramVersion>, std::string_view)         \
    X(get_bundle_owned, get_bundle, "getBundle", false, std::optional<program::ProgramBundle>, std::string_view,      \
      std::string_view)                                                                                                \
    X(get_version_owned, get_version, "getVersion", false, std::optional<program::ProgramVersion>, std::string_view,  \
      std::string_view)                                                                                                \
    X(get_activation, get_activation, "getActivation", false, std::optional<program::ProgramActivation>,             \
      std::string_view)                                                                                                \
    X(get_active_binding, get_active_binding, "getActiveBinding", false,                                              \
      std::optional<program::ProgramActivationBinding>, std::string_view)                                              \
    X(compare_activate, compare_activate, "compareActivate", false, program::ProgramActivationResult,                 \
      std::string_view, std::uint64_t, std::string_view, std::string_view)                                             \
    X(list_versions, list_versions, "listVersions", false, std::vector<program::ProgramVersion>, std::string_view)    \
    X(collect_garbage, collect_garbage, "collectGarbage", false, program::ProgramRetentionReport, std::string_view,   \
      std::vector<std::string>)

#define NEOGRAPH_JS_GRANT_OPS(X)                                                                                       \
    X(admit, admit, "admit", true, program::ProgramCoreToolGrantAdmission, program::ProgramCoreToolGrantRecord)       \
    X(load, load, "load", true, std::optional<program::ProgramCoreToolGrantRecord>,                                   \
      program::ProgramCoreToolGrantContext)                                                                            \
    X(revoke, revoke, "revoke", true, bool, program::ProgramCoreToolGrantContext)

#define NEOGRAPH_JS_FRAGMENT_OPS(X)                                                                                    \
    X(publish, publish, "publish", true, program::TaskGraphPublishResult, program::TaskGraphFragmentRecord)           \
    X(load, load, "load", true, std::optional<program::TaskGraphFragmentRecord>, std::string_view)                    \
    X(find_published_lineage, find_published_lineage, "findPublishedLineage", false,                                  \
      std::optional<program::TaskGraphFragmentRecord>, std::string_view, std::string_view, std::string_view)          \
    X(find_published, find_published, "findPublished", false, std::optional<program::TaskGraphFragmentRecord>,        \
      std::string_view, std::string_view, std::string_view, std::string_view)                                          \
    X(compare_update, compare_update, "compareUpdate", true, program::TaskGraphPublishResult, std::string_view,       \
      std::uint64_t, program::TaskGraphFragmentRecord)

#define NEOGRAPH_JS_MODULE_OPS(X)                                                                                      \
    X(publish, publish, "publish", true, void, program::ProgramModule)                                                \
    X(get, get, "get", true, std::optional<program::ProgramModule>, std::string_view)                                 \
    X(get_owned, get, "get", false, std::optional<program::ProgramModule>, std::string_view, std::string_view)        \
    X(set_lifecycle, set_lifecycle, "setLifecycle", true, void, std::string_view, program::ModuleLifecycle)

/// One method of a store: the JavaScript name, whether the object must have it, and the types that cross.
template <class R, class... A>
struct Op {
    using Result = R;
    const char* method;
    bool        required;
    static constexpr std::size_t arity = sizeof...(A);
};

/// The argument text of a call of `op`.
template <class R, class... A, class... Given>
std::string encode_call(const Op<R, A...>&, const Given&... given) {
    return encode_arguments<A...>(given...);
}

/// What `adapt` needs to know about each row of a table.
struct OpInfo {
    const char* method;
    bool        required;
};

#define NEOGRAPH_JS_OP_CONSTANT(row, cxx, js, required, R, ...) \
    inline constexpr ::neograph::wasm::Op<R, __VA_ARGS__> row{js, required};
#define NEOGRAPH_JS_OP_INFO(row, cxx, js, required, R, ...) ::neograph::wasm::OpInfo{js, required},

namespace transition_ops {
NEOGRAPH_JS_TRANSITION_OPS(NEOGRAPH_JS_OP_CONSTANT)
inline constexpr OpInfo all[] = {NEOGRAPH_JS_TRANSITION_OPS(NEOGRAPH_JS_OP_INFO)};
}  // namespace transition_ops
namespace program_ops {
NEOGRAPH_JS_PROGRAM_OPS(NEOGRAPH_JS_OP_CONSTANT)
inline constexpr OpInfo all[] = {NEOGRAPH_JS_PROGRAM_OPS(NEOGRAPH_JS_OP_INFO)};
}  // namespace program_ops
namespace grant_ops {
NEOGRAPH_JS_GRANT_OPS(NEOGRAPH_JS_OP_CONSTANT)
inline constexpr OpInfo all[] = {NEOGRAPH_JS_GRANT_OPS(NEOGRAPH_JS_OP_INFO)};
}  // namespace grant_ops
namespace fragment_ops {
NEOGRAPH_JS_FRAGMENT_OPS(NEOGRAPH_JS_OP_CONSTANT)
inline constexpr OpInfo all[] = {NEOGRAPH_JS_FRAGMENT_OPS(NEOGRAPH_JS_OP_INFO)};
}  // namespace fragment_ops
namespace module_ops {
NEOGRAPH_JS_MODULE_OPS(NEOGRAPH_JS_OP_CONSTANT)
inline constexpr OpInfo all[] = {NEOGRAPH_JS_MODULE_OPS(NEOGRAPH_JS_OP_INFO)};
}  // namespace module_ops

}  // namespace neograph::wasm
