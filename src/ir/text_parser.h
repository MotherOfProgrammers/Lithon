#pragma once

#include <string>
#include "ir.h"

// Parses the IR text format (the same format your frontend will
// eventually emit) into a lithon::ir::Module. This is scaffolding
// for M1/M2 -- it lets C++ consume IR text without a native Lithon
// parser yet (that's M10).

namespace lithon::ir {

// Throws std::runtime_error with a descriptive message on malformed input.
Module parse_ir_text(const std::string& text);

} // namespace lithon::ir
