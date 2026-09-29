#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
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

inline OptimizeStats optimize_function(lithon::ir::Function& fn) {
    OptimizeStats stats;
    fold_constants(fn, stats);
    eliminate_dead_code(fn, stats);
    return stats;
}

} // namespace lithon::jit
