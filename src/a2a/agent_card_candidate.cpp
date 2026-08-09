#include <neograph/a2a/agent_card_candidate.h>
#include <neograph/a2a/client.h>
// SHA-256 uses EVP_Digest's one-shot API; OpenSSL 3.5 documentation:
// https://docs.openssl.org/3.5/man3/EVP_DigestInit/ (fetched 2026-08-08).

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace neograph::a2a {
namespace {
constexpr std::size_t      kMaximumDurableCardBytes = 64 * 1024;
constexpr std::size_t      kMaxCardTextBytes        = 4096;
constexpr std::size_t      kMaxIdentifierBytes      = 128;
constexpr std::size_t      kMaxEndpointBytes        = 2048;
constexpr std::size_t      kMaxModes                = 8;
constexpr std::size_t      kMaxSkills               = 64;
constexpr std::size_t      kMaxTags                 = 32;
constexpr std::size_t      kMaxExamples             = 16;
constexpr std::size_t      kMaxProbeCount           = 32;
constexpr std::size_t      kMaxProbeTextBytes       = 8192;
constexpr std::string_view kHelloWorldTemplate      = "copy-ninja.hello-world-echo.v1";

[[noreturn]] void invalid(std::string message) {
    throw std::invalid_argument("Agent Card Copy Ninja PoC: " + std::move(message));
}

bool has_control_character(std::string_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

void require_bounded_text(std::string_view field, std::string_view value, std::size_t maximum) {
    if (value.empty()) invalid(std::string(field) + " must not be empty");
    if (value.size() > maximum) {
        invalid(std::string(field) + " exceeds " + std::to_string(maximum) + " bytes");
    }
    if (has_control_character(value)) {
        invalid(std::string(field) + " must not contain control characters");
    }
}

std::string require_string(const json& object,
                           const char* field,
                           std::size_t maximum = kMaxCardTextBytes) {
    if (!object.is_object() || !object.contains(field) || !object.at(field).is_string()) {
        invalid(std::string("missing string field '") + field + "'");
    }
    const auto value = object.at(field).get<std::string>();
    require_bounded_text(field, value, maximum);
    return value;
}

void require_optional_string_array(const json& object,
                                   const char* field,
                                   std::size_t maximum_items,
                                   std::size_t maximum_item_bytes,
                                   bool (*accept)(std::string_view)) {
    if (!object.contains(field)) return;
    const auto& values = object.at(field);
    if (!values.is_array() || values.size() > maximum_items) {
        invalid(std::string("field '") + field + "' must be a bounded array");
    }
    for (const auto& value : values) {
        if (!value.is_string()) {
            invalid(std::string("field '") + field + "' must contain only strings");
        }
        const auto item = value.get<std::string>();
        require_bounded_text(field, item, maximum_item_bytes);
        if (accept && !accept(item)) {
            invalid(std::string("field '") + field + "' contains an unsupported value");
        }
    }
}

bool is_text_mode(std::string_view value) {
    return value == "text" || value == "text/plain";
}

bool is_skill_identifier(std::string_view value) {
    if (value.empty() || value.size() > kMaxIdentifierBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

std::string normalize_discovery_origin(std::string                      discovery_url,
                                       const AgentCardCollectionPolicy& policy) {
    require_bounded_text("discovery_url", discovery_url, kMaxEndpointBytes);

    constexpr std::string_view https = "https://";
    constexpr std::string_view http  = "http://";
    std::string_view           scheme;
    if (discovery_url.compare(0, https.size(), https) == 0) {
        scheme = https;
    } else if (discovery_url.compare(0, http.size(), http) == 0) {
        scheme = http;
    } else {
        invalid("discovery_url must use https");
    }

    std::string authority = discovery_url.substr(scheme.size());
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.find('?') != std::string::npos || authority.find('#') != std::string::npos) {
        invalid("discovery_url must be a credential-free origin");
    }
    if (authority.back() == '/') authority.pop_back();
    if (authority.empty() || authority.find('/') != std::string::npos) {
        invalid("discovery_url must not contain a path");
    }

    if (scheme == http) {
        std::string host = authority;
        if (!host.empty() && host.front() == '[') {
            const auto closing = host.find(']');
            host               = closing == std::string::npos ? "" : host.substr(1, closing - 1);
        } else {
            const auto port = host.rfind(':');
            if (port != std::string::npos) host.erase(port);
        }
        std::transform(host.begin(), host.end(), host.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
        if (!policy.allow_loopback_http || !loopback) {
            invalid("plain HTTP is allowed only for an explicit loopback test endpoint");
        }
    }

    return std::string(scheme) + authority;
}

void validate_provenance(const AgentCardSourceProvenance& provenance) {
    require_bounded_text("source_url", provenance.source_url, kMaxEndpointBytes);
    if (provenance.source_url.compare(0, 8, "https://") != 0) {
        invalid("source_url must be an immutable HTTPS source location");
    }
    require_bounded_text("source_revision", provenance.source_revision, kMaxIdentifierBytes);
    require_bounded_text("source_license", provenance.source_license, kMaxIdentifierBytes);
}

std::string canonical_dump(const json& value) {
    try {
        return value.dump();
    } catch (const json::exception& error) {
        invalid(std::string("card cannot be canonicalized: ") + error.what());
    }
}

std::string sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int                               size = 0;
    if (EVP_Digest(input.data(), input.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Agent Card Copy Ninja PoC: OpenSSL SHA-256 failed");
    }

    std::ostringstream output;
    output << "sha256:" << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < size; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return output.str();
}

std::vector<std::string> read_text_modes(const json& card, const char* field) {
    if (!card.contains(field) || !card.at(field).is_array() || card.at(field).empty()) {
        invalid(std::string("field '") + field + "' must be a non-empty array");
    }
    require_optional_string_array(card, field, kMaxModes, 64, is_text_mode);

    std::vector<std::string> modes;
    modes.reserve(card.at(field).size());
    for (const auto& value : card.at(field))
        modes.push_back(value.get<std::string>());
    return modes;
}

bool has_jsonrpc_interface(const json& card) {
    if (card.contains("supportedInterfaces")) {
        const auto& interfaces = card.at("supportedInterfaces");
        if (!interfaces.is_array() || interfaces.empty() || interfaces.size() > kMaxModes) {
            invalid("supportedInterfaces must be a bounded non-empty array");
        }
        bool has_jsonrpc = false;
        for (const auto& interface : interfaces) {
            if (!interface.is_object()) invalid("supportedInterfaces must contain objects");
            const auto binding = require_string(interface, "protocolBinding", 64);
            (void)require_string(interface, "url", kMaxEndpointBytes);
            if (binding == "JSONRPC") has_jsonrpc = true;
        }
        return has_jsonrpc;
    }

    (void)require_string(card, "url", kMaxEndpointBytes);
    const auto binding = card.value("preferredTransport", std::string("JSONRPC"));
    require_bounded_text("preferredTransport", binding, 64);
    return binding == "JSONRPC";
}

std::vector<std::string> read_skill_ids(const json& card) {
    if (!card.contains("skills") || !card.at("skills").is_array() || card.at("skills").empty() ||
        card.at("skills").size() > kMaxSkills) {
        invalid("skills must be a bounded non-empty array");
    }

    std::set<std::string>    seen;
    std::vector<std::string> ids;
    ids.reserve(card.at("skills").size());
    for (const auto& skill : card.at("skills")) {
        if (!skill.is_object()) invalid("skills must contain objects");
        const auto id = require_string(skill, "id", kMaxIdentifierBytes);
        if (!is_skill_identifier(id)) invalid("skill id must use [A-Za-z0-9._-]");
        if (!seen.insert(id).second) invalid("skill ids must be unique");

        // Validate all free-form fields as bounded untrusted input. They are
        // deliberately excluded from the generated candidate and harness.
        (void)require_string(skill, "name");
        (void)require_string(skill, "description");
        if (!skill.contains("tags") || !skill.at("tags").is_array()) {
            invalid("skill tags must be an array");
        }
        require_optional_string_array(skill, "tags", kMaxTags, 128, nullptr);
        require_optional_string_array(skill, "examples", kMaxExamples, kMaxCardTextBytes, nullptr);
        require_optional_string_array(skill, "inputModes", kMaxModes, 64, is_text_mode);
        require_optional_string_array(skill, "outputModes", kMaxModes, 64, is_text_mode);
        ids.push_back(id);
    }
    return ids;
}

bool declares_authorization(const json& card) {
    const auto non_empty = [](const json& value) {
        return (value.is_array() || value.is_object()) && !value.empty();
    };

    if (card.contains("securitySchemes") && non_empty(card.at("securitySchemes"))) return true;
    if (card.contains("securityRequirements") && non_empty(card.at("securityRequirements")))
        return true;
    if (card.contains("skills") && card.at("skills").is_array()) {
        for (const auto& skill : card.at("skills")) {
            if (skill.is_object() && skill.contains("securityRequirements") &&
                non_empty(skill.at("securityRequirements"))) {
                return true;
            }
        }
    }
    return false;
}

bool declares_streaming(const json& card) {
    if (!card.contains("capabilities")) return false;
    const auto& capabilities = card.at("capabilities");
    if (!capabilities.is_object()) invalid("capabilities must be an object");
    if (!capabilities.contains("streaming")) return false;
    if (!capabilities.at("streaming").is_boolean()) {
        invalid("capabilities.streaming must be boolean");
    }
    return capabilities.at("streaming").get<bool>();
}

void validate_card(const json& card, const AgentCardCollectionPolicy& policy) {
    if (!card.is_object()) invalid("Agent Card must be a JSON object");
    const auto serialized = canonical_dump(card);
    if (serialized.size() > policy.max_card_bytes) {
        invalid("Agent Card exceeds the configured durable-record limit");
    }

    (void)require_string(card, "name");
    (void)require_string(card, "description");
    (void)require_string(card, "version", kMaxIdentifierBytes);
    if (!has_jsonrpc_interface(card)) {
        invalid("Agent Card does not declare a JSONRPC interface");
    }
    (void)read_text_modes(card, "defaultInputModes");
    (void)read_text_modes(card, "defaultOutputModes");
    (void)read_skill_ids(card);
    (void)declares_streaming(card);
}

std::vector<std::string> descriptor_skill_ids(const AgentCardCompatibilityCandidate& candidate) {
    try {
        const auto& ids = candidate.descriptor.at("declared_contract").at("skill_ids");
        if (!ids.is_array() || ids.empty()) throw std::out_of_range("skill_ids");
        std::vector<std::string> result;
        result.reserve(ids.size());
        for (const auto& id : ids) {
            if (!id.is_string() || !is_skill_identifier(id.get<std::string>())) {
                throw std::out_of_range("invalid skill id");
            }
            result.push_back(id.get<std::string>());
        }
        return result;
    } catch (const json::exception& error) {
        throw std::logic_error(std::string("Agent Card Copy Ninja PoC: malformed candidate: ") +
                               error.what());
    } catch (const std::out_of_range&) {
        throw std::logic_error("Agent Card Copy Ninja PoC: malformed candidate skill contract");
    }
}

std::vector<std::string> descriptor_modes(const AgentCardCompatibilityCandidate& candidate,
                                          const char*                            field) {
    try {
        const auto& modes = candidate.descriptor.at("declared_contract").at(field);
        if (!modes.is_array() || modes.empty() || modes.size() > kMaxModes) {
            throw std::out_of_range(field);
        }
        std::vector<std::string> result;
        result.reserve(modes.size());
        for (const auto& mode : modes) {
            if (!mode.is_string() || !is_text_mode(mode.get<std::string>())) {
                throw std::out_of_range(field);
            }
            result.push_back(mode.get<std::string>());
        }
        return result;
    } catch (const json::exception& error) {
        throw std::logic_error(std::string("Agent Card Copy Ninja PoC: malformed candidate: ") +
                               error.what());
    } catch (const std::out_of_range&) {
        throw std::logic_error(std::string("Agent Card Copy Ninja PoC: malformed candidate ") +
                               field);
    }
}

bool descriptor_streaming(const AgentCardCompatibilityCandidate& candidate) {
    try {
        const auto& streaming = candidate.descriptor.at("declared_contract").at("streaming");
        if (!streaming.is_boolean()) throw std::out_of_range("streaming");
        return streaming.get<bool>();
    } catch (const json::exception& error) {
        throw std::logic_error(std::string("Agent Card Copy Ninja PoC: malformed candidate: ") +
                               error.what());
    } catch (const std::out_of_range&) {
        throw std::logic_error("Agent Card Copy Ninja PoC: malformed candidate streaming contract");
    }
}

}  // namespace

AgentCardCollector::AgentCardCollector(AgentCardCollectionPolicy policy)
    : policy_(std::move(policy)) {
    if (policy_.max_card_bytes == 0 || policy_.max_card_bytes > kMaximumDurableCardBytes) {
        invalid("max_card_bytes must be between one byte and 64 KiB");
    }
}

CollectedAgentCard AgentCardCollector::collect(std::string               discovery_url,
                                               AgentCardSourceProvenance provenance) const {
    const auto origin = normalize_discovery_origin(std::move(discovery_url), policy_);
    if (provenance.discovery_url.empty()) {
        provenance.discovery_url = origin;
    } else if (normalize_discovery_origin(provenance.discovery_url, policy_) != origin) {
        invalid("provenance discovery_url must exactly match the fetched origin");
    } else {
        provenance.discovery_url = origin;
    }
    validate_provenance(provenance);

    // A fresh client intentionally has no Authorization header.  This call is
    // exactly one well-known GET; it does not invoke, probe, or delegate work
    // to the source agent.
    A2AClient client(origin);
    auto      card = client.fetch_agent_card();
    json      raw;
    to_json(raw, card);
    validate_card(raw, policy_);
    const auto serialized = canonical_dump(raw);

    CollectedAgentCard collected;
    collected.card        = std::move(card);
    collected.provenance  = std::move(provenance);
    collected.card_sha256 = sha256(serialized);
    collected.raw_card    = std::move(raw);
    return collected;
}

AgentCardCompatibilityCandidate AgentCardCandidateCompiler::compile(
    const CollectedAgentCard& collected) {
    if (collected.card_sha256.empty()) invalid("collected card digest is required");
    validate_provenance(collected.provenance);

    AgentCardCollectionPolicy integrity_policy;
    validate_card(collected.raw_card, integrity_policy);
    if (sha256(canonical_dump(collected.raw_card)) != collected.card_sha256) {
        invalid("collected card digest does not match its raw bytes");
    }

    const auto input_modes            = read_text_modes(collected.raw_card, "defaultInputModes");
    const auto output_modes           = read_text_modes(collected.raw_card, "defaultOutputModes");
    const auto skill_ids              = read_skill_ids(collected.raw_card);
    const auto streaming              = declares_streaming(collected.raw_card);
    const auto authorization_declared = declares_authorization(collected.raw_card);
    const auto id = "copy-ninja-" + collected.card_sha256.substr(std::string("sha256:").size(), 16);

    AgentCardCompatibilityCandidate candidate;
    candidate.id                 = id;
    candidate.source_card_sha256 = collected.card_sha256;
    candidate.descriptor         = {
        {"schema", "neograph/a2a-agent-card-compatibility-candidate/v1"},
        {"candidate_id", id},
        {"state", "unadmitted"},
        {"source",
                 {
             {"card_sha256", collected.card_sha256},
             {"discovery_url", collected.provenance.discovery_url},
             {"source_url", collected.provenance.source_url},
             {"source_revision", collected.provenance.source_revision},
             {"source_license", collected.provenance.source_license},
         }},
        {"declared_contract",
                 {
             {"a2a_jsonrpc", true},
             {"input_modes", input_modes},
             {"output_modes", output_modes},
             {"streaming", streaming},
             {"skill_ids", skill_ids},
             {"source_declares_authorization", authorization_declared},
         }},
        {"authority",
                 {
             {"inherits_credentials", false},
             {"inherits_network_authority", false},
             {"inherits_provider_selection", false},
             {"source_endpoint_is_never_dispatched", true},
             {"requires_new_admission", true},
         }},
        {"behavioral_equivalence", "unproven"},
        {"execution",
                 {
             {"enabled", false},
             {"source_card_text_is_executable", false},
             {"blocked_on_independent_behavioral_profile", true},
         }},
    };
    return candidate;
}

CopyNinjaHarness::CopyNinjaHarness(AgentCardCompatibilityCandidate candidate,
                                   CopyNinjaBehavioralProfile      profile)
    : candidate_(std::move(candidate)), profile_(std::move(profile)) {}

const AgentCardCompatibilityCandidate& CopyNinjaHarness::candidate() const noexcept {
    return candidate_;
}

const CopyNinjaBehavioralProfile& CopyNinjaHarness::profile() const noexcept {
    return profile_;
}

std::string CopyNinjaHarness::respond(std::string_view input) const {
    if (input.size() > kMaxProbeTextBytes || has_control_character(input)) {
        invalid("harness input exceeds the PoC text bound or contains a control character");
    }
    if (profile_.template_id == kHelloWorldTemplate) {
        return "Hello, World! I have received your request (" + std::string(input) + ")";
    }
    throw std::logic_error("Agent Card Copy Ninja PoC: materialized an unsupported template");
}

CopyNinjaNode::CopyNinjaNode(std::string name, std::shared_ptr<const CopyNinjaHarness> harness)
    : name_(std::move(name)), harness_(std::move(harness)) {
    if (!harness_) {
        invalid("CopyNinjaNode requires a materialized local harness");
    }
}

asio::awaitable<graph::NodeOutput> CopyNinjaNode::run(graph::NodeInput input) {
    const auto prompt_value = input.state.get("prompt");
    const auto prompt =
        prompt_value.is_string() ? prompt_value.get<std::string>() : prompt_value.dump();

    graph::NodeOutput output;
    output.writes.push_back(graph::ChannelWrite{"response", json(harness_->respond(prompt))});
    co_return output;
}

std::string CopyNinjaNode::get_name() const {
    return name_;
}

AgentCard CopyNinjaHarness::agent_card(std::string endpoint) const {
    require_bounded_text("candidate endpoint", endpoint, kMaxEndpointBytes);
    if (endpoint.find('@') != std::string::npos || endpoint.find('?') != std::string::npos ||
        endpoint.find('#') != std::string::npos) {
        invalid("candidate endpoint must be a credential-free origin");
    }

    const auto skill_ids    = descriptor_skill_ids(candidate_);
    const auto input_modes  = descriptor_modes(candidate_, "input_modes");
    const auto output_modes = descriptor_modes(candidate_, "output_modes");
    const auto streaming    = descriptor_streaming(candidate_);

    json skills = json::array();
    for (const auto& id : skill_ids) {
        skills.push_back({
            {"id", id},
            {"name", "Candidate capability: " + id},
            {"description",
             "Independent compatibility candidate; behavior is limited to a separately verified "
             "local profile."},
            {"tags", json::array({"copy-ninja-poc"})},
            {"inputModes", input_modes},
            {"outputModes", output_modes},
        });
    }

    json raw = {
        {"name", candidate_.id},
        {"description",
         "Independent Copy Ninja PoC candidate; not affiliated with the observed source agent."},
        {"version", "0.0.0-copy-ninja-poc"},
        {"url", endpoint},
        {"protocolVersion", "1.0"},
        {"preferredTransport", "JSONRPC"},
        {"supportedInterfaces", json::array({{
                                    {"protocolBinding", "JSONRPC"},
                                    {"protocolVersion", "1.0"},
                                    {"url", endpoint},
                                }})},
        {"capabilities",
         {
             {"streaming", streaming},
             {"pushNotifications", false},
         }},
        {"supportsAuthenticatedExtendedCard", false},
        {"defaultInputModes", input_modes},
        {"defaultOutputModes", output_modes},
        {"securitySchemes", json::object()},
        {"securityRequirements", json::array()},
        {"skills", std::move(skills)},
    };

    AgentCard card;
    from_json(raw, card);
    return card;
}

CopyNinjaHarness materialize_copy_ninja(const AgentCardCompatibilityCandidate& candidate,
                                        CopyNinjaBehavioralProfile             profile) {
    if (candidate.id.empty() || candidate.source_card_sha256.empty()) {
        invalid("candidate identity and source digest are required");
    }
    if (candidate.source_card_sha256 != profile.source_card_sha256) {
        invalid("behavioral profile is not pinned to this collected card");
    }
    if (profile.template_id != kHelloWorldTemplate) {
        invalid("behavioral profile selects an unsupported PoC template");
    }
    if (profile.development_probes.empty() || profile.development_probes.size() > kMaxProbeCount) {
        invalid("behavioral profile must contain a bounded non-empty development probe set");
    }

    CopyNinjaHarness harness(candidate, std::move(profile));
    for (const auto& probe : harness.profile().development_probes) {
        require_bounded_text("development probe input", probe.input, kMaxProbeTextBytes);
        require_bounded_text("development probe expected output", probe.expected_output,
                             kMaxProbeTextBytes);
        if (harness.respond(probe.input) != probe.expected_output) {
            invalid("behavioral profile does not match the selected local template");
        }
    }
    return harness;
}

}  // namespace neograph::a2a
