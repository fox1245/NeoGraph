#include "interpreter_exit.h"

#include "core/process_generation.h"

#include <neograph/graph/executor.h>

namespace neograph::pybind {
namespace {

bool finalizing() noexcept {
#if PY_VERSION_HEX >= 0x030D0000
    return Py_IsFinalizing() != 0;
#else
    return _Py_IsFinalizing() != 0;
#endif
}

// The generation of the process the module was imported in.
const neograph::detail::OpenedInProcess& imported_in() {
    static const neograph::detail::OpenedInProcess opened;
    return opened;
}

}  // namespace

void note_import_process() { static_cast<void>(imported_in()); }

bool process_forked_since_import() noexcept { return imported_in().inherited(); }

bool may_enter_interpreter() noexcept {
    // After Py_FinalizeEx has returned, PyGILState_Check() answers 1 for every
    // thread, so the interpreter is asked first.
    if (!Py_IsInitialized()) return false;
    if (PyGILState_Check()) return true;
    return !finalizing();
}

void quiesce_before_finalization() {
    // A node attempt that ignored its cancellation past its stop deadline
    // (#364) can outlive its run; when its body is Python it needs the
    // interpreter to finish. Wait for such attempts first: they hand their
    // result to the thread that is stopped next.
    if (!process_forked_since_import()) neograph::graph::wait_for_node_attempts();
    close_async_runtime();
    close_worker_pools();
}

}  // namespace neograph::pybind
