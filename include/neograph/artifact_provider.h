/**
 * @file artifact_provider.h
 * @brief Provider-neutral generated artifacts and long-running operations.
 *
 * Generated media APIs do not fit a text completion: their immediate response
 * may be a final artifact, a stream of progress/artifact events, or a remote
 * operation that must be polled.  This contract keeps those modes explicit,
 * carries only durable artifact references (never an unbounded byte buffer),
 * and centralizes bounded, cancellation-aware LRO polling.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/json.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

/** How a provider delivers an artifact operation's result. */
enum class ArtifactExecutionMode : std::uint8_t {
    OneShot,
    Stream,
    LongRunning,
};

NEOGRAPH_API std::string_view to_string(ArtifactExecutionMode mode) noexcept;

/** Provider-neutral lifecycle state of one generated-artifact operation. */
enum class ArtifactOperationStatus : std::uint8_t {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
};

NEOGRAPH_API std::string_view to_string(ArtifactOperationStatus status) noexcept;

/** A durable reference to generated content. Payload bytes stay at @ref uri. */
struct NEOGRAPH_API Artifact {
    std::string   identity;
    std::string   uri;
    std::string   media_type;
    std::uint64_t size_bytes = 0;
    json          metadata = json::object();

    bool operator==(const Artifact&) const = default;
};

/** Incremental provider notification for @ref ArtifactExecutionMode::Stream. */
enum class ArtifactEventKind : std::uint8_t {
    Progress,
    Artifact,
    State,
};

NEOGRAPH_API std::string_view to_string(ArtifactEventKind kind) noexcept;

struct NEOGRAPH_API ArtifactEvent {
    ArtifactEventKind        kind = ArtifactEventKind::Progress;
    std::string              operation_id;
    std::uint64_t            sequence = 0;
    std::optional<Artifact>  artifact;
    json                     data = json::object();

    bool operator==(const ArtifactEvent&) const = default;
};

/** Polling bound for one long-running operation. `max_attempts` counts polls, not submit. */
struct NEOGRAPH_API ArtifactPollPolicy {
    std::uint32_t             max_attempts = 60;
    std::chrono::milliseconds interval{1000};

    bool operator==(const ArtifactPollPolicy&) const = default;
};

/**
 * Immutable-by-convention input owned by @ref ArtifactProvider::execute_async.
 *
 * `operation` is a provider-defined operation name; `input` is its structured
 * request body.  `idempotency_key` is optional but, when supplied, MUST refer
 * to one logical generation request across retries.  LROs are bounded by
 * @ref poll and share @ref cancel_token with their submit and every poll.
 */
struct NEOGRAPH_API ArtifactRequest {
    std::string                         operation;
    json                                input = json::object();
    std::string                         idempotency_key;
    ArtifactExecutionMode               mode = ArtifactExecutionMode::OneShot;
    ArtifactPollPolicy                  poll;
    std::shared_ptr<graph::CancelToken> cancel_token;
};

/** Final or latest observed state of an artifact operation. */
struct NEOGRAPH_API ArtifactOperation {
    std::string                   id;
    ArtifactOperationStatus       status = ArtifactOperationStatus::Queued;
    std::vector<Artifact>         artifacts;
    json                          output = json::object();
    std::string                   error_code;
    std::string                   error_message;

    [[nodiscard]] bool terminal() const noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
};

using ArtifactEventCallback = std::function<void(const ArtifactEvent&)>;

/** Serialize only durable artifact data; provider transport state is excluded. */
NEOGRAPH_API void to_json(json& value, const Artifact& artifact);
NEOGRAPH_API void from_json(const json& value, Artifact& artifact);

/**
 * One explicit asynchronous artifact-provider contract.
 *
 * Implementations submit through @ref do_start. One-shot and streaming calls
 * MUST return a terminal operation there. Long-running calls may return a
 * non-terminal operation and are then polled by this base class with the
 * request's exact bound. `do_poll` receives the latest operation by value so
 * an adapter may replace opaque provider metadata on every response.
 *
 * New providers implement both protected primitives rather than a mutually
 * recursive sync/async pair. The public synchronous facade is only a bridge
 * to this async contract and propagates the request cancellation token.
 */
class NEOGRAPH_API ArtifactProvider {
  public:
    virtual ~ArtifactProvider();

    ArtifactProvider(const ArtifactProvider&) = delete;
    ArtifactProvider& operator=(const ArtifactProvider&) = delete;
    ArtifactProvider(ArtifactProvider&&) = delete;
    ArtifactProvider& operator=(ArtifactProvider&&) = delete;

    ArtifactOperation execute(ArtifactRequest request,
                              ArtifactEventCallback on_event = {});

    asio::awaitable<ArtifactOperation>
    execute_async(ArtifactRequest request,
                  ArtifactEventCallback on_event = {});

    /** Opaque diagnostic identifier. Never branch on this string for semantics. */
    virtual std::string get_name() const = 0;

  protected:
    ArtifactProvider() = default;

    virtual asio::awaitable<ArtifactOperation>
    do_start(ArtifactRequest request, ArtifactEventCallback on_event) = 0;

    virtual asio::awaitable<ArtifactOperation>
    do_poll(ArtifactOperation operation) = 0;

  private:
    asio::awaitable<ArtifactOperation>
    execute_unbound(ArtifactRequest request, ArtifactEventCallback on_event);
};

} // namespace neograph
