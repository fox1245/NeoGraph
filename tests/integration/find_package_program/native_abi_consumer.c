#include <neograph/program/native_abi.h>

#include <stddef.h>

typedef char native_binding_abi_field_must_be_first
    [offsetof(neograph_program_native_binding_v1, abi_version) == 0 ? 1 : -1];
typedef char native_binding_size_field_must_follow_abi_field
    [offsetof(neograph_program_native_binding_v1, struct_size) >
             offsetof(neograph_program_native_binding_v1, abi_version)
         ? 1
         : -1];
typedef char native_result_must_contain_v1_payload_prefix
    [sizeof(neograph_program_native_result_v1) >=
             offsetof(neograph_program_native_result_v1, payload_json) +
                 sizeof(neograph_program_native_owned_bytes_v1)
         ? 1
         : -1];

static int32_t reject_invoke(void*                                            userdata,
                             const neograph_program_native_invoke_request_v1* request,
                             neograph_program_native_completion_v1            completion,
                             void*                                            completion_userdata) {
    (void)userdata;
    (void)request;
    (void)completion;
    (void)completion_userdata;
    return NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED;
}

static void cancel_invoke(void* userdata, uint64_t invocation_id) {
    (void)userdata;
    (void)invocation_id;
}

static void destroy_binding(void* userdata) {
    (void)userdata;
}

int main(void) {
    const neograph_program_native_binding_v1 binding = {
        NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        sizeof(neograph_program_native_binding_v1),
        NULL,
        reject_invoke,
        cancel_invoke,
        destroy_binding,
    };
    return binding.abi_version == NEOGRAPH_PROGRAM_NATIVE_ABI_V1 && binding.invoke != NULL &&
                   binding.cancel != NULL && binding.destroy != NULL
               ? 0
               : 1;
}
