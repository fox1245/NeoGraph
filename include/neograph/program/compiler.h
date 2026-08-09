/**
 * @file program/compiler.h
 * @brief Pure single-root Program-v1 compiler.
 */
#pragma once

#include <neograph/program/bundle.h>
#include <neograph/program/module.h>
#include <neograph/program/registry.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::program {

struct JavaScriptCompileLimits {
    std::size_t   memory_limit_bytes          = 32u * 1024u * 1024u;
    std::size_t   max_stack_bytes             = 512u * 1024u;
    std::uint64_t max_interrupt_polls         = 1'000'000u;
    std::uint64_t max_wall_time_ms            = 60'000u;
    std::size_t   max_generated_document_bytes = 16u * 1024u * 1024u;
};

struct ProgramCompilerConfig {
    // Exact opaque build identity. It identifies Program-v1 normalization,
    // closure, Core-parser, round-trip, and validator semantics together.
    // Mandatory, nonempty UTF-8; do not synthesize from pointer/RTTI/process state.
    std::string compiler_build_id;
    // Applied only to SourceKind::JavaScript. Limits are host policy, not
    // source-authored semantics; changing them requires a new build identity.
    JavaScriptCompileLimits javascript;
    /**
     * Exact JavaScript frontend identity. These defaults describe the
     * vendored QuickJS target; changing any value is a semantic compiler
     * change and therefore changes every JavaScript bundle identity.
     */
    std::string quickjs_release = std::string(ProgramSource::JAVASCRIPT_ENGINE_VERSION);
    std::string quickjs_archive_digest =
        std::string(ProgramSource::JAVASCRIPT_QUICKJS_ARCHIVE_DIGEST);
    std::string quickjs_build_options =
        std::string(ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS);
    std::string javascript_profile = std::string(ProgramSource::JAVASCRIPT_PROFILE);
    std::uint32_t javascript_profile_version = ProgramSource::JAVASCRIPT_PROFILE_VERSION;
    std::uint32_t ng_api_version            = ProgramSource::JAVASCRIPT_NG_API_VERSION;
};

class NEOGRAPH_PROGRAM_API ProgramCompileError final : public std::runtime_error {
public:
    explicit ProgramCompileError(std::vector<Diagnostic> diagnostics);
    const std::vector<Diagnostic>& diagnostics() const noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

class NEOGRAPH_PROGRAM_API ProgramCompiler {
public:
    static constexpr std::uint32_t PROGRAM_SCHEMA_VERSION = 1;

    ProgramCompiler(RegistrySnapshot registry, ProgramCompilerConfig config);
    ProgramCompiler(ProgramCompiler&&) noexcept;
    ProgramCompiler& operator=(ProgramCompiler&&) noexcept;
    ProgramCompiler(const ProgramCompiler&)            = delete;
    ProgramCompiler& operator=(const ProgramCompiler&) = delete;
    ~ProgramCompiler();

    const std::string& compiler_build_id() const noexcept;
    const std::string& registry_snapshot_fingerprint() const noexcept;
    ProgramBundle      compile(const ProgramSource& source) const;
    /** Compile only when the source imports exactly one verified module closure. */
    ProgramBundle compile(const ProgramSource& source, const ModuleResolution& resolution) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
