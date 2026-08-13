/*
 * A deliberately tiny second QuickJS ABI provider. Keeping JS_NewRuntime and
 * the fixture-only identity functions in one archive member forces this member
 * into the final link, so an unprefixed NeoGraph QuickJS definition is either
 * captured or diagnosed as a duplicate instead of passing by link order.
 */
struct JSRuntime {
    int marker;
};

static struct JSRuntime second_runtime = {0x514a5332};
static int              runtime_calls;

struct JSRuntime* JS_NewRuntime(void) {
    ++runtime_calls;
    return &second_runtime;
}

int second_quickjs_identity(void) {
    return second_runtime.marker;
}

int second_quickjs_runtime_calls(void) {
    return runtime_calls;
}
