// Benchmark: fib(30) via three execution paths, to test the actual
// premise of the project, does native codegen deliver real speed
// over the interpreter, and how does it compare to CPython.

#include "compile_function.h"
#include "ir/ir.h"
#include "interpreter/interpreter.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
#include <chrono>

using namespace lithon::ir;
using namespace lithon::jit;
using Clock = std::chrono::steady_clock;

Module build_fib_module() {
    Module module;
    Function fib_fn;
    fib_fn.name = "fib";
    fib_fn.params = {"n"};

    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::Load; i.result = 0; i.name = "n"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 1; i.int_imm = 2; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Lt; i.result = 2; i.args = {0, 1}; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Branch; i.result = kInvalidValue; i.args = {2};
      i.name = "block1,block2"; block0.instrs.push_back(i); }

    BasicBlock block1;
    block1.label = "block1";
    { Instr i; i.op = Op::Load; i.result = 3; i.name = "n"; block1.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {3}; block1.instrs.push_back(i); }

    BasicBlock block2;
    block2.label = "block2";
    { Instr i; i.op = Op::Load; i.result = 4; i.name = "n"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 5; i.int_imm = 1; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Sub; i.result = 6; i.args = {4, 5}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 7; i.args = {6}; i.name = "fib"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Load; i.result = 8; i.name = "n"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::ConstInt; i.result = 9; i.int_imm = 2; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Sub; i.result = 10; i.args = {8, 9}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 11; i.args = {10}; i.name = "fib"; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Add; i.result = 12; i.args = {7, 11}; block2.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; i.args = {12}; block2.instrs.push_back(i); }

    fib_fn.blocks = {block0, block1, block2};
    module.functions = {fib_fn};
    return module;
}
/*
Wraps the interpreter to call fib(n) directly, bypassing the
print()/main()-only entry point run_main expects, by building a
tiny wrapper "main" that calls fib and returns its result -- but
since interpreter.h only exposes run_main() with no return value
capture, we instead directly measure the whole interpreted
execution of a wrapper module that computes fib(N) and PRINTS it,
timing wall-clock around that call. This is a fair, real
end-to-end interpreted-execution measurement.
 */
Module build_fib_with_print(int64_t n) {
    Module module = build_fib_module();
    Function main_fn;
    main_fn.name = "main";
    BasicBlock block0;
    block0.label = "block0";
    { Instr i; i.op = Op::ConstInt; i.result = 0; i.int_imm = n; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = 1; i.args = {0}; i.name = "fib"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Call; i.result = kInvalidValue; i.args = {1}; i.name = "print"; block0.instrs.push_back(i); }
    { Instr i; i.op = Op::Return; i.result = kInvalidValue; block0.instrs.push_back(i); }
    main_fn.blocks = {block0};
    module.functions.push_back(main_fn);
    return module;
}

int main() {
    const int64_t N = 30;

    // --- Native compiled ---
    Module native_module = build_fib_module();
    CompiledModule compiled = compile_module(native_module);

    void* mem = mmap(nullptr, compiled.code.size(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    std::memcpy(mem, compiled.code.data(), compiled.code.size());
    mprotect(mem, compiled.code.size(), PROT_READ | PROT_EXEC);

    typedef int64_t (*FibFunc)(int64_t);
    FibFunc native_fib = reinterpret_cast<FibFunc>(
        reinterpret_cast<uint8_t*>(mem) + compiled.function_offset.at("fib"));

    auto t0 = Clock::now();
    int64_t native_result = native_fib(N);
    auto t1 = Clock::now();
    double native_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // --- Interpreter ---
    Module interp_module = build_fib_with_print(N);
    auto t2 = Clock::now();
    printf("(interpreter output for fib(%lld): ", (long long)N);
    lithon::interp::run_main(interp_module);
    auto t3 = Clock::now();
    double interp_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    printf("\n--- fib(%lld) benchmark ---\n", (long long)N);
    printf("native compiled : %.4f ms  (result=%lld)\n", native_ms, (long long)native_result);
    printf("interpreter     : %.4f ms\n", interp_ms);
    printf("speedup         : %.1fx\n", interp_ms / native_ms);

    return 0;
}
