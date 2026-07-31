/**
 * @file program/catalog.h
 * @brief Admission and process-local materialization of immutable Programs.
 */
#pragma once

#include <neograph/program/admission.h>
#include <neograph/program/version.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

namespace detail {
class CatalogRuntimeAccess;
}

class ProgramStore;

struct ProgramAdmission {
    std::string                    owner_scope;
    AdmissionProfile               profile;
    PolicySnapshot                 policy;
    std::vector<DependencyReceipt> dependency_receipts;
};

class NEOGRAPH_PROGRAM_API ProgramAdmissionError final : public std::runtime_error {
public:
    explicit ProgramAdmissionError(std::vector<Diagnostic> diagnostics);

    const std::vector<Diagnostic>& diagnostics() const noexcept;

private:
    std::vector<Diagnostic> diagnostics_;
};

/**
 * Process-local cache of fully linked Core generations.
 *
 * The cache exposes no GraphEngine surface. It may be shared only by Catalogs
 * with the same immutable registry fingerprint and compiler build identity.
 */
class NEOGRAPH_PROGRAM_API EngineGenerationCache {
public:
    EngineGenerationCache();
    EngineGenerationCache(EngineGenerationCache&&) noexcept;
    EngineGenerationCache& operator=(EngineGenerationCache&&) noexcept;
    EngineGenerationCache(const EngineGenerationCache&)            = delete;
    EngineGenerationCache& operator=(const EngineGenerationCache&) = delete;
    ~EngineGenerationCache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class ProgramCatalog;
};

struct CatalogConfig {
    std::shared_ptr<ProgramStore>          program_store;
    RegistrySnapshot                       registry;
    std::shared_ptr<EngineGenerationCache> engines;
    std::string                            compiler_build_id;
};

class NEOGRAPH_PROGRAM_API ProgramCatalog {
public:
    explicit ProgramCatalog(CatalogConfig config);
    ProgramCatalog(ProgramCatalog&&) noexcept;
    ProgramCatalog& operator=(ProgramCatalog&&) noexcept;
    ProgramCatalog(const ProgramCatalog&)            = delete;
    ProgramCatalog& operator=(const ProgramCatalog&) = delete;
    ~ProgramCatalog();

    ProgramVersion                admit(const ProgramBundle& bundle, ProgramAdmission admission);
    std::optional<ProgramVersion> find_version(std::string_view id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class detail::CatalogRuntimeAccess;
};

}  // namespace neograph::program
