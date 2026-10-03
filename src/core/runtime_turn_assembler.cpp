#include <neograph/runtime_turn_assembler.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace neograph {
namespace {

constexpr std::string_view IDENTITY_PREAMBLE = "NeoGraph Runtime turn identity v1";

std::string identity(std::string_view domain, const json& value) {
    return detail::sha256_identity(IDENTITY_PREAMBLE, domain, detail::canonical_json_bytes(value));
}

json messages_json(const std::vector<sp::Message>& messages) {
    json result = json::array();
    for (const auto& message : messages) {
        result.push_back(message_projection_json(message));
    }
    return result;
}

std::uint64_t estimate_json_tokens(const json& value) {
    const auto bytes = detail::canonical_json_bytes(value).size();
    return static_cast<std::uint64_t>(bytes / 3u + (bytes % 3u != 0));
}

void verify_history(const std::vector<RuntimeHistoryRecord>& records,
                    const ContextEpoch& epoch, std::vector<sp::Message>& messages) {
    std::uint64_t expected = epoch.raw_from_sequence();
    std::optional<std::string> predecessor;
    for (const auto& record : records) {
        if (record.feed_id() != epoch.feed_id() || record.sequence() != expected ||
            (expected > 1 && !predecessor && !record.predecessor_id()) ||
            (predecessor && (!record.predecessor_id() || *record.predecessor_id() != *predecessor))) {
            throw std::invalid_argument("Context epoch raw history does not form its declared chain");
        }
        predecessor = record.id();
        messages.push_back(record.message());
        ++expected;
    }
    if (records.size() != epoch.raw_through_sequence() - epoch.raw_from_sequence() + 1 ||
        expected - 1 != epoch.raw_through_sequence()) {
        throw std::invalid_argument("Context epoch raw history does not cover its declared range");
    }
}

struct SelectedArtifact {
    ContextArtifact artifact;
};

bool artifact_less(const SelectedArtifact& lhs, const SelectedArtifact& rhs) {
    const auto& a = lhs.artifact;
    const auto& b = rhs.artifact;
    if (a.placement() != b.placement()) return a.placement() < b.placement();
    if (a.priority() != b.priority()) return a.priority() > b.priority();
    if (a.producer_id() != b.producer_id()) return a.producer_id() < b.producer_id();
    return a.id() < b.id();
}

sp::Message render_artifact(const ContextArtifact& artifact) {
    if (artifact.media_type() != "text/plain" && artifact.media_type() != "text/markdown") {
        throw std::invalid_argument("Context assembly only renders text/plain or text/markdown artifacts");
    }
    const auto role = artifact.kind() == ContextArtifactKind::RequiredSkill ||
                      artifact.kind() == ContextArtifactKind::HardConstraint
        ? sp::Role::System
        : sp::Role::User;
    const auto content = artifact.content();
    std::string text;
    if (content.is_string()) {
        text = content.get<std::string>();
    } else {
        if (!content.is_object() || content.size() != 1 || !content.contains("text") ||
            !content.at("text").is_string()) {
            throw std::invalid_argument("Context artifact text content must be a string or exactly {text: string}");
        }
        text = content.at("text").get<std::string>();
    }
    sp::Message message;
    message.role = role;
    message.parts.emplace_back(sp::Text{std::move(text)});
    return message;
}

void normalize_required_ids(std::vector<std::string>& ids,
                            std::string_view label) {
    for (const auto& id : ids) {
        if (!detail::is_sha256_identity(id)) {
            throw std::invalid_argument(std::string(label) +
                                        " must contain sha256 identities");
        }
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        throw std::invalid_argument(std::string(label) + " contains a duplicate");
    }
}

RuntimeContextRequirements skill_requirements(std::vector<std::string> skills) {
    RuntimeContextRequirements requirements;
    requirements.required_artifact_ids = skills;
    requirements.required_skill_artifact_ids = std::move(skills);
    return requirements;
}

}  // namespace

struct RuntimeTurnAssembler::Impl {
    ContextStore* store;
    RuntimeContextRequirements requirements;
    std::uint64_t max_input_tokens;
};

ContextBudgetBlocked::ContextBudgetBlocked()
    : std::runtime_error("context_budget_blocked") {}

RuntimeTurnAssembler::RuntimeTurnAssembler(
    ContextStore& store, std::vector<std::string> static_required_skill_artifact_ids,
    std::uint64_t max_input_tokens)
    : RuntimeTurnAssembler(store, max_input_tokens,
                           skill_requirements(
                               std::move(static_required_skill_artifact_ids))) {}

RuntimeTurnAssembler::RuntimeTurnAssembler(
    ContextStore& store, std::uint64_t max_input_tokens,
    RuntimeContextRequirements requirements) {
    normalize_required_ids(requirements.required_artifact_ids,
                           "Required context artifact ids");
    normalize_required_ids(requirements.required_skill_artifact_ids,
                           "Required skill artifact ids");
    if (!std::includes(requirements.required_artifact_ids.begin(),
                       requirements.required_artifact_ids.end(),
                       requirements.required_skill_artifact_ids.begin(),
                       requirements.required_skill_artifact_ids.end())) {
        throw std::invalid_argument(
            "Required skill artifacts must be included in required context artifacts");
    }
    impl_ = std::make_unique<Impl>(
        Impl{&store, std::move(requirements), max_input_tokens});
}
RuntimeTurnAssembler::~RuntimeTurnAssembler() = default;
RuntimeTurnAssembler::RuntimeTurnAssembler(RuntimeTurnAssembler&&) noexcept = default;
RuntimeTurnAssembler& RuntimeTurnAssembler::operator=(RuntimeTurnAssembler&&) noexcept = default;

std::uint64_t RuntimeTurnAssembler::estimate_input_tokens(const PreparedProviderRequest& request) {
    const auto bytes = request.encoded_body().size();
    return static_cast<std::uint64_t>(bytes / 3u + (bytes % 3u != 0));
}

std::string RuntimeTurnAssembler::normalized_request_digest(const PreparedProviderRequest& request) {
    return Provider::request_digest(request);
}

RuntimeTurn RuntimeTurnAssembler::assemble(
    Provider& provider, std::string owner_id, const ContextEpoch& epoch,
    ProviderRequest request) const {
    return assemble(provider, std::move(owner_id), epoch, std::move(request), {}, {});
}

RuntimeTurn RuntimeTurnAssembler::assemble(
    Provider& provider, std::string owner_id, const ContextEpoch& epoch, ProviderRequest request,
    std::vector<sp::Message> host_instructions,
    std::vector<sp::Message> trusted_supplemental) const {
    detail::validate_token(owner_id, "Context assembly owner_id");
    const bool has_messages = std::visit([](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, sp::chat::Request>) {
            return !payload.canonical_messages.empty() || !payload.messages.empty();
        } else {
            return !payload.messages.empty();
        }
    }, request.payload);
    if (has_messages) {
        throw std::invalid_argument("Runtime turn request template must not contain messages");
    }
    if (!std::includes(epoch.artifact_ids().begin(), epoch.artifact_ids().end(),
                       impl_->requirements.required_artifact_ids.begin(),
                       impl_->requirements.required_artifact_ids.end())) {
        throw std::invalid_argument("Context epoch omits a required context artifact");
    }
    if (epoch.guarantee_profile() == RuntimeGuaranteeProfile::Strict &&
        impl_->max_input_tokens == 0) {
        throw std::invalid_argument("Strict context assembly requires an input token budget");
    }

    std::vector<sp::Message> raw_messages;
    if (epoch.raw_from_sequence() != 0) {
        const ContextStoreFeed feed{owner_id, epoch.feed_id()};
        const auto range = impl_->store->snapshot_history(feed, epoch.raw_from_sequence(),
                                                           epoch.raw_through_sequence());
        if (range.digest != epoch.raw_window_digest()) {
            throw std::invalid_argument("Context epoch raw window digest does not match the feed");
        }
        verify_history(impl_->store->hydrate_records(range), epoch, raw_messages);
    }

    std::vector<SelectedArtifact> selected;
    std::vector<ContextArtifact> receipt_artifacts;
    selected.reserve(epoch.artifact_ids().size());
    receipt_artifacts.reserve(epoch.artifact_ids().size());
    for (const auto& id : epoch.artifact_ids()) {
        const auto artifact = impl_->store->get_artifact(owner_id, id);
        if (!artifact) throw std::invalid_argument("Context epoch artifact is not available to this owner");
        const bool static_required = std::binary_search(
            impl_->requirements.required_artifact_ids.begin(),
            impl_->requirements.required_artifact_ids.end(), id);
        const bool required_skill = std::binary_search(
            impl_->requirements.required_skill_artifact_ids.begin(),
            impl_->requirements.required_skill_artifact_ids.end(), id);
        if (artifact->kind() == ContextArtifactKind::RequiredSkill && !required_skill) {
            throw std::invalid_argument("Context epoch selects an unadmitted required skill artifact");
        }
        if (static_required && !artifact->required()) {
            throw std::invalid_argument(
                "Configured required context identity is not a required artifact");
        }
        if (required_skill && artifact->kind() != ContextArtifactKind::RequiredSkill) {
            throw std::invalid_argument("Configured skill identity is not a required skill artifact");
        }
        selected.push_back({*artifact});
        receipt_artifacts.push_back(*artifact);
    }
    std::sort(selected.begin(), selected.end(), artifact_less);
    const bool needs_latest_user = std::any_of(
        selected.begin(), selected.end(), [](const SelectedArtifact& item) {
            return item.artifact.placement() == ContextPlacement::BeforeLatestUser ||
                   item.artifact.placement() == ContextPlacement::AfterLatestUser;
        });
    if (needs_latest_user &&
        std::none_of(raw_messages.begin(), raw_messages.end(),
                     [](const sp::Message& message) {
                         return message.role == sp::Role::User;
                     })) {
        throw std::invalid_argument("Selected context artifacts require a raw history user message");
    }

    std::vector<sp::Message> before_history;
    std::vector<sp::Message> before;
    std::vector<sp::Message> after_user;
    std::vector<sp::Message> after_history;
    std::uint64_t mandatory = 0;
    for (const auto& item : selected) {
        const auto rendered = render_artifact(item.artifact);
        if (item.artifact.required()) {
            mandatory += estimate_json_tokens(messages_json({rendered}));
        }
        if (item.artifact.placement() == ContextPlacement::BeforeHistory) {
            before_history.push_back(rendered);
        } else if (item.artifact.placement() == ContextPlacement::BeforeLatestUser) {
            before.push_back(rendered);
        } else if (item.artifact.placement() == ContextPlacement::AfterLatestUser) {
            after_user.push_back(rendered);
        } else {
            after_history.push_back(rendered);
        }
    }

    std::vector<sp::Message> merged;
    merged.insert(merged.end(), before_history.begin(), before_history.end());
    const auto last_user = std::find_if(raw_messages.rbegin(), raw_messages.rend(),
                                        [](const sp::Message& message) { return message.role == sp::Role::User; });
    const auto insertion = last_user == raw_messages.rend()
                               ? raw_messages.size()
                               : static_cast<std::size_t>(std::distance(last_user, raw_messages.rend()) - 1);
    merged.insert(merged.end(), raw_messages.begin(), raw_messages.begin() + insertion);
    merged.insert(merged.end(), before.begin(), before.end());
    if (insertion < raw_messages.size()) {
        merged.push_back(raw_messages[insertion]);
        merged.insert(merged.end(), after_user.begin(), after_user.end());
        merged.insert(merged.end(), raw_messages.begin() + insertion + 1, raw_messages.end());
    } else {
        merged.insert(merged.end(), after_user.begin(), after_user.end());
    }
    merged.insert(merged.end(), after_history.begin(), after_history.end());
    // Slots are distinct from caller history: host instructions survive
    // interposition, and supplemental task input is receipt-bound.
    std::vector<sp::Message> assembled;
    assembled.reserve(host_instructions.size() + merged.size() + trusted_supplemental.size());
    assembled.insert(assembled.end(), host_instructions.begin(), host_instructions.end());
    assembled.insert(assembled.end(), merged.begin(), merged.end());
    for (const auto& message : trusted_supplemental) {
        // Built-in controlled calls may restate a raw user turn as their task
        // slot. The admitted RAW window remains authoritative and appears once.
        const auto duplicate_raw = std::any_of(raw_messages.begin(), raw_messages.end(),
            [&message](const sp::Message& raw) {
                return message_digest(raw) == message_digest(message) &&
                       raw.native == message.native;
            });
        if (!duplicate_raw) assembled.push_back(message);
    }
    const auto window_digest = identity("assembled-message-window/v2", messages_json(assembled));
    set_provider_request_messages(request, std::move(assembled));
    auto prepared = provider.prepare(std::move(request));
    if (!prepared.valid() || prepared.error()) {
        if (const auto* error = prepared.error()) {
            throw ProviderFailure(std::make_shared<const sp::Outcome>(
                sp::Failure{*error, {}}));
        }
        throw std::invalid_argument("Runtime provider preparation did not admit a request");
    }
    const auto normalized_digest = normalized_request_digest(prepared);
    ContextAssemblyReceiptData receipt_data;
    receipt_data.context_epoch_id = epoch.id();
    receipt_data.normalized_request_digest = normalized_digest;
    receipt_data.message_window_digest = window_digest;
    receipt_data.artifact_ids = epoch.artifact_ids();
    for (const auto& artifact : receipt_artifacts) {
        if (artifact.kind() == ContextArtifactKind::RequiredSkill) receipt_data.required_skill_artifact_ids.push_back(artifact.id());
    }
    receipt_data.raw_from_sequence = epoch.raw_from_sequence();
    receipt_data.raw_through_sequence = epoch.raw_through_sequence();
    receipt_data.estimated_input_tokens = estimate_input_tokens(prepared);
    receipt_data.mandatory_input_tokens = mandatory;
    if (impl_->max_input_tokens != 0 &&
        receipt_data.estimated_input_tokens > impl_->max_input_tokens) {
        throw ContextBudgetBlocked();
    }
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, receipt_artifacts);
    return {std::move(prepared), std::move(receipt)};
}

}  // namespace neograph
