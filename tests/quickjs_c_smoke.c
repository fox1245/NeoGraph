#include <quickjs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t polls;
    uint64_t limit;
} InterruptBudget;

static int fail(const char* message) {
    fprintf(stderr, "quickjs C embedding smoke: %s\n", message);
    return 1;
}

static int clear_exception(JSContext* context) {
    JSValue   exception     = JS_GetException(context);
    const int has_exception = !JS_IsUndefined(exception);
    JS_FreeValue(context, exception);
    return has_exception;
}

static int evaluate_truth(JSContext* context, const char* source, int flags) {
    JSValue result = JS_Eval(context, source, strlen(source), "quickjs-c-smoke.js", flags);
    if (JS_IsException(result)) {
        JS_FreeValue(context, result);
        (void)clear_exception(context);
        return 0;
    }

    const int truthy = JS_ToBool(context, result) == 1;
    JS_FreeValue(context, result);
    return truthy;
}

static JSValue native_add(JSContext*    context,
                          JSValueConst  this_value,
                          int           argc,
                          JSValueConst* argv) {
    (void)this_value;
    if (argc != 2) return JS_ThrowTypeError(context, "nativeAdd expects two arguments");

    int32_t left  = 0;
    int32_t right = 0;
    if (JS_ToInt32(context, &left, argv[0]) < 0 || JS_ToInt32(context, &right, argv[1]) < 0) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(context, left + right);
}

static int interrupt_after_budget(JSRuntime* runtime, void* opaque) {
    (void)runtime;
    InterruptBudget* budget = opaque;
    ++budget->polls;
    return budget->polls >= budget->limit;
}

int main(void) {
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) return fail("JS_NewRuntime failed");

    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        return fail("JS_NewContext failed");
    }

    int     result = 0;
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) {
        result = fail("JS_GetGlobalObject failed");
        goto done;
    }
    if (JS_SetPropertyStr(context, global, "nativeAdd",
                          JS_NewCFunction(context, native_add, "nativeAdd", 2)) != 1) {
        JS_FreeValue(context, global);
        result = fail("could not install nativeAdd");
        goto done;
    }
    JS_FreeValue(context, global);

    if (!evaluate_truth(context,
                        "nativeAdd(20, 22) === 42 && typeof std === 'undefined' && "
                        "typeof os === 'undefined' && typeof process === 'undefined' && "
                        "typeof require === 'undefined' && typeof fetch === 'undefined' && "
                        "typeof Worker === 'undefined' && typeof load === 'undefined'",
                        JS_EVAL_TYPE_GLOBAL)) {
        result = fail("explicit binding or sealed global surface check failed");
        goto done;
    }

    JSValue module =
        JS_Eval(context, "import * as os from 'os';", strlen("import * as os from 'os';"),
                "quickjs-c-smoke-module.js", JS_EVAL_TYPE_MODULE);
    if (!JS_IsException(module)) {
        JS_FreeValue(context, module);
        result = fail("unregistered os module was accepted");
        goto done;
    }
    JS_FreeValue(context, module);
    if (!clear_exception(context)) {
        result = fail("unregistered module rejection did not set an exception");
        goto done;
    }

    InterruptBudget budget = {0, 8};
    JS_SetInterruptHandler(runtime, interrupt_after_budget, &budget);
    JSValue infinite = JS_Eval(context, "for (;;) {}", strlen("for (;;) {}"),
                               "quickjs-c-smoke-interrupt.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(infinite)) {
        JS_FreeValue(context, infinite);
        result = fail("infinite evaluation was not interrupted");
        goto done;
    }
    JS_FreeValue(context, infinite);
    if (!clear_exception(context) || budget.polls < budget.limit) {
        result = fail("interrupt callback was not observed");
        goto done;
    }
    JS_SetInterruptHandler(runtime, NULL, NULL);

    JS_SetMemoryLimit(runtime, 256 * 1024);
    JSValue allocation =
        JS_Eval(context, "new Uint8Array(1024 * 1024)", strlen("new Uint8Array(1024 * 1024)"),
                "quickjs-c-smoke-memory.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(allocation)) {
        JS_FreeValue(context, allocation);
        result = fail("runtime memory limit was not enforced");
        goto done;
    }
    JS_FreeValue(context, allocation);
    if (!clear_exception(context)) {
        result = fail("memory limit rejection did not set an exception");
        goto done;
    }

    JS_SetMemoryLimit(runtime, 8 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 64 * 1024);
    JSValue recursion = JS_Eval(context, "function recurse() { return recurse(); } recurse();",
                                strlen("function recurse() { return recurse(); } recurse();"),
                                "quickjs-c-smoke-stack.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(recursion)) {
        JS_FreeValue(context, recursion);
        result = fail("runtime stack limit was not enforced");
        goto done;
    }
    JS_FreeValue(context, recursion);
    if (!clear_exception(context)) {
        result = fail("stack limit rejection did not set an exception");
        goto done;
    }

done:
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    if (result == 0) puts("quickjs C embedding smoke passed");
    return result;
}
