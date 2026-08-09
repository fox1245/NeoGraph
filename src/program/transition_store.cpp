#include <neograph/program/transition_store.h>

#include "canonical_json.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
namespace neograph::program {
namespace {
constexpr std::string_view EFFECT_FORMAT      = "neograph-program-effect-outbox-entry",
                           PUBLICATION_FORMAT = "neograph-program-transition-publication";
bool is_final(ContinuationState state) noexcept {
    return state != ContinuationState::Running && state != ContinuationState::Interrupted &&
           state != ContinuationState::AmbiguousEffect;
}

bool same_child_metadata(const ProgramChildRecord& lhs, const ProgramChildRecord& rhs) {
    return lhs.child_run_id == rhs.child_run_id && lhs.link_id == rhs.link_id &&
           lhs.link_receipt == rhs.link_receipt && lhs.invocation == rhs.invocation;
}

bool valid_child_state_transition(ProgramChildState previous, ProgramChildState next) noexcept {
    if (previous == next) return true;
    if (previous == ProgramChildState::Publishing &&
        (next == ProgramChildState::Dispatched || next == ProgramChildState::Completed ||
         next == ProgramChildState::Cancelled || next == ProgramChildState::Failed))
        return true;
    if (previous == ProgramChildState::Dispatched &&
        (next == ProgramChildState::Completed || next == ProgramChildState::Cancelled ||
         next == ProgramChildState::Failed))
        return true;
    return false;
}

bool valid_children_transition(const ProgramRunRecord& previous,
                               const ProgramRunRecord& next) {
    const auto previous_children = previous.children();
    const auto next_children     = next.children();
    for (const auto& child : previous_children) {
        const auto found = std::find_if(next_children.begin(), next_children.end(),
                                        [&](const auto& value) {
                                            return value.child_run_id == child.child_run_id;
                                        });
        if (found == next_children.end() || !same_child_metadata(child, *found) ||
            !valid_child_state_transition(child.state, found->state))
            return false;
    }
    return true;
}

bool valid_effect_outbox_binding(const ProgramRunRecord& run,
                                 const std::vector<ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;

    // A Program has one durable pending operation at a time.  An outbox entry
    // is therefore valid only when it is the exact immutable effect that left
    // the run interrupted; a caller cannot smuggle a second effect through a
    // transition publication.
    const auto pending = run.pending_effect();
    if (!pending || pending->state() != ProgramPendingState::Awaiting || effects.size() != 1 ||
        effects.front().effect() != *pending) {
        return false;
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& effect : effects) {
        if (!ids.emplace(effect.effect().effect_id()).second) return false;
    }
    return true;
}

struct ProgramTransitionHistory final {
    ProgramTransitionHistory(std::shared_ptr<const ProgramTransitionHistory> previous_history,
                             std::vector<ProgramEvent>                     appended_events,
                             std::vector<ProgramEffectOutboxEntry>         appended_effects)
        : previous(std::move(previous_history)), events(std::move(appended_events)),
          effects(std::move(appended_effects)),
          event_count((previous ? previous->event_count : 0) + events.size()),
          effect_count((previous ? previous->effect_count : 0) + effects.size()) {}

    std::shared_ptr<const ProgramTransitionHistory> previous;
    std::vector<ProgramEvent>                       events;
    std::vector<ProgramEffectOutboxEntry>           effects;
    std::size_t                                      event_count;
    std::size_t                                      effect_count;
};

using ProgramTransitionHistoryPtr = std::shared_ptr<const ProgramTransitionHistory>;

constexpr std::size_t TRANSITION_HISTORY_CHUNK_CAPACITY = 32;

template <typename Entry>
std::vector<Entry> append_entries(const std::vector<Entry>& existing,
                                  std::vector<Entry>        additions) {
    if (existing.empty()) return additions;

    std::vector<Entry> entries;
    entries.reserve(existing.size() + additions.size());
    entries.insert(entries.end(), existing.begin(), existing.end());
    entries.insert(entries.end(), std::make_move_iterator(additions.begin()),
                   std::make_move_iterator(additions.end()));
    return entries;
}

ProgramTransitionHistoryPtr append_history(
    ProgramTransitionHistoryPtr                    previous,
    std::vector<ProgramEvent>                      events,
    std::vector<ProgramEffectOutboxEntry>          effects) {
    if (events.empty() && effects.empty()) return previous;
    if (!previous ||
        previous->events.size() + events.size() > TRANSITION_HISTORY_CHUNK_CAPACITY ||
        previous->effects.size() + effects.size() > TRANSITION_HISTORY_CHUNK_CAPACITY) {
        return std::make_shared<const ProgramTransitionHistory>(
            std::move(previous), std::move(events), std::move(effects));
    }
    return std::make_shared<const ProgramTransitionHistory>(
        previous->previous, append_entries(previous->events, std::move(events)),
        append_entries(previous->effects, std::move(effects)));
}

ProgramTransitionHistoryPtr begin_history(
    const std::vector<ProgramEvent>&               events,
    const std::vector<ProgramEffectOutboxEntry>&   effects,
    std::vector<ProgramEvent>                      appended_events,
    std::vector<ProgramEffectOutboxEntry>          appended_effects) {
    return std::make_shared<const ProgramTransitionHistory>(
        nullptr, append_entries(events, std::move(appended_events)),
        append_entries(effects, std::move(appended_effects)));
}

template <typename Entry, typename Sequence>
std::vector<Entry> entries_after(const std::vector<Entry>& entries,
                                 std::uint64_t             after_sequence,
                                 Sequence&&                sequence) {
    if (entries.empty() || after_sequence >= sequence(entries.back())) return {};

    std::vector<Entry> result;
    if (!after_sequence) result.reserve(entries.size());
    for (const auto& entry : entries) {
        if (sequence(entry) > after_sequence) result.push_back(entry);
    }
    return result;
}

std::vector<ProgramEvent> event_entries_after(const ProgramTransitionHistoryPtr& history,
                                              std::uint64_t after_sequence) {
    if (!history || !history->event_count) return {};

    std::vector<ProgramEvent> result;
    if (!after_sequence) result.reserve(history->event_count);
    for (auto current = history; current; current = current->previous) {
        for (auto event = current->events.rbegin(); event != current->events.rend(); ++event) {
            if (event->sequence > after_sequence) result.push_back(*event);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<ProgramEffectOutboxEntry>
effect_entries_after(const ProgramTransitionHistoryPtr& history,
                     std::uint64_t                      after_sequence) {
    if (!history || !history->effect_count) return {};

    std::vector<ProgramEffectOutboxEntry> result;
    if (!after_sequence) result.reserve(history->effect_count);
    for (auto current = history; current; current = current->previous) {
        for (auto effect = current->effects.rbegin(); effect != current->effects.rend(); ++effect) {
            if (effect->sequence() > after_sequence) result.push_back(*effect);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

bool effect_history_contains(const std::vector<ProgramEffectOutboxEntry>& effects,
                             const ProgramTransitionHistoryPtr&            history,
                             std::string_view                              id) {
    if (std::any_of(effects.begin(), effects.end(), [&](const auto& existing) {
            return existing.effect().effect_id() == id;
        })) {
        return true;
    }
    for (auto current = history; current; current = current->previous) {
        if (std::any_of(current->effects.begin(), current->effects.end(),
                        [&](const auto& existing) {
                            return existing.effect().effect_id() == id;
                        })) {
            return true;
        }
    }
    return false;
}

bool effect_ids_are_unique(const std::vector<ProgramEffectOutboxEntry>& old_effects,
                           const ProgramTransitionHistoryPtr&            old_history,
                           const std::vector<ProgramEffectOutboxEntry>&  new_effects) {
    for (auto next = new_effects.begin(); next != new_effects.end(); ++next) {
        if (std::any_of(new_effects.begin(), next, [&](const auto& existing) {
                return existing.effect().effect_id() == next->effect().effect_id();
            }) ||
            effect_history_contains(old_effects, old_history, next->effect().effect_id())) {
            return false;
        }
    }
    return true;
}

bool same_command_coordinate(const ProgramJavaScriptCommandJournalEntry& lhs,
                             const ProgramJavaScriptCommandJournalEntry& rhs) {
    return lhs.bundle_id() == rhs.bundle_id() &&
           lhs.command_ordinal() == rhs.command_ordinal() &&
           lhs.coordinate_id() == rhs.coordinate_id() &&
           detail::canonical_json_bytes(lhs.command().to_json()) ==
               detail::canonical_json_bytes(rhs.command().to_json()) &&
           lhs.effect_identity() == rhs.effect_identity();
}

bool valid_command_history_append(
    const ProgramRunRecord&                                  run,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& new_commands) {
    if (new_commands.empty()) return true;

    auto prior = old_commands;
    std::uint64_t expected_sequence = 0;
    std::uint64_t highest_ordinal = 0;
    for (const auto& existing : prior) {
        if (existing.bundle_id() != run.bundle_id() || existing.sequence() != expected_sequence + 1)
            return false;
        ++expected_sequence;
        highest_ordinal = std::max(highest_ordinal, existing.command_ordinal());
    }
    for (const auto& entry : new_commands) {
        if (entry.bundle_id() != run.bundle_id() ||
            entry.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;

        const auto found = std::find_if(
            prior.rbegin(), prior.rend(), [&](const auto& previous) {
                return previous.command_ordinal() == entry.command_ordinal();
            });
        if (found == prior.rend()) {
            if (entry.command_ordinal() != highest_ordinal + 1 || !entry.pending() ||
                std::any_of(prior.begin(), prior.end(),
                            [](const auto& previous) { return previous.pending(); }))
                return false;
            highest_ordinal = entry.command_ordinal();
            prior.push_back(entry);
            continue;
        }

        // A command coordinate can only move once, from a durable pending
        // head to one authoritative terminal result.  Conflicting command
        // payloads or a second completion are rejected fail-closed.
        if (!found->pending() || !entry.completed() ||
            !same_command_coordinate(*found, entry)) {
            return false;
        }
        prior.push_back(entry);
    }
    return true;
}

std::string rs(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_string())
        throw std::invalid_argument("Program transition field '" + k + "' must be a string");
    return v[k].get<std::string>();
}
std::uint64_t ru(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_number_unsigned())
        throw std::invalid_argument("Program transition field '" + k + "' must be unsigned");
    return v[k].get<std::uint64_t>();
}
std::uint32_t r32(const json& v, std::string_view key) {
    auto n = ru(v, key);
    if (n > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Program transition integer exceeds uint32 range");
    return static_cast<std::uint32_t>(n);
}
json rv(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k))
        throw std::invalid_argument("Program transition requires field '" + k + "'");
    return v[k];
}
json nested(std::string bytes) {
    return detail::parse_json_strict(bytes);
}
std::string key(std::string_view owner, std::string_view run) {
    std::string out;
    out.reserve(owner.size() + run.size() + 1);
    out.append(owner);
    out.push_back('\0');
    out.append(run);
    return out;
}
void validate_pub(const ProgramTransitionPublication& publication, std::string_view owner) {
    // Child serializers validate their immutable identity and canonical body while
    // the publication compositor checks these cross-record invariants.
    const auto& run     = publication.run_record;
    const auto& journal = publication.journal_record;
    if (run.owner_scope() != owner || run.run_id() != journal.run_id ||
        run.program_version_id() != journal.program_version_id ||
        run.bundle_id() != journal.bundle_id || run.journal_head() != journal.id ||
        run.continuation() != journal.continuation ||
        run.remaining_budget() != journal.remaining_budget ||
        run.exact_checkpoint() != journal.core_checkpoint) {
        throw std::invalid_argument("Program snapshot and journal do not match");
    }

    std::uint64_t previous_sequence = 0;
    for (const auto& event : publication.events) {
        if (event.run_id != run.run_id() || event.program_version_id != run.program_version_id() ||
            event.bundle_id != run.bundle_id() ||
            event.sequence <= previous_sequence) {
            throw std::invalid_argument("Program event does not bind snapshot");
        }
        previous_sequence = event.sequence;
    }
    if (!publication.events.empty() && publication.events.back().sequence != run.event_sequence()) {
        throw std::invalid_argument("Program event sequence mismatch");
    }
    if (!publication.events.empty()) {
        const auto& latest = publication.events.back();
        const bool snapshot_operation = latest.kind == ProgramEventKind::Started ||
                                        latest.kind == ProgramEventKind::Terminal;
        if (latest.attempt != run.continuation().attempt ||
            (snapshot_operation && latest.operation_id != run.continuation().operation_id)) {
            throw std::invalid_argument("Latest Program event does not bind snapshot");
        }
        if (latest.kind == ProgramEventKind::CheckpointPublished) {
            const auto& checkpoint = std::get<ProgramCheckpointEvent>(latest.payload).checkpoint;
            if (!run.exact_checkpoint() || checkpoint != *run.exact_checkpoint()) {
                throw std::invalid_argument("Program checkpoint event disagrees with run snapshot");
            }
        }
    }

    previous_sequence = 0;
    for (const auto& effect : publication.effects) {
        if (effect.sequence() <= previous_sequence ||
            effect.effect().operation_id().empty()) {
            throw std::invalid_argument("Program effect does not bind snapshot");
        }
        previous_sequence = effect.sequence();
    }
    std::set<std::string, std::less<>> effect_ids;
    for (const auto& effect : publication.effects) {
        if (!effect_ids.emplace(effect.effect().effect_id()).second) {
            throw std::invalid_argument("Program effect outbox contains a duplicate effect id");
        }
    }
    if (!publication.effects.empty() &&
        publication.effects.back().sequence() != run.effect_sequence()) {
        throw std::invalid_argument("Program effect sequence mismatch");
    }
    previous_sequence = 0;
    for (const auto& command : publication.commands) {
        if (command.bundle_id() != run.bundle_id() ||
            command.sequence() <= previous_sequence) {
            throw std::invalid_argument(
                "JavaScript command journal entry does not bind snapshot");
        }
        previous_sequence = command.sequence();
    }
    if (run.terminal_result() && !publication.events.empty() &&
        publication.events.back().kind == ProgramEventKind::Terminal &&
        std::get<ProgramTerminalEvent>(publication.events.back().payload).status !=
            run.terminal_result()->status()) {
        throw std::invalid_argument("Terminal event/result mismatch");
    }
    if (publication.migration_plan) {
        const auto& plan = *publication.migration_plan;
        const auto fork = run.fork_receipt();
        if (!fork || plan.owner_scope() != owner ||
            plan.target_version_id() != run.program_version_id() ||
            !plan.is_compatible() ||
            plan.source_version_id() != fork->source_program_version_id()) {
            throw std::invalid_argument(
                "Program migration publication requires a compatible fork receipt");
        }
    }
}
constexpr std::size_t MAX_CANONICAL_PUBLICATION_BYTES = 16u * 1024u * 1024u;

void append_publication_bytes(std::string& out, std::string_view bytes) {
    if (bytes.size() > MAX_CANONICAL_PUBLICATION_BYTES - out.size()) {
        throw std::invalid_argument("Program JSON exceeds the 16 MiB materialized limit");
    }
    out.append(bytes);
}

std::string publication_bytes(const ProgramTransitionPublication& publication) {
    std::string bytes;
    bytes.reserve(4096);

    append_publication_bytes(bytes, "{\"commands\":[");
    for (std::size_t index = 0; index < publication.commands.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.commands[index].serialize_canonical());
    }

    append_publication_bytes(bytes, "],\"effects\":[");
    for (std::size_t index = 0; index < publication.effects.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.effects[index].serialize_canonical());
    }

    append_publication_bytes(bytes,
                             "],\"events\":[");
    for (std::size_t index = 0; index < publication.events.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.events[index].serialize_canonical());
    }

    append_publication_bytes(bytes,
                             "],\"format\":\"neograph-program-transition-publication\","
                             "\"journal_record\":");
    append_publication_bytes(bytes, publication.journal_record.serialize_canonical());
    append_publication_bytes(bytes, ",\"migration_plan\":");
    if (publication.migration_plan) {
        append_publication_bytes(bytes, publication.migration_plan->serialize_canonical());
    } else {
        append_publication_bytes(bytes, "null");
    }
    append_publication_bytes(bytes, ",\"run_record\":");
    append_publication_bytes(bytes, publication.run_record.serialize_canonical());
    append_publication_bytes(bytes, ",\"storage_schema_version\":1}");
    return bytes;
}
}  // namespace
struct ProgramEffectOutboxEntry::Impl {
    Impl(std::uint64_t s, ProgramPendingEffect e) : sequence(s), effect(std::move(e)) {
        auto b = json{{"format", std::string(EFFECT_FORMAT)},
                      {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                      {"sequence", sequence},
                      {"effect", nested(effect.serialize_canonical())}};
        id     = detail::sha256_identity("program-effect-outbox-entry/v1",
                                         detail::canonical_json_bytes(b));
    }
    std::uint64_t        sequence;
    ProgramPendingEffect effect;
    std::string          id;
};
ProgramEffectOutboxEntry::ProgramEffectOutboxEntry(std::uint64_t s, ProgramPendingEffect e)
    : ProgramEffectOutboxEntry(std::make_shared<const Impl>(s, std::move(e))) {
    if (!s) throw std::invalid_argument("Program effect outbox sequence must be positive");
}
ProgramEffectOutboxEntry::ProgramEffectOutboxEntry(std::shared_ptr<const Impl> p)
    : impl_(std::move(p)) {}
std::uint64_t ProgramEffectOutboxEntry::sequence() const noexcept {
    return impl_->sequence;
}
const ProgramPendingEffect& ProgramEffectOutboxEntry::effect() const noexcept {
    return impl_->effect;
}
const std::string& ProgramEffectOutboxEntry::id() const noexcept {
    return impl_->id;
}
std::string ProgramEffectOutboxEntry::serialize_canonical() const {
    if (!impl_->sequence) throw std::invalid_argument("Program effect sequence must be positive");
    auto b = json{{"format", std::string(EFFECT_FORMAT)},
                  {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                  {"sequence", impl_->sequence},
                  {"effect", nested(impl_->effect.serialize_canonical())}};
    if (detail::sha256_identity("program-effect-outbox-entry/v1",
                                detail::canonical_json_bytes(b)) != impl_->id)
        throw std::invalid_argument("Program effect outbox id mismatch");
    b["id"] = impl_->id;
    return detail::canonical_json_bytes(b);
}
ProgramEffectOutboxEntry ProgramEffectOutboxEntry::parse(std::string_view bytes) {
    auto v = detail::parse_json_strict(bytes);
    if (!v.is_object() || rs(v, "format") != EFFECT_FORMAT)
        throw std::invalid_argument("Stored Program effect outbox has unknown format");
    detail::reject_unknown_fields(v, "Stored Program effect outbox",
                                  {"format", "storage_schema_version", "id", "sequence", "effect"});
    if (r32(v, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored Program effect schema unsupported");
    auto out = ProgramEffectOutboxEntry(
        ru(v, "sequence"),
        ProgramPendingEffect::parse(detail::canonical_json_bytes(rv(v, "effect"))));
    if (out.id() != rs(v, "id")) throw std::invalid_argument("Stored Program effect id mismatch");
    return out;
}
std::string ProgramTransitionPublication::serialize_canonical() const {
    validate_pub(*this, run_record.owner_scope());
    return publication_bytes(*this);
}
ProgramTransitionPublication ProgramTransitionPublication::parse(std::string_view bytes) {
    auto v = detail::parse_json_strict(bytes);
    if (!v.is_object() || rs(v, "format") != PUBLICATION_FORMAT)
        throw std::invalid_argument("Stored Program publication has unknown format");
    detail::reject_unknown_fields(
        v, "Stored Program publication",
        {"format", "storage_schema_version", "run_record", "journal_record", "events", "effects",
         "commands", "migration_plan"});
    if (r32(v, "storage_schema_version") != 1)
        throw std::invalid_argument("Stored Program publication schema unsupported");
    auto run = ProgramRunRecord::parse(detail::canonical_json_bytes(rv(v, "run_record")));
    auto journal =
        ProgramJournalRecord::parse(detail::canonical_json_bytes(rv(v, "journal_record")));
    std::vector<ProgramEvent> events;
    auto                      ev = rv(v, "events");
    if (!ev.is_array()) throw std::invalid_argument("Program events must be array");
    for (const auto& e : ev)
        events.push_back(ProgramEvent::parse(detail::canonical_json_bytes(e)));
    std::vector<ProgramEffectOutboxEntry> effects;
    auto                                  fx = rv(v, "effects");
    if (!fx.is_array()) throw std::invalid_argument("Program effects must be array");
    for (const auto& e : fx)
        effects.push_back(ProgramEffectOutboxEntry::parse(detail::canonical_json_bytes(e)));
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    if (v.contains("commands")) {
        const auto& encoded_commands = rv(v, "commands");
        if (!encoded_commands.is_array())
            throw std::invalid_argument("Program command journal must be array");
        for (const auto& entry : encoded_commands) {
            commands.push_back(ProgramJavaScriptCommandJournalEntry::parse(
                detail::canonical_json_bytes(entry)));
        }
    }
    std::optional<MigrationPlan> migration_plan;
    if (v.contains("migration_plan")) {
        const auto& plan = rv(v, "migration_plan");
        if (!plan.is_null()) migration_plan = MigrationPlan::parse(detail::canonical_json_bytes(plan));
    }
    ProgramTransitionPublication out{std::move(run), std::move(journal), std::move(events),
                                     std::move(effects), std::move(migration_plan),
                                     std::move(commands)};
    validate_pub(out, out.run_record.owner_scope());
    return out;
}
struct InMemoryProgramTransitionStore::Impl {
    struct Stored {
        ProgramRunRecord                         run;
        ProgramJournalRecord                     journal;
        std::vector<ProgramEvent>                events;
        std::vector<ProgramEffectOutboxEntry>    effects;
        std::vector<ProgramJavaScriptCommandJournalEntry> commands;
        ProgramTransitionHistoryPtr              history;
        std::optional<MigrationPlan>             migration_plan;
        std::string                              bytes;
    };
    mutable std::mutex                                                mutex;
    std::map<std::string, std::shared_ptr<const Stored>, std::less<>> runs;
    std::optional<ProgramTransitionFaultPoint>                        fault;
};
InMemoryProgramTransitionStore::InMemoryProgramTransitionStore()
    : impl_(std::make_unique<Impl>()) {}
InMemoryProgramTransitionStore::~InMemoryProgramTransitionStore() = default;
InMemoryProgramTransitionStore::InMemoryProgramTransitionStore(
    InMemoryProgramTransitionStore&&) noexcept = default;
InMemoryProgramTransitionStore& InMemoryProgramTransitionStore::operator=(
    InMemoryProgramTransitionStore&&) noexcept = default;

void InMemoryProgramTransitionStore::fail_next_publication_for_testing(
    ProgramTransitionFaultPoint point) {
    std::lock_guard lock(impl_->mutex);
    impl_->fault = point;
}
std::optional<ProgramRunRecord> InMemoryProgramTransitionStore::load(std::string_view o,
                                                                     std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->run;
}
std::optional<ProgramJournalRecord> InMemoryProgramTransitionStore::latest(
    std::string_view o, std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->journal;
}
std::vector<ProgramEvent> InMemoryProgramTransitionStore::load_events(std::string_view o,
                                                                      std::string_view r,
                                                                      std::uint64_t    a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    if (snapshot->history) return event_entries_after(snapshot->history, a);
    return entries_after(snapshot->events, a,
                         [](const ProgramEvent& event) { return event.sequence; });
}
std::vector<ProgramEffectOutboxEntry> InMemoryProgramTransitionStore::load_effects(
    std::string_view o, std::string_view r, std::uint64_t a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    if (snapshot->history) return effect_entries_after(snapshot->history, a);
    return entries_after(snapshot->effects, a,
                         [](const ProgramEffectOutboxEntry& effect) {
                         return effect.sequence();
                         });
}
std::vector<ProgramJavaScriptCommandJournalEntry>
InMemoryProgramTransitionStore::load_javascript_commands(std::string_view o,
                                                         std::string_view r,
                                                         std::uint64_t    a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    return entries_after(snapshot->commands, a,
                         [](const ProgramJavaScriptCommandJournalEntry& command) {
                             return command.sequence();
                         });
}
std::optional<MigrationPlan> InMemoryProgramTransitionStore::load_migration_plan(
    std::string_view o, std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->migration_plan;
}
ProgramTransitionPublishResult InMemoryProgramTransitionStore::compare_publish(
    std::string_view owner, std::string_view expected, ProgramTransitionPublication publication) {
    std::string publication_bytes;
    try {
        publication_bytes = publication.serialize_canonical();
        if (!valid_effect_outbox_binding(publication.run_record, publication.effects)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        if (!expected.empty() && !detail::is_sha256_identity(expected)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    } catch (const std::invalid_argument&) {
        return ProgramTransitionPublishResult::Conflict;
    }

    auto            storage_key = key(owner, publication.run_record.run_id());
    std::lock_guard lock(impl_->mutex);
    auto            current = impl_->runs.find(storage_key);
    if (current != impl_->runs.end() && current->second->bytes == publication_bytes) {
        return expected == publication.journal_record.previous_id
                   ? ProgramTransitionPublishResult::AlreadyPresent
                   : ProgramTransitionPublishResult::Conflict;
    }

    if (current != impl_->runs.end() && publication.migration_plan) {
        const auto& old_plan = current->second->migration_plan;
        if (!old_plan || old_plan->id() != publication.migration_plan->id())
            return ProgramTransitionPublishResult::Conflict;
    }

    const auto new_terminal = publication.run_record.terminal_result();
    const bool publishes_terminal_event =
        !publication.events.empty() && publication.events.back().kind == ProgramEventKind::Terminal;

    if (current == impl_->runs.end()) {
        if (!expected.empty() || !publication.journal_record.previous_id.empty() ||
            publication.journal_record.sequence != 1 || publication.events.empty() ||
            publication.events.front().kind != ProgramEventKind::Started ||
            publication.run_record.event_sequence() != publication.events.size() ||
            publication.run_record.effect_sequence() != publication.effects.size() ||
            publication.events.front().sequence != 1 ||
            (!publication.effects.empty() && publication.effects.front().sequence() != 1) ||
            (new_terminal && !publishes_terminal_event) ||
            (publication.run_record.fork_receipt() && !publication.migration_plan)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    } else {
        const auto& old          = *current->second;
        const auto  old_terminal = old.run.terminal_result();
        const auto  old_fork     = old.run.fork_receipt();
        const auto  new_fork     = publication.run_record.fork_receipt();
        const bool  same_fork    = old_fork.has_value() == new_fork.has_value() &&
                               (!old_fork || old_fork->id() == new_fork->id());
        if ((old_terminal && is_final(old.run.continuation().state) && !new_terminal) ||
            (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
             !publishes_terminal_event)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        // A publication may update durable run metadata (such as a child
        // relationship) without advancing the event or effect streams.
        if (old.journal.id != expected || publication.journal_record.previous_id != expected ||
            !is_valid_program_journal_transition(old.journal, publication.journal_record) ||
            publication.run_record.created_at_ms() != old.run.created_at_ms() ||
            publication.run_record.updated_at_ms() < old.run.updated_at_ms() ||
            publication.run_record.binding_fingerprint() != old.run.binding_fingerprint() ||
            !same_fork ||
            publication.run_record.recorded_binding_set_fingerprint() !=
                old.run.recorded_binding_set_fingerprint() ||
            publication.run_record.invocation() != old.run.invocation() ||
            !valid_children_transition(old.run, publication.run_record) ||
            publication.run_record.event_sequence() !=
                old.run.event_sequence() + publication.events.size() ||
            publication.run_record.effect_sequence() !=
                old.run.effect_sequence() + publication.effects.size() ||
            (!publication.events.empty() &&
             publication.events.front().sequence != old.run.event_sequence() + 1) ||
            (!publication.effects.empty() &&
             publication.effects.front().sequence() != old.run.effect_sequence() + 1) ||
            !effect_ids_are_unique(old.effects, old.history, publication.effects)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        if (!valid_command_history_append(old.run, old.commands, publication.commands)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    if (current == impl_->runs.end() &&
        !valid_command_history_append(publication.run_record, {}, publication.commands)) {
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto maybe_fail = [&](ProgramTransitionFaultPoint point) {
        if (impl_->fault && *impl_->fault == point) {
            impl_->fault.reset();
            throw std::runtime_error("injected in-memory Program transition failure");
        }
    };
    auto staged_run = std::move(publication.run_record);
    maybe_fail(ProgramTransitionFaultPoint::AfterRunSnapshot);
    auto staged_journal = std::move(publication.journal_record);
    maybe_fail(ProgramTransitionFaultPoint::AfterJournalSnapshot);

    auto appended_events = std::move(publication.events);
    maybe_fail(ProgramTransitionFaultPoint::AfterEventSnapshot);
    auto appended_effects = std::move(publication.effects);
    maybe_fail(ProgramTransitionFaultPoint::AfterEffectSnapshot);
    auto appended_commands = std::move(publication.commands);

    std::vector<ProgramEvent>            events;
    std::vector<ProgramEffectOutboxEntry> effects;
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    ProgramTransitionHistoryPtr           history;
    if (current == impl_->runs.end()) {
        events  = std::move(appended_events);
        effects = std::move(appended_effects);
        commands = std::move(appended_commands);
    } else {
        const auto& old = *current->second;
        if (old.history) {
            history =
                append_history(old.history, std::move(appended_events), std::move(appended_effects));
        } else if (appended_events.empty() && appended_effects.empty()) {
            events  = old.events;
            effects = old.effects;
        } else {
            history = begin_history(old.events, old.effects, std::move(appended_events),
                                    std::move(appended_effects));
        }
        commands = append_entries(old.commands, std::move(appended_commands));
    }

    auto migration_plan = current == impl_->runs.end()
                              ? std::move(publication.migration_plan)
                              : current->second->migration_plan;
    auto candidate = std::make_shared<const Impl::Stored>(
        Impl::Stored{std::move(staged_run), std::move(staged_journal), std::move(events),
                     std::move(effects), std::move(commands), std::move(history),
                     std::move(migration_plan),
                     std::move(publication_bytes)});
    maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
    if (current == impl_->runs.end()) {
        impl_->runs.emplace(std::move(storage_key), std::move(candidate));
    } else {
        current->second = std::move(candidate);
    }
    return ProgramTransitionPublishResult::Published;
}
}  // namespace neograph::program
