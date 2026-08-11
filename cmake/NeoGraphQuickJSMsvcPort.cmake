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
        cutils.h
        quickjs.h)
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
    file(READ "${quickjs_source_dir}/quickjs.h" _quickjs_h)
    _neograph_quickjs_replace_exact(_quickjs_h
[=[#define JS_MKVAL(tag, val) (JSValue){ (JSValueUnion){ .uint64 = (uint32_t)(val) }, tag }
#define JS_MKPTR(tag, p) (JSValue){ (JSValueUnion){ .ptr = p }, tag }

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN (JSValue){ .u.float64 = JS_FLOAT64_NAN, JS_TAG_FLOAT64 }]=]
[=[static inline JSValue js_msvc_make_value(int64_t tag, uint32_t val)
{
    JSValue value = { 0 };
    value.u.uint64 = val;
    value.tag = tag;
    return value;
}

static inline JSValue js_msvc_make_pointer(int64_t tag, void *ptr)
{
    JSValue value = { 0 };
    value.u.ptr = ptr;
    value.tag = tag;
    return value;
}

static inline JSValue js_msvc_make_nan(void)
{
    JSValue value = { 0 };
    value.u.float64 = JS_FLOAT64_NAN;
    value.tag = JS_TAG_FLOAT64;
    return value;
}

#define JS_MKVAL(tag, val) js_msvc_make_value((tag), (uint32_t)(val))
#define JS_MKPTR(tag, p) js_msvc_make_pointer((tag), (void *)(p))

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

#define JS_NAN js_msvc_make_nan()]=]
        "MSVC JSValue constructors")
    _neograph_quickjs_replace_exact(_quickjs_h
        "return (JSValue)v;"
        "return v;"
        "redundant JSValue struct casts")
    _neograph_quickjs_replace_exact(_quickjs_h
[=[#define JS_CFUNC_DEF(name, length, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } }
#define JS_CFUNC_MAGIC_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } } }
#define JS_CFUNC_SPECIAL_DEF(name, length, cproto, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func = { length, JS_CFUNC_ ## cproto, { .cproto = func1 } } } }
#define JS_ITERATOR_NEXT_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u = { .func = { length, JS_CFUNC_iterator_next, { .iterator_next = func1 } } } }
#define JS_CGETSET_DEF(name, fgetter, fsetter) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, .u = { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } }
#define JS_CGETSET_MAGIC_DEF(name, fgetter, fsetter, magic) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, magic, .u = { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } }
#define JS_PROP_STRING_DEF(name, cstr, prop_flags) { name, prop_flags, JS_DEF_PROP_STRING, 0, .u = { .str = cstr } }
#define JS_PROP_INT32_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT32, 0, .u = { .i32 = val } }
#define JS_PROP_INT64_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT64, 0, .u = { .i64 = val } }
#define JS_PROP_DOUBLE_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, .u = { .f64 = val } }
#define JS_PROP_UNDEFINED_DEF(name, prop_flags) { name, prop_flags, JS_DEF_PROP_UNDEFINED, 0, .u = { .i32 = 0 } }
#define JS_PROP_ATOM_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_ATOM, 0, .u = { .i32 = val } }
#define JS_PROP_BOOL_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_BOOL, 0, .u = { .i32 = val } }
#define JS_OBJECT_DEF(name, tab, len, prop_flags) { name, prop_flags, JS_DEF_OBJECT, 0, .u = { .prop_list = { tab, len } } }
#define JS_ALIAS_DEF(name, from) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, -1 } } }
#define JS_ALIAS_BASE_DEF(name, from, base) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u = { .alias = { from, base } } }]=]
[=[#define JS_CFUNC_DEF(name, length, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u.func = { length, JS_CFUNC_generic, { .generic = func1 } } }
#define JS_CFUNC_MAGIC_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u.func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } }
#define JS_CFUNC_SPECIAL_DEF(name, length, cproto, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u.func = { length, JS_CFUNC_ ## cproto, { .cproto = func1 } } }
#define JS_ITERATOR_NEXT_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, .u.func = { length, JS_CFUNC_iterator_next, { .iterator_next = func1 } } }
#define JS_CGETSET_DEF(name, fgetter, fsetter) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, .u.getset.get.getter = fgetter, .u.getset.set.setter = fsetter }
#define JS_CGETSET_MAGIC_DEF(name, fgetter, fsetter, magic) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, magic, .u.getset.get.getter_magic = fgetter, .u.getset.set.setter_magic = fsetter }
#define JS_PROP_STRING_DEF(name, cstr, prop_flags) { name, prop_flags, JS_DEF_PROP_STRING, 0, .u.str = cstr }
#define JS_PROP_INT32_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT32, 0, .u.i32 = val }
#define JS_PROP_INT64_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT64, 0, .u.i64 = val }
#define JS_PROP_DOUBLE_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, .u.f64 = val }
#define JS_PROP_UNDEFINED_DEF(name, prop_flags) { name, prop_flags, JS_DEF_PROP_UNDEFINED, 0, .u.i32 = 0 }
#define JS_PROP_ATOM_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_ATOM, 0, .u.i32 = val }
#define JS_PROP_BOOL_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_BOOL, 0, .u.i32 = val }
#define JS_OBJECT_DEF(name, tab, len, prop_flags) { name, prop_flags, JS_DEF_OBJECT, 0, .u.prop_list = { tab, len } }
#define JS_ALIAS_DEF(name, from) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u.alias = { from, -1 } }
#define JS_ALIAS_BASE_DEF(name, from, base) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, .u.alias = { from, base } }]=]
        "MSVC static function-list initializers")
    file(WRITE "${output_dir}/quickjs-msvc-port.h" "${_quickjs_h}")


    file(READ "${quickjs_source_dir}/quickjs.c" _quickjs_c)
    _neograph_quickjs_replace_exact(_quickjs_c
        "#include \"quickjs.h\""
        "#include \"quickjs-msvc-port.h\""
        "patched QuickJS header include")
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
    _neograph_quickjs_replace_exact(_quickjs_c
        "1.0 / 0.0"
        "INFINITY"
        "MSVC infinity constants")
    _neograph_quickjs_replace_exact(_quickjs_c
        "static void __attribute((unused)) dump_token"
        "static void __maybe_unused dump_token"
        "debug helper attribute")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)name"
        "name"
        "symbol-name struct casts")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)brand"
        "brand"
        "brand struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)func_obj"
        "func_obj"
        "function-object struct casts")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)args[0]"
        "args[0]"
        "argument zero struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)args[1]"
        "args[1]"
        "argument one struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "return (JSValue)val;"
        "return val;"
        "return-value struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "return (JSValueConst)map_normalize_key(ctx, (JSValue)key);"
        "return map_normalize_key(ctx, key);"
        "normalized-key struct casts")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)this_val"
        "this_val"
        "this-value struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)argv[0]"
        "argv[0]"
        "argv zero struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValue)argv[1]"
        "argv[1]"
        "argv one struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValueConst)JS_NewUint32(ctx, array_length)"
        "JS_NewUint32(ctx, array_length)"
        "uint32 struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValueConst)str"
        "str"
        "string struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValueConst)JS_NewInt32(ctx, index)"
        "JS_NewInt32(ctx, index)"
        "int32 struct cast")
    _neograph_quickjs_replace_exact(_quickjs_c
        "(JSValueConst)JS_NewBool(ctx, done)"
        "JS_NewBool(ctx, done)"
        "boolean struct cast")
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
