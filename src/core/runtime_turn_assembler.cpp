#include <neograph/runtime_turn_assembler.h>

#include "canonical_json.h"
#include "prompt_prefix.h"

#include <algorithm>
#include <limits>
#include <map>
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

// Input and mandatory tokens share one basis: bytes the prepared request
// carries, three per token (#373).
std::uint64_t tokens_for_bytes(std::uint64_t bytes) {
    return bytes / 3u + (bytes % 3u != 0);
}

// A rendered artifact contributes its text to the request. Every provider
// encoding carries that text at least byte for byte; its role and framing are
// request overhead, counted in the input estimate only.
std::uint64_t text_bytes(const sp::Message& message) {
    std::uint64_t bytes = 0;
    for (const auto& part : message.parts) {
        if (const auto* text = std::get_if<sp::Text>(&part)) bytes += text->value.size();
    }
    return bytes;
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
    const auto role = context_artifact_kind_renders_as_system(artifact.kind())
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

// Digest of the tool definitions an encoded request body carries, which every
// family names `tools`; absent when the body has none. Tool definitions sit in
// front of the messages, so a change to them changes the whole request.
std::optional<std::string> tools_digest(std::string_view encoded_body) {
    try {
        const auto body = detail::parse_json_strict(encoded_body);
        if (!body.is_object() || !body.contains("tools")) return std::nullopt;
        return identity("provider-tools/v1", body.at("tools"));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The text of the message that marks where a prefix ends when the request is encoded again.
constexpr std::string_view PREFIX_MARKER = "neograph-prefix-boundary-marker-6f1d0c";

// The body of the request encoded with only the first `count` assembled messages and the marker after them.
// Nothing is sent and nothing is claimed: a probe that cannot be encoded is simply absent.
std::optional<std::string> encode_prefix_probe(
    Provider& provider, const ProviderRequest& request_template,
    const std::vector<sp::Message>& assembled, std::size_t count) {
    try {
        auto probe = request_template;
        probe.on_event = nullptr;
        std::vector<sp::Message> messages(assembled.begin(),
                                          assembled.begin() + static_cast<std::ptrdiff_t>(count));
        sp::Message marker;
        marker.role = sp::Role::User;
        marker.parts.emplace_back(sp::Text{std::string(PREFIX_MARKER)});
        messages.push_back(std::move(marker));
        set_provider_request_messages(probe, std::move(messages));
        auto prepared = provider.prepare(std::move(probe));
        if (!prepared.valid() || prepared.error()) return std::nullopt;
        return std::string(prepared.encoded_body());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct PrefixEncodings {
    std::optional<std::string> stable;
    std::optional<std::string> prefix;
};

[[noreturn]] void boundary_unavailable(const std::string& why) {
    throw ContextStabilityError(
        ContextStabilityRefusal::PrefixBoundaryUnavailable,
        "The encoded request does not show the prefix boundaries of the stable-first layout: " + why);
}

struct PrefixFields {
    std::optional<ContextPrefixBoundary> stable_boundary;
    std::optional<ContextPrefixBoundary> prefix_boundary;
    std::string prefix_digest;
};

// Where the parts end among the assembled messages.
struct PrefixShape {
    std::size_t messages = 0;      // assembled messages
    std::size_t stable_end = 0;    // host instructions and Stable artifacts
    std::size_t prefix_end = 0;    // everything in front of the Volatile artifacts
    bool system_after_stable = false;  // a Slow artifact renders as a System message
};

// Read the two boundaries from the body of the full request and the bodies of the probes.
PrefixFields read_prefix_fields(
    const PreparedProviderRequest& prepared, const PrefixEncodings& probes, const PrefixShape& shape) {
    const auto stable_end = shape.stable_end;
    const auto prefix_end = shape.prefix_end;
    const auto* descriptor = prepared.admitted_descriptor();
    if (!descriptor) boundary_unavailable("the request carries no admitted descriptor");
    const detail::WireMembers members{std::string(descriptor->request_messages_member()),
                                      std::string(descriptor->request_stream_member()),
                                      std::string(descriptor->max_output_tokens_member())};
    json full;
    try {
        full = detail::parse_json_strict(prepared.encoded_body());
    } catch (const std::exception&) {
        boundary_unavailable("the body is not JSON");
    }
    if (prepared.encoded_body().find(PREFIX_MARKER) != std::string_view::npos) {
        boundary_unavailable("the conversation contains the text that marks a boundary");
    }
    const auto read = [&](const std::optional<std::string>& body) {
        if (!body) boundary_unavailable("the prefix could not be encoded");
        return detail::locate_prefix_boundary(full, detail::parse_json_strict(*body), members, PREFIX_MARKER);
    };

    PrefixFields fields;
    std::vector<std::string> container;
    if (prefix_end == shape.messages) {
        fields.prefix_boundary = detail::whole_body_boundary(full, members);
    } else {
        const auto located = read(probes.prefix);
        if (located.status != detail::PrefixProbe::Located) {
            boundary_unavailable(located.status == detail::PrefixProbe::MembersDiffer
                                     ? "a member outside the messages changes behind the prefix"
                                     : "the marker is not where the prefix ends");
        }
        fields.prefix_boundary = located.boundary;
        container = located.container;
    }
    if (stable_end == prefix_end) {
        fields.stable_boundary = fields.prefix_boundary;
    } else if (stable_end > 0) {
        const auto located = read(probes.stable);
        if (located.status == detail::PrefixProbe::Located) {
            fields.stable_boundary = located.boundary;
        } else if (located.status == detail::PrefixProbe::MembersDiffer && shape.system_after_stable) {
            // A Slow System text shares the system field with the Stable one, and the field is one block:
            // the end of the Stable part cannot be marked on this family.
        } else {
            boundary_unavailable("the marker is not where the Stable part ends");
        }
    }
    if (fields.stable_boundary && *fields.prefix_boundary < *fields.stable_boundary) {
        boundary_unavailable("the Stable part ends behind the prefix");
    }
    fields.prefix_digest = detail::wire_prefix_digest(full, members, *fields.prefix_boundary, container);
    return fields;
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
    return tokens_for_bytes(request.encoded_body().size());
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
    const bool stable_first = epoch.assembly_layout() == AssemblyLayout::StableFirst;
    if (stable_first) {
        // The epoch records the order and nothing about the content takes part in it.
        validate_context_layout(epoch, receipt_artifacts);
        std::map<std::string, std::size_t> position;
        for (const auto& id : *epoch.ordered_artifact_ids()) position.emplace(id, position.size());
        std::sort(selected.begin(), selected.end(), [&](const SelectedArtifact& lhs, const SelectedArtifact& rhs) {
            return position.at(lhs.artifact.id()) < position.at(rhs.artifact.id());
        });
    } else {
        std::sort(selected.begin(), selected.end(), artifact_less);
    }
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
    // The ids of those messages in the order the prompt shows them.
    std::vector<std::string> before_history_ids, before_ids, after_user_ids, after_history_ids;
    std::uint64_t mandatory_bytes = 0;
    for (const auto& item : selected) {
        const auto rendered = render_artifact(item.artifact);
        if (item.artifact.required()) mandatory_bytes += text_bytes(rendered);
        if (item.artifact.placement() == ContextPlacement::BeforeHistory) {
            before_history.push_back(rendered);
            before_history_ids.push_back(item.artifact.id());
        } else if (item.artifact.placement() == ContextPlacement::BeforeLatestUser) {
            before.push_back(rendered);
            before_ids.push_back(item.artifact.id());
        } else if (item.artifact.placement() == ContextPlacement::AfterLatestUser) {
            after_user.push_back(rendered);
            after_user_ids.push_back(item.artifact.id());
        } else {
            after_history.push_back(rendered);
            after_history_ids.push_back(item.artifact.id());
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

    // Stable-first: where the Stable part and the append-only part end in the assembled messages. A System
    // message is hoisted into the start of the request by some families, so only the host instructions and
    // the Stable and Slow artifacts may be one: a RAW or supplemental System message would move the start.
    PrefixShape shape;
    PrefixEncodings probes;
    if (stable_first) {
        const auto hosts = host_instructions.size();
        const auto stable_count = static_cast<std::size_t>(std::count_if(
            selected.begin(), selected.end(), [](const SelectedArtifact& item) {
                return item.artifact.stability_class() == StabilityClass::Stable;
            }));
        shape.messages = assembled.size();
        shape.stable_end = hosts + stable_count;
        shape.prefix_end = hosts + before_history.size() + raw_messages.size();
        for (std::size_t index = 0; index < assembled.size(); ++index) {
            const bool head = index < hosts + before_history.size();
            if (assembled[index].role == sp::Role::System && !head) {
                throw ContextStabilityError(
                    ContextStabilityRefusal::VolatileSystemText,
                    "The stable-first layout allows a System message only as a host instruction or as a "
                    "Stable or Slow artifact; message " + std::to_string(index) + " is neither");
            }
        }
        const auto expected = *epoch.ordered_artifact_ids();
        std::vector<std::string> placed = before_history_ids;
        placed.insert(placed.end(), after_history_ids.begin(), after_history_ids.end());
        if (placed != expected) throw std::logic_error("Stable-first assembly did not follow the recorded artifact order");
        for (std::size_t index = shape.stable_end; index < shape.prefix_end; ++index) {
            if (assembled[index].role == sp::Role::System) shape.system_after_stable = true;
        }
        // The probes are encoded before the request itself, so that the request a provider sees last is the
        // real one.
        if (shape.prefix_end < shape.messages) {
            probes.prefix = encode_prefix_probe(provider, request, assembled, shape.prefix_end);
        }
        if (shape.stable_end > 0 && shape.stable_end < shape.prefix_end) {
            probes.stable = encode_prefix_probe(provider, request, assembled, shape.stable_end);
        }
    }

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
    receipt_data.mandatory_input_tokens = tokens_for_bytes(mandatory_bytes);
    receipt_data.assembled_artifact_ids.emplace();
    for (const auto* ids : {&before_history_ids, &before_ids, &after_user_ids, &after_history_ids}) {
        receipt_data.assembled_artifact_ids->insert(receipt_data.assembled_artifact_ids->end(),
                                                    ids->begin(), ids->end());
    }
    receipt_data.artifact_classes.emplace();
    for (const auto& item : selected) {
        receipt_data.artifact_classes->emplace(item.artifact.id(), item.artifact.stability_class());
    }
    receipt_data.encoder_family = std::string(prepared.family());
    receipt_data.tools_digest = tools_digest(prepared.encoded_body());
    receipt_data.break_reason = epoch.break_reason();
    if (stable_first) {
        auto fields = read_prefix_fields(prepared, probes, shape);
        receipt_data.stable_boundary = fields.stable_boundary;
        receipt_data.prefix_boundary = fields.prefix_boundary;
        receipt_data.prefix_digest = std::move(fields.prefix_digest);
    }
    if (impl_->max_input_tokens != 0 &&
        receipt_data.estimated_input_tokens > impl_->max_input_tokens) {
        throw ContextBudgetBlocked();
    }
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, receipt_artifacts);
    return {std::move(prepared), std::move(receipt)};
}

}  // namespace neograph
