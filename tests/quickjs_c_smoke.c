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

#if defined(_MSC_VER)
static void stage(const char* name) {
    fprintf(stderr, "quickjs C embedding smoke stage: %s\n", name);
    fflush(stderr);
}
#else
static void stage(const char* name) {
    (void)name;
}
#endif

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

static JSValue macro_generic_magic(
    JSContext* context, JSValueConst this_value, int argc, JSValueConst* argv, int magic) {
    (void)context;
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)magic;
    return JS_UNDEFINED;
}

static double macro_unary(double value) {
    return value;
}

static double macro_binary(double left, double right) {
    return left + right;
}

static JSValue macro_getter(JSContext* context, JSValueConst this_value) {
    (void)context;
    (void)this_value;
    return JS_UNDEFINED;
}

static JSValue macro_setter(JSContext* context, JSValueConst this_value, JSValueConst value) {
    (void)context;
    (void)this_value;
    (void)value;
    return JS_UNDEFINED;
}

static JSValue macro_getter_magic(JSContext* context, JSValueConst this_value, int magic) {
    (void)context;
    (void)this_value;
    (void)magic;
    return JS_UNDEFINED;
}

static JSValue macro_setter_magic(JSContext*   context,
                                  JSValueConst this_value,
                                  JSValueConst value,
                                  int          magic) {
    (void)context;
    (void)this_value;
    (void)value;
    (void)magic;
    return JS_UNDEFINED;
}

static JSValue macro_iterator_next(JSContext*    context,
                                   JSValueConst  this_value,
                                   int           argc,
                                   JSValueConst* argv,
                                   int*          done,
                                   int           magic) {
    (void)context;
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)done;
    (void)magic;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry macro_nested_entries[] = {JS_CFUNC_DEF("nested", 0, native_add)};
static const JSCFunctionListEntry macro_entries[]        = {
    JS_CFUNC_DEF("generic", 1, native_add),
    JS_CFUNC_MAGIC_DEF("magic", 2, macro_generic_magic, 3),
    JS_CFUNC_SPECIAL_DEF("unary", 1, f_f, macro_unary),
    JS_CFUNC_SPECIAL_DEF("binary", 2, f_f_f, macro_binary),
    JS_ITERATOR_NEXT_DEF("next", 1, macro_iterator_next, 4),
    JS_CGETSET_DEF("property", macro_getter, macro_setter),
    JS_CGETSET_MAGIC_DEF("magicProperty", macro_getter_magic, macro_setter_magic, 5),
    JS_PROP_STRING_DEF("string", "value", JS_PROP_CONFIGURABLE),
    JS_PROP_INT32_DEF("int32", 7, JS_PROP_WRITABLE),
    JS_PROP_INT64_DEF("int64", 8, JS_PROP_WRITABLE),
    JS_PROP_DOUBLE_DEF("double", 9.0, JS_PROP_WRITABLE),
    JS_PROP_UNDEFINED_DEF("undefined", JS_PROP_WRITABLE),
    JS_PROP_ATOM_DEF("atom", 10, JS_PROP_WRITABLE),
    JS_PROP_BOOL_DEF("bool", 1, JS_PROP_WRITABLE),
    JS_OBJECT_DEF("object", macro_nested_entries, 1, JS_PROP_WRITABLE),
    JS_ALIAS_DEF("alias", "generic"),
    JS_ALIAS_BASE_DEF("baseAlias", "generic", 3),
};

static int verify_function_list_initializers(void) {
    if (macro_entries[0].u.func.cfunc.generic != native_add ||
        macro_entries[1].u.func.cfunc.generic_magic != macro_generic_magic ||
        macro_entries[2].u.func.cfunc.f_f != macro_unary ||
        macro_entries[3].u.func.cfunc.f_f_f != macro_binary ||
        macro_entries[4].u.func.cfunc.iterator_next != macro_iterator_next) {
        return fail("function-list function fields changed");
    }
    if (macro_entries[5].u.getset.get.getter != macro_getter ||
        macro_entries[5].u.getset.set.setter != macro_setter ||
        macro_entries[6].u.getset.get.getter_magic != macro_getter_magic ||
        macro_entries[6].u.getset.set.setter_magic != macro_setter_magic) {
        return fail("function-list accessor fields changed");
    }
    if (strcmp(macro_entries[7].u.str, "value") != 0 || macro_entries[8].u.i32 != 7 ||
        macro_entries[9].u.i64 != 8 || macro_entries[10].u.f64 != 9.0 ||
        macro_entries[11].u.i32 != 0 || macro_entries[12].u.i32 != 10 ||
        macro_entries[13].u.i32 != 1 || macro_entries[14].u.prop_list.tab != macro_nested_entries ||
        macro_entries[14].u.prop_list.len != 1 || macro_entries[15].u.alias.base != -1 ||
        macro_entries[16].u.alias.base != 3) {
        return fail("function-list data fields changed");
    }
    return 0;
}

int main(void) {
    stage("verify function-list initializers");
    if (verify_function_list_initializers() != 0) return 1;

    stage("create runtime");
    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) return fail("JS_NewRuntime failed");

    stage("create context");
    JSContext* context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        return fail("JS_NewContext failed");
    }

    int result = 0;
    stage("install native binding");
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

    stage("evaluate literal");
    if (!evaluate_truth(context, "1 + 1 === 2", JS_EVAL_TYPE_GLOBAL)) {
        result = fail("literal evaluation failed");
        goto done;
    }

    stage("evaluate native binding");
    if (!evaluate_truth(context, "nativeAdd(20, 22) === 42", JS_EVAL_TYPE_GLOBAL)) {
        result = fail("native binding evaluation failed");
        goto done;
    }

    stage("evaluate sealed global surface");
    if (!evaluate_truth(context,
                        "typeof std === 'undefined' && typeof os === 'undefined' && "
                        "typeof process === 'undefined' && typeof require === 'undefined' && "
                        "typeof fetch === 'undefined' && typeof Worker === 'undefined' && "
                        "typeof load === 'undefined'",
                        JS_EVAL_TYPE_GLOBAL)) {
        result = fail("sealed global surface check failed");
        goto done;
    }

    stage("reject unregistered module");

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

    stage("interrupt infinite evaluation");

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

    stage("enforce memory limit");

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

    stage("enforce stack limit");

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
    stage("destroy context and runtime");
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    if (result == 0) puts("quickjs C embedding smoke passed");
    return result;
}
