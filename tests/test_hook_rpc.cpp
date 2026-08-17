#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/hook_rpc.h>

using namespace neograph;

namespace {
class TranscriptTransport final : public RpcTransport {
public:
    explicit TranscriptTransport(std::string response) : response_(std::move(response)) {}
    asio::awaitable<std::string> request_async(RpcRequest request) override {
        last_request = std::move(request);
        co_return response_;
    }
    RpcRequest last_request;
private:
    std::string response_;
};

HookInvocation invocation() {
    return HookInvocation::create({{}, "sha256:" + std::string(64, '1'), "sha256:" + std::string(64, '2'),
        "audit", HookPhase::BeforeToolExecution, HookDelivery::BlockingMandatory,
        HookFailureMode::FailClosed, HookIdempotency::Idempotent, ToolEffectClass::ReadOnly,
        {}, {}, json{{"subject", "report"}}});
}

std::string success(const HookInvocation& value) {
    return json{{"jsonrpc", "2.0"}, {"id", value.id()}, {"result", {
        {"invocation_id", value.id()}, {"idempotency_key", value.id()}, {"status", "succeeded"},
        {"external_effect", {{"receipt_id", "remote-receipt"}, {"outcome_known", true},
                             {"succeeded", true}, {"detail", "completed"}}}}}}.dump();
}

HookRpcExecution execute(HookRpcExecutor& executor, const HookInvocation& value) {
    return async::run_sync(executor.execute_async(value, 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}
} // namespace

TEST(HookRpcExecutor, SendsTheFixedEnvelopeAndMapsKnownSuccess) {
    const auto value = invocation();
    auto transport = std::make_shared<TranscriptTransport>(success(value));
    HookRpcExecutor executor(transport);
    const auto execution = execute(executor, value);
    EXPECT_EQ(execution.receipt.data().state, HookExecutionState::Succeeded);
    EXPECT_EQ(execution.receipt.data().external_effect.receipt_id, "remote-receipt");
    const auto request = json::parse(transport->last_request.request);
    EXPECT_EQ(request["jsonrpc"], "2.0");
    EXPECT_EQ(request["id"], value.id());
    EXPECT_EQ(request["method"], "hooks/invoke");
    EXPECT_EQ(request["params"]["invocation_id"], value.id());
    EXPECT_EQ(request["params"]["idempotency_key"], value.id());
}

TEST(HookRpcExecutor, RejectsWrongIdsResultErrorAndOversizedResponses) {
    const auto value = invocation();
    auto bad_id = json::parse(success(value)); bad_id["id"] = "other";
    EXPECT_THROW(execute(*std::make_unique<HookRpcExecutor>(std::make_shared<TranscriptTransport>(bad_id.dump())), value), RpcProtocolError);
    auto duplicate = json::parse(success(value)); duplicate["error"] = {{"code", -1}, {"message", "no"}};
    EXPECT_THROW(execute(*std::make_unique<HookRpcExecutor>(std::make_shared<TranscriptTransport>(duplicate.dump())), value), RpcProtocolError);
    HookRpcExecutor::Limits limits; limits.max_response_bytes = 8;
    HookRpcExecutor oversized(std::make_shared<TranscriptTransport>(success(value)), limits);
    EXPECT_THROW(execute(oversized, value), RpcProtocolError);
}

TEST(HookRpcExecutor, MapsDeadlineCancellationAndSchemaErrors) {
    const auto value = invocation();
    auto transport = std::make_shared<TranscriptTransport>(success(value));
    HookRpcExecutor executor(transport);
    const auto timed_out = async::run_sync(executor.execute_async(value, 1, std::chrono::steady_clock::now()));
    EXPECT_EQ(timed_out.receipt.data().state, HookExecutionState::TimedOut);
    auto cancelled = std::make_shared<graph::CancelToken>(); cancelled->cancel();
    const auto cancelled_result = async::run_sync(executor.execute_async(value, 1,
        std::chrono::steady_clock::now() + std::chrono::seconds(1), cancelled));
    EXPECT_EQ(cancelled_result.receipt.data().state, HookExecutionState::Cancelled);
    auto schema = json::parse(success(value)); schema["result"] = {{"invocation_id", value.id()},
        {"idempotency_key", value.id()}};
    HookRpcExecutor malformed(std::make_shared<TranscriptTransport>(schema.dump()));
    EXPECT_THROW(execute(malformed, value), RpcProtocolError);
}

// Both concrete optional transports adapt their wire replies to this identical
// raw transcript contract; keep protocol rejection independent of the carrier.
class HookRpcTranscriptParity : public ::testing::TestWithParam<const char*> {};

TEST_P(HookRpcTranscriptParity, RejectsTheSameMalformedTranscript) {
    const auto value = invocation();
    auto response = json::parse(success(value));
    response["id"] = std::string(GetParam()) + "-wrong-id";
    HookRpcExecutor executor(std::make_shared<TranscriptTransport>(response.dump()));
    EXPECT_THROW(execute(executor, value), RpcProtocolError);
}

INSTANTIATE_TEST_SUITE_P(StdioAndHttp, HookRpcTranscriptParity,
    ::testing::Values("stdio", "http"));
