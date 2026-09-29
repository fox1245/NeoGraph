#include <neograph/mcp/tenant.h>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace neograph::mcp {
namespace {

std::string encoded(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::string prefix(const tenant::TenantScope& scope) {
    return "__neograph_tenant__" + encoded(scope.tenant_id()) + "_";
}

bool has_prefix(std::string_view value, std::string_view wanted) {
    return value.size() >= wanted.size() && value.substr(0, wanted.size()) == wanted;
}

std::string public_id(const tenant::TenantScope& scope, std::string_view value) {
    const auto p = prefix(scope);
    if (!has_prefix(value, p))
        throw std::invalid_argument("Harness record identifier is outside its tenant namespace");
    value.remove_prefix(p.size());
    if (value.size() % 2 != 0)
        throw std::invalid_argument("Malformed tenant Harness record identifier");
    const auto digit = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        throw std::invalid_argument("Malformed tenant Harness record identifier");
    };
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<char>((digit(value[i]) << 4) | digit(value[i + 1])));
    return result;
}

// Translate only schema-owned envelope fields, never opaque requests, results or payloads.
json translate_ids(const tenant::TenantScope& scope, json record,
                   std::initializer_list<const char*> fields, bool to_private) {
    if (!record.is_object()) return record;
    for (const auto* field : fields) {
        if (!record.contains(field) || !record.at(field).is_string()) continue;
        const auto id = record.at(field).get<std::string>();
        if (id.empty()) continue;  // Optional relationship fields use the empty sentinel.
        record[field] = to_private ? ScopedHarnessStore::private_id(scope, id)
                                   : public_id(scope, id);
    }
    return record;
}

}  // namespace

ScopedHarnessStore::ScopedHarnessStore(tenant::TenantScope scope,
                                       std::shared_ptr<HarnessRecordStore> records,
                                       std::shared_ptr<HarnessJournal> journal,
                                       std::shared_ptr<HarnessRetentionStore> retention)
    : scope_(std::move(scope)), records_(std::move(records)), journal_(std::move(journal)),
      retention_(std::move(retention)) {
    if (!records_) throw std::invalid_argument("ScopedHarnessStore requires a record backend");
}

std::string ScopedHarnessStore::private_id(const tenant::TenantScope& scope, std::string_view id) {
    return prefix(scope) + encoded(id);
}

std::string ScopedHarnessStore::private_uri(const tenant::TenantScope& scope,
                                            std::string_view uri) {
    const std::string marker = "/runs/";
    const auto        start = uri.find(marker);
    if (start == std::string_view::npos) return std::string(uri);
    const auto id_start = start + marker.size();
    const auto id_end = uri.find('/', id_start);
    const auto id = uri.substr(id_start, id_end == std::string_view::npos
                                             ? std::string_view::npos
                                             : id_end - id_start);
    std::string result(uri);
    result.replace(id_start, id.size(), private_id(scope, id));
    return result;
}

void ScopedHarnessStore::save_artifact(const std::string& artifact_id, const json& record) {
    records_->save_artifact(private_id(scope_, artifact_id),
                            translate_ids(scope_, record, {"artifact_id"}, true));
}

std::optional<json> ScopedHarnessStore::load_artifact(const std::string& artifact_id) {
    auto record = records_->load_artifact(private_id(scope_, artifact_id));
    if (record) *record = translate_ids(scope_, std::move(*record), {"artifact_id"}, false);
    return record;
}

void ScopedHarnessStore::save_run(const std::string& run_id, const json& record) {
    records_->save_run(
        private_id(scope_, run_id),
        translate_ids(scope_, record,
                      {"run_id", "artifact_id", "source_run_id", "source_checkpoint_id"}, true));
}

std::optional<json> ScopedHarnessStore::load_run(const std::string& run_id) {
    auto record = records_->load_run(private_id(scope_, run_id));
    if (record)
        *record = translate_ids(
            scope_, std::move(*record),
            {"run_id", "artifact_id", "source_run_id", "source_checkpoint_id"}, false);
    return record;
}

void ScopedHarnessStore::append_event(const json& event) {
    if (!journal_) throw std::logic_error("ScopedHarnessStore has no journal backend");
    journal_->append_event(translate_ids(scope_, event, {"run_id", "artifact_id"}, true));
}

std::vector<json> ScopedHarnessStore::list_events(const std::string& run_id,
                                                   std::size_t after_sequence,
                                                   std::size_t limit) {
    if (!journal_) throw std::logic_error("ScopedHarnessStore has no journal backend");
    auto events = journal_->list_events(private_id(scope_, run_id), after_sequence, limit);
    for (auto& event : events)
        event = translate_ids(scope_, std::move(event), {"run_id", "artifact_id"}, false);
    return events;
}

HarnessRetentionResult
ScopedHarnessStore::cleanup_retained(const HarnessRetentionPolicy& policy) {
    if (!retention_) throw std::logic_error("ScopedHarnessStore has no retention backend");
    auto private_policy = policy;
    private_policy.namespace_prefix = prefix(scope_);
    for (auto& id : private_policy.protected_artifact_ids)
        id = private_id(scope_, id);
    for (auto& id : private_policy.protected_run_ids) id = private_id(scope_, id);
    auto result = retention_->cleanup_retained(private_policy);
    for (auto& id : result.artifact_ids) id = public_id(scope_, std::move(id));
    for (auto& id : result.run_ids) id = public_id(scope_, std::move(id));
    return result;
}

}  // namespace neograph::mcp
