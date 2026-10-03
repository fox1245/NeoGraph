#pragma once

#include <neograph/llm/endpoint_types.h>
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph { class CancelToken; }
namespace neograph::llm {

namespace veo_detail { struct Client; struct Operation; }

enum class VeoAspectRatio { Landscape, Portrait };
enum class VeoPersonGeneration { AllowAll, AllowAdult, DontAllow };
enum class VeoResolution { P720, P1080, P4K };
enum class VeoStatus { Pending, Succeeded, Failed, Cancelled, DeadlineExceeded };

struct VeoImage {
    std::string mime_type;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

struct VeoRequest {
    std::string prompt;
    std::optional<std::string> model;
    std::optional<VeoImage> image, last_frame;
    std::vector<VeoImage> reference_images;
    // Extension is explicitly unsupported by this client, rather than silently dropped.
    std::optional<EndpointArtifact> extension_video;
    std::optional<VeoAspectRatio> aspect_ratio;
    std::optional<VeoPersonGeneration> person_generation;
    std::optional<unsigned> duration_seconds;
    std::optional<VeoResolution> resolution;
    unsigned number_of_videos = 1;
    std::shared_ptr<graph::CancelToken> cancel_token;
};

// Copying a handle preserves the same operation; no constructor accepts provider JSON.
// Its issuing client's immutable origin/model and original deadline remain authoritative.
class NEOGRAPH_API VeoOperation {
public:
    VeoOperation(const VeoOperation&) = default;
    VeoOperation(VeoOperation&&) noexcept = default;
    VeoOperation& operator=(const VeoOperation&) = default;
    VeoOperation& operator=(VeoOperation&&) noexcept = default;
    std::string name() const;
    std::string model() const;
    std::chrono::steady_clock::time_point deadline() const noexcept;
private:
    explicit VeoOperation(std::shared_ptr<veo_detail::Operation> state);
    std::shared_ptr<veo_detail::Operation> state_;
    friend class VeoClient;
};

struct VeoSubmitResult {
    std::optional<VeoOperation> operation;
    std::optional<EndpointFailure> failure;
};
struct VeoResult {
    VeoStatus status = VeoStatus::Pending;
    std::vector<EndpointArtifact> artifacts;
    EndpointUsage usage;
    std::optional<EndpointFailure> failure;
    std::uint64_t generation_dispatches = 0, status_queries = 0, downloads = 0;
};
struct VeoDownloadResult {
    std::optional<EndpointArtifact> artifact;
    std::optional<EndpointFailure> failure;
};

class NEOGRAPH_API VeoClient {
public:
    // Closed, versioned operational JSON; the API key is never part of it.
    // Throws invalid_argument for inadmissible config/credentials, without echoing them.
    VeoClient(const json& config, std::string api_key);
    asio::awaitable<VeoSubmitResult> submit(
        asio::any_io_executor ex, VeoRequest request,
        std::optional<std::chrono::steady_clock::time_point> deadline = {}) const;
    // Server-verified resume, restricted to admitted model operation names.
    // Lookup dispatches one status GET, never a generation or editable JSON poll.
    asio::awaitable<VeoSubmitResult> lookup(
        asio::any_io_executor ex, std::string operation_name,
        std::optional<std::chrono::steady_clock::time_point> deadline = {},
        std::shared_ptr<graph::CancelToken> cancellation = {}) const;
    // One active lifecycle call per operation. Concurrent calls fail without dispatch.
    asio::awaitable<VeoResult> poll(VeoOperation operation) const;
    asio::awaitable<VeoResult> wait(VeoOperation operation) const;
    // Effective local cancellation; an already dispatched provider generation/charge
    // cannot be undone. Cancels sockets and wait timers, and forbids subsequent GETs.
    void cancel(const VeoOperation& operation) const noexcept;
    // Downloads only a video admitted by this operation, never caller-provided URI JSON.
    asio::awaitable<VeoDownloadResult> download(VeoOperation operation,
                                               std::size_t index = 0) const;
private:
    std::shared_ptr<const veo_detail::Client> client_;
};

} // namespace neograph::llm
