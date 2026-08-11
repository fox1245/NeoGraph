#include <neograph/program/authoring.h>
#include <neograph/program/compiler.h>
#include <neograph/program/registry.h>

extern "C" {
struct JSRuntime;
JSRuntime* JS_NewRuntime(void);
int        second_quickjs_identity(void);
int        second_quickjs_runtime_calls(void);
}

int main() {
    using namespace neograph::program;

    if (second_quickjs_identity() != 0x514a5332 || second_quickjs_runtime_calls() != 0) {
        return 1;
    }

    ProgramCompiler compiler(RegistrySnapshotBuilder().build(),
                             ProgramCompilerConfig{"dual-quickjs-consumer/v1"});
    try {
        (void)compile_javascript(
            compiler,
            JavaScriptPublicationRequest{"dual-engine.js", "export function define() {", {}, {}});
        return 2;
    } catch (const ProgramCompileError&) {
        // The pinned NeoGraph engine parsed the module and rejected its syntax.
        // It must not have called the second engine below.
    }

    if (second_quickjs_runtime_calls() != 0) {
        return 3;
    }
    if (JS_NewRuntime() == nullptr || second_quickjs_runtime_calls() != 1) {
        return 4;
    }
    return 0;
}
