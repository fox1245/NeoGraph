#include <neograph/program/command.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

constexpr std::array<std::string_view, 8> kCommandNames = {
    "call_core", "spawn", "await", "join", "emit", "checkpoint", "cancel_scope",
    "host_capability"};

std::string require_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty())
        throw std::invalid_argument("JavaScript command field '" + key + "' must be a nonempty string");
    return value.at(key).get<std::string>();
}

std::uint32_t require_uint32(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned())
        throw std::invalid_argument("JavaScript command field '" + key + "' must be unsigned");
    const auto number = value.at(key).get<std::uint64_t>();
    if (number > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("JavaScript command field '" + key + "' exceeds uint32 range");
    return static_cast<std::uint32_t>(number);
}

std::uint64_t require_positive_uint64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned() ||
        value.at(key).get<std::uint64_t>() == 0)
        throw std::invalid_argument("JavaScript command field '" + key + "' must be positive");
    return value.at(key).get<std::uint64_t>();
}

void reject_unknown_fields(const json& value,
                           std::string_view object_name,
                           const std::vector<std::string_view>& allowed) {
    std::set<std::string, std::less<>> accepted;
    for (const auto field : allowed) accepted.emplace(field);
    for (const auto& [field, ignored] : value.items()) {
        (void)ignored;
        if (!accepted.contains(field))
            throw std::invalid_argument("Unknown field in " + std::string(object_name) + ": " + field);
    }
}

void require_exact_fields(const json& value,
                          std::string_view object_name,
                          std::initializer_list<std::string_view> required,
                          std::initializer_list<std::string_view> optional = {}) {
    if (!value.is_object())
        throw std::invalid_argument(std::string(object_name) + " must be an object");
    std::vector<std::string_view> allowed;
    allowed.reserve(required.size() + optional.size());
    allowed.insert(allowed.end(), required.begin(), required.end());
    allowed.insert(allowed.end(), optional.begin(), optional.end());
    reject_unknown_fields(value, object_name, allowed);
    for (const auto field : required) {
        if (!value.contains(std::string(field)))
            throw std::invalid_argument(std::string(object_name) + " requires field '" +
                                        std::string(field) + "'");
    }
}

void validate_site(std::string_view source_site) {
    if (source_site.empty()) throw std::invalid_argument("JavaScript command source_site is empty");
    detail::validate_utf8(source_site);
    for (const unsigned char value : source_site) {
        if (value < 0x20u || value == 0x7fu)
            throw std::invalid_argument("JavaScript command source_site contains a control character");
    }
}

void validate_join_arguments(const json& arguments);

void validate_arguments(JavaScriptCommandKind kind, const json& arguments) {
    switch (kind) {
        case JavaScriptCommandKind::CallCore:
            require_exact_fields(arguments, "JavaScript call_core arguments", {"name", "input"});
            (void)require_string(arguments, "name");
            break;
        case JavaScriptCommandKind::Spawn:
            require_exact_fields(arguments, "JavaScript spawn arguments", {"child_binding", "input"});
            (void)require_string(arguments, "child_binding");
            break;
        case JavaScriptCommandKind::Await:
            require_exact_fields(arguments, "JavaScript await arguments", {"command"},
                                 {"timeout_ms"});
            (void)JavaScriptCommand::from_json(arguments.at("command"));
            if (arguments.contains("timeout_ms"))
                (void)require_positive_uint64(arguments, "timeout_ms");
            break;
        case JavaScriptCommandKind::Join:
            validate_join_arguments(arguments);
            break;
        case JavaScriptCommandKind::Emit:
            require_exact_fields(arguments, "JavaScript emit arguments", {"value"});
            break;
        case JavaScriptCommandKind::Checkpoint:
            require_exact_fields(arguments, "JavaScript checkpoint arguments", {"value"});
            break;
        case JavaScriptCommandKind::CancelScope:
            require_exact_fields(arguments, "JavaScript cancel_scope arguments", {"scope"},
                                 {"reason"});
            (void)require_string(arguments, "scope");
            if (arguments.contains("reason")) (void)require_string(arguments, "reason");
            break;
        case JavaScriptCommandKind::HostCapability:
            require_exact_fields(arguments, "JavaScript host_capability arguments", {"input"});
            break;
    }
}

void validate_join_arguments(const json& arguments) {
    require_exact_fields(arguments, "JavaScript join arguments", {"mode", "members"},
                         {"required_successes", "max_in_flight", "failure_policy"});
    const auto mode = require_string(arguments, "mode");
    if (mode != "all" && mode != "race" && mode != "quorum")
        throw std::invalid_argument("JavaScript join mode is unsupported: " + mode);
    if (!arguments.at("members").is_array() || arguments.at("members").empty())
        throw std::invalid_argument("JavaScript join members must be a nonempty array");
    const auto& members = arguments.at("members");
    if (mode == "race" && members.size() < 2)
        throw std::invalid_argument("JavaScript race requires at least two members");
    std::uint64_t required = 0;
    if (mode == "quorum") {
        required = require_positive_uint64(arguments, "required_successes");
        if (required > members.size())
            throw std::invalid_argument("JavaScript quorum required_successes exceeds member count");
    } else if (arguments.contains("required_successes")) {
        throw std::invalid_argument(
            "JavaScript all/race join must not include required_successes");
    }
    if (arguments.contains("max_in_flight"))
        (void)require_positive_uint64(arguments, "max_in_flight");
    if (arguments.contains("failure_policy")) {
        const auto policy = require_string(arguments, "failure_policy");
        if (policy != "fail_fast" && policy != "collect")
            throw std::invalid_argument("JavaScript join failure_policy is unsupported: " + policy);
    }
    for (const auto& member : members) (void)JavaScriptCommand::from_json(member);
}

std::uint32_t builtin_slot(JavaScriptCommandKind kind) {
    switch (kind) {
        case JavaScriptCommandKind::CallCore: return JAVASCRIPT_IMPORT_SLOT_CALL_CORE;
        case JavaScriptCommandKind::Spawn: return JAVASCRIPT_IMPORT_SLOT_SPAWN;
        case JavaScriptCommandKind::Await: return JAVASCRIPT_IMPORT_SLOT_AWAIT;
        case JavaScriptCommandKind::Join: return JAVASCRIPT_IMPORT_SLOT_JOIN;
        case JavaScriptCommandKind::Emit: return JAVASCRIPT_IMPORT_SLOT_EMIT;
        case JavaScriptCommandKind::Checkpoint: return JAVASCRIPT_IMPORT_SLOT_CHECKPOINT;
        case JavaScriptCommandKind::CancelScope: return JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE;
        case JavaScriptCommandKind::HostCapability:
            throw std::invalid_argument("Host capabilities require an admitted import slot");
    }
    throw std::invalid_argument("Unknown JavaScript command kind");
}

json command_json(std::uint32_t         protocol_version,
                  JavaScriptCommandKind kind,
                  std::uint32_t         import_slot,
                  const std::string&    source_site,
                  const json&           arguments) {
    return json{{"protocol_version", protocol_version},
                {"kind", std::string(to_string(kind))},
                {"import_slot", import_slot},
                {"source_site", source_site},
                {"arguments", detail::owned_json_copy(arguments)}};
}

}  // namespace

std::string_view to_string(JavaScriptCommandKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kCommandNames.size() ? kCommandNames[index] : "unknown";
}

JavaScriptCommandKind javascript_command_kind_from_string(std::string_view value) {
    const auto found = std::find(kCommandNames.begin(), kCommandNames.end(), value);
    if (found == kCommandNames.end())
        throw std::invalid_argument("Unknown JavaScript command kind: " + std::string(value));
    return static_cast<JavaScriptCommandKind>(std::distance(kCommandNames.begin(), found));
}

struct JavaScriptCommand::Impl {
    std::uint32_t         protocol_version = PROTOCOL_VERSION;
    JavaScriptCommandKind kind             = JavaScriptCommandKind::CallCore;
    std::uint32_t         import_slot      = 0;
    std::string           source_site;
    json                  arguments;
};

JavaScriptCommand::JavaScriptCommand(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
JavaScriptCommand::~JavaScriptCommand() = default;

JavaScriptCommand JavaScriptCommand::make(std::uint32_t         protocol_version,
                                          JavaScriptCommandKind  kind,
                                          std::uint32_t          import_slot,
                                          std::string            source_site,
                                          json                   arguments) {
    if (protocol_version != PROTOCOL_VERSION)
        throw std::invalid_argument("JavaScript command protocol_version is unsupported");
    if (to_string(kind) == "unknown") throw std::invalid_argument("Unknown JavaScript command kind");
    if (kind != JavaScriptCommandKind::HostCapability && import_slot != builtin_slot(kind))
        throw std::invalid_argument("JavaScript command import_slot does not match its kind");
    validate_site(source_site);
    if (!arguments.is_object())
        throw std::invalid_argument("JavaScript command arguments must be an object");
    auto owned_arguments = detail::owned_json_copy(arguments);
    validate_arguments(kind, owned_arguments);
    (void)detail::canonical_json_bytes(owned_arguments);

    auto impl              = std::make_shared<Impl>();
    impl->protocol_version = protocol_version;
    impl->kind             = kind;
    impl->import_slot      = import_slot;
    impl->source_site      = std::move(source_site);
    impl->arguments        = std::move(owned_arguments);
    return JavaScriptCommand(std::move(impl));
}

JavaScriptCommand JavaScriptCommand::from_json(const json& value) {
    require_exact_fields(value, "JavaScript command",
                         {"protocol_version", "kind", "import_slot", "source_site", "arguments"});
    const auto protocol_version = require_uint32(value, "protocol_version");
    const auto kind             = javascript_command_kind_from_string(require_string(value, "kind"));
    const auto import_slot      = require_uint32(value, "import_slot");
    const auto source_site      = require_string(value, "source_site");
    validate_site(source_site);
    if (!value.at("arguments").is_object())
        throw std::invalid_argument("JavaScript command arguments must be an object");
    auto result = make(protocol_version, kind, import_slot, source_site,
                       detail::owned_json_copy(value.at("arguments")));
    const auto canonical = detail::canonical_json_bytes(value);
    if (canonical != detail::canonical_json_bytes(result.to_json()))
        throw std::invalid_argument("JavaScript command is not canonical");
    return result;
}

JavaScriptCommand JavaScriptCommand::call_core(std::string source_site,
                                               std::string core_name,
                                               json        input) {
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::CallCore,
                JAVASCRIPT_IMPORT_SLOT_CALL_CORE, std::move(source_site),
                json{{"name", std::move(core_name)}, {"input", detail::owned_json_copy(input)}});
}

JavaScriptCommand JavaScriptCommand::spawn(std::string source_site,
                                           std::string child_binding,
                                           json        input) {
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::Spawn,
                JAVASCRIPT_IMPORT_SLOT_SPAWN, std::move(source_site),
                json{{"child_binding", std::move(child_binding)},
                     {"input", detail::owned_json_copy(input)}});
}

JavaScriptCommand JavaScriptCommand::await(std::string source_site,
                                           JavaScriptCommand child,
                                           std::uint64_t timeout_ms) {
    json arguments{{"command", child.to_json()}};
    if (timeout_ms != 0) arguments["timeout_ms"] = timeout_ms;
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::Await,
                JAVASCRIPT_IMPORT_SLOT_AWAIT, std::move(source_site), std::move(arguments));
}

JavaScriptCommand JavaScriptCommand::join(std::string                    source_site,
                                          std::string                    mode,
                                          std::vector<JavaScriptCommand> members,
                                          std::uint64_t                  required_successes,
                                          std::uint64_t                  max_in_flight,
                                          std::string                    failure_policy) {
    json encoded_members = json::array();
    for (const auto& member : members) encoded_members.push_back(member.to_json());
    json arguments{{"mode", std::move(mode)}, {"members", std::move(encoded_members)}};
    if (required_successes != 0) arguments["required_successes"] = required_successes;
    if (max_in_flight != 0) arguments["max_in_flight"] = max_in_flight;
    if (!failure_policy.empty()) arguments["failure_policy"] = std::move(failure_policy);
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::Join,
                JAVASCRIPT_IMPORT_SLOT_JOIN, std::move(source_site), std::move(arguments));
}

JavaScriptCommand JavaScriptCommand::emit(std::string source_site, json value) {
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::Emit,
                JAVASCRIPT_IMPORT_SLOT_EMIT, std::move(source_site),
                json{{"value", detail::owned_json_copy(value)}});
}

JavaScriptCommand JavaScriptCommand::checkpoint(std::string source_site, json value) {
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::Checkpoint,
                JAVASCRIPT_IMPORT_SLOT_CHECKPOINT, std::move(source_site),
                json{{"value", detail::owned_json_copy(value)}});
}

JavaScriptCommand JavaScriptCommand::cancel_scope(std::string source_site,
                                                  std::string scope,
                                                  std::string reason) {
    json arguments{{"scope", std::move(scope)}};
    if (!reason.empty()) arguments["reason"] = std::move(reason);
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::CancelScope,
                JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE, std::move(source_site),
                std::move(arguments));
}

JavaScriptCommand JavaScriptCommand::host_capability(std::string source_site,
                                                     std::uint32_t import_slot,
                                                     json            input) {
    return make(PROTOCOL_VERSION, JavaScriptCommandKind::HostCapability, import_slot,
                std::move(source_site),
                json{{"input", detail::owned_json_copy(input)}});
}

std::uint32_t JavaScriptCommand::protocol_version() const noexcept { return impl_->protocol_version; }
JavaScriptCommandKind JavaScriptCommand::kind() const noexcept { return impl_->kind; }
std::uint32_t JavaScriptCommand::import_slot() const noexcept { return impl_->import_slot; }
const std::string& JavaScriptCommand::source_site() const noexcept { return impl_->source_site; }
json JavaScriptCommand::arguments() const { return detail::owned_json_copy(impl_->arguments); }

json JavaScriptCommand::to_json() const {
    return command_json(impl_->protocol_version, impl_->kind, impl_->import_slot,
                        impl_->source_site, impl_->arguments);
}

bool JavaScriptCommand::operator==(const JavaScriptCommand& other) const {
    return impl_->protocol_version == other.impl_->protocol_version &&
           impl_->kind == other.impl_->kind && impl_->import_slot == other.impl_->import_slot &&
           impl_->source_site == other.impl_->source_site && impl_->arguments == other.impl_->arguments;
}

}  // namespace neograph::program
