/**
 * @file provider_binding.h
 * @brief Internal: binds one host provider to the llm_call nodes of admitted Programs.
 *
 * A host (the Python LocalProgramHost) binds one provider object, with its
 * model and request controls, under a non-secret binding label. The label is
 * the only part of the binding that enters identity: each Program version that
 * requires the provider carries a capability receipt whose binding identity is
 * a digest of the label and the provider's registry identity. The provider's
 * key, endpoint and model never enter an identity. A Program that requires no
 * provider gets no receipt, so its version id is unchanged (#372).
 *
 * Exported for the Python binding only; no compatibility promise.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/program/catalog.h>
#include <neograph/program/version.h>

#include <optional>
#include <string>
#include <string_view>

namespace neograph::program::detail {

/**
 * The label must be 1-64 characters of a-z, 0-9, '.', '_', '-' or ':',
 * starting with a letter or digit, and must not look like an API key.
 * Throws std::invalid_argument.
 */
NEOGRAPH_PROGRAM_API std::string validated_binding_label(std::string_view label);

/** Capability receipt identity of one bound provider. */
NEOGRAPH_PROGRAM_API std::string provider_binding_identity(std::string_view            label,
                                                           const ExecutableIdentity& provider);

/** The provider journal's deployment identity for a binding label. */
NEOGRAPH_PROGRAM_API std::string provider_deployment_identity(std::string_view label);

/**
 * Catalog binder for a host that binds at most one provider. A Program whose
 * closure requires a Provider gets `context` with its provider answering to
 * the registry's provider name; without a context the requirement is rejected
 * with P_ADMIT_PROVIDER. Tools and imports are never bound.
 */
NEOGRAPH_PROGRAM_API CatalogCapabilityBinder single_provider_binder(
    std::optional<graph::NodeContext> context, std::string label);

}  // namespace neograph::program::detail
