if(NOT DEFINED NEOGRAPH_SOURCE_DIR OR NOT DEFINED NEOGRAPH_BINARY_DIR)
    message(FATAL_ERROR "NEOGRAPH_SOURCE_DIR and NEOGRAPH_BINARY_DIR are required")
endif()

include("${NEOGRAPH_SOURCE_DIR}/cmake/NeoGraphQuickJSMsvcPort.cmake")
set(_output "${NEOGRAPH_BINARY_DIR}/quickjs-msvc-port-verification")
neograph_prepare_quickjs_msvc_sources(
    "${NEOGRAPH_SOURCE_DIR}/deps/quickjs"
    "${_output}"
    _sources)

list(LENGTH _sources _source_count)
if(NOT _source_count EQUAL 5)
    message(FATAL_ERROR "expected five generated QuickJS sources, got ${_source_count}")
endif()

file(READ "${_output}/quickjs.c" _quickjs)
file(READ "${_output}/quickjs-msvc-port.h" _quickjs_header)
file(READ "${_output}/cutils.h" _cutils)
file(READ "${_output}/dtoa.c" _dtoa)
file(READ "${NEOGRAPH_SOURCE_DIR}/deps/quickjs/neograph/quickjs.h" _quickjs_wrapper)

function(_require text needle label)
    string(FIND "${text}" "${needle}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "generated MSVC port is missing ${label}")
    endif()
endfunction()

function(_forbid text needle label)
    string(FIND "${text}" "${needle}" _position)
    if(NOT _position EQUAL -1)
        message(FATAL_ERROR "generated MSVC port still contains ${label}")
    endif()
endfunction()

_require("${_quickjs}" "#include \"quickjs-prefix.h\"" "the private symbol prefix")
_require("${_quickjs}" "GetSystemTimeAsFileTime" "the Windows clock shim")
_require("${_quickjs}" "defined(__EMSCRIPTEN__) || defined(_MSC_VER)" "the computed-goto guard")
_require("${_quickjs}" "!defined(__EMSCRIPTEN__) && !defined(_MSC_VER)" "the POSIX atomics guard")
_require("${_quickjs}" "__declspec(align(JS_MALLOC_ALIGN))" "allocator alignment")
_require("${_quickjs}" "_AddressOfReturnAddress()" "the MSVC stack intrinsic")
_require("${_quickjs}" "#include \"quickjs-msvc-port.h\"" "the patched public header include")
_require("${_quickjs}" "static void __maybe_unused dump_token" "the portable debug helper attribute")
_require("${_quickjs}" "double d = INFINITY;" "the MSVC-safe infinity constant")
_require("${_quickjs}" "static int js_msvc_call_c_function_from_bytecode"
          "the MSVC bytecode C-function call helper")
_require("${_quickjs}" "if (!js_msvc_call_c_function_from_bytecode(ctx, call_argv[-1], JS_UNDEFINED,"
          "the MSVC bytecode call ABI")
_require("${_quickjs}" "if (!js_msvc_call_c_function_from_bytecode(ctx, call_argv[-1], call_argv[-2],"
          "the MSVC bytecode method-call ABI")
_forbid("${_quickjs}" "1.0 / 0.0" "a compile-time floating divide by zero")
_forbid("${_quickjs}" "(JSValue)argv[0]" "an invalid JSValue argument cast")
_forbid("${_quickjs}" "(JSValueConst)JS_NewBool" "an invalid JSValueConst constructor cast")
_require("${_quickjs_wrapper}" "#if defined(_MSC_VER)" "the wrapper MSVC branch")
_require("${_quickjs_wrapper}" "#include \"quickjs-msvc-port.h\""
         "the generated MSVC consumer header")
_require("${_quickjs_header}" "static inline JSValue js_msvc_make_value" "the MSVC JSValue constructor")
_require("${_quickjs_header}" "#if defined(__cplusplus)\nstatic inline JSCFunctionListEntry js_msvc_make_function_list_entry"
         "the C++17 function-list constructors")
_require("${_quickjs_header}" "js_msvc_make_cfunc_ ## cproto ## _entry"
         "the typed C++ special-function constructor")
_forbid("${_quickjs_header}" "#define JS_MKVAL(tag, val) (JSValue){" "a JSValue compound literal")
_forbid("${_quickjs_header}" "return (JSValue)v;" "a redundant JSValue struct cast")
_require("${_quickjs_header}" "JSCFunctionType ft = { 0 };\nft.generic_magic = func;"
         "the C++17-safe function union initialization")
_forbid("${_quickjs_header}" "JSCFunctionType ft = { .generic_magic = func };"
        "a designated union initializer")
_require("${_quickjs_header}"
         "#else\n#define JS_CFUNC_DEF(name, length, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, .u = { .func ="
         "the C function-list outer-union initializer")
_forbid("${_quickjs_header}" ".u.func ="
        "a flattened C function-list union designator")
_require("${_quickjs_header}" "#include <math.h>" "the NAN declaration")
_require("${_cutils}" "_BitScanReverse64" "the 64-bit count-leading-zero intrinsic")
_require("${_cutils}" "_BitScanForward64" "the 64-bit count-trailing-zero intrinsic")
_require("${_cutils}" "memcpy(&value, tab, sizeof(value))" "unaligned-safe reads")
_require("${_dtoa}" "#if !defined(_MSC_VER)\n#include <sys/time.h>" "the dtoa POSIX guard")

file(READ "${NEOGRAPH_SOURCE_DIR}/cmake/NeoGraphQuickJSMsvcPort.cmake" _port_source)
string(REPLACE "\r\n" "\n" _port_source "${_port_source}")
string(SHA256 _port_digest "${_port_source}")
file(READ "${NEOGRAPH_SOURCE_DIR}/include/neograph/program/source.h" _source_header)
_require("${_source_header}" "${_port_digest}" "the durable MSVC port identity")
