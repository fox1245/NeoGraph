/**
 * @file api.h
 * @brief NEOGRAPH_API export/import macro for shared-library builds.
 *
 * Public class and free-function declarations decorated with
 * ``NEOGRAPH_API`` get the right linkage attribute on every supported
 * platform:
 *
 *   - When building a NeoGraph library (TU sees
 *     ``NEOGRAPH_BUILDING_LIBRARY``): ``__declspec(dllexport)`` on
 *     Windows, ``visibility("default")`` on Linux/macOS.
 *   - When consuming a NeoGraph library (downstream binding /
 *     application): ``__declspec(dllimport)`` on Windows, no-op
 *     elsewhere.
 *   - Static builds on any platform: empty.
 *
 * The CMake target machinery sets ``NEOGRAPH_BUILDING_LIBRARY`` (a
 * ``target_compile_definitions`` PRIVATE on each ``neograph_*``
 * library) and ``NEOGRAPH_STATIC_BUILD`` (when ``BUILD_SHARED_LIBS``
 * is OFF). Component-specific macros also receive their owning target's
 * private build definition.
 *
 * Most engine libraries use the shared ``NEOGRAPH_API`` macro. Components
 * whose public headers are intentionally consumed by another NeoGraph DLL
 * may define a narrower macro below. MSVC eagerly emits helpers for exported
 * classes, so treating those cross-DLL declarations as exports can create
 * references to implementations owned by the wrong DLL.
 */
#pragma once

#if defined(NEOGRAPH_STATIC_BUILD)
    // Static-only build: no decoration, every symbol is a normal
    // member of the static archive.
    #define NEOGRAPH_API
#elif defined(_WIN32) || defined(__CYGWIN__)
    #if defined(NEOGRAPH_BUILDING_LIBRARY)
        #define NEOGRAPH_API __declspec(dllexport)
    #else
        #define NEOGRAPH_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    // Linux/macOS: explicit default visibility so a downstream
    // library built with -fvisibility=hidden still sees these.
    #define NEOGRAPH_API __attribute__((visibility("default")))
#else
    #define NEOGRAPH_API
#endif

// Harness declarations belong to neograph_mcp_server but are also consumed by
// the MCP client, A2A adapter, and SQLite store libraries.
#if defined(NEOGRAPH_STATIC_BUILD)
    #define NEOGRAPH_MCP_SERVER_API
#elif defined(_WIN32) || defined(__CYGWIN__)
    #if defined(NEOGRAPH_BUILDING_MCP_SERVER)
        #define NEOGRAPH_MCP_SERVER_API __declspec(dllexport)
    #else
        #define NEOGRAPH_MCP_SERVER_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define NEOGRAPH_MCP_SERVER_API __attribute__((visibility("default")))
#else
    #define NEOGRAPH_MCP_SERVER_API
#endif

// PR 4 (v0.4.0): cross-compiler deprecation-warning suppression.
// Used inside the engine where the legacy 8-virtual default chain
// and add_cancel_hook fallback paths legitimately call deprecated
// symbols on behalf of consumers who haven't migrated yet. User
// code calling the same symbols still sees the warning.
//
//   GCC / clang  → -Wdeprecated-declarations
//   MSVC         → C4996
//
// Block-style usage:
//   NEOGRAPH_PUSH_IGNORE_DEPRECATED
//   // ... legacy default chain body ...
//   NEOGRAPH_POP_IGNORE_DEPRECATED
#if defined(__GNUC__) || defined(__clang__)
    #define NEOGRAPH_PUSH_IGNORE_DEPRECATED                            \
        _Pragma("GCC diagnostic push")                                  \
        _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
    #define NEOGRAPH_POP_IGNORE_DEPRECATED                              \
        _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
    #define NEOGRAPH_PUSH_IGNORE_DEPRECATED                             \
        __pragma(warning(push))                                          \
        __pragma(warning(disable : 4996))
    #define NEOGRAPH_POP_IGNORE_DEPRECATED                              \
        __pragma(warning(pop))
#else
    #define NEOGRAPH_PUSH_IGNORE_DEPRECATED
    #define NEOGRAPH_POP_IGNORE_DEPRECATED
#endif

// =========================================================================
// Issue #16 — silent ODR-trap detection for cpp-httplib + OpenSSL.
//
// NeoGraph's bundled cpp-httplib is built with `CPPHTTPLIB_OPENSSL_SUPPORT`
// defined (TLS ON). cpp-httplib's `ClientImpl` class adds extra members
// when this macro is set, so a downstream TU that includes `<httplib.h>`
// WITHOUT the macro sees a different class layout — a One Definition Rule
// violation. The linker keeps a single inline-function instantiation
// arbitrarily, so callers compiled against the wrong layout read members
// at the wrong offsets and SEGV inside the resolver
// (`getaddrinfo` / `internal_strlen`) the first time the LLM endpoint
// is hit.
//
// This guard catches the ordering case where the user's TU includes
// `<httplib.h>` BEFORE the first NeoGraph header (the natural order in
// many builds — system headers first, project headers second). The
// cpp-httplib include guard `CPPHTTPLIB_HTTPLIB_H` is already defined
// at this point, and we can compare macro state.
//
// Caveat: this guard does NOT fire when the user includes any NeoGraph
// header BEFORE `<httplib.h>` — at that point cpp-httplib hasn't been
// processed yet, its include guard isn't visible, so we have nothing to
// check against. For that ordering, the docstring on
// `include/neograph/llm/schema_provider.h` and
// `docs/troubleshooting.md` "C++ consumers — httplib.h macro
// consistency" explain the symptom and the fix.
//
// Disable the guard with `-DNEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD=1` in
// build flags — last-resort escape for downstream TUs that have a
// genuine reason to mismatch (vanishingly rare; the canonical fix is
// to define the macro consistently).
#if defined(CPPHTTPLIB_HTTPLIB_H)                            \
    && !defined(CPPHTTPLIB_OPENSSL_SUPPORT)                  \
    && !defined(NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD)
#  error "NeoGraph: <httplib.h> was included before this NeoGraph header without CPPHTTPLIB_OPENSSL_SUPPORT defined. NeoGraph's bundled cpp-httplib is built with TLS support ON; a mismatched macro state between TUs is an ODR violation that silently SEGVs inside getaddrinfo at runtime (issue #16). Fix: `#define CPPHTTPLIB_OPENSSL_SUPPORT` BEFORE `#include <httplib.h>` in this TU, OR add `target_compile_definitions(<your_target> PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)` to your CMakeLists. See docs/troubleshooting.md \"C++ consumers — httplib.h macro consistency\". Last-resort opt-out: `-DNEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD=1`."
#endif
