#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::program;

std::string digest(char character) {
    return "sha256:" + std::string(64, character);
}

struct PluginState {
    bool defer_completion = false;
    bool malformed_output = false;
    bool throw_from_invoke = false;
    bool throw_after_completion = false;

    std::string received_input;
    std::uint64_t received_invocation_id = 0;
    neograph_program_native_completion_v1 deferred_completion = nullptr;
    void* deferred_completion_userdata = nullptr;
    std::size_t output_releases = 0;
    std::size_t cancel_calls = 0;
    std::size_t destroy_calls = 0;
};

void release_output(void* userdata, const std::uint8_t* data, std::size_t) {
    auto* state = static_cast<PluginState*>(userdata);
    ++state->output_releases;
    delete[] data;
}

void destroy_plugin(void* userdata) {
    auto* state = static_cast<PluginState*>(userdata);
    ++state->destroy_calls;
}

void cancel_plugin(void* userdata, std::uint64_t) {
    auto* state = static_cast<PluginState*>(userdata);
    ++state->cancel_calls;
}

void complete(PluginState& state, std::string payload) {
    ASSERT_NE(state.deferred_completion, nullptr);
    auto* bytes = new std::uint8_t[payload.size()];
    std::memcpy(bytes, payload.data(), payload.size());
    const neograph_program_native_owned_bytes_v1 owned{
        bytes,
        payload.size(),
        &state,
        release_output,
    };
    const neograph_program_native_result_v1 result{
        NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        sizeof(neograph_program_native_result_v1),
        NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS,
        owned,
    };
    const auto callback = std::exchange(state.deferred_completion, nullptr);
    auto* const callback_userdata = std::exchange(state.deferred_completion_userdata, nullptr);
    callback(callback_userdata, &result);
}

std::int32_t invoke_plugin(void* userdata,
                           const neograph_program_native_invoke_request_v1* request,
                           neograph_program_native_completion_v1 completion_callback,
                           void* completion_userdata) {
    auto* state = static_cast<PluginState*>(userdata);
    if (state->throw_from_invoke) throw std::runtime_error("plugin exception");

    state->received_invocation_id = request->invocation_id;
    state->received_input.assign(reinterpret_cast<const char*>(request->input_json.data),
                                 request->input_json.size);
    state->deferred_completion          = completion_callback;
    state->deferred_completion_userdata = completion_userdata;
    if (state->defer_completion) return NEOGRAPH_PROGRAM_NATIVE_INVOKE_ACCEPTED;
    complete(*state, state->malformed_output ? "{]" : R"({"ok":true})");
    if (state->throw_after_completion) throw std::runtime_error("plugin exception after completion");
    return NEOGRAPH_PROGRAM_NATIVE_INVOKE_ACCEPTED;
}

NativeControlMetadata metadata() {
    NativeControlMetadata value;
    value.input_contract.schema = {
        {"type", "object"},
        {"required", json::array({"left", "right"})},
        {"properties",
         {{"left", {{"type", "integer"}}}, {"right", {{"type", "integer"}}}}},
        {"additionalProperties", false},
    };
    value.output_contract.schema = {
        {"type", "object"},
        {"required", json::array({"ok"})},
        {"properties", {{"ok", {{"type", "boolean"}}}}},
        {"additionalProperties", false},
    };
    value.idempotency            = NativeIdempotency::Idempotent;
    value.replay_behavior        = NativeReplayBehavior::Deterministic;
    value.resource_cost          = {4096, 4096, 1000, 1024 * 1024};
    return value;
}

neograph_program_native_binding_v1 raw_binding(PluginState& state) {
    return {
        NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        sizeof(neograph_program_native_binding_v1),
        &state,
        invoke_plugin,
        cancel_plugin,
        destroy_plugin,
    };
}

ExecutableManifest native_manifest(std::string name = "native.echo") {
    return {
        {ExecutableKind::Imported, std::move(name), "1.0.0", digest('a')},
        EffectMode::Brokered,
        "native-test-attestation",
        {"native.execute"},
        {"native.effect"},
        {},
    };
}

TEST(NativeControlBindingTest, CanonicalizesInputValidatesOutputAndReleasesPluginBytes) {
    PluginState state;
    {
        auto binding = NativeControlBinding::create(raw_binding(state), metadata());
        auto invocation = binding.invoke(41, json{{"right", 2}, {"left", 1}});

        ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(0)));
        const auto result = invocation.result();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, NativeInvocationStatus::Success);
        EXPECT_EQ(result->value, json({{"ok", true}}));
        EXPECT_EQ(state.received_invocation_id, 41U);
        EXPECT_EQ(state.received_input, R"({"left":1,"right":2})");
        EXPECT_EQ(state.output_releases, 1U);
        EXPECT_EQ(state.cancel_calls, 0U);
        EXPECT_EQ(state.destroy_calls, 0U);
    }
    EXPECT_EQ(state.destroy_calls, 1U);
}

TEST(NativeControlBindingTest, CancellationAndDestroyWaitForDeferredCompletion) {
    PluginState state;
    state.defer_completion = true;
    std::optional<NativeInvocation> invocation;
    {
        auto binding = NativeControlBinding::create(raw_binding(state), metadata());
        invocation.emplace(binding.invoke(7, json{{"left", 1}, {"right", 2}}));
    }

    EXPECT_EQ(state.destroy_calls, 0U);
    invocation->cancel();
    EXPECT_TRUE(invocation->cancel_requested());
    EXPECT_EQ(state.cancel_calls, 1U);

    complete(state, R"({"ok":true})");
    ASSERT_TRUE(invocation->wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.output_releases, 1U);
    EXPECT_EQ(state.destroy_calls, 0U);

    invocation.reset();
    EXPECT_EQ(state.destroy_calls, 1U);
}

TEST(NativeControlBindingTest, MalformedPluginOutputFailsClosedAndIsReleased) {
    PluginState state;
    state.malformed_output = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(8, json{{"left", 1}, {"right", 2}});

    ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::ProtocolFailure);
    EXPECT_EQ(result->diagnostic_code, "P_NATIVE_OUTPUT_JSON");
    EXPECT_EQ(state.output_releases, 1U);
}

TEST(NativeControlBindingTest, CppExceptionFromNonconformingPluginCannotEscapeHostBoundary) {
    PluginState state;
    state.throw_from_invoke = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());

    auto invocation = binding.invoke(9, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::ProtocolFailure);
    EXPECT_EQ(result->diagnostic_code, "P_NATIVE_INVOKE_EXCEPTION");
}


TEST(NativeControlBindingTest, SynchronousCompletionKeepsLeaseAliveUntilInvokeReturns) {
    PluginState state;
    state.throw_after_completion = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());

    auto invocation = binding.invoke(10, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.output_releases, 1U);
}
TEST(NativeControlBindingTest, RegistersThroughTheExistingImportedExecutableRegistry) {
    PluginState state;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto snapshot = std::move(RegistrySnapshotBuilder())
                        .add_native(native_manifest(), binding)
                        .build();

    const auto manifest = snapshot.find(ExecutableKind::Imported, "native.echo");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->identity.implementation_digest, digest('a'));
    const auto native = snapshot.find_native("native.echo");
    ASSERT_TRUE(native.has_value());

    auto invocation = native->invoke(10, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Success);

    const auto snapshot_manifest = snapshot.manifest();
    ASSERT_EQ(snapshot_manifest.at("entries").size(), 1U);
    const auto& encoded = snapshot_manifest.at("entries").front().at("metadata");
    EXPECT_EQ(encoded.at("native_abi_version"), NEOGRAPH_PROGRAM_NATIVE_ABI_V1);
    EXPECT_EQ(encoded.at("idempotency"), "idempotent");
    EXPECT_EQ(encoded.at("replay_behavior"), "deterministic");
}

}  // namespace
