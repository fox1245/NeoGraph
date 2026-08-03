#include <neograph/program/transition_store.h>

#include "canonical_json.h"

#include <limits>
#include <map>
#include <mutex>
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
    (void)run.serialize_canonical();
    (void)journal.serialize_canonical();

    std::uint64_t previous_sequence = 0;
    for (const auto& event : publication.events) {
        (void)event.serialize_canonical();
        if (event.run_id != run.run_id() || event.program_version_id != run.program_version_id() ||
            event.bundle_id != run.bundle_id() ||
            event.operation_id != run.continuation().operation_id ||
            event.attempt != run.continuation().attempt || event.sequence <= previous_sequence) {
            throw std::invalid_argument("Program event does not bind snapshot");
        }
        if (event.kind == ProgramEventKind::CheckpointPublished) {
            const auto& checkpoint = std::get<ProgramCheckpointEvent>(event.payload).checkpoint;
            if (!run.exact_checkpoint() || checkpoint != *run.exact_checkpoint()) {
                throw std::invalid_argument("Program checkpoint event disagrees with run snapshot");
            }
        }
        previous_sequence = event.sequence;
    }
    if (!publication.events.empty() && publication.events.back().sequence != run.event_sequence()) {
        throw std::invalid_argument("Program event sequence mismatch");
    }

    previous_sequence = 0;
    for (const auto& effect : publication.effects) {
        (void)effect.serialize_canonical();
        if (effect.sequence() <= previous_sequence ||
            effect.effect().operation_id() != run.continuation().operation_id) {
            throw std::invalid_argument("Program effect does not bind snapshot");
        }
        previous_sequence = effect.sequence();
    }
    if (!publication.effects.empty() &&
        publication.effects.back().sequence() != run.effect_sequence()) {
        throw std::invalid_argument("Program effect sequence mismatch");
    }
    if (run.terminal_result() && !publication.events.empty() &&
        publication.events.back().kind == ProgramEventKind::Terminal &&
        std::get<ProgramTerminalEvent>(publication.events.back().payload).status !=
            run.terminal_result()->status()) {
        throw std::invalid_argument("Terminal event/result mismatch");
    }
}
json pub_body(const ProgramTransitionPublication& p) {
    json events = json::array(), effects = json::array();
    for (auto& e : p.events)
        events.push_back(nested(e.serialize_canonical()));
    for (auto& e : p.effects)
        effects.push_back(nested(e.serialize_canonical()));
    return {{"format", std::string(PUBLICATION_FORMAT)},
            {"storage_schema_version", 1},
            {"run_record", nested(p.run_record.serialize_canonical())},
            {"journal_record", nested(p.journal_record.serialize_canonical())},
            {"events", std::move(events)},
            {"effects", std::move(effects)}};
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
    return detail::canonical_json_bytes(pub_body(*this));
}
ProgramTransitionPublication ProgramTransitionPublication::parse(std::string_view bytes) {
    auto v = detail::parse_json_strict(bytes);
    if (!v.is_object() || rs(v, "format") != PUBLICATION_FORMAT)
        throw std::invalid_argument("Stored Program publication has unknown format");
    detail::reject_unknown_fields(
        v, "Stored Program publication",
        {"format", "storage_schema_version", "run_record", "journal_record", "events", "effects"});
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
    ProgramTransitionPublication out{std::move(run), std::move(journal), std::move(events),
                                     std::move(effects)};
    validate_pub(out, out.run_record.owner_scope());
    return out;
}
struct InMemoryProgramTransitionStore::Impl {
    struct Stored {
        ProgramRunRecord                      run;
        ProgramJournalRecord                  journal;
        std::vector<ProgramEvent>             events;
        std::vector<ProgramEffectOutboxEntry> effects;
        std::string                           bytes;
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
    std::lock_guard l(impl_->mutex);
    auto            i = impl_->runs.find(key(o, r));
    if (i == impl_->runs.end()) return std::nullopt;
    return i->second->run;
}
std::optional<ProgramJournalRecord> InMemoryProgramTransitionStore::latest(
    std::string_view o, std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::lock_guard l(impl_->mutex);
    auto            i = impl_->runs.find(key(o, r));
    if (i == impl_->runs.end()) return std::nullopt;
    return i->second->journal;
}
std::vector<ProgramEvent> InMemoryProgramTransitionStore::load_events(std::string_view o,
                                                                      std::string_view r,
                                                                      std::uint64_t    a) const {
    std::lock_guard l(impl_->mutex);
    auto            i = impl_->runs.find(key(o, r));
    if (i == impl_->runs.end()) return {};
    std::vector<ProgramEvent> x;
    for (auto& e : i->second->events)
        if (e.sequence > a) x.push_back(e);
    return x;
}
std::vector<ProgramEffectOutboxEntry> InMemoryProgramTransitionStore::load_effects(
    std::string_view o, std::string_view r, std::uint64_t a) const {
    std::lock_guard l(impl_->mutex);
    auto            i = impl_->runs.find(key(o, r));
    if (i == impl_->runs.end()) return {};
    std::vector<ProgramEffectOutboxEntry> x;
    for (auto& e : i->second->effects)
        if (e.sequence() > a) x.push_back(e);
    return x;
}
ProgramTransitionPublishResult InMemoryProgramTransitionStore::compare_publish(
    std::string_view owner, std::string_view expected, ProgramTransitionPublication publication) {
    std::string publication_bytes;
    try {
        validate_pub(publication, owner);
        publication_bytes = publication.serialize_canonical();
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
            (new_terminal && !publishes_terminal_event)) {
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
            publication.run_record.event_sequence() !=
                old.run.event_sequence() + publication.events.size() ||
            publication.run_record.effect_sequence() !=
                old.run.effect_sequence() + publication.effects.size() ||
            (!publication.events.empty() &&
             publication.events.front().sequence != old.run.event_sequence() + 1) ||
            (!publication.effects.empty() &&
             publication.effects.front().sequence() != old.run.effect_sequence() + 1)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    const auto maybe_fail = [&](ProgramTransitionFaultPoint point) {
        if (impl_->fault && *impl_->fault == point) {
            impl_->fault.reset();
            throw std::runtime_error("injected in-memory Program transition failure");
        }
    };
    auto staged_run = publication.run_record;
    maybe_fail(ProgramTransitionFaultPoint::AfterRunSnapshot);
    auto staged_journal = publication.journal_record;
    maybe_fail(ProgramTransitionFaultPoint::AfterJournalSnapshot);

    std::vector<ProgramEvent>            events;
    std::vector<ProgramEffectOutboxEntry> effects;
    if (current != impl_->runs.end()) {
        events  = current->second->events;
        effects = current->second->effects;
    }
    events.insert(events.end(), publication.events.begin(), publication.events.end());
    maybe_fail(ProgramTransitionFaultPoint::AfterEventSnapshot);
    effects.insert(effects.end(), publication.effects.begin(), publication.effects.end());
    maybe_fail(ProgramTransitionFaultPoint::AfterEffectSnapshot);
    auto candidate = std::make_shared<const Impl::Stored>(
        Impl::Stored{std::move(staged_run), std::move(staged_journal), std::move(events),
                     std::move(effects), std::move(publication_bytes)});
    maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
    if (current == impl_->runs.end()) {
        impl_->runs.emplace(std::move(storage_key), std::move(candidate));
    } else {
        current->second = std::move(candidate);
    }
    return ProgramTransitionPublishResult::Published;
}
}  // namespace neograph::program
