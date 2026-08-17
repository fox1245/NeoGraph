#include <neograph/runtime_turn_assembler.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neograph {
namespace {

constexpr std::string_view IDENTITY_PREAMBLE = "NeoGraph Runtime turn identity v1";

std::string identity(std::string_view domain, const json& value) {
    return detail::sha256_identity(IDENTITY_PREAMBLE, domain, detail::canonical_json_bytes(value));
}

std::string mode_name(CompletionMode mode) {
    switch (mode) {
        case CompletionMode::COLLECT: return "collect";
        case CompletionMode::STREAM: return "stream";
    }
    throw std::invalid_argument("Completion mode is invalid");
}

json messages_json(const std::vector<ChatMessage>& messages) {
    json result = json::array();
    for (const auto& message : messages) {
        json value;
        to_json(value, message);
        result.push_back(std::move(value));
    }
    return result;
}

json tools_json(const std::vector<ChatTool>& tools) {
    json result = json::array();
    for (const auto& tool : tools) {
        result.push_back(json{{"name", tool.name}, {"description", tool.description},
                              {"parameters", tool.parameters}});
    }
    return result;
}

json request_json(const CompletionRequest& request) {
    const auto& params = request.params();
    return json{{"mode", mode_name(request.mode())},
                {"model", params.model},
                {"messages", messages_json(params.messages)},
                {"tools", tools_json(params.tools)},
                {"temperature", params.temperature},
                {"max_tokens", params.max_tokens},
                {"extra_fields", params.extra_fields},
                {"timeout_seconds", params.timeout_seconds}};
}

std::uint64_t estimate_json_tokens(const json& value) {
    const auto bytes = detail::canonical_json_bytes(value).size();
    return static_cast<std::uint64_t>(bytes / 3u + (bytes % 3u != 0));
}

void verify_history(const std::string& jsonl, const ContextEpoch& epoch,
                     std::vector<ChatMessage>& messages) {
    std::size_t start = 0;
    std::uint64_t expected = epoch.raw_from_sequence();
    std::optional<std::string> predecessor;
    while (start <= jsonl.size()) {
        const auto end = jsonl.find('\n', start);
        const auto line = jsonl.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto record = RuntimeHistoryRecord::parse(line);
        if (record.feed_id() != epoch.feed_id() || record.sequence() != expected ||
            (expected > 1 && !predecessor && !record.predecessor_id()) ||
            (predecessor && (!record.predecessor_id() || *record.predecessor_id() != *predecessor))) {
            throw std::invalid_argument("Context epoch raw history does not form its declared chain");
        }
        predecessor = record.id();
        messages.push_back(record.message());
        if (end == std::string::npos) break;
        start = end + 1;
        ++expected;
    }
    if (messages.size() != epoch.raw_through_sequence() - epoch.raw_from_sequence() + 1 ||
        expected != epoch.raw_through_sequence()) {
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

ChatMessage render_artifact(const ContextArtifact& artifact) {
    if (artifact.media_type() != "text/plain" && artifact.media_type() != "text/markdown") {
        throw std::invalid_argument("Context assembly only renders text/plain or text/markdown artifacts");
    }
    const auto content = artifact.content();
    if (content.is_string()) return {"system", content.get<std::string>()};
    if (!content.is_object() || content.size() != 1 || !content.contains("text") ||
        !content.at("text").is_string()) {
        throw std::invalid_argument("Context artifact text content must be a string or exactly {text: string}");
    }
    return {"system", content.at("text").get<std::string>()};
}

}  // namespace

struct RuntimeTurnAssembler::Impl {
    ContextStore* store;
    std::vector<std::string> static_required_skill_artifact_ids;
    std::uint64_t max_input_tokens;
};

ContextBudgetBlocked::ContextBudgetBlocked()
    : std::runtime_error("context_budget_blocked") {}

RuntimeTurnAssembler::RuntimeTurnAssembler(
    ContextStore& store, std::vector<std::string> static_required_skill_artifact_ids,
    std::uint64_t max_input_tokens) {
    for (const auto& id : static_required_skill_artifact_ids) {
        if (!detail::is_sha256_identity(id)) {
            throw std::invalid_argument("Static required skill artifact id must be a sha256 identity");
        }
    }
    std::sort(static_required_skill_artifact_ids.begin(), static_required_skill_artifact_ids.end());
    if (std::adjacent_find(static_required_skill_artifact_ids.begin(),
                           static_required_skill_artifact_ids.end()) !=
        static_required_skill_artifact_ids.end()) {
        throw std::invalid_argument("Static required skill artifact ids contain a duplicate");
    }
    impl_ = std::make_unique<Impl>(
        Impl{&store, std::move(static_required_skill_artifact_ids), max_input_tokens});
}
RuntimeTurnAssembler::~RuntimeTurnAssembler() = default;
RuntimeTurnAssembler::RuntimeTurnAssembler(RuntimeTurnAssembler&&) noexcept = default;
RuntimeTurnAssembler& RuntimeTurnAssembler::operator=(RuntimeTurnAssembler&&) noexcept = default;

std::uint64_t RuntimeTurnAssembler::estimate_input_tokens(const CompletionParams& params) {
    json value{{"model", params.model}, {"messages", messages_json(params.messages)},
               {"tools", tools_json(params.tools)}, {"temperature", params.temperature},
               {"max_tokens", params.max_tokens}, {"extra_fields", params.extra_fields},
               {"timeout_seconds", params.timeout_seconds}};
    return estimate_json_tokens(value);
}

std::string RuntimeTurnAssembler::normalized_request_digest(const CompletionRequest& request) {
    return identity("normalized-completion-request/v1", request_json(request));
}

RuntimeTurn RuntimeTurnAssembler::assemble(std::string owner_id,
                                           const ContextEpoch& epoch,
                                           CompletionRequest request) const {
    detail::validate_token(owner_id, "Context assembly owner_id");
    auto& params = request.params();
    detail::validate_token(params.model, "Completion model");
    if (!params.messages.empty()) {
        throw std::invalid_argument("Runtime turn request template must not contain messages");
    }
    if (!std::includes(epoch.artifact_ids().begin(), epoch.artifact_ids().end(),
                       impl_->static_required_skill_artifact_ids.begin(),
                       impl_->static_required_skill_artifact_ids.end())) {
        throw std::invalid_argument("Context epoch omits a static required skill artifact");
    }
    if (epoch.guarantee_profile() == RuntimeGuaranteeProfile::Strict &&
        impl_->max_input_tokens == 0) {
        throw std::invalid_argument("Strict context assembly requires an input token budget");
    }

    std::vector<ChatMessage> raw_messages;
    if (epoch.raw_from_sequence() != 0) {
        const ContextStoreFeed feed{owner_id, epoch.feed_id()};
        const auto range = impl_->store->snapshot_history(feed, epoch.raw_from_sequence(),
                                                           epoch.raw_through_sequence());
        if (range.digest != epoch.raw_window_digest()) {
            throw std::invalid_argument("Context epoch raw window digest does not match the feed");
        }
        verify_history(impl_->store->hydrate_history(range), epoch, raw_messages);
    }

    std::vector<SelectedArtifact> selected;
    std::vector<ContextArtifact> receipt_artifacts;
    selected.reserve(epoch.artifact_ids().size());
    receipt_artifacts.reserve(epoch.artifact_ids().size());
    for (const auto& id : epoch.artifact_ids()) {
        const auto artifact = impl_->store->get_artifact(owner_id, id);
        if (!artifact) throw std::invalid_argument("Context epoch artifact is not available to this owner");
        const bool static_required = std::binary_search(
            impl_->static_required_skill_artifact_ids.begin(),
            impl_->static_required_skill_artifact_ids.end(), id);
        if (artifact->kind() == ContextArtifactKind::RequiredSkill && !static_required) {
            throw std::invalid_argument("Context epoch selects an unadmitted required skill artifact");
        }
        if (static_required && (artifact->kind() != ContextArtifactKind::RequiredSkill ||
                                !artifact->required())) {
            throw std::invalid_argument("Configured skill identity is not a required skill artifact");
        }
        selected.push_back({*artifact});
        receipt_artifacts.push_back(*artifact);
    }
    std::sort(selected.begin(), selected.end(), artifact_less);
    if (!selected.empty() && std::none_of(raw_messages.begin(), raw_messages.end(),
                                          [](const ChatMessage& message) {
                                              return message.role == "user";
                                          })) {
        throw std::invalid_argument("Selected context artifacts require a raw history user message");
    }

    std::vector<ChatMessage> before;
    std::vector<ChatMessage> after;
    std::uint64_t mandatory = 0;
    for (const auto& item : selected) {
        const auto rendered = render_artifact(item.artifact);
        if (item.artifact.kind() == ContextArtifactKind::RequiredSkill) {
            mandatory += estimate_json_tokens(messages_json({rendered}));
        }
        (item.artifact.placement() == ContextPlacement::BeforeLatestUser ? before : after)
            .push_back(rendered);
    }

    std::vector<ChatMessage> merged;
    const auto last_user = std::find_if(raw_messages.rbegin(), raw_messages.rend(),
                                        [](const ChatMessage& message) { return message.role == "user"; });
    const auto insertion = last_user == raw_messages.rend()
                               ? raw_messages.size()
                               : static_cast<std::size_t>(std::distance(last_user, raw_messages.rend()) - 1);
    merged.insert(merged.end(), raw_messages.begin(), raw_messages.begin() + insertion);
    merged.insert(merged.end(), before.begin(), before.end());
    if (insertion < raw_messages.size()) {
        merged.push_back(raw_messages[insertion]);
        merged.insert(merged.end(), after.begin(), after.end());
        merged.insert(merged.end(), raw_messages.begin() + insertion + 1, raw_messages.end());
    } else {
        merged.insert(merged.end(), after.begin(), after.end());
    }
    params.messages = std::move(merged);

    const auto normalized_digest = normalized_request_digest(request);
    const auto window_digest = identity("assembled-message-window/v1", messages_json(params.messages));
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
    receipt_data.estimated_input_tokens = estimate_input_tokens(params);
    receipt_data.mandatory_input_tokens = mandatory;
    if (impl_->max_input_tokens != 0 &&
        receipt_data.estimated_input_tokens > impl_->max_input_tokens) {
        throw ContextBudgetBlocked();
    }
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, receipt_artifacts);
    return {std::move(request), std::move(receipt)};
}

}  // namespace neograph
