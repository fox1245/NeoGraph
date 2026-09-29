#pragma once

#include <neograph/graph/engine.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace neograph::graph {

/**
 * Owning, concrete execution capability for protocol hosts. Copies keep the
 * engine alive through in-flight invocations; administration and runtime
 * configuration are deliberately absent. Construct after configuring the
 * engine, and drain invocations before replacing its configuration.
 */
class NEOGRAPH_API GraphExecution final {
public:
    explicit GraphExecution(std::shared_ptr<GraphEngine> engine)
        : engine_(std::move(engine)) {
        if (!engine_) throw std::invalid_argument("GraphExecution requires an engine");
    }

    asio::awaitable<RunResult> run_stream_async(
        RunConfig config, GraphStreamCallback cb,
        RunMetadata metadata, RunResources resources) const {
        return engine_->run_stream_async(std::move(config), std::move(cb),
                                         std::move(metadata), std::move(resources));
    }

    asio::awaitable<RunResult> resume_async(
        RunConfig config, json value, GraphStreamCallback cb,
        RunMetadata metadata, RunResources resources) const {
        return engine_->resume_async(std::move(config), std::move(value),
                                     std::move(cb), std::move(metadata),
                                     std::move(resources));
    }

    asio::awaitable<RunResult> resume_from_async(
        RunConfig config, std::string checkpoint_id, json value,
        GraphStreamCallback cb, RunMetadata metadata,
        RunResources resources) const {
        return engine_->resume_from_async(std::move(config), std::move(checkpoint_id),
                                          std::move(value), std::move(cb),
                                          std::move(metadata), std::move(resources));
    }

    /** Resume admission only: absent session or latest interrupt phase.
     * Uses the same exclusive state-read policy as administration; does not
     * expose checkpoint values, history, or any mutation to a protocol host.
     */
    std::optional<CheckpointPhase> latest_resume_phase(const std::string& thread_id) const {
        auto history = engine_->get_state_history(thread_id, 1);
        if (history.empty()) return std::nullopt;
        return history.front().interrupt_phase;
    }

private:
    std::shared_ptr<GraphEngine> engine_;
};

} // namespace neograph::graph
