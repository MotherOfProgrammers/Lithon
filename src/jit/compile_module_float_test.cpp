// End-to-end native checks for the float cases that a unit test cannot reach,
// because each one needs real machine code to actually be executed:
//
//   1. A float value that stays live ACROSS a call. Every XMM in the temp pool
//      is caller-saved on both ABIs, and the print handler is an ordinary C
//      function that clobbers all of them. Before register_alloc.h grew a
//      spill for float ranges crossing a call, this program computed 17.5
//      natively and 1.25 in the interpreter: the two engines disagreed, and
//      only a program that interleaves prints with float arithmetic reaches
//      it at all.
//
//   2. A NaN divisor, which must PROPAGATE rather than trap. The zero check
//      materialises ZF from comisd, and comisd sets ZF for an unordered
//      compare exactly as it does for an equal one, so a naive "not equal"
//      branch takes the divide-by-zero trap on a NaN. Python's answer is that
//      1.0/nan is nan, and the interpreter already does that, so the JIT has
//      to agree or run_tier_diff.py reports a mismatch.
//
//   3. A -0.0 divisor, which must TRAP, because -0.0 compares equal to 0.0.
//      That is a different branch from case 2 and the test is worthless if the
//      two are only ever checked together. The trap handler calls exit(1), so
//      this one runs in a child process and checks the exit status.
//
// The expected output is printed for the two in-process cases; the child case
// is asserted, and the shell/CMake check is the authoritative gate.

#include "compile_function.h"
#include "ir/ir.h"
#include "exec_memory.h"

#include <cstdio>
#include <cstring>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lithon::ir;
using namespace lithon::jit;

typedef void (*VoidFunc)();

static Instr ConstF(ValueId result, double v) {
    Instr i;
    i.op = Op::ConstFloat;
    i.result = result;
    i.float_imm = v;
    return i;
}

static Instr ConstI(ValueId result, int64_t v) {
    Instr i;
    i.op = Op::ConstInt;
    i.result = result;
    i.int_imm = v;
    return i;
}

static Instr Store(const char* variable, ValueId value) {
    Instr i;
    i.op = Op::Store;
    i.result = kInvalidValue;
    i.name = variable;
    i.args = {value};
    return i;
}

static Instr Load(ValueId result, const char* variable) {
    Instr i;
    i.op = Op::Load;
    i.result = result;
    i.name = variable;
    return i;
}

static Instr Bin(Op op, ValueId result, ValueId a, ValueId b) {
    Instr i;
    i.op = op;
    i.result = result;
    i.args = {a, b};
    return i;
}

static Instr Print(ValueId value) {
    Instr i;
    i.op = Op::Call;
    i.result = kInvalidValue;
    i.name = "print";
    i.args = {value};
    return i;
}

static Instr Ret() {
    Instr i;
    i.op = Op::Return;
    i.result = kInvalidValue;
    return i;
}

static Module one_block_main(std::vector<Instr> instrs) {
    Module m;
    Function fn;
    fn.name = "main";
    BasicBlock b0;
    b0.label = "block0";
    b0.instrs = std::move(instrs);
    fn.blocks = {b0};
    m.functions = {fn};
    return m;
}

static void run_here(Module m) {
    CompiledModule c = compile_module(m);
    ExecutableBuffer mem(c.code);
    reinterpret_cast<VoidFunc>(reinterpret_cast<uint8_t*>(mem.data()) +
                               c.function_offset.at("main"))();
}

// Prints 7, then prints 2.5 * 7. The float 2.5 is live across the first call.
static Module live_across_call_module() {
    return one_block_main({
        ConstI(0, 7),                        // 7
        ConstF(1, 2.5),                      // 2.5
        Print(0),                            // "7"
        Bin(Op::Mul, 2, 1, 0),               // 17.5, with 2.5 live across the call
        Print(2),
        Ret(),
    });
}

// 1.0 / nan must print nan, not trap. nan is built the way the fuzzer builds
// it, with no NaN literal in the IR: inf - inf, where inf is 1e308 squared.
static Module nan_divisor_module() {
    return one_block_main({
        ConstF(0, 1e308),
        Bin(Op::Mul, 1, 0, 0),               // inf
        Bin(Op::Mul, 2, 1, 1),               // inf
        Bin(Op::Sub, 3, 2, 2),               // nan
        ConstF(4, 1.0),
        Bin(Op::Div, 5, 4, 3),               // 1.0 / nan == nan
        Print(5),
        Ret(),
    });
}

static Module zero_divisor_module(double divisor) {
    return one_block_main({
        ConstF(0, 1.0),
        ConstF(1, divisor),
        Bin(Op::Div, 2, 0, 1),
        Print(2),
        Ret(),
    });
}

static int run_in_child(Module m) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        run_here(std::move(m));
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main() {
    printf("--- live across call: expect 7, 17.5 ---\n");
    run_here(live_across_call_module());

    printf("--- NaN divisor: expect nan ---\n");
    run_here(nan_divisor_module());

    struct { double divisor; const char* what; } traps[] = {
        {0.0, "0.0 divisor"},
        {-0.0, "-0.0 divisor"},
    };
    for (const auto& t : traps) {
        printf("--- %s: expect 'division by zero' on stderr, exit 1 ---\n", t.what);
        int code = run_in_child(zero_divisor_module(t.divisor));
        if (code != 1) {
            fprintf(stderr, "FAIL: %s exited %d, want 1\n", t.what, code);
            return 1;
        }
    }

    printf("PASS: float live-across-call, NaN divisor propagates, zero divisors trap\n");
    return 0;
}
