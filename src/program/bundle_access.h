#pragma once

#include <neograph/program/bundle.h>

#include <vector>

namespace neograph::program::detail {

class ProgramBundleAccess {
public:
    /** A compiled bundle that also keeps the diagnostics the compiler returned. */
    static ProgramBundle compiled(ProgramBundleData data, std::vector<Diagnostic> compile_diagnostics);
};

}  // namespace neograph::program::detail
