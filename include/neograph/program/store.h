/**
 * @file program/store.h
 * @brief Atomic storage of admitted Program bundles and versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/activation.h>
#include <neograph/program/bundle.h>
#include <neograph/program/version.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

class NEOGRAPH_PROGRAM_API ProgramStore {
public:
    virtual ~ProgramStore() = default;

    /**
     * Atomically publish one admitted bundle/version tuple.
     *
     * Implementations must provide the strong exception guarantee: if this call throws,
     * neither object may become observable through get_bundle() or get_version().
     */
    virtual void publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) = 0;
    virtual std::optional<ProgramBundle>  get_bundle(std::string_view id) const               = 0;
    virtual std::optional<ProgramVersion> get_version(std::string_view id) const              = 0;

    /**
     * Owner-qualified lookups. A wrong owner is indistinguishable from an
     * absent object. The legacy unqualified overloads remain available for
     * local migration/introspection callers, but Catalog and Runtime must use
     * these overloads for authority-bearing resolution.
     */
    virtual std::optional<ProgramBundle> get_bundle(std::string_view /*owner_scope*/,
                                                    std::string_view /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramVersion> get_version(std::string_view owner_scope,
                                                      std::string_view id) const {
        if (owner_scope.empty()) return std::nullopt;
        const auto value = get_version(id);
        if (!value || value->ownership_scope() != owner_scope) return std::nullopt;
        return value;
    }

    /**
     * Owner-scoped activation lifecycle.  The default keeps the bounded P2
     * adapter intentionally administration-inaccessible. Implementations must
     * verify that the target version and policy hash belong to owner_scope and
     * must return Conflict without publishing when the generation is stale.
     */
    virtual std::optional<ProgramActivation>
    get_activation(std::string_view /*owner_scope*/) const {
        throw std::logic_error("ProgramStore does not expose activation lifecycle");
    }
    virtual ProgramActivationResult compare_activate(
        std::string_view /*owner_scope*/,
        std::uint64_t /*expected_generation*/,
        std::string_view /*version_id*/,
        std::string_view /*policy_snapshot_hash*/) {
        throw std::logic_error("ProgramStore does not expose activation lifecycle");
    }
    virtual std::vector<ProgramVersion>
    list_versions(std::string_view /*owner_scope*/) const {
        throw std::logic_error("ProgramStore does not enumerate versions");
    }
    virtual ProgramRetentionReport
    collect_garbage(std::string_view /*owner_scope*/,
                    const std::vector<std::string>& /*pinned_version_ids*/) {
        throw std::logic_error("ProgramStore does not expose retention lifecycle");
    }
};

class NEOGRAPH_PROGRAM_API InMemoryProgramStore final : public ProgramStore {
public:
    InMemoryProgramStore();
    InMemoryProgramStore(InMemoryProgramStore&&) noexcept;
    InMemoryProgramStore& operator=(InMemoryProgramStore&&) noexcept;
    InMemoryProgramStore(const InMemoryProgramStore&)            = delete;
    InMemoryProgramStore& operator=(const InMemoryProgramStore&) = delete;
    ~InMemoryProgramStore() override;

    void publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) override;
    std::optional<ProgramBundle>  get_bundle(std::string_view id) const override;
    std::optional<ProgramVersion> get_version(std::string_view id) const override;
    std::optional<ProgramBundle>  get_bundle(std::string_view owner_scope,
                                             std::string_view id) const override;
    std::optional<ProgramVersion> get_version(std::string_view owner_scope,
                                              std::string_view id) const override;
    std::optional<ProgramActivation>
    get_activation(std::string_view owner_scope) const override;
    ProgramActivationResult compare_activate(std::string_view owner_scope,
                                             std::uint64_t    expected_generation,
                                             std::string_view version_id,
                                             std::string_view policy_snapshot_hash) override;
    std::vector<ProgramVersion> list_versions(std::string_view owner_scope) const override;
    ProgramRetentionReport collect_garbage(
        std::string_view owner_scope,
        const std::vector<std::string>& pinned_version_ids) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
