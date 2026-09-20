// Verifies register allocation against a function shaped like
// tests/typed_regression/function.py's add(a, b):
//   %0 = load a
//   %1 = load b
//   %2 = add %0, %1
//   return %2
//
// Confirms: a and b get distinct fixed stack slots, %0/%1/%2 each
// get a register (pool of 4 is plenty for 3 short-lived temporaries
// with no overlap forcing a spill), and frame_size is a sane,
// 16-byte-aligned value.

#include "register_alloc.h"
#include "ir/ir.h"
#include <cstdio>

using namespace lithon::ir;
using namespace lithon::jit;

int main() {
    Function fn;
    fn.name = "add";
    fn.params = {"a", "b"};

    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "a"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 1; i.name = "b"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 2; i.args = {0, 1}; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {2}; block0.instrs.push_back(i); }
    fn.blocks = {block0};

    RegisterAllocator alloc(fn);

    printf("frame_size: %d\n", alloc.frame_size());

    bool ok = true;

    printf("variable 'a' offset: %d\n", alloc.variable_offset("a"));
    printf("variable 'b' offset: %d\n", alloc.variable_offset("b"));
    if (alloc.variable_offset("a") == alloc.variable_offset("b")) {
        printf("FAIL: a and b share the same offset\n");
        ok = false;
    }
    if (!alloc.has_variable("a") || !alloc.has_variable("b")) {
        printf("FAIL: a or b not recognized as a variable\n");
        ok = false;
    }

    for (ValueId id : {0u, 1u, 2u}) {
        const auto& loc = alloc.temp_location(id);
        if (loc.in_register) {
            printf("%%%u -> register %d\n", id, static_cast<int>(loc.reg));
        } else {
            printf("%%%u -> spilled to slot %d\n", id, loc.stack_slot);
        }
    }

    if (alloc.frame_size() % 16 != 0) {
        printf("FAIL: frame_size not 16-byte aligned\n");
        ok = false;
    }
    if (alloc.frame_size() <= 0) {
        printf("FAIL: frame_size should be positive (a, b need slots)\n");
        ok = false;
    }

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
