// Accumulator unrolling (accumulator_unroll in optimize.h): a counted
// reduction's single loop-carried accumulator is split into N partials, one
// per jammed body copy, and summed after the loop.
//
// The positive cases are checked end to end -- optimize, compile, execute,
// compare the result -- because the transform can be *correct* at the IR
// level and still emit a wrong sum if the partials are mis-promoted or the
// exit sum is placed before the tail loop. `--no-accum-unroll` (factor 1) is
// exercised on the same input as the reference, and the code-size check is
// what proves the pass actually did work rather than quietly declining.
//
// The negative cases pin each precondition, so loosening the match later has
// to be a deliberate edit to this file.

#include "compile_function.h"
#include "ir/ir.h"
#include "ir/text_parser.h"
#include "exec_memory.h"

#include <cstdio>
#include <string>

using namespace lithon::ir;
using namespace lithon::jit;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// The canonical `total += i` counted loop, `for i in range(n)`. `step` is the
// induction step (1 is the shape the pass needs); `second_reduction` adds a
// second accumulator to the body to force a decline.
static std::string counted_ir(int n, int step = 1, bool second_reduction = false) {
    std::string s =
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 0\n"
        "    store total, %0\n";
    if (second_reduction) s += "    store other, %0\n";
    s += "    %1 = const_i64 " + std::to_string(n) + "\n"
         "    %2 = const_i64 0\n"
         "    store i, %2\n"
         "    jump block1\n"
         "block1:\n"
         "    %3 = load i\n"
         "    %4 = lt %3, %1\n"
         "    branch %4, block2, block3\n"
         "block2:\n"
         "    %5 = load total\n"
         "    %6 = load i\n"
         "    %7 = add %5, %6\n"
         "    store total, %7\n";
    if (second_reduction)
        s += "    %20 = load other\n"
             "    %21 = load i\n"
             "    %22 = add %20, %21\n"
             "    store other, %22\n";
    s += "    %8 = load i\n"
         "    %9 = const_i64 " + std::to_string(step) + "\n"
         "    %10 = add %8, %9\n"
         "    store i, %10\n"
         "    jump block1\n"
         "block3:\n"
         "    %11 = load total\n";
    if (second_reduction)
        s += "    %23 = load other\n"
             "    %24 = add %11, %23\n"
             "    return %24\n";
    else
        s += "    return %11\n";
    return s;
}

// Same loop but the bound is a variable loaded in the preheader, so both
// operands of the header test are Loads (`for i in range(n)` with n a name).
static std::string variable_bound_ir(int n) {
    return std::string(
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 ") + std::to_string(n) +
        "\n"
        "    store n, %0\n"
        "    %1 = const_i64 0\n"
        "    store total, %1\n"
        "    %2 = load n\n"
        "    %3 = const_i64 0\n"
        "    store i, %3\n"
        "    jump block1\n"
        "block1:\n"
        "    %4 = load i\n"
        "    %5 = lt %4, %2\n"
        "    branch %5, block2, block3\n"
        "block2:\n"
        "    %6 = load total\n"
        "    %7 = load i\n"
        "    %8 = add %6, %7\n"
        "    store total, %8\n"
        "    %9 = load i\n"
        "    %10 = const_i64 1\n"
        "    %11 = add %9, %10\n"
        "    store i, %11\n"
        "    jump block1\n"
        "block3:\n"
        "    %12 = load total\n"
        "    return %12\n";
}

// `total += i * i`, to prove accumulator unrolling composes with strength
// reduction (the multiply becomes a running add before this pass runs).
static std::string square_ir(int n) {
    return std::string(
        "function f():\n"
        "block0:\n"
        "    %0 = const_i64 0\n"
        "    store total, %0\n"
        "    %1 = const_i64 ") + std::to_string(n) +
        "\n"
        "    %2 = const_i64 0\n"
        "    store i, %2\n"
        "    jump block1\n"
        "block1:\n"
        "    %3 = load i\n"
        "    %4 = lt %3, %1\n"
        "    branch %4, block2, block3\n"
        "block2:\n"
        "    %5 = load total\n"
        "    %6 = load i\n"
        "    %7 = load i\n"
        "    %8 = mul %6, %7\n"
        "    %9 = add %5, %8\n"
        "    store total, %9\n"
        "    %10 = load i\n"
        "    %11 = const_i64 1\n"
        "    %12 = add %10, %11\n"
        "    store i, %12\n"
        "    jump block1\n"
        "block3:\n"
        "    %13 = load total\n"
        "    return %13\n";
}

struct Result {
    int64_t value = 0;
    int accum = 0;
    size_t code_size = 0;
};

// Optimize with `factor` (1 disables), then compile the ALREADY optimized IR
// (options.optimize = false) so the pass runs exactly once and stats are not
// polluted by a second application inside compile_module.
static Result run_ir(const std::string& text, int factor = 4) {
    Module m = parse_ir_text(text);
    OptimizePasses passes;
    passes.accum_unroll = factor;
    OptimizeStats stats = optimize_function(m.functions[0], passes);
    CompileOptions options;
    options.optimize = false;
    CompiledModule compiled = compile_module(m, options);
    auto it = compiled.function_offset.find(m.functions[0].name);
    ExecutableBuffer mem(compiled.code);
    auto fn = mem.entry<int64_t (*)()>(it->second);
    Result r;
    r.value = fn();
    r.accum = stats.accum_unrolled;
    r.code_size = compiled.code.size();
    return r;
}

int main() {
    // Trip counts straddling the jam factor (4): the main loop handles the
    // n - n%4 elements in chunks of 4 and the untouched body handles the
    // remainder, so 0,1,2,3 and 4,5,6,7 and a long one all have to be exact.
    for (int n : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100}) {
        Result r = run_ir(counted_ir(n));
        long long expect = (long long)n * (n - 1) / 2;
        char what[96];
        std::snprintf(what, sizeof what, "sum 0..%d result", n - 1);
        check(r.value == expect, what);
        std::snprintf(what, sizeof what, "sum 0..%d rewritten", n - 1);
        check(r.accum == 1, what);
    }

    // The off switch must both decline and still be correct.
    {
        Result r = run_ir(counted_ir(9), 1);
        check(r.value == 36, "accum disabled: result");
        check(r.accum == 0, "accum disabled: declined");
    }

    // The rewrite is not a no-op: splitting into four jams four body copies
    // plus the exit sum, so the emitted code must grow.
    {
        Result on = run_ir(counted_ir(9), 4);
        Result off = run_ir(counted_ir(9), 1);
        std::printf("  code size: accum-on=%zu accum-off=%zu\n", on.code_size, off.code_size);
        check(on.code_size > off.code_size, "accum unroll grows the emitted code");
    }

    // Structure of the jammed block: ALL four copies run, then one terminator
    // re-tests the chunk limit. A jump after every copy (an earlier bug) makes
    // copies 1..3 unreachable and silently leaves the chain unsplit -- code
    // still grows and the result is still correct, so only this shape check
    // catches it.
    {
        Module m = parse_ir_text(counted_ir(9));
        OptimizePasses passes;
        passes.accum_unroll = 4;
        optimize_function(m.functions[0], passes);
        const BasicBlock* bmain = nullptr;
        for (const auto& b : m.functions[0].blocks)
            if (b.label.rfind("__accum_b", 0) == 0) bmain = &b;
        check(bmain != nullptr, "jammed block exists");
        if (bmain) {
            int jumps = 0, partial_stores = 0;
            size_t last_jump = 0, last_store = 0;
            for (size_t i = 0; i < bmain->instrs.size(); ++i) {
                const Instr& in = bmain->instrs[i];
                if (in.op == Op::Jump) { ++jumps; last_jump = i; }
                if (in.op == Op::Store && in.name.rfind("__accum", 0) == 0 &&
                    in.name.rfind("__accum_h", 0) != 0 && in.name.rfind("__accum_b", 0) != 0) {
                    ++partial_stores; last_store = i;
                }
            }
            check(jumps == 1, "jammed block has exactly one terminator");
            check(last_jump == bmain->instrs.size() - 1, "jammed block terminator is last");
            check(partial_stores == 4, "jammed block updates all four partials");
            check(last_store < last_jump, "every partial updates before the terminator");
        }
    }

    // Variable bound: both header operands are Loads, disambiguated by which
    // one the body unit-steps.
    {
        Result r = run_ir(variable_bound_ir(9));
        check(r.value == 36, "variable bound: result");
        check(r.accum == 1, "variable bound: rewritten");
    }

    // Composes with strength reduction: 0^2+...+36^2 = 16206.
    {
        Result r = run_ir(square_ir(37));
        check(r.value == 16206, "square reduction: result");
        check(r.accum == 1, "square reduction: rewritten");
    }

    // Negative: a non-unit induction step means the loop is not the canonical
    // counted loop, so the chunk test would be wrong.
    {
        Result r = run_ir(counted_ir(6, 2));
        check(r.accum == 0, "step-2 loop: declined");
    }

    // Negative: two reductions in one body. Splitting either one alone would
    // still be valid, but this pass deliberately handles only the single
    // accumulator shape, and the result must stay correct either way.
    {
        Result r = run_ir(counted_ir(7, 1, true));
        check(r.accum == 0, "two accumulators: declined");
        // a = 0+..+6 = 21, b = same = 21, returned as a+b = 42.
        check(r.value == 42, "two accumulators: result");
    }

    if (failures == 0) std::printf("optimize_accum_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
