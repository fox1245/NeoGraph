#include <neograph/hook_rpc.h>

#include "canonical_json.h"

#include <stdexcept>

namespace neograph { namespace {
bool nesting_within(const json& value, unsigned limit, unsigned depth = 0) {
    if (depth > limit) return false;
    if (value.is_array()) for (const auto& child : value) if (!nesting_within(child, limit, depth + 1)) return false;
    if (value.is_object()) for (const auto& [_, child] : value.items()) if (!nesting_within(child, limit, depth + 1)) return false;
    return true;
}

[[noreturn]] void protocol(const char* message) { throw RpcProtocolError(std::string("Hook RPC: ") + message); }

ExternalEffectReceipt effect_from(const json& value) {
    if (!value.is_object()) protocol("external_effect must be an object");
    detail::reject_unknown_fields(value, "Hook RPC external_effect", {"receipt_id", "outcome_known", "succeeded", "detail"});
    return {value.at("receipt_id").get<std::string>(), value.at("outcome_known").get<bool>(),
            value.at("succeeded").get<bool>(), value.at("detail").get<std::string>()};
}

HookExecutionState state_from(std::string_view value) {
    if (value == "succeeded") return HookExecutionState::Succeeded;
    if (value == "failed") return HookExecutionState::Failed;
    if (value == "timed_out") return HookExecutionState::TimedOut;
    if (value == "cancelled") return HookExecutionState::Cancelled;
    if (value == "reconciliation_required") return HookExecutionState::ReconciliationRequired;
    protocol("result has an invalid status");
}

HookExecutionReceipt receipt(const HookInvocation& invocation, std::uint32_t attempt,
                             HookExecutionState state, ExternalEffectReceipt effect = {},
                             std::string error = {}) {
    return HookExecutionReceipt::create({invocation.id(), attempt, state, std::move(effect), std::move(error)});
}

std::vector<ContextArtifact> artifacts_from(const json& value) {
    if (!value.is_array()) protocol("artifacts must be an array");
    std::vector<ContextArtifact> artifacts;
    artifacts.reserve(value.size());
    for (const auto& artifact : value) {
        // ContextArtifact's canonical parser is the allow-list. Artifacts are
        // returned as evidence only; this executor never reads authority fields.
        if (!artifact.is_object()) protocol("artifact must be an object");
        artifacts.push_back(ContextArtifact::parse(detail::canonical_json_bytes(artifact)));
    }
    return artifacts;
}
} // namespace

HookRpcExecutor::HookRpcExecutor(std::shared_ptr<RpcTransport> transport)
    : HookRpcExecutor(std::move(transport), Limits{}) {}

HookRpcExecutor::HookRpcExecutor(std::shared_ptr<RpcTransport> transport, Limits limits)
    : transport_(std::move(transport)), limits_(limits) {
    if (!transport_ || !limits_.max_response_bytes || !limits_.max_json_nesting) {
        throw std::invalid_argument("Hook RPC requires a transport and non-zero limits");
    }
}

asio::awaitable<HookRpcExecution> HookRpcExecutor::execute_async(
    const HookInvocation& invocation, std::uint32_t attempt,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) const {
    if (!attempt || deadline <= std::chrono::steady_clock::now()) {
        co_return HookRpcExecution{receipt(invocation, attempt ? attempt : 1, HookExecutionState::TimedOut, {}, "hook RPC deadline elapsed"), {}};
    }
    if (cancel_token && cancel_token->is_cancelled()) {
        co_return HookRpcExecution{receipt(invocation, attempt, HookExecutionState::Cancelled, {}, "hook RPC cancelled"), {}};
    }
    const auto canonical = invocation.serialize_canonical();
    const auto request = detail::canonical_json_bytes(json{
        {"jsonrpc", "2.0"}, {"id", invocation.id()}, {"method", "hooks/invoke"},
        {"params", {{"invocation_id", invocation.id()}, {"idempotency_key", invocation.id()},
                    {"invocation", detail::parse_json_strict(canonical)}}}});
    std::string raw;
    try {
        raw = co_await transport_->request_async({request, deadline, cancel_token});
    } catch (const std::exception& error) {
        const auto state = cancel_token && cancel_token->is_cancelled() ? HookExecutionState::Cancelled
            : std::chrono::steady_clock::now() >= deadline ? HookExecutionState::TimedOut : HookExecutionState::Failed;
        co_return HookRpcExecution{receipt(invocation, attempt, state, {}, error.what()), {}};
    }
    if (raw.size() > limits_.max_response_bytes) protocol("response exceeds byte limit");
    json response;
    try { response = detail::parse_json_strict(raw); }
    catch (const std::exception&) { protocol("response is not valid JSON"); }
    if (!nesting_within(response, limits_.max_json_nesting)) protocol("response exceeds nesting limit");
    if (!response.is_object() || response.value("jsonrpc", "") != "2.0") protocol("response is not JSON-RPC 2.0");
    detail::reject_unknown_fields(response, "Hook RPC response", {"jsonrpc", "id", "result", "error"});
    if (!response.contains("id") || response["id"] != invocation.id()) protocol("response id does not match invocation id");
    const bool has_result = response.contains("result"), has_error = response.contains("error");
    if (has_result == has_error) protocol("response must contain exactly one of result or error");
    if (has_error) {
        const auto& error = response["error"];
        if (!error.is_object()) protocol("error must be an object");
        detail::reject_unknown_fields(error, "Hook RPC error", {"code", "message", "data"});
        if (!error.contains("code") || !error["code"].is_number_integer() || !error.contains("message") || !error["message"].is_string()) protocol("error has invalid schema");
        co_return HookRpcExecution{receipt(invocation, attempt, HookExecutionState::Failed, {}, error["message"].get<std::string>()), {}};
    }
    const auto& result = response["result"];
    if (!result.is_object()) protocol("result must be an object");
    detail::reject_unknown_fields(result, "Hook RPC result", {"invocation_id", "idempotency_key", "status", "external_effect", "error", "artifacts"});
    if (result.value("invocation_id", "") != invocation.id() || result.value("idempotency_key", "") != invocation.id()) protocol("result identity does not match invocation idempotency key");
    if (!result.contains("status") || !result["status"].is_string()) protocol("result status is required");
    const auto state = state_from(result["status"].get<std::string>());
    const auto effect = result.contains("external_effect") ? effect_from(result["external_effect"]) : ExternalEffectReceipt{};
    if (result.contains("error") && !result["error"].is_string()) protocol("result error must be a string");
    const auto error = result.value("error", std::string{});
    if (state == HookExecutionState::Succeeded && (!effect.outcome_known || !effect.succeeded || effect.receipt_id.empty())) {
        protocol("successful result requires a known successful external effect receipt");
    }
    auto artifacts = result.contains("artifacts") ? artifacts_from(result["artifacts"]) : std::vector<ContextArtifact>{};
    co_return HookRpcExecution{receipt(invocation, attempt, state, effect, error), std::move(artifacts)};
}
} // namespace neograph
