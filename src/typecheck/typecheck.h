#pragma once

#include <string>
#include <vector>
#include "ir/ir.h"

// Static type-checking over the IR, per V1_SPEC.md section 0.6.
// This is Lithon's actual defining discipline -- mandatory
// annotations, sizing, overflow, conversions, branch agreement,
// function contracts -- ported from the original Python prototype
// (tools/typecheck.py) to walk ir::Module directly, so it can
// become a real stage in the compiler pipeline rather than a
// standalone script re-parsing source text.
//
// First slice (this file): 0.6.1, 0.6.4, 0.6.5 only, straight-line
// code (no branches). Widened incrementally, same as the Python
// prototype was.

namespace lithon::typecheck {

struct RCRError {
    std::string message;
};

// Returns an empty vector on success. Non-empty means the module is
// rejected -- every entry is a distinct RCR violation.
std::vector<RCRError> check_module(const lithon::ir::Module& module);

} // namespace lithon::typecheck
