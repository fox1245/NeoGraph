#include <neograph/mcp/tenant.h>

#include <stdexcept>
#include <utility>

namespace neograph::mcp {
namespace {

std::string encoded(std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

std::string prefix(const tenant::TenantScope& scope) {
    return "__neograph_tenant__" + encoded(scope.tenant_id()) + ":";
}

bool has_prefix(std::string_view value, std::string_view wanted) {
    return value.size() >= wanted.size() && value.substr(0, wanted.size()) == wanted;
}

std::string public_id(const tenant::TenantScope& scope, std::string value) {
    const auto p = prefix(scope);
    if (!has_prefix(value, p)) return value;
    value.erase(0, p.size());
    const auto separator = value.find(':');
    if (separator == std::string::npos) return {};
    std::size_t length = 0;
    try {
        length = static_cast<std::size_t>(std::stoull(value.substr(0, separator)));
    } catch (...) {
        return {};
    }
    if (separator + 1 + length != value.size()) return {};
    return value.substr(separator + 1);
}

json with_private_run(const tenant::TenantScope& scope, const json& event) {
    auto copy = event;
    if (copy.is_object() && copy.contains("run_id") && copy.at("run_id").is_string())
        copy["run_id"] = ScopedHarnessStore::private_id(scope, copy.at("run_id").get<std::string>());
    return copy;
}

json with_public_run(const tenant::TenantScope& scope, json event) {
    if (event.is_object() && event.contains("run_id") && event.at("run_id").is_string())
        event["run_id"] = public_id(scope, event.at("run_id").get<std::string>());
    return event;
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
    records_->save_artifact(private_id(scope_, artifact_id), record);
}

std::optional<json> ScopedHarnessStore::load_artifact(const std::string& artifact_id) {
    return records_->load_artifact(private_id(scope_, artifact_id));
}

void ScopedHarnessStore::save_run(const std::string& run_id, const json& record) {
    records_->save_run(private_id(scope_, run_id), record);
}

std::optional<json> ScopedHarnessStore::load_run(const std::string& run_id) {
    return records_->load_run(private_id(scope_, run_id));
}

void ScopedHarnessStore::append_event(const json& event) {
    if (!journal_) throw std::logic_error("ScopedHarnessStore has no journal backend");
    journal_->append_event(with_private_run(scope_, event));
}

std::vector<json> ScopedHarnessStore::list_events(const std::string& run_id,
                                                   std::size_t after_sequence,
                                                   std::size_t limit) {
    if (!journal_) throw std::logic_error("ScopedHarnessStore has no journal backend");
    auto events = journal_->list_events(private_id(scope_, run_id), after_sequence, limit);
    for (auto& event : events) event = with_public_run(scope_, std::move(event));
    return events;
}

HarnessRetentionResult
ScopedHarnessStore::cleanup_retained(const HarnessRetentionPolicy& policy) {
    if (!retention_) throw std::logic_error("ScopedHarnessStore has no retention backend");
    auto private_policy = policy;
    for (auto& id : private_policy.protected_artifact_ids)
        id = private_id(scope_, id);
    for (auto& id : private_policy.protected_run_ids) id = private_id(scope_, id);
    auto result = retention_->cleanup_retained(private_policy);
    for (auto& id : result.artifact_ids) id = public_id(scope_, std::move(id));
    for (auto& id : result.run_ids) id = public_id(scope_, std::move(id));
    return result;
}

}  // namespace neograph::mcp
