// End-to-end check for accumulator_unroll on a FLOAT reduction.
//
// The integer path is covered by optimize_accum_test; this reaches the two
// things only real codegen can show:
//
//   1. The values the pass invents (the partial accumulators and the exit sum)
//      are doubles, but value kinds come from an analysis of the ORIGINAL
//      module. The plumbing that folds the pass's new float ids into the
//      per-function kind vector has to actually run, or codegen types the
//      partials `int` and computes 0.0 -- the miscompile that kept this pass
//      integer-only.
//
//   2. The split loop executes and produces the right number. `total += i*0.5`
//      keeps every partial sum an exact multiple of 0.5 below 2^53, so unlike a
//      general float reduction the reassociated result is EXACT and must equal
//      the scalar one bit-for-bit. That makes a plain equality assertion valid.
//
// optimize_function() is also called directly (no kinds) to prove the pass
// declines a float accumulator rather than inventing mistyped values.

#include "compile_function.h"
#include "ir/ir.h"
#include "exec_memory.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace lithon::ir;
using namespace lithon::jit;

typedef double (*DoubleFunc)();

static Instr ConstF(ValueId r, double v) {
    Instr i;
    i.op = Op::ConstFloat;
    i.result = r;
    i.float_imm = v;
    return i;
}

static Instr ConstI(ValueId r, int64_t v) {
    Instr i;
    i.op = Op::ConstInt;
    i.result = r;
    i.int_imm = v;
    return i;
}

static Instr Load(ValueId r, const char* var) {
    Instr i;
    i.op = Op::Load;
    i.result = r;
    i.name = var;
    return i;
}

static Instr Store(const char* var, ValueId v) {
    Instr i;
    i.op = Op::Store;
    i.result = kInvalidValue;
    i.name = var;
    i.args = {v};
    return i;
}

static Instr Bin(Op op, ValueId r, ValueId a, ValueId b) {
    Instr i;
    i.op = op;
    i.result = r;
    i.args = {a, b};
    return i;
}

static Instr Jump(const char* target) {
    Instr i;
    i.op = Op::Jump;
    i.result = kInvalidValue;
    i.name = target;
    return i;
}

static Instr Branch(ValueId cond, const char* t, const char* f) {
    Instr i;
    i.op = Op::Branch;
    i.result = kInvalidValue;
    i.args = {cond};
    i.name = std::string(t) + ", " + f;
    return i;
}

static Instr Ret(ValueId v) {
    Instr i;
    i.op = Op::Return;
    i.result = kInvalidValue;
    i.args = {v};
    return i;
}

// The canonical counted reduction the frontend emits:
//
//     total = 0.0
//     for i in range(n):
//         total += i * 0.5
//     return total
static Module reduce_module(int64_t n) {
    Function fn;
    fn.name = "main";

    BasicBlock p;
    p.label = "block0";
    p.instrs = {
        ConstF(0, 0.0), Store("total", 0),
        ConstI(1, n), ConstI(2, 0), Store("i", 2),
        Jump("block1"),
    };

    BasicBlock h;
    h.label = "block1";
    h.instrs = {
        Load(3, "i"), Bin(Op::Lt, 4, 3, 1), Branch(4, "block2", "block3"),
    };

    BasicBlock b;
    b.label = "block2";
    b.instrs = {
        Load(5, "total"), Load(6, "i"), ConstF(7, 0.5),
        Bin(Op::Mul, 8, 6, 7), Bin(Op::Add, 9, 5, 8), Store("total", 9),
        Load(10, "i"), ConstI(11, 1), Bin(Op::Add, 12, 10, 11), Store("i", 12),
        Jump("block1"),
    };

    BasicBlock x;
    x.label = "block3";
    x.instrs = { Load(13, "total"), Ret(13) };

    fn.blocks = {p, h, b, x};
    Module m;
    m.functions = {fn};
    return m;
}

static double run(Module m, int accum_unroll) {
    CompileOptions opt;
    opt.accum_unroll = accum_unroll;
    CompiledModule c = compile_module(m, opt);
    ExecutableBuffer mem(c.code);
    auto fn = reinterpret_cast<DoubleFunc>(
        reinterpret_cast<uint8_t*>(mem.data()) + c.function_offset.at("main"));
    return fn();
}

int main() {
    const int64_t n = 2000000;
    const double exact = 0.5 * (static_cast<double>(n) * static_cast<double>(n - 1) / 2.0);

    CompileOptions off_opt;
    off_opt.accum_unroll = 1;
    CompileOptions on_opt;
    on_opt.accum_unroll = 4;
    size_t sz_off = compile_module(reduce_module(n), off_opt).code.size();
    size_t sz_on = compile_module(reduce_module(n), on_opt).code.size();

    // Teeth: the pass must actually have rewritten the loop.
    if (sz_on <= sz_off) {
        std::fprintf(stderr, "FAIL: --accum-unroll did not change the float loop (%zu -> %zu bytes)\n",
                     sz_off, sz_on);
        return 1;
    }

    double scalar = run(reduce_module(n), 1);
    double split = run(reduce_module(n), 4);
    if (scalar != exact) {
        std::fprintf(stderr, "FAIL: scalar float reduction %.17g != exact %.17g\n", scalar, exact);
        return 1;
    }
    if (split != exact) {
        std::fprintf(stderr, "FAIL: split float reduction %.17g != exact %.17g\n", split, exact);
        return 1;
    }

    // Without value kinds the pass cannot classify the new doubles and must
    // leave the loop alone rather than create mistyped values.
    {
        Module m = reduce_module(n);
        OptimizePasses passes;
        passes.accum_unroll = 4;
        OptimizeStats stats = optimize_function(m.functions[0], passes);
        if (stats.accum_unrolled != 0) {
            std::fprintf(stderr, "FAIL: float accumulator split without kinds (count=%d)\n",
                         stats.accum_unrolled);
            return 1;
        }
    }

    std::printf("PASS: float accumulator split exactly (%.1f), declined without kinds\n", exact);
    return 0;
}
