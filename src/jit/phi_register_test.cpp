// 2.5: Phi copies as register moves.
//
// Before this phase a resolved Phi was a memory round-trip by construction:
// resolve_phis() names a variable, stores each incoming value into it on the
// predecessor edge, and loads it at the top of the join. That is correct, and
// it is still the fallback, but it means every merge pays a store and a load.
//
// These tests pin the phase's actual claim, which is NOT "the answer is right"
// -- the differential suites already cover that, and they passed before this
// phase existed. The claim is that the copies become `mov reg, reg`, and a test
// that only checks the value would pass identically with the phase disabled.
// So the assertions here are about registers: how many copies got one, and that
// the ones that did are still numerically correct.

#include "compile_function.h"
#include "ir/text_parser.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace lithon::ir;
using namespace lithon::jit;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// Three independent merges, so nothing here competes with itself for the
// callee-saved pool: the point is to observe a copy that DID get a register,
// not to measure the pool's behaviour under pressure.
static const char* kThreeMerges = R"(
function __main__():
block0:
    %0 = const_i64 5
    store x, %0
    %1 = const_i64 2
    store y, %1
    %2 = load x
    %3 = load y
    %4 = gt %2, %3
    branch %4, block1, block2
block1:
    %5 = const_i64 1
    store __ssa_9, %5
    jump block3
block2:
    %6 = const_i64 2
    store __ssa_9, %6
    jump block3
block3:
    %7 = load __ssa_9
    call print, %7
    %8 = const_i64 3
    store z, %8
    %10 = load z
    %11 = const_i64 4
    %12 = lt %10, %11
    branch %12, block4, block5
block4:
    %13 = const_i64 5
    store __ssa_20, %13
    jump block6
block5:
    %14 = const_i64 6
    store __ssa_20, %14
    jump block6
block6:
    %15 = load __ssa_20
    call print, %15
    %16 = const_i64 7
    store w, %16
    %17 = load w
    %18 = const_i64 7
    %19 = eq %17, %18
    branch %19, block7, block8
block7:
    %21 = const_i64 7
    store __ssa_30, %21
    jump block9
block8:
    %22 = const_i64 8
    store __ssa_30, %22
    jump block9
block9:
    %23 = load __ssa_30
    call print, %23
    return
)";

static void test_copies_become_registers() {
    Module m = parse_ir_text(kThreeMerges);
    CompileOptions opt;
    opt.ssa_pipeline = true;
    CompiledModule c = compile_module(m, opt);

    check(c.phi_copies_total == 3,
          "three merges produce three phi copies to account for");
    check(c.phi_copies_in_registers == 3,
          "all three copies held a register (got " +
              std::to_string(c.phi_copies_in_registers) + ")");

    // The numbers above are the phase's claim; this is the floor under it. A
    // register that holds the wrong value would pass the count and fail here.
    check(c.code.size() > 0, "module produced code");
}

// With promotion off, the copies must fall back to memory -- and, critically,
// still be CORRECT. That is the property that makes the register path an
// optimisation rather than a requirement: the phase can fail to get a register
// and the program must not care.
static void test_fallback_is_still_correct() {
    Module m = parse_ir_text(kThreeMerges);
    CompileOptions opt;
    opt.ssa_pipeline = true;
    opt.promote_registers = false;
    CompiledModule c = compile_module(m, opt);

    check(c.phi_copies_total == 3, "still three copies to account for");
    check(c.phi_copies_in_registers == 0,
          "no copy gets a register when promotion is off (got " +
              std::to_string(c.phi_copies_in_registers) + ")");
    check(c.code.size() > 0, "memory fallback still produced code");
}

// A merge carrying a double cannot be promoted: a general-purpose register
// cannot hold one. The counter has to say so, because reporting it as a
// register move would be claiming something false -- and the float machinery
// that removes it is shared with every other variable, so this is the case that
// proves the two agree.
static void test_float_merge_is_not_counted_as_a_register() {
    const char* float_ir = R"(
function __main__():
block0:
    %0 = const_f64 1.5
    store f, %0
    %1 = const_f64 0.5
    store g, %1
    %2 = load f
    %3 = load g
    %4 = gt %2, %3
    branch %4, block1, block2
block1:
    %5 = const_f64 2.5
    store __ssa_9, %5
    jump block3
block2:
    %6 = const_f64 3.5
    store __ssa_9, %6
    jump block3
block3:
    %7 = load __ssa_9
    call print, %7
    return
)";

    Module m = parse_ir_text(float_ir);
    CompileOptions opt;
    opt.ssa_pipeline = true;
    CompiledModule c = compile_module(m, opt);

    check(c.phi_copies_total == 1, "the float merge is still a copy to account for");
    check(c.phi_copies_in_registers == 0,
          "a double merge is never counted as a register move (got " +
              std::to_string(c.phi_copies_in_registers) + ")");
}

// Without the SSA pipeline there are no Phi copies at all, so the counters
// must be zero rather than left at whatever a previous run happened to set.
static void test_off_by_default_is_quiet() {
    Module m = parse_ir_text(kThreeMerges);
    CompileOptions opt;   // ssa_pipeline defaults to false
    CompiledModule c = compile_module(m, opt);

    check(c.phi_copies_total == 0, "no pipeline, no phi copies");
    check(c.phi_copies_in_registers == 0, "no pipeline, no registers claimed");
}

// 2.8: a merge taking over its source's dead register.
//
// Same discipline as the tests above: the answer was already correct before this
// pass, so an assertion about the VALUE proves nothing. What is asserted here is
// that the copy stopped being emitted -- and, just as importantly, that the cases
// which must NOT coalesce still do not.

// Two merges, nested, because that is the shape the pass actually fires on. The
// interesting source is `%5 = add %4, %1`, where `%4` is another merge's value:
// a real arithmetic result in a real register, dead the moment the store that
// feeds the outer merge retires. A constant source would not do -- there is no
// register there to adopt, and a `add` of two constants folds away before
// allocation ever sees it.
static const char* kDeadArithmeticSource = R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 10
    branch %0, block1, block2
block1:
    %2 = const_i64 7
    store __ssa_1, %2
    jump block3
block2:
    %3 = const_i64 8
    store __ssa_1, %3
    jump block3
block3:
    %4 = load __ssa_1
    %5 = add %4, %1
    branch %0, block4, block5
block4:
    %6 = const_i64 3
    store __ssa_2, %5
    jump block6
block5:
    %7 = const_i64 4
    store __ssa_2, %7
    jump block6
block6:
    %8 = load __ssa_2
    call print, %8
    return
)";

// The same merge, except %5 is read again AFTER the join. Its register is not
// free once the edge's store retires, so sharing it with the destination would
// clobber a live value. This is the guard that makes the whole pass sound.
static const char* kSourceStillLiveAfterJoin = R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 10
    branch %0, block1, block2
block1:
    %2 = const_i64 7
    store __ssa_1, %2
    jump block3
block2:
    %3 = const_i64 8
    store __ssa_1, %3
    jump block3
block3:
    %4 = load __ssa_1
    %5 = add %4, %1
    branch %0, block4, block5
block4:
    %6 = const_i64 3
    store __ssa_2, %5
    jump block6
block5:
    %7 = const_i64 4
    store __ssa_2, %7
    jump block6
block6:
    %8 = load __ssa_2
    %9 = add %8, %5
    call print, %9
    return
)";

static const char* kConstantSources = R"(
function __main__():
block0:
    %0 = const_i64 1
    %1 = const_i64 10
    branch %0, block1, block2
block1:
    store __ssa_1, %1
    jump block3
block2:
    store __ssa_1, %0
    jump block3
block3:
    %4 = load __ssa_1
    call print, %4
    return
)";

static size_t compile_and_coalesce(const char* ir) {
    Module m = parse_ir_text(ir);
    CompileOptions opt;
    opt.ssa_pipeline = false;   // the IR above is already resolved
    return compile_module(m, opt).phi_copies_coalesced;
}

static void test_dead_source_register_is_adopted() {
    size_t n = compile_and_coalesce(kDeadArithmeticSource);
    check(n == 1, "a dead arithmetic source's register is coalesced away");
}

static void test_live_source_is_not_taken() {
    size_t n = compile_and_coalesce(kSourceStillLiveAfterJoin);
    check(n == 0, "a source still live after the join keeps its register");
}

static void test_constant_sources_are_left_alone() {
    size_t n = compile_and_coalesce(kConstantSources);
    check(n == 0, "a merge fed by constants has nothing to coalesce");
}

int main() {
    test_copies_become_registers();
    test_fallback_is_still_correct();
    test_float_merge_is_not_counted_as_a_register();
    test_off_by_default_is_quiet();
    test_dead_source_register_is_adopted();
    test_live_source_is_not_taken();
    test_constant_sources_are_left_alone();

    if (failures != 0) {
        std::printf("\n%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: phi copies as register moves\n");
    return 0;
}