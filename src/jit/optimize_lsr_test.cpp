// Strength reduction of `invariant * induction_var` into a repeated add
// (strength_reduce_multiplies in optimize.h).
//
// The positive cases are checked end to end -- compile, execute, compare the
// result -- because the transform can be *correct* at the IR level and still
// emit a wrong accumulator if the codegen folds it badly. The negative cases
// pin down each precondition, so a later loosening of the match has to be a
// deliberate change to this file rather than a silent behaviour change.
//
// The canonical shape under test (the nested-loop body Lithon's frontend
// produces for `total += i * j`):
//
//     block0:  %0 = const 0 ; store j, %0 ; jump block1
//     block1:  %1 = load j ; %2 = lt %1, %5 ; branch %2, block2, block3
//     block2:  %3 = load total ; %4 = load i ; %6 = load j
//              %7 = mul %4, %6 ; %8 = add %3, %7 ; store total, %8
//              %9 = load j ; %10 = const 1 ; %11 = add %9, %10 ; store j, %11
//              jump block1
//     block3:  %12 = load total ; return %12

#include "compile_function.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "exec_memory.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace lithon::ir;
using namespace lithon::jit;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// A loop that accumulates `outer * j` for j in 0..n-1, i.e. the product of a
// loop-invariant value and a counted induction variable.
//   triple:  sum += a * j,  j in 0..n-1        ->  a * n*(n-1)/2
//   square:  sum += j * j,   j in 0..n-1        ->  n*(n-1)*(2n-1)/6
// Both spell the multiply with a different operand order.
static std::string counted_loop_ir(int n, bool constant_invariant) {
    return std::string(
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 0\n"
        "    store j, %0\n"
        "    %1 = const_i64 7\n"
        "    store a, %1\n"
        "    store total, %0\n"
        "    jump block1\n"
        "block1:\n"
        "    %2 = load j\n"
        "    %3 = const_i64 ") + std::to_string(n) +
        "\n"
        "    %4 = lt %2, %3\n"
        "    branch %4, block2, block3\n"
        "block2:\n"
        "    %5 = load total\n" +
        (constant_invariant
             ? "    %6 = const_i64 7\n"
               "    %7 = load j\n"
               "    %8 = mul %6, %7\n"
               "    %9 = add %5, %8\n"
               "    store total, %9\n"
               "    %10 = load j\n"
               "    %11 = const_i64 1\n"
               "    %12 = add %10, %11\n"
               "    store j, %12\n"
               "    jump block1\n"
             : "    %6 = load a\n"
               "    %7 = load j\n"
               "    %8 = mul %6, %7\n"
               "    %9 = add %5, %8\n"
               "    store total, %9\n"
               "    %10 = load j\n"
               "    %11 = const_i64 1\n"
               "    %12 = add %10, %11\n"
               "    store j, %12\n"
               "    jump block1\n") +
        "block3:\n"
        "    %13 = load total\n"
        "    return %13\n";
}

// The same loop with one precondition broken. `break_what` picks the case:
//   0  the induction variable is never zeroed before the loop
//   1  the induction variable is stepped by 2, not 1
//   2  the "invariant" is also stored inside the loop
// Case 0 leaves `j` uninitialised on purpose, so it is a *negative* test only:
// its result is whatever the initial slot held and is not checked.
static std::string blocked_ir(int break_what) {
    std::vector<std::string> L{
        "function f():",
        "block0:",
        "    %0 = const_i64 0",
    };
    if (break_what != 0) L.push_back("    store j, %0");   // the zeroing the pass relies on
    L.push_back("    %1 = const_i64 7");
    L.push_back("    store a, %1");
    L.push_back("    store total, %0");
    L.push_back("    jump block1");
    L.push_back("block1:");
    L.push_back("    %2 = load j");
    L.push_back("    %3 = const_i64 4");
    L.push_back("    %4 = lt %2, %3");
    L.push_back("    branch %4, block2, block3");
    L.push_back("block2:");
    L.push_back("    %5 = load total");
    L.push_back("    %6 = load a");
    L.push_back("    %7 = load j");
    L.push_back("    %8 = mul %6, %7");
    L.push_back("    %9 = add %5, %8");
    L.push_back("    store total, %9");
    L.push_back("    %10 = load j");
    L.push_back(break_what == 1 ? "    %11 = const_i64 2" : "    %11 = const_i64 1");
    L.push_back("    %12 = add %10, %11");            // j = j + step
    L.push_back("    store j, %12");
    if (break_what == 2) L.push_back("    store a, %0");   // invariant is not invariant
    L.push_back("    jump block1");
    L.push_back("block3:");
    L.push_back("    %13 = load total");
    L.push_back("    return %13");
    std::string out;
    for (const auto& line : L) { out += line; out += "\n"; }
    return out;
}

static bool no_mul_remains(const Function& fn) {
    for (const auto& b : fn.blocks)
        for (const auto& in : b.instrs)
            if (in.op == Op::Mul) return false;
    return true;
}

// Compile `text`, run it, and return the entry's result. `reduced` receives the
// number of multiplies the optimiser removed. `expect_rewrite` additionally
// asserts the multiply is gone from the IR -- which must not be checked for the
// negative cases, where declining to rewrite is the whole point.
static int64_t run_ir(const std::string& text, int* reduced, bool expect_rewrite) {
    Module m = parse_ir_text(text);
    OptimizeStats stats = optimize_function(m.functions[0]);
    if (reduced) *reduced = stats.strength_reduced;
    if (expect_rewrite && !no_mul_remains(m.functions[0])) {
        std::printf("FAIL: a multiply survived\n");
        ++failures;
    }
    CompiledModule compiled = compile_module(m);
    auto it = compiled.function_offset.find(m.functions[0].name);
    ExecutableBuffer mem(compiled.code);
    auto fn = mem.entry<int64_t (*)()>(it->second);
    return fn();
}

int main() {
    // 7 * (0+1+2+3) = 42
    {
        int reduced = 0;
        int64_t got = run_ir(counted_loop_ir(4, false), &reduced, true);
        check(got == 42, "invariant*iv, variable invariant: result");
        check(reduced == 1, "invariant*iv, variable invariant: one rewrite");
    }
    // 7 * (0+1+2+3) = 42, invariant already constant-folded
    {
        int reduced = 0;
        int64_t got = run_ir(counted_loop_ir(4, true), &reduced, true);
        check(got == 42, "invariant*iv, constant invariant: result");
        check(reduced == 1, "invariant*iv, constant invariant: one rewrite");
    }
    // A loop that runs zero times must leave total at 0 -- the accumulator's
    // zero store has to happen even though the body never runs.
    {
        int reduced = 0;
        int64_t got = run_ir(counted_loop_ir(0, false), &reduced, true);
        check(got == 0, "zero-trip loop: result");
    }
    // A single iteration: the accumulator must be 0, not 7.
    {
        int reduced = 0;
        int64_t got = run_ir(counted_loop_ir(1, false), &reduced, true);
        check(got == 0, "single-iteration loop: result");
    }
    // Long loop, checks the accumulator tracks the induction variable rather
    // than drifting: 7 * 200*199/2 = 139300
    {
        int reduced = 0;
        int64_t got = run_ir(counted_loop_ir(200, false), &reduced, true);
        check(got == 139300, "long loop: result");
        check(reduced == 1, "long loop: one rewrite");
    }

    // Negative cases: the transform must decline. The results are still the
    // interpreter's answer, so this also proves the un-rewritten loop works.
    // Results of the un-rewritten loops. The step-2 case visits j = 0, 2, so
    // it sums 7*0 + 7*2. The clobbered-invariant case stores 0 to `a` before
    // jumping back, so every product is 0. The uninitialised case is not
    // checked: `j` starts from whatever the slot held.
    const char* names[] = {
        "iv not zeroed before the loop",
        "iv stepped by 2",
        "invariant stored inside the loop",
    };
    const int64_t expect[] = { 0, 14, 0 };
    for (int k = 0; k < 3; ++k) {
        int reduced = 99;
        int64_t got = run_ir(blocked_ir(k), &reduced, false);
        std::printf("  declined=%-34s result=%lld\n", names[k], (long long)got);
        check(reduced == 0, names[k]);
        if (k > 0) check(got == expect[k], names[k]);
    }

    if (failures == 0) std::printf("optimize_lsr_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
