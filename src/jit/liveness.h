#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"

// Loop-aware live-range computation over an ir::Function. A value's
// live range is [birth, last_use] in a flat, per-function
// instruction-index numbering (blocks concatenated in their emitted
// order) -- extended across any loop (back-edge) that contains both
// its definition and a later reaching use, so loop-carried values
// (e.g. an accumulator inside a while/for) are correctly kept alive
// for the whole loop, not just their last textual mention before the
// backward jump.

namespace lithon::jit {

struct LiveRange {
    int birth = -1;
    int last_use = -1;
};

class LivenessAnalysis {
public:
    explicit LivenessAnalysis(const lithon::ir::Function& fn) : fn_(fn) {
        flatten_instructions();
        build_block_graph();
        compute_naive_ranges();
        extend_across_back_edges();
    }

    const std::unordered_map<lithon::ir::ValueId, LiveRange>& ranges() const {
        return ranges_;
    }

    int instruction_count() const { return static_cast<int>(flat_instrs_.size()); }

private:
    const lithon::ir::Function& fn_;

    // Flattened, function-wide instruction index -> (block index, instr ptr)
    std::vector<const lithon::ir::Instr*> flat_instrs_;
    // block label -> [start_index, end_index) in the flattened list
    std::unordered_map<std::string, std::pair<int, int>> block_span_;
    std::unordered_map<std::string, size_t> block_order_; // label -> position in fn_.blocks

    std::vector<std::pair<std::string, std::string>> back_edges_; // (from_label, to_label)

    std::unordered_map<lithon::ir::ValueId, LiveRange> ranges_;

    void flatten_instructions() {
        int idx = 0;
        for (size_t bi = 0; bi < fn_.blocks.size(); ++bi) {
            const auto& block = fn_.blocks[bi];
            block_order_[block.label] = bi;
            int start = idx;
            for (const auto& instr : block.instrs) {
                flat_instrs_.push_back(&instr);
                ++idx;
            }
            block_span_[block.label] = {start, idx};
        }
    }

    void build_block_graph() {
        for (size_t bi = 0; bi < fn_.blocks.size(); ++bi) {
            const auto& block = fn_.blocks[bi];
            for (const auto& instr : block.instrs) {
                if (instr.op == lithon::ir::Op::Jump) {
                    add_edge_if_back(block.label, instr.name);
                } else if (instr.op == lithon::ir::Op::Branch) {
                    size_t comma = instr.name.find(',');
                    add_edge_if_back(block.label, instr.name.substr(0, comma));
                    add_edge_if_back(block.label, instr.name.substr(comma + 1));
                }
            }
        }
    }

    void add_edge_if_back(const std::string& from, const std::string& to) {
        auto from_it = block_order_.find(from);
        auto to_it = block_order_.find(to);
        if (from_it == block_order_.end() || to_it == block_order_.end()) return;
        // A back-edge jumps to a block at or before the current one
        // in emission order -- the structural signature of a loop,
        // given frontend.py's consistent header/body/exit ordering.
        if (to_it->second <= from_it->second) {
            back_edges_.emplace_back(from, to);
        }
    }

    void compute_naive_ranges() {
        for (int i = 0; i < instruction_count(); ++i) {
            const auto& instr = *flat_instrs_[i];
            if (instr.result != lithon::ir::kInvalidValue) {
                ranges_[instr.result].birth = i;
                if (ranges_[instr.result].last_use < i) {
                    ranges_[instr.result].last_use = i;
                }
            }
            for (auto arg : instr.args) {
                auto it = ranges_.find(arg);
                if (it != ranges_.end() && it->second.last_use < i) {
                    it->second.last_use = i;
                }
            }
        }
    }

    void extend_across_back_edges() {
        for (const auto& [from_label, to_label] : back_edges_) {
            int loop_start = block_span_[to_label].first;
            int loop_end = block_span_[from_label].second;

            for (auto& [value_id, range] : ranges_) {
                // A value already alive before the loop started, and
                // still needed at/after the loop's tail, must be kept
                // alive across the WHOLE loop body -- it survives
                // every iteration, not just the textual span between
                // its birth and its last mention.
                if (range.birth <= loop_start && range.last_use >= loop_start
                    && range.last_use < loop_end) {
                    range.last_use = loop_end;
                }
                if (range.birth < loop_end && range.birth >= loop_start
                    && range.last_use >= loop_start && range.last_use < loop_end) {
                    // Value born and used entirely inside the loop body:
                    // still needs to survive to the end of the body,
                    // since the backward jump means the block containing
                    // its last use runs again before the value is dead.
                    range.last_use = std::max(range.last_use, loop_end - 1);
                }
            }
        }
    }
};

} // namespace lithon::jit
