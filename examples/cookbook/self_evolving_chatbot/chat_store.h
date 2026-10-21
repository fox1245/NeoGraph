#pragma once
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#include <neograph/program/transition_store.h>
#include <core/native_archive.h>

#include "program_chat.h"

#include <mutex>

namespace evolving_chat {
class ChatStore final {
public:
    explicit ChatStore(const Options& options);
    ~ChatStore();
    json load(const std::string& owner);
    // Atomically replaces a snapshot only at its exact expected revision.
    void                                             save(const std::string& owner, json& value);
    std::shared_ptr<sp::NativeArchive> native_archive(
        const std::string& owner, const sp::descriptor::ValidatedDescriptor& descriptor,
        bool allow_provision);
    std::shared_ptr<neograph::program::ProgramStore> programs;
    std::shared_ptr<neograph::program::ProgramTransitionStore> transitions;
    // The same two stores for code that cannot suspend. The Program store interfaces are awaitable (#556).
    std::shared_ptr<neograph::program::BlockingProgramStore>           programs_view;
    std::shared_ptr<neograph::program::BlockingProgramTransitionStore> transitions_view;
    std::shared_ptr<neograph::graph::CheckpointStore>          checkpoints;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace evolving_chat
