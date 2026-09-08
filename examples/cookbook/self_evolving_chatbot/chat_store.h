#pragma once
#include <neograph/program/program.h>
#include <neograph/program/store.h>

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
    std::shared_ptr<neograph::program::ProgramStore> programs;
    std::shared_ptr<neograph::program::ProgramTransitionStore> transitions;
    std::shared_ptr<neograph::graph::CheckpointStore>          checkpoints;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace evolving_chat
