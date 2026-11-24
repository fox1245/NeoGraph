// What the binding does when the interpreter exits while an engine is alive.
//
// The binding owns threads that hold Python objects and run Python code: the
// thread that drives `run_async`, the pools that run Python node and tool
// bodies. When the main thread finalizes the interpreter, a thread that then
// takes the GIL is made to exit (PyThread_exit_thread), its stack is unwound
// by force through frames that may not unwind (noexcept ones end in
// std::terminate), and the destructors that run on the way touch Python
// objects whose thread state is already gone.
//
// Two measures keep that from happening:
//
//  * An atexit hook, registered when the module is imported, stops those
//    threads before finalization starts: it stops accepting work, lets the
//    callbacks that are running finish (the GIL is released while it waits)
//    and joins the threads. See quiesce_before_finalization(). In a child
//    made by fork it leaves the threads and the waits of its parent alone.
//  * What is released after that, by a thread that does not hold the GIL
//    (an abandoned handler destroyed when the process exits, a thread the
//    binding does not know), is dropped without calling into the interpreter:
//    delete_with_gil() leaks the object instead of taking the GIL, and
//    EngineGil refuses to run Python code.

#pragma once

#include <pybind11/pybind11.h>

#include <optional>
#include <stdexcept>

namespace neograph::pybind {

/// May the calling thread take the GIL? False once the interpreter is gone or
/// finalizing, unless the thread already holds the GIL (the main thread
/// tearing modules down, say), which is always safe.
bool may_enter_interpreter() noexcept;

/// The deleter of an object that owns Python references and that any thread
/// may drop last. Takes the GIL for the destructor, or leaks the object when
/// the interpreter can no longer be entered.
template <typename T>
void delete_with_gil(T* object) noexcept {
    if (!may_enter_interpreter()) return;
    if (PyGILState_Check()) {
        delete object;
        return;
    }
    pybind11::gil_scoped_acquire gil;
    delete object;
}

/// Drops a Python reference that a thread without the GIL may release last:
/// takes the GIL for the decrement, or leaves the object alone (leaks the
/// reference) when the interpreter can no longer be entered.
inline void release_with_gil(pybind11::object& object) noexcept {
    if (!may_enter_interpreter()) {
        static_cast<void>(object.release());
        return;
    }
    pybind11::gil_scoped_acquire gil;
    object = pybind11::object();
}

/// The GIL for the Python code of a node or a tool, which an engine thread
/// runs. When the interpreter can no longer be entered it throws instead of
/// calling into it, so the run fails with an ordinary error.
class EngineGil {
public:
    EngineGil() {
        if (!may_enter_interpreter())
            throw std::runtime_error(
                "the Python interpreter is shutting down; Python code is not run");
        gil_.emplace();
    }

private:
    std::optional<pybind11::gil_scoped_acquire> gil_;
};

/// Records the process the module was imported in and registers the fork handler (once). Called when the module is
/// imported; throws std::system_error when the platform cannot register the handler, which fails the import instead of
/// leaving a child that cannot tell it is one.
void note_import_process();

/// True in a child made by fork after the module was imported: the binding's threads, and what they were doing, are
/// the parent's.
bool process_forked_since_import() noexcept;

/// The atexit hook: stops the threads of the binding that run Python code and
/// hold Python objects, while the interpreter is still whole. Called with the
/// GIL released.
///
/// In a child made by fork it stops only the threads the child started itself
/// (process_owned.h) and does not wait for the node attempts: the count of
/// running attempts belongs to the whole process, so the child cannot tell its
/// own attempts from the ones its parent's threads were running at the fork,
/// which no thread of the child can finish, and the lock that guards the count
/// may have been held by one of them. An attempt the child started and that
/// outlives its run is therefore not waited for there.
void quiesce_before_finalization();

// Parts of the hook that live next to what they stop.
void close_async_runtime();         // bind_graph.cpp
void close_worker_pools();          // bind_node.cpp

}  // namespace neograph::pybind
