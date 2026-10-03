#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "loop_info.h"

// 2.2  Phi placement (SSA foundation).
//
// The IR keeps mutable variables in memory: a variable is written by Store
// and read by Load, exactly like a register spill slot. Before those can be
// promoted to real SSA values (2.3 Mem2Reg), the join points where two
// reaching definitions meet need a Phi. This header decides *where* those
// phis go, using the textbook construction:
//
//   DF[b]  = blocks y with a predecessor p that b dominates, where b does
//            not strictly dominate y (Cytron et al., "Efficiently Computing
//            Static Single Assignment Form")
//   phi(v) = for each def block of v, add v at every block in the iterated
//            dominance frontier of the defs, iterating on newly added blocks
//
// Placement is analysis-only here: the result names the (block, variable)
// pairs a rewriter must materialize. It does NOT mutate the function, because
// no codegen path emits Op::Phi yet -- inserting placeholder phis would leave
// the module uncompilable. compute_loop_info() supplies the CFG, dominators,
// and loop structure, so this and every later SSA pass agree on the CFG.

namespace lithon::jit {

struct DominanceFrontiers {
    std::vector<std::unordered_set<size_t>> df;

    bool contains(size_t b, size_t y) const {
        return b < df.size() && df[b].count(y) != 0;
    }
};

inline DominanceFrontiers compute_dominance_frontiers(const Cfg& g, const DomInfo& dom) {
    DominanceFrontiers out;
    out.df.assign(g.size(), {});
    for (size_t y = 0; y < g.size(); ++y) {
        if (g.pred[y].size() < 2) continue;   // a join only exists with 2+ preds
        const size_t stop = dom.idom[y];
        for (size_t p : g.pred[y]) {
            size_t runner = p;
            while (runner != kNoBlock && runner != stop) {
                out.df[runner].insert(y);
                runner = dom.idom[runner];
            }
        }
    }
    return out;
}

// Variables a Phi may be needed for: every variable written by a Store. A
// variable is identified by name, which is how Load/Store already address it.
inline std::unordered_map<std::string, std::vector<size_t>>
store_blocks_by_variable(const lithon::ir::Function& fn) {
    std::unordered_map<std::string, std::vector<size_t>> defs;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const auto& in : fn.blocks[b].instrs)
            if (in.op == lithon::ir::Op::Store && !in.name.empty()) defs[in.name].push_back(b);
    for (auto& kv : defs) {
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
    }
    return defs;
}

struct PhiPlacement {
    std::unordered_map<std::string, std::vector<size_t>> phi_blocks;  // var -> sorted join blocks
    std::vector<std::vector<std::string>> block_phis;                 // block -> sorted variables

    bool needs_phi(size_t block, const std::string& var) const {
        auto it = phi_blocks.find(var);
        if (it == phi_blocks.end()) return false;
        return std::binary_search(it->second.begin(), it->second.end(), block);
    }

    size_t phi_count() const {
        size_t n = 0;
        for (const auto& kv : phi_blocks) n += kv.second.size();
        return n;
    }
};

inline PhiPlacement compute_phi_placement(const lithon::ir::Function& fn,
                                          const std::unordered_set<std::string>* only = nullptr) {
    PhiPlacement placement;
    placement.block_phis.assign(fn.blocks.size(), {});
    if (fn.blocks.empty()) return placement;

    const Cfg g = build_cfg(fn);
    const DomInfo dom = compute_dominators(g);
    const DominanceFrontiers frontiers = compute_dominance_frontiers(g, dom);
    const auto defs = store_blocks_by_variable(fn);

    for (const auto& kv : defs) {
        const std::string& var = kv.first;
        if (only && !only->count(var)) continue;
        const std::vector<size_t>& def_blocks = kv.second;

        std::unordered_set<size_t> has_phi;
        std::vector<size_t> work(def_blocks.begin(), def_blocks.end());
        std::unordered_set<size_t> in_work(work.begin(), work.end());
        while (!work.empty()) {
            size_t x = work.back();
            work.pop_back();
            in_work.erase(x);
            for (size_t y : frontiers.df[x]) {
                if (!has_phi.insert(y).second) continue;   // already placed here
                placement.phi_blocks[var].push_back(y);
                // A phi is itself a def, so its own frontier may need phis too.
                if (!std::binary_search(def_blocks.begin(), def_blocks.end(), y)) {
                    work.push_back(y);
                    in_work.insert(y);
                }
            }
        }

        auto& blocks = placement.phi_blocks[var];
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
        for (size_t b : blocks) placement.block_phis[b].push_back(var);
    }

    for (auto& vars : placement.block_phis) std::sort(vars.begin(), vars.end());
    return placement;
}

// ---------------------------------------------------------------------------
// 2.3  Mem2Reg: materialize the phis and rename Load/Store to SSA values.
//
// The result is a function in SSA form: each promoted variable is written
// exactly once (a Store becomes the definition of the stored value, a Load is
// replaced by the current definition), with a Phi at every join.
//
// Parameters are deliberately NOT promoted: the IR has no Param op, so their
// incoming value has no ValueId to seed the rename; they stay memory-based.
//
// Everything else is promotable when promotable_variables() can prove it. The
// original rule was "stored in the entry block", which is safe but far too
// narrow: it declines the single most important shape there is, an if used as
// an expression, where the value is defined in *both* merge arms and nowhere
// else. The rule now is "every read is reached by a definition on every path"
// (see definitely_assigned), which promotes that case and still refuses the
// ones SSA cannot express -- reading an uninitialized variable, or defining a
// variable only inside a loop, where the loop-header Phi would need an
// incoming value from before the loop.
//
// This is not wired into the backend by default: no codegen path emits
// Op::Phi. resolve_phis() is the emitter's copy-resolution stage, and
// CompileOptions::ssa_pipeline runs the whole thing before codegen.
// ---------------------------------------------------------------------------

struct SsaStats {
    size_t vars_promoted = 0;
    size_t phis_materialized = 0;
    size_t loads_removed = 0;
    size_t stores_removed = 0;
    size_t vars_declined = 0;
};

// Forward "definitely assigned" dataflow. before[b] is the set of variables
// guaranteed assigned on entry to block b; after[b] the same on exit. The
// meet over predecessors is INTERSECTION: a variable is definitely assigned
// only if every way in assigns it. Unreachable blocks get the empty set, which
// is the conservative answer.
//
// The iteration counts DOWN from "everything" rather than up from nothing, and
// that is what makes it precise on loops. A variable assigned in a preheader
// and read inside the loop is genuinely assigned on every path to the read,
// including the ones that arrive over the back edge -- but iterating upward,
// the header's set would collapse to the meet of {preheader} and {back edge},
// where the back edge set has not yet picked the variable up, and it would stay
// collapsed forever. Downward iteration converges on the greatest fixpoint,
// which keeps it. Promotion needs the precise answer, because the alternative
// is silently refusing to promote ordinary loop accumulators.
struct AssignedAnalysis {
    std::vector<std::unordered_set<std::string>> before;
    std::vector<std::unordered_set<std::string>> after;
    std::vector<bool> reachable;

    bool assigned_at_entry(size_t b, const std::string& var) const {
        return b < before.size() && before[b].count(var) != 0;
    }
};

inline AssignedAnalysis compute_assigned(const lithon::ir::Function& fn, const Cfg& g,
                                        const DomInfo& dom) {
    using lithon::ir::Op;
    AssignedAnalysis a;
    const size_t n = g.size();
    a.reachable = dom.reachable;

    std::vector<std::unordered_set<std::string>> stored(n);
    std::unordered_set<std::string> all_vars;
    for (size_t b = 0; b < n; ++b) {
        for (const auto& in : fn.blocks[b].instrs) {
            if (in.op == Op::Store && !in.name.empty()) {
                stored[b].insert(in.name);
                all_vars.insert(in.name);
            }
        }
    }

    a.before.assign(n, all_vars);
    a.after.assign(n, all_vars);
    for (size_t b = 0; b < n; ++b)
        if (!a.reachable[b]) { a.before[b].clear(); a.after[b].clear(); }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t b = 0; b < n; ++b) {
            if (!a.reachable[b]) continue;
            std::unordered_set<std::string> entry_set;
            bool first = true;
            bool dead = false;
            for (size_t p : g.pred[b]) {
                if (!a.reachable[p]) { dead = true; break; }
                std::unordered_set<std::string> inter;
                if (first) {
                    inter = a.after[p];
                } else {
                    for (const auto& v : entry_set)
                        if (a.after[p].count(v)) inter.insert(v);
                }
                entry_set.swap(inter);
                first = false;
            }
            if (dead || first) entry_set.clear();   // unreachable edge, or no preds

            std::unordered_set<std::string> exit_set = entry_set;
            exit_set.insert(stored[b].begin(), stored[b].end());
            if (entry_set != a.before[b]) { a.before[b] = entry_set; changed = true; }
            if (exit_set != a.after[b]) { a.after[b] = exit_set; changed = true; }
        }
    }
    return a;
}

// A variable may become an SSA value when, at every read, some definition is
// guaranteed to have run; and when no Phi it needs would have to invent an
// incoming value that does not exist yet (the loop case). Everything else
// stays a memory slot.
inline std::unordered_set<std::string> promotable_variables(const lithon::ir::Function& fn) {
    using lithon::ir::Op;
    if (fn.blocks.empty()) return {};

    const Cfg g = build_cfg(fn);
    const DomInfo dom = compute_dominators(g);
    const AssignedAnalysis assigned = compute_assigned(fn, g, dom);
    const LoopInfo loops = compute_loop_info(fn);
    const std::unordered_set<std::string> params(fn.params.begin(), fn.params.end());

    std::unordered_set<std::string> stored;
    std::unordered_map<std::string, std::vector<size_t>> def_blocks;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const auto& in : fn.blocks[b].instrs)
            if (in.op == Op::Store && !in.name.empty()) {
                stored.insert(in.name);
                def_blocks[in.name].push_back(b);
            }

    std::unordered_set<std::string> out;
    for (const auto& v : stored) {
        if (params.count(v)) continue;

        // (1) every Load of v must be reached by a definition. Intra-block
        // ordering is respected: a store earlier in the block counts.
        bool ok = true;
        for (size_t b = 0; b < fn.blocks.size() && ok; ++b) {
            std::unordered_set<std::string> live_here = assigned.before[b];
            for (const auto& in : fn.blocks[b].instrs) {
                if (in.op == Op::Load && in.name == v && !live_here.count(v)) { ok = false; break; }
                if (in.op == Op::Store && in.name == v) live_here.insert(v);
            }
        }
        if (!ok) continue;

        // (2) no Phi for v may land at a loop header whose value is not yet
        // established on entry -- otherwise the Phi would need an incoming
        // operand for the first iteration, which does not exist.
        for (const auto& loop : loops.loops) {
            bool defined_in_loop = false;
            for (size_t d : def_blocks[v]) {
                if (loop.contains(d)) { defined_in_loop = true; break; }
            }
            if (!defined_in_loop) continue;
            if (!assigned.assigned_at_entry(loop.header, v)) { ok = false; break; }
        }
        if (ok) out.insert(v);
    }
    return out;
}

// Every Phi operand must have a real value: the join at block b reads v off
// each incoming edge, so a predecessor that leaves v unassigned is an edge
// with nothing to pass. Such a read is already an error in memory semantics
// (the interpreter throws "reference to undefined variable"), so the variable
// is declined rather than promoted with a made-up operand. Returns the set of
// variables that cannot be promoted this way.
inline std::unordered_set<std::string> phis_with_undefined_input(
        const lithon::ir::Function& fn, const PhiPlacement& plan, const Cfg& g,
        const AssignedAnalysis& assigned) {
    using lithon::ir::Op;
    std::unordered_set<std::string> bad;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (b >= plan.block_phis.size()) break;
        for (const std::string& var : plan.block_phis[b]) {
            for (size_t p : g.pred[b]) {
                if (!assigned.reachable[p]) continue;
                if (b < assigned.after.size() && assigned.after[p].count(var)) continue;
                bad.insert(var);
            }
        }
    }
    return bad;
}

inline void replace_all_uses(lithon::ir::Function& fn, lithon::ir::ValueId from,
                             lithon::ir::ValueId to) {
    if (from == to) return;
    for (auto& block : fn.blocks)
        for (auto& in : block.instrs)
            for (auto& a : in.args)
                if (a == from) a = to;
}

inline lithon::ir::ValueId next_value_id(const lithon::ir::Function& fn) {
    lithon::ir::ValueId next = 1;
    for (const auto& block : fn.blocks)
        for (const auto& in : block.instrs)
            if (in.result != lithon::ir::kInvalidValue) next = std::max(next, in.result + 1);
    return next;
}

inline SsaStats mem2reg(lithon::ir::Function& fn) {
    using namespace lithon::ir;
    SsaStats stats;
    if (fn.blocks.empty()) return stats;

    std::unordered_set<std::string> promotable = promotable_variables(fn);
    const Cfg g = build_cfg(fn);
    const size_t n = fn.blocks.size();
    const DomInfo dom = compute_dominators(g);
    const auto defs = store_blocks_by_variable(fn);

    // Placement may promise an operand that no edge supplies (a Phi at a join
    // whose dead side never defines the variable). Declining those variables
    // can remove phis for others, so iterate to a fixpoint.
    PhiPlacement plan;
    const AssignedAnalysis assigned = compute_assigned(fn, g, dom);
    while (true) {
        plan = compute_phi_placement(fn, &promotable);
        const auto bad = phis_with_undefined_input(fn, plan, g, assigned);
        if (bad.empty()) break;
        bool removed = false;
        for (const auto& v : bad) removed |= promotable.erase(v) != 0;
        if (!removed || promotable.empty()) break;
    }

    const auto params = std::unordered_set<std::string>(fn.params.begin(), fn.params.end());
    size_t candidates = 0;
    for (const auto& kv : defs) if (!params.count(kv.first)) ++candidates;
    stats.vars_promoted = promotable.size();
    stats.vars_declined = candidates - stats.vars_promoted;
    if (promotable.empty()) return stats;
    const std::vector<std::vector<std::string>>& block_phi_vars = plan.block_phis;

    // Materialize the phis at the top of each block, one operand per
    // predecessor edge (filled during the rename).
    ValueId next = next_value_id(fn);
    std::vector<std::unordered_map<std::string, ValueId>> phi_result(n);
    std::vector<std::unordered_map<std::string, size_t>> phi_index(n);
    std::vector<std::vector<bool>> removed(n);
    for (size_t b = 0; b < n; ++b) {
        std::vector<Instr> phis;
        for (const std::string& var : block_phi_vars[b]) {
            Instr phi;
            phi.op = Op::Phi;
            phi.result = next++;
            phi.name = var;
            phi.args.assign(g.pred[b].size(), kInvalidValue);
            phi_index[b][var] = phis.size();
            phi_result[b][var] = phi.result;
            phis.push_back(std::move(phi));
            ++stats.phis_materialized;
        }
        if (!phis.empty()) {
            auto& instrs = fn.blocks[b].instrs;
            instrs.insert(instrs.begin(), phis.begin(), phis.end());
        }
        removed[b].assign(fn.blocks[b].instrs.size(), false);
    }

    std::vector<std::unordered_map<size_t, size_t>> pred_pos(n);
    for (size_t b = 0; b < n; ++b)
        for (size_t i = 0; i < g.pred[b].size(); ++i) pred_pos[b][g.pred[b][i]] = i;

    std::vector<std::vector<size_t>> children(n);
    for (size_t b = 1; b < n; ++b)
        if (dom.idom[b] != kNoBlock) children[dom.idom[b]].push_back(b);
    for (auto& c : children) std::sort(c.begin(), c.end());

    // Cytron rename over the dominator tree: a per-variable stack of the
    // current definition, pushed at phis and stores, popped on block exit.
    std::unordered_map<std::string, std::vector<ValueId>> stack;
    std::vector<std::vector<std::string>> pushed(n);

    std::function<void(size_t)> dfs = [&](size_t b) {
        for (const std::string& var : block_phi_vars[b]) {
            stack[var].push_back(phi_result[b][var]);
            pushed[b].push_back(var);
        }
        auto& instrs = fn.blocks[b].instrs;
        for (size_t i = 0; i < instrs.size(); ++i) {
            Instr& in = instrs[i];
            if (in.op == Op::Load && promotable.count(in.name)) {
                auto sit = stack.find(in.name);
                if (sit == stack.end() || sit->second.empty()) continue;   // analysis said no
                ValueId cur = sit->second.back();
                replace_all_uses(fn, in.result, cur);
                removed[b][i] = true;
                ++stats.loads_removed;
            } else if (in.op == Op::Store && promotable.count(in.name)) {
                ValueId val = in.args.empty() ? kInvalidValue : in.args[0];
                stack[in.name].push_back(val);
                pushed[b].push_back(in.name);
                removed[b][i] = true;
                ++stats.stores_removed;
            }
        }
        for (size_t s : g.succ[b]) {
            if (block_phi_vars[s].empty()) continue;
            auto pit = pred_pos[s].find(b);
            if (pit == pred_pos[s].end()) continue;
            for (const std::string& var : block_phi_vars[s]) {
                auto sit = stack.find(var);
                if (sit == stack.end() || sit->second.empty()) continue;
                size_t idx = phi_index[s][var];
                fn.blocks[s].instrs[idx].args[pit->second] = sit->second.back();
            }
        }
        for (size_t c : children[b]) dfs(c);
        for (const std::string& var : pushed[b]) stack[var].pop_back();
    };
    if (dom.reachable[0]) dfs(0);

    for (size_t b = 0; b < n; ++b) {
        auto& instrs = fn.blocks[b].instrs;
        std::vector<Instr> kept;
        kept.reserve(instrs.size());
        for (size_t i = 0; i < instrs.size(); ++i)
            if (!removed[b][i]) kept.push_back(std::move(instrs[i]));
        instrs.swap(kept);
    }
    return stats;
}

// Structural check on the SSA result: every phi has one operand per
// predecessor edge and no operand was left undefined.
inline bool validate_ssa(const lithon::ir::Function& fn, std::string* err = nullptr) {
    using namespace lithon::ir;
    const Cfg g = build_cfg(fn);

    // Every operand must name a value this function defines. Without this, a
    // rewrite that deletes a definition while something still refers to it
    // produces IR that parses, prints, and compiles -- it just reads whatever
    // was in that virtual register, so the bug surfaces as a wrong answer with
    // no diagnostic anywhere. Phi arity alone does not catch it.
    std::unordered_set<ValueId> defined;
    for (const auto& block : fn.blocks)
        for (const auto& in : block.instrs)
            if (in.result != kInvalidValue) defined.insert(in.result);
    for (const auto& block : fn.blocks)
        for (const auto& in : block.instrs)
            for (auto a : in.args)
                if (a != kInvalidValue && !defined.count(a)) {
                    if (err)
                        *err = "operand " + std::to_string(a) + " in " + block.label +
                               " names no definition in this function";
                    return false;
                }

    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const auto& in : fn.blocks[b].instrs) {
            if (in.op != Op::Phi) continue;
            if (in.args.size() != g.pred[b].size()) {
                if (err) *err = "phi arity mismatch at " + fn.blocks[b].label;
                return false;
            }
            for (auto a : in.args)
                if (a == kInvalidValue) {
                    if (err) *err = "phi has undefined incoming value at " + fn.blocks[b].label;
                    return false;
                }
        }
    return true;
}

// Phi copy resolution -- the emitter's final SSA stage. Every Phi is a
// parallel copy: at the top of block b it wants operand i to already be in
// %r, while the predecessors still hold those values in other registers.
// Sequential moves would clobber each other, so each Phi gets a *fresh*
// variable: it becomes a load of %__ssa_r at the top of b, and every
// incoming edge stores its operand into %__ssa_r just before the jump. Edge
// stores cannot collide, because a block's outgoing edges each have their own
// copy of the instruction list. The result is ordinary memory-form IR the
// existing backend already compiles, which is what makes the SSA pipeline
// usable end to end today; a real register allocator with an interference
// graph would turn the same phis into register moves directly.
// The variable a resolved Phi reads. resolve_phis() names it, and
// select_phi_registers() recognises it, so both go through here rather than
// repeating the literal -- the two drifting apart would mean phis silently
// falling back to memory with no diagnostic, which is exactly the kind of
// quiet regression this naming exists to prevent.
inline std::string phi_var(lithon::ir::ValueId r) {
    return "__ssa_" + std::to_string(r);
}

inline bool is_phi_var(const std::string& name) {
    return name.rfind("__ssa_", 0) == 0;
}

// 2.5 Phi copies as register moves.
//
// A resolved Phi is a variable written on each incoming edge and read once at
// the top of the join, which is a memory round-trip: store on the edge, load
// in the block. Giving each such variable a DEDICATED register from the
// callee-saved pool turns both halves into `mov reg, reg`.
//
// Dedicated is the operative word, and it is what makes this correct rather
// than merely plausible. A Phi wants a *parallel* copy: several destinations
// written from several sources at the same instant, and a sequential `mov`
// clobbers its sources if a destination shares a register with a source that
// has not been moved yet. With a normal allocator that happens -- the
// destination is born at the join, the source dies at the end of the
// predecessor, and a linear allocator is free to overlap those ranges. With a
// dedicated register nothing else can hold it for the whole function, so no
// destination can ever equal any source, and the sequential moves are safe by
// construction with no interference test anywhere.
//
// The cost is a bounded budget: one callee-saved register per Phi, competing
// with promoted variables. A function with more Phis than registers keeps the
// memory path for the remainder -- still correct, just not yet a move. That
// budget is what 2.7's CFG-based allocation removes.
inline std::vector<std::string> phi_copy_variables(const lithon::ir::Function& fn) {
    std::vector<std::string> vars;
    for (const auto& block : fn.blocks)
        for (const auto& in : block.instrs)
            if (in.op == lithon::ir::Op::Load && is_phi_var(in.name)) vars.push_back(in.name);
    return vars;
}

// The one piece of 2.8 that needs no allocator change at all: a merge whose
// operands are all the same value is not a merge.
//
// `c ? 1 : 1` is exactly this after constant folding, and so is an `x = x` that
// Mem2Reg had to give a Phi because two control-flow paths reached the variable.
// Every other merge needs a copy from each predecessor, but this one needs none:
// forwarding its uses to the operand deletes the Phi and the edge stores with
// it, so a promotion-pool register is never spent on it in the first place.
//
// Dominance is the guard, and it is not decoration. Forwarding `%r` to `%v` is
// only sound where `%v` is in scope at the join. When all operands agree, valid
// SSA already implies that -- `%v` is live on every incoming edge, so it
// dominates every predecessor, hence the join -- but the passes here rewrite
// IR the validator has not necessarily seen, and an undominated forward is a
// read of an undefined virtual register: it prints, it compiles, it returns
// whatever was lying around. Checking is cheaper than that class of bug.
inline size_t forward_trivial_phis(lithon::ir::Function& fn) {
    using namespace lithon::ir;
    if (fn.blocks.empty()) return 0;

    // The operand-agreement test needs nothing but the instruction lists, so do
    // it first and bail before paying for a CFG and a dominator tree. Copy
    // propagation already deletes most same-operand merges, so on typical input
    // this is the whole cost of the pass.
    std::unordered_map<ValueId, ValueId> forward;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const auto& in : fn.blocks[b].instrs) {
            if (in.op != Op::Phi || in.result == kInvalidValue) continue;
            ValueId only = kInvalidValue;
            for (auto a : in.args) {
                if (a == kInvalidValue) continue;
                if (only == kInvalidValue) only = a;
                if (only != a) { only = kInvalidValue; break; }
            }
            // No operand at all is not a trivial phi, it is an empty one.
            if (only == kInvalidValue || only == in.result) continue;
            forward[in.result] = only;
        }
    }
    if (forward.empty()) return 0;

    // Every candidate still has to be in scope at its join, and that is the one
    // question a scan cannot answer.
    const Cfg g = build_cfg(fn);
    const DomInfo dom = compute_dominators(g);
    std::unordered_map<ValueId, size_t> def_block;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (const auto& in : fn.blocks[b].instrs)
            if (in.result != kInvalidValue) def_block.emplace(in.result, b);

    // Decide every forward before rewriting any of it: extending the table while
    // rewriting would let a chain (phi A forwarded to phi B, B to C) leave a use
    // naming a Phi that no longer exists.
    for (auto it = forward.begin(); it != forward.end();) {
        const auto def = def_block.find(it->first);
        const auto src = def_block.find(it->second);
        const bool ok = def != def_block.end() && src != def_block.end() &&
                        dom.dominates(src->second, def->second);
        if (ok) ++it; else it = forward.erase(it);
    }
    if (forward.empty()) return 0;

    // Collapse any chain to its root, with a bound so a cycle built by a broken
    // pass terminates instead of spinning.
    for (auto& kv : forward) {
        ValueId cur = kv.second;
        for (size_t step = 0; step < forward.size() + 1; ++step) {
            const auto next = forward.find(cur);
            if (next == forward.end() || next->second == kv.first) break;
            cur = next->second;
        }
        kv.second = cur;
    }

    for (auto& block : fn.blocks) {
        std::vector<Instr> kept;
        kept.reserve(block.instrs.size());
        for (auto& in : block.instrs) {
            if (in.op == Op::Phi && forward.count(in.result)) continue;   // the merge goes
            for (auto& a : in.args) {
                const auto it = forward.find(a);
                if (it != forward.end()) a = it->second;
            }
            kept.push_back(std::move(in));
        }
        block.instrs = std::move(kept);
    }
    return forward.size();
}

inline void resolve_phis(lithon::ir::Function& fn) {
    using namespace lithon::ir;
    const Cfg g = build_cfg(fn);
    const size_t n = fn.blocks.size();

    std::vector<std::unordered_map<size_t, std::vector<std::pair<std::string, ValueId>>>>
        edge_stores(n);
    for (size_t b = 0; b < n; ++b) {
        for (auto& in : fn.blocks[b].instrs) {
            if (in.op != Op::Phi) continue;
            std::string var = phi_var(in.result);
            for (size_t i = 0; i < g.pred[b].size() && i < in.args.size(); ++i) {
                ValueId arg = in.args[i];
                if (arg == kInvalidValue) continue;
                edge_stores[g.pred[b][i]][b].push_back({var, arg});
            }
            in.op = Op::Load;
            in.args.clear();
            in.name = var;
        }
    }

    for (size_t p = 0; p < n; ++p) {
        for (auto& kv : edge_stores[p]) {
            auto& instrs = fn.blocks[p].instrs;
            size_t insert_at = instrs.size();
            if (!instrs.empty()) {
                Op last = instrs.back().op;
                if (last == Op::Jump || last == Op::Branch || last == Op::Return)
                    insert_at = instrs.size() - 1;
            }
            for (auto& sv : kv.second) {
                Instr st;
                st.op = Op::Store;
                st.result = kInvalidValue;
                st.args = {sv.second};
                st.name = sv.first;
                instrs.insert(instrs.begin() + insert_at, std::move(st));
                ++insert_at;
            }
        }
    }
}

// Old name, kept because it describes what the transform produced before the
// pipeline grew a front end: verification-only round trips call this directly.
inline void lower_ssa_to_memory(lithon::ir::Function& fn) { resolve_phis(fn); }

} // namespace lithon::jit
