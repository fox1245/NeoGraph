#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
    bool return_rejected = false;
    bool return_unknown_status = false;
    bool return_rejected_after_completion = false;
    bool duplicate_completion = false;
    bool truncated_result = false;
    bool wrong_result_abi = false;
    bool unsupported_completion_status = false;
    bool invalid_owned_bytes = false;
    bool invalid_null_data = false;
    bool invalid_empty_owned_bytes = false;
    bool oversized_output = false;
    bool throw_from_release = false;
    bool complete_on_cancel = false;
    bool complete_cancelled_on_cancel = true;
    bool pause_cancel = false;

    std::string received_input;
    std::uint64_t received_invocation_id = 0;
    std::uint32_t received_request_struct_size = 0;
    std::uint32_t received_cancellation_struct_size = 0;
    int           initial_cancel_requested = -1;
    int           cancel_view_requested   = -1;
    neograph_program_native_completion_v1 deferred_completion = nullptr;
    void* deferred_completion_userdata = nullptr;
    const neograph_program_native_cancellation_v1* cancellation_view = nullptr;
    std::atomic_size_t output_releases{0};
    std::atomic_size_t completion_calls{0};
    std::atomic_size_t cancel_calls{0};
    std::atomic_size_t destroy_calls{0};
    std::atomic_bool cancel_entered{false};
    std::atomic_bool allow_cancel_completion{true};
};

void release_output(void* userdata, const std::uint8_t* data, std::size_t) {
    auto* state = static_cast<PluginState*>(userdata);
    state->output_releases.fetch_add(1, std::memory_order_relaxed);
    delete[] data;
    if (state->throw_from_release) throw std::runtime_error("plugin release exception");
}

void destroy_plugin(void* userdata) {
    auto* state = static_cast<PluginState*>(userdata);
    state->destroy_calls.fetch_add(1, std::memory_order_relaxed);
}

void emit_completion(PluginState& state,
                     std::string   payload,
                     std::uint32_t status = NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS) {
    ASSERT_NE(state.deferred_completion, nullptr);
    const auto callback          = state.deferred_completion;
    auto* const callback_userdata = state.deferred_completion_userdata;
    state.completion_calls.fetch_add(1, std::memory_order_relaxed);

    neograph_program_native_owned_bytes_v1 owned{};
    if (state.invalid_owned_bytes) {
        owned = {reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(1)),
                 1,
                 &state,
                 nullptr};
    } else if (state.invalid_null_data) {
        owned = {nullptr, 1, &state, release_output};
    } else if (state.invalid_empty_owned_bytes) {
        auto* bytes = new std::uint8_t[0];
        owned       = {bytes, 0, &state, release_output};
    } else if (!state.truncated_result && !state.wrong_result_abi && !payload.empty()) {
        if (state.oversized_output) payload.assign(5000, 'x');
        auto* bytes = new std::uint8_t[payload.size()];
        std::memcpy(bytes, payload.data(), payload.size());
        owned = {bytes, payload.size(), &state, release_output};
    }

    neograph_program_native_result_v1 result{
        state.wrong_result_abi ? 99U : NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        state.truncated_result
            ? static_cast<std::uint32_t>(offsetof(neograph_program_native_result_v1,
                                                  payload_json))
            : static_cast<std::uint32_t>(sizeof(neograph_program_native_result_v1)),
        state.unsupported_completion_status ? 99U : status,
        owned,
    };
    callback(callback_userdata, &result);
}

void complete(PluginState& state, std::string payload) {
    emit_completion(state, std::move(payload));
}

void cancel_plugin(void* userdata, std::uint64_t) {
    auto* state = static_cast<PluginState*>(userdata);
    state->cancel_calls.fetch_add(1, std::memory_order_relaxed);
    if (state->pause_cancel) {
        state->cancel_entered.store(true, std::memory_order_release);
        while (!state->allow_cancel_completion.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    if (state->cancellation_view && state->cancellation_view->is_cancel_requested) {
        state->cancel_view_requested = state->cancellation_view->is_cancel_requested(
            state->cancellation_view->userdata);
    }
    if (state->complete_on_cancel) {
        emit_completion(*state,
                        state->complete_cancelled_on_cancel ? std::string{} : R"({"ok":true})",
                        state->complete_cancelled_on_cancel
                            ? NEOGRAPH_PROGRAM_NATIVE_COMPLETION_CANCELLED
                            : NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS);
    }
}

std::int32_t invoke_plugin(void* userdata,
                           const neograph_program_native_invoke_request_v1* request,
                           neograph_program_native_completion_v1 completion_callback,
                           void* completion_userdata) {
    auto* state = static_cast<PluginState*>(userdata);
    state->received_invocation_id = request->invocation_id;
    state->received_request_struct_size = request->struct_size;
    state->received_cancellation_struct_size =
        request->cancellation ? request->cancellation->struct_size : 0;
    state->cancellation_view = request->cancellation;
    if (request->cancellation && request->cancellation->is_cancel_requested) {
        state->initial_cancel_requested = request->cancellation->is_cancel_requested(
            request->cancellation->userdata);
    }
    state->received_input.assign(reinterpret_cast<const char*>(request->input_json.data),
                                 request->input_json.size);
    state->deferred_completion          = completion_callback;
    state->deferred_completion_userdata = completion_userdata;
    if (state->throw_from_invoke) throw std::runtime_error("plugin exception");
    if (state->return_rejected) return NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED;
    if (state->return_unknown_status) return 99;
    if (state->defer_completion) return NEOGRAPH_PROGRAM_NATIVE_INVOKE_ACCEPTED;
    complete(*state, state->malformed_output ? "{]" : R"({"ok":true})");
    if (state->duplicate_completion) complete(*state, R"({"ok":true})");
    if (state->throw_after_completion) {
        throw std::runtime_error("plugin exception after completion");
    }
    if (state->return_rejected_after_completion) return NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED;
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
        EXPECT_EQ(state.received_request_struct_size,
                  sizeof(neograph_program_native_invoke_request_v1));
        EXPECT_EQ(state.received_cancellation_struct_size,
                  sizeof(neograph_program_native_cancellation_v1));
        EXPECT_EQ(state.initial_cancel_requested, 0);
        EXPECT_EQ(state.output_releases.load(), 1U);
        EXPECT_EQ(state.cancel_calls.load(), 0U);
        EXPECT_EQ(state.destroy_calls.load(), 0U);
    }
    EXPECT_EQ(state.destroy_calls.load(), 1U);
}

TEST(NativeControlBindingTest, CancellationAndDestroyWaitForDeferredCompletion) {
    PluginState state;
    state.defer_completion = true;
    std::optional<NativeInvocation> invocation;
    {
        auto binding = NativeControlBinding::create(raw_binding(state), metadata());
        invocation.emplace(binding.invoke(7, json{{"left", 1}, {"right", 2}}));
    }

    EXPECT_EQ(state.destroy_calls.load(), 0U);
    invocation->cancel();
    EXPECT_TRUE(invocation->cancel_requested());
    EXPECT_EQ(state.cancel_calls.load(), 1U);

    complete(state, R"({"ok":true})");
    ASSERT_TRUE(invocation->wait_for(std::chrono::milliseconds(0)));
    const auto result = invocation->result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.output_releases.load(), 1U);
    EXPECT_EQ(state.destroy_calls.load(), 0U);

    // The binding value is already gone, but the invocation handle keeps the
    // callback lease alive long enough to contain a late duplicate.
    complete(state, R"({"ok":true})");
    EXPECT_EQ(invocation->result()->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.output_releases.load(), 2U);

    invocation.reset();
    EXPECT_EQ(state.destroy_calls.load(), 1U);
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
    EXPECT_EQ(state.output_releases.load(), 1U);
}

TEST(NativeControlBindingTest, RejectedAndUnknownInvokeStatusesBecomeTerminalFailures) {
    PluginState rejected_state;
    rejected_state.return_rejected = true;
    {
        auto binding = NativeControlBinding::create(raw_binding(rejected_state), metadata());
        auto invocation = binding.invoke(11, json{{"left", 1}, {"right", 2}});
        ASSERT_TRUE(invocation.finished());
        const auto result = invocation.result();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, NativeInvocationStatus::ProtocolFailure);
        EXPECT_EQ(result->diagnostic_code, "P_NATIVE_INVOKE_REJECTED");
        invocation.cancel();
        EXPECT_EQ(rejected_state.cancel_calls.load(), 0U);
    }
    EXPECT_EQ(rejected_state.destroy_calls.load(), 1U);

    PluginState unknown_state;
    unknown_state.return_unknown_status = true;
    auto unknown_binding = NativeControlBinding::create(raw_binding(unknown_state), metadata());
    auto unknown_invocation = unknown_binding.invoke(12, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(unknown_invocation.finished());
    const auto unknown_result = unknown_invocation.result();
    ASSERT_TRUE(unknown_result.has_value());
    EXPECT_EQ(unknown_result->status, NativeInvocationStatus::ProtocolFailure);
    EXPECT_EQ(unknown_result->diagnostic_code, "P_NATIVE_INVOKE_STATUS");

    PluginState contract_state;
    contract_state.return_rejected_after_completion = true;
    auto contract_binding = NativeControlBinding::create(raw_binding(contract_state), metadata());
    auto contract_invocation = contract_binding.invoke(27, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(contract_invocation.result().has_value());
    EXPECT_EQ(contract_invocation.result()->status, NativeInvocationStatus::ProtocolFailure);
    EXPECT_EQ(contract_invocation.result()->diagnostic_code, "P_NATIVE_INVOKE_CONTRACT");
    EXPECT_EQ(contract_state.output_releases.load(), 1U);
}

TEST(NativeControlBindingTest, DuplicateCompletionReleasesEveryPayloadAndKeepsFirstResult) {
    PluginState state;
    state.duplicate_completion = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(13, json{{"left", 1}, {"right", 2}});

    ASSERT_TRUE(invocation.finished());
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.completion_calls.load(), 2U);
    EXPECT_EQ(state.output_releases.load(), 2U);
}

TEST(NativeControlBindingTest, CompletionAfterCancelWinsRaceWithoutDoubleCancel) {
    PluginState state;
    state.defer_completion = true;
    state.complete_on_cancel = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(14, json{{"left", 1}, {"right", 2}});

    invocation.cancel();
    invocation.cancel();
    ASSERT_TRUE(invocation.finished());
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::Cancelled);
    EXPECT_EQ(result->diagnostic_code, "P_NATIVE_CANCELLED");
    EXPECT_EQ(state.initial_cancel_requested, 0);
    EXPECT_EQ(state.cancel_view_requested, 1);
    EXPECT_EQ(state.cancel_calls.load(), 1U);
    EXPECT_EQ(state.output_releases.load(), 0U);

    // A plugin that races cancellation with one more completion cannot replace
    // the terminal result, but the duplicate allocation is still reclaimed.
    complete(state, R"({"ok":true})");
    EXPECT_EQ(invocation.result()->status, NativeInvocationStatus::Cancelled);
    EXPECT_EQ(state.output_releases.load(), 1U);
}

TEST(NativeControlBindingTest, ConcurrentCompletionAndCancelHaveOneTerminalWinner) {
    PluginState state;
    state.defer_completion = true;
    state.complete_on_cancel = true;
    state.complete_cancelled_on_cancel = true;
    state.pause_cancel = true;
    state.allow_cancel_completion.store(false, std::memory_order_relaxed);
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(26, json{{"left", 1}, {"right", 2}});

    std::thread cancel_thread([&invocation] { invocation.cancel(); });
    for (int attempt = 0; attempt != 100000 &&
                              !state.cancel_entered.load(std::memory_order_acquire);
         ++attempt) {
        std::this_thread::yield();
    }
    if (!state.cancel_entered.load(std::memory_order_acquire)) {
        state.allow_cancel_completion.store(true, std::memory_order_release);
        cancel_thread.join();
        FAIL() << "cancel callback did not start";
    }

    std::thread completion_thread([&state] { complete(state, R"({"ok":true})"); });
    completion_thread.join();
    state.allow_cancel_completion.store(true, std::memory_order_release);
    cancel_thread.join();

    ASSERT_TRUE(invocation.finished());
    ASSERT_TRUE(invocation.result().has_value());
    EXPECT_EQ(invocation.result()->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.cancel_calls.load(), 1U);
    EXPECT_EQ(state.completion_calls.load(), 2U);
    EXPECT_EQ(state.output_releases.load(), 1U);
}

TEST(NativeControlBindingTest, DroppingPendingHandleCancelsBeforeDestroy) {
    PluginState state;
    state.defer_completion = true;
    state.complete_on_cancel = true;
    {
        auto binding = NativeControlBinding::create(raw_binding(state), metadata());
        auto invocation = binding.invoke(15, json{{"left", 1}, {"right", 2}});
        EXPECT_FALSE(invocation.finished());
    }
    EXPECT_EQ(state.cancel_calls.load(), 1U);
    EXPECT_EQ(state.destroy_calls.load(), 1U);
}

TEST(NativeControlBindingTest, AsyncCompletionUsesPluginAllocatorOnCompletionThread) {
    PluginState state;
    state.defer_completion = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(16, json{{"left", 1}, {"right", 2}});

    std::thread worker([&state] { complete(state, R"({"ok":true})"); });
    ASSERT_TRUE(invocation.wait_for(std::chrono::milliseconds(100)));
    worker.join();
    ASSERT_TRUE(invocation.result().has_value());
    EXPECT_EQ(invocation.result()->status, NativeInvocationStatus::Success);
    EXPECT_EQ(state.output_releases.load(), 1U);
}

TEST(NativeControlBindingTest, TruncatedBindingAndResultAreRejectedBeforeCallbackReads) {
    PluginState state;
    auto raw = raw_binding(state);
    raw.struct_size = static_cast<std::uint32_t>(offsetof(
        neograph_program_native_binding_v1, invoke));
    EXPECT_THROW(NativeControlBinding::create(raw, metadata()), std::invalid_argument);

    raw = raw_binding(state);
    raw.struct_size = static_cast<std::uint32_t>(offsetof(
        neograph_program_native_binding_v1, destroy));
    EXPECT_THROW(NativeControlBinding::create(raw, metadata()), std::invalid_argument);

    state.truncated_result = true;
    auto binding = NativeControlBinding::create(raw_binding(state), metadata());
    auto invocation = binding.invoke(17, json{{"left", 1}, {"right", 2}});
    const auto result = invocation.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, NativeInvocationStatus::ProtocolFailure);
    EXPECT_EQ(result->diagnostic_code, "P_NATIVE_RESULT_ABI");
    EXPECT_EQ(state.output_releases.load(), 0U);
}

TEST(NativeControlBindingTest, MalformedOwnershipBudgetAndReleaseAreContained) {
    struct Fixture {
        bool PluginState::*flag;
        const char* code;
        std::size_t releases;
    };
    const Fixture fixtures[] = {
        {&PluginState::invalid_owned_bytes, "P_NATIVE_OUTPUT_OWNERSHIP", 0},
        {&PluginState::invalid_null_data, "P_NATIVE_OUTPUT_OWNERSHIP", 0},
        {&PluginState::invalid_empty_owned_bytes, "P_NATIVE_OUTPUT_OWNERSHIP", 1},
        {&PluginState::oversized_output, "P_NATIVE_OUTPUT_LIMIT", 1},
        {&PluginState::throw_from_release, "P_NATIVE_RELEASE_EXCEPTION", 1},
    };
    for (const auto& fixture : fixtures) {
        PluginState state;
        state.*(fixture.flag) = true;
        auto binding = NativeControlBinding::create(raw_binding(state), metadata());
        auto invocation = binding.invoke(18 + fixture.releases,
                                         json{{"left", 1}, {"right", 2}});
        const auto result = invocation.result();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->status, NativeInvocationStatus::ProtocolFailure);
        EXPECT_EQ(result->diagnostic_code, fixture.code);
        EXPECT_EQ(state.output_releases.load(), fixture.releases);
    }

    PluginState wrong_abi;
    wrong_abi.wrong_result_abi = true;
    auto wrong_binding = NativeControlBinding::create(raw_binding(wrong_abi), metadata());
    auto wrong_invocation = wrong_binding.invoke(24, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(wrong_invocation.result().has_value());
    EXPECT_EQ(wrong_invocation.result()->diagnostic_code, "P_NATIVE_RESULT_ABI");
    EXPECT_EQ(wrong_abi.output_releases.load(), 0U);

    PluginState unsupported;
    unsupported.unsupported_completion_status = true;
    auto unsupported_binding = NativeControlBinding::create(raw_binding(unsupported), metadata());
    auto unsupported_invocation = unsupported_binding.invoke(
        25, json{{"left", 1}, {"right", 2}});
    ASSERT_TRUE(unsupported_invocation.result().has_value());
    EXPECT_EQ(unsupported_invocation.result()->diagnostic_code, "P_NATIVE_RESULT_STATUS");
    EXPECT_EQ(unsupported.output_releases.load(), 1U);
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
    EXPECT_EQ(state.output_releases.load(), 1U);
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
    EXPECT_EQ(encoded.at("resource_cost").at("max_output_bytes"), 4096U);
    EXPECT_EQ(encoded.at("input_contract").at("schema"), metadata().input_contract.schema);
    EXPECT_EQ(encoded.at("output_contract").at("schema"), metadata().output_contract.schema);
}

TEST(NativeControlBindingTest, RegistryIdentityBindsManifestAndNativeMetadataExactly) {
    PluginState first_state;
    PluginState second_state;
    auto first = std::move(RegistrySnapshotBuilder())
                     .add_native(native_manifest(),
                                 NativeControlBinding::create(raw_binding(first_state), metadata()))
                     .build();
    auto second = std::move(RegistrySnapshotBuilder())
                      .add_native(
                          native_manifest(),
                          NativeControlBinding::create(raw_binding(second_state), metadata()))
                      .build();
    EXPECT_EQ(first.fingerprint(), second.fingerprint());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());

    auto changed_metadata = metadata();
    changed_metadata.resource_cost.max_output_bytes++;
    PluginState changed_metadata_state;
    auto changed_metadata_snapshot = std::move(RegistrySnapshotBuilder())
                                         .add_native(
                                             native_manifest(),
                                             NativeControlBinding::create(
                                                 raw_binding(changed_metadata_state),
                                                 std::move(changed_metadata)))
                                         .build();
    EXPECT_NE(first.fingerprint(), changed_metadata_snapshot.fingerprint());

    auto changed_manifest = native_manifest();
    changed_manifest.identity.implementation_digest = digest('b');
    PluginState changed_manifest_state;
    auto changed_manifest_snapshot = std::move(RegistrySnapshotBuilder())
                                         .add_native(
                                             std::move(changed_manifest),
                                             NativeControlBinding::create(
                                                 raw_binding(changed_manifest_state), metadata()))
                                         .build();
    EXPECT_NE(first.fingerprint(), changed_manifest_snapshot.fingerprint());
    EXPECT_TRUE(first.find_native("native.echo").has_value());
    EXPECT_FALSE(first.find_native("native.missing").has_value());
}

}  // namespace
