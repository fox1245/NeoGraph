/**
 * @file mcp/tenant.h
 * @brief Tenant namespaces for Harness records and journals.
 */
#pragma once

#include <neograph/mcp/harness.h>
#include <neograph/tenant.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::mcp {
using TenantScope = tenant::TenantScope;

/** Shared-backend Harness adapter that exposes only one tenant's IDs. */
class NEOGRAPH_HARNESS_API ScopedHarnessStore final : public HarnessRecordStore,
                                                       public HarnessJournal,
                                                       public HarnessRetentionStore {
public:
    ScopedHarnessStore(tenant::TenantScope scope, std::shared_ptr<HarnessRecordStore> records,
                       std::shared_ptr<HarnessJournal> journal = {},
                       std::shared_ptr<HarnessRetentionStore> retention = {});

    const tenant::TenantScope& scope() const noexcept { return scope_; }
    static std::string private_id(const tenant::TenantScope& scope, std::string_view id);
    static std::string private_uri(const tenant::TenantScope& scope, std::string_view uri);

    void save_artifact(const std::string& artifact_id, const json& record) override;
    std::optional<json> load_artifact(const std::string& artifact_id) override;
    void save_run(const std::string& run_id, const json& record) override;
    std::optional<json> load_run(const std::string& run_id) override;
    void append_event(const json& event) override;
    std::vector<json> list_events(const std::string& run_id,
                                  std::size_t after_sequence = 0,
                                  std::size_t limit = 1000) override;
    HarnessRetentionResult cleanup_retained(const HarnessRetentionPolicy& policy) override;

private:
    tenant::TenantScope scope_;
    std::shared_ptr<HarnessRecordStore> records_;
    std::shared_ptr<HarnessJournal> journal_;
    std::shared_ptr<HarnessRetentionStore> retention_;
};

using TenantHarnessStore = ScopedHarnessStore;

}  // namespace neograph::mcp
