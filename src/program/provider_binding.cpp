#include "provider_binding.h"

#include "canonical_json.h"

#include <neograph/program/diagnostic.h>
#include <neograph/provider.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace neograph::program::detail {
namespace {

constexpr std::string_view kPreamble = "NeoGraph provider binding v1";

json executable_json(const ExecutableIdentity& identity) {
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

// The registry names the provider a node requires; the bound object answers
// to that name whatever its own descriptor id is.
class RegisteredNameProvider final : public Provider {
public:
    RegisteredNameProvider(std::string name, std::shared_ptr<Provider> inner)
        : name_(std::move(name)), inner_(std::move(inner)) {}
    std::string      get_name() const override { return name_; }
    std::string_view family() const noexcept override { return inner_->family(); }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        return inner_->prepare(std::move(request));
    }

private:
    std::string               name_;
    std::shared_ptr<Provider> inner_;
};

[[noreturn]] void reject_unbound(const ExecutableIdentity& executable) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = "P_ADMIT_PROVIDER";
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"host-provider-binding", "/capability_bindings", std::nullopt};
    diagnostic.message  = "The Program requires provider '" + executable.name +
                         "' but this host binds none (a LocalProgramHost binds one with "
                         "node_context=... and binding_label=...)";
    diagnostic.witness  = json{{"executable", executable_json(executable)}};
    throw ProgramAdmissionError({diagnostic});
}

}  // namespace

std::string validated_binding_label(std::string_view label) {
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-' || c == ':';
    };
    const bool shaped = !label.empty() && label.size() <= 64 &&
                        ((label.front() >= 'a' && label.front() <= 'z') ||
                         (label.front() >= '0' && label.front() <= '9')) &&
                        std::all_of(label.begin(), label.end(), allowed);
    if (!shaped)
        throw std::invalid_argument(
            "binding_label must be 1-64 characters of a-z, 0-9, '.', '_', '-' or ':', starting "
            "with a letter or digit; it names the binding in Program identities and must never "
            "be an API key");
    if (label.rfind("sk-", 0) == 0 || label.rfind("sk_", 0) == 0)
        throw std::invalid_argument("binding_label looks like an API key; use a non-secret name");
    return std::string(label);
}

std::string provider_binding_identity(std::string_view label, const ExecutableIdentity& provider) {
    return neograph::detail::sha256_identity(
        kPreamble, "capability-receipt",
        neograph::detail::canonical_json_bytes(json{{"binding_label", std::string(label)},
                                                    {"provider", executable_json(provider)}}));
}

std::string provider_deployment_identity(std::string_view label) {
    return neograph::detail::sha256_identity(
        kPreamble, "provider-journal-deployment",
        neograph::detail::canonical_json_bytes(json{{"binding_label", std::string(label)}}));
}

CatalogCapabilityBinder single_provider_binder(std::optional<graph::NodeContext> context,
                                               std::string                       label) {
    if (context) {
        if (!context->provider)
            throw std::invalid_argument("A provider binding needs a NodeContext with a provider");
        label = validated_binding_label(label);
    }
    return [context = std::move(context), label = std::move(label)](
               const std::vector<ExecutableIdentity>& closure) {
        CatalogCapabilityBinding          binding;
        std::optional<ExecutableIdentity> provider;
        for (const auto& executable : closure) {
            if (executable.kind != ExecutableKind::Provider)
                throw std::invalid_argument("This host binds no " +
                                            std::string(to_string(executable.kind)) + " '" +
                                            executable.name + "'");
            if (!context) reject_unbound(executable);
            if (provider && *provider != executable)
                throw std::invalid_argument("This host binds one provider; the Program requires both '" +
                                            provider->name + "' and '" + executable.name + "'");
            provider = executable;
        }
        if (!provider) return binding;
        binding.node_context          = *context;
        binding.node_context.tools    = ToolSet();
        binding.node_context.provider = std::make_shared<RegisteredNameProvider>(
            provider->name, context->provider);
        binding.receipts.push_back({*provider, provider_binding_identity(label, *provider)});
        return binding;
    };
}

}  // namespace neograph::program::detail
