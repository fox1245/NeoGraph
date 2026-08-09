/**
 * @file program/source.h
 * @brief Deeply owned, immutable Program source values.
 */
#pragma once

#include <neograph/program/diagnostic.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class SourceKind { CanonicalJson, CppBuilder, JavaScript };

struct ImportRef {
    std::string source_id;
    std::string content_identity;

    bool operator==(const ImportRef&) const = default;
};

struct SourceMapEntry {
    std::string      generated_pointer;
    SourceCoordinate authored;

    bool operator==(const SourceMapEntry&) const = default;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(SourceKind kind) noexcept;
NEOGRAPH_PROGRAM_API SourceKind       source_kind_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API void             to_json(json& value, const ImportRef& import_ref);
NEOGRAPH_PROGRAM_API void             from_json(const json& value, ImportRef& import_ref);
NEOGRAPH_PROGRAM_API void             to_json(json& value, const SourceMapEntry& entry);
NEOGRAPH_PROGRAM_API void             from_json(const json& value, SourceMapEntry& entry);

class NEOGRAPH_PROGRAM_API ProgramSource {
public:
    static constexpr std::uint32_t    STORAGE_SCHEMA_VERSION            = 1;
    static constexpr std::uint32_t    JAVASCRIPT_PROGRAM_SCHEMA_VERSION = 1;
    static constexpr std::uint32_t    JAVASCRIPT_LANGUAGE_VERSION       = 1;
    static constexpr std::uint32_t    JAVASCRIPT_HOST_API_VERSION       = 1;
    static constexpr std::string_view JAVASCRIPT_ENGINE                 = "quickjs";
    static constexpr std::string_view JAVASCRIPT_ENGINE_VERSION         = "2026-06-04";

    static ProgramSource from_canonical_json(std::string                 source_id,
                                             std::string                 source_text,
                                             std::vector<ImportRef>      imports    = {},
                                             std::vector<SourceMapEntry> source_map = {});

    /**
     * Store a sealed JavaScript control source. The text is evaluated only by
     * an explicitly enabled compiler frontend; it is never a runtime node or
     * a durable bytecode artifact.
     */
    static ProgramSource from_javascript(std::string                 source_id,
                                         std::string                 source_text,
                                         std::vector<ImportRef>      imports    = {},
                                         std::vector<SourceMapEntry> source_map = {});

    static ProgramSource from_cpp_builder(std::string                 source_id,
                                          std::uint32_t               schema_version,
                                          json                        document,
                                          std::vector<ImportRef>      imports    = {},
                                          std::vector<SourceMapEntry> source_map = {});

    static ProgramSource parse(std::string_view stored_bytes);

    SourceKind                         kind() const noexcept;
    std::uint32_t                      schema_version() const noexcept;
    const std::string&                 source_id() const noexcept;
    json                               document() const;
    const std::vector<ImportRef>&      imports() const noexcept;
    const std::vector<SourceMapEntry>& source_map() const noexcept;
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
