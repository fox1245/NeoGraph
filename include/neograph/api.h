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
 * is OFF). Headers don't need any other glue.
 *
 * Why a custom macro instead of CMake's ``generate_export_header``?
 * The engine ships several libraries (``neograph_core``,
 * ``neograph_async``, ``neograph_llm``, ...) that can include each
 * other's public headers. ``generate_export_header`` produces a
 * per-library macro; managing four separate ``XXX_EXPORT`` ifdefs
 * across the headers gets tangled fast. A single ``NEOGRAPH_API``
 * keyed on whether ANY neograph_* TU is being compiled keeps the
 * source tree clean — at the cost of treating cross-library calls
 * inside the engine as if every public symbol were a same-library
 * call (which they effectively are during the engine's own build).
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
