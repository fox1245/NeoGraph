/**
 * @file graph/safe_point.h
 * @brief Host-owned capture of one durably completed graph super-step.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>

#include <memory>
#include <optional>
#include <string>

namespace neograph::graph {

class GraphEngine;
class CancelToken;

/** Exact host-supplied identity permanently bound to one linked engine. */
struct GraphGenerationIdentity {
    std::string core_name;
    std::string core_generation_id;

    bool operator==(const GraphGenerationIdentity&) const = default;
};

/** Immutable snapshot exposed only after a completed super-step is durable. */
class NEOGRAPH_API GraphSafePoint final {
public:
    GraphSafePoint(const GraphSafePoint&) = default;
    GraphSafePoint& operator=(const GraphSafePoint&) = default;

    const GraphGenerationIdentity& generation() const noexcept;
    const Checkpoint& checkpoint() const noexcept;

private:
    struct Impl;
    GraphSafePoint(GraphGenerationIdentity generation, Checkpoint checkpoint);
    std::shared_ptr<const Impl> impl_;

    friend class GraphSafePointRequest;
};

/**
 * One-shot host controller for stopping at the next completed super-step.
 *
 * The request carries no topology or execution authority. GraphEngine fulfills
 * it only after checkpoint persistence and prior pending-write settlement have
 * both succeeded. The captured value remains owned by this controller.
 */
class NEOGRAPH_API GraphSafePointRequest final {
public:
    explicit GraphSafePointRequest(GraphGenerationIdentity expected_generation);
    GraphSafePointRequest(const GraphSafePointRequest&) = delete;
    GraphSafePointRequest& operator=(const GraphSafePointRequest&) = delete;
    ~GraphSafePointRequest();

    /** Arm this controller. Returns false once already armed or fulfilled. */
    bool request() noexcept;
    bool requested() const noexcept;
    bool captured() const noexcept;
    bool closed() const noexcept;
    GraphGenerationIdentity expected_generation() const;
    std::optional<GraphSafePoint> safe_point() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool attach(const GraphGenerationIdentity& generation) noexcept;
    bool capture(const GraphGenerationIdentity& generation,
                 Checkpoint checkpoint,
                 const CancelToken* cancellation);
    void close() noexcept;
    void reject() noexcept;

    friend class GraphEngine;
};

}  // namespace neograph::graph
