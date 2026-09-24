/**
 * @file graph/provider_call_broker.h
 * @brief Host-owned, invocation-scoped boundary for built-in Core provider calls.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace neograph::graph {

/** Stable Core call identity. The host adds its Program/job scope. */
struct ProviderCallIdentity {
    std::string owner_scope;
    std::string run_id;
    std::string thread_id;
    std::string task_id;
    std::string node_name;
    /// Built-in nodes issue one provider call per task; zero in this API version.
    std::uint64_t call_ordinal = 0;
};

/**
 * The trusted host implements durable admission, replay, and reconciliation.
 * `params` are the exact request passed to Provider::invoke by the built-in
 * node. A broker may return a verified stored completion without calling the
 * provider. An exception denies the node call. Core does not claim that a
 * broker implementation is durable or that the remote effect is exactly once.
 */
class NEOGRAPH_API ProviderCallBroker {
public:
    virtual ~ProviderCallBroker() = default;

    virtual asio::awaitable<ChatCompletion> invoke(
        ProviderCallIdentity identity,
        std::shared_ptr<Provider> provider,
        CompletionParams params,
        StreamCallback on_chunk) = 0;
};

}  // namespace neograph::graph
