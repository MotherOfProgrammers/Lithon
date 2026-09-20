#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include "ir/ir.h"
#include "liveness.h"
#include "x86_encoder.h"

// Register allocation for Lithon's v1 codegen, split into two
// independent, deliberately simple mechanisms (see liveness.h's
// design note for why this split is CORRECT, not a shortcut):
//
//   1. Named variables (locals/params) -- one fixed stack slot each,
//      for the whole function. No liveness needed: this sidesteps
//      the loop-carrying problem entirely, by construction.
//   2. %N temporaries -- allocated to a small pool of scratch
//      registers via linear scan over LivenessAnalysis, spilling to
//      a stack slot when the pool is exhausted.

namespace lithon::jit {

struct ValueLocation {
    bool in_register = false;
    Reg reg{};
    int stack_slot = -1;
};

class RegisterAllocator {
public:
    explicit RegisterAllocator(const lithon::ir::Function& fn)
        : fn_(fn), liveness_(fn) {
        assign_variable_slots();
        assign_temporary_locations();
    }

    int variable_offset(const std::string& name) const {
        auto it = variable_offsets_.find(name);
        return it != variable_offsets_.end() ? it->second : 0;
    }

    bool has_variable(const std::string& name) const {
        return variable_offsets_.count(name) != 0;
    }

    const ValueLocation& temp_location(lithon::ir::ValueId id) const {
        static ValueLocation missing{};
        auto it = temp_locations_.find(id);
        return it != temp_locations_.end() ? it->second : missing;
    }

    int frame_size() const { return frame_size_; }

    const std::vector<std::string>& variable_names_in_order() const {
        return variable_order_;
    }

private:
    const lithon::ir::Function& fn_;
    LivenessAnalysis liveness_;

    std::unordered_map<std::string, int> variable_offsets_;
    std::vector<std::string> variable_order_;
    std::unordered_map<lithon::ir::ValueId, ValueLocation> temp_locations_;
    int next_slot_offset_ = 0;
    int frame_size_ = 0;

    int allocate_new_slot() {
        next_slot_offset_ -= 8;
        return next_slot_offset_;
    }

    void assign_variable_slots() {
        for (const auto& param : fn_.params) {
            if (!variable_offsets_.count(param)) {
                variable_offsets_[param] = allocate_new_slot();
                variable_order_.push_back(param);
            }
        }
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == lithon::ir::Op::Store && !variable_offsets_.count(instr.name)) {
                    variable_offsets_[instr.name] = allocate_new_slot();
                    variable_order_.push_back(instr.name);
                }
            }
        }
    }

    void assign_temporary_locations() {
        const std::vector<Reg> pool = {Reg::RAX, Reg::RCX, Reg::RDX, Reg::RBX};

        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> entries(
            liveness_.ranges().begin(), liveness_.ranges().end());
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.second.birth < b.second.birth; });

        std::unordered_set<Reg> free_regs(pool.begin(), pool.end());
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> active;

        for (const auto& entry : entries) {
            lithon::ir::ValueId id = entry.first;
            const LiveRange& range = entry.second;

            active.erase(std::remove_if(active.begin(), active.end(),
                [&](const auto& a) {
                    if (a.second.last_use < range.birth) {
                        auto loc_it = temp_locations_.find(a.first);
                        if (loc_it != temp_locations_.end() && loc_it->second.in_register) {
                            free_regs.insert(loc_it->second.reg);
                        }
                        return true;
                    }
                    return false;
                }), active.end());

            if (!free_regs.empty()) {
                Reg r = *free_regs.begin();
                free_regs.erase(free_regs.begin());
                temp_locations_[id] = ValueLocation{true, r, -1};
            } else {
                int slot = allocate_new_slot();
                temp_locations_[id] = ValueLocation{false, Reg::RAX, slot};
            }

            active.push_back(entry);
        }

        int total_bytes = -next_slot_offset_;
        frame_size_ = ((total_bytes + 15) / 16) * 16;
    }
};

} // namespace lithon::jit
