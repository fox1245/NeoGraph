#include <neograph/program/lineage.h>
#include <neograph/program/run_record.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view GENERATION_FORMAT = "neograph-program-run-generation";
constexpr std::string_view LINEAGE_FORMAT    = "neograph-program-run-lineage";

std::string require_string(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name) || !value.at(name).is_string())
        throw std::invalid_argument("Program lineage field '" + name + "' must be a string");
    return value.at(name).get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name) || !value.at(name).is_number_unsigned())
        throw std::invalid_argument("Program lineage field '" + name + "' must be unsigned");
    return value.at(name).get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const auto result = require_uint64(value, key);
    if (result > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Program lineage integer exceeds uint32 range");
    return static_cast<std::uint32_t>(result);
}

std::int64_t require_int64(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name) || !value.at(name).is_number_integer())
        throw std::invalid_argument("Program lineage field '" + name + "' must be an integer");
    if (value.at(name).is_number_unsigned()) {
        const auto result = value.at(name).get<std::uint64_t>();
        if (result > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("Program lineage integer exceeds int64 range");
        return static_cast<std::int64_t>(result);
    }
    return value.at(name).get<std::int64_t>();
}

std::optional<std::string> require_optional_identity(const json& value, std::string_view key) {
    const auto name = std::string(key);
    if (!value.contains(name))
        throw std::invalid_argument("Program lineage requires field '" + name + "'");
    if (value.at(name).is_null()) return std::nullopt;
    if (!value.at(name).is_string())
        throw std::invalid_argument("Program lineage field '" + name +
                                    "' must be a string or null");
    auto result = value.at(name).get<std::string>();
    if (!detail::is_sha256_identity(result))
        throw std::invalid_argument("Program lineage predecessor must be a sha256 identity");
    return result;
}

json encode_budget(const RunBudget& budget) {
    return {{"wall_time_ms", budget.wall_time_ms},
            {"model_tokens", budget.model_tokens},
            {"monetary_microunits", budget.monetary_microunits},
            {"max_concurrency", budget.max_concurrency},
            {"max_program_operations", budget.max_program_operations},
            {"max_core_steps", budget.max_core_steps},
            {"max_dynamic_compiles", budget.max_dynamic_compiles},
            {"max_child_depth", budget.max_child_depth},
            {"max_total_children", budget.max_total_children}};
}

RunBudget parse_budget(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Program lineage budget must be an object");
    detail::reject_unknown_fields(
        value, "Program lineage budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    return {
        require_uint64(value, "wall_time_ms"),           require_uint64(value, "model_tokens"),
        require_uint64(value, "monetary_microunits"),    require_uint32(value, "max_concurrency"),
        require_uint64(value, "max_program_operations"), require_uint64(value, "max_core_steps"),
        require_uint64(value, "max_dynamic_compiles"),   require_uint32(value, "max_child_depth"),
        require_uint64(value, "max_total_children")};
}

json generation_body(const ProgramRunGenerationData& data, std::uint32_t schema_version) {
    json value = {
        {"format", std::string(GENERATION_FORMAT)},
        {"storage_schema_version", schema_version},
        {"owner_scope", data.owner_scope},
        {"lineage_id", data.lineage_id},
        {"generation", data.generation},
        {"run_id", data.run_id},
        {"program_version_id", data.program_version_id},
        {"bundle_id", data.bundle_id},
        {"initial_run_record_id", data.initial_run_record_id},
        {"initial_journal_head", data.initial_journal_head},
         {"predecessor_generation_id",
          data.predecessor_generation_id ? json(*data.predecessor_generation_id) : json(nullptr)},
        {"created_at_ms", data.created_at_ms},
        {"child_depth", data.child_depth}};
    if (schema_version >= 2) {
        value["replacement_receipt"] =
            data.replacement_receipt
                ? detail::parse_json_strict(data.replacement_receipt->serialize_canonical())
                : json(nullptr);
    }
    if (schema_version >= 3) {
        value["graph_migration_receipt"] =
            data.graph_migration_receipt
                ? detail::parse_json_strict(
                      data.graph_migration_receipt->serialize_canonical())
                : json(nullptr);
    }
    return value;
}

json lineage_body(const ProgramRunLineageData& data) {
    return {{"format", std::string(LINEAGE_FORMAT)},
            {"storage_schema_version", ProgramRunLineage::STORAGE_SCHEMA_VERSION},
            {"owner_scope", data.owner_scope},
            {"lineage_id", data.lineage_id},
            {"root_run_id", data.root_run_id},
            {"active_generation", data.active_generation},
            {"active_generation_id", data.active_generation_id},
            {"active_run_record_id", data.active_run_record_id},
            {"active_journal_head", data.active_journal_head},
            {"remaining_budget", encode_budget(data.remaining_budget)},
            {"inflight_reservation", encode_budget(data.inflight_reservation)},
            {"predecessor_head_id",
             data.predecessor_head_id ? json(*data.predecessor_head_id) : json(nullptr)},
            {"created_at_ms", data.created_at_ms},
            {"updated_at_ms", data.updated_at_ms},
            {"committed_descendant_budget", encode_budget(data.committed_descendant_budget)}};
}

void validate_generation(const ProgramRunGenerationData& data) {
    if (data.owner_scope.empty())
        throw std::invalid_argument("Program run generation owner scope must not be empty");
    detail::validate_utf8(data.owner_scope);
    if (!detail::is_sha256_identity(data.lineage_id) ||
        !detail::is_sha256_identity(data.program_version_id) ||
        !detail::is_sha256_identity(data.bundle_id) ||
        !detail::is_sha256_identity(data.initial_run_record_id) ||
        !detail::is_sha256_identity(data.initial_journal_head)) {
        throw std::invalid_argument("Program run generation identities must be sha256 identities");
    }
    detail::validate_token(data.run_id, "Program run generation run id");
    if (data.generation == 0)
        throw std::invalid_argument("Program run generation must be positive");
    if ((data.generation == 1) != !data.predecessor_generation_id)
        throw std::invalid_argument(
            "Program run generation predecessor does not match its ordinal");
    if (data.predecessor_generation_id &&
        !detail::is_sha256_identity(*data.predecessor_generation_id))
        throw std::invalid_argument("Program run generation predecessor must be a sha256 identity");
    if (data.created_at_ms < 0)
        throw std::invalid_argument("Program run generation timestamp must not be negative");
    if (data.replacement_receipt && data.graph_migration_receipt) {
        throw std::invalid_argument(
            "Program run generation cannot carry multiple successor receipts");
    }
    if (data.replacement_receipt) {
        const auto& receipt = *data.replacement_receipt;
        if (data.generation == 1 || receipt.owner_scope() != data.owner_scope ||
            receipt.lineage_id() != data.lineage_id ||
            receipt.target_generation() != data.generation ||
            receipt.target_run_id() != data.run_id ||
            receipt.target_program_version_id() != data.program_version_id ||
            receipt.target_bundle_id() != data.bundle_id ||
            receipt.source_generation_id() != *data.predecessor_generation_id) {
            throw std::invalid_argument(
                "Program replacement receipt does not bind its successor generation");
        }
    }
    if (data.graph_migration_receipt) {
        const auto& receipt = *data.graph_migration_receipt;
        const auto& capsule = receipt.capsule();
        if (data.generation == 1 || capsule.owner_scope() != data.owner_scope ||
            capsule.lineage_id() != data.lineage_id ||
            receipt.target_generation() != data.generation ||
            receipt.target_run_id() != data.run_id ||
            receipt.target_program_version_id() != data.program_version_id ||
            receipt.target_bundle_id() != data.bundle_id ||
            capsule.source_generation_id() != *data.predecessor_generation_id) {
            throw std::invalid_argument(
                "Program Graph migration receipt does not bind its successor generation");
        }
    }
}

void validate_lineage(const ProgramRunLineageData& data) {
    if (data.owner_scope.empty())
        throw std::invalid_argument("Program run lineage owner scope must not be empty");
    detail::validate_utf8(data.owner_scope);
    if (!detail::is_sha256_identity(data.lineage_id) ||
        !detail::is_sha256_identity(data.active_generation_id) ||
        !detail::is_sha256_identity(data.active_run_record_id) ||
        !detail::is_sha256_identity(data.active_journal_head)) {
        throw std::invalid_argument("Program run lineage identities must be sha256 identities");
    }
    detail::validate_token(data.root_run_id, "Program run lineage root run id");
    if (data.lineage_id != program_run_lineage_id(data.owner_scope, data.root_run_id))
        throw std::invalid_argument("Program run lineage id does not bind its owner and root run");
    if (data.active_generation == 0)
        throw std::invalid_argument("Program run lineage active generation must be positive");
    if (data.predecessor_head_id && !detail::is_sha256_identity(*data.predecessor_head_id))
        throw std::invalid_argument("Program run lineage predecessor must be a sha256 identity");
    if (data.created_at_ms < 0 || data.updated_at_ms < data.created_at_ms)
        throw std::invalid_argument("Program run lineage timestamps are invalid");
}

template <typename Integer>
bool sum_at_most(Integer value_available,
                 Integer value_reserved,
                 Integer limit_available,
                 Integer limit_reserved) noexcept {
    using Unsigned         = std::make_unsigned_t<Integer>;
    const auto value_a     = static_cast<Unsigned>(value_available);
    const auto value_b     = static_cast<Unsigned>(value_reserved);
    const auto limit_a     = static_cast<Unsigned>(limit_available);
    const auto limit_b     = static_cast<Unsigned>(limit_reserved);
    const auto value_sum   = static_cast<Unsigned>(value_a + value_b);
    const auto limit_sum   = static_cast<Unsigned>(limit_a + limit_b);
    const bool value_carry = value_sum < value_a;
    const bool limit_carry = limit_sum < limit_a;
    return value_carry == limit_carry ? value_sum <= limit_sum : !value_carry;
}

bool budget_total_at_most(const ProgramRunLineage& value, const ProgramRunLineage& limit) noexcept {
    const auto available       = value.remaining_budget();
    const auto reserved        = value.inflight_reservation();
    const auto limit_available = limit.remaining_budget();
    const auto limit_reserved  = limit.inflight_reservation();
    return sum_at_most(available.wall_time_ms, reserved.wall_time_ms, limit_available.wall_time_ms,
                       limit_reserved.wall_time_ms) &&
           sum_at_most(available.model_tokens, reserved.model_tokens, limit_available.model_tokens,
                       limit_reserved.model_tokens) &&
           sum_at_most(available.monetary_microunits, reserved.monetary_microunits,
                       limit_available.monetary_microunits, limit_reserved.monetary_microunits) &&
           sum_at_most(available.max_concurrency, reserved.max_concurrency,
                       limit_available.max_concurrency, limit_reserved.max_concurrency) &&
           sum_at_most(available.max_program_operations, reserved.max_program_operations,
                       limit_available.max_program_operations,
                       limit_reserved.max_program_operations) &&
           sum_at_most(available.max_core_steps, reserved.max_core_steps,
                       limit_available.max_core_steps, limit_reserved.max_core_steps) &&
           sum_at_most(available.max_dynamic_compiles, reserved.max_dynamic_compiles,
                       limit_available.max_dynamic_compiles, limit_reserved.max_dynamic_compiles) &&
           sum_at_most(available.max_child_depth, reserved.max_child_depth,
                       limit_available.max_child_depth, limit_reserved.max_child_depth) &&
           sum_at_most(available.max_total_children, reserved.max_total_children,
                        limit_available.max_total_children, limit_reserved.max_total_children);
}

bool budget_at_least(const RunBudget& value, const RunBudget& limit) noexcept {
    return value.wall_time_ms >= limit.wall_time_ms && value.model_tokens >= limit.model_tokens &&
           value.monetary_microunits >= limit.monetary_microunits &&
           value.max_concurrency >= limit.max_concurrency &&
           value.max_program_operations >= limit.max_program_operations &&
           value.max_core_steps >= limit.max_core_steps &&
           value.max_dynamic_compiles >= limit.max_dynamic_compiles &&
           value.max_child_depth >= limit.max_child_depth &&
           value.max_total_children >= limit.max_total_children;
}

}  // namespace

std::string program_run_lineage_id(std::string_view owner_scope, std::string_view root_run_id) {
    if (owner_scope.empty())
        throw std::invalid_argument("Program run lineage owner scope must not be empty");
    detail::validate_utf8(owner_scope);
    detail::validate_token(root_run_id, "Program run lineage root run id");
    std::string material(owner_scope);
    material.push_back('\0');
    material.append(root_run_id);
    return detail::sha256_identity("program-run-lineage-id/v1", material);
}

struct ProgramRunGeneration::Impl {
    explicit Impl(ProgramRunGenerationData value, std::uint32_t version)
        : data(std::move(value)), schema_version(version) {
        auto value_body  = generation_body(data, schema_version);
        id               = detail::sha256_identity("program-run-generation/v1",
                                                   detail::canonical_json_bytes(value_body));
        value_body["id"] = id;
        canonical_bytes  = detail::canonical_json_bytes(value_body);
    }

    ProgramRunGenerationData data;
    std::uint32_t             schema_version;
    std::string              id;
    std::string              canonical_bytes;
};

ProgramRunGeneration::ProgramRunGeneration(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramRunGeneration ProgramRunGeneration::create(ProgramRunGenerationData data) {
    validate_generation(data);
    return ProgramRunGeneration(
        std::make_shared<const Impl>(std::move(data), STORAGE_SCHEMA_VERSION));
}

ProgramRunGeneration ProgramRunGeneration::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != GENERATION_FORMAT)
        throw std::invalid_argument("Stored Program run generation has unknown format");
    const auto schema_version = require_uint32(value, "storage_schema_version");
    if (schema_version == 1) {
        detail::reject_unknown_fields(
            value, "Stored Program run generation",
            {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
             "generation", "run_id", "program_version_id", "bundle_id",
             "initial_run_record_id", "initial_journal_head", "predecessor_generation_id",
             "created_at_ms", "child_depth"});
    } else if (schema_version == 2) {
        detail::reject_unknown_fields(
            value, "Stored Program run generation",
            {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
             "generation", "run_id", "program_version_id", "bundle_id",
             "initial_run_record_id", "initial_journal_head", "predecessor_generation_id",
             "created_at_ms", "child_depth", "replacement_receipt"});
    } else if (schema_version == STORAGE_SCHEMA_VERSION) {
        detail::reject_unknown_fields(
            value, "Stored Program run generation",
            {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
             "generation", "run_id", "program_version_id", "bundle_id",
             "initial_run_record_id", "initial_journal_head", "predecessor_generation_id",
             "created_at_ms", "child_depth", "replacement_receipt",
             "graph_migration_receipt"});
    } else {
        throw std::invalid_argument("Stored Program run generation schema is unsupported");
    }
    std::optional<ProgramReplacementReceipt> replacement_receipt;
    if (schema_version >= 2) {
        if (!value.contains("replacement_receipt")) {
            throw std::invalid_argument("Stored generation requires a replacement receipt field");
        }
        if (!value.at("replacement_receipt").is_null()) {
            replacement_receipt = ProgramReplacementReceipt::parse(
                detail::canonical_json_bytes(value.at("replacement_receipt")));
        }
    }
    std::optional<ProgramGraphMigrationReceipt> graph_migration_receipt;
    if (schema_version >= 3) {
        if (!value.contains("graph_migration_receipt")) {
            throw std::invalid_argument(
                "Stored generation requires a Graph migration receipt field");
        }
        if (!value.at("graph_migration_receipt").is_null()) {
            graph_migration_receipt = ProgramGraphMigrationReceipt::parse(
                detail::canonical_json_bytes(value.at("graph_migration_receipt")));
        }
    }
    ProgramRunGenerationData data{
        require_string(value, "owner_scope"), require_string(value, "lineage_id"),
        require_uint64(value, "generation"), require_string(value, "run_id"),
        require_string(value, "program_version_id"), require_string(value, "bundle_id"),
        require_string(value, "initial_run_record_id"),
        require_string(value, "initial_journal_head"),
        require_optional_identity(value, "predecessor_generation_id"),
        require_int64(value, "created_at_ms"), require_uint32(value, "child_depth"),
        std::move(replacement_receipt), std::move(graph_migration_receipt)};
    validate_generation(data);
    ProgramRunGeneration result(
        std::make_shared<const Impl>(std::move(data), schema_version));
    if (result.id() != require_string(value, "id"))
        throw std::invalid_argument(
            "Stored Program run generation identity does not match content");
    return result;
}

const std::string& ProgramRunGeneration::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& ProgramRunGeneration::lineage_id() const noexcept {
    return impl_->data.lineage_id;
}
std::uint64_t ProgramRunGeneration::generation() const noexcept {
    return impl_->data.generation;
}
const std::string& ProgramRunGeneration::run_id() const noexcept {
    return impl_->data.run_id;
}
const std::string& ProgramRunGeneration::program_version_id() const noexcept {
    return impl_->data.program_version_id;
}
const std::string& ProgramRunGeneration::bundle_id() const noexcept {
    return impl_->data.bundle_id;
}
const std::string& ProgramRunGeneration::initial_run_record_id() const noexcept {
    return impl_->data.initial_run_record_id;
}
const std::string& ProgramRunGeneration::initial_journal_head() const noexcept {
    return impl_->data.initial_journal_head;
}
const std::optional<std::string>& ProgramRunGeneration::predecessor_generation_id() const noexcept {
    return impl_->data.predecessor_generation_id;
}
std::int64_t ProgramRunGeneration::created_at_ms() const noexcept {
    return impl_->data.created_at_ms;
}
std::uint32_t ProgramRunGeneration::child_depth() const noexcept { return impl_->data.child_depth; }
std::optional<ProgramReplacementReceipt> ProgramRunGeneration::replacement_receipt() const {
    return impl_->data.replacement_receipt;
}
std::optional<ProgramGraphMigrationReceipt>
ProgramRunGeneration::graph_migration_receipt() const {
    return impl_->data.graph_migration_receipt;
}
const std::string& ProgramRunGeneration::id() const noexcept {
    return impl_->id;
}
std::string ProgramRunGeneration::serialize_canonical() const {
    return impl_->canonical_bytes;
}

struct ProgramRunLineage::Impl {
    explicit Impl(ProgramRunLineageData value) : data(std::move(value)) {
        auto value_body  = lineage_body(data);
        id               = detail::sha256_identity("program-run-lineage/v1",
                                                   detail::canonical_json_bytes(value_body));
        value_body["id"] = id;
        canonical_bytes  = detail::canonical_json_bytes(value_body);
    }

    ProgramRunLineageData data;
    std::string           id;
    std::string           canonical_bytes;
};

ProgramRunLineage::ProgramRunLineage(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramRunLineage ProgramRunLineage::create(ProgramRunLineageData data) {
    validate_lineage(data);
    return ProgramRunLineage(std::make_shared<const Impl>(std::move(data)));
}

ProgramRunLineage ProgramRunLineage::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != LINEAGE_FORMAT)
        throw std::invalid_argument("Stored Program run lineage has unknown format");
    detail::reject_unknown_fields(
        value, "Stored Program run lineage",
        {"format", "storage_schema_version", "id", "owner_scope", "lineage_id", "root_run_id",
         "active_generation", "active_generation_id", "active_run_record_id", "active_journal_head",
         "remaining_budget", "inflight_reservation", "predecessor_head_id", "created_at_ms",
         "updated_at_ms", "committed_descendant_budget"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored Program run lineage schema is unsupported");
    if (!value.contains("remaining_budget") || !value.contains("inflight_reservation") ||
        !value.contains("committed_descendant_budget"))
        throw std::invalid_argument("Stored Program run lineage requires budget fields");
    auto result = create(ProgramRunLineageData{
        require_string(value, "owner_scope"), require_string(value, "lineage_id"),
        require_string(value, "root_run_id"), require_uint64(value, "active_generation"),
        require_string(value, "active_generation_id"),
        require_string(value, "active_run_record_id"), require_string(value, "active_journal_head"),
        parse_budget(value.at("remaining_budget")), parse_budget(value.at("inflight_reservation")),
        require_optional_identity(value, "predecessor_head_id"),
        require_int64(value, "created_at_ms"), require_int64(value, "updated_at_ms"),
        parse_budget(value.at("committed_descendant_budget"))});
    if (result.id() != require_string(value, "id"))
        throw std::invalid_argument("Stored Program run lineage identity does not match content");
    return result;
}

const std::string& ProgramRunLineage::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& ProgramRunLineage::lineage_id() const noexcept {
    return impl_->data.lineage_id;
}
const std::string& ProgramRunLineage::root_run_id() const noexcept {
    return impl_->data.root_run_id;
}
std::uint64_t ProgramRunLineage::active_generation() const noexcept {
    return impl_->data.active_generation;
}
const std::string& ProgramRunLineage::active_generation_id() const noexcept {
    return impl_->data.active_generation_id;
}
const std::string& ProgramRunLineage::active_run_record_id() const noexcept {
    return impl_->data.active_run_record_id;
}
const std::string& ProgramRunLineage::active_journal_head() const noexcept {
    return impl_->data.active_journal_head;
}
RunBudget ProgramRunLineage::remaining_budget() const noexcept {
    return impl_->data.remaining_budget;
}
RunBudget ProgramRunLineage::inflight_reservation() const noexcept {
    return impl_->data.inflight_reservation;
}
const std::optional<std::string>& ProgramRunLineage::predecessor_head_id() const noexcept {
    return impl_->data.predecessor_head_id;
}
std::int64_t ProgramRunLineage::created_at_ms() const noexcept {
    return impl_->data.created_at_ms;
}
std::int64_t ProgramRunLineage::updated_at_ms() const noexcept {
    return impl_->data.updated_at_ms;
}
RunBudget ProgramRunLineage::committed_descendant_budget() const noexcept {
    return impl_->data.committed_descendant_budget;
}
const std::string& ProgramRunLineage::id() const noexcept {
    return impl_->id;
}
std::string ProgramRunLineage::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool is_valid_program_run_lineage_initial(const ProgramRunLineage&    lineage,
                                          const ProgramRunGeneration& generation) noexcept {
    return lineage.owner_scope() == generation.owner_scope() &&
           lineage.lineage_id() == generation.lineage_id() &&
           lineage.root_run_id() == generation.run_id() && lineage.active_generation() == 1 &&
           generation.generation() == 1 && !generation.predecessor_generation_id() &&
           !lineage.predecessor_head_id() && lineage.active_generation_id() == generation.id() &&
           lineage.active_run_record_id() == generation.initial_run_record_id() &&
           lineage.active_journal_head() == generation.initial_journal_head() &&
           lineage.created_at_ms() == generation.created_at_ms();
}

bool does_program_run_generation_bind(const ProgramRunGeneration& generation,
                                      const ProgramRunLineage&    lineage,
                                      const ProgramRunRecord&     run) noexcept {
    return generation.owner_scope() == run.owner_scope() &&
           generation.lineage_id() == lineage.lineage_id() &&
           generation.generation() == lineage.active_generation() &&
           generation.id() == lineage.active_generation_id() &&
           generation.run_id() == run.run_id() &&
           generation.program_version_id() == run.program_version_id() &&
           generation.bundle_id() == run.bundle_id() && generation.child_depth() == run.child_depth();
}

bool is_valid_program_run_lineage_transition(
    const ProgramRunLineage&                   previous,
    const ProgramRunLineage&                   next,
    const std::optional<ProgramRunGeneration>& successor) noexcept {
    if (next.owner_scope() != previous.owner_scope() ||
        next.lineage_id() != previous.lineage_id() ||
        next.root_run_id() != previous.root_run_id() ||
        next.created_at_ms() != previous.created_at_ms() ||
        next.updated_at_ms() < previous.updated_at_ms() || !next.predecessor_head_id() ||
        *next.predecessor_head_id() != previous.id() || !budget_total_at_most(next, previous) ||
        !budget_at_least(next.committed_descendant_budget(),
                         previous.committed_descendant_budget())) {
        return false;
    }

    if (!successor) {
        return next.active_generation() == previous.active_generation() &&
               next.active_generation_id() == previous.active_generation_id();
    }

    return previous.active_generation() != std::numeric_limits<std::uint64_t>::max() &&
           next.active_generation() == previous.active_generation() + 1 &&
           successor->generation() == next.active_generation() &&
           successor->owner_scope() == previous.owner_scope() &&
           successor->lineage_id() == previous.lineage_id() &&
           successor->predecessor_generation_id() &&
           *successor->predecessor_generation_id() == previous.active_generation_id() &&
           next.active_generation_id() == successor->id() &&
           next.active_run_record_id() == successor->initial_run_record_id() &&
           next.active_journal_head() == successor->initial_journal_head();
}

}  // namespace neograph::program
