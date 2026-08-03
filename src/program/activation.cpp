#include <neograph/program/activation.h>

#include "canonical_json.h"

#include <neograph/program/diagnostic.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORMAT = "neograph-program-activation";

std::string require_string(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name) || !value[name].is_string() || value[name].get<std::string>().empty())
        throw std::invalid_argument("Program activation field '" + name + "' must be a nonempty string");
    return value[name].get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name) || !value[name].is_number_unsigned())
        throw std::invalid_argument("Program activation field '" + name + "' must be unsigned");
    return value[name].get<std::uint64_t>();
}

}  // namespace

struct ProgramActivation::Impl {
    explicit Impl(ProgramActivationData value) : data(std::move(value)) {}
    ProgramActivationData data;
    std::string           id;
};

ProgramActivation ProgramActivation::create(ProgramActivationData data) {
    detail::validate_token(data.owner_scope, "Program activation owner scope");
    detail::validate_token(data.active_version_id, "Program activation version id");
    detail::validate_token(data.policy_snapshot_hash, "Program activation policy hash");
    if (data.generation == 0)
        throw std::invalid_argument("Program activation generation must be positive");

    auto impl = std::make_shared<Impl>(std::move(data));
    const json body{{"format", std::string(FORMAT)},
                    {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                    {"owner_scope", impl->data.owner_scope},
                    {"active_version_id", impl->data.active_version_id},
                    {"generation", impl->data.generation},
                    {"policy_snapshot_hash", impl->data.policy_snapshot_hash}};
    impl->id = detail::sha256_identity("program-activation/v1", detail::canonical_json_bytes(body));
    return ProgramActivation(std::move(impl));
}

ProgramActivation ProgramActivation::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) throw std::invalid_argument("Program activation must be an object");
    detail::reject_unknown_fields(value, "Program activation",
                                  {"format", "storage_schema_version", "owner_scope",
                                   "active_version_id", "generation", "policy_snapshot_hash",
                                   "id"});
    if (!value.contains("format") || value["format"] != FORMAT)
        throw std::invalid_argument("Program activation format is unsupported");
    if (!value.contains("storage_schema_version") ||
        value["storage_schema_version"] != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Program activation storage schema version is unsupported");
    auto result = create(ProgramActivationData{require_string(value, "owner_scope"),
                                               require_string(value, "active_version_id"),
                                               require_uint64(value, "generation"),
                                               require_string(value, "policy_snapshot_hash")});
    if (!value.contains("id") || value["id"] != result.id())
        throw std::invalid_argument("Program activation identity does not match its content");
    return result;
}

const std::string& ProgramActivation::owner_scope() const noexcept { return impl_->data.owner_scope; }
const std::string& ProgramActivation::active_version_id() const noexcept {
    return impl_->data.active_version_id;
}
std::uint64_t ProgramActivation::generation() const noexcept { return impl_->data.generation; }
const std::string& ProgramActivation::policy_snapshot_hash() const noexcept {
    return impl_->data.policy_snapshot_hash;
}
const std::string& ProgramActivation::id() const noexcept { return impl_->id; }

std::string ProgramActivation::serialize_canonical() const {
    const json value{{"format", std::string(FORMAT)},
                     {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                     {"owner_scope", owner_scope()},
                     {"active_version_id", active_version_id()},
                     {"generation", generation()},
                     {"policy_snapshot_hash", policy_snapshot_hash()},
                     {"id", id()}};
    return detail::canonical_json_bytes(value);
}

ProgramActivation::ProgramActivation(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::string_view to_string(ProgramActivationResult result) noexcept {
    switch (result) {
        case ProgramActivationResult::Activated: return "activated";
        case ProgramActivationResult::AlreadyPresent: return "already_present";
        case ProgramActivationResult::Conflict: return "conflict";
    }
    return "conflict";
}

}  // namespace neograph::program
