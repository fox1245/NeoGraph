#include <neograph/program/native_abi.h>

#include <stddef.h>

_Static_assert(offsetof(neograph_program_native_binding_v1, destroy) +
                   sizeof(neograph_program_native_destroy_v1) ==
                   sizeof(neograph_program_native_binding_v1),
               "v1 binding layout must be complete");
_Static_assert(offsetof(neograph_program_native_invoke_request_v1, cancellation) +
                   sizeof(const neograph_program_native_cancellation_v1*) ==
                   sizeof(neograph_program_native_invoke_request_v1),
               "v1 request layout must be complete");

static int32_t smoke_invoke(void* userdata,
                            const neograph_program_native_invoke_request_v1* request,
                            neograph_program_native_completion_v1 completion,
                            void* completion_userdata) {
    (void)userdata;
    (void)request;
    (void)completion;
    (void)completion_userdata;
    return NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED;
}

static void smoke_cancel(void* userdata, uint64_t invocation_id) {
    (void)userdata;
    (void)invocation_id;
}

static void smoke_destroy(void* userdata) {
    (void)userdata;
}

int neograph_native_abi_c_smoke(void) {
    neograph_program_native_binding_v1 binding = {
        NEOGRAPH_PROGRAM_NATIVE_ABI_V1,
        sizeof(binding),
        0,
        smoke_invoke,
        smoke_cancel,
        smoke_destroy,
    };
    return binding.abi_version == NEOGRAPH_PROGRAM_NATIVE_ABI_V1 ? 0 : 1;
}
