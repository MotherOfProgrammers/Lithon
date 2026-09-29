#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "jit_abi.h"
#include "liveness.h"
#include "optimize.h"
#include "print_guard.h"
#include "register_alloc.h"
#include "x86_encoder.h"

// The bridge from typed ir::Functions to runnable x86-64 machine code.
//
// Pipeline per function:
//   optimize_function      constant folding + dead-code elimination
//   select_promoted_...    hot variables -> callee-saved registers
//   plan_function          decide, per %N temp, whether it needs a
//                          register at all:
//                            Const      -> an immediate, no instruction
//                            Alias      -> "is" a promoted variable's
//                                          register, no copy
//                            FusedCmp   -> folded into the Branch that
//                                          consumes it (cmp + jcc)
//                            FusedStore -> computed directly in the
//                                          variable's register
//   RegisterAllocator      linear scan for what is left
//   emit                   machine code, with loop rotation and
//                          (for simple single-block bodies) unrolling
//
// For `while i < N: total = total + i; i = i + 1` this turns the old
//   mov rcx,[rbp-8]; mov rax,N; cmp; setl; movzx; test; jnz; ...
// (three memory operations per variable update) into
//   add r13, r12 ; add r12, 1 ; cmp r12, N ; jl body
//
// SCRATCH ROLES: r10 = left operand / spilled result, r11 = right
// operand / indirect-call target. Two independent scratch registers make
// the "both operands spilled" clobber bug structurally impossible.
//
// print() FORMATTING: which format a print() call uses is decided by the
// same whole-module Kind analysis print_guard.h uses to gate the
// tier_runner (infer_value_kinds, computed once up front from the
// *original* module -- folding/DCE never change a surviving value's id,
// so looking values up by id against the pre-optimization module stays
// correct after optimize_function rewrites a private copy). A provably-
// Bool argument prints True/False; a provably-Int argument prints as a
// decimal. Anything else (float, or a join of incomparable kinds) throws,
// so a caller either doesn't reach this compiler at all (tier_runner's
// guard already refused it) or gets a clear, specific error (lithon_jit,
// which has no guard) instead of silently mis-printing a float as a
// truncated integer.
//
// KNOWN, DOCUMENTED LIMITATION: floats have no arithmetic or storage
// support in the JIT at all yet (no SSE codegen) -- this pass only added
// correct *printing* of the bool case, not general float support.

namespace lithon::jit {

namespace {
static const char kIntPrintFormat[] = "%lld\n";
// No format specifiers, so these are passed directly as printf's sole
// argument (its "format string") -- safe since printf treats a string
// with no '%' as a literal, and it saves marshalling a second argument.
static const char kBoolTrueLiteral[] = "True\n";
static const char kBoolFalseLiteral[] = "False\n";
}

struct CompileOptions {
    bool optimize = true;          // constant folding + dead-code elimination
    bool promote_registers = true; // keep hot variables in registers
    bool rotate_loops = true;      // duplicate small loop headers at the back edge
    int unroll_factor = 4;         // copies of a simple loop body per back edge (1 = off)
};

struct CompiledModule {
    std::vector<uint8_t> code;
    std::unordered_map<std::string, size_t> function_offset;
};

namespace detail {

struct TempInfo {
    enum class Kind : uint8_t { Normal, Const, Alias, FusedCmp, FusedStore };
    Kind kind = Kind::Normal;
    int64_t imm = 0;
    Reg alias = Reg::RAX;
};

struct FunctionPlan {
    std::unordered_map<lithon::ir::ValueId, TempInfo> info;
    std::unordered_set<lithon::ir::ValueId> virtual_temps;
    std::vector<std::vector<uint8_t>> skip_instr;   // [block][pos]: instruction fused away
};

inline FunctionPlan plan_function(const lithon::ir::Function& fn, const PromotionMap& promoted) {
    using namespace lithon::ir;
    FunctionPlan plan;
    plan.skip_instr.resize(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) plan.skip_instr[b].assign(fn.blocks[b].instrs.size(), 0);

    struct Site { size_t b, p; };
    std::unordered_map<ValueId, std::vector<Site>> uses;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        for (size_t p = 0; p < fn.blocks[b].instrs.size(); ++p)
            for (auto arg : fn.blocks[b].instrs[p].args) uses[arg].push_back({b, p});

    auto set = [&](ValueId id, TempInfo ti) {
        plan.info[id] = ti;
        plan.virtual_temps.insert(id);
    };

    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& ins = fn.blocks[b].instrs;
        for (size_t p = 0; p < ins.size(); ++p) {
            const Instr& in = ins[p];

            if (in.op == Op::ConstInt || in.op == Op::ConstBool) {
                // Both store their value in int_imm (ir.h / text_parser.cpp).
                TempInfo ti; ti.kind = TempInfo::Kind::Const; ti.imm = in.int_imm;
                set(in.result, ti);
                continue;
            }

            if (in.op == Op::Load && promoted.count(in.name)) {
                // A load of a promoted variable needs no copy when the
                // variable cannot change while the loaded value is live:
                // every use is later in the same block, with no Store to
                // that variable in between.
                const auto& us = uses[in.result];
                bool ok = true;
                size_t last = p;
                for (const auto& u : us) {
                    if (u.b != b || u.p <= p) { ok = false; break; }
                    last = std::max(last, u.p);
                }
                for (size_t q = p + 1; ok && q < last; ++q) {
                    if (ins[q].op == Op::Store && ins[q].name == in.name) ok = false;
                }
                if (ok) {
                    TempInfo ti; ti.kind = TempInfo::Kind::Alias; ti.alias = promoted.at(in.name);
                    set(in.result, ti);
                }
                continue;
            }

            const bool has_next = p + 1 < ins.size();

            if ((in.op == Op::Lt || in.op == Op::Gt || in.op == Op::Eq) && has_next &&
                ins[p + 1].op == Op::Branch && ins[p + 1].args.size() == 1 &&
                ins[p + 1].args[0] == in.result && uses[in.result].size() == 1) {
                TempInfo ti; ti.kind = TempInfo::Kind::FusedCmp;
                set(in.result, ti);
                continue;
            }

            if ((in.op == Op::Add || in.op == Op::Sub || in.op == Op::Mul) && has_next &&
                ins[p + 1].op == Op::Store && ins[p + 1].args.size() == 1 &&
                ins[p + 1].args[0] == in.result && uses[in.result].size() == 1 &&
                promoted.count(ins[p + 1].name)) {
                TempInfo ti; ti.kind = TempInfo::Kind::FusedStore;
                set(in.result, ti);
                plan.skip_instr[b][p + 1] = 1;
                continue;
            }
        }
    }
    return plan;
}

// A loop header is worth duplicating at its back edge when it is tiny,
// side-effect free, and ends in a conditional branch.
inline bool is_rotatable_header(const lithon::ir::BasicBlock& block) {
    using lithon::ir::Op;
    if (block.instrs.empty() || block.instrs.size() > 10) return false;
    if (block.instrs.back().op != Op::Branch) return false;
    for (const auto& in : block.instrs) {
        switch (in.op) {
            case Op::ConstInt: case Op::ConstBool: case Op::Load:
            case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Lt: case Op::Gt: case Op::Eq: case Op::Branch:
                break;
            default:
                return false;
        }
    }
    return true;
}

// A loop body qualifies for unrolling when it is straight-line, pure
// arithmetic/moves that ends in the back-edge Jump: no calls, returns
// or inner control flow to duplicate.
inline bool is_unrollable_body(const lithon::ir::BasicBlock& block) {
    using lithon::ir::Op;
    if (block.instrs.size() < 2 || block.instrs.size() > 16) return false;
    if (block.instrs.back().op != Op::Jump) return false;
    for (size_t i = 0; i + 1 < block.instrs.size(); ++i) {
        switch (block.instrs[i].op) {
            case Op::ConstInt: case Op::ConstBool: case Op::Load: case Op::Store:
            case Op::Add: case Op::Sub: case Op::Mul:
            case Op::Lt: case Op::Gt: case Op::Eq:
            case Op::And: case Op::Or: case Op::Not:
                break;
            default:
                return false;
        }
    }
    return true;
}

// Flags for emit_block_body.
enum : unsigned {
    kAllowRotate = 1,          // a back-edge Jump may be replaced by an inlined header
    kStopBeforeTerminator = 2, // emit everything except the block's final Jump
    kExitOnlyBranch = 4        // Branch: jump out when false, fall through when true
};

} // namespace detail

inline CompiledModule compile_module(const lithon::ir::Module& module,
                                     const CompileOptions& options = CompileOptions{}) {
    using namespace lithon::ir;
    using detail::TempInfo;

    CodeBuffer code;
    std::unordered_map<std::string, size_t> function_offset;

    struct PendingCallPatch {
        JumpPatch patch;
        std::string target_function;
    };
    std::vector<PendingCallPatch> pending_calls;

    // Computed once, on the ORIGINAL (pre-optimization) module: see the
    // "print() FORMATTING" note above the class comment block for why
    // this stays valid after each function's private copy is folded/DCE'd.
    const std::vector<std::vector<Kind>> module_kinds = infer_value_kinds(module);

    for (size_t fn_index = 0; fn_index < module.functions.size(); ++fn_index) {
        const Function& original_fn = module.functions[fn_index];
        const std::vector<Kind>& value_kinds = module_kinds[fn_index];
        if (function_offset.count(original_fn.name)) {
            throw std::runtime_error(
                "compile_module: duplicate function name '" + original_fn.name + "'");
        }
        function_offset[original_fn.name] = code.size();

        if (original_fn.params.size() > 2) {
            throw std::runtime_error(
                "compile_module: function '" + original_fn.name + "' has more than 2 "
                "parameters -- only two register arguments are supported in this slice");
        }

        Function fn = original_fn;
        if (options.optimize) optimize_function(fn);

        PromotionMap promoted = options.promote_registers ? select_promoted_variables(fn)
                                                          : PromotionMap{};
        detail::FunctionPlan plan = detail::plan_function(fn, promoted);
        RegisterAllocator alloc(fn, promoted, plan.virtual_temps);

        constexpr Reg kL = abi::kScratchLeft;
        constexpr Reg kR = abi::kScratchRight;

        auto info_of = [&](ValueId id) -> const TempInfo& {
            static const TempInfo normal{};
            auto it = plan.info.find(id);
            return it != plan.info.end() ? it->second : normal;
        };

        // dst := value of a temp, wherever it lives.
        auto materialize_into = [&](Reg dst, ValueId id) {
            const TempInfo& ti = info_of(id);
            switch (ti.kind) {
                case TempInfo::Kind::Const:
                    emit_mov_reg_imm(code, dst, ti.imm);
                    return;
                case TempInfo::Kind::Alias:
                    if (ti.alias != dst) emit_mov_reg_reg(code, dst, ti.alias);
                    return;
                case TempInfo::Kind::Normal: {
                    const ValueLocation& loc = alloc.temp_location(id);
                    if (loc.in_register) {
                        if (loc.reg != dst) emit_mov_reg_reg(code, dst, loc.reg);
                    } else {
                        emit_load_rbp_offset(code, dst, loc.stack_slot);
                    }
                    return;
                }
                default:
                    throw std::logic_error("compile_module: read of a fused temporary");
            }
        };

        // A register holding the value; uses `scratch` only if it must load or build it.
        auto read_in = [&](ValueId id, Reg scratch) -> Reg {
            const TempInfo& ti = info_of(id);
            if (ti.kind == TempInfo::Kind::Alias) return ti.alias;
            if (ti.kind == TempInfo::Kind::Normal) {
                const ValueLocation& loc = alloc.temp_location(id);
                if (loc.in_register) return loc.reg;
            }
            materialize_into(scratch, id);
            return scratch;
        };
        auto read_left = [&](ValueId id) { return read_in(id, kL); };
        auto read_right = [&](ValueId id) { return read_in(id, kR); };

        auto imm32_of = [&](ValueId id, int32_t& out) -> bool {
            const TempInfo& ti = info_of(id);
            if (ti.kind != TempInfo::Kind::Const || !fits_imm32(ti.imm)) return false;
            out = static_cast<int32_t>(ti.imm);
            return true;
        };

        auto compute_dest = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            return loc.in_register ? loc.reg : kL;
        };
        auto commit_result = [&](ValueId id, Reg computed_in) {
            const ValueLocation& loc = alloc.temp_location(id);
            if (!loc.in_register) emit_store_rbp_offset(code, computed_in, loc.stack_slot);
        };

        auto require_variable = [&](const std::string& name, const char* what) {
            if (!alloc.has_variable(name)) {
                throw std::runtime_error(std::string("compile_module: ") + what +
                                         " undeclared variable '" + name + "'");
            }
        };

        // ---- prologue ---------------------------------------------------
        emit_prologue(code, alloc.frame_size());
        for (const auto& saved : alloc.callee_saved_slots()) {
            emit_store_rbp_offset(code, saved.first, saved.second);
        }
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (alloc.variable_in_register(fn.params[i])) {
                emit_mov_reg_reg(code, alloc.variable_reg(fn.params[i]), abi::kArgRegs[i]);
            } else {
                emit_store_rbp_offset(code, abi::kArgRegs[i], alloc.variable_offset(fn.params[i]));
            }
        }

        struct PendingBlockPatch {
            JumpPatch patch;
            std::string target_label;
        };
        std::vector<PendingBlockPatch> pending_blocks;
        std::unordered_map<std::string, size_t> block_offset;
        std::unordered_map<std::string, size_t> block_index;
        for (size_t b = 0; b < fn.blocks.size(); ++b) block_index[fn.blocks[b].label] = b;

        auto emit_branch_to = [&](Cond cond, const std::string& then_label,
                                  const std::string& else_label, const std::string& next_label) {
            if (then_label == next_label) {
                pending_blocks.push_back({emit_jcc_rel32(code, invert(cond)), else_label});
            } else if (else_label == next_label) {
                pending_blocks.push_back({emit_jcc_rel32(code, cond), then_label});
            } else {
                pending_blocks.push_back({emit_jcc_rel32(code, cond), then_label});
                pending_blocks.push_back({emit_jmp_rel32(code), else_label});
            }
        };

        // Emits block `bi`'s instructions. `next_label` is the label of the
        // block that will physically follow the emitted code (used to elide
        // jumps to the fall-through block).
        std::function<void(size_t, const std::string&, unsigned)> emit_block_body =
            [&](size_t bi, const std::string& next_label, unsigned flags) {
            const BasicBlock& block = fn.blocks[bi];
            const bool allow_rotate = (flags & detail::kAllowRotate) != 0;
            for (size_t pos = 0; pos < block.instrs.size(); ++pos) {
                if ((flags & detail::kStopBeforeTerminator) && pos + 1 == block.instrs.size()) break;
                if (plan.skip_instr[bi][pos]) continue;
                const Instr& instr = block.instrs[pos];

                switch (instr.op) {
                    case Op::ConstInt:
                    case Op::ConstBool:
                        break;   // always an immediate at its uses

                    case Op::Load: {
                        if (info_of(instr.result).kind == TempInfo::Kind::Alias) break;
                        require_variable(instr.name, "load of");
                        Reg dst = compute_dest(instr.result);
                        if (alloc.variable_in_register(instr.name)) {
                            emit_mov_reg_reg(code, dst, alloc.variable_reg(instr.name));
                        } else {
                            emit_load_rbp_offset(code, dst, alloc.variable_offset(instr.name));
                        }
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Store: {
                        require_variable(instr.name, "store to");
                        if (alloc.variable_in_register(instr.name)) {
                            materialize_into(alloc.variable_reg(instr.name), instr.args.at(0));
                        } else {
                            Reg src = read_left(instr.args.at(0));
                            emit_store_rbp_offset(code, src, alloc.variable_offset(instr.name));
                        }
                        break;
                    }

                    case Op::Add:
                    case Op::Sub:
                    case Op::Mul: {
                        const bool fused = info_of(instr.result).kind == TempInfo::Kind::FusedStore;
                        Reg dst = fused ? alloc.variable_reg(block.instrs[pos + 1].name)
                                        : compute_dest(instr.result);
                        Reg lhs = read_left(instr.args.at(0));
                        int32_t imm = 0;
                        if (imm32_of(instr.args.at(1), imm)) {
                            if (instr.op == Op::Mul) {
                                emit_imul_reg_reg_imm32(code, dst, lhs, imm);
                            } else {
                                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                                if (instr.op == Op::Add) emit_add_reg_imm32(code, dst, imm);
                                else emit_sub_reg_imm32(code, dst, imm);
                            }
                        } else {
                            Reg rhs = read_right(instr.args.at(1));
                            if (rhs == dst && lhs != dst) {
                                // dst is about to be overwritten but is also the right operand.
                                if (instr.op == Op::Sub) {
                                    emit_mov_reg_reg(code, kR, lhs);
                                    emit_sub_reg_reg(code, kR, rhs);
                                    emit_mov_reg_reg(code, dst, kR);
                                } else if (instr.op == Op::Add) {
                                    emit_add_reg_reg(code, dst, lhs);
                                } else {
                                    emit_imul_reg_reg(code, dst, lhs);
                                }
                            } else {
                                if (lhs != dst) emit_mov_reg_reg(code, dst, lhs);
                                if (instr.op == Op::Add) emit_add_reg_reg(code, dst, rhs);
                                else if (instr.op == Op::Sub) emit_sub_reg_reg(code, dst, rhs);
                                else emit_imul_reg_reg(code, dst, rhs);
                            }
                        }
                        if (!fused) commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Lt:
                    case Op::Gt:
                    case Op::Eq: {
                        if (info_of(instr.result).kind == TempInfo::Kind::FusedCmp) break;
                        Reg lhs = read_left(instr.args.at(0));
                        Reg dst = compute_dest(instr.result);
                        int32_t imm = 0;
                        if (imm32_of(instr.args.at(1), imm)) {
                            emit_cmp_reg_imm32(code, lhs, imm);
                        } else {
                            emit_cmp_reg_reg(code, lhs, read_right(instr.args.at(1)));
                        }
                        Cond cond = instr.op == Op::Lt ? Cond::Less
                                  : instr.op == Op::Gt ? Cond::Greater : Cond::Equal;
                        emit_setcc(code, cond, dst);
                        emit_movzx_reg_reg8(code, dst, dst);
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::And: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, lhs);
                        JumpPatch to_use_rhs = emit_jcc_rel32(code, Cond::NotZero);
                        emit_mov_reg_reg(code, dst, lhs);
                        JumpPatch to_end = emit_jmp_rel32(code);
                        resolve_jump_patch(code, to_use_rhs, code.size());
                        emit_mov_reg_reg(code, dst, rhs);
                        resolve_jump_patch(code, to_end, code.size());
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Or: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, lhs);
                        JumpPatch to_use_lhs = emit_jcc_rel32(code, Cond::NotZero);
                        emit_mov_reg_reg(code, dst, rhs);
                        JumpPatch to_end = emit_jmp_rel32(code);
                        resolve_jump_patch(code, to_use_lhs, code.size());
                        emit_mov_reg_reg(code, dst, lhs);
                        resolve_jump_patch(code, to_end, code.size());
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Not: {
                        Reg operand = read_left(instr.args.at(0));
                        Reg dst = compute_dest(instr.result);
                        emit_test_reg_reg(code, operand);
                        emit_setcc(code, Cond::Equal, dst);
                        emit_movzx_reg_reg8(code, dst, dst);
                        commit_result(instr.result, dst);
                        break;
                    }

                    case Op::Branch: {
                        auto targets = branch_targets(instr);
                        if (targets.size() != 2) throw std::runtime_error("compile_module: malformed branch");
                        ValueId cond_id = instr.args.at(0);
                        Cond cond = Cond::NotZero;
                        if (info_of(cond_id).kind == TempInfo::Kind::FusedCmp) {
                            // The compare producing this condition is the previous instruction.
                            const Instr& cmp = block.instrs[pos - 1];
                            Reg lhs = read_left(cmp.args.at(0));
                            int32_t imm = 0;
                            if (imm32_of(cmp.args.at(1), imm)) emit_cmp_reg_imm32(code, lhs, imm);
                            else emit_cmp_reg_reg(code, lhs, read_right(cmp.args.at(1)));
                            cond = cmp.op == Op::Lt ? Cond::Less
                                 : cmp.op == Op::Gt ? Cond::Greater : Cond::Equal;
                        } else {
                            emit_test_reg_reg(code, read_left(cond_id));
                        }
                        if (flags & detail::kExitOnlyBranch) {
                            pending_blocks.push_back({emit_jcc_rel32(code, invert(cond)), targets[1]});
                        } else {
                            emit_branch_to(cond, targets[0], targets[1], next_label);
                        }
                        break;
                    }

                    case Op::Jump: {
                        if (instr.name == next_label) break;   // fall through
                        auto it = block_index.find(instr.name);
                        if (allow_rotate && options.rotate_loops && it != block_index.end() &&
                            it->second <= bi && detail::is_rotatable_header(fn.blocks[it->second])) {
                            // Loop rotation: re-test the loop condition here instead of
                            // jumping back to the header, so each iteration executes one
                            // conditional jump instead of a conditional plus an unconditional.
                            emit_block_body(it->second, next_label, 0);
                        } else {
                            pending_blocks.push_back({emit_jmp_rel32(code), instr.name});
                        }
                        break;
                    }

                    case Op::Call: {
                        if (instr.name == "print") {
                            if (instr.args.size() != 1) {
                                throw std::runtime_error(
                                    "compile_module: print() with " +
                                    std::to_string(instr.args.size()) +
                                    " arguments (native supports exactly 1)");
                            }
                            ValueId arg = instr.args.at(0);
                            Kind k = arg < value_kinds.size() ? value_kinds[arg] : Kind::Unknown;

                            if (k == Kind::Bool) {
                                // Two literal strings, no format specifiers: select
                                // which one is printf's sole argument by branching,
                                // rather than formatting a "%s" indirection.
                                Reg v = read_left(arg);
                                emit_test_reg_reg(code, v);
                                JumpPatch to_false = emit_jcc_rel32(code, Cond::Equal);
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kBoolTrueLiteral));
                                JumpPatch to_call = emit_jmp_rel32(code);
                                resolve_jump_patch(code, to_false, code.size());
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kBoolFalseLiteral));
                                resolve_jump_patch(code, to_call, code.size());
                            } else if (k == Kind::Int) {
                                materialize_into(abi::kArgRegs[1], arg);
                                emit_mov_reg_imm(code, abi::kArgRegs[0],
                                    reinterpret_cast<int64_t>(kIntPrintFormat));
                            } else {
                                // Float or an unresolved/mixed kind: the JIT has no
                                // float codegen and no other format to fall back to.
                                // A caller with the print_guard in front of it (the
                                // tier_runner) never reaches this; a caller without
                                // one (lithon_jit) gets a clear refusal instead of a
                                // silently-wrong integer reinterpretation.
                                throw std::runtime_error(
                                    "compile_module: print() argument is not provably "
                                    "int or bool (kind: " + std::string(kind_name(k)) +
                                    "); native float printing is not implemented");
                            }
                            emit_mov_reg_imm(code, kR, reinterpret_cast<int64_t>(&std::printf));
                            emit_xor_zero(code, Reg::RAX);   // al = 0 vector regs (SysV variadic)
                            if (abi::kShadowSpace) emit_sub_rsp_imm32(code, abi::kShadowSpace);
                            emit_call_reg(code, kR);
                            if (abi::kShadowSpace) emit_add_rsp_imm32(code, abi::kShadowSpace);
                            break;
                        }
                        if (instr.args.size() > 2) {
                            throw std::runtime_error(
                                "compile_module: calls with more than 2 arguments "
                                "not supported in this slice");
                        }
                        for (size_t i = 0; i < instr.args.size(); ++i) {
                            materialize_into(abi::kArgRegs[i], instr.args[i]);
                        }
                        JumpPatch to_callee = emit_jmp_rel32(code);
                        code[to_callee.rel32_offset - 1] = 0xE8;   // rewrite jmp rel32 -> call rel32
                        pending_calls.push_back({to_callee, instr.name});

                        if (instr.result != kInvalidValue) {
                            Reg dst = compute_dest(instr.result);
                            if (dst != Reg::RAX) emit_mov_reg_reg(code, dst, Reg::RAX);
                            commit_result(instr.result, dst);
                        }
                        break;
                    }

                    case Op::Return: {
                        if (!instr.args.empty()) materialize_into(Reg::RAX, instr.args.at(0));
                        for (const auto& saved : alloc.callee_saved_slots()) {
                            emit_load_rbp_offset(code, saved.first, saved.second);
                        }
                        emit_epilogue(code);
                        emit_ret(code);
                        break;
                    }

                    default:
                        throw std::runtime_error(
                            "compile_module: opcode not implemented in this slice "
                            "(floats are the main remaining gap)");
                }
            }
        };

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            block_offset[fn.blocks[bi].label] = code.size();
            const std::string next_label = bi + 1 < fn.blocks.size() ? fn.blocks[bi + 1].label : "";

            // Unrolling: header H immediately precedes body B, H's branch
            // enters B or leaves the loop, and B is simple straight-line
            // code ending in `jump H`. Emit U copies of B, each followed
            // by an inlined copy of H's test that EXITS when false and
            // falls into the next copy when true; only the last copy's
            // test jumps back. Every iteration is still individually
            // tested, so any trip count (not just multiples of U) is exact.
            if (options.rotate_loops && options.unroll_factor > 1 && bi >= 1 &&
                detail::is_unrollable_body(fn.blocks[bi])) {
                const Instr& term = fn.blocks[bi].instrs.back();
                auto hit = block_index.find(term.name);
                if (hit != block_index.end() && hit->second == bi - 1 &&
                    detail::is_rotatable_header(fn.blocks[hit->second])) {
                    auto targets = branch_targets(fn.blocks[hit->second].instrs.back());
                    if (targets.size() == 2 && targets[0] == fn.blocks[bi].label &&
                        targets[1] != fn.blocks[bi].label) {
                        for (int k = 0; k < options.unroll_factor; ++k) {
                            emit_block_body(bi, "", detail::kStopBeforeTerminator);
                            const bool last = (k + 1 == options.unroll_factor);
                            emit_block_body(hit->second, next_label,
                                            last ? 0u : detail::kExitOnlyBranch);
                        }
                        continue;
                    }
                }
            }

            emit_block_body(bi, next_label, detail::kAllowRotate);
        }

        for (const auto& p : pending_blocks) {
            auto it = block_offset.find(p.target_label);
            if (it == block_offset.end()) {
                throw std::runtime_error(
                    "compile_module: jump to unknown block '" + p.target_label + "'");
            }
            resolve_jump_patch(code, p.patch, it->second);
        }
    }

    for (const auto& p : pending_calls) {
        auto it = function_offset.find(p.target_function);
        if (it == function_offset.end()) {
            throw std::runtime_error(
                "compile_module: call to unknown function '" + p.target_function + "'");
        }
        resolve_jump_patch(code, p.patch, it->second);
    }

    return CompiledModule{std::move(code), std::move(function_offset)};
}

} // namespace lithon::jit
