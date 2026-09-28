#pragma once
#include <algorithm>
#include <string>
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
        extend_across_back_edges(fn);
    }

    const std::unordered_map<lithon::ir::ValueId, LiveRange>& ranges() const {
        return ranges_;
    }

    int instruction_count() const { return instruction_count_; }

private:
    std::unordered_map<lithon::ir::ValueId, LiveRange> ranges_;
    int instruction_count_ = 0;

    // Intervals above are linear over textual instruction order. That is
    // wrong for loops: a value born before a loop header and read inside
    // the loop must stay live until the back-edge jump, otherwise the
    // register allocator hands its register to a value in the loop body
    // and the next iteration reads garbage.
    //
    // For every backward edge (jump at index j to a block starting at
    // index t <= j), any value born before t and live at t has its
    // last_use pushed out to j. Repeat until stable so nested loops, where
    // extending for an inner edge can bring a value into an outer edge's
    // reach, converge.
    void extend_across_back_edges(const lithon::ir::Function& fn) {
        std::unordered_map<std::string, int> block_start;
        int idx = 0;
        for (const auto& block : fn.blocks) {
            block_start[block.label] = idx;
            idx += static_cast<int>(block.instrs.size());
        }

        struct Edge { int from; int to; };
        std::vector<Edge> back_edges;
        idx = 0;
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instrs) {
                auto note = [&](const std::string& raw) {
                    std::string label = raw;
                    label.erase(0, label.find_first_not_of(" \t"));
                    label.erase(label.find_last_not_of(" \t") + 1);
                    auto it = block_start.find(label);
                    if (it != block_start.end() && it->second <= idx)
                        back_edges.push_back({idx, it->second});
                };
                if (instr.op == lithon::ir::Op::Jump) {
                    note(instr.name);
                } else if (instr.op == lithon::ir::Op::Branch) {
                    size_t comma = instr.name.find(',');
                    note(instr.name.substr(0, comma));
                    if (comma != std::string::npos) note(instr.name.substr(comma + 1));
                }
                ++idx;
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& e : back_edges) {
                for (auto& kv : ranges_) {
                    LiveRange& r = kv.second;
                    if (r.birth < e.to && r.last_use >= e.to && r.last_use < e.from) {
                        r.last_use = e.from;
                        changed = true;
                    }
                }
            }
        }
    }
};

} // namespace lithon::jit
