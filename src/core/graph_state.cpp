#include <neograph/graph/state.h>
#include <neograph/graph/cancel.h>
#include <neograph/provider_outcome_codec.h>
#include <neograph/graph/run_context.h>
#include "canonical_json.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace neograph::graph {

static bool has_budget_authority(std::uint64_t ceiling,
                                 const UsageAccumulator::AuthoritySnapshot& authority) {
    return ceiling != 0 || authority.charged != 0 || authority.reserved != 0 ||
           authority.has_report || !authority.provider_effects.empty();
}

static std::string graph_bank_binding(const json& budget) {
    return ":managed-bank:" + neograph::detail::sha256_identity(
        "NeoGraph", "graph-provider-bank-custody/v1",
        neograph::detail::canonical_json_bytes(budget));
}

static sp::Message read_portable_message(const json& value) {
    ChatMessage message;
    from_json(value, message);
    return portable_message(message);
}

// DX helper: comma-separated sorted list of declared channel names. Used by
// the "Write to unknown channel" error so the user immediately sees what
// IS declared, instead of having to compare against the JSON definition.
// Caller already holds the mutex.
static std::string declared_channel_list(
    const std::map<std::string, Channel>& channels) {
    if (channels.empty()) return "(none — no channels declared in the graph definition)";
    std::string out;
    bool first = true;
    for (const auto& kv : channels) {  // std::map iterates sorted by key
        if (!first) out += ", ";
        first = false;
        out += kv.first;
    }
    return out;
}

static json apply_retention(json value, const ChannelLifecyclePolicy& lifecycle) {
    if (lifecycle.retention == ChannelRetentionPolicy::Unbounded || !value.is_array()) {
        return value;
    }
    if (lifecycle.retention == ChannelRetentionPolicy::Latest) {
        if (value.empty()) return value;
        return json::array({value.back()});
    }
    if (lifecycle.retention_limit == 0) {
        throw std::invalid_argument(
            "bounded channel retention requires a positive retention_limit");
    }
    json retained = json::array();
    const auto first = value.size() > lifecycle.retention_limit
                            ? value.size() - lifecycle.retention_limit
                            : std::size_t(0);
    for (std::size_t i = first; i < value.size(); ++i) {
        retained.push_back(value[i]);
    }
    return retained;
}

void GraphState::init_channel(const std::string& name,
                               ReducerType type,
                               ReducerFn reducer,
                               const json& initial_value,
                               ChannelLifecyclePolicy lifecycle) {
    if (lifecycle.retention == ChannelRetentionPolicy::Bounded &&
        lifecycle.retention_limit == 0) {
        throw std::invalid_argument(
            "bounded channel retention requires a positive retention_limit");
    }
    std::unique_lock lock(mutex_);
    channels_.insert_or_assign(
        name, Channel{name, type, std::move(reducer), lifecycle,
                      apply_retention(initial_value, lifecycle), 0});
}

json GraphState::get(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end()) return json();
    return it->second.value;
}

std::vector<ChatMessage> GraphState::get_messages() const {
    auto msgs_json = get("messages");
    std::vector<ChatMessage> messages;
    if (msgs_json.is_array()) {
        for (const auto& j : msgs_json) {
            ChatMessage msg;
            from_json(j, msg);
            messages.push_back(std::move(msg));
        }
    }
    return messages;
}

std::vector<sp::Message> GraphState::get_provider_messages(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    if (const auto found = provider_histories_.find(channel); found != provider_histories_.end())
        return found->second.messages;
    std::vector<sp::Message> messages;
    if (const auto found = channels_.find(channel); found != channels_.end() && found->second.value.is_array())
        for (const auto& value : found->second.value) messages.push_back(read_portable_message(value));
    return messages;
}

std::optional<std::vector<sp::Message>> GraphState::captured_provider_messages(
    const std::string& channel) const {
    std::shared_lock lock(mutex_);
    if (const auto found = provider_histories_.find(channel); found != provider_histories_.end())
        return found->second.messages;
    return std::nullopt;
}

void GraphState::set_native_history_archive(std::shared_ptr<sp::NativeArchive> archive) {
    std::unique_lock lock(mutex_);
    native_history_archive_ = std::move(archive);
}

void GraphState::copy_provider_history_from(const GraphState& source) {
    if (&source == this) return;
    std::shared_lock source_lock(source.mutex_);
    std::unique_lock lock(mutex_);
    provider_histories_ = source.provider_histories_;
    native_history_archive_ = source.native_history_archive_;
    provider_loops_ = source.provider_loops_;
    provider_outcomes_ = source.provider_outcomes_;
    provider_budget_ = source.provider_budget_;
}

void GraphState::set_provider_run_history(std::shared_ptr<ProviderLoopHistory> loops,
                                         std::shared_ptr<ProviderOutcomes> outcomes) {
    std::unique_lock lock(mutex_);
    provider_loops_ = std::move(loops);
    provider_outcomes_ = std::move(outcomes);
}

std::vector<sp::runtime::Result> GraphState::provider_outcomes() const {
    std::shared_lock lock(mutex_);
    return provider_outcomes_ ? provider_outcomes_->snapshot() : std::vector<sp::runtime::Result>{};
}

void GraphState::configure_budget_bank(std::shared_ptr<UsageAccumulator> bank, std::uint64_t ceiling,
    bool managed, std::string owner, std::string thread, std::string graph) {
    if (!bank) throw std::invalid_argument("Budget custody requires a real usage bank");
    std::unique_lock lock(mutex_);
    provider_budget_ = NativeGraphCheckpoint::BudgetBinding{std::move(bank), ceiling, ceiling, managed,
        std::move(owner), thread, std::move(graph), std::move(thread), {}};
}

std::shared_ptr<UsageAccumulator> GraphState::budget_bank() const {
    std::shared_lock lock(mutex_);
    return provider_budget_ ? provider_budget_->bank : nullptr;
}

std::uint64_t GraphState::budget_ceiling() const {
    std::shared_lock lock(mutex_);
    return provider_budget_ ? provider_budget_->ceiling : 0;
}

std::uint64_t GraphState::budget_original_ceiling() const {
    std::shared_lock lock(mutex_);
    return provider_budget_ ? provider_budget_->original_ceiling : 0;
}

std::string GraphState::budget_original_thread_id() const {
    std::shared_lock lock(mutex_);
    if (!provider_budget_) return {};
    return provider_budget_->original_thread_id.empty()
        ? provider_budget_->thread_id : provider_budget_->original_thread_id;
}

bool GraphState::budget_managed() const {
    std::shared_lock lock(mutex_);
    return provider_budget_ && provider_budget_->managed;
}

void GraphState::defer_budget_authority_restore() {
    std::unique_lock lock(mutex_);
    defer_budget_authority_ = true;
}

void GraphState::activate_budget_authority(bool observation_only) {
    std::unique_lock lock(mutex_);
    if (!provider_budget_) return;
    auto& binding = *provider_budget_;
    if (observation_only) {
        auto bank = std::make_shared<UsageAccumulator>();
        bank->restore_authority(deferred_budget_authority_ ? std::move(*deferred_budget_authority_) :
            (deferred_budget_bank_ ? deferred_budget_bank_->authority_snapshot() : binding.bank->authority_snapshot()));
        bank->seal_observation();
        binding.bank = std::move(bank);
    } else if (deferred_budget_bank_) {
        binding.bank = std::move(deferred_budget_bank_);
    } else if (deferred_budget_authority_) {
        binding.bank->restore_authority(std::move(*deferred_budget_authority_));
    }
    deferred_budget_authority_.reset();
    deferred_budget_bank_.reset();
    defer_budget_authority_ = false;
}

json GraphState::managed_budget_projection_locked() const {
    if (!provider_budget_ || !provider_budget_->managed || !provider_budget_->bank) return json();
    const auto authority = provider_budget_->bank->authority_snapshot();
    if (!has_budget_authority(provider_budget_->original_ceiling, authority)) return json();
    return json{{"schema", "neograph.graph-managed-bank/v1"}, {"charged", authority.charged},
        {"reserved", authority.reserved}, {"provider_effects", authority.provider_effects},
        {"reports", provider_codec::encode_usage(authority.reports)}, {"has_report", authority.has_report},
        {"ceiling", provider_budget_->original_ceiling}, {"owner_scope", provider_budget_->owner_scope},
        {"thread_id", provider_budget_->thread_id}, {"graph_identity", provider_budget_->graph_identity}};
}

void GraphState::admit_budget_binding(NativeGraphCheckpoint::BudgetBinding saved,
    std::optional<UsageAccumulator::AuthoritySnapshot> authority) {
    if (!saved.bank && !authority)
        throw std::invalid_argument("Budget restore lacks original bank authority");
    if (provider_budget_) {
        auto& current = *provider_budget_;
        if (current.owner_scope.empty() && saved.original_ceiling != 0 && native_history_archive_ &&
            saved.owner_scope == native_history_archive_->owner_scope())
            current.owner_scope = saved.owner_scope;
        if (current.owner_scope != saved.owner_scope || current.thread_id != saved.thread_id ||
            current.graph_identity != saved.graph_identity)
            throw std::invalid_argument("Checkpoint bank owner/thread/graph identity differs from this invocation");
        if (saved.original_ceiling != 0 && current.ceiling > saved.original_ceiling)
            throw std::invalid_argument("Checkpoint budget ceiling cannot be increased");
        if (saved.managed && saved.original_ceiling == 0 && current.ceiling != 0)
            throw std::invalid_argument("An original unbounded observation cannot become finite currency");
        if (!current.managed) {
            if (!saved.bank || current.bank != saved.bank)
                throw std::invalid_argument("Standalone managed authority requires its original bank, not unrelated external currency");
            current.original_ceiling = saved.original_ceiling;
            current.original_thread_id = saved.original_thread_id;
            current.fork_source_thread_id = saved.fork_source_thread_id;
            current.managed = saved.managed;
            if (current.ceiling == 0) current.ceiling = saved.ceiling;
            return;
        }
        current.original_ceiling = saved.original_ceiling;
        current.original_thread_id = saved.original_thread_id;
        current.fork_source_thread_id = saved.fork_source_thread_id;
        if (defer_budget_authority_) {
            deferred_budget_bank_ = std::move(saved.bank);
            deferred_budget_authority_ = std::move(authority);
        } else if (saved.bank) current.bank = std::move(saved.bank);
        else if (authority) current.bank->restore_authority(std::move(*authority));
        if (current.ceiling == 0) current.ceiling = saved.ceiling;
    } else {
        if (defer_budget_authority_) {
            deferred_budget_bank_ = std::move(saved.bank);
            deferred_budget_authority_ = std::move(authority);
            saved.bank = std::make_shared<UsageAccumulator>();
        } else if (!saved.bank) {
            saved.bank = std::make_shared<UsageAccumulator>();
            saved.bank->restore_authority(std::move(*authority));
        }
        provider_budget_ = std::move(saved);
    }
}

void GraphState::rebind_fork_budget_thread(std::string thread, bool original_in_memory_custody) {
    std::unique_lock lock(mutex_);
    if (!provider_budget_ || !provider_budget_->managed) return;
    const auto& bank = deferred_budget_bank_ ? deferred_budget_bank_ : provider_budget_->bank;
    if (has_budget_authority(provider_budget_->original_ceiling, bank->authority_snapshot()) &&
        !original_in_memory_custody)
        throw std::invalid_argument("Managed fork requires original shared in-memory bank custody");
    if (provider_budget_->original_thread_id.empty())
        provider_budget_->original_thread_id = provider_budget_->thread_id;
    provider_budget_->fork_source_thread_id = provider_budget_->thread_id;
    provider_budget_->thread_id = std::move(thread);
}

void GraphState::validate_budget_context(const std::string& graph, const std::string& thread) const {
    std::shared_lock lock(mutex_);
    if (provider_budget_ && (provider_budget_->graph_identity != graph || provider_budget_->thread_id != thread))
        throw std::invalid_argument("Checkpoint budget is bound to another graph/thread identity");
}

void GraphState::update_provider_history_locked(const std::string& channel, const json& before, const json& incoming,
    const json& after, const std::shared_ptr<const std::vector<sp::Message>>& native) {
    auto& history = provider_histories_[channel];
    auto& provider_messages_ = history.messages;
    auto& provider_projection_ = history.projection;
    if (!after.is_array()) throw std::invalid_argument("provider history channel must be an array");
    if (native) {
        if (!incoming.is_array() || native->size() != incoming.size())
            throw std::invalid_argument("Native message write differs from portable projection");
        for (std::size_t index = 0; index < native->size(); ++index) {
            json projection;
            to_json(projection, project_message((*native)[index]));
            if (projection != incoming[index])
                throw std::invalid_argument("Native message write differs from portable projection");
        }
    }
    const auto& channel_state = channels_.at(channel);
    if (before == provider_projection_ && before.is_array() && incoming.is_array() &&
        channel_state.reducer_type == ReducerType::APPEND &&
        channel_state.lifecycle.retention == ChannelRetentionPolicy::Unbounded &&
        after.size() == before.size() + incoming.size() &&
        [&] {
            for (std::size_t index = 0; index < before.size(); ++index)
                if (before[index] != after[index]) return false;
            for (std::size_t index = 0; index < incoming.size(); ++index)
                if (incoming[index] != after[before.size() + index]) return false;
            return true;
        }()) {
        if (native) provider_messages_.insert(provider_messages_.end(), native->begin(), native->end());
        else {
            std::vector<sp::Message> portable;
            portable.reserve(incoming.size());
            for (const auto& item : incoming) portable.push_back(read_portable_message(item));
            provider_messages_.insert(provider_messages_.end(), std::make_move_iterator(portable.begin()),
                                      std::make_move_iterator(portable.end()));
        }
        provider_projection_ = after;
        return;
    }
    std::vector<sp::Message> candidates;
    json projections = json::array();
    if (before == provider_projection_) {
        candidates = provider_messages_;
        projections = before;
    } else if (before.is_array()) {
        for (const auto& item : before) {
            candidates.push_back(read_portable_message(item));
            projections.push_back(item);
        }
    }
    if (native) {
        candidates.reserve(candidates.size() + native->size());
        candidates.insert(candidates.end(), native->begin(), native->end());
        for (const auto& item : incoming) projections.push_back(item);
    } else if (incoming.is_array()) {
        for (const auto& item : incoming) {
            candidates.push_back(read_portable_message(item));
            projections.push_back(item);
        }
    }
    std::vector<sp::Message> selected;
    selected.reserve(after.size());
    std::size_t cursor = 0;
    for (const auto& item : after) {
        auto found = cursor;
        while (found < projections.size() && projections[found] != item) ++found;
        if (found == projections.size())
            throw std::invalid_argument("Message reducer cannot manufacture native history");
        selected.push_back(candidates[found]);
        cursor = found + 1;
    }
    // Native replay groups are atomic: retention cannot split a sealed group.
    std::map<const sp::NativeReplay*, std::pair<std::size_t, std::size_t>> groups;
    for (const auto& message : candidates) if (message.native) ++groups[message.native.get()].first;
    for (const auto& message : selected) if (message.native) ++groups[message.native.get()].second;
    for (const auto& [group, counts] : groups)
        if (counts.second != 0 && counts.first != counts.second)
            throw std::invalid_argument("Message retention split a native replay group");
    provider_messages_ = std::move(selected);
    provider_projection_ = after;
}

void GraphState::save_provider_history_locked(json& snapshot, bool durable, bool include_budget) const {
    const auto budget = include_budget ? managed_budget_projection_locked() : json();
    const auto loops = provider_loops_ ? provider_loops_->snapshot() :
        std::map<std::string, ProviderLoopHistory::Entry>{};
    const auto outcomes = provider_outcomes_ ? provider_outcomes_->snapshot() :
        std::vector<sp::runtime::Result>{};
    // Every archived observation authenticates original bank presence as well
    // as absence. Removing the standalone bank cannot reinterpret its history
    // as belonging to a pristine, unbounded invocation.
    const auto bank_binding = durable && native_history_archive_ &&
        (!budget.is_null() || !provider_histories_.empty() || !loops.empty() || !outcomes.empty())
        ? graph_bank_binding(budget) : std::string{};
    for (const auto& [channel, history] : provider_histories_) {
        if (durable && channels_.at(channel).lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) continue;
        json item;
        item["projection"] = json::array();
        for (const auto& message : history.messages)
            item["projection"].push_back(provider_codec::encode_message(message));
        item["portable_projection"] = history.projection;
        if (durable) {
            sp::Completion completion;
            completion.messages = history.messages;
            item["outcome"] = provider_codec::encode_outcome(sp::Outcome(std::move(completion)),
                native_history_archive_, "graph-history:" + channel + history.projection.dump() + bank_binding);
        }
        snapshot["provider_histories"][channel] = std::move(item);
    }
    if (provider_loops_) {
        for (const auto& [task, entry] : loops) {
            sp::Completion completion;
            completion.messages = entry.messages;
            const sp::Outcome outcome(std::move(completion));
            const auto binding = "graph-loop:" + task + ":" + std::to_string(entry.turns) +
                                 ":" + (entry.client_calls_ready ? "ready" : "not-ready") + bank_binding;
            snapshot["provider_loops"][task] = {{"turns", entry.turns}, {"client_calls_ready", entry.client_calls_ready},
                {"outcome", durable ? provider_codec::encode_outcome(outcome, native_history_archive_, binding)
                                     : provider_codec::observe_outcome(outcome)}};
        }
    }
    if (provider_outcomes_) {
        if (!outcomes.empty()) snapshot["provider_outcomes"] = json::array();
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            const auto binding = "graph-outcome:" + std::to_string(index) + bank_binding;
            snapshot["provider_outcomes"].push_back(durable
                ? provider_codec::encode_outcome(*outcomes[index], native_history_archive_, binding)
                : provider_codec::observe_outcome(*outcomes[index]));
        }
    }
    if (snapshot.contains("provider_histories") || snapshot.contains("provider_loops") ||
        snapshot.contains("provider_outcomes"))
        snapshot["provider_history_durable"] = durable;
    if (!include_budget) return;
    if (!budget.is_null()) {
        if (durable) {
            if (!native_history_archive_)
                throw std::invalid_argument("Durable graph-managed budget requires trusted NativeArchive custody");
            sp::Completion custody;
            custody.usage = provider_codec::decode_usage(budget.at("reports"));
            const auto binding = "neograph.graph-managed-bank/v1:" + neograph::detail::canonical_json_bytes(budget);
            snapshot["provider_managed_budget"] = {{"data", budget},
                {"custody", provider_codec::encode_outcome(sp::Outcome(std::move(custody)), native_history_archive_, binding)}};
        } else if (!durable) snapshot["provider_managed_budget"] = {{"data", budget}};
    }
}

void GraphState::restore_provider_history_locked(const json& snapshot, bool trusted_runtime_copy) {
    const bool has_bank = snapshot.contains("provider_managed_budget");
    const bool managed_invocation = provider_budget_ && provider_budget_->managed;
    if (!trusted_runtime_copy && managed_invocation &&
        has_budget_authority(provider_budget_->ceiling, provider_budget_->bank->authority_snapshot()) &&
        !has_bank)
        throw std::invalid_argument("Managed budget invocation requires original authenticated bank custody");
    const auto bank_binding = !trusted_runtime_copy && native_history_archive_ &&
        (has_bank || snapshot.contains("provider_histories") ||
         snapshot.contains("provider_loops") || snapshot.contains("provider_outcomes"))
        ? graph_bank_binding(has_bank ? snapshot.at("provider_managed_budget").at("data") : json())
        : std::string{};
    std::optional<NativeGraphCheckpoint::BudgetBinding> saved_bank;
    std::optional<UsageAccumulator::AuthoritySnapshot> authority;
    if (has_bank) {
        const auto& item = snapshot.at("provider_managed_budget");
        const auto& data = item.at("data");
        if (!data.is_object() || data.size() != 10 ||
            data.value("schema", std::string{}) != "neograph.graph-managed-bank/v1")
            throw std::invalid_argument("Managed budget lacks authentic durable custody");
        if (trusted_runtime_copy) {
            if (!managed_invocation ||
                provider_budget_->original_ceiling != data.at("ceiling").get<std::uint64_t>() ||
                provider_budget_->owner_scope != data.at("owner_scope").get<std::string>() ||
                provider_budget_->thread_id != data.at("thread_id").get<std::string>() ||
                provider_budget_->graph_identity != data.at("graph_identity").get<std::string>())
                throw std::invalid_argument("Runtime budget copy requires original trusted bank custody");
        } else {
            if (item.size() != 2 || !item.contains("custody") || !native_history_archive_)
                throw std::invalid_argument("Managed budget lacks authentic durable custody");
            const auto& custody = item.at("custody");
            if (!custody.is_object() || !custody.contains("native_archive_reference") ||
                !custody.at("native_archive_reference").is_string() ||
                custody.at("native_archive_reference").get<std::string>().empty())
                throw std::invalid_argument("Managed budget requires a genuine authenticated archive reference");
            const auto binding = "neograph.graph-managed-bank/v1:" + neograph::detail::canonical_json_bytes(data);
            auto verified = provider_codec::decode_outcome(custody, native_history_archive_, binding);
            if (provider_codec::encode_usage(outcome_usage(*verified)) != data.at("reports"))
                throw std::invalid_argument("Managed budget report differs from authenticated custody");
            authority.emplace();
            authority->charged = data.at("charged").get<std::uint64_t>();
            authority->reserved = data.at("reserved").get<std::uint64_t>();
            authority->provider_effects = data.at("provider_effects").get<std::vector<std::string>>();
            authority->reports = provider_codec::decode_usage(data.at("reports"));
            authority->has_report = data.at("has_report").get<bool>();
            const auto ceiling = data.at("ceiling").get<std::uint64_t>();
            saved_bank.emplace(NativeGraphCheckpoint::BudgetBinding{{}, ceiling, ceiling, true,
                data.at("owner_scope").get<std::string>(), data.at("thread_id").get<std::string>(),
                data.at("graph_identity").get<std::string>(), {}, {}});
        }
    }
    std::map<std::string, ProviderHistory> histories;
    if (snapshot.contains("provider_histories")) {
        for (const auto& [channel, item] : snapshot.at("provider_histories").items()) {
            if (!channels_.contains(channel)) throw std::invalid_argument("Checkpoint has undeclared provider history");
            ProviderHistory history;
            history.projection = snapshot.at("channels").at(channel).at("value");
            if (history.projection != item.at("portable_projection"))
                throw std::invalid_argument("Checkpoint portable history was edited");
            if (item.contains("outcome")) {
                auto restored = provider_codec::decode_outcome(item.at("outcome"), native_history_archive_,
                    "graph-history:" + channel + history.projection.dump() + bank_binding);
                history.messages = outcome_messages(*restored);
            } else {
                json current = json::array();
                const auto previous = provider_histories_.find(channel);
                if (previous != provider_histories_.end())
                    for (const auto& message : previous->second.messages) current.push_back(provider_codec::encode_message(message));
                if (current == item.at("projection") && previous != provider_histories_.end() &&
                    previous->second.projection == history.projection)
                    history.messages = previous->second.messages;
                else
                    for (const auto& message : item.at("projection")) history.messages.push_back(provider_codec::decode_message(message));
            }
            json full = json::array();
            for (const auto& message : history.messages)
                full.push_back(provider_codec::encode_message(message));
            if (full != item.at("projection"))
                throw std::invalid_argument("Checkpoint provider history projection was edited");
            histories.emplace(channel, std::move(history));
        }
    }
    std::map<std::string, ProviderLoopHistory::Entry> loops;
    std::vector<sp::runtime::Result> outcomes;
    if (!trusted_runtime_copy) {
        if (snapshot.contains("provider_loops")) {
            for (const auto& [task, item] : snapshot.at("provider_loops").items()) {
                const auto turns = item.at("turns").get<std::uint64_t>();
                const auto ready = item.at("client_calls_ready").get<bool>();
                auto outcome = provider_codec::decode_outcome(item.at("outcome"), native_history_archive_,
                    "graph-loop:" + task + ":" + std::to_string(turns) + ":" +
                    (ready ? "ready" : "not-ready") + bank_binding);
                if (managed_invocation && !has_bank && turns != 0)
                    throw std::invalid_argument("Provider loop progress requires original authenticated bank custody");
                loops.emplace(task, ProviderLoopHistory::Entry{outcome_messages(*outcome), turns, ready});
            }
        }
        if (snapshot.contains("provider_outcomes")) {
            std::size_t index = 0;
            for (const auto& item : snapshot.at("provider_outcomes")) {
                auto outcome = provider_codec::decode_outcome(item, native_history_archive_,
                    "graph-outcome:" + std::to_string(index++) + bank_binding);
                // Recording a provider outcome observes its report in the real
                // bank, even when usage is unknown or a no-send failure refunds
                // the reservation. A fresh managed bank cannot replace it.
                if (managed_invocation && !has_bank)
                    throw std::invalid_argument("Provider observations require original authenticated bank custody");
                outcomes.push_back(std::move(outcome));
            }
        }
    }
    // Authenticate all observations before restoring authority or merging any
    // captured state; a later invalid reference must not consume a bank restore.
    if (saved_bank) admit_budget_binding(std::move(*saved_bank), std::move(authority));
    provider_histories_ = std::move(histories);
    if (!trusted_runtime_copy) {
        if (!provider_loops_) provider_loops_ = std::make_shared<ProviderLoopHistory>();
        for (auto& [task, entry] : loops) provider_loops_->set(task, std::move(entry));
        if (!provider_outcomes_) provider_outcomes_ = std::make_shared<ProviderOutcomes>();
        if (provider_outcomes_->snapshot().empty())
            for (auto& outcome : outcomes) provider_outcomes_->add(std::move(outcome));
    }
}

void GraphState::write(const std::string& channel, const json& value) {
    std::unique_lock lock(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        throw std::runtime_error(
            "Write to unknown channel: '" + channel + "'. "
            "Declared channels: " + declared_channel_list(channels_) + ". "
            "Channel names are case-sensitive; add it to the graph "
            "definition's \"channels\" block before writing. "
            "See docs/troubleshooting.md \"Write to unknown channel\".");
    }
    auto& ch  = it->second;
    const auto combined = apply_retention(ch.reducer(ch.value, value), ch.lifecycle);
    if (provider_histories_.contains(channel))
        update_provider_history_locked(channel, ch.value, value, combined, {});
    ch.value = combined;
    ch.version = ++global_version_;
}

void GraphState::apply_writes(const std::vector<ChannelWrite>& writes) {
    std::unique_lock lock(mutex_);
    for (const auto& w : writes) {
        auto it = channels_.find(w.channel);
        if (it == channels_.end()) {
            throw std::runtime_error(
                "Write to unknown channel: '" + w.channel + "'. "
                "Declared channels: " + declared_channel_list(channels_) + ". "
                "Channel names are case-sensitive; add it to the graph "
                "definition's \"channels\" block before writing. "
                "See docs/troubleshooting.md \"Write to unknown channel\".");
        }
        auto& ch = it->second;
        // The mode is the whole point of ChannelWrite::Mode (#91): Reduce keeps
        // the reducer as the law, Overwrite is an explicit, *recorded* escape
        // from it. Because the intent rides on the write, it lands in the write
        // log, survives checkpointing, and replays identically — which a
        // side-door GraphState::overwrite() could never do.
        const auto combined = (w.mode == ChannelWrite::Mode::Overwrite)
                                  ? w.value
                                  : ch.reducer(ch.value, w.value);
        const auto retained = apply_retention(combined, ch.lifecycle);
        if (w.native_messages || provider_histories_.contains(w.channel))
            update_provider_history_locked(w.channel,
                                           w.mode == ChannelWrite::Mode::Overwrite ? json::array() : ch.value,
                                           w.value, retained, w.native_messages);
        ch.value = retained;
        ch.version = ++global_version_;
    }
}

uint64_t GraphState::channel_version(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    auto it = channels_.find(channel);
    return it != channels_.end() ? it->second.version : 0;
}

uint64_t GraphState::global_version() const {
    std::shared_lock lock(mutex_);
    return global_version_;
}

std::pair<json, std::shared_ptr<const NativeGraphCheckpoint>> GraphState::checkpoint_snapshot() const {
    std::shared_lock lock(mutex_);
    std::shared_ptr<NativeGraphCheckpoint> native;
    const auto ensure_native = [&]() -> NativeGraphCheckpoint& {
        if (!native) native.reset(new NativeGraphCheckpoint);
        return *native;
    };
    json data;
    bool requires_native = false;
    for (const auto& [name, channel] : channels_) {
        if (channel.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) continue;
        data["channels"][name] = {{"value", channel.value}, {"version", channel.version}};
    }
    data["global_version"] = global_version_;
    for (const auto& [channel, history] : provider_histories_) {
        if (channels_.at(channel).lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) continue;
        ensure_native().histories_.emplace(channel, history);
        json item;
        item["portable_projection"] = history.projection;
        item["projection"] = json::array();
        for (const auto& message : history.messages) {
            item["projection"].push_back(provider_codec::encode_message(message));
            requires_native = requires_native || bool(message.native);
        }
        data["provider_histories"][channel] = std::move(item);
    }
    auto loops = provider_loops_ ? provider_loops_->snapshot() :
        std::map<std::string, ProviderLoopHistory::Entry>{};
    for (const auto& [task, entry] : loops) {
        sp::Completion completion;
        completion.messages = entry.messages;
        data["provider_loops"][task] = {{"turns", entry.turns}, {"client_calls_ready", entry.client_calls_ready},
            {"outcome", provider_codec::observe_outcome(sp::Outcome(std::move(completion)))}};
        for (const auto& message : entry.messages) requires_native = requires_native || bool(message.native);
    }
    if (!loops.empty()) ensure_native().loops_ = std::move(loops);
    auto outcomes = provider_outcomes_ ? provider_outcomes_->snapshot() :
        std::vector<sp::runtime::Result>{};
    if (!outcomes.empty()) data["provider_outcomes"] = json::array();
    for (const auto& outcome : outcomes) {
        data["provider_outcomes"].push_back(provider_codec::observe_outcome(*outcome));
        for (const auto& message : outcome_messages(*outcome)) requires_native = requires_native || bool(message.native);
    }
    if (!outcomes.empty()) ensure_native().outcomes_ = std::move(outcomes);
    if (data.contains("provider_histories") || data.contains("provider_loops") ||
        data.contains("provider_outcomes"))
        data["provider_history_durable"] = false;
    auto budget = managed_budget_projection_locked();
    if (!budget.is_null()) {
        ensure_native().budget_ = provider_budget_;
        data["provider_managed_budget"] = {{"data", std::move(budget)}};
        requires_native = true;
    }
    if (requires_native) data["native_checkpoint_required"] = true;
    if (native) native->projection_ = data;
    return {std::move(data), std::move(native)};
}

json GraphState::serialize() const {
    std::shared_lock lock(mutex_);
    json data;
    for (const auto& [name, ch] : channels_) {
        if (ch.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) continue;
        data["channels"][name] = {
            {"value", ch.value},
            {"version", ch.version}
        };
    }
    data["global_version"] = global_version_;
    save_provider_history_locked(data);
    return data;
}

json GraphState::serialize_runtime() const {
    std::shared_lock lock(mutex_);
    return serialize_runtime_locked(true);
}

json GraphState::serialize_cache() const {
    std::shared_lock lock(mutex_);
    return serialize_runtime_locked(false);
}

json GraphState::serialize_runtime_locked(bool include_budget) const {
    json data;
    for (const auto& [name, ch] : channels_) {
        data["channels"][name] = {{"value", ch.value}, {"version", ch.version}};
    }
    data["global_version"] = global_version_;
    save_provider_history_locked(data, false, include_budget);
    return data;
}

void GraphState::restore_runtime(const json& data) {
    std::unique_lock lock(mutex_);
    for (const auto& [name, channel] : channels_) {
        if (channel.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral &&
            (!data.contains("channels") ||
             !data["channels"].contains(name) ||
             !data["channels"][name].is_object() ||
             !data["channels"][name].contains("value") ||
             !data["channels"][name].contains("version"))) {
            throw std::runtime_error(
                "Runtime snapshot is missing ephemeral channel state: " + name);
        }
    }
    restore_provider_history_locked(data, true);
    if (data.contains("channels")) {
        for (const auto& [name, ch_data] : data["channels"].items()) {
            auto it = channels_.find(name);
            if (it != channels_.end()) {
                it->second.value   = ch_data["value"];
                it->second.version = ch_data.value("version", uint64_t(0));
            }
        }
    }
    global_version_ = data.value("global_version", uint64_t(0));
}

json GraphState::ephemeral_checkpoint_guard() const {
    std::shared_lock lock(mutex_);
    json guard;
    for (const auto& [name, ch] : channels_) {
        if (ch.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) {
            if (guard.is_null()) guard = json::object();
            guard[name] = ch.version != 0;
        }
    }
    return guard;
}

void GraphState::restore_checkpoint(const json& data, const json& guard,
    std::shared_ptr<const NativeGraphCheckpoint> native) {
    std::unique_lock lock(mutex_);
    if (native && !yyjson_mut_equals(native->projection_.raw_val(), data.raw_val()))
        throw std::invalid_argument("In-memory checkpoint projection differs from original typed custody");
    if (!native && data.value("native_checkpoint_required", false))
        throw std::invalid_argument("Native checkpoint cannot be restored from JSON alone");
    if (native && !native->budget_ && provider_budget_ && provider_budget_->managed &&
        has_budget_authority(provider_budget_->ceiling, provider_budget_->bank->authority_snapshot()))
        throw std::invalid_argument("Managed budget invocation requires original authenticated bank custody");
    json expected;
    for (const auto& [name, ch] : channels_) {
        if (ch.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral) {
            if (ch.version != 0)
                throw std::runtime_error(
                    "Cannot restore checkpoint into live ephemeral channel: " + name);
            if (expected.is_null()) expected = json::object();
            expected[name] = false;
        }
    }
    if (expected.is_null()) {
        if (!guard.is_null() && guard != json::object())
            throw std::runtime_error("Checkpoint contains incompatible ephemeral channel policy");
    } else if (guard != expected) {
        throw std::runtime_error(
            "Cannot resume checkpoint: ephemeral channel was written or "
            "its lifecycle guard is missing/incompatible");
    }
    if (native) {
        if (native->budget_) admit_budget_binding(*native->budget_, std::nullopt);
        provider_histories_ = native->histories_;
        if (!provider_loops_) provider_loops_ = std::make_shared<ProviderLoopHistory>();
        for (const auto& [task, entry] : native->loops_)
            if (provider_loops_->get(task).turns <= entry.turns) provider_loops_->set(task, entry);
        if (!provider_outcomes_) provider_outcomes_ = std::make_shared<ProviderOutcomes>();
        if (provider_outcomes_->snapshot().empty())
            for (const auto& outcome : native->outcomes_) provider_outcomes_->add(outcome);
    } else restore_provider_history_locked(data);
    if (data.contains("channels")) {
        for (const auto& [name, ch_data] : data["channels"].items()) {
            auto it = channels_.find(name);
            if (it != channels_.end() &&
                it->second.lifecycle.persistence != ChannelPersistencePolicy::Ephemeral) {
                it->second.value   = ch_data["value"];
                it->second.version = ch_data.value("version", uint64_t(0));
            }
        }
    }
    global_version_ = data.value("global_version", uint64_t(0));
}

void GraphState::restore(const json& data) {
    std::unique_lock lock(mutex_);
    if (data.value("native_checkpoint_required", false))
        throw std::invalid_argument("Native checkpoint cannot be restored from JSON alone");
    for (const auto& [name, channel] : channels_) {
        if (channel.lifecycle.persistence == ChannelPersistencePolicy::Ephemeral)
            throw std::runtime_error(
                "Cannot restore ephemeral channel without a checkpoint guard: " + name);
    }
    restore_provider_history_locked(data);
    if (data.contains("channels")) {
        for (const auto& [name, ch_data] : data["channels"].items()) {
            auto it = channels_.find(name);
            if (it != channels_.end() &&
                it->second.lifecycle.persistence != ChannelPersistencePolicy::Ephemeral) {
                it->second.value   = ch_data["value"];
                it->second.version = ch_data.value("version", uint64_t(0));
            }
        }
    }
    global_version_ = data.value("global_version", uint64_t(0));
}

std::vector<std::string> GraphState::channel_names() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, _] : channels_) {
        names.push_back(name);
    }
    return names;
}

bool GraphState::has_channel(const std::string& channel) const {
    std::shared_lock lock(mutex_);
    return channels_.find(channel) != channels_.end();
}

// v1.0 (9d): run_cancel_token smuggling channel is gone; cancel flows
// through RunContext::cancel_token instead.

} // namespace neograph::graph
