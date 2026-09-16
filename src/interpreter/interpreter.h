#pragma once

#include "ir/ir.h"

// M2: the correctness oracle. Walks the IR directly using
// LithonValue, no optimization, no JIT. Every later compiler
// decision gets checked against this interpreter's output.
//
// This first slice runs blocks in file order with no branching --
// Branch/Jump/Phi aren't implemented until a test program actually
// needs them.

namespace lithon::interp {

// Executes module's "main" function. print() output goes to stdout.
// Throws std::runtime_error on any unimplemented opcode or on
// referencing an undefined variable/value.
void run_main(const lithon::ir::Module& module);

} // namespace lithon::interp
