/**
 * @file program/source.h
 * @brief Deeply owned, immutable Program source values.
 */
#pragma once

#include <neograph/program/diagnostic.h>
#include <neograph/program/schema.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/**
 * Source identity carried by Program values. CanonicalJson is retained only
 * for decoding already-stored legacy artifacts; it is not a new-authoring
 * frontend. New sources are created through the trusted C++ builder or the
 * JavaScript frontend.
 */
enum class SourceKind { CanonicalJson, CppBuilder, JavaScript };

struct ImportRef {
    std::string source_id;
    std::string content_identity;

    bool operator==(const ImportRef&) const = default;
};

/**
 * Exact source bytes for one reviewed pure JavaScript module. `source_id`
 * resolves only through an exact ImportRef receipt; `content_identity` is
 * checked against that receipt before QuickJS can compile the module.
 */
struct SealedJavaScriptModule {
    std::string source_id;
    std::string content_identity;
    std::string source_text;

    bool operator==(const SealedJavaScriptModule&) const = default;
};

struct SourceMapEntry {
    std::string      generated_pointer;
    SourceCoordinate authored;

    bool operator==(const SourceMapEntry&) const = default;
};

/**
 * Exact identity of the private JavaScript frontend.  The release alone is
 * not sufficient to identify JavaScript semantics: the vendored archive and
 * the compile-time feature set are part of the durable boundary as well.
 */
struct JavaScriptRuntimeIdentity {
    std::string  quickjs_release;
    std::string  quickjs_archive_digest;
    std::string  quickjs_build_options;
    std::string  profile;
    std::uint32_t profile_version = 0;
    std::uint32_t ng_api_version  = 0;

    bool operator==(const JavaScriptRuntimeIdentity&) const = default;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(SourceKind kind) noexcept;
NEOGRAPH_PROGRAM_API SourceKind       source_kind_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API void             to_json(json& value, const ImportRef& import_ref);
NEOGRAPH_PROGRAM_API void             from_json(const json& value, ImportRef& import_ref);
NEOGRAPH_PROGRAM_API void             to_json(json& value, const SourceMapEntry& entry);
NEOGRAPH_PROGRAM_API void             to_json(json& value,
                                              const SealedJavaScriptModule& module);
NEOGRAPH_PROGRAM_API void             from_json(const json& value,
                                                SealedJavaScriptModule& module);
NEOGRAPH_PROGRAM_API void             from_json(const json& value, SourceMapEntry& entry);

class NEOGRAPH_PROGRAM_API ProgramSource {
public:
    static constexpr std::uint32_t    STORAGE_SCHEMA_VERSION            = 1;
    static constexpr std::uint32_t    JAVASCRIPT_PROGRAM_SCHEMA_VERSION =
        LATEST_PROGRAM_SCHEMA_VERSION;
    static constexpr std::uint32_t    JAVASCRIPT_LANGUAGE_VERSION       = 1;
    static constexpr std::uint32_t    JAVASCRIPT_HOST_API_VERSION       = 1;
    static constexpr std::string_view JAVASCRIPT_ENGINE                 = "quickjs";
    static constexpr std::string_view JAVASCRIPT_ENGINE_VERSION         = "2026-06-04";
    static constexpr std::string_view JAVASCRIPT_QUICKJS_ARCHIVE_DIGEST =
        "sha256:b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a";
    static constexpr std::string_view JAVASCRIPT_QUICKJS_BUILD_OPTIONS =
#if defined(_MSC_VER)
        "CONFIG_VERSION=2026-06-04;QUICKJS_LIBC=disabled;STD_OS=disabled;"
        "DYNAMIC_MODULES=disabled;MSVC_INTRINSICS=disabled;"
        "NEOGRAPH_PLATFORM_PORT=msvc-v1;"
        "NEOGRAPH_PLATFORM_PORT_SHA256="
        "4dcab8f86e5bc365c42d27b323f333996fc525ac5759d855d7960a8a9b7d851f";
#else
        "CONFIG_VERSION=2026-06-04;QUICKJS_LIBC=disabled;STD_OS=disabled;"
        "DYNAMIC_MODULES=disabled";
#endif
    static constexpr std::string_view JAVASCRIPT_PROFILE                = "sealed-v1";
    static constexpr std::uint32_t    JAVASCRIPT_PROFILE_VERSION        = 1;
    static constexpr std::uint32_t    JAVASCRIPT_NG_API_VERSION          = 1;

    static JavaScriptRuntimeIdentity default_javascript_runtime_identity();

    /**
     * Store a sealed JavaScript control source. The text is evaluated only by
     * an explicitly enabled compiler frontend; it is never a runtime node or
     * a durable bytecode artifact.
     */
    static ProgramSource from_javascript(std::string                      source_id,
                                         std::string                      source_text,
                                         std::vector<ImportRef>           imports    = {},
                                         std::vector<SourceMapEntry>      source_map = {},
                                         std::vector<SealedJavaScriptModule> sealed_modules = {});

    static ProgramSource from_cpp_builder(std::string                 source_id,
                                          std::uint32_t               schema_version,
                                          json                        document,
                                          std::vector<ImportRef>      imports    = {},
                                          std::vector<SourceMapEntry> source_map = {});

    /** Decode a versioned stored value; this is not a public source frontend. */
    static ProgramSource parse(std::string_view stored_bytes);

    SourceKind                         kind() const noexcept;
    std::uint32_t                      schema_version() const noexcept;
    const std::string&                 source_id() const noexcept;
    json                               document() const;
    const std::vector<ImportRef>&      imports() const noexcept;
    const std::vector<SourceMapEntry>& source_map() const noexcept;
    /**
     * Source bytes for exact receipt-bound pure modules. Empty unless the
     * source was compiled with a verified module closure.
     */
    const std::vector<SealedJavaScriptModule>& sealed_modules() const noexcept;
    /** Runtime/profile identity sealed into a JavaScript source envelope. */
    JavaScriptRuntimeIdentity          javascript_runtime_identity() const;
    /**
     * Hashes the declared Program schema version and canonical authored document.
     * Import identities are dependency metadata and are bound separately by a
     * ProgramBundle's module dependency Merkle root.
     */
    const std::string& source_hash() const noexcept;
    const std::string& canonical_document() const noexcept;
    std::string        serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramSource(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
