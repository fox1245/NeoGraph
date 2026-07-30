/**
 * @file program/version.h
 * @brief Immutable stored binding between a Program bundle and admission receipts.
 */
#pragma once

#include <neograph/program/bundle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

struct ProgramVersionData {
    std::string bundle_id;
    json        admission_profile   = json::object();
    json        policy_snapshot     = json::object();
    json        dependency_receipts = json::array();
    std::string ownership_scope;
    json        core_materialization_receipt = json::object();
};

class NEOGRAPH_PROGRAM_API ProgramVersion {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramVersion(ProgramVersionData data);
    static ProgramVersion parse(std::string_view stored_bytes);

    const std::string& id() const noexcept;
    const std::string& bundle_id() const noexcept;
    json               admission_profile() const;
    json               policy_snapshot() const;
    json               dependency_receipts() const;
    const std::string& ownership_scope() const noexcept;
    json               core_materialization_receipt() const;
    std::string        serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramVersion(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
