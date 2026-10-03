#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "jit_abi.h"
#include "liveness.h"
#include "ssa.h"

// IR-level optimisation passes run before code generation. They only
// rewrite existing instructions (ConstInt / Jump) or delete them, so
// the M1 instruction set in ir.h is unchanged.
//
//  1. fold_constants       -- arithmetic/compare/logic on constants is
//                             evaluated at compile time; a Branch on a
//                             constant becomes a Jump. Constants are
//                             later emitted as immediates, never as
//                             instructions, so nothing invariant is ever
//                             re-materialised inside a loop (this is the
//                             loop-invariant-constant hoisting).
//  2. eliminate_dead_code  -- mark-and-sweep over def-use chains AND
//                             variables: side effects (Call, Return,
//                             Branch conditions) are the roots; any pure
//                             instruction, and any Store to a variable
//                             that is never observed, is deleted.

namespace lithon::jit {

struct OptimizeStats {
    int folded = 0;
    int branches_folded = 0;
    int dead_removed = 0;
    int copies_propagated = 0;
    int tail_calls = 0;
    int strength_reduced = 0;
    int accum_unrolled = 0;
    // Merges deleted outright because every operand was the same value. Each one
    // is a copy set that never had to exist, and a promotion-pool register that
    // never had to be spent on it.
    int phis_forwarded = 0;
    // Values accumulator_unroll created whose kind is double. Codegen reads
    // value kinds from an analysis of the ORIGINAL module, so this pass (the
    // only one that invents new values) reports its float results here and
    // compile_module folds them into the per-function kind vector before
    // allocation. Empty unless a float accumulator was actually split.
    std::vector<lithon::ir::ValueId> accum_float_values;
};

inline void fold_constants(lithon::ir::Function& fn, OptimizeStats& stats) {
    using namespace lithon::ir;
    std::unordered_map<ValueId, int64_t> consts;

    auto as_const = [&](ValueId id, int64_t& out) {
        auto it = consts.find(id);
        if (it == consts.end()) return false;
        out = it->second;
        return true;
    };
    auto rewrite = [&](Instr& in, int64_t v) {
        in.op = Op::ConstInt;
        in.int_imm = v;
        in.args.clear();
        consts[in.result] = v;
        ++stats.folded;
    };

    for (auto& block : fn.blocks) {
        for (auto& in : block.instrs) {
            int64_t a = 0, b = 0;
            switch (in.op) {
                case Op::ConstInt:
                case Op::ConstBool:
                    // Both store their 0/1-or-wider value in int_imm (see
                    // ir.h / text_parser.cpp); folding treats them alike.
                    consts[in.result] = in.int_imm;
                    break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b)) {
                        // Wrap-around arithmetic, matching the emitted machine code.
                        uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
                        uint64_t r = in.op == Op::Add ? ua + ub : in.op == Op::Sub ? ua - ub : ua * ub;
                        rewrite(in, static_cast<int64_t>(r));
                    }
                    break;
                case Op::Mod:
                    // Only integer constants reach here: fold_constants has no
                    // float lattice of its own, so a const_float operand makes
                    // as_const() false and the case is skipped.
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b)) {
                        // A zero divisor must NOT be folded. The interpreter
                        // traps, and the JIT's runtime check is what produces
                        // that trap; folding it to a constant here would turn a
                        // diagnosed runtime error into a silently wrong answer.
                        if (b == 0) break;
                        // a % -1 is 0 for every a, including INT64_MIN, whose
                        // quotient C++ leaves undefined and idiv traps on.
                        if (b == -1) { rewrite(in, 0); break; }
                        // C semantics (sign follows the dividend), matching
                        // the interpreter and the emitted idiv.
                        rewrite(in, static_cast<int64_t>(a % b));
                    }
                    break;
                case Op::Lt:
                case Op::Gt:
                case Op::Eq:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b)) {
                        bool r = in.op == Op::Lt ? a < b : in.op == Op::Gt ? a > b : a == b;
                        rewrite(in, r ? 1 : 0);
                    }
                    break;
                case Op::Not:
                    if (in.args.size() == 1 && as_const(in.args[0], a)) rewrite(in, a == 0 ? 1 : 0);
                    break;
                case Op::And:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b))
                        rewrite(in, a != 0 ? b : a);   // value semantics: lhs ? rhs : lhs
                    break;
                case Op::Or:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b))
                        rewrite(in, a != 0 ? a : b);   // value semantics: lhs ? lhs : rhs
                    break;
                case Op::BitAnd:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b))
                        rewrite(in, a & b);
                    break;
                case Op::BitOr:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b))
                        rewrite(in, a | b);
                    break;
                case Op::BitXor:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b))
                        rewrite(in, a ^ b);
                    break;
                case Op::Shl:
                    // Only fold a count that is actually valid. Folding an
                    // out-of-range one would replace a runtime trap with a
                    // constant, i.e. turn a loud failure into a silent wrong
                    // answer -- the exact trade this guard exists to prevent.
                    // The typechecker rejects the literal case, so an
                    // out-of-range count reaching here is only reachable from
                    // hand-written IR, and it must keep trapping.
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b)
                        && b >= 0 && b <= 63)
                        rewrite(in, static_cast<int64_t>(static_cast<uint64_t>(a)
                                                        << static_cast<uint64_t>(b)));
                    break;
                case Op::Shr:
                    if (in.args.size() == 2 && as_const(in.args[0], a) && as_const(in.args[1], b)
                        && b >= 0 && b <= 63)
                        rewrite(in, a >> static_cast<uint64_t>(b));
                    break;
                case Op::Branch:
                    if (in.args.size() == 1 && as_const(in.args[0], a)) {
                        auto targets = branch_targets(in);
                        if (targets.size() == 2) {
                            in.op = Op::Jump;
                            in.name = a != 0 ? targets[0] : targets[1];
                            in.args.clear();
                            ++stats.branches_folded;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
}

inline bool is_pure_op(lithon::ir::Op op) {
    using lithon::ir::Op;
    switch (op) {
        case Op::ConstInt: case Op::ConstBool: case Op::ConstFloat: case Op::Load:
        case Op::Add: case Op::Sub: case Op::Mul:
        case Op::Lt: case Op::Gt: case Op::Eq:
        case Op::And: case Op::Or: case Op::Not:
        // BitAnd/BitOr/BitXor only: genuinely total, so hoisting or
        // duplicating them is unobservable. Shl/Shr are deliberately NOT
        // listed even though they usually do not trap -- a count outside
        // 0..63 is a runtime error, and this predicate means "safe to MOVE"
        // (LICM, DCE). Hoisting a trapping op out of a loop, or deleting it
        // when its result is unused, changes whether it traps at all.
        //
        // That is a different question from "safe to DUPLICATE", which is
        // what the unroller asks. Shl/Shr are valid there; see
        // is_unrollable_op() in compile_function.h for why duplication
        // preserves trapping and movement does not.
        case Op::BitAnd: case Op::BitOr: case Op::BitXor:
            return true;
        default:
            return false;   // Call/Return/Branch/Jump have effects; Div/Float/Phi stay untouched
    }
}

// A value with no remaining uses is dead iff computing it has no effect. That
// is is_pure_op, plus Phi: a Phi has no side effect, but unlike a pure op it is
// position-dependent, so it must NOT be treated as pure by passes that MOVE or
// DUPLICATE instructions (is_pure_op stays the predicate there).
inline bool is_removable_if_unused(lithon::ir::Op op) {
    return is_pure_op(op) || op == lithon::ir::Op::Phi;
}

inline void eliminate_dead_code(lithon::ir::Function& fn, OptimizeStats& stats) {
    using namespace lithon::ir;

    std::unordered_map<ValueId, const Instr*> def;
    std::unordered_map<std::string, std::vector<const Instr*>> stores;
    for (const auto& block : fn.blocks) {
        for (const auto& in : block.instrs) {
            if (in.result != kInvalidValue) def[in.result] = &in;
            if (in.op == Op::Store) stores[in.name].push_back(&in);
        }
    }

    std::unordered_set<ValueId> live_temps;
    std::unordered_set<std::string> live_vars;
    std::vector<ValueId> work;

    auto mark_temp = [&](ValueId id) {
        if (live_temps.insert(id).second) work.push_back(id);
    };
    auto mark_var = [&](const std::string& name) {
        if (!live_vars.insert(name).second) return;
        for (const Instr* st : stores[name]) {
            if (!st->args.empty()) mark_temp(st->args[0]);
        }
    };

    for (const auto& block : fn.blocks) {
        for (const auto& in : block.instrs) {
            // Phi is not a side effect: in SSA it is removable when unused, so
            // it must not seed liveness the way Call/Return/Branch do.
            if (in.op != Op::Store && in.op != Op::Phi && !is_pure_op(in.op)) {
                for (auto arg : in.args) mark_temp(arg);
            }
        }
    }
    while (!work.empty()) {
        ValueId id = work.back();
        work.pop_back();
        auto it = def.find(id);
        if (it == def.end()) continue;
        const Instr* in = it->second;
        if (in->op == Op::Load) mark_var(in->name);
        else for (auto arg : in->args) mark_temp(arg);
    }

    for (auto& block : fn.blocks) {
        auto& v = block.instrs;
        size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Instr& in) {
            if (in.op == Op::Store) return live_vars.count(in.name) == 0;
            return is_removable_if_unused(in.op) && in.result != kInvalidValue
                   && live_temps.count(in.result) == 0;
        }), v.end());
        stats.dead_removed += static_cast<int>(before - v.size());
    }
}

// 2.4 Dead Store Elimination. On SSA form every promoted variable is a
// single-def value, so a definition with no users is dead by construction --
// no aliasing question, unlike the memory form. eliminate_dead_code is that
// pass; it removes unused pure defs AND unused Phis (see
// is_removable_if_unused), and still drops Stores to variables never read.
inline void dead_store_elimination(lithon::ir::Function& fn, OptimizeStats& stats) {
    eliminate_dead_code(fn, stats);
}

// 2.4 Copy propagation. In this IR a copy can only be a Phi: the operand
// flowing in on every (non-self) predecessor edge names the value the Phi
// defines. If all of them agree, the Phi is just an alias -- replace every use
// with that operand and delete it. A lone self-reference is ignored, which is
// what folds the `phi(x0, self)` a loop leaves for an unmodified variable.
// Iterated to a fixpoint because collapsing one alias can make its neighbours
// trivial too.
//
// The fixpoint alone is not sufficient, and the reason is worth stating because
// it is invisible from the outside. Two trivial Phis can alias *each other's
// targets*: block1 has `phi(%a)`, and a later join has `phi(%that, %that)`. Both
// are trivial, so both are collected in one round -- and both are about to be
// deleted. Applying that round's rewrites in block order rewrites the later
// Phi's users to %that only after %that has been deleted, leaving an operand
// that names nothing. That IR still compiles; it reads whatever happened to be
// in that virtual register.
//
// So a round resolves the whole alias graph BEFORE touching anything:
//   * every trivial Phi is replaced by the value at the END of its chain, not
//     its immediate target, so no rewrite can ever name a Phi this round is
//     deleting;
//   * a Phi whose chain dead-ends in a cycle -- two Phis aliasing each other
//     with nothing outside them -- is not safe to delete at all, so neither is
//     deleted and both stay. That case is degenerate, and keeping the Phis is
//     the conservative answer rather than a reason to fail.
inline void copy_propagate(lithon::ir::Function& fn, OptimizeStats& stats) {
    using namespace lithon::ir;
    for (;;) {
        std::unordered_map<ValueId, ValueId> alias;   // phi result -> immediate target
        std::unordered_map<const Instr*, ValueId> order;
        for (const auto& block : fn.blocks) {
            for (const auto& in : block.instrs) {
                if (in.op != Op::Phi || in.result == kInvalidValue) continue;
                ValueId target = kInvalidValue;
                bool multiple = false;
                for (ValueId a : in.args) {
                    if (a == kInvalidValue || a == in.result) continue;   // ignore self
                    if (target == kInvalidValue) target = a;
                    else if (a != target) { multiple = true; break; }
                }
                if (multiple || target == kInvalidValue) continue;
                alias[in.result] = target;
                order[&in] = target;
            }
        }
        if (alias.empty()) break;

        // A Phi may only be deleted if the value replacing it may also be
        // deleted, so grow the safe set to a fixpoint. A cycle never joins it.
        std::unordered_set<ValueId> folded;
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& kv : alias) {
                if (folded.count(kv.first)) continue;
                if (!alias.count(kv.second)) { folded.insert(kv.first); grew = true; }
            }
        }
        if (folded.empty()) break;

        // Walk each chain to its end. Terminates because nothing in `folded`
        // can reach itself through `alias` -- that would be a cycle, and cycles
        // are never folded.
        std::unordered_map<ValueId, ValueId> final_target;
        for (ValueId from : folded) {
            ValueId cur = alias[from];
            while (alias.count(cur)) cur = alias[cur];
            final_target[from] = cur;
        }
        for (const auto& kv : final_target) replace_all_uses(fn, kv.first, kv.second);

        std::unordered_set<const Instr*> doomed;
        for (const auto& kv : order)
            if (folded.count(kv.first->result)) doomed.insert(kv.first);
        for (auto& block : fn.blocks) {
            auto& v = block.instrs;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const Instr& in) { return doomed.count(&in) != 0; }),
                    v.end());
        }
        stats.copies_propagated += static_cast<int>(doomed.size());
    }
}

// 2.3 + 2.4 driver: promote to SSA, run copy propagation and dead store
// elimination to a fixpoint, then resolve the phis into edge copies the
// backend can compile. Running these passes on SSA is what makes them precise:
// a "use" is an id lookup, not an alias-tainted memory fact.
//
// validate_ssa() runs on the way out, so a placement bug fails loudly at the
// transform that caused it instead of compiling to something subtly wrong.
inline OptimizeStats optimize_ssa_function(lithon::ir::Function& fn) {
    OptimizeStats stats;
    mem2reg(fn);
    for (int iter = 0; iter < 64; ++iter) {
        const int before = stats.copies_propagated + stats.dead_removed + stats.phis_forwarded;
        copy_propagate(fn, stats);
        dead_store_elimination(fn, stats);
        // Inside the fixpoint, not after it: copy_propagate is what turns
        // `c ? 1 : 1` into a merge of one value, so a trivial phi can only
        // appear once propagation has run, and forwarding one can expose
        // another. Counting it in the convergence test is what stops the loop
        // declaring a fixpoint it has not reached.
        stats.phis_forwarded += static_cast<int>(forward_trivial_phis(fn));
        if (stats.copies_propagated + stats.dead_removed + stats.phis_forwarded == before) break;
    }
    std::string err;
    if (!validate_ssa(fn, &err))
        throw std::runtime_error("optimize_ssa_function: " + err);
    resolve_phis(fn);
    return stats;
}


// The whole SSA pipeline behind one call, so compile_module stays a readable
// list of phases and there is exactly one place that decides their order.
inline OptimizeStats run_ssa_pipeline(lithon::ir::Function& fn) {
    canonicalize_loops(fn);
    // 2.6, after canonicalization (which needs the preheader and single-latch
    // shape to reason about) and before Phi placement, so the new exit edges are
    // the edges phis are placed on.
    synthesize_loop_exits(fn);
    return optimize_ssa_function(fn);
}

// Self tail calls become loops. The frontend emits `return f(...)` as
//     %r = call f, a0, a1 ; return %r
// which is rewritten to
//     store p0, a0 ; store p1, a1 ; jump <entry block>
// Argument values are %N temporaries already computed before the call, so
// storing them one after another cannot read a parameter that was just
// overwritten. The prologue is emitted before the entry block, so the jump
// re-enters at the right place: the parameters live in their variables (a
// register when promoted), exactly as after a real call. Stack use becomes
// O(1) and the existing loop machinery (promotion weights, rotation) applies.
inline void convert_self_tail_calls(lithon::ir::Function& fn, OptimizeStats& stats) {
    using namespace lithon::ir;
    if (fn.blocks.empty()) return;
    const std::string entry = fn.blocks.front().label;
    for (auto& block : fn.blocks) {
        auto& v = block.instrs;
        for (size_t k = 0; k + 1 < v.size(); ++k) {
            const Instr& call = v[k];
            const Instr& ret = v[k + 1];
            if (call.op != Op::Call || call.name != fn.name) continue;
            if (call.result == kInvalidValue || call.args.size() != fn.params.size()) continue;
            if (ret.op != Op::Return || ret.args.size() != 1 || ret.args[0] != call.result) continue;

            std::vector<Instr> rewritten(v.begin(), v.begin() + k);
            for (size_t j = 0; j < fn.params.size(); ++j) {
                Instr st;
                st.op = Op::Store;
                st.result = kInvalidValue;
                st.name = fn.params[j];
                st.args.push_back(call.args[j]);
                rewritten.push_back(st);
            }
            Instr jmp;
            jmp.op = Op::Jump;
            jmp.result = kInvalidValue;
            jmp.name = entry;
            rewritten.push_back(jmp);
            v = std::move(rewritten);   // everything after the old return was dead
            ++stats.tail_calls;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Strength reduction: `invariant * induction_var`, re-multiplied on every
// iteration, becomes an accumulator that is advanced by the invariant.
//
//   before (i fixed, j = 0,1,2,...):   after:
//     %a  = load i                      %t  = load acc        ->  i*0
//     %b  = load j                      %nx = add %t, %a
//     %m  = mul %a, %b   -> i*j          store acc, %nx       ->  acc = i
//     ... %m used ...                   ... %t used ...      ->  i*k, then
//                                        %nx = add %t, %a
//                                        store acc, %nx
//
// %t is the multiply's replacement, so every use of %m becomes a use of
// %t; `acc` is a fresh variable nothing else in the program can name, so it
// is unobservable outside the loop it was invented for.
//
// Two placement details are load-bearing, not cosmetic:
//
//  * The advance is appended at the *end* of the body, after every use of
//    the old multiply. %t then has its last use at the appended Add, and
//    the only Store to `acc` sits at that position -- outside the window
//    plan_function() scans when deciding whether a Load can be an Alias of
//    the variable's register. Left in the middle of the body the same chain
//    is demoted to a real temporary and costs two extra `mov`s.
//  * The advance is an Add of the *already-loaded* %t and the invariant,
//    so it fuses into `add acc_reg, inv_reg` with no reload of `acc`.
//
// Restricted to the canonical counted-loop shape, because anything looser
// would need a phi to carry `acc` across the back edge:
//   * the loop is a header block H ending in a Branch and a single
//     straight-line body B ending in `jump H`, so B runs exactly once per
//     iteration on every path;
//   * H has exactly one predecessor P ending in `jump H`, so the single
//     Store that zeroes `acc` runs once per entry;
//   * the induction variable is zeroed in P, stored exactly once in B by
//     `add (load v), 1`, and is not stored anywhere else in the loop --
//     so its value at the multiply in iteration k is exactly k;
//   * the other operand is either a constant or a Load of a variable never
//     Stored in the loop;
//   * every use of the multiply's result is later in B (a use outside B
//     would read `acc` at the wrong time, or not at all if the body never
//     ran);
//   * the function stores few enough variables that `acc` still wins a
//     promotion register. Un-promoted, the chain would replace a 2-instruction
//     `mov`+`imul` with a load, an add and a store -- strictly worse.
inline void strength_reduce_multiplies(lithon::ir::Function& fn, OptimizeStats& stats) {
    using namespace lithon::ir;
    if (fn.blocks.size() < 2) return;

    // def/uses/store index over the *original* text. The pass rewrites at most
    // one multiply per loop and rebuilds the index whenever it does, so the
    // pointers below never outlive the pass.
    std::unordered_map<ValueId, const Instr*> def;
    std::unordered_map<ValueId, std::vector<std::pair<size_t, size_t>>> uses;
    std::unordered_map<std::string, std::vector<std::pair<size_t, size_t>>> store_sites;
    std::unordered_set<std::string> stored_vars;
    ValueId next_id = 0;

    auto reindex = [&]() {
        def.clear();
        uses.clear();
        store_sites.clear();
        stored_vars.clear();
        next_id = 0;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            const auto& v = fn.blocks[b].instrs;
            for (size_t p = 0; p < v.size(); ++p) {
                const Instr& in = v[p];
                if (in.result != kInvalidValue) {
                    def[in.result] = &in;
                    next_id = std::max(next_id, in.result + 1);
                }
                for (ValueId a : in.args) uses[a].push_back({b, p});
                if (in.op == Op::Store) {
                    store_sites[in.name].push_back({b, p});
                    stored_vars.insert(in.name);
                }
            }
        }
    };
    reindex();
    // `acc` has to reach kPromotionPool or the chain is a net loss; leave two
    // registers of slack so one other variable going hot cannot evict it.
    if (stored_vars.size() + 2 > abi::kPromotionPool.size()) return;

    auto label_index = [&]() {
        std::unordered_map<std::string, size_t> m;
        for (size_t b = 0; b < fn.blocks.size(); ++b) m[fn.blocks[b].label] = b;
        return m;
    };
    auto lab = label_index();

    auto const_value = [&](ValueId id, int64_t& out) {
        auto it = def.find(id);
        if (it == def.end()) return false;
        if (it->second->op != Op::ConstInt && it->second->op != Op::ConstBool) return false;
        out = it->second->int_imm;
        return true;
    };
    // `store v, x` zeroes v when x is a constant 0.
    auto is_zero_store = [&](const Instr& st) {
        int64_t v = 0;
        return st.op == Op::Store && st.args.size() == 1 && const_value(st.args[0], v) && v == 0;
    };
    // `store v, x` is the unit induction update when x = add(load v, 1).
    auto is_unit_step = [&](const Instr& st, const std::string& var) {
        if (st.op != Op::Store || st.args.size() != 1) return false;
        auto add_it = def.find(st.args[0]);
        if (add_it == def.end()) return false;
        const Instr* add = add_it->second;
        if (add->op != Op::Add || add->args.size() != 2) return false;
        bool has_load = false;
        for (ValueId a : add->args) {
            auto d = def.find(a);
            if (d == def.end()) return false;
            if (d->second->op == Op::Load && d->second->name == var) {
                if (has_load) return false;
                has_load = true;
            } else {
                int64_t v = 0;
                if (!const_value(a, v) || v != 1) return false;
            }
        }
        return has_load;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const Loop& loop : compute_loop_info(fn).loops) {
            // Two-block loops only, and the body must be the block physically
            // after the header: both passes below rewrite them by position.
            if (loop.blocks.size() != 2 || loop.latches.size() != 1) continue;
            if (loop.header == 0) continue;                         // entry block has no predecessor
            const size_t h = loop.header, body = loop.latches.front();
            if (body >= fn.blocks.size() || h + 1 != body) continue;
            auto& hv = fn.blocks[h].instrs;
            auto& bv = fn.blocks[body].instrs;
            if (hv.size() < 2 || bv.size() < 3) continue;
            if (hv.back().op != Op::Branch) continue;                // header must branch
            if (bv.back().op != Op::Jump || bv.back().name != fn.blocks[h].label) continue;

            // H must be entered only from P, and P must fall straight into H.
            // The latch targets H too, so it does not count as a predecessor.
            size_t pred = fn.blocks.size();
            size_t preds = 0;
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                if (b == body) continue;
                for (const std::string& t : branch_targets(fn.blocks[b].instrs.back())) {
                    if (lab[t] == h) { pred = b; ++preds; }
                }
            }
            if (preds != 1 || pred >= fn.blocks.size()) continue;
            // B must be entered only from H, so it runs at most once per
            // iteration and only after the zero store in P.
            size_t body_preds = 0;
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                if (b == body) continue;
                for (const std::string& t : branch_targets(fn.blocks[b].instrs.back()))
                    if (t == fn.blocks[body].label) ++body_preds;
            }
            if (body_preds != 1) continue;
            auto& pv = fn.blocks[pred].instrs;
            if (pv.empty() || pv.back().op != Op::Jump || pv.back().name != fn.blocks[h].label) continue;

            // Pick the induction variable: it is zeroed on the edge into the
            // loop, and inside the loop it is written exactly once, in the
            // body, by a unit step. Counting stores per variable globally is
            // too strict -- the frontend emits *two* initialising stores for a
            // `for` loop (the user's `k: int[64] = 0` and the loop's own `k=0`),
            // so `k` has three stores while still being a textbook counted IV.
            // What matters is only what happens between the last zeroing store
            // and the multiply.
            for (const auto& site : store_sites) {
                const std::string& var = site.first;
                const auto& locs = site.second;
                const std::pair<size_t, size_t>* step = nullptr;
                size_t in_loop = 0;
                for (const auto& s : locs) {
                    if (s.first != h && s.first != body) continue;
                    ++in_loop;
                    if (s.first == body && !step) step = &s;
                }
                if (in_loop != 1 || !step) continue;          // one write, in the body
                if (!is_unit_step(bv[step->second], var)) continue;
                // zeroed in P, and that zeroing must be P's last write to it
                bool zeroed = false;
                for (const auto& s : locs) {
                    if (s.first != pred) continue;
                    zeroed = is_zero_store(pv[s.second]);
                }
                if (!zeroed) continue;
                ValueId zero_arg = kInvalidValue;
                for (const auto& s : locs)
                    if (s.first == pred && is_zero_store(pv[s.second])) zero_arg = pv[s.second].args[0];

                // Find a multiply in the body of `var * invariant`.
                for (size_t mp = 0; mp + 1 < bv.size(); ++mp) {
                    const Instr& mul = bv[mp];
                    if (mul.op != Op::Mul || mul.args.size() != 2) continue;
                    ValueId iv_load = kInvalidValue, inv_arg = kInvalidValue;
                    bool inv_is_const = false;
                    for (ValueId a : mul.args) {
                        auto d = def.find(a);
                        if (d == def.end()) { iv_load = kInvalidValue; break; }
                        const Instr* src = d->second;
                        const bool is_const = src->op == Op::ConstInt || src->op == Op::ConstBool;
                        if (src->op != Op::Load && !is_const) { iv_load = kInvalidValue; break; }
                        if (src->op == Op::Load && src->name == var) {
                            if (iv_load != kInvalidValue) { iv_load = kInvalidValue; break; }
                            iv_load = a;
                        } else {
                            if (inv_arg != kInvalidValue) { iv_load = kInvalidValue; break; }
                            inv_arg = a;
                            inv_is_const = is_const;
                        }
                    }
                    if (iv_load == kInvalidValue || inv_arg == kInvalidValue) continue;
                    // a variable invariant must be untouched inside the loop; a
                    // constant one is invariant by construction.
                    if (!inv_is_const) {
                        const std::string& inv_var = def.find(inv_arg)->second->name;
                        bool inv_stored = false;
                        for (const auto& s : store_sites[inv_var])
                            if (s.first == h || s.first == body) inv_stored = true;
                        if (inv_stored) continue;
                    }
                    // every use of the product must be later in the body
                    auto u = uses.find(mul.result);
                    if (u == uses.end() || u->second.empty()) continue;
                    bool all_local = true;
                    for (const auto& s : u->second) if (s.first != body || s.second <= mp) all_local = false;
                    if (!all_local) continue;

                    // ---- rewrite -------------------------------------------------
                    const std::string acc = "__lsr" + std::to_string(next_id);
                    ValueId t = next_id++, nx = next_id++;
                    for (const auto& s : u->second) {
                        Instr& user = fn.blocks[s.first].instrs[s.second];
                        for (ValueId& a : user.args) if (a == mul.result) a = t;
                    }
                    Instr ld;                       // %t = load acc   (stands in for the mul)
                    ld.op = Op::Load;
                    ld.result = t;
                    ld.name = acc;
                    bv[mp] = ld;
                    Instr av;                      // %nx = add %t, inv
                    av.op = Op::Add;
                    av.result = nx;
                    av.args = {t, inv_arg};
                    Instr st;                      // store acc, %nx
                    st.op = Op::Store;
                    st.result = kInvalidValue;
                    st.name = acc;
                    st.args = {nx};
                    bv.insert(bv.end() - 1, av);
                    bv.insert(bv.end() - 1, st);
                    Instr zero;                   // store acc, 0   (in P, reusing %c)
                    zero.op = Op::Store;
                    zero.result = kInvalidValue;
                    zero.name = acc;
                    zero.args = {zero_arg};
                    pv.insert(pv.end() - 1, zero);
                    ++stats.strength_reduced;
                    changed = true;
                    // The inserts above may have reallocated both blocks'
                    // instrs vectors, which every pointer in def/uses/store_sites
                    // points into. Rebuild before the next iteration looks at
                    // them again.
                    reindex();
                    break;
                }
                if (changed) break;
            }
            if (changed) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Accumulator unrolling ("unroll and jam"). A counted reduction such as
//
//     total = 0
//     for i in range(n):
//         total += f(i)
//
// has one loop-carried dependency: every iteration's `add` reads the
// accumulator the previous iteration wrote. On a CPU where the add/multiply
// latency is more than one cycle the loop cannot retire faster than that
// chain, no matter how much other work is in flight. Unrolling the body
// alone does not help -- the U copies all feed the SAME variable, so the
// chain is merely lengthened.
//
// This pass splits the accumulator into `factor` partials and gives each
// unrolled copy its own, so the copies are independent. After the loop the
// partials are summed once into the original variable. Integer addition is
// associative and commutative modulo 2^64, so the result is bit-identical.
//
// Shape (the canonical frontend counted loop, the same one
// strength_reduce_multiplies recognises):
//
//     P:  ... store iv, 0 ; jump H          (single external predecessor)
//     H:  ... = lt (load iv), (bound) ; branch -> B, X
//     B:  ... store acc, add(load acc, x) ... store iv, add(load iv, 1) ; jump H
//     X:  ...                               (single exit, only H -> X)
//
// becomes
//
//     P:  zero __acc0..__accN-1 ; limit = bound - bound%N ; jump H_main
//     H_main:  branch (lt (load iv), limit) -> B_main, H
//     B_main:  N copies of B, copy k reducing into __accK ; jump H_main
//     H:  (unchanged) ; B: (unchanged, the scalar remainder loop) ; jump H
//     X:  acc = acc + __acc0 + ... + __accN-1 ; <original X>
//
// The chunk test is `iv < limit` with `limit = (bound/N)*N`, computed once in
// P. That is exactly "at least N elements remain and another whole chunk
// starts here", so no element is skipped or processed twice, and the test
// stays as cheap as the original `iv < bound`. `limit` is `bound - bound%N`
// rather than `(bound/N)*N` because integer `Div`'s codegen path falls through
// to add/sub/imul (see compile_function.h); `Mod` is the path the frontend
// already exercises. For a negative bound `limit <= 0`, so `iv = 0 < limit` is
// false and the jammed loop is skipped, matching the never-entered original.
//
// Default OFF because its real payoff is a long-latency FLOAT accumulator
// (~1.6x on a float reduction): an integer add chain already retires at ~1
// element/cycle, so splitting `sum 20M` is roughly neutral. It stays opt-in
// with --accum-unroll so the default pipeline keeps the bit-exact interpreter
// parity the gate relies on (a split float accumulator reassociates the adds).
// Anything that does not match this shape -- a non-straight-line body, a second
// accumulator use, an early exit, a trapping op that is not duplication-safe --
// is rejected outright.
//
// FLOAT accumulators are split too, but only when the caller passes the
// module's value kinds via `is_float`. Unlike the integer case this is NOT
// semantics-preserving: FP addition is not associative, so splitting the chain
// rounds differently whenever the partial sums are inexact. The caller opts
// into that explicitly (--accum-unroll) and this pass records every value it
// creates that is a double in stats.accum_float_values so codegen places them
// in the XMM world; without `is_float` a float reduction is left untouched.
inline void accumulator_unroll(lithon::ir::Function& fn, OptimizeStats& stats,
                               int factor, const std::vector<bool>* is_float = nullptr) {
    using namespace lithon::ir;
    if (factor < 2) return;
    if (fn.blocks.size() < 2) return;

    auto duplicable = [](Op op) {
        switch (op) {
            case Op::ConstInt: case Op::ConstBool: case Op::ConstFloat:
            case Op::Load: case Op::Store:
            case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Lt: case Op::Gt: case Op::Eq:
            case Op::And: case Op::Or: case Op::Not:
            case Op::BitAnd: case Op::BitOr: case Op::BitXor:
            case Op::Shl: case Op::Shr:
                return true;
            default:
                return false;   // Div/Mod/Call/control flow: not duplicated here
        }
    };

    std::unordered_set<std::string> skip_headers;
    ValueId next_id = 0;
    for (const auto& b : fn.blocks)
        for (const auto& in : b.instrs)
            if (in.result != kInvalidValue) next_id = std::max(next_id, in.result + 1);

    for (int round = 0; round < 64; ++round) {
        std::unordered_map<ValueId, const Instr*> def;
        std::unordered_map<ValueId, size_t> def_block;
        for (size_t b = 0; b < fn.blocks.size(); ++b)
            for (const auto& in : fn.blocks[b].instrs)
                if (in.result != kInvalidValue) { def[in.result] = &in; def_block[in.result] = b; }
        std::unordered_map<std::string, size_t> lab;
        for (size_t b = 0; b < fn.blocks.size(); ++b) lab[fn.blocks[b].label] = b;

        auto const_value = [&](ValueId id, int64_t& out) {
            auto it = def.find(id);
            if (it == def.end()) return false;
            if (it->second->op != Op::ConstInt && it->second->op != Op::ConstBool) return false;
            out = it->second->int_imm; return true;
        };
        auto load_var = [&](ValueId id, std::string& var) {
            auto it = def.find(id);
            if (it == def.end() || it->second->op != Op::Load) return false;
            var = it->second->name; return true;
        };

        bool changed = false;
        for (const Loop& loop : compute_loop_info(fn).loops) {
            // Two-block loops only, and the body must be the block physically
            // after the header: the body is rewritten by position.
            if (loop.blocks.size() != 2 || loop.latches.size() != 1) continue;
            const size_t h = loop.header, body = loop.latches.front();
            if (h == 0 || body >= fn.blocks.size() || h + 1 != body) continue;
            if (skip_headers.count(fn.blocks[h].label)) continue;
            if (fn.blocks[h].instrs.empty() || fn.blocks[body].instrs.size() < 3) continue;
            if (fn.blocks[h].instrs.back().op != Op::Branch) continue;
            const Instr& back = fn.blocks[body].instrs.back();
            if (back.op != Op::Jump || trim_label(back.name) != fn.blocks[h].label) continue;

            // Body must be straight-line, duplication-safe, and short enough
            // that the codegen unroller will leave the jammed copy alone.
            const size_t body_len = fn.blocks[body].instrs.size();
            if (body_len > 64) continue;
            bool straight = true;
            for (size_t i = 0; i + 1 < body_len; ++i)
                if (!duplicable(fn.blocks[body].instrs[i].op)) { straight = false; break; }
            if (!straight) continue;

            // Header condition must be `lt(load iv, bound)` / `gt(bound, load iv)`.
            const Instr& hterm = fn.blocks[h].instrs.back();
            if (hterm.args.size() != 1) continue;
            auto cond_it = def.find(hterm.args[0]);
            if (cond_it == def.end()) continue;
            const Instr* cond = cond_it->second;
            if ((cond->op != Op::Lt && cond->op != Op::Gt) || cond->args.size() != 2) continue;
            // Is `var` written exactly once in the body by a unit step?
            auto is_unit_step_var = [&](const std::string& var) {
                size_t n = 0; bool good = false;
                for (const auto& in : fn.blocks[body].instrs) {
                    if (in.op != Op::Store || in.name != var) continue;
                    ++n;
                    good = false;
                    if (in.args.size() == 1) {
                        auto ai = def.find(in.args[0]);
                        if (ai != def.end() && ai->second->op == Op::Add && ai->second->args.size() == 2) {
                            bool sl = false, so = false, valid = true;
                            for (ValueId a : ai->second->args) {
                                std::string vn;
                                if (load_var(a, vn) && vn == var) {
                                    if (sl) { valid = false; break; }
                                    sl = true;
                                } else {
                                    int64_t v;
                                    if (const_value(a, v) && v == 1) so = true;
                                    else { valid = false; break; }
                                }
                            }
                            good = valid && sl && so;
                        }
                    }
                }
                return n == 1 && good;
            };

            // The induction side of the test is the Load that is unit-stepped
            // in the body; the other side is the bound. When both sides are
            // loads (`range(n)`) the step decides which is which.
            std::string iv;
            ValueId bound = kInvalidValue;
            {
                std::string na, nb;
                bool la = load_var(cond->args[0], na), lb = load_var(cond->args[1], nb);
                if (la && !lb) { iv = na; bound = cond->args[1]; }
                else if (lb && !la) { iv = nb; bound = cond->args[0]; }
                else if (la && lb) {
                    bool au = is_unit_step_var(na), bu = is_unit_step_var(nb);
                    if (au && !bu) { iv = na; bound = cond->args[1]; }
                    else if (bu && !au) { iv = nb; bound = cond->args[0]; }
                    else continue;
                } else continue;
            }

            // Single external predecessor P of H, falling straight into H.
            size_t pred = fn.blocks.size(), preds = 0;
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                if (b == body) continue;
                for (const std::string& t : branch_targets(fn.blocks[b].instrs.back()))
                    if (lab.count(t) && lab[t] == h) { pred = b; ++preds; }
            }
            if (preds != 1 || pred >= fn.blocks.size()) continue;
            const auto& pv = fn.blocks[pred].instrs;
            if (pv.empty() || pv.back().op != Op::Jump || trim_label(pv.back().name) != fn.blocks[h].label) continue;

            // `bound` is either a constant defined in P, or a Load of a
            // variable P (or earlier) set and the loop never writes.
            bool bound_is_const = false;
            std::string bound_var;
            int64_t limit_const = 0;   // (bound / factor) * factor, for a constant bound
            {
                auto bit = def.find(bound);
                if (bit == def.end()) continue;
                int64_t cv;
                if (const_value(bound, cv)) {
                    auto db = def_block.find(bound);
                    if (db == def_block.end() || db->second != pred) continue;
                    bound_is_const = true;
                    limit_const = (cv / factor) * factor;
                } else if (bit->second->op == Op::Load) {
                    bound_var = bit->second->name;
                    for (const auto& in : fn.blocks[h].instrs)
                        if (in.op == Op::Store && in.name == bound_var) { bound_var.clear(); break; }
                    for (const auto& in : fn.blocks[body].instrs)
                        if (in.op == Op::Store && in.name == bound_var) { bound_var.clear(); break; }
                    if (bound_var.empty()) continue;
                } else continue;
            }

            // `iv` is zeroed in P and unit-stepped exactly once, in the body.
            bool zeroed = false;
            for (const auto& in : pv)
                if (in.op == Op::Store && in.name == iv && !in.args.empty()) {
                    int64_t v; zeroed = const_value(in.args[0], v) && v == 0;
                }
            if (!zeroed) continue;
            bool iv_stored_in_h = false;
            for (const auto& in : fn.blocks[h].instrs)
                if (in.op == Op::Store && in.name == iv) iv_stored_in_h = true;
            if (iv_stored_in_h) continue;
            size_t iv_stores_body = 0;
            bool iv_unit = false;
            for (const auto& in : fn.blocks[body].instrs) {
                if (in.op != Op::Store || in.name != iv) continue;
                ++iv_stores_body;
                bool good = false;
                if (in.args.size() == 1) {
                    auto ai = def.find(in.args[0]);
                    if (ai != def.end() && ai->second->op == Op::Add && ai->second->args.size() == 2) {
                        bool seen_load = false, seen_one = false, valid = true;
                        for (ValueId a : ai->second->args) {
                            std::string vn;
                            if (load_var(a, vn) && vn == iv) {
                                if (seen_load) { valid = false; break; }
                                seen_load = true;
                            } else {
                                int64_t v;
                                if (const_value(a, v) && v == 1) seen_one = true;
                                else { valid = false; break; }
                            }
                        }
                        good = valid && seen_load && seen_one;
                    }
                }
                iv_unit = good;
            }
            if (iv_stores_body != 1 || !iv_unit) continue;

            // Exactly one reduction `store acc, add(load acc, x)` in the body.
            std::string acc;
            int red_matches = 0;
            for (const auto& in : fn.blocks[body].instrs) {
                if (in.op != Op::Store || in.args.size() != 1) continue;
                if (in.name == iv) continue;   // the induction step is not the reduction
                auto ai = def.find(in.args[0]);
                if (ai == def.end() || ai->second->op != Op::Add || ai->second->args.size() != 2) continue;
                for (int which = 0; which < 2; ++which) {
                    ValueId l = ai->second->args[which], o = ai->second->args[1 - which];
                    std::string lv, ov;
                    if (!load_var(l, lv) || lv != in.name) continue;
                    if (load_var(o, ov) && ov == in.name) continue;
                    acc = in.name; ++red_matches;
                }
            }
            if (red_matches != 1 || acc.empty() || acc == iv) continue;
            if (!bound_var.empty() && (acc == bound_var)) continue;
            size_t acc_stores = 0, acc_loads = 0;
            for (const auto& in : fn.blocks[body].instrs) {
                if (in.op == Op::Store && in.name == acc) ++acc_stores;
                if (in.op == Op::Load && in.name == acc) ++acc_loads;
            }
            if (acc_stores != 1 || acc_loads != 1) continue;
            bool h_touches_acc = false;
            for (const auto& in : fn.blocks[h].instrs)
                if ((in.op == Op::Load || in.op == Op::Store) && in.name == acc) h_touches_acc = true;
            if (h_touches_acc) continue;

            // A float accumulator (`total = 0.0`) is split the same way, but
            // only when the caller supplied the module's value kinds. It needs
            // them to (a) tell which cloned body results are doubles so codegen
            // can be told, and (b) decline when it cannot -- mis-typing a new
            // float value as int corrupts the XMM/GP split. See the pass
            // comment: this is the one place the split is not bit-identical.
            bool float_acc = false;
            for (const auto& in : pv) {
                if (in.op != Op::Store || in.name != acc || in.args.size() != 1) continue;
                auto ai = def.find(in.args[0]);
                if (ai != def.end() && ai->second->op == Op::ConstFloat) float_acc = true;
            }
            if (float_acc && is_float == nullptr) continue;
            auto result_is_float = [&](ValueId id) {
                return is_float != nullptr && id < is_float->size() && (*is_float)[id];
            };
            auto record_float = [&](ValueId id) {
                if (float_acc) stats.accum_float_values.push_back(id);
            };

            // A body that computes a double must be either the float
            // accumulator reduction being split, or left alone. Duplicating a
            // double value whose kind codegen cannot see would type it `int`
            // and corrupt the register-class split, so decline mixed bodies.
            {
                bool unsupported = false;
                if (!float_acc) {
                    for (const auto& in : fn.blocks[body].instrs) {
                        if (in.result != kInvalidValue && result_is_float(in.result)) {
                            unsupported = true; break;
                        }
                        if (is_float == nullptr && in.op == Op::ConstFloat) {
                            unsupported = true; break;
                        }
                    }
                }
                if (unsupported) continue;
            }

            // Exit X: header's other target, reachable only from H.
            auto ht = branch_targets(hterm);
            if (ht.size() != 2) continue;
            const std::string blabel = fn.blocks[body].label;
            std::string exit_label;
            if (ht[0] == blabel) exit_label = ht[1];
            else if (ht[1] == blabel) exit_label = ht[0];
            else continue;
            if (!lab.count(exit_label)) continue;
            const size_t x = lab[exit_label];
            if (x == h || x == body) continue;
            size_t x_preds = 0;
            for (size_t b = 0; b < fn.blocks.size(); ++b)
                for (const std::string& t : branch_targets(fn.blocks[b].instrs.back()))
                    if (lab.count(t) && lab[t] == x) ++x_preds;
            if (x_preds != 1) continue;

            // No body operand may be a value defined in H or X; those blocks
            // do not dominate the jammed copy.
            bool args_ok = true;
            for (const auto& in : fn.blocks[body].instrs) {
                for (ValueId a : in.args) {
                    auto db = def_block.find(a);
                    if (db == def_block.end() || db->second == body || db->second == pred) continue;
                    args_ok = false; break;
                }
                if (!args_ok) break;
            }
            if (!args_ok) continue;

            // ---- rewrite -------------------------------------------------
            const size_t serial = next_id;
            std::vector<std::string> partial(factor);
            for (int k = 0; k < factor; ++k)
                partial[k] = "__accum" + std::to_string(serial) + "_" + std::to_string(k);
            const std::string hmain = "__accum_h" + std::to_string(serial);
            const std::string bmain = "__accum_b" + std::to_string(serial);
            const std::string hlabel = fn.blocks[h].label;

            // B_main: `factor` verbatim copies of the body, copy k reducing
            // into partial[k]; every copy increments iv, so no substitution.
            BasicBlock bmb;
            bmb.label = bmain;
            for (int k = 0; k < factor; ++k) {
                std::unordered_map<ValueId, ValueId> remap;
                for (const Instr& in : fn.blocks[body].instrs) {
                    if (in.op == Op::Jump) break;
                    Instr c = in;
                    if (c.result != kInvalidValue) {
                        ValueId nr = next_id++;
                        remap[in.result] = nr;
                        c.result = nr;
                        if (result_is_float(in.result)) record_float(nr);
                    }
                    for (ValueId& a : c.args) {
                        auto it = remap.find(a);
                        if (it != remap.end()) a = it->second;
                    }
                    if (c.op == Op::Load && c.name == acc) c.name = partial[k];
                    else if (c.op == Op::Store && c.name == acc) c.name = partial[k];
                    bmb.instrs.push_back(c);
                }
            }
            // ONE terminator after all `factor` copies: the jammed block
            // executes every copy, then re-tests its chunk limit. A jump per
            // copy would make copies 1..N-1 unreachable dead code and leave the
            // accumulator chain unsplit (only partial 0 would ever update).
            {
                Instr j; j.op = Op::Jump; j.result = kInvalidValue; j.name = hmain;
                bmb.instrs.push_back(j);
            }

            // P: zero the partials and compute the chunk limit once. Each jam
            // consumes `factor` iterations, so the jammed loop runs for
            // `limit = (bound / factor) * factor` iterations and the untouched
            // tail handles the rest. Hoisting the limit keeps H_main's test as
            // cheap as the original `iv < bound` -- a per-chunk subtraction
            // would cost more than the split could ever save.
            ValueId limit = kInvalidValue;
            {
                auto& p = fn.blocks[pred].instrs;
                Instr z;
                if (float_acc) { z.op = Op::ConstFloat; z.float_imm = 0.0; z.type_kind = "float"; }
                else { z.op = Op::ConstInt; z.int_imm = 0; z.type_kind = "int"; }
                z.type_width = 64; z.result = next_id++;
                record_float(z.result);
                p.insert(p.end() - 1, z);
                for (int k = 0; k < factor; ++k) {
                    Instr st; st.op = Op::Store; st.result = kInvalidValue;
                    st.name = partial[k]; st.args = {z.result};
                    p.insert(p.end() - 1, st);
                }
                if (bound_is_const) {
                    Instr lc; lc.op = Op::ConstInt; lc.result = next_id++; lc.int_imm = limit_const;
                    lc.type_kind = "int"; lc.type_width = 64;
                    p.insert(p.end() - 1, lc); limit = lc.result;
                } else {
                    // limit = bound - bound % factor. Integer `div` has no
                    // codegen path worth trusting here, but `mod` is the same
                    // one the frontend already exercises, and subtracting the
                    // remainder avoids it entirely.
                    Instr bl; bl.op = Op::Load; bl.result = next_id++; bl.name = bound_var;
                    p.insert(p.end() - 1, bl);
                    Instr nc; nc.op = Op::ConstInt; nc.result = next_id++; nc.int_imm = factor;
                    nc.type_kind = "int"; nc.type_width = 64;
                    p.insert(p.end() - 1, nc);
                    Instr md; md.op = Op::Mod; md.result = next_id++; md.args = {bl.result, nc.result};
                    p.insert(p.end() - 1, md);
                    Instr sb; sb.op = Op::Sub; sb.result = next_id++; sb.args = {bl.result, md.result};
                    p.insert(p.end() - 1, sb); limit = sb.result;
                }
                p.back().name = hmain;
            }

            // H_main: branch -> B_main while iv < limit, else the untouched H.
            BasicBlock hmb;
            hmb.label = hmain;
            {
                Instr ldi; ldi.op = Op::Load; ldi.result = next_id++; ldi.name = iv;
                hmb.instrs.push_back(ldi);
                Instr lt; lt.op = Op::Lt; lt.result = next_id++; lt.args = {ldi.result, limit};
                hmb.instrs.push_back(lt);
                Instr br; br.op = Op::Branch; br.result = kInvalidValue;
                br.name = bmain + ", " + hlabel; br.args = {lt.result};
                hmb.instrs.push_back(br);
            }

            // X: sum the partials back into acc before its original text.
            {
                std::vector<Instr> sums;
                ValueId prev = kInvalidValue;
                for (int k = 0; k < factor; ++k) {
                    Instr ld; ld.op = Op::Load; ld.result = next_id++; ld.name = partial[k];
                    record_float(ld.result);
                    sums.push_back(ld);
                    if (k == 0) prev = ld.result;
                    else {
                        Instr ad; ad.op = Op::Add; ad.result = next_id++;
                        record_float(ad.result);
                        ad.args = {prev, ld.result}; prev = ad.result;
                        sums.push_back(ad);
                    }
                }
                Instr al; al.op = Op::Load; al.result = next_id++; al.name = acc;
                record_float(al.result);
                sums.push_back(al);
                Instr fin; fin.op = Op::Add; fin.result = next_id++; fin.args = {al.result, prev};
                record_float(fin.result);
                sums.push_back(fin);
                Instr st; st.op = Op::Store; st.result = kInvalidValue;
                st.name = acc; st.args = {fin.result};
                sums.push_back(st);
                auto& xv = fn.blocks[x].instrs;
                xv.insert(xv.begin(), sums.begin(), sums.end());
            }

            fn.blocks.push_back(std::move(hmb));
            fn.blocks.push_back(std::move(bmb));

            skip_headers.insert(hlabel);
            ++stats.accum_unrolled;
            changed = true;
            break;
        }
        if (!changed) break;
    }
}

struct OptimizePasses {
    bool strength_reduce = true;   // rewrite invariant*IV multiplies into adds
    int accum_unroll = 1;          // split a reduction accumulator into N partials (<=1 off)
};

// `is_float` (optional) maps each original ValueId to whether it is a double;
// it is required for a float accumulator to be split. See accumulator_unroll.
inline OptimizeStats optimize_function(lithon::ir::Function& fn,
                                       const OptimizePasses& passes = OptimizePasses{},
                                       const std::vector<bool>* is_float = nullptr) {
    OptimizeStats stats;
    convert_self_tail_calls(fn, stats);
    fold_constants(fn, stats);
    if (passes.strength_reduce) strength_reduce_multiplies(fn, stats);
    accumulator_unroll(fn, stats, passes.accum_unroll, is_float);
    eliminate_dead_code(fn, stats);
    return stats;
}

} // namespace lithon::jit
