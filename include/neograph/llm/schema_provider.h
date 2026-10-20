#pragma once
#include <neograph/provider.h>
#include <descriptor/descriptor.h>
#include <core/request_controls.h>

namespace neograph::llm {

/// Provider backed by a pinned, closed/versioned descriptor and immutable SDK policy.
/// Endpoint/header/model facts are data; codecs, replay and retry are compiled.
class NEOGRAPH_API SchemaProvider final : public Provider {
public:
    struct Defaults {
        std::optional<sp::OpenRouterRouting> provider;
        std::optional<bool> responses_store;
    };
    /// Runs on the SDK's worker pool and its libcurl transport. A build without that transport (WebAssembly, a
    /// single-thread build) throws the SDK's descriptor::ConfigError at "/transport": use the constructor below.
    explicit SchemaProvider(sp::descriptor::ValidatedDescriptor descriptor,
                            sp::runtime::Options options = {}, Defaults defaults = {});
    /// Runs on a host's event loop (#535): the SDK's work is posted to `executor` and every HTTP attempt goes through
    /// `transport` (sp::runtime::Executor and HostTransport, runtime/host.h; sp::runtime::js builds both over
    /// `fetch`). Needs no libcurl, no OpenSSL and no thread. Pump the executor with async::HostLoop and use the
    /// `_async` entry points; the synchronous ones refuse (Provider::host_pumped). Throws the SDK's
    /// descriptor::ConfigError when `transport` is null, and when `executor` is null in a build without threads.
    SchemaProvider(sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
                   std::shared_ptr<sp::runtime::Executor> executor,
                   std::shared_ptr<sp::runtime::HostTransport> transport, Defaults defaults = {});
    ~SchemaProvider() override;
    static std::unique_ptr<SchemaProvider> create(
        sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options = {},
        Defaults defaults = {});
    static std::unique_ptr<SchemaProvider> create(
        sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
        std::shared_ptr<sp::runtime::Executor> executor,
        std::shared_ptr<sp::runtime::HostTransport> transport, Defaults defaults = {});
    PreparedProviderRequest prepare(ProviderRequest request) override;
    std::string_view family() const noexcept override;
    std::string get_name() const override;
    bool host_pumped() const noexcept override { return host_pumped_; }
protected:
    std::shared_ptr<sp::runtime::Client> endpoint_runtime() const override { return client_; }
private:
    std::string family_;
    std::string name_;
    const Defaults defaults_;
    bool host_pumped_ = false;
    std::shared_ptr<sp::runtime::Client> client_;
};
} // namespace neograph::llm
