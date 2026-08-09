/**
 * @file program/authoring.h
 * @brief The public authoring boundary and stored-artifact migration rules.
 *
 * JavaScript is the public source language.  The direct C++ API remains a
 * trusted embedding surface, while the JSON values carried by ProgramSource,
 * bundles, and journals remain canonical data rather than another source
 * language.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/catalog.h>
#include <neograph/program/compiler.h>
#include <neograph/program/migration.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Public source frontends understood by an embedding or transport. */
enum class AuthoringFrontend : std::uint8_t {
    JavaScript,
    TrustedCpp,
    StrictCoreJson,
    CoreDsl,
    ProgramJson,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(AuthoringFrontend frontend) noexcept;
NEOGRAPH_PROGRAM_API AuthoringFrontend authoring_frontend_from_string(std::string_view value);

/**
 * A direct JavaScript publication request.  The request deliberately carries
 * source text, not a caller-created Program JSON document; ProgramSource is
 * the one canonical envelope used by both direct and Harness publication.
 */
struct JavaScriptPublicationRequest {
    std::string                 source_id;
    std::string                 source;
    std::vector<ImportRef>      imports;
    std::vector<SourceMapEntry> source_map;
};

NEOGRAPH_PROGRAM_API ProgramSource make_javascript_source(
    JavaScriptPublicationRequest request);

/** Compile a JavaScript define()/main() module through the direct API. */
NEOGRAPH_PROGRAM_API ProgramBundle compile_javascript(
    const ProgramCompiler& compiler,
    JavaScriptPublicationRequest request);

/** Compile and admit one JavaScript define()/main() module through the direct API. */
NEOGRAPH_PROGRAM_API ProgramVersion publish_javascript(
    const ProgramCompiler& compiler,
    ProgramCatalog&       catalog,
    JavaScriptPublicationRequest request,
    ProgramAdmission      admission);

/**
 * Build the explicit cutover rule for a retained legacy artifact.
 *
 * `translated` is true only when deterministic equivalence evidence has been
 * accepted.  A legacy artifact without that proof is drain-only only when an
 * exact legacy runtime is still available for a recoverable/pinned run;
 * otherwise it is rejected.  No rule performs shape-based source selection.
 */
NEOGRAPH_PROGRAM_API StoredArtifactClassificationRule classify_stored_artifact(
    StoredArtifactKind kind,
    bool               translated,
    bool               exact_legacy_runtime,
    bool               recoverable_run);

/** Stable migration diagnostic code for a legacy public source frontend. */
NEOGRAPH_PROGRAM_API std::string_view legacy_authoring_diagnostic(
    AuthoringFrontend frontend) noexcept;

}  // namespace neograph::program
