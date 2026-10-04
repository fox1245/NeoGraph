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
    explicit SchemaProvider(sp::descriptor::ValidatedDescriptor descriptor,
                            sp::runtime::Options options = {}, Defaults defaults = {});
    ~SchemaProvider() override;
    static std::unique_ptr<SchemaProvider> create(
        sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options = {},
        Defaults defaults = {});
    PreparedProviderRequest prepare(ProviderRequest request) override;
    std::string_view family() const noexcept override;
    std::string get_name() const override;
private:
    std::string family_;
    std::string name_;
    const Defaults defaults_;
    std::shared_ptr<sp::runtime::Client> client_;
};
} // namespace neograph::llm
