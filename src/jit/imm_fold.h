#pragma once
#include <cstdint>
#include <unordered_map>
#include "ir/ir.h"

// Which ConstInt values compile_function.h will fold directly into a
// single x86 immediate operand, and therefore never materialize into a
// register or stack slot at all.
//
// This must be computed BEFORE liveness/register allocation, and fed
// into both (liveness.h, register_alloc.h), not just consulted at
// codegen time in compile_function.h. A folded value has no run-time
// existence: nothing ever reads it through a register. If liveness
// still tracked it as an ordinary value, loop back-edge range
// extension (liveness.h) would keep a loop-invariant constant "live"
// across the entire loop purely because it's textually read every
// iteration -- artificially pressuring the small %N register pool for
// a value that generates zero instructions. (Measured: this alone
// consumed one of only two allocatable temp registers for the whole
// body of a hot loop, forcing every other temporary in the loop to
// spill to the stack and erasing most of the benefit of promoting
// variables to registers in the first place.)
namespace lithon::jit {

inline bool fold_fits_i32(int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX;
}

class ImmediateFolds {
public:
    explicit ImmediateFolds(const lithon::ir::Function& fn) {
        using lithon::ir::Op;
        std::unordered_map<lithon::ir::ValueId, int64_t> const_val;
        std::unordered_map<lithon::ir::ValueId, int> use_count;

        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == Op::ConstInt) const_val[instr.result] = instr.int_imm;
                for (auto a : instr.args) ++use_count[a];
            }
        }

        auto eligible_op = [](Op op) {
            return op == Op::Add || op == Op::Sub || op == Op::Mul ||
                   op == Op::Lt || op == Op::Gt || op == Op::Eq;
        };

        // A ConstInt folds only if its SINGLE use is exactly the RHS of
        // one of the ops compile_function.h knows how to fold. If it's
        // used more than once, or its one use isn't an eligible RHS, it
        // is left to materialize normally -- codegen for that use site
        // then also takes the normal (non-folded) path, since folded()
        // is false for it. The two always agree because both consult
        // this same decision.
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                if (!eligible_op(instr.op) || instr.args.size() != 2) continue;
                lithon::ir::ValueId rhs = instr.args[1];
                auto cv = const_val.find(rhs);
                if (cv == const_val.end()) continue;
                if (use_count[rhs] != 1) continue;
                if (!fold_fits_i32(cv->second)) continue;
                value_[rhs] = static_cast<int32_t>(cv->second);
            }
        }
    }

    bool folded(lithon::ir::ValueId id) const { return value_.count(id) != 0; }
    int32_t operator[](lithon::ir::ValueId id) const { return value_.at(id); }

private:
    std::unordered_map<lithon::ir::ValueId, int32_t> value_;
};

} // namespace lithon::jit
