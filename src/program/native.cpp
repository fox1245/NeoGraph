#include <neograph/program/native.h>

#include "canonical_json.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::size_t kMinimumBindingSize =
    offsetof(neograph_program_native_binding_v1, destroy) +
    sizeof(((neograph_program_native_binding_v1*)nullptr)->destroy);
constexpr std::size_t kMinimumCancellationSize =
    offsetof(neograph_program_native_cancellation_v1, is_cancel_requested) +
    sizeof(((neograph_program_native_cancellation_v1*)nullptr)->is_cancel_requested);
constexpr std::size_t kMinimumResultSize =
    offsetof(neograph_program_native_result_v1, payload_json) +
    sizeof(neograph_program_native_owned_bytes_v1);

std::string bounded_message(const std::exception& error) {
    constexpr std::size_t kMaxMessageBytes = 1024;
    const std::string     message          = error.what();
    return message.size() <= kMaxMessageBytes ? message : message.substr(0, kMaxMessageBytes);
}

NativeInvocationResult protocol_failure(std::string code, std::string message) {
    return {NativeInvocationStatus::ProtocolFailure, nullptr, std::move(code), std::move(message)};
}

void validate_metadata(NativeControlMetadata& metadata) {
    validate_contract_schema(metadata.input_contract, "$native.input_contract");
    validate_contract_schema(metadata.output_contract, "$native.output_contract");
    metadata.input_contract.schema  = detail::owned_json_copy(metadata.input_contract.schema);
    metadata.output_contract.schema = detail::owned_json_copy(metadata.output_contract.schema);

    if (to_string(metadata.idempotency) == "unknown") {
        throw std::invalid_argument("Native control binding idempotency is unsupported");
    }
    if (to_string(metadata.replay_behavior) == "unknown") {
        throw std::invalid_argument("Native control binding replay behavior is unsupported");
    }
    if (metadata.replay_behavior == NativeReplayBehavior::Deterministic &&
        metadata.idempotency != NativeIdempotency::Idempotent) {
        throw std::invalid_argument(
            "Deterministic native control bindings must declare idempotent execution");
    }
    const auto& cost = metadata.resource_cost;
    if (cost.max_input_bytes == 0 || cost.max_output_bytes == 0 ||
        cost.max_wall_time_ms == 0 || cost.max_memory_bytes == 0) {
        throw std::invalid_argument("Native control binding resource costs must be positive");
    }
}

void validate_binding(const neograph_program_native_binding_v1& binding) {
    if (binding.abi_version != NEOGRAPH_PROGRAM_NATIVE_ABI_V1) {
        throw std::invalid_argument("Native control binding ABI version is unsupported");
    }
    if (binding.struct_size < kMinimumBindingSize) {
        throw std::invalid_argument("Native control binding v1 struct is truncated");
    }
    if (!binding.invoke || !binding.cancel || !binding.destroy) {
        throw std::invalid_argument(
            "Native control binding requires invoke, cancel, and destroy callbacks");
    }
}

bool has_valid_owned_bytes(const neograph_program_native_owned_bytes_v1& value) {
    return (value.data != nullptr || value.size == 0) &&
           (value.data == nullptr || value.release != nullptr);
}

void release_owned_bytes(const neograph_program_native_owned_bytes_v1& value,
                         NativeInvocationResult&                       failure) noexcept {
    if (!value.data || !value.release) return;
    try {
        value.release(value.release_userdata, value.data, value.size);
    } catch (...) {
        failure = protocol_failure("P_NATIVE_RELEASE_EXCEPTION",
                                   "Native output release callback threw across its C ABI");
    }
}

}  // namespace

namespace detail {

struct NativeControlBindingImpl {
    neograph_program_native_binding_v1 binding;
    NativeControlMetadata              metadata;

    ~NativeControlBindingImpl() noexcept {
        try {
            binding.destroy(binding.userdata);
        } catch (...) {
            // C ABI plugins must not throw. There is no caller at teardown to
            // receive a diagnostic, so contain a non-conforming callback here.
        }
    }
};

struct NativeInvocationImpl {
    std::shared_ptr<NativeControlBindingImpl> binding;
    std::uint64_t                             invocation_id = 0;
    mutable std::mutex                        mutex;
    std::condition_variable                   completion;
    bool                                      cancellation_requested = false;
    bool                                      cancel_sent            = false;
    bool                                      finished               = false;
    std::optional<NativeInvocationResult>    result;
};

struct NativeCallbackLease {
    explicit NativeCallbackLease(std::shared_ptr<NativeInvocationImpl> state)
        : invocation(std::move(state)) {}

    std::shared_ptr<NativeInvocationImpl>   invocation;
    neograph_program_native_cancellation_v1 cancellation;
    // The host owns one reference while invoke() is on the stack; the
    // completion callback owns the other until it returns.
    std::atomic_uint32_t references{2};
    std::atomic_bool     callback_started{false};
};

}  // namespace detail

namespace {

int32_t is_cancel_requested(void* userdata) noexcept {
    const auto* lease = static_cast<const detail::NativeCallbackLease*>(userdata);
    if (!lease || !lease->invocation) return 1;
    std::lock_guard lock(lease->invocation->mutex);
    return lease->invocation->cancellation_requested ? 1 : 0;
}

void release_callback_lease(detail::NativeCallbackLease* lease) noexcept {
    if (!lease) return;
    if (lease->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete lease;
}

void set_terminal(const std::shared_ptr<detail::NativeInvocationImpl>& invocation,
                  NativeInvocationResult                               result) noexcept {
    try {
        std::lock_guard lock(invocation->mutex);
        if (invocation->finished) return;
        invocation->finished = true;
        invocation->result.emplace(std::move(result));
        invocation->completion.notify_all();
    } catch (...) {
        // Completion is entered from C. No C++ exception may leave this path.
    }
}

NativeInvocationResult decode_completion(
    const std::shared_ptr<detail::NativeInvocationImpl>& invocation,
    const neograph_program_native_result_v1*              raw) {
    if (!raw) {
        return protocol_failure("P_NATIVE_RESULT_MISSING",
                                "Native binding completed without a result");
    }
    if (raw->abi_version != NEOGRAPH_PROGRAM_NATIVE_ABI_V1 ||
        raw->struct_size < kMinimumResultSize) {
        return protocol_failure("P_NATIVE_RESULT_ABI",
                                "Native binding returned an incompatible result ABI");
    }
    if (raw->status != NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS &&
        raw->status != NEOGRAPH_PROGRAM_NATIVE_COMPLETION_FAILURE &&
        raw->status != NEOGRAPH_PROGRAM_NATIVE_COMPLETION_CANCELLED) {
        return protocol_failure("P_NATIVE_RESULT_STATUS",
                                "Native binding returned an unsupported completion status");
    }

    const auto& payload       = raw->payload_json;
    const bool  valid_payload = has_valid_owned_bytes(payload);
    const bool  within_budget =
        payload.size <= invocation->binding->metadata.resource_cost.max_output_bytes;
    std::string bytes;
    if (valid_payload && within_budget && payload.size != 0) {
        bytes.assign(reinterpret_cast<const char*>(payload.data), payload.size);
    }
    NativeInvocationResult release_failure;
    release_owned_bytes(payload, release_failure);
    if (!release_failure.diagnostic_code.empty()) return release_failure;
    if (!valid_payload) {
        return protocol_failure("P_NATIVE_OUTPUT_OWNERSHIP",
                                "Native binding returned invalid owned output bytes");
    }
    if (!within_budget) {
        return protocol_failure("P_NATIVE_OUTPUT_LIMIT",
                                "Native binding output exceeds its declared byte limit");
    }

    json value = nullptr;
    if (!bytes.empty()) {
        try {
            value = detail::parse_json_strict(bytes);
            if (detail::canonical_json_bytes(value) != bytes) {
                return protocol_failure("P_NATIVE_OUTPUT_CANONICAL",
                                        "Native binding output is not canonical JSON");
            }
        } catch (const std::exception& error) {
            return protocol_failure("P_NATIVE_OUTPUT_JSON",
                                    "Native binding output is not strict JSON: " +
                                        bounded_message(error));
        }
    }

    if (raw->status == NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS) {
        if (bytes.empty()) {
            return protocol_failure("P_NATIVE_OUTPUT_JSON",
                                    "Native binding success output must be a JSON value");
        }
        try {
            validate_contract_value(value, invocation->binding->metadata.output_contract,
                                    "Native binding output");
        } catch (const std::exception& error) {
            return protocol_failure("P_NATIVE_OUTPUT_SCHEMA",
                                    "Native binding output violates its declared schema: " +
                                        bounded_message(error));
        }
        return {NativeInvocationStatus::Success, std::move(value), {}, {}};
    }
    if (raw->status == NEOGRAPH_PROGRAM_NATIVE_COMPLETION_CANCELLED) {
        return {NativeInvocationStatus::Cancelled, std::move(value), "P_NATIVE_CANCELLED",
                "Native binding reported cancellation"};
    }
    return {NativeInvocationStatus::Failure, std::move(value), "P_NATIVE_FAILURE",
            "Native binding reported failure"};
}

extern "C" void native_completion(void*                                     userdata,
                                    const neograph_program_native_result_v1* result) noexcept {
    auto* const lease = static_cast<detail::NativeCallbackLease*>(userdata);
    if (!lease || lease->callback_started.exchange(true, std::memory_order_acq_rel)) return;
    try {
        set_terminal(lease->invocation, decode_completion(lease->invocation, result));
    } catch (const std::exception& error) {
        set_terminal(lease->invocation,
                     protocol_failure("P_NATIVE_CALLBACK_EXCEPTION",
                                      "Native completion handling failed: " +
                                          bounded_message(error)));
    } catch (...) {
        set_terminal(lease->invocation,
                     protocol_failure("P_NATIVE_CALLBACK_EXCEPTION",
                                      "Native completion handling failed unexpectedly"));
    }
    release_callback_lease(lease);
}

}  // namespace

std::string_view to_string(NativeIdempotency value) noexcept {
    switch (value) {
        case NativeIdempotency::Idempotent:
            return "idempotent";
        case NativeIdempotency::NonIdempotent:
            return "non_idempotent";
    }
    return "unknown";
}

NativeIdempotency native_idempotency_from_string(std::string_view value) {
    if (value == "idempotent") return NativeIdempotency::Idempotent;
    if (value == "non_idempotent") return NativeIdempotency::NonIdempotent;
    throw std::invalid_argument("Unknown native idempotency: " + std::string(value));
}

std::string_view to_string(NativeReplayBehavior value) noexcept {
    switch (value) {
        case NativeReplayBehavior::Deterministic:
            return "deterministic";
        case NativeReplayBehavior::Recorded:
            return "recorded";
        case NativeReplayBehavior::Unmanaged:
            return "unmanaged";
    }
    return "unknown";
}

NativeReplayBehavior native_replay_behavior_from_string(std::string_view value) {
    if (value == "deterministic") return NativeReplayBehavior::Deterministic;
    if (value == "recorded") return NativeReplayBehavior::Recorded;
    if (value == "unmanaged") return NativeReplayBehavior::Unmanaged;
    throw std::invalid_argument("Unknown native replay behavior: " + std::string(value));
}

NativeInvocation::NativeInvocation(std::shared_ptr<detail::NativeInvocationImpl> impl)
    : impl_(std::move(impl)) {}
NativeInvocation::NativeInvocation(NativeInvocation&&) noexcept            = default;
NativeInvocation& NativeInvocation::operator=(NativeInvocation&&) noexcept = default;
NativeInvocation::~NativeInvocation()                                      = default;

void NativeInvocation::cancel() {
    if (!impl_) return;
    bool call_cancel = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->finished) return;
        impl_->cancellation_requested = true;
        if (!impl_->cancel_sent) {
            impl_->cancel_sent = true;
            call_cancel        = true;
        }
    }
    if (!call_cancel) return;
    try {
        impl_->binding->binding.cancel(impl_->binding->binding.userdata, impl_->invocation_id);
    } catch (const std::exception& error) {
        set_terminal(impl_, protocol_failure("P_NATIVE_CANCEL_EXCEPTION",
                                             "Native cancel callback threw: " +
                                                 bounded_message(error)));
    } catch (...) {
        set_terminal(impl_, protocol_failure("P_NATIVE_CANCEL_EXCEPTION",
                                             "Native cancel callback threw unexpectedly"));
    }
}

bool NativeInvocation::cancel_requested() const noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->cancellation_requested;
}

bool NativeInvocation::finished() const noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->finished;
}

bool NativeInvocation::wait_for(std::chrono::milliseconds timeout) const {
    if (!impl_) return true;
    std::unique_lock lock(impl_->mutex);
    return impl_->completion.wait_for(lock, timeout, [&] { return impl_->finished; });
}

std::optional<NativeInvocationResult> NativeInvocation::result() const {
    if (!impl_) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return impl_->result;
}

NativeControlBinding::NativeControlBinding(std::shared_ptr<detail::NativeControlBindingImpl> impl)
    : impl_(std::move(impl)) {}
NativeControlBinding::NativeControlBinding(const NativeControlBinding&) noexcept            = default;
NativeControlBinding::NativeControlBinding(NativeControlBinding&&) noexcept                 = default;
NativeControlBinding& NativeControlBinding::operator=(const NativeControlBinding&) noexcept =
    default;
NativeControlBinding& NativeControlBinding::operator=(NativeControlBinding&&) noexcept = default;
NativeControlBinding::~NativeControlBinding()                                             = default;

NativeControlBinding NativeControlBinding::create(neograph_program_native_binding_v1 binding,
                                                   NativeControlMetadata metadata) {
    validate_binding(binding);
    validate_metadata(metadata);
    auto impl      = std::make_shared<detail::NativeControlBindingImpl>();
    impl->binding  = binding;
    impl->metadata = std::move(metadata);
    return NativeControlBinding(std::move(impl));
}

bool NativeControlBinding::valid() const noexcept {
    return static_cast<bool>(impl_);
}

const NativeControlMetadata& NativeControlBinding::metadata() const {
    if (!impl_) throw std::logic_error("Native control binding is empty");
    return impl_->metadata;
}

NativeInvocation NativeControlBinding::invoke(std::uint64_t invocation_id, const json& input) const {
    if (!impl_) throw std::logic_error("Native control binding is empty");
    if (invocation_id == 0) {
        throw std::invalid_argument("Native control invocation id must be positive");
    }
    validate_contract_value(input, impl_->metadata.input_contract, "Native binding input");
    const auto canonical_input = detail::canonical_json_bytes(input);
    if (canonical_input.size() > impl_->metadata.resource_cost.max_input_bytes) {
        throw std::invalid_argument("Native binding input exceeds its declared byte limit");
    }

    auto state           = std::make_shared<detail::NativeInvocationImpl>();
    state->binding       = impl_;
    state->invocation_id = invocation_id;
    auto* lease          = new detail::NativeCallbackLease(state);
    lease->cancellation.abi_version         = NEOGRAPH_PROGRAM_NATIVE_ABI_V1;
    lease->cancellation.struct_size         = sizeof(neograph_program_native_cancellation_v1);
    lease->cancellation.userdata            = lease;
    lease->cancellation.is_cancel_requested = is_cancel_requested;

    const neograph_program_native_invoke_request_v1 request{
        NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        sizeof(neograph_program_native_invoke_request_v1),
        invocation_id,
        {reinterpret_cast<const std::uint8_t*>(canonical_input.data()), canonical_input.size()},
        &lease->cancellation,
    };

    bool release_callback_reference = false;
    try {
        const auto invoke_result = impl_->binding.invoke(impl_->binding.userdata, &request,
                                                          native_completion, lease);
        release_callback_reference = invoke_result != NEOGRAPH_PROGRAM_NATIVE_INVOKE_ACCEPTED;
        if (release_callback_reference) {
            set_terminal(state,
                         protocol_failure("P_NATIVE_INVOKE_REJECTED",
                                          "Native binding rejected the invocation before completion"));
        }
    } catch (const std::exception& error) {
        release_callback_reference = true;
        set_terminal(state, protocol_failure("P_NATIVE_INVOKE_EXCEPTION",
                                             "Native invoke callback threw: " +
                                                 bounded_message(error)));
    } catch (...) {
        release_callback_reference = true;
        set_terminal(state, protocol_failure("P_NATIVE_INVOKE_EXCEPTION",
                                             "Native invoke callback threw unexpectedly"));
    }

    // A synchronous completion has consumed its reference but not the host's.
    // A rejected/throwing plugin promises no callback; reclaim that reference
    // only when a (non-conforming) synchronous completion did not already do so.
    if (release_callback_reference &&
        !lease->callback_started.load(std::memory_order_acquire)) {
        release_callback_lease(lease);
    }
    release_callback_lease(lease);

    return NativeInvocation(std::move(state));
}

}  // namespace neograph::program
