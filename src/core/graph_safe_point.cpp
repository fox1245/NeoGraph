#include <neograph/graph/safe_point.h>
#include <neograph/graph/cancel.h>

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph::graph {

struct GraphSafePoint::Impl {
    Impl(GraphGenerationIdentity generation_value, Checkpoint checkpoint_value)
        : generation(std::move(generation_value)),
          checkpoint(std::move(checkpoint_value)) {}

    GraphGenerationIdentity generation;
    Checkpoint checkpoint;
};

GraphSafePoint::GraphSafePoint(GraphGenerationIdentity generation, Checkpoint checkpoint)
    : impl_(std::make_shared<const Impl>(std::move(generation),
                                         std::move(checkpoint))) {}

const GraphGenerationIdentity& GraphSafePoint::generation() const noexcept {
    return impl_->generation;
}

const Checkpoint& GraphSafePoint::checkpoint() const noexcept {
    return impl_->checkpoint;
}

struct GraphSafePointRequest::Impl {
    enum class State {
        Idle,
        Requested,
        Captured,
        Closed,
    };

    mutable std::mutex            mutex;
    State                         state = State::Idle;
    bool                          attached = false;
    GraphGenerationIdentity       expected_generation;
    std::optional<GraphSafePoint> safe_point;
};

GraphSafePointRequest::GraphSafePointRequest(
    GraphGenerationIdentity expected_generation)
    : impl_(std::make_unique<Impl>()) {
    if (expected_generation.core_name.empty() ||
        expected_generation.core_generation_id.empty()) {
        throw std::invalid_argument(
            "Graph safe-point request requires an exact generation identity");
    }
    impl_->expected_generation = std::move(expected_generation);
}
GraphSafePointRequest::~GraphSafePointRequest() = default;

bool GraphSafePointRequest::request() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::Idle) return false;
    impl_->state = Impl::State::Requested;
    return true;
}

bool GraphSafePointRequest::requested() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->state == Impl::State::Requested;
}

bool GraphSafePointRequest::captured() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->state == Impl::State::Captured;
}

bool GraphSafePointRequest::closed() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->state == Impl::State::Closed;
}

GraphGenerationIdentity GraphSafePointRequest::expected_generation() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->expected_generation;
}

std::optional<GraphSafePoint> GraphSafePointRequest::safe_point() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->safe_point;
}

bool GraphSafePointRequest::attach(
    const GraphGenerationIdentity& generation) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->attached || impl_->state == Impl::State::Captured ||
        impl_->state == Impl::State::Closed ||
        generation != impl_->expected_generation) {
        return false;
    }
    impl_->attached = true;
    return true;
}

bool GraphSafePointRequest::capture(
    const GraphGenerationIdentity& generation,
    Checkpoint checkpoint,
    const CancelToken* cancellation) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->attached || impl_->state != Impl::State::Requested ||
        generation != impl_->expected_generation) {
        return false;
    }
    if (cancellation) {
        cancellation->throw_if_cancelled("safe-point boundary");
    }
    if (checkpoint.id.empty() || checkpoint.thread_id.empty() ||
        checkpoint.schema_version != CHECKPOINT_SCHEMA_VERSION ||
        checkpoint.interrupt_phase != CheckpointPhase::Completed ||
        checkpoint.next_nodes.empty() || checkpoint.step < 0 ||
        checkpoint.step >= std::numeric_limits<int>::max() - 1 ||
        checkpoint.timestamp < 0) {
        throw std::logic_error(
            "Graph safe point requires an exact completed checkpoint");
    }
    GraphSafePoint safe_point(generation, std::move(checkpoint));
    impl_->safe_point = std::move(safe_point);
    impl_->state = Impl::State::Captured;
    return true;
}

void GraphSafePointRequest::close() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->attached = false;
    if (impl_->state != Impl::State::Captured) {
        impl_->state = Impl::State::Closed;
    }
}

void GraphSafePointRequest::reject() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->attached && impl_->state != Impl::State::Captured &&
        impl_->state != Impl::State::Closed) {
        impl_->state = Impl::State::Closed;
    }
}

}  // namespace neograph::graph
