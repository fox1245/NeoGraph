#pragma once

#include <neograph/program/transition_store.h>

#include <stdexcept>

namespace neograph::program::detail {

inline void validate_command_publication_head(const ProgramCommandPublicationHead& head,
                                             std::string_view owner,
                                             std::string_view run_id) {
    const auto& run = head.run_record;
    const auto& journal = head.journal_record;
    if (run.owner_scope() != owner || run.run_id() != run_id || journal.run_id != run_id ||
        run.journal_head() != journal.id || run.bundle_id() != journal.bundle_id ||
        run.program_version_id() != journal.program_version_id ||
        (head.latest_command && head.latest_command->bundle_id() != run.bundle_id())) {
        throw std::invalid_argument("Stored Program command publication head binding is corrupt");
    }
}

}  // namespace neograph::program::detail
