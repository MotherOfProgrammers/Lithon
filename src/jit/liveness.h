#pragma once

#include <unordered_map>
#include <vector>
#include "ir/ir.h"

// Live-range computation over an ir::Function's raw SSA-style value
// ids (%N registers), for register-allocating short-lived
// TEMPORARIES only (e.g. the intermediate results of "i < 10").
//
// This is deliberately NOT loop-aware, and that is correct, not a
// simplification: in this IR, a value that must survive a loop
// iteration is carried through a named Store/Load (see
// interpreter.cpp's Frame::vars), and every Load produces a BRAND
// NEW %N -- no raw value id is ever reused across a back-edge. An
// earlier attempt to extend %N live ranges across back-edges was
// therefore solving a problem that does not exist here, and it
// produced incorrect ranges for genuinely loop-local temporaries
// (caught by liveness_test.cpp). Named-variable storage is handled
// separately and much more simply: every named variable gets one
// fixed stack slot for the whole function (see the register
// allocator design note in compile_function) -- no register
// allocation for variables in v1, only for these %N temporaries.

namespace lithon::jit {

struct LiveRange {
    int birth = -1;
    int last_use = -1;
};

class LivenessAnalysis {
public:
    explicit LivenessAnalysis(const lithon::ir::Function& fn) {
        int idx = 0;
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.result != lithon::ir::kInvalidValue) {
                    ranges_[instr.result].birth = idx;
                    if (ranges_[instr.result].last_use < idx) {
                        ranges_[instr.result].last_use = idx;
                    }
                }
                for (auto arg : instr.args) {
                    auto it = ranges_.find(arg);
                    if (it != ranges_.end() && it->second.last_use < idx) {
                        it->second.last_use = idx;
                    }
                }
                ++idx;
            }
        }
        instruction_count_ = idx;
    }

    const std::unordered_map<lithon::ir::ValueId, LiveRange>& ranges() const {
        return ranges_;
    }

    int instruction_count() const { return instruction_count_; }

private:
    std::unordered_map<lithon::ir::ValueId, LiveRange> ranges_;
    int instruction_count_ = 0;
};

} // namespace lithon::jit
