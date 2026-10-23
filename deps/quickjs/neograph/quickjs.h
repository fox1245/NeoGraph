#ifndef NEOGRAPH_QUICKJS_WRAPPER_H
#define NEOGRAPH_QUICKJS_WRAPPER_H

#include "quickjs-prefix.h"
#if defined(_MSC_VER)
#include "quickjs-msvc-port.h"
#else
#include "../quickjs.h"
#endif

#if defined(__EMSCRIPTEN__)
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* NeoGraph patch (deps/quickjs/NEOGRAPH_PROVENANCE): the stack bytes between the
   stack top and the limit that JS_SetMaxStackSize() and JS_UpdateStackTop() left.
   A WebAssembly build honours a size only as far as its shadow stack holds, so
   this is the size in force. */
size_t JS_GetAppliedMaxStackSize(JSRuntime *rt);
#ifdef __cplusplus
}
#endif
#endif

#endif
