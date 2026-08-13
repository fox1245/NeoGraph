#include <neograph/program/source.h>

#include "canonical_json.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::size_t kMaxJavaScriptSourceBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxJavaScriptModuleBytes = kMaxJavaScriptSourceBytes;

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program source field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program source field '" + owned_key + "' must be unsigned");
    }
    const auto number = value[owned_key].get<unsigned long long>();
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program source integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

[[noreturn]] void throw_source_error(std::string source_id, std::string code, std::string message) {
    Diagnostic diagnostic;
    diagnostic.phase                = CompilePhase::Source;
    diagnostic.code                 = std::move(code);
    diagnostic.severity             = DiagnosticSeverity::Error;
    diagnostic.primary.source_id    = std::move(source_id);
    diagnostic.primary.json_pointer = "";
    diagnostic.primary.span.reset();
    diagnostic.message = std::move(message);
    diagnostic.witness = json::object();
    throw ProgramDiagnosticError(std::move(diagnostic));
}

void validate_source_id(std::string_view source_id) {
    if (source_id.empty()) throw std::invalid_argument("Program source_id must not be empty");
    detail::validate_utf8(source_id);
}

void validate_common(std::string_view                   source_id,
                     std::uint32_t                      schema_version,
                     const json&                        document,
                     const std::vector<ImportRef>&      imports,
                     const std::vector<SourceMapEntry>& source_map) {
    validate_source_id(source_id);
    if (schema_version == 0) throw std::invalid_argument("Program schema version must be positive");
    if (!document.is_object())
        throw std::invalid_argument("Program source document must be an object");
    if (document.contains("program_schema_version")) {
        const auto&   encoded_version = document["program_schema_version"];
        std::uint64_t embedded        = 0;
        if (encoded_version.is_number_unsigned()) {
            embedded = encoded_version.get<unsigned long long>();
        } else if (encoded_version.is_number_integer()) {
            const auto signed_version = encoded_version.get<long long>();
            if (signed_version > 0) {
                embedded = static_cast<std::uint64_t>(signed_version);
            }
        } else {
            throw std::invalid_argument(
                "Program document program_schema_version must be an integer");
        }
        if (embedded == 0 || embedded > std::numeric_limits<std::uint32_t>::max() ||
            embedded != schema_version) {
            throw std::invalid_argument(
                "Program document schema version conflicts with source metadata");
        }
    }
    for (const auto& import_ref : imports) {
        if (import_ref.source_id.empty() || import_ref.content_identity.empty()) {
            throw std::invalid_argument("Program import requires source_id and content_identity");
        }
        detail::validate_token(import_ref.source_id, "Program import source_id");
        if (!detail::is_sha256_identity(import_ref.content_identity))
            throw std::invalid_argument("Program import content_identity must be sha256-pinned");
    }
    for (const auto& mapping : source_map) {
        detail::validate_json_pointer(mapping.generated_pointer);
        detail::validate_utf8(mapping.authored.source_id);
        detail::validate_utf8(mapping.authored.json_pointer);
        json coordinate;
        to_json(coordinate, mapping.authored);
    }
}

json sealed_modules_json(const std::vector<SealedJavaScriptModule>& modules) {
    json encoded = json::array();
    for (const auto& module : modules) {
        json item;
        to_json(item, module);
        encoded.push_back(std::move(item));
    }
    return encoded;
}

void validate_sealed_modules(const std::vector<ImportRef>& imports,
                             const std::vector<SealedJavaScriptModule>& modules) {
    std::size_t total_bytes = 0;
    std::vector<std::string> source_ids;
    source_ids.reserve(modules.size());
    for (const auto& module : modules) {
        if (module.source_id.empty() || module.content_identity.empty() ||
            module.source_text.empty()) {
            throw std::invalid_argument(
                "Sealed JavaScript module requires source_id, content_identity, and source_text");
        }
        detail::validate_token(module.source_id, "Sealed JavaScript module source_id");
        if (!detail::is_sha256_identity(module.content_identity)) {
            throw std::invalid_argument(
                "Sealed JavaScript module content_identity must be sha256-pinned");
        }
        detail::validate_utf8(module.source_text);
        if (module.source_text.size() > kMaxJavaScriptModuleBytes ||
            total_bytes > kMaxJavaScriptModuleBytes - module.source_text.size()) {
            throw std::invalid_argument(
                "Sealed JavaScript module sources exceed the 16 MiB input limit");
        }
        total_bytes += module.source_text.size();
        const auto receipt = std::find_if(
            imports.begin(), imports.end(), [&](const ImportRef& import_ref) {
                return import_ref.source_id == module.source_id &&
                       import_ref.content_identity == module.content_identity;
            });
        if (receipt == imports.end()) {
            throw std::invalid_argument(
                "Sealed JavaScript module is not bound to an exact Program import receipt");
        }
        source_ids.push_back(module.source_id);
    }
    std::sort(source_ids.begin(), source_ids.end());
    if (std::adjacent_find(source_ids.begin(), source_ids.end()) != source_ids.end()) {
        throw std::invalid_argument("Sealed JavaScript modules contain a duplicate source_id");
    }
}

void validate_javascript_document(const json& document) {
    detail::reject_unknown_fields(
        document, "JavaScript Program source",
        {"language", "language_version", "engine", "engine_version",
         "quickjs_archive_digest", "quickjs_build_options", "javascript_profile",
         "javascript_profile_version", "ng_api_version", "host_api_version", "source"});
    if (require_string(document, "language") != "javascript") {
        throw std::invalid_argument("JavaScript Program source language must be javascript");
    }
    if (require_uint32(document, "language_version") !=
        ProgramSource::JAVASCRIPT_LANGUAGE_VERSION) {
        throw std::invalid_argument("JavaScript Program source language version is unsupported");
    }
    if (require_string(document, "engine") != std::string(ProgramSource::JAVASCRIPT_ENGINE) ||
        require_string(document, "engine_version") !=
            std::string(ProgramSource::JAVASCRIPT_ENGINE_VERSION)) {
        throw std::invalid_argument("JavaScript Program source engine is unsupported");
    }
    if (require_string(document, "quickjs_archive_digest") !=
            std::string(ProgramSource::JAVASCRIPT_QUICKJS_ARCHIVE_DIGEST) ||
        require_string(document, "quickjs_build_options") !=
            std::string(ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS)) {
        throw std::invalid_argument("JavaScript Program source QuickJS build identity is unsupported");
    }
    if (require_string(document, "javascript_profile") !=
            std::string(ProgramSource::JAVASCRIPT_PROFILE) ||
        require_uint32(document, "javascript_profile_version") !=
            ProgramSource::JAVASCRIPT_PROFILE_VERSION) {
        throw std::invalid_argument("JavaScript Program source profile is unsupported");
    }
    if (require_uint32(document, "ng_api_version") != ProgramSource::JAVASCRIPT_NG_API_VERSION) {
        throw std::invalid_argument("JavaScript Program source ng API version is unsupported");
    }
    if (require_uint32(document, "host_api_version") !=
        ProgramSource::JAVASCRIPT_HOST_API_VERSION) {
        throw std::invalid_argument("JavaScript Program source host API version is unsupported");
    }
    detail::validate_utf8(require_string(document, "source"));
}

std::string compute_source_hash(std::uint32_t                             schema_version,
                                const json&                               document,
                                const std::vector<SealedJavaScriptModule>& sealed_modules = {}) {
    json identity                      = json::object();
    identity["document"]               = document;
    identity["program_schema_version"] = schema_version;
    if (!sealed_modules.empty()) {
        identity["sealed_modules"] = sealed_modules_json(sealed_modules);
    }
    return detail::sha256_identity("program-source", detail::canonical_json_bytes(identity));
}

}  // namespace

struct ProgramSource::Impl {
    SourceKind                  kind;
    std::uint32_t               schema_version;
    std::string                 source_id;
    json                        document;
    std::vector<ImportRef>      imports;
    std::vector<SourceMapEntry> source_map;
    std::vector<SealedJavaScriptModule> sealed_modules;
    std::string                 source_hash;
    std::string                 canonical_document;
};

std::string_view to_string(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::CanonicalJson:
            return "canonical_json";
        case SourceKind::CppBuilder:
            return "cpp_builder";
        case SourceKind::JavaScript:
            return "javascript";
    }
    return "unknown";
}

SourceKind source_kind_from_string(std::string_view value) {
    if (value == "canonical_json") return SourceKind::CanonicalJson;
    if (value == "cpp_builder") return SourceKind::CppBuilder;
    if (value == "javascript") return SourceKind::JavaScript;
    throw std::invalid_argument("Unknown Program source kind: " + std::string(value));
}

void to_json(json& value, const ImportRef& import_ref) {
    if (import_ref.source_id.empty() || import_ref.content_identity.empty()) {
        throw std::invalid_argument("Program import requires source_id and content_identity");
    }
    detail::validate_token(import_ref.source_id, "Program import source_id");
    if (!detail::is_sha256_identity(import_ref.content_identity))
        throw std::invalid_argument("Program import content_identity must be sha256-pinned");
    value                     = json::object();
    value["source_id"]        = import_ref.source_id;
    value["content_identity"] = import_ref.content_identity;
}

void from_json(const json& value, ImportRef& import_ref) {
    if (!value.is_object()) throw std::invalid_argument("Program import must be an object");
    detail::reject_unknown_fields(value, "Program import", {"source_id", "content_identity"});
    import_ref.source_id        = require_string(value, "source_id");
    import_ref.content_identity = require_string(value, "content_identity");
    if (import_ref.source_id.empty() || import_ref.content_identity.empty()) {
        throw std::invalid_argument("Program import requires source_id and content_identity");
    }
    detail::validate_token(import_ref.source_id, "Program import source_id");
    if (!detail::is_sha256_identity(import_ref.content_identity))
        throw std::invalid_argument("Program import content_identity must be sha256-pinned");
}

void to_json(json& value, const SealedJavaScriptModule& module) {
    if (module.source_id.empty() || module.content_identity.empty() || module.source_text.empty()) {
        throw std::invalid_argument(
            "Sealed JavaScript module requires source_id, content_identity, and source_text");
    }
    detail::validate_token(module.source_id, "Sealed JavaScript module source_id");
    if (!detail::is_sha256_identity(module.content_identity)) {
        throw std::invalid_argument("Sealed JavaScript module content_identity must be sha256-pinned");
    }
    detail::validate_utf8(module.source_text);
    value = json{{"source_id", module.source_id},
                 {"content_identity", module.content_identity},
                 {"source", module.source_text}};
}

void from_json(const json& value, SealedJavaScriptModule& module) {
    if (!value.is_object()) throw std::invalid_argument("Sealed JavaScript module must be an object");
    detail::reject_unknown_fields(value, "Sealed JavaScript module",
                                  {"source_id", "content_identity", "source"});
    module.source_id        = require_string(value, "source_id");
    module.content_identity = require_string(value, "content_identity");
    module.source_text      = require_string(value, "source");
    if (module.source_id.empty() || module.content_identity.empty() || module.source_text.empty()) {
        throw std::invalid_argument(
            "Sealed JavaScript module requires source_id, content_identity, and source");
    }
    detail::validate_token(module.source_id, "Sealed JavaScript module source_id");
    if (!detail::is_sha256_identity(module.content_identity)) {
        throw std::invalid_argument("Sealed JavaScript module content_identity must be sha256-pinned");
    }
    detail::validate_utf8(module.source_text);
}

void to_json(json& value, const SourceMapEntry& entry) {
    detail::validate_json_pointer(entry.generated_pointer);
    value                      = json::object();
    value["generated_pointer"] = entry.generated_pointer;
    json authored;
    to_json(authored, entry.authored);
    value["authored"] = std::move(authored);
}

void from_json(const json& value, SourceMapEntry& entry) {
    if (!value.is_object())
        throw std::invalid_argument("Program source map entry must be an object");
    detail::reject_unknown_fields(value, "Program source map entry",
                                  {"generated_pointer", "authored"});
    entry.generated_pointer = require_string(value, "generated_pointer");
    detail::validate_json_pointer(entry.generated_pointer);
    if (!value.contains("authored")) {
        throw std::invalid_argument("Program source map entry requires authored coordinate");
    }
    from_json(value["authored"], entry.authored);
}

ProgramSource::ProgramSource(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramSource ProgramSource::from_javascript(
    std::string source_id,
    std::string source_text,
    std::vector<ImportRef> imports,
    std::vector<SourceMapEntry> source_map,
    std::vector<SealedJavaScriptModule> sealed_modules) {
    validate_source_id(source_id);
    if (source_text.size() > kMaxJavaScriptSourceBytes) {
        throw_source_error(std::move(source_id), "P_SOURCE_SIZE",
                           "JavaScript Program source exceeds the 16 MiB input limit");
    }

    json        document{{"language", "javascript"},
                         {"language_version", JAVASCRIPT_LANGUAGE_VERSION},
                         {"engine", JAVASCRIPT_ENGINE},
                         {"engine_version", JAVASCRIPT_ENGINE_VERSION},
                         {"quickjs_archive_digest", JAVASCRIPT_QUICKJS_ARCHIVE_DIGEST},
                         {"quickjs_build_options", JAVASCRIPT_QUICKJS_BUILD_OPTIONS},
                         {"javascript_profile", JAVASCRIPT_PROFILE},
                         {"javascript_profile_version", JAVASCRIPT_PROFILE_VERSION},
                         {"ng_api_version", JAVASCRIPT_NG_API_VERSION},
                         {"host_api_version", JAVASCRIPT_HOST_API_VERSION},
                         {"source", std::move(source_text)}};
    std::string canonical_document;
    std::string source_hash;
    try {
        validate_common(source_id, JAVASCRIPT_PROGRAM_SCHEMA_VERSION, document, imports,
                        source_map);
        validate_sealed_modules(imports, sealed_modules);
        validate_javascript_document(document);
        canonical_document = detail::canonical_json_bytes(document);
        source_hash =
            compute_source_hash(JAVASCRIPT_PROGRAM_SCHEMA_VERSION, document, sealed_modules);
    } catch (const std::exception& error) {
        throw_source_error(std::move(source_id), "P_SOURCE_INVALID", error.what());
    }

    auto impl                = std::make_shared<Impl>();
    impl->kind               = SourceKind::JavaScript;
    impl->schema_version     = JAVASCRIPT_PROGRAM_SCHEMA_VERSION;
    impl->source_id          = std::move(source_id);
    impl->document           = std::move(document);
    impl->imports            = std::move(imports);
    impl->source_map         = std::move(source_map);
    impl->sealed_modules     = std::move(sealed_modules);
    impl->canonical_document = std::move(canonical_document);
    impl->source_hash        = std::move(source_hash);
    return ProgramSource(std::move(impl));
}

ProgramSource ProgramSource::from_cpp_builder(std::string                 source_id,
                                              std::uint32_t               schema_version,
                                              json                        document,
                                              std::vector<ImportRef>      imports,
                                              std::vector<SourceMapEntry> source_map) {
    validate_common(source_id, schema_version, document, imports, source_map);
    json        owned_document;
    std::string canonical_document;
    try {
        owned_document     = detail::owned_json_copy(document);
        canonical_document = detail::canonical_json_bytes(owned_document);
    } catch (const std::exception& error) {
        throw_source_error(std::move(source_id), "P_SOURCE_SIZE", error.what());
    }
    if (canonical_document.size() > kMaxJavaScriptSourceBytes) {
        throw_source_error(std::move(source_id), "P_SOURCE_SIZE",
                           "Native Program builder exceeds the 16 MiB source limit");
    }
    auto impl                = std::make_shared<Impl>();
    impl->kind               = SourceKind::CppBuilder;
    impl->schema_version     = schema_version;
    impl->source_id          = std::move(source_id);
    impl->document           = std::move(owned_document);
    impl->imports            = std::move(imports);
    impl->source_map         = std::move(source_map);
    impl->canonical_document = std::move(canonical_document);
    impl->source_hash        = compute_source_hash(impl->schema_version, impl->document);
    return ProgramSource(std::move(impl));
}

ProgramSource ProgramSource::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramSource JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != "neograph-program-source") {
        throw std::invalid_argument("Stored ProgramSource has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored ProgramSource",
        {"format", "storage_schema_version", "kind", "source_id", "program_schema_version",
         "document", "source_hash", "imports", "source_map", "sealed_modules"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramSource schema version is unsupported");
    }
    const auto kind                   = source_kind_from_string(require_string(value, "kind"));
    const auto source_id              = require_string(value, "source_id");
    const auto program_schema_version = require_uint32(value, "program_schema_version");
    if (!value.contains("document") || !value["document"].is_object()) {
        throw std::invalid_argument("Stored ProgramSource document must be an object");
    }

    std::vector<ImportRef> imports;
    if (!value.contains("imports") || !value["imports"].is_array()) {
        throw std::invalid_argument("Stored ProgramSource imports must be an array");
    }
    for (const auto item : value["imports"]) {
        ImportRef import_ref;
        from_json(item, import_ref);
        imports.push_back(std::move(import_ref));
    }
    std::vector<SourceMapEntry> source_map;
    if (!value.contains("source_map") || !value["source_map"].is_array()) {
        throw std::invalid_argument("Stored ProgramSource source_map must be an array");
    }
    for (const auto item : value["source_map"]) {
        SourceMapEntry entry;
        from_json(item, entry);
        source_map.push_back(std::move(entry));
    }

    std::vector<SealedJavaScriptModule> sealed_modules;
    if (value.contains("sealed_modules")) {
        if (!value["sealed_modules"].is_array()) {
            throw std::invalid_argument("Stored ProgramSource sealed_modules must be an array");
        }
        for (const auto& item : value["sealed_modules"]) {
            SealedJavaScriptModule module;
            from_json(item, module);
            sealed_modules.push_back(std::move(module));
        }
    }

    // CanonicalJson is intentionally accepted here only as a storage decode
    // path for legacy records. It is never exposed as a source constructor and
    // the compiler rejects it before any operation-tree parsing or dispatch.
    ProgramSource parsed = [&]() {
        if (kind == SourceKind::JavaScript) {
            if (program_schema_version != JAVASCRIPT_PROGRAM_SCHEMA_VERSION) {
                throw std::invalid_argument(
                    "Stored JavaScript ProgramSource schema version is unsupported");
            }
            validate_javascript_document(value["document"]);
            return from_javascript(source_id, require_string(value["document"], "source"),
                                   std::move(imports), std::move(source_map),
                                   std::move(sealed_modules));
        }
        if (!sealed_modules.empty()) {
            throw std::invalid_argument(
                "Stored non-JavaScript ProgramSource must not contain sealed_modules");
        }
        return from_cpp_builder(source_id, program_schema_version, value["document"],
                                std::move(imports), std::move(source_map));
    }();
    auto impl                  = std::make_shared<Impl>(*parsed.impl_);
    impl->kind                 = kind;
    const auto stored_identity = require_string(value, "source_hash");
    if (!detail::is_sha256_identity(stored_identity) || stored_identity != impl->source_hash) {
        throw std::invalid_argument("Stored ProgramSource source_hash does not match its content");
    }
    return ProgramSource(std::move(impl));
}

SourceKind ProgramSource::kind() const noexcept {
    return impl_->kind;
}

JavaScriptRuntimeIdentity ProgramSource::default_javascript_runtime_identity() {
    return {std::string(JAVASCRIPT_ENGINE_VERSION),
            std::string(JAVASCRIPT_QUICKJS_ARCHIVE_DIGEST),
            std::string(JAVASCRIPT_QUICKJS_BUILD_OPTIONS),
            std::string(JAVASCRIPT_PROFILE),
            JAVASCRIPT_PROFILE_VERSION,
            JAVASCRIPT_NG_API_VERSION};
}

JavaScriptRuntimeIdentity ProgramSource::javascript_runtime_identity() const {
    if (impl_->kind != SourceKind::JavaScript) return {};
    const auto& document = impl_->document;
    return {document.at("engine_version").get<std::string>(),
            document.at("quickjs_archive_digest").get<std::string>(),
            document.at("quickjs_build_options").get<std::string>(),
            document.at("javascript_profile").get<std::string>(),
            document.at("javascript_profile_version").get<std::uint32_t>(),
            document.at("ng_api_version").get<std::uint32_t>()};
}
std::uint32_t ProgramSource::schema_version() const noexcept {
    return impl_->schema_version;
}
const std::string& ProgramSource::source_id() const noexcept {
    return impl_->source_id;
}
json ProgramSource::document() const {
    return detail::owned_json_copy(impl_->document);
}
const std::vector<ImportRef>& ProgramSource::imports() const noexcept {
    return impl_->imports;
}
const std::vector<SourceMapEntry>& ProgramSource::source_map() const noexcept {
    return impl_->source_map;
}
const std::vector<SealedJavaScriptModule>& ProgramSource::sealed_modules() const noexcept {
    return impl_->sealed_modules;
}
const std::string& ProgramSource::source_hash() const noexcept {
    return impl_->source_hash;
}
const std::string& ProgramSource::canonical_document() const noexcept {
    return impl_->canonical_document;
}

std::string ProgramSource::serialize_canonical() const {
    json value                      = json::object();
    value["format"]                 = "neograph-program-source";
    value["storage_schema_version"] = STORAGE_SCHEMA_VERSION;
    value["kind"]                   = std::string(to_string(impl_->kind));
    value["source_id"]              = impl_->source_id;
    value["program_schema_version"] = impl_->schema_version;
    value["document"]               = impl_->document;
    value["source_hash"]            = impl_->source_hash;

    json imports = json::array();
    for (const auto& import_ref : impl_->imports) {
        json encoded;
        to_json(encoded, import_ref);
        imports.push_back(std::move(encoded));
    }
    value["imports"] = std::move(imports);

    json source_map = json::array();
    for (const auto& entry : impl_->source_map) {
        json encoded;
        to_json(encoded, entry);
        source_map.push_back(std::move(encoded));
    }
    value["source_map"] = std::move(source_map);

    if (!impl_->sealed_modules.empty()) {
        value["sealed_modules"] = sealed_modules_json(impl_->sealed_modules);
    }
    return detail::canonical_json_bytes(value);
}

}  // namespace neograph::program
