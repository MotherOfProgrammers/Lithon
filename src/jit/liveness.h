#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "imm_fold.h"
#include "ir/ir.h"

namespace lithon::jit {

// Trims surrounding whitespace from a branch-target label. Needed
// because Branch encodes both targets as one comma-separated name
// ("then, else") -- without trimming, a label with a leading space
// after the split silently fails lookup against label_index in
// find_loops() below, and a real loop is missed entirely.
inline std::string trim_label(const std::string& raw) {
    std::string label = raw;
    label.erase(0, label.find_first_not_of(" \t"));
    size_t last = label.find_last_not_of(" \t");
    label.erase(last == std::string::npos ? 0 : last + 1);
    return label;
}

// Labels a Jump/Branch can transfer control to (empty for other ops).
inline std::vector<std::string> branch_targets(const lithon::ir::Instr& in) {
    std::vector<std::string> out;
    if (in.op == lithon::ir::Op::Jump) {
        out.push_back(trim_label(in.name));
    } else if (in.op == lithon::ir::Op::Branch) {
        size_t comma = in.name.find(',');
        if (comma == std::string::npos) {
            out.push_back(trim_label(in.name));
        } else {
            out.push_back(trim_label(in.name.substr(0, comma)));
            out.push_back(trim_label(in.name.substr(comma + 1)));
        }
    }
    return out;
}

// A natural loop, identified by a back edge (a jump to a block at the
// same or an earlier position). Covers blocks [first_block, last_block]
// and the flat instruction range [flat_start, flat_end].
struct LoopSpan {
    size_t first_block;
    size_t last_block;
    int flat_start;
    int flat_end;
};

inline std::vector<LoopSpan> find_loops(const lithon::ir::Function& fn) {
    std::unordered_map<std::string, size_t> label_index;
    std::vector<int> block_start(fn.blocks.size()), block_end(fn.blocks.size());
    int idx = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        label_index[fn.blocks[b].label] = b;
        block_start[b] = idx;
        idx += static_cast<int>(fn.blocks[b].instrs.size());
        block_end[b] = idx - 1;
    }
    std::vector<LoopSpan> loops;
    for (size_t j = 0; j < fn.blocks.size(); ++j) {
        for (const auto& instr : fn.blocks[j].instrs) {
            for (const auto& target : branch_targets(instr)) {
                auto it = label_index.find(target);
                if (it != label_index.end() && it->second <= j && block_end[j] >= block_start[it->second]) {
                    loops.push_back({it->second, j, block_start[it->second], block_end[j]});
                }
            }
        }
    }
    return loops;
}

struct LiveRange {
    int birth = -1;
    int last_use = -1;
};

// Linear live ranges over the flat instruction order, corrected for
// loops: a value defined BEFORE a loop and used INSIDE it is read
// again on every iteration, so it must stay live until the loop's
// back edge -- not merely until its last textual use. Loop spans are
// found structurally via find_loops() above, so this logic and any
// future pass that also needs "is this instruction inside a loop"
// (e.g. LICM, or hoisting a loop-invariant bound out of the body)
// share one definition of what a loop is, instead of two that could
// drift apart.
class LivenessAnalysis {
public:
    // `folds`, when given, marks ConstInt values that will never be
    // materialized into a register or stack slot at all (compile_
    // function.h folds them straight into one instruction's immediate
    // operand). Such an id gets NO live range here -- not at its
    // definition, and it is not treated as a "use" at its one
    // reference either -- so it can never occupy allocator pressure or
    // be dragged across a loop by the extension below.
    explicit LivenessAnalysis(const lithon::ir::Function& fn,
                              const ImmediateFolds* folds = nullptr) {
        int idx = 0;
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                bool result_folded = folds && instr.result != lithon::ir::kInvalidValue &&
                                     folds->folded(instr.result);
                if (instr.result != lithon::ir::kInvalidValue && !result_folded) {
                    ranges_[instr.result].birth = idx;
                    if (ranges_[instr.result].last_use < idx) {
                        ranges_[instr.result].last_use = idx;
                    }
                }
                for (auto arg : instr.args) {
                    if (folds && folds->folded(arg)) continue;   // not a real use: no register ever holds it
                    auto it = ranges_.find(arg);
                    if (it != ranges_.end() && it->second.last_use < idx) {
                        it->second.last_use = idx;
                    }
                }
                ++idx;
            }
        }
        instruction_count_ = idx;
        extend_across_loops(fn);
    }

    const std::unordered_map<lithon::ir::ValueId, LiveRange>& ranges() const {
        return ranges_;
    }

    int instruction_count() const { return instruction_count_; }

private:
    std::unordered_map<lithon::ir::ValueId, LiveRange> ranges_;
    int instruction_count_ = 0;

    // Repeat until stable so nested loops, where extending for an
    // inner loop can bring a value into an outer loop's span, converge.
    void extend_across_loops(const lithon::ir::Function& fn) {
        const auto loops = find_loops(fn);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& loop : loops) {
                for (auto& kv : ranges_) {
                    LiveRange& r = kv.second;
                    if (r.birth < loop.flat_start && r.last_use >= loop.flat_start &&
                        r.last_use < loop.flat_end) {
                        r.last_use = loop.flat_end;
                        changed = true;
                    }
                }
            }
        }
    }
};

} // namespace lithon::jit
