/**
 * @file program/native_abi.h
 * @brief Stable C v1 contract for externally implemented Program host bindings.
 *
 * This header deliberately contains C layouts only.  A plugin owns the
 * `userdata` it gives to a binding and must not throw across any callback.
 * Inputs are borrowed for the duration of `invoke`; a completion owns its
 * result bytes until it calls the supplied release hook exactly once.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEOGRAPH_PROGRAM_NATIVE_ABI_V1 UINT32_C(1)

#define NEOGRAPH_PROGRAM_NATIVE_INVOKE_ACCEPTED INT32_C(0)
#define NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED INT32_C(1)

#define NEOGRAPH_PROGRAM_NATIVE_COMPLETION_SUCCESS UINT32_C(1)
#define NEOGRAPH_PROGRAM_NATIVE_COMPLETION_FAILURE UINT32_C(2)
#define NEOGRAPH_PROGRAM_NATIVE_COMPLETION_CANCELLED UINT32_C(3)

/** Borrowed, non-owning bytes. `data` may be null only when `size` is zero. */
typedef struct neograph_program_native_byte_span_v1 {
    const uint8_t* data;
    size_t         size;
} neograph_program_native_byte_span_v1;

/**
 * Releases one output allocation. The host calls this at most once, after it
 * has copied the bytes. The callback must not call NeoGraph or throw.
 */
typedef void (*neograph_program_native_release_v1)(void*          userdata,
                                                    const uint8_t* data,
                                                    size_t         size);

/** Plugin-owned bytes returned through a completion callback. */
typedef struct neograph_program_native_owned_bytes_v1 {
    const uint8_t*                       data;
    size_t                               size;
    void*                                release_userdata;
    neograph_program_native_release_v1   release;
} neograph_program_native_owned_bytes_v1;

/**
 * A borrowed cancellation view. It remains valid only until the matching
 * completion callback returns. Plugins may poll it from their own worker
 * threads, but must not retain it after completion.
 */
typedef struct neograph_program_native_cancellation_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void*    userdata;
    int32_t (*is_cancel_requested)(void* userdata);
} neograph_program_native_cancellation_v1;

/** Immutable invocation input. `input_json` is canonical JSON UTF-8. */
typedef struct neograph_program_native_invoke_request_v1 {
    uint32_t                                          abi_version;
    uint32_t                                          struct_size;
    uint64_t                                          invocation_id;
    neograph_program_native_byte_span_v1              input_json;
    const neograph_program_native_cancellation_v1*    cancellation;
} neograph_program_native_invoke_request_v1;

/**
 * One terminal completion. Successful payloads are canonical JSON values.
 * Failure and cancellation payloads, when present, are canonical diagnostic
 * values. The entire result is borrowed only for the completion call.
 */
typedef struct neograph_program_native_result_v1 {
    uint32_t                                  abi_version;
    uint32_t                                  struct_size;
    uint32_t                                  status;
    neograph_program_native_owned_bytes_v1    payload_json;
} neograph_program_native_result_v1;

typedef void (*neograph_program_native_completion_v1)(
    void*                                           completion_userdata,
    const neograph_program_native_result_v1*       result);

/**
 * Starts one invocation. Return ACCEPTED only if the plugin will invoke the
 * completion callback exactly once, possibly synchronously. Return REJECTED
 * if it will not invoke completion. Neither path may throw across the ABI.
 */
typedef int32_t (*neograph_program_native_invoke_v1)(
    void*                                             userdata,
    const neograph_program_native_invoke_request_v1* request,
    neograph_program_native_completion_v1            completion,
    void*                                             completion_userdata);

/** Requests cancellation of one accepted invocation; it never frees userdata. */
typedef void (*neograph_program_native_cancel_v1)(void* userdata, uint64_t invocation_id);

/**
 * Releases plugin-owned userdata exactly once after every accepted invocation
 * has completed. A plugin must not invoke any completion after this callback.
 */
typedef void (*neograph_program_native_destroy_v1)(void* userdata);

/** Versioned plugin binding supplied to the C++ Program registration surface. */
typedef struct neograph_program_native_binding_v1 {
    uint32_t                          abi_version;
    uint32_t                          struct_size;
    void*                             userdata;
    neograph_program_native_invoke_v1 invoke;
    neograph_program_native_cancel_v1 cancel;
    neograph_program_native_destroy_v1 destroy;
} neograph_program_native_binding_v1;

#ifdef __cplusplus
}  // extern "C"
#endif
