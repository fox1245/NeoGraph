#include <neograph/graph/node.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::program;
using namespace neograph::graph;

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
    std::chrono::milliseconds block_before_return{0};

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
    std::atomic_size_t                             invocation_calls{0};
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
    state->invocation_calls.fetch_add(1, std::memory_order_release);
    if (state->block_before_return.count() > 0)
        std::this_thread::sleep_for(state->block_before_return);
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
    value.resource_declaration   = {4096, 4096, 1000, 1024 * 1024};
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

ExecutableManifest native_manifest(std::string name = "native.echo",
                                   EffectMode  mode = EffectMode::TrustedNative) {
    return {
        {ExecutableKind::Imported, std::move(name), "1.0.0", digest('a')},
        mode,
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
    EXPECT_EQ(encoded.at("resource_declaration").at("max_output_bytes"), 4096U);
    EXPECT_EQ(encoded.at("resource_declaration").at("advisory_wall_time_ms"), 1000U);
    EXPECT_EQ(encoded.at("resource_declaration").at("advisory_memory_bytes"), 1024U * 1024U);
    EXPECT_FALSE(encoded.contains("resource_cost"));
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
    changed_metadata.resource_declaration.max_output_bytes++;
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
TEST(NativeControlBindingTest, RegistrySealsUniqueNonBuiltinImportSlots) {
    PluginState first_state;
    auto        bound = std::move(RegistrySnapshotBuilder())
                     .add_native(42, native_manifest(),
                                 NativeControlBinding::create(raw_binding(first_state), metadata()))
                     .build();
    const auto encoded = bound.manifest().at("entries").front().at("metadata");
    EXPECT_EQ(encoded.at("import_slot"), 42U);

    PluginState moved_state;
    auto        moved = std::move(RegistrySnapshotBuilder())
                     .add_native(43, native_manifest(),
                                 NativeControlBinding::create(raw_binding(moved_state), metadata()))
                     .build();
    EXPECT_NE(bound.fingerprint(), moved.fingerprint());

    PluginState reserved_state;
    EXPECT_THROW(
        std::move(RegistrySnapshotBuilder())
            .add_native(NATIVE_CONTROL_IMPORT_SLOT_MIN - 1, native_manifest(),
                        NativeControlBinding::create(raw_binding(reserved_state), metadata())),
        std::invalid_argument);

    PluginState duplicate_first;
    PluginState duplicate_second;
    auto        second_manifest                    = native_manifest("native.second");
    second_manifest.identity.implementation_digest = digest('b');
    EXPECT_THROW(
        std::move(RegistrySnapshotBuilder())
            .add_native(42, native_manifest(),
                        NativeControlBinding::create(raw_binding(duplicate_first), metadata()))
            .add_native(42, std::move(second_manifest),
                        NativeControlBinding::create(raw_binding(duplicate_second), metadata()))
            .build(),
        std::invalid_argument);
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)

class NativeRuntimeNode final : public GraphNode {
public:
    explicit NativeRuntimeNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"value", true});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class BlockAfterNativeResultStore final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view owner,
                                         std::string_view run_id) const override {
        return inner_.load(owner, run_id);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner,
                                               std::string_view run_id) const override {
        return inner_.latest(owner, run_id);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner,
                                          std::string_view run_id,
                                          std::uint64_t    sequence) const override {
        return inner_.load_events(owner, run_id, sequence);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner,
                                                       std::string_view run_id,
                                                       std::uint64_t    sequence) const override {
        return inner_.load_effects(owner, run_id, sequence);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run_id, std::uint64_t sequence) const override {
        return inner_.load_javascript_commands(owner, run_id, sequence);
    }
    ProgramTransitionPublishResult compare_publish(
        std::string_view             owner,
        std::string_view             expected,
        ProgramTransitionPublication publication) override {
        const bool native_result =
            !publication.commands.empty() && publication.commands.back().completed() &&
            publication.commands.back().command().kind() == JavaScriptCommandKind::HostCapability;
        const auto published = inner_.compare_publish(owner, expected, std::move(publication));
        if (!native_result || published != ProgramTransitionPublishResult::Published)
            return published;
        std::unique_lock lock(mutex_);
        observed_ = true;
        condition_.notify_all();
        condition_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
        return published;
    }

    bool wait_for_result(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return observed_; });
    }

    void release_result() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

private:
    InMemoryProgramTransitionStore inner_;
    mutable std::mutex             mutex_;
    std::condition_variable        condition_;
    bool                           observed_ = false;
    bool                           released_ = false;
};

RegistrySnapshot native_runtime_registry(PluginState&          state,
                                         NativeControlMetadata native_metadata = metadata(),
                                         EffectMode native_mode = EffectMode::TrustedNative) {
    const auto              native_identity = native_manifest().identity;
    RegistrySnapshotBuilder builder;
    builder.add_node(
        ExecutableManifest{{ExecutableKind::Node, "native-runtime-node", "1.0.0", digest('c')},
                           EffectMode::Brokered,
                           "native-runtime-node-attestation",
                           {},
                           {},
                           {},
                           ExecutionGuarantee::Strict},
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<NativeRuntimeNode>(name);
        },
        json{{"type", "object"}}, json::object(),
        [native_identity](const json&) {
            return std::vector<ExecutableIdentity>{native_identity};
        });
    builder.add_reducer(ExecutableManifest{{ExecutableKind::Reducer, "native-runtime-overwrite",
                                            "1.0.0", digest('d')},
                                           EffectMode::Brokered,
                                           "native-runtime-reducer-attestation",
                                           {},
                                           {},
                                           {},
                                           ExecutionGuarantee::Strict},
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_native(
        42, native_manifest("native.echo", native_mode),
        NativeControlBinding::create(raw_binding(state), std::move(native_metadata)));
    return std::move(builder).build();
}

AdmissionProfile native_runtime_profile(const RegistrySnapshot& registry,
                                        AdmissionMode mode = AdmissionMode::TrustedEmbedding) {
    AdmissionProfileBuilder builder;
    builder.id("native-runtime-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(mode)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    if (mode == AdmissionMode::TrustedEmbedding)
        builder.allow_effect_mode(EffectMode::TrustedNative);
    for (const auto& identity : registry.identities())
        builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot native_runtime_policy(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("native-runtime-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:native-runtime")
        .admission_profile(profile)
        .allow_capability("native.execute")
        .allow_effect("native.effect")
        .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 2, 32, 100, 1, 0, 0})
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    if (profile.mode() == AdmissionMode::TrustedEmbedding)
        builder.allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY));
    return std::move(builder).build();
}

RunBudget native_runtime_budget() {
    return RunBudget{10000, 1000, 1000, 2, 8, 20, 0, 0, 0};
}

std::string native_runtime_source(std::uint32_t slot) {
    return "export function define() {\n"
           "  const graph = ng.graph(\"main\");\n"
           "  graph.channel(\"value\", {reducer: \"native-runtime-overwrite\", initial: false});\n"
           "  graph.node(\"work\", {type: \"native-runtime-node\"});\n"
           "  graph.entry(\"work\");\n"
           "  graph.exit(\"work\");\n"
           "  return graph;\n"
           "}\n\n"
           "export function* main(input) {\n"
           "  return yield ng.hostCapability(" +
           std::to_string(slot) +
           ", {left: input.left, right: input.right}, \"native:1\");\n"
           "}\n";
}

struct NativeAdmittedRuntime {
    PluginState                             state;
    RegistrySnapshot                        registry;
    AdmissionProfile                        profile;
    PolicySnapshot                          policy;
    std::shared_ptr<InMemoryProgramStore>   store;
    std::shared_ptr<EngineGenerationCache>  engines;
    std::shared_ptr<ProgramCatalog>         catalog;
    std::shared_ptr<CheckpointStore>        checkpoints;
    std::shared_ptr<ProgramTransitionStore> transitions;
    std::unique_ptr<ProgramRuntime>         runtime;

    explicit NativeAdmittedRuntime(std::shared_ptr<ProgramTransitionStore> journal = {},
                                   NativeControlMetadata native_metadata           = metadata(),
                                   EffectMode            native_mode = EffectMode::TrustedNative)
        : registry(native_runtime_registry(state, std::move(native_metadata), native_mode)),
          profile(native_runtime_profile(registry,
                                         native_mode == EffectMode::TrustedNative
                                             ? AdmissionMode::TrustedEmbedding
                                             : AdmissionMode::MultiTenant)),
          policy(native_runtime_policy(profile)),
          store(std::make_shared<InMemoryProgramStore>()),
          engines(std::make_shared<EngineGenerationCache>()),
          catalog(std::make_shared<ProgramCatalog>(CatalogConfig{
              store,
              registry,
              engines,
              "native-runtime-test/v1",
              [](const std::vector<ExecutableIdentity>& requested) {
                  CatalogCapabilityBinding binding;
                  for (const auto& executable : requested)
                      binding.receipts.push_back(CapabilityBindingReceipt{executable, digest('e')});
                  return binding;
              },
              1,
              {},
              "native-test-attestation"})),
          checkpoints(std::make_shared<InMemoryCheckpointStore>()),
          transitions(journal ? std::move(journal)
                              : std::make_shared<InMemoryProgramTransitionStore>()),
          runtime(make_runtime()) {}

    std::unique_ptr<ProgramRuntime> make_runtime() const {
        RuntimeConfig config;
        config.catalog           = catalog;
        config.checkpoints       = checkpoints;
        config.transitions       = transitions;
        config.scheduler_threads = 1;
        return std::make_unique<ProgramRuntime>(std::move(config));
    }

    ProgramVersion admit(std::uint32_t slot = 42, RunBudget budget = native_runtime_budget()) {
        ProgramCompiler compiler(registry, {"native-runtime-test/v1"});
        const auto      source =
            ProgramSource::from_javascript("test:native-runtime.js", native_runtime_source(slot));
        std::optional<ProgramBundle> bundle;
        try {
            bundle = compiler.compile(source, budget, ContractRecord{1, json{{"type", "object"}}},
                                      ContractRecord{1, json{{"type", "object"}}});
        } catch (const ProgramCompileError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
        return catalog->admit(*bundle,
                              ProgramAdmission{"tenant:native-runtime", profile, policy, {}});
    }
};

bool wait_for_native_calls(const PluginState&        state,
                           std::size_t               expected,
                           std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (state.invocation_calls.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return state.invocation_calls.load(std::memory_order_acquire) == expected;
}

TEST(NativeProgramRuntimeTest, AdmittedHostCapabilityRoundTripsAndReleasesOutput) {
    NativeAdmittedRuntime fixture;
    const auto            version = fixture.admit();
    const auto            result  = fixture.runtime->run("tenant:native-runtime", version,
                                                         ProgramInvocation{json{{"left", 3}, {"right", 4}},
                                                               native_runtime_budget(),
                                                               "trace-native-roundtrip",
                                                                           {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(result.output(), (json{{"ok", true}}));
    EXPECT_EQ(fixture.state.received_input, R"({"left":3,"right":4})");
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);
    EXPECT_EQ(fixture.state.output_releases.load(), 1U);
    const auto commands =
        fixture.transitions->load_javascript_commands("tenant:native-runtime", result.run_id());
    ASSERT_EQ(commands.size(), 2U);
    EXPECT_TRUE(commands.front().pending());
    EXPECT_TRUE(commands.back().completed());
    EXPECT_EQ(commands.front().coordinate_id(), commands.back().coordinate_id());
}

TEST(NativeProgramRuntimeTest, GrantedDeadlineIncludesBlockingInvokeCallback) {
    NativeAdmittedRuntime fixture;
    fixture.state.block_before_return = std::chrono::milliseconds(200);
    auto short_budget                 = native_runtime_budget();
    short_budget.wall_time_ms         = 100;
    const auto version                = fixture.admit(42, short_budget);

    const auto started = std::chrono::steady_clock::now();
    const auto result  = fixture.runtime->run(
        "tenant:native-runtime", version,
        ProgramInvocation{
            json{{"left", 3}, {"right", 4}}, short_budget, "trace-native-blocking-dispatch", {}});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_GE(elapsed, std::chrono::milliseconds(200));
    ASSERT_EQ(result.status(), ProgramTerminalStatus::TimedOut)
        << (result.failure() ? result.failure()->code + ": " + result.failure()->message + " " +
                                   result.failure()->witness.dump()
                             : "no failure detail");
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RUNTIME_TIMEOUT");
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);
    EXPECT_EQ(fixture.state.output_releases.load(), 1U);
}

TEST(NativeProgramRuntimeTest, MultiTenantAdmissionRejectsUncontainedNativeMemoryClaim) {
    auto uncontained                                       = metadata();
    uncontained.resource_declaration.advisory_memory_bytes = 4096;
    NativeAdmittedRuntime fixture({}, std::move(uncontained), EffectMode::Brokered);
    fixture.state.block_before_return = std::chrono::seconds(5);

    try {
        (void)fixture.admit();
        FAIL() << "brokered in-process native binding was admitted";
    } catch (const ProgramAdmissionError& error) {
        const auto found = std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
                                       [](const Diagnostic& diagnostic) {
                                           return diagnostic.code == "P_ADMIT_NATIVE_BOUNDARY";
                                       });
        EXPECT_TRUE(found);
    }
    EXPECT_EQ(fixture.state.invocation_calls.load(), 0U);
}

TEST(NativeProgramRuntimeTest, InvalidSlotFailsBeforeNativeCallbackOrJournalPublication) {
    NativeAdmittedRuntime fixture;
    const auto            version = fixture.admit(43);
    const auto            result  = fixture.runtime->run("tenant:native-runtime", version,
                                                         ProgramInvocation{json{{"left", 3}, {"right", 4}},
                                                               native_runtime_budget(),
                                                               "trace-native-invalid-slot",
                                                                           {}});

    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_JS_HOST_CAPABILITY_BINDING");
    EXPECT_EQ(fixture.state.invocation_calls.load(), 0U);
    EXPECT_TRUE(
        fixture.transitions->load_javascript_commands("tenant:native-runtime", result.run_id())
            .empty());
}

TEST(NativeProgramRuntimeTest, CancellationCancelsAcceptedNativeInvocationSafely) {
    NativeAdmittedRuntime fixture;
    fixture.state.defer_completion   = true;
    fixture.state.complete_on_cancel = true;
    const auto version               = fixture.admit();
    auto       handle                = fixture.runtime->start(
        "tenant:native-runtime", version,
        ProgramInvocation{
            json{{"left", 3}, {"right", 4}}, native_runtime_budget(), "trace-native-cancel", {}});
    ASSERT_TRUE(wait_for_native_calls(fixture.state, 1));

    EXPECT_TRUE(handle.cancel());
    const auto result = handle.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(fixture.state.cancel_calls.load(), 1U);
    EXPECT_EQ(fixture.state.completion_calls.load(), 1U);
}

TEST(NativeProgramRuntimeTest, CompletedCommandReplaysWithoutNativeReinvocation) {
    auto                  journal = std::make_shared<BlockAfterNativeResultStore>();
    NativeAdmittedRuntime fixture(journal);
    const auto            version  = fixture.admit();
    auto                  original = fixture.runtime->start("tenant:native-runtime", version,
                                                            ProgramInvocation{json{{"left", 3}, {"right", 4}},
                                                             native_runtime_budget(),
                                                             "trace-native-completed-crash",
                                                                              {}});
    ASSERT_TRUE(journal->wait_for_result(std::chrono::seconds(2)));
    ASSERT_EQ(fixture.state.invocation_calls.load(), 1U);

    auto       fresh_runtime = fixture.make_runtime();
    const auto replayed =
        fresh_runtime->reconnect("tenant:native-runtime", original.run_id()).wait();
    ASSERT_EQ(replayed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(replayed.output(), (json{{"ok", true}}));
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);

    journal->release_result();
    (void)original.wait();
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);
}

TEST(NativeProgramRuntimeTest, PendingCrashRecoveryRequiresReconciliationAndNeverRedispatches) {
    NativeAdmittedRuntime fixture;
    fixture.state.defer_completion   = true;
    fixture.state.complete_on_cancel = true;
    const auto version               = fixture.admit();
    auto       original              = fixture.runtime->start("tenant:native-runtime", version,
                                                              ProgramInvocation{json{{"left", 3}, {"right", 4}},
                                                             native_runtime_budget(),
                                                             "trace-native-pending-crash",
                                                                                {}});
    ASSERT_TRUE(wait_for_native_calls(fixture.state, 1));
    const auto pending =
        fixture.transitions->load_javascript_commands("tenant:native-runtime", original.run_id());
    ASSERT_EQ(pending.size(), 1U);
    ASSERT_TRUE(pending.front().pending());

    auto       fresh_runtime = fixture.make_runtime();
    const auto recovered =
        fresh_runtime->reconnect("tenant:native-runtime", original.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Interrupted);
    ASSERT_TRUE(recovered.interrupt().has_value());
    ASSERT_TRUE(recovered.interrupt()->pending_effect.has_value());
    EXPECT_EQ(recovered.interrupt()->pending_effect->idempotency(),
              ProgramEffectIdempotency::NonIdempotent);
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);

    EXPECT_TRUE(original.cancel());
    (void)original.wait();
    EXPECT_EQ(fixture.state.invocation_calls.load(), 1U);
}

#endif

}  // namespace
