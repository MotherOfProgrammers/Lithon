#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "jit_abi.h"
#include "liveness.h"

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
    int tail_calls = 0;
    int strength_reduced = 0;
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
            return true;
        default:
            return false;   // Call/Return/Branch/Jump have effects; Div/Float/Phi stay untouched
    }
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
            if (in.op != Op::Store && !is_pure_op(in.op)) {
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
            return is_pure_op(in.op) && in.result != kInvalidValue && live_temps.count(in.result) == 0;
        }), v.end());
        stats.dead_removed += static_cast<int>(before - v.size());
    }
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
        for (const LoopSpan& loop : find_loops(fn)) {
            if (loop.last_block != loop.first_block + 1) continue;   // not a two-block loop
            if (loop.first_block == 0) continue;                    // entry block has no predecessor
            const size_t h = loop.first_block, body = loop.last_block;
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

struct OptimizePasses {
    bool strength_reduce = true;   // rewrite invariant*IV multiplies into adds
};

inline OptimizeStats optimize_function(lithon::ir::Function& fn,
                                       const OptimizePasses& passes = OptimizePasses{}) {
    OptimizeStats stats;
    convert_self_tail_calls(fn, stats);
    fold_constants(fn, stats);
    if (passes.strength_reduce) strength_reduce_multiplies(fn, stats);
    eliminate_dead_code(fn, stats);
    return stats;
}

} // namespace lithon::jit
