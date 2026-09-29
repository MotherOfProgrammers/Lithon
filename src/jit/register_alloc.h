#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "jit_abi.h"
#include "liveness.h"
#include "x86_encoder.h"


namespace lithon::jit {

struct ValueLocation {
    bool in_register = false;
    Reg reg{};
    int stack_slot = -1;
};

using PromotionMap = std::unordered_map<std::string, Reg>;

// Chooses which variables to keep in registers. A variable's weight is
// the number of its loads/stores, each scaled by 10^(loop depth). Only
// variables that beat the cost of saving+restoring a register (two
// memory ops per call) are promoted.
inline PromotionMap select_promoted_variables(const lithon::ir::Function& fn) {
    using namespace lithon::ir;
    const auto loops = find_loops(fn);

    std::unordered_map<std::string, double> weight;
    for (const auto& p : fn.params) weight[p] += 1.0;   // entry store

    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        int depth = 0;
        for (const auto& loop : loops) {
            if (b >= loop.first_block && b <= loop.last_block) ++depth;
        }
        double scale = std::pow(10.0, std::min(depth, 6));
        for (const auto& in : fn.blocks[b].instrs) {
            if (in.op == Op::Load || in.op == Op::Store) weight[in.name] += scale;
        }
    }

    std::vector<std::pair<std::string, double>> ranked;
    for (const auto& kv : weight) {
        if (kv.second > 2.0) ranked.push_back(kv);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });

    PromotionMap promoted;
    for (size_t i = 0; i < ranked.size() && i < abi::kPromotionPool.size(); ++i) {
        promoted[ranked[i].first] = abi::kPromotionPool[i];
    }
    return promoted;
}

class RegisterAllocator {
public:
    // Convenience form: automatic weighted promotion, no virtual temps.
    // `folds` is forwarded to LivenessAnalysis so a folded-away constant
    // (see liveness.h) never costs a register or a spill slot it will
    // never actually use. Covers both call shapes seen in this codebase
    // to date: RegisterAllocator(fn) and RegisterAllocator(fn, &folds).
    explicit RegisterAllocator(const lithon::ir::Function& fn,
                               const ImmediateFolds* folds = nullptr)
        : RegisterAllocator(fn, select_promoted_variables(fn), {}, folds) {}

    // Full form: explicit control over which variables are promoted and
    // which %N temporaries need no location at all.
    RegisterAllocator(const lithon::ir::Function& fn, PromotionMap promoted,
                      const std::unordered_set<lithon::ir::ValueId>& virtual_temps,
                      const ImmediateFolds* folds = nullptr)
        : fn_(fn), liveness_(fn, folds), promoted_(std::move(promoted)) {
        assign_variable_slots();
        assign_callee_saved_slots();
        find_call_indices();
        assign_temporary_locations(virtual_temps);
    }

    int variable_offset(const std::string& name) const {
        auto it = variable_offsets_.find(name);
        return it != variable_offsets_.end() ? it->second : 0;
    }

    bool has_variable(const std::string& name) const {
        return variable_offsets_.count(name) != 0 || promoted_.count(name) != 0;
    }

    bool variable_in_register(const std::string& name) const {
        return promoted_.count(name) != 0;
    }

    Reg variable_register(const std::string& name) const { return promoted_.at(name); }
    // Alias for call sites written against the other in-flight naming
    // of this accessor -- kept to avoid guessing which one the current
    // compile_function.h actually calls.
    Reg variable_reg(const std::string& name) const { return variable_register(name); }

    const PromotionMap& promoted() const { return promoted_; }

    // (register, frame slot) pairs the prologue must save and every
    // return must restore -- exactly the promoted registers this
    // function actually uses, never more.
    const std::vector<std::pair<Reg, int>>& callee_saved_slots() const {
        return callee_saved_slots_;
    }

    // Convenience view over callee_saved_slots() for a caller that only
    // needs the register list. Derived, not stored redundantly.
    std::vector<Reg> used_variable_registers() const {
        std::vector<Reg> regs;
        regs.reserve(callee_saved_slots_.size());
        for (const auto& kv : callee_saved_slots_) regs.push_back(kv.first);
        return regs;
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
    PromotionMap promoted_;

    std::unordered_map<std::string, int> variable_offsets_;
    std::vector<std::string> variable_order_;
    std::vector<std::pair<Reg, int>> callee_saved_slots_;
    std::unordered_map<lithon::ir::ValueId, ValueLocation> temp_locations_;
    std::vector<int> call_indices_;
    int next_slot_offset_ = 0;
    int frame_size_ = 0;

    int allocate_new_slot() {
        next_slot_offset_ -= 8;
        return next_slot_offset_;
    }

    // A stack slot for every variable that did NOT get promoted --
    // never both a register and a slot for the same variable (see the
    // class-level comment for why that would be dead weight, not just
    // stylistically wasteful).
    void assign_variable_slots() {
        std::unordered_set<std::string> seen;
        auto assign_one = [&](const std::string& name) {
            if (!seen.insert(name).second) return;   // already handled
            variable_order_.push_back(name);
            if (!promoted_.count(name)) {
                variable_offsets_[name] = allocate_new_slot();
            }
        };
        for (const auto& param : fn_.params) assign_one(param);
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == lithon::ir::Op::Store) assign_one(instr.name);
            }
        }
    }

    void assign_callee_saved_slots() {
        for (Reg r : abi::kPromotionPool) {
            for (const auto& kv : promoted_) {
                if (kv.second == r) {
                    callee_saved_slots_.push_back({r, allocate_new_slot()});
                    break;
                }
            }
        }
    }

    // Flat instruction indices of every Call, using the SAME
    // block-then-instruction counting scheme as liveness.h, so the
    // indices line up with LiveRange.birth/last_use.
    void find_call_indices() {
        int idx = 0;
        for (const auto& block : fn_.blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op == lithon::ir::Op::Call) call_indices_.push_back(idx);
                ++idx;
            }
        }
    }

    // True if [birth, last_use] genuinely SPANS a call -- defined
    // strictly before it and used strictly after it. A value that IS
    // the call's own result (birth == call_idx) or that is merely an
    // ARGUMENT to the call (last_use == call_idx) is not spanning --
    // both are safe in a caller-saved register.
    bool spans_a_call(const LiveRange& range) const {
        for (int call_idx : call_indices_) {
            if (range.birth < call_idx && range.last_use > call_idx) return true;
        }
        return false;
    }

    void assign_temporary_locations(const std::unordered_set<lithon::ir::ValueId>& virtual_temps) {
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> entries;
        for (const auto& kv : liveness_.ranges()) {
            if (!virtual_temps.count(kv.first)) entries.push_back(kv);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.second.birth != b.second.birth ? a.second.birth < b.second.birth
                                                    : a.first < b.first;
        });

        std::vector<Reg> free_regs(abi::kTempPool.begin(), abi::kTempPool.end());
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> active;

        for (const auto& entry : entries) {
            lithon::ir::ValueId id = entry.first;
            const LiveRange& range = entry.second;

            active.erase(std::remove_if(active.begin(), active.end(), [&](const auto& a) {
                if (a.second.last_use < range.birth) {
                    auto loc_it = temp_locations_.find(a.first);
                    if (loc_it != temp_locations_.end() && loc_it->second.in_register) {
                        free_regs.push_back(loc_it->second.reg);
                    }
                    return true;
                }
                return false;
            }), active.end());

            if (!spans_a_call(range) && !free_regs.empty()) {
                // Lowest-numbered pool order first, for deterministic output.
                auto pick = std::min_element(free_regs.begin(), free_regs.end(), [](Reg a, Reg b) {
                    auto rank = [](Reg r) {
                        for (size_t i = 0; i < abi::kTempPool.size(); ++i)
                            if (abi::kTempPool[i] == r) return i;
                        return abi::kTempPool.size();
                    };
                    return rank(a) < rank(b);
                });
                Reg r = *pick;
                free_regs.erase(pick);
                temp_locations_[id] = ValueLocation{true, r, -1};
            } else {
                temp_locations_[id] = ValueLocation{false, Reg::RAX, allocate_new_slot()};
            }
            active.push_back(entry);
        }

        int total_bytes = -next_slot_offset_;
        frame_size_ = ((total_bytes + 15) / 16) * 16;
    }
};

} // namespace lithon::jit
