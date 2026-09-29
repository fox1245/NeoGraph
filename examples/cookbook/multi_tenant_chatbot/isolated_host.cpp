// Production-boundary reference host for issue #244.
//
// This offline example intentionally uses synthetic provider/model and tool
// policy identities. A real ingress authenticates the request first, then
// selects one immutable TenantHost record; it never lets request JSON choose
// a scope, provider credential, registry, or store.
#include <neograph/neograph.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

struct TenantService {
    TenantScope scope;
    std::string topology_identity;
    std::string provider_model_policy;
    std::string tool_policy;
    TenantQuota quota;
    std::shared_ptr<ScopedStore> store;
    std::shared_ptr<ScopedCheckpointStore> checkpoints;

    TenantService(TenantScope tenant_scope, std::string topology, std::string provider_policy,
                  std::string tools, TenantQuotas limits)
        : scope(std::move(tenant_scope)), topology_identity(std::move(topology)),
          provider_model_policy(std::move(provider_policy)), tool_policy(std::move(tools)),
          quota(limits), store(std::make_shared<ScopedStore>(
                               scope, std::make_shared<InMemoryStore>())),
          checkpoints(std::make_shared<ScopedCheckpointStore>(
              scope, std::make_shared<InMemoryCheckpointStore>())) {}
};

class TenantHost {
public:
    TenantHost() {
        // These are different topology/provider/tool policies and stores. In
        // production each record is loaded from a trusted control plane.
        add(std::make_shared<TenantService>(
            TenantScope("tenant-a", "authz-a"), "topology-concise", "model-a-v1", "tools-a",
            TenantQuotas{.max_concurrency = 2, .max_queue = 4, .max_model_tokens = 1000,
                         .max_artifacts = 16, .max_retained_artifacts = 8}));
        add(std::make_shared<TenantService>(
            TenantScope("tenant-b", "authz-b"), "topology-research", "model-b-v2", "tools-b",
            TenantQuotas{.max_concurrency = 1, .max_queue = 2, .max_model_tokens = 400,
                         .max_artifacts = 4, .max_retained_artifacts = 2}));
    }

    json serve(std::string_view tenant_id, std::string_view session_id, json input) {
        const auto it = services_.find(std::string(tenant_id));
        if (it == services_.end()) throw std::invalid_argument("unknown tenant");
        auto& service = *it->second;
        auto lease = TenantQuotaLease::acquire(service.quota);
        // The public session ID can repeat across tenants: ScopedStore maps it
        // to a tenant-private namespace before touching the backend.
        service.store->put({"sessions", std::string(session_id)}, "last_input", input);
        return json{{"tenant", service.scope.tenant_id()},
                    {"topology", service.topology_identity},
                    {"provider_policy", service.provider_model_policy},
                    {"tool_policy", service.tool_policy},
                    {"accepted", true}};
    }

private:
    void add(std::shared_ptr<TenantService> service) {
        services_.emplace(service->scope.tenant_id(), std::move(service));
    }
    std::unordered_map<std::string, std::shared_ptr<TenantService>> services_;
};

int main() {
    TenantHost host;
    const auto a = host.serve("tenant-a", "same-public-session", json{{"message", "a"}});
    const auto b = host.serve("tenant-b", "same-public-session", json{{"message", "b"}});
    std::cout << a.dump() << '\n' << b.dump() << '\n';
}
