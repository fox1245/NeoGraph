#include <neograph/a2a/collaboration.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/program/runtime.h>
#endif

#include <array>
#include <bit>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
 

namespace neograph::a2a {
namespace {

constexpr std::string_view COLLABORATION_FORMAT = "neograph-a2a-collaboration-v1";

std::uint64_t now_unix_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string sha256(std::string_view input) {
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                           0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                           0x1f83d9abu, 0x5be0cd19u};
    const auto compress = [&state](const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto base = i * 4;
            words[i] = (static_cast<std::uint32_t>(block[base]) << 24) |
                       (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                       static_cast<std::uint32_t>(block[base + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
            const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state[0], b = state[1], c = state[2], d = state[3];
        auto e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    };
    const auto full_blocks = input.size() / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        compress(reinterpret_cast<const std::uint8_t*>(input.data() + i * 64));
    }
    std::array<std::uint8_t, 128> tail{};
    const auto remainder = input.size() % 64;
    for (std::size_t i = 0; i < remainder; ++i)
        tail[i] = static_cast<std::uint8_t>(static_cast<unsigned char>(input[full_blocks * 64 + i]));
    tail[remainder] = 0x80;
    const std::size_t tail_size = remainder < 56 ? 64 : 128;
    const auto bit_length = static_cast<std::uint64_t>(input.size()) * 8;
    for (std::size_t i = 0; i < 8; ++i)
        tail[tail_size - 1 - i] = static_cast<std::uint8_t>(bit_length >> (i * 8));
    compress(tail.data());
    if (tail_size == 128) compress(tail.data() + 64);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (const auto word : state) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto byte = static_cast<std::uint8_t>(word >> shift);
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }
    return result;
}

std::string required_string(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_string()) {
        throw std::invalid_argument("Collaboration field '" + key + "' must be a string");
    }
    return object[key].get<std::string>();
}

std::uint64_t required_unsigned(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_number_unsigned()) {
        throw std::invalid_argument("Collaboration field '" + key + "' must be unsigned");
    }
    return object[key].get<std::uint64_t>();
}

json required_object(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_object()) {
        throw std::invalid_argument("Collaboration field '" + key + "' must be an object");
    }
    return object[key];
}

json required_array(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_array()) {
        throw std::invalid_argument("Collaboration field '" + key + "' must be an array");
    }
    return object[key];
}

std::vector<std::string> string_array(const json& values, std::string_view field) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& item : values) {
        if (!item.is_string()) {
            throw std::invalid_argument("Collaboration array '" + std::string(field) +
                                        "' must contain strings");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

json to_string_array(const std::vector<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

void require_nonempty(std::string_view value, std::string_view field) {
    if (value.empty()) {
        throw std::invalid_argument("Collaboration field '" + std::string(field) +
                                    "' must not be empty");
    }
}

void require_unique_strings(const std::vector<std::string>& values, std::string_view field) {
    std::set<std::string> unique;
    for (const auto& value : values) {
        require_nonempty(value, field);
        if (!unique.insert(value).second) {
            throw std::invalid_argument("Collaboration field '" + std::string(field) +
                                        "' contains a duplicate");
        }
    }
}

bool contains(const std::vector<std::string>& values, std::string_view value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

json link_spec_to_json(const CollaborationLinkSpec& spec) {
    return json{{"schema_version", spec.schema_version},
                {"link_id", spec.link_id},
                {"sender_owner_scope", spec.sender_owner_scope},
                {"receiver_owner_scope", spec.receiver_owner_scope},
                {"sender_agent_id", spec.sender_agent_id},
                {"receiver_agent_id", spec.receiver_agent_id},
                {"task_scope", spec.task_scope},
                {"capability_allowlist", to_string_array(spec.capability_allowlist)},
                {"effect_allowlist", to_string_array(spec.effect_allowlist)},
                {"artifact_allowlist", to_string_array(spec.artifact_allowlist)},
                {"cancellation_rights", to_string_array(spec.cancellation_rights)},
                {"expires_at_unix_ms", spec.expires_at_unix_ms},
                {"max_retries", spec.max_retries},
                {"acknowledgement_timeout_ms", spec.acknowledgement_timeout_ms}};
}

CollaborationLinkSpec link_spec_from_json(const json& value) {
    CollaborationLinkSpec spec;
    spec.schema_version = static_cast<std::uint32_t>(required_unsigned(value, "schema_version"));
    spec.link_id = required_string(value, "link_id");
    spec.sender_owner_scope = required_string(value, "sender_owner_scope");
    spec.receiver_owner_scope = required_string(value, "receiver_owner_scope");
    spec.sender_agent_id = required_string(value, "sender_agent_id");
    spec.receiver_agent_id = required_string(value, "receiver_agent_id");
    spec.task_scope = required_string(value, "task_scope");
    spec.capability_allowlist = string_array(required_array(value, "capability_allowlist"),
                                             "capability_allowlist");
    spec.effect_allowlist = string_array(required_array(value, "effect_allowlist"), "effect_allowlist");
    spec.artifact_allowlist = string_array(required_array(value, "artifact_allowlist"), "artifact_allowlist");
    spec.cancellation_rights = string_array(required_array(value, "cancellation_rights"),
                                            "cancellation_rights");
    spec.expires_at_unix_ms = required_unsigned(value, "expires_at_unix_ms");
    spec.max_retries = static_cast<std::uint32_t>(required_unsigned(value, "max_retries"));
    spec.acknowledgement_timeout_ms = required_unsigned(value, "acknowledgement_timeout_ms");
    return spec;
}

json link_payload(const CollaborationLinkSpec& spec,
                  CollaborationLinkState state,
                  std::string_view consent_fingerprint) {
    return json{{"format", std::string(COLLABORATION_FORMAT)},
                {"state", std::string(to_string(state))},
                {"spec", link_spec_to_json(spec)},
                {"consent_fingerprint", std::string(consent_fingerprint)}};
}

std::string link_hash(const CollaborationLinkSpec& spec,
                      CollaborationLinkState state,
                      std::string_view consent_fingerprint) {
    return sha256(link_payload(spec, state, consent_fingerprint).dump());
}

void validate_link_spec(const CollaborationLinkSpec& spec) {
    if (spec.schema_version != CollaborationLink::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported collaboration link schema version");
    }
    require_nonempty(spec.link_id, "link_id");
    require_nonempty(spec.sender_owner_scope, "sender_owner_scope");
    require_nonempty(spec.receiver_owner_scope, "receiver_owner_scope");
    require_nonempty(spec.sender_agent_id, "sender_agent_id");
    require_nonempty(spec.receiver_agent_id, "receiver_agent_id");
    require_nonempty(spec.task_scope, "task_scope");
    if (spec.sender_owner_scope == spec.receiver_owner_scope) {
        throw std::invalid_argument("Collaboration link must cross owner scopes");
    }
    if (spec.sender_agent_id == spec.receiver_agent_id) {
        throw std::invalid_argument("Collaboration link requires distinct agent identities");
    }
    require_unique_strings(spec.capability_allowlist, "capability_allowlist");
    require_unique_strings(spec.effect_allowlist, "effect_allowlist");
    require_unique_strings(spec.artifact_allowlist, "artifact_allowlist");
    require_unique_strings(spec.cancellation_rights, "cancellation_rights");
    if (spec.expires_at_unix_ms == 0 || spec.acknowledgement_timeout_ms == 0) {
        throw std::invalid_argument("Collaboration link expiry and acknowledgement timeout are required");
    }
    if (spec.max_retries > 1000) {
        throw std::invalid_argument("Collaboration link retry limit is unreasonable");
    }
    for (const auto& right : spec.cancellation_rights) {
        if (right != "sender" && right != "receiver") {
            throw std::invalid_argument("Unknown collaboration cancellation right");
        }
    }
}

json artifact_to_json(const CollaborationArtifactReference& artifact) {
    return json{{"artifact_identity", artifact.artifact_identity},
                {"uri", artifact.uri},
                {"media_type", artifact.media_type},
                {"size_bytes", artifact.size_bytes}};
}

CollaborationArtifactReference artifact_from_json(const json& value) {
    return CollaborationArtifactReference{required_string(value, "artifact_identity"),
                                         required_string(value, "uri"),
                                         required_string(value, "media_type"),
                                         required_unsigned(value, "size_bytes")};
}

json envelope_to_json(const CollaborationEnvelope& envelope) {
    json artifacts = json::array();
    for (const auto& artifact : envelope.artifacts) {
        artifacts.push_back(artifact_to_json(artifact));
    }
    return json{{"storage_schema_version", CollaborationEnvelope::STORAGE_SCHEMA_VERSION},
                {"link_id", envelope.link_id},
                {"sender_owner_scope", envelope.sender_owner_scope},
                {"receiver_owner_scope", envelope.receiver_owner_scope},
                {"sender_agent_id", envelope.sender_agent_id},
                {"receiver_agent_id", envelope.receiver_agent_id},
                {"sender_program_run_id", envelope.sender_program_run_id},
                {"receiver_program_run_id", envelope.receiver_program_run_id},
                {"program_version_id", envelope.program_version_id},
                {"a2a_task_id", envelope.a2a_task_id},
                {"a2a_context_id", envelope.a2a_context_id},
                {"message_id", envelope.message_id},
                {"correlation_id", envelope.correlation_id},
                {"sequence", envelope.sequence},
                {"kind", envelope.kind},
                {"idempotency_key", envelope.idempotency_key},
                {"payload", envelope.payload},
                {"artifacts", artifacts}};
}

void validate_envelope_shape(const CollaborationEnvelope& envelope) {
    require_nonempty(envelope.link_id, "link_id");
    require_nonempty(envelope.sender_owner_scope, "sender_owner_scope");
    require_nonempty(envelope.receiver_owner_scope, "receiver_owner_scope");
    require_nonempty(envelope.sender_agent_id, "sender_agent_id");
    require_nonempty(envelope.receiver_agent_id, "receiver_agent_id");
    require_nonempty(envelope.sender_program_run_id, "sender_program_run_id");
    require_nonempty(envelope.receiver_program_run_id, "receiver_program_run_id");
    require_nonempty(envelope.a2a_task_id, "a2a_task_id");
    require_nonempty(envelope.a2a_context_id, "a2a_context_id");
    require_nonempty(envelope.message_id, "message_id");
    require_nonempty(envelope.correlation_id, "correlation_id");
    require_nonempty(envelope.kind, "kind");
    require_nonempty(envelope.idempotency_key, "idempotency_key");
    if (envelope.sequence == 0) {
        throw std::invalid_argument("Collaboration sequence must start at one");
    }
    if (!envelope.payload.is_object() && !envelope.payload.is_array()) {
        throw std::invalid_argument("Collaboration payload must be structured JSON");
    }
    std::set<std::string> artifact_ids;
    for (const auto& artifact : envelope.artifacts) {
        require_nonempty(artifact.artifact_identity, "artifact_identity");
        require_nonempty(artifact.uri, "uri");
        require_nonempty(artifact.media_type, "media_type");
        if (!artifact_ids.insert(artifact.artifact_identity).second) {
            throw std::invalid_argument("Collaboration envelope contains duplicate artifact identity");
        }
    }
}

CollaborationEnvelope envelope_from_json(const json& value) {
    if (required_unsigned(value, "storage_schema_version") !=
        CollaborationEnvelope::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported collaboration envelope schema version");
    }
    CollaborationEnvelope envelope;
    envelope.link_id = required_string(value, "link_id");
    envelope.sender_owner_scope = required_string(value, "sender_owner_scope");
    envelope.receiver_owner_scope = required_string(value, "receiver_owner_scope");
    envelope.sender_agent_id = required_string(value, "sender_agent_id");
    envelope.receiver_agent_id = required_string(value, "receiver_agent_id");
    envelope.sender_program_run_id = required_string(value, "sender_program_run_id");
    envelope.receiver_program_run_id = required_string(value, "receiver_program_run_id");
    if (value.contains("program_version_id")) {
        envelope.program_version_id = required_string(value, "program_version_id");
    }
    envelope.a2a_task_id = required_string(value, "a2a_task_id");
    envelope.a2a_context_id = required_string(value, "a2a_context_id");
    envelope.message_id = required_string(value, "message_id");
    envelope.correlation_id = required_string(value, "correlation_id");
    envelope.sequence = required_unsigned(value, "sequence");
    envelope.kind = required_string(value, "kind");
    envelope.idempotency_key = required_string(value, "idempotency_key");
    if (!value.contains("payload")) {
        throw std::invalid_argument("Collaboration payload is required");
    }
    envelope.payload = value["payload"];
    for (const auto& artifact : required_array(value, "artifacts")) {
        envelope.artifacts.push_back(artifact_from_json(artifact));
    }
    validate_envelope_shape(envelope);
    return envelope;
}

json record_to_json(const CollaborationRecord& record) {
#ifdef NEOGRAPH_A2A_PROGRAM
    json program_request = nullptr;
    if (record.program_request) {
        if (!record.program_request->version || !record.program_request->invocation) {
            throw std::invalid_argument("Collaboration Program request is incomplete");
        }
        const auto& invocation = *record.program_request->invocation;
        program_request = json{
            {"version", json::parse(record.program_request->version->serialize_canonical())},
            {"invocation", json{{"input", invocation.input},
                                 {"wall_time_ms", invocation.budget.wall_time_ms},
                                 {"model_tokens", invocation.budget.model_tokens},
                                 {"monetary_microunits", invocation.budget.monetary_microunits},
                                 {"max_concurrency", invocation.budget.max_concurrency},
                                 {"max_program_operations", invocation.budget.max_program_operations},
                                 {"max_core_steps", invocation.budget.max_core_steps},
                                 {"max_dynamic_compiles", invocation.budget.max_dynamic_compiles},
                                 {"max_child_depth", invocation.budget.max_child_depth},
                                 {"max_total_children", invocation.budget.max_total_children},
                                 {"trace_id", invocation.trace_id},
                                 {"requested_run_id", invocation.requested_run_id},
                                 {"parent_run_id", invocation.parent_run_id},
                                 {"child_depth", invocation.child_depth}}}};
    }
    return json{{"envelope", envelope_to_json(record.envelope)},
                {"state", std::string(to_string(record.state))},
                {"diagnostic", record.diagnostic},
                {"program_request", std::move(program_request)}};
#else
    return json{{"envelope", envelope_to_json(record.envelope)},
                {"state", std::string(to_string(record.state))},
                {"diagnostic", record.diagnostic}};
#endif
}

CollaborationRecord record_from_json(const json& value) {
    CollaborationRecord result{envelope_from_json(required_object(value, "envelope")),
                                collaboration_record_state_from_string(required_string(value, "state")),
                                required_string(value, "diagnostic")};
#ifdef NEOGRAPH_A2A_PROGRAM
    if (value.contains("program_request") && !value["program_request"].is_null()) {
        const auto request = required_object(value, "program_request");
        const auto version = program::ProgramVersion::parse(
            required_object(request, "version").dump());
        if (!result.envelope.program_version_id.empty() &&
            result.envelope.program_version_id != version.id()) {
            throw std::invalid_argument("Collaboration ProgramVersion identity mismatch");
        }
        const auto invocation_json = required_object(request, "invocation");
        auto required_u64 = [&](std::string_view field) {
            return required_unsigned(invocation_json, field);
        };
        auto required_u32 = [&](std::string_view field) {
            const auto value_u64 = required_u64(field);
            if (value_u64 > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Collaboration Program invocation integer exceeds uint32 range");
            }
            return static_cast<std::uint32_t>(value_u64);
        };
        program::ProgramInvocation invocation;
        if (!invocation_json.contains("input")) {
            throw std::invalid_argument("Collaboration Program invocation input is required");
        }
        invocation.input = invocation_json["input"];
        invocation.budget.wall_time_ms = required_u64("wall_time_ms");
        invocation.budget.model_tokens = required_u64("model_tokens");
        invocation.budget.monetary_microunits = required_u64("monetary_microunits");
        invocation.budget.max_concurrency = required_u32("max_concurrency");
        invocation.budget.max_program_operations = required_u64("max_program_operations");
        invocation.budget.max_core_steps = required_u64("max_core_steps");
        invocation.budget.max_dynamic_compiles = required_u64("max_dynamic_compiles");
        invocation.budget.max_child_depth = required_u32("max_child_depth");
        invocation.budget.max_total_children = required_u64("max_total_children");
        invocation.trace_id = required_string(invocation_json, "trace_id");
        invocation.requested_run_id = required_string(invocation_json, "requested_run_id");
        invocation.parent_run_id = required_string(invocation_json, "parent_run_id");
        invocation.child_depth = required_u32("child_depth");
        result.program_request = CollaborationRecord::ProgramRequest{
            std::make_shared<const program::ProgramVersion>(version),
            std::make_shared<const program::ProgramInvocation>(std::move(invocation))};
    }
#endif
    return result;
}

}  // namespace

std::string_view to_string(CollaborationLinkState state) noexcept {
    switch (state) {
        case CollaborationLinkState::Proposed:
            return "proposed";
        case CollaborationLinkState::Accepted:
            return "accepted";
        case CollaborationLinkState::Revoked:
            return "revoked";
    }
    return "unknown";
}

CollaborationLinkState collaboration_link_state_from_string(std::string_view value) {
    if (value == "proposed") return CollaborationLinkState::Proposed;
    if (value == "accepted") return CollaborationLinkState::Accepted;
    if (value == "revoked") return CollaborationLinkState::Revoked;
    throw std::invalid_argument("Unknown collaboration link state");
}

struct CollaborationLink::Impl {
    CollaborationLinkSpec spec;
    CollaborationLinkState state = CollaborationLinkState::Proposed;
    std::string consent_fingerprint;
    std::string content_hash;
};

CollaborationLink::CollaborationLink(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

CollaborationLink CollaborationLink::create(CollaborationLinkSpec spec) {
    validate_link_spec(spec);
    auto impl = std::make_shared<Impl>();
    impl->spec = std::move(spec);
    impl->content_hash = link_hash(impl->spec, impl->state, impl->consent_fingerprint);
    return CollaborationLink(std::move(impl));
}

CollaborationLink CollaborationLink::parse(std::string_view stored_bytes) {
    const auto value = json::parse(stored_bytes);
    if (!value.contains("format") || required_string(value, "format") != COLLABORATION_FORMAT) {
        throw std::invalid_argument("Unknown collaboration link format");
    }
    if (required_unsigned(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported collaboration link storage schema version");
    }
    const auto spec = link_spec_from_json(required_object(value, "spec"));
    validate_link_spec(spec);
    auto impl = std::make_shared<Impl>();
    impl->spec = spec;
    impl->state = collaboration_link_state_from_string(required_string(value, "state"));
    impl->consent_fingerprint = required_string(value, "consent_fingerprint");
    impl->content_hash = required_string(value, "content_hash");
    if (impl->state == CollaborationLinkState::Accepted && impl->consent_fingerprint.empty()) {
        throw std::invalid_argument("Accepted collaboration link lacks consent proof");
    }
    if (link_hash(impl->spec, impl->state, impl->consent_fingerprint) != impl->content_hash) {
        throw std::invalid_argument("Collaboration link content hash mismatch");
    }
    return CollaborationLink(std::move(impl));
}

CollaborationLink CollaborationLink::accept(std::string receiver_agent_id,
                                             std::string consent_token) const {
    if (state() != CollaborationLinkState::Proposed) {
        throw std::logic_error("Only a proposed collaboration link may be accepted");
    }
    if (receiver_agent_id != spec().receiver_agent_id) {
        throw std::invalid_argument("Collaboration consent identity does not match receiver");
    }
    require_nonempty(consent_token, "consent_token");
    if (is_expired(now_unix_ms())) {
        throw std::runtime_error("Collaboration link invitation has expired");
    }
    auto impl = std::make_shared<Impl>(*impl_);
    impl->state = CollaborationLinkState::Accepted;
    impl->consent_fingerprint = sha256(consent_token);
    impl->content_hash = link_hash(impl->spec, impl->state, impl->consent_fingerprint);
    return CollaborationLink(std::move(impl));
}

CollaborationLink CollaborationLink::revoke(std::string actor_agent_id) const {
    if (state() == CollaborationLinkState::Revoked) {
        return *this;
    }
    if (actor_agent_id != spec().sender_agent_id && actor_agent_id != spec().receiver_agent_id) {
        throw std::invalid_argument("Only link participants may revoke collaboration");
    }
    auto impl = std::make_shared<Impl>(*impl_);
    impl->state = CollaborationLinkState::Revoked;
    impl->content_hash = link_hash(impl->spec, impl->state, impl->consent_fingerprint);
    return CollaborationLink(std::move(impl));
}

CollaborationLinkState CollaborationLink::state() const noexcept {
    return impl_->state;
}

const CollaborationLinkSpec& CollaborationLink::spec() const noexcept {
    return impl_->spec;
}

const std::string& CollaborationLink::consent_fingerprint() const noexcept {
    return impl_->consent_fingerprint;
}

const std::string& CollaborationLink::content_hash() const noexcept {
    return impl_->content_hash;
}

bool CollaborationLink::is_expired(std::uint64_t now) const noexcept {
    return spec().expires_at_unix_ms <= now;
}

bool CollaborationLink::permits_capability(std::string_view capability) const noexcept {
    return state() == CollaborationLinkState::Accepted && !is_expired(now_unix_ms()) &&
           contains(spec().capability_allowlist, capability);
}

bool CollaborationLink::permits_effect(std::string_view effect) const noexcept {
    return state() == CollaborationLinkState::Accepted && !is_expired(now_unix_ms()) &&
           contains(spec().effect_allowlist, effect);
}

bool CollaborationLink::permits_artifact(std::string_view artifact_identity) const noexcept {
    return state() == CollaborationLinkState::Accepted && !is_expired(now_unix_ms()) &&
           contains(spec().artifact_allowlist, artifact_identity);
}

bool CollaborationLink::permits_cancellation(std::string_view actor_agent_id) const noexcept {
    if (state() != CollaborationLinkState::Accepted || is_expired(now_unix_ms())) return false;
    const std::string_view side = actor_agent_id == spec().sender_agent_id ? "sender" :
                                  actor_agent_id == spec().receiver_agent_id ? "receiver" : "";
    return !side.empty() && contains(spec().cancellation_rights, side);
}

std::string CollaborationLink::serialize_canonical() const {
    auto value = link_payload(spec(), state(), consent_fingerprint());
    value["storage_schema_version"] = STORAGE_SCHEMA_VERSION;
    value["content_hash"] = content_hash();
    return value.dump();
}

CollaborationEnvelope CollaborationEnvelope::create(
    const CollaborationLink& link,
    std::string sender_program_run_id,
    std::string receiver_program_run_id,
    std::string a2a_task_id,
    std::string a2a_context_id,
    std::string message_id,
    std::string correlation_id,
    std::uint64_t sequence,
    std::string kind,
    std::string idempotency_key,
    json payload,
    std::vector<CollaborationArtifactReference> artifacts) {
    if (link.state() != CollaborationLinkState::Accepted || link.is_expired(now_unix_ms())) {
        throw std::invalid_argument("Collaboration envelope requires an accepted, live link");
    }
    CollaborationEnvelope envelope{link.spec().link_id,
                                   link.spec().sender_owner_scope,
                                   link.spec().receiver_owner_scope,
                                   link.spec().sender_agent_id,
                                   link.spec().receiver_agent_id,
                                   std::move(sender_program_run_id),
                                   std::move(receiver_program_run_id),
                                   {},
                                   std::move(a2a_task_id),
                                   std::move(a2a_context_id),
                                   std::move(message_id),
                                   std::move(correlation_id),
                                   sequence,
                                   std::move(kind),
                                   std::move(idempotency_key),
                                   std::move(payload),
                                   std::move(artifacts)};
    validate_envelope_shape(envelope);
    for (const auto& artifact : envelope.artifacts) {
        if (!link.permits_artifact(artifact.artifact_identity)) {
            throw std::invalid_argument("Collaboration artifact is outside the link allowlist");
        }
    }
    return envelope;
}

#ifdef NEOGRAPH_A2A_PROGRAM
CollaborationEnvelope CollaborationEnvelope::bind_program(
    const CollaborationEnvelope& envelope, const program::ProgramVersion& version) {
    if (version.ownership_scope() != envelope.receiver_owner_scope) {
        throw std::invalid_argument(
            "Collaboration ProgramVersion owner does not match receiver owner scope");
    }
    auto bound = envelope;
    bound.program_version_id = version.id();
    validate_envelope_shape(bound);
    return bound;
}
#endif

CollaborationEnvelope CollaborationEnvelope::parse(std::string_view stored_bytes) {
    return envelope_from_json(json::parse(stored_bytes));
}

std::string CollaborationEnvelope::serialize_canonical() const {
    validate_envelope_shape(*this);
    return envelope_to_json(*this).dump();
}

std::string CollaborationEnvelope::content_hash() const {
    return sha256(serialize_canonical());
}

bool CollaborationEnvelope::is_terminal() const noexcept {
    return kind == "completed" || kind == "failed" || kind == "canceled" || kind == "rejected" ||
           kind == "auth-required" || kind == "terminal";
}

Message collaboration_to_message(const CollaborationEnvelope& envelope) {
    const auto bytes = envelope.serialize_canonical();
    Message message;
    message.message_id = envelope.message_id;
    message.role = Role::User;
    message.task_id = envelope.a2a_task_id;
    message.context_id = envelope.a2a_context_id;
    Part part;
    part.kind = "data";
    part.data = json{{"format", std::string(COLLABORATION_FORMAT)},
                    {"envelope", json::parse(bytes)}};
    message.parts.push_back(std::move(part));
    message.metadata = json{{"neograph_collaboration", true},
                            {"link_id", envelope.link_id},
                            {"correlation_id", envelope.correlation_id},
                            {"idempotency_key", envelope.idempotency_key}};
    if (!envelope.program_version_id.empty()) {
        message.metadata["program_version_id"] = envelope.program_version_id;
    }
    return message;
}

CollaborationEnvelope collaboration_from_message(const Message& message) {
    for (const auto& part : message.parts) {
        if (part.kind != "data" || !part.data.is_object() ||
            !part.data.contains("format") || !part.data["format"].is_string() ||
            part.data["format"].get<std::string>() != COLLABORATION_FORMAT ||
            !part.data.contains("envelope") || !part.data["envelope"].is_object()) {
            continue;
        }
        return envelope_from_json(part.data["envelope"]);
    }
    throw std::invalid_argument("A2A message does not carry a NeoGraph collaboration envelope");
}

std::string_view to_string(CollaborationRecordState state) noexcept {
    switch (state) {
        case CollaborationRecordState::Accepted:
            return "accepted";
        case CollaborationRecordState::Acknowledged:
            return "acknowledged";
        case CollaborationRecordState::Canceled:
            return "canceled";
    }
    return "unknown";
}

CollaborationRecordState collaboration_record_state_from_string(std::string_view value) {
    if (value == "accepted") return CollaborationRecordState::Accepted;
    if (value == "acknowledged") return CollaborationRecordState::Acknowledged;
    if (value == "canceled") return CollaborationRecordState::Canceled;
    throw std::invalid_argument("Unknown collaboration record state");
}

std::string_view to_string(CollaborationSubmitResult result) noexcept {
    switch (result) {
        case CollaborationSubmitResult::Accepted:
            return "accepted";
        case CollaborationSubmitResult::Duplicate:
            return "duplicate";
        case CollaborationSubmitResult::Conflict:
            return "conflict";
        case CollaborationSubmitResult::Rejected:
            return "rejected";
    }
    return "unknown";
}

struct CollaborationMailbox::Impl {
    explicit Impl(std::string owner, std::string agent)
        : owner_scope(std::move(owner)), agent_id(std::move(agent)) {}

    std::string owner_scope;
    std::string agent_id;
    mutable std::mutex mutex;
    std::unordered_map<std::string, CollaborationLink> links;
    std::unordered_map<std::string, CollaborationRecord> records;
    std::unordered_map<std::string, std::uint64_t> highest_sequence;
};

CollaborationMailbox::CollaborationMailbox(std::string owner_scope, std::string agent_id)
    : impl_(std::make_shared<Impl>(std::move(owner_scope), std::move(agent_id))) {
    require_nonempty(impl_->owner_scope, "owner_scope");
    require_nonempty(impl_->agent_id, "agent_id");
}

CollaborationMailbox::CollaborationMailbox(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
CollaborationMailbox::CollaborationMailbox(CollaborationMailbox&&) noexcept = default;
CollaborationMailbox& CollaborationMailbox::operator=(CollaborationMailbox&&) noexcept = default;
CollaborationMailbox::~CollaborationMailbox() = default;

CollaborationMailbox CollaborationMailbox::parse(std::string_view stored_bytes) {
    const auto value = json::parse(stored_bytes);
    if (required_unsigned(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported collaboration mailbox storage schema version");
    }
    auto impl = std::make_shared<Impl>(required_string(value, "owner_scope"),
                                       required_string(value, "agent_id"));
    for (const auto& item : required_array(value, "links")) {
        auto link = CollaborationLink::parse(item.dump());
        if (link.spec().receiver_owner_scope != impl->owner_scope ||
            link.spec().receiver_agent_id != impl->agent_id) {
            throw std::invalid_argument("Collaboration mailbox link owner mismatch");
        }
        const auto link_id = link.spec().link_id;
        impl->links.emplace(link_id, std::move(link));
    }
    for (const auto& item : required_array(value, "records")) {
        auto record = record_from_json(item);
        const auto& envelope = record.envelope;
        if (envelope.receiver_owner_scope != impl->owner_scope ||
            envelope.receiver_agent_id != impl->agent_id) {
            throw std::invalid_argument("Collaboration mailbox record owner mismatch");
        }
        const auto record_key = envelope.idempotency_key;
        const auto record_correlation = envelope.correlation_id;
        const auto record_sequence = envelope.sequence;
        impl->records.emplace(record_key, std::move(record));
        auto& highest = impl->highest_sequence[record_correlation];
        highest = std::max(highest, record_sequence);
    }
    return CollaborationMailbox(std::move(impl));
}

const std::string& CollaborationMailbox::owner_scope() const noexcept {
    return impl_->owner_scope;
}

const std::string& CollaborationMailbox::agent_id() const noexcept {
    return impl_->agent_id;
}

void CollaborationMailbox::accept_link(CollaborationLink link, std::string consent_token) {
    if (link.spec().receiver_owner_scope != owner_scope() ||
        link.spec().receiver_agent_id != agent_id()) {
        throw std::invalid_argument("Collaboration link is not addressed to this mailbox");
    }
    if (link.state() == CollaborationLinkState::Proposed) {
        link = link.accept(agent_id(), std::move(consent_token));
    } else if (link.state() == CollaborationLinkState::Accepted &&
               link.consent_fingerprint() != sha256(consent_token)) {
        throw std::invalid_argument("Collaboration consent proof mismatch");
    }
    std::lock_guard lock(impl_->mutex);
    impl_->links.insert_or_assign(link.spec().link_id, std::move(link));
}

void CollaborationMailbox::revoke_link(std::string_view link_id, std::string actor_agent_id) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->links.find(std::string(link_id));
    if (it == impl_->links.end()) return;
    it->second = it->second.revoke(std::move(actor_agent_id));
}

CollaborationSubmitResult CollaborationMailbox::submit(CollaborationEnvelope envelope) {
    validate_envelope_shape(envelope);
    std::lock_guard lock(impl_->mutex);
    const auto link_it = impl_->links.find(envelope.link_id);
    if (link_it == impl_->links.end() || link_it->second.state() != CollaborationLinkState::Accepted ||
        link_it->second.is_expired(now_unix_ms()) ||
        envelope.receiver_owner_scope != owner_scope() ||
        envelope.receiver_agent_id != agent_id() ||
        envelope.sender_owner_scope != link_it->second.spec().sender_owner_scope ||
        envelope.sender_agent_id != link_it->second.spec().sender_agent_id) {
        return CollaborationSubmitResult::Rejected;
    }
    for (const auto& artifact : envelope.artifacts) {
        if (!link_it->second.permits_artifact(artifact.artifact_identity)) {
            return CollaborationSubmitResult::Rejected;
        }
    }
    const auto existing = impl_->records.find(envelope.idempotency_key);
    if (existing != impl_->records.end()) {
        return existing->second.envelope.content_hash() == envelope.content_hash()
                   ? CollaborationSubmitResult::Duplicate
                   : CollaborationSubmitResult::Conflict;
    }
    const auto highest = impl_->highest_sequence[envelope.correlation_id];
    if (envelope.sequence != highest + 1) {
        return CollaborationSubmitResult::Rejected;
    }
    impl_->highest_sequence[envelope.correlation_id] = envelope.sequence;
    const auto idempotency_key = envelope.idempotency_key;
    impl_->records.emplace(idempotency_key,
                           CollaborationRecord{std::move(envelope),
                                                CollaborationRecordState::Accepted,
                                                {}});
    return CollaborationSubmitResult::Accepted;
}

#ifdef NEOGRAPH_A2A_PROGRAM
CollaborationSubmitResult CollaborationMailbox::submit_program(
    CollaborationEnvelope envelope,
    program::ProgramVersion version,
    program::ProgramInvocation invocation) {
    if (version.ownership_scope() != envelope.receiver_owner_scope ||
        envelope.receiver_owner_scope != owner_scope() ||
        envelope.receiver_agent_id != agent_id()) {
        return CollaborationSubmitResult::Rejected;
    }
    if (!envelope.program_version_id.empty() &&
        envelope.program_version_id != version.id()) {
        return CollaborationSubmitResult::Conflict;
    }
    if (envelope.receiver_program_run_id.empty() ||
        invocation.requested_run_id != envelope.receiver_program_run_id) {
        return CollaborationSubmitResult::Rejected;
    }
    if (!invocation.parent_run_id.empty() || invocation.child_depth != 0) {
        return CollaborationSubmitResult::Rejected;
    }
    // ProgramRuntime enforces the admitted policy and executable closure. The
    // mailbox adds the narrower collaboration attenuation: a linked request
    // cannot carry a version whose policy grants capabilities/effects outside
    // the link's explicit allowlists.
    std::lock_guard lock(impl_->mutex);
    const auto link_it = impl_->links.find(envelope.link_id);
    if (link_it == impl_->links.end() || link_it->second.state() != CollaborationLinkState::Accepted ||
        link_it->second.is_expired(now_unix_ms()) ||
        envelope.receiver_owner_scope != owner_scope() ||
        envelope.receiver_agent_id != agent_id() ||
        envelope.sender_owner_scope != link_it->second.spec().sender_owner_scope ||
        envelope.sender_agent_id != link_it->second.spec().sender_agent_id) {
        return CollaborationSubmitResult::Rejected;
    }
    for (const auto& capability : version.policy_snapshot().allowed_capabilities()) {
        if (!link_it->second.permits_capability(capability)) return CollaborationSubmitResult::Rejected;
    }
    for (const auto& effect : version.policy_snapshot().allowed_effects()) {
        if (!link_it->second.permits_effect(effect)) return CollaborationSubmitResult::Rejected;
    }
    for (const auto& artifact : envelope.artifacts) {
        if (!link_it->second.permits_artifact(artifact.artifact_identity)) {
            return CollaborationSubmitResult::Rejected;
        }
    }
    envelope.program_version_id = version.id();
    validate_envelope_shape(envelope);
    const auto existing = impl_->records.find(envelope.idempotency_key);
    if (existing != impl_->records.end()) {
        if (existing->second.envelope.content_hash() != envelope.content_hash()) {
            return CollaborationSubmitResult::Conflict;
        }
        if (!existing->second.program_request ||
            !existing->second.program_request->version ||
            !existing->second.program_request->invocation ||
            existing->second.program_request->version->id() != version.id() ||
            existing->second.program_request->invocation->input != invocation.input ||
            existing->second.program_request->invocation->budget != invocation.budget ||
            existing->second.program_request->invocation->trace_id != invocation.trace_id ||
            existing->second.program_request->invocation->requested_run_id !=
                invocation.requested_run_id) {
            return CollaborationSubmitResult::Conflict;
        }
        return CollaborationSubmitResult::Duplicate;
    }
    const auto highest = impl_->highest_sequence[envelope.correlation_id];
    if (envelope.sequence != highest + 1) return CollaborationSubmitResult::Rejected;
    impl_->highest_sequence[envelope.correlation_id] = envelope.sequence;
    CollaborationRecord record{std::move(envelope),
                                CollaborationRecordState::Accepted,
                                {},
                                CollaborationRecord::ProgramRequest{
                                    std::make_shared<const program::ProgramVersion>(std::move(version)),
                                    std::make_shared<const program::ProgramInvocation>(
                                        std::move(invocation))}};
    impl_->records.emplace(record.envelope.idempotency_key, std::move(record));
    return CollaborationSubmitResult::Accepted;
}

CollaborationSubmitResult CollaborationMailbox::submit(
    CollaborationEnvelope envelope,
    program::ProgramVersion version,
    program::ProgramInvocation invocation) {
    return submit_program(std::move(envelope), std::move(version), std::move(invocation));
}

std::optional<CollaborationRecord::ProgramRequest>
CollaborationMailbox::get_program_request(std::string_view idempotency_key) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(std::string(idempotency_key));
    if (it == impl_->records.end() || !it->second.program_request) return std::nullopt;
    return it->second.program_request;
}
#endif

bool CollaborationMailbox::acknowledge(std::string_view idempotency_key) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(std::string(idempotency_key));
    if (it == impl_->records.end() || it->second.state == CollaborationRecordState::Canceled) {
        return false;
    }
    it->second.state = CollaborationRecordState::Acknowledged;
    return true;
}

bool CollaborationMailbox::cancel(std::string_view link_id,
                                   std::string_view correlation_id,
                                   std::string_view actor_agent_id) {
    std::lock_guard lock(impl_->mutex);
    const auto link_it = impl_->links.find(std::string(link_id));
    if (link_it == impl_->links.end() || !link_it->second.permits_cancellation(actor_agent_id)) {
        return false;
    }
    bool changed = false;
    for (auto& [_, record] : impl_->records) {
        if (record.envelope.link_id == link_id && record.envelope.correlation_id == correlation_id &&
            record.state != CollaborationRecordState::Canceled) {
            record.state = CollaborationRecordState::Canceled;
            changed = true;
        }
    }
    return changed;
}

std::optional<CollaborationRecord> CollaborationMailbox::get(std::string_view idempotency_key) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(std::string(idempotency_key));
    if (it == impl_->records.end()) return std::nullopt;
    return it->second;
}

std::vector<CollaborationRecord> CollaborationMailbox::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<CollaborationRecord> result;
    result.reserve(impl_->records.size());
    for (const auto& [_, record] : impl_->records) result.push_back(record);
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.envelope.correlation_id != rhs.envelope.correlation_id)
            return lhs.envelope.correlation_id < rhs.envelope.correlation_id;
        return lhs.envelope.sequence < rhs.envelope.sequence;
    });
    return result;
}

bool CollaborationMailbox::permits_artifact(std::string_view link_id,
                                             std::string_view artifact_identity) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->links.find(std::string(link_id));
    return it != impl_->links.end() && it->second.permits_artifact(artifact_identity);
}

std::string CollaborationMailbox::serialize_canonical() const {
    std::lock_guard lock(impl_->mutex);
    json links = json::array();
    std::vector<std::string> link_ids;
    link_ids.reserve(impl_->links.size());
    for (const auto& [id, _] : impl_->links) link_ids.push_back(id);
    std::sort(link_ids.begin(), link_ids.end());
    for (const auto& id : link_ids) {
        const auto link_bytes = impl_->links.at(id).serialize_canonical();
        links.push_back(json::parse(link_bytes));
    }
    std::vector<CollaborationRecord> values;
    values.reserve(impl_->records.size());
    for (const auto& [_, record] : impl_->records) values.push_back(record);
    std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.envelope.correlation_id != rhs.envelope.correlation_id)
            return lhs.envelope.correlation_id < rhs.envelope.correlation_id;
        return lhs.envelope.sequence < rhs.envelope.sequence;
    });
    json records = json::array();
    for (const auto& record : values) records.push_back(record_to_json(record));
    return json{{"storage_schema_version", STORAGE_SCHEMA_VERSION},
                {"owner_scope", impl_->owner_scope},
                {"agent_id", impl_->agent_id},
                {"links", links},
                {"records", records}}
        .dump();
}

}  // namespace neograph::a2a
