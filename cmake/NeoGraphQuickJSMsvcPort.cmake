# Deterministic, fail-closed MSVC portability overlay for the pinned QuickJS
# source archive.  The vendored archive remains byte-for-byte upstream; only
# build-directory copies are adapted, so archive provenance and platform-port
# provenance remain separate identities.

function(_neograph_quickjs_replace_exact variable_name needle replacement label)
    string(FIND "${${variable_name}}" "${needle}" _match)
    if(_match EQUAL -1)
        message(FATAL_ERROR
            "NeoGraph QuickJS MSVC port: expected source fragment not found (${label}). "
            "The pinned QuickJS source changed; review and version the port instead of "
            "silently compiling an unreviewed variant.")
    endif()
    string(REPLACE "${needle}" "${replacement}" _updated "${${variable_name}}")
    set(${variable_name} "${_updated}" PARENT_SCOPE)
endfunction()

function(neograph_prepare_quickjs_msvc_sources quickjs_source_dir output_dir output_variable)
    file(MAKE_DIRECTORY "${output_dir}")

    set(_inputs
        quickjs.c
        cutils.c
        dtoa.c
        libregexp.c
        libunicode.c
        cutils.h)
    foreach(_input IN LISTS _inputs)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${quickjs_source_dir}/${_input}")
    endforeach()

    file(READ "${quickjs_source_dir}/cutils.h" _cutils_h)
    _neograph_quickjs_replace_exact(_cutils_h
[=[#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define force_inline inline __attribute__((always_inline))
#define no_inline __attribute__((noinline))
#define __maybe_unused __attribute__((unused))]=]
[=[#if defined(_MSC_VER)
#include <intrin.h>
#define likely(x)       (x)
#define unlikely(x)     (x)
#define force_inline    __forceinline
#define no_inline       __declspec(noinline)
#define __maybe_unused
#define __attribute__(x)
#else
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define force_inline inline __attribute__((always_inline))
#define no_inline __attribute__((noinline))
#define __maybe_unused __attribute__((unused))
#endif]=]
        "compiler attribute shims")

    _neograph_quickjs_replace_exact(_cutils_h
[=[/* WARNING: undefined if a = 0 */
static inline int clz32(unsigned int a)
{
    return __builtin_clz(a);
}

/* WARNING: undefined if a = 0 */
static inline int clz64(uint64_t a)
{
    return __builtin_clzll(a);
}

/* WARNING: undefined if a = 0 */
static inline int ctz32(unsigned int a)
{
    return __builtin_ctz(a);
}

/* WARNING: undefined if a = 0 */
static inline int ctz64(uint64_t a)
{
    return __builtin_ctzll(a);
}]=]
[=[/* WARNING: undefined if a = 0 */
static inline int clz32(unsigned int a)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse(&index, a);
    return 31 - (int)index;
#else
    return __builtin_clz(a);
#endif
}

/* WARNING: undefined if a = 0 */
static inline int clz64(uint64_t a)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse64(&index, a);
    return 63 - (int)index;
#else
    return __builtin_clzll(a);
#endif
}

/* WARNING: undefined if a = 0 */
static inline int ctz32(unsigned int a)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, a);
    return (int)index;
#else
    return __builtin_ctz(a);
#endif
}

/* WARNING: undefined if a = 0 */
static inline int ctz64(uint64_t a)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, a);
    return (int)index;
#else
    return __builtin_ctzll(a);
#endif
}]=]
        "bit-count intrinsics")

    _neograph_quickjs_replace_exact(_cutils_h
[=[struct __attribute__((packed)) packed_u64 {
    uint64_t v;
};

struct __attribute__((packed)) packed_u32 {
    uint32_t v;
};

struct __attribute__((packed)) packed_u16 {
    uint16_t v;
};

static inline uint64_t get_u64(const uint8_t *tab)
{
    return ((const struct packed_u64 *)tab)->v;
}

static inline int64_t get_i64(const uint8_t *tab)
{
    return (int64_t)((const struct packed_u64 *)tab)->v;
}

static inline void put_u64(uint8_t *tab, uint64_t val)
{
    ((struct packed_u64 *)tab)->v = val;
}

static inline uint32_t get_u32(const uint8_t *tab)
{
    return ((const struct packed_u32 *)tab)->v;
}

static inline int32_t get_i32(const uint8_t *tab)
{
    return (int32_t)((const struct packed_u32 *)tab)->v;
}

static inline void put_u32(uint8_t *tab, uint32_t val)
{
    ((struct packed_u32 *)tab)->v = val;
}

static inline uint32_t get_u16(const uint8_t *tab)
{
    return ((const struct packed_u16 *)tab)->v;
}

static inline int32_t get_i16(const uint8_t *tab)
{
    return (int16_t)((const struct packed_u16 *)tab)->v;
}

static inline void put_u16(uint8_t *tab, uint16_t val)
{
    ((struct packed_u16 *)tab)->v = val;
}]=]
[=[static inline uint64_t get_u64(const uint8_t *tab)
{
    uint64_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline int64_t get_i64(const uint8_t *tab)
{
    int64_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline void put_u64(uint8_t *tab, uint64_t val)
{
    memcpy(tab, &val, sizeof(val));
}

static inline uint32_t get_u32(const uint8_t *tab)
{
    uint32_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline int32_t get_i32(const uint8_t *tab)
{
    int32_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline void put_u32(uint8_t *tab, uint32_t val)
{
    memcpy(tab, &val, sizeof(val));
}

static inline uint32_t get_u16(const uint8_t *tab)
{
    uint16_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline int32_t get_i16(const uint8_t *tab)
{
    int16_t value;
    memcpy(&value, tab, sizeof(value));
    return value;
}

static inline void put_u16(uint8_t *tab, uint16_t val)
{
    memcpy(tab, &val, sizeof(val));
}]=]
        "unaligned integer accessors")

    file(WRITE "${output_dir}/cutils.h" "${_cutils_h}")

    file(READ "${quickjs_source_dir}/quickjs.c" _quickjs_c)
    _neograph_quickjs_replace_exact(_quickjs_c
        "#include <sys/time.h>"
[=[#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <malloc.h>
struct timeval {
    int64_t tv_sec;
    long tv_usec;
};
static int gettimeofday(struct timeval *tv, void *timezone_ignored)
{
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    uint64_t unix_ticks;
    (void)timezone_ignored;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    tv->tv_sec = (int64_t)(unix_ticks / UINT64_C(10000000));
    tv->tv_usec = (long)((unix_ticks % UINT64_C(10000000)) / UINT64_C(10));
    return 0;
}
#else
#include <sys/time.h>
#endif]=]
        "Windows wall clock")
    _neograph_quickjs_replace_exact(_quickjs_c
[=[#if defined(__EMSCRIPTEN__)
#define DIRECT_DISPATCH  0
#else
#define DIRECT_DISPATCH  1
#endif]=]
[=[#if defined(__EMSCRIPTEN__) || defined(_MSC_VER)
#define DIRECT_DISPATCH  0
#else
#define DIRECT_DISPATCH  1
#endif]=]
        "computed-goto dispatch guard")
    _neograph_quickjs_replace_exact(_quickjs_c
        "#if !defined(__EMSCRIPTEN__)\n#define CONFIG_ATOMICS"
        "#if !defined(__EMSCRIPTEN__) && !defined(_MSC_VER)\n#define CONFIG_ATOMICS"
        "POSIX atomics guard")
    _neograph_quickjs_replace_exact(_quickjs_c
        "__attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t user_data[];"
        "__declspec(align(JS_MALLOC_ALIGN)) uint8_t user_data[];"
        "allocation block alignment")
    _neograph_quickjs_replace_exact(_quickjs_c
        "__attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t blocks[];"
        "__declspec(align(JS_MALLOC_ALIGN)) uint8_t blocks[];"
        "allocation arena alignment")
    _neograph_quickjs_replace_exact(_quickjs_c
        "__attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t zero_size_block[sizeof(JSMallocBlockHeader)];"
        "__declspec(align(JS_MALLOC_ALIGN)) uint8_t zero_size_block[sizeof(JSMallocBlockHeader)];"
        "zero-size block alignment")
    _neograph_quickjs_replace_exact(_quickjs_c
        "return (uintptr_t)__builtin_frame_address(0);"
        "return (uintptr_t)_AddressOfReturnAddress();"
        "stack pointer intrinsic")
    file(WRITE "${output_dir}/quickjs.c"
        "#include \"quickjs-prefix.h\"\n${_quickjs_c}")

    file(READ "${quickjs_source_dir}/dtoa.c" _dtoa_c)
    _neograph_quickjs_replace_exact(_dtoa_c
        "#include <sys/time.h>"
        "#if !defined(_MSC_VER)\n#include <sys/time.h>\n#endif"
        "dtoa POSIX header guard")
    file(WRITE "${output_dir}/dtoa.c"
        "#include \"quickjs-prefix.h\"\n${_dtoa_c}")

    foreach(_source IN ITEMS cutils.c libregexp.c libunicode.c)
        file(READ "${quickjs_source_dir}/${_source}" _content)
        file(WRITE "${output_dir}/${_source}"
            "#include \"quickjs-prefix.h\"\n${_content}")
    endforeach()

    set(_generated_sources
        "${output_dir}/quickjs.c"
        "${output_dir}/cutils.c"
        "${output_dir}/dtoa.c"
        "${output_dir}/libregexp.c"
        "${output_dir}/libunicode.c")
    set(${output_variable} "${_generated_sources}" PARENT_SCOPE)
endfunction()
