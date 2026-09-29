/**
 * @file graph/admin.h
 * @brief Borrowed state-administration facade for GraphEngine.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#include <optional>
#include <string>
#include <vector>

namespace neograph::graph {

class GraphEngine;

/**
 * @brief State inspection and mutation separated from graph execution calls.
 *
 * GraphAdmin borrows a GraphEngine and must not outlive it. The corresponding
 * GraphEngine methods remain as compatibility entry points; both use the same
 * exclusive admission policy: reject operations during any in-flight execution
 * (even for a different thread_id), and reject execution during administration.
 * Drain and await the execution before retrying; no administrative checkpoint
 * will be published after a rejected operation. Different engines sharing a
 * checkpoint store still need host-level coordination.
 */
class NEOGRAPH_API GraphAdmin {
public:
    explicit GraphAdmin(GraphEngine& engine) noexcept : engine_(&engine) {}

    std::optional<json> get_state(const std::string& thread_id) const;
    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;
    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "") const;
    void update_state_writes(const std::string& thread_id,
                             const std::vector<ChannelWrite>& channel_writes,
                             const std::string& as_node = "") const;
    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "") const;

private:
    GraphEngine* engine_;
};

} // namespace neograph::graph
