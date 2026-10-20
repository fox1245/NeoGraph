#include <neograph/llm/schema_provider.h>
#include <type_traits>
#include <utility>

namespace neograph::llm {
namespace {
void merge_routing(std::optional<sp::OpenRouterRouting>& target,
                   const std::optional<sp::OpenRouterRouting>& defaults) {
    if (!defaults) return;
    if (!target) { target = defaults; return; }
    if (!target->zdr) target->zdr = defaults->zdr;
    if (!target->allow_fallbacks) target->allow_fallbacks = defaults->allow_fallbacks;
    if (!target->require_parameters) target->require_parameters = defaults->require_parameters;
    if (!target->data_collection) target->data_collection = defaults->data_collection;
    if (target->only.empty()) target->only = defaults->only;
    if (target->order.empty()) target->order = defaults->order;
    if (target->ignore.empty()) target->ignore = defaults->ignore;
    if (!target->max_price) target->max_price = defaults->max_price;
}
std::string checked_family(const sp::descriptor::ValidatedDescriptor& descriptor) {
    sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
    return std::string(descriptor.family());
}
void check_defaults(const sp::descriptor::ValidatedDescriptor& descriptor, const std::string& family,
                    const SchemaProvider::Defaults& defaults) {
    if (defaults.provider) {
        if (family != "openai.chat" && family != "openai.responses")
            throw std::invalid_argument("Routing defaults are unsupported by this family");
        if (const auto error = sp::request_controls::validate_routing(descriptor, *defaults.provider))
            throw std::invalid_argument(*error);
    }
    if (defaults.responses_store && family != "openai.responses")
        throw std::invalid_argument("Responses retention defaults require Responses family");
}
}
SchemaProvider::SchemaProvider(sp::descriptor::ValidatedDescriptor descriptor,
                               sp::runtime::Options options, Defaults defaults)
    : family_(checked_family(descriptor)), name_(descriptor.id()), defaults_(std::move(defaults)) {
    check_defaults(descriptor, family_, defaults_);
#ifdef NEOGRAPH_HAVE_SP_TRANSPORT
    client_ = std::make_shared<sp::runtime::Client>(std::move(descriptor), std::move(options));
#else
    // No libcurl transport in this build: the SDK refuses a Client that has neither an executor nor a transport.
    client_ = std::make_shared<sp::runtime::Client>(std::move(descriptor), std::move(options), nullptr, nullptr);
#endif
}
SchemaProvider::SchemaProvider(sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
                               std::shared_ptr<sp::runtime::Executor> executor,
                               std::shared_ptr<sp::runtime::HostTransport> transport, Defaults defaults)
    : family_(checked_family(descriptor)), name_(descriptor.id()), defaults_(std::move(defaults)),
      host_pumped_(executor && executor->host_pumped()) {
    check_defaults(descriptor, family_, defaults_);
    client_ = std::make_shared<sp::runtime::Client>(std::move(descriptor), std::move(options),
                                                    std::move(executor), std::move(transport));
}
SchemaProvider::~SchemaProvider() = default;
std::unique_ptr<SchemaProvider> SchemaProvider::create(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options, Defaults defaults) {
    return std::make_unique<SchemaProvider>(std::move(descriptor), std::move(options), std::move(defaults));
}
std::unique_ptr<SchemaProvider> SchemaProvider::create(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::shared_ptr<sp::runtime::Executor> executor, std::shared_ptr<sp::runtime::HostTransport> transport,
    Defaults defaults) {
    return std::make_unique<SchemaProvider>(std::move(descriptor), std::move(options), std::move(executor),
                                            std::move(transport), std::move(defaults));
}
PreparedProviderRequest SchemaProvider::prepare(ProviderRequest request) {
    sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
    std::visit([&](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, sp::chat::Request> || std::is_same_v<T, sp::responses::Request>)
            merge_routing(payload.provider, defaults_.provider);
        if constexpr (std::is_same_v<T, sp::responses::Request>)
            if (!payload.store) payload.store = defaults_.responses_store;
    }, request.payload);
    return prepare_runtime(client_, std::move(request), host_pumped_);
}
std::string SchemaProvider::get_name() const { return name_; }
std::string_view SchemaProvider::family() const noexcept { return family_; }
} // namespace neograph::llm
