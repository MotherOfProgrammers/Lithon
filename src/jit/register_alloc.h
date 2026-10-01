#pragma once

#include <algorithm>
#include <array>
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
    // Nothing is virtual, so every %N temporary gets a real location.
    explicit RegisterAllocator(const lithon::ir::Function& fn)
        : RegisterAllocator(fn, select_promoted_variables(fn), {}) {}

    // Full form: explicit control over which variables are promoted and
    // which %N temporaries need no location at all. `virtual_temps` is
    // plan_function's set, threaded straight into LivenessAnalysis so a
    // value with no run-time existence is excluded BEFORE ranges are
    // computed, and used again here to leave it unallocated. One set, one
    // source of truth, and the two uses cannot drift apart.
    //
    // `float_values` are the ValueIds the caller proved are doubles (via
    // print_guard's Kind lattice). Empty means "no floats in this
    // function", which is every pre-float caller, so they keep working
    // unchanged and pay nothing.
    RegisterAllocator(const lithon::ir::Function& fn, PromotionMap promoted,
                      const VirtualTemps& virtual_temps, bool borrow = true,
                      const std::vector<lithon::ir::ValueId>& float_values = {})
        : fn_(fn), liveness_(fn, virtual_temps), promoted_(std::move(promoted)),
          borrow_(borrow) {
        assign_variable_slots();
        assign_callee_saved_slots();
        find_call_indices();
        assign_temporary_locations(virtual_temps);
        if (!float_values.empty()) assign_float_locations(float_values, virtual_temps);
        finalize_frame_size();
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

    // --- float (XMM) allocation, queried only for values the caller
    // proved are doubles. float_in_register() false means the value
    // lives in float_temp_slot(); there is no third state.
    bool is_float(lithon::ir::ValueId id) const {
        return float_temp_reg_.count(id) || float_spilled_.count(id);
    }

    bool float_in_register(lithon::ir::ValueId id) const {
        return float_temp_reg_.count(id) != 0;
    }

    Xmm float_register(lithon::ir::ValueId id) const {
        static const Xmm none = Xmm::XMM0;
        auto it = float_temp_reg_.find(id);
        return it != float_temp_reg_.end() ? it->second : none;
    }

    int float_stack_slot(lithon::ir::ValueId id) const {
        auto it = float_temp_slot_.find(id);
        return it != float_temp_slot_.end() ? it->second : 0;
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
    bool borrow_ = true;
    std::unordered_map<lithon::ir::ValueId, ValueLocation> temp_locations_;
    std::vector<int> call_indices_;
    int next_slot_offset_ = 0;
    int frame_size_ = 0;

    // The FP mirror of the GP allocation above. Every value the print
    // guard proved is a float gets an XMM location here, and a value
    // that is NOT a float never appears in these maps at all, so the
    // two worlds cannot collide: a GP register and an XMM register are
    // different storage, and separate maps make that structural rather
    // than a convention someone has to remember.
    std::unordered_map<lithon::ir::ValueId, Xmm> float_temp_reg_;
    std::unordered_map<lithon::ir::ValueId, int> float_temp_slot_;
    std::unordered_set<lithon::ir::ValueId> float_spilled_;

    int allocate_new_slot() {
        next_slot_offset_ -= 8;
        return next_slot_offset_;
    }

    // Float spills occupy 8 bytes exactly like GP ones, so they share
    // the one downward-growing frame cursor rather than a second one:
    // two cursors over the same frame is how slots start overlapping.
    int allocate_float_slot() { return allocate_new_slot(); }

    static bool is_temp_pool_reg(Reg r) {
        for (Reg t : abi::kTempPool) if (t == r) return true;
        return false;
    }

    // The lowest-indexed member of `pool` present in `free`, so allocation
    // output is deterministic run to run. `free` must be non-empty: there is
    // no sentinel register to return, because RAX is itself a legitimate
    // member of kTempPool and would be indistinguishable from "none".
    template <size_t N>
    static Reg take_lowest(const std::vector<Reg>& free, const std::array<Reg, N>& pool) {
        return *std::min_element(free.begin(), free.end(), [&](Reg a, Reg b) {
            auto rank = [&](Reg r) {
                for (size_t i = 0; i < N; ++i) if (pool[i] == r) return i;
                return N;
            };
            return rank(a) < rank(b);
        });
    }

    // Callee-saved registers no promoted variable is using. A promoted
    // variable's register is reserved for the whole function, so it can
    // never also hold a temporary. borrow_ is false only to A/B this one
    // change in benchmarks; production always borrows.
    std::vector<Reg> callee_saved_borrowable() const {
        std::vector<Reg> out;
        if (!borrow_) return out;
        for (Reg r : abi::kPromotionPool) {
            bool taken = false;
            for (const auto& kv : promoted_) if (kv.second == r) taken = true;
            if (!taken) out.push_back(r);
        }
        return out;
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

    // Assign an XMM register or a stack slot to every value `float_values`
    // proves is a double, reusing the same live-range sweep as the GP
    // side. Deliberately simpler than assign_temporary_locations: a
    // float temp is never made to survive a call in a register, because
    // every register in kFloatTempPool is caller-saved and borrowing a
    // callee-saved XMM would mean differing between the two host ABIs
    // (XMM6-15 disagree) for no measured gain. A float live across a
    // call therefore spills, which is correct and cheap.
    //
    // The spilling is not optional: host_format_double is an ordinary C
    // function, so it clobbers every caller-saved XMM. A float left in
    // one across a `call print` reads back as whatever the formatter
    // happened to leave there -- silent data corruption, not a crash.
    void assign_float_locations(const std::vector<lithon::ir::ValueId>& float_values,
                                const VirtualTemps& virtual_temps) {
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> entries;
        for (lithon::ir::ValueId id : float_values) {
            auto it = liveness_.ranges().find(id);
            // A value with no run-time existence (a constant folded away
            // into its use site) has no range and needs no location.
            if (it == liveness_.ranges().end()) continue;
            if (virtual_temps.count(id)) continue;
            entries.push_back({id, it->second});
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.second.birth != b.second.birth ? a.second.birth < b.second.birth
                                                    : a.first < b.first;
        });

        // `free_xmm` tracks which registers are free *right now*, for the
        // benefit of later entries. It is deliberately NOT the same thing as
        // float_temp_reg_, which is the permanent record of where each value
        // lives. Erasing a value from float_temp_reg_ when its live range ends
        // would be wrong: codegen runs after allocation, and it asks about a
        // value's location while emitting that value's own definition and its
        // uses. A value whose range has ended is simply never asked about
        // again, so the record has to survive. (The GP sweep above keeps
        // temp_locations_ the same way, and only recycles the register into
        // free_regs.)
        std::vector<Xmm> free_xmm(abi::kFloatTempPool.begin(), abi::kFloatTempPool.end());
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> active;

        for (const auto& entry : entries) {
            lithon::ir::ValueId id = entry.first;
            active.erase(std::remove_if(active.begin(), active.end(), [&](const auto& a) {
                if (a.second.last_use < entry.second.birth) {
                    // Recycle the register, but leave float_temp_reg_ and
                    // float_temp_slot_ alone: those say where the value
                    // lives, not whether it is still live.
                    auto it = float_temp_reg_.find(a.first);
                    if (it != float_temp_reg_.end()) free_xmm.push_back(it->second);
                    return true;
                }
                return false;
            }), active.end());

            // A value whose range spans a call cannot sit in a
            // caller-saved XMM across it, so it goes straight to a slot
            // even when registers are free. (Freeing its register below
            // still happens on the range-end sweep, so the register is
            // not leaked into a permanently unusable state.)
            const bool must_spill = spans_a_call(entry.second);
            if (!free_xmm.empty() && !must_spill) {
                Xmm r = *std::min_element(free_xmm.begin(), free_xmm.end(),
                                          [](Xmm a, Xmm b) {
                                              return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
                                          });
                free_xmm.erase(std::find(free_xmm.begin(), free_xmm.end(), r));
                float_temp_reg_[id] = r;
            } else {
                // Either every register in the pool is live across this
                // value's birth, or the value outlives a call. Either way
                // it needs a stack slot of its own.
                float_spilled_.insert(id);
                float_temp_slot_[id] = allocate_float_slot();
            }
            active.push_back(entry);
        }
    }

    void assign_temporary_locations(const VirtualTemps& virtual_temps) {
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> entries;
        for (const auto& kv : liveness_.ranges()) {
            if (!virtual_temps.count(kv.first)) entries.push_back(kv);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.second.birth != b.second.birth ? a.second.birth < b.second.birth
                                                    : a.first < b.first;
        });

        // Two pools, because a %N temp has two different constraints. A temp
        // that does not cross a call can live anywhere in kTempPool: those
        // are caller-saved, and the only thing that matters is that they
        // stay clear of argument registers and the r10/r11 scratch. A temp
        // that DOES cross a call cannot go there at all -- the callee is
        // entitled to destroy every one of them. Spilling it to the stack is
        // always correct but costs a store and a reload straddling the call,
        // which is a real memory round-trip, not a fused one. Any callee-saved
        // register not already holding a promoted variable is strictly
        // better: the call cannot touch it, and the prologue/epilogue already
        // save and restore the promoted set, so extending that list costs one
        // store and one load for the entire function.
        std::vector<Reg> free_regs(abi::kTempPool.begin(), abi::kTempPool.end());
        std::vector<Reg> free_across_calls = callee_saved_borrowable();
        std::vector<std::pair<lithon::ir::ValueId, LiveRange>> active;

        for (const auto& entry : entries) {
            lithon::ir::ValueId id = entry.first;
            const LiveRange& range = entry.second;

            active.erase(std::remove_if(active.begin(), active.end(), [&](const auto& a) {
                if (a.second.last_use < range.birth) {
                    auto loc_it = temp_locations_.find(a.first);
                    if (loc_it != temp_locations_.end() && loc_it->second.in_register) {
                        // Return a borrowed callee-saved register to its own
                        // pool; it must never re-enter the temp pool, or a
                        // later temp would be handed a register a call can
                        // destroy.
                        if (is_temp_pool_reg(loc_it->second.reg)) free_regs.push_back(loc_it->second.reg);
                        else free_across_calls.push_back(loc_it->second.reg);
                    }
                    return true;
                }
                return false;
            }), active.end());

            if (spans_a_call(range) && !free_across_calls.empty()) {
                // A call cannot destroy a callee-saved register, so this is
                // strictly better than the stack round-trip it replaces.
                Reg r = take_lowest(free_across_calls, abi::kPromotionPool);
                free_across_calls.erase(std::find(free_across_calls.begin(), free_across_calls.end(), r));
                temp_locations_[id] = ValueLocation{true, r, -1};
                // Saving it is what makes the borrow legal: the prologue now
                // stores it and every Return reloads it, so the caller's
                // value survives the call.
                callee_saved_slots_.push_back({r, allocate_new_slot()});
            } else if (!spans_a_call(range) && !free_regs.empty()) {
                Reg r = take_lowest(free_regs, abi::kTempPool);
                free_regs.erase(std::find(free_regs.begin(), free_regs.end(), r));
                temp_locations_[id] = ValueLocation{true, r, -1};
            } else {
                temp_locations_[id] = ValueLocation{false, Reg::RAX, allocate_new_slot()};
            }
            active.push_back(entry);
        }
    }

    // Computed once, after BOTH the GP and float passes have handed out
    // slots. It has to be last: the float pass allocates from the same
    // downward cursor, so a frame size computed before it ran would be
    // too small and the float spills would land outside the frame.
    void finalize_frame_size() {
        int total_bytes = -next_slot_offset_;
        frame_size_ = ((total_bytes + 15) / 16) * 16;
    }
};

} // namespace lithon::jit
