/**
 * @file program/store.h
 * @brief Atomic storage of admitted Program bundles and versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/bundle.h>
#include <neograph/program/version.h>

#include <memory>
#include <optional>
#include <string_view>

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
