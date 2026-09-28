#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <string>
#include "ir/ir.h"
#include "x86_encoder.h"
#include "register_alloc.h"

// The real bridge: walks typed ir::Functions and drives the encoder
// + register allocator to produce genuine, runnable machine code.
//
// CRITICAL CORRECTNESS FIX in this revision: earlier codegen used a
// single shared scratch register (RBX) to reload BOTH operands of a
// binary op when spilled. When both operands were spilled
// simultaneously, the second load silently clobbered the first
// before it was used -- found via a deliberately adversarial And/Or
// test where the two ops share operands and produce DIFFERENT
// correct answers, exposing the corruption.
//
// The fix: TWO dedicated, permanently-reserved scratch registers:
//   - RDX: left operand of a binop / the single operand of a
//     one-operand op / the compute-and-store register for a spilled
//     RESULT.
//   - RBX: right operand of a binop.
// The allocatable pool for real %N values shrinks to RAX/RCX
// (register_alloc.h).
//
// This slice also adds: And/Or/Not (matching the interpreter's exact
// short-circuit VALUE semantics), and print() for integers via a
// real call into the host process's own libc printf.
//
// KNOWN, DOCUMENTED LIMITATION: print() always formats as a 64-bit
// integer. It does not know if a value is actually bool/float (that
// type info isn't threaded into this untyped-IR-level codegen pass
// yet). Printing a bool natively shows 1/0 instead of True/False --
// explicit, documented, not a silent bug.

namespace lithon::jit {

namespace {
static const char kIntPrintFormat[] = "%lld\n";
}

struct CompiledModule {
    std::vector<uint8_t> code;
    std::unordered_map<std::string, size_t> function_offset;
};

inline CompiledModule compile_module(const lithon::ir::Module& module) {
    using namespace lithon::ir;

    CodeBuffer code;
    std::unordered_map<std::string, size_t> function_offset;

    struct PendingCallPatch {
        JumpPatch patch;
        std::string target_function;
    };
    std::vector<PendingCallPatch> pending_calls;

    for (const auto& fn : module.functions) {
        if (function_offset.count(fn.name)) {
            throw std::runtime_error(
                "compile_module: duplicate function name '" + fn.name + "'");
        }
        function_offset[fn.name] = code.size();

        if (fn.params.size() > 2) {
            throw std::runtime_error(
                "compile_module: function '" + fn.name + "' has more than 2 "
                "parameters -- only System V rdi/rsi are supported in this slice");
        }

        RegisterAllocator alloc(fn);

        auto read_left = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            if (loc.in_register) return loc.reg;
            emit_load_rbp_offset(code, Reg::RDX, loc.stack_slot);
            return Reg::RDX;
        };

        auto read_right = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            if (loc.in_register) return loc.reg;
            emit_load_rbp_offset(code, Reg::RBX, loc.stack_slot);
            return Reg::RBX;
        };

        auto compute_dest = [&](ValueId id) -> Reg {
            const ValueLocation& loc = alloc.temp_location(id);
            return loc.in_register ? loc.reg : Reg::RDX;
        };

        auto commit_result = [&](ValueId id, Reg computed_in) {
            const ValueLocation& loc = alloc.temp_location(id);
            if (!loc.in_register) {
                emit_store_rbp_offset(code, computed_in, loc.stack_slot);
            }
        };

        emit_prologue(code, alloc.frame_size());

        static const Reg arg_regs[2] = {Reg::RDI, Reg::RSI};
        for (size_t i = 0; i < fn.params.size(); ++i) {
            int offset = alloc.variable_offset(fn.params[i]);
            emit_store_rbp_offset(code, arg_regs[i], offset);
        }

        struct PendingBlockPatch {
            JumpPatch patch;
            std::string target_label;
        };
        std::vector<PendingBlockPatch> pending_blocks;
        std::unordered_map<std::string, size_t> block_offset;

        for (const auto& block : fn.blocks) {
            block_offset[block.label] = code.size();

            for (const auto& instr : block.instrs) {
                switch (instr.op) {
                    case Op::ConstInt: {
                        Reg dst = compute_dest(instr.result);
                        emit_mov_reg_imm64(code, dst, instr.int_imm);
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Load: {
                        if (!alloc.has_variable(instr.name)) {
                            throw std::runtime_error(
                                "compile_module: load of undeclared variable '" + instr.name + "'");
                        }
                        Reg dst = compute_dest(instr.result);
                        emit_load_rbp_offset(code, dst, alloc.variable_offset(instr.name));
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Store: {
                        Reg src = read_left(instr.args.at(0));
                        if (!alloc.has_variable(instr.name)) {
                            throw std::runtime_error(
                                "compile_module: store to undeclared variable '" + instr.name + "'");
                        }
                        emit_store_rbp_offset(code, src, alloc.variable_offset(instr.name));
                        break;
                    }
                    case Op::Add:
                    case Op::Sub:
                    case Op::Mul: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        emit_mov_reg_reg(code, dst, lhs);
                        if (instr.op == Op::Add) emit_add_reg_reg(code, dst, rhs);
                        else if (instr.op == Op::Sub) emit_sub_reg_reg(code, dst, rhs);
                        else emit_imul_reg_reg(code, dst, rhs);
                        commit_result(instr.result, dst);
                        break;
                    }
                    case Op::Lt:
                    case Op::Gt:
                    case Op::Eq: {
                        Reg lhs = read_left(instr.args.at(0));
                        Reg rhs = read_right(instr.args.at(1));
                        Reg dst = compute_dest(instr.result);
                        Cond cond = instr.op == Op::Lt ? Cond::Less
                                  : instr.op == Op::Gt ? Cond::Greater
                                  : Cond::Equal;
                        emit_cmp_reg_reg(code, lhs, rhs);
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
                        Reg cond_reg = read_left(instr.args.at(0));
                        size_t comma = instr.name.find(',');
                        std::string then_label = instr.name.substr(0, comma);
                        std::string else_label = instr.name.substr(comma + 1);

                        emit_test_reg_reg(code, cond_reg);
                        JumpPatch to_then = emit_jcc_rel32(code, Cond::NotZero);
                        pending_blocks.push_back({to_then, then_label});

                        JumpPatch to_else = emit_jmp_rel32(code);
                        pending_blocks.push_back({to_else, else_label});
                        break;
                    }
                    case Op::Jump: {
                        JumpPatch to_target = emit_jmp_rel32(code);
                        pending_blocks.push_back({to_target, instr.name});
                        break;
                    }
                    case Op::Call: {
                        if (instr.name == "print") {
                            Reg val = read_left(instr.args.at(0));
                            emit_mov_reg_imm64(code, Reg::RDI,
                                reinterpret_cast<int64_t>(kIntPrintFormat));
                            emit_mov_reg_reg(code, Reg::RSI, val);
                            emit_mov_reg_imm64(code, Reg::RCX,
                                reinterpret_cast<int64_t>(&std::printf));
                            emit_xor_zero(code, Reg::RAX);
                            emit_call_reg(code, Reg::RCX);
                            break;
                        }
                        if (instr.args.size() > 2) {
                            throw std::runtime_error(
                                "compile_module: calls with more than 2 arguments "
                                "not supported in this slice");
                        }
                        Reg arg_vals[2];
                        for (size_t i = 0; i < instr.args.size(); ++i) {
                            arg_vals[i] = (i == 0) ? read_left(instr.args[i]) : read_right(instr.args[i]);
                        }
                        for (size_t i = 0; i < instr.args.size(); ++i) {
                            emit_mov_reg_reg(code, arg_regs[i], arg_vals[i]);
                        }
                        JumpPatch to_callee = emit_jmp_rel32(code);
                        code[to_callee.rel32_offset - 1] = 0xE8;
                        pending_calls.push_back({to_callee, instr.name});

                        if (instr.result != kInvalidValue) {
                            Reg dst = compute_dest(instr.result);
                            if (dst != Reg::RAX) {
                                emit_mov_reg_reg(code, dst, Reg::RAX);
                            }
                            commit_result(instr.result, dst);
                        }
                        break;
                    }
                    case Op::Return: {
                        if (!instr.args.empty()) {
                            Reg src = read_left(instr.args.at(0));
                            if (src != Reg::RAX) {
                                emit_mov_reg_reg(code, Reg::RAX, src);
                            }
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
