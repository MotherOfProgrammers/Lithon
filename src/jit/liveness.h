#pragma once
#include <unordered_map>
#include <vector>
#include "ir/ir.h"

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
