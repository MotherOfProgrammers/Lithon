// Round-trip verifier for mem2reg (SSA construction).
//
// mem2reg is not wired into the backend -- no codegen path emits Op::Phi -- so
// correctness is shown end to end: each program is compiled and run in its
// original memory form, then promoted to SSA (mem2reg), structurally checked
// (validate_ssa: one operand per predecessor edge, none undefined), lowered
// back to memory form (lower_ssa_to_memory), and run again. Both executions
// must agree with each other and with the expected value. A transform that
// renamed a value wrongly or dropped a phi would change the second result.
//
// The programs deliberately cover the three shapes that make promotion
// non-trivial: a diamond (join needs a phi), a loop (header phi + exit),
// and a branch with no else (join phi with a value only from one side).

#include "compile_function.h"
#include "exec_memory.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "ssa.h"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace lithon::ir;
using namespace lithon::jit;

typedef int64_t (*IntFunc)();

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

static int64_t run_f(Module m) {
    CompileOptions opt;
    CompiledModule c = compile_module(m, opt);
    auto it = c.function_offset.find("f");
    if (it == c.function_offset.end()) throw std::runtime_error("function f not compiled");
    ExecutableBuffer mem(c.code);
    auto fn = reinterpret_cast<IntFunc>(
        reinterpret_cast<uint8_t*>(mem.data()) + it->second);
    return fn();
}

static size_t count_loads_stores(const Function& fn) {
    size_t n = 0;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.instrs)
            if (in.op == Op::Load || in.op == Op::Store) ++n;
    return n;
}

static void roundtrip(const char* name, const std::string& ir, int64_t expect) {
    // Ground truth: the untouched memory-form program compiled directly.
    const int64_t original = run_f(parse_ir_text(ir));

    // Promote to SSA and check the structure before lowering.
    Module ssa = parse_ir_text(ir);
    SsaStats stats = mem2reg(ssa.functions[0]);
    std::string err;
    check(validate_ssa(ssa.functions[0], &err),
          std::string(name) + ": validate_ssa (" + err + ")");
    check(stats.phis_materialized > 0, std::string(name) + ": materialized phis");
    check(count_loads_stores(ssa.functions[0]) == 0,
          std::string(name) + ": all loads/stores promoted");

    // Lower back to memory form and run it.
    lower_ssa_to_memory(ssa.functions[0]);
    const int64_t lowered = run_f(ssa);

    check(original == expect, std::string(name) + ": original == expected");
    check(lowered == expect, std::string(name) + ": lowered == expected");
    check(original == lowered, std::string(name) + ": round trip stable");
}

int main() {
    // Diamond: both arms store x; the join needs a phi.
    roundtrip("diamond",
              "function f():\n"
              "block0:\n"
              "    %0 = const_i64 1\n"
              "    store x, %0\n"
              "    %1 = const_i64 3\n"
              "    %2 = const_i64 4\n"
              "    %3 = lt %1, %2\n"
              "    branch %3, block1, block2\n"
              "block1:\n"
              "    %4 = const_i64 2\n"
              "    store x, %4\n"
              "    jump block3\n"
              "block2:\n"
              "    %5 = const_i64 5\n"
              "    store x, %5\n"
              "    jump block3\n"
              "block3:\n"
              "    %6 = load x\n"
              "    %7 = const_i64 8\n"
              "    %8 = add %6, %7\n"
              "    return %8\n",
              10);

    // Counted loop: x accumulates 0+1+2+3+4, with a header phi.
    roundtrip("loop",
              "function f():\n"
              "block0:\n"
              "    %0 = const_i64 0\n"
              "    store x, %0\n"
              "    %1 = const_i64 0\n"
              "    store i, %1\n"
              "    %2 = const_i64 5\n"
              "    store n, %2\n"
              "    jump block1\n"
              "block1:\n"
              "    %3 = load i\n"
              "    %4 = load n\n"
              "    %5 = lt %3, %4\n"
              "    branch %5, block2, block3\n"
              "block2:\n"
              "    %6 = load x\n"
              "    %7 = load i\n"
              "    %8 = add %6, %7\n"
              "    store x, %8\n"
              "    %9 = load i\n"
              "    %10 = const_i64 1\n"
              "    %11 = add %9, %10\n"
              "    store i, %11\n"
              "    jump block1\n"
              "block3:\n"
              "    %12 = load x\n"
              "    return %12\n",
              10);

    // Branch without an else: the phi at the join takes the pre-branch value
    // from one edge and the updated value from the other.
    roundtrip("if_no_else",
              "function f():\n"
              "block0:\n"
              "    %0 = const_i64 7\n"
              "    store x, %0\n"
              "    %1 = const_i64 1\n"
              "    %2 = const_i64 2\n"
              "    %3 = lt %1, %2\n"
              "    branch %3, block1, block2\n"
              "block1:\n"
              "    %4 = const_i64 100\n"
              "    store x, %4\n"
              "    jump block2\n"
              "block2:\n"
              "    %5 = load x\n"
              "    return %5\n",
              100);

    // No control flow: promotion is a pure load/store elimination, no phi.
    {
        Module m = parse_ir_text(
            "function f():\n"
            "block0:\n"
            "    %0 = const_i64 5\n"
            "    store x, %0\n"
            "    %1 = load x\n"
            "    %2 = const_i64 3\n"
            "    %3 = add %1, %2\n"
            "    return %3\n");
        const int64_t before = run_f(m);
        SsaStats stats = mem2reg(m.functions[0]);
        std::string err;
        check(validate_ssa(m.functions[0], &err), "straightline: validate_ssa (" + err + ")");
        check(stats.phis_materialized == 0, "straightline: no phi");
        check(stats.loads_removed == 1 && stats.stores_removed == 1,
              "straightline: load + store removed");
        check(count_loads_stores(m.functions[0]) == 0, "straightline: promoted");
        lower_ssa_to_memory(m.functions[0]);
        const int64_t after = run_f(m);
        check(before == 8 && after == 8, "straightline: 8 == 8 == 8");
    }

    // The shape a conditional *expression* compiles to: the temporary is
    // defined in both arms and nowhere else, so the old "must be stored in the
    // entry block" rule refused it -- and refused it on exactly the program
    // that most needs SSA.
    roundtrip("ifexpr",
              "function f():\n"
              "block0:\n"
              "    %0 = const_i64 1\n"
              "    %1 = const_i64 2\n"
              "    %2 = lt %0, %1\n"
              "    branch %2, block1, block2\n"
              "block1:\n"
              "    %3 = const_i64 10\n"
              "    store t, %3\n"
              "    jump block3\n"
              "block2:\n"
              "    %4 = const_i64 20\n"
              "    store t, %4\n"
              "    jump block3\n"
              "block3:\n"
              "    %5 = load t\n"
              "    return %5\n",
              10);

    // The negative half of the same rule: read at the join, defined on only one
    // side. There is no value for the other edge to pass, so promotion must
    // decline -- the interpreter throws on this program, and inventing an
    // operand would turn an error into a silent wrong answer.
    {
        Module m = parse_ir_text(
            "function f():\n"
            "block0:\n"
            "    %0 = const_i64 1\n"
            "    %1 = const_i64 2\n"
            "    %2 = lt %0, %1\n"
            "    branch %2, block1, block2\n"
            "block1:\n"
            "    %3 = const_i64 7\n"
            "    store t, %3\n"
            "    jump block2\n"
            "block2:\n"
            "    %4 = load t\n"
            "    return %4\n");
        SsaStats stats = mem2reg(m.functions[0]);
        check(stats.vars_promoted == 0, "one_sided: not promoted");
        check(stats.vars_declined == 1, "one_sided: reported as declined");
        check(count_loads_stores(m.functions[0]) == 2, "one_sided: load + store kept");
    }

    // A value assigned in the preheader and read inside the loop must stay
    // promotable: "definitely assigned" has to reach its greatest fixpoint, or
    // the header's meet with the back edge throws the value away.
    roundtrip("preheader_into_loop",
              "function f():\n"
              "block0:\n"
              "    %0 = const_i64 7\n"
              "    store k, %0\n"
              "    %1 = const_i64 0\n"
              "    store i, %1\n"
              "    %2 = const_i64 4\n"
              "    store n, %2\n"
              "    jump block1\n"
              "block1:\n"
              "    %3 = load i\n"
              "    %4 = load n\n"
              "    %5 = lt %3, %4\n"
              "    branch %5, block2, block3\n"
              "block2:\n"
              "    %6 = load k\n"
              "    %7 = load i\n"
              "    %8 = add %6, %7\n"
              "    store k, %8\n"
              "    %9 = load i\n"
              "    %10 = const_i64 1\n"
              "    %11 = add %9, %10\n"
              "    store i, %11\n"
              "    jump block1\n"
              "block3:\n"
              "    %12 = load k\n"
              "    return %12\n",
              7 + 1 + 2 + 3);

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: mem2reg round-trips diamond, loop, if/else-less, straightline,\n"
                "      ifexpr, preheader-into-loop; declines one-sided definitions\n");
    return 0;
}
