#include <neograph/program/authoring.h>

#include <stdexcept>
#include <utility>

namespace neograph::program {

std::string_view to_string(AuthoringFrontend frontend) noexcept {
    switch (frontend) {
        case AuthoringFrontend::JavaScript:
            return "javascript";
        case AuthoringFrontend::TrustedCpp:
            return "trusted_cpp";
        case AuthoringFrontend::StrictCoreJson:
            return "strict_core_json";
        case AuthoringFrontend::CoreDsl:
            return "core_dsl";
        case AuthoringFrontend::ProgramJson:
            return "program_json";
    }
    return "unknown";
}

AuthoringFrontend authoring_frontend_from_string(std::string_view value) {
    if (value == "javascript") return AuthoringFrontend::JavaScript;
    if (value == "trusted_cpp") return AuthoringFrontend::TrustedCpp;
    if (value == "strict_core_json") return AuthoringFrontend::StrictCoreJson;
    if (value == "core_dsl") return AuthoringFrontend::CoreDsl;
    if (value == "program_json") return AuthoringFrontend::ProgramJson;
    throw std::invalid_argument("Unknown authoring frontend: " + std::string(value));
}

ProgramSource make_javascript_source(JavaScriptPublicationRequest request) {
    return ProgramSource::from_javascript(std::move(request.source_id), std::move(request.source),
                                          std::move(request.imports),
                                          std::move(request.source_map));
}

ProgramBundle compile_javascript(const ProgramCompiler& compiler,
                                 JavaScriptPublicationRequest request) {
    return compiler.compile(make_javascript_source(std::move(request)));
}

ProgramVersion publish_javascript(const ProgramCompiler& compiler,
                                  ProgramCatalog&       catalog,
                                  JavaScriptPublicationRequest request,
                                  ProgramAdmission      admission) {
    auto bundle = compile_javascript(compiler, std::move(request));
    return catalog.admit(bundle, std::move(admission));
}

StoredArtifactClassificationRule classify_stored_artifact(
    StoredArtifactKind kind,
    bool               translated,
    bool               exact_legacy_runtime,
    bool               recoverable_run) {
    if (translated) {
        return {StoredArtifactClassification::Translated, true, true, false, {}, kind};
    }
    if (exact_legacy_runtime && recoverable_run) {
        return {StoredArtifactClassification::DrainOnly, false, false, true,
                "P_MIGRATION_DRAIN_ONLY", kind};
    }
    const auto diagnostic = kind == StoredArtifactKind::LegacyCoreDefinition
                                ? "P_MIGRATION_CORE_REJECTED"
                                : "P_MIGRATION_PROGRAM_REJECTED";
    return {StoredArtifactClassification::Rejected, false, false, false, diagnostic, kind};
}

std::string_view legacy_authoring_diagnostic(AuthoringFrontend frontend) noexcept {
    switch (frontend) {
        case AuthoringFrontend::CoreDsl:
            return "P_MIGRATION_CORE_DSL";
        case AuthoringFrontend::StrictCoreJson:
            return "P_MIGRATION_CORE_JSON";
        case AuthoringFrontend::ProgramJson:
            return "P_MIGRATION_PROGRAM_JSON";
        case AuthoringFrontend::JavaScript:
        case AuthoringFrontend::TrustedCpp:
            return {};
    }
    return {};
}

}  // namespace neograph::program
