// Internal: the one mapping from a classified NeoGraph failure to a gRPC
// status, shared by GraphService, ToolService and the clients.
//
// A failure leaves as a canonical status code with a google.rpc.Status in the
// `grpc-status-details-bin` trailer carrying one google.rpc.ErrorInfo
// (AIP-193): `reason` is an UPPER_SNAKE constant derived from the stable
// NeoGraph code ("HOOK_BLOCKED" for "NG.HookBlocked", "TIMEOUT" for
// "NG.Timeout"; an author code gives its class's reason), `domain` is
// "neograph", and `metadata` holds `code` (the stable NeoGraph code itself),
// `retryable` and the class's fields (a Hook block's invocation_id and state,
// an interrupt's reason). The message is the public message, never what().
// See the reference, "Protocol failure mapping".
//
// Both messages are encoded and decoded here directly on the protobuf wire
// format, so NeoGraph needs no googleapis dependency; any client decodes them
// with the standard google.rpc types.
#pragma once

#ifdef NEOGRAPH_HAVE_GRPC

#include <neograph/graph/types.h>

#include <grpcpp/support/status.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::grpc::detail {

inline constexpr std::string_view kErrorDomain = "neograph";
inline constexpr std::string_view kErrorInfoType = "type.googleapis.com/google.rpc.ErrorInfo";

struct ErrorInfo {
    std::string reason;
    std::string domain;
    std::map<std::string, std::string> metadata;
};

namespace wire {

inline void varint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

inline void bytes(std::string& out, std::uint32_t field, std::string_view value) {
    varint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
    varint(out, value.size());
    out.append(value);
}

// Reads one varint at `pos`; false on a truncated or overlong one.
inline bool read_varint(std::string_view in, std::size_t& pos, std::uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) return false;
        const auto byte = static_cast<unsigned char>(in[pos++]);
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

// Calls `field(number, payload)` for every length-delimited field and skips
// the others; false on malformed input.
template <typename Field>
bool each_field(std::string_view in, Field field) {
    std::size_t pos = 0;
    while (pos < in.size()) {
        std::uint64_t tag = 0;
        if (!read_varint(in, pos, tag)) return false;
        const auto type = tag & 7;
        if (type == 0) {
            std::uint64_t ignored = 0;
            if (!read_varint(in, pos, ignored)) return false;
        } else if (type == 1 || type == 5) {
            const std::size_t width = type == 1 ? 8 : 4;
            if (in.size() - pos < width) return false;
            pos += width;
        } else if (type == 2) {
            std::uint64_t length = 0;
            if (!read_varint(in, pos, length) || in.size() - pos < length) return false;
            field(static_cast<std::uint32_t>(tag >> 3), in.substr(pos, length));
            pos += length;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace wire

/// Serialized google.rpc.Status{code, message, details: [Any{ErrorInfo}]}.
inline std::string status_details(::grpc::StatusCode code, std::string_view message,
                                  const ErrorInfo& info) {
    std::string error_info;
    wire::bytes(error_info, 1, info.reason);
    wire::bytes(error_info, 2, info.domain);
    for (const auto& [key, value] : info.metadata) {
        std::string entry;
        wire::bytes(entry, 1, key);
        wire::bytes(entry, 2, value);
        wire::bytes(error_info, 3, entry);
    }
    std::string any;
    wire::bytes(any, 1, kErrorInfoType);
    wire::bytes(any, 2, error_info);
    std::string status;
    if (code != ::grpc::StatusCode::OK) {
        wire::varint(status, (1u << 3) | 0);
        wire::varint(status, static_cast<std::uint64_t>(code));
    }
    wire::bytes(status, 2, message);
    wire::bytes(status, 3, any);
    return status;
}

/// The ErrorInfo of a status's details, when it carries one.
inline std::optional<ErrorInfo> error_info_of(std::string_view details) {
    std::optional<ErrorInfo> found;
    const bool ok = wire::each_field(details, [&](std::uint32_t number, std::string_view any) {
        if (number != 3 || found) return;
        std::string_view type, value;
        if (!wire::each_field(any, [&](std::uint32_t n, std::string_view payload) {
                if (n == 1) type = payload;
                if (n == 2) value = payload;
            }) || type != kErrorInfoType)
            return;
        ErrorInfo info;
        if (!wire::each_field(value, [&](std::uint32_t n, std::string_view payload) {
                if (n == 1) info.reason = std::string(payload);
                if (n == 2) info.domain = std::string(payload);
                if (n == 3) {
                    std::string key, entry_value;
                    if (wire::each_field(payload, [&](std::uint32_t m, std::string_view part) {
                            if (m == 1) key = std::string(part);
                            if (m == 2) entry_value = std::string(part);
                        }))
                        info.metadata[key] = entry_value;
                }
            }))
            return;
        found = std::move(info);
    });
    return ok ? found : std::nullopt;
}

/// One failure's gRPC form.
struct WireFailure {
    ::grpc::StatusCode code = ::grpc::StatusCode::UNKNOWN;
    std::string message;
    ErrorInfo info;

    ::grpc::Status status() const {
        return ::grpc::Status(code, message, status_details(code, message, info));
    }
};

/// The AIP-193 ErrorInfo reason of a stable NeoGraph code: the code after
/// "NG." in UPPER_SNAKE ("NG.ToolNotFound" -> "TOOL_NOT_FOUND"). Author codes
/// are open-ended, so an author code gives the reason of its class
/// (`failure_class`), and its exact code travels in metadata["code"]. At most
/// 63 characters of [A-Z0-9_], starting with a letter.
std::string error_reason(std::string_view code, graph::NodeFailureClass failure_class);

/// The stable NeoGraph code an ErrorInfo carries: metadata["code"], or, from a
/// server that predates it, a reason that is itself an "NG." code. Empty when
/// it carries none.
std::string error_code_of(const ErrorInfo& info);

/// The canonical status of a failure class (reference, "Protocol failure
/// mapping"). A Hook block whose Hook timed out is DEADLINE_EXCEEDED.
::grpc::StatusCode status_code_of(const graph::FailureVerdict& verdict);

/// The gRPC form of a classified failure: its status code, public message and
/// ErrorInfo (reason: error_reason(); metadata: code, retryable and the run
/// control signal's fields).
WireFailure wire_failure(const graph::FailureVerdict& verdict);

/// A request the service refuses before running anything; `ng_code` is its
/// stable code ("NG.InvalidRequest", "NG.ToolNotFound", ...).
WireFailure request_failure(::grpc::StatusCode code, std::string ng_code, std::string message);

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
