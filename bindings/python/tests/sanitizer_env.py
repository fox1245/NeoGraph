"""The environment of a child process that a Python test starts under AddressSanitizer.

The sanitizer job runs the Python suite with AddressSanitizer preloaded and leak
detection on, and a child process inherits both. A script that defines a node
function and compiles an engine in the same module ends with that engine alive:
the engine holds the node object, the node holds the function, and the function
holds the module's globals, which hold the engine. Python's collector cannot see
the part of that cycle that runs through the C++ side, so finalization does not
free it, and whether LeakSanitizer reports the engine's control block depends on
the Python build. The binding also gives up on an object by design when a thread
without the GIL drops it after the interpreter is gone (interpreter_exit.h).

The C++ suites check for leaks. A child process here checks what it prints and
how it exits, so leak detection is turned off for it; the other AddressSanitizer
checks (use after free, overflows) stay on.
"""

import os


def without_leak_detection(env=None):
    """Return a copy of `env` (default: the current environment) for a child process."""
    env = dict(os.environ if env is None else env)
    options = [part for part in env.get("ASAN_OPTIONS", "").split(":")
               if part and not part.startswith("detect_leaks=")]
    if "ASAN_OPTIONS" in env or "asan" in env.get("LD_PRELOAD", ""):
        env["ASAN_OPTIONS"] = ":".join([*options, "detect_leaks=0"])
    return env
