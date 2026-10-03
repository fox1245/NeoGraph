#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::llm {

enum class EndpointFailureKind {
    InvalidConfig, InvalidRequest, Unsupported, Transport, Protocol,
    Provider, Cancelled, DeadlineExceeded, ResourceLimit
};

// Endpoint results are not chat Completions and do not manufacture EndTurn.
// No automatic retry is implied by an HTTP status or an absent response.
struct EndpointFailure {
    EndpointFailureKind kind = EndpointFailureKind::Protocol;
    std::string safe_message;
    int http_status = 0;
    std::string provider_code;
    bool request_may_have_left = false;
};

struct EndpointUsage {
    std::optional<std::uint64_t> input_tokens, output_tokens, total_tokens;
    std::optional<std::uint64_t> cached_input_tokens;
    std::shared_ptr<const json> reported;
};

struct EndpointArtifact {
    std::string mime_type, uri, file_id;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::shared_ptr<const json> metadata;
};

} // namespace neograph::llm
