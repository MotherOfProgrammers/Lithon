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
#include "ssa.h"
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
    // Depth comes from the CFG analysis (header dominance), not from a textual
    // block range. A loop whose body is not contiguous -- which is exactly what
    // accumulator_unroll produces by jamming the main loop after the remainder
    // -- used to score as depth 0 and lose its variables to a colder rival.
    const LoopInfo loops = compute_loop_info(fn);

    std::unordered_map<std::string, double> weight;
    for (const auto& p : fn.params) weight[p] += 1.0;   // entry store

    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        int depth = 0;
        for (const auto& loop : loops.loops) {
            if (loop.contains(b)) ++depth;
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

// 2.5. Hands out the callee-saved registers a promoted variable did not take to
// resolved Phi variables, so each incoming edge stores with `mov reg, reg`
// instead of touching memory. Only the first Phis are served, in program order;
// past the pool's size the rest keep the memory path, which is slower but
// exactly as correct.
//
// Real variables are ranked and placed first, deliberately. That ranking is
// weighted by loop depth, so the values that benefit most keep their registers;
// a Phi is one join, and is rarely the hotter side of that trade.
inline PromotionMap select_phi_registers(const lithon::ir::Function& fn,
                                          const PromotionMap& taken) {
    std::unordered_set<Reg> used;
    for (const auto& kv : taken) used.insert(kv.second);

    PromotionMap phis;
    for (const auto& var : phi_copy_variables(fn)) {
        if (phis.count(var) || taken.count(var)) continue;
        for (Reg r : abi::kPromotionPool) {
            if (used.count(r)) continue;
            phis[var] = r;
            used.insert(r);
            break;
        }
    }
    return phis;
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
        temp_pool_.assign(abi::kTempPool.begin(), abi::kTempPool.end());
        assign_variable_slots();
        assign_temporary_locations(virtual_temps);
        // After the temporaries, because a merge's source register is only known
        // once they are allocated; and after the variables, because a merge's
        // old callee-saved register has to be released back to the frame.
        // Before assign_callee_saved_slots, which must save exactly the set the
        // FINAL promoted map names -- a save decided from the pre-coalescing map
        // would either miss a register now in use or keep one that is not.
        phi_copies_coalesced_ = coalesce_phi_registers();
        assign_callee_saved_slots();
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

    // 2.8. How many merges ended up sharing a dead source's register, so the
    // copy that edge would have emitted was elided instead. Zero is a normal
    // answer, not a failure: a merge fed by a constant has nothing to share.
    size_t phi_copies_coalesced() const { return phi_copies_coalesced_; }

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
    // Callee-saved registers the TEMPORARIES borrowed (they must survive a call,
    // so they cannot use a caller-saved one). Kept apart from callee_saved_slots_
    // because coalescing rewrites promoted_ in between the two passes, and the
    // frame has to be derived from the final map -- see assign_callee_saved_slots.
    std::vector<Reg> borrowed_saved_;
    // 2.8. Merges whose destination register was replaced by a dead source's.
    size_t phi_copies_coalesced_ = 0;
    bool borrow_ = true;
    std::vector<Reg> temp_pool_;
    std::unordered_map<lithon::ir::ValueId, ValueLocation> temp_locations_;
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

    // One save per register, not one per name that holds it: after coalescing a
    // register can be reached from a promoted variable AND from a merge that was
    // given a temporary's register, and pushing it twice would still balance --
    // two pops for two pushes -- but it would also burn a second frame slot for a
    // slot that already exists. Membership is keyed on the REGISTER because the
    // register is what the prologue actually has to preserve.
    void assign_callee_saved_slots() {
        for (Reg r : abi::kPromotionPool) {
            bool needed = std::find(borrowed_saved_.begin(), borrowed_saved_.end(), r) !=
                          borrowed_saved_.end();
            if (!needed) {
                for (const auto& kv : promoted_) {
                    if (kv.second == r) { needed = true; break; }
                }
            }
            if (needed) callee_saved_slots_.push_back({r, allocate_new_slot()});
        }
    }

    // Flat instruction indices of every Call, using the SAME
    // block-then-instruction counting scheme as liveness.h, so the
    // indices line up with LiveRange.birth/last_use.
    // Whether a value survives a Call is decided by the CFG liveness above,
    // which knows which blocks a call can execute on and whether the value is
    // live across them. The previous test compared flat instruction indices --
    // "is a number between two other numbers" -- which is not the same question
    // once a loop is involved, and was the reason this allocator needed the
    // loop-span fixup at all. See LiveRange::spans_call.

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
        for (lithon::ir::ValueId id : float_values) {
            if (virtual_temps.count(id)) continue;
            auto it = liveness_.ranges().find(id);
            // A value with no run-time existence (a constant folded away into
            // its use site) has no range and needs no location.
            if (it == liveness_.ranges().end()) continue;
            const LiveRange& range = it->second;

            // A float temp is never made to survive a call in a register,
            // because every register in kFloatTempPool is caller-saved and
            // borrowing a callee-saved XMM would mean differing between the two
            // host ABIs (XMM6-15 disagree) for no measured gain.
            //
            // The spilling is not optional: host_format_double is an ordinary
            // C function, so it clobbers every caller-saved XMM. A float left
            // in one across a `call print` reads back as whatever the formatter
            // happened to leave there -- silent data corruption, not a crash.
            Xmm chosen = Xmm::XMM0;
            bool found = false;
            if (!range.spans_call) {
                for (Xmm r : abi::kFloatTempPool) {
                    if (float_held_by_neighbour(id, r)) continue;
                    chosen = r;
                    found = true;
                    break;
                }
            }
            if (found) {
                float_temp_reg_[id] = chosen;
            } else {
                // Either every register in the pool is live at once with this
                // value, or the value outlives a call. Either way it needs a
                // stack slot of its own.
                float_spilled_.insert(id);
                float_temp_slot_[id] = allocate_float_slot();
            }
        }
    }

    bool float_held_by_neighbour(lithon::ir::ValueId id, Xmm r) const {
        for (const auto& kv : float_temp_reg_) {
            if (kv.second != r) continue;
            if (liveness_.interferes(id, kv.first)) return true;
        }
        return false;
    }

    // Greedy colouring of the interference graph.
    //
    // Values are coloured in definition order (RPO block, then position), each
    // taking the lowest-numbered register in its pool that none of its
    // already-coloured neighbours holds; if every register in the pool is taken
    // by a neighbour, the value spills.
    //
    // This replaced a linear sweep over instruction indices with two
    // assumptions that no longer hold: that the flat order was a legal
    // execution order (it is not, inside a loop) and that two values
    // interfere exactly when their flat index ranges overlap (they do not -- a
    // definition can precede its own first read, and a value can be carried
    // around a back edge). Both are now expressed by real interference edges.
    void assign_temporary_locations(const VirtualTemps& virtual_temps) {
        for (lithon::ir::ValueId id : liveness_.allocation_order()) {
            if (virtual_temps.count(id)) continue;
            auto it = liveness_.ranges().find(id);
            if (it == liveness_.ranges().end()) continue;
            const LiveRange& range = it->second;

            // Two pools, because a %N temp has two different constraints. A
            // temp that does not cross a call can live anywhere in kTempPool:
            // those are caller-saved, and the only thing that matters is that
            // they stay clear of argument registers and the r10/r11 scratch. A
            // temp that DOES cross a call cannot go there at all -- the callee
            // is entitled to destroy every one of them. Spilling it to the
            // stack is always correct but costs a store and a reload
            // straddling the call, which is a real memory round-trip, not a
            // fused one. Any callee-saved register not already holding a
            // promoted variable is strictly better: the call cannot touch it,
            // and the prologue/epilogue already save and restore the promoted
            // set, so extending that list costs one store and one load for the
            // entire function.
            // Pool order is the declared order, and the first free register
            // in it wins -- so a given function allocates identically from one
            // run to the next.
            const std::vector<Reg> across_calls = callee_saved_borrowable();
            const std::vector<Reg>& pool = range.spans_call ? across_calls : temp_pool_;
            Reg chosen = Reg::RAX;
            bool found = false;
            for (Reg r : pool) {
                if (gp_held_by_neighbour(id, r)) continue;
                chosen = r;
                found = true;
                break;
            }
            if (!found) {
                temp_locations_[id] = ValueLocation{false, Reg::RAX, allocate_new_slot()};
                continue;
            }
            temp_locations_[id] = ValueLocation{true, chosen, -1};
            // Borrowed callee-saved registers must be saved and restored; a
            // caller-saved one needs nothing. Recorded separately, because
            // assign_callee_saved_slots runs AFTER coalescing and has to fold
            // this together with the promoted set without saving anything twice.
            if (range.spans_call) borrowed_saved_.push_back(chosen);
        }
    }

    // True if an already-placed neighbour of `id` holds `r`. Walking the
    // interference set rather than keeping a per-register occupant list means
    // the answer is derived from the same graph the allocation is colouring, so
    // the two cannot disagree about who holds what. Values are visited in
    // allocation order, so everything in temp_locations_ is already decided.
    bool gp_held_by_neighbour(lithon::ir::ValueId id, Reg r) const {
        for (const auto& kv : temp_locations_) {
            if (kv.second.in_register && kv.second.reg == r && liveness_.interferes(id, kv.first))
                return true;
        }
        return false;
    }

    // 2.8. Give a merge's destination the register its source already occupies
    // on one incoming edge, so that edge's `mov dst, src` has nothing to do:
    // `materialize_into` already elides a move whose source and destination are
    // the same register, so the copy disappears without any new codegen.
    //
    // Why it has to run HERE, after the temporaries: the register being claimed
    // is a TEMPORARY's, and only allocation knows it. Every condition below is
    // about that one register's occupancy, so this is a question about the
    // interference graph, and the graph exists by now.
    //
    // Why it is safe, in one sentence: the source is dead the instant its store
    // retires, so the only thing the register still has to hold between that
    // store and the merge's load is the merge itself.
    size_t coalesce_phi_registers() {
        using namespace lithon::ir;
        const Cfg g = build_cfg(fn_);

        // RPO position per block, so a value's [lo,hi] and a join's extent can
        // be compared as intervals. Unreachable blocks get -1 and drop out of
        // every range, which is what we want: nothing can be live there.
        std::vector<int> rpo_pos(fn_.blocks.size(), -1);
        {
            // liveness_'s own RPO, not a second computation of it. Two reverse
            // postorders of one CFG agree today, and the day they disagree this
            // pass would compare positions from different orderings and silently
            // mis-size a live range -- so there is exactly one source.
            const std::vector<size_t>& rpo = liveness_.reverse_postorder();
            for (size_t i = 0; i < rpo.size(); ++i) rpo_pos[rpo[i]] = static_cast<int>(i);
        }

        // Flat instruction index, in liveness.h's exact counting scheme: block
        // order, then instruction order. It has to be the same numbering or the
        // `last_use == store` comparison below compares two unrelated numbers --
        // and it is the one comparison the whole pass rests on.
        std::vector<int> flat(fn_.blocks.size());
        {
            int idx = 0;
            for (size_t b = 0; b < fn_.blocks.size(); ++b) {
                flat[b] = idx;
                idx += static_cast<int>(fn_.blocks[b].instrs.size());
            }
        }

        // A register one merge already claimed. Two merges reaching the SAME join
        // are both live at that join's load, so they cannot share; claiming
        // globally is cruder than that and rejects some pairs that are in fact
        // far apart, which costs a copy that could have gone but never costs
        // correctness.
        std::unordered_set<Reg> claimed;
        size_t coalesced = 0;

        for (const auto& var : phi_copy_variables(fn_)) {
            const auto pit = promoted_.find(var);
            if (pit == promoted_.end()) continue;

            // The join: the one block that loads this merge's variable back.
            size_t join = fn_.blocks.size();
            int load_pos = -1;
            for (size_t b = 0; b < fn_.blocks.size(); ++b) {
                for (size_t i = 0; i < fn_.blocks[b].instrs.size(); ++i) {
                    const auto& in = fn_.blocks[b].instrs[i];
                    if (in.op == Op::Load && in.name == var) { join = b; load_pos = static_cast<int>(i); }
                }
            }
            if (join == fn_.blocks.size()) continue;

            // The extent the merge is live over: from each incoming edge's store
            // to the join's load. Every store on every edge writes this one
            // register, so the extent is the UNION of the predecessors plus the
            // join -- not the single edge whose source we happen to be adopting.
            int lo = rpo_pos[join];
            int hi = rpo_pos[join];
            for (size_t pb : g.pred[join]) {
                const int p = rpo_pos[pb];
                if (p < 0) continue;
                lo = std::min(lo, p);
                hi = std::max(hi, p);
            }

            // Does the merge outlive a call? Only then does its register have to
            // be callee-saved, and only then is a caller-saved source register
            // disqualified. The call that matters is one the merge can meet: after
            // some store, or before the join's load.
            bool spans_call = false;
            for (size_t pb : g.pred[join]) {
                bool store_seen = false;
                for (const auto& in : fn_.blocks[pb].instrs) {
                    if (in.op == Op::Store && in.name == var) { store_seen = true; continue; }
                    if (store_seen && in.op == Op::Call) { spans_call = true; break; }
                }
                if (spans_call) break;
            }
            if (!spans_call) {
                for (int i = 0; i < load_pos; ++i)
                    if (fn_.blocks[join].instrs[i].op == Op::Call) { spans_call = true; break; }
            }

            for (size_t pb : g.pred[join]) {
                const Instr* store = nullptr;
                size_t store_pos = 0;
                for (size_t i = 0; i < fn_.blocks[pb].instrs.size(); ++i) {
                    const auto& in = fn_.blocks[pb].instrs[i];
                    if (in.op == Op::Store && in.name == var && in.args.size() == 1) {
                        store = &in;
                        store_pos = i;
                    }
                }
                if (!store) continue;
                const ValueId src = store->args[0];
                const int store_idx = flat[pb] + static_cast<int>(store_pos);

                // A constant, a fused operand or an alias has no register at all
                // -- there is nothing to share, and the copy that remains is the
                // cheapest kind there is: an immediate into the merge's register.
                const ValueLocation& sloc = temp_location(src);
                if (!sloc.in_register) continue;

                // The whole safety argument, as one comparison. `last_use` is the
                // highest instruction that mentions the value, and the store
                // mentions it, so equality says: nothing after this store wants
                // this register. The merge is now the only thing that does.
                const auto rit = liveness_.ranges().find(src);
                if (rit == liveness_.ranges().end() || rit->second.last_use != store_idx) continue;

                const Reg target = sloc.reg;
                if (claimed.count(target)) continue;
                if (spans_call && is_temp_pool_reg(target)) continue;

                // Somebody else may already hold this register too -- the
                // allocator shares a register between values that do not
                // interfere. Sharing is fine only for whoever is dead before the
                // merge needs the register, so every OTHER holder's live range
                // must miss the merge's extent entirely.
                bool clash = false;
                for (const auto& kv : temp_locations_) {
                    if (kv.first == src || !kv.second.in_register || kv.second.reg != target) continue;
                    const auto trit = liveness_.ranges().find(kv.first);
                    if (trit == liveness_.ranges().end()) continue;
                    if (trit->second.lo <= hi && lo <= trit->second.hi) { clash = true; break; }
                }
                if (clash) continue;

                promoted_[var] = target;
                claimed.insert(target);
                ++coalesced;
                break;   // one register per merge; the first passing edge wins
            }
        }
        return coalesced;
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
